#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mmf/mmf.h"

#define PHOTO_CAPACITY 256
#define PATH_LEN 256

typedef struct {
  char photo_path[PATH_LEN];
  char thumb_path[PATH_LEN];
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint64_t sequence;
  uint64_t timestamp_us;
  size_t photo_bytes;
  size_t thumb_bytes;
  time_t created_at;
} PhotoItem;

typedef struct {
  char dir[PATH_LEN];
  PhotoItem photos[PHOTO_CAPACITY];
  size_t count;
  size_t current;
  int gallery_mode;
  mmf_display_t* display;
} AppState;

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

static const char* fmt_name(uint32_t fmt) {
  switch (fmt) {
    case MMF_PIXFMT_NV12:
      return "NV12";
    case MMF_PIXFMT_NV21:
      return "NV21";
    case MMF_PIXFMT_RGB888:
      return "RGB888";
    case MMF_PIXFMT_BGR888:
      return "BGR888";
    case MMF_PIXFMT_GRAY8:
      return "GRAY8";
    case MMF_PIXFMT_RGB888_PLANAR:
      return "RGB888_PLANAR";
    case MMF_PIXFMT_ARGB8888:
      return "ARGB8888";
    case MMF_PIXFMT_JPEG:
      return "JPEG";
    default:
      return "UNKNOWN";
  }
}

static int ensure_dir(const char* path) {
  if (mkdir(path, 0777) == 0 || errno == EEXIST)
    return 0;
  return -1;
}

static size_t file_size(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  return (size_t)st.st_size;
}

static int write_packet(const char* path, const mmf_packet_t* packet) {
  FILE* fp;
  if (packet == NULL || packet->data == NULL || packet->bytes == 0)
    return -1;
  fp = fopen(path, "wb");
  if (fp == NULL)
    return -1;
  if (fwrite(packet->data, 1, packet->bytes, fp) != packet->bytes) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return 0;
}

static int encode_source_to_jpeg(mmf_camera_source_t source, uint32_t quality, const char* path,
                                 PhotoItem* info, int fill_info) {
  mmf_camera_config_t cfg;
  mmf_camera_t* camera = NULL;
  mmf_video_frame_t frame;
  mmf_packet_t jpeg;
  mmf_result_t ret;

  ret = mmf_camera_get_default_config(source, &cfg);
  if (ret != MMF_OK) {
    printf("default camera config failed: %s\n", mmf_system_last_error());
    return -1;
  }
  ret = mmf_camera_open(&cfg, &camera);
  if (ret != MMF_OK) {
    printf("open camera %s failed: %s\n", source_name(source), mmf_system_last_error());
    return -1;
  }

  memset(&frame, 0, sizeof(frame));
  ret = mmf_camera_get_frame(camera, &frame, cfg.timeout_ms ? cfg.timeout_ms : 1000);
  if (ret != MMF_OK) {
    printf("get frame %s failed: %s\n", source_name(source), mmf_system_last_error());
    mmf_camera_close(camera);
    return -1;
  }

  memset(&jpeg, 0, sizeof(jpeg));
  ret = mmf_jpg_encode(&frame, quality, &jpeg);
  if (ret == MMF_OK && write_packet(path, &jpeg) != 0) {
    ret = MMF_EIO;
  }
  if (ret != MMF_OK) {
    printf("jpeg encode %s failed: %s\n", source_name(source), mmf_system_last_error());
  } else if (fill_info && info != NULL) {
    info->width = frame.width;
    info->height = frame.height;
    info->format = frame.pixel_format;
    info->sequence = frame.sequence;
    info->timestamp_us = frame.timestamp_us;
    info->photo_bytes = jpeg.bytes;
  } else if (info != NULL) {
    info->thumb_bytes = jpeg.bytes;
  }

  mmf_jpg_release_packet(NULL, &jpeg);
  mmf_camera_put_frame(camera, &frame);
  mmf_camera_close(camera);
  return ret == MMF_OK ? 0 : -1;
}

