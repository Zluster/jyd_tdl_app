#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "mmf/mmf.h"

static const char* result_name(mmf_result_t ret) {
  return ret == MMF_OK ? "ok" : "fail";
}

static void cleanup_system(void) {
  mmf_system_deinit();
}

static mmf_camera_source_t parse_source(const char* name) {
  if (strcmp(name, "main") == 0)
    return MMF_CAMERA_SRC_MAIN;
  if (strcmp(name, "ai") == 0)
    return MMF_CAMERA_SRC_AI;
  if (strcmp(name, "live") == 0)
    return MMF_CAMERA_SRC_LIVE;
  if (strcmp(name, "subrgb") == 0)
    return MMF_CAMERA_SRC_SUBRGB;
  if (strcmp(name, "rgb") == 0)
    return MMF_CAMERA_SRC_RGB;
  if (strcmp(name, "screen") == 0)
    return MMF_CAMERA_SRC_SCREEN;
  return MMF_CAMERA_SRC_LIVE;
}

static int system_once(void) {
  mmf_system_status_t status;
  mmf_version_t version = mmf_system_version();
  mmf_scale_mode_t scale = MMF_SCALE_STRETCH;
  int failed = 0;

  failed += mmf_system_wait_ready(1000) == MMF_OK ? 0 : 1;
  failed += mmf_system_get_status(&status) == MMF_OK ? 0 : 1;
  failed += mmf_system_get_vpss_scale_mode(MMF_CAMERA_SRC_AI, &scale) == MMF_OK ? 0 : 1;
  failed += mmf_system_set_vpss_scale_mode(MMF_CAMERA_SRC_AI, scale) == MMF_OK ? 0 : 1;
  printf("system: failed=%d version=%u.%u.%u git=%s local=%d remote=%d msg=%d\n", failed,
         version.major, version.minor, version.patch,
         version.git_version ? version.git_version : "", status.local_ready, status.remote_ready,
         status.msg_connected);
  if (failed)
    printf("error: %s\n", mmf_system_last_error());
  return failed == 0 ? 0 : 1;
}

static int list_outputs(void) {
  mmf_camera_output_desc_t outputs[10];
  size_t count = 0;
  mmf_result_t ret = mmf_camera_list_outputs(outputs, 10, &count);
  if (ret != MMF_OK) {
    printf("list outputs failed: %d\n", ret);
    return 1;
  }
  for (size_t i = 0; i < count; ++i) {
    printf("%s dev=%d grp=%d chn=%d %ux%u fmt=%d scale=%d depth=%u available=%d\n",
           outputs[i].name, outputs[i].device, outputs[i].vpss_group,
           outputs[i].vpss_channel, outputs[i].width, outputs[i].height,
           outputs[i].pixel_format, outputs[i].scale_mode, outputs[i].depth,
           outputs[i].available);
  }
  return 0;
}

static int snapshot_source(const char* source_name, mmf_camera_device_t device, const char* path) {
  if (strcmp(source_name, "screen") == 0) {
    mmf_display_config_t display_cfg;
    mmf_display_t* display = NULL;
    mmf_display_snapshot_config_t snap;
    mmf_display_get_default_config(&display_cfg);
    mmf_result_t ret = mmf_display_open(&display_cfg, &display);
    memset(&snap, 0, sizeof(snap));
    snap.path = path;
    snap.jpeg_quality = 80;
    snap.timeout_ms = 1000;
    snap.include_osd = MMF_FALSE;
    if (ret == MMF_OK) {
      ret = mmf_display_snapshot(display, &snap);
    }
    printf("snapshot screen -> %s: %s (%d)\n", path, result_name(ret), ret);
    if (ret != MMF_OK) {
      printf("error: %s\n", mmf_system_last_error());
    }
    if (display)
      mmf_display_close(display);
    return ret == MMF_OK ? 0 : 1;
  }

  mmf_camera_config_t cfg;
  mmf_camera_t* camera = NULL;
  mmf_camera_source_t source = parse_source(source_name);
  mmf_result_t ret = mmf_camera_get_default_config(source, &cfg);
  if (ret != MMF_OK) {
    printf("default config failed: %d\n", ret);
    return 1;
  }
  cfg.device = device;
  ret = mmf_camera_open(&cfg, &camera);
  if (ret != MMF_OK) {
    printf("camera open failed: %d %s\n", ret, mmf_system_last_error());
    return 1;
  }
  ret = mmf_camera_snapshot(camera, path, MMF_CODEC_JPEG);
  printf("snapshot %s dev=%d -> %s: %s (%d)\n", source_name, device, path,
         result_name(ret), ret);
  if (ret != MMF_OK) {
    printf("error: %s\n", mmf_system_last_error());
  }
  mmf_camera_close(camera);
  return ret == MMF_OK ? 0 : 1;
}

static int audio_read_once(int channels) {
  mmf_audio_input_config_t cfg;
  mmf_audio_input_t* input = NULL;
  mmf_audio_frame_t frame;
  mmf_audio_get_default_input_config(&cfg);
  cfg.io.channels = (uint32_t)channels;
  cfg.enable_3a = MMF_TRUE;
  mmf_result_t ret = mmf_audio_input_open(&cfg, &input);
  if (ret != MMF_OK) {
    printf("audio input open failed: %d %s\n", ret, mmf_system_last_error());
    return 1;
  }
  memset(&frame, 0, sizeof(frame));
  ret = mmf_audio_input_read(input, &frame, 1000);
  printf("audio read: %s (%d) %uHz %uch bytes=%u seq=%llu\n", result_name(ret), ret,
         frame.sample_rate, frame.channels, (unsigned)frame.bytes,
         (unsigned long long)frame.sequence);
  mmf_audio_input_release(input, &frame);
  mmf_audio_input_close(input);
  return ret == MMF_OK ? 0 : 1;
}

