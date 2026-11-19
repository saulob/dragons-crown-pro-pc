#include "appearance_session.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <set>

#include "appearance_net.h"
#include "dcp_party.h"

namespace dcp::appearance_session {
namespace {

namespace an = appearance_net;

constexpr uint64_t kMinRetryMs = 2000;

std::string Hex16(uint16_t value) {
    char text[8];
    std::snprintf(text, sizeof(text), "0x%x", value);
    return text;
}

std::string Describe(const an::Recipe& recipe, size_t bytes) {
    return "class=" + std::string(an::ClassName(recipe.klass)) + " schema=" + std::to_string(an::kSchema) +
           " parts=" + std::to_string(recipe.parts.size()) + " bytes=" + std::to_string(bytes) +
           " hash=" + an::ShortHash(an::Hash(recipe));
}

} // namespace

void Session::Log(const std::string& line) const {
    if (sink_.log) sink_.log("Online appearance: " + line);
}

void Session::SetLocal(int slot, std::vector<uint8_t> recipe) {
    if (!party::ValidSlot(slot)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    local_[size_t(slot)] = std::move(recipe);
}

const std::vector<uint8_t>* Session::LocalLook() const {
    const int mine = party::MemberSlot(me_);
    if (party::ValidSlot(mine) && local_[size_t(mine)]) return &*local_[size_t(mine)];
    for (const auto& look : local_)
        if (look) return &*look;
    return nullptr;
}

void Session::SetCapable(bool capable) {
    std::lock_guard<std::mutex> lock(mutex_);
    capable_ = capable;
}

void Session::DropPeer(uint16_t member, const char* why) {
    if (!peers_.erase(member)) return;
    Log("member " + Hex16(member) + " look removed (" + why + ")");
    if (sink_.clear) sink_.clear(party::MemberSlot(member));
}

void Session::DropAll(const char* why) {
    std::vector<uint16_t> members;
    for (const auto& [member, peer] : peers_) members.push_back(member);
    for (const uint16_t member : members) DropPeer(member, why);
}

void Session::Entered(uint64_t room, uint16_t me, const net::Json& answer, bool resumed) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (room != room_) DropAll("another room");
        room_ = room;
        me_ = me;
        if (resumed) sent_.reset();
        else sent_ = std::vector<uint8_t>{};
        in_flight_.reset();
        retry_at_ms_ = 0;
        std::set<uint16_t> listed;
        for (const net::Json& entry : answer["appearances"].items()) listed.insert(uint16_t(entry["member"].as_int()));
        std::vector<uint16_t> stale;
        for (const auto& [member, peer] : peers_)
            if (!listed.count(member)) stale.push_back(member);
        for (const uint16_t member : stale) DropPeer(member, "not in the room's snapshot");
    }
    size_t count = 0;
    for (const net::Json& entry : answer["appearances"].items()) {
        Received(uint16_t(entry["member"].as_int()), entry["appearance"]);
        ++count;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Log(std::string(resumed ? "resumed" : "entered") + " room as member " + Hex16(me) + ", snapshot of " +
        std::to_string(count) + " look(s)");
}

void Session::Left() {
    std::lock_guard<std::mutex> lock(mutex_);
    DropAll("room left");
    room_ = 0;
    me_ = 0;
    sent_.reset();
    in_flight_.reset();
    retry_at_ms_ = 0;
}

void Session::MemberLeft(uint16_t member) {
    std::lock_guard<std::mutex> lock(mutex_);
    DropPeer(member, "member left");
}

void Session::Received(uint16_t member, const net::Json& appearance) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!room_ || member == me_) return;
    const int slot = party::MemberSlot(member);
    if (!party::ValidSlot(slot)) return;
    if (appearance.is_null()) {
        DropPeer(member, "native look");
        return;
    }
    std::vector<uint8_t> bytes;
    an::Refusal why = an::Refusal::kSchema;
    std::optional<an::Recipe> recipe;
    if (appearance["schema"].as_int() == an::kSchema && net::hex_decode(appearance["data"].as_string(), bytes))
        recipe = an::Decode(bytes, &why);
    if (!recipe || recipe->native()) {
        if (!recipe) Log("member " + Hex16(member) + " look refused (" + an::RefusalText(why) + "); drawn native");
        DropPeer(member, recipe ? "native look" : "invalid look");
        return;
    }
    const std::vector<uint8_t> normal = an::Encode(*recipe);
    const uint64_t hash = an::Hash(*recipe);
    auto found = peers_.find(member);
    if (found != peers_.end() && found->second.hash == hash && found->second.recipe == normal) return;
    peers_[member] = Peer{normal, hash};
    Log("remote member=" + Hex16(member) + " slot=" + std::to_string(slot) + " " + Describe(*recipe, bytes.size()));
    if (sink_.set) sink_.set(slot, normal);
}

