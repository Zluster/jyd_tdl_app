#include "mmf/mmf_touch.h"

#include <stdlib.h>
#include <string.h>

struct mmf_touch {
  mmf_touch_config_t config;
  uint64_t event_count;
};

void mmf_touch_get_default_config(mmf_touch_config_t* config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->input_device = "/dev/input/event0";
  config->screen_width = 720;
  config->screen_height = 1280;
  config->read_timeout_ms = 1000;
}

mmf_result_t mmf_touch_open(const mmf_touch_config_t* config, mmf_touch_t** touch) {
  if (config == NULL || touch == NULL) {
    return MMF_EINVAL;
  }
  *touch = (mmf_touch_t*)calloc(1, sizeof(**touch));
  if (*touch == NULL) {
    return MMF_ENOMEM;
  }
  (*touch)->config = *config;
  return MMF_ENOTSUP;
}

void mmf_touch_close(mmf_touch_t* touch) {
  free(touch);
}

mmf_result_t mmf_touch_read_event(mmf_touch_t* touch, mmf_touch_event_t* event,
                                  uint32_t timeout_ms) {
  (void)touch;
  (void)event;
  (void)timeout_ms;
  return MMF_ENOTSUP;
}

mmf_result_t mmf_touch_get_status(mmf_touch_t* touch, mmf_touch_status_t* status) {
  if (touch == NULL || status == NULL) {
    return MMF_EINVAL;
  }
  memset(status, 0, sizeof(*status));
  status->opened = MMF_TRUE;
  status->screen_width = touch->config.screen_width;
  status->screen_height = touch->config.screen_height;
  status->event_count = touch->event_count;
  return MMF_ENOTSUP;
}
