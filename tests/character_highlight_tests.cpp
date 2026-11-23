// Character Highlight, stage 1: the setting, the F7 hotkey, the atlas signature table and the runtime image match.
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "atlas_signatures.h"
#include "character_highlight.h"
#include "character_highlight_core.h"
#include "character_highlight_draw.h"
#include "cpk_fixture.h"
#include "dcp_launcher.h"
#include "dcp_keys.h"
#include "dcp_settings.h"
#include "guest_hook.h"
#include "settings.h"

namespace fs = std::filesystem;
namespace hl = dcp::highlight;

namespace {

int failures = 0;
#define CHECK(cond)                                         \
    do {                                                    \
        if (!(cond)) {                                      \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                     \
        }                                                   \
    } while (0)

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string Source(const char* relative) {
    return ReadAll(fs::path(DCP_SOURCE_DIR) / relative);
}

void WriteAll(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string Texels(size_t size, uint32_t seed) {
    std::string bytes(size, '\0');
    uint32_t state = seed * 2654435761u + 1;
    for (char& c : bytes) {
        state = state * 1664525u + 1013904223u;
        c = static_cast<char>(state >> 24);
    }
    return bytes;
}

void Put32(std::string& out, size_t at, uint32_t value) {
    std::memcpy(out.data() + at, &value, sizeof(value));
}

struct SyntheticTexture {
    uint32_t width, height;
    std::string texels;
};

std::string BuildFtx(const std::vector<SyntheticTexture>& textures) {
    std::string out(0x100, '\0');
    std::memcpy(out.data(), "FTEX", 4);
    Put32(out, 12, static_cast<uint32_t>(textures.size()));
    for (const SyntheticTexture& texture : textures) {
        const size_t at = out.size();
        out.resize(at + 0x100, '\0');
        Put32(out, at, 0x20464e47);
        Put32(out, at + 4, 0xf8);
        Put32(out, at + 5 * 4, 0x02900008);
        Put32(out, at + 6 * 4, (texture.width - 1) | ((texture.height - 1) << 14));
        Put32(out, at + 11 * 4, static_cast<uint32_t>(texture.texels.size()));
        out += texture.texels;
        out.resize(out.size() + 0x40, '\0');
    }
    return out;
}

const std::vector<SyntheticTexture>& Atlas(uint32_t seed) {
    static std::vector<std::vector<SyntheticTexture>> cache(16);
    auto& atlas = cache[seed];
    if (atlas.empty()) {
        atlas = {{1024, 1024, Texels(1u << 20, seed)}, {1024, 2048, Texels(2u << 20, seed + 100)}};
    }
    return atlas;
}

void TestSettingAndHotkey() {
    for (const dcp::ManagedSetting& setting : dcp::ManagedSettings()) {
        CHECK(setting.key != "character_highlight");
    }
    int chord_rows = 0;
    for (const dcp::ManagedSetting& setting : dcp::ManagedSettings()) {
        chord_rows += setting.key == dcp::kControllerCharacterHighlightKey && setting.default_value == "back+r3";
    }
    CHECK(chord_rows == 1 && std::string(dcp::kControllerCharacterHighlightKey) == hl::kChordKey);
    CHECK(hl::kChords[hl::kDefaultChord].first == hl::PadButton::Back && hl::kChords[hl::kDefaultChord].second == hl::PadButton::RightStick);
    CHECK(std::string(hl::kChords[hl::kDefaultChord].label) == "View / Back + R3");
    for (const hl::Chord& chord : hl::kChords) {
        CHECK(dcp::IsValidSetting(dcp::kControllerCharacterHighlightKey, chord.name));
        if (std::string(chord.name) == "off") continue;
        const bool options = (chord.first == hl::PadButton::LeftStick && chord.second == hl::PadButton::RightStick) ||
                             (chord.first == hl::PadButton::RightStick && chord.second == hl::PadButton::LeftStick);
        const bool exit = chord.first == hl::PadButton::LeftShoulder && chord.second == hl::PadButton::RightShoulder;
        CHECK(!options && !exit);
    }
    CHECK(!dcp::IsValidSetting(dcp::kControllerCharacterHighlightKey, "l3+r3") && !dcp::IsValidSetting(dcp::kControllerCharacterHighlightKey, "banana"));
    CHECK(hl::ChordFromName("back+l3") == 1 && hl::ChordFromName("off") == hl::kChordOff && hl::ChordFromName("") == -1);

    const fs::path file = fs::temp_directory_path() / "dcp-highlight-tests" / "dcp.ini";
    fs::remove_all(file.parent_path());
    WriteAll(file, "controller_character_highlight = l1+r3\n");
    DcpSettings::Values values;
    CHECK(values.character_highlight_chord == 0);
    CHECK(DcpSettings::Read(file.string(), values) && values.character_highlight_chord == 2);
    WriteAll(file, "controller_character_highlight = banana\n");
    CHECK(DcpSettings::Read(file.string(), values) && values.character_highlight_chord == 2);
    WriteAll(file, "hotkey_character_highlight = h\n");
    DcpSettings::Values keyed;
    CHECK(keyed.character_highlight_key == SDL_SCANCODE_F7);
    CHECK(DcpSettings::Read(file.string(), keyed) && keyed.character_highlight_key == SDL_SCANCODE_H);

    const DcpKeyInfo& action = dcp_key_actions[DCP_KEY_HOTKEY_CHARACTER_HIGHLIGHT];
    CHECK(std::string(action.ini_key) == "hotkey_character_highlight" && std::string(action.label) == "Character Highlight");
    CHECK(action.default_scancode == SDL_SCANCODE_F7 && DCP_KEY_HOTKEY_CHARACTER_HIGHLIGHT >= DCP_KEY_FIRST_HOTKEY);
    int hotkey_rows = 0;
    for (const dcp::ManagedSetting& setting : dcp::ManagedSettings()) {
        hotkey_rows += setting.key == "hotkey_character_highlight" && setting.default_value == "f7";
    }
    CHECK(hotkey_rows == 1);

    DcpKeyMap keys;
    dcp_keys_defaults(&keys);
    CHECK(keys.scancode[DCP_KEY_HOTKEY_CHARACTER_HIGHLIGHT] == SDL_SCANCODE_F7);
    CHECK(dcp_keys_apply(&keys, "hotkey_character_highlight", "f8") && keys.scancode[DCP_KEY_HOTKEY_CHARACTER_HIGHLIGHT] == SDL_SCANCODE_F8);
    CHECK(!dcp_keys_apply(&keys, "hotkey_character_highlight", "notakey") && keys.scancode[DCP_KEY_HOTKEY_CHARACTER_HIGHLIGHT] == SDL_SCANCODE_F8);
    dcp_keys_load(file.string().c_str(), &keys);
    char name[32];
    CHECK(keys.scancode[DCP_KEY_HOTKEY_CHARACTER_HIGHLIGHT] == SDL_SCANCODE_H);
    CHECK(std::string(dcp_key_name(keys.scancode[DCP_KEY_HOTKEY_CHARACTER_HIGHLIGHT], name, sizeof(name))) == "h");

    for (int other = 0; other < DCP_KEY_COUNT; ++other) {
        if (other == DCP_KEY_HOTKEY_CHARACTER_HIGHLIGHT) continue;
        CHECK(dcp_key_actions[other].default_scancode != SDL_SCANCODE_F7);
    }
    for (const int key : {SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4}) CHECK(key != SDL_SCANCODE_F7);
    fs::remove_all(file.parent_path());
}

void TestNaming() {
    CHECK(std::string(hl::ClassName(0)) == "Fighter" && std::string(hl::ClassName(1)) == "Amazon" &&
          std::string(hl::ClassName(2)) == "Elf" && std::string(hl::ClassName(3)) == "Dwarf" &&
          std::string(hl::ClassName(4)) == "Sorceress" && std::string(hl::ClassName(5)) == "Wizard");
    CHECK(hl::ClassName(-1) == nullptr && hl::ClassName(hl::kClassCount) == nullptr);
    CHECK(hl::AtlasPath(1, 0) == "player/Amazon00.ftx" && hl::AtlasPath(1, 4) == "player/Amazon04.ftx");
    for (int klass = 0; klass < hl::kClassCount; ++klass) {
        for (int color = 0; color <= hl::kMaxColor; ++color) {
            const std::string path = hl::AtlasPath(klass, color);
            CHECK(path == std::string("player/") + hl::ClassName(klass) + "0" + std::to_string(color) + ".ftx");
        }
    }
    CHECK(hl::AtlasPath(0, -1).empty() && hl::AtlasPath(0, hl::kMaxColor + 1).empty() && hl::AtlasPath(6, 0).empty());
}

void TestTable() {
    const std::string ftx = BuildFtx(Atlas(1));
    const auto textures = hl::ParseFtx(reinterpret_cast<const uint8_t*>(ftx.data()), ftx.size());
    CHECK(textures && textures->size() == 2);
    if (!textures) return;
    CHECK((*textures)[0].width == 1024 && (*textures)[0].height == 1024 && (*textures)[0].data_size == (1u << 20));
    CHECK((*textures)[1].width == 1024 && (*textures)[1].height == 2048 && (*textures)[1].data_size == (2u << 20));
    CHECK(!hl::ParseFtx(reinterpret_cast<const uint8_t*>(ftx.data()), 40));
    std::string broken = ftx;
    broken.replace(0, 4, "NOPE");
    CHECK(!hl::ParseFtx(reinterpret_cast<const uint8_t*>(broken.data()), broken.size()));
    std::string truncated = ftx.substr(0, ftx.size() / 2);
    CHECK(!hl::ParseFtx(reinterpret_cast<const uint8_t*>(truncated.data()), truncated.size()));

    const auto entries = hl::EntriesFor(1, 0, reinterpret_cast<const uint8_t*>(ftx.data()), ftx.size());
    CHECK(entries && entries->size() == 2 && (*entries)[0].signature != (*entries)[1].signature);
    if (!entries) return;
    const std::string text = hl::FormatTable(*entries);
    const auto parsed = hl::ParseTable(text);
    CHECK(parsed && parsed->size() == 2);
    if (parsed) {
        for (size_t i = 0; i < 2; ++i) {
            CHECK((*parsed)[i].klass == 1 && (*parsed)[i].color == 0 && (*parsed)[i].texture == static_cast<int>(i));
            CHECK((*parsed)[i].signature == (*entries)[i].signature && (*parsed)[i].texels.width == (*entries)[i].texels.width &&
                  (*parsed)[i].texels.height == (*entries)[i].texels.height && (*parsed)[i].texels.data_size == (*entries)[i].texels.data_size);
        }
    }
    CHECK(text.size() < 512 && text.find(':') == std::string::npos && text.find('/') == std::string::npos &&
          text.find('\\') == std::string::npos);
    CHECK(!hl::ParseTable("garbage\n") && !hl::ParseTable("") && !hl::ParseTable(std::string(hl::kTableHeader) + "\natlas class=9 color=0\n"));

    const std::string other = BuildFtx(Atlas(2));
    const auto different = hl::EntriesFor(1, 0, reinterpret_cast<const uint8_t*>(other.data()), other.size());
    CHECK(different && (*different)[0].signature != (*entries)[0].signature);
    CHECK(hl::SampleSignature(reinterpret_cast<const uint8_t*>(ftx.data()), 0) == hl::SampleSignature(reinterpret_cast<const uint8_t*>(ftx.data()), 0));
}

void TestGenerator() {
    const fs::path base = fs::temp_directory_path() / "dcp-highlight-generator";
    fs::remove_all(base);
    const fs::path game = base / "game", out = base / "image";
    fs::create_directories(game);

    CHECK(dcp::BuildAtlasSignatures(game, out).empty() && !fs::exists(out / hl::kTableFile));

    const std::string amazon = BuildFtx(Atlas(1)), fighter = BuildFtx(Atlas(2));
    WriteAll(game / "DRACRO.CPK", cpk_build::BuildCpkWith({{"player/Amazon00.ftx", amazon}, {"player/Fighter03.ftx", fighter}, {"stage/other.ftx", "x"}}));
    CHECK(dcp::BuildAtlasSignatures(game, out).empty());
    const std::string text = ReadAll(out / hl::kTableFile);
    const auto entries = hl::ParseTable(text);
    CHECK(entries && entries->size() == 4);
    if (entries) {
        int amazon_rows = 0, fighter_rows = 0;
        for (const hl::AtlasEntry& entry : *entries) {
            amazon_rows += entry.klass == 1 && entry.color == 0;
            fighter_rows += entry.klass == 0 && entry.color == 3;
        }
        CHECK(amazon_rows == 2 && fighter_rows == 2);
    }
    CHECK(text.find(base.string()) == std::string::npos && text.find("FTEX") == std::string::npos);
    CHECK(!fs::exists(out / (std::string(hl::kTableFile) + ".tmp")));

    WriteAll(game / "DRACRO.CPK", cpk_build::BuildCpkWith({{"player/Amazon00.ftx", "not an ftx"}}));
    CHECK(!dcp::BuildAtlasSignatures(game, out).empty());
    CHECK(ReadAll(out / hl::kTableFile) == text);

    WriteAll(game / "DRACRO.CPK", cpk_build::BuildCpkWith({{"stage/other.ftx", "x"}}));
    CHECK(dcp::BuildAtlasSignatures(game, out).empty() && !fs::exists(out / hl::kTableFile));
    fs::remove_all(base);
}

std::vector<uint64_t> g_vtable, g_game_object, g_party;
std::array<std::vector<uint64_t>, 4> g_units;
std::array<uint32_t, 4> g_custom_key{};
std::array<bool, 4> g_remote_custom{};

uint64_t GUEST_HOOK_ABI FakeGameOfUnit(uint64_t) {
    return reinterpret_cast<uint64_t>(g_game_object.data());
}

uint32_t FakeLook(int slot, uint64_t, int, int, int owner) {
    if (owner == 1) return g_remote_custom[static_cast<size_t>(slot)] ? CHARACTER_HIGHLIGHT_REMOTE_CUSTOM : CHARACTER_HIGHLIGHT_NATIVE_LOOK;
    return g_custom_key[static_cast<size_t>(slot)];
}

void SetSeat(int slot, int klass, int color, int32_t owner = 0, uint32_t flags = 0, int32_t pad = -1) {
    unsigned char* seat = reinterpret_cast<unsigned char*>(g_party.data()) + 0xa8 + static_cast<size_t>(slot) * 0x23b0;
    seat[0x27] = static_cast<unsigned char>(klass);
    seat[0x28] = static_cast<unsigned char>(color);
    std::memcpy(seat + 0x21e0, &owner, sizeof(owner));
    std::memcpy(seat + 0x21e4, &flags, sizeof(flags));
    std::memcpy(seat + 0x21dc, &pad, sizeof(pad));
}

void Observe(int slot, int times) {
    for (int i = 0; i < times; ++i) character_highlight_observe_local_unit(reinterpret_cast<uint64_t>(g_units[static_cast<size_t>(slot)].data()));
}

void ObserveAll() {
    for (int slot = 0; slot < 4; ++slot) Observe(slot, 61);
}

void BuildParty() {
    g_vtable.assign(0x100 / 8, 0);
    g_game_object.assign(0x100 / 8, 0);
    g_party.assign((0xa8 + 4 * 0x23b0 + 16) / 8, 0);
    g_vtable[0xd0 / 8] = reinterpret_cast<uint64_t>(&FakeGameOfUnit);
    g_game_object[0x80 / 8] = reinterpret_cast<uint64_t>(g_party.data());
    g_custom_key = {};
    g_remote_custom = {};
    for (int slot = 0; slot < 4; ++slot) {
        auto& unit = g_units[static_cast<size_t>(slot)];
        unit.assign(0x4000 / 8, 0);
        unit[0] = reinterpret_cast<uint64_t>(g_vtable.data());
        const int32_t value = slot;
        std::memcpy(reinterpret_cast<unsigned char*>(unit.data()) + 0xa70, &value, sizeof(value));
        SetSeat(slot, 0, 0, -1);
    }
}

uint64_t Address(const std::string& s) { return reinterpret_cast<uint64_t>(s.data()); }

hl::PlayerColor Color(bool r, bool g, bool b) {
    return {r, g, b};
}

struct Images {
    std::string amazon, elf, custom, net;
};

Images& Textures() {
    static Images images{Atlas(1)[0].texels, Atlas(2)[0].texels, Atlas(5)[0].texels, Atlas(6)[0].texels};
    return images;
}

void RegisterImages() {
    Images& t = Textures();
    for (const auto& [id, texels] : {std::pair<uint32_t, const std::string*>{7, &t.amazon}, {8, &t.elf}, {9, &t.custom}, {10, &t.net}}) {
        character_highlight_image_registered(id, Address(*texels), texels->size(), 1024, 1024);
    }
}

void LoadTables() {
    const fs::path base = fs::temp_directory_path() / "dcp-highlight-runtime";
    fs::remove_all(base);
    CHECK(character_highlight_load_table((base / "missing.txt").string().c_str()) == -1);
    const std::string amazon = BuildFtx(Atlas(1)), elf = BuildFtx(Atlas(2)), custom = BuildFtx(Atlas(5));
    auto rows = *hl::EntriesFor(1, 0, reinterpret_cast<const uint8_t*>(amazon.data()), amazon.size());
    const auto more = *hl::EntriesFor(2, 0, reinterpret_cast<const uint8_t*>(elf.data()), elf.size());
    rows.insert(rows.end(), more.begin(), more.end());
    WriteAll(base / "bad.txt", "nothing useful\n");
    CHECK(character_highlight_load_table((base / "bad.txt").string().c_str()) == -1);
    WriteAll(base / hl::kTableFile, hl::FormatTable(rows));
    CHECK(character_highlight_load_table((base / hl::kTableFile).string().c_str()) == 4);
    CHECK(character_highlight_add_custom_atlas("PLAYER/ELF00_D3C01_1E5D3BC5.FTX", 2, 0, reinterpret_cast<const uint8_t*>(custom.data()), custom.size()) == 2);
    CHECK(character_highlight_add_custom_atlas("PLAYER/BAD.FTX", 2, 0, reinterpret_cast<const uint8_t*>("junk"), 4) == 0);
    fs::remove_all(base);
    RegisterImages();
}

void TestProcessStartsOff() {
    BuildParty();
    for (int slot = 0; slot < 4; ++slot) CHECK(character_highlight_player_enabled(slot) == 0);
    CHECK(character_highlight_local_slot(0) == -1 && character_highlight_toggle_local(0) == -1);
    character_highlight_set_look_resolver(&FakeLook);
    LoadTables();
}

void TestOfflineLocalCoop() {
    BuildParty();
    SetSeat(0, 1, 0);
    SetSeat(1, 2, 0);
    SetSeat(2, 3, 0, 0, 1);
    ObserveAll();
    CHECK(character_highlight_local_slot(0) == 0 && character_highlight_local_slot(1) == 1 && character_highlight_local_slot(2) == -1);
    CHECK(character_highlight_toggle_local(0) == 0 && character_highlight_toggle_local(2) == -1 && character_highlight_toggle_local(-1) == -1);
    CHECK(character_highlight_player_enabled(0) == 1 && character_highlight_player_enabled(1) == 0 && character_highlight_player_enabled(2) == 0);
    CHECK(character_highlight_slot_for_image(7) == 0 && character_highlight_slot_for_image(8) == -1);
    CHECK(character_highlight_slot_for_address(Address(Textures().amazon)) == 0 &&
          character_highlight_slot_for_address(Address(Textures().elf)) == -1);
    CHECK(character_highlight_toggle_local(1) == 1);
    CHECK(character_highlight_slot_for_image(7) == 0 && character_highlight_slot_for_image(8) == 1);
    CHECK(character_highlight_slot_for_address(Address(Textures().amazon)) == 0 &&
          character_highlight_slot_for_address(Address(Textures().elf)) == 1);
    CHECK(character_highlight_toggle_local(0) == 0);
    CHECK(character_highlight_slot_for_image(7) == -1 && character_highlight_slot_for_image(8) == 1);
    CHECK(character_highlight_toggle_local(1) == 1);
    for (int slot = 0; slot < 4; ++slot) CHECK(character_highlight_player_enabled(slot) == 0);
    CHECK(character_highlight_slot_for_image(9) == -1 && character_highlight_slot_for_image(10) == -1 && character_highlight_slot_for_image(99) == -1);
}

void TestInstanceAHostAndInstanceBGuest() {
    BuildParty();
    SetSeat(0, 1, 0, 0);
    SetSeat(1, 2, 0, 1);
    ObserveAll();
    CHECK(character_highlight_local_slot(0) == 0 && character_highlight_local_slot(1) == -1);
    CHECK(character_highlight_toggle_local(1) == -1 && character_highlight_player_enabled(1) == 0);
    CHECK(character_highlight_toggle_local(0) == 0);
    CHECK(character_highlight_slot_for_image(7) == 0);
    CHECK(character_highlight_slot_for_image(8) == -1);
    CHECK(hl::ColorOfPlayer(character_highlight_slot_for_image(7)) == Color(true, false, false));
    CHECK(character_highlight_toggle_local(0) == 0 && character_highlight_player_enabled(0) == 0);

    BuildParty();
    SetSeat(0, 1, 0, 1);
    SetSeat(1, 2, 0, 0);
    ObserveAll();
    CHECK(character_highlight_local_slot(0) == 1 && character_highlight_local_slot(1) == -1);
    CHECK(character_highlight_toggle_local(1) == -1);
    CHECK(character_highlight_toggle_local(0) == 1);
    CHECK(character_highlight_player_enabled(0) == 0 && character_highlight_player_enabled(1) == 1);
    CHECK(character_highlight_slot_for_image(8) == 1);
    CHECK(hl::ColorOfPlayer(character_highlight_slot_for_image(8)) == Color(false, false, true));
    CHECK(character_highlight_slot_for_image(7) == -1);
    CHECK(character_highlight_toggle_local(0) == 1 && character_highlight_player_enabled(1) == 0);
}

void TestControllerPadBinding() {
    BuildParty();
    SetSeat(0, 2, 0, 1, 0, 255);
    SetSeat(1, 2, 0, 0, 0, 0);
    ObserveAll();
    CHECK(character_highlight_toggle_pad(0) == 1 && character_highlight_player_enabled(1) == 1 && character_highlight_player_enabled(0) == 0);
    CHECK(character_highlight_toggle_pad(1) == -1 && character_highlight_toggle_pad(255) == -1 && character_highlight_toggle_pad(-1) == -1);
    CHECK(character_highlight_toggle_pad(0) == 1 && character_highlight_player_enabled(1) == 0);

    BuildParty();
    SetSeat(0, 5, 0, 1, 0, 255);
    SetSeat(1, 2, 0, 0, 0, 1);
    SetSeat(2, 1, 0, 0, 0, 0);
    ObserveAll();
    CHECK(character_highlight_local_slot(0) == 1 && character_highlight_local_slot(1) == 2);
    CHECK(character_highlight_toggle_pad(0) == 2 && character_highlight_player_enabled(2) == 1 && character_highlight_player_enabled(1) == 0);
    CHECK(character_highlight_slot_for_image(7) == 2 && character_highlight_slot_for_image(8) == -1);
    CHECK(hl::ColorOfPlayer(character_highlight_slot_for_image(7)) == Color(false, true, false));
    CHECK(character_highlight_toggle_pad(1) == 1 && character_highlight_player_enabled(1) == 1 && character_highlight_player_enabled(2) == 1);
    CHECK(hl::ColorOfPlayer(character_highlight_slot_for_image(8)) == Color(false, false, true));
    CHECK(character_highlight_toggle_pad(0) == 2 && character_highlight_toggle_pad(1) == 1);
    CHECK(character_highlight_toggle_pad(2) == -1 && character_highlight_toggle_pad(3) == -1);
    CHECK(character_highlight_toggle_local(0) == 1 && character_highlight_player_enabled(1) == 1);
    CHECK(character_highlight_toggle_local(0) == 1 && character_highlight_player_enabled(1) == 0);

    BuildParty();
    SetSeat(0, 1, 0, 1, 0, 0);
    SetSeat(1, 2, 0, 0, 0, 1);
    ObserveAll();
    CHECK(character_highlight_toggle_pad(0) == -1);
    for (int slot = 0; slot < 4; ++slot) CHECK(character_highlight_player_enabled(slot) == 0);
    CHECK(character_highlight_toggle_pad(1) == 1 && character_highlight_toggle_pad(1) == 1);

    BuildParty();
    for (int slot = 0; slot < 4; ++slot) SetSeat(slot, slot == 0 ? 1 : 2, 0, 0, slot == 0 ? 0 : 0, slot);
    ObserveAll();
    for (int pad = 0; pad < 4; ++pad) {
        CHECK(character_highlight_toggle_pad(pad) == pad);
        for (int other = 0; other < 4; ++other) CHECK(character_highlight_player_enabled(other) == (other == pad ? 1 : 0));
        CHECK(character_highlight_toggle_pad(pad) == pad);
    }
}

void TestRemoteExclusionAndStaleSlots() {
    BuildParty();
    SetSeat(0, 2, 0, 1);
    SetSeat(1, 2, 0, 0);
    ObserveAll();
    CHECK(character_highlight_toggle_local(0) == 1);
    CHECK(character_highlight_slot_for_image(8) == -1);
    CHECK(character_highlight_toggle_local(0) == 1);

    BuildParty();
    SetSeat(0, 1, 0, 0);
    SetSeat(1, 2, 0, 0);
    ObserveAll();
    CHECK(character_highlight_toggle_local(1) == 1 && character_highlight_slot_for_image(8) == 1);
    SetSeat(1, 2, 0, 1);
    Observe(1, 61);
    CHECK(character_highlight_player_enabled(1) == 0 && character_highlight_slot_for_image(8) == -1);
    SetSeat(1, 2, 0, 0);
    Observe(1, 61);
    CHECK(character_highlight_player_enabled(1) == 0 && character_highlight_slot_for_image(8) == -1);
    CHECK(character_highlight_toggle_local(1) == 1 && character_highlight_toggle_local(1) == 1);
}

void TestCustomLooks() {
    BuildParty();
    g_custom_key[1] = character_highlight_custom_key("PLAYER/ELF00_D3C01_1E5D3BC5.FTX");
    SetSeat(0, 2, 0, 1);
    SetSeat(1, 2, 0, 0);
    ObserveAll();
    CHECK(character_highlight_toggle_local(0) == 1);
    CHECK(character_highlight_slot_for_image(9) == 1);
    CHECK(character_highlight_slot_for_image(8) == -1);
    CHECK(character_highlight_slot_for_image(10) == -1);
    CHECK(character_highlight_toggle_local(0) == 1);

    g_remote_custom[0] = true;
    SetSeat(0, 2, 0, 1);
    g_custom_key[1] = CHARACTER_HIGHLIGHT_NATIVE_LOOK;
    Observe(0, 61);
    Observe(1, 61);
    CHECK(character_highlight_toggle_local(0) == 1);
    CHECK(character_highlight_slot_for_image(8) == 1);
    CHECK(character_highlight_slot_for_image(10) == -1 && character_highlight_slot_for_image(9) == -1);
    CHECK(character_highlight_toggle_local(0) == 1);
}

void TestSharedAtlasAmbiguity() {
    BuildParty();
    SetSeat(0, 2, 0, 1);
    SetSeat(1, 2, 0, 0);
    ObserveAll();
    CHECK(character_highlight_toggle_local(0) == 1);
    CHECK(character_highlight_slot_for_image(8) == -1);
    CHECK(character_highlight_toggle_local(0) == 1);

    BuildParty();
    SetSeat(0, 2, 0, 0);
    SetSeat(1, 2, 0, 0);
    ObserveAll();
    CHECK(character_highlight_toggle_local(0) == 0);
    CHECK(character_highlight_toggle_local(1) == 1);
    CHECK(character_highlight_slot_for_image(8) == -1);
    CHECK(character_highlight_toggle_local(0) == 0 && character_highlight_toggle_local(1) == 1);
    SetSeat(1, 1, 0, 0);
    Observe(1, 61);
    CHECK(character_highlight_toggle_local(0) == 0 && character_highlight_slot_for_image(8) == 0);
    CHECK(character_highlight_toggle_local(0) == 0);
}

void TestImageCacheCounts() {
    const unsigned hashed = character_highlight_hash_count();
    Images& t = Textures();
    for (int i = 0; i < 1000; ++i) character_highlight_slot_for_image(7);
    Observe(0, 300);
    CHECK(character_highlight_hash_count() == hashed);
    character_highlight_image_registered(5, Address(t.amazon), t.amazon.size(), 1024, 512);
    character_highlight_image_registered(5, Address(t.amazon), 4096, 1024, 1024);
    character_highlight_image_registered(5, 0, t.amazon.size(), 1024, 1024);
    CHECK(character_highlight_hash_count() == hashed);
    character_highlight_image_unregistered(7);
    BuildParty();
    SetSeat(0, 1, 0, 0);
    ObserveAll();
    CHECK(character_highlight_toggle_local(0) == 0 && character_highlight_slot_for_image(7) == -1);
    CHECK(character_highlight_toggle_local(0) == 0);
    character_highlight_set_look_resolver(nullptr);
}

struct Mask {
    int width, height;
    std::vector<float> coverage;

    Mask(int width_, int height_) : width(width_), height(height_), coverage(size_t(width) * size_t(height), 0.0f) {}

    float& at(int x, int y) {
        return coverage[size_t(y) * size_t(width) + size_t(x)];
    }
    float at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0.0f;
        return coverage[size_t(y) * size_t(width) + size_t(x)];
    }
    void cover(int x0, int y0, int x1, int y1, float value) {
        for (int y = std::max(y0, 0); y < std::min(y1, height); ++y) {
            for (int x = std::max(x0, 0); x < std::min(x1, width); ++x) {
                at(x, y) = std::max(at(x, y), value);
            }
        }
    }
    void clear() {
        std::fill(coverage.begin(), coverage.end(), 0.0f);
    }
};

float SampleMask(const Mask& mask, float x, float y) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const float top = mask.at(x0, y0) * (1.0f - fx) + mask.at(x0 + 1, y0) * fx;
    const float bottom = mask.at(x0, y0 + 1) * (1.0f - fx) + mask.at(x0 + 1, y0 + 1) * fx;
    return top * (1.0f - fy) + bottom * fy;
}

