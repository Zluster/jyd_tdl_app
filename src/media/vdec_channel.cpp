#include "tdl_app/vdec_channel.hpp"

#include <cstdio>
#include <cstring>

#include "cvi_buffer.h"
#include "cvi_comm_vdec.h"
#include "cvi_common.h"
#include "cvi_vdec.h"

namespace tdl_app {
namespace {

constexpr int kDefaultStreamBufferSize = 2 * 1024 * 1024;

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

PAYLOAD_TYPE_E toPayload(VdecChannel::Codec codec) {
  switch (codec) {
    case VdecChannel::Codec::H265:
      return PT_H265;
    case VdecChannel::Codec::Jpeg:
      return PT_JPEG;
    case VdecChannel::Codec::Mjpeg:
      return PT_MJPEG;
    case VdecChannel::Codec::H264:
    default:
      return PT_H264;
  }
}

VIDEO_MODE_E toVideoMode(VdecChannel::Mode mode) {
  switch (mode) {
    case VdecChannel::Mode::Frame:
      return VIDEO_MODE_FRAME;
    case VdecChannel::Mode::Compat:
      return VIDEO_MODE_COMPAT;
    case VdecChannel::Mode::Stream:
    default:
      return VIDEO_MODE_STREAM;
  }
}

bool isVideoCodec(VdecChannel::Codec codec) {
  return codec == VdecChannel::Codec::H264 || codec == VdecChannel::Codec::H265;
}

bool isPictureCodec(VdecChannel::Codec codec) {
  return codec == VdecChannel::Codec::Jpeg || codec == VdecChannel::Codec::Mjpeg;
}

std::uint32_t resolveFrameBufferSize(const VdecChannel::Config &config) {
  if (config.frame_buffer_size > 0) {
    return static_cast<std::uint32_t>(config.frame_buffer_size);
  }
  return VDEC_GetPicBufferSize(
      toPayload(config.codec), static_cast<CVI_U32>(config.width),
      static_cast<CVI_U32>(config.height),
      static_cast<PIXEL_FORMAT_E>(config.output_pixel_format), DATA_BITWIDTH_8,
      static_cast<COMPRESS_MODE_E>(config.compress_mode));
}

std::uint32_t resolveStreamBufferSize(const VdecChannel::Config &config) {
  if (config.stream_buffer_size > 0) {
    return static_cast<std::uint32_t>(config.stream_buffer_size);
  }
  const CVI_U32 default_size = ALIGN(
      static_cast<CVI_U32>(config.width * config.height), 0x4000);
  return default_size > 0 ? default_size : kDefaultStreamBufferSize;
}

std::uint32_t resolveFrameBufferCount(const VdecChannel::Config &config) {
  if (isPictureCodec(config.codec) &&
      config.frame_buffer_count == VdecChannel::Config{}.frame_buffer_count) {
    return 1;
  }
  return static_cast<std::uint32_t>(config.frame_buffer_count);
}

std::uint32_t resolveDisplayFrameCount(const VdecChannel::Config &config) {
  if (isPictureCodec(config.codec) &&
      config.display_frame_count == VdecChannel::Config{}.display_frame_count) {
    return 0;
  }
  return static_cast<std::uint32_t>(config.display_frame_count);
}

class VdecChannelImpl {
 public:
  explicit VdecChannelImpl(const VdecChannel::Config &config)
      : config_(config) {}

  ~VdecChannelImpl() { close(); }

