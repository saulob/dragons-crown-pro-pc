#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace trg::update {

// The release API, the release download pages and the hosts GitHub redirects asset downloads to.
inline std::vector<std::string> GitHubReleaseHosts() {
  return {"api.github.com", "github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"};
}

inline bool AsciiEqualsNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// An https:// address on the default port, without user info, whose host is one of `hosts`.
inline bool UrlHostAllowed(std::string_view url, const std::vector<std::string>& hosts) {
  constexpr std::string_view kScheme = "https://";
  if (url.size() <= kScheme.size() || !AsciiEqualsNoCase(url.substr(0, kScheme.size()), kScheme)) {
    return false;
  }
  std::string_view authority = url.substr(kScheme.size());
  authority = authority.substr(0, authority.find_first_of("/?#"));
  if (authority.find('@') != std::string_view::npos || authority.find('\\') != std::string_view::npos) {
    return false;
  }
  if (const size_t colon = authority.find(':'); colon != std::string_view::npos) {
    if (authority.substr(colon + 1) != "443") {
      return false;
    }
    authority = authority.substr(0, colon);
  }
  for (const std::string& host : hosts) {
    if (AsciiEqualsNoCase(authority, host)) {
      return true;
    }
  }
  return false;
}

// UrlHostAllowed, and the address starts with `prefix` when one is given.
inline bool UrlAllowed(std::string_view url, const std::vector<std::string>& hosts, std::string_view prefix) {
  return UrlHostAllowed(url, hosts) && (prefix.empty() || url.substr(0, prefix.size()) == prefix);
}

}  // namespace trg::update
