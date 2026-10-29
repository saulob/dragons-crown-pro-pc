// WebSocket client on BSD sockets, with OpenSSL for wss:// (system certificate store, host name checked).
#include "net/websocket.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <vector>

#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include "net/crypto.h"

namespace dcp::net {
namespace {

std::string Base64(const uint8_t* data, size_t size) {
    std::string out(4 * ((size + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, int(size));
    return out;
}

int Connect(const WsUrl& url, int timeout_ms, std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    const std::string port = std::to_string(url.port);
    if (getaddrinfo(url.host.c_str(), port.c_str(), &hints, &found) != 0 || !found) {
        if (error) *error = "the lobby's host name does not resolve";
        return -1;
    }
    int fd = -1;
    for (const addrinfo* p = found; p && fd < 0; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) != 0 && errno != EINPROGRESS) {
            ::close(fd), fd = -1;
            continue;
        }
        pollfd pfd{fd, POLLOUT, 0};
        int so_error = 0;
        socklen_t length = sizeof(so_error);
        if (poll(&pfd, 1, timeout_ms) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) != 0 || so_error) {
            ::close(fd), fd = -1;
        }
    }
    freeaddrinfo(found);
    if (fd < 0) {
        if (error) *error = "the lobby refused the connection or is not running";
        return -1;
    }
    const int flags = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));
    return fd;
}

class PosixSocket final : public WebSocket {
public:
    PosixSocket(int fd, SSL_CTX* context, SSL* ssl) : fd_(fd), context_(context), ssl_(ssl) {}

    ~PosixSocket() override {
        close();
        if (ssl_) SSL_free(ssl_);
        if (context_) SSL_CTX_free(context_);
        ::close(fd_);
    }

    // Blocking I/O with a deadline; nonblocking sockets underneath so close() can end a wait.
    bool Write(const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        while (size) {
            if (closed_) return false;
            int n;
            if (ssl_) {
                n = SSL_write(ssl_, p, int(size));
                if (n <= 0) {
                    const int e = SSL_get_error(ssl_, n);
                    if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return false;
                    if (!Wait(e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, 10000)) return false;
                    continue;
                }
            } else {
                n = int(::send(fd_, p, size, MSG_NOSIGNAL));
                if (n < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
                    if (!Wait(POLLOUT, 10000)) return false;
                    continue;
                }
            }
            p += n, size -= size_t(n);
        }
        return true;
    }

    // Some bytes; false once the connection ends.
    bool Read(uint8_t* out, size_t size, size_t& got, int timeout_ms) {
        for (;;) {
            if (closed_) return false;
            if (ssl_) {
                const int n = SSL_read(ssl_, out, int(size));
                if (n > 0) return got = size_t(n), true;
                const int e = SSL_get_error(ssl_, n);
                if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return false;
                if (!Wait(e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, timeout_ms)) return false;
            } else {
                const ssize_t n = ::recv(fd_, out, size, 0);
                if (n > 0) return got = size_t(n), true;
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return false;
                if (!Wait(POLLIN, timeout_ms)) return false;
            }
        }
    }

    bool ReadExact(uint8_t* out, size_t size) {
        while (size) {
            if (pending_.empty()) {
                uint8_t chunk[16384];
                size_t got = 0;
                if (!Read(chunk, sizeof(chunk), got, 75000)) return false;
                pending_.insert(pending_.end(), chunk, chunk + got);
            }
            const size_t take = std::min(size, pending_.size());
            std::memcpy(out, pending_.data(), take);
            pending_.erase(pending_.begin(), pending_.begin() + ptrdiff_t(take));
            out += take, size -= take;
        }
        return true;
    }

    bool Frame(uint8_t opcode, const void* payload, size_t size) {
        std::vector<uint8_t> frame;
        frame.push_back(uint8_t(0x80 | opcode));
        if (size < 126) {
            frame.push_back(uint8_t(0x80 | size));
        } else if (size < 0x10000) {
            frame.push_back(0x80 | 126);
            frame.push_back(uint8_t(size >> 8));
            frame.push_back(uint8_t(size));
        } else {
            frame.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i) frame.push_back(uint8_t(uint64_t(size) >> (8 * i)));
        }
        uint8_t mask[4];
        random_bytes(mask, sizeof(mask));
        frame.insert(frame.end(), mask, mask + 4);
        const auto* p = static_cast<const uint8_t*>(payload);
        for (size_t i = 0; i < size; ++i) frame.push_back(p[i] ^ mask[i & 3]);
        std::lock_guard<std::mutex> lock(send_mutex_);
        return Write(frame.data(), frame.size());
    }

    bool send_text(const std::string& text) override { return !closed_ && Frame(1, text.data(), text.size()); }

    bool receive(std::string& text) override {
        text.clear();
        for (;;) {
            uint8_t head[2];
            if (!ReadExact(head, 2)) return false;
            const uint8_t opcode = head[0] & 0x0f;
            uint64_t length = head[1] & 0x7f;
            if (head[1] & 0x80) return false; // servers do not mask
            if (length >= 126) {
                uint8_t ext[8];
                const size_t n = length == 126 ? 2 : 8;
                if (!ReadExact(ext, n)) return false;
                length = 0;
                for (size_t i = 0; i < n; ++i) length = length << 8 | ext[i];
            }
            if (length > 4 * 1024 * 1024 || text.size() + length > 4 * 1024 * 1024) return false;
            std::string payload(size_t(length), '\0');
            if (length && !ReadExact(reinterpret_cast<uint8_t*>(payload.data()), size_t(length))) return false;
            if (opcode == 8) {
                Frame(8, payload.data(), std::min<size_t>(payload.size(), 2));
                return false;
            }
            if (opcode == 9) {
                Frame(10, payload.data(), payload.size());
                continue;
            }
            if (opcode == 10) continue;
            if (opcode != 0 && opcode != 1 && opcode != 2) return false;
            text += payload;
            if (head[0] & 0x80) return true;
        }
    }

    void close() override {
        if (closed_.exchange(true)) return;
        ::shutdown(fd_, SHUT_RDWR);
    }

