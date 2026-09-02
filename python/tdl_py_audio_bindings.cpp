// Python audio bindings are intentionally separate from tdl_py_module.cpp.
// They use only the project Audio API and direct CV184X BMRT algorithm cores.

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "tdl_app/audio.hpp"

#ifdef TDL_PY_WITH_NPU
#include "tdl_app/direct_keyword_spotter.hpp"
#include "tdl_app/npu_asr_recognizer.hpp"
#include "tdl_app/speaker_recognizer.hpp"
#endif

namespace nb = nanobind;

namespace {

bool pcm16FromBytes(const nb::bytes &pcm, std::vector<std::int16_t> *samples,
                    std::string *error) {
  if (!samples) {
    if (error) *error = "PCM output pointer is null";
    return false;
  }
  if (pcm.size() == 0 || pcm.size() % sizeof(std::int16_t) != 0) {
    if (error) *error = "PCM must be non-empty signed 16-bit mono bytes";
    return false;
  }
  samples->resize(pcm.size() / sizeof(std::int16_t));
  std::memcpy(samples->data(), pcm.c_str(), pcm.size());
  return true;
}

std::string defaultFirmwarePath() {
  const char *firmware = std::getenv("BMRUNTIME_USING_FIRMWARE");
  return (firmware && firmware[0]) ? std::string(firmware)
                                   : "/lib/firmware/libbm1688_kernel_module.so";
}

tdl_app::ModelSessionConfig modelConfig(const std::string &model_spec) {
  return tdl_app::ModelSessionConfig::fromSpec(model_spec,
                                                defaultFirmwarePath());
}

// This is private plumbing for the high-level algorithm classes below.  Each
// Python application owns one algorithm and therefore one microphone stream;
// PCM is never part of the app-facing real-time API.
class RealtimeMicrophone {
 public:
  bool start(int input_volume, int points_per_frame, int frame_count,
             int frame_depth, int timeout_ms, std::string *error) {
    tdl_app::AudioInputStreamConfig config;
    config.io.sample_rate = 16000;
    config.io.channels = 1;
    config.io.bit_depth = 16;
    config.io.ai_volume = input_volume;
    config.io.points_per_frame = points_per_frame;
    config.io.frame_count = frame_count;
    config.io.frame_depth = frame_depth;
    config.io.timeout_ms = timeout_ms;
    return audio_.openInputStream(config, error);
  }

  bool read(std::vector<std::int16_t> *samples,
            tdl_app::AudioPcmChunk *metadata, std::string *error) {
    if (!samples) {
      if (error) *error = "PCM sample output pointer is null";
      return false;
    }
    tdl_app::AudioPcmChunk chunk;
    if (!audio_.readInputChunk(&chunk, error)) return false;
    if (chunk.sample_rate != 16000 || chunk.channels != 1 ||
        chunk.bit_depth != 16 || chunk.data.empty() ||
        chunk.data.size() % sizeof(std::int16_t) != 0) {
      if (error) *error = "microphone did not return 16 kHz mono PCM16";
      return false;
    }
    samples->resize(chunk.data.size() / sizeof(std::int16_t));
    std::memcpy(samples->data(), chunk.data.data(), chunk.data.size());
    if (metadata) *metadata = std::move(chunk);
    return true;
  }

  bool stop(std::string *error) { return audio_.closeInputStream(error); }

  bool opened() const {
    tdl_app::AudioInputStreamStatus status;
    return audio_.inputStreamStatus(&status, nullptr) && status.opened;
  }

 private:
  tdl_app::Audio audio_;
};

// High-level AI/AO facade for Python. Audio operations return false and
// preserve the hardware error in last_error so an application can decide how
// to recover without an exception unwinding its UI loop.
#ifndef TDL_AUDIO_ONLY
class PyAudio {
 public:
  bool recordWav(const std::string &path, double seconds, int sample_rate,
                 int channels, int input_volume, int points_per_frame,
                 int frame_count, int frame_depth, int timeout_ms) {
    const tdl_app::AudioIoConfig config = makeConfig(
        sample_rate, channels, input_volume, 24, points_per_frame,
        frame_count, frame_depth, timeout_ms);
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.recordWav(path, seconds, config, &error);
    }
    last_error_ = error;
    return ok;
  }

