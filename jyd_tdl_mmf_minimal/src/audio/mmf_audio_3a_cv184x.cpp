#include "mmf_cv184x_common.hpp"
#include "mmf_cv184x_resources.hpp"

using namespace mmf_cv184x;

extern "C" {
typedef struct cviIPCMSG_MESSAGE_S {
  CVI_BOOL bIsResp;
  CVI_U64 u64Id;
  CVI_U32 u32Module;
  CVI_U32 u32CMD;
  CVI_S32 s32RetVal;
  CVI_U32 u32BodyLen;
  CVI_S32 as32PrivData[8];
  CVI_VOID* pBody;
#ifdef __arm__
  CVI_U32 u32VirAddrPadding;
#endif
} CVI_IPCMSG_MESSAGE_S;

typedef void (*CVI_IPCMSG_HANDLE_FN_PTR)(CVI_S32 s32Id, CVI_IPCMSG_MESSAGE_S* pstMsg);

CVI_S32 CVI_IPCMSG_Connect(CVI_S32* ps32Id, const CVI_CHAR* pszServiceName,
                           CVI_IPCMSG_HANDLE_FN_PTR pfnMessageHandle);
CVI_IPCMSG_MESSAGE_S* CVI_IPCMSG_CreateMessage(CVI_U32 u32Module, CVI_U32 u32CMD,
                                               CVI_VOID* pBody, CVI_U32 u32BodyLen);
CVI_S32 CVI_IPCMSG_SendSync(CVI_S32 s32Id, CVI_IPCMSG_MESSAGE_S* pstMsg,
                            CVI_IPCMSG_MESSAGE_S** ppstMsg, CVI_S32 s32TimeoutMs);
CVI_VOID CVI_IPCMSG_DestroyMessage(CVI_IPCMSG_MESSAGE_S* pstMsg);
}

