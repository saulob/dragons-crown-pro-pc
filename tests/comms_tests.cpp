// Comms Wheel core, words and native bridge: ids, radial layout, hold input, wire body, limits, localization, usage counts
// and the balloon bridge on synthetic memory and on the real boot images when they are on disk.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "character_highlight_core.h"
#include "comms_bridge.h"
#include "comms_text.h"
#include "dcp_comms.h"
#include "dcp_pad_buttons.h"

using namespace dcp::comms;

static int failures;
#define CHECK(x)                                                \
    do {                                                        \
        if (!(x)) {                                             \
            ++failures;                                         \
            std::printf("FAIL line %d: %s\n", __LINE__, #x);    \
        }                                                       \
    } while (0)

static void TestIds() {
    CHECK(kMessageCount == 8);
    CHECK(static_cast<int>(Message::kHello) == 0 && static_cast<int>(Message::kThanks) == 1 &&
          static_cast<int>(Message::kGoodJob) == 2 && static_cast<int>(Message::kFollowMe) == 3 &&
          static_cast<int>(Message::kGroupUp) == 4 && static_cast<int>(Message::kNeedHelp) == 5 &&
          static_cast<int>(Message::kReady) == 6 && static_cast<int>(Message::kLetsGo) == 7);
    const char* keys[] = {"hello", "thanks", "good_job", "follow_me", "group_up", "need_help", "ready", "lets_go"};
    for (int i = 0; i < 8; ++i) {
        CHECK(ValidId(i) && std::string(IdKey(i)) == keys[i]);
    }
    CHECK(!ValidId(-1) && !ValidId(8) && !ValidId(255) && *IdKey(8) == '\0');
    CHECK(kSectors == 8 && kSectorMessages.size() == 8);
}

static void TestLayout() {
    CHECK(MessageAtSector(0) == static_cast<int>(Message::kReady));
    CHECK(MessageAtSector(1) == static_cast<int>(Message::kHello));
    CHECK(MessageAtSector(2) == static_cast<int>(Message::kNeedHelp));
    CHECK(MessageAtSector(3) == static_cast<int>(Message::kLetsGo));
    CHECK(MessageAtSector(4) == static_cast<int>(Message::kGroupUp));
    CHECK(MessageAtSector(5) == static_cast<int>(Message::kFollowMe));
    CHECK(MessageAtSector(6) == static_cast<int>(Message::kGoodJob));
    CHECK(MessageAtSector(7) == static_cast<int>(Message::kThanks));
    CHECK(MessageAtSector(-1) == -1 && MessageAtSector(8) == -1);
    bool seen[8] = {};
    for (int s = 0; s < 8; ++s) {
        seen[MessageAtSector(s)] = true;
        CHECK(SectorOfMessage(MessageAtSector(s)) == s);
    }
    for (bool b : seen) {
        CHECK(b);
    }
    CHECK(SectorOfMessage(9) == -1);
}

static void TestSectors() {
    const float z = kStickDeadZone;
    CHECK(SectorAt(0, -1, z) == 0);   // up
    CHECK(SectorAt(1, -1, z) == 1);   // up-right
    CHECK(SectorAt(1, 0, z) == 2);    // right
    CHECK(SectorAt(1, 1, z) == 3);    // down-right
    CHECK(SectorAt(0, 1, z) == 4);    // down
    CHECK(SectorAt(-1, 1, z) == 5);   // down-left
    CHECK(SectorAt(-1, 0, z) == 6);   // left
    CHECK(SectorAt(-1, -1, z) == 7);  // up-left
    // 45 degree sectors: 22.5 degrees either side of up stay in sector 0
    CHECK(SectorAt(0.38f, -1, z) == 0 && SectorAt(-0.38f, -1, z) == 0);
    CHECK(SectorAt(0.45f, -1, z) == 1 && SectorAt(-0.45f, -1, z) == 7);
    // dead zone
    CHECK(SectorAt(0, 0, z) == -1 && SectorAt(0.3f, 0.3f, z) == -1 && SectorAt(0.49f, 0, z) == -1);
    CHECK(SectorAt(0.5f, 0, z) == 2);
    CHECK(SectorAt(NAN, 1, z) == -1 && SectorAt(1, INFINITY, z) == -1);
    int counts[8] = {};
    for (int deg = 0; deg < 360; ++deg) {
        const float r = deg * 3.14159265f / 180.0f;
        const int s = SectorAt(std::sin(r), -std::cos(r), z);
        CHECK(s >= 0 && s < 8);
        if (s >= 0) {
            ++counts[s];
        }
    }
    for (int c : counts) {
        CHECK(c == 45);
    }
}

static WheelInput Pad(bool chord_down, float x = 0, float y = 0, bool available = true) {
    WheelInput in;
    in.available = available;
    in.pad_buttons = chord_down ? static_cast<uint8_t>(kPadBack | kPadDpadUp) : 0;
    in.stick_x = x;
    in.stick_y = y;
    return in;
}

static WheelInput Key(bool down, float mx = 0, float my = 0, bool available = true) {
    WheelInput in;
    in.available = available;
    in.key_down = down;
    in.mouse_x = mx;
    in.mouse_y = my;
    return in;
}

static void TestControllerLatches() {
    WheelController wheel;
    WheelOutput out = wheel.Step(Pad(false));
    CHECK(!out.open && !out.hide_pad_up);
    out = wheel.Step(Pad(true));
    CHECK(out.open && out.source == Source::kController && out.hide_pad_up && out.hide_confirm && out.hide_cancel &&
          out.hide_right_stick && out.selected == -1);
    out = wheel.Step(Pad(false));
    CHECK(out.open && out.send == -1 && out.hide_pad_up);
    out = wheel.Step(Pad(false, 1, 0));
    CHECK(out.open && out.selected == 2);
    out = wheel.Step(Pad(false, 0, 0));
    CHECK(out.open && out.selected == 2 && out.send == -1);
    out = wheel.Step(Pad(false, 0, 1));
    CHECK(out.selected == 4);
    out = wheel.Step(Pad(false, 0.1f, 0.1f));
    CHECK(out.open && out.selected == 4);
    WheelInput cross = Pad(false);
    cross.confirm_held = true;
    out = wheel.Step(cross);
    CHECK(out.send == static_cast<int>(Message::kGroupUp) && !out.open && out.hide_confirm && !wheel.Active());
    out = wheel.Step(cross);
    CHECK(out.send == -1 && out.hide_confirm && !out.open);
    out = wheel.Step(Pad(false));
    CHECK(!out.hide_confirm && !out.hide_pad_up && !out.hide_cancel && !out.hide_right_stick);
}

static void TestHeldOpenerStaysHidden() {
    WheelController wheel;
    wheel.Step(Pad(true));
    WheelInput cancel = Pad(true);
    cancel.cancel_held = true;
    WheelOutput out = wheel.Step(cancel);
    CHECK(!out.open && out.send == -1 && out.hide_pad_up && out.hide_cancel);
    out = wheel.Step(Pad(true));
    CHECK(!out.open && out.hide_pad_up);
    out = wheel.Step(Pad(false));
    CHECK(!out.hide_pad_up && !out.hide_cancel);
    out = wheel.Step(Pad(true));
    CHECK(out.open);
}

static void TestControllerCancelAndEmptyConfirm() {
    WheelController wheel;
    wheel.Step(Pad(true));
    wheel.Step(Pad(false, 1, 0));
    WheelInput circle = Pad(false, 1, 0);
    circle.cancel_held = true;
    WheelOutput out = wheel.Step(circle);
    CHECK(!out.open && out.send == -1 && out.hide_cancel && !wheel.Active());
    out = wheel.Step(Pad(false));
    CHECK(!out.hide_cancel);

    WheelController none;
    none.Step(Pad(true));
    WheelInput confirm = Pad(false, 0.2f, 0.1f);
    confirm.confirm_held = true;
    out = none.Step(confirm);
    CHECK(out.open && out.send == -1);

    WheelController held;
    WheelInput open = Pad(true);
    open.confirm_held = open.cancel_held = true;
    held.Step(open);
    open.pad_buttons = 0;
    open.stick_y = -1;
    out = held.Step(open);
    CHECK(out.open && out.selected == 0);
}

static void TestKeyboardLatches() {
    WheelController wheel;
    WheelOutput out = wheel.Step(Key(true));
    CHECK(out.open && out.source == Source::kKeyboard && out.hide_keyboard_axes && !out.hide_right_stick);
    out = wheel.Step(Key(false));
    CHECK(out.open && out.send == -1);
    out = wheel.Step(Key(false, -1, 0));
    CHECK(out.selected == 6);
    out = wheel.Step(Key(false, 0, 0));
    CHECK(out.open && out.selected == 6);
    WheelInput click = Key(false, 0, 0);
    click.click = true;
    out = wheel.Step(click);
    CHECK(out.send == static_cast<int>(Message::kGoodJob) && !out.open && !out.hide_keyboard_axes);
    out = wheel.Step(Key(false));
    CHECK(!out.open && out.send == -1);
}

static void TestKeyboardCancel() {
    for (int how = 0; how < 3; ++how) {
        WheelController wheel;
        wheel.Step(Key(true));
        wheel.Step(Key(false, 1, 0));
        WheelInput cancel = Key(false, 1, 0);
        cancel.cancel_click = how == 0;
        cancel.cancel_held = how == 1;
        cancel.click = how == 2;
        const WheelOutput out = wheel.Step(cancel);
        CHECK(out.send == (how == 2 ? static_cast<int>(Message::kNeedHelp) : -1) && !out.open);
    }
}

static void TestKeyboardNavigation() {
    WheelController wheel;
    wheel.Step(Key(true));
    WheelInput in = Key(false);
    const struct {
        uint8_t keys;
        int sector;
    } steps[] = {{kNavUp, 0},
                 {0, 0},
                 {kNavUp | kNavRight, 1},
                 {kNavRight, 1},
                 {0, 1},
                 {kNavRight, 2},
                 {kNavRight | kNavDown, 3},
                 {kNavDown, 3},
                 {0, 3},
                 {kNavDown, 4},
                 {kNavDown | kNavLeft, 5},
                 {kNavLeft, 5},
                 {0, 5},
                 {kNavLeft, 6},
                 {kNavLeft | kNavUp, 7},
                 {kNavUp, 7}};
    for (const auto& step : steps) {
        in.nav = step.keys;
        CHECK(wheel.Step(in).selected == step.sector);
    }
    in.nav = 0;
    wheel.Step(in);
    in.confirm_held = true;
    const WheelOutput out = wheel.Step(in);
    CHECK(out.send == static_cast<int>(Message::kThanks) && out.hide_confirm && !out.open);

    WheelController held;
    WheelInput start = Key(true);
    start.nav = kNavUp;
    held.Step(start);
    start.key_down = false;
    CHECK(held.Step(start).selected == -1);

    WheelController mixed;
    mixed.Step(Key(true));
    mixed.Step(Key(false, 0, -1));
    WheelInput right = Key(false, 0, -1);
    right.nav = kNavRight;
    CHECK(mixed.Step(right).selected == 2);
    CHECK(mixed.Step(right).selected == 2);
    CHECK(mixed.Step(Key(false, 0, 1)).selected == 4);
}

static void TestAvailability() {
    WheelController solo;
    WheelOutput out = solo.Step(Pad(true, 0, 0, false));
    CHECK(!solo.Active() && !out.hide_pad_up && !out.open);
    out = solo.Step(Key(true, 0, 0, false));
    CHECK(!solo.Active() && !out.open);

    CHECK(!WheelAvailable(true, 1, true, true));
    CHECK(!WheelAvailable(true, 0, true, true));
    CHECK(WheelAvailable(true, 2, true, true));
    CHECK(WheelAvailable(true, 4, true, true));
    CHECK(!WheelAvailable(false, 2, true, true));
    CHECK(!WheelAvailable(true, 2, false, true));
    CHECK(!WheelAvailable(true, 2, true, false));

    WheelController wheel;
    wheel.Step(Pad(true));
    wheel.Step(Pad(false, 1, 0));
    out = wheel.Step(Pad(false, 1, 0, false));
    CHECK(!out.open && out.send == -1);
    out = wheel.Step(Pad(false, 1, 0));
    CHECK(!out.open);
}

static void TestRateLimit() {
    RateLimit local(kLocalMinGapMs);
    CHECK(kLocalMinGapMs == 2000);
    CHECK(local.Allow(10000));
    CHECK(!local.Allow(10001) && !local.Allow(11999));
    CHECK(local.Allow(12000));
    CHECK(!local.Allow(13000));
    local.Reset();
    CHECK(local.Allow(13001));

    Inbox inbox;
    WireMessage m{1, 5, 7};
    CHECK(inbox.Accept(m, 5000));
    CHECK(!inbox.Accept(m, 5100));                        // the repeated datagram
    CHECK(!inbox.Accept(WireMessage{1, 6, 8}, 5200));     // too soon from the same slot
    CHECK(inbox.Accept(WireMessage{2, 6, 8}, 5200));      // another slot is independent
    CHECK(inbox.Accept(WireMessage{1, 6, 9}, 5000 + kRemoteMinGapMs));
    CHECK(!inbox.Accept(WireMessage{4, 0, 1}, 9000));     // slot out of range
    CHECK(!inbox.Accept(WireMessage{0, 8, 1}, 9000));     // id out of range
    int accepted = 0;
    Inbox flood;
    for (int i = 0; i < 100; ++i) {
        accepted += flood.Accept(WireMessage{3, 0, static_cast<uint8_t>(i)}, 20000 + i * 10) ? 1 : 0;
    }
    CHECK(accepted == 1);
}

static void TestWire() {
    CHECK(kWireBytes == 4 && sizeof(WireMessage) == 3);
    for (int slot = 0; slot < kSlots; ++slot) {
        for (int id = 0; id < kMessageCount; ++id) {
            uint8_t body[kWireBytes];
            EncodeWire(WireMessage{static_cast<uint8_t>(slot), static_cast<uint8_t>(id), 42}, body);
            const auto decoded = DecodeWire(body, sizeof(body));
            CHECK(decoded && decoded->slot == slot && decoded->id == id && decoded->seq == 42);
        }
    }
    uint8_t body[kWireBytes];
    EncodeWire(WireMessage{1, 3, 9}, body);
    CHECK(body[0] == kWireVersion && body[1] == 1 && body[2] == 3 && body[3] == 9);
    for (int id = 8; id < 256; ++id) {
        uint8_t bad[kWireBytes] = {kWireVersion, 0, static_cast<uint8_t>(id), 0};
        CHECK(!DecodeWire(bad, sizeof(bad)));
    }
    uint8_t bad_slot[kWireBytes] = {kWireVersion, 4, 0, 0};
    uint8_t bad_version[kWireBytes] = {kWireVersion + 1, 0, 0, 0};
    CHECK(!DecodeWire(bad_slot, sizeof(bad_slot)) && !DecodeWire(bad_version, sizeof(bad_version)));
    CHECK(!DecodeWire(body, kWireBytes - 1) && !DecodeWire(body, 0) && !DecodeWire(nullptr, kWireBytes));
    uint8_t text[32] = "Need help, follow me please";
    CHECK(!DecodeWire(text, sizeof(text)) && !DecodeWire(text, 27));
    uint8_t long_body[kWireBytes + 1] = {kWireVersion, 0, 0, 0, 0};
    CHECK(!DecodeWire(long_body, sizeof(long_body)));
}

static void TestText() {
    CHECK(COMMS_TEXT_MESSAGES == 8 && COMMS_TEXT_LANGUAGES == 10);
    CHECK(comms_text_set_language("english", 0) == 1 && !std::strcmp(comms_text_language(), "english"));
    const char* english[] = {"Hello", "Thanks", "Good job", "Follow me", "Group up", "Need help", "Ready", "Let's go"};
    for (int i = 0; i < 8; ++i) {
        CHECK(!std::strcmp(comms_text_message(i), english[i]));
        CHECK(!std::strcmp(comms_text_lookup("english", i), english[i]));
    }
    const char* portuguese[] = {"Olá", "Valeu", "Bom trabalho", "Me sigam", "Juntem-se", "Preciso de ajuda", "Pronto", "Vamos lá"};
    for (int i = 0; i < 8; ++i) {
        CHECK(!std::strcmp(comms_text_lookup("brazilian_portuguese", i), portuguese[i]));
    }
    CHECK(comms_text_set_language("brazilian_portuguese", 0) == 1);
    CHECK(!std::strcmp(comms_text_message(5), "Preciso de ajuda"));
    CHECK(*comms_text_message(8) == '\0' && *comms_text_message(-1) == '\0');
    CHECK(comms_text_lookup("klingon", 0) == nullptr && *comms_text_lookup("english", 9) == '\0');

    // two clients show the same id in their own language
    for (int id = 0; id < 8; ++id) {
        const std::string a = comms_text_lookup("english", id), b = comms_text_lookup("brazilian_portuguese", id);
        CHECK(!a.empty() && !b.empty());
    }
    CHECK(std::string(comms_text_lookup("english", 5)) != comms_text_lookup("brazilian_portuguese", 5));

    int languages = 0;
    for (int l = 0; comms_text_language_key(l); ++l) {
        ++languages;
        const char* key = comms_text_language_key(l);
        CHECK(comms_text_set_language(key, 0) == 1 && !std::strcmp(comms_text_language(), key));
        for (int id = 0; id < 8; ++id) {
            CHECK(*comms_text_message(id) != '\0');
            uint16_t units[64];
            const size_t n = comms_text_game_utf16(id, units, 64);
            CHECK(n > 0 && n <= 30 && units[n] == 0);
            if (std::strcmp(key, "english")) {
                CHECK(std::strcmp(comms_text_message(id), comms_text_lookup("english", id)) != 0);
            }
        }
    }
    CHECK(languages == COMMS_TEXT_LANGUAGES);
    CHECK(comms_text_language_key(10) == nullptr && comms_text_language_key(-1) == nullptr);

    comms_text_set_language("brazilian_portuguese", 0);
    uint16_t units[16];
    CHECK(comms_text_game_utf16(0, units, 16) == 3 && units[0] == 'O' && units[1] == 'l' && units[2] == 0xe1 && units[3] == 0);
    CHECK(comms_text_game_utf16(8, units, 16) == 0 && comms_text_game_utf16(0, units, 1) == 0 && comms_text_game_utf16(0, nullptr, 16) == 0);
    CHECK(comms_text_game_utf16(5, units, 8) == 6);
    comms_text_set_language("japanese", 0);
    CHECK(comms_text_game_utf16(0, units, 16) == 5 && units[0] == 0x3053);
    CHECK(comms_text_set_language("russian", 1) == 1 && !std::strcmp(comms_text_language(), "russian"));
    CHECK(comms_text_game_utf16(1, units, 16) > 0);
    comms_text_set_language("klingon", 0);
    CHECK(!std::strcmp(comms_text_language(), "english"));
    comms_text_set_language("english", 0);
}

static void TestUsage() {
    UsageCounts usage;
    CHECK(usage.Empty() && !usage.MostUsed() && usage.Percent(0) == 0.0);
    CHECK(!usage.Add(8) && !usage.Add(-1) && usage.total == 0);
    for (int i = 0; i < 5; ++i) {
        CHECK(usage.Add(5));
    }
    usage.Add(1, 3);
    usage.Add(0, 2);
    CHECK(usage.total == 10 && usage.by_id[5] == 5 && usage.by_id[1] == 3 && usage.by_id[0] == 2);
    CHECK(usage.Percent(5) == 50.0 && usage.Percent(1) == 30.0 && usage.Percent(7) == 0.0);
    CHECK(usage.MostUsed() && *usage.MostUsed() == 5);
    UsageCounts tie;
    tie.Add(6, 2);
    tie.Add(3, 2);
    CHECK(*tie.MostUsed() == 3);
    uint64_t sum = 0;
    for (uint64_t c : usage.by_id) {
        sum += c;
    }
    CHECK(sum == usage.total);
    UsageCounts big;
    big.Add(5, 1684);
    big.Add(1, 940);
    big.Add(0, 2696);
    CHECK(big.total == 5320 && big.Percent(5) > 31.6 && big.Percent(5) < 31.7);
}

// ---- native bridge on synthetic memory

static uint64_t g_now;
static uint64_t FakeClock() {
    return g_now;
}

struct FakeUnit {
    alignas(16) uint8_t bytes[0xb00];
};
struct FakeOwner {
    alignas(16) uint8_t bytes[0x2b00];
};

static void MakeUnit(FakeUnit& unit, FakeOwner& owner, int slot, bool human) {
    std::memset(&unit, 0, sizeof(unit));
    std::memset(&owner, 0, sizeof(owner));
    const uint64_t owner_address = reinterpret_cast<uintptr_t>(&owner);
    std::memcpy(unit.bytes + 0x1e0, &owner_address, 8);
    const int32_t s = slot;
    std::memcpy(unit.bytes + 0xa70, &s, 4);
    const uint32_t category = human ? 1u : 2u;
    std::memcpy(owner.bytes + 0x2a0c, &category, 4);
}

static std::vector<uint64_t> g_shown;
static std::vector<std::vector<uint16_t>> g_texts;
static bool g_accept = true;

static void GUEST_HOOK_ABI FakeShow(uint64_t owner) {
    g_shown.push_back(owner);
    if (!g_accept) {
        return;
    }
    GuestRegs regs;
    std::memset(&regs, 0, sizeof(regs));
    regs.r13 = owner;
    regs.rsi = 0x1234;
    comms_bridge_text_hook(&regs);
    const uint16_t* text = reinterpret_cast<const uint16_t*>(static_cast<uintptr_t>(regs.rsi));
    std::vector<uint16_t> copy;
    while (*text) {
        copy.push_back(*text++);
    }
    g_texts.push_back(copy);
}

static uint64_t Address(const void* p) {
    return reinterpret_cast<uintptr_t>(p);
}

static void TestBridgeSlots() {
    comms_text_set_language("english", 0);
    comms_bridge_attach(FakeShow, FakeClock);
    CHECK(comms_bridge_active());
    FakeUnit units[4];
    FakeOwner owners[4];
    for (int s = 0; s < 4; ++s) {
        MakeUnit(units[s], owners[s], s, true);
    }
    g_now = 100000;
    for (int s = 0; s < 4; ++s) {
        CHECK(!comms_bridge_unit_running(s));
        comms_bridge_unit_tick(Address(&units[s]));
        CHECK(comms_bridge_unit_running(s));
    }
    CHECK(g_shown.empty());
    for (int s = 0; s < 4; ++s) {
        g_shown.clear();
        g_texts.clear();
        const int id = 5 - s;
        comms_bridge_show(s, id);
        for (int u = 0; u < 4; ++u) {
            comms_bridge_unit_tick(Address(&units[u]));
        }
        CHECK(g_shown.size() == 1 && g_shown[0] == Address(&owners[s]));
        const char* expected = comms_text_message(id);
        CHECK(g_texts.size() == 1 && g_texts[0].size() == std::strlen(expected));
        for (size_t i = 0; i < g_texts[0].size() && i < std::strlen(expected); ++i) {
            CHECK(g_texts[0][i] == static_cast<uint16_t>(expected[i]));
        }
        g_shown.clear();
        comms_bridge_unit_tick(Address(&units[s]));
        CHECK(g_shown.empty());
        g_now += 10;
    }

    comms_bridge_show(-1, 0);
    comms_bridge_show(4, 0);
    comms_bridge_show(0, 8);
    comms_bridge_show(0, -1);
    g_shown.clear();
    comms_bridge_unit_tick(Address(&units[0]));
    CHECK(g_shown.empty());

    // a unit that is not a human player, a bad slot and a null unit never show anything
    FakeUnit cpu_unit, odd_unit;
    FakeOwner cpu_owner, odd_owner;
    MakeUnit(cpu_unit, cpu_owner, 1, false);
    MakeUnit(odd_unit, odd_owner, 7, true);
    comms_bridge_show(1, 2);
    comms_bridge_unit_tick(Address(&cpu_unit));
    comms_bridge_unit_tick(Address(&odd_unit));
    comms_bridge_unit_tick(0);
    CHECK(g_shown.empty());
    comms_bridge_unit_tick(Address(&units[1]));
    CHECK(g_shown.size() == 1 && g_shown[0] == Address(&owners[1]));
    g_now += 600;
    CHECK(!comms_bridge_unit_running(0));
}

static void TestBridgeRefusal() {
    comms_bridge_attach(FakeShow, FakeClock);
    FakeUnit unit;
    FakeOwner owner;
    MakeUnit(unit, owner, 2, true);
    g_now = 200000;
    g_shown.clear();
    g_texts.clear();
    g_accept = false;
    comms_bridge_show(2, 3);
    comms_bridge_unit_tick(Address(&unit));
    CHECK(g_shown.size() == 1);
    comms_bridge_unit_tick(Address(&unit));
    CHECK(g_shown.size() == 1);  // retry waits
    g_now += 300;
    comms_bridge_unit_tick(Address(&unit));
    CHECK(g_shown.size() == 2);
    g_accept = true;
    g_now += 300;
    comms_bridge_unit_tick(Address(&unit));
    CHECK(g_shown.size() == 3 && g_texts.size() == 1);
    g_now += 300;
    comms_bridge_unit_tick(Address(&unit));
    CHECK(g_shown.size() == 3);  // shown once

    g_accept = false;
    comms_bridge_show(2, 4);
    g_now += 5000;
    g_shown.clear();
    comms_bridge_unit_tick(Address(&unit));
    CHECK(g_shown.empty());  // too old, dropped
    g_accept = true;
}

static void TestTextHookOnlyDuringShow() {
    GuestRegs regs;
    std::memset(&regs, 0, sizeof(regs));
    regs.r13 = 0x1000;
    regs.rsi = 0x1234;
    comms_bridge_text_hook(&regs);
    CHECK(regs.rsi == 0x1234);
}

// ---- locator and install

static const uint8_t kPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
                                    0xec, 0xd8, 0x02, 0x00, 0x00, 0x4c, 0x8b, 0x25, 0x9d, 0x68, 0x8d, 0x00, 0x49, 0x89,
                                    0xfd, 0x49, 0x8b, 0x04, 0x24};