float NeighborMax(const Mask& mask, int x, int y, float radius) {
    std::array<float, hl::kOutlineDirections.size()> values;
    for (size_t i = 0; i < hl::kOutlineDirections.size(); ++i) {
        const hl::Offset direction = hl::kOutlineDirections[i];
        values[i] = SampleMask(mask, static_cast<float>(x) + static_cast<float>(direction.x) * radius,
                               static_cast<float>(y) + static_cast<float>(direction.y) * radius);
    }
    return hl::MaxCoverage(values);
}

float OutlineAt(const Mask& mask, int x, int y, float radius) {
    return hl::OutlineCoverage(mask.at(x, y), NeighborMax(mask, x, y, radius));
}

void TestMaskEmpty() {
    const Mask mask(16, 16);
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) CHECK(OutlineAt(mask, x, y, 1.75f) == 0.0f);
    }
}

void TestMaskSinglePixel() {
    Mask mask(11, 11);
    mask.at(5, 5) = 1.0f;
    CHECK(OutlineAt(mask, 5, 5, 1.0f) == 0.0f);
    for (const hl::Offset direction : hl::kOutlineDirections) {
        CHECK(OutlineAt(mask, 5 + direction.x, 5 + direction.y, 1.0f) > 0.0f);
    }
}

void TestMaskBlock() {
    const float radius = hl::OutlinePixels(1080.0f);
    Mask mask(24, 24);
    mask.cover(6, 6, 18, 18, 1.0f);
    for (int y = 6; y < 18; ++y) {
        for (int x = 6; x < 18; ++x) CHECK(OutlineAt(mask, x, y, radius) == 0.0f);
    }
    CHECK(OutlineAt(mask, 5, 12, radius) > 0.0f);
    CHECK(OutlineAt(mask, 18, 12, radius) > 0.0f);
    CHECK(OutlineAt(mask, 12, 5, radius) > 0.0f);
    CHECK(OutlineAt(mask, 12, 18, radius) > 0.0f);
}

