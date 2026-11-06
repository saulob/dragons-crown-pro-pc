// Discord Rich Presence over Discord's local IPC (named pipe on Windows, unix socket on Linux). No SDK. A missing or
// closed Discord is retried quietly; closing the game closes the connection, and Discord then clears the presence itself.
#include "discord_presence.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "../gpu/shim/dcp_settings.h"
#include "cpk_patch.h"
#include "dcp_party.h"
#include "discord_core.h"
#include "runtime_online.h"
#include "utf8.h"

extern "C" { // runtime.h has no C++ linkage block
int runtime_pad_slot_human(int slot);
void runtime_file_set_read_observer(void (*observe)(const char* guest, uint64_t offset, uint64_t size));
}

#ifndef DCP_DISCORD_CLIENT_ID
#define DCP_DISCORD_CLIENT_ID ""
#endif

namespace {

using namespace dcp::discord;

#ifdef _WIN32
class Link {
public:
    bool Open() const { return handle_ != INVALID_HANDLE_VALUE; }
    bool Connect(const std::string&) {
        for (int i = 0; i < 10 && !Open(); ++i) {
            const std::string pipe = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
            handle_ = CreateFileA(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        return Open();
    }
    int64_t Send(const void* data, size_t size) {
        DWORD done = 0;
        return WriteFile(handle_, data, static_cast<DWORD>(size), &done, nullptr) ? done : -1;
    }
    int64_t Receive(void* data, size_t size) {
        DWORD done = 0;
        return ReadFile(handle_, data, static_cast<DWORD>(size), &done, nullptr) ? done : -1;
    }
    void Close() {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
uint32_t Pid() { return GetCurrentProcessId(); }
#else
class Link {
public:
    bool Open() const { return fd_ >= 0; }
    bool Connect(const std::string& dir) {
        // Native, Flatpak and Snap Discord.
        for (const char* sub : {"/", "/app/com.discordapp.Discord/", "/snap.discord/"}) {
            for (int i = 0; i < 10; ++i) {
                sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s%sdiscord-ipc-%d", dir.c_str(), sub, i);
                const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
                if (fd < 0) return false;
                if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    fd_ = fd;
                    return true;
                }
                close(fd);
            }
        }
        return false;
    }
    int64_t Send(const void* data, size_t size) {
        ssize_t n;
        do {
            n = send(fd_, data, size, MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);
        return n;
    }
    int64_t Receive(void* data, size_t size) {
        ssize_t n;
        do {
            n = recv(fd_, data, size, 0);
        } while (n < 0 && errno == EINTR);
        return n;
    }
    void Close() {
        close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};
uint32_t Pid() { return static_cast<uint32_t>(getpid()); }
#endif

// Game threads may still be reading while exit() runs destructors: the index is never freed.
std::atomic<const AreaIndex*> g_areas{nullptr};
std::atomic<int> g_place{-1};

// A new place is logged, so the mapping can be checked against play. With DCP_DISCORD_TRACE=1 the other archive
// entries are tracked too (place -1) and each one read is logged, to find what a part of the game loads.
void Enter(const Area& a) {
    if (a.place < 0) {
        static std::atomic<const Area*> last{nullptr};
        if (last.exchange(&a, std::memory_order_relaxed) != &a) std::printf("Discord: read %s\n", a.path.c_str());
        return;
    }
    if (g_place.exchange(a.place, std::memory_order_relaxed) != a.place)
        std::printf("Discord: place %s (%s)\n", kPlaces[a.place].name, a.path.c_str());
}

void ObserveRead(const char* guest, uint64_t offset, uint64_t size) noexcept {
    const AreaIndex* areas = g_areas.load(std::memory_order_acquire);
    if (const Area* area = areas ? areas->Find(guest, offset, size) : nullptr) Enter(*area);
}

std::string Utf8Name(const std::filesystem::path& path) {
    const std::u8string name = path.filename().u8string();
    return {name.begin(), name.end()};
}

void TrackArchive(const std::filesystem::path& file, bool trace, AreaIndex& areas) {
    dcp::CpkArchive archive;
    if (!archive.Open(file)) return;
    const std::string name = Utf8Name(file);
    for (const dcp::CpkEntry& entry : archive.entries()) {
        const int place = PlaceOf(entry.path);
        if (place >= 0 || (trace && entry.path.rfind("snd/", 0) != 0)) // streamed sound would flood the trace
            areas.Add(name, entry.offset, entry.size, place, entry.path);
    }
}

void LoadPlaces(const std::filesystem::path& app0, bool trace) noexcept {
    try {
        auto areas = std::make_unique<AreaIndex>();
        std::error_code ec;
        for (std::filesystem::directory_iterator it(app0, ec), end; !ec && it != end; it.increment(ec)) {
            std::string extension = Utf8Name(it->path().extension());
            for (char& c : extension) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (extension == ".CPK") TrackArchive(it->path(), trace, *areas);
        }
        areas->Seal();
        std::printf("Discord: %zu area files tracked\n", areas->size());
        if (!areas->size()) return;
        g_areas.store(areas.release(), std::memory_order_release);
        runtime_file_set_read_observer(ObserveRead);
    } catch (...) {
        std::printf("Discord: the game's archives could not be read, no places\n");
    }
}

// This PC's player and the party size. Class and level come from the player-unit tick (dcp_party.h), so they are
// known only while a character is on screen and on a build with that tick; offline, AI allies (party slots without a
// controller) are not counted.
Status Current() {
    Status s;
    s.online = runtime_online_active() != 0;
    s.place = g_place.load(std::memory_order_relaxed);
    const dcp::online::RoomView room = s.online ? dcp::online::room_view() : dcp::online::RoomView{};
    const int64_t now = dcp::party::NowMs();
    const dcp::party::UnitTable& units = dcp::party::SharedUnits();
    if (const auto cls = units.ClassAt(room.slot, now)) {
        s.cls = *cls;
        s.level = units.LevelAt(room.slot, now).value_or(0);
    }
    int humans = 0;
    for (int slot = 0; slot < kPartyLimit; ++slot) humans += runtime_pad_slot_human(slot) ? 1 : 0;
    s.players = PlayerCount(s.online, room.members, humans);
    return s;
}

void LogAnswer(const char* what, const std::string& answer) {
    std::printf("Discord: %s: %.200s\n", what, answer.c_str());
}

constexpr auto kPollInterval = std::chrono::seconds(5);
constexpr auto kRefusedInterval = std::chrono::seconds(60);
constexpr int kRefreshPolls = 12; // sending is the only thing that notices Discord restarting while nothing changes

void Run(const std::string& client_id, const std::string& socket_dir) {
    const long long start = static_cast<long long>(std::time(nullptr));
    Link link;
    std::string sent, answer;
    unsigned nonce = 0;
    int unchanged = 0;
    bool logged_refusal = false, logged_error = false;
    auto wait = kPollInterval;
    for (;; std::this_thread::sleep_for(wait)) {
        wait = kPollInterval;
        if (!link.Open()) {
            if (!link.Connect(socket_dir)) continue;
            const Reply reply = Exchange(link, kOpHandshake, Handshake(client_id), answer);
            if (reply != Reply::kAnswered) {
                link.Close();
                if (reply == Reply::kClosed) {
                    wait = kRefusedInterval;
                    if (!logged_refusal) LogAnswer("refused", answer);
                    logged_refusal = true;
                }
                continue;
            }
            std::printf("Discord: connected\n");
            sent.clear();
        }
        const std::string activity = Activity(Current(), start, Pid());
        if (activity == sent && ++unchanged < kRefreshPolls) continue;
        unchanged = 0;
        if (Exchange(link, kOpFrame, SetActivity(++nonce, activity), answer) != Reply::kAnswered) {
            std::printf("Discord: connection lost\n");
            link.Close();
            continue;
        }
        if (ReportsError(answer) && !logged_error) {
            LogAnswer("error", answer);
            logged_error = true;
        }
        sent = activity;
    }
}

void Worker(std::string client_id, std::string socket_dir, std::filesystem::path app0, bool trace) noexcept {
    LoadPlaces(app0, trace);
    try {
        Run(client_id, socket_dir);
    } catch (...) {
        std::printf("Discord: presence stopped\n");
    }
}

bool ReadConfig(const char* settings_file, std::string& client_id) {
    bool enabled = true;
    std::ifstream file(dcp::PathFromUtf8(settings_file ? settings_file : ""));
    for (std::string line, key, value; std::getline(file, line);) {
        if (!DcpSettings::ParseLine(line, key, value)) continue;
        if (key == "discord_presence") enabled = DcpSettings::ParseBool(value, enabled);
        if (key == "discord_client_id") client_id = value;
    }
    return enabled;
}

std::string SocketDir() {
#ifndef _WIN32
    for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
        if (const char* dir = std::getenv(var)) return dir;
#endif
    return "/tmp";
}

} // namespace

// The thread is detached and never joined. Closing the window and the fatal paths end in _Exit or TerminateProcess;
// exit() runs destructors under it, but it then reads only leaked or trivially destructible state, and a destroyed room
// lock that throws ends it in Worker.
extern "C" int discord_presence_start(const char* settings_file, const char* app0) {
    try {
        std::string client_id = DCP_DISCORD_CLIENT_ID;
        if (!ReadConfig(settings_file, client_id) || !ValidClientId(client_id)) {
            std::printf("Discord: presence off\n");
            return 0;
        }
        const char* trace = std::getenv("DCP_DISCORD_TRACE");
        std::thread(Worker, client_id, SocketDir(), app0 ? dcp::PathFromUtf8(app0) : std::filesystem::path(),
                    trace && *trace == '1')
            .detach();
        return 1;
    } catch (...) {
        std::printf("Discord: presence off, it could not start\n");
        return 0;
    }
}
