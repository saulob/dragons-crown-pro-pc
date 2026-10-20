#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace trg::update {

class Sha256 {
 public:
  Sha256();
  void Update(const void* data, size_t size);
  std::array<std::uint8_t, 32> Final();

 private:
  void Transform(const std::uint8_t* block);

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  size_t buffered_ = 0;
  std::uint64_t total_ = 0;
};

std::string Sha256Hex(std::string_view data);
std::string Sha256FileHex(const std::filesystem::path& path, std::string& error);

bool IsValidSha256(std::string_view text);
std::string NormalizeSha256(std::string_view text);

}  // namespace trg::update
