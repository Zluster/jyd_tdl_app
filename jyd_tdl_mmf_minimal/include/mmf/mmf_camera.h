#ifndef MMF_CAMERA_H
#define MMF_CAMERA_H

#include "mmf_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmf_camera mmf_camera_t;

typedef enum {
  MMF_CAMERA_SRC_AI = 0,
  MMF_CAMERA_SRC_LIVE = 1,
  MMF_CAMERA_SRC_MAIN = 2,
  MMF_CAMERA_SRC_SUBRGB = 3,
  MMF_CAMERA_SRC_SCREEN = 4,
  MMF_CAMERA_SRC_RGB = 5,
} mmf_camera_source_t;

typedef enum {
  MMF_CAMERA_DEVICE_FRONT = 0,
  MMF_CAMERA_DEVICE_REAR = 1,
} mmf_camera_device_t;

typedef struct {
  mmf_camera_source_t source;
  mmf_camera_device_t device;
  uint32_t width;
  uint32_t height;
  mmf_pixel_format_t pixel_format;
  mmf_scale_mode_t scale_mode;
  uint32_t timeout_ms;
} mmf_camera_config_t;

typedef struct {
  mmf_bool_t opened;
  mmf_bool_t sensor_online;
  uint32_t sensor_id;
  mmf_camera_config_t active_config;
} mmf_camera_status_t;

typedef struct {
  mmf_camera_source_t source;
  mmf_camera_device_t device;
  const char* name;
  int32_t vpss_group;
  int32_t vpss_channel;
  uint32_t width;
  uint32_t height;
  uint32_t stride[3];
  uint32_t depth;
  mmf_pixel_format_t pixel_format;
  mmf_scale_mode_t scale_mode;
  mmf_bool_t mirror;
  mmf_bool_t flip;
  mmf_bool_t available;
} mmf_camera_output_desc_t;

typedef enum {
  MMF_CAMERA_AE_AUTO = 0,
  MMF_CAMERA_AE_MANUAL = 1,
} mmf_camera_ae_mode_t;

typedef enum {
  MMF_CAMERA_AWB_AUTO = 0,
  MMF_CAMERA_AWB_MANUAL = 1,
} mmf_camera_awb_mode_t;

typedef struct {
  uint32_t red_gain;
  uint32_t green_gain;
  uint32_t blue_gain;
} mmf_camera_wb_t;

mmf_result_t mmf_camera_get_default_config(mmf_camera_source_t source, mmf_camera_config_t* config);
mmf_result_t mmf_camera_open(const mmf_camera_config_t* config, mmf_camera_t** camera);
void mmf_camera_close(mmf_camera_t* camera);
mmf_result_t mmf_camera_get_status(mmf_camera_t* camera, mmf_camera_status_t* status);
mmf_result_t mmf_camera_list_outputs(mmf_camera_output_desc_t* outputs, size_t capacity,
                                     size_t* count);

mmf_result_t mmf_camera_get_frame(mmf_camera_t* camera, mmf_video_frame_t* frame,
                                  uint32_t timeout_ms);
mmf_result_t mmf_camera_put_frame(mmf_camera_t* camera, mmf_video_frame_t* frame);
mmf_result_t mmf_camera_release_frame(mmf_camera_t* camera, mmf_video_frame_t* frame);
mmf_result_t mmf_camera_snapshot(mmf_camera_t* camera, const char* path, mmf_codec_t codec);

mmf_result_t mmf_camera_set_scale_mode(mmf_camera_source_t source, mmf_scale_mode_t mode);
mmf_result_t mmf_camera_get_scale_mode(mmf_camera_source_t source, mmf_scale_mode_t* mode);

mmf_result_t mmf_camera_set_ae_mode(mmf_camera_ae_mode_t mode);
mmf_result_t mmf_camera_get_ae_mode(mmf_camera_ae_mode_t* mode);
mmf_result_t mmf_camera_set_awb_mode(mmf_camera_awb_mode_t mode);
mmf_result_t mmf_camera_get_awb_mode(mmf_camera_awb_mode_t* mode);
mmf_result_t mmf_camera_set_exposure(uint32_t exposure_us);
mmf_result_t mmf_camera_get_exposure(uint32_t* exposure_us);
mmf_result_t mmf_camera_set_gain(uint32_t analog_gain);
mmf_result_t mmf_camera_get_gain(uint32_t* analog_gain);
mmf_result_t mmf_camera_set_isp_gain(uint32_t digital_gain);
mmf_result_t mmf_camera_get_isp_gain(uint32_t* digital_gain);
mmf_result_t mmf_camera_set_wb(const mmf_camera_wb_t* wb);
mmf_result_t mmf_camera_get_wb(mmf_camera_wb_t* wb);
mmf_result_t mmf_camera_set_flip(mmf_bool_t enable);
mmf_result_t mmf_camera_get_flip(mmf_bool_t* enable);
mmf_result_t mmf_camera_set_mirror(mmf_bool_t enable);
mmf_result_t mmf_camera_get_mirror(mmf_bool_t* enable);

#ifdef __cplusplus
}
#endif

#endif