static int audio_3a_global_once(void) {
  mmf_audio_3a_config_t cfg;
  mmf_audio_3a_status_t status;
  mmf_bool_t enabled = MMF_FALSE;
  uint32_t level = 0;
  uint32_t gain = 0;
  uint32_t compress = 0;
  int32_t delay = 0;
  int32_t target = 0;
  int failed = 0;

  mmf_audio_get_default_3a(&cfg);
  failed += mmf_audio_3a_set_aec(cfg.aec_enable) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_get_aec(&enabled) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_set_ns(cfg.ns_enable) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_get_ns(&enabled) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_set_agc(cfg.agc_enable) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_get_agc(&enabled) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_set_aec_level(cfg.aec_level) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_get_aec_level(&level) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_set_aec_delay(cfg.aec_delay_ms) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_get_aec_delay(&delay) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_set_ns_level(cfg.ns_level) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_get_ns_level(&level) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_set_agc_target(cfg.agc_target_db) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_get_agc_target(&target) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_set_agc_max_gain(cfg.agc_max_gain) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_get_agc_max_gain(&gain) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_set_agc_compress(cfg.agc_compress) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_get_agc_compress(&compress) == MMF_OK ? 0 : 1;
  failed += mmf_audio_3a_get_status(&status) == MMF_OK ? 0 : 1;
  printf("audio 3a global: failed=%d supported=%d aec=%d ns=%d agc=%d\n", failed, status.supported,
         status.config.aec_enable, status.config.ns_enable, status.config.agc_enable);
  return failed == 0 ? 0 : 1;
}

static int audio_control_once(void) {
  mmf_audio_input_config_t in_cfg;
  mmf_audio_output_config_t out_cfg;
  mmf_audio_input_t* input = NULL;
  mmf_audio_output_t* output = NULL;
  mmf_audio_stream_status_t status;
  mmf_audio_3a_config_t vqe;
  int volume = 0;
  int failed = 0;

  mmf_audio_get_default_input_config(&in_cfg);
  mmf_audio_get_default_output_config(&out_cfg);
  in_cfg.enable_3a = MMF_FALSE;
  mmf_result_t ret = mmf_audio_input_open(&in_cfg, &input);
  printf("audio input open: %s (%d)\n", result_name(ret), ret);
  if (ret != MMF_OK)
    return 1;
  ret = mmf_audio_output_open(&out_cfg, &output);
  printf("audio output open: %s (%d)\n", result_name(ret), ret);
  if (ret != MMF_OK) {
    mmf_audio_input_close(input);
    return 1;
  }

  failed += mmf_audio_input_get_status(input, &status) == MMF_OK ? 0 : 1;
  failed += mmf_audio_output_get_status(output, &status) == MMF_OK ? 0 : 1;
  failed += mmf_audio_input_get_volume(input, &volume) == MMF_OK ? 0 : 1;
  failed += mmf_audio_input_set_volume(input, volume) == MMF_OK ? 0 : 1;
  failed += mmf_audio_output_get_volume(output, &volume) == MMF_OK ? 0 : 1;
  failed += mmf_audio_output_set_volume(output, volume) == MMF_OK ? 0 : 1;
  mmf_audio_3a_get_default_config(&vqe);
  vqe.aec_enable = MMF_FALSE;
  vqe.ns_enable = MMF_FALSE;
  vqe.agc_enable = MMF_FALSE;
  failed += mmf_audio_input_set_3a(input, &vqe) == MMF_OK ? 0 : 1;
  failed += mmf_audio_input_get_3a(input, &vqe) == MMF_OK ? 0 : 1;
  failed += mmf_audio_output_drain(output) == MMF_OK ? 0 : 1;

  mmf_audio_input_close(input);
  mmf_audio_output_close(output);
  printf("audio control: failed=%d\n", failed);
  return failed == 0 ? 0 : 1;
}

static int audio_codec_once(void) {
  mmf_audio_input_config_t in_cfg;
  mmf_audio_input_t* input = NULL;
  mmf_audio_frame_t frame;
  mmf_audio_codec_config_t codec_cfg;
  mmf_audio_encoder_t* encoder = NULL;
  mmf_packet_t packet;

  mmf_audio_get_default_input_config(&in_cfg);
  in_cfg.enable_3a = MMF_FALSE;
  mmf_result_t ret = mmf_audio_input_open(&in_cfg, &input);
  if (ret != MMF_OK) {
    printf("audio codec input open failed: %d %s\n", ret, mmf_system_last_error());
    return 1;
  }
  memset(&frame, 0, sizeof(frame));
  ret = mmf_audio_input_read(input, &frame, 1000);
  if (ret != MMF_OK) {
    printf("audio codec read failed: %d %s\n", ret, mmf_system_last_error());
    mmf_audio_input_close(input);
    return 1;
  }

  memset(&codec_cfg, 0, sizeof(codec_cfg));
  codec_cfg.codec = MMF_CODEC_G711A;
  codec_cfg.sample_rate = frame.sample_rate;
  codec_cfg.channels = frame.channels;
  ret = mmf_audio_encoder_open(&codec_cfg, &encoder);
  if (ret == MMF_OK) {
    memset(&packet, 0, sizeof(packet));
    ret = mmf_audio_encoder_encode(encoder, &frame, &packet);
  }
  if (ret != MMF_OK) {
    printf("audio encode failed: %d %s\n", ret, mmf_system_last_error());
    if (encoder)
      mmf_audio_encoder_close(encoder);
    mmf_audio_input_release(input, &frame);
    mmf_audio_input_close(input);
    return 1;
  }

  printf("audio codec encode: %s (%d) packet=%u\n", result_name(ret), ret, (unsigned)packet.bytes);
  mmf_audio_encoder_release(encoder, &packet);
  mmf_audio_encoder_close(encoder);
  mmf_audio_input_release(input, &frame);
  mmf_audio_input_close(input);
  return ret == MMF_OK ? 0 : 1;
}

