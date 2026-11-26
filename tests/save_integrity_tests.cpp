// Focused headless tests of the online save integrity record (launcher/save_integrity.h): the file, the bits, the save
// key, the Editor classification through ApplyAll, the character_overrides.dat backfill and the launcher hello.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "appearance_custom.h"
#include "hp_override.h"
#include "net/json.h"
#include "net/lobby_link.h"
#include "save_data.h"
#include "save_discovery.h"
#include "save_edit.h"
#include "save_integrity.h"

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

constexpr uint32_t kCheat = kIntegrityCheatUsed;
constexpr uint32_t kEditor = kIntegrityGameplayEditorUsed;
const SaveKey kUsa{"CUSA10454", "SAVEDATA00"};

void PutU16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b[off] = uint8_t(v);
    b[off + 1] = uint8_t(v >> 8);
}

void PutU32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (8 * i));
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

size_t RecordBase(int slot, int index) {
    return size_t(slot) * kDataSlotStride + kCharacterOffset + size_t(index) * kCharacterStride;
}

std::vector<uint8_t> MakeSave() {
    std::vector<uint8_t> bytes(kSaveFileSize, 0);
    PutU32(bytes, kSaveMagicOffset, kSaveMagic);
    for (size_t s = 0; s < kDataSlotCount; ++s) {
        for (size_t c = 0; c < kCharacterCount; ++c) PutU32(bytes, RecordBase(int(s), int(c)), kEmptyRecordFill);
    }
    const size_t base = RecordBase(0, 0);
    std::memset(bytes.data() + base, 0, kCharacterStride);
    PutU16(bytes, base + kCharHpCurrent, 300);
    PutU16(bytes, base + kCharHpMax, 300);
    PutU16(bytes, base + kCharStrength, 100);
    bytes[base + kCharLevel] = 9;
    bytes[base + kCharClass] = uint8_t(CharacterClass::kFighter);
    for (size_t i = 0; i < kCharIdentityBytes; ++i) bytes[base + kCharIdentity + i] = uint8_t(0x21 + i);
    std::memcpy(bytes.data() + base + kCharName, "HERO", 4);
    std::memcpy(bytes.data() + base + kCharNameSecond, "HERO", 4);
    PutU32(bytes, 0, SaveCrc32(bytes));
    return bytes;
}

fs::path SaveFile(const fs::path& user, const SaveKey& key) {
    return user / "savedata" / "1" / key.title_id / key.save_dir / "SAVE0.DAT";
}

fs::path FreshUser(const fs::path& root, const char* name, const SaveKey& key = kUsa) {
    const fs::path user = root / name;
    fs::remove_all(user);
    WriteFile(SaveFile(user, key), MakeSave());
    return user;
}

SaveEdit OpenEdit(const fs::path& user, const SaveKey& key = kUsa) {
    for (DiscoveredSave& save : DiscoverSaves(user, key.title_id)) {
        if (save.dir_name == key.save_dir) return SaveEdit(std::move(save), BackupFolder(user));
    }
    CHECK(false);
    return SaveEdit(DiscoveredSave{}, BackupFolder(user));
}

uint32_t StoredFlags(const fs::path& user, const SaveKey& key = kUsa) {
    SaveIntegrityStore store(SaveIntegrityStorePath(user));
    CHECK(store.Load());
    return store.Flags(key);
}

ApplyOutcome Apply(const fs::path& user, SaveEdit& edit, CustomStore& custom_working, CustomStore& custom_saved,
                   const SaveKey& key = kUsa) {
    CharacterOverrideStore override_saved(CharacterOverrideStorePath(user));
    CHECK(override_saved.Load());
    CharacterOverrideStore override_next = override_saved;
    SyncCharacterOverrides(override_next, edit.working(), edit.persisted(), key.title_id, key.save_dir);
    const IntegrityTarget target{user, key};
    return ApplyAll(&edit, custom_working, custom_saved, &override_next, &override_saved, &target);
}

ApplyOutcome Apply(const fs::path& user, SaveEdit& edit) {
    CustomStore custom(CustomStorePath(user));
    CHECK(custom.Load());
    return Apply(user, edit, custom, custom);
}

