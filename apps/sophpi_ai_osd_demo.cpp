#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <termios.h>
#include <unistd.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "cvi_comm_video.h"
#include "cvi_sys.h"
#include "tdl_app/media_link.hpp"
#include "tdl_app/osd_region.hpp"
#include "tdl_app/tdl_app.hpp"
#include "tdl_app/vo_output.hpp"

namespace {

// Global stop flag so a SIGINT/SIGTERM (e.g. Ctrl+C) unwinds through the normal
// cleanup path instead of killing the process and leaving stale VPSS binds / VO
// state on the small core (which then makes the next CVI_SYS_Bind fail).
std::atomic<bool> g_running{true};

void handleSignal(int) { g_running.store(false); }

// The whole example follows the dual-OS split documented in
// docs/sophpi_camera_demo_zh.md: the small core owns sensor/VI/VPSS/VO, the
// big core only attaches, binds the display path, and drives the OSD overlay.
//
// Two independent grp0 channels are used at the same time:
//   - ai   (grp0/ch1, 640x640)   -> read + detected in the main loop
//   - live (grp0/ch2, 1280x720)  -> hardware-bound to the screen as background
//
// There is no separate inference thread: the OSD overlay is redrawn straight
// from each detection result, so its refresh rate is simply the detector
// throughput (capture + inference + render, all in one loop).
struct Options {
  camera_demo_support::CommonOptions camera;
  std::string model_spec;
  std::string firmware;
  float threshold = 0.25f;
  int top_k = 5;
  std::string logo_path = "/root/logo.png";
  int vo_dev = 0;
  int layer = 0;
  int vo_chn = 0;
  int screen_width = 720;
  int screen_height = 1280;
  int interface_type = tdl_app::VoInterfaceType::Mipi;
  int interface_sync = tdl_app::VoInterfaceSync::P720_1280_60;
};

struct TerminalGuard {
  bool ok = false;
  termios old_attr {};
};

// Which algorithm family a model-spec maps to. Drives both which algorithm
// object is loaded and how its result is rendered onto the OSD overlay.
enum class Family {
  Detection,
  Classification,
  Keypoint,
  InstanceSeg,
  Ocr,
};

std::string toUpperCopy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                 });
  return value;
}

const char *familyName(Family family) {
  switch (family) {
    case Family::Detection:
      return "detection";
    case Family::Classification:
      return "classification";
    case Family::Keypoint:
      return "keypoint";
    case Family::InstanceSeg:
      return "instance-seg";
    case Family::Ocr:
      return "ocr";
  }
  return "detection";
}

// Prefer the model-spec's task field; fall back to model_type prefixes; default
// to detection when nothing matches.
Family detectFamily(const std::string &model_spec) {
  tdl_app::ModelDescriptor descriptor;
  std::string ignored_error;
  if (!tdl_app::loadModelDescriptor(model_spec, &descriptor, &ignored_error)) {
    return Family::Detection;
  }

  const std::string task = toUpperCopy(descriptor.task_name);
  if (task == "CLASSIFY" || task == "CLASSIFICATION") {
    return Family::Classification;
  }
  if (task == "KEYPOINT" || task == "LANDMARK") {
    return Family::Keypoint;
  }
  if (task == "SEGMENTATION" || task == "INSTANCE_SEGMENTATION") {
    return Family::InstanceSeg;
  }
  if (task == "OCR") {
    return Family::Ocr;
  }
  if (task == "DETECT" || task == "DETECTION") {
    return Family::Detection;
  }

  const std::string model_type = toUpperCopy(descriptor.model_type);
  if (model_type.compare(0, 6, "PP_OCR") == 0 ||
      model_type.compare(0, 6, "PLATE_") == 0 ||
      model_type.compare(0, 3, "LPR") == 0) {
    return Family::Ocr;
  }
  if (model_type.find("SEG") != std::string::npos) {
    return Family::InstanceSeg;
  }
  if (model_type.compare(0, 8, "KEYPOINT") == 0 ||
      model_type.find("POSE") != std::string::npos) {
    return Family::Keypoint;
  }
  if (model_type.compare(0, 3, "CLS") == 0 ||
      model_type.find("CLASSIFIER") != std::string::npos) {
    return Family::Classification;
  }
  return Family::Detection;
}

