#include <cstdlib>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  std::string template_image;
  std::string search_image;
  std::string model_spec;
  std::string firmware;
  std::string model_dir;
  std::string output;
  tdl_app::Box init_box;
  bool has_init_box = false;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_single_object_tracker_demo --template-image FILE --search-image FILE\n"
      << "                                 --model-spec FILE --init-box x1,y1,x2,y2\n"
      << "                                 [--firmware FILE] [--model-dir DIR] [--output FILE]\n";
}

bool parseBox(const std::string &text, tdl_app::Box *box) {
  if (!box) {
    return false;
  }
  float x1 = 0.0f;
  float y1 = 0.0f;
  float x2 = 0.0f;
  float y2 = 0.0f;
  if (std::sscanf(text.c_str(), "%f,%f,%f,%f", &x1, &y1, &x2, &y2) != 4) {
    return false;
  }
  box->x1 = x1;
  box->y1 = y1;
  box->x2 = x2;
  box->y2 = y2;
  return true;
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

    if (arg == "--template-image") {
      const char *value = requireValue("--template-image");
      if (!value) return false;
      opt->template_image = value;
    } else if (arg == "--search-image") {
      const char *value = requireValue("--search-image");
      if (!value) return false;
      opt->search_image = value;
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
    } else if (arg == "--init-box") {
      const char *value = requireValue("--init-box");
      if (!value) return false;
      if (!parseBox(value, &opt->init_box)) {
        std::cerr << "invalid --init-box, expected x1,y1,x2,y2\n";
        return false;
      }
      opt->has_init_box = true;
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }

  if (opt->template_image.empty()) {
    std::cerr << "template-image is required\n";
    return false;
  }
  if (opt->search_image.empty()) {
    std::cerr << "search-image is required\n";
    return false;
  }
  if (opt->model_spec.empty()) {
    std::cerr << "model-spec is required\n";
    return false;
  }
  if (!opt->has_init_box) {
    std::cerr << "init-box is required\n";
    return false;
  }
  return true;
}

bool saveAnnotatedImage(const std::string &image_path, const std::string &output,
                        const tdl_app::Box &init_box,
                        const tdl_app::SingleObjectTrackingResult &result,
                        std::string *error) {
  cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) {
      *error = "failed to read image for annotation: " + image_path;
    }
    return false;
  }

  cv::rectangle(image,
                cv::Point(static_cast<int>(result.box.x1),
                          static_cast<int>(result.box.y1)),
                cv::Point(static_cast<int>(result.box.x2),
                          static_cast<int>(result.box.y2)),
                cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
  const std::string text = cv::format("score=%.3f", result.confidence);
  cv::putText(image, text,
              cv::Point(static_cast<int>(result.box.x1),
                        std::max(12, static_cast<int>(result.box.y1) - 6)),
              cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1,
              cv::LINE_AA);

  if (!cv::imwrite(output, image)) {
    if (error) {
      *error = "failed to write output image: " + output;
    }
    return false;
  }
  static_cast<void>(init_box);
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  tdl_app::SingleObjectTracker tracker;
  std::string error;
  if (!tracker.load(tdl_app::ModelSessionConfig::fromSpec(
                        opt.model_spec, opt.firmware, opt.model_dir),
                    &error)) {
    std::cerr << "load failed: " << error << "\n";
    return 2;
  }
  if (!tracker.initialize(opt.template_image, opt.init_box, &error)) {
    std::cerr << "initialize failed: " << error << "\n";
    return 3;
  }

  tdl_app::SingleObjectTrackingResult result;
  if (!tracker.run(opt.search_image, &result, &error)) {
    std::cerr << "run failed: " << error << "\n";
    return 4;
  }

  std::cout << "score: " << result.confidence << "\n";
  std::cout << "box: (" << result.box.x1 << "," << result.box.y1 << ","
            << result.box.x2 << "," << result.box.y2 << ")\n";

  if (!opt.output.empty()) {
    if (!saveAnnotatedImage(opt.search_image, opt.output, opt.init_box, result,
                            &error)) {
      std::cerr << "save failed: " << error << "\n";
      return 5;
    }
    std::cout << "saved: " << opt.output << "\n";
  }
  return 0;
}
