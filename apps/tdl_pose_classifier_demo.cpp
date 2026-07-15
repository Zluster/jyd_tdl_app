#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "tdl_app/advanced.hpp"
#include "tdl_app/pose_classifier.hpp"

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSkeleton[][2] = {
    {0, 1}, {0, 2}, {1, 3}, {2, 4}, {5, 6}, {5, 7}, {7, 9},
    {6, 8}, {8, 10}, {5, 11}, {6, 12}, {11, 12}, {11, 13},
    {13, 15}, {12, 14}, {14, 16}};

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string image;
  std::string model_spec = "./configs/model_specs/pose_yolov8.mud";
  std::string firmware;
  std::string output;
  std::string dump_frame;
  std::string dump_overlay;
  float keypoint_threshold = 0.05f;
  float ema_alpha = 0.65f;
  int smooth_frames = 5;
  int warmup = 5;
};

double elapsedMs(const Clock::time_point &begin, const Clock::time_point &end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

void usage() {
  std::cout
      << "Usage:\n"
      << "  tdl_pose_classifier_demo [--image FILE | camera options]\n"
      << "      --model-spec FILE [--output FILE]\n"
      << "      [--keypoint-threshold 0.05] [--ema-alpha 0.65]\n"
      << "      [--smooth-frames 5] [--warmup 5] [--frames 300]\n"
      << "      [--group 0] [--channel 1]\n"
      << "      [--dump-frame FILE] [--dump-overlay FILE]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    bool handled = false;
    std::string error;
    if (!camera_demo_support::parseCommonArgs(argc, argv, &i, &opt->camera,
                                              &handled, &error)) {
      std::cerr << error << "\n"; return false;
    }
    if (handled) continue;
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n"; return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--camera") {
      continue;
    } else if (arg == "--image") {
      const char *v = value("--image"); if (!v) return false; opt->image = v;
    } else if (arg == "--model-spec") {
      const char *v = value("--model-spec"); if (!v) return false; opt->model_spec = v;
    } else if (arg == "--firmware") {
      const char *v = value("--firmware"); if (!v) return false; opt->firmware = v;
    } else if (arg == "--output") {
      const char *v = value("--output"); if (!v) return false; opt->output = v;
    } else if (arg == "--dump-frame") {
      const char *v = value("--dump-frame"); if (!v) return false; opt->dump_frame = v;
    } else if (arg == "--dump-overlay") {
      const char *v = value("--dump-overlay"); if (!v) return false; opt->dump_overlay = v;
    } else if (arg == "--keypoint-threshold") {
      const char *v = value("--keypoint-threshold"); if (!v) return false;
      opt->keypoint_threshold = static_cast<float>(std::atof(v));
    } else if (arg == "--ema-alpha") {
      const char *v = value("--ema-alpha"); if (!v) return false;
      opt->ema_alpha = static_cast<float>(std::atof(v));
    } else if (arg == "--smooth-frames") {
      const char *v = value("--smooth-frames"); if (!v) return false;
      opt->smooth_frames = std::atoi(v);
    } else if (arg == "--warmup") {
      const char *v = value("--warmup"); if (!v) return false; opt->warmup = std::atoi(v);
    } else if (arg == "-h" || arg == "--help") {
      usage(); std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n"; return false;
    }
  }
  if (!opt->image.empty() && opt->output.empty()) {
    std::cerr << "--image requires --output for functional verification\n";
    return false;
  }
  if (!opt->dump_overlay.empty() && opt->dump_frame.empty()) {
    std::cerr << "--dump-overlay requires --dump-frame\n"; return false;
  }
  return !opt->model_spec.empty() && opt->smooth_frames > 0 &&
         opt->camera.frames > 0 && opt->warmup >= 0;
}