void TestFormat(const fs::path& root) {
    SaveIntegrityStore a(root / "a.dat"), b(root / "b.dat");
    a.Mark({"CUSA10454", "SAVEDATA00"}, kEditor);
    a.Mark({"CUSA07001", "SAVEDATA00"}, kCheat);
    a.Mark({"CUSA10454", "SAVEDATA01"}, kCheat);
    b.Mark({"CUSA10454", "SAVEDATA01"}, kCheat);
    b.Mark({"CUSA07001", "SAVEDATA00"}, kCheat);
    b.Mark({"CUSA10454", "SAVEDATA00"}, kEditor);
    const std::vector<uint8_t> bytes = a.Serialize();
    CHECK(bytes == b.Serialize()); // deterministic whatever the marking order
    CHECK(bytes.size() > 12 && std::memcmp(bytes.data(), "DCPI", 4) == 0 && bytes[4] == 1 && bytes[5] == 0 && bytes[8] == 3);
    SaveIntegrityStore parsed(root / "c.dat");
    CHECK(parsed.Parse(bytes) && parsed.entries() == a.entries() && parsed.Serialize() == bytes);
    CHECK((parsed.entries().front().first == SaveKey{"CUSA07001", "SAVEDATA00"}));
    CHECK(parsed.Flags({"CUSA10454", "SAVEDATA00"}) == kEditor && parsed.TitleFlags("CUSA10454") == (kCheat | kEditor));
    CHECK(parsed.Flags({"CUSA10209", "SAVEDATA00"}) == 0);

    SaveIntegrityStore empty(root / "d.dat");
    CHECK(empty.Parse(empty.Serialize()) && empty.entries().empty());

    // Invalid keys and bits are never recorded.
    SaveIntegrityStore invalid(root / "e.dat");
    invalid.Mark({"", "SAVEDATA00"}, kCheat);
    invalid.Mark({"CUSA10454", "../x"}, kCheat);
    invalid.Mark({"CUSA10454", "A|B"}, kCheat);
    invalid.Mark({"CUSA10454", "SAVEDATA00"}, 0);
    invalid.Mark({"CUSA10454", "SAVEDATA00"}, 4);
    CHECK(invalid.entries().empty());
}

void TestVersionsAndCorruption(const fs::path& root) {
    SaveIntegrityStore good(root / "good.dat");
    good.Mark(kUsa, kCheat);
    const std::vector<uint8_t> valid = good.Serialize();
    const auto rejects = [&](std::vector<uint8_t> bytes, const char* what) {
        SaveIntegrityStore store(root / "x.dat");
        const bool parsed = store.Parse(bytes);
        CHECK(!parsed && !store.error().empty());
        if (parsed) std::fprintf(stderr, "accepted: %s\n", what);
    };
    std::vector<uint8_t> newer = valid;
    newer[4] = 2;
    rejects(newer, "newer version");
    std::vector<uint8_t> magic = valid;
    magic[0] = 'X';
    rejects(magic, "magic");
    rejects(std::vector<uint8_t>(valid.begin(), valid.end() - 1), "truncated");
    std::vector<uint8_t> trailing = valid;
    trailing.push_back(0);
    rejects(trailing, "trailing");
    std::vector<uint8_t> zero = valid;
    zero[zero.size() - 4] = 0;
    rejects(zero, "zero flags");
    std::vector<uint8_t> unknown_bit = valid;
    unknown_bit[unknown_bit.size() - 4] = 5;
    rejects(unknown_bit, "unknown bit");
    rejects({}, "empty");

    SaveIntegrityStore unsorted(root / "u.dat");
    unsorted.Mark({"CUSA10454", "SAVEDATA01"}, kCheat);
    unsorted.Mark({"CUSA10454", "SAVEDATA00"}, kCheat);
    std::vector<uint8_t> swapped = unsorted.Serialize();
    const size_t first = 12, record = 1 + 9 + 1 + 10 + 4;
    std::vector<uint8_t> reordered(swapped.begin(), swapped.begin() + first);
    reordered.insert(reordered.end(), swapped.begin() + first + record, swapped.end());
    reordered.insert(reordered.end(), swapped.begin() + first, swapped.begin() + first + record);
    rejects(reordered, "unsorted");
    std::vector<uint8_t> duplicate(swapped.begin(), swapped.begin() + first + record);
    duplicate.insert(duplicate.end(), swapped.begin() + first, swapped.begin() + first + record);
    rejects(duplicate, "duplicate");

    // A newer or corrupt file is never overwritten, and the state it holds is unknown, never clean.
    for (const std::vector<uint8_t>& bytes : {newer, trailing}) {
        const fs::path user = FreshUser(root, "kept");
        const fs::path path = SaveIntegrityStorePath(user);
        WriteFile(path, bytes);
        std::string error;
        CHECK(MarkSaveIntegrity(user, kUsa, kEditor, &error) == MarkResult::kFailed && !error.empty());
        CHECK(ReadFile(path) == bytes);
        SaveIntegrityStore store(path);
        CHECK(!store.Load() && !store.Save() && ReadFile(path) == bytes);
        CHECK(!SaveIntegrityFlags(user, DiscoverSaves(user, "CUSA10454"), "CUSA10454", "SAVEDATA00"));
        CHECK(HelloIntegrityFlags(user, "CUSA10454", "SAVEDATA00") == -1 && HelloIntegrityFlags(user, "CUSA10454", "") == -1);
        SaveEdit edit = OpenEdit(user);
        CHECK(edit.working().SetGold(0, 1234));
        const ApplyOutcome outcome = Apply(user, edit);
        CHECK(!outcome.ok() && outcome.integrity_failed && outcome.save == SaveEdit::Result::kNothingToApply);
        CHECK(SaveModel::FromFile(SaveFile(user, kUsa)).Slot(0)->gold == 0 && ReadFile(path) == bytes);
    }
}

