#include "controller/TaskApi.h"

#include "common/Log.h"
#include "usp-controller-api.pb.h"

namespace usp {

TaskApi::TaskApi(Controller& controller, stomp::StompClient& stomp) : controller_(controller), stomp_(stomp) {}

void TaskApi::start() {
  if (!controller_.config().task_dest.empty()) stomp_.subscribe(controller_.config().task_dest);
}

void TaskApi::publish(const std::string& dest, const std::string& correlation_id, const std::string& json) {
  std::vector<std::pair<std::string, std::string>> headers = {{"content-type", "application/json"}};
  if (!correlation_id.empty()) headers.emplace_back("correlation-id", correlation_id);
  if (!stomp_.send(dest, json, headers)) LOG_WARN("TaskApi: could not publish result to " << dest);
}

void TaskApi::handle_frame(const stomp::Frame& f) {
  // RabbitMQ maps AMQP reply_to <-> STOMP "reply-to" and correlation_id <-> "correlation-id".
  std::string reply_dest = f.header("reply-to").value_or("");
  if (reply_dest.empty()) reply_dest = controller_.config().result_dest;
  // A bare name is an AMQP queue declared by the backend: address it without re-declaring it.
  if (!reply_dest.empty() && reply_dest[0] != '/') reply_dest = "/amq/queue/" + reply_dest;
  const std::string correlation_id = f.header("correlation-id").value_or("");

  usp_controller::Task task;
  std::string error;
  if (!from_json(f.body, task, error)) {
    LOG_WARN("TaskApi: invalid task JSON: " << error);
    usp_controller::TaskResult r;
    r.set_status("error");
    r.set_error("invalid task JSON: " + error);
    if (!reply_dest.empty()) publish(reply_dest, correlation_id, to_json(r, false));
    return;
  }

  if (task.list_agents()) {
    usp_controller::TaskResult r;
    r.set_task_id(task.task_id());
    r.set_status("ok");
    for (const auto& a : controller_.agents()) {
      auto* ai = r.add_agents();
      ai->set_endpoint_id(a.endpoint_id);
      ai->set_destination(a.destination);
      ai->set_usp_version(a.usp_version);
      ai->set_online(a.online);
      ai->set_last_seen(a.last_seen);
      ai->set_oui(a.oui);
      ai->set_product_class(a.product_class);
      ai->set_serial_number(a.serial_number);
    }
    if (!reply_dest.empty()) publish(reply_dest, correlation_id, to_json(r, false));
    return;
  }

  if (task.endpoint_id().empty() || task.request().req_type_case() == Request::REQ_TYPE_NOT_SET) {
    usp_controller::TaskResult r;
    r.set_task_id(task.task_id());
    r.set_status("error");
    r.set_error("task needs endpoint_id and request");
    if (!reply_dest.empty()) publish(reply_dest, correlation_id, to_json(r, false));
    return;
  }

  LOG_INFO("TaskApi: task " << task.task_id() << " -> " << task.endpoint_id());
  const std::string task_id = task.task_id();
  const std::string endpoint = task.endpoint_id();
  controller_.send_request(endpoint, task.request(), static_cast<int>(task.timeout_ms()),
                           [this, task_id, endpoint, reply_dest, correlation_id](const RequestOutcome& o) {
                             usp_controller::TaskResult r;
                             r.set_task_id(task_id);
                             r.set_endpoint_id(endpoint);
                             r.set_status(status_name(o.status));
                             r.set_error(o.error);
                             if (o.status == RequestOutcome::Status::Ok ||
                                 o.status == RequestOutcome::Status::UspError)
                               *r.mutable_response() = o.response;
                             if (!reply_dest.empty()) publish(reply_dest, correlation_id, to_json(r, false));
                           });
}

}  // namespace usp
