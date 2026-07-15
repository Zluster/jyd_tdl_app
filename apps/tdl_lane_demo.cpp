#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  std::string image;
  std::string model_spec;
  std::string firmware;
  std::string model_dir;
  std::string output;
  std::string dump_frame;
  std::string dump_overlay;
  bool camera = false;
  int group = 0;
  int channel = 1;
  int timeout_ms = 1000;
  int frames = 1;
  int warmup = 0;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_lane_demo (--image FILE | --camera) --model-spec FILE\n"
      << "                [--firmware FILE] [--model-dir DIR]\n"
      << "                [--output FILE] [--warmup N] [--frames N]\n"
      << "                [--group N] [--channel N] [--timeout-ms N]\n"
      << "                [--dump-frame FILE] [--dump-overlay FILE]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto requireValue = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--image") {
      const char *value = requireValue("--image");
      if (!value) return false;
      opt->image = value;
    } else if (arg == "--camera") {
      opt->camera = true;
    } else if (arg == "--model-spec") {
      const char *value = requireValue("--model-spec");
      if (!value) return false;
      opt->model_spec = value;
    } else if (arg == "--firmware") {
      const char *value = requireValue("--firmware");
      if (!value) return false;
      opt->firmware = value;
    } else if (arg == "--model-dir") {
      const char *value = requireValue("--model-dir");
      if (!value) return false;
      opt->model_dir = value;
    } else if (arg == "--output") {
      const char *value = requireValue("--output");
      if (!value) return false;
      opt->output = value;
    } else if (arg == "--dump-frame") {
      const char *value = requireValue("--dump-frame");
      if (!value) return false;
      opt->dump_frame = value;
    } else if (arg == "--dump-overlay") {
      const char *value = requireValue("--dump-overlay");
      if (!value) return false;
      opt->dump_overlay = value;
    } else if (arg == "--group") {
      const char *value = requireValue("--group");
      if (!value) return false;
      opt->group = std::atoi(value);
    } else if (arg == "--channel") {
      const char *value = requireValue("--channel");
      if (!value) return false;
      opt->channel = std::atoi(value);
    } else if (arg == "--timeout-ms") {
      const char *value = requireValue("--timeout-ms");
      if (!value) return false;
      opt->timeout_ms = std::atoi(value);
    } else if (arg == "--frames") {
      const char *value = requireValue("--frames");
      if (!value) return false;
      opt->frames = std::atoi(value);
    } else if (arg == "--warmup") {
      const char *value = requireValue("--warmup");
      if (!value) return false;
      opt->warmup = std::atoi(value);
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  if (!opt->camera && opt->image.empty()) {
    std::cerr << "--image or --camera is required\n";
    return false;
  }
  if (opt->model_spec.empty()) {
    std::cerr << "model-spec is required\n";
    return false;
  }
  if (opt->frames <= 0) {
    std::cerr << "--frames must be positive\n";
    return false;
  }
  if (opt->warmup < 0) {
    std::cerr << "--warmup must be non-negative\n";
    return false;
  }
  if (!opt->dump_overlay.empty() && opt->dump_frame.empty()) {
    std::cerr << "--dump-overlay requires --dump-frame\n";
    return false;
  }
  return true;
}

