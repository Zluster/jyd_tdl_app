#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "tdl_app/audio_input.hpp"
#include "tdl_app/npu_keyword_spotter.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Options {
  std::string mode = "interactive";
  std::string text;
  std::string pinyin;
  std::string pcm_path;
  std::string dump_pcm_path;
  std::string model_spec = "./configs/model_specs/npu_zipformer_zh_kws.mud";
  std::string keyword_file = "./kws_keywords.txt";
  std::string registry = "./kws_registry.json";
  std::string registry_tool = "./tools/kws_keyword_registry.py";
  std::string firmware;
  int max_seconds = 0;
  int ai_device = 0;
  int ai_channel = 0;
  int ai_card_id = -1;
  int ai_volume = 24;
  int points_per_frame = 1600;
  int frame_count = 8;
  int frame_depth = 8;
  int timeout_ms = 1000;
  float input_gain = 1.0f;
  float keyword_score = 0.0f;
  float keyword_threshold = 0.0f;
  bool enable_vqe = false;
};

void usage() {
  std::cout
      << "Usage:\n"
      << "  tdl_npu_keyword_spotter_demo                    # interactive menu\n"
      << "  tdl_npu_keyword_spotter_demo --mode add --text Chinese [--pinyin ni3 hao3]\n"
      << "      [--score F] [--threshold F]\n"
      << "  tdl_npu_keyword_spotter_demo --mode remove --text Chinese\n"
      << "  tdl_npu_keyword_spotter_demo --mode list\n"
      << "  tdl_npu_keyword_spotter_demo --mode realtime [options]\n"
      << "  tdl_npu_keyword_spotter_demo --mode pcm --pcm FILE [options]\n\n"
      << "Keywords accept Chinese text. Pinyin is optional and only needed to override\n"
      << "a polyphone. Realtime input is signed 16-bit, mono, 16 kHz PCM from AI.\n"
      << "  --dump-pcm FILE        Save realtime microphone PCM for diagnosis\n"
      << "  --input-gain F         Fixed realtime PCM gain (default 1.0)\n"
      << "  --enable-vqe           Enable stable ANR; dynamic AGC is intentionally disabled\n"
      << "  --points-per-frame N   AI samples per frame (default 1600, 100 ms)\n";
}

bool next(int argc, char **argv, int *index, const char *name, std::string *value) {
  if (*index + 1 >= argc) { std::cerr << name << " requires a value\n"; return false; }
  *value = argv[++*index];
  return true;
}

bool nextInt(int argc, char **argv, int *index, const char *name, int *value) {
  std::string text;
  if (!next(argc, argv, index, name, &text)) return false;
  try { *value = std::stoi(text); return true; } catch (...) { return false; }
}

bool nextFloat(int argc, char **argv, int *index, const char *name, float *value) {
  std::string text;
  if (!next(argc, argv, index, name, &text)) return false;
  try { *value = std::stof(text); return true; } catch (...) { return false; }
}

