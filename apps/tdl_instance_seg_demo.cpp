#include <chrono>
#include <cstdlib>
#include <iomanip>
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
  int device = 0;
  int group = 0;
  int channel = 1;
  int timeout_ms = 1000;
  int frames = 1;
  int warmup = 0;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_instance_seg_demo (--image FILE | --camera) --model-spec FILE\n"
      << "                        [--firmware FILE] [--model-dir DIR] [--output FILE]\n"
      << "                        [--device 0|1] [--group N] [--channel N] [--timeout-ms N]\n"
      << "                        [--warmup N] [--frames N]\n"
      << "                        [--dump-frame FILE] [--dump-overlay FILE]\n";
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
    } else if (arg == "--device") {
      const char *value = requireValue("--device");
      if (!value) return false;
      opt->device = std::atoi(value);
      if (opt->device != 0 && opt->device != 1) {
        std::cerr << "--device must be 0 (front) or 1 (rear)\n";
        return false;
      }
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
  if (opt->camera && !opt->output.empty()) {
    std::cerr << "--output requires --image so the overlay matches the input frame\n";
    return false;
  }
  if (opt->frames <= 0 || opt->warmup < 0) {
    std::cerr << "--frames must be positive and --warmup must be non-negative\n";
    return false;
  }
  if (!opt->dump_overlay.empty() && opt->dump_frame.empty()) {
    std::cerr << "--dump-overlay requires --dump-frame\n";
    return false;
  }
  return true;
}

