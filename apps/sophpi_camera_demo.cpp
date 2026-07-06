#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "cvi_sys.h"
#include "tdl_app/media_link.hpp"
#include "tdl_app/osd_region.hpp"
#include "tdl_app/vo_output.hpp"

namespace {

enum class DemoMode {
  Live,
  Gallery,
};

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string source = "live";
  int vo_dev = 0;
  int layer = 0;
  int vo_chn = 0;
  int screen_width = 720;
  int screen_height = 1280;
  int interface_type = tdl_app::VoInterfaceType::Mipi;
  int interface_sync = tdl_app::VoInterfaceSync::P720_1280_60;
  std::string album_dir = "./album";
};

struct DemoState {
  DemoMode mode = DemoMode::Live;
  std::vector<std::string> photos;
  int index = -1;
  std::string status = "ready";
};

struct TerminalGuard {
  bool ok = false;
#ifdef _WIN32
#else
  termios old_attr {};
#endif
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  sophpi_camera_demo [--source live|ai|main|subrgb|screen]\n"
      << "                     [--album-dir DIR]\n"
      << "                     [--screen-width N] [--screen-height N]\n";
}

bool hasSuffixCaseInsensitive(const std::string &value,
                              const std::string &suffix) {
  if (value.size() < suffix.size()) {
    return false;
  }
  const std::size_t offset = value.size() - suffix.size();
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    const char a = static_cast<char>(
        std::tolower(static_cast<unsigned char>(value[offset + i])));
    const char b =
        static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
    if (a != b) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> scanAlbum(const std::string &dir) {
  std::vector<std::string> files;
  DIR *dp = opendir(dir.c_str());
  if (!dp) {
    return files;
  }
  while (true) {
    dirent *entry = readdir(dp);
    if (!entry) {
      break;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    if (!hasSuffixCaseInsensitive(name, ".jpg") &&
        !hasSuffixCaseInsensitive(name, ".jpeg") &&
        !hasSuffixCaseInsensitive(name, ".bmp") &&
        !hasSuffixCaseInsensitive(name, ".png")) {
      continue;
    }
    files.push_back(dir + "/" + name);
  }
  closedir(dp);
  std::sort(files.begin(), files.end());
  return files;
}

std::string basenameOnly(const std::string &path) {
  const std::size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

bool ensureDir(const std::string &path, std::string *error) {
#ifdef _WIN32
  (void)path;
  if (error) *error = "Windows is not used for target runtime";
  return false;
#else
  DIR *dp = opendir(path.c_str());
  if (dp) {
    closedir(dp);
    return true;
  }
  if (mkdir(path.c_str(), 0755) == 0) {
    return true;
  }
  if (error) {
    *error = "mkdir failed: " + path;
  }
  return false;
#endif
}

bool prepareTerminal(TerminalGuard *guard, std::string *error) {
#ifdef _WIN32
  (void)guard;
  (void)error;
  return true;
#else
  if (!guard) {
    if (error) *error = "terminal guard is null";
    return false;
  }
  if (tcgetattr(STDIN_FILENO, &guard->old_attr) != 0) {
    if (error) *error = "tcgetattr failed";
    return false;
  }
  termios raw = guard->old_attr;
  raw.c_lflag &= static_cast<unsigned int>(~(ICANON | ECHO));
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
    if (error) *error = "tcsetattr failed";
    return false;
  }
  guard->ok = true;
  return true;
#endif
}

void restoreTerminal(TerminalGuard *guard) {
#ifdef _WIN32
  (void)guard;
#else
  if (guard && guard->ok) {
    tcsetattr(STDIN_FILENO, TCSANOW, &guard->old_attr);
    guard->ok = false;
  }
#endif
}

int readKey() {
#ifdef _WIN32
  return -1;
#else
  unsigned char ch = 0;
  const int n = static_cast<int>(::read(STDIN_FILENO, &ch, 1));
  return n == 1 ? static_cast<int>(ch) : -1;
#endif
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
    if (arg == "--source") {
      const char *v = value("--source");
      if (!v) return false;
      opt->source = v;
    } else if (arg == "--album-dir") {
      const char *v = value("--album-dir");
      if (!v) return false;
      opt->album_dir = v;
    } else if (arg == "--screen-width") {
      const char *v = value("--screen-width");
      if (!v) return false;
      opt->screen_width = std::atoi(v);
    } else if (arg == "--screen-height") {
      const char *v = value("--screen-height");
      if (!v) return false;
      opt->screen_height = std::atoi(v);
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

tdl_app::VoOutput::Config makeVoConfig(const Options &opt) {
  tdl_app::VoOutput::Config config;
  config.device = opt.vo_dev;
  config.layer = opt.layer;
  config.channel = opt.vo_chn;
  config.width = opt.screen_width;
  config.height = opt.screen_height;
  config.pixel_format = tdl_app::PixelFormat::NV12;
  config.interface_type = opt.interface_type;
  config.interface_sync = opt.interface_sync;
  return config;
}

tdl_app::OsdRegion::Config makeGalleryOsdConfig(const Options &opt) {
  (void)opt;
  return tdl_app::OsdRegion::canvas(
      122, tdl_app::DualOsLayout::kLiveWidth, tdl_app::DualOsLayout::kLiveHeight,
      tdl_app::PixelFormat::ARGB8888, 2, 0);
}

tdl_app::MediaChannel makeDisplayOutputChannel() {
  return tdl_app::MediaChannel::vpss(tdl_app::DualOsLayout::kDisplayVpssGroup,
                                     tdl_app::DualOsLayout::kDisplayChannel);
}

bool frameToBgra(const tdl_app::Frame &frame, cv::Mat *bgra, std::string *error) {
  if (!bgra) {
    if (error) *error = "bgra output is null";
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
  std::size_t map_size = 0;
  for (int i = 0; i < 3; ++i) {
    map_size += vf.u32Length[i];
  }
  if (width <= 0 || height <= 0 || map_size == 0) {
    if (error) *error = "invalid frame";
    return false;
  }
  auto *mapped =
      static_cast<unsigned char *>(CVI_SYS_Mmap(vf.u64PhyAddr[0], map_size));
  if (!mapped) {
    if (error) *error = "CVI_SYS_Mmap failed";
    return false;
  }
  CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], mapped, map_size);

  bool ok = true;
  if (format == PIXEL_FORMAT_ARGB_8888) {
    cv::Mat argb(height, width, CV_8UC4, mapped,
                 static_cast<std::size_t>(vf.u32Stride[0]));
    argb.copyTo(*bgra);
  } else if (format == PIXEL_FORMAT_RGB_888 || format == PIXEL_FORMAT_BGR_888) {
    cv::Mat packed(height, width, CV_8UC3, mapped,
                   static_cast<std::size_t>(vf.u32Stride[0]));
    const int code = format == PIXEL_FORMAT_RGB_888 ? cv::COLOR_RGB2BGRA
                                                    : cv::COLOR_BGR2BGRA;
    cv::cvtColor(packed, *bgra, code);
  } else if (format == PIXEL_FORMAT_RGB_888_PLANAR ||
             format == PIXEL_FORMAT_BGR_888_PLANAR) {
    cv::Mat plane0(height, width, CV_8UC1);
    cv::Mat plane1(height, width, CV_8UC1);
    cv::Mat plane2(height, width, CV_8UC1);
    unsigned char *base0 = mapped;
    unsigned char *base1 = mapped + vf.u32Length[0];
    unsigned char *base2 = mapped + vf.u32Length[0] + vf.u32Length[1];
    for (int y = 0; y < height; ++y) {
      std::memcpy(plane0.ptr(y), base0 + y * vf.u32Stride[0], width);
      std::memcpy(plane1.ptr(y), base1 + y * vf.u32Stride[1], width);
      std::memcpy(plane2.ptr(y), base2 + y * vf.u32Stride[2], width);
    }
    cv::Mat bgr;
    if (format == PIXEL_FORMAT_RGB_888_PLANAR) {
      cv::merge(std::vector<cv::Mat>{plane2, plane1, plane0}, bgr);
    } else {
      cv::merge(std::vector<cv::Mat>{plane0, plane1, plane2}, bgr);
    }
    cv::cvtColor(bgr, *bgra, cv::COLOR_BGR2BGRA);
  } else if (format == PIXEL_FORMAT_NV12 || format == PIXEL_FORMAT_NV21) {
    unsigned char *y_base = mapped;
    unsigned char *uv_base = mapped + vf.u32Length[0];
    cv::Mat y_plane(height, width, CV_8UC1, y_base,
                    static_cast<std::size_t>(vf.u32Stride[0]));
    cv::Mat uv_plane(height / 2, width / 2, CV_8UC2, uv_base,
                     static_cast<std::size_t>(vf.u32Stride[1]));
    const int code = format == PIXEL_FORMAT_NV21 ? cv::COLOR_YUV2BGRA_NV21
                                                 : cv::COLOR_YUV2BGRA_NV12;
    cv::cvtColorTwoPlane(y_plane, uv_plane, *bgra, code);
  } else if (format == PIXEL_FORMAT_YUV_400) {
    cv::Mat gray(height, width, CV_8UC1);
    for (int y = 0; y < height; ++y) {
      std::memcpy(gray.ptr(y), mapped + y * vf.u32Stride[0], width);
    }
    cv::cvtColor(gray, *bgra, cv::COLOR_GRAY2BGRA);
  } else {
    ok = false;
    if (error) {
      *error = "unsupported frame format for preview: " + std::to_string(format);
    }
  }

  CVI_SYS_Munmap(mapped, map_size);
  return ok;
}

bool blitToOsdCanvas(const cv::Mat &bgr, const tdl_app::OsdCanvas &canvas,
                     std::string *error) {
  if (!canvas.data || canvas.width <= 0 || canvas.height <= 0 ||
      canvas.stride <= 0) {
    if (error) {
      *error = "invalid osd canvas: data=" +
               std::to_string(canvas.data ? 1 : 0) +
               " size=" + std::to_string(canvas.width) + "x" +
               std::to_string(canvas.height) + " stride=" +
               std::to_string(canvas.stride);
    }
    return false;
  }
  if (canvas.pixel_format != tdl_app::PixelFormat::ARGB8888) {
    if (error) *error = "gallery osd requires ARGB8888 canvas";
    return false;
  }

  cv::Mat resized;
  cv::resize(bgr, resized, cv::Size(canvas.width, canvas.height), 0, 0,
             cv::INTER_LINEAR);
  cv::Mat bgra;
  cv::cvtColor(resized, bgra, cv::COLOR_BGR2BGRA);
  for (int y = 0; y < canvas.height; ++y) {
    std::memcpy(static_cast<std::uint8_t *>(canvas.data) + y * canvas.stride,
                bgra.ptr(y), static_cast<std::size_t>(canvas.width) * 4);
  }
  return true;
}

std::uint32_t argb(std::uint8_t a, std::uint8_t r, std::uint8_t g,
                   std::uint8_t b) {
  return (static_cast<std::uint32_t>(a) << 24) |
         (static_cast<std::uint32_t>(r) << 16) |
         (static_cast<std::uint32_t>(g) << 8) | b;
}

const std::uint8_t *glyphRows(char ch) {
  switch (ch) {
    case 'A': {
      static const std::uint8_t rows[] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11,
                                          0x11};
      return rows;
    }
    case 'B': {
      static const std::uint8_t rows[] = {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11,
                                          0x1e};
      return rows;
    }
    case 'C': {
      static const std::uint8_t rows[] = {0x0f, 0x10, 0x10, 0x10, 0x10, 0x10,
                                          0x0f};
      return rows;
    }
    case 'D': {
      static const std::uint8_t rows[] = {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11,
                                          0x1e};
      return rows;
    }
    case 'E': {
      static const std::uint8_t rows[] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10,
                                          0x1f};
      return rows;
    }
    case 'G': {
      static const std::uint8_t rows[] = {0x0f, 0x10, 0x10, 0x17, 0x11, 0x11,
                                          0x0f};
      return rows;
    }
    case 'H': {
      static const std::uint8_t rows[] = {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11,
                                          0x11};
      return rows;
    }
    case 'I': {
      static const std::uint8_t rows[] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04,
                                          0x1f};
      return rows;
    }
    case 'L': {
      static const std::uint8_t rows[] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                                          0x1f};
      return rows;
    }
    case 'M': {
      static const std::uint8_t rows[] = {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11,
                                          0x11};
      return rows;
    }
    case 'N': {
      static const std::uint8_t rows[] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11,
                                          0x11};
      return rows;
    }
    case 'O': {
      static const std::uint8_t rows[] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11,
                                          0x0e};
      return rows;
    }
    case 'P': {
      static const std::uint8_t rows[] = {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10,
                                          0x10};
      return rows;
    }
    case 'Q': {
      static const std::uint8_t rows[] = {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12,
                                          0x0d};
      return rows;
    }
    case 'R': {
      static const std::uint8_t rows[] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12,
                                          0x11};
      return rows;
    }
    case 'T': {
      static const std::uint8_t rows[] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04,
                                          0x04};
      return rows;
    }
    case 'U': {
      static const std::uint8_t rows[] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                                          0x0e};
      return rows;
    }
    case 'V': {
      static const std::uint8_t rows[] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a,
                                          0x04};
      return rows;
    }
    case 'X': {
      static const std::uint8_t rows[] = {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11,
                                          0x11};
      return rows;
    }
    case 'Y': {
      static const std::uint8_t rows[] = {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04,
                                          0x04};
      return rows;
    }
    case '0': {
      static const std::uint8_t rows[] = {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11,
                                          0x0e};
      return rows;
    }
    case '1': {
      static const std::uint8_t rows[] = {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04,
                                          0x0e};
      return rows;
    }
    case '2': {
      static const std::uint8_t rows[] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08,
                                          0x1f};
      return rows;
    }
    case '3': {
      static const std::uint8_t rows[] = {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01,
                                          0x1e};
      return rows;
    }
    case '4': {
      static const std::uint8_t rows[] = {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02,
                                          0x02};
      return rows;
    }
    case '5': {
      static const std::uint8_t rows[] = {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01,
                                          0x1e};
      return rows;
    }
    case '6': {
      static const std::uint8_t rows[] = {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11,
                                          0x0e};
      return rows;
    }
    case '7': {
      static const std::uint8_t rows[] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08,
                                          0x08};
      return rows;
    }
    case '8': {
      static const std::uint8_t rows[] = {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11,
                                          0x0e};
      return rows;
    }
    case '9': {
      static const std::uint8_t rows[] = {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01,
                                          0x0e};
      return rows;
    }
    case ':': {
      static const std::uint8_t rows[] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04,
                                          0x00};
      return rows;
    }
    case '/': {
      static const std::uint8_t rows[] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08,
                                          0x10};
      return rows;
    }
    case ' ': {
      static const std::uint8_t rows[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0x00};
      return rows;
    }
    default: {
      static const std::uint8_t rows[] = {0x1f, 0x01, 0x02, 0x04, 0x04, 0x00,
                                          0x04};
      return rows;
    }
  }
}