void TestMaskOverlap() {
    Mask mask(32, 32);
    mask.cover(6, 6, 20, 20, 0.9f);
    mask.cover(14, 14, 28, 28, 0.5f);
    for (int y = 6; y < 28; ++y) {
        for (int x = 6; x < 28; ++x) {
            if (mask.at(x, y) > hl::kCoverageThreshold) CHECK(OutlineAt(mask, x, y, 1.75f) == 0.0f);
        }
    }
    CHECK(OutlineAt(mask, 5, 10, 1.75f) > 0.0f && OutlineAt(mask, 28, 24, 1.75f) > 0.0f);
    CHECK(OutlineAt(mask, 17, 17, 1.75f) == 0.0f);
}

void TestMaskMultipart() {
    const float radius = hl::OutlinePixels(1080.0f);
    Mask mask(64, 64);
    mask.cover(18, 8, 42, 50, 0.8f);  // chest
    mask.cover(34, 18, 54, 36, 0.6f); // arm
    mask.cover(12, 28, 26, 58, 0.5f); // cape
    int covered = 0, outline = 0;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            const float coverage = mask.at(x, y);
            const float edge = OutlineAt(mask, x, y, radius);
            if (coverage > hl::kCoverageThreshold) {
                ++covered;
                CHECK(edge == 0.0f);
            } else if (edge > 0.0f) {
                ++outline;
            }
        }
    }
    CHECK(covered > 0 && outline > 0);
}

