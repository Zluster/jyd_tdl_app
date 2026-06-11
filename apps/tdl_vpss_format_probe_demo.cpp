#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "cvi_errno.h"
#include "cvi_sys.h"
#include "cvi_vpss.h"
#include "tdl_app/advanced.hpp"

namespace {

struct ProbeTarget {
  int pixel_format = PIXEL_FORMAT_NV12;
  const char *name = "NV12";
};

struct Options {
  std::string sensor_ini = "./configs/sensor_cfg_cv1842hp_wevb_cv2003_ipcamera.ini";
  int vi_device = 0;
  int vi_pipe = 0;
  int vi_channel = 0;
  int group = 0;
  int channel = 2;
  int width = 320;
  int height = 240;
  bool online_vpss = true;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_vpss_format_probe_demo [--sensor-ini FILE]\n"
      << "                             [--vi-device N] [--vi-pipe N] [--vi-channel N]\n"
      << "                             [--group N] [--channel N]\n"
      << "                             [--width N] [--height N]\n"
      << "                             [--online-vpss 0|1]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--sensor-ini") {
      const char *v = value("--sensor-ini");
      if (!v) return false;
      opt->sensor_ini = v;
    } else if (arg == "--vi-device") {
      const char *v = value("--vi-device");
      if (!v) return false;
      opt->vi_device = std::atoi(v);
    } else if (arg == "--vi-pipe") {
      const char *v = value("--vi-pipe");
      if (!v) return false;
      opt->vi_pipe = std::atoi(v);
    } else if (arg == "--vi-channel") {
      const char *v = value("--vi-channel");
      if (!v) return false;
      opt->vi_channel = std::atoi(v);
    } else if (arg == "--group") {
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
    } else if (arg == "--online-vpss") {
      const char *v = value("--online-vpss");
      if (!v) return false;
      opt->online_vpss = std::atoi(v) != 0;
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

  tdl_app::SensorMedia::Config config = tdl_app::SensorMedia::fullStackSensor(
      opt.sensor_ini, true, opt.vi_device, opt.vi_pipe, opt.vi_channel, opt.group,
      0, opt.width, opt.height, PIXEL_FORMAT_NV12);
  config.online_vpss = opt.online_vpss;
  config.vpss_outputs.clear();
  config.vpss_outputs.push_back(
      tdl_app::SensorMedia::vpssOutput(opt.group, 0, opt.width, opt.height,
                                       PIXEL_FORMAT_NV12, true));

  std::string error;
  tdl_app::SensorMedia sensor(config);
  if (!sensor.open(&error)) {
    std::cerr << "sensor open failed: " << error << "\n";
    return 2;
  }

  std::cout << "probe begin: online_vpss=" << (opt.online_vpss ? 1 : 0)
            << " group=" << opt.group
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

  sensor.close();
  return 0;
}