private:
    bool Wait(short events, int timeout_ms) {
        pollfd pfd{fd_, events, 0};
        return poll(&pfd, 1, timeout_ms) == 1 && !closed_ && !(pfd.revents & (POLLERR | POLLNVAL));
    }

    int fd_;
    SSL_CTX* context_;
    SSL* ssl_;
    std::atomic<bool> closed_{false};
    std::mutex send_mutex_;
    std::vector<uint8_t> pending_;

    friend std::unique_ptr<WebSocket> WebSocket::connect(const std::string&, int, std::string*);
};

} // namespace

std::unique_ptr<WebSocket> WebSocket::connect(const std::string& url, int timeout_ms, std::string* error) {
    WsUrl parts;
    if (!parse_ws_url(url, parts, error)) return nullptr;
    const int fd = Connect(parts, timeout_ms, error);
    if (fd < 0) return nullptr;
    SSL_CTX* context = nullptr;
    SSL* ssl = nullptr;
    if (parts.tls) {
        context = SSL_CTX_new(TLS_client_method());
        if (context) {
            SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
            SSL_CTX_set_default_verify_paths(context);
            SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
            ssl = SSL_new(context);
        }
        if (!ssl) {
            if (error) *error = "TLS is unavailable";
            if (context) SSL_CTX_free(context);
            ::close(fd);
            return nullptr;
        }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, parts.host.c_str());
        SSL_set1_host(ssl, parts.host.c_str());
    }
    auto socket = std::make_unique<PosixSocket>(fd, context, ssl);
    if (ssl) {
        for (;;) {
            const int r = SSL_connect(ssl);
            if (r == 1) break;
            const int e = SSL_get_error(ssl, r);
            if ((e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) || !socket->Wait(e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, timeout_ms)) {
                if (error) *error = "the lobby's TLS certificate was not accepted or the handshake failed";
                return nullptr;
            }
        }
    }
    uint8_t nonce[16];
    random_bytes(nonce, sizeof(nonce));
    const std::string key = Base64(nonce, sizeof(nonce));
    const bool default_port = parts.port == (parts.tls ? 443 : 80);
    const std::string request = "GET " + parts.path + " HTTP/1.1\r\nHost: " + parts.host +
                                (default_port ? "" : ":" + std::to_string(parts.port)) +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                                "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: DragonsCrownProPC\r\n\r\n";
    if (!socket->Write(request.data(), request.size())) {
        if (error) *error = "the lobby closed the connection";
        return nullptr;
    }
    std::string response;
    while (response.find("\r\n\r\n") == std::string::npos) {
        uint8_t chunk[2048];
        size_t got = 0;
        if (response.size() > 16384 || !socket->Read(chunk, sizeof(chunk), got, timeout_ms)) {
            if (error) *error = "the lobby did not answer the WebSocket upgrade";
            return nullptr;
        }
        response.append(reinterpret_cast<char*>(chunk), got);
    }
    const size_t end = response.find("\r\n\r\n") + 4;
    socket->pending_.assign(response.begin() + ptrdiff_t(end), response.end());
    response.resize(end);
    const std::string expected_source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(expected_source.data()), expected_source.size(), digest);
    const std::string accept = Base64(digest, sizeof(digest));
    std::string lower = response;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (response.rfind("HTTP/1.1 101", 0) != 0) {
        if (error) {
            *error = response.rfind("HTTP/1.1 429", 0) == 0 ? "the lobby has too many connections from this address"
                                                            : "the lobby answered " + response.substr(0, response.find('\r'));
        }
        return nullptr;
    }
    if (response.find(accept) == std::string::npos || lower.find("upgrade: websocket") == std::string::npos) {
        if (error) *error = "the lobby's WebSocket answer is invalid";
        return nullptr;
    }
    return socket;
}

} // namespace dcp::net
