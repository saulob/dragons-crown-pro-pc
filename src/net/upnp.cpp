#include "net/upnp.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef _WIN32
#include <poll.h>
#endif

namespace dcp::net::upnp {
namespace {

constexpr char kDescription[] = "Dragon's Crown Pro";
constexpr uint32_t kLeaseSeconds = 3600;

struct Gateway {
    Endpoint http;        // the router's description server
    std::string control;  // control URL path
    std::string service;  // service type
    uint32_t lan_ip = 0;  // our address towards it
};

struct HttpUrl {
    Endpoint at;
    std::string path;
};

bool ParseHttpUrl(const std::string& url, HttpUrl& out) {
    if (url.rfind("http://", 0) != 0) return false;
    const std::string rest = url.substr(7);
    const size_t slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t colon = authority.find(':');
    out.at.ip = parse_ip(authority.substr(0, colon));
    out.at.port = colon == std::string::npos ? 80 : uint16_t(std::strtoul(authority.c_str() + colon + 1, nullptr, 10));
    return out.at.valid();
}

bool WaitSocket(socket_t s, bool write, int timeout_ms) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv) == 1;
#else
    pollfd pfd{s, short(write ? POLLOUT : POLLIN), 0};
    return poll(&pfd, 1, timeout_ms) == 1;
#endif
}

// One HTTP/1.1 request to a LAN address; the status code, or 0 when it failed.
int Http(const Endpoint& to, const std::string& request, std::string& body) {
    body.clear();
    const socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) return 0;
    set_nonblocking(s);
    const sockaddr_in a = to_sockaddr(to);
    int status = 0;
    if (connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) == 0 || would_block(last_socket_error())) {
        if (WaitSocket(s, true, 3000)) {
            size_t sent = 0;
            while (sent < request.size() && WaitSocket(s, true, 3000)) {
                const int n = int(send(s, request.data() + sent, int(request.size() - sent), 0));
                if (n <= 0) break;
                sent += size_t(n);
            }
            std::string response;
            const uint64_t deadline = now_ms() + 5000;
            while (sent == request.size() && response.size() < 256 * 1024 && now_ms() < deadline && WaitSocket(s, false, 3000)) {
                char chunk[4096];
                const int n = int(recv(s, chunk, sizeof(chunk), 0));
                if (n <= 0) break;
                response.append(chunk, size_t(n));
            }
            unsigned code = 0;
            if (std::sscanf(response.c_str(), "HTTP/1.%*u %u", &code) == 1) status = int(code);
            const size_t end = response.find("\r\n\r\n");
            if (end != std::string::npos) body = response.substr(end + 4);
        }
    }
    close_socket(s);
    return status;
}

std::string Tag(const std::string& xml, const std::string& name, size_t from = 0, size_t* at = nullptr) {
    const std::string open = "<" + name + ">", close = "</" + name + ">";
    const size_t start = xml.find(open, from);
    if (start == std::string::npos) return {};
    const size_t end = xml.find(close, start);
    if (end == std::string::npos) return {};
    if (at) *at = end;
    std::string value = xml.substr(start + open.size(), end - start - open.size());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\n' || value.back() == '\r')) value.pop_back();
    while (!value.empty() && (value.front() == ' ' || value.front() == '\n' || value.front() == '\r')) value.erase(0, 1);
    return value;
}

// Routers answer SSDP with the URL of their device description; only LAN routers that answered themselves count.
std::vector<HttpUrl> Discover(uint32_t lan_ip) {
    std::vector<HttpUrl> found;
    const socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return found;
    const sockaddr_in local = to_sockaddr({lan_ip, 0});
    bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
    const sockaddr_in group = to_sockaddr({parse_ip("239.255.255.250"), 1900});
    for (const char* target : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                               "urn:schemas-upnp-org:service:WANIPConnection:1",
                               "urn:schemas-upnp-org:device:InternetGatewayDevice:2"}) {
        const std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") +
                                   target + "\r\n\r\n";
        sendto(s, search.data(), int(search.size()), 0, reinterpret_cast<const sockaddr*>(&group), sizeof(group));
    }
    const uint64_t deadline = now_ms() + 2200;
    while (now_ms() < deadline && found.size() < 4) {
        if (!WaitSocket(s, false, int(deadline - std::min(deadline, now_ms())))) break;
        char buffer[2048];
        sockaddr_in from{};
        socklen_t length = sizeof(from);
        const int n = int(recvfrom(s, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&from), &length));
        if (n <= 0) continue;
        buffer[n] = 0;
        std::string reply(buffer, size_t(n)), lower = reply;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const size_t at = lower.find("\nlocation:");
        if (at == std::string::npos) continue;
        size_t start = at + 10;
        while (start < reply.size() && reply[start] == ' ') ++start;
        const std::string location = reply.substr(start, reply.find('\r', start) - start);
        HttpUrl url;
        if (!ParseHttpUrl(location, url) || url.at.ip != from_sockaddr(from).ip || !is_lan(url.at.ip)) continue;
        if (std::none_of(found.begin(), found.end(), [&](const HttpUrl& u) { return u.at == url.at && u.path == url.path; }))
            found.push_back(url);
    }
    close_socket(s);
    return found;
}

