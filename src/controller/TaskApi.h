#pragma once

#include <string>

#include "controller/Controller.h"
#include "stomp/StompClient.h"

namespace usp {

// Northbound JSON task API over RabbitMQ (STOMP).
// A backend publishes a usp_controller.Task as JSON to config.task_dest; the
// controller forwards the USP request to the Agent and publishes a
// usp_controller.TaskResult as JSON to the task's "reply-to" destination
// (or to config.result_dest when the task has none).
class TaskApi {
 public:
  TaskApi(Controller& controller, stomp::StompClient& stomp);
  void start();
  void handle_frame(const stomp::Frame& f);  // I/O thread

 private:
  void publish(const std::string& dest, const std::string& correlation_id, const std::string& json);

  Controller& controller_;
  stomp::StompClient& stomp_;
};

}  // namespace usp
