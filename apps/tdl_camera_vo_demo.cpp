#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>

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
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_camera_vo_demo [--backend vi|vpss]\n"
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
      << "                     [--ui-color-step N] [--ui-update-ms N]\n";
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

  opt.camera.enable_preview_output = true;
  opt.camera.preview_group = opt.camera.group;
  opt.camera.preview_channel = 1;
  opt.camera.preview_width = opt.screen_width > 0 ? opt.screen_width : opt.camera.width;
  opt.camera.preview_height =
      opt.screen_height > 0 ? opt.screen_height : opt.camera.height;
  opt.camera.preview_pixel_format = opt.camera.pixel_format;

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

  if (opt.ui_osd) {
    const int loop_count = infinite ? std::numeric_limits<int>::max() : opt.camera.frames;
    std::uint64_t total_ui_us = 0;
    int ui_updates = 0;
    int cur_x = opt.ui_x;
    int cur_y = opt.ui_y;
    for (int i = 0; i < loop_count; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      if (opt.ui_move_step_x != 0 || opt.ui_move_step_y != 0) {
        cur_x += opt.ui_move_step_x;
        cur_y += opt.ui_move_step_y;
        if (!ui_region->moveTo(cur_x, cur_y, &error)) {
          std::cerr << "ui osd move failed: " << error << "\n";
          ui_region->detach();
          ui_region->destroy();
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
          preview_link.unbind();
          camera_demo_support::closeCameraRuntime(&runtime);
          return 10;
        }
        fillOsdCanvas(canvas, opt.ui_pixel_format, nextUiColor(opt, i));
        if (!ui_region->updateCanvas(&error)) {
          std::cerr << "ui osd updateCanvas failed: " << error << "\n";
          ui_region->detach();
          ui_region->destroy();
          preview_link.unbind();
          camera_demo_support::closeCameraRuntime(&runtime);
          return 11;
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
    std::cout << "ui osd summary: updates=" << ui_updates
              << " avg_update_us=" << avg_ui_us << "\n";
    ui_region->detach();
    ui_region->destroy();
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
  preview_link.unbind();
  camera_demo_support::closeCameraRuntime(&runtime);
  return 0;
}
