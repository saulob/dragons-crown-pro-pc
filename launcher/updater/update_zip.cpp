#include "updater/update_zip.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "updater/third_party/miniz/miniz.h"
#include "updater/update_paths.h"

namespace trg::update {
namespace {

class ZipArchive {
 public:
  ZipArchive() = default;
  ~ZipArchive() {
    if (open_) {
      mz_zip_reader_end(&archive_);
    }
  }
  ZipArchive(const ZipArchive&) = delete;
  ZipArchive& operator=(const ZipArchive&) = delete;

  bool Open(const std::vector<std::uint8_t>& data) {
    open_ = mz_zip_reader_init_mem(&archive_, data.data(), data.size(), 0) != MZ_FALSE;
    return open_;
  }

  mz_zip_archive& get() { return archive_; }

 private:
  mz_zip_archive archive_{};
  bool open_ = false;
};

std::string DuplicateKey(std::string path) {
#if defined(_WIN32)
  std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
  return path;
}

bool ReadArchive(const std::filesystem::path& archive, const ZipLimits& limits, std::vector<std::uint8_t>& data,
                 std::string& error) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(archive, ec);
  if (ec) {
    error = "Cannot read the downloaded update.";
    return false;
  }
  if (size > limits.max_archive_bytes) {
    error = "The downloaded update is too large.";
    return false;
  }
  std::ifstream file(archive, std::ios::binary);
  if (!file) {
    error = "Cannot read the downloaded update.";
    return false;
  }
  data.resize(size_t(size));
  if (!data.empty()) {
    file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
  }
  if (!file || size_t(file.gcount()) != data.size()) {
    error = "Cannot read the downloaded update.";
    return false;
  }
  return true;
}

}  // namespace

bool IsSymlinkZipEntry(std::uint32_t external_attributes) {
  const std::uint32_t mode = external_attributes >> 16;
  return (mode & 0170000u) == 0120000u;
}

bool ExtractZip(const std::filesystem::path& archive, const std::filesystem::path& destination,
                const ZipLimits& limits, std::string& error) {
  error.clear();
  std::error_code ec;
  std::filesystem::create_directories(destination, ec);
  if (ec || !std::filesystem::is_directory(destination, ec)) {
    error = "Cannot create the update staging folder.";
    return false;
  }

  std::vector<std::uint8_t> bytes;
  if (!ReadArchive(archive, limits, bytes, error)) {
    return false;
  }
  ZipArchive zip;
  if (!zip.Open(bytes)) {
    error = "The downloaded update is not a valid ZIP archive.";
    return false;
  }

  const mz_uint entries = mz_zip_reader_get_num_files(&zip.get());
  if (entries > limits.max_entries) {
    error = "The update archive holds too many files.";
    return false;
  }
  std::set<std::string> names;
  std::uint64_t total = 0;
  for (mz_uint index = 0; index < entries; ++index) {
    mz_zip_archive_file_stat stat{};
    if (mz_zip_reader_file_stat(&zip.get(), index, &stat) == MZ_FALSE) {
      error = "The update archive is damaged.";
      return false;
    }
    const mz_uint name_size = mz_zip_reader_get_filename(&zip.get(), index, nullptr, 0);
    if (name_size <= 1) {
      error = "The update archive holds an entry without a name.";
      return false;
    }
    std::vector<char> name(name_size);
    mz_zip_reader_get_filename(&zip.get(), index, name.data(), name_size);
    const std::string raw_name(name.data(), name_size - 1);
    const bool directory = stat.m_is_directory || (!raw_name.empty() && raw_name.back() == '/');
    std::string normalized;
    std::string reason;
    if (!NormalizeRelativePath(raw_name, normalized, &reason)) {
      error = "The update archive contains an unsafe path (" + reason + ").";
      return false;
    }
    if (IsProtectedRelativePath(normalized)) {
      error = "The update archive contains a protected path: " + normalized + ".";
      return false;
    }
    if (!names.insert(DuplicateKey(normalized)).second) {
      error = "The update archive contains the same path twice: " + normalized + ".";
      return false;
    }
    if (directory) {
      continue;
    }
    if (stat.m_is_encrypted || !stat.m_is_supported) {
      error = "The update archive uses an unsupported entry.";
      return false;
    }
    if (IsSymlinkZipEntry(stat.m_external_attr)) {
      error = "The update archive contains a symbolic link: " + normalized + ".";
      return false;
    }
    if (stat.m_uncomp_size > limits.max_total_bytes || total > limits.max_total_bytes - stat.m_uncomp_size) {
      error = "The update archive is too large when unpacked.";
      return false;
    }
    total += stat.m_uncomp_size;

    std::vector<std::uint8_t> data(size_t(std::max<mz_uint64>(stat.m_uncomp_size, 1)));
    if (mz_zip_reader_extract_to_mem(&zip.get(), index, data.data(), size_t(stat.m_uncomp_size), 0) == MZ_FALSE) {
      error = "The update archive is damaged.";
      return false;
    }

    const std::filesystem::path target = destination / PathFromUtf8(normalized);
    if (!PathIsWithin(target, destination)) {
      error = "The update archive contains a path outside the staging folder.";
      return false;
    }
    const std::filesystem::path parent = target.parent_path();
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      error = "Cannot create the update staging folder.";
      return false;
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "Cannot write the update file " + normalized + ".";
      return false;
    }
    if (stat.m_uncomp_size > 0) {
      out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(stat.m_uncomp_size));
    }
    out.close();
    if (!out) {
      error = "Cannot write the update file " + normalized + ".";
      return false;
    }
  }
  return true;
}