static int audio_loopback_once(int channels, int frames, int enable_3a, int volume) {
  mmf_audio_input_config_t in_cfg;
  mmf_audio_output_config_t out_cfg;
  mmf_audio_input_t* input = NULL;
  mmf_audio_output_t* output = NULL;
  int failed = 0;

  if (frames <= 0)
    frames = 20;
  mmf_audio_get_default_input_config(&in_cfg);
  mmf_audio_get_default_output_config(&out_cfg);
  in_cfg.io.channels = (uint32_t)channels;
  out_cfg.io.channels = (uint32_t)channels;
  if (volume >= 0) {
    in_cfg.io.input_volume = volume;
    out_cfg.io.output_volume = volume;
  }
  in_cfg.enable_3a = enable_3a ? MMF_TRUE : MMF_FALSE;
  in_cfg.config_3a.aec_enable = enable_3a ? MMF_TRUE : MMF_FALSE;
  in_cfg.config_3a.ns_enable = enable_3a ? MMF_TRUE : MMF_FALSE;
  in_cfg.config_3a.agc_enable = enable_3a ? MMF_TRUE : MMF_FALSE;
  out_cfg.provide_aec_reference = enable_3a ? MMF_TRUE : MMF_FALSE;
  mmf_result_t ret = mmf_audio_input_open(&in_cfg, &input);
  if (ret == MMF_OK)
    ret = mmf_audio_output_open(&out_cfg, &output);
  if (ret != MMF_OK) {
    printf("audio loopback open failed: %d %s\n", ret, mmf_system_last_error());
    if (input)
      mmf_audio_input_close(input);
    return 1;
  }
  for (int i = 0; i < frames; ++i) {
    mmf_audio_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    ret = mmf_audio_input_read(input, &frame, 1000);
    if (ret == MMF_OK)
      ret = mmf_audio_output_write(output, &frame, 1000);
    mmf_audio_input_release(input, &frame);
    if (ret != MMF_OK) {
      printf("audio loopback frame %d failed: %d %s\n", i, ret, mmf_system_last_error());
      failed = 1;
      break;
    }
  }
  mmf_audio_output_drain(output);
  mmf_audio_output_close(output);
  mmf_audio_input_close(input);
  printf("audio loopback: channels=%d frames=%d 3a=%d volume=%d failed=%d\n", channels, frames,
         enable_3a ? 1 : 0, volume >= 0 ? volume : in_cfg.io.input_volume, failed);
  return failed == 0 ? 0 : 1;
}

static int audio_3a_effect_once(int channels, int frames, int volume) {
  int failed = 0;
  if (frames <= 0)
    frames = 100;
  if (volume < 0)
    volume = 16;
  printf("audio 3a effect: phase 1 raw loopback, channels=%d frames=%d volume=%d\n", channels,
         frames, volume);
  failed += audio_loopback_once(channels, frames, 0, volume) == 0 ? 0 : 1;
  printf("audio 3a effect: phase 2 3A loopback, channels=%d frames=%d volume=%d\n", channels,
         frames, volume);
  failed += audio_loopback_once(channels, frames, 1, volume) == 0 ? 0 : 1;
  printf("audio 3a effect: failed=%d, expected: phase 2 should have less echo/noise/howling\n",
         failed);
  return failed == 0 ? 0 : 1;
}

static int get_camera_frame(const char* source_name, mmf_camera_t** camera,
                            mmf_video_frame_t* frame) {
  mmf_camera_config_t cfg;
  mmf_camera_source_t source = parse_source(source_name);
  mmf_result_t ret = mmf_camera_get_default_config(source, &cfg);
  if (ret != MMF_OK) {
    printf("default config failed: %d\n", ret);
    return 1;
  }
  ret = mmf_camera_open(&cfg, camera);
  if (ret != MMF_OK) {
    printf("camera open failed: %d %s\n", ret, mmf_system_last_error());
    return 1;
  }
  mmf_camera_status_t status;
  memset(&status, 0, sizeof(status));
  ret = mmf_camera_get_status(*camera, &status);
  if (ret != MMF_OK) {
    printf("camera status failed: %d %s\n", ret, mmf_system_last_error());
    mmf_camera_close(*camera);
    *camera = NULL;
    return 1;
  }
  memset(frame, 0, sizeof(*frame));
  ret = mmf_camera_get_frame(*camera, frame, 1000);
  if (ret != MMF_OK) {
    printf("camera get frame failed: %d %s\n", ret, mmf_system_last_error());
    mmf_camera_close(*camera);
    *camera = NULL;
    return 1;
  }
  printf("frame %s %ux%u fmt=%d seq=%llu\n", source_name, frame->width, frame->height,
         frame->pixel_format, (unsigned long long)frame->sequence);
  return 0;
}

static int put_camera_frame(const char* source_name, mmf_camera_t* camera,
                            mmf_video_frame_t* frame) {
  if (strcmp(source_name, "ai") == 0) {
    return mmf_camera_release_frame(camera, frame) == MMF_OK ? 0 : 1;
  }
  return mmf_camera_put_frame(camera, frame) == MMF_OK ? 0 : 1;
}

