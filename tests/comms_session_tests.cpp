// Comms Wheel session rules on fakes: who may send and receive, the rate limit, spoofed slots and the
// telemetry batches. No game, no network.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include "comms_session_logic.h"

using namespace dcp::comms;

namespace {

int g_failures = 0;

#define CHECK(cond)                                             \
    do {                                                        \
        if (!(cond)) {                                          \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++g_failures;                                       \
        }                                                       \
    } while (0)

struct Sent {
    uint64_t account;
    std::array<uint8_t, kWireBytes> body;
};

struct Fake {
    uint64_t now = 100000;
    Room room;
    bool bridge = true, unit_running = true, report_open = true, report_accepts = true;
    std::vector<Sent> sent;
    std::vector<std::pair<int, int>> shown;
    std::vector<Received> incoming;
    std::vector<std::array<uint64_t, kMessageCount>> reports;
    ReportResult result;
    std::vector<std::string> logs;

    Fake() {
        room.slot = 0;
        room.members = 2;
        room.peers = {{2002, 1}};
    }

    Host MakeHost() {
        Host host;
        host.now_ms = [this] { return now; };
        host.room = [this] { return room; };
        host.bridge_active = [this] { return bridge; };
        host.unit_running = [this](int) { return unit_running; };
        host.send = [this](uint64_t account, const uint8_t* body, size_t size) {
            Sent s{account, {}};
            if (size == kWireBytes) std::copy(body, body + size, s.body.begin());
            sent.push_back(s);
            return true;
        };
        host.show = [this](int slot, int id) { shown.emplace_back(slot, id); };
        host.take = [this] { return std::exchange(incoming, {}); };
        host.report_open = [this] { return report_open; };
        host.report_send = [this](const std::array<uint64_t, kMessageCount>& counts) {
            if (!report_accepts) return false;
            reports.push_back(counts);
            return true;
        };
        host.report_result = [this] { return std::exchange(result, ReportResult{}); };
        host.log = [this](const std::string& line) { logs.push_back(line); };
        return host;
    }

