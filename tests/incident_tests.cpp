// Focused tests of the Bug Report page's logic: which logs it offers, what the privacy filter leaves, and that the preview
// and the request are the same report. Synthetic logs in a temporary folder; no window, no network.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "game_profile.h"
#include "incident_report.h"
#include "localization/ui_text.h"
#include "log_scrubber.h"
#include "page_activation.h"
#include "net/lobby_link.h"
#include "report_history.h"

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

const std::string kGameSession = "[run] 2026-11-05T03:51:00\nLauncher: closed, starting the game\nDCP: game build CUSA10454 1.03 USA\n";
const std::string kGameplaySession = kGameSession + "Runtime: guest thread 'guest' created (stack=1048576, prio=700)\n";
const std::string kLauncherSession = "[run] 2026-11-05T03:51:00\nLauncher: shown\nLauncher: closed\n";
const std::string kLauncherCrash = "[run] 2026-11-05T03:51:00\nLauncher: shown\nLauncher: window 1689x840\n";

struct TempFolder {
    fs::path path;
    TempFolder() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("dcp-incident-tests-" + std::to_string(tick));
        fs::create_directories(path);
    }
    ~TempFolder() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string LogName(int number) {
    char name[64];
    std::snprintf(name, sizeof(name), "dragons_crown_pro_%03d.log", number);
    return name;
}

fs::path WriteLog(const fs::path& folder, const std::string& name, const std::string& text, int minutes_ago) {
    const fs::path file = folder / name;
    std::ofstream(file, std::ios::binary) << text;
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::minutes(minutes_ago));
    return file;
}

std::vector<std::string> Names(const std::vector<GameLog>& logs) {
    std::vector<std::string> names;
    for (const GameLog& log : logs) names.push_back(log.name);
    return names;
}

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

const fs::path kSource(DCP_SOURCE_DIR);

PrivateFacts Facts() {
    PrivateFacts facts;
    facts.user_names = {"Zefa Tester", "zefa"};
    facts.machine_names = {"ZEFA-DESKTOP"};
    facts.profile_folders = {"C:\\Users\\Zefa Tester"};
    facts.install_folders = {"D:\\Jogos\\Zefa\\DCP"};
    facts.secrets = {std::string(64, 'a'), "7654321098765432"};
    return facts;
}

std::string Scrub(std::string_view text) { return ScrubText(text, Facts()); }

bool Has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

ReportFacts SampleFacts() { return {"0.9.3", "CUSA10454 1.03 USA", "1.03", "CUSA10454", "Windows 11 (build 26200)"}; }

void TestNewestFiveFirst() {
    TempFolder temp;
    // Modification time decides, not the number in the name.
    const int ages[] = {50, 10, 40, 20, 60, 30, 70};
    for (int i = 0; i < 7; ++i) WriteLog(temp.path, LogName(i + 1), kGameSession, ages[i]);
    WriteLog(temp.path, "last_run.log", kGameSession, 1);
    WriteLog(temp.path, "dragons_crown_pro_abc.log", kGameSession, 1);
    WriteLog(temp.path, "dragons_crown_pro_012.log.bak", kGameSession, 1);
    WriteLog(temp.path, "dragons_crown_pro_7.log", kGameSession, 1);
    WriteLog(temp.path, "notes.txt", kGameSession, 1);
    fs::create_directories(temp.path / "dragons_crown_pro_099.log");
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    const std::vector<std::string> expected = {LogName(2), LogName(4), LogName(6), LogName(3), LogName(1)};
    CHECK(logs.size() == kMostGameLogs);
    CHECK(Names(logs) == expected);
    CHECK(logs[0].bytes == kGameSession.size() && logs[0].written > logs[1].written);
}

void TestFewerThanFive() {
    TempFolder temp;
    CHECK(FindGameLogs(temp.path, {}).empty());
    CHECK(FindGameLogs(temp.path / "missing", {}).empty());
    WriteLog(temp.path, LogName(1), kGameSession, 30);
    WriteLog(temp.path, LogName(2), kGameSession, 20);
    const std::vector<std::string> expected = {LogName(2), LogName(1)};
    CHECK(Names(FindGameLogs(temp.path, {})) == expected);
}

void TestSessionsThatAreNotGameLogs() {
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameSession, 40);
    WriteLog(temp.path, LogName(2), kLauncherSession, 30);
    WriteLog(temp.path, LogName(3), kLauncherCrash, 20);
    WriteLog(temp.path, LogName(4), "", 15);
    const fs::path running = WriteLog(temp.path, LogName(5), kLauncherCrash, 0);
    const std::vector<std::string> expected = {LogName(5), LogName(3), LogName(1)};
    CHECK(Names(FindGameLogs(temp.path, running)) == expected);

    // Opening the launcher repeatedly must not push the game's logs off the list.
    TempFolder many;
    WriteLog(many.path, LogName(1), kGameSession, 100);
    for (int i = 2; i < 12; ++i) WriteLog(many.path, LogName(i), kLauncherSession, 100 - i);
    const std::vector<std::string> only_game = {LogName(1)};
    CHECK(Names(FindGameLogs(many.path, {})) == only_game);

    // The marks are what the launcher and the loader write.
    CHECK(Has(ReadAll(kSource / "src" / "loader.c"), "DCP: game build %s"));
    const std::string launcher = ReadAll(kSource / "launcher" / "launcher.cpp");
    CHECK(Has(launcher, "\"closed, starting the game\" : \"closed\""));
    CHECK(Has(launcher, "std::printf(\"Launcher: \");"));
    CHECK(Has(ReadAll(kSource / "src" / "runtime_thread.c"), "\"Runtime: guest thread '%s' created"));
}

std::vector<LogRole> Roles(const std::vector<GameLog>& logs) {
    std::vector<LogRole> roles;
    for (const GameLog& log : logs) roles.push_back(log.role);
    return roles;
}

void TestCurrentSessionAndLastGameplay() {
    using enum LogRole;
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameplaySession, 60);
    WriteLog(temp.path, LogName(2), kGameplaySession, 50);
    WriteLog(temp.path, LogName(3), kLauncherSession, 40);
    WriteLog(temp.path, LogName(4), kLauncherSession, 30);
    // First whatever its time says: the file is still open, so its time can lag.
    const fs::path running = WriteLog(temp.path, LogName(5), kLauncherCrash, 120);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, running);
    const std::vector<std::string> names = {LogName(5), LogName(2), LogName(1)};
    CHECK(Names(logs) == names);
    CHECK(Roles(logs) == (std::vector<LogRole>{kCurrentLauncher, kLastGameplay, kOtherGameplay}));
    CHECK(DefaultGameLog(logs) == 1);
    CHECK(logs[0].activity == LogActivity::kLauncher && logs[1].activity == LogActivity::kGameplay);
}

void TestLauncherSessionsAreNotGameplay() {
    using enum LogRole;
    // Sessions that never left the launcher earn no label, closed or crashed, however many there are.
    TempFolder closed;
    for (int i = 1; i < 8; ++i) WriteLog(closed.path, LogName(i), kLauncherSession, 100 - i);
    const fs::path running = WriteLog(closed.path, LogName(8), kLauncherCrash, 0);
    const std::vector<GameLog> alone = FindGameLogs(closed.path, running);
    CHECK(Roles(alone) == (std::vector<LogRole>{kCurrentLauncher}));
    CHECK(DefaultGameLog(alone) == 0);

    TempFolder crashed;
    WriteLog(crashed.path, LogName(1), kGameplaySession, 60);
    WriteLog(crashed.path, LogName(2), kLauncherCrash, 30);
    const fs::path open = WriteLog(crashed.path, LogName(3), kLauncherCrash, 0);
    const std::vector<GameLog> logs = FindGameLogs(crashed.path, open);
    CHECK(Roles(logs) == (std::vector<LogRole>{kCurrentLauncher, kOtherLauncher, kLastGameplay}));
    CHECK(DefaultGameLog(logs) == 2);

    // A crashed launcher is still worth reporting when no game has run.
    TempFolder only_crash;
    WriteLog(only_crash.path, LogName(1), kLauncherCrash, 30);
    const fs::path mine = WriteLog(only_crash.path, LogName(2), kLauncherCrash, 0);
    CHECK(DefaultGameLog(FindGameLogs(only_crash.path, mine)) == 1);
}

void TestDefaultLog() {
    using enum LogRole;
    // A game that failed before its own code ran did not play, but it is the session a report is about.
    TempFolder failed;
    WriteLog(failed.path, LogName(1), kGameplaySession, 60);
    WriteLog(failed.path, LogName(2), kGameSession, 30);
    const fs::path running = WriteLog(failed.path, LogName(3), kLauncherCrash, 0);
    const std::vector<GameLog> logs = FindGameLogs(failed.path, running);
    CHECK(Roles(logs) == (std::vector<LogRole>{kCurrentLauncher, kNone, kLastGameplay}));
    CHECK(DefaultGameLog(logs) == 1);

    // The running log with gameplay is the current session, and nothing else claims to be the last one played.
    TempFolder current;
    WriteLog(current.path, LogName(1), kGameplaySession, 60);
    const fs::path played = WriteLog(current.path, LogName(2), kGameplaySession, 0);
    const std::vector<GameLog> both = FindGameLogs(current.path, played);
    CHECK(Roles(both) == (std::vector<LogRole>{kCurrentGameplay, kOtherGameplay}));
    CHECK(DefaultGameLog(both) == 0);

    // Without the running log the newest game session is simply the last one played.
    TempFolder finished;
    WriteLog(finished.path, LogName(1), kGameplaySession, 60);
    WriteLog(finished.path, LogName(2), kGameplaySession, 30);
    const std::vector<GameLog> closed = FindGameLogs(finished.path, {});
    CHECK(Roles(closed) == (std::vector<LogRole>{kLastGameplay, kOtherGameplay}));
    CHECK(DefaultGameLog(closed) == 0);

    // The running log may still be empty; it is offered, and a list of nothing has no selection to break.
    TempFolder empty;
    const fs::path fresh = WriteLog(empty.path, LogName(1), "", 0);
    CHECK(Roles(FindGameLogs(empty.path, fresh)) == (std::vector<LogRole>{kCurrentLauncher}));
    CHECK(DefaultGameLog({}) == 0);

    CHECK(LogRoleText(kNone) == nullptr);
    CHECK(std::string(LogRoleText(kCurrentLauncher)) == "Current launcher session");
    CHECK(std::string(LogRoleText(kCurrentGameplay)) == "Current session \xC2\xB7 Launcher + gameplay");
    CHECK(std::string(LogRoleText(kLastGameplay)) == "Last gameplay");
}

void TestOneChosenLogIsSent() {
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameSession + "Runtime: first\n", 30);
    WriteLog(temp.path, LogName(2), kGameSession + "Runtime: second\n", 20);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    CHECK(logs.size() == 2);
    const auto package = BuildIncidentPackage("It froze", logs[1], SampleFacts(), {});
    CHECK(package.has_value());
    if (!package) return;
    CHECK(package->Value("log_file") == LogName(1));
    CHECK(Has(package->Value("log_text"), "Runtime: first") && !Has(package->Value("log_text"), "Runtime: second"));
    int log_fields = 0;
    for (const IncidentField& field : package->fields) log_fields += field.key == "log_text";
    CHECK(log_fields == 1);
}

void TestAddresses() {
    CHECK(Scrub("Connection test: lobby answered; it sees 177.18.65.14") == "Connection test: lobby answered; it sees <ipv4>");
    CHECK(Scrub("member 0x20 is at 203.0.113.7:3658 now") == "member 0x20 is at <ipv4>:3658 now");
    CHECK(Scrub("UPnP router http://192.168.0.1:5000/x") == "UPnP router http://<ipv4>:5000/x");
    CHECK(Scrub("addr:10.1.2.3, peer=172.16.0.9.") == "addr:<ipv4>, peer=<ipv4>.");
    CHECK(Scrub("bind 0.0.0.0:3658 and 127.0.0.1") == "bind 0.0.0.0:3658 and 127.0.0.1");

    CHECK(Scrub("it sees 2001:db8:85a3::8a2e:370:7334") == "it sees <ipv6>");
    CHECK(Scrub("peer [2001:db8::1]:3658") == "peer [<ipv6>]:3658");
    CHECK(Scrub("lobby has 2804:14c:65:8ba2:1111:2222:3333:1122:3658)") == "lobby has <ipv6>:3658)");
    CHECK(Scrub("link fe80::1c2d:3eff:fe4a:5b6c%eth0 up") == "link <ipv6> up");
    CHECK(Scrub("full 2001:0db8:0000:0000:0000:ff00:0042:8329.") == "full <ipv6>.");
    CHECK(Scrub("mapped ::ffff:203.0.113.7 here") == "mapped <ipv6> here");
    CHECK(Scrub("loopback ::1 and any ::") == "loopback ::1 and any ::");
}

void TestDiagnosticsStay() {
    for (const char* line : {
             "Launcher: eboot.bin SHA-256 f9bff150c4c28507096e7e143a07492063ed89bf05ac6cb995a55a0946e3508a",
             "GPU: Vulkan device NVIDIA GeForce RTX 4070 (NVIDIA 621.1.0.0, Vulkan 1.4.357)",
             "GPU: Vulkan device NVIDIA GeForce (NVIDIA 96.1.0.0, Vulkan 1.1.0.0)",
             "Runtime: guest thread 'guest' created (stack=1048576, prio=700)",
             "[run] 2026-11-03T01:42:52",
             "Window: display 1 of 1 (Generic PnP Monitor), requested 1436x808",
             "GPU: experimental AMD water workaround off (vendor 0x10de, feedback loop layout)",
             "Trophies: NPWR03852_00 loaded, 49 trophies, 48 count toward Platinum",
             "Runtime: std::vector::at 00007FF6:0012:AB34 at 12:34:56",
             "Lobby: connected to wss://dcp-lobby.saulobenigno.workers.dev",
             "Vulkan layers: path C:\\Program Files\\NVIDIA Corporation\\layer.json",
             "Vulkan layers: path C:\\ProgramData\\obs-studio-hook\\graphics-hook64.json",
             "Vulkan layers: path C:\\WINDOWS\\System32\\DriverStore\\nvoglv64.dll",
             "Matching2: join room 4096 as member 0x20 (error code 123456)",
         }) {
        CHECK(Scrub(line) == line);
    }
}

void TestLocalPaths() {
    CHECK(Scrub("Settings: C:\\Users\\Zefa Tester\\AppData\\dcp.ini") == "Settings: <home>\\AppData\\dcp.ini");
    CHECK(Scrub("Settings: c:/users/zefa tester/AppData/dcp.ini") == "Settings: <home>/AppData/dcp.ini");
    CHECK(Scrub("{\"path\":\"C:\\\\Users\\\\Zefa Tester\\\\x.ini\"}") == "{\"path\":\"<home>\\\\x.ini\"}");
    CHECK(Scrub("{\"path\":\"D:\\\\Jogos\\\\Zefa\\\\DCP\\\\x.ini\"}") == "{\"path\":\"<install>\\\\x.ini\"}");
    CHECK(Scrub("{\"path\":\"\\\\\\\\FILESERVER\\\\ana\\\\a.log\"}") == "{\"path\":\"<network-path>\\\\a.log\"}");
    CHECK(Scrub("x>\\\\FILESERVER\\ana\\a.log") == "x><network-path>\\a.log");
    CHECK(Scrub("other C:\\Users\\Someone Else\\Documents\\a.txt") == "other <home>\\Documents\\a.txt");
    CHECK(Scrub("[run] \"D:\\Jogos\\Zefa\\DCP\\dragons_crown_pro.exe\"") == "[run] \"<install>\\dragons_crown_pro.exe\"");
    CHECK(Scrub("Launcher: game files in D:\\Jogos\\Zefa\\DCP\\game\\prepared\\v1.03: valid") ==
          "Launcher: game files in <install>\\game\\prepared\\v1.03: valid");
    CHECK(Scrub("package E:\\torrent_complete\\Zefas Stuff\\game.pkg") == "package <folder>\\game.pkg");
    CHECK(Scrub("Linux /home/ana/.local/share/dcp/x.log") == "Linux <home>/.local/share/dcp/x.log");
    CHECK(Scrub("Linux /run/media/ana/Games/dcp") == "Linux <home>/Games/dcp");
    CHECK(Scrub("share \\\\FILESERVER\\ana\\logs\\a.log") == "share <network-path>\\a.log");
    CHECK(Scrub("share \\\\NAS\\Games\\Jane Roe\\x.pkg: refused") == "share <network-path>\\x.pkg: refused");

    // Folders are named after people outside the profile too, with any punctuation.
    CHECK(Scrub("package /mnt/nas/Jane Roe/x.pkg") == "package <folder>/x.pkg");
    CHECK(Scrub("package \"/srv/Jane Roe/x.pkg\"") == "package \"<folder>/x.pkg\"");
    CHECK(Scrub("package D:\\Jane Roe (Backup)\\x.pkg") == "package <folder>\\x.pkg");
    CHECK(Scrub("package D:\\Roe, Jane\\x.pkg: refused") == "package <folder>\\x.pkg: refused");
    CHECK(Scrub("loaded C:\\Windows\\a.dll, D:\\Jane Roe\\x.pkg") == "loaded C:\\Windows\\a.dll, <folder>\\x.pkg");
    CHECK(Scrub("loaded /usr/lib/libvulkan.so.1 (found)") == "loaded /usr/lib/libvulkan.so.1 (found)");
    CHECK(Scrub("guest open /app0/data/DRACRO.CPK") == "guest open /app0/data/DRACRO.CPK");
    CHECK(Scrub("Render 1920/1080 and/or 21:9") == "Render 1920/1080 and/or 21:9");

    // An encoded name that is not UTF-8 goes with its path, and the rest of the text stays valid.
    CHECK(Scrub("Settings: C:\\Users\\Jo\xE3o\\dcp.ini") == "Settings: <home>\\dcp.ini");
    CHECK(Scrub("bad byte \xE3 here") == "bad byte ? here");
}

