#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <iostream>
#include <vector>
#include <string>
#include <thread>

#include "cvi_buffer.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_vo.h"
#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  int vo_dev = 0;
  int layer = 0;
  int vo_chn = 0;
  int width = 640;
  int height = 960;
  int interface_type = tdl_app::VoInterfaceType::Mipi;
  int interface_sync = tdl_app::VoInterfaceSync::P480_640_60;
  int frames = 30;
  int interval_ms = 300;
  int common_blocks = 3;
  std::string mode = "motion";
  bool print_frame_time = false;
  int y = 76;
  int u = 84;
  int v = 255;
};

struct PreparedFrame {
  VIDEO_FRAME_INFO_S frame{};
  VB_BLK blk = VB_INVALID_HANDLE;
};

struct TimingStats {
  std::uint64_t total_us = 0;
  std::uint64_t min_us = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_us = 0;

  void add(std::uint64_t value) {
    total_us += value;
    if (value < min_us) min_us = value;
    if (value > max_us) max_us = value;
  }

  std::uint64_t avg(int count) const {
    return count > 0 ? (total_us / static_cast<std::uint64_t>(count)) : 0;
  }

  std::uint64_t minOrZero(int count) const {
    return count > 0 ? min_us : 0;
  }
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_mmf_vo_fill_demo [--vo-dev N] [--layer N] [--vo-chn N]\n"
      << "                       [--width N] [--height N]\n"
      << "                       [--interface-type N] [--interface-sync N]\n"
      << "                       [--frames N] [--interval-ms N] [--common-blocks N]\n"
      << "                       [--mode solid|bars|grid|motion|gradient|border] [--print-frame-time]\n"
      << "                       [--y N] [--u N] [--v N]\n";
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