static int scan_album(AppState* app) {
  DIR* dir;
  struct dirent* ent;

  app->count = 0;
  app->current = 0;
  dir = opendir(app->dir);
  if (dir == NULL) {
    return -1;
  }
  while ((ent = readdir(dir)) != NULL && app->count < PHOTO_CAPACITY) {
    const char* name = ent->d_name;
    size_t len = strlen(name);
    PhotoItem* item;
    char* dot;

    if (len < 8 || strncmp(name, "photo_", 6) != 0 || strcmp(name + len - 4, ".jpg") != 0 ||
        strstr(name, "_thumb") != NULL) {
      continue;
    }

    item = &app->photos[app->count];
    memset(item, 0, sizeof(*item));
    snprintf(item->photo_path, sizeof(item->photo_path), "%s/%s", app->dir, name);
    snprintf(item->thumb_path, sizeof(item->thumb_path), "%s/%s", app->dir, name);
    dot = strrchr(item->thumb_path, '.');
    if (dot != NULL) {
      snprintf(dot, item->thumb_path + sizeof(item->thumb_path) - dot, "_thumb.jpg");
    }
    item->photo_bytes = file_size(item->photo_path);
    item->thumb_bytes = file_size(item->thumb_path);
    item->created_at = time(NULL);
    app->count += 1;
  }
  closedir(dir);
  return 0;
}

static void print_photo_detail(const AppState* app) {
  const PhotoItem* p;
  char timebuf[64];
  struct tm tmv;

  if (app->count == 0) {
    printf("album empty\n");
    return;
  }
  p = &app->photos[app->current];
  localtime_r(&p->created_at, &tmv);
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);
  printf("photo detail [%u/%u]\n", (unsigned)(app->current + 1), (unsigned)app->count);
  printf("  photo: %s\n", p->photo_path);
  printf("  thumb: %s\n", p->thumb_path);
  printf("  size : %ux%u fmt=%s seq=%llu pts=%llu\n", p->width, p->height, fmt_name(p->format),
         (unsigned long long)p->sequence, (unsigned long long)p->timestamp_us);
  printf("  file : photo=%u bytes thumb=%u bytes\n", (unsigned)p->photo_bytes,
         (unsigned)p->thumb_bytes);
  printf("  time : %s\n", timebuf);
}

static int display_live(AppState* app) {
  mmf_result_t ret = mmf_display_bind_camera(app->display, MMF_CAMERA_SRC_LIVE);
  if (ret != MMF_OK) {
    printf("display live failed: %s\n", mmf_system_last_error());
    return -1;
  }
  app->gallery_mode = 0;
  printf("live mode\n");
  return 0;
}

static int display_current_photo(AppState* app, int show_detail) {
  mmf_display_show_options_t show;
  PhotoItem* p;
  mmf_result_t ret;

  if (app->count == 0) {
    printf("album empty, press c to capture first\n");
    return -1;
  }

  p = &app->photos[app->current];
  mmf_display_unbind(app->display);
  mmf_display_get_default_show_options(&show);
  show.target = MMF_DISPLAY_TARGET_OSD;
  show.dst_rect.x = 0;
  show.dst_rect.y = 0;
  show.dst_rect.width = 720;
  show.dst_rect.height = 405;
  show.osd_handle = 130;
  show.osd_layer = 20;
  ret = mmf_display_show_image_file(app->display, p->photo_path, &show);
  if (ret != MMF_OK) {
    printf("show photo failed: %s\n", mmf_system_last_error());
    return -1;
  }
  app->gallery_mode = 1;
  printf("gallery [%u/%u]: %s\n", (unsigned)(app->current + 1), (unsigned)app->count,
         p->photo_path);
  if (show_detail) {
    print_photo_detail(app);
  }
  return 0;
}

static int display_current_thumb(AppState* app) {
  mmf_display_show_options_t show;
  PhotoItem* p;
  mmf_result_t ret;

  if (app->count == 0) {
    printf("album empty\n");
    return -1;
  }
  p = &app->photos[app->current];
  mmf_display_unbind(app->display);
  mmf_display_get_default_show_options(&show);
  show.target = MMF_DISPLAY_TARGET_OSD;
  show.dst_rect.x = 40;
  show.dst_rect.y = 40;
  show.dst_rect.width = 240;
  show.dst_rect.height = 240;
  show.osd_handle = 131;
  show.osd_layer = 20;
  ret = mmf_display_show_image_file(app->display, p->thumb_path, &show);
  if (ret != MMF_OK) {
    printf("show thumb failed: %s\n", mmf_system_last_error());
    return -1;
  }
  printf("thumbnail [%u/%u]: %s\n", (unsigned)(app->current + 1), (unsigned)app->count,
         p->thumb_path);
  print_photo_detail(app);
  return 0;
}

