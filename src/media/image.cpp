#include "tdl_app/image.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "cvi_comm_video.h"
#include "cvi_sys.h"
#include "tdl_app/graphic_vo_layer.hpp"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

bool frameToBgr(const Frame &frame, cv::Mat *image, std::string *error) {
  if (!image) {
    setError(error, "image output is null");
    return false;
  }
  if (!frame.native) {
    setError(error, "frame has no native buffer");
    return false;
  }

  auto *video = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
  const auto &vf = video->stVFrame;
  const int width = static_cast<int>(vf.u32Width);
  const int height = static_cast<int>(vf.u32Height);
  const int format = static_cast<int>(vf.enPixelFormat);

  if (width <= 0 || height <= 0) {
    setError(error, "invalid frame size");
    return false;
  }

  if (format != PIXEL_FORMAT_NV21 &&
      format != PIXEL_FORMAT_NV12 &&
      format != PIXEL_FORMAT_YUV_400 &&
      format != PIXEL_FORMAT_RGB_888 &&
      format != PIXEL_FORMAT_BGR_888 &&
      format != PIXEL_FORMAT_RGB_888_PLANAR &&
      format != PIXEL_FORMAT_BGR_888_PLANAR) {
    setError(error,
             "image save only supports NV21/NV12/YUV400/RGB888/BGR888/"
             "RGB888_PLANAR/BGR888_PLANAR, format=" +
                 std::to_string(format));
    return false;
  }

  std::size_t map_size = 0;
  for (int i = 0; i < 3; ++i) {
    map_size += vf.u32Length[i];
  }
  if (map_size == 0) {
    setError(error, "frame buffer length is zero");
    return false;
  }

  auto *mapped =
      static_cast<unsigned char *>(CVI_SYS_Mmap(vf.u64PhyAddr[0], map_size));
  if (!mapped) {
    setError(error, "CVI_SYS_Mmap failed");
    return false;
  }
  CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[0], mapped, map_size);

  bool ok = true;
  do {
    if (format == PIXEL_FORMAT_RGB_888 || format == PIXEL_FORMAT_BGR_888) {
      cv::Mat packed(height, width, CV_8UC3);
      for (int y = 0; y < height; ++y) {
        std::memcpy(packed.ptr(y), mapped + y * vf.u32Stride[0], width * 3);
      }
      if (format == PIXEL_FORMAT_RGB_888) {
        cv::cvtColor(packed, *image, cv::COLOR_RGB2BGR);
      } else {
        *image = packed.clone();
      }
      break;
    }

    if (format == PIXEL_FORMAT_RGB_888_PLANAR ||
        format == PIXEL_FORMAT_BGR_888_PLANAR) {
      cv::Mat plane0(height, width, CV_8UC1);
      cv::Mat plane1(height, width, CV_8UC1);
      cv::Mat plane2(height, width, CV_8UC1);
      unsigned char *base0 = mapped;
      unsigned char *base1 = mapped + vf.u32Length[0];
      unsigned char *base2 = mapped + vf.u32Length[0] + vf.u32Length[1];
      for (int y = 0; y < height; ++y) {
        std::memcpy(plane0.ptr(y), base0 + y * vf.u32Stride[0], width);
        std::memcpy(plane1.ptr(y), base1 + y * vf.u32Stride[1], width);
        std::memcpy(plane2.ptr(y), base2 + y * vf.u32Stride[2], width);
      }
      std::vector<cv::Mat> channels;
      if (format == PIXEL_FORMAT_RGB_888_PLANAR) {
        channels = {plane2, plane1, plane0};
      } else {
        channels = {plane0, plane1, plane2};
      }
      cv::merge(channels, *image);
      break;
    }

    if (format == PIXEL_FORMAT_YUV_400) {
      cv::Mat gray(height, width, CV_8UC1);
      for (int y = 0; y < height; ++y) {
        std::memcpy(gray.ptr(y), mapped + y * vf.u32Stride[0], width);
      }
      cv::cvtColor(gray, *image, cv::COLOR_GRAY2BGR);
      break;
    }

    cv::Mat yuv(height + height / 2, width, CV_8UC1);
    unsigned char *y_base = mapped;
    unsigned char *uv_base = mapped + vf.u32Length[0];
    for (int y = 0; y < height; ++y) {
      std::memcpy(yuv.ptr(y), y_base + y * vf.u32Stride[0], width);
    }
    for (int y = 0; y < height / 2; ++y) {
      std::memcpy(yuv.ptr(height + y), uv_base + y * vf.u32Stride[1], width);
    }

    const int code = format == PIXEL_FORMAT_NV21
                         ? cv::COLOR_YUV2BGR_NV21
                         : cv::COLOR_YUV2BGR_NV12;
    cv::cvtColor(yuv, *image, code);
  } while (false);

  CVI_SYS_Munmap(mapped, map_size);
  return ok;
}

