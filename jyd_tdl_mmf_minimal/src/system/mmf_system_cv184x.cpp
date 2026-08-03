#include "mmf_cv184x_common.hpp"

using namespace mmf_cv184x;

extern "C" {

mmf_result_t mmf_system_init(const mmf_system_config_t* config) {
  (void)config;
  std::string error;
  return ok_or_error(mmf_cvi::ensureMmfRuntimeInitialized(&error), error);
}

void mmf_system_deinit(void) {}

mmf_result_t mmf_system_get_status(mmf_system_status_t* status) {
  if (status == nullptr)
    return MMF_EINVAL;
  std::memset(status, 0, sizeof(*status));
  status->local_ready = MMF_TRUE;
  status->remote_ready = MMF_TRUE;
  status->msg_connected = MMF_TRUE;
  return MMF_OK;
}

mmf_result_t mmf_system_wait_ready(uint32_t timeout_ms) {
  (void)timeout_ms;
  return mmf_system_init(nullptr);
}

const char* mmf_system_last_error(void) {
  std::lock_guard<std::mutex> lock(g_error_mutex);
  return g_last_error.c_str();
}

mmf_version_t mmf_system_version(void) {
  mmf_version_t version;
  version.major = MMF_API_VERSION_MAJOR;
  version.minor = MMF_API_VERSION_MINOR;
  version.patch = MMF_API_VERSION_PATCH;
  version.git_version = "mmf_minimal_native";
  return version;
}

mmf_result_t mmf_system_set_vpss_scale_mode(mmf_camera_source_t source, mmf_scale_mode_t mode) {
  return mmf_camera_set_scale_mode(source, mode);
}

mmf_result_t mmf_system_get_vpss_scale_mode(mmf_camera_source_t source, mmf_scale_mode_t* mode) {
  return mmf_camera_get_scale_mode(source, mode);
}

}  // extern "C"
