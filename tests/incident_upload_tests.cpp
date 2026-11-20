// C7D4M: the launcher's own crash dump upload against the development lobby (tools/test.ps1 starts one and passes it as
// DCP_TEST_LOBBY): a report and its dump under one Report ID, a log-only report, a dump that cannot be sent, a retry that
// resumes the report it belongs to, and the receipt that guards it. No window, no production service, a few kilobytes of
// fake dump.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "incident_report.h"
#include "net/identity.h"
#include "net/lobby_link.h"
#include "net/online_version.h"

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

struct TempFolder {
    fs::path path;
    TempFolder() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("dcp-incident-upload-" + std::to_string(tick));
        fs::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

const std::string kGameplayLog = "[run] 2026-11-05T03:51:00\nLauncher: closed, starting the game\nDCP: game build CUSA10454 1.03 USA\n"
                                 "Runtime: guest thread 'guest' created (stack=1048576, prio=700)\nCRASH: access violation\n";

ReportFacts Facts() { return {"1.4.1", "CUSA10454 1.03 USA", "1.03", "CUSA10454", "Windows 11 (build 26200)"}; }

// Three chunks and a part, with a pattern that shows a lost or reordered byte.
constexpr size_t kDumpSize = 3 * kDumpChunkBytes + 123;

void WriteDump(const fs::path& file, size_t size) {
    std::string bytes(size, '\0');
    for (size_t i = 0; i < size; ++i) bytes[i] = char((i * 31 + 7) & 0xff);
    bytes.replace(0, 4, "MDMP");
    std::ofstream(file, std::ios::binary) << bytes;
}

// A gameplay log and the dump of its crash, found the way the page finds them.
GameLog CrashedLog(const fs::path& folder, bool with_dump = true) {
    const fs::path log = folder / "dragons_crown_pro_042.log";
    std::ofstream(log, std::ios::binary) << kGameplayLog;
    if (with_dump) WriteDump(folder / "dragons_crown_pro_042.dmp", kDumpSize);
    const std::vector<GameLog> logs = FindGameLogs(folder, {});
    return logs.empty() ? GameLog{} : logs[0];
}

net::OnlineConfig ConfigFor(const std::string& url) {
    net::OnlineConfig config;
    config.lobby = url;
    return config;
}

// What the service holds for a report: the answer of "incident.dump", which names where the upload stands.
struct Probe {
    std::string code;
    net::Json answer;
};

Probe Ask(const std::string& url, const std::string& ticket, const std::string& receipt) {
    Probe probe;
    net::LobbySession session;
    std::string error;
    if (!session.connect(url, &error)) {
        probe.code = "no_connection";
        return probe;
    }
    net::Json claim = net::Json::Object();
    claim.set("ticket", ticket);
    claim.set("receipt", receipt);
    const std::optional<net::Json> answer = session.call(kIncidentDumpOperation, claim, &probe.code);
    if (answer) probe.answer = *answer;
    return probe;
}

void TestReportAndDumpAreOneReport(const std::string& url) {
    TempFolder temp;
    const GameLog log = CrashedLog(temp.path);
    CHECK(log.CanAttachCrash());
    const auto package = BuildIncidentPackage("It crashed after joining a friend", log, Facts(), {}, true);
    CHECK(package && package->crash_dump);
    if (!package) return;
    const IncidentReceipt receipt = FileIncident(ConfigFor(url), temp.path / "user", net::kOnlineGame, *package);
    CHECK(receipt.outcome == IncidentOutcome::kSent && receipt.crash == CrashUpload::kUploaded);
    CHECK(receipt.ticket.rfind("DCP-", 0) == 0 && receipt.credential.size() == 64);
    const Probe held = Ask(url, receipt.ticket, receipt.credential);
    CHECK(held.code == "ok" && held.answer["complete"].as_bool() && held.answer["bytes"].as_u64() == kDumpSize &&
          held.answer["stored"].as_u64() == kDumpSize);
    // The same report is what a player reads back; the dump was never a second report.
    TrackedReport tracked = TrackedReportOf(*package, receipt);
    const std::optional<IncidentPackage> read = FetchSentReport(ConfigFor(url), tracked);
    CHECK(read && read->Value("log_file") == "dragons_crown_pro_042.log");

    // Sending the dump again, as a retry does, finds it there and changes nothing.
    CHECK(UploadCrashDump(ConfigFor(url), receipt.ticket, receipt.credential, *package->crash_dump) == CrashUpload::kUploaded);
}

void TestLogOnlyReportsWorkAsBefore(const std::string& url) {
    TempFolder temp;
    const GameLog log = CrashedLog(temp.path);
    const auto package = BuildIncidentPackage("It crashed after joining a friend", log, Facts(), {}, false);
    CHECK(package && !package->crash_dump);
    if (!package) return;
    const IncidentReceipt receipt = FileIncident(ConfigFor(url), temp.path / "user", net::kOnlineGame, *package);
    CHECK(receipt.outcome == IncidentOutcome::kSent && receipt.crash == CrashUpload::kNotRequested && !receipt.ticket.empty());
    CHECK(Ask(url, receipt.ticket, receipt.credential).code == "bad_request");
}

void TestDumpThatCannotBeSentIsReportedNotHidden(const std::string& url) {
    TempFolder temp;
    const GameLog log = CrashedLog(temp.path, false);
    const auto package = BuildIncidentPackage("It froze", log, Facts(), {}, true);
    CHECK(package && !package->crash_dump && package->crash_dump_requested);
    if (!package) return;
    const IncidentReceipt receipt = FileIncident(ConfigFor(url), temp.path / "user", net::kOnlineGame, *package);
    CHECK(receipt.outcome == IncidentOutcome::kSent && receipt.crash == CrashUpload::kFailed);
}

void TestRetryResumesTheSameReport(const std::string& url) {
    TempFolder temp;
    const GameLog log = CrashedLog(temp.path);
    const auto package = BuildIncidentPackage("It crashed after joining a friend", log, Facts(), {}, true);
    CHECK(package && package->crash_dump);
    if (!package || !package->crash_dump) return;

    // A first try that stops after two chunks: the connection drops.
    const net::Identity identity = net::load_identity(temp.path / "user");
    CHECK(identity.ok());
    net::LobbySession first;
    std::string error, code;
    CHECK(first.open(url, net::hello_body(identity, net::online_name("Player"), net::kOnlineGame, false), nullptr, &error));
    const std::optional<net::Json> filed = first.call(kIncidentOperation, IncidentRequestBody(*package), &code);
    CHECK(filed && (*filed)["dump"].as_string() == "expected");
    if (!filed) return;
    const std::string ticket = (*filed)["ticket"].as_string(), receipt = (*filed)["receipt"].as_string();
    net::Json claim = net::Json::Object();
    claim.set("ticket", ticket);
    claim.set("receipt", receipt);
    CHECK(first.call(kIncidentDumpOperation, claim, &code).has_value());
    std::vector<uint8_t> bytes(4 + kDumpChunkBytes);
    bytes[0] = 0x44, bytes[1] = 0x49, bytes[2] = 1, bytes[3] = 1;
    std::ifstream in(package->crash_dump->path, std::ios::binary);
    for (int chunk = 0; chunk < 2; ++chunk) {
        in.read(reinterpret_cast<char*>(bytes.data() + 4), std::streamsize(kDumpChunkBytes));
        CHECK(first.send_binary(bytes.data(), bytes.size()));
    }
    // The connection drops with the upload unfinished; how much arrived first is up to the network.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    first.close();

    // The service keeps what arrived; the retry sends the rest under the same Report ID. The service limits how often one
    // address asks, so the wait is a pause, not a poll.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const Probe waiting = Ask(url, ticket, receipt);
    const uint64_t stored = waiting.answer["stored"].as_u64();
    CHECK(waiting.code == "ok" && !waiting.answer["complete"].as_bool() && stored > 0 && stored < kDumpSize && stored % kDumpChunkBytes == 0);
    CHECK(UploadCrashDump(ConfigFor(url), ticket, receipt, *package->crash_dump) == CrashUpload::kUploaded);
}

void TestReceiptGuardsTheUpload(const std::string& url) {
    TempFolder temp;
    const GameLog log = CrashedLog(temp.path);
    const auto package = BuildIncidentPackage("It crashed after joining a friend", log, Facts(), {}, true);
    if (!package || !package->crash_dump) {
        CHECK(false);
        return;
    }
    // A report that announced no dump takes none, and a wrong receipt is not an owner.
    TempFolder other;
    const GameLog plain_log = CrashedLog(other.path, false);
    const auto plain = BuildIncidentPackage("It froze", plain_log, Facts(), {}, false);
    CHECK(plain.has_value());
    if (!plain) return;
    const IncidentReceipt filed = FileIncident(ConfigFor(url), other.path / "user", net::kOnlineGame, *plain);
    CHECK(filed.outcome == IncidentOutcome::kSent);
    CHECK(UploadCrashDump(ConfigFor(url), filed.ticket, filed.credential, *package->crash_dump) == CrashUpload::kFailed);
    CHECK(UploadCrashDump(ConfigFor(url), filed.ticket, std::string(64, 'f'), *package->crash_dump) == CrashUpload::kFailed);
    // A dump whose size is not the one announced is stopped, not sent.
    TempFolder third;
    const GameLog log3 = CrashedLog(third.path);
    const auto announced = BuildIncidentPackage("It crashed after joining a friend", log3, Facts(), {}, true);
    CHECK(announced && announced->crash_dump);
    if (!announced || !announced->crash_dump) return;
    const IncidentReceipt real = FileIncident(ConfigFor(url), third.path / "user", net::kOnlineGame, *announced);
    CHECK(real.crash == CrashUpload::kUploaded);
    CrashDump grown = *announced->crash_dump;
    grown.bytes += 10;
    CHECK(UploadCrashDump(ConfigFor(url), real.ticket, real.credential, grown) == CrashUpload::kFailed);
}

void TestUnreachableServiceIsAFailure() {
    TempFolder temp;
    const GameLog log = CrashedLog(temp.path);
    const auto package = BuildIncidentPackage("It crashed", log, Facts(), {}, true);
    if (!package || !package->crash_dump) {
        CHECK(false);
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    CHECK(UploadCrashDump(ConfigFor("ws://127.0.0.1:1/"), "DCP-AAAAAA", std::string(64, 'a'), *package->crash_dump) == CrashUpload::kFailed);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(30));
}

} // namespace

int main() {
    const char* url = std::getenv("DCP_TEST_LOBBY");
    if (!url || !*url) {
        std::printf("incident-upload: set DCP_TEST_LOBBY to a running development lobby (tools/test.ps1 does)\n");
        return 1;
    }
    TestReportAndDumpAreOneReport(url);
    TestLogOnlyReportsWorkAsBefore(url);
    TestDumpThatCannotBeSentIsReportedNotHidden(url);
    TestRetryResumesTheSameReport(url);
    TestReceiptGuardsTheUpload(url);
    TestUnreachableServiceIsAFailure();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