cv::Scalar colorForIndex(int index) {
  static const cv::Scalar kPalette[] = {
      cv::Scalar(255, 56, 56),   cv::Scalar(255, 157, 151),
      cv::Scalar(255, 112, 31),  cv::Scalar(255, 178, 29),
      cv::Scalar(207, 210, 49),  cv::Scalar(72, 249, 10),
      cv::Scalar(146, 204, 23),  cv::Scalar(61, 219, 134),
      cv::Scalar(26, 147, 52),   cv::Scalar(0, 212, 187),
      cv::Scalar(44, 153, 168),  cv::Scalar(0, 194, 255),
      cv::Scalar(52, 69, 147),   cv::Scalar(100, 115, 255),
      cv::Scalar(0, 24, 236),    cv::Scalar(132, 56, 255),
      cv::Scalar(82, 0, 133),    cv::Scalar(203, 56, 255)};
  return kPalette[index % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

bool saveAnnotatedImage(const std::string &image_path, const std::string &output,
                        const tdl_app::InstanceSegmentationResult &result,
                        std::string *error) {
  cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) {
      *error = "failed to read image for annotation: " + image_path;
    }
    return false;
  }

  cv::Mat overlay = image.clone();
  for (std::size_t i = 0; i < result.instances.size(); ++i) {
    const auto &instance = result.instances[i];
    const cv::Scalar color = colorForIndex(instance.box.class_id >= 0
                                               ? instance.box.class_id
                                               : static_cast<int>(i));

    if (!instance.mask.empty() &&
        static_cast<int>(instance.mask.size()) ==
            result.mask_width * result.mask_height &&
        result.mask_width == image.cols && result.mask_height == image.rows) {
      for (int y = 0; y < image.rows; ++y) {
        cv::Vec3b *row = overlay.ptr<cv::Vec3b>(y);
        for (int x = 0; x < image.cols; ++x) {
          if (instance.mask[static_cast<size_t>(y * image.cols + x)] == 0) {
            continue;
          }
          row[x][0] = static_cast<unsigned char>(0.55 * row[x][0] + 0.45 * color[0]);
          row[x][1] = static_cast<unsigned char>(0.55 * row[x][1] + 0.45 * color[1]);
          row[x][2] = static_cast<unsigned char>(0.55 * row[x][2] + 0.45 * color[2]);
        }
      }
    }

    if (!instance.outline.empty()) {
      std::vector<cv::Point> contour;
      contour.reserve(instance.outline.size());
      for (const auto &point : instance.outline) {
        contour.emplace_back(static_cast<int>(point.x),
                             static_cast<int>(point.y));
      }
      const std::vector<std::vector<cv::Point>> contours{contour};
      cv::drawContours(overlay, contours, -1, color, 2, cv::LINE_AA);
    }

    const cv::Point p1(static_cast<int>(instance.box.x1),
                       static_cast<int>(instance.box.y1));
    const cv::Point p2(static_cast<int>(instance.box.x2),
                       static_cast<int>(instance.box.y2));
    cv::rectangle(overlay, p1, p2, color, 2, cv::LINE_AA);

    const std::string text = std::to_string(instance.box.class_id) + ":" +
                             cv::format("%.2f", instance.box.score);
    int baseline = 0;
    const cv::Size text_size =
        cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
    const int text_top = std::max(0, p1.y - text_size.height - 6);
    cv::rectangle(overlay, cv::Point(p1.x, text_top),
                  cv::Point(p1.x + text_size.width + 6,
                            text_top + text_size.height + baseline + 6),
                  color, cv::FILLED);
    cv::putText(overlay, text,
                cv::Point(p1.x + 3, text_top + text_size.height + 1),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 1,
                cv::LINE_AA);
  }

  if (!cv::imwrite(output, overlay)) {
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

  tdl_app::InstanceSegmenter segmenter;
  std::string error;
  if (!segmenter.load(tdl_app::ModelSessionConfig::fromSpec(
                          opt.model_spec, opt.firmware, opt.model_dir),
                      &error)) {
    std::cerr << "load failed: " << error << "\n";
    return 2;
  }

  tdl_app::InstanceSegmentationResult result;
  bool run_ok = false;
  if (opt.camera) {
    tdl_app::Camera::Config camera_config =
        opt.group == 0 && opt.channel == 1
            ? tdl_app::Camera::ai(opt.timeout_ms)
            : tdl_app::Camera::vpss(opt.group, opt.channel, 640, 640,
                                     tdl_app::PixelFormat::RGB888_PLANAR,
                                     opt.timeout_ms);
    camera_config.device = opt.device;
    tdl_app::Camera camera(camera_config);
    if (!camera.open(&error)) {
      std::cerr << "camera open failed: " << error << "\n";
      return 3;
    }
    const int total_frames = opt.warmup + opt.frames;
    double read_sum_ms = 0.0;
    double infer_sum_ms = 0.0;
    double total_sum_ms = 0.0;
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
      const auto infer_begin = read_end;
      run_ok = segmenter.runFrame(frame, &result, &error);
      const auto infer_end = std::chrono::steady_clock::now();
      if (run_ok && index == total_frames - 1 && !opt.dump_frame.empty()) {
        if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, &error) ||
            (!opt.dump_overlay.empty() &&
             !saveAnnotatedImage(opt.dump_frame, opt.dump_overlay, result, &error))) {
          std::cerr << "failed to save instance result: " << error << "\n";
          camera.releaseFrame();
          camera.close();
          return 4;
        }
      }
      camera.releaseFrame();
      if (!run_ok) break;
      if (index >= opt.warmup) {
        read_sum_ms += std::chrono::duration<double, std::milli>(read_end - read_begin).count();
        infer_sum_ms += std::chrono::duration<double, std::milli>(infer_end - infer_begin).count();
        total_sum_ms += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - total_begin).count();
      }
    }
    camera.close();
    if (run_ok) {
      const double count = static_cast<double>(opt.frames);
      const double avg_total = total_sum_ms / count;
      std::cout << std::fixed << std::setprecision(3)
                << "camera_frames=" << opt.frames
                << " avg_read_ms=" << read_sum_ms / count
                << " avg_infer_ms=" << infer_sum_ms / count
                << " avg_total_ms=" << avg_total
                << " fps=" << (avg_total > 0.0 ? 1000.0 / avg_total : 0.0) << "\n";
    }
  } else {
    run_ok = segmenter.run(opt.image, &result, &error);
  }
  if (!run_ok) {
    std::cerr << "run failed: " << error << "\n";
    return 3;
  }

  std::cout << "instances: " << result.instanceCount() << "\n";
  for (std::size_t i = 0; i < result.instances.size(); ++i) {
    const auto &instance = result.instances[i];
    std::cout << "  [" << i << "] class_id=" << instance.box.class_id
              << " score=" << instance.box.score
              << " box=(" << instance.box.x1 << "," << instance.box.y1 << ","
              << instance.box.x2 << "," << instance.box.y2 << ")"
              << " outline_points=" << instance.outline.size() << "\n";
  }

  if (!opt.output.empty()) {
    if (!saveAnnotatedImage(opt.image, opt.output, result, &error)) {
      std::cerr << "save failed: " << error << "\n";
      return 4;
    }
    std::cout << "saved: " << opt.output << "\n";
  }
  if (!opt.dump_frame.empty()) std::cout << "saved_frame: " << opt.dump_frame << "\n";
  if (!opt.dump_overlay.empty()) std::cout << "saved_overlay: " << opt.dump_overlay << "\n";
  return 0;
}
