/* Custom looks of the players of an online room (appearance_net.h): this client's own look published to the lobby, and
 * every other member's validated look handed to the renderer by party slot. Nothing here while Online Play is off. */
#ifndef DCP_APPEARANCE_SESSION_H
#define DCP_APPEARANCE_SESSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A member's normalized recipe for party slot 0-3, or its removal: the slot is drawn native again. */
typedef void (*AppearancePeerSet)(int slot, const uint8_t *recipe, size_t size);
typedef void (*AppearancePeerClear)(int slot);

/* Once, before the game starts: with Online Play on and the remote looks installed. */
void appearance_session_enable(AppearancePeerSet set, AppearancePeerClear clear);
int appearance_session_enabled(void);

/* The look the game draws for this client's player in party slot 0-3: a normalized recipe, or size 0 for native. Any
 * thread. */
void appearance_session_set_local(int slot, const uint8_t *recipe, size_t size);

#ifdef __cplusplus
}

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "net/json.h"

namespace dcp::appearance_session {

struct Sink {
    std::function<void(int slot, const std::vector<uint8_t>& recipe)> set;
    std::function<void(int slot)> clear;
    std::function<void(const std::string& line)> log;
};

// The lobby side of the room's looks. The lobby keeps each member's look with its seat; a member gets the others' in its
// create, join and resume answers, then each change as a room.appearance event.
class Session {
public:
    explicit Session(Sink sink) : sink_(std::move(sink)) {}

    // Our member's slot is published; until the game draws our player there, the one it drew in another slot is.
    void SetLocal(int slot, std::vector<uint8_t> recipe);
    // Whether the lobby answered hello with the appearance capability; an older lobby gets nothing.
    void SetCapable(bool capable);
    // A create, join or resume answer: the room, our member id and the `appearances` snapshot.
    void Entered(uint64_t room, uint16_t me, const net::Json& answer, bool resumed);
    void Left();
    void MemberLeft(uint16_t member);
    void Received(uint16_t member, const net::Json& appearance);

    // The room.appearance body due now, if any; at most one in flight.
    std::optional<net::Json> TakePublish(uint64_t now_ms);
    void PublishAnswered(const std::string& code, int64_t retry_ms, uint64_t now_ms);

    std::optional<std::vector<uint8_t>> PeerRecipe(uint16_t member) const;
    size_t PeerCount() const;

private:
    struct Peer {
        std::vector<uint8_t> recipe;
        uint64_t hash = 0;
    };
    void Log(const std::string& line) const;
    void DropPeer(uint16_t member, const char* why);
    void DropAll(const char* why);
    const std::vector<uint8_t>* LocalLook() const;

    mutable std::mutex mutex_;
    Sink sink_;
    bool capable_ = false;
    uint64_t room_ = 0;
    uint16_t me_ = 0;
    std::array<std::optional<std::vector<uint8_t>>, 4> local_;
    std::optional<std::vector<uint8_t>> sent_; // what the lobby holds for our seat; unknown after a resume
    std::optional<std::vector<uint8_t>> in_flight_;
    uint64_t in_flight_room_ = 0;
    uint64_t retry_at_ms_ = 0;
    std::map<uint16_t, Peer> peers_;
};

Session& Shared();

} // namespace dcp::appearance_session
#endif

#endif
