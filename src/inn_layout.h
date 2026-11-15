/* The Dragon's Haven Inn background for the seven-row Boss Battle menu. The shared menu layout (stage/Menu.mbs, base and
 * language copies) lays out the Inn menu's six row backings in every part that shows the menu. inn_layout_apply() points
 * a loaded layout at cloned tables in which those parts carry seven rows at the given centres; inn_layout_restore()
 * points it back at its own tables, which are never written. Any layout other than the known one is left untouched. */
#ifndef DCP_INN_LAYOUT_H
#define DCP_INN_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { INN_LAYOUT_ROWS = 7, INN_LAYOUT_VANILLA_ROWS = 6, INN_LAYOUT_PARTS = 16 };

typedef enum {
    INN_LAYOUT_OTHER = 0,     /* not the menu layout */
    INN_LAYOUT_APPLIED,
    INN_LAYOUT_UNSUPPORTED,   /* the menu layout, but not the known one; untouched */
    INN_LAYOUT_NO_MEMORY
} InnLayoutStatus;

/* The parts of a loaded layout that show the Inn menu, and the first of each part's six row layers. */
typedef struct {
    int count;
    uint16_t part[INN_LAYOUT_PARTS];
    uint16_t first_row_layer[INN_LAYOUT_PARTS];
    float rows[INN_LAYOUT_VANILLA_ROWS]; /* the vanilla row centres */
    char failure[64];                    /* why the scan failed */
} InnLayoutScan;

/* 1 when `layout` is the known menu layout; `scan` gets its Inn menu parts. */
int inn_layout_scan(const uint8_t *layout, InnLayoutScan *scan);

/* `row_scale` scales the rows' backings vertically. */
InnLayoutStatus inn_layout_apply(uint8_t *layout, const float centers[INN_LAYOUT_ROWS], float row_scale);
/* 1 when the layout was applied and now uses its own tables again. */
int inn_layout_restore(uint8_t *layout);
int inn_layout_applied(const uint8_t *layout);
/* The native Options confirmation lights custom row `row`: 4 for Options (the default), 3 for Boss Battle. */
void inn_layout_options_row(uint8_t *layout, int row);
/* A layer position the game passes for `part`, as the part's `instances` layers were made from this layout's tables. */
int inn_layout_position(const uint8_t *layout, int part, int position, int instances);
/* Forgets every clone, for layouts that no longer exist; the tests use it between layouts. */
void inn_layout_reset(void);

#ifdef __cplusplus
}
#endif

#endif
