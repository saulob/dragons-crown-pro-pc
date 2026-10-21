#include "updater/github_release.h"

#include <cctype>

#include "updater/json.h"
#include "updater/sha256.h"

namespace trg::update {
namespace {

std::string Trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

std::string Lower(std::string_view text) {
  std::string lower(text);
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return lower;
}

std::optional<std::string> StringField(const json::Value& object, const char* key, bool required, std::string* error) {
  const json::Value* value = object.Find(key);
  if (!value) {
    if (required && error) {
      *error = std::string("The release response is missing ") + key + ".";
    }
    return std::nullopt;
  }
  if (!value->IsString()) {
    if (required && error) {
      *error = std::string("The release response has an invalid ") + key + ".";
    }
    return std::nullopt;
  }
  return value->AsString();
}

bool BoolField(const json::Value& object, const char* key, bool fallback, std::string* error) {
  const json::Value* value = object.Find(key);
  if (!value) {
    return fallback;
  }
  if (!value->IsBool()) {
    if (error) {
      *error = std::string("The release response has an invalid ") + key + ".";
    }
    return fallback;
  }
  return value->AsBool();
}

}  // namespace

std::optional<std::string> ReleaseAsset::sha256() const {
  return ParseSha256Digest(digest);
}

std::optional<Release> ParseRelease(std::string_view text, std::string* error) {
  const std::optional<json::Value> root = json::Parse(text, error);
  if (!root || !root->IsObject()) {
    if (error && error->empty()) {
      *error = "The release response is not a JSON object.";
    }
    return std::nullopt;
  }

  const std::optional<std::string> tag = StringField(*root, "tag_name", true, error);
  if (!tag) {
    return std::nullopt;
  }
  const std::optional<SemanticVersion> version = ParseVersion(*tag);
  if (!version) {
    if (error) {
      *error = "The release tag is not a semantic version: " + *tag + ".";
    }
    return std::nullopt;
  }

  Release release;
  release.tag = *tag;
  release.version = *version;
  release.draft = BoolField(*root, "draft", false, error);
  release.prerelease = BoolField(*root, "prerelease", false, error);
  if (error && !error->empty()) {
    return std::nullopt;
  }
  if (const std::optional<std::string> html_url = StringField(*root, "html_url", false, error); html_url) {
    release.html_url = *html_url;
  }
  if (error && !error->empty()) {
    return std::nullopt;
  }

  const json::Value* assets = root->Find("assets");
  if (!assets) {
    return release;
  }
  if (!assets->IsArray()) {
    if (error) {
      *error = "The release response has an invalid assets list.";
    }
    return std::nullopt;
  }
  for (size_t i = 0; i < assets->AsArray().size(); ++i) {
    const json::Value* item = assets->At(i);
    if (!item || !item->IsObject()) {
      if (error) {
        *error = "The release response has an invalid asset.";
      }
      return std::nullopt;
    }
    const std::optional<std::string> name = StringField(*item, "name", true, error);
    const std::optional<std::string> url = StringField(*item, "browser_download_url", true, error);
    const std::optional<std::string> digest = StringField(*item, "digest", false, error);
    if (!name || !url || (error && !error->empty())) {
      return std::nullopt;
    }
    const json::Value* size = item->Find("size");
    if (!size || !size->IsNumber()) {
      if (error) {
        *error = "The release response has an asset without a size.";
      }
      return std::nullopt;
    }
    const std::optional<std::int64_t> size_value = size->AsInt64();
    if (!size_value || *size_value < 0) {
      if (error) {
        *error = "The release response has an asset with an invalid size.";
      }
      return std::nullopt;
    }
    ReleaseAsset asset;
    asset.name = *name;
    asset.download_url = *url;
    asset.size = static_cast<std::uint64_t>(*size_value);
    if (digest) {
      asset.digest = *digest;
    }
    release.assets.push_back(std::move(asset));
  }
  return release;
}

bool IsInstallableRelease(const Release& release) {
  return !release.draft && !release.prerelease && release.version.stable();
}

const ReleaseAsset* FindAsset(const Release& release, std::string_view name) {
  for (const ReleaseAsset& asset : release.assets) {
    if (asset.name == name) {
      return &asset;
    }
  }
  return nullptr;
}

std::optional<std::string> ParseSha256Digest(std::string_view digest) {
  const std::string trimmed = Trim(digest);
  if (trimmed.size() != 71 || Lower(trimmed.substr(0, 7)) != "sha256:") {
    return std::nullopt;
  }
  const std::string_view hash(trimmed.data() + 7, 64);
  if (!IsValidSha256(hash)) {
    return std::nullopt;
  }
  return NormalizeSha256(hash);
}

std::optional<std::string> ParseSha256File(std::string_view content) {
  const std::string trimmed = Trim(content);
  const std::string_view text(trimmed);
  const size_t end = text.find_first_of(" \t\r\n");
  const std::string_view hash = text.substr(0, end);
  if (!IsValidSha256(hash)) {
    return std::nullopt;
  }
  return NormalizeSha256(hash);
}

}  // namespace trg::update
