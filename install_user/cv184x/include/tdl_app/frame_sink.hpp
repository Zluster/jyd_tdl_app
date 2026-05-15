#pragma once

#include <memory>
#include <string>

namespace tdl_app {

struct Frame;
struct AlgorithmResult;

class FrameSink {
 public:
  virtual ~FrameSink() = default;
  virtual bool open(std::string *error = nullptr) = 0;
  virtual bool write(const Frame &frame, const AlgorithmResult &result,
                     std::string *error = nullptr) = 0;
  virtual void close() = 0;
};

class NullFrameSink final : public FrameSink {
 public:
  bool open(std::string *error = nullptr) override;
  bool write(const Frame &frame, const AlgorithmResult &result,
             std::string *error = nullptr) override;
  void close() override;
};

class RtspFrameSink final : public FrameSink {
 public:
  enum class Codec {
    H264,
    H265,
  };

  struct Config {
    int venc_channel = 0;
    int width = 0;
    int height = 0;
    Codec codec = Codec::H264;
  };

  static Config h264(int venc_channel = 0, int width = 1920,
                     int height = 1080) {
    Config config;
    config.venc_channel = venc_channel;
    config.width = width;
    config.height = height;
    config.codec = Codec::H264;
    return config;
  }

  static Config h265(int venc_channel = 0, int width = 1920,
                     int height = 1080) {
    Config config = h264(venc_channel, width, height);
    config.codec = Codec::H265;
    return config;
  }

  RtspFrameSink();
  explicit RtspFrameSink(const Config &config);
  ~RtspFrameSink() override;

  bool open(std::string *error = nullptr) override;
  bool write(const Frame &frame, const AlgorithmResult &result,
             std::string *error = nullptr) override;
  void close() override;

 private:
  class Impl;
  Config config_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tdl_app
