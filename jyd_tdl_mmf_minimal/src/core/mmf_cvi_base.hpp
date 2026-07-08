#ifndef MMF_CVI_BASE_HPP
#define MMF_CVI_BASE_HPP

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cvi_audio.h"
#include "cvi_buffer.h"
#include "cvi_comm_adec.h"
#include "cvi_comm_aenc.h"
#include "cvi_comm_region.h"
#include "cvi_comm_vdec.h"
#include "cvi_comm_venc.h"
#include "cvi_comm_video.h"
#include "cvi_comm_vpss.h"
#include "cvi_region.h"
#include "cvi_sys.h"
#include "cvi_type.h"
#include "cvi_vdec.h"
#include "cvi_venc.h"
#include "cvi_vi.h"
#include "cvi_vo.h"
#include "cvi_vpss.h"
#include "mmf/mmf.h"

namespace mmf_cvi {

struct PixelFormat {
  static constexpr int RGB888 = PIXEL_FORMAT_RGB_888;
  static constexpr int BGR888 = PIXEL_FORMAT_BGR_888;
  static constexpr int RGB888_PLANAR = PIXEL_FORMAT_RGB_888_PLANAR;
  static constexpr int BGR888_PLANAR = PIXEL_FORMAT_BGR_888_PLANAR;
  static constexpr int ARGB1555 = PIXEL_FORMAT_ARGB_1555;
  static constexpr int ARGB4444 = PIXEL_FORMAT_ARGB_4444;
  static constexpr int ARGB8888 = PIXEL_FORMAT_ARGB_8888;
  static constexpr int YUV400 = PIXEL_FORMAT_YUV_400;
  static constexpr int NV12 = PIXEL_FORMAT_NV12;
  static constexpr int NV21 = PIXEL_FORMAT_NV21;
};

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
  static constexpr int kAiWidth = 640;
  static constexpr int kAiHeight = 640;
  static constexpr int kLiveWidth = 1280;
  static constexpr int kLiveHeight = 720;
  static constexpr int kSubRgbWidth = 640;
  static constexpr int kSubRgbHeight = 640;
  static constexpr int kScreenWidth = 720;
  static constexpr int kScreenHeight = 1280;
  static constexpr int kVoDevice = 0;
  static constexpr int kVoChannel = 0;
  static constexpr int kVoRotation = 90;
};

enum class MediaModule { Vi = 0, Vpss = 1, Venc = 2, Vo = 3, Rgn = 4, Vdec = 5, Unknown = 255 };
struct MediaSize {
  int width = 0;
  int height = 0;
  static MediaSize make(int w, int h) {
    MediaSize s;
    s.width = w;
    s.height = h;
    return s;
  }
};
struct MediaChannel {
  MediaModule module = MediaModule::Unknown;
  int device = 0;
  int channel = 0;
  static MediaChannel vpss(int group = 0, int channel = 0) {
    MediaChannel c;
    c.module = MediaModule::Vpss;
    c.device = group;
    c.channel = channel;
    return c;
  }
  static MediaChannel vo(int layer = 0, int channel = 0) {
    MediaChannel c;
    c.module = MediaModule::Vo;
    c.device = layer;
    c.channel = channel;
    return c;
  }
  static MediaChannel vdec(int channel = 0) {
    MediaChannel c;
    c.module = MediaModule::Vdec;
    c.channel = channel;
    return c;
  }
};
struct Frame {
  std::string image_path;
  void* native = nullptr;
  int width = 0;
  int height = 0;
  int format = 0;
  uint64_t timestamp_us = 0;
  uint32_t sequence = 0;
};
}  // namespace mmf_cvi

#endif  // MMF_CVI_BASE_HPP
