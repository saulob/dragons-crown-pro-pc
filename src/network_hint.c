#include "network_hint.h"

#include <string.h>

static NetworkHintFunctions functions;
static int portuguese_text, failed, owner_shown;
static uint64_t shown_window, shown_ui;

static uint64_t guest_u64(uint64_t at) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)at, sizeof(value));
    return value;
}

static uint64_t window_of(uint64_t ui) {
    return guest_u64(ui + NETWORK_HINT_UI_WINDOWS + (uint64_t)NETWORK_HINT_WINDOW_TYPE * NETWORK_HINT_UI_SLOT_SIZE);
}

static uint64_t show_method(uint64_t window) {
    return guest_u64(guest_u64(window) + NETWORK_HINT_SHOW_SLOT);
}

static int show_is_the_games(uint64_t window) {
    static const uint8_t body[] = {0x89, 0xb7, 0x34, 0x06, 0x00, 0x00, 0x88, 0x97, 0x39, 0x06, 0x00, 0x00,
                                   0xbe, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x47, 0x50};
    const uint64_t method = show_method(window);
    return method && !memcmp((const void *)(uintptr_t)method, body, sizeof(body));
}

static void hide_shown(void) {
    if (!owner_shown) return;
    owner_shown = 0;
    if (window_of(shown_ui) == shown_window) functions.hide(shown_window);
}

void network_hint_configure(const NetworkHintFunctions *game) {
    hide_shown();
    failed = 0;
    if (game) functions = *game;
    else memset(&functions, 0, sizeof(functions));
}

void network_hint_set_portuguese(int portuguese) {
    portuguese_text = portuguese != 0;
}

int network_hint_shown_by(NetworkHintOwner owner) {
    return owner_shown == (int)owner;
}

void network_hint_show(NetworkHintOwner owner, uint64_t manager) {
    if (failed || (owner_shown && owner_shown != (int)owner)) return;
    if (!functions.create || !functions.text || !functions.hide || !functions.refresh) return;
    const uint64_t ui = guest_u64(manager + NETWORK_HINT_MANAGER_UI);
    if (!ui || (owner_shown && shown_ui == ui && window_of(ui) == shown_window)) return;
    const uint64_t window = functions.create(ui, 0);
    if (!window || (functions.verify_show && !show_is_the_games(window))) {
        failed = 1;
        return;
    }
    functions.text(window, NETWORK_HINT_MESSAGE, portuguese_text ? NETWORK_HINT_GLYPH NETWORK_HINT_TEXT_PT : NETWORK_HINT_GLYPH NETWORK_HINT_TEXT_EN);
    functions.refresh(window + NETWORK_HINT_TEXT_OBJECT);
    ((NetworkHintShowFn)(uintptr_t)show_method(window))(window, -1, 0);
    shown_window = window;
    shown_ui = ui;
    owner_shown = (int)owner;
}

void network_hint_hide_any(void) {
    hide_shown();
}

void network_hint_hide(NetworkHintOwner owner) {
    if (owner_shown == (int)owner) hide_shown();
}
