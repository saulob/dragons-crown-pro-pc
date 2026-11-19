#include "comms_text.h"

#include <stdatomic.h>
#include <string.h>

#include "cyrillic_glyphs.h"

typedef struct {
    const char *key;
    const char *text[COMMS_TEXT_MESSAGES];
} Language;

/* Order of the ids: Hello, Thanks, Good job, Follow me, Group up, Need help, Ready, Let's go. */
static const Language kLanguages[COMMS_TEXT_LANGUAGES] = {
    {"english", {"Hello", "Thanks", "Good job", "Follow me", "Group up", "Need help", "Ready", "Let's go"}},
    {"french", {"Salut", "Merci", "Bien joué", "Suivez-moi", "Regroupez-vous", "Besoin d'aide", "Prêt", "On y va"}},
    {"german", {"Hallo", "Danke", "Gut gemacht", "Folgt mir", "Sammeln", "Brauche Hilfe", "Bereit", "Los geht's"}},
    {"italian", {"Ciao", "Grazie", "Ben fatto", "Seguitemi", "Riuniamoci", "Serve aiuto", "Pronto", "Andiamo"}},
    {"spanish", {"Hola", "Gracias", "Buen trabajo", "Seguidme", "Agrupaos", "Necesito ayuda", "Listo", "Vamos"}},
    {"brazilian_portuguese",
        {"Olá", "Valeu", "Bom trabalho", "Me sigam", "Juntem-se", "Preciso de ajuda", "Pronto", "Vamos lá"}},
    {"russian", {"Привет", "Спасибо", "Отлично", "За мной", "Держимся вместе", "Нужна помощь", "Готов", "Вперед"}},
    {"japanese", {"こんにちは", "ありがとう", "ナイス", "ついてきて", "集合", "助けて", "準備OK", "行こう"}},
    {"traditional_chinese", {"你好", "謝謝", "幹得好", "跟我來", "集合", "需要幫助", "準備好了", "出發吧"}},
    {"korean", {"안녕", "고마워", "잘했어", "따라와", "모여", "도와줘", "준비됐어", "가자"}},
};

enum { TEXT_BYTES = 160 };

static struct {
    char text[COMMS_TEXT_MESSAGES][TEXT_BYTES];
    Language language;
} kana;

static _Atomic(const Language *) active = &kLanguages[0];

static const Language *kana_copy(const Language *source) {
    kana.language = *source;
    for (int i = 0; i < COMMS_TEXT_MESSAGES; ++i) {
        if (!cyrillic_kana_encode(source->text[i], kana.text[i], TEXT_BYTES)) return NULL;
        kana.language.text[i] = kana.text[i];
    }
    return &kana.language;
}

int comms_text_set_language(const char *key, int kana_cyrillic) {
    for (int i = 0; key && i < COMMS_TEXT_LANGUAGES; ++i) {
        if (strcmp(key, kLanguages[i].key)) continue;
        const Language *chosen = kana_cyrillic ? kana_copy(&kLanguages[i]) : &kLanguages[i];
        atomic_store(&active, chosen ? chosen : &kLanguages[0]);
        return chosen != NULL;
    }
    atomic_store(&active, &kLanguages[0]);
    return 0;
}

const char *comms_text_language(void) {
    return atomic_load(&active)->key;
}

const char *comms_text_message(int id) {
    return id >= 0 && id < COMMS_TEXT_MESSAGES ? atomic_load(&active)->text[id] : "";
}

const char *comms_text_lookup(const char *language_key, int id) {
    for (int i = 0; language_key && i < COMMS_TEXT_LANGUAGES; ++i)
        if (!strcmp(language_key, kLanguages[i].key)) return id >= 0 && id < COMMS_TEXT_MESSAGES ? kLanguages[i].text[id] : "";
    return NULL;
}

size_t comms_text_game_utf16(int id, uint16_t *out, size_t capacity) {
    if (id < 0 || id >= COMMS_TEXT_MESSAGES || !out || capacity < 2) return 0;
    const Language *language = atomic_load(&active);
    const unsigned char *p = (const unsigned char *)language->text[id];
    size_t n = 0;
    while (*p && n + 2 < capacity) {
        uint32_t cp = *p++;
        int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
        cp = extra == 3 ? cp & 0x07 : extra == 2 ? cp & 0x0f : extra == 1 ? cp & 0x1f : cp;
        while (extra-- > 0 && (*p & 0xc0) == 0x80) cp = (cp << 6) | (*p++ & 0x3f);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xd800 + (cp >> 10));
            out[n++] = (uint16_t)(0xdc00 + (cp & 0x3ff));
        } else {
            out[n++] = (uint16_t)cp;
        }
    }
    out[n] = 0;
    return n;
}

const char *comms_text_language_key(int index) {
    return index >= 0 && index < COMMS_TEXT_LANGUAGES ? kLanguages[index].key : NULL;
}
