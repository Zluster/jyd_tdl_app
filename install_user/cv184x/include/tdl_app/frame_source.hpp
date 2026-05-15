#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace tdl_app {

struct Frame {
  std::string image_path;
  void *native = nullptr;
  int width = 0;
  int height = 0;
  int format = 0;
  uint64_t sequence = 0;
  uint64_t timestamp_us = 0;
};

class FrameSource {
 public:
  virtual ~FrameSource() = default;
  virtual bool open(std::string *error = nullptr) = 0;
  virtual bool read(Frame *frame, std::string *error = nullptr) = 0;
  virtual void close() = 0;
};

class ImageFileSource final : public FrameSource {
 public:
  explicit ImageFileSource(std::string path);
  bool open(std::string *error = nullptr) override;
  bool read(Frame *frame, std::string *error = nullptr) override;
  void close() override;

 private:
  std::string path_;
  bool consumed_ = false;
};

class CameraSource final : public FrameSource {
 public:
  enum class Backend {
    Vi,
    Vpss,
  };

  struct Config {
    Backend backend = Backend::Vpss;
    int device = 0;
    int group = 0;
    int pipe = 0;
    int channel = 0;
    int width = 0;
    int height = 0;
    int pixel_format = 0;
    int timeout_ms = 1000;
  };

  CameraSource();
  explicit CameraSource(const Config &config);
  ~CameraSource() override;
  bool open(std::string *error = nullptr) override;
  bool read(Frame *frame, std::string *error = nullptr) override;
  void close() override;

 private:
  class Impl;
  Config config_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tdl_app
