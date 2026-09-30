#pragma once

#include <string>

#include "usp-msg-1-4.pb.h"
#include "usp-record-1-4.pb.h"

namespace usp {

// STOMP content-type for USP Records (TR-369, STOMP MTP binding).
inline constexpr const char* kUspContentType = "application/vnd.bbf.usp.msg";
// STOMP header carrying the sender's own destination (TR-369, R-STOMP.*).
inline constexpr const char* kReplyToDestHeader = "reply-to-dest";

// USP error codes used by the controller (TR-369 "Error Codes").
inline constexpr unsigned kErrMessageFailed = 7000;
inline constexpr unsigned kErrMessageNotSupported = 7001;
inline constexpr unsigned kErrInternal = 7003;

std::string new_msg_id(const std::string& prefix);

// Header msg_type matching the request / response carried in the body.
Header::MsgType request_msg_type(const Request& req);
Header::MsgType response_msg_type(const Response& resp);

Msg make_request(const std::string& msg_id, const Request& req);
Msg make_response(const std::string& msg_id, const Response& resp);
Msg make_error(const std::string& msg_id, unsigned code, const std::string& text);

// Wraps a serialized Msg into a NoSessionContext Record and serializes it.
std::string make_record(const std::string& version, const std::string& from_id, const std::string& to_id,
                        const Msg& msg);
std::string make_stomp_connect_record(const std::string& version, const std::string& from_id,
                                      const std::string& to_id, const std::string& own_destination);

// Protobuf JSON (field names as in the .proto files).
std::string to_json(const google::protobuf::Message& m, bool pretty = true);
bool from_json(const std::string& json, google::protobuf::Message& m, std::string& error);

// Name of the request/response in the body, e.g. "get", "get_resp", "error".
std::string body_kind(const Msg& msg);

}  // namespace usp
