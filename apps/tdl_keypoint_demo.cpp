#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  std::string image;
  std::string model_spec;
  std::string firmware;
  std::string model_dir;
  std::string output;
};

constexpr int kPose17Skeleton[][2] = {
    {0, 1},  {0, 2},  {1, 3},  {2, 4},  {5, 6},  {5, 7},  {7, 9},
    {6, 8},  {8, 10}, {5, 11}, {6, 12}, {11, 12}, {11, 13}, {13, 15},
    {12, 14}, {14, 16}};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_keypoint_demo --image FILE --model-spec FILE\n"
      << "                    [--firmware FILE] [--model-dir DIR] [--output FILE]\n";
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
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  if (opt->image.empty()) {
    std::cerr << "image path is required\n";
    return false;
  }
  if (opt->model_spec.empty()) {
    std::cerr << "model-spec is required\n";
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
  if (!detector.run(opt.image, &result, &error)) {
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
  return 0;
}
