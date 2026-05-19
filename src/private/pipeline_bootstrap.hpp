#pragma once

#include <memory>
#include <string>

#include "tdl_app/mmf.hpp"
#include "tdl_app/sensor_media.hpp"

namespace tdl_app {

class PipelineBootstrap {
 public:
  virtual ~PipelineBootstrap() = default;
  virtual bool open(std::string *error) = 0;
  virtual void close() = 0;
};

class MmfBootstrap final : public PipelineBootstrap {
 public:
  explicit MmfBootstrap(const Mmf::Config &config) : mmf_(config) {}
  bool open(std::string *error) override { return mmf_.open(error); }
  void close() override { mmf_.close(); }

 private:
  Mmf mmf_;
};

class SensorMediaBootstrap final : public PipelineBootstrap {
 public:
  explicit SensorMediaBootstrap(const SensorMedia::Config &config)
      : sensor_media_(config) {}
  bool open(std::string *error) override { return sensor_media_.open(error); }
  void close() override { sensor_media_.close(); }

 private:
  SensorMedia sensor_media_;
};

}  // namespace tdl_app
