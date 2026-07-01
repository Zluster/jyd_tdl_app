#include <algorithm>
#include <atomic>
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

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  sophpi_ai_osd_demo --model-spec FILE\n"
      << "                     [--firmware FILE] [--threshold 0.25]\n"
      << "                     [--logo /root/logo.png]\n"
      << "                     [--screen-width N] [--screen-height N]\n";
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

// Convert the ai-channel frame into a BGR cv::Mat so it can be drawn as the
// top-left picture-in-picture "ai data source" preview.
bool frameToBgr(const tdl_app::Frame &frame, cv::Mat *out, std::string *error) {
  if (!out) {
    if (error) *error = "output image is null";
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
    if (error) *error = "invalid ai frame";
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
  if (format == PIXEL_FORMAT_RGB_888_PLANAR ||
      format == PIXEL_FORMAT_BGR_888_PLANAR) {
    cv::Mat p0(height, width, CV_8UC1);
    cv::Mat p1(height, width, CV_8UC1);
    cv::Mat p2(height, width, CV_8UC1);
    unsigned char *b0 = mapped;
    unsigned char *b1 = mapped + vf.u32Length[0];
    unsigned char *b2 = mapped + vf.u32Length[0] + vf.u32Length[1];
    for (int y = 0; y < height; ++y) {
      std::memcpy(p0.ptr(y), b0 + y * vf.u32Stride[0], width);
      std::memcpy(p1.ptr(y), b1 + y * vf.u32Stride[1], width);
      std::memcpy(p2.ptr(y), b2 + y * vf.u32Stride[2], width);
    }
    if (format == PIXEL_FORMAT_RGB_888_PLANAR) {
      cv::merge(std::vector<cv::Mat>{p2, p1, p0}, *out);
    } else {
      cv::merge(std::vector<cv::Mat>{p0, p1, p2}, *out);
    }
  } else if (format == PIXEL_FORMAT_RGB_888 || format == PIXEL_FORMAT_BGR_888) {
    cv::Mat packed(height, width, CV_8UC3, mapped,
                   static_cast<std::size_t>(vf.u32Stride[0]));
    if (format == PIXEL_FORMAT_RGB_888) {
      cv::cvtColor(packed, *out, cv::COLOR_RGB2BGR);
    } else {
      packed.copyTo(*out);
    }
  } else if (format == PIXEL_FORMAT_NV12 || format == PIXEL_FORMAT_NV21) {
    cv::Mat y_plane(height, width, CV_8UC1, mapped,
                    static_cast<std::size_t>(vf.u32Stride[0]));
    cv::Mat uv_plane(height / 2, width / 2, CV_8UC2, mapped + vf.u32Length[0],
                     static_cast<std::size_t>(vf.u32Stride[1]));
    const int code = format == PIXEL_FORMAT_NV21 ? cv::COLOR_YUV2BGR_NV21
                                                 : cv::COLOR_YUV2BGR_NV12;
    cv::cvtColorTwoPlane(y_plane, uv_plane, *out, code);
  } else if (format == PIXEL_FORMAT_YUV_400) {
    cv::Mat gray(height, width, CV_8UC1);
    for (int y = 0; y < height; ++y) {
      std::memcpy(gray.ptr(y), mapped + y * vf.u32Stride[0], width);
    }
    cv::cvtColor(gray, *out, cv::COLOR_GRAY2BGR);
  } else {
    ok = false;
    if (error) {
      *error = "unsupported ai frame format: " + std::to_string(format);
    }
  }

  CVI_SYS_Munmap(mapped, map_size);
  return ok && !out->empty();
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

// Compose the whole overlay directly in the OSD canvas's native landscape
// orientation (same as the live VPSS channel, before VO). VO already applies
// a hardware rotation (CVI_VO_SetChnRotation) to this channel's whole
// composited output (video + attached OSD region), so there is no need to
// pre-rotate the overlay in software: the hardware rotation handles both the
// live video and this overlay together.
void renderOverlay(cv::Mat *overlay, const Options & /*opt*/,
                   const std::vector<tdl_app::Box> &boxes,
                   const std::vector<std::string> &labels, int frame_w,
                   int frame_h, const cv::Mat &logo, const cv::Mat &ai_image,
                   double fps) {
  const int cw = tdl_app::DualOsLayout::kLiveWidth;   // 1280
  const int ch = tdl_app::DualOsLayout::kLiveHeight;  // 720
  overlay->create(ch, cw, CV_8UC4);
  overlay->setTo(cv::Scalar(0, 0, 0, 0));

  const cv::Scalar green(0, 255, 0, 255);
  const cv::Scalar blue(255, 0, 0, 255);
  const cv::Scalar red(0, 0, 255, 255);
  const cv::Scalar white(255, 255, 255, 255);

  const double scale_x =
      static_cast<double>(cw) / std::max(1, frame_w);
  const double scale_y =
      static_cast<double>(ch) / std::max(1, frame_h);

  // Detection boxes + class + confidence.
  // Commented out for a performance test (perf: how much does box/text
  // drawing cost per frame).
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

  // Logo bottom-right (respects its own alpha), drawn before borders so the
  // border stays visible over the logo edge.
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

  // Screen-edge borders: thick blue first, thin red on top of it.
  const cv::Rect frame_rect(4, 4, cw - 8, ch - 8);
  cv::rectangle(*overlay, frame_rect, blue, 8);
  cv::rectangle(*overlay, frame_rect, red, 2);

  // Top-left: the ai data source itself, drawn as a small preview, plus a
  // label. No rotation needed here either: VO rotates this whole overlay
  // together with the video, so the thumbnail is drawn as-is.
  const int margin = 24;
  int text_y = margin + 24;
  // if (!ai_image.empty()) {
  //   const int side = ch / 3;
  //   cv::Mat thumb;
  //   cv::resize(ai_image, thumb, cv::Size(side, side), 0, 0, cv::INTER_AREA);
  //   cv::Mat thumb_bgra;
  //   cv::cvtColor(thumb, thumb_bgra, cv::COLOR_BGR2BGRA);
  //   const cv::Rect roi(margin, margin, side, side);
  //   thumb_bgra.copyTo((*overlay)(roi));
  //   cv::rectangle(*overlay, roi, green, 2);
  //   putLabel(*overlay, "AI SRC", cv::Point(margin + 4, margin + side + 22), 0.6,
  //            white);
  //   text_y = margin + side + 52;
  // }

  // Frame rate of the merged capture+infer+render loop.
  putLabel(*overlay, "FPS: " + cv::format("%.1f", fps),
           cv::Point(margin, text_y), 0.6, white);
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

  tdl_app::Detector::Config det_config;
  det_config.model_spec = opt.model_spec;
  det_config.firmware = opt.firmware;
  tdl_app::Detector detector(det_config, &error);
  if (!detector.initialized()) {
    std::cerr << "detector load failed: " << error << "\n";
    camera_demo_support::closeCameraRuntime(&runtime);
    return 3;
  }
  std::cerr << "detector loaded model_type=" << detector.modelType() << "\n";

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

  bool visible = false;
  cv::Mat overlay;
  double fps = 0.0;
  auto last = std::chrono::steady_clock::now();

  // Single loop: read the ai-channel frame, run detection, render the OSD
  // overlay straight from the result, and push it to the canvas. No separate
  // inference thread; the OSD refresh rate is simply the detector throughput.
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
    const auto detect_start = std::chrono::steady_clock::now();
    const bool detect_ok = detector(*video, infer_options, &result, &error);
    const double detect_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - detect_start)
            .count();
    std::cerr << "detect took " << cv::format("%.2f", detect_ms) << " ms\n";
    if (!detect_ok) {
      std::cerr << "detector run failed: " << error << "\n";
      continue;
    }

    cv::Mat ai_bgr;
    std::string convert_error;
    if (!frameToBgr(frame, &ai_bgr, &convert_error)) {
      ai_bgr.release();
    }

    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last).count();
    last = now;
    if (dt > 0.0) {
      const double inst = 1.0 / dt;
      fps = fps <= 0.0 ? inst : fps * 0.8 + inst * 0.2;
    }

    renderOverlay(&overlay, opt, result.boxes, result.labels, frame.width,
                  frame.height, logo, cv::Mat(), fps);
    // renderOverlay(&overlay, opt, result.boxes, result.labels, frame.width,
    //                 frame.height, logo, ai_bgr, fps);
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
