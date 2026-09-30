#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "common/Config.h"
#include "stomp/StompFrame.h"

namespace usp::stomp {

// STOMP 1.2 client with automatic reconnect and heart-beating.
// All socket I/O happens on one internal thread; send() only queues frames,
// so it is safe to call from any thread, including from the message handler.
class StompClient {
 public:
  using MessageHandler = std::function<void(const Frame&)>;
  using ConnectedHandler = std::function<void()>;

  explicit StompClient(StompOptions opts);
  ~StompClient();

  StompClient(const StompClient&) = delete;
  StompClient& operator=(const StompClient&) = delete;

  // Handlers must be set before start(). They run on the I/O thread.
  void on_message(MessageHandler h) { message_handler_ = std::move(h); }
  void on_connected(ConnectedHandler h) { connected_handler_ = std::move(h); }

  // Registers a subscription; (re)sent on every connect.
  void subscribe(const std::string& destination);
  // STOMP subscription id used for `destination` ("" if not subscribed). MESSAGE
  // frames carry it in the "subscription" header.
  std::string subscription_id(const std::string& destination);

  void start();
  void stop();

  bool connected() const { return connected_; }

  // Queues a SEND frame. Returns false if not connected (the frame is dropped).
  bool send(const std::string& destination, const std::string& body,
            const std::vector<std::pair<std::string, std::string>>& extra_headers = {});

 private:
  void run();
  bool session(int& backoff_ms);  // one connection lifetime; returns true if it got CONNECTED
  void wake();

  StompOptions opts_;
  MessageHandler message_handler_;
  ConnectedHandler connected_handler_;

  std::mutex mutex_;  // guards outbox_, subscriptions_
  std::deque<std::string> outbox_;
  std::map<std::string, std::string> subscriptions_;  // destination -> subscription id
  int next_sub_id_ = 0;

  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::thread thread_;
  int wake_pipe_[2] = {-1, -1};
};

}  // namespace usp::stomp
