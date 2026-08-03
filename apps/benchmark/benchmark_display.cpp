#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "framework.hpp"
#include "screen.hpp"
#include "tdl_app/media_types.hpp"

namespace tdl_bench {
namespace {

std::uint32_t argb(std::uint8_t a, std::uint8_t r, std::uint8_t g,
                   std::uint8_t b) {
  return (static_cast<std::uint32_t>(a) << 24) |
         (static_cast<std::uint32_t>(r) << 16) |
         (static_cast<std::uint32_t>(g) << 8) | b;
}

// 仿照 sophpi_camera_demo：大核借小核 grp0(live) -> grp1 -> VO 链路显示，
// 在 grp1/ch0 贴全屏 OSD，循环刷红/绿/蓝纯色并统计 fps。
class DisplayBenchmark : public BenchmarkModule {
 public:
  const char *name() const override { return "Display(RGB OSD via grp1->VO)"; }

  bool load(BenchmarkContext &ctx, std::string *error) override {
    const RunConfig &cfg = ctx.config();
    ScreenDisplay::Config sc;
    sc.vo_dev = cfg.vo_dev;
    sc.layer = cfg.layer;
    sc.vo_chn = cfg.vo_chn;
    sc.screen_width = cfg.screen_width;
    sc.screen_height = cfg.screen_height;
    sc.interface_type = cfg.interface_type;
    sc.interface_sync = cfg.interface_sync;
    screen_.reset(new ScreenDisplay());
    if (!screen_->open(sc, error)) {
      screen_.reset();
      return false;
    }
    return true;
  }

  bool loop(BenchmarkContext &ctx, std::string *error) override {
    const RunConfig &cfg = ctx.config();
    const int frames = cfg.frames > 0 ? cfg.frames : 60;
    const std::uint32_t colors[3] = {argb(255, 255, 0, 0),   // red
                                     argb(255, 0, 255, 0),   // green
                                     argb(255, 0, 0, 255)};  // blue

    std::cout << "[Display] full-screen RGB via grp1->VO OSD, " << frames
              << " frames @ " << screen_->canvasWidth() << "x"
              << screen_->canvasHeight() << std::endl;

    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
      // 全程分三段，红/绿/蓝各占 1/3，便于肉眼确认全屏刷新。
      const int color_idx = std::min(2, (i * 3) / frames);
      if (!screen_->showColor(colors[color_idx], error)) {
        return false;
      }
    }
    const auto end = std::chrono::steady_clock::now();

    const double total_us = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
            .count());
    const double per_frame_us = frames > 0 ? (total_us / frames) : 0.0;
    const double fps = per_frame_us > 0.0 ? (1000000.0 / per_frame_us) : 0.0;
    std::cout << "  RGB flush (red/green/blue): " << per_frame_us
              << " us/frame, " << fps << " fps" << std::endl;
    return true;
  }

  void exit(BenchmarkContext &ctx) override {
    (void)ctx;
    if (screen_) {
      screen_->close();
      screen_.reset();
    }
  }

 private:
  std::unique_ptr<ScreenDisplay> screen_;
};

}  // namespace

std::unique_ptr<BenchmarkModule> createDisplayModule() {
  return std::unique_ptr<BenchmarkModule>(new DisplayBenchmark());
}

}  // namespace tdl_bench
