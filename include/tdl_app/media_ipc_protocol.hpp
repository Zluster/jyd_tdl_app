#pragma once

#include <cstdint>
#include <string>

#include "tdl_app/media_types.hpp"

namespace tdl_app {

struct MediaIpcProtocol {
  static constexpr std::uint32_t kVersion = 1;
  static constexpr std::uint32_t kDefaultRemoteId = 0;
  static constexpr std::uint32_t kDefaultPort = 41;
  static constexpr std::int32_t kDefaultTimeoutMs = 3000;
  static constexpr const char *kDefaultServiceName = "tdl_media";
};

enum class MediaIpcModule : std::uint32_t {
  Sys = 1,
  Vb = 2,
  Vi = 3,
  Vpss = 4,
  Vo = 5,
  GraphicVo = 6,
  Rgn = 7,
  Venc = 8,
  Vdec = 9,
  Sensor = 10,
  Audio = 11,
  Pipeline = 12,
};

enum class MediaIpcCommand : std::uint32_t {
  Open = 1,
  Close = 2,
  Start = 3,
  Stop = 4,
  Bind = 5,
  Unbind = 6,
  GetFrame = 7,
  ReleaseFrame = 8,
  SetVisible = 9,
  UpdateBitmap = 10,
  Clear = 11,
  QueryStatus = 12,
  Ping = 13,
};

struct MediaIpcServiceConfig {
  std::string service_name = MediaIpcProtocol::kDefaultServiceName;
  std::uint32_t remote_id = MediaIpcProtocol::kDefaultRemoteId;
  std::uint32_t port = MediaIpcProtocol::kDefaultPort;
  std::uint32_t priority = 0;
  std::int32_t timeout_ms = MediaIpcProtocol::kDefaultTimeoutMs;
};

struct MediaIpcHeader {
  std::uint32_t version = MediaIpcProtocol::kVersion;
  std::uint32_t module = 0;
  std::uint32_t command = 0;
  std::uint32_t sequence = 0;
  std::uint32_t payload_bytes = 0;
};

struct MediaIpcStatus {
  std::int32_t code = 0;
  std::uint32_t detail = 0;
};

struct MediaIpcChannelRef {
  std::uint32_t module = 0;
  std::int32_t device = 0;
  std::int32_t channel = 0;

  static MediaIpcChannelRef fromMediaChannel(const MediaChannel &value) {
    MediaIpcChannelRef out;
    out.module = static_cast<std::uint32_t>(value.module);
    out.device = value.device;
    out.channel = value.channel;
    return out;
  }
};

struct MediaIpcSharedBuffer {
  std::uint64_t physical = 0;
  std::uint64_t virtual_address = 0;
  std::uint32_t bytes = 0;
  std::uint32_t stride = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t pixel_format = 0;
};

struct MediaIpcOpenVoRequest {
  std::int32_t device = 0;
  std::int32_t layer = 0;
  std::int32_t channel = 0;
  std::int32_t width = 640;
  std::int32_t height = 640;
  std::int32_t pixel_format = PixelFormat::NV12;
  std::int32_t interface_type = VoInterfaceType::Mipi;
  std::int32_t interface_sync = VoInterfaceSync::P720_1280_60;
  std::int32_t display_buf_len = 3;
  std::int32_t frame_rate = 25;
  std::int32_t channel_x = 0;
  std::int32_t channel_y = 0;
  std::int32_t priority = 0;
};

struct MediaIpcOpenGraphicVoRequest {
  std::int32_t layer = 0;
  std::int32_t width = 720;
  std::int32_t height = 1280;
  std::int32_t format = GraphicBufferFormat::Argb8888;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t display_width = 0;
  std::int32_t display_height = 0;
  std::int32_t screen_width = 0;
  std::int32_t screen_height = 0;
  std::int32_t double_buffer = 0;
  std::int32_t visible = 1;
};

struct MediaIpcGraphicVoClearRequest {
  std::uint32_t argb = 0xFF000000U;
};

struct MediaIpcGraphicVoUpdateRequest {
  MediaIpcSharedBuffer buffer;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
};

struct MediaIpcBindRequest {
  MediaIpcChannelRef source;
  MediaIpcChannelRef destination;
};

struct MediaIpcOpenVpssRequest {
  std::int32_t group = 0;
  std::int32_t input_width = 1920;
  std::int32_t input_height = 1080;
  std::int32_t output_width = 640;
  std::int32_t output_height = 640;
  std::int32_t channel = 0;
  std::int32_t pixel_format = PixelFormat::NV12;
  std::int32_t src_fps = -1;
  std::int32_t dst_fps = -1;
  std::int32_t mirror = 0;
  std::int32_t flip = 0;
};

struct MediaIpcOpenRegionRequest {
  std::int32_t handle = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::int32_t pixel_format = 24;
  std::int32_t canvas_count = 2;
  std::uint32_t bg_color = 0;
};

struct MediaIpcAttachRegionRequest {
  std::int32_t handle = 0;
  MediaIpcChannelRef channel;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t layer = 0;
  std::int32_t visible = 1;
};

struct MediaIpcOpenVencRequest {
  std::int32_t channel = 0;
  std::int32_t codec = 0;
  std::int32_t width = 1920;
  std::int32_t height = 1080;
  std::int32_t bitrate_kbps = 2048;
  std::int32_t gop = 25;
  std::int32_t src_fps = 25;
  std::int32_t dst_fps = 25;
};

struct MediaIpcRequestFrame {
  MediaIpcChannelRef channel;
  std::int32_t timeout_ms = MediaIpcProtocol::kDefaultTimeoutMs;
};

struct MediaIpcFrameReply {
  MediaIpcSharedBuffer buffer;
  std::uint64_t pts_us = 0;
};

}  // namespace tdl_app
