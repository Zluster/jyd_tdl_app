#pragma once

// Shared VPSS/VI frame -> BGR cv::Mat conversion for the CPU-preprocess NN
// runtimes. Every runtime used to carry its own copy of this code; new
// runtimes must use this header instead of duplicating it.

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "cvi_comm_video.h"
#include "cvi_sys.h"
#include "tdl_app/frame_source.hpp"

namespace tdl_app {
namespace frame_convert {

inline void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

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

  auto *mapped =
      static_cast<unsigned char *>(CVI_SYS_Mmap(vf.u64PhyAddr[0], map_size));
  if (!mapped) {
    setError(error, "CVI_SYS_Mmap failed");
    return false;
  }
  CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], mapped, map_size);

  bool ok = true;
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

  CVI_SYS_Munmap(mapped, map_size);
  if (!ok || image->empty()) {
    if (ok) {
      setError(error, "failed to convert frame to BGR image");
    }
    return false;
  }
  return true;
}

inline bool frameToBgrMat(const Frame &frame, cv::Mat *image,
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