  nb::object capturePcm(double seconds, int sample_rate, int channels,
                        int input_volume, int points_per_frame,
                        int frame_count, int frame_depth, int timeout_ms) {
    const tdl_app::AudioIoConfig config = makeConfig(
        sample_rate, channels, input_volume, 24, points_per_frame,
        frame_count, frame_depth, timeout_ms);
    std::string error;
    std::vector<std::uint8_t> pcm;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      tdl_app::AudioInputStreamConfig input_config;
      input_config.io = config;
      ok = audio_.openInputStream(input_config, &error);
      if (ok) {
        const std::size_t bytes_per_sample =
            static_cast<std::size_t>(config.bit_depth / 8);
        const std::size_t target_bytes = static_cast<std::size_t>(
            seconds * config.sample_rate * config.channels * bytes_per_sample + 0.5);
        if (seconds <= 0.0 || bytes_per_sample == 0 || target_bytes == 0) {
          error = "capture seconds and audio format must be valid";
          ok = false;
        }
        while (ok && pcm.size() < target_bytes) {
          tdl_app::AudioPcmChunk chunk;
          if (!audio_.readInputChunk(&chunk, &error)) {
            ok = false;
            break;
          }
          if (chunk.data.empty()) {
            error = "audio input returned an empty PCM chunk";
            ok = false;
            break;
          }
          const std::size_t count = std::min(
              chunk.data.size(), target_bytes - pcm.size());
          pcm.insert(pcm.end(), chunk.data.begin(), chunk.data.begin() + count);
        }
        std::string close_error;
        if (!audio_.closeInputStream(&close_error) && ok) {
          error = close_error;
          ok = false;
        }
      }
    }
    last_error_ = error;
    if (!ok) {
      return nb::none();
    }
    return nb::bytes(reinterpret_cast<const char *>(pcm.data()), pcm.size());
  }

  bool playWav(const std::string &path, int output_volume, int timeout_ms) {
    const tdl_app::AudioIoConfig config = makeConfig(
        16000, 1, 24, output_volume, 160, 8, 8, timeout_ms);
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.playWav(path, config, &error);
    }
    last_error_ = error;
    return ok;
  }

  bool loopback(double seconds, int sample_rate, int channels,
                int input_volume, int output_volume, int points_per_frame,
                int frame_count, int frame_depth, int timeout_ms) {
    tdl_app::AudioSessionConfig config;
    config.io = makeConfig(sample_rate, channels, input_volume, output_volume,
                           points_per_frame, frame_count, frame_depth,
                           timeout_ms);
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = audio_.loopback(seconds, config, &error);
    }
    last_error_ = error;
    return ok;
  }

  bool setInputVolume(int volume) {
    std::string error;
    const bool ok = audio_.setInputVolume(volume, {}, &error);
    last_error_ = error;
    return ok;
  }

  bool setOutputVolume(int volume) {
    std::string error;
    const bool ok = audio_.setOutputVolume(volume, {}, &error);
    last_error_ = error;
    return ok;
  }

  nb::object inputVolume() {
    int volume = 0;
    std::string error;
    if (!audio_.getInputVolume(&volume, {}, &error)) {
      last_error_ = error;
      return nb::none();
    }
    last_error_.clear();
    return nb::int_(volume);
  }

  nb::object outputVolume() {
    int volume = 0;
    std::string error;
    if (!audio_.getOutputVolume(&volume, {}, &error)) {
      last_error_ = error;
      return nb::none();
    }
    last_error_.clear();
    return nb::int_(volume);
  }

  nb::dict status() const {
    const tdl_app::AudioStatus status = audio_.status();
    nb::dict out;
    out["runtime_ready"] = status.runtime_ready;
    out["input_stream_open"] = status.input_stream_open;
    out["output_stream_open"] = status.output_stream_open;
    out["session_open"] = status.session_open;
    out["sample_rate"] = status.sample_rate;
    out["channels"] = status.channels;
    out["bit_depth"] = status.bit_depth;
    out["ai_device"] = status.ai_device;
    out["ai_channel"] = status.ai_channel;
    out["ao_device"] = status.ao_device;
    out["ao_channel"] = status.ao_channel;
    out["note"] = status.note;
    return out;
  }

  const std::string &lastError() const { return last_error_; }

 private:
  static tdl_app::AudioIoConfig makeConfig(int sample_rate, int channels,
                                            int input_volume,
                                            int output_volume,
                                            int points_per_frame,
                                            int frame_count, int frame_depth,
                                            int timeout_ms) {
    tdl_app::AudioIoConfig config;
    config.sample_rate = sample_rate;
    config.channels = channels;
    config.ai_volume = input_volume;
    config.ao_volume = output_volume;
    config.points_per_frame = points_per_frame;
    config.frame_count = frame_count;
    config.frame_depth = frame_depth;
    config.timeout_ms = timeout_ms;
    return config;
  }

  tdl_app::Audio audio_;
  std::string last_error_;
};
#endif