void fillRect(std::uint32_t *canvas, int width, int height, int x, int y, int w,
              int h, std::uint32_t color) {
  if (!canvas || width <= 0 || height <= 0) {
    return;
  }
  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(width, x + w);
  const int y1 = std::min(height, y + h);
  for (int py = y0; py < y1; ++py) {
    for (int px = x0; px < x1; ++px) {
      canvas[static_cast<std::size_t>(py * width + px)] = color;
    }
  }
}

void drawChar(std::uint32_t *canvas, int width, int height, int x, int y,
              char ch, std::uint32_t color, int scale = 2) {
  const std::uint8_t *rows = glyphRows(ch);
  for (int row = 0; row < 7; ++row) {
    for (int col = 0; col < 5; ++col) {
      if ((rows[row] & (1 << (4 - col))) == 0) {
        continue;
      }
      fillRect(canvas, width, height, x + col * scale, y + row * scale, scale,
               scale, color);
    }
  }
}

void drawText(std::uint32_t *canvas, int width, int height, int x, int y,
              const std::string &text, std::uint32_t color, int scale = 2) {
  int cursor = x;
  for (char ch : text) {
    drawChar(canvas, width, height, cursor, y,
             static_cast<char>(std::toupper(static_cast<unsigned char>(ch))),
             color, scale);
    cursor += 6 * scale;
  }
}

