#include "mmf_cv184x_common.hpp"
#include "mmf_cv184x_resources.hpp"

using namespace mmf_cv184x;

struct mmf_audio_input {
  explicit mmf_audio_input(const mmf_audio_input_config_t& cfg)
      : config(cfg), input(to_input_config(cfg)) {}
  mmf_audio_input_config_t config;
  mmf_cvi::AudioInput input;
  mmf_cvi::AudioFrame frame;
  std::vector<std::uint8_t> interleaved;
  uint64_t frames_read = 0;
  bool talk_vqe_configured = false;
};

struct mmf_audio_output {
  explicit mmf_audio_output(const mmf_audio_output_config_t& cfg)
      : config(cfg), output(to_output_config(cfg)) {}
  mmf_audio_output_config_t config;
  mmf_cvi::AudioOutput output;
  uint64_t frames_written = 0;
};

extern "C" {

mmf_result_t mmf_audio_input_open(const mmf_audio_input_config_t* config,
                                  mmf_audio_input_t** input) {
  if (config == nullptr || input == nullptr)
    return MMF_EINVAL;
  std::unique_ptr<mmf_audio_input_t> ptr(new mmf_audio_input_t(*config));
  std::string error;
  if (!ptr->input.open(&error))
    return ok_or_error(false, error);
  if (config->enable_3a) {
    mmf_cvi::AudioTalkVqeConfig vqe = to_talk_vqe(config->config_3a, config->io.sample_rate);
    ptr->talk_vqe_configured = ptr->input.configureTalkVqe(vqe, config->aec_reference_device,
                                                           config->aec_reference_channel, &error);
  }
  audio_3a_note_input_open(ptr->config.config_3a, ptr->config.enable_3a == MMF_TRUE,
                           ptr->talk_vqe_configured);
  *input = ptr.release();
  return MMF_OK;
}

void mmf_audio_input_close(mmf_audio_input_t* input) {
  if (input == nullptr)
    return;
  audio_3a_note_input_close(input->config.enable_3a == MMF_TRUE, input->talk_vqe_configured);
  input->input.close();
  delete input;
}

mmf_result_t mmf_audio_input_read(mmf_audio_input_t* input, mmf_audio_frame_t* frame,
                                  uint32_t timeout_ms) {
  if (input == nullptr || frame == nullptr)
    return MMF_EINVAL;
  std::string error;
  if (!input->input.readFrame(&input->frame, static_cast<int>(timeout_ms), &error)) {
    return ok_or_error(false, error);
  }
  if (input->frame.channels.empty())
    return MMF_EIO;
  const size_t channel_count = input->frame.channels.size();
  const mmf_audio_format_t fmt = from_native_bit_width(input->frame.bit_width);
  const size_t sample_bytes = bytes_per_sample(fmt);
  const size_t samples = input->frame.channels[0].size() / sample_bytes;
  input->interleaved.assign(samples * channel_count * sample_bytes, 0);
  for (size_t sample = 0; sample < samples; ++sample) {
    for (size_t ch = 0; ch < channel_count; ++ch) {
      std::memcpy(input->interleaved.data() + (sample * channel_count + ch) * sample_bytes,
                  input->frame.channels[ch].data() + sample * sample_bytes, sample_bytes);
    }
  }
  std::memset(frame, 0, sizeof(*frame));
  frame->sample_rate = input->config.io.sample_rate;
  frame->channels = static_cast<uint32_t>(channel_count);
  frame->samples_per_frame = static_cast<uint32_t>(samples);
  frame->format = fmt;
  frame->sequence = input->frame.sequence;
  frame->timestamp_us = input->frame.timestamp;
  frame->data = input->interleaved.data();
  frame->bytes = input->interleaved.size();
  input->frames_read += 1;
  return MMF_OK;
}

mmf_result_t mmf_audio_input_release(mmf_audio_input_t* input, mmf_audio_frame_t* frame) {
  (void)input;
  if (frame != nullptr) {
    frame->data = nullptr;
    frame->bytes = 0;
  }
  return MMF_OK;
}

mmf_result_t mmf_audio_input_get_status(mmf_audio_input_t* input,
                                        mmf_audio_stream_status_t* status) {
  if (input == nullptr || status == nullptr)
    return MMF_EINVAL;
  std::memset(status, 0, sizeof(*status));
  status->opened = MMF_TRUE;
  status->running = MMF_TRUE;
  status->enable_3a = input->config.enable_3a;
  status->frames_processed = input->frames_read;
  return MMF_OK;
}

mmf_result_t mmf_audio_input_set_3a(mmf_audio_input_t* input, const mmf_audio_3a_config_t* config) {
  if (input == nullptr || config == nullptr)
    return MMF_EINVAL;
  const mmf_bool_t enable_3a =
      (config->aec_enable || config->ns_enable || config->agc_enable) ? MMF_TRUE : MMF_FALSE;
  if (enable_3a == MMF_FALSE) {
    input->config.config_3a = *config;
    input->config.enable_3a = MMF_FALSE;
    audio_3a_note_input_config(*config, false, false);
    input->talk_vqe_configured = false;
    return MMF_OK;
  }

  if (input->talk_vqe_configured && input->config.enable_3a == MMF_TRUE &&
      std::memcmp(&input->config.config_3a, config, sizeof(*config)) == 0) {
    return MMF_OK;
  }

  std::string error;
  mmf_cvi::AudioTalkVqeConfig vqe = to_talk_vqe(*config, input->config.io.sample_rate);
  const bool ok = input->input.configureTalkVqe(vqe, input->config.aec_reference_device,
                                                input->config.aec_reference_channel, &error);
  if (!ok) {
    return ok_or_error(false, error);
  }
  input->config.config_3a = *config;
  input->config.enable_3a = MMF_TRUE;
  input->talk_vqe_configured = true;
  audio_3a_note_input_config(*config, true, true);
  return MMF_OK;
}

mmf_result_t mmf_audio_input_get_3a(mmf_audio_input_t* input, mmf_audio_3a_config_t* config) {
  if (input == nullptr || config == nullptr)
    return MMF_EINVAL;
  *config = input->config.config_3a;
  return MMF_OK;
}

mmf_result_t mmf_audio_input_set_volume(mmf_audio_input_t* input, int volume) {
  if (input == nullptr)
    return MMF_EINVAL;
  std::string error;
  if (!input->input.setVolume(volume, &error))
    return ok_or_error(false, error);
  input->config.io.input_volume = volume;
  return MMF_OK;
}

mmf_result_t mmf_audio_input_get_volume(mmf_audio_input_t* input, int* volume) {
  if (input == nullptr || volume == nullptr)
    return MMF_EINVAL;
  std::string error;
  return ok_or_error(input->input.getVolume(volume, &error), error);
}

mmf_result_t mmf_audio_output_open(const mmf_audio_output_config_t* config,
                                   mmf_audio_output_t** output) {
  if (config == nullptr || output == nullptr)
    return MMF_EINVAL;
  std::unique_ptr<mmf_audio_output_t> ptr(new mmf_audio_output_t(*config));
  std::string error;
  if (!ptr->output.open(&error))
    return ok_or_error(false, error);
  audio_3a_note_output_open(config->provide_aec_reference == MMF_TRUE);
  *output = ptr.release();
  return MMF_OK;
}

void mmf_audio_output_close(mmf_audio_output_t* output) {
  if (output == nullptr)
    return;
  if (output->frames_written == 0) {
    const size_t sample_bytes = bytes_per_sample(output->config.io.format);
    const uint32_t channels = output->config.io.channels == 0 ? 1 : output->config.io.channels;
    const uint32_t samples =
        output->config.io.samples_per_frame == 0 ? 160 : output->config.io.samples_per_frame;
    std::vector<std::uint8_t> silence(samples * channels * sample_bytes, 0);
    mmf_audio_frame_t frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.sample_rate = output->config.io.sample_rate;
    frame.channels = channels;
    frame.samples_per_frame = samples;
    frame.format = output->config.io.format;
    frame.data = silence.data();
    frame.bytes = silence.size();
    (void)mmf_audio_output_write(output, &frame, 1000);
  }
  output->output.close();
  audio_3a_note_output_close(output->config.provide_aec_reference == MMF_TRUE);
  delete output;
}

mmf_result_t mmf_audio_output_write(mmf_audio_output_t* output, const mmf_audio_frame_t* frame,
                                    uint32_t timeout_ms) {
  if (output == nullptr || frame == nullptr || frame->data == nullptr) {
    return MMF_EINVAL;
  }
  const size_t sample_bytes = bytes_per_sample(frame->format);
  if (sample_bytes == 0 || frame->channels == 0)
    return MMF_EINVAL;
  const size_t samples = frame->bytes / (sample_bytes * frame->channels);
  const auto* data = static_cast<const uint8_t*>(frame->data);
  mmf_cvi::AudioFrame out;
  out.bit_width = to_native_bit_width(frame->format);
  out.sound_mode =
      frame->channels > 1 ? mmf_cvi::AudioSoundMode::Stereo : mmf_cvi::AudioSoundMode::Mono;
  out.sequence = static_cast<uint32_t>(frame->sequence);
  out.timestamp = frame->timestamp_us;
  out.channels.resize(frame->channels);
  for (uint32_t ch = 0; ch < frame->channels; ++ch) {
    out.channels[ch].resize(samples * sample_bytes);
  }
  for (size_t sample = 0; sample < samples; ++sample) {
    for (uint32_t ch = 0; ch < frame->channels; ++ch) {
      std::memcpy(out.channels[ch].data() + sample * sample_bytes,
                  data + (sample * frame->channels + ch) * sample_bytes, sample_bytes);
    }
  }
  std::string error;
  if (!output->output.writeFrame(out, static_cast<int>(timeout_ms), &error)) {
    return ok_or_error(false, error);
  }
  output->frames_written += 1;
  return MMF_OK;
}

mmf_result_t mmf_audio_output_drain(mmf_audio_output_t* output) {
  (void)output;
  return MMF_OK;
}

mmf_result_t mmf_audio_output_get_status(mmf_audio_output_t* output,
                                         mmf_audio_stream_status_t* status) {
  if (output == nullptr || status == nullptr)
    return MMF_EINVAL;
  std::memset(status, 0, sizeof(*status));
  status->opened = MMF_TRUE;
  status->running = MMF_TRUE;
  status->frames_processed = output->frames_written;
  return MMF_OK;
}

mmf_result_t mmf_audio_output_set_volume(mmf_audio_output_t* output, int volume) {
  if (output == nullptr)
    return MMF_EINVAL;
  std::string error;
  if (!output->output.setVolume(volume, &error))
    return ok_or_error(false, error);
  output->config.io.output_volume = volume;
  return MMF_OK;
}

mmf_result_t mmf_audio_output_get_volume(mmf_audio_output_t* output, int* volume) {
  if (output == nullptr || volume == nullptr)
    return MMF_EINVAL;
  std::string error;
  return ok_or_error(output->output.getVolume(volume, &error), error);
}

void mmf_audio_get_default_3a(mmf_audio_3a_config_t* config) {
  fill_default_3a(config);
}

void mmf_audio_get_default_input_config(mmf_audio_input_config_t* config) {
  if (config == nullptr)
    return;
  std::memset(config, 0, sizeof(*config));
  config->io.sample_rate = 16000;
  config->io.channels = 1;
  config->io.format = MMF_AUDIO_FMT_S16_LE;
  config->io.samples_per_frame = 160;
  config->io.frame_count = 8;
  config->io.timeout_ms = 1000;
  config->io.input_volume = 24;
  mmf_audio_get_default_3a(&config->config_3a);
}

void mmf_audio_get_default_output_config(mmf_audio_output_config_t* config) {
  if (config == nullptr)
    return;
  std::memset(config, 0, sizeof(*config));
  config->io.sample_rate = 16000;
  config->io.channels = 1;
  config->io.format = MMF_AUDIO_FMT_S16_LE;
  config->io.samples_per_frame = 160;
  config->io.frame_count = 8;
  config->io.timeout_ms = 1000;
  config->io.output_volume = 24;
}

}  // extern "C"
