#ifndef MMF_JPG_HTTP_H
#define MMF_JPG_HTTP_H

#include "mmf_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmf_jpg_http_server mmf_jpg_http_server_t;

typedef enum {
  MMF_JPG_HTTP_MODE_CAMERA_PULL = 0,
  MMF_JPG_HTTP_MODE_DISPLAY_PULL = 1,
  MMF_JPG_HTTP_MODE_PUSH = 2,
} mmf_jpg_http_mode_t;

typedef struct {
  const char* bind_ip;
  uint16_t port;
  const char* snapshot_path;
  const char* stream_path;
  mmf_jpg_http_mode_t mode;
  mmf_camera_source_t camera_source;
  uint32_t width;
  uint32_t height;
  uint32_t fps;
  uint32_t jpeg_quality;
  uint32_t cache_max_frames;
  uint32_t venc_channel;
} mmf_jpg_http_config_t;

typedef struct {
  mmf_bool_t opened;
  uint16_t port;
  uint32_t client_count;
  uint64_t frames_published;
  uint64_t bytes_published;
  uint64_t last_frame_sequence;
  mmf_bool_t streaming;
} mmf_jpg_http_status_t;

void mmf_jpg_http_get_default_config(mmf_jpg_http_config_t* config);
mmf_result_t mmf_jpg_http_open(const mmf_jpg_http_config_t* config, mmf_jpg_http_server_t** server);
void mmf_jpg_http_close(mmf_jpg_http_server_t* server);
mmf_result_t mmf_jpg_http_start_stream(mmf_jpg_http_server_t* server);
mmf_result_t mmf_jpg_http_stop_stream(mmf_jpg_http_server_t* server);
mmf_result_t mmf_jpg_http_get_status(mmf_jpg_http_server_t* server, mmf_jpg_http_status_t* status);
mmf_result_t mmf_jpg_http_publish_jpeg(mmf_jpg_http_server_t* server, const void* jpeg_data,
                                       size_t jpeg_bytes, uint64_t sequence, uint64_t timestamp_us);
mmf_result_t mmf_jpg_http_publish_frame(mmf_jpg_http_server_t* server,
                                        const mmf_video_frame_t* frame, uint32_t jpeg_quality);

#ifdef __cplusplus
}
#endif

#endif
