#include "stomp/StompClient.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>

#include "common/Log.h"
#include "stomp/Transport.h"

namespace usp::stomp {

using Clock = std::chrono::steady_clock;

StompClient::StompClient(StompOptions opts) : opts_(std::move(opts)) {
  if (::pipe2(wake_pipe_, O_CLOEXEC | O_NONBLOCK) != 0) {
    LOG_ERROR("pipe2 failed; STOMP client cannot run");
  }
}

StompClient::~StompClient() {
  stop();
  if (wake_pipe_[0] >= 0) ::close(wake_pipe_[0]);
  if (wake_pipe_[1] >= 0) ::close(wake_pipe_[1]);
}

void StompClient::subscribe(const std::string& destination) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (subscriptions_.count(destination)) return;
  subscriptions_[destination] = "sub-" + std::to_string(next_sub_id_++);
  if (connected_) {
    Frame f{"SUBSCRIBE", {{"id", subscriptions_[destination]}, {"destination", destination}, {"ack", "auto"}}, ""};
    outbox_.push_back(encode_frame(f));
    wake();
  }
}

std::string StompClient::subscription_id(const std::string& destination) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = subscriptions_.find(destination);
  return it == subscriptions_.end() ? "" : it->second;
}

void StompClient::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void StompClient::stop() {
  if (!running_.exchange(false)) return;
  wake();
  if (thread_.joinable()) thread_.join();
}

void StompClient::wake() {
  char c = 1;
  if (wake_pipe_[1] >= 0) {
    ssize_t r = ::write(wake_pipe_[1], &c, 1);
    (void)r;  // EAGAIN means a wake-up is already pending
  }
}

bool StompClient::send(const std::string& destination, const std::string& body,
                       const std::vector<std::pair<std::string, std::string>>& extra_headers) {
  Frame f;
  f.command = "SEND";
  f.headers.emplace_back("destination", destination);
  for (const auto& h : extra_headers) f.headers.push_back(h);
  f.headers.emplace_back("content-length", std::to_string(body.size()));
  f.body = body;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) return false;
    outbox_.push_back(encode_frame(f));
  }
  wake();
  return true;
}

void StompClient::run() {
  int backoff_ms = 1000;
  while (running_) {
    if (session(backoff_ms)) backoff_ms = 1000;
    if (!running_) break;
    // Jittered exponential backoff, interruptible by stop().
    int delay = backoff_ms / 2 + std::rand() % (backoff_ms / 2 + 1);
    LOG_INFO("STOMP: reconnecting in " << delay << " ms");
    auto until = Clock::now() + std::chrono::milliseconds(delay);
    while (running_ && Clock::now() < until) {
      pollfd p{wake_pipe_[0], POLLIN, 0};
      ::poll(&p, 1, 200);
      char drain[64];
      while (::read(wake_pipe_[0], drain, sizeof(drain)) > 0) {}
    }
    backoff_ms = std::min(backoff_ms * 2, std::max(1000, opts_.reconnect_max_ms));
  }
}

