#include "hp_override.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace dcp {
namespace {

constexpr uint32_t kMagic = 0x4F484344; // "DCHO"
constexpr uint32_t kVersion = 3;
constexpr uint32_t kVersionNoIdentity = 2;
constexpr uint32_t kVersionHpOnly = 1;
constexpr size_t kStoredValues = 7;

void Put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(uint8_t(value));
    out.push_back(uint8_t(value >> 8));
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

void PutText(std::vector<uint8_t>& out, std::string_view text) {
    const size_t n = std::min<size_t>(text.size(), 255);
    out.push_back(uint8_t(n));
    out.insert(out.end(), text.begin(), text.begin() + std::ptrdiff_t(n));
}

class Cursor {
public:
    explicit Cursor(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    bool U8(uint8_t& v) {
        if (at_ + 1 > bytes_.size()) return false;
        v = bytes_[at_++];
        return true;
    }
    bool U16(uint16_t& v) {
        if (at_ + 2 > bytes_.size()) return false;
        v = uint16_t(bytes_[at_]) | uint16_t(bytes_[at_ + 1]) << 8;
        at_ += 2;
        return true;
    }
    bool U32(uint32_t& v) {
        if (at_ + 4 > bytes_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(bytes_[at_++]) << (8 * i);
        return true;
    }
    bool Text(std::string& v) {
        uint8_t n = 0;
        if (!U8(n) || at_ + n > bytes_.size()) return false;
        v.assign(reinterpret_cast<const char*>(bytes_.data() + at_), n);
        at_ += n;
        return true;
    }
    bool done() const { return at_ == bytes_.size(); }

private:
    std::span<const uint8_t> bytes_;
    size_t at_ = 0;
};

std::optional<CustomKey> ParseKey(const std::string& text) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '|') {
            fields.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (fields.size() != 5 || fields[0].empty() || fields[1].empty()) return std::nullopt;
    CustomKey key;
    key.title_id = fields[0];
    key.save_dir = fields[1];
    try {
        key.data_slot = std::stoi(fields[2]);
        key.character_slot = std::stoi(fields[3]);
        const int klass_id = std::stoi(fields[4]);
        if (key.data_slot < 0 || key.data_slot >= int(kDataSlotCount) || key.character_slot < 0 ||
            key.character_slot >= int(kCharacterCount) || klass_id < 0 || klass_id >= int(kCharacterClasses.size())) {
            return std::nullopt;
        }
        key.klass = ClassFromId(uint8_t(klass_id));
    } catch (...) {
        return std::nullopt;
    }
    return key;
}

bool WriteAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out.flush()) {
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) std::filesystem::remove(temporary, ec);
    return !ec;
}

uint16_t FieldValue(const CharacterOverride& entry, size_t index) {
    switch (index) {
    case 0: return entry.max_hp;
    case 1: return entry.strength;
    case 2: return entry.constitution;
    case 3: return entry.dexterity;
    case 4: return entry.intelligence;
    case 5: return entry.magic_resistance;
    default: return entry.luck;
    }
}

void SetFieldValue(CharacterOverride& entry, size_t index, uint16_t value) {
    switch (index) {
    case 0: entry.max_hp = value; break;
    case 1: entry.strength = value; break;
    case 2: entry.constitution = value; break;
    case 3: entry.dexterity = value; break;
    case 4: entry.intelligence = value; break;
    case 5: entry.magic_resistance = value; break;
    default: entry.luck = value; break;
    }
}

} // namespace

uint16_t CharacterOverride::Value(uint16_t field) const {
    switch (field) {
    case CHARACTER_OVERRIDE_MAX_HP: return max_hp;
    case CHARACTER_OVERRIDE_STRENGTH: return strength;
    case CHARACTER_OVERRIDE_CONSTITUTION: return constitution;
    case CHARACTER_OVERRIDE_DEXTERITY: return dexterity;
    case CHARACTER_OVERRIDE_INTELLIGENCE: return intelligence;
    case CHARACTER_OVERRIDE_MAGIC_RESISTANCE: return magic_resistance;
    default: return luck;
    }
}

