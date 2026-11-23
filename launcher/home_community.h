// The Home page's most played classes, from the service's aggregate counts.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "home_stats.h"

namespace dcp::home {

inline constexpr size_t kClassesShown = 6;

struct RankedClass {
    std::string name;
    int64_t count = 0;
    std::string share;  // "41%", "<1%"; empty when there is nothing to share
};

// Most played first, ties in the game's class order; classes nobody played are left out.
std::vector<RankedClass> RankClasses(const std::vector<ClassCount>& classes, size_t limit = kClassesShown);

} // namespace dcp::home