static int jpg_roundtrip(const char* source_name) {
  mmf_camera_t* camera = NULL;
  mmf_video_frame_t frame;
  if (get_camera_frame(source_name, &camera, &frame) != 0) {
    return 1;
  }

  mmf_jpg_encoder_config_t enc_cfg;
  mmf_jpg_encoder_t* encoder = NULL;
  mmf_packet_t jpeg;
  mmf_jpg_get_default_encoder_config(&enc_cfg);
  enc_cfg.width = frame.width;
  enc_cfg.height = frame.height;
  enc_cfg.input_format = frame.pixel_format;
  enc_cfg.quality = 80;
  mmf_result_t ret = mmf_jpg_encoder_open(&enc_cfg, &encoder);
  if (ret == MMF_OK) {
    memset(&jpeg, 0, sizeof(jpeg));
    ret = mmf_jpg_encode_frame(encoder, &frame, &jpeg);
  }
  if (ret != MMF_OK) {
    printf("jpg encode failed: %d %s\n", ret, mmf_system_last_error());
    if (encoder)
      mmf_jpg_encoder_close(encoder);
    put_camera_frame(source_name, camera, &frame);
    mmf_camera_close(camera);
    return 1;
  }
  printf("jpg encode ok bytes=%u seq=%llu\n", (unsigned)jpeg.bytes,
         (unsigned long long)jpeg.sequence);

  mmf_jpg_decoder_config_t dec_cfg;
  mmf_jpg_decoder_t* decoder = NULL;
  mmf_video_frame_t decoded;
  mmf_jpg_get_default_decoder_config(&dec_cfg);
  dec_cfg.max_width = frame.width;
  dec_cfg.max_height = frame.height;
  dec_cfg.output_format = MMF_PIXFMT_NV21;
  ret = mmf_jpg_decoder_open(&dec_cfg, &decoder);
  if (ret == MMF_OK) {
    memset(&decoded, 0, sizeof(decoded));
    ret = mmf_jpg_decode_packet(decoder, &jpeg, &decoded);
  }
  if (ret != MMF_OK) {
    printf("jpg decode failed: %d %s\n", ret, mmf_system_last_error());
    if (decoder)
      mmf_jpg_decoder_close(decoder);
    mmf_jpg_release_packet(encoder, &jpeg);
    mmf_jpg_encoder_close(encoder);
    put_camera_frame(source_name, camera, &frame);
    mmf_camera_close(camera);
    return 1;
  }
  printf("jpg decode ok %ux%u fmt=%d seq=%llu\n", decoded.width, decoded.height,
         decoded.pixel_format, (unsigned long long)decoded.sequence);

  mmf_jpg_release_frame(decoder, &decoded);
  mmf_jpg_decoder_close(decoder);
  mmf_jpg_release_packet(encoder, &jpeg);
  mmf_jpg_encoder_close(encoder);

  mmf_packet_t one_shot_jpeg;
  memset(&one_shot_jpeg, 0, sizeof(one_shot_jpeg));
  ret = mmf_jpg_encode(&frame, 75, &one_shot_jpeg);
  if (ret != MMF_OK || one_shot_jpeg.bytes == 0) {
    printf("jpg one-shot encode failed: %d %s\n", ret, mmf_system_last_error());
    put_camera_frame(source_name, camera, &frame);
    mmf_camera_close(camera);
    return 1;
  }
  printf("jpg one-shot encode ok bytes=%u\n", (unsigned)one_shot_jpeg.bytes);

  mmf_video_frame_t one_shot_decoded;
  memset(&one_shot_decoded, 0, sizeof(one_shot_decoded));
  ret = mmf_jpg_decode(one_shot_jpeg.data, one_shot_jpeg.bytes, MMF_PIXFMT_NV21, &one_shot_decoded);
  if (ret != MMF_OK) {
    printf("jpg one-shot decode failed: %d %s\n", ret, mmf_system_last_error());
    put_camera_frame(source_name, camera, &frame);
    mmf_camera_close(camera);
    return 1;
  }
  printf("jpg one-shot decode ok %ux%u fmt=%d\n", one_shot_decoded.width, one_shot_decoded.height,
         one_shot_decoded.pixel_format);

  put_camera_frame(source_name, camera, &frame);
  mmf_camera_close(camera);
  return 0;
}

static int display_frame(const char* source_name) {
  if (strcmp(source_name, "screen") == 0) {
    mmf_display_config_t display_cfg;
    mmf_display_t* display = NULL;
    mmf_display_snapshot_config_t snap;
    mmf_display_get_default_config(&display_cfg);
    mmf_result_t ret = mmf_display_open(&display_cfg, &display);
    memset(&snap, 0, sizeof(snap));
    snap.path = "/tmp/mmf_display_frame_screen.jpg";
    snap.jpeg_quality = 80;
    snap.timeout_ms = 1000;
    snap.include_osd = MMF_FALSE;
    if (ret == MMF_OK) {
      ret = mmf_display_snapshot(display, &snap);
    }
    printf("display frame screen snapshot: %s (%d) path=%s\n", result_name(ret), ret, snap.path);
    if (ret != MMF_OK) {
      printf("error: %s\n", mmf_system_last_error());
    }
    if (display)
      mmf_display_close(display);
    return ret == MMF_OK ? 0 : 1;
  }

  mmf_camera_t* camera = NULL;
  mmf_video_frame_t frame;
  if (get_camera_frame(source_name, &camera, &frame) != 0) {
    return 1;
  }

  mmf_display_config_t display_cfg;
  mmf_display_t* display = NULL;
  mmf_display_get_default_config(&display_cfg);
  mmf_result_t ret = mmf_display_open(&display_cfg, &display);
  if (ret == MMF_OK) {
    ret = mmf_display_show_frame(display, &frame, NULL);
  }
  printf("display frame %s: %s (%d)\n", source_name, result_name(ret), ret);
  if (ret != MMF_OK) {
    printf("error: %s\n", mmf_system_last_error());
  }
  if (display)
    mmf_display_close(display);
  put_camera_frame(source_name, camera, &frame);
  mmf_camera_close(camera);
  return ret == MMF_OK ? 0 : 1;
}

