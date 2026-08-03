#include "mjpeg_server.hpp"

#include <cstring>
#include <chrono>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mjpeg_server {
namespace {

constexpr const char *kBoundary = "mjpegframe";

// Send timeout: a viewer stalled longer than this is dropped (it can simply
// reconnect), which keeps one bad client from pinning a server thread.
constexpr int kSendTimeoutSec = 3;

const char kIndexHtml[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>sophpi AI stream</title><style>"
    "body{margin:0;background:#111;color:#eee;font-family:sans-serif;"
    "display:flex;flex-direction:column;align-items:center;"
    "justify-content:center;min-height:100vh}"
    "img{max-width:100vw;max-height:90vh}"
    "p{margin:8px 0 0;font-size:12px;color:#888}"
    "</style></head><body>"
    "<img src=\"/stream\" alt=\"stream\">"
    "<p>sophpi_ai_osd_demo &middot; MJPEG stream &middot; "
    "<a href=\"/snapshot\" style=\"color:#6af\">snapshot</a></p>"
    "</body></html>";

bool sendAll(int fd, const void *data, std::size_t size) {
  const auto *ptr = static_cast<const std::uint8_t *>(data);
  std::size_t sent = 0;
  while (sent < size) {
    const ssize_t n = ::send(fd, ptr + sent, size - sent, MSG_NOSIGNAL);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

bool sendString(int fd, const std::string &text) {
  return sendAll(fd, text.data(), text.size());
}

}  // namespace

MjpegServer::~MjpegServer() { stop(); }

bool MjpegServer::start(int port, std::string *error) {
  if (running_.load()) {
    return true;
  }

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    if (error) *error = "socket() failed";
    return false;
  }
  const int reuse = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
      0) {
    if (error) {
      *error = "bind() failed on port " + std::to_string(port) +
               " (port already in use?)";
    }
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 4) != 0) {
    if (error) *error = "listen() failed";
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  running_.store(true);
  accept_thread_ = std::thread([this]() { acceptLoop(); });
  return true;
}

void MjpegServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  // Unblock accept() and all client sends, then join everything.
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  frame_cv_.notify_all();
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  std::lock_guard<std::mutex> lock(clients_mutex_);
  for (auto &client : clients_) {
    if (client->fd >= 0) {
      ::shutdown(client->fd, SHUT_RDWR);
    }
    if (client->thread.joinable()) {
      client->thread.join();
    }
    if (client->fd >= 0) {
      ::close(client->fd);
      client->fd = -1;
    }
  }
  clients_.clear();

  std::lock_guard<std::mutex> frame_lock(frame_mutex_);
  frame_.clear();
  frame_seq_ = 0;
}

void MjpegServer::publish(const std::uint8_t *data, std::size_t size) {
  if (!running_.load() || !data || size == 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    frame_.assign(data, data + size);
    ++frame_seq_;
  }
  frame_cv_.notify_all();
}

void MjpegServer::pruneFinishedClients() {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  for (auto it = clients_.begin(); it != clients_.end();) {
    if ((*it)->done.load()) {
      if ((*it)->thread.joinable()) {
        (*it)->thread.join();
      }
      if ((*it)->fd >= 0) {
        ::close((*it)->fd);
      }
      it = clients_.erase(it);
    } else {
      ++it;
    }
  }
}

void MjpegServer::acceptLoop() {
  while (running_.load()) {
    sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr *>(&peer),
                            &peer_len);
    if (fd < 0) {
      // listen_fd_ was shut down by stop(), or a transient error.
      if (!running_.load()) {
        break;
      }
      continue;
    }

    timeval timeout;
    timeout.tv_sec = kSendTimeoutSec;
    timeout.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const int nodelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    pruneFinishedClients();

    auto client = std::unique_ptr<Client>(new Client());
    client->fd = fd;
    Client *raw = client.get();
    client->thread = std::thread([this, raw]() { clientLoop(raw); });
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.push_back(std::move(client));
  }
}

void MjpegServer::clientLoop(Client *client) {
  const int fd = client->fd;

  char request[1024] = {0};
  const ssize_t got = ::recv(fd, request, sizeof(request) - 1, 0);
  if (got <= 0) {
    client->done.store(true);
    return;
  }

  std::string path = "/";
  {
    const std::string text(request, static_cast<std::size_t>(got));
    if (text.compare(0, 4, "GET ") == 0) {
      const auto end = text.find(' ', 4);
      if (end != std::string::npos) {
        path = text.substr(4, end - 4);
      }
    }
  }

  if (path == "/" || path == "/index.html") {
    std::string response =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: " +
        std::to_string(sizeof(kIndexHtml) - 1) +
        "\r\n"
        "Connection: close\r\n\r\n";
    response += kIndexHtml;
    sendString(fd, response);
    client->done.store(true);
    return;
  }

  if (path == "/snapshot") {
    std::vector<std::uint8_t> jpeg;
    {
      std::unique_lock<std::mutex> lock(frame_mutex_);
      frame_cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
        return !running_.load() || frame_seq_ > 0;
      });
      jpeg = frame_;
    }
    if (jpeg.empty()) {
      sendString(fd,
                 "HTTP/1.0 503 Service Unavailable\r\n"
                 "Content-Length: 0\r\nConnection: close\r\n\r\n");
    } else {
      const std::string header =
          "HTTP/1.0 200 OK\r\n"
          "Content-Type: image/jpeg\r\n"
          "Content-Length: " +
          std::to_string(jpeg.size()) +
          "\r\n"
          "Connection: close\r\n\r\n";
      if (sendString(fd, header)) {
        sendAll(fd, jpeg.data(), jpeg.size());
      }
    }
    client->done.store(true);
    return;
  }

  if (path != "/stream") {
    sendString(fd,
               "HTTP/1.0 404 Not Found\r\n"
               "Content-Length: 0\r\nConnection: close\r\n\r\n");
    client->done.store(true);
    return;
  }

  const std::string header =
      "HTTP/1.0 200 OK\r\n"
      "Server: sophpi-ai-osd\r\n"
      "Cache-Control: no-cache\r\n"
      "Pragma: no-cache\r\n"
      "Connection: close\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=" +
      std::string(kBoundary) + "\r\n\r\n";
  if (!sendString(fd, header)) {
    client->done.store(true);
    return;
  }

  std::vector<std::uint8_t> jpeg;
  std::uint64_t last_seq = 0;
  while (running_.load()) {
    {
      std::unique_lock<std::mutex> lock(frame_mutex_);
      frame_cv_.wait_for(lock, std::chrono::milliseconds(200), [&]() {
        return !running_.load() || frame_seq_ != last_seq;
      });
      if (!running_.load()) {
        break;
      }
      if (frame_seq_ == last_seq || frame_.empty()) {
        continue;
      }
      last_seq = frame_seq_;
      jpeg = frame_;
    }

    const std::string part =
        "--" + std::string(kBoundary) +
        "\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: " +
        std::to_string(jpeg.size()) + "\r\n\r\n";
    if (!sendString(fd, part) || !sendAll(fd, jpeg.data(), jpeg.size()) ||
        !sendString(fd, "\r\n")) {
      break;
    }
  }
  client->done.store(true);
}

}  // namespace mjpeg_server
