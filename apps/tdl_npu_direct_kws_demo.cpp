#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "tdl_app/audio_input.hpp"
#include "tdl_app/direct_keyword_spotter.hpp"

namespace {

constexpr int kSampleRate = 16000;
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Options {
  std::string model_spec = "./configs/model_specs/npu_zipformer_zh_kws.mud";
  std::string keywords = "./configs/kws_keywords.default.txt";
  std::string pcm_path;
  std::string dump_pcm_path;
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
  float threshold = -1.0f;
  int beam_width = 6;
  bool enable_vqe = false;
  bool print_scores = false;
};

void usage() {
  std::cout << "Usage: tdl_npu_direct_kws_demo [options]\\n"
            << "  CPU Fbank + direct CV184X BF16 encoder/decoder/joiner RNNT KWS.\\n"
            << "  --model-spec FILE  KWS model spec\\n"
            << "  --keywords FILE    keyword registry\\n"
            << "  --pcm FILE         Signed 16-bit, mono, 16 kHz PCM input\\n"
            << "  --dump-pcm FILE    Save realtime Signed 16-bit, mono, 16 kHz PCM\\n"
            << "  --threshold N      Override all keyword confidence gates (0..1)\\n"
            << "  --beam-width N     RNNT beam width, 1..8 (default: 6)\\n"
            << "  --print-scores     Print every registered keyword score for tuning\\n"
            << "  --max-seconds N    stop after N seconds, 0 means Ctrl+C\\n"
            << "  --firmware FILE    BM Runtime firmware\\n"
            << "  --ai-device N --ai-channel N --ai-card-id N --ai-volume N\\n"
            << "  --points-per-frame N --frame-count N --frame-depth N --timeout-ms N\\n"
            << "  --enable-vqe\\n";
}

bool next(int argc, char **argv, int *index, const char *name, std::string *value) {
  if (*index + 1 >= argc) { std::cerr << name << " requires a value\\n"; return false; }
  *value = argv[++*index]; return true;
}
bool nextInt(int argc, char **argv, int *index, const char *name, int *value) {
  std::string text; if (!next(argc, argv, index, name, &text)) return false;
  try { *value = std::stoi(text); return true; }
  catch (...) { std::cerr << name << " must be an integer\\n"; return false; }
}
bool nextFloat(int argc, char **argv, int *index, const char *name, float *value) {
  std::string text; if (!next(argc, argv, index, name, &text)) return false;
  try { *value = std::stof(text); return true; }
  catch (...) { std::cerr << name << " must be a number\\n"; return false; }
}
bool parse(int argc, char **argv, Options *options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--model-spec") { if (!next(argc, argv, &i, "--model-spec", &options->model_spec)) return false; }
    else if (arg == "--keywords") { if (!next(argc, argv, &i, "--keywords", &options->keywords)) return false; }
    else if (arg == "--pcm") { if (!next(argc, argv, &i, "--pcm", &options->pcm_path)) return false; }
    else if (arg == "--dump-pcm") { if (!next(argc, argv, &i, "--dump-pcm", &options->dump_pcm_path)) return false; }
    else if (arg == "--threshold") { if (!nextFloat(argc, argv, &i, "--threshold", &options->threshold)) return false; }
    else if (arg == "--beam-width") { if (!nextInt(argc, argv, &i, "--beam-width", &options->beam_width)) return false; }
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
    else if (arg == "--enable-vqe") options->enable_vqe = true;
    else if (arg == "--print-scores") options->print_scores = true;
    else if (arg == "-h" || arg == "--help") { usage(); std::exit(0); }
    else { std::cerr << "unknown argument: " << arg << "\\n"; return false; }
  }
  return options->threshold >= -1.0f && options->threshold <= 1.0f &&
         options->beam_width >= 1 && options->beam_width <= 8 && options->max_seconds >= 0 &&
         options->points_per_frame > 0 && options->frame_count > 0 &&
         options->frame_depth > 0 && options->timeout_ms > 0;
}
bool frameToSamples(const tdl_app::AudioFrame &frame, std::vector<std::int16_t> *samples) {
  if (frame.channels.empty() || frame.channels.front().empty() ||
      frame.channels.front().size() % sizeof(std::int16_t) != 0) return false;
  const std::vector<std::uint8_t> &bytes = frame.channels.front();
  samples->resize(bytes.size() / sizeof(std::int16_t));
  std::memcpy(samples->data(), bytes.data(), bytes.size()); return true;
}
bool readPcm(const std::string &path, std::vector<std::int16_t> *samples) {
  std::ifstream input(path.c_str(), std::ios::binary); if (!input) return false;
  input.seekg(0, std::ios::end); const std::streamoff bytes = input.tellg(); input.seekg(0, std::ios::beg);
  if (bytes <= 0 || bytes % static_cast<std::streamoff>(sizeof(std::int16_t)) != 0) return false;
  samples->resize(static_cast<std::size_t>(bytes) / sizeof(std::int16_t));
  return static_cast<bool>(input.read(reinterpret_cast<char *>(samples->data()), bytes));
}
void printHits(const std::vector<tdl_app::DirectKeywordResult> &hits) {
  for (const auto &hit : hits) std::cout << "keyword=" << hit.name << " confidence=" << hit.confidence
    << " threshold=" << hit.threshold << " trigger=" << (hit.triggered ? 1 : 0) << "\\n";
}
void printScores(const tdl_app::DirectKeywordSpotter &spotter) {
  for (const auto &score : spotter.scores()) std::cout << "score keyword=" << score.name
    << " confidence=" << score.confidence << " matched=" << score.matched_tokens << "/" << score.total_tokens
    << " tokens=\\\"" << score.matched_text << "\\\" complete=" << (score.complete ? 1 : 0)
    << " threshold=" << score.threshold << "\\n";
}
}  // namespace

