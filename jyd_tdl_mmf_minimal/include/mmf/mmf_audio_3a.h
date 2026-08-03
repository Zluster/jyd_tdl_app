#ifndef MMF_AUDIO_3A_H
#define MMF_AUDIO_3A_H

#include "mmf_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  mmf_bool_t aec_enable;
  mmf_bool_t ns_enable;
  mmf_bool_t agc_enable;
  uint32_t aec_level;
  int32_t aec_delay_ms;
  uint32_t ns_level;
  int32_t agc_target_db;
  uint32_t agc_max_gain;
  uint32_t agc_compress;
  uint32_t vendor_filter_len;
  uint32_t vendor_std_thrd;
  uint32_t vendor_supp_coeff;
  uint32_t vendor_snr_coeff;
  mmf_bool_t vendor_vad_enable;
} mmf_audio_3a_config_t;

typedef struct {
  mmf_bool_t supported;
  mmf_bool_t applied;
  mmf_bool_t has_playback_reference;
  mmf_audio_3a_config_t config;
} mmf_audio_3a_status_t;

void mmf_audio_3a_get_default_config(mmf_audio_3a_config_t* config);
mmf_result_t mmf_audio_3a_set_aec(mmf_bool_t enable);
mmf_result_t mmf_audio_3a_get_aec(mmf_bool_t* enable);
mmf_result_t mmf_audio_3a_set_ns(mmf_bool_t enable);
mmf_result_t mmf_audio_3a_get_ns(mmf_bool_t* enable);
mmf_result_t mmf_audio_3a_set_agc(mmf_bool_t enable);
mmf_result_t mmf_audio_3a_get_agc(mmf_bool_t* enable);
mmf_result_t mmf_audio_3a_set_aec_level(uint32_t level);
mmf_result_t mmf_audio_3a_get_aec_level(uint32_t* level);
mmf_result_t mmf_audio_3a_set_aec_delay(int32_t delay_ms);
mmf_result_t mmf_audio_3a_get_aec_delay(int32_t* delay_ms);
mmf_result_t mmf_audio_3a_set_ns_level(uint32_t level);
mmf_result_t mmf_audio_3a_get_ns_level(uint32_t* level);
mmf_result_t mmf_audio_3a_set_agc_target(int32_t target_db);
mmf_result_t mmf_audio_3a_get_agc_target(int32_t* target_db);
mmf_result_t mmf_audio_3a_set_agc_max_gain(uint32_t max_gain);
mmf_result_t mmf_audio_3a_get_agc_max_gain(uint32_t* max_gain);
mmf_result_t mmf_audio_3a_set_agc_compress(uint32_t compress);
mmf_result_t mmf_audio_3a_get_agc_compress(uint32_t* compress);
mmf_result_t mmf_audio_3a_get_status(mmf_audio_3a_status_t* status);

#ifdef __cplusplus
}
#endif

#endif