void TestNames() {
    CHECK(Scrub("Signed in on ZEFA-DESKTOP as zefa") == "Signed in on <computer> as <user>");
    CHECK(Scrub("zefa-desktop answered") == "<computer> answered");
    CHECK(Scrub("zefaland stays, azefa stays") == "zefaland stays, azefa stays");
    PrivateFacts generic;
    generic.user_names = {"deck"};
    generic.machine_names = {"steamdeck"};
    CHECK(ScrubText("Settings: Windows, Steam Deck deck", generic) == "Settings: Windows, Steam Deck deck");
    PrivateFacts collision;
    collision.user_names = {"home"};
    CHECK(ScrubText("/home/ana/x", collision) == "<home>/x");
}

void TestSecrets() {
    const std::string secret(64, 'a');
    CHECK(Scrub("online secret " + secret + " here") == "online secret <secret> here");
    CHECK(Scrub("Online: on as Ana (account 7654321098765432), build") == "Online: on as Ana (account <secret>), build");
    CHECK(Scrub("secret=abc123") == "secret=<secret>");
    CHECK(Scrub("password: hunter2 next") == "password: <secret> next");
    CHECK(Scrub("{\"token\":\"tok-1\",\"x\":1}") == "{\"token\":\"<secret>\",\"x\":1}");
    CHECK(Scrub("url?api_key=k1&page=2") == "url?api_key=<secret>&page=2");
    CHECK(Scrub("Authorization: Bearer abc def") == "Authorization: <secret>");
    CHECK(Scrub("header Bearer abc.def") == "header Bearer <secret>");
    CHECK(Scrub("Friend code: ABCD-EF23") == "Friend code: <secret>");
    CHECK(Scrub("friend_code=ABCD-EF23") == "friend_code=<secret>");
    CHECK(Scrub("jwt eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.abcdefgh end") == "jwt <secret> end");
}

void TestOtherPlayers() {
    CHECK(Scrub("Chat: Bob: meet me at 203.0.113.7") == "Chat: <message removed>");
    CHECK(Scrub("P2P: peer 1234567890123 at 203.0.113.7:3658") == "P2P: peer <player> at <ipv4>:3658");
    CHECK(Scrub("P2P: link 2 to peer 1234567890123 connected") == "P2P: link 2 to peer <player> connected");
    CHECK(Scrub("Rudp: context 1 -> peer 987654321, link 2") == "Rudp: context 1 -> peer <player>, link 2");
    CHECK(Scrub("Online: player account 5550001112 joined") == "Online: player account <player> joined");
    CHECK(Scrub("Matching2: search for the room with code ABC234") == "Matching2: search for the room with code <room>");
    CHECK(Scrub("Matching2: created room 4096 as member 0x10 (owner 0x10, 1 member(s), code ZX9K2M)") ==
          "Matching2: created room 4096 as member 0x10 (owner 0x10, 1 member(s), code <room>)");
    CHECK(Scrub("Matching2: peer 12 left") == "Matching2: peer 12 left");
}

void TestTextIsClean() {
    CHECK(Scrub("one\r\ntwo\r\n") == "one\ntwo\n");
    CHECK(Scrub(std::string_view("a\0b\x01" "c\td\x7F", 8)) == "abc\td");
    CHECK(Scrub("caf\xC3\xA9 \xF0\x9F\x90\x89") == "caf\xC3\xA9 \xF0\x9F\x90\x89");
    CHECK(Scrub("c1 \xC2\x85 gone") == "c1  gone");
    CHECK(Scrub("").empty());
}

void TestSizeLimit() {
    TempFolder temp;
    std::string big = kGameSession;
    for (int i = 0; big.size() < 512 * 1024; ++i) big += "Runtime: line " + std::to_string(i) + " at 0x10000\n";
    big += "Error: the last line\n";
    WriteLog(temp.path, LogName(1), big, 5);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    CHECK(logs.size() == 1);
    const auto package = BuildIncidentPackage("It froze", logs[0], SampleFacts(), {});
    CHECK(package.has_value());
    if (!package) return;
    const std::string& text = package->Value("log_text");
    CHECK(text.size() <= kMostLogBytes && text.size() > kMostLogBytes - 256);
    CHECK(text.starts_with("Runtime: line ") && text.ends_with("Error: the last line\n"));
    CHECK(package->Value("log_clipped") == "yes");

    WriteLog(temp.path, LogName(2), kGameSession, 1);
    const auto small = BuildIncidentPackage("It froze", FindGameLogs(temp.path, {})[0], SampleFacts(), {});
    CHECK(small && small->Value("log_clipped") == "no" && small->Value("log_text") == kGameSession);

    CHECK(LastBytes("abc", 10) == "abc");
    CHECK(LastBytes("one\ntwo\nthree\n", 9) == "three\n");

    // The whole limit of line breaks, the worst case for JSON escaping, fits the lobby's frame.
    IncidentPackage worst = *small;
    for (IncidentField& field : worst.fields) {
        if (field.key == "log_text") field.value.assign(kMostLogBytes, '\n');
        if (field.key == "description") field.value.assign(kMostDescriptionCharacters, '"');
    }
    CHECK(IncidentRequestBody(worst).dump().size() < 160 * 1024 - 64);
}

size_t CountOf(const std::string& text, std::string_view part) {
    size_t count = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++count;
    return count;
}

void TestPreviewIsTheRequest() {
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameSession + "Lobby sees 203.0.113.7\n", 5);
    const auto logs = FindGameLogs(temp.path, {});
    const auto package = BuildIncidentPackage("  It froze at 203.0.113.9\nTwice.  ", logs[0], SampleFacts(), Facts());
    CHECK(package.has_value());
    if (!package) return;
    // Sending without a preview builds the same report the preview would have shown.
    const auto direct = BuildIncidentPackage("  It froze at 203.0.113.9\nTwice.  ", logs[0], SampleFacts(), Facts());
    CHECK(direct && IncidentRequestBody(*direct).dump() == IncidentRequestBody(*package).dump());
    const net::Json body = IncidentRequestBody(*package);
    const std::string preview = IncidentPreviewText(*package);
    CHECK(body.members().size() == package->fields.size());
    for (const IncidentField& field : package->fields) {
        CHECK(body[field.key].as_string() == field.value);
        CHECK(field.value.empty() || Has(preview, field.value));
        CHECK(Has(preview, field.label));
    }
    CHECK(package->Value("description") == "It froze at <ipv4>\nTwice.");
    CHECK(Has(package->Value("log_text"), "Lobby sees <ipv4>"));
    CHECK(!Has(preview, "203.0.113"));
    CHECK(!Has(body.dump(), "203.0.113"));
    const char* const wanted[] = {"description", "launcher_version", "game_build", "game_version", "cusa", "operating_system", "log_file", "log_clipped", "log_text"};
    CHECK(body.members().size() == std::size(wanted));
    for (const char* key : wanted) CHECK(body.has(key));

    // One way to build a report: an open preview is sent as shown, and otherwise Send builds it the same way.
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    const size_t build = page.find("BuildIncidentPackage(");
    CHECK(build != std::string::npos && page.find("BuildIncidentPackage(", build + 1) == std::string::npos);
    CHECK(Has(page, "previewed_ = BuildPackage(facts);"));
    CHECK(Has(page, "preview_text_ = IncidentPreviewText(*previewed_);"));
    CHECK(Has(page, "package = previewed_ ? previewed_ : BuildPackage(facts);"));
    CHECK(Has(page, "FileIncident(config, user_dir, net::kOnlineGame, *package)"));
    CHECK(page.find("IncidentRequestBody") == std::string::npos);
    // Only a running delivery holds Send back; a bad draft is answered when it is pressed.
    CHECK(CountOf(page, "ImGui::BeginDisabled(sending);") == 4 && !Has(page, "BeginDisabled(!previewed_"));
    CHECK(!Has(page, "problem != DraftProblem::kNone ||"));
    CHECK(CountOf(page, "if (!AcceptDraft()) return;") == 2);
}

void TestPreviewButton() {
    using namespace localization;
    CHECK(std::string(PreviewButtonText(false)) == "Open preview");
    CHECK(std::string(PreviewButtonText(true)) == "Close preview");
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    CHECK(Has(page, "PreviewButtonText(previewed_ != nullptr)"));
    CHECK(Has(page, "if (previewed_) {\n        ForgetPreview();\n        return;\n    }"));
    SetActiveLanguage(kPortugueseBrazilKey);
    CHECK(std::string(Tr(PreviewButtonText(false))) == "Abrir visualiza\xC3\xA7\xC3\xA3o");
    CHECK(std::string(Tr(PreviewButtonText(true))) == "Fechar visualiza\xC3\xA7\xC3\xA3o");
    SetActiveLanguage("english");
}

void TestPageStackPairs() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    for (const auto& [open, close] : {std::pair{"ImGui::PushID(", "ImGui::PopID()"}, std::pair{"ImGui::PushFont(", "ImGui::PopFont()"},
                                      std::pair{"ImGui::PushStyleColor(", "ImGui::PopStyleColor()"},
                                      std::pair{"ImGui::BeginDisabled(", "ImGui::EndDisabled()"}}) {
        CHECK(CountOf(page, open) == CountOf(page, close));
    }
    // A table opens inside the first ui.Row and closes in ui.EndRows, and it must find its own ID on top of the stack then, so a
    // row's control pushes its ID only after ui.Row has run.
    const size_t logs = page.find("void IncidentPage::DrawLogs(");
    const size_t loop = page.find("for (size_t i = 0; i < logs_.size(); ++i) {", logs);
    const size_t row = page.find("ui.RowLeft(", loop), control = page.find("ui.RowControl();", loop);
    const size_t push = page.find("ImGui::PushID(", loop), pop = page.find("ImGui::PopID();", loop);
    const size_t end_rows = page.find("ui.EndRows();", loop);
    CHECK(logs != std::string::npos && loop != std::string::npos);
    CHECK(row < control && control < push && push < pop && pop < end_rows);
    CHECK(page.find("return;", loop) == std::string::npos || page.find("return;", loop) > end_rows);
}

void TestDraftChecks() {
    CHECK(kFewestDescriptionCharacters == 10);
    CHECK(CheckDraft("", true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft("  \n\t ", true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft(std::string(40, ' '), true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft("123456789", true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft(" \n 123456789 \t\n ", true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft("1234567890", true) == DraftProblem::kNone);
    CHECK(CheckDraft("  1234567890  ", true) == DraftProblem::kNone);
    CHECK(CheckDraft("It froze, twice", false) == DraftProblem::kNoLog);
    CHECK(CheckDraft("123456789", false) == DraftProblem::kDescriptionTooShort);
    std::string accents;
    for (int i = 0; i < 9; ++i) accents += "\xC3\xA9";
    CHECK(CheckDraft(accents, true) == DraftProblem::kDescriptionTooShort);
    CHECK(CheckDraft(accents + "\xC3\xA9", true) == DraftProblem::kNone);
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    CHECK(kFewestDescriptionCharacters == 10);
    CHECK(Has(page, "\"Please describe the problem, minimum 10 characters\""));
    CHECK(!Has(page, "Describe the problem") && !Has(page, "Preview the report"));
    CHECK(Has(page, "\"1. Report the problem\""));
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameSession, 5);
    const GameLog log = FindGameLogs(temp.path, {})[0];
    CHECK(!BuildIncidentPackage(" \n", log, SampleFacts(), {}).has_value());
    GameLog gone = log;
    gone.path = temp.path / "removed.log";
    CHECK(!BuildIncidentPackage("It froze", gone, SampleFacts(), {}).has_value());
}

void TestDescriptionLimit() {
    CHECK(BytesOfCharacters("abc", 2) == 2 && BytesOfCharacters("abc", 5) == 3 && BytesOfCharacters("", 5) == 0);
    CHECK(BytesOfCharacters("\xC3\xA9\xC3\xA9", 1) == 2);
    CHECK(BytesOfCharacters("\xF0\x9F\x90\x89x", 1) == 4);
    CHECK(CountCharacters("\xC3\xA9\xF0\x9F\x90\x89x") == 3);
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameSession, 5);
    const GameLog log = FindGameLogs(temp.path, {})[0];
    std::string text;
    for (size_t i = 0; i < kMostDescriptionCharacters + 20; ++i) text += "\xC3\xA9";
    const auto package = BuildIncidentPackage(text, log, SampleFacts(), {});
    CHECK(package && CountCharacters(package->Value("description")) == kMostDescriptionCharacters);
}

void TestLogListText() {
    std::tm date{};
    date.tm_year = 2026 - 1900;
    date.tm_mon = 10;
    date.tm_mday = 5;
    date.tm_hour = 3;
    date.tm_min = 51;
    date.tm_isdst = -1;
    const std::time_t written = std::mktime(&date);
    CHECK(LogDateText(written, false) == "Nov 5, 2026 03:51");
    CHECK(LogDateText(written, true) == "05/11/2026 03:51");
    CHECK(LogSizeText(184 * 1024) == "184 KB");
    CHECK(LogSizeText(10) == "1 KB");
    CHECK(LogSizeText(1536 * 1024) == "1.5 MB");
}

void TestOutcomes() {
    CHECK(IncidentOutcomeOf("ok") == IncidentOutcome::kSent);
    CHECK(IncidentOutcomeOf("incident_hourly_limit") == IncidentOutcome::kRecentLimit);
    CHECK(IncidentOutcomeOf("incident_daily_limit") == IncidentOutcome::kDailyLimit);
    CHECK(IncidentOutcomeOf("limit") == IncidentOutcome::kRecentLimit);
    CHECK(std::string(IncidentLimitTitle(IncidentOutcome::kRecentLimit)) == "Too many reports were sent recently");
    CHECK(std::string(IncidentLimitTitle(IncidentOutcome::kDailyLimit)) == "Daily report limit reached");
    CHECK(IncidentLimitTitle(IncidentOutcome::kSent) == nullptr);
    CHECK(IncidentOutcomeOf("bad_request") == IncidentOutcome::kRefused);
    CHECK(IncidentOutcomeOf("too_large") == IncidentOutcome::kRefused);
    CHECK(IncidentOutcomeOf(net::kUnreachable) == IncidentOutcome::kUnreachable);
    CHECK(IncidentOutcomeOf(net::kLinkLost) == IncidentOutcome::kUnreachable);
    CHECK(IncidentOutcomeOf("timeout") == IncidentOutcome::kUnreachable);
    CHECK(IncidentOutcomeOf("internal") == IncidentOutcome::kServiceError);
    CHECK(IncidentOutcomeOf("unauthorized") == IncidentOutcome::kServiceError);
}

void TestLimitsKeepTheDraft() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    const size_t collect = page.find("void IncidentPage::CollectDelivery()");
    const size_t forget = page.find("void IncidentPage::ForgetPreview()");
    CHECK(collect != std::string::npos && collect < forget);
    const std::string collected = page.substr(collect, forget - collect);
    const size_t sent = collected.find("if (result_->outcome == IncidentOutcome::kSent) {");
    const size_t cleared = collected.find("description_[0] = ");
    CHECK(sent != std::string::npos && sent < cleared && cleared != std::string::npos);
    CHECK(CountOf(page, "description_[0] = ") == 1);
    CHECK(!Has(collected, "selected_") && !Has(collected, "FindLogs"));
    CHECK(Has(page, "IncidentLimitTitle(result_->outcome), \"Please try again later\""));
}

void TestResultComesIntoView() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    CHECK(CountOf(page, "bring_result_into_view_ = true;") == 4);
    const size_t collect = page.find("void IncidentPage::CollectDelivery()");
    const size_t send = page.find("void IncidentPage::Send(");
    const size_t draw = page.find("void IncidentPage::DrawSend(");
    CHECK(page.find("bring_result_into_view_ = true;", collect) < page.find("void IncidentPage::ForgetPreview()"));
    CHECK(page.find("bring_result_into_view_ = true;", send) < draw);
    const size_t result = page.find("DrawResult(ui);", draw);
    const size_t scroll = page.find("if (std::exchange(bring_result_into_view_, false)) ImGui::SetScrollHereY(1.0f);", draw);
    CHECK(result != std::string::npos && scroll != std::string::npos && result < scroll);
}

void TestNoQuotaOnThePage() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    for (const char* text : {"daily", "online account", "your address", "Online Play", "remaining", "per day", "per hour"}) {
        CHECK(!Has(page, text));
    }
    const std::string table = ReadAll(kSource / "launcher" / "localization" / "pt_br_ui_text.cpp");
    CHECK(!Has(table, "To keep to the daily limit") && !Has(table, "Too many reports were sent from here"));
    CHECK(!Has(table, "Muitos relat\xC3\xB3rios foram enviados daqui"));
}

const std::string kReceipt(64, 'a');

IncidentPackage SamplePackage() {
    IncidentPackage package;
    package.fields = {{"description", "Description", "It froze"}, {"launcher_version", "Launcher version", "0.9.3"},
                      {"game_build", "Game build", "CUSA10454 1.03 USA"}, {"game_version", "Game version", "1.03"},
                      {"cusa", "CUSA", "CUSA10454"}, {"operating_system", "Operating system", "Windows 11 (build 26200)"},
                      {"log_file", "Selected log", "dragons_crown_pro_186.log"}, {"log_clipped", "Log shortened to its end", "no"},
                      {"log_text", "Log after privacy filtering", "Launcher: shown\n"}};
    return package;
}

TrackedReport Tracked(const std::string& ticket, int64_t submitted_at) {
    return TrackedReportOf(SamplePackage(), {IncidentOutcome::kSent, ticket, kReceipt, submitted_at});
}

ReportStatus StatusOf(const std::string& ticket, const std::string& status, int64_t updated_at, const std::string& version = {},
                      const std::string& message = {}) {
    return {ticket, status, updated_at, version, message, {}};
}

