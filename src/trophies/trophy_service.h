#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "trophies/trophy_data.h"
#include "trophies/trophy_store.h"

namespace dcp::trophies {

namespace err {
inline constexpr int32_t kInvalidArgument = static_cast<int32_t>(0x80551604);
inline constexpr int32_t kInvalidHandle = static_cast<int32_t>(0x80551608);
inline constexpr int32_t kInvalidContext = static_cast<int32_t>(0x80551609);
inline constexpr int32_t kInvalidTrophyId = static_cast<int32_t>(0x8055160A);
inline constexpr int32_t kTrophyAlreadyUnlocked = static_cast<int32_t>(0x8055160C);
inline constexpr int32_t kPlatinumCannotUnlock = static_cast<int32_t>(0x8055160D);
inline constexpr int32_t kNotRegistered = static_cast<int32_t>(0x8055160F);
inline constexpr int32_t kAlreadyRegistered = static_cast<int32_t>(0x80551610);
inline constexpr int32_t kContextAlreadyExists = static_cast<int32_t>(0x80551613);
inline constexpr int32_t kContextExceedsMax = static_cast<int32_t>(0x80551623);
inline constexpr int32_t kHandleExceedsMax = static_cast<int32_t>(0x80551624);
} // namespace err

// OrbisNpTrophyFlagArray: one bit per trophy id.
struct FlagArray {
    uint32_t bits[4];
};

// The trophy a game has just earned, for the in-game notice.
struct UnlockNotice {
    int id = 0;
    Grade grade = Grade::kBronze;
    std::string name;
    std::string np_comm_id;
    std::filesystem::path icon;
};

class TrophyService {
public:
    void Configure(const std::filesystem::path& app0, const std::filesystem::path& user_dir);
    void SetClock(std::function<int64_t()> unix_seconds);

    int32_t CreateContext(int32_t* context, int32_t user, uint32_t label, uint64_t options);
    int32_t CreateHandle(int32_t* handle);
    int32_t RegisterContext(int32_t context, int32_t handle, uint64_t options);
    int32_t DestroyHandle(int32_t handle);
    int32_t DestroyContext(int32_t context);
    int32_t UnlockTrophy(int32_t context, int32_t handle, int32_t trophy_id, int32_t* platinum_id);
    // `details` and `data` are the guest's OrbisNpTrophy structures; both are required and sized as the SDK sizes them.
    int32_t GetGameInfo(int32_t context, int32_t handle, void* details, void* data);
    int32_t GetTrophyInfo(int32_t context, int32_t handle, int32_t trophy_id, void* details, void* data);
    int32_t GetUnlockState(int32_t context, int32_t handle, FlagArray* flags, uint32_t* count);

    bool TakeNotice(UnlockNotice& notice);

private:
    struct Context {
        int32_t user = 0;
        uint32_t label = 0;
        bool registered = false;
    };

    TrophyStore& StoreOf(int32_t user);
    int32_t FindContext(int32_t context, Context*& found);
    int32_t CheckRegistered(int32_t context, int32_t handle, Context*& found);
    bool KnownTrophy(int id) const;
    int UnlockedInSet(const TrophyStore& store) const;
    void Persist(TrophyStore& store);

    std::mutex lock_;
    std::function<int64_t()> clock_;
    std::filesystem::path user_dir_;
    std::string np_comm_id_;
    std::optional<TrophySet> set_;
    std::map<int32_t, Context> contexts_;
    std::set<int32_t> handles_;
    std::map<int32_t, TrophyStore> stores_;
    std::deque<UnlockNotice> notices_;
    int32_t next_context_ = 1;
    int32_t next_handle_ = 1;
};

} // namespace dcp::trophies