namespace {

mmf_audio_3a_config_t g_3a_config;
bool g_3a_config_inited = false;
std::mutex g_3a_mutex;
uint32_t g_input_sessions = 0;
uint32_t g_applied_input_sessions = 0;
uint32_t g_playback_reference_sessions = 0;
bool g_audio_msg_runtime_retained = false;
CVI_S32 g_audio_msg_si_id = -1;

constexpr const char* kMsgServiceName = "CVI_MMF_MSG";
constexpr CVI_S32 kAudioIpcTimeoutMs = 3000;
constexpr CVI_U32 kAudioCmdSet3AConfig = 12;
constexpr CVI_U32 kAudioCmdGet3AConfig = 13;
constexpr CVI_U32 kAudioCmdApply3AConfig = 14;

struct MsgAudio3aConfig {
  uint8_t aec_enable;
  uint8_t anr_enable;
  uint8_t agc_enable;
  uint8_t reserved;
  int32_t aec_level;
  int32_t aec_delay_ms;
  int32_t anr_level;
  int32_t agc_target;
  int32_t agc_max_gain;
  int32_t agc_compress;
};

mmf_audio_3a_config_t* global_3a() {
  if (!g_3a_config_inited) {
    fill_default_3a(&g_3a_config);
    g_3a_config_inited = true;
  }
  return &g_3a_config;
}

CVI_U32 msg_modfd(CVI_U32 mod, CVI_U32 dev, CVI_U32 chn, bool block) {
  return ((mod & 0xffu) << 24) | ((dev & 0xffu) << 16) | ((chn & 0xffu) << 8) |
         (block ? 1u : 0u);
}

MsgAudio3aConfig to_msg_config(const mmf_audio_3a_config_t& cfg) {
  MsgAudio3aConfig out;
  std::memset(&out, 0, sizeof(out));
  out.aec_enable = cfg.aec_enable == MMF_TRUE ? 1 : 0;
  out.anr_enable = cfg.ns_enable == MMF_TRUE ? 1 : 0;
  out.agc_enable = cfg.agc_enable == MMF_TRUE ? 1 : 0;
  out.aec_level = static_cast<int32_t>(cfg.aec_level);
  out.aec_delay_ms = cfg.aec_delay_ms;
  out.anr_level = static_cast<int32_t>(cfg.ns_level);
  out.agc_target = cfg.agc_target_db;
  out.agc_max_gain = static_cast<int32_t>(cfg.agc_max_gain);
  out.agc_compress = static_cast<int32_t>(cfg.agc_compress);
  return out;
}

void from_msg_config(const MsgAudio3aConfig& in, mmf_audio_3a_config_t* cfg) {
  if (cfg == nullptr) {
    return;
  }
  cfg->aec_enable = in.aec_enable ? MMF_TRUE : MMF_FALSE;
  cfg->ns_enable = in.anr_enable ? MMF_TRUE : MMF_FALSE;
  cfg->agc_enable = in.agc_enable ? MMF_TRUE : MMF_FALSE;
  cfg->aec_level = static_cast<uint32_t>(in.aec_level < 0 ? 0 : in.aec_level);
  cfg->aec_delay_ms = in.aec_delay_ms;
  cfg->ns_level = static_cast<uint32_t>(in.anr_level < 0 ? 0 : in.anr_level);
  cfg->agc_target_db = in.agc_target;
  cfg->agc_max_gain = static_cast<uint32_t>(in.agc_max_gain < 0 ? 0 : in.agc_max_gain);
  cfg->agc_compress = static_cast<uint32_t>(in.agc_compress < 0 ? 0 : in.agc_compress);
}

mmf_result_t ensure_audio_msg_connection() {
  if (!g_audio_msg_runtime_retained) {
    std::string error;
    if (!mmf_cvi::retainAudioRuntime(&error)) {
      return ok_or_error(false, error);
    }
    g_audio_msg_runtime_retained = true;
  }
  if (g_audio_msg_si_id >= 0) {
    return MMF_OK;
  }
  CVI_S32 ret = CVI_IPCMSG_Connect(&g_audio_msg_si_id, kMsgServiceName, nullptr);
  if (ret != CVI_SUCCESS) {
    g_audio_msg_si_id = -1;
    set_last_error("CVI_IPCMSG_Connect(audio 3A) failed, ret=" + std::to_string(ret));
    return MMF_EIO;
  }
  return MMF_OK;
}

mmf_result_t audio_msg_request(CVI_U32 command, const void* request, CVI_U32 request_len,
                               void* response, CVI_U32 response_len) {
  mmf_result_t ready = ensure_audio_msg_connection();
  if (ready != MMF_OK) {
    return ready;
  }

  CVI_IPCMSG_MESSAGE_S* msg = CVI_IPCMSG_CreateMessage(
      msg_modfd(CVI_ID_AUD, 0, 0, true), command, const_cast<void*>(request), request_len);
  if (msg == nullptr) {
    set_last_error("CVI_IPCMSG_CreateMessage(audio 3A) failed");
    return MMF_EIO;
  }

  CVI_IPCMSG_MESSAGE_S* resp = nullptr;
  CVI_S32 ret = CVI_IPCMSG_SendSync(g_audio_msg_si_id, msg, &resp, kAudioIpcTimeoutMs);
  CVI_IPCMSG_DestroyMessage(msg);
  if (ret != CVI_SUCCESS) {
    set_last_error("CVI_IPCMSG_SendSync(audio 3A) failed, ret=" + std::to_string(ret));
    return MMF_EIO;
  }

  mmf_result_t result = MMF_OK;
  if (resp == nullptr) {
    set_last_error("audio 3A response is null");
    result = MMF_EIO;
  } else if (resp->s32RetVal != CVI_SUCCESS) {
    set_last_error("audio 3A command failed, ret=" + std::to_string(resp->s32RetVal));
    result = MMF_EIO;
  } else if (response_len > 0 &&
             (resp->pBody == nullptr || resp->u32BodyLen < response_len)) {
    set_last_error("audio 3A response body is invalid");
    result = MMF_EIO;
  } else if (response_len > 0) {
    std::memcpy(response, resp->pBody, response_len);
  }

  if (resp != nullptr) {
    CVI_IPCMSG_DestroyMessage(resp);
  }
  return result;
}

mmf_result_t push_3a_config_to_smallcore(const mmf_audio_3a_config_t& config) {
  MsgAudio3aConfig msg = to_msg_config(config);
  return audio_msg_request(kAudioCmdSet3AConfig, &msg, sizeof(msg), nullptr, 0);
}

mmf_result_t apply_3a_config_direct(const mmf_audio_3a_config_t& config) {
  if (!g_audio_msg_runtime_retained) {
    std::string error;
    if (!mmf_cvi::retainAudioRuntime(&error)) {
      return ok_or_error(false, error);
    }
    g_audio_msg_runtime_retained = true;
  }

  mmf_cvi::AudioTalkVqeConfig vqe = to_talk_vqe(config, 16000);
  AI_TALKVQE_CONFIG_S native;
  std::memset(&native, 0, sizeof(native));
  native.para_client_config = vqe.client_config;
  native.u32OpenMask = vqe.open_mask;
  native.s32WorkSampleRate = vqe.work_sample_rate;
  native.stAecCfg.para_aec_filter_len = vqe.aec.filter_length;
  native.stAecCfg.para_aes_std_thrd = vqe.aec.std_threshold;
  native.stAecCfg.para_aes_supp_coeff = vqe.aec.suppress_coeff;
  native.stAnrCfg.para_nr_snr_coeff = vqe.anr.snr_coeff;
  native.stAnrCfg.para_nr_init_sile_time = vqe.anr.initial_silence_time;
  native.stAgcCfg.para_agc_max_gain = vqe.agc.max_gain;
  native.stAgcCfg.para_agc_target_high = vqe.agc.target_high;
  native.stAgcCfg.para_agc_target_low = vqe.agc.target_low;
  native.stAgcCfg.para_agc_vad_ena = vqe.agc.vad_enabled ? CVI_TRUE : CVI_FALSE;
  native.stAecDelayCfg.para_aec_init_filter_len = vqe.delay.initial_filter_length;
  native.stAecDelayCfg.para_dg_target = vqe.delay.digital_gain_target;
  native.stAecDelayCfg.para_delay_sample = vqe.delay.delay_sample;
  native.para_notch_freq = vqe.notch_frequency;

  CVI_S32 ret = CVI_AI_SetTalkVqeAttr(0, 0, 0, 0, &native);
  if (ret != CVI_SUCCESS) {
    set_last_error("CVI_AI_SetTalkVqeAttr direct audio 3A failed, ret=" + std::to_string(ret));
    return MMF_EIO;
  }
  return MMF_OK;
}

mmf_result_t set_global_3a_config(const mmf_audio_3a_config_t& config) {
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  mmf_audio_3a_config_t old = *global_3a();
  *global_3a() = config;
  mmf_result_t ret = push_3a_config_to_smallcore(config);
  if (ret != MMF_OK && g_input_sessions > 0) {
    ret = apply_3a_config_direct(config);
  }
  if (ret != MMF_OK) {
    *global_3a() = old;
  }
  return ret;
}

}  // namespace

