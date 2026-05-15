#pragma once

#include <string>

#include "tdl_app/media_types.hpp"

namespace tdl_app {

class MediaSystem {
 public:
  struct Config {
    bool reuse_existing = true;
    bool configure_vb = false;
    VideoBufferPoolConfig pool;

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
  bool opened_ = false;
  bool own_sys_ = false;
  bool own_vb_ = false;
};

}  // namespace tdl_app