void CharacterOverride::Set(uint16_t field, uint16_t value) {
    present |= field;
    switch (field) {
    case CHARACTER_OVERRIDE_MAX_HP: max_hp = value; break;
    case CHARACTER_OVERRIDE_STRENGTH: strength = value; break;
    case CHARACTER_OVERRIDE_CONSTITUTION: constitution = value; break;
    case CHARACTER_OVERRIDE_DEXTERITY: dexterity = value; break;
    case CHARACTER_OVERRIDE_INTELLIGENCE: intelligence = value; break;
    case CHARACTER_OVERRIDE_MAGIC_RESISTANCE: magic_resistance = value; break;
    default: luck = value; break;
    }
}

void CharacterOverride::Clear(uint16_t field) {
    present &= uint16_t(~field);
    switch (field) {
    case CHARACTER_OVERRIDE_MAX_HP: max_hp = 0; break;
    case CHARACTER_OVERRIDE_STRENGTH: strength = 0; break;
    case CHARACTER_OVERRIDE_CONSTITUTION: constitution = 0; break;
    case CHARACTER_OVERRIDE_DEXTERITY: dexterity = 0; break;
    case CHARACTER_OVERRIDE_INTELLIGENCE: intelligence = 0; break;
    case CHARACTER_OVERRIDE_MAGIC_RESISTANCE: magic_resistance = 0; break;
    default: luck = 0; break;
    }
}

bool CharacterOverride::BelongsTo(std::span<const uint8_t> character_identity) const {
    return character_identity.size() == identity.size() &&
           appearance_same_identity(identity.data(), character_identity.data()) != 0;
}

std::filesystem::path CharacterOverrideStorePath(const std::filesystem::path& user_dir) {
    return user_dir / "character_overrides.dat";
}

void SyncCharacterOverrides(CharacterOverrideStore& store, const SaveModel& working, const SaveModel& persisted,
                            std::string_view title_id, std::string_view save_dir) {
    const auto sync = [](CharacterOverride& next, uint16_t field, uint16_t value, uint16_t before, bool zero_clears) {
        if (value == before) return;
        if (zero_clears && value == 0) next.Clear(field);
        else next.Set(field, value);
    };
    for (int slot = 0; slot < int(kDataSlotCount); ++slot) {
        const DataSlotView* before = persisted.Slot(slot);
        const DataSlotView* after = working.Slot(slot);
        if (!before || !after) continue;
        for (int index = 0; index < int(kCharacterCount); ++index) {
            const CharacterView& character = after->characters[index];
            if (!character.present) continue;
            const CustomKey key{std::string(title_id), std::string(save_dir), slot, index, character.klass};
            CharacterOverride next = store.Get(key).value_or(CharacterOverride{});
            const std::span<const uint8_t> identity =
                working.bytes().subspan(character.record_offset + kCharIdentity, kCharIdentityBytes);
            if (next.Bound() && !next.BelongsTo(identity)) next = CharacterOverride{};
            std::ranges::copy(identity, next.identity.begin());
            const CharacterView& old = before->characters[index];
            sync(next, CHARACTER_OVERRIDE_MAX_HP, character.hp_max, old.hp_max, true);
            sync(next, CHARACTER_OVERRIDE_STRENGTH, character.stats.strength, old.stats.strength, false);
            sync(next, CHARACTER_OVERRIDE_CONSTITUTION, character.stats.constitution, old.stats.constitution, false);
            sync(next, CHARACTER_OVERRIDE_DEXTERITY, character.stats.dexterity, old.stats.dexterity, false);
            sync(next, CHARACTER_OVERRIDE_INTELLIGENCE, character.stats.intelligence, old.stats.intelligence, false);
            sync(next, CHARACTER_OVERRIDE_MAGIC_RESISTANCE, character.stats.magic_resistance,
                 old.stats.magic_resistance, false);
            sync(next, CHARACTER_OVERRIDE_LUCK, character.stats.luck, old.stats.luck, false);
            if (next.Any()) store.Set(key, next);
            else store.Clear(key);
        }
    }
}

