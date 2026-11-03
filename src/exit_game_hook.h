/* Exit Game on the title menu. The title builds its main menu with six items (the sixth is PlayStation Store) and then
 * cuts the list to five. Here the list keeps its sixth item and gets a sixth row widget built by the game's own
 * constructors, and the help text limit follows. The row shows SystemMsg record 388 rewritten by exit_game_text.h.
 * The game only draws the row: while it is selected the port owns the command. The title's tick publishes the selection,
 * the host's accept press (controller South or Enter) requests the exit through the registered handler and never
 * reaches the guest. Every hook checks its bytes first; an image that differs is left untouched. */
#ifndef DCP_EXIT_GAME_HOOK_H
#define DCP_EXIT_GAME_HOOK_H

#include <stddef.h>
#include <stdint.h>

#include "feature_sites.h"
#include "guest_hook.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_GAME_HOOK_COUNT 3
#define EXIT_GAME_ROW_INDEX 5
#define EXIT_GAME_MENU_ITEMS 6

/* The menu items' count and the help text limit hold the number of rows before Exit Game. */
#define EXIT_GAME_OLD_ROW_COUNT 5

/* Distances from the call that sizes the item list back to the calls that build the last of the five row widgets. */
#define EXIT_GAME_NEW_CALL_BACK 158
#define EXIT_GAME_CTOR_CALL_BACK 144
#define EXIT_GAME_ADD_CALL_BACK 104

/* The host's exit: schedules the process exit on the window thread; nonzero for the first request. */
typedef int (*ExitGameHandler)(void);
typedef uint64_t (*ExitGameClock)(void); /* milliseconds */

typedef void *(GUEST_HOOK_ABI *ExitGameNewFn)(uint64_t size);
typedef void (GUEST_HOOK_ABI *ExitGameCtorFn)(void *widget, void *argument);
typedef uint64_t (GUEST_HOOK_ABI *ExitGameAddFn)(void *menu, void *widget);

void exit_game_set_handler(ExitGameHandler handler);
void exit_game_set_clock(ExitGameClock clock);
void exit_game_set_row_functions(ExitGameNewFn new_fn, ExitGameCtorFn ctor, ExitGameAddFn add);
void exit_game_reset(void);

/* Whether the sixth row was reported selected within the last few frames. */
void exit_game_set_selected(int selected);
int exit_game_selected(void);

/* Calls the handler once; 0 when there is none or it was already called. */
int exit_game_invoke_handler(void);
int exit_game_requested(void);

/* Host input: `accept_down` is the port's accept (South, Enter). Returns 1 while the guest must not see the press. */
int exit_game_filter_accept(int accept_down);

/* Which controller button accepts: East (Circle, B) when set, else South (Cross, A). */
void exit_game_set_accept_east(int east);
int exit_game_accept_east(void);

void GUEST_HOOK_ABI exit_game_sheet_hook(GuestRegs *regs);
void GUEST_HOOK_ABI exit_game_row_hook(GuestRegs *regs);
void GUEST_HOOK_ABI exit_game_tick_hook(GuestRegs *regs);

size_t exit_game_hook_stub_bytes(void);

/* 1 when every site's bytes match and the hooks and the byte patches are installed; 0 otherwise, the image untouched
 * (the reason is printed). */
int exit_game_hook_install_located(uint8_t *image, uint64_t size, uint64_t entry, const DcpExitGameSites *sites,
                                   uint8_t *stubs, size_t stubs_size);

#ifdef __cplusplus
}
#endif

#endif
