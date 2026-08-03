#include "mmf_cv184x_resources.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

namespace mmf_cv184x {
namespace {

std::string resource_path(CodecResourceType type, uint32_t channel) {
  const char* name = type == CodecResourceType::Venc ? "venc" : "vdec";
  return std::string("/tmp/mmf_") + name + "_" + std::to_string(channel) + ".lock";
}

}  // namespace

void codec_resource_lease_init(CodecResourceLease* lease) {
  if (lease == nullptr)
    return;
  lease->fd = -1;
  lease->path[0] = '\0';
}

bool codec_resource_lease_valid(const CodecResourceLease* lease) {
  return lease != nullptr && lease->fd >= 0;
}

void codec_resource_lease_release(CodecResourceLease* lease) {
  if (lease == nullptr)
    return;
  if (lease->fd >= 0) {
    (void)::flock(lease->fd, LOCK_UN);
    (void)::close(lease->fd);
    lease->fd = -1;
  }
  lease->path[0] = '\0';
}

mmf_result_t acquire_codec_resource(CodecResourceType type, uint32_t channel, const char* owner,
                                    uint32_t timeout_ms, CodecResourceLease* lease) {
  if (lease == nullptr) {
    return MMF_EINVAL;
  }
  codec_resource_lease_release(lease);

  const std::string path = resource_path(type, channel);
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
  if (fd < 0) {
    set_last_error("open codec resource lock failed: " + path);
    return MMF_EIO;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (true) {
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
      (void)::ftruncate(fd, 0);
      const std::string text = std::string(owner ? owner : "unknown") + "\n";
      (void)::write(fd, text.data(), text.size());
      lease->fd = fd;
      std::snprintf(lease->path, sizeof(lease->path), "%s", path.c_str());
      return MMF_OK;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
      (void)::close(fd);
      set_last_error("lock codec resource failed: " + path);
      return MMF_EIO;
    }
    if (timeout_ms == 0 || std::chrono::steady_clock::now() >= deadline) {
      (void)::close(fd);
      set_last_error("codec resource busy: " + path);
      return MMF_EBUSY;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

mmf_result_t acquire_codec_operation(CodecResourceType type, const char* owner, uint32_t timeout_ms,
                                     CodecResourceLease* lease) {
  return acquire_codec_resource(type, 0xffffffffU, owner, timeout_ms, lease);
}

}  // namespace mmf_cv184x
