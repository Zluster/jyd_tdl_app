#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
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

static int write_osd_ppm(const char* path) {
  FILE* fp = fopen(path, "w");
  if (fp == NULL)
    return -1;
  const int width = 240;
  const int height = 96;
  fprintf(fp, "P3\n%d %d\n255\n", width, height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      int r = 0;
      int g = 0;
      int b = 0;
      if (x < width / 3) {
        r = 255;
        g = y < height / 2 ? 255 : 64;
      } else if (x < width * 2 / 3) {
        g = 255;
        b = y < height / 2 ? 255 : 64;
      } else {
        r = y < height / 2 ? 64 : 255;
        b = 255;
      }
      if (x < 4 || y < 4 || x >= width - 4 || y >= height - 4) {
        r = 255;
        g = 255;
        b = 255;
      }
      fprintf(fp, "%d %d %d ", r, g, b);
    }
    fprintf(fp, "\n");
  }
  fclose(fp);
  return 0;
}

static int camera_roundtrip(mmf_camera_source_t source) {
  mmf_camera_config_t camera_cfg;
  mmf_camera_t* camera = NULL;
  mmf_video_frame_t frame;
  mmf_jpg_encoder_config_t enc_cfg;
  mmf_jpg_encoder_t* encoder = NULL;
  mmf_packet_t jpeg;
  mmf_video_frame_t decoded;
  int failed = 0;

  mmf_result_t ret = mmf_camera_get_default_config(source, &camera_cfg);
  if (ret == MMF_OK)
    ret = mmf_camera_open(&camera_cfg, &camera);
  if (ret != MMF_OK) {
    printf("roundtrip camera open %s failed: %d %s\n", source_name(source), ret,
           mmf_system_last_error());
    return -1;
  }

  memset(&frame, 0, sizeof(frame));
  ret = mmf_camera_get_frame(camera, &frame, 1000);
  if (ret != MMF_OK) {
    printf("roundtrip get frame failed: %d %s\n", ret, mmf_system_last_error());
    mmf_camera_close(camera);
    return -1;
  }

  mmf_jpg_get_default_encoder_config(&enc_cfg);
  enc_cfg.width = frame.width;
  enc_cfg.height = frame.height;
  enc_cfg.input_format = frame.pixel_format;
  enc_cfg.quality = 92;
  enc_cfg.venc_channel = 0;
  ret = mmf_jpg_encoder_open(&enc_cfg, &encoder);
  memset(&jpeg, 0, sizeof(jpeg));
  if (ret == MMF_OK)
    ret = mmf_jpg_encode_frame(encoder, &frame, &jpeg);
  if (ret != MMF_OK) {
    printf("roundtrip encode failed: %d %s\n", ret, mmf_system_last_error());
    failed = 1;
  } else {
    memset(&decoded, 0, sizeof(decoded));
    ret = mmf_jpg_decode(jpeg.data, jpeg.bytes, MMF_PIXFMT_NV21, &decoded);
    if (ret != MMF_OK) {
      printf("roundtrip decode failed: %d %s\n", ret, mmf_system_last_error());
      failed = 1;
    } else {
      printf("roundtrip %s: %ux%u fmt=%d jpeg=%u decoded=%ux%u fmt=%d\n", source_name(source),
             frame.width, frame.height, frame.pixel_format, (unsigned)jpeg.bytes, decoded.width,
             decoded.height, decoded.pixel_format);
    }
  }

  if (encoder != NULL) {
    mmf_jpg_release_packet(encoder, &jpeg);
    mmf_jpg_encoder_close(encoder);
  }
  mmf_camera_put_frame(camera, &frame);
  mmf_camera_close(camera);
  return failed ? -1 : 0;
}

static int audio_probe(void) {
  mmf_audio_input_config_t cfg;
  mmf_audio_input_t* input = NULL;
  mmf_audio_frame_t frame;
  mmf_audio_get_default_input_config(&cfg);
  cfg.enable_3a = MMF_FALSE;
  mmf_result_t ret = mmf_audio_input_open(&cfg, &input);
  if (ret != MMF_OK) {
    printf("audio open failed: %d %s\n", ret, mmf_system_last_error());
    return -1;
  }
  memset(&frame, 0, sizeof(frame));
  ret = mmf_audio_input_read(input, &frame, 1000);
  if (ret == MMF_OK) {
    printf("audio read: %uHz %uch bytes=%u seq=%llu\n", frame.sample_rate, frame.channels,
           (unsigned)frame.bytes, (unsigned long long)frame.sequence);
    mmf_audio_input_release(input, &frame);
  } else {
    printf("audio read failed: %d %s\n", ret, mmf_system_last_error());
  }
  mmf_audio_input_close(input);
  return ret == MMF_OK ? 0 : -1;
}

static int http_self_probe(unsigned short port, const char* path, size_t* bytes_out) {
  char request[256];
  char buffer[1024];
  size_t total = 0;
  int fd = -1;

  for (int attempt = 0; attempt < 30; ++attempt) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
      break;
    }
    close(fd);
    fd = -1;
    usleep(100000);
  }
  if (fd < 0) {
    return -1;
  }

  snprintf(request, sizeof(request),
           "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n", path);
  if (send(fd, request, strlen(request), 0) < 0) {
    close(fd);
    return -1;
  }
  while (1) {
    ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
    if (n > 0) {
      total += (size_t)n;
      continue;
    }
    if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    close(fd);
    return -1;
  }
  close(fd);
  if (bytes_out != NULL) {
    *bytes_out = total;
  }
  return total > 0 ? 0 : -1;
}

