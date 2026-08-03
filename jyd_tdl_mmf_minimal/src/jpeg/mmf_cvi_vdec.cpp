#include "mmf_cv184x_common.hpp"

namespace mmf_cvi {
namespace {
PAYLOAD_TYPE_E toPayload(VdecChannel::Codec codec) {
  if (codec == VdecChannel::Codec::H265)
    return PT_H265;
  if (codec == VdecChannel::Codec::Jpeg)
    return PT_JPEG;
  if (codec == VdecChannel::Codec::Mjpeg)
    return PT_MJPEG;
  return PT_H264;
}
VIDEO_MODE_E toMode(VdecChannel::Mode mode) {
  if (mode == VdecChannel::Mode::Frame)
    return VIDEO_MODE_FRAME;
  if (mode == VdecChannel::Mode::Compat)
    return VIDEO_MODE_COMPAT;
  return VIDEO_MODE_STREAM;
}
}  // namespace
VdecChannel::VdecChannel() = default;
VdecChannel::VdecChannel(const Config& config) : config_(config) {}
VdecChannel::~VdecChannel() {
  close();
}
bool VdecChannel::open(std::string* error) {
  if (opened_)
    return true;
  if (!ensureMmfRuntimeInitialized(error))
    return false;
  VDEC_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enType = toPayload(config_.codec);
  attr.enMode = toMode(config_.mode);
  attr.u32PicWidth = config_.width;
  attr.u32PicHeight = config_.height;
  attr.u32StreamBufSize = config_.stream_buffer_size > 0
                              ? config_.stream_buffer_size
                              : ALIGN(config_.width * config_.height, 0x4000);
  attr.u32FrameBufSize =
      config_.frame_buffer_size > 0
          ? config_.frame_buffer_size
          : VDEC_GetPicBufferSize(attr.enType, config_.width, config_.height,
                                  static_cast<PIXEL_FORMAT_E>(config_.output_pixel_format),
                                  DATA_BITWIDTH_8,
                                  static_cast<COMPRESS_MODE_E>(config_.compress_mode));
  attr.u32FrameBufCnt = config_.frame_buffer_count;
  attr.enCompressMode = static_cast<COMPRESS_MODE_E>(config_.compress_mode);
  attr.u8CommandQueueDepth = config_.command_queue_depth;
  attr.u8ReorderEnable = config_.reorder_enable ? 1 : 0;
  int ret = CVI_VDEC_CreateChn(config_.channel, &attr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VDEC_CreateChn failed, ret=" + std::to_string(ret));
    return false;
  }
  created_ = true;
  VDEC_CHN_PARAM_S param;
  std::memset(&param, 0, sizeof(param));
  ret = CVI_VDEC_GetChnParam(config_.channel, &param);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VDEC_GetChnParam failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  param.enType = attr.enType;
  param.enPixelFormat = static_cast<PIXEL_FORMAT_E>(config_.output_pixel_format);
  param.u32DisplayFrameNum = config_.display_frame_count;
  param.stVdecPictureParam.u32Alpha = 255;
  ret = CVI_VDEC_SetChnParam(config_.channel, &param);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VDEC_SetChnParam failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  ret = CVI_VDEC_StartRecvStream(config_.channel);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VDEC_StartRecvStream failed, ret=" + std::to_string(ret));
    close();
    return false;
  }
  started_ = true;
  opened_ = true;
  return true;
}
bool VdecChannel::sendStream(const StreamPacket& packet, std::string* error) {
  if (!opened_ && !open(error))
    return false;
  if (!packet.data || packet.size == 0) {
    setError(error, "empty vdec stream");
    return false;
  }
  VDEC_STREAM_S stream;
  std::memset(&stream, 0, sizeof(stream));
  stream.pu8Addr = const_cast<CVI_U8*>(static_cast<const CVI_U8*>(packet.data));
  stream.u32Len = packet.size;
  stream.u64PTS = packet.pts;
  stream.u64DTS = packet.dts;
  stream.bEndOfFrame = packet.end_of_frame ? CVI_TRUE : CVI_FALSE;
  stream.bEndOfStream = packet.end_of_stream ? CVI_TRUE : CVI_FALSE;
  stream.bDisplay = packet.display ? CVI_TRUE : CVI_FALSE;
  int ret = CVI_VDEC_SendStream(config_.channel, &stream, config_.timeout_ms);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VDEC_SendStream failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}
bool VdecChannel::read(Frame* frame, std::string* error) {
  if (!opened_ && !open(error))
    return false;
  if (!frame)
    return false;
  releaseFrame();
  int ret = CVI_VDEC_GetFrame(config_.channel, &frame_info_, config_.timeout_ms);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_VDEC_GetFrame failed, ret=" + std::to_string(ret));
    return false;
  }
  frame_valid_ = true;
  VIDEO_FRAME_S& v = frame_info_.stVFrame;
  frame->native = &frame_info_;
  frame->width = v.u32Width;
  frame->height = v.u32Height;
  frame->format = v.enPixelFormat;
  frame->sequence = v.u32SeqenceNo;
  frame->timestamp_us = v.u64PTS;
  return true;
}
void VdecChannel::releaseFrame() {
  if (frame_valid_) {
    CVI_VDEC_ReleaseFrame(config_.channel, &frame_info_);
    std::memset(&frame_info_, 0, sizeof(frame_info_));
    frame_valid_ = false;
  }
}
void VdecChannel::close() {
  releaseFrame();
  if (started_) {
    CVI_VDEC_StopRecvStream(config_.channel);
    started_ = false;
  }
  if (created_) {
    CVI_VDEC_DestroyChn(config_.channel);
    created_ = false;
  }
  opened_ = false;
}
}  // namespace mmf_cvi
