#include "tdl_app/media_link.hpp"

#include "cvi_sys.h"

#include "media/private/media_common.hpp"

namespace tdl_app {

MediaLink::MediaLink() = default;

MediaLink::MediaLink(const Config &config) : config_(config) {}

MediaLink::~MediaLink() { unbind(); }

bool MediaLink::bind(std::string *error) {
  if (bound_) {
    return true;
  }

  const MMF_CHN_S src = private_media::detail::toMmfChannel(config_.source);
  const MMF_CHN_S dst =
      private_media::detail::toMmfChannel(config_.destination);
  const int ret = CVI_SYS_Bind(&src, &dst);
  if (ret != CVI_SUCCESS) {
    private_media::detail::setError(
        error, "CVI_SYS_Bind failed, ret=" + std::to_string(ret));
    return false;
  }

  bound_ = true;
  return true;
}

void MediaLink::unbind() {
  if (!bound_) {
    return;
  }
  const MMF_CHN_S src = private_media::detail::toMmfChannel(config_.source);
  const MMF_CHN_S dst =
      private_media::detail::toMmfChannel(config_.destination);
  CVI_SYS_UnBind(&src, &dst);
  bound_ = false;
}

bool MediaLink::isBound() const { return bound_; }

}  // namespace tdl_app
