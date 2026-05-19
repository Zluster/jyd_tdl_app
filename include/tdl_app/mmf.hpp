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
    std::vector<PoolConfig> pools;
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

  class Builder {
   public:
    Builder &setPool(const PoolConfig &pool) {
      config_.pool = pool;
      config_.pools.clear();
      config_.pools.push_back(pool);
      return *this;
    }

    Builder &setPools(const std::vector<PoolConfig> &pools) {
      config_.pools = pools;
      if (!config_.pools.empty()) {
        config_.pool = config_.pools.front();
      }
      return *this;
    }

    Builder &addPool(const PoolConfig &pool) {
      if (config_.pools.empty()) {
        config_.pool = pool;
      }
      config_.pools.push_back(pool);
      return *this;
    }

    Builder &addVpss(const VpssConfig &config) {
      config_.graph.vpss.push_back(config);
      return *this;
    }

    Builder &addBind(const BindConfig &config) {
      config_.graph.binds.push_back(config);
      return *this;
    }

    Builder &addViToVpss(const VpssConfig &config, int vi_pipe = 0,
                         int vi_channel = 0) {
      config_.graph.vpss.push_back(config);
      config_.graph.binds.push_back(
          Mmf::viToVpss(vi_pipe, vi_channel, config.group, config.channel));
      return *this;
    }

    Builder &addViToVpss(const std::vector<VpssConfig> &configs,
                         int vi_pipe = 0, int vi_channel = 0) {
      for (std::vector<VpssConfig>::const_iterator it = configs.begin();
           it != configs.end(); ++it) {
        addViToVpss(*it, vi_pipe, vi_channel);
      }
      return *this;
    }

    Builder &addVpssToVenc(int vpss_group, int vpss_channel,
                           int venc_channel) {
      config_.graph.binds.push_back(Mmf::bind(
          MediaChannel::vpss(vpss_group, vpss_channel),
          MediaChannel::venc(venc_channel)));
      return *this;
    }

    Builder &addVdecToVpss(int vdec_channel, int vpss_group,
                           int vpss_channel) {
      config_.graph.binds.push_back(Mmf::bind(
          MediaChannel::vdec(vdec_channel),
          MediaChannel::vpss(vpss_group, vpss_channel)));
      return *this;
    }

    Builder &addVpssToVo(int vpss_group, int vpss_channel, int vo_device = 0,
                         int vo_channel = 0) {
      config_.graph.binds.push_back(Mmf::bind(
          MediaChannel::vpss(vpss_group, vpss_channel),
          MediaChannel::vo(vo_device, vo_channel)));
      return *this;
    }

    Builder &addVenc(const VencChannel::Config &config) {
      config_.graph.venc.push_back(config);
      return *this;
    }

    Builder &addVdec(const VdecChannel::Config &config) {
      config_.graph.vdec.push_back(config);
      return *this;
    }

    Builder &addVo(const VoOutput::Config &config) {
      config_.graph.vo.push_back(config);
      return *this;
    }

    Builder &addOsd(const OsdRegion::Config &config) {
      config_.graph.osd.push_back(config);
      return *this;
    }

    Builder &addOsdAttach(const OsdAttachConfig &config) {
      config_.graph.osd_attaches.push_back(config);
      return *this;
    }

    Builder &setReuseExistingSystem(bool reuse) {
      config_.reuse_existing_system = reuse;
      return *this;
    }

    Builder &setConfigureVb(bool configure) {
      config_.configure_vb = configure;
      return *this;
    }

    Config build() const { return config_; }

   private:
    Config config_;
  };

  bool open(std::string *error = nullptr);
  void close();

  bool isOpen() const;

  static Builder builder() { return Builder(); }

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
    config.pools.push_back(config.pool);
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
    if (!outputs.empty()) {
      PoolConfig pool;
      pool.width = outputs.front().output_width;
      pool.height = outputs.front().output_height;
      pool.pixel_format = outputs.front().pixel_format;
      config.pool = pool;
    }
    config.graph.vpss = outputs;
    for (std::vector<VpssConfig>::const_iterator it = outputs.begin();
         it != outputs.end(); ++it) {
      PoolConfig pool;
      pool.width = it->output_width;
      pool.height = it->output_height;
      pool.pixel_format = it->pixel_format;
      config.pools.push_back(pool);
    }
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
