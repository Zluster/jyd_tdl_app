#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "tdl_app/tdl_app.hpp"

namespace {

struct Options {
  std::string pcm_path;
  std::string model_spec;
  std::string firmware;
  std::string model_dir;
  int chunk_bytes = 0;
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_vad_demo --pcm FILE --model-spec FILE\n"
      << "               [--firmware FILE] [--model-dir DIR]\n"
      << "               [--chunk-bytes 3200]\n";
}

bool parseInt(const std::string &value, int *out) {
  try {
    *out = std::stoi(value);
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

    if (arg == "--pcm") {
      const char *value = requireValue("--pcm");
      if (!value) return false;
      opt->pcm_path = value;
    } else if (arg == "--model-spec") {
      const char *value = requireValue("--model-spec");
      if (!value) return false;
      opt->model_spec = value;
    } else if (arg == "--firmware") {
      const char *value = requireValue("--firmware");
      if (!value) return false;
      opt->firmware = value;
    } else if (arg == "--model-dir") {
      const char *value = requireValue("--model-dir");
      if (!value) return false;
      opt->model_dir = value;
    } else if (arg == "--chunk-bytes") {
      const char *value = requireValue("--chunk-bytes");
      if (!value || !parseInt(value, &opt->chunk_bytes)) {
        std::cerr << "invalid --chunk-bytes value\n";
        return false;
      }
    } else if (arg == "-h" || arg == "--help") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return false;
    }
  }

  if (opt->pcm_path.empty()) {
    std::cerr << "pcm path is required\n";
    return false;
  }
  if (opt->model_spec.empty()) {
    std::cerr << "model-spec is required\n";
    return false;
  }
  if (opt->chunk_bytes < 0) {
    std::cerr << "chunk-bytes must be >= 0\n";
    return false;
  }
  return true;
}

bool readFile(const std::string &path, std::vector<std::uint8_t> *data) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    return false;
  }
  ifs.seekg(0, std::ios::end);
  const std::streamsize size = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  if (size < 0) {
    return false;
  }
  data->resize(static_cast<std::size_t>(size));
  return ifs.read(reinterpret_cast<char *>(data->data()), size).good() ||
         ifs.gcount() == size;
}

void printResult(std::size_t chunk_index,
                 const tdl_app::VoiceActivityResult &result) {
  std::cout << "chunk[" << chunk_index << "]"
            << " has_speech=" << (result.has_speech ? 1 : 0)
            << " start_event=" << (result.start_event ? 1 : 0)
            << " end_event=" << (result.end_event ? 1 : 0)
            << " segments=" << result.segmentCount() << "\n";
  for (std::size_t i = 0; i < result.segments.size(); ++i) {
    std::cout << "  segment[" << i << "] start_ms="
              << result.segments[i].start_ms
              << " end_ms=" << result.segments[i].end_ms << "\n";
  }
}

}  // namespace

int main(int argc, char **argv) {
  Options opt;
  if (!parseArgs(argc, argv, &opt)) {
    printUsage();
    return 1;
  }

  std::vector<std::uint8_t> pcm;
  if (!readFile(opt.pcm_path, &pcm)) {
    std::cerr << "failed to read pcm file: " << opt.pcm_path << "\n";
    return 2;
  }

  tdl_app::VoiceActivityDetector vad;
  std::string error;
  if (!vad.load(
          tdl_app::ModelSessionConfig::fromSpec(opt.model_spec, opt.firmware,
                                                opt.model_dir, "VAD_FSMN"),
          &error)) {
    std::cerr << "load failed: " << error << "\n";
    return 3;
  }

  if (opt.chunk_bytes <= 0 || static_cast<std::size_t>(opt.chunk_bytes) >= pcm.size()) {
    tdl_app::VoiceActivityResult result;
    if (!vad.run(pcm, true, &result, &error)) {
      std::cerr << "run failed: " << error << "\n";
      return 4;
    }
    printResult(0, result);
    return 0;
  }

  std::size_t offset = 0;
  std::size_t chunk_index = 0;
  while (offset < pcm.size()) {
    const std::size_t remaining = pcm.size() - offset;
    const std::size_t take = std::min<std::size_t>(
        remaining, static_cast<std::size_t>(opt.chunk_bytes));
    const bool is_final = offset + take >= pcm.size();
    std::vector<std::uint8_t> chunk(pcm.begin() + offset,
                                    pcm.begin() + offset + take);
    tdl_app::VoiceActivityResult result;
    if (!vad.run(chunk, is_final, &result, &error)) {
      std::cerr << "run failed at chunk " << chunk_index << ": " << error
                << "\n";
      return 4;
    }
    printResult(chunk_index, result);
    offset += take;
    ++chunk_index;
  }
  return 0;
}