void TestSentReportIsRemembered() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    {
        ReportHistory history(file);
        history.Load();
        CHECK(history.reports().empty());
        CHECK(history.Add(Tracked("DCP-AAAAAA", 1000)));
        CHECK(history.Add(Tracked("DCP-BBBBBB", 3000)));
        CHECK(history.Add(Tracked("DCP-CCCCCC", 2000)));
    }
    ReportHistory restarted(file);
    restarted.Load();
    CHECK(restarted.reports().size() == 3);
    if (restarted.reports().size() != 3) return;
    CHECK(restarted.reports()[0].ticket == "DCP-BBBBBB" && restarted.reports()[1].ticket == "DCP-CCCCCC");
    const TrackedReport& report = restarted.reports()[2];
    CHECK(report.ticket == "DCP-AAAAAA" && report.receipt == kReceipt && report.submitted_at == 1000);
    CHECK(report.description == "It froze" && report.log_file == "dragons_crown_pro_186.log");
    CHECK(report.launcher_version == "0.9.3" && report.game_build == "CUSA10454 1.03 USA" && report.cusa == "CUSA10454");
    CHECK(report.operating_system == "Windows 11 (build 26200)");
    CHECK(report.status == "new" && report.status_updated_at == 1000 && !report.unseen);

    const std::string saved = ReadAll(file);
    CHECK(Has(saved, kReceipt) && !Has(saved, "Launcher: shown") && !Has(saved, "account") && !Has(saved, "secret"));
    CHECK(!fs::exists(fs::path(file).concat(".new")));
}

void TestHistoryIsBounded() {
    TempFolder temp;
    ReportHistory history(temp.path / kReportHistoryFile);
    for (int i = 0; i < int(kMostTrackedReports) + 5; ++i) history.Add(Tracked("DCP-" + std::to_string(100000 + i), i));
    CHECK(history.reports().size() == kMostTrackedReports);
    CHECK(history.reports().front().submitted_at == int64_t(kMostTrackedReports) + 4 && history.reports().back().submitted_at == 5);
    const net::Json request = IncidentStatusRequest(history.reports());
    CHECK(request["reports"].size() == kMostTrackedReports);
    CHECK(Has(ReadAll(kSource / "server" / "lobby" / "src" / "incidents.js"), "statusBatch: " + std::to_string(kMostTrackedReports) + ","));
}

void TestHistorySaveFailureKeepsThePrevious() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    ReportHistory history(file);
    CHECK(history.Add(Tracked("DCP-AAAAAA", 1000)));
    const std::string before = ReadAll(file);
    fs::create_directory(fs::path(file).concat(".new"));
    CHECK(!history.Add(Tracked("DCP-BBBBBB", 2000)));
    CHECK(history.reports().size() == 2);
    CHECK(ReadAll(file) == before);

    ReportHistory unwritable(temp.path / kReportHistoryFile / "nested" / kReportHistoryFile);
    CHECK(!unwritable.Add(Tracked("DCP-CCCCCC", 3000)));
    CHECK(unwritable.reports().size() == 1);
}

void TestDamagedHistoryIsSetAside() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    for (const char* text : {"{\"reports\": [{\"ticket\"", "not json", "[]", "{}"}) {
        std::ofstream(file, std::ios::binary) << text;
        ReportHistory history(file);
        history.Load();
        CHECK(history.reports().empty());
        CHECK(!fs::exists(file));
        CHECK(ReadAll(fs::path(file).concat(".damaged")) == text);
        CHECK(history.Add(Tracked("DCP-AAAAAA", 1000)));
        fs::remove(file);
    }
    std::ofstream(file, std::ios::binary) << "{\"reports\": [{\"ticket\": \"DCP-AAAAAA\"}, {\"receipt\": \"x\"}]}";
    ReportHistory partial(file);
    partial.Load();
    CHECK(partial.reports().empty());
}

void TestStatusChangesMarkAReportUpdated() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    ReportHistory history(file);
    history.Add(Tracked("DCP-AAAAAA", 1000));

    CHECK(!history.Apply({StatusOf("DCP-AAAAAA", "new", 1000)}));
    CHECK(!history.HasUnseen());

    CHECK(history.Apply({StatusOf("DCP-AAAAAA", "investigating", 2000, "", "Looking at it")}));
    CHECK(history.HasUnseen());
    CHECK(history.reports()[0].status == "investigating" && history.reports()[0].player_message == "Looking at it");
    CHECK(history.reports()[0].status_updated_at == 2000);
    CHECK(history.MarkSeen("DCP-AAAAAA"));
    CHECK(!history.HasUnseen() && !history.MarkSeen("DCP-AAAAAA"));
    CHECK(!history.Apply({StatusOf("DCP-AAAAAA", "investigating", 2000, "", "Looking at it")}));

    CHECK(history.Apply({StatusOf("DCP-AAAAAA", "fixed", 3000, "0.9.3")}));
    CHECK(history.HasUnseen() && history.reports()[0].fixed_version == "0.9.3" && history.reports()[0].player_message.empty());
    CHECK(history.Save());

    ReportHistory restarted(file);
    restarted.Load();
    CHECK(restarted.HasUnseen());
    restarted.MarkSeen("DCP-AAAAAA");
    CHECK(restarted.Save());
    ReportHistory again(file);
    again.Load();
    CHECK(!again.HasUnseen() && again.reports()[0].status == "fixed");

    CHECK(!history.Apply({}));
    CHECK(!history.Apply({StatusOf("DCP-ZZZZZZ", "fixed", 9000)}));
    CHECK(!history.Apply({StatusOf("DCP-AAAAAA", "something_new", 9000)}));
    CHECK(history.reports()[0].status == "fixed" && history.reports()[0].status_updated_at == 3000);
}

std::string Body(const std::string& text, std::string_view signature) {
    const size_t start = text.find(signature);
    if (start == std::string::npos) return {};
    const size_t end = text.find("\n}\n", start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

void TestRemoveIsLocal() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    ReportHistory history(file);
    history.Add(Tracked("DCP-AAAAAA", 1000));
    history.Add(Tracked("DCP-BBBBBB", 2000));
    CHECK(history.Remove("DCP-AAAAAA") && !history.Remove("DCP-AAAAAA"));
    CHECK(history.Save());
    CHECK(!Has(ReadAll(file), "DCP-AAAAAA") && Has(ReadAll(file), "DCP-BBBBBB"));
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string reports = Body(page, "void MyReports::DrawReports(");
    CHECK(!reports.empty());
    CHECK(Has(reports, "if (removed == open_ticket_) Collapse();") && Has(reports, "history_.Remove(removed);") && Has(reports, "SaveHistory();"));
    CHECK(reports.find("for (size_t i = 0;") < reports.find("history_.Remove(removed);"));
    CHECK(!Has(reports, "Fetch") && !Has(reports, "Refresh"));
    CHECK(!Has(ReadAll(kSource / "launcher" / "incident_report.cpp"), "incident.delete"));
}

void TestDeveloperReplyIsKeptApartFromAStatusChange() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    ReportHistory history(file);
    history.Add(Tracked("DCP-AAAAAA", 1000));
    history.Add(Tracked("DCP-BBBBBB", 2000));
    CHECK(!history.HasUnseen() && !history.HasUnseenMessage() && history.NextUnseen().empty());

    // A status with no message is an update, never a reply.
    CHECK(history.Apply({StatusOf("DCP-AAAAAA", "investigating", 3000)}));
    CHECK(history.HasUnseen() && !history.HasUnseenMessage());
    CHECK(!history.reports()[1].message_unseen && history.reports()[1].player_message.empty());

    // A new message is a reply.
    CHECK(history.Apply({StatusOf("DCP-BBBBBB", "investigating", 3500, "", "Please send a newer log")}));
    CHECK(history.HasUnseenMessage() && history.reports()[0].message_unseen && !history.reports()[1].message_unseen);

    // The reply comes first even though the other report changed later, and the newest update leads within a kind.
    CHECK(history.Apply({StatusOf("DCP-AAAAAA", "fixed", 9000, "0.9.4")}));
    CHECK(history.NextUnseen() == "DCP-BBBBBB");
    CHECK(history.MarkSeen("DCP-BBBBBB"));
    CHECK(!history.HasUnseenMessage() && history.HasUnseen() && history.NextUnseen() == "DCP-AAAAAA");
    CHECK(!history.reports()[0].unseen && !history.reports()[0].message_unseen && history.reports()[1].unseen);

    // An unchanged message with a new status does not make the old message new again.
    CHECK(history.Apply({StatusOf("DCP-BBBBBB", "fixed", 9500, "0.9.4", "Please send a newer log")}));
    CHECK(history.HasUnseen() && !history.reports()[0].message_unseen);
    CHECK(history.MarkSeen("DCP-BBBBBB"));
    // A changed message is new again, and removing the message ends the reply.
    CHECK(history.Apply({StatusOf("DCP-BBBBBB", "fixed", 9600, "0.9.4", "Thanks, it is fixed")}));
    CHECK(history.reports()[0].message_unseen);
    CHECK(history.Apply({StatusOf("DCP-BBBBBB", "fixed", 9700, "0.9.4", "")}));
    CHECK(!history.reports()[0].message_unseen && history.reports()[0].unseen);
    history.MarkSeen("DCP-BBBBBB");

    // Two updates: the most recently updated report leads.
    ReportHistory two(temp.path / "two.json");
    two.Add(Tracked("DCP-AAAAAA", 1000));
    two.Add(Tracked("DCP-BBBBBB", 2000));
    two.Apply({StatusOf("DCP-AAAAAA", "investigating", 5000), StatusOf("DCP-BBBBBB", "investigating", 4000)});
    CHECK(two.NextUnseen() == "DCP-AAAAAA");
    two.Apply({StatusOf("DCP-BBBBBB", "fixed", 6000, "0.9.4")});
    CHECK(two.NextUnseen() == "DCP-BBBBBB");
    two.MarkSeen("DCP-BBBBBB");
    CHECK(two.HasUnseen() && two.NextUnseen() == "DCP-AAAAAA");
    two.MarkSeen("DCP-AAAAAA");
    CHECK(!two.HasUnseen() && two.NextUnseen().empty());
}

void TestReadStateSurvivesARestart() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    {
        ReportHistory history(file);
        history.Add(Tracked("DCP-AAAAAA", 1000));
        history.Add(Tracked("DCP-BBBBBB", 2000));
        history.Apply({StatusOf("DCP-AAAAAA", "investigating", 3000, "", "We are looking"), StatusOf("DCP-BBBBBB", "closed", 3100)});
        CHECK(history.Save());
    }
    ReportHistory restarted(file);
    restarted.Load();
    CHECK(restarted.reports().size() == 2);
    if (restarted.reports().size() != 2) return;
    const TrackedReport& reply = restarted.reports()[1];
    const TrackedReport& plain = restarted.reports()[0];
    CHECK(reply.ticket == "DCP-AAAAAA" && reply.unseen && reply.message_unseen && reply.player_message == "We are looking");
    CHECK(plain.ticket == "DCP-BBBBBB" && plain.unseen && !plain.message_unseen);
    CHECK(restarted.HasUnseenMessage() && restarted.NextUnseen() == "DCP-AAAAAA");
    restarted.MarkSeen("DCP-AAAAAA");
    CHECK(restarted.Save());

    ReportHistory again(file);
    again.Load();
    CHECK(!again.HasUnseenMessage() && again.HasUnseen() && again.NextUnseen() == "DCP-BBBBBB");
    CHECK(again.reports()[1].status == "investigating" && again.reports()[1].status_updated_at == 3000);
    CHECK(again.reports()[1].player_message == "We are looking");
}

void TestOlderHistoryFilesLoadSafely() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    const std::string receipt = kReceipt;
    const auto entry = [&](const std::string& ticket, const std::string& extra) {
        return "{\"ticket\":\"" + ticket + "\",\"receipt\":\"" + receipt + "\",\"submitted_at\":1000,\"description\":\"It froze\"," +
               "\"log_file\":\"dragons_crown_pro_186.log\",\"launcher_version\":\"0.9.3\",\"game_build\":\"CUSA10454 1.03 USA\"," +
               "\"cusa\":\"CUSA10454\",\"operating_system\":\"Windows 11\",\"status\":\"investigating\",\"status_updated_at\":2000," +
               "\"fixed_version\":\"\"," + extra + "}";
    };
    std::ofstream(file, std::ios::binary) << "{\"version\":1,\"reports\":[" +
        entry("DCP-AAAAAA", "\"player_message\":\"Old reply\",\"unseen\":true") + "," +
        entry("DCP-BBBBBB", "\"player_message\":\"\",\"unseen\":true") + "," +
        entry("DCP-CCCCCC", "\"player_message\":\"Read reply\",\"unseen\":false") + "," +
        entry("DCP-DDDDDD", "\"player_message\":\"\",\"unseen\":false,\"message_unseen\":true") + "]}";
    ReportHistory history(file);
    history.Load();
    CHECK(history.reports().size() == 4);
    if (history.reports().size() != 4) return;
    const auto find = [&](const char* ticket) -> const TrackedReport& {
        for (const TrackedReport& report : history.reports()) {
            if (report.ticket == ticket) return report;
        }
        return history.reports().front();
    };
    for (const char* ticket : {"DCP-AAAAAA", "DCP-BBBBBB", "DCP-CCCCCC", "DCP-DDDDDD"}) {
        CHECK(find(ticket).receipt == receipt && find(ticket).status == "investigating" && !find(ticket).unavailable);
    }
    CHECK(find("DCP-AAAAAA").unseen && find("DCP-AAAAAA").message_unseen);
    CHECK(find("DCP-BBBBBB").unseen && !find("DCP-BBBBBB").message_unseen);
    CHECK(!find("DCP-CCCCCC").unseen && !find("DCP-CCCCCC").message_unseen);
    CHECK(!find("DCP-DDDDDD").message_unseen);
    CHECK(history.Save());
    const std::string saved = ReadAll(file);
    CHECK(Has(saved, "\"message_unseen\"") && Has(saved, "\"unavailable\"") && Has(saved, receipt));
    ReportHistory again(file);
    again.Load();
    CHECK(again.reports().size() == 4);
}

void TestReportDeletedOnTheServiceIsUnavailable() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    ReportHistory history(file);
    history.Add(Tracked("DCP-AAAAAA", 1000));
    history.Add(Tracked("DCP-BBBBBB", 2000));
    history.Apply({StatusOf("DCP-AAAAAA", "fixed", 3000, "0.9.4", "Done")});
    history.MarkSeen("DCP-AAAAAA");

    // A check that answered, without DCP-AAAAAA: it is marked once, keeps everything it knew, and is not an update.
    const std::vector<std::string> asked = {"DCP-AAAAAA", "DCP-BBBBBB"};
    CHECK(history.Apply({StatusOf("DCP-BBBBBB", "new", 2000)}, asked));
    const TrackedReport& gone = history.reports()[1];
    CHECK(gone.ticket == "DCP-AAAAAA" && gone.unavailable);
    CHECK(gone.status == "fixed" && gone.fixed_version == "0.9.4" && gone.player_message == "Done" && gone.receipt == kReceipt);
    CHECK(!gone.unseen && !gone.message_unseen && !history.HasUnseen() && history.NextUnseen().empty());
    CHECK(!history.reports()[0].unavailable);

    // The same answer again changes nothing, so nothing is saved and nothing is shown as new.
    CHECK(!history.Apply({StatusOf("DCP-BBBBBB", "new", 2000)}, asked));
    CHECK(!history.HasUnseen());

    // A report that was not asked about is never marked.
    ReportHistory partial(temp.path / "partial.json");
    partial.Add(Tracked("DCP-AAAAAA", 1000));
    partial.Add(Tracked("DCP-BBBBBB", 2000));
    CHECK(!partial.Apply({}, {}));
    CHECK(!partial.Apply({}, {"DCP-ZZZZZZ"}));
    CHECK(!partial.reports()[0].unavailable && !partial.reports()[1].unavailable);

    // It survives a restart, and a report that comes back clears the mark without calling itself an update.
    CHECK(history.Save());
    ReportHistory restarted(file);
    restarted.Load();
    CHECK(restarted.reports().size() == 2 && restarted.reports()[1].unavailable && !restarted.HasUnseen());
    CHECK(restarted.Apply({StatusOf("DCP-AAAAAA", "fixed", 3000, "0.9.4", "Done")}, {"DCP-AAAAAA"}));
    CHECK(!restarted.reports()[1].unavailable && !restarted.HasUnseen());
    CHECK(restarted.Remove("DCP-AAAAAA") && restarted.Save());
    CHECK(!Has(ReadAll(file), "DCP-AAAAAA"));

    // A damaged or unreadable answer is no answer: it never reaches Apply as an empty list.
    const std::string report = ReadAll(kSource / "launcher" / "incident_report.cpp");
    CHECK(Has(Body(report, "std::optional<std::vector<ReportStatus>> FetchReportStatuses("), "!(*answer)[\"reports\"].is_array()"));
}

void TestStartupCheckNoticesAndShowWork() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string refresh = Body(page, "void MyReports::Refresh(");
    CHECK(Has(refresh, "if (!report.unavailable || asked_by_player) reports.push_back(report);"));
    CHECK(Has(refresh, "check_->asked.push_back(report.ticket);") && Has(refresh, "if (check_) return;"));
    CHECK(Has(Body(page, "void MyReports::Collect("), "history_.Apply(*check_->statuses, check_->asked)"));
    CHECK(Has(Body(page, "bool MyReports::HasUpdates("), "history_.HasUnseen() || history_.HasUnseenMessage()"));
    CHECK(Has(Body(page, "bool MyReports::HasNewReply("), "history_.HasUnseenMessage()"));

    const std::string show = Body(page, "void MyReports::ShowUpdate(");
    CHECK(Has(show, "history_.NextUnseen()") && Has(show, "Open(ticket);") && Has(show, "if (open_ticket_ == ticket)"));
    CHECK(Has(Body(page, "void MyReports::Open("), "fresh_reply_.clear();") && Has(Body(page, "void MyReports::Collapse("), "fresh_reply_.clear();"));

    const std::string incident = ReadAll(kSource / "launcher" / "incident_page.cpp");
    const std::string draw = Body(incident, "void IncidentPage::Draw(");
    CHECK(Has(draw, "if (my_reports_.HasUpdates()) {") && Has(draw, "const bool reply = my_reports_.HasNewReply();"));
    CHECK(Has(draw, "reply ? \"A report has a new reply\" : \"A report has an update\""));
    CHECK(Has(draw, "reply ? \"The developer replied to one of your reports.\" : \"Its status changed since you last looked.\""));
    CHECK(Has(draw, "my_reports_.ShowUpdate();") && !Has(draw, "BringIntoView"));
    CHECK(draw.find("my_reports_.Refresh(config);") < draw.find("my_reports_.HasUpdates()"));
    const std::string launcher = ReadAll(kSource / "launcher" / "launcher.cpp");
    CHECK(Has(launcher, "incident_page_->CheckReports(online);") && CountOf(launcher, "CheckReports(") == 1);
    CHECK(Has(ReadAll(kSource / "launcher" / "incident_page.h"), "bool HasReportUpdates() { return my_reports_.HasUpdates(); }"));
    CHECK(!Has(launcher, "OpenPopup(\"Report") && !Has(incident, "OpenPopup("));
}

