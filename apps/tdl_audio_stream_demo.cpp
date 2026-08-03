#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "tdl_app/audio.hpp"

namespace {

struct Options {
  std::string mode;
  std::string input_path;
  std::string output_path;
  std::string payload = "g711a";
  int seconds = 3;
  int sample_rate = 16000;
  int channels = 1;
  int bit_depth = 16;
  int points_per_frame = 160;
  int frame_count = 8;
  int frame_depth = 8;
  int ai_device = 0;
  int ai_channel = 0;
  int ao_device = 0;
  int ao_channel = 0;
  int ai_card_id = -1;
  int ao_card_id = -1;
  int ai_volume = 24;
  int ao_volume = 24;
  int timeout_ms = 1000;
  bool enable_talk_vqe = false;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_audio_stream_demo --mode status\n"
      << "  tdl_audio_stream_demo --mode wav-record --output FILE.wav [options]\n"
      << "  tdl_audio_stream_demo --mode wav-play --input FILE.wav [options]\n"
      << "  tdl_audio_stream_demo --mode pcm-capture --output FILE.pcm [options]\n"
      << "  tdl_audio_stream_demo --mode pcm-play --input FILE.pcm [options]\n"
      << "  tdl_audio_stream_demo --mode loopback [options]\n"
      << "  tdl_audio_stream_demo --mode talk3a-loopback [options]\n"
      << "  tdl_audio_stream_demo --mode encode --output FILE.bin [--payload g711a|g711u] [options]\n"
      << "  tdl_audio_stream_demo --mode decode --input FILE.bin [options]\n"
      << "\n"
      << "Options:\n"
      << "  --seconds N           Duration in seconds (default 3)\n"
      << "  --sample-rate N       Sample rate (default 16000)\n"
      << "  --channels N          1 or 2 (default 1)\n"
      << "  --bit-depth N         8/16/24/32 (default 16)\n"
      << "  --points-per-frame N  Samples per frame (default 160)\n"
      << "  --frame-count N       Driver frame count (default 8)\n"
      << "  --frame-depth N       AI user frame depth (default 8)\n"
      << "  --ai-device N         AI device id (default 0)\n"
      << "  --ai-channel N        AI channel id (default 0)\n"
      << "  --ao-device N         AO device id (default 0)\n"
      << "  --ao-channel N        AO channel id (default 0)\n"
      << "  --ai-card-id N        AI card id (default -1)\n"
      << "  --ao-card-id N        AO card id (default -1)\n"
      << "  --ai-volume N         AI volume step (default 24)\n"
      << "  --ao-volume N         AO volume db (default 24)\n"
      << "  --timeout-ms N        IO timeout in ms (default 1000)\n"
      << "  --enable-talk-vqe     Enable AI talk VQE on capture/session path\n";
}

bool parseInt(const std::string &value, int *out) {
  try {
    *out = std::stoi(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool requireValue(int argc, char **argv, int *index, const char *name,
                  std::string *value) {
  if (*index + 1 >= argc) {
    std::cerr << name << " requires a value\n";
    return false;
  }
  *value = argv[++(*index)];
  return true;
}

bool requireInt(int argc, char **argv, int *index, const char *name,
                int *value) {
  std::string text;
  if (!requireValue(argc, argv, index, name, &text)) {
    return false;
  }
  if (!parseInt(text, value)) {
    std::cerr << "invalid value for " << name << ": " << text << "\n";
    return false;
  }
  return true;
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mode") {
      if (!requireValue(argc, argv, &i, "--mode", &opt->mode)) return false;
    } else if (arg == "--input") {
      if (!requireValue(argc, argv, &i, "--input", &opt->input_path)) return false;
    } else if (arg == "--output") {
      if (!requireValue(argc, argv, &i, "--output", &opt->output_path)) return false;
    } else if (arg == "--payload") {
      if (!requireValue(argc, argv, &i, "--payload", &opt->payload)) return false;
    } else if (arg == "--seconds") {
      if (!requireInt(argc, argv, &i, "--seconds", &opt->seconds)) return false;
    } else if (arg == "--sample-rate") {
      if (!requireInt(argc, argv, &i, "--sample-rate", &opt->sample_rate)) return false;
    } else if (arg == "--channels") {
      if (!requireInt(argc, argv, &i, "--channels", &opt->channels)) return false;
    } else if (arg == "--bit-depth") {
      if (!requireInt(argc, argv, &i, "--bit-depth", &opt->bit_depth)) return false;
    } else if (arg == "--points-per-frame") {
      if (!requireInt(argc, argv, &i, "--points-per-frame", &opt->points_per_frame)) return false;
    } else if (arg == "--frame-count") {
      if (!requireInt(argc, argv, &i, "--frame-count", &opt->frame_count)) return false;
    } else if (arg == "--frame-depth") {
      if (!requireInt(argc, argv, &i, "--frame-depth", &opt->frame_depth)) return false;
    } else if (arg == "--ai-device") {
      if (!requireInt(argc, argv, &i, "--ai-device", &opt->ai_device)) return false;
    } else if (arg == "--ai-channel") {
      if (!requireInt(argc, argv, &i, "--ai-channel", &opt->ai_channel)) return false;
    } else if (arg == "--ao-device") {
      if (!requireInt(argc, argv, &i, "--ao-device", &opt->ao_device)) return false;
    } else if (arg == "--ao-channel") {
      if (!requireInt(argc, argv, &i, "--ao-channel", &opt->ao_channel)) return false;
    } else if (arg == "--ai-card-id") {
      if (!requireInt(argc, argv, &i, "--ai-card-id", &opt->ai_card_id)) return false;
    } else if (arg == "--ao-card-id") {
      if (!requireInt(argc, argv, &i, "--ao-card-id", &opt->ao_card_id)) return false;
    } else if (arg == "--ai-volume") {
      if (!requireInt(argc, argv, &i, "--ai-volume", &opt->ai_volume)) return false;
    } else if (arg == "--ao-volume") {
      if (!requireInt(argc, argv, &i, "--ao-volume", &opt->ao_volume)) return false;
    } else if (arg == "--timeout-ms") {
      if (!requireInt(argc, argv, &i, "--timeout-ms", &opt->timeout_ms)) return false;
    } else if (arg == "--enable-talk-vqe") {
      opt->enable_talk_vqe = true;
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
  if ((opt->mode == "wav-record" || opt->mode == "pcm-capture" || opt->mode == "encode") &&
      opt->output_path.empty()) {
    std::cerr << "--output is required for mode " << opt->mode << "\n";
    return false;
  }
  if ((opt->mode == "wav-play" || opt->mode == "pcm-play" || opt->mode == "decode") &&
      opt->input_path.empty()) {
    std::cerr << "--input is required for mode " << opt->mode << "\n";
    return false;
  }
  if (opt->seconds < 0) {
    std::cerr << "--seconds must be >= 0\n";
    return false;
  }
  if (opt->channels != 1 && opt->channels != 2) {
    std::cerr << "--channels must be 1 or 2\n";
    return false;
  }
  if (opt->bit_depth != 8 && opt->bit_depth != 16 && opt->bit_depth != 24 &&
      opt->bit_depth != 32) {
    std::cerr << "--bit-depth must be 8/16/24/32\n";
    return false;
  }
  if (opt->points_per_frame <= 0 || opt->frame_count <= 0 || opt->frame_depth <= 0) {
    std::cerr << "frame parameters must be > 0\n";
    return false;
  }
  if (opt->payload != "g711a" && opt->payload != "g711u") {
    std::cerr << "--payload only supports g711a or g711u\n";
    return false;
  }
  return true;
}

std::size_t bytesPerSample(const Options &opt) {
  return static_cast<std::size_t>(opt.bit_depth / 8);
}

std::size_t frameBytes(const Options &opt) {
  return static_cast<std::size_t>(opt.points_per_frame) *
         static_cast<std::size_t>(opt.channels) * bytesPerSample(opt);
}

std::uint64_t targetSamples(const Options &opt) {
  return opt.seconds <= 0
      ? 0
      : static_cast<std::uint64_t>(opt.seconds) * static_cast<std::uint64_t>(opt.sample_rate);
}

tdl_app::AudioIoConfig makeIoConfig(const Options &opt) {
  tdl_app::AudioIoConfig config;
  config.sample_rate = opt.sample_rate;
  config.channels = opt.channels;
  config.bit_depth = opt.bit_depth;
  config.points_per_frame = opt.points_per_frame;
  config.frame_count = opt.frame_count;
  config.frame_depth = opt.frame_depth;
  config.ai_device = opt.ai_device;
  config.ai_channel = opt.ai_channel;
  config.ao_device = opt.ao_device;
  config.ao_channel = opt.ao_channel;
  config.ai_card_id = opt.ai_card_id;
  config.ao_card_id = opt.ao_card_id;
  config.ai_volume = opt.ai_volume;
  config.ao_volume = opt.ao_volume;
  config.timeout_ms = opt.timeout_ms;
  return config;
}

tdl_app::AudioPayloadType parsePayload(const std::string &payload) {
  if (payload == "g711u") {
    return tdl_app::AudioPayloadType::G711U;
  }
  return tdl_app::AudioPayloadType::G711A;
}

void writeU32(std::ofstream *out, std::uint32_t value) {
  out->put(static_cast<char>(value & 0xff));
  out->put(static_cast<char>((value >> 8) & 0xff));
  out->put(static_cast<char>((value >> 16) & 0xff));
  out->put(static_cast<char>((value >> 24) & 0xff));
}

bool readU32(std::ifstream *in, std::uint32_t *value) {
  char bytes[4] = {0, 0, 0, 0};
  in->read(bytes, 4);
  if (in->gcount() != 4) {
    return false;
  }
  *value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[0])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[3])) << 24);
  return true;
}

