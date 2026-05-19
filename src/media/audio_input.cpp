#include "tdl_app/audio_input.hpp"

#include <cstring>
#include <string>

#include "cvi_audio.h"
#include "media/private/audio_runtime.hpp"
#include "media/private/audio_vendor_utils.hpp"

namespace tdl_app {

AudioInput::AudioInput() = default;

AudioInput::AudioInput(const Config &config) : config_(config) {}

AudioInput::~AudioInput() { close(); }

AudioInput::Config AudioInput::mono16k(int device, int channel, int card_id) {
  Config config;
  config.device = device;
  config.channel = channel;
  config.card_id = card_id;
  config.sample_rate = AudioSampleRate::Hz16000;
  config.bit_width = AudioBitWidth::Bits16;
  config.sound_mode = AudioSoundMode::Mono;
  config.channel_count = 1;
  config.points_per_frame = 160;
  config.frame_count = 8;
  return config;
}

bool AudioInput::open(std::string *error) {
  if (opened_) {
    return true;
  }
  if (!retainAudioRuntime(error)) {
    return false;
  }

  AIO_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enSamplerate = private_audio::toVendor(config_.sample_rate);
  attr.enBitwidth = private_audio::toVendor(config_.bit_width);
  attr.enWorkmode = private_audio::toVendor(config_.work_mode);
  attr.enI2sType = private_audio::toVendor(config_.i2s_type);
  attr.enSoundmode = private_audio::toVendor(config_.sound_mode);
  attr.u32FrmNum = static_cast<CVI_U32>(config_.frame_count);
  attr.u32PtNumPerFrm = static_cast<CVI_U32>(config_.points_per_frame);
  attr.u32ChnCnt = static_cast<CVI_U32>(config_.channel_count);
  attr.u32ClkSel = static_cast<CVI_U32>(config_.clock_select);

  int ret = CVI_AI_SetPubAttr(config_.device, &attr);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    private_audio::setError(error,
                            "CVI_AI_SetPubAttr failed, ret=" + std::to_string(ret));
    return false;
  }

  if (config_.card_id >= 0) {
    ret = CVI_AI_SetCard(config_.device, config_.card_id);
    if (ret != CVI_SUCCESS) {
      releaseAudioRuntime();
      private_audio::setError(error,
                              "CVI_AI_SetCard failed, ret=" + std::to_string(ret));
      return false;
    }
  }

  ret = CVI_AI_Enable(config_.device);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    private_audio::setError(error,
                            "CVI_AI_Enable failed, ret=" + std::to_string(ret));
    return false;
  }

  ret = CVI_AI_EnableChn(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    CVI_AI_Disable(config_.device);
    releaseAudioRuntime();
    private_audio::setError(error,
                            "CVI_AI_EnableChn failed, ret=" + std::to_string(ret));
    return false;
  }

  AI_CHN_PARAM_S param;
  std::memset(&param, 0, sizeof(param));
  param.u32UsrFrmDepth = static_cast<CVI_U32>(config_.frame_depth);
  ret = CVI_AI_SetChnParam(config_.device, config_.channel, &param);
  if (ret != CVI_SUCCESS) {
    CVI_AI_DisableChn(config_.device, config_.channel);
    CVI_AI_Disable(config_.device);
    releaseAudioRuntime();
    private_audio::setError(
        error, "CVI_AI_SetChnParam failed, ret=" + std::to_string(ret));
    return false;
  }

  ret = CVI_AI_SetVolume(config_.device, config_.volume_step);
  if (ret != CVI_SUCCESS) {
    CVI_AI_DisableChn(config_.device, config_.channel);
    CVI_AI_Disable(config_.device);
    releaseAudioRuntime();
    private_audio::setError(error,
                            "CVI_AI_SetVolume failed, ret=" + std::to_string(ret));
    return false;
  }

  opened_ = true;
  return true;
}

bool AudioInput::readFrame(AudioFrame *frame, int timeout_ms, std::string *error) {
  if (!opened_) {
    if (!open(error)) {
      return false;
    }
  }
  if (!frame) {
    private_audio::setError(error, "audio input frame pointer is null");
    return false;
  }

  AUDIO_FRAME_S vendor_frame;
  std::memset(&vendor_frame, 0, sizeof(vendor_frame));

  const int ret = CVI_AI_GetFrame(config_.device, config_.channel, &vendor_frame,
                                  nullptr, timeout_ms);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AI_GetFrame failed, ret=" + std::to_string(ret));
    return false;
  }

  private_audio::fromVendor(vendor_frame, config_.bit_width, config_.sound_mode,
                            frame);

  CVI_AI_ReleaseFrame(config_.device, config_.channel, &vendor_frame, nullptr);

  if (frame->channels.empty()) {
    private_audio::setError(error, "audio input returned empty frame");
    return false;
  }
  return true;
}

bool AudioInput::setVolume(int volume_step, std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  const int ret = CVI_AI_SetVolume(config_.device, volume_step);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AI_SetVolume failed, ret=" + std::to_string(ret));
    return false;
  }
  config_.volume_step = volume_step;
  return true;
}