void TestMaskSemiTransparentCenter() {
    CHECK(hl::OutlineCoverage(0.4f, 1.0f) == 0.0f);
    CHECK(hl::OutlineCoverage(hl::kCoverageThreshold, 1.0f) > 0.0f);
    CHECK(hl::OutlineCoverage(0.0f, 0.5f) == 0.5f);
    CHECK(hl::OutlineCoverage(0.5f, 0.0f) == 0.0f);
    CHECK(hl::MaxCoverage(std::array{0.0f, 0.25f, 0.75f, 0.5f}) == 0.75f);
}

void TestMaskClear() {
    Mask mask(32, 32);
    mask.cover(4, 4, 12, 12, 1.0f);
    CHECK(OutlineAt(mask, 3, 8, 1.75f) > 0.0f);
    mask.clear();
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) CHECK(OutlineAt(mask, x, y, 1.75f) == 0.0f);
    }
    mask.cover(20, 20, 28, 28, 1.0f);
    CHECK(OutlineAt(mask, 3, 8, 1.75f) == 0.0f);
    CHECK(OutlineAt(mask, 19, 24, 1.75f) > 0.0f);
}

void TestMaskSlotGroups() {
    Mask mask(32, 32);
    mask.cover(4, 4, 12, 12, 1.0f);
    CHECK(mask.at(8, 8) == 1.0f && OutlineAt(mask, 3, 8, 1.75f) > 0.0f);
    mask.clear();
    mask.cover(20, 20, 28, 28, 1.0f);
    CHECK(OutlineAt(mask, 3, 8, 1.75f) == 0.0f);
    CHECK(OutlineAt(mask, 19, 24, 1.75f) > 0.0f);
}

