#include "tdl_app/mmf.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "tdl_app/media_link.hpp"
#include "tdl_app/media_system.hpp"
#include "tdl_app/media_types.hpp"
#include "tdl_app/osd_region.hpp"
#include "tdl_app/vdec_channel.hpp"
#include "tdl_app/venc_channel.hpp"
#include "tdl_app/vo_output.hpp"
#include "tdl_app/vpss_group.hpp"

namespace tdl_app {
namespace {

VpssGroup::Config toVpssGroupConfig(const Mmf::VpssConfig &config) {
  VpssGroup::Config vpss_config;
  vpss_config.group.group = config.group;
  vpss_config.group.max_size.width = config.input_width;
  vpss_config.group.max_size.height = config.input_height;
  vpss_config.group.pixel_format = config.pixel_format;
  vpss_config.group.frame_rate.src_fps = config.src_fps;
  vpss_config.group.frame_rate.dst_fps = config.dst_fps;
  vpss_config.channel.channel = config.channel;
  vpss_config.channel.output_size.width = config.output_width;
  vpss_config.channel.output_size.height = config.output_height;
  vpss_config.channel.pixel_format = config.pixel_format;
  vpss_config.channel.frame_rate.src_fps = config.src_fps;
  vpss_config.channel.frame_rate.dst_fps = config.dst_fps;
  vpss_config.channel.mirror = config.mirror;
  vpss_config.channel.flip = config.flip;
  vpss_config.channel.normalize = config.normalize;
  return vpss_config;
}

MediaLink::Config toLinkConfig(const Mmf::BindConfig &config) {
  MediaLink::Config link_config;
  link_config.source = config.source;
  link_config.destination = config.destination;
  return link_config;
}

}  // namespace

class Mmf::Impl {
 public:
  explicit Impl(Config config) : config_(config) {}

  bool open(std::string *error) {
    if (opened_) {
      return true;
    }

    if (!openSystem(error)) {
      close();
      return false;
    }
    if (!openAllVpss(error)) {
      close();
      return false;
    }
    if (!openAllVdec(error)) {
      close();
      return false;
    }
    if (!openAllVenc(error)) {
      close();
      return false;
    }
    if (!openAllVo(error)) {
      close();
      return false;
    }
    if (!openAllOsd(error)) {
      close();
      return false;
    }
    if (!openAllOsdAttaches(error)) {
      close();
      return false;
    }
    if (!openAllLinks(error)) {
      close();
      return false;
    }

    opened_ = true;
    return true;
  }

  void close() {
    for (auto it = media_links_.rbegin(); it != media_links_.rend(); ++it) {
      if (*it) {
        (*it)->unbind();
      }
    }
    media_links_.clear();
    for (auto it = osd_regions_.rbegin(); it != osd_regions_.rend(); ++it) {
      if (*it) {
        (*it)->detach();
        (*it)->destroy();
      }
    }
    osd_regions_.clear();
    for (auto it = vo_outputs_.rbegin(); it != vo_outputs_.rend(); ++it) {
      if (*it) {
        (*it)->close();
      }
    }
    vo_outputs_.clear();
    for (auto it = venc_channels_.rbegin(); it != venc_channels_.rend(); ++it) {
      if (*it) {
        (*it)->close();
      }
    }
    venc_channels_.clear();
    for (auto it = vdec_channels_.rbegin(); it != vdec_channels_.rend(); ++it) {
      if (*it) {
        (*it)->close();
      }
    }
    vdec_channels_.clear();
    for (auto it = vpss_groups_.rbegin(); it != vpss_groups_.rend(); ++it) {
      if (*it) {
        (*it)->close();
      }
    }
    vpss_groups_.clear();
    if (media_system_) {
      media_system_->close();
      media_system_.reset();
    }
    opened_ = false;
  }

  bool isOpen() const { return opened_; }

 private:
  bool openSystem(std::string *error) {
    MediaSystem::Config system_config =
        MediaSystem::Config::attachExisting();
    system_config.reuse_existing = config_.reuse_existing_system;
    system_config.configure_vb = config_.configure_vb;
    std::vector<PoolConfig> pools = config_.pools;
    if (pools.empty()) {
      pools.push_back(config_.pool);
    }
    for (std::size_t i = 0; i < pools.size(); ++i) {
      VideoBufferPoolConfig pool;
      pool.size.width = pools[i].width;
      pool.size.height = pools[i].height;
      pool.pixel_format = pools[i].pixel_format;
      pool.block_count = pools[i].block_count;
      pool.align = pools[i].align;
      pool.cached = pools[i].cached;
      system_config.pools.push_back(pool);
    }
    if (!system_config.pools.empty()) {
      system_config.pool = system_config.pools.front();
    }

    media_system_.reset(new MediaSystem(system_config));
    return media_system_->open(error);
  }

  std::vector<VpssConfig> effectiveVpssConfigs() const {
    if (!config_.graph.vpss.empty()) {
      return config_.graph.vpss;
    }
    if (config_.vpss.enable) {
      return {config_.vpss};
    }
    return {};
  }

