#include "controller/Controller.h"

#include <future>

#include "common/Log.h"

namespace usp {

using Clock = std::chrono::steady_clock;

const char* status_name(RequestOutcome::Status s) {
  switch (s) {
    case RequestOutcome::Status::Ok: return "ok";
    case RequestOutcome::Status::UspError: return "usp_error";
    case RequestOutcome::Status::Timeout: return "timeout";
    case RequestOutcome::Status::SendFailed: return "error";
  }
  return "error";
}

namespace {
// MQTT topics bridged by RabbitMQ are reachable from STOMP as /topic/<routing key>,
// where the MQTT '/' separator becomes '.'.
std::string mqtt_topic_to_stomp(const std::string& topic) {
  std::string key = topic;
  for (char& c : key)
    if (c == '/') c = '.';
  return "/topic/" + key;
}
}  // namespace

Controller::Controller(ControllerConfig cfg, stomp::StompClient& stomp) : cfg_(std::move(cfg)), stomp_(stomp) {
  for (const auto& kv : cfg_.static_agents) {
    AgentState a;
    a.endpoint_id = kv.first;
    a.destination = kv.second;
    agents_[kv.first] = a;
  }
}

Controller::~Controller() { stop(); }

void Controller::start() {
  if (running_.exchange(true)) return;
  stomp_.subscribe(cfg_.controller_dest);
  janitor_thread_ = std::thread([this] { janitor(); });
}

void Controller::stop() {
  if (!running_.exchange(false)) return;
  janitor_cv_.notify_all();
  if (janitor_thread_.joinable()) janitor_thread_.join();
  // Fail whatever is still waiting.
  std::map<std::string, Pending> left;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    left.swap(pending_);
  }
  for (auto& kv : left) {
    RequestOutcome o;
    o.status = RequestOutcome::Status::SendFailed;
    o.error = "controller shutting down";
    kv.second.cb(o);
  }
}

std::vector<AgentState> Controller::agents() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AgentState> out;
  for (const auto& kv : agents_) out.push_back(kv.second);
  return out;
}

std::string Controller::destination_for(const std::string& endpoint_id, std::string* version) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = agents_.find(endpoint_id);
  if (it == agents_.end()) return "";
  if (version) *version = it->second.usp_version;
  return it->second.destination;
}

bool Controller::send_msg(const std::string& endpoint_id, const Msg& msg, std::string& error) {
  std::string version;
  std::string dest = destination_for(endpoint_id, &version);
  if (dest.empty()) {
    error = "no STOMP destination known for agent '" + endpoint_id +
            "' (wait for it to connect, or configure agent.\"" + endpoint_id + "\" = /queue/...)";
    return false;
  }
  if (version.empty()) version = cfg_.usp_version;
  std::string record = make_record(version, cfg_.endpoint_id, endpoint_id, msg);
  LOG_DEBUG("USP -> " << endpoint_id << " [" << dest << "] " << to_json(msg, false));
  if (!stomp_.send(dest, record, {{"content-type", kUspContentType}, {kReplyToDestHeader, cfg_.controller_dest}})) {
    error = "not connected to the STOMP broker";
    return false;
  }
  return true;
}

void Controller::send_request(const std::string& endpoint_id, const Request& req, int timeout_ms,
                              ResponseCallback cb) {
  const std::string msg_id = new_msg_id("ctrl");
  Msg msg = make_request(msg_id, req);
  if (timeout_ms <= 0) timeout_ms = cfg_.request_timeout_ms;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_[msg_id] = Pending{endpoint_id, Clock::now() + std::chrono::milliseconds(timeout_ms), cb};
  }
  std::string error;
  if (!send_msg(endpoint_id, msg, error)) {
    bool owned = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      owned = pending_.erase(msg_id) > 0;
    }
    if (owned) {
      RequestOutcome o;
      o.status = RequestOutcome::Status::SendFailed;
      o.error = error;
      cb(o);
    }
    return;
  }
  LOG_INFO("USP -> " << endpoint_id << " " << body_kind(msg) << " msg_id=" << msg_id);
}

RequestOutcome Controller::request_sync(const std::string& endpoint_id, const Request& req, int timeout_ms) {
  auto promise = std::make_shared<std::promise<RequestOutcome>>();
  auto fut = promise->get_future();
  send_request(endpoint_id, req, timeout_ms, [promise](const RequestOutcome& o) { promise->set_value(o); });
  return fut.get();
}

