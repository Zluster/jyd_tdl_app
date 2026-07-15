#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  std::string input_path = "-";
  std::string asr_model_spec;
  std::string vad_model_spec;
  std::string firmware;
  std::string model_dir;
  std::string dump_dir;
  std::size_t chunk_bytes = 3200;
  int sample_rate = 16000;
  bool verbose = false;
};

constexpr int kBytesPerSample = 2;
constexpr int kChannels = 1;

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_dualos_asr_demo --asr-model-spec FILE --vad-model-spec FILE\n"
      << "                      [--input FILE|-] [--firmware FILE]\n"
      << "                      [--model-dir DIR] [--chunk-bytes 3200]\n"
      << "                      [--dump-dir DIR] [--verbose]\n"
      << "\n"
      << "Input convention:\n"
      << "  - raw pcm\n"
      << "  - 16 kHz\n"
      << "  - 16-bit signed little-endian\n"
      << "  - mono\n"
      << "\n"
      << "Notes:\n"
      << "  - Use --input - to read from stdin / named pipe bridge\n"
      << "  - chunk-bytes defaults to 100 ms at 16k mono s16le\n";
}

bool parseInt(const std::string &value, int *out) {
  try {
    *out = std::stoi(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool parseSize(const std::string &value, std::size_t *out) {
  try {
    const unsigned long long parsed = std::stoull(value);
    if (parsed > std::numeric_limits<std::size_t>::max()) {
      return false;
    }
    *out = static_cast<std::size_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool parseArgs(int argc, char **argv, Options *opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto requireValue = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << name << " requires a value\n";
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--input") {
      const char *value = requireValue("--input");
      if (!value) return false;
      opt->input_path = value;
    } else if (arg == "--asr-model-spec") {
      const char *value = requireValue("--asr-model-spec");
      if (!value) return false;
      opt->asr_model_spec = value;
    } else if (arg == "--vad-model-spec") {
      const char *value = requireValue("--vad-model-spec");
      if (!value) return false;
      opt->vad_model_spec = value;
    } else if (arg == "--firmware") {
      const char *value = requireValue("--firmware");
      if (!value) return false;
      opt->firmware = value;
    } else if (arg == "--model-dir") {
      const char *value = requireValue("--model-dir");
      if (!value) return false;
      opt->model_dir = value;
    } else if (arg == "--dump-dir") {
      const char *value = requireValue("--dump-dir");
      if (!value) return false;
      opt->dump_dir = value;
    } else if (arg == "--chunk-bytes") {
      const char *value = requireValue("--chunk-bytes");
      if (!value || !parseSize(value, &opt->chunk_bytes)) {
        std::cerr << "invalid --chunk-bytes value\n";
        return false;
      }
    } else if (arg == "--sample-rate") {
      const char *value = requireValue("--sample-rate");
      if (!value || !parseInt(value, &opt->sample_rate)) {
        std::cerr << "invalid --sample-rate value\n";
        return false;
      }
    } else if (arg == "--verbose") {
      opt->verbose = true;
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }

  if (opt->asr_model_spec.empty()) {
    std::cerr << "--asr-model-spec is required\n";
    return false;
  }
  if (opt->vad_model_spec.empty()) {
    std::cerr << "--vad-model-spec is required\n";
    return false;
  }
  if (opt->sample_rate != 16000) {
    std::cerr << "only 16 kHz is supported in this demo\n";
    return false;
  }
  if (opt->chunk_bytes == 0 || (opt->chunk_bytes & 1u) != 0u) {
    std::cerr << "--chunk-bytes must be a positive even number\n";
    return false;
  }
  return true;
}

std::size_t bytesPerMs(int sample_rate) {
  return static_cast<std::size_t>(sample_rate / 1000) * kBytesPerSample *
         kChannels;
}

bool readChunk(std::istream &input, std::size_t chunk_bytes,
               std::vector<std::uint8_t> *chunk, std::string *error) {
  chunk->assign(chunk_bytes, 0);
  input.read(reinterpret_cast<char *>(chunk->data()),
             static_cast<std::streamsize>(chunk_bytes));
  const std::streamsize got = input.gcount();
  if (got < 0) {
    if (error) {
      *error = "stream returned a negative byte count";
    }
    return false;
  }
  chunk->resize(static_cast<std::size_t>(got));
  if (got == 0 && input.bad()) {
    if (error) {
      *error = "failed while reading audio input stream";
    }
    return false;
  }
  return true;
}

bool dumpSegment(const std::string &dump_dir, int utterance_index,
                 std::int32_t start_ms, std::int32_t end_ms,
                 const std::vector<std::uint8_t> &segment,
                 std::string *error) {
  if (dump_dir.empty()) {
    return true;
  }
  const std::string path =
      dump_dir + "/utt_" + std::to_string(utterance_index) + "_" +
      std::to_string(start_ms) + "_" + std::to_string(end_ms) + ".pcm";
  std::ofstream ofs(path, std::ios::binary);
  if (!ofs) {
    if (error) {
      *error = "failed to open dump file: " + path;
    }
    return false;
  }
  ofs.write(reinterpret_cast<const char *>(segment.data()),
            static_cast<std::streamsize>(segment.size()));
  if (!ofs.good()) {
    if (error) {
      *error = "failed to write dump file: " + path;
    }
    return false;
  }
  return true;
}

bool processVadResult(const tdl_app::VoiceActivityResult &vad_result,
                      const Options &opt, std::size_t bytes_per_ms,
                      std::vector<std::uint8_t> *buffered_pcm,
                      std::size_t *buffer_base_ms,
                      std::size_t *last_processed_end_ms,
                      int *utterance_index,
                      tdl_app::SpeechRecognizer *recognizer,
                      std::string *error) {
  std::size_t new_last_processed_end_ms = *last_processed_end_ms;
  for (const auto &segment : vad_result.segments) {
    if (segment.start_ms < 0 || segment.end_ms <= segment.start_ms) {
      continue;
    }

    const std::size_t start_ms = static_cast<std::size_t>(segment.start_ms);
    const std::size_t end_ms = static_cast<std::size_t>(segment.end_ms);
    if (end_ms <= *last_processed_end_ms) {
      continue;
    }
    if (start_ms < *buffer_base_ms) {
      if (error) {
        *error = "segment start timestamp fell behind buffered audio window";
      }
      return false;
    }

    const std::size_t start_offset = (start_ms - *buffer_base_ms) * bytes_per_ms;
    const std::size_t end_offset = (end_ms - *buffer_base_ms) * bytes_per_ms;
    if (end_offset > buffered_pcm->size() || start_offset >= end_offset) {
      if (error) {
        *error = "segment timestamps exceed buffered PCM range";
      }
      return false;
    }

    std::vector<std::uint8_t> utterance(buffered_pcm->begin() + start_offset,
                                        buffered_pcm->begin() + end_offset);
    tdl_app::SpeechRecognitionResult asr_result;
    if (!recognizer->run(utterance, &asr_result, error)) {
      return false;
    }

    ++(*utterance_index);
    std::cout << "utt[" << *utterance_index << "] start_ms=" << segment.start_ms
              << " end_ms=" << segment.end_ms
              << " bytes=" << utterance.size()
              << " text=" << asr_result.text << "\n";

    if (!dumpSegment(opt.dump_dir, *utterance_index, segment.start_ms,
                     segment.end_ms, utterance, error)) {
      return false;
    }
    new_last_processed_end_ms = end_ms;
  }

  if (new_last_processed_end_ms > *buffer_base_ms) {
    const std::size_t prune_bytes =
        (new_last_processed_end_ms - *buffer_base_ms) * bytes_per_ms;
    if (prune_bytes > buffered_pcm->size()) {
      if (error) {
        *error = "prune range exceeds buffered PCM size";
      }
      return false;
    }
    buffered_pcm->erase(buffered_pcm->begin(),
                        buffered_pcm->begin() + prune_bytes);
    *buffer_base_ms = new_last_processed_end_ms;
    *last_processed_end_ms = new_last_processed_end_ms;
  }

  if (opt.verbose) {
    std::cerr << "vad: has_speech=" << (vad_result.has_speech ? 1 : 0)
              << " start_event=" << (vad_result.start_event ? 1 : 0)
              << " end_event=" << (vad_result.end_event ? 1 : 0)
              << " segments=" << vad_result.segmentCount()
              << " buffered_bytes=" << buffered_pcm->size() << "\n";
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::ifstream input_file;
  std::istream *input = &std::cin;
  if (opt.input_path != "-") {
    input_file.open(opt.input_path, std::ios::binary);
    if (!input_file) {
      std::cerr << "failed to open input: " << opt.input_path << "\n";
      return 2;
    }
    input = &input_file;
  }

  std::string error;
  tdl_app::VoiceActivityDetector vad;
  if (!vad.load(
          tdl_app::ModelSessionConfig::fromSpec(opt.vad_model_spec, opt.firmware,
                                                opt.model_dir, "VAD_FSMN"),
          &error)) {
    std::cerr << "vad load failed: " << error << "\n";
    return 3;
  }

  tdl_app::SpeechRecognizer recognizer;
  if (!recognizer.load(
          tdl_app::ModelSessionConfig::fromSpec(opt.asr_model_spec, opt.firmware,
                                                opt.model_dir),
          &error)) {
    std::cerr << "asr load failed: " << error << "\n";
    return 4;
  }

  const std::size_t bytes_per_ms = bytesPerMs(opt.sample_rate);
  std::vector<std::uint8_t> buffered_pcm;
  std::vector<std::uint8_t> chunk;
  std::size_t buffer_base_ms = 0;
  std::size_t last_processed_end_ms = 0;
  int utterance_index = 0;

  while (true) {
    if (!readChunk(*input, opt.chunk_bytes, &chunk, &error)) {
      std::cerr << "read failed: " << error << "\n";
      return 5;
    }
    if (chunk.empty()) {
      break;
    }

    buffered_pcm.insert(buffered_pcm.end(), chunk.begin(), chunk.end());
    tdl_app::VoiceActivityResult vad_result;
    if (!vad.run(chunk, false, &vad_result, &error)) {
      std::cerr << "vad run failed: " << error << "\n";
      return 6;
    }
    if (!processVadResult(vad_result, opt, bytes_per_ms, &buffered_pcm,
                          &buffer_base_ms, &last_processed_end_ms,
                          &utterance_index, &recognizer, &error)) {
      std::cerr << "segment processing failed: " << error << "\n";
      return 7;
    }
  }

  tdl_app::VoiceActivityResult final_result;
  if (!vad.run(std::vector<std::uint8_t>(), true, &final_result, &error)) {
    std::cerr << "vad final flush failed: " << error << "\n";
    return 8;
  }
  if (!processVadResult(final_result, opt, bytes_per_ms, &buffered_pcm,
                        &buffer_base_ms, &last_processed_end_ms,
                        &utterance_index, &recognizer, &error)) {
    std::cerr << "final segment processing failed: " << error << "\n";
    return 9;
  }

  if (opt.verbose) {
    std::cerr << "completed, utterances=" << utterance_index
              << " remaining_buffered_bytes=" << buffered_pcm.size() << "\n";
  }
  return 0;
}