  bool open(std::string *error) {
    if (opened_) {
      return true;
    }

    std::fprintf(stderr,
                 "vdec: open begin ch=%d codec=%d mode=%d size=%dx%d timeout_ms=%d\n",
                 config_.channel, static_cast<int>(config_.codec),
                 static_cast<int>(config_.mode), config_.width, config_.height,
                 config_.timeout_ms);

    VDEC_CHN_ATTR_S attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.enType = toPayload(config_.codec);
    attr.enMode = toVideoMode(config_.mode);
    attr.u32PicWidth = static_cast<CVI_U32>(config_.width);
    attr.u32PicHeight = static_cast<CVI_U32>(config_.height);
    attr.u32StreamBufSize = resolveStreamBufferSize(config_);
    attr.u32FrameBufSize = resolveFrameBufferSize(config_);
    attr.u32FrameBufCnt = resolveFrameBufferCount(config_);
    attr.enCompressMode = static_cast<COMPRESS_MODE_E>(config_.compress_mode);
    attr.u8CommandQueueDepth =
        static_cast<CVI_U8>(config_.command_queue_depth);
    attr.u8ReorderEnable = config_.reorder_enable ? 1 : 0;

    std::fprintf(stderr, "vdec: create channel\n");
    int ret = CVI_SUCCESS;
    if (config_.picture_pool_id >= 0 || config_.tmv_pool_id >= 0) {
      VDEC_MOD_PARAM_S mod_param;
      std::memset(&mod_param, 0, sizeof(mod_param));
      ret = CVI_VDEC_GetModParam(&mod_param);
      if (ret != CVI_SUCCESS) {
        setError(error, "CVI_VDEC_GetModParam failed, ret=" + std::to_string(ret));
        return false;
      }
      mod_param.enVdecVBSource = VB_SOURCE_USER;
      ret = CVI_VDEC_SetModParam(&mod_param);
      if (ret != CVI_SUCCESS) {
        setError(error, "CVI_VDEC_SetModParam failed, ret=" + std::to_string(ret));
        return false;
      }
      std::fprintf(stderr, "vdec: set module vb source to user\n");
    }

    ret = CVI_VDEC_CreateChn(config_.channel, &attr);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VDEC_CreateChn failed, ret=" + std::to_string(ret));
      return false;
    }
    created_ = true;
    std::fprintf(stderr, "vdec: create channel done\n");

    if (config_.picture_pool_id >= 0 || config_.tmv_pool_id >= 0) {
      VDEC_CHN_POOL_S pool;
      std::memset(&pool, 0, sizeof(pool));
      pool.hPicVbPool = config_.picture_pool_id >= 0
                            ? static_cast<VB_POOL>(config_.picture_pool_id)
                            : VB_INVALID_POOLID;
      pool.hTmvVbPool = config_.tmv_pool_id >= 0
                            ? static_cast<VB_POOL>(config_.tmv_pool_id)
                            : VB_INVALID_POOLID;
      std::fprintf(stderr, "vdec: attach vb pool pic=%d tmv=%d\n",
                   config_.picture_pool_id, config_.tmv_pool_id);
      ret = CVI_VDEC_AttachVbPool(config_.channel, &pool);
      if (ret != CVI_SUCCESS) {
        setError(error,
                 "CVI_VDEC_AttachVbPool failed, ret=" + std::to_string(ret));
        close();
        return false;
      }
      pool_attached_ = true;
      std::fprintf(stderr, "vdec: attach vb pool done\n");
    }

    VDEC_CHN_PARAM_S param;
    std::memset(&param, 0, sizeof(param));
    ret = CVI_VDEC_GetChnParam(config_.channel, &param);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VDEC_GetChnParam failed, ret=" + std::to_string(ret));
      close();
      return false;
    }

    if (isVideoCodec(config_.codec)) {
      param.stVdecVideoParam.s32ErrThreshold = 100;
      param.stVdecVideoParam.enDecMode = VIDEO_DEC_MODE_IPB;
      param.stVdecVideoParam.enOutputOrder = VIDEO_OUTPUT_ORDER_DISP;
      param.stVdecVideoParam.enCompressMode =
          static_cast<COMPRESS_MODE_E>(config_.compress_mode);
      param.stVdecVideoParam.enVideoFormat = VIDEO_FORMAT_LINEAR;
    } else {
      param.stVdecPictureParam.u32Alpha = 255;
    }
    param.enType = toPayload(config_.codec);
    param.enPixelFormat =
        static_cast<PIXEL_FORMAT_E>(config_.output_pixel_format);
    param.u32DisplayFrameNum = resolveDisplayFrameCount(config_);
    std::fprintf(stderr, "vdec: set channel param\n");
    ret = CVI_VDEC_SetChnParam(config_.channel, &param);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VDEC_SetChnParam failed, ret=" + std::to_string(ret));
      close();
      return false;
    }
    std::fprintf(stderr, "vdec: set channel param done\n");

