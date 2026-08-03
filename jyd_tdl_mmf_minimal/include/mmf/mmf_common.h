#ifndef MMF_COMMON_H
#define MMF_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MMF_API_VERSION_MAJOR 0
#define MMF_API_VERSION_MINOR 3
#define MMF_API_VERSION_PATCH 0

typedef enum {
  MMF_OK = 0,
  MMF_EINVAL = -22,
  MMF_ENOMEM = -12,
  MMF_ENOTSUP = -95,
  MMF_EBUSY = -16,
  MMF_ETIMEOUT = -110,
  MMF_EIO = -5,
  MMF_EAGAIN = -11,
  MMF_ENOTREADY = -1001,
  MMF_ESTATE = -1002,
} mmf_result_t;

typedef enum {
  MMF_FALSE = 0,
  MMF_TRUE = 1,
} mmf_bool_t;

typedef enum {
  MMF_PIXFMT_UNKNOWN = 0,
  MMF_PIXFMT_NV12 = 1,
  MMF_PIXFMT_NV21 = 2,
  MMF_PIXFMT_RGB888 = 3,
  MMF_PIXFMT_BGR888 = 4,
  MMF_PIXFMT_GRAY8 = 5,
  MMF_PIXFMT_RGB888_PLANAR = 6,
  MMF_PIXFMT_ARGB8888 = 7,
  MMF_PIXFMT_JPEG = 100,
} mmf_pixel_format_t;

typedef enum {
  MMF_SCALE_FIT_BLACK = 0,
  MMF_SCALE_CENTER_CROP = 1,
  MMF_SCALE_STRETCH = 2,
} mmf_scale_mode_t;

typedef enum {
  MMF_ROTATE_0 = 0,
  MMF_ROTATE_90 = 1,
  MMF_ROTATE_180 = 2,
  MMF_ROTATE_270 = 3,
} mmf_rotate_t;

typedef enum {
  MMF_AUDIO_FMT_UNKNOWN = 0,
  MMF_AUDIO_FMT_S16_LE = 1,
  MMF_AUDIO_FMT_S24_LE = 2,
  MMF_AUDIO_FMT_S32_LE = 3,
} mmf_audio_format_t;

typedef enum {
  MMF_CODEC_NONE = 0,
  MMF_CODEC_JPEG = 1,
  MMF_CODEC_H264 = 2,
  MMF_CODEC_H265 = 3,
  MMF_CODEC_G711A = 100,
  MMF_CODEC_G711U = 101,
  MMF_CODEC_AAC = 102,
} mmf_codec_t;

typedef struct {
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
} mmf_rect_t;

typedef struct {
  int32_t module_id;
  int32_t device_id;
  int32_t channel_id;
} mmf_mmf_channel_t;

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t stride[3];
  uint64_t sequence;
  uint64_t timestamp_us;
  mmf_pixel_format_t pixel_format;
  void* plane[3];
  size_t plane_bytes[3];
  void* priv;
} mmf_video_frame_t;

typedef struct {
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t samples_per_frame;
  uint64_t sequence;
  uint64_t timestamp_us;
  mmf_audio_format_t format;
  void* data;
  size_t bytes;
  void* priv;
} mmf_audio_frame_t;

typedef struct {
  const void* data;
  size_t bytes;
  mmf_codec_t codec;
  uint64_t sequence;
  uint64_t timestamp_us;
  void* priv;
} mmf_packet_t;

typedef struct {
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  const char* git_version;
} mmf_version_t;

#ifdef __cplusplus
}
#endif

#endif
