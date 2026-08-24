#include "tdl_app/face_recognizer.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "cvi_buffer.h"
#include "cvi_comm_video.h"
#include "cvi_comm_vb.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include "tdl_app/face_detector.hpp"
#include "tdl_app/feature_extractor.hpp"
#include "tdl_app/video_buffer.hpp"

#include "algorithm/private/frame_convert.hpp"

namespace tdl_app {
namespace {

bool faceProfileEnabled() {
  const char *value = std::getenv("TDL_BENCH_PROFILE");
  return value && value[0] != '\0' && value[0] != '0';
}

double faceElapsedMs(std::chrono::steady_clock::time_point begin,
                     std::chrono::steady_clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

constexpr int kAlignedWidth = 112;
constexpr int kAlignedHeight = 112;
constexpr char kFaceDatabaseMagic[] = {'J', 'Y', 'D', 'F', 'A', 'C', 'E', '1'};
constexpr std::uint32_t kFaceDatabaseVersion = 1;
constexpr std::uint32_t kMaxFaceDatabaseEntries = 1024;
constexpr std::uint32_t kMaxFaceNameBytes = 256;
constexpr std::uint32_t kMaxFeatureDimension = 4096;

void setError(std::string *error, const std::string &message) {
  if (error) *error = message;
}

template <typename T>
bool writeValue(std::ostream *stream, const T &value) {
  stream->write(reinterpret_cast<const char *>(&value), sizeof(value));
  return stream->good();
}

template <typename T>
bool readValue(std::istream *stream, T *value) {
  stream->read(reinterpret_cast<char *>(value), sizeof(*value));
  return stream->good();
}

bool normalize(std::vector<float> *feature, std::string *error) {
  if (!feature || feature->empty()) {
    setError(error, "face feature is empty");
    return false;
  }

  float norm_square = 0.0f;
  for (float value : *feature) norm_square += value * value;
  if (norm_square <= 1e-12f) {
    setError(error, "face feature has zero norm");
    return false;
  }

  const float inverse_norm = 1.0f / std::sqrt(norm_square);
  for (float &value : *feature) value *= inverse_norm;
  return true;
}

float cosineSimilarity(const std::vector<float> &lhs,
                       const std::vector<float> &rhs) {
  if (lhs.empty() || lhs.size() != rhs.size()) return -1.0f;
  float dot = 0.0f;
  for (std::size_t i = 0; i < lhs.size(); ++i) dot += lhs[i] * rhs[i];
  return dot;
}

bool chooseLargestFace(const AlgorithmResult &detected, Box *face,
                       std::string *error) {
  if (!face || detected.boxes.empty()) {
    setError(error, "no face detected");
    return false;
  }
  const auto best = std::max_element(
      detected.boxes.begin(), detected.boxes.end(),
      [](const Box &lhs, const Box &rhs) { return lhs.score < rhs.score; });
  if (best->landmarks.size() < 5) {
    setError(error, "SCRFD result does not contain five landmarks");
    return false;
  }
  *face = *best;
  return true;
}

// Produces the source-to-reference transform for the standard 112x112 face
// template. cv::warpAffine computes the inverse mapping internally.
cv::Mat sourceToReferenceTransform(const Box &face) {
  static const cv::Point2f kReference[] = {
      {38.2946f, 51.6963f}, {73.5318f, 51.5014f}, {56.0252f, 71.7366f},
      {41.5493f, 92.3655f}, {70.7299f, 92.2041f}};

  cv::Point2f source_center;
  cv::Point2f target_center;
  for (int i = 0; i < 5; ++i) {
    source_center.x += face.landmarks[static_cast<std::size_t>(i)].x;
    source_center.y += face.landmarks[static_cast<std::size_t>(i)].y;
    target_center += kReference[i];
  }
  source_center *= 0.2f;
  target_center *= 0.2f;

  float denominator = 0.0f;
  float alpha_sum = 0.0f;
  float beta_sum = 0.0f;
  for (int i = 0; i < 5; ++i) {
    const cv::Point2f source(
        face.landmarks[static_cast<std::size_t>(i)].x - source_center.x,
        face.landmarks[static_cast<std::size_t>(i)].y - source_center.y);
    const cv::Point2f target = kReference[i] - target_center;
    denominator += source.dot(source);
    alpha_sum += source.dot(target);
    beta_sum += source.x * target.y - source.y * target.x;
  }

  const float alpha = denominator > 1e-6f ? alpha_sum / denominator : 1.0f;
  const float beta = denominator > 1e-6f ? beta_sum / denominator : 0.0f;
  const float tx = target_center.x - alpha * source_center.x +
                   beta * source_center.y;
  const float ty = target_center.y - beta * source_center.x -
                   alpha * source_center.y;
  return (cv::Mat_<float>(2, 3) << alpha, -beta, tx, beta, alpha, ty);
}

}  // namespace

struct FaceRecognizer::Impl {
  // BMRT on CV184X has a single a53lite kernel-module lifetime. Release the
  // feature runtime before the detector so SCRFD owns the final unload
  // operation.
  ~Impl() {
    extractor.reset();
    detector.reset();
  }