static const uint8_t kSite[] = {0x4c, 0x0f, 0x47, 0x75, 0x88, 0x4c, 0x89, 0xf6, 0xe8, 0xe4, 0x2d, 0x2a, 0x00};

static std::vector<uint8_t> Synthetic(uint32_t show, uint32_t site_offset, uint32_t caller, size_t stubs) {
    const size_t size = 0x4000;
    std::vector<uint8_t> image(size + stubs, 0x90);
    std::memcpy(&image[show], kPrologue, sizeof(kPrologue));
    std::memcpy(&image[show + site_offset], kSite, sizeof(kSite));
    image[caller] = 0xe8;
    const int32_t rel = static_cast<int32_t>(show) - static_cast<int32_t>(caller + 5);
    std::memcpy(&image[caller + 1], &rel, 4);
    return image;
}

static void TestLocateSynthetic() {
    std::vector<uint8_t> image = Synthetic(0x400, 0x3ef, 0x2000, 0);
    CommsBridgeSites sites{};
    CHECK(comms_bridge_locate(image.data(), 0x4000, &sites));
    CHECK(sites.show == 0x400 && sites.text_call == 0x400 + 0x3ef + 8);
    CHECK(!comms_bridge_locate(nullptr, 0x4000, &sites) && !comms_bridge_locate(image.data(), 0x4000, nullptr));
    CHECK(!comms_bridge_locate(image.data(), 16, &sites));

    std::vector<uint8_t> no_caller = image;
    no_caller[0x2000] = 0x90;
    CHECK(!comms_bridge_locate(no_caller.data(), 0x4000, &sites));

    std::vector<uint8_t> two_callers = image;
    two_callers[0x2100] = 0xe8;
    const int32_t rel = 0x400 - (0x2100 + 5);
    std::memcpy(&two_callers[0x2101], &rel, 4);
    CHECK(!comms_bridge_locate(two_callers.data(), 0x4000, &sites));

    std::vector<uint8_t> no_site = image;
    no_site[0x400 + 0x3ef] = 0x90;
    CHECK(!comms_bridge_locate(no_site.data(), 0x4000, &sites));

    std::vector<uint8_t> two_sites = image;
    std::memcpy(&two_sites[0x400 + 0x300], kSite, sizeof(kSite));
    CHECK(!comms_bridge_locate(two_sites.data(), 0x4000, &sites));

    std::vector<uint8_t> two_functions = image;
    std::memcpy(&two_functions[0x1000], kPrologue, sizeof(kPrologue));
    CHECK(!comms_bridge_locate(two_functions.data(), 0x4000, &sites));

    std::vector<uint8_t> far_site = Synthetic(0x400, 0x700, 0x2000, 0);
    CHECK(!comms_bridge_locate(far_site.data(), 0x4000, &sites));
}

