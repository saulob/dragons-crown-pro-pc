#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trg::update {

struct SemanticVersion {
  std::uint64_t major = 0;
  std::uint64_t minor = 0;
  std::uint64_t patch = 0;
  std::vector<std::string> prerelease;
  std::string build;

  bool stable() const { return prerelease.empty(); }
  std::string CoreString() const;
  std::string ToString() const;

  bool operator==(const SemanticVersion&) const = default;
  bool operator<(const SemanticVersion& other) const { return Compare(*this, other) < 0; }
  bool operator<=(const SemanticVersion& other) const { return Compare(*this, other) <= 0; }

  static int Compare(const SemanticVersion& left, const SemanticVersion& right);
};

std::optional<SemanticVersion> ParseVersion(std::string_view text);

}  // namespace trg::update
