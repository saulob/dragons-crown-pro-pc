// The production entry points the wheel calls (comms_session_send / available, and the pump hook the online runtime
// drives), linked from the real src/comms_session.cpp. Only the runtime behind them is stubbed.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "comms_bridge.h"
#include "comms_session.h"
#include "net/peer_socket.h"
#include "runtime_online.h"

namespace {

int g_failures = 0;
#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) {                                          \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

struct World {
    uint64_t now = 100000;
    dcp::online::CommsRoom room;
    bool link_open = true;
    std::vector<std::pair<int, int>> shown;
    std::vector<std::array<uint8_t, 4>> datagrams;
    std::vector<std::array<uint64_t, 8>> reports;
    std::vector<std::array<uint8_t, 4>> inbox;
    dcp::online::CommsReportResult result;
    void (*hook)() = nullptr;
} w;

} // namespace

namespace dcp::net {
uint64_t now_ms() { return w.now; }
bool PeerSocket::send_comms(uint64_t, const uint8_t* body, size_t) {
    std::array<uint8_t, 4> copy{};
    std::copy(body, body + 4, copy.begin());
    w.datagrams.push_back(copy);
    return true;
}
std::vector<PeerSocket::CommsPacket> PeerSocket::take_comms() {
    std::vector<CommsPacket> out;
    for (const auto& body : w.inbox) {
        CommsPacket packet;
        packet.account = 2002;
        std::copy(body.begin(), body.end(), packet.body.begin());
        out.push_back(packet);
    }
    w.inbox.clear();
    return out;
}
} // namespace dcp::net

namespace dcp::online {
CommsRoom comms_room() { return w.room; }
bool comms_report_open() { return w.link_open; }
bool comms_report_send(const std::array<uint64_t, 8>& counts) {
    w.reports.push_back(counts);
    return true;
}
CommsReportResult comms_report_result() { return std::exchange(w.result, CommsReportResult{}); }
void set_pump_hook(void (*hook)()) { w.hook = hook; }
std::shared_ptr<net::PeerSocket> peer_socket() { return std::shared_ptr<net::PeerSocket>(reinterpret_cast<net::PeerSocket*>(1), [](net::PeerSocket*) {}); }
} // namespace dcp::online

extern "C" {
int comms_bridge_active(void) { return 1; }
int comms_bridge_unit_running(int) { return 1; }
void comms_bridge_show(int slot, int id) { w.shown.emplace_back(slot, id); }
}