#ifdef TDL_PY_WITH_NPU
class PySpeakerRecognizer {
 public:
  bool load(const std::string &model_spec) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.load(modelConfig(model_spec), &error);
    }
    last_error_ = error;
    return ok;
  }

  bool enroll(const std::string &label, const nb::bytes &pcm) {
    std::vector<std::int16_t> samples;
    if (!pcm16FromBytes(pcm, &samples, &last_error_)) return false;
    tdl_app::SpeakerEmbedding embedding;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.extract(samples, &embedding, &last_error_);
    }
    if (!ok) return false;
    if (!database_.upsert(label, embedding, &last_error_)) return false;
    last_error_.clear();
    return true;
  }

  nb::object recognize(const nb::bytes &pcm, float threshold) {
    std::vector<std::int16_t> samples;
    if (!pcm16FromBytes(pcm, &samples, &last_error_)) {
      return nb::none();
    }
    tdl_app::SpeakerEmbedding embedding;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.extract(samples, &embedding, &last_error_);
    }
    if (!ok) {
      return nb::none();
    }
    const tdl_app::SpeakerMatch match =
        recognizer_.identify(embedding, database_, threshold);
    nb::dict result;
    result["label"] = match.label;
    result["score"] = match.score;
    result["matched"] = match.matched;
    last_error_.clear();
    return result;
  }

  nb::object verify(const std::string &label, const nb::bytes &pcm,
                    float threshold) {
    std::vector<std::int16_t> samples;
    if (!pcm16FromBytes(pcm, &samples, &last_error_)) return nb::none();
    tdl_app::SpeakerEmbedding embedding;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.extract(samples, &embedding, &last_error_);
    }
    if (!ok) return nb::none();
    return result(recognizer_.verify(label, embedding, database_, threshold));
  }

  bool beginEnroll(const std::string &label, double seconds, int input_volume,
                   int points_per_frame, int timeout_ms) {
    return beginCapture(CaptureMode::Enroll, label, seconds, 0.60f,
                        input_volume, points_per_frame, timeout_ms);
  }

  bool beginVerify(const std::string &label, double seconds, float threshold,
                   int input_volume, int points_per_frame, int timeout_ms) {
    return beginCapture(CaptureMode::Verify, label, seconds, threshold,
                        input_volume, points_per_frame, timeout_ms);
  }

  bool beginIdentify(double seconds, float threshold, int input_volume,
                     int points_per_frame, int timeout_ms) {
    return beginCapture(CaptureMode::Identify, "", seconds, threshold,
                        input_volume, points_per_frame, timeout_ms);
  }

  // Read one live audio frame.  Applications call poll() in their regular UI
  // loop; it returns progress until the requested voice sample is complete.
  nb::object poll() {
    if (mode_ == CaptureMode::None) {
      last_error_ = "no speaker capture is active";
      return nb::none();
    }
    std::vector<std::int16_t> chunk;
    tdl_app::AudioPcmChunk metadata;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.read(&chunk, &metadata, &error);
    }
    if (!ok) {
      microphone_.stop(nullptr);
      mode_ = CaptureMode::None;
      last_error_ = error;
      return nb::none();
    }
    const std::size_t remaining = target_samples_ - samples_.size();
    const std::size_t count = std::min(remaining, chunk.size());
    samples_.insert(samples_.end(), chunk.begin(), chunk.begin() + count);
    if (samples_.size() < target_samples_) {
      return progress(false, metadata);
    }

    microphone_.stop(nullptr);
    tdl_app::SpeakerEmbedding embedding;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.extract(samples_, &embedding, &error);
    }
    if (!ok) {
      resetCapture();
      last_error_ = error;
      return nb::none();
    }

    nb::dict out = progress(true, metadata);
    if (mode_ == CaptureMode::Enroll) {
      ok = database_.upsert(label_, embedding, &error);
      out["label"] = label_;
      out["score"] = 1.0f;
      out["matched"] = ok;
    } else if (mode_ == CaptureMode::Verify) {
      appendMatch(&out, recognizer_.verify(label_, embedding, database_, threshold_));
    } else {
      appendMatch(&out, recognizer_.identify(embedding, database_, threshold_));
    }
    resetCapture();
    last_error_ = error;
    return ok ? nb::object(out) : nb::none();
  }

  void cancel() {
    microphone_.stop(nullptr);
    resetCapture();
    last_error_.clear();
  }

  bool capturing() const { return mode_ != CaptureMode::None; }

  bool saveDatabase(const std::string &path) {
    const bool ok = database_.save(path, &last_error_);
    return ok;
  }

  bool loadDatabase(const std::string &path) {
    const bool ok = database_.load(path, &last_error_);
    return ok;
  }

  void clear() {
    database_.clear();
    last_error_.clear();
  }

  std::vector<std::string> labels() const { return database_.labels(); }
  bool initialized() const { return recognizer_.initialized(); }
  const std::string &lastError() const { return last_error_; }

 private:
  enum class CaptureMode { None, Enroll, Verify, Identify };

  static nb::dict result(const tdl_app::SpeakerMatch &match) {
    nb::dict out;
    appendMatch(&out, match);
    return out;
  }

  static void appendMatch(nb::dict *out, const tdl_app::SpeakerMatch &match) {
    (*out)["label"] = match.label;
    (*out)["score"] = match.score;
    (*out)["matched"] = match.matched;
  }

  bool beginCapture(CaptureMode mode, const std::string &label, double seconds,
                    float threshold, int input_volume, int points_per_frame,
                    int timeout_ms) {
    if (!recognizer_.initialized()) {
      last_error_ = "speaker model is not loaded";
      return false;
    }
    if (mode_ != CaptureMode::None) {
      last_error_ = "speaker capture is already active";
      return false;
    }
    if (seconds <= 0.0) {
      last_error_ = "capture seconds must be > 0";
      return false;
    }
    const std::size_t target = static_cast<std::size_t>(seconds * 16000.0 + 0.5);
    if (target == 0) {
      last_error_ = "capture duration is too short";
      return false;
    }
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.start(input_volume, points_per_frame, 8, 8,
                             timeout_ms, &error);
    }
    if (!ok) {
      last_error_ = error;
      return false;
    }
    mode_ = mode;
    label_ = label;
    threshold_ = threshold;
    target_samples_ = target;
    samples_.clear();
    samples_.reserve(target);
    last_error_.clear();
    return true;
  }

  nb::dict progress(bool done, const tdl_app::AudioPcmChunk &chunk) const {
    nb::dict out;
    out["done"] = done;
    out["progress"] = static_cast<double>(samples_.size()) /
                      static_cast<double>(target_samples_);
    out["timestamp"] = chunk.timestamp;
    out["sequence"] = chunk.sequence;
    return out;
  }

  void resetCapture() {
    mode_ = CaptureMode::None;
    label_.clear();
    threshold_ = 0.60f;
    target_samples_ = 0;
    samples_.clear();
  }

  tdl_app::SpeakerRecognizer recognizer_;
  tdl_app::SpeakerDatabase database_;
  RealtimeMicrophone microphone_;
  CaptureMode mode_ = CaptureMode::None;
  std::string label_;
  float threshold_ = 0.60f;
  std::size_t target_samples_ = 0;
  std::vector<std::int16_t> samples_;
  std::string last_error_;
};