// 17-keypoint COCO pose skeleton (used when a keypoint model returns 17 points).
constexpr int kPose17Skeleton[][2] = {
    {0, 1},  {0, 2},   {1, 3},   {2, 4},   {5, 6},   {5, 7},
    {7, 9},  {6, 8},   {8, 10},  {5, 11},  {6, 12},  {11, 12},
    {11, 13}, {13, 15}, {12, 14}, {14, 16}};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  sophpi_ai_osd_demo --model-spec FILE\n"
      << "                     [--firmware FILE] [--threshold 0.25]\n"
      << "                     [--top-k 5]\n"
      << "                     [--logo /root/logo.png]\n"
      << "                     [--screen-width N] [--screen-height N]\n"
      << "\n"
      << "  Model family is auto-detected from the model-spec (task field):\n"
      << "  detection / classification / keypoint / instance-seg / ocr.\n";
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
    if (arg == "--model-spec") {
      const char *v = value("--model-spec");
      if (!v) return false;
      opt->model_spec = v;
    } else if (arg == "--firmware") {
      const char *v = value("--firmware");
      if (!v) return false;
      opt->firmware = v;
    } else if (arg == "--threshold") {
      const char *v = value("--threshold");
      if (!v) return false;
      opt->threshold = static_cast<float>(std::atof(v));
    } else if (arg == "--top-k") {
      const char *v = value("--top-k");
      if (!v) return false;
      opt->top_k = std::atoi(v);
    } else if (arg == "--logo") {
      const char *v = value("--logo");
      if (!v) return false;
      opt->logo_path = v;
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
  return !opt->model_spec.empty();
}

bool prepareTerminal(TerminalGuard *guard, std::string *error) {
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
}

void restoreTerminal(TerminalGuard *guard) {
  if (guard && guard->ok) {
    tcsetattr(STDIN_FILENO, TCSANOW, &guard->old_attr);
    guard->ok = false;
  }
}