GraphicVoLayer::Config toLayerConfig(const Image::Config &config) {
  GraphicVoLayer::Config out =
      GraphicVoLayer::argb8888(config.layer, config.screen_width,
                               config.screen_height, config.device);
  out.screen_width = config.screen_width;
  out.screen_height = config.screen_height;
  out.display_width = config.screen_width;
  out.display_height = config.screen_height;
  out.show = false;
  out.double_buffer = config.double_buffer;
  return out;
}

}  // namespace

Image::Image() = default;

Image::Image(const Config &config) : config_(config) {}

Image::~Image() { close(); }

Image::Image(Image &&other) noexcept = default;

Image &Image::operator=(Image &&other) noexcept = default;

bool Image::open(std::string *error) {
  if (layer_ && layer_->isOpen()) {
    return true;
  }
  layer_.reset(new GraphicVoLayer(toLayerConfig(config_)));
  if (!layer_->open(error)) {
    layer_.reset();
    return false;
  }
  if (!layer_->clear(config_.clear_color, error)) {
    close();
    return false;
  }
  if (!layer_->present(error)) {
    close();
    return false;
  }
  if (!layer_->setVisible(false, error)) {
    close();
    return false;
  }
  return true;
}

void Image::close() {
  if (layer_) {
    layer_->close();
    layer_.reset();
  }
}

bool Image::isOpen() const { return layer_ && layer_->isOpen(); }

bool Image::show(const std::string &path, std::string *error) {
  return show(path, 0, 0, config_.screen_width, config_.screen_height, error);
}

bool Image::show(const std::string &path, int x, int y, int width, int height,
                 std::string *error) {
  if (!open(error)) {
    return false;
  }

  cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
  if (image.empty()) {
    setError(error, "failed to load image: " + path);
    return false;
  }

  GraphicVoLayer::BufferView view = layer_->buffer();
  if (!view.data || view.bytes_per_pixel != 4) {
    setError(error, "graphic layer buffer is unavailable");
    return false;
  }

  if (width <= 0) {
    width = view.width;
  }
  if (height <= 0) {
    height = view.height;
  }
  if (x < 0 || y < 0 || x + width > view.width || y + height > view.height) {
    setError(error, "image target rect is out of range");
    return false;
  }

  if (!layer_->clear(config_.clear_color, error)) {
    return false;
  }

  cv::Mat resized;
  cv::resize(image, resized, cv::Size(width, height), 0, 0, cv::INTER_LINEAR);
  cv::Mat bgra;
  cv::cvtColor(resized, bgra, cv::COLOR_BGR2BGRA);

  auto *base = static_cast<std::uint8_t *>(view.data);
  for (int row = 0; row < height; ++row) {
    std::memcpy(base + static_cast<std::size_t>(y + row) * view.stride +
                    static_cast<std::size_t>(x) * 4,
                bgra.ptr(row), static_cast<std::size_t>(width) * 4);
  }

  if (!layer_->setVisible(true, error)) {
    return false;
  }
  return layer_->present(error);
}

bool Image::clear(std::string *error) {
  if (!open(error)) {
    return false;
  }
  if (!layer_->clear(config_.clear_color, error)) {
    return false;
  }
  if (!layer_->present(error)) {
    return false;
  }
  return layer_->setVisible(false, error);
}

bool Image::setVisible(bool visible, std::string *error) {
  if (!open(error)) {
    return false;
  }
  return layer_->setVisible(visible, error);
}

bool Image::save(const Frame &frame, const std::string &path,
                 std::string *error) {
  cv::Mat image;
  if (!frameToBgr(frame, &image, error)) {
    return false;
  }
  if (!cv::imwrite(path, image)) {
    setError(error, "failed to write image: " + path);
    return false;
  }
  return true;
}

}  // namespace tdl_app