void Controller::janitor() {
  while (running_) {
    {
      std::unique_lock<std::mutex> lock(janitor_mutex_);
      janitor_cv_.wait_for(lock, std::chrono::milliseconds(250));
    }
    std::vector<std::pair<std::string, Pending>> expired;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto now = Clock::now();
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.deadline <= now) {
          expired.emplace_back(it->first, std::move(it->second));
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& e : expired) {
      LOG_WARN("USP: request " << e.first << " to " << e.second.endpoint_id << " timed out");
      RequestOutcome o;
      o.status = RequestOutcome::Status::Timeout;
      o.error = "no response from agent before timeout";
      e.second.cb(o);
    }
  }
}

void Controller::handle_frame(const stomp::Frame& f) {
  auto ct = f.header("content-type");
  if (ct && ct->rfind(kUspContentType, 0) != 0) {
    LOG_WARN("USP: ignoring message with content-type '" << *ct << "'");
    return;
  }
  usp_record::Record rec;
  if (!rec.ParseFromString(f.body)) {
    LOG_WARN("USP: dropping message that is not a valid USP Record (" << f.body.size() << " bytes)");
    return;
  }
  handle_record(rec, f.header(kReplyToDestHeader).value_or(""));
}

void Controller::handle_record(const usp_record::Record& rec, const std::string& reply_to_dest) {
  const std::string& from = rec.from_id();
  if (from.empty()) {
    LOG_WARN("USP: dropping Record without from_id");
    return;
  }
  if (rec.to_id() != cfg_.endpoint_id) {
    LOG_WARN("USP: dropping Record from " << from << " addressed to '" << rec.to_id() << "', we are '"
                                          << cfg_.endpoint_id << "' (check Device.LocalAgent.Controller.{i}.EndpointID)");
    return;
  }

  bool is_new = false;
  std::string learned_dest = reply_to_dest;
  if (learned_dest.empty() && rec.has_stomp_connect()) learned_dest = rec.stomp_connect().subscribed_destination();
  if (learned_dest.empty() && rec.has_mqtt_connect() && !rec.mqtt_connect().subscribed_topic().empty())
    learned_dest = mqtt_topic_to_stomp(rec.mqtt_connect().subscribed_topic());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agents_.find(from);
    if (it == agents_.end()) {
      is_new = true;
      it = agents_.emplace(from, AgentState{}).first;
      it->second.endpoint_id = from;
    }
    AgentState& a = it->second;
    if (!learned_dest.empty() && learned_dest != a.destination) {
      if (!a.destination.empty()) LOG_INFO("USP: agent " << from << " destination changed to " << learned_dest);
      a.destination = learned_dest;
    }
    if (!rec.version().empty()) a.usp_version = rec.version();
    a.online = !rec.has_disconnect();
    a.last_seen = utc_now_iso8601();
  }
  if (is_new) LOG_INFO("USP: new agent " << from << " (USP " << rec.version() << ")");

  switch (rec.record_type_case()) {
    case usp_record::Record::kNoSessionContext: {
      if (rec.payload_security() != usp_record::Record::PLAINTEXT) {
        LOG_WARN("USP: " << from << " sent an encrypted payload (TLS12); E2E session security is not supported");
        return;
      }
      Msg msg;
      if (!msg.ParseFromString(rec.no_session_context().payload())) {
        LOG_WARN("USP: " << from << " sent an unparsable USP Message");
        return;
      }
      handle_msg(from, msg);
      return;
    }
    case usp_record::Record::kStompConnect:
      LOG_INFO("USP: agent " << from << " connected over STOMP, destination "
                             << rec.stomp_connect().subscribed_destination());
      run_boot_actions(from, "STOMP connect");
      return;
    case usp_record::Record::kMqttConnect:
      LOG_INFO("USP: agent " << from << " connected over MQTT, topic " << rec.mqtt_connect().subscribed_topic());
      run_boot_actions(from, "MQTT connect");
      return;
    case usp_record::Record::kWebsocketConnect:
    case usp_record::Record::kUdsConnect:
      LOG_INFO("USP: agent " << from << " sent a connect record");
      return;
    case usp_record::Record::kDisconnect:
      LOG_INFO("USP: agent " << from << " disconnected: " << rec.disconnect().reason() << " (code "
                             << rec.disconnect().reason_code() << ")");
      return;
    case usp_record::Record::kSessionContext:
      LOG_WARN("USP: " << from << " uses an E2E Session Context record, which this controller does not support; "
                          "configure the agent for no-session-context records");
      return;
    case usp_record::Record::RECORD_TYPE_NOT_SET:
      LOG_WARN("USP: " << from << " sent a Record without a record_type");
      return;
  }
}

