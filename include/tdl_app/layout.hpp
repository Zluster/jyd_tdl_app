#pragma once

#include "tdl_app/media_types.hpp"

namespace tdl_app {

struct DualOsLayout {
  static constexpr int kCaptureVpssGroup = 0;
  static constexpr int kDisplayVpssGroup = 1;

  static constexpr int kMainChannel = 0;
  static constexpr int kAiChannel = 1;
  static constexpr int kLiveChannel = 2;
  static constexpr int kSubRgbChannel = 3;
  static constexpr int kDisplayChannel = 0;

  static constexpr int kMainWidth = 1920;
  static constexpr int kMainHeight = 1080;
  static constexpr int kMainPixelFormat = PixelFormat::NV12;

  static constexpr int kAiWidth = 640;
  static constexpr int kAiHeight = 640;
  static constexpr int kAiPixelFormat = PixelFormat::RGB888_PLANAR;

  static constexpr int kLiveWidth = 1280;
  static constexpr int kLiveHeight = 720;
  static constexpr int kLivePixelFormat = PixelFormat::NV12;

  static constexpr int kSubRgbWidth = 640;
  static constexpr int kSubRgbHeight = 640;
  static constexpr int kSubRgbPixelFormat = PixelFormat::NV21;

  static constexpr int kScreenWidth = 720;
  static constexpr int kScreenHeight = 1280;
  static constexpr int kDisplaySourceWidth = 1280;
  static constexpr int kDisplaySourceHeight = 720;
  static constexpr int kDisplayPixelFormat = PixelFormat::NV12;

  static constexpr int kVoDevice = 0;
  static constexpr int kVoChannel = 0;
  static constexpr int kVoRotation = 90;
};

}  // namespace tdl_app