bool parse(int argc, char **argv, Options *options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--mode") { if (!next(argc, argv, &i, "--mode", &options->mode)) return false; }
    else if (arg == "--text") { if (!next(argc, argv, &i, "--text", &options->text)) return false; }
    else if (arg == "--pinyin") { if (!next(argc, argv, &i, "--pinyin", &options->pinyin)) return false; }
    else if (arg == "--pcm") { if (!next(argc, argv, &i, "--pcm", &options->pcm_path)) return false; }
    else if (arg == "--dump-pcm") { if (!next(argc, argv, &i, "--dump-pcm", &options->dump_pcm_path)) return false; }
    else if (arg == "--model-spec") { if (!next(argc, argv, &i, "--model-spec", &options->model_spec)) return false; }
    else if (arg == "--keywords") { if (!next(argc, argv, &i, "--keywords", &options->keyword_file)) return false; }
    else if (arg == "--registry") { if (!next(argc, argv, &i, "--registry", &options->registry)) return false; }
    else if (arg == "--registry-tool") { if (!next(argc, argv, &i, "--registry-tool", &options->registry_tool)) return false; }
    else if (arg == "--firmware") { if (!next(argc, argv, &i, "--firmware", &options->firmware)) return false; }
    else if (arg == "--max-seconds") { if (!nextInt(argc, argv, &i, "--max-seconds", &options->max_seconds)) return false; }
    else if (arg == "--ai-device") { if (!nextInt(argc, argv, &i, "--ai-device", &options->ai_device)) return false; }
    else if (arg == "--ai-channel") { if (!nextInt(argc, argv, &i, "--ai-channel", &options->ai_channel)) return false; }
    else if (arg == "--ai-card-id") { if (!nextInt(argc, argv, &i, "--ai-card-id", &options->ai_card_id)) return false; }
    else if (arg == "--ai-volume") { if (!nextInt(argc, argv, &i, "--ai-volume", &options->ai_volume)) return false; }
    else if (arg == "--points-per-frame") { if (!nextInt(argc, argv, &i, "--points-per-frame", &options->points_per_frame)) return false; }
    else if (arg == "--frame-count") { if (!nextInt(argc, argv, &i, "--frame-count", &options->frame_count)) return false; }
    else if (arg == "--frame-depth") { if (!nextInt(argc, argv, &i, "--frame-depth", &options->frame_depth)) return false; }
    else if (arg == "--timeout-ms") { if (!nextInt(argc, argv, &i, "--timeout-ms", &options->timeout_ms)) return false; }
    else if (arg == "--input-gain") { if (!nextFloat(argc, argv, &i, "--input-gain", &options->input_gain)) return false; }
    else if (arg == "--score") { if (!nextFloat(argc, argv, &i, "--score", &options->keyword_score)) return false; }
    else if (arg == "--threshold") { if (!nextFloat(argc, argv, &i, "--threshold", &options->keyword_threshold)) return false; }
    else if (arg == "--enable-vqe") options->enable_vqe = true;
    else if (arg == "-h" || arg == "--help") { usage(); std::exit(0); }
    else { std::cerr << "unknown argument: " << arg << "\n"; return false; }
  }
  const bool valid_mode = options->mode == "interactive" || options->mode == "add" ||
      options->mode == "remove" || options->mode == "list" || options->mode == "realtime" ||
      options->mode == "pcm";
  return valid_mode && !options->model_spec.empty() && options->max_seconds >= 0 &&
      options->points_per_frame > 0 && options->frame_count > 0 && options->frame_depth > 0 &&
      options->timeout_ms > 0 &&
      options->input_gain > 0.0f && options->input_gain <= 16.0f &&
      options->keyword_score >= 0.0f && options->keyword_threshold >= 0.0f &&
      options->keyword_threshold <= 1.0f &&
      ((options->mode != "add" && options->mode != "remove") || !options->text.empty()) &&
      (options->mode != "pcm" || !options->pcm_path.empty());
}

bool shellText(const std::string &text) {
  if (text.empty()) return false;
  for (unsigned char c : text) {
    if (c < 0x80 && !(c == ' ' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == ':')) return false;
  }
  return true;
}

bool updateRegistry(const Options &options, const std::string &command, const std::string &text,
                    const std::string &pinyin) {
  if (!shellText(text) || (!pinyin.empty() && !shellText(pinyin))) {
    std::cerr << "keyword text or pinyin contains unsupported characters\n";
    return false;
  }
  const std::string call = "PYTHONPATH=./tools python3 " + options.registry_tool +
      " --tokens ./models/cv184x/kws_tokens.txt --registry " + options.registry +
      " --keywords " + options.keyword_file + " " + command + " --text '" + text + "'" +
      (pinyin.empty() ? "" : " --pinyin '" + pinyin + "'") +
      (options.keyword_score > 0.0f ? " --score " + std::to_string(options.keyword_score) : "") +
      (options.keyword_threshold > 0.0f ? " --threshold " + std::to_string(options.keyword_threshold) : "");
  return std::system(call.c_str()) == 0;
}

bool readPcm(const std::string &path, std::vector<std::int16_t> *samples) {
  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) return false;
  input.seekg(0, std::ios::end);
  const std::streamoff bytes = input.tellg();
  input.seekg(0, std::ios::beg);
  if (bytes <= 0 || bytes % static_cast<std::streamoff>(sizeof(std::int16_t))) return false;
  samples->resize(static_cast<std::size_t>(bytes) / sizeof(std::int16_t));
  return static_cast<bool>(input.read(reinterpret_cast<char *>(samples->data()), bytes));
}

