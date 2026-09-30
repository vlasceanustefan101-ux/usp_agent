#include "controller/Cli.h"

#include <poll.h>
#include <unistd.h>

#include <iostream>

#include "common/Log.h"

namespace usp {

std::vector<std::string> Cli::tokenize(const std::string& line, std::string& error) {
  std::vector<std::string> out;
  std::string cur;
  bool in_token = false, in_quotes = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_quotes) {
      if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
        cur += line[++i];
      } else if (c == '"') {
        in_quotes = false;
      } else {
        cur += c;
      }
    } else if (c == '"') {
      in_quotes = true;
      in_token = true;
    } else if (c == ' ' || c == '\t') {
      if (in_token) {
        out.push_back(cur);
        cur.clear();
        in_token = false;
      }
    } else {
      cur += c;
      in_token = true;
    }
  }
  if (in_quotes) {
    error = "unterminated quote";
    return {};
  }
  if (in_token) out.push_back(cur);
  return out;
}

void Cli::print_help() {
  std::cout <<
      R"(Commands (the agent is the one selected with 'use', or the only known agent):
  agents                               list known agents
  use <endpoint_id>                    select the agent to talk to
  get <path> [path...]                 Get, e.g. get Device.DeviceInfo.
  set <obj_path> <param>=<value>...    Set, e.g. set Device.WiFi.SSID.1. SSID="Test Net"
  add <obj_path> [param=value...]      Add an object instance
  del <obj_path> [obj_path...]         Delete object instances
  op <command> [arg=value...]          Operate, e.g. op Device.Reboot()
  inst <obj_path> [obj_path...]        GetInstances
  gsdm <obj_path> [obj_path...]        GetSupportedDM (first level only: add -1)
  proto                                GetSupportedProtocol
  sub <NotifType> <ReferenceList>      create a subscription, e.g. sub Event Device.Boot!
                                         NotifType: ValueChange ObjectCreation ObjectDeletion
                                                    OperationComplete Event
  raw <json usp.Request>               send any request, e.g. raw {"get":{"param_paths":["Device."]}}
  loglevel <debug|info|warn|error>
  help | quit
)";
}

std::string Cli::resolve_agent() {
  if (!current_agent_.empty()) return current_agent_;
  auto agents = controller_.agents();
  if (agents.size() == 1) return agents.front().endpoint_id;
  return "";
}

namespace {

bool split_kv(const std::string& s, std::string& k, std::string& v) {
  auto eq = s.find('=');
  if (eq == std::string::npos || eq == 0) return false;
  k = s.substr(0, eq);
  v = s.substr(eq + 1);
  return true;
}

void print_outcome(const RequestOutcome& o) {
  if (o.status == RequestOutcome::Status::Ok) {
    std::cout << to_json(o.response.body().response()) << std::endl;
  } else if (o.status == RequestOutcome::Status::UspError) {
    std::cout << "USP Error:\n" << to_json(o.response.body().error()) << std::endl;
  } else {
    std::cout << status_name(o.status) << ": " << o.error << std::endl;
  }
}

}  // namespace

