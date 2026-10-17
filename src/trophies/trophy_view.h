#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "trophies/trophy_data.h"
#include "trophies/trophy_store.h"

namespace dcp::trophies {

inline constexpr char kHiddenTrophyName[] = "Hidden Trophy";
inline constexpr char kHiddenTrophyDetail[] = "Unlock this trophy to see what it is.";

struct TrophyRow {
    int id = 0;
    Grade grade = Grade::kBronze;
    bool unlocked = false;
    bool masked = false;
    int64_t unlock_time = 0;
    std::string name;
    std::string detail;
};

struct TrophyView {
    std::string np_comm_id;
    std::string title;
    std::vector<TrophyRow> rows;
    int total = 0;
    int unlocked = 0;
    int percent = 0;
    std::array<int, 5> total_by_grade{};    // indexed by Grade
    std::array<int, 5> unlocked_by_grade{}; // indexed by Grade
};

int ProgressPercent(int unlocked, int total);

TrophyView BuildTrophyView(const TrophySet& set, const TrophyStore& store);

} // namespace dcp::trophies