void TestPersistence(const fs::path& root) {
    const fs::path user = root / "persist";
    fs::remove_all(user);
    CHECK(MarkSaveIntegrity(user, kUsa, kCheat) == MarkResult::kMarked);
    CHECK(StoredFlags(user) == kCheat);
    CHECK(MarkSaveIntegrity(user, kUsa, kCheat) == MarkResult::kAlreadySet);
    CHECK(MarkSaveIntegrity(user, kUsa, kEditor) == MarkResult::kMarked && StoredFlags(user) == (kCheat | kEditor));
    CHECK(MarkSaveIntegrity(user, kUsa, kCheat) == MarkResult::kAlreadySet && StoredFlags(user) == (kCheat | kEditor));
    CHECK(MarkSaveIntegrity(user, {"", "SAVEDATA00"}, kCheat) == MarkResult::kFailed);
    CHECK(MarkSaveIntegrity(user, kUsa, 0) == MarkResult::kFailed);
    int others = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(user)) others += entry.path().filename() != "online_integrity.dat";
    CHECK(others == 0); // no temporary file is left behind
    SaveIntegrityStore reread(SaveIntegrityStorePath(user));
    CHECK(reread.Load() && reread.Serialize() == ReadFile(SaveIntegrityStorePath(user)));

    CHECK(dcp_save_integrity_mark(user.string().c_str(), "CUSA10209", "SAVEDATA00", 1) == 1);
    CHECK(dcp_save_integrity_mark(nullptr, "CUSA10209", "SAVEDATA00", 1) == 0);
    CHECK(StoredFlags(user, {"CUSA10209", "SAVEDATA00"}) == kCheat);
    CHECK(dcp_save_integrity_flags(user.string().c_str(), "CUSA10209", "SAVEDATA00") == 1);
    CHECK(dcp_save_integrity_flags(user.string().c_str(), "CUSA10209", nullptr) == 1);
    CHECK(dcp_save_integrity_flags(user.string().c_str(), "", nullptr) == -1);
}

void TestAccumulationAndIsolation(const fs::path& root) {
    const fs::path user = root / "bits";
    fs::remove_all(user);
    const SaveKey hk{"CUSA10209", "SAVEDATA00"}, second{"CUSA10454", "SAVEDATA01"};
    CHECK(StoredFlags(user) == 0);
    MarkSaveIntegrity(user, kUsa, kCheat);
    CHECK(StoredFlags(user) == 1);
    MarkSaveIntegrity(user, kUsa, kEditor);
    CHECK(StoredFlags(user) == 3);
    MarkSaveIntegrity(user, hk, kEditor);
    CHECK(StoredFlags(user, hk) == 2);
    MarkSaveIntegrity(user, hk, kCheat);
    CHECK(StoredFlags(user, hk) == 3);
    CHECK(StoredFlags(user, second) == 0 && StoredFlags(user, {"CUSA07001", "SAVEDATA00"}) == 0);
    MarkSaveIntegrity(user, second, kCheat);
    CHECK(StoredFlags(user, second) == 1 && StoredFlags(user) == 3 && StoredFlags(user, hk) == 3);
    SaveIntegrityStore store(SaveIntegrityStorePath(user));
    CHECK(store.Load());
    store.Mark(kUsa, 0);
    store.Mark(kUsa, kCheat);
    CHECK(store.Flags(kUsa) == 3 && store.TitleFlags("CUSA10454") == 3 && store.TitleFlags("CUSA10209") == 3);
}

