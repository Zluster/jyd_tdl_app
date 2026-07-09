#include "tdl_app/nn_plate_recognizer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <cstring>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "algorithm/private/bmrt_utils.hpp"
#include "algorithm/private/frame_convert.hpp"
#include "cvi_comm_video.h"
#include "cvi_sys.h"

namespace tdl_app {
namespace {

const std::vector<std::string> kLegacyPlateChars = {
    "浜?", "娌?", "娲?", "娓?", "鍐€", "鏅?", "钂?", "杈?", "鍚?", "榛?",
    "鑻?", "娴?", "鐨?", "闂?", "璧?", "椴?", "璞?", "閯?", "婀?", "绮?",
    "妗?", "鐞?", "宸?", "璐?", "浜?", "钘?", "闄?", "鐢?", "闈?", "瀹?",
    "鏂?", "瀛?", "璀?", "娓?", "婢?", "鎸?", "浣?", "棰?", "姘?", "娣?",
    "鍗?", "闄?", "绌?", "0", "1", "2", "3", "4", "5", "6", "7", "8",
    "9", "A", "B", "C", "D", "E", "F", "G", "H", "J", "K", "L", "M", "N",
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "I", "O", "-"};

std::string normalizeToken(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                 });
  std::replace(value.begin(), value.end(), '-', '_');
  return value;
}

bool isTruthy(const std::string &value) {
  const std::string token = normalizeToken(value);
  return token == "1" || token == "TRUE" || token == "YES" || token == "ON";
}

std::vector<std::string> loadLabelFile(const std::string &path) {
  std::vector<std::string> labels;
  std::ifstream ifs(path);
  if (!ifs) {
    return labels;
  }

  std::string line;
  while (std::getline(ifs, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      labels.push_back(line);
    }
  }
  return labels;
}

std::vector<float> parseFloatList(const std::string &value) {
  std::vector<float> parsed;
  std::string token;
  std::istringstream ss(value);
  while (std::getline(ss, token, ',')) {
    try {
      parsed.push_back(std::stof(token));
    } catch (...) {
      parsed.clear();
      break;
    }
  }
  return parsed;
}

std::string joinTexts(const std::vector<std::string> &texts) {
  std::string out;
  for (size_t i = 0; i < texts.size(); ++i) {
    if (texts[i].empty()) {
      continue;
    }
    if (!out.empty()) {
      out += '\n';
    }
    out += texts[i];
  }
  return out;
}

std::string resolveRelativeToDescriptor(const ModelDescriptor &descriptor,
                                        const std::string &value) {
  if (value.empty()) {
    return std::string();
  }
  if (value.front() == '/' ||
      (value.size() >= 2 && value[1] == ':')) {
    return value;
  }
  return descriptor.descriptor_dir + "/" + value;
}

void compensateQuantizedInputScale(const bmrt_runtime::Session &session,
                                   std::vector<float> *tensor) {
  if (!tensor) {
    return;
  }
  const bm_net_info_t *net_info = session.netInfo();
  if (!net_info || !net_info->input_scales || net_info->input_num < 1) {
    return;
  }
  const bm_data_type_t dtype = net_info->input_dtypes[0];
  if (dtype != BM_INT8 && dtype != BM_UINT8) {
    return;
  }
  const float input_scale = net_info->input_scales[0];
  if (input_scale == 0.0f || std::fabs(input_scale) <= 1.0f) {
    return;
  }
  const float factor = input_scale * input_scale;
  for (float &value : *tensor) {
    value *= factor;
  }
}