static int write_test_bmp(const char* path) {
  static const unsigned char bmp[] = {
      0x42, 0x4d, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00,
      0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
      0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x13, 0x0b, 0x00, 0x00,
      0x13, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
      0x00, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00,
  };
  FILE* fp = fopen(path, "wb");
  if (!fp)
    return -1;
  size_t written = fwrite(bmp, 1, sizeof(bmp), fp);
  fclose(fp);
  return written == sizeof(bmp) ? 0 : -1;
}

static int camera_control_once(void) {
  mmf_camera_ae_mode_t ae = MMF_CAMERA_AE_AUTO;
  mmf_camera_awb_mode_t awb = MMF_CAMERA_AWB_AUTO;
  uint32_t value = 0;
  mmf_camera_wb_t wb;
  int failed = 0;

  printf("isp control: begin AE/AWB/exposure/gain/WB test\n");

  failed += mmf_camera_get_ae_mode(&ae) == MMF_OK ? 0 : 1;
  failed += mmf_camera_set_ae_mode(MMF_CAMERA_AE_MANUAL) == MMF_OK ? 0 : 1;
  failed += mmf_camera_get_ae_mode(&ae) == MMF_OK ? 0 : 1;
  printf("isp control: ae after manual=%d\n", ae);
  failed += mmf_camera_set_exposure(10000) == MMF_OK ? 0 : 1;
  failed += mmf_camera_get_exposure(&value) == MMF_OK ? 0 : 1;
  printf("isp control: exposure after set=%u\n", value);
  failed += mmf_camera_set_gain(1024) == MMF_OK ? 0 : 1;
  failed += mmf_camera_get_gain(&value) == MMF_OK ? 0 : 1;
  printf("isp control: analog gain after set=%u\n", value);
  failed += mmf_camera_set_isp_gain(1024) == MMF_OK ? 0 : 1;
  failed += mmf_camera_get_isp_gain(&value) == MMF_OK ? 0 : 1;
  printf("isp control: isp gain after set=%u\n", value);
  failed += mmf_camera_set_ae_mode(MMF_CAMERA_AE_AUTO) == MMF_OK ? 0 : 1;

  failed += mmf_camera_get_awb_mode(&awb) == MMF_OK ? 0 : 1;
  failed += mmf_camera_set_awb_mode(MMF_CAMERA_AWB_MANUAL) == MMF_OK ? 0 : 1;
  failed += mmf_camera_get_awb_mode(&awb) == MMF_OK ? 0 : 1;
  printf("isp control: awb after manual=%d\n", awb);
  wb.red_gain = 1024;
  wb.green_gain = 1024;
  wb.blue_gain = 1024;
  failed += mmf_camera_set_wb(&wb) == MMF_OK ? 0 : 1;
  memset(&wb, 0, sizeof(wb));
  failed += mmf_camera_get_wb(&wb) == MMF_OK ? 0 : 1;
  printf("isp control: wb after set r=%u g=%u b=%u\n", wb.red_gain, wb.green_gain, wb.blue_gain);
  failed += mmf_camera_set_awb_mode(MMF_CAMERA_AWB_AUTO) == MMF_OK ? 0 : 1;

  printf("isp control: failed=%d ae=%d awb=%d\n", failed, ae, awb);
  if (failed)
    printf("error: %s\n", mmf_system_last_error());
  return failed == 0 ? 0 : 1;
}

static int display_control_once(void) {
  mmf_display_config_t cfg;
  mmf_display_show_options_t show;
  mmf_display_t* display = NULL;
  mmf_display_status_t status;
  mmf_display_snapshot_config_t snap;
  mmf_rect_t window;
  const char* test_bmp = "/tmp/mmf_test_2x2.bmp";
  int failed = 0;

  mmf_display_get_default_config(&cfg);
  mmf_display_get_default_show_options(&show);
  mmf_result_t ret = mmf_display_open(&cfg, &display);
  if (ret != MMF_OK) {
    printf("display open failed: %d %s\n", ret, mmf_system_last_error());
    return 1;
  }
  window.x = 0;
  window.y = 0;
  window.width = cfg.panel_width;
  window.height = cfg.panel_height;
  failed += mmf_display_set_window(display, &window, MMF_SCALE_FIT_BLACK, 0) == MMF_OK ? 0 : 1;
  failed += mmf_display_get_status(display, &status) == MMF_OK ? 0 : 1;
  failed += mmf_display_bind_camera(display, MMF_CAMERA_SRC_LIVE) == MMF_OK ? 0 : 1;
  failed += mmf_display_get_status(display, &status) == MMF_OK ? 0 : 1;
  failed += mmf_display_unbind(display) == MMF_OK ? 0 : 1;

  memset(&snap, 0, sizeof(snap));
  snap.path = "/tmp/mmf_display_snapshot.jpg";
  snap.jpeg_quality = 80;
  snap.timeout_ms = 1000;
  snap.include_osd = MMF_FALSE;
  failed += mmf_display_snapshot(display, &snap) == MMF_OK ? 0 : 1;
  if (write_test_bmp(test_bmp) == 0) {
    show.target = MMF_DISPLAY_TARGET_OSD;
    show.dst_rect.x = 0;
    show.dst_rect.y = 0;
    show.dst_rect.width = 64;
    show.dst_rect.height = 64;
    failed += mmf_display_show_image_file(display, test_bmp, &show) == MMF_OK ? 0 : 1;
    snap.path = "/tmp/mmf_display_snapshot_osd.jpg";
    snap.include_osd = MMF_TRUE;
    failed += mmf_display_snapshot(display, &snap) == MMF_OK ? 0 : 1;
  } else {
    failed += 1;
  }
  failed += mmf_display_clear(display) == MMF_OK ? 0 : 1;
  failed += mmf_display_clear_overlay(display, 0) == MMF_OK ? 0 : 1;
  mmf_display_close(display);

  printf("display control: failed=%d snapshot=%s\n", failed, snap.path);
  if (failed)
    printf("error: %s\n", mmf_system_last_error());
  return failed == 0 ? 0 : 1;
}

