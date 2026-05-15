#include "tdl_app/video_buffer.hpp"

#include <cstdio>
#include <cstring>

#include "cvi_buffer.h"
#include "cvi_comm_vb.h"
#include "cvi_vb.h"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

std::uint32_t resolveBlockSize(const VideoBufferPoolConfig &config) {
  return COMMON_GetPicBufferSize(
      static_cast<CVI_U32>(config.size.width),
      static_cast<CVI_U32>(config.size.height),
      static_cast<PIXEL_FORMAT_E>(config.pixel_format), DATA_BITWIDTH_8,
      COMPRESS_MODE_NONE, static_cast<CVI_U32>(config.align));
}

std::uint32_t resolveBlockSize(const VideoBufferPool::Config &config) {
  if (config.block_size > 0) {
    return config.block_size;
  }
  VideoBufferPoolConfig out;
  out.size = config.size;
  out.pixel_format = config.pixel_format;
  out.block_count = config.block_count;
  out.align = config.align;
  out.cached = config.cached;
  out.remap_mode = config.remap_mode;
  return resolveBlockSize(out);
}

VB_REMAP_MODE_E toRemapMode(bool cached) {
  return cached ? VB_REMAP_MODE_CACHED : VB_REMAP_MODE_NOCACHE;
}

VB_REMAP_MODE_E toRemapMode(int remap_mode, bool cached) {
  if (remap_mode >= 0 && remap_mode < static_cast<int>(VB_REMAP_MODE_BUTT)) {
    return static_cast<VB_REMAP_MODE_E>(remap_mode);
  }
  return toRemapMode(cached);
}

}  // namespace

VideoBufferManager::VideoBufferManager() = default;

VideoBufferManager::VideoBufferManager(const Config &config) : config_(config) {}

VideoBufferManager::~VideoBufferManager() { close(); }

bool VideoBufferManager::open(std::string *error) {
  if (opened_) {
    return true;
  }
  if (config_.common_pools.empty()) {
    opened_ = true;
    return true;
  }
  if (config_.common_pools.size() > VB_MAX_COMM_POOLS) {
    setError(error, "too many common VB pools requested");
    return false;
  }

  VB_CONFIG_S vb_config;
  std::memset(&vb_config, 0, sizeof(vb_config));
  vb_config.u32MaxPoolCnt =
      static_cast<CVI_U32>(config_.common_pools.size());
  for (std::size_t i = 0; i < config_.common_pools.size(); ++i) {
    const VideoBufferPoolConfig &pool = config_.common_pools[i];
    vb_config.astCommPool[i].u32BlkSize = resolveBlockSize(pool);
    vb_config.astCommPool[i].u32BlkCnt =
        static_cast<CVI_U32>(pool.block_count);
    vb_config.astCommPool[i].enRemapMode =
        toRemapMode(pool.remap_mode, pool.cached);
  }

  int ret = CVI_VB_SetConfig(&vb_config);
  if (ret != CVI_SUCCESS) {
    if (!config_.reuse_existing) {
      setError(error, "CVI_VB_SetConfig failed, ret=" + std::to_string(ret));
      return false;
    }
    opened_ = true;
    own_vb_ = false;
    return true;
  }

  ret = CVI_VB_Init();
  if (ret != CVI_SUCCESS) {
    if (!config_.reuse_existing) {
      setError(error, "CVI_VB_Init failed, ret=" + std::to_string(ret));
      return false;
    }
    opened_ = true;
    own_vb_ = false;
    return true;
  }

  opened_ = true;
  own_vb_ = true;
  return true;
}

void VideoBufferManager::close() {
  if (own_vb_) {
    CVI_VB_Exit();
    own_vb_ = false;
  }
  opened_ = false;
}

bool VideoBufferManager::isOpen() const { return opened_; }

VideoBufferPool::VideoBufferPool() = default;

VideoBufferPool::VideoBufferPool(const Config &config) : config_(config) {}

VideoBufferPool::~VideoBufferPool() { destroy(); }

