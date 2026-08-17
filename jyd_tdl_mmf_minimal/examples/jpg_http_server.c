#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mmf/mmf.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo) {
  (void)signo;
  g_stop = 1;
}

static mmf_camera_source_t parse_source(const char* name) {
  if (name == NULL)
    return MMF_CAMERA_SRC_LIVE;
  if (strcmp(name, "main") == 0)
    return MMF_CAMERA_SRC_MAIN;
  if (strcmp(name, "ai") == 0)
    return MMF_CAMERA_SRC_AI;
  if (strcmp(name, "subrgb") == 0)
    return MMF_CAMERA_SRC_SUBRGB;
  if (strcmp(name, "screen") == 0)
    return MMF_CAMERA_SRC_SCREEN;
  return MMF_CAMERA_SRC_LIVE;
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
  return "live";
}

int main(int argc, char** argv) {
  mmf_jpg_http_config_t config;
  mmf_jpg_http_server_t* server = 0;
  mmf_system_config_t sys = {MMF_TRUE, MMF_TRUE, 3000};

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  mmf_system_init(&sys);
  mmf_jpg_http_get_default_config(&config);
  config.camera_source = argc >= 3 ? parse_source(argv[2]) : MMF_CAMERA_SRC_LIVE;
  config.mode = config.camera_source == MMF_CAMERA_SRC_SCREEN
                    ? MMF_JPG_HTTP_MODE_DISPLAY_PULL
                    : MMF_JPG_HTTP_MODE_CAMERA_PULL;
  config.port = argc >= 2 ? (uint16_t)atoi(argv[1]) : 18090;
  config.jpeg_quality = argc >= 4 ? (uint32_t)atoi(argv[3]) : 92;
  config.fps = argc >= 5 ? (uint32_t)atoi(argv[4]) : 8;
  config.venc_channel = argc >= 6 ? (uint32_t)atoi(argv[5]) : config.venc_channel;
  if (config.port == 0)
    config.port = 18090;
  if (config.jpeg_quality < 1)
    config.jpeg_quality = 1;
  if (config.jpeg_quality > 99)
    config.jpeg_quality = 99;

  if (mmf_jpg_http_open(&config, &server) != MMF_OK) {
    printf("jpg http open failed: %s\n", mmf_system_last_error());
    mmf_system_deinit();
    return 1;
  }
  if (mmf_jpg_http_start_stream(server) != MMF_OK) {
    printf("jpg http start failed: %s\n", mmf_system_last_error());
    mmf_jpg_http_close(server);
    mmf_system_deinit();
    return 1;
  }

  printf("jpg http server started\n");
  printf("  source=%s mode=%s venc=%u quality=%u fps=%u%s\n",
         source_name(config.camera_source),
         config.mode == MMF_JPG_HTTP_MODE_DISPLAY_PULL ? "display" : "camera",
         config.venc_channel, config.jpeg_quality, config.fps,
         config.fps == 0 ? " (unlimited)" : "");
  printf("  snapshot: http://<board-ip>:%u%s\n", config.port, config.snapshot_path);
  printf("  stream  : http://<board-ip>:%u%s\n", config.port, config.stream_path);
  printf("press Ctrl+C to stop\n");

  while (!g_stop) {
    mmf_jpg_http_status_t status;
    if (mmf_jpg_http_get_status(server, &status) == MMF_OK) {
      printf("streaming=%d clients=%u frames=%llu bytes=%llu last_seq=%llu\n", status.streaming,
             status.client_count, (unsigned long long)status.frames_published,
             (unsigned long long)status.bytes_published,
             (unsigned long long)status.last_frame_sequence);
    }
    sleep(3);
  }

  mmf_jpg_http_close(server);
  mmf_system_deinit();
  printf("jpg http server stopped\n");
  return 0;
}
