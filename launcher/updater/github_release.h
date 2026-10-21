#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "updater/version.h"

namespace trg::update {

struct ReleaseAsset {
  std::string name;
  std::string download_url;
  std::string digest;
  std::uint64_t size = 0;

  std::optional<std::string> sha256() const;
};

struct Release {
  std::string tag;
  SemanticVersion version;
  bool draft = false;
  bool prerelease = false;
  std::string html_url;
  std::vector<ReleaseAsset> assets;
};

std::optional<Release> ParseRelease(std::string_view json, std::string* error = nullptr);
bool IsInstallableRelease(const Release& release);
const ReleaseAsset* FindAsset(const Release& release, std::string_view name);
std::optional<std::string> ParseSha256Digest(std::string_view digest);
std::optional<std::string> ParseSha256File(std::string_view content);

}  // namespace trg::update
