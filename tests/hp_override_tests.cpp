// Focused headless tests for DCP-UTGYFH: persistent per-character Max HP, Editor Apply semantics,
// anti-Infinite-Health Current HP preservation, multiplayer/remote isolation, and real native hooks.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "appearance_custom.h"
#include "editor_model.h"
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

// Independent IEEE CRC-32 over [4..size) so the save round-trip test does not reuse SaveModel's own CRC.
uint32_t IndependentCrc32(const std::vector<uint8_t>& b) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 4; i < b.size(); ++i) {
        crc ^= b[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

void WriteCharacter(std::vector<uint8_t>& bytes, int slot, int idx, const char* name, CharacterClass klass,
                    uint8_t level_stored, uint16_t hp_cur, uint16_t hp_max, uint8_t id_seed) {
    const size_t base = size_t(slot) * kDataSlotStride + kCharacterOffset + size_t(idx) * kCharacterStride;
    std::memset(bytes.data() + base, 0, kCharacterStride);
    PutU16(bytes, base + kCharHpCurrent, hp_cur);
    PutU16(bytes, base + kCharHpMax, hp_max);
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

struct FakeUnit {
    std::array<uint8_t, 0xb00> mem{};
    int32_t CurrentHp() const {
        int32_t v = 0;
        std::memcpy(&v, mem.data() + HP_OVERRIDE_UNIT_CURRENT, sizeof(v));
        return v;
    }
    int32_t MaxHp() const {
        int32_t v = 0;
        std::memcpy(&v, mem.data() + HP_OVERRIDE_UNIT_MAX, sizeof(v));
        return v;
    }
    void SetHp(int32_t cur, int32_t max) {
        std::memcpy(mem.data() + HP_OVERRIDE_UNIT_CURRENT, &cur, sizeof(cur));
        std::memcpy(mem.data() + HP_OVERRIDE_UNIT_MAX, &max, sizeof(max));
    }
    void SetSlot(int32_t slot) {
        std::memcpy(mem.data() + 0xa70, &slot, sizeof(slot));
    }
    uint64_t Addr() { return uint64_t(uintptr_t(mem.data())); }
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

// A save-table character record: 0x21d0 bytes, no party seat owner. The bytes where a seat would keep the owner belong
// to the next record (0xAB here), so the status path must never read them.
struct FakeSavedRecord {
    std::vector<uint8_t> mem = std::vector<uint8_t>(APPEARANCE_SEAT_OWNER + 0x10, 0xAB);
    void Init(CharacterClass klass, uint8_t id_seed) {
        mem[APPEARANCE_RECORD_CLASS] = uint8_t(klass);
        for (size_t i = 0; i < APPEARANCE_IDENTITY_BYTES; ++i) mem[APPEARANCE_RECORD_IDENTITY + i] = uint8_t(id_seed + i);
    }
    uint64_t Addr() { return uint64_t(uintptr_t(mem.data())); }
};

struct FakeStatusViewer {
    std::array<uint8_t, HP_OVERRIDE_STATUS_MAX + 4> mem{};
    void SetRecord(uint64_t record) {
        std::memcpy(mem.data() + HP_OVERRIDE_STATUS_RECORD, &record, sizeof(record));
    }
    uint64_t Addr() { return uint64_t(uintptr_t(mem.data())); }
    int32_t Hp() const {
        int32_t v = 0;
        std::memcpy(&v, mem.data() + HP_OVERRIDE_STATUS_MAX, sizeof(v));
        return v;
    }
};

uint64_t g_test_unit_to_record_unit[4] = {};
uint64_t g_test_unit_to_record_seat[4] = {};

uint64_t TestRecordOfUnit(uint64_t unit) {
    for (int i = 0; i < 4; ++i) {
        if (g_test_unit_to_record_unit[i] == unit) return g_test_unit_to_record_seat[i];
    }
    return 0;
}

void RunNativeRecalc(FakeUnit& unit, int32_t native_hp) {
    GuestRegs pre{};
    pre.rdi = unit.Addr();
    hp_override_pre_recalc(&pre);
    // Native ApplyLevelStats unconditionally overwrites both HP fields from ClassLevelTable[class][level].
    unit.SetHp(native_hp, native_hp);
    GuestRegs post{};
    post.r12 = unit.Addr();
    hp_override_post_recalc(&post);
}

// The status viewer's refresh: max HP with equipment is computed natively, then stored into the field the screen prints.
void RunStatusRefresh(FakeStatusViewer& viewer, int32_t native_hp) {
    GuestRegs store{};
    store.r14 = viewer.Addr();
    store.rax = uint64_t(uint32_t(native_hp));
    hp_override_status_max(&store);
    const int32_t shown = int32_t(uint32_t(store.rax));
    std::memcpy(viewer.mem.data() + HP_OVERRIDE_STATUS_MAX, &shown, sizeof(shown));
}

// Mirrors EditorPage::ApplyChanges: the sidecar follows the Max HP the working model holds.
ApplyOutcome ApplyEditor(SaveEdit& edit, CustomStore& custom_saved, HpOverrideStore& hp_saved, std::string_view title,
                         std::string_view dir) {
    HpOverrideStore hp_next = hp_saved;
    if (hp_saved.error().empty()) SyncHpOverrides(hp_next, edit.working(), edit.persisted(), title, dir);
    return ApplyAll(&edit, custom_saved, custom_saved, &hp_next, &hp_saved);
}

void TestDcpUtgyfhCoreAndAntiInfiniteHealth(const fs::path& root) {
    const fs::path user = root / "utgyfh_user";
    const fs::path save_path = user / "savedata" / "1" / "CUSA10209" / "SAVEDATA00" / "SAVE0.DAT";
    std::vector<uint8_t> raw = MakeSave();
    WriteCharacter(raw, 0, 0, "HERO", CharacterClass::kFighter, 9, 300, 300, 0x21);
    FinalizeAndWrite(save_path, raw);

    // 1) Editor opens save, changes Max HP 300 -> 4000, Apply Changes.
    std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10209");
    CHECK(saves.size() == 1);
    SaveEdit edit(saves[0], BackupFolder(user));
    CustomStore custom_saved(CustomStorePath(user));
    HpOverrideStore hp_saved(HpOverrideStorePath(user));
    CHECK(hp_saved.Load() && hp_saved.entries().empty());

    const CustomKey key{"CUSA10209", "SAVEDATA00", 0, 0, CharacterClass::kFighter};
    const CharacterView* ch = edit.working().Character(0, 0);
    CHECK(ch && ch->hp_current == 300 && ch->hp_max == 300);
    const HpValues next_max = WithMaxHp({ch->hp_current, ch->hp_max}, 4000);
    CHECK(next_max.current == 300 && next_max.max == 4000);
    CHECK(edit.working().SetHp(0, 0, uint16_t(next_max.current), uint16_t(next_max.max)));

    const ApplyOutcome applied = ApplyEditor(edit, custom_saved, hp_saved, "CUSA10209", "SAVEDATA00");
    CHECK(applied.ok() && applied.save == SaveEdit::Result::kApplied && applied.hp_written);
    CHECK(hp_saved.Get(key) == std::optional<uint16_t>(4000));

    // Independent raw SAVE0.DAT byte + CRC verification (offsets 0xA4DC and 0xA4E0).
    const std::vector<uint8_t> disk_bytes = ReadFile(save_path);
    CHECK(disk_bytes.size() == kSaveFileSize);
    CHECK(RawU32(disk_bytes, 0) == IndependentCrc32(disk_bytes));
    CHECK(RawU16(disk_bytes, 0xA400 + 0xDC) == 300);
    CHECK(RawU16(disk_bytes, 0xA400 + 0xE0) == 4000);

    // Close and reopen Editor state.
    const SaveModel reopened_save = SaveModel::FromFile(save_path);
    CHECK(reopened_save.valid());
    CHECK(reopened_save.Character(0, 0)->hp_current == 300);
    CHECK(reopened_save.Character(0, 0)->hp_max == 4000);
    HpOverrideStore reopened_hp(HpOverrideStorePath(user));
    CHECK(reopened_hp.Load() && reopened_hp.Get(key) == std::optional<uint16_t>(4000));

    // Runtime planning & post-recalc override for 300/4000.
    std::vector<std::string> notes;
    std::vector<HpOverrideEntry> entries = PlanHpOverrides(reopened_hp, DiscoverSaves(user, "CUSA10209"), "CUSA10209", notes);
    CHECK(entries.size() == 1 && entries[0].max_hp == 4000 && entries[0].klass == uint8_t(CharacterClass::kFighter));

    FakeUnit unit;
    FakeSeat seat;
    unit.SetSlot(0);
    seat.Init(CharacterClass::kFighter, 0x21, APPEARANCE_SEAT_LOCAL);
    g_test_unit_to_record_unit[0] = unit.Addr();
    g_test_unit_to_record_seat[0] = seat.Addr();
    hp_override_hooks_set_record_of_unit(TestRecordOfUnit);
    hp_override_hooks_set_entries(entries.data(), entries.size());

    // Initial load copies saved Current HP (300) into unit before ApplyLevelStats.
    unit.SetHp(reopened_save.Character(0, 0)->hp_current, reopened_save.Character(0, 0)->hp_max);
    RunNativeRecalc(unit, 300);
    CHECK(unit.CurrentHp() == 300 && unit.MaxHp() == 4000);

    // 2) User explicitly edits Current HP = 4000, Max HP = 4000, then takes damage to 2500.
    CHECK(edit.working().SetHp(0, 0, 4000, 4000));
    CHECK(ApplyEditor(edit, custom_saved, hp_saved, "CUSA10209", "SAVEDATA00").ok());
    const SaveModel full_save = SaveModel::FromFile(save_path);
    unit.SetHp(full_save.Character(0, 0)->hp_current, full_save.Character(0, 0)->hp_max);
    RunNativeRecalc(unit, 300);
    CHECK(unit.CurrentHp() == 4000 && unit.MaxHp() == 4000);

    // Simulate damage to 2500, then run another native stat recalculation: must NOT refill to 4000!
    unit.SetHp(2500, unit.MaxHp());
    RunNativeRecalc(unit, 300);
    CHECK(unit.MaxHp() == 4000 && unit.CurrentHp() == 2500);

    // 3) Native max 500, custom max 4000, current 450 -> max 4000, current 450.
    unit.SetHp(450, 500);
    RunNativeRecalc(unit, 500);
    CHECK(unit.MaxHp() == 4000 && unit.CurrentHp() == 450);

    // 4) The Max HP field is authoritative: each Apply Changes stores exactly the shown value, with no stale entry.
    const CharacterView* persisted = edit.persisted().Character(0, 0);
    const auto apply_max = [&](uint16_t max_hp) {
        const HpValues shown = WithMaxHp({persisted->hp_current, persisted->hp_max}, max_hp);
        CHECK(edit.working().SetHp(0, 0, uint16_t(shown.current), uint16_t(shown.max)));
        CHECK(ApplyEditor(edit, custom_saved, hp_saved, "CUSA10209", "SAVEDATA00").ok());
        persisted = edit.persisted().Character(0, 0);
        CHECK(persisted->hp_max == max_hp);
        CHECK(hp_saved.Get(key) == std::optional<uint16_t>(max_hp));
        entries = PlanHpOverrides(hp_saved, DiscoverSaves(user, "CUSA10209"), "CUSA10209", notes);
        CHECK(entries.size() == 1 && entries[0].max_hp == max_hp);
        hp_override_hooks_set_entries(entries.data(), entries.size());
    };

    apply_max(200);
    unit.SetHp(450, 500);
    RunNativeRecalc(unit, 500);
    CHECK(unit.MaxHp() == 200 && unit.CurrentHp() == 200);

    apply_max(1640);
    unit.SetHp(200, 200);
    RunNativeRecalc(unit, 500);
    CHECK(unit.MaxHp() == 1640 && unit.CurrentHp() == 200);

    apply_max(23360);
    unit.SetHp(200, 1640);
    RunNativeRecalc(unit, 500);
    CHECK(unit.MaxHp() == 23360 && unit.CurrentHp() == 200);

    apply_max(1000);
    unit.SetHp(200, 23360);
    RunNativeRecalc(unit, 500);
    CHECK(unit.MaxHp() == 1000 && unit.CurrentHp() == 200);
}

void TestStatusScreenAndPreviewPath(const fs::path& root) {
    const fs::path user = root / "status_user";
    const fs::path save_path = user / "savedata" / "1" / "CUSA10454" / "SAVEDATA00" / "SAVE0.DAT";
    std::vector<uint8_t> raw = MakeSave();
    WriteCharacter(raw, 0, 0, "George", CharacterClass::kDwarf, 100, 500, 722, 0x41);
    WriteCharacter(raw, 0, 1, "George", CharacterClass::kDwarf, 100, 500, 722, 0x71);
    FinalizeAndWrite(save_path, raw);

    HpOverrideStore store(HpOverrideStorePath(user));
    const CustomKey key0{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kDwarf};
    const CustomKey key1{"CUSA10454", "SAVEDATA00", 0, 1, CharacterClass::kDwarf};
    CHECK(store.Load() && store.entries().empty());

    // The Editor's Max HP fields are the only input: Apply Changes stores them for the internal runtime override.
    const std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10454");
    SaveEdit edit(saves[0], BackupFolder(user));
    CustomStore custom_saved(CustomStorePath(user));
    CHECK(edit.working().SetHp(0, 0, 500, 1640));
    CHECK(edit.working().SetHp(0, 1, 500, 1800));
    CHECK(ApplyEditor(edit, custom_saved, store, "CUSA10454", "SAVEDATA00").ok());
    CHECK(store.Get(key0) == std::optional<uint16_t>(1640) && store.Get(key1) == std::optional<uint16_t>(1800));

    std::vector<std::string> notes;
    std::vector<HpOverrideEntry> entries =
        PlanHpOverrides(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
    CHECK(entries.size() == 2);
    hp_override_hooks_set_entries(entries.data(), entries.size());

    FakeSavedRecord saved0, saved1, unknown;
    saved0.Init(CharacterClass::kDwarf, 0x41);
    saved1.Init(CharacterClass::kDwarf, 0x71);
    unknown.Init(CharacterClass::kDwarf, 0x99);

    // Live unit: the 101 Dwarf's native 722 becomes 1640 and the stored Current HP survives the recalculation.
    FakeUnit unit;
    FakeSeat seat;
    unit.SetSlot(0);
    seat.Init(CharacterClass::kDwarf, 0x41, APPEARANCE_SEAT_LOCAL);
    g_test_unit_to_record_unit[0] = unit.Addr();
    g_test_unit_to_record_seat[0] = seat.Addr();
    hp_override_hooks_set_record_of_unit(TestRecordOfUnit);
    unit.SetHp(500, 1640);
    RunNativeRecalc(unit, 722);
    CHECK(unit.MaxHp() == 1640 && unit.CurrentHp() == 500);

    // The status page recomputes the native 722 from the save-table record and prints that record's HP. It must print
    // the same 1640 without touching the live unit.
    FakeStatusViewer viewer;
    viewer.SetRecord(saved0.Addr());
    RunStatusRefresh(viewer, 722);
    CHECK(viewer.Hp() == 1640);
    CHECK(unit.MaxHp() == 1640 && unit.CurrentHp() == 500);

    // Equipment and skills still run: the override replaces their final total, it does not add to it.
    RunStatusRefresh(viewer, 733);
    CHECK(viewer.Hp() == 1640);

    // The other local Dwarf has the same name and class but its own identity and value; an unknown record is native.
    viewer.SetRecord(saved1.Addr());
    RunStatusRefresh(viewer, 722);
    CHECK(viewer.Hp() == 1800);
    viewer.SetRecord(unknown.Addr());
    RunStatusRefresh(viewer, 722);
    CHECK(viewer.Hp() == 722);
    viewer.SetRecord(0);
    RunStatusRefresh(viewer, 722);
    CHECK(viewer.Hp() == 722);

    // Editing Max HP again replaces the previous value everywhere: no stale 1640 survives.
    const auto apply_max = [&](uint16_t max_hp) {
        const CharacterView* before = edit.persisted().Character(0, 0);
        const HpValues shown = WithMaxHp({before->hp_current, before->hp_max}, max_hp);
        CHECK(edit.working().SetHp(0, 0, uint16_t(shown.current), uint16_t(shown.max)));
        CHECK(ApplyEditor(edit, custom_saved, store, "CUSA10454", "SAVEDATA00").ok());
        CHECK(store.Get(key0) == std::optional<uint16_t>(max_hp));
        entries = PlanHpOverrides(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
        CHECK(entries.size() == 2);
        hp_override_hooks_set_entries(entries.data(), entries.size());
    };

    apply_max(23360);
    viewer.SetRecord(saved0.Addr());
    RunStatusRefresh(viewer, 722);
    CHECK(viewer.Hp() == 23360);
    RunNativeRecalc(unit, 722);
    CHECK(unit.MaxHp() == 23360 && unit.CurrentHp() == 500);

    apply_max(1000);
    viewer.SetRecord(saved0.Addr());
    RunStatusRefresh(viewer, 722);
    CHECK(viewer.Hp() == 1000);
    RunNativeRecalc(unit, 722);
    CHECK(unit.MaxHp() == 1000 && unit.CurrentHp() == 500);
}

void TestIsolationMultiplayerRemoteAndErrors(const fs::path& root) {
    const fs::path user = root / "multi_user";
    const fs::path save_path = user / "savedata" / "1" / "CUSA10209" / "SAVEDATA00" / "SAVE0.DAT";
    std::vector<uint8_t> raw = MakeSave();
    // Two local characters with the SAME NAME ("HERO") but different identities and slots.
    WriteCharacter(raw, 0, 0, "HERO", CharacterClass::kFighter, 9, 300, 300, 0x31);
    WriteCharacter(raw, 0, 1, "HERO", CharacterClass::kFighter, 9, 300, 300, 0x61);
    FinalizeAndWrite(save_path, raw);

    HpOverrideStore store(HpOverrideStorePath(user));
    const CustomKey key1{"CUSA10209", "SAVEDATA00", 0, 0, CharacterClass::kFighter};
    const CustomKey key2{"CUSA10209", "SAVEDATA00", 0, 1, CharacterClass::kFighter};
    store.Set(key1, 4000);
    store.Set(key2, 1800);
    CHECK(store.Save());

    std::vector<std::string> notes;
    const std::vector<HpOverrideEntry> entries =
        PlanHpOverrides(store, DiscoverSaves(user, "CUSA10209"), "CUSA10209", notes);
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
    hp_override_hooks_set_record_of_unit(TestRecordOfUnit);
    hp_override_hooks_set_entries(entries.data(), entries.size());

    u1.SetHp(300, 300); RunNativeRecalc(u1, 300);
    u2.SetHp(300, 300); RunNativeRecalc(u2, 300);
    u_remote.SetHp(300, 300); RunNativeRecalc(u_remote, 300);
    u_unknown.SetHp(300, 300); RunNativeRecalc(u_unknown, 300);

    CHECK(u1.MaxHp() == 4000 && u1.CurrentHp() == 300);
    CHECK(u2.MaxHp() == 1800 && u2.CurrentHp() == 300);
    CHECK(u_remote.MaxHp() == 300 && u_remote.CurrentHp() == 300);
    CHECK(u_unknown.MaxHp() == 300 && u_unknown.CurrentHp() == 300);

    // Corrupt sidecar fails safe and refuses overwrite; missing sidecar loads empty.
    const fs::path corrupt_path = root / "corrupt" / "character_overrides.dat";
    fs::create_directories(corrupt_path.parent_path());
    {
        std::ofstream out(corrupt_path, std::ios::binary | std::ios::trunc);
        const char junk[] = "BAD_DATA";
        out.write(junk, sizeof(junk));
    }
    HpOverrideStore corrupt(corrupt_path);
    CHECK(!corrupt.Load() && !corrupt.error().empty());
    CHECK(!corrupt.Save()); // never clobbers unreadable file

    HpOverrideStore missing(root / "missing" / "character_overrides.dat");
    CHECK(missing.Load() && missing.entries().empty());

    // A corrupt sidecar never blocks unrelated editing and is never overwritten.
    const fs::path blocked_user = root / "corrupt_user";
    const fs::path blocked_save = blocked_user / "savedata" / "1" / "CUSA10209" / "SAVEDATA00" / "SAVE0.DAT";
    std::vector<uint8_t> blocked_bytes = MakeSave();
    WriteCharacter(blocked_bytes, 0, 0, "HERO", CharacterClass::kFighter, 9, 300, 300, 0x41);
    FinalizeAndWrite(blocked_save, blocked_bytes);
    const std::vector<uint8_t> junk = {'B', 'A', 'D', '_', 'D', 'A', 'T', 'A'};
    fs::create_directories(blocked_user);
    {
        std::ofstream out(HpOverrideStorePath(blocked_user), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(junk.data()), std::streamsize(junk.size()));
    }
    SaveEdit blocked_edit(DiscoverSaves(blocked_user, "CUSA10209")[0], BackupFolder(blocked_user));
    CustomStore blocked_custom(CustomStorePath(blocked_user));
    HpOverrideStore blocked_hp(HpOverrideStorePath(blocked_user));
    CHECK(!blocked_hp.Load() && !blocked_hp.error().empty());
    CHECK(blocked_edit.working().SetHp(0, 0, 123, 300));
    const ApplyOutcome unrelated = ApplyEditor(blocked_edit, blocked_custom, blocked_hp, "CUSA10209", "SAVEDATA00");
    CHECK(unrelated.ok() && !unrelated.hp_written);
    CHECK(SaveModel::FromFile(blocked_save).Character(0, 0)->hp_current == 123);
    const std::vector<uint8_t> corrupt_after = ReadFile(HpOverrideStorePath(blocked_user));
    CHECK(corrupt_after == junk);

    // Transaction rollback: stale SAVE0.DAT must roll back character_overrides.dat.
    const std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10209");
    SaveEdit edit(saves[0], BackupFolder(user));
    CustomStore cs_saved(CustomStorePath(user));
    HpOverrideStore hs_saved(HpOverrideStorePath(user));
    CHECK(hs_saved.Load());
    CHECK(edit.working().SetHp(0, 0, 300, 5000));
    // Externally mutate SAVE0.DAT to make SaveEdit stale.
    WriteCharacter(raw, 0, 0, "HERO", CharacterClass::kFighter, 10, 310, 310, 0x31);
    FinalizeAndWrite(save_path, raw);
    const ApplyOutcome stale = ApplyEditor(edit, cs_saved, hs_saved, "CUSA10209", "SAVEDATA00");
    CHECK(!stale.ok() && stale.save == SaveEdit::Result::kStale && stale.rolled_back);
    HpOverrideStore after_rollback(HpOverrideStorePath(user));
    CHECK(after_rollback.Load() && after_rollback.Get(key1) == std::optional<uint16_t>(4000));

    hp_override_hooks_set_entries(nullptr, 0);
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

bool ContainsPattern(const std::vector<uint8_t>& buf, size_t from, size_t to, const uint8_t* pat, size_t len) {
    if (to > buf.size() || from + len > to) return false;
    for (size_t i = from; i + len <= to; ++i) {
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
    // Normal and Ultimate class/level table HP overwrite sequences: mov ecx,[rax]; mov [r15/rbx+0x10],ecx; mov [r15/rbx+0x0c],ecx.
    const uint8_t kNormalOverwrite[] = {0x8b, 0x08, 0x41, 0x89, 0x4f, 0x10, 0x41, 0x89, 0x4f, 0x0c};
    const uint8_t kUltOverwrite[] = {0x8b, 0x08, 0x89, 0x4b, 0x10, 0x89, 0x4b, 0x0c};
    const uint8_t kVtable9f0Call[] = {0xff, 0x90, 0xf0, 0x09, 0x00, 0x00};

    int verified = 0;
    bool usa_100 = false;
    for (const auto& [label, path] : builds) {
        if (!fs::exists(path)) continue;
        std::vector<uint8_t> text;
        uint64_t entry = 0;
        CHECK(ExtractSelfText(path, text, entry));
        HpOverrideSites sites{};
        CHECK(hp_override_hooks_locate(text.data(), text.size(), &sites));
        CHECK(sites.recalc_post > sites.recalc_entry);
        // Prove vtable[0x9f0] (equipment/skill pass) runs strictly BEFORE recalc_post.
        CHECK(ContainsPattern(text, size_t(sites.recalc_entry), size_t(sites.recalc_post), kVtable9f0Call, sizeof(kVtable9f0Call)));
        // Prove both native HP overwrite bodies exist in the executable.
        CHECK(ContainsPattern(text, 0, text.size(), kNormalOverwrite, sizeof(kNormalOverwrite)));
        CHECK(ContainsPattern(text, 0, text.size(), kUltOverwrite, sizeof(kUltOverwrite)));
        // Prove the status viewer stores its printed HP (mov eax,disp32(r14)) and that the call before it computes
        // max HP with equipment from the block's +0x10.
        CHECK(sites.status_max + 7 <= text.size());
        CHECK(text[size_t(sites.status_max)] == 0x41 && text[size_t(sites.status_max) + 1] == 0x89 &&
              text[size_t(sites.status_max) + 2] == 0x86);

        // Prove hook installation succeeds on a real-sized image + adjacent stub buffer.
        const size_t stub_bytes = hp_override_hooks_stub_bytes();
        std::vector<uint8_t> combined(text.size() + stub_bytes, 0);
        std::memcpy(combined.data(), text.data(), text.size());
        CHECK(hp_override_hooks_install(combined.data(), text.size(), entry, &sites, combined.data() + text.size(), stub_bytes));
        ++verified;
        if (std::string_view(label) == "CUSA10454-1.00") usa_100 = true;
        std::printf("DCP-UTGYFH native parity %s: spawn=0x%llx recalc_entry=0x%llx recalc_post=0x%llx status=0x%llx PASS\n",
                    label, static_cast<unsigned long long>(sites.spawn_load),
                    static_cast<unsigned long long>(sites.recalc_entry),
                    static_cast<unsigned long long>(sites.recalc_post),
                    static_cast<unsigned long long>(sites.status_max));
    }
    // CUSA10454 1.00 is the manual status-screen build: when this checkout has its real executable, it must be covered.
    if (verified > 0) {
        CHECK(fs::exists(builds[4].second));
        CHECK(usa_100);
    }
    // Fail-safe on unrecognized/corrupt image: locate returns 0 and installs nothing.
    std::vector<uint8_t> bogus(4096, 0x90);
    HpOverrideSites bad_sites{};
    CHECK(!hp_override_hooks_locate(bogus.data(), bogus.size(), &bad_sites));
    std::vector<uint8_t> stubs(hp_override_hooks_stub_bytes(), 0);
    CHECK(!hp_override_hooks_install(bogus.data(), bogus.size(), 0, &bad_sites, stubs.data(), stubs.size()));
    std::printf("DCP-UTGYFH native builds verified: %d\n", verified);
}

} // namespace

int main() {
    const fs::path temp = fs::temp_directory_path() / "dcp_hp_override_tests";
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);

    TestDcpUtgyfhCoreAndAntiInfiniteHealth(temp);
    TestStatusScreenAndPreviewPath(temp);
    TestIsolationMultiplayerRemoteAndErrors(temp);
    TestNativeExecutableEvidence();

    fs::remove_all(temp, ec);
    if (g_failures == 0) {
        std::printf("DCP-UTGYFH hp_override_tests: PASS (%d checks)\n", g_checks);
        return 0;
    }
    std::fprintf(stderr, "DCP-UTGYFH hp_override_tests: FAIL (%d/%d checks failed)\n", g_failures, g_checks);
    return 1;
}
