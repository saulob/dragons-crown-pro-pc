#include "exit_game_text.h"

#include <string.h>

#include "cyrillic_glyphs.h"

#define STORE_ROW "PlayStation$Store"
#define SHEET_TABLE_OFFSET 0x20
#define SHEET_TYPE_UTF8 3
#define KANA_TEXT_BYTES 512
#define NOT_THIS_LANGUAGE (-1)

/* UK shares the US text. Japanese and Korean keep the English row words their title menus already use. */
const ExitGameLanguage exit_game_languages[EXIT_GAME_LANGUAGE_COUNT] = {
    {"english", "English", "Data Install", "Install the game to the ##(HDD) for reduced load times.", "Exit Game", "Exit the game.", NULL, 0},
    {"german", "German", "Installation", "Installiere das Spiel auf die ##(HDD), um Ladezeiten zu verkürzen.", "Spiel beenden",
     "Beende das Spiel.", NULL, 0},
    {"spanish", "Spanish", "Instalación de datos", "Instala el juego en el ##(HDD) para reducir el tiempo de carga.",
     "Salir del juego", "Sal del juego.", NULL, 0},
    {"french", "French", "Installation des données", "Installez le jeu sur le ##(HDD) pour réduire les temps de chargement.",
     "Quitter le jeu", "Quittez le jeu.", NULL, 0},
    {"italian", "Italian", "Installazione dei dati", "Installa il gioco sul ##(HDD) per ridurre i tempi di caricamento.",
     "Esci dal gioco", "Esci dal gioco.", NULL, 0},
    {"brazilian_portuguese", "Portuguese (Brazil)", "Instalação de Dados",
     "Instale o jogo no ##(HDD) para diminuir os tempos de carregamento.", "Sair do Jogo", "Saia do jogo.",
     "Instalacao de Dados", 0},
    {"russian", "Russian", "Установка данных", "Установите игру на ##(HDD), чтобы уменьшить время загрузки.", "Выйти",
     "Выйти из игры.", NULL, 1},
    {"japanese", "Japanese", "Data Install", "##(HDD)にデータをインストールして読み込みを高速化します", "Exit Game", "ゲームを終了します", NULL, 0},
    {"traditional_chinese", "Traditional Chinese", "安裝遊戲資料", "將遊戲資料安裝至##(HDD)中以加速載入速度。", "結束遊戲", "結束遊戲。", NULL, 0},
    {"korean", "Korean", "Data Install", "##(HDD)에 데이터를 설치하여 로딩 시간을 빠르게 합니다", "Exit Game", "게임을 종료합니다", NULL, 0},
};

typedef struct {
    uint8_t *install, *row, *help, *store_help;
} Records;

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int find_records(uint8_t *sheet, size_t size, Records *out) {
    if (size < SHEET_TABLE_OFFSET || memcmp(sheet, "FMSB", 4) || sheet[0x18] != SHEET_TYPE_UTF8 ||
        read_u32(sheet + 0x14) != EXIT_GAME_SYSTEM_MSG_COUNT)
        return 0;
    const size_t end = (size_t)read_u32(sheet + 4) + SHEET_TABLE_OFFSET;
    const size_t first = SHEET_TABLE_OFFSET + (size_t)EXIT_GAME_SYSTEM_MSG_COUNT * 8;
    if (end > size - 4 || end <= first || memcmp(sheet + end, "FEOC", 4)) return 0;
    uint8_t *p = sheet + first;
    for (int index = 0; index <= EXIT_GAME_STORE_HELP_RECORD; ++index) {
        if (index == EXIT_GAME_INSTALL_RECORD) out->install = p;
        else if (index == EXIT_GAME_ROW_RECORD) out->row = p;
        else if (index == EXIT_GAME_HELP_RECORD) out->help = p;
        else if (index == EXIT_GAME_STORE_HELP_RECORD) out->store_help = p;
        while (p < sheet + end && *p) ++p;
        if (p >= sheet + end) return 0;
        ++p;
    }
    return 1;
}

static int is_record(const uint8_t *record, const char *text) {
    return !strcmp((const char *)record, text);
}

static int patch_as(Records *records, const ExitGameLanguage *candidate) {
    const int done = is_record(records->row, candidate->row) && is_record(records->help, candidate->help);
    const int install = is_record(records->install, candidate->install_original) ||
                        (candidate->install_unaccented && is_record(records->install, candidate->install_unaccented));
    const int original = install && is_record(records->row, STORE_ROW) && is_record(records->help, candidate->help_original);
    if (!original && !done) return NOT_THIS_LANGUAGE;
    if (done) return EXIT_GAME_TEXT_ALREADY;
    const size_t install_length = strlen((const char *)records->install), store_length = strlen(STORE_ROW);
    const size_t help_length = strlen(candidate->help_original), store_help_length = strlen((const char *)records->store_help);
    const size_t row_length = strlen(candidate->row), new_help_length = strlen(candidate->help);
    if (row_length > store_length || new_help_length > help_length) return EXIT_GAME_TEXT_NO_ROOM;
    const size_t spare = store_length - row_length;
    memset(records->install + install_length, ' ', spare);
    records->install[install_length + spare] = 0;
    memcpy(records->install + install_length + spare + 1, candidate->row, row_length + 1);
    memcpy(records->help, candidate->help, new_help_length + 1);
    memmove(records->help + new_help_length + 1, records->store_help, store_help_length);
    memset(records->help + new_help_length + 1 + store_help_length, ' ', help_length - new_help_length);
    records->help[help_length + 1 + store_help_length] = 0;
    return EXIT_GAME_TEXT_PATCHED;
}

static int patch_as_kana(Records *records, const ExitGameLanguage *candidate) {
    static char text[4][KANA_TEXT_BYTES];
    const char *const plain[4] = {candidate->install_original, candidate->help_original, candidate->row, candidate->help};
    for (int i = 0; i < 4; ++i) {
        if (!cyrillic_kana_encode(plain[i], text[i], sizeof(text[i]))) return NOT_THIS_LANGUAGE;
    }
    const ExitGameLanguage kana = {candidate->key, candidate->language, text[0], text[1], text[2], text[3], NULL, 0};
    return patch_as(records, &kana);
}

ExitGameTextStatus exit_game_text_patch(uint8_t *sheet, size_t size, const ExitGameLanguage **language) {
    Records records;
    if (!find_records(sheet, size, &records)) return EXIT_GAME_TEXT_NOT_SYSTEM_MSG;
    for (size_t i = 0; i < EXIT_GAME_LANGUAGE_COUNT; ++i) {
        const ExitGameLanguage *candidate = &exit_game_languages[i];
        int status = patch_as(&records, candidate);
        if (status == NOT_THIS_LANGUAGE && candidate->kana_cyrillic) status = patch_as_kana(&records, candidate);
        if (status == NOT_THIS_LANGUAGE) continue;
        if (language) *language = candidate;
        return (ExitGameTextStatus)status;
    }
    return EXIT_GAME_TEXT_UNKNOWN;
}

const char *exit_game_text_status_text(ExitGameTextStatus status) {
    switch (status) {
    case EXIT_GAME_TEXT_PATCHED: return "patched";
    case EXIT_GAME_TEXT_ALREADY: return "already patched";
    case EXIT_GAME_TEXT_NOT_SYSTEM_MSG: return "not the SystemMsg sheet";
    case EXIT_GAME_TEXT_UNKNOWN: return "language not recognised";
    case EXIT_GAME_TEXT_NO_ROOM: return "text does not fit";
    }
    return "unknown";
}