void TestDeveloperMessageIsItsOwnPanel() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string expanded = Body(page, "void MyReports::DrawExpanded(");
    const size_t fresh = expanded.find("if (report.message_unseen) fresh_reply_ = report.ticket;");
    const size_t seen = expanded.find("history_.MarkSeen(report.ticket)");
    const size_t summary = expanded.find("DrawSummary("), message = expanded.find("DrawDeveloperMessage("), facts = expanded.find("DrawFacts(");
    CHECK(fresh != std::string::npos && fresh < seen && seen < summary && summary < message && message < facts);
    CHECK(Has(expanded, "if (!report.player_message.empty()) {") && Has(expanded, "fresh_reply_ == report.ticket"));
    CHECK(!Has(Body(page, "void DrawFacts("), "Message from developer"));
    const std::string panel = Body(page, "void DrawDeveloperMessage(");
    CHECK(Has(panel, "Tr(\"Message from developer\")") && Has(panel, "Tr(\"New reply\")") && Has(panel, "if (fresh) {"));
    CHECK(Has(panel, "theme.accent") && Has(panel, "theme.on_accent") && Has(panel, "ImGui::Dummy("));
    const std::string row = Body(page, "void MyReports::DrawRow(");
    CHECK(Has(row, "if (report.message_unseen) NewReplyMarker(ui);"));
    CHECK(row.find("StatusText(report)") < row.find("NewReplyMarker(ui)") && row.find("NewReplyMarker(ui)") < row.find("DateText(report.submitted_at)"));
    CHECK(Has(Body(page, "std::string StatusText("), "\"No longer available\"") && Has(Body(page, "std::string StatusText("), "\"Updated\""));
    CHECK(Has(expanded, "if (report.unavailable) {") && Has(expanded, "This report was removed from the online service."));
    CHECK(!Has(ReadAll(kSource / "launcher" / "report_history.h"), "dashboard") && !Has(page, "dashboard"));
}

void TestStatusProtocol() {
    TempFolder temp;
    ReportHistory history(temp.path / kReportHistoryFile);
    history.Add(Tracked("DCP-AAAAAA", 1000));
    const net::Json request = IncidentStatusRequest(history.reports());
    CHECK(request.size() == 1 && request["reports"].size() == 1);
    CHECK(request["reports"].items()[0]["ticket"].as_string() == "DCP-AAAAAA");
    CHECK(request["reports"].items()[0]["receipt"].as_string() == kReceipt);
    CHECK(request["reports"].items()[0].size() == 2);

    const auto answer = net::Json::parse(R"({"reports":[{"ticket":"DCP-AAAAAA","status":"fixed","status_updated_at":1793891198608,
        "fixed_version":"0.9.3","player_message":"Done"},{"ticket":"DCP-BBBBBB","status":"closed","status_updated_at":5}]})");
    CHECK(answer.has_value());
    if (!answer) return;
    const std::vector<ReportStatus> statuses = ReadReportStatuses(*answer);
    CHECK(statuses.size() == 2);
    if (statuses.size() != 2) return;
    CHECK(statuses[0].updated_at == 1793891198608 && statuses[0].fixed_version == "0.9.3" && statuses[0].player_message == "Done");
    CHECK(statuses[1].status == "closed" && statuses[1].fixed_version.empty());
    CHECK(ReadReportStatuses(net::Json::Object()).empty());

    const IncidentPackage package = SamplePackage();
    CHECK(IncidentPreviewText(SentReportOf(IncidentRequestBody(package))) == IncidentPreviewText(package));

    const std::string desk = ReadAll(kSource / "server" / "lobby" / "src" / "lobby.js");
    CHECK(Has(desk, std::string("'") + kIncidentStatusOperation + "'(att, msg)") && Has(desk, std::string("'") + kIncidentReadOperation + "'(att, msg)"));
    CHECK(Has(desk, std::string("'") + kIncidentFeedbackOperation + "'(att, msg)"));
    CHECK(Has(desk, std::string("'") + kIncidentReadOperation + "', '" + kIncidentFeedbackOperation + "'"));
}

void TestStatusTexts() {
    for (const char* status : {"new", "investigating", "awaiting_player_test", "fixed", "cannot_reproduce", "duplicate", "not_a_bug", "closed"}) {
        CHECK(ReportStatusText(status) != nullptr);
        CHECK(Has(ReadAll(kSource / "server" / "lobby" / "src" / "incidents.js"), std::string("'") + status + "'"));
    }
    CHECK(ReportStatusText("something_new") == nullptr);
    CHECK(std::string(ReportStatusText("cannot_reproduce")) == "Cannot reproduce");
    CHECK(std::string(ReportStatusText(kAwaitingPlayerTest)) == "Awaiting player test");
    TrackedReport report = Tracked("DCP-AAAAAA", 1000);
    report.status = kAwaitingPlayerTest;
    CHECK(ReportStatusLine(report) == "Awaiting player test");
    report.status = "closed";
    report.fixed_version = "0.9.3";
    CHECK(ReportStatusLine(report) == "Closed");
    report.status = "fixed";
    CHECK(ReportStatusLine(report) == "Fixed in version 0.9.3");
    report.fixed_version.clear();
    CHECK(ReportStatusLine(report) == "Fixed");
    CHECK(ReportPreview("Short") == "Short");
    CHECK(ReportPreview("two\nlines") == "two lines");
    CHECK(ReportPreview(std::string(80, 'x')) == std::string(60, 'x') + "...");
}

void TestReceiptsStayOutOfLogsAndSettings() {
    for (const char* name : {"incident_report.cpp", "my_reports.cpp", "report_history.cpp", "incident_page.cpp"}) {
        const std::string text = ReadAll(kSource / "launcher" / name);
        CHECK(!text.empty());
        for (size_t at = text.find("printf("); at != std::string::npos; at = text.find("printf(", at + 1)) {
            const std::string line = text.substr(at, text.find('\n', at) - at);
            CHECK(!Has(line, "receipt") && !Has(line, "credential"));
        }
    }
    CHECK(!Has(ReadAll(kSource / "launcher" / "settings.cpp"), "receipt"));
    CHECK(!Has(ReadAll(kSource / "launcher" / "online_settings.cpp"), "receipt"));
    CHECK(std::string_view(kReportHistoryFile) != "dcp.ini");
    const std::string online = ReadAll(kSource / "docs" / "ONLINE.md");
    CHECK(Has(online, "incident.status") && Has(online, "receipt"));
}

void TestLookupsTakeNoAccountAndNeverBlock() {
    const std::string report = ReadAll(kSource / "launcher" / "incident_report.cpp");
    const size_t fetch = report.find("FetchReportStatuses(const");
    CHECK(fetch != std::string::npos);
    const std::string lookups = report.substr(fetch);
    CHECK(CountOf(lookups, "session.connect(config.lobby, &error)") == 3);
    CHECK(!Has(lookups, "load_identity") && !Has(lookups, "hello_body") && !Has(lookups, "session.open"));
    CHECK(!Has(lookups, "printf"));

    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    CHECK(CountOf(page, "std::thread(") == 3 && CountOf(page, ".detach();") == 3);
    CHECK(CountOf(page, "FetchReportStatuses(") == 1 && CountOf(page, "FetchSentReport(") == 1 && CountOf(page, "SendTestFeedback(") == 1);
    const std::string launcher = ReadAll(kSource / "launcher" / "launcher.cpp");
    CHECK(Has(launcher, "incident_page_->CheckReports(online);"));
    CHECK(Has(launcher, "[this] { return incident_page_->HasReportUpdates(); }, nullptr};"));
    CHECK(!Has(launcher, "OpenPopup(\"Report"));
}

void TestNewStatusesCollectOnTheUiThread() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const size_t collect = page.find("void MyReports::Collect()");
    const size_t save = page.find("void MyReports::SaveHistory()");
    CHECK(collect != std::string::npos && collect < save);
    const std::string collected = page.substr(collect, save - collect);
    CHECK(Has(collected, "check_->statuses && history_.Apply(*check_->statuses, check_->asked)"));
    CHECK(Has(collected, "check_failed_ = asked_by_player_ && !check_->statuses;"));
    const std::string thread = page.substr(page.find("check->statuses = FetchReportStatuses"), 90);
    CHECK(Has(thread, "check->done = true;") && !Has(thread, "history_"));
}

void TestMyReportsPageStack() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    for (const auto& [open, close] : {std::pair{"ImGui::PushID(", "ImGui::PopID()"}, std::pair{"ImGui::PushFont(", "ImGui::PopFont()"},
                                      std::pair{"ImGui::PushStyleColor(", "ImGui::PopStyleColor()"},
                                      std::pair{"ImGui::PushStyleVar(", "ImGui::PopStyleVar()"},
                                      std::pair{"ImGui::BeginDisabled(", "ImGui::EndDisabled()"},
                                      std::pair{"ImGui::PushTextWrapPos(", "ImGui::PopTextWrapPos()"},
                                      std::pair{"ImGui::BeginTable(", "ImGui::EndTable()"}, std::pair{"ImGui::Indent(", "ImGui::Unindent("},
                                      std::pair{"ImGui::BeginPopupModal(", "ImGui::EndPopup()"}, std::pair{"layers.Split(", "layers.Merge("}}) {
        CHECK(CountOf(page, open) == CountOf(page, close));
        CHECK(CountOf(page, open) > 0);
    }
    CHECK(!Has(page, "BeginChild"));

    // A collapsed row opens its table with RowLeft before it pushes its ID, which is popped before the next row.
    const std::string row = Body(page, "void MyReports::DrawRow(");
    const size_t left = row.find("ui.RowLeft("), control = row.find("ui.RowControl();");
    const size_t push = row.find("ImGui::PushID("), pop = row.find("ImGui::PopID();");
    CHECK(!row.empty() && left < control && control < push && push < pop);
    CHECK(!Has(row, "return;") && !Has(row, "Dummy"));

    // An expanded report closes the rows before it, and its tables open and close between its own PushID and PopID.
    const std::string expanded = Body(page, "void MyReports::DrawExpanded(");
    const size_t close_rows = expanded.find("ui.EndRows();"), push_id = expanded.find("ImGui::PushID(");
    const size_t summary = expanded.find("DrawSummary("), facts = expanded.find("DrawFacts("), actions = expanded.find("DrawActions(");
    const size_t merged = expanded.find("layers.Merge(draw);"), pop_id = expanded.find("ImGui::PopID();");
    CHECK(!expanded.empty() && close_rows < push_id && push_id < summary && summary < facts && facts < actions && actions < merged &&
          merged < pop_id);
    CHECK(!Has(expanded, "return;") && CountOf(expanded, "ImGui::PushID(") == 1 && CountOf(expanded, "ImGui::PopID();") == 1);
    for (const char* table : {"void MyReports::DrawSummary(", "void DrawSummary(", "void DrawFacts("}) {
        const std::string body = Body(page, table);
        if (body.empty()) continue;
        CHECK(Has(body, "if (!ImGui::BeginTable(") && Has(body, "ImGui::EndTable();"));
        CHECK(!Has(body, "ImGui::PushID(") && !Has(body, "ImGui::PopID("));
    }
    const std::string confirm = Body(page, "bool MyReports::ConfirmRemoval(");
    CHECK(confirm.find("ImGui::EndPopup();") != std::string::npos && confirm.find("if (!open) return false;") < confirm.find("ImGui::EndPopup();"));
    CHECK(confirm.find("ImGui::PopStyleVar();") < confirm.find("if (!open) return false;"));
}

void TestRefreshLivesInTheHeader() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string draw = Body(page, "void MyReports::Draw(");
    const std::string header = Body(page, "void MyReports::DrawHeader(");
    CHECK(!draw.empty() && draw.find("DrawHeader(ui);") < draw.find("DrawReports(ui);"));
    CHECK(header.find("ImGui::TextUnformatted(title);") < header.find("ImGui::Button(Tr(check_ ? \"Checking...\" : \"Refresh\")"));
    CHECK(Has(header, "ImGui::SameLine(ImGui::GetCursorPosX() + room - width)") && Has(header, "if (room >= title_width"));
    CHECK(header.find("ImGui::Button(") < header.find("ui.Help(\"Only reports sent from this PC are listed here.\");"));
    CHECK(Has(header, "ImGui::BeginDisabled(check_ != nullptr);") && Has(header, "check_failed_"));

    CHECK(!Has(page, "ButtonRow(\"Refresh\"") && !Has(page, "Checks these reports"));
    CHECK(!Has(ReadAll(kSource / "launcher" / "localization" / "pt_br_ui_text.cpp"), "Checks these reports for a new status"));
    CHECK(CountOf(page, "Refresh(LobbyConfig(ui), true)") == 1 && CountOf(page, "void MyReports::Refresh(") == 1);
    CHECK(CountOf(page, "std::make_shared<StatusCheck>()") == 1 && CountOf(page, "FetchReportStatuses(") == 1);
    CHECK(Has(ReadAll(kSource / "launcher" / "incident_page.h"), "my_reports_.Refresh(config);"));
    CHECK(Has(ReadAll(kSource / "launcher" / "incident_page.cpp"), "my_reports_.Refresh(config);"));
    CHECK(Has(ReadAll(kSource / "launcher" / "launcher.cpp"), "incident_page_->CheckReports(online);"));
}

void TestDetailsExpandInPlace() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string header = ReadAll(kSource / "launcher" / "my_reports.h");
    const std::string reports = Body(page, "void MyReports::DrawReports(");
    const size_t expanded = reports.find("DrawExpanded(ui, reports[i], i, toggled, removed)");
    const size_t collapsed = reports.find("DrawRow(ui, reports[i], i, toggled)");
    CHECK(reports.find("for (size_t i = 0; i < reports.size(); ++i) {") < expanded && expanded < collapsed);
    CHECK(Has(reports, "if (reports[i].ticket == open_ticket_) DrawExpanded"));
    CHECK(collapsed < reports.find("if (!toggled.empty()) Open(toggled);"));
    CHECK(!Has(page, "DrawDetail") && !Has(page, "DrawRows") && CountOf(page, "DrawExpanded(ui") == 1);
    const std::string draw = Body(page, "void MyReports::Draw(");
    CHECK(draw.rfind("DrawReports(ui);") != std::string::npos && draw.find("DrawReports(ui);") > draw.find("DrawHeader(ui);"));

    // One open report: a second View details replaces it, and the same one closes it.
    CHECK(Has(header, "std::string open_ticket_;") && !Has(header, "open_tickets") && !Has(header, "std::set<std::string>"));
    const std::string open = Body(page, "void MyReports::Open(");
    CHECK(Has(open, "if (open_ticket_ == ticket) {") && Has(open, "Collapse();") && Has(open, "open_ticket_ = ticket;"));
    CHECK(open.find("Collapse();") < open.find("open_ticket_ = ticket;"));
    const std::string collapse = Body(page, "void MyReports::Collapse(");
    CHECK(Has(collapse, "open_ticket_.clear();") && Has(collapse, "ForgetSentReport();"));
    const std::string summary = Body(page, "void DrawSummary(");
    CHECK(Has(summary, "ImGui::Button(Tr(\"Close details\")") && Has(summary, "toggled = report.ticket;"));
    const std::string row = Body(page, "void MyReports::DrawRow(");
    CHECK(Has(row, "ImGui::Button(Tr(\"View details\")") && Has(row, "toggled = report.ticket;"));
    CHECK(Has(row, "ReportPreview(report.description)") && Has(row, "StatusText(report)") && Has(row, "DateText(report.submitted_at)"));
    CHECK(Has(Body(page, "std::string StatusText("), "\"Updated\""));
}

void TestExpandedReportIsOneFramedItem() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string expanded = Body(page, "void MyReports::DrawExpanded(");
    const char* order[] = {"history_.MarkSeen(report.ticket)", "layers.Split(draw, 2);", "layers.SetCurrentChannel(draw, 1);", "DrawSummary(",
                           "DrawFacts(", "DrawActions(", "DrawSentReport(ui);", "layers.SetCurrentChannel(draw, 0);", "DrawFrame(",
                           "layers.Merge(draw);"};
    size_t at = 0;
    for (const char* part : order) {
        const size_t found = expanded.find(part);
        CHECK(found != std::string::npos && found >= at);
        at = found == std::string::npos ? at : found;
    }
    CHECK(Has(expanded, "if (sent_ticket_ == report.ticket) DrawSentReport(ui);"));

    // Summary and details share one frame, with no row line between them.
    for (const char* name : {"void DrawSummary(", "void DrawFacts(", "void MyReports::DrawActions(", "void MyReports::DrawSentReport(",
                             "void MyReports::DrawExpanded("}) {
        const std::string body = Body(page, name);
        CHECK(!body.empty());
        for (const char* row : {"ui.Row(", "ui.RowLeft(", "ui.RowControl(", "ButtonRow(", "ConfirmRow(", "InfoGrid(", "Separator"}) {
            CHECK(!Has(body, row));
        }
    }
    const std::string frame = Body(page, "void DrawFrame(");
    CHECK(Has(frame, "border_panel") && Has(frame, "theme.panel") && Has(frame, "theme.frame") && !Has(frame, "AddRect("));
    CHECK(CountOf(frame, "AddRectFilled(") == 3 && Has(frame, "PanelBorderWidth(scale)"));
    CHECK(Has(Body(page, "void DrawFacts("), "ImGui::BeginTable(\"##facts\"") && Has(Body(page, "void DrawSummary("), "ImGui::BeginTable(\"##summary\""));

    const std::string facts = Body(page, "void DrawFacts(");
    for (const char* label : {"Report ID", "Submitted", "Description", "Selected log", "Game build", "Launcher version", "Status",
                              "Status updated", "Fixed in version"}) {
        CHECK(Has(facts, std::string("\"") + label + "\""));
    }
    CHECK(Has(facts, "report.status == \"fixed\" && !report.fixed_version.empty()"));
    CHECK(!Has(page, "BeginChild") && !Has(page, "SetScrollHereY(1.0f)"));
    CHECK(Has(Body(page, "void KeepInView("), "top < window->InnerRect.Min.y"));
}

