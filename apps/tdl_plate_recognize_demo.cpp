#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "demo_support.hpp"
#include "image_demo_support.hpp"
#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  image_demo_support::CommonOptions common;
  bool camera = false;
  bool has_roi = false;
  tdl_app::Box roi;
  int group = 0;
  int channel = 1;
  int timeout_ms = 1000;
  int frames = 1;
  int warmup = 0;
  std::string dump_frame;
  std::string dump_overlay;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_plate_recognize_demo (--image FILE | --camera) --model-spec FILE\n"
      << "                           [--firmware FILE] [--roi x,y,w,h]\n"
      << "                           [--output FILE] [--group N] [--channel N]\n"
      << "                           [--warmup N] [--frames N]\n"
      << "                           [--dump-frame FILE] [--dump-overlay FILE]\n";
}

bool parseRoi(const std::string &value, tdl_app::Box *roi) {
  if (!roi) {
    return false;
  }
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
  if (std::sscanf(value.c_str(), "%f,%f,%f,%f", &x, &y, &w, &h) != 4) {
    return false;
  }
  roi->x1 = x;
  roi->y1 = y;
  roi->x2 = x + w;
  roi->y2 = y + h;
  return true;
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    std::string parse_error;
    auto value = [&](const char *name) -> const char * {
      return image_demo_support::valueForArg(argc, argv, &i, name, &parse_error);
    };
    if (arg == "--camera") { opt->camera = true; continue; }
    if (arg == "--group") { const char *v = value("--group"); if (!v) return false; opt->group = std::atoi(v); continue; }
    if (arg == "--channel") { const char *v = value("--channel"); if (!v) return false; opt->channel = std::atoi(v); continue; }
    if (arg == "--timeout-ms") { const char *v = value("--timeout-ms"); if (!v) return false; opt->timeout_ms = std::atoi(v); continue; }
    if (arg == "--frames") { const char *v = value("--frames"); if (!v) return false; opt->frames = std::atoi(v); continue; }
    if (arg == "--warmup") { const char *v = value("--warmup"); if (!v) return false; opt->warmup = std::atoi(v); continue; }
    if (arg == "--dump-frame") { const char *v = value("--dump-frame"); if (!v) return false; opt->dump_frame = v; continue; }
    if (arg == "--dump-overlay") { const char *v = value("--dump-overlay"); if (!v) return false; opt->dump_overlay = v; continue; }
    bool handled = false;
    if (!image_demo_support::parseCommonArgs(argc, argv, &i, &opt->common,
                                             &handled, &parse_error)) {
      std::cerr << parse_error << "\n";
      return false;
    }
    if (handled) {
      continue;
    }

    if (arg == "--roi") {
      const char *value =
          image_demo_support::valueForArg(argc, argv, &i, "--roi", &parse_error);
      if (!value || !parseRoi(value, &opt->roi)) {
        std::cerr << "invalid --roi value, expected x,y,w,h\n";
        return false;
      }
      opt->has_roi = true;
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    }

    std::cerr << "unknown argument: " << arg << "\n";
    return false;
  }

  std::string error;
  if (opt->camera) {
    if (opt->common.model_spec.empty() || !opt->has_roi || opt->frames <= 0 ||
        opt->warmup < 0 || (!opt->dump_overlay.empty() && opt->dump_frame.empty())) {
      std::cerr << "camera mode requires --model-spec, --roi and valid frame/dump options\n";
      return false;
    }
    return true;
  }
  if (!image_demo_support::validateRequired(opt->common, true, &error)) {
    std::cerr << error << "\n";
    return false;
  }
  return true;
}