int readKey() {
  unsigned char ch = 0;
  const int n = static_cast<int>(::read(STDIN_FILENO, &ch, 1));
  return n == 1 ? static_cast<int>(ch) : -1;
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

tdl_app::MediaChannel makeDisplayChannel() {
  return tdl_app::MediaChannel::vpss(tdl_app::DualOsLayout::kDisplayVpssGroup,
                                     tdl_app::DualOsLayout::kDisplayChannel);
}

tdl_app::OsdRegion::Config makeOsdConfig() {
  return tdl_app::OsdRegion::canvas(
      130, tdl_app::DualOsLayout::kLiveWidth,
      tdl_app::DualOsLayout::kLiveHeight, tdl_app::PixelFormat::ARGB8888, 2, 0);
}

bool bindLinks(tdl_app::MediaLink *preview_link, tdl_app::MediaLink *display_link,
               std::string *error) {
  if (!preview_link->bind(error)) {
    return false;
  }
  if (!display_link->bind(error)) {
    preview_link->unbind();
    return false;
  }
  return true;
}

void unbindLinks(tdl_app::MediaLink *preview_link,
                 tdl_app::MediaLink *display_link) {
  if (display_link) display_link->unbind();
  if (preview_link) preview_link->unbind();
}

// Load and normalize the logo to BGRA once. Oversized logos are scaled down so
// they never dominate the portrait screen.
cv::Mat loadLogoBgra(const std::string &path, int max_w, int max_h) {
  cv::Mat raw = cv::imread(path, cv::IMREAD_UNCHANGED);
  if (raw.empty()) {
    std::cerr << "logo not loaded (skipped): " << path << "\n";
    return cv::Mat();
  }
  cv::Mat bgra;
  if (raw.channels() == 4) {
    bgra = raw;
  } else if (raw.channels() == 3) {
    cv::cvtColor(raw, bgra, cv::COLOR_BGR2BGRA);
  } else if (raw.channels() == 1) {
    cv::cvtColor(raw, bgra, cv::COLOR_GRAY2BGRA);
  } else {
    std::cerr << "logo has unsupported channel count: " << raw.channels()
              << "\n";
    return cv::Mat();
  }
  const double sx = static_cast<double>(max_w) / bgra.cols;
  const double sy = static_cast<double>(max_h) / bgra.rows;
  const double scale = std::min({1.0, sx, sy});
  if (scale < 1.0) {
    cv::resize(bgra, bgra,
               cv::Size(std::max(1, static_cast<int>(bgra.cols * scale)),
                        std::max(1, static_cast<int>(bgra.rows * scale))),
               0, 0, cv::INTER_AREA);
  }
  return bgra;
}

void putLabel(cv::Mat &img, const std::string &text, cv::Point org,
              double scale, const cv::Scalar &color) {
  cv::putText(img, text, org, cv::FONT_HERSHEY_SIMPLEX, scale,
              cv::Scalar(0, 0, 0, 255), 3, cv::LINE_AA);
  cv::putText(img, text, org, cv::FONT_HERSHEY_SIMPLEX, scale, color, 1,
              cv::LINE_AA);
}

std::string labelFor(const std::vector<std::string> &labels, int class_id) {
  if (class_id >= 0 && class_id < static_cast<int>(labels.size())) {
    return labels[class_id];
  }
  return std::to_string(class_id);
}

cv::Scalar paletteColor(int index) {
  static const cv::Scalar kPalette[] = {
      cv::Scalar(56, 56, 255, 255),   cv::Scalar(151, 157, 255, 255),
      cv::Scalar(31, 112, 255, 255),  cv::Scalar(29, 178, 255, 255),
      cv::Scalar(49, 210, 207, 255),  cv::Scalar(10, 249, 72, 255),
      cv::Scalar(23, 204, 146, 255),  cv::Scalar(134, 219, 61, 255),
      cv::Scalar(52, 147, 26, 255),   cv::Scalar(187, 212, 0, 255),
      cv::Scalar(168, 153, 44, 255),  cv::Scalar(255, 194, 0, 255),
      cv::Scalar(147, 69, 52, 255),   cv::Scalar(255, 115, 100, 255),
      cv::Scalar(236, 24, 0, 255),    cv::Scalar(255, 56, 132, 255),
      cv::Scalar(133, 0, 82, 255),    cv::Scalar(255, 56, 203, 255)};
  const int count = static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]));
  return kPalette[((index % count) + count) % count];
}

// Create a fresh transparent overlay in the OSD canvas's native landscape
// orientation. VO applies a hardware rotation to the whole composited output
// (video + attached OSD region), so no software pre-rotation is needed here.
void beginOverlay(cv::Mat *overlay) {
  const int cw = tdl_app::DualOsLayout::kLiveWidth;   // 1280
  const int ch = tdl_app::DualOsLayout::kLiveHeight;  // 720
  overlay->create(ch, cw, CV_8UC4);
  overlay->setTo(cv::Scalar(0, 0, 0, 0));
}

// Logo (bottom-right, alpha-aware), screen-edge borders and the merged
// capture+infer+render FPS. Shared by every family after its own drawing.
void finishOverlay(cv::Mat *overlay, const cv::Mat &logo, double fps) {
  const int cw = tdl_app::DualOsLayout::kLiveWidth;
  const int ch = tdl_app::DualOsLayout::kLiveHeight;
  const cv::Scalar blue(255, 0, 0, 255);
  const cv::Scalar red(0, 0, 255, 255);
  const cv::Scalar white(255, 255, 255, 255);

  if (!logo.empty()) {
    const int margin = 20;
    const int lw = std::min(logo.cols, cw - 2 * margin);
    const int lh = std::min(logo.rows, ch - 2 * margin);
    if (lw > 0 && lh > 0) {
      const int x = cw - lw - margin;
      const int y = ch - lh - margin;
      cv::Rect roi(x, y, lw, lh);
      cv::Mat src = logo(cv::Rect(0, 0, lw, lh));
      std::vector<cv::Mat> channels;
      cv::split(src, channels);
      cv::Mat mask = channels[3];
      src.copyTo((*overlay)(roi), mask);
    }
  }

  const cv::Rect frame_rect(4, 4, cw - 8, ch - 8);
  cv::rectangle(*overlay, frame_rect, blue, 8);
  cv::rectangle(*overlay, frame_rect, red, 2);

  putLabel(*overlay, "FPS: " + cv::format("%.1f", fps), cv::Point(24, 48), 0.6,
           white);
}

