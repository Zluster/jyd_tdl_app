#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "tdl_app/npu_keyword_spotter.hpp"

int main(int argc, char **argv) {
  if (argc != 5 || std::string(argv[1]) != "--pcm" ||
      std::string(argv[3]) != "--keywords") {
    std::cerr << "Usage: tdl_npu_keyword_spotter_pcm_smoke --pcm FILE --keywords FILE\n";
    return 2;
  }

  std::ifstream input(argv[2], std::ios::binary);
  if (!input) {
    std::cerr << "cannot read PCM: " << argv[2] << "\n";
    return 1;
  }
  input.seekg(0, std::ios::end);
  const std::streamoff bytes = input.tellg();
  input.seekg(0, std::ios::beg);
  if (bytes <= 0 || bytes % static_cast<std::streamoff>(sizeof(std::int16_t))) {
    std::cerr << "PCM must be signed 16-bit mono samples\n";
    return 1;
  }
  std::vector<std::int16_t> pcm(static_cast<std::size_t>(bytes) / sizeof(std::int16_t));
  if (!input.read(reinterpret_cast<char *>(pcm.data()), bytes)) {
    std::cerr << "failed reading PCM\n";
    return 1;
  }

  tdl_app::NpuKeywordSpotter spotter;
  std::string error;
  if (!spotter.load("./configs/model_specs/npu_zipformer_zh_kws.mud", &error) ||
      !spotter.loadKeywords(argv[4], &error)) {
    std::cerr << "load failed: " << error << "\n";
    return 1;
  }
  std::vector<tdl_app::KeywordEvent> events;
  if (!spotter.acceptPcm(pcm, &events, &error)) {
    std::cerr << "decode failed: " << error << "\n";
    return 1;
  }
  std::vector<tdl_app::KeywordEvent> tail_events;
  if (!spotter.finish(&tail_events, &error)) {
    std::cerr << "finalize failed: " << error << "\n";
    return 1;
  }
  events.insert(events.end(), tail_events.begin(), tail_events.end());
  for (const auto &event : events) {
    std::printf("event keyword=%s action=TRIGGER\n", event.text.c_str());
  }
  return events.empty() ? 3 : 0;
}
