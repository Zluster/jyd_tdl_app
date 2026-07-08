#ifndef MMF_SYSTEM_H
#define MMF_SYSTEM_H

#include "mmf_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  mmf_bool_t auto_connect_msg;
  mmf_bool_t wait_remote_ready;
  uint32_t wait_timeout_ms;
} mmf_system_config_t;

typedef struct {
  mmf_bool_t local_ready;
  mmf_bool_t remote_ready;
  mmf_bool_t msg_connected;
  uint32_t reconnect_count;
} mmf_system_status_t;

mmf_result_t mmf_system_init(const mmf_system_config_t* config);
void mmf_system_deinit(void);
mmf_result_t mmf_system_get_status(mmf_system_status_t* status);
mmf_result_t mmf_system_wait_ready(uint32_t timeout_ms);
const char* mmf_system_last_error(void);
mmf_version_t mmf_system_version(void);
mmf_result_t mmf_system_set_vpss_scale_mode(mmf_camera_source_t source, mmf_scale_mode_t mode);
mmf_result_t mmf_system_get_vpss_scale_mode(mmf_camera_source_t source, mmf_scale_mode_t* mode);

#ifdef __cplusplus
}
#endif

#endif
