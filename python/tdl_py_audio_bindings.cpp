// Python audio bindings are intentionally separate from tdl_py_module.cpp.
// They use only the project Audio API and direct CV184X BMRT algorithm cores.

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <algorithm>
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

// High-level AI/AO facade for Python. Audio operations return false and
// preserve the hardware error in last_error so an application can decide how
// to recover without an exception unwinding its UI loop.
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

#ifdef TDL_PY_WITH_NPU
class PySpeakerRecognizer {
 public:
  bool load(const std::string &model_spec) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.load(model_spec, &error);
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
  tdl_app::SpeakerRecognizer recognizer_;
  tdl_app::SpeakerDatabase database_;
  std::string last_error_;
};

class PyStreamingAsr {
 public:
  bool load(const std::string &model_spec) {
    std::string error;
    bool ok = false;
    {
      nb::gil_scoped_release guard;
      ok = recognizer_.load(model_spec, &error);
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

  void reset() {
    recognizer_.reset();
    last_error_.clear();
  }

  bool initialized() const { return recognizer_.initialized(); }
  const std::string &text() const { return recognizer_.text(); }
  const std::string &lastError() const { return last_error_; }

 private:
  tdl_app::NpuStreamingAsr recognizer_;
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
      ok = spotter_.load(model_spec, keywords_path, "", threshold,
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
  std::string last_error_;
};

#endif






}  // namespace

void registerAudioBindings(nb::module_ &m) {
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
      .def("save_database", &PySpeakerRecognizer::saveDatabase,
           nb::arg("path"))
      .def("load_database", &PySpeakerRecognizer::loadDatabase,
           nb::arg("path"))
      .def("clear", &PySpeakerRecognizer::clear)
      .def("labels", &PySpeakerRecognizer::labels)
      .def_prop_ro("initialized", &PySpeakerRecognizer::initialized)
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
      .def("reset", &PyStreamingAsr::reset)
      .def_prop_ro("initialized", &PyStreamingAsr::initialized)
      .def_prop_ro("text", &PyStreamingAsr::text)
      .def_prop_ro("last_error", &PyStreamingAsr::lastError);

  // --- Keyword spotting ----------------------------------------------------
  nb::class_<PyKeywordSpotter>(m, "KeywordSpotter",
      "Direct BMRT RNNT keyword spotter over signed 16-bit mono 16 kHz PCM."
      " No Sherpa or ONNX Runtime is used.")
      .def(nb::init<>())
      .def("load", &PyKeywordSpotter::load,
           nb::arg("model_spec"), nb::arg("keywords_path"),
           nb::arg("threshold") = -1.0f, nb::arg("beam_width") = 6,
           "Load CV184X KWS bmodels and a keyword token file.")
      .def("accept", &PyKeywordSpotter::accept, nb::arg("pcm"),
           "Accept PCM; return newly triggered keyword dictionaries, or None on failure.")
      .def("finish", &PyKeywordSpotter::finish)
      .def("scores", &PyKeywordSpotter::scores,
           "Return current score dictionaries for every configured keyword.")
      .def("reset", &PyKeywordSpotter::reset)
      .def_prop_ro("initialized", &PyKeywordSpotter::initialized)
      .def_prop_ro("last_error", &PyKeywordSpotter::lastError);
#endif


}