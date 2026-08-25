#include <algorithm>

#include "mmf_cv184x_common.hpp"
#include "mmf_cv184x_resources.hpp"

using namespace mmf_cv184x;

struct mmf_jpg_http_server {
  mmf_jpg_http_config_t config;
  std::atomic<bool> streaming{false};
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> client_count{0};
  std::atomic<uint64_t> frames_published{0};
  std::atomic<uint64_t> bytes_published{0};
  std::atomic<uint64_t> last_frame_sequence{0};
  std::thread worker;
  std::thread producer;
  int listen_fd = -1;
  std::mutex jpeg_mutex;
  std::condition_variable jpeg_cv;
  std::vector<std::uint8_t> last_jpeg;
  uint64_t last_jpeg_sequence = 0;
  uint64_t last_jpeg_timestamp_us = 0;
  std::mutex pull_mutex;
  mmf_camera_t* pull_camera = nullptr;
  mmf_jpg_encoder_t* pull_encoder = nullptr;
  uint32_t pull_width = 0;
  uint32_t pull_height = 0;
  mmf_pixel_format_t pull_format = MMF_PIXFMT_UNKNOWN;
  uint32_t pull_quality = 0;
  uint32_t pull_venc_channel = 0;
  CodecResourceLease display_venc_lease = MMF_CODEC_RESOURCE_LEASE_INIT;
  VPSS_GRP display_rotate_vpss_group = -1;
  bool display_rotate_vpss_created = false;
  bool display_rotate_vpss_channel_enabled = false;
  bool display_rotate_vpss_started = false;
  bool display_rotate_vpss_source_bound = false;
  bool display_venc_opened = false;
  bool display_venc_bound = false;
  uint64_t display_venc_sequence = 0;
  int display_venc_last_error = CVI_SUCCESS;
};

constexpr int kDisplayVencTimeoutMs = 1000;
constexpr int kDisplayVencMaxWidth = 2560;
constexpr int kDisplayVencMaxHeight = 1440;
constexpr int kDisplayVencBufferSize = 1024 * 1024;
constexpr VPSS_CHN kDisplayRotateVpssChannel = 0;
constexpr uint32_t kDisplayVencMaxFps = 30;

static uint32_t normalize_display_venc_fps(uint32_t fps) {
  return std::max(1U, std::min(kDisplayVencMaxFps, fps == 0 ? kDisplayVencMaxFps : fps));
}

static mmf_result_t report_display_venc_error(mmf_jpg_http_server_t* server,
                                              const char* operation, int error) {
  if (server != nullptr && server->display_venc_last_error != error) {
    std::fprintf(stderr, "jpg-http display venc: %s failed, ret=0x%x\n", operation, error);
    server->display_venc_last_error = error;
  }
  set_last_error(std::string("jpg-http display venc: ") + operation +
                 " failed, ret=" + std::to_string(error));
  return MMF_EIO;
}

static void close_http_display_venc(mmf_jpg_http_server_t* server) {
  if (server == nullptr) {
    return;
  }

  const VPSS_GRP rotate_group = server->display_rotate_vpss_group;
  if (server->display_venc_bound && rotate_group >= 0) {
    MMF_CHN_S source;
    MMF_CHN_S destination;
    std::memset(&source, 0, sizeof(source));
    std::memset(&destination, 0, sizeof(destination));
    source.enModId = CVI_ID_VPSS;
    source.s32DevId = rotate_group;
    source.s32ChnId = kDisplayRotateVpssChannel;
    destination.enModId = CVI_ID_VENC;
    destination.s32ChnId = static_cast<CVI_S32>(server->config.venc_channel);
    (void)CVI_SYS_UnBind(&source, &destination);
    server->display_venc_bound = false;
  }
  if (server->display_venc_opened) {
    (void)CVI_VENC_StopRecvFrame(static_cast<VENC_CHN>(server->config.venc_channel));
    (void)CVI_VENC_DestroyChn(static_cast<VENC_CHN>(server->config.venc_channel));
    server->display_venc_opened = false;
  }
  if (server->display_rotate_vpss_source_bound && rotate_group >= 0) {
    MMF_CHN_S source;
    MMF_CHN_S destination;
    std::memset(&source, 0, sizeof(source));
    std::memset(&destination, 0, sizeof(destination));
    source.enModId = CVI_ID_VPSS;
    source.s32DevId = mmf_cvi::DualOsLayout::kDisplayVpssGroup;
    source.s32ChnId = mmf_cvi::DualOsLayout::kDisplayChannel;
    destination.enModId = CVI_ID_VPSS;
    destination.s32DevId = rotate_group;
    destination.s32ChnId = kDisplayRotateVpssChannel;
    (void)CVI_SYS_UnBind(&source, &destination);
    server->display_rotate_vpss_source_bound = false;
  }
  if (server->display_rotate_vpss_started && rotate_group >= 0) {
    (void)CVI_VPSS_StopGrp(rotate_group);
    server->display_rotate_vpss_started = false;
  }
  if (server->display_rotate_vpss_channel_enabled && rotate_group >= 0) {
    (void)CVI_VPSS_DisableChn(rotate_group, kDisplayRotateVpssChannel);
    server->display_rotate_vpss_channel_enabled = false;
  }
  if (server->display_rotate_vpss_created && rotate_group >= 0) {
    (void)CVI_VPSS_DestroyGrp(rotate_group);
    server->display_rotate_vpss_created = false;
  }
  server->display_rotate_vpss_group = -1;
  codec_resource_lease_release(&server->display_venc_lease);
  server->display_venc_sequence = 0;
  server->display_venc_last_error = CVI_SUCCESS;
}