void Controller::handle_msg(const std::string& endpoint_id, const Msg& msg) {
  const std::string& msg_id = msg.header().msg_id();
  LOG_DEBUG("USP <- " << endpoint_id << " " << to_json(msg, false));

  if (msg.body().has_request()) {
    if (msg.body().request().has_notify()) {
      handle_notify(endpoint_id, msg);
      return;
    }
    LOG_WARN("USP: " << endpoint_id << " sent a " << body_kind(msg) << " request; controllers do not serve requests");
    std::string error;
    send_msg(endpoint_id, make_error(msg_id, kErrMessageNotSupported, "Controller does not handle this request"),
             error);
    return;
  }

  // Response or Error: match the pending request.
  Pending p;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(msg_id);
    if (it != pending_.end()) {
      p = std::move(it->second);
      pending_.erase(it);
      found = true;
    }
  }
  if (!found) {
    LOG_WARN("USP: " << endpoint_id << " sent " << body_kind(msg) << " for unknown or expired msg_id " << msg_id);
    return;
  }
  if (p.endpoint_id != endpoint_id) {
    LOG_WARN("USP: response for msg_id " << msg_id << " came from " << endpoint_id << ", expected " << p.endpoint_id);
  }
  RequestOutcome o;
  o.response = msg;
  if (msg.body().has_error()) {
    o.status = RequestOutcome::Status::UspError;
    o.error = "USP error " + std::to_string(msg.body().error().err_code()) + ": " + msg.body().error().err_msg();
  } else {
    o.status = RequestOutcome::Status::Ok;
  }
  LOG_INFO("USP <- " << endpoint_id << " " << body_kind(msg) << " msg_id=" << msg_id);
  p.cb(o);
}

void Controller::handle_notify(const std::string& endpoint_id, const Msg& msg) {
  const Notify& n = msg.body().request().notify();
  LOG_INFO("USP <- " << endpoint_id << " Notify subscription=" << n.subscription_id() << "\n" << to_json(n));

  if (n.send_resp()) {
    Response resp;
    resp.mutable_notify_resp()->set_subscription_id(n.subscription_id());
    std::string error;
    if (!send_msg(endpoint_id, make_response(msg.header().msg_id(), resp), error))
      LOG_WARN("USP: cannot send NotifyResp to " << endpoint_id << ": " << error);
  }

  if (n.has_on_board_req()) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto& a = agents_[endpoint_id];
      a.oui = n.on_board_req().oui();
      a.product_class = n.on_board_req().product_class();
      a.serial_number = n.on_board_req().serial_number();
    }
    LOG_INFO("USP: OnBoardRequest from " << endpoint_id << " oui=" << n.on_board_req().oui()
                                         << " product_class=" << n.on_board_req().product_class()
                                         << " serial=" << n.on_board_req().serial_number() << " versions="
                                         << n.on_board_req().agent_supported_protocol_versions());
    run_boot_actions(endpoint_id, "OnBoardRequest");
  } else if (n.has_event() && n.event().event_name() == "Boot!") {
    run_boot_actions(endpoint_id, "Boot! event");
  }

  if (notify_listener_) notify_listener_(endpoint_id, n);
}

void Controller::run_boot_actions(const std::string& endpoint_id, const char* reason) {
  if (cfg_.on_boot_get.empty()) return;
  {
    // Boot!, OnBoardRequest and the connect record often arrive together: act once.
    std::lock_guard<std::mutex> lock(mutex_);
    auto& a = agents_[endpoint_id];
    auto now = Clock::now();
    if (a.last_boot_actions != Clock::time_point{} && now - a.last_boot_actions < std::chrono::seconds(10)) return;
    a.last_boot_actions = now;
  }
  LOG_INFO("USP: " << reason << " from " << endpoint_id << ", running on-boot Get");
  Request req;
  for (const auto& p : cfg_.on_boot_get) req.mutable_get()->add_param_paths(p);
  send_request(endpoint_id, req, 0, [endpoint_id](const RequestOutcome& o) {
    if (o.status == RequestOutcome::Status::Ok)
      LOG_INFO("USP: on-boot Get from " << endpoint_id << ":\n" << to_json(o.response.body().response()));
    else
      LOG_WARN("USP: on-boot Get from " << endpoint_id << " failed: " << o.error);
  });
}

}  // namespace usp
