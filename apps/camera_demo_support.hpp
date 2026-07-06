#pragma once

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "tdl_app/advanced.hpp"

namespace camera_demo_support {

struct CommonOptions {
  std::string backend = "vpss";
  bool use_mmf = false;
  bool use_sensor_media = false;
  bool use_ipcamera_helper = false;
  bool attach_existing = true;
  std::string sensor_ini;
  std::string sensor_model;
  std::string sensor_profile;
  std::string ipcamera_binary = "/mnt/sd/install/ipcamera";
  std::string ipcamera_ini = "/mnt/sd/install/cv1842hp_wevb_cv2003.ini";
  int frames = 1;
  int device = 0;
  int group = tdl_app::DualOsLayout::kCaptureVpssGroup;
  int pipe = 0;
  int channel = tdl_app::DualOsLayout::kAiChannel;
  int pixel_format = tdl_app::DualOsLayout::kAiPixelFormat;
  int width = tdl_app::DualOsLayout::kAiWidth;
  int height = tdl_app::DualOsLayout::kAiHeight;
  int timeout_ms = 1000;
  int hold_ms = 0;
  bool enable_preview_output = false;
  int preview_group = tdl_app::DualOsLayout::kCaptureVpssGroup;
  int preview_channel = tdl_app::DualOsLayout::kLiveChannel;
  int preview_width = 0;
  int preview_height = 0;
  int preview_pixel_format = tdl_app::DualOsLayout::kLivePixelFormat;
  bool enable_pip_output = false;
  int pip_group = tdl_app::DualOsLayout::kCaptureVpssGroup;
  int pip_channel = tdl_app::DualOsLayout::kSubRgbChannel;
  int pip_width = 0;
  int pip_height = 0;
  int pip_pixel_format = tdl_app::DualOsLayout::kSubRgbPixelFormat;
};

struct CameraRuntime {
  std::unique_ptr<tdl_app::Mmf> mmf;
  std::unique_ptr<tdl_app::SensorMedia> sensor_media;
  tdl_app::Camera camera;
};

bool setCameraPreset(CommonOptions *opt, const std::string &preset,
                     std::string *error = nullptr);
std::string describeCameraPreset(const CommonOptions &opt);
const char *backendName(tdl_app::Camera::Backend backend);
bool parseCommonArgs(int argc, char **argv, int *index, CommonOptions *opt,
                     bool *handled, std::string *error = nullptr);
bool resolveSensorIni(CommonOptions *opt, std::string *error = nullptr);
std::vector<tdl_app::SensorMedia::SensorProfile> supportedSensorProfiles();
std::string describeSensorSelection(const CommonOptions &opt);
tdl_app::Camera::Config makeCameraConfig(const CommonOptions &opt);
bool openCameraRuntime(const CommonOptions &opt, CameraRuntime *runtime,
                       std::string *error = nullptr);
void closeCameraRuntime(CameraRuntime *runtime);
tdl_app::MediaChannel previewChannel(const CommonOptions &opt,
                                     const tdl_app::Camera::Config &camera_config);

std::string frameOutputPath(const std::string &output, int index);
bool saveFrameAsImage(const tdl_app::Frame &frame, const std::string &output_path,
                      std::string *error = nullptr);
void dumpCameraDiagnostics();

}  // namespace camera_demo_support

