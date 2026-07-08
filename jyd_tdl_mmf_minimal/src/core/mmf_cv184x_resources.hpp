#ifndef MMF_CV184X_RESOURCES_HPP
#define MMF_CV184X_RESOURCES_HPP

#include "mmf_cv184x_common.hpp"

namespace mmf_cv184x {

enum class CodecResourceType {
  Venc,
  Vdec,
};

struct CodecResourceLease {
  int fd;
  char path[128];
};

#define MMF_CODEC_RESOURCE_LEASE_INIT \
  {                                   \
    -1, {                             \
      0                               \
    }                                 \
  }

void codec_resource_lease_init(CodecResourceLease* lease);
bool codec_resource_lease_valid(const CodecResourceLease* lease);
void codec_resource_lease_release(CodecResourceLease* lease);

mmf_result_t acquire_codec_resource(CodecResourceType type, uint32_t channel, const char* owner,
                                    uint32_t timeout_ms, CodecResourceLease* lease);
mmf_result_t acquire_codec_operation(CodecResourceType type, const char* owner, uint32_t timeout_ms,
                                     CodecResourceLease* lease);

void audio_3a_note_input_open(const mmf_audio_3a_config_t& config, bool enabled, bool configured);
void audio_3a_note_input_config(const mmf_audio_3a_config_t& config, bool enabled, bool configured);
void audio_3a_note_input_close(bool enabled, bool configured);
void audio_3a_note_output_open(bool provide_reference);
void audio_3a_note_output_close(bool provide_reference);

}  // namespace mmf_cv184x

#endif
