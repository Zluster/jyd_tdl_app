#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "cvi_buffer.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "cvi_vo.h"

#include "framework.hpp"
#include "tdl_app/video_buffer.hpp"
#include "tdl_app/vo_output.hpp"

namespace tdl_bench {
namespace {

struct PreparedFrame {
  VIDEO_FRAME_INFO_S frame{};
  VB_BLK blk = VB_INVALID_HANDLE;
};

// 准备一个 NV12 帧：从全局 VB 拿块并 mmap 两个平面。
bool prepareFrame(int width, int height, PreparedFrame *out,
                  std::string *error) {
  VB_CAL_CONFIG_S calc;
  COMMON_GetPicBufferConfig(static_cast<CVI_U32>(width),
                            static_cast<CVI_U32>(height), PIXEL_FORMAT_NV12,
                            DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN,
                            &calc);

  VIDEO_FRAME_INFO_S *frame = &out->frame;
  std::memset(frame, 0, sizeof(*frame));
  frame->stVFrame.enCompressMode = COMPRESS_MODE_NONE;
  frame->stVFrame.enPixelFormat = PIXEL_FORMAT_NV12;
  frame->stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
  frame->stVFrame.enColorGamut = COLOR_GAMUT_BT601;
  frame->stVFrame.enDynamicRange = DYNAMIC_RANGE_SDR8;
  frame->stVFrame.u32Width = static_cast<CVI_U32>(width);
  frame->stVFrame.u32Height = static_cast<CVI_U32>(height);
  frame->stVFrame.u32Stride[0] = calc.u32MainStride;
  frame->stVFrame.u32Stride[1] = calc.u32CStride;
  frame->stVFrame.u32Length[0] = calc.u32MainYSize;
  frame->stVFrame.u32Length[1] = calc.u32MainCSize;

  out->blk = CVI_VB_GetBlock(VB_INVALID_POOLID, calc.u32VBSize);
  if (out->blk == VB_INVALID_HANDLE) {
    if (error) *error = "CVI_VB_GetBlock failed";
    return false;
  }

  frame->u32PoolId = CVI_VB_Handle2PoolId(out->blk);
  frame->stVFrame.u64PhyAddr[0] = CVI_VB_Handle2PhysAddr(out->blk);
  frame->stVFrame.u64PhyAddr[1] =
      frame->stVFrame.u64PhyAddr[0] + ALIGN(calc.u32MainYSize, calc.u16AddrAlign);

  frame->stVFrame.pu8VirAddr[0] = static_cast<CVI_U8 *>(CVI_SYS_MmapCache(
      frame->stVFrame.u64PhyAddr[0], frame->stVFrame.u32Length[0]));
  if (!frame->stVFrame.pu8VirAddr[0]) {
    if (error) *error = "CVI_SYS_MmapCache plane0 failed";
    CVI_VB_ReleaseBlock(out->blk);
    out->blk = VB_INVALID_HANDLE;
    return false;
  }
  frame->stVFrame.pu8VirAddr[1] = static_cast<CVI_U8 *>(CVI_SYS_MmapCache(
      frame->stVFrame.u64PhyAddr[1], frame->stVFrame.u32Length[1]));
  if (!frame->stVFrame.pu8VirAddr[1]) {
    if (error) *error = "CVI_SYS_MmapCache plane1 failed";
    CVI_SYS_Munmap(frame->stVFrame.pu8VirAddr[0], frame->stVFrame.u32Length[0]);
    frame->stVFrame.pu8VirAddr[0] = nullptr;
    CVI_VB_ReleaseBlock(out->blk);
    out->blk = VB_INVALID_HANDLE;
    return false;
  }
  return true;
}

void releaseFrame(PreparedFrame *prepared) {
  VIDEO_FRAME_INFO_S *frame = &prepared->frame;
  if (frame->stVFrame.pu8VirAddr[0]) {
    CVI_SYS_Munmap(frame->stVFrame.pu8VirAddr[0], frame->stVFrame.u32Length[0]);
    frame->stVFrame.pu8VirAddr[0] = nullptr;
  }
  if (frame->stVFrame.pu8VirAddr[1]) {
    CVI_SYS_Munmap(frame->stVFrame.pu8VirAddr[1], frame->stVFrame.u32Length[1]);
    frame->stVFrame.pu8VirAddr[1] = nullptr;
  }
  if (prepared->blk != VB_INVALID_HANDLE) {
    CVI_VB_ReleaseBlock(prepared->blk);
    prepared->blk = VB_INVALID_HANDLE;
  }
}

// 填一个移动方块，保证每帧画面有变化、便于肉眼确认刷新。
void fillMotion(VIDEO_FRAME_INFO_S *frame, int frame_index) {
  auto &vf = frame->stVFrame;
  for (CVI_U32 row = 0; row < vf.u32Height; ++row) {
    std::memset(vf.pu8VirAddr[0] + row * vf.u32Stride[0], 32, vf.u32Width);
  }
  for (CVI_U32 row = 0; row < vf.u32Height / 2; ++row) {
    std::uint8_t *uv = vf.pu8VirAddr[1] + row * vf.u32Stride[1];
    for (CVI_U32 col = 0; col < vf.u32Width; col += 2) {
      uv[col] = 128;
      uv[col + 1] = 128;
    }
  }
  const int box = 80;
  const int max_x = std::max(1, static_cast<int>(vf.u32Width) - box);
  const int max_y = std::max(1, static_cast<int>(vf.u32Height) - box);
  const int x0 = (frame_index * 17) % max_x;
  const int y0 = (frame_index * 11) % max_y;
  const int box_w = std::min(box, static_cast<int>(vf.u32Width));
  const int box_h = std::min(box, static_cast<int>(vf.u32Height));
  for (int r = 0; r < box_h; ++r) {
    std::uint8_t *y_row = vf.pu8VirAddr[0] + (y0 + r) * vf.u32Stride[0];
    for (int c = 0; c < box_w; ++c) {
      y_row[x0 + c] = 200;
    }
  }
}

// 在固定屏幕时序下，以 res 作为 VO 通道尺寸送 frames 帧，测刷新率。
// VO 硬件会把该分辨率画布缩放到物理屏，等价于 disp.show(WxH 图)。
bool runResolution(const RunConfig &cfg, int width, int height,
                   std::string *error) {
  const int frames = cfg.frames > 0 ? cfg.frames : 60;
  const int buffers = cfg.buffers > 0 ? cfg.buffers : 3;

  tdl_app::VoOutput::Config vo_config;
  vo_config.device = cfg.vo_dev;
  vo_config.layer = cfg.layer;
  vo_config.channel = cfg.vo_chn;
  vo_config.width = width;
  vo_config.height = height;
  vo_config.pixel_format = tdl_app::PixelFormat::NV12;
  vo_config.interface_type = cfg.interface_type;
  vo_config.interface_sync = cfg.interface_sync;
  tdl_app::VoOutput vo(vo_config);
  if (!vo.open(error)) {
    return false;
  }

  std::vector<PreparedFrame> pool(static_cast<std::size_t>(buffers));
  for (int i = 0; i < buffers; ++i) {
    if (!prepareFrame(width, height, &pool[i], error)) {
      for (int j = 0; j < i; ++j) releaseFrame(&pool[j]);
      vo.close();
      return false;
    }
  }

  bool ok = true;
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) {
    PreparedFrame &pf = pool[static_cast<std::size_t>(i % buffers)];
    fillMotion(&pf.frame, i);
    CVI_SYS_IonFlushCache(pf.frame.stVFrame.u64PhyAddr[0],
                          pf.frame.stVFrame.pu8VirAddr[0],
                          pf.frame.stVFrame.u32Length[0]);
    CVI_SYS_IonFlushCache(pf.frame.stVFrame.u64PhyAddr[1],
                          pf.frame.stVFrame.pu8VirAddr[1],
                          pf.frame.stVFrame.u32Length[1]);
    const int ret = CVI_VO_SendFrame(cfg.layer, cfg.vo_chn, &pf.frame, -1);
    if (ret != CVI_SUCCESS) {
      if (error) *error = "CVI_VO_SendFrame failed, ret=" + std::to_string(ret);
      ok = false;
      break;
    }
  }
  const auto end = std::chrono::steady_clock::now();

