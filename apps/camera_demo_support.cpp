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

std::string trim(const std::string &value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         (value[begin] == ' ' || value[begin] == '\t' ||
          value[begin] == '\r' || value[begin] == '\n')) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' ||
          value[end - 1] == '\r' || value[end - 1] == '\n')) {
    --end;
  }
  return value.substr(begin, end - begin);
}

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
  } else if (arg == "--use-mmf") {
    opt->use_mmf = true;
    opt->use_sensor_media = false;
  } else if (arg == "--use-sensor-media") {
    opt->use_sensor_media = true;
    opt->use_mmf = false;
  } else if (arg == "--use-ipcamera-helper") {
    opt->use_sensor_media = true;
    opt->use_mmf = false;
    opt->use_ipcamera_helper = true;
  } else if (arg == "--attach-existing") {
    opt->attach_existing = true;
  } else if (arg == "--sensor-ini") {
    const char *v = valueForArg(argc, argv, index, "--sensor-ini", error);
    if (!v) return false;
    opt->sensor_ini = v;
  } else if (arg == "--sensor-model") {
    const char *v = valueForArg(argc, argv, index, "--sensor-model", error);
    if (!v) return false;
    opt->sensor_model = v;
  } else if (arg == "--sensor-profile") {
    const char *v = valueForArg(argc, argv, index, "--sensor-profile", error);
    if (!v) return false;
    opt->sensor_profile = v;
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

std::vector<tdl_app::SensorMedia::SensorProfile> supportedSensorProfiles() {
  std::vector<tdl_app::SensorMedia::SensorProfile> profiles;
  profiles.push_back(tdl_app::SensorMedia::cv2003Profile());
  profiles.push_back(tdl_app::SensorMedia::cv2003OneLaneProfile());
  profiles.push_back(tdl_app::SensorMedia::gc2053Profile());
  profiles.push_back(tdl_app::SensorMedia::gc2053TwoLaneProfile());
  profiles.push_back(tdl_app::SensorMedia::gc2053OneLaneProfile());
  profiles.push_back(tdl_app::SensorMedia::gc2093Profile());
  return profiles;
}

const char *sensorProfileForModel(const std::string &model) {
  const std::string lower = toLower(model);
  if (lower == "cv2003" || lower == "cv2003_2lane") {
    return "sensor_cfg_cv2003.ini";
  }
  if (lower == "cv2003_1lane" || lower == "cv2003_1l") {
    return "sensor_cfg_cv2003_1lane.ini";
  }
  if (lower == "gc2053") {
    return "sensor_cfg_gc2053.ini";
  }
  if (lower == "gc2053_2lane") {
    return "sensor_cfg_gc2053_2lane.ini";
  }
  if (lower == "gc2053_1lane" || lower == "gc2053_1l" ||
      lower == "gc2053_slave") {
    return "sensor_cfg_gc2053_1lane.ini";
  }
  if (lower == "gc2093") {
    return "sensor_cfg_gc2093.ini";
  }
  return nullptr;
}

bool resolveSensorIni(CommonOptions *opt, std::string *error) {
  if (!opt) {
    if (error) {
      *error = "resolveSensorIni received null options";
    }
    return false;
  }

  if (!opt->sensor_ini.empty()) {
    return true;
  }

  if (opt->sensor_profile.empty() && !opt->sensor_model.empty()) {
    const char *inferred = sensorProfileForModel(opt->sensor_model);
    if (inferred != nullptr) {
      opt->sensor_profile = inferred;
    }
  }

  if (!opt->sensor_profile.empty()) {
    opt->sensor_ini = opt->sensor_profile;
    return true;
  }

  if (!opt->sensor_model.empty()) {
    const char *profile = sensorProfileForModel(opt->sensor_model);
    if (profile == nullptr) {
      if (error) {
        *error = "unknown sensor_model: " + opt->sensor_model;
      }
      return false;
    }
    opt->sensor_profile = profile;
    opt->sensor_ini = opt->sensor_profile;
    return true;
  }

  opt->sensor_model = "cv2003";
  opt->sensor_profile = "sensor_cfg_cv2003.ini";
  opt->sensor_ini = opt->sensor_profile;
  if (error) {
    error->clear();
  }
  return true;
}

std::string describeSensorSelection(const CommonOptions &opt) {
  std::ostringstream oss;
  oss << "sensor_ini=" << opt.sensor_ini;
  if (!opt.sensor_model.empty()) {
    oss << " sensor_model=" << opt.sensor_model;
  }
  if (!opt.sensor_profile.empty()) {
    oss << " sensor_profile=" << opt.sensor_profile;
  }
  return oss.str();
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

  runtime->mmf.reset();
  runtime->sensor_media.reset();

  CommonOptions resolved = opt;

  if (resolved.use_sensor_media) {
    if (!resolveSensorIni(&resolved, error)) {
      return false;
    }

    std::cerr << "camera runtime: " << describeSensorSelection(resolved) << "\n";
    if (!resolved.sensor_model.empty() &&
        (toLower(resolved.sensor_model) == "gc2053" ||
         toLower(resolved.sensor_model) == "gc2053_2lane" ||
         toLower(resolved.sensor_model) == "gc2053_1lane" ||
         toLower(resolved.sensor_model) == "gc2053_1l" ||
         toLower(resolved.sensor_model) == "gc2053_slave")) {
      std::cerr
          << "camera runtime: gc2053 selected; make sure libsns_gc2053.so is "
             "built and packaged into the runtime image\n";
    }

    const bool use_vpss_backend = resolved.backend != "vi";
    tdl_app::SensorMedia::Config sensor_config =
        resolved.use_ipcamera_helper
            ? tdl_app::SensorMedia::ipcameraHelper(
                  resolved.sensor_ini, resolved.ipcamera_binary,
                  resolved.ipcamera_ini, use_vpss_backend, resolved.device,
                  resolved.pipe, resolved.channel, resolved.group,
                  resolved.channel, resolved.width, resolved.height,
                  resolved.pixel_format)
            : (resolved.attach_existing
                   ? tdl_app::SensorMedia::attachExisting(
                         resolved.sensor_ini, use_vpss_backend,
                         resolved.device, resolved.pipe, resolved.channel,
                         resolved.group, resolved.channel, resolved.width,
                         resolved.height, resolved.pixel_format)
                   : tdl_app::SensorMedia::fullStackSensor(
                         resolved.sensor_ini, use_vpss_backend,
                         resolved.device, resolved.pipe, resolved.channel,
                         resolved.group, resolved.channel, resolved.width,
                         resolved.height, resolved.pixel_format));
    if (resolved.enable_preview_output &&
        resolved.preview_width > 0 &&
        resolved.preview_height > 0) {
      sensor_config.vpss_outputs.push_back(tdl_app::SensorMedia::vpssOutput(
          resolved.preview_group, resolved.preview_channel,
          resolved.preview_width, resolved.preview_height,
          resolved.preview_pixel_format, false));
    }
    if (resolved.enable_pip_output &&
        resolved.pip_width > 0 &&
        resolved.pip_height > 0) {
      sensor_config.vpss_outputs.push_back(tdl_app::SensorMedia::vpssOutput(
          resolved.pip_group, resolved.pip_channel, resolved.pip_width,
          resolved.pip_height, resolved.pip_pixel_format, false));
    }
    runtime->sensor_media.reset(new tdl_app::SensorMedia(sensor_config));
    if (!runtime->sensor_media->open(error)) {
      return false;
    }

    tdl_app::Camera::Config camera_config = makeCameraConfig(resolved);
    if (!resolved.use_ipcamera_helper && !resolved.attach_existing &&
        resolved.backend == "vi") {
      std::cerr
          << "camera runtime: sensor-media full-stack uses online VPSS; "
             "reading frames from VPSS grp="
          << resolved.group << " ch=" << resolved.channel << "\n";
      camera_config =
          tdl_app::Camera::vpss(resolved.group, resolved.channel,
                                resolved.width, resolved.height,
                                resolved.pixel_format, resolved.timeout_ms);
      camera_config.device = resolved.device;
    }
    runtime->camera = tdl_app::Camera(camera_config);
    return runtime->camera.open(error);
  } else if (resolved.use_mmf) {
    tdl_app::Mmf::Config mmf_config;
    mmf_config.pool.width = resolved.width;
    mmf_config.pool.height = resolved.height;
    mmf_config.pool.pixel_format = resolved.pixel_format;
    mmf_config.vpss.enable = true;
    mmf_config.vpss.group = resolved.group;
    mmf_config.vpss.channel = resolved.channel;
    mmf_config.vpss.input_width = resolved.width;
    mmf_config.vpss.input_height = resolved.height;
    mmf_config.vpss.output_width = resolved.width;
    mmf_config.vpss.output_height = resolved.height;
    mmf_config.vpss.pixel_format = resolved.pixel_format;
    mmf_config.bind =
        tdl_app::Mmf::viToVpss(resolved.pipe, resolved.channel,
                               resolved.group, resolved.channel);
    runtime->mmf.reset(new tdl_app::Mmf(mmf_config));
    if (!runtime->mmf->open(error)) {
      return false;
    }
  }

  runtime->camera = tdl_app::Camera(makeCameraConfig(resolved));
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

tdl_app::MediaChannel previewChannel(const CommonOptions &opt,
                                     const tdl_app::Camera::Config &camera_config) {
  if (opt.enable_preview_output && opt.preview_width > 0 && opt.preview_height > 0) {
    return tdl_app::MediaChannel::vpss(opt.preview_group, opt.preview_channel);
  }
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

  bool ok = false;
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