    if (arg == "--vo-dev") {
      const char *v = value("--vo-dev");
      if (!v) return false;
      opt->vo_dev = std::atoi(v);
    } else if (arg == "--layer") {
      const char *v = value("--layer");
      if (!v) return false;
      opt->layer = std::atoi(v);
    } else if (arg == "--vo-chn") {
      const char *v = value("--vo-chn");
      if (!v) return false;
      opt->vo_chn = std::atoi(v);
    } else if (arg == "--width") {
      const char *v = value("--width");
      if (!v) return false;
      opt->width = std::atoi(v);
    } else if (arg == "--height") {
      const char *v = value("--height");
      if (!v) return false;
      opt->height = std::atoi(v);
    } else if (arg == "--interface-type") {
      const char *v = value("--interface-type");
      if (!v) return false;
      opt->interface_type = std::atoi(v);
    } else if (arg == "--interface-sync") {
      const char *v = value("--interface-sync");
      if (!v) return false;
      opt->interface_sync = std::atoi(v);
    } else if (arg == "--frames") {
      const char *v = value("--frames");
      if (!v) return false;
      opt->frames = std::atoi(v);
    } else if (arg == "--common-blocks") {
      const char *v = value("--common-blocks");
      if (!v) return false;
      opt->common_blocks = std::atoi(v);
    } else if (arg == "--interval-ms") {
      const char *v = value("--interval-ms");
      if (!v) return false;
      opt->interval_ms = std::atoi(v);
    } else if (arg == "--mode") {
      const char *v = value("--mode");
      if (!v) return false;
      opt->mode = v;
    } else if (arg == "--print-frame-time") {
      opt->print_frame_time = true;
    } else if (arg == "--y") {
      const char *v = value("--y");
      if (!v) return false;
      opt->y = std::atoi(v);
    } else if (arg == "--u") {
      const char *v = value("--u");
      if (!v) return false;
      opt->u = std::atoi(v);
    } else if (arg == "--v") {
      const char *v = value("--v");
      if (!v) return false;
      opt->v = std::atoi(v);
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
      const int bar = static_cast<int>(((col / 2) + frame_index * 4) / (bar_width / 2)) % 8;
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

void fillGradientFrame(VIDEO_FRAME_INFO_S *frame, int frame_index) {
  auto &vf = frame->stVFrame;
  for (CVI_U32 row = 0; row < vf.u32Height; ++row) {
    std::uint8_t *y_row = vf.pu8VirAddr[0] + row * vf.u32Stride[0];
    for (CVI_U32 col = 0; col < vf.u32Width; ++col) {
      y_row[col] = static_cast<std::uint8_t>((col + row + frame_index * 4) & 0xFF);
    }
  }
  for (CVI_U32 row = 0; row < vf.u32Height / 2; ++row) {
    std::uint8_t *uv_row = vf.pu8VirAddr[1] + row * vf.u32Stride[1];
    for (CVI_U32 col = 0; col < vf.u32Width; col += 2) {
      uv_row[col] = static_cast<std::uint8_t>((64 + row + frame_index * 3) & 0xFF);
      uv_row[col + 1] = static_cast<std::uint8_t>((192 + col / 2 + frame_index * 5) & 0xFF);
    }
  }
}

void fillBorderFrame(VIDEO_FRAME_INFO_S *frame, int frame_index) {
  auto &vf = frame->stVFrame;
  fillNv12Frame(frame, 24, 128, 128);
  const int border = 8 + (frame_index % 48);
  for (CVI_U32 row = 0; row < vf.u32Height; ++row) {
    std::uint8_t *y_row = vf.pu8VirAddr[0] + row * vf.u32Stride[0];
    const bool active_row =
        row < static_cast<CVI_U32>(border) ||
        row + static_cast<CVI_U32>(border) >= vf.u32Height;
    for (CVI_U32 col = 0; col < vf.u32Width; ++col) {
      const bool active_col =
          col < static_cast<CVI_U32>(border) ||
          col + static_cast<CVI_U32>(border) >= vf.u32Width;
      if (active_row || active_col) y_row[col] = 235;
    }
  }
  for (CVI_U32 row = 0; row < vf.u32Height / 2; ++row) {
    std::uint8_t *uv_row = vf.pu8VirAddr[1] + row * vf.u32Stride[1];
    const bool active_row =
        row < static_cast<CVI_U32>(border / 2) ||
        row + static_cast<CVI_U32>(border / 2) >= (vf.u32Height / 2);
    for (CVI_U32 col = 0; col < vf.u32Width; col += 2) {
      const bool active_col =
          col < static_cast<CVI_U32>(border) ||
          col + static_cast<CVI_U32>(border) >= vf.u32Width;
      if (active_row || active_col) {
        uv_row[col] = 16;
        uv_row[col + 1] = 240;
      }
    }
  }
}

void fillByMode(VIDEO_FRAME_INFO_S *frame, const Options &opt, int frame_index) {
  if (opt.mode == "solid") {
    const int y = (opt.y + (frame_index * 23)) & 0xFF;
    const int u = (opt.u + (frame_index * 11)) & 0xFF;
    const int v = (opt.v + (frame_index * 17)) & 0xFF;
    fillNv12Frame(frame, y, u, v);
  } else if (opt.mode == "bars") {
    fillBarsFrame(frame, frame_index);
  } else if (opt.mode == "grid") {
    fillGridFrame(frame, frame_index);
  } else if (opt.mode == "gradient") {
    fillGradientFrame(frame, frame_index);
  } else if (opt.mode == "border") {
    fillBorderFrame(frame, frame_index);
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
  frame->stVFrame.enColorGamut = COLOR_GAMUT_BT601;
  frame->stVFrame.enDynamicRange = DYNAMIC_RANGE_SDR8;
  frame->stVFrame.u32Width = size.u32Width;
  frame->stVFrame.u32Height = size.u32Height;
  frame->stVFrame.u32Stride[0] = calc.u32MainStride;
  frame->stVFrame.u32Stride[1] = calc.u32CStride;
  frame->stVFrame.u32Length[0] = calc.u32MainYSize;
  frame->stVFrame.u32Length[1] = calc.u32MainCSize;

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
      CVI_SYS_MmapCache(frame->stVFrame.u64PhyAddr[0], frame->stVFrame.u32Length[0]));
  if (!frame->stVFrame.pu8VirAddr[0]) {
    if (error) *error = "CVI_SYS_MmapCache plane0 failed";
    CVI_VB_ReleaseBlock(*blk);
    *blk = VB_INVALID_HANDLE;
    return false;
  }

  frame->stVFrame.pu8VirAddr[1] = static_cast<CVI_U8 *>(
      CVI_SYS_MmapCache(frame->stVFrame.u64PhyAddr[1], frame->stVFrame.u32Length[1]));
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

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }
  if (opt.common_blocks < 2) opt.common_blocks = 2;
  if (opt.frames < 1) opt.frames = 1;
  if (opt.interval_ms < 0) opt.interval_ms = 0;

  std::string error;
  tdl_app::SysContext sys;
  if (!sys.open(&error)) {
    std::cerr << "sys open failed: " << error << "\n";
    return 2;
  }

  tdl_app::VideoBufferManager::Config vb_manager_config;
  vb_manager_config.common_pools.push_back(
      tdl_app::VideoBufferPoolConfig{{opt.width, opt.height},
                                     tdl_app::PixelFormat::NV12,
                                     opt.common_blocks,
                                     64,
                                     true});
  tdl_app::VideoBufferManager vb_manager(vb_manager_config);
  if (!vb_manager.open(&error)) {
    std::cerr << "vb manager open failed: " << error << "\n";
    return 3;
  }

  tdl_app::VoOutput::Config vo_config;
  vo_config.device = opt.vo_dev;
  vo_config.layer = opt.layer;
  vo_config.channel = opt.vo_chn;
  vo_config.width = opt.width;
  vo_config.height = opt.height;
  vo_config.interface_type = opt.interface_type;
  vo_config.interface_sync = opt.interface_sync;

  tdl_app::VoOutput vo(vo_config);
  if (!vo.open(&error)) {
    std::cerr << "vo open failed: " << error << "\n";
    return 4;
  }

  std::vector<PreparedFrame> frame_pool;
  if (!prepareFramePool(opt.width, opt.height, opt.common_blocks, &frame_pool,
                        &error)) {
    std::cerr << "prepare frame pool failed: " << error << "\n";
    return 5;
  }

  using clock = std::chrono::steady_clock;
  TimingStats fill_stats;
  TimingStats flush_stats;
  TimingStats send_stats;
  TimingStats frame_stats;
  const auto all_begin = clock::now();

  for (int i = 0; i < opt.frames; ++i) {
    PreparedFrame &prepared = frame_pool[static_cast<std::size_t>(i % frame_pool.size())];
    auto &frame = prepared.frame;
    const auto frame_begin = clock::now();

    const auto fill_begin = clock::now();
    fillByMode(&frame, opt, i);
    const auto fill_end = clock::now();

    const auto flush_begin = clock::now();
    CVI_SYS_IonFlushCache(frame.stVFrame.u64PhyAddr[0], frame.stVFrame.pu8VirAddr[0],
                          frame.stVFrame.u32Length[0]);
    CVI_SYS_IonFlushCache(frame.stVFrame.u64PhyAddr[1], frame.stVFrame.pu8VirAddr[1],
                          frame.stVFrame.u32Length[1]);
    const auto flush_end = clock::now();

    const auto send_begin = clock::now();
    const int ret = CVI_VO_SendFrame(opt.layer, opt.vo_chn, &frame, -1);
    const auto send_end = clock::now();
    if (ret != CVI_SUCCESS) {
      std::cerr << "CVI_VO_SendFrame failed, ret=" << ret << "\n";
      return 6;
    }
    const auto frame_end = clock::now();

    const auto fill_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(fill_end - fill_begin).count());
    const auto flush_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(flush_end - flush_begin).count());
    const auto send_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(send_end - send_begin).count());
    const auto frame_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_begin).count());
    fill_stats.add(fill_us);
    flush_stats.add(flush_us);
    send_stats.add(send_us);
    frame_stats.add(frame_us);

    if (opt.print_frame_time) {
      std::cout << "frame[" << i << "] fill_us=" << fill_us
                << " flush_us=" << flush_us
                << " send_us=" << send_us
                << " frame_us=" << frame_us << "\n";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));
  }

  const auto all_end = clock::now();
  const auto total_us =
      std::chrono::duration_cast<std::chrono::microseconds>(all_end - all_begin).count();
  const double fps =
      total_us > 0 ? (static_cast<double>(opt.frames) * 1000000.0 / static_cast<double>(total_us))
                   : 0.0;
  std::cout << "vo fill summary:"
            << " mode=" << opt.mode
            << " frames=" << opt.frames
            << " buffers=" << frame_pool.size()
            << " total_ms=" << (total_us / 1000.0)
            << " fps=" << fps
            << " avg_fill_us=" << fill_stats.avg(opt.frames)
            << " min_fill_us=" << fill_stats.minOrZero(opt.frames)
            << " max_fill_us=" << fill_stats.max_us
            << " avg_flush_us=" << flush_stats.avg(opt.frames)
            << " min_flush_us=" << flush_stats.minOrZero(opt.frames)
            << " max_flush_us=" << flush_stats.max_us
            << " avg_send_us=" << send_stats.avg(opt.frames)
            << " min_send_us=" << send_stats.minOrZero(opt.frames)
            << " max_send_us=" << send_stats.max_us
            << " avg_frame_us=" << frame_stats.avg(opt.frames)
            << " min_frame_us=" << frame_stats.minOrZero(opt.frames)
            << " max_frame_us=" << frame_stats.max_us
            << "\n";

  for (auto &prepared : frame_pool) {
    releaseFrame(&prepared.frame, prepared.blk);
    prepared.blk = VB_INVALID_HANDLE;
  }

  return 0;
}