static int http_get_once(uint16_t port, const char* path, size_t* bytes_out) {
  int fd = -1;
  char req[256];
  char buf[1024];
  size_t total = 0;

  for (int attempt = 0; attempt < 20; ++attempt) {
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }
    struct timeval tv;
    tv.tv_sec = 2;
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

  snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
           path);
  if (send(fd, req, strlen(req), 0) < 0) {
    close(fd);
    return -1;
  }
  while (1) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n > 0) {
      total += (size_t)n;
      if (strstr(path, "stream") != NULL && total > 4096) {
        break;
      }
      continue;
    }
    if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    close(fd);
    return -1;
  }
  close(fd);
  if (bytes_out)
    *bytes_out = total;
  return total > 0 ? 0 : -1;
}

static int http_probe(const char* kind, uint16_t port) {
  mmf_jpg_http_config_t cfg;
  mmf_jpg_http_server_t* server = NULL;
  mmf_jpg_http_get_default_config(&cfg);
  cfg.port = port;
  cfg.mode = MMF_JPG_HTTP_MODE_CAMERA_PULL;
  cfg.camera_source = MMF_CAMERA_SRC_MAIN;
  cfg.fps = 5;
  mmf_result_t ret = mmf_jpg_http_open(&cfg, &server);
  if (ret == MMF_OK) {
    ret = mmf_jpg_http_start_stream(server);
  }
  if (ret != MMF_OK) {
    printf("http start failed: %d %s\n", ret, mmf_system_last_error());
    if (server)
      mmf_jpg_http_close(server);
    return 1;
  }

  size_t bytes = 0;
  const char* path = strcmp(kind, "stream") == 0 ? cfg.stream_path : cfg.snapshot_path;
  int ok = http_get_once(port, path, &bytes);
  mmf_jpg_http_status_t status;
  memset(&status, 0, sizeof(status));
  mmf_jpg_http_get_status(server, &status);
  printf("http %s: %s bytes=%u frames=%llu clients=%u\n", kind, ok == 0 ? "ok" : "fail",
         (unsigned)bytes, (unsigned long long)status.frames_published, status.client_count);
  mmf_jpg_http_stop_stream(server);
  mmf_jpg_http_close(server);
  return ok == 0 ? 0 : 1;
}

static int http_push_probe(uint16_t port) {
  static const unsigned char tiny_jpeg[] = {
      0xff,
      0xd8,
      0xff,
      0xd9,
  };
  mmf_jpg_http_config_t cfg;
  mmf_jpg_http_server_t* server = NULL;
  mmf_jpg_http_get_default_config(&cfg);
  cfg.port = port;
  cfg.mode = MMF_JPG_HTTP_MODE_PUSH;
  mmf_result_t ret = mmf_jpg_http_open(&cfg, &server);
  if (ret == MMF_OK) {
    ret = mmf_jpg_http_start_stream(server);
  }
  if (ret == MMF_OK) {
    ret = mmf_jpg_http_publish_jpeg(server, tiny_jpeg, sizeof(tiny_jpeg), 1, 0);
  }
  if (ret != MMF_OK) {
    printf("http push start failed: %d %s\n", ret, mmf_system_last_error());
    if (server)
      mmf_jpg_http_close(server);
    return 1;
  }

  size_t bytes = 0;
  int ok = http_get_once(port, cfg.snapshot_path, &bytes);
  mmf_jpg_http_status_t status;
  memset(&status, 0, sizeof(status));
  mmf_jpg_http_get_status(server, &status);
  printf("http push: %s bytes=%u frames=%llu clients=%u\n", ok == 0 ? "ok" : "fail",
         (unsigned)bytes, (unsigned long long)status.frames_published, status.client_count);
  mmf_jpg_http_stop_stream(server);
  mmf_jpg_http_close(server);
  return ok == 0 ? 0 : 1;
}

static int http_publish_frame_probe(uint16_t port) {
  mmf_camera_t* camera = NULL;
  mmf_video_frame_t frame;
  mmf_jpg_http_config_t cfg;
  mmf_jpg_http_server_t* server = NULL;

  if (get_camera_frame("main", &camera, &frame) != 0) {
    return 1;
  }

  mmf_jpg_http_get_default_config(&cfg);
  cfg.port = port;
  cfg.mode = MMF_JPG_HTTP_MODE_PUSH;
  mmf_result_t ret = mmf_jpg_http_open(&cfg, &server);
  if (ret == MMF_OK)
    ret = mmf_jpg_http_start_stream(server);
  if (ret == MMF_OK)
    ret = mmf_jpg_http_publish_frame(server, &frame, 80);
  if (ret != MMF_OK) {
    printf("http publish-frame failed: %d %s\n", ret, mmf_system_last_error());
    if (server)
      mmf_jpg_http_close(server);
    put_camera_frame("main", camera, &frame);
    mmf_camera_close(camera);
    return 1;
  }

  size_t bytes = 0;
  int ok = http_get_once(port, cfg.snapshot_path, &bytes);
  mmf_jpg_http_status_t status;
  memset(&status, 0, sizeof(status));
  mmf_jpg_http_get_status(server, &status);
  printf("http publish-frame: %s bytes=%u frames=%llu\n", ok == 0 ? "ok" : "fail", (unsigned)bytes,
         (unsigned long long)status.frames_published);
  mmf_jpg_http_stop_stream(server);
  mmf_jpg_http_close(server);
  put_camera_frame("main", camera, &frame);
  mmf_camera_close(camera);
  return ok == 0 ? 0 : 1;
}

