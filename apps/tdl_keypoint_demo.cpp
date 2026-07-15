#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

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
};

constexpr int kPose17Skeleton[][2] = {
    {0, 1},  {0, 2},  {1, 3},  {2, 4},  {5, 6},  {5, 7},  {7, 9},
    {6, 8},  {8, 10}, {5, 11}, {6, 12}, {11, 12}, {11, 13}, {13, 15},
    {12, 14}, {14, 16}};

constexpr int kHand21Skeleton[][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 4}, {0, 5}, {5, 6}, {6, 7}, {7, 8},
    {0, 9}, {9, 10}, {10, 11}, {11, 12}, {0, 13}, {13, 14}, {14, 15},
    {15, 16}, {0, 17}, {17, 18}, {18, 19}, {19, 20}};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_keypoint_demo (--image FILE | --camera) --model-spec FILE\n"
      << "                    [--firmware FILE] [--model-dir DIR] [--output FILE]\n"
      << "                    [--dump-frame FILE] [--dump-overlay FILE]\n"
      << "                    [--group N] [--channel N] [--timeout-ms N] [--frames N]\n";
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
  if (opt->camera && !opt->output.empty()) {
    std::cerr << "--output is only supported with --image; use --dump-overlay with --camera\n";
    return false;
  }
  if (!opt->dump_overlay.empty() && opt->dump_frame.empty()) {
    std::cerr << "--dump-overlay requires --dump-frame\n";
    return false;
  }
  if (opt->frames <= 0) {
    std::cerr << "--frames must be positive\n";
    return false;
  }
  return true;
}

bool saveAnnotatedImage(const std::string &image_path, const std::string &output,
                        const tdl_app::KeypointResult &result,
                        std::string *error) {
  cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) {
      *error = "failed to read image for annotation: " + image_path;
    }
    return false;
  }

  if (result.pointCount() == 17) {
    for (const auto &edge : kPose17Skeleton) {
      const int a = edge[0];
      const int b = edge[1];
      if (result.points[a].score <= 0.05f || result.points[b].score <= 0.05f) {
        continue;
      }
      cv::line(image,
               cv::Point(static_cast<int>(result.points[a].x),
                         static_cast<int>(result.points[a].y)),
               cv::Point(static_cast<int>(result.points[b].x),
                         static_cast<int>(result.points[b].y)),
               cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
    }
  }
  if (result.pointCount() == 21) {
    for (const auto &edge : kHand21Skeleton) {
      cv::line(image,
               cv::Point(static_cast<int>(result.points[edge[0]].x),
                         static_cast<int>(result.points[edge[0]].y)),
               cv::Point(static_cast<int>(result.points[edge[1]].x),
                         static_cast<int>(result.points[edge[1]].y)),
               cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
    }
  }

  for (std::size_t i = 0; i < result.points.size(); ++i) {
    const auto &point = result.points[i];
    const cv::Scalar color =
        point.score > 0.5f ? cv::Scalar(0, 0, 255) : cv::Scalar(255, 0, 0);
    cv::circle(image, cv::Point(static_cast<int>(point.x),
                                static_cast<int>(point.y)),
               3, color, cv::FILLED, cv::LINE_AA);
    cv::putText(image, std::to_string(i),
                cv::Point(static_cast<int>(point.x) + 4,
                          static_cast<int>(point.y) - 4),
                cv::FONT_HERSHEY_SIMPLEX, 0.35, cv::Scalar(0, 255, 0), 1,
                cv::LINE_AA);
  }

  if (!cv::imwrite(output, image)) {
    if (error) {
      *error = "failed to write output image: " + output;
    }
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

  tdl_app::KeypointDetector detector;
  std::string error;
  if (!detector.load(tdl_app::ModelSessionConfig::fromSpec(
                         opt.model_spec, opt.firmware, opt.model_dir),
                     &error)) {
    std::cerr << "load failed: " << error << "\n";
    return 2;
  }

  tdl_app::KeypointResult result;
  double total_read_ms = 0.0;
  double total_infer_ms = 0.0;
  double total_pipeline_ms = 0.0;
  std::string saved_frame;
  std::string saved_overlay;
  if (opt.camera) {
    tdl_app::Camera::Config camera_config =
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
    for (int index = 0; index < opt.frames; ++index) {
      tdl_app::Frame frame;
      const auto read_begin = std::chrono::steady_clock::now();
      if (!camera.read(&frame, &error)) {
        std::cerr << "camera read failed: " << error << "\n";
        camera.close();
        return 3;
      }
      const auto read_end = std::chrono::steady_clock::now();
      const auto infer_begin = std::chrono::steady_clock::now();
      const bool ok = detector.runFrame(frame, &result, &error);
      const auto infer_end = std::chrono::steady_clock::now();
      if (!ok) {
        std::cerr << "runFrame failed: " << error << "\n";
        camera.releaseFrame();
        camera.close();
        return 3;
      }
      if (index == opt.frames - 1 && !opt.dump_frame.empty()) {
        if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame,
                                                   &error)) {
          std::cerr << "failed to save frame: " << error << "\n";
          camera.releaseFrame();
          camera.close();
          return 4;
        }
        saved_frame = opt.dump_frame;
        if (!opt.dump_overlay.empty() &&
            !saveAnnotatedImage(saved_frame, opt.dump_overlay, result, &error)) {
          std::cerr << "failed to save overlay: " << error << "\n";
          camera.releaseFrame();
          camera.close();
          return 4;
        }
        saved_overlay = opt.dump_overlay;
      }
      camera.releaseFrame();
      const double read_ms =
          std::chrono::duration<double, std::milli>(read_end - read_begin).count();
      const double infer_ms =
          std::chrono::duration<double, std::milli>(infer_end - infer_begin).count();
      total_read_ms += read_ms;
      total_infer_ms += infer_ms;
      total_pipeline_ms += read_ms + infer_ms;
    }
    camera.close();
    std::cout << "camera_frames: " << opt.frames << "\n";
    const double avg_read_ms = total_read_ms / opt.frames;
    const double avg_infer_ms = total_infer_ms / opt.frames;
    const double avg_total_ms = total_pipeline_ms / opt.frames;
    std::cout << "avg_read_ms: " << avg_read_ms << "\n";
    std::cout << "avg_infer_ms: " << avg_infer_ms << "\n";
    std::cout << "avg_total_ms: " << avg_total_ms << "\n";
    std::cout << "fps: " << (avg_total_ms > 0.0 ? 1000.0 / avg_total_ms : 0.0)
              << "\n";
  } else if (!detector.run(opt.image, &result, &error)) {
    std::cerr << "run failed: " << error << "\n";
    return 3;
  }

  std::cout << "points: " << result.pointCount() << "\n";
  for (std::size_t i = 0; i < result.points.size(); ++i) {
    std::cout << "  [" << i << "] x=" << result.points[i].x
              << " y=" << result.points[i].y
              << " score=" << result.points[i].score << "\n";
  }

  if (!opt.output.empty()) {
    if (!saveAnnotatedImage(opt.image, opt.output, result, &error)) {
      std::cerr << "save failed: " << error << "\n";
      return 4;
    }
    std::cout << "saved: " << opt.output << "\n";
  }
  if (!saved_frame.empty()) {
    std::cout << "saved_frame: " << saved_frame << "\n";
  }
  if (!saved_overlay.empty()) {
    std::cout << "saved_overlay: " << saved_overlay << "\n";
  }
  return 0;
}
