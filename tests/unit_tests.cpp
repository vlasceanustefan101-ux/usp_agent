// Small self-contained unit tests (no external test framework needed).
#include <cstdio>
#include <string>

#include "common/Config.h"
#include "controller/Cli.h"
#include "stomp/StompFrame.h"
#include "usp/UspCodec.h"

namespace {
int g_failures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                         \
    }                                                                       \
  } while (0)

using namespace usp;
using namespace usp::stomp;

void test_frame_roundtrip_binary_body() {
  Frame f{"SEND", {{"destination", "/queue/a"}, {"reply-to-dest", "/queue/b:c"}}, std::string("a\0b\0c", 5)};
  std::string wire = encode_frame(f);
  FrameDecoder d;
  // Feed byte by byte to exercise partial reads.
  Frame out;
  std::string err;
  FrameDecoder::Result r = FrameDecoder::Result::NeedMore;
  for (char c : wire) {
    d.feed(&c, 1);
    r = d.next(out, err);
    if (r != FrameDecoder::Result::NeedMore) break;
  }
  CHECK(r == FrameDecoder::Result::Frame);
  CHECK(out.command == "SEND");
  CHECK(out.body == std::string("a\0b\0c", 5));
  CHECK(out.header("reply-to-dest").value_or("") == "/queue/b:c");  // ':' escaped as \c on the wire
  CHECK(wire.find("/queue/b\\cc") != std::string::npos);
}

void test_heartbeat_and_two_frames() {
  FrameDecoder d;
  std::string data = "\n\r\nMESSAGE\ndestination:/queue/x\n\nhello";
  data.push_back('\0');
  data += "\nRECEIPT\nreceipt-id:1\n\n";
  data.push_back('\0');
  d.feed(data.data(), data.size());
  Frame f;
  std::string err;
  CHECK(d.next(f, err) == FrameDecoder::Result::HeartBeat);
  CHECK(d.next(f, err) == FrameDecoder::Result::HeartBeat);
  CHECK(d.next(f, err) == FrameDecoder::Result::Frame);
  CHECK(f.command == "MESSAGE" && f.body == "hello");
  CHECK(d.next(f, err) == FrameDecoder::Result::HeartBeat);
  CHECK(d.next(f, err) == FrameDecoder::Result::Frame);
  CHECK(f.command == "RECEIPT" && f.header("receipt-id").value_or("") == "1");
  CHECK(d.next(f, err) == FrameDecoder::Result::NeedMore);
}

void test_bad_content_length() {
  FrameDecoder d;
  std::string data = "MESSAGE\ncontent-length:2\n\nabc";
  data.push_back('\0');
  d.feed(data.data(), data.size());
  Frame f;
  std::string err;
  CHECK(d.next(f, err) == FrameDecoder::Result::Error);
}

void test_repeated_header_first_wins() {
  FrameDecoder d;
  std::string data = "MESSAGE\nfoo:1\nfoo:2\n\n";
  data.push_back('\0');
  d.feed(data.data(), data.size());
  Frame f;
  std::string err;
  CHECK(d.next(f, err) == FrameDecoder::Result::Frame);
  CHECK(f.header("foo").value_or("") == "1");
}

void test_codec() {
  Request req;
  req.mutable_get()->add_param_paths("Device.DeviceInfo.");
  Msg m = make_request("id-1", req);
  CHECK(m.header().msg_type() == Header::GET);
  CHECK(body_kind(m) == "get");
  std::string rec_bytes = make_record("1.3", "proto::ctrl", "os::agent", m);
  usp_record::Record rec;
  CHECK(rec.ParseFromString(rec_bytes));
  CHECK(rec.to_id() == "os::agent" && rec.from_id() == "proto::ctrl" && rec.version() == "1.3");
  Msg back;
  CHECK(back.ParseFromString(rec.no_session_context().payload()));
  CHECK(back.body().request().get().param_paths(0) == "Device.DeviceInfo.");

  Response resp;
  resp.mutable_notify_resp()->set_subscription_id("s1");
  CHECK(make_response("x", resp).header().msg_type() == Header::NOTIFY_RESP);
  CHECK(make_error("x", 7001, "no").header().msg_type() == Header::ERROR);

  Request parsed;
  std::string err;
  CHECK(from_json(R"({"set":{"allow_partial":false,"update_objs":[{"obj_path":"Device.WiFi.SSID.1.",
      "param_settings":[{"param":"SSID","value":"x","required":true}]}]}})", parsed, err));
  CHECK(parsed.has_set() && parsed.set().update_objs(0).param_settings(0).value() == "x");
  CHECK(!from_json("{\"nope\":1}", parsed, err));
}

void test_cli_tokenizer() {
  std::string err;
  auto t = Cli::tokenize(R"(set Device.WiFi.SSID.1. SSID="My Net" Enable=true)", err);
  CHECK(err.empty());
  CHECK(t.size() == 4);
  CHECK(t[2] == "SSID=My Net");
  Cli::tokenize("get \"unterminated", err);
  CHECK(!err.empty());
}

void test_config() {
  ControllerConfig cfg;
  std::string err;
  CHECK(apply_config_value(cfg, "stomp.port", "61614", err) && cfg.stomp.port == 61614);
  CHECK(!apply_config_value(cfg, "stomp.port", "abc", err));
  CHECK(!apply_config_value(cfg, "no.such.key", "1", err));
  CHECK(apply_config_value(cfg, "controller.on_boot_get", "Device.DeviceInfo., Device.WiFi.", err));
  CHECK(cfg.on_boot_get.size() == 2 && cfg.on_boot_get[1] == "Device.WiFi.");
  CHECK(apply_config_value(cfg, "agent.os::1234", "/queue/x", err) && cfg.static_agents["os::1234"] == "/queue/x");
}

}  // namespace

int main() {
  test_frame_roundtrip_binary_body();
  test_heartbeat_and_two_frames();
  test_bad_content_length();
  test_repeated_header_first_wins();
  test_codec();
  test_cli_tokenizer();
  test_config();
  if (g_failures == 0) std::printf("all tests passed\n");
  return g_failures == 0 ? 0 : 1;
}
