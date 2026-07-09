#include "camera_demo_support.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "cvi_comm_video.h"
#include "cvi_sys.h"

namespace camera_demo_support {
namespace {

std::string toLower(std::string value) {
  for (char &ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

const char *valueForArg(int argc, char **argv, int *index, const char *name,
                        std::string *error) {
  if (*index + 1 >= argc) {
    if (error) {
      *error = std::string("missing value for ") + name;
    }
    return nullptr;
  }
  return argv[++(*index)];
}

}  // namespace

const char *backendName(tdl_app::Camera::Backend backend) {
  return backend == tdl_app::Camera::Backend::Vi ? "VI" : "VPSS";
}

bool setCameraPreset(CommonOptions *opt, const std::string &preset,
                     std::string *error) {
  if (!opt) {
    if (error) {
      *error = "setCameraPreset received null options";
    }
    return false;
  }

  const std::string lower = toLower(preset);
  opt->backend = "vpss";

  if (lower == "ai") {
    opt->group = tdl_app::DualOsLayout::kCaptureVpssGroup;
    opt->channel = tdl_app::DualOsLayout::kAiChannel;
    opt->width = tdl_app::DualOsLayout::kAiWidth;
    opt->height = tdl_app::DualOsLayout::kAiHeight;
    opt->pixel_format = tdl_app::DualOsLayout::kAiPixelFormat;
    return true;
  }
  if (lower == "live") {
    opt->group = tdl_app::DualOsLayout::kCaptureVpssGroup;
    opt->channel = tdl_app::DualOsLayout::kLiveChannel;
    opt->width = tdl_app::DualOsLayout::kLiveWidth;
    opt->height = tdl_app::DualOsLayout::kLiveHeight;
    opt->pixel_format = tdl_app::DualOsLayout::kLivePixelFormat;
    return true;
  }
  if (lower == "main") {
    opt->group = tdl_app::DualOsLayout::kCaptureVpssGroup;
    opt->channel = tdl_app::DualOsLayout::kMainChannel;
    opt->width = tdl_app::DualOsLayout::kMainWidth;
    opt->height = tdl_app::DualOsLayout::kMainHeight;
    opt->pixel_format = tdl_app::DualOsLayout::kMainPixelFormat;
    return true;
  }
  if (lower == "subrgb") {
    opt->group = tdl_app::DualOsLayout::kCaptureVpssGroup;
    opt->channel = tdl_app::DualOsLayout::kSubRgbChannel;
    opt->width = tdl_app::DualOsLayout::kSubRgbWidth;
    opt->height = tdl_app::DualOsLayout::kSubRgbHeight;
    opt->pixel_format = tdl_app::DualOsLayout::kSubRgbPixelFormat;
    return true;
  }
  if (lower == "screen") {
    opt->group = tdl_app::DualOsLayout::kDisplayVpssGroup;
    opt->channel = tdl_app::DualOsLayout::kDisplayChannel;
    opt->width = tdl_app::DualOsLayout::kDisplaySourceWidth;
    opt->height = tdl_app::DualOsLayout::kDisplaySourceHeight;
    opt->pixel_format = tdl_app::DualOsLayout::kDisplayPixelFormat;
    return true;
  }

  if (error) {
    *error = "unknown camera preset: " + preset +
             " (expected ai|live|main|subrgb|screen)";
  }
  return false;
}

std::string describeCameraPreset(const CommonOptions &opt) {
  if (opt.backend == "vpss" &&
      opt.group == tdl_app::DualOsLayout::kCaptureVpssGroup) {
    if (opt.channel == tdl_app::DualOsLayout::kAiChannel &&
        opt.width == tdl_app::DualOsLayout::kAiWidth &&
        opt.height == tdl_app::DualOsLayout::kAiHeight) {
      return "ai";
    }
    if (opt.channel == tdl_app::DualOsLayout::kLiveChannel &&
        opt.width == tdl_app::DualOsLayout::kLiveWidth &&
        opt.height == tdl_app::DualOsLayout::kLiveHeight) {
      return "live";
    }
    if (opt.channel == tdl_app::DualOsLayout::kMainChannel &&
        opt.width == tdl_app::DualOsLayout::kMainWidth &&
        opt.height == tdl_app::DualOsLayout::kMainHeight) {
      return "main";
    }
    if (opt.channel == tdl_app::DualOsLayout::kSubRgbChannel &&
        opt.width == tdl_app::DualOsLayout::kSubRgbWidth &&
        opt.height == tdl_app::DualOsLayout::kSubRgbHeight) {
      return "subrgb";
    }
  }
  if (opt.backend == "vpss" &&
      opt.group == tdl_app::DualOsLayout::kDisplayVpssGroup &&
      opt.channel == tdl_app::DualOsLayout::kDisplayChannel &&
      opt.width == tdl_app::DualOsLayout::kDisplaySourceWidth &&
      opt.height == tdl_app::DualOsLayout::kDisplaySourceHeight) {
    return "screen";
  }

  std::ostringstream oss;
  oss << opt.backend << " grp=" << opt.group
      << " ch=" << opt.channel
      << " " << opt.width << "x" << opt.height
      << " fmt=" << opt.pixel_format;
  return oss.str();
}

bool parseCommonArgs(int argc, char **argv, int *index, CommonOptions *opt,
                     bool *handled, std::string *error) {
  if (!index || !opt || !handled) {
    if (error) {
      *error = "parseCommonArgs received null pointer";
    }
    return false;
  }

  *handled = true;
  const std::string arg = argv[*index];

  if (arg == "--backend") {
    const char *v = valueForArg(argc, argv, index, "--backend", error);
    if (!v) return false;
    opt->backend = v;
  } else if (arg == "--frames") {
    const char *v = valueForArg(argc, argv, index, "--frames", error);
    if (!v) return false;
    opt->frames = std::atoi(v);
  } else if (arg == "--device") {
    const char *v = valueForArg(argc, argv, index, "--device", error);
    if (!v) return false;
    opt->device = std::atoi(v);
  } else if (arg == "--group") {
    const char *v = valueForArg(argc, argv, index, "--group", error);
    if (!v) return false;
    opt->group = std::atoi(v);
  } else if (arg == "--pipe") {
    const char *v = valueForArg(argc, argv, index, "--pipe", error);
    if (!v) return false;
    opt->pipe = std::atoi(v);
  } else if (arg == "--channel") {
    const char *v = valueForArg(argc, argv, index, "--channel", error);
    if (!v) return false;
    opt->channel = std::atoi(v);
  } else if (arg == "--pixel-format") {
    const char *v = valueForArg(argc, argv, index, "--pixel-format", error);
    if (!v) return false;
    opt->pixel_format = std::atoi(v);
  } else if (arg == "--width") {
    const char *v = valueForArg(argc, argv, index, "--width", error);
    if (!v) return false;
    opt->width = std::atoi(v);
  } else if (arg == "--height") {
    const char *v = valueForArg(argc, argv, index, "--height", error);
    if (!v) return false;
    opt->height = std::atoi(v);
  } else if (arg == "--timeout-ms") {
    const char *v = valueForArg(argc, argv, index, "--timeout-ms", error);
    if (!v) return false;
    opt->timeout_ms = std::atoi(v);
  } else if (arg == "--hold-ms") {
    const char *v = valueForArg(argc, argv, index, "--hold-ms", error);
    if (!v) return false;
    opt->hold_ms = std::atoi(v);
  } else {
    *handled = false;
  }

  return true;
}

tdl_app::Camera::Config makeCameraConfig(const CommonOptions &opt) {
  tdl_app::Camera::Config camera_config;
  if (opt.backend == "vi") {
    camera_config = tdl_app::Camera::vi(opt.pipe, opt.channel, opt.width,
                                        opt.height, opt.pixel_format,
                                        opt.timeout_ms);
  } else if (opt.group == tdl_app::DualOsLayout::kCaptureVpssGroup &&
             opt.channel == tdl_app::DualOsLayout::kAiChannel &&
             opt.width == tdl_app::DualOsLayout::kAiWidth &&
             opt.height == tdl_app::DualOsLayout::kAiHeight) {
    camera_config = tdl_app::Camera::ai(opt.timeout_ms);
  } else if (opt.group == tdl_app::DualOsLayout::kCaptureVpssGroup &&
             opt.channel == tdl_app::DualOsLayout::kLiveChannel &&
             opt.width == tdl_app::DualOsLayout::kLiveWidth &&
             opt.height == tdl_app::DualOsLayout::kLiveHeight) {
    camera_config = tdl_app::Camera::live(opt.timeout_ms);
  } else if (opt.group == tdl_app::DualOsLayout::kCaptureVpssGroup &&
             opt.channel == tdl_app::DualOsLayout::kMainChannel &&
             opt.width == tdl_app::DualOsLayout::kMainWidth &&
             opt.height == tdl_app::DualOsLayout::kMainHeight) {
    camera_config = tdl_app::Camera::mainFrame(opt.timeout_ms);
  } else if (opt.group == tdl_app::DualOsLayout::kCaptureVpssGroup &&
             opt.channel == tdl_app::DualOsLayout::kSubRgbChannel &&
             opt.width == tdl_app::DualOsLayout::kSubRgbWidth &&
             opt.height == tdl_app::DualOsLayout::kSubRgbHeight) {
    camera_config = tdl_app::Camera::subRgb(opt.timeout_ms);
  } else if (opt.group == tdl_app::DualOsLayout::kDisplayVpssGroup &&
             opt.channel == tdl_app::DualOsLayout::kDisplayChannel &&
             opt.width == tdl_app::DualOsLayout::kDisplaySourceWidth &&
             opt.height == tdl_app::DualOsLayout::kDisplaySourceHeight) {
    camera_config = tdl_app::Camera::screen(opt.timeout_ms);
  } else {
    camera_config =
        tdl_app::Camera::vpss(opt.group, opt.channel, opt.width, opt.height,
                              opt.pixel_format, opt.timeout_ms);
  }
  camera_config.device = opt.device;
  return camera_config;
}

bool openCameraRuntime(const CommonOptions &opt, CameraRuntime *runtime,
                       std::string *error) {
  if (!runtime) {
    if (error) {
      *error = "camera runtime pointer is null";
    }
    return false;
  }

  runtime->camera = tdl_app::Camera(makeCameraConfig(opt));
  return runtime->camera.open(error);
}

void closeCameraRuntime(CameraRuntime *runtime) {
  if (!runtime) {
    return;
  }
  runtime->camera.close();
}

tdl_app::MediaChannel previewChannel(const CommonOptions &opt,
                                     const tdl_app::Camera::Config &camera_config) {
  (void)opt;
  return camera_config.backend == tdl_app::Camera::Backend::Vi
             ? tdl_app::MediaChannel::vi(camera_config.pipe, camera_config.channel)
             : tdl_app::MediaChannel::vpss(camera_config.group, camera_config.channel);
}

std::string frameOutputPath(const std::string &output, int index) {
  if (output.empty() || index == 0) {
    return output;
  }
  const std::size_t dot = output.find_last_of('.');
  if (dot == std::string::npos || dot == 0) {
    return output + "_" + std::to_string(index);
  }
  return output.substr(0, dot) + "_" + std::to_string(index) + output.substr(dot);
}

bool frameToBgrMat(const tdl_app::Frame &frame, cv::Mat *out_bgr,
                   std::string *error) {
  if (!out_bgr) {
    if (error) {
      *error = "out_bgr is null";
    }
    return false;
  }
  if (!frame.native) {
    if (error) {
      *error = "frame has no native VIDEO_FRAME_INFO_S buffer";
    }
    return false;
  }

  auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
  const auto &vf = video->stVFrame;
  const int width = static_cast<int>(vf.u32Width);
  const int height = static_cast<int>(vf.u32Height);
  const int format = static_cast<int>(vf.enPixelFormat);

  if (width <= 0 || height <= 0) {
    if (error) {
      *error = "invalid frame size";
    }
    return false;
  }

  if (format != PIXEL_FORMAT_NV21 &&
      format != PIXEL_FORMAT_NV12 &&
      format != PIXEL_FORMAT_YUV_400 &&
      format != PIXEL_FORMAT_RGB_888 &&
      format != PIXEL_FORMAT_BGR_888 &&
      format != PIXEL_FORMAT_RGB_888_PLANAR &&
      format != PIXEL_FORMAT_BGR_888_PLANAR) {
    if (error) {
      *error =
          "snapshot only supports NV21/NV12/YUV400/RGB888/BGR888/"
          "RGB888_PLANAR/BGR888_PLANAR, format=" +
          std::to_string(format);
    }
    return false;
  }

  std::size_t map_size = 0;
  for (int i = 0; i < 3; ++i) {
    map_size += vf.u32Length[i];
  }
  if (map_size == 0) {
    if (error) {
      *error = "frame buffer length is zero";
    }
    return false;
  }

  auto *mapped =
      static_cast<unsigned char *>(CVI_SYS_Mmap(vf.u64PhyAddr[0], map_size));
  if (!mapped) {
    if (error) {
      *error = "CVI_SYS_Mmap failed";
    }
    return false;
  }
  CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], mapped, map_size);

