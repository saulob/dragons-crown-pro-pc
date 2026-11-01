// A WebSocket client for the lobby: ws:// and wss:// (WinHTTP on Windows, sockets and OpenSSL elsewhere). Text
// and binary messages. One thread may receive while others send; close() from any thread ends a blocked receive().
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dcp::net {

struct WsUrl {
    bool tls = false;
    std::string host;
    uint16_t port = 0;
    std::string path = "/";
};

// False with the reason for anything but ws://host[:port][/path] or wss://...
bool parse_ws_url(const std::string& url, WsUrl& out, std::string* error);

// The first of the Linux distributions' CA bundles that exists, or empty. The AppImage's OpenSSL only knows its build
// prefix (/usr/local/ssl), which players' systems do not have.
std::string system_ca_bundle(const std::function<bool(const std::string&)>& exists);

class WebSocket {
public:
    virtual ~WebSocket() = default;

    // nullptr with the reason when the server cannot be reached or refuses the upgrade.
    static std::unique_ptr<WebSocket> connect(const std::string& url, int timeout_ms, std::string* error);

    virtual bool send_text(const std::string& text) = 0;
    virtual bool send_binary(const uint8_t* data, size_t size) = 0;
    // A whole message, and whether it was binary; false once the connection is closed or broken.
    virtual bool receive(std::string& data, bool& binary) = 0;
    virtual void close() = 0;
};

} // namespace dcp::net