  std::vector<BindConfig> effectiveLinkConfigs() const {
    if (!config_.graph.binds.empty()) {
      return config_.graph.binds;
    }
    if (config_.bind.enable) {
      return {config_.bind};
    }
    return {};
  }

  bool openAllVpss(std::string *error) {
    const auto configs = effectiveVpssConfigs();
    std::vector<VpssGroup::Config> grouped_configs;
    for (const auto &config : configs) {
      const auto it = std::find_if(
          grouped_configs.begin(), grouped_configs.end(),
          [&](const VpssGroup::Config &item) {
            return item.group.group == config.group;
          });
      if (it == grouped_configs.end()) {
        VpssGroup::Config merged = toVpssGroupConfig(config);
        merged.channels.clear();
        merged.channels.push_back(merged.channel);
        grouped_configs.push_back(merged);
      } else {
        if (it->group.max_size.width != config.input_width ||
            it->group.max_size.height != config.input_height ||
            it->group.pixel_format != config.pixel_format) {
          if (error) {
            *error = "mmf vpss group " + std::to_string(config.group) +
                     " has inconsistent group attributes across channels";
          }
          return false;
        }
        it->channels.push_back(toVpssGroupConfig(config).channel);
      }
    }

    for (const auto &config : grouped_configs) {
      std::unique_ptr<VpssGroup> group(new VpssGroup(config));
      if (!group->open(error)) {
        return false;
      }
      vpss_groups_.push_back(std::move(group));
    }
    return true;
  }

  bool openAllLinks(std::string *error) {
    const auto configs = effectiveLinkConfigs();
    for (const auto &config : configs) {
      std::unique_ptr<MediaLink> link(new MediaLink(toLinkConfig(config)));
      if (!link->bind(error)) {
        return false;
      }
      media_links_.push_back(std::move(link));
    }
    return true;
  }

  bool openAllVenc(std::string *error) {
    for (const auto &config : config_.graph.venc) {
      std::unique_ptr<VencChannel> channel(new VencChannel(config));
      if (!channel->open(error)) {
        return false;
      }
      venc_channels_.push_back(std::move(channel));
    }
    return true;
  }

  bool openAllVdec(std::string *error) {
    for (const auto &config : config_.graph.vdec) {
      std::unique_ptr<VdecChannel> channel(new VdecChannel(config));
      if (!channel->open(error)) {
        return false;
      }
      vdec_channels_.push_back(std::move(channel));
    }
    return true;
  }

  bool openAllVo(std::string *error) {
    for (const auto &config : config_.graph.vo) {
      std::unique_ptr<VoOutput> output(new VoOutput(config));
      if (!output->open(error)) {
        return false;
      }
      vo_outputs_.push_back(std::move(output));
    }
    return true;
  }

  bool openAllOsd(std::string *error) {
    for (const auto &config : config_.graph.osd) {
      std::unique_ptr<OsdRegion> region(new OsdRegion(config));
      if (!region->create(error)) {
        return false;
      }
      osd_regions_.push_back(std::move(region));
    }
    return true;
  }

  bool openAllOsdAttaches(std::string *error) {
    for (const auto &config : config_.graph.osd_attaches) {
      if (!config.enable) {
        continue;
      }

      OsdRegion *region = findOsdRegion(config.handle);
      if (!region) {
        if (error) {
          *error = "mmf osd attach references unknown handle: " +
                   std::to_string(config.handle);
        }
        return false;
      }

      if (!region->attach(config.channel, config.x, config.y, config.layer,
                          error)) {
        return false;
      }
      if (!region->setVisible(config.visible, error)) {
        return false;
      }
    }
    return true;
  }

  OsdRegion *findOsdRegion(int handle) {
    const auto it = std::find_if(
        osd_regions_.begin(), osd_regions_.end(),
        [handle](const std::unique_ptr<OsdRegion> &region) {
          return region && region->handle() == handle;
        });
    if (it == osd_regions_.end()) {
      return nullptr;
    }
    return it->get();
  }

  Config config_;
  std::unique_ptr<MediaSystem> media_system_;
  std::vector<std::unique_ptr<VpssGroup>> vpss_groups_;
  std::vector<std::unique_ptr<VdecChannel>> vdec_channels_;
  std::vector<std::unique_ptr<VencChannel>> venc_channels_;
  std::vector<std::unique_ptr<VoOutput>> vo_outputs_;
  std::vector<std::unique_ptr<OsdRegion>> osd_regions_;
  std::vector<std::unique_ptr<MediaLink>> media_links_;
  bool opened_ = false;
};

Mmf::Mmf() : Mmf(Config{}) {}

Mmf::Mmf(const Config &config) : config_(config), impl_(new Impl(config_)) {}

Mmf::~Mmf() {
  delete impl_;
  impl_ = nullptr;
}

bool Mmf::open(std::string *error) { return impl_->open(error); }

void Mmf::close() { impl_->close(); }

bool Mmf::isOpen() const { return impl_->isOpen(); }

}  // namespace tdl_app
