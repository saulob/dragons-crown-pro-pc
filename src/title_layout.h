/* The four-row title menu's art. The game's title layout (stage/title.mbs, loaded once per language) draws six menu rows;
 * the port's menu has four. title_layout_patch() rewrites the loaded layout's part and layer tables so the parts the title
 * uses lay out four rows with the selected-row layers on the Options, Gallery and Exit Game rows. The layout is checked
 * against the known original (or already patched) bytes first; any other layout is left untouched. */
#ifndef DCP_TITLE_LAYOUT_H
#define DCP_TITLE_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TITLE_LAYOUT_NOT_TITLE = 0, /* some other layout */
    TITLE_LAYOUT_PATCHED,
    TITLE_LAYOUT_CURRENT,       /* already patched */
    TITLE_LAYOUT_UNSUPPORTED    /* looks like the title layout but differs from the known one; untouched */
} TitleLayoutStatus;

/* `layout` is the loaded layout object. */
TitleLayoutStatus title_layout_patch(uint8_t *layout);

#ifdef __cplusplus
}
#endif

#endif
