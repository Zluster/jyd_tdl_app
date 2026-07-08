#ifndef MMF_AUDIO_H
#define MMF_AUDIO_H

#include "mmf_audio_3a.h"
#include "mmf_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmf_audio_input mmf_audio_input_t;
typedef struct mmf_audio_output mmf_audio_output_t;

typedef struct {
  uint32_t sample_rate;
  uint32_t channels;
  mmf_audio_format_t format;
  uint32_t samples_per_frame;
  uint32_t frame_count;
  uint32_t timeout_ms;
  int ai_device;
  int ai_channel;
  int ao_device;
  int ao_channel;
  int input_volume;
  int output_volume;
} mmf_audio_io_config_t;

typedef struct {
  mmf_audio_io_config_t io;
  mmf_bool_t enable_3a;
  mmf_audio_3a_config_t config_3a;
  int aec_reference_device;
  int aec_reference_channel;
} mmf_audio_input_config_t;

typedef struct {
  mmf_audio_io_config_t io;
  mmf_bool_t provide_aec_reference;
} mmf_audio_output_config_t;

typedef struct {
  mmf_bool_t opened;
  mmf_bool_t running;
  mmf_bool_t enable_3a;
  uint32_t xrun_count;
  uint64_t frames_processed;
} mmf_audio_stream_status_t;

mmf_result_t mmf_audio_input_open(const mmf_audio_input_config_t* config,
                                  mmf_audio_input_t** input);
void mmf_audio_input_close(mmf_audio_input_t* input);
mmf_result_t mmf_audio_input_read(mmf_audio_input_t* input, mmf_audio_frame_t* frame,
                                  uint32_t timeout_ms);
mmf_result_t mmf_audio_input_release(mmf_audio_input_t* input, mmf_audio_frame_t* frame);
mmf_result_t mmf_audio_input_get_status(mmf_audio_input_t* input,
                                        mmf_audio_stream_status_t* status);
mmf_result_t mmf_audio_input_set_3a(mmf_audio_input_t* input, const mmf_audio_3a_config_t* config);
mmf_result_t mmf_audio_input_get_3a(mmf_audio_input_t* input, mmf_audio_3a_config_t* config);
mmf_result_t mmf_audio_input_set_volume(mmf_audio_input_t* input, int volume);
mmf_result_t mmf_audio_input_get_volume(mmf_audio_input_t* input, int* volume);

mmf_result_t mmf_audio_output_open(const mmf_audio_output_config_t* config,
                                   mmf_audio_output_t** output);
void mmf_audio_output_close(mmf_audio_output_t* output);
mmf_result_t mmf_audio_output_write(mmf_audio_output_t* output, const mmf_audio_frame_t* frame,
                                    uint32_t timeout_ms);
mmf_result_t mmf_audio_output_drain(mmf_audio_output_t* output);
mmf_result_t mmf_audio_output_get_status(mmf_audio_output_t* output,
                                         mmf_audio_stream_status_t* status);
mmf_result_t mmf_audio_output_set_volume(mmf_audio_output_t* output, int volume);
mmf_result_t mmf_audio_output_get_volume(mmf_audio_output_t* output, int* volume);

void mmf_audio_get_default_3a(mmf_audio_3a_config_t* config);
void mmf_audio_get_default_input_config(mmf_audio_input_config_t* config);
void mmf_audio_get_default_output_config(mmf_audio_output_config_t* config);

#ifdef __cplusplus
}
#endif

#endif