  Impl()
      : detector(FaceDetector::scrfd()),
        extractor(FeatureExtractor::generic()) {}

  bool prepareAlignedFrame(const Frame &source, const Box &face,
                           Frame *aligned, VideoBufferBlock *block,
                           std::string *error) {
    const auto align_begin = std::chrono::steady_clock::now();
    if (!aligned || !block) {
      setError(error, "aligned frame output is null");
      return false;
    }
    *block = VideoBufferBlock{};
    block->handle = VB_INVALID_HANDLE;
    if (face.landmarks.size() < 5) {
      setError(error, "face does not contain five landmarks");
      return false;
    }

    const auto convert_begin = std::chrono::steady_clock::now();
    cv::Mat source_bgr;
    if (!frame_convert::frameToBgrMat(source, &bgr_converter, &source_bgr,
                                      error)) {
      return false;
    }
    const auto convert_end = std::chrono::steady_clock::now();

    const auto warp_begin = std::chrono::steady_clock::now();
    cv::Mat aligned_bgr;
    cv::warpAffine(source_bgr, aligned_bgr, sourceToReferenceTransform(face),
                   cv::Size(kAlignedWidth, kAlignedHeight),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT);
    const auto warp_end = std::chrono::steady_clock::now();
    if (aligned_bgr.empty() || aligned_bgr.type() != CV_8UC3) {
      setError(error, "face alignment produced an invalid BGR image");
      return false;
    }

    const auto vb_begin = std::chrono::steady_clock::now();
    VB_CAL_CONFIG_S calc;
    std::memset(&calc, 0, sizeof(calc));
    COMMON_GetPicBufferConfig(kAlignedWidth, kAlignedHeight, PIXEL_FORMAT_BGR_888,
                              DATA_BITWIDTH_8, COMPRESS_MODE_NONE, 64, &calc);
    const VB_BLK handle = CVI_VB_GetBlock(VB_INVALID_POOLID, calc.u32VBSize);
    if (handle == VB_INVALID_HANDLE) {
      setError(error, "CVI_VB_GetBlock failed for aligned face, size=" +
                          std::to_string(calc.u32VBSize));
      return false;
    }
    block->handle = handle;
    block->physical = CVI_VB_Handle2PhysAddr(handle);
    block->pool_id = static_cast<int>(CVI_VB_Handle2PoolId(handle));
    block->size = calc.u32MainYSize;
    block->virtual_addr = CVI_SYS_MmapCache(block->physical, block->size);
    if (block->physical == 0 || !block->virtual_addr) {
      CVI_VB_ReleaseBlock(handle);
      *block = VideoBufferBlock{};
      setError(error, "CVI_SYS_MmapCache failed for aligned face");
      return false;
    }

    auto *destination = static_cast<unsigned char *>(block->virtual_addr);
    for (int y = 0; y < kAlignedHeight; ++y) {
      std::memcpy(destination + static_cast<std::size_t>(y) * calc.u32MainStride,
                  aligned_bgr.ptr(y), static_cast<std::size_t>(kAlignedWidth) * 3);
    }
    if (CVI_SYS_IonFlushCache(block->physical, destination, calc.u32MainYSize) !=
        CVI_SUCCESS) {
      CVI_SYS_Munmap(block->virtual_addr, block->size);
      CVI_VB_ReleaseBlock(block->handle);
      *block = VideoBufferBlock{};
      setError(error, "CVI_SYS_IonFlushCache failed for aligned face");
      return false;
    }
    const auto vb_end = std::chrono::steady_clock::now();

    if (faceProfileEnabled()) {
      std::fprintf(stderr,
                   "[profile] face align: convert=%.3f warp=%.3f vb=%.3f "
                   "total=%.3f ms\n",
                   faceElapsedMs(convert_begin, convert_end),
                   faceElapsedMs(warp_begin, warp_end),
                   faceElapsedMs(vb_begin, vb_end),
                   faceElapsedMs(align_begin, vb_end));
    }

    aligned_video = VIDEO_FRAME_INFO_S{};
    auto &vf = aligned_video.stVFrame;
    vf.enCompressMode = COMPRESS_MODE_NONE;
    vf.enPixelFormat = PIXEL_FORMAT_BGR_888;
    vf.enVideoFormat = VIDEO_FORMAT_LINEAR;
    vf.enColorGamut = COLOR_GAMUT_BT601;
    vf.enDynamicRange = DYNAMIC_RANGE_SDR8;
    vf.u32Width = kAlignedWidth;
    vf.u32Height = kAlignedHeight;
    vf.u32Stride[0] = calc.u32MainStride;
    vf.u32Length[0] = calc.u32MainYSize;
    vf.u64PhyAddr[0] = block->physical;
    vf.pu8VirAddr[0] = destination;
    aligned_video.u32PoolId = static_cast<CVI_U32>(block->pool_id);

    aligned->image_path.clear();
    aligned->native = &aligned_video;
    aligned->width = kAlignedWidth;
    aligned->height = kAlignedHeight;
    aligned->format = PixelFormat::BGR888;
    aligned->sequence = source.sequence;
    aligned->timestamp_us = source.timestamp_us;
    return true;
  }