static int touch_status_once(void) {
  mmf_touch_config_t cfg;
  mmf_touch_t* touch = NULL;
  mmf_touch_status_t status;
  mmf_touch_get_default_config(&cfg);
  mmf_result_t ret = mmf_touch_open(&cfg, &touch);
  if (ret != MMF_OK) {
    printf("touch open failed: %d %s\n", ret, mmf_system_last_error());
    return 1;
  }
  memset(&status, 0, sizeof(status));
  ret = mmf_touch_get_status(touch, &status);
  printf("touch status: %s (%d) opened=%d %ux%u events=%llu\n", result_name(ret), ret,
         status.opened, status.screen_width, status.screen_height,
         (unsigned long long)status.event_count);
  mmf_touch_close(touch);
  return ret == MMF_OK ? 0 : 1;
}

static int reentry_test(int loops) {
  int failed = 0;
  if (loops <= 0)
    loops = 10;
  for (int i = 0; i < loops; ++i) {
    if (jpg_roundtrip("main") != 0) {
      failed++;
      break;
    }
    if (audio_read_once(1) != 0) {
      failed++;
      break;
    }
    if (audio_codec_once() != 0) {
      failed++;
      break;
    }
  }
  printf("reentry loops=%d failed=%d\n", loops, failed);
  return failed == 0 ? 0 : 1;
}

static int all_test(void) {
  int failed = 0;
  failed += system_once() == 0 ? 0 : 1;
  failed += list_outputs() == 0 ? 0 : 1;
  failed += audio_read_once(1) == 0 ? 0 : 1;
  failed += audio_read_once(2) == 0 ? 0 : 1;
  failed += audio_3a_global_once() == 0 ? 0 : 1;
  failed += audio_control_once() == 0 ? 0 : 1;
  failed += audio_codec_once() == 0 ? 0 : 1;
  failed += jpg_roundtrip("main") == 0 ? 0 : 1;
  failed += camera_control_once() == 0 ? 0 : 1;
  failed += display_frame("screen") == 0 ? 0 : 1;
  failed += display_control_once() == 0 ? 0 : 1;
  failed += http_probe("snapshot", 18080) == 0 ? 0 : 1;
  failed += http_probe("stream", 18081) == 0 ? 0 : 1;
  failed += http_push_probe(18082) == 0 ? 0 : 1;
  failed += http_publish_frame_probe(18083) == 0 ? 0 : 1;
  printf("all-test failed=%d\n", failed);
  return failed == 0 ? 0 : 1;
}

static int stress_test(int loops) {
  int failed = 0;
  if (loops <= 0)
    loops = 50;
  for (int i = 0; i < loops; ++i) {
    printf("stress loop %d/%d\n", i + 1, loops);
    if (jpg_roundtrip("main") != 0) {
      failed++;
      break;
    }
    if (audio_read_once((i & 1) ? 2 : 1) != 0) {
      failed++;
      break;
    }
  }
  printf("stress loops=%d failed=%d\n", loops, failed);
  return failed == 0 ? 0 : 1;
}

