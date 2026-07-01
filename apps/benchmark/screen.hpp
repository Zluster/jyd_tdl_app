#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "camera_demo_support.hpp"
#include "tdl_app/media_link.hpp"
#include "tdl_app/osd_region.hpp"
#include "tdl_app/vo_output.hpp"

namespace tdl_bench {

// 参照 sophpi_camera_demo：自建 live 相机 -> grp0(live) -> grp1/ch0 -> VO 链路，
// 并在 grp1/ch0 挂全屏 ARGB OSD，通过更新 OSD 画布显示纯色。
// 绑定前做防御性解绑、create 前做 RGN 清理，保证跨异常退出可重入。
class ScreenDisplay {
 public:
  struct Config {
    int vo_dev = 0;
    int layer = 0;
    int vo_chn = 0;
    int screen_width = 720;
    int screen_height = 1280;
    int interface_type = 0;  // tdl_app::VoInterfaceType::Mipi
    int interface_sync = 0;  // tdl_app::VoInterfaceSync::P720_1280_60
    int osd_handle = 140;
  };

  ScreenDisplay();
  ~ScreenDisplay();

  ScreenDisplay(const ScreenDisplay &) = delete;
  ScreenDisplay &operator=(const ScreenDisplay &) = delete;

  bool open(const Config &config, std::string *error = nullptr);
  void close();
  bool isOpen() const { return osd_ != nullptr; }

  int canvasWidth() const { return canvas_width_; }
  int canvasHeight() const { return canvas_height_; }

  // 全屏纯色（0xAARRGGBB）。
  bool showColor(std::uint32_t argb, std::string *error = nullptr);

 private:
  camera_demo_support::CommonOptions camera_options_;
  camera_demo_support::CameraRuntime runtime_;
  std::unique_ptr<tdl_app::VoOutput> vo_;
  std::unique_ptr<tdl_app::MediaLink> preview_link_;
  std::unique_ptr<tdl_app::MediaLink> display_link_;
  std::unique_ptr<tdl_app::OsdRegion> osd_;
  int canvas_width_ = 0;
  int canvas_height_ = 0;
};

}  // namespace tdl_bench