namespace mmf_cv184x {

void audio_3a_note_input_open(const mmf_audio_3a_config_t& config, bool enabled, bool configured) {
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *global_3a() = config;
  g_input_sessions += 1;
  if (enabled && configured) {
    g_applied_input_sessions += 1;
  }
}

void audio_3a_note_input_config(const mmf_audio_3a_config_t& config, bool enabled,
                                bool configured) {
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *global_3a() = config;
  if (enabled && configured && g_applied_input_sessions == 0) {
    g_applied_input_sessions = 1;
  } else if ((!enabled || !configured) && g_applied_input_sessions > 0) {
    g_applied_input_sessions -= 1;
  }
}

void audio_3a_note_input_close(bool enabled, bool configured) {
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  if (g_input_sessions > 0) {
    g_input_sessions -= 1;
  }
  if (enabled && configured && g_applied_input_sessions > 0) {
    g_applied_input_sessions -= 1;
  }
}

void audio_3a_note_output_open(bool provide_reference) {
  if (!provide_reference) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  g_playback_reference_sessions += 1;
}

void audio_3a_note_output_close(bool provide_reference) {
  if (!provide_reference) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  if (g_playback_reference_sessions > 0) {
    g_playback_reference_sessions -= 1;
  }
}

}  // namespace mmf_cv184x

