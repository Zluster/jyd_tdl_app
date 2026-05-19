#include "tdl_app/media_ipc_client.hpp"

#include <cstddef>
#include <cstdint>

namespace tdl_app {
namespace {

void fillDefaultStatus(MediaIpcStatus *status, std::int32_t code) {
  if (status) {
    status->code = code;
    status->detail = 0;
  }
}

}  // namespace

class MediaIpcClient::Impl {
 public:
  explicit Impl(const MediaIpcServiceConfig &config) : config_(config) {}

  bool open(std::string *error) {
    (void)error;
    opened_ = true;
    return true;
  }

  void close() { opened_ = false; }

  bool isOpen() const { return opened_; }

  bool sendSimple(MediaIpcModule module, MediaIpcCommand command,
                  const void *payload, std::size_t payload_size,
                  MediaIpcStatus *status, std::string *error) {
    (void)module;
    (void)command;
    (void)payload;
    (void)payload_size;
    (void)error;
    if (!opened_) {
      fillDefaultStatus(status, -1);
      return false;
    }
    fillDefaultStatus(status, 0);
    return true;
  }

  bool requestFrame(const MediaIpcRequestFrame &request, MediaIpcFrameReply *reply,
                    MediaIpcStatus *status, std::string *error) {
    (void)request;
    (void)error;
    if (!opened_) {
      fillDefaultStatus(status, -1);
      return false;
    }
    if (reply) {
      *reply = MediaIpcFrameReply{};
    }
    fillDefaultStatus(status, 0);
    return true;
  }

 private:
  MediaIpcServiceConfig config_;
  bool opened_ = false;
};

MediaIpcClient::MediaIpcClient() : MediaIpcClient(MediaIpcServiceConfig{}) {}

MediaIpcClient::MediaIpcClient(const MediaIpcServiceConfig &config)
    : config_(config), impl_(new Impl(config_)) {}

MediaIpcClient::~MediaIpcClient() {
  delete impl_;
  impl_ = nullptr;
}

bool MediaIpcClient::open(std::string *error) { return impl_->open(error); }

void MediaIpcClient::close() { impl_->close(); }

bool MediaIpcClient::isOpen() const { return impl_->isOpen(); }

bool MediaIpcClient::ping(std::string *error) {
  return sendSimple(MediaIpcModule::Sys, MediaIpcCommand::Ping, nullptr, 0,
                    nullptr, error);
}

bool MediaIpcClient::openVo(const MediaIpcOpenVoRequest &request,
                            MediaIpcStatus *status, std::string *error) {
  return sendSimple(MediaIpcModule::Vo, MediaIpcCommand::Open, &request,
                    sizeof(request), status, error);
}

bool MediaIpcClient::openGraphicVo(const MediaIpcOpenGraphicVoRequest &request,
                                   MediaIpcStatus *status,
                                   std::string *error) {
  return sendSimple(MediaIpcModule::GraphicVo, MediaIpcCommand::Open, &request,
                    sizeof(request), status, error);
}

bool MediaIpcClient::clearGraphicVo(const MediaIpcGraphicVoClearRequest &request,
                                    MediaIpcStatus *status,
                                    std::string *error) {
  return sendSimple(MediaIpcModule::GraphicVo, MediaIpcCommand::Clear, &request,
                    sizeof(request), status, error);
}

bool MediaIpcClient::bind(const MediaIpcBindRequest &request,
                          MediaIpcStatus *status, std::string *error) {
  return sendSimple(MediaIpcModule::Sys, MediaIpcCommand::Bind, &request,
                    sizeof(request), status, error);
}

bool MediaIpcClient::openVpss(const MediaIpcOpenVpssRequest &request,
                              MediaIpcStatus *status, std::string *error) {
  return sendSimple(MediaIpcModule::Vpss, MediaIpcCommand::Open, &request,
                    sizeof(request), status, error);
}

bool MediaIpcClient::openRegion(const MediaIpcOpenRegionRequest &request,
                                MediaIpcStatus *status, std::string *error) {
  return sendSimple(MediaIpcModule::Rgn, MediaIpcCommand::Open, &request,
                    sizeof(request), status, error);
}

bool MediaIpcClient::attachRegion(const MediaIpcAttachRegionRequest &request,
                                  MediaIpcStatus *status,
                                  std::string *error) {
  return sendSimple(MediaIpcModule::Rgn, MediaIpcCommand::UpdateBitmap,
                    &request, sizeof(request), status, error);
}

bool MediaIpcClient::openVenc(const MediaIpcOpenVencRequest &request,
                              MediaIpcStatus *status, std::string *error) {
  return sendSimple(MediaIpcModule::Venc, MediaIpcCommand::Open, &request,
                    sizeof(request), status, error);
}

bool MediaIpcClient::requestFrame(const MediaIpcRequestFrame &request,
                                  MediaIpcFrameReply *reply,
                                  MediaIpcStatus *status,
                                  std::string *error) {
  return impl_->requestFrame(request, reply, status, error);
}

bool MediaIpcClient::sendSimple(MediaIpcModule module, MediaIpcCommand command,
                                const void *payload, std::size_t payload_size,
                                MediaIpcStatus *status, std::string *error) {
  return impl_->sendSimple(module, command, payload, payload_size, status,
                           error);
}

}  // namespace tdl_app
