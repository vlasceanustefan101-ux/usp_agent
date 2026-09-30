// Minimal simulated USP Agent over STOMP, for testing the controller without a
// real device. It keeps a tiny in-memory data model and answers Get, Set, Add,
// Operate and GetSupportedProtocol. It is a test tool, not a TR-181 implementation.
#include <atomic>
#include <csignal>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#include "common/Config.h"
#include "common/Log.h"
#include "stomp/StompClient.h"
#include "usp/UspCodec.h"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

constexpr unsigned kErrInvalidPath = 7026;

struct Agent {
  std::string endpoint_id = "proto::fake-agent-1";
  std::string dest = "/queue/usp.agent.fake-agent-1";
  std::string controller_id = "proto::usp-controller";
  std::string controller_dest = "/queue/usp.controller";
  std::string version = "1.3";
  usp::stomp::StompClient* stomp = nullptr;

  std::mutex mutex;
  std::map<std::string, std::string> dm = {
      {"Device.DeviceInfo.Manufacturer", "FakeVendor"},
      {"Device.DeviceInfo.ManufacturerOUI", "000000"},
      {"Device.DeviceInfo.ModelName", "FakeAgent"},
      {"Device.DeviceInfo.ProductClass", "Simulator"},
      {"Device.DeviceInfo.SerialNumber", "FAKE0001"},
      {"Device.DeviceInfo.SoftwareVersion", "0.1.0"},
      {"Device.WiFi.SSID.1.Enable", "true"},
      {"Device.WiFi.SSID.1.SSID", "FakeNet"},
  };
  int next_sub = 1;

  void send(const usp::Msg& msg) {
    stomp->send(controller_dest, usp::make_record(version, endpoint_id, controller_id, msg),
                {{"content-type", usp::kUspContentType}, {usp::kReplyToDestHeader, dest}});
  }

  static std::string object_of(const std::string& param) { return param.substr(0, param.rfind('.') + 1); }

