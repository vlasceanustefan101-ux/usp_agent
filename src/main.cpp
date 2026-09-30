// TR-369 (USP) Controller using the STOMP MTP over RabbitMQ.
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>

#include "common/Config.h"
#include "common/Log.h"
#include "controller/Cli.h"
#include "controller/Controller.h"
#include "controller/TaskApi.h"
#include "stomp/StompClient.h"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

void usage(const char* argv0) {
  std::cout << "Usage: " << argv0 << " [--config FILE] [--set key=value]... [--no-cli]\n"
            << "  --config FILE      configuration file (key = value), see config/controller.conf\n"
            << "  --set key=value    override one configuration key (repeatable)\n"
            << "  --no-cli           do not start the interactive console\n"
            << "Environment variables USP_* override the file (see README).\n";
}

}  // namespace

int main(int argc, char** argv) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;
  usp::ControllerConfig cfg;
  std::string error;
  std::vector<std::pair<std::string, std::string>> overrides;
  bool force_no_cli = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--config" || a == "-c") && i + 1 < argc) {
      if (!usp::load_config_file(argv[++i], cfg, error)) {
        std::cerr << "config error: " << error << std::endl;
        return 2;
      }
    } else if (a == "--set" && i + 1 < argc) {
      std::string kv = argv[++i];
      auto eq = kv.find('=');
      if (eq == std::string::npos) {
        std::cerr << "--set expects key=value" << std::endl;
        return 2;
      }
      overrides.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    } else if (a == "--no-cli") {
      force_no_cli = true;
    } else if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return 0;
    } else {
      std::cerr << "unknown argument '" << a << "'" << std::endl;
      usage(argv[0]);
      return 2;
    }
  }
  if (!usp::apply_env_overrides(cfg, error)) {
    std::cerr << "environment error: " << error << std::endl;
    return 2;
  }
  for (const auto& kv : overrides) {
    if (!usp::apply_config_value(cfg, kv.first, kv.second, error)) {
      std::cerr << "--set error: " << error << std::endl;
      return 2;
    }
  }
  if (force_no_cli) cfg.cli = false;
  usp::set_log_level(usp::parse_log_level(cfg.log_level));

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  LOG_INFO("USP controller " << cfg.endpoint_id << " listening on " << cfg.controller_dest
                             << (cfg.task_dest.empty() ? "" : ", tasks on " + cfg.task_dest));

  usp::stomp::StompClient stomp(cfg.stomp);
  usp::Controller controller(cfg, stomp);
  usp::TaskApi tasks(controller, stomp);

  controller.start();
  tasks.start();
  // Route by subscription: task API frames vs. USP Records from Agents.
  const std::string task_sub = cfg.task_dest.empty() ? "" : stomp.subscription_id(cfg.task_dest);
  stomp.on_message([&](const usp::stomp::Frame& f) {
    if (!task_sub.empty() && f.header("subscription").value_or("") == task_sub) {
      tasks.handle_frame(f);
    } else {
      controller.handle_frame(f);
    }
  });
  stomp.start();

  const bool interactive = cfg.cli && ::isatty(STDIN_FILENO);
  if (interactive) {
    usp::Cli cli(controller);
    cli.run(g_stop);
  } else {
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  LOG_INFO("shutting down");
  stomp.stop();
  controller.stop();
  google::protobuf::ShutdownProtobufLibrary();
  return 0;
}
