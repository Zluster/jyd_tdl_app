#include "tdl_app/camera.hpp"

#include <memory>
#include <utility>

#include "tdl_app/frame_reader.hpp"
#include "tdl_app/image.hpp"

namespace tdl_app {
namespace {

FrameReader::Config toReaderConfig(const Camera::Config &config) {
  FrameReader::Config out;
  out.timeout_ms = config.timeout_ms;
  out.channel.module = config.backend == Camera::Backend::Vi
                           ? MediaModule::Vi
                           : MediaModule::Vpss;
  out.channel.device = config.backend == Camera::Backend::Vi
                           ? config.pipe
                           : config.group;
  out.channel.channel = config.channel;
  return out;
}

}  // namespace

Camera::Camera() = default;

Camera::Camera(const Config &config) : config_(config) {}

Camera::Camera(Camera &&other) noexcept = default;

Camera &Camera::operator=(Camera &&other) noexcept = default;

Camera::~Camera() = default;

bool Camera::open(std::string *error) {
  close();
  source_ = createSource();
  return source_->open(error);
}

bool Camera::open(CameraSourceId source, std::string *error) {
  config_ = forSource(source, config_.timeout_ms);
  return open(error);
}

bool Camera::read(Frame *frame, std::string *error) {
  if (!source_) {
    if (error) {
      *error = "camera is not opened";
    }
    return false;
  }
  return source_->read(frame, error);
}

bool Camera::readInfo(CameraFrameInfo *info, std::string *error) {
  if (!info) {
    if (error) {
      *error = "camera frame info is null";
    }
    return false;
  }
  Frame frame;
  if (!read(&frame, error)) {
    return false;
  }
  info->width = frame.width;
  info->height = frame.height;
  info->pixel_format = frame.format;
  info->sequence = frame.sequence;
  info->timestamp_us = frame.timestamp_us;
  return true;
}

bool Camera::snapshot(const std::string &path, std::string *error) {
  bool opened_here = false;
  if (!source_) {
    if (!open(error)) {
      return false;
    }
    opened_here = true;
  }

  Frame frame;
  const bool ok = read(&frame, error) && Image::save(frame, path, error);
  if (opened_here) {
    close();
  }
  return ok;
}

void Camera::close() {
  if (source_) {
    source_->close();
    source_.reset();
  }
}

Camera::Config Camera::forSource(CameraSourceId source, int timeout_ms) {
  switch (source) {
    case CameraSourceId::Ai:
      return ai(timeout_ms);
    case CameraSourceId::Live:
      return live(timeout_ms);
    case CameraSourceId::Main:
      return mainFrame(timeout_ms);
    case CameraSourceId::SubRgb:
      return subRgb(timeout_ms);
  }
  return live(timeout_ms);
}

const char *Camera::sourceName(CameraSourceId source) {
  switch (source) {
    case CameraSourceId::Ai:
      return "ai";
    case CameraSourceId::Live:
      return "live";
    case CameraSourceId::Main:
      return "main";
    case CameraSourceId::SubRgb:
      return "subrgb";
  }
  return "unknown";
}

std::unique_ptr<FrameSource> Camera::createSource() const {
  class ReaderSource final : public FrameSource {
   public:
    explicit ReaderSource(const FrameReader::Config &config)
        : reader_(config) {}

    bool open(std::string *error) override { return reader_.open(error); }
    bool read(Frame *frame, std::string *error) override {
      return reader_.read(frame, error);
    }
    void close() override { reader_.close(); }

   private:
    FrameReader reader_;
  };

  return std::unique_ptr<FrameSource>(new ReaderSource(toReaderConfig(config_)));
}

}  // namespace tdl_app
