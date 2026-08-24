#pragma once

// Shared VPSS/VI frame -> BGR cv::Mat conversion for the CPU-preprocess NN
// runtimes. Every runtime used to carry its own copy of this code; new
// runtimes must use this header instead of duplicating it.

#include <cstddef>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "cvi_comm_video.h"
#include "cvi_comm_vpss.h"
#include "cvi_sys.h"
#include "cvi_vpss.h"
#include "tdl_app/frame_source.hpp"

namespace tdl_app {
namespace frame_convert {

inline bool profileEnabled() {
  const char *value = std::getenv("TDL_BENCH_PROFILE");
  return value && value[0] && value[0] != '0';
}

inline double elapsedMs(const std::chrono::steady_clock::time_point &begin) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - begin)
      .count();
}

inline void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

// Converts a live MMF frame to packed BGR with VPSS.  The output still needs
// one linear copy into cv::Mat because OpenCV owns the following warpAffine
// input, but the expensive RGB-planar channel shuffle stays in hardware.
class VpssBgrConverter {
 public:
  VpssBgrConverter() = default;
  ~VpssBgrConverter() { close(); }

  VpssBgrConverter(const VpssBgrConverter &) = delete;
  VpssBgrConverter &operator=(const VpssBgrConverter &) = delete;

  bool convert(const Frame &frame, cv::Mat *image, std::string *error) {
    if (!frame.native) {
      setError(error, "frame has no native VIDEO_FRAME_INFO_S buffer");
      return false;
    }
    return convertVideoFrame(*static_cast<const VIDEO_FRAME_INFO_S *>(frame.native),
                             image, error);
  }

  void close() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (channel_enabled_) {
      CVI_VPSS_DisableChn(group_id_, 0);
      channel_enabled_ = false;
    }
    if (group_started_) {
      CVI_VPSS_StopGrp(group_id_);
      group_started_ = false;
    }
    if (group_created_) {
      CVI_VPSS_DestroyGrp(group_id_);
      group_created_ = false;
    }
    group_id_ = -1;
    source_width_ = 0;
    source_height_ = 0;
    source_format_ = -1;
  }

 private:
  bool convertVideoFrame(const VIDEO_FRAME_INFO_S &source, cv::Mat *image,
                         std::string *error) {
    const auto total_begin = std::chrono::steady_clock::now();
    if (!image) {
      setError(error, "output image pointer is null");
      return false;
    }
    const VIDEO_FRAME_S &src = source.stVFrame;
    const int width = static_cast<int>(src.u32Width);
    const int height = static_cast<int>(src.u32Height);
    if (width <= 0 || height <= 0 || src.u64PhyAddr[0] == 0) {
      setError(error, "invalid source frame for VPSS BGR conversion");
      return false;
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!group_created_) {
      if (!createGroup(source, error)) return false;
    } else if (source_width_ != width || source_height_ != height ||
               source_format_ != static_cast<int>(src.enPixelFormat)) {
      setError(error, "VPSS BGR converter source format changed");
      return false;
    }

    const auto vpss_begin = std::chrono::steady_clock::now();
    VIDEO_FRAME_INFO_S source_copy = source;
    int ret = CVI_VPSS_SendFrame(group_id_, &source_copy, -1);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_SendFrame failed, ret=" + std::to_string(ret));
      return false;
    }
    const double vpss_ms = elapsedMs(vpss_begin);

    VIDEO_FRAME_INFO_S output{};
    ret = CVI_VPSS_GetChnFrame(group_id_, 0, &output, -1);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_GetChnFrame failed, ret=" + std::to_string(ret));
      return false;
    }
    struct OutputLease {
      int group = -1;
      VIDEO_FRAME_INFO_S *frame = nullptr;
      ~OutputLease() {
        if (frame) CVI_VPSS_ReleaseChnFrame(group, 0, frame);
      }
    } lease{group_id_, &output};

    const VIDEO_FRAME_S &dst = output.stVFrame;
    if (dst.enPixelFormat != PIXEL_FORMAT_BGR_888 ||
        dst.u32Width != src.u32Width || dst.u32Height != src.u32Height ||
        dst.u64PhyAddr[0] == 0 || dst.u32Length[0] == 0) {
      setError(error, "VPSS BGR converter returned an unexpected frame layout");
      return false;
    }
    const auto copy_begin = std::chrono::steady_clock::now();
    auto *mapped = static_cast<unsigned char *>(
        CVI_SYS_Mmap(dst.u64PhyAddr[0], dst.u32Length[0]));
    if (!mapped) {
      setError(error, "CVI_SYS_Mmap failed for VPSS BGR output");
      return false;
    }
    CVI_SYS_IonInvalidateCache(dst.u64PhyAddr[0], mapped, dst.u32Length[0]);
    cv::Mat output_image(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
      std::memcpy(output_image.ptr<unsigned char>(y),
                  mapped + static_cast<std::size_t>(y) * dst.u32Stride[0],
                  static_cast<std::size_t>(width) * 3);
    }
    CVI_SYS_Munmap(mapped, dst.u32Length[0]);
    *image = std::move(output_image);
    if (profileEnabled()) {
      std::fprintf(stderr,
                   "[profile] frameToBgrMat VPSS: group=%d size=%dx%d "
                   "vpss=%.3f map_copy=%.3f total=%.3f ms\n",
                   group_id_, width, height, vpss_ms, elapsedMs(copy_begin),
                   elapsedMs(total_begin));
    }
    return true;
  }

  bool createGroup(const VIDEO_FRAME_INFO_S &source, std::string *error) {
    static std::mutex create_mutex;
    std::lock_guard<std::mutex> create_lock(create_mutex);
    group_id_ = CVI_VPSS_GetAvailableGrp();
    if (group_id_ < 0) {
      setError(error, "no free VPSS group for BGR conversion");
      return false;
    }
    const VIDEO_FRAME_S &src = source.stVFrame;
    VPSS_GRP_ATTR_S group_attr{};
    group_attr.stFrameRate.s32SrcFrameRate = -1;
    group_attr.stFrameRate.s32DstFrameRate = -1;
    group_attr.enPixelFormat = src.enPixelFormat;
    group_attr.u32MaxW = src.u32Width;
    group_attr.u32MaxH = src.u32Height;
    group_attr.u8VpssDev = 0;
    int ret = CVI_VPSS_CreateGrp(group_id_, &group_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_CreateGrp failed, ret=" + std::to_string(ret));
      group_id_ = -1;
      return false;
    }
    group_created_ = true;

    VPSS_CHN_ATTR_S channel_attr{};
    channel_attr.u32Width = src.u32Width;
    channel_attr.u32Height = src.u32Height;
    channel_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
    channel_attr.enPixelFormat = PIXEL_FORMAT_BGR_888;
    channel_attr.stFrameRate.s32SrcFrameRate = -1;
    channel_attr.stFrameRate.s32DstFrameRate = -1;
    channel_attr.u32Depth = 1;
    channel_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
    channel_attr.stNormalize.bEnable = CVI_FALSE;
    ret = CVI_VPSS_SetChnAttr(group_id_, 0, &channel_attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_SetChnAttr failed, ret=" + std::to_string(ret));
      close();
      return false;
    }
    ret = CVI_VPSS_EnableChn(group_id_, 0);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_EnableChn failed, ret=" + std::to_string(ret));
      close();
      return false;
    }
    channel_enabled_ = true;
    ret = CVI_VPSS_StartGrp(group_id_);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VPSS_StartGrp failed, ret=" + std::to_string(ret));
      close();
      return false;
    }
    group_started_ = true;
    source_width_ = static_cast<int>(src.u32Width);
    source_height_ = static_cast<int>(src.u32Height);
    source_format_ = static_cast<int>(src.enPixelFormat);
    return true;
  }

  std::recursive_mutex mutex_;
  int group_id_ = -1;
  int source_width_ = 0;
  int source_height_ = 0;
  int source_format_ = -1;
  bool group_created_ = false;
  bool channel_enabled_ = false;
  bool group_started_ = false;
};

