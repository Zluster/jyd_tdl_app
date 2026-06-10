#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cvi_buffer.h"
#include "cvi_errno.h"
#include "cvi_comm_sys.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_comm_vpss.h"
#include "cvi_vpss.h"
#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  int handle = 100;
  int width = 200;
  int height = 120;
  int x = 20;
  int y = 20;
  int layer = 0;
  int vo_dev = 0;
  int vo_chn = 0;
  int pixel_format = tdl_app::PixelFormat::ARGB1555;
  std::uint32_t color = 0x7FFF;
  int hold_ms = 3000;
  bool init_vo = false;
  int screen_width = 640;
  int screen_height = 960;
  int interface_type = tdl_app::VoInterfaceType::Mipi;
  int interface_sync = tdl_app::VoInterfaceSync::P480_640_60;
  std::string attach_module = "vo";
  int vpss_group = 0;
  int vpss_chn = 0;
  int feed_width = 640;
  int feed_height = 960;
  int feed_frames = 90;
  int feed_interval_ms = 33;
  int common_blocks = 3;
  std::string feed_mode = "motion";
  int osd_move_step_x = 0;
  int osd_move_step_y = 0;
  int osd_color_step = 0;
};

struct PreparedFrame {
  VIDEO_FRAME_INFO_S frame {};
  VB_BLK blk = VB_INVALID_HANDLE;
};

struct NativeVpssContext {
  int group = 0;
  int channel = 0;
  int vo_layer = 0;
  int vo_chn = 0;
  bool mode_configured = false;
  bool group_created = false;
  bool channel_enabled = false;
  bool group_started = false;
  bool vo_bound = false;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_mmf_osd_fill_demo [--handle N] [--width N] [--height N]\n"
      << "                        [--x N] [--y N] [--layer N]\n"
      << "                        [--vo-dev N] [--vo-chn N]\n"
      << "                        [--pixel-format N] [--color 0xHEX]\n"
      << "                        [--hold-ms N] [--init-vo]\n"
      << "                        [--attach-module vo|vpss]\n"
      << "                        [--vpss-group N] [--vpss-chn N]\n"
      << "                        [--feed-width N] [--feed-height N]\n"
      << "                        [--feed-frames N] [--feed-interval-ms N]\n"
      << "                        [--common-blocks N]\n"
      << "                        [--feed-mode solid|bars|grid|motion]\n"
      << "                        [--osd-move-step-x N] [--osd-move-step-y N]\n"
      << "                        [--osd-color-step N]\n"
      << "                        [--screen-width N] [--screen-height N]\n"
      << "                        [--interface-type N] [--interface-sync N]\n";
}