bool writeEncodedHeader(std::ofstream *out, const Options &opt, std::string *error) {
  out->write("TAUD", 4);
  writeU32(out, 1);
  writeU32(out, static_cast<std::uint32_t>(parsePayload(opt.payload)));
  writeU32(out, static_cast<std::uint32_t>(opt.sample_rate));
  writeU32(out, static_cast<std::uint32_t>(opt.channels));
  writeU32(out, static_cast<std::uint32_t>(opt.bit_depth));
  writeU32(out, static_cast<std::uint32_t>(opt.points_per_frame));
  if (!(*out)) {
    if (error) *error = "failed to write encoded file header";
    return false;
  }
  return true;
}

bool readEncodedHeader(std::ifstream *in, Options *opt, std::string *error) {
  char magic[4] = {0, 0, 0, 0};
  in->read(magic, 4);
  if (in->gcount() != 4 || std::string(magic, 4) != "TAUD") {
    if (error) *error = "invalid encoded file header";
    return false;
  }
  std::uint32_t version = 0;
  std::uint32_t payload = 0;
  std::uint32_t sample_rate = 0;
  std::uint32_t channels = 0;
  std::uint32_t bit_depth = 0;
  std::uint32_t points_per_frame = 0;
  if (!readU32(in, &version) || !readU32(in, &payload) || !readU32(in, &sample_rate) ||
      !readU32(in, &channels) || !readU32(in, &bit_depth) || !readU32(in, &points_per_frame)) {
    if (error) *error = "encoded file header is truncated";
    return false;
  }
  if (version != 1) {
    if (error) *error = "unsupported encoded file version";
    return false;
  }
  opt->payload = payload == static_cast<std::uint32_t>(tdl_app::AudioPayloadType::G711U)
      ? "g711u"
      : "g711a";
  opt->sample_rate = static_cast<int>(sample_rate);
  opt->channels = static_cast<int>(channels);
  opt->bit_depth = static_cast<int>(bit_depth);
  opt->points_per_frame = static_cast<int>(points_per_frame);
  return true;
}

