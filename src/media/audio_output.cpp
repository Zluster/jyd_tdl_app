#include "tdl_app/audio_output.hpp"

#include <cstring>
#include <string>

#include "cvi_audio.h"
#include "media/private/audio_runtime.hpp"
#include "media/private/audio_vendor_utils.hpp"

namespace tdl_app {

AudioOutput::AudioOutput() = default;

AudioOutput::AudioOutput(const Config &config) : config_(config) {}

AudioOutput::~AudioOutput() { close(); }

AudioOutput::Config AudioOutput::mono16k(int device, int channel, int card_id) {
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

bool AudioOutput::open(std::string *error) {
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

  int ret = CVI_AO_SetPubAttr(config_.device, &attr);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    private_audio::setError(error,
                            "CVI_AO_SetPubAttr failed, ret=" + std::to_string(ret));
    return false;
  }

  if (config_.card_id >= 0) {
    ret = CVI_AO_SetCard(config_.device, config_.card_id);
    if (ret != CVI_SUCCESS) {
      releaseAudioRuntime();
      private_audio::setError(error,
                              "CVI_AO_SetCard failed, ret=" + std::to_string(ret));
      return false;
    }
  }

  ret = CVI_AO_Enable(config_.device);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    private_audio::setError(error,
                            "CVI_AO_Enable failed, ret=" + std::to_string(ret));
    return false;
  }

  ret = CVI_AO_EnableChn(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    CVI_AO_Disable(config_.device);
    releaseAudioRuntime();
    private_audio::setError(error,
                            "CVI_AO_EnableChn failed, ret=" + std::to_string(ret));
    return false;
  }

  ret = CVI_AO_SetVolume(config_.device, config_.volume_db);
  if (ret != CVI_SUCCESS) {
    CVI_AO_DisableChn(config_.device, config_.channel);
    CVI_AO_Disable(config_.device);
    releaseAudioRuntime();
    private_audio::setError(error,
                            "CVI_AO_SetVolume failed, ret=" + std::to_string(ret));
    return false;
  }

  opened_ = true;
  return true;
}

bool AudioOutput::writeFrame(const AudioFrame &frame, int timeout_ms,
                             std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  if (frame.channels.empty()) {
    private_audio::setError(error, "audio output frame is empty");
    return false;
  }

  AUDIO_FRAME_S vendor_frame = private_audio::toVendor(frame);

  const int ret = CVI_AO_SendFrame(config_.device, config_.channel, &vendor_frame,
                                   timeout_ms);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_SendFrame failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::setVolume(int volume_db, std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  const int ret = CVI_AO_SetVolume(config_.device, volume_db);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_SetVolume failed, ret=" + std::to_string(ret));
    return false;
  }
  config_.volume_db = volume_db;
  return true;
}

