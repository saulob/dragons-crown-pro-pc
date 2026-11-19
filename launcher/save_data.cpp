#include "save_data.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace dcp {
namespace {

uint16_t ReadU16(std::span<const uint8_t> bytes, size_t offset) {
    if (offset + 2 > bytes.size()) {
        return 0;
    }
    return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1]) << 8;
}

uint32_t ReadU32(std::span<const uint8_t> bytes, size_t offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

void WriteU16(std::span<uint8_t> bytes, size_t offset, uint16_t value) {
    if (offset + 2 > bytes.size()) {
        return;
    }
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void WriteU32(std::span<uint8_t> bytes, size_t offset, uint32_t value) {
    if (offset + 4 > bytes.size()) {
        return;
    }
    for (size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

size_t CharacterOffset(size_t slot, size_t character) {
    return slot * kDataSlotStride + kCharacterOffset + character * kCharacterStride;
}

size_t DataSlotOffset(size_t slot) { return slot * kDataSlotStride; }

bool RecordPresent(std::span<const uint8_t> bytes, size_t offset) {
    // An empty record is the 0xCC fill; a reserved-but-unused record keeps the identity block but an empty name. A real
    // character always has a name, so both are treated as absent.
    return offset + kCharName + 1 <= bytes.size() && ReadU32(bytes, offset) != kEmptyRecordFill &&
           bytes[offset + kCharName] != 0;
}

} // namespace

uint32_t SaveCrc32(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 4; i < bytes.size(); ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

std::string_view ClassName(CharacterClass klass) {
    switch (klass) {
    case CharacterClass::kFighter: return "Fighter";
    case CharacterClass::kAmazon: return "Amazon";
    case CharacterClass::kElf: return "Elf";
    case CharacterClass::kDwarf: return "Dwarf";
    case CharacterClass::kWizard: return "Wizard";
    case CharacterClass::kSorceress: return "Sorceress";
    }
    return "Unknown";
}

CharacterClass ClassFromId(uint8_t id) {
    switch (id) {
    case 0: return CharacterClass::kFighter;
    case 1: return CharacterClass::kAmazon;
    case 2: return CharacterClass::kElf;
    case 3: return CharacterClass::kDwarf;
    case 4: return CharacterClass::kSorceress;
    case 5: return CharacterClass::kWizard;
    }
    return CharacterClass::kFighter;
}

bool ValidateSave(std::span<const uint8_t> bytes, std::vector<std::string>& errors) {
    if (bytes.size() != kSaveFileSize) {
        char message[96];
        std::snprintf(message, sizeof(message), "unexpected size %zu (expected %zu)", bytes.size(), kSaveFileSize);
        errors.emplace_back(message);
        return false;
    }
    if (ReadU32(bytes, kSaveMagicOffset) != kSaveMagic) {
        errors.emplace_back("missing DCSD magic");
        return false;
    }
    const uint32_t stored = ReadU32(bytes, kSaveCrcOffset);
    const uint32_t computed = SaveCrc32(bytes);
    if (stored != computed) {
        char message[96];
        std::snprintf(message, sizeof(message), "CRC mismatch: stored %08X, computed %08X", stored, computed);
        errors.emplace_back(message);
        return false;
    }
    return true;
}

SaveModel SaveModel::FromBytes(std::span<const uint8_t> bytes) {
    SaveModel model;
    model.bytes_.assign(bytes.begin(), bytes.end());
    model.valid_ = ValidateSave(model.bytes_, model.errors_);
    if (!model.valid_) {
        return model;
    }
    model.crc_valid_ = true;
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
        DataSlotView& view = model.slots_[slot];
        view.base_offset = DataSlotOffset(slot);
        view.gold = ReadU32(model.bytes_, view.base_offset + kDataSlotGoldOffset);
        for (size_t character = 0; character < kCharacterCount; ++character) {
            CharacterView& record = view.characters[character];
            record.record_offset = CharacterOffset(slot, character);
            record.slot = static_cast<int>(character);
            record.present = RecordPresent(model.bytes_, record.record_offset);
            if (!record.present) {
                continue;
            }
            record.level_stored = model.bytes_[record.record_offset + kCharLevel];
            record.skill_points = ReadU16(model.bytes_, record.record_offset + kCharSkillPoints);
            record.hp_current = ReadU16(model.bytes_, record.record_offset + kCharHpCurrent);
            record.hp_max = ReadU16(model.bytes_, record.record_offset + kCharHpMax);
            record.klass = ClassFromId(model.bytes_[record.record_offset + kCharClass]);
            record.color = model.bytes_[record.record_offset + kCharColor];
            const size_t name_offset = record.record_offset + kCharName;
            size_t length = 0;
            while (length < kCharNameBytes && model.bytes_[name_offset + length] != 0) {
                ++length;
            }
            record.name.assign(reinterpret_cast<const char*>(model.bytes_.data() + name_offset), length);
        }
    }
    return model;
}

SaveModel SaveModel::FromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        SaveModel model;
        model.errors_.push_back("could not open " + path.string());
        return model;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return FromBytes(bytes);
}

const DataSlotView* SaveModel::Slot(int index) const {
    return index >= 0 && index < static_cast<int>(kDataSlotCount) ? &slots_[index] : nullptr;
}

const CharacterView* SaveModel::Character(int slot, int character) const {
    const DataSlotView* data = Slot(slot);
    if (!data || character < 0 || character >= static_cast<int>(kCharacterCount)) {
        return nullptr;
    }
    return &data->characters[character];
}

int SaveModel::DisplayedLevel(uint8_t stored) { return static_cast<int>(stored) + 1; }

uint8_t SaveModel::StoredLevel(int displayed) {
    return static_cast<uint8_t>(std::clamp(displayed - 1, 0, 254));
}

bool SaveModel::SetGold(int slot, uint32_t gold) {
    if (slot < 0 || slot >= static_cast<int>(kDataSlotCount) || gold > kMaxGold) {
        return false;
    }
    WriteU32(bytes_, slots_[slot].base_offset + kDataSlotGoldOffset, gold);
    slots_[slot].gold = gold;
    return true;
}

bool SaveModel::SetLevel(int slot, int character, int displayed_level) {
    if (displayed_level < kMinLevel || displayed_level > kMaxLevel) {
        return false;
    }
    const DataSlotView* data = Slot(slot);
    if (!data || character < 0 || character >= static_cast<int>(kCharacterCount) || !data->characters[character].present) {
        return false;
    }
    bytes_[data->characters[character].record_offset + kCharLevel] = StoredLevel(displayed_level);
    slots_[slot].characters[character].level_stored = StoredLevel(displayed_level);
    return true;
}

bool SaveModel::SetSkillPoints(int slot, int character, int skill_points) {
    if (skill_points < 0 || skill_points > kMaxSkillPoints) {
        return false;
    }
    const DataSlotView* data = Slot(slot);
    if (!data || character < 0 || character >= static_cast<int>(kCharacterCount) || !data->characters[character].present) {
        return false;
    }
    WriteU16(bytes_, data->characters[character].record_offset + kCharSkillPoints, static_cast<uint16_t>(skill_points));
    slots_[slot].characters[character].skill_points = static_cast<uint16_t>(skill_points);
    return true;
}

bool SaveModel::SetHp(int slot, int character, uint16_t current, uint16_t max) {
    if (current > kMaxHp || max > kMaxHp || current > max) {
        return false;
    }
    const DataSlotView* data = Slot(slot);
    if (!data || character < 0 || character >= static_cast<int>(kCharacterCount) || !data->characters[character].present) {
        return false;
    }
    WriteU16(bytes_, data->characters[character].record_offset + kCharHpCurrent, current);
    WriteU16(bytes_, data->characters[character].record_offset + kCharHpMax, max);
    slots_[slot].characters[character].hp_current = current;
    slots_[slot].characters[character].hp_max = max;
    return true;
}

bool SaveModel::SetName(int slot, int character, std::string_view utf8_name) {
    if (utf8_name.empty() || utf8_name.size() > kCharNameBytes) {
        return false;
    }
    for (unsigned char ch : utf8_name) {
        if (ch < 0x20 || ch > 0x7E) {
            return false; // the Western builds store ASCII names; refuse anything that would not round-trip
        }
    }
    const DataSlotView* data = Slot(slot);
    if (!data || character < 0 || character >= static_cast<int>(kCharacterCount) || !data->characters[character].present) {
        return false;
    }
    const size_t offset = data->characters[character].record_offset + kCharName;
    std::memset(bytes_.data() + offset, 0, kCharNameBytes);
    std::memcpy(bytes_.data() + offset, utf8_name.data(), utf8_name.size());
    std::memset(bytes_.data() + offset + (kCharNameSecond - kCharName), 0, kCharNameBytes);
    std::memcpy(bytes_.data() + offset + (kCharNameSecond - kCharName), utf8_name.data(), utf8_name.size());
    slots_[slot].characters[character].name.assign(utf8_name);
    return true;
}

bool SaveModel::SetColor(int slot, int character, int color) {
    if (color < 0 || color >= kNativeColorCount) {
        return false;
    }
    const DataSlotView* data = Slot(slot);
    if (!data || character < 0 || character >= static_cast<int>(kCharacterCount) || !data->characters[character].present) {
        return false;
    }
    bytes_[data->characters[character].record_offset + kCharColor] = static_cast<uint8_t>(color);
    slots_[slot].characters[character].color = static_cast<uint8_t>(color);
    return true;
}

std::vector<uint8_t> SaveModel::Serialize() const {
    std::vector<uint8_t> result = bytes_;
    WriteU32(result, kSaveCrcOffset, SaveCrc32(result));
    return result;
}

std::string DescribeSave(const SaveModel& model) {
    if (!model.valid()) {
        std::string text = "invalid save";
        for (const std::string& error : model.errors()) {
            text += "; " + error;
        }
        return text;
    }
    std::string text = "DCSD, CRC " + (model.crc_valid() ? std::string("valid") : std::string("invalid"));
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
        const DataSlotView& data = model.slots()[slot];
        text += "\nDATA " + std::to_string(slot + 1) + " gold " + std::to_string(data.gold);
        for (const CharacterView& character : data.characters) {
            if (!character.present) {
                continue;
            }
            text += "\n  [" + std::to_string(character.slot + 1) + "] " + character.name + " " +
                    std::string(ClassName(character.klass)) + " Lv " + std::to_string(SaveModel::DisplayedLevel(character.level_stored)) +
                    " Color " + std::to_string(character.color) + " SP " + std::to_string(character.skill_points) +
                    " HP " + std::to_string(character.hp_current) + "/" + std::to_string(character.hp_max);
        }
    }
    return text;
}

} // namespace dcp
