// The trophy list of a game as its package describes it: the structure of TROPCONF.XML (ids, grades, hidden flags,
// platinum links) and the texts of a language file (TROP_nn.XML), parsed defensively: package data is input, not trust.
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
    int platinum_link = kNoTrophy; // pid
    int group = -1;                // gid; -1 when the list has none
    std::string name;
    std::string detail;
};

struct TrophySet {
    std::string np_comm_id;
    std::string title;
    std::string detail;
    int groups = 0;
    std::vector<Trophy> trophies; // ascending ids

    const Trophy* Find(int id) const;
    int Count(Grade grade) const;
};

// Names, details and group count of one language file.
struct TrophyTexts {
    std::string title;
    std::string detail;
    int groups = 0;
    std::vector<Trophy> trophies; // id, name and detail only
};

// nullopt for anything that is not a well-formed trophy list: no trophies, an id outside 0..127 or repeated, an unknown
// grade, nesting or size beyond sane limits.
std::optional<TrophySet> ParseTrophyConf(std::string_view xml);

std::optional<TrophyTexts> ParseTrophyTexts(std::string_view xml);

// Sets the title, detail, group count and each trophy's name and detail from the texts.
void ApplyTrophyTexts(TrophySet& set, const TrophyTexts& texts);

// Whether a trophy counts toward the Platinum trophy.
bool CountsTowardPlatinum(const Trophy& trophy);

// "TROP<id>.PNG": the icon file of a trophy inside the extracted pack.
std::string TrophyIconName(int id);

} // namespace dcp::trophies
