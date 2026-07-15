#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "ocr_overlay_support.hpp"
#include "tdl_app/plate_recognizer.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  camera_demo_support::CommonOptions camera;
  std::string model_spec = "./configs/model_specs/pp_ocr.mud";
  std::string firmware;
  std::string model_dir;
  std::string dump_frame;
  std::string dump_overlay;
  std::string font = "./fonts/DroidSansFallbackFull.ttf";
  int warmup = 30;
};

void usage() {
  std::cout
      << "Usage:\n"
      << "  tdl_pp_ocr_camera_demo [--model-spec FILE] [--firmware FILE]\n"
      << "      [--model-dir DIR] [--group 0] [--channel 1]\n"
      << "      [--font FILE]\n"
      << "      [--warmup 30] [--frames 300]\n"
      << "      [--dump-frame FILE] [--dump-overlay FILE]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    bool handled = false;
    std::string parse_error;
    if (!camera_demo_support::parseCommonArgs(argc, argv, &i, &opt->camera,
                                              &handled, &parse_error)) {
      std::cerr << parse_error << "\n";
      return false;
    }
    if (handled) continue;
    auto value = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--camera") {
      continue;
    } else if (arg == "--model-spec") {
      const char *v = value("--model-spec"); if (!v) return false; opt->model_spec = v;
    } else if (arg == "--firmware") {
      const char *v = value("--firmware"); if (!v) return false; opt->firmware = v;
    } else if (arg == "--model-dir") {
      const char *v = value("--model-dir"); if (!v) return false; opt->model_dir = v;
    } else if (arg == "--font") {
      const char *v = value("--font"); if (!v) return false; opt->font = v;
    } else if (arg == "--warmup") {
      const char *v = value("--warmup"); if (!v) return false; opt->warmup = std::atoi(v);
    } else if (arg == "--dump-frame") {
      const char *v = value("--dump-frame"); if (!v) return false; opt->dump_frame = v;
    } else if (arg == "--dump-overlay") {
      const char *v = value("--dump-overlay"); if (!v) return false; opt->dump_overlay = v;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  return !opt->model_spec.empty() && opt->warmup >= 0 &&
         opt->camera.frames > 0 &&
         (opt->dump_overlay.empty() || !opt->dump_frame.empty());
}

std::string lineText(const tdl_app::AlgorithmResult &result, size_t index) {
  if (index >= result.attributes.size()) return std::string();
  const std::string prefix = "ocr_text:";
  const std::string &name = result.attributes[index].name;
  return name.compare(0, prefix.size(), prefix) == 0
             ? name.substr(prefix.size())
             : name;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    usage();
    return 1;
  }

  tdl_app::PlateRecognizer::Config config;
  config.model_spec = opt.model_spec;
  config.firmware = opt.firmware;
  config.model_dir = opt.model_dir;
  tdl_app::PlateRecognizer recognizer;
  std::string error;
  if (!recognizer.load(config, &error)) {
    std::cerr << "OCR load failed: " << error << "\n";
    return 2;
  }

  camera_demo_support::CameraRuntime camera;
  if (!camera_demo_support::openCameraRuntime(opt.camera, &camera, &error)) {
    std::cerr << "camera open failed: " << error << "\n";
    return 3;
  }

  double read_sum = 0.0;
  tdl_app::OcrProfile sum;
  tdl_app::OcrProfile last_profile;
  tdl_app::AlgorithmResult last_result;
  const int total_frames = opt.warmup + opt.camera.frames;
  for (int index = 0; index < total_frames; ++index) {
    tdl_app::Frame frame;
    const auto read_begin = Clock::now();
    if (!camera.camera.read(&frame, &error)) {
      camera_demo_support::closeCameraRuntime(&camera);
      std::cerr << "camera read failed: " << error << "\n";
      return 4;
    }
    const double read_ms = std::chrono::duration<double, std::milli>(
        Clock::now() - read_begin).count();

    tdl_app::AlgorithmResult result;
    tdl_app::OcrProfile profile;
    tdl_app::InferOptions infer_options;
    if (!recognizer.runFrameProfiled(frame, infer_options, &result, &profile,
                                     &error)) {
      camera.camera.releaseFrame();
      camera_demo_support::closeCameraRuntime(&camera);
      std::cerr << "OCR failed: " << error << "\n";
      return 5;
    }

    if (index == total_frames - 1 && !opt.dump_frame.empty()) {
      if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, &error) ||
          (!opt.dump_overlay.empty() &&
           !ocr_overlay_support::saveAnnotatedImage(
               opt.dump_frame, opt.dump_overlay, result, opt.font, &error))) {
        camera.camera.releaseFrame();
        camera_demo_support::closeCameraRuntime(&camera);
        std::cerr << "save failed: " << error << "\n";
        return 6;
      }
    }
    camera.camera.releaseFrame();
    if (index < opt.warmup) continue;

    read_sum += read_ms;
    sum.frame_convert_ms += profile.frame_convert_ms;
    sum.det_preprocess_ms += profile.det_preprocess_ms;
    sum.det_inference_ms += profile.det_inference_ms;
    sum.det_postprocess_ms += profile.det_postprocess_ms;
    sum.rectify_ms += profile.rectify_ms;
    sum.rec_preprocess_ms += profile.rec_preprocess_ms;
    sum.rec_inference_ms += profile.rec_inference_ms;
    sum.rec_decode_ms += profile.rec_decode_ms;
    sum.total_ms += profile.total_ms;
    sum.text_regions += profile.text_regions;
    sum.hardware_det_preprocess = profile.hardware_det_preprocess;
    last_profile = profile;
    last_result = std::move(result);
  }
  camera_demo_support::closeCameraRuntime(&camera);

  const double count = static_cast<double>(opt.camera.frames);
  const double avg_pipeline = sum.total_ms / count;
  const double avg_total = read_sum / count + avg_pipeline;
  std::cout << std::fixed << std::setprecision(3)
            << "frames=" << opt.camera.frames
            << " hardware_det_preprocess="
            << (sum.hardware_det_preprocess ? 1 : 0)
            << " avg_text_regions=" << sum.text_regions / count << "\n"
            << "avg_read_ms=" << read_sum / count
            << " avg_frame_convert_ms=" << sum.frame_convert_ms / count
            << " avg_det_preprocess_ms=" << sum.det_preprocess_ms / count
            << " avg_det_inference_ms=" << sum.det_inference_ms / count
            << " avg_det_postprocess_ms=" << sum.det_postprocess_ms / count
            << "\n"
            << "avg_rectify_cpu_ms=" << sum.rectify_ms / count
            << " avg_rec_preprocess_ms=" << sum.rec_preprocess_ms / count
            << " avg_rec_inference_ms=" << sum.rec_inference_ms / count
            << " avg_rec_decode_ms=" << sum.rec_decode_ms / count << "\n"
            << "avg_pipeline_ms=" << avg_pipeline
            << " avg_total_ms=" << avg_total
            << " fps=" << (avg_total > 0.0 ? 1000.0 / avg_total : 0.0)
            << "\n";
  for (size_t i = 0; i < last_result.boxes.size(); ++i) {
    std::cout << "text[" << i << "] score=" << last_result.boxes[i].score
              << " value=" << lineText(last_result, i) << "\n";
  }
  if (!opt.dump_frame.empty()) std::cout << "saved_frame=" << opt.dump_frame << "\n";
  if (!opt.dump_overlay.empty()) std::cout << "saved_overlay=" << opt.dump_overlay << "\n";
  return 0;
}
