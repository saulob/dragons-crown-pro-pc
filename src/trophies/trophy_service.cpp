#include "trophies/trophy_service.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "trophies/trophy_pack.h"
#include "trophies/trophy_view.h"

namespace fs = std::filesystem;

namespace dcp::trophies {
namespace {

constexpr size_t kMaxContexts = 8;
constexpr size_t kMaxHandles = 4;
constexpr size_t kTitleSize = 128;
constexpr size_t kGameDetailSize = 1024;
constexpr size_t kTrophyNameSize = 128;
constexpr size_t kTrophyDetailSize = 1024;
constexpr size_t kGameDetailsStructSize = 0x4A0;
constexpr size_t kGameDataStructSize = 0x20;
constexpr size_t kTrophyDetailsStructSize = 0x498;
constexpr size_t kTrophyDataStructSize = 0x18;

struct GameDetails {
    uint64_t size;
    uint32_t groups, trophies, platinum, gold, silver, bronze;
    char title[kTitleSize];
    char description[kGameDetailSize];
};
struct GameData {
    uint64_t size;
    uint32_t unlocked, platinum, gold, silver, bronze, percent;
};
struct TrophyDetails {
    uint64_t size;
    int32_t id;
    int32_t grade;
    int32_t group;
    uint8_t hidden;
    uint8_t reserved[3];
    char name[kTrophyNameSize];
    char description[kTrophyDetailSize];
};
struct TrophyData {
    uint64_t size;
    int32_t id;
    uint8_t unlocked;
    uint8_t reserved[3];
    uint64_t timestamp;
};
static_assert(sizeof(GameDetails) == kGameDetailsStructSize && sizeof(GameData) == kGameDataStructSize &&
              sizeof(TrophyDetails) == kTrophyDetailsStructSize && sizeof(TrophyData) == kTrophyDataStructSize);

template <size_t N>
void CopyText(char (&out)[N], const std::string& text) {
    std::memset(out, 0, N);
    std::memcpy(out, text.data(), std::min(text.size(), N - 1));
}

uint64_t GuestSize(const void* guest) {
    uint64_t size = 0;
    std::memcpy(&size, guest, sizeof(size));
    return size;
}

template <typename T>
void StoreStruct(void* guest, T& value) {
    value.size = sizeof(T);
    std::memcpy(guest, &value, sizeof(T));
}

} // namespace

void TrophyService::Configure(const fs::path& app0, const fs::path& user_dir) {
    std::lock_guard<std::mutex> guard(lock_);
    user_dir_ = user_dir;
    np_comm_id_.clear();
    set_.reset();
    stores_.clear();
    const PackResult pack = PrepareTrophyPack(app0, user_dir);
    np_comm_id_ = pack.np_comm_id;
    if (!np_comm_id_.empty()) {
        set_ = LoadTrophySet(user_dir, np_comm_id_);
    }
    if (set_) {
        const auto toward_platinum = std::count_if(set_->trophies.begin(), set_->trophies.end(), CountsTowardPlatinum);
        std::printf("Trophies: %s loaded, %zu trophies, %d count toward Platinum\n", np_comm_id_.c_str(),
                    set_->trophies.size(), static_cast<int>(toward_platinum));
    } else {
        std::printf("Trophies: metadata unavailable (%s)%s%s\n", DescribePack(pack.status),
                    np_comm_id_.empty() ? "" : ", NP Communication ID ", np_comm_id_.c_str());
    }
}

void TrophyService::SetClock(std::function<int64_t()> unix_seconds) {
    std::lock_guard<std::mutex> guard(lock_);
    clock_ = std::move(unix_seconds);
}

TrophyStore& TrophyService::StoreOf(int32_t user) {
    const auto it = stores_.find(user);
    if (it != stores_.end()) {
        return it->second;
    }
    TrophyStore store;
    if (!np_comm_id_.empty()) {
        store = TrophyStore(TrophyDirectory(user_dir_, np_comm_id_), np_comm_id_, user);
        store.Load();
    }
    return stores_.emplace(user, std::move(store)).first->second;
}

bool TrophyService::KnownTrophy(int id) const {
    return set_ ? set_->Find(id) != nullptr : id >= 0 && id < kMaxTrophies;
}

int TrophyService::UnlockedInSet(const TrophyStore& store) const {
    return static_cast<int>(std::count_if(set_->trophies.begin(), set_->trophies.end(),
                                          [&store](const Trophy& trophy) { return store.IsUnlocked(trophy.id); }));
}

void TrophyService::Persist(TrophyStore& store) {
    if (!np_comm_id_.empty() && !store.Save()) {
        std::printf("Trophies: could not write %s\n", store.file().string().c_str());
    }
}

int32_t TrophyService::CreateContext(int32_t* context, int32_t user, uint32_t label, uint64_t options) {
    if (!context || options != 0) {
        return err::kInvalidArgument;
    }
    std::lock_guard<std::mutex> guard(lock_);
    if (contexts_.size() >= kMaxContexts) {
        return err::kContextExceedsMax;
    }
    for (const auto& [id, existing] : contexts_) {
        if (existing.user == user && existing.label == label) {
            return err::kContextAlreadyExists;
        }
    }
    *context = next_context_;
    contexts_[next_context_++] = Context{user, label, false};
    return 0;
}

int32_t TrophyService::CreateHandle(int32_t* handle) {
    if (!handle) {
        return err::kInvalidArgument;
    }
    std::lock_guard<std::mutex> guard(lock_);
    if (handles_.size() >= kMaxHandles) {
        return err::kHandleExceedsMax;
    }
    *handle = next_handle_;
    handles_.insert(next_handle_++);
    return 0;
}

int32_t TrophyService::FindContext(int32_t context, Context*& found) {
    const auto it = contexts_.find(context);
    if (context == kNoTrophy || it == contexts_.end()) {
        return err::kInvalidContext;
    }
    found = &it->second;
    return 0;
}

int32_t TrophyService::RegisterContext(int32_t context, int32_t handle, uint64_t options) {
    if (options != 0) {
        return err::kInvalidArgument;
    }
    std::lock_guard<std::mutex> guard(lock_);
    if (context == kNoTrophy) {
        return err::kInvalidContext;
    }
    if (handle == kNoTrophy) {
        return err::kInvalidHandle;
    }
    Context* found = nullptr;
    if (const int32_t status = FindContext(context, found)) {
        return status;
    }
    if (!handles_.count(handle)) {
        return err::kInvalidHandle;
    }
    if (found->registered) {
        return err::kAlreadyRegistered;
    }
    found->registered = true;
    if (set_) {
        std::printf("Trophies: progress %d/%zu\n", UnlockedInSet(StoreOf(found->user)), set_->trophies.size());
    }
    return 0;
}

int32_t TrophyService::DestroyHandle(int32_t handle) {
    std::lock_guard<std::mutex> guard(lock_);
    return handle != kNoTrophy && handles_.erase(handle) ? 0 : err::kInvalidHandle;
}

int32_t TrophyService::DestroyContext(int32_t context) {
    std::lock_guard<std::mutex> guard(lock_);
    return context != kNoTrophy && contexts_.erase(context) ? 0 : err::kInvalidContext;
}

int32_t TrophyService::CheckRegistered(int32_t context, int32_t handle, Context*& found) {
    if (context == kNoTrophy) {
        return err::kInvalidContext;
    }
    if (handle == kNoTrophy) {
        return err::kInvalidHandle;
    }
    if (const int32_t status = FindContext(context, found)) {
        return status;
    }
    if (!handles_.count(handle)) {
        return err::kInvalidHandle;
    }
    return found->registered ? 0 : err::kNotRegistered;
}

int32_t TrophyService::UnlockTrophy(int32_t context, int32_t handle, int32_t trophy_id, int32_t* platinum_id) {
    std::lock_guard<std::mutex> guard(lock_);
    if (context == kNoTrophy) {
        return err::kInvalidContext;
    }
    if (handle == kNoTrophy) {
        return err::kInvalidHandle;
    }
    if (trophy_id >= kMaxTrophies) {
        return err::kInvalidTrophyId;
    }
    if (!platinum_id) {
        return err::kInvalidArgument;
    }
    Context* found = nullptr;
    if (const int32_t status = CheckRegistered(context, handle, found)) {
        return status;
    }
    TrophyStore& store = StoreOf(found->user);
    if (store.IsUnlocked(trophy_id)) {
        return err::kTrophyAlreadyUnlocked;
    }
    const Trophy* trophy = nullptr;
    const Trophy* platinum = nullptr;
    int base = 0, base_unlocked = 0;
    if (set_) {
        for (const Trophy& each : set_->trophies) {
            if (each.grade == Grade::kPlatinum) {
                platinum = &each;
                if (each.id == trophy_id) {
                    return err::kPlatinumCannotUnlock;
                }
            }
            if (CountsTowardPlatinum(each)) {
                ++base;
                base_unlocked += store.IsUnlocked(each.id) ? 1 : 0;
            }
            if (each.id == trophy_id) {
                trophy = &each;
            }
        }
        if (!trophy) {
            return err::kInvalidTrophyId;
        }
    } else if (trophy_id < 0) {
        return err::kInvalidTrophyId;
    }
    *platinum_id = kNoTrophy;
    const int64_t now = clock_ ? clock_() : static_cast<int64_t>(std::time(nullptr));
    store.Unlock(trophy_id, now);
    std::vector<const Trophy*> earned{trophy};
    const int after = base_unlocked + (trophy && CountsTowardPlatinum(*trophy) ? 1 : 0);
    if (platinum && !store.IsUnlocked(platinum->id) && base > 0 && after == base) {
        store.Unlock(platinum->id, now);
        *platinum_id = platinum->id;
        earned.push_back(platinum);
    }
    Persist(store);
    for (const Trophy* item : earned) {
        if (!item) {
            std::printf("Trophies: trophy %d unlocked\n", trophy_id);
            continue;
        }
        std::printf("Trophies: trophy %d unlocked (%s)\n", item->id, GradeName(item->grade));
        notices_.push_back({item->id, item->grade, item->name, np_comm_id_, TrophyIconPath(user_dir_, np_comm_id_, item->id)});
    }
    if (set_) {
        std::printf("Trophies: progress %d/%zu\n", UnlockedInSet(store), set_->trophies.size());
    }
    return 0;
}

int32_t TrophyService::GetGameInfo(int32_t context, int32_t handle, void* details, void* data) {
    std::lock_guard<std::mutex> guard(lock_);
    if (context == kNoTrophy) {
        return err::kInvalidContext;
    }
    if (handle == kNoTrophy) {
        return err::kInvalidHandle;
    }
    if (!details || !data) {
        return err::kInvalidArgument;
    }
    if (GuestSize(details) != kGameDetailsStructSize || GuestSize(data) != kGameDataStructSize) {
        return err::kInvalidArgument;
    }
    Context* found = nullptr;
    if (const int32_t status = FindContext(context, found)) {
        return status;
    }
    if (!found->registered) {
        return err::kNotRegistered;
    }
    GameDetails info{};
    GameData progress{};
    const TrophyStore& store = StoreOf(found->user);
    if (set_) {
        const TrophyView view = BuildTrophyView(*set_, store);
        const auto total = [&](Grade grade) { return static_cast<uint32_t>(view.total_by_grade[static_cast<size_t>(grade)]); };
        const auto done = [&](Grade grade) { return static_cast<uint32_t>(view.unlocked_by_grade[static_cast<size_t>(grade)]); };
        info.groups = static_cast<uint32_t>(set_->groups);
        info.trophies = static_cast<uint32_t>(view.total);
        info.platinum = total(Grade::kPlatinum);
        info.gold = total(Grade::kGold);
        info.silver = total(Grade::kSilver);
        info.bronze = total(Grade::kBronze);
        CopyText(info.title, set_->title);
        CopyText(info.description, set_->detail);
        progress.unlocked = static_cast<uint32_t>(view.unlocked);
        progress.platinum = done(Grade::kPlatinum);
        progress.gold = done(Grade::kGold);
        progress.silver = done(Grade::kSilver);
        progress.bronze = done(Grade::kBronze);
        progress.percent = static_cast<uint32_t>(view.percent);
    } else {
        progress.unlocked = static_cast<uint32_t>(store.UnlockedCount());
    }
    StoreStruct(details, info);
    StoreStruct(data, progress);
    return 0;
}

int32_t TrophyService::GetTrophyInfo(int32_t context, int32_t handle, int32_t trophy_id, void* details, void* data) {
    std::lock_guard<std::mutex> guard(lock_);
    if (context == kNoTrophy) {
        return err::kInvalidContext;
    }
    if (handle == kNoTrophy) {
        return err::kInvalidHandle;
    }
    if (trophy_id >= kMaxTrophies) {
        return err::kInvalidTrophyId;
    }
    if (!details || !data) {
        return err::kInvalidArgument;
    }
    if (GuestSize(details) != kTrophyDetailsStructSize || GuestSize(data) != kTrophyDataStructSize) {
        return err::kInvalidArgument;
    }
    Context* found = nullptr;
    if (const int32_t status = FindContext(context, found)) {
        return status;
    }
    if (!found->registered) {
        return err::kNotRegistered;
    }
    const TrophyStore& store = StoreOf(found->user);
    const Trophy* trophy = set_ ? set_->Find(trophy_id) : nullptr;
    if (set_ && !trophy) {
        return 0;
    }
    TrophyDetails info{};
    TrophyData progress{};
    info.id = trophy_id;
    info.group = -1;
    progress.id = trophy_id;
    if (trophy) {
        info.grade = static_cast<int32_t>(trophy->grade);
        info.group = trophy->group;
        info.hidden = trophy->hidden;
        CopyText(info.name, trophy->name);
        CopyText(info.description, trophy->detail);
    }
    progress.unlocked = store.IsUnlocked(trophy_id);
    progress.timestamp = static_cast<uint64_t>(store.UnlockTime(trophy_id));
    StoreStruct(details, info);
    StoreStruct(data, progress);
    return 0;
}

int32_t TrophyService::GetUnlockState(int32_t context, int32_t handle, FlagArray* flags, uint32_t* count) {
    if (!flags || !count) {
        return err::kInvalidArgument;
    }
    std::lock_guard<std::mutex> guard(lock_);
    Context* found = nullptr;
    if (const int32_t status = CheckRegistered(context, handle, found)) {
        return status;
    }
    *flags = FlagArray{};
    *count = set_ ? static_cast<uint32_t>(set_->trophies.size()) : 0;
    for (const auto& [id, time] : StoreOf(found->user).Unlocked()) {
        if (KnownTrophy(id)) {
            flags->bits[id >> 5] |= 1u << (id & 31);
        }
    }
    return 0;
}

bool TrophyService::TakeNotice(UnlockNotice& notice) {
    std::lock_guard<std::mutex> guard(lock_);
    if (notices_.empty()) {
        return false;
    }
    notice = std::move(notices_.front());
    notices_.pop_front();
    return true;
}

} // namespace dcp::trophies
