#include "tdl_app/media_system.hpp"

#include <memory>
#include <vector>

#include "tdl_app/sys_context.hpp"
#include "tdl_app/video_buffer.hpp"

namespace tdl_app {

MediaSystem::MediaSystem() = default;

MediaSystem::MediaSystem(const Config &config) : config_(config) {}

MediaSystem::~MediaSystem() { close(); }

bool MediaSystem::open(std::string *error) {
  if (opened_) {
    return true;
  }

  if (config_.configure_vb) {
    std::vector<VideoBufferPoolConfig> pools = config_.pools;
    if (pools.empty()) {
      pools.push_back(config_.pool);
    }

    video_buffers_.reset(
        new VideoBufferManager(VideoBufferManager::withCommonPools(
            pools, config_.reuse_existing)));
    if (!video_buffers_->open(error)) {
      video_buffers_.reset();
      return false;
    }
  }

  sys_context_.reset(
      new SysContext(config_.reuse_existing ? SysContext::Config::reuseExisting()
                                            : SysContext::Config::createNew()));
  if (!sys_context_->open(error)) {
    close();
    return false;
  }

  opened_ = true;
  return true;
}

void MediaSystem::close() {
  if (sys_context_) {
    sys_context_->close();
    sys_context_.reset();
  }
  if (video_buffers_) {
    video_buffers_->close();
    video_buffers_.reset();
  }
  opened_ = false;
}

bool MediaSystem::isOpen() const { return opened_; }

}  // namespace tdl_app