  cv::Mat image;
  do {
    if (format == PIXEL_FORMAT_RGB_888 || format == PIXEL_FORMAT_BGR_888) {
      cv::Mat packed(height, width, CV_8UC3);
      for (int y = 0; y < height; ++y) {
        std::memcpy(packed.ptr(y), mapped + y * vf.u32Stride[0], width * 3);
      }
      if (format == PIXEL_FORMAT_RGB_888) {
        cv::cvtColor(packed, image, cv::COLOR_RGB2BGR);
      } else {
        image = packed.clone();
      }
      break;
    }

    if (format == PIXEL_FORMAT_RGB_888_PLANAR ||
        format == PIXEL_FORMAT_BGR_888_PLANAR) {
      cv::Mat plane0(height, width, CV_8UC1);
      cv::Mat plane1(height, width, CV_8UC1);
      cv::Mat plane2(height, width, CV_8UC1);
      unsigned char *base0 = mapped;
      unsigned char *base1 = mapped + vf.u32Length[0];
      unsigned char *base2 = mapped + vf.u32Length[0] + vf.u32Length[1];
      for (int y = 0; y < height; ++y) {
        std::memcpy(plane0.ptr(y), base0 + y * vf.u32Stride[0], width);
        std::memcpy(plane1.ptr(y), base1 + y * vf.u32Stride[1], width);
        std::memcpy(plane2.ptr(y), base2 + y * vf.u32Stride[2], width);
      }
      std::vector<cv::Mat> channels;
      if (format == PIXEL_FORMAT_RGB_888_PLANAR) {
        channels = {plane2, plane1, plane0};
      } else {
        channels = {plane0, plane1, plane2};
      }
      cv::merge(channels, image);
      break;
    }

    if (format == PIXEL_FORMAT_YUV_400) {
      cv::Mat gray(height, width, CV_8UC1);
      for (int y = 0; y < height; ++y) {
        std::memcpy(gray.ptr(y), mapped + y * vf.u32Stride[0], width);
      }
      cv::cvtColor(gray, image, cv::COLOR_GRAY2BGR);
      break;
    }

    cv::Mat yuv(height + height / 2, width, CV_8UC1);
    unsigned char *y_base = mapped;
    unsigned char *uv_base = mapped + vf.u32Length[0];
    for (int y = 0; y < height; ++y) {
      std::memcpy(yuv.ptr(y), y_base + y * vf.u32Stride[0], width);
    }
    for (int y = 0; y < height / 2; ++y) {
      std::memcpy(yuv.ptr(height + y), uv_base + y * vf.u32Stride[1], width);
    }

    const int code = format == PIXEL_FORMAT_NV21
                         ? cv::COLOR_YUV2BGR_NV21
                         : cv::COLOR_YUV2BGR_NV12;
    cv::cvtColor(yuv, image, code);
  } while (false);

