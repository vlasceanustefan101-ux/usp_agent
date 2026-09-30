#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/Config.h"
#include "stomp/StompClient.h"
#include "usp/UspCodec.h"

namespace usp {

struct AgentState {
  std::string endpoint_id;
  std::string destination;
  std::string usp_version;
  bool online = false;
  std::string last_seen;
  std::string oui, product_class, serial_number;
  std::chrono::steady_clock::time_point last_boot_actions{};
};

struct RequestOutcome {
  enum class Status { Ok, UspError, Timeout, SendFailed };
  Status status = Status::SendFailed;
  Msg response;       // valid for Ok and UspError
  std::string error;  // human readable, for anything but Ok
};
const char* status_name(RequestOutcome::Status s);

// USP Controller over the STOMP MTP.
//  - receives USP Records on its own destination (config.controller_dest),
//  - tracks Agents and the STOMP destination to reach each of them,
//  - answers Notify requests with NotifyResp when send_resp is set,
//  - sends requests to Agents and matches responses by msg_id.
class Controller {
 public:
  using ResponseCallback = std::function<void(const RequestOutcome&)>;
  using NotifyListener = std::function<void(const std::string& endpoint_id, const Notify&)>;

  Controller(ControllerConfig cfg, stomp::StompClient& stomp);
  ~Controller();

  void start();
  void stop();

  // Entry point for MESSAGE frames on the controller destination (I/O thread).
  void handle_frame(const stomp::Frame& f);

  // Sends `req` to the Agent. `cb` is invoked exactly once, on the I/O thread
  // (response) or on the timeout thread. `timeout_ms <= 0` uses the default.
  void send_request(const std::string& endpoint_id, const Request& req, int timeout_ms, ResponseCallback cb);
  // Blocking variant. Must NOT be called from the I/O thread (i.e. from a callback).
  RequestOutcome request_sync(const std::string& endpoint_id, const Request& req, int timeout_ms = 0);

  std::vector<AgentState> agents() const;
  const ControllerConfig& config() const { return cfg_; }
  void set_notify_listener(NotifyListener l) { notify_listener_ = std::move(l); }

 private:
  struct Pending {
    std::string endpoint_id;
    std::chrono::steady_clock::time_point deadline;
    ResponseCallback cb;
  };

  void handle_record(const usp_record::Record& rec, const std::string& reply_to_dest);
  void handle_msg(const std::string& endpoint_id, const Msg& msg);
  void handle_notify(const std::string& endpoint_id, const Msg& msg);
  bool send_msg(const std::string& endpoint_id, const Msg& msg, std::string& error);
  void run_boot_actions(const std::string& endpoint_id, const char* reason);
  std::string destination_for(const std::string& endpoint_id, std::string* version) const;
  void janitor();

  ControllerConfig cfg_;
  stomp::StompClient& stomp_;
  NotifyListener notify_listener_;

  mutable std::mutex mutex_;  // guards agents_ and pending_
  std::map<std::string, AgentState> agents_;
  std::map<std::string, Pending> pending_;  // msg_id -> pending request

  std::atomic<bool> running_{false};
  std::mutex janitor_mutex_;
  std::condition_variable janitor_cv_;
  std::thread janitor_thread_;
};

}  // namespace usp
