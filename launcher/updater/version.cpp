#include "updater/version.h"

#include <algorithm>
#include <charconv>
#include <cstddef>

namespace trg::update {
namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && IsSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && IsSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

bool IsNumeric(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool IsIdentifierChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

std::optional<std::uint64_t> ParseNumber(std::string_view text) {
  if (!IsNumeric(text) || (text.size() > 1 && text.front() == '0')) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

bool ParseIdentifiers(std::string_view text, std::vector<std::string>& identifiers, bool prerelease) {
  if (text.empty()) {
    return false;
  }
  while (true) {
    const size_t dot = text.find('.');
    const std::string_view identifier = dot == std::string_view::npos ? text : text.substr(0, dot);
    if (identifier.empty() ||
        !std::all_of(identifier.begin(), identifier.end(), [](char c) { return IsIdentifierChar(c); })) {
      return false;
    }
    if (prerelease && IsNumeric(identifier) && identifier.size() > 1 && identifier.front() == '0') {
      return false;
    }
    identifiers.emplace_back(identifier);
    if (dot == std::string_view::npos) {
      return true;
    }
    text.remove_prefix(dot + 1);
  }
}

int ComparePrerelease(const SemanticVersion& left, const SemanticVersion& right) {
  if (left.prerelease.empty() || right.prerelease.empty()) {
    if (left.prerelease.empty() == right.prerelease.empty()) {
      return 0;
    }
    return left.prerelease.empty() ? 1 : -1;
  }
  const size_t count = std::min(left.prerelease.size(), right.prerelease.size());
  for (size_t i = 0; i < count; ++i) {
    const std::string& a = left.prerelease[i];
    const std::string& b = right.prerelease[i];
    const bool a_numeric = IsNumeric(a);
    const bool b_numeric = IsNumeric(b);
    if (a_numeric && b_numeric) {
      if (a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
      }
      if (a != b) {
        return a < b ? -1 : 1;
      }
    } else if (a_numeric != b_numeric) {
      return a_numeric ? -1 : 1;
    } else if (a != b) {
      return a < b ? -1 : 1;
    }
  }
  if (left.prerelease.size() == right.prerelease.size()) {
    return 0;
  }
  return left.prerelease.size() < right.prerelease.size() ? -1 : 1;
}

}  // namespace

std::string SemanticVersion::CoreString() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::string SemanticVersion::ToString() const {
  std::string text = CoreString();
  if (!prerelease.empty()) {
    text += '-';
    for (size_t i = 0; i < prerelease.size(); ++i) {
      if (i != 0) {
        text += '.';
      }
      text += prerelease[i];
    }
  }
  if (!build.empty()) {
    text += '+';
    text += build;
  }
  return text;
}

int SemanticVersion::Compare(const SemanticVersion& left, const SemanticVersion& right) {
  if (left.major != right.major) {
    return left.major < right.major ? -1 : 1;
  }
  if (left.minor != right.minor) {
    return left.minor < right.minor ? -1 : 1;
  }
  if (left.patch != right.patch) {
    return left.patch < right.patch ? -1 : 1;
  }
  return ComparePrerelease(left, right);
}

std::optional<SemanticVersion> ParseVersion(std::string_view text) {
  text = Trim(text);
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  SemanticVersion version;
  const size_t prerelease_start = text.find('-');
  const size_t build_start = text.find('+');
  size_t core_end = text.size();
  if (prerelease_start != std::string_view::npos && (build_start == std::string_view::npos || prerelease_start < build_start)) {
    core_end = prerelease_start;
  } else if (build_start != std::string_view::npos) {
    core_end = build_start;
  }
  const std::string_view core = text.substr(0, core_end);
  size_t part_start = 0;
  std::uint64_t* parts[] = {&version.major, &version.minor, &version.patch};
  size_t part_index = 0;
  while (part_start <= core.size()) {
    if (part_index >= 3) {
      return std::nullopt;
    }
    const size_t dot = core.find('.', part_start);
    const std::string_view part =
        dot == std::string_view::npos ? core.substr(part_start) : core.substr(part_start, dot - part_start);
    const std::optional<std::uint64_t> number = ParseNumber(part);
    if (!number) {
      return std::nullopt;
    }
    *parts[part_index++] = *number;
    if (dot == std::string_view::npos) {
      break;
    }
    part_start = dot + 1;
  }
  if (part_index != 3) {
    return std::nullopt;
  }

  if (prerelease_start != std::string_view::npos &&
      (build_start == std::string_view::npos || prerelease_start < build_start)) {
    const size_t prerelease_end = build_start == std::string_view::npos ? text.size() : build_start;
    if (!ParseIdentifiers(text.substr(prerelease_start + 1, prerelease_end - prerelease_start - 1),
                          version.prerelease, true)) {
      return std::nullopt;
    }
  }
  if (build_start != std::string_view::npos) {
    std::vector<std::string> build;
    if (!ParseIdentifiers(text.substr(build_start + 1), build, false)) {
      return std::nullopt;
    }
    for (const std::string& identifier : build) {
      if (!version.build.empty()) {
        version.build += '.';
      }
      version.build += identifier;
    }
  }
  return version;
}

}  // namespace trg::update