bool StompClient::session(int& backoff_ms) {
  (void)backoff_ms;
  std::string error;
  LOG_INFO("STOMP: connecting to " << opts_.host << ":" << opts_.port << (opts_.tls ? " (TLS)" : "")
                                   << " vhost=" << opts_.vhost);
  auto transport = connect_transport(opts_, error);
  if (!transport) {
    LOG_WARN("STOMP: " << error);
    return false;
  }

  const int hb = std::max(0, opts_.heartbeat_ms);
  Frame connect{"CONNECT",
                {{"accept-version", "1.2"},
                 {"host", opts_.vhost},
                 {"login", opts_.username},
                 {"passcode", opts_.password},
                 {"heart-beat", std::to_string(hb) + "," + std::to_string(hb)}},
                ""};
  std::string wire = encode_frame(connect);
  if (!transport->write_all(wire.data(), wire.size())) {
    LOG_WARN("STOMP: failed to send CONNECT");
    return false;
  }

  FrameDecoder decoder;
  char buf[65536];
  long send_interval_ms = 0, recv_interval_ms = 0;
  auto last_sent = Clock::now();
  auto last_recv = Clock::now();
  const auto connect_deadline = Clock::now() + std::chrono::seconds(15);
  bool got_connected = false;

  auto cleanup = [&]() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    outbox_.clear();
  };

  while (running_) {
    // Flush queued frames.
    if (got_connected) {
      std::deque<std::string> pending;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(outbox_);
      }
      for (const auto& data : pending) {
        if (!transport->write_all(data.data(), data.size())) {
          LOG_WARN("STOMP: write failed, connection lost");
          cleanup();
          return true;
        }
        last_sent = Clock::now();
      }
    }

    int timeout_ms = 1000;
    if (!transport->has_pending()) {
      pollfd fds[2] = {{transport->fd(), POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
      int rc = ::poll(fds, 2, timeout_ms);
      if (rc < 0 && errno != EINTR) {
        LOG_WARN("STOMP: poll failed");
        cleanup();
        return got_connected;
      }
      if (fds[1].revents & POLLIN) {
        char drain[64];
        while (::read(wake_pipe_[0], drain, sizeof(drain)) > 0) {}
      }
      if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) {
        auto now = Clock::now();
        if (!got_connected && now > connect_deadline) {
          LOG_WARN("STOMP: no CONNECTED frame from broker");
          cleanup();
          return false;
        }
        if (got_connected && send_interval_ms > 0 &&
            now - last_sent >= std::chrono::milliseconds(send_interval_ms)) {
          if (!transport->write_all("\n", 1)) {
            LOG_WARN("STOMP: heart-beat write failed");
            cleanup();
            return true;
          }
          last_sent = now;
        }
        if (got_connected && recv_interval_ms > 0 &&
            now - last_recv > std::chrono::milliseconds(recv_interval_ms * 2 + 1000)) {
          LOG_WARN("STOMP: broker heart-beat timeout");
          cleanup();
          return true;
        }
        continue;
      }
    }

    long n = transport->read(buf, sizeof(buf));
    if (n <= 0) {
      LOG_WARN("STOMP: connection closed by broker");
      cleanup();
      return got_connected;
    }
    last_recv = Clock::now();
    decoder.feed(buf, static_cast<size_t>(n));

    for (;;) {
      Frame f;
      auto r = decoder.next(f, error);
      if (r == FrameDecoder::Result::NeedMore) break;
      if (r == FrameDecoder::Result::HeartBeat) continue;
      if (r == FrameDecoder::Result::Error) {
        LOG_ERROR("STOMP: protocol error: " << error);
        cleanup();
        return got_connected;
      }
      if (f.command == "CONNECTED") {
        long sx = 0, sy = 0;
        if (auto h = f.header("heart-beat")) {
          sx = std::atol(h->c_str());
          auto comma = h->find(',');
          if (comma != std::string::npos) sy = std::atol(h->c_str() + comma + 1);
        }
        send_interval_ms = (hb > 0 && sy > 0) ? std::max<long>(hb, sy) : 0;
        recv_interval_ms = (hb > 0 && sx > 0) ? std::max<long>(hb, sx) : 0;
        LOG_INFO("STOMP: connected (server=" << f.header("server").value_or("?")
                                             << ", heart-beat send=" << send_interval_ms
                                             << "ms recv=" << recv_interval_ms << "ms)");
        {
          std::lock_guard<std::mutex> lock(mutex_);
          for (const auto& s : subscriptions_) {
            Frame sub{"SUBSCRIBE", {{"id", s.second}, {"destination", s.first}, {"ack", "auto"}}, ""};
            std::string w = encode_frame(sub);
            if (!transport->write_all(w.data(), w.size())) {
              connected_ = false;
              return true;
            }
            LOG_INFO("STOMP: subscribed to " << s.first);
          }
          connected_ = true;
        }
        got_connected = true;
        last_sent = Clock::now();
        if (connected_handler_) connected_handler_();
      } else if (f.command == "MESSAGE") {
        if (message_handler_) {
          try {
            message_handler_(f);
          } catch (const std::exception& e) {
            LOG_ERROR("STOMP: message handler threw: " << e.what());
          }
        }
      } else if (f.command == "ERROR") {
        LOG_ERROR("STOMP: broker ERROR: " << f.header("message").value_or("") << " "
                                          << f.body.substr(0, 512));
        cleanup();
        return got_connected;
      } else if (f.command == "RECEIPT") {
        LOG_DEBUG("STOMP: receipt " << f.header("receipt-id").value_or(""));
      } else {
        LOG_WARN("STOMP: ignoring unexpected frame " << f.command);
      }
    }
  }
  // Graceful shutdown.
  Frame disc{"DISCONNECT", {}, ""};
  std::string w = encode_frame(disc);
  transport->write_all(w.data(), w.size());
  cleanup();
  return got_connected;
}

}  // namespace usp::stomp