cv::Mat resizeWithAspectRatio(const cv::Mat &image, int target_width,
                              int target_height, float *ratio,
                              int *pad_top, int *pad_left) {
  *ratio = std::min(static_cast<float>(target_height) / image.rows,
                    static_cast<float>(target_width) / image.cols);
  const int resized_w = static_cast<int>(std::round(image.cols * (*ratio)));
  const int resized_h = static_cast<int>(std::round(image.rows * (*ratio)));
  *pad_top = (target_height - resized_h) / 2;
  *pad_left = (target_width - resized_w) / 2;

  cv::Mat resized;
  cv::resize(image, resized, cv::Size(resized_w, resized_h), 0, 0,
             cv::INTER_LINEAR);

  cv::Mat padded(target_height, target_width, CV_8UC3, cv::Scalar(0, 0, 0));
  resized.copyTo(padded(cv::Rect(*pad_left, *pad_top, resized_w, resized_h)));
  return padded;
}

float polygonScore(const cv::Mat &prob_map, const std::vector<cv::Point> &poly) {
  if (poly.empty()) {
    return 0.0f;
  }
  cv::Rect rect = cv::boundingRect(poly);
  rect &= cv::Rect(0, 0, prob_map.cols, prob_map.rows);
  if (rect.width <= 0 || rect.height <= 0) {
    return 0.0f;
  }

  cv::Mat mask(rect.height, rect.width, CV_8UC1, cv::Scalar(0));
  std::vector<std::vector<cv::Point>> shifted(1);
  shifted[0].reserve(poly.size());
  for (const auto &pt : poly) {
    shifted[0].push_back(cv::Point(pt.x - rect.x, pt.y - rect.y));
  }
  cv::fillPoly(mask, shifted, cv::Scalar(255));

  const cv::Scalar mean_value = cv::mean(prob_map(rect), mask);
  return static_cast<float>(mean_value[0]);
}

cv::RotatedRect safeMinAreaRect(const std::vector<cv::Point> &poly) {
  if (poly.size() >= 3) {
    return cv::minAreaRect(poly);
  }
  if (poly.empty()) {
    return cv::RotatedRect();
  }
  const cv::Rect rect = cv::boundingRect(poly);
  return cv::RotatedRect(
      cv::Point2f(rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f),
      cv::Size2f(static_cast<float>(rect.width), static_cast<float>(rect.height)),
      0.0f);
}

cv::RotatedRect expandRotatedRect(const cv::RotatedRect &rect,
                                  float unclip_ratio) {
  if (unclip_ratio <= 0.0f) {
    return rect;
  }
  const float width = std::max(rect.size.width, 1.0f);
  const float height = std::max(rect.size.height, 1.0f);
  const float area = width * height;
  const float perimeter = 2.0f * (width + height);
  if (perimeter <= 0.0f) {
    return rect;
  }

  const float distance = area * unclip_ratio / perimeter;
  cv::RotatedRect expanded = rect;
  expanded.size.width = width + distance * 2.0f;
  expanded.size.height = height + distance * 2.0f;
  return expanded;
}

cv::Mat cropRotatedRect(const cv::Mat &image, const cv::RotatedRect &rect) {
  if (image.empty()) {
    return cv::Mat();
  }
  cv::Point2f points[4];
  rect.points(points);
  std::vector<cv::Point2f> src(4);
  for (int i = 0; i < 4; ++i) {
    src[static_cast<size_t>(i)] = points[i];
  }

  float width = std::max(rect.size.width, rect.size.height);
  float height = std::min(rect.size.width, rect.size.height);
  if (width < 1.0f || height < 1.0f) {
    return cv::Mat();
  }

  std::sort(src.begin(), src.end(), [](const cv::Point2f &lhs,
                                       const cv::Point2f &rhs) {
    return lhs.y < rhs.y || (lhs.y == rhs.y && lhs.x < rhs.x);
  });
  std::vector<cv::Point2f> top{src[0], src[1]};
  std::vector<cv::Point2f> bottom{src[2], src[3]};
  std::sort(top.begin(), top.end(),
            [](const cv::Point2f &lhs, const cv::Point2f &rhs) {
              return lhs.x < rhs.x;
            });
  std::sort(bottom.begin(), bottom.end(),
            [](const cv::Point2f &lhs, const cv::Point2f &rhs) {
              return lhs.x < rhs.x;
            });

  std::vector<cv::Point2f> ordered = {
      top[0], top[1], bottom[1], bottom[0]};
  std::vector<cv::Point2f> dst = {
      cv::Point2f(0.0f, 0.0f),
      cv::Point2f(width - 1.0f, 0.0f),
      cv::Point2f(width - 1.0f, height - 1.0f),
      cv::Point2f(0.0f, height - 1.0f)};

  cv::Mat transform = cv::getPerspectiveTransform(ordered, dst);
  cv::Mat warped;
  cv::warpPerspective(image, warped, transform,
                      cv::Size(static_cast<int>(std::round(width)),
                               static_cast<int>(std::round(height))),
                      cv::INTER_LINEAR, cv::BORDER_REPLICATE);
  if (warped.rows > warped.cols * 1.5f) {
    cv::rotate(warped, warped, cv::ROTATE_90_CLOCKWISE);
  }
  return warped;
}