  CVI_SYS_Munmap(mapped, map_size);

  if (image.empty()) {
    if (error) {
      *error = "frame conversion produced an empty image";
    }
    return false;
  }
  *out_bgr = std::move(image);
  return true;
}

bool saveFrameAsImage(const tdl_app::Frame &frame, const std::string &output_path,
                      std::string *error) {
  cv::Mat image;
  if (!frameToBgrMat(frame, &image, error)) {
    return false;
  }
  if (!cv::imwrite(output_path, image)) {
    if (error) {
      *error = "failed to write image: " + output_path;
    }
    return false;
  }
  return true;
}

void dumpCameraDiagnostics() {
  const char *ld_library_path = std::getenv("LD_LIBRARY_PATH");
  const char *pwd = std::getenv("PWD");
  std::cerr << "dual-os diagnostics:\n";
  std::cerr << "  cwd=" << (pwd ? pwd : "<unset>") << "\n";
  std::cerr << "  LD_LIBRARY_PATH="
            << (ld_library_path ? ld_library_path : "<unset>") << "\n";
  std::cerr << "  hint=run from the packaged runtime and source ./env.sh first,\n";
  std::cerr << "       or use ./run_camera_capture_demo.sh instead of calling\n";
  std::cerr << "       ./bin/tdl_camera_capture_demo directly\n";
}

}  // namespace camera_demo_support



