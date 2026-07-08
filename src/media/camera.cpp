#include "tdl_app/camera.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <utility>

#include "cvi_comm_video.h"
#include "cvi_sys.h"
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

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

void fillInfo(const Frame &frame, CameraFrameInfo *info) {
  if (!info) {
    return;
  }
  info->width = frame.width;
  info->height = frame.height;
  info->pixel_format = frame.format;
  info->sequence = frame.sequence;
  info->timestamp_us = frame.timestamp_us;
  info->stride[0] = 0;
  info->stride[1] = 0;
  info->stride[2] = 0;
  if (!frame.native) {
    return;
  }
  const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
  for (int i = 0; i < 3; ++i) {
    info->stride[i] = static_cast<int>(video->stVFrame.u32Stride[i]);
  }
}

bool planeGeometry(const VIDEO_FRAME_S &vf, int plane, int *rows,
                   int *bytes_per_row) {
  if (!rows || !bytes_per_row) {
    return false;
  }
  *rows = 0;
  *bytes_per_row = 0;
  const int width = static_cast<int>(vf.u32Width);
  const int height = static_cast<int>(vf.u32Height);
  const int format = static_cast<int>(vf.enPixelFormat);
  switch (format) {
    case PIXEL_FORMAT_RGB_888:
    case PIXEL_FORMAT_BGR_888:
      if (plane == 0) {
        *rows = height;
        *bytes_per_row = width * 3;
      }
      return true;
    case PIXEL_FORMAT_RGB_888_PLANAR:
    case PIXEL_FORMAT_BGR_888_PLANAR:
      if (plane >= 0 && plane < 3) {
        *rows = height;
        *bytes_per_row = width;
      }
      return true;
    case PIXEL_FORMAT_YUV_400:
      if (plane == 0) {
        *rows = height;
        *bytes_per_row = width;
      }
      return true;
    case PIXEL_FORMAT_NV12:
    case PIXEL_FORMAT_NV21:
      if (plane == 0) {
        *rows = height;
        *bytes_per_row = width;
        return true;
      }
      if (plane == 1) {
        *rows = height / 2;
        *bytes_per_row = width;
        return true;
      }
      return true;
    default:
      if (vf.u32Stride[plane] > 0 && vf.u32Length[plane] > 0) {
        *rows = static_cast<int>(vf.u32Length[plane] / vf.u32Stride[plane]);
        *bytes_per_row = static_cast<int>(vf.u32Stride[plane]);
      }
      return true;
  }
}

bool writePlaneRows(const VIDEO_FRAME_S &vf, int plane, std::ofstream *out,
                    std::size_t *bytes_written, std::string *error) {
  if (!out || !(*out)) {
    setError(error, "output file is not open");
    return false;
  }
  if (vf.u64PhyAddr[plane] == 0 || vf.u32Length[plane] == 0) {
    return true;
  }

  void *mapped = CVI_SYS_Mmap(vf.u64PhyAddr[plane], vf.u32Length[plane]);
  if (!mapped) {
    setError(error, "CVI_SYS_Mmap failed for plane " + std::to_string(plane));
    return false;
  }
  CVI_SYS_IonInvalidateCache(vf.u64PhyAddr[plane], mapped, vf.u32Length[plane]);

  int rows = 0;
  int bytes_per_row = 0;
  planeGeometry(vf, plane, &rows, &bytes_per_row);
  const std::size_t stride = static_cast<std::size_t>(vf.u32Stride[plane]);
  const std::size_t length = static_cast<std::size_t>(vf.u32Length[plane]);
  const auto *base = static_cast<const std::uint8_t *>(mapped);

  bool ok = true;
  if (rows <= 0 || bytes_per_row <= 0 || stride == 0) {
    out->write(reinterpret_cast<const char *>(base),
               static_cast<std::streamsize>(length));
    ok = out->good();
    if (ok && bytes_written) {
      *bytes_written += length;
    }
  } else {
    for (int row = 0; row < rows; ++row) {
      const std::size_t row_offset = stride * static_cast<std::size_t>(row);
      if (row_offset >= length) {
        break;
      }
      const std::size_t available = length - row_offset;
      const std::size_t copy_bytes = std::min<std::size_t>(
          available, static_cast<std::size_t>(bytes_per_row));
      out->write(reinterpret_cast<const char *>(base + row_offset),
                 static_cast<std::streamsize>(copy_bytes));
      if (!out->good()) {
        ok = false;
        break;
      }
      if (bytes_written) {
        *bytes_written += copy_bytes;
      }
    }
  }

  CVI_SYS_Munmap(mapped, vf.u32Length[plane]);
  if (!ok) {
    setError(error, "failed to write frame plane data");
  }
  return ok;
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
    setError(error, "camera is not opened");
    return false;
  }
  return source_->read(frame, error);
}

void Camera::releaseFrame() {
  if (source_) {
    source_->releaseFrame();
  }
}

bool Camera::readInfo(CameraFrameInfo *info, std::string *error) {
  if (!info) {
    setError(error, "camera frame info is null");
    return false;
  }
  Frame frame;
  if (!read(&frame, error)) {
    return false;
  }
  fillInfo(frame, info);
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

bool Camera::dumpFrame(const std::string &path, CameraFrameInfo *info,
                       std::size_t *bytes_written, std::string *error) {
  bool opened_here = false;
  if (!source_) {
    if (!open(error)) {
      return false;
    }
    opened_here = true;
  }

  Frame frame;
  if (!read(&frame, error)) {
    if (opened_here) {
      close();
    }
    return false;
  }
  fillInfo(frame, info);

  if (!frame.native) {
    if (opened_here) {
      close();
    }
    setError(error, "frame has no native VIDEO_FRAME_INFO_S buffer");
    return false;
  }

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    if (opened_here) {
      close();
    }
    setError(error, "failed to open dump output: " + path);
    return false;
  }

  if (bytes_written) {
    *bytes_written = 0;
  }
  const auto *video = static_cast<const VIDEO_FRAME_INFO_S *>(frame.native);
  bool ok = true;
  for (int plane = 0; plane < 3; ++plane) {
    if (!writePlaneRows(video->stVFrame, plane, &out, bytes_written, error)) {
      ok = false;
      break;
    }
  }

  if (opened_here) {
    close();
  }
  return ok;
}

bool Camera::dumpFrameBmp(const std::string &path, CameraFrameInfo *info,
                          std::string *error) {
  bool opened_here = false;
  if (!source_) {
    if (!open(error)) {
      return false;
    }
    opened_here = true;
  }

  Frame frame;
  const bool ok = read(&frame, error) && Image::save(frame, path, error);
  if (ok) {
    fillInfo(frame, info);
  }
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
    case CameraSourceId::Screen:
      return screen(timeout_ms);
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
    case CameraSourceId::Screen:
      return "screen";
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

