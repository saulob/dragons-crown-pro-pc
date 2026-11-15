#include "boss_battle_text.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cyrillic_glyphs.h"

typedef struct {
    const char *key;
    const char *text[BOSS_BATTLE_TEXT_COUNT];
    const char *boss[BOSS_BATTLE_TEXT_BOSSES];
    const char *classes[BOSS_BATTLE_TEXT_CLASSES];
} Language;

/* Boss names: ObjectParam (the Wyvern without its "(Female)", the Pirates as the defeat banner names them). Classes:
 * ObjectParam 0-5. Inn: SystemMsg 232. The port's languages take the same records from localization/<translation>. */
static const Language kLanguages[BOSS_BATTLE_TEXT_LANGUAGES] = {
    {"english",
        {"Fight All",
         "Fight every boss in sequence.",
         "Return to Dragon's Haven Inn.",
         "Attempts",
         "Wins",
         "Deaths",
         "Quits",
         "Win Rate",
         "Best Time",
         "Runs",
         "Clears",
         "Best Progress",
         "Quit Boss Battle",
         "Quit Boss Battle?",
         "%s - %s, level %d"},
        {"Harpy", "Wyvern", "Doom Beetle", "Golem", "Gazer", "Pirates", "Minotaur", "Vampire", "Warlock", "Medusa",
         "Wraith", "Killer Rabbit", "Kraken", "Cyclops", "Chimera", "Gargoyle Gate", "Red Dragon", "Arch Demon",
         "Ancient Dragon", "Demon Lord"},
        {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"}},
    {"french",
        {"Affronter tous",
         "Affrontez tous les boss à la suite.",
         "Retour au Havre du dragon.",
         "Tentatives",
         "Victoires",
         "Morts",
         "Abandons",
         "Taux de victoire",
         "Meilleur temps",
         "Parties",
         "Réussites",
         "Meilleure progression",
         "Quitter le combat de boss",
         "Quitter le combat de boss ?",
         "%s - %s, niveau %d"},
        {"Harpie", "Wyverne", "Scarabée maudit", "Golem", "Oculus", "Pirates", "Minotaure", "Vampire", "Sorcier",
         "Méduse", "Faucheur", "Lapin tueur", "Kraken", "Cyclope", "Chimère", "Porte des gargouilles", "Dragon rouge",
         "Archidémon", "Ancien dragon", "Seigneur démon"},
        {"Guerrier", "Amazone", "Elfe", "Nain", "Ensorceleuse", "Enchanteur"}},
    {"german",
        {"Alle bekämpfen",
         "Bekämpfe alle Bosse nacheinander.",
         "Zurück zum „Drachenhafen“-Gasthaus.",
         "Versuche",
         "Siege",
         "Tode",
         "Abbrüche",
         "Siegquote",
         "Bestzeit",
         "Läufe",
         "Abschlüsse",
         "Bester Fortschritt",
         "Bosskampf beenden",
         "Bosskampf beenden?",
         "%s - %s, Stufe %d"},
        {"Harpyie", "Wyvern", "Unheilkäfer", "Golem", "Gaffer", "Piraten", "Minotaurus", "Vampir", "Hexenmeister",
         "Medusa", "Geist", "Killerkaninchen", "Kraken", "Zyklop", "Chimäre", "Gargoyle-Tor", "Roter Drache",
         "Erzdämon", "Uralter Drache", "Dämonenlord"},
        {"Kämpfer", "Amazone", "Elfe", "Zwerg", "Hexe", "Magier"}},
    {"italian",
        {"Affrontali tutti",
         "Affronta tutti i boss in sequenza.",
         "Torna alla Locanda del drago guardiano.",
         "Tentativi",
         "Vittorie",
         "Morti",
         "Ritiri",
         "Percentuale vittorie",
         "Tempo migliore",
         "Partite",
         "Completamenti",
         "Progresso migliore",
         "Abbandona la sfida ai boss",
         "Abbandonare la sfida ai boss?",
         "%s - %s, livello %d"},
        {"Arpia", "Viverna", "Scarabeo infernale", "Golem", "Osservatore", "Pirati", "Minotauro", "Vampiro", "Warlock",
         "Medusa", "Wraith", "Coniglio killer", "Kraken", "Ciclopi", "Chimera", "Cancello del gargoyle", "Drago rosso",
         "Arcidemone", "Drago antico", "Signore dei demoni"},
        {"Guerriero", "Amazzone", "Elfa", "Nano", "Strega", "Mago"}},
    {"spanish",
        {"Luchar contra todos",
         "Lucha contra todos los jefes seguidos.",
         "Vuelve a La Guarida del Dragón.",
         "Intentos",
         "Victorias",
         "Muertes",
         "Abandonos",
         "Tasa de victorias",
         "Mejor tiempo",
         "Partidas",
         "Completadas",
         "Mejor progreso",
         "Abandonar la batalla de jefes",
         "¿Abandonar la batalla de jefes?",
         "%s - %s, nivel %d"},
        {"Arpía", "Bestia alada", "Escarabajo maldito", "Gólem", "Mirón", "Piratas", "Minotauro", "Vampiro", "Brujo",
         "Medusa", "Espectro", "Conejo asesino", "Kraken", "Cíclope", "Quimera", "Puerta de las Gárgolas",
         "Dragón rojo", "Archidemonio", "Antiguo dragón", "Lord demonio"},
        {"Guerrero", "Amazona", "Elfa", "Enano", "Hechicera", "Mago"}},
    {"brazilian_portuguese",
        {"Enfrentar Todos",
         "Enfrente todos os chefes em sequência.",
         "Volte para a Taverna Refúgio do Dragão.",
         "Tentativas",
         "Vitórias",
         "Mortes",
         "Desistências",
         "Taxa de Vitórias",
         "Melhor Tempo",
         "Partidas",
         "Conclusões",
         "Melhor Progresso",
         "Sair da Batalha de Chefes",
         "Sair da Batalha de Chefes?",
         "%s - %s, nível %d"},
        {"Harpia", "Wyvern", "Besouro da Perdição", "Golem", "Observador", "Piratas", "Minotauro", "Vampiro", "Bruxo",
         "Medusa", "Aparição", "Coelho Assassino", "Kraken", "Ciclope", "Quimera", "Portão Gárgula", "Dragão Vermelho",
         "Arquidemônio", "Dragão Ancestral", "Lorde Demônio"},
        {"Guerreiro", "Amazona", "Elfa", "Anão", "Feiticeira", "Mago"}},
    {"russian",
        {"Сразиться со всеми",
         "Сразитесь со всеми боссами подряд.",
         "Вернуться в трактир \"Убежище дракона\".",
         "Попытки",
         "Победы",
         "Смерти",
         "Выходы",
         "Процент побед",
         "Лучшее время",
         "Забеги",
         "Прохождения",
         "Лучший прогресс",
         "Выйти из битвы с боссом",
         "Выйти из битвы с боссом?",
         "%s - %s, уровень %d"},
        {"Гарпия", "Виверна", "Жук тьмы", "Голем", "Сглаз", "Пираты", "Минотавр", "Вампир", "Колдун", "Медуза", "Дух",
         "Кролик-убийца", "Кракен", "Циклоп", "Химера", "Горгульи врата", "Алый дракон", "Архидемон", "Древний дракон",
         "Демонический лорд"},
        {"Рыцарь", "Амазонка", "Эльфийка", "Гном", "Ведьма", "Волшебник"}},
    {"japanese",
        {"全ボス連戦",
         "すべてのボスと続けて戦います",
         "竜の天国亭に戻ります",
         "挑戦回数",
         "勝利回数",
         "敗北回数",
         "中断回数",
         "勝率",
         "ベストタイム",
         "挑戦回数",
         "クリア回数",
         "最高到達",
         "ボスバトルをやめる",
         "ボスバトルをやめますか?",
         "%s - %s Lv%d"},
        {"ハーピー", "ワイバーン", "ドゥームビートル", "ゴーレム", "ゲイザー", "パイレーツ", "ミノタウロス", "ヴァンパイア", "ウォーロック", "メデューサ", "レイス", "キラーラビット",
         "クラーケン", "サイクロプス", "キマイラ", "ガーゴイルゲート", "レッドドラゴン", "アークデーモン", "エンシェントドラゴン", "デーモンロード"},
        {"ファイター", "アマゾン", "エルフ", "ドワーフ", "ソーサレス", "ウイザード"}},
    {"traditional_chinese",
        {"連續挑戰",
         "依序挑戰所有首領。",
         "返回龍天堂酒館。",
         "挑戰次數",
         "勝利次數",
         "陣亡次數",
         "放棄次數",
         "勝率",
         "最佳時間",
         "挑戰次數",
         "完成次數",
         "最佳進度",
         "放棄首領挑戰",
         "要放棄首領挑戰嗎？",
         "%s - %s 等級%d"},
        {"鳥身女妖", "雙足飛龍", "末日甲蟲", "魔像", "凝視怪", "海盜", "牛頭怪", "吸血鬼", "術士", "梅杜莎", "怨靈", "殺人兔", "海怪", "獨眼巨人", "奇美拉",
         "石像鬼之門", "紅龍", "高階惡魔", "上古巨龍", "魔侯"},
        {"戰士", "亞馬遜女戰士", "精靈", "矮人", "女法師", "巫師"}},
    {"korean",
        {"연속 배틀",
         "모든 보스와 차례로 싸운다",
         "용의 천국정으로 돌아갑니다",
         "도전 횟수",
         "승리",
         "사망",
         "포기",
         "승률",
         "최고 기록",
         "도전 횟수",
         "완주",
         "최고 진행",
         "보스 배틀 그만두기",
         "보스 배틀을 그만두시겠습니까?",
         "%s - %s Lv%d"},
        {"하피", "와이번", "둠 비틀", "골렘", "게이저", "해적", "미노타우로스", "뱀파이어", "워록", "메두사", "레이스", "킬러 래빗", "크라켄", "사이클롭스", "키마이라",
         "가고일 게이트", "레드 드래곤", "아크 데몬", "에이션트 드래곤", "데몬 로드"},
        {"파이터", "아마존", "엘프", "드워프", "소서리스", "위자드"}},
};

enum { TEXT_BYTES = 160 };

/* Russian re-encoded for a font that draws it over kana. */
static struct {
    char text[BOSS_BATTLE_TEXT_COUNT][TEXT_BYTES];
    char boss[BOSS_BATTLE_TEXT_BOSSES][TEXT_BYTES];
    char classes[BOSS_BATTLE_TEXT_CLASSES][TEXT_BYTES];
    Language language;
} kana;

static _Atomic(const Language *) active = &kLanguages[0];

static int encode(const char *text, char *out, const char **slot) {
    if (!cyrillic_kana_encode(text, out, TEXT_BYTES)) return 0;
    *slot = out;
    return 1;
}

static const Language *kana_copy(const Language *source) {
    kana.language = *source;
    for (int i = 0; i < BOSS_BATTLE_TEXT_COUNT; ++i)
        if (!encode(source->text[i], kana.text[i], &kana.language.text[i])) return NULL;
    for (int i = 0; i < BOSS_BATTLE_TEXT_BOSSES; ++i)
        if (!encode(source->boss[i], kana.boss[i], &kana.language.boss[i])) return NULL;
    for (int i = 0; i < BOSS_BATTLE_TEXT_CLASSES; ++i)
        if (!encode(source->classes[i], kana.classes[i], &kana.language.classes[i])) return NULL;
    return &kana.language;
}

int boss_battle_text_set_language(const char *key, int kana_cyrillic) {
    for (int i = 0; key && i < BOSS_BATTLE_TEXT_LANGUAGES; ++i) {
        if (strcmp(key, kLanguages[i].key)) continue;
        const Language *chosen = kana_cyrillic ? kana_copy(&kLanguages[i]) : &kLanguages[i];
        atomic_store(&active, chosen ? chosen : &kLanguages[0]);
        if (!chosen) printf("Boss Battle: %s text does not fit its font encoding, English is shown\n", key);
        return chosen != NULL;
    }
    atomic_store(&active, &kLanguages[0]);
    return 0;
}

const char *boss_battle_text_language(void) {
    return atomic_load(&active)->key;
}

const char *boss_battle_text(BossBattleTextKey key) {
    return key >= 0 && key < BOSS_BATTLE_TEXT_COUNT ? atomic_load(&active)->text[key] : "";
}

const char *boss_battle_text_boss(int index) {
    return index >= 0 && index < BOSS_BATTLE_TEXT_BOSSES ? atomic_load(&active)->boss[index] : "";
}

const char *boss_battle_text_class(int class_index) {
    return class_index >= 0 && class_index < BOSS_BATTLE_TEXT_CLASSES ? atomic_load(&active)->classes[class_index] : NULL;
}

const char *boss_battle_text_language_key(int index) {
    return index >= 0 && index < BOSS_BATTLE_TEXT_LANGUAGES ? kLanguages[index].key : NULL;
}
