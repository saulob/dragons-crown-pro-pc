#include "trophies/trophy_key.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>

namespace fs = std::filesystem;

namespace dcp::trophies {
namespace {

constexpr size_t kKeyHexDigits = 2 * std::tuple_size_v<TrophyKey>;
constexpr uintmax_t kMaxKeyFileBytes = 1u << 20;
constexpr std::string_view kSetName = "\"TrophyKeySet\"";
constexpr std::string_view kKeyName = "\"ReleaseTrophyKey\"";

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string HexOf(const TrophyKey& key) {
    std::string hex;
    char pair[3];
    for (const uint8_t byte : key) {
        std::snprintf(pair, sizeof(pair), "%02X", byte);
        hex += pair;
    }
    return hex;
}

size_t SkipSpace(std::string_view text, size_t at) {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    return at;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

bool QuotedAt(std::string_view text, size_t at, char quote, std::string_view& contents, size_t& end) {
    if (at >= text.size() || text[at] != quote) {
        return false;
    }
    const size_t close = text.find(quote, at + 1);
    if (close == std::string_view::npos) {
        return false;
    }
    contents = text.substr(at + 1, close - at - 1);
    end = close + 1;
    return true;
}

size_t FindKeyName(std::string_view text) {
    const size_t set_at = text.find(kSetName);
    return set_at == std::string_view::npos ? std::string_view::npos : text.find(kKeyName, set_at);
}

std::optional<size_t> JsonValueQuote(std::string_view text, size_t name_at) {
    size_t at = SkipSpace(text, name_at + kKeyName.size());
    if (at >= text.size() || text[at] != ':') {
        return std::nullopt;
    }
    at = SkipSpace(text, at + 1);
    return at < text.size() && text[at] == '"' ? std::optional<size_t>(at) : std::nullopt;
}

bool IsJsonObject(std::string_view text) {
    const std::string_view body = Trim(text);
    return body.size() > 1 && body.front() == '{' && body.back() == '}';
}

std::string ReadText(const fs::path& path) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxKeyFileBytes) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool WriteAtomically(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) {
            out.close();
            fs::remove(temp, ec);
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        fs::remove(temp, ec);
        return false;
    }
    return true;
}

bool ObjectIsEmptyAfter(const std::string& text, size_t open) {
    const size_t next = SkipSpace(text, open + 1);
    return next < text.size() && text[next] == '}';
}

std::optional<std::string> WithKeyValue(std::string text, const std::string& hex) {
    if (Trim(text).empty()) {
        return "{\n  \"TrophyKeySet\": {\n    \"ReleaseTrophyKey\": \"" + hex + "\"\n  }\n}\n";
    }
    if (!IsJsonObject(text)) {
        return std::nullopt;
    }
    const size_t key_at = FindKeyName(text);
    if (key_at != std::string::npos) {
        const std::optional<size_t> open = JsonValueQuote(text, key_at);
        const size_t close = open ? text.find('"', *open + 1) : std::string::npos;
        if (close == std::string::npos) {
            return std::nullopt;
        }
        text.replace(*open + 1, close - *open - 1, hex);
        return text;
    }
    const std::string member = "\"ReleaseTrophyKey\": \"" + hex + "\"";
    const size_t set_at = text.find(kSetName);
    if (set_at != std::string::npos) {
        const size_t colon = SkipSpace(text, set_at + kSetName.size());
        const size_t open = colon < text.size() && text[colon] == ':' ? SkipSpace(text, colon + 1) : text.size();
        if (open >= text.size() || text[open] != '{') {
            return std::nullopt;
        }
        text.insert(open + 1, member + (ObjectIsEmptyAfter(text, open) ? "" : ", "));
        return text;
    }
    const size_t root = text.find('{');
    text.insert(root + 1, "\"TrophyKeySet\": {" + member + "}" + (ObjectIsEmptyAfter(text, root) ? "" : ", "));
    return text;
}

bool IsKeysHeader(std::string_view line) {
    if (line.substr(0, 6) != "[Keys]") {
        return false;
    }
    const std::string_view rest = Trim(line.substr(6));
    return rest.empty() || rest[0] == '#';
}

} // namespace

std::optional<TrophyKey> ParseKeyHex(std::string_view text) {
    if (text.size() != kKeyHexDigits) {
        return std::nullopt;
    }
    TrophyKey key{};
    for (size_t i = 0; i < key.size(); ++i) {
        const int high = HexValue(text[i * 2]), low = HexValue(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        key[i] = static_cast<uint8_t>(high << 4 | low);
    }
    return key;
}

std::optional<TrophyKey> KeyFromKeysJson(std::string_view text) {
    const size_t key_at = FindKeyName(text);
    if (key_at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<size_t> open = JsonValueQuote(text, key_at);
    std::string_view value;
    size_t end = 0;
    if (!open || !QuotedAt(text, *open, '"', value, end)) {
        return std::nullopt;
    }
    return ParseKeyHex(value);
}

std::optional<TrophyKey> KeyFromConfigToml(std::string_view text) {
    constexpr std::string_view kName = "TrophyKey";
    bool in_keys = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view line = Trim(text.substr(at, eol - at));
        at = eol + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line[0] == '[') {
            in_keys = IsKeysHeader(line);
            continue;
        }
        if (!in_keys || line.substr(0, kName.size()) != kName) {
            continue;
        }
        size_t value_at = SkipSpace(line, kName.size());
        if (value_at >= line.size() || line[value_at] != '=') {
            continue;
        }
        value_at = SkipSpace(line, value_at + 1);
        const char quote = value_at < line.size() ? line[value_at] : '\0';
        std::string_view value;
        size_t end = 0;
        if ((quote != '"' && quote != '\'') || !QuotedAt(line, value_at, quote, value, end)) {
            return std::nullopt;
        }
        const std::string_view rest = Trim(line.substr(end));
        if (!rest.empty() && rest[0] != '#') {
            return std::nullopt;
        }
        return ParseKeyHex(value);
    }
    return std::nullopt;
}

std::optional<TrophyKey> KeyFromFile(const fs::path& file) {
    const std::string text = ReadText(file);
    if (text.empty()) {
        return std::nullopt;
    }
    std::string extension = file.extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension == ".toml" ? KeyFromConfigToml(text) : KeyFromKeysJson(text);
}

bool StoreTrophyKey(const fs::path& user_dir, const TrophyKey& key, KeySource source) {
    const fs::path file = user_dir / "keys.json";
    std::error_code ec;
    const std::string existing = fs::exists(file, ec) ? ReadText(file) : std::string();
    const std::optional<std::string> updated = WithKeyValue(existing, HexOf(key));
    if (!updated || !WriteAtomically(file, *updated)) {
        return false;
    }
    std::printf("Trophies: Trophy Key %s\n", source == KeySource::kImported ? "imported from shadPS4 configuration" : "saved");
    return true;
}

bool ClearTrophyKey(const fs::path& user_dir) {
    const fs::path file = user_dir / "keys.json";
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        return true;
    }
    std::string text = ReadText(file);
    const size_t key_at = FindKeyName(text);
    if (key_at == std::string::npos) {
        return true;
    }
    const std::optional<size_t> open = JsonValueQuote(text, key_at);
    const size_t close = open ? text.find('"', *open + 1) : std::string::npos;
    if (close == std::string::npos) {
        return false;
    }
    text.erase(*open + 1, close - *open - 1);
    if (!WriteAtomically(file, text)) {
        return false;
    }
    std::printf("Trophies: Trophy Key removed\n");
    return true;
}

} // namespace dcp::trophies
