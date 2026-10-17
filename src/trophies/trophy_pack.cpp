#include "trophies/trophy_pack.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <tuple>
#include <vector>

#include "trophies/trophy_key.h"
#include "trophies/trophy_store.h"

namespace fs = std::filesystem;

namespace dcp::trophies {
namespace {

constexpr char kStampFile[] = "pack.stamp";
constexpr uintmax_t kMaxSmallFile = 1u << 20;

std::string ReadSmallFile(const fs::path& path) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxSmallFile) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string Lower(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::optional<fs::path> FindInsensitive(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
        if (Lower(entry.path().filename().string()) == name) {
            return entry.path();
        }
    }
    return std::nullopt;
}

uint64_t KeyFingerprint(const std::optional<TrophyKey>& key) {
    uint64_t hash = 1469598103934665603ull;
    if (key) {
        for (const uint8_t byte : *key) {
            hash = (hash ^ byte) * 1099511628211ull;
        }
    }
    return hash;
}

std::string StampText(uintmax_t size, const std::optional<TrophyKey>& key, const char* result) {
    char text[128];
    std::snprintf(text, sizeof(text), "dcp-trophy-pack 1\nsize %llu\nkey %s%016llx\nresult %s\n",
                  static_cast<unsigned long long>(size), key ? "" : "none-", static_cast<unsigned long long>(KeyFingerprint(key)),
                  result);
    return text;
}

struct StampedResult {
    const char* text;
    PackStatus status;
};
constexpr StampedResult kStampedResults[] = {
    {"ready", PackStatus::kReady}, {"key-missing", PackStatus::kKeyMissing}, {"key-rejected", PackStatus::kKeyRejected}};

PackStatus StatusOfStamp(const std::string& stamp, uintmax_t size, const std::optional<TrophyKey>& key) {
    for (const StampedResult& known : kStampedResults) {
        if (stamp == StampText(size, key, known.text)) {
            return known.status;
        }
    }
    return PackStatus::kDamaged;
}

const char* StampOfStatus(PackStatus status) {
    for (const StampedResult& known : kStampedResults) {
        if (known.status == status) {
            return known.text;
        }
    }
    return nullptr;
}

bool WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.flush();
    return static_cast<bool>(out);
}

bool SwapIn(const fs::path& from, const fs::path& dir, const char* name) {
    std::error_code ec;
    fs::remove_all(dir / name, ec);
    fs::rename(from / name, dir / name, ec);
    return !ec;
}

} // namespace

const char* DescribePack(PackStatus status) {
    switch (status) {
    case PackStatus::kReady: return "ready";
    case PackStatus::kNoNpBind: return "no NP Communication ID (sce_sys/npbind.dat)";
    case PackStatus::kNoPack: return "no trophy pack (sce_sys/trophy/trophy00.trp)";
    case PackStatus::kKeyMissing: return "trophy key missing or invalid (user/keys.json)";
    case PackStatus::kKeyRejected: return "the Trophy Key could not decrypt this game's trophy metadata";
    case PackStatus::kDamaged: return "trophy pack damaged or not writable";
    }
    return "";
}

std::optional<std::string> ReadNpCommId(const fs::path& npbind) {
    const std::string data = ReadSmallFile(npbind);
    constexpr size_t kRecord = 0x80;
    if (data.size() < kRecord + 16 || data.compare(0, 4, "\xD2\x94\xA0\x18") != 0) {
        return std::nullopt;
    }
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(data[i]); };
    if (byte(kRecord) != 0x00 || byte(kRecord + 1) != 0x10 || byte(kRecord + 2) != 0x00 || byte(kRecord + 3) != 0x0C) {
        return std::nullopt;
    }
    std::string id = data.substr(kRecord + 4, 12);
    return IsNpCommId(id) ? std::optional<std::string>(id) : std::nullopt;
}

std::optional<TrophyKey> LoadTrophyKey(const fs::path& user_dir) {
    return KeyFromKeysJson(ReadSmallFile(user_dir / "keys.json"));
}

PackResult PrepareTrophyPack(const fs::path& app0, const fs::path& user_dir) {
    PackResult result;
    const std::optional<std::string> id = ReadNpCommId(app0 / "sce_sys" / "npbind.dat");
    if (!id) {
        return result;
    }
    result.np_comm_id = *id;
    result.status = PackStatus::kNoPack;
    const std::optional<fs::path> trophy_folder = FindInsensitive(app0 / "sce_sys", "trophy");
    const std::optional<fs::path> trp = trophy_folder ? FindInsensitive(*trophy_folder, "trophy00.trp") : std::nullopt;
    if (!trp) {
        return result;
    }
    std::error_code ec;
    const uintmax_t size = fs::file_size(*trp, ec);
    const std::optional<TrophyKey> key = LoadTrophyKey(user_dir);
    const fs::path dir = TrophyDirectory(user_dir, *id);
    const std::string known = ReadSmallFile(dir / kStampFile);
    if (!ec && !known.empty()) {
        const PackStatus status = StatusOfStamp(known, size, key);
        if (status != PackStatus::kDamaged && (status != PackStatus::kReady || LoadTrophySet(user_dir, *id))) {
            result.status = status;
            return result;
        }
    }

    result.status = PackStatus::kDamaged;
    const fs::path temp = dir / ".extract";
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);
    const TrpResult extracted = ExtractTrp(*trp, *id, key, temp);
    switch (extracted.status) {
    case TrpStatus::kOk:
        if (SwapIn(temp, dir, "Xml") && SwapIn(temp, dir, "Icons")) {
            result.status = PackStatus::kReady;
        }
        break;
    case TrpStatus::kKeyMissing:
        if (SwapIn(temp, dir, "Icons")) {
            result.status = PackStatus::kKeyMissing;
        }
        break;
    case TrpStatus::kKeyRejected:
        result.status = PackStatus::kKeyRejected;
        break;
    default:
        break;
    }
    fs::remove_all(temp, ec);
    if (const char* stamped = StampOfStatus(result.status)) {
        WriteText(dir / kStampFile, StampText(size, key, stamped));
    }
    return result;
}

std::optional<TrophySet> LoadTrophySet(const fs::path& user_dir, const std::string& np_comm_id) {
    if (!IsNpCommId(np_comm_id)) {
        return std::nullopt;
    }
    const fs::path xml_dir = TrophyDirectory(user_dir, np_comm_id) / "Xml";
    std::optional<TrophySet> set = ParseTrophyConf(ReadSmallFile(xml_dir / "TROPCONF.XML"));
    if (!set) {
        return std::nullopt;
    }
    set->np_comm_id = np_comm_id;
    std::vector<std::string> names;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(xml_dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("TROP", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".XML") == 0 &&
            name != "TROPCONF.XML") {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    // English (US, then UK), the pack's default list, then the rest.
    for (const char* preferred : {"TROP.XML", "TROP_18.XML", "TROP_01.XML"}) {
        const auto it = std::find(names.begin(), names.end(), preferred);
        if (it != names.end()) {
            std::rotate(names.begin(), it, it + 1);
        }
    }
    for (const std::string& name : names) {
        if (const std::optional<TrophyTexts> texts = ParseTrophyTexts(ReadSmallFile(xml_dir / name))) {
            ApplyTrophyTexts(*set, *texts);
            break;
        }
    }
    return set;
}

fs::path TrophyIconPath(const fs::path& user_dir, const std::string& np_comm_id, int id) {
    return TrophyDirectory(user_dir, np_comm_id) / "Icons" / TrophyIconName(id);
}

} // namespace dcp::trophies
