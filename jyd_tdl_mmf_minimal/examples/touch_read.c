#include "mmf/mmf.h"

int main(void) {
  mmf_touch_config_t config;
  mmf_touch_t* touch = 0;
  mmf_touch_event_t event;

  mmf_touch_get_default_config(&config);
  if (mmf_touch_open(&config, &touch) != MMF_OK) {
    return 1;
  }
  if (mmf_touch_read_event(touch, &event, 1000) != MMF_OK) {
    mmf_touch_close(touch);
    return 2;
  }
  mmf_touch_close(touch);
  return 0;
}
