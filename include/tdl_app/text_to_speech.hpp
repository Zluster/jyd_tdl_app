#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "tdl_app/algorithm_engine.hpp"

namespace tdl_app {

// 一次合成的统计信息，供应用显示与排查用。
struct TtsSynthesisStats {
  int chunks = 0;      // 文本切出的段数（每段不超过 32 个 token）
  int rendered = 0;    // 实际解码出音频的段数
  long frames = 0;     // 累计 latent 帧数
  long samples = 0;    // 输出 16 kHz 采样点数
  double duration = 0.0;  // 输出时长（秒）
};

// CV184X 中文文字转语音。
//
// 与其它音频算法一致：文本前处理、音素切分、NPU 推理、重采样全部在 C++ 内
// 完成，Python 侧只负责调用。模型由 .mud 描述：
//
//   [basic]
//   model = <前端 bmodel>
//   [extra]
//   frontend_model = <前端 bmodel>
//   decoder_model  = <解码 bmodel>
//
// 前端把文本 token 变成 latent 与逐 token 时长，解码器再把 latent 还原成
// 22.05 kHz 波形，最后按 22.05 kHz -> 16 kHz 重采样输出 16 kHz 单声道 PCM。
// 不依赖任何 Python 侧音素表，也不使用 Sherpa / ONNX Runtime。
class TextToSpeech {
 public:
  using Config = ModelSessionConfig;

  TextToSpeech();
  ~TextToSpeech();

  TextToSpeech(const TextToSpeech &) = delete;
  TextToSpeech &operator=(const TextToSpeech &) = delete;

  bool load(const Config &config, std::string *error = nullptr);
  bool load(const std::string &model_spec, std::string *error = nullptr);
  bool load(const std::string &model_spec, const std::string &firmware,
            std::string *error = nullptr);

  // 合成整段文本，返回 16 kHz、单声道、有符号 16 位小端 PCM。
  bool run(const std::string &text, std::vector<std::int16_t> *pcm16le_mono,
           std::string *error = nullptr);

  // 合成并写成标准 PCM WAV 文件。
  bool synthesize(const std::string &text, const std::string &wav_path,
                  std::string *error = nullptr);

  // 只做文本前处理：返回每段的 token（含 BOS/EOS），便于检查切分结果。
  bool tokens(const std::string &text, std::vector<std::vector<int>> *chunks,
              std::string *error = nullptr);

  bool initialized() const;
  const Config &config() const { return config_; }
  const TtsSynthesisStats &lastStats() const;

  // 卸载 NPU 模型并释放设备。
  void reset();

 private:
  class Impl;
  Config config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
