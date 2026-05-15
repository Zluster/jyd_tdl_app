#include "tdl_app/sys_context.hpp"

#include <cstring>

#include "cvi_common.h"
#include "cvi_sys.h"

namespace tdl_app {
namespace {

void setError(std::string *error, const std::string &message) {
  if (error) {
    *error = message;
  }
}

}  // namespace

SysContext::SysContext() = default;

SysContext::SysContext(const Config &config) : config_(config) {}

SysContext::~SysContext() { close(); }

bool SysContext::open(std::string *error) {
  if (opened_) {
    return true;
  }

  const int ret = CVI_SYS_Init();
  if (ret == CVI_SUCCESS) {
    opened_ = true;
    own_sys_ = true;
    return true;
  }

  if (config_.reuse_existing) {
    opened_ = true;
    own_sys_ = false;
    return true;
  }

  setError(error, "CVI_SYS_Init failed, ret=" + std::to_string(ret));
  return false;
}

void SysContext::close() {
  if (own_sys_) {
    CVI_SYS_Exit();
    own_sys_ = false;
  }
  opened_ = false;
}

bool SysContext::isOpen() const { return opened_; }

bool SysContext::allocIon(std::size_t size, const std::string &name, bool cached,
                          IonBuffer *buffer, std::string *error) const {
  if (!buffer) {
    setError(error, "ion buffer pointer is null");
    return false;
  }

  CVI_U64 physical = 0;
  void *virtual_addr = nullptr;
  const int ret = cached
                      ? CVI_SYS_IonAlloc_Cached(
                            &physical, &virtual_addr, name.c_str(),
                            static_cast<CVI_U32>(size))
                      : CVI_SYS_IonAlloc(&physical, &virtual_addr, name.c_str(),
                                         static_cast<CVI_U32>(size));
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SYS_IonAlloc failed, ret=" + std::to_string(ret));
    return false;
  }

  buffer->physical = physical;
  buffer->virtual_addr = virtual_addr;
  buffer->size = size;
  buffer->cached = cached;
  return true;
}

bool SysContext::freeIon(IonBuffer *buffer, std::string *error) const {
  if (!buffer) {
    setError(error, "ion buffer pointer is null");
    return false;
  }
  if (!buffer->virtual_addr) {
    return true;
  }

  const int ret =
      CVI_SYS_IonFree(buffer->physical, static_cast<CVI_VOID *>(buffer->virtual_addr));
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SYS_IonFree failed, ret=" + std::to_string(ret));
    return false;
  }

  buffer->physical = 0;
  buffer->virtual_addr = nullptr;
  buffer->size = 0;
  buffer->cached = false;
  return true;
}

bool SysContext::flushIon(const IonBuffer &buffer, std::string *error) const {
  const int ret = CVI_SYS_IonFlushCache(
      buffer.physical, static_cast<CVI_VOID *>(buffer.virtual_addr),
      static_cast<CVI_U32>(buffer.size));
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SYS_IonFlushCache failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool SysContext::invalidateIon(const IonBuffer &buffer,
                               std::string *error) const {
  const int ret = CVI_SYS_IonInvalidateCache(
      buffer.physical, static_cast<CVI_VOID *>(buffer.virtual_addr),
      static_cast<CVI_U32>(buffer.size));
  if (ret != CVI_SUCCESS) {
    setError(error,
             "CVI_SYS_IonInvalidateCache failed, ret=" + std::to_string(ret));
    return false;
  }
  return true;
}

bool SysContext::getVersion(std::string *version, std::string *error) const {
  if (!version) {
    setError(error, "version pointer is null");
    return false;
  }

  MMF_VERSION_S info;
  std::memset(&info, 0, sizeof(info));
  const int ret = CVI_SYS_GetVersion(&info);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SYS_GetVersion failed, ret=" + std::to_string(ret));
    return false;
  }

  *version = info.version;
  return true;
}

bool SysContext::getChipId(std::uint32_t *chip_id, std::string *error) const {
  if (!chip_id) {
    setError(error, "chip id pointer is null");
    return false;
  }

  CVI_U32 out = 0;
  const int ret = CVI_SYS_GetChipId(&out);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SYS_GetChipId failed, ret=" + std::to_string(ret));
    return false;
  }
  *chip_id = out;
  return true;
}

bool SysContext::getChipVersion(std::uint32_t *chip_version,
                                std::string *error) const {
  if (!chip_version) {
    setError(error, "chip version pointer is null");
    return false;
  }

  CVI_U32 out = 0;
  const int ret = CVI_SYS_GetChipVersion(&out);
  if (ret != CVI_SUCCESS) {
    setError(error,
             "CVI_SYS_GetChipVersion failed, ret=" + std::to_string(ret));
    return false;
  }
  *chip_version = out;
  return true;
}

bool SysContext::getCurrentPts(std::uint64_t *pts_us,
                               std::string *error) const {
  if (!pts_us) {
    setError(error, "pts pointer is null");
    return false;
  }

  CVI_U64 out = 0;
  const int ret = CVI_SYS_GetCurPTS(&out);
  if (ret != CVI_SUCCESS) {
    setError(error, "CVI_SYS_GetCurPTS failed, ret=" + std::to_string(ret));
    return false;
  }
  *pts_us = out;
  return true;
}

}  // namespace tdl_app