    Received Packet(uint64_t account, uint8_t slot, uint8_t id, uint8_t seq) {
        Received r;
        r.account = account;
        const WireMessage m{slot, id, seq};
        EncodeWire(m, r.body.data());
        return r;
    }
};

void TestAvailability() {
    Fake f;
    Session s(f.MakeHost());
    CHECK(s.Available());
    f.room.members = 1;
    CHECK(!s.Available());
    f.room = Room{};
    CHECK(!s.Available());
    f = Fake();
    Session t(f.MakeHost());
    f.bridge = false;
    CHECK(!t.Available());
    f.bridge = true;
    f.unit_running = false;
    CHECK(!t.Available());
    f.unit_running = true;
    f.room.slot = 4;
    CHECK(!t.Available());
    f.room.slot = -1;
    CHECK(!t.Available());
}

void TestSend() {
    Fake f;
    f.room.members = 3;
    f.room.peers = {{2002, 1}, {3003, 2}, {4004, 3}};
    Session s(f.MakeHost());
    CHECK(!s.Send(8) && !s.Send(-1) && f.sent.empty() && f.shown.empty());
    CHECK(s.Send(5));
    CHECK(f.sent.size() == 3 && f.sent[0].account == 2002 && f.sent[1].account == 3003 && f.sent[2].account == 4004);
    const auto decoded = DecodeWire(f.sent[0].body.data(), kWireBytes);
    CHECK(decoded && decoded->slot == 0 && decoded->id == 5);
    CHECK(f.sent[0].body == f.sent[1].body && f.sent[1].body == f.sent[2].body);
    CHECK(f.shown.size() == 1 && f.shown[0] == std::make_pair(0, 5));
    CHECK(s.PendingReport() == 1);

    f.now += kLocalMinGapMs - 1;
    CHECK(!s.Send(2) && f.sent.size() == 3 && f.shown.size() == 1 && s.PendingReport() == 1);
    f.now += 1;
    CHECK(s.Send(2));
    const auto next = DecodeWire(f.sent[3].body.data(), kWireBytes);
    CHECK(next && next->id == 2 && next->seq != decoded->seq);

    f.room.members = 1;
    f.now += 10000;
    CHECK(!s.Send(1));
}

void TestReceive() {
    Fake f;
    f.room.members = 3;
    f.room.peers = {{2002, 1}, {3003, 2}};
    Session s(f.MakeHost());

    f.incoming = {f.Packet(2002, 1, 5, 7), f.Packet(2002, 1, 5, 7)};
    s.Pump();
    CHECK(f.shown.size() == 1 && f.shown[0] == std::make_pair(1, 5));

    f.incoming = {f.Packet(2002, 2, 3, 1)}; // claims another player's slot
    s.Pump();
    CHECK(f.shown.size() == 1);
    f.incoming = {f.Packet(9999, 1, 3, 1)}; // not in the room
    s.Pump();
    CHECK(f.shown.size() == 1);
    f.incoming = {f.Packet(3003, 0, 3, 1)}; // claims this player's slot
    s.Pump();
    CHECK(f.shown.size() == 1);
    Received bad = f.Packet(3003, 2, 3, 1);
    bad.body[2] = 8;
    f.incoming = {bad};
    s.Pump();
    CHECK(f.shown.size() == 1);

    f.now += 100;
    f.incoming = {f.Packet(2002, 1, 6, 8)}; // flooding: inside the remote gap
    s.Pump();
    CHECK(f.shown.size() == 1);
    f.now += kRemoteMinGapMs;
    f.incoming = {f.Packet(2002, 1, 6, 8), f.Packet(3003, 2, 0, 1)};
    s.Pump();
    CHECK(f.shown.size() == 3 && f.shown[1] == std::make_pair(1, 6) && f.shown[2] == std::make_pair(2, 0));

    f.room.members = 1;
    f.now += 10000;
    f.incoming = {f.Packet(2002, 1, 1, 9)};
    s.Pump();
    CHECK(f.shown.size() == 3);
}

uint64_t Total(const std::array<uint64_t, kMessageCount>& counts) {
    uint64_t total = 0;
    for (uint64_t n : counts) total += n;
    return total;
}

void TestFirstReportWithinFiveSeconds() {
    Fake f;
    Session s(f.MakeHost());
    s.Pump();
    CHECK(f.reports.empty());
    CHECK(s.Send(5));
    s.Pump();
    CHECK(f.reports.empty());
    f.now += kFirstReportDelayMs - 1;
    s.Pump();
    CHECK(f.reports.empty());
    f.now += 1;
    s.Pump();
    CHECK(f.reports.size() == 1 && f.reports[0][5] == 1 && Total(f.reports[0]) == 1);
    CHECK(s.PendingReport() == 1);
    f.result = {ReportState::kAccepted, "ok"};
    s.Pump();
    CHECK(s.PendingReport() == 0 && f.reports.size() == 1);
    CHECK(f.logs.size() == 2 && f.logs[0] == "Comms Wheel telemetry: report pending" &&
          f.logs[1] == "Comms Wheel telemetry: reported 1 use(s)");
}

void TestLaterUsesAreBatched() {
    Fake f;
    Session s(f.MakeHost());
    CHECK(s.Send(0));
    f.now += kFirstReportDelayMs;
    s.Pump();
    f.result = {ReportState::kAccepted, "ok"};
    s.Pump();
    CHECK(f.reports.size() == 1 && s.PendingReport() == 0);

    f.now += kLocalMinGapMs;
    CHECK(s.Send(5));
    f.now += kLocalMinGapMs;
    CHECK(s.Send(5));
    f.now += kLocalMinGapMs;
    CHECK(s.Send(6));
    s.Pump();
    CHECK(f.reports.size() == 1 && s.PendingReport() == 3);
    f.now += kReportGapMs;
    s.Pump();
    CHECK(f.reports.size() == 2 && f.reports[1][5] == 2 && f.reports[1][6] == 1 && f.reports[1][0] == 0);
    f.result = {ReportState::kAccepted, "ok"};
    s.Pump();
    CHECK(s.PendingReport() == 0);
    f.now += 10 * kReportGapMs;
    s.Pump();
    CHECK(f.reports.size() == 2);
    CHECK(s.Send(1));
    s.Pump();
    CHECK(f.reports.size() == 3);
}

void TestPendingSurvivesUntilAccepted() {
    Fake f;
    Session s(f.MakeHost());
    CHECK(s.Send(5));
    f.now += kFirstReportDelayMs;
    s.Pump();
    CHECK(f.reports.size() == 1 && s.PendingReport() == 1);
    f.now += kLocalMinGapMs;
    CHECK(s.Send(1));
    s.Pump();
    CHECK(f.reports.size() == 1 && s.PendingReport() == 2);
    f.result = {ReportState::kAccepted, "ok"};
    s.Pump();
    CHECK(s.PendingReport() == 1);
    f.now += kReportGapMs;
    s.Pump();
    CHECK(f.reports.size() == 2 && f.reports[1][1] == 1 && Total(f.reports[1]) == 1);
}

void TestFailedReportIsRetainedAndRetried() {
    Fake f;
    Session s(f.MakeHost());
    CHECK(s.Send(5));
    f.now += kLocalMinGapMs;
    CHECK(s.Send(5));
    f.now += kFirstReportDelayMs;
    s.Pump();
    CHECK(f.reports.size() == 1 && f.reports[0][5] == 2);
    f.result = {ReportState::kFailed, "bad_request"};
    s.Pump();
    CHECK(s.PendingReport() == 2 && f.reports.size() == 1);
    CHECK(f.logs.back() == "Comms Wheel telemetry: report failed: bad_request");
    f.now += kReportRetryMs - 1;
    s.Pump();
    CHECK(f.reports.size() == 1);
    f.now += 1;
    s.Pump();
    CHECK(f.reports.size() == 2 && f.reports[1][5] == 2);
    f.result = {ReportState::kAccepted, "ok"};
    s.Pump();
    CHECK(s.PendingReport() == 0 && f.logs.back() == "Comms Wheel telemetry: reported 2 use(s)");
}

void TestUnansweredReportIsRetained() {
    Fake f;
    Session s(f.MakeHost());
    CHECK(s.Send(7));
    f.now += kFirstReportDelayMs;
    s.Pump();
    CHECK(f.reports.size() == 1);
    f.now += kReportReplyMs + 1;
    s.Pump();
    CHECK(s.PendingReport() == 1);
    f.now += kReportRetryMs;
    s.Pump();
    CHECK(f.reports.size() == 2 && f.reports[1][7] == 1);
}

void TestLinkNotOpenKeepsCounts() {
    Fake f;
    Session s(f.MakeHost());
    f.report_open = false;
    CHECK(s.Send(2));
    f.now += 100000;
    s.Pump();
    CHECK(f.reports.empty() && s.PendingReport() == 1);
    f.report_open = true;
    f.report_accepts = false;
    s.Pump();
    CHECK(f.reports.empty() && s.PendingReport() == 1);
    f.report_accepts = true;
    s.Pump();
    CHECK(f.reports.size() == 1 && f.reports[0][2] == 1);
}

void TestReportFlushOnLeaving() {
    Fake f;
    Session s(f.MakeHost());
    s.Pump();
    CHECK(s.Send(4));
    f.now += 1000;
    s.Pump();
    CHECK(f.reports.empty());
    f.room.members = 1;
    s.Pump();
    CHECK(f.reports.size() == 1 && f.reports[0][4] == 1);
}

void TestHostAndGuestReport() {
    for (const int slot : {0, 1}) {
        Fake f;
        f.room.slot = slot;
        f.room.peers = {{2002, 1 - slot}};
        Session s(f.MakeHost());
        CHECK(s.Send(0));
        f.now += kFirstReportDelayMs;
        s.Pump();
        CHECK(f.reports.size() == 1 && f.reports[0][0] == 1 && Total(f.reports[0]) == 1);
    }
}

void TestTelemetryCountsOnlyLocalSends() {
    Fake sender;
    sender.room.members = 4;
    sender.room.peers = {{2002, 1}, {3003, 2}, {4004, 3}};
    Session s(sender.MakeHost());
    CHECK(s.Send(5));
    CHECK(s.PendingReport() == 1);
    sender.now += kFirstReportDelayMs;
    s.Pump();
    uint64_t total = 0;
    for (uint64_t n : sender.reports.at(0)) total += n;
    CHECK(sender.reports.size() == 1 && total == 1 && sender.reports[0][5] == 1);

    Fake receiver;
    receiver.room.members = 4;
    receiver.room.peers = {{2002, 1}, {3003, 2}, {4004, 3}};
    Session r(receiver.MakeHost());
    receiver.incoming = {receiver.Packet(2002, 1, 5, 7), receiver.Packet(2002, 1, 5, 7), receiver.Packet(2002, 1, 5, 7)};
    r.Pump();
    CHECK(receiver.shown.size() == 1 && r.PendingReport() == 0 && receiver.reports.empty());
    receiver.now += 200000;
    r.Pump();
    CHECK(receiver.reports.empty());
}

std::string Source(const char* relative) {
    std::ifstream in(std::string(DCP_SOURCE_DIR) + "/" + relative, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

void TestRuntimeDrivesTheReporter() {
    const std::string matching = Source("src/runtime_matching2.cpp");
    const size_t pump = matching.find("void pump() {");
    CHECK(pump != std::string::npos && matching.find("g_pump_hook()", pump) == std::string::npos);
    CHECK(matching.find("if (void (*hook)() = g_pump_hook) hook();", pump) != std::string::npos);
    const std::string online = Source("src/runtime_online.cpp");
    const size_t callback = online.find("np_check_callback()");
    CHECK(callback != std::string::npos && online.find("pump();", callback) < callback + 80);
    CHECK(Source("src/loader.c").find("comms_session_register();") != std::string::npos);
    CHECK(Source("src/comms_session.cpp").find("online::set_pump_hook(Pump)") != std::string::npos);
    CHECK(matching.find("request(\"comms.report\"") != std::string::npos && matching.find("Kind::kCommsReport") != std::string::npos);
}

} // namespace

int main() {
    TestAvailability();
    TestSend();
    TestReceive();
    TestTelemetryCountsOnlyLocalSends();
    TestFirstReportWithinFiveSeconds();
    TestLaterUsesAreBatched();
    TestPendingSurvivesUntilAccepted();
    TestFailedReportIsRetainedAndRetried();
    TestUnansweredReportIsRetained();
    TestLinkNotOpenKeepsCounts();
    TestReportFlushOnLeaving();
    TestHostAndGuestReport();
    TestRuntimeDrivesTheReporter();
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