bool saveCameraOverlay(const std::string &input, const std::string &output,
                       const tdl_app::Box &roi, const std::string &text,
                       std::string *error) {
  cv::Mat image = cv::imread(input, cv::IMREAD_COLOR);
  if (image.empty()) { if (error) *error = "failed to read plate snapshot"; return false; }
  cv::rectangle(image, cv::Point(static_cast<int>(roi.x1), static_cast<int>(roi.y1)),
                cv::Point(static_cast<int>(roi.x2), static_cast<int>(roi.y2)),
                cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
  cv::putText(image, text.empty() ? "plate: <empty>" : text,
              cv::Point(static_cast<int>(roi.x1), std::max(18, static_cast<int>(roi.y1) - 5)),
              cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
  if (!cv::imwrite(output, image)) { if (error) *error = "failed to write plate overlay"; return false; }
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
  tdl_app::PlateRecognizer recognizer;
  if (!recognizer.load(image_demo_support::plateRecognizerConfig(opt.common),
                       &error)) {
    std::cerr << "initialize failed: " << error << "\n";
    return 2;
  }

  tdl_app::InferOptions infer_options;
  tdl_app::AlgorithmResult result;
  if (opt.camera) {
    const tdl_app::Camera::Config camera_config =
        opt.group == 0 && opt.channel == 1
            ? tdl_app::Camera::ai(opt.timeout_ms)
            : tdl_app::Camera::vpss(opt.group, opt.channel, 640, 640,
                                    tdl_app::PixelFormat::RGB888_PLANAR, opt.timeout_ms);
    tdl_app::Camera camera(camera_config);
    if (!camera.open(&error)) { std::cerr << "camera open failed: " << error << "\n"; return 3; }
    double read_sum = 0.0, infer_sum = 0.0, total_sum = 0.0;
    const int total_frames = opt.warmup + opt.frames;
    for (int index = 0; index < total_frames; ++index) {
      tdl_app::Frame frame;
      const auto total_begin = std::chrono::steady_clock::now();
      const auto read_begin = total_begin;
      if (!camera.read(&frame, &error)) { camera.close(); std::cerr << "camera read failed: " << error << "\n"; return 3; }
      const auto read_end = std::chrono::steady_clock::now();
      const auto infer_begin = read_end;
      const bool ok = recognizer.runFrame(frame, opt.roi, infer_options, &result, &error);
      const auto infer_end = std::chrono::steady_clock::now();
      if (ok && index == total_frames - 1 && !opt.dump_frame.empty()) {
        if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, &error) ||
            (!opt.dump_overlay.empty() && !saveCameraOverlay(opt.dump_frame, opt.dump_overlay, opt.roi, result.text, &error))) {
          camera.releaseFrame(); camera.close(); std::cerr << "save failed: " << error << "\n"; return 4;
        }
      }
      camera.releaseFrame();
      if (!ok) { camera.close(); std::cerr << "run failed: " << error << "\n"; return 3; }
      if (index >= opt.warmup) {
        read_sum += std::chrono::duration<double, std::milli>(read_end - read_begin).count();
        infer_sum += std::chrono::duration<double, std::milli>(infer_end - infer_begin).count();
        total_sum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - total_begin).count();
      }
    }
    camera.close();
    const double count = static_cast<double>(opt.frames);
    const double total = total_sum / count;
    std::cout << std::fixed << std::setprecision(3) << "camera_frames=" << opt.frames
              << " avg_read_ms=" << read_sum / count << " avg_infer_ms=" << infer_sum / count
              << " avg_total_ms=" << total << " fps=" << (total > 0.0 ? 1000.0 / total : 0.0)
              << " text=" << result.text << "\n";
    if (!opt.dump_frame.empty()) std::cout << "saved_frame=" << opt.dump_frame << "\n";
    if (!opt.dump_overlay.empty()) std::cout << "saved_overlay=" << opt.dump_overlay << "\n";
    return 0;
  }
  const bool ok = opt.has_roi
                      ? recognizer.run(opt.common.image, opt.roi, infer_options,
                                       &result, &error)
                      : recognizer.run(opt.common.image, infer_options, &result,
                                       &error);
  if (!ok) {
    std::cerr << "run failed: " << error << "\n";
    return 3;
  }

  if (!opt.common.output.empty()) {
    if (!image_demo_support::saveAnnotatedOutputIfRequested(opt.common, result,
                                                            &error)) {
      std::cerr << "save failed: " << error << "\n";
      return 4;
    }
    std::cout << "saved: " << opt.common.output << "\n";
  }

  demo_support::dumpResult(result);
  return 0;
}
