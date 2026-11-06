#include "log_scrubber.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <optional>

#include "net/identity.h"
#include "net/json.h"
#include "utf8.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dcp {
namespace {

constexpr std::string_view kHome = "<home>";
constexpr std::string_view kInstall = "<install>";
constexpr std::string_view kFolder = "<folder>";
constexpr std::string_view kNetworkPath = "<network-path>";
constexpr std::string_view kUser = "<user>";
constexpr std::string_view kComputer = "<computer>";
constexpr std::string_view kSecret = "<secret>";
constexpr std::string_view kPlayer = "<player>";
constexpr std::string_view kRoom = "<room>";
constexpr std::string_view kIpv4 = "<ipv4>";
constexpr std::string_view kIpv6 = "<ipv6>";
constexpr std::string_view kChatLine = "Chat: <message removed>";

constexpr std::string_view kSecretWords[] = {
    "secret",     "password",    "passwd",      "passphrase", "pwd",         "token",  "api_key", "apikey",
    "api-key",    "access_key",  "private_key", "session_key", "room_key",   "cookie", "credential",
    "friend_code", "friend-code", "friend code",
};

// Default account and machine names: they say nothing about a person and are ordinary words in a log.
constexpr std::string_view kGenericNames[] = {"admin", "administrator", "deck",   "default", "game", "guest",     "localhost",
                                              "owner", "player",        "public", "root",    "steam", "steamdeck", "user"};

constexpr std::string_view kSystemFolders[] = {"windows", "program files", "program files (x86)", "programdata"};

// A dotted number straight after one of these is a version, not an address.
constexpr std::string_view kVersionWords[] = {"amd",   "api",  "build",  "driver", "intel", "mesa",
                                              "nvidia", "radeon", "sdk", "ver",    "version", "vulkan"};

constexpr std::string_view kProfileRoots[] = {"/var/home/", "/run/media/", "/home/", "/Users/", "/media/"};

char Folded(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool IsWord(char c) { return IsDigit(c) || IsLetter(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80; }
bool IsSeparator(char c) { return c == '\\' || c == '/'; }
bool IsBlank(char c) { return c == ' ' || c == '\t'; }

bool StartsFolded(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) { return Folded(a) == Folded(b); });
}

// Word boundaries of [at, at + size) in `text`.
bool StandsAlone(std::string_view text, size_t at, size_t size) {
    return (at == 0 || !IsWord(text[at - 1])) && (at + size >= text.size() || !IsWord(text[at + size]));
}

bool InsidePlaceholder(std::string_view text, size_t at, size_t size) {
    return at > 0 && text[at - 1] == '<' && at + size < text.size() && text[at + size] == '>';
}

std::string ReplaceWord(std::string_view line, std::string_view word, std::string_view with) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        if (StartsFolded(line.substr(i), word) && StandsAlone(line, i, word.size()) && !InsidePlaceholder(line, i, word.size())) {
            out += with;
            i += word.size();
        } else {
            out += line[i++];
        }
    }
    return out;
}

// The length of `folder` at the start of `text`, in any case and with either separator, doubled or not; 0 when it is
// not there. A longer last name (`Users\Ann2` for `Users\Ann`) does not count.
size_t FolderLength(std::string_view text, std::string_view folder) {
    size_t t = 0, f = 0;
    while (f < folder.size()) {
        if (t >= text.size()) return 0;
        if (IsSeparator(folder[f])) {
            if (!IsSeparator(text[t])) return 0;
            while (t < text.size() && IsSeparator(text[t])) ++t;
            while (f < folder.size() && IsSeparator(folder[f])) ++f;
        } else {
            if (Folded(text[t]) != Folded(folder[f])) return 0;
            ++t;
            ++f;
        }
    }
    return t < text.size() && IsWord(text[t]) ? 0 : t;
}

struct KnownFolder {
    std::string path;
    std::string_view token;
};