bool ResolveStagingRoot(const std::filesystem::path& staging, const std::vector<std::string>& required_files,
                        std::filesystem::path& root, std::string& error) {
  error.clear();
  const auto has_required = [&](const std::filesystem::path& base) {
    for (const std::string& required : required_files) {
      std::string normalized;
      if (!NormalizeRelativePath(required, normalized) ||
          !IsRegularFile(base / PathFromUtf8(normalized))) {
        return false;
      }
    }
    return true;
  };

  if (required_files.empty() || has_required(staging)) {
    root = staging;
    return true;
  }

  std::error_code ec;
  std::vector<std::filesystem::path> directories;
  size_t files = 0;
  for (std::filesystem::directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec)) {
    const std::filesystem::file_status status = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (std::filesystem::is_directory(status)) {
      directories.push_back(it->path());
    } else {
      ++files;
    }
  }
  if (ec || files != 0 || directories.size() != 1) {
    error = "The update does not contain " + required_files.front() + ".";
    return false;
  }
  if (!has_required(directories.front())) {
    error = "The update does not contain " + required_files.front() + ".";
    return false;
  }
  root = directories.front();
  return true;
}

bool ValidateStaging(const std::filesystem::path& staging, const std::vector<std::string>& required_files,
                     std::string& error) {
  error.clear();
  std::error_code ec;
  if (!std::filesystem::is_directory(staging, ec)) {
    error = "The update staging folder is missing.";
    return false;
  }
  size_t count = 0;
  std::uint64_t total = 0;
  for (std::filesystem::recursive_directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec)) {
    const std::filesystem::file_status status = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (std::filesystem::is_symlink(status)) {
      error = "The update staging folder contains a symbolic link.";
      return false;
    }
    if (std::filesystem::is_directory(status)) {
      continue;
    }
    if (!std::filesystem::is_regular_file(status)) {
      error = "The update staging folder contains an unsupported file.";
      return false;
    }
    const std::filesystem::path relative = it->path().lexically_relative(staging);
    const std::string path = Utf8Path(relative);
    std::string reason;
    if (!IsSafeRelativePath(path, &reason)) {
      error = "The update staging folder contains an unsafe path (" + reason + ").";
      return false;
    }
    if (IsProtectedRelativePath(path)) {
      error = "The update staging folder contains a protected path: " + path + ".";
      return false;
    }
    const std::uintmax_t size = it->file_size(ec);
    if (ec) {
      break;
    }
    constexpr std::uint64_t kMaxStagedBytes = 1ull << 30;
    if (size > kMaxStagedBytes || total > kMaxStagedBytes - size) {
      error = "The update staging folder is too large.";
      return false;
    }
    total += size;
    ++count;
  }
  if (ec) {
    error = "Cannot read the update staging folder.";
    return false;
  }
  if (count == 0) {
    error = "The update archive is empty.";
    return false;
  }
  for (const std::string& required : required_files) {
    std::string normalized;
    if (!NormalizeRelativePath(required, normalized)) {
      error = "The updater requires an invalid file: " + required + ".";
      return false;
    }
    const std::filesystem::path target = staging / PathFromUtf8(normalized);
    if (!PathIsWithin(target, staging) || !IsRegularFile(target)) {
      error = "The update is missing " + normalized + ".";
      return false;
    }
  }
  return true;
}

std::vector<std::string> ListStagingFiles(const std::filesystem::path& staging, std::string& error) {
  error.clear();
  std::vector<std::string> files;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(staging, ec), end; !ec && it != end; it.increment(ec)) {
    const std::filesystem::file_status status = it->symlink_status(ec);
    if (ec) {
      break;
    }
    if (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)) {
      continue;
    }
    const std::string path = Utf8Path(it->path().lexically_relative(staging));
    if (!IsSafeRelativePath(path) || IsProtectedRelativePath(path)) {
      error = "The update staging folder contains an unsafe path.";
      return {};
    }
    files.push_back(path);
  }
  if (ec) {
    error = "Cannot read the update staging folder.";
    return {};
  }
  std::sort(files.begin(), files.end());
  return files;
}

}  // namespace trg::update
