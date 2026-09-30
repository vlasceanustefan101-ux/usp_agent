#pragma once

#include <memory>
#include <string>

#include "common/Config.h"

namespace usp::stomp {

// Byte stream to the broker: plain TCP or TLS. Blocking I/O; used from a
// single I/O thread only.
class Transport {
 public:
  virtual ~Transport() = default;
  // Returns bytes read, 0 on orderly close, -1 on error.
  virtual long read(char* buf, size_t len) = 0;
  // Writes all bytes. Returns false on error.
  virtual bool write_all(const char* data, size_t len) = 0;
  virtual int fd() const = 0;
  // True if decrypted data is already buffered (TLS) and poll() would not see it.
  virtual bool has_pending() const { return false; }
};

// Connects according to `opts` (TLS if opts.tls). Returns nullptr and sets
// `error` on failure.
std::unique_ptr<Transport> connect_transport(const StompOptions& opts, std::string& error);

}  // namespace usp::stomp
