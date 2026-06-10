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
};

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  tdl_asr_demo --pcm FILE --model-spec FILE\n"
      << "               [--firmware FILE] [--model-dir DIR]\n"
      << "\n"
      << "PCM convention:\n"
      << "  - raw pcm\n"
      << "  - 16 kHz\n"
      << "  - 16-bit signed little-endian\n"
      << "  - mono\n";
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
  if (opt->pcm_path.size() >= 4 &&
      opt->pcm_path.substr(opt->pcm_path.size() - 4) == ".wav") {
    std::cerr << "wav is not supported here, please pass raw 16k/16bit/mono pcm\n";
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
  if (pcm.empty()) {
    std::cerr << "pcm file is empty: " << opt.pcm_path << "\n";
    return 2;
  }
  if ((pcm.size() & 1u) != 0u) {
    std::cerr << "pcm byte size must be even for 16-bit samples\n";
    return 2;
  }

  tdl_app::SpeechRecognizer recognizer;
  std::string error;
  if (!recognizer.load(
          tdl_app::ModelSessionConfig::fromSpec(opt.model_spec, opt.firmware,
                                                opt.model_dir),
          &error)) {
    std::cerr << "load failed: " << error << "\n";
    return 3;
  }

  tdl_app::SpeechRecognitionResult result;
  if (!recognizer.run(pcm, &result, &error)) {
    std::cerr << "run failed: " << error << "\n";
    return 4;
  }

  std::cout << "pcm_bytes: " << pcm.size() << "\n";
  std::cout << "text: " << result.text << "\n";
  return 0;
}