bool saveLaneOverlay(const std::string &image_path, const std::string &output,
                     const tdl_app::LaneDetectionResult &result,
                     std::string *error) {
  cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to read lane image: " + image_path;
    return false;
  }
  for (const auto &lane : result.lanes) {
    cv::line(image, cv::Point(static_cast<int>(std::lround(lane.start.x)),
                              static_cast<int>(std::lround(lane.start.y))),
             cv::Point(static_cast<int>(std::lround(lane.end.x)),
                       static_cast<int>(std::lround(lane.end.y))),
             cv::Scalar(0, 255, 0), 3, cv::LINE_AA);
  }
  if (!cv::imwrite(output, image)) {
    if (error) *error = "failed to write lane overlay: " + output;
    return false;
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

  tdl_app::LaneDetector detector;
  std::string error;
  if (!detector.load(tdl_app::ModelSessionConfig::fromSpec(
                         opt.model_spec, opt.firmware, opt.model_dir),
                     &error)) {
    std::cerr << "load failed: " << error << "\n";
    return 2;
  }

  tdl_app::LaneDetectionResult result;
  if (opt.camera) {
    const tdl_app::Camera::Config camera_config =
        opt.group == 0 && opt.channel == 1
            ? tdl_app::Camera::ai(opt.timeout_ms)
            : tdl_app::Camera::vpss(opt.group, opt.channel, 640, 640,
                                    tdl_app::PixelFormat::RGB888_PLANAR,
                                    opt.timeout_ms);
    tdl_app::Camera camera(camera_config);
    if (!camera.open(&error)) {
      std::cerr << "camera open failed: " << error << "\n";
      return 3;
    }
    double read_sum_ms = 0.0;
    double infer_sum_ms = 0.0;
    double total_sum_ms = 0.0;
    const int total_frames = opt.warmup + opt.frames;
    for (int index = 0; index < total_frames; ++index) {
      tdl_app::Frame frame;
      const auto total_begin = std::chrono::steady_clock::now();
      const auto read_begin = total_begin;
      if (!camera.read(&frame, &error)) {
        std::cerr << "camera read failed: " << error << "\n";
        camera.close();
        return 3;
      }
      const auto read_end = std::chrono::steady_clock::now();
      const auto begin = read_end;
      const bool ok = detector.runFrame(frame, &result, &error);
      const auto end = std::chrono::steady_clock::now();
      if (ok && index == total_frames - 1 && !opt.dump_frame.empty()) {
        if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, &error) ||
            (!opt.dump_overlay.empty() &&
             !saveLaneOverlay(opt.dump_frame, opt.dump_overlay, result, &error))) {
          std::cerr << "failed to save lane result: " << error << "\n";
          camera.releaseFrame();
          camera.close();
          return 4;
        }
      }
      camera.releaseFrame();
      if (!ok) {
        std::cerr << "runFrame failed: " << error << "\n";
        camera.close();
        return 3;
      }
      if (index >= opt.warmup) {
        read_sum_ms += std::chrono::duration<double, std::milli>(read_end - read_begin).count();
        infer_sum_ms += std::chrono::duration<double, std::milli>(end - begin).count();
        total_sum_ms += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - total_begin).count();
      }
    }
    camera.close();
    const double count = static_cast<double>(opt.frames);
    const double avg_total_ms = total_sum_ms / count;
    std::cout << std::fixed << std::setprecision(3)
              << "camera_frames=" << opt.frames
              << " avg_read_ms=" << read_sum_ms / count
              << " avg_infer_ms=" << infer_sum_ms / count
              << " avg_total_ms=" << avg_total_ms
              << " fps=" << (avg_total_ms > 0.0 ? 1000.0 / avg_total_ms : 0.0)
              << "\n";
  } else if (!detector.run(opt.image, &result, &error)) {
    std::cerr << "run failed: " << error << "\n";
    return 3;
  }

  const std::string overlay = opt.camera ? opt.dump_overlay : opt.output;
  const std::string source = opt.camera ? opt.dump_frame : opt.image;
  if (!overlay.empty() && !saveLaneOverlay(source, overlay, result, &error)) {
    std::cerr << "save overlay failed: " << error << "\n";
    return 4;
  }

  std::cout << "lanes: " << result.laneCount() << "\n";
  std::cout << "lane_state: " << result.lane_state << "\n";
  for (std::size_t i = 0; i < result.lanes.size(); ++i) {
    std::cout << "  [" << i << "] start=(" << result.lanes[i].start.x << ","
              << result.lanes[i].start.y << ") end=("
              << result.lanes[i].end.x << "," << result.lanes[i].end.y
              << ") score=" << result.lanes[i].score << "\n";
  }
  if (!opt.dump_frame.empty()) std::cout << "saved_frame=" << opt.dump_frame << "\n";
  if (!overlay.empty()) std::cout << "saved_overlay=" << overlay << "\n";
  return 0;
}
