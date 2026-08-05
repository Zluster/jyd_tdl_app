#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mmf/mmf.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo) {
  (void)signo;
  g_stop = 1;
}

static const char* source_name(mmf_camera_source_t source) {
  switch (source) {
    case MMF_CAMERA_SRC_MAIN:
      return "main";
    case MMF_CAMERA_SRC_AI:
      return "ai";
    case MMF_CAMERA_SRC_LIVE:
      return "live";
    case MMF_CAMERA_SRC_SUBRGB:
      return "subrgb";
    case MMF_CAMERA_SRC_SCREEN:
      return "screen";
  }
  return "unknown";
}

static int log_outputs(void) {
  mmf_camera_output_desc_t outputs[10];
  size_t count = 0;
  mmf_result_t ret = mmf_camera_list_outputs(outputs, 10, &count);
  if (ret != MMF_OK) {
    printf("list outputs failed: %d %s\n", ret, mmf_system_last_error());
    return -1;
  }
  for (size_t i = 0; i < count; ++i) {
    printf("output %-7s grp=%d chn=%d %ux%u fmt=%d scale=%d available=%d\n",
           outputs[i].name ? outputs[i].name : source_name(outputs[i].source),
           outputs[i].vpss_group, outputs[i].vpss_channel, outputs[i].width, outputs[i].height,
           outputs[i].pixel_format, outputs[i].scale_mode, outputs[i].available);
  }
  return 0;
}

static int sample_frame(mmf_camera_source_t source) {
  mmf_camera_config_t cfg;
  mmf_camera_t* camera = NULL;
  mmf_video_frame_t frame;
  mmf_result_t ret = mmf_camera_get_default_config(source, &cfg);
  if (ret == MMF_OK)
    ret = mmf_camera_open(&cfg, &camera);
  if (ret != MMF_OK) {
    printf("camera open %s failed: %d %s\n", source_name(source), ret, mmf_system_last_error());
    return -1;
  }
  memset(&frame, 0, sizeof(frame));
  ret = mmf_camera_get_frame(camera, &frame, 1000);
  if (ret == MMF_OK) {
    printf("frame %-7s %ux%u fmt=%d seq=%llu pts=%llu\n", source_name(source), frame.width,
           frame.height, frame.pixel_format, (unsigned long long)frame.sequence,
           (unsigned long long)frame.timestamp_us);
    mmf_camera_put_frame(camera, &frame);
  } else {
    printf("get frame %s failed: %d %s\n", source_name(source), ret, mmf_system_last_error());
  }
  mmf_camera_close(camera);
  return ret == MMF_OK ? 0 : -1;
}

static int save_snapshot(mmf_camera_source_t source, const char* dir, int index) {
  mmf_camera_config_t cfg;
  mmf_camera_t* camera = NULL;
  char path[256];
  mmf_result_t ret = mmf_camera_get_default_config(source, &cfg);
  if (ret == MMF_OK)
    ret = mmf_camera_open(&cfg, &camera);
  if (ret != MMF_OK) {
    printf("snapshot open %s failed: %d %s\n", source_name(source), ret, mmf_system_last_error());
    return -1;
  }
  snprintf(path, sizeof(path), "%s/%s_%04d.jpg", dir, source_name(source), index);
  ret = mmf_camera_snapshot(camera, path, MMF_CODEC_JPEG);
  printf("snapshot %-7s -> %s: %s (%d)\n", source_name(source), path, ret == MMF_OK ? "ok" : "fail",
         ret);
  if (ret != MMF_OK)
    printf("error: %s\n", mmf_system_last_error());
  mmf_camera_close(camera);
  return ret == MMF_OK ? 0 : -1;
}

int main(int argc, char** argv) {
  int duration_s = argc >= 2 ? atoi(argv[1]) : 0;
  int port = argc >= 3 ? atoi(argv[2]) : 18090;
  if (port <= 0 || port > 65535)
    port = 18090;

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  mmf_system_config_t sys = {MMF_TRUE, MMF_TRUE, 3000};
  mmf_result_t ret = mmf_system_init(&sys);
  if (ret != MMF_OK) {
    printf("system init failed: %d %s\n", ret, mmf_system_last_error());
    return 1;
  }

  log_outputs();

  mmf_jpg_http_config_t http_cfg;
  mmf_jpg_http_server_t* http = NULL;
  mmf_jpg_http_get_default_config(&http_cfg);
  http_cfg.port = (uint16_t)port;
  /* Pull the shared VPSS display output; do not change its VO binding. */
  http_cfg.mode = MMF_JPG_HTTP_MODE_CAMERA_PULL;
  http_cfg.camera_source = MMF_CAMERA_SRC_SCREEN;
  http_cfg.fps = 8;
  http_cfg.jpeg_quality = 92;
  http_cfg.venc_channel = 1;
  ret = mmf_jpg_http_open(&http_cfg, &http);
  if (ret == MMF_OK)
    ret = mmf_jpg_http_start_stream(http);
  if (ret != MMF_OK) {
    printf("http start failed: %d %s\n", ret, mmf_system_last_error());
    if (http)
      mmf_jpg_http_close(http);
    mmf_system_deinit();
    return 1;
  }
  printf("http ready: http://<board-ip>:%d%s and %s\n", port, http_cfg.snapshot_path,
         http_cfg.stream_path);

  const time_t start = time(NULL);
  while (!g_stop && (duration_s <= 0 || (int)(time(NULL) - start) < duration_s)) {
    sleep(1);
  }

  mmf_jpg_http_status_t http_status;
  memset(&http_status, 0, sizeof(http_status));
  mmf_jpg_http_get_status(http, &http_status);
  printf("http stats: frames=%llu bytes=%llu clients=%u last_seq=%llu\n",
         (unsigned long long)http_status.frames_published,
         (unsigned long long)http_status.bytes_published, http_status.client_count,
         (unsigned long long)http_status.last_frame_sequence);

  mmf_jpg_http_stop_stream(http);
  mmf_jpg_http_close(http);
  mmf_system_deinit();
  return 0;
}
