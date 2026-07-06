#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "cvi_errno.h"
#include "cvi_sys.h"
#include "cvi_vpss.h"
#include "camera_demo_support.hpp"
#include "tdl_app/advanced.hpp"

namespace {

struct ProbeTarget {
  int pixel_format = PIXEL_FORMAT_NV12;
  const char *name = "NV12";
};

struct Options {
  camera_demo_support::CommonOptions camera;
  int group = tdl_app::DualOsLayout::kCaptureVpssGroup;
  int channel = tdl_app::DualOsLayout::kLiveChannel;
  int width = tdl_app::DualOsLayout::kLiveWidth;
  int height = tdl_app::DualOsLayout::kLiveHeight;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_vpss_format_probe_demo [--use-sensor-media] [--attach-existing]\n"
      << "                             [--sensor-model NAME] [--sensor-profile FILE]\n"
      << "                             [--sensor-ini FILE]\n"
      << "                             [--device N] [--group N] [--pipe N] [--channel N]\n"
      << "                             [--group N] [--channel N]\n"
      << "                             [--width N] [--height N]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    bool handled = false;
    std::string parse_error;
    if (!camera_demo_support::parseCommonArgs(argc, argv, &i, &opt->camera,
                                              &handled, &parse_error)) {
      std::cerr << parse_error << "\n";
      return false;
    }
    if (handled) {
      continue;
    }
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--group") {
      const char *v = value("--group");
      if (!v) return false;
      opt->group = std::atoi(v);
    } else if (arg == "--channel") {
      const char *v = value("--channel");
      if (!v) return false;
      opt->channel = std::atoi(v);
    } else if (arg == "--width") {
      const char *v = value("--width");
      if (!v) return false;
      opt->width = std::atoi(v);
    } else if (arg == "--height") {
      const char *v = value("--height");
      if (!v) return false;
      opt->height = std::atoi(v);
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  return true;
}

std::string decodeVpssError(int ret) {
  switch (ret) {
    case CVI_SUCCESS:
      return "CVI_SUCCESS";
    case CVI_ERR_VPSS_NULL_PTR:
      return "CVI_ERR_VPSS_NULL_PTR";
    case CVI_ERR_VPSS_NOTREADY:
      return "CVI_ERR_VPSS_NOTREADY";
    case CVI_ERR_VPSS_INVALID_DEVID:
      return "CVI_ERR_VPSS_INVALID_DEVID";
    case CVI_ERR_VPSS_INVALID_CHNID:
      return "CVI_ERR_VPSS_INVALID_CHNID";
    case CVI_ERR_VPSS_EXIST:
      return "CVI_ERR_VPSS_EXIST";
    case CVI_ERR_VPSS_UNEXIST:
      return "CVI_ERR_VPSS_UNEXIST";
    case CVI_ERR_VPSS_NOT_SUPPORT:
      return "CVI_ERR_VPSS_NOT_SUPPORT";
    case CVI_ERR_VPSS_NOT_PERM:
      return "CVI_ERR_VPSS_NOT_PERM";
    case CVI_ERR_VPSS_NOMEM:
      return "CVI_ERR_VPSS_NOMEM";
    case CVI_ERR_VPSS_NOBUF:
      return "CVI_ERR_VPSS_NOBUF";
    case CVI_ERR_VPSS_ILLEGAL_PARAM:
      return "CVI_ERR_VPSS_ILLEGAL_PARAM";
    case CVI_ERR_VPSS_BUSY:
      return "CVI_ERR_VPSS_BUSY";
    case CVI_ERR_VPSS_BUF_EMPTY:
      return "CVI_ERR_VPSS_BUF_EMPTY";
    default:
      break;
  }
  return std::to_string(ret);
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::vector<ProbeTarget> targets = {
      {PIXEL_FORMAT_NV12, "NV12"},
      {PIXEL_FORMAT_RGB_888, "RGB888"},
      {PIXEL_FORMAT_ARGB_8888, "ARGB8888"},
  };

  if (opt.camera.sensor_ini.empty() && opt.camera.sensor_model.empty() &&
      opt.camera.sensor_profile.empty()) {
    opt.camera.sensor_model = "cv2003";
  }
  opt.camera.use_sensor_media = true;
  opt.camera.group = opt.group;
  opt.camera.channel = opt.channel;
  opt.camera.width = opt.width;
  opt.camera.height = opt.height;
  opt.camera.pixel_format = PIXEL_FORMAT_NV12;
  opt.camera.enable_preview_output = false;
  opt.camera.enable_pip_output = false;

  camera_demo_support::CameraRuntime runtime;
  std::string error;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &runtime, &error)) {
    std::cerr << "camera runtime open failed: " << error << "\n";
    return 2;
  }

  std::cout << "probe begin: group=" << opt.group
            << " channel=" << opt.channel
            << " size=" << opt.width << "x" << opt.height << "\n";

  for (const auto &target : targets) {
    VPSS_CHN_ATTR_S chn_attr;
    std::memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.u32Width = static_cast<CVI_U32>(opt.width);
    chn_attr.u32Height = static_cast<CVI_U32>(opt.height);
    chn_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
    chn_attr.enPixelFormat = static_cast<PIXEL_FORMAT_E>(target.pixel_format);
    chn_attr.stFrameRate.s32SrcFrameRate = -1;
    chn_attr.stFrameRate.s32DstFrameRate = -1;
    chn_attr.u32Depth = 1;
    chn_attr.bMirror = CVI_FALSE;
    chn_attr.bFlip = CVI_FALSE;
    chn_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
    chn_attr.stNormalize.bEnable = CVI_FALSE;

    const int ret = CVI_VPSS_SetChnAttr(opt.group, opt.channel, &chn_attr);
    std::cout << "probe fmt=" << target.name
              << "(" << target.pixel_format << ")"
              << " ret=" << ret
              << " " << decodeVpssError(ret) << "\n";
    if (ret == CVI_SUCCESS) {
      CVI_VPSS_EnableChn(opt.group, opt.channel);
      CVI_VPSS_DisableChn(opt.group, opt.channel);
    }
  }

  camera_demo_support::closeCameraRuntime(&runtime);
  return 0;
}
