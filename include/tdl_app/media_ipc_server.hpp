#pragma once

#include <string>

#include "tdl_app/media_ipc_protocol.hpp"

namespace tdl_app {

class MediaIpcServer {
 public:
  class Handler {
   public:
    virtual ~Handler() = default;

    virtual bool onPing(MediaIpcStatus *status, std::string *error) = 0;
    virtual bool onOpenVo(const MediaIpcOpenVoRequest &request,
                          MediaIpcStatus *status,
                          std::string *error) = 0;
    virtual bool onOpenGraphicVo(const MediaIpcOpenGraphicVoRequest &request,
                                 MediaIpcStatus *status,
                                 std::string *error) = 0;
    virtual bool onClearGraphicVo(const MediaIpcGraphicVoClearRequest &request,
                                  MediaIpcStatus *status,
                                  std::string *error) = 0;
    virtual bool onBind(const MediaIpcBindRequest &request,
                        MediaIpcStatus *status,
                        std::string *error) = 0;
    virtual bool onOpenVpss(const MediaIpcOpenVpssRequest &request,
                            MediaIpcStatus *status,
                            std::string *error) = 0;
    virtual bool onOpenRegion(const MediaIpcOpenRegionRequest &request,
                              MediaIpcStatus *status,
                              std::string *error) = 0;
    virtual bool onAttachRegion(const MediaIpcAttachRegionRequest &request,
                                MediaIpcStatus *status,
                                std::string *error) = 0;
    virtual bool onOpenVenc(const MediaIpcOpenVencRequest &request,
                            MediaIpcStatus *status,
                            std::string *error) = 0;
    virtual bool onRequestFrame(const MediaIpcRequestFrame &request,
                                MediaIpcFrameReply *reply,
                                MediaIpcStatus *status,
                                std::string *error) = 0;
  };

  MediaIpcServer();
  MediaIpcServer(const MediaIpcServiceConfig &config, Handler *handler);
  ~MediaIpcServer();

  MediaIpcServer(const MediaIpcServer &) = delete;
  MediaIpcServer &operator=(const MediaIpcServer &) = delete;

  void setConfig(const MediaIpcServiceConfig &config);
  void setHandler(Handler *handler);

  bool open(std::string *error = nullptr);
  void close();
  bool isOpen() const;

 private:
  MediaIpcServiceConfig config_;
  Handler *handler_ = nullptr;
  class Impl;
  Impl *impl_ = nullptr;
};

}  // namespace tdl_app
