#pragma once

#include <cstdlib>
#include <memory>
#include <string>

#include "tdl_app/advanced.hpp"

namespace camera_demo_support {

struct CommonOptions {
  std::string backend = "vpss";
  bool use_mmf = false;
  bool use_sensor_media = false;
  bool use_ipcamera_helper = false;
  bool attach_existing = false;
  std::string sensor_ini = "./configs/sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini";
  std::string ipcamera_binary = "/mnt/sd/install/ipcamera";
  std::string ipcamera_ini = "/mnt/sd/install/cv1842hp_wevb_cv2003.ini";
  int frames = 1;
  int device = 0;
  int group = 0;
  int pipe = 0;
  int channel = 0;
  int pixel_format = 18;
  int width = 640;
  int height = 640;
  int timeout_ms = 1000;
  int hold_ms = 0;
};

struct CameraRuntime {
  std::unique_ptr<tdl_app::Mmf> mmf;
  std::unique_ptr<tdl_app::SensorMedia> sensor_media;
  tdl_app::Camera camera;
};

const char *backendName(tdl_app::Camera::Backend backend);
bool parseCommonArgs(int argc, char **argv, int *index, CommonOptions *opt,
                     bool *handled, std::string *error = nullptr);
tdl_app::Camera::Config makeCameraConfig(const CommonOptions &opt);
bool openCameraRuntime(const CommonOptions &opt, CameraRuntime *runtime,
                       std::string *error = nullptr);
void closeCameraRuntime(CameraRuntime *runtime);

std::string frameOutputPath(const std::string &output, int index);
bool saveFrameAsImage(const tdl_app::Frame &frame, const std::string &output_path,
                      std::string *error = nullptr);
void dumpCameraDiagnostics();

}  // namespace camera_demo_support
