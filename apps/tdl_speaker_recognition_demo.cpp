#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "tdl_app/audio_input.hpp"
#include "tdl_app/speaker_recognizer.hpp"

namespace {

struct Options {
  std::string mode = "interactive";
  std::string model_spec = "./configs/model_specs/speaker_campplus_sv.mud";
  std::string firmware;
  std::string database = "./speakers.db";
  std::string label;
  std::string pcm_path;
  std::string dump_pcm;
  int seconds = 3;
  int min_speech_ms = 1500;
  int silence_ms = 700;
  int speech_start_ms = 100;
  int max_segment_ms = 5000;
  int max_events = 0;
  int ai_device = 0;
  int ai_channel = 0;
  int ai_card_id = -1;
  int ai_volume = 24;
  int points_per_frame = 160;
  int frame_count = 8;
  int frame_depth = 8;
  int timeout_ms = 1000;
  float threshold = 0.60f;
  float energy_threshold = 350.0f;
  bool enable_vqe = false;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_speaker_recognition_demo                     # interactive menu\n"
      << "  tdl_speaker_recognition_demo --mode register --label NAME [options]\n"
      << "  tdl_speaker_recognition_demo --mode identify [options]\n"
      << "  tdl_speaker_recognition_demo --mode verify --label NAME [options]\n"
      << "  tdl_speaker_recognition_demo --mode monitor [options]\n\n"
      << "Register, identify, and verify capture signed 16-bit, mono, 16 kHz PCM\n"
      << "from the AI device unless --pcm is provided.\n\n"
      << "Core options:\n"
      << "  --model-spec FILE       CAMPPlus BF16 model spec\n"
      << "  --firmware FILE         BM runtime firmware library\n"
      << "  --database FILE         Persistent enrollment database\n"
      << "  --label NAME            Required for register and verify\n"
      << "  --pcm FILE              Offline 16 kHz mono signed 16-bit PCM input\n"
      << "  --seconds N             Capture duration for register/identify/verify (default 3)\n"
      << "  --threshold F           Cosine match threshold (default 0.60)\n"
      << "  --dump-pcm FILE         Save captured PCM for diagnosis\n\n"
      << "Monitor options:\n"
      << "  --energy-threshold F    RMS threshold for speech gate (default 350)\n"
      << "  --speech-start-ms N     Consecutive speech before a segment begins (default 100)\n"
      << "  --min-speech-ms N       Minimum segment duration to identify (default 1500)\n"
      << "  --silence-ms N          Silence that finishes a segment (default 700)\n"
      << "  --max-segment-ms N      Maximum segment duration (default 5000)\n"
      << "  --max-events N          Stop after N recognized segments, 0 means forever\n\n"
      << "Audio options:\n"
      << "  --ai-device N --ai-channel N --ai-card-id N --ai-volume N\n"
      << "  --points-per-frame N --frame-count N --frame-depth N --timeout-ms N\n"
      << "  --enable-vqe            Enable AI AGC/ANR before capture\n";
}

bool parseInt(const std::string &text, int *value) {
  try {
    *value = std::stoi(text);
    return true;
  } catch (...) {
    return false;
  }
}

bool parseFloat(const std::string &text, float *value) {
  try {
    *value = std::stof(text);
    return true;
  } catch (...) {
    return false;
  }
}

bool nextString(int argc, char **argv, int *index, const char *name,
                std::string *value) {
  if (*index + 1 >= argc) {
    std::cerr << name << " requires a value\n";
    return false;
  }
  *value = argv[++*index];
  return true;
}

bool nextInt(int argc, char **argv, int *index, const char *name, int *value) {
  std::string text;
  return nextString(argc, argv, index, name, &text) && parseInt(text, value);
}

bool nextFloat(int argc, char **argv, int *index, const char *name, float *value) {
  std::string text;
  return nextString(argc, argv, index, name, &text) && parseFloat(text, value);
}

bool parseArgs(int argc, char **argv, Options *options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mode") {
      if (!nextString(argc, argv, &i, "--mode", &options->mode)) return false;
    } else if (arg == "--model-spec") {
      if (!nextString(argc, argv, &i, "--model-spec", &options->model_spec)) return false;
    } else if (arg == "--firmware") {
      if (!nextString(argc, argv, &i, "--firmware", &options->firmware)) return false;
    } else if (arg == "--database") {
      if (!nextString(argc, argv, &i, "--database", &options->database)) return false;
    } else if (arg == "--label") {
      if (!nextString(argc, argv, &i, "--label", &options->label)) return false;
    } else if (arg == "--pcm") {
      if (!nextString(argc, argv, &i, "--pcm", &options->pcm_path)) return false;
    } else if (arg == "--dump-pcm") {
      if (!nextString(argc, argv, &i, "--dump-pcm", &options->dump_pcm)) return false;
    } else if (arg == "--seconds") {
      if (!nextInt(argc, argv, &i, "--seconds", &options->seconds)) return false;
    } else if (arg == "--threshold") {
      if (!nextFloat(argc, argv, &i, "--threshold", &options->threshold)) return false;
    } else if (arg == "--energy-threshold") {
      if (!nextFloat(argc, argv, &i, "--energy-threshold", &options->energy_threshold)) return false;
    } else if (arg == "--min-speech-ms") {
      if (!nextInt(argc, argv, &i, "--min-speech-ms", &options->min_speech_ms)) return false;
    } else if (arg == "--silence-ms") {
      if (!nextInt(argc, argv, &i, "--silence-ms", &options->silence_ms)) return false;
    } else if (arg == "--speech-start-ms") {
      if (!nextInt(argc, argv, &i, "--speech-start-ms", &options->speech_start_ms)) return false;
    } else if (arg == "--max-segment-ms") {
      if (!nextInt(argc, argv, &i, "--max-segment-ms", &options->max_segment_ms)) return false;
    } else if (arg == "--max-events") {
      if (!nextInt(argc, argv, &i, "--max-events", &options->max_events)) return false;
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
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }
  if (options->mode != "interactive" && options->mode != "register" &&
      options->mode != "identify" && options->mode != "verify" &&
      options->mode != "monitor") {
    std::cerr << "--mode must be interactive, register, identify, verify, or monitor\n";
    return false;
  }
  if ((options->mode == "register" || options->mode == "verify") &&
      options->label.empty()) {
    std::cerr << "--label is required for " << options->mode << " mode\n";
    return false;
  }
  if (!options->pcm_path.empty() &&
      (options->mode == "interactive" || options->mode == "monitor")) {
    std::cerr << "--pcm is supported only by register, identify, and verify modes\n";
    return false;
  }
  if (options->seconds < 1 || options->threshold < -1.0f ||
      options->threshold > 1.0f || options->energy_threshold <= 0.0f ||
      options->min_speech_ms < 1000 || options->silence_ms < 100 ||
      options->speech_start_ms < 10 || options->max_segment_ms < options->min_speech_ms ||
      options->max_events < 0 || options->points_per_frame <= 0 ||
      options->frame_count <= 0 || options->frame_depth <= 0 ||
      options->timeout_ms <= 0) {
    std::cerr << "invalid numeric option\n";
    return false;
  }
  return true;
}