struct OcrLine {
  Box box;
  std::string text;
  float score = 0.0f;
};

}  // namespace

class NnPlateRecognizer::CustomRuntime {
 public:
  bool open(const EngineConfig &config, const ModelDescriptor &descriptor,
            std::string *error) {
    descriptor_ = descriptor;
    runtime_name_ = normalizeToken(descriptor.runtime.empty()
                                       ? descriptor.model_type
                                       : descriptor.runtime);
    if (runtime_name_ == "PP_OCR" ||
        normalizeToken(descriptor.model_type) == "PP_OCR") {
      return openPpOcr(config, descriptor, error);
    }
    return openSingleRecognizer(config, descriptor, error);
  }

  bool inferImage(const std::string &image_path, const Box *roi,
                  AlgorithmResult *result, std::string *error) {
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      bmrt_runtime::setError(error, "failed to read image: " + image_path);
      return false;
    }
    return inferMat(image, roi, result, error);
  }

  bool inferMat(const cv::Mat &image, const Box *roi, AlgorithmResult *result,
                std::string *error) {
    if (!result) {
      bmrt_runtime::setError(error, "result pointer is null");
      return false;
    }
    if (image.empty()) {
      bmrt_runtime::setError(error, "input image is empty");
      return false;
    }

    cv::Mat cropped = image;
    cv::Rect image_roi(0, 0, image.cols, image.rows);
    if (roi) {
      image_roi = bmrt_runtime::clampRoi(*roi, image.cols, image.rows);
      cropped = image(image_roi).clone();
    }

    if (pp_ocr_mode_) {
      return inferPpOcr(cropped, image_roi, result, error);
    }
    return inferSingleRecognizer(cropped, result, error);
  }

 private:
  bool openSingleRecognizer(const EngineConfig &config,
                            const ModelDescriptor &descriptor,
                            std::string *error) {
    if (!single_session_.open(config, descriptor, error)) {
      return false;
    }
    single_mean_ =
        bmrt_runtime::expandChannelValues(descriptor.mean, 127.5f);
    single_scale_ =
        bmrt_runtime::expandChannelValues(descriptor.scale, 1.0f / 128.0f);
    labels_ = descriptor.labels;
    if (labels_.empty()) {
      labels_ = kLegacyPlateChars;
    }
    return true;
  }

  bool openPpOcr(const EngineConfig &config, const ModelDescriptor &descriptor,
                 std::string *error) {
    pp_ocr_mode_ = true;

    if (!det_session_.open(config, descriptor, error)) {
      return false;
    }
    det_mean_ = bmrt_runtime::expandChannelValues(descriptor.mean, 0.0f);
    det_scale_ = bmrt_runtime::expandChannelValues(descriptor.scale, 1.0f);

    const auto rec_model_it = descriptor.extra.find("rec_model");
    if (rec_model_it == descriptor.extra.end() || rec_model_it->second.empty()) {
      bmrt_runtime::setError(error, "pp_ocr descriptor missing rec_model");
      return false;
    }

    ModelDescriptor rec_descriptor = descriptor;
    rec_descriptor.model_path =
        resolveRelativeToDescriptor(descriptor, rec_model_it->second);
    rec_descriptor.mean.clear();
    rec_descriptor.scale.clear();
    rec_descriptor.labels.clear();
    rec_descriptor.input_type = descriptor.input_type;
    rec_descriptor.runtime = "PLATE_RECOGNIZER";
    rec_descriptor.model_type = "PP_OCR_REC";

    const auto rec_mean_it = descriptor.extra.find("rec_mean");
    if (rec_mean_it != descriptor.extra.end()) {
      rec_descriptor.mean = parseFloatList(rec_mean_it->second);
    }
    const auto rec_scale_it = descriptor.extra.find("rec_scale");
    if (rec_scale_it != descriptor.extra.end()) {
      rec_descriptor.scale = parseFloatList(rec_scale_it->second);
    }

    const auto labels_it = descriptor.extra.find("labels");
    if (labels_it != descriptor.extra.end()) {
      label_path_ = resolveRelativeToDescriptor(descriptor, labels_it->second);
      labels_ = loadLabelFile(label_path_);
    }
    if (labels_.empty()) {
      bmrt_runtime::setError(
          error, "pp_ocr label file missing or empty: " + label_path_);
      return false;
    }

    if (!rec_session_.open(config, rec_descriptor, error)) {
      return false;
    }
    rec_mean_ = bmrt_runtime::expandChannelValues(rec_descriptor.mean, 127.5f);
    rec_scale_ =
        bmrt_runtime::expandChannelValues(rec_descriptor.scale, 1.0f / 128.0f);

    const auto det_flag_it = descriptor.extra.find("det");
    if (det_flag_it != descriptor.extra.end()) {
      det_enabled_ = isTruthy(det_flag_it->second);
    }
    if (!det_enabled_) {
      bmrt_runtime::setError(error, "pp_ocr requires det=true");
      return false;
    }

    const auto thresh_it = descriptor.extra.find("det_box_thresh");
    if (thresh_it != descriptor.extra.end()) {
      det_box_thresh_ = std::stof(thresh_it->second);
    }
    const auto bin_thresh_it = descriptor.extra.find("det_thresh");
    if (bin_thresh_it != descriptor.extra.end()) {
      det_binary_thresh_ = std::stof(bin_thresh_it->second);
    }
    const auto unclip_it = descriptor.extra.find("det_unclip_ratio");
    if (unclip_it != descriptor.extra.end()) {
      det_unclip_ratio_ = std::stof(unclip_it->second);
    }
    const auto min_size_it = descriptor.extra.find("det_min_size");
    if (min_size_it != descriptor.extra.end()) {
      det_min_size_ = std::stoi(min_size_it->second);
    }
    return true;
  }

  bool inferSingleRecognizer(const cv::Mat &cropped, AlgorithmResult *result,
                             std::string *error) {
    cv::Mat resized;
    cv::resize(cropped, resized,
               cv::Size(single_session_.inputWidth(), single_session_.inputHeight()),
               0, 0, cv::INTER_LINEAR);

    std::vector<float> input_tensor;
    bmrt_runtime::writeImageToTensor(
        resized, bmrt_runtime::wantsRgbInput(descriptor_, false),
        single_session_.nchwLayout(), single_mean_, single_scale_, &input_tensor);

    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!single_session_.launch(input_tensor, &outputs, error)) {
      return false;
    }
    if (outputs.empty()) {
      bmrt_runtime::setError(error, "plate recognizer produced no outputs");
      return false;
    }

    *result = AlgorithmResult{};
    result->text = greedyDecodeLegacy(outputs[0], labels_, nullptr);
    return true;
  }

  bool inferPpOcr(const cv::Mat &image, const cv::Rect &image_roi,
                  AlgorithmResult *result, std::string *error) {
    std::vector<OcrLine> lines;
    if (!runDetection(image, image_roi, &lines, error)) {
      return false;
    }

    result->clear();
    std::vector<std::string> texts;
    for (const auto &line : lines) {
      result->boxes.push_back(line.box);
      result->attributes.push_back({"ocr_text:" + line.text, line.score});
      texts.push_back(line.text);
    }
    result->text = joinTexts(texts);
    return true;
  }

  bool runDetection(const cv::Mat &image, const cv::Rect &image_roi,
                    std::vector<OcrLine> *lines, std::string *error) {
    float ratio = 1.0f;
    int pad_top = 0;
    int pad_left = 0;
    cv::Mat padded = resizeWithAspectRatio(image, det_session_.inputWidth(),
                                           det_session_.inputHeight(), &ratio,
                                           &pad_top, &pad_left);

    std::vector<float> input_tensor;
    bmrt_runtime::writeImageToTensor(
        padded, bmrt_runtime::wantsRgbInput(descriptor_, false),
        det_session_.nchwLayout(), det_mean_, det_scale_, &input_tensor);
    compensateQuantizedInputScale(det_session_, &input_tensor);

    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!det_session_.launch(input_tensor, &outputs, error)) {
      return false;
    }
    if (outputs.empty()) {
      bmrt_runtime::setError(error, "pp_ocr det produced no outputs");
      return false;
    }

    const auto &tensor = outputs[0];
    if (tensor.shape.num_dims != 4 || tensor.data.empty()) {
      bmrt_runtime::setError(error, "unexpected pp_ocr det output shape");
      return false;
    }

    const int height = tensor.shape.dims[tensor.shape.num_dims - 2];
    const int width = tensor.shape.dims[tensor.shape.num_dims - 1];
    cv::Mat prob(height, width, CV_32FC1);
    float prob_min = std::numeric_limits<float>::max();
    float prob_max = std::numeric_limits<float>::lowest();
    double prob_sum = 0.0;
    for (int y = 0; y < height; ++y) {
      float *row = prob.ptr<float>(y);
      for (int x = 0; x < width; ++x) {
        const float value = tensor.data[static_cast<size_t>(y * width + x)];
        row[x] = value;
        prob_min = std::min(prob_min, value);
        prob_max = std::max(prob_max, value);
        prob_sum += value;
      }
    }
    const double prob_mean = prob_sum / static_cast<double>(width * height);

    cv::Mat bitmap;
    cv::threshold(prob, bitmap, det_binary_thresh_, 255, cv::THRESH_BINARY);
    bitmap.convertTo(bitmap, CV_8UC1);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
    if (!det_debug_printed_) {
      std::cout << "pp_ocr det debug: image=" << image.cols << "x" << image.rows
                << " padded=" << padded.cols << "x" << padded.rows
                << " ratio=" << ratio
                << " pad=(" << pad_left << "," << pad_top << ")\n";
      std::cout << "pp_ocr det debug: prob_range=[" << prob_min << ","
                << prob_max << "] prob_mean=" << prob_mean
                << " det_thresh=" << det_binary_thresh_
                << " box_thresh=" << det_box_thresh_ << "\n";
      std::cout << "pp_ocr det debug: contours=" << contours.size() << "\n";
    }

    lines->clear();
    size_t accepted_contours = 0;
    size_t accepted_crops = 0;
    for (const auto &contour : contours) {
      if (contour.size() < 4) {
        continue;
      }

      const float score = polygonScore(prob, contour);
      if (score < det_box_thresh_) {
        continue;
      }
      ++accepted_contours;

      const cv::RotatedRect min_rect = safeMinAreaRect(contour);
      if (std::min(min_rect.size.width, min_rect.size.height) < det_min_size_) {
        continue;
      }

      const cv::RotatedRect expanded_rect =
          expandRotatedRect(min_rect, det_unclip_ratio_);

      cv::Mat crop = cropRotatedRect(padded, expanded_rect);
      if (crop.empty()) {
        continue;
      }

      std::string text;
      float rec_score = 0.0f;
      if (!runRecognition(crop, &text, &rec_score, error)) {
        return false;
      }
      if (text.empty()) {
        continue;
      }
      ++accepted_crops;

      cv::Point2f rect_points[4];
      expanded_rect.points(rect_points);
      float min_x = std::numeric_limits<float>::max();
      float min_y = std::numeric_limits<float>::max();
      float max_x = std::numeric_limits<float>::lowest();
      float max_y = std::numeric_limits<float>::lowest();
      for (const auto &pt : rect_points) {
        const float x = (pt.x - pad_left) / ratio + static_cast<float>(image_roi.x);
        const float y = (pt.y - pad_top) / ratio + static_cast<float>(image_roi.y);
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
      }

      OcrLine line;
      line.box.class_id = 0;
      line.box.score = rec_score;
      line.box.x1 = std::max(0.0f, min_x);
      line.box.y1 = std::max(0.0f, min_y);
      line.box.x2 = std::max(line.box.x1 + 1.0f, max_x);
      line.box.y2 = std::max(line.box.y1 + 1.0f, max_y);
      line.text = text;
      line.score = rec_score;
      lines->push_back(std::move(line));
    }

    if (!det_debug_printed_) {
      std::cout << "pp_ocr det debug: accepted_contours=" << accepted_contours
                << " accepted_texts=" << accepted_crops
                << " final_lines=" << lines->size() << "\n";
      det_debug_printed_ = true;
    }

    std::sort(lines->begin(), lines->end(),
              [](const OcrLine &lhs, const OcrLine &rhs) {
                if (std::fabs(lhs.box.y1 - rhs.box.y1) > 8.0f) {
                  return lhs.box.y1 < rhs.box.y1;
                }
                return lhs.box.x1 < rhs.box.x1;
              });
    return true;
  }

  bool runRecognition(const cv::Mat &crop, std::string *text, float *score,
                      std::string *error) {
    const int target_h = rec_session_.inputHeight();
    const int target_w = rec_session_.inputWidth();
    if (target_h <= 0 || target_w <= 0) {
      bmrt_runtime::setError(error, "invalid pp_ocr rec input size");
      return false;
    }

    const float aspect = static_cast<float>(crop.cols) /
                         std::max(1, crop.rows);
    int resized_w = static_cast<int>(std::round(target_h * aspect));
    resized_w = std::max(1, std::min(target_w, resized_w));

    cv::Mat resized;
    cv::resize(crop, resized, cv::Size(resized_w, target_h), 0, 0,
               cv::INTER_LINEAR);
    cv::Mat canvas(target_h, target_w, CV_8UC3, cv::Scalar(0, 0, 0));
    resized.copyTo(canvas(cv::Rect(0, 0, resized_w, target_h)));

    std::vector<float> input_tensor;
    bmrt_runtime::writeImageToTensor(
        canvas, bmrt_runtime::wantsRgbInput(descriptor_, false),
        rec_session_.nchwLayout(), rec_mean_, rec_scale_, &input_tensor);
    compensateQuantizedInputScale(rec_session_, &input_tensor);

    std::vector<bmrt_runtime::OutputTensor> outputs;
    if (!rec_session_.launch(input_tensor, &outputs, error)) {
      return false;
    }
    if (outputs.empty()) {
      bmrt_runtime::setError(error, "pp_ocr rec produced no outputs");
      return false;
    }

    *text = greedyDecodePpOcr(outputs[0], labels_, score);
    if (!rec_debug_printed_) {
      std::cout << "pp_ocr rec debug: crop=" << crop.cols << "x" << crop.rows
                << " decoded=\"" << *text << "\" score="
                << (score ? *score : 0.0f) << "\n";
      rec_debug_printed_ = true;
    }
    return true;
  }

  std::string greedyDecodeLegacy(const bmrt_runtime::OutputTensor &tensor,
                                 const std::vector<std::string> &labels,
                                 float *score) const {
    if (score) {
      *score = 0.0f;
    }
    if (tensor.shape.num_dims < 2 || tensor.data.empty()) {
      return std::string();
    }

    const int time_steps = tensor.shape.dims[tensor.shape.num_dims - 1];
    int class_count = 1;
    for (int d = 1; d < tensor.shape.num_dims - 1; ++d) {
      class_count *= tensor.shape.dims[d];
    }
    if (time_steps <= 0 || class_count <= 0) {
      return std::string();
    }

    std::vector<int> index(static_cast<size_t>(time_steps), 0);
    std::vector<float> probs(static_cast<size_t>(time_steps), 0.0f);
    for (int t = 0; t < time_steps; ++t) {
      float max_value = tensor.data[static_cast<size_t>(t)];
      int max_index = 0;
      for (int c = 1; c < class_count; ++c) {
        const float value =
            tensor.data[static_cast<size_t>(t + c * time_steps)];
        if (value > max_value) {
          max_value = value;
          max_index = c;
        }
      }
      index[static_cast<size_t>(t)] = max_index;
      probs[static_cast<size_t>(t)] = max_value;
    }

    std::vector<int> dedup;
    std::vector<float> dedup_scores;
    const int blank_index = static_cast<int>(labels.size()) - 1;
    int prev = blank_index;
    for (int t = 0; t < time_steps; ++t) {
      const int current = index[static_cast<size_t>(t)];
      if (current == blank_index) {
        prev = current;
        continue;
      }
      if (current == prev) {
        continue;
      }
      dedup.push_back(current);
      dedup_scores.push_back(probs[static_cast<size_t>(t)]);
      prev = current;
    }

    std::string decoded;
    float score_sum = 0.0f;
    int score_count = 0;
    for (size_t i = 0; i < dedup.size(); ++i) {
      const int value = dedup[i];
      if (value >= 0 && value < static_cast<int>(labels.size())) {
        decoded += labels[static_cast<size_t>(value)];
        score_sum += dedup_scores[i];
        ++score_count;
      }
    }
    if (score && score_count > 0) {
      *score = score_sum / static_cast<float>(score_count);
    }
    return decoded;
  }

  std::string greedyDecodePpOcr(const bmrt_runtime::OutputTensor &tensor,
                                const std::vector<std::string> &labels,
                                float *score) const {
    if (score) {
      *score = 0.0f;
    }
    if (tensor.shape.num_dims < 2 || tensor.data.empty()) {
      return std::string();
    }

    const int class_count = tensor.shape.dims[tensor.shape.num_dims - 1];
    int time_steps = 1;
    for (int d = 1; d < tensor.shape.num_dims - 1; ++d) {
      time_steps *= tensor.shape.dims[d];
    }
    if (class_count <= 1 || time_steps <= 0) {
      return std::string();
    }

    std::vector<int> index(static_cast<size_t>(time_steps), 0);
    std::vector<float> probs(static_cast<size_t>(time_steps), 0.0f);
    for (int t = 0; t < time_steps; ++t) {
      const size_t base = static_cast<size_t>(t * class_count);
      float max_value = tensor.data[base];
      int max_index = 0;
      for (int c = 1; c < class_count; ++c) {
        const float value = tensor.data[base + static_cast<size_t>(c)];
        if (value > max_value) {
          max_value = value;
          max_index = c;
        }
      }
      index[static_cast<size_t>(t)] = max_index;
      probs[static_cast<size_t>(t)] = max_value;
    }

    std::string decoded;
    float score_sum = 0.0f;
    int score_count = 0;
    int prev = 0;
    for (int t = 0; t < time_steps; ++t) {
      const int current = index[static_cast<size_t>(t)];
      if (current == 0) {
        prev = current;
        continue;
      }
      if (current == prev) {
        continue;
      }
      const int label_index = current - 1;
      if (label_index >= 0 && label_index < static_cast<int>(labels.size())) {
        decoded += labels[static_cast<size_t>(label_index)];
        score_sum += probs[static_cast<size_t>(t)];
        ++score_count;
      }
      prev = current;
    }
    if (score && score_count > 0) {
      *score = score_sum / static_cast<float>(score_count);
    }
    return decoded;
  }

  ModelDescriptor descriptor_;
  std::string runtime_name_;
  bool pp_ocr_mode_ = false;
  bool det_enabled_ = false;
  float det_binary_thresh_ = 0.3f;
  float det_box_thresh_ = 0.6f;
  float det_unclip_ratio_ = 1.5f;
  int det_min_size_ = 3;
  std::string label_path_;
  std::vector<std::string> labels_;
  bool det_debug_printed_ = false;
  bool rec_debug_printed_ = false;

  std::vector<float> single_mean_;
  std::vector<float> single_scale_;
  bmrt_runtime::Session single_session_;

  std::vector<float> det_mean_;
  std::vector<float> det_scale_;
  bmrt_runtime::Session det_session_;

  std::vector<float> rec_mean_;
  std::vector<float> rec_scale_;
  bmrt_runtime::Session rec_session_;
};

