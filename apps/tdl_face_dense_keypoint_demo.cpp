#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "bmlib_runtime.h"
#include "bmruntime_interface.h"
#include "algorithm/private/vpss_preprocessor.hpp"
#include "camera_demo_support.hpp"
#include "demo_support.hpp"
#include "tdl_app/tdl_app.hpp"
#include "tdl_app/face_detector.hpp"

namespace {

struct Options {
  std::string image;
  std::string detector_model_spec;
  std::string keypoint_model_spec;
  std::string firmware;
  std::string output;
  std::string dump_frame;
  std::string dump_overlay;
  bool camera = false;
  int group = 0;
  int channel = 1;
  int timeout_ms = 1000;
  int frames = 1;
  int warmup = 0;
  float threshold = 0.25f;
  float roi_expand_ratio = 0.0f;
  int detector_face_class_id = 0;
};

struct TempFile {
  std::string path;

  ~TempFile() {
    if (!path.empty()) {
      std::remove(path.c_str());
    }
  }
};

struct SquareCrop {
  cv::Mat image;
  float x = 0.0f;
  float y = 0.0f;
  float size = 0.0f;
};

struct AffineCrop {
  cv::Mat image;
  cv::Mat inverse_transform;
};

std::string toUpper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                 });
  return value;
}

std::string normalizeToken(std::string value) {
  value = toUpper(std::move(value));
  std::replace(value.begin(), value.end(), '-', '_');
  return value;
}

