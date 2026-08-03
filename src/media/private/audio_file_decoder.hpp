#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tdl_app {
namespace private_audio {

constexpr int kPlaybackSampleRate = 16000;
constexpr int kPlaybackChannels = 2;
constexpr int kPlaybackBitDepth = 16;
constexpr int kPlaybackSamplesPerFrame = 160;

using Pcm16StereoSink =
    std::function<bool(const std::vector<std::uint8_t> &, std::string *)>;

bool decodeAudioFileToPcm16Stereo(const std::string &path,
                                  const Pcm16StereoSink &sink,
                                  std::string *error);

}  // namespace private_audio
}  // namespace tdl_app

