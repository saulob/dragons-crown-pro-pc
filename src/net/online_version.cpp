#include "net/online_version.h"

#include <fstream>

#ifndef DCP_PORT_VERSION
#error "DCP_PORT_VERSION comes from the CMake project VERSION"
#endif

namespace dcp::net {
#ifndef _WIN32
namespace {

std::string FirstLine(const char* path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

} // namespace
#endif

bool online_compatible(int remote_protocol) { return remote_protocol == kOnlineProtocolVersion; }

const std::string& port_version() {
    static const std::string version = DCP_PORT_VERSION;
    return version;
}

std::string platform_name(bool windows, const std::string& dmi_vendor, const std::string& dmi_product) {
    if (windows) return "Windows";
    if (dmi_vendor == "Valve" && (dmi_product == "Jupiter" || dmi_product == "Galileo")) return "Steam Deck";
    return "Linux";
}

const std::string& host_platform() {
#ifdef _WIN32
    static const std::string platform = platform_name(true, "", "");
#else
    static const std::string platform =
        platform_name(false, FirstLine("/sys/class/dmi/id/sys_vendor"), FirstLine("/sys/class/dmi/id/product_name"));
#endif
    return platform;
}

std::string system_type_text(const std::string& platform, const std::string& version) {
    return version.empty() ? platform : platform + " \xC2\xB7 v" + version;
}

} // namespace dcp::net
