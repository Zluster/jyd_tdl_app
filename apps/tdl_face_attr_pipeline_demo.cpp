#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "camera_demo_support.hpp"
#include "demo_support.hpp"
#include "tdl_app/algorithm_engine.hpp"
#include "tdl_app/face_attribute_classifier.hpp"
#include "tdl_app/face_detector.hpp"
#include "tdl_app/multi_stage_pipeline.hpp"

namespace {

struct Options {
  std::string image;
  std::string detector_model_spec;
  std::string attribute_model_spec;
  std::string firmware;
  std::string output;
  std::string dump_frame;
  std::string dump_overlay;
  float threshold = 0.25f;
  bool camera = false;
  int group = 0;
  int channel = 1;
  int timeout_ms = 1000;
  int frames = 1;
};

const char *valueForArg(int argc, char **argv, int *index, const char *name) {
  if (!index || *index + 1 >= argc) {
    std::cerr << "missing value for " << name << "\n";
    return nullptr;
  }
  return argv[++(*index)];
}

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_face_attr_pipeline_demo (--image FILE | --camera)\n"
      << "      --detector-model-spec FILE --attribute-model-spec FILE\n"
      << "      [--firmware FILE] [--threshold 0.25] [--output FILE]\n"
      << "      [--dump-frame FILE] [--dump-overlay FILE]\n"
      << "      [--group N] [--channel N] [--timeout-ms N] [--frames N]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  if (!opt) {
    return false;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--image") {
      const char *value = valueForArg(argc, argv, &i, "--image");
      if (!value) return false;
      opt->image = value;
      continue;
    }
    if (arg == "--camera") {
      opt->camera = true;
      continue;
    }
    if (arg == "--detector-model-spec") {
      const char *value =
          valueForArg(argc, argv, &i, "--detector-model-spec");
      if (!value) return false;
      opt->detector_model_spec = value;
      continue;
    }
    if (arg == "--attribute-model-spec") {
      const char *value =
          valueForArg(argc, argv, &i, "--attribute-model-spec");
      if (!value) return false;
      opt->attribute_model_spec = value;
      continue;
    }
    if (arg == "--firmware") {
      const char *value = valueForArg(argc, argv, &i, "--firmware");
      if (!value) return false;
      opt->firmware = value;
      continue;
    }
    if (arg == "--threshold") {
      const char *value = valueForArg(argc, argv, &i, "--threshold");
      if (!value) return false;
      opt->threshold = static_cast<float>(std::atof(value));
      continue;
    }
    if (arg == "--output") {
      const char *value = valueForArg(argc, argv, &i, "--output");
      if (!value) return false;
      opt->output = value;
      continue;
    }
    if (arg == "--dump-frame") {
      const char *value = valueForArg(argc, argv, &i, "--dump-frame");
      if (!value) return false;
      opt->dump_frame = value;
      continue;
    }
    if (arg == "--dump-overlay") {
      const char *value = valueForArg(argc, argv, &i, "--dump-overlay");
      if (!value) return false;
      opt->dump_overlay = value;
      continue;
    }
    if (arg == "--group") {
      const char *value = valueForArg(argc, argv, &i, "--group");
      if (!value) return false;
      opt->group = std::atoi(value);
      continue;
    }
    if (arg == "--channel") {
      const char *value = valueForArg(argc, argv, &i, "--channel");
      if (!value) return false;
      opt->channel = std::atoi(value);
      continue;
    }
    if (arg == "--timeout-ms") {
      const char *value = valueForArg(argc, argv, &i, "--timeout-ms");
      if (!value) return false;
      opt->timeout_ms = std::atoi(value);
      continue;
    }
    if (arg == "--frames") {
      const char *value = valueForArg(argc, argv, &i, "--frames");
      if (!value) return false;
      opt->frames = std::atoi(value);
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    }

    std::cerr << "unknown argument: " << arg << "\n";
    return false;
  }

  if (!opt->camera && opt->image.empty()) {
    std::cerr << "--image or --camera is required\n";
    return false;
  }
  if (opt->detector_model_spec.empty()) {
    std::cerr << "detector model spec is required\n";
    return false;
  }
  if (opt->attribute_model_spec.empty()) {
    std::cerr << "attribute model spec is required\n";
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

void dumpStageResult(const tdl_app::MultiStagePipeline::StageResult &stage) {
  std::cout << "stage[" << stage.stage_index << "] " << stage.name;
  if (stage.source_stage >= 0) {
    std::cout << " source_stage=" << stage.source_stage
              << " source_result=" << stage.source_result_index
              << " source_box=" << stage.source_box_index;
  }
  std::cout << "\n";
  demo_support::dumpResult(stage.output);
}

std::string describeAttributes(const tdl_app::AlgorithmResult &result) {
  std::string text;
  for (const auto &attribute : result.attributes) {
    if (!text.empty()) {
      text += " ";
    }
    if (attribute.name == "emotion") {
      static const char *kEmotionNames[] = {
          "neutral", "happy", "sad", "surprise", "fear", "disgust", "anger"};
      const int index = static_cast<int>(attribute.value);
      text += "emotion=";
      text += index >= 0 && index < 7 ? kEmotionNames[index] : "unknown";
    } else {
      text += attribute.name + "=" +
              std::to_string(static_cast<int>(attribute.value));
    }
  }
  return text.empty() ? "attribute=none" : text;
}

bool writeCameraOverlay(const std::string &source_path,
                        const std::string &overlay_path,
                        const tdl_app::AlgorithmResult &faces,
                        const std::vector<tdl_app::AlgorithmResult> &attributes,
                        std::string *error) {
  cv::Mat image = cv::imread(source_path, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to read snapshot for overlay: " + source_path;
    return false;
  }
  for (size_t index = 0; index < faces.boxes.size(); ++index) {
    const auto &box = faces.boxes[index];
    const int x1 = std::max(0, std::min(static_cast<int>(std::floor(box.x1)),
                                        image.cols - 1));
    const int y1 = std::max(0, std::min(static_cast<int>(std::floor(box.y1)),
                                        image.rows - 1));
    const int x2 = std::max(0, std::min(static_cast<int>(std::ceil(box.x2)),
                                        image.cols - 1));
    const int y2 = std::max(0, std::min(static_cast<int>(std::ceil(box.y2)),
                                        image.rows - 1));
    cv::rectangle(image, cv::Point(x1, y1), cv::Point(x2, y2),
                  cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    const std::string text =
        index < attributes.size() ? describeAttributes(attributes[index]) : "attribute=failed";
    cv::putText(image, text, cv::Point(x1, std::max(16, y1 - 5)),
                cv::FONT_HERSHEY_SIMPLEX, 0.35, cv::Scalar(0, 255, 0), 1,
                cv::LINE_AA);
    for (const auto &point : box.landmarks) {
      cv::circle(image,
                 cv::Point(static_cast<int>(std::lround(point.x)),
                           static_cast<int>(std::lround(point.y))),
                 2, cv::Scalar(0, 0, 255), cv::FILLED, cv::LINE_AA);
    }
  }
  if (!cv::imwrite(overlay_path, image)) {
    if (error) *error = "failed to write face attribute overlay: " + overlay_path;
    return false;
  }
  return true;
}

int runCamera(const Options &opt, tdl_app::FaceDetector *detector,
              tdl_app::FaceAttributeClassifier *classifier,
              std::string *error) {
  const tdl_app::Camera::Config camera_config =
      opt.group == 0 && opt.channel == 1
          ? tdl_app::Camera::ai(opt.timeout_ms)
          : tdl_app::Camera::vpss(opt.group, opt.channel, 640, 640,
                                  tdl_app::PixelFormat::RGB888_PLANAR,
                                  opt.timeout_ms);
  tdl_app::Camera camera(camera_config);
  if (!camera.open(error)) {
    std::cerr << "camera open failed: " << *error << "\n";
    return 4;
  }

  tdl_app::InferOptions infer_options;
  infer_options.threshold = opt.threshold;
  double read_sum_ms = 0.0;
  double detect_sum_ms = 0.0;
  double attribute_sum_ms = 0.0;
  double total_sum_ms = 0.0;
  double face_sum = 0.0;
  tdl_app::AlgorithmResult last_faces;
  std::vector<tdl_app::AlgorithmResult> last_attributes;
  tdl_app::AlgorithmResult last_detected_faces;
  std::vector<tdl_app::AlgorithmResult> last_detected_attributes;
  std::string saved_frame;
  std::string saved_overlay;

  for (int index = 0; index < opt.frames; ++index) {
    tdl_app::Frame frame;
    const auto read_begin = std::chrono::steady_clock::now();
    if (!camera.read(&frame, error)) {
      std::cerr << "camera read failed: " << *error << "\n";
      camera.close();
      return 4;
    }
    const auto read_end = std::chrono::steady_clock::now();
    const auto detect_begin = std::chrono::steady_clock::now();
    tdl_app::AlgorithmResult faces;
    if (!detector->detectFrame(frame, infer_options, &faces, error)) {
      std::cerr << "face detect failed: " << *error << "\n";
      camera.releaseFrame();
      camera.close();
      return 5;
    }
    const auto detect_end = std::chrono::steady_clock::now();
    const auto attribute_begin = std::chrono::steady_clock::now();
    std::vector<tdl_app::AlgorithmResult> attributes;
    attributes.reserve(faces.boxes.size());
    for (const auto &face : faces.boxes) {
      tdl_app::AlgorithmResult attribute;
      if (!classifier->classifyFrameCrop(frame, face, infer_options, &attribute,
                                         error)) {
        std::cerr << "face attribute failed: " << *error << "\n";
        camera.releaseFrame();
        camera.close();
        return 6;
      }
      attributes.push_back(std::move(attribute));
    }
    const auto attribute_end = std::chrono::steady_clock::now();

    // A benchmark's final frame can legitimately contain no face. Capture the
    // first successful detection so the requested functional overlay is useful.
    if (!faces.boxes.empty() && saved_frame.empty() && !opt.dump_frame.empty()) {
      if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, error)) {
        std::cerr << "failed to save frame: " << *error << "\n";
        camera.releaseFrame();
        camera.close();
        return 7;
      }
      saved_frame = opt.dump_frame;
      if (!opt.dump_overlay.empty() &&
          !writeCameraOverlay(saved_frame, opt.dump_overlay, faces, attributes,
                              error)) {
        std::cerr << "failed to save overlay: " << *error << "\n";
        camera.releaseFrame();
        camera.close();
        return 7;
      }
      saved_overlay = opt.dump_overlay;
    }
    camera.releaseFrame();

    const double read_ms =
        std::chrono::duration<double, std::milli>(read_end - read_begin).count();
    const double detect_ms = std::chrono::duration<double, std::milli>(
                                 detect_end - detect_begin)
                                 .count();
    const double attribute_ms = std::chrono::duration<double, std::milli>(
                                    attribute_end - attribute_begin)
                                    .count();
    read_sum_ms += read_ms;
    detect_sum_ms += detect_ms;
    attribute_sum_ms += attribute_ms;
    total_sum_ms += read_ms + detect_ms + attribute_ms;
    face_sum += faces.boxes.size();
    if (!faces.boxes.empty()) {
      last_detected_faces = faces;
      last_detected_attributes = attributes;
    }
    last_faces = std::move(faces);
    last_attributes = std::move(attributes);
  }
  camera.close();

  const double frame_count = static_cast<double>(opt.frames);
  const double avg_total_ms = total_sum_ms / frame_count;
  std::cout << std::fixed << std::setprecision(3)
            << "camera_frames: " << opt.frames << "\n"
            << "avg_read_ms: " << read_sum_ms / frame_count << "\n"
            << "avg_detect_ms: " << detect_sum_ms / frame_count << "\n"
            << "avg_attribute_ms: " << attribute_sum_ms / frame_count << "\n"
            << "avg_total_ms: " << avg_total_ms << "\n"
            << "avg_faces: " << face_sum / frame_count << "\n"
            << "fps: " << (avg_total_ms > 0.0 ? 1000.0 / avg_total_ms : 0.0)
            << "\n";
  if (!saved_frame.empty()) std::cout << "saved_frame: " << saved_frame << "\n";
  if (!saved_overlay.empty()) std::cout << "saved_overlay: " << saved_overlay << "\n";
  const tdl_app::AlgorithmResult &reported_faces =
      last_detected_faces.boxes.empty() ? last_faces : last_detected_faces;
  const std::vector<tdl_app::AlgorithmResult> &reported_attributes =
      last_detected_faces.boxes.empty() ? last_attributes : last_detected_attributes;
  for (size_t index = 0; index < reported_faces.boxes.size(); ++index) {
    std::cout << "face[" << index << "] score="
              << reported_faces.boxes[index].score;
    if (index < reported_attributes.size()) {
      std::cout << " " << describeAttributes(reported_attributes[index]);
    }
    std::cout << "\n";
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::string error;

  tdl_app::FaceDetector detector;
  tdl_app::FaceDetector::Config detector_config;
  detector_config.model_spec = opt.detector_model_spec;
  detector_config.firmware = opt.firmware;
  if (!detector.load(detector_config, &error)) {
    std::cerr << "face detector initialize failed: " << error << "\n";
    return 2;
  }

  tdl_app::FaceAttributeClassifier classifier;
  tdl_app::FaceAttributeClassifier::Config attribute_config;
  attribute_config.model_spec = opt.attribute_model_spec;
  attribute_config.firmware = opt.firmware;
  if (!classifier.load(attribute_config, &error)) {
    std::cerr << "face attribute initialize failed: " << error << "\n";
    return 3;
  }

  if (opt.camera) {
    return runCamera(opt, &detector, &classifier, &error);
  }

  tdl_app::MultiStagePipeline pipeline;
  pipeline.setImage(opt.image);
  const int detect_stage = pipeline.addFaceDetectorStage("face_detect", detector);
  pipeline.addFaceAttributeStage("face_attribute", classifier, detect_stage);

  tdl_app::InferOptions infer_options;
  infer_options.threshold = opt.threshold;

  tdl_app::MultiStagePipeline::Result result;
  double total_ms = 0.0;
  for (int index = 0; index < opt.frames; ++index) {
    const auto begin = std::chrono::steady_clock::now();
    const bool ok = pipeline.runOnce(infer_options, &result, &error);
    const auto end = std::chrono::steady_clock::now();
    if (!ok) {
      std::cerr << "pipeline run failed: " << error << "\n";
      pipeline.close();
      return 4;
    }
    total_ms += std::chrono::duration<double, std::milli>(end - begin).count();
  }
  pipeline.close();

  if (!opt.output.empty() &&
      !demo_support::saveAnnotatedImage(opt.image, opt.output, result.primary,
                                        &error)) {
    std::cerr << "save failed: " << error << "\n";
    return 5;
  }

  if (!opt.output.empty()) {
    std::cout << "saved: " << opt.output << "\n";
  }

  std::cout << "primary result\n";
  demo_support::dumpResult(result.primary);
  for (const auto &stage : result.stages) {
    dumpStageResult(stage);
  }
  return 0;
}