void TestWhatWasSentStaysInTheReport() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    CHECK(CountOf(page, "\"##sent\"") == 1 && CountOf(page, "DrawSentReport(ui)") == 1);
    const std::string sent = Body(page, "void MyReports::DrawSentReport(");
    CHECK(Has(sent, "\"##sent\"") && Has(sent, "sent_unavailable_") && Has(sent, "ImGuiInputTextFlags_ReadOnly"));
    const std::string actions = Body(page, "void MyReports::DrawActions(");
    CHECK(Has(actions, "\"What was sent\"") && Has(actions, "\"Hide report\"") && Has(actions, "ReadSentReport(report, LobbyConfig(ui));"));
    CHECK(Has(actions, "ConfirmRemoval(ui)") && Has(actions, "removed = report.ticket;"));
    const std::string confirm = Body(page, "bool MyReports::ConfirmRemoval(");
    CHECK(Has(confirm, "Remove this report from this PC?") && Has(confirm, "ui.AccentButton(\"Remove\"") && Has(confirm, "Cancel"));
    CHECK(!Has(confirm, "Fetch") && !Has(actions, "incident.delete"));
    const std::string forget = Body(page, "void MyReports::ForgetSentReport(");
    CHECK(Has(forget, "sent_ticket_.clear();") && Has(forget, "sent_text_.clear();") && Has(forget, "sent_unavailable_ = false;"));
    const std::string collect = Body(page, "void MyReports::Collect(");
    CHECK(Has(collect, "reading_->ticket == sent_ticket_"));
}

void TestSelectedLogButtonSitsInItsFacts() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    const std::string facts = Body(page, "bool DrawLogFacts(");
    CHECK(!facts.empty());
    CHECK(CountOf(page, "\"View selected log\"") == 1 && CountOf(facts, "ImGui::Button(Tr(\"View selected log\")") == 1);
    CHECK(CountOf(page, "trg::OpenUrl(") == 1 && Has(page, "if (DrawLogFacts(ui, log)) trg::OpenUrl(Utf8(log.path));"));
    CHECK(!Has(page, "ButtonRow(\"Selected log\"") && !Has(page, "Opens the selected log") && !Has(page, "InfoGrid("));
    CHECK(!Has(facts, "BeginDisabled") && !Has(facts, "OpenUrl") && !Has(facts, "ui.Row") && !Has(facts, "ButtonRow"));

    // The button is the last cell of the grid's second row, as wide as the page's other buttons and flush with their right edge.
    const size_t last_written = facts.find("FactLabel(ui, \"Last written\");"), size = facts.find("FactLabel(ui, \"Size\");");
    const size_t place = facts.find("ImGui::SetCursorScreenPos(ImVec2(left + room - width, cell.y));");
    const size_t button = facts.find("ImGui::Button(Tr(\"View selected log\")"), end_table = facts.find("ImGui::EndTable();");
    CHECK(last_written < size && size < place && place < button && button < end_table);
    CHECK(Has(facts, "ControlColumnWidth(room, scale), left + room - cell.x)") && Has(facts, "ImVec2(width, 0)"));
    CHECK(CountOf(facts, "ImGui::SetCursorScreenPos(") == 1);
    CHECK(Has(facts, "ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoClip") && CountOf(facts, "ImGui::BeginTable(") == 1);

    // Nothing follows the grid: the next section starts right after it.
    const std::string logs = Body(page, "void IncidentPage::DrawLogs(");
    const size_t grid = logs.find("DrawLogFacts(ui, log)");
    CHECK(grid != std::string::npos);
    const std::string after = logs.substr(grid);
    for (const char* extra : {"ui.Row", "Spacer", "Dummy", "ButtonRow", "Separator", "EndRows", "Paragraph", "Help("}) CHECK(!Has(after, extra));
    CHECK(Has(page, "DrawLogs(ui, sending);\n    ui.Heading(\"3. Preview what will be sent\");"));

    // Stack: the cell padding and the table close together, and nothing returns between them.
    CHECK(CountOf(facts, "ImGui::PushStyleVar(") == 1 && CountOf(facts, "ImGui::PopStyleVar();") == 2);
    CHECK(facts.find("ImGui::PopStyleVar();") < facts.find("return false;") && facts.find("ImGui::EndTable();") < facts.rfind("ImGui::PopStyleVar();"));
    CHECK(CountOf(facts, "return ") == 2 && !Has(facts, "ImGui::PushID(") && !Has(facts, "ImGui::PopID("));
    for (const char* helper : {"void FactLabel(", "void FactValue(", "float SemiboldWidth("}) {
        const std::string body = Body(page, helper);
        CHECK(CountOf(body, "ImGui::PushFont(") == CountOf(body, "ImGui::PopFont()"));
        CHECK(CountOf(body, "ImGui::PushStyleColor(") == CountOf(body, "ImGui::PopStyleColor()"));
        CHECK(CountOf(body, "ImGui::PushTextWrapPos(") == CountOf(body, "ImGui::PopTextWrapPos()"));
    }
}

// ---- C7D4M: the crash dump of a gameplay session travels with its report ----

fs::path WriteDump(const fs::path& folder, const std::string& name, size_t size, int minutes_ago, bool signature = true) {
    const fs::path file = folder / name;
    std::string bytes(size, 'x');
    if (signature && size >= 4) bytes.replace(0, 4, "MDMP");
    std::ofstream(file, std::ios::binary) << bytes;
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::minutes(minutes_ago));
    return file;
}

const GameLog* LogNamed(const std::vector<GameLog>& logs, const std::string& name) {
    for (const GameLog& log : logs) {
        if (log.name == name) return &log;
    }
    return nullptr;
}

void TestCrashNeedsItsOwnDump() {
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameplaySession, 30);
    WriteLog(temp.path, LogName(2), kGameplaySession, 20);
    WriteLog(temp.path, LogName(3), kGameplaySession, 10);
    WriteDump(temp.path, "dragons_crown_pro_002.dmp", 5000, 20);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    CHECK(logs.size() == 3);
    const GameLog* first = LogNamed(logs, LogName(1));
    const GameLog* second = LogNamed(logs, LogName(2));
    const GameLog* third = LogNamed(logs, LogName(3));
    CHECK(first && second && third);
    if (!first || !second || !third) return;
    // A log without a dump is not a crash, however its text reads.
    CHECK(!first->Crashed() && !first->CanAttachCrash());
    CHECK(second->Crashed() && second->CanAttachCrash() && second->crash->name == "dragons_crown_pro_002.dmp");
    CHECK(second->crash->path == temp.path / "dragons_crown_pro_002.dmp" && second->crash->bytes == 5000);
    // dragons_crown_pro_003.log is not helped by dragons_crown_pro_002.dmp.
    CHECK(!third->Crashed() && !third->CanAttachCrash() && !third->crash.has_value());
    CHECK(CheckCrashDump(*third, temp.path / "dragons_crown_pro_002.dmp") == DumpProblem::kNotThisLog);
    CHECK(DumpNameOf(LogName(3)) == "dragons_crown_pro_003.dmp" && DumpNameOf("dragons_crown_pro_003.dmp").empty() && DumpNameOf("notes.log").empty());

    WriteDump(temp.path, "dragons_crown_pro_003.dmp", 5000, 10);
    const std::vector<GameLog> again = FindGameLogs(temp.path, {});
    CHECK(LogNamed(again, LogName(3)) && LogNamed(again, LogName(3))->Crashed());
    CHECK(LogNamed(again, LogName(1)) && !LogNamed(again, LogName(1))->Crashed());
}

void TestLogWordsDoNotMakeACrash() {
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameplaySession + "CRASH: access violation (0xc0000005)\n  minidump: dragons_crown_pro_001.dmp (next to this log)\n", 5);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    CHECK(logs.size() == 1 && !logs[0].Crashed() && !DefaultAttachCrashReport(logs[0]));
}

void TestDumpOfAnotherRunIsNotPaired() {
    TempFolder temp;
    WriteLog(temp.path, LogName(3), kGameplaySession, 5);
    // Left behind by an earlier session that had this number.
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", 5000, 90);
    CHECK(!FindGameLogs(temp.path, {})[0].Crashed());
    // Written at the end of the log's own session, seconds from its last line, it is paired.
    const fs::path dump = WriteDump(temp.path, "dragons_crown_pro_003.dmp", 5000, 5);
    fs::last_write_time(dump, fs::last_write_time(temp.path / LogName(3)) - std::chrono::seconds(kDumpPairingSeconds - 20));
    CHECK(FindGameLogs(temp.path, {})[0].Crashed());
    fs::last_write_time(dump, fs::last_write_time(temp.path / LogName(3)) - std::chrono::seconds(kDumpPairingSeconds + 30));
    CHECK(!FindGameLogs(temp.path, {})[0].Crashed());
    fs::last_write_time(dump, fs::last_write_time(temp.path / LogName(3)) + std::chrono::seconds(kDumpPairingSeconds + 30));
    CHECK(!FindGameLogs(temp.path, {})[0].Crashed());
}

void TestSelectedCrashLogAttachesByDefault() {
    TempFolder temp;
    WriteLog(temp.path, LogName(1), kGameplaySession, 20);
    WriteLog(temp.path, LogName(2), kGameplaySession, 10);
    WriteDump(temp.path, "dragons_crown_pro_002.dmp", 5000, 10);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    CHECK(logs.size() == 2);
    CHECK(DefaultAttachCrashReport(*LogNamed(logs, LogName(2))));
    CHECK(!DefaultAttachCrashReport(*LogNamed(logs, LogName(1))));
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    // Choosing a log, and finding the logs again, sets the box from the log; the page keeps no other default.
    CHECK(Has(page, "attach_crash_report_ = !logs_.empty() && DefaultAttachCrashReport(logs_[selected_]);"));
    CHECK(Has(page, "SelectLog(DefaultGameLog(logs_));") && Has(page, "SelectLog(i);"));
    CHECK(CountOf(page, "attach_crash_report_ =") == 1);
}

void TestUncheckedSendsOnlyTheLog() {
    TempFolder temp;
    WriteLog(temp.path, LogName(3), kGameplaySession, 10);
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", 5000, 10);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    CHECK(logs.size() == 1 && logs[0].CanAttachCrash());
    const auto package = BuildIncidentPackage("It crashed after joining", logs[0], SampleFacts(), {}, false);
    CHECK(package.has_value());
    if (!package) return;
    CHECK(!package->crash_dump.has_value() && !package->crash_dump_requested);
    const net::Json body = IncidentRequestBody(*package);
    CHECK(!body.has("dump_file") && !body.has("dump_bytes") && body.members().size() == 9);
    CHECK(!Has(IncidentPreviewText(*package), ".dmp"));
    // The default, without the argument, is the log alone: nothing is attached by accident.
    const auto plain = BuildIncidentPackage("It crashed after joining", logs[0], SampleFacts(), {});
    CHECK(plain && !plain->crash_dump.has_value() && IncidentRequestBody(*plain).dump() == body.dump());
}

void TestCheckedSendsTheLogAndItsDump() {
    TempFolder temp;
    WriteLog(temp.path, LogName(2), kGameplaySession, 20);
    WriteLog(temp.path, LogName(3), kGameplaySession + "Runtime: third\n", 10);
    WriteDump(temp.path, "dragons_crown_pro_002.dmp", 7000, 20);
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", 5000, 10);
    const std::vector<GameLog> logs = FindGameLogs(temp.path, {});
    const GameLog& third = *LogNamed(logs, LogName(3));
    const auto package = BuildIncidentPackage("It crashed after joining", third, SampleFacts(), {}, true);
    CHECK(package.has_value());
    if (!package) return;
    CHECK(Has(package->Value("log_text"), "Runtime: third") && package->Value("log_file") == LogName(3));
    CHECK(package->crash_dump_requested && package->crash_dump.has_value());
    if (!package->crash_dump) return;
    CHECK(package->crash_dump->name == "dragons_crown_pro_003.dmp" && package->crash_dump->bytes == 5000);
    CHECK(package->crash_dump->path == temp.path / "dragons_crown_pro_003.dmp");
    const net::Json body = IncidentRequestBody(*package);
    CHECK(body["dump_file"].as_string() == "dragons_crown_pro_003.dmp" && body["dump_bytes"].as_int() == 5000 && body.members().size() == 11);
    CHECK(body["log_file"].as_string() == LogName(3));
    // The dump's bytes are never part of the request or the preview; only its name and size.
    CHECK(Has(IncidentPreviewText(*package), "dragons_crown_pro_003.dmp") && !Has(body.dump(), "MDMP") && !Has(IncidentPreviewText(*package), "MDMP"));
    // A box checked on a log that has no dump attaches nothing.
    const auto without = BuildIncidentPackage("It crashed after joining", *LogNamed(logs, LogName(2)), SampleFacts(), {}, true);
    CHECK(without && without->crash_dump && without->crash_dump->name == "dragons_crown_pro_002.dmp");
    TempFolder bare;
    WriteLog(bare.path, LogName(4), kGameplaySession, 10);
    const auto none = BuildIncidentPackage("It froze", FindGameLogs(bare.path, {})[0], SampleFacts(), {}, true);
    CHECK(none && !none->crash_dump && none->crash_dump_requested);
}

void TestUnexpectedDumpNamesAreRefused() {
    TempFolder temp;
    WriteLog(temp.path, LogName(3), kGameplaySession, 10);
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", 5000, 10);
    const GameLog log = FindGameLogs(temp.path, {})[0];
    CHECK(CheckCrashDump(log, temp.path / "dragons_crown_pro_003.dmp") == DumpProblem::kNone);
    for (const char* name : {"dragons_crown_pro_003.dmp.exe", "dragons_crown_pro_003.dmp.txt", "dragons_crown_pro_003_2.dmp", "dragons_crown_pro_0030.dmp", "dragons_crown_pro_03.dmp",
                             "crash.dmp", "dragons_crown_pro_003.log", "dragons_crown_pro_003", ".dmp", "dragons_crown_pro_003.dmpx", "xdragons_crown_pro_003.dmp", "dragons_crown_pro_002.dmp"}) {
        WriteDump(temp.path, name, 5000, 10);
        CHECK(CheckCrashDump(log, temp.path / name) == DumpProblem::kNotThisLog);
    }
    // A package built from a log whose dump was swapped for another file keeps the log and drops the dump.
    GameLog swapped = log;
    swapped.crash = CrashDump{temp.path / "dragons_crown_pro_002.dmp", "dragons_crown_pro_003.dmp", 5000};
    const auto package = BuildIncidentPackage("It crashed", swapped, SampleFacts(), {}, true);
    CHECK(package && !package->crash_dump && package->crash_dump_requested);
}

void TestDumpPathTraversalIsRefused() {
    TempFolder temp;
    WriteLog(temp.path, LogName(3), kGameplaySession, 10);
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", 5000, 10);
    fs::create_directories(temp.path / "inner");
    WriteDump(temp.path / "inner", "dragons_crown_pro_003.dmp", 5000, 10);
    const GameLog log = FindGameLogs(temp.path, {})[0];
    CHECK(CheckCrashDump(log, temp.path / "inner" / "dragons_crown_pro_003.dmp") == DumpProblem::kNotThisLog);
    CHECK(CheckCrashDump(log, temp.path / ".." / "dragons_crown_pro_003.dmp") == DumpProblem::kNotThisLog);
    CHECK(CheckCrashDump(log, temp.path / ".." / temp.path.filename() / "inner" / ".." / "inner" / "dragons_crown_pro_003.dmp") == DumpProblem::kNotThisLog);
    CHECK(CheckCrashDump(log, fs::path("dragons_crown_pro_003.dmp")) == DumpProblem::kNotThisLog);
    CHECK(CheckCrashDump(log, fs::path("..\\dragons_crown_pro_003.dmp")) == DumpProblem::kNotThisLog);
    CHECK(CheckCrashDump(log, fs::path("../dragons_crown_pro_003.dmp")) == DumpProblem::kNotThisLog);
    CHECK(CheckCrashDump(log, temp.path / "dragons_crown_pro_003.dmp\\..\\dragons_crown_pro_002.dmp") == DumpProblem::kNotThisLog);
    GameLog elsewhere = log;
    elsewhere.crash = CrashDump{temp.path / "inner" / "dragons_crown_pro_003.dmp", "dragons_crown_pro_003.dmp", 5000};
    const auto package = BuildIncidentPackage("It crashed", elsewhere, SampleFacts(), {}, true);
    CHECK(package && !package->crash_dump);
    CHECK(CheckCrashDump(log, temp.path / "dragons_crown_pro_003.dmp") == DumpProblem::kNone);
}

void TestOversizedDumpIsRefusedCleanly() {
    TempFolder temp;
    WriteLog(temp.path, LogName(3), kGameplaySession, 10);
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", kMostDumpBytes, 10);
    GameLog log = FindGameLogs(temp.path, {})[0];
    CHECK(log.Crashed() && log.CanAttachCrash() && log.crash->bytes == kMostDumpBytes);
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", kMostDumpBytes + 1, 10);
    log = FindGameLogs(temp.path, {})[0];
    // The crash is still reported, the dump is not offered, and a package asked for it sends the log alone.
    CHECK(log.Crashed() && !log.CanAttachCrash() && log.crash_problem == DumpProblem::kTooLarge && !DefaultAttachCrashReport(log));
    const auto package = BuildIncidentPackage("It crashed", log, SampleFacts(), {}, true);
    CHECK(package && !package->crash_dump && package->crash_dump_requested);
    CHECK(package && !IncidentRequestBody(*package).has("dump_file"));
}

