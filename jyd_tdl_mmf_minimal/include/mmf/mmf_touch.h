#ifndef MMF_TOUCH_H
#define MMF_TOUCH_H

#include "mmf_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmf_touch mmf_touch_t;

typedef enum {
  MMF_TOUCH_EVT_UNKNOWN = 0,
  MMF_TOUCH_EVT_DOWN = 1,
  MMF_TOUCH_EVT_MOVE = 2,
  MMF_TOUCH_EVT_UP = 3,
  MMF_TOUCH_EVT_CANCEL = 4,
} mmf_touch_event_type_t;

typedef struct {
  int32_t x;
  int32_t y;
  int32_t pressure;
  int32_t tracking_id;
  uint64_t timestamp_us;
} mmf_touch_point_t;

typedef struct {
  mmf_touch_event_type_t type;
  uint32_t point_count;
  mmf_touch_point_t points[10];
} mmf_touch_event_t;

typedef struct {
  const char* input_device;
  uint32_t screen_width;
  uint32_t screen_height;
  uint32_t read_timeout_ms;
  mmf_bool_t swap_xy;
  mmf_bool_t invert_x;
  mmf_bool_t invert_y;
} mmf_touch_config_t;

typedef struct {
  mmf_bool_t opened;
  uint32_t screen_width;
  uint32_t screen_height;
  uint64_t event_count;
} mmf_touch_status_t;

void mmf_touch_get_default_config(mmf_touch_config_t* config);
mmf_result_t mmf_touch_open(const mmf_touch_config_t* config, mmf_touch_t** touch);
void mmf_touch_close(mmf_touch_t* touch);
mmf_result_t mmf_touch_read_event(mmf_touch_t* touch, mmf_touch_event_t* event,
                                  uint32_t timeout_ms);
mmf_result_t mmf_touch_get_status(mmf_touch_t* touch, mmf_touch_status_t* status);

#ifdef __cplusplus
}
#endif

#endif