class PyStreamingAsr {
 public:
  bool load(const std::string &model_spec) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.load(modelConfig(model_spec), &error);
    }
    last_error_ = error;
    return ok;
  }

  nb::object accept(const nb::bytes &pcm) {
    std::vector<std::int16_t> samples;
    if (!pcm16FromBytes(pcm, &samples, &last_error_)) return nb::none();
    std::string text;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.acceptPcm(samples, &text, &last_error_);
    }
    if (!ok) return nb::none();
    last_error_.clear();
    return nb::str(text.c_str());
  }

  nb::object finish() {
    std::string text;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.finish(&text, &last_error_);
    }
    if (!ok) return nb::none();
    last_error_.clear();
    return nb::str(text.c_str());
  }

  bool start(int input_volume, int points_per_frame, int timeout_ms) {
    if (!recognizer_.initialized()) {
      last_error_ = "ASR model is not loaded";
      return false;
    }
    recognizer_.reset();
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.start(input_volume, points_per_frame, 8, 8,
                             timeout_ms, &error);
    }
    last_error_ = error;
    return ok;
  }

  // One real-time microphone frame in, one incremental recognition result
  // out.  "text" is only what appeared in this call; "full_text" is the
  // utterance accumulated since start()/reset().
  nb::object read() {
    std::vector<std::int16_t> samples;
    tdl_app::AudioPcmChunk chunk;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.read(&samples, &chunk, &error);
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    std::string text;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.acceptPcm(samples, &text, &error);
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    nb::dict out;
    out["text"] = text;
    out["full_text"] = recognizer_.text();
    out["timestamp"] = chunk.timestamp;
    out["sequence"] = chunk.sequence;
    last_error_.clear();
    return out;
  }

  bool stop() {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.stop(&error);
    }
    last_error_ = error;
    return ok;
  }

  bool listening() const { return microphone_.opened(); }

  void reset() {
    recognizer_.reset();
    last_error_.clear();
  }

  bool initialized() const { return recognizer_.initialized(); }
  const std::string &text() const { return recognizer_.text(); }
  const std::string &lastError() const { return last_error_; }

 private:
  tdl_app::NpuStreamingAsr recognizer_;
  RealtimeMicrophone microphone_;
  std::string last_error_;
};

