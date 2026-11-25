// A synthetic game executable: instruction shapes planted from the production tables, wired to each other. It holds no
// game code beyond the shapes the port already checks.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Fx Fx;

Fx *fx_create(uint64_t size, uint64_t code_region);
Fx *fx_clone(const Fx *fx);
void fx_destroy(Fx *fx);
unsigned char *fx_image(Fx *fx);
uint64_t fx_size(const Fx *fx);
int fx_conflicts(const Fx *fx);
void fx_context(Fx *fx, const char *what);

void fx_put(Fx *fx, uint64_t at, const void *bytes, size_t length);
void fx_shape(Fx *fx, uint64_t at, const short *shape, size_t count);
uint64_t fx_alloc(Fx *fx, size_t length);
uint64_t fx_text(Fx *fx, const char *text);
/* A rel32 field at `field` of an instruction that ends at `end`: an existing target is kept and returned. */
uint64_t fx_rel(Fx *fx, uint64_t field, uint64_t end, size_t room);
void fx_rel_to(Fx *fx, uint64_t field, uint64_t end, uint64_t target);
uint64_t fx_call(Fx *fx, uint64_t at, size_t room);
void fx_call_to(Fx *fx, uint64_t at, uint64_t target);
void fx_jump_to(Fx *fx, uint64_t at, uint64_t target);
uint64_t fx_jump(Fx *fx, uint64_t at, size_t room);
/* A pointer slot: an existing pointer is kept and returned. */
uint64_t fx_pointer(Fx *fx, uint64_t slot, size_t room);
void fx_pointer_to(Fx *fx, uint64_t slot, uint64_t target);
/* Entry `index` of a table of offsets from the table. */
void fx_entry(Fx *fx, uint64_t table, int index, uint64_t target);

/* Moves everything up by `distance`: branches keep their targets, pointer slots follow. */
void fx_shift(Fx *fx, uint64_t distance);
/* Copies the planted run that holds `at` to a free place, its branches still reaching their targets; 0 when there is none. */
uint64_t fx_duplicate(Fx *fx, uint64_t at);
int fx_planted(const Fx *fx, uint64_t at);

#define FX_SHAPE(fx, at, shape) fx_shape(fx, at, shape, sizeof(shape) / sizeof(*(shape)))
#define FX_BYTES(fx, at, bytes) fx_put(fx, at, bytes, sizeof(bytes))

/* One per feature whose shapes are private to its source; `layout` is the key of the build whose table places them. */
void fixture_boss_battle(Fx *fx, const char *layout, uint64_t text_changed);
void fixture_room_leave(Fx *fx, uint64_t room_wish);
void fixture_item_attack_cap(Fx *fx, const char *layout);
void fixture_kraken(Fx *fx, const char *layout);
void fixture_skip_intro(Fx *fx, const char *layout);
void fixture_skip_offers(Fx *fx, const char *layout);
void fixture_gallery(Fx *fx, const char *layout);
void fixture_menu_buttons(Fx *fx, const char *layout);
void fixture_endgame(Fx *fx, const char *layout);
void fixture_system_type(Fx *fx, const char *label);
void fixture_exit_game(Fx *fx, uint64_t row_widget, uint64_t tick, const uint64_t table_loads[2]);
void fixture_ultrawide(Fx *fx, uint64_t queue, uint64_t model_draw, uint64_t worldmap_draw);
void fixture_touch(Fx *fx);
void fixture_appearance(Fx *fx);
void fixture_character_override(Fx *fx);
void fixture_default_messages(Fx *fx);
void fixture_comms(Fx *fx);

/* A second, complete copy of the searched code, at a free place. */
void fixture_skip_intro_again(Fx *fx);
void fixture_skip_offers_again(Fx *fx);
void fixture_menu_buttons_again(Fx *fx);

/* 1 when the source's own table lists the build, 0 when it only searches. */
int fixture_lists_item_attack_cap(const char *game);
int fixture_lists_kraken(const char *game);

#ifdef __cplusplus
}
#endif