// Detection: boxes + class label + confidence.
void drawDetection(cv::Mat *overlay, const std::vector<tdl_app::Box> &boxes,
                   const std::vector<std::string> &labels, double scale_x,
                   double scale_y) {
  const cv::Scalar green(0, 255, 0, 255);
  for (const auto &box : boxes) {
    const float lx1 = static_cast<float>(box.x1 * scale_x);
    const float ly1 = static_cast<float>(box.y1 * scale_y);
    const float lx2 = static_cast<float>(box.x2 * scale_x);
    const float ly2 = static_cast<float>(box.y2 * scale_y);
    cv::Point p1(static_cast<int>(std::min(lx1, lx2)),
                 static_cast<int>(std::min(ly1, ly2)));
    cv::Point p2(static_cast<int>(std::max(lx1, lx2)),
                 static_cast<int>(std::max(ly1, ly2)));
    cv::rectangle(*overlay, p1, p2, green, 2, cv::LINE_AA);

    const std::string text =
        labelFor(labels, box.class_id) + " " + cv::format("%.2f", box.score);
    int baseline = 0;
    const cv::Size ts =
        cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
    const int top = std::max(0, p1.y - ts.height - 6);
    cv::rectangle(*overlay, cv::Point(p1.x, top),
                  cv::Point(p1.x + ts.width + 6, top + ts.height + baseline + 6),
                  green, cv::FILLED);
    cv::putText(*overlay, text, cv::Point(p1.x + 3, top + ts.height + 1),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0, 255), 1,
                cv::LINE_AA);
  }
}

// Classification: a top-k text panel in the top-left corner.
void drawClassification(cv::Mat *overlay, const tdl_app::AlgorithmResult &result,
                        int top_k) {
  const cv::Scalar white(255, 255, 255, 255);
  const cv::Scalar yellow(0, 255, 255, 255);
  const int limit = std::min(static_cast<int>(result.classes.size()),
                             std::max(1, top_k));
  int y = 90;
  for (int i = 0; i < limit; ++i) {
    const auto &item = result.classes[i];
    const std::string name = labelFor(result.labels, item.class_id);
    const std::string text = cv::format("%d. %s  %.1f%%", i + 1, name.c_str(),
                                        item.score * 100.0f);
    putLabel(*overlay, text, cv::Point(24, y), 0.7, i == 0 ? yellow : white);
    y += 34;
  }
}

// Keypoint / pose: points + skeleton (skeleton drawn when 17 points present).
void drawKeypoint(cv::Mat *overlay, const tdl_app::KeypointResult &result,
                  double scale_x, double scale_y) {
  const cv::Scalar skeleton(0, 255, 255, 255);
  auto mapped = [&](const tdl_app::Point &pt) {
    return cv::Point(static_cast<int>(pt.x * scale_x),
                     static_cast<int>(pt.y * scale_y));
  };

  if (result.pointCount() == 17) {
    for (const auto &edge : kPose17Skeleton) {
      const auto &a = result.points[edge[0]];
      const auto &b = result.points[edge[1]];
      if (a.score <= 0.05f || b.score <= 0.05f) {
        continue;
      }
      cv::line(*overlay, mapped(a), mapped(b), skeleton, 2, cv::LINE_AA);
    }
  }

  for (std::size_t i = 0; i < result.points.size(); ++i) {
    const auto &pt = result.points[i];
    const cv::Scalar color = pt.score > 0.5f ? cv::Scalar(0, 0, 255, 255)
                                             : cv::Scalar(255, 0, 0, 255);
    cv::circle(*overlay, mapped(pt), 4, color, cv::FILLED, cv::LINE_AA);
  }
}

