#include "tdl_app/camera.hpp"

#include <memory>
#include <utility>

#include "tdl_app/frame_reader.hpp"

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
  source_ = createSource();
  return source_->open(error);
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

void Camera::close() {
  if (source_) {
    source_->close();
  }
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