NnPlateRecognizer::NnPlateRecognizer(std::string model_type)
    : model_type_(std::move(model_type)) {}

NnPlateRecognizer::~NnPlateRecognizer() = default;

TaskType NnPlateRecognizer::task() const { return TaskType::Ocr; }

std::string NnPlateRecognizer::modelType() const { return model_type_; }

bool NnPlateRecognizer::loadDescriptor(std::string *error) {
  descriptor_loaded_ = false;
  descriptor_ = ModelDescriptor{};
  if (config_.model_descriptor_file.empty()) {
    bmrt_runtime::setError(
        error,
        "plate recognizer requires model_spec / model_descriptor_file");
    return false;
  }
  if (!loadModelDescriptor(config_.model_descriptor_file, &descriptor_, error)) {
    return false;
  }
  descriptor_loaded_ = true;
  return true;
}

bool NnPlateRecognizer::load(EngineConfig config, std::string *error) {
  config_ = std::move(config);
  if (!loadDescriptor(error)) {
    return false;
  }
  custom_runtime_.reset(new CustomRuntime());
  if (!custom_runtime_->open(config_, descriptor_, error)) {
    custom_runtime_.reset();
    return false;
  }
  initialized_ = true;
  return true;
}

bool NnPlateRecognizer::initialize(EngineConfig config, std::string *error) {
  return load(std::move(config), error);
}

