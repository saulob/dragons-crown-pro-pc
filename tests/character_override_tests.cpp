// Focused headless tests for the Character Editor's persistent stat overrides: Apply semantics, per-field native
// fallback, live/status runtime overrides, identity isolation, sidecar compatibility, and real native hooks.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "appearance_custom.h"
#include "editor_model.h"
#include "feature_sites.h"
#include "hp_override.h"
#include "hp_override_hooks.h"
#include "hp_override_runtime.h"
#include "save_backup.h"
#include "save_data.h"
#include "save_discovery.h"
#include "save_edit.h"

namespace {

namespace fs = std::filesystem;
using namespace dcp;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* expr, int line) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
        ++g_failures;
    }
}

#define CHECK(expr) Check(static_cast<bool>(expr), #expr, __LINE__)

const CharacterStats kNative{257, 237, 148, 45, 139, 177};

void PutU16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b[off] = uint8_t(v);
    b[off + 1] = uint8_t(v >> 8);
}

void PutU32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (8 * i));
}

uint16_t RawU16(const std::vector<uint8_t>& b, size_t off) {
    return uint16_t(b[off]) | uint16_t(b[off + 1]) << 8;
}

uint32_t RawU32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) v |= uint32_t(b[off + i]) << (8 * i);
    return v;
}

