#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dcp::trophies {

inline constexpr int kMaxTrophies = 128;
inline constexpr int kNoTrophy = -1;

enum class Grade { kUnknown = 0, kPlatinum = 1, kGold = 2, kSilver = 3, kBronze = 4 };

const char* GradeName(Grade grade);

struct Trophy {
    int id = 0;
    Grade grade = Grade::kBronze;
    bool hidden = false;
    int platinum_link = kNoTrophy;
    int group = -1;
    std::string name;
    std::string detail;
};

struct TrophySet {
    std::string np_comm_id;
    std::string title;
    std::string detail;
    int groups = 0;
    std::vector<Trophy> trophies;

    const Trophy* Find(int id) const;
    int Count(Grade grade) const;
};

// Names, details and group count of one language file.
struct TrophyTexts {
    std::string title;
    std::string detail;
    int groups = 0;
    std::vector<Trophy> trophies;
};

std::optional<TrophySet> ParseTrophyConf(std::string_view xml);

std::optional<TrophyTexts> ParseTrophyTexts(std::string_view xml);

// Sets the title, detail, group count and each trophy's name and detail from the texts.
void ApplyTrophyTexts(TrophySet& set, const TrophyTexts& texts);

// Whether a trophy counts toward the Platinum trophy.
bool CountsTowardPlatinum(const Trophy& trophy);

std::string TrophyIconName(int id);

} // namespace dcp::trophies