void renderGalleryFrame(std::uint32_t *dst, int width, int height,
                        const cv::Mat &image, const DemoState &state) {
  std::fill(dst, dst + static_cast<std::size_t>(width) * height,
            argb(255, 0, 0, 0));

  if (image.empty() || width <= 0 || height <= 0) {
    return;
  }

  cv::Mat bgra;
  int draw_w = width;
  int draw_h = height;
  int offset_x = 0;
  int offset_y = 0;

  if (image.cols == width && image.rows == height) {
    cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
  } else {
    const double scale_x = static_cast<double>(width) / image.cols;
    const double scale_y = static_cast<double>(height) / image.rows;
    const double scale = std::min(scale_x, scale_y);
    draw_w = std::max(1, static_cast<int>(image.cols * scale));
    draw_h = std::max(1, static_cast<int>(image.rows * scale));
    offset_x = (width - draw_w) / 2;
    offset_y = (height - draw_h) / 2;

    cv::Mat resized;
    cv::resize(image, resized, cv::Size(draw_w, draw_h), 0, 0, cv::INTER_LINEAR);
    cv::cvtColor(resized, bgra, cv::COLOR_BGR2BGRA);
  }

  for (int y = 0; y < draw_h; ++y) {
    std::memcpy(reinterpret_cast<std::uint8_t *>(dst) +
                    static_cast<std::size_t>((offset_y + y) * width + offset_x) * 4,
                bgra.ptr(y), static_cast<std::size_t>(draw_w) * 4);
  }

  fillRect(dst, width, height, 0, 0, width, 78, argb(180, 0, 0, 0));
  fillRect(dst, width, height, 0, height - 96, width, 96, argb(180, 0, 0, 0));
  drawText(dst, width, height, 24, 18, "MODE: ALBUM",
           argb(255, 255, 255, 255), 3);
  drawText(dst, width, height, 24, 52,
           "PHOTO: " + std::to_string(state.index + 1) + "/" +
               std::to_string(state.photos.size()),
           argb(255, 200, 220, 255), 2);
  drawText(dst, width, height, 24, height - 44,
           "N NEXT  P PREV  L LIVE  Q QUIT",
           argb(255, 255, 255, 255), 2);
}

