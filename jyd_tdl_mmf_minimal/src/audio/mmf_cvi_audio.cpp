#include "mmf_cv184x_common.hpp"

namespace mmf_cvi {
namespace {
std::mutex g_audio_mutex;
int g_audio_refs = 0;
size_t bytesPerSample(AudioBitWidth value) {
  return value == AudioBitWidth::Bits32   ? 4
         : value == AudioBitWidth::Bits24 ? 3
         : value == AudioBitWidth::Bits8  ? 1
                                          : 2;
}
AUDIO_SAMPLE_RATE_E toVendor(AudioSampleRate v) {
  return static_cast<AUDIO_SAMPLE_RATE_E>(static_cast<int>(v));
}
AUDIO_BIT_WIDTH_E toVendor(AudioBitWidth v) {
  if (v == AudioBitWidth::Bits32)
    return AUDIO_BIT_WIDTH_32;
  if (v == AudioBitWidth::Bits24)
    return AUDIO_BIT_WIDTH_24;
  if (v == AudioBitWidth::Bits8)
    return AUDIO_BIT_WIDTH_8;
  return AUDIO_BIT_WIDTH_16;
}
AIO_MODE_E toVendor(AudioWorkMode v) {
  return static_cast<AIO_MODE_E>(static_cast<int>(v));
}
AIO_I2STYPE_E toVendor(AudioI2sType v) {
  return static_cast<AIO_I2STYPE_E>(static_cast<int>(v));
}
AUDIO_SOUND_MODE_E toVendor(AudioSoundMode v) {
  return v == AudioSoundMode::Stereo ? AUDIO_SOUND_MODE_STEREO : AUDIO_SOUND_MODE_MONO;
}
PAYLOAD_TYPE_E toVendor(AudioPayloadType v) {
  return static_cast<PAYLOAD_TYPE_E>(static_cast<int>(v));
}
G726_BPS_E toVendor(AudioG726Bitrate v) {
  return static_cast<G726_BPS_E>(static_cast<int>(v));
}
ADPCM_TYPE_E toVendor(AudioAdpcmType v) {
  return static_cast<ADPCM_TYPE_E>(static_cast<int>(v));
}
ADEC_MODE_E toVendor(AudioDecodeMode v) {
  return static_cast<ADEC_MODE_E>(static_cast<int>(v));
}
AUDIO_FRAME_S toVendor(const AudioFrame& frame) {
  AUDIO_FRAME_S out;
  std::memset(&out, 0, sizeof(out));
  out.enBitwidth = toVendor(frame.bit_width);
  out.enSoundmode = toVendor(frame.sound_mode);
  out.u64TimeStamp = frame.timestamp;
  out.u32Seq = frame.sequence;
  const size_t sample_bytes = bytesPerSample(frame.bit_width);
  const size_t channel_bytes = frame.bytes_per_channel
                                   ? frame.bytes_per_channel
                                   : (frame.channels.empty() ? 0 : frame.channels.front().size());
  out.u32Len = sample_bytes ? channel_bytes / sample_bytes : 0;
  for (size_t i = 0; i < frame.channels.size() && i < 2; ++i)
    out.u64VirAddr[i] = const_cast<uint8_t*>(frame.channels[i].data());
  return out;
}
void fromVendor(const AUDIO_FRAME_S& v, AudioBitWidth bit_width, AudioSoundMode sound_mode,
                AudioFrame* frame) {
  const size_t sample_bytes = bytesPerSample(bit_width);
  const size_t channel_bytes = static_cast<size_t>(v.u32Len) * sample_bytes;
  frame->bit_width = bit_width;
  frame->sound_mode = sound_mode;
  frame->timestamp = v.u64TimeStamp;
  frame->sequence = v.u32Seq;
  frame->bytes_per_channel = channel_bytes;
  frame->channels.clear();
  if (sound_mode == AudioSoundMode::Stereo && v.u64VirAddr[0] && !v.u64VirAddr[1] &&
      channel_bytes > 0) {
    const uint8_t* interleaved = reinterpret_cast<const uint8_t*>(v.u64VirAddr[0]);
    frame->channels.assign(2, std::vector<uint8_t>(channel_bytes));
    const size_t samples = channel_bytes / sample_bytes;
    for (size_t s = 0; s < samples; ++s) {
      std::memcpy(frame->channels[0].data() + s * sample_bytes,
                  interleaved + (s * 2) * sample_bytes, sample_bytes);
      std::memcpy(frame->channels[1].data() + s * sample_bytes,
                  interleaved + (s * 2 + 1) * sample_bytes, sample_bytes);
    }
    return;
  }
  for (int i = 0; i < 2; ++i)
    if (v.u64VirAddr[i] && channel_bytes) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(v.u64VirAddr[i]);
      frame->channels.emplace_back(p, p + channel_bytes);
    }
}
void fromVendor(const AUDIO_STREAM_S& v, AudioPayloadType payload, AudioEncodedStream* s) {
  s->payload_type = payload;
  s->timestamp = v.u64TimeStamp;
  s->sequence = v.u32Seq;
  s->data.clear();
  if (v.pStream && v.u32Len)
    s->data.assign(v.pStream, v.pStream + v.u32Len);
}
AUDIO_STREAM_S toVendor(const AudioEncodedStream& s) {
  AUDIO_STREAM_S out;
  std::memset(&out, 0, sizeof(out));
  out.pStream = const_cast<uint8_t*>(s.data.data());
  out.u32Len = s.data.size();
  out.u64TimeStamp = s.timestamp;
  out.u32Seq = s.sequence;
  return out;
}
}  // namespace
bool retainAudioRuntime(std::string* error) {
  std::lock_guard<std::mutex> lock(g_audio_mutex);
  if (g_audio_refs++ > 0)
    return true;
  int ret = CVI_AUDIO_INIT();
  if (ret != CVI_SUCCESS) {
    g_audio_refs = 0;
    setError(error, "CVI_AUDIO_INIT failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}
void releaseAudioRuntime() {
  std::lock_guard<std::mutex> lock(g_audio_mutex);
  if (g_audio_refs <= 0) {
    g_audio_refs = 0;
    return;
  }
  if (--g_audio_refs == 0)
    CVI_AUDIO_DEINIT();
}
AudioInput::AudioInput() = default;
AudioInput::AudioInput(const Config& config) : config_(config) {}
AudioInput::~AudioInput() {
  close();
}
bool AudioInput::open(std::string* error) {
  if (opened_)
    return true;
  if (!retainAudioRuntime(error))
    return false;
  AIO_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enSamplerate = toVendor(config_.sample_rate);
  attr.enBitwidth = toVendor(config_.bit_width);
  attr.enWorkmode = toVendor(config_.work_mode);
  attr.enI2sType = toVendor(config_.i2s_type);
  attr.enSoundmode = toVendor(config_.sound_mode);
  attr.u32FrmNum = config_.frame_count;
  attr.u32PtNumPerFrm = config_.points_per_frame;
  attr.u32ChnCnt = config_.channel_count;
  attr.u32ClkSel = config_.clock_select;
  int ret = CVI_AI_SetPubAttr(config_.device, &attr);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    setError(error, "CVI_AI_SetPubAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  if (config_.card_id >= 0) {
    ret = CVI_AI_SetCard(config_.device, config_.card_id);
    if (ret != CVI_SUCCESS) {
      releaseAudioRuntime();
      setError(error, "CVI_AI_SetCard failed, ret=" + std::to_string(ret));
      return false;
    }
  }
  ret = CVI_AI_Enable(config_.device);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    setError(error, "CVI_AI_Enable failed, ret=" + std::to_string(ret));
    return false;
  }
  ret = CVI_AI_EnableChn(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    CVI_AI_Disable(config_.device);
    releaseAudioRuntime();
    setError(error, "CVI_AI_EnableChn failed, ret=" + std::to_string(ret));
    return false;
  }
  AI_CHN_PARAM_S param;
  std::memset(&param, 0, sizeof(param));
  param.u32UsrFrmDepth = config_.frame_depth;
  ret = CVI_AI_SetChnParam(config_.device, config_.channel, &param);
  if (ret != CVI_SUCCESS) {
    close();
    setError(error, "CVI_AI_SetChnParam failed, ret=" + std::to_string(ret));
    return false;
  }
  ret = CVI_AI_SetVolume(config_.device, config_.volume_step);
  if (ret != CVI_SUCCESS) {
    close();
    setError(error, "CVI_AI_SetVolume failed, ret=" + std::to_string(ret));
    return false;
  }
  opened_ = true;
  return true;
}
bool AudioInput::readFrame(AudioFrame* frame, int timeout_ms, std::string* error) {
  if (!opened_ && !open(error))
    return false;
  if (!frame)
    return false;
  AUDIO_FRAME_S vf;
  std::memset(&vf, 0, sizeof(vf));
  int ret = CVI_AI_GetFrame(config_.device, config_.channel, &vf, nullptr, timeout_ms);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AI_GetFrame failed, ret=" + std::to_string(ret));
    return false;
  }
  fromVendor(vf, config_.bit_width, config_.sound_mode, frame);
  CVI_AI_ReleaseFrame(config_.device, config_.channel, &vf, nullptr);
  return !frame->channels.empty();
}
bool AudioInput::setVolume(int volume, std::string* error) {
  if (!opened_ && !open(error))
    return false;
  int ret = CVI_AI_SetVolume(config_.device, volume);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AI_SetVolume failed, ret=" + std::to_string(ret));
    return false;
  }
  config_.volume_step = volume;
  return true;
}
bool AudioInput::getVolume(int* volume, std::string* error) const {
  if (!volume)
    return false;
  int ret = CVI_AI_GetVolume(config_.device, volume);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AI_GetVolume failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}