void TestEmptyAndForeignDumpFiles() {
    TempFolder temp;
    WriteLog(temp.path, LogName(3), kGameplaySession, 10);
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", 0, 10);
    CHECK(!FindGameLogs(temp.path, {})[0].Crashed());
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", 5000, 10, false);
    const GameLog log = FindGameLogs(temp.path, {})[0];
    CHECK(log.Crashed() && !log.CanAttachCrash() && log.crash_problem == DumpProblem::kNotADump);
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", kFewestDumpBytes - 1, 10);
    CHECK(FindGameLogs(temp.path, {})[0].crash_problem == DumpProblem::kNotADump);
    fs::remove(temp.path / "dragons_crown_pro_003.dmp");
    fs::create_directories(temp.path / "dragons_crown_pro_003.dmp");
    CHECK(!FindGameLogs(temp.path, {})[0].Crashed());
}

void TestCrashPageKeepsItsLabels() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    // The crash label is added to the session's own label; the checkbox exists only for a crashed log and is off when the dump
    // cannot be sent.
    CHECK(Has(page, "const char* role = LogRoleText(log.role);") && Has(page, "if (role) AccentText(ui, role);"));
    CHECK(Has(page, "ImGui::TextUnformatted(Tr(\"Crash detected\"));"));
    CHECK(Has(page, "if (!log.Crashed()) return;") && Has(page, "ImGui::BeginDisabled(sending || !attachable);"));
    CHECK(Has(page, "ImGui::Checkbox(Tr(\"Attach crash report\"), &attach_crash_report_)"));
    CHECK(Has(page, "Includes the crash dump to help diagnose the problem. A crash dump may contain process memory used for diagnostics, ") &&
          Has(page, "\"and it is not filtered like the log.\""));
    CHECK(Has(page, "attach_crash_report_ && logs_[selected_].CanAttachCrash()"));
    CHECK(std::string_view(LogRoleText(LogRole::kCurrentLauncher)) == "Current launcher session" &&
          std::string_view(LogRoleText(LogRole::kLastGameplay)) == "Last gameplay");
    // No control picks a dump: the page has no file chooser for one, only the log's own.
    CHECK(!Has(page, "FileBrowser") && !Has(page, "OpenFileDialog") && !Has(page, ".dmp"));
    // Turning the box on or off changes what goes out, so a preview is shown again; sending stays held while anything runs.
    CHECK(Has(page, "if (ImGui::Checkbox(Tr(\"Attach crash report\"), &attach_crash_report_)) ForgetPreview();"));
    CHECK(Has(page, "const bool sending = delivery_ != nullptr || dump_retry_ != nullptr;"));
    CHECK(Has(page, "The report was sent, but its crash report could not be uploaded."));
    CHECK(Has(page, "UploadCrashDump(config, ticket, receipt, dump)"));
}

void TestRetryNeverFilesAgain() {
    const std::string report = ReadAll(kSource / "launcher" / "incident_report.cpp");
    const size_t once = report.find("DumpStep SendDumpOnce(");
    const size_t file = report.find("IncidentReceipt FileIncident(");
    CHECK(once != std::string::npos && file != std::string::npos && once < file);
    const std::string upload = report.substr(once, file - once);
    CHECK(!Has(upload, "kIncidentOperation") && !Has(upload, "IncidentRequestBody") && !Has(upload, "hello_body"));
    CHECK(Has(upload, "kIncidentDumpOperation") && Has(upload, "kIncidentDumpEndOperation") && Has(upload, "ReceiptClaim(ticket, receipt)"));
    // One filing per Send: the dump is sent after the report was accepted, on its answer, and never refiles the report.
    CHECK(CountOf(report, "session.call(kIncidentOperation") == 1);
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    CHECK(CountOf(page, "FileIncident(") == 1 && CountOf(page, "UploadCrashDump(") == 1);
    CHECK(Has(report, "(*answer)[\"dump\"].as_string() != \"expected\""));
}

void TestDumpWireMatchesTheWorker() {
    const std::string desk = ReadAll(kSource / "server" / "lobby" / "src" / "incidents.js");
    CHECK(Has(desk, "dumpBytes: " + std::to_string(kMostDumpBytes / 1024 / 1024) + " * 1024 * 1024,"));
    CHECK(Has(desk, "dumpChunkBytes: " + std::to_string(kDumpChunkBytes / 1024) + " * 1024,"));
    CHECK(Has(desk, "dumpMinBytes: " + std::to_string(kFewestDumpBytes) + ","));
    CHECK(Has(desk, "DUMP_FRAME = { magic: [0x44, 0x49], version: 1, chunk: 1, headerBytes: 4 }"));
    const std::string lobby = ReadAll(kSource / "server" / "lobby" / "src" / "lobby.js");
    CHECK(Has(lobby, std::string("'") + kIncidentDumpOperation + "'(att, msg)") && Has(lobby, std::string("'") + kIncidentDumpEndOperation + "'(att, msg)"));
    CHECK(kMostDumpBytes > 2876968 * 2 && kMostDumpBytes <= 16 * 1024 * 1024);
}


void TestLinksAndDirectoriesAreNeverAttached() {
    TempFolder temp;
    WriteLog(temp.path, LogName(3), kGameplaySession, 10);
    const fs::path target = WriteDump(temp.path, "elsewhere.bin", 5000, 10);
    std::error_code ec;
    fs::create_symlink(target, temp.path / "dragons_crown_pro_003.dmp", ec);
    if (ec) {
        std::printf("note: no symbolic link could be made here; the link case is skipped\n");
        return;
    }
    const GameLog log = FindGameLogs(temp.path, {})[0];
    CHECK(CheckCrashDump(log, temp.path / "dragons_crown_pro_003.dmp") == DumpProblem::kNotAFile);
    CHECK(!log.Crashed() && !log.CanAttachCrash());
}

void TestDumpPairingWindowIsTheReferenceOne() {
    CHECK(kDumpPairingSeconds == 120 && kDumpUploadAttempts == 3);
    CHECK(kMostDumpBytes == 8u * 1024u * 1024u && kDumpChunkBytes == 64u * 1024u && kFewestDumpBytes == 32);
}

void TestPreviewDescribesTheDumpWithoutItsBytes() {
    TempFolder temp;
    WriteLog(temp.path, LogName(3), kGameplaySession, 10);
    WriteDump(temp.path, "dragons_crown_pro_003.dmp", 5000, 10);
    const GameLog log = FindGameLogs(temp.path, {})[0];
    const auto plain = BuildIncidentPackage("It crashed after joining", log, SampleFacts(), {}, false);
    const auto attached = BuildIncidentPackage("It crashed after joining", log, SampleFacts(), {}, true);
    CHECK(plain && attached && attached->crash_dump);
    if (!plain || !attached) return;
    const std::string without = IncidentPreviewText(*plain), with = IncidentPreviewText(*attached);
    CHECK(!Has(without, "Crash report") && !Has(without, ".dmp"));
    CHECK(Has(with, "Crash report: dragons_crown_pro_003.dmp (5 KB, sent as a file, not shown here)"));
    CHECK(!Has(with, "MDMP") && !Has(with, "xxxx"));
    CHECK(with.substr(0, without.size()) == without);
    localization::SetActiveLanguage(kPortugueseBrazilKey);
    const std::string portuguese = IncidentPreviewText(*attached);
    localization::SetActiveLanguage("english");
    CHECK(Has(portuguese, "Relat\xC3\xB3rio de travamento: dragons_crown_pro_003.dmp") && Has(portuguese, "enviado como arquivo, n\xC3\xA3o exibido aqui"));
}

void TestDumpIsReadFromTheFileWhenSent() {
    const std::string header = ReadAll(kSource / "launcher" / "incident_report.h");
    const std::string report = ReadAll(kSource / "launcher" / "incident_report.cpp");
    const size_t declared = header.find("struct CrashDump {");
    const std::string crash = header.substr(declared, header.find("};", declared) - declared);
    CHECK(declared != std::string::npos && Has(crash, "std::filesystem::path path;") && !Has(crash, "std::vector") && !Has(crash, "std::string data"));
    const size_t once = report.find("DumpStep SendDumpOnce(");
    const size_t file = report.find("IncidentReceipt FileIncident(");
    CHECK(once != std::string::npos && file != std::string::npos && once < file);
    const std::string upload = report.substr(once, file - once);
    CHECK(Has(upload, "std::ifstream in(dump.path, std::ios::binary);") && Has(upload, "in.seekg(std::streamoff(at));"));
    CHECK(Has(upload, "std::vector<uint8_t> frame(kHeader + kDumpChunkBytes);") && Has(upload, "in.read("));
    CHECK(Has(upload, "uint64_t at = (*begun)[\"stored\"].as_u64();"));
    const std::string build = Body(report, "std::optional<IncidentPackage> BuildIncidentPackage(");
    CHECK(Has(build, "fs::file_size(log.crash->path, size_error)") && !Has(build, "ReadPart(log.crash"));
}

void TestCrashStringsAreTranslated() {
    using namespace localization;
    SetActiveLanguage(kPortugueseBrazilKey);
    for (const char* text : {"Crash detected", "Attach crash report", "Crash report", "sent as a file, not shown here", "Uploading the crash report...",
                             "The crash report was uploaded with it.", "Retry crash report upload",
                             "The report was sent, but its crash report could not be uploaded.",
                             "The crash dump of this session cannot be sent, so only the log is."}) {
        CHECK(std::string(Tr(text)) != text);
    }
    CHECK(Has(Tr("Includes the crash dump to help diagnose the problem. A crash dump may contain process memory used for diagnostics, and it is not filtered like the log."),
              "mem\xC3\xB3ria do processo"));
    SetActiveLanguage("english");
}

void TestOrdinaryReportsAndHistoryAreUnchanged() {
    TempFolder temp;
    WriteLog(temp.path, LogName(3), kGameplaySession, 10);
    const GameLog log = FindGameLogs(temp.path, {})[0];
    CHECK(!log.Crashed() && !log.CanAttachCrash() && !DefaultAttachCrashReport(log));
    const auto package = BuildIncidentPackage("It froze on the first boss", log, SampleFacts(), {});
    CHECK(package && !package->crash_dump && !package->crash_dump_requested);
    if (!package) return;
    CHECK(IncidentRequestBody(*package).members().size() == 9);
    IncidentReceipt receipt;
    receipt.outcome = IncidentOutcome::kSent;
    receipt.ticket = "DCP-AAAAAA";
    receipt.credential = kReceipt;
    receipt.crash = CrashUpload::kFailed;
    const TrackedReport tracked = TrackedReportOf(*package, receipt);
    CHECK(tracked.ticket == "DCP-AAAAAA" && tracked.receipt == kReceipt);
}

void TestRetryUsesTheReportAlreadyFiled() {
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    const std::string retry = Body(page, "void IncidentPage::RetryDump(");
    CHECK(Has(retry, "result_->ticket") && Has(retry, "result_->credential") && Has(retry, "UploadCrashDump("));
    CHECK(!Has(retry, "FileIncident(") && !Has(retry, "BuildIncidentPackage(") && !Has(retry, "BuildPackage("));
    CHECK(Has(retry, "if (!result_ || !sent_dump_ || result_->ticket.empty() || result_->credential.empty() || dump_retry_) return;"));
    CHECK(Has(page, "if (sent_dump_ && !result_->credential.empty() && ImGui::Button(Tr(\"Retry crash report upload\"))) RetryDump(ui);"));
    const std::string collect = Body(page, "void IncidentPage::CollectDumpRetry(");
    CHECK(Has(collect, "result_->crash = dump_retry_->outcome;") && !Has(collect, "result_ ="));
}

void TestCrashDumpIsNeverLogged() {
    const std::string report = ReadAll(kSource / "launcher" / "incident_report.cpp");
    size_t lines = 0;
    for (size_t at = report.find("std::printf(\"Bug report: crash report"); at != std::string::npos;
         at = report.find("std::printf(\"Bug report: crash report", at + 1)) {
        const std::string line = report.substr(at, report.find(';', at) - at);
        CHECK(!Has(line, "receipt") && !Has(line, "credential") && Has(line, "dump.name.c_str()") && Has(line, "ticket.c_str()"));
        ++lines;
    }
    CHECK(lines == 2);
}

void TestLobbyAgrees() {
    const std::string desk = ReadAll(kSource / "server" / "lobby" / "src" / "incidents.js");
    const std::string lobby = ReadAll(kSource / "server" / "lobby" / "src" / "lobby.js");
    CHECK(Has(lobby, std::string("'") + kIncidentOperation + "'(att, msg)"));
    CHECK(Has(desk, "incident_hourly_limit:") && Has(desk, "incident_daily_limit:") && Has(lobby, "fail(refusal, 'report limit');"));
    CHECK(Has(desk, "descriptionChars: " + std::to_string(kMostDescriptionCharacters) + ","));
    CHECK(Has(desk, "logBytes: " + std::to_string(kMostLogBytes / 1024) + " * 1024,"));
    CHECK(Has(desk, "dragons_crown_pro_[0-9]{3,10}"));
    CHECK(Has(desk, "frameChars: 160 * 1024,"));
    // A report is stored as received or refused: the Worker has no second filter that could change what the player saw.
    CHECK(!Has(desk, ".replace(") && Has(desk, "value !== value.trim()"));
}

void TestProductionLobbyOnly() {
    // The page reaches the lobby the way the Online page does: the saved settings, the DCP_* overrides, net::kDefaultLobbyUrl.
    for (const char* file : {"incident_page.cpp", "incident_page.h", "incident_report.cpp", "incident_report.h"}) {
        const std::string text = ReadAll(kSource / "launcher" / file);
        CHECK(!text.empty() && !Has(text, "workers.dev") && !Has(text, "wss://") && !Has(text, "ws://") && !Has(text, "localhost") &&
              !Has(text, "127.0.0.1"));
    }
    const std::string page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    CHECK(Has(page, "net::OnlineConfig config = OnlineConfigOf(ui.settings());") && Has(page, "net::apply_environment(config);"));
    const std::string settings = ReadAll(kSource / "src" / "net" / "online_config.h");
    CHECK(Has(settings, "kDefaultLobbyUrl[] = \"wss://dcp-lobby.saulobenigno.workers.dev\";"));
    CHECK(Has(settings, "std::string lobby = kDefaultLobbyUrl;"));
}

ReportStatus TestRequestOf(const std::string& ticket, int64_t updated_at, const std::string& feedback = {}) {
    return {ticket, kAwaitingPlayerTest, updated_at, {}, "Please test the latest build.", feedback};
}

const TrackedReport& OnlyReport(const ReportHistory& history) { return history.reports().front(); }

void TestPlayerTestIsOpenUntilAnswered() {
    TempFolder temp;
    ReportHistory history(temp.path / kReportHistoryFile);
    history.Add(Tracked("DCP-AAAAAA", 1000));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kNone);
    CHECK(history.Apply({StatusOf("DCP-AAAAAA", "investigating", 1500)}));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kNone);

    CHECK(history.Apply({TestRequestOf("DCP-AAAAAA", 2000)}));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kAwaitingAnswer);
    CHECK(OnlyReport(history).unseen && OnlyReport(history).message_unseen);
    CHECK(OnlyReport(history).status == kAwaitingPlayerTest && OnlyReport(history).test_feedback.empty());

    CHECK(!history.RecordTestFeedback("DCP-AAAAAA", 1999, kTestFixed));
    CHECK(!history.RecordTestFeedback("DCP-AAAAAA", 2001, kTestFixed));
    CHECK(!history.RecordTestFeedback("DCP-ZZZZZZ", 2000, kTestFixed));
    CHECK(!history.RecordTestFeedback("DCP-AAAAAA", 2000, "pending"));
    CHECK(!history.RecordTestFeedback("DCP-AAAAAA", 2000, ""));
    CHECK(!history.RecordTestFeedback("DCP-AAAAAA", 2000, "Fixed"));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kAwaitingAnswer);

    CHECK(history.RecordTestFeedback("DCP-AAAAAA", 2000, kTestFixed));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kConfirmedFixed);
    CHECK(OnlyReport(history).test_feedback_revision == 2000);

    CHECK(!history.Apply({TestRequestOf("DCP-AAAAAA", 2000)}));
    CHECK(!history.Apply({TestRequestOf("DCP-AAAAAA", 2000, kTestFixed)}));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kConfirmedFixed);
}

void TestANewRequestCanBeAnsweredAgain() {
    TempFolder temp;
    ReportHistory history(temp.path / kReportHistoryFile);
    history.Add(Tracked("DCP-AAAAAA", 1000));
    history.Apply({TestRequestOf("DCP-AAAAAA", 2000)});
    CHECK(history.RecordTestFeedback("DCP-AAAAAA", 2000, kTestNotFixed));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kStillHappening);

    CHECK(history.Apply({TestRequestOf("DCP-AAAAAA", 3000)}));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kAwaitingAnswer);
    CHECK(OnlyReport(history).test_feedback_revision == 2000);
    CHECK(!history.RecordTestFeedback("DCP-AAAAAA", 2000, kTestFixed));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kAwaitingAnswer);
    CHECK(history.RecordTestFeedback("DCP-AAAAAA", 3000, kTestFixed));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kConfirmedFixed);

    CHECK(history.Apply({StatusOf("DCP-AAAAAA", "fixed", 4000, "1.4.1")}));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kNone);
    CHECK(!history.RecordTestFeedback("DCP-AAAAAA", 3000, kTestFixed));
    CHECK(history.Apply({TestRequestOf("DCP-AAAAAA", 5000)}));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kAwaitingAnswer);
}

void TestTheServiceAnswerIsKept() {
    TempFolder temp;
    ReportHistory history(temp.path / kReportHistoryFile);
    history.Add(Tracked("DCP-AAAAAA", 1000));
    CHECK(history.Apply({TestRequestOf("DCP-AAAAAA", 2000, kTestNotFixed)}));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kStillHappening);
    CHECK(!history.Apply({TestRequestOf("DCP-AAAAAA", 2000, kTestNotFixed)}));

    CHECK(history.Apply({TestRequestOf("DCP-AAAAAA", 3000)}));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kAwaitingAnswer);
    CHECK(history.Apply({TestRequestOf("DCP-AAAAAA", 3000, kTestFixed)}));
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kConfirmedFixed);
    CHECK(history.Apply({TestRequestOf("DCP-AAAAAA", 3000, "something_else")}) == false);
    CHECK(PlayerTestOf(OnlyReport(history)) == PlayerTest::kConfirmedFixed);
}