bool frameSamples(const tdl_app::AudioFrame &frame, std::vector<std::int16_t> *samples) {
  if (frame.channels.empty() || frame.channels[0].empty() ||
      frame.channels[0].size() % sizeof(std::int16_t)) return false;
  const std::vector<std::uint8_t> &bytes = frame.channels[0];
  samples->resize(bytes.size() / sizeof(std::int16_t));
  std::memcpy(samples->data(), bytes.data(), bytes.size());
  return true;
}

void applyFixedGain(float gain, std::vector<std::int16_t> *samples) {
  if (gain == 1.0f) return;
  for (std::int16_t &sample : *samples) {
    const int scaled = static_cast<int>(static_cast<float>(sample) * gain);
    sample = static_cast<std::int16_t>(
        std::max(-32768, std::min(32767, scaled)));
  }
}

bool initializeSpotter(const Options &options, tdl_app::NpuKeywordSpotter *spotter,
                       std::string *error) {
  const tdl_app::ModelSessionConfig config =
      tdl_app::ModelSessionConfig::fromSpec(options.model_spec, options.firmware);
  return spotter->load(config, error) &&
         spotter->loadKeywords(options.keyword_file, error);
}

void emitEvents(const std::vector<tdl_app::KeywordEvent> &events) {
  for (const tdl_app::KeywordEvent &event : events) {
    std::printf("event keyword=%s probability=%.2f%% action=TRIGGER\n",
                event.text.c_str(), event.confidence * 100.0f);
  }
  if (!events.empty()) std::fflush(stdout);
}

void emitScores(const std::vector<tdl_app::KeywordScore> &scores) {
  if (scores.empty()) return;
  for (std::size_t i = 0; i < scores.size(); ++i) {
    std::printf("kw%zu[%s]: %.3f;\t", i, scores[i].text.c_str(),
                scores[i].probability);
  }
  std::printf("\n");
  std::fflush(stdout);
}

bool processPcm(tdl_app::NpuKeywordSpotter *spotter,
                const std::vector<std::int16_t> &samples, std::string *error) {
  std::vector<tdl_app::KeywordEvent> events;
  std::vector<tdl_app::KeywordScore> scores;
  if (!spotter->acceptPcm(samples, &events, &scores, error)) return false;
  emitScores(scores);
  emitEvents(events);
  return true;
}

bool processPcmStreaming(tdl_app::NpuKeywordSpotter *spotter,
                         const std::vector<std::int16_t> &samples,
                         std::size_t chunk_samples, std::string *error) {
  for (std::size_t offset = 0; offset < samples.size(); offset += chunk_samples) {
    const std::size_t count = std::min(chunk_samples, samples.size() - offset);
    const std::vector<std::int16_t> chunk(samples.begin() + offset,
                                          samples.begin() + offset + count);
    if (!processPcm(spotter, chunk, error)) return false;
  }
  return true;
}

