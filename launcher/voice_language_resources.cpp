#include "voice_language_resources.h"

#include <string_view>

#include "cpk_patch.h"
#include "dcp_launcher.h"
#include "dcp_voice.h"
#include "utf8.h"

namespace dcp {
namespace {

constexpr std::string_view kSoundTable = "snd/dc_sound.acf";

uint32_t InLanguage(uint32_t japanese, int language) {
    return language == DCP_VOICE_ENGLISH ? japanese + DCP_VOICE_ENGLISH_OFFSET : japanese;
}

void AddBanks(std::vector<std::string>& names, std::string_view prefix, const uint32_t* families, int count,
              std::string_view suffix, int language) {
    for (int i = 0; i < count; ++i) {
        names.push_back(std::string(prefix) + std::to_string(InLanguage(families[i], language)) + std::string(suffix));
    }
}

} // namespace

std::vector<std::string> VoiceLanguageBankNames(int category, int language) {
    std::vector<std::string> names;
    if (language != DCP_VOICE_ENGLISH && language != DCP_VOICE_JAPANESE) {
        return names;
    }
    switch (category) {
    case DCP_VOICE_PLAYER:
        AddBanks(names, "snd/se_player", kDcpPlayerVoiceFamilies, DCP_PLAYER_VOICE_FAMILY_COUNT, "_v.acb", language);
        AddBanks(names, "snd/se_voice", kDcpCampVoiceFamilies, DCP_CAMP_VOICE_FAMILY_COUNT, ".acb", language);
        break;
    case DCP_VOICE_NPC:
        AddBanks(names, "snd/se_gimmick", kDcpGimmickVoiceFamilies, DCP_GIMMICK_VOICE_FAMILY_COUNT, "_v.acb", language);
        AddBanks(names, "snd/se_enemy", kDcpNpcEnemyBankFamilies, DCP_NPC_ENEMY_BANK_FAMILY_COUNT, "_v.acb", language);
        break;
    case DCP_VOICE_ENEMY:
        AddBanks(names, "snd/se_enemy", kDcpEnemyVoiceFamilies, DCP_ENEMY_VOICE_FAMILY_COUNT, "_v.acb", language);
        break;
    case DCP_VOICE_NARRATOR:
        for (uint32_t i = 0; i < DCP_NARRATOR_COUNT; ++i) {
            const std::string stem = "snd/narration_gm" +
                                     std::to_string(InLanguage(DCP_NARRATOR_FIRST_BASE + i * DCP_NARRATOR_BASE_STEP, language));
            names.push_back(stem + ".acb");
            names.push_back(stem + ".awb");
        }
        break;
    default:
        break;
    }
    return names;
}

unsigned VoiceLanguageCategories(const std::filesystem::path& app0, int language) {
    if (language != DCP_VOICE_ENGLISH && language != DCP_VOICE_JAPANESE) {
        return 0;
    }
    CpkArchive archive;
    if (!archive.Open(app0 / "DRACRO.CPK") || !archive.Find(kSoundTable)) {
        return 0;
    }
    unsigned categories = 0;
    for (int category = DCP_VOICE_PLAYER; category < DCP_VOICE_CATEGORY_COUNT; ++category) {
        bool all = true;
        for (const std::string& name : VoiceLanguageBankNames(category, language)) {
            all = all && archive.Find(name) != nullptr;
        }
        if (all) {
            categories |= DCP_VOICE_CATEGORY_BIT(category);
        }
    }
    return categories;
}

std::vector<int> VoiceLanguagesOffered(const std::filesystem::path& app0, const GameBuild* build) {
    std::vector<int> languages;
    if (!SupportsVoiceLanguage(build)) {
        return languages;
    }
    for (const int language : {int(DCP_VOICE_ENGLISH), int(DCP_VOICE_JAPANESE)}) {
        if (VoiceLanguageCategories(app0, language) != 0) {
            languages.push_back(language);
        }
    }
    return languages;
}

} // namespace dcp

extern "C" unsigned dcp_voice_language_resources(const char* app0, int language) {
    return app0 ? dcp::VoiceLanguageCategories(dcp::PathFromUtf8(app0), language) : 0u;
}
