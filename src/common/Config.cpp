#include "common/Config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace usp {

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::vector<std::string> split_list(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t pos = s.find(sep, start);
    if (pos == std::string::npos) pos = s.size();
    std::string item = trim(s.substr(start, pos - start));
    if (!item.empty()) out.push_back(item);
    start = pos + 1;
  }
  return out;
}

namespace {

bool parse_bool(const std::string& v, bool& out) {
  if (v == "1" || v == "true" || v == "yes" || v == "on") { out = true; return true; }
  if (v == "0" || v == "false" || v == "no" || v == "off") { out = false; return true; }
  return false;
}

bool parse_int(const std::string& v, int& out) {
  try {
    size_t idx = 0;
    int n = std::stoi(v, &idx);
    if (idx != v.size()) return false;
    out = n;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace

bool apply_config_value(ControllerConfig& cfg, const std::string& key, const std::string& value,
                        std::string& error) {
  auto bad = [&](const char* what) {
    error = "invalid " + std::string(what) + " for '" + key + "': '" + value + "'";
    return false;
  };
  StompOptions& s = cfg.stomp;
  if (key == "stomp.host") s.host = value;
  else if (key == "stomp.port") { if (!parse_int(value, s.port)) return bad("integer"); }
  else if (key == "stomp.vhost") s.vhost = value;
  else if (key == "stomp.username") s.username = value;
  else if (key == "stomp.password") s.password = value;
  else if (key == "stomp.heartbeat_ms") { if (!parse_int(value, s.heartbeat_ms)) return bad("integer"); }
  else if (key == "stomp.reconnect_max_ms") { if (!parse_int(value, s.reconnect_max_ms)) return bad("integer"); }
  else if (key == "stomp.tls") { if (!parse_bool(value, s.tls)) return bad("boolean"); }
  else if (key == "stomp.tls_verify") { if (!parse_bool(value, s.tls_verify)) return bad("boolean"); }
  else if (key == "stomp.tls_ca_file") s.tls_ca_file = value;
  else if (key == "stomp.tls_cert_file") s.tls_cert_file = value;
  else if (key == "stomp.tls_key_file") s.tls_key_file = value;
  else if (key == "controller.endpoint_id") cfg.endpoint_id = value;
  else if (key == "controller.destination") cfg.controller_dest = value;
  else if (key == "controller.task_destination") cfg.task_dest = value;
  else if (key == "controller.result_destination") cfg.result_dest = value;
  else if (key == "controller.usp_version") cfg.usp_version = value;
  else if (key == "controller.request_timeout_ms") { if (!parse_int(value, cfg.request_timeout_ms)) return bad("integer"); }
  else if (key == "controller.on_boot_get") cfg.on_boot_get = split_list(value);
  else if (key == "controller.cli") { if (!parse_bool(value, cfg.cli)) return bad("boolean"); }
  else if (key == "log.level") cfg.log_level = value;
  else if (key.rfind("agent.", 0) == 0 && key.size() > 6) cfg.static_agents[key.substr(6)] = value;
  else {
    error = "unknown configuration key '" + key + "'";
    return false;
  }
  return true;
}

bool load_config_file(const std::string& path, ControllerConfig& cfg, std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot open config file '" + path + "'";
    return false;
  }
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos) {
      error = path + ":" + std::to_string(lineno) + ": expected 'key = value'";
      return false;
    }
    std::string key = trim(t.substr(0, eq));
    std::string value = trim(t.substr(eq + 1));
    // Agent endpoint IDs contain ':' so they are written as agent."os::1234" = ...
    if (key.size() >= 8 && key.rfind("agent.\"", 0) == 0 && key.back() == '"')
      key = "agent." + key.substr(7, key.size() - 8);
    std::string err;
    if (!apply_config_value(cfg, key, value, err)) {
      error = path + ":" + std::to_string(lineno) + ": " + err;
      return false;
    }
  }
  return true;
}

bool apply_env_overrides(ControllerConfig& cfg, std::string& error) {
  static const std::pair<const char*, const char*> kEnv[] = {
      {"USP_STOMP_HOST", "stomp.host"},
      {"USP_STOMP_PORT", "stomp.port"},
      {"USP_STOMP_VHOST", "stomp.vhost"},
      {"USP_STOMP_USERNAME", "stomp.username"},
      {"USP_STOMP_PASSWORD", "stomp.password"},
      {"USP_STOMP_HEARTBEAT_MS", "stomp.heartbeat_ms"},
      {"USP_STOMP_TLS", "stomp.tls"},
      {"USP_STOMP_TLS_VERIFY", "stomp.tls_verify"},
      {"USP_STOMP_TLS_CA_FILE", "stomp.tls_ca_file"},
      {"USP_STOMP_TLS_CERT_FILE", "stomp.tls_cert_file"},
      {"USP_STOMP_TLS_KEY_FILE", "stomp.tls_key_file"},
      {"USP_CONTROLLER_ENDPOINT_ID", "controller.endpoint_id"},
      {"USP_CONTROLLER_DESTINATION", "controller.destination"},
      {"USP_CONTROLLER_TASK_DESTINATION", "controller.task_destination"},
      {"USP_CONTROLLER_RESULT_DESTINATION", "controller.result_destination"},
      {"USP_CONTROLLER_USP_VERSION", "controller.usp_version"},
      {"USP_CONTROLLER_REQUEST_TIMEOUT_MS", "controller.request_timeout_ms"},
      {"USP_CONTROLLER_ON_BOOT_GET", "controller.on_boot_get"},
      {"USP_CONTROLLER_CLI", "controller.cli"},
      {"USP_LOG_LEVEL", "log.level"},
  };
  for (const auto& kv : kEnv) {
    const char* v = std::getenv(kv.first);
    if (v == nullptr) continue;
    if (!apply_config_value(cfg, kv.second, v, error)) {
      error = std::string(kv.first) + ": " + error;
      return false;
    }
  }
  return true;
}

}  // namespace usp
