#include "demo_support.hpp"

#include <iostream>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace demo_support {
namespace {

std::string labelForBox(const tdl_app::AlgorithmResult &result,
                        const tdl_app::Box &box) {
  if (box.class_id >= 0 &&
      box.class_id < static_cast<int>(result.labels.size())) {
    return result.labels[box.class_id];
  }
  return std::to_string(box.class_id);
}

}  // namespace

void printLabels(const tdl_app::AlgorithmResult &result) {
  if (result.labels.empty()) {
    return;
  }
  std::cout << "labels:";
  for (const auto &label : result.labels) {
    std::cout << " " << label;
  }
  std::cout << "\n";
}

void dumpResult(const tdl_app::AlgorithmResult &result) {
  std::cout << "classes: " << result.classes.size() << "\n";
  for (size_t i = 0; i < result.classes.size(); ++i) {
    std::cout << "  [" << i << "] id=" << result.classes[i].class_id
              << " score=" << result.classes[i].score;
    if (result.classes[i].class_id >= 0 &&
        result.classes[i].class_id < static_cast<int>(result.labels.size())) {
      std::cout << " label=" << result.labels[result.classes[i].class_id];
    }
    std::cout << "\n";
  }

  std::cout << "boxes: " << result.boxes.size() << "\n";
  for (size_t i = 0; i < result.boxes.size(); ++i) {
    const auto &b = result.boxes[i];
    std::cout << "  [" << i << "] id=" << b.class_id << " score=" << b.score
              << " box=(" << b.x1 << "," << b.y1 << "," << b.x2 << ","
              << b.y2 << ") landmarks=" << b.landmarks.size() << "\n";
  }

  std::cout << "points: " << result.points.size() << "\n";
  std::cout << "attributes: " << result.attributes.size() << "\n";
  for (const auto &attr : result.attributes) {
    std::cout << "  " << attr.name << "=" << attr.value << "\n";
  }
  if (!result.text.empty()) {
    std::cout << "text: " << result.text << "\n";
  }
  if (!result.feature.empty()) {
    std::cout << "feature elements: " << result.feature.size() << "\n";
  }
}

bool saveAnnotatedImage(const std::string &image_path,
                        const std::string &output_path,
                        const tdl_app::AlgorithmResult &result,
                        std::string *error) {
  cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) {
      *error = "failed to read image for annotation: " + image_path;
    }
    return false;
  }

  for (const auto &box : result.boxes) {
    const cv::Point p1(static_cast<int>(box.x1), static_cast<int>(box.y1));
    const cv::Point p2(static_cast<int>(box.x2), static_cast<int>(box.y2));
    cv::rectangle(image, p1, p2, cv::Scalar(0, 255, 0), 2);

    const std::string text =
        labelForBox(result, box) + ":" + cv::format("%.2f", box.score);
    int baseline = 0;
    const cv::Size text_size =
        cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
    const int text_top = std::max(0, p1.y - text_size.height - 6);
    cv::rectangle(image, cv::Point(p1.x, text_top),
                  cv::Point(p1.x + text_size.width + 6,
                            text_top + text_size.height + baseline + 6),
                  cv::Scalar(0, 255, 0), cv::FILLED);
    cv::putText(image, text,
                cv::Point(p1.x + 3, text_top + text_size.height + 1),
                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0), 1);

    for (const auto &landmark : box.landmarks) {
      cv::circle(image,
                 cv::Point(static_cast<int>(landmark.x),
                           static_cast<int>(landmark.y)),
                 2, cv::Scalar(255, 0, 0), cv::FILLED);
    }
  }

  for (const auto &point : result.points) {
    cv::circle(image,
               cv::Point(static_cast<int>(point.x), static_cast<int>(point.y)),
               3, cv::Scalar(0, 0, 255), cv::FILLED);
  }

  if (!result.text.empty()) {
    cv::putText(image, result.text, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX,
                0.8, cv::Scalar(0, 255, 255), 2);
  }

  if (result.boxes.empty() && !result.classes.empty()) {
    const auto &top1 = result.classes.front();
    std::string text =
        std::to_string(top1.class_id) + ":" + cv::format("%.2f", top1.score);
    if (top1.class_id >= 0 &&
        top1.class_id < static_cast<int>(result.labels.size())) {
      text = result.labels[top1.class_id] + ":" + cv::format("%.2f", top1.score);
    }
    cv::putText(image, text, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.8,
                cv::Scalar(0, 255, 255), 2);
  }

  if (!cv::imwrite(output_path, image)) {
    if (error) {
      *error = "failed to write output image: " + output_path;
    }
    return false;
  }
  return true;
}

}  // namespace demo_support
