// Dragon's Crown Pro SAVE0.DAT: structure, validation and safe in-memory editing.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dcp {

inline constexpr size_t kSaveFileSize = 0x20EB18;      // 2,157,336 bytes
inline constexpr uint32_t kSaveMagic = 0x44534344;     // "DCSD" at 0x04
inline constexpr size_t kSaveCrcOffset = 0x00;         // u32 little-endian, over [0x04, EOF)
inline constexpr size_t kSaveMagicOffset = 0x04;

inline constexpr size_t kDataSlotCount = 3;
inline constexpr size_t kDataSlotStride = 0xAF8D4;
inline constexpr size_t kDataSlotGoldOffset = 0xAF288; // u32 little-endian
inline constexpr size_t kCharacterCount = 16;
inline constexpr size_t kCharacterStride = 0x21D0;
inline constexpr size_t kCharacterOffset = 0xA400;     // first record within a data slot

// Character record field offsets (relative to the record start).
inline constexpr size_t kCharHpCurrent = 0xDC;         // u16 little-endian
inline constexpr size_t kCharHpMax = 0xE0;             // u16 little-endian
inline constexpr size_t kCharSkillPoints = 0xF4;       // u16 little-endian
inline constexpr size_t kCharLevel = 0xF6;             // u8; stored = displayed level - 1
inline constexpr size_t kCharClass = 0xF7;             // u8
inline constexpr size_t kCharColor = 0xF8;             // u8, native colour preset 0..4
inline constexpr size_t kCharIdentity = 0x1D4;         // the block the game tells saved characters apart by
inline constexpr size_t kCharIdentityBytes = 18;
inline constexpr size_t kCharName = 0x208;             // 8-byte, 8-bit (ASCII on Western builds)
inline constexpr size_t kCharNameBytes = 8;
inline constexpr size_t kCharNameSecond = 0x210;       // reading/furigana copy; mirrored on Western builds
inline constexpr uint32_t kEmptyRecordFill = 0xCCCCCCCC;

inline constexpr int kMinLevel = 1;
inline constexpr int kMaxLevel = 255;
inline constexpr int kMaxSkillPoints = 255;
inline constexpr int kMaxHp = 65535;
inline constexpr uint32_t kMaxGold = 99999999;

inline constexpr int kNativeColorCount = 5;

// The game's class ids (its texture and name tables are indexed by them); never written.
enum class CharacterClass : uint8_t { kFighter = 0, kAmazon = 1, kElf = 2, kDwarf = 3, kSorceress = 4, kWizard = 5 };

constexpr std::array<CharacterClass, 6> kCharacterClasses = {
    CharacterClass::kFighter, CharacterClass::kAmazon,    CharacterClass::kElf,
    CharacterClass::kDwarf,   CharacterClass::kSorceress, CharacterClass::kWizard,
};

std::string_view ClassName(CharacterClass klass);
CharacterClass ClassFromId(uint8_t id);

// A character as shown and edited; offsets point into the working byte buffer.
struct CharacterView {
    size_t record_offset = 0; // absolute offset of the record in the buffer
    bool present = false;
    int slot = 0;             // 0-based record index within its data slot
    uint8_t level_stored = 0; // displayed level is level_stored + 1
    uint16_t skill_points = 0;
    uint16_t hp_current = 0;
    uint16_t hp_max = 0;
    std::string name;         // UTF-8
    CharacterClass klass = CharacterClass::kFighter;
    uint8_t color = 0;        // native colour preset as stored; valid is 0..4
    bool color_valid() const { return color < kNativeColorCount; }
};

struct DataSlotView {
    size_t base_offset = 0; // absolute offset of the slot in the buffer
    uint32_t gold = 0;
    std::array<CharacterView, kCharacterCount> characters;
};

// A parsed, editable SAVE0.DAT. The byte buffer is a working copy; edits write into it and ApplyChanges recomputes
// the CRC and persists it. Reading and selecting never touch the buffer.
class SaveModel {
public:
    static SaveModel FromBytes(std::span<const uint8_t> bytes);
    static SaveModel FromFile(const std::filesystem::path& path);

    bool valid() const { return valid_; }
    const std::vector<std::string>& errors() const { return errors_; }
    bool crc_valid() const { return crc_valid_; }

    std::span<const uint8_t> bytes() const { return bytes_; }
    std::span<uint8_t> bytes() { return bytes_; }
    std::span<const DataSlotView> slots() const { return slots_; }

    const DataSlotView* Slot(int index) const;
    const CharacterView* Character(int slot, int character) const;

    // Level helpers. Stored level is displayed - 1; the game also accepts raw 255 (displayed 255) on the newest
    // Pro build, so the editor clamps displayed levels to 1..255 and stores displayed - 1.
    static int DisplayedLevel(uint8_t stored);
    static uint8_t StoredLevel(int displayed);

    // Edits a field on the working copy. Returns false when out of range or structurally unsafe.
    bool SetGold(int slot, uint32_t gold);
    bool SetLevel(int slot, int character, int displayed_level);
    bool SetSkillPoints(int slot, int character, int skill_points);
    bool SetHp(int slot, int character, uint16_t current, uint16_t max);
    bool SetName(int slot, int character, std::string_view utf8_name);
    // The native colour preset the game's own Edit character screen writes.
    bool SetColor(int slot, int character, int color);

    // Recomputes the CRC over the working copy. Serialize() returns the finished bytes; Apply never writes by itself.
    std::vector<uint8_t> Serialize() const;

private:
    bool valid_ = false;
    bool crc_valid_ = false;
    std::vector<std::string> errors_;
    std::vector<uint8_t> bytes_;
    std::array<DataSlotView, kDataSlotCount> slots_{};
};

// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) over bytes[4..size); the standard the game uses.
uint32_t SaveCrc32(std::span<const uint8_t> bytes);

// True when bytes look like a structurally valid SAVE0.DAT (size, magic, CRC). Problems are collected in errors.
bool ValidateSave(std::span<const uint8_t> bytes, std::vector<std::string>& errors);

// A full string dump of the model for logs and diagnostics; no field is hidden.
std::string DescribeSave(const SaveModel& model);

} // namespace dcp
