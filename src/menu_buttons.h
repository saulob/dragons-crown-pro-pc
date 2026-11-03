/* Japanese Menu Buttons: the game keeps one "Cross confirms" flag in its system settings object. The key setup reads it
 * to give Confirm and Cancel to Cross and Circle (or the reverse) and to pick the prompt pictures. The USA builds store
 * 1 in it when they read the system language; the Japanese and Asian builds never store it and keep their native 0, so
 * Circle confirms. With the setting on, the USA builds' two stores keep 0. Nothing else is touched. */
#ifndef DCP_MENU_BUTTONS_H
#define DCP_MENU_BUTTONS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MENU_BUTTONS_MAX_WRITES 2
#define MENU_BUTTONS_COMPARES 4

typedef struct {
    uint64_t reader;                          /* the key setup that reads the flag, 0 when not found */
    uint64_t writes[MENU_BUTTONS_MAX_WRITES]; /* the immediate of each store of 1 into the flag */
    int write_count;
    uint64_t compares[MENU_BUTTONS_COMPARES]; /* the immediates of the readers that test the flag against 0 */
    uint64_t load;                            /* the reader that copies the flag */
    int western_sites;                        /* all five readers were found */
} MenuButtonSites;

/* The saved choice (0/1) with DCP_JAPANESE_MENU_BUTTONS (0/1, off/on) taking precedence. */
int menu_buttons_wanted(int saved);

/* The sites of a build ("CUSA10454-1.00"; NULL: not a supported build), all zero when its code does not match. Returns 1
 * when the build is listed; any other build is searched and must match exactly once. */
int menu_buttons_locate(const unsigned char *image, uint64_t size, const char *game, MenuButtonSites *sites);

/* Puts the chosen layout in place before the game starts: 1 when it is in effect. The USA code starts Western and its
 * two stores of 1 are zeroed for Japanese; the Japanese and Asian code starts Japanese and its five readers are made
 * to see a set flag for Western. Nothing is written unless every site matches; a mismatch says why. */
int menu_buttons_install(unsigned char *image, uint64_t size, const char *game, int japanese);

#ifdef __cplusplus
}
#endif

#endif
