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

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "cvi_comm_video.h"
#include "cvi_sys.h"
#include "mjpeg_server.hpp"
#include "tdl_app/audio.hpp"
#include "tdl_app/media_link.hpp"
#include "tdl_app/osd_region.hpp"
#include "tdl_app/tdl_app.hpp"
#include "tdl_app/venc_channel.hpp"
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
  int screen_height = 480;
  int interface_type = tdl_app::VoInterfaceType::Mipi;
  int interface_sync = tdl_app::VoInterfaceSync::P720_480_60;
  int stream_port = 8080;
  bool start_stream = false;
  std::string control_pipe = "/tmp/sophpi_ai_osd.ctrl";
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
      << "                     [--stream] [--stream-port 8080]\n"
      << "                     [--control-pipe /tmp/sophpi_ai_osd.ctrl]\n"
      << "\n"
      << "  Model family is auto-detected from the model-spec (task field):\n"
      << "  detection / classification / keypoint / instance-seg / ocr.\n"
      << "\n"
      << "Runtime keys:\n"
      << "  m  enter a new model-spec path to switch model on the fly\n"
      << "  t  toggle the MJPEG http stream (default off)\n"
      << "  a  toggle mic->speaker audio loopback (default off)\n"
      << "  q  quit\n"
      << "\n"
      << "Control pipe (same commands, for scripting / background runs):\n"
      << "  echo t > /tmp/sophpi_ai_osd.ctrl\n"
      << "  echo a > /tmp/sophpi_ai_osd.ctrl\n"
      << "  echo \"m /path/to/model.mud\" > /tmp/sophpi_ai_osd.ctrl\n"
      << "  echo q > /tmp/sophpi_ai_osd.ctrl\n";
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
    } else if (arg == "--stream-port") {
      const char *v = value("--stream-port");
      if (!v) return false;
      opt->stream_port = std::atoi(v);
    } else if (arg == "--stream") {
      opt->start_stream = true;
    } else if (arg == "--control-pipe") {
      const char *v = value("--control-pipe");
      if (!v) return false;
      opt->control_pipe = v;
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

std::string trimCopy(const std::string &value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return std::string();
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

// Non-loopback IPv4 addresses of the board, used to print reachable stream
// URLs when the MJPEG server is switched on.
std::vector<std::string> listIpv4Addresses() {
  std::vector<std::string> result;
  ifaddrs *ifaces = nullptr;
  if (getifaddrs(&ifaces) != 0) {
    return result;
  }
  for (const ifaddrs *iface = ifaces; iface; iface = iface->ifa_next) {
    if (!iface->ifa_addr || iface->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    const auto *addr = reinterpret_cast<const sockaddr_in *>(iface->ifa_addr);
    char text[INET_ADDRSTRLEN] = {0};
    if (!inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text))) {
      continue;
    }
    if (std::strcmp(text, "127.0.0.1") == 0) {
      continue;
    }
    result.push_back(text);
  }
  freeifaddrs(ifaces);
  return result;
}

// All algorithm objects in one place; only the object matching the active
// family holds a loaded model at any given time.
struct AlgorithmSet {
  tdl_app::Detector detector;
  tdl_app::Classifier classifier;
  tdl_app::KeypointDetector keypoint;
  tdl_app::InstanceSegmenter segmenter;
  tdl_app::PlateRecognizer plate;

  bool load(Family family, const tdl_app::ModelSessionConfig &config,
            std::string *error) {
    switch (family) {
      case Family::Detection:
        return detector.load(config, error);
      case Family::Classification:
        return classifier.load(config, error);
      case Family::Keypoint:
        return keypoint.load(config, error);
      case Family::InstanceSeg:
        return segmenter.load(config, error);
      case Family::Ocr:
        return plate.load(config, error);
    }
    return false;
  }

  void reset(Family family) {
    switch (family) {
      case Family::Detection:
        detector.reset();
        break;
      case Family::Classification:
        classifier.reset();
        break;
      case Family::Keypoint:
        keypoint.reset();
        break;
      case Family::InstanceSeg:
        segmenter.reset();
        break;
      case Family::Ocr:
        plate.reset();
        break;
    }
  }
};

// Background model loader. The switch policy is unload-first: the caller
// resets the old model on the main thread, then start() loads the new spec on
// a worker thread while the main loop keeps the live view and OSD running.
// The worker only touches the (currently unloaded) target algorithm object,
// and the main loop skips inference until take() reports the result, so no
// extra locking is needed.
class ModelLoader {
 public:
  ~ModelLoader() { join(); }

  void start(AlgorithmSet *algorithms, Family family,
             const tdl_app::ModelSessionConfig &config) {
    join();
    done_.store(false);
    active_ = true;
    family_ = family;
    spec_ = config.model_spec;
    worker_ = std::thread([this, algorithms, family, config]() {
      success_ = algorithms->load(family, config, &error_);
      done_.store(true);
    });
  }

  bool active() const { return active_; }
  bool finished() const { return active_ && done_.load(); }
  Family family() const { return family_; }
  const std::string &spec() const { return spec_; }

  // Call once finished() is true; joins the worker and reports the result.
  bool take(std::string *error) {
    join();
    active_ = false;
    if (!success_ && error) {
      *error = error_;
    }
    return success_;
  }

  void join() {
    if (worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  std::thread worker_;
  std::atomic<bool> done_{false};
  bool active_ = false;
  bool success_ = false;
  std::string error_;
  Family family_ = Family::Detection;
  std::string spec_;
};

// Minimal line editor for typing a model-spec path while the terminal is in
// raw mode (characters are echoed manually).
struct CommandInput {
  bool active = false;
  std::string buffer;

  void begin() {
    active = true;
    buffer.clear();
    std::cout << "\nmodel-spec> " << std::flush;
  }

  // Returns true when a full line was submitted (stored in *line).
  bool feed(int key, std::string *line) {
    if (key == '\r' || key == '\n') {
      std::cout << "\n" << std::flush;
      *line = buffer;
      active = false;
      buffer.clear();
      return true;
    }
    if (key == 27) {  // ESC cancels
      active = false;
      buffer.clear();
      std::cout << " (cancelled)\n" << std::flush;
      return false;
    }
    if (key == 127 || key == 8) {  // backspace
      if (!buffer.empty()) {
        buffer.pop_back();
        std::cout << "\b \b" << std::flush;
      }
      return false;
    }
    if (key >= 32 && key < 127) {
      buffer.push_back(static_cast<char>(key));
      std::cout << static_cast<char>(key) << std::flush;
    }
    return false;
  }
};

// Line-oriented control FIFO so the demo can also be driven from scripts or
// while running in the background: `echo t > /tmp/sophpi_ai_osd.ctrl`.
// Commands mirror the interactive keys: t / a / q / "m <model-spec-path>".
class ControlPipe {
 public:
  ~ControlPipe() { close(); }

  bool open(const std::string &path, std::string *error) {
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
      if (!S_ISFIFO(st.st_mode)) {
        if (error) {
          *error = "control pipe path exists and is not a FIFO: " + path;
        }
        return false;
      }
    } else if (::mkfifo(path.c_str(), 0666) != 0) {
      if (error) {
        *error = "mkfifo failed: " + path;
      }
      return false;
    }
    // O_RDWR keeps this process registered as a writer, so read() reports
    // EAGAIN instead of a permanent EOF whenever external writers disconnect.
    fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
      if (error) {
        *error = "failed to open control pipe: " + path;
      }
      return false;
    }
    return true;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  // Appends every complete (newline-terminated) command line to *lines.
  void readLines(std::vector<std::string> *lines) {
    if (fd_ < 0) {
      return;
    }
    char buf[256];
    while (true) {
      const ssize_t n = ::read(fd_, buf, sizeof(buf));
      if (n <= 0) {
        break;
      }
      pending_.append(buf, static_cast<std::size_t>(n));
    }
    std::size_t pos;
    while ((pos = pending_.find('\n')) != std::string::npos) {
      lines->push_back(pending_.substr(0, pos));
      pending_.erase(0, pos + 1);
    }
  }

 private:
  int fd_ = -1;
  std::string pending_;
};

// Mic -> speaker audio loopback (same as `tdl_audio_stream_demo --mode
// loopback`, but toggleable instead of fixed-duration). A worker thread pumps
// PCM chunks from the input stream straight to the output stream; the SDK's
// blocking Audio::loopback(seconds) cannot be stopped midway, so the pump is
// built from the standalone stream APIs instead.
class AudioLoopback {
 public:
  ~AudioLoopback() { stop(); }

  bool start(std::string *error) {
    if (opened_) {
      return true;
    }
    const tdl_app::AudioInputStreamConfig config;  // SDK defaults: 16k/1ch/16bit
    if (!audio_.openInputStream(config, error)) {
      return false;
    }
    if (!audio_.openOutputStream(config.io, error)) {
      audio_.closeInputStream();
      return false;
    }
    opened_ = true;
    stop_requested_.store(false);
    running_.store(true);
    worker_ = std::thread([this]() { pump(); });
    return true;
  }

  void stop() {
    stop_requested_.store(true);
    if (worker_.joinable()) {
      worker_.join();
    }
    if (opened_) {
      audio_.closeInputStream();
      audio_.closeOutputStream();
      opened_ = false;
    }
    running_.store(false);
  }

  bool running() const { return running_.load(); }

  // Called from the main loop: if the pump thread died on its own (device
  // errors), finish the teardown so the next toggle starts from a clean state.
  void poll() {
    if (opened_ && !running_.load()) {
      stop();
      std::cout << "audio loopback stopped\n";
    }
  }

 private:
  void pump() {
    std::string error;
    int fail_count = 0;
    while (!stop_requested_.load()) {
      tdl_app::AudioPcmChunk chunk;
      if (!audio_.readInputChunk(&chunk, &error) ||
          !audio_.writeOutputChunk(chunk, &error)) {
        if (++fail_count >= 5) {
          std::cerr << "audio loopback failed: " << error << "\n";
          break;
        }
        continue;
      }
      fail_count = 0;
    }
    running_.store(false);
  }

  tdl_app::Audio audio_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  bool opened_ = false;
};

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

// AI-channel preview (picture-in-picture): the raw 640x640 frame fed to the
// detector, shown as a small opaque thumbnail in the top-left corner so the
// operator can see exactly what the model is looking at, independent of the
// live background channel. Drawn before the family-specific overlay so
// detection boxes / classification text still render on top of it.
void drawAiPreview(cv::Mat *overlay, const tdl_app::Frame &frame) {
  cv::Mat bgr;
  std::string error;
  if (!camera_demo_support::frameToBgrMat(frame, &bgr, &error)) {
    return;
  }
  cv::Mat bgra;
  cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);

  const int ch = tdl_app::DualOsLayout::kLiveHeight;
  const int side = ch / 4;
  cv::resize(bgra, bgra, cv::Size(side, side), 0, 0, cv::INTER_AREA);

  const int margin = 20;
  const cv::Rect roi(margin, margin, side, side);
  bgra.copyTo((*overlay)(roi));
  cv::rectangle(*overlay, roi, cv::Scalar(255, 255, 255, 255), 2, cv::LINE_AA);
}

// Maps model-result coordinates (in the ai frame, including its letterbox
// black bars) onto the live/OSD canvas. The ai channel is produced by VPSS
// ASPECT_RATIO_AUTO: the live picture is fit into the ai frame with black
// padding, so mapping must subtract the padding offset before scaling.
struct CoordMap {
  double scale_x = 1.0;
  double scale_y = 1.0;
  double offset_x = 0.0;
  double offset_y = 0.0;

  double mapX(double x) const { return (x - offset_x) * scale_x; }
  double mapY(double y) const { return (y - offset_y) * scale_y; }
  cv::Point map(double x, double y) const {
    return cv::Point(static_cast<int>(mapX(x)), static_cast<int>(mapY(y)));
  }
};

CoordMap makeAiToLiveMap(int frame_w, int frame_h, int live_w, int live_h) {
  CoordMap map;
  frame_w = std::max(1, frame_w);
  frame_h = std::max(1, frame_h);
  // Content rect: the live-aspect picture fit inside the ai frame, centered.
  const double fit = std::min(static_cast<double>(frame_w) / live_w,
                              static_cast<double>(frame_h) / live_h);
  const double content_w = live_w * fit;
  const double content_h = live_h * fit;
  map.offset_x = (frame_w - content_w) / 2.0;
  map.offset_y = (frame_h - content_h) / 2.0;
  map.scale_x = live_w / content_w;
  map.scale_y = live_h / content_h;
  return map;
}

// Detection: boxes + class label + confidence.
void drawDetection(cv::Mat *overlay, const std::vector<tdl_app::Box> &boxes,
                   const std::vector<std::string> &labels,
                   const CoordMap &map) {
  const cv::Scalar green(0, 255, 0, 255);
  for (const auto &box : boxes) {
    const float lx1 = static_cast<float>(map.mapX(box.x1));
    const float ly1 = static_cast<float>(map.mapY(box.y1));
    const float lx2 = static_cast<float>(map.mapX(box.x2));
    const float ly2 = static_cast<float>(map.mapY(box.y2));
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
                  const CoordMap &map) {
  const cv::Scalar skeleton(0, 255, 255, 255);
  auto mapped = [&](const tdl_app::Point &pt) { return map.map(pt.x, pt.y); };

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
                     const CoordMap &map, bool draw_mask) {
  for (std::size_t i = 0; i < result.instances.size(); ++i) {
    const auto &instance = result.instances[i];
    const cv::Scalar color = paletteColor(
        instance.box.class_id >= 0 ? instance.box.class_id
                                   : static_cast<int>(i));

    if (draw_mask && !instance.outline.empty()) {
      std::vector<cv::Point> poly;
      poly.reserve(instance.outline.size());
      for (const auto &pt : instance.outline) {
        poly.push_back(map.map(pt.x, pt.y));
      }
      const std::vector<std::vector<cv::Point>> polys{poly};
      cv::fillPoly(*overlay, polys, color, cv::LINE_AA);
    }

    if (!instance.outline.empty()) {
      std::vector<cv::Point> poly;
      poly.reserve(instance.outline.size());
      for (const auto &pt : instance.outline) {
        poly.push_back(map.map(pt.x, pt.y));
      }
      const std::vector<std::vector<cv::Point>> polys{poly};
      cv::polylines(*overlay, polys, true, color, 2, cv::LINE_AA);
    }

    cv::Point p1 = map.map(instance.box.x1, instance.box.y1);
    cv::Point p2 = map.map(instance.box.x2, instance.box.y2);
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
             const CoordMap &map) {
  const cv::Scalar green(0, 255, 0, 255);
  const cv::Scalar white(255, 255, 255, 255);
  const std::string prefix = "ocr_text:";
  for (std::size_t i = 0; i < result.boxes.size(); ++i) {
    const auto &box = result.boxes[i];
    cv::Point p1 = map.map(box.x1, box.y1);
    cv::Point p2 = map.map(box.x2, box.y2);
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

  Family family = detectFamily(opt.model_spec);
  std::cerr << "model family=" << familyName(family) << "\n";

  AlgorithmSet algorithms;
  if (!algorithms.load(
          family,
          tdl_app::ModelSessionConfig::fromSpec(opt.model_spec, opt.firmware),
          &error)) {
    std::cerr << "model load failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 3;
  }
  // Declared after `algorithms` so the loader thread is joined before the
  // algorithm objects are destroyed.
  ModelLoader loader;

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

  // Interactive keyboard control only makes sense on a real terminal. When
  // stdin is not a tty (nohup / backgrounded / redirected), skip the raw-mode
  // setup and rely on the control pipe alone.
  TerminalGuard terminal;
  const bool stdin_is_tty = ::isatty(STDIN_FILENO) == 1;
  if (stdin_is_tty && !prepareTerminal(&terminal, &error)) {
    std::cerr << error << "\n";
    region.detach();
    region.destroy();
    unbindLinks(&preview_link, &display_link);
    vo.close();
    camera_demo_support::closeCameraRuntime(&runtime);
    return 8;
  }

  ControlPipe control_pipe;
  if (!control_pipe.open(opt.control_pipe, &error)) {
    std::cerr << "control pipe disabled: " << error << "\n";
  } else {
    std::cout << "control pipe: " << opt.control_pipe << "\n"
              << "  echo t > " << opt.control_pipe << "    # toggle stream\n"
              << "  echo a > " << opt.control_pipe << "    # toggle audio\n"
              << "  echo \"m /path/to/model.mud\" > " << opt.control_pipe
              << "    # switch model\n"
              << "  echo q > " << opt.control_pipe << "    # quit\n";
  }

  std::cout << "sophpi_ai_osd_demo running "
            << "(press m to switch model, q to quit)\n";

  tdl_app::InferOptions infer_options;
  infer_options.threshold = opt.threshold;
  infer_options.top_k = opt.top_k;

  const int cw = tdl_app::DualOsLayout::kLiveWidth;
  const int ch = tdl_app::DualOsLayout::kLiveHeight;

  bool visible = false;
  cv::Mat overlay;
  double fps = 0.0;
  auto last = std::chrono::steady_clock::now();

  // Runtime model switching state. `model_ready` is false between the old
  // model being unloaded and the new one finishing its background load; the
  // loop keeps running (live video + OSD status) but skips inference.
  bool model_ready = true;
  std::string status_text;
  CommandInput command;

  // Validate the typed spec first (a typo must not kill the current model),
  // then unload the old model and load the new one on the worker thread.
  auto requestModelSwitch = [&](const std::string &line) {
    const std::string spec = trimCopy(line);
    if (spec.empty()) {
      return;
    }
    tdl_app::ModelDescriptor descriptor;
    std::string desc_error;
    if (!tdl_app::loadModelDescriptor(spec, &descriptor, &desc_error)) {
      std::cout << "invalid model-spec: " << desc_error << "\n";
      return;
    }
    const Family next = detectFamily(spec);
    if (model_ready) {
      algorithms.reset(family);
      model_ready = false;
    }
    std::cout << "loading " << spec << " (family=" << familyName(next)
              << ")...\n";
    loader.start(&algorithms, next,
                 tdl_app::ModelSessionConfig::fromSpec(spec, opt.firmware));
    status_text = "Loading model...";
  };

  // MJPEG streaming state ('t' toggles, default off). The stream is fed from
  // the idle subrgb channel (grp0/ch3, NV21): the hardware MJPEG encoder only
  // handles YUV input, so the ai channel's RGB planar frames cannot be sent
  // to it directly (they get misinterpreted as YUV: green cast + purple bars).
  mjpeg_server::MjpegServer stream_server;
  tdl_app::Camera stream_camera(tdl_app::Camera::subRgb(500));
  tdl_app::VencChannel stream_venc(tdl_app::VencChannel::mjpeg(
      0, tdl_app::DualOsLayout::kSubRgbWidth,
      tdl_app::DualOsLayout::kSubRgbHeight));
  bool stream_on = false;
  int stream_fail_count = 0;
  std::vector<std::uint8_t> stream_jpeg;

  auto stopStream = [&]() {
    if (!stream_on) {
      return;
    }
    stream_server.stop();
    stream_venc.close();
    stream_camera.close();
    stream_on = false;
    std::cout << "stream off\n";
  };

  auto startStream = [&]() {
    std::string stream_error;
    if (!stream_camera.open(&stream_error)) {
      std::cout << "stream camera open failed: " << stream_error << "\n";
      return;
    }
    if (!stream_venc.open(&stream_error)) {
      std::cout << "stream venc open failed: " << stream_error << "\n";
      stream_camera.close();
      return;
    }
    if (!stream_server.start(opt.stream_port, &stream_error)) {
      std::cout << "stream server start failed: " << stream_error << "\n";
      stream_venc.close();
      stream_camera.close();
      return;
    }
    stream_on = true;
    stream_fail_count = 0;
    const auto addresses = listIpv4Addresses();
    if (addresses.empty()) {
      std::cout << "stream on: http://<board-ip>:" << opt.stream_port << "\n";
    } else {
      for (const auto &address : addresses) {
        std::cout << "stream on: http://" << address << ":" << opt.stream_port
                  << "\n";
      }
    }
  };

  // Mic -> speaker loopback ('a' toggles, default off).
  AudioLoopback audio_loopback;

  auto toggleStream = [&]() {
    if (stream_on) {
      stopStream();
    } else {
      startStream();
    }
  };

  if (opt.start_stream) {
    startStream();
  }

  auto toggleAudio = [&]() {
    if (audio_loopback.running()) {
      audio_loopback.stop();
      std::cout << "audio loopback off\n";
    } else {
      std::string audio_error;
      if (audio_loopback.start(&audio_error)) {
        std::cout << "audio loopback on (mic -> speaker)\n";
      } else {
        std::cout << "audio loopback start failed: " << audio_error << "\n";
      }
    }
  };

  // One control-pipe line = one command; mirrors the interactive keys.
  auto handleControlLine = [&](const std::string &raw) {
    const std::string line = trimCopy(raw);
    if (line.empty()) {
      return;
    }
    if (line == "q") {
      g_running.store(false);
    } else if (line == "t") {
      toggleStream();
    } else if (line == "a") {
      toggleAudio();
    } else if (line.compare(0, 2, "m ") == 0) {
      if (loader.active()) {
        std::cout << "model is still loading, please wait\n";
      } else {
        requestModelSwitch(line.substr(2));
      }
    } else {
      std::cout << "unknown control command: " << line << "\n";
    }
  };

  // Single loop: read the ai-channel frame, run the family's inference, render
  // the OSD overlay straight from the result, and push it to the canvas. No
  // separate inference thread; the OSD refresh rate is simply the model
  // throughput (capture + inference + render, all in one loop).
  while (g_running.load()) {
    // Drain every pending keystroke so typing a path stays responsive even
    // though this loop only iterates once per frame.
    for (int key = readKey(); key != -1; key = readKey()) {
      if (command.active) {
        std::string line;
        if (command.feed(key, &line)) {
          requestModelSwitch(line);
        }
        continue;
      }
      if (key == 'q') {
        g_running.store(false);
      } else if (key == 'm') {
        if (loader.active()) {
          std::cout << "\nmodel is still loading, please wait\n";
        } else {
          command.begin();
        }
      } else if (key == 't') {
        toggleStream();
      } else if (key == 'a') {
        toggleAudio();
      }
    }

    // Commands arriving over the control FIFO (echo t > /tmp/...).
    std::vector<std::string> control_lines;
    control_pipe.readLines(&control_lines);
    for (const auto &line : control_lines) {
      handleControlLine(line);
    }

    if (!g_running.load()) {
      break;
    }

    audio_loopback.poll();

    if (loader.finished()) {
      const Family next = loader.family();
      const std::string spec = loader.spec();
      std::string load_error;
      if (loader.take(&load_error)) {
        family = next;
        model_ready = true;
        status_text.clear();
        std::cout << "model switched: " << spec << " (family="
                  << familyName(family) << ")\n";
      } else {
        status_text = "Model load failed (press m to retry)";
        std::cout << "model load failed: " << load_error << "\n"
                  << "press m to enter another model-spec\n";
      }
    }

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

    if (stream_on) {
      tdl_app::Frame stream_frame;
      tdl_app::VencChannel::EncodedPacket packet;
      bool stream_ok = stream_camera.read(&stream_frame, &error);
      if (stream_ok) {
        stream_ok = stream_venc.encode(stream_frame, &packet, &error);
      }
      if (!stream_ok) {
        std::cerr << "stream frame failed: " << error << "\n";
        if (++stream_fail_count >= 5) {
          std::cout << "stream disabled after repeated failures\n";
          stopStream();
        }
      } else if (!packet.blocks.empty()) {
        stream_fail_count = 0;
        stream_jpeg.clear();
        for (const auto &block : packet.blocks) {
          stream_jpeg.insert(stream_jpeg.end(), block.begin(), block.end());
        }
        stream_server.publish(stream_jpeg.data(), stream_jpeg.size());
      }
    }

    tdl_app::AlgorithmResult result;
    tdl_app::KeypointResult kp_result;
    tdl_app::InstanceSegmentationResult seg_result;

    if (model_ready) {
      const auto infer_start = std::chrono::steady_clock::now();
      bool infer_ok = false;
      switch (family) {
        case Family::Detection:
          infer_ok =
              algorithms.detector.run(*video, infer_options, &result, &error);
          break;
        case Family::Classification:
          infer_ok = algorithms.classifier.runFrame(frame, infer_options,
                                                    &result, &error);
          break;
        case Family::Keypoint:
          infer_ok = algorithms.keypoint.runFrame(frame, &kp_result, &error);
          break;
        case Family::InstanceSeg:
          infer_ok = algorithms.segmenter.runFrame(frame, &seg_result, &error);
          break;
        case Family::Ocr:
          infer_ok =
              algorithms.plate.runFrame(frame, infer_options, &result, &error);
          break;
      }
      const double infer_ms =
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - infer_start)
              .count();
      // std::cerr << "infer took " << cv::format("%.2f", infer_ms) << " ms\n";
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
    } else {
      // No model loaded (background switch in progress or last load failed):
      // keep the FPS estimate frozen and let the loop idle a little.
      last = std::chrono::steady_clock::now();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const CoordMap coord_map =
        makeAiToLiveMap(frame.width, frame.height, cw, ch);
    beginOverlay(&overlay);
    // drawAiPreview(&overlay, frame);
    if (model_ready) {
      switch (family) {
        case Family::Detection:
          drawDetection(&overlay, result.boxes, result.labels, coord_map);
          break;
        case Family::Classification:
          drawClassification(&overlay, result, opt.top_k);
          break;
        case Family::Keypoint:
          drawKeypoint(&overlay, kp_result, coord_map);
          break;
        case Family::InstanceSeg:
          drawInstanceSeg(&overlay, seg_result, coord_map, seg_draw_mask);
          break;
        case Family::Ocr:
          drawOcr(&overlay, result, coord_map);
          break;
      }
    }
    if (!status_text.empty()) {
      putLabel(overlay, status_text, cv::Point(24, 90), 0.8,
               cv::Scalar(0, 255, 255, 255));
    }
    finishOverlay(&overlay, logo, fps);

    if (!pushToCanvas(&region, overlay, &visible, &error)) {
      std::cerr << "osd push failed: " << error << "\n";
    }
  }

  audio_loopback.stop();
  stopStream();

  // Make sure a still-running background load finishes before the media
  // pipeline below is torn down.
  loader.join();

  restoreTerminal(&terminal);
  region.detach();
  region.destroy();
  unbindLinks(&preview_link, &display_link);
  vo.close();
  camera_demo_support::closeCameraRuntime(&runtime);
  std::cout << "sophpi_ai_osd_demo exit\n";
  return 0;
}
