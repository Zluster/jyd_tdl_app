#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "tdl_app/face_detector.hpp"
#include "tdl_app/feature_extractor.hpp"

namespace {

struct Options {
  std::string reference_image;
  std::string query_image;
  std::string detector_model_spec;
  std::string feature_model_spec;
  std::string firmware;
  std::string output;
  float face_threshold = 0.25f;
  float match_threshold = 0.50f;
  int repeat = 1;
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
      << "  tdl_face_recognition_demo --reference-image FILE --query-image FILE\n"
      << "      --detector-model-spec FILE --feature-model-spec FILE\n"
      << "      [--firmware FILE] [--face-threshold 0.25]\n"
      << "      [--match-threshold 0.50] [--repeat N] [--output FILE]\n";
}

bool parseArgs(int argc, char **argv, Options *opt) {
  if (!opt) return false;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--reference-image") {
      const char *value = valueForArg(argc, argv, &index, "--reference-image");
      if (!value) return false;
      opt->reference_image = value;
    } else if (arg == "--query-image") {
      const char *value = valueForArg(argc, argv, &index, "--query-image");
      if (!value) return false;
      opt->query_image = value;
    } else if (arg == "--detector-model-spec") {
      const char *value =
          valueForArg(argc, argv, &index, "--detector-model-spec");
      if (!value) return false;
      opt->detector_model_spec = value;
    } else if (arg == "--feature-model-spec") {
      const char *value =
          valueForArg(argc, argv, &index, "--feature-model-spec");
      if (!value) return false;
      opt->feature_model_spec = value;
    } else if (arg == "--firmware") {
      const char *value = valueForArg(argc, argv, &index, "--firmware");
      if (!value) return false;
      opt->firmware = value;
    } else if (arg == "--face-threshold") {
      const char *value = valueForArg(argc, argv, &index, "--face-threshold");
      if (!value) return false;
      opt->face_threshold = static_cast<float>(std::atof(value));
    } else if (arg == "--match-threshold") {
      const char *value = valueForArg(argc, argv, &index, "--match-threshold");
      if (!value) return false;
      opt->match_threshold = static_cast<float>(std::atof(value));
    } else if (arg == "--repeat") {
      const char *value = valueForArg(argc, argv, &index, "--repeat");
      if (!value) return false;
      opt->repeat = std::atoi(value);
    } else if (arg == "--output") {
      const char *value = valueForArg(argc, argv, &index, "--output");
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
  if (opt->reference_image.empty() || opt->query_image.empty() ||
      opt->detector_model_spec.empty() || opt->feature_model_spec.empty()) {
    std::cerr << "reference/query images and both model specs are required\n";
    return false;
  }
  if (opt->repeat <= 0) {
    std::cerr << "--repeat must be positive\n";
    return false;
  }
  return true;
}

bool chooseFace(const tdl_app::AlgorithmResult &faces, tdl_app::Box *face,
                std::string *error) {
  if (!face || faces.boxes.empty()) {
    if (error) *error = "no face detected";
    return false;
  }
  const auto best = std::max_element(
      faces.boxes.begin(), faces.boxes.end(),
      [](const tdl_app::Box &lhs, const tdl_app::Box &rhs) {
        return lhs.score < rhs.score;
      });
  if (best->landmarks.size() < 5) {
    if (error) *error = "SCRFD result does not contain 5 face landmarks";
    return false;
  }
  *face = *best;
  return true;
}

cv::Mat faceAlignmentTransform(const tdl_app::Box &face) {
  static const cv::Point2f kReference[] = {
      {38.2946f, 51.6963f}, {73.5318f, 51.5014f}, {56.0252f, 71.7366f},
      {41.5493f, 92.3655f}, {70.7299f, 92.2041f}};

  cv::Point2f source_center(0.0f, 0.0f);
  cv::Point2f reference_center(0.0f, 0.0f);
  for (int index = 0; index < 5; ++index) {
    source_center.x += face.landmarks[static_cast<size_t>(index)].x;
    source_center.y += face.landmarks[static_cast<size_t>(index)].y;
    reference_center += kReference[index];
  }
  source_center *= 0.2f;
  reference_center *= 0.2f;

  float denominator = 0.0f;
  float alpha_sum = 0.0f;
  float beta_sum = 0.0f;
  for (int index = 0; index < 5; ++index) {
    const cv::Point2f source(
        face.landmarks[static_cast<size_t>(index)].x - source_center.x,
        face.landmarks[static_cast<size_t>(index)].y - source_center.y);
    const cv::Point2f target = kReference[index] - reference_center;
    denominator += source.x * source.x + source.y * source.y;
    alpha_sum += source.x * target.x + source.y * target.y;
    beta_sum += source.x * target.y - source.y * target.x;
  }
  const float alpha = denominator > 1e-6f ? alpha_sum / denominator : 1.0f;
  const float beta = denominator > 1e-6f ? beta_sum / denominator : 0.0f;
  const float tx = reference_center.x - alpha * source_center.x +
                   beta * source_center.y;
  const float ty = reference_center.y - beta * source_center.x -
                   alpha * source_center.y;
  return (cv::Mat_<float>(2, 3) << alpha, -beta, tx, beta, alpha, ty);
}

std::string alignedPath(const std::string &tag) {
  static const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return "/tmp/jyd_face_recognition_" + std::to_string(nonce) + "_" + tag +
         ".jpg";
}

bool prepareAlignedFace(const std::string &image_path, const std::string &tag,
                        tdl_app::FaceDetector *detector,
                        const tdl_app::InferOptions &detect_options,
                        tdl_app::Box *face, std::string *aligned_path,
                        double *elapsed_ms, std::string *error) {
  if (!detector || !face || !aligned_path) {
    if (error) *error = "aligned face output pointer is null";
    return false;
  }
  const auto begin = std::chrono::steady_clock::now();
  tdl_app::AlgorithmResult faces;
  if (!detector->detect(image_path, detect_options, &faces, error) ||
      !chooseFace(faces, face, error)) {
    return false;
  }
  cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to read image: " + image_path;
    return false;
  }
  cv::Mat aligned;
  cv::warpAffine(image, aligned, faceAlignmentTransform(*face), cv::Size(112, 112),
                 cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
  *aligned_path = alignedPath(tag);
  if (!cv::imwrite(*aligned_path, aligned)) {
    if (error) *error = "failed to save aligned face: " + *aligned_path;
    return false;
  }
  if (elapsed_ms) {
    *elapsed_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - begin)
                      .count();
  }
  return true;
}

