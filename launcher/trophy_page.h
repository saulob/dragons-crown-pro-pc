#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "trg/launcher.h"
#include "trophies/trophy_key.h"
#include "trophy_content.h"

namespace dcp {

class LauncherHost;

class TrophyPage {
public:
    TrophyPage(LauncherHost& host, std::filesystem::path user_dir) : host_(host), user_dir_(std::move(user_dir)) {}

    void SetGame(const std::filesystem::path& app0);
    void Invalidate();
    void Draw(trg::Ui& ui);

private:
    ImTextureID Icon(int id);
    void DrawBody(trg::Ui& ui);
    void DrawKeySetup(trg::Ui& ui, const char* title, const char* detail, const char* more);
    void DrawKeyRow(trg::Ui& ui);
    void DrawModals(trg::Ui& ui);
    void ImportKey();
    void OfferKey(const trophies::TrophyKey& key, trophies::KeySource source);
    void StoreKey(const trophies::TrophyKey& key, trophies::KeySource source);
    void ShowNotice(std::string text);
    void DrawSummary(trg::Ui& ui);
    void DrawSavedUnlocks(trg::Ui& ui);
    void DrawRow(trg::Ui& ui, const trophies::TrophyRow& row, float width);

    enum class Modal { kNone, kEnter, kReplace, kRemove, kNotice };

    LauncherHost& host_;
    std::filesystem::path user_dir_;
    std::filesystem::path app0_;
    std::optional<TrophyPageContent> content_;
    int last_frame_ = -10;
    int icons_budget_ = 0;
    std::map<std::filesystem::path, ImTextureID> icons_;
    Modal modal_ = Modal::kNone;
    bool open_modal_ = false;
    bool show_key_ = false;
    char entry_[48] = {};
    std::string entry_error_;
    std::string notice_;
    std::optional<trophies::TrophyKey> pending_key_;
    trophies::KeySource pending_source_ = trophies::KeySource::kEntered;
};

} // namespace dcp
