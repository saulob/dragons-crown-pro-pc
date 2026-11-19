#include "save_backup.h"

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
    std::string base = "SAVE0_" + stamp + "_" + std::string(title_id) + "_" + std::string(dir_name);
    BackupInfo info;
    info.title_id = std::string(title_id);
    info.dir_name = std::string(dir_name);
    info.original_path = save0_path.string();
    info.timestamp = stamp;
    info.file = backup_folder / (base + ".DAT");
    info.meta = backup_folder / (base + ".meta");
    if (std::filesystem::exists(info.file, ec) || std::filesystem::exists(info.meta, ec)) {
        return std::nullopt; // never overwrite
    }
    if (!WriteAllBytes(info.file, bytes) || !WriteText(info.meta, MetaText(info))) {
        return std::nullopt;
    }
    return info;
}

std::vector<BackupInfo> ListBackups(const std::filesystem::path& backup_folder) {
    std::vector<BackupInfo> backups;
    std::error_code ec;
    if (!std::filesystem::is_directory(backup_folder, ec)) {
        return backups;
    }
    for (std::filesystem::directory_iterator it(backup_folder, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        if (it->path().extension() != ".meta") {
            continue;
        }
        BackupInfo info;
        info.meta = it->path();
        info.file = it->path();
        info.file.replace_extension(".DAT");
        std::ifstream in(info.meta);
        std::string line;
        while (std::getline(in, line)) {
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
        backups.push_back(std::move(info));
    }
    std::sort(backups.begin(), backups.end(), [](const BackupInfo& a, const BackupInfo& b) {
        return a.timestamp > b.timestamp;
    });
    return backups;
}

std::optional<std::string> RestoreBackup(const BackupInfo& backup, const std::filesystem::path& save0_path,
                                         const std::filesystem::path& backup_folder) {
    std::vector<uint8_t> bytes;
    if (!ReadAllBytes(backup.file, bytes)) {
        return "could not read the backup";
    }
    std::vector<std::string> problems;
    if (!ValidateSave(bytes, problems)) {
        return "the backup is not a valid save: " + (problems.empty() ? std::string("unknown") : problems.front());
    }
    if (std::filesystem::exists(save0_path)) {
        if (!CreateBackup(backup_folder, save0_path, backup.title_id, backup.dir_name)) {
            return "could not back up the current save before restoring";
        }
    }
    return WriteSaveAtomically(save0_path, bytes, ValidateSave);
}

} // namespace dcp