void TestClassification() {
    const SaveModel before = SaveModel::FromBytes(MakeSave());
    CHECK(before.valid());
    const auto changed = [&](auto edit) {
        SaveModel after = before;
        CHECK(edit(after));
        return IsGameplayEdit(before, after);
    };
    CHECK(!IsGameplayEdit(before, before));
    CHECK(!changed([](SaveModel& m) { return m.SetName(0, 0, "ZED"); }));
    CHECK(!changed([](SaveModel& m) { return m.SetColor(0, 0, 3); }));
    CHECK(!changed([](SaveModel& m) { return m.SetName(0, 0, "ZED") && m.SetColor(0, 0, 4); }));
    CHECK(changed([](SaveModel& m) { return m.SetLevel(0, 0, 50); }));
    CHECK(changed([](SaveModel& m) { return m.SetSkillPoints(0, 0, 99); }));
    CHECK(changed([](SaveModel& m) { return m.SetHp(0, 0, 100, 300); }));
    CHECK(changed([](SaveModel& m) { return m.SetHp(0, 0, 300, 900); }));
    CHECK(changed([](SaveModel& m) { return m.SetGold(0, 5000); }));
    CHECK(changed([](SaveModel& m) { return m.SetDifficulty(0, 0, 2); }));
    const CharacterStats native = before.Character(0, 0)->stats;
    for (int field = 0; field < 6; ++field) {
        CHECK(changed([&](SaveModel& m) {
            CharacterStats stats = native;
            uint16_t* values[] = {&stats.strength, &stats.constitution, &stats.dexterity, &stats.intelligence,
                                  &stats.magic_resistance, &stats.luck};
            *values[field] = 777;
            return m.SetStats(0, 0, stats);
        }));
    }
    // A byte no Editor field names yet still counts: anything outside the cosmetic bytes is gameplay.
    SaveModel raw = before;
    raw.bytes()[RecordBase(0, 0) + 0x300] ^= 1;
    CHECK(IsGameplayEdit(before, raw));
    SaveModel crc_only = before;
    crc_only.bytes()[0] ^= 0xFF;
    CHECK(!IsGameplayEdit(before, crc_only));
}