void TestThePlayerAnswerSurvivesARestart() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    ReportHistory history(file);
    history.Add(Tracked("DCP-AAAAAA", 1000));
    history.Apply({TestRequestOf("DCP-AAAAAA", 2000)});
    history.RecordTestFeedback("DCP-AAAAAA", 2000, kTestNotFixed);
    CHECK(history.Save());
    CHECK(Has(ReadAll(file), "\"test_feedback\":\"not_fixed\"") && Has(ReadAll(file), "\"test_feedback_revision\":2000"));

    ReportHistory restarted(file);
    restarted.Load();
    CHECK(restarted.reports().size() == 1 && PlayerTestOf(OnlyReport(restarted)) == PlayerTest::kStillHappening);
    CHECK(restarted.Apply({TestRequestOf("DCP-AAAAAA", 3000)}));
    CHECK(PlayerTestOf(OnlyReport(restarted)) == PlayerTest::kAwaitingAnswer);
    CHECK(restarted.Save());
    ReportHistory again(file);
    again.Load();
    CHECK(PlayerTestOf(OnlyReport(again)) == PlayerTest::kAwaitingAnswer && OnlyReport(again).test_feedback_revision == 2000);
}

void TestHistoryFilesWithoutAnswersStillLoad() {
    TempFolder temp;
    const fs::path file = temp.path / kReportHistoryFile;
    const auto entry = [](const std::string& ticket, const std::string& status, int64_t updated, const std::string& extra) {
        return "{\"ticket\":\"" + ticket + "\",\"receipt\":\"" + kReceipt + "\",\"submitted_at\":1000,\"description\":\"It froze\"," +
               "\"log_file\":\"dragons_crown_pro_186.log\",\"launcher_version\":\"0.9.3\",\"game_build\":\"CUSA10454 1.03 USA\"," +
               "\"cusa\":\"CUSA10454\",\"operating_system\":\"Windows 11\",\"status\":\"" + status + "\",\"status_updated_at\":" +
               std::to_string(updated) + ",\"fixed_version\":\"\",\"player_message\":\"Please test\",\"unseen\":false" + extra + "}";
    };
    std::ofstream(file, std::ios::binary) << "{\"version\":1,\"reports\":[" +
        entry("DCP-AAAAAA", "awaiting_player_test", 2000, "") + "," +
        entry("DCP-BBBBBB", "awaiting_player_test", 2000, ",\"test_feedback\":\"maybe\",\"test_feedback_revision\":2000") + "," +
        entry("DCP-CCCCCC", "awaiting_player_test", 2000, ",\"test_feedback\":\"fixed\",\"test_feedback_revision\":1500") + "," +
        entry("DCP-DDDDDD", "awaiting_player_test", 2000, ",\"test_feedback\":\"not_fixed\",\"test_feedback_revision\":2000") + "," +
        entry("DCP-EEEEEE", "fixed", 2000, "") + "]}";
    ReportHistory history(file);
    history.Load();
    CHECK(history.reports().size() == 5);
    if (history.reports().size() != 5) return;
    const auto state = [&](const char* ticket) {
        for (const TrackedReport& report : history.reports()) {
            if (report.ticket == ticket) return PlayerTestOf(report);
        }
        return PlayerTest::kNone;
    };
    CHECK(state("DCP-AAAAAA") == PlayerTest::kAwaitingAnswer);
    CHECK(state("DCP-BBBBBB") == PlayerTest::kAwaitingAnswer);
    CHECK(state("DCP-CCCCCC") == PlayerTest::kAwaitingAnswer);
    CHECK(state("DCP-DDDDDD") == PlayerTest::kStillHappening);
    CHECK(state("DCP-EEEEEE") == PlayerTest::kNone);
    CHECK(history.Save());
    ReportHistory again(file);
    again.Load();
    CHECK(again.reports().size() == 5);
}

void TestFeedbackProtocol() {
    TrackedReport report = Tracked("DCP-AAAAAA", 1000);
    report.status = kAwaitingPlayerTest;
    report.status_updated_at = 2000;
    const net::Json body = IncidentFeedbackRequest(report, kTestNotFixed, SampleFacts());
    CHECK(body.size() == 6);
    CHECK(body["ticket"].as_string() == "DCP-AAAAAA" && body["receipt"].as_string() == kReceipt);
    CHECK(body["revision"].as_int() == 2000 && body["result"].as_string() == "not_fixed");
    CHECK(body["launcher_version"].as_string() == "0.9.3" && body["game_build"].as_string() == "CUSA10454 1.03 USA");
    CHECK(std::string(kIncidentFeedbackOperation) == "incident.feedback");

    const auto answer = net::Json::parse(R"({"reports":[
        {"ticket":"DCP-AAAAAA","status":"awaiting_player_test","status_updated_at":2000,"test_feedback":"not_fixed","test_feedback_at":5},
        {"ticket":"DCP-BBBBBB","status":"awaiting_player_test","status_updated_at":2000,"test_feedback":"pending"},
        {"ticket":"DCP-CCCCCC","status":"awaiting_player_test","status_updated_at":2000,"test_feedback":"bogus"},
        {"ticket":"DCP-DDDDDD","status":"awaiting_player_test","status_updated_at":2000},
        {"ticket":"DCP-EEEEEE","status":"fixed","status_updated_at":2000,"fixed_version":"1.4.1"}]})");
    CHECK(answer.has_value());
    if (!answer) return;
    const std::vector<ReportStatus> statuses = ReadReportStatuses(*answer);
    CHECK(statuses.size() == 5);
    if (statuses.size() != 5) return;
    CHECK(statuses[0].test_feedback == kTestNotFixed);
    for (size_t i = 1; i < statuses.size(); ++i) CHECK(statuses[i].test_feedback.empty());
    CHECK(statuses[3].status == kAwaitingPlayerTest && statuses[3].player_message.empty());

    CHECK(FeedbackOutcomeOf("ok") == FeedbackOutcome::kSaved);
    for (const char* code : {"stale_request", "already_answered", "not_found"}) CHECK(FeedbackOutcomeOf(code) == FeedbackOutcome::kStale);
    for (const std::string& code : {std::string(net::kLinkLost), std::string(net::kUnreachable), std::string("timeout")}) {
        CHECK(FeedbackOutcomeOf(code) == FeedbackOutcome::kUnreachable);
    }
    for (const char* code : {"bad_request", "limit", "internal", "too_large", ""}) CHECK(FeedbackOutcomeOf(code) == FeedbackOutcome::kFailed);

    const std::string incidents = ReadAll(kSource / "server" / "lobby" / "src" / "incidents.js");
    for (const char* code : {"stale_request", "already_answered", "'not_found'"}) CHECK(Has(incidents, code));
    CHECK(Has(incidents, "const TEST_RESULTS") || Has(incidents, "TEST_RESULTS = ['fixed', 'not_fixed']"));
    CHECK(std::string(kTestFixed) == "fixed" && std::string(kTestNotFixed) == "not_fixed");
}

void TestAPageIsEnteredOnceNotOnEveryFrame() {
    PageActivation page;
    int entries = 0;
    for (int frame = 40; frame < 160; ++frame) entries += page.Entered(frame);
    CHECK(entries == 1);

    CHECK(!page.Entered(160));
    CHECK(page.Entered(300));
    for (int frame = 301; frame < 400; ++frame) CHECK(!page.Entered(frame));
    CHECK(page.Entered(500) && page.Entered(502) && !page.Entered(503));

    PageActivation fresh;
    CHECK(fresh.Entered(0) && !fresh.Entered(1));
}

void TestBugReportRefreshesOnStartupAndOnEveryEntry() {
    const std::string incident = ReadAll(kSource / "launcher" / "incident_page.cpp");
    const std::string draw = Body(incident, "void IncidentPage::Draw(");
    const size_t entered = draw.find("if (activation_.Entered(ImGui::GetFrameCount())) {");
    const size_t refresh = draw.find("my_reports_.Refresh(config);");
    const size_t loading = draw.find("if (Loading()) {");
    CHECK(entered != std::string::npos && entered < refresh && refresh < loading);
    CHECK(CountOf(incident, "my_reports_.Refresh(") == 1 && CountOf(incident, "activation_.Entered(") == 1);
    CHECK(!Has(incident, "RefreshIfStale") && !Has(ReadAll(kSource / "launcher" / "my_reports.cpp"), "RefreshIfStale"));
    CHECK(!Has(ReadAll(kSource / "launcher" / "my_reports.h"), "RefreshIfStale"));

    const std::string reports = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string refresh_body = Body(reports, "void MyReports::Refresh(");
    CHECK(refresh_body.find("Collect();") < refresh_body.find("if (check_) return;") &&
          refresh_body.find("if (check_) return;") < refresh_body.find("std::make_shared<StatusCheck>()"));
    CHECK(CountOf(reports, "std::make_shared<StatusCheck>()") == 1 && Has(refresh_body, ".detach();") && !Has(reports, ".join()"));
    CHECK(Has(Body(reports, "void MyReports::DrawHeader("), "Refresh(LobbyConfig(ui), true)"));

    const std::string launcher = ReadAll(kSource / "launcher" / "launcher.cpp");
    const size_t made = launcher.find("incident_page_ = std::make_unique<IncidentPage>(IncidentPlaces());");
    const size_t startup = launcher.find("incident_page_->CheckReports(online);");
    const size_t first_frame = launcher.find("outcome = Frame();");
    CHECK(made != std::string::npos && made < startup && startup < first_frame);
    CHECK(CountOf(launcher, "CheckReports(") == 1);
    CHECK(Has(ReadAll(kSource / "launcher" / "incident_page.h"),
              "void CheckReports(const net::OnlineConfig& config) { my_reports_.Refresh(config); }"));
}

void TestBugReportShowsLoadingWhileItsLogsAreListed() {
    const std::string incident = ReadAll(kSource / "launcher" / "incident_page.cpp");
    const std::string draw = Body(incident, "void IncidentPage::Draw(");
    const size_t collect = draw.find("CollectLogScan();"), loading = draw.find("if (Loading()) {");
    const size_t start = draw.find("StartLogScan();"), view = draw.find("DrawLoadingView(ui, \"Loading Bug Report...\");");
    const size_t leave = draw.find("return;", view), content = draw.find("my_reports_.Draw(ui, facts);");
    CHECK(collect < loading && loading < start && start < view && view < leave && leave < content);
    CHECK(Has(ReadAll(kSource / "launcher" / "incident_page.h"), "bool Loading() const { return !logs_found_; }"));

    const std::string scan = Body(incident, "void IncidentPage::StartLogScan(");
    CHECK(Has(scan, "if (scan_) return;") && Has(scan, "std::thread(") && Has(scan, "FindGameLogs(") && Has(scan, ".detach();"));
    CHECK(CountOf(incident, "FindGameLogs(") == 2 && !Has(scan, "logs_ = "));
    const std::string collect_body = Body(incident, "void IncidentPage::CollectLogScan(");
    CHECK(Has(collect_body, "if (!scan_ || !scan_->done) return;") && Has(collect_body, "logs_found_ = true;"));
    CHECK(collect_body.find("scan_.reset();") < collect_body.find("logs_found_ = true;"));
    CHECK(!Has(draw, "FindLogs();"));
}

void TestLocalReportsStayVisibleWhileTheyAreChecked() {
    const std::string reports = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string draw = Body(reports, "void MyReports::Draw(");
    const size_t header = draw.find("DrawHeader(ui);"), list = draw.find("DrawReports(ui);");
    CHECK(header != std::string::npos && header < list);
    const std::string between = draw.substr(header, list - header);
    CHECK(!Has(between, "check_") && !Has(draw, "DrawLoadingView") && !Has(draw, "Loading"));
    CHECK(Has(reports, "ImGui::BeginDisabled(check_ != nullptr);") && Has(reports, "\"Checking...\""));
    const std::string collect = Body(reports, "void MyReports::Collect(");
    CHECK(Has(collect, "history_.Apply(*check_->statuses, check_->asked)") && Has(collect, "check_.reset();"));
    CHECK(!Has(collect, "reports_.clear") && !Has(Body(reports, "void MyReports::Refresh("), "history_.Load"));
}

void TestSlowPagesShowLoadingFirstAndReuseTheEditorView() {
    const std::string view = ReadAll(kSource / "launcher" / "loading_view.h");
    CHECK(Has(view, "inline void DrawLoadingView(trg::Ui& ui, const char* english_text)") && CountOf(view, "PathArcTo(") == 1);
    CHECK(Has(view, "Tr(english_text)") && Has(view, "GetTime()") && !Has(view, "OpenPopup"));

    size_t spinners = 0;
    for (const auto& entry : fs::directory_iterator(kSource / "launcher")) {
        if (entry.path().extension() == ".cpp") spinners += CountOf(ReadAll(entry.path()), "PathArcTo(");
    }
    CHECK(spinners == 0);

    const std::string editor = ReadAll(kSource / "launcher" / "editor_page.cpp");
    CHECK(Has(Body(editor, "void EditorPage::DrawAppearanceLoading("), "DrawLoadingView(ui, \"Loading appearance...\");"));
    const std::string editor_draw = Body(editor, "void EditorPage::Draw(");
    const size_t editor_view = editor_draw.find("DrawLoadingView(ui, \"Loading Editor...\");");
    CHECK(editor_draw.find("if (!loaded_ && !std::exchange(loading_shown_, true)) {") < editor_view &&
          editor_view < editor_draw.find("return;") && editor_draw.find("return;") < editor_draw.find("Refresh();"));

    const std::string trophies = ReadAll(kSource / "launcher" / "trophy_page.cpp");
    const std::string trophy_draw = Body(trophies, "void TrophyPage::Draw(");
    const size_t trophy_view = trophy_draw.find("DrawLoadingView(ui, \"Loading Trophies...\");");
    CHECK(trophy_view != std::string::npos && trophy_view < trophy_draw.find("LoadTrophyPage(") &&
          Has(trophy_draw, "!loaded_once_ && !app0_.empty() && !std::exchange(loading_shown_, true)"));

    CHECK(!Has(ReadAll(kSource / "launcher" / "launcher.cpp"), "DrawLoadingView"));
    CHECK(!Has(ReadAll(kSource / "launcher" / "online_page.cpp"), "DrawLoadingView"));
}

std::vector<std::string> WorkerFeedbackFields() {
    const std::string source = ReadAll(kSource / "server" / "lobby" / "src" / "incidents.js");
    const size_t start = source.find("const FEEDBACK_FIELDS = new Set([");
    const size_t end = source.find("]);", start);
    std::vector<std::string> fields;
    if (start == std::string::npos || end == std::string::npos) return fields;
    for (size_t at = source.find('\'', start); at != std::string::npos && at < end; at = source.find('\'', at + 1)) {
        const size_t close = source.find('\'', at + 1);
        fields.push_back(source.substr(at + 1, close - at - 1));
        at = close;
    }
    return fields;
}

TrackedReport AwaitingReport(int64_t revision) {
    TrackedReport report = Tracked("DCP-AAAAAA", 1000);
    report.status = kAwaitingPlayerTest;
    report.status_updated_at = revision;
    return report;
}

void TestFeedbackRequestIsExactlyWhatTheWorkerReads() {
    const int64_t revision = 1795191393443;
    const net::Json body = IncidentFeedbackRequest(AwaitingReport(revision), kTestFixed, SampleFacts());
    const std::string expected = std::string("{\"ticket\":\"DCP-AAAAAA\",\"receipt\":\"") + kReceipt +
                                 "\",\"revision\":1795191393443,\"result\":\"fixed\",\"launcher_version\":\"0.9.3\"," +
                                 "\"game_build\":\"CUSA10454 1.03 USA\"}";
    CHECK(body.dump() == expected);

    const std::vector<std::string> allowed = WorkerFeedbackFields();
    CHECK(allowed.size() == 6);
    for (const auto& [key, value] : body.members()) {
        CHECK(std::find(allowed.begin(), allowed.end(), key) != allowed.end());
        (void)value;
    }
    for (const std::string& key : allowed) CHECK(body.has(key));
    CHECK(body["revision"].as_int() == revision && body.dump().find("e+") == std::string::npos);
    CHECK(body["result"].as_string() == "fixed" && IncidentFeedbackRequest(AwaitingReport(revision), kTestNotFixed, SampleFacts())["result"].as_string() == "not_fixed");

    const std::string desk = ReadAll(kSource / "server" / "lobby" / "src" / "lobby.js");
    const std::size_t set_at = desk.find("const NO_SESSION_OPS = new Set([");
    const std::string no_session = set_at == std::string::npos ? "" : desk.substr(set_at, desk.find(']', set_at) - set_at);
    for (const char* op : {"'hello'", "'incident.status'", "'incident.read'", "'incident.feedback'"}) CHECK(Has(no_session, op));
    CHECK(!Has(no_session, "'incident.file'") && !Has(no_session, "admin") && !Has(no_session, "incident.list"));
    CHECK(Has(desk, "'incident.dump'(att, msg) {") && Has(desk, "'incident.dump_end'(att, msg) {") && Has(desk, "this.incidents.claimed(claim) === null"));
    CHECK(Has(desk, "'incident.feedback'(att, msg) {") && Has(desk, "readFeedback(msg)"));
    CHECK(Has(desk, "this.limitIncidentLookup(att, 'incidentFeedbackAddress');"));
}