bool VideoBufferPool::create(std::string *error) {
  if (created_) {
    return true;
  }

  block_size_ = resolveBlockSize(config_);
  VB_POOL_CONFIG_S pool_config;
  std::memset(&pool_config, 0, sizeof(pool_config));
  pool_config.u32BlkSize = block_size_;
  pool_config.u32BlkCnt = static_cast<CVI_U32>(config_.block_count);
  pool_config.enRemapMode = toRemapMode(config_.remap_mode, config_.cached);
  if (!config_.name.empty()) {
    std::snprintf(pool_config.acName, sizeof(pool_config.acName), "%s",
                  config_.name.c_str());
  }

  pool_id_ = CVI_VB_CreatePool(&pool_config);
  if (pool_id_ == VB_INVALID_POOLID) {
    setError(error, "CVI_VB_CreatePool failed");
    return false;
  }

  created_ = true;
  return true;
}

void VideoBufferPool::destroy() {
  if (mapped_) {
    CVI_VB_MunmapPool(pool_id_);
    mapped_ = false;
  }
  if (created_) {
    CVI_VB_DestroyPool(pool_id_);
    created_ = false;
  }
  pool_id_ = VB_INVALID_POOLID;
  block_size_ = 0;
}

bool VideoBufferPool::isCreated() const { return created_; }

int VideoBufferPool::poolId() const { return static_cast<int>(pool_id_); }

bool VideoBufferPool::acquire(VideoBufferBlock *block, std::string *error) {
  if (!created_ && !create(error)) {
    return false;
  }
  if (!block) {
    setError(error, "video buffer block pointer is null");
    return false;
  }

  const VB_BLK handle = CVI_VB_GetBlock(pool_id_, block_size_);
  if (handle == VB_INVALID_HANDLE) {
    setError(error, "CVI_VB_GetBlock failed");
    return false;
  }

  block->handle = handle;
  block->physical = CVI_VB_Handle2PhysAddr(handle);
  block->virtual_addr = nullptr;
  block->size = block_size_;
  block->pool_id = static_cast<int>(pool_id_);
  return true;
}

bool VideoBufferPool::release(VideoBufferBlock *block, std::string *error) {
  if (!block) {
    setError(error, "video buffer block pointer is null");
    return false;
  }
  if (block->pool_id < 0 || block->size == 0 ||
      block->handle == VB_INVALID_HANDLE) {
    return true;
  }

  const int ret = CVI_VB_ReleaseBlock(block->handle);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VB_ReleaseBlock failed, ret=" + std::to_string(ret));
    return false;
  }

  block->handle = 0;
  block->physical = 0;
  block->virtual_addr = nullptr;
  block->size = 0;
  block->pool_id = -1;
  return true;
}

bool VideoBufferPool::map(VideoBufferBlock *block, std::string *error) {
  if (!block) {
    setError(error, "video buffer block pointer is null");
    return false;
  }
  if (!created_) {
    setError(error, "video buffer pool is not created");
    return false;
  }
  if (!mapped_) {
    const int ret = CVI_VB_MmapPool(pool_id_);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VB_MmapPool failed, ret=" + std::to_string(ret));
      return false;
    }
    mapped_ = true;
  }

  void *virtual_addr = nullptr;
  const int ret = CVI_VB_GetBlockVirAddr(pool_id_, block->handle, &virtual_addr);
  if (ret != CVI_SUCCESS) {
    setError(error,
             "CVI_VB_GetBlockVirAddr failed, ret=" + std::to_string(ret));
    return false;
  }

  block->virtual_addr = virtual_addr;
  return true;
}

bool VideoBufferPool::unmap(std::string *error) {
  if (!mapped_) {
    return true;
  }
  const int ret = CVI_VB_MunmapPool(pool_id_);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VB_MunmapPool failed, ret=" + std::to_string(ret));
    return false;
  }
  mapped_ = false;
  return true;
}

}  // namespace tdl_app