void renderLiveFrame(std::uint32_t *dst, int width, int height,
                     const DemoState &state) {
  std::fill(dst, dst + static_cast<std::size_t>(width) * height, 0x00000000u);
  fillRect(dst, width, height, 0, 0, width, 78, argb(180, 0, 0, 0));
  fillRect(dst, width, height, 0, height - 96, width, 96, argb(180, 0, 0, 0));
  drawText(dst, width, height, 24, 18, "MODE: LIVE CAMERA",
           argb(255, 255, 255, 255), 3);
  drawText(dst, width, height, 24, 52,
           "PHOTOS: " + std::to_string(state.photos.size()),
           argb(255, 200, 220, 255), 2);
  drawText(dst, width, height, 24, height - 80,
           "STATUS: " + state.status, argb(255, 255, 230, 140), 2);
  drawText(dst, width, height, 24, height - 44,
           "C CAPTURE  G GALLERY  Q QUIT",
           argb(255, 255, 255, 255), 2);
}

bool showLiveOverlay(tdl_app::OsdRegion *region, const DemoState &state,
                     std::string *error) {
  if (!region) {
    if (error) *error = "live osd region is null";
    return false;
  }
  tdl_app::OsdCanvas canvas;
  if (!region->getCanvas(&canvas, error)) {
    return false;
  }
  if (!canvas.data || canvas.width <= 0 || canvas.height <= 0 ||
      canvas.stride != canvas.width * 4) {
    if (error) {
      *error = "invalid live canvas: " + std::to_string(canvas.width) + "x" +
               std::to_string(canvas.height) + " stride=" +
               std::to_string(canvas.stride);
    }
    return false;
  }
  renderLiveFrame(static_cast<std::uint32_t *>(canvas.data), canvas.width,
                  canvas.height, state);
  if (!region->updateCanvas(error)) {
    return false;
  }
  return region->setVisible(true, error);
}