// Instance segmentation: box + polygon outline per instance. Per-pixel mask
// blending is too costly for the live loop, so it is only drawn when
// TDL_APP_SEG_DEBUG is set.
void drawInstanceSeg(cv::Mat *overlay,
                     const tdl_app::InstanceSegmentationResult &result,
                     double scale_x, double scale_y, bool draw_mask) {
  for (std::size_t i = 0; i < result.instances.size(); ++i) {
    const auto &instance = result.instances[i];
    const cv::Scalar color = paletteColor(
        instance.box.class_id >= 0 ? instance.box.class_id
                                   : static_cast<int>(i));

    if (draw_mask && !instance.outline.empty()) {
      std::vector<cv::Point> poly;
      poly.reserve(instance.outline.size());
      for (const auto &pt : instance.outline) {
        poly.emplace_back(static_cast<int>(pt.x * scale_x),
                          static_cast<int>(pt.y * scale_y));
      }
      const std::vector<std::vector<cv::Point>> polys{poly};
      cv::fillPoly(*overlay, polys, color, cv::LINE_AA);
    }

    if (!instance.outline.empty()) {
      std::vector<cv::Point> poly;
      poly.reserve(instance.outline.size());
      for (const auto &pt : instance.outline) {
        poly.emplace_back(static_cast<int>(pt.x * scale_x),
                          static_cast<int>(pt.y * scale_y));
      }
      const std::vector<std::vector<cv::Point>> polys{poly};
      cv::polylines(*overlay, polys, true, color, 2, cv::LINE_AA);
    }

    cv::Point p1(static_cast<int>(instance.box.x1 * scale_x),
                 static_cast<int>(instance.box.y1 * scale_y));
    cv::Point p2(static_cast<int>(instance.box.x2 * scale_x),
                 static_cast<int>(instance.box.y2 * scale_y));
    cv::rectangle(*overlay, p1, p2, color, 2, cv::LINE_AA);

    const std::string text = std::to_string(instance.box.class_id) + " " +
                             cv::format("%.2f", instance.box.score);
    int baseline = 0;
    const cv::Size ts =
        cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
    const int top = std::max(0, p1.y - ts.height - 6);
    cv::rectangle(*overlay, cv::Point(p1.x, top),
                  cv::Point(p1.x + ts.width + 6, top + ts.height + baseline + 6),
                  color, cv::FILLED);
    cv::putText(*overlay, text, cv::Point(p1.x + 3, top + ts.height + 1),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0, 255), 1,
                cv::LINE_AA);
  }
}

// OCR: detected text boxes plus the recognized string above each box. The
// per-box text is carried in result.attributes as "ocr_text:<text>".
void drawOcr(cv::Mat *overlay, const tdl_app::AlgorithmResult &result,
             double scale_x, double scale_y) {
  const cv::Scalar green(0, 255, 0, 255);
  const cv::Scalar white(255, 255, 255, 255);
  const std::string prefix = "ocr_text:";
  for (std::size_t i = 0; i < result.boxes.size(); ++i) {
    const auto &box = result.boxes[i];
    cv::Point p1(static_cast<int>(box.x1 * scale_x),
                 static_cast<int>(box.y1 * scale_y));
    cv::Point p2(static_cast<int>(box.x2 * scale_x),
                 static_cast<int>(box.y2 * scale_y));
    cv::rectangle(*overlay, p1, p2, green, 2, cv::LINE_AA);

    std::string text;
    if (i < result.attributes.size() &&
        result.attributes[i].name.compare(0, prefix.size(), prefix) == 0) {
      text = result.attributes[i].name.substr(prefix.size());
    }
    if (!text.empty()) {
      putLabel(*overlay, text, cv::Point(p1.x, std::max(20, p1.y - 6)), 0.6,
               white);
    }
  }
  if (result.boxes.empty() && !result.text.empty()) {
    putLabel(*overlay, result.text, cv::Point(24, 90), 0.7, white);
  }
}

