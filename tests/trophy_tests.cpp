#include <cstdio>
#include <fcntl.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "crypto.h"
#include "dcp_png.h"
#include "options.h"
#include "trophy_content.h"
#include "trophies/trophy_key.h"
#include "trophies/trophy_data.h"
#include "trophies/trophy_pack.h"
#include "trophies/trophy_service.h"
#include "trophies/trophy_store.h"
#include "trophies/trophy_view.h"
#include "trophies/trp_archive.h"

namespace fs = std::filesystem;
using namespace dcp;
using namespace dcp::trophies;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

constexpr char kNpId[] = "NPWR00001_00";
const TrophyKey kKey = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

struct Spec {
    int id;
    char grade;
    bool hidden;
    int pid;
    int gid;
    const char* name;
    const char* detail;
};

const std::vector<Spec> kSpecs = {
    {0, 'P', false, -1, 0, "All Done", "Earn every trophy."},
    {1, 'G', false, 0, 1, "Gold One", "Do a &quot;gold&quot; thing &amp; more."},
    {2, 'S', false, 0, 1, "Silver One", "Do a silver thing."},
    {3, 'B', true, 0, 1, "Secret", "The hidden description."},
    {4, 'B', false, 0, 1, "Bronze One", "Do a bronze thing."},
};

std::string ConfXml(const std::vector<Spec>& specs) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<trophyconf version=\"1.1\">\n<npcommid>" << kNpId
        << "</npcommid>\n";
    for (const Spec& s : specs) {
        xml << "<trophy id=\"" << (s.id < 10 ? "00" : "0") << s.id << "\" hidden=\"" << (s.hidden ? "yes" : "no")
            << "\" ttype=\"" << s.grade << "\" pid=\"" << s.pid << "\"";
        if (s.gid >= 0) xml << " gid=\"" << s.gid << "\"";
        xml << "/>\n";
    }
    xml << "</trophyconf>\n";
    return xml.str();
}

std::string TextXml(const std::vector<Spec>& specs, const char* title) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<trophyconf version=\"1.1\">\n<title-name>" << title
        << "</title-name>\n<title-detail>Test detail</title-detail>\n<group id=\"000\"/>\n";
    for (const Spec& s : specs) {
        xml << "<trophy id=\"" << (s.id < 10 ? "00" : "0") << s.id << "\"><name>" << s.name << "</name><detail>"
            << s.detail << "</detail></trophy>\n";
    }
    xml << "</trophyconf>\n";
    return xml.str();
}