inline void copyPackedRgbToBgr(const VIDEO_FRAME_S &vf,
                               const unsigned char *mapped, int width,
                               int height, bool input_is_bgr, cv::Mat *image) {
  cv::Mat output(height, width, CV_8UC3);
  for (int y = 0; y < height; ++y) {
    const unsigned char *src = mapped + y * vf.u32Stride[0];
    unsigned char *dst = output.ptr<unsigned char>(y);
    if (input_is_bgr) {
      std::memcpy(dst, src, static_cast<size_t>(width) * 3);
    } else {
      for (int x = 0; x < width; ++x) {
        dst[x * 3 + 0] = src[x * 3 + 2];
        dst[x * 3 + 1] = src[x * 3 + 1];
        dst[x * 3 + 2] = src[x * 3 + 0];
      }
    }
  }
  *image = std::move(output);
}

inline void copyPlanarRgbToBgr(const VIDEO_FRAME_S &vf,
                               const unsigned char *mapped, int width,
                               int height, bool input_is_bgr, cv::Mat *image) {
  const unsigned char *plane0 = mapped;
  const unsigned char *plane1 = mapped + vf.u32Length[0];
  const unsigned char *plane2 = plane1 + vf.u32Length[1];
  cv::Mat output(height, width, CV_8UC3);
  for (int y = 0; y < height; ++y) {
    const unsigned char *src0 = plane0 + y * vf.u32Stride[0];
    const unsigned char *src1 = plane1 + y * vf.u32Stride[1];
    const unsigned char *src2 = plane2 + y * vf.u32Stride[2];
    cv::Vec3b *dst = output.ptr<cv::Vec3b>(y);
    for (int x = 0; x < width; ++x) {
      if (input_is_bgr) {
        dst[x] = cv::Vec3b(src0[x], src1[x], src2[x]);
      } else {
        dst[x] = cv::Vec3b(src2[x], src1[x], src0[x]);
      }
    }
  }
  *image = std::move(output);
}

