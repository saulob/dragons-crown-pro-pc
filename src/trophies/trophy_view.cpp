#include "trophies/trophy_view.h"

namespace dcp::trophies {

int ProgressPercent(int unlocked, int total) {
    return total > 0 ? unlocked * 100 / total : 0;
}

TrophyView BuildTrophyView(const TrophySet& set, const TrophyStore& store) {
    TrophyView view;
    view.np_comm_id = set.np_comm_id;
    view.title = set.title;
    for (const Trophy& trophy : set.trophies) {
        TrophyRow row;
        row.id = trophy.id;
        row.grade = trophy.grade;
        row.unlocked = store.IsUnlocked(trophy.id);
        row.unlock_time = store.UnlockTime(trophy.id);
        row.masked = trophy.hidden && !row.unlocked;
        row.name = row.masked ? kHiddenTrophyName : trophy.name;
        row.detail = row.masked ? kHiddenTrophyDetail : trophy.detail;
        ++view.total;
        ++view.total_by_grade[static_cast<size_t>(trophy.grade)];
        if (row.unlocked) {
            ++view.unlocked;
            ++view.unlocked_by_grade[static_cast<size_t>(trophy.grade)];
        }
        view.rows.push_back(std::move(row));
    }
    view.percent = ProgressPercent(view.unlocked, view.total);
    return view;
}

} // namespace dcp::trophies