bool capturePhoto(camera_demo_support::CameraRuntime *runtime,
                  const std::string &album_dir, DemoState *state,
                  std::string *status, std::string *error) {
  if (!runtime || !state) {
    if (error) *error = "invalid capture state";
    return false;
  }
  tdl_app::Frame frame;
  if (!runtime->camera.read(&frame, error)) {
    return false;
  }
  state->photos = scanAlbum(album_dir);
  const int next_index = static_cast<int>(state->photos.size()) + 1;
  char filename[64] = {0};
  std::snprintf(filename, sizeof(filename), "photo_%04d.jpg", next_index);
  const std::string output_path = album_dir + "/" + filename;
  if (!camera_demo_support::saveFrameAsImage(frame, output_path, error)) {
    return false;
  }
  state->photos = scanAlbum(album_dir);
  state->index = static_cast<int>(state->photos.size()) - 1;
  state->status = "captured " + std::string(filename);
  if (status) {
    *status = state->status;
  }
  return true;
}

bool showGalleryImage(const DemoState &state, tdl_app::OsdRegion *region,
                      std::string *status, std::string *error) {
  if (state.index < 0 || state.index >= static_cast<int>(state.photos.size())) {
    if (error) *error = "album is empty";
    return false;
  }
  cv::Mat image = cv::imread(state.photos[state.index], cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to load image: " + state.photos[state.index];
    return false;
  }
  if (!region) {
    if (error) *error = "gallery osd region is null";
    return false;
  }
  tdl_app::OsdCanvas canvas;
  if (!region->getCanvas(&canvas, error)) {
    return false;
  }

  if (canvas.width <= 0 || canvas.height <= 0 || canvas.stride < canvas.width * 4 ||
      !canvas.data) {
    if (error) {
      *error = "invalid gallery canvas from region: " +
               std::to_string(canvas.width) + "x" + std::to_string(canvas.height) +
               " stride=" + std::to_string(canvas.stride);
    }
    return false;
  }

  try {
    if (canvas.stride == canvas.width * 4) {
      renderGalleryFrame(static_cast<std::uint32_t *>(canvas.data), canvas.width,
                         canvas.height, image, state);
    } else if (!blitToOsdCanvas(image, canvas, error)) {
      return false;
    }
  } catch (const cv::Exception &ex) {
    if (error) {
      *error = "opencv gallery render failed: src=" +
               std::to_string(image.cols) + "x" + std::to_string(image.rows) +
               " dst=" + std::to_string(canvas.width) + "x" +
               std::to_string(canvas.height) + " stride=" +
               std::to_string(canvas.stride) + " msg=" + ex.what();
    }
    return false;
  }
  if (!region->updateCanvas(error)) {
    return false;
  }
  if (!region->setVisible(true, error)) {
    return false;
  }
  if (status) {
    *status = "gallery " + basenameOnly(state.photos[state.index]);
  }
  return true;
}

