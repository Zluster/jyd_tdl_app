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

static int write_osd_ppm(const char* path) {
  FILE* fp = fopen(path, "w");
  if (fp == NULL)
    return -1;
  const int width = 240;
  const int height = 96;
  fprintf(fp, "P3\n%d %d\n255\n", width, height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      int r = x < width / 3 ? 255 : 0;
      int g = x >= width / 3 && x < width * 2 / 3 ? 255 : 0;
      int b = x >= width * 2 / 3 ? 255 : 0;
      if (y >= height / 2) {
        r = 255 - r;
        g = 255 - g;
        b = 255 - b;
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

int main(int argc, char** argv) {
  mmf_display_config_t config;
  mmf_display_t* display = 0;
  mmf_camera_source_t source = argc >= 2 ? parse_source(argv[1]) : MMF_CAMERA_SRC_LIVE;
  int enable_osd = argc >= 3 ? atoi(argv[2]) : 1;
  const char* osd_path = "/tmp/mmf_display_osd.ppm";
  mmf_system_config_t sys = {MMF_TRUE, MMF_TRUE, 3000};

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  mmf_system_init(&sys);
  mmf_display_get_default_config(&config);
  if (mmf_display_open(&config, &display) != MMF_OK) {
    printf("display open failed: %s\n", mmf_system_last_error());
    mmf_system_deinit();
    return 1;
  }
  if (mmf_display_bind_camera(display, source) != MMF_OK) {
    printf("display bind %s failed: %s\n", source_name(source), mmf_system_last_error());
    mmf_display_close(display);
    mmf_system_deinit();
    return 2;
  }

  if (enable_osd && write_osd_ppm(osd_path) == 0) {
    mmf_display_show_options_t show;
    mmf_display_get_default_show_options(&show);
    show.target = MMF_DISPLAY_TARGET_OSD;
    show.dst_rect.x = 24;
    show.dst_rect.y = 24;
    show.dst_rect.width = 240;
    show.dst_rect.height = 96;
    show.osd_handle = 124;
    show.osd_layer = 20;
    if (mmf_display_show_image_file(display, osd_path, &show) != MMF_OK) {
      printf("display osd failed: %s\n", mmf_system_last_error());
    }
  }

  printf("display started: source=%s osd=%d\n", source_name(source), enable_osd);
  printf("press Ctrl+C to stop\n");
  while (!g_stop) {
    mmf_display_status_t status;
    if (mmf_display_get_status(display, &status) == MMF_OK) {
      printf("display status: showing=%d mode=%d source=%s\n", status.showing, status.mode,
             source_name(status.bound_camera_source));
    }
    sleep(3);
  }

  mmf_display_clear_overlay(display, 124);
  mmf_display_unbind(display);
  mmf_display_close(display);
  mmf_system_deinit();
  printf("display stopped\n");
  return 0;
}
