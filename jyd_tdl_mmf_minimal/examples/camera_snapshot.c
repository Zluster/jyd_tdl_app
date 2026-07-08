#include "mmf/mmf.h"

int main(void) {
  mmf_system_config_t sys = {1, 1, 3000};
  mmf_camera_config_t camera_cfg;
  mmf_camera_t* camera = 0;

  if (mmf_system_init(&sys) != MMF_OK) {
    return 1;
  }
  if (mmf_camera_get_default_config(MMF_CAMERA_SRC_AI, &camera_cfg) != MMF_OK) {
    return 2;
  }
  if (mmf_camera_open(&camera_cfg, &camera) != MMF_OK) {
    return 3;
  }
  if (mmf_camera_snapshot(camera, "/tmp/ai.jpg", MMF_CODEC_JPEG) != MMF_OK) {
    mmf_camera_close(camera);
    return 4;
  }
  mmf_camera_close(camera);
  return 0;
}