bool Describe(const HttpUrl& location, Gateway& out) {
    std::string xml;
    const std::string request = "GET " + location.path + " HTTP/1.1\r\nHost: " + endpoint_text(location.at) +
                                "\r\nConnection: close\r\n\r\n";
    if (Http(location.at, request, xml) != 200) return false;
    for (size_t at = 0;;) {
        const size_t service = xml.find("<service>", at);
        if (service == std::string::npos) return false;
        size_t end = 0;
        const std::string block = xml.substr(service, xml.find("</service>", service) - service);
        at = service + 9;
        const std::string type = Tag(block, "serviceType", 0, &end);
        if (type.find(":WANIPConnection:") == std::string::npos && type.find(":WANPPPConnection:") == std::string::npos) continue;
        std::string control = Tag(block, "controlURL");
        if (control.rfind("http://", 0) == 0) {
            HttpUrl absolute;
            if (!ParseHttpUrl(control, absolute) || absolute.at.ip != location.at.ip) continue;
            out.http = absolute.at;
            control = absolute.path;
        } else {
            out.http = location.at;
            if (control.empty() || control[0] != '/') control = "/" + control;
        }
        out.control = control;
        out.service = type;
        return true;
    }
}

// A SOAP action; the response body, and the UPnP error code (0 = success, -1 = no answer).
int Soap(const Gateway& g, const std::string& action, const std::string& args, std::string& response) {
    const std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action +
                             " xmlns:u=\"" + g.service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    const std::string request = "POST " + g.control + " HTTP/1.1\r\nHost: " + endpoint_text(g.http) +
                                "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + g.service + "#" + action +
                                "\"\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    const int status = Http(g.http, request, response);
    if (status == 200) return 0;
    if (status == 0) return -1;
    const std::string code = Tag(response, "errorCode");
    return code.empty() ? 500 : std::atoi(code.c_str());
}

std::string MappingArgs(uint16_t external, uint16_t internal, uint32_t lan_ip, uint32_t lease) {
    return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(external) +
           "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + std::to_string(internal) +
           "</NewInternalPort><NewInternalClient>" + ip_text(lan_ip) +
           "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" + kDescription +
           "</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>";
}

bool FindGateway(Gateway& g, std::string& detail) {
    const uint32_t lan = default_route_ip();
    if (!lan || !is_lan(lan)) {
        detail = lan ? "this PC is not behind a home router" : "no network route";
        return false;
    }
    for (const HttpUrl& location : Discover(lan)) {
        if (Describe(location, g)) {
            g.lan_ip = lan;
            return true;
        }
    }
    detail = "no UPnP router answered";
    return false;
}

// Adds the mapping (trying a few external ports on conflicts); the external port, or 0.
uint16_t AddMapping(const Gateway& g, uint16_t local_port, uint32_t& lease, std::string& detail) {
    std::string response;
    for (uint16_t offset = 0; offset < 5; ++offset) {
        const uint16_t external = uint16_t(local_port + offset);
        int error = Soap(g, "AddPortMapping", MappingArgs(external, local_port, g.lan_ip, lease), response);
        if (error == 725) {
            lease = 0;
            error = Soap(g, "AddPortMapping", MappingArgs(external, local_port, g.lan_ip, lease), response);
        }
        if (error == 0) return external;
        if (error != 718 && error != 727) {
            detail = error < 0 ? "the router did not answer" : "the router refused the mapping (error " + std::to_string(error) + ")";
            return 0;
        }
    }
    detail = "the router's ports are taken by other mappings";
    return 0;
}

void RemoveMapping(const Gateway& g, uint16_t external) {
    std::string response;
    Soap(g, "DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(external) +
                                     "</NewExternalPort><NewProtocol>UDP</NewProtocol>", response);
}

uint32_t ExternalAddress(const Gateway& g) {
    std::string response;
    if (Soap(g, "GetExternalIPAddress", "", response) != 0) return 0;
    return parse_ip(Tag(response, "NewExternalIPAddress"));
}

} // namespace

PortMapping::~PortMapping() { stop(); }

void PortMapping::start(uint16_t local_port) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) return;
    stopping_ = false;
    status_ = {State::kWorking, {}, "looking for a UPnP router"};
    thread_ = std::thread([this, local_port] { Run(local_port); });
}

void PortMapping::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

Status PortMapping::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void PortMapping::Run(uint16_t local_port) {
    if (!sockets_ready()) return;
    Gateway g;
    std::string detail;
    auto finish = [this](State state, Endpoint external, std::string text) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = {state, external, std::move(text)};
        std::printf("UPnP: %s\n", status_.detail.c_str());
    };
    if (!FindGateway(g, detail)) return finish(State::kFailed, {}, detail);
    uint32_t lease = kLeaseSeconds;
    const uint16_t external = AddMapping(g, local_port, lease, detail);
    if (!external) return finish(State::kFailed, {}, detail);
    const uint32_t wan = ExternalAddress(g);
    finish(State::kMapped, {wan, external},
           "UDP " + std::to_string(external) + " -> " + ip_text(g.lan_ip) + ":" + std::to_string(local_port) + " mapped on " +
               ip_text(g.http.ip) + ", router address " + (wan ? ip_text(wan) : "unknown"));
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        const auto wait = std::chrono::seconds(lease ? lease / 2 : 24 * 3600);
        if (wake_.wait_for(lock, wait, [this] { return stopping_; })) break;
        lock.unlock();
        uint32_t renewed = lease;
        AddMapping(g, local_port, renewed, detail);
        lock.lock();
    }
    lock.unlock();
    RemoveMapping(g, external);
    std::printf("UPnP: UDP %u mapping removed\n", external);
}

Status probe(uint16_t port) {
    Status result;
    if (!sockets_ready()) return {State::kFailed, {}, "no sockets"};
    Gateway g;
    std::string detail;
    if (!FindGateway(g, detail)) return {State::kFailed, {}, detail};
    uint32_t lease = 60;
    const uint16_t external = AddMapping(g, port, lease, detail);
    if (!external) return {State::kFailed, {}, detail};
    const uint32_t wan = ExternalAddress(g);
    RemoveMapping(g, external);
    return {State::kMapped, {wan, external}, "the router accepts port mappings"};
}

} // namespace dcp::net::upnp
