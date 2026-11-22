#include "save_backup.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#include "save_data.h"

namespace dcp {
namespace {

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    char buffer[24];
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &tm);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    char result[32];
    std::snprintf(result, sizeof(result), "%s-%03d", buffer, static_cast<int>(millis));
    return result;
}

bool ReadAllBytes(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return in.good() || in.eof();
}

bool WriteAllBytes(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    return out.good();
}

bool WriteText(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out << text;
    out.flush();
    return out.good();
}

std::string MetaText(const BackupInfo& info) {
    std::ostringstream text;
    text << "timestamp=" << info.timestamp << "\n"
         << "title_id=" << info.title_id << "\n"
         << "data_slot=" << info.dir_name << "\n"
         << "original_path=" << info.original_path << "\n";
    return text.str();
}

std::string ValueOf(const std::string& line) {
    const size_t at = line.find('=');
    return at == std::string::npos ? std::string() : line.substr(at + 1);
}

constexpr size_t kMaxNameField = 32;
constexpr uintmax_t kMaxMetaBytes = 4096;
constexpr int kMaxNameAttempts = 100;

// Title ids and savedata folder names become part of a file name, so only a plain token is kept.
std::string NameToken(std::string_view text) {
    std::string token;
    for (const char ch : text.substr(0, std::min(text.size(), kMaxNameField))) {
        const bool plain = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        token += plain ? ch : '_';
    }
    return token.empty() ? std::string("unknown") : token;
}

bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code first, second;
    const std::filesystem::path left = std::filesystem::weakly_canonical(a, first);
    const std::filesystem::path right = std::filesystem::weakly_canonical(b, second);
    return !first && !second && left == right;
}

bool PlainToken(const std::string& text) { return !text.empty() && text.size() <= kMaxNameField && NameToken(text) == text; }

} // namespace

std::optional<std::string> WriteSaveAtomically(const std::filesystem::path& target, std::span<const uint8_t> bytes,
                                               bool (*validate)(std::span<const uint8_t>, std::vector<std::string>&)) {
    std::error_code ec;
    const std::filesystem::path folder = target.parent_path();
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path temporary = target.string() + ".tmp";
    std::filesystem::remove(temporary, ec);

    std::vector<std::string> problems;
    if (validate && !validate(bytes, problems)) {
        return "write refused: " + (problems.empty() ? std::string("invalid save") : problems.front());
    }
    if (!WriteAllBytes(temporary, bytes)) {
        std::filesystem::remove(temporary, ec);
        return "could not write the temporary copy";
    }
    std::vector<uint8_t> reopened;
    if (!ReadAllBytes(temporary, reopened) || reopened != std::vector<uint8_t>(bytes.begin(), bytes.end())) {
        std::filesystem::remove(temporary, ec);
        return "temporary copy did not reopen identically";
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return "could not replace the save: " + ec.message();
    }
    std::vector<std::string> final_problems;
    if (validate) {
        std::vector<uint8_t> final_bytes;
        if (!ReadAllBytes(target, final_bytes) || !validate(final_bytes, final_problems)) {
            return "reopened save failed validation: " +
                   (final_problems.empty() ? std::string("unknown") : final_problems.front());
        }
    }
    return std::nullopt;
}

std::optional<BackupInfo> CreateBackup(const std::filesystem::path& backup_folder, const std::filesystem::path& save0_path,
                                       std::string_view title_id, std::string_view dir_name) {
    std::vector<uint8_t> bytes;
    if (!ReadAllBytes(save0_path, bytes)) {
        return std::nullopt;
    }
    std::error_code ec;
    std::filesystem::create_directories(backup_folder, ec);
    const std::string stamp = Timestamp();
    BackupInfo info;
    info.title_id = NameToken(title_id);
    info.dir_name = NameToken(dir_name);
    info.original_path = save0_path.string();
    const std::string base = "SAVE0_" + stamp + "_" + info.title_id + "_" + info.dir_name;
    for (int attempt = 0; attempt < kMaxNameAttempts; ++attempt) {
        const std::string suffix = attempt == 0 ? std::string() : "_" + std::to_string(attempt);
        info.timestamp = stamp + suffix;
        info.file = backup_folder / (base + suffix + ".DAT");
        info.meta = backup_folder / (base + suffix + ".meta");
        if (std::filesystem::exists(info.file, ec) || std::filesystem::exists(info.meta, ec)) {
            continue; // never overwrite
        }
        std::vector<uint8_t> written;
        if (!WriteAllBytes(info.file, bytes) || !ReadAllBytes(info.file, written) || written != bytes ||
            !WriteText(info.meta, MetaText(info))) {
            std::filesystem::remove(info.file, ec);
            std::filesystem::remove(info.meta, ec);
            return std::nullopt;
        }
        return info;
    }
    return std::nullopt;
}

