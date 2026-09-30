#pragma once

#include <map>
#include <string>
#include <vector>

namespace usp {

// Connection settings for the STOMP broker (RabbitMQ with rabbitmq_stomp).
struct StompOptions {
  std::string host = "localhost";
  int port = 61613;
  std::string vhost = "/";
  std::string username = "guest";
  std::string password = "guest";
  int heartbeat_ms = 10000;        // 0 disables STOMP heart-beating
  int reconnect_max_ms = 30000;    // upper bound for exponential reconnect backoff
  bool tls = false;
  bool tls_verify = true;
  std::string tls_ca_file;
  std::string tls_cert_file;
  std::string tls_key_file;
};

struct ControllerConfig {
  StompOptions stomp;
  std::string endpoint_id = "proto::usp-controller";
  // Destination the controller SUBSCRIBEs to; Agents send their USP Records here.
  std::string controller_dest = "/queue/usp.controller";
  // Northbound task API (JSON). Empty string disables it.
  std::string task_dest = "/queue/usp.controller.tasks";
  std::string result_dest = "/queue/usp.controller.results";
  // USP version put in outgoing Records when the Agent's version is not known yet.
  std::string usp_version = "1.3";
  int request_timeout_ms = 30000;
  // Parameter paths fetched with a Get when an Agent boots / on-boards.
  std::vector<std::string> on_boot_get = {"Device.DeviceInfo."};
  // Statically configured Agent destinations (endpoint id -> STOMP destination).
  std::map<std::string, std::string> static_agents;
  std::string log_level = "info";
  bool cli = true;
};

// Loads "key = value" lines. Unknown keys are reported as errors.
// Returns false and fills `error` on failure.
bool load_config_file(const std::string& path, ControllerConfig& cfg, std::string& error);
// Applies USP_* environment variables on top of the current configuration.
bool apply_env_overrides(ControllerConfig& cfg, std::string& error);
// Applies a single key/value (same keys as the config file).
bool apply_config_value(ControllerConfig& cfg, const std::string& key, const std::string& value,
                        std::string& error);

std::vector<std::string> split_list(const std::string& s, char sep = ',');
std::string trim(const std::string& s);

}  // namespace usp
