#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace trg::update {

struct ZipLimits {
  std::uint64_t max_total_bytes = 1ull << 30;
  size_t max_entries = 4096;
  std::uint64_t max_archive_bytes = 512ull << 20;
};

bool IsSymlinkZipEntry(std::uint32_t external_attributes);

bool ExtractZip(const std::filesystem::path& archive, const std::filesystem::path& destination,
                const ZipLimits& limits, std::string& error);

bool ResolveStagingRoot(const std::filesystem::path& staging, const std::vector<std::string>& required_files,
                        std::filesystem::path& root, std::string& error);

bool ValidateStaging(const std::filesystem::path& staging, const std::vector<std::string>& required_files,
                     std::string& error);

std::vector<std::string> ListStagingFiles(const std::filesystem::path& staging, std::string& error);

}  // namespace trg::update