// Convert a VPSS/VI frame straight into a BGR cv::Mat in memory so a
// CPU-preprocess runtime can consume a live data stream without writing a
// file first.
inline bool videoFrameToBgrMat(const VIDEO_FRAME_INFO_S &video_frame,
                               cv::Mat *image, std::string *error) {
  const auto total_begin = std::chrono::steady_clock::now();
  if (!image) {
    setError(error, "output image pointer is null");
    return false;
  }
  const auto &vf = video_frame.stVFrame;
  const int width = static_cast<int>(vf.u32Width);
  const int height = static_cast<int>(vf.u32Height);
  const int format = static_cast<int>(vf.enPixelFormat);
  if (width <= 0 || height <= 0) {
    setError(error, "invalid frame size");
    return false;
  }

  std::size_t map_size = 0;
  for (int i = 0; i < 3; ++i) {
    map_size += vf.u32Length[i];
  }
  if (map_size == 0) {
    setError(error, "frame buffer length is zero");
    return false;
  }

  const auto mmap_begin = std::chrono::steady_clock::now();
  auto *mapped =
      static_cast<unsigned char *>(CVI_SYS_Mmap(vf.u64PhyAddr[0], map_size));
  if (!mapped) {
    setError(error, "CVI_SYS_Mmap failed");
    return false;
  }
  const double mmap_ms = elapsedMs(mmap_begin);
  const auto cache_begin = std::chrono::steady_clock::now();
  CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], mapped, map_size);
  const double cache_ms = elapsedMs(cache_begin);

  bool ok = true;
  const auto copy_begin = std::chrono::steady_clock::now();
  if (format == PIXEL_FORMAT_BGR_888) {
    copyPackedRgbToBgr(vf, mapped, width, height, true, image);
  } else if (format == PIXEL_FORMAT_RGB_888) {
    copyPackedRgbToBgr(vf, mapped, width, height, false, image);
  } else if (format == PIXEL_FORMAT_BGR_888_PLANAR) {
    copyPlanarRgbToBgr(vf, mapped, width, height, true, image);
  } else if (format == PIXEL_FORMAT_RGB_888_PLANAR) {
    copyPlanarRgbToBgr(vf, mapped, width, height, false, image);
  } else if (format == PIXEL_FORMAT_YUV_400) {
    cv::Mat gray(height, width, CV_8UC1);
    for (int y = 0; y < height; ++y) {
      std::memcpy(gray.ptr(y), mapped + y * vf.u32Stride[0], width);
    }
    cv::cvtColor(gray, *image, cv::COLOR_GRAY2BGR);
  } else if (format == PIXEL_FORMAT_NV12 || format == PIXEL_FORMAT_NV21) {
    cv::Mat yuv(height + height / 2, width, CV_8UC1);
    const unsigned char *y_base = mapped;
    const unsigned char *uv_base = mapped + vf.u32Length[0];
    for (int y = 0; y < height; ++y) {
      std::memcpy(yuv.ptr(y), y_base + y * vf.u32Stride[0], width);
    }
    for (int y = 0; y < height / 2; ++y) {
      std::memcpy(yuv.ptr(height + y), uv_base + y * vf.u32Stride[1], width);
    }
    const int code = format == PIXEL_FORMAT_NV21 ? cv::COLOR_YUV2BGR_NV21
                                                 : cv::COLOR_YUV2BGR_NV12;
    cv::cvtColor(yuv, *image, code);
  } else {
    ok = false;
    setError(error,
             "unsupported frame pixel format "
             "(supported: RGB/BGR packed+planar, NV12/NV21, YUV400)");
  }
  const double copy_ms = elapsedMs(copy_begin);

  const auto unmap_begin = std::chrono::steady_clock::now();
  CVI_SYS_Munmap(mapped, map_size);
  const double unmap_ms = elapsedMs(unmap_begin);
  if (profileEnabled()) {
    std::fprintf(stderr,
                 "[profile] frameToBgrMat: fmt=%d size=%dx%d map=%zu "
                 "mmap=%.3f cache=%.3f copy_color=%.3f unmap=%.3f total=%.3f\n",
                 format, width, height, map_size, mmap_ms, cache_ms, copy_ms,
                 unmap_ms, elapsedMs(total_begin));
  }
  if (!ok || image->empty()) {
    if (ok) {
      setError(error, "failed to convert frame to BGR image");
    }
    return false;
  }
  return true;
}

inline bool frameToBgrMat(const Frame &frame, VpssBgrConverter *converter,
                          cv::Mat *image, std::string *error) {
  if (!converter) {
    setError(error, "VPSS BGR converter is null");
    return false;
  }
  return converter->convert(frame, image, error);
}

// Explicit CPU fallback for non-MMF callers.  Online algorithms should use
// the overload above with a long-lived VpssBgrConverter.
inline bool cpuFrameToBgrMat(const Frame &frame, cv::Mat *image,
                             std::string *error) {
  if (!frame.native) {
    setError(error, "frame has no native VIDEO_FRAME_INFO_S buffer");
    return false;
  }
  const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
  return videoFrameToBgrMat(*video, image, error);
}

}  // namespace frame_convert
}  // namespace tdl_app