bool AudioOutput::getVolume(int *volume_db, std::string *error) const {
  if (!opened_) {
    private_audio::setError(error, "audio output is not open");
    return false;
  }
  if (!volume_db) {
    private_audio::setError(error, "audio output volume pointer is null");
    return false;
  }
  const int ret = CVI_AO_GetVolume(config_.device, volume_db);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_GetVolume failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::setTrackMode(AudioTrackMode mode, std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  const int ret =
      CVI_AO_SetTrackMode(config_.device, private_audio::toVendor(mode));
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AO_SetTrackMode failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::getTrackMode(AudioTrackMode *mode, std::string *error) const {
  if (!opened_) {
    private_audio::setError(error, "audio output is not open");
    return false;
  }
  if (!mode) {
    private_audio::setError(error, "audio output track mode pointer is null");
    return false;
  }
  AUDIO_TRACK_MODE_E vendor_mode = AUDIO_TRACK_NORMAL;
  const int ret = CVI_AO_GetTrackMode(config_.device, &vendor_mode);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AO_GetTrackMode failed, ret=" + std::to_string(ret));
    return false;
  }
  *mode = private_audio::fromVendor(vendor_mode);
  return true;
}

bool AudioOutput::setMute(bool enabled, const AudioFade &fade,
                          std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  const AUDIO_FADE_S vendor_fade = private_audio::toVendor(fade);
  const int ret = CVI_AO_SetMute(config_.device, enabled ? CVI_TRUE : CVI_FALSE,
                                 &vendor_fade);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_SetMute failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::getMute(bool *enabled, AudioFade *fade,
                          std::string *error) const {
  if (!opened_) {
    private_audio::setError(error, "audio output is not open");
    return false;
  }
  if (!enabled) {
    private_audio::setError(error, "audio output mute flag pointer is null");
    return false;
  }
  CVI_BOOL vendor_enabled = CVI_FALSE;
  AUDIO_FADE_S vendor_fade;
  std::memset(&vendor_fade, 0, sizeof(vendor_fade));
  const int ret = CVI_AO_GetMute(config_.device, &vendor_enabled, &vendor_fade);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_GetMute failed, ret=" + std::to_string(ret));
    return false;
  }
  *enabled = vendor_enabled == CVI_TRUE;
  if (fade) {
    *fade = private_audio::fromVendor(vendor_fade);
  }
  return true;
}

bool AudioOutput::setVqeConfig(const AudioOutputVqeConfig &config,
                               std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  AO_VQE_CONFIG_S vendor_config;
  std::memset(&vendor_config, 0, sizeof(vendor_config));
  vendor_config.u32OpenMask = config.open_mask;
  vendor_config.s32WorkSampleRate = config.work_sample_rate;
  vendor_config.s32channels = config.channels;
  vendor_config.stHpfParam.type = config.hpf.type;
  vendor_config.stHpfParam.f0 = config.hpf.f0;
  vendor_config.stHpfParam.Q = config.hpf.q;
  vendor_config.stHpfParam.gainDb = config.hpf.gain_db;
  vendor_config.stEqParam.bandIdx = config.eq.band_index;
  vendor_config.stEqParam.freq = config.eq.frequency;
  vendor_config.stEqParam.QValue = config.eq.q_value;
  vendor_config.stEqParam.gainDb = config.eq.gain_db;
  vendor_config.stDrcCompressor.attackTimeMs =
      config.drc_compressor.attack_time_ms;
  vendor_config.stDrcCompressor.releaseTimeMs =
      config.drc_compressor.release_time_ms;
  vendor_config.stDrcCompressor.ratio = config.drc_compressor.ratio;
  vendor_config.stDrcCompressor.thresholdDb =
      config.drc_compressor.threshold_db;
  vendor_config.stDrcLimiter.attackTimeMs = config.drc_limiter.attack_time_ms;
  vendor_config.stDrcLimiter.releaseTimeMs = config.drc_limiter.release_time_ms;
  vendor_config.stDrcLimiter.thresholdDb = config.drc_limiter.threshold_db;
  vendor_config.stDrcLimiter.postGain = config.drc_limiter.post_gain_db;
  vendor_config.stDrcExpander.attackTimeMs = config.drc_expander.attack_time_ms;
  vendor_config.stDrcExpander.releaseTimeMs =
      config.drc_expander.release_time_ms;
  vendor_config.stDrcExpander.holdTimeMs = config.drc_expander.hold_time_ms;
  vendor_config.stDrcExpander.ratio = config.drc_expander.ratio;
  vendor_config.stDrcExpander.thresholdDb = config.drc_expander.threshold_db;
  vendor_config.stDrcExpander.minDb = config.drc_expander.min_db;

  const int ret =
      CVI_AO_SetVqeAttr(config_.device, config_.channel, &vendor_config);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_SetVqeAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::getVqeConfig(AudioOutputVqeConfig *config,
                               std::string *error) const {
  if (!opened_) {
    private_audio::setError(error, "audio output is not open");
    return false;
  }
  if (!config) {
    private_audio::setError(error, "audio output vqe config pointer is null");
    return false;
  }
  AO_VQE_CONFIG_S vendor_config;
  std::memset(&vendor_config, 0, sizeof(vendor_config));
  const int ret =
      CVI_AO_GetVqeAttr(config_.device, config_.channel, &vendor_config);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_GetVqeAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  config->open_mask = vendor_config.u32OpenMask;
  config->work_sample_rate = vendor_config.s32WorkSampleRate;
  config->channels = vendor_config.s32channels;
  config->hpf.type = vendor_config.stHpfParam.type;
  config->hpf.f0 = vendor_config.stHpfParam.f0;
  config->hpf.q = vendor_config.stHpfParam.Q;
  config->hpf.gain_db = vendor_config.stHpfParam.gainDb;
  config->eq.band_index = vendor_config.stEqParam.bandIdx;
  config->eq.frequency = vendor_config.stEqParam.freq;
  config->eq.q_value = vendor_config.stEqParam.QValue;
  config->eq.gain_db = vendor_config.stEqParam.gainDb;
  config->drc_compressor.attack_time_ms =
      vendor_config.stDrcCompressor.attackTimeMs;
  config->drc_compressor.release_time_ms =
      vendor_config.stDrcCompressor.releaseTimeMs;
  config->drc_compressor.ratio = vendor_config.stDrcCompressor.ratio;
  config->drc_compressor.threshold_db =
      vendor_config.stDrcCompressor.thresholdDb;
  config->drc_limiter.attack_time_ms = vendor_config.stDrcLimiter.attackTimeMs;
  config->drc_limiter.release_time_ms =
      vendor_config.stDrcLimiter.releaseTimeMs;
  config->drc_limiter.threshold_db = vendor_config.stDrcLimiter.thresholdDb;
  config->drc_limiter.post_gain_db = vendor_config.stDrcLimiter.postGain;
  config->drc_expander.attack_time_ms =
      vendor_config.stDrcExpander.attackTimeMs;
  config->drc_expander.release_time_ms =
      vendor_config.stDrcExpander.releaseTimeMs;
  config->drc_expander.hold_time_ms = vendor_config.stDrcExpander.holdTimeMs;
  config->drc_expander.ratio = vendor_config.stDrcExpander.ratio;
  config->drc_expander.threshold_db = vendor_config.stDrcExpander.thresholdDb;
  config->drc_expander.min_db = vendor_config.stDrcExpander.minDb;
  return true;
}

bool AudioOutput::enableVqe(std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  const int ret = CVI_AO_EnableVqe(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_EnableVqe failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::disableVqe(std::string *error) {
  if (!opened_) {
    private_audio::setError(error, "audio output is not open");
    return false;
  }
  const int ret = CVI_AO_DisableVqe(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_DisableVqe failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::enableResample(AudioSampleRate input_sample_rate,
                                 std::string *error) {
  if (!opened_ && !open(error)) {
    return false;
  }
  const int ret = CVI_AO_EnableReSmp(config_.device, config_.channel,
                                     private_audio::toVendor(input_sample_rate));
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AO_EnableReSmp failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::disableResample(std::string *error) {
  if (!opened_) {
    private_audio::setError(error, "audio output is not open");
    return false;
  }
  const int ret = CVI_AO_DisableReSmp(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(
        error, "CVI_AO_DisableReSmp failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::clearBuffer(std::string *error) {
  if (!opened_) {
    private_audio::setError(error, "audio output is not open");
    return false;
  }
  const int ret = CVI_AO_ClearChnBuf(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_ClearChnBuf failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::pause(std::string *error) {
  if (!opened_) {
    private_audio::setError(error, "audio output is not open");
    return false;
  }
  const int ret = CVI_AO_PauseChn(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_PauseChn failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::resume(std::string *error) {
  if (!opened_) {
    private_audio::setError(error, "audio output is not open");
    return false;
  }
  const int ret = CVI_AO_ResumeChn(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_ResumeChn failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool AudioOutput::queryState(ChannelState *state, std::string *error) const {
  if (!opened_) {
    private_audio::setError(error, "audio output is not open");
    return false;
  }
  if (!state) {
    private_audio::setError(error, "audio output state pointer is null");
    return false;
  }

  AO_CHN_STATE_S vendor_state;
  std::memset(&vendor_state, 0, sizeof(vendor_state));
  const int ret = CVI_AO_QueryChnStat(config_.device, config_.channel,
                                      &vendor_state);
  if (ret != CVI_SUCCESS) {
    private_audio::setError(error,
                            "CVI_AO_QueryChnStat failed, ret=" + std::to_string(ret));
    return false;
  }

  state->total = vendor_state.u32ChnTotalNum;
  state->free = vendor_state.u32ChnFreeNum;
  state->busy = vendor_state.u32ChnBusyNum;
  return true;
}

void AudioOutput::close() {
  if (!opened_) {
    return;
  }
  CVI_AO_DisableChn(config_.device, config_.channel);
  CVI_AO_Disable(config_.device);
  releaseAudioRuntime();
  opened_ = false;
}

bool AudioOutput::isOpen() const { return opened_; }

}  // namespace tdl_app
