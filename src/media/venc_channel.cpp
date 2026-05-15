#include "tdl_app/venc_channel.hpp"

#include <cstdlib>
#include <cstring>

#include "cvi_type.h"
#include "cvi_venc.h"
#include "tdl_app/frame_source.hpp"

namespace tdl_app {
namespace {

constexpr int kFrameTimeoutMs = 20000;
constexpr int kMaxPicWidth = 2560;
constexpr int kMaxPicHeight = 1440;
constexpr int kBufSize = 1024 * 1024;

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

PAYLOAD_TYPE_E toPayload(VencChannel::Codec codec) {
  return codec == VencChannel::Codec::H265 ? PT_H265 : PT_H264;
}

class VencChannelImpl {
 public:
  explicit VencChannelImpl(const VencChannel::Config &config) : config_(config) {}

  bool open(std::string *error) {
    if (opened_) {
      return true;
    }
    if (config_.width <= 0 || config_.height <= 0) {
      setError(error, "venc width/height must be positive");
      return false;
    }

    VENC_CHN_ATTR_S attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.stVencAttr.u32PicWidth = static_cast<CVI_U32>(config_.width);
    attr.stVencAttr.u32PicHeight = static_cast<CVI_U32>(config_.height);
    attr.stVencAttr.u32MaxPicWidth = kMaxPicWidth;
    attr.stVencAttr.u32MaxPicHeight = kMaxPicHeight;
    attr.stVencAttr.u32BufSize = kBufSize;
    attr.stVencAttr.enType = toPayload(config_.codec);
    attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
    attr.stGopAttr.stNormalP.s32IPQpDelta = 2;

    if (attr.stVencAttr.enType == PT_H264) {
      attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
      attr.stRcAttr.stH264Cbr.u32Gop = static_cast<CVI_U32>(config_.gop);
      attr.stRcAttr.stH264Cbr.u32StatTime = 2;
      attr.stRcAttr.stH264Cbr.fr32DstFrameRate = config_.dst_fps;
      attr.stRcAttr.stH264Cbr.u32SrcFrameRate = static_cast<CVI_U32>(config_.src_fps);
      attr.stRcAttr.stH264Cbr.u32BitRate = static_cast<CVI_U32>(config_.bitrate_kbps);
    } else {
      attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
      attr.stRcAttr.stH265Cbr.u32Gop = static_cast<CVI_U32>(config_.gop);
      attr.stRcAttr.stH265Cbr.u32StatTime = 2;
      attr.stRcAttr.stH265Cbr.fr32DstFrameRate = config_.dst_fps;
      attr.stRcAttr.stH265Cbr.u32SrcFrameRate = static_cast<CVI_U32>(config_.src_fps);
      attr.stRcAttr.stH265Cbr.u32BitRate = static_cast<CVI_U32>(config_.bitrate_kbps);
    }

    int ret = CVI_VENC_CreateChn(config_.channel, &attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VENC_CreateChn failed, ret=" + std::to_string(ret));
      return false;
    }

    VENC_RECV_PIC_PARAM_S recv;
    std::memset(&recv, 0, sizeof(recv));
    recv.s32RecvPicNum = -1;
    ret = CVI_VENC_StartRecvFrame(config_.channel, &recv);
    if (ret != CVI_SUCCESS) {
      CVI_VENC_DestroyChn(config_.channel);
      setError(error, "CVI_VENC_StartRecvFrame failed, ret=" + std::to_string(ret));
      return false;
    }

    opened_ = true;
    return true;
  }

  bool encode(const Frame &frame, VencChannel::EncodedPacket *packet,
              std::string *error) {
    if (!opened_ && !open(error)) {
      return false;
    }
    if (!packet) {
      setError(error, "encoded packet pointer is null");
      return false;
    }
    if (!frame.native) {
      setError(error, "venc encode requires native VIDEO_FRAME_INFO_S frame");
      return false;
    }

    auto *video_frame = static_cast<VIDEO_FRAME_INFO_S *>(frame.native);
    int ret = CVI_VENC_SendFrame(config_.channel, video_frame, kFrameTimeoutMs);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VENC_SendFrame failed, ret=" + std::to_string(ret));
      return false;
    }

    VENC_CHN_STATUS_S status;
    std::memset(&status, 0, sizeof(status));
    ret = CVI_VENC_QueryStatus(config_.channel, &status);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VENC_QueryStatus failed, ret=" + std::to_string(ret));
      return false;
    }
    if (status.u32CurPacks == 0) {
      packet->blocks.clear();
      return true;
    }

    VENC_STREAM_S stream;
    std::memset(&stream, 0, sizeof(stream));
    stream.pstPack = static_cast<VENC_PACK_S *>(
        std::malloc(sizeof(VENC_PACK_S) * status.u32CurPacks));
    if (!stream.pstPack) {
      setError(error, "failed to allocate venc pack buffer");
      return false;
    }

    ret = CVI_VENC_GetStream(config_.channel, &stream, kFrameTimeoutMs);
    if (ret != CVI_SUCCESS) {
      std::free(stream.pstPack);
      setError(error, "CVI_VENC_GetStream failed, ret=" + std::to_string(ret));
      return false;
    }

    packet->blocks.clear();
    packet->blocks.reserve(stream.u32PackCount);
    for (unsigned int i = 0; i < stream.u32PackCount; ++i) {
      const VENC_PACK_S &pack = stream.pstPack[i];
      const auto *begin = pack.pu8Addr + pack.u32Offset;
      const auto size = static_cast<std::size_t>(pack.u32Len - pack.u32Offset);
      packet->blocks.emplace_back(begin, begin + size);
    }

    CVI_VENC_ReleaseStream(config_.channel, &stream);
    std::free(stream.pstPack);
    return true;
  }

  void close() {
    if (!opened_) {
      return;
    }
    CVI_VENC_StopRecvFrame(config_.channel);
    CVI_VENC_DestroyChn(config_.channel);
    opened_ = false;
  }

  bool isOpen() const { return opened_; }

 private:
  VencChannel::Config config_;
  bool opened_ = false;
};

}  // namespace

class VencChannel::Impl : public VencChannelImpl {
 public:
  explicit Impl(const VencChannel::Config &config) : VencChannelImpl(config) {}
};

VencChannel::VencChannel() : VencChannel(Config{}) {}

VencChannel::VencChannel(const Config &config)
    : config_(config), impl_(new Impl(config_)) {}

VencChannel::~VencChannel() {
  delete impl_;
  impl_ = nullptr;
}

bool VencChannel::open(std::string *error) { return impl_->open(error); }

bool VencChannel::encode(const Frame &frame, EncodedPacket *packet,
                         std::string *error) {
  return impl_->encode(frame, packet, error);
}

void VencChannel::close() { impl_->close(); }

bool VencChannel::isOpen() const { return impl_->isOpen(); }

}  // namespace tdl_app
