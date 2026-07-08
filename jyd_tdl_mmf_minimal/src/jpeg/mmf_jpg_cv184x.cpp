#include "mmf_cv184x_common.hpp"
#include "mmf_cv184x_resources.hpp"

using namespace mmf_cv184x;

struct mmf_jpg_encoder {
  explicit mmf_jpg_encoder(const mmf_jpg_encoder_config_t& cfg)
      : config(cfg),
        venc(mmf_cvi::VencChannel::mjpeg(static_cast<int>(cfg.venc_channel),
                                         static_cast<int>(cfg.width), static_cast<int>(cfg.height),
                                         0, 25, 25, static_cast<int>(cfg.quality))) {
    codec_resource_lease_init(&lease);
  }
  mmf_jpg_encoder_config_t config;
  CodecResourceLease lease;
  mmf_cvi::VencChannel venc;
  mmf_cvi::VencChannel::EncodedPacket packet;
  std::vector<std::uint8_t> contiguous;
};

struct mmf_jpg_decoder {
  explicit mmf_jpg_decoder(const mmf_jpg_decoder_config_t& cfg)
      : config(cfg),
        vdec(mmf_cvi::VdecChannel::jpeg(
            static_cast<int>(cfg.vdec_channel), static_cast<int>(cfg.max_width),
            static_cast<int>(cfg.max_height), mmf_cvi::VdecChannel::Mode::Frame,
            to_native_pixfmt(cfg.output_format))) {
    codec_resource_lease_init(&lease);
  }
  mmf_jpg_decoder_config_t config;
  CodecResourceLease lease;
  mmf_cvi::VdecChannel vdec;
  mmf_cvi::Frame frame;
  bool frame_valid = false;
};

namespace {

std::mutex g_jpg_codec_mutex;
thread_local std::vector<std::uint8_t> g_one_shot_jpeg;
thread_local std::vector<std::uint8_t> g_one_shot_frame;

}  // namespace

