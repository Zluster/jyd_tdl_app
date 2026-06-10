#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  std::string device = "/dev/fb0";
  int x = 20;
  int y = 20;
  int width = 240;
  int height = 160;
  int screen_width = 640;
  int screen_height = 960;
  std::uint32_t color = 0xCC20A0FFU;
  std::uint32_t color2 = 0xCCFFB020U;
  std::string mode = "checker";
  int frames = 1;
  int interval_ms = 200;
  bool hide = false;
  bool double_buffer = false;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_graphic_vo_demo [--device /dev/fb0] [--x N] [--y N]\n"
      << "                      [--width N] [--height N]\n"
      << "                      [--screen-width N] [--screen-height N]\n"
      << "                      [--color 0xAARRGGBB] [--color2 0xAARRGGBB]\n"
      << "                      [--mode solid|checker|border|gradient|move]\n"
      << "                      [--frames N] [--interval-ms N]\n"
      << "                      [--double-buffer] [--hide]\n";
}

bool parseColor(const std::string &text, std::uint32_t *value) {
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(text.c_str(), &end, 0);
  if (end == text.c_str() || *end != '\0') return false;
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

    if (arg == "--device") {
      const char *v = value("--device");
      if (!v) return false;
      opt->device = v;
    } else if (arg == "--x") {
      const char *v = value("--x");
      if (!v) return false;
      opt->x = std::atoi(v);
    } else if (arg == "--y") {
      const char *v = value("--y");
      if (!v) return false;
      opt->y = std::atoi(v);
    } else if (arg == "--width") {
      const char *v = value("--width");
      if (!v) return false;
      opt->width = std::atoi(v);
    } else if (arg == "--height") {
      const char *v = value("--height");
      if (!v) return false;
      opt->height = std::atoi(v);
    } else if (arg == "--screen-width") {
      const char *v = value("--screen-width");
      if (!v) return false;
      opt->screen_width = std::atoi(v);
    } else if (arg == "--screen-height") {
      const char *v = value("--screen-height");
      if (!v) return false;
      opt->screen_height = std::atoi(v);
    } else if (arg == "--color") {
      const char *v = value("--color");
      if (!v) return false;
      if (!parseColor(v, &opt->color)) {
        std::cerr << "invalid color: " << v << "\n";
        return false;
      }
    } else if (arg == "--color2") {
      const char *v = value("--color2");
      if (!v) return false;
      if (!parseColor(v, &opt->color2)) {
        std::cerr << "invalid color2: " << v << "\n";
        return false;
      }
    } else if (arg == "--mode") {
      const char *v = value("--mode");
      if (!v) return false;
      opt->mode = v;
    } else if (arg == "--frames") {
      const char *v = value("--frames");
      if (!v) return false;
      opt->frames = std::atoi(v);
    } else if (arg == "--interval-ms") {
      const char *v = value("--interval-ms");
      if (!v) return false;
      opt->interval_ms = std::atoi(v);
    } else if (arg == "--double-buffer") {
      opt->double_buffer = true;
    } else if (arg == "--hide") {
      opt->hide = true;
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  if (opt->width < 1) opt->width = 1;
  if (opt->height < 1) opt->height = 1;
  if (opt->frames < 1) opt->frames = 1;
  if (opt->interval_ms < 0) opt->interval_ms = 0;
  return true;
}

inline std::uint8_t alphaOf(std::uint32_t argb) {
  return static_cast<std::uint8_t>((argb >> 24) & 0xFFU);
}

inline std::uint8_t redOf(std::uint32_t argb) {
  return static_cast<std::uint8_t>((argb >> 16) & 0xFFU);
}

inline std::uint8_t greenOf(std::uint32_t argb) {
  return static_cast<std::uint8_t>((argb >> 8) & 0xFFU);
}

inline std::uint8_t blueOf(std::uint32_t argb) {
  return static_cast<std::uint8_t>(argb & 0xFFU);
}

inline std::uint32_t makeArgb(std::uint8_t a, std::uint8_t r, std::uint8_t g,
                              std::uint8_t b) {
  return (static_cast<std::uint32_t>(a) << 24) |
         (static_cast<std::uint32_t>(r) << 16) |
         (static_cast<std::uint32_t>(g) << 8) |
         static_cast<std::uint32_t>(b);
}

void fillSolid(tdl_app::GraphicVoLayer::BufferView view, std::uint32_t color) {
  for (int y = 0; y < view.height; ++y) {
    auto *row = reinterpret_cast<std::uint32_t *>(
        static_cast<std::uint8_t *>(view.data) + y * view.stride);
    for (int x = 0; x < view.width; ++x) row[x] = color;
  }
}

void fillChecker(tdl_app::GraphicVoLayer::BufferView view, std::uint32_t c0,
                 std::uint32_t c1, int frame_index) {
  const int step = 24;
  const int phase = frame_index % step;
  for (int y = 0; y < view.height; ++y) {
    auto *row = reinterpret_cast<std::uint32_t *>(
        static_cast<std::uint8_t *>(view.data) + y * view.stride);
    for (int x = 0; x < view.width; ++x) {
      const int tile = ((x + phase) / step + (y + phase) / step) & 1;
      row[x] = tile ? c0 : c1;
    }
  }
}

void fillBorder(tdl_app::GraphicVoLayer::BufferView view, std::uint32_t bg,
                std::uint32_t fg, int frame_index) {
  fillSolid(view, bg);
  const int border = 6 + (frame_index % 36);
  for (int y = 0; y < view.height; ++y) {
    auto *row = reinterpret_cast<std::uint32_t *>(
        static_cast<std::uint8_t *>(view.data) + y * view.stride);
    const bool active_y = (y < border) || (y + border >= view.height);
    for (int x = 0; x < view.width; ++x) {
      const bool active_x = (x < border) || (x + border >= view.width);
      if (active_x || active_y) row[x] = fg;
    }
  }
}

void fillGradient(tdl_app::GraphicVoLayer::BufferView view, int frame_index) {
  for (int y = 0; y < view.height; ++y) {
    auto *row = reinterpret_cast<std::uint32_t *>(
        static_cast<std::uint8_t *>(view.data) + y * view.stride);
    for (int x = 0; x < view.width; ++x) {
      const std::uint8_t a = static_cast<std::uint8_t>(96 + ((x + frame_index * 3) & 0x7F));
      const std::uint8_t r = static_cast<std::uint8_t>((x * 255) / std::max(1, view.width - 1));
      const std::uint8_t g = static_cast<std::uint8_t>((y * 255) / std::max(1, view.height - 1));
      const std::uint8_t b = static_cast<std::uint8_t>((x + y + frame_index * 8) & 0xFF);
      row[x] = makeArgb(a, r, g, b);
    }
  }
}

void fillMove(tdl_app::GraphicVoLayer::BufferView view, std::uint32_t bg,
              std::uint32_t fg, int frame_index) {
  fillSolid(view, bg);
  const int box_w = std::max(24, view.width / 4);
  const int box_h = std::max(24, view.height / 4);
  const int max_x = std::max(1, view.width - box_w);
  const int max_y = std::max(1, view.height - box_h);
  const int x0 = (frame_index * 11) % max_x;
  const int y0 = (frame_index * 7) % max_y;
  for (int y = 0; y < box_h; ++y) {
    auto *row = reinterpret_cast<std::uint32_t *>(
        static_cast<std::uint8_t *>(view.data) + (y0 + y) * view.stride);
    for (int x = 0; x < box_w; ++x) row[x0 + x] = fg;
  }
}

void fillByMode(tdl_app::GraphicVoLayer::BufferView view, const Options &opt,
                int frame_index) {
  if (opt.mode == "solid") {
    fillSolid(view, opt.color);
  } else if (opt.mode == "border") {
    fillBorder(view, opt.color, opt.color2, frame_index);
  } else if (opt.mode == "gradient") {
    fillGradient(view, frame_index);
  } else if (opt.mode == "move") {
    fillMove(view, opt.color, opt.color2, frame_index);
  } else {
    fillChecker(view, opt.color, opt.color2, frame_index);
  }
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::string error;
  tdl_app::GraphicVoLayer::Config config =
      tdl_app::GraphicVoLayer::argb8888(0, opt.width, opt.height, opt.device);
  config.x = opt.x;
  config.y = opt.y;
  config.show = !opt.hide;
  config.double_buffer = opt.double_buffer;
  config.display_width = opt.width;
  config.display_height = opt.height;
  config.screen_width = opt.screen_width;
  config.screen_height = opt.screen_height;

  tdl_app::GraphicVoLayer layer(config);
  if (!layer.open(&error)) {
    std::cerr << "graphic vo open failed: " << error << "\n";
    return 2;
  }

  const auto view = layer.buffer();
  if (!view.data || view.bytes_per_pixel != 4) {
    std::cerr << "graphic vo invalid buffer view\n";
    return 3;
  }

  using clock = std::chrono::steady_clock;
  std::uint64_t total_fill_us = 0;
  std::uint64_t total_present_us = 0;
  const auto begin_all = clock::now();

  for (int i = 0; i < opt.frames; ++i) {
    const auto begin_fill = clock::now();
    fillByMode(view, opt, i);
    const auto end_fill = clock::now();

    const auto begin_present = clock::now();
    if (!layer.present(&error)) {
      std::cerr << "graphic vo present failed: " << error << "\n";
      return 4;
    }
    const auto end_present = clock::now();

    total_fill_us += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_fill - begin_fill).count());
    total_present_us += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_present - begin_present).count());

    if (opt.interval_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));
    }
  }

  const auto end_all = clock::now();
  const auto total_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end_all - begin_all).count());
  const double fps =
      total_us > 0 ? (static_cast<double>(opt.frames) * 1000000.0 / static_cast<double>(total_us))
                   : 0.0;

  std::cout << "graphic_vo:"
            << " device=" << opt.device
            << " pos=(" << opt.x << "," << opt.y << ")"
            << " size=" << view.width << "x" << view.height
            << " stride=" << view.stride
            << " bpp=" << (view.bytes_per_pixel * 8)
            << " mode=" << opt.mode
            << " alpha0=" << static_cast<int>(alphaOf(opt.color))
            << " alpha1=" << static_cast<int>(alphaOf(opt.color2))
            << " frames=" << opt.frames
            << " total_ms=" << (total_us / 1000.0)
            << " fps=" << fps
            << " avg_fill_us=" << (opt.frames > 0 ? total_fill_us / opt.frames : 0)
            << " avg_present_us=" << (opt.frames > 0 ? total_present_us / opt.frames : 0)
            << "\n";

  return 0;
}