int main(int argc, char** argv) {
  const int port = argc >= 2 ? atoi(argv[1]) : 18090;
  const int duration_s = argc >= 3 ? atoi(argv[2]) : 0;
  const mmf_camera_source_t source = parse_source(argc >= 4 ? argv[3] : "live");
  const char* osd_path = "/tmp/mmf_pipeline_soak_osd.ppm";

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);
  mkdir("/tmp/mmf_pipeline_soak", 0755);

  mmf_system_config_t sys = {MMF_TRUE, MMF_TRUE, 3000};
  mmf_result_t ret = mmf_system_init(&sys);
  if (ret != MMF_OK) {
    printf("system init failed: %d %s\n", ret, mmf_system_last_error());
    return 1;
  }

  mmf_display_config_t display_cfg;
  mmf_display_t* display = NULL;
  mmf_display_get_default_config(&display_cfg);
  ret = mmf_display_open(&display_cfg, &display);
  if (ret == MMF_OK)
    ret = mmf_display_bind_camera(display, source);
  if (ret != MMF_OK) {
    printf("display bind failed: %d %s\n", ret, mmf_system_last_error());
  } else if (write_osd_ppm(osd_path) == 0) {
    mmf_display_show_options_t show;
    mmf_display_get_default_show_options(&show);
    show.target = MMF_DISPLAY_TARGET_OSD;
    show.dst_rect.x = 24;
    show.dst_rect.y = 24;
    show.dst_rect.width = 240;
    show.dst_rect.height = 96;
    show.osd_handle = 124;
    show.osd_layer = 20;
    ret = mmf_display_show_image_file(display, osd_path, &show);
    printf("display osd: %s (%d)\n", ret == MMF_OK ? "ok" : "fail", ret);
    if (ret != MMF_OK)
      printf("osd error: %s\n", mmf_system_last_error());
  }

  mmf_jpg_http_config_t http_cfg;
  mmf_jpg_http_server_t* http = NULL;
  mmf_jpg_http_get_default_config(&http_cfg);
  http_cfg.port = (uint16_t)(port > 0 ? port : 18090);
  http_cfg.mode = MMF_JPG_HTTP_MODE_CAMERA_PULL;
  http_cfg.camera_source = source;
  http_cfg.fps = 8;
  http_cfg.jpeg_quality = 92;
  http_cfg.venc_channel = 1;
  ret = mmf_jpg_http_open(&http_cfg, &http);
  if (ret == MMF_OK)
    ret = mmf_jpg_http_start_stream(http);
  if (ret != MMF_OK) {
    printf("http start failed: %d %s\n", ret, mmf_system_last_error());
    if (display)
      mmf_display_close(display);
    mmf_system_deinit();
    return 1;
  }

  printf("pipeline soak running\n");
  printf("  source=%s http_venc=1 test_venc=1\n", source_name(source));
  printf("  snapshot: http://<board-ip>:%u%s\n", http_cfg.port, http_cfg.snapshot_path);
  printf("  stream  : http://<board-ip>:%u%s\n", http_cfg.port, http_cfg.stream_path);
  printf("  duration=%d seconds, 0 means until Ctrl+C\n", duration_s);
  size_t self_probe_bytes = 0;
  if (http_self_probe(http_cfg.port, http_cfg.snapshot_path, &self_probe_bytes) == 0) {
    printf("http self-probe: ok bytes=%u path=%s\n", (unsigned)self_probe_bytes,
           http_cfg.snapshot_path);
  } else {
    printf("http self-probe: failed port=%u path=%s error=%s\n", http_cfg.port,
           http_cfg.snapshot_path, mmf_system_last_error());
  }

  const time_t start = time(NULL);
  int tick = 0;
  while (!g_stop && (duration_s <= 0 || (int)(time(NULL) - start) < duration_s)) {
    mmf_jpg_http_status_t status;
    memset(&status, 0, sizeof(status));
    mmf_jpg_http_get_status(http, &status);
    printf("status tick=%d streaming=%d clients=%u http_frames=%llu http_bytes=%llu\n", tick,
           status.streaming, status.client_count, (unsigned long long)status.frames_published,
           (unsigned long long)status.bytes_published);
    if (status.client_count == 0 && (tick % 2) == 0) {
      camera_roundtrip(source);
    } else if (status.client_count > 0 && (tick % 2) == 0) {
      printf("roundtrip skipped while HTTP client is connected\n");
    }
    if ((tick % 5) == 0) {
      audio_probe();
    }
    sleep(1);
    tick++;
  }

  mmf_jpg_http_stop_stream(http);
  mmf_jpg_http_close(http);
  if (display) {
    mmf_display_clear_overlay(display, 124);
    mmf_display_close(display);
  }
  mmf_system_deinit();
  return 0;
}