void TestEditorApply(const fs::path& root) {
    // Name, native colour and custom appearance never mark; a gameplay field does, once applied.
    const fs::path user = FreshUser(root, "editor");
    {
        SaveEdit edit = OpenEdit(user);
        CHECK(edit.working().SetName(0, 0, "ZED") && edit.working().SetColor(0, 0, 2));
        CHECK(Apply(user, edit).ok() && !fs::exists(SaveIntegrityStorePath(user)));
    }
    {
        CustomStore saved(CustomStorePath(user));
        CHECK(saved.Load());
        CustomStore working = saved;
        CustomLook look;
        look.enabled = true;
        working.Set({kUsa.title_id, kUsa.save_dir, 0, 0, CharacterClass::kFighter}, look);
        SaveEdit edit = OpenEdit(user);
        const ApplyOutcome outcome = Apply(user, edit, working, saved);
        CHECK(outcome.ok() && outcome.custom_written && !outcome.integrity_marked && !fs::exists(SaveIntegrityStorePath(user)));
    }
    {
        const SaveEdit opened = OpenEdit(user); // opening only
        CHECK(!opened.dirty());
        SaveEdit pending = OpenEdit(user);
        CHECK(pending.working().SetLevel(0, 0, 80));
        pending.Discard();
        CHECK(!pending.dirty() && Apply(user, pending).ok() && !fs::exists(SaveIntegrityStorePath(user)));
    }
    {
        SaveEdit invalid = OpenEdit(user);
        std::vector<uint8_t> bytes(invalid.working().bytes().begin(), invalid.working().bytes().end());
        PutU16(bytes, RecordBase(0, 0) + kCharHpCurrent, 900); // current above max: refused
        PutU32(bytes, 0, SaveCrc32(bytes));
        invalid.working() = SaveModel::FromBytes(bytes);
        const ApplyOutcome refused = Apply(user, invalid);
        CHECK(!refused.ok() && refused.save == SaveEdit::Result::kInvalidHp && !fs::exists(SaveIntegrityStorePath(user)));
    }
    {
        SaveEdit stale = OpenEdit(user);
        CHECK(stale.working().SetGold(0, 999));
        std::vector<uint8_t> moved = ReadFile(SaveFile(user, kUsa));
        PutU32(moved, size_t(0) * kDataSlotStride + kDataSlotGoldOffset, 5);
        PutU32(moved, 0, SaveCrc32(moved));
        WriteFile(SaveFile(user, kUsa), moved); // the game wrote the save meanwhile
        const ApplyOutcome outcome = Apply(user, stale);
        CHECK(!outcome.ok() && outcome.save == SaveEdit::Result::kStale && outcome.rolled_back);
        CHECK(!fs::exists(SaveIntegrityStorePath(user)));
    }
    {
        SaveEdit edit = OpenEdit(user);
        CHECK(edit.working().SetLevel(0, 0, 80));
        const ApplyOutcome outcome = Apply(user, edit);
        CHECK(outcome.ok() && outcome.integrity_marked && StoredFlags(user) == kEditor);
        SaveEdit again = OpenEdit(user);
        CHECK(again.working().SetGold(0, 777));
        const ApplyOutcome second = Apply(user, again);
        CHECK(second.ok() && !second.integrity_marked && StoredFlags(user) == kEditor);
    }
    // A failed Apply after an earlier mark keeps the earlier mark.
    {
        MarkSaveIntegrity(user, kUsa, kCheat);
        SaveEdit stale = OpenEdit(user);
        CHECK(stale.working().SetGold(0, 4));
        std::vector<uint8_t> moved = ReadFile(SaveFile(user, kUsa));
        PutU32(moved, kDataSlotGoldOffset, 6);
        PutU32(moved, 0, SaveCrc32(moved));
        WriteFile(SaveFile(user, kUsa), moved);
        CHECK(Apply(user, stale).save == SaveEdit::Result::kStale && StoredFlags(user) == 3);
    }
}

void TestReplacedSaveKeepsMark(const fs::path& root) {
    const fs::path user = FreshUser(root, "replaced");
    MarkSaveIntegrity(user, kUsa, kEditor);
    WriteFile(SaveFile(user, kUsa), MakeSave()); // a clean import or restore over the same container
    CHECK(SaveIntegrityFlags(user, DiscoverSaves(user, "CUSA10454"), "CUSA10454", "SAVEDATA00") == kEditor);
    fs::remove_all(SaveFile(user, kUsa).parent_path()); // even a deleted container keeps its mark for the next one
    CHECK(HelloIntegrityFlags(user, "CUSA10454", "SAVEDATA00") == int(kEditor));
    CHECK(HelloIntegrityFlags(user, "CUSA10454", "") == int(kEditor));
}