bool AudioInput::configureTalkVqe(const AudioTalkVqeConfig& config, int output_device,
                                  int output_channel, std::string* error) {
  if (!opened_ && !open(error))
    return false;
  AI_TALKVQE_CONFIG_S v;
  std::memset(&v, 0, sizeof(v));
  v.para_client_config = config.client_config;
  v.u32OpenMask = config.open_mask;
  v.s32WorkSampleRate = config.work_sample_rate;
  v.stAecCfg.para_aec_filter_len = config.aec.filter_length;
  v.stAecCfg.para_aes_std_thrd = config.aec.std_threshold;
  v.stAecCfg.para_aes_supp_coeff = config.aec.suppress_coeff;
  v.stAnrCfg.para_nr_snr_coeff = config.anr.snr_coeff;
  v.stAnrCfg.para_nr_init_sile_time = config.anr.initial_silence_time;
  v.stAgcCfg.para_agc_max_gain = config.agc.max_gain;
  v.stAgcCfg.para_agc_target_high = config.agc.target_high;
  v.stAgcCfg.para_agc_target_low = config.agc.target_low;
  v.stAgcCfg.para_agc_vad_ena = config.agc.vad_enabled ? CVI_TRUE : CVI_FALSE;
  v.stAecDelayCfg.para_aec_init_filter_len = config.delay.initial_filter_length;
  v.stAecDelayCfg.para_dg_target = config.delay.digital_gain_target;
  v.stAecDelayCfg.para_delay_sample = config.delay.delay_sample;
  v.para_notch_freq = config.notch_frequency;
  int ret =
      CVI_AI_SetTalkVqeAttr(config_.device, config_.channel, output_device, output_channel, &v);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AI_SetTalkVqeAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}