std::string baseName(const std::string &path) {
  const size_t pos = path.find_last_of("/\\");
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

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
      << "  tdl_face_dense_keypoint_demo (--image FILE | --camera)\n"
      << "      --detector-model-spec FILE --keypoint-model-spec FILE\n"
      << "      [--firmware FILE] [--threshold 0.25] [--output FILE]\n"
      << "      [--roi-expand-ratio 0.0] [--detector-face-class-id 0]\n"
      << "      [--group 0] [--channel 1] [--timeout-ms 1000]\n"
      << "      [--warmup 30] [--frames 300]\n"
      << "      [--dump-frame FILE] [--dump-overlay FILE]\n";
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
    if (arg == "--keypoint-model-spec") {
      const char *value =
          valueForArg(argc, argv, &i, "--keypoint-model-spec");
      if (!value) return false;
      opt->keypoint_model_spec = value;
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
    if (arg == "--warmup") {
      const char *value = valueForArg(argc, argv, &i, "--warmup");
      if (!value) return false;
      opt->warmup = std::atoi(value);
      continue;
    }
    if (arg == "--roi-expand-ratio") {
      const char *value = valueForArg(argc, argv, &i, "--roi-expand-ratio");
      if (!value) return false;
      opt->roi_expand_ratio = static_cast<float>(std::atof(value));
      continue;
    }
    if (arg == "--detector-face-class-id") {
      const char *value =
          valueForArg(argc, argv, &i, "--detector-face-class-id");
      if (!value) return false;
      opt->detector_face_class_id = std::atoi(value);
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
  if (opt->keypoint_model_spec.empty()) {
    std::cerr << "keypoint model spec is required\n";
    return false;
  }
  if (opt->camera && !opt->output.empty()) {
    std::cerr << "--output is offline-only; use --dump-overlay with --camera\n";
    return false;
  }
  if (!opt->dump_overlay.empty() && opt->dump_frame.empty()) {
    std::cerr << "--dump-overlay requires --dump-frame\n";
    return false;
  }
  if (opt->frames <= 0 || opt->warmup < 0) {
    std::cerr << "--frames must be positive and --warmup must be non-negative\n";
    return false;
  }
  return true;
}

tdl_app::Box makeSquareBox(const tdl_app::Box &box, float expand_ratio) {
  const float w = std::max(1.0f, box.x2 - box.x1);
  const float h = std::max(1.0f, box.y2 - box.y1);
  const float cx = (box.x1 + box.x2) * 0.5f;
  const float cy = (box.y1 + box.y2) * 0.5f;
  const float side = std::max(w, h) * std::max(1.0f, 1.0f + expand_ratio);

  tdl_app::Box square = box;
  square.x1 = cx - side * 0.5f;
  square.y1 = cy - side * 0.5f;
  square.x2 = cx + side * 0.5f;
  square.y2 = cy + side * 0.5f;
  return square;
}

bool extractSquareCrop(const cv::Mat &image, const tdl_app::Box &box,
                       SquareCrop *square_crop, std::string *error) {
  if (!square_crop) {
    if (error) {
      *error = "square crop output pointer is null";
    }
    return false;
  }

  const float side = std::max(1.0f, std::max(box.x2 - box.x1, box.y2 - box.y1));
  const float cx = (box.x1 + box.x2) * 0.5f;
  const float cy = (box.y1 + box.y2) * 0.5f;
  const int crop_size = std::max(1, static_cast<int>(std::ceil(side)));
  const int left = static_cast<int>(std::floor(cx - side * 0.5f));
  const int top = static_cast<int>(std::floor(cy - side * 0.5f));

  cv::Mat crop(crop_size, crop_size, image.type(), cv::Scalar::all(0));

  const int src_x1 = std::max(0, left);
  const int src_y1 = std::max(0, top);
  const int src_x2 = std::min(image.cols, left + crop_size);
  const int src_y2 = std::min(image.rows, top + crop_size);
  if (src_x2 <= src_x1 || src_y2 <= src_y1) {
    if (error) {
      *error = "square crop falls outside of image";
    }
    return false;
  }

  const int dst_x = src_x1 - left;
  const int dst_y = src_y1 - top;
  const cv::Rect src_rect(src_x1, src_y1, src_x2 - src_x1, src_y2 - src_y1);
  const cv::Rect dst_rect(dst_x, dst_y, src_rect.width, src_rect.height);
  image(src_rect).copyTo(crop(dst_rect));

  square_crop->image = crop;
  square_crop->x = static_cast<float>(left);
  square_crop->y = static_cast<float>(top);
  square_crop->size = static_cast<float>(crop_size);
  return true;
}

bool buildAlignmentSourcePoints(const tdl_app::Box &box,
                                std::vector<cv::Point2f> *src_points,
                                std::string *error) {
  if (!src_points) {
    if (error) {
      *error = "alignment source point output pointer is null";
    }
    return false;
  }
  src_points->clear();
  src_points->reserve(5);
  if (box.landmarks.size() < 5) {
    if (error) {
      *error =
          "detector does not provide 5-point face landmarks; use SCRFD-based face detection";
    }
    return false;
  }
  for (int i = 0; i < 5; ++i) {
    src_points->emplace_back(box.landmarks[static_cast<size_t>(i)].x,
                             box.landmarks[static_cast<size_t>(i)].y);
  }
  return true;
}

bool extractAlignedFaceCrop(const cv::Mat &image, const tdl_app::Box &box,
                            int output_size, float expand_ratio,
                            bool *used_box_heuristic, AffineCrop *affine_crop,
                            std::string *error) {
  if (!affine_crop) {
    if (error) {
      *error = "affine crop output pointer is null";
    }
    return false;
  }
  if (output_size <= 0) {
    if (error) {
      *error = "invalid affine crop output size";
    }
    return false;
  }

  std::vector<cv::Point2f> src_points;
  if (!buildAlignmentSourcePoints(box, &src_points, error) ||
      src_points.size() < 5) {
    return false;
  }
  if (used_box_heuristic) {
    *used_box_heuristic = false;
  }
  const float w = std::max(1.0f, box.x2 - box.x1);
  const float h = std::max(1.0f, box.y2 - box.y1);
  const cv::Point2f eye_center = (src_points[0] + src_points[1]) * 0.5f;
  const cv::Point2f mouth_center = (src_points[3] + src_points[4]) * 0.5f;
  const cv::Point2f face_axis = mouth_center - eye_center;
  const float cx = (box.x1 + box.x2) * 0.5f + face_axis.x * 0.10f;
  const float cy = (box.y1 + box.y2) * 0.5f + face_axis.y * 0.35f;
  const float crop_scale = std::max(1.0f, 1.0f + expand_ratio);
  const float side = std::max(w, h) * crop_scale;
  const float half_side = side * 0.5f;

  const cv::Point2f &left_eye = src_points[0];
  const cv::Point2f &right_eye = src_points[1];
  const float theta = std::atan2(right_eye.y - left_eye.y,
                                 right_eye.x - left_eye.x);
  const float cos_theta = std::cos(theta);
  const float sin_theta = std::sin(theta);

  auto rotateAroundCenter = [&](float dx, float dy) -> cv::Point2f {
    return cv::Point2f(cx + dx * cos_theta - dy * sin_theta,
                       cy + dx * sin_theta + dy * cos_theta);
  };

  std::vector<cv::Point2f> src_affine = {
      rotateAroundCenter(-half_side, -half_side),
      rotateAroundCenter(-half_side, half_side),
      rotateAroundCenter(half_side, half_side),
  };
  std::vector<cv::Point2f> dst_affine = {
      cv::Point2f(0.0f, 0.0f),
      cv::Point2f(0.0f, static_cast<float>(output_size)),
      cv::Point2f(static_cast<float>(output_size),
                  static_cast<float>(output_size)),
  };

  cv::Mat transform = cv::getAffineTransform(src_affine, dst_affine);
  if (transform.empty()) {
    if (error) {
      *error = "failed to estimate face alignment transform";
    }
    return false;
  }

  cv::warpAffine(image, affine_crop->image, transform,
                 cv::Size(output_size, output_size), cv::INTER_NEAREST,
                 cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
  cv::invertAffineTransform(transform, affine_crop->inverse_transform);
  return true;
}

bool writeTempCrop(const cv::Mat &crop, int face_index, TempFile *temp_file,
                   std::string *error) {
  if (!temp_file) {
    if (error) {
      *error = "temp file output pointer is null";
    }
    return false;
  }

  char buffer[L_tmpnam];
  if (!std::tmpnam(buffer)) {
    if (error) {
      *error = "failed to allocate temporary filename";
    }
    return false;
  }

  temp_file->path = std::string(buffer) + "_face_" + std::to_string(face_index) +
                    ".png";
  if (!cv::imwrite(temp_file->path, crop)) {
    if (error) {
      *error = "failed to write temporary crop: " + temp_file->path;
    }
    return false;
  }
  return true;
}

struct DenseProfile {
  double vpss_roi_ms = 0.0;
  double bmrt_ms = 0.0;
  double output_copy_decode_ms = 0.0;
  double total_ms = 0.0;
};

double elapsedMs(const std::chrono::steady_clock::time_point &begin,
                 const std::chrono::steady_clock::time_point &end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}

class DenseLandmarkRuntime {
 public:
  ~DenseLandmarkRuntime() { close(); }

  int inputWidth() const { return input_width_; }
  int inputHeight() const { return input_height_; }
  int coordinateExtent() const {
    return std::max(1, static_cast<int>(std::lround(coordinate_extent_)));
  }

  bool open(const std::string &model_spec, const std::string &firmware,
            std::string *error) {
    close();

    if (!tdl_app::loadModelDescriptor(model_spec, &descriptor_, error)) {
      return false;
    }
    if (descriptor_.model_path.empty()) {
      if (error) {
        *error = "dense landmark descriptor missing model path";
      }
      return false;
    }

    bm_status_t status = bm_dev_request(&handle_, 0);
    if (status != BM_SUCCESS) {
      if (error) {
        *error = "bm_dev_request failed";
      }
      close();
      return false;
    }

    if (!firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", firmware.c_str(), 1);
    }

    runtime_ = bmrt_create(handle_);
    if (!runtime_) {
      if (error) {
        *error = "bmrt_create failed";
      }
      close();
      return false;
    }

    if (!bmrt_load_bmodel(runtime_, descriptor_.model_path.c_str())) {
      if (error) {
        *error = "bmrt_load_bmodel failed: " + descriptor_.model_path;
      }
      close();
      return false;
    }

    const char **net_names = nullptr;
    bmrt_get_network_names(runtime_, &net_names);
    if (!net_names || bmrt_get_network_number(runtime_) <= 0) {
      if (error) {
        *error = "dense landmark bmodel has no network";
      }
      if (net_names) {
        std::free(net_names);
      }
      close();
      return false;
    }
    net_name_ = net_names[0];
    std::free(net_names);

    net_info_ = bmrt_get_network_info(runtime_, net_name_.c_str());
    if (!net_info_) {
      if (error) {
        *error = "bmrt_get_network_info failed";
      }
      close();
      return false;
    }
    if (net_info_->input_num != 1 || net_info_->output_num < 1 ||
        net_info_->stage_num < 1) {
      if (error) {
        *error = "unexpected dense landmark network io layout";
      }
      close();
      return false;
    }

    if (!parseInputShape(net_info_->stages[0].input_shapes[0], error)) {
      close();
      return false;
    }

    if (!parseOutputLayout(error)) {
      close();
      return false;
    }
    input_dtype_ = net_info_->input_dtypes[0];
    if (!nchw_layout_ && input_dtype_ == BM_UINT8) {
      tdl_app::bmrt_runtime::VpssPreprocessor::Config vpss_config;
      vpss_config.width = input_width_;
      vpss_config.height = input_height_;
      vpss_config.rgb = normalizeToken(descriptor_.input_type) == "RGB";
      vpss_config.interleaved = true;
      vpss_config.input_dtype = input_dtype_;
      vpss_config.input_scale =
          net_info_->input_scales ? net_info_->input_scales[0] : 1.0f;
      vpss_config.input_zero_point = net_info_->input_zero_point
                                         ? net_info_->input_zero_point[0]
                                         : 0;
      const float mean = descriptor_.mean.empty() ? 0.0f : descriptor_.mean[0];
      const float scale = descriptor_.scale.empty()
                              ? 1.0f / 255.0f
                              : descriptor_.scale[0];
      vpss_config.mean = {{mean, mean, mean}};
      vpss_config.scale = {{scale, scale, scale}};
      hardware_preprocessor_.reset(new tdl_app::bmrt_runtime::VpssPreprocessor());
      if (!hardware_preprocessor_->open(handle_, vpss_config, error) ||
          !allocateDeviceOutputs(error)) {
        close();
        return false;
      }
    }
    return true;
  }

  bool infer(const cv::Mat &crop, std::vector<tdl_app::Point> *points,
             std::string *error) const {
    if (!runtime_ || !net_info_ || !points) {
      if (error) {
        *error = "dense landmark runtime is not initialized";
      }
      return false;
    }

    std::vector<std::uint8_t> input_tensor;
    preprocess(crop, &input_tensor);

    void *input_ptrs[1] = {input_tensor.data()};
    bm_shape_t input_shapes[1];
    input_shapes[0] = net_info_->stages[0].input_shapes[0];

    std::vector<std::vector<std::uint8_t>> output_bytes(
        static_cast<size_t>(net_info_->output_num));
    std::vector<void *> output_ptrs(static_cast<size_t>(net_info_->output_num),
                                    nullptr);
    std::vector<bm_shape_t> output_shapes(
        static_cast<size_t>(net_info_->output_num), bm_shape_t{});

    for (int i = 0; i < net_info_->output_num; ++i) {
      output_bytes[static_cast<size_t>(i)].resize(net_info_->max_output_bytes[i]);
      output_ptrs[static_cast<size_t>(i)] =
          output_bytes[static_cast<size_t>(i)].data();
      std::memset(&output_shapes[static_cast<size_t>(i)], 0, sizeof(bm_shape_t));
    }

    if (!bmrt_launch_data(runtime_, net_name_.c_str(), input_ptrs, input_shapes,
                          1, output_ptrs.data(), output_shapes.data(),
                          net_info_->output_num, true)) {
      if (error) {
        *error = "bmrt_launch_data failed";
      }
      return false;
    }

    return decodePoints(output_bytes, output_shapes, points, error);
  }

  bool inferFrameCrop(void *native_frame,
                      const tdl_app::bmrt_runtime::VpssPreprocessor::Roi &roi,
                      std::vector<tdl_app::Point> *points,
                      DenseProfile *profile, std::string *error) {
    if (!hardware_preprocessor_ || !native_frame || !points) {
      if (error) *error = "dense landmark VPSS path is unavailable";
      return false;
    }
    const auto total_begin = std::chrono::steady_clock::now();
    const auto vpss_begin = total_begin;
    if (!hardware_preprocessor_->preprocess(native_frame, &roi, error)) {
      return false;
    }
    if (profile) profile->vpss_roi_ms = elapsedMs(vpss_begin, std::chrono::steady_clock::now());

    bm_tensor_t input_tensor{};
    bmrt_tensor_with_device(&input_tensor, hardware_preprocessor_->inputMemory(),
                            input_dtype_, net_info_->stages[0].input_shapes[0]);
    std::vector<bm_tensor_t> output_tensors(
        static_cast<size_t>(net_info_->output_num), bm_tensor_t{});
    for (int i = 0; i < net_info_->output_num; ++i) {
      bmrt_tensor_with_device(&output_tensors[static_cast<size_t>(i)],
                              output_memories_[static_cast<size_t>(i)],
                              net_info_->output_dtypes[i],
                              net_info_->stages[0].output_shapes[i]);
    }
    const auto bmrt_begin = std::chrono::steady_clock::now();
    if (!bmrt_launch_tensor_ex(runtime_, net_name_.c_str(), &input_tensor, 1,
                               output_tensors.data(), net_info_->output_num,
                               true, false) || bm_thread_sync(handle_) != BM_SUCCESS) {
      if (error) *error = "dense landmark device launch failed";
      return false;
    }
    if (profile) profile->bmrt_ms = elapsedMs(bmrt_begin, std::chrono::steady_clock::now());

    const auto copy_begin = std::chrono::steady_clock::now();
    std::vector<std::vector<std::uint8_t>> output_bytes(
        static_cast<size_t>(net_info_->output_num));
    std::vector<bm_shape_t> output_shapes(static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      output_shapes[static_cast<size_t>(i)] = output_tensors[static_cast<size_t>(i)].shape;
      size_t count = 1;
      for (int d = 0; d < output_shapes[static_cast<size_t>(i)].num_dims; ++d) {
        count *= static_cast<size_t>(output_shapes[static_cast<size_t>(i)].dims[d]);
      }
      output_bytes[static_cast<size_t>(i)].resize(
          count * bmrt_data_type_size(net_info_->output_dtypes[i]));
      if (bm_memcpy_d2s(handle_, output_bytes[static_cast<size_t>(i)].data(),
                        output_tensors[static_cast<size_t>(i)].device_mem) != BM_SUCCESS) {
        if (error) *error = "dense landmark output copy failed";
        return false;
      }
    }
    const bool ok = decodePoints(output_bytes, output_shapes, points, error);
    if (profile) {
      profile->output_copy_decode_ms = elapsedMs(copy_begin, std::chrono::steady_clock::now());
      profile->total_ms = elapsedMs(total_begin, std::chrono::steady_clock::now());
    }
    return ok;
  }

 private:
  bool parseInputShape(const bm_shape_t &shape, std::string *error) {
    if (shape.num_dims != 4) {
      if (error) {
        *error = "dense landmark model only supports 4D input";
      }
      return false;
    }
    if (shape.dims[3] == 3) {
      nchw_layout_ = false;
      input_height_ = shape.dims[1];
      input_width_ = shape.dims[2];
      return true;
    }
    if (shape.dims[1] == 3) {
      nchw_layout_ = true;
      input_height_ = shape.dims[2];
      input_width_ = shape.dims[3];
      return true;
    }
    if (error) {
      *error = "unable to infer dense landmark input layout";
    }
    return false;
  }

  bool parseOutputLayout(std::string *error) {
    coord_output_index_ = -1;
    score_output_index_ = -1;
    landmark_count_ = 0;
    coordinate_extent_ = static_cast<float>(std::max(input_width_, input_height_));
    size_t best_coord_elements = 0;
    for (int i = 0; i < net_info_->output_num; ++i) {
      size_t element_count = 1;
      const bm_shape_t &shape = net_info_->stages[0].output_shapes[i];
      for (int d = 0; d < shape.num_dims; ++d) {
        element_count *= static_cast<size_t>(shape.dims[d]);
      }
      if (element_count > 3 && element_count % 3 == 0 &&
          element_count > best_coord_elements) {
        coord_output_index_ = i;
        best_coord_elements = element_count;
        landmark_count_ = static_cast<int>(element_count / 3);
      } else if (element_count == 1) {
        score_output_index_ = i;
      }
    }
    if (coord_output_index_ < 0) {
      if (error) {
        *error =
            "dense landmark model does not expose a usable coordinate output";
      }
      return false;
    }
    coordinate_extent_ = inferQuantizedCoordinateExtent(coord_output_index_);
    return true;
  }

  float inferQuantizedCoordinateExtent(int output_index) const {
    const bm_data_type_t dtype = net_info_->output_dtypes[output_index];
    if (dtype != BM_INT8 && dtype != BM_UINT8) {
      return static_cast<float>(std::max(input_width_, input_height_));
    }

    const float scale =
        net_info_->output_scales ? net_info_->output_scales[output_index] : 1.0f;
    const int zero_point = net_info_->output_zero_point
                               ? net_info_->output_zero_point[output_index]
                               : 0;
    const int qmin = (dtype == BM_INT8) ? -128 : 0;
    const int qmax = (dtype == BM_INT8) ? 127 : 255;
    const float min_value = (static_cast<float>(qmin) - zero_point) * scale;
    const float max_value = (static_cast<float>(qmax) - zero_point) * scale;
    const float magnitude =
        std::max(std::fabs(min_value), std::fabs(max_value));
    if (magnitude <= 1.5f) {
      return 1.0f;
    }
    return std::max(16.0f, std::round(magnitude / 16.0f) * 16.0f);
  }

  void preprocess(const cv::Mat &crop,
                  std::vector<std::uint8_t> *tensor) const {
    cv::Mat resized;
    cv::resize(crop, resized, cv::Size(input_width_, input_height_), 0, 0,
               cv::INTER_LINEAR);

    const bool rgb_input = normalizeToken(descriptor_.input_type) == "RGB";
    cv::Mat prepared;
    if (rgb_input) {
      cv::cvtColor(resized, prepared, cv::COLOR_BGR2RGB);
    } else {
      prepared = resized;
    }

    tensor->assign(static_cast<size_t>(input_width_ * input_height_ * 3), 0);
    if (!nchw_layout_) {
      size_t index = 0;
      for (int y = 0; y < prepared.rows; ++y) {
        for (int x = 0; x < prepared.cols; ++x) {
          const cv::Vec3b pixel = prepared.at<cv::Vec3b>(y, x);
          (*tensor)[index++] = pixel[0];
          (*tensor)[index++] = pixel[1];
          (*tensor)[index++] = pixel[2];
        }
      }
      return;
    }

    size_t index = 0;
    for (int c = 0; c < 3; ++c) {
      for (int y = 0; y < prepared.rows; ++y) {
        for (int x = 0; x < prepared.cols; ++x) {
          (*tensor)[index++] = prepared.at<cv::Vec3b>(y, x)[c];
        }
      }
    }
  }

  bool allocateDeviceOutputs(std::string *error) {
    output_memories_.resize(static_cast<size_t>(net_info_->output_num));
    for (int i = 0; i < net_info_->output_num; ++i) {
      size_t bytes = net_info_->max_output_bytes[i];
      if (bytes == 0) {
        const bm_shape_t &shape = net_info_->stages[0].output_shapes[i];
        size_t count = 1;
        for (int d = 0; d < shape.num_dims; ++d) {
          count *= static_cast<size_t>(shape.dims[d]);
        }
        bytes = count * bmrt_data_type_size(net_info_->output_dtypes[i]);
      }
      if (bytes == 0 ||
          bm_malloc_device_byte(handle_, &output_memories_[static_cast<size_t>(i)],
                                bytes) != BM_SUCCESS) {
        if (error) *error = "dense landmark output allocation failed";
        return false;
      }
    }
    return true;
  }

  void releaseDeviceOutputs() {
    if (handle_) {
      for (bm_device_mem_t &memory : output_memories_) {
        if (memory.size > 0) {
          bm_free_device(handle_, memory);
          memory = bm_device_mem_t{};
        }
      }
    }
    output_memories_.clear();
  }

  bool decodePoints(const std::vector<std::vector<std::uint8_t>> &output_bytes,
                    const std::vector<bm_shape_t> &output_shapes,
                    std::vector<tdl_app::Point> *points,
                    std::string *error) const {
    if (!points || coord_output_index_ < 0 ||
        static_cast<size_t>(coord_output_index_) >= output_bytes.size()) {
      if (error) *error = "dense landmark coordinate output is unavailable";
      return false;
    }
    std::vector<float> coords;
    if (!decodeOutput(output_bytes[static_cast<size_t>(coord_output_index_)],
                      output_shapes[static_cast<size_t>(coord_output_index_)],
                      coord_output_index_, &coords, error)) {
      return false;
    }
    if (coords.size() < 3 || coords.size() % 3 != 0) {
      if (error) *error = "unexpected dense landmark coordinate count: " +
                           std::to_string(coords.size());
      return false;
    }
    if (std::getenv("TDL_DENSE_TRACE")) {
      float minimum = std::numeric_limits<float>::infinity();
      float maximum = -std::numeric_limits<float>::infinity();
      for (float value : coords) {
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
      }
      std::cerr << "dense landmark trace: values=" << coords.size()
                << " min=" << minimum << " max=" << maximum
                << " first=";
      const size_t sample_count = std::min<size_t>(18, coords.size());
      for (size_t i = 0; i < sample_count; ++i) {
        std::cerr << (i == 0 ? "" : ",") << coords[i];
      }
      std::cerr << "\n";
    }
    if (!debug_printed_) {
      std::cout << "dense landmark debug: model=" << descriptor_.model_path
                << " input=" << input_width_ << "x" << input_height_
                << " layout=" << (nchw_layout_ ? "nchw" : "nhwc")
                << " points=" << landmark_count_
                << " coord_output_index=" << coord_output_index_
                << " coord_extent=" << coordinate_extent_ << "\n";
      debug_printed_ = true;
    }
    points->clear();
    points->reserve(coords.size() / 3);
    const float x_scale = static_cast<float>(input_width_) /
                          std::max(1.0f, coordinate_extent_);
    const float y_scale = static_cast<float>(input_height_) /
                          std::max(1.0f, coordinate_extent_);
    for (size_t i = 0; i + 2 < coords.size(); i += 3) {
      tdl_app::Point point;
      point.x = std::max(0.0f,
                         std::min(static_cast<float>(input_width_),
                                  coords[i] * x_scale));
      point.y = std::max(0.0f,
                         std::min(static_cast<float>(input_height_),
                                  coords[i + 1] * y_scale));
      points->push_back(point);
    }
    return true;
  }

  bool decodeOutput(const std::vector<std::uint8_t> &raw_bytes,
                    const bm_shape_t &shape, int output_index,
                    std::vector<float> *decoded, std::string *error) const {
    size_t element_count = 1;
    for (int d = 0; d < shape.num_dims; ++d) {
      element_count *= static_cast<size_t>(shape.dims[d]);
    }
    decoded->assign(element_count, 0.0f);

    const float scale =
        net_info_->output_scales ? net_info_->output_scales[output_index] : 1.0f;
    const int zero_point = net_info_->output_zero_point
                               ? net_info_->output_zero_point[output_index]
                               : 0;
    const bm_data_type_t dtype = net_info_->output_dtypes[output_index];

    if (dtype == BM_FLOAT32) {
      const float *ptr = reinterpret_cast<const float *>(raw_bytes.data());
      decoded->assign(ptr, ptr + element_count);
      return true;
    }
    if (dtype == BM_INT8) {
      const int8_t *ptr = reinterpret_cast<const int8_t *>(raw_bytes.data());
      for (size_t i = 0; i < element_count; ++i) {
        (*decoded)[i] =
            (static_cast<int>(ptr[i]) - zero_point) * scale;
      }
      return true;
    }
    if (dtype == BM_UINT8) {
      const uint8_t *ptr = reinterpret_cast<const uint8_t *>(raw_bytes.data());
      for (size_t i = 0; i < element_count; ++i) {
        (*decoded)[i] =
            (static_cast<int>(ptr[i]) - zero_point) * scale;
      }
      return true;
    }

    if (error) {
      *error = "unsupported dense landmark output dtype";
    }
    return false;
  }

  void close() {
    hardware_preprocessor_.reset();
    releaseDeviceOutputs();
    if (runtime_) {
      bmrt_destroy(runtime_);
      runtime_ = nullptr;
    }
    if (handle_) {
      bm_dev_free(handle_);
      handle_ = nullptr;
    }
    net_info_ = nullptr;
    net_name_.clear();
    input_width_ = 0;
    input_height_ = 0;
    nchw_layout_ = false;
    coord_output_index_ = -1;
    score_output_index_ = -1;
    landmark_count_ = 0;
    coordinate_extent_ = 0.0f;
    input_dtype_ = BM_UINT8;
    descriptor_ = tdl_app::ModelDescriptor{};
  }

  tdl_app::ModelDescriptor descriptor_;
  bm_handle_t handle_ = nullptr;
  void *runtime_ = nullptr;
  const bm_net_info_t *net_info_ = nullptr;
  std::string net_name_;
  int input_width_ = 0;
  int input_height_ = 0;
  bool nchw_layout_ = false;
  int coord_output_index_ = -1;
  int score_output_index_ = -1;
  int landmark_count_ = 0;
  float coordinate_extent_ = 0.0f;
  bm_data_type_t input_dtype_ = BM_UINT8;
  std::unique_ptr<tdl_app::bmrt_runtime::VpssPreprocessor>
      hardware_preprocessor_;
  std::vector<bm_device_mem_t> output_memories_;
  mutable bool debug_printed_ = false;
};

bool isCustomDenseLandmarkSpec(const std::string &model_spec) {
  tdl_app::ModelDescriptor descriptor;
  std::string error;
  if (!tdl_app::loadModelDescriptor(model_spec, &descriptor, &error)) {
    return false;
  }
  const std::string runtime = normalizeToken(descriptor.runtime);
  const std::string model_type = normalizeToken(descriptor.model_type);
  const std::string model_name = toUpper(baseName(descriptor.model_path));
  return runtime == "FACE_DENSE_LANDMARK" ||
         model_type == "FACE_LANDMARKS_DENSE" ||
         model_type == "FACE_LANDMARKS_468" ||
         model_type == "FACE_LANDMARKS_478" ||
         model_name.find("FACE_LANDMARKS") != std::string::npos;
}

bool isScrfdDetectorSpec(const std::string &model_spec) {
  tdl_app::ModelDescriptor descriptor;
  std::string error;
  if (!tdl_app::loadModelDescriptor(model_spec, &descriptor, &error)) {
    return false;
  }
  const std::string runtime = normalizeToken(descriptor.runtime);
  const std::string model_type = normalizeToken(descriptor.model_type);
  return runtime == "SCRFD" || model_type.find("SCRFD") != std::string::npos;
}

bool makeHardwareRoi(const tdl_app::Box &box, float expand_ratio,
                     int image_width, int image_height,
                     tdl_app::bmrt_runtime::VpssPreprocessor::Roi *roi) {
  if (!roi || image_width <= 0 || image_height <= 0) return false;
  const tdl_app::Box square = makeSquareBox(box, expand_ratio);
  const int x1 = std::max(0, std::min(image_width - 1,
                                      static_cast<int>(std::floor(square.x1))));
  const int y1 = std::max(0, std::min(image_height - 1,
                                      static_cast<int>(std::floor(square.y1))));
  const int x2 = std::max(x1 + 1, std::min(image_width,
                                      static_cast<int>(std::ceil(square.x2))));
  const int y2 = std::max(y1 + 1, std::min(image_height,
                                      static_cast<int>(std::ceil(square.y2))));
  roi->x = x1;
  roi->y = y1;
  roi->width = x2 - x1;
  roi->height = y2 - y1;
  return true;
}

int runCamera(const Options &opt, tdl_app::FaceDetector *face_detector,
              DenseLandmarkRuntime *dense_landmark, std::string *error) {
  if (!face_detector || !dense_landmark) {
    if (error) *error = "camera dense landmark runtime is null";
    return 5;
  }
  camera_demo_support::CommonOptions camera_options;
  camera_options.group = opt.group;
  camera_options.channel = opt.channel;
  camera_options.timeout_ms = opt.timeout_ms;
  camera_options.frames = opt.frames;
  camera_demo_support::CameraRuntime camera;
  if (!camera_demo_support::openCameraRuntime(camera_options, &camera, error)) {
    return 5;
  }

  const tdl_app::InferOptions detect_options =
      tdl_app::InferOptions::detection(opt.threshold);
  double read_sum_ms = 0.0;
  double detect_sum_ms = 0.0;
  double roi_sum_ms = 0.0;
  double bmrt_sum_ms = 0.0;
  double output_sum_ms = 0.0;
  double post_sum_ms = 0.0;
  double total_sum_ms = 0.0;
  double face_sum = 0.0;
  std::size_t last_points = 0;
  const int total_frames = opt.warmup + opt.frames;

  for (int index = 0; index < total_frames; ++index) {
    tdl_app::Frame frame;
    const auto total_begin = std::chrono::steady_clock::now();
    const auto read_begin = total_begin;
    if (!camera.camera.read(&frame, error)) {
      camera_demo_support::closeCameraRuntime(&camera);
      return 6;
    }
    const auto read_end = std::chrono::steady_clock::now();
    const auto detect_begin = read_end;
    tdl_app::AlgorithmResult detections;
    if (!face_detector->detectFrame(frame, detect_options, &detections, error)) {
      camera.camera.releaseFrame();
      camera_demo_support::closeCameraRuntime(&camera);
      return 7;
    }
    const auto detect_end = std::chrono::steady_clock::now();
    const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
    if (!video) {
      if (error) *error = "camera frame has no native VIDEO_FRAME_INFO_S";
      camera.camera.releaseFrame();
      camera_demo_support::closeCameraRuntime(&camera);
      return 7;
    }

    tdl_app::AlgorithmResult result;
    result.labels = detections.labels;
    DenseProfile frame_profile;
    const auto post_begin = std::chrono::steady_clock::now();
    for (const tdl_app::Box &box : detections.boxes) {
      if (box.class_id != opt.detector_face_class_id) continue;
      tdl_app::bmrt_runtime::VpssPreprocessor::Roi roi;
      if (!makeHardwareRoi(box, opt.roi_expand_ratio,
                           static_cast<int>(video->stVFrame.u32Width),
                           static_cast<int>(video->stVFrame.u32Height), &roi)) {
        continue;
      }
      std::vector<tdl_app::Point> local_points;
      DenseProfile profile;
      if (!dense_landmark->inferFrameCrop(frame.native, roi, &local_points,
                                          &profile, error)) {
        camera.camera.releaseFrame();
        camera_demo_support::closeCameraRuntime(&camera);
        return 8;
      }
      frame_profile.vpss_roi_ms += profile.vpss_roi_ms;
      frame_profile.bmrt_ms += profile.bmrt_ms;
      frame_profile.output_copy_decode_ms += profile.output_copy_decode_ms;
      frame_profile.total_ms += profile.total_ms;
      result.boxes.push_back(box);
      for (const tdl_app::Point &point : local_points) {
        tdl_app::Point mapped;
        mapped.x = roi.x + point.x * roi.width / dense_landmark->inputWidth();
        mapped.y = roi.y + point.y * roi.height / dense_landmark->inputHeight();
        mapped.score = point.score;
        result.points.push_back(mapped);
      }
    }
    const auto post_end = std::chrono::steady_clock::now();

    if (index == total_frames - 1 && !opt.dump_frame.empty()) {
      if (!camera_demo_support::saveFrameAsImage(frame, opt.dump_frame, error) ||
          (!opt.dump_overlay.empty() &&
           !demo_support::saveAnnotatedImage(opt.dump_frame, opt.dump_overlay,
                                             result, error))) {
        camera.camera.releaseFrame();
        camera_demo_support::closeCameraRuntime(&camera);
        return 9;
      }
    }
    camera.camera.releaseFrame();
    if (index < opt.warmup) continue;

    read_sum_ms += elapsedMs(read_begin, read_end);
    detect_sum_ms += elapsedMs(detect_begin, detect_end);
    roi_sum_ms += frame_profile.vpss_roi_ms;
    bmrt_sum_ms += frame_profile.bmrt_ms;
    output_sum_ms += frame_profile.output_copy_decode_ms;
    post_sum_ms += elapsedMs(post_begin, post_end);
    total_sum_ms += elapsedMs(total_begin, std::chrono::steady_clock::now());
    face_sum += result.boxes.size();
    last_points = result.points.size();
  }
  camera_demo_support::closeCameraRuntime(&camera);
  const double count = static_cast<double>(opt.frames);
  const double average_total = total_sum_ms / count;
  std::cout << std::fixed << std::setprecision(3)
            << "camera_frames=" << opt.frames
            << " avg_read_ms=" << read_sum_ms / count
            << " avg_detect_ms=" << detect_sum_ms / count
            << " avg_vpss_roi_ms=" << roi_sum_ms / count
            << " avg_bmrt_ms=" << bmrt_sum_ms / count
            << " avg_output_copy_decode_ms=" << output_sum_ms / count
            << " avg_postprocess_ms=" << post_sum_ms / count
            << " avg_total_ms=" << average_total
            << " fps=" << (average_total > 0.0 ? 1000.0 / average_total : 0.0)
            << " avg_faces=" << face_sum / count
            << " last_dense_points=" << last_points << "\n";
  if (!opt.dump_frame.empty()) std::cout << "saved_frame=" << opt.dump_frame << "\n";
  if (!opt.dump_overlay.empty()) std::cout << "saved_overlay=" << opt.dump_overlay << "\n";
  return 0;
}

int runOffline(const Options &opt, bool use_scrfd_detector,
               bool use_custom_dense_landmark, tdl_app::Detector *detector,
               tdl_app::FaceDetector *face_detector,
               tdl_app::KeypointDetector *keypoint,
               DenseLandmarkRuntime *dense_landmark, std::string *error) {
  cv::Mat image = cv::imread(opt.image, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) {
      *error = "failed to read image: " + opt.image +
               " (pass an existing image path or use --camera)";
    }
    return 2;
  }

  const tdl_app::InferOptions infer_options =
      tdl_app::InferOptions::detection(opt.threshold);
  const int total_runs = opt.warmup + opt.frames;
  double detect_sum_ms = 0.0;
  double crop_sum_ms = 0.0;
  double dense_sum_ms = 0.0;
  double post_sum_ms = 0.0;
  double total_sum_ms = 0.0;
  tdl_app::AlgorithmResult final_result;
  std::size_t total_points = 0;

  for (int run = 0; run < total_runs; ++run) {
    const auto total_begin = std::chrono::steady_clock::now();
    const auto detect_begin = total_begin;
    tdl_app::AlgorithmResult detect_result;
    const bool detect_ok =
        use_scrfd_detector
            ? face_detector->run(opt.image, infer_options, &detect_result, error)
            : detector->run(opt.image, infer_options, &detect_result, error);
    if (!detect_ok) {
      return 5;
    }
    const auto detect_end = std::chrono::steady_clock::now();

    tdl_app::AlgorithmResult run_result;
    run_result.labels = detect_result.labels;
    std::size_t run_points = 0;
    double run_crop_ms = 0.0;
    double run_dense_ms = 0.0;
    const auto post_begin = detect_end;
    for (const tdl_app::Box &box : detect_result.boxes) {
      if (box.class_id != opt.detector_face_class_id) continue;
      run_result.boxes.push_back(box);

      std::vector<tdl_app::Point> local_points;
      SquareCrop square_crop;
      const auto crop_begin = std::chrono::steady_clock::now();
      if (use_custom_dense_landmark) {
        // KEYPOINT_FACE_V2 expects an expanded square face crop. Applying a
        // five-point affine alignment here changes the model's input geometry.
        const tdl_app::Box roi = makeSquareBox(box, opt.roi_expand_ratio);
        if (!extractSquareCrop(image, roi, &square_crop, error)) {
          continue;
        }
      } else {
        const tdl_app::Box roi = makeSquareBox(box, opt.roi_expand_ratio);
        if (!extractSquareCrop(image, roi, &square_crop, error)) {
          continue;
        }
      }
      const auto crop_end = std::chrono::steady_clock::now();
      const auto dense_begin = crop_end;
      if (use_custom_dense_landmark) {
        if (!dense_landmark->infer(square_crop.image, &local_points, error)) {
          return 7;
        }
      } else {
        TempFile crop_file;
        tdl_app::KeypointResult keypoint_result;
        if (!writeTempCrop(square_crop.image,
                           static_cast<int>(run_result.boxes.size() - 1),
                           &crop_file, error) ||
            !keypoint->run(crop_file.path, &keypoint_result, error)) {
          if (error && error->empty()) *error = "keypoint inference failed";
          return 7;
        }
        local_points = keypoint_result.points;
      }
      const auto dense_end = std::chrono::steady_clock::now();
      run_crop_ms += elapsedMs(crop_begin, crop_end);
      run_dense_ms += elapsedMs(dense_begin, dense_end);

      for (const tdl_app::Point &point : local_points) {
        tdl_app::Point mapped;
        mapped = point;
        mapped.x = square_crop.x + point.x * square_crop.size /
                                     dense_landmark->inputWidth();
        mapped.y = square_crop.y + point.y * square_crop.size /
                                     dense_landmark->inputHeight();
        mapped.x = std::max(0.0f,
                            std::min(mapped.x,
                                     static_cast<float>(image.cols - 1)));
        mapped.y = std::max(0.0f,
                            std::min(mapped.y,
                                     static_cast<float>(image.rows - 1)));
        run_result.points.push_back(mapped);
      }
      run_points += local_points.size();
    }
    run_result.text = "faces=" + std::to_string(run_result.boxes.size()) +
                      " dense_points=" + std::to_string(run_points);
    const auto total_end = std::chrono::steady_clock::now();
    if (run >= opt.warmup) {
      detect_sum_ms += elapsedMs(detect_begin, detect_end);
      crop_sum_ms += run_crop_ms;
      dense_sum_ms += run_dense_ms;
      post_sum_ms += elapsedMs(post_begin, total_end) - run_crop_ms - run_dense_ms;
      total_sum_ms += elapsedMs(total_begin, total_end);
    }
    final_result = std::move(run_result);
    total_points = run_points;
  }

  if (!opt.output.empty() &&
      !demo_support::saveAnnotatedImage(opt.image, opt.output, final_result,
                                        error)) {
    return 8;
  }

  const double count = static_cast<double>(opt.frames);
  const double average_total = total_sum_ms / count;
  std::cout << std::fixed << std::setprecision(3)
            << "offline_runs=" << opt.frames
            << " avg_detect_ms=" << detect_sum_ms / count
            << " avg_crop_align_ms=" << crop_sum_ms / count
            << " avg_dense_infer_ms=" << dense_sum_ms / count
            << " avg_postprocess_ms=" << post_sum_ms / count
            << " avg_total_ms=" << average_total
            << " fps=" << (average_total > 0.0 ? 1000.0 / average_total : 0.0)
            << " last_faces=" << final_result.boxes.size()
            << " last_dense_points=" << total_points << "\n";
  if (!opt.output.empty()) std::cout << "saved: " << opt.output << "\n";
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

  const bool use_scrfd_detector = isScrfdDetectorSpec(opt.detector_model_spec);
  tdl_app::Detector detector;
  tdl_app::FaceDetector face_detector;
  if (use_scrfd_detector) {
    tdl_app::FaceDetector::Config detector_config;
    detector_config.model_spec = opt.detector_model_spec;
    detector_config.firmware = opt.firmware;
    if (!face_detector.load(detector_config, &error)) {
      std::cerr << "detector initialize failed: " << error << "\n";
      return 3;
    }
  } else {
    tdl_app::Detector::Config detector_config;
    detector_config.model_spec = opt.detector_model_spec;
    detector_config.firmware = opt.firmware;
    if (!detector.load(detector_config, &error)) {
      std::cerr << "detector initialize failed: " << error << "\n";
      return 3;
    }
  }

  const bool use_custom_dense_landmark =
      isCustomDenseLandmarkSpec(opt.keypoint_model_spec);
  tdl_app::KeypointDetector keypoint;
  DenseLandmarkRuntime dense_landmark;
  if (use_custom_dense_landmark) {
    if (!dense_landmark.open(opt.keypoint_model_spec, opt.firmware, &error)) {
      std::cerr << "dense landmark initialize failed: " << error << "\n";
      return 4;
    }
  } else {
    tdl_app::KeypointDetector::Config keypoint_config;
    keypoint_config.model_spec = opt.keypoint_model_spec;
    keypoint_config.firmware = opt.firmware;
    if (!keypoint.load(keypoint_config, &error)) {
      std::cerr << "keypoint initialize failed: " << error << "\n";
      return 4;
    }
  }

  if (opt.camera) {
    if (!use_scrfd_detector || !use_custom_dense_landmark) {
      std::cerr << "--camera requires SCRFD detection and the custom dense landmark model\n";
      return 5;
    }
    const int ret = runCamera(opt, &face_detector, &dense_landmark, &error);
    if (ret != 0) {
      std::cerr << "camera dense landmark failed: " << error << "\n";
    }
    return ret;
  }

  const int ret = runOffline(opt, use_scrfd_detector, use_custom_dense_landmark,
                             &detector, &face_detector, &keypoint,
                             &dense_landmark, &error);
  if (ret != 0) {
    std::cerr << "offline dense landmark failed: " << error << "\n";
  }
  return ret;
}