uint32_t IndependentCrc32(const std::vector<uint8_t>& b) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 4; i < b.size(); ++i) {
        crc ^= b[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

void WriteCharacter(std::vector<uint8_t>& bytes, int slot, int idx, const char* name, CharacterClass klass,
                    uint8_t level_stored, uint16_t hp_cur, uint16_t hp_max, uint8_t id_seed,
                    const CharacterStats& stats = {}) {
    const size_t base = size_t(slot) * kDataSlotStride + kCharacterOffset + size_t(idx) * kCharacterStride;
    std::memset(bytes.data() + base, 0, kCharacterStride);
    PutU16(bytes, base + kCharHpCurrent, hp_cur);
    PutU16(bytes, base + kCharHpMax, hp_max);
    PutU16(bytes, base + kCharStrength, stats.strength);
    PutU16(bytes, base + kCharConstitution, stats.constitution);
    PutU16(bytes, base + kCharDexterity, stats.dexterity);
    PutU16(bytes, base + kCharIntelligence, stats.intelligence);
    PutU16(bytes, base + kCharMagicResistance, stats.magic_resistance);
    PutU16(bytes, base + kCharLuck, stats.luck);
    bytes[base + kCharLevel] = level_stored;
    bytes[base + kCharClass] = uint8_t(klass);
    bytes[base + kCharColor] = 0;
    for (size_t i = 0; i < kCharIdentityBytes; ++i) bytes[base + kCharIdentity + i] = uint8_t(id_seed + i);
    const size_t len = std::min<size_t>(std::strlen(name), kCharNameBytes);
    std::memcpy(bytes.data() + base + kCharName, name, len);
    std::memcpy(bytes.data() + base + kCharNameSecond, name, len);
}

std::vector<uint8_t> MakeSave() {
    std::vector<uint8_t> bytes(kSaveFileSize, 0);
    PutU32(bytes, kSaveMagicOffset, kSaveMagic);
    for (size_t s = 0; s < kDataSlotCount; ++s) {
        for (size_t c = 0; c < kCharacterCount; ++c) {
            const size_t base = s * kDataSlotStride + kCharacterOffset + c * kCharacterStride;
            PutU32(bytes, base, kEmptyRecordFill);
        }
    }
    return bytes;
}

void FinalizeAndWrite(const fs::path& path, std::vector<uint8_t>& bytes) {
    PutU32(bytes, 0, IndependentCrc32(bytes));
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteAll(const fs::path& path, std::span<const uint8_t> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

constexpr uint64_t kPlayerUpdate = 0x2ea7e0, kEnemyUpdate = 0xdc6e0;
const std::array<uint64_t, 8> kPlayerVirtuals = {0, 0, 0, 0, 0, kPlayerUpdate};
const std::array<uint64_t, 8> kEnemyVirtuals = {0, 0, 0, 0, 0, kEnemyUpdate};

struct FakeUnit {
    std::array<uint8_t, 0xb00> mem{};
    FakeUnit() { SetVirtuals(kPlayerVirtuals); }
    void SetVirtuals(const std::array<uint64_t, 8>& virtuals) {
        const uint64_t address = uint64_t(uintptr_t(virtuals.data()));
        std::memcpy(mem.data(), &address, sizeof(address));
    }
    int32_t CurrentHp() const { return ReadI32(CHARACTER_OVERRIDE_UNIT_CURRENT); }
    int32_t MaxHp() const { return ReadI32(CHARACTER_OVERRIDE_UNIT_MAX); }
    uint16_t Stat(uint32_t offset) const {
        uint16_t v = 0;
        std::memcpy(&v, mem.data() + offset, sizeof(v));
        return v;
    }
    CharacterStats Stats() const {
        return {Stat(CHARACTER_OVERRIDE_UNIT_STRENGTH), Stat(CHARACTER_OVERRIDE_UNIT_CONSTITUTION),
                Stat(CHARACTER_OVERRIDE_UNIT_DEXTERITY), Stat(CHARACTER_OVERRIDE_UNIT_INTELLIGENCE),
                Stat(CHARACTER_OVERRIDE_UNIT_MAGIC_RESISTANCE), Stat(CHARACTER_OVERRIDE_UNIT_LUCK)};
    }
    float EffectiveStat(uint32_t offset) const {
        float v = 0.0f;
        std::memcpy(&v, mem.data() + offset, sizeof(v));
        return v;
    }
    CharacterStats EffectiveStats() const {
        return {uint16_t(EffectiveStat(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_STRENGTH)),
                uint16_t(EffectiveStat(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_CONSTITUTION)),
                uint16_t(EffectiveStat(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_DEXTERITY)),
                uint16_t(EffectiveStat(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_INTELLIGENCE)),
                uint16_t(EffectiveStat(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_MAGIC_RESISTANCE)),
                uint16_t(EffectiveStat(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_LUCK))};
    }
    void SetEffectiveStats(const CharacterStats& stats) {
        StoreF32(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_STRENGTH, stats.strength);
        StoreF32(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_CONSTITUTION, stats.constitution);
        StoreF32(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_DEXTERITY, stats.dexterity);
        StoreF32(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_INTELLIGENCE, stats.intelligence);
        StoreF32(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_MAGIC_RESISTANCE, stats.magic_resistance);
        StoreF32(CHARACTER_OVERRIDE_UNIT_EFFECTIVE_LUCK, stats.luck);
    }
    void SetHp(int32_t cur, int32_t max) {
        std::memcpy(mem.data() + CHARACTER_OVERRIDE_UNIT_CURRENT, &cur, sizeof(cur));
        std::memcpy(mem.data() + CHARACTER_OVERRIDE_UNIT_MAX, &max, sizeof(max));
    }
    void SetStats(const CharacterStats& stats) {
        StoreU16(CHARACTER_OVERRIDE_UNIT_STRENGTH, stats.strength);
        StoreU16(CHARACTER_OVERRIDE_UNIT_CONSTITUTION, stats.constitution);
        StoreU16(CHARACTER_OVERRIDE_UNIT_DEXTERITY, stats.dexterity);
        StoreU16(CHARACTER_OVERRIDE_UNIT_INTELLIGENCE, stats.intelligence);
        StoreU16(CHARACTER_OVERRIDE_UNIT_MAGIC_RESISTANCE, stats.magic_resistance);
        StoreU16(CHARACTER_OVERRIDE_UNIT_LUCK, stats.luck);
    }
    void SetSlot(int32_t slot) { std::memcpy(mem.data() + 0xa70, &slot, sizeof(slot)); }
    uint64_t Addr() { return uint64_t(uintptr_t(mem.data())); }

private:
    int32_t ReadI32(uint32_t offset) const {
        int32_t v = 0;
        std::memcpy(&v, mem.data() + offset, sizeof(v));
        return v;
    }
    void StoreU16(uint32_t offset, uint16_t value) { std::memcpy(mem.data() + offset, &value, sizeof(value)); }
    void StoreF32(uint32_t offset, float value) { std::memcpy(mem.data() + offset, &value, sizeof(value)); }
};

struct FakeSeat {
    std::array<uint8_t, 0x23b0> mem{};
    void Init(CharacterClass klass, uint8_t id_seed, int32_t owner) {
        mem.fill(0);
        mem[APPEARANCE_RECORD_CLASS] = uint8_t(klass);
        for (size_t i = 0; i < APPEARANCE_IDENTITY_BYTES; ++i) mem[APPEARANCE_RECORD_IDENTITY + i] = uint8_t(id_seed + i);
        std::memcpy(mem.data() + APPEARANCE_SEAT_OWNER, &owner, sizeof(owner));
    }
    uint64_t Addr() { return uint64_t(uintptr_t(mem.data())); }
};

struct FakeSavedRecord {
    std::vector<uint8_t> mem = std::vector<uint8_t>(APPEARANCE_SEAT_OWNER + 0x10, 0xAB);
    void Init(CharacterClass klass, uint8_t id_seed) {
        mem[APPEARANCE_RECORD_CLASS] = uint8_t(klass);
        for (size_t i = 0; i < APPEARANCE_IDENTITY_BYTES; ++i) mem[APPEARANCE_RECORD_IDENTITY + i] = uint8_t(id_seed + i);
    }
    uint64_t Addr() { return uint64_t(uintptr_t(mem.data())); }
};

struct FakeStatusViewer {
    std::array<uint8_t, CHARACTER_OVERRIDE_STATUS_LUCK + 4> mem{};
    void SetRecord(uint64_t record) { std::memcpy(mem.data() + CHARACTER_OVERRIDE_STATUS_RECORD, &record, sizeof(record)); }
    uint64_t Addr() { return uint64_t(uintptr_t(mem.data())); }
    int32_t Hp() const { return ReadI32(CHARACTER_OVERRIDE_STATUS_MAX); }
    CharacterStats Stats() const {
        return {uint16_t(ReadI32(CHARACTER_OVERRIDE_STATUS_STRENGTH)),
                uint16_t(ReadI32(CHARACTER_OVERRIDE_STATUS_CONSTITUTION)),
                uint16_t(ReadI32(CHARACTER_OVERRIDE_STATUS_DEXTERITY)),
                uint16_t(ReadI32(CHARACTER_OVERRIDE_STATUS_INTELLIGENCE)),
                uint16_t(ReadI32(CHARACTER_OVERRIDE_STATUS_MAGIC_RESISTANCE)),
                uint16_t(ReadI32(CHARACTER_OVERRIDE_STATUS_LUCK))};
    }
    void SetHp(int32_t hp) { StoreI32(CHARACTER_OVERRIDE_STATUS_MAX, hp); }
    void SetLuck(int32_t value) { StoreI32(CHARACTER_OVERRIDE_STATUS_LUCK, value); }
    void SetStats(const CharacterStats& stats) {
        StoreI32(CHARACTER_OVERRIDE_STATUS_STRENGTH, stats.strength);
        StoreI32(CHARACTER_OVERRIDE_STATUS_CONSTITUTION, stats.constitution);
        StoreI32(CHARACTER_OVERRIDE_STATUS_DEXTERITY, stats.dexterity);
        StoreI32(CHARACTER_OVERRIDE_STATUS_INTELLIGENCE, stats.intelligence);
        StoreI32(CHARACTER_OVERRIDE_STATUS_MAGIC_RESISTANCE, stats.magic_resistance);
        StoreI32(CHARACTER_OVERRIDE_STATUS_LUCK, stats.luck);
    }

private:
    int32_t ReadI32(uint32_t offset) const {
        int32_t v = 0;
        std::memcpy(&v, mem.data() + offset, sizeof(v));
        return v;
    }
    void StoreI32(uint32_t offset, int32_t value) { std::memcpy(mem.data() + offset, &value, sizeof(value)); }
};

uint64_t g_test_unit_to_record_unit[4] = {};
uint64_t g_test_unit_to_record_seat[4] = {};

uint64_t TestRecordOfUnit(uint64_t unit) {
    for (int i = 0; i < 4; ++i) {
        if (g_test_unit_to_record_unit[i] == unit) return g_test_unit_to_record_seat[i];
    }
    return 0;
}

void RunNativeRecalc(FakeUnit& unit, int32_t native_hp, const CharacterStats& native_stats) {
    GuestRegs pre{};
    pre.rdi = unit.Addr();
    character_override_pre_recalc(&pre);
    unit.SetHp(native_hp, native_hp);
    unit.SetStats(native_stats);
    unit.SetEffectiveStats(native_stats);
    GuestRegs post{};
    post.r12 = unit.Addr();
    character_override_post_recalc(&post);
}

void RunStatusRefresh(FakeStatusViewer& viewer, int32_t native_hp, const CharacterStats& native_stats) {
    viewer.SetHp(native_hp);
    viewer.SetStats(native_stats);
    GuestRegs store{};
    store.r14 = viewer.Addr();
    store.rax = uint64_t(uint32_t(native_hp));
    character_override_status_max(&store);
    viewer.SetHp(int32_t(uint32_t(store.rax)));
    GuestRegs stats{};
    stats.r14 = viewer.Addr();
    stats.rax = native_stats.luck;
    character_override_status_stats(&stats);
    viewer.SetLuck(int32_t(uint32_t(stats.rax)));
}

CharacterStats AllCustom() {
    return {1000, 2000, 3000, 4000, 5000, 6000};
}

void SetOnly(CharacterOverride& override, uint16_t field, uint16_t value) {
    override = CharacterOverride{};
    override.Set(field, value);
}

CharacterStats With(CharacterStats stats, uint16_t field, uint16_t value) {
    switch (field) {
    case CHARACTER_OVERRIDE_STRENGTH: stats.strength = value; break;
    case CHARACTER_OVERRIDE_CONSTITUTION: stats.constitution = value; break;
    case CHARACTER_OVERRIDE_DEXTERITY: stats.dexterity = value; break;
    case CHARACTER_OVERRIDE_INTELLIGENCE: stats.intelligence = value; break;
    case CHARACTER_OVERRIDE_MAGIC_RESISTANCE: stats.magic_resistance = value; break;
    default: stats.luck = value; break;
    }
    return stats;
}

ApplyOutcome ApplyEditor(SaveEdit& edit, CustomStore& custom_saved, CharacterOverrideStore& override_saved,
                         std::string_view title, std::string_view dir) {
    CharacterOverrideStore override_next = override_saved;
    if (override_saved.error().empty())
        SyncCharacterOverrides(override_next, edit.working(), edit.persisted(), title, dir);
    return ApplyAll(&edit, custom_saved, custom_saved, &override_next, &override_saved);
}

void TestMaxHpRegressionAndAntiInfiniteHealth(const fs::path& root) {
    const fs::path user = root / "utgyfh_user";
    const fs::path save_path = user / "savedata" / "1" / "CUSA10209" / "SAVEDATA00" / "SAVE0.DAT";
    std::vector<uint8_t> raw = MakeSave();
    WriteCharacter(raw, 0, 0, "HERO", CharacterClass::kFighter, 9, 300, 300, 0x21, kNative);
    FinalizeAndWrite(save_path, raw);

    std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10209");
    CHECK(saves.size() == 1);
    SaveEdit edit(saves[0], BackupFolder(user));
    CustomStore custom_saved(CustomStorePath(user));
    CharacterOverrideStore override_saved(CharacterOverrideStorePath(user));
    CHECK(override_saved.Load() && override_saved.entries().empty());

    const CustomKey key{"CUSA10209", "SAVEDATA00", 0, 0, CharacterClass::kFighter};
    const CharacterView* ch = edit.working().Character(0, 0);
    CHECK(ch && ch->hp_current == 300 && ch->hp_max == 300);
    const HpValues next_max = WithMaxHp({ch->hp_current, ch->hp_max}, 4000);
    CHECK(next_max.current == 300 && next_max.max == 4000);
    CHECK(edit.working().SetHp(0, 0, uint16_t(next_max.current), uint16_t(next_max.max)));

    const ApplyOutcome applied = ApplyEditor(edit, custom_saved, override_saved, "CUSA10209", "SAVEDATA00");
    CHECK(applied.ok() && applied.save == SaveEdit::Result::kApplied && applied.override_written);
    const std::optional<CharacterOverride> stored = override_saved.Get(key);
    CHECK(stored && stored->Has(CHARACTER_OVERRIDE_MAX_HP) && stored->max_hp == 4000 && stored->present == 1);

    const std::vector<uint8_t> disk_bytes = ReadFile(save_path);
    CHECK(disk_bytes.size() == kSaveFileSize);
    CHECK(RawU32(disk_bytes, 0) == IndependentCrc32(disk_bytes));
    CHECK(RawU16(disk_bytes, 0xA400 + kCharHpCurrent) == 300);
    CHECK(RawU16(disk_bytes, 0xA400 + kCharHpMax) == 4000);

    const SaveModel reopened_save = SaveModel::FromFile(save_path);
    CHECK(reopened_save.valid());
    CHECK(reopened_save.Character(0, 0)->hp_current == 300);
    CHECK(reopened_save.Character(0, 0)->hp_max == 4000);
    CharacterOverrideStore reopened_override(CharacterOverrideStorePath(user));
    CHECK(reopened_override.Load() && reopened_override.Get(key)->max_hp == 4000);

    std::vector<std::string> notes;
    std::vector<CharacterOverrideEntry> entries =
        PlanCharacterOverrides(reopened_override, DiscoverSaves(user, "CUSA10209"), "CUSA10209", notes);
    CHECK(entries.size() == 1 && entries[0].max_hp == 4000 && entries[0].present == CHARACTER_OVERRIDE_MAX_HP);

    FakeUnit unit;
    FakeSeat seat;
    unit.SetSlot(0);
    seat.Init(CharacterClass::kFighter, 0x21, APPEARANCE_SEAT_LOCAL);
    g_test_unit_to_record_unit[0] = unit.Addr();
    g_test_unit_to_record_seat[0] = seat.Addr();
    character_override_hooks_set_record_of_unit(TestRecordOfUnit);
    character_override_hooks_set_entries(entries.data(), entries.size());

    unit.SetHp(reopened_save.Character(0, 0)->hp_current, reopened_save.Character(0, 0)->hp_max);
    RunNativeRecalc(unit, 300, kNative);
    CHECK(unit.CurrentHp() == 300 && unit.MaxHp() == 4000);

    CHECK(edit.working().SetHp(0, 0, 4000, 4000));
    CHECK(ApplyEditor(edit, custom_saved, override_saved, "CUSA10209", "SAVEDATA00").ok());
    const SaveModel full_save = SaveModel::FromFile(save_path);
    unit.SetHp(full_save.Character(0, 0)->hp_current, full_save.Character(0, 0)->hp_max);
    RunNativeRecalc(unit, 300, kNative);
    CHECK(unit.CurrentHp() == 4000 && unit.MaxHp() == 4000);

    unit.SetHp(2500, unit.MaxHp());
    RunNativeRecalc(unit, 300, kNative);
    CHECK(unit.MaxHp() == 4000 && unit.CurrentHp() == 2500);

    unit.SetHp(450, 500);
    RunNativeRecalc(unit, 500, kNative);
    CHECK(unit.MaxHp() == 4000 && unit.CurrentHp() == 450);

    const auto apply_max = [&](uint16_t max_hp) {
        const CharacterView* persisted = edit.persisted().Character(0, 0);
        const HpValues shown = WithMaxHp({persisted->hp_current, persisted->hp_max}, max_hp);
        CHECK(edit.working().SetHp(0, 0, uint16_t(shown.current), uint16_t(shown.max)));
        CHECK(ApplyEditor(edit, custom_saved, override_saved, "CUSA10209", "SAVEDATA00").ok());
        CHECK(override_saved.Get(key)->max_hp == max_hp);
        entries = PlanCharacterOverrides(override_saved, DiscoverSaves(user, "CUSA10209"), "CUSA10209", notes);
        CHECK(entries.size() == 1 && entries[0].max_hp == max_hp);
        character_override_hooks_set_entries(entries.data(), entries.size());
    };

    apply_max(200);
    unit.SetHp(450, 500);
    RunNativeRecalc(unit, 500, kNative);
    CHECK(unit.MaxHp() == 200 && unit.CurrentHp() == 200);

    apply_max(65535);
    unit.SetHp(200, 200);
    RunNativeRecalc(unit, 500, kNative);
    CHECK(unit.MaxHp() == 65535 && unit.CurrentHp() == 200);
}

void TestEveryStatLiveAndStatus(const fs::path& root) {
    const fs::path user = root / "stats_user";
    const fs::path save_path = user / "savedata" / "1" / "CUSA10454" / "SAVEDATA00" / "SAVE0.DAT";
    std::vector<uint8_t> raw = MakeSave();
    WriteCharacter(raw, 0, 0, "George", CharacterClass::kDwarf, 100, 500, 722, 0x41, kNative);
    FinalizeAndWrite(save_path, raw);

    CharacterOverrideStore store(CharacterOverrideStorePath(user));
    const CustomKey key{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kDwarf};
    CHECK(store.Load() && store.entries().empty());

    const std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10454");
    SaveEdit edit(saves[0], BackupFolder(user));
    CustomStore custom_saved(CustomStorePath(user));
    CHECK(edit.working().SetHp(0, 0, 500, 65535));
    const CharacterView* ch = edit.working().Character(0, 0);
    CHECK(ch && ch->stats == kNative);
    const CharacterStats custom = AllCustom();
    CHECK(edit.working().SetStats(0, 0, custom));
    CHECK(ApplyEditor(edit, custom_saved, store, "CUSA10454", "SAVEDATA00").ok());

    const std::optional<CharacterOverride> all = store.Get(key);
    CHECK(all && all->present == CHARACTER_OVERRIDE_ALL && all->max_hp == 65535);
    CHECK(all->strength == 1000 && all->constitution == 2000 && all->dexterity == 3000);
    CHECK(all->intelligence == 4000 && all->magic_resistance == 5000 && all->luck == 6000);

    const std::vector<uint8_t> disk = ReadFile(save_path);
    CHECK(RawU16(disk, 0xA400 + kCharStrength) == 1000);
    CHECK(RawU16(disk, 0xA400 + kCharIntelligence) == 4000);
    CHECK(RawU16(disk, 0xA400 + kCharDexterity) == 3000);
    CHECK(RawU16(disk, 0xA400 + kCharLuck) == 6000);
    CHECK(RawU16(disk, 0xA400 + kCharConstitution) == 2000);
    CHECK(RawU16(disk, 0xA400 + kCharMagicResistance) == 5000);
    CHECK(RawU16(disk, 0xA400 + kCharHpMax) == 65535);

    std::vector<std::string> notes;
    std::vector<CharacterOverrideEntry> entries =
        PlanCharacterOverrides(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
    CHECK(entries.size() == 1 && entries[0].present == CHARACTER_OVERRIDE_ALL);

    FakeUnit unit;
    FakeSeat seat;
    unit.SetSlot(0);
    seat.Init(CharacterClass::kDwarf, 0x41, APPEARANCE_SEAT_LOCAL);
    g_test_unit_to_record_unit[0] = unit.Addr();
    g_test_unit_to_record_seat[0] = seat.Addr();
    character_override_hooks_set_record_of_unit(TestRecordOfUnit);
    character_override_hooks_set_entries(entries.data(), entries.size());
    unit.SetHp(500, 65535);
    RunNativeRecalc(unit, 722, kNative);
    CHECK(unit.MaxHp() == 65535 && unit.CurrentHp() == 500);
    CHECK(unit.Stats() == custom);
    CHECK(unit.EffectiveStats() == custom);

    // An equipment or effect refresh rebuilds the effective bundle; the overrides finalize it again.
    const CharacterStats with_equipment{284, 237, 148, 45, 139, 177};
    unit.SetEffectiveStats(with_equipment);
    GuestRegs bundle{};
    bundle.rbx = unit.Addr();
    character_override_bundle_stats(&bundle);
    CHECK(unit.EffectiveStats() == custom);
    unit.SetEffectiveStats(with_equipment);
    GuestRegs effect{};
    effect.r14 = unit.Addr();
    character_override_effect_stats(&effect);
    CHECK(unit.EffectiveStats() == custom);

    FakeSavedRecord saved0;
    saved0.Init(CharacterClass::kDwarf, 0x41);
    FakeStatusViewer viewer;
    viewer.SetRecord(saved0.Addr());
    RunStatusRefresh(viewer, 722, kNative);
    CHECK(viewer.Hp() == 65535);
    CHECK(viewer.Stats() == custom);
    RunStatusRefresh(viewer, 722, with_equipment);
    CHECK(viewer.Stats() == custom);

    const std::array<uint16_t, 6> fields = {CHARACTER_OVERRIDE_STRENGTH,  CHARACTER_OVERRIDE_CONSTITUTION,
                                            CHARACTER_OVERRIDE_DEXTERITY, CHARACTER_OVERRIDE_INTELLIGENCE,
                                            CHARACTER_OVERRIDE_MAGIC_RESISTANCE, CHARACTER_OVERRIDE_LUCK};
    const std::array<uint16_t, 6> values = {1000, 1100, 1200, 1300, 1400, 1500};
    for (size_t i = 0; i < fields.size(); ++i) {
        CharacterOverride one;
        SetOnly(one, fields[i], values[i]);
        store.Set(key, one);
        entries = PlanCharacterOverrides(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
        CHECK(entries.size() == 1);
        character_override_hooks_set_entries(entries.data(), entries.size());
        unit.SetHp(500, 722);
        RunNativeRecalc(unit, 722, kNative);
        CHECK(unit.Stats() == With(kNative, fields[i], values[i]));
        CHECK(unit.EffectiveStats() == With(kNative, fields[i], values[i]));
        viewer.SetRecord(saved0.Addr());
        RunStatusRefresh(viewer, 722, kNative);
        CHECK(viewer.Stats() == With(kNative, fields[i], values[i]));
    }

    // Native recalculation (class, level, equipment) runs first; only the overridden field is final.
    CharacterOverride str_only;
    SetOnly(str_only, CHARACTER_OVERRIDE_STRENGTH, 1000);
    store.Set(key, str_only);
    entries = PlanCharacterOverrides(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
    character_override_hooks_set_entries(entries.data(), entries.size());
    const CharacterStats recalculated{284, 250, 160, 50, 150, 190};
    unit.SetHp(500, 3000);
    RunNativeRecalc(unit, 3000, recalculated);
    CHECK(unit.Stats() == With(recalculated, CHARACTER_OVERRIDE_STRENGTH, 1000));
    CHECK(unit.EffectiveStats() == With(recalculated, CHARACTER_OVERRIDE_STRENGTH, 1000));
    viewer.SetRecord(saved0.Addr());
    RunStatusRefresh(viewer, 3000, recalculated);
    CHECK(viewer.Stats() == With(recalculated, CHARACTER_OVERRIDE_STRENGTH, 1000));

    // Changing an existing override replaces it; no stale 1000 survives.
    str_only.Set(CHARACTER_OVERRIDE_STRENGTH, 900);
    store.Set(key, str_only);
    entries = PlanCharacterOverrides(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
    character_override_hooks_set_entries(entries.data(), entries.size());
    RunNativeRecalc(unit, 3000, recalculated);
    CHECK(unit.Stats().strength == 900);
    CHECK(unit.EffectiveStats().strength == 900);
    RunStatusRefresh(viewer, 3000, recalculated);
    CHECK(viewer.Stats().strength == 900);

    // The native u16 field is the editable limit: 65535 survives both paths unchanged.
    str_only.Set(CHARACTER_OVERRIDE_STRENGTH, kMaxCharacterStat);
    store.Set(key, str_only);
    entries = PlanCharacterOverrides(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
    character_override_hooks_set_entries(entries.data(), entries.size());
    RunNativeRecalc(unit, 3000, recalculated);
    CHECK(unit.Stats().strength == kMaxCharacterStat);
    CHECK(unit.EffectiveStats().strength == kMaxCharacterStat);
    RunStatusRefresh(viewer, 3000, recalculated);
    CHECK(viewer.Stats().strength == kMaxCharacterStat);
}

void TestNameOnlyApplyKeepsNativeAndExisting(const fs::path& root) {
    const fs::path user = root / "name_user";
    const fs::path save_path = user / "savedata" / "1" / "CUSA10454" / "SAVEDATA00" / "SAVE0.DAT";
    std::vector<uint8_t> raw = MakeSave();
    WriteCharacter(raw, 0, 0, "George", CharacterClass::kDwarf, 100, 500, 722, 0x41, kNative);
    FinalizeAndWrite(save_path, raw);

    CharacterOverrideStore store(CharacterOverrideStorePath(user));
    const CustomKey key{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kDwarf};
    CHECK(store.Load() && store.entries().empty());
    SaveEdit edit(DiscoverSaves(user, "CUSA10454")[0], BackupFolder(user));
    CustomStore custom_saved(CustomStorePath(user));
    CHECK(edit.working().SetName(0, 0, "Georgett"));
    CHECK(ApplyEditor(edit, custom_saved, store, "CUSA10454", "SAVEDATA00").ok());
    CHECK(store.entries().empty());
    std::vector<std::string> notes;
    CHECK(PlanCharacterOverrides(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes).empty());

    CharacterOverride str;
    str.Set(CHARACTER_OVERRIDE_STRENGTH, 1000);
    store.Set(key, str);
    CHECK(store.Save());
    SaveEdit second(DiscoverSaves(user, "CUSA10454")[0], BackupFolder(user));
    CustomStore second_custom(CustomStorePath(user));
    CharacterOverrideStore second_store(CharacterOverrideStorePath(user));
    CHECK(second_store.Load());
    CHECK(second.working().SetName(0, 0, "George2"));
    CHECK(ApplyEditor(second, second_custom, second_store, "CUSA10454", "SAVEDATA00").ok());
    const std::optional<CharacterOverride> kept = second_store.Get(key);
    CHECK(kept && kept->present == CHARACTER_OVERRIDE_STRENGTH && kept->strength == 1000);

    const std::vector<CharacterOverrideEntry> entries =
        PlanCharacterOverrides(second_store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
    CHECK(entries.size() == 1);
    character_override_hooks_set_entries(entries.data(), entries.size());
    FakeUnit unit;
    FakeSeat seat;
    unit.SetSlot(0);
    seat.Init(CharacterClass::kDwarf, 0x41, APPEARANCE_SEAT_LOCAL);
    g_test_unit_to_record_unit[0] = unit.Addr();
    g_test_unit_to_record_seat[0] = seat.Addr();
    character_override_hooks_set_record_of_unit(TestRecordOfUnit);
    RunNativeRecalc(unit, 722, kNative);
    CHECK(unit.Stats() == With(kNative, CHARACTER_OVERRIDE_STRENGTH, 1000));
    CHECK(unit.EffectiveStats() == With(kNative, CHARACTER_OVERRIDE_STRENGTH, 1000));
    CHECK(unit.MaxHp() == 722);
    character_override_hooks_set_entries(nullptr, 0);
}

void TestIsolationIdentityRemoteAndSidecar(const fs::path& root) {
    const fs::path user = root / "multi_user";
    const fs::path save_path = user / "savedata" / "1" / "CUSA10209" / "SAVEDATA00" / "SAVE0.DAT";
    std::vector<uint8_t> raw = MakeSave();
    WriteCharacter(raw, 0, 0, "HERO", CharacterClass::kFighter, 9, 300, 300, 0x31, kNative);
    WriteCharacter(raw, 0, 1, "HERO", CharacterClass::kFighter, 9, 300, 300, 0x61, kNative);
    FinalizeAndWrite(save_path, raw);

    CharacterOverrideStore store(CharacterOverrideStorePath(user));
    const CustomKey key1{"CUSA10209", "SAVEDATA00", 0, 0, CharacterClass::kFighter};
    const CustomKey key2{"CUSA10209", "SAVEDATA00", 0, 1, CharacterClass::kFighter};
    CharacterOverride a;
    a.Set(CHARACTER_OVERRIDE_STRENGTH, 1000);
    a.Set(CHARACTER_OVERRIDE_CONSTITUTION, 1100);
    CharacterOverride b;
    b.Set(CHARACTER_OVERRIDE_STRENGTH, 2000);
    b.Set(CHARACTER_OVERRIDE_CONSTITUTION, 2100);
    store.Set(key1, a);
    store.Set(key2, b);
    CHECK(store.Save());

    std::vector<std::string> notes;
    const std::vector<CharacterOverrideEntry> entries =
        PlanCharacterOverrides(store, DiscoverSaves(user, "CUSA10209"), "CUSA10209", notes);
    CHECK(entries.size() == 2);

    FakeUnit u1, u2, u_remote, u_unknown;
    FakeSeat s1, s2, s_remote, s_unknown;
    u1.SetSlot(0); s1.Init(CharacterClass::kFighter, 0x31, APPEARANCE_SEAT_LOCAL);
    u2.SetSlot(1); s2.Init(CharacterClass::kFighter, 0x61, APPEARANCE_SEAT_LOCAL);
    u_remote.SetSlot(2); s_remote.Init(CharacterClass::kFighter, 0x31, APPEARANCE_SEAT_REMOTE);
    u_unknown.SetSlot(3); s_unknown.Init(CharacterClass::kFighter, 0x99, APPEARANCE_SEAT_LOCAL);

    g_test_unit_to_record_unit[0] = u1.Addr(); g_test_unit_to_record_seat[0] = s1.Addr();
    g_test_unit_to_record_unit[1] = u2.Addr(); g_test_unit_to_record_seat[1] = s2.Addr();
    g_test_unit_to_record_unit[2] = u_remote.Addr(); g_test_unit_to_record_seat[2] = s_remote.Addr();
    g_test_unit_to_record_unit[3] = u_unknown.Addr(); g_test_unit_to_record_seat[3] = s_unknown.Addr();
    character_override_hooks_set_record_of_unit(TestRecordOfUnit);
    character_override_hooks_set_entries(entries.data(), entries.size());

    u1.SetHp(300, 300); RunNativeRecalc(u1, 300, kNative);
    u2.SetHp(300, 300); RunNativeRecalc(u2, 300, kNative);
    u_remote.SetHp(300, 300); RunNativeRecalc(u_remote, 300, kNative);
    u_unknown.SetHp(300, 300); RunNativeRecalc(u_unknown, 300, kNative);

    const CharacterStats local_a =
        With(With(kNative, CHARACTER_OVERRIDE_STRENGTH, 1000), CHARACTER_OVERRIDE_CONSTITUTION, 1100);
    const CharacterStats local_b =
        With(With(kNative, CHARACTER_OVERRIDE_STRENGTH, 2000), CHARACTER_OVERRIDE_CONSTITUTION, 2100);
    CHECK(u1.Stats() == local_a && u1.EffectiveStats() == local_a);
    CHECK(u2.Stats() == local_b && u2.EffectiveStats() == local_b);
    CHECK(u_remote.Stats() == kNative && u_remote.EffectiveStats() == kNative);
    CHECK(u_unknown.Stats() == kNative && u_unknown.EffectiveStats() == kNative);

    // An enemy whose own data reads as Player 1's slot keeps its stats in every hook.
    FakeUnit enemy;
    enemy.SetVirtuals(kEnemyVirtuals);
    enemy.SetSlot(0);
    g_test_unit_to_record_unit[3] = enemy.Addr();
    g_test_unit_to_record_seat[3] = s1.Addr();
    enemy.SetHp(50, 50);
    RunNativeRecalc(enemy, 90, kNative);
    CHECK(enemy.Stats() == kNative && enemy.EffectiveStats() == kNative && enemy.MaxHp() == 90);
    GuestRegs enemy_regs{};
    enemy_regs.rbx = enemy_regs.r14 = enemy.Addr();
    character_override_bundle_stats(&enemy_regs);
    character_override_effect_stats(&enemy_regs);
    CHECK(enemy.EffectiveStats() == kNative);
    enemy.SetVirtuals(kPlayerVirtuals);
    character_override_bundle_stats(&enemy_regs);
    CHECK(enemy.EffectiveStats() == local_a);
    character_override_hooks_set_player_update(0);
    u1.SetHp(300, 300); RunNativeRecalc(u1, 300, kNative);
    CHECK(u1.Stats() == kNative);
    character_override_hooks_set_player_update(kPlayerUpdate);

    FakeSavedRecord saved0, saved1, unknown;
    saved0.Init(CharacterClass::kFighter, 0x31);
    saved1.Init(CharacterClass::kFighter, 0x61);
    unknown.Init(CharacterClass::kFighter, 0x99);
    FakeStatusViewer viewer;
    viewer.SetRecord(saved0.Addr());
    RunStatusRefresh(viewer, 300, kNative);
    CHECK(viewer.Stats().strength == 1000 && viewer.Stats().constitution == 1100);
    viewer.SetRecord(saved1.Addr());
    RunStatusRefresh(viewer, 300, kNative);
    CHECK(viewer.Stats().strength == 2000 && viewer.Stats().constitution == 2100);
    viewer.SetRecord(unknown.Addr());
    RunStatusRefresh(viewer, 300, kNative);
    CHECK(viewer.Stats() == kNative);
    viewer.SetRecord(0);
    RunStatusRefresh(viewer, 300, kNative);
    CHECK(viewer.Stats() == kNative);

    // Version 1 HP-only sidecars still load and save as the current version without losing the HP override.
    const fs::path v1_path = root / "v1" / "character_overrides.dat";
    std::vector<uint8_t> v1 = {0x44, 0x43, 0x48, 0x4F, 1, 0, 0, 0, 1, 0, 0, 0};
    const std::string v1_key = "CUSA10209|SAVEDATA00|0|0|0";
    v1.push_back(uint8_t(v1_key.size()));
    v1.insert(v1.end(), v1_key.begin(), v1_key.end());
    v1.push_back(0x2C);
    v1.push_back(0x01);
    WriteAll(v1_path, v1);
    CharacterOverrideStore v1_store(v1_path);
    CHECK(v1_store.Load());
    const std::optional<CharacterOverride> loaded_v1 = v1_store.Get(CustomKey{"CUSA10209", "SAVEDATA00", 0, 0, CharacterClass::kFighter});
    CHECK(loaded_v1 && loaded_v1->present == CHARACTER_OVERRIDE_MAX_HP && loaded_v1->max_hp == 300);
    CHECK(v1_store.Save());
    CharacterOverrideStore v2_store(v1_path);
    CHECK(v2_store.Load());
    const std::optional<CharacterOverride> loaded_v2 = v2_store.Get(CustomKey{"CUSA10209", "SAVEDATA00", 0, 0, CharacterClass::kFighter});
    CHECK(loaded_v2 && loaded_v2->present == CHARACTER_OVERRIDE_MAX_HP && loaded_v2->max_hp == 300);

    const fs::path corrupt_path = root / "corrupt" / "character_overrides.dat";
    const std::vector<uint8_t> junk = {'B', 'A', 'D', '_', 'D', 'A', 'T', 'A'};
    WriteAll(corrupt_path, junk);
    CharacterOverrideStore corrupt(corrupt_path);
    CHECK(!corrupt.Load() && !corrupt.error().empty());
    CHECK(!corrupt.Save());
    CHECK(ReadFile(corrupt_path) == junk);

    CharacterOverrideStore missing(root / "missing" / "character_overrides.dat");
    CHECK(missing.Load() && missing.entries().empty());

    const fs::path blocked_user = root / "corrupt_user";
    const fs::path blocked_save = blocked_user / "savedata" / "1" / "CUSA10209" / "SAVEDATA00" / "SAVE0.DAT";
    std::vector<uint8_t> blocked_bytes = MakeSave();
    WriteCharacter(blocked_bytes, 0, 0, "HERO", CharacterClass::kFighter, 9, 300, 300, 0x41, kNative);
    FinalizeAndWrite(blocked_save, blocked_bytes);
    WriteAll(CharacterOverrideStorePath(blocked_user), junk);
    SaveEdit blocked_edit(DiscoverSaves(blocked_user, "CUSA10209")[0], BackupFolder(blocked_user));
    CustomStore blocked_custom(CustomStorePath(blocked_user));
    CharacterOverrideStore blocked_override(CharacterOverrideStorePath(blocked_user));
    CHECK(!blocked_override.Load() && !blocked_override.error().empty());
    CHECK(blocked_edit.working().SetHp(0, 0, 123, 300));
    const ApplyOutcome unrelated = ApplyEditor(blocked_edit, blocked_custom, blocked_override, "CUSA10209", "SAVEDATA00");
    CHECK(unrelated.ok() && !unrelated.override_written);
    CHECK(SaveModel::FromFile(blocked_save).Character(0, 0)->hp_current == 123);
    CHECK(ReadFile(CharacterOverrideStorePath(blocked_user)) == junk);

    const std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10209");
    SaveEdit edit(saves[0], BackupFolder(user));
    CustomStore cs_saved(CustomStorePath(user));
    CharacterOverrideStore os_saved(CharacterOverrideStorePath(user));
    CHECK(os_saved.Load());
    CHECK(edit.working().SetHp(0, 0, 300, 5000));
    WriteCharacter(raw, 0, 0, "HERO", CharacterClass::kFighter, 10, 310, 310, 0x31, kNative);
    FinalizeAndWrite(save_path, raw);
    const ApplyOutcome stale = ApplyEditor(edit, cs_saved, os_saved, "CUSA10209", "SAVEDATA00");
    CHECK(!stale.ok() && stale.save == SaveEdit::Result::kStale && stale.rolled_back);
    CharacterOverrideStore after_rollback(CharacterOverrideStorePath(user));
    CHECK(after_rollback.Load() && after_rollback.Get(key1)->strength == 1000);

    character_override_hooks_set_entries(nullptr, 0);
}

// The override follows the character it was made for, not the slot: a new character of the same class in that slot, an
// import or a restore keeps its native stats.
void TestOverrideStaysWithItsCharacter(const fs::path& root) {
    const fs::path user = root / "bound_user";
    const fs::path save_path = user / "savedata" / "1" / "CUSA10209" / "SAVEDATA00" / "SAVE0.DAT";
    const CustomKey key{"CUSA10209", "SAVEDATA00", 0, 0, CharacterClass::kFighter};
    const auto write = [&](const char* name, uint8_t seed) {
        std::vector<uint8_t> raw = MakeSave();
        WriteCharacter(raw, 0, 0, name, CharacterClass::kFighter, 9, 300, 300, seed, kNative);
        FinalizeAndWrite(save_path, raw);
    };
    const auto planned = [&] {
        CharacterOverrideStore store(CharacterOverrideStorePath(user));
        CHECK(store.Load());
        std::vector<std::string> notes;
        return PlanCharacterOverrides(store, DiscoverSaves(user, "CUSA10209"), "CUSA10209", notes);
    };
    write("HERO", 0x21);
    CustomStore custom(CustomStorePath(user));
    CharacterOverrideStore saved(CharacterOverrideStorePath(user));
    CHECK(saved.Load());
    {
        SaveEdit edit(DiscoverSaves(user, "CUSA10209")[0], BackupFolder(user));
        CharacterStats stats = kNative;
        stats.strength = 900;
        CHECK(edit.working().SetStats(0, 0, stats));
        CHECK(ApplyEditor(edit, custom, saved, "CUSA10209", "SAVEDATA00").ok());
    }
    CHECK(saved.Get(key) && saved.Get(key)->Bound() && planned().size() == 1);
    write("RENAMED", 0x21);
    CHECK(planned().size() == 1);
    write("OTHER", 0x71);
    CHECK(planned().empty());

    // Editing the replacement drops what belonged to the old character.
    SaveEdit edit(DiscoverSaves(user, "CUSA10209")[0], BackupFolder(user));
    CharacterStats stats = kNative;
    stats.luck = 400;
    CHECK(edit.working().SetStats(0, 0, stats));
    CHECK(ApplyEditor(edit, custom, saved, "CUSA10209", "SAVEDATA00").ok());
    const std::vector<CharacterOverrideEntry> entries = planned();
    CHECK(entries.size() == 1 && entries[0].present == CHARACTER_OVERRIDE_LUCK && entries[0].luck == 400);

    std::vector<uint8_t> bytes = saved.Serialize();
    CharacterOverrideStore parsed(root / "unused.dat");
    CHECK(parsed.Parse(bytes) && parsed == saved);
    bytes.pop_back();
    CHECK(!parsed.Parse(bytes));
    bytes = saved.Serialize();
    const size_t present_at = 12 + 1 + key.Text().size();
    bytes[present_at] = CHARACTER_OVERRIDE_MAX_HP;
    CHECK(!parsed.Parse(bytes)); // a Max HP of zero

    // Version 2 has no identity: it still loads, and the next Apply binds it.
    std::vector<uint8_t> v2 = saved.Serialize();
    v2[4] = 2;
    v2.resize(v2.size() - kCharIdentityBytes);
    WriteAll(CharacterOverrideStorePath(user), v2);
    CharacterOverrideStore legacy(CharacterOverrideStorePath(user));
    CHECK(legacy.Load() && legacy.Get(key) && !legacy.Get(key)->Bound() && planned().size() == 1);
    SyncCharacterOverrides(legacy, edit.working(), edit.persisted(), "CUSA10209", "SAVEDATA00");
    CHECK(legacy.Get(key)->Bound() && legacy.Get(key)->luck == 400);
}

// Minimal PS4 SELF parser to extract the executable PT_LOAD segment from real eboot.bin files.
bool ExtractSelfText(const fs::path& eboot_path, std::vector<uint8_t>& text_out, uint64_t& entry_out) {
    const std::vector<uint8_t> data = ReadFile(eboot_path);
    if (data.size() < 64 || std::memcmp(data.data(), "O\x15=\x1d", 4) != 0) return false;
    const uint16_t seg_count = RawU16(data, 24);
    const size_t elf_base = 32 + size_t(seg_count) * 32;
    if (elf_base + 64 > data.size() || std::memcmp(data.data() + elf_base, "\x7f""ELF\x02\x01\x01", 7) != 0) return false;
    uint64_t entry = 0, phoff = 0;
    std::memcpy(&entry, data.data() + elf_base + 24, 8);
    std::memcpy(&phoff, data.data() + elf_base + 32, 8);
    const uint16_t phnum = RawU16(data, elf_base + 56);
    entry_out = entry;
    for (uint16_t i = 0; i < seg_count; ++i) {
        uint64_t flags = 0, off = 0, sz = 0;
        std::memcpy(&flags, data.data() + 32 + i * 32, 8);
        std::memcpy(&off, data.data() + 32 + i * 32 + 8, 8);
        std::memcpy(&sz, data.data() + 32 + i * 32 + 16, 8);
        if (!(flags & 0x800)) continue;
        const size_t ph_idx = size_t((flags >> 20) & 4095);
        if (ph_idx >= phnum) continue;
        const size_t ph_at = elf_base + size_t(phoff) + ph_idx * 56;
        if (ph_at + 56 > data.size()) continue;
        const uint32_t p_type = RawU32(data, ph_at);
        const uint32_t p_flags = RawU32(data, ph_at + 4);
        if (p_type == 1 && (p_flags & 1)) {
            if (off + sz > data.size()) return false;
            text_out.assign(data.begin() + std::ptrdiff_t(off), data.begin() + std::ptrdiff_t(off + sz));
            return true;
        }
    }
    return false;
}

bool ContainsPattern(const std::vector<uint8_t>& buf, const uint8_t* pat, size_t len) {
    if (len > buf.size()) return false;
    for (size_t i = 0; i + len <= buf.size(); ++i) {
        if (buf[i] == pat[0] && std::memcmp(buf.data() + i, pat, len) == 0) return true;
    }
    return false;
}

void TestNativeExecutableEvidence() {
    const fs::path src_dir(DCP_SOURCE_DIR);
    const std::array<std::pair<const char*, fs::path>, 6> builds = {{
        {"CUSA10209-1.00", src_dir / "game" / "prepared" / "CUSA10209-1.00" / "eboot.bin"},
        {"CUSA07001-1.00", src_dir / "game" / "prepared" / "CUSA07001-1.00" / "eboot.bin"},
        {"CUSA07001-1.02", src_dir / "game" / "prepared" / "CUSA07001-1.02" / "eboot.bin"},
        {"CUSA07001-1.03", src_dir / "game" / "prepared" / "CUSA07001-1.03" / "eboot.bin"},
        {"CUSA10454-1.00", src_dir / "game" / "extracted" / "base" / "eboot.bin"},
        {"v1.03",          src_dir / "game" / "prepared" / "v1.03" / "eboot.bin"},
    }};
    const uint8_t kNormalOverwrite[] = {0x8b, 0x08, 0x41, 0x89, 0x4f, 0x10, 0x41, 0x89, 0x4f, 0x0c};
    const uint8_t kUltOverwrite[] = {0x8b, 0x08, 0x89, 0x4b, 0x10, 0x89, 0x4b, 0x0c};
    const uint8_t kVtable9f0Call[] = {0xff, 0x90, 0xf0, 0x09, 0x00, 0x00};
    // Gameplay stat accessors read the same six block fields the override writes.
    const uint8_t kAccessorStrength[] = {0x41, 0x0f, 0xb7, 0x86, 0xbc, 0x00, 0x00, 0x00};
    const uint8_t kAccessorIntelligence[] = {0x41, 0x0f, 0xb7, 0x86, 0xbe, 0x00, 0x00, 0x00};
    const uint8_t kAccessorDexterity[] = {0x41, 0x0f, 0xb7, 0x86, 0xc0, 0x00, 0x00, 0x00};
    const uint8_t kAccessorLuck[] = {0x41, 0x0f, 0xb7, 0x86, 0xc2, 0x00, 0x00, 0x00};
    const uint8_t kAccessorConstitution[] = {0x41, 0x0f, 0xb7, 0x86, 0xc4, 0x00, 0x00, 0x00};
    const uint8_t kAccessorMagicResistance[] = {0x41, 0x0f, 0xb7, 0x86, 0xc6, 0x00, 0x00, 0x00};
    // 0xec4da: the runtime stat sync copies all six effective fields into a unit's status block.
    const uint8_t kStatSync[] = {
        0x66, 0x45, 0x89, 0x85, 0xbc, 0x00, 0x00, 0x00, 0x66, 0x41, 0x89, 0x95, 0xbe, 0x00, 0x00, 0x00,
        0x66, 0x41, 0x89, 0x8d, 0xc0, 0x00, 0x00, 0x00, 0x66, 0x41, 0x89, 0xb5, 0xc2, 0x00, 0x00, 0x00,
        0x66, 0x41, 0x89, 0x85, 0xc4, 0x00, 0x00, 0x00, 0x66, 0x41, 0x89, 0xbd, 0xc6, 0x00, 0x00, 0x00,
    };
    // 0xe5780: the live damage path consumes the attacker's STR/INT/DEX and the defender's MGR/CON bundles.
    const uint8_t kDamageStrength[] = {0x8b, 0x83, 0x0c, 0x01, 0x00, 0x00};
    const uint8_t kDamageIntelligence[] = {0x8b, 0x83, 0x10, 0x01, 0x00, 0x00};
    const uint8_t kDamageDexterity[] = {0xc5, 0xfa, 0x59, 0x83, 0x14, 0x01, 0x00, 0x00};
    const uint8_t kDamageMagicResistance[] = {0x41, 0x8b, 0x85, 0x18, 0x01, 0x00, 0x00};
    const uint8_t kDamageConstitution[] = {0x41, 0x8b, 0x85, 0x1c, 0x01, 0x00, 0x00};
    // 0xe636b: the critical roll compares the attacker's LUC (and the target's) before the percent roll.
    const uint8_t kCriticalLuck[] = {0xc4, 0xc1, 0x7a, 0x10, 0x8e, 0x20, 0x01, 0x00, 0x00};
    const uint8_t kCriticalTargetLuck[] = {0xc5, 0xfa, 0x10, 0x80, 0x20, 0x01, 0x00, 0x00};
    // Status getters read the block's six fields before equipment bonuses are added.
    const uint8_t kStatusStrengthSource[] = {0x0f, 0xb7, 0x47, 0x18};
    const uint8_t kStatusIntelligenceSource[] = {0x0f, 0xb7, 0x47, 0x1a};
    const uint8_t kStatusDexteritySource[] = {0x0f, 0xb7, 0x47, 0x1c};
    const uint8_t kStatusLuckSource[] = {0x0f, 0xb7, 0x47, 0x1e};
    const uint8_t kStatusConstitutionSource[] = {0x0f, 0xb7, 0x47, 0x20};
    const uint8_t kStatusMagicResistanceSource[] = {0x0f, 0xb7, 0x47, 0x22};

    int verified = 0;
    bool usa_100 = false;
    for (const auto& [label, path] : builds) {
        if (!fs::exists(path)) continue;
        std::vector<uint8_t> text;
        uint64_t entry = 0;
        CHECK(ExtractSelfText(path, text, entry));
        CharacterOverrideSites sites{};
        CHECK(character_override_hooks_locate(text.data(), text.size(), &sites));
        CHECK(sites.status_stats > sites.status_max && sites.status_stats - sites.status_max < 0x200);
        CHECK(sites.bundle_stats != 0 && sites.effect_stats != 0);
        CHECK(ContainsPattern(text, kVtable9f0Call, sizeof(kVtable9f0Call)));
        CHECK(ContainsPattern(text, kNormalOverwrite, sizeof(kNormalOverwrite)));
        CHECK(ContainsPattern(text, kUltOverwrite, sizeof(kUltOverwrite)));
        CHECK(ContainsPattern(text, kAccessorStrength, sizeof(kAccessorStrength)));
        CHECK(ContainsPattern(text, kAccessorIntelligence, sizeof(kAccessorIntelligence)));
        CHECK(ContainsPattern(text, kAccessorDexterity, sizeof(kAccessorDexterity)));
        CHECK(ContainsPattern(text, kAccessorLuck, sizeof(kAccessorLuck)));
        CHECK(ContainsPattern(text, kAccessorConstitution, sizeof(kAccessorConstitution)));
        CHECK(ContainsPattern(text, kAccessorMagicResistance, sizeof(kAccessorMagicResistance)));
        CHECK(ContainsPattern(text, kStatSync, sizeof(kStatSync)));
        CHECK(ContainsPattern(text, kDamageStrength, sizeof(kDamageStrength)));
        CHECK(ContainsPattern(text, kDamageIntelligence, sizeof(kDamageIntelligence)));
        CHECK(ContainsPattern(text, kDamageDexterity, sizeof(kDamageDexterity)));
        CHECK(ContainsPattern(text, kDamageMagicResistance, sizeof(kDamageMagicResistance)));
        CHECK(ContainsPattern(text, kDamageConstitution, sizeof(kDamageConstitution)));
        CHECK(ContainsPattern(text, kCriticalLuck, sizeof(kCriticalLuck)));
        CHECK(ContainsPattern(text, kCriticalTargetLuck, sizeof(kCriticalTargetLuck)));
        CHECK(ContainsPattern(text, kStatusStrengthSource, sizeof(kStatusStrengthSource)));
        CHECK(ContainsPattern(text, kStatusIntelligenceSource, sizeof(kStatusIntelligenceSource)));
        CHECK(ContainsPattern(text, kStatusDexteritySource, sizeof(kStatusDexteritySource)));
        CHECK(ContainsPattern(text, kStatusLuckSource, sizeof(kStatusLuckSource)));
        CHECK(ContainsPattern(text, kStatusConstitutionSource, sizeof(kStatusConstitutionSource)));
        CHECK(ContainsPattern(text, kStatusMagicResistanceSource, sizeof(kStatusMagicResistanceSource)));

        const DcpExecRange range{0, text.size()};
        const DcpImageView view{text.data(), text.size(), &range, 1};
        DcpPartySites party{};
        DcpLocateReport report{};
        CHECK(dcp_locate_party_sites(&view, &party, &report));
        CHECK(party.tick != 0 && party.unit_slot == 0xa70);
        sites.player_update = party.tick;

        const size_t stub_bytes = character_override_hooks_stub_bytes();
        std::vector<uint8_t> combined(text.size() + stub_bytes, 0);
        std::memcpy(combined.data(), text.data(), text.size());
        CHECK(character_override_hooks_install(combined.data(), text.size(), entry, &sites, combined.data() + text.size(),
                                               stub_bytes));
        ++verified;
        if (std::string_view(label) == "CUSA10454-1.00") usa_100 = true;
        std::printf("F3K8S native parity %s: spawn=0x%llx recalc_entry=0x%llx recalc_post=0x%llx status_max=0x%llx "
                    "status_stats=0x%llx PASS\n",
                    label, static_cast<unsigned long long>(sites.spawn_load),
                    static_cast<unsigned long long>(sites.recalc_entry),
                    static_cast<unsigned long long>(sites.recalc_post),
                    static_cast<unsigned long long>(sites.status_max),
                    static_cast<unsigned long long>(sites.status_stats));
    }
    if (verified > 0) {
        CHECK(fs::exists(builds[4].second));
        CHECK(usa_100);
    }
    std::vector<uint8_t> bogus(4096, 0x90);
    CharacterOverrideSites bad_sites{};
    CHECK(!character_override_hooks_locate(bogus.data(), bogus.size(), &bad_sites));
    std::vector<uint8_t> stubs(character_override_hooks_stub_bytes(), 0);
    CHECK(!character_override_hooks_install(bogus.data(), bogus.size(), 0, &bad_sites, stubs.data(), stubs.size()));
    std::printf("F3K8S native builds verified: %d\n", verified);
}

} // namespace

int main() {
    const fs::path temp = fs::temp_directory_path() / "dcp_character_override_tests";
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);

    character_override_hooks_set_player_update(kPlayerUpdate);
    TestMaxHpRegressionAndAntiInfiniteHealth(temp);
    TestEveryStatLiveAndStatus(temp);
    TestNameOnlyApplyKeepsNativeAndExisting(temp);
    TestIsolationIdentityRemoteAndSidecar(temp);
    TestOverrideStaysWithItsCharacter(temp);
    TestNativeExecutableEvidence();

    fs::remove_all(temp, ec);
    if (g_failures == 0) {
        std::printf("F3K8S character_override_tests: PASS (%d checks)\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "F3K8S character_override_tests: FAIL (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