std::vector<KnownFolder> KnownFolders(const PrivateFacts& facts) {
    std::vector<KnownFolder> folders;
    const auto add = [&folders](const std::vector<std::string>& paths, std::string_view token) {
        for (std::string path : paths) {
            while (!path.empty() && IsSeparator(path.back())) path.pop_back();
            if (path.size() >= 3) folders.push_back({std::move(path), token});
        }
    };
    add(facts.profile_folders, kHome);
    add(facts.install_folders, kInstall);
    // The longest first: the install folder is often inside the profile, and each log path should name the nearest.
    std::stable_sort(folders.begin(), folders.end(),
                     [](const KnownFolder& a, const KnownFolder& b) { return a.path.size() > b.path.size(); });
    return folders;
}

std::string ReplaceKnownFolders(std::string_view line, const std::vector<KnownFolder>& folders) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        size_t matched = 0;
        std::string_view token;
        if (i == 0 || !IsWord(line[i - 1])) {
            for (const KnownFolder& folder : folders) {
                matched = FolderLength(line.substr(i), folder.path);
                if (matched) {
                    token = folder.token;
                    break;
                }
            }
        }
        if (matched) {
            out += token;
            i += matched;
        } else {
            out += line[i++];
        }
    }
    return out;
}

// Where a name in a path ends: the next separator, or something no folder name holds in a log.
size_t NameEnd(std::string_view line, size_t at) {
    size_t i = at;
    while (i < line.size() && !IsSeparator(line[i]) && line[i] != '"' && line[i] != ':' && line[i] != ',' && line[i] != ')' &&
           line[i] != '<' && line[i] != '>') {
        ++i;
    }
    return i;
}

size_t SkipSeparators(std::string_view line, size_t at) {
    while (at < line.size() && IsSeparator(line[at])) ++at;
    return at;
}

// The end of a Windows profile folder starting at `at`, or nullopt.
std::optional<size_t> WindowsProfileEnd(std::string_view line, size_t at) {
    if (at + 3 >= line.size() || !IsLetter(line[at]) || line[at + 1] != ':' || !IsSeparator(line[at + 2])) return std::nullopt;
    if (at > 0 && IsWord(line[at - 1])) return std::nullopt;
    const size_t users = SkipSeparators(line, at + 2);
    if (!StartsFolded(line.substr(users), "users") || users + 5 >= line.size() || !IsSeparator(line[users + 5])) return std::nullopt;
    return NameEnd(line, SkipSeparators(line, users + 5));
}

std::optional<size_t> PosixProfileEnd(std::string_view line, size_t at) {
    if (line[at] != '/' || (at > 0 && (IsWord(line[at - 1]) || line[at - 1] == '.'))) return std::nullopt;
    for (const std::string_view root : kProfileRoots) {
        if (line.substr(at).starts_with(root) && at + root.size() < line.size()) return NameEnd(line, at + root.size());
    }
    return std::nullopt;
}

// True after a replaced folder: JSON doubles a path's separators, so `<home>\\x.ini` is not a share.
bool FollowsReplacedFolder(std::string_view line, size_t at) {
    const std::string_view before = line.substr(0, at);
    return before.ends_with(kHome) || before.ends_with(kInstall);
}

// The end of `\\server\share` starting at `at`, plain or JSON-escaped, or nullopt. `\\?\C:\` is a local path.
std::optional<size_t> NetworkPathEnd(std::string_view line, size_t at) {
    if (!line.substr(at).starts_with("\\\\")) return std::nullopt;
    const size_t server = SkipSeparators(line, at);
    if (server >= line.size() || !IsWord(line[server])) return std::nullopt;
    if (at > 0 && (IsWord(line[at - 1]) || line[at - 1] == ':' || IsSeparator(line[at - 1]))) return std::nullopt;
    if (FollowsReplacedFolder(line, at)) return std::nullopt;
    const size_t host = NameEnd(line, server);
    return host < line.size() && IsSeparator(line[host]) ? NameEnd(line, SkipSeparators(line, host)) : host;
}

std::string ReplaceProfilePaths(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        if (const auto end = WindowsProfileEnd(line, i)) {
            out += kHome;
            i = *end;
        } else if (const auto end = PosixProfileEnd(line, i)) {
            out += kHome;
            i = *end;
        } else if (const auto end = NetworkPathEnd(line, i)) {
            out += kNetworkPath;
            i = *end;
        } else {
            out += line[i++];
        }
    }
    return out;
}

