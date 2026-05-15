#include "tdl_app/rtsp_streamer.hpp"

#include <memory>
#include <utility>

namespace tdl_app {
namespace {

RtspFrameSink::Config toSinkConfig(const RtspStreamer::Config &config) {
  RtspFrameSink::Config out;
  out.venc_channel = config.venc_channel;
  out.width = config.width;
  out.height = config.height;
  out.codec = config.codec;
  return out;
}

}  // namespace

RtspStreamer::RtspStreamer() = default;

RtspStreamer::RtspStreamer(const Config &config) : config_(config) {}

RtspStreamer::~RtspStreamer() = default;

bool RtspStreamer::open(std::string *error) {
  sink_ = createSink();
  return sink_->open(error);
}

bool RtspStreamer::write(const Frame &frame, const AlgorithmResult &result,
                         std::string *error) {
  if (!sink_) {
    if (error) {
      *error = "rtsp streamer is not opened";
    }
    return false;
  }
  return sink_->write(frame, result, error);
}

void RtspStreamer::close() {
  if (sink_) {
    sink_->close();
  }
}

std::unique_ptr<FrameSink> RtspStreamer::createSink() const {
  return std::unique_ptr<FrameSink>(new RtspFrameSink(toSinkConfig(config_)));
}

}  // namespace tdl_app
