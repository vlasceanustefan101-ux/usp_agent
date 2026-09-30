#include "stomp/Transport.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#ifdef USP_WITH_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#endif

namespace usp::stomp {

namespace {

int tcp_connect(const std::string& host, int port, std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port_str = std::to_string(port);
  int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
  if (rc != 0) {
    error = "resolve " + host + ": " + gai_strerror(rc);
    return -1;
  }
  int fd = -1;
  for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    error = "connect " + host + ":" + port_str + ": " + std::strerror(errno);
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd >= 0) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  }
  return fd;
}

class TcpTransport : public Transport {
 public:
  explicit TcpTransport(int fd) : fd_(fd) {}
  ~TcpTransport() override { ::close(fd_); }

  long read(char* buf, size_t len) override {
    for (;;) {
      ssize_t n = ::recv(fd_, buf, len, 0);
      if (n < 0 && errno == EINTR) continue;
      return static_cast<long>(n);
    }
  }

  bool write_all(const char* data, size_t len) override {
    while (len > 0) {
      ssize_t n = ::send(fd_, data, len, MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      data += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  }

  int fd() const override { return fd_; }

 private:
  int fd_;
};

#ifdef USP_WITH_TLS
std::string ssl_error_string() {
  unsigned long e = ERR_get_error();
  if (e == 0) return "unknown TLS error";
  char buf[256];
  ERR_error_string_n(e, buf, sizeof(buf));
  return buf;
}

class TlsTransport : public Transport {
 public:
  TlsTransport(int fd, SSL_CTX* ctx, SSL* ssl) : fd_(fd), ctx_(ctx), ssl_(ssl) {}
  ~TlsTransport() override {
    SSL_shutdown(ssl_);
    SSL_free(ssl_);
    SSL_CTX_free(ctx_);
    ::close(fd_);
  }

  long read(char* buf, size_t len) override {
    int n = SSL_read(ssl_, buf, static_cast<int>(len));
    if (n > 0) return n;
    int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_ZERO_RETURN) return 0;
    return -1;
  }

  bool write_all(const char* data, size_t len) override {
    while (len > 0) {
      int n = SSL_write(ssl_, data, static_cast<int>(len));
      if (n <= 0) return false;
      data += n;
      len -= static_cast<size_t>(n);
    }
    return true;
  }

  int fd() const override { return fd_; }
  bool has_pending() const override { return SSL_pending(ssl_) > 0; }

 private:
  int fd_;
  SSL_CTX* ctx_;
  SSL* ssl_;
};

std::unique_ptr<Transport> tls_connect(int fd, const StompOptions& o, std::string& error) {
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (ctx == nullptr) {
    error = "SSL_CTX_new: " + ssl_error_string();
    ::close(fd);
    return nullptr;
  }
  SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
  // USP forbids TLS renegotiation (R-E2E.42 / MTP security).
  SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
  auto fail = [&](const std::string& what) -> std::unique_ptr<Transport> {
    error = what + ": " + ssl_error_string();
    SSL_CTX_free(ctx);
    ::close(fd);
    return nullptr;
  };
  if (o.tls_verify) {
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (!o.tls_ca_file.empty()) {
      if (SSL_CTX_load_verify_locations(ctx, o.tls_ca_file.c_str(), nullptr) != 1)
        return fail("load CA file " + o.tls_ca_file);
    } else if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      return fail("load default CA paths");
    }
  }
  if (!o.tls_cert_file.empty()) {
    if (SSL_CTX_use_certificate_chain_file(ctx, o.tls_cert_file.c_str()) != 1)
      return fail("load client certificate " + o.tls_cert_file);
    const std::string& key = o.tls_key_file.empty() ? o.tls_cert_file : o.tls_key_file;
    if (SSL_CTX_use_PrivateKey_file(ctx, key.c_str(), SSL_FILETYPE_PEM) != 1)
      return fail("load client key " + key);
  }
  SSL* ssl = SSL_new(ctx);
  if (ssl == nullptr) return fail("SSL_new");
  SSL_set_fd(ssl, fd);
  SSL_set_tlsext_host_name(ssl, o.host.c_str());
  if (o.tls_verify) {
    X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    X509_VERIFY_PARAM_set1_host(param, o.host.c_str(), 0);
  }
  if (SSL_connect(ssl) != 1) {
    std::string e = ssl_error_string();
    long vr = SSL_get_verify_result(ssl);
    if (vr != X509_V_OK) e += " (verify: " + std::string(X509_verify_cert_error_string(vr)) + ")";
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    ::close(fd);
    error = "TLS handshake with " + o.host + ": " + e;
    return nullptr;
  }
  return std::make_unique<TlsTransport>(fd, ctx, ssl);
}
#endif

}  // namespace

std::unique_ptr<Transport> connect_transport(const StompOptions& opts, std::string& error) {
  int fd = tcp_connect(opts.host, opts.port, error);
  if (fd < 0) return nullptr;
  if (!opts.tls) return std::make_unique<TcpTransport>(fd);
#ifdef USP_WITH_TLS
  return tls_connect(fd, opts, error);
#else
  ::close(fd);
  error = "TLS requested but the program was built without USP_WITH_TLS";
  return nullptr;
#endif
}

}  // namespace usp::stomp
