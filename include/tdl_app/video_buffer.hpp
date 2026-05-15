#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "tdl_app/media_types.hpp"

namespace tdl_app {

struct VideoBufferBlock {
  std::uint64_t handle = 0;
  std::uint64_t physical = 0;
  void *virtual_addr = nullptr;
  std::size_t size = 0;
  int pool_id = -1;
};

class VideoBufferManager {
 public:
  struct Config {
    bool reuse_existing = true;
    std::vector<VideoBufferPoolConfig> common_pools;
  };

  static Config withCommonPools(const std::vector<VideoBufferPoolConfig> &pools,
                                bool reuse_existing = true) {
    Config config;
    config.reuse_existing = reuse_existing;
    config.common_pools = pools;
    return config;
  }

  VideoBufferManager();
  explicit VideoBufferManager(const Config &config);
  ~VideoBufferManager();

  VideoBufferManager(const VideoBufferManager &) = delete;
  VideoBufferManager &operator=(const VideoBufferManager &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

 private:
  Config config_;
  bool opened_ = false;
  bool own_vb_ = false;
};

class VideoBufferPool {
 public:
  struct Config {
    MediaSize size {1920, 1080};
    int pixel_format = 18;
    int block_count = 4;
    int align = 64;
    bool cached = true;
    int remap_mode = -1;
    std::uint32_t block_size = 0;
    std::string name;
  };

  static Config picture(int width = 1920, int height = 1080,
                        int pixel_format = 18, int block_count = 4,
                        int align = 64, bool cached = true,
                        int remap_mode = -1,
                        const std::string &name = std::string()) {
    Config config;
    config.size.width = width;
    config.size.height = height;
    config.pixel_format = pixel_format;
    config.block_count = block_count;
    config.align = align;
    config.cached = cached;
    config.remap_mode = remap_mode;
    config.name = name;
    return config;
  }

  VideoBufferPool();
  explicit VideoBufferPool(const Config &config);
  ~VideoBufferPool();

  VideoBufferPool(const VideoBufferPool &) = delete;
  VideoBufferPool &operator=(const VideoBufferPool &) = delete;

  bool create(std::string *error = nullptr);
  void destroy();
  bool isCreated() const;
  int poolId() const;

  bool acquire(VideoBufferBlock *block, std::string *error = nullptr);
  bool release(VideoBufferBlock *block, std::string *error = nullptr);
  bool map(VideoBufferBlock *block, std::string *error = nullptr);
  bool unmap(std::string *error = nullptr);

 private:
  Config config_;
  unsigned int pool_id_ = static_cast<unsigned int>(-1);
  std::uint32_t block_size_ = 0;
  bool created_ = false;
  bool mapped_ = false;
};

}  // namespace tdl_app
