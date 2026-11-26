#include "save_integrity.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>

namespace dcp {
namespace {

constexpr uint32_t kMagic = 0x49504344; // "DCPI"
constexpr uint32_t kVersion = 1;
constexpr size_t kTitleChars = 15;
constexpr size_t kDirChars = 31;
constexpr int kMarkAttempts = 3;

void Put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(uint8_t(value >> (8 * i)));
}

void PutText(std::vector<uint8_t>& out, std::string_view text) {
    out.push_back(uint8_t(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

class Cursor {
public:
    explicit Cursor(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    bool U32(uint32_t& v) {
        if (at_ + 4 > bytes_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(bytes_[at_++]) << (8 * i);
        return true;
    }
    bool Text(std::string& v) {
        if (at_ + 1 > bytes_.size()) return false;
        const size_t n = bytes_[at_++];
        if (at_ + n > bytes_.size()) return false;
        v.assign(reinterpret_cast<const char*>(bytes_.data() + at_), n);
        at_ += n;
        return true;
    }
    bool done() const { return at_ == bytes_.size(); }

private:
    std::span<const uint8_t> bytes_;
    size_t at_ = 0;
};

bool ValidName(std::string_view text, size_t max) {
    return !text.empty() && text.size() <= max && text.front() != '.' &&
           std::ranges::all_of(text, [](char c) { return c > 0x20 && c < 0x7F && c != '/' && c != '\\' && c != '|'; });
}

std::optional<std::vector<uint8_t>> ReadAll(const std::filesystem::path& path, bool& readable) {
    std::error_code ec;
    readable = true;
    if (!std::filesystem::exists(path, ec)) {
        readable = !ec;
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        readable = false;
        return std::nullopt;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) readable = false;
    return bytes;
}

// The launcher and the game can both write the file: each writer has its own temporary name.
std::filesystem::path TemporaryFor(const std::filesystem::path& path) {
    static std::atomic<uint64_t> counter{0};
    const uint64_t stamp = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    return path.string() + ".tmp" + std::to_string(stamp ^ (counter.fetch_add(1) << 48));
}

bool WriteAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temporary = TemporaryFor(path);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!out.flush()) {
            out.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) std::filesystem::remove(temporary, ec);
    return !ec;
}

} // namespace

bool ValidSaveKey(const SaveKey& key) {
    return ValidName(key.title_id, kTitleChars) && ValidName(key.save_dir, kDirChars);
}

std::filesystem::path SaveIntegrityStorePath(const std::filesystem::path& user_dir) {
    return user_dir / "online_integrity.dat";
}

uint32_t SaveIntegrityStore::Flags(const SaveKey& key) const {
    for (const auto& [stored, flags] : entries_) {
        if (stored == key) return flags;
    }
    return 0;
}

uint32_t SaveIntegrityStore::TitleFlags(std::string_view title_id) const {
    uint32_t flags = 0;
    for (const auto& [stored, bits] : entries_) {
        if (stored.title_id == title_id) flags |= bits;
    }
    return flags;
}

void SaveIntegrityStore::Mark(const SaveKey& key, uint32_t bits) {
    bits &= kIntegrityKnownBits;
    if (bits == 0 || !ValidSaveKey(key)) return;
    const auto at = std::ranges::lower_bound(entries_, key, {}, &std::pair<SaveKey, uint32_t>::first);
    if (at != entries_.end() && at->first == key) {
        at->second |= bits;
    } else {
        entries_.insert(at, {key, bits});
    }
}

std::vector<uint8_t> SaveIntegrityStore::Serialize() const {
    std::vector<uint8_t> out;
    Put32(out, kMagic);
    Put32(out, kVersion);
    Put32(out, uint32_t(entries_.size()));
    for (const auto& [key, flags] : entries_) {
        PutText(out, key.title_id);
        PutText(out, key.save_dir);
        Put32(out, flags);
    }
    return out;
}

bool SaveIntegrityStore::Parse(std::span<const uint8_t> bytes) {
    Cursor in(bytes);
    uint32_t magic = 0, version = 0, count = 0;
    if (!in.U32(magic) || magic != kMagic || !in.U32(version)) {
        error_ = "unrecognized online integrity data";
        return false;
    }
    if (version != kVersion) {
        error_ = "online integrity file version " + std::to_string(version) + " is not supported";
        return false;
    }
    if (!in.U32(count)) {
        error_ = "online integrity file is truncated";
        return false;
    }
    std::vector<std::pair<SaveKey, uint32_t>> entries;
    for (uint32_t i = 0; i < count; ++i) {
        SaveKey key;
        uint32_t flags = 0;
        if (!in.Text(key.title_id) || !in.Text(key.save_dir) || !in.U32(flags)) {
            error_ = "online integrity file is truncated";
            return false;
        }
        const bool ordered = entries.empty() || entries.back().first < key;
        if (!ValidSaveKey(key) || flags == 0 || (flags & ~kIntegrityKnownBits) != 0 || !ordered) {
            error_ = "online integrity file has a malformed entry";
            return false;
        }
        entries.emplace_back(std::move(key), flags);
    }
    if (!in.done()) {
        error_ = "online integrity file has trailing data";
        return false;
    }
    entries_ = std::move(entries);
    error_.clear();
    return true;
}

bool SaveIntegrityStore::Load() {
    entries_.clear();
    error_.clear();
    bool readable = true;
    const std::optional<std::vector<uint8_t>> bytes = ReadAll(path_, readable);
    if (!readable) {
        error_ = "the online integrity file cannot be read";
        return false;
    }
    return !bytes || Parse(*bytes);
}

bool SaveIntegrityStore::Save() const {
    return error_.empty() && WriteAtomic(path_, Serialize());
}

MarkResult MarkSaveIntegrity(const std::filesystem::path& user_dir, const SaveKey& key, uint32_t bits, std::string* error) {
    bits &= kIntegrityKnownBits;
    if (bits == 0 || !ValidSaveKey(key)) {
        if (error) *error = "invalid save key";
        return MarkResult::kFailed;
    }
    SaveIntegrityStore store(SaveIntegrityStorePath(user_dir));
    bool wrote = false;
    for (int attempt = 0; attempt < kMarkAttempts; ++attempt) {
        if (!store.Load()) break;
        if ((store.Flags(key) & bits) == bits) return wrote ? MarkResult::kMarked : MarkResult::kAlreadySet;
        store.Mark(key, bits);
        if (!store.Save()) break;
        wrote = true;
    }
    if (error) *error = store.error().empty() ? "the online integrity file could not be written" : store.error();
    return MarkResult::kFailed;
}

bool IsGameplayEdit(const SaveModel& before, const SaveModel& after) {
    const std::span<const uint8_t> old_bytes = before.bytes();
    std::vector<uint8_t> masked(after.bytes().begin(), after.bytes().end());
    if (!before.valid() || masked.size() != old_bytes.size()) return true;
    const auto restore = [&](size_t offset, size_t length) {
        std::copy_n(old_bytes.begin() + std::ptrdiff_t(offset), length, masked.begin() + std::ptrdiff_t(offset));
    };
    restore(kSaveCrcOffset, kSaveMagicOffset);
    for (const DataSlotView& slot : before.slots()) {
        for (const CharacterView& character : slot.characters) {
            if (!character.present) continue;
            restore(character.record_offset + kCharName, kCharNameBytes);
            restore(character.record_offset + kCharNameSecond, kCharNameBytes);
            restore(character.record_offset + kCharColor, 1);
        }
    }
    return !std::ranges::equal(masked, old_bytes);
}

uint32_t OverrideEvidence(const CharacterOverrideStore& overrides, const DiscoveredSave& save) {
    if (!save.model.valid()) return 0;
    for (const auto& [key, entry] : overrides.entries()) {
        if (!entry.Any() || key.title_id != save.title_id || key.save_dir != save.dir_name) continue;
        const CharacterView* character = save.model.Character(key.data_slot, key.character_slot);
        if (!character || !character->present || character->klass != key.klass) continue;
        const std::span<const uint8_t> identity =
            save.model.bytes().subspan(character->record_offset + kCharIdentity, kCharIdentityBytes);
        if (entry.Bound() && !entry.BelongsTo(identity)) continue;
        return kIntegrityGameplayEditorUsed;
    }
    return 0;
}

std::optional<uint32_t> SaveIntegrityFlags(const std::filesystem::path& user_dir, std::span<const DiscoveredSave> saves,
                                           std::string_view title_id, std::string_view save_dir) {
    SaveIntegrityStore store(SaveIntegrityStorePath(user_dir));
    if (title_id.empty() || !store.Load()) return std::nullopt;
    CharacterOverrideStore overrides(CharacterOverrideStorePath(user_dir));
    const bool overrides_read = overrides.Load();
    uint32_t flags = save_dir.empty() ? store.TitleFlags(title_id) : store.Flags({std::string(title_id), std::string(save_dir)});
    for (const DiscoveredSave& save : saves) {
        if (save.title_id != title_id || (!save_dir.empty() && save.dir_name != save_dir)) continue;
        const SaveKey key{save.title_id, save.dir_name};
        const uint32_t evidence = overrides_read ? OverrideEvidence(overrides, save) : 0;
        if ((evidence & ~store.Flags(key)) != 0) MarkSaveIntegrity(user_dir, key, evidence);
        flags |= evidence;
    }
    if (!overrides_read && (flags & kIntegrityGameplayEditorUsed) == 0) return std::nullopt;
    return flags;
}

int HelloIntegrityFlags(const std::filesystem::path& user_dir, std::string_view title_id, std::string_view save_dir) {
    if (title_id.empty()) return -1;
    const std::vector<DiscoveredSave> saves = DiscoverSaves(user_dir, title_id);
    const std::optional<uint32_t> flags = SaveIntegrityFlags(user_dir, saves, title_id, save_dir);
    return flags ? int(*flags) : -1;
}

} // namespace dcp

extern "C" int dcp_save_integrity_mark(const char* user_dir, const char* title_id, const char* save_dir, unsigned bits) {
    if (!user_dir || !title_id || !save_dir) return 0;
    return dcp::MarkSaveIntegrity(user_dir, {title_id, save_dir}, bits) != dcp::MarkResult::kFailed ? 1 : 0;
}

extern "C" int dcp_save_integrity_flags(const char* user_dir, const char* title_id, const char* save_dir) {
    if (!user_dir || !title_id) return -1;
    return dcp::HelloIntegrityFlags(user_dir, title_id, save_dir ? save_dir : "");
}