  for (auto &item : pool) releaseFrame(&item);
  vo.close();

  if (!ok) {
    return false;
  }

  const double total_us = static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
          .count());
  const double per_frame_us = frames > 0 ? (total_us / frames) : 0.0;
  const double fps = per_frame_us > 0.0 ? (1000000.0 / per_frame_us) : 0.0;
  std::cout << "  send " << width << "x" << height << ": " << per_frame_us
            << " us/frame, " << fps << " fps" << std::endl;
  return true;
}

class DisplayBenchmark : public BenchmarkModule {
 public:
  const char *name() const override { return "Display(VoOutput)"; }

  bool load(BenchmarkContext &ctx, std::string *error) override {
    const RunConfig &cfg = ctx.config();

    resolutions_ = {{320, 240},
                    {320, 320},
                    {640, 480},
                    {640, 640},
                    {cfg.screen_width, cfg.screen_height}};

    // common pool 需容纳最大帧，供各分辨率从全局 VB 取块。
    int max_w = 0;
    int max_h = 0;
    for (const auto &res : resolutions_) {
      max_w = std::max(max_w, res.first);
      max_h = std::max(max_h, res.second);
    }

    tdl_app::VideoBufferManager::Config vb_config;
    tdl_app::VideoBufferPoolConfig pool;
    pool.size = {max_w, max_h};
    pool.pixel_format = tdl_app::PixelFormat::NV12;
    pool.block_count = cfg.buffers > 0 ? cfg.buffers : 3;
    pool.align = 64;
    pool.cached = true;
    vb_config.common_pools.push_back(pool);
    vb_.reset(new tdl_app::VideoBufferManager(vb_config));
    if (!vb_->open(error)) {
      vb_.reset();
      return false;
    }
    return true;
  }

  bool loop(BenchmarkContext &ctx, std::string *error) override {
    const RunConfig &cfg = ctx.config();
    std::cout << "[Display] VoOutput send, panel sync=" << cfg.interface_sync
              << ", " << (cfg.frames > 0 ? cfg.frames : 60) << " frames/res"
              << std::endl;

    for (const auto &res : resolutions_) {
      if (!runResolution(cfg, res.first, res.second, error)) {
        return false;
      }
    }
    return true;
  }

  void exit(BenchmarkContext &ctx) override {
    (void)ctx;
    if (vb_) {
      vb_->close();
      vb_.reset();
    }
  }

 private:
  std::vector<std::pair<int, int>> resolutions_;
  std::unique_ptr<tdl_app::VideoBufferManager> vb_;
};

}  // namespace

std::unique_ptr<BenchmarkModule> createDisplayModule() {
  return std::unique_ptr<BenchmarkModule>(new DisplayBenchmark());
}

}  // namespace tdl_bench