static int stress_http_safe_test(int loops) {
  int failed = 0;
  if (loops <= 0)
    loops = 50;
  for (int i = 0; i < loops; ++i) {
    mmf_camera_t* camera = NULL;
    mmf_video_frame_t frame;
    printf("stress-http-safe loop %d/%d\n", i + 1, loops);
    if (get_camera_frame((i & 1) ? "main" : "live", &camera, &frame) != 0) {
      failed++;
      break;
    }
    if (put_camera_frame((i & 1) ? "main" : "live", camera, &frame) != 0) {
      failed++;
      mmf_camera_close(camera);
      break;
    }
    mmf_camera_close(camera);
    if (audio_read_once((i & 1) ? 2 : 1) != 0) {
      failed++;
      break;
    }
  }
  printf("stress-http-safe loops=%d failed=%d\n", loops, failed);
  return failed == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  mmf_system_config_t sys = {MMF_TRUE, MMF_TRUE, 3000};
  (void)mmf_system_init(&sys);
  atexit(cleanup_system);

  if (argc < 2 || strcmp(argv[1], "list") == 0) {
    return list_outputs();
  }
  if (strcmp(argv[1], "system") == 0) {
    return system_once();
  }
  if (strcmp(argv[1], "snapshot") == 0 && argc >= 5) {
    return snapshot_source(argv[2], (mmf_camera_device_t)atoi(argv[3]), argv[4]);
  }
  if (strcmp(argv[1], "snapshot") == 0 && argc >= 4) {
    return snapshot_source(argv[2], MMF_CAMERA_DEVICE_FRONT, argv[3]);
  }
  if (strcmp(argv[1], "frame") == 0) {
    mmf_camera_t* camera = NULL;
    mmf_video_frame_t frame;
    const char* source = argc >= 3 ? argv[2] : "live";
    int ret = get_camera_frame(source, &camera, &frame);
    if (ret == 0) {
      ret = put_camera_frame(source, camera, &frame);
      mmf_camera_close(camera);
    }
    return ret;
  }
  if (strcmp(argv[1], "audio-read") == 0) {
    int channels = argc >= 3 ? atoi(argv[2]) : 1;
    return audio_read_once(channels);
  }
  if (strcmp(argv[1], "audio-control") == 0) {
    return audio_control_once();
  }
  if (strcmp(argv[1], "audio-3a-global") == 0 || strcmp(argv[1], "audio-3a") == 0) {
    return audio_3a_global_once();
  }
  if (strcmp(argv[1], "audio-codec") == 0) {
    return audio_codec_once();
  }
  if (strcmp(argv[1], "audio-loopback") == 0) {
    int channels = argc >= 3 ? atoi(argv[2]) : 1;
    int frames = argc >= 4 ? atoi(argv[3]) : 20;
    int enable_3a = argc >= 5 ? atoi(argv[4]) : 1;
    int volume = argc >= 6 ? atoi(argv[5]) : 24;
    return audio_loopback_once(channels, frames, enable_3a, volume);
  }
  if (strcmp(argv[1], "audio-3a-effect") == 0) {
    int channels = argc >= 3 ? atoi(argv[2]) : 1;
    int frames = argc >= 4 ? atoi(argv[3]) : 100;
    int volume = argc >= 5 ? atoi(argv[4]) : 16;
    return audio_3a_effect_once(channels, frames, volume);
  }
  if (strcmp(argv[1], "jpg-roundtrip") == 0) {
    return jpg_roundtrip(argc >= 3 ? argv[2] : "main");
  }
  if (strcmp(argv[1], "camera-control") == 0 || strcmp(argv[1], "isp-control") == 0) {
    return camera_control_once();
  }
  if (strcmp(argv[1], "display-frame") == 0) {
    return display_frame(argc >= 3 ? argv[2] : "screen");
  }
  if (strcmp(argv[1], "display-control") == 0) {
    return display_control_once();
  }
  if (strcmp(argv[1], "http-snapshot") == 0) {
    uint16_t port = argc >= 3 ? (uint16_t)atoi(argv[2]) : 18080;
    return http_probe("snapshot", port);
  }
  if (strcmp(argv[1], "http") == 0 && argc >= 3 && strcmp(argv[2], "snapshot") == 0) {
    uint16_t port = argc >= 4 ? (uint16_t)atoi(argv[3]) : 18080;
    return http_probe("snapshot", port);
  }
  if (strcmp(argv[1], "http-stream") == 0) {
    uint16_t port = argc >= 3 ? (uint16_t)atoi(argv[2]) : 18081;
    return http_probe("stream", port);
  }
  if (strcmp(argv[1], "http") == 0 && argc >= 3 && strcmp(argv[2], "stream") == 0) {
    uint16_t port = argc >= 4 ? (uint16_t)atoi(argv[3]) : 18081;
    return http_probe("stream", port);
  }
  if (strcmp(argv[1], "http-push") == 0) {
    uint16_t port = argc >= 3 ? (uint16_t)atoi(argv[2]) : 18082;
    return http_push_probe(port);
  }
  if (strcmp(argv[1], "http-publish-frame") == 0) {
    uint16_t port = argc >= 3 ? (uint16_t)atoi(argv[2]) : 18083;
    return http_publish_frame_probe(port);
  }
  if (strcmp(argv[1], "touch-status") == 0) {
    return touch_status_once();
  }
  if (strcmp(argv[1], "reentry") == 0) {
    return reentry_test(argc >= 3 ? atoi(argv[2]) : 10);
  }
  if (strcmp(argv[1], "stress") == 0) {
    return stress_test(argc >= 3 ? atoi(argv[2]) : 50);
  }
  if (strcmp(argv[1], "stress-http-safe") == 0) {
    return stress_http_safe_test(argc >= 3 ? atoi(argv[2]) : 50);
  }
  if (strcmp(argv[1], "all") == 0) {
    return all_test();
  }

  printf("usage:\n");
  printf("  %s system\n", argv[0]);
  printf("  %s list\n", argv[0]);
  printf("  %s frame [main|ai|live|subrgb|screen]\n", argv[0]);
  printf("  %s snapshot ai|live|subrgb|screen|rgb [device:0|1] out.jpg\n", argv[0]);
  printf("  %s audio-read [channels]\n", argv[0]);
  printf("  %s audio-3a-global\n", argv[0]);
  printf("  %s audio-3a\n", argv[0]);
  printf("  %s audio-control\n", argv[0]);
  printf("  %s audio-codec\n", argv[0]);
  printf("  %s audio-loopback [channels] [frames] [enable_3a] [volume]\n", argv[0]);
  printf("  %s audio-3a-effect [channels] [frames] [volume]\n", argv[0]);
  printf("  %s jpg-roundtrip [main|ai|live|subrgb]\n", argv[0]);
  printf("  %s camera-control\n", argv[0]);
  printf("  %s isp-control\n", argv[0]);
  printf("  %s display-frame [screen]\n", argv[0]);
  printf("  %s display-control\n", argv[0]);
  printf("  %s http-snapshot [port]\n", argv[0]);
  printf("  %s http-stream [port]\n", argv[0]);
  printf("  %s http snapshot [port]\n", argv[0]);
  printf("  %s http stream [port]\n", argv[0]);
  printf("  %s http-push [port]\n", argv[0]);
  printf("  %s http-publish-frame [port]\n", argv[0]);
  printf("  %s touch-status\n", argv[0]);
  printf("  %s reentry [loops]\n", argv[0]);
  printf("  %s stress [loops]\n", argv[0]);
  printf("  %s stress-http-safe [loops]\n", argv[0]);
  printf("  %s all\n", argv[0]);
  return 2;
}
