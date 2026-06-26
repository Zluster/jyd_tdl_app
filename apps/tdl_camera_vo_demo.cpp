#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include <opencv2/imgproc.hpp>

#include "cvi_sys.h"
#include "camera_demo_support.hpp"
#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  camera_demo_support::CommonOptions camera;
  int vo_dev = 0;
  int layer = 0;
  int vo_chn = 0;
  int screen_width = 0;
  int screen_height = 0;
  int interface_type = tdl_app::VoInterfaceType::Mipi;
  int interface_sync = tdl_app::VoInterfaceSync::P480_640_60;
  int preview_fps = 30;
  bool ui_osd = false;
  int ui_handle = 120;
  int ui_width = 220;
  int ui_height = 120;
  int ui_x = 20;
  int ui_y = 20;
  int ui_layer = 0;
  int ui_pixel_format = tdl_app::PixelFormat::ARGB8888;
  std::uint32_t ui_color = 0x80FF2020u;
  int ui_move_step_x = 0;
  int ui_move_step_y = 0;
  int ui_color_step = 0;
  int ui_update_ms = 33;
  bool pip = false;
  std::string pip_mode = "gfx";
  int pip_group = 0;
  int pip_channel = 2;
  int pip_handle = 121;
  int pip_layer = 1;
  int pip_width = 640;
  int pip_height = 480;
  int pip_x = 80;
  int pip_y = 80;
  int pip_pixel_format = 18;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_camera_vo_demo [--backend vi|vpss]\n"
      << "                     [default: dual-os existing MMF path]\n"
      << "                     [--use-mmf | --use-sensor-media]\n"
      << "                     [--attach-existing]\n"
      << "                     [--sensor-ini FILE] [--frames N]\n"
      << "                     [--device N] [--group N] [--pipe N] [--channel N]\n"
      << "                     [--width N] [--height N] [--pixel-format N]\n"
      << "                     [--timeout-ms N] [--hold-ms N]\n"
      << "                     [--vo-dev N] [--layer N] [--vo-chn N]\n"
      << "                     [--screen-width N] [--screen-height N]\n"
      << "                     [--interface-type N] [--interface-sync N]\n"
      << "                     [--preview-fps N]\n"
      << "                     [--ui-osd] [--ui-handle N]\n"
      << "                     [--ui-width N] [--ui-height N]\n"
      << "                     [--ui-x N] [--ui-y N] [--ui-layer N]\n"
      << "                     [--ui-pixel-format N] [--ui-color 0xHEX]\n"
      << "                     [--ui-move-step-x N] [--ui-move-step-y N]\n"
      << "                     [--ui-color-step N] [--ui-update-ms N]\n"
      << "                     [--pip] [--pip-mode gfx|osd]\n"
      << "                     [--pip-group N] [--pip-channel N]\n"
      << "                     [--pip-handle N] [--pip-layer N]\n"
      << "                     [--pip-width N] [--pip-height N]\n"
      << "                     [--pip-x N] [--pip-y N] [--pip-pixel-format N]\n";
}