bool bindLiveLinks(tdl_app::MediaLink *preview_link, tdl_app::MediaLink *display_link,
                   std::string *error) {
  if (!preview_link || !display_link) {
    if (error) *error = "live link is null";
    return false;
  }
  if (!preview_link->bind(error)) {
    return false;
  }
  if (!display_link->bind(error)) {
    preview_link->unbind();
    return false;
  }
  return true;
}

void unbindLiveLinks(tdl_app::MediaLink *preview_link, tdl_app::MediaLink *display_link) {
  if (display_link) {
    display_link->unbind();
  }
  if (preview_link) {
    preview_link->unbind();
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
  if (!camera_demo_support::setCameraPreset(&opt.camera, opt.source, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (!ensureDir(opt.album_dir, &error)) {
    std::cerr << error << "\n";
    return 2;
  }

  camera_demo_support::CameraRuntime runtime;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &runtime, &error)) {
    std::cerr << "camera runtime open failed: " << error << "\n";
    return 3;
  }

  tdl_app::VoOutput vo(makeVoConfig(opt));
  if (!vo.open(&error)) {
    std::cerr << "vo open failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 4;
  }

  tdl_app::MediaLink preview_link({camera_demo_support::previewChannel(
                                       opt.camera, runtime.camera.config()),
                                   makeDisplayOutputChannel()});
  tdl_app::MediaLink display_link(
      {makeDisplayOutputChannel(), tdl_app::MediaChannel::vo(opt.layer, opt.vo_chn)});

  if (!bindLiveLinks(&preview_link, &display_link, &error)) {
    std::cerr << "live bind failed: " << error << "\n";
    vo.close();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 5;
  }

  DemoState state;
  state.photos = scanAlbum(opt.album_dir);
  if (!state.photos.empty()) {
    state.index = static_cast<int>(state.photos.size()) - 1;
  }

  tdl_app::OsdRegion gallery_region(makeGalleryOsdConfig(opt));
  if (!gallery_region.create(&error)) {
    std::cerr << "gallery osd create failed: " << error << "\n";
    unbindLiveLinks(&preview_link, &display_link);
    vo.close();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 6;
  }
  if (!gallery_region.attach(
          makeDisplayOutputChannel(), 0, 0, 10, &error)) {
    std::cerr << "gallery osd attach failed: " << error << "\n";
    gallery_region.destroy();
    unbindLiveLinks(&preview_link, &display_link);
    vo.close();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 7;
  }
  if (!showLiveOverlay(&gallery_region, state, &error)) {
    std::cerr << "live overlay init failed: " << error << "\n";
    gallery_region.detach();
    gallery_region.destroy();
    unbindLiveLinks(&preview_link, &display_link);
    vo.close();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 8;
  }
  std::string status = "ready";

  TerminalGuard terminal;
  if (!prepareTerminal(&terminal, &error)) {
    std::cerr << error << "\n";
    gallery_region.detach();
    gallery_region.destroy();
    unbindLiveLinks(&preview_link, &display_link);
    vo.close();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 9;
  }

  std::cout << "sophpi_camera_demo\n"
            << "  c : capture photo\n"
            << "  g : gallery mode\n"
            << "  n : next photo\n"
            << "  p : previous photo\n"
            << "  l : back to live\n"
            << "  q : quit\n";

  bool running = true;
  while (running) {
    const int key = readKey();
    if (key < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      continue;
    }
    switch (key) {
      case 'c': {
        if (!capturePhoto(&runtime, opt.album_dir, &state, &status, &error)) {
          state.status = "capture failed";
          std::cerr << "capture failed: " << error << "\n";
        } else {
          state.status = status;
          std::cout << status << "\n";
        }
        break;
      }
      case 'g': {
        if (state.photos.empty()) {
          state.photos = scanAlbum(opt.album_dir);
          if (!state.photos.empty()) {
            state.index = static_cast<int>(state.photos.size()) - 1;
          }
        }
        if (state.photos.empty()) {
          state.status = "album empty";
          std::cout << "album is empty\n";
          break;
        }
        if (!showGalleryImage(state, &gallery_region, &status, &error)) {
          state.status = "gallery failed";
          std::cerr << "gallery show failed: " << error << "\n";
        } else {
          state.mode = DemoMode::Gallery;
          state.status = status;
          std::cout << status << "\n";
        }
        break;
      }
      case 'n': {
        if (state.mode != DemoMode::Gallery || state.photos.empty()) {
          break;
        }
        state.index = (state.index + 1) % static_cast<int>(state.photos.size());
        if (!showGalleryImage(state, &gallery_region, &status, &error)) {
          state.status = "gallery next failed";
          std::cerr << "gallery next failed: " << error << "\n";
        } else {
          state.status = status;
          std::cout << status << "\n";
        }
        break;
      }
      case 'p': {
        if (state.mode != DemoMode::Gallery || state.photos.empty()) {
          break;
        }
        state.index = (state.index - 1 + static_cast<int>(state.photos.size())) %
                      static_cast<int>(state.photos.size());
        if (!showGalleryImage(state, &gallery_region, &status, &error)) {
          state.status = "gallery prev failed";
          std::cerr << "gallery prev failed: " << error << "\n";
        } else {
          state.status = status;
          std::cout << status << "\n";
        }
        break;
      }
      case 'l': {
        if (state.mode != DemoMode::Gallery) {
          break;
        }
        if (!showLiveOverlay(&gallery_region, state, &error)) {
          state.status = "live overlay failed";
          std::cerr << "live overlay failed: " << error << "\n";
          break;
        }
        state.mode = DemoMode::Live;
        status = "live";
        state.status = status;
        std::cout << status << "\n";
        break;
      }
      case 'q': {
        running = false;
        break;
      }
      default:
        break;
    }
  }

  restoreTerminal(&terminal);
  gallery_region.detach();
  gallery_region.destroy();
  unbindLiveLinks(&preview_link, &display_link);
  vo.close();
  camera_demo_support::closeCameraRuntime(&runtime);
  std::cout << "camera demo exit\n";
  return 0;
}