void TestMaskRegression() {
    const float kRadius = hl::OutlinePixels(1080.0f);
    for (int step = 0; step < 8; ++step) {
        Mask mask(96, 96);
        const int dx = step * 2;
        const int dy = step;
        mask.cover(20 + dx, 10 + dy, 50 + dx, 60 + dy, 0.6f);  // torso
        mask.cover(42 + dx, 25 + dy, 68 + dx, 45 + dy, 0.7f);  // arm
        mask.cover(15 + dx, 35 + dy, 32 + dx, 70 + dy, 0.4f);  // cape
        int outline = 0;
        for (int y = 0; y < mask.height; ++y) {
            for (int x = 0; x < mask.width; ++x) {
                const float edge = OutlineAt(mask, x, y, kRadius);
                if (mask.at(x, y) > hl::kCoverageThreshold) {
                    CHECK(edge == 0.0f);
                } else if (edge > 0.0f) {
                    ++outline;
                }
            }
        }
        CHECK(outline > 0);
        CHECK(OutlineAt(mask, 18 + dx, 10 + dy, kRadius) > 0.0f);
        CHECK(OutlineAt(mask, 50 + dx, 55 + dy, kRadius) > 0.0f);
    }
}

void TestOutlineColorPath() {
    const std::array<uint32_t, 4> identity = hl::kIdentityColorSwizzle;
    const std::array<uint32_t, 4> bgr = {6, 5, 4, 7};
    const std::array<std::array<float, 4>, 4> expected = {{
        {1.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 0.0f, 1.0f},
    }};
    for (int slot = 0; slot < 4; ++slot) {
        CHECK(hl::OutlineColorOfPlayer(slot) == expected[static_cast<size_t>(slot)]);
        for (const auto& swizzle : {identity, bgr}) {
            const hl::OutlineSettings settings = hl::MakeOutlineSettings(slot, 1920.0f, 1080.0f, 1.75f, swizzle);
            CHECK(settings.color == expected[static_cast<size_t>(slot)]);
            CHECK(settings.size[0] == 1920.0f && settings.size[1] == 1080.0f);
            CHECK(settings.pixels == 1.75f && settings.pad == 0.0f && settings.swizzle == swizzle);
            const auto stored = hl::ApplyColorSwizzle(settings.color, settings.swizzle);
            CHECK(hl::ApplyColorSwizzle(stored, settings.swizzle) == expected[static_cast<size_t>(slot)]);
        }
    }
    CHECK(sizeof(hl::OutlineSettings) == 48);
    CHECK(offsetof(hl::OutlineSettings, color) == 0);
    CHECK(offsetof(hl::OutlineSettings, size) == 16);
    CHECK(offsetof(hl::OutlineSettings, pixels) == 24);
    CHECK(offsetof(hl::OutlineSettings, swizzle) == 32);

    const std::array<float, 4> blue{0.0f, 0.0f, 1.0f, 1.0f};
    const auto slot0_stored = hl::ApplyColorSwizzle(hl::OutlineColorOfPlayer(0), bgr);
    CHECK(slot0_stored == blue);
    CHECK(hl::ApplyColorSwizzle(slot0_stored, bgr) == expected[0]);
    const auto slot1_stored = hl::ApplyColorSwizzle(hl::OutlineColorOfPlayer(1), bgr);
    CHECK(hl::ApplyColorSwizzle(slot1_stored, bgr) == expected[1]);
    const auto slot3_stored = hl::ApplyColorSwizzle(hl::OutlineColorOfPlayer(3), bgr);
    CHECK(hl::ApplyColorSwizzle(slot3_stored, bgr) == expected[3]);
    CHECK(hl::MakeOutlineSettings(0, 1.0f, 1.0f, 1.0f, bgr).color != blue);
}