bool NnPlateRecognizer::predict(const std::string &image_path,
                                const InferOptions &options,
                                AlgorithmResult *result, std::string *error) {
  Frame frame;
  frame.image_path = image_path;
  return predictFrame(frame, options, result, error);
}

bool NnPlateRecognizer::predictFrame(const Frame &frame,
                                     const InferOptions &options,
                                     AlgorithmResult *result,
                                     std::string *error) {
  (void)options;
  if (!custom_runtime_ || !initialized_) {
    bmrt_runtime::setError(error, "model is not initialized");
    return false;
  }
  if (!frame.image_path.empty()) {
    return custom_runtime_->inferImage(frame.image_path, nullptr, result,
                                       error);
  }
  cv::Mat image;
  if (!frame_convert::frameToBgrMat(frame, &image, error)) {
    return false;
  }
  return custom_runtime_->inferMat(image, nullptr, result, error);
}

bool NnPlateRecognizer::predictCrop(const std::string &image_path, const Box &roi,
                                    const InferOptions &options,
                                    AlgorithmResult *result,
                                    std::string *error) {
  (void)options;
  if (!custom_runtime_ || !initialized_) {
    bmrt_runtime::setError(error, "model is not initialized");
    return false;
  }
  return custom_runtime_->inferImage(image_path, &roi, result, error);
}

}  // namespace tdl_app
