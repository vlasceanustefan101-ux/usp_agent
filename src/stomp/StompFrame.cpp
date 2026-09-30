#include "stomp/StompFrame.h"

#include <cstdlib>

namespace usp::stomp {

std::optional<std::string> Frame::header(const std::string& name) const {
  for (const auto& h : headers)
    if (h.first == name) return h.second;
  return std::nullopt;
}

void Frame::set_header(const std::string& name, const std::string& value) {
  for (auto& h : headers) {
    if (h.first == name) {
      h.second = value;
      return;
    }
  }
  headers.emplace_back(name, value);
}

std::string escape_header(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case ':': out += "\\c"; break;
      default: out += c;
    }
  }
  return out;
}

std::optional<std::string> unescape_header(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\') {
      out += s[i];
      continue;
    }
    if (++i >= s.size()) return std::nullopt;
    switch (s[i]) {
      case '\\': out += '\\'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 'c': out += ':'; break;
      default: return std::nullopt;
    }
  }
  return out;
}

namespace {
bool is_connect_frame(const std::string& cmd) {
  return cmd == "CONNECT" || cmd == "STOMP" || cmd == "CONNECTED";
}
}  // namespace

std::string encode_frame(const Frame& f) {
  const bool raw = is_connect_frame(f.command);
  std::string out = f.command;
  out += '\n';
  bool has_len = false;
  for (const auto& h : f.headers) {
    if (h.first == "content-length") has_len = true;
    out += raw ? h.first : escape_header(h.first);
    out += ':';
    out += raw ? h.second : escape_header(h.second);
    out += '\n';
  }
  if (!has_len && !f.body.empty()) {
    out += "content-length:" + std::to_string(f.body.size()) + "\n";
  }
  out += '\n';
  out += f.body;
  out += '\0';
  return out;
}

FrameDecoder::Result FrameDecoder::next(Frame& out, std::string& error) {
  // Heart-beats (and optional EOLs between frames) are bare LF / CRLF.
  if (!buf_.empty() && (buf_[0] == '\n' || buf_[0] == '\r')) {
    if (buf_[0] == '\r') {
      if (buf_.size() < 2) return Result::NeedMore;
      if (buf_[1] != '\n') {
        error = "stray CR in stream";
        return Result::Error;
      }
      buf_.erase(0, 2);
    } else {
      buf_.erase(0, 1);
    }
    return Result::HeartBeat;
  }
  if (buf_.empty()) return Result::NeedMore;

  // Header section ends with an empty line.
  size_t hdr_end = buf_.find("\n\n");
  size_t hdr_end_crlf = buf_.find("\r\n\r\n");
  size_t sep_len = 2;
  if (hdr_end_crlf != std::string::npos && (hdr_end == std::string::npos || hdr_end_crlf < hdr_end)) {
    hdr_end = hdr_end_crlf;
    sep_len = 4;
  }
  if (hdr_end == std::string::npos) {
    if (buf_.size() > max_frame_bytes_) {
      error = "frame header too large";
      return Result::Error;
    }
    return Result::NeedMore;
  }

  Frame f;
  const std::string head = buf_.substr(0, hdr_end);
  size_t pos = 0;
  bool first = true;
  while (pos <= head.size()) {
    size_t eol = head.find('\n', pos);
    if (eol == std::string::npos) eol = head.size();
    std::string line = head.substr(pos, eol - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    pos = eol + 1;
    if (first) {
      f.command = line;
      first = false;
      continue;
    }
    if (line.empty()) continue;
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
      error = "malformed header line '" + line + "'";
      return Result::Error;
    }
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    if (!is_connect_frame(f.command)) {
      auto n = unescape_header(name);
      auto v = unescape_header(value);
      if (!n || !v) {
        error = "invalid header escape in '" + line + "'";
        return Result::Error;
      }
      name = *n;
      value = *v;
    }
    // Repeated headers: only the first value is used.
    if (!f.header(name)) f.headers.emplace_back(std::move(name), std::move(value));
  }
  if (f.command.empty()) {
    error = "empty STOMP command";
    return Result::Error;
  }

  const size_t body_start = hdr_end + sep_len;
  size_t body_len = 0;
  size_t frame_end = 0;  // index of the terminating NUL
  if (auto cl = f.header("content-length")) {
    char* endp = nullptr;
    unsigned long long n = std::strtoull(cl->c_str(), &endp, 10);
    if (cl->empty() || *endp != '\0' || n > max_frame_bytes_) {
      error = "invalid content-length '" + *cl + "'";
      return Result::Error;
    }
    body_len = static_cast<size_t>(n);
    if (buf_.size() < body_start + body_len + 1) return Result::NeedMore;
    frame_end = body_start + body_len;
    if (buf_[frame_end] != '\0') {
      error = "frame not terminated by NUL after content-length bytes";
      return Result::Error;
    }
  } else {
    size_t nul = buf_.find('\0', body_start);
    if (nul == std::string::npos) {
      if (buf_.size() > max_frame_bytes_) {
        error = "frame too large";
        return Result::Error;
      }
      return Result::NeedMore;
    }
    frame_end = nul;
    body_len = nul - body_start;
  }
  f.body = buf_.substr(body_start, body_len);
  buf_.erase(0, frame_end + 1);
  out = std::move(f);
  return Result::Frame;
}

}  // namespace usp::stomp
