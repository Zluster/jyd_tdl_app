#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "bmlib_runtime.h"
#include "bmruntime_interface.h"
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
      << "  tdl_face_dense_keypoint_demo --image FILE\n"
      << "      --detector-model-spec FILE --keypoint-model-spec FILE\n"
      << "      [--firmware FILE] [--threshold 0.25] [--output FILE]\n"
      << "      [--roi-expand-ratio 0.0] [--detector-face-class-id 0]\n";
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

  if (opt->image.empty()) {
    std::cerr << "image path is required\n";
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

    std::vector<float> coords;
    if (!decodeOutput(output_bytes[static_cast<size_t>(coord_output_index_)],
                      output_shapes[static_cast<size_t>(coord_output_index_)],
                      coord_output_index_, &coords, error)) {
      return false;
    }
    if (coords.size() < 3 || coords.size() % 3 != 0) {
      if (error) {
        *error = "unexpected dense landmark coordinate count: " +
                 std::to_string(coords.size());
      }
      return false;
    }

    if (!debug_printed_) {
      float min_x = std::numeric_limits<float>::max();
      float min_y = std::numeric_limits<float>::max();
      float min_z = std::numeric_limits<float>::max();
      float max_x = std::numeric_limits<float>::lowest();
      float max_y = std::numeric_limits<float>::lowest();
      float max_z = std::numeric_limits<float>::lowest();
      for (size_t i = 0; i + 2 < coords.size(); i += 3) {
        min_x = std::min(min_x, coords[i + 0]);
        min_y = std::min(min_y, coords[i + 1]);
        min_z = std::min(min_z, coords[i + 2]);
        max_x = std::max(max_x, coords[i + 0]);
        max_y = std::max(max_y, coords[i + 1]);
        max_z = std::max(max_z, coords[i + 2]);
      }

      std::cout << "dense landmark debug: model=" << descriptor_.model_path
                << " input=" << input_width_ << "x" << input_height_
                << " layout=" << (nchw_layout_ ? "nchw" : "nhwc")
                << " points=" << landmark_count_
                << " coord_output_index=" << coord_output_index_
                << " coord_extent=" << coordinate_extent_ << "\n";
      std::cout << "dense landmark debug: xyz0=(" << coords[0] << ","
                << coords[1] << "," << coords[2] << ") xyz1=(" << coords[3]
                << "," << coords[4] << "," << coords[5] << ") xyz2=("
                << coords[6] << "," << coords[7] << "," << coords[8]
                << ")\n";
      std::cout << "dense landmark debug: x_range=[" << min_x << "," << max_x
                << "] y_range=[" << min_y << "," << max_y << "] z_range=["
                << min_z << "," << max_z << "]\n";
      if (score_output_index_ >= 0) {
        std::vector<float> score;
        if (decodeOutput(output_bytes[static_cast<size_t>(score_output_index_)],
                         output_shapes[static_cast<size_t>(score_output_index_)],
                         score_output_index_, &score, nullptr) &&
            !score.empty()) {
          std::cout << "dense landmark debug: score=" << score[0] << "\n";
        }
      }
      debug_printed_ = true;
    }

    points->clear();
    points->reserve(coords.size() / 3);
    for (size_t i = 0; i + 2 < coords.size(); i += 3) {
      tdl_app::Point point;
      point.x = std::max(0.0f,
                         std::min(static_cast<float>(crop.cols), coords[i + 0]));
      point.y = std::max(0.0f,
                         std::min(static_cast<float>(crop.rows), coords[i + 1]));
      points->push_back(point);
    }
    return true;
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

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  cv::Mat image = cv::imread(opt.image, cv::IMREAD_COLOR);
  if (image.empty()) {
    std::cerr << "failed to read image: " << opt.image << "\n";
    return 2;
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

  tdl_app::AlgorithmResult detect_result;
  const tdl_app::InferOptions infer_options =
      tdl_app::InferOptions::detection(opt.threshold);
  const bool detect_ok = use_scrfd_detector
                             ? face_detector.run(opt.image, infer_options,
                                                 &detect_result, &error)
                             : detector.run(opt.image, infer_options,
                                            &detect_result, &error);
  if (!detect_ok) {
    std::cerr << "face detect failed: " << error << "\n";
    return 5;
  }

  tdl_app::AlgorithmResult final_result;
  final_result.labels = detect_result.labels;
  for (const auto &box : detect_result.boxes) {
    if (box.class_id == opt.detector_face_class_id) {
      final_result.boxes.push_back(box);
    }
  }

  std::size_t total_points = 0;
  for (std::size_t i = 0; i < final_result.boxes.size(); ++i) {
    std::vector<tdl_app::Point> local_points;
    bool used_affine_crop = false;
    bool used_box_heuristic = false;
    AffineCrop affine_crop;
    SquareCrop square_crop;
    if (use_custom_dense_landmark) {
      if (extractAlignedFaceCrop(image, final_result.boxes[i],
                                 dense_landmark.inputWidth(),
                                 opt.roi_expand_ratio, &used_box_heuristic,
                                 &affine_crop, &error)) {
        used_affine_crop = true;
      } else {
        std::cerr << "aligned face crop failed for face[" << i
                  << "]: " << error << "\n";
        return 6;
      }

      const cv::Mat &dense_input = affine_crop.image;
      if (!dense_landmark.infer(dense_input, &local_points, &error)) {
        std::cerr << "dense landmark failed for face[" << i
                  << "]: " << error << "\n";
        return 7;
      }
    } else {
      const tdl_app::Box roi =
          makeSquareBox(final_result.boxes[i], opt.roi_expand_ratio);
      if (!extractSquareCrop(image, roi, &square_crop, &error)) {
        std::cerr << "skip face[" << i << "]: " << error << "\n";
        continue;
      }

      TempFile crop_file;
      if (!writeTempCrop(square_crop.image, static_cast<int>(i), &crop_file,
                         &error)) {
        std::cerr << "failed to create crop for face[" << i << "]: " << error
                  << "\n";
        return 6;
      }

      tdl_app::KeypointResult keypoint_result;
      if (!keypoint.run(crop_file.path, &keypoint_result, &error)) {
        std::cerr << "keypoint failed for face[" << i << "]: " << error
                  << "\n";
        return 7;
      }
      local_points = keypoint_result.points;
    }

    if (used_affine_crop) {
      std::cout << "face[" << i << "] roi=(affine_aligned,"
                << affine_crop.image.cols << "x" << affine_crop.image.rows
                << ")";
    } else {
      std::cout << "face[" << i << "] roi=(" << square_crop.x << ","
                << square_crop.y << "," << (square_crop.x + square_crop.size)
                << "," << (square_crop.y + square_crop.size) << ")";
    }
    std::cout
              << " dense_points=" << local_points.size() << "\n";

    for (const auto &point : local_points) {
      tdl_app::Point mapped;
      if (used_affine_crop) {
        const double x = static_cast<double>(point.x);
        const double y = static_cast<double>(point.y);
        mapped.x = static_cast<float>(affine_crop.inverse_transform.at<double>(0, 0) * x +
                                      affine_crop.inverse_transform.at<double>(0, 1) * y +
                                      affine_crop.inverse_transform.at<double>(0, 2));
        mapped.y = static_cast<float>(affine_crop.inverse_transform.at<double>(1, 0) * x +
                                      affine_crop.inverse_transform.at<double>(1, 1) * y +
                                      affine_crop.inverse_transform.at<double>(1, 2));
      } else {
        mapped = point;
        mapped.x += square_crop.x;
        mapped.y += square_crop.y;
      }
      mapped.x = std::max(0.0f,
                          std::min(mapped.x, static_cast<float>(image.cols - 1)));
      mapped.y = std::max(0.0f,
                          std::min(mapped.y, static_cast<float>(image.rows - 1)));
      final_result.points.push_back(mapped);
    }
    total_points += local_points.size();
  }

  final_result.text = "faces=" + std::to_string(final_result.boxes.size()) +
                      " dense_points=" + std::to_string(total_points);

  if (!opt.output.empty() &&
      !demo_support::saveAnnotatedImage(opt.image, opt.output, final_result,
                                        &error)) {
    std::cerr << "save failed: " << error << "\n";
    return 8;
  }

  if (!opt.output.empty()) {
    std::cout << "saved: " << opt.output << "\n";
  }

  std::cout << "face_count: " << final_result.boxes.size() << "\n";
  std::cout << "dense_point_count: " << total_points << "\n";
  return 0;
}