bool drawOverlay(const std::string &input, const std::string &output,
                 const tdl_app::PoseClassificationResult &result,
                 float threshold, std::string *error) {
  cv::Mat image = cv::imread(input, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to read image for pose overlay: " + input;
    return false;
  }
  const auto &points = result.keypoints.points;
  if (points.size() == 17) {
    for (const auto &edge : kSkeleton) {
      const auto &a = points[edge[0]];
      const auto &b = points[edge[1]];
      if (a.score < threshold || b.score < threshold) continue;
      cv::line(image, cv::Point(static_cast<int>(a.x), static_cast<int>(a.y)),
               cv::Point(static_cast<int>(b.x), static_cast<int>(b.y)),
               cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
    }
    for (size_t i = 0; i < points.size(); ++i) {
      if (points[i].score < threshold) continue;
      cv::circle(image, cv::Point(static_cast<int>(points[i].x),
                                  static_cast<int>(points[i].y)),
                 4, cv::Scalar(0, 0, 255), cv::FILLED, cv::LINE_AA);
    }
  }
  const std::string text =
      std::string(tdl_app::humanPoseClassName(result.pose)) +
      cv::format(" %.2f", result.confidence);
  cv::rectangle(image, cv::Rect(8, 8, std::min(image.cols - 16, 310), 42),
                cv::Scalar(20, 20, 20), cv::FILLED);
  cv::putText(image, text, cv::Point(18, 38), cv::FONT_HERSHEY_SIMPLEX, 0.8,
              cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
  if (!cv::imwrite(output, image)) {
    if (error) *error = "failed to write pose overlay: " + output;
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) { usage(); return 1; }
  tdl_app::PoseClassifier::Config config;
  config.keypoint = tdl_app::ModelSessionConfig::fromSpec(
      opt.model_spec, opt.firmware);
  config.keypoint_threshold = opt.keypoint_threshold;
  config.coordinate_ema_alpha = opt.ema_alpha;
  config.label_smooth_frames = opt.smooth_frames;
  tdl_app::PoseClassifier classifier;
  std::string error;
  if (!classifier.load(config, &error)) {
    std::cerr << "load failed: " << error << "\n"; return 2;
  }

  if (!opt.image.empty()) {
    tdl_app::PoseClassificationResult result;
    if (!classifier.run(opt.image, &result, &error)) {
      std::cerr << "run failed: " << error << "\n"; return 3;
    }
    if (!drawOverlay(opt.image, opt.output, result, opt.keypoint_threshold, &error)) {
      std::cerr << "overlay failed: " << error << "\n"; return 4;
    }
    std::cout << "raw_pose=" << tdl_app::humanPoseClassName(result.raw_pose)
              << " pose=" << tdl_app::humanPoseClassName(result.pose)
              << " confidence=" << result.confidence
              << " keypoint_ms=" << result.profile.keypoint_ms
              << " total_ms=" << result.profile.total_ms
              << " saved_overlay=" << opt.output << "\n";
    return 0;
  }

  camera_demo_support::CameraRuntime camera;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &camera, &error)) {
    std::cerr << "camera open failed: " << error << "\n"; return 3;
  }
  double read_sum = 0.0, keypoint_sum = 0.0, coordinate_sum = 0.0;
  double rule_sum = 0.0, label_sum = 0.0, algorithm_sum = 0.0, total_sum = 0.0;
  tdl_app::PoseClassificationResult last;
  const int total_frames = opt.warmup + opt.camera.frames;
  for (int index = 0; index < total_frames; ++index) {
    if (index == opt.warmup) classifier.resetSmoothing();
    tdl_app::Frame frame;
    const auto read_begin = Clock::now();
    if (!camera.camera.read(&frame, &error)) {
      std::cerr << "camera read failed: " << error << "\n";
      camera_demo_support::closeCameraRuntime(&camera); return 4;
    }
    const auto read_end = Clock::now();
    tdl_app::PoseClassificationResult result;
    if (!classifier.runFrame(frame, &result, &error)) {
      std::cerr << "pose run failed: " << error << "\n";
      camera.camera.releaseFrame();
      camera_demo_support::closeCameraRuntime(&camera); return 5;
    }
    if (index == total_frames - 1 && !opt.dump_frame.empty()) {
      if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, &error) ||
          (!opt.dump_overlay.empty() &&
           !drawOverlay(opt.dump_frame, opt.dump_overlay, result,
                        opt.keypoint_threshold, &error))) {
        std::cerr << "effect image failed: " << error << "\n";
        camera.camera.releaseFrame();
        camera_demo_support::closeCameraRuntime(&camera); return 6;
      }
    }
    camera.camera.releaseFrame();
    if (index < opt.warmup) continue;
    const double read_ms = elapsedMs(read_begin, read_end);
    read_sum += read_ms;
    keypoint_sum += result.profile.keypoint_ms;
    coordinate_sum += result.profile.keypoint_smooth_ms;
    rule_sum += result.profile.rule_ms;
    label_sum += result.profile.label_smooth_ms;
    algorithm_sum += result.profile.total_ms;
    total_sum += read_ms + result.profile.total_ms;
    last = std::move(result);
  }
  camera_demo_support::closeCameraRuntime(&camera);
  const double count = static_cast<double>(opt.camera.frames);
  const double avg_total = total_sum / count;
  std::cout << std::fixed << std::setprecision(3)
            << "frames=" << opt.camera.frames
            << " raw_pose=" << tdl_app::humanPoseClassName(last.raw_pose)
            << " pose=" << tdl_app::humanPoseClassName(last.pose)
            << " confidence=" << last.confidence << "\n"
            << "avg_read_ms=" << read_sum / count
            << " avg_keypoint_ms=" << keypoint_sum / count
            << " avg_keypoint_smooth_ms=" << coordinate_sum / count
            << " avg_rule_ms=" << rule_sum / count
            << " avg_label_smooth_ms=" << label_sum / count
            << " avg_algorithm_ms=" << algorithm_sum / count
            << " avg_total_ms=" << avg_total
            << " fps=" << (avg_total > 0.0 ? 1000.0 / avg_total : 0.0) << "\n";
  if (!opt.dump_frame.empty()) std::cout << "saved_frame=" << opt.dump_frame << "\n";
  if (!opt.dump_overlay.empty()) std::cout << "saved_overlay=" << opt.dump_overlay << "\n";
  return 0;
}
