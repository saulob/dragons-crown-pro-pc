#include "net/identity.h"

#include <fstream>
#include <sstream>
#include <system_error>

#include "net/crypto.h"
#include "net/json.h"

namespace dcp::net {
namespace fs = std::filesystem;
namespace {

bool IsHex(const std::string& s) {
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

Identity Read(const fs::path& file) {
    Identity id;
    id.file = file;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        id.error = "cannot read " + file.string();
        return id;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "account") id.account = Json(value).as_u64();
        if (key == "secret" && value.size() == 64 && IsHex(value)) id.secret = value;
    }
    if (!id.ok()) {
        id.account = 0;
        id.secret.clear();
        id.error = file.string() + " is damaged; delete it to make a new online identity";
    }
    return id;
}

} // namespace

Identity load_identity(const fs::path& user_dir, bool create) {
    const fs::path file = user_dir / kIdentityFileName;
    std::error_code ec;
    if (fs::exists(file, ec) || !create) return Read(file);
    uint8_t secret[32];
    uint64_t account = 0;
    while (account == 0) account = random_u64() >> 1;
    if (!random_bytes(secret, sizeof(secret))) {
        Identity id;
        id.error = "the system random generator failed";
        return id;
    }
    fs::create_directories(user_dir, ec);
    const fs::path temp = user_dir / (std::string(kIdentityFileName) + ".new" + std::to_string(random_u64() & 0xffffff));
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << "; Online identity of this installation. Keep it private: anyone with this file can play as you.\n"
            << "; Deleting it makes a new account (friends and friend code are lost).\n"
            << "account=" << u64_text(account) << "\n"
            << "secret=" << hex_encode(secret, sizeof(secret)) << "\n";
        if (!out) {
            Identity id;
            id.error = "cannot write " + temp.string();
            return id;
        }
    }
    // A link fails when another process created the file first; then its identity is the one kept.
    fs::create_hard_link(temp, file, ec);
    if (ec && !fs::exists(file)) {
        ec.clear();
        fs::rename(temp, file, ec);
    }
    fs::remove(temp, ec);
    return Read(file);
}

std::string online_name(const std::string& wanted, const std::string& fallback) {
    std::string out;
    for (const char c : wanted) {
        if (out.size() == 16) break;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) out += c;
        else if (c == ' ' || static_cast<unsigned char>(c) >= 0x80) {
            if (!out.empty() && out.back() != '_') out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? fallback : out;
}

} // namespace dcp::net