static void TestInstallSynthetic() {
    comms_bridge_attach(nullptr, nullptr);
    CHECK(!comms_bridge_active());
    const size_t stubs = comms_bridge_stub_bytes();
    CHECK(stubs > 0);
    std::vector<uint8_t> image = Synthetic(0x400, 0x3ef, 0x2000, 0x1000 + stubs);
    const uint64_t size = 0x4000, entry = 0x400;
    CommsBridgeSites sites{};
    CHECK(comms_bridge_locate(image.data(), size, &sites));
    CHECK(!comms_bridge_install(image.data(), size, entry, nullptr, image.data() + 0x4000, stubs));
    CHECK(!comms_bridge_install(image.data(), size, entry, &sites, image.data() + 0x4000, stubs - 1));
    CHECK(!comms_bridge_active());
    const std::vector<uint8_t> before = image;
    CommsBridgeSites wrong = sites;
    wrong.text_call += 1;
    CHECK(!comms_bridge_install(image.data(), size, entry, &wrong, image.data() + 0x4000, stubs));
    CHECK(image == before);
    CHECK(comms_bridge_install(image.data(), size, entry, &sites, image.data() + 0x4000, stubs));
    CHECK(comms_bridge_active());
    CHECK(image[sites.text_call] == 0xe9 && image[0x400] == 0x55);
    comms_bridge_attach(nullptr, nullptr);
}