bool parseU32(const std::string &text, std::uint32_t *value) {
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 0);
  if (end == text.c_str() || *end != '\0') {
    return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

void fillOsdCanvas(const tdl_app::OsdCanvas &canvas, int pixel_format,
                   std::uint32_t color) {
  if (!canvas.data || canvas.width <= 0 || canvas.height <= 0 || canvas.stride <= 0) {
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

std::uint32_t nextUiColor(const Options &opt, int frame_index) {
  if (opt.ui_color_step == 0) {
    return opt.ui_color;
  }
  if (opt.ui_pixel_format == tdl_app::PixelFormat::ARGB8888) {
    const std::uint32_t alpha = opt.ui_color & 0xFF000000u;
    const std::uint32_t rgb =
        (opt.ui_color + static_cast<std::uint32_t>(frame_index * opt.ui_color_step)) &
        0x00FFFFFFu;
    return alpha | rgb;
  }
  const int next = static_cast<int>(opt.ui_color & 0xFFFFu) +
                   frame_index * opt.ui_color_step;
  return static_cast<std::uint32_t>(next & 0xFFFF);
}

bool frameToBgra(const tdl_app::Frame &frame, cv::Mat *bgra, std::string *error) {
  if (!bgra) {
    if (error) *error = "bgra output pointer is null";
    return false;
  }
  if (!frame.native) {
    if (error) *error = "frame has no native buffer";
    return false;
  }

  auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
  const auto &vf = video->stVFrame;
  const int width = static_cast<int>(vf.u32Width);
  const int height = static_cast<int>(vf.u32Height);
  const int format = static_cast<int>(vf.enPixelFormat);
  if (width <= 0 || height <= 0) {
    if (error) *error = "invalid frame size";
    return false;
  }
  std::size_t map_size = 0;
  for (int i = 0; i < 3; ++i) {
    map_size += vf.u32Length[i];
  }
  if (map_size == 0) {
    if (error) *error = "frame buffer length is zero";
    return false;
  }

  auto *mapped =
      static_cast<unsigned char *>(CVI_SYS_Mmap(vf.u64PhyAddr[0], map_size));
  if (!mapped) {
    if (error) *error = "CVI_SYS_Mmap failed";
    return false;
  }
  CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], mapped, map_size);

  if (format == PIXEL_FORMAT_ARGB_8888) {
    cv::Mat argb(height, width, CV_8UC4, mapped,
                 static_cast<std::size_t>(vf.u32Stride[0]));
    if (vf.u32Stride[0] == static_cast<CVI_U32>(width * 4)) {
      argb.copyTo(*bgra);
    } else {
      bgra->create(height, width, CV_8UC4);
      for (int y = 0; y < height; ++y) {
        std::memcpy(bgra->ptr(y), argb.ptr(y), static_cast<std::size_t>(width) * 4);
      }
    }
    CVI_SYS_Munmap(mapped, map_size);
    return true;
  }

  if (format == PIXEL_FORMAT_RGB_888 || format == PIXEL_FORMAT_BGR_888) {
    cv::Mat rgb(height, width, CV_8UC3, mapped,
                static_cast<std::size_t>(vf.u32Stride[0]));
    const int to_bgra = format == PIXEL_FORMAT_RGB_888 ? cv::COLOR_RGB2BGRA
                                                       : cv::COLOR_BGR2BGRA;
    cv::cvtColor(rgb, *bgra, to_bgra);
    CVI_SYS_Munmap(mapped, map_size);
    return true;
  }

  if (format != PIXEL_FORMAT_NV12 && format != PIXEL_FORMAT_NV21) {
    if (error) {
      *error = "unsupported pip frame format: " + std::to_string(format) +
               ", expected NV12/NV21/RGB888/BGR888/ARGB8888";
    }
    CVI_SYS_Munmap(mapped, map_size);
    return false;
  }

  unsigned char *y_base = mapped;
  unsigned char *uv_base = mapped + vf.u32Length[0];
  cv::Mat y_plane(height, width, CV_8UC1, y_base,
                  static_cast<std::size_t>(vf.u32Stride[0]));
  cv::Mat uv_plane(height / 2, width / 2, CV_8UC2, uv_base,
                   static_cast<std::size_t>(vf.u32Stride[1]));
  const int to_bgra = format == PIXEL_FORMAT_NV21 ? cv::COLOR_YUV2BGRA_NV21
                                                  : cv::COLOR_YUV2BGRA_NV12;
  cv::cvtColorTwoPlane(y_plane, uv_plane, *bgra, to_bgra);
  CVI_SYS_Munmap(mapped, map_size);
  return true;
}

bool blitToGraphicLayer(const cv::Mat &bgra, tdl_app::GraphicVoLayer *layer,
                        std::string *error) {
  if (!layer || !layer->isOpen()) {
    if (error) *error = "graphic layer is not open";
    return false;
  }
  tdl_app::GraphicVoLayer::BufferView view = layer->buffer();
  if (!view.data || view.width <= 0 || view.height <= 0 || view.stride <= 0 ||
      view.bytes_per_pixel != 4) {
    if (error) *error = "invalid graphic layer buffer";
    return false;
  }

  cv::Mat resized;
  if (bgra.cols != view.width || bgra.rows != view.height) {
    cv::resize(bgra, resized, cv::Size(view.width, view.height), 0, 0, cv::INTER_LINEAR);
  }
  const cv::Mat &src = resized.empty() ? bgra : resized;

  for (int y = 0; y < view.height; ++y) {
    std::memcpy(static_cast<std::uint8_t *>(view.data) + y * view.stride,
                src.ptr(y), static_cast<std::size_t>(view.width) * 4);
  }
  return layer->present(error);
}

bool blitToOsdCanvas(const cv::Mat &bgra, const tdl_app::OsdCanvas &canvas,
                     std::string *error) {
  if (!canvas.data || canvas.width <= 0 || canvas.height <= 0 || canvas.stride <= 0) {
    if (error) *error = "invalid osd canvas";
    return false;
  }
  if (canvas.pixel_format != tdl_app::PixelFormat::ARGB8888) {
    if (error) *error = "pip osd currently requires ARGB8888 canvas";
    return false;
  }

  cv::Mat resized;
  if (bgra.cols != canvas.width || bgra.rows != canvas.height) {
    cv::resize(bgra, resized, cv::Size(canvas.width, canvas.height), 0, 0,
               cv::INTER_LINEAR);
  }
  const cv::Mat &src = resized.empty() ? bgra : resized;

  for (int y = 0; y < canvas.height; ++y) {
    std::memcpy(static_cast<std::uint8_t *>(canvas.data) + y * canvas.stride,
                src.ptr(y), static_cast<std::size_t>(canvas.width) * 4);
  }
  return true;
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
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
    } else if (arg == "--preview-fps") {
      const char *v = value("--preview-fps");
      if (!v) return false;
      opt->preview_fps = std::atoi(v);
    } else if (arg == "--ui-osd") {
      opt->ui_osd = true;
    } else if (arg == "--ui-handle") {
      const char *v = value("--ui-handle");
      if (!v) return false;
      opt->ui_handle = std::atoi(v);
    } else if (arg == "--ui-width") {
      const char *v = value("--ui-width");
      if (!v) return false;
      opt->ui_width = std::atoi(v);
    } else if (arg == "--ui-height") {
      const char *v = value("--ui-height");
      if (!v) return false;
      opt->ui_height = std::atoi(v);
    } else if (arg == "--ui-x") {
      const char *v = value("--ui-x");
      if (!v) return false;
      opt->ui_x = std::atoi(v);
    } else if (arg == "--ui-y") {
      const char *v = value("--ui-y");
      if (!v) return false;
      opt->ui_y = std::atoi(v);
    } else if (arg == "--ui-layer") {
      const char *v = value("--ui-layer");
      if (!v) return false;
      opt->ui_layer = std::atoi(v);
    } else if (arg == "--ui-pixel-format") {
      const char *v = value("--ui-pixel-format");
      if (!v) return false;
      opt->ui_pixel_format = std::atoi(v);
    } else if (arg == "--ui-color") {
      const char *v = value("--ui-color");
      if (!v) return false;
      if (!parseU32(v, &opt->ui_color)) {
        std::cerr << "invalid value for --ui-color\n";
        return false;
      }
    } else if (arg == "--ui-move-step-x") {
      const char *v = value("--ui-move-step-x");
      if (!v) return false;
      opt->ui_move_step_x = std::atoi(v);
    } else if (arg == "--ui-move-step-y") {
      const char *v = value("--ui-move-step-y");
      if (!v) return false;
      opt->ui_move_step_y = std::atoi(v);
    } else if (arg == "--ui-color-step") {
      const char *v = value("--ui-color-step");
      if (!v) return false;
      opt->ui_color_step = std::atoi(v);
    } else if (arg == "--ui-update-ms") {
      const char *v = value("--ui-update-ms");
      if (!v) return false;
      opt->ui_update_ms = std::atoi(v);
    } else if (arg == "--pip") {
      opt->pip = true;
    } else if (arg == "--pip-mode") {
      const char *v = value("--pip-mode");
      if (!v) return false;
      opt->pip_mode = v;
    } else if (arg == "--pip-group") {
      const char *v = value("--pip-group");
      if (!v) return false;
      opt->pip_group = std::atoi(v);
    } else if (arg == "--pip-channel") {
      const char *v = value("--pip-channel");
      if (!v) return false;
      opt->pip_channel = std::atoi(v);
    } else if (arg == "--pip-handle") {
      const char *v = value("--pip-handle");
      if (!v) return false;
      opt->pip_handle = std::atoi(v);
    } else if (arg == "--pip-layer") {
      const char *v = value("--pip-layer");
      if (!v) return false;
      opt->pip_layer = std::atoi(v);
    } else if (arg == "--pip-width") {
      const char *v = value("--pip-width");
      if (!v) return false;
      opt->pip_width = std::atoi(v);
    } else if (arg == "--pip-height") {
      const char *v = value("--pip-height");
      if (!v) return false;
      opt->pip_height = std::atoi(v);
    } else if (arg == "--pip-x") {
      const char *v = value("--pip-x");
      if (!v) return false;
      opt->pip_x = std::atoi(v);
    } else if (arg == "--pip-y") {
      const char *v = value("--pip-y");
      if (!v) return false;
      opt->pip_y = std::atoi(v);
    } else if (arg == "--pip-pixel-format") {
      const char *v = value("--pip-pixel-format");
      if (!v) return false;
      opt->pip_pixel_format = std::atoi(v);
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

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  if (opt.camera.frames <= 0) {
    opt.camera.frames = std::numeric_limits<int>::max();
  }
  if (opt.preview_fps <= 0) {
    opt.preview_fps = 30;
  }
  if (opt.pip_mode != "gfx" && opt.pip_mode != "osd") {
    std::cerr << "unsupported --pip-mode: " << opt.pip_mode << "\n";
    return 1;
  }

  opt.camera.enable_preview_output = true;
  opt.camera.preview_group = opt.camera.group;
  opt.camera.preview_channel = 1;
  opt.camera.preview_width = opt.screen_width > 0 ? opt.screen_width : opt.camera.width;
  opt.camera.preview_height =
      opt.screen_height > 0 ? opt.screen_height : opt.camera.height;
  opt.camera.preview_pixel_format = opt.camera.pixel_format;
  if (opt.pip) {
    opt.camera.enable_pip_output = true;
    opt.camera.pip_group = opt.pip_group;
    opt.camera.pip_channel = opt.pip_channel;
    opt.camera.pip_width = opt.pip_width;
    opt.camera.pip_height = opt.pip_height;
    opt.camera.pip_pixel_format = opt.pip_pixel_format;
  }
  std::string error;
  camera_demo_support::CameraRuntime runtime;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &runtime, &error)) {
    std::cerr << "camera runtime open failed: " << error << "\n";
    return 2;
  }

  const tdl_app::Camera::Config &camera_config = runtime.camera.config();
  const int screen_width = opt.screen_width > 0 ? opt.screen_width : camera_config.width;
  const int screen_height = opt.screen_height > 0 ? opt.screen_height : camera_config.height;

  std::cerr << "camera_vo(bind): backend="
            << camera_demo_support::backendName(camera_config.backend)
            << " pipe=" << camera_config.pipe
            << " group=" << camera_config.group
            << " channel=" << camera_config.channel
            << " size=" << camera_config.width << "x" << camera_config.height
            << " format=" << camera_config.pixel_format
            << " screen=" << screen_width << "x" << screen_height << "\n";

  if (opt.camera.hold_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(opt.camera.hold_ms));
  }

  tdl_app::VoOutput::Config vo_config;
  vo_config.device = opt.vo_dev;
  vo_config.layer = opt.layer;
  vo_config.channel = opt.vo_chn;
  vo_config.width = screen_width;
  vo_config.height = screen_height;
  vo_config.pixel_format = camera_config.pixel_format;
  vo_config.interface_type = opt.interface_type;
  vo_config.interface_sync = opt.interface_sync;

  tdl_app::VoOutput vo(vo_config);
  if (!vo.open(&error)) {
    std::cerr << "vo open failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 3;
  }

  tdl_app::MediaLink::Config link_config;
  link_config.source = camera_demo_support::previewChannel(opt.camera, camera_config);
  link_config.destination = tdl_app::MediaChannel::vo(opt.layer, opt.vo_chn);
  tdl_app::MediaLink preview_link(link_config);
  std::cerr << "camera_vo(bind): preview "
            << camera_demo_support::backendName(camera_config.backend)
            << " source_group=" << opt.camera.preview_group
            << " source_channel=" << opt.camera.preview_channel
            << " -> VO layer=" << opt.layer
            << " chn=" << opt.vo_chn << "\n";
  if (!preview_link.bind(&error)) {
    std::cerr << "preview bind failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 4;
  }

  std::unique_ptr<tdl_app::GraphicVoLayer> pip_graphic;
  std::unique_ptr<tdl_app::OsdRegion> pip_region;
  std::unique_ptr<tdl_app::Camera> pip_camera;
  if (opt.pip) {
    if (opt.pip_mode == "gfx") {
      tdl_app::GraphicVoLayer::Config gfx_config =
          tdl_app::GraphicVoLayer::argb8888(0, opt.pip_width, opt.pip_height, "/dev/fb0");
      gfx_config.x = opt.pip_x;
      gfx_config.y = opt.pip_y;
      gfx_config.display_width = opt.pip_width;
      gfx_config.display_height = opt.pip_height;
      gfx_config.screen_width = screen_width;
      gfx_config.screen_height = screen_height;
      gfx_config.show = true;
      gfx_config.double_buffer = true;
      pip_graphic.reset(new tdl_app::GraphicVoLayer(gfx_config));
      if (!pip_graphic->open(&error)) {
        std::cerr << "pip graphic open failed: " << error << "\n";
        preview_link.unbind();
        camera_demo_support::closeCameraRuntime(&runtime);
        return 12;
      }
    } else {
      tdl_app::OsdRegion::Config pip_osd_config =
          tdl_app::OsdRegion::canvas(opt.pip_handle, opt.pip_width, opt.pip_height,
                                     tdl_app::PixelFormat::ARGB8888, 2, 0);
      pip_region.reset(new tdl_app::OsdRegion(pip_osd_config));
      if (!pip_region->create(&error)) {
        std::cerr << "pip osd create failed: " << error << "\n";
        preview_link.unbind();
        camera_demo_support::closeCameraRuntime(&runtime);
        return 12;
      }
      if (!pip_region->attach(camera_demo_support::previewChannel(opt.camera, camera_config),
                              opt.pip_x, opt.pip_y, opt.pip_layer, &error)) {
        std::cerr << "pip osd attach failed: " << error << "\n";
        pip_region->destroy();
        preview_link.unbind();
        camera_demo_support::closeCameraRuntime(&runtime);
        return 13;
      }
    }

    tdl_app::Camera::Config pip_camera_config =
        tdl_app::Camera::vpss(opt.pip_group, opt.pip_channel, opt.pip_width,
                              opt.pip_height, opt.pip_pixel_format,
                              opt.camera.timeout_ms);
    pip_camera_config.device = opt.camera.device;
    pip_camera.reset(new tdl_app::Camera(pip_camera_config));
    if (!pip_camera->open(&error)) {
      std::cerr << "pip camera open failed: " << error << "\n";
      if (pip_region) {
        pip_region->detach();
        pip_region->destroy();
      }
      if (pip_graphic) pip_graphic->close();
      preview_link.unbind();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 14;
    }
    std::cerr << "camera_vo(pip): mode=" << opt.pip_mode
              << " source=VPSS grp=" << pip_camera_config.group
              << " ch=" << pip_camera_config.channel
              << " fmt=" << pip_camera_config.pixel_format
              << " rect=" << opt.pip_x << "," << opt.pip_y
              << " " << opt.pip_width << "x" << opt.pip_height << "\n";
  }

  std::unique_ptr<tdl_app::OsdRegion> ui_region;
  if (opt.ui_osd) {
    tdl_app::OsdRegion::Config osd_config =
        tdl_app::OsdRegion::canvas(opt.ui_handle, opt.ui_width, opt.ui_height,
                                   opt.ui_pixel_format, 2, 0);
    ui_region.reset(new tdl_app::OsdRegion(osd_config));
    if (!ui_region->create(&error)) {
      std::cerr << "ui osd create failed: " << error << "\n";
      preview_link.unbind();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 5;
    }
    if (!ui_region->attach(camera_demo_support::previewChannel(opt.camera, camera_config),
                           opt.ui_x, opt.ui_y, opt.ui_layer, &error)) {
      std::cerr << "ui osd attach failed: " << error << "\n";
      ui_region->destroy();
      preview_link.unbind();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 6;
    }

    tdl_app::OsdCanvas canvas;
    if (!ui_region->getCanvas(&canvas, &error)) {
      std::cerr << "ui osd getCanvas failed: " << error << "\n";
      ui_region->detach();
      ui_region->destroy();
      preview_link.unbind();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 7;
    }
    fillOsdCanvas(canvas, opt.ui_pixel_format, opt.ui_color);
    if (!ui_region->updateCanvas(&error)) {
      std::cerr << "ui osd updateCanvas failed: " << error << "\n";
      ui_region->detach();
      ui_region->destroy();
      preview_link.unbind();
      camera_demo_support::closeCameraRuntime(&runtime);
      return 8;
    }
  }

  const bool infinite = (opt.camera.frames == std::numeric_limits<int>::max());
  const int total_ms = infinite ? -1 : (opt.camera.frames * 1000) / opt.preview_fps;
  std::cout << "preview bind ok: source="
            << camera_demo_support::backendName(camera_config.backend)
            << " duration_ms=" << total_ms
            << " press Ctrl+C to stop if needed\n";

  if (opt.ui_osd || opt.pip) {
    const int loop_count = infinite ? std::numeric_limits<int>::max() : opt.camera.frames;
    std::uint64_t total_ui_us = 0;
    int ui_updates = 0;
    int cur_x = opt.ui_x;
    int cur_y = opt.ui_y;
    for (int i = 0; i < loop_count; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      if (opt.pip) {
        tdl_app::Frame pip_frame;
        if (!pip_camera->read(&pip_frame, &error)) {
          std::cerr << "pip camera read failed: " << error << "\n";
          if (ui_region) {
            ui_region->detach();
            ui_region->destroy();
          }
          if (pip_region) {
            pip_region->detach();
            pip_region->destroy();
          }
          if (pip_camera) pip_camera->close();
          if (pip_graphic) pip_graphic->close();
          preview_link.unbind();
          camera_demo_support::closeCameraRuntime(&runtime);
          return 15;
        }
        cv::Mat bgra;
        if (!frameToBgra(pip_frame, &bgra, &error)) {
          std::cerr << "pip frame convert failed: " << error << "\n";
          if (ui_region) {
            ui_region->detach();
            ui_region->destroy();
          }
          if (pip_region) {
            pip_region->detach();
            pip_region->destroy();
          }
          if (pip_camera) pip_camera->close();
          if (pip_graphic) pip_graphic->close();
          preview_link.unbind();
          camera_demo_support::closeCameraRuntime(&runtime);
          return 16;
        }
        if (pip_graphic) {
          if (!blitToGraphicLayer(bgra, pip_graphic.get(), &error)) {
            std::cerr << "pip graphic present failed: " << error << "\n";
            if (ui_region) {
              ui_region->detach();
              ui_region->destroy();
            }
            if (pip_region) {
              pip_region->detach();
              pip_region->destroy();
            }
            if (pip_camera) pip_camera->close();
            if (pip_graphic) pip_graphic->close();
            preview_link.unbind();
            camera_demo_support::closeCameraRuntime(&runtime);
            return 17;
          }
        } else if (pip_region) {
          tdl_app::OsdCanvas pip_canvas;
          if (!pip_region->getCanvas(&pip_canvas, &error)) {
            std::cerr << "pip osd getCanvas failed: " << error << "\n";
            if (ui_region) {
              ui_region->detach();
              ui_region->destroy();
            }
            pip_region->detach();
            pip_region->destroy();
            if (pip_camera) pip_camera->close();
            preview_link.unbind();
            camera_demo_support::closeCameraRuntime(&runtime);
            return 18;
          }
          if (!blitToOsdCanvas(bgra, pip_canvas, &error)) {
            std::cerr << "pip osd copy failed: " << error << "\n";
            if (ui_region) {
              ui_region->detach();
              ui_region->destroy();
            }
            pip_region->detach();
            pip_region->destroy();
            if (pip_camera) pip_camera->close();
            preview_link.unbind();
            camera_demo_support::closeCameraRuntime(&runtime);
            return 19;
          }
          if (!pip_region->updateCanvas(&error)) {
            std::cerr << "pip osd updateCanvas failed: " << error << "\n";
            if (ui_region) {
              ui_region->detach();
              ui_region->destroy();
            }
            pip_region->detach();
            pip_region->destroy();
            if (pip_camera) pip_camera->close();
            preview_link.unbind();
            camera_demo_support::closeCameraRuntime(&runtime);
            return 20;
          }
        }
      }

      if (opt.ui_osd) {
      if (opt.ui_move_step_x != 0 || opt.ui_move_step_y != 0) {
        cur_x += opt.ui_move_step_x;
        cur_y += opt.ui_move_step_y;
        if (!ui_region->moveTo(cur_x, cur_y, &error)) {
          std::cerr << "ui osd move failed: " << error << "\n";
          ui_region->detach();
          ui_region->destroy();
          if (pip_region) {
            pip_region->detach();
            pip_region->destroy();
          }
          if (pip_camera) pip_camera->close();
          if (pip_graphic) pip_graphic->close();
          preview_link.unbind();
          camera_demo_support::closeCameraRuntime(&runtime);
          return 9;
        }
      }
      if (opt.ui_color_step != 0) {
        tdl_app::OsdCanvas canvas;
        if (!ui_region->getCanvas(&canvas, &error)) {
          std::cerr << "ui osd getCanvas failed: " << error << "\n";
          ui_region->detach();
          ui_region->destroy();
          if (pip_region) {
            pip_region->detach();
            pip_region->destroy();
          }
          if (pip_camera) pip_camera->close();
          if (pip_graphic) pip_graphic->close();
          preview_link.unbind();
          camera_demo_support::closeCameraRuntime(&runtime);
          return 10;
        }
        fillOsdCanvas(canvas, opt.ui_pixel_format, nextUiColor(opt, i));
        if (!ui_region->updateCanvas(&error)) {
          std::cerr << "ui osd updateCanvas failed: " << error << "\n";
          ui_region->detach();
          ui_region->destroy();
          if (pip_region) {
            pip_region->detach();
            pip_region->destroy();
          }
          if (pip_camera) pip_camera->close();
          if (pip_graphic) pip_graphic->close();
          preview_link.unbind();
          camera_demo_support::closeCameraRuntime(&runtime);
          return 11;
        }
      }
      }
      const auto t1 = std::chrono::steady_clock::now();
      total_ui_us += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
      ++ui_updates;
      if (!infinite && opt.ui_update_ms <= 0) {
        continue;
      }
      const int sleep_ms = opt.ui_update_ms > 0 ? opt.ui_update_ms : (1000 / opt.preview_fps);
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    const double avg_ui_us =
        ui_updates > 0 ? static_cast<double>(total_ui_us) / ui_updates : 0.0;
    std::cout << "overlay summary: updates=" << ui_updates
              << " avg_update_us=" << avg_ui_us << "\n";
    if (ui_region) {
      ui_region->detach();
      ui_region->destroy();
    }
    if (pip_region) {
      pip_region->detach();
      pip_region->destroy();
    }
    if (pip_camera) pip_camera->close();
    if (pip_graphic) pip_graphic->close();
    preview_link.unbind();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 0;
  }

  if (infinite) {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(total_ms));
  if (pip_region) {
    pip_region->detach();
    pip_region->destroy();
  }
  if (pip_camera) pip_camera->close();
  if (pip_graphic) pip_graphic->close();
  preview_link.unbind();
  camera_demo_support::closeCameraRuntime(&runtime);
  return 0;
}
