#include "tdl_app/media_ipc_server.hpp"

namespace tdl_app {

class MediaIpcServer::Impl {
 public:
  Impl(const MediaIpcServiceConfig &config, Handler *handler)
      : config_(config), handler_(handler) {}

  void setConfig(const MediaIpcServiceConfig &config) { config_ = config; }

  void setHandler(Handler *handler) { handler_ = handler; }

  bool open(std::string *error) {
    (void)error;
    if (!handler_) {
      return false;
    }
    opened_ = true;
    return true;
  }

  void close() { opened_ = false; }

  bool isOpen() const { return opened_; }

 private:
  MediaIpcServiceConfig config_;
  Handler *handler_ = nullptr;
  bool opened_ = false;
};

MediaIpcServer::MediaIpcServer()
    : MediaIpcServer(MediaIpcServiceConfig{}, nullptr) {}

MediaIpcServer::MediaIpcServer(const MediaIpcServiceConfig &config,
                               Handler *handler)
    : config_(config), handler_(handler), impl_(new Impl(config_, handler_)) {}

MediaIpcServer::~MediaIpcServer() {
  delete impl_;
  impl_ = nullptr;
}

void MediaIpcServer::setConfig(const MediaIpcServiceConfig &config) {
  config_ = config;
  impl_->setConfig(config_);
}

void MediaIpcServer::setHandler(Handler *handler) {
  handler_ = handler;
  impl_->setHandler(handler_);
}

bool MediaIpcServer::open(std::string *error) { return impl_->open(error); }

void MediaIpcServer::close() { impl_->close(); }

bool MediaIpcServer::isOpen() const { return impl_->isOpen(); }

}  // namespace tdl_app