tdl_app::AudioInput::Config createAudioConfig(const Options &options) {
  tdl_app::AudioInput::Config config = tdl_app::AudioInput::mono16k(
      options.ai_device, options.ai_channel, options.ai_card_id);
  config.volume_step = options.ai_volume;
  config.points_per_frame = options.points_per_frame;
  config.frame_count = options.frame_count;
  config.frame_depth = options.frame_depth;
  return config;
}

bool appendFrame(const tdl_app::AudioFrame &frame, std::vector<std::int16_t> *samples,
                 std::string *error) {
  if (!samples || frame.channels.empty() || frame.channels.front().empty() ||
      frame.channels.front().size() % sizeof(std::int16_t) != 0) {
    if (error) *error = "audio input did not return 16-bit mono PCM";
    return false;
  }
  const std::vector<std::uint8_t> &bytes = frame.channels.front();
  const std::size_t old_size = samples->size();
  samples->resize(old_size + bytes.size() / sizeof(std::int16_t));
  std::memcpy(samples->data() + old_size, bytes.data(), bytes.size());
  return true;
}

float rms(const std::vector<std::int16_t> &samples) {
  if (samples.empty()) return 0.0f;
  double power = 0.0;
  for (std::int16_t sample : samples) {
    const double value = sample;
    power += value * value;
  }
  return static_cast<float>(std::sqrt(power / samples.size()));
}

bool captureSeconds(tdl_app::AudioInput *audio, const Options &options,
                    std::vector<std::int16_t> *samples, std::string *error) {
  const std::size_t wanted = static_cast<std::size_t>(options.seconds) * 16000;
  samples->clear();
  samples->reserve(wanted + 1600);
  while (samples->size() < wanted) {
    tdl_app::AudioFrame frame;
    if (!audio->readFrame(&frame, options.timeout_ms, error) ||
        !appendFrame(frame, samples, error)) {
      return false;
    }
  }
  samples->resize(wanted);
  return true;
}

bool dumpPcm(const std::string &path, const std::vector<std::int16_t> &samples,
             std::string *error) {
  if (path.empty()) return true;
  std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
  if (!output) {
    if (error) *error = "cannot create PCM dump: " + path;
    return false;
  }
  output.write(reinterpret_cast<const char *>(samples.data()),
               samples.size() * sizeof(std::int16_t));
  if (!output) {
    if (error) *error = "cannot write PCM dump: " + path;
    return false;
  }
  return true;
}

