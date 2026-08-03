#include "mmf/mmf.h"

int main(void) {
  mmf_audio_input_config_t input_cfg;
  mmf_audio_output_config_t output_cfg;
  mmf_audio_input_t* input = 0;
  mmf_audio_output_t* output = 0;

  mmf_audio_get_default_input_config(&input_cfg);
  mmf_audio_get_default_output_config(&output_cfg);

  input_cfg.io.sample_rate = 16000;
  input_cfg.io.channels = 1;
  input_cfg.enable_3a = MMF_TRUE;

  output_cfg.io.sample_rate = 16000;
  output_cfg.io.channels = 1;
  output_cfg.provide_aec_reference = MMF_TRUE;

  if (mmf_audio_input_open(&input_cfg, &input) != MMF_OK) {
    return 1;
  }
  if (mmf_audio_output_open(&output_cfg, &output) != MMF_OK) {
    mmf_audio_input_close(input);
    return 2;
  }

  mmf_audio_output_close(output);
  mmf_audio_input_close(input);
  return 0;
}
