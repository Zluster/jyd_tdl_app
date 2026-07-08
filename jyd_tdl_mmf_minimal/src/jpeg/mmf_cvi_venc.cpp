#include <algorithm>
#include <cstdlib>

#include "mmf_cv184x_common.hpp"

namespace mmf_cvi {
namespace {
constexpr int kFrameTimeoutMs = 20000;
constexpr int kMaxPicWidth = 2560;
constexpr int kMaxPicHeight = 1440;
constexpr int kBufSize = 1024 * 1024;
PAYLOAD_TYPE_E toPayload(VencChannel::Codec codec) {
  if (codec == VencChannel::Codec::H265)
    return PT_H265;
  if (codec == VencChannel::Codec::Mjpeg)
    return PT_MJPEG;
  return PT_H264;
}
}  // namespace
VencChannel::VencChannel() = default;
VencChannel::VencChannel(const Config& config) : config_(config) {}
VencChannel::~VencChannel() {
  close();
}
bool VencChannel::open(std::string* error) {
  if (opened_)
    return true;
  if (!ensureMmfRuntimeInitialized(error))
    return false;
  VENC_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.stVencAttr.u32PicWidth = config_.width;
  attr.stVencAttr.u32PicHeight = config_.height;
  attr.stVencAttr.u32MaxPicWidth = kMaxPicWidth;
  attr.stVencAttr.u32MaxPicHeight = kMaxPicHeight;
  attr.stVencAttr.u32BufSize = kBufSize;
  attr.stVencAttr.enType = toPayload(config_.codec);
  attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
  attr.stGopAttr.stNormalP.s32IPQpDelta = 2;
  if (attr.stVencAttr.enType == PT_MJPEG) {
    attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGFIXQP;
    attr.stRcAttr.stMjpegFixQp.u32SrcFrameRate = config_.src_fps;
    attr.stRcAttr.stMjpegFixQp.fr32DstFrameRate = config_.dst_fps;
    attr.stRcAttr.stMjpegFixQp.u32Qfactor = std::max(1, std::min(99, config_.qfactor));
  } else if (attr.stVencAttr.enType == PT_H265) {
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
    attr.stRcAttr.stH265Cbr.u32Gop = config_.gop;
    attr.stRcAttr.stH265Cbr.u32StatTime = 2;
    attr.stRcAttr.stH265Cbr.fr32DstFrameRate = config_.dst_fps;
    attr.stRcAttr.stH265Cbr.u32SrcFrameRate = config_.src_fps;
    attr.stRcAttr.stH265Cbr.u32BitRate = config_.bitrate_kbps;
  } else {
    attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    attr.stRcAttr.stH264Cbr.u32Gop = config_.gop;
    attr.stRcAttr.stH264Cbr.u32StatTime = 2;
    attr.stRcAttr.stH264Cbr.fr32DstFrameRate = config_.dst_fps;
    attr.stRcAttr.stH264Cbr.u32SrcFrameRate = config_.src_fps;
    attr.stRcAttr.stH264Cbr.u32BitRate = config_.bitrate_kbps;
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
bool VencChannel::encode(const Frame& frame, EncodedPacket* packet, std::string* error) {
  if (!opened_ && !open(error))
    return false;
  if (!packet || !frame.native) {
    setError(error, "venc encode requires native frame and packet");
    return false;
  }
  int ret = CVI_VENC_SendFrame(config_.channel, static_cast<VIDEO_FRAME_INFO_S*>(frame.native),
                               kFrameTimeoutMs);
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
  stream.pstPack = static_cast<VENC_PACK_S*>(std::malloc(sizeof(VENC_PACK_S) * status.u32CurPacks));
  if (!stream.pstPack) {
    setError(error, "malloc venc packs failed");
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
  packet->key_frame = true;
  for (CVI_U32 i = 0; i < stream.u32PackCount; ++i) {
    const VENC_PACK_S& pack = stream.pstPack[i];
    const uint8_t* begin = pack.pu8Addr + pack.u32Offset;
    const size_t size = pack.u32Len > pack.u32Offset ? pack.u32Len - pack.u32Offset : 0;
    packet->blocks.emplace_back(begin, begin + size);
  }
  CVI_VENC_ReleaseStream(config_.channel, &stream);
  std::free(stream.pstPack);
  return true;
}
void VencChannel::close() {
  if (!opened_)
    return;
  CVI_VENC_StopRecvFrame(config_.channel);
  CVI_VENC_DestroyChn(config_.channel);
  opened_ = false;
}
}  // namespace mmf_cvi