void TestFeedbackAnswers() {
    const auto reply = [](const std::string& json) { return *net::Json::parse(json); };
    const net::Json accepted = reply(R"({"re":1,"code":"ok","ticket":"DCP-AAAAAA","revision":5,"test_feedback":"fixed","test_feedback_at":9})");
    FeedbackAnswer answer = FeedbackAnswerOf("ok", accepted, kTestFixed);
    CHECK(answer.outcome == FeedbackOutcome::kSaved && answer.stage == "call" && answer.code == "ok" && answer.detail.empty());

    answer = FeedbackAnswerOf("stale_request", reply(R"({"re":1,"code":"stale_request"})"), kTestFixed);
    CHECK(answer.outcome == FeedbackOutcome::kStale && answer.code == "stale_request");
    answer = FeedbackAnswerOf("already_answered", reply(R"({"re":1,"code":"already_answered"})"), kTestFixed);
    CHECK(answer.outcome == FeedbackOutcome::kStale && answer.code == "already_answered");

    answer = FeedbackAnswerOf("bad_request", reply(R"({"re":1,"code":"bad_request","detail":"revision"})"), kTestFixed);
    CHECK(answer.outcome == FeedbackOutcome::kFailed && answer.code == "bad_request" && answer.detail == "revision");
    answer = FeedbackAnswerOf("bad_request", reply(R"({"re":1,"code":"bad_request"})"), kTestFixed);
    CHECK(answer.outcome == FeedbackOutcome::kFailed && answer.detail.empty());
    answer = FeedbackAnswerOf("bad_request", reply("{\"detail\":\"" + std::string(500, 'x') + "\"}"), kTestFixed);
    CHECK(answer.detail.size() == 80);

    for (const char* malformed : {R"({"re":1,"code":"ok"})", R"({"re":1,"code":"ok","test_feedback":"pending"})",
                                  R"({"re":1,"code":"ok","test_feedback":"not_fixed"})", R"({"re":1,"code":"ok","test_feedback":7})"}) {
        answer = FeedbackAnswerOf("ok", reply(malformed), kTestFixed);
        CHECK(answer.outcome == FeedbackOutcome::kFailed && answer.code == "malformed_reply");
    }
    CHECK(FeedbackAnswerOf("ok", net::Json(), kTestFixed).outcome == FeedbackOutcome::kFailed);

    for (const std::string& code : {std::string(net::kLinkLost), std::string(net::kUnreachable), std::string("timeout")}) {
        answer = FeedbackAnswerOf(code, net::Json(), kTestFixed);
        CHECK(answer.outcome == FeedbackOutcome::kUnreachable && answer.code == code && answer.detail.empty());
    }
    CHECK(FeedbackAnswerOf("", net::Json(), kTestFixed).outcome == FeedbackOutcome::kFailed);
    CHECK(FeedbackAnswerOf("internal", reply(R"({"code":"internal"})"), kTestFixed).outcome == FeedbackOutcome::kFailed);
}

void TestFeedbackDiagnosticsNameTheStepAndNeverTheReceipt() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string send = Body(page, "void MyReports::SendFeedback(");
    const std::string collect = Body(page, "void MyReports::CollectFeedback(");
    CHECK(Has(send, "\"Bug report feedback: ticket=%s result=%s revision=%lld\\n\""));
    CHECK(Has(collect, "\"Bug report feedback: accepted ticket=%s result=%s\\n\""));
    CHECK(Has(collect, "\"Bug report feedback: failed ticket=%s result=%s stage=%s code=%s detail=%s\\n\""));
    CHECK(send.find("std::printf(") < send.find("std::thread("));
    for (const std::string* text : {&send, &collect}) {
        for (size_t at = text->find("printf("); at != std::string::npos; at = text->find("printf(", at + 1)) {
            const size_t end = text->find(");", at);
            const std::string statement = text->substr(at, end - at);
            CHECK(!Has(statement, "receipt") && !Has(statement, "credential") && !Has(statement, "dump()"));
        }
    }
    const std::string report = ReadAll(kSource / "launcher" / "incident_report.cpp");
    const std::string sending = report.substr(report.find("FeedbackAnswer SendTestFeedback("));
    CHECK(Has(sending, "\"connect\"") && Has(sending, "session.last_reply()") && !Has(sending, "printf"));
}

void TestPlayerTestPanel() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    const std::string panel = Body(page, "void MyReports::DrawPlayerTest(");
    CHECK(!panel.empty());
    for (const char* text : {"Did this fix the issue?", "Yes, fixed", "No, still happening", "You confirmed that this issue is fixed",
                             "The issue is still happening", "Please send a new Bug Report so we can receive fresh logs",
                             "Send a new report"}) {
        CHECK(Has(panel, std::string("\"") + text + "\""));
    }
    const std::string expanded = Body(page, "void MyReports::DrawExpanded(");
    const size_t message = expanded.find("DrawDeveloperMessage("), test = expanded.find("DrawPlayerTest("), facts = expanded.find("DrawFacts(");
    CHECK(message != std::string::npos && message < test && test < facts);

    CHECK(Has(panel, "ImGui::BeginDisabled(feedback_ != nullptr);") && Has(panel, "ImGui::EndDisabled();"));
    CHECK(Has(panel, "SendFeedback(report, kTestFixed,") && Has(panel, "SendFeedback(report, kTestNotFixed,"));
    CHECK(!Has(panel, "SendTestFeedback(") && !Has(panel, "FileIncident(") && !Has(panel, "BuildIncidentPackage("));
    CHECK(Has(panel, "new_report_requested_ = true;"));

    const std::string send = Body(page, "void MyReports::SendFeedback(");
    CHECK(Has(send, "if (feedback_) return;") && Has(send, "std::thread(") && Has(send, "SendTestFeedback(") && Has(send, ".detach();"));
    CHECK(!Has(send, "history_"));

    const std::string collect = Body(page, "void MyReports::CollectFeedback(");
    CHECK(CountOf(collect, "history_.RecordTestFeedback(") == 1 && Has(collect, "SaveHistory();"));
    CHECK(collect.find("FeedbackOutcome::kSaved") < collect.find("history_.RecordTestFeedback("));
    CHECK(Has(collect, "feedback_error_ticket_ = sent->ticket;") && Has(collect, "refresh_wanted_ = true;"));

    const std::string draw = Body(page, "void MyReports::Draw(");
    CHECK(Has(draw, "if (refresh_wanted_ && !check_) {") && Has(draw, "Refresh(LobbyConfig(ui));"));
    const std::string incident_page = ReadAll(kSource / "launcher" / "incident_page.cpp");
    CHECK(Has(incident_page, "my_reports_.Draw(ui, facts);") && Has(incident_page, "my_reports_.TakeNewReportRequest()"));
    CHECK(!Has(ReadAll(kSource / "launcher" / "incident_page.cpp"), "kTestFixed"));
}

std::vector<std::string> QuotedLiterals(const std::string& source) {
    std::vector<std::string> found;
    for (size_t i = source.find('"'); i != std::string::npos; i = source.find('"', i + 1)) {
        const size_t end = source.find('"', i + 1);
        if (end == std::string::npos) break;
        found.push_back(source.substr(i + 1, end - i - 1));
        i = end;
    }
    return found;
}

// What the player-test panel and its messages say, read from the source.
std::vector<std::string> PlayerTestLiterals() {
    const std::string page = ReadAll(kSource / "launcher" / "my_reports.cpp");
    std::vector<std::string> literals;
    for (const std::string& text : QuotedLiterals(Body(page, "void MyReports::DrawPlayerTest("))) {
        if (text.find(' ') != std::string::npos) literals.push_back(text);
    }
    return literals;
}

// The page's own text, read from its source: every literal a player can see.
std::vector<std::string> PageLiterals() {
    const std::string source = ReadAll(kSource / "launcher" / "incident_page.cpp");
    std::vector<std::string> found;
    std::string current;
    bool open = false;
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '#') {
            i = source.find('\n', i);
            if (i == std::string::npos) break;
            continue;
        }
        if (source[i] != '"') continue;
        size_t end = i + 1;
        while (source[end] != '"') end += source[end] == '\\' ? 2 : 1;
        current += source.substr(i + 1, end - i - 1);
        size_t next = end + 1;
        while (next < source.size() && (source[next] == ' ' || source[next] == '\n')) ++next;
        open = next < source.size() && source[next] == '"';
        if (!open) {
            found.push_back(current);
            current.clear();
        }
        i = end;
    }
    return found;
}

void TestPortuguese() {
    using namespace localization;
    SetActiveLanguage(kPortugueseBrazilKey);
    int checked = 0;
    for (const std::string& text : PageLiterals()) {
        const bool visible = !text.empty() && !text.starts_with("##") && text.find(' ') != std::string::npos &&
                             text.front() != ' ' && text.find('\\') == std::string::npos;
        if (!visible && text != "Preview" && text != "Size" && text != "Sending...") continue;
        ++checked;
        if (Tr(text) == text) std::printf("no Portuguese text for: %s\n", text.c_str());
        CHECK(Tr(text) != text);
    }
    CHECK(checked >= 25);
    const auto package = [] {
        TempFolder temp;
        WriteLog(temp.path, LogName(1), kGameSession, 5);
        return BuildIncidentPackage("x", FindGameLogs(temp.path, {})[0], SampleFacts(), {});
    }();
    CHECK(package.has_value());
    if (package) {
        for (const IncidentField& field : package->fields) {
            if (field.label != "CUSA") CHECK(Tr(std::string(field.label)) != field.label);
        }
    }
    CHECK(std::string(Tr("Bug Report")) == "Relatar bug");
    CHECK(std::string(Tr("Preview")) == "Visualizar envio");
    CHECK(std::string(Tr("1. Report the problem")) == "1. Relate o problema");
    CHECK(std::string(Tr("Please describe the problem, minimum 10 characters")) == "Descreva o problema, m\xC3\xADnimo de 10 caracteres");
    CHECK(std::string(Tr(LogRoleText(LogRole::kOtherGameplay))) == "Sess\xC3\xA3o de jogo");
    CHECK(std::string(Tr(LogRoleText(LogRole::kOtherLauncher))) == "Somente launcher");
    CHECK(std::string(Tr("View selected log")) == "Ver log selecionado");
    CHECK(!Has(ReadAll(kSource / "launcher" / "localization" / "pt_br_ui_text.cpp"), "Opens the selected log"));
    CHECK(std::string(Tr(LogRoleText(LogRole::kCurrentLauncher))) == "Sess\xC3\xA3o atual do launcher");
    CHECK(std::string(Tr(LogRoleText(LogRole::kCurrentGameplay))) == "Sess\xC3\xA3o atual \xC2\xB7 Launcher + jogo");
    CHECK(std::string(Tr(LogRoleText(LogRole::kLastGameplay))) == "\xC3\x9Altima partida");
    CHECK(std::string(Tr("Report a problem and send it with one game log.")) != "Report a problem and send it with one game log.");
    CHECK(std::string(Tr(IncidentLimitTitle(IncidentOutcome::kRecentLimit))) == "Muitos relat\xC3\xB3rios foram enviados recentemente");
    CHECK(std::string(Tr(IncidentLimitTitle(IncidentOutcome::kDailyLimit))) == "Limite di\xC3\xA1rio de relat\xC3\xB3rios atingido");
    CHECK(std::string(Tr("Please try again later")) == "Tente novamente mais tarde");
    const std::pair<const char*, const char*> statuses[] = {{"new", "Novo"}, {"investigating", "Investigando"},
        {"awaiting_player_test", "Aguardando teste do jogador"}, {"fixed", "Corrigido"},
        {"cannot_reproduce", "Não foi possível reproduzir"}, {"duplicate", "Duplicado"}, {"not_a_bug", "Não é um bug"}, {"closed", "Fechado"}};
    for (const auto& [status, text] : statuses) CHECK(std::string(Tr(ReportStatusText(status))) == text);
    const std::pair<const char*, const char*> reports[] = {{"My reports", "Meus relatórios"}, {"Fixed in version", "Corrigido na versão"},
        {"Message from developer", "Mensagem do desenvolvedor"}, {"Updated", "Atualizado"}, {"Refresh", "Atualizar"},
        {"New reply", "Nova resposta"}, {"No longer available", "N\xC3\xA3o est\xC3\xA1 mais dispon\xC3\xADvel"},
        {"A report has a new reply", "Um relat\xC3\xB3rio recebeu uma nova resposta"},
        {"The developer replied to one of your reports.", "O desenvolvedor respondeu a um dos seus relat\xC3\xB3rios."},
        {"This report was removed from the online service. The last known status is shown.",
         "Este relat\xC3\xB3rio foi removido do servi\xC3\xA7o online. O \xC3\xBAltimo status conhecido \xC3\xA9 mostrado."},
        {"No reports sent from this PC", "Nenhum relatório enviado por este PC"}, {"Remove from this PC", "Remover deste PC"},
        {"Only reports sent from this PC are listed here.", "Somente relatórios enviados por este PC aparecem aqui."}};
    for (const auto& [english, portuguese] : reports) CHECK(std::string(Tr(english)) == portuguese);
    for (const std::string& english : PlayerTestLiterals()) {
        if (Tr(english) == english) std::printf("no Portuguese text for: %s\n", english.c_str());
        CHECK(Tr(english) != english);
    }
    const std::pair<const char*, const char*> loading[] = {
        {"Close details", "Fechar detalhes"}, {"Loading Bug Report...", "Carregando Relat\xC3\xB3rio de bug..."},
        {"Loading Editor...", "Carregando Editor..."}, {"Loading Trophies...", "Carregando Trof\xC3\xA9us..."},
        {"Loading appearance...", "Carregando apar\xC3\xAAncia..."}};
    for (const auto& [english, portuguese] : loading) CHECK(std::string(Tr(english)) == portuguese);
    CHECK(std::string(Tr("Did this fix the issue?")) == "Isso resolveu o problema?");
    CHECK(std::string(Tr("Yes, fixed")) == "Sim, resolvido");
    CHECK(std::string(Tr("No, still happening")) == "N\xC3\xA3o, ainda acontece");
    CHECK(std::string(Tr("You confirmed that this issue is fixed")) == "Voc\xC3\xAA confirmou que este problema foi resolvido");
    CHECK(std::string(Tr("The issue is still happening")) == "O problema ainda est\xC3\xA1 acontecendo");
    TrackedReport fixed;
    fixed.status = "fixed";
    fixed.fixed_version = "0.9.3";
    CHECK(ReportStatusLine(fixed) == "Corrigido na versão 0.9.3");
    CHECK(std::string(Tr("Send report")) == "Enviar relatório");
    CHECK(std::string(Tr("Selected log")) == "Log selecionado");
    CHECK(std::string(Tr("Description")) == "Descrição");
    CHECK(std::string(Tr("No personal user data is intentionally included.")) == "Nenhum dado pessoal do usu\xC3\xA1rio \xC3\xA9 inclu\xC3\xAD" "do intencionalmente.");
    CHECK(Has(Tr(std::string("Before anything is sent, the launcher takes "
                             "addresses, user and computer names, private folders, passwords, online secrets and other "
                             "players' details out of the selected log. Automatic filtering cannot promise to catch "
                             "everything, so open the preview if you want to check what goes out.")),
              "Antes de qualquer envio"));
    SetActiveLanguage("english");
}

} // namespace

int main() {
    TestNewestFiveFirst();
    TestFewerThanFive();
    TestSessionsThatAreNotGameLogs();
    TestCurrentSessionAndLastGameplay();
    TestLauncherSessionsAreNotGameplay();
    TestDefaultLog();
    TestOneChosenLogIsSent();
    TestAddresses();
    TestDiagnosticsStay();
    TestLocalPaths();
    TestNames();
    TestSecrets();
    TestOtherPlayers();
    TestTextIsClean();
    TestSizeLimit();
    TestPreviewIsTheRequest();
    TestPreviewButton();
    TestPageStackPairs();
    TestDraftChecks();
    TestDescriptionLimit();
    TestLogListText();
    TestOutcomes();
    TestLimitsKeepTheDraft();
    TestResultComesIntoView();
    TestNoQuotaOnThePage();
    TestSentReportIsRemembered();
    TestHistoryIsBounded();
    TestHistorySaveFailureKeepsThePrevious();
    TestDamagedHistoryIsSetAside();
    TestStatusChangesMarkAReportUpdated();
    TestDeveloperReplyIsKeptApartFromAStatusChange();
    TestReadStateSurvivesARestart();
    TestOlderHistoryFilesLoadSafely();
    TestReportDeletedOnTheServiceIsUnavailable();
    TestRemoveIsLocal();
    TestStatusProtocol();
    TestStatusTexts();
    TestPlayerTestIsOpenUntilAnswered();
    TestANewRequestCanBeAnsweredAgain();
    TestTheServiceAnswerIsKept();
    TestThePlayerAnswerSurvivesARestart();
    TestHistoryFilesWithoutAnswersStillLoad();
    TestFeedbackProtocol();
    TestAPageIsEnteredOnceNotOnEveryFrame();
    TestBugReportRefreshesOnStartupAndOnEveryEntry();
    TestBugReportShowsLoadingWhileItsLogsAreListed();
    TestLocalReportsStayVisibleWhileTheyAreChecked();
    TestSlowPagesShowLoadingFirstAndReuseTheEditorView();
    TestFeedbackRequestIsExactlyWhatTheWorkerReads();
    TestFeedbackAnswers();
    TestFeedbackDiagnosticsNameTheStepAndNeverTheReceipt();
    TestPlayerTestPanel();
    TestReceiptsStayOutOfLogsAndSettings();
    TestLookupsTakeNoAccountAndNeverBlock();
    TestNewStatusesCollectOnTheUiThread();
    TestMyReportsPageStack();
    TestRefreshLivesInTheHeader();
    TestDetailsExpandInPlace();
    TestExpandedReportIsOneFramedItem();
    TestWhatWasSentStaysInTheReport();
    TestStartupCheckNoticesAndShowWork();
    TestDeveloperMessageIsItsOwnPanel();
    TestSelectedLogButtonSitsInItsFacts();
    TestCrashNeedsItsOwnDump();
    TestLogWordsDoNotMakeACrash();
    TestDumpOfAnotherRunIsNotPaired();
    TestSelectedCrashLogAttachesByDefault();
    TestUncheckedSendsOnlyTheLog();
    TestCheckedSendsTheLogAndItsDump();
    TestUnexpectedDumpNamesAreRefused();
    TestDumpPathTraversalIsRefused();
    TestOversizedDumpIsRefusedCleanly();
    TestEmptyAndForeignDumpFiles();
    TestLinksAndDirectoriesAreNeverAttached();
    TestDumpPairingWindowIsTheReferenceOne();
    TestPreviewDescribesTheDumpWithoutItsBytes();
    TestDumpIsReadFromTheFileWhenSent();
    TestCrashStringsAreTranslated();
    TestOrdinaryReportsAndHistoryAreUnchanged();
    TestRetryUsesTheReportAlreadyFiled();
    TestCrashDumpIsNeverLogged();
    TestCrashPageKeepsItsLabels();
    TestRetryNeverFilesAgain();
    TestDumpWireMatchesTheWorker();
    TestLobbyAgrees();
    TestProductionLobbyOnly();
    TestPortuguese();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