int realtime(const Options &options) {
  std::string error;
  tdl_app::NpuKeywordSpotter spotter;
  if (!initializeSpotter(options, &spotter, &error)) {
    std::cerr << "KWS initialization failed: " << error << "\n";
    return 1;
  }
  tdl_app::AudioInput::Config audio_config = tdl_app::AudioInput::mono16k(
      options.ai_device, options.ai_channel, options.ai_card_id);
  audio_config.volume_step = options.ai_volume;
  audio_config.points_per_frame = options.points_per_frame;
  audio_config.frame_count = options.frame_count;
  audio_config.frame_depth = options.frame_depth;
  tdl_app::AudioInput audio(audio_config);
  if (!audio.open(&error)) {
    std::cerr << "audio open failed: " << error << "\n";
    return 1;
  }
  if (options.enable_vqe) {
    tdl_app::AudioTalkVqeConfig vqe = tdl_app::AudioTalkVqeConfig::agcAnr();
    vqe.open_mask = 0x4;  // ANR only; dynamic AGC degraded repeated KWS utterances.
    if (!audio.configureTalkVqe(vqe, 0, 0, &error) ||
        !audio.enableVqe(&error)) {
      std::cerr << "audio VQE enable failed: " << error << "\n";
      return 1;
    }
  }
  g_stop = 0;
  std::signal(SIGINT, onSignal);
  std::ofstream dump_pcm;
  if (!options.dump_pcm_path.empty()) {
    dump_pcm.open(options.dump_pcm_path.c_str(),
                  std::ios::binary | std::ios::trunc);
    if (!dump_pcm) {
      std::cerr << "cannot create PCM dump: " << options.dump_pcm_path << "\n";
      return 1;
    }
  }
  std::cout << "realtime KWS started: input_gain=" << options.input_gain
            << " vqe=" << (options.enable_vqe ? "ANR" : "off")
            << "; press Ctrl+C to stop\n";
  int elapsed_ms = 0;
  while (!g_stop && (options.max_seconds == 0 || elapsed_ms < options.max_seconds * 1000)) {
    tdl_app::AudioFrame frame;
    if (!audio.readFrame(&frame, options.timeout_ms, &error)) { std::cerr << "audio read failed: " << error << "\n"; return 1; }
    std::vector<std::int16_t> samples;
    if (!frameSamples(frame, &samples)) { std::cerr << "audio frame is not 16-bit mono PCM\n"; return 1; }
    applyFixedGain(options.input_gain, &samples);
    if (dump_pcm.is_open()) {
      dump_pcm.write(reinterpret_cast<const char *>(samples.data()),
                     static_cast<std::streamsize>(samples.size() * sizeof(samples[0])));
      if (!dump_pcm) {
        std::cerr << "failed writing PCM dump: " << options.dump_pcm_path << "\n";
        return 1;
      }
    }
    if (!processPcm(&spotter, samples, &error)) {
      std::cerr << "KWS decode failed: " << error << "\n";
      return 1;
    }
    elapsed_ms += static_cast<int>(samples.size() * 1000 / 16000);
  }
  std::vector<tdl_app::KeywordEvent> tail_events;
  if (!spotter.finish(&tail_events, &error)) {
    std::cerr << "KWS finalize failed: " << error << "\n";
    return 1;
  }
  emitEvents(tail_events);
  return 0;
}

int interactive(Options options) {
  for (;;) {
    std::cout << "\n1. register keyword\n2. real-time identify\n3. list keywords\nq. quit\nselect: ";
    std::string input;
    if (!std::getline(std::cin, input) || input == "q" || input == "Q") return 0;
    if (input == "1") {
      std::cout << "keyword text: ";
      std::getline(std::cin, options.text);
      std::cout << "pinyin override (optional): ";
      std::getline(std::cin, options.pinyin);
      updateRegistry(options, "add", options.text, options.pinyin);
    } else if (input == "2") {
      realtime(options);
    } else if (input == "3") {
      std::system(("PYTHONPATH=./tools python3 " + options.registry_tool + " --tokens ./models/cv184x/kws_tokens.txt --registry " + options.registry + " --keywords " + options.keyword_file + " list").c_str());
    }
  }
}

}  // namespace

int main(int argc, char **argv) {
  Options options;
  if (!parse(argc, argv, &options)) { usage(); return 2; }
  if (options.mode == "interactive") return interactive(options);
  if (options.mode == "add" || options.mode == "remove") return updateRegistry(options, options.mode, options.text, options.pinyin) ? 0 : 1;
  if (options.mode == "list") return std::system(("PYTHONPATH=./tools python3 " + options.registry_tool + " --tokens ./models/cv184x/kws_tokens.txt --registry " + options.registry + " --keywords " + options.keyword_file + " list").c_str()) == 0 ? 0 : 1;
  if (options.mode == "realtime") return realtime(options);
  std::vector<std::int16_t> samples;
  if (!readPcm(options.pcm_path, &samples)) { std::cerr << "cannot read PCM: " << options.pcm_path << "\n"; return 1; }
  std::string error;
  tdl_app::NpuKeywordSpotter spotter;
  if (!initializeSpotter(options, &spotter, &error)) {
    std::cerr << "KWS initialization failed: " << error << "\n";
    return 1;
  }
  if (!processPcmStreaming(&spotter, samples, 1600, &error)) {
    std::cerr << "KWS decode failed: " << error << "\n";
    return 1;
  }
  std::vector<tdl_app::KeywordEvent> tail_events;
  if (!spotter.finish(&tail_events, &error)) {
    std::cerr << "KWS finalize failed: " << error << "\n";
    return 1;
  }
  emitEvents(tail_events);
  return 0;
}