bool readPcm(const std::string &path, std::vector<std::int16_t> *samples,
             std::string *error) {
  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) {
    if (error) *error = "cannot open PCM input: " + path;
    return false;
  }
  input.seekg(0, std::ios::end);
  const std::streamoff bytes = input.tellg();
  input.seekg(0, std::ios::beg);
  if (bytes <= 0 || bytes % static_cast<std::streamoff>(sizeof(std::int16_t)) != 0) {
    if (error) *error = "PCM input must contain whole signed 16-bit samples: " + path;
    return false;
  }
  samples->resize(static_cast<std::size_t>(bytes) / sizeof(std::int16_t));
  if (!input.read(reinterpret_cast<char *>(samples->data()), bytes)) {
    if (error) *error = "cannot read PCM input: " + path;
    return false;
  }
  return true;
}

bool loadDatabase(const Options &options, bool allow_missing,
                  tdl_app::SpeakerDatabase *database, std::string *error) {
  std::ifstream file(options.database.c_str(), std::ios::binary);
  if (!file) {
    if (allow_missing) return true;
    if (error) *error = "speaker database does not exist: " + options.database;
    return false;
  }
  return database->load(options.database, error);
}

void printMatch(const tdl_app::SpeakerMatch &match, const char *prefix) {
  std::cout << prefix << " label="
            << (match.label.empty() ? "unknown" : match.label)
            << " score=" << match.score
            << " matched=" << (match.matched ? 1 : 0)
            << " door_action=" << (match.matched ? "OPEN" : "KEEP_CLOSED")
            << "\n";
}

bool recognizeSegment(tdl_app::SpeakerRecognizer *recognizer,
                      const tdl_app::SpeakerDatabase &database,
                      const std::vector<std::int16_t> &samples,
                      const Options &options) {
  tdl_app::SpeakerEmbedding embedding;
  std::string error;
  if (!recognizer->extract(samples, &embedding, &error)) {
    std::cerr << "segment extraction failed: " << error << "\n";
    return false;
  }
  printMatch(recognizer->identify(embedding, database, options.threshold), "event");
  return true;
}

bool registerSpeaker(tdl_app::AudioInput *audio,
                     tdl_app::SpeakerRecognizer *recognizer,
                     tdl_app::SpeakerDatabase *database,
                     const std::string &label, const Options &options) {
  std::vector<std::int16_t> samples;
  std::string error;
  std::cout << "recording " << options.seconds << " seconds for " << label
            << ", please speak now\n";
  if (!captureSeconds(audio, options, &samples, &error) ||
      !dumpPcm(options.dump_pcm, samples, &error)) {
    std::cerr << "audio capture failed: " << error << "\n";
    return false;
  }
  tdl_app::SpeakerEmbedding embedding;
  if (!recognizer->extract(samples, &embedding, &error) ||
      !database->upsert(label, embedding, &error) ||
      !database->save(options.database, &error)) {
    std::cerr << "speaker registration failed: " << error << "\n";
    return false;
  }
  std::cout << "registered label=" << label
            << " database=" << options.database
            << " enrolled=" << database->size()
            << " embedding_dim=" << embedding.values.size() << "\n";
  return true;
}

int runMonitor(tdl_app::AudioInput *audio, tdl_app::SpeakerRecognizer *recognizer,
               const tdl_app::SpeakerDatabase &database, const Options &options) {
  std::vector<std::int16_t> segment;
  std::vector<std::int16_t> pending;
  int speech_ms = 0;
  int silence_ms = 0;
  int events = 0;
  bool collecting = false;
  std::cout << "monitor started: enrolled=" << database.size()
            << " threshold=" << options.threshold
            << " energy_threshold=" << options.energy_threshold << "\n";
  for (;;) {
    tdl_app::AudioFrame frame;
    std::string error;
    if (!audio->readFrame(&frame, options.timeout_ms, &error)) {
      std::cerr << "audio capture failed: " << error << "\n";
      return 1;
    }
    std::vector<std::int16_t> frame_samples;
    if (!appendFrame(frame, &frame_samples, &error)) {
      std::cerr << "audio frame failed: " << error << "\n";
      return 1;
    }
    const int frame_ms = std::max(1, static_cast<int>(frame_samples.size() * 1000 / 16000));
    const bool speech = rms(frame_samples) >= options.energy_threshold;
    if (!collecting) {
      if (speech) {
        pending.insert(pending.end(), frame_samples.begin(), frame_samples.end());
        speech_ms += frame_ms;
        if (speech_ms >= options.speech_start_ms) {
          collecting = true;
          segment.swap(pending);
          silence_ms = 0;
        }
      } else {
        pending.clear();
        speech_ms = 0;
      }
      continue;
    }
    segment.insert(segment.end(), frame_samples.begin(), frame_samples.end());
    if (speech) {
      silence_ms = 0;
    } else {
      silence_ms += frame_ms;
    }
    const int segment_ms = static_cast<int>(segment.size() * 1000 / 16000);
    if (silence_ms < options.silence_ms && segment_ms < options.max_segment_ms) {
      continue;
    }
    if (segment_ms >= options.min_speech_ms) {
      recognizeSegment(recognizer, database, segment, options);
      ++events;
      if (options.max_events > 0 && events >= options.max_events) {
        return 0;
      }
    } else {
      std::cout << "segment discarded: duration_ms=" << segment_ms << "\n";
    }
    segment.clear();
    pending.clear();
    speech_ms = 0;
    silence_ms = 0;
    collecting = false;
  }
}

