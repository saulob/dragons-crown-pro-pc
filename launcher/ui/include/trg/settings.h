// Where launcher settings live. The launcher only talks to a SettingsStore, so
// each game keeps its own format: a plain `key = value` text file (the SMS
// port), engine cvars (Outpost Kaloki X), or anything else behind callbacks.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace trg {

class SettingsStore {
 public:
  virtual ~SettingsStore() = default;

  // The stored value, or `def` when the key is missing or empty.
  virtual std::string Get(std::string_view key, std::string_view def = {}) const = 0;
  virtual void Set(std::string_view key, std::string value) = 0;
  // Writes the settings out. False when they could not be written.
  virtual bool Save() = 0;
  // Puts every setting back to its default ("Reset all settings").
  virtual void ResetToDefaults() {}

  bool GetBool(std::string_view key, bool def) const;
  int GetInt(std::string_view key, int def) const;
  float GetFloat(std::string_view key, float def) const;
  void SetBool(std::string_view key, bool v) { Set(key, v ? true_value : false_value); }
  void SetInt(std::string_view key, int v) { Set(key, std::to_string(v)); }
  void SetFloat(std::string_view key, float v, int decimals = 2);

  // What Toggle and SetBool write. GetBool also accepts 1/yes/true/on.
  std::string true_value = "on";
  std::string false_value = "off";
};

// `name = value` lines. Comments, blank lines and order are kept on save; a
// setting that was missing replaces its commented example (`# name = value`)
// when there is one, else it is appended under "# Set from the launcher".
class TextFileSettings : public SettingsStore {
 public:
  explicit TextFileSettings(std::string path) : path_(std::move(path)) { Load(); }

  void Load();
  std::string Get(std::string_view key, std::string_view def = {}) const override;
  void Set(std::string_view key, std::string value) override;
  bool Save() override;
  void ResetToDefaults() override;

  const std::string& path() const { return path_; }
  bool Has(std::string_view key) const { return values_.count(std::string(key)) != 0; }

 private:
  std::string path_;
  std::vector<std::string> lines_;
  std::map<std::string, std::string, std::less<>> values_;
  std::vector<std::string> cleared_;  // keys reset to defaults: commented out on save
};

// Settings kept somewhere else, reached through functions (engine cvars, a
// registry, a JSON file the game already owns...).
class CallbackSettings : public SettingsStore {
 public:
  std::function<std::string(std::string_view key)> get;  // "" when unset
  std::function<void(std::string_view key, const std::string& value)> set;
  std::function<bool()> save;
  std::function<void()> reset;

  std::string Get(std::string_view key, std::string_view def = {}) const override;
  void Set(std::string_view key, std::string value) override;
  bool Save() override { return save ? save() : true; }
  void ResetToDefaults() override {
    if (reset) reset();
  }
};

// In memory only: for tests and the demo.
class MemorySettings : public SettingsStore {
 public:
  std::string Get(std::string_view key, std::string_view def = {}) const override;
  void Set(std::string_view key, std::string value) override { values_[std::string(key)] = std::move(value); }
  bool Save() override { return true; }
  void ResetToDefaults() override { values_.clear(); }

 private:
  std::map<std::string, std::string, std::less<>> values_;
};

// Parses one `name = value` line (with `allow_comment`, also `# name = value`).
// A trailing ` # comment` is not part of the value.
bool ParseSettingLine(std::string_view line, bool allow_comment, std::string& key, std::string& value);

}  // namespace trg