static mmf_result_t ensure_http_display_rotate_vpss(mmf_jpg_http_server_t* server) {
  if (server->display_rotate_vpss_source_bound) {
    return MMF_OK;
  }

  VPSS_GRP_ATTR_S group_attr;
  std::memset(&group_attr, 0, sizeof(group_attr));
  group_attr.u32MaxW = server->config.width;
  group_attr.u32MaxH = server->config.height;
  group_attr.enPixelFormat = PIXEL_FORMAT_NV12;
  group_attr.stFrameRate.s32SrcFrameRate = -1;
  group_attr.stFrameRate.s32DstFrameRate = -1;
  const int available_group = CVI_VPSS_GetAvailableGrp();
  if (available_group < 0) {
    return report_display_venc_error(server, "CVI_VPSS_GetAvailableGrp", available_group);
  }
  const VPSS_GRP rotate_group = static_cast<VPSS_GRP>(available_group);
  const int create_ret = CVI_VPSS_CreateGrp(rotate_group, &group_attr);
  if (create_ret != CVI_SUCCESS) {
    return report_display_venc_error(server, "CVI_VPSS_CreateGrp", create_ret);
  }
  server->display_rotate_vpss_created = true;
  server->display_rotate_vpss_group = rotate_group;

  VPSS_CHN_ATTR_S channel_attr;
  std::memset(&channel_attr, 0, sizeof(channel_attr));
  channel_attr.u32Width = server->config.width;
  channel_attr.u32Height = server->config.height;
  channel_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
  channel_attr.enPixelFormat = PIXEL_FORMAT_NV12;
  channel_attr.stFrameRate.s32SrcFrameRate = -1;
  channel_attr.stFrameRate.s32DstFrameRate = -1;
  channel_attr.u32Depth = 2;
  channel_attr.bMirror = CVI_TRUE;
  channel_attr.bFlip = CVI_TRUE;
  channel_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
  const int set_attr_ret = CVI_VPSS_SetChnAttr(rotate_group,
                                              kDisplayRotateVpssChannel, &channel_attr);
  if (set_attr_ret != CVI_SUCCESS) {
    close_http_display_venc(server);
    return report_display_venc_error(server, "CVI_VPSS_SetChnAttr", set_attr_ret);
  }
  const int enable_ret = CVI_VPSS_EnableChn(rotate_group,
                                            kDisplayRotateVpssChannel);
  if (enable_ret != CVI_SUCCESS) {
    close_http_display_venc(server);
    return report_display_venc_error(server, "CVI_VPSS_EnableChn", enable_ret);
  }
  server->display_rotate_vpss_channel_enabled = true;
  const int start_ret = CVI_VPSS_StartGrp(rotate_group);
  if (start_ret != CVI_SUCCESS) {
    close_http_display_venc(server);
    return report_display_venc_error(server, "CVI_VPSS_StartGrp", start_ret);
  }
  server->display_rotate_vpss_started = true;

  MMF_CHN_S source;
  MMF_CHN_S destination;
  std::memset(&source, 0, sizeof(source));
  std::memset(&destination, 0, sizeof(destination));
  source.enModId = CVI_ID_VPSS;
  source.s32DevId = mmf_cvi::DualOsLayout::kDisplayVpssGroup;
  source.s32ChnId = mmf_cvi::DualOsLayout::kDisplayChannel;
  destination.enModId = CVI_ID_VPSS;
  destination.s32DevId = rotate_group;
  destination.s32ChnId = kDisplayRotateVpssChannel;
  const int bind_ret = CVI_SYS_Bind(&source, &destination);
  if (bind_ret != CVI_SUCCESS) {
    close_http_display_venc(server);
    return report_display_venc_error(server, "CVI_SYS_Bind display VPSS->rotate VPSS",
                                     bind_ret);
  }
  server->display_rotate_vpss_source_bound = true;
  return MMF_OK;
}