bool AudioInput::getVolume(int *volume_step, std::string *error) const {
  if (!opened_) {
    private_audio::setError(error, "audio input is not open");
    return false;
  }
  if (!volume_step) {
    private_audio::setError(error, "audio input volume pointer is null");
    return false;
  }
  int ret = CVI_AI_GetVolume(config_.device, volume_step);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AI_GetVolume failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioInput::setTrackMode(AudioTrackMode mode, std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  const int ret =
      CVI_AI_SetTrackMode(config_.device, private_audio::toVendor(mode));
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AI_SetTrackMode failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioInput::getTrackMode(AudioTrackMode *mode, std::string *error) const {
  if (!opened_) {
    private_audio::setError(error, "audio input is not open");
    return false;
  }
  if (!mode) {
    private_audio::setError(error, "audio input track mode pointer is null");
    return false;
  }
  AUDIO_TRACK_MODE_E vendor_mode = AUDIO_TRACK_NORMAL;
  const int ret = CVI_AI_GetTrackMode(config_.device, &vendor_mode);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AI_GetTrackMode failed, ret=" + std::to_string(ret));
    return false;
  }
  *mode = private_audio::fromVendor(vendor_mode);
  return true;
}

bool AudioInput::enableResample(AudioSampleRate output_sample_rate,
                                std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  const int ret = CVI_AI_EnableReSmp(config_.device, config_.channel,
                                     private_audio::toVendor(output_sample_rate));
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AI_EnableReSmp failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioInput::disableResample(std::string *error) {
  if (!opened_) {
    private_audio::setError(error, "audio input is not open");
    return false;
  }
  const int ret = CVI_AI_DisableReSmp(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AI_DisableReSmp failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioInput::enableVqe(std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  const int ret = CVI_AI_EnableVqe(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AI_EnableVqe failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioInput::disableVqe(std::string *error) {
  if (!opened_) {
    private_audio::setError(error, "audio input is not open");
    return false;
  }
  const int ret = CVI_AI_DisableVqe(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AI_DisableVqe failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioInput::setVqeMask(std::uint32_t mask, std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  const int ret = CVI_AI_VqeFunConfig(config_.device, config_.channel, mask);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AI_VqeFunConfig failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioInput::configureTalkVqe(const AudioTalkVqeConfig &config,
                                  int output_device, int output_channel,
                                  std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  AI_TALKVQE_CONFIG_S vendor_config;
  std::memset(&vendor_config, 0, sizeof(vendor_config));
  vendor_config.para_client_config = config.client_config;
  vendor_config.u32OpenMask = config.open_mask;
  vendor_config.s32WorkSampleRate = config.work_sample_rate;
  vendor_config.stAecCfg.para_aec_filter_len = config.aec.filter_length;
  vendor_config.stAecCfg.para_aes_std_thrd = config.aec.std_threshold;
  vendor_config.stAecCfg.para_aes_supp_coeff = config.aec.suppress_coeff;
  vendor_config.stAnrCfg.para_nr_snr_coeff = config.anr.snr_coeff;
  vendor_config.stAnrCfg.para_nr_init_sile_time =
      config.anr.initial_silence_time;
  vendor_config.stAgcCfg.para_agc_max_gain = config.agc.max_gain;
  vendor_config.stAgcCfg.para_agc_target_high = config.agc.target_high;
  vendor_config.stAgcCfg.para_agc_target_low = config.agc.target_low;
  vendor_config.stAgcCfg.para_agc_vad_ena =
      config.agc.vad_enabled ? CVI_TRUE : CVI_FALSE;
  vendor_config.stAecDelayCfg.para_aec_init_filter_len =
      config.delay.initial_filter_length;
  vendor_config.stAecDelayCfg.para_dg_target = config.delay.digital_gain_target;
  vendor_config.stAecDelayCfg.para_delay_sample = config.delay.delay_sample;
  vendor_config.para_notch_freq = config.notch_frequency;

  const int ret =
      CVI_AI_SetTalkVqeAttr(config_.device, config_.channel, output_device,
                            output_channel, &vendor_config);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AI_SetTalkVqeAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioInput::queryTalkVqe(AudioTalkVqeConfig *config,
                              std::string *error) const {
  if (!opened_) {
    private_audio::setError(error, "audio input is not open");
    return false;
  }
  if (!config) {
    private_audio::setError(error, "audio input talk vqe config pointer is null");
    return false;
  }
  AI_TALKVQE_CONFIG_S vendor_config;
  std::memset(&vendor_config, 0, sizeof(vendor_config));
  const int ret =
      CVI_AI_GetTalkVqeAttr(config_.device, config_.channel, &vendor_config);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AI_GetTalkVqeAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  config->client_config = vendor_config.para_client_config;
  config->open_mask = vendor_config.u32OpenMask;
  config->work_sample_rate = vendor_config.s32WorkSampleRate;
  config->aec.filter_length = vendor_config.stAecCfg.para_aec_filter_len;
  config->aec.std_threshold = vendor_config.stAecCfg.para_aes_std_thrd;
  config->aec.suppress_coeff = vendor_config.stAecCfg.para_aes_supp_coeff;
  config->anr.snr_coeff = vendor_config.stAnrCfg.para_nr_snr_coeff;
  config->anr.initial_silence_time =
      vendor_config.stAnrCfg.para_nr_init_sile_time;
  config->agc.max_gain = vendor_config.stAgcCfg.para_agc_max_gain;
  config->agc.target_high = vendor_config.stAgcCfg.para_agc_target_high;
  config->agc.target_low = vendor_config.stAgcCfg.para_agc_target_low;
  config->agc.vad_enabled = vendor_config.stAgcCfg.para_agc_vad_ena == CVI_TRUE;
  config->delay.initial_filter_length =
      vendor_config.stAecDelayCfg.para_aec_init_filter_len;
  config->delay.digital_gain_target =
      vendor_config.stAecDelayCfg.para_dg_target;
  config->delay.delay_sample = vendor_config.stAecDelayCfg.para_delay_sample;
  config->notch_frequency = vendor_config.para_notch_freq;
  return true;
}

void AudioInput::close() {
  if (!opened_) {
    return;
  }
  CVI_AI_DisableChn(config_.device, config_.channel);
  CVI_AI_Disable(config_.device);
  releaseAudioRuntime();
  opened_ = false;
}

bool AudioInput::isOpen() const { return opened_; }

}  // namespace tdl_app