void TestOverrideBackfill(const fs::path& root) {
    const CustomKey hero{kUsa.title_id, kUsa.save_dir, 0, 0, CharacterClass::kFighter};
    const auto with_override = [&](const char* name, std::optional<std::array<uint8_t, kCharIdentityBytes>> identity,
                                   CharacterClass klass = CharacterClass::kFighter) {
        const fs::path user = FreshUser(root, name);
        CharacterOverrideStore store(CharacterOverrideStorePath(user));
        CharacterOverride entry;
        entry.Set(CHARACTER_OVERRIDE_MAX_HP, 4000);
        if (identity) entry.identity = *identity;
        CustomKey key = hero;
        key.klass = klass;
        store.Set(key, entry);
        CHECK(store.Save());
        return user;
    };
    std::array<uint8_t, kCharIdentityBytes> own{};
    for (size_t i = 0; i < own.size(); ++i) own[i] = uint8_t(0x21 + i);
    std::array<uint8_t, kCharIdentityBytes> other = own;
    other[0] ^= 0x40;

    const fs::path matched = with_override("backfill_matched", own);
    CHECK(!fs::exists(SaveIntegrityStorePath(matched)));
    CHECK(HelloIntegrityFlags(matched, "CUSA10454", "SAVEDATA00") == int(kEditor));
    CHECK(StoredFlags(matched) == kEditor); // recorded, so it stays when the override goes
    fs::remove(CharacterOverrideStorePath(matched));
    CHECK(HelloIntegrityFlags(matched, "CUSA10454", "") == int(kEditor));

    const fs::path legacy = with_override("backfill_unbound", std::nullopt); // files before version 3 have no identity
    CHECK(HelloIntegrityFlags(legacy, "CUSA10454", "SAVEDATA00") == int(kEditor));

    const fs::path stranger = with_override("backfill_other_identity", other);
    CHECK(HelloIntegrityFlags(stranger, "CUSA10454", "SAVEDATA00") == 0 && !fs::exists(SaveIntegrityStorePath(stranger)));
    const fs::path wrong_class = with_override("backfill_other_class", own, CharacterClass::kElf);
    CHECK(HelloIntegrityFlags(wrong_class, "CUSA10454", "SAVEDATA00") == 0);

    // character_appearance.dat alone is never evidence.
    const fs::path appearance = FreshUser(root, "backfill_appearance");
    CustomStore looks(CustomStorePath(appearance));
    CustomLook look;
    look.enabled = true;
    looks.Set(hero, look);
    CHECK(looks.Save() && HelloIntegrityFlags(appearance, "CUSA10454", "SAVEDATA00") == 0);

    // An unreadable character_overrides.dat leaves the state unknown unless the Editor bit is already recorded.
    const fs::path unreadable = FreshUser(root, "backfill_unreadable");
    WriteFile(CharacterOverrideStorePath(unreadable), {'J', 'U', 'N', 'K'});
    CHECK(HelloIntegrityFlags(unreadable, "CUSA10454", "SAVEDATA00") == -1);
    MarkSaveIntegrity(unreadable, kUsa, kEditor);
    CHECK(HelloIntegrityFlags(unreadable, "CUSA10454", "SAVEDATA00") == int(kEditor));
}

void TestHello() {
    net::Json hello = net::Json::Object();
    net::set_integrity_flags(hello, -1);
    CHECK(!hello.has("integrity_flags"));
    net::set_integrity_flags(hello, 4);
    CHECK(!hello.has("integrity_flags"));
    for (int flags = 0; flags <= 3; ++flags) {
        net::Json body = net::Json::Object();
        net::set_integrity_flags(body, flags);
        CHECK(body.has("integrity_flags") && body["integrity_flags"].as_int() == flags);
    }
    const std::optional<net::Json> reply =
        net::Json::parse(R"({"capabilities":{"save_integrity":true,"clean_online_required":true}})");
    CHECK(reply && net::save_integrity_capability(*reply) && net::clean_online_required(*reply));
    const std::optional<net::Json> old = net::Json::parse(R"({"capabilities":{"relay_supported":true}})");
    CHECK(old && !net::save_integrity_capability(*old) && !net::clean_online_required(*old));
    CHECK(net::save_integrity_refusal("save_integrity_marked") && net::save_integrity_refusal("save_integrity_unknown"));
    CHECK(!net::save_integrity_refusal("unauthorized") && !net::save_integrity_refusal("proto"));
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "dcp_save_integrity_tests";
    fs::remove_all(root);
    fs::create_directories(root);
    struct Group {
        const char* name;
        void (*run)(const fs::path&);
    };
    const Group groups[] = {
        {"integrity-format", [](const fs::path& r) { TestFormat(r); TestVersionsAndCorruption(r); TestPersistence(r); }},
        {"integrity-bits", [](const fs::path& r) { TestAccumulationAndIsolation(r); TestReplacedSaveKeepsMark(r); }},
        {"integrity-editor", [](const fs::path& r) { TestClassification(); TestEditorApply(r); }},
        {"integrity-backfill", [](const fs::path& r) { TestOverrideBackfill(r); }},
        {"integrity-hello", [](const fs::path&) { TestHello(); }},
    };
    for (const Group& group : groups) {
        const int before = g_failures;
        group.run(root);
        std::printf("GROUP|%s|%s\n", group.name, g_failures == before ? "PASS" : "FAIL");
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
