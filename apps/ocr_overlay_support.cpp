#include "ocr_overlay_support.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace ocr_overlay_support {
namespace {

std::vector<std::uint32_t> decodeUtf8(const std::string &text) {
  std::vector<std::uint32_t> codepoints;
  for (size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index]);
    std::uint32_t value = 0;
    size_t count = 0;
    if ((first & 0x80) == 0) {
      value = first;
      count = 1;
    } else if ((first & 0xe0) == 0xc0) {
      value = first & 0x1f;
      count = 2;
    } else if ((first & 0xf0) == 0xe0) {
      value = first & 0x0f;
      count = 3;
    } else if ((first & 0xf8) == 0xf0) {
      value = first & 0x07;
      count = 4;
    } else {
      codepoints.push_back(0xfffd);
      ++index;
      continue;
    }
    if (index + count > text.size()) {
      codepoints.push_back(0xfffd);
      break;
    }
    bool valid = true;
    for (size_t offset = 1; offset < count; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + offset]);
      if ((next & 0xc0) != 0x80) {
        valid = false;
        break;
      }
      value = (value << 6) | (next & 0x3f);
    }
    codepoints.push_back(valid ? value : 0xfffd);
    index += valid ? count : 1;
  }
  return codepoints;
}

class UnicodeRenderer {
 public:
  ~UnicodeRenderer() {
    if (face_) FT_Done_Face(face_);
    if (library_) FT_Done_FreeType(library_);
  }

  bool open(const std::string &font_path, int pixel_height,
            std::string *error) {
    if (FT_Init_FreeType(&library_) != 0) {
      if (error) *error = "FT_Init_FreeType failed";
      return false;
    }
    if (FT_New_Face(library_, font_path.c_str(), 0, &face_) != 0) {
      if (error) *error = "failed to load OCR font: " + font_path;
      return false;
    }
    if (FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(pixel_height)) != 0) {
      if (error) *error = "FT_Set_Pixel_Sizes failed";
      return false;
    }
    return true;
  }

  void draw(cv::Mat *image, const std::string &text, int x, int baseline,
            const cv::Scalar &color) {
    if (!image || image->empty() || !face_) return;
    int pen_x = x;
    for (std::uint32_t codepoint : decodeUtf8(text)) {
      if (codepoint == '\n') {
        pen_x = x;
        baseline += static_cast<int>(face_->size->metrics.height >> 6);
        continue;
      }
      if (FT_Load_Char(face_, codepoint, FT_LOAD_RENDER) != 0) continue;
      const FT_GlyphSlot glyph = face_->glyph;
      blendBitmap(image, glyph->bitmap, pen_x + glyph->bitmap_left,
                  baseline - glyph->bitmap_top, color);
      pen_x += static_cast<int>(glyph->advance.x >> 6);
      if (pen_x >= image->cols - 2) break;
    }
  }

 private:
  static void blendBitmap(cv::Mat *image, const FT_Bitmap &bitmap, int left,
                          int top, const cv::Scalar &color) {
    const int pitch = bitmap.pitch;
    for (int row = 0; row < static_cast<int>(bitmap.rows); ++row) {
      const int image_y = top + row;
      if (image_y < 0 || image_y >= image->rows) continue;
      const unsigned char *source = pitch >= 0
          ? bitmap.buffer + row * pitch
          : bitmap.buffer + (static_cast<int>(bitmap.rows) - 1 - row) *
                                (-pitch);
      for (int column = 0; column < static_cast<int>(bitmap.width); ++column) {
        const int image_x = left + column;
        if (image_x < 0 || image_x >= image->cols) continue;
        const float alpha = source[column] / 255.0f;
        cv::Vec3b &pixel = image->at<cv::Vec3b>(image_y, image_x);
        for (int channel = 0; channel < 3; ++channel) {
          pixel[channel] = static_cast<unsigned char>(
              pixel[channel] * (1.0f - alpha) + color[channel] * alpha);
        }
      }
    }
  }

  FT_Library library_ = nullptr;
  FT_Face face_ = nullptr;
};

std::string lineText(const tdl_app::AlgorithmResult &result, size_t index) {
  if (index >= result.attributes.size()) return std::string();
  const std::string prefix = "ocr_text:";
  const std::string &name = result.attributes[index].name;
  return name.compare(0, prefix.size(), prefix) == 0
             ? name.substr(prefix.size())
             : name;
}

}  // namespace

bool saveAnnotatedImage(const std::string &input, const std::string &output,
                        const tdl_app::AlgorithmResult &result,
                        const std::string &font_path, std::string *error) {
  cv::Mat image = cv::imread(input, cv::IMREAD_COLOR);
  if (image.empty()) {
    if (error) *error = "failed to read OCR image: " + input;
    return false;
  }
  UnicodeRenderer renderer;
  if (!renderer.open(font_path, std::max(24, image.rows / 28), error)) {
    return false;
  }
  for (size_t index = 0; index < result.boxes.size(); ++index) {
    const auto &box = result.boxes[index];
    std::vector<cv::Point> polygon;
    for (const auto &point : box.landmarks) {
      polygon.emplace_back(static_cast<int>(point.x),
                           static_cast<int>(point.y));
    }
    if (polygon.size() == 4) {
      cv::polylines(image, std::vector<std::vector<cv::Point>>{polygon}, true,
                    cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    } else {
      cv::rectangle(image,
                    cv::Point(static_cast<int>(box.x1), static_cast<int>(box.y1)),
                    cv::Point(static_cast<int>(box.x2), static_cast<int>(box.y2)),
                    cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    }
    const std::string text = "OCR#" + std::to_string(index) + " " +
                             lineText(result, index);
    const int x = std::max(2, static_cast<int>(box.x1));
    int baseline = static_cast<int>(box.y1) - 6;
    if (baseline < 30) baseline = std::min(image.rows - 2,
                                          static_cast<int>(box.y2) + 36);
    renderer.draw(&image, text, x + 2, baseline + 2, cv::Scalar(0, 0, 0));
    renderer.draw(&image, text, x, baseline, cv::Scalar(0, 255, 255));
  }
  if (!cv::imwrite(output, image)) {
    if (error) *error = "failed to write OCR overlay: " + output;
    return false;
  }
  return true;
}

}  // namespace ocr_overlay_support
