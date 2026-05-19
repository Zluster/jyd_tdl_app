#include "tdl_app/frame_sink.hpp"

#include <cstring>
#include <memory>
#include <string>

#include "cvi_rtsp/rtsp.h"
#include "cvi_venc.h"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/venc_channel.hpp"

namespace tdl_app {
namespace {

constexpr int kRtspPort = 554;

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

CVI_RTSP_VIDEO_CODEC toRtspCodec(RtspFrameSink::Codec codec) {
  return codec == RtspFrameSink::Codec::H265 ? RTSP_VIDEO_H265 : RTSP_VIDEO_H264;
}

VencChannel::Codec toVencCodec(RtspFrameSink::Codec codec) {
  return codec == RtspFrameSink::Codec::H265
             ? VencChannel::Codec::H265
             : VencChannel::Codec::H264;
}

}  // namespace

bool NullFrameSink::open(std::string *error) {
  (void)error;
  return true;
}

bool NullFrameSink::write(const Frame &frame, const AlgorithmResult &result,
                          std::string *error) {
  (void)frame;
  (void)result;
  (void)error;
  return true;
}

void NullFrameSink::close() {}

class RtspFrameSink::Impl {
 public:
  explicit Impl(Config config)
      : config_(config), venc_(makeVencConfig(config_)) {}

  ~Impl() { close(); }

  bool open(std::string *error) {
    if (opened_) {
      return true;
    }
    if (config_.width <= 0 || config_.height <= 0) {
      setError(error, "rtsp sink width/height must be set");
      return false;
    }
    if (!venc_.open(error)) {
      return false;
    }
    if (!initRtsp(error)) {
      venc_.close();
      return false;
    }
    opened_ = true;
    return true;
  }

  bool write(const Frame &frame, const AlgorithmResult &result,
             std::string *error) {
    (void)result;
    if (!opened_ && !open(error)) {
      return false;
    }
    if (!frame.native) {
      setError(error, "rtsp sink requires native VIDEO_FRAME_INFO_S frame");
      return false;
    }

    VencChannel::EncodedPacket packet;
    if (!venc_.encode(frame, &packet, error)) {
      return false;
    }
    if (packet.blocks.empty()) {
      return true;
    }

    CVI_RTSP_DATA data;
    std::memset(&data, 0, sizeof(data));
    data.blockCnt = static_cast<int>(packet.blocks.size());
    for (int i = 0; i < data.blockCnt; ++i) {
      data.dataPtr[i] = packet.blocks[static_cast<std::size_t>(i)].data();
      data.dataLen[i] =
          static_cast<unsigned int>(packet.blocks[static_cast<std::size_t>(i)].size());
    }

    const int ret = CVI_RTSP_WriteFrame(ctx_, session_->video, &data);
    if (ret != 0) {
      setError(error, "CVI_RTSP_WriteFrame failed, ret=" + std::to_string(ret));
      return false;
    }
    return true;
  }

  void close() {
    if (ctx_) {
      CVI_RTSP_Stop(ctx_);
      if (session_) {
        CVI_RTSP_DestroySession(ctx_, session_);
        session_ = nullptr;
      }
      CVI_RTSP_Destroy(&ctx_);
      ctx_ = nullptr;
    }
    venc_.close();
    opened_ = false;
  }

 private:
  static VencChannel::Config makeVencConfig(const Config &config) {
    VencChannel::Config out;
    out.channel = config.venc_channel;
    out.width = config.width;
    out.height = config.height;
    out.codec = toVencCodec(config.codec);
    return out;
  }

  bool initRtsp(std::string *error) {
    CVI_RTSP_CONFIG cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.port = kRtspPort;
    int ret = CVI_RTSP_Create(&ctx_, &cfg);
    if (ret != 0) {
      setError(error, "CVI_RTSP_Create failed, ret=" + std::to_string(ret));
      return false;
    }

    CVI_RTSP_SESSION_ATTR attr;
    std::memset(&attr, 0, sizeof(attr));
    std::snprintf(attr.name, sizeof(attr.name), "%s",
                  config_.codec == Codec::H265 ? "h265" : "h264");
    attr.video.codec = toRtspCodec(config_.codec);
    ret = CVI_RTSP_CreateSession(ctx_, &attr, &session_);
    if (ret != 0) {
      setError(error, "CVI_RTSP_CreateSession failed, ret=" + std::to_string(ret));
      return false;
    }
    ret = CVI_RTSP_Start(ctx_);
    if (ret != 0) {
      setError(error, "CVI_RTSP_Start failed, ret=" + std::to_string(ret));
      return false;
    }
    return true;
  }

  Config config_;
  VencChannel venc_;
  CVI_RTSP_CTX *ctx_ = nullptr;
  CVI_RTSP_SESSION *session_ = nullptr;
  bool opened_ = false;
};

RtspFrameSink::RtspFrameSink() : RtspFrameSink(Config{}) {}

RtspFrameSink::RtspFrameSink(const Config &config)
    : config_(config), impl_(new Impl(config_)) {}

RtspFrameSink::~RtspFrameSink() = default;

bool RtspFrameSink::open(std::string *error) { return impl_->open(error); }

bool RtspFrameSink::write(const Frame &frame, const AlgorithmResult &result,
                          std::string *error) {
  return impl_->write(frame, result, error);
}

void RtspFrameSink::close() { impl_->close(); }

}  // namespace tdl_app
