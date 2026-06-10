#pragma once

#include <memory>
#include <string>

namespace tdl_app {

struct Frame;
struct AlgorithmResult;

class FrameSink {
 public:
  virtual ~FrameSink() = default;
  virtual bool open(std::string *error = nullptr) = 0;
  virtual bool write(const Frame &frame, const AlgorithmResult &result,
                     std::string *error = nullptr) = 0;
  virtual void close() = 0;
};

class NullFrameSink final : public FrameSink {
 public:
  bool open(std::string *error = nullptr) override;
  bool write(const Frame &frame, const AlgorithmResult &result,
             std::string *error = nullptr) override;
  void close() override;
};

}  // namespace tdl_app