tdl_app::AudioEncoder::Config makeEncoderConfig(const Options &opt) {
  if (opt.payload == "g711u") {
    return tdl_app::AudioEncoder::g711u(0, opt.points_per_frame);
  }
  return tdl_app::AudioEncoder::g711a(0, opt.points_per_frame);
}

tdl_app::AudioDecoder::Config makeDecoderConfig(const Options &opt) {
  if (opt.payload == "g711u") {
    return tdl_app::AudioDecoder::g711u(0, opt.sample_rate);
  }
  return tdl_app::AudioDecoder::g711a(0, opt.sample_rate);
}

void printStatus(const tdl_app::AudioStatus &status) {
  std::cout
      << "audio runtime=" << (status.runtime_ready ? "ready" : "not-ready") << "\n"
      << "input_stream_open=" << (status.input_stream_open ? "yes" : "no") << "\n"
      << "output_stream_open=" << (status.output_stream_open ? "yes" : "no") << "\n"
      << "session_open=" << (status.session_open ? "yes" : "no") << "\n"
      << "talk_vqe_supported=" << (status.talk_vqe_supported ? "yes" : "no") << "\n"
      << "encoder_supported=" << (status.encoder_supported ? "yes" : "no") << "\n"
      << "decoder_supported=" << (status.decoder_supported ? "yes" : "no") << "\n"
      << "default_io=" << status.sample_rate << "Hz/" << status.bit_depth << "bit/"
      << status.channels << "ch ai(" << status.ai_device << "," << status.ai_channel << ") ao("
      << status.ao_device << "," << status.ao_channel << ")\n";
  if (!status.note.empty()) {
    std::cout << "note=" << status.note << "\n";
  }
}