void TestOutlineThickness() {
    constexpr float kOld1080 = 1.75f;
    const float p720 = hl::OutlinePixels(720.0f);
    const float p1080 = hl::OutlinePixels(1080.0f);
    const float p1440 = hl::OutlinePixels(1440.0f);
    const float p2160 = hl::OutlinePixels(2160.0f);
    CHECK(p720 >= 1.5f && p720 <= 1.75f);
    CHECK(p1080 >= 2.25f && p1080 <= 2.5f);
    CHECK(p1440 >= 3.0f && p1440 <= 3.25f);
    CHECK(p2160 >= 4.5f && p2160 <= 5.0f);
    CHECK(p1080 > kOld1080 * 1.25f && p1080 <= kOld1080 * 1.35f + 0.001f);

    for (int slot = 0; slot < 4; ++slot) {
        const hl::OutlineSettings settings =
            hl::MakeOutlineSettings(slot, 1920.0f, 1080.0f, p1080, hl::kIdentityColorSwizzle);
        CHECK(settings.pixels == p1080);
    }

    Mask mask(40, 40);
    mask.cover(10, 10, 30, 30, 1.0f);
    CHECK(OutlineAt(mask, 9, 20, p1080) > 0.0f);
    CHECK(OutlineAt(mask, 8, 20, p1080) > 0.0f);
    CHECK(OutlineAt(mask, 7, 20, p1080) > 0.0f);
    CHECK(OutlineAt(mask, 6, 20, p1080) == 0.0f);
    CHECK(OutlineAt(mask, 7, 20, kOld1080) == 0.0f);
    for (int y = 10; y < 30; ++y) {
        for (int x = 10; x < 30; ++x) CHECK(OutlineAt(mask, x, y, p1080) == 0.0f);
    }
}