int runInteractive(tdl_app::AudioInput *audio,
                   tdl_app::SpeakerRecognizer *recognizer,
                   tdl_app::SpeakerDatabase *database,
                   const Options &options) {
  for (;;) {
    std::cout << "\n1. register speaker\n2. real-time identify\nq. quit\nselect: ";
    std::string selection;
    if (!std::getline(std::cin, selection) || selection == "q" || selection == "Q") {
      return 0;
    }
    if (selection == "1") {
      std::cout << "speaker name: ";
      std::string label;
      if (!std::getline(std::cin, label) || label.empty()) {
        std::cerr << "speaker name cannot be empty\n";
        continue;
      }
      registerSpeaker(audio, recognizer, database, label, options);
      continue;
    }
    if (selection == "2") {
      std::cout << "real-time identification started; press Ctrl+C to stop\n";
      return runMonitor(audio, recognizer, *database, options);
    }
    std::cerr << "invalid selection\n";
  }
}

}  // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parseArgs(argc, argv, &options)) {
    printUsage();
    return 2;
  }

  tdl_app::SpeakerRecognizer recognizer;
  std::string error;
  if (!recognizer.load(
          tdl_app::ModelSessionConfig::fromSpec(options.model_spec, options.firmware),
          &error)) {
    std::cerr << "model load failed: " << error << "\n";
    return 1;
  }
  tdl_app::SpeakerDatabase database;
  if (!loadDatabase(options, options.mode == "register" ||
                                 options.mode == "interactive",
                    &database, &error)) {
    std::cerr << "database load failed: " << error << "\n";
    return 1;
  }

  std::vector<std::int16_t> samples;
  if (!options.pcm_path.empty()) {
    if (!readPcm(options.pcm_path, &samples, &error) ||
        !dumpPcm(options.dump_pcm, samples, &error)) {
      std::cerr << "PCM input failed: " << error << "\n";
      return 1;
    }
  } else {
    tdl_app::AudioInput audio(createAudioConfig(options));
    if (!audio.open(&error)) {
      std::cerr << "audio open failed: " << error << "\n";
      return 1;
    }
    if (options.enable_vqe &&
        (!audio.configureTalkVqe(tdl_app::AudioTalkVqeConfig::agcAnr(), 0, 0, &error) ||
         !audio.enableVqe(&error))) {
      std::cerr << "audio VQE enable failed: " << error << "\n";
      return 1;
    }
    if (options.mode == "interactive") {
      return runInteractive(&audio, &recognizer, &database, options);
    }
    if (options.mode == "monitor") {
      return runMonitor(&audio, &recognizer, database, options);
    }
    if (!captureSeconds(&audio, options, &samples, &error) ||
        !dumpPcm(options.dump_pcm, samples, &error)) {
      std::cerr << "audio capture failed: " << error << "\n";
      return 1;
    }
  }
  tdl_app::SpeakerEmbedding embedding;
  if (!recognizer.extract(samples, &embedding, &error)) {
    std::cerr << "speaker extraction failed: " << error << "\n";
    return 1;
  }

  if (options.mode == "register") {
    if (!database.upsert(options.label, embedding, &error) ||
        !database.save(options.database, &error)) {
      std::cerr << "speaker registration failed: " << error << "\n";
      return 1;
    }
    std::cout << "registered label=" << options.label
              << " database=" << options.database
              << " enrolled=" << database.size()
              << " embedding_dim=" << embedding.values.size() << "\n";
    return 0;
  }
  if (options.mode == "verify") {
    printMatch(recognizer.verify(options.label, embedding, database, options.threshold),
               "verify");
  } else {
    printMatch(recognizer.identify(embedding, database, options.threshold),
               "identify");
  }
  return 0;
}
