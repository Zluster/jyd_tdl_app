#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mjpeg_server {

// Minimal MJPEG-over-HTTP streaming server (multipart/x-mixed-replace).
//
// Endpoints:
//   /          tiny HTML page embedding the stream
//   /stream    the MJPEG multipart stream
//   /snapshot  a single JPEG
//
// Threading model: one accept thread plus one thread per connected client.
// The server keeps only the latest published JPEG; slow clients simply skip
// frames, so memory usage stays bounded no matter how slow a viewer is.
class MjpegServer {
 public:
  MjpegServer() = default;
  ~MjpegServer();

  MjpegServer(const MjpegServer &) = delete;
  MjpegServer &operator=(const MjpegServer &) = delete;

  bool start(int port, std::string *error = nullptr);
  void stop();
  bool running() const { return running_.load(); }

  // Copies the JPEG buffer; the caller may reuse it immediately.
  void publish(const std::uint8_t *data, std::size_t size);

 private:
  struct Client {
    int fd = -1;
    std::thread thread;
    std::atomic<bool> done{false};
  };

  void acceptLoop();
  void clientLoop(Client *client);
  void pruneFinishedClients();

  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  std::thread accept_thread_;

  std::mutex clients_mutex_;
  std::vector<std::unique_ptr<Client>> clients_;

  std::mutex frame_mutex_;
  std::condition_variable frame_cv_;
  std::vector<std::uint8_t> frame_;
  std::uint64_t frame_seq_ = 0;
};

}  // namespace mjpeg_server