static int capture_photo(AppState* app) {
  PhotoItem item;
  char stamp[32];
  struct tm tmv;
  time_t now = time(NULL);

  localtime_r(&now, &tmv);
  strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
  memset(&item, 0, sizeof(item));
  snprintf(item.photo_path, sizeof(item.photo_path), "%s/photo_%s.jpg", app->dir, stamp);
  snprintf(item.thumb_path, sizeof(item.thumb_path), "%s/photo_%s_thumb.jpg", app->dir, stamp);
  item.created_at = now;

  if (encode_source_to_jpeg(MMF_CAMERA_SRC_MAIN, 92, item.photo_path, &item, 1) != 0) {
    return -1;
  }
  if (encode_source_to_jpeg(MMF_CAMERA_SRC_SUBRGB, 72, item.thumb_path, &item, 0) != 0) {
    unlink(item.photo_path);
    return -1;
  }
  item.photo_bytes = file_size(item.photo_path);
  item.thumb_bytes = file_size(item.thumb_path);

  if (app->count < PHOTO_CAPACITY) {
    app->photos[app->count] = item;
    app->current = app->count;
    app->count += 1;
  } else {
    app->photos[PHOTO_CAPACITY - 1] = item;
    app->current = PHOTO_CAPACITY - 1;
  }
  printf("captured: %s\n", item.photo_path);
  print_photo_detail(app);
  return 0;
}

static void print_help(void) {
  printf("mmf_camera_album_app\n");
  printf("  c : capture main photo + subrgb thumbnail\n");
  printf("  g : gallery show current photo\n");
  printf("  t : thumbnail/detail view\n");
  printf("  n : next photo\n");
  printf("  p : previous photo\n");
  printf("  d : print current photo detail\n");
  printf("  l : live preview\n");
  printf("  r : rescan album directory\n");
  printf("  q : quit\n");
}

int main(int argc, char** argv) {
  AppState app;
  mmf_system_config_t sys = {MMF_TRUE, MMF_TRUE, 3000};
  mmf_display_config_t display_cfg;
  int ch;

  memset(&app, 0, sizeof(app));
  snprintf(app.dir, sizeof(app.dir), "%s", argc >= 2 ? argv[1] : "/tmp/mmf_photos");

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  if (ensure_dir(app.dir) != 0) {
    printf("create album dir failed: %s\n", app.dir);
    return 1;
  }
  if (mmf_system_init(&sys) != MMF_OK) {
    printf("system init failed: %s\n", mmf_system_last_error());
    return 2;
  }
  mmf_display_get_default_config(&display_cfg);
  if (mmf_display_open(&display_cfg, &app.display) != MMF_OK) {
    printf("display open failed: %s\n", mmf_system_last_error());
    mmf_system_deinit();
    return 3;
  }
  scan_album(&app);
  print_help();
  display_live(&app);

  while (!g_stop) {
    printf("album[%u] > ", (unsigned)app.count);
    fflush(stdout);
    ch = getchar();
    if (ch == EOF)
      break;
    if (ch == '\n' || ch == '\r')
      continue;
    while (getchar() != '\n' && !feof(stdin)) {
    }
    ch = tolower(ch);

    if (ch == 'q') {
      break;
    } else if (ch == 'c') {
      if (capture_photo(&app) == 0) {
        display_current_photo(&app, 0);
      }
    } else if (ch == 'g') {
      display_current_photo(&app, 1);
    } else if (ch == 't') {
      display_current_thumb(&app);
    } else if (ch == 'n') {
      if (app.count > 0) {
        app.current = (app.current + 1) % app.count;
        display_current_photo(&app, 1);
      }
    } else if (ch == 'p') {
      if (app.count > 0) {
        app.current = (app.current + app.count - 1) % app.count;
        display_current_photo(&app, 1);
      }
    } else if (ch == 'd') {
      print_photo_detail(&app);
    } else if (ch == 'l') {
      display_live(&app);
    } else if (ch == 'r') {
      scan_album(&app);
      printf("rescanned: %u photos\n", (unsigned)app.count);
    } else {
      print_help();
    }
  }

  mmf_display_clear_overlay(app.display, 130);
  mmf_display_clear_overlay(app.display, 131);
  mmf_display_unbind(app.display);
  mmf_display_close(app.display);
  mmf_system_deinit();
  printf("album app stopped\n");
  return 0;
}
