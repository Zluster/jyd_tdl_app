#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include "c_apis/tdl_model_def.h"
#include "c_apis/tdl_sdk.h"
#include "nn/tdl_model_list.h"
#include "tdl_app/frame_source.hpp"
#include "tdl_app/model_descriptor.hpp"

namespace tdl_app {
namespace private_tdl_sdk {

inline void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

inline std::string normalizeToken(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  std::replace(value.begin(), value.end(), '-', '_');
  return value;
}

inline std::string resolveModelPath(const ModelSessionConfig &config,
                                    std::string *error) {
  if (config.model_spec.empty()) {
    setError(error, "model_spec is empty");
    return std::string();
  }
  ModelDescriptor descriptor;
  if (!loadModelDescriptor(config.model_spec, &descriptor, error)) {
    return std::string();
  }
  return tdl_app::resolveModelPath(descriptor);
}

inline std::string resolveModelToken(const ModelSessionConfig &config,
                                     const std::string &requested_model_type,
                                     const std::string &fallback_model_type,
                                     std::string *error) {
  if (!config.model_type.empty()) {
    return normalizeToken(config.model_type);
  }

  if (!config.model_spec.empty()) {
    ModelDescriptor descriptor;
    std::string ignored_error;
    if (loadModelDescriptor(config.model_spec, &descriptor, &ignored_error) &&
        !descriptor.model_type.empty()) {
      return normalizeToken(descriptor.model_type);
    }
  }

  if (!requested_model_type.empty()) {
    return normalizeToken(requested_model_type);
  }

  if (!fallback_model_type.empty()) {
    return normalizeToken(fallback_model_type);
  }

  setError(error, "model_type is empty and no default is available");
  return std::string();
}

inline bool resolveModelId(const std::string &model_token, TDLModel *model_id,
                           std::string *error) {
  if (!model_id) {
    setError(error, "model id pointer is null");
    return false;
  }
  const std::string normalized = normalizeToken(model_token);
#define X(name, comment)                                                    \
  if (normalized == #name || normalized == "TDL_MODEL_" #name) {           \
    *model_id = TDL_MODEL_##name;                                           \
    return true;                                                            \
  }
  MODEL_TYPE_LIST
#undef X
  setError(error, "unsupported TDL model_type: " + model_token);
  return false;
}

class ImageGuard {
 public:
  ImageGuard() = default;
  ~ImageGuard() { reset(); }

  ImageGuard(const ImageGuard &) = delete;
  ImageGuard &operator=(const ImageGuard &) = delete;

  bool load(const std::string &image_path, std::string *error) {
    reset();
    image_ = TDL_ReadImage(image_path.c_str());
    if (!image_) {
      setError(error, "TDL_ReadImage failed: " + image_path);
      return false;
    }
    return true;
  }

  bool wrap(const Frame &frame, std::string *error) {
    reset();
    if (!frame.image_path.empty()) {
      return load(frame.image_path, error);
    }
    if (!frame.native) {
      setError(error, "frame has neither image_path nor native pointer");
      return false;
    }
    image_ = TDL_WrapFrame(frame.native, false, false);
    if (!image_) {
      setError(error, "TDL_WrapFrame failed");
      return false;
    }
    return true;
  }

  TDLImage get() const { return image_; }

  void reset() {
    if (image_) {
      TDL_DestroyImage(image_);
      image_ = nullptr;
    }
  }

 private:
  TDLImage image_ = nullptr;
};

class Session {
 public:
  Session() = default;
  ~Session() { close(); }

  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  bool open(const ModelSessionConfig &config, const std::string &model_token,
            std::string *error) {
    close();

    const std::string model_path = resolveModelPath(config, error);
    if (model_path.empty()) {
      return false;
    }
    if (!config.firmware.empty()) {
      setenv("BMRUNTIME_USING_FIRMWARE", config.firmware.c_str(), 0);
    }
    if (!resolveModelId(model_token, &model_id_, error)) {
      return false;
    }

    handle_ = TDL_CreateHandle(0);
    if (!handle_) {
      setError(error, "TDL_CreateHandle failed");
      model_id_ = TDL_MODEL_INVALID;
      return false;
    }

    const int ret =
        TDL_OpenModel(handle_, model_id_, model_path.c_str(), nullptr, 0);
    if (ret != 0) {
      setError(error, "TDL_OpenModel failed, ret=" + std::to_string(ret));
      close();
      return false;
    }
    return true;
  }

  void close() {
    if (handle_) {
      if (model_id_ != TDL_MODEL_INVALID) {
        TDL_CloseModel(handle_, model_id_);
      }
      TDL_DestroyHandle(handle_);
      handle_ = nullptr;
    }
    model_id_ = TDL_MODEL_INVALID;
  }

  bool initialized() const { return handle_ != nullptr; }
  TDLHandle handle() const { return handle_; }
  TDLModel modelId() const { return model_id_; }

 private:
  TDLHandle handle_ = nullptr;
  TDLModel model_id_ = TDL_MODEL_INVALID;
};

}  // namespace private_tdl_sdk
}  // namespace tdl_app
