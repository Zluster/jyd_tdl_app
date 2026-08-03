#ifndef MMF_AUDIO_CODEC_H
#define MMF_AUDIO_CODEC_H

#include "mmf_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmf_audio_encoder mmf_audio_encoder_t;
typedef struct mmf_audio_decoder mmf_audio_decoder_t;

typedef struct {
  mmf_codec_t codec;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t bitrate;
  uint32_t channel_id;
} mmf_audio_codec_config_t;

mmf_result_t mmf_audio_encoder_open(const mmf_audio_codec_config_t* config,
                                    mmf_audio_encoder_t** encoder);
void mmf_audio_encoder_close(mmf_audio_encoder_t* encoder);
mmf_result_t mmf_audio_encoder_encode(mmf_audio_encoder_t* encoder, const mmf_audio_frame_t* pcm,
                                      mmf_packet_t* packet);
mmf_result_t mmf_audio_encoder_release(mmf_audio_encoder_t* encoder, mmf_packet_t* packet);

mmf_result_t mmf_audio_decoder_open(const mmf_audio_codec_config_t* config,
                                    mmf_audio_decoder_t** decoder);
void mmf_audio_decoder_close(mmf_audio_decoder_t* decoder);
mmf_result_t mmf_audio_decoder_decode(mmf_audio_decoder_t* decoder, const mmf_packet_t* packet,
                                      mmf_audio_frame_t* pcm);
mmf_result_t mmf_audio_decoder_release(mmf_audio_decoder_t* decoder, mmf_audio_frame_t* pcm);

#ifdef __cplusplus
}
#endif

#endif
