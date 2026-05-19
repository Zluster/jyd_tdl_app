#pragma once

#include <string>

#include "tdl_app/media_ipc_protocol.hpp"

namespace tdl_app {

class MediaIpcClient {
 public:
  MediaIpcClient();
  explicit MediaIpcClient(const MediaIpcServiceConfig &config);
  ~MediaIpcClient();

  MediaIpcClient(const MediaIpcClient &) = delete;
  MediaIpcClient &operator=(const MediaIpcClient &) = delete;

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

  bool ping(std::string *error = nullptr);

  bool openVo(const MediaIpcOpenVoRequest &request,
              MediaIpcStatus *status = nullptr,
              std::string *error = nullptr);
  bool openGraphicVo(const MediaIpcOpenGraphicVoRequest &request,
                     MediaIpcStatus *status = nullptr,
                     std::string *error = nullptr);
  bool clearGraphicVo(const MediaIpcGraphicVoClearRequest &request,
                      MediaIpcStatus *status = nullptr,
                      std::string *error = nullptr);
  bool bind(const MediaIpcBindRequest &request,
            MediaIpcStatus *status = nullptr,
            std::string *error = nullptr);
  bool openVpss(const MediaIpcOpenVpssRequest &request,
                MediaIpcStatus *status = nullptr,
                std::string *error = nullptr);
  bool openRegion(const MediaIpcOpenRegionRequest &request,
                  MediaIpcStatus *status = nullptr,
                  std::string *error = nullptr);
  bool attachRegion(const MediaIpcAttachRegionRequest &request,
                    MediaIpcStatus *status = nullptr,
                    std::string *error = nullptr);
  bool openVenc(const MediaIpcOpenVencRequest &request,
                MediaIpcStatus *status = nullptr,
                std::string *error = nullptr);
  bool requestFrame(const MediaIpcRequestFrame &request,
                    MediaIpcFrameReply *reply,
                    MediaIpcStatus *status = nullptr,
                    std::string *error = nullptr);

 private:
  bool sendSimple(MediaIpcModule module, MediaIpcCommand command,
                  const void *payload, std::size_t payload_size,
                  MediaIpcStatus *status, std::string *error);

  class Impl;
  MediaIpcServiceConfig config_;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
