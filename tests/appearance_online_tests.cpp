// Online custom looks without the game: the recipe codec and its stable part ids, the room session against an in-process
// lobby that behaves like server/lobby (late join, leave, resume), the remote textures and the seat-aware hooks.
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "appearance_hooks.h"
#include "appearance_net.h"
#include "appearance_remote.h"
#include "appearance_session.h"
#include "dcp_party.h"
#include "net/json.h"

namespace {

int g_checks = 0, g_failures = 0;

void Check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAILED line %d: %s\n", line, what);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using namespace dcp;
namespace an = dcp::appearance_net;
using Bytes = std::vector<uint8_t>;

an::NetPart Colored(uint8_t id, uint8_t r, uint8_t g, uint8_t b) {
    return an::NetPart{id, false, std::array<uint8_t, 3>{r, g, b}};
}
an::NetPart Hidden(uint8_t id) { return an::NetPart{id, true, std::nullopt}; }

an::Recipe Make(int klass, int base, std::vector<an::NetPart> parts) {
    return an::Recipe{uint8_t(klass), uint8_t(base), std::move(parts)};
}

Bytes Encoded(const an::Recipe& recipe) {
    return an::Encode(*an::Normalized(recipe));
}

// Fighter, base 0: Armor green, Helmet hidden, Horns blue, Shield Accent red.
an::Recipe FighterA() {
    return Make(0, 0, {Colored(1, 0x22, 0xaa, 0x55), Hidden(8), Colored(7, 0x22, 0x55, 0xff), Colored(12, 0xdd, 0x22, 0x22)});
}
an::Recipe FighterRed() { return Make(0, 0, {Colored(1, 0xcc, 0x10, 0x10)}); }

// ---- the recipe ----

void TestPartIds() {
    // The wire ids of schema 1: never renumbered, whatever the Editor's labels or table order become.
    const std::map<int, std::vector<std::pair<int, std::string>>> golden = {
        {0, {{1, "armor"}, {2, "trim"}, {3, "leather"}, {4, "eye"}, {5, "crest"}, {6, "crest_accent"}, {7, "horns"}, {8, "helmet"},
             {9, "face"}, {10, "sword"}, {11, "shield"}, {12, "shield_accent"}}},
        {1, {{1, "hair"}, {2, "skin"}, {3, "tattoo"}, {4, "eye"}, {5, "feather"}, {6, "armor"}, {7, "bikini"}, {8, "axe"}}},
        {2, {{1, "hood"}, {2, "hair"}, {3, "skin"}, {4, "eye"}, {5, "tunic"}, {6, "belt"}, {7, "collar"}, {8, "shorts"}, {9, "gloves"},
             {10, "laces"}, {11, "boots"}, {12, "quiver"}, {13, "arrows"}, {14, "bow"}}},
        {3, {{1, "beard"}, {2, "cape"}, {3, "skin"}, {4, "helmet"}, {5, "eye"}, {6, "feather"}, {7, "wrist_guards"}, {8, "belt"},
             {9, "boots"}, {10, "hammers"}}},
        {4, {{1, "hair"}, {2, "skin"}, {3, "eye"}, {4, "dress"}, {5, "bodice"}, {6, "lace"}, {7, "frills"}, {8, "hat"}, {9, "hat_band"},
             {10, "boots"}, {11, "accessories"}, {12, "staff"}}},
        {5, {{1, "robe"}, {2, "hood"}, {3, "pendant"}, {4, "hair"}, {5, "skin"}, {6, "gloves"}, {7, "belt"}, {8, "trousers"},
             {9, "boots"}, {10, "staff"}}},
    };
    for (const auto& [klass, expected] : golden) {
        const std::span<const an::PartName> table = an::PartTable(klass);
        CHECK(table.size() == expected.size());
        for (size_t i = 0; i < expected.size() && i < table.size(); ++i)
            CHECK(table[i].id == expected[i].first && table[i].key == expected[i].second);
        CHECK(table.size() <= an::kMaxParts);
        // Every Editor part travels, and nothing else does.
        const std::span<const CustomPart> parts = CustomPartsOf(CharacterClass(klass));
        CHECK(parts.size() == table.size());
        for (const CustomPart& part : parts) {
            const int id = an::PartId(klass, part.key);
            CHECK(id > 0 && an::PartKey(klass, uint8_t(id)) == part.key);
            if (part.label != part.key) CHECK(an::PartId(klass, part.label) < 0);
        }
    }
    CHECK(an::PartId(0, "Engraved Trim") < 0 && an::PartId(0, "trim") == 2);
    CHECK(an::PartId(1, "Boots & Gauntlets") < 0 && an::PartId(1, "armor") == 6);
    CHECK(an::PartKey(1, 12).empty() && an::PartKey(6, 1).empty() && an::PartTable(9).empty());
}

void TestEncodeDecode() {
    const Bytes native = Encoded(Make(2, 3, {}));
    CHECK(native == Bytes({1, 2, 3, 0}));
    CHECK(an::Decode(native)->native());

    const Bytes one = Encoded(Make(0, 0, {Colored(1, 0x22, 0xaa, 0x55)}));
    CHECK(one == Bytes({1, 0, 0, 1, 1, 2, 0x22, 0xaa, 0x55}));
    const Bytes hidden = Encoded(Make(0, 0, {Hidden(8)}));
    CHECK(hidden == Bytes({1, 0, 0, 1, 8, 1}));
    const Bytes both = Encoded(Make(0, 0, {an::NetPart{4, true, std::array<uint8_t, 3>{1, 2, 3}}}));
    CHECK(both == Bytes({1, 0, 0, 1, 4, 3, 1, 2, 3}));

    const an::Recipe a = FighterA();
    const Bytes bytes = Encoded(a);
    CHECK(bytes.size() == 4 + 5 + 2 + 5 + 5);
    const std::optional<an::Recipe> back = an::Decode(bytes);
    CHECK(back && *back == *an::Normalized(a) && an::Encode(*back) == bytes);

    // Order on the wire does not matter; the normalized recipe and its hash are the same.
    Bytes shuffled = {1, 0, 0, 4, 12, 2, 0xdd, 0x22, 0x22, 8, 1, 1, 2, 0x22, 0xaa, 0x55, 7, 2, 0x22, 0x55, 0xff};
    const std::optional<an::Recipe> other = an::Decode(shuffled);
    CHECK(other && an::Encode(*other) == bytes && an::Hash(*other) == an::Hash(a));
    std::vector<an::NetPart> reversed(a.parts.rbegin(), a.parts.rend());
    CHECK(an::Hash(Make(0, 0, reversed)) == an::Hash(a));
    CHECK(an::Hash(FighterRed()) != an::Hash(a));
    CHECK(an::Hash(Make(0, 1, a.parts)) != an::Hash(a));
    CHECK(an::Hash(Make(1, 0, {Colored(1, 0x22, 0xaa, 0x55)})) != an::Hash(Make(0, 0, {Colored(1, 0x22, 0xaa, 0x55)})));
    CHECK(an::ShortHash(an::Hash(a)).size() == 8);

    // Normalizing drops what changes nothing and what the class lacks; Encode takes only normalized recipes.
    const std::optional<an::Recipe> tidy = an::Normalized(Make(0, 0, {an::NetPart{3, false, std::nullopt}, Colored(99, 1, 1, 1), Hidden(8)}));
    CHECK(tidy && tidy->parts.size() == 1 && tidy->parts[0].id == 8);
    CHECK(an::Encode(Make(0, 0, {Hidden(8), Colored(1, 1, 1, 1)})).empty());
    CHECK(!an::Normalized(Make(6, 0, {})) && !an::Normalized(Make(0, 5, {})));

    // The largest look: every part of the class with most parts, coloured and hidden.
    an::Recipe worst = Make(2, 4, {});
    for (const an::PartName& part : an::PartTable(2)) worst.parts.push_back(an::NetPart{part.id, true, std::array<uint8_t, 3>{255, 0, 255}});
    const Bytes largest = Encoded(worst);
    CHECK(largest.size() == 4 + 14 * 5 && largest.size() <= an::kMaxBytes && largest.size() <= APPEARANCE_RECIPE_BYTES);
    CHECK(an::Decode(largest) && an::Decode(largest)->parts.size() == 14);
    std::printf("recipe sizes: one colour %zu bytes, four parts %zu bytes, worst case %zu bytes (limit %zu)\n", one.size(),
                bytes.size(), largest.size(), an::kMaxBytes);
}

void TestRefusals() {
    const auto refused = [](Bytes bytes, an::Refusal expected) {
        an::Refusal why = an::Refusal::kNone;
        return !an::Decode(bytes, &why) && why == expected;
    };
    CHECK(refused({}, an::Refusal::kEmpty));
    CHECK(refused({2, 0, 0, 0}, an::Refusal::kSchema));
    CHECK(refused({0, 0, 0, 0}, an::Refusal::kSchema));
    CHECK(refused({1, 0, 0}, an::Refusal::kTruncated));
    CHECK(refused({1, 6, 0, 0}, an::Refusal::kClass));
    CHECK(refused({1, 0xff, 0, 0}, an::Refusal::kClass));
    CHECK(refused({1, 0, 5, 0}, an::Refusal::kBase));
    CHECK(refused({1, 0, 0, uint8_t(an::kMaxParts + 1)}, an::Refusal::kCount));
    CHECK(refused({1, 0, 0, 1, 13, 1}, an::Refusal::kPart));
    CHECK(refused({1, 1, 0, 1, 12, 1}, an::Refusal::kPart)); // Shield Accent is no Amazon part
    CHECK(refused({1, 0, 0, 1, 0, 1}, an::Refusal::kPart));
    CHECK(refused({1, 0, 0, 2, 8, 1, 8, 1}, an::Refusal::kDuplicate));
    CHECK(refused({1, 0, 0, 2, 8, 1, 8, 2, 1, 2, 3}, an::Refusal::kDuplicate));
    CHECK(refused({1, 0, 0, 1, 8, 0}, an::Refusal::kFlags));
    CHECK(refused({1, 0, 0, 1, 8, 4}, an::Refusal::kFlags));
    CHECK(refused({1, 0, 0, 1, 8, 0x81}, an::Refusal::kFlags));
    CHECK(refused({1, 0, 0, 1, 8}, an::Refusal::kTruncated));
    CHECK(refused({1, 0, 0, 1, 1, 2, 0x22, 0xaa}, an::Refusal::kTruncated));
    CHECK(refused({1, 0, 0, 2, 8, 1}, an::Refusal::kTruncated));
    CHECK(refused({1, 0, 0, 1, 8, 1, 0}, an::Refusal::kTrailing));
    CHECK(refused({1, 0, 0, 0, 0}, an::Refusal::kTrailing));
    CHECK(refused(Bytes(an::kMaxBytes + 1, 1), an::Refusal::kTooLarge));
    for (int r = 0; r <= int(an::Refusal::kTrailing); ++r) CHECK(std::strlen(an::RefusalText(an::Refusal(r))) > 0);
}

void TestLookConversion() {
    CustomLook look{true, 0, {}};
    look.SetPartColor("armor", Rgb{0x22, 0xaa, 0x55});
    look.SetPartHidden("helmet", true);
    look.SetPartColor("horns", Rgb{0x22, 0x55, 0xff});
    look.SetPartColor("shield_accent", Rgb{0xdd, 0x22, 0x22});
    look.parts.push_back(PartLook{"headgear", Rgb{1, 2, 3}, false}); // a key no class has any more
    const std::optional<an::Recipe> recipe = RecipeOfLook(CharacterClass::kFighter, 0, look);
    CHECK(recipe && an::Encode(*recipe) == Encoded(FighterA()));
    const CustomLook back = LookOfRecipe(*recipe);
    CHECK(back.enabled && back.base == 0 && back.parts.size() == 4);
    for (const char* key : {"armor", "helmet", "horns", "shield_accent"}) {
        const PartLook* original = look.Find(key);
        const PartLook* rebuilt = back.Find(key);
        CHECK(original && rebuilt && original->hidden == rebuilt->hidden && original->color == rebuilt->color);
    }
    CustomLook disabled = look;
    disabled.enabled = false;
    CHECK(!RecipeOfLook(CharacterClass::kFighter, 0, disabled));
    CHECK(!RecipeOfLook(CharacterClass::kFighter, 0, CustomLook{true, 0, {}}));
    CHECK(!RecipeOfLook(CharacterClass::kFighter, 7, look));
    CHECK(WeaponChanged(CharacterClass::kFighter, 1, back) && !WeaponChanged(CharacterClass::kFighter, 0, back));
}

// ---- the room ----

struct FakeSink {
    std::map<int, Bytes> looks;
    int sets = 0, clears = 0;
    std::vector<std::string> lines;
    appearance_session::Sink Make() {
        return {[this](int slot, const Bytes& recipe) {
                    looks[slot] = recipe;
                    ++sets;
                },
                [this](int slot) {
                    looks.erase(slot);
                    ++clears;
                },
                [this](const std::string& line) { lines.push_back(line); }};
    }
    bool Logged(const std::string& part) const {
        for (const std::string& line : lines)
            if (line.find(part) != std::string::npos) return true;
        return false;
    }
};

net::Json Look(const Bytes& bytes) {
    net::Json look = net::Json::Object();
    look.set("schema", int64_t(bytes.empty() ? 0 : bytes[0]));
    look.set("data", net::hex_encode(bytes.data(), bytes.size()));
    return look;
}

// The lobby's side of room.appearance, as server/lobby/src/lobby.js keeps it: per seat, relayed to the others, in the
// answers of a later join or a resume, gone with the seat.
struct MiniLobby {
    struct Seat {
        appearance_session::Session* session;
        std::string data;
    };
    uint64_t room = 4096;
    std::map<uint16_t, Seat> seats;
    uint16_t next_free() const {
        for (uint16_t id : {0x10, 0x20, 0x30, 0x40})
            if (!seats.count(id)) return id;
        return 0;
    }
    net::Json Answer(uint16_t me) const {
        net::Json list = net::Json::Array();
        for (const auto& [member, seat] : seats) {
            if (member == me || seat.data.empty()) continue;
            Bytes bytes;
            net::hex_decode(seat.data, bytes);
            net::Json entry = net::Json::Object();
            entry.set("member", int64_t(member));
            entry.set("appearance", Look(bytes));
            list.push(std::move(entry));
        }
        net::Json answer = net::Json::Object();
        answer.set("appearances", std::move(list));
        return answer;
    }
    uint16_t Join(appearance_session::Session& session) {
        const uint16_t me = next_free();
        seats[me] = Seat{&session, ""};
        session.SetCapable(true);
        session.Entered(room, me, Answer(me), false);
        return me;
    }
    void Leave(uint16_t member) {
        seats[member].session->Left();
        seats.erase(member);
        for (auto& [other, seat] : seats) seat.session->MemberLeft(member);
    }
    void Pump(uint16_t member, uint64_t now = 1000) {
        Seat& seat = seats[member];
        std::optional<net::Json> body = seat.session->TakePublish(now);
        if (!body) return;
        const net::Json& look = (*body)["appearance"];
        const std::string data = look.is_null() ? "" : look["data"].as_string();
        const bool changed = data != seat.data;
        seat.data = data;
        seat.session->PublishAnswered("ok", 0, now);
        if (!changed) return;
        for (auto& [other, peer] : seats)
            if (other != member) peer.session->Received(member, look);
    }
};

void TestSessionPublish() {
    FakeSink sink;
    appearance_session::Session session(sink.Make());
    session.SetLocal(0, Encoded(FighterA()));
    CHECK(!session.TakePublish(0)); // no room
    session.Entered(4096, 0x10, net::Json::Object(), false);
    CHECK(!session.TakePublish(0)); // the lobby never offered appearance
    session.SetCapable(true);
    std::optional<net::Json> body = session.TakePublish(0);
    CHECK(body && (*body)["room"].as_string() == "4096" && (*body)["appearance"]["schema"].as_int() == 1);
    CHECK(body && (*body)["appearance"]["data"].as_string() == net::hex_encode(Encoded(FighterA()).data(), Encoded(FighterA()).size()));
    CHECK(!session.TakePublish(0)); // one in flight
    session.PublishAnswered("ok", 0, 0);
    CHECK(sink.Logged("local publish member=0x10 class=Fighter schema=1 parts=4"));
    CHECK(!sink.Logged("22aa55") && !sink.Logged("22AA55"));
    CHECK(!session.TakePublish(10)); // unchanged
    session.SetLocal(0, Encoded(FighterA()));
    CHECK(!session.TakePublish(20));

    session.SetLocal(0, Encoded(FighterRed()));
    body = session.TakePublish(30);
    CHECK(body.has_value());
    session.PublishAnswered("limit", 30000, 30);
    CHECK(!session.TakePublish(1000));
    CHECK(session.TakePublish(30 + 30000).has_value());
    session.PublishAnswered("ok", 0, 30030);
    CHECK(!session.TakePublish(30040));

    // Native again: the lobby is told to clear.
    session.SetLocal(0, {});
    body = session.TakePublish(40000);
    CHECK(body && (*body)["appearance"].is_null());
    session.PublishAnswered("ok", 0, 40000);
    CHECK(!session.TakePublish(40001));

    // A resumed seat republishes (the lobby answers an unchanged look without spending budget); a new room starts native.
    session.SetLocal(0, Encoded(FighterA()));
    session.PublishAnswered("ok", 0, 0);
    body = session.TakePublish(50000);
    session.PublishAnswered("ok", 0, 50000);
    session.Entered(4096, 0x10, net::Json::Object(), true);
    CHECK(session.TakePublish(50001).has_value());
    session.PublishAnswered("bad_request", 0, 50001);
    CHECK(!session.TakePublish(50002)); // a refused look is not retried
    session.Left();
    CHECK(!session.TakePublish(50003));
    session.SetLocal(0, {});
    session.Entered(4097, 0x10, net::Json::Object(), false);
    CHECK(!session.TakePublish(50004));

    // A guest's own slot is what it publishes; before the game drew it there, the look it drew elsewhere.
    FakeSink guest_sink;
    appearance_session::Session guest(guest_sink.Make());
    guest.SetCapable(true);
    guest.SetLocal(0, Encoded(FighterRed()));
    guest.Entered(4096, 0x20, net::Json::Object(), false);
    body = guest.TakePublish(0);
    CHECK(body && (*body)["appearance"]["data"].as_string() == net::hex_encode(Encoded(FighterRed()).data(), Encoded(FighterRed()).size()));
    guest.PublishAnswered("ok", 0, 0);
    guest.SetLocal(1, Encoded(FighterA()));
    body = guest.TakePublish(1);
    CHECK(body && (*body)["appearance"]["data"].as_string() == net::hex_encode(Encoded(FighterA()).data(), Encoded(FighterA()).size()));
    guest.SetLocal(9, Encoded(FighterRed()));
}

void TestSessionReceive() {
    FakeSink sink;
    appearance_session::Session session(sink.Make());
    session.Received(0x20, Look(Encoded(FighterA()))); // no room yet: ignored
    CHECK(sink.sets == 0);
    session.SetCapable(true);
    session.Entered(4096, 0x10, net::Json::Object(), false);
    session.Received(0x20, Look(Encoded(FighterA())));
    CHECK(sink.looks.count(party::MemberSlot(0x20)) && sink.looks[1] == Encoded(FighterA()));
    CHECK(sink.Logged("remote member=0x20 slot=1 class=Fighter schema=1 parts=4"));
    const int sets = sink.sets;
    Bytes shuffled = {1, 0, 0, 4, 12, 2, 0xdd, 0x22, 0x22, 8, 1, 1, 2, 0x22, 0xaa, 0x55, 7, 2, 0x22, 0x55, 0xff};
    session.Received(0x20, Look(shuffled));
    CHECK(sink.sets == sets); // the same look in another order: nothing rebuilt
    session.Received(0x10, Look(Encoded(FighterRed())));
    CHECK(!sink.looks.count(0)); // our own seat is never a remote look
    session.Received(0x30, Look(Encoded(FighterRed())));
    CHECK(sink.looks[2] == Encoded(FighterRed()) && sink.looks[1] != sink.looks[2]);
    CHECK(session.PeerCount() == 2);

    // Anything invalid is native, and logged without its content.
    for (const Bytes& bad : {Bytes{1, 9, 0, 0}, Bytes{1, 0, 0, 1, 13, 1}, Bytes{1, 0, 0, 2, 8, 1, 8, 1}, Bytes{1, 0, 0, 1, 1, 2, 0x22}}) {
        session.Received(0x30, Look(Encoded(FighterRed())));
        CHECK(sink.looks.count(2));
        session.Received(0x30, Look(bad));
        CHECK(!sink.looks.count(2) && !session.PeerRecipe(0x30));
    }
    session.Received(0x30, Look(Encoded(FighterRed())));
    net::Json future = Look(Encoded(FighterRed()));
    future.set("schema", int64_t(2));
    session.Received(0x30, future);
    CHECK(!sink.looks.count(2) && sink.Logged("unknown schema"));
    session.Received(0x30, Look(Encoded(FighterRed())));
    net::Json garbage = net::Json::Object();
    garbage.set("schema", int64_t(1));
    garbage.set("data", "zz");
    session.Received(0x30, garbage);
    CHECK(!sink.looks.count(2));
    session.Received(0x30, Look(Encoded(FighterRed())));
    session.Received(0x30, net::Json());
    CHECK(!sink.looks.count(2));
    session.Received(0x30, Look(Encoded(Make(0, 0, {}))));
    CHECK(!sink.looks.count(2) && !session.PeerRecipe(0x30));
    session.Received(0x50, Look(Encoded(FighterRed()))); // not a member id
    session.Received(0x00, Look(Encoded(FighterRed())));
    CHECK(sink.looks.size() == 1);

    session.MemberLeft(0x20);
    CHECK(sink.looks.empty() && session.PeerCount() == 0);
    session.Received(0x20, Look(Encoded(FighterA())));
    session.Left();
    CHECK(sink.looks.empty());
    for (const std::string& line : sink.lines) CHECK(line.find("22aa55") == std::string::npos && line.find("dd2222") == std::string::npos);
}

// A and B in a room, C later: everyone draws everyone; a reconnect and a leave leave nothing stale.
void TestLateJoinAndReconnect() {
    FakeSink sa, sb, sc;
    appearance_session::Session a(sa.Make()), b(sb.Make()), c(sc.Make());
    MiniLobby lobby;
    a.SetLocal(0, Encoded(FighterA()));
    const uint16_t ma = lobby.Join(a);
    lobby.Pump(ma);
    b.SetLocal(0, {});
    const uint16_t mb = lobby.Join(b);
    lobby.Pump(mb);
    CHECK(ma == 0x10 && mb == 0x20);
    CHECK(sb.looks.size() == 1 && sb.looks[0] == Encoded(FighterA())); // B sees A from its join answer
    CHECK(sa.looks.empty());                                            // A sees B native

    // B customizes later: A gets the update.
    b.SetLocal(1, Encoded(FighterRed()));
    lobby.Pump(mb);
    CHECK(sa.looks.size() == 1 && sa.looks[1] == Encoded(FighterRed()));

    // C joins late with its own look: it gets A and B, they get C.
    const an::Recipe elf = Make(2, 1, {Hidden(1), Colored(14, 10, 200, 10)});
    c.SetLocal(2, Encoded(elf));
    const uint16_t mc = lobby.Join(c);
    CHECK(mc == 0x30 && sc.looks.size() == 2 && sc.looks[0] == Encoded(FighterA()) && sc.looks[1] == Encoded(FighterRed()));
    lobby.Pump(mc);
    CHECK(sa.looks[2] == Encoded(elf) && sb.looks[2] == Encoded(elf));

    // A drops and comes back as a new join: its old look goes, the new one arrives; nothing of the old seat stays.
    lobby.Leave(ma);
    CHECK(!sb.looks.count(0) && !sc.looks.count(0));
    CHECK(sa.looks.empty());
    a.SetLocal(0, Encoded(FighterRed()));
    const uint16_t again = lobby.Join(a);
    CHECK(again == 0x10 && sa.looks.size() == 2);
    lobby.Pump(again);
    CHECK(sb.looks[0] == Encoded(FighterRed()) && sc.looks[0] == Encoded(FighterRed()));

    // B's link drops while C leaves: B's resumed seat gets a snapshot without C, and B republishes its own look.
    lobby.seats.erase(mc);
    a.MemberLeft(mc);
    CHECK(sb.looks.count(2));
    b.Entered(lobby.room, mb, lobby.Answer(mb), true);
    CHECK(sb.looks.size() == 1 && sb.looks[0] == Encoded(FighterRed()) && !sb.looks.count(2));
    CHECK(b.TakePublish(5000).has_value());
}

// ---- the textures ----

struct FakeTextures {
    uint64_t now = 100000;
    std::map<std::string, std::vector<uint8_t>> served;
    std::vector<std::string> lines;
    int builds = 0;
    bool fail = false;
    size_t atlas_size = 64;
    RemoteLooks::Host Host() {
        RemoteLooks::Host host;
        host.threaded = false;
        host.now_ms = [this] { return now; };
        host.log = [this](const std::string& line) { lines.push_back(line); };
        host.serve = [this](const RemoteEntry& entry, const uint8_t* data) {
            if (data) served[entry.name] = std::vector<uint8_t>(data, data + entry.size);
            else served.erase(entry.name);
            return true;
        };
        host.build = [this](CharacterClass klass, int base, const CustomLook& look, std::string* error) -> std::optional<LookTextures> {
            ++builds;
            if (fail) {
                *error = "test failure";
                return std::nullopt;
            }
            const std::optional<an::Recipe> recipe = RecipeOfLook(klass, base, look);
            const uint8_t mark = recipe ? uint8_t(an::Hash(*recipe)) : 0;
            LookTextures textures{std::vector<uint8_t>(atlas_size, mark), {}};
            for (size_t w = 0; w < WeaponTexturesOf(klass).size(); ++w)
                if (WeaponChanged(klass, w, look)) textures.weapons.emplace_back(w, std::vector<uint8_t>(16, uint8_t(mark + 1 + w)));
            return textures;
        };
        return host;
    }
};

std::vector<RemoteTextureSet> FakePlan() {
    std::vector<RemoteTextureSet> plan;
    for (int k = 0; k < an::kClassCount; ++k) {
        for (int base = 0; base < an::kBaseCount; ++base) {
            for (int i = 0; i < kRemoteLooksPerPreset; ++i) {
                RemoteTextureSet set{CharacterClass(k), base, i, {RemoteAtlasEntry(CharacterClass(k), base, i), 64, 0}, {}};
                for (size_t w = 0; w < WeaponTexturesOf(CharacterClass(k)).size(); ++w)
                    set.weapons.push_back({RemoteWeaponEntry(CharacterClass(k), base, w, i), 16, 0});
                plan.push_back(set);
            }
        }
    }
    return plan;
}

void TestRemoteLooks() {
    CHECK(RemoteAtlasEntry(CharacterClass::kFighter, 0, 0) == "PLAYER/FIGHTER00_NET1.FTX");
    CHECK(RemoteAtlasEntry(CharacterClass::kSorceress, 4, 1) == "PLAYER/SORCERESS04_NET2.FTX");
    CHECK(RemoteWeaponEntry(CharacterClass::kFighter, 2, 1, 3) == "PLAYER/FIGHTER02_SHIELD01_NET4.FTX");
    CHECK(RemoteWeaponEntry(CharacterClass::kElf, 0, 1, 0).empty());

    FakeTextures fake;
    RemoteLooks looks(FakePlan(), fake.Host());
    const Bytes a = Encoded(FighterA()), red = Encoded(FighterRed());
    looks.Set(1, a);
    CHECK(!looks.Of(1, 0, 0)); // not built yet: native
    looks.Drain();
    const AppearanceInstance* first = looks.Of(1, 0, 0);
    CHECK(first && std::string(first->entry) == "PLAYER/FIGHTER00_NET1.FTX" && first->remote != 0);
    CHECK(first && fake.served["PLAYER/FIGHTER00_NET1.FTX"] == std::vector<uint8_t>(64, uint8_t(an::Hash(FighterA()))));
    CHECK(first && std::string(first->weapons[1].entry) == "PLAYER/FIGHTER00_SHIELD01_NET1.FTX" &&
          std::string(first->weapons[1].native) == "PLAYER/FIGHTER00_SHIELD01.FTX" && first->weapons[0].entry[0] == 0);
    CHECK(first && Bytes(first->recipe, first->recipe + first->recipe_size) == a);
    CHECK(!looks.Of(1, 0, 1) && !looks.Of(1, 1, 0) && !looks.Of(0, 0, 0) && !looks.Of(7, 0, 0));

    // A second Fighter with another look: its own texture.
    looks.Set(2, red);
    looks.Drain();
    const AppearanceInstance* second = looks.Of(2, 0, 0);
    CHECK(second && std::string(second->entry) == "PLAYER/FIGHTER00_NET2.FTX" && second->weapons[1].entry[0] == 0);
    CHECK(fake.served["PLAYER/FIGHTER00_NET1.FTX"] != fake.served["PLAYER/FIGHTER00_NET2.FTX"]);
    CHECK(looks.Builds() == 2 && fake.builds == 2);

    // An identical recipe rebuilds nothing; the same look on another player shares the texture.
    looks.Set(1, a);
    looks.Set(3, a);
    looks.Drain();
    CHECK(fake.builds == 2 && looks.TextureOf(3) == looks.TextureOf(1) && looks.Of(3, 0, 0));
    CHECK(looks.BySerial(first->remote) == first);

    // A player leaves: its look goes, the other keeps the shared texture; an effect's old serial no longer resolves.
    const uint16_t old_serial = looks.Of(3, 0, 0)->remote;
    looks.Clear(3);
    CHECK(!looks.Of(3, 0, 0) && looks.Of(1, 0, 0) && !looks.BySerial(old_serial));

    // The same player back with the same look: no rebuild.
    looks.Set(3, a);
    looks.Drain();
    CHECK(fake.builds == 2 && looks.Of(3, 0, 0));

    // A look changing class.
    const an::Recipe dwarf = Make(3, 2, {Colored(10, 200, 0, 0)});
    looks.Set(3, Encoded(dwarf));
    looks.Drain();
    const AppearanceInstance* hammer = looks.Of(3, 3, 2);
    CHECK(hammer && std::string(hammer->entry) == "PLAYER/DWARF02_NET1.FTX" &&
          std::string(hammer->weapons[0].entry) == "PLAYER/DWARF02_ARMS01_NET1.FTX" && !looks.Of(3, 0, 0));
    // Another preset of a class has textures of its own.
    looks.Set(0, Encoded(Make(0, 3, {Colored(1, 1, 2, 3)})));
    looks.Drain();
    CHECK(looks.Of(0, 0, 3) && std::string(looks.Of(0, 0, 3)->entry) == "PLAYER/FIGHTER03_NET1.FTX");
    looks.Clear(0);

    // Released textures stay with their look for a while; a new look takes a texture never used.
    looks.Clear(1);
    looks.Clear(2);
    const an::Recipe blue = Make(0, 0, {Colored(1, 0, 0, 200)});
    looks.Set(1, Encoded(blue));
    looks.Drain();
    CHECK(looks.Of(1, 0, 0) && std::string(looks.Of(1, 0, 0)->entry) == "PLAYER/FIGHTER00_NET3.FTX");
    static_assert(kRemoteLooksPerPreset == 3);
    const an::Recipe gold = Make(0, 0, {Colored(1, 200, 160, 0)});
    looks.Set(2, Encoded(gold));
    looks.Drain();
    CHECK(!looks.Of(2, 0, 0)); // every free texture was drawn a moment ago
    fake.now += kRemoteReuseQuietMs - 1;
    looks.Drain();
    CHECK(!looks.Of(2, 0, 0));
    fake.now += 1;
    looks.Drain();
    CHECK(looks.Of(2, 0, 0) && std::string(looks.Of(2, 0, 0)->entry) == "PLAYER/FIGHTER00_NET1.FTX");
    const an::Recipe white = Make(0, 0, {Colored(1, 250, 250, 250)});
    looks.Set(0, Encoded(white));
    looks.Drain();
    CHECK(looks.Of(0, 0, 0) && std::string(looks.Of(0, 0, 0)->entry) == "PLAYER/FIGHTER00_NET2.FTX");
    looks.Clear(3);

    // Invalid recipes and failed builds leave the player native.
    looks.Set(1, Bytes{1, 0, 0, 1, 13, 1});
    CHECK(!looks.Of(1, 0, 0) && looks.TextureOf(1) < 0);
    fake.fail = true;
    looks.Set(1, Encoded(Make(4, 0, {Hidden(8)})));
    looks.Drain();
    CHECK(!looks.Of(1, 4, 0));
    fake.fail = false;
    fake.atlas_size = 65; // not the reserved size
    looks.Set(1, Encoded(Make(5, 0, {Hidden(2)})));
    looks.Drain();
    CHECK(!looks.Of(1, 5, 0));
    fake.atlas_size = 64;

    // A look nobody has worn for a while gives its memory back and is rebuilt if it returns.
    const an::Recipe green = Make(1, 4, {Colored(2, 0, 200, 0)});
    looks.Set(1, Encoded(green));
    looks.Drain();
    CHECK(fake.served.count("PLAYER/AMAZON04_NET1.FTX"));
    looks.Clear(1);
    fake.now += kRemoteKeepMs - 1;
    looks.Drain();
    CHECK(fake.served.count("PLAYER/AMAZON04_NET1.FTX"));
    fake.now += 1;
    looks.Drain();
    CHECK(!fake.served.count("PLAYER/AMAZON04_NET1.FTX"));
    const int before_return = fake.builds;
    looks.Set(1, Encoded(green));
    looks.Drain();
    CHECK(fake.builds == before_return + 1 && looks.Of(1, 1, 4));

    looks.Reset();
    for (int slot = 0; slot < kRemoteSeats; ++slot) CHECK(looks.TextureOf(slot) < 0);
    for (const std::string& line : fake.lines) CHECK(line.find("#") == std::string::npos);
}

// ---- the hooks ----

struct FakeSeat {
    std::vector<uint8_t> record = std::vector<uint8_t>(0x2200, 0);
    std::vector<uint8_t> unit = std::vector<uint8_t>(0xb00, 0);
    std::vector<uint8_t> owner = std::vector<uint8_t>(0x3a00, 0);
    FakeSeat(int slot, int seat_owner, int klass, int color, uint8_t identity) {
        record[APPEARANCE_RECORD_CLASS] = uint8_t(klass);
        record[APPEARANCE_RECORD_COLOR] = uint8_t(color);
        std::memset(record.data() + APPEARANCE_RECORD_IDENTITY, identity, APPEARANCE_IDENTITY_BYTES);
        const int32_t owner_value = seat_owner;
        std::memcpy(record.data() + APPEARANCE_SEAT_OWNER, &owner_value, 4);
        unit[0xcb] = uint8_t(klass);
        unit[0xcc] = uint8_t(color);
        const int32_t slot_value = slot;
        std::memcpy(unit.data() + 0xa70, &slot_value, 4);
        const uint64_t unit_address = Address(unit);
        std::memcpy(owner.data() + 0x9b8, &unit_address, 8);
        const uint16_t class_word = uint16_t(klass);
        std::memcpy(owner.data() + 0x39e0, &class_word, 2);
    }
    static uint64_t Address(const std::vector<uint8_t>& bytes) { return uint64_t(uintptr_t(bytes.data())); }
};

std::vector<const FakeSeat*> g_seats;
RemoteLooks* g_remote = nullptr;
std::vector<std::pair<int, const AppearanceInstance*>> g_seen;

uint64_t SeatOfUnit(uint64_t unit) {
    for (const FakeSeat* seat : g_seats)
        if (FakeSeat::Address(seat->unit) == unit) return FakeSeat::Address(seat->record);
    return 0;
}
const AppearanceInstance* RemoteOf(int slot, int klass, int color) { return g_remote->Of(slot, klass, color); }
const AppearanceInstance* RemoteSerial(uint16_t serial) { return g_remote->BySerial(serial); }
void Seen(int slot, const AppearanceInstance* instance) { g_seen.emplace_back(slot, instance); }

const char* GUEST_HOOK_ABI FakeWeaponName(int klass, uint32_t item) {
    static std::string name;
    name = item == 0 ? WeaponAtlasName(CharacterClass(klass), WeaponTexturesOf(CharacterClass(klass))[0]) : "PLAYER/ITEM_ARMS77.FTX";
    return name.c_str();
}
const char* GUEST_HOOK_ABI FakeShieldName(int klass, uint32_t item) {
    static std::string name;
    name = item == 0 && klass == 0 ? "PLAYER/FIGHTER00_SHIELD01.FTX" : "PLAYER/ITEM_SHIELD77.FTX";
    return name.c_str();
}

std::string Atlas(const FakeSeat& seat) {
    GuestRegs regs{};
    regs.rax = FakeSeat::Address(seat.unit);
    regs.rdi = seat.unit[0xcb];
    regs.rsi = seat.unit[0xcc];
    appearance_unit_atlas(&regs);
    return regs.skip ? std::string(reinterpret_cast<const char*>(uintptr_t(regs.rax))) : "native";
}

std::string Weapon(const FakeSeat& seat, int which, uint32_t item) {
    GuestRegs regs{};
    regs.rax = FakeSeat::Address(seat.unit);
    regs.rdi = seat.unit[0xcb];
    regs.rsi = item;
    if (which) appearance_unit_shield(&regs);
    else appearance_unit_weapon(&regs);
    return regs.skip ? std::string(reinterpret_cast<const char*>(uintptr_t(regs.rax))) : "native";
}

void TestHooks() {
    FakeTextures fake;
    RemoteLooks looks(FakePlan(), fake.Host());
    g_remote = &looks;
    const FakeSeat me(0, APPEARANCE_SEAT_LOCAL, 0, 0, 0x11), host_a(1, APPEARANCE_SEAT_REMOTE, 0, 0, 0x11),
        b(2, APPEARANCE_SEAT_REMOTE, 0, 0, 0x22), plain(3, APPEARANCE_SEAT_REMOTE, 2, 1, 0x33);
    g_seats = {&me, &host_a, &b, &plain};
    appearance_hooks_set_record_of_unit(SeatOfUnit);
    appearance_hooks_set_weapon_names(FakeWeaponName, FakeShieldName);
    AppearanceInstance mine{};
    std::memset(mine.identity, 0x11, sizeof(mine.identity));
    mine.klass = 0;
    mine.color = 0;
    std::snprintf(mine.entry, sizeof(mine.entry), "PLAYER/FIGHTER00_D1C01_AAAAAAAA.FTX");
    const Bytes my_recipe = Encoded(FighterRed());
    std::memcpy(mine.recipe, my_recipe.data(), my_recipe.size());
    mine.recipe_size = uint8_t(my_recipe.size());
    appearance_hooks_set_instances(&mine, 1);

    // Offline: the saved character's look follows its identity, as before.
    appearance_hooks_set_online(nullptr, nullptr, nullptr);
    CHECK(Atlas(me) == mine.entry && Atlas(host_a) == mine.entry && Atlas(b) == "native");

    // Online: remote seats wear their player's look, ours only in our seat, even for a copied save.
    appearance_hooks_set_online(RemoteOf, RemoteSerial, Seen);
    CHECK(Atlas(me) == mine.entry);
    CHECK(!g_seen.empty() && g_seen.back().first == 0 && g_seen.back().second == &mine);
    CHECK(Atlas(host_a) == "native" && Atlas(b) == "native" && Atlas(plain) == "native");
    looks.Set(1, Encoded(FighterA()));
    looks.Set(2, Encoded(Make(0, 0, {Colored(1, 200, 0, 0), Colored(10, 0, 0, 255)})));
    looks.Drain();
    CHECK(Atlas(host_a) == "PLAYER/FIGHTER00_NET1.FTX" && Atlas(b) == "PLAYER/FIGHTER00_NET2.FTX");
    CHECK(Atlas(plain) == "native");

    // Weapons: only where the game would draw the default one, as for a saved look.
    CHECK(Weapon(host_a, 1, 0) == "PLAYER/FIGHTER00_SHIELD01_NET1.FTX" && Weapon(host_a, 0, 0) == "native");
    CHECK(Weapon(host_a, 1, 5) == "native");
    CHECK(Weapon(b, 0, 0) == "PLAYER/FIGHTER00_ARMS01_NET2.FTX" && Weapon(b, 0, 7) == "native" && Weapon(b, 1, 0) == "native");

    // A native character in our seat is reported native.
    FakeSeat other_me(0, APPEARANCE_SEAT_LOCAL, 1, 2, 0x44);
    g_seats.push_back(&other_me);
    CHECK(Atlas(other_me) == "native" && g_seen.back().first == 0 && g_seen.back().second == nullptr);
    // A seat that is no other player's keeps the offline rule, and is not reported as ours.
    FakeSeat empty(1, -1, 0, 0, 0x11);
    g_seats.push_back(&empty);
    const size_t reported = g_seen.size();
    CHECK(Atlas(empty) == mine.entry && g_seen.size() == reported);

    // Effects follow their owner's look while that look is current.
    std::vector<uint8_t> effect(0x3900, 0);
    const uint64_t effect_address = FakeSeat::Address(effect);
    const auto copy_look = [&](const FakeSeat& owner) {
        GuestRegs regs{};
        regs.rdi = effect_address;
        regs.rsi = FakeSeat::Address(owner.owner);
        appearance_effect_copy(&regs);
        const uint16_t klass = owner.unit[0xcb], color = owner.unit[0xcc];
        std::memcpy(effect.data() + 0x38e8, &klass, 2);
        std::memcpy(effect.data() + 0x38ea, &color, 2);
    };
    const auto effect_atlas = [&]() -> std::string {
        GuestRegs regs{};
        regs.rdi = effect_address;
        appearance_effect_atlas(&regs);
        return regs.skip ? std::string(reinterpret_cast<const char*>(uintptr_t(regs.rax))) : "native";
    };
    copy_look(host_a);
    CHECK(effect_atlas() == "PLAYER/FIGHTER00_NET1.FTX");
    copy_look(me);
    CHECK(effect_atlas() == mine.entry);
    copy_look(b);
    CHECK(effect_atlas() == "PLAYER/FIGHTER00_NET2.FTX");
    looks.Clear(2);
    CHECK(effect_atlas() == "native" && Atlas(b) == "native");

    appearance_hooks_set_online(nullptr, nullptr, nullptr);
    appearance_hooks_set_instances(nullptr, 0);
    appearance_hooks_set_record_of_unit(nullptr);
    appearance_hooks_set_weapon_names(nullptr, nullptr);
    g_seats.clear();
    g_remote = nullptr;
}

// Session to textures to hooks, as the loader wires them: B draws A's look in A's seat.
void TestEndToEnd() {
    FakeTextures fake;
    RemoteLooks looks(FakePlan(), fake.Host());
    appearance_session::Session b(appearance_session::Sink{
        [&](int slot, const Bytes& recipe) { looks.Set(slot, recipe); },
        [&](int slot) { looks.Clear(slot); },
        nullptr});
    appearance_session::Session a(appearance_session::Sink{nullptr, nullptr, nullptr});
    MiniLobby lobby;
    a.SetLocal(0, Encoded(FighterA()));
    const uint16_t ma = lobby.Join(a);
    lobby.Pump(ma);
    const uint16_t mb = lobby.Join(b);
    looks.Drain();
    const FakeSeat seat_a(party::MemberSlot(ma), APPEARANCE_SEAT_REMOTE, 0, 0, 0x11);
    g_seats = {&seat_a};
    g_remote = &looks;
    appearance_hooks_set_record_of_unit(SeatOfUnit);
    appearance_hooks_set_online(RemoteOf, RemoteSerial, Seen);
    CHECK(Atlas(seat_a) == "PLAYER/FIGHTER00_NET1.FTX");
    CHECK(fake.served["PLAYER/FIGHTER00_NET1.FTX"] == std::vector<uint8_t>(64, uint8_t(an::Hash(FighterA()))));
    lobby.Leave(ma);
    CHECK(Atlas(seat_a) == "native");
    lobby.Leave(mb);
    appearance_hooks_set_online(nullptr, nullptr, nullptr);
    appearance_hooks_set_record_of_unit(nullptr);
    g_seats.clear();
    g_remote = nullptr;
}

} // namespace

int main() {
    TestPartIds();
    TestEncodeDecode();
    TestRefusals();
    TestLookConversion();
    TestSessionPublish();
    TestSessionReceive();
    TestLateJoinAndReconnect();
    TestRemoteLooks();
    TestHooks();
    TestEndToEnd();
    std::printf("appearance online tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