bool runStatus(std::string *error) {
  (void)error;
  tdl_app::Audio audio;
  printStatus(audio.status());
  return true;
}

bool runWavRecord(const Options &opt, std::string *error) {
  tdl_app::Audio audio;
  return audio.recordWav(opt.output_path, static_cast<double>(opt.seconds), makeIoConfig(opt), error);
}

bool runWavPlay(const Options &opt, std::string *error) {
  tdl_app::Audio audio;
  return audio.playWav(opt.input_path, makeIoConfig(opt), error);
}

bool runPcmCapture(const Options &opt, std::string *error) {
  tdl_app::Audio audio;
  tdl_app::AudioInputStreamConfig config;
  config.io = makeIoConfig(opt);
  config.enable_talk_vqe = opt.enable_talk_vqe;
  config.talk_vqe = tdl_app::AudioTalkVqeConfig::talk3a(opt.sample_rate);
  config.reference_output_device = opt.ao_device;
  config.reference_output_channel = opt.ao_channel;
  if (!audio.openInputStream(config, error)) {
    return false;
  }

  bool ok = false;
  std::ofstream out(opt.output_path, std::ios::binary);
  if (!out) {
    if (error) *error = "failed to open pcm output file: " + opt.output_path;
    audio.closeInputStream();
    return false;
  }

  const std::uint64_t target = targetSamples(opt);
  std::uint64_t captured = 0;
  const std::size_t bytes_per_audio_sample = bytesPerSample(opt) * static_cast<std::size_t>(opt.channels);
  while (target == 0 || captured < target) {
    tdl_app::AudioPcmChunk chunk;
    if (!audio.readInputChunk(&chunk, error)) {
      goto done;
    }
    if (target != 0) {
      const std::uint64_t remain = target - captured;
      const std::size_t max_bytes = static_cast<std::size_t>(remain) * bytes_per_audio_sample;
      if (chunk.data.size() > max_bytes) {
        chunk.data.resize(max_bytes);
      }
    }
    out.write(reinterpret_cast<const char *>(chunk.data.data()), static_cast<std::streamsize>(chunk.data.size()));
    if (!out) {
      if (error) *error = "failed to write pcm output file: " + opt.output_path;
      goto done;
    }
    captured += bytes_per_audio_sample == 0 ? 0 : (chunk.data.size() / bytes_per_audio_sample);
    if (target == 0 && chunk.data.empty()) {
      break;
    }
  }

  ok = true;

done:
  audio.closeInputStream();
  return ok;
}

bool runPcmPlay(const Options &opt, std::string *error) {
  tdl_app::Audio audio;
  const tdl_app::AudioIoConfig io = makeIoConfig(opt);
  if (!audio.openOutputStream(io, error)) {
    return false;
  }

  bool ok = false;
  std::ifstream in(opt.input_path, std::ios::binary);
  if (!in) {
    if (error) *error = "failed to open pcm input file: " + opt.input_path;
    audio.closeOutputStream();
    return false;
  }

  const std::size_t chunk_bytes = frameBytes(opt);
  std::vector<std::uint8_t> buffer(chunk_bytes);
  std::uint32_t sequence = 0;
  while (in) {
    in.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) {
      break;
    }
    tdl_app::AudioPcmChunk chunk;
    chunk.sample_rate = opt.sample_rate;
    chunk.channels = opt.channels;
    chunk.bit_depth = opt.bit_depth;
    chunk.sequence = ++sequence;
    chunk.data.assign(buffer.begin(), buffer.begin() + got);
    if (!audio.writeOutputChunk(chunk, error)) {
      goto done;
    }
  }

  ok = true;

done:
  audio.closeOutputStream();
  return ok;
}

bool runLoopback(const Options &opt, bool talk3a, std::string *error) {
  tdl_app::Audio audio;
  tdl_app::AudioSessionConfig config;
  config.io = makeIoConfig(opt);
  config.enable_input = true;
  config.enable_output = true;
  config.enable_encoder = false;
  config.enable_decoder = false;
  config.enable_talk_vqe = talk3a || opt.enable_talk_vqe;
  config.talk_vqe = tdl_app::AudioTalkVqeConfig::talk3a(opt.sample_rate);
  config.reference_output_device = opt.ao_device;
  config.reference_output_channel = opt.ao_channel;
  return audio.loopback(static_cast<double>(opt.seconds), config, error);
}