// No software rotation here: the overlay is already composed in the canvas's
// native landscape orientation, and VO's hardware channel rotation takes care
// of rotating it (together with the video) onto the physical panel.
bool pushToCanvas(tdl_app::OsdRegion *region, const cv::Mat &overlay,
                  bool *visible, std::string *error) {
  tdl_app::OsdCanvas canvas;
  if (!region->getCanvas(&canvas, error)) {
    return false;
  }
  if (!canvas.data || canvas.width != overlay.cols ||
      canvas.height != overlay.rows) {
    if (error) {
      *error = "canvas mismatch: canvas=" + std::to_string(canvas.width) + "x" +
               std::to_string(canvas.height) + " overlay=" +
               std::to_string(overlay.cols) + "x" +
               std::to_string(overlay.rows);
    }
    return false;
  }
  for (int y = 0; y < canvas.height; ++y) {
    std::memcpy(static_cast<std::uint8_t *>(canvas.data) + y * canvas.stride,
                overlay.ptr(y),
                static_cast<std::size_t>(canvas.width) * 4);
  }
  if (!region->updateCanvas(error)) {
    return false;
  }
  if (!*visible) {
    if (!region->setVisible(true, error)) {
      return false;
    }
    *visible = true;
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

  std::string error;
  if (!camera_demo_support::setCameraPreset(&opt.camera, "ai", &error)) {
    std::cerr << error << "\n";
    return 1;
  }

  camera_demo_support::CameraRuntime runtime;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &runtime, &error)) {
    std::cerr << "camera runtime open failed: " << error << "\n";
    return 2;
  }

  const Family family = detectFamily(opt.model_spec);
  std::cerr << "model family=" << familyName(family) << "\n";

  const tdl_app::ModelSessionConfig session_config =
      tdl_app::ModelSessionConfig::fromSpec(opt.model_spec, opt.firmware);

  tdl_app::Detector detector;
  tdl_app::Classifier classifier;
  tdl_app::KeypointDetector keypoint;
  tdl_app::InstanceSegmenter segmenter;
  tdl_app::PlateRecognizer plate;

  bool load_ok = false;
  switch (family) {
    case Family::Detection:
      load_ok = detector.load(session_config, &error);
      break;
    case Family::Classification:
      load_ok = classifier.load(session_config, &error);
      break;
    case Family::Keypoint:
      load_ok = keypoint.load(session_config, &error);
      break;
    case Family::InstanceSeg:
      load_ok = segmenter.load(session_config, &error);
      break;
    case Family::Ocr:
      load_ok = plate.load(session_config, &error);
      break;
  }
  if (!load_ok) {
    std::cerr << "model load failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 3;
  }

  const bool seg_draw_mask = std::getenv("TDL_APP_SEG_DEBUG") != nullptr;

  tdl_app::VoOutput vo(makeVoConfig(opt));
  if (!vo.open(&error)) {
    std::cerr << "vo open failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 4;
  }

  // Live channel is the on-screen background; ai channel feeds inference.
  tdl_app::MediaLink preview_link(
      {tdl_app::MediaChannel::vpss(tdl_app::DualOsLayout::kCaptureVpssGroup,
                                   tdl_app::DualOsLayout::kLiveChannel),
       makeDisplayChannel()});
  tdl_app::MediaLink display_link(
      {makeDisplayChannel(), tdl_app::MediaChannel::vo(opt.layer, opt.vo_chn)});
  if (!bindLinks(&preview_link, &display_link, &error)) {
    std::cerr << "live bind failed: " << error << "\n";
    vo.close();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 5;
  }

  tdl_app::OsdRegion region(makeOsdConfig());
  if (!region.create(&error)) {
    std::cerr << "osd create failed: " << error << "\n";
    unbindLinks(&preview_link, &display_link);
    vo.close();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 6;
  }
  if (!region.attach(makeDisplayChannel(), 0, 0, 10, &error)) {
    std::cerr << "osd attach failed: " << error << "\n";
    region.destroy();
    unbindLinks(&preview_link, &display_link);
    vo.close();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 7;
  }

  // Sized for the final on-screen appearance: VO rotates the whole landscape
  // canvas 90 degrees, so a pre-rotation box of kLiveWidth/4 x kLiveHeight/2
  // ends up occupying the same on-panel footprint as before.
  const cv::Mat logo = loadLogoBgra(opt.logo_path,
                                    tdl_app::DualOsLayout::kLiveWidth / 4,
                                    tdl_app::DualOsLayout::kLiveHeight / 2);

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  TerminalGuard terminal;
  if (!prepareTerminal(&terminal, &error)) {
    std::cerr << error << "\n";
    region.detach();
    region.destroy();
    unbindLinks(&preview_link, &display_link);
    vo.close();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 8;
  }

  std::cout << "sophpi_ai_osd_demo running (press q to quit)\n";

  tdl_app::InferOptions infer_options;
  infer_options.threshold = opt.threshold;
  infer_options.top_k = opt.top_k;

  const int cw = tdl_app::DualOsLayout::kLiveWidth;
  const int ch = tdl_app::DualOsLayout::kLiveHeight;

  bool visible = false;
  cv::Mat overlay;
  double fps = 0.0;
  auto last = std::chrono::steady_clock::now();

  // Single loop: read the ai-channel frame, run the family's inference, render
  // the OSD overlay straight from the result, and push it to the canvas. No
  // separate inference thread; the OSD refresh rate is simply the model
  // throughput (capture + inference + render, all in one loop).
  while (g_running.load()) {
    tdl_app::Frame frame;
    if (!runtime.camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
    if (!video) {
      continue;
    }

    tdl_app::AlgorithmResult result;
    tdl_app::KeypointResult kp_result;
    tdl_app::InstanceSegmentationResult seg_result;

    const auto infer_start = std::chrono::steady_clock::now();
    bool infer_ok = false;
    switch (family) {
      case Family::Detection:
        infer_ok = detector.run(*video, infer_options, &result, &error);
        break;
      case Family::Classification:
        infer_ok = classifier.runFrame(frame, infer_options, &result, &error);
        break;
      case Family::Keypoint:
        infer_ok = keypoint.runFrame(frame, &kp_result, &error);
        break;
      case Family::InstanceSeg:
        infer_ok = segmenter.runFrame(frame, &seg_result, &error);
        break;
      case Family::Ocr:
        infer_ok = plate.runFrame(frame, infer_options, &result, &error);
        break;
    }
    const double infer_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - infer_start)
            .count();
    std::cerr << "infer took " << cv::format("%.2f", infer_ms) << " ms\n";
    if (!infer_ok) {
      std::cerr << "inference failed: " << error << "\n";
      continue;
    }

    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    if (dt > 0.0) {
      const double inst = 1.0 / dt;
      fps = fps <= 0.0 ? inst : fps * 0.8 + inst * 0.2;
    }

    const double scale_x = static_cast<double>(cw) / std::max(1, frame.width);
    const double scale_y = static_cast<double>(ch) / std::max(1, frame.height);
    beginOverlay(&overlay);
    switch (family) {
      case Family::Detection:
        drawDetection(&overlay, result.boxes, result.labels, scale_x, scale_y);
        break;
      case Family::Classification:
        drawClassification(&overlay, result, opt.top_k);
        break;
      case Family::Keypoint:
        drawKeypoint(&overlay, kp_result, scale_x, scale_y);
        break;
      case Family::InstanceSeg:
        drawInstanceSeg(&overlay, seg_result, scale_x, scale_y, seg_draw_mask);
        break;
      case Family::Ocr:
        drawOcr(&overlay, result, scale_x, scale_y);
        break;
    }
    finishOverlay(&overlay, logo, fps);

    if (!pushToCanvas(&region, overlay, &visible, &error)) {
      std::cerr << "osd push failed: " << error << "\n";
    }

    const int key = readKey();
    if (key == 'q') {
      g_running.store(false);
      break;
    }
  }

  restoreTerminal(&terminal);
  region.detach();
  region.destroy();
  unbindLinks(&preview_link, &display_link);
  vo.close();
  camera_demo_support::closeCameraRuntime(&runtime);
  std::cout << "sophpi_ai_osd_demo exit\n";
  return 0;
}
