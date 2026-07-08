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

static unsigned peak_pcm16(const mmf_audio_frame_t* frame) {
  const int16_t* samples = (const int16_t*)frame->data;
  size_t count = frame->bytes / sizeof(int16_t);
  unsigned peak = 0;
  for (size_t i = 0; i < count; ++i) {
    int v = samples[i] < 0 ? -samples[i] : samples[i];
    if ((unsigned)v > peak)
      peak = (unsigned)v;
  }
  return peak;
}

static void print_usage(const char* argv0) {
  printf(
      "usage: %s [duration_s] [sample_rate] [channels] [volume] [enable_3a] [loopback] [codec]\n",
      argv0);
  printf("example: %s 30 16000 2 32 1 1 1\n", argv0);
  printf("  enable_3a: 0/1, applies AEC/NS/AGC to capture\n");
  printf("  loopback : 0/1, write captured chunks to AO while reading AI\n");
  printf("  codec    : 0/1, run G711A encode/decode on every captured chunk\n");
}

int main(int argc, char** argv) {
  int duration_s = argc >= 2 ? atoi(argv[1]) : 20;
  int sample_rate = argc >= 3 ? atoi(argv[2]) : 16000;
  int channels = argc >= 4 ? atoi(argv[3]) : 1;
  int volume = argc >= 5 ? atoi(argv[4]) : 32;
  int enable_3a = argc >= 6 ? atoi(argv[5]) : 1;
  int loopback = argc >= 7 ? atoi(argv[6]) : 1;
  int codec = argc >= 8 ? atoi(argv[7]) : 1;

  if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
    print_usage(argv[0]);
    return 0;
  }
  if (duration_s <= 0)
    duration_s = 20;
  if (sample_rate <= 0)
    sample_rate = 16000;
  if (channels != 1 && channels != 2)
    channels = 1;
  if (volume < 0)
    volume = 0;
  if (volume > 63)
    volume = 63;

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  mmf_system_config_t sys = {MMF_TRUE, MMF_TRUE, 3000};
  mmf_result_t ret = mmf_system_init(&sys);
  if (ret != MMF_OK) {
    printf("system init failed: %d %s\n", ret, mmf_system_last_error());
    return 1;
  }

  mmf_audio_input_config_t in_cfg;
  mmf_audio_output_config_t out_cfg;
  mmf_audio_input_t* input = NULL;
  mmf_audio_output_t* output = NULL;
  mmf_audio_encoder_t* encoder = NULL;

  mmf_audio_get_default_input_config(&in_cfg);
  mmf_audio_get_default_output_config(&out_cfg);
  in_cfg.io.sample_rate = (uint32_t)sample_rate;
  in_cfg.io.channels = (uint32_t)channels;
  in_cfg.io.input_volume = volume;
  in_cfg.enable_3a = enable_3a ? MMF_TRUE : MMF_FALSE;
  in_cfg.config_3a.aec_enable = enable_3a ? MMF_TRUE : MMF_FALSE;
  in_cfg.config_3a.ns_enable = enable_3a ? MMF_TRUE : MMF_FALSE;
  in_cfg.config_3a.agc_enable = enable_3a ? MMF_TRUE : MMF_FALSE;

  out_cfg.io.sample_rate = (uint32_t)sample_rate;
  out_cfg.io.channels = (uint32_t)channels;
  out_cfg.io.output_volume = volume;
  out_cfg.provide_aec_reference = loopback ? MMF_TRUE : MMF_FALSE;

  ret = mmf_audio_input_open(&in_cfg, &input);
  if (ret != MMF_OK) {
    printf("audio input open failed: %d %s\n", ret, mmf_system_last_error());
    mmf_system_deinit();
    return 1;
  }
  (void)mmf_audio_input_set_volume(input, volume);

  if (loopback) {
    ret = mmf_audio_output_open(&out_cfg, &output);
    if (ret != MMF_OK) {
      printf("audio output open failed: %d %s\n", ret, mmf_system_last_error());
      mmf_audio_input_close(input);
      mmf_system_deinit();
      return 1;
    }
    (void)mmf_audio_output_set_volume(output, volume);
  }

  if (codec) {
    mmf_audio_codec_config_t codec_cfg;
    memset(&codec_cfg, 0, sizeof(codec_cfg));
    codec_cfg.codec = MMF_CODEC_G711A;
    codec_cfg.sample_rate = (uint32_t)sample_rate;
    codec_cfg.channels = (uint32_t)channels;
    ret = mmf_audio_encoder_open(&codec_cfg, &encoder);
    if (ret != MMF_OK) {
      printf("audio encoder open failed, continue without codec: %d %s\n", ret,
             mmf_system_last_error());
      if (encoder) {
        mmf_audio_encoder_close(encoder);
        encoder = NULL;
      }
      codec = 0;
    }
  }

  printf(
      "audio full duplex: duration=%ds rate=%d channels=%d volume=%d 3a=%d loopback=%d codec=%d\n",
      duration_s, sample_rate, channels, volume, enable_3a, loopback, codec);

  uint64_t read_frames = 0;
  uint64_t written_frames = 0;
  uint64_t encoded_packets = 0;
  uint64_t decoded_frames = 0;
  uint64_t read_bytes = 0;
  uint64_t codec_bytes = 0;
  uint64_t errors = 0;
  unsigned peak = 0;
  time_t start = time(NULL);
  time_t last_print = start;

  while (!g_stop && (int)(time(NULL) - start) < duration_s) {
    mmf_audio_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    ret = mmf_audio_input_read(input, &frame, 1000);
    if (ret != MMF_OK) {
      printf("audio read failed: %d %s\n", ret, mmf_system_last_error());
      errors++;
      continue;
    }

    read_frames++;
    read_bytes += frame.bytes;
    unsigned frame_peak = peak_pcm16(&frame);
    if (frame_peak > peak)
      peak = frame_peak;

    mmf_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    if (codec && encoder) {
      ret = mmf_audio_encoder_encode(encoder, &frame, &packet);
      if (ret == MMF_OK) {
        encoded_packets++;
        codec_bytes += packet.bytes;
      } else {
        printf("audio encode frame failed: %d %s\n", ret, mmf_system_last_error());
        errors++;
      }
    }

    if (loopback && output) {
      ret = mmf_audio_output_write(output, &frame, 1000);
      if (ret == MMF_OK) {
        written_frames++;
      } else {
        printf("audio write failed: %d %s\n", ret, mmf_system_last_error());
        errors++;
      }
    }

    if (encoder && packet.data)
      mmf_audio_encoder_release(encoder, &packet);
    mmf_audio_input_release(input, &frame);

    if (time(NULL) != last_print) {
      last_print = time(NULL);
      printf(
          "stats: read=%llu write=%llu enc=%llu dec=%llu bytes=%llu codec_bytes=%llu peak=%u "
          "errors=%llu\n",
          (unsigned long long)read_frames, (unsigned long long)written_frames,
          (unsigned long long)encoded_packets, (unsigned long long)decoded_frames,
          (unsigned long long)read_bytes, (unsigned long long)codec_bytes, peak,
          (unsigned long long)errors);
    }
  }

  if (encoder)
    mmf_audio_encoder_close(encoder);
  mmf_audio_input_close(input);
  if (output) {
    mmf_audio_output_drain(output);
    mmf_audio_output_close(output);
  }
  mmf_system_deinit();

  printf(
      "final: read=%llu write=%llu enc=%llu dec=%llu bytes=%llu codec_bytes=%llu peak=%u "
      "errors=%llu\n",
      (unsigned long long)read_frames, (unsigned long long)written_frames,
      (unsigned long long)encoded_packets, (unsigned long long)decoded_frames,
      (unsigned long long)read_bytes, (unsigned long long)codec_bytes, peak,
      (unsigned long long)errors);
  return errors == 0 ? 0 : 1;
}