  usp::Response do_get(const usp::Get& get) {
    usp::Response resp;
    auto* gr = resp.mutable_get_resp();
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& path : get.param_paths()) {
      auto* rpr = gr->add_req_path_results();
      rpr->set_requested_path(path);
      std::map<std::string, usp::GetResp::ResolvedPathResult*> by_obj;
      for (const auto& kv : dm) {
        bool match = path.back() == '.' ? kv.first.rfind(path, 0) == 0 : kv.first == path;
        if (!match) continue;
        std::string obj = object_of(kv.first);
        auto& res = by_obj[obj];
        if (res == nullptr) {
          res = rpr->add_resolved_path_results();
          res->set_resolved_path(obj);
        }
        (*res->mutable_result_params())[kv.first.substr(obj.size())] = kv.second;
      }
      if (by_obj.empty()) {
        rpr->set_err_code(kErrInvalidPath);
        rpr->set_err_msg("Invalid path: " + path);
      }
    }
    return resp;
  }

  usp::Msg do_set(const std::string& msg_id, const usp::Set& set) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& obj : set.update_objs())
      for (const auto& ps : obj.param_settings())
        if (!dm.count(obj.obj_path() + ps.param()))
          return usp::make_error(msg_id, kErrInvalidPath, "Invalid path: " + obj.obj_path() + ps.param());
    usp::Response resp;
    for (const auto& obj : set.update_objs()) {
      auto* r = resp.mutable_set_resp()->add_updated_obj_results();
      r->set_requested_path(obj.obj_path());
      auto* inst = r->mutable_oper_status()->mutable_oper_success()->add_updated_inst_results();
      inst->set_affected_path(obj.obj_path());
      for (const auto& ps : obj.param_settings()) {
        dm[obj.obj_path() + ps.param()] = ps.value();
        (*inst->mutable_updated_params())[ps.param()] = ps.value();
      }
    }
    return usp::make_response(msg_id, resp);
  }

  usp::Msg do_add(const std::string& msg_id, const usp::Add& add) {
    usp::Response resp;
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& obj : add.create_objs()) {
      auto* r = resp.mutable_add_resp()->add_created_obj_results();
      r->set_requested_path(obj.obj_path());
      if (obj.obj_path() != "Device.LocalAgent.Subscription.") {
        if (!add.allow_partial()) return usp::make_error(msg_id, kErrInvalidPath, "Only subscriptions can be added");
        r->mutable_oper_status()->mutable_oper_failure()->set_err_code(kErrInvalidPath);
        continue;
      }
      std::string inst = obj.obj_path() + std::to_string(next_sub++) + ".";
      for (const auto& ps : obj.param_settings()) dm[inst + ps.param()] = ps.value();
      auto* ok = r->mutable_oper_status()->mutable_oper_success();
      ok->set_instantiated_path(inst);
      if (dm.count(inst + "ID")) (*ok->mutable_unique_keys())["ID"] = dm[inst + "ID"];
    }
    return usp::make_response(msg_id, resp);
  }

  void handle(const usp::stomp::Frame& f) {
    usp_record::Record rec;
    usp::Msg msg;
    if (!rec.ParseFromString(f.body) || !rec.has_no_session_context() ||
        !msg.ParseFromString(rec.no_session_context().payload())) {
      LOG_WARN("fake-agent: ignoring unparsable record");
      return;
    }
    if (rec.to_id() != endpoint_id) {
      LOG_WARN("fake-agent: record for " << rec.to_id() << " ignored");
      return;
    }
    const std::string& id = msg.header().msg_id();
    LOG_INFO("fake-agent <- " << usp::body_kind(msg) << " msg_id=" << id);
    if (!msg.body().has_request()) return;  // e.g. NotifyResp
    const usp::Request& req = msg.body().request();
    usp::Response resp;
    switch (req.req_type_case()) {
      case usp::Request::kGet:
        send(usp::make_response(id, do_get(req.get())));
        return;
      case usp::Request::kSet:
        send(do_set(id, req.set()));
        return;
      case usp::Request::kAdd:
        send(do_add(id, req.add()));
        return;
      case usp::Request::kGetSupportedProtocol:
        resp.mutable_get_supported_protocol_resp()->set_agent_supported_protocol_versions("1.0,1.1,1.2,1.3,1.4");
        send(usp::make_response(id, resp));
        return;
      case usp::Request::kOperate: {
        auto* r = resp.mutable_operate_resp()->add_operation_results();
        r->set_executed_command(req.operate().command());
        if (req.operate().command() == "Device.Reboot()") {
          r->mutable_req_output_args();
        } else {
          r->mutable_cmd_failure()->set_err_code(7022);
          r->mutable_cmd_failure()->set_err_msg("Command not supported by fake agent");
        }
        send(usp::make_response(id, resp));
        return;
      }
      default:
        send(usp::make_error(id, usp::kErrMessageNotSupported, "Not supported by fake agent"));
    }
  }

  void on_connected() {
    stomp->send(controller_dest, usp::make_stomp_connect_record(version, endpoint_id, controller_id, dest),
                {{"content-type", usp::kUspContentType}, {usp::kReplyToDestHeader, dest}});
    usp::Request req;
    auto* n = req.mutable_notify();
    n->set_subscription_id("boot");
    n->set_send_resp(true);
    auto* ev = n->mutable_event();
    ev->set_obj_path("Device.");
    ev->set_event_name("Boot!");
    (*ev->mutable_params())["Cause"] = "LocalReboot";
    (*ev->mutable_params())["FirmwareUpdated"] = "false";
    send(usp::make_request(usp::new_msg_id("agent"), req));
    LOG_INFO("fake-agent: sent STOMPConnect record and Boot! notification");
  }
};

}  // namespace

int main(int argc, char** argv) {
  usp::ControllerConfig cfg;  // reused only for STOMP options / USP_STOMP_* env
  std::string error;
  if (!usp::apply_env_overrides(cfg, error)) {
    std::cerr << error << std::endl;
    return 2;
  }
  Agent agent;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string k = argv[i], v = argv[i + 1];
    if (k == "--endpoint") agent.endpoint_id = v;
    else if (k == "--dest") agent.dest = v;
    else if (k == "--controller") agent.controller_id = v;
    else if (k == "--controller-dest") agent.controller_dest = v;
    else if (k == "--host") cfg.stomp.host = v;
    else if (k == "--port") cfg.stomp.port = std::stoi(v);
    else if (k == "--user") cfg.stomp.username = v;
    else if (k == "--pass") cfg.stomp.password = v;
    else if (k == "--vhost") cfg.stomp.vhost = v;
    else {
      std::cerr << "unknown option " << k << "\n"
                << "options: --endpoint --dest --controller --controller-dest --host --port --user --pass --vhost\n";
      return 2;
    }
  }
  if ((argc - 1) % 2 != 0) {
    std::cerr << "options take one value each" << std::endl;
    return 2;
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  usp::stomp::StompClient stomp(cfg.stomp);
  agent.stomp = &stomp;
  stomp.subscribe(agent.dest);
  stomp.on_message([&](const usp::stomp::Frame& f) { agent.handle(f); });
  stomp.on_connected([&] { agent.on_connected(); });
  stomp.start();
  LOG_INFO("fake-agent " << agent.endpoint_id << " on " << agent.dest << " -> controller " << agent.controller_id);
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stomp.stop();
  return 0;
}
