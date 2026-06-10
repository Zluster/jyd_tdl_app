#include "tdl_app/frame_sink.hpp"

namespace tdl_app {

bool NullFrameSink::open(std::string *error) {
  (void)error;
  return true;
}

bool NullFrameSink::write(const Frame &frame, const AlgorithmResult &result,
                          std::string *error) {
  (void)frame;
  (void)result;
  (void)error;
  return true;
}

void NullFrameSink::close() {}

}  // namespace tdl_app