static void close_http_pull_resources(mmf_jpg_http_server_t* server) {
  if (server == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(server->pull_mutex);
  close_http_display_venc(server);
  if (server->pull_encoder != nullptr) {
    mmf_jpg_encoder_close(server->pull_encoder);
    server->pull_encoder = nullptr;
  }
  if (server->pull_camera != nullptr) {
    mmf_camera_close(server->pull_camera);
    server->pull_camera = nullptr;
  }
  server->pull_width = 0;
  server->pull_height = 0;
  server->pull_format = MMF_PIXFMT_UNKNOWN;
  server->pull_quality = 0;
  server->pull_venc_channel = 0;
}

static mmf_result_t ensure_http_display_venc(mmf_jpg_http_server_t* server) {
  if (server == nullptr) {
    return MMF_EINVAL;
  }
  if (server->display_venc_bound) {
    return MMF_OK;
  }

  mmf_result_t ret = ensure_http_display_rotate_vpss(server);
  if (ret != MMF_OK) {
    close_http_display_venc(server);
    return ret;
  }

  ret = acquire_codec_resource(CodecResourceType::Venc, server->config.venc_channel,
                               "jpg-http-display", 1000, &server->display_venc_lease);
  if (ret != MMF_OK) {
    set_last_error("jpg-http display venc: VENC channel is busy");
    if (server->display_venc_last_error != static_cast<int>(ret)) {
      std::fprintf(stderr, "jpg-http display venc: VENC%d resource busy, ret=%d\n",
                   server->config.venc_channel, ret);
      server->display_venc_last_error = static_cast<int>(ret);
    }
    return ret;
  }

  VENC_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.stVencAttr.u32PicWidth = server->config.width;
  attr.stVencAttr.u32PicHeight = server->config.height;
  attr.stVencAttr.u32MaxPicWidth = kDisplayVencMaxWidth;
  attr.stVencAttr.u32MaxPicHeight = kDisplayVencMaxHeight;
  attr.stVencAttr.u32BufSize = kDisplayVencBufferSize;
  attr.stVencAttr.enType = PT_MJPEG;
  attr.stVencAttr.bEsBufQueueEn = CVI_TRUE;
  attr.stVencAttr.bIsoSendFrmEn = CVI_TRUE;
  attr.stGopAttr.enGopMode = VENC_GOPMODE_NORMALP;
  attr.stGopAttr.stNormalP.s32IPQpDelta = 2;
  attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGFIXQP;
  const uint32_t frame_rate = normalize_display_venc_fps(server->config.fps);
  attr.stRcAttr.stMjpegFixQp.u32SrcFrameRate = frame_rate;
  attr.stRcAttr.stMjpegFixQp.fr32DstFrameRate = frame_rate;
  attr.stRcAttr.stMjpegFixQp.u32Qfactor =
      std::max(1U, std::min(99U, server->config.jpeg_quality));

  const VENC_CHN channel = static_cast<VENC_CHN>(server->config.venc_channel);
  const int create_ret = CVI_VENC_CreateChn(channel, &attr);
  if (create_ret != CVI_SUCCESS) {
    close_http_display_venc(server);
    return report_display_venc_error(server, "CVI_VENC_CreateChn", create_ret);
  }
  server->display_venc_opened = true;

  MMF_CHN_S source;
  MMF_CHN_S destination;
  std::memset(&source, 0, sizeof(source));
  std::memset(&destination, 0, sizeof(destination));
  source.enModId = CVI_ID_VPSS;
  source.s32DevId = server->display_rotate_vpss_group;
  source.s32ChnId = kDisplayRotateVpssChannel;
  destination.enModId = CVI_ID_VENC;
  destination.s32ChnId = static_cast<CVI_S32>(server->config.venc_channel);
  const int bind_ret = CVI_SYS_Bind(&source, &destination);
  if (bind_ret != CVI_SUCCESS) {
    close_http_display_venc(server);
    return report_display_venc_error(server, "CVI_SYS_Bind rotate VPSS->VENC", bind_ret);
  }
  server->display_venc_bound = true;

  VENC_RECV_PIC_PARAM_S recv;
  std::memset(&recv, 0, sizeof(recv));
  recv.s32RecvPicNum = -1;
  const int start_ret = CVI_VENC_StartRecvFrame(channel, &recv);
  if (start_ret != CVI_SUCCESS) {
    close_http_display_venc(server);
    return report_display_venc_error(server, "CVI_VENC_StartRecvFrame", start_ret);
  }
  server->display_venc_last_error = CVI_SUCCESS;
  std::fprintf(stderr,
               "jpg-http display venc: VPSS%d.%d -> VPSS%d.%d (mirror+flip) -> VENC%d at %u fps\n",
               mmf_cvi::DualOsLayout::kDisplayVpssGroup,
               mmf_cvi::DualOsLayout::kDisplayChannel, source.s32DevId,
               source.s32ChnId, destination.s32ChnId, frame_rate);
  return MMF_OK;
}

static mmf_result_t capture_http_display_jpeg(mmf_jpg_http_server_t* server,
                                              std::vector<std::uint8_t>* jpeg,
                                              uint64_t* sequence) {
  const mmf_result_t ret = ensure_http_display_venc(server);
  if (ret != MMF_OK) {
    return ret;
  }

  const VENC_CHN channel = static_cast<VENC_CHN>(server->config.venc_channel);
  VENC_CHN_STATUS_S status;
  std::memset(&status, 0, sizeof(status));
  const int query_ret = CVI_VENC_QueryStatus(channel, &status);
  if (query_ret != CVI_SUCCESS) {
    return report_display_venc_error(server, "CVI_VENC_QueryStatus", query_ret);
  }
  if (status.u32CurPacks == 0) {
    return MMF_ENOTREADY;
  }

  VENC_STREAM_S stream;
  std::memset(&stream, 0, sizeof(stream));
  stream.pstPack = static_cast<VENC_PACK_S*>(
      std::malloc(sizeof(VENC_PACK_S) * status.u32CurPacks));
  if (stream.pstPack == nullptr) {
    return MMF_ENOMEM;
  }
  const int stream_ret = CVI_VENC_GetStream(channel, &stream, kDisplayVencTimeoutMs);
  if (stream_ret != CVI_SUCCESS) {
    std::free(stream.pstPack);
    return report_display_venc_error(server, "CVI_VENC_GetStream", stream_ret);
  }

  jpeg->clear();
  for (CVI_U32 index = 0; index < stream.u32PackCount; ++index) {
    const VENC_PACK_S& pack = stream.pstPack[index];
    if (pack.u32Len > pack.u32Offset) {
      const auto* begin = pack.pu8Addr + pack.u32Offset;
      jpeg->insert(jpeg->end(), begin, begin + pack.u32Len - pack.u32Offset);
    }
  }
  (void)CVI_VENC_ReleaseStream(channel, &stream);
  std::free(stream.pstPack);
  if (jpeg->empty()) {
    return MMF_ENOTREADY;
  }
  if (sequence != nullptr) {
    *sequence = ++server->display_venc_sequence;
  }
  server->display_venc_last_error = CVI_SUCCESS;
  return MMF_OK;
}

static mmf_result_t ensure_http_pull_camera(mmf_jpg_http_server_t* server) {
  if (server == nullptr) {
    return MMF_EINVAL;
  }
  if (server->pull_camera != nullptr) {
    return MMF_OK;
  }

  mmf_camera_config_t camera_cfg;
  mmf_result_t ret = mmf_camera_get_default_config(server->config.camera_source, &camera_cfg);
  if (ret != MMF_OK) {
    return ret;
  }
  if (server->config.width > 0) {
    camera_cfg.width = server->config.width;
  }
  if (server->config.height > 0) {
    camera_cfg.height = server->config.height;
  }
  return mmf_camera_open(&camera_cfg, &server->pull_camera);
}

static mmf_result_t ensure_http_pull_encoder(mmf_jpg_http_server_t* server,
                                             const mmf_video_frame_t* frame) {
  if (server == nullptr || frame == nullptr) {
    return MMF_EINVAL;
  }
  if (server->pull_encoder != nullptr && server->pull_width == frame->width &&
      server->pull_height == frame->height && server->pull_format == frame->pixel_format &&
      server->pull_quality == server->config.jpeg_quality &&
      server->pull_venc_channel == server->config.venc_channel) {
    return MMF_OK;
  }

  if (server->pull_encoder != nullptr) {
    mmf_jpg_encoder_close(server->pull_encoder);
    server->pull_encoder = nullptr;
  }

  mmf_jpg_encoder_config_t enc_cfg;
  mmf_jpg_get_default_encoder_config(&enc_cfg);
  enc_cfg.width = frame->width;
  enc_cfg.height = frame->height;
  enc_cfg.input_format = frame->pixel_format;
  enc_cfg.quality = server->config.jpeg_quality;
  enc_cfg.role = MMF_JPG_ROLE_HTTP_STREAM;
  enc_cfg.venc_channel = server->config.venc_channel;
  mmf_result_t ret = mmf_jpg_encoder_open(&enc_cfg, &server->pull_encoder);
  if (ret != MMF_OK) {
    return ret;
  }

  server->pull_width = frame->width;
  server->pull_height = frame->height;
  server->pull_format = frame->pixel_format;
  server->pull_quality = server->config.jpeg_quality;
  server->pull_venc_channel = server->config.venc_channel;
  return MMF_OK;
}

static void reset_http_pull_encoder(mmf_jpg_http_server_t* server) {
  if (server == nullptr || server->pull_encoder == nullptr) {
    return;
  }
  mmf_jpg_encoder_close(server->pull_encoder);
  server->pull_encoder = nullptr;
  server->pull_width = 0;
  server->pull_height = 0;
  server->pull_format = MMF_PIXFMT_UNKNOWN;
  server->pull_quality = 0;
  server->pull_venc_channel = 0;
}

static bool write_all(int fd, const void* data, size_t bytes) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  while (bytes > 0) {
    const ssize_t n = ::send(fd, p, bytes, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    p += n;
    bytes -= static_cast<size_t>(n);
  }
  return true;
}

static bool write_string(int fd, const std::string& text) {
  return write_all(fd, text.data(), text.size());
}

static void close_fd(int* fd) {
  if (fd != nullptr && *fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}

static std::string http_path_from_request(const char* request) {
  if (request == nullptr) {
    return "/";
  }
  const char* space = std::strchr(request, ' ');
  if (space == nullptr) {
    return "/";
  }
  const char* path = space + 1;
  const char* end = std::strchr(path, ' ');
  if (end == nullptr || end <= path) {
    return "/";
  }
  return std::string(path, end);
}

static mmf_result_t capture_http_jpeg(mmf_jpg_http_server_t* server,
                                      std::vector<std::uint8_t>* jpeg, uint64_t* sequence) {
  if (server == nullptr || jpeg == nullptr) {
    return MMF_EINVAL;
  }
  if (server->config.mode == MMF_JPG_HTTP_MODE_PUSH) {
    std::lock_guard<std::mutex> lock(server->jpeg_mutex);
    if (server->last_jpeg.empty()) {
      return MMF_ENOTREADY;
    }
    *jpeg = server->last_jpeg;
    if (sequence != nullptr) {
      *sequence = server->last_jpeg_sequence;
    }
    return MMF_OK;
  }

  std::lock_guard<std::mutex> pull_lock(server->pull_mutex);
  if (server->config.mode == MMF_JPG_HTTP_MODE_DISPLAY_PULL) {
    return capture_http_display_jpeg(server, jpeg, sequence);
  }
  mmf_result_t ret = ensure_http_pull_camera(server);
  if (ret != MMF_OK) {
    return ret;
  }

  mmf_video_frame_t frame;
  std::memset(&frame, 0, sizeof(frame));
  ret = mmf_camera_get_frame(server->pull_camera, &frame, 1000);
  if (ret == MMF_OK) {
    ret = ensure_http_pull_encoder(server, &frame);
    if (ret == MMF_OK) {
      mmf_packet_t packet;
      std::memset(&packet, 0, sizeof(packet));
      ret = mmf_jpg_encode_frame(server->pull_encoder, &frame, &packet);
      if (ret == MMF_OK && packet.data != nullptr && packet.bytes > 0) {
        const auto* begin = static_cast<const std::uint8_t*>(packet.data);
        jpeg->assign(begin, begin + packet.bytes);
        if (sequence != nullptr) {
          *sequence = packet.sequence;
        }
        mmf_jpg_release_packet(server->pull_encoder, &packet);
      } else if (ret != MMF_OK) {
        reset_http_pull_encoder(server);
      }
    }
    (void)mmf_camera_put_frame(server->pull_camera, &frame);
  }
  return ret;
}

static bool wait_latest_http_jpeg(mmf_jpg_http_server_t* server, uint64_t last_sequence,
                                  std::vector<std::uint8_t>* jpeg, uint64_t* sequence) {
  if (server == nullptr || jpeg == nullptr || sequence == nullptr) {
    return false;
  }
  std::unique_lock<std::mutex> lock(server->jpeg_mutex);
  server->jpeg_cv.wait_for(lock, std::chrono::seconds(2), [server, last_sequence]() {
    return server->stop.load() || !server->streaming.load() ||
           (!server->last_jpeg.empty() && server->last_jpeg_sequence != last_sequence);
  });
  if (server->stop.load() || !server->streaming.load() || server->last_jpeg.empty() ||
      server->last_jpeg_sequence == last_sequence) {
    return false;
  }
  *jpeg = server->last_jpeg;
  *sequence = server->last_jpeg_sequence;
  return true;
}

static void publish_http_jpeg_cache(mmf_jpg_http_server_t* server, const void* jpeg_data,
                                    size_t jpeg_bytes, uint64_t sequence, uint64_t timestamp_us) {
  if (server == nullptr || jpeg_data == nullptr || jpeg_bytes == 0) {
    return;
  }
  const auto* data = static_cast<const uint8_t*>(jpeg_data);
  {
    std::lock_guard<std::mutex> lock(server->jpeg_mutex);
    server->last_jpeg.assign(data, data + jpeg_bytes);
    server->last_jpeg_sequence = sequence;
    server->last_jpeg_timestamp_us = timestamp_us;
    server->last_frame_sequence.store(sequence);
  }
  server->jpeg_cv.notify_all();
}

static void http_producer_thread(mmf_jpg_http_server_t* server) {
  if (server == nullptr || server->config.mode == MMF_JPG_HTTP_MODE_PUSH) {
    return;
  }
  const uint32_t fps = server->config.fps;
  const auto interval =
      fps == 0 ? std::chrono::milliseconds(0) : std::chrono::milliseconds(1000 / fps);
  while (!server->stop.load() && server->streaming.load()) {
    if (server->client_count.load() == 0) {
      if (server->config.mode == MMF_JPG_HTTP_MODE_DISPLAY_PULL) {
        std::lock_guard<std::mutex> lock(server->pull_mutex);
        close_http_display_venc(server);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    std::vector<std::uint8_t> jpeg;
    uint64_t sequence = 0;
    const mmf_result_t ret = capture_http_jpeg(server, &jpeg, &sequence);
    if (ret == MMF_OK && !jpeg.empty()) {
      publish_http_jpeg_cache(server, jpeg.data(), jpeg.size(), sequence, 0);
    } else if (ret == MMF_EBUSY || ret == MMF_ENOTREADY) {
      std::this_thread::sleep_for(std::chrono::milliseconds(
          server->config.mode == MMF_JPG_HTTP_MODE_DISPLAY_PULL ? 5 : 100));
    }
    if (server->config.mode != MMF_JPG_HTTP_MODE_DISPLAY_PULL && interval.count() > 0) {
      std::this_thread::sleep_for(interval);
    }
  }
}

static bool send_snapshot_response(mmf_jpg_http_server_t* server, int fd) {
  std::vector<std::uint8_t> jpeg;
  uint64_t sequence = 0;
  bool ready = false;
  if (server->config.mode == MMF_JPG_HTTP_MODE_PUSH) {
    ready = capture_http_jpeg(server, &jpeg, &sequence) == MMF_OK;
  } else {
    ready = wait_latest_http_jpeg(server, static_cast<uint64_t>(-1), &jpeg, &sequence);
  }
  if (!ready || jpeg.empty()) {
    const char body[] = "snapshot not ready\n";
    write_string(fd,
                 "HTTP/1.1 503 Service Unavailable\r\n"
                 "Content-Type: text/plain\r\n"
                 "Connection: close\r\n"
                 "Content-Length: 19\r\n\r\n");
    write_all(fd, body, sizeof(body) - 1);
    return false;
  }

  char header[256];
  const int n = std::snprintf(header, sizeof(header),
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: image/jpeg\r\n"
                              "Cache-Control: no-store\r\n"
                              "Connection: close\r\n"
                              "X-Frame-Sequence: %llu\r\n"
                              "Content-Length: %zu\r\n\r\n",
                              static_cast<unsigned long long>(sequence), jpeg.size());
  if (n <= 0) {
    return false;
  }
  if (!write_all(fd, header, static_cast<size_t>(n))) {
    return false;
  }
  const bool ok = write_all(fd, jpeg.data(), jpeg.size());
  if (ok) {
    server->frames_published.fetch_add(1);
    server->bytes_published.fetch_add(jpeg.size());
    server->last_frame_sequence.store(sequence);
  }
  return ok;
}

static void stream_client(mmf_jpg_http_server_t* server, int fd) {
  const char* header =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=mmfjpg\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n\r\n";
  if (!write_string(fd, header)) {
    close_fd(&fd);
    server->client_count.fetch_sub(1);
    return;
  }
  uint64_t sent_sequence = 0;
  while (server->streaming.load() && !server->stop.load()) {
    std::vector<std::uint8_t> jpeg;
    uint64_t sequence = 0;
    if (wait_latest_http_jpeg(server, sent_sequence, &jpeg, &sequence) && !jpeg.empty()) {
      char part[256];
      const int n = std::snprintf(part, sizeof(part),
                                  "--mmfjpg\r\n"
                                  "Content-Type: image/jpeg\r\n"
                                  "X-Frame-Sequence: %llu\r\n"
                                  "Content-Length: %zu\r\n\r\n",
                                  static_cast<unsigned long long>(sequence), jpeg.size());
      if (n <= 0 || !write_all(fd, part, static_cast<size_t>(n)) ||
          !write_all(fd, jpeg.data(), jpeg.size()) || !write_string(fd, "\r\n")) {
        break;
      }
      server->frames_published.fetch_add(1);
      server->bytes_published.fetch_add(jpeg.size());
      server->last_frame_sequence.store(sequence);
      sent_sequence = sequence;
    }
  }
  close_fd(&fd);
  server->client_count.fetch_sub(1);
}

static void send_index_response(int fd, const char* stream_path) {
  const std::string body =
      "<!doctype html><html><head><meta charset=\"utf-8\">"
      "<title>MJPEG stream</title><style>"
      "body{margin:0;background:#111;display:flex;min-height:100vh;"
      "align-items:center;justify-content:center}"
      "img{max-width:100vw;max-height:100vh}</style></head><body><img src=\"" +
      std::string(stream_path) + "\" alt=\"stream\"></body></html>";
  const std::string header =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n"
      "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  write_string(fd, header);
  write_string(fd, body);
}

static void handle_http_client(mmf_jpg_http_server_t* server, int fd) {
  timeval tv;
  tv.tv_sec = 3;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  char request[1024];
  const ssize_t n = ::recv(fd, request, sizeof(request) - 1, 0);
  if (n <= 0) {
    close_fd(&fd);
    server->client_count.fetch_sub(1);
    return;
  }
  request[n] = '\0';
  const std::string path = http_path_from_request(request);
  const char* snapshot_path =
      server->config.snapshot_path ? server->config.snapshot_path : "/snapshot.jpg";
  const char* stream_path =
      server->config.stream_path ? server->config.stream_path : "/stream.mjpg";

  if (path == "/" || path == "/index.html") {
    send_index_response(fd, stream_path);
    close_fd(&fd);
    server->client_count.fetch_sub(1);
    return;
  }
  if (path == snapshot_path) {
    send_snapshot_response(server, fd);
    close_fd(&fd);
    server->client_count.fetch_sub(1);
    return;
  }
  if (path == stream_path) {
    stream_client(server, fd);
    return;
  }

  const char body[] = "not found\n";
  write_string(fd,
               "HTTP/1.1 404 Not Found\r\n"
               "Content-Type: text/plain\r\n"
               "Connection: close\r\n"
               "Content-Length: 10\r\n\r\n");
  write_all(fd, body, sizeof(body) - 1);
  close_fd(&fd);
  server->client_count.fetch_sub(1);
}

static void spawn_http_client(mmf_jpg_http_server_t* server, int fd) {
  if (server == nullptr || fd < 0) {
    close_fd(&fd);
    return;
  }
  server->client_count.fetch_add(1);
  try {
    std::thread(handle_http_client, server, fd).detach();
  } catch (...) {
    close_fd(&fd);
    server->client_count.fetch_sub(1);
  }
}

static void http_server_thread(mmf_jpg_http_server_t* server) {
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    set_last_error("jpg http socket failed");
    server->streaming.store(false);
    return;
  }
  server->listen_fd = listen_fd;

  int on = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(server->config.port);
  const char* bind_ip = server->config.bind_ip ? server->config.bind_ip : "0.0.0.0";
  if (::inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }

  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(listen_fd, 8) < 0) {
    set_last_error("jpg http bind/listen failed");
    server->streaming.store(false);
    close_fd(&server->listen_fd);
    return;
  }

  int epfd = ::epoll_create1(0);
  if (epfd < 0) {
    set_last_error("jpg http epoll_create1 failed");
    server->streaming.store(false);
    close_fd(&server->listen_fd);
    return;
  }
  epoll_event event;
  std::memset(&event, 0, sizeof(event));
  event.events = EPOLLIN;
  event.data.fd = listen_fd;
  if (::epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &event) < 0) {
    set_last_error("jpg http epoll_ctl failed");
    server->streaming.store(false);
    ::close(epfd);
    close_fd(&server->listen_fd);
    return;
  }

  while (!server->stop.load()) {
    epoll_event events[4];
    const int ready = ::epoll_wait(epfd, events, 4, 500);
    if (ready <= 0) {
      continue;
    }
    for (int i = 0; i < ready; ++i) {
      if (events[i].data.fd != listen_fd) {
        continue;
      }
      int client = ::accept(listen_fd, nullptr, nullptr);
      if (client < 0) {
        continue;
      }
      spawn_http_client(server, client);
    }
  }
  ::close(epfd);
  close_fd(&server->listen_fd);
}

extern "C" {

void mmf_jpg_http_get_default_config(mmf_jpg_http_config_t* config) {
  if (config == nullptr)
    return;
  std::memset(config, 0, sizeof(*config));
  config->bind_ip = "0.0.0.0";
  config->port = 8080;
  config->snapshot_path = "/snapshot.jpg";
  config->stream_path = "/stream.mjpg";
  config->mode = MMF_JPG_HTTP_MODE_DISPLAY_PULL;
  config->camera_source = MMF_CAMERA_SRC_SCREEN;
  config->width = mmf_cvi::DualOsLayout::kScreenWidth;
  config->height = mmf_cvi::DualOsLayout::kScreenHeight;
  config->fps = 10;
  config->jpeg_quality = 92;
  config->cache_max_frames = 2;
  config->venc_channel = kJpegHttpVencChannel;
}

mmf_result_t mmf_jpg_http_open(const mmf_jpg_http_config_t* config,
                               mmf_jpg_http_server_t** server) {
  if (config == nullptr || server == nullptr)
    return MMF_EINVAL;
  std::unique_ptr<mmf_jpg_http_server_t> ptr(new mmf_jpg_http_server_t);
  ptr->config = *config;
  *server = ptr.release();
  return MMF_OK;
}

void mmf_jpg_http_close(mmf_jpg_http_server_t* server) {
  if (server == nullptr)
    return;
  mmf_jpg_http_stop_stream(server);
  delete server;
}

mmf_result_t mmf_jpg_http_start_stream(mmf_jpg_http_server_t* server) {
  if (server == nullptr)
    return MMF_EINVAL;
  if (server->streaming.load())
    return MMF_OK;
  server->stop.store(false);
  server->streaming.store(true);
  try {
    server->worker = std::thread(http_server_thread, server);
    server->producer = std::thread(http_producer_thread, server);
  } catch (...) {
    server->streaming.store(false);
    server->stop.store(true);
    if (server->worker.joinable()) {
      server->worker.join();
    }
    set_last_error("failed to create jpg http worker thread");
    return MMF_ENOMEM;
  }
  return MMF_OK;
}

mmf_result_t mmf_jpg_http_stop_stream(mmf_jpg_http_server_t* server) {
  if (server == nullptr)
    return MMF_EINVAL;
  server->stop.store(true);
  server->streaming.store(false);
  if (server->listen_fd >= 0) {
    ::shutdown(server->listen_fd, SHUT_RDWR);
  }
  server->jpeg_cv.notify_all();
  if (server->worker.joinable()) {
    server->worker.join();
  }
  if (server->producer.joinable()) {
    server->producer.join();
  }
  for (int i = 0; i < 60 && server->client_count.load() > 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  close_http_pull_resources(server);
  return MMF_OK;
}

mmf_result_t mmf_jpg_http_get_status(mmf_jpg_http_server_t* server, mmf_jpg_http_status_t* status) {
  if (server == nullptr || status == nullptr)
    return MMF_EINVAL;
  std::memset(status, 0, sizeof(*status));
  status->opened = MMF_TRUE;
  status->port = server->config.port;
  status->client_count = server->client_count.load();
  status->frames_published = server->frames_published.load();
  status->bytes_published = server->bytes_published.load();
  status->last_frame_sequence = server->last_frame_sequence.load();
  status->streaming = server->streaming.load() ? MMF_TRUE : MMF_FALSE;
  return MMF_OK;
}

mmf_result_t mmf_jpg_http_publish_jpeg(mmf_jpg_http_server_t* server, const void* jpeg_data,
                                       size_t jpeg_bytes, uint64_t sequence,
                                       uint64_t timestamp_us) {
  if (server == nullptr || (jpeg_data == nullptr && jpeg_bytes > 0))
    return MMF_EINVAL;
  publish_http_jpeg_cache(server, jpeg_data, jpeg_bytes, sequence, timestamp_us);
  return MMF_OK;
}

mmf_result_t mmf_jpg_http_publish_frame(mmf_jpg_http_server_t* server,
                                        const mmf_video_frame_t* frame, uint32_t jpeg_quality) {
  if (server == nullptr || frame == nullptr)
    return MMF_EINVAL;
  mmf_packet_t jpeg;
  std::memset(&jpeg, 0, sizeof(jpeg));
  mmf_jpg_encoder_config_t cfg;
  mmf_jpg_encoder_t* encoder = nullptr;
  mmf_jpg_get_default_encoder_config(&cfg);
  cfg.width = frame->width;
  cfg.height = frame->height;
  cfg.input_format = frame->pixel_format;
  cfg.quality = jpeg_quality;
  cfg.role = MMF_JPG_ROLE_HTTP_STREAM;
  cfg.venc_channel = server->config.venc_channel;
  mmf_result_t ret = mmf_jpg_encoder_open(&cfg, &encoder);
  if (ret == MMF_OK)
    ret = mmf_jpg_encode_frame(encoder, frame, &jpeg);
  if (ret == MMF_OK) {
    ret = mmf_jpg_http_publish_jpeg(server, jpeg.data, jpeg.bytes, frame->sequence,
                                    frame->timestamp_us);
  }
  if (encoder != nullptr) {
    mmf_jpg_release_packet(encoder, &jpeg);
    mmf_jpg_encoder_close(encoder);
  }
  return ret;
}

}  // extern "C"
