#pragma once

#include <cstdlib>
#include <string>

#include <opencv2/core.hpp>

#include "tdl_app/advanced.hpp"

namespace camera_demo_support {

struct CommonOptions {
  std::string backend = "vpss";
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
};

struct CameraRuntime {
  tdl_app::Camera camera;
};

bool setCameraPreset(CommonOptions *opt, const std::string &preset,
                     std::string *error = nullptr);
std::string describeCameraPreset(const CommonOptions &opt);
const char *backendName(tdl_app::Camera::Backend backend);
bool parseCommonArgs(int argc, char **argv, int *index, CommonOptions *opt,
                     bool *handled, std::string *error = nullptr);
tdl_app::Camera::Config makeCameraConfig(const CommonOptions &opt);
bool openCameraRuntime(const CommonOptions &opt, CameraRuntime *runtime,
                       std::string *error = nullptr);
void closeCameraRuntime(CameraRuntime *runtime);
tdl_app::MediaChannel previewChannel(const CommonOptions &opt,
                                     const tdl_app::Camera::Config &camera_config);

std::string frameOutputPath(const std::string &output, int index);
// Maps the frame's native VIDEO_FRAME_INFO_S buffer and converts it to a BGR
// cv::Mat. Supports NV21/NV12/YUV400/RGB888/BGR888/RGB888_PLANAR/BGR888_PLANAR,
// the same set saveFrameAsImage() supports (it is built on top of this).
bool frameToBgrMat(const tdl_app::Frame &frame, cv::Mat *out_bgr,
                   std::string *error = nullptr);
bool saveFrameAsImage(const tdl_app::Frame &frame, const std::string &output_path,
                      std::string *error = nullptr);
void dumpCameraDiagnostics();

}  // namespace camera_demo_support