bool Cli::execute(const std::string& line) {
  std::string error;
  auto args = tokenize(line, error);
  if (!error.empty()) {
    std::cout << "error: " << error << std::endl;
    return true;
  }
  if (args.empty()) return true;
  const std::string cmd = args[0];

  if (cmd == "quit" || cmd == "exit") return false;
  if (cmd == "help" || cmd == "?") {
    print_help();
    return true;
  }
  if (cmd == "agents") {
    auto agents = controller_.agents();
    if (agents.empty()) std::cout << "(no agents yet)" << std::endl;
    for (const auto& a : agents) {
      std::cout << (a.endpoint_id == resolve_agent() ? "* " : "  ") << a.endpoint_id
                << "  dest=" << (a.destination.empty() ? "?" : a.destination)
                << "  usp=" << (a.usp_version.empty() ? "?" : a.usp_version) << "  "
                << (a.online ? "online" : "offline") << "  last_seen=" << a.last_seen;
      if (!a.serial_number.empty()) std::cout << "  serial=" << a.serial_number;
      std::cout << std::endl;
    }
    return true;
  }
  if (cmd == "use") {
    if (args.size() != 2) {
      std::cout << "usage: use <endpoint_id>" << std::endl;
      return true;
    }
    current_agent_ = args[1];
    std::cout << "using agent " << current_agent_ << std::endl;
    return true;
  }
  if (cmd == "loglevel" && args.size() == 2) {
    set_log_level(parse_log_level(args[1]));
    return true;
  }

  Request req;
  if (cmd == "get" && args.size() >= 2) {
    for (size_t i = 1; i < args.size(); ++i) req.mutable_get()->add_param_paths(args[i]);
  } else if (cmd == "set" && args.size() >= 3) {
    auto* set = req.mutable_set();
    set->set_allow_partial(false);
    auto* obj = set->add_update_objs();
    obj->set_obj_path(args[1]);
    for (size_t i = 2; i < args.size(); ++i) {
      std::string k, v;
      if (!split_kv(args[i], k, v)) {
        std::cout << "expected param=value, got '" << args[i] << "'" << std::endl;
        return true;
      }
      auto* ps = obj->add_param_settings();
      ps->set_param(k);
      ps->set_value(v);
      ps->set_required(true);
    }
  } else if (cmd == "add" && args.size() >= 2) {
    auto* add = req.mutable_add();
    add->set_allow_partial(false);
    auto* obj = add->add_create_objs();
    obj->set_obj_path(args[1]);
    for (size_t i = 2; i < args.size(); ++i) {
      std::string k, v;
      if (!split_kv(args[i], k, v)) {
        std::cout << "expected param=value, got '" << args[i] << "'" << std::endl;
        return true;
      }
      auto* ps = obj->add_param_settings();
      ps->set_param(k);
      ps->set_value(v);
      ps->set_required(true);
    }
  } else if (cmd == "del" && args.size() >= 2) {
    req.mutable_delete_()->set_allow_partial(false);
    for (size_t i = 1; i < args.size(); ++i) req.mutable_delete_()->add_obj_paths(args[i]);
  } else if (cmd == "op" && args.size() >= 2) {
    auto* op = req.mutable_operate();
    op->set_command(args[1]);
    op->set_command_key(new_msg_id("cli-op"));
    op->set_send_resp(true);
    for (size_t i = 2; i < args.size(); ++i) {
      std::string k, v;
      if (!split_kv(args[i], k, v)) {
        std::cout << "expected arg=value, got '" << args[i] << "'" << std::endl;
        return true;
      }
      (*op->mutable_input_args())[k] = v;
    }
  } else if (cmd == "inst" && args.size() >= 2) {
    for (size_t i = 1; i < args.size(); ++i) req.mutable_get_instances()->add_obj_paths(args[i]);
    req.mutable_get_instances()->set_first_level_only(false);
  } else if (cmd == "gsdm" && args.size() >= 2) {
    auto* g = req.mutable_get_supported_dm();
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i] == "-1") g->set_first_level_only(true);
      else g->add_obj_paths(args[i]);
    }
    g->set_return_commands(true);
    g->set_return_events(true);
    g->set_return_params(true);
    g->set_return_unique_key_sets(true);
  } else if (cmd == "proto") {
    req.mutable_get_supported_protocol()->set_controller_supported_protocol_versions("1.0,1.1,1.2,1.3,1.4");
  } else if (cmd == "sub" && args.size() == 3) {
    auto* obj = req.mutable_add()->add_create_objs();
    obj->set_obj_path("Device.LocalAgent.Subscription.");
    const std::pair<const char*, std::string> params[] = {{"Enable", "true"},
                                                          {"ID", new_msg_id("sub")},
                                                          {"NotifType", args[1]},
                                                          {"ReferenceList", args[2]},
                                                          {"Persistent", "true"}};
    for (const auto& p : params) {
      auto* ps = obj->add_param_settings();
      ps->set_param(p.first);
      ps->set_value(p.second);
      ps->set_required(true);
    }
  } else if (cmd == "raw" && args.size() >= 2) {
    std::string json = line.substr(line.find("raw") + 3);
    if (!from_json(json, req, error)) {
      std::cout << "invalid usp.Request JSON: " << error << std::endl;
      return true;
    }
  } else {
    std::cout << "unknown command or wrong arguments; type 'help'" << std::endl;
    return true;
  }

  const std::string agent = resolve_agent();
  if (agent.empty()) {
    std::cout << "no agent selected: use 'agents' and 'use <endpoint_id>'" << std::endl;
    return true;
  }
  print_outcome(controller_.request_sync(agent, req));
  return true;
}

void Cli::run(const std::atomic<bool>& stop) {
  std::cout << "USP controller console. Type 'help' for commands." << std::endl;
  std::string line;
  while (!stop) {
    std::cout << "usp> " << std::flush;
    // Wait for input while still reacting to shutdown requests.
    for (;;) {
      if (stop) return;
      pollfd p{STDIN_FILENO, POLLIN, 0};
      int rc = ::poll(&p, 1, 500);
      if (rc > 0) break;
    }
    if (!std::getline(std::cin, line)) return;
    if (!execute(line)) return;
  }
}

}  // namespace usp