std::vector<BackupInfo> ListBackups(const std::filesystem::path& backup_folder) {
    std::vector<BackupInfo> backups;
    std::error_code ec;
    if (!std::filesystem::is_directory(backup_folder, ec)) {
        return backups;
    }
    for (std::filesystem::directory_iterator it(backup_folder, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        if (it->path().extension() != ".meta" || !it->is_regular_file(ec)) {
            continue;
        }
        BackupInfo info;
        info.meta = it->path();
        info.file = it->path();
        info.file.replace_extension(".DAT");
        const uintmax_t meta_size = std::filesystem::file_size(info.meta, ec);
        if (ec || meta_size > kMaxMetaBytes || !std::filesystem::is_regular_file(info.file, ec)) {
            continue;
        }
        std::ifstream in(info.meta, std::ios::binary);
        std::string text(static_cast<size_t>(meta_size), '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<size_t>(std::max<std::streamsize>(in.gcount(), 0)));
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("title_id=", 0) == 0) {
                info.title_id = ValueOf(line);
            } else if (line.rfind("data_slot=", 0) == 0) {
                info.dir_name = ValueOf(line);
            } else if (line.rfind("original_path=", 0) == 0) {
                info.original_path = ValueOf(line);
            } else if (line.rfind("timestamp=", 0) == 0) {
                info.timestamp = ValueOf(line);
            }
        }
        if (!PlainToken(info.title_id)) info.title_id.clear();
        if (!PlainToken(info.dir_name)) info.dir_name.clear();
        if (info.timestamp.size() > kMaxNameField) info.timestamp.clear();
        backups.push_back(std::move(info));
    }
    std::sort(backups.begin(), backups.end(), [](const BackupInfo& a, const BackupInfo& b) {
        return a.timestamp > b.timestamp;
    });
    return backups;
}

std::optional<std::string> DeleteBackup(const BackupInfo& backup, const std::filesystem::path& backup_folder,
                                        std::string_view title_id, std::string_view dir_name) {
    const auto named_like_a_backup = [](const BackupInfo& info) {
        return info.file.extension() == ".DAT" && info.meta.extension() == ".meta" && info.file.stem() == info.meta.stem();
    };
    if (!named_like_a_backup(backup) || !SamePath(backup.file.parent_path(), backup_folder) ||
        !SamePath(backup.meta.parent_path(), backup_folder)) {
        return "this is not a backup in the backup folder";
    }
    for (const BackupInfo& listed : ListBackups(backup_folder)) {
        if (!SamePath(listed.file, backup.file) || !SamePath(listed.meta, backup.meta)) continue;
        if (listed.title_id != title_id || listed.dir_name != dir_name) return "this backup belongs to another save";
        std::error_code ec;
        if (std::filesystem::is_symlink(listed.file, ec) || std::filesystem::is_symlink(listed.meta, ec)) {
            return "this backup is a link, not a file";
        }
        if (!std::filesystem::remove(listed.file, ec) || ec) return "could not delete the backup file";
        std::filesystem::remove(listed.meta, ec);
        return ec ? std::optional<std::string>("the backup was deleted but its information file was not") : std::nullopt;
    }
    return "the backup no longer exists";
}

std::optional<std::string> RestoreBackup(const BackupInfo& backup, const std::filesystem::path& save0_path,
                                         const std::filesystem::path& backup_folder, std::string_view title_id,
                                         std::string_view dir_name) {
    std::vector<uint8_t> bytes;
    if (!ReadAllBytes(backup.file, bytes)) {
        return "could not read the backup";
    }
    std::vector<std::string> problems;
    if (!ValidateSave(bytes, problems)) {
        return "the backup is not a valid save: " + (problems.empty() ? std::string("unknown") : problems.front());
    }
    std::error_code ec;
    if (std::filesystem::exists(save0_path, ec)) {
        if (!CreateBackup(backup_folder, save0_path, title_id, dir_name)) {
            return "could not back up the current save before restoring";
        }
    }
    if (std::optional<std::string> error = WriteSaveAtomically(save0_path, bytes, ValidateSave)) {
        return error;
    }
    std::vector<uint8_t> written;
    if (!ReadAllBytes(save0_path, written) || written != bytes) {
        return "the restored save did not read back identically";
    }
    return std::nullopt;
}

} // namespace dcp