float cosineSimilarity(const std::vector<float> &lhs,
                       const std::vector<float> &rhs) {
  if (lhs.empty() || lhs.size() != rhs.size()) return 0.0f;
  float dot = 0.0f;
  float lhs_norm = 0.0f;
  float rhs_norm = 0.0f;
  for (size_t index = 0; index < lhs.size(); ++index) {
    dot += lhs[index] * rhs[index];
    lhs_norm += lhs[index] * lhs[index];
    rhs_norm += rhs[index] * rhs[index];
  }
  const float norm = std::sqrt(lhs_norm * rhs_norm);
  return norm > 1e-6f ? dot / norm : 0.0f;
}

bool saveOverlay(const std::string &image_path, const std::string &output,
                 const tdl_app::Box &face, float similarity, bool matched,
                 std::string *error) {
  cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to read query image: " + image_path;
    return false;
  }
  const cv::Scalar color = matched ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
  cv::rectangle(image, cv::Point(static_cast<int>(face.x1), static_cast<int>(face.y1)),
                cv::Point(static_cast<int>(face.x2), static_cast<int>(face.y2)),
                color, 2, cv::LINE_AA);
  for (const auto &point : face.landmarks) {
    cv::circle(image, cv::Point(static_cast<int>(std::lround(point.x)),
                                static_cast<int>(std::lround(point.y))),
               2, cv::Scalar(255, 0, 0), cv::FILLED, cv::LINE_AA);
  }
  cv::putText(image, cv::format("%s %.3f", matched ? "match" : "different",
                                similarity),
              cv::Point(static_cast<int>(face.x1),
                        std::max(16, static_cast<int>(face.y1) - 5)),
              cv::FONT_HERSHEY_SIMPLEX, 0.5, color, 1, cv::LINE_AA);
  if (!cv::imwrite(output, image)) {
    if (error) *error = "failed to write recognition overlay: " + output;
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

  std::string error;
  tdl_app::FaceDetector detector;
  if (!detector.load(tdl_app::ModelSessionConfig::fromSpec(
                         opt.detector_model_spec, opt.firmware),
                     &error)) {
    std::cerr << "face detector load failed: " << error << "\n";
    return 2;
  }
  tdl_app::FeatureExtractor extractor;
  if (!extractor.load(tdl_app::ModelSessionConfig::fromSpec(
                          opt.feature_model_spec, opt.firmware),
                      &error)) {
    std::cerr << "feature extractor load failed: " << error << "\n";
    return 3;
  }

  const tdl_app::InferOptions detect_options =
      tdl_app::InferOptions::detection(opt.face_threshold);
  const tdl_app::InferOptions feature_options;
  double reference_prepare_ms = 0.0;
  double query_prepare_ms = 0.0;
  double reference_enroll_ms = 0.0;
  double query_feature_sum_ms = 0.0;
  double match_sum_ms = 0.0;
  std::string reference_aligned_path;
  std::string query_aligned_path;
  tdl_app::Box reference_face;
  tdl_app::Box query_face;
  tdl_app::AlgorithmResult reference_feature;
  tdl_app::AlgorithmResult query_feature;

  if (!prepareAlignedFace(opt.reference_image, "reference", &detector,
                          detect_options, &reference_face,
                          &reference_aligned_path, &reference_prepare_ms,
                          &error) ||
      !prepareAlignedFace(opt.query_image, "query", &detector, detect_options,
                          &query_face, &query_aligned_path, &query_prepare_ms,
                          &error)) {
    std::remove(reference_aligned_path.c_str());
    std::remove(query_aligned_path.c_str());
    std::cerr << "face recognition failed: " << error << "\n";
    return 4;
  }

  const auto enroll_begin = std::chrono::steady_clock::now();
  if (!extractor.extract(reference_aligned_path, feature_options,
                         &reference_feature, &error) ||
      reference_feature.feature.empty()) {
    std::remove(reference_aligned_path.c_str());
    std::remove(query_aligned_path.c_str());
    std::cerr << "reference enrollment failed: " << error << "\n";
    return 4;
  }
  reference_enroll_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - enroll_begin)
                            .count();

  float similarity = 0.0f;
  for (int iteration = 0; iteration < opt.repeat; ++iteration) {
    const auto feature_begin = std::chrono::steady_clock::now();
    if (!extractor.extract(query_aligned_path, feature_options, &query_feature,
                           &error) ||
        query_feature.feature.empty()) {
      std::remove(reference_aligned_path.c_str());
      std::remove(query_aligned_path.c_str());
      std::cerr << "face recognition failed: " << error << "\n";
      return 4;
    }
    const auto feature_end = std::chrono::steady_clock::now();
    const auto match_begin = feature_end;
    similarity =
        cosineSimilarity(reference_feature.feature, query_feature.feature);
    const auto match_end = std::chrono::steady_clock::now();
    query_feature_sum_ms += std::chrono::duration<double, std::milli>(
                                feature_end - feature_begin)
                                .count();
    match_sum_ms += std::chrono::duration<double, std::milli>(match_end -
                                                               match_begin)
                        .count();
  }

  const bool matched = similarity >= opt.match_threshold;
  const double average_feature_ms = query_feature_sum_ms / opt.repeat;
  const double average_match_ms = match_sum_ms / opt.repeat;
  const double average_query_ms = average_feature_ms + average_match_ms;
  std::cout << std::fixed << std::setprecision(3)
            << "feature_dim: " << reference_feature.feature.size() << "\n"
            << "similarity: " << similarity << "\n"
            << "match_threshold: " << opt.match_threshold << "\n"
            << "matched: " << (matched ? 1 : 0) << "\n"
            << "reference_prepare_ms: " << reference_prepare_ms << "\n"
            << "reference_enroll_ms: " << reference_enroll_ms << "\n"
            << "query_prepare_ms: " << query_prepare_ms << "\n"
            << "avg_query_feature_ms: " << average_feature_ms << "\n"
            << "avg_match_ms: " << average_match_ms << "\n"
            << "avg_query_ms: " << average_query_ms << "\n"
            << "queries_per_second: "
            << (average_query_ms > 0.0 ? 1000.0 / average_query_ms : 0.0)
            << "\n";

  if (!opt.output.empty()) {
    if (!saveOverlay(opt.query_image, opt.output, query_face, similarity, matched,
                     &error)) {
      std::cerr << "save failed: " << error << "\n";
      std::remove(reference_aligned_path.c_str());
      std::remove(query_aligned_path.c_str());
      return 5;
    }
    std::cout << "saved: " << opt.output << "\n";
  }
  std::remove(reference_aligned_path.c_str());
  std::remove(query_aligned_path.c_str());
  return 0;
}