bool runEncode(const Options &opt, std::string *error) {
  tdl_app::Audio audio;
  tdl_app::AudioSessionConfig config;
  config.io = makeIoConfig(opt);
  config.enable_input = true;
  config.enable_output = false;
  config.enable_encoder = true;
  config.enable_decoder = false;
  config.enable_talk_vqe = opt.enable_talk_vqe;
  config.talk_vqe = tdl_app::AudioTalkVqeConfig::talk3a(opt.sample_rate);
  config.reference_output_device = opt.ao_device;
  config.reference_output_channel = opt.ao_channel;
  config.encoder = makeEncoderConfig(opt);
  if (!audio.openSession(config, error)) {
    return false;
  }

  bool ok = false;
  std::ofstream out(opt.output_path, std::ios::binary);
  if (!out) {
    if (error) *error = "failed to open encoded output file: " + opt.output_path;
    audio.closeSession();
    return false;
  }
  if (!writeEncodedHeader(&out, opt, error)) {
    audio.closeSession();
    return false;
  }

  const std::uint64_t target = targetSamples(opt);
  std::uint64_t captured = 0;
  while (target == 0 || captured < target) {
    tdl_app::AudioEncodedStream stream;
    if (!audio.readEncodedChunk(&stream, error)) {
      goto done;
    }
    writeU32(&out, static_cast<std::uint32_t>(stream.data.size()));
    if (!stream.data.empty()) {
      out.write(reinterpret_cast<const char *>(stream.data.data()), static_cast<std::streamsize>(stream.data.size()));
    }
    if (!out) {
      if (error) *error = "failed to write encoded output file: " + opt.output_path;
      goto done;
    }
    captured += static_cast<std::uint64_t>(opt.points_per_frame);
    if (target == 0 && stream.data.empty()) {
      break;
    }
  }

  ok = true;

done:
  audio.closeSession();
  return ok;
}

bool runDecode(const Options &cli_opt, std::string *error) {
  Options opt = cli_opt;
  std::ifstream in(opt.input_path, std::ios::binary);
  if (!in) {
    if (error) *error = "failed to open encoded input file: " + opt.input_path;
    return false;
  }
  if (!readEncodedHeader(&in, &opt, error)) {
    return false;
  }

  tdl_app::Audio audio;
  tdl_app::AudioSessionConfig config;
  config.io = makeIoConfig(opt);
  config.enable_input = false;
  config.enable_output = true;
  config.enable_encoder = false;
  config.enable_decoder = true;
  config.enable_talk_vqe = false;
  config.decoder = makeDecoderConfig(opt);
  if (!audio.openSession(config, error)) {
    return false;
  }

  bool ok = false;
  while (true) {
    std::uint32_t packet_size = 0;
    if (!readU32(&in, &packet_size)) {
      if (in.eof()) {
        ok = true;
      } else {
        if (error) *error = "encoded input file is truncated";
      }
      break;
    }
    tdl_app::AudioEncodedStream stream;
    stream.payload_type = parsePayload(opt.payload);
    stream.data.resize(packet_size);
    if (packet_size > 0) {
      in.read(reinterpret_cast<char *>(stream.data.data()), static_cast<std::streamsize>(packet_size));
      if (in.gcount() != static_cast<std::streamsize>(packet_size)) {
        if (error) *error = "encoded input file payload is truncated";
        break;
      }
    }
    if (!audio.writeDecodedChunk(stream, error)) {
      break;
    }
  }

  audio.closeSession();
  return ok;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::string error;
  bool ok = false;
  if (opt.mode == "status") {
    ok = runStatus(&error);
  } else if (opt.mode == "wav-record") {
    ok = runWavRecord(opt, &error);
  } else if (opt.mode == "wav-play") {
    ok = runWavPlay(opt, &error);
  } else if (opt.mode == "pcm-capture") {
    ok = runPcmCapture(opt, &error);
  } else if (opt.mode == "pcm-play") {
    ok = runPcmPlay(opt, &error);
  } else if (opt.mode == "loopback") {
    ok = runLoopback(opt, false, &error);
  } else if (opt.mode == "talk3a-loopback") {
    ok = runLoopback(opt, true, &error);
  } else if (opt.mode == "encode") {
    ok = runEncode(opt, &error);
  } else if (opt.mode == "decode") {
    ok = runDecode(opt, &error);
  } else {
    error = "unsupported mode: " + opt.mode;
  }

  if (!ok) {
    if (!error.empty()) {
      std::cerr << error << "\n";
    }
    return 1;
  }
  return 0;
}

