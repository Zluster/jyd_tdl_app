#pragma once

#include <string>
#include <vector>

#include "tdl_app/media_types.hpp"

namespace tdl_app {

class VpssGroup {
 public:
  struct Config {
    VpssGroupConfig group;
    VpssChannelConfig channel;
    std::vector<VpssChannelConfig> channels;
  };

  VpssGroup();
  explicit VpssGroup(const Config &config);
  ~VpssGroup();

  VpssGroup(const VpssGroup &) = delete;
  VpssGroup &operator=(const VpssGroup &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

 private:
  Config config_;
  bool created_ = false;
  bool channel_enabled_ = false;
  bool started_ = false;
  std::vector<int> enabled_channels_;
};

}  // namespace tdl_app