  bool extractFeature(const Frame &source, const Box &face,
                      std::vector<float> *feature, std::string *error) {
    Frame aligned;
    VideoBufferBlock block;
    if (!prepareAlignedFrame(source, face, &aligned, &block, error)) return false;

    AlgorithmResult result;
    const bool ok = extractor.extractFrame(aligned, InferOptions{}, &result, error);
    aligned.native = nullptr;
    if (block.virtual_addr) {
      CVI_SYS_Munmap(block.virtual_addr, block.size);
    }
    if (block.handle != VB_INVALID_HANDLE) {
      CVI_VB_ReleaseBlock(block.handle);
    }
    if (!ok || result.feature.empty()) {
      if (error && error->empty()) *error = "feature extractor returned no face feature";
      return false;
    }

    *feature = std::move(result.feature);
    return normalize(feature, error);
  }

  FaceRecognizer::Config config;
  FaceDetector detector;
  FeatureExtractor extractor;
  frame_convert::VpssBgrConverter bgr_converter;
  VIDEO_FRAME_INFO_S aligned_video{};
  std::map<std::string, std::vector<float>> database;
  bool loaded = false;
};

FaceRecognizer::FaceRecognizer() : impl_(new Impl()) {}
FaceRecognizer::~FaceRecognizer() = default;

bool FaceRecognizer::load(const Config &config, std::string *error) {
  if (!impl_) {
    setError(error, "face recognizer is unavailable");
    return false;
  }
  if (config.detector_model_spec.empty() || config.feature_model_spec.empty()) {
    setError(error, "detector_model_spec and feature_model_spec are required");
    return false;
  }
  if (config.max_faces <= 0) {
    setError(error, "max_faces must be positive");
    return false;
  }

  if (!impl_->detector.load(ModelSessionConfig::fromSpec(
          config.detector_model_spec, config.firmware), error) ||
      !impl_->extractor.load(ModelSessionConfig::fromSpec(
          config.feature_model_spec, config.firmware), error)) {
    return false;
  }
  impl_->config = config;
  impl_->loaded = true;
  return true;
}

bool FaceRecognizer::initialized() const {
  return impl_ && impl_->loaded && impl_->detector.initialized() &&
         impl_->extractor.initialized();
}

bool FaceRecognizer::enrollFrame(const std::string &name, const Frame &frame,
                                 std::string *error) {
  if (!initialized()) {
    setError(error, "face recognizer is not initialized");
    return false;
  }
  if (name.empty()) {
    setError(error, "face name is empty");
    return false;
  }

  const auto total_begin = std::chrono::steady_clock::now();
  AlgorithmResult detected;
  const auto detect_begin = std::chrono::steady_clock::now();
  if (!impl_->detector.detectFrame(
          frame, InferOptions::detection(impl_->config.face_threshold),
          &detected, error)) {
    return false;
  }
  const auto detect_end = std::chrono::steady_clock::now();
  Box face;
  if (!chooseLargestFace(detected, &face, error)) return false;

  const auto feature_begin = std::chrono::steady_clock::now();
  std::vector<float> feature;
  if (!impl_->extractFeature(frame, face, &feature, error)) return false;
  const auto feature_end = std::chrono::steady_clock::now();
  impl_->database[name] = std::move(feature);
  if (faceProfileEnabled()) {
    const auto total_end = std::chrono::steady_clock::now();
    std::fprintf(stderr,
                 "[profile] face enroll outer: detect=%.3f feature=%.3f "
                 "other=%.3f total=%.3f ms\n",
                 faceElapsedMs(detect_begin, detect_end),
                 faceElapsedMs(feature_begin, feature_end),
                 faceElapsedMs(detect_end, feature_begin),
                 faceElapsedMs(total_begin, total_end));
  }
  return true;
}

bool FaceRecognizer::recognizeFrame(
    const Frame &frame, std::vector<FaceRecognitionResult> *results,
    std::string *error) {
  return recognizeFrame(frame, impl_ ? impl_->config.face_threshold : 0.25f,
                        0.45f,
                        impl_ ? impl_->config.match_threshold : 0.50f,
                        results, error);
}

bool FaceRecognizer::recognizeFrame(
    const Frame &frame, float face_threshold, float iou_threshold,
    float match_threshold, std::vector<FaceRecognitionResult> *results,
    std::string *error) {
  if (!results) {
    setError(error, "face recognition results pointer is null");
    return false;
  }
  results->clear();
  if (!initialized()) {
    setError(error, "face recognizer is not initialized");
    return false;
  }

  const auto total_begin = std::chrono::steady_clock::now();
  AlgorithmResult detected;
  const auto detect_begin = std::chrono::steady_clock::now();
  if (!impl_->detector.detectFrame(
          frame, InferOptions::detection(face_threshold, iou_threshold),
          &detected, error)) {
    return false;
  }
  const auto detect_end = std::chrono::steady_clock::now();

  double feature_total_ms = 0.0;
  double match_total_ms = 0.0;
  int feature_count = 0;

  for (const Box &face : detected.boxes) {
    if (static_cast<int>(results->size()) >= impl_->config.max_faces) break;
    if (face.landmarks.size() < 5) continue;

    const auto feature_begin = std::chrono::steady_clock::now();
    std::vector<float> feature;
    if (!impl_->extractFeature(frame, face, &feature, error)) return false;
    const auto feature_end = std::chrono::steady_clock::now();
    feature_total_ms += faceElapsedMs(feature_begin, feature_end);
    ++feature_count;

    FaceRecognitionResult recognition;
    recognition.box = face;
    const auto match_begin = std::chrono::steady_clock::now();
    int class_id = 1;
    for (const auto &entry : impl_->database) {
      const float score = cosineSimilarity(feature, entry.second);
      if (score > recognition.similarity) {
        recognition.name = entry.first;
        recognition.similarity = score;
        recognition.class_id = class_id;
      }
      ++class_id;
    }
    recognition.matched = !recognition.name.empty() &&
                          recognition.similarity >= match_threshold;
    if (!recognition.matched) {
      recognition.name = "unknown";
      recognition.class_id = 0;
    }
    recognition.feature = std::move(feature);
    results->push_back(std::move(recognition));
    match_total_ms += faceElapsedMs(match_begin,
                                    std::chrono::steady_clock::now());
  }
  if (faceProfileEnabled()) {
    const auto total_end = std::chrono::steady_clock::now();
    std::fprintf(stderr,
                 "[profile] face recognize outer: detect=%.3f feature=%.3f "
                 "match=%.3f faces=%d other=%.3f total=%.3f ms\n",
                 faceElapsedMs(detect_begin, detect_end), feature_total_ms,
                 match_total_ms, feature_count,
                 faceElapsedMs(detect_end, total_end) - feature_total_ms -
                     match_total_ms,
                 faceElapsedMs(total_begin, total_end));
  }
  return true;
}

bool FaceRecognizer::addFace(const FaceRecognitionResult &face,
                             const std::string &name, std::string *error) {
  if (!impl_) {
    setError(error, "face recognizer is unavailable");
    return false;
  }
  if (name.empty() || name.size() > kMaxFaceNameBytes) {
    setError(error, "face name must contain 1 to 256 bytes");
    return false;
  }
  if (face.feature.empty() || face.feature.size() > kMaxFeatureDimension) {
    setError(error, "recognition result has no usable face feature");
    return false;
  }
  std::vector<float> feature = face.feature;
  if (!normalize(&feature, error)) return false;
  impl_->database[name] = std::move(feature);
  return true;
}

bool FaceRecognizer::saveFaces(const std::string &path, std::string *error) const {
  if (!impl_) {
    setError(error, "face recognizer is unavailable");
    return false;
  }
  if (path.empty()) {
    setError(error, "face database path is empty");
    return false;
  }
  std::uint32_t feature_dimension = 0;
  for (const auto &entry : impl_->database) {
    if (entry.first.empty() || entry.first.size() > kMaxFaceNameBytes ||
        entry.second.empty() || entry.second.size() > kMaxFeatureDimension) {
      setError(error, "face database contains an invalid entry");
      return false;
    }
    if (feature_dimension == 0) {
      feature_dimension = static_cast<std::uint32_t>(entry.second.size());
    } else if (feature_dimension != entry.second.size()) {
      setError(error, "face database contains mixed feature dimensions");
      return false;
    }
  }
  if (impl_->database.size() > kMaxFaceDatabaseEntries) {
    setError(error, "face database has too many entries");
    return false;
  }

  const std::string temporary_path = path + ".tmp";
  std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    setError(error, "failed to open face database for writing: " + temporary_path);
    return false;
  }
  const std::uint32_t entry_count =
      static_cast<std::uint32_t>(impl_->database.size());
  stream.write(kFaceDatabaseMagic, sizeof(kFaceDatabaseMagic));
  if (!stream.good() || !writeValue(&stream, kFaceDatabaseVersion) ||
      !writeValue(&stream, feature_dimension) || !writeValue(&stream, entry_count)) {
    setError(error, "failed to write face database header");
    return false;
  }
  for (const auto &entry : impl_->database) {
    const std::uint32_t name_size =
        static_cast<std::uint32_t>(entry.first.size());
    if (!writeValue(&stream, name_size)) {
      setError(error, "failed to write face database name length");
      return false;
    }
    stream.write(entry.first.data(), name_size);
    stream.write(reinterpret_cast<const char *>(entry.second.data()),
                 static_cast<std::streamsize>(entry.second.size() * sizeof(float)));
    if (!stream.good()) {
      setError(error, "failed to write face database entry");
      return false;
    }
  }
  stream.close();
  if (!stream) {
    setError(error, "failed to finalize face database");
    return false;
  }
  if (std::rename(temporary_path.c_str(), path.c_str()) != 0) {
    setError(error, "failed to replace face database: " + path);
    return false;
  }
  return true;
}

bool FaceRecognizer::loadFaces(const std::string &path, std::string *error) {
  if (!impl_) {
    setError(error, "face recognizer is unavailable");
    return false;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    setError(error, "failed to open face database: " + path);
    return false;
  }
  char magic[sizeof(kFaceDatabaseMagic)]{};
  stream.read(magic, sizeof(magic));
  std::uint32_t version = 0;
  std::uint32_t feature_dimension = 0;
  std::uint32_t entry_count = 0;
  if (!stream.good() || std::memcmp(magic, kFaceDatabaseMagic, sizeof(magic)) != 0 ||
      !readValue(&stream, &version) ||
      !readValue(&stream, &feature_dimension) ||
      !readValue(&stream, &entry_count) || version != kFaceDatabaseVersion ||
      entry_count > kMaxFaceDatabaseEntries ||
      feature_dimension > kMaxFeatureDimension ||
      (entry_count != 0 && feature_dimension == 0)) {
    setError(error, "face database header is invalid");
    return false;
  }

  std::map<std::string, std::vector<float>> database;
  for (std::uint32_t i = 0; i < entry_count; ++i) {
    std::uint32_t name_size = 0;
    if (!readValue(&stream, &name_size) || name_size == 0 ||
        name_size > kMaxFaceNameBytes) {
      setError(error, "face database name is invalid");
      return false;
    }
    std::string name(name_size, '\0');
    std::vector<float> feature(feature_dimension);
    stream.read(&name[0], name_size);
    stream.read(reinterpret_cast<char *>(feature.data()),
                static_cast<std::streamsize>(feature.size() * sizeof(float)));
    if (!stream.good() || !normalize(&feature, error)) {
      if (error && error->empty()) *error = "face database entry is truncated";
      return false;
    }
    if (!database.emplace(std::move(name), std::move(feature)).second) {
      setError(error, "face database contains duplicate names");
      return false;
    }
  }
  impl_->database = std::move(database);
  return true;
}

bool FaceRecognizer::remove(const std::string &name) {
  return impl_ && impl_->database.erase(name) != 0;
}

void FaceRecognizer::clear() {
  if (impl_) impl_->database.clear();
}

std::vector<std::string> FaceRecognizer::names() const {
  std::vector<std::string> names;
  if (!impl_) return names;
  for (const auto &entry : impl_->database) names.push_back(entry.first);
  return names;
}

}  // namespace tdl_app