static void TestLocateRealImages() {
    struct Build {
        const char* folder;
        uint32_t show, call;
    };
    const Build builds[] = {{"v1.03", 0x2e8130, 0x2e8527},
                            {"CUSA07001-1.00", 0x2e7640, 0x2e7a37},
                            {"CUSA07001-1.02", 0x2e7a10, 0x2e7e07},
                            {"CUSA07001-1.03", 0x2e7a60, 0x2e7e57},
                            {"CUSA10209-1.00", 0x2e77d0, 0x2e7bc7}};
    for (const Build& build : builds) {
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/build/image/%s/boot.bin", DCP_SOURCE_DIR, build.folder);
        FILE* f = std::fopen(path, "rb");
        if (!f) {
            std::printf("  %s: no local boot image\n", build.folder);
            continue;
        }
        uint8_t magic[8];
        uint64_t header[5];
        bool ok = std::fread(magic, 1, 8, f) == 8 && !std::memcmp(magic, "DCPBOOT1", 8) && std::fread(header, 8, 5, f) == 5;
        uint64_t skip[6];
        ok = ok && std::fread(skip, 8, 2, f) == 2;
        uint64_t modules = 0;
        uint64_t extra[4];
        ok = ok && std::fread(extra, 8, 4, f) == 4 && std::fread(&modules, 8, 1, f) == 1;
        uint64_t bindings = 0;
        ok = ok && std::fseek(f, static_cast<long>(modules * 56), SEEK_CUR) == 0 && std::fread(&bindings, 8, 1, f) == 1;
        ok = ok && std::fseek(f, static_cast<long>(bindings * 24 + header[2] * 24 + header[4] * 128 + header[3] * 32), SEEK_CUR) == 0;
        std::vector<uint8_t> image(header[0]);
        ok = ok && std::fread(image.data(), 1, image.size(), f) == image.size();
        std::fclose(f);
        CHECK(ok);
        if (!ok) {
            continue;
        }
        CommsBridgeSites sites{};
        CHECK(comms_bridge_locate(image.data(), image.size(), &sites));
        CHECK(sites.show == build.show && sites.text_call == build.call);
        std::printf("  %s: balloon 0x%x, text call 0x%x\n", build.folder, sites.show, sites.text_call);
    }
}

