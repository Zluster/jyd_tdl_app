#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "tdl_app/audio_input.hpp"
#include "tdl_app/npu_asr_recognizer.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Options {
  std::string mode = "realtime";
  std::string pcm_path;
  std::string model_spec = "./configs/model_specs/npu_zipformer_zh_14m_asr.mud";
  std::string firmware;
  int max_seconds = 0;
  int ai_device = 0;
  int ai_channel = 0;
  int ai_card_id = -1;
  int ai_volume = 24;
  int points_per_frame = 160;
  int frame_count = 8;
  int frame_depth = 8;
  int timeout_ms = 1000;
  bool enable_vqe = false;
};

void usage() {
  std::cout
      << "Usage:\n"
      << "  tdl_npu_asr_demo --mode realtime [options]\n"
      << "  tdl_npu_asr_demo --mode pcm --pcm FILE [options]\n\n"
      << "Both modes use signed 16-bit, mono, 16 kHz PCM.\n"
      << "Realtime prints decoded text incrementally; Ctrl+C finishes the current utterance.\n\n"
      << "Options:\n"
      << "  --model-spec FILE    NPU Zipformer 39/320/5537 model spec\n"
      << "  --firmware FILE      BM Runtime firmware path\n"
      << "  --max-seconds N      Realtime stop limit, 0 means until Ctrl+C\n"
      << "  --ai-device N --ai-channel N --ai-card-id N --ai-volume N\n"
      << "  --points-per-frame N --frame-count N --frame-depth N --timeout-ms N\n"
      << "  --enable-vqe         Enable AI AGC/ANR before capture\n";
}

bool next(int argc, char **argv, int *index, const char *name, std::string *value) {
  if (*index + 1 >= argc) {
    std::cerr << name << " requires a value\n";
    return false;
  }
  *value = argv[++*index];
  return true;
}

bool nextInt(int argc, char **argv, int *index, const char *name, int *value) {
  std::string text;
  if (!next(argc, argv, index, name, &text)) return false;
  try {
    *value = std::stoi(text);
    return true;
  } catch (...) {
    std::cerr << name << " must be an integer\n";
    return false;
  }
}

bool parse(int argc, char **argv, Options *options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mode") {
      if (!next(argc, argv, &i, "--mode", &options->mode)) return false;
    } else if (arg == "--pcm") {
      if (!next(argc, argv, &i, "--pcm", &options->pcm_path)) return false;
    } else if (arg == "--model-spec") {
      if (!next(argc, argv, &i, "--model-spec", &options->model_spec)) return false;
    } else if (arg == "--firmware") {
      if (!next(argc, argv, &i, "--firmware", &options->firmware)) return false;
    } else if (arg == "--max-seconds") {
      if (!nextInt(argc, argv, &i, "--max-seconds", &options->max_seconds)) return false;
    } else if (arg == "--ai-device") {
      if (!nextInt(argc, argv, &i, "--ai-device", &options->ai_device)) return false;
    } else if (arg == "--ai-channel") {
      if (!nextInt(argc, argv, &i, "--ai-channel", &options->ai_channel)) return false;
    } else if (arg == "--ai-card-id") {
      if (!nextInt(argc, argv, &i, "--ai-card-id", &options->ai_card_id)) return false;
    } else if (arg == "--ai-volume") {
      if (!nextInt(argc, argv, &i, "--ai-volume", &options->ai_volume)) return false;
    } else if (arg == "--points-per-frame") {
      if (!nextInt(argc, argv, &i, "--points-per-frame", &options->points_per_frame)) return false;
    } else if (arg == "--frame-count") {
      if (!nextInt(argc, argv, &i, "--frame-count", &options->frame_count)) return false;
    } else if (arg == "--frame-depth") {
      if (!nextInt(argc, argv, &i, "--frame-depth", &options->frame_depth)) return false;
    } else if (arg == "--timeout-ms") {
      if (!nextInt(argc, argv, &i, "--timeout-ms", &options->timeout_ms)) return false;
    } else if (arg == "--enable-vqe") {
      options->enable_vqe = true;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  if ((options->mode != "pcm" && options->mode != "realtime") ||
      (options->mode == "pcm" && options->pcm_path.empty()) ||
      options->max_seconds < 0 || options->points_per_frame <= 0 ||
      options->frame_count <= 0 || options->frame_depth <= 0 || options->timeout_ms <= 0) {
    return false;
  }
  return true;
}