void WriteAll(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << data;
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string Hex(const uint8_t* data, size_t size) {
    std::string out;
    char pair[3];
    for (size_t i = 0; i < size; ++i) {
        std::snprintf(pair, sizeof(pair), "%02x", data[i]);
        out += pair;
    }
    return out;
}

std::string FromHex(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out += char(std::stoi(hex.substr(i, 2), nullptr, 16));
    return out;
}

void Put32(std::string& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = char(v >> (24 - 8 * i) & 255);
}

void Put64(std::string& out, size_t at, uint64_t v) {
    Put32(out, at, uint32_t(v >> 32));
    Put32(out, at + 4, uint32_t(v));
}

struct TrpEntry {
    std::string name;
    uint32_t flag;
    std::string data;
};

std::string EncryptList(const std::string& xml, const std::string& np_id, const TrophyKey& key) {
    TrophyKey list_key{};
    CHECK(DeriveListKey(key, np_id, list_key));
    std::string padded = xml;
    padded.resize((xml.size() + 15) / 16 * 16 + 16, '\0');
    uint8_t iv[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 9, 8, 7, 6, 5, 4};
    const std::string iv_text(reinterpret_cast<const char*>(iv), 16);
    crypto::AesKey aes;
    aes.Init(crypto::AesMode::kCbc, list_key.data());
    aes.Encrypt(reinterpret_cast<uint8_t*>(padded.data()), padded.size(), iv);
    return iv_text + padded;
}

std::string BuildTrp(const std::vector<TrpEntry>& entries, size_t entry_size = 64) {
    const size_t table = 0x60 + entries.size() * entry_size;
    std::string file(table, '\0');
    Put32(file, 0, 0xDCA24D00);
    Put32(file, 4, 3);
    Put32(file, 0x10, uint32_t(entries.size()));
    Put32(file, 0x14, uint32_t(entry_size));
    for (size_t i = 0; i < entries.size(); ++i) {
        const size_t at = 0x60 + i * entry_size;
        std::memcpy(&file[at], entries[i].name.data(), entries[i].name.size());
        Put64(file, at + 32, file.size());
        Put64(file, at + 40, entries[i].data.size());
        Put32(file, at + 48, entries[i].flag);
        file += entries[i].data;
    }
    Put64(file, 8, file.size());
    return file;
}

std::string PngBytes() {
    return std::string("\x89PNG\r\n\x1a\n", 8) + "synthetic";
}

std::vector<TrpEntry> StandardEntries(const std::vector<Spec>& specs = kSpecs, const TrophyKey& key = kKey) {
    std::vector<TrpEntry> entries;
    entries.push_back({"TROPCONF.ESFM", 3, EncryptList(ConfXml(specs), kNpId, key)});
    entries.push_back({"TROP.ESFM", 3, EncryptList(TextXml(specs, "Default"), kNpId, key)});
    entries.push_back({"TROP_01.ESFM", 3, EncryptList(TextXml(specs, "English"), kNpId, key)});
    entries.push_back({"ICON0.PNG", 0, PngBytes()});
    for (const Spec& s : specs) {
        entries.push_back({TrophyIconName(s.id), 0, PngBytes()});
    }
    return entries;
}

std::string NpBind(const char* id) {
    std::string data(0x214, '\0');
    Put32(data, 0, 0xD294A018);
    Put32(data, 4, 1);
    data[0x80] = 0x00;
    data[0x81] = 0x10;
    data[0x82] = 0x00;
    data[0x83] = 0x0C;
    std::memcpy(&data[0x84], id, 12);
    return data;
}

std::string KeyJson(const TrophyKey& key) {
    return "{\"TrophyKeySet\":{\"ReleaseTrophyKey\":\"" + Hex(key.data(), key.size()) + "\"}}";
}

// Upper-case pack file names, as some dumps have them.
fs::path MakeGame(const fs::path& base, const char* name, const std::string& trp, const char* np_id = kNpId) {
    const fs::path app0 = base / name;
    WriteAll(app0 / "sce_sys" / "npbind.dat", NpBind(np_id));
    if (!trp.empty()) {
        WriteAll(app0 / "sce_sys" / "TROPHY" / "TROPHY00.TRP", trp);
    }
    return app0;
}

struct Session {
    TrophyService service;
    int32_t context = 0, handle = 0;

    Session(const fs::path& app0, const fs::path& user, int64_t* clock = nullptr) {
        if (clock) service.SetClock([clock] { return (*clock)++; });
        service.Configure(app0, user);
        CHECK(service.CreateContext(&context, 1, 0, 0) == 0);
        CHECK(service.CreateHandle(&handle) == 0);
        CHECK(service.RegisterContext(context, handle, 0) == 0);
    }
    int32_t Unlock(int id, int32_t* platinum = nullptr) {
        int32_t unused = 0;
        return service.UnlockTrophy(context, handle, id, platinum ? platinum : &unused);
    }
    uint32_t Count(FlagArray& flags) {
        uint32_t count = 99;
        CHECK(service.GetUnlockState(context, handle, &flags, &count) == 0);
        return count;
    }
};

TrophySet LoadedSet() {
    TrophySet set = *ParseTrophyConf(ConfXml(kSpecs));
    ApplyTrophyTexts(set, *ParseTrophyTexts(TextXml(kSpecs, "English")));
    return set;
}

void TestErrorCodes() {
    CHECK(err::kInvalidArgument == int32_t(0x80551604) && err::kInvalidHandle == int32_t(0x80551608));
    CHECK(err::kInvalidContext == int32_t(0x80551609) && err::kInvalidTrophyId == int32_t(0x8055160A));
    CHECK(err::kTrophyAlreadyUnlocked == int32_t(0x8055160C) && err::kPlatinumCannotUnlock == int32_t(0x8055160D));
    CHECK(err::kNotRegistered == int32_t(0x8055160F) && err::kAlreadyRegistered == int32_t(0x80551610));
    CHECK(err::kContextAlreadyExists == int32_t(0x80551613) && err::kContextExceedsMax == int32_t(0x80551623));
    CHECK(err::kHandleExceedsMax == int32_t(0x80551624));
}

void TestParsing() {
    const std::optional<TrophySet> conf = ParseTrophyConf(ConfXml(kSpecs));
    CHECK(conf.has_value());
    if (!conf) return;
    CHECK(conf->np_comm_id == kNpId && conf->trophies.size() == 5);
    CHECK(conf->Find(0)->grade == Grade::kPlatinum && conf->Find(1)->grade == Grade::kGold);
    CHECK(conf->Find(2)->grade == Grade::kSilver && conf->Find(4)->grade == Grade::kBronze);
    CHECK(conf->Count(Grade::kBronze) == 2 && conf->Count(Grade::kPlatinum) == 1);
    CHECK(conf->Find(3)->hidden && !conf->Find(4)->hidden);
    CHECK(conf->Find(0)->platinum_link == -1 && conf->Find(0)->group == 0 && conf->Find(1)->platinum_link == 0);
    CHECK(!CountsTowardPlatinum(*conf->Find(0)) && CountsTowardPlatinum(*conf->Find(1)));
    CHECK(conf->Find(9) == nullptr && TrophyIconName(7) == "TROP007.PNG");
    CHECK(conf->Find(1)->name.empty());

    const TrophySet set = LoadedSet();
    CHECK(set.title == "English" && set.detail == "Test detail" && set.groups == 1);
    CHECK(set.Find(1)->detail == "Do a \"gold\" thing & more." && set.Find(3)->name == "Secret");

    const std::optional<TrophySet> no_group = ParseTrophyConf(
        "<trophyconf><trophy id=\"1\" ttype=\"B\" pid=\"0\"/><trophy id=\"2\" ttype=\"B\" pid=\"0\" gid=\"2\"/></trophyconf>");
    CHECK(no_group && no_group->Find(1)->group == -1 && CountsTowardPlatinum(*no_group->Find(1)) &&
          CountsTowardPlatinum(*no_group->Find(2)));

    CHECK(!ParseTrophyConf("") && !ParseTrophyTexts(""));
    CHECK(!ParseTrophyConf("not xml at all"));
    CHECK(!ParseTrophyConf("<trophyconf><trophy id=\"1\" ttype=\"B\"><name>x</name>"));
    CHECK(!ParseTrophyConf("<trophyconf><trophy id=\"1\" ttype=\"Q\"/></trophyconf>"));
    CHECK(!ParseTrophyConf("<trophyconf><trophy id=\"128\" ttype=\"B\"/></trophyconf>"));
    CHECK(!ParseTrophyConf("<trophyconf><trophy id=\"-1\" ttype=\"B\"/></trophyconf>"));
    CHECK(!ParseTrophyConf("<trophyconf><trophy id=\"1\" ttype=\"B\"/><trophy id=\"1\" ttype=\"G\"/></trophyconf>"));
    CHECK(!ParseTrophyConf("<trophyconf></trophyconf>"));
    CHECK(!ParseTrophyConf("<other><trophy id=\"1\" ttype=\"B\"/></other>"));
    std::string deep = "<trophyconf>";
    for (int i = 0; i < 100; ++i) deep += "<a>";
    CHECK(!ParseTrophyConf(deep) && !ParseTrophyTexts(deep));
}

void TestStore(const fs::path& base) {
    const fs::path dir = base / "store";
    TrophyStore store(dir, kNpId, 1);
    store.Load();
    CHECK(store.UnlockedCount() == 0 && !store.IsUnlocked(3));
    CHECK(store.Unlock(3, 1000));
    CHECK(!store.Unlock(3, 2000));
    CHECK(store.UnlockTime(3) == 1000);
    CHECK(!store.Unlock(128, 1) && !store.Unlock(-1, 1));
    CHECK(store.Save());
    TrophyStore again(dir, kNpId, 1);
    again.Load();
    CHECK(again.IsUnlocked(3) && again.UnlockTime(3) == 1000 && again.UnlockedCount() == 1);
    TrophyStore other_user(dir, kNpId, 2);
    other_user.Load();
    CHECK(other_user.UnlockedCount() == 0);

    WriteAll(again.file(), "garbage\nunlocked 1 1\n");
    TrophyStore broken(dir, kNpId, 1);
    broken.Load();
    CHECK(broken.UnlockedCount() == 0);
    CHECK(fs::exists(again.file().string() + ".corrupt") && !fs::exists(again.file()));
    CHECK(broken.Unlock(1, 5) && broken.Save());

    WriteAll(again.file(), "DCP-TROPHIES 1\nnpcommid NPWR99999_99\nunlocked 1 1\n");
    TrophyStore foreign(dir, kNpId, 1);
    foreign.Load();
    CHECK(foreign.UnlockedCount() == 0);

    WriteAll(again.file(), "DCP-TROPHIES 1\r\nnpcommid " + std::string(kNpId) + "\r\nunlocked 2 7\nunlocked 2 9\nunlocked x\nunlocked 999 1\n");
    TrophyStore tolerant(dir, kNpId, 1);
    tolerant.Load();
    CHECK(tolerant.UnlockedCount() == 1 && tolerant.UnlockTime(2) == 7);
    CHECK(IsNpCommId("NPWR03852_00") && !IsNpCommId("NPWR03852-00") && !IsNpCommId("../../etc/x") && !IsNpCommId(""));
}

void TestView() {
    const TrophySet set = LoadedSet();
    TrophyStore store;
    TrophyView view = BuildTrophyView(set, store);
    CHECK(view.total == 5 && view.unlocked == 0 && view.percent == 0);
    CHECK(view.rows[3].masked && view.rows[3].name == kHiddenTrophyName &&
          view.rows[3].detail.find("hidden description") == std::string::npos);
    CHECK(!view.rows[4].masked && view.rows[4].name == "Bronze One");
    store.Unlock(3, 10);
    store.Unlock(4, 11);
    view = BuildTrophyView(set, store);
    CHECK(view.unlocked == 2 && view.percent == 40);
    CHECK(!view.rows[3].masked && view.rows[3].name == "Secret" && view.rows[3].detail == "The hidden description.");
    CHECK(view.total_by_grade[size_t(Grade::kBronze)] == 2 && view.unlocked_by_grade[size_t(Grade::kBronze)] == 2);
    CHECK(view.unlocked_by_grade[size_t(Grade::kGold)] == 0);
    CHECK(ProgressPercent(12, 49) == 24 && ProgressPercent(0, 0) == 0 && ProgressPercent(49, 49) == 100);
}

void TestKeyAndCrypto(const fs::path& base) {
    const fs::path user = base / "key-user";
    const std::string good = Hex(kKey.data(), kKey.size());
    const auto load = [&](const std::string& json) {
        WriteAll(user / "keys.json", json);
        return LoadTrophyKey(user);
    };
    CHECK(load("{\"TrophyKeySet\":{\"ReleaseTrophyKey\":\"" + good + "\"}}") == kKey);
    CHECK(load("{\"TrophyKeySet\": {\"ReleaseTrophyKey\" : \"0A0B0c0d0E0F00010203040506070809\"}}") ==
          TrophyKey({0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
    CHECK(!load("{\"TrophyKeySet\":{\"ReleaseTrophyKey\":\"" + good.substr(0, 30) + "\"}}"));
    CHECK(!load("{\"TrophyKeySet\":{\"ReleaseTrophyKey\":\"" + good.substr(0, 31) + "\"}}"));
    CHECK(!load("{\"TrophyKeySet\":{\"ReleaseTrophyKey\":\"" + good + "00\"}}"));
    CHECK(!load("{\"TrophyKeySet\":{\"ReleaseTrophyKey\":\"" + good + "0\"}}"));
    CHECK(!load("{\"TrophyKeySet\":{\"ReleaseTrophyKey\":\"" + good.substr(0, 31) + "g\"}}"));
    CHECK(!load("{\"TrophyKeySet\":{\"ReleaseTrophyKey\":\" " + good.substr(0, 31) + "\"}}"));
    CHECK(!load("{\"TrophyKeySet\":{\"ReleaseTrophyKey\":\"\"}}"));
    CHECK(!load("{\"TrophyKeySet\":{\"ReleaseTrophyKey\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16]}}"));
    CHECK(!load("{\"TrophyKeySet\":{}}"));
    fs::remove(user / "keys.json");
    CHECK(!LoadTrophyKey(user));

    // AES-128 key 000102..0f; NP Communication ID "NPWR00001_00" zero-padded to 16 bytes; vectors made with OpenSSL.
    const TrophyKey fips = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    TrophyKey list_key{};
    CHECK(DeriveListKey(fips, kNpId, list_key) && Hex(list_key.data(), 16) == "4761044ac7eada69a8872d54f1788c12");
    CHECK(!DeriveListKey(fips, "NPWR", list_key));
    const std::string entry = FromHex(
        "090807060504030201000908070605045e6ed72d10a649464dea45050203b37033f55afef384e96d56898376bb5b176ce7d5072bb54a87ac6fdf"
        "617c1cdf4d6a8ca820d45d6bd47e35cdebb3068e7032");
    const auto* bytes = reinterpret_cast<const uint8_t*>(entry.data());
    const std::string expected = "<?xml version=\"1.0\"?><trophyconf/>";
    std::string xml;
    CHECK(DecryptTrophyList(fips, kNpId, bytes, entry.size(), xml) && xml == expected);
    CHECK(!DecryptTrophyList(kKey, kNpId, bytes, entry.size(), xml));
    CHECK(!DecryptTrophyList(fips, "NPWR00002_00", bytes, entry.size(), xml));
    CHECK(!DecryptTrophyList(fips, kNpId, bytes, 16, xml) && !DecryptTrophyList(fips, kNpId, bytes, 0, xml));
    CHECK(!DecryptTrophyList(fips, kNpId, bytes, entry.size() - 1, xml));
    CHECK(!DecryptTrophyList(fips, kNpId, bytes, entry.size() - 16 + 3, xml));

    const std::string signed_entry = FromHex(
        "0908070605040302010009080706050424af038e8f0d28965203111990965997ee8dff9f7264c040ce41c81d5e43cc66d36a2f091ed2388faf228c"
        "4682c7e3476be99bd9051f949f17320e99dcf50784");
    CHECK(DecryptTrophyList(fips, kNpId, reinterpret_cast<const uint8_t*>(signed_entry.data()), signed_entry.size(), xml) &&
          xml == "<!--Sce-Np-Trophy-Signature: x-->\n<trophyconf/>");
}

void TestTrp(const fs::path& base) {
    const fs::path dest = base / "trp-ok";
    const fs::path trp = base / "ok.trp";
    WriteAll(trp, BuildTrp(StandardEntries()));
    TrpResult r = ExtractTrp(trp, kNpId, kKey, dest);
    CHECK(r.status == TrpStatus::kOk && r.icons == 6 && r.lists == 3);
    CHECK(fs::exists(dest / "Icons" / "TROP000.PNG") && fs::exists(dest / "Xml" / "TROP_01.XML") &&
          fs::exists(dest / "Xml" / "TROPCONF.XML"));
    CHECK(ParseTrophyConf(ReadAll(dest / "Xml" / "TROPCONF.XML")).has_value());
    CHECK(ParseTrophyTexts(ReadAll(dest / "Xml" / "TROP_01.XML")).has_value());

    r = ExtractTrp(trp, kNpId, std::nullopt, base / "trp-nokey");
    CHECK(r.status == TrpStatus::kKeyMissing && r.icons == 6 && r.lists == 0);
    TrophyKey wrong = kKey;
    wrong[0] ^= 0xFF;
    r = ExtractTrp(trp, kNpId, wrong, base / "trp-wrong");
    CHECK(r.status == TrpStatus::kKeyRejected && r.lists == 0 && !fs::exists(base / "trp-wrong" / "Xml" / "TROPCONF.XML"));
    r = ExtractTrp(trp, "NPWR00002_00", kKey, base / "trp-wrongid");
    CHECK(r.status == TrpStatus::kKeyRejected);

    for (const size_t size : {size_t(0), size_t(10), size_t(16), size_t(17), size_t(31)}) {
        std::vector<TrpEntry> entries = {{"TROP.ESFM", 3, std::string(size, 'x')}};
        WriteAll(base / "enc.trp", BuildTrp(entries));
        CHECK(ExtractTrp(base / "enc.trp", kNpId, kKey, base / "enc-out").status == TrpStatus::kBadEntry);
        CHECK(ExtractTrp(base / "enc.trp", kNpId, std::nullopt, base / "enc-out2").status == TrpStatus::kBadEntry);
    }
    std::vector<TrpEntry> junk = {{"TROP.ESFM", 3, std::string(48, 'x')}};
    WriteAll(base / "junk.trp", BuildTrp(junk));
    CHECK(ExtractTrp(base / "junk.trp", kNpId, kKey, base / "junk-out").status == TrpStatus::kKeyRejected);

    const std::string trp_bytes = ReadAll(trp);
    WriteAll(base / "short.trp", trp_bytes.substr(0, 40));
    CHECK(ExtractTrp(base / "short.trp", kNpId, kKey, base / "t1").status == TrpStatus::kUnreadable);
    CHECK(ExtractTrp(base / "missing.trp", kNpId, kKey, base / "t1").status == TrpStatus::kUnreadable);
    WriteAll(base / "cut.trp", trp_bytes.substr(0, 0x60 + 64 * 3));
    CHECK(ExtractTrp(base / "cut.trp", kNpId, kKey, base / "t2").status == TrpStatus::kBadHeader);
    WriteAll(base / "cutdata.trp", trp_bytes.substr(0, trp_bytes.size() - 5));
    CHECK(ExtractTrp(base / "cutdata.trp", kNpId, kKey, base / "t2b").status == TrpStatus::kBadEntry);
    std::string bad_magic = trp_bytes;
    bad_magic[0] = 0;
    WriteAll(base / "magic.trp", bad_magic);
    CHECK(ExtractTrp(base / "magic.trp", kNpId, kKey, base / "t3").status == TrpStatus::kBadHeader);
    std::string many = trp_bytes;
    Put32(many, 0x10, 100000);
    WriteAll(base / "many.trp", many);
    CHECK(ExtractTrp(base / "many.trp", kNpId, kKey, base / "t4").status == TrpStatus::kBadHeader);

    std::string offset = trp_bytes;
    Put64(offset, 0x60 + 32, offset.size() + 100);
    WriteAll(base / "offset.trp", offset);
    CHECK(ExtractTrp(base / "offset.trp", kNpId, kKey, base / "t5").status == TrpStatus::kBadEntry);
    std::string overflow = trp_bytes;
    Put64(overflow, 0x60 + 32, 0xFFFFFFFFFFFFFFF0ull);
    Put64(overflow, 0x60 + 40, 0x20);
    WriteAll(base / "overflow.trp", overflow);
    CHECK(ExtractTrp(base / "overflow.trp", kNpId, kKey, base / "t6").status == TrpStatus::kBadEntry);
    std::string oversized = trp_bytes;
    Put64(oversized, 0x60 + 40, 1ull << 40);
    WriteAll(base / "oversized.trp", oversized);
    CHECK(ExtractTrp(base / "oversized.trp", kNpId, kKey, base / "t7").status == TrpStatus::kBadEntry);

    for (const char* name : {"../evil.PNG", "..\\evil.PNG", "sub/evil.PNG", "C:evil.PNG", "..", ".hidden.PNG"}) {
        std::vector<TrpEntry> entries = {{name, 0, PngBytes()}};
        WriteAll(base / "evil.trp", BuildTrp(entries));
        CHECK(ExtractTrp(base / "evil.trp", kNpId, kKey, base / "evil-out").status == TrpStatus::kBadEntry);
        CHECK(!fs::exists(base / "evil.PNG"));
    }
    std::vector<TrpEntry> twice = {{"A.PNG", 0, PngBytes()}, {"a.png", 0, PngBytes()}};
    WriteAll(base / "twice.trp", BuildTrp(twice));
    CHECK(ExtractTrp(base / "twice.trp", kNpId, kKey, base / "t8").status == TrpStatus::kBadEntry);
    std::vector<TrpEntry> lower = {{"trop000.png", 0, PngBytes()}};
    WriteAll(base / "lower.trp", BuildTrp(lower));
    CHECK(ExtractTrp(base / "lower.trp", kNpId, kKey, base / "t9").status == TrpStatus::kOk);
    CHECK(fs::exists(base / "t9" / "Icons" / "TROP000.PNG"));
}

void TestPack(const fs::path& base) {
    const fs::path user = base / "pack-user";
    const std::string trp = BuildTrp(StandardEntries());
    const fs::path app0 = MakeGame(base, "pack-game", trp);
    CHECK(ReadNpCommId(app0 / "sce_sys" / "npbind.dat") == std::string(kNpId));
    WriteAll(base / "bad-npbind.dat", std::string(0x214, 'x'));
    CHECK(!ReadNpCommId(base / "bad-npbind.dat") && !ReadNpCommId(base / "none.dat"));

    PackResult pack = PrepareTrophyPack(app0, user);
    CHECK(pack.status == PackStatus::kKeyMissing && pack.np_comm_id == kNpId);
    CHECK(fs::exists(TrophyIconPath(user, kNpId, 1)) && !LoadTrophySet(user, kNpId));

    WriteAll(user / "keys.json", KeyJson(kKey));
    pack = PrepareTrophyPack(app0, user);
    CHECK(pack.status == PackStatus::kReady);
    std::optional<TrophySet> set = LoadTrophySet(user, kNpId);
    CHECK(set && set->title == "English" && set->trophies.size() == 5 && set->Find(1)->name == "Gold One");

    const fs::file_time_type stamp_time = fs::last_write_time(TrophyDirectory(user, kNpId) / "pack.stamp");
    CHECK(PrepareTrophyPack(app0, user).status == PackStatus::kReady);
    CHECK(fs::last_write_time(TrophyDirectory(user, kNpId) / "pack.stamp") == stamp_time);

    TrophyKey wrong = kKey;
    wrong[5] ^= 1;
    WriteAll(user / "keys.json", KeyJson(wrong));
    CHECK(PrepareTrophyPack(app0, user).status == PackStatus::kKeyRejected);
    CHECK(LoadTrophySet(user, kNpId).has_value());

    CHECK(PrepareTrophyPack(base / "nowhere", user).status == PackStatus::kNoNpBind);
    const fs::path nopack = MakeGame(base, "pack-none", "");
    CHECK(PrepareTrophyPack(nopack, user).status == PackStatus::kNoPack);
    const fs::path junk = MakeGame(base, "pack-junk", "not a trp, but long enough to be read by the unpacker, really......");
    CHECK(PrepareTrophyPack(junk, base / "junk-user").status == PackStatus::kDamaged);
    CHECK(!LoadTrophySet(user, "../escape") && !LoadTrophySet(user, "NPWR12345_00"));
}

struct Guest {
    alignas(8) unsigned char game_details[0x4A0] = {};
    alignas(8) unsigned char game_data[0x20] = {};
    alignas(8) unsigned char trophy_details[0x498] = {};
    alignas(8) unsigned char trophy_data[0x18] = {};
    Guest() {
        const uint64_t sizes[4] = {0x4A0, 0x20, 0x498, 0x18};
        std::memcpy(game_details, &sizes[0], 8);
        std::memcpy(game_data, &sizes[1], 8);
        std::memcpy(trophy_details, &sizes[2], 8);
        std::memcpy(trophy_data, &sizes[3], 8);
    }
};

void TestService(const fs::path& base) {
    const fs::path user = base / "svc-user";
    WriteAll(user / "keys.json", KeyJson(kKey));
    const fs::path app0 = MakeGame(base, "svc-game", BuildTrp(StandardEntries()));
    int64_t clock = 1700000000;
    {
        Session s(app0, user, &clock);
        FlagArray flags{};
        CHECK(s.Count(flags) == 5 && flags.bits[0] == 0);

        int32_t platinum = 77;
        CHECK(s.Unlock(2, &platinum) == 0 && platinum == kNoTrophy);
        platinum = 77;
        CHECK(s.Unlock(2, &platinum) == err::kTrophyAlreadyUnlocked && platinum == 77);
        CHECK(s.Unlock(99) == err::kInvalidTrophyId && s.Unlock(-1) == err::kInvalidTrophyId &&
              s.Unlock(128) == err::kInvalidTrophyId);
        CHECK(s.Unlock(0) == err::kPlatinumCannotUnlock);
        CHECK(s.service.UnlockTrophy(s.context, s.handle, 1, nullptr) == err::kInvalidArgument);
        CHECK(s.Unlock(3) == 0);
        CHECK(s.Count(flags) == 5 && flags.bits[0] == ((1u << 2) | (1u << 3)) && flags.bits[1] == 0);

        UnlockNotice notice;
        CHECK(s.service.TakeNotice(notice) && notice.id == 2 && notice.grade == Grade::kSilver && notice.name == "Silver One");
        CHECK(s.service.TakeNotice(notice) && notice.id == 3);
        CHECK(!s.service.TakeNotice(notice));

        Guest g;
        CHECK(s.service.GetGameInfo(s.context, s.handle, g.game_details, g.game_data) == 0);
        uint32_t d[6], p[6];
        std::memcpy(d, g.game_details + 8, sizeof(d));
        std::memcpy(p, g.game_data + 8, sizeof(p));
        CHECK(d[0] == 1 && d[1] == 5 && d[2] == 1 && d[3] == 1 && d[4] == 1 && d[5] == 2);
        CHECK(std::string(reinterpret_cast<char*>(g.game_details) + 32) == "English");
        CHECK(std::string(reinterpret_cast<char*>(g.game_details) + 32 + 128) == "Test detail");
        CHECK(p[0] == 2 && p[1] == 0 && p[2] == 0 && p[3] == 1 && p[4] == 1 && p[5] == 40);
        CHECK(s.service.GetGameInfo(s.context, s.handle, nullptr, g.game_data) == err::kInvalidArgument);
        CHECK(s.service.GetGameInfo(s.context, s.handle, g.game_details, nullptr) == err::kInvalidArgument);
        const uint64_t bad_size = 0x4A1;
        std::memcpy(g.game_details, &bad_size, 8);
        CHECK(s.service.GetGameInfo(s.context, s.handle, g.game_details, g.game_data) == err::kInvalidArgument);

        Guest t;
        CHECK(s.service.GetTrophyInfo(s.context, s.handle, 3, t.trophy_details, t.trophy_data) == 0);
        int32_t id, grade, group;
        std::memcpy(&id, t.trophy_details + 8, 4);
        std::memcpy(&grade, t.trophy_details + 12, 4);
        std::memcpy(&group, t.trophy_details + 16, 4);
        CHECK(id == 3 && grade == 4 && group == 1 && t.trophy_details[20] == 1);
        CHECK(std::string(reinterpret_cast<char*>(t.trophy_details) + 24) == "Secret");
        CHECK(std::string(reinterpret_cast<char*>(t.trophy_details) + 24 + 128) == "The hidden description.");
        CHECK(t.trophy_data[12] == 1);
        uint64_t stamp;
        std::memcpy(&stamp, t.trophy_data + 16, 8);
        CHECK(stamp == uint64_t(clock - 1));
        Guest locked;
        CHECK(s.service.GetTrophyInfo(s.context, s.handle, 4, locked.trophy_details, locked.trophy_data) == 0);
        CHECK(std::string(reinterpret_cast<char*>(locked.trophy_details) + 24) == "Bronze One" && locked.trophy_data[12] == 0);
        Guest absent;
        CHECK(s.service.GetTrophyInfo(s.context, s.handle, 99, absent.trophy_details, absent.trophy_data) == 0);
        CHECK(absent.trophy_details[8] == 0);
        CHECK(s.service.GetTrophyInfo(s.context, s.handle, 128, t.trophy_details, t.trophy_data) == err::kInvalidTrophyId);
        CHECK(s.service.GetTrophyInfo(s.context, s.handle, 4, t.trophy_details, nullptr) == err::kInvalidArgument);
        CHECK(s.service.GetTrophyInfo(s.context, s.handle, 4, nullptr, t.trophy_data) == err::kInvalidArgument);
        const uint64_t wrong = 0x40;
        std::memcpy(t.trophy_data, &wrong, 8);
        CHECK(s.service.GetTrophyInfo(s.context, s.handle, 4, t.trophy_details, t.trophy_data) == err::kInvalidArgument);
    }
    {
        Session s(app0, user, &clock);
        FlagArray flags{};
        CHECK(s.Count(flags) == 5 && flags.bits[0] == ((1u << 2) | (1u << 3)));
        TrophyStore store(TrophyDirectory(user, kNpId), kNpId, 1);
        store.Load();
        const TrophyView view = BuildTrophyView(*LoadTrophySet(user, kNpId), store);
        CHECK(view.unlocked == 2 && view.rows[2].unlocked && view.rows[3].unlocked && !view.rows[3].masked);
        CHECK(view.rows[2].unlock_time == 1700000000);

        int32_t platinum = 0;
        CHECK(s.Unlock(1, &platinum) == 0 && platinum == kNoTrophy);
        CHECK(s.Unlock(4, &platinum) == 0 && platinum == 0);
        CHECK(s.Unlock(0) == err::kTrophyAlreadyUnlocked);
        CHECK(s.Count(flags) == 5 && flags.bits[0] == 0x1F);
        UnlockNotice notice;
        int notices = 0;
        while (s.service.TakeNotice(notice)) ++notices;
        CHECK(notices == 3);
    }

    Session s(app0, user);
    int32_t extra = 0, platinum = 0;
    CHECK(s.service.CreateContext(&extra, 1, 0, 0) == err::kContextAlreadyExists);
    CHECK(s.service.CreateContext(&extra, 1, 1, 0) == 0 && extra != s.context);
    CHECK(s.service.CreateContext(nullptr, 1, 5, 0) == err::kInvalidArgument);
    CHECK(s.service.CreateContext(&extra, 1, 5, 7) == err::kInvalidArgument);
    CHECK(s.service.RegisterContext(-1, s.handle, 0) == err::kInvalidContext);
    CHECK(s.service.RegisterContext(999, s.handle, 0) == err::kInvalidContext);
    CHECK(s.service.RegisterContext(s.context, -1, 0) == err::kInvalidHandle);
    CHECK(s.service.RegisterContext(s.context, 999, 0) == err::kInvalidHandle);
    CHECK(s.service.RegisterContext(s.context, s.handle, 1) == err::kInvalidArgument);
    CHECK(s.service.RegisterContext(s.context, s.handle, 0) == err::kAlreadyRegistered);
    FlagArray flags{};
    uint32_t count = 0;
    CHECK(s.service.UnlockTrophy(extra, s.handle, 1, &platinum) == err::kNotRegistered);
    CHECK(s.service.GetUnlockState(extra, s.handle, &flags, &count) == err::kNotRegistered);
    Guest g;
    CHECK(s.service.GetGameInfo(extra, s.handle, g.game_details, g.game_data) == err::kNotRegistered);
    CHECK(s.service.GetTrophyInfo(extra, s.handle, 1, g.trophy_details, g.trophy_data) == err::kNotRegistered);
    CHECK(s.service.GetUnlockState(s.context, s.handle, nullptr, nullptr) == err::kInvalidArgument);
    CHECK(s.service.GetUnlockState(-1, s.handle, &flags, &count) == err::kInvalidContext);
    CHECK(s.service.GetUnlockState(s.context, -1, &flags, &count) == err::kInvalidHandle);
    CHECK(s.service.UnlockTrophy(-1, s.handle, 1, &platinum) == err::kInvalidContext);
    CHECK(s.service.UnlockTrophy(s.context, -1, 1, &platinum) == err::kInvalidHandle);
    CHECK(s.service.UnlockTrophy(999, s.handle, 1, &platinum) == err::kInvalidContext);
    CHECK(s.service.UnlockTrophy(s.context, 999, 1, &platinum) == err::kInvalidHandle);
    int32_t handles[4] = {};
    CHECK(s.service.CreateHandle(&handles[0]) == 0 && s.service.CreateHandle(&handles[1]) == 0 &&
          s.service.CreateHandle(&handles[2]) == 0);
    CHECK(s.service.CreateHandle(&handles[3]) == err::kHandleExceedsMax && s.service.CreateHandle(nullptr) == err::kInvalidArgument);
    CHECK(s.service.DestroyHandle(handles[0]) == 0 && s.service.CreateHandle(&handles[3]) == 0);
    int32_t made = 0, ignored = 0;
    int32_t status = 0;
    for (uint32_t label = 10; label < 30 && status == 0; ++label) {
        status = s.service.CreateContext(&ignored, 1, label, 0);
        made += status == 0;
    }
    CHECK(status == err::kContextExceedsMax && made == 6);
    CHECK(s.service.DestroyHandle(s.handle) == 0 && s.service.DestroyHandle(s.handle) == err::kInvalidHandle &&
          s.service.DestroyHandle(-1) == err::kInvalidHandle);
    CHECK(s.service.GetUnlockState(s.context, s.handle, &flags, &count) == err::kInvalidHandle);
    CHECK(s.service.DestroyContext(s.context) == 0 && s.service.DestroyContext(s.context) == err::kInvalidContext &&
          s.service.DestroyContext(-1) == err::kInvalidContext);
}

void TestPlatinum(const fs::path& base) {
    const fs::path user = base / "plat-user";
    WriteAll(user / "keys.json", KeyJson(kKey));
    const std::vector<Spec> specs = {
        {0, 'P', false, -1, 0, "P", "p"}, {1, 'G', false, 0, 1, "A", "a"}, {2, 'B', false, 0, 1, "B", "b"},
        {3, 'B', false, 0, 0, "Base group", "c"}, {4, 'S', false, -1, 1, "Unlinked", "d"}};
    Session s(MakeGame(base, "plat-game", BuildTrp(StandardEntries(specs))), user);
    int32_t platinum = 5;
    CHECK(s.Unlock(3, &platinum) == 0 && platinum == kNoTrophy);
    CHECK(s.Unlock(1, &platinum) == 0 && platinum == kNoTrophy);
    CHECK(s.Unlock(4, &platinum) == 0 && platinum == kNoTrophy);
    CHECK(s.Unlock(2, &platinum) == 0 && platinum == 0);
    FlagArray flags{};
    CHECK(s.Count(flags) == 5 && (flags.bits[0] & 1u));
    UnlockNotice notice;
    int notices = 0;
    while (s.service.TakeNotice(notice)) ++notices;
    CHECK(notices == 5);
}

void TestRegionsAndMissingData(const fs::path& base) {
    const fs::path user = base / "region-user";
    WriteAll(user / "keys.json", KeyJson(kKey));
    const fs::path us = MakeGame(base, "region-us", BuildTrp(StandardEntries()));
    std::vector<Spec> fewer(kSpecs.begin(), kSpecs.begin() + 4);
    const fs::path jp = MakeGame(base, "region-jp", BuildTrp(StandardEntries(fewer)));
    {
        Session s(us, user);
        CHECK(s.Unlock(1) == 0);
    }
    {
        Session s(jp, user);
        FlagArray flags{};
        CHECK(s.Count(flags) == 4 && flags.bits[0] == 2);
        CHECK(s.Unlock(2) == 0);
    }
    {
        Session s(us, user);
        FlagArray flags{};
        CHECK(s.Count(flags) == 5 && flags.bits[0] == 6);
    }

    const fs::path other = MakeGame(base, "region-other", BuildTrp(StandardEntries()), "NPWR00002_00");
    {
        Session s(other, user);
        FlagArray flags{};
        CHECK(s.Count(flags) == 0 && flags.bits[0] == 0);
    }
    CHECK(fs::exists(TrophyDirectory(user, kNpId) / "progress_user1.txt"));

    fs::remove_all(us);
    const fs::path reinstalled = MakeGame(base, "region-us", BuildTrp(StandardEntries()));
    {
        Session s(reinstalled, user);
        FlagArray flags{};
        CHECK(s.Count(flags) == 5 && flags.bits[0] == 6);
    }

    const fs::path empty_user = base / "empty-user";
    {
        Session s(base / "no-game", empty_user);
        CHECK(s.Unlock(5) == 0);
        FlagArray flags{};
        CHECK(s.Count(flags) == 0 && flags.bits[0] == 32);
        CHECK(s.Unlock(500) == err::kInvalidTrophyId && s.Unlock(-1) == err::kInvalidTrophyId);
        CHECK(!fs::exists(empty_user / "trophy"));
    }

    const fs::path keyless_user = base / "keyless-user";
    {
        Session s(reinstalled, keyless_user);
        CHECK(s.Unlock(4) == 0);
        Guest g;
        CHECK(s.service.GetGameInfo(s.context, s.handle, g.game_details, g.game_data) == 0);
    }
    WriteAll(keyless_user / "keys.json", "{\"TrophyKeySet\":{\"ReleaseTrophyKey\":\"1234\"}}");
    {
        Session s(reinstalled, keyless_user);
        FlagArray flags{};
        CHECK(s.Count(flags) == 0 && flags.bits[0] == 16);
    }
    WriteAll(keyless_user / "keys.json", KeyJson(kKey));
    CHECK(PrepareTrophyPack(reinstalled, keyless_user).status == PackStatus::kReady);
    TrophyStore store(TrophyDirectory(keyless_user, kNpId), kNpId, 1);
    store.Load();
    CHECK(BuildTrophyView(*LoadTrophySet(keyless_user, kNpId), store).unlocked == 1);
}

std::string CaptureStdout(const std::function<void()>& run, const fs::path& file) {
    std::fflush(stdout);
    const int saved = dup(1);
    const int fd = open(file.string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(fd, 1);
    close(fd);
    run();
    std::fflush(stdout);
    dup2(saved, 1);
    close(saved);
    return ReadAll(file);
}

std::string Upper(std::string text) {
    for (char& c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

void TestKeyEntryAndImport(const fs::path& base) {
    const std::string hex = Hex(kKey.data(), kKey.size());
    CHECK(ParseKeyHex(hex) == kKey && ParseKeyHex(Upper(hex)) == kKey);
    CHECK(ParseKeyHex("0a0B0c0D0e0F00010203040506070809") == TrophyKey({0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
    CHECK(!ParseKeyHex("") && !ParseKeyHex(hex.substr(0, 31)) && !ParseKeyHex(hex + "0") && !ParseKeyHex(hex + hex));
    CHECK(!ParseKeyHex(" " + hex.substr(1)) && !ParseKeyHex(hex.substr(0, 31) + " ") && !ParseKeyHex(hex + "\n"));
    CHECK(!ParseKeyHex(hex.substr(0, 31) + "g") && !ParseKeyHex("0x" + hex.substr(2)));
    CHECK(!ParseKeyHex("01020304-05060708-090a0b0c-0d0e0f") && !ParseKeyHex("01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f"));

    const std::string json = "{\"TrophyKeySet\": {\"ReleaseTrophyKey\": \"" + hex + "\"}}";
    CHECK(KeyFromKeysJson(json) == kKey);
    CHECK(!KeyFromKeysJson("{}") && !KeyFromKeysJson("{\"TrophyKeySet\": {}}"));
    CHECK(!KeyFromKeysJson("{\"ReleaseTrophyKey\": \"" + hex + "\"}"));
    CHECK(!KeyFromKeysJson("{\"TrophyKeySet\": {\"ReleaseTrophyKey\": \"" + hex.substr(2) + "\"}}"));
    CHECK(!KeyFromKeysJson("{\"TrophyKeySet\": {\"ReleaseTrophyKey\": \"" + hex));
    CHECK(!KeyFromKeysJson("{\"TrophyKeySet\": {\"ReleaseTrophyKey\" \"" + hex + "\"}}"));
    CHECK(!KeyFromKeysJson("not json at all") && !KeyFromKeysJson(""));

    const std::string toml = "[General]\nName = \"x\"\n\n[Keys]\n# note\nTrophyKey = \"" + hex + "\"  # mine\n[Other]\nTrophyKey = \"zz\"\n";
    CHECK(KeyFromConfigToml(toml) == kKey);
    CHECK(KeyFromConfigToml("[Keys]\nTrophyKey='" + Upper(hex) + "'\n") == kKey);
    CHECK(KeyFromConfigToml("[Keys]\r\nTrophyKey = \"" + hex + "\"\r\n") == kKey);
    CHECK(!KeyFromConfigToml("[Keys]\nOther = 1\n") && !KeyFromConfigToml("[General]\nTrophyKey = \"" + hex + "\"\n"));
    CHECK(!KeyFromConfigToml("TrophyKey = \"" + hex + "\"\n") && !KeyFromConfigToml(""));
    CHECK(!KeyFromConfigToml("[Keys]\nTrophyKey = \"" + hex + "\n") && !KeyFromConfigToml("[Keys\nTrophyKey = \"" + hex + "\"\n"));
    CHECK(!KeyFromConfigToml("[Keys]\nTrophyKey = " + hex + "\n") && !KeyFromConfigToml("[Keys]\nTrophyKey = \"" + hex.substr(2) + "\"\n"));
    CHECK(!KeyFromConfigToml("[Keys]\nTrophyKey = \"" + hex + "\" junk\n"));

    WriteAll(base / "imp" / "keys.json", json + "\n");
    WriteAll(base / "imp" / "config.toml", toml);
    WriteAll(base / "imp" / "KEYS.TOML", toml);
    WriteAll(base / "imp" / "empty.json", "");
    CHECK(KeyFromFile(base / "imp" / "keys.json") == kKey && KeyFromFile(base / "imp" / "config.toml") == kKey);
    CHECK(KeyFromFile(base / "imp" / "KEYS.TOML") == kKey);
    CHECK(!KeyFromFile(base / "imp" / "empty.json") && !KeyFromFile(base / "imp" / "missing.json"));
}

void TestKeyStorage(const fs::path& base) {
    const TrophyKey other = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    const std::string other_hex = Hex(other.data(), other.size());

    const fs::path fresh = base / "ks-fresh";
    CHECK(StoreTrophyKey(fresh, kKey, KeySource::kEntered) && LoadTrophyKey(fresh) == kKey);

    const fs::path user = base / "ks-user";
    const std::string source_text = "{\"Unrelated\": \"keep-me\", \"TrophyKeySet\": {\"Note\": 1, \"ReleaseTrophyKey\": \"" +
                                    other_hex + "\", \"Tail\": [1, 2]}, \"Later\": true}";
    WriteAll(user / "keys.json", source_text);
    CHECK(LoadTrophyKey(user) == other);
    CHECK(KeyFromKeysJson(ReadAll(user / "keys.json")) == other);
    CHECK(StoreTrophyKey(user, kKey, KeySource::kEntered) && LoadTrophyKey(user) == kKey);
    const std::string stored = ReadAll(user / "keys.json");
    CHECK(stored.find("keep-me") != std::string::npos && stored.find("\"Later\": true") != std::string::npos &&
          stored.find("\"Tail\": [1, 2]") != std::string::npos && stored.find("\"Note\": 1") != std::string::npos);
    CHECK(stored.find(Upper(other_hex)) == std::string::npos);

    const fs::path no_member = base / "ks-nomember";
    WriteAll(no_member / "keys.json", "{\"TrophyKeySet\": {\"Other\": 5}, \"Z\": 1}");
    CHECK(StoreTrophyKey(no_member, kKey, KeySource::kEntered) && LoadTrophyKey(no_member) == kKey);
    CHECK(ReadAll(no_member / "keys.json").find("\"Other\": 5") != std::string::npos);
    const fs::path empty_set = base / "ks-emptyset";
    WriteAll(empty_set / "keys.json", "{\"TrophyKeySet\": {}}");
    CHECK(StoreTrophyKey(empty_set, kKey, KeySource::kEntered) && LoadTrophyKey(empty_set) == kKey);
    const fs::path no_set = base / "ks-noset";
    WriteAll(no_set / "keys.json", "{\"Z\": 1}");
    CHECK(StoreTrophyKey(no_set, kKey, KeySource::kEntered) && LoadTrophyKey(no_set) == kKey &&
          ReadAll(no_set / "keys.json").find("\"Z\": 1") != std::string::npos);
    const fs::path empty_obj = base / "ks-emptyobj";
    WriteAll(empty_obj / "keys.json", "{}");
    CHECK(StoreTrophyKey(empty_obj, kKey, KeySource::kEntered) && LoadTrophyKey(empty_obj) == kKey);

    const fs::path broken = base / "ks-broken";
    WriteAll(broken / "keys.json", "this is not json");
    CHECK(!StoreTrophyKey(broken, kKey, KeySource::kEntered) && ReadAll(broken / "keys.json") == "this is not json");

    const fs::path source = base / "ks-source";
    WriteAll(source / "keys.json", "{\"Secret2\": \"do-not-copy\", \"TrophyKeySet\": {\"ReleaseTrophyKey\": \"" +
                                       Hex(kKey.data(), kKey.size()) + "\", \"Extra\": \"do-not-copy-either\"}}");
    const fs::path target = base / "ks-target";
    const std::optional<TrophyKey> imported = KeyFromFile(source / "keys.json");
    CHECK(imported && StoreTrophyKey(target, *imported, KeySource::kImported) && LoadTrophyKey(target) == kKey);
    const std::string copied = ReadAll(target / "keys.json");
    CHECK(copied.find("do-not-copy") == std::string::npos && copied.find("Secret2") == std::string::npos);

    CHECK(KeyFromFile(source / "keys.json") == kKey && LoadTrophyKey(user) == kKey);

    CHECK(ClearTrophyKey(user) && !LoadTrophyKey(user));
    const std::string cleared = ReadAll(user / "keys.json");
    CHECK(cleared.find("keep-me") != std::string::npos && cleared.find("\"Later\": true") != std::string::npos &&
          cleared.find("\"ReleaseTrophyKey\": \"\"") != std::string::npos);
    CHECK(ClearTrophyKey(user) && ClearTrophyKey(base / "ks-never-existed"));
    CHECK(StoreTrophyKey(user, other, KeySource::kEntered) && LoadTrophyKey(user) == other);
}

void TestKeyManagementFlow(const fs::path& base) {
    const fs::path user = base / "flow-user";
    const fs::path app0 = MakeGame(base, "flow-game", BuildTrp(StandardEntries()));
    const std::string hex = Hex(kKey.data(), kKey.size());

    TrophyPageContent content = LoadTrophyPage(fs::path(), user);
    CHECK(content.state == TrophyPageState::kNoGame && !fs::exists(user / "keys.json"));

    content = LoadTrophyPage(app0, user);
    CHECK(content.state == TrophyPageState::kNoList && content.pack == PackStatus::kKeyMissing && !content.key_configured);

    {
        Session s(app0, user);
        CHECK(s.Unlock(2) == 0);
    }
    const std::string progress = ReadAll(TrophyDirectory(user, kNpId) / "progress_user1.txt");
    CHECK(!progress.empty());
    content = LoadTrophyPage(app0, user);
    CHECK(content.state == TrophyPageState::kNoList && content.saved_unlocks.size() == 1 && content.saved_unlocks[0].id == 2 &&
          content.saved_unlocks[0].unix_time > 0);

    std::string log = CaptureStdout(
        [&] {
            CHECK(StoreTrophyKey(user, kKey, KeySource::kEntered));
            content = LoadTrophyPage(app0, user);
        },
        base / "log1.txt");
    CHECK(content.state == TrophyPageState::kList && content.key_configured && content.view.total == 5);
    CHECK(content.view.unlocked == 1 && content.view.rows[2].unlocked);
    CHECK(log.find("Trophy Key saved") != std::string::npos);
    CHECK(log.find(hex) == std::string::npos && log.find(Upper(hex)) == std::string::npos);

    TrophyKey wrong = kKey;
    wrong[3] ^= 0x55;
    const std::string wrong_hex = Hex(wrong.data(), wrong.size());
    log = CaptureStdout(
        [&] {
            CHECK(StoreTrophyKey(user, wrong, KeySource::kImported));
            content = LoadTrophyPage(app0, user);
            Session s(app0, user);
            CHECK(s.Unlock(1) == 0);
        },
        base / "log2.txt");
    CHECK(content.state == TrophyPageState::kList && content.key_configured);
    CHECK(log.find("imported from shadPS4 configuration") != std::string::npos);
    CHECK(log.find(wrong_hex) == std::string::npos && log.find(Upper(wrong_hex)) == std::string::npos);

    const fs::path user2 = base / "flow-user2";
    CHECK(StoreTrophyKey(user2, wrong, KeySource::kEntered));
    content = LoadTrophyPage(app0, user2);
    CHECK(content.state == TrophyPageState::kNoList && content.pack == PackStatus::kKeyRejected && content.key_configured);
    CHECK(LoadTrophyKey(user2) == wrong);

    log = CaptureStdout([&] { CHECK(ClearTrophyKey(user2)); }, base / "log3.txt");
    CHECK(log.find("removed") != std::string::npos && log.find(wrong_hex) == std::string::npos);
    content = LoadTrophyPage(app0, user2);
    CHECK(content.state == TrophyPageState::kNoList && content.pack == PackStatus::kKeyMissing && !content.key_configured);

    CHECK(ClearTrophyKey(user));
    CHECK(ReadAll(TrophyDirectory(user, kNpId) / "progress_user1.txt").find("unlocked 2 ") != std::string::npos);
    CHECK(ReadAll(TrophyDirectory(user, kNpId) / "progress_user1.txt").find("unlocked 1 ") != std::string::npos);
    CHECK(fs::exists(TrophyDirectory(user, kNpId) / "Xml" / "TROPCONF.XML"));
    content = LoadTrophyPage(app0, user);
    CHECK(content.state == TrophyPageState::kList && !content.key_configured && content.view.unlocked == 2);
}

void TestLauncherPieces(const fs::path& base) {
    CHECK(std::size(kPageOrder) >= 3 && kPageOrder[std::size(kPageOrder) - 2] == PageId::kTrophies &&
          kPageOrder[std::size(kPageOrder) - 1] == PageId::kAbout && kPageOrder[std::size(kPageOrder) - 3] == PageId::kControls);
    CHECK(std::string(PageName(PageId::kTrophies)) == "Trophies");
    DcpPng::Image image;
    CHECK(!DcpPng::LoadRgba((base / "missing.png").string().c_str(), image));
    WriteAll(base / "fake.png", PngBytes());
    CHECK(!DcpPng::LoadRgba((base / "fake.png").string().c_str(), image));
}

void TestNoEmbeddedKey() {
    const std::regex sixteen_byte_literal("(0[xX][0-9A-Fa-f]{2}[ \t]*,[ \t\r\n]*){15}0[xX][0-9A-Fa-f]{2}");
    for (const fs::directory_entry& entry : fs::directory_iterator(DCP_TROPHY_SOURCE_DIR)) {
        std::ifstream in(entry.path(), std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        const std::string source = text.str();
        CHECK(!std::regex_search(source, sixteen_byte_literal));
    }
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path base = fs::temp_directory_path() / ("dcp_trophy_tests_" + std::to_string(getpid()));
    fs::remove_all(base);
    fs::create_directories(base);

    TestErrorCodes();
    TestParsing();
    TestStore(base);
    TestView();
    TestKeyAndCrypto(base);
    TestTrp(base);
    TestPack(base);
    TestService(base);
    TestPlatinum(base);
    TestRegionsAndMissingData(base);
    TestKeyEntryAndImport(base);
    TestKeyStorage(base);
    TestKeyManagementFlow(base);
    TestLauncherPieces(base);
    TestNoEmbeddedKey();

    std::error_code ec;
    fs::remove_all(base, ec);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
