#include "updater/update_paths.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace trg::update {
namespace {

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return lower;
}

bool IsReservedDeviceName(std::string_view segment) {
  const size_t dot = segment.find('.');
  const std::string stem = Lower(dot == std::string_view::npos ? segment : segment.substr(0, dot));
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
    return true;
  }
  if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9') {
    return true;
  }
  return false;
}

bool SameComponent(const std::filesystem::path& left, const std::filesystem::path& right) {
#if defined(_WIN32)
  std::wstring a = left.native();
  std::wstring b = right.native();
  std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
  std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
  return a == b;
#else
  return left == right;
#endif
}

bool IsReparsePoint([[maybe_unused]] const std::filesystem::path& path) {
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

}  // namespace

std::string Utf8Path(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

std::filesystem::path PathFromUtf8(std::string_view text) {
  const std::u8string value(reinterpret_cast<const char8_t*>(text.data()), text.size());
  return std::filesystem::path(value);
}

bool NormalizeRelativePath(std::string_view input, std::string& normalized, std::string* reason) {
  normalized.clear();
  if (input.empty()) {
    if (reason) {
      *reason = "empty path";
    }
    return false;
  }
  if (input.front() == '/' || input.front() == '\\') {
    if (reason) {
      *reason = "absolute path";
    }
    return false;
  }
  if (input.size() >= 2 && input[1] == ':') {
    if (reason) {
      *reason = "drive-qualified path";
    }
    return false;
  }

  std::string path(input);
  std::replace(path.begin(), path.end(), '\\', '/');
  while (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  if (path.empty()) {
    if (reason) {
      *reason = "empty path";
    }
    return false;
  }
  if (path.size() > 1024) {
    if (reason) {
      *reason = "path too long";
    }
    return false;
  }

  size_t start = 0;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    const std::string_view segment =
        slash == std::string::npos ? std::string_view(path).substr(start) : std::string_view(path).substr(start, slash - start);
    if (segment.empty()) {
      if (reason) {
        *reason = "empty path segment";
      }
      return false;
    }
    if (segment == ".") {
      if (slash == std::string::npos) {
        if (reason) {
          *reason = "empty path segment";
        }
        return false;
      }
      start = slash + 1;
      continue;
    }
    if (segment == "..") {
      if (reason) {
        *reason = "path traversal";
      }
      return false;
    }
    if (segment.size() > 255) {
      if (reason) {
        *reason = "path segment too long";
      }
      return false;
    }
    for (const char c : segment) {
      const unsigned char byte = static_cast<unsigned char>(c);
      if (byte < 0x20 || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
        if (reason) {
          *reason = "invalid character in path";
        }
        return false;
      }
    }
    if (segment.back() == '.' || segment.back() == ' ') {
      if (reason) {
        *reason = "invalid path segment ending";
      }
      return false;
    }
    if (IsReservedDeviceName(segment)) {
      if (reason) {
        *reason = "reserved path name";
      }
      return false;
    }
    if (!normalized.empty()) {
      normalized += '/';
    }
    normalized += segment;
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return !normalized.empty();
}

bool IsSafeRelativePath(std::string_view path, std::string* reason) {
  std::string normalized;
  return NormalizeRelativePath(path, normalized, reason);
}

bool IsProtectedRelativePath(std::string_view normalized_path) {
  static constexpr const char* kProtectedNames[] = {
      "game", "user", "user_data", "user_data.migrating", "logs", "cache", "content", "savedata", "trophy",
      "trophies", "screenshot", "screenshots", "sce_sys", "sce_module", "prepared", "extracted", "image", "dlc",
      "cusa10454", "cusa07001", "cusa10209"};
  static constexpr const char* kProtectedFiles[] = {
      "dcp.ini", "keys.json", "last_run.log", "last_run.prev.log", "eboot.bin", "param.sfo", "boot.bin",
      "content.bin", "image.json", "imports.json", "param.bin", "memory.dat", "trophy_key.bin"};
  static constexpr const char* kProtectedExtensions[] = {
      ".pkg", ".sfo", ".self", ".sprx", ".prx", ".elf", ".cpk", ".psarc", ".plt", ".trp", ".sav", ".dat",
      ".log", ".dmp", ".pdb", ".ilk", ".obj", ".bin", ".ini", ".json", ".key", ".meta", ".spv", ".part",
      ".bak", ".tmp", ".zip", ".7z", ".rar", ".tar", ".gz", ".appimage"};

  const std::string path = Lower(normalized_path);
  size_t start = 0;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    const std::string segment =
        slash == std::string::npos ? path.substr(start) : path.substr(start, slash - start);
    for (const char* name : kProtectedNames) {
      if (segment == name) {
        return true;
      }
    }
    for (const char* name : kProtectedFiles) {
      if (segment == name) {
        return true;
      }
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }

  const size_t dot = path.rfind('.');
  const size_t slash = path.rfind('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    const std::string_view extension(path.data() + dot);
    for (const char* protected_extension : kProtectedExtensions) {
      if (extension == protected_extension) {
        return true;
      }
    }
  }
  return false;
}

bool PathIsWithin(const std::filesystem::path& path, const std::filesystem::path& root) {
  std::error_code ec;
  const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, ec);
  if (ec || canonical_root.empty()) {
    return false;
  }
  const std::filesystem::path canonical_path = std::filesystem::weakly_canonical(path, ec);
  if (ec || canonical_path.empty()) {
    return false;
  }
  auto root_part = canonical_root.begin();
  auto path_part = canonical_path.begin();
  for (; root_part != canonical_root.end(); ++root_part, ++path_part) {
    if (path_part == canonical_path.end() || !SameComponent(*path_part, *root_part)) {
      return false;
    }
  }
  return true;
}

bool IsRegularFile(const std::filesystem::path& path) {
  std::error_code ec;
  const std::filesystem::file_status status = std::filesystem::symlink_status(path, ec);
  if (ec || status.type() != std::filesystem::file_type::regular) {
    return false;
  }
  return !IsReparsePoint(path);
}

bool HasReparseAncestor(const std::filesystem::path& path, const std::filesystem::path& root) {
  std::error_code ec;
  const std::filesystem::path relative = path.lexically_relative(root);
  if (relative.empty() || *relative.begin() == "..") {
    return true;
  }
  std::filesystem::path current = root;
  const std::filesystem::path parents = relative.parent_path();
  for (const std::filesystem::path& component : parents) {
    current /= component;
    const std::filesystem::file_status status = std::filesystem::symlink_status(current, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
      continue;
    }
    if (ec) {
      return true;
    }
    if (!std::filesystem::exists(status)) {
      continue;
    }
    if (std::filesystem::is_symlink(status) || IsReparsePoint(current)) {
      return true;
    }
  }
  return IsReparsePoint(root);
}

}  // namespace trg::update
