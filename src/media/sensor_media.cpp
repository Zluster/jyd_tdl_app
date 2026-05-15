#include "tdl_app/sensor_media.hpp"

#include "media/private/sensor_media_impl.hpp"

namespace tdl_app {

class SensorMedia::Impl {
 public:
  explicit Impl(const Config &config) : inner_(config) {}

  bool open(std::string *error) { return inner_.open(error); }
  void close() { inner_.close(); }
  bool isOpen() const { return inner_.isOpen(); }

 private:
  private_media::SensorMediaImpl inner_;
};

SensorMedia::SensorMedia() : SensorMedia(Config{}) {}

SensorMedia::SensorMedia(const Config &config)
    : config_(config), impl_(new Impl(config_)) {}

SensorMedia::~SensorMedia() {
  delete impl_;
  impl_ = nullptr;
}

bool SensorMedia::open(std::string *error) { return impl_->open(error); }

void SensorMedia::close() { impl_->close(); }

bool SensorMedia::isOpen() const { return impl_->isOpen(); }

}  // namespace tdl_app
