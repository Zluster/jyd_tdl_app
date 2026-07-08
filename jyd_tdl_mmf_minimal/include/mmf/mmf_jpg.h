#ifndef MMF_JPG_H
#define MMF_JPG_H

#include "mmf_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmf_jpg_encoder mmf_jpg_encoder_t;
typedef struct mmf_jpg_decoder mmf_jpg_decoder_t;

typedef enum {
  MMF_JPG_ROLE_ONESHOT = 0,
  MMF_JPG_ROLE_SNAPSHOT = 1,
  MMF_JPG_ROLE_HTTP_STREAM = 2,
} mmf_jpg_role_t;

typedef struct {
  uint32_t width;
  uint32_t height;
  mmf_pixel_format_t input_format;
  uint32_t quality;
  uint32_t venc_channel;
  mmf_jpg_role_t role;
  mmf_bool_t exclusive_channel;
} mmf_jpg_encoder_config_t;

typedef struct {
  uint32_t max_width;
  uint32_t max_height;
  mmf_pixel_format_t output_format;
  uint32_t vdec_channel;
} mmf_jpg_decoder_config_t;

void mmf_jpg_get_default_encoder_config(mmf_jpg_encoder_config_t* config);
void mmf_jpg_get_default_decoder_config(mmf_jpg_decoder_config_t* config);

mmf_result_t mmf_jpg_encoder_open(const mmf_jpg_encoder_config_t* config,
                                  mmf_jpg_encoder_t** encoder);
void mmf_jpg_encoder_close(mmf_jpg_encoder_t* encoder);
mmf_result_t mmf_jpg_encode_frame(mmf_jpg_encoder_t* encoder, const mmf_video_frame_t* frame,
                                  mmf_packet_t* jpeg);
mmf_result_t mmf_jpg_release_packet(mmf_jpg_encoder_t* encoder, mmf_packet_t* jpeg);

mmf_result_t mmf_jpg_decoder_open(const mmf_jpg_decoder_config_t* config,
                                  mmf_jpg_decoder_t** decoder);
void mmf_jpg_decoder_close(mmf_jpg_decoder_t* decoder);
mmf_result_t mmf_jpg_decode_packet(mmf_jpg_decoder_t* decoder, const mmf_packet_t* jpeg,
                                   mmf_video_frame_t* frame);
mmf_result_t mmf_jpg_release_frame(mmf_jpg_decoder_t* decoder, mmf_video_frame_t* frame);

mmf_result_t mmf_jpg_encode(const mmf_video_frame_t* frame, uint32_t quality, mmf_packet_t* jpeg);
mmf_result_t mmf_jpg_decode(const void* jpeg_data, size_t jpeg_bytes,
                            mmf_pixel_format_t output_format, mmf_video_frame_t* frame);

#ifdef __cplusplus
}
#endif

#endif