class PyKeywordSpotter {
 public:
  bool load(const std::string &model_spec, const std::string &keywords_path,
            float threshold, int beam_width) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = spotter_.load(model_spec, keywords_path, defaultFirmwarePath(), threshold,
                         beam_width, &error);
    }
    last_error_ = error;
    return ok;
  }

  nb::object accept(const nb::bytes &pcm) {
    std::vector<std::int16_t> samples;
    if (!pcm16FromBytes(pcm, &samples, &last_error_)) return nb::none();
    std::vector<tdl_app::DirectKeywordResult> hits;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = spotter_.accept(samples, &hits, &last_error_);
    }
    if (!ok) return nb::none();
    last_error_.clear();
    return result(hits);
  }

  nb::object finish() {
    std::vector<tdl_app::DirectKeywordResult> hits;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = spotter_.finish(&hits, &last_error_);
    }
    if (!ok) return nb::none();
    last_error_.clear();
    return result(hits);
  }

  bool start(int input_volume, int points_per_frame, int timeout_ms) {
    if (!spotter_.initialized()) {
      last_error_ = "KWS model is not loaded";
      return false;
    }
    spotter_.reset();
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.start(input_volume, points_per_frame, 8, 8,
                             timeout_ms, &error);
    }
    last_error_ = error;
    return ok;
  }

  // Read and evaluate exactly one microphone frame.  An empty list is the
  // normal no-keyword result; None means a microphone or inference error.
  nb::object read() {
    std::vector<std::int16_t> samples;
    tdl_app::AudioPcmChunk chunk;
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.read(&samples, &chunk, &error);
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    std::vector<tdl_app::DirectKeywordResult> hits;
    {
      nb::gil_scoped_release guard;
      ok = spotter_.accept(samples, &hits, &error);
    }
    if (!ok) {
      last_error_ = error;
      return nb::none();
    }
    last_error_.clear();
    return result(hits);
  }

  bool stop() {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = microphone_.stop(&error);
    }
    last_error_ = error;
    return ok;
  }

  bool listening() const { return microphone_.opened(); }

  nb::list scores() const { return result(spotter_.scores()); }

  void reset() {
    spotter_.reset();
    last_error_.clear();
  }

  bool initialized() const { return spotter_.initialized(); }
  const std::string &lastError() const { return last_error_; }

 private:
  static nb::dict one(const tdl_app::DirectKeywordResult &source) {
    nb::dict out;
    out["name"] = source.name;
    out["confidence"] = source.confidence;
    out["threshold"] = source.threshold;
    out["matched_tokens"] = source.matched_tokens;
    out["total_tokens"] = source.total_tokens;
    out["matched_text"] = source.matched_text;
    out["complete"] = source.complete;
    out["triggered"] = source.triggered;
    return out;
  }

  static nb::list result(const std::vector<tdl_app::DirectKeywordResult> &hits) {
    nb::list out;
    for (const tdl_app::DirectKeywordResult &hit : hits) out.append(one(hit));
    return out;
  }

  tdl_app::DirectKeywordSpotter spotter_;
  RealtimeMicrophone microphone_;
  std::string last_error_;
};