bool IsSystemFolder(std::string_view path_after_drive) {
    return std::any_of(std::begin(kSystemFolders), std::end(kSystemFolders), [&](std::string_view folder) {
        if (!StartsFolded(path_after_drive, folder)) return false;
        return path_after_drive.size() == folder.size() || IsSeparator(path_after_drive[folder.size()]);
    });
}

// Paths in a log run to the end of the line, a quote, or the words after them.
size_t PathEnd(std::string_view line, size_t at) {
    constexpr std::string_view kStops = "\"<>|*?";
    for (size_t i = at; i < line.size(); ++i) {
        if (kStops.find(line[i]) != std::string_view::npos) return i;
        const bool closes = line[i] == ':' || line[i] == ',' || line[i] == ';';
        if (closes && i + 1 < line.size() && line[i + 1] == ' ') return i;
        if (line[i] == ' ' && i + 1 < line.size() && line[i + 1] == '(') return i;
    }
    return line.size();
}

// A folder outside the system's can be named after anyone, and only the file's own name helps a diagnosis.
std::string ReplaceLocalFolders(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        const bool drive = i + 2 < line.size() && IsLetter(line[i]) && line[i + 1] == ':' && IsSeparator(line[i + 2]) &&
                           (i == 0 || !IsWord(line[i - 1]));
        if (!drive) {
            out += line[i++];
            continue;
        }
        const size_t start = SkipSeparators(line, i + 2), end = PathEnd(line, start);
        if (IsSystemFolder(line.substr(start, end - start))) {
            out += line.substr(i, end - i);
        } else {
            size_t last = end;
            while (last > start && IsSeparator(line[last - 1])) --last;
            const std::string_view path = line.substr(start, last - start);
            const size_t separator = path.find_last_of("\\/");
            out += kFolder;
            if (separator != std::string_view::npos) {
                out += path.substr(separator);
            } else {
                out += line[i + 2];
                out += path;
            }
        }
        i = end;
    }
    return out;
}

bool EndsSecret(char c) {
    return IsBlank(c) || c == '"' || c == '\'' || c == ',' || c == ';' || c == '&' || c == ')' || c == '}' || c == ']';
}

// After a secret's name: `name = value`, `"name":"value"` and the like. The value's start, or nullopt.
std::optional<size_t> ValueAfterName(std::string_view line, size_t at) {
    while (at < line.size() && (IsWord(line[at]) || line[at] == '-')) ++at;
    if (at < line.size() && (line[at] == '"' || line[at] == '\'')) ++at;
    while (at < line.size() && IsBlank(line[at])) ++at;
    if (at >= line.size() || (line[at] != '=' && line[at] != ':')) return std::nullopt;
    ++at;
    while (at < line.size() && IsBlank(line[at])) ++at;
    if (at < line.size() && (line[at] == '"' || line[at] == '\'')) ++at;
    return at < line.size() && !EndsSecret(line[at]) ? std::optional<size_t>(at) : std::nullopt;
}

size_t SecretWordSize(std::string_view text) {
    for (const std::string_view word : kSecretWords) {
        if (StartsFolded(text, word)) return word.size();
    }
    return 0;
}

size_t WebTokenLength(std::string_view text) {
    if (!text.starts_with("eyJ")) return 0;
    size_t i = 0, dots = 0;
    for (; i < text.size() && (IsWord(text[i]) || text[i] == '-' || text[i] == '.'); ++i) dots += text[i] == '.';
    return dots >= 2 && i >= 24 ? i : 0;
}