int main(int argc, char **argv) {
  Options options; if (!parse(argc, argv, &options)) { usage(); return 2; }
  tdl_app::DirectKeywordSpotter kws; std::string error;
  if (!kws.load(options.model_spec, options.keywords, options.firmware, options.threshold, options.beam_width, &error)) {
    std::cerr << "KWS load failed: " << error << "\\n"; std::_Exit(3); }
  std::signal(SIGINT, onSignal); std::signal(SIGTERM, onSignal);
  if (!options.pcm_path.empty()) {
    std::vector<std::int16_t> samples; std::vector<tdl_app::DirectKeywordResult> hits;
    if (!readPcm(options.pcm_path, &samples) || !kws.accept(samples, &hits, &error)) {
      std::cerr << "offline PCM KWS failed: " << error << "\\n"; std::_Exit(4); }
    printHits(hits); if (options.print_scores) printScores(kws);
    std::vector<tdl_app::DirectKeywordResult> tail_hits;
    if (!kws.finish(&tail_hits, &error)) { std::cerr << "offline PCM KWS finish failed: " << error << "\\n"; std::_Exit(5); }
    printHits(tail_hits); if (options.print_scores) printScores(kws);
    std::cout << "offline PCM KWS stopped\\n" << std::flush; std::_Exit(0);
  }
  tdl_app::AudioInput::Config config = tdl_app::AudioInput::mono16k(options.ai_device, options.ai_channel, options.ai_card_id);
  config.volume_step = options.ai_volume; config.points_per_frame = options.points_per_frame; config.frame_count = options.frame_count; config.frame_depth = options.frame_depth;
  tdl_app::AudioInput audio(config);
  if (!audio.open(&error) || (options.enable_vqe && !audio.enableVqe(&error))) { std::cerr << "audio initialization failed: " << error << "\\n"; audio.close(); std::_Exit(4); }
  std::ofstream dump_pcm;
  if (!options.dump_pcm_path.empty()) { dump_pcm.open(options.dump_pcm_path.c_str(), std::ios::binary | std::ios::trunc); if (!dump_pcm) { std::cerr << "cannot create PCM dump: " << options.dump_pcm_path << "\\n"; audio.close(); std::_Exit(4); } }
  const std::size_t sample_limit = static_cast<std::size_t>(options.max_seconds) * kSampleRate; std::size_t captured = 0;
  std::cout << "direct KWS listening; Ctrl+C to stop\\n" << std::flush;
  while (!g_stop && (sample_limit == 0 || captured < sample_limit)) {
    tdl_app::AudioFrame frame; std::vector<std::int16_t> samples; std::vector<tdl_app::DirectKeywordResult> hits;
    if (!audio.readFrame(&frame, options.timeout_ms, &error) || !frameToSamples(frame, &samples) || !kws.accept(samples, &hits, &error)) { std::cerr << "KWS failed: " << error << "\\n"; audio.close(); std::_Exit(5); }
    if (dump_pcm.is_open()) { dump_pcm.write(reinterpret_cast<const char *>(samples.data()), static_cast<std::streamsize>(samples.size() * sizeof(samples[0]))); if (!dump_pcm) { std::cerr << "failed writing PCM dump: " << options.dump_pcm_path << "\\n"; audio.close(); std::_Exit(5); } }
    captured += samples.size(); printHits(hits); if (options.print_scores) printScores(kws);
  }
  std::vector<tdl_app::DirectKeywordResult> tail_hits; if (!kws.finish(&tail_hits, &error)) std::cerr << "KWS finish failed: " << error << "\\n"; printHits(tail_hits); if (options.print_scores) printScores(kws);
  audio.close(); std::cout << "direct KWS stopped\\n" << std::flush; std::_Exit(0);
}