bool parseU32(const std::string &text, std::uint32_t *value) {
  char *end = nullptr;
  unsigned long parsed = std::strtoul(text.c_str(), &end, 0);
  if (end == text.c_str() || *end != '\0') {
    return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
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

    if (arg == "--handle") {
      const char *v = value("--handle");
      if (!v) return false;
      opt->handle = std::atoi(v);
    } else if (arg == "--width") {
      const char *v = value("--width");
      if (!v) return false;
      opt->width = std::atoi(v);
    } else if (arg == "--height") {
      const char *v = value("--height");
      if (!v) return false;
      opt->height = std::atoi(v);
    } else if (arg == "--x") {
      const char *v = value("--x");
      if (!v) return false;
      opt->x = std::atoi(v);
    } else if (arg == "--y") {
      const char *v = value("--y");
      if (!v) return false;
      opt->y = std::atoi(v);
    } else if (arg == "--layer") {
      const char *v = value("--layer");
      if (!v) return false;
      opt->layer = std::atoi(v);
    } else if (arg == "--vo-dev") {
      const char *v = value("--vo-dev");
      if (!v) return false;
      opt->vo_dev = std::atoi(v);
    } else if (arg == "--vo-chn") {
      const char *v = value("--vo-chn");
      if (!v) return false;
      opt->vo_chn = std::atoi(v);
    } else if (arg == "--pixel-format") {
      const char *v = value("--pixel-format");
      if (!v) return false;
      opt->pixel_format = std::atoi(v);
    } else if (arg == "--color") {
      const char *v = value("--color");
      if (!v) return false;
      if (!parseU32(v, &opt->color)) return false;
    } else if (arg == "--hold-ms") {
      const char *v = value("--hold-ms");
      if (!v) return false;
      opt->hold_ms = std::atoi(v);
    } else if (arg == "--attach-module") {
      const char *v = value("--attach-module");
      if (!v) return false;
      opt->attach_module = v;
    } else if (arg == "--vpss-group") {
      const char *v = value("--vpss-group");
      if (!v) return false;
      opt->vpss_group = std::atoi(v);
    } else if (arg == "--vpss-chn") {
      const char *v = value("--vpss-chn");
      if (!v) return false;
      opt->vpss_chn = std::atoi(v);
    } else if (arg == "--feed-width") {
      const char *v = value("--feed-width");
      if (!v) return false;
      opt->feed_width = std::atoi(v);
    } else if (arg == "--feed-height") {
      const char *v = value("--feed-height");
      if (!v) return false;
      opt->feed_height = std::atoi(v);
    } else if (arg == "--feed-frames") {
      const char *v = value("--feed-frames");
      if (!v) return false;
      opt->feed_frames = std::atoi(v);
    } else if (arg == "--feed-interval-ms") {
      const char *v = value("--feed-interval-ms");
      if (!v) return false;
      opt->feed_interval_ms = std::atoi(v);
    } else if (arg == "--common-blocks") {
      const char *v = value("--common-blocks");
      if (!v) return false;
      opt->common_blocks = std::atoi(v);
    } else if (arg == "--feed-mode") {
      const char *v = value("--feed-mode");
      if (!v) return false;
      opt->feed_mode = v;
    } else if (arg == "--osd-move-step-x") {
      const char *v = value("--osd-move-step-x");
      if (!v) return false;
      opt->osd_move_step_x = std::atoi(v);
    } else if (arg == "--osd-move-step-y") {
      const char *v = value("--osd-move-step-y");
      if (!v) return false;
      opt->osd_move_step_y = std::atoi(v);
    } else if (arg == "--osd-color-step") {
      const char *v = value("--osd-color-step");
      if (!v) return false;
      opt->osd_color_step = std::atoi(v);
    } else if (arg == "--screen-width") {
      const char *v = value("--screen-width");
      if (!v) return false;
      opt->screen_width = std::atoi(v);
    } else if (arg == "--screen-height") {
      const char *v = value("--screen-height");
      if (!v) return false;
      opt->screen_height = std::atoi(v);
    } else if (arg == "--interface-type") {
      const char *v = value("--interface-type");
      if (!v) return false;
      opt->interface_type = std::atoi(v);
    } else if (arg == "--interface-sync") {
      const char *v = value("--interface-sync");
      if (!v) return false;
      opt->interface_sync = std::atoi(v);
    } else if (arg == "--init-vo") {
      opt->init_vo = true;
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

void fillCanvas(const tdl_app::OsdCanvas &canvas, int pixel_format,
                std::uint32_t color) {
  if (!canvas.data || canvas.width <= 0 || canvas.height <= 0 ||
      canvas.stride <= 0) {
    return;
  }

  if (pixel_format == tdl_app::PixelFormat::ARGB8888) {
    for (int y = 0; y < canvas.height; ++y) {
      auto *row = reinterpret_cast<std::uint32_t *>(
          static_cast<std::uint8_t *>(canvas.data) + y * canvas.stride);
      for (int x = 0; x < canvas.width; ++x) {
        row[x] = color;
      }
    }
    return;
  }

  const std::uint16_t packed = static_cast<std::uint16_t>(color & 0xFFFFu);
  for (int y = 0; y < canvas.height; ++y) {
    auto *row = reinterpret_cast<std::uint16_t *>(
        static_cast<std::uint8_t *>(canvas.data) + y * canvas.stride);
    for (int x = 0; x < canvas.width; ++x) {
      row[x] = packed;
    }
  }
}

std::uint32_t animateOsdColor(const Options &opt, int frame_index) {
  if (opt.osd_color_step == 0) {
    return opt.color;
  }
  if (opt.pixel_format == tdl_app::PixelFormat::ARGB8888) {
    const std::uint32_t alpha = opt.color & 0xFF000000u;
    const std::uint32_t rgb = (opt.color + static_cast<std::uint32_t>(frame_index * opt.osd_color_step)) &
                              0x00FFFFFFu;
    return alpha | rgb;
  }
  const int next = static_cast<int>(opt.color & 0xFFFFu) + frame_index * opt.osd_color_step;
  return static_cast<std::uint32_t>(next & 0xFFFF);
}

void fillNv12Frame(VIDEO_FRAME_INFO_S *frame, int y_value, int u_value,
                   int v_value) {
  auto &vf = frame->stVFrame;
  for (CVI_U32 row = 0; row < vf.u32Height; ++row) {
    std::memset(vf.pu8VirAddr[0] + row * vf.u32Stride[0], y_value,
                vf.u32Width);
  }

  for (CVI_U32 row = 0; row < vf.u32Height / 2; ++row) {
    std::uint8_t *uv = vf.pu8VirAddr[1] + row * vf.u32Stride[1];
    for (CVI_U32 col = 0; col < vf.u32Width; col += 2) {
      uv[col] = static_cast<std::uint8_t>(u_value);
      uv[col + 1] = static_cast<std::uint8_t>(v_value);
    }
  }
}

void fillBarsFrame(VIDEO_FRAME_INFO_S *frame, int frame_index) {
  static const int kColors[8][3] = {
      {235, 128, 128}, {210, 16, 146},  {170, 166, 16}, {145, 54, 34},
      {106, 202, 222}, {81, 90, 240},   {41, 240, 110}, {16, 128, 128},
  };
  auto &vf = frame->stVFrame;
  const int bar_width = static_cast<int>(vf.u32Width / 8);
  for (CVI_U32 row = 0; row < vf.u32Height; ++row) {
    std::uint8_t *y_row = vf.pu8VirAddr[0] + row * vf.u32Stride[0];
    for (CVI_U32 col = 0; col < vf.u32Width; ++col) {
      const int bar = static_cast<int>((col + frame_index * 8) / bar_width) % 8;
      y_row[col] = static_cast<std::uint8_t>(kColors[bar][0]);
    }
  }
  for (CVI_U32 row = 0; row < vf.u32Height / 2; ++row) {
    std::uint8_t *uv_row = vf.pu8VirAddr[1] + row * vf.u32Stride[1];
    for (CVI_U32 col = 0; col < vf.u32Width; col += 2) {
      const int bar =
          static_cast<int>(((col / 2) + frame_index * 4) / (bar_width / 2)) % 8;
      uv_row[col] = static_cast<std::uint8_t>(kColors[bar][1]);
      uv_row[col + 1] = static_cast<std::uint8_t>(kColors[bar][2]);
    }
  }
}

void fillGridFrame(VIDEO_FRAME_INFO_S *frame, int frame_index) {
  auto &vf = frame->stVFrame;
  const int step = 40;
  const int phase = frame_index % step;
  for (CVI_U32 row = 0; row < vf.u32Height; ++row) {
    std::uint8_t *y_row = vf.pu8VirAddr[0] + row * vf.u32Stride[0];
    const bool horizontal = ((static_cast<int>(row) + phase) % step) < 2;
    for (CVI_U32 col = 0; col < vf.u32Width; ++col) {
      const bool vertical = ((static_cast<int>(col) + phase) % step) < 2;
      y_row[col] = (horizontal || vertical) ? 235 : 32;
    }
  }
  for (CVI_U32 row = 0; row < vf.u32Height / 2; ++row) {
    std::uint8_t *uv_row = vf.pu8VirAddr[1] + row * vf.u32Stride[1];
    for (CVI_U32 col = 0; col < vf.u32Width; col += 2) {
      uv_row[col] = 128;
      uv_row[col + 1] = 128;
    }
  }
}

void fillMotionFrame(VIDEO_FRAME_INFO_S *frame, int frame_index) {
  auto &vf = frame->stVFrame;
  const int box_size = 120;
  const int x0 = (frame_index * 17) % static_cast<int>(vf.u32Width);
  const int y0 = (frame_index * 11) % static_cast<int>(vf.u32Height);
  fillNv12Frame(frame, 32, 128, 128);
  for (int row = 0; row < box_size; ++row) {
    const int yy = (y0 + row) % static_cast<int>(vf.u32Height);
    std::uint8_t *y_row = vf.pu8VirAddr[0] + yy * vf.u32Stride[0];
    for (int col = 0; col < box_size; ++col) {
      const int xx = (x0 + col) % static_cast<int>(vf.u32Width);
      y_row[xx] = 200;
    }
  }
  for (int row = 0; row < box_size / 2; ++row) {
    const int yy = ((y0 / 2) + row) % static_cast<int>(vf.u32Height / 2);
    std::uint8_t *uv_row = vf.pu8VirAddr[1] + yy * vf.u32Stride[1];
    for (int col = 0; col < box_size; col += 2) {
      const int xx = (x0 + col) % static_cast<int>(vf.u32Width);
      uv_row[xx] = 16;
      uv_row[xx + 1] = 240;
    }
  }
}

void fillFeedFrame(VIDEO_FRAME_INFO_S *frame, const Options &opt,
                   int frame_index) {
  if (opt.feed_mode == "solid") {
    const int y = (48 + frame_index * 19) & 0xFF;
    const int u = (96 + frame_index * 7) & 0xFF;
    const int v = (160 + frame_index * 11) & 0xFF;
    fillNv12Frame(frame, y, u, v);
  } else if (opt.feed_mode == "bars") {
    fillBarsFrame(frame, frame_index);
  } else if (opt.feed_mode == "grid") {
    fillGridFrame(frame, frame_index);
  } else {
    fillMotionFrame(frame, frame_index);
  }
}

bool prepareFrame(int width, int height, VIDEO_FRAME_INFO_S *frame,
                  VB_BLK *blk, std::string *error) {
  SIZE_S size;
  size.u32Width = static_cast<CVI_U32>(width);
  size.u32Height = static_cast<CVI_U32>(height);

  VB_CAL_CONFIG_S calc;
  COMMON_GetPicBufferConfig(size.u32Width, size.u32Height, PIXEL_FORMAT_NV12,
                            DATA_BITWIDTH_8, COMPRESS_MODE_NONE,
                            DEFAULT_ALIGN, &calc);

  std::memset(frame, 0, sizeof(*frame));
  frame->stVFrame.enCompressMode = COMPRESS_MODE_NONE;
  frame->stVFrame.enPixelFormat = PIXEL_FORMAT_NV12;
  frame->stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
  frame->stVFrame.enColorGamut = COLOR_GAMUT_BT709;
  frame->stVFrame.enDynamicRange = DYNAMIC_RANGE_SDR8;
  frame->stVFrame.u32Width = size.u32Width;
  frame->stVFrame.u32Height = size.u32Height;
  frame->stVFrame.u32Stride[0] = calc.u32MainStride;
  frame->stVFrame.u32Stride[1] = calc.u32CStride;
  frame->stVFrame.u32Stride[2] = calc.u32CStride;
  frame->stVFrame.u32Length[0] = calc.u32MainYSize;
  frame->stVFrame.u32Length[1] = calc.u32MainCSize;
  frame->stVFrame.u32TimeRef = 0;
  frame->stVFrame.u64PTS = 0;

  *blk = CVI_VB_GetBlock(VB_INVALID_POOLID, calc.u32VBSize);
  if (*blk == VB_INVALID_HANDLE) {
    if (error) *error = "CVI_VB_GetBlock failed";
    return false;
  }

  frame->u32PoolId = CVI_VB_Handle2PoolId(*blk);
  frame->stVFrame.u64PhyAddr[0] = CVI_VB_Handle2PhysAddr(*blk);
  frame->stVFrame.u64PhyAddr[1] =
      frame->stVFrame.u64PhyAddr[0] +
      ALIGN(calc.u32MainYSize, calc.u16AddrAlign);

  frame->stVFrame.pu8VirAddr[0] = static_cast<CVI_U8 *>(
      CVI_SYS_MmapCache(frame->stVFrame.u64PhyAddr[0],
                        frame->stVFrame.u32Length[0]));
  if (!frame->stVFrame.pu8VirAddr[0]) {
    if (error) *error = "CVI_SYS_MmapCache plane0 failed";
    CVI_VB_ReleaseBlock(*blk);
    *blk = VB_INVALID_HANDLE;
    return false;
  }

  frame->stVFrame.pu8VirAddr[1] = static_cast<CVI_U8 *>(
      CVI_SYS_MmapCache(frame->stVFrame.u64PhyAddr[1],
                        frame->stVFrame.u32Length[1]));
  if (!frame->stVFrame.pu8VirAddr[1]) {
    if (error) *error = "CVI_SYS_MmapCache plane1 failed";
    CVI_SYS_Munmap(frame->stVFrame.pu8VirAddr[0], frame->stVFrame.u32Length[0]);
    frame->stVFrame.pu8VirAddr[0] = nullptr;
    CVI_VB_ReleaseBlock(*blk);
    *blk = VB_INVALID_HANDLE;
    return false;
  }

  return true;
}

void releaseFrame(VIDEO_FRAME_INFO_S *frame, VB_BLK blk) {
  if (frame->stVFrame.pu8VirAddr[0]) {
    CVI_SYS_Munmap(frame->stVFrame.pu8VirAddr[0], frame->stVFrame.u32Length[0]);
    frame->stVFrame.pu8VirAddr[0] = nullptr;
  }
  if (frame->stVFrame.pu8VirAddr[1]) {
    CVI_SYS_Munmap(frame->stVFrame.pu8VirAddr[1], frame->stVFrame.u32Length[1]);
    frame->stVFrame.pu8VirAddr[1] = nullptr;
  }
  if (blk != VB_INVALID_HANDLE) {
    CVI_VB_ReleaseBlock(blk);
  }
}

bool prepareFramePool(int width, int height, int count,
                      std::vector<PreparedFrame> *frames, std::string *error) {
  frames->clear();
  frames->reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    PreparedFrame prepared;
    if (!prepareFrame(width, height, &prepared.frame, &prepared.blk, error)) {
      for (auto &item : *frames) {
        releaseFrame(&item.frame, item.blk);
        item.blk = VB_INVALID_HANDLE;
      }
      frames->clear();
      return false;
    }
    frames->push_back(prepared);
  }
  return true;
}

bool configureVpssMemMode(std::string *error) {
  VI_VPSS_MODE_S vi_vpss_mode;
  std::memset(&vi_vpss_mode, 0, sizeof(vi_vpss_mode));
  vi_vpss_mode.aenMode[0] = VI_OFFLINE_VPSS_OFFLINE;
  vi_vpss_mode.aenMode[1] = VI_OFFLINE_VPSS_OFFLINE;
  int ret = CVI_SYS_SetVIVPSSMode(&vi_vpss_mode);
  if (ret != CVI_SUCCESS) {
    if (error) {
      *error = "CVI_SYS_SetVIVPSSMode failed, ret=" + std::to_string(ret);
    }
    return false;
  }

  VPSS_MODE_S vpss_mode;
  std::memset(&vpss_mode, 0, sizeof(vpss_mode));
  vpss_mode.enMode = VPSS_MODE_SINGLE;
  vpss_mode.aenInput[0] = VPSS_INPUT_MEM;
  ret = CVI_VPSS_SetMode(&vpss_mode);
  if (ret != CVI_SUCCESS) {
    if (error) *error = "CVI_VPSS_SetMode failed, ret=" + std::to_string(ret);
    return false;
  }
  std::fprintf(stderr, "osd_vpss: set vi_vpss offline/offline, mode single/mem\n");
  return true;
}

std::string decodeVpssError(int ret) {
  switch (ret) {
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
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%08X",
                static_cast<unsigned int>(ret));
  return buffer;
}

bool bindVpssToVo(int vpss_group, int vpss_chn, int vo_layer, int vo_chn,
                  std::string *error) {
  MMF_CHN_S src;
  MMF_CHN_S dst;
  std::memset(&src, 0, sizeof(src));
  std::memset(&dst, 0, sizeof(dst));
  src.enModId = CVI_ID_VPSS;
  src.s32DevId = vpss_group;
  src.s32ChnId = vpss_chn;
  dst.enModId = CVI_ID_VO;
  dst.s32DevId = vo_layer;
  dst.s32ChnId = vo_chn;
  const int ret = CVI_SYS_Bind(&src, &dst);
  if (ret != CVI_SUCCESS) {
    if (error) {
      *error = "CVI_SYS_Bind(VPSS->VO) failed, ret=" + std::to_string(ret);
    }
    return false;
  }
  return true;
}

void unbindVpssToVo(int vpss_group, int vpss_chn, int vo_layer, int vo_chn) {
  MMF_CHN_S src;
  MMF_CHN_S dst;
  std::memset(&src, 0, sizeof(src));
  std::memset(&dst, 0, sizeof(dst));
  src.enModId = CVI_ID_VPSS;
  src.s32DevId = vpss_group;
  src.s32ChnId = vpss_chn;
  dst.enModId = CVI_ID_VO;
  dst.s32DevId = vo_layer;
  dst.s32ChnId = vo_chn;
  CVI_SYS_UnBind(&src, &dst);
}

bool openNativeVpss(const Options &opt, NativeVpssContext *ctx,
                    std::string *error) {
  if (!ctx) {
    if (error) *error = "native vpss context is null";
    return false;
  }

  if (!configureVpssMemMode(error)) {
    return false;
  }
  ctx->mode_configured = true;
  ctx->group = opt.vpss_group;
  ctx->channel = opt.vpss_chn;
  ctx->vo_layer = opt.layer;
  ctx->vo_chn = opt.vo_chn;

  VPSS_GRP_ATTR_S grp_attr;
  std::memset(&grp_attr, 0, sizeof(grp_attr));
  grp_attr.stFrameRate.s32SrcFrameRate = -1;
  grp_attr.stFrameRate.s32DstFrameRate = -1;
  grp_attr.enPixelFormat = PIXEL_FORMAT_NV12;
  grp_attr.u32MaxW = static_cast<CVI_U32>(opt.feed_width);
  grp_attr.u32MaxH = static_cast<CVI_U32>(opt.feed_height);

  int ret = CVI_VPSS_CreateGrp(ctx->group, &grp_attr);
  if (ret != CVI_SUCCESS) {
    if (error) *error = "CVI_VPSS_CreateGrp failed, ret=" + std::to_string(ret);
    return false;
  }
  ctx->group_created = true;

  VPSS_CHN_ATTR_S chn_attr;
  std::memset(&chn_attr, 0, sizeof(chn_attr));
  chn_attr.u32Width = static_cast<CVI_U32>(opt.screen_width);
  chn_attr.u32Height = static_cast<CVI_U32>(opt.screen_height);
  chn_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
  chn_attr.enPixelFormat = PIXEL_FORMAT_NV12;
  chn_attr.stFrameRate.s32SrcFrameRate = 30;
  chn_attr.stFrameRate.s32DstFrameRate = 30;
  chn_attr.u32Depth = 1;
  chn_attr.bMirror = CVI_FALSE;
  chn_attr.bFlip = CVI_FALSE;
  chn_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;

  ret = CVI_VPSS_SetChnAttr(ctx->group, ctx->channel, &chn_attr);
  if (ret != CVI_SUCCESS) {
    if (error) *error = "CVI_VPSS_SetChnAttr failed, ret=" + std::to_string(ret);
    return false;
  }

  ret = CVI_VPSS_EnableChn(ctx->group, ctx->channel);
  if (ret != CVI_SUCCESS) {
    if (error) *error = "CVI_VPSS_EnableChn failed, ret=" + std::to_string(ret);
    return false;
  }
  ctx->channel_enabled = true;

  ret = CVI_VPSS_StartGrp(ctx->group);
  if (ret != CVI_SUCCESS) {
    if (error) *error = "CVI_VPSS_StartGrp failed, ret=" + std::to_string(ret);
    return false;
  }
  ctx->group_started = true;

  if (!bindVpssToVo(ctx->group, ctx->channel, ctx->vo_layer, ctx->vo_chn,
                    error)) {
    return false;
  }
  ctx->vo_bound = true;
  return true;
}

void closeNativeVpss(NativeVpssContext *ctx) {
  if (!ctx) return;
  if (ctx->vo_bound) {
    unbindVpssToVo(ctx->group, ctx->channel, ctx->vo_layer, ctx->vo_chn);
    ctx->vo_bound = false;
  }
  if (ctx->group_started) {
    CVI_VPSS_StopGrp(ctx->group);
    ctx->group_started = false;
  }
  if (ctx->channel_enabled) {
    CVI_VPSS_DisableChn(ctx->group, ctx->channel);
    ctx->channel_enabled = false;
  }
  if (ctx->group_created) {
    CVI_VPSS_DestroyGrp(ctx->group);
    ctx->group_created = false;
  }
}

void dumpBindState(int vpss_group, int vpss_chn) {
  MMF_CHN_S src;
  std::memset(&src, 0, sizeof(src));
  src.enModId = CVI_ID_VPSS;
  src.s32DevId = vpss_group;
  src.s32ChnId = vpss_chn;

  MMF_BIND_DEST_S dests;
  std::memset(&dests, 0, sizeof(dests));
  const int ret = CVI_SYS_GetBindbySrc(&src, &dests);
  std::fprintf(stderr, "osd_vpss: get bind ret=%d num=%u\n", ret, dests.u32Num);
  if (ret == CVI_SUCCESS) {
    for (CVI_U32 i = 0; i < dests.u32Num; ++i) {
      const MMF_CHN_S &dst = dests.astMmfChn[i];
      std::fprintf(stderr,
                   "osd_vpss: bind[%u] mod=%d dev=%d chn=%d\n",
                   i, static_cast<int>(dst.enModId), dst.s32DevId, dst.s32ChnId);
    }
  }
}

void probeVpssFrame(int vpss_group, int vpss_chn) {
  VIDEO_FRAME_INFO_S frame;
  std::memset(&frame, 0, sizeof(frame));
  const int ret = CVI_VPSS_GetChnFrame(vpss_group, vpss_chn, &frame, 100);
  if (ret != CVI_SUCCESS) {
    std::fprintf(stderr, "osd_vpss: probe get frame ret=%d (%s)\n", ret,
                 decodeVpssError(ret).c_str());
    return;
  }
  std::fprintf(stderr,
               "osd_vpss: probe frame %ux%u fmt=%d pts=%llu\n",
               frame.stVFrame.u32Width, frame.stVFrame.u32Height,
               static_cast<int>(frame.stVFrame.enPixelFormat),
               static_cast<unsigned long long>(frame.stVFrame.u64PTS));
  CVI_VPSS_ReleaseChnFrame(vpss_group, vpss_chn, &frame);
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }
  if (opt.common_blocks < 2) opt.common_blocks = 2;
  if (opt.feed_frames < 1) opt.feed_frames = 1;
  if (opt.feed_interval_ms < 0) opt.feed_interval_ms = 0;

  std::string error;
  tdl_app::SysContext sys;
  if (!sys.open(&error)) {
    std::cerr << "sys open failed: " << error << "\n";
    return 2;
  }

  std::unique_ptr<tdl_app::VoOutput> vo;
  if (opt.init_vo) {
    tdl_app::VoOutput::Config vo_config;
    vo_config.device = opt.vo_dev;
    vo_config.layer = opt.layer;
    vo_config.channel = opt.vo_chn;
    vo_config.width = opt.screen_width;
    vo_config.height = opt.screen_height;
    vo_config.interface_type = opt.interface_type;
    vo_config.interface_sync = opt.interface_sync;
    vo.reset(new tdl_app::VoOutput(vo_config));
    if (!vo->open(&error)) {
      std::cerr << "vo open failed: " << error << "\n";
      return 3;
    }
  }

  std::unique_ptr<tdl_app::VideoBufferManager> vb_manager;
  NativeVpssContext native_vpss;
  std::vector<PreparedFrame> frame_pool;

  if (opt.attach_module == "vpss") {
    if (!opt.init_vo) {
      std::cerr << "attach-module=vpss requires --init-vo so VPSS can bind to VO\n";
      return 3;
    }

    tdl_app::VideoBufferManager::Config vb_cfg;
    vb_cfg.common_pools.push_back(
        tdl_app::VideoBufferPoolConfig{{opt.feed_width, opt.feed_height},
                                       tdl_app::PixelFormat::NV12,
                                       opt.common_blocks,
                                       64,
                                       true});
    vb_manager.reset(new tdl_app::VideoBufferManager(vb_cfg));
    if (!vb_manager->open(&error)) {
      std::cerr << "vb manager open failed: " << error << "\n";
      return 3;
    }

    if (!openNativeVpss(opt, &native_vpss, &error)) {
      std::cerr << "vpss open failed: " << error << "\n";
      return 3;
    }
    dumpBindState(opt.vpss_group, opt.vpss_chn);

    if (!prepareFramePool(opt.feed_width, opt.feed_height, 1,
                          &frame_pool, &error)) {
      std::cerr << "prepare frame pool failed: " << error << "\n";
      return 3;
    }
  }

  tdl_app::OsdRegion::Config cfg = tdl_app::OsdRegion::canvas(
      opt.handle, opt.width, opt.height, opt.pixel_format, 2, 0);
  tdl_app::OsdRegion osd(cfg);
  if (!osd.create(&error)) {
    std::cerr << "osd create failed: " << error << "\n";
    return 4;
  }

  const tdl_app::MediaChannel target_channel =
      (opt.attach_module == "vpss")
          ? tdl_app::MediaChannel::vpss(opt.vpss_group, opt.vpss_chn)
          : tdl_app::MediaChannel::vo(opt.layer, opt.vo_chn);

  if (!osd.attach(target_channel, opt.x, opt.y, opt.layer, &error)) {
    std::cerr << "osd attach failed: " << error << "\n";
    return 5;
  }

  tdl_app::OsdCanvas canvas;
  if (!osd.getCanvas(&canvas, &error)) {
    std::cerr << "osd get canvas failed: " << error << "\n";
    return 6;
  }
  std::fprintf(stderr, "osd: canvas size=%dx%d stride=%d fmt=%d\n", canvas.width,
               canvas.height, canvas.stride, canvas.pixel_format);

  fillCanvas(canvas, opt.pixel_format, opt.color);
  if (!osd.updateCanvas(&error)) {
    std::cerr << "osd update canvas failed: " << error << "\n";
    return 7;
  }

  if (opt.attach_module == "vpss") {
    using clock = std::chrono::steady_clock;
    std::uint64_t total_fill_us = 0;
    std::uint64_t total_send_us = 0;
    const auto begin_all = clock::now();
    for (int i = 0; i < opt.feed_frames; ++i) {
      PreparedFrame &prepared =
          frame_pool[static_cast<std::size_t>(i % frame_pool.size())];
      auto &frame = prepared.frame;

      if (opt.osd_move_step_x != 0 || opt.osd_move_step_y != 0) {
        const int next_x = opt.x + opt.osd_move_step_x * i;
        const int next_y = opt.y + opt.osd_move_step_y * i;
        if (!osd.moveTo(next_x, next_y, &error)) {
          std::cerr << "osd move failed: " << error << "\n";
          return 8;
        }
      }
      if (opt.osd_color_step != 0) {
        tdl_app::OsdCanvas dynamic_canvas;
        if (!osd.getCanvas(&dynamic_canvas, &error)) {
          std::cerr << "osd get canvas failed during loop: " << error << "\n";
          return 8;
        }
        fillCanvas(dynamic_canvas, opt.pixel_format, animateOsdColor(opt, i));
        if (!osd.updateCanvas(&error)) {
          std::cerr << "osd update canvas failed during loop: " << error << "\n";
          return 8;
        }
      }

      const auto begin_fill = clock::now();
      fillFeedFrame(&frame, opt, i);
      frame.stVFrame.u32TimeRef = static_cast<CVI_U32>(i);
      frame.stVFrame.u64PTS = static_cast<CVI_U64>(i);
      CVI_SYS_IonFlushCache(frame.stVFrame.u64PhyAddr[0],
                            frame.stVFrame.pu8VirAddr[0],
                            frame.stVFrame.u32Length[0]);
      CVI_SYS_IonFlushCache(frame.stVFrame.u64PhyAddr[1],
                            frame.stVFrame.pu8VirAddr[1],
                            frame.stVFrame.u32Length[1]);
      const auto end_fill = clock::now();

      const auto begin_send = clock::now();
      const int ret = CVI_VPSS_SendFrame(opt.vpss_group, &frame, 1000);
      const auto end_send = clock::now();
      if (ret != CVI_SUCCESS) {
        std::cerr << "CVI_VPSS_SendFrame failed, ret=" << ret << " ("
                  << decodeVpssError(ret) << ")\n";
        return 8;
      }
      if (i == 0 || i == opt.feed_frames - 1) {
        probeVpssFrame(opt.vpss_group, opt.vpss_chn);
      }

      total_fill_us += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(end_fill - begin_fill)
              .count());
      total_send_us += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(end_send - begin_send)
              .count());

      if (opt.feed_interval_ms > 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(opt.feed_interval_ms));
      }
    }
    const auto end_all = clock::now();
    const auto total_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_all - begin_all).count());
    const double fps =
        total_us > 0
            ? (static_cast<double>(opt.feed_frames) * 1000000.0 /
               static_cast<double>(total_us))
            : 0.0;
    std::cout << "osd_vpss summary:"
              << " frames=" << opt.feed_frames
              << " total_ms=" << (total_us / 1000.0)
              << " fps=" << fps
              << " avg_fill_us="
              << (opt.feed_frames > 0 ? total_fill_us / opt.feed_frames : 0)
              << " avg_send_us="
              << (opt.feed_frames > 0 ? total_send_us / opt.feed_frames : 0)
              << "\n";
  } else {
    std::this_thread::sleep_for(std::chrono::milliseconds(opt.hold_ms));
  }

  osd.detach();
  osd.destroy();
  for (auto &prepared : frame_pool) {
    releaseFrame(&prepared.frame, prepared.blk);
    prepared.blk = VB_INVALID_HANDLE;
  }
  closeNativeVpss(&native_vpss);
  return 0;
}
