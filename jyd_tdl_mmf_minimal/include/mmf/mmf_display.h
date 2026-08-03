#ifndef MMF_DISPLAY_H
#define MMF_DISPLAY_H

#include "mmf_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmf_display mmf_display_t;

typedef enum {
  MMF_DISPLAY_MODE_NONE = 0,
  MMF_DISPLAY_MODE_CAMERA_BIND = 1,
  MMF_DISPLAY_MODE_FRAME_PUSH = 2,
  MMF_DISPLAY_MODE_IMAGE_FILE = 3,
  MMF_DISPLAY_MODE_OVERLAY = 4,
} mmf_display_mode_t;

typedef enum {
  MMF_DISPLAY_TARGET_VO = 0,
  MMF_DISPLAY_TARGET_OSD = 1,
} mmf_display_target_t;

typedef struct {
  uint32_t panel_width;
  uint32_t panel_height;
  uint32_t layer;
  uint32_t channel;
  mmf_rotate_t rotate;
  mmf_scale_mode_t scale_mode;
  uint32_t bg_color;
  mmf_rect_t window;
} mmf_display_config_t;

typedef struct {
  mmf_bool_t opened;
  mmf_bool_t showing;
  mmf_display_mode_t mode;
  uint32_t layer;
  uint32_t channel;
  uint64_t frame_count;
  mmf_camera_source_t bound_camera_source;
  mmf_camera_device_t bound_camera_device;
  mmf_rect_t window;
} mmf_display_status_t;

typedef struct {
  mmf_display_target_t target;
  mmf_rect_t dst_rect;
  uint32_t osd_handle;
  uint32_t osd_layer;
  mmf_bool_t clear_before_show;
} mmf_display_show_options_t;

typedef struct {
  const char* path;
  uint32_t jpeg_quality;
  uint32_t timeout_ms;
  mmf_bool_t include_osd;
} mmf_display_snapshot_config_t;

void mmf_display_get_default_config(mmf_display_config_t* config);
void mmf_display_get_default_show_options(mmf_display_show_options_t* options);
mmf_result_t mmf_display_open(const mmf_display_config_t* config, mmf_display_t** display);
void mmf_display_close(mmf_display_t* display);
mmf_result_t mmf_display_get_status(mmf_display_t* display, mmf_display_status_t* status);
mmf_result_t mmf_display_set_window(mmf_display_t* display, const mmf_rect_t* window,
                                    mmf_scale_mode_t scale_mode, uint32_t bg_color);
mmf_result_t mmf_display_bind_camera(mmf_display_t* display, mmf_camera_source_t source);
mmf_result_t mmf_display_bind_camera_device(mmf_display_t* display,
                                            mmf_camera_source_t source,
                                            mmf_camera_device_t device);
mmf_result_t mmf_display_unbind(mmf_display_t* display);
mmf_result_t mmf_display_show_frame(mmf_display_t* display, const mmf_video_frame_t* frame,
                                    const mmf_display_show_options_t* options);
mmf_result_t mmf_display_show_image_file(mmf_display_t* display, const char* path,
                                         const mmf_display_show_options_t* options);
mmf_result_t mmf_display_snapshot(mmf_display_t* display,
                                  const mmf_display_snapshot_config_t* config);
mmf_result_t mmf_display_clear(mmf_display_t* display);
mmf_result_t mmf_display_clear_overlay(mmf_display_t* display, uint32_t osd_handle);

#ifdef __cplusplus
}
#endif

#endif
