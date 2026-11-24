#include "hp_override.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace dcp {
namespace {

constexpr uint32_t kMagic = 0x4F484344; // "DCHO"
constexpr uint32_t kVersion = 1;

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

} // namespace

std::filesystem::path HpOverrideStorePath(const std::filesystem::path& user_dir) {
    return user_dir / "character_overrides.dat";
}

void SyncHpOverrides(HpOverrideStore& store, const SaveModel& working, const SaveModel& persisted,
                     std::string_view title_id, std::string_view save_dir) {
    for (int slot = 0; slot < int(kDataSlotCount); ++slot) {
        const DataSlotView* before = persisted.Slot(slot);
        const DataSlotView* after = working.Slot(slot);
        if (!before || !after) continue;
        for (int index = 0; index < int(kCharacterCount); ++index) {
            const CharacterView& character = after->characters[index];
            if (!character.present) continue;
            const CustomKey key{std::string(title_id), std::string(save_dir), slot, index, character.klass};
            if (!store.Get(key) && character.hp_max == before->characters[index].hp_max) continue;
            store.Set(key, character.hp_max > 0 ? std::optional<uint16_t>(character.hp_max) : std::nullopt);
        }
    }
}

std::vector<uint8_t> HpOverrideStore::Serialize() const {
    std::vector<uint8_t> out;
    Put32(out, kMagic);
    Put32(out, kVersion);
    Put32(out, uint32_t(entries_.size()));
    for (const auto& [key, max_hp] : entries_) {
        PutText(out, key.Text());
        Put16(out, max_hp);
    }
    return out;
}

bool HpOverrideStore::Parse(std::span<const uint8_t> bytes) {
    Cursor in(bytes);
    uint32_t magic = 0, version = 0, count = 0;
    if (!in.U32(magic) || magic != kMagic || !in.U32(version) || !in.U32(count)) {
        error_ = "unrecognized HP override data";
        return false;
    }
    if (version != kVersion) {
        error_ = "HP override file version " + std::to_string(version) + " is not supported";
        return false;
    }
    std::vector<std::pair<CustomKey, uint16_t>> entries;
    for (uint32_t i = 0; i < count; ++i) {
        std::string key_text;
        uint16_t max_hp = 0;
        if (!in.Text(key_text) || !in.U16(max_hp)) {
            error_ = "HP override file is truncated";
            return false;
        }
        const std::optional<CustomKey> key = ParseKey(key_text);
        if (!key || max_hp == 0) {
            error_ = "HP override file has a malformed entry";
            return false;
        }
        entries.emplace_back(*key, max_hp);
    }
    if (!in.done()) {
        error_ = "HP override file has trailing data";
        return false;
    }
    entries_ = std::move(entries);
    error_.clear();
    return true;
}

bool HpOverrideStore::Load() {
    entries_.clear();
    error_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) return true;
    std::ifstream in(path_, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Parse(bytes);
}

bool HpOverrideStore::Save() const {
    if (!error_.empty()) return false;
    std::error_code ec;
    if (entries_.empty()) {
        std::filesystem::remove(path_, ec);
        return !std::filesystem::exists(path_, ec);
    }
    return WriteAtomic(path_, Serialize());
}

std::optional<uint16_t> HpOverrideStore::Get(const CustomKey& key) const {
    for (const auto& [stored, max_hp] : entries_) {
        if (stored == key) return max_hp;
    }
    return std::nullopt;
}

void HpOverrideStore::Set(const CustomKey& key, std::optional<uint16_t> max_hp) {
    const bool remove = !max_hp || *max_hp == 0;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first == key) {
            if (remove) entries_.erase(it);
            else it->second = *max_hp;
            return;
        }
    }
    if (!remove) entries_.emplace_back(key, *max_hp);
}

} // namespace dcp