namespace {

std::string Source(const char* relative) {
    std::ifstream in(std::string(DCP_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

const char* const kLog = "comms_runtime_test.log";
std::string Captured;

std::string LogText() {
    std::fflush(stdout);
    std::ifstream in(kLog, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void Capture(void (*body)()) {
    const size_t before = LogText().size();
    body();
    Captured = LogText().substr(before);
}

size_t Count(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

void Reset() {
    static uint64_t epoch = 0;
    epoch += 10000000;
    w = World{};
    w.now = epoch;
    w.room.slot = 1;
    w.room.members = 4;
    w.room.peers = {{2002, 0}, {3003, 2}, {4004, 3}};
}

uint64_t Total(const std::array<uint64_t, 8>& c) {
    uint64_t t = 0;
    for (uint64_t n : c) t += n;
    return t;
}

void TestProductionSendCountsOnce() {
    Reset();
    comms_session_register();
    CHECK(w.hook != nullptr);
    CHECK(comms_session_available() == 1);
    Capture([] {
        CHECK(comms_session_send(5) == 1);
        CHECK(w.shown.size() == 1 && w.shown[0] == std::make_pair(1, 5));
        CHECK(w.datagrams.size() == 3);
        w.now += 1999;
        CHECK(comms_session_send(0) == 0);
        w.now += 1;
        CHECK(comms_session_send(0) == 1);
    });
    CHECK(Count(Captured, "Comms Wheel telemetry: report pending") == 1);
    CHECK(w.reports.empty());
    w.now += 5000;
    Capture([] { w.hook(); });
    CHECK(w.reports.size() == 1 && Total(w.reports[0]) == 2 && w.reports[0][5] == 1 && w.reports[0][0] == 1);
    w.result.accepted = true;
    Capture([] { w.hook(); });
    CHECK(Count(Captured, "reported 2 use(s)") == 1);
}

void TestEveryIdCountsOnce() {
    Reset();
    comms_session_register();
    Capture([] {
        for (int id = 0; id < 8; ++id) {
            CHECK(comms_session_send(id) == 1);
            w.now += 2000;
        }
        CHECK(comms_session_send(8) == 0 && comms_session_send(-1) == 0);
    });
    w.now += 5000;
    Capture([] { w.hook(); });
    CHECK(w.reports.size() == 1 && Total(w.reports[0]) == 8);
    for (uint64_t n : w.reports[0]) CHECK(n == 1);
    w.result.accepted = true;
    Capture([] { w.hook(); });
}

void TestReceiveCountsNothing() {
    Reset();
    comms_session_register();
    w.inbox = {{1, 0, 5, 7}, {1, 0, 5, 7}, {1, 0, 5, 7}};
    Capture([] { w.hook(); });
    CHECK(w.shown.size() == 1 && w.shown[0] == std::make_pair(0, 5));
    CHECK(Count(Captured, "report pending") == 0);
    w.now += 200000;
    Capture([] { w.hook(); });
    CHECK(w.reports.empty());
}

void TestNotAvailableCountsNothing() {
    Reset();
    comms_session_register();
    w.room.members = 1;
    CHECK(comms_session_available() == 0 && comms_session_send(3) == 0);
    w.now += 100000;
    Capture([] { w.hook(); });
    CHECK(w.reports.empty() && w.shown.empty() && w.datagrams.empty());
}

void TestWheelReachesOnlyTheCanonicalSend() {
    const std::string pad = Source("src/runtime_pad.c");
    CHECK(pad.find("comms_session_send(out.send)") != std::string::npos);
    size_t calls = 0;
    for (size_t at = pad.find("comms_session_send("); at != std::string::npos; at = pad.find("comms_session_send(", at + 1)) ++calls;
    CHECK(calls == 1);
    CHECK(Source("gpu/shim/dcp_comms_wheel.cpp").find("comms_session_send") == std::string::npos);
    CHECK(Source("gpu/shim/dcp_overlay.cpp").find("send_comms") == std::string::npos);

    const char* files[] = {"src/runtime_pad.c", "src/runtime_matching2.cpp", "src/runtime_rudp.cpp", "src/runtime_online.cpp",
                           "src/comms_bridge.cpp", "gpu/shim/dcp_comms_wheel.cpp", "gpu/shim/dcp_overlay.cpp"};
    for (const char* file : files) CHECK(Source(file).find("send_comms(") == std::string::npos);
    const std::string session = Source("src/comms_session.cpp");
    CHECK(session.find("send_comms(") != std::string::npos);
    const std::string logic = Source("src/comms_session_logic.h");
    CHECK(logic.find("NoteLocalSend(id, now);") != std::string::npos);
    size_t notes = 0;
    for (size_t at = logic.find("NoteLocalSend("); at != std::string::npos; at = logic.find("NoteLocalSend(", at + 1)) ++notes;
    CHECK(notes == 2);
    CHECK(logic.find("report_.Add(") == logic.rfind("report_.Add("));
}

} // namespace

int main() {
    std::freopen(kLog, "wb", stdout);
    TestProductionSendCountsOnce();
    TestEveryIdCountsOnce();
    TestReceiveCountsNothing();
    TestNotAvailableCountsNothing();
    TestWheelReachesOnlyTheCanonicalSend();
    std::fprintf(stderr, "%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
