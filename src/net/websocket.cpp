#include "net/websocket.h"

#include <cstdlib>

namespace dcp::net {

bool parse_ws_url(const std::string& url, WsUrl& out, std::string* error) {
    auto fail = [error](const char* why) {
        if (error) *error = why;
        return false;
    };
    std::string rest;
    if (url.rfind("ws://", 0) == 0) {
        out.tls = false, rest = url.substr(5);
    } else if (url.rfind("wss://", 0) == 0) {
        out.tls = true, rest = url.substr(6);
    } else {
        return fail("the lobby address must start with ws:// or wss://");
    }
    const size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    out.port = out.tls ? 443 : 80;
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        char* end = nullptr;
        const long port = std::strtol(authority.c_str() + colon + 1, &end, 10);
        if (!end || *end || port <= 0 || port > 65535) return fail("the lobby address has an invalid port");
        out.port = uint16_t(port);
        authority.resize(colon);
    }
    if (authority.empty()) return fail("the lobby address has no host");
    for (const char c : authority)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return fail("the lobby address has an invalid host");
    out.host = authority;
    return true;
}

} // namespace dcp::net