void AudioInput::close() {
  if (!opened_)
    return;
  CVI_AI_DisableChn(config_.device, config_.channel);
  CVI_AI_Disable(config_.device);
  releaseAudioRuntime();
  opened_ = false;
}
AudioOutput::AudioOutput() = default;
AudioOutput::AudioOutput(const Config& config) : config_(config) {}
AudioOutput::~AudioOutput() {
  close();
}
bool AudioOutput::open(std::string* error) {
  if (opened_)
    return true;
  if (!retainAudioRuntime(error))
    return false;
  AIO_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enSamplerate = toVendor(config_.sample_rate);
  attr.enBitwidth = toVendor(config_.bit_width);
  attr.enWorkmode = toVendor(config_.work_mode);
  attr.enI2sType = toVendor(config_.i2s_type);
  attr.enSoundmode = toVendor(config_.sound_mode);
  attr.u32FrmNum = config_.frame_count;
  attr.u32PtNumPerFrm = config_.points_per_frame;
  attr.u32ChnCnt = config_.channel_count;
  attr.u32ClkSel = config_.clock_select;
  int ret = CVI_AO_SetPubAttr(config_.device, &attr);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    setError(error, "CVI_AO_SetPubAttr failed, ret=" + std::to_string(ret));
    return false;
  }
  if (config_.card_id >= 0) {
    ret = CVI_AO_SetCard(config_.device, config_.card_id);
    if (ret != CVI_SUCCESS) {
      releaseAudioRuntime();
      setError(error, "CVI_AO_SetCard failed, ret=" + std::to_string(ret));
      return false;
    }
  }
  ret = CVI_AO_Enable(config_.device);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    setError(error, "CVI_AO_Enable failed, ret=" + std::to_string(ret));
    return false;
  }
  ret = CVI_AO_EnableChn(config_.device, config_.channel);
  if (ret != CVI_SUCCESS) {
    CVI_AO_Disable(config_.device);
    releaseAudioRuntime();
    setError(error, "CVI_AO_EnableChn failed, ret=" + std::to_string(ret));
    return false;
  }
  ret = CVI_AO_SetVolume(config_.device, config_.volume_db);
  if (ret != CVI_SUCCESS) {
    close();
    setError(error, "CVI_AO_SetVolume failed, ret=" + std::to_string(ret));
    return false;
  }
  opened_ = true;
  return true;
}
bool AudioOutput::writeFrame(const AudioFrame& frame, int timeout_ms, std::string* error) {
  if (!opened_ && !open(error))
    return false;
  AUDIO_FRAME_S vf = toVendor(frame);
  int ret = CVI_AO_SendFrame(config_.device, config_.channel, &vf, timeout_ms);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AO_SendFrame failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}