extern "C" {

void mmf_audio_3a_get_default_config(mmf_audio_3a_config_t* config) {
  fill_default_3a(config);
}

mmf_result_t mmf_audio_3a_set_aec(mmf_bool_t enable) {
  mmf_audio_3a_config_t config;
  {
    std::lock_guard<std::mutex> lock(g_3a_mutex);
    config = *global_3a();
  }
  config.aec_enable = enable;
  return set_global_3a_config(config);
}

mmf_result_t mmf_audio_3a_get_aec(mmf_bool_t* enable) {
  if (enable == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *enable = global_3a()->aec_enable;
  return MMF_OK;
}

mmf_result_t mmf_audio_3a_set_ns(mmf_bool_t enable) {
  mmf_audio_3a_config_t config;
  {
    std::lock_guard<std::mutex> lock(g_3a_mutex);
    config = *global_3a();
  }
  config.ns_enable = enable;
  return set_global_3a_config(config);
}

mmf_result_t mmf_audio_3a_get_ns(mmf_bool_t* enable) {
  if (enable == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *enable = global_3a()->ns_enable;
  return MMF_OK;
}

mmf_result_t mmf_audio_3a_set_agc(mmf_bool_t enable) {
  mmf_audio_3a_config_t config;
  {
    std::lock_guard<std::mutex> lock(g_3a_mutex);
    config = *global_3a();
  }
  config.agc_enable = enable;
  return set_global_3a_config(config);
}

mmf_result_t mmf_audio_3a_get_agc(mmf_bool_t* enable) {
  if (enable == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *enable = global_3a()->agc_enable;
  return MMF_OK;
}

mmf_result_t mmf_audio_3a_set_aec_level(uint32_t level) {
  mmf_audio_3a_config_t config;
  {
    std::lock_guard<std::mutex> lock(g_3a_mutex);
    config = *global_3a();
  }
  config.aec_level = level;
  return set_global_3a_config(config);
}

mmf_result_t mmf_audio_3a_get_aec_level(uint32_t* level) {
  if (level == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *level = global_3a()->aec_level;
  return MMF_OK;
}

mmf_result_t mmf_audio_3a_set_aec_delay(int32_t delay_ms) {
  mmf_audio_3a_config_t config;
  {
    std::lock_guard<std::mutex> lock(g_3a_mutex);
    config = *global_3a();
  }
  config.aec_delay_ms = delay_ms;
  return set_global_3a_config(config);
}

mmf_result_t mmf_audio_3a_get_aec_delay(int32_t* delay_ms) {
  if (delay_ms == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *delay_ms = global_3a()->aec_delay_ms;
  return MMF_OK;
}

mmf_result_t mmf_audio_3a_set_ns_level(uint32_t level) {
  mmf_audio_3a_config_t config;
  {
    std::lock_guard<std::mutex> lock(g_3a_mutex);
    config = *global_3a();
  }
  config.ns_level = level;
  return set_global_3a_config(config);
}

mmf_result_t mmf_audio_3a_get_ns_level(uint32_t* level) {
  if (level == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *level = global_3a()->ns_level;
  return MMF_OK;
}

mmf_result_t mmf_audio_3a_set_agc_target(int32_t target_db) {
  mmf_audio_3a_config_t config;
  {
    std::lock_guard<std::mutex> lock(g_3a_mutex);
    config = *global_3a();
  }
  config.agc_target_db = target_db;
  return set_global_3a_config(config);
}

mmf_result_t mmf_audio_3a_get_agc_target(int32_t* target_db) {
  if (target_db == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *target_db = global_3a()->agc_target_db;
  return MMF_OK;
}

mmf_result_t mmf_audio_3a_set_agc_max_gain(uint32_t max_gain) {
  mmf_audio_3a_config_t config;
  {
    std::lock_guard<std::mutex> lock(g_3a_mutex);
    config = *global_3a();
  }
  config.agc_max_gain = max_gain;
  return set_global_3a_config(config);
}

mmf_result_t mmf_audio_3a_get_agc_max_gain(uint32_t* max_gain) {
  if (max_gain == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *max_gain = global_3a()->agc_max_gain;
  return MMF_OK;
}

mmf_result_t mmf_audio_3a_set_agc_compress(uint32_t compress) {
  mmf_audio_3a_config_t config;
  {
    std::lock_guard<std::mutex> lock(g_3a_mutex);
    config = *global_3a();
  }
  config.agc_compress = compress;
  return set_global_3a_config(config);
}

mmf_result_t mmf_audio_3a_get_agc_compress(uint32_t* compress) {
  if (compress == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  *compress = global_3a()->agc_compress;
  return MMF_OK;
}

mmf_result_t mmf_audio_3a_get_status(mmf_audio_3a_status_t* status) {
  if (status == nullptr)
    return MMF_EINVAL;
  std::lock_guard<std::mutex> lock(g_3a_mutex);
  std::memset(status, 0, sizeof(*status));
  status->supported = MMF_TRUE;
  status->applied = g_applied_input_sessions > 0 ? MMF_TRUE : MMF_FALSE;
  status->has_playback_reference = g_playback_reference_sessions > 0 ? MMF_TRUE : MMF_FALSE;
  status->config = *global_3a();
  return MMF_OK;
}

}  // extern "C"
