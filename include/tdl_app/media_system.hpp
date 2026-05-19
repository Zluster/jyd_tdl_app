#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tdl_app/media_types.hpp"

namespace tdl_app {

class SysContext;
class VideoBufferManager;

class MediaSystem {
 public:
  struct Config {
    bool reuse_existing = true;
    bool configure_vb = false;
    VideoBufferPoolConfig pool;
    std::vector<VideoBufferPoolConfig> pools;

    static Config attachExisting() {
      Config config;
      config.reuse_existing = true;
      config.configure_vb = false;
      return config;
    }

    static Config withVideoBuffer(const VideoBufferPoolConfig &pool_config,
                                  bool reuse = true) {
      Config config;
      config.reuse_existing = reuse;
      config.configure_vb = true;
      config.pool = pool_config;
      config.pools.push_back(pool_config);
      return config;
    }

    static Config withVideoBuffers(const std::vector<VideoBufferPoolConfig> &pool_configs,
                                   bool reuse = true) {
      Config config;
      config.reuse_existing = reuse;
      config.configure_vb = true;
      config.pools = pool_configs;
      if (!config.pools.empty()) {
        config.pool = config.pools.front();
      }
      return config;
    }
  };

  MediaSystem();
  explicit MediaSystem(const Config &config);
  ~MediaSystem();

  MediaSystem(const MediaSystem &) = delete;
  MediaSystem &operator=(const MediaSystem &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

 private:
  Config config_;
  std::unique_ptr<SysContext> sys_context_;
  std::unique_ptr<VideoBufferManager> video_buffers_;
  bool opened_ = false;
};

}  // namespace tdl_app
