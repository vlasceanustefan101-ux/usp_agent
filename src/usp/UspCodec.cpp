#include "usp/UspCodec.h"

#include <google/protobuf/util/json_util.h>

#include <atomic>
#include <chrono>
#include <random>

namespace usp {

std::string new_msg_id(const std::string& prefix) {
  static std::atomic<unsigned long> counter{0};
  static const unsigned long boot = static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
          .count());
  return prefix + "-" + std::to_string(boot) + "-" + std::to_string(++counter);
}

Header::MsgType request_msg_type(const Request& req) {
  switch (req.req_type_case()) {
    case Request::kGet: return Header::GET;
    case Request::kGetSupportedDm: return Header::GET_SUPPORTED_DM;
    case Request::kGetInstances: return Header::GET_INSTANCES;
    case Request::kSet: return Header::SET;
    case Request::kAdd: return Header::ADD;
    case Request::kDelete: return Header::DELETE;
    case Request::kOperate: return Header::OPERATE;
    case Request::kNotify: return Header::NOTIFY;
    case Request::kGetSupportedProtocol: return Header::GET_SUPPORTED_PROTO;
    case Request::kRegister: return Header::REGISTER;
    case Request::kDeregister: return Header::DEREGISTER;
    case Request::REQ_TYPE_NOT_SET: break;
  }
  return Header::ERROR;
}

Header::MsgType response_msg_type(const Response& resp) {
  switch (resp.resp_type_case()) {
    case Response::kGetResp: return Header::GET_RESP;
    case Response::kGetSupportedDmResp: return Header::GET_SUPPORTED_DM_RESP;
    case Response::kGetInstancesResp: return Header::GET_INSTANCES_RESP;
    case Response::kSetResp: return Header::SET_RESP;
    case Response::kAddResp: return Header::ADD_RESP;
    case Response::kDeleteResp: return Header::DELETE_RESP;
    case Response::kOperateResp: return Header::OPERATE_RESP;
    case Response::kNotifyResp: return Header::NOTIFY_RESP;
    case Response::kGetSupportedProtocolResp: return Header::GET_SUPPORTED_PROTO_RESP;
    case Response::kRegisterResp: return Header::REGISTER_RESP;
    case Response::kDeregisterResp: return Header::DEREGISTER_RESP;
    case Response::RESP_TYPE_NOT_SET: break;
  }
  return Header::ERROR;
}

Msg make_request(const std::string& msg_id, const Request& req) {
  Msg m;
  m.mutable_header()->set_msg_id(msg_id);
  m.mutable_header()->set_msg_type(request_msg_type(req));
  *m.mutable_body()->mutable_request() = req;
  return m;
}

Msg make_response(const std::string& msg_id, const Response& resp) {
  Msg m;
  m.mutable_header()->set_msg_id(msg_id);
  m.mutable_header()->set_msg_type(response_msg_type(resp));
  *m.mutable_body()->mutable_response() = resp;
  return m;
}

Msg make_error(const std::string& msg_id, unsigned code, const std::string& text) {
  Msg m;
  m.mutable_header()->set_msg_id(msg_id);
  m.mutable_header()->set_msg_type(Header::ERROR);
  auto* e = m.mutable_body()->mutable_error();
  e->set_err_code(code);
  e->set_err_msg(text);
  return m;
}

namespace {
usp_record::Record base_record(const std::string& version, const std::string& from_id, const std::string& to_id) {
  usp_record::Record r;
  r.set_version(version);
  r.set_from_id(from_id);
  r.set_to_id(to_id);
  r.set_payload_security(usp_record::Record::PLAINTEXT);
  return r;
}
}  // namespace

std::string make_record(const std::string& version, const std::string& from_id, const std::string& to_id,
                        const Msg& msg) {
  auto r = base_record(version, from_id, to_id);
  r.mutable_no_session_context()->set_payload(msg.SerializeAsString());
  return r.SerializeAsString();
}

std::string make_stomp_connect_record(const std::string& version, const std::string& from_id,
                                      const std::string& to_id, const std::string& own_destination) {
  auto r = base_record(version, from_id, to_id);
  auto* c = r.mutable_stomp_connect();
  c->set_version(usp_record::STOMPConnectRecord::V1_2);
  c->set_subscribed_destination(own_destination);
  return r.SerializeAsString();
}

std::string to_json(const google::protobuf::Message& m, bool pretty) {
  google::protobuf::util::JsonPrintOptions opts;
  opts.add_whitespace = pretty;
  opts.preserve_proto_field_names = true;
  std::string out;
  auto st = google::protobuf::util::MessageToJsonString(m, &out, opts);
  if (!st.ok()) return "{\"json_error\":\"" + std::string(st.message()) + "\"}";
  if (pretty && !out.empty() && out.back() == '\n') out.pop_back();
  return out;
}

bool from_json(const std::string& json, google::protobuf::Message& m, std::string& error) {
  google::protobuf::util::JsonParseOptions opts;
  opts.ignore_unknown_fields = false;
  auto st = google::protobuf::util::JsonStringToMessage(json, &m, opts);
  if (!st.ok()) {
    error = std::string(st.message());
    return false;
  }
  return true;
}

std::string body_kind(const Msg& msg) {
  const Body& b = msg.body();
  if (b.has_request()) {
    const auto* d = b.request().GetDescriptor();
    const auto* oneof = d->FindOneofByName("req_type");
    const auto* field = b.request().GetReflection()->GetOneofFieldDescriptor(b.request(), oneof);
    return field ? field->name() : "request";
  }
  if (b.has_response()) {
    const auto* d = b.response().GetDescriptor();
    const auto* oneof = d->FindOneofByName("resp_type");
    const auto* field = b.response().GetReflection()->GetOneofFieldDescriptor(b.response(), oneof);
    return field ? field->name() : "response";
  }
  if (b.has_error()) return "error";
  return "empty";
}

}  // namespace usp