std::string MaskLabelledSecrets(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        const std::string_view rest = line.substr(i);
        const bool at_word = i == 0 || !IsWord(line[i - 1]);
        if (at_word && StartsFolded(rest, "authorization")) {
            if (const auto value = ValueAfterName(line, i)) {
                out += line.substr(i, *value - i);
                out += kSecret;
                return out;
            }
        }
        if (at_word && StartsFolded(rest, "bearer ")) {
            size_t end = i + 7;
            while (end < line.size() && !EndsSecret(line[end])) ++end;
            out += line.substr(i, 7);
            out += kSecret;
            i = end;
            continue;
        }
        if (at_word) {
            if (const size_t size = WebTokenLength(rest)) {
                out += kSecret;
                i += size;
                continue;
            }
        }
        const size_t word = at_word ? SecretWordSize(rest) : 0;
        if (word) {
            if (const auto value = ValueAfterName(line, i + word)) {
                size_t end = *value;
                while (end < line.size() && !EndsSecret(line[end])) ++end;
                out += line.substr(i, *value - i);
                out += kSecret;
                i = end;
                continue;
            }
        }
        out += line[i++];
    }
    return out;
}

// `peer 1234567`: an account id long enough not to be an ordinary count.
std::string MaskAccounts(std::string_view line, std::string_view label) {
    constexpr size_t kShortestAccount = 6;
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        if (line.substr(i).starts_with(label) && (i == 0 || !IsWord(line[i - 1]))) {
            size_t end = i + label.size();
            while (end < line.size() && IsDigit(line[end])) ++end;
            if (end - i - label.size() >= kShortestAccount) {
                out += label;
                out += kPlayer;
                i = end;
                continue;
            }
        }
        out += line[i++];
    }
    return out;
}

bool IsRoomCode(std::string_view text) {
    constexpr std::string_view kAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    constexpr size_t kLength = 6;
    if (text.size() < kLength || (text.size() > kLength && IsWord(text[kLength]))) return false;
    const std::string_view code = text.substr(0, kLength);
    return std::all_of(code.begin(), code.end(), [&](char c) { return kAlphabet.find(c) != std::string_view::npos; }) &&
           std::any_of(code.begin(), code.end(), IsLetter);
}

std::string MaskRoomCodes(std::string_view line) {
    constexpr std::string_view kLabel = "code ";
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        if (line.substr(i).starts_with(kLabel) && (i == 0 || !IsWord(line[i - 1])) && IsRoomCode(line.substr(i + kLabel.size()))) {
            out += kLabel;
            out += kRoom;
            i += kLabel.size() + 6;
        } else {
            out += line[i++];
        }
    }
    return out;
}

// The size of the dotted address at the start of `text`, or 0.
size_t Ipv4Size(std::string_view text) {
    size_t i = 0;
    for (int octet = 0; octet < 4; ++octet) {
        size_t digits = 0;
        int value = 0;
        while (i < text.size() && IsDigit(text[i]) && digits < 4) {
            value = value * 10 + (text[i] - '0');
            ++i;
            ++digits;
        }
        if (digits == 0 || digits > 3 || value > 255) return 0;
        if (octet < 3) {
            if (i >= text.size() || text[i] != '.') return 0;
            ++i;
        }
    }
    if (i < text.size() && IsWord(text[i])) return 0;
    if (i + 1 < text.size() && text[i] == '.' && IsDigit(text[i + 1])) return 0;
    return i;
}

bool FollowsVersionWord(std::string_view line, size_t at) {
    while (at > 0 && (IsBlank(line[at - 1]) || line[at - 1] == '(')) --at;
    const size_t end = at;
    while (at > 0 && IsLetter(line[at - 1])) --at;
    std::string word(line.substr(at, end - at));
    std::transform(word.begin(), word.end(), word.begin(), Folded);
    return std::find(std::begin(kVersionWords), std::end(kVersionWords), word) != std::end(kVersionWords);
}

bool IsLocalIpv4(std::string_view address) {
    return address.starts_with("0.") || address.starts_with("127.");
}

