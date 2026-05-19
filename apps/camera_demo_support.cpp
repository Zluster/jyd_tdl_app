#include "camera_demo_support.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "cvi_comm_video.h"
#include "cvi_sys.h"

namespace camera_demo_support {
namespace {

void dumpProcFile(const std::string &title, const std::string &path) {
  std::ifstream input(path);
  std::cerr << title << " (" << path << ")\n";
  if (!input) {
    std::cerr << "  unavailable\n";
    return;
  }
  std::string line;
  while (std::getline(input, line)) {
    std::cerr << line << "\n";
  }
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
  } else if (arg == "--use-mmf") {
    opt->use_mmf = true;
  } else if (arg == "--use-sensor-media") {
    opt->use_sensor_media = true;
  } else if (arg == "--use-ipcamera-helper") {
    opt->use_sensor_media = true;
    opt->use_ipcamera_helper = true;
  } else if (arg == "--attach-existing") {
    opt->attach_existing = true;
  } else if (arg == "--sensor-ini") {
    const char *v = valueForArg(argc, argv, index, "--sensor-ini", error);
    if (!v) return false;
    opt->sensor_ini = v;
  } else if (arg == "--ipcamera-bin") {
    const char *v = valueForArg(argc, argv, index, "--ipcamera-bin", error);
    if (!v) return false;
    opt->ipcamera_binary = v;
  } else if (arg == "--ipcamera-ini") {
    const char *v = valueForArg(argc, argv, index, "--ipcamera-ini", error);
    if (!v) return false;
    opt->ipcamera_ini = v;
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
  tdl_app::Camera::Config camera_config =
      opt.backend == "vi"
          ? tdl_app::Camera::vi(opt.pipe, opt.channel, opt.width, opt.height,
                                opt.pixel_format, opt.timeout_ms)
          : tdl_app::Camera::vpss(opt.group, opt.channel, opt.width, opt.height,
                                  opt.pixel_format, opt.timeout_ms);
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

  runtime->mmf.reset();
  runtime->sensor_media.reset();

  if (opt.use_sensor_media) {
    const bool use_vpss_backend = opt.backend != "vi";
    tdl_app::SensorMedia::Config sensor_config =
        opt.use_ipcamera_helper
            ? tdl_app::SensorMedia::ipcameraHelper(
                  opt.sensor_ini, opt.ipcamera_binary, opt.ipcamera_ini,
                  use_vpss_backend, opt.device, opt.pipe, opt.channel,
                  opt.group, opt.channel, opt.width, opt.height,
                  opt.pixel_format)
            : (opt.attach_existing
                   ? tdl_app::SensorMedia::attachExisting(
                         opt.sensor_ini, use_vpss_backend, opt.device, opt.pipe,
                         opt.channel, opt.group, opt.channel, opt.width,
                         opt.height, opt.pixel_format)
                   : tdl_app::SensorMedia::fullStackSensor(
                         opt.sensor_ini, use_vpss_backend, opt.device, opt.pipe,
                         opt.channel, opt.group, opt.channel, opt.width,
                         opt.height, opt.pixel_format));
    runtime->sensor_media.reset(new tdl_app::SensorMedia(sensor_config));
    if (!runtime->sensor_media->open(error)) {
      return false;
    }

    tdl_app::Camera::Config camera_config = makeCameraConfig(opt);
    if (!opt.use_ipcamera_helper && !opt.attach_existing &&
        opt.backend == "vi") {
      std::cerr
          << "camera runtime: sensor-media full-stack uses online VPSS; "
             "reading frames from VPSS grp="
          << opt.group << " ch=" << opt.channel << "\n";
      camera_config =
          tdl_app::Camera::vpss(opt.group, opt.channel, opt.width, opt.height,
                                opt.pixel_format, opt.timeout_ms);
      camera_config.device = opt.device;
    }
    runtime->camera = tdl_app::Camera(camera_config);
    return runtime->camera.open(error);
  } else if (opt.use_mmf) {
    tdl_app::Mmf::Config mmf_config;
    mmf_config.pool.width = opt.width;
    mmf_config.pool.height = opt.height;
    mmf_config.pool.pixel_format = opt.pixel_format;
    mmf_config.vpss.enable = true;
    mmf_config.vpss.group = opt.group;
    mmf_config.vpss.channel = opt.channel;
    mmf_config.vpss.input_width = opt.width;
    mmf_config.vpss.input_height = opt.height;
    mmf_config.vpss.output_width = opt.width;
    mmf_config.vpss.output_height = opt.height;
    mmf_config.vpss.pixel_format = opt.pixel_format;
    mmf_config.bind =
        tdl_app::Mmf::viToVpss(opt.pipe, opt.channel, opt.group, opt.channel);
    runtime->mmf.reset(new tdl_app::Mmf(mmf_config));
    if (!runtime->mmf->open(error)) {
      return false;
    }
  }

  runtime->camera = tdl_app::Camera(makeCameraConfig(opt));
  return runtime->camera.open(error);
}

void closeCameraRuntime(CameraRuntime *runtime) {
  if (!runtime) {
    return;
  }
  runtime->camera.close();
  if (runtime->sensor_media) {
    runtime->sensor_media->close();
  }
  if (runtime->mmf) {
    runtime->mmf->close();
  }
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

bool saveFrameAsImage(const tdl_app::Frame &frame, const std::string &output_path,
                      std::string *error) {
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
      format != PIXEL_FORMAT_YUV_400) {
    if (error) {
      *error = "snapshot only supports NV21/NV12/YUV400, format=" +
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

  bool ok = false;
  cv::Mat image;
  do {
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

  if (!image.empty()) {
    ok = cv::imwrite(output_path, image);
  }
  CVI_SYS_Munmap(mapped, map_size);

  if (!ok && error) {
    *error = "failed to write image: " + output_path;
  }
  return ok;
}

void dumpCameraDiagnostics() {
  dumpProcFile("demo: /proc/mipi-rx", "/proc/mipi-rx");
  dumpProcFile("demo: /proc/soph/vi", "/proc/soph/vi");
  dumpProcFile("demo: /proc/soph/vi_dbg", "/proc/soph/vi_dbg");
  dumpProcFile("demo: /proc/soph/vpss", "/proc/soph/vpss");
}

}  // namespace camera_demo_support
