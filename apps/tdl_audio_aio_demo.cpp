#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "tdl_app/advanced.hpp"

namespace {

struct Options {
  std::string mode;
  std::string input_path;
  std::string output_path;
  int seconds = 5;
  int ai_device = 0;
  int ai_channel = 0;
  int ao_device = 0;
  int ao_channel = 0;
  int ai_card_id = -1;
  int ao_card_id = -1;
  int sample_rate = 16000;
  int bit_width = 16;
  int channels = 1;
  int points_per_frame = 160;
  int frame_count = 8;
  int frame_depth = 8;
  int ai_volume = 0;
  int ao_volume = 0;
  int timeout_ms = 1000;
  bool enable_ao_vqe = false;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_audio_aio_demo --mode record --output FILE [options]\n"
      << "  tdl_audio_aio_demo --mode play --input FILE [options]\n"
      << "  tdl_audio_aio_demo --mode loopback [options]\n"
      << "  tdl_audio_aio_demo --mode talk3a [options]\n"
      << "\n"
      << "Modes:\n"
      << "  record   Capture raw PCM from AI and write to file\n"
      << "  play     Read raw PCM from file and send to AO\n"
      << "  loopback Capture from AI and play to AO in real time\n"
      << "  talk3a   Loopback with AI talk VQE (AEC/ANR/AGC)\n"
      << "\n"
      << "Common options:\n"
      << "  --seconds N           Duration for record/loopback/talk3a (default 5)\n"
      << "  --sample-rate N       8000/16000/32000/44100/48000... (default 16000)\n"
      << "  --bit-width N         8/16/24/32 (default 16)\n"
      << "  --channels N          1 or 2 (default 1)\n"
      << "  --points-per-frame N  Samples per frame (default 160)\n"
      << "  --frame-count N       Driver frame count (default 8)\n"
      << "  --frame-depth N       AI user frame depth (default 8)\n"
      << "  --timeout-ms N        Frame timeout (default 1000)\n"
      << "  --ai-device N         AI device id (default 0)\n"
      << "  --ai-channel N        AI channel id (default 0)\n"
      << "  --ao-device N         AO device id (default 0)\n"
      << "  --ao-channel N        AO channel id (default 0)\n"
      << "  --ai-card-id N        AI card id (default -1)\n"
      << "  --ao-card-id N        AO card id (default -1)\n"
      << "  --ai-volume N         AI volume step (default 0)\n"
      << "  --ao-volume N         AO volume db (default 0)\n"
      << "  --enable-ao-vqe       Enable AO speaker VQE preset\n";
}

bool parseInt(const std::string &value, int *out) {
  try {
    *out = std::stoi(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool parseRequiredInt(int argc, char **argv, int *index, const char *name,
                      int *out) {
  if (*index + 1 >= argc) {
    std::cerr << name << " requires a value\n";
    return false;
  }
  ++(*index);
  if (!parseInt(argv[*index], out)) {
    std::cerr << "invalid value for " << name << ": " << argv[*index] << "\n";
    return false;
  }
  return true;
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mode") {
      if (i + 1 >= argc) {
        std::cerr << "--mode requires a value\n";
        return false;
      }
      opt->mode = argv[++i];
    } else if (arg == "--input") {
      if (i + 1 >= argc) {
        std::cerr << "--input requires a value\n";
        return false;
      }
      opt->input_path = argv[++i];
    } else if (arg == "--output") {
      if (i + 1 >= argc) {
        std::cerr << "--output requires a value\n";
        return false;
      }
      opt->output_path = argv[++i];
    } else if (arg == "--seconds") {
      if (!parseRequiredInt(argc, argv, &i, "--seconds", &opt->seconds)) {
        return false;
      }
    } else if (arg == "--sample-rate") {
      if (!parseRequiredInt(argc, argv, &i, "--sample-rate",
                            &opt->sample_rate)) {
        return false;
      }
    } else if (arg == "--bit-width") {
      if (!parseRequiredInt(argc, argv, &i, "--bit-width", &opt->bit_width)) {
        return false;
      }
    } else if (arg == "--channels") {
      if (!parseRequiredInt(argc, argv, &i, "--channels", &opt->channels)) {
        return false;
      }
    } else if (arg == "--points-per-frame") {
      if (!parseRequiredInt(argc, argv, &i, "--points-per-frame",
                            &opt->points_per_frame)) {
        return false;
      }
    } else if (arg == "--frame-count") {
      if (!parseRequiredInt(argc, argv, &i, "--frame-count",
                            &opt->frame_count)) {
        return false;
      }
    } else if (arg == "--frame-depth") {
      if (!parseRequiredInt(argc, argv, &i, "--frame-depth",
                            &opt->frame_depth)) {
        return false;
      }
    } else if (arg == "--timeout-ms") {
      if (!parseRequiredInt(argc, argv, &i, "--timeout-ms",
                            &opt->timeout_ms)) {
        return false;
      }
    } else if (arg == "--ai-device") {
      if (!parseRequiredInt(argc, argv, &i, "--ai-device",
                            &opt->ai_device)) {
        return false;
      }
    } else if (arg == "--ai-channel") {
      if (!parseRequiredInt(argc, argv, &i, "--ai-channel",
                            &opt->ai_channel)) {
        return false;
      }
    } else if (arg == "--ao-device") {
      if (!parseRequiredInt(argc, argv, &i, "--ao-device",
                            &opt->ao_device)) {
        return false;
      }
    } else if (arg == "--ao-channel") {
      if (!parseRequiredInt(argc, argv, &i, "--ao-channel",
                            &opt->ao_channel)) {
        return false;
      }
    } else if (arg == "--ai-card-id") {
      if (!parseRequiredInt(argc, argv, &i, "--ai-card-id",
                            &opt->ai_card_id)) {
        return false;
      }
    } else if (arg == "--ao-card-id") {
      if (!parseRequiredInt(argc, argv, &i, "--ao-card-id",
                            &opt->ao_card_id)) {
        return false;
      }
    } else if (arg == "--ai-volume") {
      if (!parseRequiredInt(argc, argv, &i, "--ai-volume",
                            &opt->ai_volume)) {
        return false;
      }
    } else if (arg == "--ao-volume") {
      if (!parseRequiredInt(argc, argv, &i, "--ao-volume",
                            &opt->ao_volume)) {
        return false;
      }
    } else if (arg == "--enable-ao-vqe") {
      opt->enable_ao_vqe = true;
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }

  if (opt->mode.empty()) {
    std::cerr << "--mode is required\n";
    return false;
  }
  if (opt->mode == "record" && opt->output_path.empty()) {
    std::cerr << "--output is required for record mode\n";
    return false;
  }
  if (opt->mode == "play" && opt->input_path.empty()) {
    std::cerr << "--input is required for play mode\n";
    return false;
  }
  if (opt->mode != "record" && opt->mode != "play" &&
      opt->mode != "loopback" && opt->mode != "talk3a") {
    std::cerr << "unsupported mode: " << opt->mode << "\n";
    return false;
  }
  if (opt->seconds <= 0) {
    std::cerr << "--seconds must be > 0\n";
    return false;
  }
  if (opt->channels != 1 && opt->channels != 2) {
    std::cerr << "--channels must be 1 or 2\n";
    return false;
  }
  if (opt->bit_width != 8 && opt->bit_width != 16 && opt->bit_width != 24 &&
      opt->bit_width != 32) {
    std::cerr << "--bit-width must be 8/16/24/32\n";
    return false;
  }
  if (opt->points_per_frame <= 0 || opt->frame_count <= 0 ||
      opt->frame_depth <= 0) {
    std::cerr << "frame parameters must be > 0\n";
    return false;
  }
  return true;
}

tdl_app::AudioSampleRate toSampleRate(int sample_rate) {
  switch (sample_rate) {
    case 8000:
      return tdl_app::AudioSampleRate::Hz8000;
    case 11025:
      return tdl_app::AudioSampleRate::Hz11025;
    case 16000:
      return tdl_app::AudioSampleRate::Hz16000;
    case 22050:
      return tdl_app::AudioSampleRate::Hz22050;
    case 24000:
      return tdl_app::AudioSampleRate::Hz24000;
    case 32000:
      return tdl_app::AudioSampleRate::Hz32000;
    case 44100:
      return tdl_app::AudioSampleRate::Hz44100;
    case 48000:
      return tdl_app::AudioSampleRate::Hz48000;
    case 64000:
      return tdl_app::AudioSampleRate::Hz64000;
    default:
      throw std::runtime_error("unsupported sample rate");
  }
}

tdl_app::AudioBitWidth toBitWidth(int bit_width) {
  switch (bit_width) {
    case 8:
      return tdl_app::AudioBitWidth::Bits8;
    case 16:
      return tdl_app::AudioBitWidth::Bits16;
    case 24:
      return tdl_app::AudioBitWidth::Bits24;
    case 32:
      return tdl_app::AudioBitWidth::Bits32;
    default:
      throw std::runtime_error("unsupported bit width");
  }
}

tdl_app::AudioSoundMode toSoundMode(int channels) {
  return channels == 1 ? tdl_app::AudioSoundMode::Mono
                       : tdl_app::AudioSoundMode::Stereo;
}

tdl_app::AudioInput::Config makeAiConfig(const Options &opt) {
  tdl_app::AudioInput::Config config;
  config.device = opt.ai_device;
  config.channel = opt.ai_channel;
  config.card_id = opt.ai_card_id;
  config.sample_rate = toSampleRate(opt.sample_rate);
  config.bit_width = toBitWidth(opt.bit_width);
  config.sound_mode = toSoundMode(opt.channels);
  config.points_per_frame = opt.points_per_frame;
  config.frame_count = opt.frame_count;
  config.frame_depth = opt.frame_depth;
  config.channel_count = opt.channels;
  config.volume_step = opt.ai_volume;
  return config;
}

tdl_app::AudioOutput::Config makeAoConfig(const Options &opt) {
  tdl_app::AudioOutput::Config config;
  config.device = opt.ao_device;
  config.channel = opt.ao_channel;
  config.card_id = opt.ao_card_id;
  config.sample_rate = toSampleRate(opt.sample_rate);
  config.bit_width = toBitWidth(opt.bit_width);
  config.sound_mode = toSoundMode(opt.channels);
  config.points_per_frame = opt.points_per_frame;
  config.frame_count = opt.frame_count;
  config.channel_count = opt.channels;
  config.volume_db = opt.ao_volume;
  return config;
}

std::size_t bytesPerSample(const Options &opt) {
  return static_cast<std::size_t>(opt.bit_width / 8);
}

std::size_t bytesPerFramePerChannel(const Options &opt) {
  return static_cast<std::size_t>(opt.points_per_frame) * bytesPerSample(opt);
}

std::size_t totalFrames(const Options &opt) {
  const std::size_t samples = static_cast<std::size_t>(opt.seconds) *
                              static_cast<std::size_t>(opt.sample_rate);
  const std::size_t points = static_cast<std::size_t>(opt.points_per_frame);
  return (samples + points - 1) / points;
}

bool appendFrameAsPcm(const tdl_app::AudioFrame &frame, std::ofstream *ofs,
                      std::string *error) {
  if (!ofs || !(*ofs)) {
    if (error) *error = "output file is not open";
    return false;
  }
  if (frame.channels.empty()) {
    if (error) *error = "audio frame is empty";
    return false;
  }

  const std::size_t channel_count = frame.channels.size();
  const std::size_t bytes = frame.channels.front().size();
  for (std::size_t i = 1; i < channel_count; ++i) {
    if (frame.channels[i].size() != bytes) {
      if (error) *error = "audio frame channel sizes do not match";
      return false;
    }
  }

  if (channel_count == 1) {
    ofs->write(reinterpret_cast<const char *>(frame.channels[0].data()),
               static_cast<std::streamsize>(bytes));
    return ofs->good();
  }

  const std::size_t sample_bytes = static_cast<std::size_t>(
      static_cast<int>(frame.bit_width) / 8);
  if (sample_bytes == 0 || bytes % sample_bytes != 0) {
    if (error) *error = "audio frame size does not align with bit width";
    return false;
  }
  const std::size_t sample_count = bytes / sample_bytes;
  std::vector<std::uint8_t> interleaved(bytes * channel_count);
  for (std::size_t sample = 0; sample < sample_count; ++sample) {
    for (std::size_t ch = 0; ch < channel_count; ++ch) {
      const std::size_t src_offset = sample * sample_bytes;
      const std::size_t dst_offset =
          (sample * channel_count + ch) * sample_bytes;
      std::copy_n(frame.channels[ch].data() + src_offset, sample_bytes,
                  interleaved.data() + dst_offset);
    }
  }
  ofs->write(reinterpret_cast<const char *>(interleaved.data()),
             static_cast<std::streamsize>(interleaved.size()));
  return ofs->good();
}

bool readFrameFromPcm(std::ifstream *ifs, const Options &opt,
                      tdl_app::AudioFrame *frame, bool *eof_reached,
                      std::string *error) {
  if (!ifs || !(*ifs)) {
    if (error) *error = "input file is not open";
    return false;
  }
  if (!frame) {
    if (error) *error = "audio frame pointer is null";
    return false;
  }
  if (!eof_reached) {
    if (error) *error = "eof flag pointer is null";
    return false;
  }

  const std::size_t sample_bytes = bytesPerSample(opt);
  const std::size_t frame_bytes_per_channel = bytesPerFramePerChannel(opt);
  const std::size_t total_bytes = frame_bytes_per_channel * opt.channels;
  std::vector<std::uint8_t> packed(total_bytes, 0);
  ifs->read(reinterpret_cast<char *>(packed.data()),
            static_cast<std::streamsize>(packed.size()));
  const std::streamsize got = ifs->gcount();
  if (got <= 0) {
    *eof_reached = true;
    return true;
  }
  if (static_cast<std::size_t>(got) < packed.size()) {
    std::fill(packed.begin() + got, packed.end(), 0);
    *eof_reached = true;
  } else {
    *eof_reached = false;
  }

  frame->bit_width = toBitWidth(opt.bit_width);
  frame->sound_mode = toSoundMode(opt.channels);
  frame->bytes_per_channel = static_cast<std::uint32_t>(frame_bytes_per_channel);
  frame->channels.assign(opt.channels,
                         std::vector<std::uint8_t>(frame_bytes_per_channel, 0));

  if (opt.channels == 1) {
    std::copy_n(packed.data(), frame_bytes_per_channel, frame->channels[0].data());
    return true;
  }

  for (int sample = 0; sample < opt.points_per_frame; ++sample) {
    for (int ch = 0; ch < opt.channels; ++ch) {
      const std::size_t src_offset =
          (static_cast<std::size_t>(sample) * opt.channels + ch) * sample_bytes;
      const std::size_t dst_offset =
          static_cast<std::size_t>(sample) * sample_bytes;
      std::copy_n(packed.data() + src_offset, sample_bytes,
                  frame->channels[ch].data() + dst_offset);
    }
  }
  return true;
}

bool runRecord(const Options &opt, std::string *error) {
  tdl_app::AudioInput input(makeAiConfig(opt));
  if (!input.open(error)) {
    return false;
  }

  std::ofstream ofs(opt.output_path, std::ios::binary);
  if (!ofs) {
    if (error) *error = "failed to open output file: " + opt.output_path;
    return false;
  }

  const std::size_t frame_total = totalFrames(opt);
  for (std::size_t i = 0; i < frame_total; ++i) {
    tdl_app::AudioFrame frame;
    if (!input.readFrame(&frame, opt.timeout_ms, error)) {
      return false;
    }
    if (!appendFrameAsPcm(frame, &ofs, error)) {
      return false;
    }
  }

  std::cout << "recorded: " << opt.output_path
            << " frames=" << frame_total << "\n";
  return true;
}

bool runPlay(const Options &opt, std::string *error) {
  tdl_app::AudioOutput output(makeAoConfig(opt));
  if (!output.open(error)) {
    return false;
  }
  if (opt.enable_ao_vqe) {
    if (!output.setVqeConfig(
            tdl_app::AudioOutputVqeConfig::speakerEnhance(opt.sample_rate,
                                                          opt.channels),
            error) ||
        !output.enableVqe(error)) {
      return false;
    }
  }

  std::ifstream ifs(opt.input_path, std::ios::binary);
  if (!ifs) {
    if (error) *error = "failed to open input file: " + opt.input_path;
    return false;
  }

  std::size_t frame_index = 0;
  for (;;) {
    tdl_app::AudioFrame frame;
    bool eof_reached = false;
    if (!readFrameFromPcm(&ifs, opt, &frame, &eof_reached, error)) {
      return false;
    }
    if (frame.channels.empty()) {
      break;
    }
    frame.sequence = static_cast<std::uint32_t>(frame_index++);
    if (!output.writeFrame(frame, opt.timeout_ms, error)) {
      return false;
    }
    if (eof_reached) {
      break;
    }
  }

  std::cout << "played: " << opt.input_path
            << " frames=" << frame_index << "\n";
  return true;
}

bool runLoopback(const Options &opt, bool enable_talk_vqe, std::string *error) {
  tdl_app::AudioOutput output(makeAoConfig(opt));
  if (!output.open(error)) {
    return false;
  }
  if (opt.enable_ao_vqe) {
    if (!output.setVqeConfig(
            tdl_app::AudioOutputVqeConfig::speakerEnhance(opt.sample_rate,
                                                          opt.channels),
            error) ||
        !output.enableVqe(error)) {
      return false;
    }
  }

  tdl_app::AudioInput input(makeAiConfig(opt));
  if (!input.open(error)) {
    return false;
  }
  if (enable_talk_vqe) {
    if (!input.configureTalkVqe(
            tdl_app::AudioTalkVqeConfig::talk3a(opt.sample_rate),
            opt.ao_device, opt.ao_channel, error) ||
        !input.enableVqe(error)) {
      return false;
    }
  }

  const std::size_t frame_total = totalFrames(opt);
  for (std::size_t i = 0; i < frame_total; ++i) {
    tdl_app::AudioFrame frame;
    if (!input.readFrame(&frame, opt.timeout_ms, error)) {
      return false;
    }
    frame.sequence = static_cast<std::uint32_t>(i);
    if (!output.writeFrame(frame, opt.timeout_ms, error)) {
      return false;
    }
  }

  std::cout << (enable_talk_vqe ? "talk3a" : "loopback")
            << ": frames=" << frame_total << "\n";
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::string error;
  try {
    if (opt.mode == "record") {
      if (!runRecord(opt, &error)) {
        std::cerr << "record failed: " << error << "\n";
        return 2;
      }
    } else if (opt.mode == "play") {
      if (!runPlay(opt, &error)) {
        std::cerr << "play failed: " << error << "\n";
        return 3;
      }
    } else if (opt.mode == "loopback") {
      if (!runLoopback(opt, false, &error)) {
        std::cerr << "loopback failed: " << error << "\n";
        return 4;
      }
    } else if (opt.mode == "talk3a") {
      if (!runLoopback(opt, true, &error)) {
        std::cerr << "talk3a failed: " << error << "\n";
        return 5;
      }
    }
  } catch (const std::exception &ex) {
    std::cerr << "invalid audio option: " << ex.what() << "\n";
    return 6;
  }

  return 0;
}