    std::fprintf(stderr, "vdec: start recv stream\n");
    ret = CVI_VDEC_StartRecvStream(config_.channel);
    if (ret != CVI_SUCCESS) {
      setError(error,
               "CVI_VDEC_StartRecvStream failed, ret=" + std::to_string(ret));
      close();
      return false;
    }
    std::fprintf(stderr, "vdec: start recv stream done\n");

    started_ = true;
    opened_ = true;
    std::fprintf(stderr, "vdec: open done\n");
    return true;
  }

  void close() {
    releaseFrame();
    if (started_) {
      CVI_VDEC_StopRecvStream(config_.channel);
      started_ = false;
    }
    if (pool_attached_) {
      CVI_VDEC_DetachVbPool(config_.channel);
      pool_attached_ = false;
    }
    if (created_) {
      CVI_VDEC_DestroyChn(config_.channel);
      created_ = false;
    }
    opened_ = false;
  }

  bool isOpen() const { return opened_; }

  bool sendStream(const VdecChannel::StreamPacket &packet, std::string *error) {
    if (!opened_ && !open(error)) {
      return false;
    }
    if (!packet.data || packet.size == 0) {
      setError(error, "vdec stream packet is empty");
      return false;
    }

    VDEC_STREAM_S stream;
    std::memset(&stream, 0, sizeof(stream));
    stream.u32Len = static_cast<CVI_U32>(packet.size);
    stream.u64PTS = packet.pts;
    stream.u64DTS = packet.dts;
    stream.bEndOfFrame = packet.end_of_frame ? CVI_TRUE : CVI_FALSE;
    stream.bEndOfStream = packet.end_of_stream ? CVI_TRUE : CVI_FALSE;
    stream.bDisplay = packet.display ? CVI_TRUE : CVI_FALSE;
    stream.pu8Addr =
        const_cast<CVI_U8 *>(static_cast<const CVI_U8 *>(packet.data));

    const int ret =
        CVI_VDEC_SendStream(config_.channel, &stream, config_.timeout_ms);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VDEC_SendStream failed, ret=" + std::to_string(ret));
      return false;
    }
    return true;
  }

  bool read(Frame *frame, std::string *error) {
    if (!opened_ && !open(error)) {
      return false;
    }
    if (!frame) {
      setError(error, "frame pointer is null");
      return false;
    }

    releaseFrame();

    const int ret =
        CVI_VDEC_GetFrame(config_.channel, &frame_info_, config_.timeout_ms);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VDEC_GetFrame failed, ret=" + std::to_string(ret));
      return false;
    }

    frame_valid_ = true;
    const auto &video = frame_info_.stVFrame;
    const bool empty_frame =
        video.u32Width == 0 && video.u32Height == 0 &&
        video.u32Length[0] == 0 && video.u32Length[1] == 0 &&
        video.u32Length[2] == 0;
    if (empty_frame) {
      std::fprintf(stderr,
                   "vdec: empty frame received src_end=%d pts=%llu seq=%u\n",
                   video.bSrcEnd ? 1 : 0,
                   static_cast<unsigned long long>(video.u64PTS),
                   video.u32SeqenceNo);
      releaseFrame();
      setError(error, video.bSrcEnd ? "vdec returned EOS frame"
                                    : "vdec returned empty frame");
      return false;
    }

    std::fprintf(stderr,
                 "vdec: frame width=%u height=%u fmt=%d len0=%u len1=%u len2=%u src_end=%d pts=%llu seq=%u\n",
                 video.u32Width, video.u32Height,
                 static_cast<int>(video.enPixelFormat),
                 video.u32Length[0], video.u32Length[1], video.u32Length[2],
                 video.bSrcEnd ? 1 : 0,
                 static_cast<unsigned long long>(video.u64PTS),
                 video.u32SeqenceNo);

    frame->image_path.clear();
    frame->native = &frame_info_;
    frame->width = static_cast<int>(video.u32Width);
    frame->height = static_cast<int>(video.u32Height);
    frame->format = static_cast<int>(video.enPixelFormat);
    frame->sequence = video.u32SeqenceNo;
    frame->timestamp_us = video.u64PTS;
    return true;
  }

  void releaseFrame() {
    if (!frame_valid_) {
      return;
    }
    CVI_VDEC_ReleaseFrame(config_.channel, &frame_info_);
    std::memset(&frame_info_, 0, sizeof(frame_info_));
    frame_valid_ = false;
  }

  bool queryStatus(VdecChannel::Status *status, std::string *error) const {
    if (!status) {
      setError(error, "vdec status pointer is null");
      return false;
    }

    std::fprintf(stderr, "vdec: query status\n");
    VDEC_CHN_STATUS_S raw;
    std::memset(&raw, 0, sizeof(raw));
    const int ret = CVI_VDEC_QueryStatus(config_.channel, &raw);
    if (ret != CVI_SUCCESS) {
      setError(error, "CVI_VDEC_QueryStatus failed, ret=" + std::to_string(ret));
      return false;
    }
    std::fprintf(stderr, "vdec: query status done left_bytes=%d left_frames=%d left_pics=%d started=%d\n",
                 raw.u32LeftStreamBytes, raw.u32LeftStreamFrames,
                 raw.u32LeftPics, raw.bStartRecvStream ? 1 : 0);

    status->left_stream_bytes = raw.u32LeftStreamBytes;
    status->left_stream_frames = raw.u32LeftStreamFrames;
    status->left_pics = raw.u32LeftPics;
    status->started = raw.bStartRecvStream == CVI_TRUE;
    status->received_frames = raw.u32RecvStreamFrames;
    status->decoded_frames = raw.u32DecodeStreamFrames;
    status->width = static_cast<int>(raw.u32Width);
    status->height = static_cast<int>(raw.u32Height);
    return true;
  }

 private:
  VdecChannel::Config config_;
  VIDEO_FRAME_INFO_S frame_info_ {};
  bool frame_valid_ = false;
  bool opened_ = false;
  bool created_ = false;
  bool started_ = false;
  bool pool_attached_ = false;
};

}  // namespace

class VdecChannel::Impl : public VdecChannelImpl {
 public:
  explicit Impl(const Config &config) : VdecChannelImpl(config) {}
};

VdecChannel::VdecChannel() : VdecChannel(Config{}) {}

VdecChannel::VdecChannel(const Config &config)
    : config_(config), impl_(new Impl(config_)) {}

VdecChannel::~VdecChannel() {
  delete impl_;
  impl_ = nullptr;
}

bool VdecChannel::open(std::string *error) { return impl_->open(error); }

void VdecChannel::close() { impl_->close(); }

bool VdecChannel::isOpen() const { return impl_->isOpen(); }

bool VdecChannel::sendStream(const StreamPacket &packet, std::string *error) {
  return impl_->sendStream(packet, error);
}

bool VdecChannel::read(Frame *frame, std::string *error) {
  return impl_->read(frame, error);
}

void VdecChannel::releaseFrame() { impl_->releaseFrame(); }

bool VdecChannel::queryStatus(Status *status, std::string *error) const {
  return impl_->queryStatus(status, error);
}

}  // namespace tdl_app
