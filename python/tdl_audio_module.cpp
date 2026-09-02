// Isolated CV184X audio module. Keep the binding classes in one source so
// tdl_py and tdl_audio expose identical streaming ASR/KWS/speaker APIs.
#define TDL_AUDIO_ONLY 1
#include "tdl_py_audio_bindings.cpp"

NB_MODULE(tdl_audio, m) {
  m.doc() = "CV184X isolated real-time audio bindings.";
  registerAudioBindings(m);
}