// Whether `text`, made only of hex digits, colons and dots, is an IPv6 address other than the unspecified or loopback one.
bool IsIpv6(std::string_view text) {
    if (text == "::" || text == "::1") return false;
    const size_t gap = text.find("::");
    if (gap != std::string_view::npos && text.find("::", gap + 1) != std::string_view::npos) return false;
    if (std::none_of(text.begin(), text.end(), IsDigit)) return false;
    int groups = 0;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ':') {
            ++i;
            continue;
        }
        size_t end = i;
        while (end < text.size() && text[end] != ':') ++end;
        const std::string_view group = text.substr(i, end - i);
        const bool last = end == text.size();
        if (group.find('.') != std::string_view::npos) {
            if (!last || Ipv4Size(group) != group.size()) return false;
            groups += 2;
        } else {
            if (group.size() > 4 || !std::all_of(group.begin(), group.end(), IsHex)) return false;
            ++groups;
        }
        i = end;
    }
    return gap == std::string_view::npos ? groups == 8 : groups <= 7;
}

// The size of the IPv6 address at the start of `text`, with its zone, or 0.
size_t Ipv6Size(std::string_view text) {
    size_t end = 0;
    while (end < text.size() && (IsHex(text[end]) || text[end] == ':' || text[end] == '.')) ++end;
    while (end > 0 && (text[end - 1] == '.' || (text[end - 1] == ':' && !(end >= 2 && text[end - 2] == ':')))) --end;
    if (end < 3 || !IsIpv6(text.substr(0, end))) return 0;
    if (end < text.size() && text[end] == '%') {
        size_t zone = end + 1;
        while (zone < text.size() && (IsWord(text[zone]) || text[zone] == '.' || text[zone] == '-')) ++zone;
        return zone > end + 1 ? zone : 0;
    }
    return end < text.size() && IsWord(text[end]) ? 0 : end;
}

std::string MaskAddresses(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size();) {
        const char previous = i > 0 ? line[i - 1] : ' ';
        const bool clear_start = !IsWord(previous) && previous != '.' && (IsHex(line[i]) || line[i] == ':');
        if (clear_start && previous != ':') {
            if (const size_t size = Ipv6Size(line.substr(i))) {
                out += kIpv6;
                i += size;
                continue;
            }
        }
        if (clear_start) {
            if (const size_t size = Ipv4Size(line.substr(i))) {
                if (IsLocalIpv4(line.substr(i)) || FollowsVersionWord(line, i)) {
                    out += line.substr(i, size);
                } else {
                    out += kIpv4;
                }
                i += size;
                continue;
            }
        }
        out += line[i++];
    }
    return out;
}

bool WorthMatching(std::string_view name) {
    constexpr size_t kShortestName = 2;
    if (name.size() < kShortestName) return false;
    return std::none_of(std::begin(kGenericNames), std::end(kGenericNames), [&](std::string_view generic) {
        return generic.size() == name.size() && StartsFolded(name, generic);
    });
}

size_t Utf8SequenceSize(std::string_view text) {
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(0);
    if (lead < 0x80) return 1;
    const size_t size = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC2 ? 2 : 0;
    if (size == 0 || lead > 0xF4 || text.size() < size) return 0;
    for (size_t i = 1; i < size; ++i) {
        if ((byte(i) & 0xC0) != 0x80) return 0;
    }
    if ((lead == 0xE0 && byte(1) < 0xA0) || (lead == 0xED && byte(1) > 0x9F) || (lead == 0xF0 && byte(1) < 0x90) ||
        (lead == 0xF4 && byte(1) > 0x8F)) {
        return 0;
    }
    return size;
}

std::string CleanCharacters(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const size_t size = Utf8SequenceSize(text.substr(i));
        const char c = text[i];
        if (size == 0) {
            out += '?';
            ++i;
        } else if (size == 1) {
            const bool control = (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) && c != '\n' && c != '\t';
            if (!control) out += c;
            ++i;
        } else {
            const bool c1_control = static_cast<unsigned char>(c) == 0xC2 && static_cast<unsigned char>(text[i + 1]) < 0xA0;
            if (!c1_control) out += text.substr(i, size);
            i += size;
        }
    }
    return out;
}

struct KnownName {
    std::string_view name;
    std::string_view token;
};

