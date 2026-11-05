// Discord Rich Presence over Discord's local IPC (named pipe on Windows, unix socket on Linux). No SDK: a frame is
// [opcode u32][length u32][JSON]. Opcode 0 is the handshake, 1 a command. A missing or closed Discord is retried
// quietly; closing the game closes the connection, and Discord then clears the presence itself.
#include "discord_presence.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "../gpu/shim/dcp_settings.h"
#include "dcp_party.h"
#include "runtime_online.h"

extern "C" int runtime_pad_slot_human(int slot); // runtime.h has no C++ linkage block

#ifndef DCP_DISCORD_CLIENT_ID
#define DCP_DISCORD_CLIENT_ID ""
#endif

namespace {

#ifdef _WIN32
using Handle = HANDLE;
const Handle kNone = INVALID_HANDLE_VALUE;

Handle Connect() {
    for (int i = 0; i < 10; ++i) {
        const std::string pipe = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
        Handle h = CreateFileA(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
    }
    return kNone;
}
bool Write(Handle h, const void* data, uint32_t size) {
    DWORD done = 0;
    return WriteFile(h, data, size, &done, nullptr) && done == size;
}
bool Read(Handle h, void* data, uint32_t size) {
    for (uint32_t got = 0; got < size;) {
        DWORD n = 0;
        if (!ReadFile(h, static_cast<char*>(data) + got, size - got, &n, nullptr) || n == 0) return false;
        got += n;
    }
    return true;
}
void Close(Handle h) { CloseHandle(h); }
uint32_t Pid() { return GetCurrentProcessId(); }
#else
using Handle = int;
const Handle kNone = -1;

Handle Connect() {
    const char* dir = nullptr;
    for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
        if ((dir = std::getenv(var))) break;
    const std::string base = dir ? dir : "/tmp";
    // Native, Flatpak and Snap Discord.
    for (const char* sub : {"/", "/app/com.discordapp.Discord/", "/snap.discord/"}) {
        for (int i = 0; i < 10; ++i) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s%sdiscord-ipc-%d", base.c_str(), sub, i);
            const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd < 0) return kNone;
            if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return fd;
            close(fd);
        }
    }
    return kNone;
}
bool Write(Handle h, const void* data, uint32_t size) {
    return send(h, data, size, MSG_NOSIGNAL) == static_cast<ssize_t>(size);
}
bool Read(Handle h, void* data, uint32_t size) {
    for (uint32_t got = 0; got < size;) {
        const ssize_t n = recv(h, static_cast<char*>(data) + got, size - got, 0);
        if (n <= 0) return false;
        got += static_cast<uint32_t>(n);
    }
    return true;
}
void Close(Handle h) { close(h); }
uint32_t Pid() { return static_cast<uint32_t>(getpid()); }
#endif

// Sends one frame and waits for Discord's reply, so its answers never pile up in the pipe.
bool Exchange(Handle h, uint32_t op, const std::string& json) {
    const uint32_t header[2] = {op, static_cast<uint32_t>(json.size())}; // little endian on every target
    if (!Write(h, header, sizeof(header)) || !Write(h, json.data(), header[1])) return false;
    uint32_t reply[2];
    if (!Read(h, reply, sizeof(reply)) || reply[1] > (1u << 20)) return false;
    std::string body(reply[1], '\0');
    return Read(h, body.data(), reply[1]) && reply[0] == 1; // 2 is Discord closing the connection
}

const char* const kClasses[] = {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"};

// This PC's player and the party size. Class and level come from the player-unit tick (dcp_party.h), so they are
// known only while a character is on screen; offline, AI allies (party slots without a controller) are not counted.
struct Status {
    bool online = false;
    int cls = -1, level = 0, players = 1;
};

Status Current() {
    Status s;
    s.online = runtime_online_active() != 0;
    const dcp::online::RoomView room = s.online ? dcp::online::room_view() : dcp::online::RoomView{};
    const int64_t now = dcp::party::NowMs();
    const dcp::party::UnitTable& units = dcp::party::SharedUnits();
    if (const auto cls = units.ClassAt(room.slot, now)) {
        s.cls = *cls;
        s.level = *units.LevelAt(room.slot, now);
    }
    if (room.members > 0) {
        s.players = room.members;
    } else {
        s.players = 0;
        for (int slot = 0; slot < 4; ++slot) s.players += runtime_pad_slot_human(slot) ? 1 : 0;
    }
    return s;
}

// The SET_ACTIVITY arguments.
std::string Activity(const Status& s, long long start) {
    const std::string details =
        s.cls >= 0 ? std::string(kClasses[s.cls]) + " · Lv " + std::to_string(s.level) : "In menus";
    const std::string state = s.online ? (s.players > 1 ? "Online co-op" : "Online, waiting for players")
                                       : (s.players > 1 ? "Couch co-op" : "Solo");
    // The small image is the class: upload class art named fighter, amazon, elf, dwarf, sorceress, wizard.
    std::string small;
    if (s.cls >= 0) {
        std::string key = kClasses[s.cls];
        for (char& c : key) c = static_cast<char>(c | 0x20);
        small = ",\"small_image\":\"" + key + "\",\"small_text\":\"" + kClasses[s.cls] + "\"";
    }
    return "{\"pid\":" +
           std::to_string(Pid()) + ",\"activity\":{\"details\":\"" + details + "\",\"state\":\"" + state +
           "\",\"timestamps\":{\"start\":" + std::to_string(start) + "},\"party\":{\"id\":\"dcp\",\"size\":[" +
           std::to_string(s.players) + ",4]},\"assets\":{\"large_image\":\"icon\",\"large_text\":\"Dragon's Crown Pro\"" +
           small + "}}}";
}

void Run(std::string client_id) {
    const long long start = static_cast<long long>(std::time(nullptr));
    Handle h = kNone;
    std::string sent;
    for (;; std::this_thread::sleep_for(std::chrono::seconds(5))) {
        if (h == kNone) {
            h = Connect();
            if (h == kNone) continue;
            if (!Exchange(h, 0, "{\"v\":1,\"client_id\":\"" + client_id + "\"}")) {
                Close(h);
                h = kNone;
                continue;
            }
            std::printf("Discord: connected\n");
            sent.clear();
        }
        const std::string activity = Activity(Current(), start);
        if (activity == sent) continue;
        static unsigned nonce = 0; // Discord matches replies by nonce: one per command
        if (!Exchange(h, 1, "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(++nonce) + "\",\"args\":" +
                                activity + "}")) {
            Close(h);
            h = kNone;
            continue;
        }
        sent = activity;
    }
}

} // namespace

extern "C" void discord_presence_start(const char* settings_file) {
    bool enabled = true;
    std::string client_id = DCP_DISCORD_CLIENT_ID;
    std::ifstream file(settings_file ? settings_file : "");
    for (std::string line, key, value; std::getline(file, line);) {
        if (!DcpSettings::ParseLine(line, key, value)) continue;
        if (key == "discord_presence") enabled = DcpSettings::ParseBool(value, enabled);
        if (key == "discord_client_id") client_id = value;
    }
    // The id goes into JSON unescaped: digits only.
    if (!enabled || client_id.empty() || client_id.find_first_not_of("0123456789") != std::string::npos) {
        std::printf("Discord: presence off\n");
        return;
    }
    std::thread(Run, client_id).detach();
}