static void TestControllerShortcutDCP_TWGQWK() {
    // Default is a safe chord, not bare D-pad Up.
    const ControllerShortcut& def = ControllerShortcutAt(kDefaultControllerShortcut);
    CHECK(std::string(def.name) == "back+dpad_up");
    CHECK(def.modifier != PadButton::kNone && def.trigger == PadButton::kDpadUp);
    CHECK(ShortcutMask(kDefaultControllerShortcut) == static_cast<uint8_t>(kPadBack | kPadDpadUp));
    CHECK(ControllerShortcutFromName("back+dpad_up") == kDefaultControllerShortcut);
    CHECK(ControllerShortcutFromName("dpad_up") == kControllerShortcutLegacyDpadUp);
    CHECK(ControllerShortcutFromName("off") == kControllerShortcutOff);
    CHECK(ControllerShortcutFromName("banana") == -1);

    // Plain D-pad Up/Down/Left/Right pass through unchanged while closed under the default shortcut.
    WheelController wheel;
    WheelInput dpad_up;
    dpad_up.available = true;
    dpad_up.pad_buttons = kPadDpadUp;
    WheelOutput out = wheel.Step(dpad_up);
    CHECK(!out.open && !out.hide_pad_up && out.hide_pad_buttons == 0 && !out.hide_confirm && !out.hide_cancel &&
          !out.hide_right_stick);

    // Simulate all four D-pad directions passing through PadData bits while closed.
    for (const uint32_t dir : {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT}) {
        uint32_t buttons = dir;
        WheelInput in;
        in.available = true;
        in.pad_buttons = (dir == BTN_UP) ? kPadDpadUp : 0;
        const WheelOutput step = wheel.Step(in);
        if (step.hide_pad_up) buttons &= ~BTN_UP;
        CHECK(!step.open && buttons == dir);
    }

    // Configured safe chord opens the wheel, hides opener buttons, and is edge-triggered.
    WheelInput chord;
    chord.available = true;
    chord.pad_buttons = static_cast<uint8_t>(kPadBack | kPadDpadUp);
    out = wheel.Step(chord);
    CHECK(out.open && out.source == Source::kController && out.hide_pad_up &&
          out.hide_pad_buttons == static_cast<uint8_t>(kPadBack | kPadDpadUp));

    // Holding the chord while cancelling does not immediately reopen the wheel.
    WheelInput cancel_while_held = chord;
    cancel_while_held.cancel_held = true;
    out = wheel.Step(cancel_while_held);
    CHECK(!out.open && out.hide_pad_up && out.hide_pad_buttons == static_cast<uint8_t>(kPadBack | kPadDpadUp));
    out = wheel.Step(chord);
    CHECK(!out.open && out.hide_pad_up && out.hide_pad_buttons == static_cast<uint8_t>(kPadBack | kPadDpadUp));

    // Releasing only one button does not re-arm; releasing both re-arms and allows a new activation.
    WheelInput partial;
    partial.available = true;
    partial.pad_buttons = kPadBack;
    out = wheel.Step(partial);
    CHECK(!out.open && !out.hide_pad_up && out.hide_pad_buttons == kPadBack);
    out = wheel.Step(chord);
    CHECK(!out.open);
    WheelInput released;
    released.available = true;
    out = wheel.Step(released);
    CHECK(!out.open && !out.hide_pad_up && out.hide_pad_buttons == 0);
    out = wheel.Step(chord);
    CHECK(out.open && out.source == Source::kController);

    // Already held before becoming available does not accidentally open when available turns true.
    WheelController preheld;
    WheelInput before = chord;
    before.available = false;
    CHECK(!preheld.Step(before).open);
    CHECK(!preheld.Step(chord).open);
    CHECK(!preheld.Step(released).open);
    CHECK(preheld.Step(chord).open);

    // Off prevents any controller input from opening Comms Wheel; keyboard Comms Wheel still works.
    WheelController off_wheel;
    WheelInput off_in;
    off_in.available = true;
    off_in.controller_shortcut = kControllerShortcutOff;
    off_in.pad_buttons = static_cast<uint8_t>(kPadBack | kPadL1 | kPadR1 | kPadDpadUp);
    out = off_wheel.Step(off_in);
    CHECK(!out.open && !out.hide_pad_up && out.hide_pad_buttons == 0);
    off_in.key_down = true;
    out = off_wheel.Step(off_in);
    CHECK(out.open && out.source == Source::kKeyboard);

    // Legacy D-pad Up opens only when explicitly selected.
    WheelController legacy;
    WheelInput legacy_in;
    legacy_in.available = true;
    legacy_in.controller_shortcut = kControllerShortcutLegacyDpadUp;
    legacy_in.pad_buttons = kPadDpadUp;
    out = legacy.Step(legacy_in);
    CHECK(out.open && out.source == Source::kController && out.hide_pad_up && out.hide_pad_buttons == kPadDpadUp);

    // Buttons from two different controllers never combine into one chord.
    const std::array<uint8_t, 2> split_pads = {kPadBack, kPadDpadUp};
    CHECK(!AnyControllerHoldsShortcut(kDefaultControllerShortcut, split_pads));
    const std::array<uint8_t, 2> same_pad = {static_cast<uint8_t>(kPadBack | kPadDpadUp), 0};
    CHECK(AnyControllerHoldsShortcut(kDefaultControllerShortcut, same_pad));

    // Character Highlight chords remain independent of Comms Wheel shortcuts.
    for (const auto& hl_chord : dcp::highlight::kChords) {
        if (std::string(hl_chord.name) == "off") continue;
        for (const auto& comms_chord : kControllerShortcuts) {
            CHECK(std::string(hl_chord.name) != std::string(comms_chord.name));
        }
    }
    std::printf("DCP-TWGQWK core shortcut regression passed\n");
}

int main() {
    TestIds();
    TestLayout();
    TestSectors();
    TestControllerLatches();
    TestHeldOpenerStaysHidden();
    TestControllerCancelAndEmptyConfirm();
    TestControllerShortcutDCP_TWGQWK();
    TestKeyboardLatches();
    TestKeyboardCancel();
    TestKeyboardNavigation();
    TestAvailability();
    TestRateLimit();
    TestWire();
    TestText();
    TestUsage();
    TestBridgeSlots();
    TestBridgeRefusal();
    TestTextHookOnlyDuringShow();
    TestLocateSynthetic();
    TestInstallSynthetic();
    TestLocateRealImages();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("comms tests passed\n");
    return 0;
}