bool readPcm(const std::string &path, std::vector<std::int16_t> *samples) {
  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) return false;
  input.seekg(0, std::ios::end);
  const std::streamoff bytes = input.tellg();
  input.seekg(0, std::ios::beg);
  if (bytes <= 0 || bytes % static_cast<std::streamoff>(sizeof(std::int16_t)) != 0) return false;
  samples->resize(static_cast<std::size_t>(bytes) / sizeof(std::int16_t));
  return static_cast<bool>(input.read(reinterpret_cast<char *>(samples->data()), bytes));
}

bool frameToSamples(const tdl_app::AudioFrame &frame, std::vector<std::int16_t> *samples) {
  if (frame.channels.empty() || frame.channels.front().empty() ||
      frame.channels.front().size() % sizeof(std::int16_t) != 0) return false;
  const std::vector<std::uint8_t> &bytes = frame.channels.front();
  samples->resize(bytes.size() / sizeof(std::int16_t));
  std::memcpy(samples->data(), bytes.data(), bytes.size());
  return true;
}

bool append(tdl_app::NpuStreamingAsr *asr, const std::vector<std::int16_t> &samples,
            std::string *error) {
  std::string delta;
  if (!asr->acceptPcm(samples, &delta, error)) return false;
  if (!delta.empty()) std::cout << delta << std::flush;
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse(argc, argv, &options)) {
    usage();
    return 2;
  }
  tdl_app::NpuStreamingAsr asr;
  std::string error;
  if (!asr.load(tdl_app::ModelSessionConfig::fromSpec(options.model_spec, options.firmware),
                &error)) {
    std::cerr << "load failed: " << error << "\n";
    std::_Exit(3);
  }
  if (options.mode == "pcm") {
    std::vector<std::int16_t> samples;
    if (!readPcm(options.pcm_path, &samples) || !append(&asr, samples, &error)) {
      std::cerr << "PCM recognition failed: " << error << "\n";
      std::_Exit(4);
    }
  } else {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    tdl_app::AudioInput::Config audio_config = tdl_app::AudioInput::mono16k(
        options.ai_device, options.ai_channel, options.ai_card_id);
    audio_config.volume_step = options.ai_volume;
    audio_config.points_per_frame = options.points_per_frame;
    audio_config.frame_count = options.frame_count;
    audio_config.frame_depth = options.frame_depth;
    tdl_app::AudioInput audio(audio_config);
    if (!audio.open(&error) || (options.enable_vqe && !audio.enableVqe(&error))) {
      std::cerr << "audio initialization failed: " << error << "\n";
      audio.close();
      std::_Exit(5);
    }
    const std::size_t limit = static_cast<std::size_t>(options.max_seconds) * 16000;
    std::size_t captured = 0;
    std::cout << "listening: " << std::flush;
    while (!g_stop && (limit == 0 || captured < limit)) {
      tdl_app::AudioFrame frame;
      std::vector<std::int16_t> samples;
      if (!audio.readFrame(&frame, options.timeout_ms, &error) ||
          !frameToSamples(frame, &samples) || !append(&asr, samples, &error)) {
        std::cerr << "\nrealtime recognition failed: " << error << "\n";
        audio.close();
        std::_Exit(6);
      }
      captured += samples.size();
    }
    audio.close();
  }
  std::string tail;
  if (!asr.finish(&tail, &error)) {
    std::cerr << "finish failed: " << error << "\n";
    std::_Exit(7);
  }
  if (!tail.empty()) std::cout << tail;
  std::cout << "\ntext: " << asr.text() << "\n" << std::flush;

  // Current CV184X BM Runtime crashes while destroying a multi-bmodel ASR graph.
  // All output has been flushed and the process owns no externally shared state.
  std::_Exit(0);
}
