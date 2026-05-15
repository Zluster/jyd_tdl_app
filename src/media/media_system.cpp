#include "tdl_app/media_system.hpp"

#include <cstring>

#include "cvi_buffer.h"
#include "cvi_comm_vb.h"
#include "cvi_sys.h"
#include "cvi_vb.h"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

}  // namespace

MediaSystem::MediaSystem() = default;

MediaSystem::MediaSystem(const Config &config) : config_(config) {}

MediaSystem::~MediaSystem() { close(); }

bool MediaSystem::open(std::string *error) {
  if (opened_) {
    return true;
  }

  int ret = CVI_SYS_Init();
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SYS_Init failed, ret=" + std::to_string(ret));
    return false;
  }
  own_sys_ = true;

  if (config_.configure_vb) {
    VB_CONFIG_S vb_config;
    std::memset(&vb_config, 0, sizeof(vb_config));
    vb_config.u32MaxPoolCnt = 1;
    vb_config.astCommPool[0].u32BlkSize = COMMON_GetPicBufferSize(
        static_cast<CVI_U32>(config_.pool.size.width),
        static_cast<CVI_U32>(config_.pool.size.height),
        static_cast<PIXEL_FORMAT_E>(config_.pool.pixel_format), DATA_BITWIDTH_8,
        COMPRESS_MODE_NONE, static_cast<CVI_U32>(config_.pool.align));
    vb_config.astCommPool[0].u32BlkCnt =
        static_cast<CVI_U32>(config_.pool.block_count);
    vb_config.astCommPool[0].enRemapMode =
        config_.pool.cached ? VB_REMAP_MODE_CACHED : VB_REMAP_MODE_NONE;

    ret = CVI_VB_SetConfig(&vb_config);
    if (ret == CVI_SUCCESS) {
      ret = CVI_VB_Init();
      if (ret != CVI_SUCCESS) {
        setError(error, "CVI_VB_Init failed, ret=" + std::to_string(ret));
        close();
        return false;
      }
      own_vb_ = true;
    } else if (!config_.reuse_existing) {
      setError(error, "CVI_VB_SetConfig failed, ret=" + std::to_string(ret));
      close();
      return false;
    }
  }

  opened_ = true;
  return true;
}

void MediaSystem::close() {
  if (own_vb_) {
    CVI_VB_Exit();
    own_vb_ = false;
  }
  if (own_sys_) {
    CVI_SYS_Exit();
    own_sys_ = false;
  }
  opened_ = false;
}

bool MediaSystem::isOpen() const { return opened_; }

}  // namespace tdl_app