std::vector<uint8_t> CharacterOverrideStore::Serialize() const {
    std::vector<uint8_t> out;
    Put32(out, kMagic);
    Put32(out, kVersion);
    Put32(out, uint32_t(entries_.size()));
    for (const auto& [key, entry] : entries_) {
        PutText(out, key.Text());
        Put16(out, entry.present);
        for (size_t i = 0; i < kStoredValues; ++i) Put16(out, FieldValue(entry, i));
        out.insert(out.end(), entry.identity.begin(), entry.identity.end());
    }
    return out;
}

bool CharacterOverrideStore::Parse(std::span<const uint8_t> bytes) {
    Cursor in(bytes);
    uint32_t magic = 0, version = 0, count = 0;
    if (!in.U32(magic) || magic != kMagic || !in.U32(version) || !in.U32(count)) {
        error_ = "unrecognized character override data";
        return false;
    }
    if (version != kVersion && version != kVersionNoIdentity && version != kVersionHpOnly) {
        error_ = "character override file version " + std::to_string(version) + " is not supported";
        return false;
    }
    std::vector<std::pair<CustomKey, CharacterOverride>> entries;
    for (uint32_t i = 0; i < count; ++i) {
        std::string key_text;
        if (!in.Text(key_text)) {
            error_ = "character override file is truncated";
            return false;
        }
        const std::optional<CustomKey> key = ParseKey(key_text);
        if (!key) {
            error_ = "character override file has a malformed entry";
            return false;
        }
        CharacterOverride entry;
        if (version == kVersionHpOnly) {
            uint16_t max_hp = 0;
            if (!in.U16(max_hp) || max_hp == 0) {
                error_ = "character override file is truncated";
                return false;
            }
            entry.Set(CHARACTER_OVERRIDE_MAX_HP, max_hp);
        } else {
            if (!in.U16(entry.present) || (entry.present & ~CHARACTER_OVERRIDE_ALL) != 0) {
                error_ = "character override file has a malformed entry";
                return false;
            }
            for (size_t field = 0; field < kStoredValues; ++field) {
                uint16_t value = 0;
                if (!in.U16(value)) {
                    error_ = "character override file is truncated";
                    return false;
                }
                if (entry.present & (1u << field)) SetFieldValue(entry, field, value);
            }
            if (!entry.Any() || (entry.Has(CHARACTER_OVERRIDE_MAX_HP) && entry.max_hp == 0)) {
                error_ = "character override file has a malformed entry";
                return false;
            }
            for (uint8_t& byte : entry.identity) {
                if (version == kVersion && !in.U8(byte)) {
                    error_ = "character override file is truncated";
                    return false;
                }
            }
        }
        entries.emplace_back(*key, entry);
    }
    if (!in.done()) {
        error_ = "character override file has trailing data";
        return false;
    }
    entries_ = std::move(entries);
    error_.clear();
    return true;
}

bool CharacterOverrideStore::Load() {
    entries_.clear();
    error_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) return true;
    std::ifstream in(path_, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Parse(bytes);
}

bool CharacterOverrideStore::Save() const {
    if (!error_.empty()) return false;
    std::error_code ec;
    if (entries_.empty()) {
        std::filesystem::remove(path_, ec);
        return !std::filesystem::exists(path_, ec);
    }
    return WriteAtomic(path_, Serialize());
}

std::optional<CharacterOverride> CharacterOverrideStore::Get(const CustomKey& key) const {
    for (const auto& [stored, entry] : entries_) {
        if (stored == key) return entry;
    }
    return std::nullopt;
}

void CharacterOverrideStore::Set(const CustomKey& key, std::optional<CharacterOverride> override) {
    const bool remove = !override || !override->Any();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first == key) {
            if (remove) entries_.erase(it);
            else it->second = *override;
            return;
        }
    }
    if (!remove) entries_.emplace_back(key, *override);
}

} // namespace dcp
