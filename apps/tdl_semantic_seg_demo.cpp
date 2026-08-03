#include <array>
#include <chrono>
#include <cstdint>
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
      << "  tdl_semantic_seg_demo (--image FILE | --camera) --model-spec FILE\n"
      << "                        [--firmware FILE] [--model-dir DIR]\n"
      << "                        [--output MASK.png] [--frames N] [--warmup N]\n"
      << "                        [--device 0|1] [--group N] [--channel N] [--timeout-ms N]\n"
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

bool writeMask(const tdl_app::SemanticSegmentationResult &result,
               const std::string &path, std::string *error) {
  if (result.output_width <= 0 || result.output_height <= 0 ||
      result.class_id.size() !=
          static_cast<size_t>(result.output_width * result.output_height)) {
    if (error) *error = "semantic segmentation output shape is invalid";
    return false;
  }
  static const std::array<cv::Vec3b, 8> kPalette = {{
      cv::Vec3b(0, 0, 0), cv::Vec3b(0, 255, 0), cv::Vec3b(0, 0, 255),
      cv::Vec3b(255, 0, 0), cv::Vec3b(0, 255, 255), cv::Vec3b(255, 0, 255),
      cv::Vec3b(255, 255, 0), cv::Vec3b(255, 255, 255),
  }};
  cv::Mat mask(result.output_height, result.output_width, CV_8UC3);
  for (int y = 0; y < result.output_height; ++y) {
    for (int x = 0; x < result.output_width; ++x) {
      const std::uint8_t class_id =
          result.class_id[static_cast<size_t>(y * result.output_width + x)];
      mask.at<cv::Vec3b>(y, x) = kPalette[class_id % kPalette.size()];
    }
  }
  if (result.width > 0 && result.height > 0) {
    cv::resize(mask, mask, cv::Size(result.width, result.height), 0, 0,
               cv::INTER_NEAREST);
  }
  if (!cv::imwrite(path, mask)) {
    if (error) *error = "failed to write semantic mask: " + path;
    return false;
  }
  return true;
}

bool writeOverlay(const std::string &image_path, const std::string &output,
                  const tdl_app::SemanticSegmentationResult &result,
                  std::string *error) {
  cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
  if (image.empty() || result.output_width <= 0 || result.output_height <= 0 ||
      result.class_id.size() != static_cast<size_t>(result.output_width * result.output_height)) {
    if (error) *error = "invalid semantic overlay input";
    return false;
  }
  static const std::array<cv::Vec3b, 8> kPalette = {{
      cv::Vec3b(0, 0, 0), cv::Vec3b(0, 255, 0), cv::Vec3b(0, 0, 255),
      cv::Vec3b(255, 0, 0), cv::Vec3b(0, 255, 255), cv::Vec3b(255, 0, 255),
      cv::Vec3b(255, 255, 0), cv::Vec3b(255, 255, 255),
  }};
  cv::Mat mask(result.output_height, result.output_width, CV_8UC3);
  for (int y = 0; y < mask.rows; ++y) {
    for (int x = 0; x < mask.cols; ++x) {
      mask.at<cv::Vec3b>(y, x) = kPalette[result.class_id[static_cast<size_t>(y * mask.cols + x)] % kPalette.size()];
    }
  }
  cv::resize(mask, mask, image.size(), 0, 0, cv::INTER_NEAREST);
  cv::addWeighted(image, 0.55, mask, 0.45, 0.0, image);
  if (!cv::imwrite(output, image)) {
    if (error) *error = "failed to write semantic overlay: " + output;
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

  tdl_app::SemanticSegmenter segmenter;
  std::string error;
  if (!segmenter.load(tdl_app::ModelSessionConfig::fromSpec(
                          opt.model_spec, opt.firmware, opt.model_dir),
                      &error)) {
    std::cerr << "load failed: " << error << "\n";
    return 2;
  }

  tdl_app::SemanticSegmentationResult result;
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
             !writeOverlay(opt.dump_frame, opt.dump_overlay, result, &error))) {
          std::cerr << "failed to save semantic result: " << error << "\n";
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
    return 4;
  }

  std::cout << "output_width: " << result.output_width << "\n";
  std::cout << "output_height: " << result.output_height << "\n";
  std::cout << "pixels: " << result.pixelCount() << "\n";
  std::array<size_t, 256> histogram{};
  for (std::uint8_t class_id : result.class_id) {
    histogram[class_id]++;
  }
  for (size_t class_id = 0; class_id < histogram.size(); ++class_id) {
    if (histogram[class_id] != 0) {
      std::cout << "class[" << class_id << "] pixels="
                << histogram[class_id] << "\n";
    }
  }
  if (!opt.output.empty() && !writeMask(result, opt.output, &error)) {
    std::cerr << "write output failed: " << error << "\n";
    return 4;
  }
  if (!opt.output.empty()) {
    std::cout << "mask: " << opt.output << "\n";
  }
  if (!opt.dump_frame.empty()) std::cout << "saved_frame: " << opt.dump_frame << "\n";
  if (!opt.dump_overlay.empty()) std::cout << "saved_overlay: " << opt.dump_overlay << "\n";
  return 0;
}