std::optional<net::Json> Session::TakePublish(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!capable_ || !room_ || in_flight_ || now_ms < retry_at_ms_) return std::nullopt;
    const std::vector<uint8_t>* chosen = LocalLook();
    const std::vector<uint8_t> local = chosen ? *chosen : std::vector<uint8_t>{};
    if (sent_ && *sent_ == local) return std::nullopt;
    net::Json body = net::Json::Object();
    body.set("room", net::u64_text(room_));
    if (local.empty()) {
        body.set("appearance", net::Json());
    } else {
        net::Json look = net::Json::Object();
        look.set("schema", int64_t(local[0]));
        look.set("data", net::hex_encode(local.data(), local.size()));
        body.set("appearance", std::move(look));
    }
    in_flight_ = local;
    in_flight_room_ = room_;
    return body;
}

void Session::PublishAnswered(const std::string& code, int64_t retry_ms, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!in_flight_) return;
    const std::vector<uint8_t> payload = std::move(*in_flight_);
    in_flight_.reset();
    if (in_flight_room_ != room_) return;
    if (code == "limit") {
        retry_at_ms_ = now_ms + std::max<uint64_t>(kMinRetryMs, retry_ms > 0 ? uint64_t(retry_ms) : 0);
        Log("local publish delayed by the lobby's budget");
        return;
    }
    sent_ = payload;
    if (code != "ok") {
        Log("local publish refused (" + code + "); the others draw this player native");
        return;
    }
    const std::optional<an::Recipe> recipe = payload.empty() ? std::nullopt : an::Decode(payload);
    if (recipe) Log("local publish member=" + Hex16(me_) + " " + Describe(*recipe, payload.size()));
    else Log("local publish member=" + Hex16(me_) + " native");
}

std::optional<std::vector<uint8_t>> Session::PeerRecipe(uint16_t member) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = peers_.find(member);
    if (found == peers_.end()) return std::nullopt;
    return found->second.recipe;
}

size_t Session::PeerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
}

namespace {

std::atomic<AppearancePeerSet> g_set{nullptr};
std::atomic<AppearancePeerClear> g_clear{nullptr};
std::atomic<bool> g_enabled{false};

} // namespace

Session& Shared() {
    static Session session(Sink{
        [](int slot, const std::vector<uint8_t>& recipe) {
            if (AppearancePeerSet set = g_set.load()) set(slot, recipe.data(), recipe.size());
        },
        [](int slot) {
            if (AppearancePeerClear clear = g_clear.load()) clear(slot);
        },
        [](const std::string& line) { std::printf("%s\n", line.c_str()); },
    });
    return session;
}

} // namespace dcp::appearance_session

extern "C" void appearance_session_enable(AppearancePeerSet set, AppearancePeerClear clear) {
    using namespace dcp::appearance_session;
    g_set.store(set);
    g_clear.store(clear);
    g_enabled.store(set && clear);
}

extern "C" int appearance_session_enabled(void) {
    return dcp::appearance_session::g_enabled.load() ? 1 : 0;
}

extern "C" void appearance_session_set_local(int slot, const uint8_t* recipe, size_t size) {
    dcp::appearance_session::Shared().SetLocal(slot, recipe && size ? std::vector<uint8_t>(recipe, recipe + size)
                                                                    : std::vector<uint8_t>{});
}
