#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace usp::stomp {

// A STOMP 1.2 frame. Header order is preserved; for repeated headers the
// first occurrence wins (STOMP 1.2, section "Repeated Header Entries").
struct Frame {
  std::string command;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;  // may contain binary data (protobuf)

  std::optional<std::string> header(const std::string& name) const;
  void set_header(const std::string& name, const std::string& value);
};

// Serializes a frame. Adds content-length for frames that carry a body.
// Header escaping is not applied to CONNECT/CONNECTED frames (STOMP 1.2).
std::string encode_frame(const Frame& f);

std::string escape_header(const std::string& s);
// Returns std::nullopt for an invalid escape sequence.
std::optional<std::string> unescape_header(const std::string& s);

// Incremental decoder for a byte stream coming from the broker.
class FrameDecoder {
 public:
  explicit FrameDecoder(size_t max_frame_bytes = 16 * 1024 * 1024) : max_frame_bytes_(max_frame_bytes) {}

  void feed(const char* data, size_t len) { buf_.append(data, len); }

  enum class Result { Frame, NeedMore, HeartBeat, Error };
  // Extracts the next item from the buffer. On Error, `error` describes the problem
  // and the connection should be closed.
  Result next(Frame& out, std::string& error);

 private:
  std::string buf_;
  size_t max_frame_bytes_;
};

}  // namespace usp::stomp