#endif






}  // namespace

void registerAudioBindings(nb::module_ &m) {
#ifndef TDL_AUDIO_ONLY
  // --- Audio ---------------------------------------------------------------
  nb::class_<PyAudio>(m, "Audio",
      "Basic AI/AO audio control. Methods return False on a hardware error; "
      "inspect last_error to handle it in the application.")
      .def(nb::init<>())
      .def("record_wav", &PyAudio::recordWav,
           nb::arg("path"), nb::arg("seconds") = 3.0,
           nb::arg("sample_rate") = 16000, nb::arg("channels") = 1,
           nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160,
           nb::arg("frame_count") = 8, nb::arg("frame_depth") = 8,
           nb::arg("timeout_ms") = 1000,
           "Record signed PCM into a standard WAV file.")
      .def("capture_pcm", &PyAudio::capturePcm,
           nb::arg("seconds") = 3.0, nb::arg("sample_rate") = 16000,
           nb::arg("channels") = 1, nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160,
           nb::arg("frame_count") = 8, nb::arg("frame_depth") = 8,
           nb::arg("timeout_ms") = 1000,
           "Capture signed interleaved PCM bytes for inference.")
      .def("play_wav", &PyAudio::playWav,
           nb::arg("path"), nb::arg("output_volume") = 24,
           nb::arg("timeout_ms") = 1000,
           "Play a standard PCM WAV file through AO.")
      .def("loopback", &PyAudio::loopback,
           nb::arg("seconds") = 3.0, nb::arg("sample_rate") = 16000,
           nb::arg("channels") = 1, nb::arg("input_volume") = 24,
           nb::arg("output_volume") = 24,
           nb::arg("points_per_frame") = 160,
           nb::arg("frame_count") = 8, nb::arg("frame_depth") = 8,
           nb::arg("timeout_ms") = 1000,
           "Route microphone input directly to speaker output for a fixed time.")
      .def("set_input_volume", &PyAudio::setInputVolume, nb::arg("volume"))
      .def("set_output_volume", &PyAudio::setOutputVolume, nb::arg("volume"))
      .def("input_volume", &PyAudio::inputVolume,
           "Return current input volume, or None on failure.")
      .def("output_volume", &PyAudio::outputVolume,
           "Return current output volume, or None on failure.")
      .def("status", &PyAudio::status)
      .def_prop_ro("last_error", &PyAudio::lastError);
#endif

#ifdef TDL_PY_WITH_NPU
  // --- Speaker recognition -------------------------------------------------
  nb::class_<PySpeakerRecognizer>(m, "SpeakerRecognizer",
      "CAMPPlus speaker enrollment and recognition over 16 kHz mono PCM.")
      .def(nb::init<>())
      .def("load", &PySpeakerRecognizer::load, nb::arg("model_spec"),
           "Load the CAMPPlus speaker model.")
      .def("enroll", &PySpeakerRecognizer::enroll,
           nb::arg("label"), nb::arg("pcm"),
           "Extract a voice embedding and add or replace this label.")
      .def("recognize", &PySpeakerRecognizer::recognize,
           nb::arg("pcm"), nb::arg("threshold") = 0.60f,
           "Return {label, score, matched}, or None when extraction fails.")
      .def("verify", &PySpeakerRecognizer::verify,
           nb::arg("label"), nb::arg("pcm"), nb::arg("threshold") = 0.60f,
           "Verify a PCM sample against one enrolled label.")
      .def("begin_enroll", &PySpeakerRecognizer::beginEnroll,
           nb::arg("label"), nb::arg("seconds") = 3.0,
           nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160, nb::arg("timeout_ms") = 1000,
           "Start non-blocking microphone enrollment; use poll() until done.")
      .def("begin_verify", &PySpeakerRecognizer::beginVerify,
           nb::arg("label"), nb::arg("seconds") = 3.0,
           nb::arg("threshold") = 0.60f, nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160, nb::arg("timeout_ms") = 1000,
           "Start non-blocking microphone verification; use poll() until done.")
      .def("begin_identify", &PySpeakerRecognizer::beginIdentify,
           nb::arg("seconds") = 3.0, nb::arg("threshold") = 0.60f,
           nb::arg("input_volume") = 24,
           nb::arg("points_per_frame") = 160, nb::arg("timeout_ms") = 1000,
           "Start non-blocking microphone identification; use poll() until done.")
      .def("poll", &PySpeakerRecognizer::poll,
           "Consume one live microphone frame and return enrollment progress/result.")
      .def("cancel", &PySpeakerRecognizer::cancel)
      .def("save_database", &PySpeakerRecognizer::saveDatabase,
           nb::arg("path"))
      .def("load_database", &PySpeakerRecognizer::loadDatabase,
           nb::arg("path"))
      .def("clear", &PySpeakerRecognizer::clear)
      .def("labels", &PySpeakerRecognizer::labels)
      .def_prop_ro("initialized", &PySpeakerRecognizer::initialized)
      .def_prop_ro("capturing", &PySpeakerRecognizer::capturing)
      .def_prop_ro("last_error", &PySpeakerRecognizer::lastError);

  // --- Streaming ASR -------------------------------------------------------
  nb::class_<PyStreamingAsr>(m, "StreamingAsr",
      "Direct BMRT Zipformer ASR over signed 16-bit mono 16 kHz PCM."
      " No Sherpa or ONNX Runtime is used.")
      .def(nb::init<>())
      .def("load", &PyStreamingAsr::load, nb::arg("model_spec"),
           "Load the CV184X encoder/decoder/joiner ASR model set.")
      .def("accept", &PyStreamingAsr::accept, nb::arg("pcm"),
           "Accept PCM and return only text decoded by this call, or None on failure.")
      .def("finish", &PyStreamingAsr::finish,
           "Flush the final ASR chunk and return only final text, or None on failure.")
      .def("start", &PyStreamingAsr::start,
           nb::arg("input_volume") = 24, nb::arg("points_per_frame") = 160,
           nb::arg("timeout_ms") = 1000,
           "Open the microphone and reset this real-time recognition session.")
      .def("read", &PyStreamingAsr::read,
           "Recognize one real-time microphone frame; return text and metadata.")
      .def("stop", &PyStreamingAsr::stop,
           "Close the microphone; call finish() separately to flush ASR.")
      .def("reset", &PyStreamingAsr::reset)
      .def_prop_ro("initialized", &PyStreamingAsr::initialized)
      .def_prop_ro("listening", &PyStreamingAsr::listening)
      .def_prop_ro("text", &PyStreamingAsr::text)
      .def_prop_ro("last_error", &PyStreamingAsr::lastError);

  // --- Keyword spotting ----------------------------------------------------
  nb::class_<PyKeywordSpotter>(m, "KeywordSpotter",
      "Direct BMRT RNNT keyword spotter over signed 16-bit mono 16 kHz PCM."
      " No Sherpa or ONNX Runtime is used.")
      .def(nb::init<>())
      .def("load", &PyKeywordSpotter::load,
           nb::arg("model_spec"), nb::arg("keywords_path"),
           nb::arg("threshold") = -1.0f, nb::arg("beam_width") = 2,
           "Load CV184X KWS bmodels and a keyword token file.")
      .def("accept", &PyKeywordSpotter::accept, nb::arg("pcm"),
           "Accept PCM; return newly triggered keyword dictionaries, or None on failure.")
      .def("finish", &PyKeywordSpotter::finish)
      .def("start", &PyKeywordSpotter::start,
           nb::arg("input_volume") = 24, nb::arg("points_per_frame") = 160,
           nb::arg("timeout_ms") = 1000,
           "Open the microphone and reset this real-time keyword session.")
      .def("read", &PyKeywordSpotter::read,
           "Evaluate one real-time microphone frame and return keyword hits.")
      .def("stop", &PyKeywordSpotter::stop,
           "Close the microphone; call finish() separately to flush KWS.")
      .def("scores", &PyKeywordSpotter::scores,
           "Return current score dictionaries for every configured keyword.")
      .def("reset", &PyKeywordSpotter::reset)
      .def_prop_ro("initialized", &PyKeywordSpotter::initialized)
      .def_prop_ro("listening", &PyKeywordSpotter::listening)
      .def_prop_ro("last_error", &PyKeywordSpotter::lastError);
#endif


}
