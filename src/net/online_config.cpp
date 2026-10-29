#include "net/online_config.h"

#include <cstdlib>

namespace dcp::net {
namespace {

const char* Env(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? value : nullptr;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
    return s.substr(start);
}

} // namespace

bool parse_bool(const std::string& value, bool fallback) {
    const std::string v = Trim(value);
    if (v == "1" || v == "on" || v == "true" || v == "yes") return true;
    if (v == "0" || v == "off" || v == "false" || v == "no") return false;
    return fallback;
}

void apply_environment(OnlineConfig& config) {
    if (const char* v = Env("DCP_ONLINE")) config.enabled = parse_bool(v, config.enabled);
    if (const char* v = Env("DCP_LOBBY")) config.lobby = v;
    if (const char* v = Env("DCP_ONLINE_NAME")) config.name = v;
    if (const char* v = Env("DCP_UPNP")) config.upnp = parse_bool(v, config.upnp);
    if (const char* v = Env("DCP_UDP_PORT")) {
        const long port = std::strtol(v, nullptr, 10);
        if (port >= 0 && port <= 65535) config.udp_port = uint16_t(port);
    }
    if (const char* v = Env("DCP_STUN")) config.stun = v;
}

std::vector<std::pair<std::string, uint16_t>> stun_servers(const std::string& list) {
    std::vector<std::pair<std::string, uint16_t>> out;
    if (Trim(list) == "off") return out;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        const std::string item = Trim(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!item.empty()) {
            const size_t colon = item.rfind(':');
            uint16_t port = 3478;
            std::string host = item;
            if (colon != std::string::npos) {
                const long p = std::strtol(item.c_str() + colon + 1, nullptr, 10);
                host = item.substr(0, colon);
                if (p > 0 && p <= 65535) port = uint16_t(p);
            }
            if (!host.empty()) out.emplace_back(host, port);
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

} // namespace dcp::net
