#pragma once

#include <string>
#include <vector>

#include "tdl_app/osd_region.hpp"
#include "tdl_app/vdec_channel.hpp"
#include "tdl_app/venc_channel.hpp"
#include "tdl_app/vo_output.hpp"

namespace tdl_app {

class Mmf {
 public:
  struct PoolConfig {
    int width = 1920;
    int height = 1080;
    int pixel_format = 18;
    int block_count = 8;
    int align = 64;
    bool cached = true;
  };

  struct VpssConfig {
    int group = 0;
    int input_width = 1920;
    int input_height = 1080;
    int output_width = 640;
    int output_height = 640;
    int channel = 0;
    int pixel_format = 18;
    int src_fps = -1;
    int dst_fps = -1;
    bool enable = false;
    bool mirror = false;
    bool flip = false;
    bool normalize = false;
  };

  struct BindConfig {
    MediaChannel source;
    MediaChannel destination;
    bool enable = false;
  };

  struct OsdAttachConfig {
    int handle = -1;
    MediaChannel channel;
    int x = 0;
    int y = 0;
    int layer = 0;
    bool visible = true;
    bool enable = false;
  };

  struct GraphConfig {
    std::vector<VpssConfig> vpss;
    std::vector<VencChannel::Config> venc;
    std::vector<VdecChannel::Config> vdec;
    std::vector<VoOutput::Config> vo;
    std::vector<OsdRegion::Config> osd;
    std::vector<OsdAttachConfig> osd_attaches;
    std::vector<BindConfig> binds;
  };

  struct Config {
    PoolConfig pool;
    VpssConfig vpss;
    BindConfig bind;
    GraphConfig graph;
    bool reuse_existing_system = true;
    bool configure_vb = true;

    static Config attachExisting() {
      Config config;
      config.reuse_existing_system = true;
      config.configure_vb = false;
      return config;
    }
  };

  Mmf();
  explicit Mmf(const Config &config);
  ~Mmf();

  Mmf(const Mmf &) = delete;
  Mmf &operator=(const Mmf &) = delete;

  bool open(std::string *error = nullptr);
  void close();

  bool isOpen() const;

  static VpssConfig vpssOutput(int group = 0, int channel = 0,
                               int input_width = 1920, int input_height = 1080,
                               int output_width = 640, int output_height = 640,
                               int pixel_format = 18) {
    VpssConfig config;
    config.group = group;
    config.channel = channel;
    config.input_width = input_width;
    config.input_height = input_height;
    config.output_width = output_width;
    config.output_height = output_height;
    config.pixel_format = pixel_format;
    config.enable = true;
    return config;
  }

  static BindConfig viToVpss(int vi_pipe = 0, int vi_channel = 0,
                             int vpss_group = 0, int vpss_channel = 0) {
    BindConfig config;
    config.source = MediaChannel::vi(vi_pipe, vi_channel);
    config.destination = MediaChannel::vpss(vpss_group, vpss_channel);
    config.enable = true;
    return config;
  }

  static BindConfig bind(const MediaChannel &source,
                         const MediaChannel &destination) {
    BindConfig config;
    config.source = source;
    config.destination = destination;
    config.enable = true;
    return config;
  }

  static OsdAttachConfig osdAttach(int handle, const MediaChannel &channel,
                                   int x = 0, int y = 0, int layer = 0,
                                   bool visible = true) {
    OsdAttachConfig config;
    config.handle = handle;
    config.channel = channel;
    config.x = x;
    config.y = y;
    config.layer = layer;
    config.visible = visible;
    config.enable = true;
    return config;
  }

  static Config singleVpss(int group = 0, int channel = 0,
                           int input_width = 1920, int input_height = 1080,
                           int output_width = 640, int output_height = 640,
                           int pixel_format = 18,
                           bool bind_vi = false, int vi_pipe = 0,
                           int vi_channel = 0) {
    Config config;
    config.pool.width = output_width;
    config.pool.height = output_height;
    config.pool.pixel_format = pixel_format;
    config.vpss = vpssOutput(group, channel, input_width, input_height,
                             output_width, output_height, pixel_format);
    if (bind_vi) {
      config.bind = viToVpss(vi_pipe, vi_channel, group, channel);
    }
    return config;
  }

  static Config viToMultiVpss(
      const std::vector<VpssConfig> &outputs, int vi_pipe = 0,
      int vi_channel = 0, bool configure_vb = true) {
    Config config;
    config.configure_vb = configure_vb;
    config.graph.vpss = outputs;
    for (std::vector<VpssConfig>::const_iterator it = outputs.begin();
         it != outputs.end(); ++it) {
      config.graph.binds.push_back(
          viToVpss(vi_pipe, vi_channel, it->group, it->channel));
    }
    return config;
  }

  static Config mediaGraph(const GraphConfig &graph,
                           bool reuse_existing_system = true,
                           bool configure_vb = true) {
    Config config;
    config.graph = graph;
    config.reuse_existing_system = reuse_existing_system;
    config.configure_vb = configure_vb;
    return config;
  }

 private:
  class Impl;
  Config config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
