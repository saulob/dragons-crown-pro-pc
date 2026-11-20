// Focused tests for the Editor's SAVE0.DAT handling (CRC32, validation, parsing, editing, writing, backups, imports, the
// native colour field), the appearance sidecar, and the FMBS/FTX/BC7 character art path. Synthetic saves are built in
// memory; real saves and the installed game, when present under the source tree, are only read (saves via a temporary
// copy). No window, no game.
#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <memory>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <random>
#include <set>
#include <cstring>

#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <atomic>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <future>

#include "appearance_custom.h"
#include "appearance_hooks.h"
#include "appearance_loader.h"
#include "appearance_runtime.h"
#include "character_art.h"
#include "cpk_fixture.h"
#include "cpk_patch.h"
#include "dcp_launcher.h"
#include "editor_model.h"
#include "fighter_horns.h"
#include "game_mbs.h"
#include "game_profile.h"
#include "game_texture.h"
#include "localization/ui_text.h"
#include "save_backup.h"
#include "save_data.h"
#include "save_discovery.h"
#include "save_edit.h"
#include "save_import.h"
#include "sprite_render.h"
#include "trg/layout.h"

namespace fs = std::filesystem;
using namespace dcp;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

void PutU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void PutU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void PutName(std::vector<uint8_t>& bytes, size_t offset, const char* name) {
    std::memset(bytes.data() + offset, 0, kCharNameBytes);
    std::memcpy(bytes.data() + offset, name, std::strlen(name));
}

size_t RecordOffset(int slot, int character) {
    return slot * kDataSlotStride + kCharacterOffset + character * kCharacterStride;
}

void SetCharacter(std::vector<uint8_t>& bytes, int slot, int character, const char* name, uint16_t hp, uint8_t level,
                  uint16_t skill_points) {
    const size_t base = RecordOffset(slot, character);
    PutU32(bytes, base, 0); // present marker (not the empty 0xCC fill)
    PutU16(bytes, base + kCharHpCurrent, hp);
    PutU16(bytes, base + kCharHpMax, hp);
    PutU16(bytes, base + kCharSkillPoints, skill_points);
    bytes[base + kCharLevel] = level;
    PutName(bytes, base + kCharName, name);
    PutName(bytes, base + kCharNameSecond, name);
}

std::vector<uint8_t> MakeSyntheticSave() {
    std::vector<uint8_t> bytes(kSaveFileSize, 0xCC);
    std::memcpy(bytes.data() + kSaveMagicOffset, "DCSD", 4);
    const uint32_t gold[] = {100, 7757, 100};
    for (size_t slot = 0; slot < kDataSlotCount; ++slot) {
        PutU32(bytes, slot * kDataSlotStride + kDataSlotGoldOffset, gold[slot]);
    }
    SetCharacter(bytes, 0, 0, "Eppie", 349, 14, 15);
    SetCharacter(bytes, 0, 1, "Allie", 300, 0, 1);
    PutU32(bytes, kSaveCrcOffset, SaveCrc32(bytes));
    return bytes;
}

void WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool ReadBytes(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// A texel of a Fighter helmet sprite: with the helmet hidden it is repainted, cleared or kept as TestFighterBareHead checks.
bool InFighterHelmetSprite(CharacterClass klass, std::string_view key, int x, int y) {
    if (klass != CharacterClass::kFighter || key != "helmet") return false;
    const std::span<const HelmetSpriteRule> rules = FighterHelmetSprites();
    return std::any_of(rules.begin(), rules.end(), [&](const HelmetSpriteRule& rule) { return rule.rect.Contains(x, y); });
}

void TestCrc32() {
    const std::vector<uint8_t> save = MakeSyntheticSave();
    const uint32_t stored = [&] {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(save[i]) << (8 * i);
        }
        return v;
    }();
    CHECK(stored == SaveCrc32(save));
    CHECK(SaveCrc32(save) != SaveCrc32(std::vector<uint8_t>(save.begin() + 4, save.end())));
}

void TestParseValid() {
    const std::vector<uint8_t> save = MakeSyntheticSave();
    const SaveModel model = SaveModel::FromBytes(save);
    CHECK(model.valid());
    CHECK(model.crc_valid());
    CHECK(model.errors().empty());
    CHECK(model.slots().size() == kDataSlotCount);
}

void TestInvalidCrc() {
    std::vector<uint8_t> save = MakeSyntheticSave();
    save[0] ^= 0xFF;
    const SaveModel model = SaveModel::FromBytes(save);
    CHECK(!model.valid());
    CHECK(!model.errors().empty());
}

void TestTruncated() {
    const std::vector<uint8_t> save = MakeSyntheticSave();
    const std::vector<uint8_t> truncated(save.begin(), save.begin() + 1024);
    std::vector<std::string> errors;
    CHECK(!ValidateSave(truncated, errors));
    CHECK(!errors.empty());
}

void TestSlots() {
    const SaveModel model = SaveModel::FromBytes(MakeSyntheticSave());
    CHECK(model.Slot(0) != nullptr);
    CHECK(model.Slot(2) != nullptr);
    CHECK(model.Slot(3) == nullptr);
    CHECK(model.slots()[0].gold == 100);
    CHECK(model.slots()[1].gold == 7757);
    CHECK(model.slots()[2].gold == 100);
}

void TestCharacters() {
    const SaveModel model = SaveModel::FromBytes(MakeSyntheticSave());
    const CharacterView* first = model.Character(0, 0);
    const CharacterView* second = model.Character(0, 1);
    const CharacterView* empty = model.Character(0, 5);
    CHECK(first && first->present);
    CHECK(second && second->present);
    CHECK(empty && !empty->present);
    CHECK(first->name == "Eppie");
    CHECK(second->name == "Allie");
    CHECK(first->hp_current == 349 && first->hp_max == 349);
    CHECK(first->skill_points == 15);
    CHECK(SaveModel::DisplayedLevel(first->level_stored) == 15);
    CHECK(SaveModel::DisplayedLevel(second->level_stored) == 1);
    // Character stride: the second record is exactly one stride past the first.
    CHECK(second->record_offset - first->record_offset == kCharacterStride);
}

void TestEdits() {
    SaveModel model = SaveModel::FromBytes(MakeSyntheticSave());
    CHECK(model.SetGold(0, 12345));
    CHECK(model.slots()[0].gold == 12345);
    CHECK(model.SetLevel(0, 0, 255));
    CHECK(model.Character(0, 0)->level_stored == 254);
    CHECK(model.SetSkillPoints(0, 0, 200));
    CHECK(model.Character(0, 0)->skill_points == 200);
    CHECK(model.SetHp(0, 0, 500, 600));
    CHECK(model.Character(0, 0)->hp_current == 500 && model.Character(0, 0)->hp_max == 600);
    CHECK(!model.SetHp(0, 0, 700, 600)); // current above max is refused
    CHECK(model.SetName(0, 0, "Bruno"));
    CHECK(model.Character(0, 0)->name == "Bruno");
    CHECK(!model.SetName(0, 0, "toolongname")); // over 8 characters is refused
    CHECK(!model.SetLevel(0, 0, 300));          // above the legal maximum is refused
    CHECK(!model.SetLevel(0, 5, 10));           // an empty character is refused
}

void TestSerializeRecomputesCrc() {
    SaveModel model = SaveModel::FromBytes(MakeSyntheticSave());
    model.SetGold(0, 9999);
    const std::vector<uint8_t> serialized = model.Serialize();
    const SaveModel reopened = SaveModel::FromBytes(serialized);
    CHECK(reopened.valid());
    CHECK(reopened.crc_valid());
    CHECK(reopened.slots()[0].gold == 9999);
}

void TestAtomicWrite(const fs::path& base) {
    const fs::path target = base / "atomic" / "SAVE0.DAT";
    const std::vector<uint8_t> save = MakeSyntheticSave();
    const auto error = WriteSaveAtomically(target, save, ValidateSave);
    CHECK(!error.has_value());
    std::vector<uint8_t> on_disk;
    CHECK(ReadBytes(target, on_disk));
    CHECK(on_disk == save);

    // A write that fails validation leaves the target untouched.
    std::vector<uint8_t> bad = save;
    bad[0] ^= 0xFF; // corrupt the CRC
    const auto bad_error = WriteSaveAtomically(target, bad, ValidateSave);
    CHECK(bad_error.has_value());
    std::vector<uint8_t> after_failure;
    CHECK(ReadBytes(target, after_failure));
    CHECK(after_failure == save);
}

void TestBackupRestore(const fs::path& base) {
    const fs::path save_dir = base / "savedata" / "1" / "CUSA10454" / "SAVEDATA00";
    const fs::path save0 = save_dir / "SAVE0.DAT";
    WriteBytes(save0, MakeSyntheticSave());
    const fs::path backup_folder = base / "save_backups";

    const auto backup = CreateBackup(backup_folder, save0, "CUSA10454", "SAVEDATA00");
    CHECK(backup.has_value());
    CHECK(fs::exists(backup->file));
    CHECK(fs::exists(backup->meta));

    // Never overwrites an old backup.
    const auto second = CreateBackup(backup_folder, save0, "CUSA10454", "SAVEDATA00");
    CHECK(second.has_value()); // a new timestamp, not the same file
    CHECK(second->file != backup->file);

    const std::vector<BackupInfo> backups = ListBackups(backup_folder);
    CHECK(backups.size() >= 2);

    // Restore the first backup over a changed save.
    SaveModel model = SaveModel::FromFile(save0);
    model.SetGold(0, 55555);
    WriteSaveAtomically(save0, model.Serialize(), ValidateSave);
    const auto restore_error = RestoreBackup(*backup, save0, backup_folder);
    CHECK(!restore_error.has_value());
    const SaveModel restored = SaveModel::FromFile(save0);
    CHECK(restored.valid());
    CHECK(restored.slots()[0].gold == 100);
}

void TestImport(const fs::path& base) {
    const std::vector<uint8_t> save = MakeSyntheticSave();
    const fs::path good_file = base / "import_good" / "SAVE0.DAT";
    WriteBytes(good_file, save);
    const ImportResult good = InspectImport(good_file);
    CHECK(good.valid);
    CHECK(good.data_slots == static_cast<int>(kDataSlotCount));
    CHECK(good.characters == 2);

    std::vector<uint8_t> bad = save;
    bad[0] ^= 0xFF;
    const fs::path bad_file = base / "import_bad" / "SAVE0.DAT";
    WriteBytes(bad_file, bad);
    const ImportResult rejected = InspectImport(bad_file);
    CHECK(!rejected.valid);
    CHECK(!rejected.errors.empty());

    // Title id recognition for the Apollo Vita/PS3 ids.
    CHECK(std::string(ImportPlatformOf("PCSE00019")) == "Vita");
    CHECK(std::string(ImportPlatformOf("BLUS30767")) == "PS3");
    CHECK(ImportRegionOf("PCSB00408") == "Europe");
    CHECK(std::string(ImportPlatformOf("NOTREAL")) == "");
}

SaveModel ModelWithColors(const std::vector<std::vector<int>>& rows) {
    std::vector<uint8_t> bytes = MakeSyntheticSave();
    for (size_t character = 0; character < rows.size(); ++character) {
        bytes[RecordOffset(0, int(character)) + kCharClass] = uint8_t(rows[character][0]);
        bytes[RecordOffset(0, int(character)) + kCharColor] = uint8_t(rows[character][1]);
    }
    PutU32(bytes, kSaveCrcOffset, SaveCrc32(bytes));
    return SaveModel::FromBytes(bytes);
}

fs::path WriteEditableSave(const fs::path& user, const std::vector<uint8_t>& bytes) {
    const fs::path file = user / "savedata" / "1" / "CUSA10454" / "SAVEDATA00" / "SAVE0.DAT";
    fs::remove_all(user);
    WriteBytes(file, bytes);
    return file;
}

SaveEdit OpenEdit(const fs::path& user) {
    std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10454");
    CHECK(saves.size() == 1);
    return SaveEdit(saves.front(), BackupFolder(user));
}

void TestSaveEdit(const fs::path& base) {
    const fs::path user = base / "edit_user";
    const SaveModel model = ModelWithColors({{1, 2}, {4, 0}});
    const std::vector<uint8_t> original(model.bytes().begin(), model.bytes().end());
    const fs::path file = WriteEditableSave(user, original);

    SaveEdit edit = OpenEdit(user);
    CHECK(!edit.dirty());
    CHECK(edit.Apply() == SaveEdit::Result::kNothingToApply);
    std::vector<uint8_t> on_disk;
    CHECK(ReadBytes(file, on_disk) && on_disk == original);
    CHECK(ListBackups(BackupFolder(user)).empty());

    CHECK(edit.working().SetColor(0, 0, 4));
    CHECK(edit.dirty());
    CHECK(edit.working().Character(0, 0)->color == 4 && edit.persisted().Character(0, 0)->color == 2);
    edit.Discard();
    CHECK(!edit.dirty() && edit.working().Character(0, 0)->color == 2);
    CHECK(ReadBytes(file, on_disk) && on_disk == original);

    CHECK(edit.working().SetColor(0, 0, 4));
    CHECK(edit.working().SetColor(0, 0, 2));
    CHECK(!edit.dirty()); // back at the saved value

    CHECK(edit.working().SetColor(0, 0, 4));
    const std::vector<std::string> changes = DescribeColorChanges(edit.save(), edit.persisted(), edit.working());
    CHECK(changes.size() == 1 && changes[0] == "CUSA10454 DATA 1 slot 1 Amazon Eppie native colour 2 -> 4");
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    CHECK(!edit.dirty());
    CHECK(edit.persisted().Character(0, 0)->color == 4 && edit.working().Character(0, 0)->color == 4);
    CHECK(ReadBytes(file, on_disk));
    CHECK(on_disk.size() == original.size() && SaveModel::FromBytes(on_disk).valid());
    size_t differing = 0;
    size_t differing_at = 0;
    for (size_t i = 4; i < on_disk.size(); ++i) {
        if (on_disk[i] != original[i]) {
            ++differing;
            differing_at = i;
        }
    }
    CHECK(differing == 1 && differing_at == RecordOffset(0, 0) + kCharColor && on_disk[differing_at] == 4);
    CHECK(ListBackups(BackupFolder(user)).size() == 1);
    CHECK(!fs::exists(fs::path(file.string() + ".tmp")));

    const SaveEdit reopened = OpenEdit(user);
    CHECK(!reopened.dirty() && reopened.persisted().Character(0, 0)->color == 4);
    CHECK(reopened.persisted().Character(0, 1)->color == 0 && reopened.persisted().Character(0, 1)->name == "Allie");
}

void TestInvalidAppearance(const fs::path& base) {
    const fs::path user = base / "invalid_user";
    const SaveModel corrupt = ModelWithColors({{1, 1}, {3, 9}});
    CHECK(corrupt.valid());
    CHECK(!corrupt.Character(0, 1)->color_valid() && corrupt.Character(0, 0)->color_valid());
    const std::vector<uint8_t> original(corrupt.bytes().begin(), corrupt.bytes().end());
    const fs::path file = WriteEditableSave(user, original);

    SaveEdit edit = OpenEdit(user);
    const std::optional<InvalidAppearance> invalid = edit.FirstInvalidAppearance();
    CHECK(invalid && invalid->slot == 0 && invalid->character == 1 && invalid->value == 9 && invalid->name == "Allie");
    CHECK(!edit.dirty());
    CHECK(!edit.working().SetColor(0, 1, 5) && !edit.working().SetColor(0, 1, -1));

    CHECK(edit.working().SetLevel(0, 0, 9));
    CHECK(edit.Apply() == SaveEdit::Result::kInvalidAppearance);
    std::vector<uint8_t> on_disk;
    CHECK(ReadBytes(file, on_disk) && on_disk == original);
    CHECK(ListBackups(BackupFolder(user)).empty());

    CHECK(edit.working().SetColor(0, 1, 3));
    CHECK(!edit.FirstInvalidAppearance());
    CHECK(edit.Apply() == SaveEdit::Result::kApplied);
    const SaveModel fixed = SaveModel::FromFile(file);
    CHECK(fixed.valid() && fixed.Character(0, 1)->color == 3 && fixed.Character(0, 0)->color == 1);
    CHECK(SaveModel::DisplayedLevel(fixed.Character(0, 0)->level_stored) == 9);
}

void TestNoChangeGuarantee() {
    const std::vector<uint8_t> save = MakeSyntheticSave();
    const SaveModel model = SaveModel::FromBytes(save);
    CHECK(model.valid());
    // Selecting and serializing without any edit must reproduce the input byte for byte.
    const std::vector<uint8_t> serialized = model.Serialize();
    CHECK(serialized == save);
}

void TestRealSave(const fs::path& source_dir) {
    // Copies a real save to a temporary location and checks the parser agrees with the verified layout.
    const fs::path real = source_dir / "user" / "test-client-b" / "savedata" / "1" / "CUSA10454" / "SAVEDATA00" / "SAVE0.DAT";
    std::error_code ec;
    if (!fs::is_regular_file(real, ec)) {
        std::printf("real save not present, skipping its check\n");
        return;
    }
    const fs::path copy = fs::temp_directory_path() / "dcp_save_editor_real" / "SAVE0.DAT";
    fs::remove_all(copy.parent_path(), ec);
    fs::create_directories(copy.parent_path(), ec);
    fs::copy_file(real, copy, ec);
    CHECK(!ec);

    const SaveModel model = SaveModel::FromFile(copy);
    CHECK(model.valid());
    CHECK(model.crc_valid());
    // The verified Pro layout has 3 DATA slots and gold in the first at the known offset.
    CHECK(model.slots().size() == kDataSlotCount);
    CHECK(model.slots()[0].gold <= kMaxGold);
    bool any_present = false;
    for (const CharacterView& character : model.slots()[0].characters) {
        any_present = any_present || character.present;
    }
    CHECK(any_present);
    fs::remove_all(copy.parent_path(), ec);
}

void TestDiscovery(const fs::path& base) {
    const fs::path save0 = base / "savedata" / "1" / "CUSA10454" / "SAVEDATA00" / "SAVE0.DAT";
    WriteBytes(save0, MakeSyntheticSave());
    const std::vector<DiscoveredSave> saves = DiscoverSaves(base, "CUSA10454");
    CHECK(saves.size() == 1);
    CHECK(saves[0].title_id == "CUSA10454");
    CHECK(saves[0].region == "USA");
    CHECK(saves[0].current);
    CHECK(saves[0].model.valid());
    CHECK(fs::exists(saves[0].save0_path));
}

void PutBit(std::vector<uint8_t>& out, size_t& bit, int value) {
    if (value) out[bit >> 3] |= uint8_t(1 << (bit & 7));
    ++bit;
}

void PutBits(std::vector<uint8_t>& out, size_t& bit, int value, int count) {
    for (int i = 0; i < count; ++i) PutBit(out, bit, (value >> i) & 1);
}

// Solid mode-6 block: endpoints carry a p-bit after the alpha bits, index 0 everywhere.
std::vector<uint8_t> MakeMode6Block(int r, int g, int b, int a, int pbit) {
    std::vector<uint8_t> block(16, 0);
    size_t bit = 0;
    for (int i = 0; i < 6; ++i) PutBit(block, bit, 0);
    PutBit(block, bit, 1);
    for (const int channel : {r, g, b, a}) {
        PutBits(block, bit, channel, 7);
        PutBits(block, bit, channel, 7);
    }
    PutBit(block, bit, pbit);
    PutBit(block, bit, pbit);
    for (int t = 0; t < 16; ++t) PutBits(block, bit, 0, t == 0 ? 3 : 4);
    return block;
}

void TestBc7Decode() {
    const std::vector<uint8_t> block = MakeMode6Block(64, 96, 32, 120, 1);
    uint8_t rgba[64];
    DecodeBc7Block(block.data(), rgba);
    for (int t = 0; t < 16; ++t) {
        CHECK(rgba[t * 4 + 0] == 129 && rgba[t * 4 + 1] == 193 && rgba[t * 4 + 2] == 65 && rgba[t * 4 + 3] == 241);
    }

    // Mode 1, partition 0 (texels 2,3,6,7,... in subset 1): endpoint bits run channel by channel across all four
    // endpoints, then one shared p-bit per subset.
    std::vector<uint8_t> two(16, 0);
    size_t bit = 0;
    PutBit(two, bit, 0);
    PutBit(two, bit, 1);
    PutBits(two, bit, 0, 6);
    for (const int value : {10, 10, 50, 50}) PutBits(two, bit, value, 6); // red: subset 0 e0 e1, subset 1 e0 e1
    for (const int value : {20, 20, 40, 40}) PutBits(two, bit, value, 6); // green
    for (const int value : {30, 30, 5, 5}) PutBits(two, bit, value, 6);   // blue
    PutBit(two, bit, 1); // subset 0 p-bit
    PutBit(two, bit, 0); // subset 1 p-bit
    DecodeBc7Block(two.data(), rgba);
    const auto expand7 = [](int v) { return (v << 1) | (v >> 6); };
    CHECK(rgba[0] == expand7(10 << 1 | 1) && rgba[1] == expand7(20 << 1 | 1) && rgba[2] == expand7(30 << 1 | 1));
    CHECK(rgba[2 * 4 + 0] == expand7(50 << 1) && rgba[2 * 4 + 1] == expand7(40 << 1) && rgba[2 * 4 + 2] == expand7(5 << 1));
    CHECK(rgba[3] == 255 && rgba[2 * 4 + 3] == 255);
}

std::vector<uint8_t> MakeSolidPack(int r, int g, int b, int a) {
    const size_t gnf = 0xC0;
    std::vector<uint8_t> pack(gnf + 0x100 + 16, 0);
    std::memcpy(pack.data(), "FTEX", 4);
    PutU32(pack, 0x08, 0x80);
    PutU32(pack, 0x0C, 1);
    std::memcpy(pack.data() + 0x80, "FTX0", 4);
    PutU32(pack, 0x88, uint32_t(gnf - 0x80));
    std::memcpy(pack.data() + gnf, "GNF ", 4);
    PutU32(pack, gnf + 16 + 4, 41u << 20);     // BC7
    PutU32(pack, gnf + 16 + 8, 3 | (3 << 14)); // 4x4
    const std::vector<uint8_t> block = MakeMode6Block(r, g, b, a, 0);
    std::memcpy(pack.data() + gnf + 0x100, block.data(), 16);
    return pack;
}

void TestFtxDecode() {
    const std::vector<uint8_t> pack = MakeSolidPack(64, 96, 32, 120);
    std::string error;
    const std::optional<FtxTexture> texture = DecodeFtx(pack, &error);
    CHECK(texture.has_value());
    if (!texture) return;
    CHECK(texture->width == 4 && texture->height == 4);
    CHECK(texture->rgba[0] == 128 && texture->rgba[1] == 192 && texture->rgba[2] == 64 && texture->rgba[3] == 240);
    CHECK(FtxSurfaceCount(pack) == 1);
    const std::optional<FtxSurface> surface = ReadFtxSurface(pack, 0, &error);
    CHECK(surface && surface->Texel(3, 3)[1] == 192 && surface->Texel(-5, 99)[0] == 128);
    CHECK(!ReadFtxSurface(pack, 1, &error).has_value());

    std::string rejected;
    CHECK(!DecodeFtx(std::vector<uint8_t>(16, 0), &rejected).has_value());
    CHECK(!rejected.empty());
}

void PutF32(std::vector<uint8_t>& bytes, size_t offset, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    PutU32(bytes, offset, bits);
}

void PutQuad(std::vector<uint8_t>& bytes, size_t at, float left, float top, float right, float bottom) {
    const float corners[4][2] = {{left, top}, {left, bottom}, {right, bottom}, {right, top}};
    PutF32(bytes, at, (left + right) / 2);
    PutF32(bytes, at + 4, (top + bottom) / 2);
    for (int c = 0; c < 4; ++c) {
        PutF32(bytes, at + 8 + c * 8, corners[c][0]);
        PutF32(bytes, at + 12 + c * 8, corners[c][1]);
    }
    PutF32(bytes, at + 40, left);
    PutF32(bytes, at + 44, top);
}

// One animation, one track, one frame, one key, two layers: a full square (layer 0) and its left half (layer 1).
std::vector<uint8_t> MakeSyntheticMbs() {
    std::vector<uint8_t> mbs(0x400, 0);
    std::memcpy(mbs.data(), "FMBS", 4);
    mbs[0x14] = 0x72;
    const size_t colors = 0x120, sources = 0x138, destinations = 0x168, layers = 0x1C8, keys = 0x1E8, transforms = 0x204,
                 frames = 0x228, animations = 0x248, tracks = 0x278;
    PutU32(mbs, 0xB0, colors);       PutU16(mbs, 0x54, 1);
    PutU32(mbs, 0xB8, sources);      PutU16(mbs, 0x56, 1);
    PutU32(mbs, 0xC0, destinations); PutU16(mbs, 0x58, 2);
    PutU32(mbs, 0xD0, layers);       PutU32(mbs, 0x50, 2);
    PutU32(mbs, 0xE0, keys);         PutU16(mbs, 0x62, 1);
    PutU32(mbs, 0xE8, transforms);   PutU16(mbs, 0x5E, 1);
    PutU32(mbs, 0xF0, frames);       PutU16(mbs, 0x60, 1);
    PutU32(mbs, 0xF8, animations);   PutU16(mbs, 0x64, 1);
    PutU32(mbs, 0x100, tracks);      PutU16(mbs, 0x66, 1);
    std::memset(mbs.data() + colors, 0xFF, 0x18);
    PutQuad(mbs, sources, 0.0f, 0.0f, 1.0f, 1.0f);
    PutQuad(mbs, destinations, -10.0f, -20.0f, 10.0f, 0.0f);
    PutQuad(mbs, destinations + 0x30, -10.0f, -20.0f, 0.0f, 0.0f);
    for (int layer = 0; layer < 2; ++layer) {
        const size_t at = layers + size_t(layer) * 0x10;
        mbs[at + 5] = 1;
        PutU16(mbs, at + 14, uint16_t(layer)); // destination
    }
    PutF32(mbs, keys + 0, -10.0f); PutF32(mbs, keys + 4, -20.0f); PutF32(mbs, keys + 8, 10.0f); PutF32(mbs, keys + 12, 0.0f);
    PutU16(mbs, keys + 22, 2);
    PutF32(mbs, transforms + 24, 1.0f);
    PutF32(mbs, transforms + 28, 1.0f);
    PutU32(mbs, transforms + 32, 0xFFFFFFFF);
    PutU16(mbs, frames + 6, 1);
    PutF32(mbs, animations + 0, -10.0f); PutF32(mbs, animations + 4, -20.0f);
    PutF32(mbs, animations + 8, 10.0f); PutF32(mbs, animations + 12, 0.0f);
    std::memcpy(mbs.data() + animations + 0x10, "IDLE", 4);
    mbs[animations + 0x2A] = 1;
    PutU16(mbs, tracks + 2, 1);
    return mbs;
}

void TestMbsParse() {
    const std::vector<uint8_t> bytes = MakeSyntheticMbs();
    std::string error;
    const std::optional<Mbs> mbs = ParseMbs(bytes, &error);
    CHECK(mbs.has_value());
    if (!mbs) return;
    CHECK(mbs->animations.size() == 1 && mbs->animations[0].name == "IDLE");
    CHECK(mbs->layers.size() == 2 && mbs->keys[0].layer_count == 2);
    CHECK(FrameLayers(*mbs, 0, 0) == std::vector<uint32_t>({0, 1}));

    std::vector<uint8_t> bad = bytes;
    bad[0] = 'X';
    CHECK(!ParseMbs(bad).has_value());
    std::vector<uint8_t> dangling = bytes;
    PutU16(dangling, 0x1C8 + 14, 9); // a layer naming a destination quad that does not exist
    CHECK(!ParseMbs(dangling, &error).has_value());
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 0x200);
    CHECK(!ParseMbs(truncated).has_value());
}

void TestSpriteRender() {
    const std::optional<Mbs> mbs = ParseMbs(MakeSyntheticMbs());
    const std::optional<FtxSurface> surface = ReadFtxSurface(MakeSolidPack(127, 0, 0, 127), 0);
    CHECK(mbs && surface);
    if (!mbs || !surface) return;
    SpriteRequest request;
    request.mbs = &*mbs;
    request.scale = 2.0f;
    request.textures[0] = &*surface;
    const SpriteImage image = RenderSprite(request);
    CHECK(image.width == 40 && image.height == 40);
    const auto at = [&](int x, int y) { return size_t(y) * size_t(image.width) + size_t(x); };
    CHECK(image.rgba[at(30, 20) * 4] == 254 && image.rgba[at(30, 20) * 4 + 3] == 254);
    CHECK(image.layer[at(30, 20)] == 0); // right half: only the square
    CHECK(image.layer[at(10, 20)] == 1); // left half: the second layer is drawn last

    request.visible = [](uint32_t layer) { return layer == 0; };
    CHECK(RenderSprite(request).layer[at(10, 20)] == 0);
    request.visible = nullptr;
    request.shade = [](uint32_t layer, uint8_t, int, int, std::array<uint8_t, 4>& texel) {
        if (layer == 1) texel = {0, 0, 254, 255};
    };
    const SpriteImage shaded = RenderSprite(request);
    CHECK(shaded.rgba[at(10, 20) * 4 + 2] == 254 && shaded.rgba[at(10, 20) * 4] == 0);
    CHECK(shaded.rgba[at(30, 20) * 4] == 254);
}

void TestNativeColor() {
    std::vector<uint8_t> bytes = MakeSyntheticSave();
    bytes[RecordOffset(0, 0) + kCharClass] = 1;
    bytes[RecordOffset(0, 0) + kCharColor] = 3;
    bytes[RecordOffset(0, 1) + kCharClass] = 4;
    PutU32(bytes, kSaveCrcOffset, SaveCrc32(bytes));
    SaveModel model = SaveModel::FromBytes(bytes);
    CHECK(model.valid());
    CHECK(model.Character(0, 0)->klass == CharacterClass::kAmazon && model.Character(0, 0)->color == 3);
    CHECK(model.Character(0, 1)->klass == CharacterClass::kSorceress);
    CHECK(ClassFromId(5) == CharacterClass::kWizard);
    CHECK(!model.SetColor(0, 0, kNativeColorCount));
    CHECK(!model.SetColor(0, 5, 1)); // empty record
    CHECK(model.SetColor(0, 0, 1));
    const std::vector<uint8_t> written = model.Serialize();
    CHECK(written[RecordOffset(0, 0) + kCharColor] == 1);
    CHECK(SaveModel::FromBytes(written).Character(0, 0)->color == 1);
    size_t changed = 0;
    for (size_t i = 4; i < written.size(); ++i) changed += written[i] != bytes[i];
    CHECK(changed == 1);

    CHECK(NativeColorMessage(CharacterClass::kFighter, 0) == 871);
    CHECK(NativeColorMessage(CharacterClass::kAmazon, 0) == 876);
    CHECK(NativeColorMessage(CharacterClass::kDwarf, 4) == 885);
    CHECK(NativeColorMessage(CharacterClass::kElf, 2) == 888);
    CHECK(NativeColorMessage(CharacterClass::kSorceress, 0) == 891);
    CHECK(NativeColorMessage(CharacterClass::kWizard, 4) == 900);
    CHECK(NativeColorMessage(CharacterClass::kWizard, 5) == -1);
    CHECK(NativeColorAtlas(CharacterClass::kAmazon, 0) == "PLAYER/AMAZON00.FTX");
    CHECK(NativeColorAtlas(CharacterClass::kSorceress, 3) == "PLAYER/SORCERESS03.FTX");
    CHECK(NativeColorAtlas(CharacterClass::kSorceress, 5).empty() && NativeColorAtlas(CharacterClass::kFighter, -1).empty());
    for (const CharacterClass klass : kCharacterClasses) {
        CHECK(std::string(ClassName(klass)).size() > 2);
    }
    CHECK(std::string(ClassName(ClassFromId(4))) == "Sorceress" && std::string(ClassName(ClassFromId(5))) == "Wizard");
    CHECK(std::string(ClassName(ClassFromId(0))) == "Fighter" && std::string(ClassName(ClassFromId(1))) == "Amazon");
    CHECK(std::string(ClassName(ClassFromId(2))) == "Elf" && std::string(ClassName(ClassFromId(3))) == "Dwarf");
}

uint64_t Fnv1a(const std::vector<uint8_t>& bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (const uint8_t byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

size_t Opaque(const SpriteImage& image) {
    size_t count = 0;
    for (size_t p = 3; p < image.rgba.size(); p += 4) count += image.rgba[p] >= 128;
    return count;
}

// The installed USA game, when the source tree has one: every class's portrait and all five presets come out of the
// real files.
void TestRealArt(const fs::path& source_dir) {
    const fs::path app0 = source_dir / "game" / "prepared" / "v1.03";
    std::error_code ec;
    if (!fs::is_regular_file(app0 / "DRACRO.CPK", ec)) {
        std::printf("installed game not present, skipping the character art check\n");
        return;
    }
    CharacterArt art(ArtSourcesOf(app0, "DRACRO_US.CPK"));
    const char* const names[6][5] = {
        {"Sterling Silver", "Burnished Bronze", "Divine Gold", "True Steel", "Oxblood Copper"},
        {"Platinum Blonde", "Ash Blonde", "Honey Blonde", "Lilac Blonde", "Brunette"},
        {"Sunny Forest", "Rocky Brown", "Snow Aqua", "Maple Scarlet", "Grayish Grandeur"},
        {"Ivory White", "Crimson Red", "Ebony Dark", "Cedar Brown", "Amber Yellow"},
        {"Silky Black", "Deep Crow", "Crescent Night", "Dark Blue", "Wine Scarlet"},
        {"Black Sapphire", "Rhodolite Garnet", "Grayish Quartz", "Spinel Violet", "Blue Turquoise"},
    };
    int rendered = 0;
    for (const CharacterClass klass : kCharacterClasses) {
        const SpriteImage* face = art.Portrait(klass);
        CHECK(face != nullptr);
        if (face) {
            CHECK(face->width >= 90 && face->width <= 140 && face->height >= 90 && face->height <= 140);
            CHECK(Opaque(*face) > size_t(face->width * face->height) / 2);
        }
        std::set<uint64_t> hashes;
        for (int color = 0; color < kNativeColorCount; ++color) {
            CHECK(art.ColorName(klass, color, "US") == names[int(klass)][color]);
            const SpriteImage* figure = art.Preview(klass, color);
            CHECK(figure != nullptr);
            if (!figure) continue;
            ++rendered;
            CHECK(figure->width >= 300 && figure->width <= 700 && figure->height >= 380 && figure->height <= 700);
            CHECK(Opaque(*figure) > size_t(figure->width * figure->height) / 6);
            CHECK(figure->layer.size() == size_t(figure->width) * size_t(figure->height));
            hashes.insert(Fnv1a(figure->rgba));
        }
        CHECK(hashes.size() == size_t(kNativeColorCount)); // every preset is a different picture
        CHECK(art.Preview(klass, kNativeColorCount) == nullptr && art.Preview(klass, -1) == nullptr);
        CHECK(art.ColorName(klass, kNativeColorCount, "US").empty());
    }
    CHECK(rendered == 30);

    CHECK(art.ColorName(CharacterClass::kAmazon, 0, "FR") == "Blond platine");
    CHECK(art.ColorName(CharacterClass::kAmazon, 0, "ES") == "Rubio platino");
    CHECK(art.ColorName(CharacterClass::kWizard, 0, "FR") == "Saphir noir");
    CHECK(!art.ColorName(CharacterClass::kAmazon, 0, "").empty());

    const SpriteImage* platinum = art.Preview(CharacterClass::kAmazon, 0);
    const SpriteImage* brunette = art.Preview(CharacterClass::kAmazon, 4);
    CHECK(platinum && brunette);
    if (platinum && brunette) {
        CHECK(platinum->width == brunette->width && platinum->height == brunette->height);
        size_t same_layer = 0;
        for (size_t p = 0; p < platinum->layer.size(); ++p) same_layer += platinum->layer[p] == brunette->layer[p];
        CHECK(same_layer * 100 >= platinum->layer.size() * 98); // only edge alpha differs between painted atlases
        CHECK(Fnv1a(platinum->rgba) == 0xc7a2e5425ab369ebull);
        CHECK(Fnv1a(brunette->rgba) == 0x70f1ee801d1c6e14ull);
    }
}

#ifdef DCP_SOURCE_DIR
std::string ReadSource(const char* relative) {
    std::ifstream in(fs::path(DCP_SOURCE_DIR) / relative, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
#endif

const GameBuild* FindBuild(std::string_view label) {
    for (const GameBuild& build : SupportedBuilds()) {
        if (label == build.label) return &build;
    }
    return nullptr;
}

void MakeBuildFolder(const GameRoots& roots, const GameBuild& build) {
    fs::create_directories(roots.prepared / build.folder);
}

void WriteSaveFor(const fs::path& user, const char* title, const char* folder, const std::vector<uint8_t>& bytes) {
    WriteBytes(user / "savedata" / "1" / title / folder / "SAVE0.DAT", bytes);
}

void TestEditorModel(const fs::path& base) {
    const fs::path root = base / "model";
    fs::remove_all(root);
    const GameRoots roots{root / "game" / "prepared", root / "game" / "image"};
    const GameBuild* us100 = FindBuild("CUSA10454 1.00 USA");
    const GameBuild* us101 = FindBuild("CUSA10454 1.01 USA");
    const GameBuild* jp100 = FindBuild("CUSA07001 1.00 Japan");
    const GameBuild* asia = FindBuild("CUSA10209 1.00 Asia/Hong Kong");
    CHECK(us100 && us101 && jp100 && asia);
    if (!us100 || !us101 || !jp100 || !asia) return;
    for (const GameBuild* build : {us100, us101, jp100}) MakeBuildFolder(roots, *build);

    const std::vector<EditorBuild> builds = ListEditorBuilds(roots, us101);
    CHECK(builds.size() == 3);
    CHECK(builds.size() == 3 && builds[0].build == us100 && builds[1].build == us101 && builds[2].build == jp100);
    CHECK(builds.size() == 3 && !builds[0].configured && builds[1].configured && !builds[2].configured);
    CHECK(DefaultBuildIndex(builds) == 1);
    CHECK(DefaultBuildIndex(ListEditorBuilds(roots, nullptr)) == 0);
    CHECK(DefaultBuildIndex(ListEditorBuilds(roots, asia)) == 0); // the configured build is not installed
    CHECK(ListEditorBuilds(GameRoots{root / "nowhere", root / "nowhere"}, us100).empty());

    // US 1.00 and 1.01 are separate choices over the same savedata; the Japanese title has its own.
    const fs::path user = root / "user";
    const std::vector<uint8_t> save = MakeSyntheticSave();
    WriteSaveFor(user, "CUSA10454", "SAVEDATA00", save);
    WriteSaveFor(user, "CUSA10454", "SAVEDATA01", save);
    WriteSaveFor(user, "CUSA07001", "SAVEDATA00", save);
    std::vector<uint8_t> broken = save;
    broken[0] ^= 0xFF;
    WriteSaveFor(user, "CUSA10209", "SAVEDATA00", broken);
    const std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10454");
    CHECK(saves.size() == 4);
    const std::vector<size_t> for_us100 = SavesOfBuild(saves, builds[0]);
    const std::vector<size_t> for_us101 = SavesOfBuild(saves, builds[1]);
    const std::vector<size_t> for_jp = SavesOfBuild(saves, builds[2]);
    CHECK(for_us100.size() == 2 && for_us100 == for_us101);
    CHECK(for_jp.size() == 1);
    for (const size_t index : for_us100) CHECK(saves[index].title_id == "CUSA10454");
    for (const size_t index : for_jp) CHECK(saves[index].title_id == "CUSA07001");
    CHECK(for_us100.size() == 2 && saves[for_us100[0]].dir_name == "SAVEDATA00" && saves[for_us100[1]].dir_name == "SAVEDATA01");

    // Build description.
    GameCheck check;
    check.state = GameFileState::kValid;
    check.eboot_sha256 = "f9bff150c4c28507096e7e143a07492063ed89bf05ac6cb995a55a0946e3508a";
    const BuildInfo us = DescribeBuild(builds[0], roots, &check);
    CHECK(us.label == "CUSA10454 1.00 USA" && us.title_id == "CUSA10454" && us.version == "1.00" && us.region == "USA");
    CHECK(us.prepared == "game/prepared/v1.03");
    CHECK(us.archive == "DRACRO_US.CPK");
    CHECK(us.executable == "f9bff150c4c2...");
    CHECK(us.state == GameFileState::kValid);
    check.eboot_sha256 = "0123456789abcdef0123456789abcdef";
    CHECK(DescribeBuild(builds[0], roots, &check).executable == "0123456789ab...");
    CHECK(DescribeBuild(builds[0], roots, nullptr).executable == "f9bff150c4c2..." &&
          DescribeBuild(builds[0], roots, nullptr).state == GameFileState::kMissing);
    CHECK(DescribeBuild(builds[2], roots, nullptr).archive == "DRACRO.CPK");
    CHECK(DescribeBuild(builds[2], roots, nullptr).prepared == "game/prepared/CUSA07001-1.00");
    CHECK(ShortFingerprint("abc") == "abc" && ShortFingerprint("") == "");

    // Save description and choice labels.
    const DiscoveredSave& first = saves[for_us100[0]];
    const SaveInfo info = DescribeSaveFile(first);
    CHECK(info.title_id == "CUSA10454" && info.region == "USA" && info.characters == 2);
    CHECK(info.size == kSaveFileSize && info.crc == "Valid");
    CHECK(info.path.size() > 9 && info.path.substr(info.path.size() - 9) == "SAVE0.DAT");
    CHECK(info.modified.size() == 16 && info.modified[4] == '-' && info.modified[13] == ':');
    // The installed game line names the build as the Play page does: title id, version, region.
    const std::string label_us = builds[0].build->label, label_jp = builds[2].build->label;
    CHECK(label_us == "CUSA10454 1.00 USA" && label_jp == "CUSA07001 1.00 Japan");
    const std::string line = InstalledGameLine("Installed game", label_us, 1, "save found", "saves found");
    CHECK(line == "Installed game: CUSA10454 1.00 USA \xC2\xB7 1 save found");
    CHECK(InstalledGameLine("Installed game", label_us, 2, "save found", "saves found") ==
          "Installed game: CUSA10454 1.00 USA \xC2\xB7 2 saves found");
    CHECK(InstalledGameLine("Jogo instalado", label_jp, 2, "save encontrado", "saves encontrados") ==
          "Jogo instalado: CUSA07001 1.00 Japan \xC2\xB7 2 saves encontrados");
    // The separator is one real middle dot (U+00B7 as UTF-8), never its Latin-1 reading.
    CHECK(line.find("\xC3\x82") == std::string::npos && line.find(" \xC2\xB7 ") != std::string::npos);
    for (const std::string& text : {ReadSource("launcher/editor_model.cpp"), ReadSource("launcher/editor_page.cpp")}) {
        CHECK(text.find("\xC3\x82") == std::string::npos);
    }
    const DiscoveredSave* corrupt = nullptr;
    for (const DiscoveredSave& candidate : saves) {
        if (candidate.title_id == "CUSA10209") corrupt = &candidate;
    }
    CHECK(corrupt != nullptr);
    if (corrupt) {
        CHECK(DescribeSaveFile(*corrupt).crc != "Valid" && !DescribeSaveFile(*corrupt).crc.empty());
        CHECK(!corrupt->model.valid());
        CHECK(DescribeSaveFile(*corrupt).region == "Asia/Hong Kong");
    }

    // DATA 1..3 are records inside the file; SAVEDATA00 is the folder.
    CHECK(DataSlotName(0) == "DATA 1" && DataSlotName(1) == "DATA 2" && DataSlotName(2) == "DATA 3");
    CHECK(first.dir_name == "SAVEDATA00" && first.dir_name != DataSlotName(0));
    for (int slot = 0; slot < int(kDataSlotCount); ++slot) CHECK(first.dir_name.find(DataSlotName(slot)) == std::string::npos);
    const DataSlotInfo one = DescribeDataSlot(first.model, 0);
    CHECK(one.name == "DATA 1" && one.gold == 100 && one.characters == 2 && one.primary == "Eppie (Fighter)");
    const DataSlotInfo two = DescribeDataSlot(first.model, 1);
    CHECK(two.name == "DATA 2" && two.gold == 7757 && two.characters == 0 && two.primary.empty());
    CHECK(DescribeDataSlot(first.model, 3).characters == 0);
    CHECK(FirstPopulatedSlot(first.model) == 0 && FirstPopulatedSlot(SaveModel{}) == 0);
    // The Save Data combo lists every DATA slot of the one save: "DATA N - X characters".
    CHECK(DataSlotChoiceLabel(first.model, 0, "character", "characters") == "DATA 1 - 2 characters");
    CHECK(DataSlotChoiceLabel(first.model, 1, "character", "characters") == "DATA 2 - 0 characters");
    CHECK(DataSlotChoiceLabel(first.model, 2, "personagem", "personagens") == "DATA 3 - 0 personagens");
    CHECK(DataSlotChoiceLabel(SaveModel{}, 0, "character", "characters") == "DATA 1 - 0 characters");
}

std::string FunctionBody(const std::string& page, const std::string& signature);

size_t Count(const std::string& text, const std::string& needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
    return count;
}

void TestEditorLayoutAudit() {
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    CHECK(!page.empty());
    // Every Begin has its End, so no child, style, font, id or disabled scope is left open.
    const std::pair<const char*, const char*> pairs[] = {
        {"ImGui::BeginChild(", "ImGui::EndChild()"},       {"ImGui::BeginCombo(", "ImGui::EndCombo()"},
        {"ImGui::BeginDisabled(", "ImGui::EndDisabled()"}, {"ImGui::BeginGroup(", "ImGui::EndGroup()"},
        {"ImGui::BeginTable(", "ImGui::EndTable()"},       {"ImGui::PushID(", "ImGui::PopID()"},
        {"ImGui::PushStyleVar(", "ImGui::PopStyleVar()"},
        {"ImGui::PushFont(", "ImGui::PopFont()"},          {"ImGui::PushClipRect(", "ImGui::PopClipRect()"},
    };
    for (const auto& [begin, end] : pairs) CHECK(Count(page, begin) == Count(page, end));
    CHECK(Count(page, "ImGui::PushStyleColor(") == Count(page, "ImGui::PopStyleColor()") + 3 * Count(page, "ImGui::PopStyleColor(3)"));
    CHECK(Count(page, "ImGui::BeginChild(") == 1 && Count(page, "ImGui::EndChild()") == 1);
    CHECK(page.find("BeginChild(\"##editor_body\"") != std::string::npos);
    CHECK(page.find("BeginTable(\"##appearance\"") != std::string::npos);
    for (const char* text : {"not a customization file", "no parts that can be edited separately"}) {
        CHECK(page.find(text) == std::string::npos);
        CHECK(ReadSource("launcher/appearance_custom.cpp").find(text) == std::string::npos);
    }
    // The native colours are only the Base Color choices; there is no Custom row or preset list.
    CHECK(page.find("is_custom") == std::string::npos && page.find("const int preset = row - 1;") == std::string::npos);
    // Moving the cursor past the last item without submitting another is what Dear ImGui rejects.
    CHECK(page.find("SetCursorPos") == std::string::npos && page.find("SetCursorScreenPos") == std::string::npos);
    CHECK(page.find("SetCursorPosX") == std::string::npos && page.find("SetCursorPosY") == std::string::npos);
    // The tabs are the only section switch, and the scrolling body is a child under them.
    CHECK(page.find("Segmented(\"##editor_section\"") == std::string::npos);
    CHECK(page.find("DrawChanges(ui);") < page.find("BeginChild(\"##editor_body\""));
#endif
}

void TestEditorSections() {
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const std::string header = ReadSource("launcher/editor_page.h");
    CHECK(!page.empty() && !header.empty());
    CHECK(page.find("{Tr(\"Character\"), Tr(\"Save Data\")}") == std::string::npos);
    CHECK(page.find("Tr(\"Save Data\"), Tr(\"Appearance\")") == std::string::npos && header.find("Section") == std::string::npos);
    CHECK(page.find("art_->Portrait(") != std::string::npos && page.find("Tr(\"Asset unavailable\")") != std::string::npos);
    // The green Editor status card is gone; the launcher's own page title and blurb head the page.
    CHECK(page.find("StatusCard") == std::string::npos && page.find("Tr(\"Editing\")") == std::string::npos);
    CHECK(page.find("Tr(\"Progression\")") == std::string::npos && page.find("Tr(\"Equipment\")") == std::string::npos);
    CHECK(Count(page, "Segmented(\"##editor_section\"") == 0 && Count(page, "Tr(\"Save Data\")") == 1 && Count(page, "Tr(\"Character\")") == 0);
    // The Editor opens on the saves of the installed game: Save Data row, game line, then loading or the characters.
    const std::string flow = FunctionBody(page, "void EditorPage::DrawCharacter(");
    const size_t choice = flow.find("DrawSaveData(ui);");
    const size_t line = flow.find("InstalledGameLine(");
    const size_t list = flow.find("DrawCharacterList(ui);");
    CHECK(choice != std::string::npos && choice < line && line < list);
    CHECK(flow.find("SelectedBuild()->build->label") != std::string::npos && flow.find("FriendlyBuildText") == std::string::npos);
    for (const char* gone : {"DrawBuild", "DrawSave(", "DrawDataSlot", "DrawTransfer", "Tr(\"Game Build\")", "select_build",
                             "SelectBuild", "Tr(\"Gold\")", "kGoldRange", "Tr(\"Save path\")", "Tr(\"Savedata folder\")",
                             "Tr(\"Modified\")", "InfoGrid", "SaveChoiceLabel", "BuildChoiceLabel", "Save 1", "select_save", "SelectSave", "save_loader",
                             "Loading characters", "Headgear", "HeadgearRow"}) {
        CHECK(page.find(gone) == std::string::npos);
    }
    CHECK(header.find("DrawSave(") == std::string::npos && header.find("DrawBuild") == std::string::npos);
    for (const char* dead : {"DrawPortraitPlaceholder", "ClassAccent(character.klass)[0]", "Custom Colors", "Custom Color",
                             "Skin", "Primary clothing", "Secondary clothing", "Armor", "Accessories",
                             "AppearanceStore", "PresetStore", "AppearancePart"}) {
        CHECK(page.find(dead) == std::string::npos);
        CHECK(header.find(dead) == std::string::npos);
    }
    // One compact row of pending-changes controls, drawn above the section tabs.
    const size_t warning = page.find("Tr(row.status)");
    const size_t apply = page.find("Tr(\"Apply Changes\")");
    const size_t discard = page.find("Tr(\"Discard Changes\")");
    CHECK(warning != std::string::npos && apply != std::string::npos && discard != std::string::npos);
    CHECK(page.find("DrawChanges(ui);") < page.find("DrawCharacter(ui);"));
    // Only Apply Changes writes the save; the launcher's own Save never does.
    CHECK(page.find("ApplyAll(") != std::string::npos && page.find("edit_->Apply()") == std::string::npos);
    const std::string launcher = ReadSource("launcher/launcher.cpp");
    CHECK(launcher.find("Save keeps the launcher settings. Play saves them and starts the game.") != std::string::npos);
    CHECK(launcher.find("editor_page_->dirty()") != std::string::npos);

    const std::string portuguese = ReadSource("launcher/localization/pt_br_ui_text.cpp");
    for (const char* dead : {"Custom Colors\t", "Primary clothing\t",
                             "Native Color\t", "Current preset\t"}) {
        CHECK(portuguese.find(dead) == std::string::npos);
    }
    for (const char* text : {"Part Color\t", "Edit Appearance\t", "Unable to load appearance\t", "Invalid appearance value\t",
                             "Asset unavailable\t", "Loading appearance...\tCarregando apar\xC3\xAAncia...\n"}) {
        CHECK(portuguese.find(text) != std::string::npos);
    }
    for (const char* dead : {"\nCurrent Color\t", "\nSaved Color\t", "\nColor\t", "\nChoose Custom to edit"}) {
        CHECK(portuguese.find(dead) == std::string::npos);
    }
#endif
}

void TestActionRow() {
    const ActionRow clean = ActionRowFor(false);
    const ActionRow dirty = ActionRowFor(true);
    CHECK(!clean.apply_enabled && !clean.apply_accent && !clean.discard_enabled && std::string(clean.status).empty());
    CHECK(dirty.apply_enabled && dirty.apply_accent && dirty.discard_enabled && std::string(dirty.status) == "Unsaved changes");
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const size_t start = page.find("void EditorPage::DrawChanges(");
    const size_t end = page.find("\n}\n", start);
    const std::string row = page.substr(start, end - start);
    // Drawn every frame in the same order; only enabled state, accent and the right-hand text follow the changes.
    CHECK(row.find("return;") == std::string::npos);
    CHECK(row.find("Tr(\"Apply Changes\")") < row.find("Tr(\"Discard Changes\")"));
    CHECK(row.find("Tr(\"Discard Changes\")") < row.find("Tr(row.status)"));
    CHECK(row.find("BeginDisabled(!row.apply_enabled)") != std::string::npos);
    CHECK(row.find("BeginDisabled(!row.discard_enabled)") != std::string::npos);
    CHECK(page.find("    DrawChanges(ui);") < page.find("DrawCharacter(ui);"));
#endif
}

void TestSaveDataText() {
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    CHECK(page.find("Installed Build") == std::string::npos && page.find("Installed build") == std::string::npos);
    CHECK(Count(page, "Tr(\"Game Build\")") == 0);
    CHECK(page.find("Tr(\"Compatible with PS4, PS3 and PS Vita saves.\")") == std::string::npos);
    const std::string portuguese = ReadSource("launcher/localization/pt_br_ui_text.cpp");
    for (const std::string& text : {page, portuguese, ReadSource("README.md"), ReadSource("launcher/save_import.cpp"),
                                    ReadSource("launcher/save_import.h")}) {
        std::string lower = text;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        CHECK(lower.find("apollo") == std::string::npos);
    }
    CHECK(portuguese.find("Compatible with PS4, PS3 and PS Vita saves.\t") != std::string::npos);
#endif
}

void TestCustomStore(const fs::path& base) {
    const fs::path path = base / "custom" / "character_customization.dat";
    fs::remove_all(path.parent_path());
    CustomStore store(path);
    CHECK(store.Load() && store.entries().empty());
    const CustomKey vivian{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kAmazon};
    const CustomKey other_slot{"CUSA10454", "SAVEDATA00", 0, 1, CharacterClass::kAmazon};
    const CustomKey other_title{"CUSA07001", "SAVEDATA00", 0, 0, CharacterClass::kAmazon};
    CHECK(!(vivian == other_slot) && !(vivian == other_title));
    CHECK(vivian.Text() == "CUSA10454|SAVEDATA00|0|0|1");

    CustomLook look{true, 3, {}};
    look.Edit("hair").color = Rgb{10, 20, 30};
    look.Edit("tattoo").hidden = true;
    look.Edit("unused"); // no change: dropped
    store.Set(vivian, look);
    CHECK(store.Get(vivian) && store.Get(vivian)->parts.size() == 2);
    CHECK(!store.Get(other_slot) && !store.Get(other_title));
    CHECK(store.Save());

    CustomStore reloaded(path);
    CHECK(reloaded.Load() && reloaded == store);
    const std::optional<CustomLook> got = reloaded.Get(vivian);
    CHECK(got && got->enabled && got->base == 3);
    CHECK((got && got->Find("hair") && got->Find("hair")->color == (Rgb{10, 20, 30}) && !got->Find("hair")->hidden));
    CHECK(got && got->Find("tattoo") && got->Find("tattoo")->hidden && !got->Find("tattoo")->color);

    // Changing a value and putting it back is no change.
    CustomStore working = reloaded;
    CustomLook edited = *working.Get(vivian);
    edited.Edit("hair").color = Rgb{99, 99, 99};
    working.Set(vivian, edited);
    CHECK(!(working == reloaded));
    edited.Edit("hair").color = Rgb{10, 20, 30};
    working.Set(vivian, edited);
    CHECK(working == reloaded);

    // Reset Custom: no parts and not enabled removes the entry; no entry is the native look.
    CustomLook cleared{false, 3, {}};
    working.Set(vivian, cleared);
    CHECK(!working.Get(vivian) && working.entries().empty());

    // A newer or foreign file is refused and never overwritten.
    std::vector<uint8_t> bytes = store.Serialize();
    bytes[4] = 9;
    CustomStore newer(path);
    CHECK(!newer.Parse(bytes) && !newer.error().empty());
    CHECK(!newer.Save());
    std::vector<uint8_t> truncated = store.Serialize();
    truncated.resize(truncated.size() - 2);
    CHECK(!CustomStore(path).Parse(truncated));
}

void TestApplyAll(const fs::path& base) {
    const fs::path user = base / "apply_user";
    const SaveModel model = ModelWithColors({{1, 0}, {4, 2}});
    const std::vector<uint8_t> original(model.bytes().begin(), model.bytes().end());
    const fs::path file = WriteEditableSave(user, original);
    CustomStore saved(CustomStorePath(user));
    CHECK(saved.Load());
    const CustomKey key{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kAmazon};

    // Save and sidecar together.
    SaveEdit edit = OpenEdit(user);
    CustomStore working = saved;
    CustomLook look{true, 0, {}};
    look.Edit("hair").color = Rgb{200, 40, 40};
    working.Set(key, look);
    CHECK(edit.working().SetColor(0, 0, 2));
    ApplyOutcome outcome = ApplyAll(&edit, working, saved);
    CHECK(outcome.ok() && outcome.custom_written && outcome.save == SaveEdit::Result::kApplied);
    CHECK(saved == working && !edit.dirty());
    CHECK(SaveModel::FromFile(file).Character(0, 0)->color == 2);
    CustomStore on_disk(CustomStorePath(user));
    CHECK(on_disk.Load() && on_disk == working);

    // Sidecar only: the save is untouched.
    std::vector<uint8_t> before;
    CHECK(ReadBytes(file, before));
    look.Edit("hair").hidden = true;
    working.Set(key, look);
    outcome = ApplyAll(&edit, working, saved);
    CHECK(outcome.ok() && outcome.save == SaveEdit::Result::kNothingToApply && saved == working);
    std::vector<uint8_t> after;
    CHECK(ReadBytes(file, after) && after == before);

    // The save cannot be written: the sidecar goes back to what it was and nothing reports applied.
    const CustomStore committed = saved;
    look.Edit("hair").color = Rgb{1, 2, 3};
    working.Set(key, look);
    CHECK(edit.working().SetColor(0, 0, 4));
    fs::remove(file);
    outcome = ApplyAll(&edit, working, saved);
    CHECK(!outcome.ok() && outcome.save == SaveEdit::Result::kBackupFailed && outcome.rolled_back && !outcome.custom_written);
    CHECK(saved == committed);
    CustomStore restored(CustomStorePath(user));
    CHECK(restored.Load() && restored == committed);
}

void TestRecolor() {
    const Rgb target{110, 130, 170};
    const float part_luma = 120.0f;
    const std::array<uint8_t, 4> dark{90, 80, 70, 200}, light{170, 160, 140, 77};
    const std::array<uint8_t, 4> a = Recolor(dark, target, part_luma), b = Recolor(light, target, part_luma);
    CHECK(a[3] == 200 && b[3] == 77); // alpha kept
    // The shading survives: the luma difference between two texels is kept within rounding.
    const float in = Luma(light[0], light[1], light[2]) - Luma(dark[0], dark[1], dark[2]);
    const float out = Luma(b[0], b[1], b[2]) - Luma(a[0], a[1], a[2]);
    CHECK(std::fabs(in - out) < 2.0f);
    // A texel at the part's mean luma becomes the target colour.
    const std::array<uint8_t, 4> mean = Recolor({120, 120, 120, 255}, target, 120.0f);
    CHECK(mean[0] == 110 && mean[1] == 130 && mean[2] == 170);
    CHECK(std::fabs(Luma(a[0], a[1], a[2]) - Luma(dark[0], dark[1], dark[2]) - (Luma(110, 130, 170) - part_luma)) < 1.5f);
}

void TestBc7Encode() {
    uint8_t solid[64], block[16], back[64];
    for (int t = 0; t < 16; ++t) {
        solid[t * 4] = 130; solid[t * 4 + 1] = 40; solid[t * 4 + 2] = 220; solid[t * 4 + 3] = 255;
    }
    EncodeBc7Block(solid, block);
    DecodeBc7Block(block, back);
    int solid_error = 0;
    for (int i = 0; i < 64; ++i) solid_error = std::max(solid_error, std::abs(int(solid[i]) - int(back[i])));
    CHECK(solid_error <= 1); // one p-bit is shared by an endpoint's four channels
    uint8_t ramp[64];
    for (int t = 0; t < 16; ++t) {
        ramp[t * 4] = uint8_t(t * 16); ramp[t * 4 + 1] = uint8_t(255 - t * 12); ramp[t * 4 + 2] = 90; ramp[t * 4 + 3] = uint8_t(255 - t * 8);
    }
    EncodeBc7Block(ramp, block);
    DecodeBc7Block(block, back);
    int worst = 0;
    for (int i = 0; i < 64; ++i) worst = std::max(worst, std::abs(int(ramp[i]) - int(back[i])));
    CHECK(worst <= 4);
    // A part hidden beside one still seen, the hidden texels padded with the colours beside them: the hidden texels come
    // back fully transparent and the seen ones fully opaque, close to their colour.
    uint8_t edge[64];
    for (int t = 0; t < 16; ++t) {
        const bool hidden = (t & 3) < 2 || t == 6;
        const uint8_t shade = uint8_t(60 + (t >> 2) * 40);
        edge[t * 4] = uint8_t(150 + (t & 3) * 20); edge[t * 4 + 1] = shade; edge[t * 4 + 2] = uint8_t(255 - shade); edge[t * 4 + 3] = hidden ? 0 : 255;
    }
    EncodeBc7Block(edge, block);
    DecodeBc7Block(block, back);
    int color_error = 0;
    for (int t = 0; t < 16; ++t) {
        CHECK(back[t * 4 + 3] == edge[t * 4 + 3]);
        if (!edge[t * 4 + 3]) continue;
        for (int c = 0; c < 3; ++c) color_error = std::max(color_error, std::abs(int(edge[t * 4 + c]) - int(back[t * 4 + c])));
    }
    CHECK(color_error <= 48);
}

void TestCustomParts() {
    for (const CharacterClass klass : kCharacterClasses) {
        const std::span<const CustomPart> parts = CustomPartsOf(klass);
        CHECK(parts.size() >= 4);
        uint16_t all = 0;
        for (const CustomPart& part : parts) {
            CHECK((part.regions != 0 || part.HasPieces() || part.weapon >= 0) && (all & part.regions) == 0); // a region belongs to one part
            all |= part.regions;
            CHECK(FindCustomPart(klass, part.key) == &part);
            for (uint8_t r = 1; r <= kRegionCount; ++r) {
                if (part.regions >> (r - 1) & 1) CHECK(PartOfRegion(klass, r) == &part);
            }
        }
        // Every learned region is editable, except where only pieces own it: the Wizard's spell effects and spellbook share
        // colours with the robe, and the Elf's hair, eyes, laces and accessories share colour clusters with cloth.
        const uint16_t effects = klass == CharacterClass::kWizard ? uint16_t(0xFF) : klass == CharacterClass::kElf ? uint16_t(0x74) : uint16_t(0);
        CHECK(all == uint16_t(((1u << kRegionCount) - 1) & ~effects));
        CHECK(PartOfRegion(klass, 0) == nullptr && PartOfRegion(klass, kRegionCount + 1) == nullptr);
    }
    CHECK(FindCustomPart(CharacterClass::kAmazon, "hair") && FindCustomPart(CharacterClass::kSorceress, "hair"));
    CHECK(FindCustomPart(CharacterClass::kFighter, "armor") && FindCustomPart(CharacterClass::kDwarf, "beard") &&
          FindCustomPart(CharacterClass::kElf, "hood") && FindCustomPart(CharacterClass::kWizard, "robe"));
}

void TestRegionMapFile() {
    RegionMap map{4, 3, {0, 1, 1, 1, 2, 2, 0, 0, 8, 8, 8, 3}, {}};
    const std::vector<uint8_t> bytes = SerializeRegionMap(map, "game");
    const std::optional<RegionMap> back = ParseRegionMap(bytes, "game");
    CHECK(back && back->width == 4 && back->height == 3 && back->region == map.region);
    CHECK(!ParseRegionMap(bytes, "other game")); // made for other game files: learned again
    std::vector<uint8_t> broken = bytes;
    broken.resize(broken.size() - 1);
    CHECK(!ParseRegionMap(broken, "game"));
}

// The installed USA game: Custom on every class, every part, through the preview and the in-game atlas.
void TestRealCustom(const fs::path& base, const fs::path& source_dir) {
    const fs::path app0 = source_dir / "game" / "prepared" / "v1.03";
    std::error_code ec;
    if (!fs::is_regular_file(app0 / "DRACRO.CPK", ec)) {
        std::printf("installed game not present, skipping the custom appearance check\n");
        return;
    }
    CharacterArtSources sources = ArtSourcesOf(app0, "DRACRO_US.CPK");
    sources.cache_dir = base / "region_cache";
    CharacterArt art(sources);
    CpkArchive cpk;
    CHECK(cpk.Open(app0 / "DRACRO.CPK"));
    for (const CharacterClass klass : kCharacterClasses) {
        const std::vector<RegionMap>* maps = art.Regions(klass);
        CHECK(maps != nullptr && maps->size() == BodySurfacesOf(klass).size());
        if (!maps) continue;
        std::vector<std::vector<uint8_t>> packs;
        for (int color = 0; color < kNativeColorCount; ++color) {
            packs.push_back(cpk.Read(*cpk.Find(AtlasName(klass, color))));
            for (const BodySurface& body : BodySurfacesOf(klass)) CHECK(ReadFtxSurface(packs.back(), body.surface).has_value());
        }
        // Learned once, then read back identically from the cache.
        CharacterArt cached(sources);
        const std::vector<RegionMap>* again = cached.Regions(klass);
        CHECK(again && again->front().region == maps->front().region);

        const SpriteImage native = *art.Preview(klass, 0);
        std::map<std::string_view, size_t> visible;
        for (int y = 0; y < native.height; ++y) {
            for (int x = 0; x < native.width; ++x) {
                if (const CustomPart* part = art.PartAt(klass, native, x, y)) ++visible[part->key];
            }
        }
        CHECK(visible.size() >= 3); // several regions resolve from clicks on the figure
        size_t tested = 0;
        for (const CustomPart& part : CustomPartsOf(klass)) {
            if (part.weapon >= 0) continue;
            size_t texels = 0;
            const uint16_t piece_bit = PieceBitOf(klass, part);
            for (const RegionMap& map : *maps) {
                for (const uint8_t r : map.region) texels += r && (part.regions >> (r - 1) & 1);
                for (const uint16_t pieces : map.pieces) texels += (pieces & piece_bit) != 0;
            }
            CHECK(texels > 0);
            if (visible[part.key] < 200 || tested >= 2) continue;
            ++tested;
            for (int color = 0; color < kNativeColorCount; ++color) {
                const SpriteImage before = *art.Preview(klass, color);
                CustomLook look{true, uint8_t(color), {}};
                look.Edit(part.key).color = Rgb{40, 160, 90};
                const SpriteImage recolored = *art.CustomPreview(klass, color, look);
                CustomLook gone{true, uint8_t(color), {}};
                gone.Edit(part.key).hidden = true;
                const SpriteImage hidden = *art.CustomPreview(klass, color, gone);
                size_t alpha_changed = 0, changed = 0, untouched_changed = 0, still_shown = 0;
                for (size_t p = 0; p < before.layer.size(); ++p) {
                    int to_recolored = 0, to_hidden = 0;
                    for (int c = 0; c < 4; ++c) {
                        to_recolored = std::max(to_recolored, std::abs(int(before.rgba[p * 4 + c]) - int(recolored.rgba[p * 4 + c])));
                        to_hidden = std::max(to_hidden, std::abs(int(before.rgba[p * 4 + c]) - int(hidden.rgba[p * 4 + c])));
                    }
                    alpha_changed += before.rgba[p * 4 + 3] != recolored.rgba[p * 4 + 3];
                    changed += to_recolored > 1;
                    untouched_changed += to_recolored > 1 && to_hidden == 0;
                    still_shown += art.PartAt(klass, hidden, int(p % size_t(hidden.width)), int(p / size_t(hidden.width))) == &part;
                }
                CHECK(alpha_changed == 0);               // recolouring keeps alpha
                CHECK(changed >= visible[part.key] / 2); // the part changes
                CHECK(untouched_changed * 20 <= visible[part.key] + 20); // pixels the part does not reach stay
                CHECK(still_shown * 50 <= visible[part.key] + 50);       // hidden: only filtered edges remain
            }
            // The in-game atlas: same shader, only touched blocks re-encoded, the rest byte-identical.
            CustomLook look{true, 0, {}};
            look.Edit(part.key).color = Rgb{40, 160, 90};
            CHECK(CustomizeAtlas(packs[0], *maps, klass, CustomLook{true, 0, {}}) == packs[0]);
            const std::optional<std::vector<uint8_t>> custom = CustomizeAtlas(packs[0], *maps, klass, look);
            CHECK(custom && *custom != packs[0]);
            if (!custom) continue;
            const BodySurface body = BodySurfacesOf(klass).front();
            const RegionMap& map = maps->front();
            const std::optional<FtxSurfaceLayout> layout = FtxLayoutOf(packs[0], body.surface);
            const std::optional<FtxSurface> original = ReadFtxSurface(packs[0], body.surface);
            const std::optional<FtxSurface> second = ReadFtxSurface(packs[0], body.surface);
            const std::optional<FtxSurface> served = ReadFtxSurface(*custom, body.surface);
            const LookShader preview(klass, look, map, [&](int x, int y) { return original->Texel(x, y); });
            const LookShader runtime(klass, look, map, [&](int x, int y) { return second->Texel(x, y); });
            uint64_t preview_hash = 1469598103934665603ull, runtime_hash = preview_hash;
            double error = 0.0;
            size_t samples = 0, outside = 0;
            for (int by = 0; by < layout->height / 4; by += 3) {
                for (int bx = 0; bx < layout->width / 4; bx += 3) {
                    bool touched = false;
                    for (int t = 0; t < 16; ++t) touched |= preview.ChangesAt(bx * 4 + (t & 3), by * 4 + (t >> 2));
                    const size_t at = FtxBlockOffset(*layout, bx, by);
                    if (!touched) {
                        outside += std::memcmp(packs[0].data() + at, custom->data() + at, 16) != 0;
                        continue;
                    }
                    for (int t = 0; t < 16; ++t) {
                        const int x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                        std::array<uint8_t, 4> a = original->Texel(x, y), b = second->Texel(x, y);
                        preview.ApplyAt(x, y, a);
                        runtime.ApplyAt(x, y, b);
                        for (int c = 0; c < 4; ++c) {
                            preview_hash = (preview_hash ^ a[c]) * 1099511628211ull;
                            runtime_hash = (runtime_hash ^ b[c]) * 1099511628211ull;
                        }
                        const std::array<uint8_t, 4> s = served->Texel(x, y);
                        for (int c = 0; c < 4; ++c) error += std::abs(int(s[c]) - int(a[c]));
                        samples += 4;
                    }
                }
            }
            CHECK(preview_hash == runtime_hash);
            CHECK(outside == 0);
            CHECK(samples > 0 && error / double(samples) < 4.0); // BC7 re-encoding stays close to the preview
        }
        CHECK(tested >= 1);
        // Custom over every base preset gives a different picture.
        std::set<uint64_t> looks;
        const CustomPart& first = CustomPartsOf(klass).front();
        for (int color = 0; color < kNativeColorCount; ++color) {
            CustomLook look{true, uint8_t(color), {}};
            look.Edit(first.key).color = Rgb{200, 60, 60};
            looks.insert(Fnv1a(art.CustomPreview(klass, color, look)->rgba));
        }
        CHECK(looks.size() == size_t(kNativeColorCount));
    }
}

// The game's identity block of a saved character: account bytes, a serial per character, the DATA slot last.
std::array<uint8_t, kCharIdentityBytes> IdentityFor(int slot, uint8_t serial) {
    return {0x02, 0xdc, 0x50, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, serial, 0x00, 0x00, 0x00, 0x00, uint8_t(slot)};
}

// Characters of DATA 1 as {class, preset}, each with its own identity (serials 0x1d, 0x1e, ...).
std::vector<uint8_t> SaveWithCharacters(const std::vector<std::vector<int>>& rows, bool identities = true) {
    std::vector<uint8_t> bytes = MakeSyntheticSave();
    for (size_t character = 0; character < rows.size(); ++character) {
        const size_t record = RecordOffset(0, int(character));
        if (character >= 2) SetCharacter(bytes, 0, int(character), "Extra", 200, 3, 2);
        bytes[record + kCharClass] = uint8_t(rows[character][0]);
        bytes[record + kCharColor] = uint8_t(rows[character][1]);
        if (identities) {
            const auto identity = IdentityFor(0, uint8_t(0x1d + character));
            std::memcpy(bytes.data() + record + kCharIdentity, identity.data(), identity.size());
        }
    }
    PutU32(bytes, kSaveCrcOffset, SaveCrc32(bytes));
    return bytes;
}

CustomLook HairLook(Rgb color) {
    CustomLook look{true, 3, {}};
    look.Edit("hair").color = color;
    return look;
}

void TestInstanceKeys() {
    const CustomLook blue = HairLook({30, 90, 230}), red = HairLook({200, 0, 0});
    CHECK(CustomLookHash(blue) == CustomLookHash(HairLook({30, 90, 230})));
    CHECK(CustomLookHash(blue) != CustomLookHash(red));
    CustomLook hidden = blue;
    hidden.Edit("hair").hidden = true;
    CHECK(CustomLookHash(hidden) != CustomLookHash(blue));
    CustomInstanceKey key{0, 0, CharacterClass::kAmazon, 3, CustomLookHash(blue)};
    const std::string entry = CustomInstanceEntry(key);
    CHECK(entry.rfind("PLAYER/AMAZON03_D1C01_", 0) == 0 && entry.size() == std::string("PLAYER/AMAZON03_D1C01_12345678.FTX").size());
    CHECK(entry.substr(entry.size() - 4) == ".FTX");
    CHECK(key.Text().rfind("DATA 1 slot 1 Amazon colour 3 look ", 0) == 0);
    CustomInstanceKey other_slot = key;
    other_slot.character_slot = 1;
    CustomInstanceKey other_data = key;
    other_data.data_slot = 2;
    CustomInstanceKey other_look = key;
    other_look.look_hash = CustomLookHash(red);
    CustomInstanceKey other_base = key;
    other_base.base = 2;
    std::set<std::string> entries{entry};
    for (const CustomInstanceKey& changed : {other_slot, other_data, other_look, other_base}) {
        CHECK(changed != key);
        entries.insert(CustomInstanceEntry(changed));
    }
    CHECK(entries.size() == 5); // DATA slot, character slot, preset and look each make another instance
    CHECK(CustomInstanceEntry(other_base).rfind("PLAYER/AMAZON02_D1C01_", 0) == 0);
    CHECK(CustomInstanceEntry({0, 0, CharacterClass::kAmazon, 7, 1}).empty());
}

void TestRuntimeInstances(const fs::path& base) {
    const fs::path user = base / "runtime_user";
    fs::remove_all(user);
    WriteSaveFor(user, "CUSA10454", "SAVEDATA00", SaveWithCharacters({{1, 3}, {1, 3}, {1, 3}}));
    const std::vector<DiscoveredSave> saves = DiscoverSaves(user, "CUSA10454");
    CustomStore store(CustomStorePath(user));
    std::vector<std::string> notes;
    CHECK(PlanCustomInstances(store, saves, "CUSA10454", notes).empty()); // no sidecar: native

    const CustomKey first{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kAmazon};
    const CustomKey second{"CUSA10454", "SAVEDATA00", 0, 1, CharacterClass::kAmazon};
    const CustomKey third{"CUSA10454", "SAVEDATA00", 0, 2, CharacterClass::kAmazon};
    const CustomLook blue = HairLook({30, 90, 230}), red = HairLook({200, 0, 0});
    store.Set(first, blue);
    std::vector<CustomInstance> planned = PlanCustomInstances(store, saves, "CUSA10454", notes);
    CHECK(planned.size() == 1 && planned[0].key == first && planned[0].instance.base == 3);
    CHECK(planned.size() == 1 && planned[0].identity == IdentityFor(0, 0x1d));
    CHECK(PlanCustomInstances(store, saves, "CUSA07001", notes).empty());

    CustomLook disabled = blue;
    disabled.enabled = false;
    store.Set(first, disabled);
    CHECK(PlanCustomInstances(store, saves, "CUSA10454", notes).empty()); // Native chosen: edits kept, not served

    // Two Amazons on the same preset with different looks: both custom, each on its own atlas.
    store.Set(first, blue);
    store.Set(second, red);
    notes.clear();
    planned = PlanCustomInstances(store, saves, "CUSA10454", notes);
    CHECK(planned.size() == 2 && notes.empty());
    if (planned.size() == 2) {
        CHECK(planned[0].entry != planned[1].entry && planned[0].identity != planned[1].identity);
        CHECK(planned[0].instance.base == planned[1].instance.base && planned[0].instance.klass == planned[1].instance.klass);
        CHECK(planned[0].look == blue && planned[1].look == red);
    }
    // The same look on two characters is still two instances: nothing is shared between characters.
    store.Set(second, blue);
    planned = PlanCustomInstances(store, saves, "CUSA10454", notes);
    CHECK(planned.size() == 2 && planned[0].entry != planned[1].entry);
    // Three of a kind: two custom looks, the third stays native.
    store.Set(second, red);
    planned = PlanCustomInstances(store, saves, "CUSA10454", notes);
    CHECK(planned.size() == 2);
    for (const CustomInstance& instance : planned) CHECK(!(instance.key == third));
    // Planning again gives the same instances.
    const std::vector<CustomInstance> again = PlanCustomInstances(store, saves, "CUSA10454", notes);
    CHECK(again.size() == planned.size() && again[0].entry == planned[0].entry && again[1].entry == planned[1].entry);

    const CustomKey wrong_class{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kWizard};
    CustomStore mismatched(CustomStorePath(user));
    mismatched.Set(wrong_class, blue);
    CHECK(PlanCustomInstances(mismatched, saves, "CUSA10454", notes).empty());

    // A save without identities cannot be told apart in the game: native, with a note.
    const fs::path blank_user = base / "runtime_blank";
    fs::remove_all(blank_user);
    WriteSaveFor(blank_user, "CUSA10454", "SAVEDATA00", SaveWithCharacters({{1, 3}, {1, 3}}, false));
    notes.clear();
    CHECK(PlanCustomInstances(store, DiscoverSaves(blank_user, "CUSA10454"), "CUSA10454", notes).empty() && !notes.empty());

    // One character saved in two folders: one look serves it, two different looks leave it native.
    const fs::path twice = base / "runtime_twice";
    fs::remove_all(twice);
    WriteSaveFor(twice, "CUSA10454", "SAVEDATA00", SaveWithCharacters({{1, 3}}));
    WriteSaveFor(twice, "CUSA10454", "SAVEDATA01", SaveWithCharacters({{1, 3}}));
    const std::vector<DiscoveredSave> both = DiscoverSaves(twice, "CUSA10454");
    CustomStore copies(CustomStorePath(twice));
    copies.Set(first, blue);
    copies.Set({"CUSA10454", "SAVEDATA01", 0, 0, CharacterClass::kAmazon}, blue);
    CHECK(PlanCustomInstances(copies, both, "CUSA10454", notes).size() == 1);
    copies.Set({"CUSA10454", "SAVEDATA01", 0, 0, CharacterClass::kAmazon}, red);
    notes.clear();
    CHECK(PlanCustomInstances(copies, both, "CUSA10454", notes).empty() && !notes.empty());
}

// ---- the hooks, on records and units laid out in host memory as the game lays them out ----

struct FakeCharacter {
    std::vector<uint8_t> record = std::vector<uint8_t>(0x200, 0);
    std::vector<uint8_t> unit = std::vector<uint8_t>(0xb00, 0);
    std::vector<uint8_t> owner = std::vector<uint8_t>(0x3a00, 0);
    FakeCharacter(int klass, int color, const std::array<uint8_t, kCharIdentityBytes>& identity) {
        record[APPEARANCE_RECORD_CLASS] = uint8_t(klass);
        record[APPEARANCE_RECORD_COLOR] = uint8_t(color);
        std::memcpy(record.data() + APPEARANCE_RECORD_IDENTITY, identity.data(), identity.size());
        unit[0xcb] = uint8_t(klass);
        unit[0xcc] = uint8_t(color);
        const uint64_t unit_address = Address(unit);
        std::memcpy(owner.data() + 0x9b8, &unit_address, 8);
        const uint16_t class_word = uint16_t(klass);
        std::memcpy(owner.data() + 0x39e0, &class_word, 2);
    }
    static uint64_t Address(const std::vector<uint8_t>& bytes) { return uint64_t(uintptr_t(bytes.data())); }
};

std::vector<const FakeCharacter*> g_fake_party;

uint64_t FakeRecordOfUnit(uint64_t unit) {
    for (const FakeCharacter* character : g_fake_party) {
        if (FakeCharacter::Address(character->unit) == unit) return FakeCharacter::Address(character->record);
    }
    return 0;
}

AppearanceInstance InstanceFor(const std::array<uint8_t, kCharIdentityBytes>& identity, int klass, int color, const char* entry,
                               const char* label) {
    AppearanceInstance instance{};
    std::memcpy(instance.identity, identity.data(), identity.size());
    instance.klass = uint8_t(klass);
    instance.color = uint8_t(color);
    std::snprintf(instance.entry, sizeof(instance.entry), "%s", entry);
    std::snprintf(instance.label, sizeof(instance.label), "%s", label);
    return instance;
}

uint64_t UnitAtlas(const FakeCharacter& character, uint64_t native) {
    GuestRegs regs{};
    regs.rax = FakeCharacter::Address(character.unit);
    regs.rdi = character.unit[0xcb];
    regs.rsi = character.unit[0xcc];
    appearance_unit_atlas(&regs);
    return regs.skip ? regs.rax : native;
}

void TestAppearanceHandlers() {
    const FakeCharacter vivian(1, 3, IdentityFor(0, 0x1d)), nicky(1, 3, IdentityFor(0, 0x1e)), zora(1, 3, IdentityFor(0, 0x1f));
    const FakeCharacter other_slot(1, 3, IdentityFor(1, 0x1d)); // the same serial in DATA 2
    g_fake_party = {&vivian, &nicky, &zora, &other_slot};
    appearance_hooks_set_record_of_unit(FakeRecordOfUnit);
    const AppearanceInstance served[] = {
        InstanceFor(IdentityFor(0, 0x1d), 1, 3, "PLAYER/AMAZON03_D1C01_AAAAAAAA.FTX", "DATA 1 slot 1 Amazon colour 3"),
        InstanceFor(IdentityFor(0, 0x1e), 1, 3, "PLAYER/AMAZON03_D1C02_BBBBBBBB.FTX", "DATA 1 slot 2 Amazon colour 3"),
    };
    const uint64_t native = 0x5151;

    appearance_hooks_set_instances(nullptr, 0);
    CHECK(UnitAtlas(vivian, native) == native); // no instances: every character native

    appearance_hooks_set_instances(served, 2);
    const uint64_t first = UnitAtlas(vivian, native), second = UnitAtlas(nicky, native);
    CHECK(first == uint64_t(uintptr_t(served[0].entry)) && second == uint64_t(uintptr_t(served[1].entry)));
    CHECK(UnitAtlas(vivian, native) == first);           // the same character gets the same instance again
    CHECK(UnitAtlas(zora, native) == native);            // same class and preset, no look: native
    CHECK(UnitAtlas(other_slot, native) == native);      // another DATA slot's character
    CHECK(appearance_instance_of_record(FakeCharacter::Address(vivian.record), 1, 2) == nullptr); // preset changed in game
    FakeCharacter changed = vivian;
    changed.record[APPEARANCE_RECORD_COLOR] = 2;
    changed.unit[0xcc] = 2;
    g_fake_party.push_back(&changed);
    CHECK(UnitAtlas(changed, native) == native);
    g_fake_party.pop_back();

    // Bytes 6 and 7 are not part of the identity the game compares.
    std::array<uint8_t, kCharIdentityBytes> loose = IdentityFor(0, 0x1d);
    loose[6] = 0x55;
    loose[7] = 0x66;
    CHECK(appearance_same_identity(loose.data(), served[0].identity));
    loose[12] = 0x44;
    CHECK(!appearance_same_identity(loose.data(), served[0].identity));

    // The Edit character preview reads the record itself.
    GuestRegs select{};
    select.rax = FakeCharacter::Address(nicky.record);
    select.rdi = 1;
    select.rsi = 3;
    appearance_select_atlas(&select);
    CHECK(select.skip == 1 && select.rax == uint64_t(uintptr_t(served[1].entry)));
    GuestRegs plain{};
    plain.rax = FakeCharacter::Address(zora.record);
    plain.rdi = 1;
    plain.rsi = 3;
    appearance_select_atlas(&plain);
    CHECK(plain.skip == 0 && plain.rax == FakeCharacter::Address(zora.record));

    // Effects copying a look: bound to their owner's instance, released by an explicit look, rebound on reuse.
    std::vector<uint8_t> effect(0x3900, 0);
    const uint64_t effect_address = FakeCharacter::Address(effect);
    const auto copy_look = [&](const FakeCharacter& owner) {
        GuestRegs regs{};
        regs.rdi = effect_address;
        regs.rsi = FakeCharacter::Address(owner.owner);
        appearance_effect_copy(&regs);
        const uint16_t klass = owner.unit[0xcb], color = owner.unit[0xcc];
        std::memcpy(effect.data() + 0x38e8, &klass, 2);
        std::memcpy(effect.data() + 0x38ea, &color, 2);
    };
    const auto effect_atlas = [&]() {
        GuestRegs regs{};
        regs.rdi = effect_address;
        regs.rax = native;
        appearance_effect_atlas(&regs);
        return regs.skip ? regs.rax : native;
    };
    copy_look(vivian);
    CHECK(effect_atlas() == uint64_t(uintptr_t(served[0].entry)));
    GuestRegs explicit_look{};
    explicit_look.rdi = effect_address;
    appearance_effect_set(&explicit_look);
    CHECK(effect_atlas() == native);
    copy_look(nicky);
    CHECK(effect_atlas() == uint64_t(uintptr_t(served[1].entry)));
    copy_look(zora);
    CHECK(effect_atlas() == native);
    copy_look(vivian);
    const uint16_t other_class = 0;
    std::memcpy(effect.data() + 0x38e8, &other_class, 2); // the descriptor was rewritten some other way
    CHECK(effect_atlas() == native);

    appearance_hooks_set_instances(nullptr, 0);
    copy_look(vivian);
    CHECK(effect_atlas() == native);
    appearance_hooks_set_record_of_unit(nullptr);
    g_fake_party.clear();
}

// ---- the hooks installed and run: the atlas getters, the preview call and the effect paths as the game's code ----

#ifdef _WIN32
uint8_t* ExecutableMemory(size_t size) {
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
}
#else
uint8_t* ExecutableMemory(size_t size) {
    void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? nullptr : static_cast<uint8_t*>(memory);
}
#endif

void Put(uint8_t* at, std::initializer_list<uint8_t> bytes) {
    std::copy(bytes.begin(), bytes.end(), at);
}

void PutRel(uint8_t* image, size_t at, size_t target) {
    const int32_t rel = int32_t(int64_t(target) - int64_t(at + 5));
    std::memcpy(image + at + 1, &rel, 4);
}

void TestAppearanceStubs() {
    enum : size_t { kAtlas = 0x100, kUnitOf = 0x180, kItemOf = 0x1c0, kGetterA = 0x200, kGetterB = 0x280, kSelect = 0x300,
                    kCopy = 0x400, kSet = 0x500, kName = 0x580, kWeaponName = 0x600, kShieldName = 0x640,
                    kWeaponGetters = 0x680, kNative = 0x800, kArmsDefault = 0x840, kArmsOther = 0x880, kShieldDefault = 0x8c0,
                    kShieldOther = 0x900, kWeaponPreview = 0x940, kShieldPreview = 0x9a0, kImage = 0x1000 };
    uint8_t* image = ExecutableMemory(0x4000);
    if (!image) {
        std::printf("no executable memory: appearance stub execution skipped\n");
        return;
    }
    std::memset(image, 0xcc, 0x4000);
    // atlas(class, preset): "NATIVE" for classes 0..5
    Put(image + kAtlas, {0x31, 0xc0, 0x89, 0xf9, 0x83, 0xff, 0x05, 0x77, 0x08, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3, 0xc3});
    const int32_t native_rel = int32_t(kNative - (kAtlas + 16));
    std::memcpy(image + kAtlas + 12, &native_rel, 4);
    std::memcpy(image + kNative, "NATIVE", 7);
    Put(image + kUnitOf, {0x48, 0x8b, 0x87, 0xb8, 0x09, 0x00, 0x00, 0xc3});
    for (const size_t getter : {size_t(kGetterA), size_t(kGetterB)}) {
        Put(image + getter, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0xe8, 0, 0, 0, 0, 0x44, 0x0f,
                             0xbe, 0xb0, 0xcb, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0xe8, 0, 0, 0, 0, 0x0f, 0xb6, 0xb0,
                             0xcc, 0x00, 0x00, 0x00, 0x44, 0x89, 0xf7, 0x5b, 0x41, 0x5e, 0x5d, 0xe9, 0, 0, 0, 0});
        PutRel(image, getter + 10, kUnitOf);
        PutRel(image, getter + 26, kUnitOf);
        PutRel(image, getter + 45, kAtlas);
    }
    // preview(holder): push r12; push r14; mov r14,rdi; the game's record read and atlas call; rax = r12
    Put(image + kSelect - 7, {0x41, 0x54, 0x41, 0x56, 0x49, 0x89, 0xfe});
    Put(image + kSelect, {0x49, 0x8b, 0x86, 0xa8, 0x00, 0x00, 0x00, 0x45, 0x31, 0xe4, 0x48, 0x85, 0xc0, 0x74, 0x10, 0x0f,
                          0xbe, 0x78, 0x27, 0x0f, 0xb6, 0x70, 0x28, 0xe8, 0, 0, 0, 0, 0x49, 0x89, 0xc4,
                          0x4c, 0x89, 0xe0, 0x41, 0x5e, 0x41, 0x5c, 0xc3});
    PutRel(image, kSelect + 23, kAtlas);
    // copy(effect, owner), byte for byte the game's
    Put(image + kCopy, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x49, 0x89, 0xf6, 0x48, 0x89, 0xfb, 0x4d, 0x85, 0xf6, 0x74,
                        0x5b, 0x4c, 0x89, 0xf7, 0xe8, 0, 0, 0, 0, 0x48, 0x85, 0xc0, 0x74, 0x4e, 0x41, 0x0f, 0xb7, 0x8e, 0xe0,
                        0x39, 0x00, 0x00, 0x0f, 0xb6, 0x80, 0xcc, 0x00, 0x00, 0x00, 0xc6, 0x83, 0xe0, 0x38, 0x00, 0x00, 0x02,
                        0x66, 0x89, 0x8b, 0xe2, 0x38, 0x00, 0x00, 0x66, 0x89, 0x83, 0xe4, 0x38, 0x00, 0x00, 0xc6, 0x83, 0xe6,
                        0x38, 0x00, 0x00, 0x02, 0x66, 0x89, 0x8b, 0xe8, 0x38, 0x00, 0x00, 0x66, 0x89, 0x83, 0xea, 0x38, 0x00,
                        0x00, 0xc6, 0x83, 0xec, 0x38, 0x00, 0x00, 0x02, 0x66, 0x89, 0x8b, 0xee, 0x38, 0x00, 0x00, 0x66, 0x89,
                        0x83, 0xf0, 0x38, 0x00, 0x00, 0x5b, 0x41, 0x5e, 0x5d, 0xc3});
    PutRel(image, kCopy + 21, kUnitOf);
    Put(image + kSet, {0xc6, 0x87, 0xe6, 0x38, 0x00, 0x00, 0x02, 0x66, 0x89, 0xb7, 0xe8, 0x38, 0x00, 0x00, 0x66, 0x89, 0x97,
                       0xea, 0x38, 0x00, 0x00, 0xc3});
    Put(image + kName, {0x8b, 0xb7, 0xe8, 0x38, 0x00, 0x00, 0x0f, 0xbf, 0xfe, 0xc1, 0xfe, 0x10, 0xe9, 0, 0, 0, 0});
    PutRel(image, kName + 12, kAtlas);
    // Weapon and shield names (class, item): the default texture for item 1, another one for any other item.
    std::memcpy(image + kArmsDefault, "PLAYER/FIGHTER00_ARMS01.FTX", 28);
    std::memcpy(image + kArmsOther, "PLAYER/FIGHTER00_ARMS03.FTX", 28);
    std::memcpy(image + kShieldDefault, "PLAYER/FIGHTER00_SHIELD01.FTX", 30);
    std::memcpy(image + kShieldOther, "PLAYER/FIGHTER00_SHIELD03.FTX", 30);
    const auto put_lea = [&](size_t at, size_t text) {
        Put(image + at, {0x48, 0x8d, 0x05, 0, 0, 0, 0});
        const int32_t rel = int32_t(text - (at + 7));
        std::memcpy(image + at + 3, &rel, 4);
    };
    for (const auto& [function, fallback, other] : {std::tuple{size_t(kWeaponName), size_t(kArmsDefault), size_t(kArmsOther)},
                                                    std::tuple{size_t(kShieldName), size_t(kShieldDefault), size_t(kShieldOther)}}) {
        put_lea(function, fallback);
        Put(image + function + 7, {0x83, 0xfe, 0x01, 0x74, 0x07}); // cmp esi, 1; je ret
        put_lea(function + 12, other);
        Put(image + function + 19, {0xc3});
    }
    Put(image + kItemOf, {0x48, 0x89, 0xf8, 0xc3}); // the unit's equipped item is the unit block + 0x1e8
    const size_t weapon_getters[] = {kWeaponGetters, kWeaponGetters + 0x50, kWeaponGetters + 0xa0, kWeaponGetters + 0xf0};
    for (size_t g = 0; g < 4; ++g) {
        const size_t at = weapon_getters[g];
        Put(image + at, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0xe8, 0, 0, 0, 0, 0x48, 0x85, 0xc0, 0x74, 0x39,
                         0x48, 0x05, 0xe8, 0x01, 0x00, 0x00, 0x48, 0x89, 0xc7, 0xe8, 0, 0, 0, 0, 0x48, 0x85, 0xc0, 0x74, 0x26, 0x44,
                         0x8b, 0x70, 0x40, 0x48, 0x89, 0xdf, 0x41, 0x81, 0xe6, 0xff, 0xff, 0xff, 0x01, 0xe8, 0, 0, 0, 0, 0x0f, 0xbe,
                         0xb8, 0xcb, 0x00, 0x00, 0x00, 0x44, 0x89, 0xf6, 0x5b, 0x41, 0x5e, 0x5d, 0xe9, 0, 0, 0, 0});
        PutRel(image, at + 10, kUnitOf);
        PutRel(image, at + 29, kItemOf);
        PutRel(image, at + 53, kUnitOf);
        PutRel(image, at + 72, g < 2 ? kWeaponName : kShieldName);
    }
    // preview(holder, item): push r14; mov r14,rdi; mov rax,rsi; the game's record read and name call; pop r14; ret
    for (const auto& [at, function] : {std::pair{size_t(kWeaponPreview), size_t(kWeaponName)}, std::pair{size_t(kShieldPreview), size_t(kShieldName)}}) {
        Put(image + at, {0x41, 0x56, 0x49, 0x89, 0xfe, 0x48, 0x89, 0xf0, 0x49, 0x8b, 0x8e, 0xa8, 0x00, 0x00, 0x00, 0xbe, 0xff,
                         0xff, 0xff, 0x01, 0x23, 0x70, 0x40, 0x0f, 0xbe, 0x79, 0x27, 0xe8, 0, 0, 0, 0, 0x41, 0x5e, 0xc3});
        PutRel(image, at + 27, function);
    }

    AppearanceSites sites{};
    CHECK(appearance_hooks_locate(image, kImage, &sites) == 1);
    CHECK(sites.unit_atlas[0] == kGetterA + 45 && sites.unit_atlas[1] == kGetterB + 45 && sites.select_atlas == kSelect + 23);
    CHECK(sites.effect_copy == kCopy && sites.effect_set == kSet && sites.effect_atlas == kName);
    CHECK(sites.unit_weapon[0][0] == weapon_getters[0] + 72 && sites.unit_weapon[0][1] == weapon_getters[1] + 72);
    CHECK(sites.unit_weapon[1][0] == weapon_getters[2] + 72 && sites.unit_weapon[1][1] == weapon_getters[3] + 72);
    CHECK(sites.select_weapon[0] == kWeaponPreview + 27 && sites.select_weapon[1] == kShieldPreview + 27);
    CHECK(sites.weapon_name[0] == kWeaponName && sites.weapon_name[1] == kShieldName);

    using Getter = const char*(GUEST_HOOK_ABI*)(uint64_t owner);
    using Preview = const char*(GUEST_HOOK_ABI*)(uint64_t holder);
    using Copy = void(GUEST_HOOK_ABI*)(uint64_t effect, uint64_t owner);
    using Set = void(GUEST_HOOK_ABI*)(uint64_t effect, int klass, int color);
    using Name = const char*(GUEST_HOOK_ABI*)(uint64_t effect);
    const Getter getter_a = reinterpret_cast<Getter>(image + kGetterA), getter_b = reinterpret_cast<Getter>(image + kGetterB);
    const Preview preview = reinterpret_cast<Preview>(image + kSelect - 7);
    const Copy copy = reinterpret_cast<Copy>(image + kCopy);
    const Set set = reinterpret_cast<Set>(image + kSet);
    const Name name = reinterpret_cast<Name>(image + kName);
    using WeaponPreview = const char*(GUEST_HOOK_ABI*)(uint64_t holder, uint64_t item);
    const auto weapon = [&](size_t getter, uint64_t owner) { return std::string(reinterpret_cast<Getter>(image + getter)(owner)); };
    const WeaponPreview weapon_preview = reinterpret_cast<WeaponPreview>(image + kWeaponPreview);
    const WeaponPreview shield_preview = reinterpret_cast<WeaponPreview>(image + kShieldPreview);

    FakeCharacter vivian(1, 3, IdentityFor(0, 0x1d)), nicky(1, 3, IdentityFor(0, 0x1e)), zora(1, 3, IdentityFor(0, 0x1f));
    const auto equip = [](FakeCharacter& character, uint32_t item) { std::memcpy(character.unit.data() + 0x1e8 + 0x40, &item, 4); };
    for (FakeCharacter* character : {&vivian, &nicky, &zora}) equip(*character, 1);
    g_fake_party = {&vivian, &nicky, &zora};
    const auto owner = [](const FakeCharacter& c) { return FakeCharacter::Address(c.owner); };
    CHECK(std::string(getter_a(owner(vivian))) == "NATIVE"); // the code runs as the game's before any hook

    CHECK(weapon(weapon_getters[0], owner(vivian)) == "PLAYER/FIGHTER00_ARMS01.FTX");

    AppearanceInstance served[] = {
        InstanceFor(IdentityFor(0, 0x1d), 1, 3, "PLAYER/AMAZON03_D1C01_AAAAAAAA.FTX", "DATA 1 slot 1 Amazon colour 3"),
        InstanceFor(IdentityFor(0, 0x1e), 1, 3, "PLAYER/AMAZON03_D1C02_BBBBBBBB.FTX", "DATA 1 slot 2 Amazon colour 3"),
    };
    std::snprintf(served[0].weapons[0].native, sizeof(served[0].weapons[0].native), "PLAYER/FIGHTER00_ARMS01.FTX");
    std::snprintf(served[0].weapons[0].entry, sizeof(served[0].weapons[0].entry), "PLAYER/FIGHTER00_ARMS01_D1C01_AAAAAAAA.FTX");
    std::snprintf(served[0].weapons[1].native, sizeof(served[0].weapons[1].native), "PLAYER/FIGHTER00_SHIELD01.FTX");
    std::snprintf(served[0].weapons[1].entry, sizeof(served[0].weapons[1].entry), "PLAYER/FIGHTER00_SHIELD01_D1C01_AAAAAAAA.FTX");
    std::snprintf(served[1].weapons[1].native, sizeof(served[1].weapons[1].native), "PLAYER/FIGHTER00_SHIELD01.FTX");
    std::snprintf(served[1].weapons[1].entry, sizeof(served[1].weapons[1].entry), "PLAYER/FIGHTER00_SHIELD01_D1C02_BBBBBBBB.FTX");
    appearance_hooks_set_record_of_unit(FakeRecordOfUnit);
    appearance_hooks_set_instances(served, 2);
    CHECK(appearance_hooks_install(image, kImage, 0, &sites, image + 0x2000, appearance_hooks_stub_bytes()) == 1);

    // Weapons: the character's own default texture only; another item, another character or no custom weapon stay native.
    CHECK(weapon(weapon_getters[0], owner(vivian)) == served[0].weapons[0].entry);
    CHECK(weapon(weapon_getters[1], owner(vivian)) == served[0].weapons[0].entry);
    CHECK(weapon(weapon_getters[2], owner(vivian)) == served[0].weapons[1].entry);
    CHECK(weapon(weapon_getters[3], owner(nicky)) == served[1].weapons[1].entry);
    CHECK(weapon(weapon_getters[0], owner(nicky)) == "PLAYER/FIGHTER00_ARMS01.FTX"); // the shield alone is custom
    CHECK(weapon(weapon_getters[0], owner(zora)) == "PLAYER/FIGHTER00_ARMS01.FTX");
    CHECK(weapon(weapon_getters[2], owner(zora)) == "PLAYER/FIGHTER00_SHIELD01.FTX");
    equip(vivian, 3);
    CHECK(weapon(weapon_getters[0], owner(vivian)) == "PLAYER/FIGHTER00_ARMS03.FTX"); // an equipped weapon keeps its look
    CHECK(weapon(weapon_getters[2], owner(vivian)) == "PLAYER/FIGHTER00_SHIELD03.FTX");
    equip(vivian, 1);
    std::vector<uint8_t> weapon_holder(0x100, 0);
    const uint64_t vivian_record = FakeCharacter::Address(vivian.record), zora_record = FakeCharacter::Address(zora.record);
    const uint64_t item = FakeCharacter::Address(vivian.unit) + 0x1e8;
    std::memcpy(weapon_holder.data() + 0xa8, &vivian_record, 8);
    CHECK(std::string(weapon_preview(FakeCharacter::Address(weapon_holder), item)) == served[0].weapons[0].entry);
    CHECK(std::string(shield_preview(FakeCharacter::Address(weapon_holder), item)) == served[0].weapons[1].entry);
    std::memcpy(weapon_holder.data() + 0xa8, &zora_record, 8);
    CHECK(std::string(weapon_preview(FakeCharacter::Address(weapon_holder), item)) == "PLAYER/FIGHTER00_ARMS01.FTX");

    CHECK(std::string(getter_a(owner(vivian))) == served[0].entry);
    CHECK(std::string(getter_b(owner(nicky))) == served[1].entry);
    CHECK(std::string(getter_a(owner(zora))) == "NATIVE");
    CHECK(std::string(getter_b(owner(vivian))) == served[0].entry);
    std::vector<uint8_t> holder(0x100, 0);
    const uint64_t record = FakeCharacter::Address(nicky.record);
    std::memcpy(holder.data() + 0xa8, &record, 8);
    CHECK(std::string(preview(FakeCharacter::Address(holder))) == served[1].entry);
    const uint64_t native_record = FakeCharacter::Address(zora.record);
    std::memcpy(holder.data() + 0xa8, &native_record, 8);
    CHECK(std::string(preview(FakeCharacter::Address(holder))) == "NATIVE");

    std::vector<uint8_t> effect(0x3900, 0);
    const uint64_t effect_address = FakeCharacter::Address(effect);
    copy(effect_address, owner(vivian));
    CHECK(effect[0x38e6] == 2 && effect[0x38e8] == 1 && effect[0x38ea] == 3); // the game's own copy still runs
    CHECK(std::string(name(effect_address)) == served[0].entry);
    set(effect_address, 1, 3);
    CHECK(std::string(name(effect_address)) == "NATIVE");
    copy(effect_address, owner(nicky));
    CHECK(std::string(name(effect_address)) == served[1].entry);
    copy(effect_address, owner(zora));
    CHECK(std::string(name(effect_address)) == "NATIVE");

    appearance_hooks_set_instances(nullptr, 0);
    CHECK(std::string(getter_a(owner(vivian))) == "NATIVE");
    CHECK(weapon(weapon_getters[0], owner(vivian)) == "PLAYER/FIGHTER00_ARMS01.FTX");
    appearance_hooks_set_record_of_unit(nullptr);
    appearance_hooks_set_weapon_names(nullptr, nullptr);
    g_fake_party.clear();
    // A second install over the hooked sites finds no original bytes: nothing is written.
    CHECK(appearance_hooks_locate(image, kImage, &sites) == 0);
}

// ---- the local boot images: the six sites found once, on the atlas function, and installable ----

uint64_t ReadLe64At(std::ifstream& in) {
    uint8_t b[8] = {};
    in.read(reinterpret_cast<char*>(b), 8);
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | b[i];
    return v;
}

// The memory image inside build/image/<folder>/boot.bin (layout written by launcher/boot_image.cpp).
std::vector<uint8_t> BootImage(const fs::path& source_dir, const std::string& folder, uint64_t* entry) {
    fs::path path = source_dir / "build" / "image" / folder / "boot.bin";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) path = source_dir / "build" / "research" / "image" / folder / "boot.bin";
    std::ifstream in(path, std::ios::binary);
    char magic[8] = {};
    if (!in || !in.read(magic, 8) || std::memcmp(magic, "DCPBOOT1", 8) != 0) return {};
    const uint64_t size = ReadLe64At(in);
    *entry = ReadLe64At(in);
    const uint64_t segments = ReadLe64At(in), relocs = ReadLe64At(in), names = ReadLe64At(in);
    ReadLe64At(in), ReadLe64At(in);
    for (int i = 0; i < 4; ++i) ReadLe64At(in);
    const uint64_t modules = ReadLe64At(in);
    in.seekg(std::streamoff(modules * 56), std::ios::cur);
    const uint64_t bindings = ReadLe64At(in);
    in.seekg(std::streamoff(bindings * 24 + segments * 24 + names * 128 + relocs * 32), std::ios::cur);
    std::vector<uint8_t> image(size);
    if (!in.read(reinterpret_cast<char*>(image.data()), std::streamsize(size))) return {};
    return image;
}

void TestAppearanceSites(const fs::path& source_dir) {
    struct Expected {
        const char* folder;
        uint64_t unit[2], select, copy, set, name;
        uint64_t weapon_unit[2][2], weapon_select[2], weapon_name[2];
    };
    static const Expected kBuilds[] = {
        {"v1.03", {0x10899d, 0x2dfc8d}, 0x39f511, 0x30e0b0, 0x30e150, 0x30de3e,
         {{0x1088f8, 0x2dfbe8}, {0x108958, 0x2dfc48}}, {0x39f49d, 0x39f4f2}, {0x251f0, 0x25330}},
        {"CUSA10454-1.01", {0x10899d, 0x2dfc8d}, 0x39f511, 0x30e0b0, 0x30e150, 0x30de3e,
         {{0x1088f8, 0x2dfbe8}, {0x108958, 0x2dfc48}}, {0x39f49d, 0x39f4f2}, {0x251f0, 0x25330}},
        {"CUSA07001-1.00", {0x107f8d, 0x2df19d}, 0x39e991, 0x30d5c0, 0x30d660, 0x30d34e,
         {{0x107ee8, 0x2df0f8}, {0x107f48, 0x2df158}}, {0x39e91d, 0x39e972}, {0x24f30, 0x25070}},
        {"CUSA07001-1.02", {0x10826d, 0x2df56d}, 0x39edd1, 0x30d990, 0x30da30, 0x30d71e,
         {{0x1081c8, 0x2df4c8}, {0x108228, 0x2df528}}, {0x39ed5d, 0x39edb2}, {0x24f50, 0x25090}},
        {"CUSA07001-1.03", {0x1082bd, 0x2df5bd}, 0x39ee21, 0x30d9e0, 0x30da80, 0x30d76e,
         {{0x108218, 0x2df518}, {0x108278, 0x2df578}}, {0x39edad, 0x39ee02}, {0x24f50, 0x25090}},
        {"CUSA10209-1.00", {0x10811d, 0x2df32d}, 0x39eb01, 0x30d750, 0x30d7f0, 0x30d4de,
         {{0x108078, 0x2df288}, {0x1080d8, 0x2df2e8}}, {0x39ea8d, 0x39eae2}, {0x24e10, 0x24f50}},
    };
    int checked = 0;
    for (const Expected& build : kBuilds) {
        uint64_t entry = 0;
        std::vector<uint8_t> image = BootImage(source_dir, build.folder, &entry);
        if (image.empty()) {
            std::printf("boot image %s not present, skipping its appearance sites\n", build.folder);
            continue;
        }
        ++checked;
        AppearanceSites sites{};
        CHECK(appearance_hooks_locate(image.data(), image.size(), &sites) == 1);
        CHECK(sites.unit_atlas[0] == build.unit[0] && sites.unit_atlas[1] == build.unit[1]);
        CHECK(sites.select_atlas == build.select && sites.effect_copy == build.copy && sites.effect_set == build.set &&
              sites.effect_atlas == build.name);
        for (int w = 0; w < APPEARANCE_WEAPON_COUNT; ++w) {
            CHECK(sites.unit_weapon[w][0] == build.weapon_unit[w][0] && sites.unit_weapon[w][1] == build.weapon_unit[w][1]);
            CHECK(sites.select_weapon[w] == build.weapon_select[w] && sites.weapon_name[w] == build.weapon_name[w]);
        }
        const size_t size = image.size(), room = appearance_hooks_stub_bytes();
        image.resize(size + 0x1000 + room);
        CHECK(appearance_hooks_install(image.data(), size, entry, &sites, image.data() + size + 0x1000, room) == 1);
        for (const uint64_t site : {sites.unit_atlas[0], sites.unit_atlas[1], sites.select_atlas, sites.effect_copy,
                                    sites.effect_set, sites.effect_atlas, sites.unit_weapon[0][0], sites.unit_weapon[0][1],
                                    sites.unit_weapon[1][0], sites.unit_weapon[1][1], sites.select_weapon[0], sites.select_weapon[1]}) {
            CHECK(image[site] == 0xe9);
        }
        appearance_hooks_set_weapon_names(nullptr, nullptr);
    }
    std::printf("appearance sites checked on %d local boot images\n", checked);
}

// The installed USA archive: per-character atlases added in CRI's order, every original entry left where it was.
void TestRealCustomOverlay(const fs::path& base, const fs::path& source_dir) {
    const fs::path app0 = source_dir / "game" / "prepared" / "v1.03";
    const fs::path archive_path = app0 / "DRACRO.CPK";
    std::error_code ec;
    if (!fs::is_regular_file(archive_path, ec)) {
        std::printf("installed game not present, skipping the per-character overlay check\n");
        return;
    }
    const fs::path user = base / "overlay_user";
    fs::remove_all(user);
    WriteSaveFor(user, "CUSA10454", "SAVEDATA00", SaveWithCharacters({{1, 3}, {1, 3}, {1, 3}, {0, 3}}));
    CustomStore store(CustomStorePath(user));
    store.Set({"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kAmazon}, HairLook({30, 90, 230}));
    store.Set({"CUSA10454", "SAVEDATA00", 0, 1, CharacterClass::kAmazon}, HairLook({200, 0, 0}));
    CustomLook armor{true, 3, {}};
    armor.Edit("armor").color = Rgb{40, 160, 90};
    store.Set({"CUSA10454", "SAVEDATA00", 0, 3, CharacterClass::kFighter}, armor);
    std::vector<std::string> notes;
    const std::vector<CustomInstance> planned = PlanCustomInstances(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
    CHECK(planned.size() == 3);

    CharacterArtSources sources = ArtSourcesOf(app0, "DRACRO_US.CPK");
    sources.cache_dir = base / "region_cache";
    CharacterArt art(sources);
    CHECK(!BuildCustomOverlay(archive_path, {}, art, notes));
    const std::optional<CustomOverlay> built = BuildCustomOverlay(archive_path, planned, art, notes);
    CHECK(built && built->runtime.size() == 3);
    if (!built || built->runtime.size() != 3) return;
    const uint64_t length = fs::file_size(archive_path);
    CHECK(built->overlay.length > length);
    for (const auto& [offset, data] : built->overlay.patches) CHECK(offset == 16 || offset >= length); // nothing old rewritten

    CpkArchive original, served;
    CHECK(original.Open(archive_path) && OpenOverlaid(archive_path, built->overlay, served));
    CHECK(served.entries().size() == original.entries().size() + 3);
    size_t moved = 0;
    for (const CpkEntry& entry : original.entries()) {
        const CpkEntry* kept = served.Find(entry.path);
        moved += !kept || kept->offset != entry.offset || kept->size != entry.size || kept->id != entry.id;
    }
    CHECK(moved == 0);
    const auto cri_key = [](const CpkEntry& entry) {
        std::string upper = entry.path;
        for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
        return upper;
    };
    bool ordered = true;
    for (size_t i = 1; i < served.entries().size(); ++i) ordered &= cri_key(served.entries()[i - 1]) < cri_key(served.entries()[i]);
    CHECK(ordered);

    const std::vector<uint8_t> amazon = original.Read(*original.Find("PLAYER/AMAZON03.FTX"));
    std::set<uint64_t> contents;
    for (size_t i = 0; i < planned.size(); ++i) {
        const AppearanceInstance& runtime = built->runtime[i];
        CHECK(planned[i].entry == runtime.entry && std::memcmp(runtime.identity, planned[i].identity.data(), kCharIdentityBytes) == 0);
        CHECK(runtime.klass == uint8_t(planned[i].instance.klass) && runtime.color == 3);
        const CpkEntry* entry = served.Find(runtime.entry);
        CHECK(entry && entry->size == entry->extract_size && entry->id >= original.entries().size());
        if (!entry) continue;
        const std::vector<uint8_t> bytes = ReadOverlaid(archive_path, built->overlay, entry->offset, entry->size);
        const std::vector<RegionMap>* maps = art.Regions(planned[i].instance.klass);
        const std::vector<uint8_t> source = original.Read(*original.Find(NativeColorAtlas(planned[i].instance.klass, 3)));
        CHECK(maps && bytes == CustomizeAtlas(source, *maps, planned[i].instance.klass, planned[i].look));
        CHECK(ReadFtxSurface(bytes, 0).has_value());
        contents.insert(Fnv1a(bytes));
    }
    CHECK(contents.size() == 3 && !contents.count(Fnv1a(amazon))); // two Amazons of one preset, two different atlases
    const CpkEntry* native = served.Find("PLAYER/AMAZON03.FTX");
    CHECK(native && native->offset == original.Find("PLAYER/AMAZON03.FTX")->offset); // the third Amazon reads the native one

    std::string error;
    const std::vector<std::pair<std::string, std::vector<uint8_t>>> existing = {{"PLAYER/AMAZON03.FTX", {1, 2, 3}}};
    CHECK(!OverlayAddedEntries(archive_path, length, existing, &error) && !error.empty());
}

void TestCustomOverrideModel(const fs::path& base) {
    CustomLook look{true, 2, {}};
    CHECK(look.OverrideCount() == 0 && !look.active());
    look.SetPartHidden("armor", false); // choosing "Shown" on a part that was never changed is not an override
    CHECK(look.OverrideCount() == 0 && !look.active());
    look.Edit("armor"); // an empty entry is not an override either
    CHECK(look.OverrideCount() == 0 && !look.active());
    look.ResetOverrides();
    look.SetPartColor("armor", {10, 20, 30});
    CHECK(look.OverrideCount() == 1 && look.active());
    look.SetPartHidden("trim", true);
    CHECK(look.OverrideCount() == 2);
    look.SetPartHidden("trim", false);
    CHECK(look.OverrideCount() == 1 && !look.Find("trim")); // hidden then shown leaves nothing behind
    look.SetPartHidden("armor", true);
    look.SetPartHidden("armor", false);
    CHECK(look.Find("armor") && look.Find("armor")->color); // the colour override survives
    look.ResetPart("armor");
    CHECK(look.OverrideCount() == 0 && look.parts.empty() && !look.active());
    look.SetPartColor("armor", {1, 2, 3});
    look.SetPartColor("trim", {4, 5, 6});
    look.ResetOverrides();
    CHECK(look.parts.empty() && look.enabled && look.base == 2);

    const CustomKey key{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kFighter};
    CustomStore store(CustomStorePath(base / "override_model"));
    store.Set(key, CustomLook{true, 2, {}});
    CHECK(store.Get(key) && store.Get(key)->enabled && store.Get(key)->parts.empty());
    CustomStore reread(CustomStorePath(base / "override_model"));
    CHECK(reread.Parse(store.Serialize()) && reread.Get(key) && reread.Get(key)->OverrideCount() == 0);
    for (const std::string& line : DescribeCustomChanges(CustomStore(CustomStorePath(base)), store)) {
        CHECK(line.find("Custom part") == std::string::npos);
    }

    const fs::path user = base / "zero_override_user";
    fs::remove_all(user);
    WriteSaveFor(user, "CUSA10454", "SAVEDATA00", SaveWithCharacters({{0, 2}, {1, 3}}));
    CustomStore zero(CustomStorePath(user));
    zero.Set({"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kFighter}, CustomLook{true, 2, {}});
    CustomLook empty_entry{true, 3, {}};
    empty_entry.Edit("hair");
    zero.Set({"CUSA10454", "SAVEDATA00", 0, 1, CharacterClass::kAmazon}, empty_entry);
    std::vector<std::string> notes;
    CHECK(PlanCustomInstances(zero, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes).empty()); // no atlas is generated
    CustomLook one{true, 3, {}};
    one.SetPartColor("hair", {30, 90, 230});
    zero.Set({"CUSA10454", "SAVEDATA00", 0, 1, CharacterClass::kAmazon}, one);
    CHECK(PlanCustomInstances(zero, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes).size() == 1);

#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const size_t start = page.find("void EditorPage::DrawCustom(");
    const std::string custom = page.substr(start, page.find("\n}\n", start) - start);
    CHECK(custom.find("look.Edit(") == std::string::npos && custom.find("look.parts") == std::string::npos);
    CHECK(page.find("look.SetPartColor(") != std::string::npos && custom.find("look.ResetPart(") != std::string::npos);
    const size_t reset_start = page.find("void EditorPage::DrawResetCustom(");
    const std::string reset = page.substr(reset_start, page.find("\n}\n", reset_start) - reset_start);
    CHECK(reset.find("look.ResetOverrides()") != std::string::npos && custom.find("SetPartHidden(") != std::string::npos);
    size_t at = 0;
    while ((at = custom.find("selected_part_ =", at)) != std::string::npos) {
        CHECK(custom.substr(at, 90).find("SetLook") == std::string::npos); // choosing a part never writes the look
        at += 10;
    }
    CHECK(custom.find("PartAverage") != std::string::npos && custom.find("ColorEdit3") == std::string::npos);
    const size_t appearance_start = page.find("void EditorPage::DrawAppearance(");
    const std::string appearance = page.substr(appearance_start, page.find("\n}\n", appearance_start) - appearance_start);
    CHECK(appearance.find("if (look.active() && figure)") != std::string::npos);
    CHECK(appearance.find("FindCustomPart(character.klass, selected_part_) : nullptr") == std::string::npos); // no tint over the figure
#endif
}

void TestCustomBaseInvariant(const fs::path& base, const fs::path& source_dir) {
    const fs::path app0 = source_dir / "game" / "prepared" / "v1.03";
    std::error_code ec;
    if (!fs::is_regular_file(app0 / "DRACRO.CPK", ec)) {
        std::printf("installed game not present, skipping the Custom base-colour invariant\n");
        return;
    }
    CharacterArtSources sources = ArtSourcesOf(app0, "DRACRO_US.CPK");
    sources.cache_dir = base / "region_cache";
    CharacterArt art(sources);
    CpkArchive cpk;
    CHECK(cpk.Open(app0 / "DRACRO.CPK"));
    size_t exact_previews = 0, exact_atlases = 0;
    for (const CharacterClass klass : kCharacterClasses) {
        const std::vector<RegionMap>* maps = art.Regions(klass);
        CHECK(maps != nullptr);
        if (!maps) continue;
        std::vector<std::vector<uint8_t>> packs;
        for (int color = 0; color < kNativeColorCount; ++color) packs.push_back(cpk.Read(*cpk.Find(AtlasName(klass, color))));
        const std::span<const BodySurface> bodies = BodySurfacesOf(klass);

        for (int color = 0; color < kNativeColorCount; ++color) {
            const SpriteImage* native = art.Preview(klass, color);
            CHECK(native != nullptr);
            if (!native) continue;
            CustomLook zero{true, uint8_t(color), {}};
            const SpriteImage* custom = art.CustomPreview(klass, color, zero);
            CHECK(custom && custom->rgba == native->rgba && Fnv1a(custom->rgba) == Fnv1a(native->rgba));
            exact_previews += custom && custom->rgba == native->rgba;
            exact_atlases += CustomizeAtlas(packs[size_t(color)], *maps, klass, zero) == packs[size_t(color)];
            zero.Edit(CustomPartsOf(klass).front().key); // an entry with no colour and no visibility change
            const SpriteImage* touched_nothing = art.CustomPreview(klass, color, zero);
            CHECK(touched_nothing && touched_nothing->rgba == native->rgba);
            CHECK(CustomizeAtlas(packs[size_t(color)], *maps, klass, zero) == packs[size_t(color)]);

            // The native average is information for the swatch and creates no override.
            for (const CustomPart& part : CustomPartsOf(klass)) {
                const CustomLook watched{true, uint8_t(color), {}};
                art.PartAverage(klass, color, part);
                CHECK(watched.OverrideCount() == 0 && art.CustomPreview(klass, color, watched)->rgba == native->rgba);
            }
        }

        // One explicit override: everything outside its region keeps the exact native texels.
        for (const CustomPart& part : CustomPartsOf(klass)) {
            if (part.weapon >= 0) continue; // a weapon is its own texture, checked with the weapons
            for (const int color : {0, 1}) {
                CustomLook look{true, uint8_t(color), {}};
                look.SetPartColor(part.key, {30, 90, 230});
                CustomLook hidden{true, uint8_t(color), {}};
                hidden.SetPartHidden(part.key, true);
                size_t inside = 0, inside_changed = 0, outside_changed = 0, alpha_changed = 0, hidden_alpha_wrong = 0;
                for (size_t b = 0; b < bodies.size() && b < maps->size(); ++b) {
                    const std::optional<FtxSurface> source = ReadFtxSurface(packs[size_t(color)], bodies[b].surface);
                    CHECK(source.has_value());
                    if (!source) continue;
                    const RegionMap& map = (*maps)[b];
                    const LookShader shader(klass, look, map, [&](int x, int y) { return source->Texel(x, y); });
                    const LookShader hider(klass, hidden, map, [&](int x, int y) { return source->Texel(x, y); });
                    const uint16_t piece_bit = PieceBitOf(klass, part);
                    for (int y = 0; y < map.height; ++y) {
                        for (int x = 0; x < map.width; ++x) {
                            const uint8_t region = map.At(x, y);
                            const std::array<uint8_t, 4> native = source->Texel(x, y);
                            std::array<uint8_t, 4> after = native, gone = native;
                            shader.ApplyAt(x, y, after);
                            hider.ApplyAt(x, y, gone);
                            // A texel a piece part owns follows that part; the rest follow their region.
                            const uint16_t owned = map.Pieces(x, y) ;
                            const bool in_part = owned ? (piece_bit && (owned & piece_bit))
                                                       : (region && (part.regions >> (region - 1) & 1));
                            const bool helmet_sprite = InFighterHelmetSprite(klass, part.key, x, y);
                            if (!in_part) {
                                outside_changed += after != native || (!helmet_sprite && gone != native);
                                continue;
                            }
                            inside += native[3] > 0;
                            inside_changed += native[3] > 0 && after != native;
                            alpha_changed += after[3] != native[3];
                            // Hidden: its own texels cleared, nothing filled in; a part over a head shows the bare head.
                            if (!helmet_sprite) hidden_alpha_wrong += part.bare_underlay.empty() ? gone[3] != 0 : gone[3] != native[3];
                        }
                    }
                }
                CHECK(outside_changed == 0 && alpha_changed == 0 && hidden_alpha_wrong == 0);
                CHECK(inside > 0 && inside_changed * 2 >= inside);

                // The runtime atlas differs from native only because of the override; Reset Part returns the native bytes.
                const std::optional<std::vector<uint8_t>> served = CustomizeAtlas(packs[size_t(color)], *maps, klass, look);
                CHECK(served && *served != packs[size_t(color)]);
                look.ResetPart(part.key);
                CHECK(!look.active() && CustomizeAtlas(packs[size_t(color)], *maps, klass, look) == packs[size_t(color)]);
                CHECK(art.CustomPreview(klass, color, look)->rgba == art.Preview(klass, color)->rgba);
            }
        }

        // Base switch: the override moves onto the new base and only its region differs from that preset.
        const CustomPart& first = CustomPartsOf(klass).front();
        for (int color = 1; color < kNativeColorCount; ++color) {
            CustomLook look{true, uint8_t(color), {}};
            look.SetPartColor(first.key, {30, 90, 230});
            for (size_t b = 0; b < bodies.size() && b < maps->size(); ++b) {
                const std::optional<FtxSurface> source = ReadFtxSurface(packs[size_t(color)], bodies[b].surface);
                if (!source) continue;
                const RegionMap& map = (*maps)[b];
                const LookShader shader(klass, look, map, [&](int x, int y) { return source->Texel(x, y); });
                size_t outside_changed = 0, region_changed = 0;
                for (int y = 0; y < map.height; ++y) {
                    for (int x = 0; x < map.width; ++x) {
                        std::array<uint8_t, 4> texel = source->Texel(x, y);
                        const std::array<uint8_t, 4> native = texel;
                        shader.ApplyAt(x, y, texel);
                        (shader.ChangesAt(x, y) ? region_changed : outside_changed) += texel != native;
                    }
                }
                CHECK(outside_changed == 0);
                CHECK(b > 0 || region_changed > 0);
            }
        }
    }
    CHECK(exact_previews == size_t(6 * kNativeColorCount) && exact_atlases == size_t(6 * kNativeColorCount)); // 30 of 30
    std::printf("custom zero-override equals native: %zu/30 previews, %zu/30 atlases\n", exact_previews, exact_atlases);
}

void TestColorPickerModel() {
    CHECK(kPickerWidth >= 380.0f && kPickerWidth <= 420.0f && kPickerHeight <= 480.0f);
    const PickerPoint size{kPickerWidth, kPickerHeight};
    const PickerPoint view_min{0, 0}, view_max{1280, 800};
    // Room on the right of the swatch: it opens there.
    PickerPoint at = PlacePicker({300, 200}, {372, 234}, size, view_min, view_max, 250.0f, 12.0f);
    CHECK(at.x == 384.0f && at.y == 200.0f);
    // The swatch sits at the right edge of the page: the picker is clamped against the view, clear of the preview.
    at = PlacePicker({1100, 200}, {1172, 234}, size, view_min, view_max, 560.0f, 12.0f);
    CHECK(at.x == view_max.x - size.x && at.x >= 560.0f && at.x + size.x <= view_max.x);
    // Too low: moved up so that it stays inside the view.
    at = PlacePicker({1100, 700}, {1172, 734}, size, view_min, view_max, 560.0f, 12.0f);
    CHECK(at.y + size.y <= view_max.y && at.y >= view_min.y);
    // A narrow view keeps it inside even when the preview cannot be kept clear.
    at = PlacePicker({500, 100}, {560, 134}, size, view_min, {620, 800}, 400.0f, 12.0f);
    CHECK(at.x >= 0.0f && at.x + size.x <= 620.0f + 0.01f);
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const PickerPoint scaled{kPickerWidth * scale, kPickerHeight * scale};
        const PickerPoint place = PlacePicker({1500 * scale, 300}, {1580 * scale, 334}, scaled, {0, 0}, {1920 * scale, 1080 * scale},
                                              700 * scale, 12 * scale);
        CHECK(place.x >= 700 * scale && place.x + scaled.x <= 1920 * scale + 0.01f);
    }

    using O = PickerOutcome;
    CHECK(ResolvePicker(true, false, false, false, false) == O::kOpen);
    CHECK(ResolvePicker(true, true, false, false, false) == O::kCommit);   // OK
    CHECK(ResolvePicker(true, false, false, true, false) == O::kCommit);   // Enter
    CHECK(ResolvePicker(false, false, false, false, false) == O::kCommit); // a click outside
    CHECK(ResolvePicker(true, false, true, false, false) == O::kCancel);   // Cancel
    CHECK(ResolvePicker(false, false, false, false, true) == O::kCancel);  // Escape closes the popup first
    CHECK(ResolvePicker(true, false, false, false, true) == O::kCancel);

    // Cancel puts back exactly what the character had, so the dirty state returns with it.
    const CustomKey key{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kFighter};
    CustomStore saved(CustomStorePath("picker_saved")), working(CustomStorePath("picker_working"));
    CustomLook native_custom{true, 2, {}};
    saved.Set(key, native_custom);
    working = saved;
    CHECK(working == saved);
    std::optional<CustomLook> before = working.Get(key); // snapshot when the picker opens
    CustomLook live = *before;
    live.SetPartColor("armor", {200, 30, 30}); // live preview while choosing
    working.Set(key, live);
    CHECK(!(working == saved));
    live = *before; // Cancel from no override: no override comes back, not the displayed native average
    working.Set(key, live);
    CHECK(working == saved && working.Get(key)->OverrideCount() == 0);

    CustomLook with_override = native_custom;
    with_override.SetPartColor("armor", {10, 20, 30});
    with_override.SetPartHidden("trim", true);
    saved.Set(key, with_override);
    working = saved;
    before = working.Get(key);
    live = *before;
    live.SetPartColor("armor", {250, 250, 0});
    working.Set(key, live);
    CHECK(!(working == saved));
    working.Set(key, *before); // Cancel from an existing override restores it exactly, hidden trim included
    CHECK(working == saved && working.Get(key)->Find("armor")->color == (Rgb{10, 20, 30}) && working.Get(key)->Find("trim")->hidden);
    live.SetPartColor("armor", {250, 250, 0}); // OK keeps the live colour
    working.Set(key, live);
    CHECK(working.Get(key)->Find("armor")->color == (Rgb{250, 250, 0}) && !(working == saved));

#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const size_t start = page.find("void EditorPage::DrawColorPicker(");
    const std::string picker = page.substr(start, page.find("\n}\n", start) - start);
    CHECK(picker.find("PlacePicker(") != std::string::npos && picker.find("kPickerWidth * scale") != std::string::npos);
    CHECK(picker.find("ui.AccentButton(Tr(\"OK\")") != std::string::npos && picker.find("ImGui::Button(Tr(\"Cancel\")") != std::string::npos);
    CHECK(picker.find("ImGuiKey_Enter") != std::string::npos && picker.find("ImGuiKey_Escape") != std::string::npos);
    CHECK(picker.find("ColorPicker4(") != std::string::npos && picker.find("pick_.original") != std::string::npos);
    CHECK(picker.find("ImGuiColorEditFlags_DisplayRGB") != std::string::npos && picker.find("ImGuiColorEditFlags_DisplayHSV") != std::string::npos &&
          picker.find("ImGuiColorEditFlags_DisplayHex") != std::string::npos && picker.find("ImGuiColorEditFlags_PickerHueBar") != std::string::npos);
    CHECK(picker.find("ResolvePicker(") != std::string::npos && picker.find("RestoreLook(pick_.before);") != std::string::npos);
    CHECK(page.find("ColorEdit3(") == std::string::npos);
    CHECK(page.find("pick_.before = custom_working_.Get(") != std::string::npos);
    // Opening the picker or choosing a colour never touches the base colour.
    CHECK(picker.find("SetColor(") == std::string::npos && picker.find("look.base =") == std::string::npos);
#endif
}

void TestEditorNavigationModel() {
    CharacterNav nav;
    CHECK(!nav.editing() && !nav.appearance() && nav.open() == -1);
    CHECK(!nav.RequestAppearance() && !nav.loading()); // nothing to open without a character
    CHECK(!nav.appearance());
    nav.Open(3);
    CHECK(nav.editing() && !nav.appearance() && !nav.loading() && nav.open() == 3);
    CHECK(nav.RequestAppearance());
    CHECK(nav.editing() && nav.loading() && !nav.appearance() && nav.open() == 3); // the character stays fixed
    CHECK(!nav.RequestAppearance() && nav.loading());                           // no second load
    nav.FinishAppearance(true);
    CHECK(nav.editing() && nav.appearance() && !nav.loading() && nav.open() == 3);
    CHECK(!nav.RequestAppearance() && nav.appearance());
    nav.BackToCharacter();
    CHECK(nav.editing() && !nav.appearance() && nav.open() == 3); // the same character
    CHECK(nav.RequestAppearance());
    nav.FinishAppearance(false); // a failed load returns to the same Character Editor
    CHECK(nav.editing() && !nav.appearance() && !nav.loading() && nav.open() == 3);
    nav.FinishAppearance(true); // a late result without a request opens nothing
    CHECK(!nav.appearance());
    CHECK(nav.RequestAppearance());
    nav.Back();
    CHECK(!nav.editing() && !nav.appearance() && !nav.loading());
    nav.Open(1);
    CHECK(!nav.appearance() && !nav.loading()); // a character opens on its editor, not on a stale appearance view

#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const std::string header = ReadSource("launcher/editor_page.h");
    CHECK(header.find("Section") == std::string::npos && page.find("Section::") == std::string::npos);
    CHECK(page.find("{Tr(\"Character\"), Tr(\"Save Data\")}") == std::string::npos);
    const size_t draw_character = page.find("void EditorPage::DrawCharacter(");
    const std::string flow = page.substr(draw_character, page.find("\n}\n", draw_character) - draw_character);
    CHECK(flow.find("if (nav_.loading())") < flow.find("if (nav_.appearance())"));
    CHECK(flow.find("if (nav_.appearance())") < flow.find("if (nav_.editing())") && flow.find("DrawAppearance(ui)") < flow.find("DrawCharacterEditor(ui)"));
    CHECK(flow.find("DrawCharacterList(ui)") != std::string::npos && flow.find("DrawAppearanceLoading(ui)") != std::string::npos);
    const size_t editor_start = page.find("void EditorPage::DrawCharacterEditor(");
    const std::string editor = page.substr(editor_start, page.find("\n}\n", editor_start) - editor_start);
    CHECK(editor.find("TitleWithButton(ui, Tr(\"Character Editor\"), Tr(\"Back to Characters\"))") != std::string::npos);
    CHECK(editor.find("Tr(\"Appearance\")") == std::string::npos && editor.find("RequestAppearance()") != std::string::npos);
    const size_t appearance_start = page.find("void EditorPage::DrawAppearance(");
    const std::string view = page.substr(appearance_start, page.find("\n}\n", appearance_start) - appearance_start);
    CHECK(view.find("TitleWithButton(ui, Tr(\"Appearance\"), Tr(\"Back to Character Editor\"))") != std::string::npos);
    CHECK(view.find("nav_.BackToCharacter();") != std::string::npos && view.find("SetLook(") == std::string::npos);
    CHECK(view.find("nav_.BackToCharacter()") != std::string::npos && view.find("nav_.Back()") == std::string::npos);
    // Navigating writes nothing and discards nothing.
    for (const std::string& body : {editor, flow}) {
        CHECK(body.find("ApplyChanges") == std::string::npos && body.find("DiscardChanges") == std::string::npos && body.find("Save()") == std::string::npos);
    }
    CHECK(view.find("ApplyChanges") == std::string::npos && view.find("DiscardChanges") == std::string::npos && view.find("Save()") == std::string::npos);
    CHECK(Count(page, "DrawChanges(ui);") == 1 && Count(page, "Segmented(\"##editor_section\"") == 0 && Count(page, "ImGui::BeginChild(") == 1);
    // The character list names the chosen DATA slot, in both languages, and only in its title.
    CHECK(CharacterListTitle("Characters", 0) == "Characters (DATA 1)" && CharacterListTitle("Characters", 2) == "Characters (DATA 3)");
    CHECK(CharacterListTitle("Personagens", 1) == "Personagens (DATA 2)");
    CHECK(ReadSource("launcher/localization/pt_br_ui_text.cpp").find("\nCharacters\tPersonagens\n") != std::string::npos);
    const size_t list_start = page.find("void EditorPage::DrawCharacterList(");
    const std::string list = page.substr(list_start, page.find("\n}\n", list_start) - list_start);
    CHECK(list.find("ui.Heading(CharacterListTitle(Tr(\"Characters\"), selected_slot_).c_str());") != std::string::npos);
    const std::string slot_choice = FunctionBody(page, "void EditorPage::DrawSaveData(");
    CHECK(slot_choice.find("DataSlotChoiceLabel(model, selected_slot_, one, many)") != std::string::npos);
    CHECK(slot_choice.find("for (int slot = 0; slot < static_cast<int>(kDataSlotCount); ++slot)") != std::string::npos);
    CHECK(slot_choice.find("Tr(\"Gold\")") == std::string::npos && slot_choice.find("SelectSlot(slot, ui)") != std::string::npos);
    CHECK(slot_choice.find("Tr(\"character\")") != std::string::npos && slot_choice.find("Tr(\"characters\")") != std::string::npos);
    CHECK(list.find("DataSlotName(") == std::string::npos && Count(page, "CharacterListTitle(") == 1);
#endif
}

void TestFeather(const fs::path& base, const fs::path& source_dir) {
    const fs::path app0 = source_dir / "game" / "prepared" / "v1.03";
    std::error_code ec;
    if (!fs::is_regular_file(app0 / "DRACRO.CPK", ec)) {
        std::printf("installed game not present, skipping the feather check\n");
        return;
    }
    CharacterArtSources sources = ArtSourcesOf(app0, "DRACRO_US.CPK");
    sources.cache_dir = base / "region_cache";
    CharacterArt art(sources);
    CpkArchive cpk;
    CHECK(cpk.Open(app0 / "DRACRO.CPK"));
    const CustomPart* feather = FindCustomPart(CharacterClass::kAmazon, "feather");
    CHECK(feather != nullptr && !feather->pieces.empty() && PieceBitOf(CharacterClass::kAmazon, *feather) != 0);
    if (!feather) return;
    const std::vector<RegionMap>* maps = art.Regions(CharacterClass::kAmazon);
    CHECK(maps != nullptr);
    if (!maps) return;
    const std::optional<Mbs> select = ParseMbs(cpk.Read(*cpk.Find("PLAYER/AMAZON00_SELECT.MBS")));
    CHECK(select.has_value());
    for (int color = 0; color < kNativeColorCount; ++color) {
        const SpriteImage native = *art.Preview(CharacterClass::kAmazon, color);
        size_t feather_pixels = 0;
        for (int y = 0; y < native.height; ++y) {
            for (int x = 0; x < native.width; ++x) feather_pixels += art.PartAt(CharacterClass::kAmazon, native, x, y) == feather;
        }
        CHECK(feather_pixels >= 150); // the feather is really there in the rendered Amazon
        CustomLook look{true, uint8_t(color), {}};
        look.SetPartColor("feather", {40, 120, 255});
        const SpriteImage changed = *art.CustomPreview(CharacterClass::kAmazon, color, look);
        size_t differing = 0, on_feather = 0, elsewhere = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            int delta = 0;
            for (int c = 0; c < 4; ++c) delta = std::max(delta, std::abs(int(native.rgba[p * 4 + c]) - int(changed.rgba[p * 4 + c])));
            if (delta <= 1) continue;
            ++differing;
            if (native.layer[p] != kNoLayer && select->layers[native.layer[p]].id == 362) ++on_feather;
            else ++elsewhere;
        }
        CHECK(differing >= 100 && on_feather >= elsewhere * 6); // the feather changes; at most filtered edges around it
        look.ResetPart("feather");
        CHECK(art.CustomPreview(CharacterClass::kAmazon, color, look)->rgba == native.rgba); // exact native pixels again
    }
}

// What the removed Headgear switch used to hide, as the parts that now hold it.
void SetHeadgear(CharacterClass klass, CustomLook& look, bool hidden) {
    for (const char* key : {"helmet", "feather", "hood", "hat", "hat_band"}) {
        const bool takes = (klass == CharacterClass::kDwarf && (std::string(key) == "helmet" || std::string(key) == "feather")) ||
                           (klass == CharacterClass::kAmazon && std::string(key) == "feather") ||
                           ((klass == CharacterClass::kElf || klass == CharacterClass::kWizard) && std::string(key) == "hood") ||
                           (klass == CharacterClass::kSorceress && (std::string(key) == "hat" || std::string(key) == "hat_band"));
        if (takes) look.SetPartHidden(key, hidden);
    }
}
bool HeadgearHidden(CharacterClass klass, const CustomLook& look) {
    const PartLook* part = look.Find(klass == CharacterClass::kDwarf || klass == CharacterClass::kAmazon ? "feather" :
                                     klass == CharacterClass::kSorceress ? "hat" : "hood");
    return part && part->hidden;
}

// There is no separate Headgear switch: a helmet, hat, hood, feather or horns is a part like any other, in Selected Part.
void TestNoHeadgear(const fs::path& base, const fs::path& source_dir) {
    for (const CharacterClass klass : kCharacterClasses) {
        for (const CustomPart& part : CustomPartsOf(klass)) {
            CHECK(part.key != "headgear" && part.label != "Headgear");
            CHECK(PartLabel(klass, part.key) == part.label);
        }
    }
    CHECK(PartLabel(CharacterClass::kFighter, "headgear") == "headgear"); // not a part of any class
    for (const auto& [klass, key, label] : {std::tuple{CharacterClass::kAmazon, "feather", "Feather"}, {CharacterClass::kElf, "hood", "Hood & Cape"},
                                            {CharacterClass::kDwarf, "helmet", "Helmet"}, {CharacterClass::kDwarf, "feather", "Feather"},
                                            {CharacterClass::kSorceress, "hat", "Hat"}, {CharacterClass::kWizard, "hood", "Hood"},
                                            {CharacterClass::kFighter, "helmet", "Helmet"}, {CharacterClass::kFighter, "horns", "Horns"}}) {
        const CustomPart* part = FindCustomPart(klass, key);
        CHECK(part && part->label == label);
    }
    // A look stored by an earlier build with the old switch loses it and keeps everything else.
    const fs::path user = base / "no_headgear_user";
    fs::remove_all(user);
    CustomStore store(CustomStorePath(user));
    const CustomKey key{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kSorceress};
    CustomLook old{true, 2, {}};
    old.Edit("headgear").hidden = true;
    old.SetPartColor("hair", Rgb{10, 20, 30});
    store.Set(key, old);
    CHECK(store.Save());
    CustomStore reloaded(CustomStorePath(user));
    CHECK(reloaded.Load());
    const std::optional<CustomLook> got = reloaded.Get(key);
    CHECK(got && !got->Find("headgear") && got->Find("hair") && got->Find("hair")->color == (Rgb{10, 20, 30}));

    // Two Sorceresses on one preset stay independent; the hat is hidden on the first alone.
    WriteSaveFor(user, "CUSA10454", "SAVEDATA00", SaveWithCharacters({{4, 2}, {4, 2}}));
    CustomStore pair(CustomStorePath(user));
    CustomLook hat_hidden{true, 2, {}};
    hat_hidden.SetPartHidden("hat", true);
    pair.Set({"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kSorceress}, hat_hidden);
    pair.Set({"CUSA10454", "SAVEDATA00", 0, 1, CharacterClass::kSorceress}, CustomLook{true, 2, {}});
    std::vector<std::string> notes;
    const std::vector<CustomInstance> planned = PlanCustomInstances(pair, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
    CHECK(planned.size() == 1 && planned[0].key.character_slot == 0 && planned[0].look.Find("hat") && planned[0].look.Find("hat")->hidden);
    (void)source_dir;
}

void TestEditorBuildChoice() {
    const GameBuild* us100 = FindBuild("CUSA10454 1.00 USA");
    const GameBuild* jp102 = FindBuild("CUSA07001 1.02 Japan");
    CHECK(us100 && jp102);
    if (!us100 || !jp102) return;
    CHECK(std::string(us100->label) == "CUSA10454 1.00 USA" && std::string(jp102->label) == "CUSA07001 1.02 Japan");
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    CHECK(page.find("select_build") == std::string::npos && page.find("DrawBuild") == std::string::npos);
    CHECK(page.find("selected_build_ = DefaultBuildIndex(builds_);") != std::string::npos);
    CHECK(Count(page, "BeginCombo(\"##select_slot\"") == 1);
    for (const char* metadata : {"Tr(\"Title ID\")", "Tr(\"Version\")", "Tr(\"Prepared image\")", "Tr(\"Language archive\")",
                                 "Tr(\"Executable\")", "Tr(\"Game files\")", "Tr(\"SAVE0.DAT size\")"}) {
        CHECK(page.find(metadata) == std::string::npos);
    }
    CHECK(page.find("Tr(\"Save path\")") == std::string::npos && page.find("Tr(\"CRC32\")") == std::string::npos);
    CHECK(page.find("Editor: build ") == std::string::npos);
#endif
}

void TestEditorNumbers() {
    CHECK(kGoldRange.min == 0 && kGoldRange.max == 99999999 && kGoldRange.max == int(kMaxGold));
    CHECK(StepNumber(100, -1, kGoldRange) == 99 && StepNumber(100, 1, kGoldRange) == 101);
    CHECK(StepNumber(0, -1, kGoldRange) == 0);
    CHECK(StepNumber(99999999, 1, kGoldRange) == 99999999 && StepNumber(99999998, 1, kGoldRange) == 99999999);
    CHECK(StepNumber(2356557, 1, kGoldRange) == 2356558 && StepNumber(2356557, -1, kGoldRange) == 2356556);
    CHECK(ClampNumber(-5, kGoldRange) == 0 && ClampNumber(123456789, kGoldRange) == 99999999);
    CHECK(FormatThousands(0) == "0" && FormatThousands(100) == "100" && FormatThousands(1000) == "1,000");
    CHECK(FormatThousands(2356557) == "2,356,557" && FormatThousands(99999999) == "99,999,999");
    CHECK(FormatThousands(999) == "999" && FormatThousands(1234567) == "1,234,567");
    CHECK(ParseNumber("2,356,557", kGoldRange) == 2356557 && ParseNumber("42", kLevelRange) == 42);
    CHECK(ParseNumber("999999999999", kGoldRange) == 99999999 && ParseNumber("0", kLevelRange) == 1);
    CHECK(!ParseNumber("", kGoldRange) && !ParseNumber("12a", kGoldRange) && !ParseNumber(",", kGoldRange));
    CHECK(std::to_string(ParseNumber("1,000", kGoldRange).value()) == "1000"); // separators are never stored

    CHECK(kLevelRange.min == 1 && kLevelRange.max == 255);
    CHECK(StepNumber(42, 1, kLevelRange) == 43 && StepNumber(42, -1, kLevelRange) == 41);
    CHECK(StepNumber(1, -1, kLevelRange) == 1 && StepNumber(255, 1, kLevelRange) == 255);
    CHECK(StepNumber(15, 1, kSkillPointRange) == 16 && StepNumber(15, -1, kSkillPointRange) == 14);
    CHECK(StepNumber(0, -1, kSkillPointRange) == 0 && StepNumber(kMaxSkillPoints, 1, kSkillPointRange) == kMaxSkillPoints);
    CHECK(StepNumber(300, 1, kHpRange) == 301 && StepNumber(300, -1, kHpRange) == 299);
    CHECK(StepNumber(kMaxHp, 1, kHpRange) == kMaxHp && StepNumber(0, -1, kHpRange) == 0);

    HpValues hp{300, 300};
    CHECK(WithCurrentHp(hp, 301).current == 300);             // Current never exceeds Max
    CHECK(WithCurrentHp(hp, 299).current == 299 && WithCurrentHp(hp, -4).current == 0);
    hp = WithMaxHp(hp, 299);                                  // lowering Max lowers Current
    CHECK(hp.max == 299 && hp.current == 299);
    hp = WithMaxHp({100, 300}, 500);                          // raising Max keeps Current
    CHECK(hp.max == 500 && hp.current == 100);
    CHECK(WithMaxHp({10, 10}, 70000).max == kMaxHp && WithMaxHp({10, 10}, -1).max == 0 && WithMaxHp({10, 10}, -1).current == 0);

    // The model setters take exactly the stepped values.
    SaveModel model = ModelWithColors({{1, 2}, {4, 0}});
    CHECK(model.SetGold(0, 99999999) && !model.SetGold(0, 100000000));
    CHECK(model.SetLevel(0, 0, StepNumber(255, 1, kLevelRange)) && model.Character(0, 0)->level_stored == 254);
    const CharacterView* eppie = model.Character(0, 0);
    const HpValues next = WithMaxHp({eppie->hp_current, eppie->hp_max}, eppie->hp_max - 1);
    CHECK(model.SetHp(0, 0, uint16_t(next.current), uint16_t(next.max)) && model.Character(0, 0)->hp_current == model.Character(0, 0)->hp_max);

#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const size_t start = page.find("bool EditorPage::NumberControl(");
    const std::string control = page.substr(start, page.find("\n}\n", start) - start);
    CHECK(control.find("StepNumber(*value, -1, range)") != std::string::npos && control.find("StepNumber(*value, 1, range)") != std::string::npos);
    CHECK(control.find("ParseNumber(") != std::string::npos);
    for (const char* field : {"NumberControl(ui, \"level\", &level, kLevelRange",
                              "NumberControl(ui, \"skill_points\"", "NumberControl(ui, \"hp_current\"", "NumberControl(ui, \"hp_max\""}) {
        CHECK(page.find(field) != std::string::npos);
    }
    CHECK(page.find("ImGui::SliderInt(\"##value\"") != std::string::npos && page.find("ImGui::DragInt(\"##value\"") != std::string::npos);
    CHECK(page.find("WithMaxHp(") != std::string::npos && page.find("WithCurrentHp(") != std::string::npos);
#endif
}

void TestEditorCharacterNavigation(const fs::path& base) {
    CharacterNav nav;
    CHECK(!nav.editing() && nav.open() == -1); // the first state is the list
    nav.Open(2);
    CHECK(nav.editing() && nav.open() == 2);
    nav.Back();
    CHECK(!nav.editing());

    const fs::path user = base / "nav_user";
    const SaveModel model = ModelWithColors({{1, 2}, {4, 0}});
    const std::vector<uint8_t> original(model.bytes().begin(), model.bytes().end());
    const fs::path file = WriteEditableSave(user, original);
    SaveEdit edit = OpenEdit(user);
    nav.Open(1);
    CHECK(edit.working().SetHp(0, 1, 10, 20) && edit.working().SetSkillPoints(0, 1, 7));
    nav.Back();
    nav.Open(0);
    CHECK(edit.dirty() && edit.working().Character(0, 1)->skill_points == 7 && edit.working().Character(0, 1)->hp_current == 10);
    nav.Back();
    CHECK(edit.dirty()); // edits wait in the working copy until Apply
    std::vector<uint8_t> on_disk;
    CHECK(ReadBytes(file, on_disk) && on_disk == original);
    CHECK(ListBackups(BackupFolder(user)).empty());

    const CharacterView& row = *model.Character(0, 0);
    const CharacterRowText text = DescribeCharacterRow(row, "Amazon", "Lv", "Slot");
    CHECK(text.name == row.name && text.detail == "Amazon \xC2\xB7 Lv 15 \xC2\xB7 Slot 1");
    CHECK(DescribeCharacterRow(*model.Character(0, 1), "Sorceress", "Nv", "Slot").detail == "Sorceress \xC2\xB7 Nv 1 \xC2\xB7 Slot 2");

#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const std::string header = ReadSource("launcher/editor_page.h");
    const size_t dispatch = page.find("void EditorPage::DrawCharacter(");
    const std::string draw = page.substr(dispatch, page.find("\n}\n", dispatch) - dispatch);
    CHECK(draw.find("if (nav_.editing())") != std::string::npos && draw.find("DrawCharacterEditor(ui)") < draw.find("DrawCharacterList(ui)"));
    // A row opens the editor; the editor opens with Back to Characters; only the list draws the rows.
    CHECK(page.find("SelectCharacter(character.slot);") != std::string::npos);
    CHECK(page.find("void EditorPage::SelectCharacter(int character) {\n    selected_character_ = character;\n    nav_.Open(character);") != std::string::npos);
    const size_t list = page.find("void EditorPage::DrawCharacterList(");
    const size_t editor = page.find("void EditorPage::DrawCharacterEditor(");
    const std::string list_body = page.substr(list, editor - list);
    const std::string editor_body = page.substr(editor, page.find("\n}\n", editor) - editor);
    CHECK(list_body.find("SelectedSlot()->characters") != std::string::npos && editor_body.find("SelectedSlot()->characters") == std::string::npos);
    CHECK(editor_body.find("Tr(\"Back to Characters\")") != std::string::npos && editor_body.find("nav_.Back()") != std::string::npos);
    CHECK(editor_body.find("Tr(\"Back to Characters\")") < editor_body.find("DrawArt("));
    CHECK(editor_body.find("Tr(\"Gold\")") == std::string::npos && editor_body.find("gold") == std::string::npos);
    CHECK(editor_body.find("ApplyChanges") == std::string::npos && editor_body.find("edit_->Apply") == std::string::npos);
    CHECK(editor_body.find("InputText(\"##name\"") != std::string::npos && editor_body.find("NumberControl(ui, \"name\"") == std::string::npos);
    CHECK(header.find("CharacterNav nav_;") != std::string::npos);
    // The slot is part of the compact text, never a lone value at the far right of the row.
    CHECK(list_body.find("DescribeCharacterRow(") != std::string::npos && list_body.find("slot_text") == std::string::npos);
    CHECK(list_body.find("size.x - pad") == std::string::npos && list_body.find("slot_size") == std::string::npos);
    CHECK(list_body.find("art_->Portrait(") != std::string::npos);
    // Moving between characters never asks to apply or discard.
    CHECK(page.find("PendingSwitch::kCharacter") == std::string::npos && header.find("kSlot, kCharacter") == std::string::npos);
#endif
}

void TestEditorHeaderLayout() {
    const ActionRow clean = ActionRowFor(false), dirty = ActionRowFor(true);
    CHECK(std::string(clean.status).empty() && std::string(dirty.status) == "Unsaved changes");
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.25f}) {
        const float available = 1026.0f * scale;
        const float left_width = 320.0f * scale;
        const float status_width = 116.0f * scale;
        const float frame_height = trg::layout::kControlHeight * scale;
        const float spacing_x = 8.0f * scale;
        const float spacing_y = trg::layout::kItemGap * scale;
        const EditorHeaderGeometry clean_geom =
            LayoutEditorHeader(false, available, left_width, status_width, scale, frame_height, spacing_x, spacing_y);
        const EditorHeaderGeometry dirty_geom =
            LayoutEditorHeader(true, available, left_width, status_width, scale, frame_height, spacing_x, spacing_y);
        CHECK(clean_geom.header_y == dirty_geom.header_y);
        CHECK(clean_geom.header_height == dirty_geom.header_height);
        CHECK(clean_geom.tabs_y == dirty_geom.tabs_y);
        CHECK(clean_geom.body_y == dirty_geom.body_y);
        CHECK(clean_geom.controls.pair_offset == dirty_geom.controls.pair_offset);
        CHECK(clean_geom.controls.pair_width == dirty_geom.controls.pair_width);
        CHECK(clean_geom.controls.apply_x == dirty_geom.controls.apply_x);
        CHECK(clean_geom.controls.discard_x == dirty_geom.controls.discard_x);
        CHECK(clean_geom.controls.status_gap == dirty_geom.controls.status_gap);
        CHECK(clean_geom.controls.status_x == dirty_geom.controls.status_x);
        CHECK(clean_geom.controls.status_width == dirty_geom.controls.status_width);
        // Apply and Discard sit on the Editor title row, centred in the header width, with status reserved on the right.
        const float title_center_y = (trg::layout::kContentTop + trg::layout::kTextPage * 0.5f) * scale;
        CHECK(std::abs((clean_geom.header_y + frame_height * 0.5f) - title_center_y) <= 1.0f);
        CHECK(clean_geom.header_y + frame_height <= clean_geom.tabs_y);
        CHECK(clean_geom.tabs_y == (trg::layout::kContentTop + trg::layout::kTextPage + trg::layout::kItemGap) * scale);
        CHECK(clean_geom.controls.apply_x < clean_geom.controls.discard_x);
        CHECK(clean_geom.controls.apply_x > left_width);
        CHECK(std::abs(clean_geom.controls.pair_offset * 2.0f + clean_geom.controls.pair_width - available) < 0.01f);
        CHECK(clean_geom.controls.status_x >= clean_geom.controls.discard_x + 170.0f * scale);
        CHECK(std::abs(clean_geom.controls.status_x + clean_geom.controls.status_width - available) < 0.01f);
    }
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const size_t start = page.find("void EditorPage::DrawChanges(");
    const std::string row = page.substr(start, page.find("\n}\n", start) - start);
    CHECK(Count(row, "size)") >= 2 && row.find("ImGui::Dummy(ImVec2(layout.status_gap, height))") != std::string::npos);
    CHECK(row.find("ImGui::Dummy(ImVec2(layout.status_width, height))") != std::string::npos);
    CHECK(row.find("LayoutEditorHeader(dirty(), available, left_width, status_width") != std::string::npos);
    CHECK(row.find("header->Pos.y + geom.header_y") != std::string::npos);
    CHECK(row.find("ui.Spacer(") == std::string::npos);
    CHECK(row.find("const ImVec2 size(170.0f * scale, 0.0f);") != std::string::npos);
    const size_t draw = page.find("void EditorPage::Draw(");
    const std::string body = page.substr(draw, page.find("\n}\n", draw) - draw);
    CHECK(body.find("DrawChanges(ui);") < body.find("DrawCharacter(ui);"));
    CHECK(Count(body, "ui.Spacer(") == 0);
    CHECK(Count(page, "ImGui::BeginChild(") == 1);
#endif
}

void TestEditorResponsiveLayout() {
    const float gap = 8.0f;
    CHECK(GridColumns(900.0f, kCardMinWidth, gap, 2) == 2 && GridColumns(760.0f, kCardMinWidth, gap, 2) == 2);
    CHECK(GridColumns(2.0f * kCardMinWidth + gap, kCardMinWidth, gap, 2) == 2);
    CHECK(GridColumns(2.0f * kCardMinWidth + gap - 1.0f, kCardMinWidth, gap, 2) == 1);
    CHECK(GridColumns(400.0f, kCardMinWidth, gap, 2) == 1 && GridColumns(10.0f, kCardMinWidth, gap, 2) == 1);
    CHECK(GridColumns(5000.0f, kCardMinWidth, gap, 2) == 2); // never more columns than asked
    for (const float width : {320.0f, 500.0f, 620.0f, 700.0f, 1000.0f, 1600.0f}) {
        const int columns = GridColumns(width, kCardMinWidth, gap, 2);
        const float cell = GridCellWidth(width, gap, columns, kCardMaxWidth);
        CHECK(cell <= kCardMaxWidth);
        CHECK(columns == 1 || cell >= kCardMinWidth);                          // cards are never cramped
        CHECK(float(columns) * cell + float(columns - 1) * gap <= width + 0.01f); // nothing overflows sideways
    }
    for (const float scale : {1.0f, 1.5f, 2.25f}) { // thresholds follow the width actually available at each scale
        CHECK(GridColumns(1000.0f, kCardMinWidth * scale, gap, 2) == (1000.0f >= 2.0f * kCardMinWidth * scale + gap ? 2 : 1));
    }

    // The slot stays inside the compact card text.
    const SaveModel model = ModelWithColors({{1, 2}});
    const CharacterRowText row = DescribeCharacterRow(*model.Character(0, 0), "Amazon", "Lv", "Slot");
    CHECK(row.detail.find("Slot 1") != std::string::npos && row.detail.find("Amazon") == 0);
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const size_t list = page.find("void EditorPage::DrawCharacterList(");
    const size_t editor = page.find("void EditorPage::DrawCharacterEditor(");
    const std::string list_body = page.substr(list, editor - list);
    const std::string editor_body = page.substr(editor, page.find("\n}\n", editor) - editor);
    CHECK(list_body.find("GridColumns(available, kCardMinWidth * scale") != std::string::npos);
    CHECK(list_body.find("GridCellWidth(available, gap, columns, kCardMaxWidth * scale)") != std::string::npos);
    CHECK(list_body.find("ImGui::SameLine()") != std::string::npos && list_body.find("Indent(") == std::string::npos);
    CHECK(list_body.find("slot_text") == std::string::npos && list_body.find("size.x - pad") == std::string::npos);
    CHECK(editor_body.find("BeginTable(") == std::string::npos);
    CHECK(editor_body.find("ImGuiTableFlags_ScrollX") == std::string::npos && editor_body.find("ImGuiTableFlags_ScrollY") == std::string::npos);
    CHECK(Count(page, "ImGui::BeginChild(") == 1);
#endif
}

void TestEditorNumberControlLook() {
    CHECK(kNumberGlyphRatio >= 0.45f && kNumberGlyphThickness >= 2.0f);
    CHECK(NumberHasThumb(kLevelRange) && NumberHasThumb(kSkillPointRange) && NumberHasThumb(kHpRange) && NumberHasThumb({0, 300}));
    CHECK(!NumberHasThumb(kGoldRange)); // gold is dragged by small relative steps, never by position
    CHECK(kNumberThumbMinWidth >= 12.0f); // Level 1 still shows a real handle
    CHECK(StepNumber(1000, 1, kGoldRange) == 1001 && StepNumber(1000, -1, kGoldRange) == 999); // a huge range keeps single steps
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const size_t start = page.find("bool EditorPage::NumberControl(");
    const std::string control = page.substr(start, page.find("\n}\n", start) - start);
    CHECK(control.find("ImGui::Button(\"##minus\", ImVec2(button, height))") != std::string::npos);
    CHECK(control.find("ImGui::Button(\"##plus\", ImVec2(button, height))") != std::string::npos);
    CHECK(control.find("const float height = ImGui::GetFrameHeight();") != std::string::npos && control.find("kNumberGlyphRatio") != std::string::npos);
    CHECK(control.find("StepNumber(*value, -1, range)") != std::string::npos && control.find("StepNumber(*value, 1, range)") != std::string::npos);
    CHECK(control.find("##minus") < control.find("SliderInt(\"##value\"") && control.find("SliderInt(\"##value\"") < control.find("##plus"));
    CHECK(control.find("DragInt(\"##value\", &shown, drag_speed") != std::string::npos);
    CHECK(control.find("ImGuiStyleVar_GrabMinSize, kNumberThumbMinWidth * scale") != std::string::npos);
    CHECK(control.find("ProgressFraction") == std::string::npos && control.find("AddRectFilled") == std::string::npos);
    CHECK(control.find("InvisibleButton") == std::string::npos && control.find("drag_remainder_") == std::string::npos);
    CHECK(control.find("ImGuiCol_Text, accent") != std::string::npos && control.find("NoInput") != std::string::npos);
    CHECK(control.find("FormatThousands") != std::string::npos && control.find("ParseNumber(") != std::string::npos);
    CHECK(control.find("SetTooltip") == std::string::npos);
    CHECK(page.find("Drag to adjust") == std::string::npos);
    CHECK(ReadSource("launcher/localization/pt_br_ui_text.cpp").find("Drag to adjust") == std::string::npos);
    CHECK(Count(page, "NumberControl(ui, ") == 4); // Level, Skill Points, Current HP, Max HP share it; there is no Gold
#endif
}

void TestEditorDirtyFields() {
    CHECK(FieldStyleFor(5, 5) == FieldStyle::kClean);
    CHECK(FieldStyleFor(5, 6) == FieldStyle::kChanged);
    CHECK(FieldStyleFor(5, 7) == FieldStyle::kChanged && FieldStyleFor(5, 8) == FieldStyle::kChanged); // against the saved value
    CHECK(FieldStyleFor(5, 5) == FieldStyle::kClean);                                                  // reverted
    CHECK(FieldStyleFor(uint32_t(100), uint32_t(101)) == FieldStyle::kChanged && FieldStyleFor(uint32_t(100), uint32_t(100)) == FieldStyle::kClean);
    CHECK(FieldStyleFor(std::string("Eppie"), std::string("Eppi")) == FieldStyle::kChanged);
    CHECK(FieldStyleFor(std::string("Eppie"), std::string("Eppie")) == FieldStyle::kClean);

    // The same through the model: every field follows the persisted value, and reverting clears it.
    const fs::path user = fs::temp_directory_path() / "dcp_save_editor_tests" / "dirty_user";
    const SaveModel model = ModelWithColors({{1, 2}, {4, 0}});
    WriteEditableSave(user, std::vector<uint8_t>(model.bytes().begin(), model.bytes().end()));
    SaveEdit edit = OpenEdit(user);
    const auto character = [&] { return *edit.working().Character(0, 0); };
    const auto saved = [&] { return *edit.persisted().Character(0, 0); };
    const int level = SaveModel::DisplayedLevel(saved().level_stored);
    CHECK(FieldStyleFor(saved().level_stored, character().level_stored) == FieldStyle::kClean);
    CHECK(edit.working().SetLevel(0, 0, level + 1));
    CHECK(FieldStyleFor(saved().level_stored, character().level_stored) == FieldStyle::kChanged);
    CHECK(edit.working().SetLevel(0, 0, level + 3));
    CHECK(FieldStyleFor(saved().level_stored, character().level_stored) == FieldStyle::kChanged);
    CHECK(edit.working().SetLevel(0, 0, level));
    CHECK(FieldStyleFor(saved().level_stored, character().level_stored) == FieldStyle::kClean && !edit.dirty());

    CHECK(edit.working().SetSkillPoints(0, 0, saved().skill_points + 1));
    CHECK(FieldStyleFor(saved().skill_points, character().skill_points) == FieldStyle::kChanged && edit.dirty());
    CHECK(edit.working().SetSkillPoints(0, 0, saved().skill_points));
    CHECK(FieldStyleFor(saved().skill_points, character().skill_points) == FieldStyle::kClean && !edit.dirty());

    const uint16_t hp = saved().hp_current, hp_max = saved().hp_max;
    CHECK(edit.working().SetHp(0, 0, uint16_t(hp - 1), hp_max));
    CHECK(FieldStyleFor(saved().hp_current, character().hp_current) == FieldStyle::kChanged);
    CHECK(FieldStyleFor(saved().hp_max, character().hp_max) == FieldStyle::kClean);
    const HpValues lowered = WithMaxHp({character().hp_current, character().hp_max}, hp_max - 5); // Max below Current drags Current
    CHECK(edit.working().SetHp(0, 0, uint16_t(lowered.current), uint16_t(lowered.max)));
    CHECK(FieldStyleFor(saved().hp_max, character().hp_max) == FieldStyle::kChanged);
    CHECK(FieldStyleFor(saved().hp_current, character().hp_current) == FieldStyle::kChanged && character().hp_current <= character().hp_max);
    CHECK(edit.working().SetHp(0, 0, hp, hp_max));
    CHECK(FieldStyleFor(saved().hp_current, character().hp_current) == FieldStyle::kClean &&
          FieldStyleFor(saved().hp_max, character().hp_max) == FieldStyle::kClean && !edit.dirty());

    const uint32_t gold = edit.persisted().Slot(0)->gold;
    CHECK(edit.working().SetGold(0, gold + 1));
    CHECK(FieldStyleFor(edit.persisted().Slot(0)->gold, edit.working().Slot(0)->gold) == FieldStyle::kChanged);
    CHECK(edit.working().SetGold(0, gold));
    CHECK(FieldStyleFor(edit.persisted().Slot(0)->gold, edit.working().Slot(0)->gold) == FieldStyle::kClean && !edit.dirty());

    CHECK(edit.working().SetName(0, 0, "Eppi"));
    CHECK(FieldStyleFor(saved().name, character().name) == FieldStyle::kChanged);
    CHECK(edit.working().SetName(0, 0, saved().name));
    CHECK(FieldStyleFor(saved().name, character().name) == FieldStyle::kClean && !edit.dirty());

#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    CHECK(page.find("saved->level_stored != character.level_stored") != std::string::npos);
    CHECK(page.find("saved->skill_points != character.skill_points") != std::string::npos);
    CHECK(page.find("saved->hp_current != character.hp_current") != std::string::npos);
    CHECK(page.find("saved->hp_max != character.hp_max") != std::string::npos);
    CHECK(page.find("FieldStyleFor(saved->name, character.name)") != std::string::npos);
    CHECK(page.find("gold") == std::string::npos);
    CHECK(page.find("edit_->persisted()") != std::string::npos);
#endif
}

std::string FunctionBody(const std::string& page, const std::string& signature);

void TestEditorSummary() {
    const SaveModel model = ModelWithColors({{1, 2}});
    const CharacterView saved = *model.Character(0, 0);
    CharacterView working = saved;
    CharacterIdentity identity = DescribeIdentity(working, &saved, "Amazon", "Lv", "Slot");
    CHECK(identity.name == saved.name && identity.parts.size() == 3);
    CHECK(identity.parts[0].text == "Amazon" && identity.parts[1].text == "Lv 15" && identity.parts[2].text == "Slot 1");
    CHECK(!identity.parts[0].changed && !identity.parts[1].changed && !identity.parts[2].changed);

    working.level_stored = uint8_t(saved.level_stored + 1);
    identity = DescribeIdentity(working, &saved, "Amazon", "Lv", "Slot");
    CHECK(identity.parts[1].text == "Lv 16" && identity.parts[1].changed); // working level, marked
    working.level_stored = saved.level_stored;
    identity = DescribeIdentity(working, &saved, "Amazon", "Lv", "Slot");
    CHECK(identity.parts[1].text == "Lv 15" && !identity.parts[1].changed); // back to normal
    // The base colour is never part of the identity, changed or not.
    working.color = uint8_t((saved.color + 1) % kNativeColorCount);
    identity = DescribeIdentity(working, &saved, "Amazon", "Lv", "Slot");
    CHECK(identity.parts.size() == 3 && !identity.parts[0].changed && !identity.parts[1].changed && !identity.parts[2].changed);
    for (const IdentityPart& part : identity.parts) {
        for (const char* colour : {"Blonde", "Rose", "Platinum", "Silky"}) CHECK(part.text.find(colour) == std::string::npos);
    }
    CHECK(!DescribeIdentity(working, nullptr, "Amazon", "Lv", "Slot").parts[1].changed);
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const std::string editor = FunctionBody(page, "void EditorPage::DrawCharacterEditor(");
    CHECK(editor.find("DescribeIdentity(character, saved,") != std::string::npos);
    CHECK(page.find("part.changed ? ImGui::GetColorU32(ui.theme().accent)") != std::string::npos && editor.find("DrawIdentity(") != std::string::npos);
    CHECK(editor.find("Unsaved changes") == std::string::npos && editor.find("ColorName(") == std::string::npos);
    // One line: the name and the details on one baseline, centred in the row the caller gives; no second text row.
    const std::string draw = FunctionBody(page, "void EditorPage::DrawIdentity(");
    CHECK(draw.find("IdentityLineOf(ui, row_height)") != std::string::npos && draw.find("GetTextLineHeightWithSpacing") == std::string::npos);
    CHECK(Count(draw, "draw->AddText(") == 3 && draw.find("at.y + line.name_y") != std::string::npos &&
          draw.find("at.y + line.detail_y") != std::string::npos);
    const std::string size = FunctionBody(page, "ImVec2 EditorPage::IdentitySize(");
    CHECK(size.find("IdentityLineOf(ui, 0.0f).height") != std::string::npos);
    const std::string line = FunctionBody(page, "IdentityLine EditorPage::IdentityLineOf(");
    CHECK(Count(line, "GetFontBaked()->Ascent") == 2 && line.find("LayoutIdentityLine(") != std::string::npos);
    // The same helper on both pages: the editor centres the line on its summary row, Appearance on the line itself.
    CHECK(Count(page, "DrawIdentity(ui, ") == 2 && editor.find("identity, layout.height);") != std::string::npos);
    const std::string view = FunctionBody(page, "void EditorPage::DrawAppearance(");
    CHECK(Count(view, "DrawIdentity(") == 1 && view.find("ui.Spacer(") == std::string::npos && view.find("ColorName(") == std::string::npos);
    CHECK(view.find("ImGui::Dummy(identity_size);") < view.find("DrawCustom(ui, character, look);"));
#endif
}

void TestEditorHeaderRow() {
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const size_t start = page.find("void EditorPage::DrawCharacterEditor(");
    const std::string editor = page.substr(start, page.find("\n}\n", start) - start);
    const size_t back = editor.find("Tr(\"Back to Characters\")");
    const size_t title = editor.find("Tr(\"Character Editor\")");
    const size_t portrait = editor.find("DrawArt(");
    CHECK(back != std::string::npos && title != std::string::npos);
    CHECK(title < back && back < portrait);
    CHECK(editor.find("ui.Heading(") == std::string::npos);
    CHECK(editor.find("LayoutCharacterSummary(") != std::string::npos);
    CHECK(page.find("DrawChanges(ui);") < page.find("DrawCharacter(ui);"));
    const size_t row_start = page.find("void EditorPage::DrawChanges(");
    const std::string row = page.substr(row_start, page.find("\n}\n", row_start) - row_start);
    CHECK(row.find("return;") == std::string::npos && row.find("ui.Spacer(") == std::string::npos);
    CHECK(Count(page, "ImGui::BeginChild(") == 1 && Count(page, "ImGui::BeginTable(") == 1);
#endif
}

void TestEditorCompactLayout() {
    // Game Build: label left, selector on the right 58%; stacked only when narrow.
    const LabelRowLayout wide = LayoutLabelRow(900.0f, 1.0f);
    CHECK(!wide.stacked && std::abs(wide.control_width - 900.0f * kRowControlFraction) < 0.01f);
    CHECK(wide.control_offset > 300.0f && wide.control_offset + wide.control_width <= 900.0f + 0.01f);
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        for (const float width : {300.0f, 480.0f, 700.0f, 1200.0f, 2000.0f}) {
            const LabelRowLayout row = LayoutLabelRow(width, scale);
            CHECK(row.control_offset + row.control_width <= width + 0.01f && row.control_width > 0.0f); // no overflow
            CHECK(row.stacked == (width < kRowStackWidth * scale));
            CHECK(!row.stacked || (row.control_offset == 0.0f && row.control_width == width));
        }
    }
    CHECK(LayoutLabelRow(kRowStackWidth - 1.0f, 1.0f).stacked && !LayoutLabelRow(kRowStackWidth, 1.0f).stacked);

    // Header: the right-hand button is pushed to the far edge.
    CHECK(RightAlignGap(800.0f, 120.0f, 160.0f) == 520.0f && RightAlignGap(200.0f, 120.0f, 160.0f) == 0.0f);

    // Appearance: wide gives the controls more room, narrow stacks; the preview is smaller and keeps its aspect.
    CHECK(AppearanceColumns(900.0f, 1.0f) == 2 && AppearanceColumns(500.0f, 1.0f) == 1);
    CHECK(AppearanceColumns(1000.0f, 1.5f) == 2 && AppearanceColumns(900.0f, 2.25f) == 1);
    CHECK(kControlsColumnWeight > kPreviewColumnWeight && std::abs(kPreviewColumnWeight + kControlsColumnWeight - 1.0f) < 0.001f);
    for (const float width : {700.0f, 900.0f, 1400.0f}) {
        const float old_height = std::clamp(width * 0.5f * 1.3f, 280.0f, 620.0f); // the previous half-width column
        const float height = PreviewHeight(width * kPreviewColumnWeight, 1.0f);
        CHECK(height <= old_height * 0.8f && height >= old_height * 0.65f);
    }
    CHECK(PreviewHeight(5000.0f, 1.0f) == 440.0f && PreviewHeight(1.0f, 2.0f) == 440.0f);
    SpriteImage image;
    image.width = 412;
    image.height = 562;
    const float box_w = 300.0f, box_h = 400.0f;
    const float scale = std::min(box_w / float(image.width), box_h / float(image.height));
    CHECK(std::abs(float(image.width) * scale / (float(image.height) * scale) - float(image.width) / float(image.height)) < 0.0001f);
    for (const float s : {0.4f, 0.55f, 1.0f}) {
        const ImagePoint p = MapToImage(110.0f + 100.0f * s, 40.0f + 200.0f * s, 110.0f, 40.0f, s);
        CHECK(p.x == 100 && p.y == 200); // a pixel of the image is found again at any scale
        const ImagePoint origin = MapToImage(110.0f, 40.0f, 110.0f, 40.0f, s);
        CHECK(origin.x == 0 && origin.y == 0);
    }

    // Numeric controls: compact, a large glyph, the bar, the dirty colour and the exact step are kept.
    CHECK(kNumberGlyphRatio >= 0.45f);

    // A stored look keeps its edits through the enabled flag.
    const fs::path user = fs::temp_directory_path() / "dcp_save_editor_tests" / "custom_toggle";
    fs::remove_all(user);
    CustomStore store(CustomStorePath(user));
    const CustomKey key{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kAmazon};
    CustomLook look{true, 2, {}};
    look.Edit("hair").color = Rgb{10, 20, 30};
    store.Set(key, look);
    CustomLook collapsed = *store.Get(key);
    collapsed.enabled = false;
    store.Set(key, collapsed);
    CHECK(store.Get(key) && !store.Get(key)->enabled && store.Get(key)->parts == look.parts);
    CustomLook back = *store.Get(key);
    back.enabled = true;
    store.Set(key, back);
    CHECK(*store.Get(key) == look);

#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    // Save Data: one compact row, label left and the save choice right, no CRC line.
    const std::string build = FunctionBody(page, "void EditorPage::DrawSaveData(");
    CHECK(build.find("ui.Heading(") == std::string::npos && build.find("LayoutLabelRow(") == std::string::npos);
    CHECK(build.find("Tr(\"Save Data\")") < build.find("BeginCombo(\"##select_slot\""));
    // The combo spans the Apply / Discard pair of the header: same left edge, same width.
    CHECK(build.find("header_pair_x_") != std::string::npos && build.find("SetNextItemWidth(width)") != std::string::npos);
    CHECK(build.find("float width = header_pair_width_;") != std::string::npos);
    const std::string changes = FunctionBody(page, "void EditorPage::DrawChanges(");
    CHECK(changes.find("header_pair_x_ = (body ? body->Pos.x : 0.0f) + layout.pair_offset;") != std::string::npos);
    CHECK(changes.find("header_pair_width_ = layout.pair_width;") != std::string::npos);
    CHECK(page.find("Tr(\"CRC32\")") == std::string::npos && page.find("info.crc") == std::string::npos);
    CHECK(ReadSource("launcher/save_data.cpp").find("SaveCrc32") != std::string::npos);
    CHECK(ReadSource("launcher/save_edit.cpp").find("Serialize") != std::string::npos);
    // The action row buttons use the standard frame height; Save Data never adds a dirty-only row.
    CHECK(page.find("const ImVec2 size(170.0f * scale, 0.0f);") != std::string::npos);
    CHECK(build.find("ui.Help(Tr(\"Apply or discard") == std::string::npos);
    CHECK(page.find("kCompact") == std::string::npos && page.find("ImGuiStyleVar_FramePadding") == std::string::npos);
    // Character Editor header: title on the left, Back to Characters right after it on the same row.
    const size_t editor_start = page.find("void EditorPage::DrawCharacterEditor(");
    const std::string editor = page.substr(editor_start, page.find("\n}\n", editor_start) - editor_start);
    CHECK(editor.find("TitleWithButton(ui, Tr(\"Character Editor\"), Tr(\"Back to Characters\"))") != std::string::npos);
    CHECK(Count(editor, "Tr(\"Character Editor\")") == 1);
    // Back to Characters sits right after the title: no width sums, no right alignment.
    const std::string title_row = FunctionBody(page, "bool EditorPage::TitleWithButton(");
    CHECK(title_row.find("AlignTextToFramePadding()") < title_row.find("TextUnformatted(title)"));
    CHECK(title_row.find("ImGui::SameLine();") < title_row.find("ImGui::Button(button)"));
    for (const char* push : {"GetContentRegionAvail", "RightAlignGap", "SetCursor", "Dummy(", "Spacer(", "SameLine(row"}) {
        CHECK(title_row.find(push) == std::string::npos);
    }
    // Both pages use the same title row; nothing right-aligns a header button any more.
    CHECK(page.find("bool EditorPage::HeaderRow(") == std::string::npos && page.find("LayoutHeaderRow") == std::string::npos);
    CHECK(Count(page, "TitleWithButton(ui, Tr(") == 2);
    // Appearance: responsive columns, resized preview, the custom controls beside it.
    const size_t app_start = page.find("void EditorPage::DrawAppearance(");
    const std::string appearance = page.substr(app_start, page.find("\n}\n", app_start) - app_start);
    CHECK(appearance.find("AppearanceColumns(") != std::string::npos && appearance.find("PreviewHeight(avail.x, scale)") != std::string::npos);
    CHECK(appearance.find("MapToImage(") != std::string::npos && appearance.find("DrawCustom(ui, character, look);") != std::string::npos);
    CHECK(appearance.find("ImGui::BeginChild(") == std::string::npos && Count(page, "ImGui::BeginChild(") == 1);
    const size_t custom_start = page.find("void EditorPage::DrawCustom(");
    const std::string custom_body = page.substr(custom_start, page.find("\n}\n", custom_start) - custom_start);
    CHECK(custom_body.find("ui.Heading(") == std::string::npos && custom_body.find("BeginChild") == std::string::npos);
    for (const char* control : {"DrawBaseColor(", "Tr(\"Selected Part\")", "Tr(\"Part Color\")", "Tr(\"Part Visibility\")",
                                "Tr(\"Reset Part\")", "DrawResetCustom("}) {
        CHECK(custom_body.find(control) != std::string::npos);
    }
#endif
}

void TestEditorFixedLayout() {
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const float button = 170.0f * scale, spacing = 8.0f * scale, frame_h = trg::layout::kControlHeight * scale;
        for (const float width : {700.0f * scale, 1000.0f * scale, 1600.0f * scale}) {
            const ActionRowLayout layout = LayoutActionRow(width, button, spacing, 0.0f, 110.0f * scale);
            CHECK(layout.pair_width == 2.0f * button + spacing);
            CHECK(layout.pair_offset > 0.0f);
            CHECK(layout.apply_x == layout.pair_offset && layout.discard_x == layout.pair_offset + button + spacing);
            CHECK(std::abs(layout.pair_offset * 2.0f + layout.pair_width - width) < 0.01f);
            CHECK(layout.pair_offset + layout.pair_width <= width + 0.01f);
            CHECK(std::abs(layout.status_x + layout.status_width - width) < 0.01f);
            const ActionRowLayout again = LayoutActionRow(width, button, spacing, 0.0f, 110.0f * scale);
            CHECK(again.pair_offset == layout.pair_offset && again.status_x == layout.status_x);
        }
        // Control column width and numeric control fit inside the compact right-side column (~34%..45% on a wide launcher).
        for (const float row_width : {800.0f * scale, 1006.0f * scale}) {
            const float control_w = trg::layout::ControlColumnWidth(row_width, scale);
            CHECK(control_w >= row_width * 0.33f && control_w <= row_width * 0.45f);
            const NumberControlLayout num = LayoutNumberControl(control_w, frame_h, spacing, scale);
            CHECK(num.height == frame_h && num.button_width == frame_h);
            CHECK(std::abs(num.total_width - control_w) < 0.01f);
            CHECK(num.field_width >= 48.0f * scale);
        }
    }
    CHECK(LayoutActionRow(100.0f, 170.0f, 8.0f).pair_offset == 0.0f);
    CHECK(RightAlignGap(900.0f, 0.0f, 120.0f) == 780.0f && RightAlignGap(900.0f, 0.0f, 0.0f) == 900.0f);
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const size_t start = page.find("void EditorPage::DrawChanges(");
    const std::string row = page.substr(start, page.find("\n}\n", start) - start);
    const size_t lead = row.find("ImGui::Dummy(ImVec2(layout.pair_offset");
    const size_t apply = row.find("Tr(\"Apply Changes\")");
    const size_t discard = row.find("Tr(\"Discard Changes\")");
    const size_t status = row.find("Tr(row.status)");
    CHECK(lead != std::string::npos && lead < apply && apply < discard && discard < status);
    CHECK(Count(page, "Tr(\"Apply Changes\")") == 3 && Count(page, "Tr(\"Discard Changes\")") == 2);
    CHECK(Count(row, "Tr(\"Apply Changes\")") == 2 && Count(row, "Tr(\"Discard Changes\")") == 1);
    CHECK(row.find("return;") == std::string::npos && row.find("ImGui::SameLine(0.0f, 0.0f);") != std::string::npos);
    CHECK(row.find("LayoutLabelRow") == std::string::npos);
    const size_t draw = page.find("void EditorPage::Draw(");
    const std::string body = page.substr(draw, page.find("\n}\n", draw) - draw);
    CHECK(body.find("DrawChanges(ui);") < body.find("DrawCharacter(ui);"));
    CHECK(Count(body, "DrawChanges(ui);") == 1 && Count(page, "Segmented(\"##editor_section\"") == 0);
    // Nothing conditional sits between the header and the body, so the pending state cannot move it.
    const size_t after_header = body.find("DrawChanges(ui);") + std::strlen("DrawChanges(ui);");
    const std::string between = body.substr(after_header, body.find("BeginChild(\"##editor_body\"") - after_header);
    CHECK(between.find("if (") == std::string::npos && between.find("Text") == std::string::npos);
    CHECK(page.find("FirstInvalidAppearance") == std::string::npos);

    // Character Editor: one compact trg::Ui row per field, label left, control right, stacked vertically.
    const size_t editor_start = page.find("void EditorPage::DrawCharacterEditor(");
    const std::string editor = page.substr(editor_start, page.find("\n}\n", editor_start) - editor_start);
    CHECK(editor.find("BeginTable(") == std::string::npos && editor.find("ImGui::TableNext") == std::string::npos);
    CHECK(editor.find("GridColumns(") == std::string::npos && editor.find("columns") == std::string::npos);
    CHECK(editor.find("LayoutLabelRow(") == std::string::npos && editor.find("kFieldMinWidth") == std::string::npos);
    CHECK(editor.find("ImGuiTableFlags_Scroll") == std::string::npos && editor.find("BeginChild") == std::string::npos);
    CHECK(editor.find("const auto label =") == std::string::npos);
    const size_t row_name = editor.find("ui.Row(Tr(\"Name\")");
    const size_t name = editor.find("InputText(\"##name\"");
    const size_t row_level = editor.find("ui.Row(Tr(\"Level\")");
    const size_t level = editor.find("NumberControl(ui, \"level\"");
    const size_t row_skill = editor.find("ui.Row(Tr(\"Skill Points\")");
    const size_t skill = editor.find("NumberControl(ui, \"skill_points\"");
    const size_t row_current = editor.find("ui.Row(Tr(\"Current HP\")");
    const size_t current = editor.find("NumberControl(ui, \"hp_current\"");
    const size_t row_max = editor.find("ui.Row(Tr(\"Max HP\")");
    const size_t maximum = editor.find("NumberControl(ui, \"hp_max\"");
    const size_t end_rows = editor.find("ui.EndRows();");
    CHECK(row_name != std::string::npos && row_name < name && name < row_level && row_level < level);
    CHECK(level < row_skill && row_skill < skill && skill < row_current && row_current < current);
    CHECK(current < row_max && row_max < maximum && maximum < end_rows);
    CHECK(Count(editor, "ui.Row(") == 5 && Count(editor, "ui.EndRows();") == 1);
    CHECK(editor.find("ui.Row(Tr(\"Level\"));") != std::string::npos);
    CHECK(editor.find("stored value is level minus one") == std::string::npos);
    CHECK(ReadSource("launcher/localization/pt_br_ui_text.cpp").find("stored value is level minus one") == std::string::npos);
    CHECK(editor.find("TitleWithButton(ui, Tr(\"Character Editor\"), Tr(\"Back to Characters\"))") != std::string::npos);
    CHECK(editor.find("Button(Tr(\"Back to Characters\"), ") == std::string::npos);
    CHECK(page.find("1.35f") == std::string::npos && page.find("* 1.2f") == std::string::npos);
    CHECK(page.find("GridColumns(available, kCardMinWidth * scale") != std::string::npos);

    // Numeric control: standard height for minus, value and plus, yellow bar, step 1, dirty colour.
    CHECK(kNumberGlyphRatio >= 0.45f);
    CHECK(page.find("kNumberButtonHeightScale") == std::string::npos && page.find("kNumberBarHeightFraction") == std::string::npos);
    const size_t control_start = page.find("bool EditorPage::NumberControl(");
    const std::string control = page.substr(control_start, page.find("\n}\n", control_start) - control_start);
    CHECK(control.find("const float height = ImGui::GetFrameHeight();") != std::string::npos && control.find("const float button = height;") != std::string::npos);
    CHECK(control.find("LayoutNumberControl(") != std::string::npos);
    CHECK(control.find("ImGui::SetNextItemWidth(field)") != std::string::npos && control.find("ImVec2(button, height)") != std::string::npos);
    CHECK(control.find("StepNumber(*value, -1, range)") != std::string::npos && control.find("StepNumber(*value, 1, range)") != std::string::npos);
    CHECK(control.find("ParseNumber(") != std::string::npos && control.find("FormatThousands") != std::string::npos);
    CHECK(control.find("SetTooltip") == std::string::npos && control.find("FieldStyle::kChanged") != std::string::npos);
    CHECK(page.find("full_frame_height_") == std::string::npos);

    // Character Editor header: title first, Back to Characters after it on the same row.
    const size_t title = editor.find("Tr(\"Character Editor\")");
    const size_t back = editor.find("Tr(\"Back to Characters\")");
    CHECK(title < back);
    CHECK(Count(page, "ImGui::BeginChild(") == 1);
#endif
}

std::string FunctionBody(const std::string& page, const std::string& signature) {
    const size_t start = page.find(signature);
    if (start == std::string::npos) return {};
    return page.substr(start, page.find("\n}\n", start) - start);
}

std::optional<std::pair<uint64_t, AppearancePrepared>> WaitForJob(AppearanceLoader& loader) {
    for (int i = 0; i < 2000; ++i) {
        if (auto done = loader.Collect()) return done;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

void TestAppearanceLooks() {
    // Opening the page edits a custom look but stores nothing: no override is no entry, so nothing is dirty.
    const CustomLook fresh = EditableLook(std::nullopt, 3);
    CHECK(fresh.enabled && fresh.base == 3 && fresh.parts.empty() && !fresh.active());
    CustomLook dormant{false, 1, {}};
    dormant.SetPartColor("hair", {1, 2, 3});
    CHECK(EditableLook(dormant, 1).OverrideCount() == 0 && !EditableLook(dormant, 1).active()); // the runtime ignores it too
    CustomLook custom{true, 2, {}};
    custom.SetPartHidden("hat", true);
    CHECK(EditableLook(custom, 2) == custom);

    const CustomStore empty(CustomStorePath(fs::temp_directory_path() / "dcp_save_editor_tests" / "looks_user"));
    CustomStore working = empty;
    const CustomKey key{"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kSorceress};
    working.Set(key, StorableLook(fresh));
    CHECK(working == empty && !working.Get(key)); // zero overrides is the native preset
    CustomLook edited = fresh;
    edited.SetPartColor("hair", {9, 9, 9});
    working.Set(key, StorableLook(edited));
    CHECK(working.Get(key) && working.Get(key)->enabled && working.Get(key)->Find("hair"));
    edited.ResetOverrides(); // Reset Custom: part colours and visibility all go
    working.Set(key, StorableLook(edited));
    CHECK(working == empty);
    edited.SetPartHidden("hair", true);
    working.Set(key, StorableLook(edited));
    CHECK(working.Get(key)->Find("hair")->hidden);
    edited.ResetOverrides();
    working.Set(key, StorableLook(edited));
    CHECK(working == empty && !StorableLook(edited).enabled);
    CustomLook based = fresh; // a Base Color change alone stores nothing beyond the save's colour byte
    based.base = 4;
    working.Set(key, StorableLook(based));
    CHECK(working == empty);
    // Cancel of a picker opened with no stored look takes the colour back out.
    working.Set(key, StorableLook(custom));
    working.Set(key, std::optional<CustomLook>().value_or(CustomLook{}));
    CHECK(working == empty);

    CHECK(PartVisibilityLabel("Part Visibility", "Hair") == "Part Visibility (Hair)");
    CHECK(PartVisibilityLabel("Part Visibility", "Hat Band") == "Part Visibility (Hat Band)");
    CHECK(PartVisibilityLabel("Visibilidade da parte", "Pena") == "Visibilidade da parte (Pena)");
    CHECK(PartVisibilityLabel("Part Visibility", "") == "Part Visibility");
}

void TestAppearanceLoader() {
    {
        AppearanceLoader loader;
        CHECK(!loader.busy() && !loader.Collect());
        std::atomic<bool> release{false};
        const uint64_t first = loader.Start([&release] {
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return AppearancePrepared{nullptr, true, {}};
        });
        CHECK(first != 0 && loader.busy());
        CHECK(loader.Start([] { return AppearancePrepared{}; }) == 0); // no duplicate job
        CHECK(!loader.Collect());                                       // not finished yet
        release = true;
        const auto done = WaitForJob(loader);
        CHECK(done && done->first == first && done->second.ok && !loader.busy());
        CHECK(!loader.Collect()); // a result is handed over once

        const uint64_t second = loader.Start([]() -> AppearancePrepared { throw std::runtime_error("broken atlas"); });
        const auto failed = WaitForJob(loader);
        CHECK(second > first && failed && !failed->second.ok && failed->second.error == "broken atlas" && !failed->second.art);
    }
    // Missing game files fail the preparation with a reason instead of hanging.
    AppearanceLoader loader;
    const AppearanceRequest missing{CharacterArtSources{}, CharacterClass::kAmazon, 2, CustomLook{}};
    CHECK(loader.Start([missing] { return PrepareAppearance(missing); }) != 0);
    const auto done = WaitForJob(loader);
    CHECK(done && !done->second.ok && !done->second.error.empty());

    // The loader never outlives its job: destroying it waits for the worker.
    std::atomic<bool> finished{false};
    {
        AppearanceLoader short_lived;
        short_lived.Start([&finished] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            finished = true;
            return AppearancePrepared{};
        });
    }
    CHECK(finished.load());
}

void TestAppearancePrepare(const fs::path& base, const fs::path& source_dir) {
    const fs::path app0 = source_dir / "game" / "prepared" / "v1.03";
    std::error_code ec;
    if (!fs::is_regular_file(app0 / "DRACRO.CPK", ec)) {
        std::printf("installed game not present, skipping the appearance preparation check\n");
        return;
    }
    CharacterArtSources sources = ArtSourcesOf(app0, "DRACRO_US.CPK");
    sources.cache_dir = base / "prepare_cache";
    CharacterArt ui_art(sources);
    CHECK(!ui_art.Ready(CharacterClass::kSorceress, 2));
    CustomLook hidden{true, 2, {}};
    hidden.SetPartHidden("hat", true);
    AppearanceLoader loader;
    const AppearanceRequest request{ui_art.sources(), CharacterClass::kSorceress, 2, hidden};
    CHECK(loader.Start([request] { return PrepareAppearance(request); }) != 0);
    auto done = WaitForJob(loader);
    CHECK(done && done->second.ok && done->second.art);
    if (!done || !done->second.art) return;
    ui_art.Adopt(std::move(*done->second.art), CharacterClass::kSorceress);
    CHECK(ui_art.Ready(CharacterClass::kSorceress, 2)); // the page opens without reading the game files again

    CharacterArt fresh(sources);
    CHECK(ui_art.Preview(CharacterClass::kSorceress, 2)->rgba == fresh.Preview(CharacterClass::kSorceress, 2)->rgba);
    CHECK(ui_art.CustomPreview(CharacterClass::kSorceress, 2, hidden)->rgba ==
          fresh.CustomPreview(CharacterClass::kSorceress, 2, hidden)->rgba);
    const CustomLook shown = EditableLook(std::nullopt, 2); // zero overrides are the native preset
    CHECK(ui_art.CustomPreview(CharacterClass::kSorceress, 2, shown)->rgba == fresh.Preview(CharacterClass::kSorceress, 2)->rgba);
    CHECK(ui_art.Regions(CharacterClass::kSorceress));
    // Another colour of the adopted class still renders on demand.
    CHECK(ui_art.Preview(CharacterClass::kSorceress, 0)->rgba == fresh.Preview(CharacterClass::kSorceress, 0)->rgba);
    // The second visit is the cached fast path.
    CHECK(ui_art.Ready(CharacterClass::kSorceress, 2) && ui_art.Ready(CharacterClass::kSorceress, 0));
}

void TestAppearanceUx() {
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const std::string header = ReadSource("launcher/editor_page.h");
    CHECK(!page.empty() && !header.empty());

    // No tabs: the page is one flow and a finished save load is adopted before anything is drawn.
    const std::string draw = FunctionBody(page, "void EditorPage::Draw(");
    CHECK(draw.find("Segmented(\"##editor_section\"") == std::string::npos && draw.find("Tr(\"Appearance\")") == std::string::npos);
    CHECK(draw.find("CollectAppearanceLoad();") < draw.find("DrawChanges(ui);") && draw.find("DrawCharacter(ui);") != std::string::npos);
    CHECK(draw.find("CollectSaveLoad") == std::string::npos);
    CHECK(draw.find("CollectAppearanceLoad();") < draw.find("DrawChanges(ui);"));

    // Character Editor: one way into Appearance, in the summary row at the right, after the portrait and text.
    const std::string editor = FunctionBody(page, "void EditorPage::DrawCharacterEditor(");
    CHECK(Count(page, "Tr(\"Edit Appearance\")") == 1 && Count(page, "RequestAppearance();") == 1);
    CHECK(editor.find("TitleWithButton(ui, Tr(\"Character Editor\"), Tr(\"Back to Characters\"))") < editor.find("DrawArt("));
    CHECK(editor.find("DrawArt(") < editor.find("Tr(\"Edit Appearance\")"));
    CHECK(editor.find("LayoutCharacterSummary(") < editor.find("Tr(\"Edit Appearance\")"));
    CHECK(editor.find("Tr(\"Appearance\")") == std::string::npos && editor.find("nav_.RequestAppearance") == std::string::npos);
    CHECK(editor.find("Tr(\"Unable to load appearance\")") != std::string::npos);

    // Loading: the click only records the request; the work starts after the loading frame is drawn.
    const std::string request = FunctionBody(page, "void EditorPage::RequestAppearance(");
    CHECK(request.find("nav_.RequestAppearance()") != std::string::npos && request.find("appearance_loader_") == std::string::npos);
    CHECK(request.find("selected_character_") != std::string::npos && request.find("save->save0_path") != std::string::npos);
    const std::string loading = FunctionBody(page, "void EditorPage::DrawAppearanceLoading(");
    CHECK(loading.find("Tr(\"Loading appearance...\")") < loading.find("PathArcTo(") &&
          loading.find("PathArcTo(") < loading.find("AdvanceAppearanceLoad();"));
    CHECK(loading.find("GetTime()") != std::string::npos && loading.find("OpenPopup") == std::string::npos);
    CHECK(Count(page, "AdvanceAppearanceLoad();") == 1);
    const std::string advance = FunctionBody(page, "void EditorPage::AdvanceAppearanceLoad(");
    CHECK(advance.find("art_->Ready(") < advance.find("appearance_loader_.Start("));
    CHECK(advance.find("AppearanceJobCurrent()") != std::string::npos && advance.find("serial != 0") != std::string::npos);
    const std::string collect = FunctionBody(page, "void EditorPage::CollectAppearanceLoad(");
    CHECK(collect.find("art_->Adopt(") < collect.find("nav_.FinishAppearance(true)"));
    CHECK(collect.find("nav_.FinishAppearance(false);") != std::string::npos && collect.find("appearance_failed_ = true;") != std::string::npos);
    CHECK(collect.find("Editor: appearance could not be loaded: ") != std::string::npos);
    for (const std::string& body : {request, loading, advance, collect, editor}) {
        CHECK(body.find("ApplyAll") == std::string::npos && body.find("ApplyChanges") == std::string::npos &&
              body.find("SetLook(") == std::string::npos && body.find(".Save()") == std::string::npos &&
              body.find("->Save()") == std::string::npos);
    }
    CHECK(header.find("AppearanceLoader appearance_loader_;\n};") != std::string::npos); // last member, joined first
    // Nothing sleeps to show the state, and the worker side never touches Dear ImGui.
    const std::string loader_source = ReadSource("launcher/appearance_loader.cpp") + ReadSource("launcher/appearance_loader.h");
    for (const std::string& text : {page, loader_source}) {
        CHECK(text.find("sleep_for") == std::string::npos && text.find("Sleep(") == std::string::npos);
    }
    CHECK(loader_source.find("ImGui") == std::string::npos && loader_source.find("imgui") == std::string::npos);
    CHECK(loader_source.find("detach") == std::string::npos && loader_source.find("worker_.join()") != std::string::npos);
    const std::string art = ReadSource("launcher/character_art.cpp");
    CHECK(art.find("imgui") == std::string::npos && art.find("ImGui") == std::string::npos);

    // Appearance: one custom editor; identity on top of the controls; fixed control order.
    const std::string view = FunctionBody(page, "void EditorPage::DrawAppearance(");
    for (const char* dead : {"Current Color", "Saved Color", "is_custom", "Tr(\"Custom\")", "Choose Custom", "kNativeColorCount",
                             "TextUnformatted(character.name", "TextUnformatted(Tr(ClassName("}) {
        CHECK(view.find(dead) == std::string::npos);
    }
    CHECK(page.find("Tr(\"Color\")") == std::string::npos && page.find("Current Color") == std::string::npos);
    CHECK(view.find("DrawIdentity(ui, ImGui::GetCursorScreenPos(), identity, identity_size.y);") < view.find("DrawCustom(ui, character, look);"));
    const std::string identity = FunctionBody(page, "void EditorPage::DrawIdentity(");
    CHECK(identity.find("ui.fonts().semibold") < identity.find("identity.name") && identity.find("identity.parts") != std::string::npos);
    const std::string custom = FunctionBody(page, "void EditorPage::DrawCustom(");
    const size_t base_color = custom.find("DrawBaseColor(");
    const size_t part = custom.find("Tr(\"Selected Part\")");
    const size_t color = custom.find("Tr(\"Part Color\")");
    const size_t visibility = custom.find("PartVisibilityLabel(Tr(\"Part Visibility\"), Tr(std::string(part->label).c_str()))");
    const size_t reset = custom.find("DrawResetCustom(");
    CHECK(base_color < part && part < color && color < visibility && visibility < reset && reset != std::string::npos);
    CHECK(custom.find("Headgear") == std::string::npos && page.find("Headgear") == std::string::npos);
    CHECK(custom.find("ui.Row(visibility_label.c_str()") != std::string::npos);
    const std::string base = FunctionBody(page, "void EditorPage::DrawBaseColor(");
    CHECK(base.find("Tr(\"Base Color\")") != std::string::npos && base.find("preset < kNativeColorCount") != std::string::npos && kNativeColorCount == 5);
    CHECK(base.find("ColorName(character.klass, preset)") != std::string::npos && base.find("look.base = uint8_t(preset);") != std::string::npos);
    const std::string reset_body = FunctionBody(page, "void EditorPage::DrawResetCustom(");
    CHECK(reset_body.find("BeginDisabled(look.OverrideCount() == 0)") != std::string::npos && reset_body.find("ResetOverrides()") != std::string::npos);
    const std::string current = FunctionBody(page, "CustomLook EditorPage::CurrentLook(");
    CHECK(current.find("EditableLook(") != std::string::npos);
    CHECK(FunctionBody(page, "void EditorPage::SetLook(").find("StorableLook(look)") != std::string::npos);

    // Picker: a thin border in the launcher's panel colour, same size and placement, same OK / Cancel rules.
    const std::string picker = FunctionBody(page, "void EditorPage::DrawColorPicker(");
    CHECK(picker.find("PushStyleVar(ImGuiStyleVar_PopupBorderSize, std::max(1.0f, scale))") < picker.find("BeginPopup(\"##color_picker\")"));
    CHECK(picker.find("PushStyleColor(ImGuiCol_Border, ui.theme().border_panel)") < picker.find("BeginPopup(\"##color_picker\")"));
    CHECK(picker.find("theme().accent)") == std::string::npos && picker.find("warn_dot") == std::string::npos);
    CHECK(picker.find("kPickerWidth * scale") != std::string::npos && picker.find("preview_right_") != std::string::npos);
    CHECK(kPickerWidth == 400.0f && kPickerHeight == 400.0f);
    // OK and Cancel sit in the column under Current / Original, so the popup has no button row of its own.
    CHECK(picker.find("ImVec2 button(") == std::string::npos && picker.find("ImGui::SameLine(picker_max.x") != std::string::npos);
    CHECK(picker.find("ColorPicker4(") < picker.find("ImGui::SameLine(picker_max.x") && picker.find("ImGui::SameLine(picker_max.x") < picker.find("Tr(\"OK\")"));
    CHECK(picker.find("Tr(\"OK\")") < picker.find("Tr(\"Cancel\")") && Count(picker, "SameLine(") == 1);
    CHECK(picker.find("const float column = 3.0f * ImGui::GetFrameHeight();") != std::string::npos); // the width of the swatches
    CHECK(picker.find("ui.AccentButton(Tr(\"OK\"), ImVec2(column, 0.0f))") != std::string::npos);
    CHECK(picker.find("ImGui::Button(Tr(\"Cancel\"), ImVec2(column, 0.0f))") != std::string::npos);
    CHECK(picker.find("ImGui::EndGroup();") < picker.find("ResolvePicker("));
    CHECK(picker.find("RestoreLook(pick_.before)") != std::string::npos && picker.find("ImGuiKey_Escape") != std::string::npos);

    // Reset Custom: the confirmation is the same row, with its buttons in the control column.
    const std::string normal = reset_body.substr(0, reset_body.find("ui.Row(Tr(\"Reset Custom\"), Tr(\"Remove every"));
    const std::string confirm = reset_body.substr(normal.size());
    CHECK(Count(normal, "ButtonRow(Tr(\"Reset Custom\"), Tr(\"Removes every custom change") == 1);
    CHECK(confirm.find("ui.Row(Tr(\"Reset Custom\"), Tr(\"Remove every custom change of this character?\"))") == 0);
    CHECK(confirm.find("TextUnformatted") == std::string::npos && confirm.find("AlignTextToFramePadding") == std::string::npos);
    CHECK(confirm.find("ui.Row(") < confirm.find("Tr(\"Remove changes\")") && confirm.find("Tr(\"Remove changes\")") < confirm.find("Tr(\"Keep changes\")"));
    CHECK(confirm.find("ui.EndRows();") > confirm.find("Tr(\"Keep changes\")"));
    CHECK(confirm.find("ImGuiCol_Button, ui.theme().warn") != std::string::npos && confirm.find("PopStyleColor(3)") < confirm.find("Tr(\"Keep changes\")"));
    CHECK(confirm.find("ImVec2(half, 0.0f)") != std::string::npos && confirm.find("ImVec2(-FLT_MIN, 0.0f)") != std::string::npos);
    CHECK(confirm.find("OverrideCount") == std::string::npos && Count(reset_body, "ui.EndRows();") == 2);
    CHECK(Count(reset_body, "ui.Row(") + Count(reset_body, "ButtonRow(") == 2); // one row in either state
    CHECK(reset_body.find("ImGui::OpenPopup") == std::string::npos && reset_body.find("BeginPopupModal") == std::string::npos);
    CHECK(ResolvePicker(false, false, false, false, false) == PickerOutcome::kCommit); // a click outside keeps the colour
    CHECK(ResolvePicker(true, false, false, true, false) == PickerOutcome::kCommit && ResolvePicker(true, false, false, false, true) == PickerOutcome::kCancel);
#endif
}

struct PartRender {
    CharacterArt& art;
    CharacterClass klass;
    const Mbs& select;
    uint32_t IdAt(const SpriteImage& image, size_t p) const {
        return image.layer[p] == kNoLayer ? 0xFFFFFFFFu : select.layers[image.layer[p]].id;
    }
    bool Visible(const SpriteImage& image, size_t p) const { return image.layer[p] != kNoLayer && image.rgba[p * 4 + 3] >= 64; }
    const CustomPart* PartOf(const SpriteImage& image, size_t p) const {
        return art.PartAt(klass, image, int(p % size_t(image.width)), int(p / size_t(image.width)));
    }
    static int Delta(const SpriteImage& a, const SpriteImage& b, size_t p) {
        int delta = 0;
        for (int c = 0; c < 4; ++c) delta = std::max(delta, std::abs(int(a.rgba[p * 4 + c]) - int(b.rgba[p * 4 + c])));
        return delta;
    }
};

// What a part change does to the preview: how many pixels move, and how many of them are away from the part.
struct PartChange {
    size_t changed = 0, outside = 0, part_pixels = 0, part_changed = 0, alpha_changed = 0;
};
// `outside` counts changed pixels more than two pixels from the part: its filtered rim is the part's own edge.
PartChange Compare(const PartRender& r, const SpriteImage& native, const SpriteImage& after, const CustomPart* part) {
    PartChange result;
    std::vector<uint8_t> mine(native.layer.size(), 0);
    for (size_t p = 0; p < native.layer.size(); ++p) mine[p] = r.PartOf(native, p) == part;
    const auto near_part = [&](size_t p) {
        const int x = int(p % size_t(native.width)), y = int(p / size_t(native.width));
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx >= 0 && ny >= 0 && nx < native.width && ny < native.height && mine[size_t(ny) * size_t(native.width) + size_t(nx)]) return true;
            }
        }
        return false;
    };
    for (size_t p = 0; p < native.layer.size(); ++p) {
        const int delta = PartRender::Delta(native, after, p);
        result.part_pixels += mine[p];
        result.part_changed += mine[p] && delta > 1;
        result.changed += delta > 1;
        result.outside += delta > 1 && !mine[p] && !near_part(p);
        result.alpha_changed += std::abs(int(native.rgba[p * 4 + 3]) - int(after.rgba[p * 4 + 3])) > 8;
    }
    return result;
}

std::optional<Mbs> SelectModel(CpkArchive& cpk, CharacterClass klass) {
    std::string upper(ClassName(klass));
    for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    return ParseMbs(cpk.Read(*cpk.Find("PLAYER/" + upper + "00_SELECT.MBS")));
}

// Pixels of the native preview, by the id of the piece that draws them, that a changed preview moved or removed.
struct PieceDamage {
    size_t pixels = 0, moved = 0, removed = 0, created = 0;
};
template <typename Select>
PieceDamage Damage(const PartRender& r, const SpriteImage& native, const SpriteImage& after, Select selected) {
    PieceDamage damage;
    for (size_t p = 0; p < native.layer.size(); ++p) {
        const int a = native.rgba[p * 4 + 3], b = after.rgba[p * 4 + 3];
        damage.created += b > a + 8;
        if (!r.Visible(native, p) || !selected(r.IdAt(native, p), p)) continue;
        ++damage.pixels;
        damage.moved += PartRender::Delta(native, after, p) > 24;
        damage.removed += b + 8 < a;
    }
    return damage;
}

CustomLook Painted(int color, std::initializer_list<const char*> keys, Rgb value = {0, 230, 255}) {
    CustomLook look{true, uint8_t(color), {}};
    for (const char* key : keys) look.SetPartColor(key, value);
    return look;
}

CustomLook Hidden(int color, std::initializer_list<const char*> keys) {
    CustomLook look{true, uint8_t(color), {}};
    for (const char* key : keys) look.SetPartHidden(key, true);
    return look;
}

void TestPartMappingTables() {
    const auto keys_of = [](CharacterClass klass) {
        std::vector<std::string> keys;
        for (const CustomPart& part : CustomPartsOf(klass)) keys.emplace_back(part.key);
        return keys;
    };
    CHECK((keys_of(CharacterClass::kAmazon) == std::vector<std::string>{"hair", "skin", "tattoo", "eye", "feather", "armor", "bikini", "axe"}));
    CHECK((keys_of(CharacterClass::kSorceress) == std::vector<std::string>{"hair", "skin", "eye", "dress", "bodice", "lace", "frills", "hat",
                                                                           "hat_band", "boots", "accessories", "staff"}));
    CHECK((keys_of(CharacterClass::kFighter) ==
           std::vector<std::string>{"armor", "trim", "leather", "eye", "crest", "crest_accent", "horns", "helmet", "face", "sword",
                                    "shield", "shield_accent"}));
    const CustomPart* tattoo = FindCustomPart(CharacterClass::kAmazon, "tattoo");
    const CustomPart* eye = FindCustomPart(CharacterClass::kAmazon, "eye");
    const CustomPart* feather = FindCustomPart(CharacterClass::kAmazon, "feather");
    const CustomPart* skin = FindCustomPart(CharacterClass::kAmazon, "skin");
    const CustomPart* hair = FindCustomPart(CharacterClass::kAmazon, "hair");
    CHECK(tattoo && eye && feather && skin && hair);
    if (!tattoo || !eye || !feather || !skin || !hair) return;
    CHECK(tattoo->label == "Tattoo" && eye->label == "Eye" && feather->label == "Feather");
    CHECK(tattoo->mark == PaintedMark::kTattooInk && eye->mark == PaintedMark::kPainted && eye->piece_regions == uint16_t(1u << 1));
    // Eye and Feather share no region and no piece.
    CHECK(feather->regions == 0 && feather->OwnsPiece(362) && !eye->OwnsPiece(362) && (eye->regions & feather->regions) == 0);
    CHECK(eye->OwnsPiece(41) && eye->OwnsPiece(42) && !feather->OwnsPiece(41) && !hair->OwnsPiece(41));
    // Hair owns hair pieces only: no boot, armour, gauntlet or leg piece, and no region of its own.
    CHECK(hair->regions == 0 && hair->OwnsPiece(211) && hair->OwnsPiece(301) && hair->OwnsPiece(10));
    for (const uint32_t id : {502u, 503u, 504u, 505u, 506u, 521u, 522u, 525u, 526u, 527u, 530u, 531u, 500u, 41u, 362u}) CHECK(!hair->OwnsPiece(id));
    CHECK(skin->OwnsPiece(41) && skin->OwnsPiece(530) && !skin->OwnsPiece(211) && !skin->OwnsPiece(521));
    CHECK(PieceBitOf(CharacterClass::kAmazon, *tattoo) != PieceBitOf(CharacterClass::kAmazon, *skin));
    const CustomPart* armor = FindCustomPart(CharacterClass::kAmazon, "armor");
    CHECK(armor && armor->OwnsPiece(521) && armor->OwnsPiece(503) && !armor->OwnsPiece(211));

    const CustomPart* dress = FindCustomPart(CharacterClass::kSorceress, "dress");
    const CustomPart* bodice = FindCustomPart(CharacterClass::kSorceress, "bodice");
    const CustomPart* boots = FindCustomPart(CharacterClass::kSorceress, "boots");
    CHECK(dress && bodice && boots && dress->label == "Dress" && bodice->label == "Bodice");
    if (!dress || !bodice || !boots) return;
    for (const uint32_t id : {506u, 516u, 526u, 536u, 541u, 548u, 549u, 560u, 659u, 695u}) CHECK(dress->OwnsPiece(id));
    for (const uint32_t id : {1000u, 1012u, 1421u, 80u, 8u, 700u}) CHECK(!dress->OwnsPiece(id));
    CHECK(bodice->OwnsPiece(1012) && bodice->OwnsPiece(1200) && bodice->piece_regions == uint16_t(1u));
    CHECK(FindCustomPart(CharacterClass::kSorceress, "dress_lining") == nullptr && FindCustomPart(CharacterClass::kSorceress, "skirt_inside") == nullptr);
    const CustomPart* hair_s = FindCustomPart(CharacterClass::kSorceress, "hair");
    CHECK(hair_s && hair_s->OwnsPiece(134) && hair_s->OwnsPiece(18) && hair_s->OwnsPiece(212) && !hair_s->OwnsPiece(1) && !hair_s->OwnsPiece(80));
    const std::span<const uint32_t> hat = FindCustomPart(CharacterClass::kSorceress, "hat")->pieces;
    for (const uint32_t id : {80u, 81u, 89u, 91u, 92u, 99u, 66u, 70u}) CHECK(std::find(hat.begin(), hat.end(), id) != hat.end());
    for (const uint32_t id : {1u, 8u, 78u, 79u, 100u, 500u}) CHECK(std::find(hat.begin(), hat.end(), id) == hat.end());

    // A piece shared by parts is split by texel: at most one part takes all of it, the others take regions or a mark,
    // and two region-limited parts never take the same region of it.
    for (const CharacterClass klass : kCharacterClasses) {
        uint16_t used = 0;
        std::set<std::string_view> keys;
        for (const CustomPart& part : CustomPartsOf(klass)) {
            CHECK(keys.insert(part.key).second && part.key != "headgear" && !part.label.empty());
            for (const char* vague : {"Part ", "Extra", "Unknown", "Residual", "Region"}) CHECK(part.label.find(vague) == std::string_view::npos);
            const uint16_t bit = PieceBitOf(klass, part);
            CHECK(bit == 0 || (used & bit) == 0);
            CHECK(!part.HasPieces() || bit != 0); // every piece part has a bit of the plane
            used |= bit;
        }
        const std::span<const CustomPart> parts = CustomPartsOf(klass);
        for (uint32_t id = 0; id < 1600; ++id) {
            std::vector<const CustomPart*> owners;
            for (const CustomPart& part : parts) {
                if (part.OwnsPiece(id)) owners.push_back(&part);
            }
            size_t whole = 0;
            for (size_t i = 0; i < owners.size(); ++i) {
                whole += !owners[i]->piece_regions && owners[i]->mark == PaintedMark::kNone;
                for (size_t j = i + 1; j < owners.size(); ++j) {
                    if (owners[i]->mark != PaintedMark::kNone || owners[j]->mark != PaintedMark::kNone) continue;
                    CHECK((owners[i]->piece_regions & owners[j]->piece_regions) == 0);
                }
            }
            CHECK(whole <= 1);
        }
    }
    for (const auto& [klass, owner] : {std::pair{CharacterClass::kAmazon, "feather"}, {CharacterClass::kSorceress, "hat"}}) {
        for (const uint32_t id : FindCustomPart(klass, owner)->pieces) {
            for (const CustomPart& part : CustomPartsOf(klass)) CHECK(!part.OwnsPiece(id) || part.key == owner);
        }
    }
    CHECK(TattooInk({160, 90, 50, 255}, {120, 50, 90, 255}, kTattooInkCore));
    CHECK(!TattooInk({200, 120, 70, 255}, {205, 125, 78, 255}, kTattooInkEdge));
    CHECK(!TattooInk({160, 90, 50, 0}, {120, 50, 90, 255}, kTattooInkCore));
    CHECK(kTattooInkEdge < kTattooInkCore);

    // Obsolete fragment parts are gone from the page and from both languages.
    const std::string portuguese = ReadSource("launcher/localization/pt_br_ui_text.cpp");
    for (const std::string& text : {ReadSource("launcher/appearance_custom.cpp"), ReadSource("launcher/editor_page.cpp"), portuguese}) {
        for (const char* dead : {"Dress Lining", "Skirt Inside", "dress_lining", "skirt_inside", "Hat & Corset"}) CHECK(text.find(dead) == std::string::npos);
    }
    for (const char* text : {"\nBodice\t", "\nEye\t", "\nTattoo\t", "\nDress\t", "\nHat\t", "\nEyes\t", "\nBust Lace\t",
                             "\nSleeve Frills\t", "\nEngraved Trim\t", "\nCrest\t", "\nSword\t", "\nShield\t", "\nAxe\t",
                             "\nBow\t", "\nHammers\t", "\nStaff\t", "\nRandom\t", "\nAll Colors Random\t", "\nBeard\t",
                             "\nCape\t", "\nHelmet\t", "\nWrist Guards\t", "\nBelt\t", "\nTrousers\t", "\nPendant\t", "\nCollar\t", "\nShorts\t",
                             "\nQuiver\t", "\nArrows\t", "\nLaces & Trim\t"}) {
        CHECK(portuguese.find(text) != std::string::npos);
    }
    for (const CharacterClass klass : kCharacterClasses) {
        for (const CustomPart& part : CustomPartsOf(klass)) CHECK(portuguese.find("\n" + std::string(part.label) + "\t") != std::string::npos);
    }
}

void TestPartCacheVersion(const fs::path& base, const fs::path& source_dir) {
    CHECK(kPieceCacheVersion == "pieces-22" && kRegionCacheVersion == "regions-2");
    const std::string part_source = ReadSource("launcher/appearance_custom.cpp") + ReadSource("launcher/appearance_custom.h");
    for (const char* old : {"pieces-3", "pieces-4", "pieces-5", "pieces-6", "pieces-7", "pieces-8", "pieces-9\"", "pieces-10", "pieces-11", "pieces-12", "pieces-13", "pieces-14", "pieces-15", "pieces-16", "pieces-17", "pieces-18", "pieces-19", "pieces-20", "regions-1"}) {
        CHECK(part_source.find(old) == std::string::npos);
    }
    // A 16-bit plane round-trips, and a plane of another key or size is refused.
    const std::vector<uint16_t> plane = {0, 0, 0x0101, 0x0200, 0x0200, 0x8001, 0, 0};
    const std::vector<uint8_t> stored = SerializePiecePlane(4, 2, plane, "id|pieces-5");
    CHECK(ParsePiecePlane(stored, "id|pieces-5", 4, 2) == std::optional<std::vector<uint16_t>>(plane));
    CHECK(!ParsePiecePlane(stored, "id|pieces-9", 4, 2) && !ParsePiecePlane(stored, "id|pieces-5", 2, 4));
    const fs::path app0 = source_dir / "game" / "prepared" / "v1.03";
    std::error_code ec;
    if (!fs::is_regular_file(app0 / "DRACRO.CPK", ec)) return;
    CharacterArtSources sources = ArtSourcesOf(app0, "DRACRO_US.CPK");
    sources.cache_dir = base / "stale_cache";
    fs::remove_all(sources.cache_dir);
    std::vector<uint16_t> fresh;
    std::vector<uint8_t> fresh_regions;
    {
        CharacterArt art(sources);
        const std::vector<RegionMap>* maps = art.Regions(CharacterClass::kAmazon);
        CHECK(maps != nullptr && !maps->empty() && !maps->front().pieces.empty());
        if (maps) fresh = maps->front().pieces, fresh_regions = maps->front().region;
    }
    // Old pieces-4 and regions-1 files holding wrong data are not read back; the new ones replace them.
    const auto age = [](const fs::path& file, const std::string& key, const std::string& old) {
        std::vector<uint8_t> bytes;
        CHECK(ReadBytes(file, bytes));
        const auto at = std::search(bytes.begin(), bytes.end(), key.begin(), key.end());
        CHECK(at != bytes.end() && bytes.size() > 12);
        if (at == bytes.end() || bytes.size() <= 12) return;
        bytes[12] = uint8_t(bytes[12] + old.size() - key.size()); // the stored key's length
        const auto from = bytes.erase(at, at + std::ptrdiff_t(key.size()));
        bytes.insert(from, old.begin(), old.end());
        std::fill(bytes.end() - std::min<size_t>(bytes.size() / 2, 400), bytes.end(), uint8_t(0));
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    };
    const fs::path pieces_file = sources.cache_dir / "pieces_Amazon_0.bin";
    const fs::path regions_file = sources.cache_dir / "regions_Amazon_0.bin";
    age(pieces_file, "|Amazon|pieces-22", "|Amazon|pieces-21");
    age(regions_file, "|Amazon|regions-2", "|Amazon|regions-1");
    {
        CharacterArt rebuilt(sources);
        const std::vector<RegionMap>* maps = rebuilt.Regions(CharacterClass::kAmazon);
        CHECK(maps != nullptr && maps->front().pieces == fresh && maps->front().region == fresh_regions);
    }
    for (const auto& [file, key] : {std::pair{pieces_file, std::string("|Amazon|pieces-22")}, std::pair{regions_file, std::string("|Amazon|regions-2")}}) {
        std::vector<uint8_t> renewed;
        CHECK(ReadBytes(file, renewed));
        CHECK(std::search(renewed.begin(), renewed.end(), key.begin(), key.end()) != renewed.end());
    }
    CharacterArt cached(sources);
    CHECK(cached.Regions(CharacterClass::kAmazon) && cached.Regions(CharacterClass::kAmazon)->front().pieces == fresh);
}

// Small detached blobs of drawn pixels: a hidden part must not leave any behind.
size_t Islands(const SpriteImage& image, size_t below = 400) {
    std::vector<uint8_t> seen(image.layer.size(), 0);
    size_t islands = 0;
    for (size_t start = 0; start < seen.size(); ++start) {
        if (seen[start] || image.rgba[start * 4 + 3] < 6) continue;
        std::vector<size_t> stack{start};
        seen[start] = 1;
        size_t size = 0;
        while (!stack.empty()) {
            const size_t p = stack.back();
            stack.pop_back();
            ++size;
            const int x = int(p % size_t(image.width)), y = int(p / size_t(image.width));
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= image.width || ny >= image.height) continue;
                    const size_t q = size_t(ny) * size_t(image.width) + size_t(nx);
                    if (seen[q] || image.rgba[q * 4 + 3] < 6) continue;
                    seen[q] = 1;
                    stack.push_back(q);
                }
            }
        }
        islands += size < below;
    }
    return islands;
}

void TestAmazonParts(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kAmazon;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(select && art.Regions(klass));
    if (!select || !art.Regions(klass)) return;
    const PartRender r{art, klass, *select};
    const CustomPart* hair = FindCustomPart(klass, "hair");
    const CustomPart* skin = FindCustomPart(klass, "skin");
    const CustomPart* tattoo = FindCustomPart(klass, "tattoo");
    const CustomPart* eye = FindCustomPart(klass, "eye");
    const CustomPart* feather = FindCustomPart(klass, "feather");
    const CustomPart* armor = FindCustomPart(klass, "armor");
    std::vector<FtxSurface> presets;
    for (int c = 0; c < kNativeColorCount; ++c) presets.push_back(*ReadFtxSurface(cpk.Read(*cpk.Find(AtlasName(klass, c))), 0));
    const auto is_hair = [&](uint32_t id, size_t) { return id == 201 || id == 211 || id == 221; };
    // Hair painted onto other pieces: the strand behind the neck and the hair on the head.
    const auto hair_painted_on = [&](uint32_t id) {
        return id == 320 || std::find(hair->tone_pieces.begin(), hair->tone_pieces.end(), id) != hair->tone_pieces.end();
    };
    const auto is_feather = [&](uint32_t id, size_t) { return id == 362; };
    for (int color = 0; color < kNativeColorCount; ++color) {
        const SpriteImage native = *art.Preview(klass, color);
        const auto part_pixels = [&](const CustomPart* part) {
            return [&r, &native, part](uint32_t, size_t p) { return r.PartOf(native, p) == part; };
        };
        const PieceDamage hair_all = Damage(r, native, native, is_hair);
        CHECK(hair_all.pixels > 5000);
        size_t hair_elsewhere = 0, armor_as_hair = 0, feather_pixels = 0, eye_pixels = 0, tattoo_pixels = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            if (!r.Visible(native, p)) continue;
            const CustomPart* part = r.PartOf(native, p);
            const uint32_t id = r.IdAt(native, p);
            hair_elsewhere += is_hair(id, p) && part != hair;
            armor_as_hair += part == hair && !is_hair(id, p) && !hair_painted_on(id);
            feather_pixels += part == feather;
            eye_pixels += part == eye;
            tattoo_pixels += part == tattoo;
        }
        CHECK(hair_elsewhere * 100 <= hair_all.pixels && armor_as_hair * 100 <= hair_all.pixels);
        CHECK(feather_pixels > 600 && eye_pixels >= 10 && eye_pixels < 200 && tattoo_pixels > 2500);

        // Hair: recolour and hiding reach hair only; boots, armour and gauntlets keep every pixel.
        const SpriteImage hair_cyan = *art.CustomPreview(klass, color, Painted(color, {"hair"}));
        const PieceDamage hair_armor_c = Damage(r, native, hair_cyan, part_pixels(armor));
        CHECK(hair_armor_c.pixels > 6000 && hair_armor_c.moved * 100 <= hair_armor_c.pixels * 2);
        CHECK(Damage(r, native, hair_cyan, is_hair).moved * 100 >= hair_all.pixels * 95);
        const SpriteImage bald = *art.CustomPreview(klass, color, Hidden(color, {"hair"}));
        const PieceDamage hair_gone = Damage(r, native, bald, is_hair);
        CHECK(hair_gone.moved * 100 >= hair_gone.pixels * 94);
        size_t hair_visible = 0;
        for (size_t p = 0; p < bald.layer.size(); ++p) hair_visible += r.Visible(bald, p) && (is_hair(r.IdAt(bald, p), p) || r.PartOf(bald, p) == hair);
        CHECK(hair_visible == 0);
        const PieceDamage armor_kept = Damage(r, native, bald, part_pixels(armor));
        CHECK(armor_kept.pixels > 6000 && armor_kept.removed * 100 <= armor_kept.pixels && armor_kept.moved * 100 <= armor_kept.pixels * 2);
        // The feather's soft edge lies over hair on the head, which goes.
        const PieceDamage feather_kept = Damage(r, native, bald, is_feather);
        CHECK(feather_kept.removed * 100 <= feather_kept.pixels * 3 && feather_kept.moved * 100 <= feather_kept.pixels * 3);
        // The legs stay too: no pixel of a skin piece outside the hair's reach is removed.
        size_t leg_removed = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            const uint32_t id = r.IdAt(native, p);
            leg_removed += r.Visible(native, p) && (id == 527 || id == 531 || id == 530) && bald.rgba[p * 4 + 3] + 8 < native.rgba[p * 4 + 3];
        }
        CHECK(leg_removed == 0);

        // Skin: only skin changes; hair, feather, eye, boots, armour, clothing and tattoo keep their pixels.
        const SpriteImage skin_green = *art.CustomPreview(klass, color, Painted(color, {"skin"}, {60, 220, 90}));
        CHECK(Compare(r, native, skin_green, skin).part_changed * 100 >= Compare(r, native, skin_green, skin).part_pixels * 95);
        // A part's core is where it and its four neighbours agree: the rim blends with whatever lies next to it.
        const auto core_of = [&](const CustomPart* part) {
            return [&r, &native, part](uint32_t, size_t p) {
                const size_t w = size_t(native.width);
                return r.PartOf(native, p) == part && p > w && p + w < native.layer.size() && r.PartOf(native, p - 1) == part &&
                       r.PartOf(native, p + 1) == part && r.PartOf(native, p - w) == part && r.PartOf(native, p + w) == part;
            };
        };
        const auto untouched = [&](auto selected, size_t allowed_per_mille) {
            const PieceDamage d = Damage(r, native, skin_green, selected);
            return d.pixels > 0 && d.moved * 1000 <= d.pixels * allowed_per_mille;
        };
        CHECK(untouched(is_hair, 30) && untouched(is_feather, 30) && untouched(part_pixels(armor), 30));
        CHECK(untouched(core_of(tattoo), 200) && untouched(part_pixels(eye), 30) && untouched(core_of(FindCustomPart(klass, "bikini")), 100));

        // Boots & Gauntlets hidden leave none of the bands' and bracelets' white highlights next to where they were drawn.
        const SpriteImage no_gear = *art.CustomPreview(klass, color, Hidden(color, {"armor"}));
        const auto near_gear = [&](size_t p) {
            const int x = int(p % size_t(native.width)), y = int(p / size_t(native.width));
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    if (x + dx < 0 || y + dy < 0 || x + dx >= native.width || y + dy >= native.height) continue;
                    const size_t q = size_t(y + dy) * size_t(native.width) + size_t(x + dx);
                    if (r.Visible(native, q) && r.PartOf(native, q) == armor) return true;
                }
            }
            return false;
        };
        size_t arm_pixels = 0, white_left = 0;
        for (size_t p = 0; p < no_gear.layer.size(); ++p) {
            if (!r.Visible(no_gear, p) || no_gear.texture[p] != 0) continue;
            const uint32_t id = r.IdAt(no_gear, p);
            if ((id != 500 && id != 501 && id != 504) || !near_gear(p)) continue;
            ++arm_pixels;
            // White in every preset: the highlights no preset tints, where warm skin always keeps its hue.
            const int tx = int(no_gear.texel[p] % uint32_t(presets.front().width())), ty = int(no_gear.texel[p] / uint32_t(presets.front().width()));
            bool white = true;
            for (const FtxSurface& preset : presets) {
                const std::array<uint8_t, 4> t = preset.Texel(tx, ty);
                const auto [red, blue] = TexelChroma(t);
                white &= Luma(t[0], t[1], t[2]) >= 150.0f && (std::fabs(red) + std::fabs(blue)) * 100.0f <= 22.0f;
            }
            white_left += white;
        }
        CHECK(arm_pixels > 200 && white_left == 0);
        // Skin hidden takes the skin only: the tattoo and the eye painted on it stay while they are shown.
        const SpriteImage no_skin = *art.CustomPreview(klass, color, Hidden(color, {"skin"}));
        const PieceDamage skin_gone = Damage(r, native, no_skin, part_pixels(skin));
        CHECK(skin_gone.pixels > 0 && skin_gone.moved * 100 >= skin_gone.pixels * 97);
        CHECK(Damage(r, native, no_skin, core_of(tattoo)).removed * 100 <= Damage(r, native, no_skin, core_of(tattoo)).pixels * 3);
        CHECK(Damage(r, native, no_skin, is_hair).moved * 100 <= hair_all.pixels * 3);
        CHECK(Damage(r, native, no_skin, is_feather).moved * 100 <= feather_pixels * 2);
        const PieceDamage boots_kept = Damage(r, native, no_skin, [&](uint32_t id, size_t) { return id == 521 || id == 526; });
        CHECK(boots_kept.pixels > 1000 && boots_kept.removed * 100 <= boots_kept.pixels * 3 && boots_kept.moved * 100 <= boots_kept.pixels * 3);
        CHECK(Damage(r, native, no_skin, [](uint32_t, size_t) { return true; }).created == 0);

        // Tattoo: independent of Skin; hiding it clears its own texels, colouring it leaves the skin.
        const PartChange ink = Compare(r, native, *art.CustomPreview(klass, color, Painted(color, {"tattoo"})), tattoo);
        CHECK(ink.part_changed * 10 >= ink.part_pixels * 9 && ink.outside * 10 <= ink.changed && ink.alpha_changed == 0);
        const SpriteImage no_ink = *art.CustomPreview(klass, color, Hidden(color, {"tattoo"}));
        const PieceDamage ink_gone = Damage(r, native, no_ink, core_of(tattoo));
        CHECK(ink_gone.pixels > 0 && ink_gone.moved * 10 >= ink_gone.pixels * 9 && ink_gone.removed * 4 >= ink_gone.pixels * 3);
        CHECK(Damage(r, native, no_ink, core_of(skin)).removed * 100 <= Damage(r, native, no_ink, core_of(skin)).pixels);
        CHECK(FindCustomPart(klass, "tattoo") != FindCustomPart(klass, "skin"));

        // Eye and Feather are independent in colour, visibility and reset.
        const SpriteImage eye_cyan = *art.CustomPreview(klass, color, Painted(color, {"eye"}));
        const SpriteImage feather_pink = *art.CustomPreview(klass, color, Painted(color, {"feather"}, {255, 0, 200}));
        CHECK(Damage(r, native, eye_cyan, part_pixels(eye)).moved * 10 >= eye_pixels * 8);
        CHECK(Damage(r, native, eye_cyan, part_pixels(feather)).moved == 0 && Damage(r, native, eye_cyan, is_hair).moved == 0);
        CHECK(Damage(r, native, feather_pink, part_pixels(feather)).moved * 10 >= feather_pixels * 9);
        CHECK(Damage(r, native, feather_pink, part_pixels(eye)).moved == 0);
        const SpriteImage no_feather = *art.CustomPreview(klass, color, Hidden(color, {"feather"}));
        const SpriteImage no_eye = *art.CustomPreview(klass, color, Hidden(color, {"eye"}));
        CHECK(Damage(r, native, no_feather, part_pixels(feather)).moved * 10 >= feather_pixels * 9);
        CHECK(Damage(r, native, no_feather, part_pixels(eye)).removed == 0 && Damage(r, native, no_feather, part_pixels(eye)).moved == 0);
        CHECK(Damage(r, native, no_eye, part_pixels(feather)).removed == 0 && Damage(r, native, no_eye, part_pixels(feather)).moved == 0);
        CHECK(Damage(r, native, no_eye, part_pixels(eye)).removed * 10 >= eye_pixels * 7); // its own texels cleared
        CustomLook both_colors = Painted(color, {"eye", "feather"});
        both_colors.ResetPart("eye");
        CHECK(both_colors.Find("eye") == nullptr && both_colors.Find("feather") && both_colors.Find("feather")->color);
        const SpriteImage feather_only = *art.CustomPreview(klass, color, both_colors);
        CHECK(Damage(r, native, feather_only, part_pixels(eye)).moved == 0 && Damage(r, native, feather_only, part_pixels(feather)).moved > 0);
        both_colors = Painted(color, {"eye", "feather"});
        both_colors.ResetPart("feather");
        CHECK(both_colors.Find("feather") == nullptr && both_colors.Find("eye") && both_colors.OverrideCount() == 1);

        // Hair hidden leaves no fragment: the strand behind the neck and the hair painted onto the head go with it.
        size_t strand = 0, strand_hair = 0, scalp_hair = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            if (!r.Visible(native, p)) continue;
            const uint32_t id = r.IdAt(native, p);
            strand += id == 320;
            strand_hair += id == 320 && r.PartOf(native, p) == hair;
            scalp_hair += (id == 1 || id == 41) && r.PartOf(native, p) == hair;
        }
        CHECK(strand > 20 && strand_hair * 10 >= strand * 9 && scalp_hair > 20);
        CHECK(Islands(bald) <= Islands(native));
        // The arm bands and bracelets are Boots & Gauntlets: Skin hidden keeps them, Boots & Gauntlets hidden takes them.
        // The bands are thin, so only their core, away from the skin they blend into.
        const auto bands = [&](uint32_t id, size_t p) { return (id == 500 || id == 501 || id == 504) && core_of(armor)(id, p); };
        const PieceDamage bands_kept = Damage(r, native, no_skin, bands);
        CHECK(bands_kept.pixels > 100 && bands_kept.removed * 100 <= bands_kept.pixels * 3);
        const SpriteImage no_armor = *art.CustomPreview(klass, color, Hidden(color, {"armor"}));
        CHECK(Damage(r, native, no_armor, bands).moved * 100 >= bands_kept.pixels * 95);
        // Hiding the feather takes the feather alone, and Feather Color reaches nothing else.
        CustomLook bare_head{true, uint8_t(color), {}};
        bare_head.SetPartHidden("feather", true);
        const SpriteImage no_headgear = *art.CustomPreview(klass, color, bare_head);
        CHECK(Damage(r, native, no_headgear, part_pixels(feather)).moved * 10 >= feather_pixels * 9);
        for (const CustomPart* other : {hair, skin, eye, armor, tattoo}) {
            const PieceDamage kept = Damage(r, native, no_headgear, part_pixels(other));
            CHECK(kept.pixels > 0 && kept.removed * 100 <= kept.pixels * 2);
            CHECK(Damage(r, native, feather_pink, part_pixels(other)).moved * 100 <= kept.pixels);
        }
    }

    // The in-game atlas: eye and feather texels never change with each other, and a look with no change is native.
    const std::vector<RegionMap>& maps = *art.Regions(klass);
    for (int color = 0; color < kNativeColorCount; ++color) {
        const std::vector<uint8_t> pack = cpk.Read(*cpk.Find(AtlasName(klass, color)));
        CHECK(CustomizeAtlas(pack, maps, klass, EditableLook(std::nullopt, uint8_t(color))) == pack);
        const std::optional<FtxSurface> before = ReadFtxSurface(pack, 0);
        const uint16_t eye_bit = PieceBitOf(klass, *eye), feather_bit = PieceBitOf(klass, *feather);
        const auto texels_changed = [&](const CustomLook& look, uint8_t bit, int region_bit) {
            const std::optional<std::vector<uint8_t>> served = CustomizeAtlas(pack, maps, klass, look);
            CHECK(served.has_value());
            size_t changed = 0, total = 0;
            if (!served) return std::pair<size_t, size_t>{0, 0};
            const std::optional<FtxSurface> after = ReadFtxSurface(*served, 0);
            const RegionMap& map = maps.front();
            for (int y = 0; y < map.height; ++y) {
                for (int x = 0; x < map.width; ++x) {
                    if (!(map.Pieces(x, y) & bit) && !(region_bit && map.At(x, y) == region_bit)) continue;
                    ++total;
                    changed += before->Texel(x, y) != after->Texel(x, y);
                }
            }
            return std::pair<size_t, size_t>{changed, total};
        };
        const auto eye_only = texels_changed(Painted(color, {"eye"}), feather_bit, 0);
        const auto feather_only = texels_changed(Painted(color, {"feather"}, {255, 0, 200}), eye_bit, 0);
        CHECK(eye_only.second > 500 && eye_only.first == 0);       // every feather texel is byte-identical
        CHECK(feather_only.second > 5 && feather_only.first == 0); // every eye texel is byte-identical
        // Shader level, before any BC7 encoding: a part's colour never reaches another part's own texels.
        const auto shader_moves = [&](const CustomLook& look, uint8_t bit) {
            const LookShader shader(klass, look, maps.front(), [&](int x, int y) { return before->Texel(x, y); });
            size_t moved = 0, total = 0;
            for (int y = 0; y < maps.front().height; y += 2) {
                for (int x = 0; x < maps.front().width; x += 2) {
                    if (!(maps.front().Pieces(x, y) & bit)) continue;
                    std::array<uint8_t, 4> texel = before->Texel(x, y);
                    const std::array<uint8_t, 4> native = texel;
                    shader.ApplyAt(x, y, texel);
                    ++total;
                    moved += texel != native;
                }
            }
            return std::pair<size_t, size_t>{moved, total};
        };
        const uint16_t tattoo_bit = PieceBitOf(klass, *tattoo), skin_bit = PieceBitOf(klass, *skin), hair_bit = PieceBitOf(klass, *hair);
        CHECK(shader_moves(Painted(color, {"skin"}, {60, 220, 90}), tattoo_bit).second > 1000 && shader_moves(Painted(color, {"skin"}, {60, 220, 90}), tattoo_bit).first == 0);
        CHECK(shader_moves(Painted(color, {"skin"}, {60, 220, 90}), eye_bit).first == 0 && shader_moves(Painted(color, {"skin"}, {60, 220, 90}), feather_bit).first == 0);
        CHECK(shader_moves(Painted(color, {"hair"}), skin_bit).first == 0 && shader_moves(Painted(color, {"hair"}), tattoo_bit).first == 0);
        CHECK(shader_moves(Painted(color, {"hair"}), PieceBitOf(klass, *armor)).first == 0 && shader_moves(Painted(color, {"hair"}), eye_bit).first == 0);
        CHECK(shader_moves(Painted(color, {"tattoo"}), skin_bit).first == 0 && shader_moves(Painted(color, {"tattoo"}), hair_bit).first == 0);
        CHECK(shader_moves(Painted(color, {"eye"}), feather_bit).first == 0 && shader_moves(Painted(color, {"feather"}), eye_bit).first == 0);
        CHECK(shader_moves(Painted(color, {"tattoo"}), tattoo_bit).first * 10 >= shader_moves(Painted(color, {"tattoo"}), tattoo_bit).second * 9);
        CHECK(texels_changed(Painted(color, {"feather"}, {255, 0, 200}), feather_bit, 0).first * 10 >= eye_only.second * 8);
        CHECK(texels_changed(Painted(color, {"eye"}), eye_bit, 0).first * 10 >= feather_only.second * 8);
    }
}

void TestSorceressParts(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kSorceress;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(select && art.Regions(klass));
    if (!select || !art.Regions(klass)) return;
    const PartRender r{art, klass, *select};
    const RegionMap& map = art.Regions(klass)->front();
    const CustomPart* hair = FindCustomPart(klass, "hair");
    const CustomPart* dress = FindCustomPart(klass, "dress");
    const CustomPart* bodice = FindCustomPart(klass, "bodice");
    const CustomPart* hat = FindCustomPart(klass, "hat");
    const CustomPart* boots = FindCustomPart(klass, "boots");
    const auto is_hair = [&](uint32_t id, size_t) { return hair->OwnsPiece(id); };
    const auto is_dress = [&](uint32_t id, size_t) { return dress->OwnsPiece(id); };
    const auto is_boots = [&](uint32_t id, size_t) { return boots->OwnsPiece(id); };
    const auto is_body = [&](uint32_t id, size_t) { return id == 1 || (id >= 1400 && id < 1500 && !boots->OwnsPiece(id)); };
    const std::span<const uint32_t> headgear = hat->pieces;
    const auto is_hat_piece = [&](uint32_t id, size_t) { return std::find(headgear.begin(), headgear.end(), id) != headgear.end(); };
    const auto is_bodice_ink = [&](uint32_t id, size_t p) {
        const SpriteImage& n = *art.Preview(klass, 0);
        return bodice->OwnsPiece(id) && map.At(int(n.texel[p] % uint32_t(map.width)), int(n.texel[p] / uint32_t(map.width))) == 1;
    };
    for (int color = 0; color < kNativeColorCount; ++color) {
        const SpriteImage native = *art.Preview(klass, color);
        const PieceDamage hair_all = Damage(r, native, native, is_hair);
        const PieceDamage dress_all = Damage(r, native, native, is_dress);
        CHECK(hair_all.pixels > 4000 && dress_all.pixels > 12000);

        // Hair: the old floating fragments are gone; hat, face, skin, dress, bodice, staff and boots stay.
        const SpriteImage bald = *art.CustomPreview(klass, color, Hidden(color, {"hair"}));
        size_t hair_left = 0;
        for (size_t p = 0; p < bald.layer.size(); ++p) {
            hair_left += r.Visible(bald, p) && hair->OwnsPiece(r.IdAt(bald, p));
            hair_left += r.Visible(bald, p) && r.PartOf(bald, p) == hair;
        }
        CHECK(hair_left == 0);
        for (const auto& selected : {std::function<bool(uint32_t, size_t)>(is_dress), std::function<bool(uint32_t, size_t)>(is_boots),
                                     std::function<bool(uint32_t, size_t)>(is_body), std::function<bool(uint32_t, size_t)>(is_hat_piece),
                                     std::function<bool(uint32_t, size_t)>([&](uint32_t id, size_t) { return bodice->OwnsPiece(id); }),
                                     std::function<bool(uint32_t, size_t)>([](uint32_t id, size_t) { return id == 5012; })}) {
            const PieceDamage d = Damage(r, native, bald, selected);
            CHECK(d.pixels > 0 && d.removed * 100 <= d.pixels * 3 && d.moved * 100 <= d.pixels * 3);
        }

        // Dress: lining, inner skirt, rear panels and the dark folds are all the one part; nothing of it survives hiding.
        size_t lining = 0, lining_ok = 0, inner = 0, inner_ok = 0, outside_dress = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            if (!r.Visible(native, p) || native.texture[p] != 0) continue;
            const uint32_t id = r.IdAt(native, p);
            const CustomPart* part = r.PartOf(native, p);
            const int region = map.At(int(native.texel[p] % uint32_t(map.width)), int(native.texel[p] / uint32_t(map.width)));
            lining += id == 548;
            lining_ok += id == 548 && part == dress;
            inner += id == 516 && region == 1;
            inner_ok += id == 516 && region == 1 && part == dress;
            outside_dress += dress->OwnsPiece(id) && part != dress;
        }
        CHECK(lining > 400 && lining_ok == lining && inner > 300 && inner_ok == inner && outside_dress == 0);
        const SpriteImage dress_cyan = *art.CustomPreview(klass, color, Painted(color, {"dress"}));
        const PieceDamage cyan_d = Damage(r, native, dress_cyan, is_dress);
        CHECK(cyan_d.moved * 100 >= cyan_d.pixels * 97);
        for (const auto& selected : {std::function<bool(uint32_t, size_t)>(is_body), std::function<bool(uint32_t, size_t)>(is_boots),
                                     std::function<bool(uint32_t, size_t)>(is_hair)}) {
            const PieceDamage d = Damage(r, native, dress_cyan, selected);
            CHECK(d.pixels > 0 && d.moved * 100 <= d.pixels * 2);
        }
        const SpriteImage no_dress = *art.CustomPreview(klass, color, Hidden(color, {"dress"}));
        const PieceDamage dress_gone = Damage(r, native, no_dress, is_dress);
        CHECK(dress_gone.moved * 1000 >= dress_gone.pixels * 985);
        size_t dress_left = 0;
        for (size_t p = 0; p < no_dress.layer.size(); ++p) dress_left += r.Visible(no_dress, p) && (dress->OwnsPiece(r.IdAt(no_dress, p)) || r.PartOf(no_dress, p) == dress);
        CHECK(dress_left == 0);
        for (const auto& selected : {std::function<bool(uint32_t, size_t)>(is_body), std::function<bool(uint32_t, size_t)>(is_boots),
                                     std::function<bool(uint32_t, size_t)>(is_hair), std::function<bool(uint32_t, size_t)>(is_hat_piece),
                                     std::function<bool(uint32_t, size_t)>([&](uint32_t id, size_t) { return bodice->OwnsPiece(id); })}) {
            const PieceDamage d = Damage(r, native, no_dress, selected);
            CHECK(d.pixels > 0 && d.removed * 100 <= d.pixels && d.moved * 100 <= d.pixels * 3);
        }
        CHECK(Damage(r, native, no_dress, [](uint32_t, size_t) { return true; }).created == 0);

        // Bodice: an existing garment, coloured or removed on its own; nothing is made up in its place.
        size_t bodice_pixels = 0, bodice_ok = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            if (!r.Visible(native, p) || native.texture[p] != 0 || !is_bodice_ink(r.IdAt(native, p), p)) continue;
            ++bodice_pixels;
            bodice_ok += r.PartOf(native, p) == bodice;
        }
        CHECK(bodice_pixels > 1500 && bodice_ok * 100 >= bodice_pixels * 98);
        const PartChange tint = Compare(r, native, *art.CustomPreview(klass, color, Painted(color, {"bodice"})), bodice);
        CHECK(tint.part_changed * 100 >= tint.part_pixels * 95 && tint.alpha_changed == 0 && tint.outside * 10 <= tint.changed);
        const SpriteImage no_bodice = *art.CustomPreview(klass, color, Hidden(color, {"bodice"}));
        const PartChange cut = Compare(r, native, no_bodice, bodice);
        CHECK(cut.part_changed * 100 >= cut.part_pixels * 97 && cut.outside * 10 <= cut.changed);
        CHECK(Damage(r, native, no_bodice, [](uint32_t, size_t) { return true; }).created == 0);
        for (const auto& selected : {std::function<bool(uint32_t, size_t)>(is_dress), std::function<bool(uint32_t, size_t)>(is_boots),
                                     std::function<bool(uint32_t, size_t)>(is_hair), std::function<bool(uint32_t, size_t)>(is_hat_piece),
                                     std::function<bool(uint32_t, size_t)>(is_body)}) {
            const PieceDamage d = Damage(r, native, no_bodice, selected);
            CHECK(d.pixels > 0 && d.removed * 100 <= d.pixels && d.moved * 100 <= d.pixels * 3);
        }
        CustomLook reset = Painted(color, {"bodice"});
        reset.ResetPart("bodice");
        CHECK(art.CustomPreview(klass, color, reset)->rgba == native.rgba);
        CHECK(art.CustomPreview(klass, color, EditableLook(std::nullopt, uint8_t(color)))->rgba == native.rgba);

        // Hat and Hat Band hidden: every hat, band and brim pixel and the dark outline pixels of the hat go; head, hair and body stay.
        CustomLook gone{true, uint8_t(color), {}};
        gone.SetPartHidden("hat", true);
        gone.SetPartHidden("hat_band", true);
        const SpriteImage no_hat = *art.CustomPreview(klass, color, gone);
        size_t hat_left = 0, dark_left = 0;
        for (size_t p = 0; p < no_hat.layer.size(); ++p) {
            if (!r.Visible(no_hat, p)) continue;
            const uint32_t id = r.IdAt(no_hat, p);
            hat_left += is_hat_piece(id, p);
            const int sum = no_hat.rgba[p * 4] + no_hat.rgba[p * 4 + 1] + no_hat.rgba[p * 4 + 2];
            dark_left += sum < 90 && !hair->OwnsPiece(id) && id != 1 && !dress->OwnsPiece(id) && !bodice->OwnsPiece(id) && !boots->OwnsPiece(id) &&
                         id != 5012 && !(id >= 1400 && id < 1500) && !(id >= 1000 && id < 1300) && native.rgba[p * 4 + 3] != 0 &&
                         r.PartOf(native, p) == hat;
        }
        CHECK(hat_left == 0 && dark_left == 0);
        for (const auto& selected : {std::function<bool(uint32_t, size_t)>(is_hair), std::function<bool(uint32_t, size_t)>(is_body),
                                     std::function<bool(uint32_t, size_t)>(is_dress), std::function<bool(uint32_t, size_t)>(is_boots),
                                     std::function<bool(uint32_t, size_t)>([&](uint32_t id, size_t) { return bodice->OwnsPiece(id); })}) {
            const PieceDamage d = Damage(r, native, no_hat, selected);
            CHECK(d.pixels > 0 && d.removed * 100 <= d.pixels && d.moved * 100 <= d.pixels * 3);
        }
    }

    // Headgear in the game atlas covers every hat piece of the in-game model, not only the ones the select pose draws.
    const std::vector<RegionMap>& maps = *art.Regions(klass);
    const std::optional<Mbs> game = ParseMbs(cpk.Read(*cpk.Find(GameModelName(klass))));
    CHECK(game.has_value());
    if (game) {
        size_t hat_quads = 0, covered = 0;
        std::set<uint32_t> seen;
        for (const MbsLayer& layer : game->layers) {
            if (layer.texture != 0 || !is_hat_piece(layer.id, 0)) continue;
            const MbsQuad& q = game->sources[layer.source];
            if (q.center.x < 0.0f || q.center.x > 1.0f || q.center.y < 0.0f || q.center.y > 1.0f) continue;
            seen.insert(layer.id);
            ++hat_quads;
            const int qx = int(q.center.x * float(maps.front().width)), qy = int(q.center.y * float(maps.front().height));
            covered += (maps.front().Pieces(qx, qy) & PieceBitOf(klass, *hat)) != 0 || maps.front().At(qx, qy) == 2 || maps.front().At(qx, qy) == 6;
        }
        CHECK(seen.size() >= 20 && hat_quads > 1000 && covered * 100 >= hat_quads * 98);
        size_t hair_quads = 0, hair_covered = 0;
        for (const MbsLayer& layer : game->layers) {
            if (layer.texture != 0 || !hair->OwnsPiece(layer.id)) continue;
            const MbsQuad& q = game->sources[layer.source];
            if (q.center.x < 0.0f || q.center.x > 1.0f || q.center.y < 0.0f || q.center.y > 1.0f) continue;
            ++hair_quads;
            hair_covered += (maps.front().Pieces(int(q.center.x * float(maps.front().width)), int(q.center.y * float(maps.front().height))) & PieceBitOf(klass, *hair)) != 0;
        }
        CHECK(hair_quads > 1000 && hair_covered * 100 >= hair_quads * 90);
    }
}

// Pixels of `part` in the native preview, and how many of them a changed preview moved or removed.
struct PartEffect {
    size_t pixels = 0, moved = 0, removed = 0;
};
PartEffect EffectOn(const PartRender& r, const SpriteImage& native, const SpriteImage& after, const CustomPart* part) {
    PartEffect effect;
    for (size_t p = 0; p < native.layer.size(); ++p) {
        if (!r.Visible(native, p) || r.PartOf(native, p) != part) continue;
        ++effect.pixels;
        effect.moved += PartRender::Delta(native, after, p) > 24;
        effect.removed += int(after.rgba[p * 4 + 3]) + 8 < int(native.rgba[p * 4 + 3]);
    }
    return effect;
}

// Pixels a preview still draws from one texture id.
size_t DrawnFrom(const SpriteImage& image, uint8_t texture) {
    size_t drawn = 0;
    for (size_t p = 0; p < image.layer.size(); ++p) drawn += image.layer[p] != kNoLayer && image.texture[p] == texture;
    return drawn;
}

// Native pixels drawn from one texture id, and how many of them a changed preview moved or removed.
PartEffect EffectOnTexture(const SpriteImage& native, const SpriteImage& after, uint8_t texture) {
    PartEffect effect;
    for (size_t p = 0; p < native.layer.size(); ++p) {
        if (native.layer[p] == kNoLayer || native.rgba[p * 4 + 3] < 64 || native.texture[p] != texture) continue;
        ++effect.pixels;
        effect.moved += PartRender::Delta(native, after, p) > 24;
        effect.removed += int(after.rgba[p * 4 + 3]) + 8 < int(native.rgba[p * 4 + 3]);
    }
    return effect;
}

void TestSorceressSplit(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kSorceress;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(select && art.Regions(klass));
    if (!select || !art.Regions(klass)) return;
    const PartRender r{art, klass, *select};
    const CustomPart* skin = FindCustomPart(klass, "skin");
    const CustomPart* lace = FindCustomPart(klass, "lace");
    const CustomPart* frills = FindCustomPart(klass, "frills");
    const CustomPart* eye = FindCustomPart(klass, "eye");
    const CustomPart* hair = FindCustomPart(klass, "hair");
    const CustomPart* dress = FindCustomPart(klass, "dress");
    const CustomPart* bodice = FindCustomPart(klass, "bodice");
    CHECK(skin && lace && frills && eye && hair && dress && bodice);
    if (!skin || !lace || !frills || !eye || !hair || !dress || !bodice) return;
    CHECK(lace->label == "Bust Lace" && frills->label == "Sleeve Frills" && eye->label == "Eyes");
    CHECK(lace->mark == PaintedMark::kDyed && frills->mark == PaintedMark::kDyed);

    const RegionMap& map = art.Regions(klass)->front();
    size_t lace_texels = 0, frill_texels = 0, eye_texels = 0;
    for (const uint16_t v : map.pieces) {
        lace_texels += (v & PieceBitOf(klass, *lace)) != 0;
        frill_texels += (v & PieceBitOf(klass, *frills)) != 0;
        eye_texels += (v & PieceBitOf(klass, *eye)) != 0;
    }
    CHECK(lace_texels > 2000 && frill_texels > 300 && eye_texels > 300);
    std::vector<FtxSurface> presets;
    for (int c = 0; c < kNativeColorCount; ++c) presets.push_back(*ReadFtxSurface(cpk.Read(*cpk.Find(AtlasName(klass, c))), 0));
    const auto dyed = [&](const SpriteImage& image, size_t p) {
        std::array<std::array<uint8_t, 4>, kNativeColorCount> t{};
        const int x = int(image.texel[p] % uint32_t(map.width)), y = int(image.texel[p] / uint32_t(map.width));
        for (int c = 0; c < kNativeColorCount; ++c) t[size_t(c)] = presets[size_t(c)].Texel(x, y);
        return Dyed(t, kDyedCoreShift, kDyedCoreSaturation);
    };
    // The skin's own tone through the presets, from the texels it owns.
    std::vector<std::pair<float, float>> skin_tone(kNativeColorCount);
    size_t skin_samples = 0;
    for (int y = 0; y < map.height; y += 2) {
        for (int x = 0; x < map.width; x += 2) {
            if (!(map.Pieces(x, y) & PieceBitOf(klass, *skin)) || presets[0].Texel(x, y)[3] < 255) continue;
            for (int c = 0; c < kNativeColorCount; ++c) {
                const auto [red, blue] = TexelChroma(presets[size_t(c)].Texel(x, y));
                skin_tone[size_t(c)].first += red;
                skin_tone[size_t(c)].second += blue;
            }
            ++skin_samples;
        }
    }
    for (auto& [red, blue] : skin_tone) {
        red /= float(std::max<size_t>(skin_samples, 1));
        blue /= float(std::max<size_t>(skin_samples, 1));
    }
    CHECK(skin_samples > 1000);
    // Another part's colour reaches only the blended rim of a part (the frills are small and mostly rim).
    const auto unmoved = [](const PartEffect& e) { return e.pixels > 0 && e.moved * 100 <= e.pixels * 5; };

    for (int color = 0; color < kNativeColorCount; ++color) {
        const SpriteImage native = *art.Preview(klass, color);
        // The three marked areas are garments of their own: the lace on both sides of the bust and the arm frill.
        size_t lace_left = 0, lace_right = 0, frill_arm = 0, skin_dyed = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            if (!r.Visible(native, p) || native.texture[p] != 0) continue;
            const uint32_t id = r.IdAt(native, p);
            const CustomPart* part = r.PartOf(native, p);
            lace_left += part == lace && (id == 1010 || id == 1012);
            lace_right += part == lace && id == 1011;
            frill_arm += part == frills && id == 1202;
            skin_dyed += part == skin && (id == 1010 || id == 1011 || id == 1012 || id == 1202) && dyed(native, p);
        }
        CHECK(lace_left > 300 && lace_right > 150 && frill_arm > 100);
        CHECK(skin_dyed == 0);

        // Each colour is independent.
        const SpriteImage lace_red = *art.CustomPreview(klass, color, Painted(color, {"lace"}, {220, 30, 30}));
        const PartEffect lace_changed = EffectOn(r, native, lace_red, lace);
        CHECK(lace_changed.moved * 100 >= lace_changed.pixels * 90 && lace_changed.removed == 0);
        CHECK(unmoved(EffectOn(r, native, lace_red, frills)) && unmoved(EffectOn(r, native, lace_red, skin)));
        const SpriteImage frill_blue = *art.CustomPreview(klass, color, Painted(color, {"frills"}, {30, 90, 230}));
        const PartEffect frill_changed = EffectOn(r, native, frill_blue, frills);
        CHECK(frill_changed.moved * 100 >= frill_changed.pixels * 85);
        CHECK(unmoved(EffectOn(r, native, frill_blue, lace)) && unmoved(EffectOn(r, native, frill_blue, skin)));
        const SpriteImage skin_tan = *art.CustomPreview(klass, color, Painted(color, {"skin"}, {40, 150, 60}));
        const PartEffect skin_changed = EffectOn(r, native, skin_tan, skin);
        CHECK(skin_changed.moved * 100 >= skin_changed.pixels * 80);
        CHECK(unmoved(EffectOn(r, native, skin_tan, lace)));
        CHECK(unmoved(EffectOn(r, native, skin_tan, frills)));
        CHECK(unmoved(EffectOn(r, native, skin_tan, eye)));

        // Skin hidden: the garments stay, and so do the eyes while they are shown.
        const SpriteImage bare = *art.CustomPreview(klass, color, Hidden(color, {"skin"}));
        for (const CustomPart* garment : {lace, frills, bodice, dress}) {
            const PartEffect e = EffectOn(r, native, bare, garment);
            CHECK(e.pixels > 0 && e.removed * 100 <= e.pixels * 3);
        }
        const PartEffect eyes_kept = EffectOn(r, native, bare, eye);
        CHECK(eyes_kept.pixels > 20 && eyes_kept.removed * 100 <= eyes_kept.pixels * 30); // their rim blends with the face
        const PartEffect skin_gone = EffectOn(r, native, bare, skin);
        CHECK(skin_gone.moved * 100 >= skin_gone.pixels * 95);
        // No speck of skin is left on the lace and frill pieces: what still draws there is the dyed cloth.
        size_t garment_left = 0, skin_toned = 0;
        for (size_t p = 0; p < bare.layer.size(); ++p) {
            if (!r.Visible(bare, p) || bare.texture[p] != 0) continue;
            const uint32_t id = r.IdAt(bare, p);
            if (!lace->OwnsPiece(id) && !frills->OwnsPiece(id)) continue;
            std::array<std::array<uint8_t, 4>, kNativeColorCount> t{};
            const int x = int(bare.texel[p] % uint32_t(map.width)), y = int(bare.texel[p] / uint32_t(map.width));
            for (int c = 0; c < kNativeColorCount; ++c) t[size_t(c)] = presets[size_t(c)].Texel(x, y);
            ++garment_left;
            skin_toned += ToneDistance(t, skin_tone) <= kSkinToneCore;
        }
        CHECK(garment_left > 500 && skin_toned == 0);

        // Lace and frills hidden: each goes alone, and nothing is painted in its place.
        for (const CustomPart* garment : {lace, frills}) {
            const SpriteImage gone = *art.CustomPreview(klass, color, Hidden(color, {std::string(garment->key).c_str()}));
            const PartEffect own = EffectOn(r, native, gone, garment);
            size_t left = 0;
            for (size_t p = 0; p < gone.layer.size(); ++p) left += r.Visible(gone, p) && r.PartOf(gone, p) == garment;
            // What lies behind can match the cloth's shade, and the rim blends with its neighbours.
            CHECK(own.pixels > 100 && own.moved * 100 >= own.pixels * 75 && left * 100 <= own.pixels);
            for (const CustomPart* other : {skin, eye, hair, dress, bodice, lace, frills}) {
                if (other == garment) continue;
                const PartEffect e = EffectOn(r, native, gone, other);
                CHECK(e.pixels > 0 && e.removed * 100 <= e.pixels * 3);
            }
            CHECK(Damage(r, native, gone, [](uint32_t, size_t) { return true; }).created == 0);
        }

        // Hair and Dress hidden leave no fragment anywhere.
        const SpriteImage bald = *art.CustomPreview(klass, color, Hidden(color, {"hair"}));
        const PartEffect hair_gone = EffectOn(r, native, bald, hair);
        CHECK(Islands(bald) <= Islands(native));
        CHECK(hair_gone.moved * 100 >= hair_gone.pixels * 97);
        const SpriteImage no_dress = *art.CustomPreview(klass, color, Hidden(color, {"dress"}));
        const PartEffect dress_gone = EffectOn(r, native, no_dress, dress);
        CHECK(Islands(no_dress) <= Islands(native));
        CHECK(dress_gone.moved * 100 >= dress_gone.pixels * 97);
        CHECK(Damage(r, native, no_dress, [](uint32_t, size_t) { return true; }).created == 0);
    }

    // In game: every quad the model draws for the hair or the dress, in any pose, is cleared by hiding the part.
    const std::optional<Mbs> game = ParseMbs(cpk.Read(*cpk.Find(GameModelName(klass))));
    CHECK(game.has_value());
    if (!game) return;
    for (const CustomPart* part : {hair, dress}) {
        const LookShader shader(klass, Hidden(0, {std::string(part->key).c_str()}), map,
                                [&](int x, int y) { return presets[0].Texel(x, y); });
        size_t quads = 0, left = 0;
        for (const MbsLayer& layer : game->layers) {
            if (layer.texture != 0 || !part->OwnsPiece(layer.id)) continue;
            const MbsQuad& q = game->sources[layer.source];
            if (q.center.x < 0.0f || q.center.x > 1.0f || q.center.y < 0.0f || q.center.y > 1.0f) continue;
            const int x = int(q.center.x * float(map.width)), y = int(q.center.y * float(map.height));
            std::array<uint8_t, 4> texel = presets[0].Texel(x, y);
            if (texel[3] < 64) continue;
            ++quads;
            shader.ApplyAt(x, y, texel);
            left += texel[3] != 0;
        }
        CHECK(quads > 1000 && left == 0);
    }
}

// The part whose look a texel of an atlas surface follows: its piece part, else its region's part.
const CustomPart* OwnerOfTexel(CharacterClass klass, const RegionMap& map, int x, int y) {
    const uint16_t owned = map.Pieces(x, y);
    if (owned) {
        for (const CustomPart& part : CustomPartsOf(klass)) {
            if (const uint16_t bit = PieceBitOf(klass, part); bit && (owned & bit)) return &part;
        }
    }
    return PartOfRegion(klass, map.At(x, y));
}

struct BodyAtlas {
    std::vector<uint8_t> pack;
    std::vector<FtxSurface> surfaces; // by BodySurfacesOf
};
BodyAtlas ReadBodyAtlas(CpkArchive& cpk, CharacterClass klass, int color) {
    BodyAtlas atlas{cpk.Read(*cpk.Find(AtlasName(klass, color))), {}};
    for (const BodySurface& body : BodySurfacesOf(klass)) atlas.surfaces.push_back(*ReadFtxSurface(atlas.pack, body.surface));
    return atlas;
}

// Texels of every body surface (a sample of them) that a look changes, split by whether their owner is one of `parts`.
struct AtlasChange {
    size_t inside = 0, inside_changed = 0, outside_changed = 0, inside_cleared = 0;
};
AtlasChange ChangeOnAtlas(CharacterClass klass, const std::vector<RegionMap>& maps, const BodyAtlas& atlas, const CustomLook& look,
                          std::initializer_list<const CustomPart*> parts) {
    AtlasChange change;
    for (size_t b = 0; b < maps.size() && b < atlas.surfaces.size(); ++b) {
        const RegionMap& map = maps[b];
        const FtxSurface& surface = atlas.surfaces[b];
        const LookShader shader(klass, look, map, [&](int x, int y) { return surface.Texel(x, y); });
        for (int y = 1; y < map.height; y += 3) {
            for (int x = 1; x < map.width; x += 3) {
                const std::array<uint8_t, 4> before = surface.Texel(x, y);
                if (!before[3] || !map.At(x, y)) continue;
                std::array<uint8_t, 4> after = before;
                shader.ApplyAt(x, y, after);
                const CustomPart* owner = OwnerOfTexel(klass, map, x, y);
                const bool mine = std::find(parts.begin(), parts.end(), owner) != parts.end();
                change.inside += mine;
                change.inside_changed += mine && after != before;
                change.inside_cleared += mine && after[3] == 0;
                change.outside_changed += !mine && after != before;
            }
        }
    }
    return change;
}

uint64_t ImageHash(const SpriteImage& image) {
    uint64_t hash = 1469598103934665603ull;
    for (const uint8_t byte : image.rgba) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

// The in-game atlas of a look matches the shader texel for texel (within BC7), keeps every untouched block byte for
// byte, and draws the select pose as the Editor preview draws it.
void CheckServedLook(CharacterArt& art, CpkArchive& cpk, CharacterClass klass, const Mbs& select, const CustomLook& look) {
    const std::vector<RegionMap>& maps = *art.Regions(klass);
    const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, look.base);
    const std::optional<std::vector<uint8_t>> served = CustomizeAtlas(atlas.pack, maps, klass, look);
    CHECK(served && *served != atlas.pack && served->size() == atlas.pack.size());
    if (!served) return;
    const std::span<const BodySurface> bodies = BodySurfacesOf(klass);
    std::vector<FtxSurface> served_surfaces;
    for (size_t b = 0; b < bodies.size() && b < maps.size(); ++b) {
        const FtxSurface& before = atlas.surfaces[b];
        const std::optional<FtxSurface> after = ReadFtxSurface(*served, bodies[b].surface);
        const std::optional<FtxSurfaceLayout> layout = FtxLayoutOf(atlas.pack, bodies[b].surface);
        CHECK(after && layout);
        if (!after || !layout) return;
        const LookShader shader(klass, look, maps[b], [&](int x, int y) { return before.Texel(x, y); });
        double error = 0.0;
        size_t samples = 0, untouched_differ = 0, transparent_shown = 0;
        for (int by = 0; by < layout->height / 4; by += 2) {
            for (int bx = 0; bx < layout->width / 4; bx += 2) {
                bool touched = false;
                for (int t = 0; t < 16 && !touched; ++t) touched = shader.ChangesAt(bx * 4 + (t & 3), by * 4 + (t >> 2));
                const size_t at = FtxBlockOffset(*layout, bx, by);
                if (!touched) {
                    untouched_differ += std::memcmp(atlas.pack.data() + at, served->data() + at, 16) != 0;
                    continue;
                }
                for (int t = 0; t < 16; ++t) {
                    const int x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                    std::array<uint8_t, 4> expected = before.Texel(x, y);
                    shader.ApplyAt(x, y, expected);
                    const std::array<uint8_t, 4> got = after->Texel(x, y);
                    if (!expected[3]) { // a cleared texel stays cleared; its colour only pads the edge beside it
                        transparent_shown += got[3] != 0;
                        continue;
                    }
                    for (int c = 0; c < 4; ++c) error += std::abs(int(got[c]) - int(expected[c]));
                    samples += 4;
                }
            }
        }
        CHECK(untouched_differ == 0 && transparent_shown == 0 && (samples == 0 || error / double(samples) < 4.0));
        served_surfaces.push_back(*after);
    }
    std::vector<FtxSurface> weapons;
    for (size_t w = 0; w < WeaponTexturesOf(klass).size(); ++w) {
        const std::vector<uint8_t> pack = cpk.Read(*cpk.Find(WeaponAtlasName(klass, WeaponTexturesOf(klass)[w])));
        const std::optional<std::vector<uint8_t>> weapon = CustomizeWeapon(pack, klass, w, look);
        CHECK(weapon.has_value());
        if (!weapon) return;
        weapons.push_back(*ReadFtxSurface(*weapon, 0));
    }
    SpriteRequest request;
    request.mbs = &select;
    request.animation = PreviewAnimation(klass);
    request.scale = 2.0f;
    for (size_t b = 0; b < served_surfaces.size(); ++b) request.textures[bodies[b].texture] = &served_surfaces[b];
    for (size_t w = 0; w < weapons.size(); ++w) request.textures[WeaponTexturesOf(klass)[w].texture] = &weapons[w];
    const SpriteImage runtime = RenderSprite(request);
    const SpriteImage preview = *art.CustomPreview(klass, look.base, look);
    CHECK(runtime.width == preview.width && runtime.height == preview.height && runtime.rgba.size() == preview.rgba.size());
    if (runtime.rgba.size() != preview.rgba.size()) return;
    // Compared as seen: colour weighted by alpha, and alpha. A transparent texel's colour shows nowhere.
    double difference = 0.0;
    for (size_t p = 0; p < runtime.layer.size(); ++p) {
        const uint8_t* a = &runtime.rgba[p * 4];
        const uint8_t* b = &preview.rgba[p * 4];
        for (int c = 0; c < 3; ++c) difference += std::abs(int(a[c]) * a[3] - int(b[c]) * b[3]) / 255.0;
        difference += std::abs(int(a[3]) - int(b[3]));
    }
    CHECK(difference / double(runtime.rgba.size()) < 1.0);
}

// A frame of a gameplay animation of the in-game model, drawn from the atlas with the look applied as the game's is. A
// track, when given, is drawn alone: tracks run on their own clocks, so the same frame index of two is not one moment.
SpriteImage RenderGamePose(CpkArchive& cpk, const std::vector<RegionMap>& maps, CharacterClass klass, const Mbs& game,
                           std::string_view animation, size_t frame, const CustomLook& look, std::optional<size_t> track = std::nullopt) {
    const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, look.base);
    const std::span<const BodySurface> bodies = BodySurfacesOf(klass);
    const std::span<const WeaponTexture> weapon_textures = WeaponTexturesOf(klass);
    std::vector<FtxSurface> weapons;
    for (const WeaponTexture& weapon : weapon_textures) weapons.push_back(*ReadFtxSurface(cpk.Read(*cpk.Find(WeaponAtlasName(klass, weapon))), 0));
    SpriteRequest request;
    request.mbs = &game;
    for (size_t a = 0; a < game.animations.size(); ++a) {
        if (game.animations[a].name == animation) request.animation = a;
    }
    request.frame = frame;
    std::set<uint32_t> drawn;
    if (track) {
        const MbsAnimation& anim = game.animations[request.animation];
        const MbsTrack& run = game.tracks[anim.first_track + *track];
        const MbsKey& key = game.keys[game.frames[run.first_frame + std::min<size_t>(frame, run.frame_count - 1)].key];
        for (uint32_t i = 0; i < key.layer_count; ++i) drawn.insert(key.first_layer + i);
        request.visible = [&drawn](uint32_t layer) { return drawn.count(layer) > 0; };
    }
    for (size_t b = 0; b < bodies.size(); ++b) request.textures[bodies[b].texture] = &atlas.surfaces[b];
    for (size_t w = 0; w < weapons.size(); ++w) request.textures[weapon_textures[w].texture] = &weapons[w];
    std::vector<LookShader> shaders;
    std::vector<WeaponShader> weapon_shaders;
    if (look.active()) {
        for (size_t b = 0; b < bodies.size() && b < maps.size(); ++b) {
            const FtxSurface* surface = &atlas.surfaces[b];
            shaders.emplace_back(klass, look, maps[b], [surface](int x, int y) { return surface->Texel(x, y); });
        }
        for (size_t w = 0; w < weapons.size(); ++w) {
            const FtxSurface* surface = &weapons[w];
            weapon_shaders.emplace_back(klass, look, w, [surface](int x, int y) { return surface->Texel(x, y); }, surface->width(),
                                        surface->height());
        }
        request.shade = [&](uint32_t, uint8_t texture, int x, int y, std::array<uint8_t, 4>& texel) {
            for (size_t b = 0; b < shaders.size(); ++b) {
                if (bodies[b].texture == texture) shaders[b].ApplyAt(x, y, texel);
            }
            for (size_t w = 0; w < weapon_shaders.size(); ++w) {
                if (weapon_textures[w].texture == texture) weapon_shaders[w].Apply(texel);
            }
        };
    }
    return RenderSprite(request);
}

// For every class, every Selected Part and every base colour: Hidden clears the part's own texels on the atlas the game
// is served and nothing else; the preview changes; Shown again and Reset Part give back the native preview.
void TestEveryPartHides(CharacterArt& art, CpkArchive& cpk) {
    size_t audited = 0;
    for (const CharacterClass klass : kCharacterClasses) {
        const std::vector<RegionMap>* maps = art.Regions(klass);
        CHECK(maps != nullptr);
        if (!maps) continue;
        const std::span<const WeaponTexture> weapons = WeaponTexturesOf(klass);
        for (int color = 0; color < kNativeColorCount; ++color) {
            const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, color);
            const SpriteImage native = *art.Preview(klass, color);
            for (const CustomPart& part : CustomPartsOf(klass)) {
                const std::string key(part.key);
                const CustomLook hidden = Hidden(color, {key.c_str()});
                if (part.weapon >= 0) {
                    // On its own texture: the part's texels lose their alpha, the band's or the rest's stay.
                    const std::vector<uint8_t> pack = cpk.Read(*cpk.Find(WeaponAtlasName(klass, weapons[size_t(part.weapon)])));
                    const FtxSurface surface = *ReadFtxSurface(pack, 0);
                    const WeaponShader shader(klass, hidden, size_t(part.weapon), [&](int x, int y) { return surface.Texel(x, y); },
                                              surface.width(), surface.height());
                    const bool split = WeaponAccentOf(klass, size_t(part.weapon)) != nullptr;
                    size_t mine = 0, cleared = 0, others_changed = 0;
                    for (int y = 0; y < surface.height(); y += 3) {
                        for (int x = 0; x < surface.width(); x += 3) {
                            const std::array<uint8_t, 4> before = surface.Texel(x, y);
                            if (!before[3]) continue;
                            const float band = split ? AccentWeight(before) : 0.0f;
                            std::array<uint8_t, 4> after = before;
                            shader.Apply(after);
                            if (band == (part.accent ? 1.0f : 0.0f)) {
                                ++mine;
                                cleared += after[3] == 0;
                            } else if (band == (part.accent ? 0.0f : 1.0f)) {
                                others_changed += after != before;
                            }
                        }
                    }
                    CHECK(mine > 100 && cleared == mine && others_changed == 0);
                    CHECK(ChangeOnAtlas(klass, *maps, atlas, hidden, {}).outside_changed == 0); // the body keeps every texel
                    // A strong colour moves the part's texels and leaves the rest of the texture and the body.
                    const CustomLook strong = Painted(color, {key.c_str()}, {255, 0, 128});
                    const WeaponShader tint(klass, strong, size_t(part.weapon), [&](int x, int y) { return surface.Texel(x, y); },
                                            surface.width(), surface.height());
                    size_t tinted = 0, others_tinted = 0;
                    for (int y = 0; y < surface.height(); y += 3) {
                        for (int x = 0; x < surface.width(); x += 3) {
                            const std::array<uint8_t, 4> before = surface.Texel(x, y);
                            if (!before[3]) continue;
                            const float band = split ? AccentWeight(before) : 0.0f;
                            std::array<uint8_t, 4> after = before;
                            tint.Apply(after);
                            if (band == (part.accent ? 1.0f : 0.0f)) tinted += after != before;
                            else if (band == (part.accent ? 0.0f : 1.0f)) others_tinted += after != before;
                        }
                    }
                    CHECK(tinted * 10 >= mine * 9 && others_tinted == 0);
                    CHECK(ChangeOnAtlas(klass, *maps, atlas, strong, {}).outside_changed == 0);
                } else {
                    // A part with a head under it is painted as that bare head instead; every other part goes transparent.
                    const AtlasChange change = ChangeOnAtlas(klass, *maps, atlas, hidden, {&part});
                    const size_t expected_cleared = part.bare_underlay.empty() ? change.inside : 0;
                    const bool repainted = klass == CharacterClass::kFighter && part.key == "helmet"; // TestFighterBareHead
                    CHECK(change.inside > 0 && (repainted || (change.inside_cleared == expected_cleared && change.outside_changed == 0)));
                    const AtlasChange tint = ChangeOnAtlas(klass, *maps, atlas, Painted(color, {key.c_str()}, {255, 0, 128}), {&part});
                    CHECK(tint.inside_changed * 10 >= tint.inside * 9 && tint.inside_cleared == 0 && tint.outside_changed == 0);
                }
                // The Fighter's face is drawn only under a lifted helmet, in poses the Appearance preview never shows.
                CHECK(part.key == "face" || art.CustomPreview(klass, color, hidden)->rgba != native.rgba);
                CustomLook shown = hidden;
                shown.SetPartHidden(key, false);
                CHECK(art.CustomPreview(klass, color, shown)->rgba == native.rgba);
                CustomLook reset = Painted(color, {key.c_str()}, {255, 0, 128});
                reset.SetPartHidden(key, true);
                reset.ResetPart(key);
                CHECK(reset.Find(key) == nullptr && art.CustomPreview(klass, color, reset)->rgba == native.rgba);
            }
        }
        audited += CustomPartsOf(klass).size();
    }
    std::printf("Every selectable part hides: %zu parts in %zu classes\n", audited, std::size(kCharacterClasses));
}

// Texels some preset draws, and their owner, in one helmet sprite of the Fighter's atlas.
struct HornSpriteOwners {
    int width = 0, height = 0;
    std::vector<uint8_t> drawn;
    std::vector<const CustomPart*> owner;
};
HornSpriteOwners OwnersOfSprite(const RegionMap& map, const std::vector<FtxSurface>& presets, const HornSprite& sprite) {
    HornSpriteOwners owners;
    owners.width = sprite.x1 - sprite.x0;
    owners.height = sprite.y1 - sprite.y0;
    owners.drawn.assign(size_t(owners.width) * size_t(owners.height), 0);
    owners.owner.assign(owners.drawn.size(), nullptr);
    for (int y = 0; y < owners.height; ++y) {
        for (int x = 0; x < owners.width; ++x) {
            const size_t at = size_t(y) * size_t(owners.width) + size_t(x);
            for (const FtxSurface& preset : presets) owners.drawn[at] |= preset.Texel(sprite.x0 + x, sprite.y0 + y)[3] != 0;
            owners.owner[at] = OwnerOfTexel(CharacterClass::kFighter, map, sprite.x0 + x, sprite.y0 + y);
        }
    }
    return owners;
}

// 4-connected patches of drawn texels whose owner `in` accepts: their size, whether one holds a seed, and whether one
// touches a drawn texel `in` refuses.
struct Patch {
    size_t size = 0;
    bool seeded = false;
    bool touches_other = false;
};
template <typename In>
std::vector<Patch> Patches(const HornSpriteOwners& owners, In in, std::span<const HornCut> seeds) {
    std::vector<Patch> patches;
    std::vector<uint8_t> seen(owners.drawn.size(), 0);
    for (size_t start = 0; start < seen.size(); ++start) {
        if (seen[start] || !owners.drawn[start] || !in(owners.owner[start])) continue;
        std::vector<size_t> stack{start};
        seen[start] = 1;
        Patch patch;
        while (!stack.empty()) {
            const size_t at = stack.back();
            stack.pop_back();
            ++patch.size;
            const int x = int(at % size_t(owners.width)), y = int(at / size_t(owners.width));
            for (const HornCut& cut : seeds) patch.seeded |= cut.seed.x == x && cut.seed.y == y;
            const int sx[4] = {1, -1, 0, 0}, sy[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; ++d) {
                const int nx = x + sx[d], ny = y + sy[d];
                if (nx < 0 || ny < 0 || nx >= owners.width || ny >= owners.height) continue;
                const size_t next = size_t(ny) * size_t(owners.width) + size_t(nx);
                if (!owners.drawn[next]) continue;
                if (!in(owners.owner[next])) {
                    patch.touches_other = true;
                    continue;
                }
                if (seen[next]) continue;
                seen[next] = 1;
                stack.push_back(next);
            }
        }
        patches.push_back(patch);
    }
    return patches;
}

// Texels inside each helmet sprite that are the helmet's, read from the art: the dark back and neck guard, the visor,
// the inside of a fallen helmet and the sockets the horns sit in. Sprite rectangles as in fighter_horns.cpp.
struct HelmetAnchor {
    int x0, y0;
    std::vector<std::pair<int, int>> points;
};
const std::vector<HelmetAnchor>& HelmetAnchors() {
    static const std::vector<HelmetAnchor> anchors = {
        {372, 1856, {{95, 150}, {170, 150}, {132, 60}, {84, 128}}},     // #0
        {756, 1436, {{190, 80}, {140, 120}, {126, 92}}},                 // #2
        {2040, 288, {{95, 150}, {170, 150}, {132, 60}}},                 // #4
        {2268, 1668, {{60, 140}, {110, 60}, {50, 125}}},                 // #5
        {2436, 1412, {{60, 140}, {110, 60}, {50, 125}}},                 // #6
        {2464, 1140, {{95, 150}, {170, 150}, {132, 60}}},                // #7
        {2720, 0, {{30, 140}, {50, 170}, {100, 120}, {20, 120}}},        // #8
        {2764, 844, {{30, 140}, {50, 170}, {100, 120}, {20, 120}, {40, 116}}}, // #9
        {2924, 1868, {{150, 30}, {150, 100}, {112, 100}}},               // #10
        {2952, 256, {{130, 170}, {120, 110}}},                           // #11
        {3108, 1412, {{65, 125}, {110, 170}}},                           // #12
        {3352, 1140, {{60, 140}, {90, 60}, {56, 105}}},                  // #15
        {3496, 1608, {{50, 70}, {100, 90}}},                             // #16
        {3560, 844, {{75, 90}, {110, 60}}},                              // #17
        {3566, 256, {{80, 40}, {60, 130}}},                              // #18
        {3664, 0, {{40, 140}, {100, 80}, {40, 118}}},                    // #19
        {3844, 1608, {{90, 90}, {50, 120}}},                             // #20
    };
    return anchors;
}

// Helmet and Horns in one helmet sprite share no texel: every drawn texel is the horns', the helmet's or the eye's; each
// horn is one patch holding its seed; nothing of the helmet is left as a fragment; the helmet's own texels are never horn.
void CheckHornOwnership(const std::string& build, const RegionMap& map, const std::vector<FtxSurface>& presets) {
    const CustomPart* horns = FindCustomPart(CharacterClass::kFighter, "horns");
    const CustomPart* helmet = FindCustomPart(CharacterClass::kFighter, "helmet");
    const CustomPart* eye = FindCustomPart(CharacterClass::kFighter, "eye");
    const CustomPart* face = FindCustomPart(CharacterClass::kFighter, "face");
    const std::span<const HornSprite> sprites = FighterHornSprites();
    CHECK(sprites.size() == 17 && HelmetAnchors().size() == sprites.size());
    size_t skin_wrong = 0, horn_texels = 0, horn_patches = 0, horn_cuts = 0, foreign = 0, fragments = 0, stray = 0, anchors_wrong = 0;
    for (const HornSprite& sprite : sprites) {
        const HornSpriteOwners owners = OwnersOfSprite(map, presets, sprite);
        for (size_t at = 0; at < owners.drawn.size(); ++at) {
            if (!owners.drawn[at]) continue;
            foreign += owners.owner[at] != horns && owners.owner[at] != helmet && owners.owner[at] != eye && owners.owner[at] != face;
            horn_texels += owners.owner[at] == horns;
            const int x = sprite.x0 + int(at % size_t(owners.width)), y = sprite.y0 + int(at / size_t(owners.width));
            CHECK(std::popcount(map.Pieces(x, y)) <= 1);
        }
        for (const Patch& patch : Patches(owners, [&](const CustomPart* p) { return p == horns; }, sprite.horns)) {
            ++horn_patches;
            stray += !patch.seeded;
        }
        horn_cuts += sprite.horns.size();
        // A speck of the art's soft edge stands alone; one the cut made would lie against a horn.
        for (const Patch& patch : Patches(owners, [&](const CustomPart* p) { return p != horns; }, {})) {
            fragments += patch.size < 200 && patch.touches_other;
        }
        for (const HornCut& cut : sprite.horns) {
            CHECK(owners.owner[size_t(cut.seed.y) * size_t(owners.width) + size_t(cut.seed.x)] == horns);
        }
        // The skin seen through the visor, read from the art: nose, cheek and chin of the front helmets.
        static const std::vector<HelmetAnchor> skin = {{372, 1856, {{138, 113}, {136, 125}}}, {2040, 288, {{138, 113}, {136, 125}}},
                                                       {2464, 1140, {{138, 113}, {136, 125}}}, {2764, 844, {{100, 137}, {102, 147}}}};
        for (const HelmetAnchor& anchor : skin) {
            if (anchor.x0 != sprite.x0 || anchor.y0 != sprite.y0) continue;
            for (const auto& [x, y] : anchor.points) skin_wrong += owners.owner[size_t(y) * size_t(owners.width) + size_t(x)] != face;
        }
        for (const HelmetAnchor& anchor : HelmetAnchors()) {
            if (anchor.x0 != sprite.x0 || anchor.y0 != sprite.y0) continue;
            for (const auto& [x, y] : anchor.points) {
                const size_t at = size_t(y) * size_t(owners.width) + size_t(x);
                anchors_wrong += !owners.drawn[at] || owners.owner[at] != helmet;
            }
        }
    }
    std::printf("%s: horns %zu texels in %zu patches, foreign %zu, stray horn patches %zu, helmet fragments %zu, anchors wrong %zu, skin anchors wrong %zu\n",
                build.c_str(), horn_texels, horn_patches, foreign, stray, fragments, anchors_wrong, skin_wrong);
    CHECK(horn_texels > 60000 && horn_patches == horn_cuts && horn_cuts == 34 && foreign == 0 && stray == 0 && fragments == 0 && anchors_wrong == 0 && skin_wrong == 0);
}

size_t HelmetRuleProblems(const Mbs& model);

// The Helmet and Horns of every local build with the Fighter's art: atlases and models are compared by content, and each
// build learns its own ownership. Builds without local files are named.
void TestFighterHornBuilds(const fs::path& base, const fs::path& source_dir) {
    const fs::path prepared = source_dir / "game" / "prepared";
    const fs::path us101 = source_dir / "build" / "research" / "archaeology" / "data" / "RU101" / "DRACRO_US.CPK";
    struct LocalBuild {
        const char* name;
        std::vector<fs::path> archives;
    };
    const std::vector<LocalBuild> builds = {
        {"CUSA10454 1.00 USA", {prepared / "v1.03" / "DRACRO_US.CPK", prepared / "v1.03" / "DRACRO.CPK"}},
        {"CUSA10454 1.01 USA", {us101, prepared / "v1.03" / "DRACRO.CPK"}},
        {"CUSA07001 1.00 Japan", {prepared / "CUSA07001-1.00" / "DRACRO.CPK"}},
        {"CUSA07001 1.02 Japan", {prepared / "CUSA07001-1.02" / "DRACRO_PATCH_JP.CPK", prepared / "CUSA07001-1.02" / "DRACRO.CPK"}},
        {"CUSA07001 1.03 Japan", {prepared / "CUSA07001-1.03" / "DRACRO_PATCH_JP.CPK", prepared / "CUSA07001-1.03" / "DRACRO.CPK"}},
        {"CUSA10209 1.00 Asia/Hong Kong", {prepared / "CUSA10209-1.00" / "DRACRO_CK.CPK", prepared / "CUSA10209-1.00" / "DRACRO.CPK"}},
        {"CUSA10209 1.02 Asia/Hong Kong", {prepared / "CUSA10209-1.02" / "DRACRO_PATCH_CK.CPK", prepared / "CUSA10209-1.02" / "DRACRO.CPK"}},
    };
    const CharacterClass klass = CharacterClass::kFighter;
    std::vector<std::string> entries = {GameModelName(klass), "PLAYER/FIGHTER00_SELECT.MBS"};
    for (int color = 0; color < kNativeColorCount; ++color) entries.push_back(AtlasName(klass, color));
    std::optional<std::vector<std::vector<uint8_t>>> reference;
    size_t checked = 0;
    for (const LocalBuild& build : builds) {
        std::error_code ec;
        const bool present = std::all_of(build.archives.begin(), build.archives.end(),
                                         [&](const fs::path& archive) { return fs::is_regular_file(archive, ec); });
        if (!present) {
            std::printf("%s: no local files, Helmet/Horns not validated on it\n", build.name);
            continue;
        }
        CharacterArtSources sources;
        sources.archives = build.archives;
        sources.cache_dir = base / "horn_builds" / std::to_string(checked);
        fs::remove_all(sources.cache_dir);
        std::vector<std::unique_ptr<CpkArchive>> open;
        for (const fs::path& archive : build.archives) {
            open.push_back(std::make_unique<CpkArchive>());
            CHECK(open.back()->Open(archive));
        }
        std::vector<std::vector<uint8_t>> contents;
        for (const std::string& entry : entries) {
            std::vector<uint8_t> bytes;
            for (const auto& archive : open) {
                if (const auto found = archive->Find(entry)) {
                    bytes = archive->Read(*found);
                    break;
                }
            }
            CHECK(!bytes.empty());
            contents.push_back(std::move(bytes));
        }
        if (!reference) reference = contents;
        std::printf("%s: Fighter model and atlases %s the first build's\n", build.name, contents == *reference ? "identical to" : "DIFFER from");
        CharacterArt art(sources);
        const std::vector<RegionMap>* maps = art.Regions(klass);
        CHECK(maps != nullptr);
        if (!maps) continue;
        std::vector<FtxSurface> presets;
        for (int color = 0; color < kNativeColorCount; ++color) presets.push_back(*ReadFtxSurface(contents[size_t(2 + color)], 0));
        CheckHornOwnership(build.name, maps->front(), presets);
        const std::optional<Mbs> model = ParseMbs(contents.front());
        const size_t rule_problems = model ? HelmetRuleProblems(*model) : 1;
        std::printf("%s: helmet sprite rules %s\n", build.name, rule_problems ? "DO NOT match the model" : "match the model");
        CHECK(rule_problems == 0);
        const std::optional<std::vector<uint8_t>> with_copies = FighterModelWithHelmetCopies(contents.front());
        size_t copied_atlases = 0;
        for (int color = 0; color < kNativeColorCount; ++color) {
            std::vector<uint8_t> pack = contents[size_t(2 + color)];
            copied_atlases += AddHelmetCopies(pack);
        }
        const size_t off_head = model ? OffHeadHelmetLayers(*model).size() : 0;
        std::printf("%s: helmet copies %s, %zu off-head helmet records, %zu atlases copied\n", build.name, with_copies ? "made" : "NOT made",
                    off_head, copied_atlases);
        CHECK(with_copies && off_head == 65 && copied_atlases == size_t(kNativeColorCount));
        ++checked;
    }
    CHECK(checked >= 6);
}

// Helmet and Horns through the preview and the served atlas, in every Base Color: hiding, colouring, Random and Reset
// Part of one touch nothing of the other, the eye or the rest of the knight.
void TestFighterHelmetAndHorns(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kFighter;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(select && art.Regions(klass));
    if (!select || !art.Regions(klass)) return;
    const std::vector<RegionMap>& maps = *art.Regions(klass);
    const CustomPart* horns = FindCustomPart(klass, "horns");
    const CustomPart* helmet = FindCustomPart(klass, "helmet");
    const CustomPart* face = FindCustomPart(klass, "face");
    const auto owner_at = [&](const SpriteImage& image, size_t p) -> const CustomPart* {
        if (image.layer[p] == kNoLayer || image.texture[p] != 0) return nullptr;
        const RegionMap& map = maps.front();
        return OwnerOfTexel(klass, map, int(image.texel[p] % uint32_t(map.width)), int(image.texel[p] / uint32_t(map.width)));
    };
    // The preview filters like the game: a pixel blends the 2x2 texels around its sample point, so a pixel next to a part
    // is reached by it even when its nearest texel is another part's.
    const auto reaches = [&](const SpriteImage& image, size_t p, const CustomPart* part) {
        if (image.layer[p] == kNoLayer || image.texture[p] != 0) return false;
        const RegionMap& map = maps.front();
        const int x = int(image.texel[p] % uint32_t(map.width)), y = int(image.texel[p] / uint32_t(map.width));
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (OwnerOfTexel(klass, map, x + dx, y + dy) == part) return true;
            }
        }
        return false;
    };
    // Every drawn texel the pixel's filter reads is the part's: hidden, such a pixel has nothing left to show.
    const auto samples_only = [&](const SpriteImage& image, size_t p, const CustomPart* part) {
        if (image.layer[p] == kNoLayer || image.texture[p] != 0) return false;
        const RegionMap& map = maps.front();
        const int x = int(image.texel[p] % uint32_t(map.width)), y = int(image.texel[p] / uint32_t(map.width));
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const CustomPart* owner = OwnerOfTexel(klass, map, x + dx, y + dy);
                if (owner && owner != part) return false;
            }
        }
        return true;
    };
    std::mt19937 random(11);
    for (int color = 0; color < kNativeColorCount; ++color) {
        const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, color);
        const SpriteImage native = *art.Preview(klass, color);
        size_t horn_pixels = 0, helmet_pixels = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            if (native.rgba[p * 4 + 3] == 0) continue;
            horn_pixels += owner_at(native, p) == horns;
            helmet_pixels += owner_at(native, p) == helmet;
        }
        CHECK(horn_pixels > 1500 && helmet_pixels > 1500);
        // Pixels the part reaches, widened by the filter's reach in the image (its soft edge over other layers or nothing).
        const auto footprint = [&](const SpriteImage& a, const SpriteImage& b, const CustomPart* part) {
            std::vector<uint8_t> mask(a.layer.size(), 0);
            for (size_t p = 0; p < a.layer.size(); ++p) {
                if (!reaches(a, p, part) && !reaches(b, p, part)) continue;
                const int x = int(p % size_t(a.width)), y = int(p / size_t(a.width));
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (x + dx >= 0 && y + dy >= 0 && x + dx < a.width && y + dy < a.height) mask[size_t(y + dy) * size_t(a.width) + size_t(x + dx)] = 1;
                    }
                }
            }
            return mask;
        };
        const auto changed = [&](const SpriteImage& after) {
            std::vector<uint8_t> mask(after.layer.size(), 0);
            // Compared premultiplied: a pixel the filter leaves all but transparent shows no colour. A part seen through an all
            // but opaque layer over it moves the pixel by a level or two of rounding.
            for (size_t p = 0; p < after.layer.size(); ++p) {
                int delta = std::abs(int(native.rgba[p * 4 + 3]) - int(after.rgba[p * 4 + 3]));
                for (int c = 0; c < 3; ++c) {
                    delta = std::max(delta, std::abs(int(native.rgba[p * 4 + c]) * native.rgba[p * 4 + 3] - int(after.rgba[p * 4 + c]) * after.rgba[p * 4 + 3]) / 255);
                }
                mask[p] = delta > 2;
            }
            return mask;
        };
        for (const CustomPart* part : {horns, helmet, face}) {
            const std::string key(part->key);
            const SpriteImage hidden = *art.CustomPreview(klass, color, Hidden(color, {key.c_str()}));
            const SpriteImage painted = *art.CustomPreview(klass, color, Painted(color, {key.c_str()}, {230, 40, 200}));
            CustomLook rolled{true, uint8_t(color), {}};
            rolled.SetPartColor(key, NextRandomColor(random, std::nullopt));
            const SpriteImage randomized = *art.CustomPreview(klass, color, rolled);
            const std::vector<uint8_t> by_hide = changed(hidden), by_paint = changed(painted), by_random = changed(randomized);
            const std::vector<uint8_t> near_part = footprint(native, hidden, part);
            // A pixel the part does not draw, even through a translucent layer over it, changes under none of its looks: what
            // one look changes, hiding changes too, but for the part's own filtered edge.
            size_t own = 0, own_hidden = 0, own_painted = 0, left = 0, hide_only = 0, paint_only = 0, random_only = 0;
            for (size_t p = 0; p < native.layer.size(); ++p) {
                if (owner_at(native, p) == part && native.rgba[p * 4 + 3] != 0) {
                    ++own;
                    own_hidden += by_hide[p];
                    own_painted += by_paint[p];
                }
                left += hidden.rgba[p * 4 + 3] != 0 && samples_only(hidden, p, part);
                if (near_part[p]) continue;
                hide_only += by_hide[p] && !by_paint[p];
                paint_only += by_paint[p] && !by_hide[p];
                random_only += by_random[p] && !by_hide[p];
            }
            std::printf("Fighter %s colour %d: %zu pixels, hidden %zu, painted %zu; hidden leaves %zu; outside the part: hide %zu, paint %zu, random %zu\n",
                        key.c_str(), color, own, own_hidden, own_painted, left, hide_only, paint_only, random_only);
            // The hidden helmet's sprites show the bare head instead, which TestFighterBareHead checks.
            const bool repainted = part == helmet;
            CHECK(own > (part == face ? 40u : 1500u) && own_painted * 100 >= own * 90 && (repainted || (left == 0 && hide_only == 0)) &&
                  paint_only == 0 && random_only == 0);
            for (const CustomLook& look : {Hidden(color, {key.c_str()}), Painted(color, {key.c_str()}, {230, 40, 200}), rolled}) {
                if (repainted && look.parts.front().hidden) continue;
                const AtlasChange change = ChangeOnAtlas(klass, maps, atlas, look, {part});
                CHECK(change.inside > 0 && change.inside_changed * 100 >= change.inside * 90 && change.outside_changed == 0);
            }
            CustomLook reset = Hidden(color, {key.c_str()});
            reset.SetPartColor(key, {1, 2, 3});
            reset.ResetPart(key);
            CHECK(art.CustomPreview(klass, color, reset)->rgba == native.rgba);
            CHECK(ChangeOnAtlas(klass, maps, atlas, reset, {}).outside_changed == 0);
        }
        // On the atlas, the face hidden changes no texel of the helmet's, nor the horns hidden any of the helmet's or eye's.
        for (const char* key : {"face", "horns"}) {
            const AtlasChange change = ChangeOnAtlas(klass, maps, atlas, Hidden(color, {key}), {FindCustomPart(klass, key)});
            CHECK(change.inside > 0 && change.inside_cleared == change.inside && change.outside_changed == 0);
        }
        // The horns hidden leave the eye; the helmet hidden repaints its visor with the bare head.
        const SpriteImage hornless = *art.CustomPreview(klass, color, Hidden(color, {"horns"}));
        size_t kept_wrong = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            const CustomPart* owner = owner_at(native, p);
            if (owner && owner->key == "eye" && !reaches(native, p, horns) && !reaches(native, p, helmet)) {
                kept_wrong += std::memcmp(&native.rgba[p * 4], &hornless.rgba[p * 4], 4) != 0;
            }
        }
        CHECK(kept_wrong == 0);
    }
    for (const char* key : {"horns", "helmet"}) {
        CheckServedLook(art, cpk, klass, *select, Hidden(0, {key}));
        CheckServedLook(art, cpk, klass, *select, Painted(3, {key}, {40, 220, 90}));
    }
}

AtlasRect SourceRectOf(const Mbs& model, const MbsLayer& layer, int width, int height) {
    const MbsQuad& quad = model.sources[layer.source];
    AtlasRect rect{1 << 30, 1 << 30, -(1 << 30), -(1 << 30)};
    for (const MbsPoint& c : quad.corners) {
        rect.x0 = std::min(rect.x0, int(std::lround(c.x * float(width))));
        rect.x1 = std::max(rect.x1, int(std::lround(c.x * float(width))));
        rect.y0 = std::min(rect.y0, int(std::lround(c.y * float(height))));
        rect.y1 = std::max(rect.y1, int(std::lround(c.y * float(height))));
    }
    return rect;
}

// The helmet sprite table against a model: every helmet sprite has a rule over its exact atlas rectangle, every bare-head
// layer is a bare-head sprite of the model, and no other sprite shares a rectangle the rules repaint or clear.
size_t HelmetRuleProblems(const Mbs& model) {
    constexpr int kAtlasSize = 4096;
    std::map<uint32_t, std::set<std::tuple<int, int, int, int>>> rects;
    for (const MbsLayer& layer : model.layers) {
        if (layer.texture != 0) continue;
        const AtlasRect r = SourceRectOf(model, layer, kAtlasSize, kAtlasSize);
        rects[layer.id].insert({r.x0, r.y0, r.x1, r.y1});
    }
    const std::span<const HelmetSpriteRule> rules = FighterHelmetSprites();
    const auto rule_of = [&](uint32_t id) {
        return std::find_if(rules.begin(), rules.end(), [&](const HelmetSpriteRule& r) { return r.id == id; });
    };
    size_t problems = 0;
    for (const auto& [id, shapes] : rects) {
        if (id < 1100 || id >= 1200) continue;
        problems += rule_of(id) == rules.end();
    }
    for (const HelmetSpriteRule& rule : rules) {
        const auto found = rects.find(rule.id);
        problems += found == rects.end() || found->second.size() != 1 ||
                    *found->second.begin() != std::tuple{rule.rect.x0, rule.rect.y0, rule.rect.x1, rule.rect.y1};
        problems += (rule.kind == HelmetSprite::kBareHead) == rule.layers.empty();
        for (const BareHeadLayer& layer : rule.layers) {
            bool bare_sprite = false;
            for (const auto& [id, shapes] : rects) {
                bare_sprite |= id >= 1200 && id < 1300 && shapes.count({layer.source.x0, layer.source.y0, layer.source.x1, layer.source.y1});
            }
            problems += !bare_sprite;
        }
        for (const auto& [id, shapes] : rects) {
            if (id == rule.id) continue;
            for (const auto& [x0, y0, x1, y1] : shapes) problems += x0 < rule.rect.x1 && rule.rect.x0 < x1 && y0 < rule.rect.y1 && rule.rect.y0 < y1;
        }
    }
    return problems;
}

// The frame each track of an animation draws at every tick: tracks run on their own clocks.
std::vector<std::vector<std::pair<size_t, size_t>>> AnimationTicks(const Mbs& model, size_t animation) {
    const MbsAnimation& anim = model.animations[animation];
    long length = 0;
    for (size_t t = 0; t < anim.track_count; ++t) {
        const MbsTrack& run = model.tracks[anim.first_track + t];
        long sum = 0;
        for (size_t f = 0; f < run.frame_count; ++f) sum += model.frames[run.first_frame + f].duration;
        length = std::max(length, sum);
    }
    std::vector<std::vector<std::pair<size_t, size_t>>> ticks;
    for (long tick = 0; tick < length; ++tick) {
        std::vector<std::pair<size_t, size_t>> frames;
        for (size_t t = 0; t < anim.track_count; ++t) {
            const MbsTrack& run = model.tracks[anim.first_track + t];
            if (!run.frame_count) continue;
            long end = 0;
            size_t f = 0;
            while (f + 1 < run.frame_count && tick >= (end += model.frames[run.first_frame + f].duration)) ++f;
            frames.emplace_back(t, f);
        }
        ticks.push_back(std::move(frames));
    }
    return ticks;
}

// One tick of an animation: every track at its own frame, drawn in track order.
SpriteImage RenderTick(const Mbs& model, size_t animation, const std::vector<std::pair<size_t, size_t>>& frames,
                       const std::array<const FtxSurface*, 16>& textures, float scale) {
    SpriteImage out;
    std::vector<float> color;
    for (const auto& [t, f] : frames) {
        const MbsTrack& run = model.tracks[model.animations[animation].first_track + t];
        const MbsKey& key = model.keys[model.frames[run.first_frame + f].key];
        SpriteRequest request;
        request.mbs = &model;
        request.animation = animation;
        request.frame = f;
        request.scale = scale;
        request.textures = textures;
        request.visible = [&](uint32_t layer) { return layer >= key.first_layer && layer < key.first_layer + key.layer_count; };
        const SpriteImage image = RenderSprite(request);
        if (out.rgba.empty()) {
            out = image;
            std::fill(out.layer.begin(), out.layer.end(), kNoLayer);
            color.assign(image.rgba.size(), 0.0f);
        }
        for (size_t p = 0; p < image.layer.size(); ++p) {
            const float alpha = image.rgba[p * 4 + 3] / 255.0f;
            for (size_t c = 0; c < 3; ++c) color[p * 4 + c] = image.rgba[p * 4 + c] / 255.0f * alpha + color[p * 4 + c] * (1.0f - alpha);
            color[p * 4 + 3] = alpha + color[p * 4 + 3] * (1.0f - alpha);
            if (image.layer[p] != kNoLayer) out.layer[p] = image.layer[p];
        }
    }
    for (size_t p = 0; p < out.layer.size(); ++p) {
        const float alpha = std::clamp(color[p * 4 + 3], 0.0f, 1.0f);
        out.rgba[p * 4 + 3] = uint8_t(std::lround(alpha * 255.0f));
        for (size_t c = 0; c < 3; ++c) {
            out.rgba[p * 4 + c] = uint8_t(alpha > 0 ? std::lround(std::clamp(color[p * 4 + c] / alpha, 0.0f, 1.0f) * 255.0f) : 0);
        }
    }
    return out;
}

// Every tick of every animation with the helmet hidden, by what the drawn helmet records sample in the hidden atlas.
struct HelmetTicks {
    long total = 0, official_head = 0, game_head = 0, no_head = 0, off_head_hidden = 0, visor_cleared = 0, helmet_visible = 0,
         duplicate = 0, conflicts = 0;
};
HelmetTicks CountHelmetTicks(const Mbs& model, const std::set<uint32_t>& off_head, const FtxSurface& hidden) {
    std::map<uint16_t, bool> shows;
    for (const MbsLayer& layer : model.layers) {
        if (layer.id < 1100 || layer.id >= 1200 || shows.count(layer.source)) continue;
        const AtlasRect r = SourceRectOf(model, layer, hidden.width(), hidden.height());
        bool any = false;
        for (int y = r.y0; y < r.y1 && !any; ++y) {
            for (int x = r.x0; x < r.x1 && !any; ++x) any = hidden.Texel(x, y)[3] != 0;
        }
        shows[layer.source] = any;
    }
    std::map<uint32_t, HelmetSprite> kind;
    for (const HelmetSpriteRule& rule : FighterHelmetSprites()) kind[rule.id] = rule.kind;
    HelmetTicks ticks;
    std::set<uint32_t> on_head;
    for (size_t a = 0; a < model.animations.size(); ++a) {
        for (const auto& frames : AnimationTicks(model, a)) {
            ++ticks.total;
            std::vector<const MbsKey*> keys;
            for (const auto& [t, f] : frames) keys.push_back(&model.keys[model.frames[model.tracks[model.animations[a].first_track + t].first_frame + f].key]);
            bool bare = false, off = false, visible = false, cleared = false, two_heads = false, any_head = false;
            for (const MbsKey* key : keys) {
                for (uint32_t i = 0; i < key->layer_count; ++i) {
                    const uint32_t id = model.layers[key->first_layer + i].id;
                    bare |= id >= 1200 && id < 1300;
                }
            }
            for (const MbsKey* key : keys) {
                std::set<uint32_t> heads;
                bool body = false;
                for (uint32_t i = 0; i < key->layer_count; ++i) {
                    const uint32_t id = model.layers[key->first_layer + i].id;
                    body |= id < 1100 || (id >= 1300 && id < 3000);
                }
                for (uint32_t i = 0; i < key->layer_count; ++i) {
                    const uint32_t layer = key->first_layer + i;
                    const MbsLayer& record = model.layers[layer];
                    if (record.id < 1100 || record.id >= 1200) continue;
                    const bool sampled = shows[record.source];
                    if (body && !bare) on_head.insert(layer);
                    if (off_head.count(layer)) {
                        off = true;
                        visible |= sampled;
                    } else if (kind[record.id] == HelmetSprite::kBareHead) {
                        if (sampled) heads.insert(record.id);
                    } else {
                        cleared = true;
                        visible |= sampled;
                    }
                }
                two_heads |= heads.size() > 1; // one body, one head; a second body drawn by another track has its own
                any_head |= !heads.empty();
            }
            ticks.official_head += !bare && any_head;
            ticks.game_head += bare;
            ticks.no_head += !bare && !any_head;
            ticks.off_head_hidden += off;
            ticks.visor_cleared += cleared;
            ticks.helmet_visible += visible;
            ticks.duplicate += (bare && any_head) || two_heads;
        }
    }
    for (const uint32_t layer : off_head) ticks.conflicts += on_head.count(layer);
    return ticks;
}

// Helmet Hidden shows the Fighter's own bare head: the helmet sprites he wears are repainted with the game's bare-head
// sprites of the same Base Color, under the horns and through the Face's own look, in the preview, the served atlas and
// the gameplay poses alike. Visor pieces and helmets off the head go, nothing else of the atlas changes.
void TestFighterBareHead(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kFighter;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    const std::optional<Mbs> game = ParseMbs(cpk.Read(*cpk.Find(GameModelName(klass))));
    CHECK(select && game && art.Regions(klass));
    if (!select || !game || !art.Regions(klass)) return;
    const std::vector<RegionMap>& maps = *art.Regions(klass);
    const RegionMap& map = maps.front();
    const CustomPart* helmet = FindCustomPart(klass, "helmet");
    const CustomPart* horns = FindCustomPart(klass, "horns");
    const std::span<const HelmetSpriteRule> rules = FighterHelmetSprites();
    CHECK(HelmetRuleProblems(*game) == 0);
    const auto rule_of = [&](uint32_t id) -> const HelmetSpriteRule* {
        for (const HelmetSpriteRule& rule : rules) {
            if (rule.id == id) return &rule;
        }
        return nullptr;
    };
    const auto kinds = [&](HelmetSprite kind) {
        std::set<uint32_t> ids;
        for (const HelmetSpriteRule& rule : rules) {
            if (rule.kind == kind) ids.insert(rule.id);
        }
        return ids;
    };
    CHECK((kinds(HelmetSprite::kBareHead) == std::set<uint32_t>{1100, 1101, 1102, 1103, 1105, 1107, 1110, 1113, 1115, 1120, 1122, 1127}));
    CHECK((kinds(HelmetSprite::kClear) == std::set<uint32_t>{1104, 1106, 1111, 1116, 1117, 1118, 1130, 1132, 1134, 1135}));

    const auto is_horn = [&](int x, int y) { return OwnerOfTexel(klass, map, x, y) == horns; };
    std::array<std::array<double, 3>, kNativeColorCount> head_means{};
    for (int color = 0; color < kNativeColorCount; ++color) {
        const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, color);
        const FtxSurface& surface = atlas.surfaces.front();
        const auto read = [&](int x, int y) { return surface.Texel(x, y); };
        const LookShader bare(klass, Hidden(color, {"helmet"}), map, read);
        const LookShader bald(klass, Hidden(color, {"helmet", "horns"}), map, read);
        CustomLook dyed = Hidden(color, {"helmet"});
        dyed.SetPartColor("face", {40, 220, 90});
        dyed.SetPartColor("helmet", {230, 20, 20});
        const LookShader green(klass, dyed, map, read);
        size_t head = 0, green_head = 0, red_head = 0, metal_left = 0, horns_drawn = 0, horns_kept = 0, horns_gone = 0, cleared_wrong = 0,
               outside_changed = 0;
        double sum[3] = {}, weight = 0.0;
        for (const HelmetSpriteRule& rule : rules) {
            for (int y = rule.rect.y0; y < rule.rect.y1; ++y) {
                for (int x = rule.rect.x0; x < rule.rect.x1; ++x) {
                    const std::array<uint8_t, 4> before = surface.Texel(x, y);
                    std::array<uint8_t, 4> shown = before, bald_texel = before, dyed_texel = before;
                    bare.ApplyAt(x, y, shown);
                    if (rule.kind == HelmetSprite::kClear) {
                        cleared_wrong += shown[3] != 0;
                        continue;
                    }
                    bald.ApplyAt(x, y, bald_texel);
                    green.ApplyAt(x, y, dyed_texel);
                    const bool horn = is_horn(x, y);
                    if (horn && before[3] == 255) {
                        ++horns_drawn;
                        horns_kept += shown == before;
                        horns_gone += bald_texel != before;
                    }
                    // Where the head is opaque its outline can match the metal's colour exactly; elsewhere the metal must be gone.
                    metal_left += !horn && before[3] == 255 && bald_texel[3] != 255 && OwnerOfTexel(klass, map, x, y) == helmet && shown == before;
                    if (horn || bald_texel[3] < 200) continue;
                    ++head;
                    green_head += dyed_texel[1] > dyed_texel[0] + 30 && dyed_texel[1] > dyed_texel[2] + 30;
                    red_head += dyed_texel[0] > dyed_texel[1] + 60 && dyed_texel[0] > dyed_texel[2] + 60;
                    if (rule.id == 1100) {
                        for (int c = 0; c < 3; ++c) sum[c] += bald_texel[size_t(c)];
                        weight += 1.0;
                    }
                }
            }
        }
        for (int y = 1; y < map.height; y += 3) {
            for (int x = 1; x < map.width; x += 3) {
                if (std::any_of(rules.begin(), rules.end(), [&](const HelmetSpriteRule& r) { return r.rect.Contains(x, y); })) continue;
                const std::array<uint8_t, 4> before = surface.Texel(x, y);
                std::array<uint8_t, 4> after = before;
                bare.ApplyAt(x, y, after);
                // The helmet's own texels next to its sprites, which filtering reaches, are cleared as before.
                outside_changed += after != before && OwnerOfTexel(klass, map, x, y) != helmet;
            }
        }
        for (int c = 0; c < 3; ++c) head_means[size_t(color)][size_t(c)] = weight > 0.0 ? sum[c] / weight : 0.0;
        // The colour of the game's own bare head in this Base Color, read from its face sprite.
        double face_sum[3] = {}, face_weight = 0.0;
        for (const BareHeadLayer& layer : rule_of(1100)->layers) {
            for (int y = layer.source.y0; y < layer.source.y1; ++y) {
                for (int x = layer.source.x0; x < layer.source.x1; ++x) {
                    const std::array<uint8_t, 4> t = surface.Texel(x, y);
                    if (t[3] < 200) continue;
                    for (int c = 0; c < 3; ++c) face_sum[c] += t[size_t(c)];
                    face_weight += 1.0;
                }
            }
        }
        double off = 0.0;
        for (int c = 0; c < 3; ++c) off = std::max(off, std::abs(face_sum[c] / face_weight - head_means[size_t(color)][size_t(c)]));
        std::printf("Fighter bare head colour %d: %zu head texels (green with Face dyed %zu, red %zu), metal left %zu, horns %zu kept %zu gone when "
                    "hidden %zu, cleared wrong %zu, outside changed %zu, off the game's bare head by %.1f\n",
                    color, head, green_head, red_head, metal_left, horns_drawn, horns_kept, horns_gone, cleared_wrong, outside_changed, off);
        CHECK(head > 50000 && green_head * 100 >= head * 95 && red_head == 0 && metal_left == 0);
        CHECK(horns_drawn > 50000 && horns_kept == horns_drawn && horns_gone == horns_drawn);
        CHECK(cleared_wrong == 0 && outside_changed == 0 && off < 30.0);

        // The preview's select pose wears helmet 1100: hidden, the head shows where it was; shown, the native preview is back.
        const SpriteImage native = *art.Preview(klass, color);
        const SpriteImage hidden = *art.CustomPreview(klass, color, Hidden(color, {"helmet"}));
        CustomLook shown_again = Hidden(color, {"helmet"});
        shown_again.SetPartHidden("helmet", false);
        CHECK(art.CustomPreview(klass, color, shown_again)->rgba == native.rgba);
        size_t helmet_pixels = 0, head_pixels = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            const bool on_1100 = native.layer[p] != kNoLayer && select->layers[native.layer[p]].id == 1100;
            helmet_pixels += on_1100 && native.rgba[p * 4 + 3] != 0;
            head_pixels += hidden.layer[p] != kNoLayer && select->layers[hidden.layer[p]].id == 1100 && hidden.rgba[p * 4 + 3] != 0;
        }
        std::printf("Fighter preview colour %d: helmet %zu pixels, bare head and horns %zu\n", color, helmet_pixels, head_pixels);
        CHECK(helmet_pixels > 3000 && head_pixels * 100 >= helmet_pixels * 50);
    }
    double spread = 0.0;
    for (int a = 0; a < kNativeColorCount; ++a) {
        for (int b = 0; b < kNativeColorCount; ++b) {
            for (int c = 0; c < 3; ++c) spread = std::max(spread, std::abs(head_means[size_t(a)][size_t(c)] - head_means[size_t(b)][size_t(c)]));
        }
    }
    CHECK(spread > 20.0); // each Base Color draws its own head

    // Gameplay poses, each track alone: the repainted sprite draws a head where the helmet was, it is the frame's only
    // head, and none of the helmet's own texels is left in it.
    struct Pose {
        const char* animation;
        size_t frame;
    };
    const Pose poses[] = {{"IDLE", 0}, {"MOVE_2", 0}, {"RUN_2", 0}, {"RUN_2", 1}, {"ATTACK_1", 0}, {"KNOCKBACK", 0}, {"BLOWDOWN_4", 0},
                          {"DIE", 0},  {"JUMP_2", 0}, {"TURN_1", 1}, {"FT_UI_DRINK_1", 3}};
    for (const int color : {0, 3}) {
        const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, color);
        for (const Pose& pose : poses) {
            const SpriteImage before = RenderGamePose(cpk, maps, klass, *game, pose.animation, pose.frame, CustomLook{true, uint8_t(color), {}}, 0);
            const SpriteImage after = RenderGamePose(cpk, maps, klass, *game, pose.animation, pose.frame, Hidden(color, {"helmet"}), 0);
            std::set<uint32_t> heads;
            size_t bare_layers = 0;
            double bx = 0, by = 0, ax = 0, ay = 0;
            size_t helmet_pixels = 0, head_pixels = 0, residue = 0;
            for (size_t p = 0; p < before.layer.size(); ++p) {
                if (before.layer[p] != kNoLayer) {
                    const uint32_t id = game->layers[before.layer[p]].id;
                    const HelmetSpriteRule* rule = rule_of(id);
                    if (rule && rule->kind == HelmetSprite::kBareHead) {
                        heads.insert(id);
                        if (before.rgba[p * 4 + 3]) {
                            ++helmet_pixels;
                            bx += double(p % size_t(before.width));
                            by += double(p / size_t(before.width));
                        }
                    }
                    bare_layers += id >= 1200 && id < 1300;
                }
                if (after.layer[p] == kNoLayer || !after.rgba[p * 4 + 3]) continue;
                const HelmetSpriteRule* rule = rule_of(game->layers[after.layer[p]].id);
                if (!rule || rule->kind != HelmetSprite::kBareHead) continue;
                ++head_pixels;
                ax += double(p % size_t(after.width));
                ay += double(p / size_t(after.width));
                const int tx = int(after.texel[p] % uint32_t(map.width)), ty = int(after.texel[p] / uint32_t(map.width));
                const std::array<uint8_t, 4> drawn = atlas.surfaces.front().Texel(tx, ty);
                residue += OwnerOfTexel(klass, map, tx, ty) == helmet && drawn[3] == 255 &&
                           std::memcmp(&before.rgba[p * 4], &after.rgba[p * 4], 4) == 0 && before.layer[p] == after.layer[p];
            }
            const double distance = helmet_pixels && head_pixels ? std::hypot(ax / double(head_pixels) - bx / double(helmet_pixels),
                                                                              ay / double(head_pixels) - by / double(helmet_pixels))
                                                                 : 1e9;
            std::printf("Fighter pose %s f%zu colour %d: helmet sprite %u, %zu helmet pixels, %zu head pixels, centres %.1f apart, residue %zu\n",
                        pose.animation, pose.frame, color, heads.empty() ? 0u : *heads.begin(), helmet_pixels, head_pixels, distance, residue);
            CHECK(heads.size() == 1 && bare_layers == 0 && head_pixels * 100 >= helmet_pixels * 30 && distance < 30.0 && residue * 100 <= head_pixels);
        }
    }
    CheckServedLook(art, cpk, klass, *select, Hidden(0, {"helmet"}));
    CheckServedLook(art, cpk, klass, *select, Hidden(2, {"helmet", "horns"}));
    CustomLook dyed = Hidden(4, {"helmet"});
    dyed.SetPartColor("face", {40, 220, 90});
    CheckServedLook(art, cpk, klass, *select, dyed);
}

// Helmet Hidden in every animation: the helmets the model draws off the head (held, dropped, flying) sample copies that the
// hidden atlas leaves empty, so the bare head shows once and no helmet shows anywhere; every other Fighter draws as before.
void TestFighterHelmetContexts(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kFighter;
    const std::vector<uint8_t> bytes = cpk.Read(*cpk.Find(GameModelName(klass)));
    const std::optional<Mbs> original = ParseMbs(bytes);
    const std::optional<std::vector<uint8_t>> patched_bytes = FighterModelWithHelmetCopies(bytes);
    const std::optional<Mbs> patched = patched_bytes ? ParseMbs(*patched_bytes) : std::nullopt;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(original && patched && select && art.Regions(klass));
    if (!original || !patched || !select || !art.Regions(klass)) return;
    const std::vector<RegionMap>& maps = *art.Regions(klass);
    const std::vector<uint32_t> off_list = OffHeadHelmetLayers(*original);
    const std::set<uint32_t> off_head(off_list.begin(), off_list.end());
    const std::span<const HelmetCopy> copies = FighterHelmetCopies();
    std::set<uint32_t> copied;
    for (const HelmetCopy& copy : copies) copied.insert(copy.id);

    // Only the off-head records of the copied sprites move, each to its sprite's copy, moved by whole blocks.
    CHECK(patched->sources.size() == original->sources.size() + copies.size() && patched->layers.size() == original->layers.size() &&
          patched->keys.size() == original->keys.size() && patched->frames.size() == original->frames.size());
    size_t moved = 0, wrong_move = 0;
    for (size_t i = 0; i < original->layers.size(); ++i) {
        const MbsLayer& a = original->layers[i];
        const MbsLayer& b = patched->layers[i];
        const bool should = off_head.count(uint32_t(i)) && copied.count(a.id);
        moved += a.source != b.source;
        wrong_move += (a.source != b.source) != should || a.id != b.id || a.color != b.color || a.destination != b.destination || a.texture != b.texture;
        if (!should) continue;
        const HelmetCopy& copy = *std::find_if(copies.begin(), copies.end(), [&](const HelmetCopy& c) { return c.id == a.id; });
        const AtlasRect from = SourceRectOf(*original, a, 4096, 4096), to = SourceRectOf(*patched, b, 4096, 4096);
        wrong_move += to.x0 != from.x0 + copy.dx || to.y0 != from.y0 + copy.dy || to.x1 != from.x1 + copy.dx || to.y1 != from.y1 + copy.dy;
    }
    std::printf("Fighter helmet contexts: %zu off-head helmet records, %zu moved to copies, %zu wrong\n", off_head.size(), moved, wrong_move);
    CHECK(off_head.size() == 65 && moved == 39 && wrong_move == 0);

    // Each copy lands where neither model samples and no preset draws, and carries its sprite's texels exactly.
    std::vector<AtlasRect> sampled;
    for (const Mbs* model : {&*original, &*select}) {
        for (const MbsLayer& layer : model->layers) {
            if (layer.texture == 0) sampled.push_back(SourceRectOf(*model, layer, 4096, 4096));
        }
    }
    for (int color = 0; color < kNativeColorCount; ++color) {
        std::vector<uint8_t> pack = cpk.Read(*cpk.Find(AtlasName(klass, color)));
        const FtxSurface before = *ReadFtxSurface(pack, 0);
        CHECK(AddHelmetCopies(pack));
        const FtxSurface after = *ReadFtxSurface(pack, 0);
        size_t occupied = 0, differ = 0, outside = 0;
        for (const HelmetCopy& copy : copies) {
            const HelmetSpriteRule& rule = *std::find_if(FighterHelmetSprites().begin(), FighterHelmetSprites().end(),
                                                         [&](const HelmetSpriteRule& r) { return r.id == copy.id; });
            const AtlasRect from = HelmetCopySource(rule);
            const AtlasRect to{from.x0 + copy.dx, from.y0 + copy.dy, from.x1 + copy.dx, from.y1 + copy.dy};
            for (const AtlasRect& r : sampled) occupied += r.x0 < to.x1 && to.x0 < r.x1 && r.y0 < to.y1 && to.y0 < r.y1;
            for (int y = from.y0; y < from.y1; ++y) {
                for (int x = from.x0; x < from.x1; ++x) {
                    occupied += before.Texel(x + copy.dx, y + copy.dy)[3] != 0;
                    differ += after.Texel(x + copy.dx, y + copy.dy) != before.Texel(x, y);
                }
            }
        }
        for (int y = 0; y < 4096; y += 7) {
            for (int x = 0; x < 4096; x += 7) {
                const bool in_copy = std::any_of(copies.begin(), copies.end(), [&](const HelmetCopy& copy) {
                    const HelmetSpriteRule& rule = *std::find_if(FighterHelmetSprites().begin(), FighterHelmetSprites().end(),
                                                                 [&](const HelmetSpriteRule& r) { return r.id == copy.id; });
                    const AtlasRect from = HelmetCopySource(rule);
                    return AtlasRect{from.x0 + copy.dx, from.y0 + copy.dy, from.x1 + copy.dx, from.y1 + copy.dy}.Contains(x, y);
                });
                outside += !in_copy && after.Texel(x, y) != before.Texel(x, y);
            }
        }
        CHECK(occupied == 0 && differ == 0 && outside == 0);
    }

    // Every tick of every animation, for several looks with the helmet hidden.
    for (const int color : {0, 3}) {
        for (const int variant : {0, 1, 2}) {
            CustomLook look = Hidden(color, {"helmet"});
            if (variant == 1) look.SetPartHidden("horns", true);
            if (variant == 2) look.SetPartColor("face", {40, 220, 90});
            const std::vector<uint8_t> native = cpk.Read(*cpk.Find(AtlasName(klass, color)));
            const std::optional<std::vector<uint8_t>> pack = CustomizeAtlas(native, maps, klass, look);
            CHECK(pack.has_value());
            if (!pack) continue;
            const HelmetTicks ticks = CountHelmetTicks(*patched, off_head, *ReadFtxSurface(*pack, 0));
            std::printf("Fighter helmet hidden, colour %d look %d: %ld ticks, official bare head %ld, game's bare head %ld, no head %ld; "
                        "off-head helmet transparent %ld, visor pieces cleared %ld; helmet visible %ld, duplicate heads %ld, conflicts %ld\n",
                        color, variant, ticks.total, ticks.official_head, ticks.game_head, ticks.no_head, ticks.off_head_hidden, ticks.visor_cleared,
                        ticks.helmet_visible, ticks.duplicate, ticks.conflicts);
            CHECK(ticks.total == ticks.official_head + ticks.game_head + ticks.no_head);
            CHECK(ticks.helmet_visible == 0 && ticks.duplicate == 0 && ticks.conflicts == 0 && ticks.official_head > ticks.game_head);
        }
    }

    // Rendered tick by tick: every animation that draws 1101, on the head or off it, and the poses around it.
    std::set<std::string> animations = {"IDLE_A", "IDLE_B", "WAKE_B", "BLOWDOWN_B_3", "BLOWDOWN_B_4", "BLOWDOWN_B_5", "BLOWDOWN_KNOCKBACK_B",
                                        "DIE_B", "DIE", "BLOWDOWN_4", "MOVE_2", "RUN_2", "JUMP_3", "ATTACK_1", "ATTACK_5_END"};
    for (const MbsAnimation& animation : original->animations) {
        const bool variant = animation.name.size() > 2 && animation.name[1] == '_' &&
                             (animation.name[0] == 'K' || animation.name[0] == 'T' || animation.name[0] == 'S');
        if (variant) continue;
        for (size_t t = 0; t < animation.track_count; ++t) {
            const MbsTrack& run = original->tracks[animation.first_track + t];
            for (size_t f = 0; f < run.frame_count; ++f) {
                const MbsKey& key = original->keys[original->frames[run.first_frame + f].key];
                for (uint32_t i = 0; i < key.layer_count; ++i) {
                    if (original->layers[key.first_layer + i].id == 1101) animations.insert(animation.name);
                }
            }
        }
    }
    std::map<uint32_t, HelmetSprite> kind;
    for (const HelmetSpriteRule& rule : FighterHelmetSprites()) kind[rule.id] = rule.kind;
    const std::vector<uint8_t> native_pack = cpk.Read(*cpk.Find(AtlasName(klass, 0)));
    std::vector<uint8_t> copied_pack = native_pack;
    CHECK(AddHelmetCopies(copied_pack));
    const std::optional<std::vector<uint8_t>> hidden_pack = CustomizeAtlas(native_pack, maps, klass, Hidden(0, {"helmet"}));
    CHECK(hidden_pack.has_value());
    if (!hidden_pack) return;
    const FtxSurface native = *ReadFtxSurface(native_pack, 0), with_copies = *ReadFtxSurface(copied_pack, 0),
                     hidden = *ReadFtxSurface(*hidden_pack, 0);
    std::vector<FtxSurface> weapons;
    for (const WeaponTexture& weapon : WeaponTexturesOf(klass)) weapons.push_back(*ReadFtxSurface(cpk.Read(*cpk.Find(WeaponAtlasName(klass, weapon))), 0));
    std::array<const FtxSurface*, 16> before{}, after{}, bare{};
    before[0] = &native;
    after[0] = &with_copies;
    bare[0] = &hidden;
    for (size_t w = 0; w < weapons.size(); ++w) before[WeaponTexturesOf(klass)[w].texture] = after[WeaponTexturesOf(klass)[w].texture] =
        bare[WeaponTexturesOf(klass)[w].texture] = &weapons[w];
    long rendered = 0, native_off = 0, helmet_pixels = 0, beside = 0, headless = 0;
    for (const std::string& name : animations) {
        size_t a = 0;
        while (a < original->animations.size() && original->animations[a].name != name) ++a;
        CHECK(a < original->animations.size());
        if (a >= original->animations.size()) continue;
        for (const auto& frames : AnimationTicks(*original, a)) {
            ++rendered;
            const SpriteImage o = RenderTick(*original, a, frames, before, 0.5f);
            const SpriteImage c = RenderTick(*patched, a, frames, after, 0.5f);
            const SpriteImage h = RenderTick(*patched, a, frames, bare, 0.5f);
            int worst = 0;
            for (size_t p = 0; p < o.rgba.size(); ++p) worst = std::max(worst, std::abs(int(o.rgba[p]) - int(c.rgba[p])));
            native_off += worst > 1; // the same texels at other coordinates: float rounding moves a channel by one at most
            size_t helmet = 0, head = 0, game_head = 0, native_head = 0;
            for (size_t p = 0; p < h.layer.size(); ++p) {
                if (o.layer[p] != kNoLayer && o.rgba[p * 4 + 3] >= 32) {
                    const uint32_t layer = o.layer[p], id = original->layers[layer].id;
                    native_head += id >= 1100 && id < 1200 && !off_head.count(layer) && kind[id] == HelmetSprite::kBareHead;
                }
                if (h.layer[p] == kNoLayer || h.rgba[p * 4 + 3] < 32) continue;
                const uint32_t layer = h.layer[p], id = patched->layers[layer].id;
                game_head += id >= 1200 && id < 1300;
                if (id < 1100 || id >= 1200) continue;
                if (off_head.count(layer) || kind[id] != HelmetSprite::kBareHead) ++helmet;
                else ++head;
            }
            helmet_pixels += long(helmet);
            beside += head && game_head;
            headless += native_head > 20 && !head;
        }
    }
    std::printf("Fighter helmet contexts rendered: %zu animations, %ld ticks; native drawn otherwise %ld, helmet pixels %ld, head beside the "
                "game's head %ld, helmet on the head left without a head %ld\n",
                animations.size(), rendered, native_off, helmet_pixels, beside, headless);
    CHECK(animations.size() == 93 && native_off == 0 && helmet_pixels == 0 && beside == 0 && headless == 0);
}

// With a Fighter's helmet hidden the overlay serves the model with the helmet copies and every native Fighter atlas with
// them; a Fighter that keeps its helmet gets them in its own atlas, the hidden one does not.
void TestRealHelmetHiddenOverlay(const fs::path& base, const fs::path& source_dir) {
    const fs::path app0 = source_dir / "game" / "prepared" / "v1.03";
    const fs::path archive_path = app0 / "DRACRO.CPK";
    std::error_code ec;
    if (!fs::is_regular_file(archive_path, ec)) return;
    const fs::path user = base / "helmet_overlay_user";
    fs::remove_all(user);
    WriteSaveFor(user, "CUSA10454", "SAVEDATA00", SaveWithCharacters({{0, 2}, {0, 2}, {1, 3}}));
    CustomStore store(CustomStorePath(user));
    store.Set({"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kFighter}, Hidden(2, {"helmet"}));
    store.Set({"CUSA10454", "SAVEDATA00", 0, 1, CharacterClass::kFighter}, Painted(2, {"helmet"}, {200, 40, 40}));
    store.Set({"CUSA10454", "SAVEDATA00", 0, 2, CharacterClass::kAmazon}, HairLook({30, 90, 230}));
    std::vector<std::string> notes;
    const std::vector<CustomInstance> planned = PlanCustomInstances(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
    CHECK(planned.size() == 3);
    CharacterArtSources sources = ArtSourcesOf(app0, "DRACRO_US.CPK");
    sources.cache_dir = base / "region_cache";
    CharacterArt art(sources);
    const std::optional<CustomOverlay> built = BuildCustomOverlay(archive_path, planned, art, notes);
    CHECK(built && built->runtime.size() == 3);
    if (!built || built->runtime.size() != 3) return;
    CpkArchive original, served;
    CHECK(original.Open(archive_path) && OpenOverlaid(archive_path, built->overlay, served));
    CHECK(served.entries().size() == original.entries().size() + 3);
    const auto read_served = [&](const std::string& path) {
        const CpkEntry* entry = served.Find(path);
        return entry ? ReadOverlaid(archive_path, built->overlay, entry->offset, entry->size) : std::vector<uint8_t>{};
    };
    const std::vector<uint8_t> model = original.Read(*original.Find(GameModelName(CharacterClass::kFighter)));
    CHECK(read_served(GameModelName(CharacterClass::kFighter)) == FighterModelWithHelmetCopies(model));
    for (int color = 0; color < kNativeColorCount; ++color) {
        std::vector<uint8_t> expected = original.Read(*original.Find(AtlasName(CharacterClass::kFighter, color)));
        CHECK(AddHelmetCopies(expected) && read_served(AtlasName(CharacterClass::kFighter, color)) == expected);
    }
    size_t moved = 0;
    for (const CpkEntry& entry : original.entries()) {
        const CpkEntry* kept = served.Find(entry.path);
        const bool replaced = entry.path.rfind("player/Fighter0", 0) == 0 && (entry.path.find(".ftx") == 16 || entry.path == "player/Fighter00.mbs");
        moved += !kept || (!replaced && (kept->offset != entry.offset || kept->size != entry.size));
    }
    CHECK(moved == 0);
    const std::vector<RegionMap>* maps = art.Regions(CharacterClass::kFighter);
    const std::vector<uint8_t> source = original.Read(*original.Find(NativeColorAtlas(CharacterClass::kFighter, 2)));
    for (size_t i = 0; i < 2; ++i) {
        std::optional<std::vector<uint8_t>> expected = CustomizeAtlas(source, *maps, CharacterClass::kFighter, planned[i].look);
        CHECK(expected.has_value());
        if (!expected) continue;
        const PartLook* helmet = planned[i].look.Find("helmet");
        if (!(helmet && helmet->hidden)) CHECK(AddHelmetCopies(*expected));
        CHECK(read_served(planned[i].entry) == *expected);
    }
}

// Online: another player's look arrives as a recipe and is built, from this client's own files, into a texture reserved
// at launch; byte for byte what the saved-look pipeline serves for the same look, weapons included.
void TestRemoteLookParity(const fs::path& base, const fs::path& source_dir) {
    const fs::path app0 = source_dir / "game" / "prepared" / "v1.03";
    const fs::path archive_path = app0 / "DRACRO.CPK";
    std::error_code ec;
    if (!fs::is_regular_file(archive_path, ec)) {
        std::printf("installed game not present, skipping the remote look parity check\n");
        return;
    }
    CpkArchive original;
    CHECK(original.Open(archive_path));
    std::string error;
    const std::optional<std::vector<RemoteTextureSet>> plan = PlanRemoteTextures(original, &error);
    CHECK(plan && plan->size() == size_t(6 * kNativeColorCount * kRemoteLooksPerPreset));
    if (!plan) return;
    uint64_t total = 0;
    for (const RemoteTextureSet& set : *plan) {
        const CpkEntry* atlas = original.Find(NativeColorAtlas(set.klass, set.base));
        CHECK(atlas && std::max(atlas->size, atlas->extract_size) == set.atlas.size);
        CHECK(set.weapons.size() == WeaponTexturesOf(set.klass).size());
        total += set.atlas.size;
        for (const RemoteEntry& weapon : set.weapons) total += weapon.size;
        if (set.index == 0)
            std::printf("remote textures: %s colour %d atlas %llu bytes\n", std::string(ClassName(set.klass)).c_str(), set.base,
                        static_cast<unsigned long long>(set.atlas.size));
    }
    std::printf("remote textures: %zu sets, %llu MB of archive space reserved (no memory until a look is built), archive has %zu entries\n",
                plan->size(), static_cast<unsigned long long>(total >> 20), original.entries().size());

    const fs::path user = base / "remote_parity_user";
    fs::remove_all(user);
    WriteSaveFor(user, "CUSA10454", "SAVEDATA00", SaveWithCharacters({{0, 0}, {2, 1}, {0, 0}}));
    CustomLook fighter{true, 0, {}};
    fighter.SetPartColor("armor", Rgb{0x22, 0xaa, 0x55});
    fighter.SetPartHidden("helmet", true);
    fighter.SetPartColor("horns", Rgb{0x22, 0x55, 0xff});
    fighter.SetPartColor("shield_accent", Rgb{0xdd, 0x22, 0x22});
    CustomLook elf{true, 1, {}};
    elf.SetPartHidden("hood", true);
    elf.SetPartColor("bow", Rgb{0x10, 0xc0, 0x40});
    CustomLook red_fighter{true, 0, {}};
    red_fighter.SetPartColor("armor", Rgb{0xcc, 0x10, 0x10});
    red_fighter.SetPartColor("sword", Rgb{0xff, 0xd0, 0x00});
    CustomStore store(CustomStorePath(user));
    store.Set({"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kFighter}, fighter);
    store.Set({"CUSA10454", "SAVEDATA00", 0, 1, CharacterClass::kElf}, elf);
    store.Set({"CUSA10454", "SAVEDATA00", 0, 2, CharacterClass::kFighter}, red_fighter);
    std::vector<std::string> notes;
    const std::vector<CustomInstance> planned = PlanCustomInstances(store, DiscoverSaves(user, "CUSA10454"), "CUSA10454", notes);
    CHECK(planned.size() == 3);
    CharacterArtSources sources = ArtSourcesOf(app0, "DRACRO_US.CPK");
    sources.cache_dir = base / "region_cache";
    CharacterArt art(sources);
    const std::optional<CustomOverlay> built = BuildCustomOverlay(archive_path, planned, art, notes, &*plan);
    CHECK(built && built->runtime.size() == 3 && built->remote.size() == plan->size() && built->helmet_copies);
    if (!built || built->runtime.size() != 3 || built->remote.size() != plan->size()) return;

    // The reserved textures are listed where the overlay leaves room for them; nothing old moved.
    CpkArchive served;
    CHECK(OpenOverlaid(archive_path, built->overlay, served));
    size_t reserved = 0;
    for (const RemoteTextureSet& set : built->remote) {
        for (const RemoteEntry* entry : {&set.atlas}) {
            const CpkEntry* listed = served.Find(entry->name);
            CHECK(listed && listed->offset == entry->offset && listed->size == entry->size && listed->extract_size == entry->size);
            ++reserved;
        }
        for (const RemoteEntry& weapon : set.weapons) {
            const CpkEntry* listed = served.Find(weapon.name);
            CHECK(listed && listed->offset == weapon.offset && listed->size == weapon.size);
            ++reserved;
        }
    }
    CHECK(built->overlay.reserved.size() == reserved);
    CHECK(served.entries().size() == original.entries().size() + 3 + 3 + reserved); // three atlases, the shield, the bow, the sword
    const auto read_served = [&](const std::string& path) {
        const CpkEntry* entry = served.Find(path);
        return entry ? ReadOverlaid(archive_path, built->overlay, entry->offset, entry->size) : std::vector<uint8_t>{};
    };

    // Each saved look carries its recipe for the room.
    for (size_t i = 0; i < planned.size(); ++i) {
        const std::optional<appearance_net::Recipe> recipe = RecipeOfLook(planned[i].instance.klass, planned[i].instance.base, planned[i].look);
        const AppearanceInstance& runtime = built->runtime[i];
        CHECK(recipe && std::vector<uint8_t>(runtime.recipe, runtime.recipe + runtime.recipe_size) == appearance_net::Encode(*recipe));
    }

    // Player B's client: the recipes arrive and are built from its own files.
    std::map<std::string, std::vector<uint8_t>> remote_bytes;
    RemoteLooks::Host host;
    host.threaded = false;
    host.now_ms = [] { return uint64_t(1000000); };
    host.log = [&](const std::string& line) { notes.push_back(line); };
    host.serve = [&](const RemoteEntry& entry, const uint8_t* data) {
        const CpkEntry* listed = served.Find(entry.name);
        CHECK(listed && listed->offset == entry.offset && listed->size == entry.size);
        remote_bytes[entry.name] = std::vector<uint8_t>(data, data + entry.size);
        return true;
    };
    host.build = [&](CharacterClass klass, int color, const CustomLook& look, std::string* why) -> std::optional<LookTextures> {
        const EntryReader read = [&](const std::string& entry) {
            const CpkEntry* found = original.Find(entry);
            return found ? original.Read(*found) : std::vector<uint8_t>{};
        };
        const std::vector<RegionMap>* maps = art.Regions(klass);
        if (!maps) return std::nullopt;
        std::vector<std::string> weapon_notes;
        return BuildLookTextures(read(NativeColorAtlas(klass, color)), read, *maps, klass, look, built->helmet_copies, weapon_notes, why);
    };
    RemoteLooks looks(built->remote, host);
    for (int slot = 1; slot <= 3; ++slot) {
        const AppearanceInstance& from = built->runtime[size_t(slot - 1)];
        looks.Set(slot, std::span<const uint8_t>(from.recipe, from.recipe_size));
    }
    looks.Drain();
    const AppearanceInstance* a = looks.Of(1, 0, 0);
    const AppearanceInstance* e = looks.Of(2, 2, 1);
    const AppearanceInstance* r = looks.Of(3, 0, 0);
    CHECK(a && e && r && a != r);
    if (!a || !e || !r) return;
    CHECK(std::string(a->entry) == "PLAYER/FIGHTER00_NET1.FTX" && std::string(r->entry) == "PLAYER/FIGHTER00_NET2.FTX" &&
          std::string(e->entry) == "PLAYER/ELF01_NET1.FTX");
    // Same bytes as the saved-look pipeline, per atlas and per default weapon.
    CHECK(!remote_bytes[a->entry].empty() && remote_bytes[a->entry] == read_served(built->runtime[0].entry));
    CHECK(!remote_bytes[e->entry].empty() && remote_bytes[e->entry] == read_served(built->runtime[1].entry));
    CHECK(!remote_bytes[r->entry].empty() && remote_bytes[r->entry] == read_served(built->runtime[2].entry));
    CHECK(remote_bytes[a->entry] != remote_bytes[r->entry]); // two Fighters, two atlases
    CHECK(std::string(a->weapons[1].entry) == "PLAYER/FIGHTER00_SHIELD01_NET1.FTX" && a->weapons[0].entry[0] == 0);
    CHECK(remote_bytes[a->weapons[1].entry] == read_served(built->runtime[0].weapons[1].entry));
    CHECK(std::string(r->weapons[0].entry) == "PLAYER/FIGHTER00_ARMS01_NET2.FTX" && r->weapons[1].entry[0] == 0);
    CHECK(remote_bytes[r->weapons[0].entry] == read_served(built->runtime[2].weapons[0].entry));
    CHECK(std::string(e->weapons[0].entry) == "PLAYER/ELF01_ARMS01_NET1.FTX");
    CHECK(remote_bytes[e->weapons[0].entry] == read_served(built->runtime[1].weapons[0].entry));
    // The hidden helmet's atlas leaves the copies empty; the other Fighter's atlas has them, as served natives do.
    std::vector<uint8_t> native = original.Read(*original.Find(NativeColorAtlas(CharacterClass::kFighter, 0)));
    CHECK(AddHelmetCopies(native) && read_served(NativeColorAtlas(CharacterClass::kFighter, 0)) == native);
    std::printf("remote look parity: %zu reserved textures, Fighter/Elf/Fighter recipes of %u/%u/%u bytes built identical to saved looks\n",
                reserved, unsigned(built->runtime[0].recipe_size), unsigned(built->runtime[1].recipe_size),
                unsigned(built->runtime[2].recipe_size));
}

void TestFighterParts(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kFighter;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(select && art.Regions(klass));
    if (!select || !art.Regions(klass)) return;
    const PartRender r{art, klass, *select};
    const auto part = [&](const char* key) { return FindCustomPart(klass, key); };
    const CustomPart* armor = part("armor");
    const CustomPart* trim = part("trim");
    const CustomPart* leather = part("leather");
    const CustomPart* eye = part("eye");
    const CustomPart* crest = part("crest");
    const CustomPart* gem = part("crest_accent");
    const CustomPart* horns = part("horns");
    const CustomPart* helmet = part("helmet");
    const CustomPart* face = part("face");
    const CustomPart* sword = part("sword");
    const CustomPart* shield = part("shield");
    const CustomPart* band = part("shield_accent");
    CHECK(armor && trim && leather && eye && crest && gem && horns && helmet && face && sword && shield && band);
    if (!armor || !trim || !leather || !eye || !crest || !gem || !horns || !helmet || !face || !sword || !shield || !band) return;
    std::vector<std::string> keys;
    for (const CustomPart& each : CustomPartsOf(klass)) keys.emplace_back(each.key);
    CHECK((keys == std::vector<std::string>{"armor", "trim", "leather", "eye", "crest", "crest_accent", "horns", "helmet", "face", "sword",
                                            "shield", "shield_accent"}));
    CHECK(PartLabel(klass, "eye") == "Eye" && PartLabel(klass, "crest_accent") == "Crest Accent" && PartLabel(klass, "horns") == "Horns" &&
          PartLabel(klass, "helmet") == "Helmet" && PartLabel(klass, "crest") == "Crest" && PartLabel(klass, "leather") == "Leather");
    CHECK(FindCustomPart(klass, "accent") == nullptr); // the visor glow was never an accent of the armor
    const std::span<const WeaponTexture> weapons = WeaponTexturesOf(klass);
    CHECK(weapons.size() == 2 && weapons[0].name == "ARMS01" && weapons[0].texture == 1 && weapons[1].name == "SHIELD01" &&
          weapons[1].texture == 2);
    CHECK(sword->weapon == 0 && shield->weapon == 1 && WeaponPartOfTexture(klass, 1) == sword && WeaponPartOfTexture(klass, 2) == shield);
    CHECK(band->weapon == 1 && band->accent && WeaponPartOf(klass, 1) == shield &&
          WeaponAccentOf(klass, 1) == band && WeaponAccentOf(klass, 0) == nullptr);
    CHECK(band->label == "Shield Accent" && trim->label == "Engraved Trim");
    // The engraved pauldrons are whole pieces, not a colour region of them; the visor glow is a material of its own.
    CHECK(trim->mark == PaintedMark::kNone && trim->piece_regions == 0 && trim->OwnsPiece(800) && trim->OwnsPiece(807) && trim->OwnsPiece(899));
    CHECK(eye->mark == PaintedMark::kSkinTone && eye->tone.size() == size_t(kNativeColorCount) && eye->regions == 0);
    CHECK(gem->OwnsPiece(900) && crest->OwnsPiece(900) && gem->mark == PaintedMark::kPainted && gem->piece_regions != 0);
    CHECK(helmet->OwnsPiece(1100) && horns->OwnsPiece(1100) && horns->mark == PaintedMark::kHorn && helmet->mark == PaintedMark::kNone);
    CHECK(!face->OwnsPiece(1100) && face->OwnsPiece(1200));

    // The visor glow is its own material: blue, yellow, blue, teal, pale blue through the presets. The crest's gem shifts
    // otherwise and stays the crest's; the shield's band is a texture of its own.
    const std::vector<RegionMap>& maps = *art.Regions(klass);
    const uint16_t eye_bit = PieceBitOf(klass, *eye);
    size_t glow = 0;
    for (const uint16_t v : maps.front().pieces) glow += (v & eye_bit) != 0;
    CHECK(glow > 100 && glow < 5000);

    const auto body_parts = {armor, trim, leather, eye, crest, gem, horns, helmet};
    for (int color = 0; color < kNativeColorCount; ++color) {
        const SpriteImage native = *art.Preview(klass, color);
        size_t sword_pixels = 0, shield_pixels = 0, band_pixels = 0, crest_pixels = 0, gem_pixels = 0, eye_pixels = 0, horn_pixels = 0, helmet_pixels = 0,
               trim_pixels = 0, trim_expected = 0, wrong = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            if (!r.Visible(native, p)) continue;
            const CustomPart* owner = r.PartOf(native, p);
            const uint32_t id = r.IdAt(native, p);
            if (native.texture[p] == 1) {
                sword_pixels += owner == sword;
                wrong += owner != sword;
            } else if (native.texture[p] == 2) {
                shield_pixels += owner == shield;
                band_pixels += owner == band;
                wrong += owner != shield && owner != band;
            } else {
                const bool in_crest = id >= 900 && id <= 914, in_helmet = id >= 1100 && id <= 1135, in_trim = id >= 800 && id <= 899;
                wrong += owner == sword || owner == shield || owner == band || in_crest != (owner == crest || owner == gem);
                // The helmet sprite holds helmet, horns and eye and nothing else; Leather never reaches the head or the crest.
                wrong += in_helmet && owner != helmet && owner != horns && owner != eye && owner != face;
                wrong += (in_helmet || in_crest || in_trim) && (owner == leather || owner == armor);
                crest_pixels += owner == crest;
                gem_pixels += owner == gem;
                eye_pixels += owner == eye;
                horn_pixels += owner == horns;
                helmet_pixels += owner == helmet;
                trim_expected += in_trim;
                trim_pixels += in_trim && owner == trim;
            }
        }
        CHECK(sword_pixels > 2000 && shield_pixels > 6000 && band_pixels > 1000 && crest_pixels > 400 && gem_pixels > 400 && wrong == 0);
        CHECK(horn_pixels > 600 && helmet_pixels > 600 && eye_pixels > 0);
        CHECK(trim_expected > 2000 && trim_pixels == trim_expected); // the engraved pauldrons are complete, every pixel of them

        // Each colour reaches its own part and no other; hiding a part removes nothing of the others.
        for (const CustomPart* painted : body_parts) {
            const std::string key(painted->key);
            const SpriteImage after = *art.CustomPreview(klass, color, Painted(color, {key.c_str()}, {0, 255, 64}));
            const PartEffect own = EffectOn(r, native, after, painted);
            CHECK(own.pixels > 0 && own.removed == 0 && (own.pixels < 50 || own.moved * 100 >= own.pixels * 80));
            const SpriteImage without = *art.CustomPreview(klass, color, Hidden(color, {key.c_str()}));
            const PartEffect gone = EffectOn(r, native, without, painted);
            // Helmet and horns hidden over the dark knight behind them move little; TestFighterHelmetAndHorns checks that none is left.
            CHECK(painted == horns || painted == helmet || gone.pixels < 50 || gone.moved * 100 >= gone.pixels * (painted == gem ? 50 : 60)); // its texels cleared: what lies behind shows (a gem over its own frame barely shows it)
            // A neighbour drawn translucent over the part, or filtered with it along their edge, blends with it: a
            // neighbour pixel counts only away from the part's rim and where hiding the part leaves it unchanged.
            std::vector<uint8_t> rim(native.layer.size(), 0);
            for (size_t p = 0; p < native.layer.size(); ++p) {
                if (!r.Visible(native, p) || r.PartOf(native, p) != painted) continue;
                const int x = int(p % size_t(native.width)), y = int(p / size_t(native.width));
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (x + dx >= 0 && y + dy >= 0 && x + dx < native.width && y + dy < native.height) {
                            rim[size_t(y + dy) * size_t(native.width) + size_t(x + dx)] = 1;
                        }
                    }
                }
            }
            for (const CustomPart& other : CustomPartsOf(klass)) {
                if (&other == painted) continue;
                size_t pixels = 0, leaked = 0, removed = 0;
                for (size_t p = 0; p < native.layer.size(); ++p) {
                    if (!r.Visible(native, p) || r.PartOf(native, p) != &other) continue;
                    ++pixels;
                    leaked += !rim[p] && PartRender::Delta(native, after, p) > 24 && PartRender::Delta(native, without, p) <= 2;
                    removed += !rim[p] && int(without.rgba[p * 4 + 3]) + 8 < int(native.rgba[p * 4 + 3]);
                }
                CHECK(leaked * 100 <= pixels && removed * 100 <= pixels * 2 + 2);
            }
        }
        // Hidden armour leaves what the game draws without it, and nothing in its place.
        const SpriteImage unarmoured = *art.CustomPreview(klass, color, Hidden(color, {"armor"}));
        const PartEffect plate = EffectOn(r, native, unarmoured, armor);
        CHECK(plate.removed * 100 >= plate.pixels * 80 && Damage(r, native, unarmoured, [](uint32_t, size_t) { return true; }).created == 0);

        // The weapons and the shield's band: each takes its colour and its visibility alone.
        const SpriteImage red_sword = *art.CustomPreview(klass, color, Painted(color, {"sword"}, {220, 40, 40}));
        CHECK(EffectOn(r, native, red_sword, sword).moved * 100 >= EffectOn(r, native, red_sword, sword).pixels * 90);
        CHECK(EffectOnTexture(native, red_sword, 2).moved == 0 && EffectOnTexture(native, red_sword, 0).moved * 100 <= EffectOnTexture(native, red_sword, 0).pixels);
        const SpriteImage green_shield = *art.CustomPreview(klass, color, Painted(color, {"shield"}, {40, 200, 60}));
        CHECK(EffectOn(r, native, green_shield, shield).moved * 100 >= EffectOn(r, native, green_shield, shield).pixels * 80);
        CHECK(EffectOn(r, native, green_shield, band).moved * 100 <= EffectOn(r, native, green_shield, band).pixels * 10); // only its soft rim
        CHECK(EffectOnTexture(native, green_shield, 1).moved == 0);
        const SpriteImage red_band = *art.CustomPreview(klass, color, Painted(color, {"shield_accent"}, {230, 30, 30}));
        CHECK(EffectOn(r, native, red_band, band).moved * 100 >= EffectOn(r, native, red_band, band).pixels * 80);
        CHECK(EffectOn(r, native, red_band, shield).moved * 100 <= EffectOn(r, native, red_band, shield).pixels * 3);
        const SpriteImage no_band = *art.CustomPreview(klass, color, Hidden(color, {"shield_accent"}));
        CHECK(EffectOn(r, native, no_band, band).moved * 100 >= EffectOn(r, native, no_band, band).pixels * 90);
        CHECK(EffectOn(r, native, no_band, shield).moved * 100 <= EffectOn(r, native, no_band, shield).pixels * 3);
        const SpriteImage no_sword = *art.CustomPreview(klass, color, Hidden(color, {"sword"}));
        CHECK(EffectOnTexture(native, no_sword, 1).pixels > 0 && DrawnFrom(no_sword, 1) == 0);
        CHECK(EffectOnTexture(native, no_sword, 2).moved * 100 <= EffectOnTexture(native, no_sword, 2).pixels);
        CHECK(EffectOnTexture(native, no_sword, 0).removed * 100 <= EffectOnTexture(native, no_sword, 0).pixels);
        // The shield hidden leaves its band, which is a part of its own.
        const SpriteImage no_shield = *art.CustomPreview(klass, color, Hidden(color, {"shield"}));
        CHECK(EffectOn(r, native, no_shield, shield).moved * 100 >= EffectOn(r, native, no_shield, shield).pixels * 75);
        CHECK(EffectOn(r, native, no_shield, band).removed * 100 <= EffectOn(r, native, no_shield, band).pixels * 30); // its soft rim
        CHECK(EffectOnTexture(native, no_shield, 1).moved * 100 <= EffectOnTexture(native, no_shield, 1).pixels);
        const SpriteImage no_crest = *art.CustomPreview(klass, color, Hidden(color, {"crest"}));
        size_t crest_left = 0;
        for (size_t p = 0; p < no_crest.layer.size(); ++p) {
            crest_left += no_crest.layer[p] != kNoLayer && no_crest.rgba[p * 4 + 3] >= 64 && r.IdAt(no_crest, p) == 900 && r.PartOf(native, p) == crest;
        }
        CHECK(crest_left <= 12 && Damage(r, native, no_crest, [](uint32_t, size_t) { return true; }).created == 0);

        // Crest and its gem hide on their own too.
        const SpriteImage no_gem = *art.CustomPreview(klass, color, Hidden(color, {"crest_accent"}));
        const SpriteImage no_frame = *art.CustomPreview(klass, color, Hidden(color, {"crest"}));
        CHECK(EffectOn(r, native, no_gem, gem).moved * 100 >= EffectOn(r, native, no_gem, gem).pixels * 50);
        CHECK(EffectOn(r, native, no_gem, crest).moved * 100 <= EffectOn(r, native, no_gem, crest).pixels * 3);
        CHECK(EffectOn(r, native, no_frame, gem).moved * 100 <= EffectOn(r, native, no_frame, gem).pixels * 6);
    }

    // On the atlas, texel by texel: a part's colour, its hiding and a random colour change only the part's own texels.
    std::mt19937 random(7);
    for (int color : {0, 3}) {
        const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, color);
        for (const CustomPart* each : body_parts) {
            const std::string key(each->key);
            for (const CustomLook& look : {Painted(color, {key.c_str()}, {200, 40, 160}), Hidden(color, {key.c_str()})}) {
                if (each == helmet && look.parts.front().hidden) continue; // the bare head: TestFighterBareHead
                const AtlasChange change = ChangeOnAtlas(klass, maps, atlas, look, {each});
                CHECK(change.inside > 0 && change.inside_changed * 100 >= change.inside * 60 && change.outside_changed == 0);
            }
            CustomLook randomized{true, uint8_t(color), {}};
            randomized.SetPartColor(key, NextRandomColor(random, std::nullopt));
            CHECK(ChangeOnAtlas(klass, maps, atlas, randomized, {each}).outside_changed == 0);
        }
        // Armour recolour leaves the glow, the trim and the crest; the glow's colour leaves the armour.
        const AtlasChange gold = ChangeOnAtlas(klass, maps, atlas, Painted(color, {"armor"}, {200, 150, 40}), {armor});
        CHECK(gold.outside_changed == 0);
        // Hidden armour and leather together leave nothing of either.
        const AtlasChange bare = ChangeOnAtlas(klass, maps, atlas, Hidden(color, {"armor", "leather"}), {armor, leather});
        CHECK(bare.inside > 0 && bare.inside_cleared == bare.inside && bare.outside_changed == 0);
    }

    // All Colors Random: every part a colour, the base and the visibility kept.
    CustomLook all{true, 2, {}};
    all.SetPartHidden("armor", true);
    all.SetPartHidden("shield_accent", true);
    all.RandomizeColors(klass, random);
    CHECK(all.base == 2 && all.Find("armor")->hidden && all.Find("shield_accent")->hidden);
    for (const CustomPart& each : CustomPartsOf(klass)) CHECK(all.Find(each.key) && all.Find(each.key)->color);

    // In game: the served atlas matches the preview, and gameplay poses lose the armour and gain nothing.
    CustomLook mixed = Hidden(0, {"armor", "trim", "shield_accent"});
    mixed.SetPartColor("eye", {230, 30, 30});
    mixed.SetPartHidden("horns", true);
    mixed.SetPartColor("crest_accent", {30, 90, 220});
    mixed.SetPartColor("crest", {30, 200, 90});
    CheckServedLook(art, cpk, klass, *select, mixed);
    const std::optional<Mbs> game = ParseMbs(cpk.Read(*cpk.Find(GameModelName(klass))));
    CHECK(game.has_value());
    if (!game) return;
    for (const auto& [animation, frame] : {std::pair{"IDLE", 0}, std::pair{"MOVE_2", 4}, std::pair{"RUN_ATTACK", 3}}) {
        const SpriteImage native = RenderGamePose(cpk, maps, klass, *game, animation, size_t(frame), CustomLook{true, 0, {}});
        const SpriteImage hidden = RenderGamePose(cpk, maps, klass, *game, animation, size_t(frame), Hidden(0, {"armor"}));
        size_t body = 0, removed = 0, created = 0;
        for (size_t p = 0; p < native.layer.size() && p < hidden.layer.size(); ++p) {
            created += hidden.rgba[p * 4 + 3] > native.rgba[p * 4 + 3] + 8;
            if (native.layer[p] == kNoLayer || native.texture[p] != 0 || native.rgba[p * 4 + 3] < 128) continue;
            ++body;
            removed += int(hidden.rgba[p * 4 + 3]) + 8 < int(native.rgba[p * 4 + 3]);
        }
        CHECK(body > 1000 && removed * 100 >= body * 40 && created == 0);
    }

    // Runtime weapons: re-encoded with the preview's shader; untouched blocks stay byte-identical.
    for (size_t w = 0; w < weapons.size(); ++w) {
        const CustomPart* weapon = WeaponPartOf(klass, w);
        const std::vector<uint8_t> pack = cpk.Read(*cpk.Find(WeaponAtlasName(klass, weapons[w])));
        CHECK(CustomizeWeapon(pack, klass, w, CustomLook{true, 0, {}}) == pack);
        CHECK(CustomizeWeapon(pack, klass, w, Painted(0, {w == 0 ? "shield" : "sword"})) == pack); // the other weapon's look
        CHECK((CustomizeWeapon(pack, klass, w, Painted(0, {"shield_accent"})) == pack) == (w == 0));
        for (const CustomLook& look : {Painted(0, {std::string(weapon->key).c_str()}, {200, 40, 40}), Hidden(0, {"shield_accent"})}) {
            const std::optional<std::vector<uint8_t>> custom = CustomizeWeapon(pack, klass, w, look);
            CHECK(custom.has_value() && (*custom != pack) == WeaponChanged(klass, w, look));
            if (!custom || *custom == pack) continue;
            const std::optional<FtxSurfaceLayout> layout = FtxLayoutOf(pack, 0);
            const std::optional<FtxSurface> original = ReadFtxSurface(pack, 0);
            const std::optional<FtxSurface> served = ReadFtxSurface(*custom, 0);
            CHECK(layout && original && served);
            if (!layout || !original || !served) continue;
            const WeaponShader shader(klass, look, w, [&](int x, int y) { return original->Texel(x, y); }, original->width(), original->height());
            double error = 0.0;
            size_t samples = 0, outside = 0;
            for (int by = 0; by < layout->height / 4; by += 3) {
                for (int bx = 0; bx < layout->width / 4; bx += 3) {
                    bool opaque = false;
                    for (int t = 0; t < 16; ++t) opaque |= original->Texel(bx * 4 + (t & 3), by * 4 + (t >> 2))[3] != 0;
                    const size_t at = FtxBlockOffset(*layout, bx, by);
                    if (!opaque) {
                        outside += std::memcmp(pack.data() + at, custom->data() + at, 16) != 0;
                        continue;
                    }
                    for (int t = 0; t < 16; ++t) {
                        std::array<uint8_t, 4> expected = original->Texel(bx * 4 + (t & 3), by * 4 + (t >> 2));
                        shader.Apply(expected);
                        const std::array<uint8_t, 4> got = served->Texel(bx * 4 + (t & 3), by * 4 + (t >> 2));
                        for (int c = 0; c < 4; ++c) error += std::abs(int(got[c]) - int(expected[c]));
                        samples += 4;
                    }
                }
            }
            CHECK(outside == 0 && samples > 0 && error / double(samples) < 4.0);
        }
    }
    // The band is the shield's strongly blue paint; its soft edge blends the two parts.
    CHECK(AccentWeight({40, 90, 230, 255}) == 1.0f && AccentWeight({120, 118, 100, 255}) == 0.0f && AccentWeight({40, 90, 230, 0}) == 0.0f);
    const float edge = AccentWeight({100, 110, 150, 255});
    CHECK(edge > 0.0f && edge < 1.0f);
}

void TestDwarfParts(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kDwarf;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(select && art.Regions(klass));
    if (!select || !art.Regions(klass)) return;
    const PartRender r{art, klass, *select};
    std::vector<std::string> keys;
    for (const CustomPart& part : CustomPartsOf(klass)) keys.emplace_back(part.key);
    CHECK((keys == std::vector<std::string>{"beard", "cape", "skin", "helmet", "eye", "feather", "wrist_guards", "belt", "boots", "hammers"}));
    const auto part = [&](const char* key) { return FindCustomPart(klass, key); };
    CHECK(part("beard")->label == "Beard" && part("cape")->label == "Cape" && part("feather")->label == "Feather");
    // The helmet and its feathers are parts of their own, hidden like any other.
    CHECK(part("helmet") && part("feather") && part("helmet")->bare_underlay == "skin" && part("feather")->bare_underlay.empty());
    // Piece sets are disjoint but for the eye, painted on the helmet's own pieces and told apart by being alike in every
    // preset while the helmet is recoloured.
    CHECK(part("eye")->label == "Eye" && part("eye")->mark == PaintedMark::kSteady);
    for (uint32_t id = 0; id < 1000; ++id) {
        size_t owners = 0;
        for (const CustomPart& candidate : CustomPartsOf(klass)) owners += candidate.OwnsPiece(id) && &candidate != part("eye");
        CHECK(owners <= 1 && (!part("eye")->OwnsPiece(id) || part("helmet")->OwnsPiece(id)));
    }

    // Representative select pieces resolve to their semantic part where the figure is clicked.
    const SpriteImage native0 = *art.Preview(klass, 0);
    const auto share = [&](std::initializer_list<uint32_t> ids, const CustomPart* expected) {
        size_t pixels = 0, hits = 0;
        for (size_t p = 0; p < native0.layer.size(); ++p) {
            if (!r.Visible(native0, p) || std::find(ids.begin(), ids.end(), r.IdAt(native0, p)) == ids.end()) continue;
            ++pixels;
            hits += r.PartOf(native0, p) == expected;
        }
        return std::pair{pixels, hits};
    };
    const auto all_of = [&](std::initializer_list<uint32_t> ids, const char* key, size_t percent) {
        const auto [pixels, hits] = share(ids, part(key));
        CHECK(pixels > 200 && hits * 100 >= pixels * percent);
    };
    all_of({0}, "beard", 99);
    all_of({1, 3}, "feather", 99);
    all_of({800, 810}, "cape", 99);
    all_of({120, 122, 123, 124}, "belt", 99);
    all_of({135, 141}, "boots", 80);
    all_of({1000, 1005}, "hammers", 99);
    CHECK(share({2}, part("helmet")).second > 300 && share({2}, part("skin")).second > 50); // helmet and face, one piece
    CHECK(share({106, 161}, part("wrist_guards")).second > 1000);

    const std::vector<RegionMap>& maps = *art.Regions(klass);
    const auto unmoved = [](const PartEffect& e) { return e.pixels > 0 && e.moved * 100 <= e.pixels * 5; };
    const auto drawn = [&](const SpriteImage& image, const CustomPart* owner) {
        size_t n = 0;
        for (size_t p = 0; p < image.layer.size(); ++p) n += r.Visible(image, p) && r.PartOf(image, p) == owner;
        return n;
    };
    for (int color = 0; color < kNativeColorCount; ++color) {
        const SpriteImage native = *art.Preview(klass, color);
        // Each colour reaches its own part and no other. Soft fur and fringe drawn over a neighbour blend into its pixels,
        // and a neighbour filters with the part along their edge, so a neighbour pixel counts only away from the part's rim
        // and if hiding the part leaves it unchanged.
        for (const CustomPart& painted : CustomPartsOf(klass)) {
            const std::string key(painted.key);
            const SpriteImage after = *art.CustomPreview(klass, color, Painted(color, {key.c_str()}, {0, 255, 64}));
            const PartEffect own = EffectOn(r, native, after, &painted);
            CHECK(own.pixels > 20 && own.moved * 100 >= own.pixels * 85 && own.removed == 0);
            const SpriteImage without = *art.CustomPreview(klass, color, Hidden(color, {key.c_str()}));
            std::vector<uint8_t> rim(native.layer.size(), 0);
            for (size_t p = 0; p < native.layer.size(); ++p) {
                if (!r.Visible(native, p) || r.PartOf(native, p) != &painted) continue;
                const int x = int(p % size_t(native.width)), y = int(p / size_t(native.width));
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (x + dx >= 0 && y + dy >= 0 && x + dx < native.width && y + dy < native.height) {
                            rim[size_t(y + dy) * size_t(native.width) + size_t(x + dx)] = 1;
                        }
                    }
                }
            }
            for (const CustomPart& other : CustomPartsOf(klass)) {
                if (&other == &painted) continue;
                size_t pixels = 0, leaked = 0;
                for (size_t p = 0; p < native.layer.size(); ++p) {
                    if (!r.Visible(native, p) || r.PartOf(native, p) != &other) continue;
                    ++pixels;
                    leaked += !rim[p] && PartRender::Delta(native, after, p) > 24 && PartRender::Delta(native, without, p) <= 2;
                }
                CHECK(pixels > 0 && leaked * 100 <= pixels);
                // Hiding one part removes nothing of another; the skin lies under every translucent fringe, so its own
                // hiding is checked texel by texel on the atlas below.
                const PartEffect kept = EffectOn(r, native, without, &other);
                CHECK(painted.key == "skin" || kept.removed * 100 <= kept.pixels * 2 + 2);
            }
            CHECK(Damage(r, native, without, [](uint32_t, size_t) { return true; }).created == 0);
        }
        // Hiding a part takes its pixels away and paints nothing in their place (the helmet leaves the bare head, checked
        // below).
        for (const char* key : {"beard", "cape", "feather", "hammers", "belt", "wrist_guards", "boots"}) {
            const SpriteImage after = *art.CustomPreview(klass, color, Hidden(color, {key}));
            CHECK(drawn(after, part(key)) * 50 <= drawn(native, part(key)) + 50);
        }
        // The eye is the helmet's opening: helmet and headgear hidden keep it, its colour and its hiding touch it alone.
        const PartEffect eye_native = EffectOn(r, native, native, part("eye"));
        CHECK(eye_native.pixels > 20);
        for (const SpriteImage* gone : {art.CustomPreview(klass, color, Hidden(color, {"helmet"})), art.CustomPreview(klass, color, Hidden(color, {"helmet", "feather"}))}) {
            const PartEffect kept = EffectOn(r, native, *gone, part("eye"));
            CHECK(kept.removed * 10 <= kept.pixels && kept.moved * 10 <= kept.pixels * 2); // only its rim shared with the helmet
        }
        // Hidden, the eye is gone from view: the beard behind the head shows, in some presets close to the eye's grey.
        const SpriteImage no_eye = *art.CustomPreview(klass, color, Hidden(color, {"eye"}));
        size_t eye_pixels = 0, eye_changed = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            if (!r.Visible(native, p) || r.PartOf(native, p) != part("eye")) continue;
            ++eye_pixels;
            eye_changed += PartRender::Delta(native, no_eye, p) > 2;
        }
        CHECK(eye_pixels > 20 && eye_changed * 10 >= eye_pixels * 9);
        const PartEffect helmet_left = EffectOn(r, native, *art.CustomPreview(klass, color, Hidden(color, {"eye"})), part("helmet"));
        CHECK(helmet_left.removed * 100 <= helmet_left.pixels * 3);
        // Feather hidden keeps the helmet; the helmet hidden keeps the feathers and the face, and shows the bare head.
        CHECK(unmoved(EffectOn(r, native, *art.CustomPreview(klass, color, Hidden(color, {"feather"})), part("helmet"))));
        const SpriteImage bare = *art.CustomPreview(klass, color, Hidden(color, {"helmet"}));
        CHECK(unmoved(EffectOn(r, native, bare, part("feather"))));
        const PartEffect face = EffectOn(r, native, bare, part("skin"));
        CHECK(face.removed * 100 <= face.pixels * 2 + 2);
        // Headgear hidden: the feathers go and the helmet's place is painted as a bare head of skin, every pixel kept; the
        // eye, the beard, the cape and the rest stay. Hiding the helmet and the feathers one by one does the same.
        CustomLook headgear{true, uint8_t(color), {}};
        SetHeadgear(klass, headgear, true);
        const SpriteImage bald = *art.CustomPreview(klass, color, headgear);
        const PartEffect head = EffectOn(r, native, bald, part("helmet"));
        CHECK(head.pixels > 100 && head.removed * 100 <= head.pixels * 3 && head.moved * 100 >= head.pixels * 50);
        CHECK(drawn(bald, part("feather")) == 0 && bald.rgba == art.CustomPreview(klass, color, Hidden(color, {"helmet", "feather"}))->rgba);
        CustomLook tinted = headgear;
        tinted.SetPartColor("skin", {40, 120, 220});
        const SpriteImage blue = *art.CustomPreview(klass, color, tinted);
        size_t blue_head = 0, head_pixels = 0;
        for (size_t p = 0; p < native.layer.size(); ++p) {
            if (!r.Visible(native, p) || r.PartOf(native, p) != part("helmet")) continue;
            ++head_pixels;
            blue_head += blue.rgba[p * 4 + 2] > blue.rgba[p * 4] + 40;
        }
        CHECK(head_pixels > 0 && blue_head * 100 >= head_pixels * 80); // the bare head is the skin's, in the skin's colour
        for (const char* key : {"beard", "cape", "skin", "wrist_guards", "belt", "boots", "hammers"}) {
            const PartEffect e = EffectOn(r, native, bald, part(key));
            CHECK(e.removed * 100 <= e.pixels * 2 + 2);
        }
        CHECK(unmoved(EffectOn(r, native, bald, part("beard"))) && unmoved(EffectOn(r, native, bald, part("cape"))));
        // Beard hidden keeps the face and the helmet; with the headgear too, a whole bare head with its eye is left.
        const SpriteImage no_beard = *art.CustomPreview(klass, color, Hidden(color, {"beard"}));
        const PartEffect helmet_kept = EffectOn(r, native, no_beard, part("helmet"));
        CHECK(drawn(no_beard, part("helmet")) > 1000 && helmet_kept.removed * 50 <= helmet_kept.pixels && helmet_kept.moved * 100 <= helmet_kept.pixels * 6);
        CustomLook both = headgear;
        both.SetPartHidden("beard", true);
        const SpriteImage bare_head = *art.CustomPreview(klass, color, both);
        CHECK(drawn(bare_head, part("beard")) == 0 && drawn(bare_head, part("feather")) == 0 && drawn(bare_head, part("skin")) > 0);
        const PartEffect bare_eye = EffectOn(r, native, bare_head, part("eye")), bare_helmet = EffectOn(r, native, bare_head, part("helmet"));
        CHECK(bare_eye.removed * 10 <= bare_eye.pixels && bare_helmet.removed * 100 <= bare_helmet.pixels * 3);
        // The bare head does not depend on the helmet's own visibility, and hiding the feathers leaves the eye alone.
        CustomLook headgear_helmet = headgear;
        headgear_helmet.SetPartHidden("helmet", true);
        CHECK(art.CustomPreview(klass, color, headgear_helmet)->rgba == bald.rgba);
        CHECK(unmoved(EffectOn(r, native, *art.CustomPreview(klass, color, Hidden(color, {"feather"})), part("eye"))));
        // Recolouring the helmet changes only the helmet; recolouring the feathers changes only the feathers.
        for (const char* key : {"helmet", "feather"}) {
            const SpriteImage after = *art.CustomPreview(klass, color, Painted(color, {key}, {230, 40, 40}));
            for (const CustomPart& other : CustomPartsOf(klass)) {
                if (other.key == key || other.key == "skin" || other.key == "beard") continue; // soft rims over the face and fur
                CHECK(unmoved(EffectOn(r, native, after, &other)) || EffectOn(r, native, after, &other).pixels == 0);
            }
        }
    }

    // Hashes of the four head looks, kept with the test output.
    const SpriteImage native = *art.Preview(klass, 0);
    CustomLook headgear{true, 0, {}};
    SetHeadgear(klass, headgear, true);
    CustomLook both = headgear;
    both.SetPartHidden("beard", true);
    const std::vector<std::pair<std::string, CustomLook>> judged = {
        {"native", CustomLook{true, 0, {}}}, {"headgear-hidden", headgear}, {"beard-hidden", Hidden(0, {"beard"})}, {"headgear-beard-hidden", both}};
    std::set<uint64_t> hashes;
    std::string record;
    for (const auto& [name, look] : judged) {
        const uint64_t hash = ImageHash(*art.CustomPreview(klass, 0, look));
        art.CustomPreview(klass, 1, Hidden(1, {"cape"}));
        CHECK(ImageHash(*art.CustomPreview(klass, 0, look)) == hash); // the same look draws the same pixels
        hashes.insert(hash);
        char line[96];
        std::snprintf(line, sizeof(line), "Dwarf %s %016llx\n", name.c_str(), static_cast<unsigned long long>(hash));
        record += line;
    }
    CHECK(hashes.size() == judged.size() && hashes.count(ImageHash(native)) == 1);
    std::printf("%s", record.c_str());
    std::error_code ec;
    const fs::path hash_file = fs::temp_directory_path() / "dcp_save_editor_tests" / "dwarf_preview_hashes.txt";
    fs::create_directories(hash_file.parent_path(), ec);
    std::ofstream(hash_file, std::ios::binary | std::ios::trunc) << record;

    // On the atlas, texel by texel: each part's colour, hiding and random colour change only its own texels.
    std::mt19937 random(11);
    for (int color : {0, 2}) {
        const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, color);
        for (const CustomPart& each : CustomPartsOf(klass)) {
            if (each.weapon >= 0) continue;
            const std::string key(each.key);
            for (const CustomLook& look : {Painted(color, {key.c_str()}, {200, 40, 160}), Hidden(color, {key.c_str()})}) {
                const AtlasChange change = ChangeOnAtlas(klass, maps, atlas, look, {&each});
                CHECK(change.inside > 0 && change.inside_changed * 100 >= change.inside * 60 && change.outside_changed == 0);
            }
            CustomLook randomized{true, uint8_t(color), {}};
            randomized.SetPartColor(key, NextRandomColor(random, std::nullopt));
            CHECK(ChangeOnAtlas(klass, maps, atlas, randomized, {&each}).outside_changed == 0);
        }
        CustomLook bald{true, uint8_t(color), {}};
        SetHeadgear(klass, bald, true);
        const AtlasChange head = ChangeOnAtlas(klass, maps, atlas, bald, {part("helmet"), part("feather")});
        CHECK(head.inside > 0 && head.outside_changed == 0);
        // Wrist guards, belt and boots hidden are cleared texel for texel, never painted as skin; the skin on the same
        // pieces stays. The helmet hidden keeps every texel, painted as the bare head.
        for (const char* key : {"wrist_guards", "belt", "boots"}) {
            const AtlasChange gear = ChangeOnAtlas(klass, maps, atlas, Hidden(color, {key}), {part(key)});
            CHECK(gear.inside > 0 && gear.inside_cleared == gear.inside && gear.outside_changed == 0);
        }
        const AtlasChange helmet = ChangeOnAtlas(klass, maps, atlas, Hidden(color, {"helmet"}), {part("helmet")});
        CHECK(helmet.inside > 0 && helmet.inside_cleared == 0 && helmet.inside_changed * 10 >= helmet.inside * 9 &&
              helmet.outside_changed == 0);
    }

    // All Colors Random gives every part a colour and keeps the base and the visibility.
    CustomLook all{true, 4, {}};
    all.SetPartHidden("beard", true);
    all.RandomizeColors(klass, random);
    CHECK(all.base == 4 && all.Find("beard")->hidden && !all.Find("helmet")->hidden);
    for (const CustomPart& each : CustomPartsOf(klass)) CHECK(all.Find(each.key) && all.Find(each.key)->color);

    // In game: the served atlas draws the preview's pixels.
    CheckServedLook(art, cpk, klass, *select, both);
    CustomLook gear_off = Hidden(0, {"wrist_guards", "belt", "boots"});
    gear_off.SetPartColor("feather", {200, 60, 200});
    CheckServedLook(art, cpk, klass, *select, gear_off);
    CustomLook bare_head{true, 2, {}};
    SetHeadgear(klass, bare_head, true);
    bare_head.SetPartHidden("helmet", true);
    CheckServedLook(art, cpk, klass, *select, bare_head);

    // In game: every quad of the beard, the feathers and the cape, in any pose, is cleared by hiding the part.
    const std::optional<Mbs> game = ParseMbs(cpk.Read(*cpk.Find(GameModelName(klass))));
    CHECK(game.has_value());
    if (!game) return;
    const std::span<const BodySurface> bodies = BodySurfacesOf(klass);
    const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, 0);
    for (const char* key : {"beard", "feather", "cape"}) {
        size_t quads = 0, left = 0;
        CustomLook look = Hidden(0, {key});
        for (size_t b = 0; b < bodies.size(); ++b) {
            const RegionMap& map = maps[b];
            const LookShader shader(klass, look, map, [&](int x, int y) { return atlas.surfaces[b].Texel(x, y); });
            for (const MbsLayer& layer : game->layers) {
                if (layer.texture != bodies[b].texture || !part(key)->OwnsPiece(layer.id)) continue;
                const MbsQuad& q = game->sources[layer.source];
                if (q.center.x < 0.0f || q.center.x > 1.0f || q.center.y < 0.0f || q.center.y > 1.0f) continue;
                const int x = int(q.center.x * float(map.width)), y = int(q.center.y * float(map.height));
                std::array<uint8_t, 4> texel = atlas.surfaces[b].Texel(x, y);
                if (texel[3] < 64 || OwnerOfTexel(klass, map, x, y) != part(key)) continue;
                ++quads;
                shader.ApplyAt(x, y, texel);
                left += texel[3] != 0;
            }
        }
        CHECK(quads > 10 && left == 0);
    }
    // Gameplay poses: the headgear and beard hidden take the helmet away, leave no feather and draw nothing new.
    const std::set<uint32_t> wings(part("feather")->pieces.begin(), part("feather")->pieces.end());
    const std::set<uint32_t> helmets(part("helmet")->pieces.begin(), part("helmet")->pieces.end());
    size_t head = 0, head_changed = 0, feathers = 0, created = 0;
    for (const auto& [animation, frame] : {std::pair{"IDLE", 0}, std::pair{"MOVE_2", 4}, std::pair{"RUN_ATTACK", 3}, std::pair{"BLOWDOWN_3", 2}}) {
        const SpriteImage before = RenderGamePose(cpk, maps, klass, *game, animation, size_t(frame), CustomLook{true, 0, {}});
        const SpriteImage after = RenderGamePose(cpk, maps, klass, *game, animation, size_t(frame), both);
        for (size_t p = 0; p < after.layer.size() && p < before.layer.size(); ++p) {
            if (after.layer[p] != kNoLayer && after.rgba[p * 4 + 3] >= 64 && wings.count(game->layers[after.layer[p]].id)) ++feathers;
            created += after.rgba[p * 4 + 3] > before.rgba[p * 4 + 3] + 8;
            if (before.layer[p] == kNoLayer || before.rgba[p * 4 + 3] < 128 || !helmets.count(game->layers[before.layer[p]].id)) continue;
            ++head;
            head_changed += PartRender::Delta(before, after, p) > 24;
        }
    }
    CHECK(head > 500 && head_changed * 100 >= head * 50 && feathers == 0 && created == 0);
}

// Dwarf: the headgear and the helmet leave the bare head with its face, eye and beard; the gear goes transparent.
void TestDwarfHeadAndGear(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kDwarf;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(select && art.Regions(klass));
    if (!select || !art.Regions(klass)) return;
    const PartRender r{art, klass, *select};
    const auto part = [&](const char* key) { return FindCustomPart(klass, key); };
    for (int color = 0; color < kNativeColorCount; ++color) {
        const SpriteImage native = *art.Preview(klass, color);
        // A part's pixels with no other part within `reach` of them: what the preview draws of it alone.
        const auto core_of = [&](const CustomPart* owner, int reach = 2) {
            std::vector<size_t> core;
            for (size_t p = 0; p < native.layer.size(); ++p) {
                if (!r.Visible(native, p) || r.PartOf(native, p) != owner) continue;
                const int x = int(p % size_t(native.width)), y = int(p / size_t(native.width));
                bool alone = true;
                for (int dy = -reach; dy <= reach && alone; ++dy) {
                    for (int dx = -reach; dx <= reach && alone; ++dx) {
                        if (x + dx < 0 || y + dy < 0 || x + dx >= native.width || y + dy >= native.height) continue;
                        const size_t beside = size_t(y + dy) * size_t(native.width) + size_t(x + dx);
                        alone = !r.Visible(native, beside) || r.PartOf(native, beside) == owner;
                    }
                }
                if (alone) core.push_back(p);
            }
            return core;
        };
        const auto changed = [&](const SpriteImage& after, const std::vector<size_t>& core) {
            return size_t(std::count_if(core.begin(), core.end(), [&](size_t p) { return PartRender::Delta(native, after, p) > 2; }));
        };
        const auto moved = [&](const SpriteImage& after, const std::vector<size_t>& core) {
            return size_t(std::count_if(core.begin(), core.end(), [&](size_t p) { return PartRender::Delta(native, after, p) > 24; }));
        };
        const auto as_before = [&](const SpriteImage& after, const std::vector<size_t>& core) {
            return size_t(std::count_if(core.begin(), core.end(), [&](size_t p) { return r.Visible(after, p) && PartRender::Delta(native, after, p) <= 8; }));
        };
        const auto still_drawn = [&](const SpriteImage& after, const CustomPart* owner) {
            size_t pixels = 0;
            for (size_t p = 0; p < after.layer.size(); ++p) pixels += r.Visible(after, p) && r.PartOf(after, p) == owner;
            return pixels;
        };
        const auto cleared = [&](const SpriteImage& after, const std::vector<size_t>& core) {
            return size_t(std::count_if(core.begin(), core.end(), [&](size_t p) { return int(after.rgba[p * 4 + 3]) + 8 < int(native.rgba[p * 4 + 3]); }));
        };
        const auto see_through = [&](const SpriteImage& after, const std::vector<size_t>& core) {
            return size_t(std::count_if(core.begin(), core.end(), [&](size_t p) {
                return int(after.rgba[p * 4 + 3]) + 8 < int(native.rgba[p * 4 + 3]) || r.IdAt(after, p) != r.IdAt(native, p);
            }));
        };
        const std::vector<size_t> helmet = core_of(part("helmet")), feather = core_of(part("feather")), eye = core_of(part("eye"), 1),
                                  beard = core_of(part("beard")), skin = core_of(part("skin"));
        CHECK(helmet.size() > 100 && feather.size() > 100 && eye.size() > 20 && beard.size() > 100 && skin.size() > 100);

        CustomLook headgear{true, uint8_t(color), {}};
        SetHeadgear(klass, headgear, true);
        CustomLook headgear_helmet = headgear;
        headgear_helmet.SetPartHidden("helmet", true);
        for (const CustomLook& look : {headgear, headgear_helmet, Hidden(color, {"helmet"})}) {
            const SpriteImage after = *art.CustomPreview(klass, color, look);
            // The helmet is gone from view, its place kept as the bare head; the face, the eye and the beard stay (the fur's
            // soft edge over the head shows the head through it).
            CHECK(as_before(after, helmet) * 100 <= helmet.size() && cleared(after, helmet) == 0);
            CHECK(changed(after, eye) == 0 && changed(after, skin) == 0 && cleared(after, beard) == 0 && moved(after, beard) * 50 <= beard.size());
            // The feathers go with the headgear and stay with the helmet alone.
            if (HeadgearHidden(klass, look)) CHECK(still_drawn(after, part("feather")) == 0 && cleared(after, feather) * 10 >= feather.size() * 9);
            else CHECK(changed(after, feather) == 0);
        }
        const SpriteImage no_feather = *art.CustomPreview(klass, color, Hidden(color, {"feather"}));
        CHECK(still_drawn(no_feather, part("feather")) == 0 && changed(no_feather, helmet) == 0 && changed(no_feather, eye) == 0);

        // Hidden gear lets what lies behind it show, never a fill painted over its place.
        for (const char* key : {"wrist_guards", "belt", "boots", "hammers"}) {
            const std::vector<size_t> gear = core_of(part(key));
            const SpriteImage after = *art.CustomPreview(klass, color, Hidden(color, {key}));
            CHECK(gear.size() > 50 && see_through(after, gear) == gear.size() && moved(after, skin) * 1000 <= skin.size()); // soft edges
        }
    }
    CustomLook bald{true, 1, {}};
    SetHeadgear(klass, bald, true);
    CheckServedLook(art, cpk, klass, *select, bald);
    CheckServedLook(art, cpk, klass, *select, Hidden(3, {"helmet"}));
    CheckServedLook(art, cpk, klass, *select, Hidden(4, {"wrist_guards", "belt", "boots"}));
}

void TestWizardParts(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kWizard;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(select && art.Regions(klass));
    if (!select || !art.Regions(klass)) return;
    const PartRender r{art, klass, *select};
    std::vector<std::string> keys;
    for (const CustomPart& part : CustomPartsOf(klass)) keys.emplace_back(part.key);
    CHECK((keys == std::vector<std::string>{"robe", "hood", "pendant", "hair", "skin", "gloves", "belt", "trousers", "boots", "staff"}));
    const auto part = [&](const char* key) { return FindCustomPart(klass, key); };
    // The scattered pale texels once called Trim are the robe's piping and highlights; the pendant is a piece of its own.
    for (const char* gone : {"cloak", "accents", "trim"}) CHECK(part(gone) == nullptr);
    CHECK(part("pendant")->label == "Pendant" && part("pendant")->OwnsPiece(1503) && !part("robe")->OwnsPiece(1503));
    // Feet are the boots' whole; a leg piece gives its white to the trousers.
    for (const uint32_t foot : {2002u, 2005u, 2006u, 2079u}) CHECK(!part("trousers")->OwnsPiece(foot) && part("boots")->OwnsPiece(foot));
    for (const uint32_t leg : {2000u, 2001u, 2004u}) CHECK(part("trousers")->OwnsPiece(leg));

    // Representative select pieces resolve to their semantic part where the figure is clicked.
    const SpriteImage native0 = *art.Preview(klass, 0);
    const auto all_of = [&](std::initializer_list<uint32_t> ids, const char* key, size_t percent) {
        size_t pixels = 0, hits = 0;
        for (size_t p = 0; p < native0.layer.size(); ++p) {
            if (!r.Visible(native0, p) || std::find(ids.begin(), ids.end(), r.IdAt(native0, p)) == ids.end()) continue;
            ++pixels;
            hits += r.PartOf(native0, p) == part(key);
        }
        CHECK(pixels > 50 && hits * 100 >= pixels * percent);
    };
    all_of({200, 208}, "hair", 99);
    all_of({0}, "skin", 70);
    all_of({1507, 1588}, "gloves", 99);
    all_of({1001}, "belt", 95);
    all_of({2002, 2005}, "boots", 99);
    all_of({2000}, "trousers", 99);
    all_of({2512, 2518, 1000}, "robe", 99);
    all_of({1503}, "pendant", 99);
    all_of({5013}, "staff", 99);

    const auto drawn = [&](const SpriteImage& image, const CustomPart* owner) {
        size_t n = 0;
        for (size_t p = 0; p < image.layer.size(); ++p) n += r.Visible(image, p) && r.PartOf(image, p) == owner;
        return n;
    };
    for (int color = 0; color < kNativeColorCount; ++color) {
        const SpriteImage native = *art.Preview(klass, color);
        // Pairwise: a strong colour on A moves A and no pixel of any other part B; hiding A removes A and nothing of B.
        // A neighbour filtered with A along their edge, or drawn translucent over it, blends with it: a neighbour pixel
        // counts only away from A's rim and where hiding A leaves it unchanged.
        for (const CustomPart& painted : CustomPartsOf(klass)) {
            const std::string key(painted.key);
            const SpriteImage after = *art.CustomPreview(klass, color, Painted(color, {key.c_str()}, {0, 255, 64}));
            const SpriteImage without = *art.CustomPreview(klass, color, Hidden(color, {key.c_str()}));
            const PartEffect own = EffectOn(r, native, after, &painted);
            CHECK(own.pixels > 10 && own.moved * 100 >= own.pixels * 80 && own.removed == 0);
            const PartEffect gone = EffectOn(r, native, without, &painted);
            CHECK(drawn(without, &painted) * 50 <= gone.pixels + 50 && Damage(r, native, without, [](uint32_t, size_t) { return true; }).created == 0);
            std::vector<uint8_t> rim(native.layer.size(), 0);
            for (size_t p = 0; p < native.layer.size(); ++p) {
                if (!r.Visible(native, p) || r.PartOf(native, p) != &painted) continue;
                const int x = int(p % size_t(native.width)), y = int(p / size_t(native.width));
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (x + dx >= 0 && y + dy >= 0 && x + dx < native.width && y + dy < native.height) {
                            rim[size_t(y + dy) * size_t(native.width) + size_t(x + dx)] = 1;
                        }
                    }
                }
            }
            for (const CustomPart& other : CustomPartsOf(klass)) {
                if (&other == &painted) continue;
                size_t pixels = 0, leaked = 0, removed = 0;
                for (size_t p = 0; p < native.layer.size(); ++p) {
                    if (!r.Visible(native, p) || r.PartOf(native, p) != &other) continue;
                    ++pixels;
                    const bool blended = rim[p] || PartRender::Delta(native, without, p) > 2;
                    leaked += !blended && PartRender::Delta(native, after, p) > 24;
                    removed += !rim[p] && int(without.rgba[p * 4 + 3]) + 8 < int(native.rgba[p * 4 + 3]);
                }
                CHECK(pixels > 0 && leaked * 100 <= pixels && removed * 100 <= pixels);
            }
        }
        // Trousers and boots share leg pieces and nothing else: each colour and each hiding leaves the other native.
        const SpriteImage red_trousers = *art.CustomPreview(klass, color, Painted(color, {"trousers"}, {230, 20, 20}));
        const SpriteImage blue_boots = *art.CustomPreview(klass, color, Painted(color, {"boots"}, {20, 40, 230}));
        const SpriteImage no_trousers = *art.CustomPreview(klass, color, Hidden(color, {"trousers"}));
        const SpriteImage no_boots = *art.CustomPreview(klass, color, Hidden(color, {"boots"}));
        const auto untouched = [&](const SpriteImage& after, const char* key) {
            size_t pixels = 0, moved = 0;
            for (size_t p = 0; p < native.layer.size(); ++p) {
                if (!r.Visible(native, p) || r.PartOf(native, p) != part(key)) continue;
                ++pixels;
                moved += PartRender::Delta(native, after, p) > 24;
            }
            return pixels > 100 && moved * 100 <= pixels * 2; // their shared seam only
        };
        CHECK(untouched(red_trousers, "boots") && untouched(no_trousers, "boots"));
        CHECK(untouched(blue_boots, "trousers") && untouched(no_boots, "trousers"));
        // Hair hidden leaves the face whole.
        const SpriteImage bald = *art.CustomPreview(klass, color, Hidden(color, {"hair"}));
        CHECK(EffectOn(r, native, bald, part("skin")).removed * 100 <= EffectOn(r, native, bald, part("skin")).pixels * 3);
        // Headgear: the hood goes; face, hair, gloves, staff and the robe's body stay.
        CustomLook hood{true, uint8_t(color), {}};
        SetHeadgear(klass, hood, true);
        const SpriteImage no_hood = *art.CustomPreview(klass, color, hood);
        CHECK(no_hood.rgba != native.rgba);
        for (const char* key : {"skin", "hair", "gloves", "belt", "boots", "staff"}) {
            const PartEffect e = EffectOn(r, native, no_hood, part(key));
            CHECK(e.pixels > 0 && e.removed * 100 <= e.pixels * 2);
        }
        CHECK(drawn(no_hood, part("robe")) * 10 >= drawn(native, part("robe")) * 9);
    }

    // On the atlas the game is served, texel by texel and in every preset: A's colour and A's hiding change A's texels only.
    const std::vector<RegionMap>& maps = *art.Regions(klass);
    for (int color = 0; color < kNativeColorCount; ++color) {
        const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, color);
        for (const CustomPart& each : CustomPartsOf(klass)) {
            if (each.weapon >= 0) continue;
            const std::string key(each.key);
            for (const CustomLook& look : {Painted(color, {key.c_str()}, {0, 255, 64}), Hidden(color, {key.c_str()})}) {
                const AtlasChange change = ChangeOnAtlas(klass, maps, atlas, look, {&each});
                CHECK(change.inside > 0 && change.inside_changed * 100 >= change.inside * 90 && change.outside_changed == 0);
            }
        }
    }
    // The staff is its own texture: no body look touches it, and its look touches no body texel.
    const std::vector<uint8_t> staff_pack = cpk.Read(*cpk.Find(WeaponAtlasName(klass, WeaponTexturesOf(klass)[0])));
    CustomLook body_only = Hidden(0, {"robe", "pendant", "hair", "skin", "gloves", "belt", "trousers", "boots"});
    CHECK(CustomizeWeapon(staff_pack, klass, 0, body_only) == staff_pack);
    CHECK(ChangeOnAtlas(klass, maps, ReadBodyAtlas(cpk, klass, 0), Painted(0, {"staff"}), {}).outside_changed == 0);

    // All Colors Random: every part its own colour; base, visibility and headgear kept; any part still hides after it.
    CustomLook all{true, 2, {}};
    SetHeadgear(klass, all, true);
    all.SetPartHidden("belt", true);
    std::mt19937 random(21);
    all.RandomizeColors(klass, random);
    CHECK(all.base == 2 && HeadgearHidden(klass, all) && all.Find("belt")->hidden && !all.Find("robe")->hidden);
    std::set<std::tuple<int, int, int>> colors;
    for (const CustomPart& each : CustomPartsOf(klass)) {
        CHECK(all.Find(each.key) && all.Find(each.key)->color);
        if (all.Find(each.key) && all.Find(each.key)->color) colors.insert({all.Find(each.key)->color->r, all.Find(each.key)->color->g, all.Find(each.key)->color->b});
    }
    CHECK(colors.size() == CustomPartsOf(klass).size());
    for (const CustomPart& each : CustomPartsOf(klass)) {
        CustomLook hidden = all;
        hidden.SetPartHidden(each.key, true);
        CHECK(drawn(*art.CustomPreview(klass, 2, hidden), &each) * 50 <= drawn(native0, &each) + 50);
    }

    // In game: the served atlas draws the preview's pixels.
    CustomLook mixed = Hidden(0, {"robe", "hair"});
    mixed.SetPartColor("trousers", {230, 20, 20});
    mixed.SetPartColor("boots", {20, 40, 230});
    mixed.SetPartColor("pendant", {240, 200, 30});
    CheckServedLook(art, cpk, klass, *select, mixed);

    // Gameplay poses: the trousers' colour never reaches a foot, the boots' never a trouser piece; hiding the hair clears
    // every hair quad; spell effects keep their colours under any look.
    const std::optional<Mbs> game = ParseMbs(cpk.Read(*cpk.Find(GameModelName(klass))));
    CHECK(game.has_value());
    if (!game) return;
    const std::set<uint32_t> legs(part("trousers")->pieces.begin(), part("trousers")->pieces.end());
    size_t foot_pixels = 0, foot_moved = 0, trouser_pixels = 0, trouser_moved = 0;
    for (const auto& [animation, frame] : {std::pair{"IDLE", 0}, std::pair{"MOVE_2", 4}, std::pair{"RUN_ATTACK", 3}}) {
        const SpriteImage before = RenderGamePose(cpk, maps, klass, *game, animation, size_t(frame), CustomLook{true, 0, {}});
        const SpriteImage reds = RenderGamePose(cpk, maps, klass, *game, animation, size_t(frame), Painted(0, {"trousers"}, {230, 20, 20}));
        const SpriteImage blues = RenderGamePose(cpk, maps, klass, *game, animation, size_t(frame), Painted(0, {"boots"}, {20, 40, 230}));
        for (size_t p = 0; p < before.layer.size(); ++p) {
            if (before.layer[p] == kNoLayer || before.rgba[p * 4 + 3] < 128 || before.texture[p] != 0) continue;
            const uint32_t id = game->layers[before.layer[p]].id;
            if (id < 2000 || id > 2099) continue;
            if (!legs.count(id)) {
                ++foot_pixels;
                foot_moved += PartRender::Delta(before, reds, p) > 24;
            } else if (PartRender::Delta(before, reds, p) > 24) {
                ++trouser_pixels;
                trouser_moved += PartRender::Delta(before, blues, p) > 24;
            }
        }
    }
    CHECK(foot_pixels > 200 && foot_moved * 100 <= foot_pixels * 2 && trouser_pixels > 200 && trouser_moved * 100 <= trouser_pixels * 2);
    const RegionMap& map = maps.front();
    const FtxSurface atlas = *ReadFtxSurface(cpk.Read(*cpk.Find(AtlasName(klass, 0))), 0);
    const LookShader hairless(klass, Hidden(0, {"hair"}), map, [&](int x, int y) { return atlas.Texel(x, y); });
    CustomLook every{true, 0, {}};
    std::mt19937 seeded(5);
    every.RandomizeColors(klass, seeded);
    const LookShader painted(klass, every, map, [&](int x, int y) { return atlas.Texel(x, y); });
    size_t hair_quads = 0, hair_left = 0, effect_quads = 0, effect_changed = 0;
    for (const MbsLayer& layer : game->layers) {
        if (layer.texture != 0) continue;
        const MbsQuad& q = game->sources[layer.source];
        if (q.center.x < 0.0f || q.center.x > 1.0f || q.center.y < 0.0f || q.center.y > 1.0f) continue;
        const int x = int(q.center.x * float(map.width)), y = int(q.center.y * float(map.height));
        std::array<uint8_t, 4> texel = atlas.Texel(x, y);
        if (texel[3] < 64) continue;
        if (layer.id >= 200 && layer.id < 300) {
            ++hair_quads;
            hairless.ApplyAt(x, y, texel);
            hair_left += texel[3] != 0;
        } else if (layer.id >= 1200 && layer.id <= 1222) {
            ++effect_quads;
            const std::array<uint8_t, 4> before = texel;
            painted.ApplyAt(x, y, texel);
            effect_changed += texel != before;
        }
    }
    CHECK(hair_quads > 100 && hair_left == 0);
    CHECK(effect_quads > 10 && effect_changed == 0);
}

void TestElfParts(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kElf;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(select && art.Regions(klass));
    if (!select || !art.Regions(klass)) return;
    const PartRender r{art, klass, *select};
    std::vector<std::string> keys;
    for (const CustomPart& part : CustomPartsOf(klass)) keys.emplace_back(part.key);
    CHECK((keys == std::vector<std::string>{"hood", "hair", "skin", "eye", "tunic", "belt", "collar", "shorts", "gloves", "laces", "boots",
                                            "quiver", "arrows", "bow"}));
    const auto part = [&](const char* key) { return FindCustomPart(klass, key); };
    // No vague grab-bags: Accessories and Leggings are gone, Hair and Skin are apart, Bow, Quiver and Arrows are three.
    for (const char* gone : {"accessories", "leggings"}) CHECK(part(gone) == nullptr);
    CHECK(part("eye")->label == "Eyes" && part("skin")->mark == PaintedMark::kSkinTone);

    // Representative select pieces resolve to their semantic part where the figure is clicked.
    const SpriteImage native0 = *art.Preview(klass, 0);
    const auto all_of = [&](std::initializer_list<uint32_t> ids, const char* key, size_t percent) {
        size_t pixels = 0, hits = 0;
        for (size_t p = 0; p < native0.layer.size(); ++p) {
            if (!r.Visible(native0, p) || std::find(ids.begin(), ids.end(), r.IdAt(native0, p)) == ids.end()) continue;
            ++pixels;
            hits += r.PartOf(native0, p) == part(key);
        }
        CHECK(pixels > 50 && hits * 100 >= pixels * percent);
    };
    all_of({1500, 1510, 1532}, "hood", 97);
    all_of({10, 11}, "hair", 90);
    all_of({503, 504}, "skin", 80);
    all_of({1000, 1002}, "tunic", 90);
    all_of({1003}, "belt", 80);
    all_of({500}, "collar", 99);
    all_of({505}, "gloves", 99);
    all_of({514, 518}, "boots", 70);
    all_of({3000}, "quiver", 99);
    all_of({4000, 4100, 4200, 4400}, "arrows", 99);
    all_of({5000, 5002}, "bow", 99);
    // The bare thighs between shorts and boots are skin, and the eyes are their own part.
    size_t thigh_skin = 0, eye_pixels = 0;
    for (size_t p = 0; p < native0.layer.size(); ++p) {
        if (!r.Visible(native0, p)) continue;
        const uint32_t id = r.IdAt(native0, p);
        thigh_skin += (id == 512 || id == 516) && r.PartOf(native0, p) == part("skin");
        eye_pixels += id == 0 && r.PartOf(native0, p) == part("eye");
    }
    CHECK(thigh_skin > 300 && eye_pixels > 20);

    const auto drawn = [&](const SpriteImage& image, const CustomPart* owner) {
        size_t n = 0;
        for (size_t p = 0; p < image.layer.size(); ++p) n += r.Visible(image, p) && r.PartOf(image, p) == owner;
        return n;
    };
    for (int color = 0; color < kNativeColorCount; ++color) {
        const SpriteImage native = *art.Preview(klass, color);
        for (const CustomPart& painted : CustomPartsOf(klass)) {
            const SpriteImage after =
                *art.CustomPreview(klass, color, Painted(color, {std::string(painted.key).c_str()}, {0, 255, 64}));
            const PartEffect own = EffectOn(r, native, after, &painted);
            CHECK(own.pixels > 20 && own.moved * 100 >= own.pixels * 75 && own.removed == 0);
        }
        // Hiding takes only the part away and shows the native art behind it.
        for (const char* key : {"hood", "hair", "collar", "quiver", "arrows", "bow"}) {
            const SpriteImage after = *art.CustomPreview(klass, color, Hidden(color, {key}));
            CHECK(drawn(after, part(key)) <= 2); // a filtered rim texel at most
            CHECK(Damage(r, native, after, [](uint32_t, size_t) { return true; }).created == 0);
            for (const CustomPart& other : CustomPartsOf(klass)) {
                if (&other == part(key)) continue;
                const PartEffect e = EffectOn(r, native, after, &other);
                CHECK(e.pixels == 0 || e.removed * 100 <= e.pixels * 8); // the fingers round the bow grip blend with it
            }
        }
        // Headgear: the hood goes; face, hair, eyes and the rest of the body stay.
        CustomLook hood{true, uint8_t(color), {}};
        SetHeadgear(klass, hood, true);
        const SpriteImage no_hood = *art.CustomPreview(klass, color, hood);
        CHECK(no_hood.rgba != native.rgba);
        for (const char* key : {"skin", "eye", "hair", "tunic", "boots", "bow", "quiver", "arrows"}) {
            const PartEffect e = EffectOn(r, native, no_hood, part(key));
            CHECK(e.pixels > 0 && e.removed * 100 <= e.pixels * 5); // the cape's rim shares texels with its neighbours
        }
        // Every one of the 14 parts: Hidden clears its own pixels and no other part's (away from its rim, where they
        // filter together); Shown again, or Reset Part, gives back the native preview.
        for (const CustomPart& hidden_part : CustomPartsOf(klass)) {
            const std::string key(hidden_part.key);
            const SpriteImage after = *art.CustomPreview(klass, color, Hidden(color, {key.c_str()}));
            CHECK(drawn(native, &hidden_part) > 0 && drawn(after, &hidden_part) * 20 <= drawn(native, &hidden_part) + 20);
            std::vector<uint8_t> rim(native.layer.size(), 0);
            for (size_t p = 0; p < native.layer.size(); ++p) {
                if (!r.Visible(native, p) || r.PartOf(native, p) != &hidden_part) continue;
                const int x = int(p % size_t(native.width)), y = int(p / size_t(native.width));
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (x + dx >= 0 && y + dy >= 0 && x + dx < native.width && y + dy < native.height) {
                            rim[size_t(y + dy) * size_t(native.width) + size_t(x + dx)] = 1;
                        }
                    }
                }
            }
            for (const CustomPart& other : CustomPartsOf(klass)) {
                if (&other == &hidden_part) continue;
                size_t pixels = 0, removed = 0;
                for (size_t p = 0; p < native.layer.size(); ++p) {
                    if (!r.Visible(native, p) || r.PartOf(native, p) != &other || rim[p]) continue;
                    ++pixels;
                    removed += int(after.rgba[p * 4 + 3]) + 8 < int(native.rgba[p * 4 + 3]);
                }
                CHECK(removed * 100 <= pixels * 2 + 2);
            }
            CustomLook shown = Hidden(color, {key.c_str()});
            shown.SetPartHidden(key, false);
            CHECK(art.CustomPreview(klass, color, shown)->rgba == native.rgba);
            CustomLook reset = Painted(color, {key.c_str()}, {255, 0, 128});
            reset.SetPartHidden(key, true);
            reset.ResetPart(key);
            CHECK(reset.Find(key) == nullptr && art.CustomPreview(klass, color, reset)->rgba == native.rgba);
        }
        // Belt, shorts, laces and boots leave no fragment of themselves (their rim filters with what they lie on), and each
        // leaves the core of its neighbour whole: the tunic under the belt, the laces on a hidden boot, the boot under
        // hidden laces.
        const auto core_kept = [&](const SpriteImage& after, const CustomPart* kept) {
            const size_t w = size_t(native.width);
            size_t pixels = 0, removed = 0;
            for (size_t p = w; p + w < native.layer.size(); ++p) {
                const bool core = r.Visible(native, p) && r.PartOf(native, p) == kept && r.PartOf(native, p - 1) == kept &&
                                  r.PartOf(native, p + 1) == kept && r.PartOf(native, p - w) == kept && r.PartOf(native, p + w) == kept;
                if (!core) continue;
                ++pixels;
                removed += int(after.rgba[p * 4 + 3]) + 8 < int(native.rgba[p * 4 + 3]);
            }
            return pixels > 20 && removed * 100 <= pixels * 2;
        };
        for (const auto& [gone, kept] : {std::pair{"belt", "tunic"}, std::pair{"shorts", "tunic"}, std::pair{"boots", "laces"},
                                         std::pair{"laces", "boots"}}) {
            const SpriteImage after = *art.CustomPreview(klass, color, Hidden(color, {gone}));
            CHECK(drawn(after, part(gone)) * 100 <= drawn(native, part(gone)) * 3 && core_kept(after, part(kept)));
        }
    }
    // On the atlas the game is served, in every preset: hiding a part clears exactly its own texels.
    for (int color = 0; color < kNativeColorCount; ++color) {
        const BodyAtlas atlas = ReadBodyAtlas(cpk, klass, color);
        for (const CustomPart& each : CustomPartsOf(klass)) {
            if (each.weapon >= 0) continue;
            const AtlasChange change = ChangeOnAtlas(klass, *art.Regions(klass), atlas, Hidden(color, {std::string(each.key).c_str()}), {&each});
            CHECK(change.inside > 0 && change.inside_cleared == change.inside && change.outside_changed == 0);
        }
    }
    CustomLook served = Hidden(0, {"skin", "belt", "laces"});
    served.SetPartColor("boots", {30, 200, 90});
    CheckServedLook(art, cpk, klass, *SelectModel(cpk, klass), served);

    // In the atlas, a part's colour changes no texel another part owns, on either surface it paints.
    const RegionMap& map = art.Regions(klass)->front();
    const FtxSurface base = *ReadFtxSurface(cpk.Read(*cpk.Find(AtlasName(klass, 0))), 0);
    for (const CustomPart& painted : CustomPartsOf(klass)) {
        if (painted.weapon >= 0) continue;
        const LookShader shader(klass, Painted(0, {std::string(painted.key).c_str()}, {0, 255, 64}), map,
                                [&](int x, int y) { return base.Texel(x, y); });
        size_t own = 0, foreign = 0;
        for (int y = 0; y < map.height; y += 2) {
            for (int x = 0; x < map.width; x += 2) {
                std::array<uint8_t, 4> texel = base.Texel(x, y);
                if (!texel[3]) continue;
                const std::array<uint8_t, 4> before = texel;
                shader.ApplyAt(x, y, texel);
                if (texel == before) continue;
                const uint16_t pieces = map.Pieces(x, y);
                const CustomPart* owner = nullptr;
                for (const CustomPart& candidate : CustomPartsOf(klass)) {
                    if (const uint16_t bit = PieceBitOf(klass, candidate); bit && (pieces & bit)) owner = &candidate;
                }
                if (!pieces) owner = PartOfRegion(klass, map.At(x, y));
                if (owner == &painted) ++own;
                else ++foreign;
            }
        }
        CHECK(own > 50 && foreign == 0);
    }
}

// Elf materials told apart by the game's own presets, independently of the part masks: what each base colour paints a
// texel with says which garment it is.
enum class ElfMaterial { kUnknown, kBelt, kTunic, kShorts, kSkin, kBoots, kLaces, kTrim };

struct ElfPresets {
    std::vector<BodyAtlas> atlases;
    std::array<std::array<uint8_t, 4>, kNativeColorCount> At(int x, int y) const {
        std::array<std::array<uint8_t, 4>, kNativeColorCount> texels{};
        for (size_t p = 0; p < texels.size(); ++p) texels[p] = atlases[p].surfaces[0].Texel(x, y);
        return texels;
    }
};

// Texels next to transparency or to another part are left out: the preview filters them together.
ElfMaterial ElfMaterialOf(const ElfPresets& presets, const RegionMap& map, int x, int y) {
    const FtxSurface& first = presets.atlases[0].surfaces[0];
    const CustomPart* owner = OwnerOfTexel(CharacterClass::kElf, map, x, y);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (x + dx < 0 || y + dy < 0 || x + dx >= first.width() || y + dy >= first.height()) return ElfMaterial::kUnknown;
            if (first.Texel(x + dx, y + dy)[3] < 200 || OwnerOfTexel(CharacterClass::kElf, map, x + dx, y + dy) != owner) {
                return ElfMaterial::kUnknown;
            }
        }
    }
    const auto t = presets.At(x, y);
    const auto luma = [](const std::array<uint8_t, 4>& c) { return Luma(c[0], c[1], c[2]); };
    float mean[3] = {};
    for (const auto& c : t) {
        for (size_t k = 0; k < 3; ++k) mean[k] += float(c[k]) / float(t.size());
    }
    float drift = 0.0f;
    for (const auto& c : t) {
        for (size_t k = 0; k < 3; ++k) drift = std::max(drift, std::fabs(float(c[k]) - mean[k]));
    }
    drift = drift * 100.0f / ((mean[0] + mean[1] + mean[2]) / 3.0f + 40.0f);
    const std::string_view key = owner ? owner->key : std::string_view{};
    if (key == "belt" || key == "tunic") {
        if (drift <= 10.0f && luma(t[0]) >= 40.0f) return ElfMaterial::kBelt; // leather every preset paints alike
        if (drift >= 30.0f) return ElfMaterial::kTunic;                           // cloth each preset dyes its own colour
        return ElfMaterial::kUnknown;
    }
    if (key != "shorts" && key != "skin" && key != "boots" && key != "laces") return ElfMaterial::kUnknown;
    const auto ratio = [](const std::array<uint8_t, 4>& c, size_t a, size_t b) { return float(c[a]) / std::max(1.0f, float(c[b])); };
    if (luma(t[0]) >= 30.0f && t[4][1] >= t[4][0] + 10 && t[2][2] >= t[2][1] + 10) return ElfMaterial::kLaces; // green, mauve
    if (luma(t[0]) >= 60.0f && ratio(t[1], 0, 1) >= 1.8f && ratio(t[1], 2, 1) <= 0.5f && ratio(t[3], 0, 1) < 1.8f) {
        return ElfMaterial::kSkin; // deep orange in preset 2, golden in preset 4
    }
    if (luma(t[0]) >= 30.0f && (ratio(t[3], 0, 1) >= 2.0f || t[3][2] > t[3][1] + 3)) {
        // Red or purple leather; the trim is the cuff's own leather, a light line of it the laces part owns.
        return key == "laces" ? ElfMaterial::kTrim : ElfMaterial::kBoots;
    }
    // Shorts: leather the third preset darkens, or cloth it paints blue-grey; the light fold of a cuff is neither.
    const bool shorts_dye = luma(t[2]) <= luma(t[0]) * 0.7f || t[2][2] >= t[2][1];
    if (luma(t[0]) >= 50.0f && shorts_dye && ratio(t[1], 0, 1) < 1.3f && ratio(t[3], 0, 1) < 1.5f && t[3][2] < t[3][1]) {
        return ElfMaterial::kShorts;
    }
    return ElfMaterial::kUnknown;
}

const char* ElfMaterialPart(ElfMaterial material) {
    switch (material) {
    case ElfMaterial::kBelt: return "belt";
    case ElfMaterial::kTunic: return "tunic";
    case ElfMaterial::kShorts: return "shorts";
    case ElfMaterial::kSkin: return "skin";
    case ElfMaterial::kBoots: return "boots";
    case ElfMaterial::kLaces:
    case ElfMaterial::kTrim: return "laces";
    case ElfMaterial::kUnknown: break;
    }
    return "";
}

void TestElfMaterials(CharacterArt& art, CpkArchive& cpk) {
    const CharacterClass klass = CharacterClass::kElf;
    const std::optional<Mbs> select = SelectModel(cpk, klass);
    CHECK(select && art.Regions(klass));
    if (!select || !art.Regions(klass)) return;
    const RegionMap& map = art.Regions(klass)->front();
    ElfPresets presets;
    for (int color = 0; color < kNativeColorCount; ++color) presets.atlases.push_back(ReadBodyAtlas(cpk, klass, color));
    CHECK(presets.atlases[0].surfaces[0].width() == map.width && presets.atlases[0].surfaces[0].height() == map.height);
    const auto part = [&](const char* key) { return FindCustomPart(klass, key); };

    // Over the whole atlas, every pose of the game: each material belongs to its part.
    std::vector<ElfMaterial> material(size_t(map.width) * size_t(map.height), ElfMaterial::kUnknown);
    std::map<ElfMaterial, std::pair<size_t, size_t>> owned; // texels, texels the material's part owns
    for (int y = 1; y + 1 < map.height; ++y) {
        for (int x = 1; x + 1 < map.width; ++x) {
            if (!map.At(x, y)) continue;
            const CustomPart* owner = OwnerOfTexel(klass, map, x, y);
            const ElfMaterial found = ElfMaterialOf(presets, map, x, y);
            material[size_t(y) * size_t(map.width) + size_t(x)] = found;
            if (found == ElfMaterial::kUnknown) continue;
            auto& [texels, right] = owned[found];
            ++texels;
            right += owner == part(ElfMaterialPart(found));
        }
    }
    CHECK(owned.size() == 7);
    // No texel belongs to two parts.
    CHECK(std::none_of(map.pieces.begin(), map.pieces.end(), [](uint16_t v) { return std::popcount(v) > 1; }));
    CHECK(owned[ElfMaterial::kTrim].first * 100 <= owned[ElfMaterial::kBoots].first * 8); // a line on the cuff, not the boot
    for (const auto& [found, counts] : owned) {
        CHECK(counts.first > 500 && counts.second * 1000 >= counts.first * 995);
        if (counts.second * 1000 < counts.first * 995) std::printf("  %s: %zu of %zu owned\n", ElfMaterialPart(found), counts.second, counts.first);
    }

    // In the preview, in every preset: a hidden part leaves no pixel of its material and takes none of a neighbour's,
    // and its colour, picked or random, moves no pixel of a neighbour's.
    const PartRender r{art, klass, *select};
    const auto material_at = [&](const SpriteImage& image, size_t p) {
        if (!r.Visible(image, p) || image.texture[p] != 0) return ElfMaterial::kUnknown;
        return material[image.texel[p]];
    };
    std::mt19937 random(0xF2A9C);
    for (int color = 0; color < kNativeColorCount; ++color) {
        const SpriteImage native = *art.Preview(klass, color);
        // Pixels still drawn with a texel of the material.
        const auto left = [&](const SpriteImage& image, ElfMaterial gone) {
            size_t pixels = 0;
            for (size_t p = 0; p < image.layer.size(); ++p) pixels += material_at(image, p) == gone;
            return pixels;
        };
        // Pixels within two of a part's own, where the preview filters its edge into what lies beside it.
        const auto rim_of = [&](const CustomPart* owner) {
            std::vector<uint8_t> rim(native.layer.size(), 0);
            for (size_t p = 0; p < native.layer.size(); ++p) {
                if (!r.Visible(native, p) || r.PartOf(native, p) != owner) continue;
                const int x = int(p % size_t(native.width)), y = int(p / size_t(native.width));
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        if (x + dx >= 0 && y + dy >= 0 && x + dx < native.width && y + dy < native.height) {
                            rim[size_t(y + dy) * size_t(native.width) + size_t(x + dx)] = 1;
                        }
                    }
                }
            }
            return rim;
        };
        const auto removed = [&](const SpriteImage& after, ElfMaterial kept, const std::vector<uint8_t>& rim) {
            size_t pixels = 0, gone = 0;
            for (size_t p = 0; p < native.layer.size(); ++p) {
                if (material_at(native, p) != kept || rim[p]) continue;
                ++pixels;
                gone += int(after.rgba[p * 4 + 3]) + 8 < int(native.rgba[p * 4 + 3]);
            }
            return std::pair{pixels, gone};
        };
        const auto moved = [&](const SpriteImage& after, ElfMaterial kept, const std::vector<uint8_t>& rim) {
            size_t pixels = 0, changed = 0;
            for (size_t p = 0; p < native.layer.size(); ++p) {
                if (material_at(native, p) != kept || rim[p]) continue;
                ++pixels;
                changed += PartRender::Delta(native, after, p) > 1;
            }
            return std::pair{pixels, changed};
        };
        struct Case {
            const char* hidden;
            std::vector<ElfMaterial> gone;
            std::vector<ElfMaterial> kept;
        };
        const std::vector<Case> cases = {
            {"belt", {ElfMaterial::kBelt}, {ElfMaterial::kTunic, ElfMaterial::kSkin, ElfMaterial::kShorts}},
            {"shorts", {ElfMaterial::kShorts}, {ElfMaterial::kSkin, ElfMaterial::kTunic, ElfMaterial::kBelt, ElfMaterial::kBoots, ElfMaterial::kLaces, ElfMaterial::kTrim}},
            {"laces", {ElfMaterial::kLaces, ElfMaterial::kTrim}, {ElfMaterial::kBoots, ElfMaterial::kSkin, ElfMaterial::kShorts}},
            {"boots", {ElfMaterial::kBoots}, {ElfMaterial::kLaces, ElfMaterial::kTrim, ElfMaterial::kSkin, ElfMaterial::kShorts}}};
        for (const Case& c : cases) {
            const SpriteImage after = *art.CustomPreview(klass, color, Hidden(color, {c.hidden}));
            for (const ElfMaterial gone : c.gone) {
                CHECK(left(native, gone) > 20 && left(after, gone) == 0);
                if (left(after, gone)) std::printf("  colour %d %s hidden leaves %zu %s pixels\n", color, c.hidden, left(after, gone), ElfMaterialPart(gone));
            }
            const std::vector<uint8_t> rim = rim_of(part(c.hidden));
            for (const ElfMaterial kept : c.kept) {
                const auto [pixels, gone] = removed(after, kept, rim);
                CHECK(gone == 0);
                if (gone) std::printf("  colour %d %s hidden takes %zu of %zu %s pixels\n", color, c.hidden, gone, pixels, ElfMaterialPart(kept));
            }
        }
        // Colour, picked or random, moves the part's own material and no pixel of any other.
        const ElfMaterial all[] = {ElfMaterial::kBelt, ElfMaterial::kTunic, ElfMaterial::kShorts, ElfMaterial::kSkin,
                                   ElfMaterial::kBoots, ElfMaterial::kLaces, ElfMaterial::kTrim};
        for (const char* painted : {"belt", "shorts", "laces", "boots"}) {
            const std::vector<uint8_t> rim = rim_of(part(painted));
            for (const Rgb value : {Rgb{0, 255, 64}, NextRandomColor(random, std::nullopt), NextRandomColor(random, std::nullopt)}) {
                const SpriteImage after = *art.CustomPreview(klass, color, Painted(color, {painted}, value));
                for (const ElfMaterial other : all) {
                    const bool own = ElfMaterialPart(other) == std::string_view(painted);
                    const auto [pixels, changed] = moved(after, other, own ? std::vector<uint8_t>(rim.size(), 0) : rim);
                    if (own) {
                        CHECK(pixels > 20 && changed * 10 >= pixels * 9);
                        continue;
                    }
                    CHECK(changed == 0);
                    if (changed) std::printf("  colour %d %s colour moves %zu of %zu %s pixels\n", color, painted, changed, pixels, ElfMaterialPart(other));
                }
            }
        }
        // In the atlas the game is served: the hidden part's texels are transparent to the last bit, with the colour of
        // what is around them, and every other texel keeps its alpha.
        for (const char* hidden : {"belt", "shorts", "laces", "boots"}) {
            const uint16_t bit = PieceBitOf(klass, *part(hidden));
            const std::optional<std::vector<uint8_t>> served = CustomizeAtlas(presets.atlases[size_t(color)].pack, *art.Regions(klass), klass, Hidden(color, {hidden}));
            CHECK(served.has_value());
            if (!served) continue;
            const FtxSurface after = *ReadFtxSurface(*served, 0);
            const FtxSurface& before = presets.atlases[size_t(color)].surfaces[0];
            size_t mine = 0, shown = 0, black = 0, others_faded = 0;
            for (int y = 1; y + 1 < map.height; y += 2) {
                for (int x = 1; x + 1 < map.width; x += 2) {
                    const std::array<uint8_t, 4> was = before.Texel(x, y), now = after.Texel(x, y);
                    if (!was[3]) continue;
                    if (map.Pieces(x, y) & bit) {
                        ++mine;
                        shown += now[3] != 0;
                        bool beside_seen = false; // a lit texel of another part beside it, whose edge its colour would tint
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                const std::array<uint8_t, 4> beside = before.Texel(x + dx, y + dy);
                                beside_seen = beside_seen || (!(map.Pieces(x + dx, y + dy) & bit) && beside[3] == 255 && Luma(beside[0], beside[1], beside[2]) >= 40.0f);
                            }
                        }
                        black += beside_seen && now[0] + now[1] + now[2] == 0 && was[0] + was[1] + was[2] > 0;
                    } else {
                        others_faded += was[3] == 255 && now[3] < 240;
                    }
                }
            }
            CHECK(mine > 1000 && shown == 0 && black == 0 && others_faded == 0);
            if (shown || black || others_faded) std::printf("  colour %d %s served: %zu shown, %zu black, %zu faded\n", color, hidden, shown, black, others_faded);
        }
    }
    // The atlas the game is served draws the same.
    CheckServedLook(art, cpk, klass, *select, Hidden(0, {"belt", "shorts", "laces"}));
    CheckServedLook(art, cpk, klass, *select, Hidden(3, {"boots"}));
}

void TestClassWeapons(CharacterArt& art) {
    const std::map<CharacterClass, std::vector<std::string>> expected = {
        {CharacterClass::kFighter, {"Sword", "Shield"}}, {CharacterClass::kAmazon, {"Axe"}},       {CharacterClass::kElf, {"Bow"}},
        {CharacterClass::kDwarf, {"Hammers"}},           {CharacterClass::kSorceress, {"Staff"}}, {CharacterClass::kWizard, {"Staff"}},
    };
    for (const CharacterClass klass : kCharacterClasses) {
        const std::span<const WeaponTexture> weapons = WeaponTexturesOf(klass);
        CHECK(weapons.size() == expected.at(klass).size());
        for (size_t w = 0; w < weapons.size(); ++w) {
            const CustomPart* part = WeaponPartOf(klass, w);
            CHECK(part && part->label == expected.at(klass)[w] && !part->HasPieces() &&
                  part->regions == 0);
            if (!part) continue;
            CHECK(WeaponPartOfTexture(klass, weapons[w].texture) == part);
            const SpriteImage native = *art.Preview(klass, 0);
            const SpriteImage painted = *art.CustomPreview(klass, 0, Painted(0, {std::string(part->key).c_str()}, {40, 90, 230}));
            for (uint8_t texture = 0; texture < 3; ++texture) {
                const PartEffect e = EffectOnTexture(native, painted, texture);
                // A weapon with an accent leaves its band to the accent.
                if (texture == weapons[w].texture) CHECK(e.pixels > 500 && e.moved * 100 >= e.pixels * (WeaponAccentOf(klass, w) ? 75 : 90));
                else CHECK(e.moved * 100 <= e.pixels);
            }
            // Hidden with its accent, a weapon draws nothing; the accent is a part of its own.
            CustomLook hidden = Hidden(0, {std::string(part->key).c_str()});
            if (const CustomPart* accent = WeaponAccentOf(klass, w)) hidden.SetPartHidden(accent->key, true);
            const SpriteImage gone = *art.CustomPreview(klass, 0, hidden);
            CHECK(DrawnFrom(native, weapons[w].texture) > 500 && DrawnFrom(gone, weapons[w].texture) == 0);
            CHECK(EffectOnTexture(native, gone, 0).removed * 100 <= EffectOnTexture(native, gone, 0).pixels);
        }
    }
    CHECK(WeaponAtlasName(CharacterClass::kDwarf, WeaponTexturesOf(CharacterClass::kDwarf)[0]) == "PLAYER/DWARF00_ARMS01.FTX");
    CHECK(WeaponAtlasName(CharacterClass::kFighter, WeaponTexturesOf(CharacterClass::kFighter)[1]) == "PLAYER/FIGHTER00_SHIELD01.FTX");
}

void TestPartCapabilities() {
    // EVERY SELECTABLE PART SUPPORTS HIDDEN: every Selected Part of every class takes a colour, a random colour, Reset
    // Part, Hidden and Shown. On a surface it alone covers, its colour moves every texel, Hidden clears every texel, and
    // Shown or Reset Part brings the native back.
    std::mt19937 random(3);
    for (const CharacterClass klass : kCharacterClasses) {
        for (const CustomPart& part : CustomPartsOf(klass)) {
            const std::string key(part.key);
            std::function<std::array<uint8_t, 4>(const CustomLook&)> draw;
            std::array<uint8_t, 4> native{};
            if (part.weapon >= 0) {
                // Half the texture its blue band, half plain.
                const TexelReader weapon = [](int x, int) {
                    return x < 2 ? std::array<uint8_t, 4>{40, 90, 230, 255} : std::array<uint8_t, 4>{120, 118, 100, 255};
                };
                native = weapon(part.accent ? 0 : 3, 0);
                draw = [&, weapon, native](const CustomLook& look) {
                    std::array<uint8_t, 4> texel = native;
                    WeaponShader(klass, look, size_t(part.weapon), weapon, 4, 4).Apply(texel);
                    return texel;
                };
            } else {
                uint8_t region = 1;
                while (part.regions && !(part.regions >> (region - 1) & 1)) ++region;
                const auto map = std::make_shared<RegionMap>(
                    RegionMap{4, 4, std::vector<uint8_t>(16, region), std::vector<uint16_t>(16, PieceBitOf(klass, part))});
                const TexelReader grey = [](int, int) { return std::array<uint8_t, 4>{120, 120, 120, 255}; };
                native = grey(1, 1);
                draw = [&, map, grey, native](const CustomLook& look) {
                    std::array<uint8_t, 4> texel = native;
                    LookShader(klass, look, *map, grey).ApplyAt(1, 1, texel);
                    return texel;
                };
            }
            CustomLook look{true, 0, {}};
            look.SetPartColor(key, {200, 40, 40});
            const std::array<uint8_t, 4> red = draw(look);
            const bool supports_color = red != native && red[3] == native[3];
            look.SetPartColor(key, NextRandomColor(random, look.Find(key)->color));
            const bool supports_random = draw(look) != red && draw(look)[3] == native[3];
            look.ResetPart(key);
            const bool supports_reset = look.Find(key) == nullptr && draw(look) == native;
            look.SetPartHidden(key, true);
            const bool supports_hidden = draw(look)[3] == 0;
            look.SetPartHidden(key, false);
            const bool supports_visibility = supports_hidden && draw(look) == native; // Shown gives the part back
            CHECK(supports_color && supports_random && supports_reset && supports_visibility && supports_hidden);
        }
    }
    // Saved looks stay readable: a part stored with a colour and no hidden bit is Shown, and the key of a part that no
    // longer exists (the Wizard's old Trim) changes nothing.
    CustomStore saved(fs::path("unused"));
    CustomLook stored{true, 1, {}};
    stored.SetPartColor("skin", {10, 20, 30});
    stored.SetPartHidden("trim", true);
    saved.Set({"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kWizard}, stored);
    CustomStore reread(fs::path("unused"));
    CHECK(reread.Parse(saved.Serialize()));
    const std::optional<CustomLook> back = reread.Get({"CUSA10454", "SAVEDATA00", 0, 0, CharacterClass::kWizard});
    CHECK(back && back->Find("skin") && !back->Find("skin")->hidden && back->Find("trim") && back->Find("trim")->hidden);
    CHECK(FindCustomPart(CharacterClass::kWizard, "trim") == nullptr);
    const RegionMap map{4, 4, std::vector<uint8_t>(16, 4), std::vector<uint16_t>(16, 0)};
    const TexelReader grey = [](int, int) { return std::array<uint8_t, 4>{120, 120, 120, 255}; };
    std::array<uint8_t, 4> untouched = grey(1, 1);
    LookShader(CharacterClass::kWizard, Hidden(0, {"trim"}), map, grey).ApplyAt(1, 1, untouched);
    CHECK(untouched == grey(1, 1));
    // A piece part's hiding clears the texels that carry its bit and no others.
    const CustomPart* fighter_helmet = FindCustomPart(CharacterClass::kFighter, "helmet");
    CHECK(fighter_helmet != nullptr);
    if (fighter_helmet) {
        RegionMap owned = map;
        std::fill(owned.pieces.begin(), owned.pieces.end(), PieceBitOf(CharacterClass::kFighter, *fighter_helmet));
        std::array<uint8_t, 4> gone{120, 120, 120, 255};
        LookShader(CharacterClass::kFighter, Hidden(0, {"helmet"}), owned, grey).ApplyAt(2, 2, gone);
        CHECK(gone[3] == 0);
        std::array<uint8_t, 4> kept{120, 120, 120, 255};
        LookShader(CharacterClass::kFighter, Hidden(0, {"horns"}), owned, grey).ApplyAt(2, 2, kept);
        CHECK(kept[3] == 255);
        std::array<uint8_t, 4> bare{120, 120, 120, 255};
        LookShader(CharacterClass::kFighter, Hidden(0, {"helmet"}), map, grey).ApplyAt(2, 2, bare);
        CHECK(bare[3] == 255);
    }
}

void TestRandomColors() {
    // Seeded generators give the same colours; every colour is opaque RGB, bright and saturated enough to read.
    std::mt19937 a(1234), b(1234);
    std::set<std::tuple<int, int, int>> seen;
    for (int i = 0; i < 500; ++i) {
        const Rgb x = RandomVisibleColor(a), y = RandomVisibleColor(b);
        CHECK(x == y);
        const int high = std::max({x.r, x.g, x.b}), low = std::min({x.r, x.g, x.b});
        CHECK(high >= 135 && (high - low) * 100 >= high * 40);
        seen.insert({x.r, x.g, x.b});
    }
    CHECK(seen.size() > 450);
    CHECK(ColorFromHsv(0.0f, 1.0f, 1.0f) == (Rgb{255, 0, 0}) && ColorFromHsv(1.0f / 3.0f, 1.0f, 1.0f) == (Rgb{0, 255, 0}));
    // Random on one part: a new colour each time, nothing else changes.
    std::mt19937 generator(7);
    CustomLook look{true, 2, {}};
    look.SetPartHidden("hat", true);
    look.SetPartColor("dress", Rgb{10, 20, 30});
    const CustomLook before = look;
    std::optional<Rgb> previous;
    for (int i = 0; i < 20; ++i) {
        const Rgb next = NextRandomColor(generator, previous);
        CHECK(!previous || next != *previous);
        look.SetPartColor("hair", next);
        previous = next;
    }
    CHECK(look.base == 2 && look.Find("hat")->hidden && !look.Find("hat")->color && look.Find("dress")->color == (Rgb{10, 20, 30}));
    CHECK(look.Find("hair")->color == previous && !look.Find("hair")->hidden);
    CHECK(before.Find("hair") == nullptr);
    // All Colors Random: every part that takes a colour gets its own; visibility, the base and the headgear stay.
    for (const CharacterClass klass : kCharacterClasses) {
        CustomLook all{true, 3, {}};
        const std::string hidden_key(CustomPartsOf(klass).front().key);
        all.SetPartHidden(hidden_key, true);
        std::mt19937 seeded(99);
        all.RandomizeColors(klass, seeded);
        std::set<std::tuple<int, int, int>> colors;
        size_t colored = 0;
        for (const CustomPart& part : CustomPartsOf(klass)) {
            const PartLook* entry = all.Find(part.key);
            CHECK(entry && entry->color.has_value());
            if (!entry || !entry->color) continue;
            ++colored;
            colors.insert({entry->color->r, entry->color->g, entry->color->b});
            CHECK(entry->hidden == (part.key == hidden_key));
        }
        CHECK(colored >= 6 && colors.size() == colored);
        CHECK(all.base == 3 && !all.Find("headgear"));
        std::mt19937 again(99);
        CustomLook repeat{true, 3, {}};
        repeat.SetPartHidden(hidden_key, true);
        repeat.RandomizeColors(klass, again);
        CHECK(repeat == all); // deterministic with a seeded generator
        all.RandomizeColors(klass, seeded);
        for (const CustomPart& part : CustomPartsOf(klass)) {
            CHECK(all.Find(part.key)->color != repeat.Find(part.key)->color); // a new colour each time
        }
    }
}

void TestIdentityHeaderLayout() {
    // Name and details share one baseline whatever their fonts' ascents, and the line is centred in its row.
    for (const float row : {0.0f, 24.0f, 60.0f}) {
        const IdentityLine line = LayoutIdentityLine(15.0f, 19.0f, 13.0f, 18.0f, row);
        CHECK(std::abs((line.name_y + 15.0f) - (line.detail_y + 13.0f)) < 0.001f);
        CHECK(line.height == 20.0f);
        const float top = std::min(line.name_y, line.detail_y);
        if (row > line.height) CHECK(std::abs(top - (row - line.height) * 0.5f) < 0.001f);
        else CHECK(top == 0.0f);
    }
    const IdentityLine same = LayoutIdentityLine(14.0f, 18.0f, 14.0f, 18.0f, 18.0f);
    CHECK(same.name_y == same.detail_y && same.height == 18.0f);
    CHECK(PickerSwatchesHeight(20.0f, 14.0f, 4.0f) == 102.0f);
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const std::string custom = FunctionBody(page, "void EditorPage::DrawCustom(");
    // Part Color: swatch, Random, Reset Part, then Part Visibility; every part has all of them.
    const size_t swatch = custom.find("ImGui::ColorButton(\"##part_color\"");
    const size_t random = custom.find("ImGui::Button(Tr(\"Random\"))");
    const size_t reset_part = custom.find("ImGui::Button(Tr(\"Reset Part\"))");
    CHECK(swatch < random && random < reset_part && reset_part < custom.find("Tr(\"Part Visibility\")"));
    CHECK(custom.find("NextRandomColor(random_, current_color)") > random);
    CHECK(custom.find("CanColor") == std::string::npos && custom.find("CanHide") == std::string::npos);
    // Part Visibility (<part>) with Shown and Hidden, for every selected part: no condition hides or disables the row, and
    // there is no Headgear row: a helmet, hat or hood is a part like any other.
    const size_t visibility = custom.find("PartVisibilityLabel(Tr(\"Part Visibility\"), Tr(std::string(part->label).c_str()))");
    const size_t segmented = custom.find("ui.Segmented(\"##part_visibility\", {Tr(\"Shown\"), Tr(\"Hidden\")}");
    const size_t row_end = custom.find("ui.EndRows();", reset_part);
    CHECK(visibility != std::string::npos && segmented != std::string::npos && visibility < segmented && segmented < row_end);
    const std::string row = custom.substr(reset_part, row_end - reset_part);
    CHECK(Count(row, "if (") == 1 && row.find("if (visibility >= 0 && (visibility == 1) != current_hidden)") != std::string::npos);
    CHECK(Count(row, "ImGui::BeginDisabled(") == 0 && row.find("ImGui::EndDisabled();") < row.find("Part Visibility")); // Reset Part's alone
    for (const char* gate : {"supports_visibility", "can_hide", "hide_safe", "CanHide", "HeadgearLockedReason"}) CHECK(row.find(gate) == std::string::npos);
    CHECK(PartVisibilityLabel("Part Visibility", "Skin") == "Part Visibility (Skin)");
    CHECK(page.find("DrawHeadgearRow") == std::string::npos && page.find("##headgear") == std::string::npos);
    CHECK(ReadSource("launcher/editor_page.h").find("Headgear") == std::string::npos);
    const size_t all_random = custom.find("DrawAllColorsRandom(ui, character.klass, look);");
    const size_t reset = custom.find("DrawResetCustom(ui, look);");
    CHECK(custom.find("ui.EndRows();") < all_random && all_random < reset && reset != std::string::npos);
    const std::string all = FunctionBody(page, "void EditorPage::DrawAllColorsRandom(");
    CHECK(all.find("ui.ButtonRow(Tr(\"All Colors Random\")") != std::string::npos && all.find("look.RandomizeColors(klass, random_);") != std::string::npos);
    CHECK(all.find("SetLook(look);") != std::string::npos && all.find("look.base") == std::string::npos);
    CHECK(ReadSource("launcher/editor_page.h").find("std::mt19937 random_{std::random_device{}()};") != std::string::npos);
    const std::string picker = FunctionBody(page, "void EditorPage::DrawColorPicker(");
    CHECK(picker.find("PickerSwatchesHeight(ImGui::GetFrameHeight(), ImGui::GetTextLineHeight(), ImGui::GetStyle().ItemSpacing.y)") <
          picker.find("Tr(\"OK\")"));
    CHECK(picker.find("picker_min") == std::string::npos);
#endif
}

void TestWeaponEntries() {
    const CustomInstanceKey fighter{0, 1, CharacterClass::kFighter, 2, 0x1122334455667788ull};
    CHECK(CustomInstanceEntry(fighter) == "PLAYER/FIGHTER02_D1C02_444444CC.FTX");
    CHECK(CustomWeaponEntry(fighter, 0) == "PLAYER/FIGHTER00_ARMS01_D1C02_444444CC.FTX");
    CHECK(CustomWeaponEntry(fighter, 1) == "PLAYER/FIGHTER00_SHIELD01_D1C02_444444CC.FTX" && CustomWeaponEntry(fighter, 2).empty());
    const CustomInstanceKey sorceress{3, 15, CharacterClass::kSorceress, 4, ~0ull};
    const std::string longest = CustomWeaponEntry(sorceress, 0);
    CHECK(longest == "PLAYER/SORCERESS00_ARMS01_D4C16_00000000.FTX" && longest.size() < APPEARANCE_ENTRY_BYTES);
    CHECK(CustomWeaponEntry(sorceress, 1).empty());
    CHECK(std::string("PLAYER/SORCERESS00_ARMS01.FTX").size() < APPEARANCE_WEAPON_NAME_BYTES);
    CustomLook look{true, 0, {}};
    CHECK(!WeaponChanged(CharacterClass::kFighter, 0, look) && !WeaponChanged(CharacterClass::kFighter, 1, look));
    look.SetPartColor("sword", Rgb{1, 2, 3});
    CHECK(WeaponChanged(CharacterClass::kFighter, 0, look) && !WeaponChanged(CharacterClass::kFighter, 1, look));
    look.SetPartHidden("shield", true);
    CHECK(WeaponChanged(CharacterClass::kFighter, 1, look));
    look.enabled = false;
    CHECK(!WeaponChanged(CharacterClass::kFighter, 0, look));
    // A weapon look changes the look's hash, so a new instance entry is made for it.
    CustomLook plain{true, 0, {}};
    plain.SetPartColor("armor", Rgb{9, 9, 9});
    CustomLook armed = plain;
    armed.SetPartColor("sword", Rgb{9, 9, 9});
    CHECK(CustomLookHash(plain) != CustomLookHash(armed));
}

void TestPartMapping(const fs::path& base, const fs::path& source_dir) {
    const fs::path app0 = source_dir / "game" / "prepared" / "v1.03";
    std::error_code ec;
    if (!fs::is_regular_file(app0 / "DRACRO.CPK", ec)) {
        std::printf("installed game not present, skipping the part mapping check\n");
        return;
    }
    CharacterArtSources sources = ArtSourcesOf(app0, "DRACRO_US.CPK");
    sources.cache_dir = base / "part_cache";
    CharacterArt art(sources);
    CpkArchive cpk;
    CHECK(cpk.Open(app0 / "DRACRO.CPK"));
    TestAmazonParts(art, cpk);
    TestSorceressParts(art, cpk);
    TestSorceressSplit(art, cpk);
    TestFighterParts(art, cpk);
    TestFighterHelmetAndHorns(art, cpk);
    TestFighterBareHead(art, cpk);
    TestFighterHelmetContexts(art, cpk);
    TestDwarfParts(art, cpk);
    TestDwarfHeadAndGear(art, cpk);
    TestWizardParts(art, cpk);
    TestElfParts(art, cpk);
    TestElfMaterials(art, cpk);
    TestClassWeapons(art);
    TestEveryPartHides(art, cpk);
}

void TestCompactSummary() {
    for (const float scale : {1.0f, 1.5f, 2.25f}) {
        const float frame = trg::layout::kControlHeight * scale;
        for (const float row : {700.0f * scale, 1000.0f * scale, 1600.0f * scale}) {
            const float control = trg::layout::ControlColumnWidth(row, scale);
            const SummaryLayout layout = LayoutCharacterSummary(row, 260.0f * scale, control, frame, 8.0f * scale, scale);
            CHECK(layout.button_height > frame);                                             // larger than a row button
            CHECK(std::abs(layout.button_y + layout.button_height * 0.5f - layout.height * 0.5f) <= 1.0f); // centred
            CHECK(std::abs(layout.button_x - (row - control)) < 0.01f && std::abs(layout.button_width - control) < 0.01f);
            CHECK(layout.button_x + layout.button_width <= row + 0.01f && layout.button_x > 260.0f * scale);
            CHECK(layout.height < 72.0f * scale); // lower than the old portrait and three text lines
        }
        // Narrow: the button never covers the identity.
        const SummaryLayout narrow = LayoutCharacterSummary(500.0f * scale, 400.0f * scale, 300.0f * scale, frame, 8.0f * scale, scale);
        CHECK(narrow.button_x >= 408.0f * scale - 0.01f && narrow.button_x + narrow.button_width <= 500.0f * scale + 0.01f);
    }
    SpriteImage image;
    image.width = 10;
    image.height = 8;
    image.rgba.assign(10 * 8 * 4, 0);
    const PixelBox empty = OpaqueBounds(image, 1);
    CHECK(empty.x0 == 0 && empty.y0 == 0 && empty.x1 == 10 && empty.y1 == 8);
    for (int y = 2; y <= 4; ++y)
        for (int x = 3; x <= 5; ++x) image.rgba[(size_t(y) * 10 + size_t(x)) * 4 + 3] = 255;
    const PixelBox box = OpaqueBounds(image, 1);
    CHECK(box.x0 == 2 && box.y0 == 1 && box.x1 == 7 && box.y1 == 6);
    const ArtFit fit = FitArt(10.0f, 20.0f, 100.0f, 200.0f, PixelBox{0, 0, 50, 50});
    CHECK(fit.scale == 2.0f && fit.x == 10.0f && fit.y == 20.0f && fit.height == 100.0f); // set against the top
    const ArtFit tall = FitArt(0.0f, 0.0f, 100.0f, 100.0f, PixelBox{5, 5, 30, 105});
    CHECK(tall.scale == 1.0f && tall.x == 37.5f && tall.width == 25.0f);
    // A pixel of the image maps back through the crop at any scale.
    const ImagePoint p = MapToImage(tall.x - 5.0f * tall.scale + 12.0f, tall.y - 5.0f * tall.scale + 40.0f, tall.x - 5.0f * tall.scale,
                                    tall.y - 5.0f * tall.scale, tall.scale);
    CHECK(p.x == 12 && p.y == 40);
#ifdef DCP_SOURCE_DIR
    const std::string page = ReadSource("launcher/editor_page.cpp");
    const std::string editor = FunctionBody(page, "void EditorPage::DrawCharacterEditor(");
    CHECK(editor.find("LayoutCharacterSummary(row_width") != std::string::npos);
    CHECK(editor.find("trg::layout::ControlColumnWidth(row_width, scale)") != std::string::npos);
    CHECK(editor.find("ImGui::Button(Tr(\"Edit Appearance\"), ImVec2(layout.button_width, layout.button_height))") != std::string::npos);
    CHECK(editor.find("ImGui::SameLine(row_start + layout.button_x)") < editor.find("Tr(\"Edit Appearance\")"));
    CHECK(editor.find("DrawIdentity(ui, ") != std::string::npos && editor.find("Tr(\"Lv\")") != std::string::npos);
    CHECK(editor.find("72.0f") == std::string::npos && editor.find("class_text") == std::string::npos);
    const std::string identity = FunctionBody(page, "void EditorPage::DrawIdentity(");
    CHECK(identity.find("ImVec2(at.x, at.y + line.name_y)") < identity.find("draw->AddText(ImVec2(x, y)"));
    CHECK(identity.find("part.changed ? ImGui::GetColorU32(ui.theme().accent)") != std::string::npos);
    const std::string view = FunctionBody(page, "void EditorPage::DrawAppearance(");
    CHECK(view.find("PreviewCrop(character.klass, color)") != std::string::npos && view.find("FitArt(") != std::string::npos);
    CHECK(view.find("InvisibleButton(\"##figure\", ImVec2(avail.x, figure_height))") != std::string::npos);
    CHECK(view.find("fit.x - float(crop->x0) * fit.scale") != std::string::npos); // hit testing follows the crop
    CHECK(view.find("DrawIdentity(ui, ImGui::GetCursorScreenPos(), identity, identity_size.y)") < view.find("DrawCustom(ui, character, look);"));
    CHECK(view.find("TitleWithButton(ui, Tr(\"Appearance\"), Tr(\"Back to Character Editor\"))") < view.find("BeginTable("));
    const std::string portuguese = ReadSource("launcher/localization/pt_br_ui_text.cpp");
    for (const char* text : {"\nTattoo\tTatuagem\n", "\nBodice\t", "\nBoots\t"}) CHECK(portuguese.find(text) != std::string::npos);
    CHECK(portuguese.find("\nBack to Character Editor\tVoltar ao editor de personagem\n") != std::string::npos);
    CHECK(portuguese.find("\nBack to Character\t") == std::string::npos);
#endif
}

// ---- Custom looks composed with the port's translation on one DRACRO.CPK ----

using FileList = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

std::vector<uint8_t> Pattern(size_t size, uint8_t seed) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t(seed + i * 7);
    return bytes;
}

struct SyntheticArchive {
    fs::path path;
    uint64_t length = 0;
    FileList files;
};

SyntheticArchive MakeSyntheticArchive(const fs::path& dir) {
    SyntheticArchive archive;
    fs::create_directories(dir);
    archive.path = dir / "DRACRO.CPK";
    archive.files = {{"msgsheet/Menu.fms", Pattern(300, 1)},
                     {"PLAYER/AMAZON03.FTX", Pattern(5000, 2)},
                     {"PLAYER/FIGHTER00.MBS", Pattern(1500, 3)},
                     {"stage/Interface00.ftx", Pattern(2600, 4)},
                     {"stage/Opening.ftx", Pattern(900, 5)}};
    std::vector<std::pair<std::string, std::string>> text;
    for (const auto& [path, data] : archive.files) text.emplace_back(path, std::string(data.begin(), data.end()));
    const std::string blob = cpk_build::BuildCpkWith(text);
    WriteBytes(archive.path, std::vector<uint8_t>(blob.begin(), blob.end()));
    archive.length = blob.size();
    return archive;
}

FileList TranslatedFiles() {
    return {{"msgsheet/Menu.fms", Pattern(420, 11)}, {"stage/Interface00.ftx", Pattern(3100, 12)}};
}
FileList LookReplaced() {
    return {{"PLAYER/FIGHTER00.MBS", Pattern(1700, 21)}};
}
FileList LookAdded() {
    return {{"PLAYER/AMAZON03_X1.FTX", Pattern(6000, 22)}, {"PLAYER/AMAZON03_X2.FTX", Pattern(6001, 23)}};
}
const std::vector<std::pair<std::string, uint64_t>>& LookReserved() {
    static const std::vector<std::pair<std::string, uint64_t>> reserved = {{"PLAYER/AMAZON03_NET0.FTX", 6000},
                                                                           {"PLAYER/AMAZON03_NET1.FTX", 6000}};
    return reserved;
}

struct Composed {
    std::optional<ArchiveOverlay> translation, layer;
    ArchiveOverlay overlay; // what the guest reads
    std::string error;
};

Composed ComposeSynthetic(const SyntheticArchive& archive, const FileList& translated, bool custom, const FileList& replaced,
                          const FileList& added, const std::vector<std::pair<std::string, uint64_t>>& reserved) {
    Composed result;
    if (!translated.empty()) {
        CpkArchive cpk;
        if (cpk.Open(archive.path)) result.translation = OverlayEntries(cpk, archive.length, translated, &result.error);
    }
    if (custom) {
        result.layer = OverlayChangedEntries(archive.path, archive.length, replaced, added, &result.error, reserved,
                                             result.translation ? &*result.translation : nullptr);
    }
    if (!result.error.empty()) std::printf("overlay error: %s\n", result.error.c_str());
    if (result.translation && result.layer) result.overlay = ComposeOverlays(*result.translation, *result.layer);
    else if (result.layer) result.overlay = *result.layer;
    else if (result.translation) result.overlay = *result.translation;
    return result;
}

std::optional<std::vector<uint8_t>> EntryBytes(const SyntheticArchive& archive, const ArchiveOverlay& overlay, const std::string& path) {
    CpkArchive served;
    if (!OpenOverlaid(archive.path, overlay, served)) return std::nullopt;
    const CpkEntry* entry = served.Find(path);
    if (!entry) return std::nullopt;
    return ReadOverlaid(archive.path, overlay, entry->offset, entry->size);
}

bool Serves(const SyntheticArchive& archive, const ArchiveOverlay& overlay, const FileList& files) {
    for (const auto& [path, data] : files) {
        const auto bytes = EntryBytes(archive, overlay, path);
        if (!bytes || *bytes != data) return false;
    }
    return true;
}

// Entries nobody changed keep their place, size and id, so the native look is read exactly as before.
bool KeepsNative(const SyntheticArchive& archive, const ArchiveOverlay& overlay, const std::set<std::string>& changed) {
    CpkArchive original, served;
    if (!original.Open(archive.path) || !OpenOverlaid(archive.path, overlay, served)) return false;
    for (const CpkEntry& entry : original.entries()) {
        if (changed.count(entry.path)) continue;
        const CpkEntry* now = served.Find(entry.path);
        if (!now || now->offset != entry.offset || now->size != entry.size) return false;
    }
    return true;
}

bool NoDuplicateEntries(const ArchiveOverlay& overlay, const SyntheticArchive& archive, size_t expected) {
    CpkArchive served;
    if (!OpenOverlaid(archive.path, overlay, served) || served.entries().size() != expected) return false;
    std::set<std::string> names;
    std::set<uint32_t> ids;
    for (const CpkEntry& entry : served.entries()) {
        std::string upper = entry.path;
        for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (!names.insert(upper).second || !ids.insert(entry.id).second) return false;
    }
    return true;
}

bool PatchesEqual(const ArchiveOverlay& a, const ArchiveOverlay& b) {
    return a.length == b.length && a.patches == b.patches;
}

// The appended ranges of the look layer start after every translated byte, aligned, and meet nothing of the translation.
bool AppendedAfterTranslation(const SyntheticArchive& archive, const Composed& built, uint64_t align) {
    if (!built.translation || !built.layer) return false;
    const uint64_t floor = built.translation->length;
    if (!LayerProblem(*built.layer, floor).empty()) return false;
    CpkArchive served;
    if (!OpenOverlaid(archive.path, built.overlay, served)) return false;
    for (const auto& [path, size] : LookReserved()) {
        const CpkEntry* entry = served.Find(path);
        if (!entry || entry->offset < floor || entry->offset % align != 0 || entry->size != size) return false;
        const auto at = std::find_if(built.layer->reserved.begin(), built.layer->reserved.end(),
                                     [&](const ReservedEntry& r) { return r.path == path; });
        if (at == built.layer->reserved.end() || at->offset != entry->offset) return false;
    }
    for (const auto& [path, data] : LookAdded()) {
        const CpkEntry* entry = served.Find(path);
        if (!entry || entry->offset < floor || entry->offset % align != 0) return false;
    }
    for (const auto& [at, data] : built.layer->patches) {
        if (at != 16 && at % align != 0) return false;
        for (const auto& [other, bytes] : built.translation->patches) {
            if (other == 16) continue;
            if (at < other + bytes.size() && other < at + data.size()) return false;
        }
    }
    return built.layer->length >= floor;
}

void TestCompositionAppearanceOnly(const fs::path& dir) {
    const SyntheticArchive archive = MakeSyntheticArchive(dir / "only_look");
    const Composed built = ComposeSynthetic(archive, {}, true, LookReplaced(), LookAdded(), LookReserved());
    CHECK(built.layer && !built.translation);
    if (!built.layer) return;
    for (const auto& [offset, data] : built.layer->patches) CHECK(offset == 16 || offset >= archive.length);
    CHECK(built.layer->length > archive.length);
    CHECK(Serves(archive, built.overlay, LookReplaced()) && Serves(archive, built.overlay, LookAdded()));
    CHECK(KeepsNative(archive, built.overlay, {"PLAYER/FIGHTER00.MBS"}));
    CHECK(NoDuplicateEntries(built.overlay, archive, archive.files.size() + LookAdded().size() + LookReserved().size()));
    const Composed again = ComposeSynthetic(archive, {}, true, LookReplaced(), LookAdded(), LookReserved());
    CHECK(again.layer && PatchesEqual(*again.layer, *built.layer));
}

void TestCompositionTranslationOnly(const fs::path& dir) {
    const SyntheticArchive archive = MakeSyntheticArchive(dir / "only_translation");
    const Composed built = ComposeSynthetic(archive, TranslatedFiles(), false, {}, {}, {});
    CHECK(built.translation && !built.layer);
    if (!built.translation) return;
    CHECK(Serves(archive, built.overlay, TranslatedFiles()));
    CHECK(KeepsNative(archive, built.overlay, {"msgsheet/Menu.fms", "stage/Interface00.ftx"}));
    CHECK(NoDuplicateEntries(built.overlay, archive, archive.files.size()));
}

void TestCompositionBothLayers(const fs::path& dir) {
    const SyntheticArchive archive = MakeSyntheticArchive(dir / "both");
    const Composed built = ComposeSynthetic(archive, TranslatedFiles(), true, LookReplaced(), LookAdded(), LookReserved());
    CHECK(built.translation && built.layer);
    if (!built.translation || !built.layer) return;
    CHECK(Serves(archive, built.overlay, TranslatedFiles()));
    CHECK(Serves(archive, built.overlay, LookReplaced()) && Serves(archive, built.overlay, LookAdded()));
    CHECK(AppendedAfterTranslation(archive, built, 0x800));
    CHECK(built.overlay.length == built.layer->length && built.overlay.length > built.translation->length);
    CHECK(KeepsNative(archive, built.overlay, {"msgsheet/Menu.fms", "stage/Interface00.ftx", "PLAYER/FIGHTER00.MBS"}));
    CHECK(NoDuplicateEntries(built.overlay, archive, archive.files.size() + LookAdded().size() + LookReserved().size()));

    const Composed again = ComposeSynthetic(archive, TranslatedFiles(), true, LookReplaced(), LookAdded(), LookReserved());
    CHECK(again.layer && again.translation && PatchesEqual(again.overlay, built.overlay));

    // Layering the same look again is refused, not grown.
    std::string error;
    CHECK(!OverlayChangedEntries(archive.path, archive.length, {}, LookAdded(), &error, {}, &built.overlay) && !error.empty());

    // The translation alone, with no look, is the registered overlay: nothing to add is an error, not a rewrite.
    CHECK(!OverlayChangedEntries(archive.path, archive.length, {}, {}, &error, {}, &*built.translation));
}

// Both change one entry: the look's version is served, deterministically, and the translation's other entries survive.
void TestCompositionSameEntry(const fs::path& dir) {
    const SyntheticArchive archive = MakeSyntheticArchive(dir / "same_entry");
    FileList translated = TranslatedFiles();
    translated.emplace_back("PLAYER/FIGHTER00.MBS", Pattern(1600, 31));
    std::sort(translated.begin(), translated.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const Composed built = ComposeSynthetic(archive, translated, true, LookReplaced(), LookAdded(), LookReserved());
    CHECK(built.translation && built.layer);
    if (!built.translation || !built.layer) return;
    const auto fighter = EntryBytes(archive, built.overlay, "PLAYER/FIGHTER00.MBS");
    CHECK(fighter && *fighter == LookReplaced()[0].second);
    FileList others = translated;
    others.erase(std::remove_if(others.begin(), others.end(), [](const auto& f) { return f.first == "PLAYER/FIGHTER00.MBS"; }), others.end());
    CHECK(Serves(archive, built.overlay, others));
    CHECK(Serves(archive, *built.translation, translated)); // the translation-only archive is untouched
    const Composed again = ComposeSynthetic(archive, translated, true, LookReplaced(), LookAdded(), LookReserved());
    CHECK(again.layer && PatchesEqual(again.overlay, built.overlay));
}

// A look that cannot be composed leaves the translation-only archive valid and reports no layer.
void TestCompositionFailure(const fs::path& dir) {
    const SyntheticArchive archive = MakeSyntheticArchive(dir / "failure");
    Composed base = ComposeSynthetic(archive, TranslatedFiles(), false, {}, {}, {});
    CHECK(base.translation.has_value());
    if (!base.translation) return;
    const FileList duplicate = {{"PLAYER/AMAZON03.FTX", Pattern(10, 1)}};
    std::string error;
    CHECK(!OverlayChangedEntries(archive.path, archive.length, {}, duplicate, &error, {}, &*base.translation) && !error.empty());
    const std::vector<std::pair<std::string, uint64_t>> taken = {{"stage/Opening.ftx", 100}};
    CHECK(!OverlayChangedEntries(archive.path, archive.length, {}, {}, &error, taken, &*base.translation) && !error.empty());
    const FileList missing = {{"PLAYER/NOWHERE.MBS", Pattern(10, 1)}};
    CHECK(!OverlayChangedEntries(archive.path, archive.length, missing, {}, &error, {}, &*base.translation) && !error.empty());
    CHECK(Serves(archive, *base.translation, TranslatedFiles()));
    CHECK(NoDuplicateEntries(*base.translation, archive, archive.files.size()));
    ArchiveOverlay broken = *base.translation;
    broken.length += 0x800;
    ArchiveOverlay layer = broken;
    layer.patches = {{broken.length - 0x1000, std::vector<uint8_t>(16, 1)}};
    CHECK(!LayerProblem(layer, broken.length).empty());
}

// The loader composes on the registered overlay and never reports a look that was not composed.
void TestCompositionLoader(const fs::path& source_dir) {
    (void)source_dir;
    const std::string loader = ReadSource("src/loader.c");
    CHECK(loader.find("registered_patches(\"DRACRO.CPK\"") != std::string::npos);
    CHECK(loader.find("base_length ? extend_archive(archive,length,patches,count) : overlay_archive(archive,length,patches,count)") != std::string::npos);
    CHECK(loader.find("already overlaid by a translation") == std::string::npos);
    const size_t composed = loader.find("custom looks not used, %s could not be overlaid");
    const size_t enabled = loader.find("appearance_session_enable(");
    CHECK(composed != std::string::npos && enabled != std::string::npos && composed < enabled);
}

// What one language of a build does to DRACRO.CPK, from the production profile.
bool TranslationTouchesSharedArchive(const GameBuild& build, const TextLanguage& language) {
    if (language.translation.empty()) return false;
    const TranslationProfile& profile = *build.translation;
    const std::string_view shared = "DRACRO.CPK";
    return shared == profile.archive || (language.translation == "pt-BR" && shared == profile.visual_archive);
}

ArchiveOverlay OverlayOfRegistered(uint64_t length, const DcpFilePatch* patches, size_t count) {
    ArchiveOverlay overlay;
    overlay.length = length;
    for (size_t i = 0; i < count; ++i) overlay.patches.emplace_back(patches[i].offset, std::vector<uint8_t>(patches[i].data, patches[i].data + patches[i].size));
    return overlay;
}

// The production entry points on the build's real archive: the translation the loader registers, then the look built on it.
bool g_synthetic_only = false;

std::string RealCombination(const fs::path& base, const fs::path& source_dir, const GameBuild& build, const TextLanguage& language,
                            bool touches) {
    if (g_synthetic_only) return "NO_DATA";
    const fs::path app0 = source_dir / "game" / "prepared" / build.folder;
    const fs::path image = source_dir / "build" / "image" / build.folder;
    const fs::path archive_path = app0 / "DRACRO.CPK";
    if (!fs::is_regular_file(archive_path) || !fs::is_regular_file(image / "image.json")) return "NO_DATA";
    const int failures_before = g_failures;
    const fs::path root = base / "real_composition" / (std::string(build.folder) + "_" + std::string(language.key));
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path ini = root / "dcp.ini";
    std::ofstream(ini) << "text_language=" << language.key << "\n";

    uint64_t length = 0;
    DcpFilePatch* patches = nullptr;
    size_t count = 0;
    char text[1024];
    const char* translated = dcp_text_translation(ini.string().c_str(), image.string().c_str(), app0.string().c_str(), &length, &patches,
                                                  &count, text, sizeof(text), language.translation == kPortRussian ? 1 : 0, nullptr);
    std::optional<ArchiveOverlay> translation;
    if (translated && std::string_view(translated) == "DRACRO.CPK") {
        translation = OverlayOfRegistered(length, patches, count);
    } else {
        uint64_t stage_length = 0;
        DcpFilePatch* stage_patches = nullptr;
        size_t stage_count = 0;
        const char* stage = dcp_text_stage_overlay(&stage_length, &stage_patches, &stage_count);
        if (stage && std::string_view(stage) == "DRACRO.CPK") translation = OverlayOfRegistered(stage_length, stage_patches, stage_count);
    }
    localization::SetActiveLanguage("english");
    CHECK(translation.has_value() == touches);

    const fs::path user = root / "user";
    WriteSaveFor(user, build.title_id, "SAVEDATA00", SaveWithCharacters({{1, 3}, {1, 3}}));
    CustomStore store(CustomStorePath(user));
    store.Set({build.title_id, "SAVEDATA00", 0, 0, CharacterClass::kAmazon}, HairLook({30, 90, 230}));
    store.Set({build.title_id, "SAVEDATA00", 0, 1, CharacterClass::kAmazon}, HairLook({200, 0, 0}));
    CHECK(store.Save());

    struct Built {
        ArchiveOverlay layer;
        std::vector<AppearanceInstance> instances;
        std::string message;
        bool ok = false;
    };
    const auto build_look = [&](const ArchiveOverlay* under) {
        Built built;
        uint64_t look_length = 0;
        DcpFilePatch* look_patches = nullptr;
        size_t look_count = 0;
        const AppearanceInstance* instances = nullptr;
        size_t instance_count = 0;
        std::vector<DcpFilePatch> base_patches;
        if (under) {
            for (const auto& [offset, data] : under->patches) base_patches.push_back({offset, data.size(), data.data()});
        }
        char message[4096];
        const char* name = dcp_custom_appearance(image.string().c_str(), app0.string().c_str(), user.string().c_str(), 1,
                                                 under ? under->length : 0, base_patches.data(), base_patches.size(), &look_length,
                                                 &look_patches, &look_count, &instances, &instance_count, message, sizeof(message));
        built.message = message;
        built.ok = name && std::string_view(name) == "DRACRO.CPK";
        if (!built.ok) return built;
        built.layer = OverlayOfRegistered(look_length, look_patches, look_count);
        built.instances.assign(instances, instances + instance_count);
        return built;
    };
    const Built look = build_look(translation ? &*translation : nullptr);
    CHECK(look.ok && look.instances.size() == 2);
    CHECK(look.message.find("texture sets reserved for other players' looks") != std::string::npos);
    if (!look.ok) return "FAIL";

    const uint64_t floor = translation ? translation->length : fs::file_size(archive_path);
    CHECK(LayerProblem(look.layer, floor).empty());
    CHECK(look.layer.length > floor);
    const ArchiveOverlay composed = translation ? ComposeOverlays(*translation, look.layer) : look.layer;

    CpkArchive original, translated_view, served;
    CHECK(original.Open(archive_path) && OpenOverlaid(archive_path, composed, served));
    if (translation) CHECK(OpenOverlaid(archive_path, *translation, translated_view));
    const CpkArchive& before = translation ? translated_view : original;
    CHECK(served.entries().size() > before.entries().size());
    size_t translated_entries = 0;
    for (const CpkEntry& entry : original.entries()) {
        const CpkEntry* kept = before.Find(entry.path);
        if (!translation || !kept || kept->offset == entry.offset) continue;
        const CpkEntry* now = served.Find(entry.path);
        const bool replaced_by_look = look.message.find(entry.path + " is also changed") != std::string::npos;
        if (!now) { CHECK(false); continue; }
        ++translated_entries;
        if (!replaced_by_look) {
            CHECK(now->size == kept->size && ReadOverlaid(archive_path, composed, now->offset, now->size) ==
                                                 ReadOverlaid(archive_path, *translation, kept->offset, kept->size));
        }
    }
    CHECK(!translation || translated_entries > 0);
    for (const AppearanceInstance& instance : look.instances) {
        const CpkEntry* entry = served.Find(instance.entry);
        CHECK(entry && entry->offset >= floor && entry->offset % served.align() == 0);
        CHECK(instance.recipe_size > 0);
    }

    // The same looks on the untranslated archive: identical recipes and identical textures (once per build).
    static std::set<std::string> compared;
    const bool compare_plain = translation && compared.insert(build.folder).second;
    const Built plain = compare_plain ? build_look(nullptr) : Built{};
    CHECK(!compare_plain || (plain.ok && plain.instances.size() == look.instances.size()));
    if (compare_plain && plain.ok && plain.instances.size() == look.instances.size()) {
        CpkArchive plain_served;
        CHECK(OpenOverlaid(archive_path, plain.layer, plain_served));
        for (size_t i = 0; i < look.instances.size(); ++i) {
            CHECK(look.instances[i].recipe_size == plain.instances[i].recipe_size &&
                  std::memcmp(look.instances[i].recipe, plain.instances[i].recipe, look.instances[i].recipe_size) == 0);
            const CpkEntry* a = served.Find(look.instances[i].entry);
            const CpkEntry* b = plain_served.Find(plain.instances[i].entry);
            CHECK(a && b && a->size == b->size);
            if (a && b && a->size == b->size) {
                CHECK(ReadOverlaid(archive_path, composed, a->offset, a->size) == ReadOverlaid(archive_path, plain.layer, b->offset, b->size));
            }
        }
    }
    return g_failures == failures_before ? "PASS" : "FAIL";
}

// One row per supported build and language: the combination is composed on a synthetic archive, and on the real one when
// the game is installed here. An omitted build or language cannot pass unnoticed.
void TestCompositionMatrix(const fs::path& base, const fs::path& source_dir) {
    const SyntheticArchive archive = MakeSyntheticArchive(base / "matrix");
    size_t rows = 0;
    for (const GameBuild& build : SupportedBuilds()) {
        bool baseline = false;
        for (const TextLanguage& language : TextLanguagesOf(&build)) {
            const bool touches = TranslationTouchesSharedArchive(build, language);
            const int before = g_failures;
            const Composed built = ComposeSynthetic(archive, touches ? TranslatedFiles() : FileList{}, true, LookReplaced(), LookAdded(), LookReserved());
            CHECK(built.layer.has_value() && built.translation.has_value() == touches);
            CHECK(Serves(archive, built.overlay, LookAdded()) && Serves(archive, built.overlay, LookReplaced()));
            if (touches) {
                CHECK(Serves(archive, built.overlay, TranslatedFiles()));
                CHECK(AppendedAfterTranslation(archive, built, 0x800));
            }
            const bool run_real = touches || !baseline;
            baseline = baseline || !touches;
            const std::string real = run_real ? RealCombination(base, source_dir, build, language, touches) : "NO_DATA";
            const bool passed = g_failures == before && real != "FAIL";
            std::printf("APPEARANCE_TRANSLATION|%s|%s|%s|%s|%s|%s\n", build.title_id, build.version, std::string(language.name).c_str(),
                        touches ? "translation+appearance" : "appearance only", real == "PASS" ? "real+synthetic" : "synthetic",
                        passed ? "PASS" : "FAIL");
            ++rows;
        }
    }
    CHECK(rows >= SupportedBuilds().size());
}

void RunAppearanceTranslation(const fs::path& base, const char* source_dir) {
    const fs::path dir = base / "composition";
    TestCompositionAppearanceOnly(dir);
    TestCompositionTranslationOnly(dir);
    TestCompositionBothLayers(dir);
    TestCompositionSameEntry(dir);
    TestCompositionFailure(dir);
    TestCompositionLoader(source_dir ? fs::path(source_dir) : fs::path());
    TestCompositionMatrix(base, source_dir ? fs::path(source_dir) : fs::path());
}

// One row per supported build: the Editor finds, parses and attributes a save under the build's title id.
void TestSaveProfiles(const fs::path& base, const fs::path& source) {
    for (const GameBuild& build : SupportedBuilds()) {
        const int before = g_failures;
        const fs::path user = base / "save_profiles" / build.folder;
        WriteSaveFor(user, build.title_id, "SAVEDATA00", SaveWithCharacters({{1, 3}, {0, 2}}));
        const std::vector<DiscoveredSave> saves = DiscoverSaves(user, build.title_id);
        CHECK(saves.size() == 1);
        CHECK(!saves.empty() && saves[0].title_id == build.title_id && saves[0].current && saves[0].model.valid() && !saves[0].region.empty());
        bool real = false;
        std::error_code ec;
        std::vector<fs::path> candidates = {source / "user"};
        for (const auto& entry : fs::directory_iterator(source / "user", ec)) {
            if (entry.is_directory(ec)) candidates.push_back(entry.path());
        }
        for (const fs::path& candidate : candidates) {
            for (const DiscoveredSave& found : DiscoverSaves(candidate, build.title_id)) real = real || (found.title_id == build.title_id && found.model.valid());
        }
        std::printf("EDITOR_SAVE|%s|%s|%s|%s\n", build.title_id, build.version, real ? "real" : "synthetic", g_failures == before ? "PASS" : "FAIL");
    }
}

struct EditorGroup {
    const char* name;
    bool needs_game;
    std::function<void()> run;
};

bool GameDataPresent(const fs::path& source) {
    return !source.empty() && fs::is_regular_file(source / "game" / "prepared" / "v1.03" / "DRACRO.CPK") &&
           fs::is_regular_file(source / "build" / "image" / "v1.03" / "image.json");
}

std::vector<EditorGroup> EditorGroups(const fs::path& base, const fs::path& source) {
    return {
        {"editor-save-parse", false, [&] { TestCrc32(); TestParseValid(); TestInvalidCrc(); TestTruncated(); TestSlots(); TestCharacters(); TestSaveProfiles(base, source); }},
        {"editor-save-write", false,
         [&] {
             TestEdits();
             TestSerializeRecomputesCrc();
             TestAtomicWrite(base);
             TestBackupRestore(base);
             TestImport(base);
             TestSaveEdit(base);
             TestInvalidAppearance(base);
             TestNoChangeGuarantee();
         }},
        {"editor-isolation", false,
         [&] {
             TestDiscovery(base);
             TestEditorModel(base);
             TestEditorCharacterNavigation(base);
             TestEditorNavigationModel();
         }},
        {"editor-apply-discard", false,
         [&] {
             TestApplyAll(base);
             TestEditorDirtyFields();
             TestEditorBuildChoice();
             TestEditorNumbers();
         }},
        {"editor-identity", false,
         [&] {
             TestInstanceKeys();
             TestRuntimeInstances(base);
             TestCustomStore(base);
             TestAppearanceHandlers();
             TestAppearanceStubs();
         }},
        {"editor-looks", false,
         [&] {
             TestCustomOverrideModel(base);
             TestAppearanceLooks();
             TestAppearanceLoader();
             TestPartMappingTables();
             TestPartCapabilities();
             TestRandomColors();
             TestCustomParts();
             TestRecolor();
             TestWeaponEntries();
             TestColorPickerModel();
             TestRegionMapFile();
         }},
        {"editor-art", false,
         [] {
             TestBc7Decode();
             TestFtxDecode();
             TestMbsParse();
             TestSpriteRender();
             TestNativeColor();
             TestBc7Encode();
         }},
        {"editor-ui-model", false,
         [] {
             TestAppearanceUx();
             TestEditorSections();
             TestEditorLayoutAudit();
             TestActionRow();
             TestEditorHeaderLayout();
             TestEditorResponsiveLayout();
             TestEditorNumberControlLook();
             TestEditorSummary();
             TestEditorHeaderRow();
             TestEditorCompactLayout();
             TestEditorFixedLayout();
             TestSaveDataText();
             TestIdentityHeaderLayout();
             TestCompactSummary();
         }},
        {"appearance-translation", false, [&] { RunAppearanceTranslation(base, source.empty() ? nullptr : source.string().c_str()); }},
        {"editor-real-save", true, [&] { TestRealSave(source); }},
        {"editor-real-art", true, [&] { TestRealArt(source); TestRealCustom(base, source); }},
        {"editor-real-hooks", true, [&] { TestAppearanceSites(source); }},
        {"editor-real-overlay", true,
         [&] {
             TestRealCustomOverlay(base, source);
             TestRealHelmetHiddenOverlay(base, source);
             TestRemoteLookParity(base, source);
             TestCustomBaseInvariant(base, source);
         }},
        {"editor-real-parts", true,
         [&] {
             TestFeather(base, source);
             TestNoHeadgear(base, source);
             TestFighterHornBuilds(base, source);
             TestAppearancePrepare(base, source);
             TestPartMapping(base, source);
             TestPartCacheVersion(base, source);
         }},
    };
}

// Runs the named groups (all when `selected` is empty) and prints one GROUP line each; returns the failures.
int RunEditorGroups(const fs::path& base, const fs::path& source, const std::set<std::string>& selected) {
    const bool data = GameDataPresent(source);
    for (const EditorGroup& group : EditorGroups(base, source)) {
        if (!selected.empty() && !selected.count(group.name)) continue;
        const int checks = g_checks, failures = g_failures;
        const auto started = std::chrono::steady_clock::now();
        group.run();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const char* state = g_failures != failures ? "FAIL" : group.needs_game && !data ? "NO_DATA" : "PASS";
        std::printf("GROUP|%s|%s|checks=%d|ms=%lld\n", group.name, state, g_checks - checks, ms);
        std::fflush(stdout);
    }
    return g_failures;
}

} // namespace

int main(int argc, char** argv) {
    const fs::path base = fs::temp_directory_path() / "dcp_save_editor_tests";
    fs::remove_all(base);
    fs::create_directories(base);
#ifdef DCP_SOURCE_DIR
    const fs::path source = DCP_SOURCE_DIR;
#else
    const fs::path source;
#endif
    const std::string mode = argc > 1 ? argv[1] : "all";
    g_synthetic_only = argc > 2 && std::strcmp(argv[2], "synthetic") == 0;
    std::set<std::string> selected;
    if (mode == "appearance-translation") {
        selected = {"appearance-translation"};
    } else if (mode == "release") {
        for (const EditorGroup& group : EditorGroups(base, source)) selected.insert(group.name);
    } else if (mode == "quick") {
        g_synthetic_only = true;
        for (const EditorGroup& group : EditorGroups(base, source)) {
            if (!group.needs_game) selected.insert(group.name);
        }
    } else if (mode != "all") {
        std::printf("usage: dcp-save-editor-tests [all | release | quick | appearance-translation [synthetic]]\n");
        return 2;
    }
    RunEditorGroups(base, source, selected);
    std::error_code cleanup;
    fs::remove_all(base, cleanup);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