extern "C" {

void mmf_jpg_get_default_encoder_config(mmf_jpg_encoder_config_t* config) {
  if (config == nullptr)
    return;
  std::memset(config, 0, sizeof(*config));
  config->width = 1280;
  config->height = 720;
  config->input_format = MMF_PIXFMT_NV21;
  config->quality = 80;
  config->venc_channel = kJpegOneshotVencChannel;
  config->role = MMF_JPG_ROLE_ONESHOT;
  config->exclusive_channel = MMF_TRUE;
}

void mmf_jpg_get_default_decoder_config(mmf_jpg_decoder_config_t* config) {
  if (config == nullptr)
    return;
  std::memset(config, 0, sizeof(*config));
  config->max_width = 1920;
  config->max_height = 1080;
  config->output_format = MMF_PIXFMT_NV21;
  config->vdec_channel = 0;
}

mmf_result_t mmf_jpg_encoder_open(const mmf_jpg_encoder_config_t* config,
                                  mmf_jpg_encoder_t** encoder) {
  if (config == nullptr || encoder == nullptr)
    return MMF_EINVAL;
  std::unique_ptr<mmf_jpg_encoder_t> ptr(new mmf_jpg_encoder_t(*config));
  const char* owner =
      config->role == MMF_JPG_ROLE_HTTP_STREAM
          ? "jpg-http"
          : (config->role == MMF_JPG_ROLE_SNAPSHOT ? "jpg-snapshot" : "jpg-oneshot");
  mmf_result_t ret = acquire_codec_resource(CodecResourceType::Venc, config->venc_channel, owner,
                                            1000, &ptr->lease);
  if (ret != MMF_OK)
    return ret;
  std::string error;
  {
    CodecResourceLease op = MMF_CODEC_RESOURCE_LEASE_INIT;
    ret = acquire_codec_operation(CodecResourceType::Venc, "venc-open", 1000, &op);
    if (ret != MMF_OK)
      return ret;
    if (!ptr->venc.open(&error)) {
      codec_resource_lease_release(&op);
      return ok_or_error(false, error);
    }
    codec_resource_lease_release(&op);
  }
  *encoder = ptr.release();
  return MMF_OK;
}

void mmf_jpg_encoder_close(mmf_jpg_encoder_t* encoder) {
  if (encoder == nullptr)
    return;
  {
    CodecResourceLease op = MMF_CODEC_RESOURCE_LEASE_INIT;
    if (acquire_codec_operation(CodecResourceType::Venc, "venc-close", 1000, &op) == MMF_OK) {
      encoder->venc.close();
      codec_resource_lease_release(&op);
    } else {
      encoder->venc.close();
    }
  }
  codec_resource_lease_release(&encoder->lease);
  delete encoder;
}

mmf_result_t mmf_jpg_encode_frame(mmf_jpg_encoder_t* encoder, const mmf_video_frame_t* frame,
                                  mmf_packet_t* jpeg) {
  if (encoder == nullptr || frame == nullptr || jpeg == nullptr)
    return MMF_EINVAL;
  if (frame->priv == nullptr)
    return MMF_EINVAL;
  mmf_cvi::Frame tdl_frame;
  tdl_frame.native = frame->priv;
  tdl_frame.width = static_cast<int>(frame->width);
  tdl_frame.height = static_cast<int>(frame->height);
  tdl_frame.format = to_native_pixfmt(frame->pixel_format);
  tdl_frame.sequence = frame->sequence;
  tdl_frame.timestamp_us = frame->timestamp_us;
  std::string error;
  {
    CodecResourceLease op = MMF_CODEC_RESOURCE_LEASE_INIT;
    mmf_result_t ret = acquire_codec_operation(CodecResourceType::Venc, "venc-encode", 1000, &op);
    if (ret != MMF_OK)
      return ret;
    if (!encoder->venc.encode(tdl_frame, &encoder->packet, &error)) {
      codec_resource_lease_release(&op);
      return ok_or_error(false, error);
    }
    codec_resource_lease_release(&op);
  }
  encoder->contiguous.clear();
  for (const auto& block : encoder->packet.blocks) {
    encoder->contiguous.insert(encoder->contiguous.end(), block.begin(), block.end());
  }
  jpeg->data = encoder->contiguous.data();
  jpeg->bytes = encoder->contiguous.size();
  jpeg->codec = MMF_CODEC_JPEG;
  jpeg->sequence = frame->sequence;
  jpeg->timestamp_us = frame->timestamp_us;
  return MMF_OK;
}

mmf_result_t mmf_jpg_release_packet(mmf_jpg_encoder_t* encoder, mmf_packet_t* jpeg) {
  (void)encoder;
  if (jpeg != nullptr) {
    jpeg->data = nullptr;
    jpeg->bytes = 0;
  }
  return MMF_OK;
}

mmf_result_t mmf_jpg_decoder_open(const mmf_jpg_decoder_config_t* config,
                                  mmf_jpg_decoder_t** decoder) {
  if (config == nullptr || decoder == nullptr)
    return MMF_EINVAL;
  std::unique_ptr<mmf_jpg_decoder_t> ptr(new mmf_jpg_decoder_t(*config));
  mmf_result_t ret = acquire_codec_resource(CodecResourceType::Vdec, config->vdec_channel,
                                            "jpg-decode", 1000, &ptr->lease);
  if (ret != MMF_OK)
    return ret;
  std::string error;
  {
    CodecResourceLease op = MMF_CODEC_RESOURCE_LEASE_INIT;
    ret = acquire_codec_operation(CodecResourceType::Vdec, "vdec-open", 1000, &op);
    if (ret != MMF_OK)
      return ret;
    if (!ptr->vdec.open(&error)) {
      codec_resource_lease_release(&op);
      return ok_or_error(false, error);
    }
    codec_resource_lease_release(&op);
  }
  *decoder = ptr.release();
  return MMF_OK;
}

void mmf_jpg_decoder_close(mmf_jpg_decoder_t* decoder) {
  if (decoder == nullptr)
    return;
  {
    CodecResourceLease op = MMF_CODEC_RESOURCE_LEASE_INIT;
    if (acquire_codec_operation(CodecResourceType::Vdec, "vdec-close", 1000, &op) == MMF_OK) {
      decoder->vdec.releaseFrame();
      decoder->vdec.close();
      codec_resource_lease_release(&op);
    } else {
      decoder->vdec.releaseFrame();
      decoder->vdec.close();
    }
  }
  codec_resource_lease_release(&decoder->lease);
  delete decoder;
}

mmf_result_t mmf_jpg_decode_packet(mmf_jpg_decoder_t* decoder, const mmf_packet_t* jpeg,
                                   mmf_video_frame_t* frame) {
  if (decoder == nullptr || jpeg == nullptr || frame == nullptr || jpeg->data == nullptr ||
      jpeg->bytes == 0) {
    return MMF_EINVAL;
  }

  std::lock_guard<std::mutex> lock(g_jpg_codec_mutex);
  mmf_cvi::VdecChannel::StreamPacket packet;
  packet.data = jpeg->data;
  packet.size = jpeg->bytes;
  packet.pts = jpeg->timestamp_us;
  packet.dts = jpeg->timestamp_us;
  packet.end_of_frame = true;
  packet.end_of_stream = false;
  packet.display = true;

  std::string error;
  {
    CodecResourceLease op = MMF_CODEC_RESOURCE_LEASE_INIT;
    mmf_result_t ret = acquire_codec_operation(CodecResourceType::Vdec, "vdec-decode", 1000, &op);
    if (ret != MMF_OK)
      return ret;
    if (!decoder->vdec.sendStream(packet, &error)) {
      codec_resource_lease_release(&op);
      return ok_or_error(false, error);
    }
    if (!decoder->vdec.read(&decoder->frame, &error)) {
      codec_resource_lease_release(&op);
      return ok_or_error(false, error);
    }
    codec_resource_lease_release(&op);
  }

  std::memset(frame, 0, sizeof(*frame));
  frame->width = static_cast<uint32_t>(decoder->frame.width);
  frame->height = static_cast<uint32_t>(decoder->frame.height);
  frame->pixel_format = from_native_pixfmt(decoder->frame.format);
  frame->sequence = decoder->frame.sequence;
  frame->timestamp_us = decoder->frame.timestamp_us;
  frame->priv = decoder->frame.native;
  decoder->frame_valid = true;
  return MMF_OK;
}

mmf_result_t mmf_jpg_release_frame(mmf_jpg_decoder_t* decoder, mmf_video_frame_t* frame) {
  if (decoder != nullptr && decoder->frame_valid) {
    CodecResourceLease op = MMF_CODEC_RESOURCE_LEASE_INIT;
    if (acquire_codec_operation(CodecResourceType::Vdec, "vdec-release", 1000, &op) == MMF_OK) {
      decoder->vdec.releaseFrame();
      codec_resource_lease_release(&op);
    } else {
      decoder->vdec.releaseFrame();
    }
    decoder->frame_valid = false;
  }
  if (frame != nullptr) {
    std::memset(frame, 0, sizeof(*frame));
  }
  return MMF_OK;
}

mmf_result_t mmf_jpg_encode(const mmf_video_frame_t* frame, uint32_t quality, mmf_packet_t* jpeg) {
  if (frame == nullptr || jpeg == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_jpg_codec_mutex);
  mmf_jpg_encoder_config_t cfg;
  mmf_jpg_get_default_encoder_config(&cfg);
  cfg.width = frame->width;
  cfg.height = frame->height;
  cfg.input_format = frame->pixel_format;
  cfg.quality = quality;
  mmf_jpg_encoder_t* encoder = nullptr;
  mmf_result_t ret = mmf_jpg_encoder_open(&cfg, &encoder);
  if (ret != MMF_OK)
    return ret;
  ret = mmf_jpg_encode_frame(encoder, frame, jpeg);
  if (ret == MMF_OK && jpeg->data != nullptr && jpeg->bytes > 0) {
    const auto* begin = static_cast<const std::uint8_t*>(jpeg->data);
    g_one_shot_jpeg.assign(begin, begin + jpeg->bytes);
    jpeg->data = g_one_shot_jpeg.data();
    jpeg->bytes = g_one_shot_jpeg.size();
  }
  mmf_jpg_encoder_close(encoder);
  return ret;
}

mmf_result_t mmf_jpg_decode(const void* jpeg_data, size_t jpeg_bytes,
                            mmf_pixel_format_t output_format, mmf_video_frame_t* frame) {
  if (jpeg_data == nullptr || jpeg_bytes == 0 || frame == nullptr) {
    return MMF_EINVAL;
  }

  mmf_jpg_decoder_config_t cfg;
  mmf_jpg_get_default_decoder_config(&cfg);
  cfg.output_format = output_format;
  mmf_jpg_decoder_t* decoder = nullptr;
  mmf_result_t ret = mmf_jpg_decoder_open(&cfg, &decoder);
  if (ret != MMF_OK) {
    return ret;
  }

  mmf_packet_t packet;
  std::memset(&packet, 0, sizeof(packet));
  packet.data = jpeg_data;
  packet.bytes = jpeg_bytes;
  packet.codec = MMF_CODEC_JPEG;

  mmf_video_frame_t decoded;
  std::memset(&decoded, 0, sizeof(decoded));
  ret = mmf_jpg_decode_packet(decoder, &packet, &decoded);
  if (ret == MMF_OK) {
    ret = copy_native_video_frame(decoded, frame, &g_one_shot_frame);
  }
  mmf_jpg_release_frame(decoder, &decoded);
  mmf_jpg_decoder_close(decoder);
  return ret;
}

}  // extern "C"
