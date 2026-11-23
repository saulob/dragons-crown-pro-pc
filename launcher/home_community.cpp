#include "home_community.h"

#include <algorithm>

namespace dcp::home {
namespace {

constexpr const char* kClassOrder[] = {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"};

int ClassIndex(const std::string& name) {
    for (int i = 0; i < 6; ++i) {
        if (name == kClassOrder[i]) {
            return i;
        }
    }
    return -1;
}

std::string Share(int64_t count, int64_t total) {
    if (count <= 0 || total <= 0) {
        return "";
    }
    const int64_t percent = (count * 100 + total / 2) / total;
    return percent < 1 ? "<1%" : std::to_string(percent) + "%";
}

} // namespace

std::vector<RankedClass> RankClasses(const std::vector<ClassCount>& classes, size_t limit) {
    std::vector<RankedClass> ranked;
    int64_t total = 0;
    for (const ClassCount& row : classes) {
        if (row.count > 0 && ClassIndex(row.name) >= 0) {
            ranked.push_back({row.name, row.count, ""});
            total += row.count;
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const RankedClass& a, const RankedClass& b) {
        return a.count != b.count ? a.count > b.count : ClassIndex(a.name) < ClassIndex(b.name);
    });
    if (ranked.size() > limit) {
        ranked.resize(limit);
    }
    for (RankedClass& row : ranked) {
        row.share = Share(row.count, total);
    }
    return ranked;
}

} // namespace dcp::home