// The longest first: an account named like the start of the computer's name must not split it.
std::vector<KnownName> KnownNames(const PrivateFacts& facts) {
    std::vector<KnownName> names;
    const auto add = [&names](const std::vector<std::string>& list, std::string_view token) {
        for (const std::string& name : list) {
            if (WorthMatching(name)) names.push_back({name, token});
        }
    };
    add(facts.user_names, kUser);
    add(facts.machine_names, kComputer);
    std::stable_sort(names.begin(), names.end(), [](const KnownName& a, const KnownName& b) { return a.name.size() > b.name.size(); });
    return names;
}

std::string ScrubLine(std::string_view raw, const PrivateFacts& facts, const std::vector<KnownFolder>& folders,
                      const std::vector<KnownName>& names) {
    if (raw.starts_with("Chat:")) return std::string(kChatLine);
    std::string line(raw);
    for (const std::string& secret : facts.secrets) {
        if (!secret.empty()) line = ReplaceWord(line, secret, kSecret);
    }
    line = ReplaceKnownFolders(line, folders);
    line = ReplaceProfilePaths(line);
    line = ReplaceLocalFolders(line);
    line = MaskLabelledSecrets(line);
    line = MaskAccounts(line, "peer ");
    line = MaskAccounts(line, "account ");
    line = MaskRoomCodes(line);
    line = MaskAddresses(line);
    for (const KnownName& known : names) line = ReplaceWord(line, known.name, known.token);
    return line;
}

void AddOnce(std::vector<std::string>& list, std::string value) {
    if (!value.empty() && std::find(list.begin(), list.end(), value) == list.end()) list.push_back(std::move(value));
}

#ifdef _WIN32
std::wstring EnvironmentValue(const wchar_t* name) {
    wchar_t value[1024];
    const DWORD size = GetEnvironmentVariableW(name, value, DWORD(std::size(value)));
    return size > 0 && size < std::size(value) ? std::wstring(value, size) : std::wstring();
}

std::string InCodePage(const std::wstring& text, UINT page) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(page, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(std::max(size, 0)), '\0');
    if (size > 0) WideCharToMultiByte(page, 0, text.data(), int(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

void AddSpellings(std::vector<std::string>& list, const std::wstring& value) {
    AddOnce(list, InCodePage(value, CP_UTF8));
    AddOnce(list, InCodePage(value, CP_ACP));
}

void AddSystemFacts(PrivateFacts& facts) {
    AddSpellings(facts.user_names, EnvironmentValue(L"USERNAME"));
    AddSpellings(facts.machine_names, EnvironmentValue(L"COMPUTERNAME"));
    AddSpellings(facts.machine_names, EnvironmentValue(L"USERDOMAIN"));
    AddSpellings(facts.profile_folders, EnvironmentValue(L"USERPROFILE"));
}
#else
std::string EnvironmentValue(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

void AddSystemFacts(PrivateFacts& facts) {
    AddOnce(facts.user_names, EnvironmentValue("USER"));
    AddOnce(facts.user_names, EnvironmentValue("LOGNAME"));
    AddOnce(facts.profile_folders, EnvironmentValue("HOME"));
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) == 0) AddOnce(facts.machine_names, host);
}
#endif

} // namespace

PrivateFacts GatherPrivateFacts(const std::vector<std::filesystem::path>& install_folders, const std::filesystem::path& user_dir) {
    PrivateFacts facts;
    AddSystemFacts(facts);
    for (const std::filesystem::path& folder : install_folders) AddOnce(facts.install_folders, Utf8(folder));
    const net::Identity identity = net::load_identity(user_dir, false);
    if (identity.ok()) {
        AddOnce(facts.secrets, identity.secret);
        AddOnce(facts.secrets, net::u64_text(identity.account));
    }
    return facts;
}

std::string ScrubText(std::string_view text, const PrivateFacts& facts) {
    const std::vector<KnownFolder> folders = KnownFolders(facts);
    const std::vector<KnownName> names = KnownNames(facts);
    std::string out;
    out.reserve(text.size());
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out += ScrubLine(line, facts, folders, names);
        if (end == text.size()) break;
        out += '\n';
        start = end + 1;
    }
    return CleanCharacters(out);
}

} // namespace dcp