bool AudioOutput::setVolume(int volume, std::string* error) {
  if (!opened_ && !open(error))
    return false;
  int ret = CVI_AO_SetVolume(config_.device, volume);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AO_SetVolume failed, ret=" + std::to_string(ret));
    return false;
  }
  config_.volume_db = volume;
  return true;
}
bool AudioOutput::getVolume(int* volume, std::string* error) const {
  if (!volume)
    return false;
  int ret = CVI_AO_GetVolume(config_.device, volume);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AO_GetVolume failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}
void AudioOutput::close() {
  if (!opened_)
    return;
  CVI_AO_DisableChn(config_.device, config_.channel);
  CVI_AO_Disable(config_.device);
  releaseAudioRuntime();
  opened_ = false;
}
AudioEncoder::AudioEncoder() = default;
AudioEncoder::AudioEncoder(const Config& config) : config_(config) {}
AudioEncoder::~AudioEncoder() {
  close();
}
bool AudioEncoder::open(std::string* error) {
  if (opened_)
    return true;
  if (!retainAudioRuntime(error))
    return false;
  AENC_ATTR_G711_S g711;
  std::memset(&g711, 0, sizeof(g711));
  AENC_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enType = toVendor(config_.payload_type);
  attr.u32PtNumPerFrm = config_.points_per_frame;
  attr.u32BufSize = config_.buffer_size;
  attr.pValue = &g711;
  attr.bFileDbgMode = config_.file_debug_mode ? CVI_TRUE : CVI_FALSE;
  int ret = CVI_AENC_CreateChn(config_.channel, &attr);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    setError(error, "CVI_AENC_CreateChn failed, ret=" + std::to_string(ret));
    return false;
  }
  opened_ = true;
  return true;
}
bool AudioEncoder::encodeFrame(const AudioFrame& frame, AudioEncodedStream* stream,
                               std::string* error) {
  if (!opened_ && !open(error))
    return false;
  if (!stream)
    return false;
  AUDIO_FRAME_S vf = toVendor(frame);
  int ret = CVI_AENC_SendFrame(config_.channel, &vf, nullptr);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AENC_SendFrame failed, ret=" + std::to_string(ret));
    return false;
  }
  AUDIO_STREAM_S vs;
  std::memset(&vs, 0, sizeof(vs));
  ret = CVI_AENC_GetStream(config_.channel, &vs, 1000);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_AENC_GetStream failed, ret=" + std::to_string(ret));
    return false;
  }
  fromVendor(vs, config_.payload_type, stream);
  CVI_AENC_ReleaseStream(config_.channel, &vs);
  return !stream->empty();
}
void AudioEncoder::close() {
  if (!opened_)
    return;
  CVI_AENC_DestroyChn(config_.channel);
  releaseAudioRuntime();
  opened_ = false;
}
AudioDecoder::AudioDecoder() = default;
AudioDecoder::AudioDecoder(const Config& config) : config_(config) {}
AudioDecoder::~AudioDecoder() {
  close();
}
bool AudioDecoder::open(std::string* error) {
  if (opened_)
    return true;
  if (!retainAudioRuntime(error))
    return false;
  ADEC_ATTR_G711_S g711;
  std::memset(&g711, 0, sizeof(g711));
  ADEC_CHN_ATTR_S attr;
  std::memset(&attr, 0, sizeof(attr));
  attr.enType = toVendor(config_.payload_type);
  attr.u32BufSize = config_.buffer_size;
  attr.enMode = toVendor(config_.decode_mode);
  attr.pValue = &g711;
  attr.bFileDbgMode = config_.file_debug_mode ? CVI_TRUE : CVI_FALSE;
  attr.s32BytesPerSample = config_.bytes_per_sample;
  attr.s32frame_size = config_.frame_size;
  attr.s32ChannelNums = config_.channel_count;
  attr.s32Sample_rate = config_.sample_rate;
  int ret = CVI_ADEC_CreateChn(config_.channel, &attr);
  if (ret != CVI_SUCCESS) {
    releaseAudioRuntime();
    setError(error, "CVI_ADEC_CreateChn failed, ret=" + std::to_string(ret));
    return false;
  }
  opened_ = true;
  return true;
}
bool AudioDecoder::decodeStream(const AudioEncodedStream& stream, AudioFrame* frame, bool block,
                                std::string* error) {
  if (!opened_ && !open(error))
    return false;
  if (!frame || stream.empty())
    return false;
  AUDIO_STREAM_S vs = toVendor(stream);
  int ret = CVI_ADEC_SendStream(config_.channel, &vs, block ? CVI_TRUE : CVI_FALSE);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_ADEC_SendStream failed, ret=" + std::to_string(ret));
    return false;
  }
  AUDIO_FRAME_INFO_S info;
  std::memset(&info, 0, sizeof(info));
  ret = CVI_ADEC_GetFrame(config_.channel, &info, block ? 1000 : 0);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_ADEC_GetFrame failed, ret=" + std::to_string(ret));
    return false;
  }
  if (!info.pstFrame) {
    setError(error, "CVI_ADEC_GetFrame returned null");
    return false;
  }
  AudioBitWidth bw = config_.bytes_per_sample >= 4   ? AudioBitWidth::Bits32
                     : config_.bytes_per_sample >= 3 ? AudioBitWidth::Bits24
                                                     : AudioBitWidth::Bits16;
  AudioSoundMode sm = config_.channel_count > 1 ? AudioSoundMode::Stereo : AudioSoundMode::Mono;
  fromVendor(*info.pstFrame, bw, sm, frame);
  CVI_ADEC_ReleaseFrame(config_.channel, &info);
  return !frame->empty();
}
void AudioDecoder::close() {
  if (!opened_)
    return;
  CVI_ADEC_DestroyChn(config_.channel);
  releaseAudioRuntime();
  opened_ = false;
}
}  // namespace mmf_cvi