void TestOutlinePlan() {
    CHECK(hl::kOutlineDirections.size() == 8);
    for (size_t i = 0; i < hl::kOutlineDirections.size(); ++i) {
        const hl::Offset o = hl::kOutlineDirections[i];
        CHECK(!(o.x == 0 && o.y == 0) && o.x >= -1 && o.x <= 1 && o.y >= -1 && o.y <= 1);
        for (size_t j = 0; j < i; ++j) CHECK(!(o == hl::kOutlineDirections[j]));
    }
    CHECK(hl::OutlinePixels(720.0f) >= 1.5f && hl::OutlinePixels(720.0f) <= 1.75f);
    CHECK(hl::OutlinePixels(1080.0f) >= 2.25f && hl::OutlinePixels(1080.0f) <= 2.5f);
    CHECK(hl::OutlinePixels(1440.0f) >= 3.0f && hl::OutlinePixels(1440.0f) <= 3.25f);
    CHECK(hl::OutlinePixels(2160.0f) >= 4.5f && hl::OutlinePixels(2160.0f) <= 5.0f);
    CHECK(hl::OutlinePixels(8640.0f) == hl::kOutlineMaxPixels && hl::OutlinePixels(100.0f) == hl::kOutlineMinPixels);
    CHECK(hl::OutlinePixels(-1080.0f) == hl::OutlinePixels(1080.0f));
    CHECK(hl::OutlinePixels(0.0f) == 0.0f && hl::OutlinePixels(0.5f) == 0.0f && hl::OutlinePixels(1e9f) == 0.0f &&
          hl::OutlinePixels(std::nanf("")) == 0.0f);
    CHECK(hl::kCoverageThreshold > 0.0f && hl::kCoverageThreshold < 0.01f);

    CHECK(hl::kPlayerColors.size() == 4);
    CHECK(hl::ColorOfPlayer(0) == Color(true, false, false));
    CHECK(hl::ColorOfPlayer(1) == Color(false, false, true));
    CHECK(hl::ColorOfPlayer(2) == Color(false, true, false));
    CHECK(hl::ColorOfPlayer(3) == Color(true, true, false));
    for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < a; ++b) CHECK(!(hl::ColorOfPlayer(a) == hl::ColorOfPlayer(b)));
    }
    CHECK(hl::ColorOfPlayer(-1) == hl::ColorOfPlayer(0) && hl::ColorOfPlayer(9) == hl::ColorOfPlayer(0));
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::string Between(const std::string& text, const std::string& from, const std::string& to) {
    const size_t start = text.find(from);
    if (start == std::string::npos) return {};
    const size_t end = text.find(to, start + from.size());
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

void TestSources() {
    const std::string window = Source("gpu/shim/window.cpp"), settings = Source("gpu/shim/dcp_settings.cpp");
    const std::string settings_h = Source("gpu/shim/dcp_settings.h"), launcher = Source("launcher/launcher.cpp");
    const std::string loader = Source("src/loader.c"), cache = Source("gpu/shadps4/video_core/texture_cache/texture_cache.cpp");
    const std::string runtime = Source("src/character_highlight.cpp"), pt = Source("launcher/localization/pt_br_ui_text.cpp");
    const std::string launcher_settings = Source("launcher/settings.cpp"), launcher_settings_h = Source("launcher/settings.h");
    const std::string docs = Source("docs/LAUNCHER.md"), window_h = Source("gpu/shim/sdl_window.h");

    CHECK(Contains(window, "event.key.scancode == DcpSettings::Get().character_highlight_key") &&
          Contains(window, "character_highlight_toggle_local(0);") && !Contains(window, "character_highlight_toggle_player"));
    CHECK(Contains(settings_h, "character_highlight_key{SDL_SCANCODE_F7}") && Contains(settings, "&v.character_highlight_key};") &&
          Contains(settings, "dcp::highlight::kChordKey"));
    CHECK(!Contains(settings_h, "std::atomic<bool> character_highlight{") && !Contains(settings, "key == \"character_highlight\""));

    const std::string gameplay = Between(launcher, "ui.Toggle(\"Story Skip Offer\"", "ui.EndRows();");
    CHECK(!gameplay.empty() && !Contains(gameplay, "Character Highlight"));
    CHECK(!Contains(launcher, "kCharacterHighlightHelp") && !Contains(launcher, "kCharacterHighlightKey") &&
          !Contains(launcher_settings, "kCharacterHighlightKey") && !Contains(launcher_settings_h, "kCharacterHighlightKey") &&
          !Contains(launcher_settings, "dcp_character_highlight"));
    const size_t controls = launcher.find("void PageControls(trg::Ui& ui) {");
    const size_t shortcuts = launcher.find("ui.Section(\"Controller shortcuts\", true)", controls);
    const size_t chord_row = launcher.find("ui.Choice(\"Character Highlight\", kCharacterHighlightChordHelp, dcp::highlight::kChordKey", shortcuts);
    CHECK(controls != std::string::npos && shortcuts != std::string::npos && shortcuts < chord_row);
    CHECK(Contains(pt, "Character Highlight\tDestaque do personagem\n"));
    CHECK(Contains(pt, "Highlights your locally controlled character on this screen only") &&
          Contains(pt, "Destaca somente o personagem controlado localmente nesta tela"));
    CHECK(Contains(launcher, "Highlights your locally controlled character on this screen only") && !Contains(launcher, "Offline only"));
    CHECK(!Contains(pt, "Offline only") && !Contains(pt, "Apenas offline"));
    CHECK(Contains(docs, "### Character Highlight") && Contains(docs, "no Gameplay setting") && Contains(docs, "never sent to another client") && !Contains(docs, "offline only") && Contains(docs, "`controller_character_highlight`") &&
          !Contains(docs, "| `character_highlight` |"));

    CHECK(Contains(window_h, "void UpdateHighlightChord();"));
    const std::string chord = Between(window, "void WindowSDL::UpdateHighlightChord()", "// Window thread. A DCP_MOTION_HELD");
    CHECK(Contains(chord, "dcp::highlight::kChordOff") && Contains(chord, "down && !was_down") &&
          Contains(chord, "character_highlight_toggle_pad(runtime_pad_player_of_instance(entry.first))") &&
          !Contains(chord, "printf") && !Contains(chord, "nline") && !Contains(chord, "available"));
    CHECK(Contains(window, "UpdateExitChord();\n    UpdateHighlightChord();"));
    CHECK(Contains(Source("src/runtime_pad.c"), "int runtime_pad_player_of_instance(uint32_t instance) {") &&
          Contains(Source("src/runtime_pad.c"), "dcp_pad_owners_slot_of(&owners,instance)"));

    CHECK(Contains(loader, "character_highlight_set_look_resolver(highlight_look);") &&
          Contains(loader, "character_highlight_load_table(atlas_table);") &&
          Contains(loader, "runtime_cheats_set_player_observer(character_highlight_observe_unit);") &&
          Contains(loader, "party_hook_add_unit_listener(character_highlight_observe_local_unit);") &&
          Contains(loader, "party_hook_add_unit_listener(comms_bridge_unit_tick);") &&
          !Contains(loader, "party_hook_set_unit_listener") && !Contains(loader, "highlight_party_unit") &&
          Contains(loader, "if (owner==1) return dcp_remote_appearance_of(slot,klass,color) ? CHARACTER_HIGHLIGHT_REMOTE_CUSTOM") &&
          !Contains(loader, "online_query") && !Contains(loader, "highlight_online_active") && !Contains(loader, "character_highlight_configure"));
    CHECK(Contains(Source("launcher/appearance_runtime.cpp"), "character_highlight_add_custom_atlas(instance.entry.c_str()"));
    CHECK(Contains(cache, "character_highlight_image_registered(image_id.index") &&
          Contains(cache, "character_highlight_image_unregistered(image_id.index);"));
    CHECK(!Contains(cache, "SilhouetteView") && !Contains(cache, "character_highlight_draw.h"));

    const std::string rasterizer = Source("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp");
    const std::string header = Source("gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h");
    const std::string mask_cpp = Source("gpu/shadps4/video_core/renderer_vulkan/vk_highlight_mask.cpp");
    const std::string mask_h = Source("gpu/shadps4/video_core/renderer_vulkan/vk_highlight_mask.h");
    const std::string pipeline_cpp = Source("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp");
    const std::string pipeline_h = Source("gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.h");
    const std::string draw = Source("src/character_highlight_draw.h");
    const std::string outline = Source("gpu/shadps4/video_core/host_shaders/character_highlight_outline.frag");
    const std::string draw_record = Between(rasterizer, "void Rasterizer::DrawRecord(", "void Rasterizer::DrawIndirect(");
    const std::string start = Between(rasterizer, "void Rasterizer::StartHighlightGroup(", "void Rasterizer::RecordHighlightMask(");
    const std::string mask_draw = Between(rasterizer, "void Rasterizer::RecordHighlightMask(", "void Rasterizer::FlushHighlightOutline()");
    const std::string flush = Between(rasterizer, "void Rasterizer::FlushHighlightOutline()", "void Rasterizer::UpdateViewportScissorState()");
    CHECK(!draw_record.empty() && !start.empty() && !mask_draw.empty() && !flush.empty());

    CHECK(Contains(draw_record, "const int highlight_slot = HighlightSlotForDraw();") &&
          Contains(draw_record, "if (highlight.active && !HighlightGroupContinues(highlight_slot))") &&
          Contains(draw_record, "if (highlight.active && !HighlightContinuesEarly(pipeline))") &&
          Contains(draw_record, "FlushHighlightOutline();") && Contains(draw_record, "StartHighlightGroup(highlight_slot, pipeline, state)") &&
          Contains(draw_record, "RecordHighlightMask(pipeline, is_indexed, num_indices"));
    CHECK(draw_record.find("HighlightContinuesEarly(pipeline)") < draw_record.find("PrepareRenderState(pipeline)"));
    CHECK(Contains(rasterizer, "character_highlight_slot_for_address(address) == highlight.slot"));
    CHECK(draw_record.find("if (highlight.active && !HighlightGroupContinues(highlight_slot))") < draw_record.find("BeginRendering(pipeline)"));
    CHECK(draw_record.find("RecordHighlightMask(pipeline") < draw_record.find("scheduler.Record([=](vk::CommandBuffer cmdbuf) {\n        if (skip_water_reader)"));
    size_t original_records = 0;
    for (size_t at = draw_record.find("scheduler.Record([=](vk::CommandBuffer cmdbuf) {\n        if (skip_water_reader)"); at != std::string::npos;
         at = draw_record.find("scheduler.Record([=](vk::CommandBuffer cmdbuf) {\n        if (skip_water_reader)", at + 1)) ++original_records;
    CHECK(original_records == 1);

    CHECK(Contains(start, "highlight_mask.Acquire(state.width, state.height)") &&
          Contains(start, "highlight.color = cb_descs[0].first") && Contains(start, "highlight.depth = db_desc.first") &&
          Contains(start, "highlight.clear_mask = true") && !Contains(start, "Silhouette"));
    CHECK(Contains(mask_draw, "scheduler.EndRendering();") && Contains(mask_draw, "highlight_mask.TransitionToAttachment(*highlight.mask)") &&
          Contains(mask_draw, "is_clear = highlight.clear_mask ? 1u : 0u") &&
          Contains(mask_draw, "scheduler.BeginRendering(mask_state)") && Contains(mask_draw, "scheduler.BeginRendering(restore)") &&
          Contains(mask_draw, "cmdbuf.setDepthWriteEnable(false)") && Contains(mask_draw, "cmdbuf.setDepthWriteEnable(depth_write)") &&
          Contains(mask_draw, "cmdbuf.setStencilTestEnable(false)") && Contains(mask_draw, "cmdbuf.setStencilTestEnable(stencil_test)") &&
          Contains(mask_draw, "depth_clear = false"));
    CHECK(mask_draw.find("scheduler.BeginRendering(mask_state)") < mask_draw.find("cmdbuf.bindPipeline") &&
          mask_draw.find("cmdbuf.draw") < mask_draw.find("scheduler.BeginRendering(restore)"));
    CHECK(!Contains(mask_draw, "SilhouetteView") && !Contains(mask_draw, "SampleSignature") && !Contains(mask_draw, "std::vector") &&
          !Contains(mask_draw, "new "));

    CHECK(Contains(flush, "highlight_mask.Composite(highlight.target, highlight.color_format, highlight.samples") &&
          Contains(flush, "dcp::highlight::OutlinePixels(highlight.target.height)") &&
          Contains(flush, "outline active slot=%d") && Contains(flush, "highlight_announced") && Contains(flush, "highlight = {};"));
    size_t prints = 0;
    for (size_t at = flush.find("printf"); at != std::string::npos; at = flush.find("printf", at + 1)) ++prints;
    CHECK(prints == 1 && !Contains(flush, "DEBUG_SAULO") && !Contains(flush, "DCP_TRACE_CHARACTER_HIGHLIGHT"));

    CHECK(Contains(mask_h, "struct Extent") && Contains(mask_h, "kMaxExtents = 2") && Contains(mask_h, "CharacterHighlightMask") &&
          Contains(mask_h, "kMaxPipelines = 8"));
    CHECK(Contains(mask_cpp, "extent.width == width && extent.height == height") && Contains(mask_cpp, "extent.image.Create") &&
          Contains(mask_cpp, "vk::Format::eR8G8B8A8Unorm") && Contains(mask_cpp, "TransitionToAttachment") &&
          Contains(mask_cpp, "TransitionToSample") && Contains(mask_cpp, "vk::ColorComponentFlagBits::eA") &&
          Contains(mask_cpp, "vk::CullModeFlagBits::eNone") && Contains(mask_cpp, "scheduler.EndRendering();") &&
          Contains(mask_cpp, "scheduler.BeginRendering(state)") && !Contains(mask_cpp, "std::vector") &&
          !Contains(mask_cpp, "mapMemory") && !Contains(mask_cpp, "SampleSignature"));
    CHECK(!Contains(mask_cpp, "new ") && !Contains(mask_cpp, "printf"));

    CHECK(Contains(pipeline_h, "vk::Pipeline HighlightMaskPipeline() const") && Contains(pipeline_h, "mutable vk::Pipeline mask_pipeline") &&
          Contains(pipeline_h, "mask_vertex_attributes") && Contains(pipeline_h, "std::mutex mask_pipeline_mutex"));
    CHECK(Contains(pipeline_cpp, "GraphicsPipeline::HighlightMaskPipeline") && Contains(pipeline_cpp, "CreateHighlightMaskPipeline") &&
          Contains(pipeline_cpp, "vk::BlendOp::eMax") && Contains(pipeline_cpp, "colorWriteMask = vk::ColorComponentFlagBits::eA") &&
          Contains(pipeline_cpp, "mask_tcs_module") && Contains(pipeline_cpp, "mask_fragment_module") &&
          !Contains(pipeline_cpp, "SampleSignature") && !Contains(pipeline_cpp, "DCP_TRACE_CHARACTER_HIGHLIGHT"));

    CHECK(Contains(draw, "OutlineCoverage") && Contains(draw, "kCoverageThreshold") && Contains(draw, "MaxCoverage") &&
          Contains(draw, "ColorOfPlayer") && Contains(draw, "OutlinePixels") && !Contains(draw, "kMaxOutlineDraws") &&
          !Contains(draw, "OutlineDrawCount"));
    CHECK(Contains(outline, "texture(maskSampler") && Contains(outline, "directions[8]") && Contains(outline, "center > (0.5 / 255.0)") &&
          Contains(outline, "max(around - center, 0.0)") && !Contains(outline, "blur") && !Contains(outline, "pulse"));
    CHECK(Contains(outline, "uvec4 color_swizzle") && Contains(outline, "swizzle_component") &&
          !Contains(outline, "outline_color.bgr") && !Contains(outline, "outline_color.bgra"));
    CHECK(Contains(draw, "struct OutlineSettings") && Contains(draw, "MakeOutlineSettings") &&
          Contains(draw, "ApplyColorSwizzle") && Contains(draw, "kIdentityColorSwizzle") &&
          Contains(draw, "OutlineColorOfPlayer"));
    CHECK(Contains(rasterizer, "column.swizzle.array[i]") && Contains(flush, "highlight.color_swizzle") &&
          Contains(mask_cpp, "MakeOutlineSettings") && Contains(mask_cpp, "sizeof(settings)"));

    for (const char* gone : {"SilhouetteView", "silhouette_snapshot", "normal_snapshot", "BuildHighlightWrites",
                             "RecordHighlightOutline", "kMaxOutlineDraws", "OutlineDrawCount", "setViewportWithCount(shifted)",
                             "character_highlight_descriptors", "HighlightSnapshot"}) {
        CHECK(!Contains(rasterizer, gone) && !Contains(header, gone) && !Contains(draw, gone) && !Contains(mask_cpp, gone));
    }
    CHECK(!fs::exists(fs::path(DCP_SOURCE_DIR) / "src/character_highlight_descriptors.h"));

    const std::string query = Between(runtime, "extern \"C\" int character_highlight_slot_for_image", "\n}\n");
    const std::string observe = Between(runtime, "extern \"C\" void character_highlight_observe_unit", "\n}\n");
    CHECK(!query.empty() && !Contains(query, "printf") && !Contains(query, "SampleSignature") && Contains(query, "enabled == 0"));
    CHECK(!observe.empty() && !Contains(observe, "printf"));
    CHECK(!Contains(runtime, "SendTo") && !Contains(runtime, "socket") && !Contains(runtime, "lobby") && !Contains(runtime, "online") &&
          !Contains(runtime, "Online") && !Contains(runtime, "g_online"));
    for (const char* file : {"src/character_highlight.h", "src/character_highlight_core.h", "src/character_highlight_draw.h"}) {
        CHECK(!Contains(Source(file), "online") && !Contains(Source(file), "Online Play"));
    }
    const fs::path root = fs::path(DCP_SOURCE_DIR);
    for (const fs::path& dir : {root / "src" / "net"}) {
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string text = ReadAll(entry.path());
            CHECK(!Contains(text, "character_highlight") && !Contains(text, "CharacterHighlight"));
        }
    }
    for (const char* file : {"src/runtime_online.cpp", "src/runtime_online.h", "src/appearance_net.cpp", "src/appearance_net.h",
                             "src/appearance_session.cpp", "src/comms_session.cpp", "src/comms_bridge.cpp", "src/early_story_session.c",
                             "src/runtime_matching2.cpp", "src/runtime_rudp.cpp", "launcher/online_client.cpp", "launcher/online_settings.cpp",
                             "src/runtime_savedata.c", "launcher/save_data.cpp"}) {
        const std::string text = Source(file);
        CHECK(!text.empty() && !Contains(text, "character_highlight") && !Contains(text, "CharacterHighlight"));
    }
    CHECK(!Contains(Source("src/net/online_version.h"), "highlight"));

    const std::string debug_tag = std::string("DEBUG_") + "SAULO CharacterHighlight";
    const std::string trace_env = std::string("DCP_TRACE_") + "CHARACTER_HIGHLIGHT";
    for (const char* file : {"src/character_highlight.cpp", "src/character_highlight.h", "src/character_highlight_core.h", "src/loader.c",
                             "gpu/shim/window.cpp", "gpu/shadps4/video_core/texture_cache/texture_cache.cpp", "launcher/atlas_signatures.cpp",
                             "launcher/boot_image.cpp", "tools/build.ps1", "CMakeLists.txt", "src/character_highlight_draw.h",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_highlight_mask.cpp", "gpu/shadps4/video_core/renderer_vulkan/vk_highlight_mask.h",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp", "gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.h",
                             "gpu/shadps4/video_core/host_shaders/character_highlight_outline.frag",
                             "gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp", "gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h", "launcher/launcher.cpp"}) {
        const std::string text = Source(file);
        CHECK(!text.empty() && !Contains(text, debug_tag) && !Contains(text, trace_env));
    }
    CHECK(!fs::exists(fs::path(DCP_SOURCE_DIR) / "src/character_highlight_trace.c"));
}

} // namespace

int main() {
    TestSettingAndHotkey();
    TestNaming();
    TestTable();
    TestGenerator();
    TestProcessStartsOff();
    TestOfflineLocalCoop();
    TestInstanceAHostAndInstanceBGuest();
    TestControllerPadBinding();
    TestRemoteExclusionAndStaleSlots();
    TestCustomLooks();
    TestSharedAtlasAmbiguity();
    TestImageCacheCounts();
    TestMaskEmpty();
    TestMaskSinglePixel();
    TestMaskBlock();
    TestMaskOverlap();
    TestMaskMultipart();
    TestMaskSemiTransparentCenter();
    TestMaskClear();
    TestMaskSlotGroups();
    TestMaskRegression();
    TestOutlineColorPath();
    TestOutlineThickness();
    TestOutlinePlan();
    TestSources();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("Character Highlight tests passed\n");
    return 0;
}
