#include "boss_battle.h"
#include "exit_game_text.h"
#include "hydeland_town.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define ANY (-1)

enum {
    SCENE_ACT = 0x5b,
    SCENE_STEP = 0x5c,
    SCENE_STAGE = 0xf0,
    SCENE_NEXT_STAGE = 0xf8,
    SCENE_ROUND = 0x108,
    VTABLE_DISPATCH = 0x60,
    VTABLE_CONTEXT = 0xd0,
    CONTEXT_SERVER = 0x80,
    CONTEXT_SCENE = 0x88,
    LEAD_FACADE_MANAGER = 0x70,
    LEAD_FACADE_SIZE = 0x80,
    RECORD_LEVEL = 0x26,
    RECORD_CLASS = 0x27,
    RECORD_NAME = 0x138,
    RECORD_NAME_ALTERNATE = 0x140,
    RECORD_NAME_FLAGS = 0x21a8,
    RECORD_NAME_BYTES = 8,
    ROUND_RECORD = 0xd0,
    HOOK_LENGTH = 5
};

/* The scene manager's per-frame act handler: entry, the act switch, and its tail, which calls the menu handler. */
static const short kDispatcher[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x41, 0x89, 0xf6, 0x49, 0x89, 0xff,
    0x41, 0x80, 0xfe, 0x04,
};
static const short kActSwitch[] = {
    0x41, 0x80, 0xfe, 0x09, 0x0f, 0x87, ANY, ANY, ANY, ANY, 0x48, 0x8d, 0x0d, ANY, ANY, ANY, ANY, 0x48, 0x63, 0x04,
    0x81, 0x48, 0x01, 0xc8, 0xff, 0xe0,
};
static const short kActCase[] = {0x4c, 0x89, 0xff, 0xe8, ANY, ANY, ANY, ANY};
static const short kFrameTail[] = {
    0x41, 0x8a, 0x87, 0xdc, 0x94, 0x00, 0x00, 0x3c, 0xff, 0x74, 0x17, 0x49, 0x8b, 0x0f, 0x0f, 0xb6, 0xf0, 0x4c, 0x89, 0xff,
    0xff, 0x91, 0xa8, 0x00, 0x00, 0x00, 0x41, 0xc6, 0x87, 0xdc, 0x94, 0x00, 0x00, 0xff, 0x4c, 0x89, 0xff, 0xe8, ANY, ANY,
    ANY, ANY, 0x4c, 0x89, 0xff, 0x48, 0x83, 0xc4, 0x08, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xe9,
    ANY, ANY, ANY, ANY,
};
enum { kActSwitchAt = 0x185, kActTableLea = 0x18f, kHookInTail = 0x25 };

/* The stage-end act: the scene's context, whose server makes the scene requests, and the switch on its step byte. */
static const short kStageEnd[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xe4, 0xe0, 0x48, 0x81, 0xec,
    0x40, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x00, 0x48, 0x89, 0x84,
    0x24, 0x20, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x03, 0xff, 0x90, 0xd0, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc6, 0xbe, 0x0a, 0x00,
    0x00, 0x00, 0x49, 0x8b, 0xbe, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x89, 0x7c, 0x24, 0x40, 0xe8, ANY, ANY, ANY, ANY, 0x48,
    0x89, 0x44, 0x24, 0x60, 0x48, 0x89, 0xdf, 0x49, 0x8b, 0x86, 0xb8, 0x00, 0x00, 0x00, 0x4d, 0x8b, 0xae, 0x80, 0x00, 0x00,
    0x00, 0x4d, 0x8b, 0xbe, 0x90, 0x00, 0x00, 0x00,
};
static const short kStageEndSteps[] = {
    0x41, 0x0f, 0xb6, 0x46, 0x5c, 0x4c, 0x8b, 0x74, 0x24, 0x38, 0x48, 0x83, 0xf8, 0x19, 0x0f, 0x87, ANY, ANY, ANY, ANY,
    0x48, 0x8d, 0x0d, ANY, ANY, ANY, ANY, 0x48, 0x63, 0x04, 0x81, 0x48, 0x01, 0xc8, 0xff, 0xe0,
};
enum { kStepTableLea = 0x14, STAGE_END_STEPS = 26 };

/* The scene request builder (DC_GAMESERVERCMD_SETUPSCENE_REQUEST, message 0x5dc), whole. */
static const short kRequest[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0xe8, 0x00, 0x00, 0x00,
    0x48, 0x8b, 0x05, ANY, ANY, ANY, ANY, 0x4c, 0x89, 0x8d, 0xf0, 0xfe, 0xff, 0xff, 0x89, 0x8d, 0xf8, 0xfe, 0xff, 0xff,
    0x89, 0x95, 0xfc, 0xfe, 0xff, 0xff, 0x49, 0x89, 0xfc, 0x4c, 0x89, 0xc3, 0x49, 0x89, 0xf5, 0x48, 0x8b, 0x00, 0x48, 0x89,
    0x45, 0xd0, 0x49, 0x8b, 0x04, 0x24, 0xff, 0x90, 0xd0, 0x00, 0x00, 0x00, 0x4c, 0x8b, 0xb8, 0x88, 0x00, 0x00, 0x00, 0x4c,
    0x89, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x74, 0x0b, 0x41, 0x80, 0x7f, 0x5b, 0x05, 0x41, 0x0f, 0x94, 0xc6,
    0xeb, 0x03, 0x45, 0x31, 0xf6, 0x4d, 0x85, 0xed, 0x0f, 0x84, ANY, ANY, ANY, ANY, 0x48, 0x8d, 0xbd, 0x00, 0xff, 0xff,
    0xff, 0xbe, 0xdc, 0x05, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x8d, 0x05, ANY, ANY, ANY, ANY, 0xc5, 0xf9,
    0x76, 0xc0, 0x48, 0x8d, 0xbd, 0x38, 0xff, 0xff, 0xff, 0xba, 0x3f, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xee, 0x48, 0x83, 0xc0,
    0x10, 0x48, 0x89, 0x85, 0x00, 0xff, 0xff, 0xff, 0xc5, 0xfa, 0x7f, 0x85, 0x28, 0xff, 0xff, 0xff, 0xc5, 0xfc, 0x57, 0xc0,
    0xc6, 0x45, 0xcc, 0x00, 0xc6, 0x45, 0xcd, 0x00, 0xc6, 0x45, 0xcf, 0xff, 0xc7, 0x45, 0xc8, 0xff, 0xff, 0xff, 0xff, 0xc6,
    0x45, 0xce, 0x00, 0x48, 0xc7, 0x45, 0xc0, 0xff, 0xff, 0xff, 0xff, 0xc5, 0xfc, 0x11, 0x45, 0x98, 0xc5, 0xfc, 0x11, 0x85,
    0x78, 0xff, 0xff, 0xff, 0xc5, 0xfc, 0x11, 0x85, 0x58, 0xff, 0xff, 0xff, 0xc5, 0xfc, 0x11, 0x85, 0x38, 0xff, 0xff, 0xff,
    0x48, 0xc7, 0x45, 0xb8, 0x00, 0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x8b, 0x85, 0xfc, 0xfe, 0xff, 0xff, 0x8b,
    0x95, 0xf8, 0xfe, 0xff, 0xff, 0x8a, 0x4d, 0x10, 0x48, 0x85, 0xdb, 0x89, 0x45, 0xb8, 0x89, 0x55, 0xbc, 0x88, 0x4d, 0xcc,
    0x44, 0x88, 0x75, 0xcd, 0x74, 0x25, 0xc5, 0xfc, 0x57, 0xc0, 0x48, 0x8d, 0xbd, 0x78, 0xff, 0xff, 0xff, 0xba, 0x3f, 0x00,
    0x00, 0x00, 0x48, 0x89, 0xde, 0xc5, 0xfc, 0x11, 0x45, 0x98, 0xc5, 0xfc, 0x11, 0x85, 0x78, 0xff, 0xff, 0xff, 0xe8, ANY,
    ANY, ANY, ANY, 0x8b, 0x45, 0x18, 0x8b, 0x55, 0x20, 0x8a, 0x4d, 0x28, 0x88, 0x45, 0xcf, 0x48, 0x8b, 0x45, 0x30, 0x89,
    0x55, 0xc8, 0x88, 0x4d, 0xce, 0x48, 0x85, 0xc0, 0x74, 0x0c, 0xc5, 0xf8, 0x10, 0x00, 0xc5, 0xf8, 0x11, 0x85, 0x28, 0xff,
    0xff, 0xff, 0x41, 0x8b, 0x87, 0xf0, 0x00, 0x00, 0x00, 0x41, 0x8b, 0x8f, 0x08, 0x01, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0x89,
    0x4d, 0xc0, 0x89, 0x45, 0xc4, 0x49, 0x8b, 0x04, 0x24, 0xff, 0x90, 0x78, 0x01, 0x00, 0x00, 0x48, 0x89, 0xc7, 0xe8, ANY,
    ANY, ANY, ANY, 0x48, 0x8b, 0x8d, 0xf0, 0xfe, 0xff, 0xff, 0x48, 0x85, 0xc9, 0x74, 0x02, 0x8a, 0x01, 0x49, 0x8b, 0x1c,
    0x24, 0x4c, 0x8d, 0xb5, 0x00, 0xff, 0xff, 0xff, 0x0f, 0xb6, 0xc8, 0xba, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xe7, 0x4c,
    0x89, 0xf6, 0xff, 0x93, 0x60, 0x01, 0x00, 0x00, 0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x8b, 0x05, ANY,
    ANY, ANY, ANY, 0x48, 0x8b, 0x00, 0x48, 0x3b, 0x45, 0xd0, 0x75, 0x12, 0x48, 0x81, 0xc4, 0xe8, 0x00, 0x00, 0x00, 0x5b,
    0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e, 0x41, 0x5f, 0x5d, 0xc3,
};

/* The round table entry: index * 0xd0 into the table whose address the image holds; a record starts with its file. */
static const short kRoundEntry[] = {
    0x48, 0x63, 0xc7, 0x48, 0x69, 0xc0, 0xd0, 0x00, 0x00, 0x00, 0x48, 0x03, 0x05, ANY, ANY, ANY, ANY, 0xc3,
};
enum { kRoundSlotDisp = 13 };

/* The scene manager's context getter: the context is the scene's +0x90. */
static const short kContextGetter[] = {0x48, 0x8b, 0x87, 0x90, 0x00, 0x00, 0x00, 0xc3};

typedef struct {
    const char *game;
    uint64_t dispatcher, frame_tail, stage_end, stage_end_steps, request, round_entry, scene_vtable;
    uint64_t inn_vtable, inn_builder, inn_update, inn_built_block, inn_result_block, inn_help_block;
    uint64_t arrange_block, story_clear_block, labyrinth_select, dragon_block, title_case, title_vtable, list_vtable;
    uint64_t inn_constructor, inn_flush_block, save_copy_block, save_allocation, play_data_allocation;
    uint64_t leave_mode, leave_party_block, gauge_getter, wipe_block;
    uint64_t boss_clear_block, clear_screen_block, results_screen_block, town_return_block;
    uint64_t options_open_block, options_setup_title, options_setup_built, options_window_vtable, options_update_block;
    uint64_t options_result_block, options_body_vtable, row_frame;
    uint64_t results_window_vtable, set_level, level_pattern, write_stats;
} BuildSites;

/* The Inn menu builder's last row widget and its six items (SystemMsg 919-925 without 922), then the cut to six. */
static const short kInnBuilt[] = {
    0xbf, 0xc0, 0x05, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xc3, 0x4c, 0x89, 0xe6, 0x48, 0x89, 0xdf, 0xe8,
    ANY, ANY, ANY, ANY, 0x8b, 0x45, 0xc4, 0x4c, 0x89, 0xef, 0x48, 0x89, 0xde, 0x89, 0x83, 0x50, 0x03, 0x00, 0x00, 0x44,
    0x89, 0xbb, 0x54, 0x03, 0x00, 0x00, 0x49, 0xbf, 0x00, 0x00, 0x40, 0x3f, 0x00, 0x00, 0x40, 0x3f, 0x4c, 0x89, 0xbb, 0x20,
    0x03, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0xe8, ANY, ANY, ANY, ANY, 0xbe, 0x97, 0x03, 0x00, 0x00, 0x4c, 0x89,
    0xef, 0x48, 0x89, 0xc2, 0xe8, ANY, ANY, ANY, ANY, 0xe8, ANY, ANY, ANY, ANY, 0xbe, 0x98, 0x03, 0x00, 0x00, 0x4c,
    0x89, 0xef, 0x48, 0x89, 0xc2, 0xe8, ANY, ANY, ANY, ANY, 0xe8, ANY, ANY, ANY, ANY, 0xbe, 0x99, 0x03, 0x00, 0x00,
    0x4c, 0x89, 0xef, 0x48, 0x89, 0xc2, 0xe8, ANY, ANY, ANY, ANY, 0xe8, ANY, ANY, ANY, ANY, 0xbe, 0x9b, 0x03, 0x00,
    0x00, 0x4c, 0x89, 0xef, 0x48, 0x89, 0xc2, 0xe8, ANY, ANY, ANY, ANY, 0xe8, ANY, ANY, ANY, ANY, 0xbe, 0x9c, 0x03,
    0x00, 0x00, 0x4c, 0x89, 0xef, 0x48, 0x89, 0xc2, 0xe8, ANY, ANY, ANY, ANY, 0xe8, ANY, ANY, ANY, ANY, 0xbe, 0x9d,
    0x03, 0x00, 0x00, 0x4c, 0x89, 0xef, 0x48, 0x89, 0xc2, 0xe8, ANY, ANY, ANY, ANY, 0x49, 0x8d, 0xbe, 0x10, 0x48, 0x00,
    0x00, 0xbe, 0x06, 0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x41, 0x80, 0x8e, 0x07, 0x45, 0x00, 0x00, 0x10,
};
enum {
    kInnAllocateCall = 0x05,
    kInnWidgetCall = 0x13,
    kInnAddWidgetCall = 0x3f,
    kInnFirstItemCall = 0x54,
    kInnItemStride = 0x15,
    kInnBuiltHook = 0xd3,
    INN_BUILT_LENGTH = 8
};

/* The Inn menu frame: the list result read, then the switch over results -2..5. */
static const short kInnResult[] = {
    0x4d, 0x8d, 0xbd, 0xa0, 0x3b, 0x00, 0x00, 0x4c, 0x89, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x83, 0xc0, 0x02, 0x83, 0xf8,
    0x07, 0x0f, 0x87,
};
enum { kInnResultHook = 0x0a };

/* The list result the read returns, whole: the hook answers in its place. */
static const short kListResult[] = {
    0xf6, 0x87, 0x65, 0x09, 0x00, 0x00, 0x08, 0x74, 0x0e, 0x83, 0xbf, 0xb8, 0x00, 0x00, 0x00, 0x00, 0xb8, 0xff, 0xff, 0xff,
    0xff, 0x7f, 0x06, 0x8b, 0x87, 0x60, 0x09, 0x00, 0x00, 0xc3,
};

/* The Inn menu frame's help text: the cursor row's message from a six-entry table, none past it. */
static const short kInnHelp[] = {
    0x49, 0x63, 0x85, 0xf8, 0x44, 0x00, 0x00, 0x31, 0xf6, 0x48, 0x83, 0xf8, 0x05, 0x77, 0x0a, 0x48, 0x8d, 0x0d, ANY, ANY,
    ANY, ANY, 0x8b, 0x34, 0x81, 0xba, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xef,
};
enum { kInnHelpLea = 0x0f, kInnHelpHook = 0x19, INN_HOOK_LENGTH = 5 };

enum { INN_VTABLE_BUILD = 0xc88, INN_VTABLE_UPDATE = 0x590, INN_BUILDER_SPAN = 0x1000, INN_UPDATE_SPAN = 0x500 };

/* The placement group check: -1 always, the labyrinth by floor, then the story test (below step 45 only group 5),
 * else the difficulty (99 as 4). */
static const short kArrangeStory[] = {
    0x4c, 0x89, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x74, 0x13, 0x41, 0x8b, 0xbf, 0xa8, 0x00, 0x00, 0x00, 0xe8,
    ANY, ANY, ANY, ANY, 0x41, 0x39, 0xc5, 0x75, 0x2e, 0xeb, 0x2e, 0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY, ANY, 0x84,
    0xc0, 0x74, 0x08, 0x41, 0x83, 0xfd, 0x05, 0x75, 0x1a, 0xeb, 0x1a, 0x41, 0x8b, 0x84, 0x24, 0x84, 0x00, 0x00, 0x00, 0xb9,
    0x04, 0x00, 0x00, 0x00, 0x83, 0xf8, 0x63, 0x0f, 0x45, 0xc8, 0x41, 0x39, 0xcd, 0x74, 0x02, 0x31, 0xdb,
};
enum { kArrangeStoryHook = 0x22 };

/* The story test: the lead character's story step below 45. */
static const short kStoryTest[] = {
    0x55, 0x48, 0x89, 0xe5, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x85, 0xc0, 0x74, 0x0a, 0x48, 0x89, 0xc7, 0xe8, ANY, ANY,
    ANY, ANY, 0xeb, 0x02, 0x31, 0xc0, 0x5d, 0xc3,
};
static const short kStoryBelow45[] = {0x80, 0x7f, 0x6f, 0x2d, 0x0f, 0x92, 0xc0, 0xc3};
enum { kStoryTestAccessorCall = 0x11 };

/* The final boss's result: whether this clear advances the character's story tier, then the tier writer. */
static const short kStoryClear[] = {
    0x48, 0x8b, 0x45, 0xc0, 0x80, 0xb8, 0xcf, 0x01, 0x00, 0x00, 0x00, 0x74, 0x35, 0x8b, 0x5d, 0xb8, 0x4c, 0x89, 0xef, 0x89,
    0xde, 0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x74, 0x1c, 0x89, 0xd8, 0x4c, 0x89, 0xef, 0x89, 0xde, 0x83, 0xc8, 0x02,
    0x83, 0xf8, 0x02, 0x41, 0x0f, 0x95, 0x87, 0xa4, 0xb5, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY,
};
static const short kStoryTierApplies[] = {
    0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x89, 0xf3, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x85, 0xc0, 0x74, 0x0c, 0x48, 0x89,
    0xc7, 0x89, 0xde, 0xe8, ANY, ANY, ANY, ANY, 0xeb, 0x02, 0x31, 0xc0, 0x48, 0x83, 0xc4, 0x08, 0x5b, 0x5d, 0xc3,
};
enum { kStoryClearHook = 0x15 };

/* The Castle Gate's labyrinth floor choice: the scene fields the Demon King's floor is prepared in. */
static const short kLabyrinthSelect[] = {
    0x41, 0x89, 0xb6, 0xa8, 0x00, 0x00, 0x00, 0x41, 0x80, 0x7f, 0x38, 0x00, 0x0f, 0x84, ANY, ANY, ANY, ANY, 0x83, 0xf8,
    0x11, 0x0f, 0x85, ANY, ANY, ANY, ANY, 0x41, 0xc6, 0x86, 0xa4, 0x00, 0x00, 0x00, 0x01, 0x49, 0xc7, 0x86, 0xac, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0xc7, 0x86, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* The Inn calls that pass a main-list row: the row immediate, then the list call. */
typedef enum { INN_SHIFT, INN_MIRROR, INN_OPTIONS } InnCallKind;

typedef struct {
    uint32_t at; /* the `mov esi, row` */
    const char *bytes;
    unsigned length;
    InnCallKind kind;
} InnCall;

#define INN_CALL(at, bytes, kind) {at, bytes, sizeof(bytes) - 1, kind}

/* A row index the native Inn passes for Options (3), Return to title (4) or Leave tavern (5) moves down one row; a
 * mirror call also sets the Boss Battle row the same way, and the Options calls of the tutorial do too. */
static const InnCall kInnCalls[] = {
    INN_CALL(0x3a4016, "\xbe\x05\x00\x00\x00\xba\x01\x00\x00\x00\x4c\x89\xe7\xe8", INN_MIRROR),
    INN_CALL(0x3a4044, "\xbe\x05\x00\x00\x00\x41\x83\xfd\xff\x4c\x89\xff\x0f\x94\xc2\xe8", INN_MIRROR),
    INN_CALL(0x3a4c96, "\xbe\x05\x00\x00\x00\x48\x8b\x7d\xb8\x31\xd2\xb9\x01\x00\x00\x00\xe8", INN_SHIFT),
    INN_CALL(0x3a5824, "\xbe\x03\x00\x00\x00\xba\x01\x00\x00\x00\x4c\x89\xf7\xe8", INN_OPTIONS),
    INN_CALL(0x3a5836, "\xbe\x04\x00\x00\x00\xba\x01\x00\x00\x00\x4c\x89\xf7\xe8", INN_SHIFT),
    INN_CALL(0x3a5848, "\xbe\x05\x00\x00\x00\xba\x01\x00\x00\x00\x4c\x89\xf7\xe8", INN_SHIFT),
    INN_CALL(0x3a6235, "\xbe\x03\x00\x00\x00\x31\xd2\x4c\x89\xf7\xe8", INN_OPTIONS),
    INN_CALL(0x3a6244, "\xbe\x04\x00\x00\x00\x31\xd2\x4c\x89\xf7\xe8", INN_SHIFT),
    INN_CALL(0x3a6253, "\xbe\x05\x00\x00\x00\x31\xd2\x4c\x89\xf7\xe8", INN_SHIFT),
    INN_CALL(0x3a62c1, "\xbe\x05\x00\x00\x00\x4c\x89\xff\xe8", INN_SHIFT),
    INN_CALL(0x3a62d6, "\xbe\x05\x00\x00\x00\x31\xd2\xb9\x01\x00\x00\x00\x4c\x89\xff\xe8", INN_SHIFT),
};
enum { INN_CALLS = sizeof(kInnCalls) / sizeof(*kInnCalls), LIST_VTABLE_IS_DISABLED = 0xe8 };
_Static_assert(INN_CALLS == BOSS_BATTLE_INN_CALLS, "every Inn row call has a site");

/* The list's set-disabled (list, row, disabled) and set-cursor (list, row, 0, 1) entries. */
static const short kListSetDisabled[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x89, 0xf3, 0x41, 0x89, 0xd6};
static const short kListSetCursor[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x41, 0x89, 0xf6};

/* The Ancient Dragon's construction: the context, the difficulty, whether the round is the labyrinth, then the check
 * that the lead character holds the nine talismans of that difficulty, kept as whether hits can damage it. */
static const short kDragonTalismans[] = {
    0xe8, ANY, ANY, ANY, ANY, 0x49, 0x89, 0xc7, 0x49, 0x8b, 0xbf, 0x20, 0x01, 0x00, 0x00, 0x4d, 0x8b, 0xb7, 0xf0, 0x00,
    0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xdf, 0x41, 0x89, 0xc4, 0xe8, ANY, ANY, ANY, ANY, 0x89, 0xc1,
    0xb0, 0x01, 0x84, 0xc9, 0x75, 0x16, 0x4c, 0x89, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x0f, 0xb6, 0xd0, 0x4c, 0x89, 0xf7,
    0x44, 0x89, 0xe6, 0xe8, ANY, ANY, ANY, ANY, 0xc5, 0xf8, 0x10, 0x05, ANY, ANY, ANY, ANY, 0x88, 0x83, 0xc1, 0x38,
    0x00, 0x00,
};
static const short kTalismanCheck[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x41, 0x89, 0xf6, 0x49, 0x89, 0xff,
    0x41, 0xb5, 0x01, 0xbb, 0x01, 0x00, 0x00, 0x00, 0x44, 0x0f, 0xb6, 0xe2, 0x8d, 0x73, 0xff, 0x4c, 0x89, 0xff, 0x44, 0x89,
    0xf2, 0x44, 0x89, 0xe1, 0xe8,
};
enum { kTalismanCall = 0x3f };

/* The scene factory's title case: allocate the title scene, then construct it. */
static const short kTitleCase[] = {
    0xbf, 0x60, 0x56, 0x16, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xc3, 0x4c, 0x89, 0xfe, 0x48, 0x89, 0xdf, 0xe8,
};
static const short kTitleConstructor[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38, 0xba, 0x07, 0x00,
    0x00, 0x00, 0x48, 0x89, 0xfb, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x8d, 0x05,
};
enum { kTitleCall = 0x13, kTitleVtableLea = 0x1e };

/* The Inn constructor: its base constructor, then its vtable. */
static const short kInnConstructor[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xfb, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x8d,
    0x05, ANY, ANY, ANY, ANY,
};
enum { kInnConstructorVtableLea = 0x12, INN_CONSTRUCTOR_SPAN = 0x800 };

/* The last of the constructor's four moves of a party record back into the play data, then its next store. */
static const short kInnFlush[] = {
    0x49, 0x8d, 0xbf, 0xb8, 0x6b, 0x00, 0x00, 0xbe, 0x01, 0x00, 0x00, 0x00, 0x41, 0xc7, 0x87, 0xe4, 0x69, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x41, 0xc7, 0x87, 0x94, 0x8d, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x4d,
    0x8b, 0xbe, 0x90, 0x00, 0x00, 0x00,
};
enum { kInnFlushHook = 0x1c, INN_FLUSHED_LENGTH = 11 };

/* The save writer: the slots are normalised, then the whole save is copied for the write job. */
static const short kSaveCopy[] = {
    0xe8, ANY, ANY, ANY, ANY, 0x49, 0x8b, 0x44, 0x24, 0x20, 0x49, 0x8b, 0xbc, 0x24, 0xd0, 0x07, 0x00, 0x00, 0xba, 0x18,
    0xeb, 0x20, 0x00, 0x48, 0x8b, 0xb0, 0xe8, 0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY,
};
static const short kSaveCopyEntry[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x49, 0x89, 0xf7, 0x41, 0x89, 0xd6,
};
enum { kSaveCopyCall = 0x1e };

/* The save object's allocation (its size, kept at context +0xe8) and the play data's (its size, listed per user). */
static const short kSaveAllocation[] = {
    0xbf, 0x18, 0xeb, 0x20, 0x00, 0x4c, 0x89, 0xb3, 0xe0, 0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x49, 0x89, 0xc6,
    0x48, 0x89, 0xde, 0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY, ANY, 0xbf, 0x78, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xb3, 0xe8,
    0x00, 0x00, 0x00,
};
static const short kPlayDataAllocation[] = {
    0x41, 0x0f, 0xb6, 0x46, 0x74, 0x48, 0x8d, 0x0d, ANY, ANY, ANY, ANY, 0x88, 0x83, 0x15, 0xeb, 0x20, 0x00, 0x48, 0x83,
    0x3c, 0xc1, 0x00, 0x75, ANY, 0xbf, 0xd4, 0xf8, 0x0a, 0x00, 0xe8,
};
enum { kPlayDataTableLea = 0x05 };

/* The Inn menu's Options row (native row 3): the Options mode (0x8e), whose first frame opens the Options page. */
static const short kInnOptionsCase[] = {0x49, 0x8b, 0x45, 0x00, 0xbe, 0x8e, 0x00, 0x00, 0x00, 0xeb, 0x1f};
static const short kOptionsOpen[] = {
    0x4c, 0x89, 0xef, 0xe8, ANY, ANY, ANY, ANY, 0x49, 0x81, 0xc4, 0x0c, 0xf2, 0x0a, 0x00, 0x31, 0xf6, 0xba, 0x01, 0x00,
    0x00, 0x00, 0x48, 0x89, 0xdf, 0x48, 0x89, 0xc1, 0x4d, 0x89, 0xe0, 0xe8, ANY, ANY, ANY, ANY,
};
/* The page set up for the Inn (layout 1): its title widget with the Options message, then the first page's rows. */
static const short kOptionsTitle[] = {
    0xbf, 0xc0, 0x05, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x8b, 0x75, 0xd0, 0x49, 0x89, 0xc6, 0x4c, 0x89, 0xf7,
    0xe8, ANY, ANY, ANY, ANY, 0x48, 0xb8, 0x00, 0x05, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0xbe, 0x9b, 0x03, 0x00, 0x00,
    0x4c, 0x89, 0xf7, 0x49, 0x89, 0x86, 0x50, 0x03, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY,
};
static const short kOptionsBuilt[] = {
    0x4c, 0x89, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x8b, 0x43, 0x38, 0x4c, 0x89, 0xff, 0x89, 0x83, 0x38, 0x01, 0x00, 0x00,
};
/* The first page's builder: page 1, eight rows of the row class. */
static const short kPageBuilder[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x49, 0x89, 0xfe, 0x41, 0xc7, 0x86,
    0x50, 0x0c, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x8d, 0x05, ANY, ANY, ANY, ANY,
    0x45, 0x31, 0xff, 0x48, 0x83, 0xc0, 0x10,
};
/* The page window's act 2: the help line, the page's row logic, the list's input and result, then on a result other
 * than back the current row. */
static const short kOptionsAct2[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xfc, 0xe8, ANY, ANY, ANY, ANY, 0x4d,
    0x8d, 0x74, 0x24, 0x50, 0xbe, 0x01, 0x00, 0x00, 0x00, 0x48, 0x89, 0xc3, 0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY, ANY,
    0x84, 0xc0, 0x74, 0x09, 0x41, 0x80,
};
static const short kOptionsUpdate[] = {
    0x49, 0x8d, 0xbc, 0x24, 0xa0, 0x0f, 0x00, 0x00, 0xbe, 0xd0, 0x01, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89,
    0xdf, 0xe8, ANY, ANY, ANY, ANY, 0x41, 0x89, 0xc7, 0x48, 0x8b, 0x03, 0x48, 0x89, 0xdf, 0xff, 0x90, 0x78, 0x01, 0x00,
    0x00, 0x83, 0xf8, 0xfe, 0x0f, 0x85, ANY, ANY, ANY, ANY,
};
static const short kOptionsResult[] = {
    0x48, 0x89, 0xdf, 0xe8, ANY, ANY, ANY, ANY, 0x41, 0x8b, 0x8c, 0x24, 0x4c, 0x0d, 0x00, 0x00, 0x48, 0x63, 0x80, 0x64,
    0x03, 0x00, 0x00,
};
/* The page's row fill (the list's 0xb8) does nothing on a page past 3; the list's cursor moves (0x148 up, 0x150 down). */
static const short kPageFill[] = {0x8b, 0x87, 0x50, 0x0c, 0x00, 0x00, 0x48, 0x83, 0xf8, 0x03, 0x77, 0x1b};
static const short kListMove[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18, 0x49, 0x89, 0xfe,
    0x49, 0x8b, 0x06,
};
static const short kRowFrame[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0xc5, 0xfa, 0x11, 0x45, 0xf4, 0x48, 0x89, 0xfb};
enum {
    kInnResultTableLea = 0x1b,
    INN_OPTIONS_RESULT_ENTRY = BOSS_BATTLE_INN_OPTIONS_ROW + 2, /* the switch covers results -2..5 */
    INN_VTABLE_OPTIONS = 0x728,
    kOptionsOpenInMode = 0x120,
    kOptionsOpenCall = 0x1f,
    OPTIONS_SETUP_SPAN = 0x300,
    kOptionsTitleCall = 0x32,
    kOptionsBuilderCall = 0x03,
    kOptionsBuiltHook = 0x08,
    OPTIONS_BUILT_LENGTH = 6,
    OPTIONS_VTABLE_ACT = 0xe0,
    kOptionsAct2Call = 0x4e,
    OPTIONS_ACT2_SPAN = 0x560,
    kOptionsUpdateCall = 0x15,
    kOptionsBackJump = 0x2c,
    kOptionsResultCall = 0x03,
    LIST_VTABLE_FILL = 0xb8,
    LIST_VTABLE_UP = 0x148,
    LIST_VTABLE_DOWN = 0x150,
    LIST_MOVE_LENGTH = 6
};

/* The Inn's Leave mode (its vtable's 0x7d0), state 8: the party goes to the game server, then the Inn is left. */
static const short kLeaveParty[] = {
    0xbe, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xf7, 0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x0f, 0x84, ANY, ANY, ANY,
    ANY, 0x4c, 0x89, 0xff, 0xe8, ANY, ANY, ANY, ANY,
};
static const short kInnParty[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28, 0xe8, ANY, ANY,
    ANY, ANY, 0x48, 0x8b, 0xb8, 0xe8, 0x00, 0x00, 0x00,
};
/* Its last party member's entry request, then the three party requests that follow. */
static const short kInnPartyTail[] = {
    0x41, 0x0f, 0xb6, 0xcf, 0x4c, 0x8b, 0x7d, 0xb8, 0x4c, 0x89, 0xf6, 0x48, 0x89, 0xc2, 0x41, 0x89, 0xd8, 0x41, 0x89, 0xd9,
    0x6a, 0x00, 0x6a, 0xff, 0x6a, 0x00, 0x6a, 0x00, 0x6a, 0x00, 0x6a, 0x00, 0x6a, 0x01, 0xe8, ANY, ANY, ANY, ANY, 0x48,
    0x83, 0xc4, 0x40, 0xff, 0xc3, 0x83, 0xfb, 0x04, 0x0f, 0x85, ANY, ANY, ANY, ANY, 0x48, 0x8b, 0x5d, 0xd0, 0x48, 0x8b,
    0xbb, 0x80, 0x00, 0x00, 0x00, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x8b, 0xbb, 0x80, 0x00, 0x00, 0x00, 0xe8, ANY, ANY,
    ANY, ANY, 0x48, 0x8b, 0xbb, 0x80, 0x00, 0x00, 0x00, 0x48, 0x83, 0xc4, 0x28, 0x5b, 0x41, 0x5c, 0x41, 0x5d, 0x41, 0x5e,
    0x41, 0x5f, 0x5d, 0xe9, ANY, ANY, ANY, ANY,
};
enum { INN_VTABLE_LEAVE = 0x7d0, LEAVE_SPAN = 0x400, kLeavePartyCall = 0x18, kInnPartyTailAt = 0xf9 };

/* The boss HP gauge (UI window 0x22) getter: the window the probe reads. */
static const short kGaugeGetter[] = {
    0xbe, 0x22, 0x00, 0x00, 0x00, 0xe9, ANY, ANY, ANY, ANY, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x48, 0x8b, 0x87, 0xb8,
    0x04, 0x00, 0x00, 0xc3,
};

/* The common enemy handler, after a death: a unit whose placement ends the stage asks for the round end with its own
 * end kind. */
static const short kBossClear[] = {
    0x48, 0x8b, 0xbd, 0xa0, 0xfd, 0xff, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x74, 0x35, 0x49, 0x83, 0xbf, 0xe0,
    0x01, 0x00, 0x00, 0x00, 0x74, 0x2b, 0x49, 0x8b, 0x07, 0x4c, 0x89, 0xff, 0xff, 0x90, 0xd0, 0x00, 0x00, 0x00, 0x48, 0x8b,
    0x98, 0x80, 0x00, 0x00, 0x00, 0x49, 0x8b, 0x07, 0x4c, 0x89, 0xff, 0xff, 0x90, 0xa8, 0x08, 0x00, 0x00, 0x31, 0xd2, 0x48,
    0x89, 0xdf, 0x89, 0xc6, 0xe8, ANY, ANY, ANY, ANY,
};
static const short kStageEndingUnit[] = {
    0x83, 0xbf, 0x04, 0x2a, 0x00, 0x00, 0xff, 0x74, 0x24, 0x55, 0x48, 0x89, 0xe5, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x85,
    0xc0, 0x5d, 0x74, 0x15, 0x83, 0xb8, 0x20, 0x02, 0x00, 0x00, 0x23, 0x75, 0x0c, 0x8a, 0x80, 0x6f, 0x03, 0x00, 0x00, 0x24,
    0x08, 0xc0, 0xe8, 0x03, 0xc3, 0x31, 0xc0, 0xc3,
};
/* DC_GAMESERVERCMD_ROUNDEND_REQUEST (0x5f0). */
static const short kRoundEndRequest[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x25, ANY, ANY,
    ANY, ANY, 0x41, 0x89, 0xf7, 0x48, 0x89, 0xfb, 0x48, 0x8d, 0x7d, 0xa8, 0xbe, 0xf0, 0x05, 0x00, 0x00,
};
enum { kStageEndingUnitCall = 0x07, kBossClearCall = 0x40 };

/* Stage-end step 6 opens the stage clear screen (UI window 0xa); without one it goes on to step 7, which then waits for
 * the screen's answer. */
static const short kClearScreen[] = {
    0xbe, 0x0a, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xc3, 0x48, 0x85, 0xdb, 0x74,
    0x10, 0x41, 0x8b, 0xb4, 0x24, 0xe8, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0xe8, ANY, ANY, ANY, ANY, 0x4c, 0x89, 0xe7,
    0xe8, ANY, ANY, ANY, ANY,
};
/* Stage-end step 21 opens the results screen (UI window 0xc); without one it goes straight to step 23. */
static const short kResultsScreen[] = {
    0x48, 0x8b, 0x5c, 0x24, 0x40, 0xbe, 0x0c, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x8b,
    0x7c, 0x24, 0x18, 0xbe, 0x01, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc6, 0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x0f, 0x84,
    ANY, ANY, ANY, ANY, 0xbe, 0x0c, 0x00, 0x00, 0x00, 0x48, 0x89, 0xdf, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x85, 0xc0,
    0x0f, 0x85, ANY, ANY, ANY, ANY, 0x48, 0x8b, 0x7c, 0x24, 0x18, 0xbe, 0x01, 0x00, 0x00, 0x00, 0xba, 0x17, 0x00, 0x00,
    0x00, 0xe9, ANY, ANY, ANY, ANY,
};
static const short kWindowOpener[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x41, 0x89, 0xf6, 0x49, 0x89, 0xff, 0x41, 0x8d, 0x5e, 0xff,
    0x83, 0xfb, 0x42,
};
enum { kClearScreenStepOffset = 0x77, kClearScreenCall = 0x08, kResultsScreenCall = 0x34, STEP_CLEAR_SCREEN = 6,
       STEP_RESULTS_SCREEN = 21 };

/* The results screen's act table, and its act 2: the experience award and level-up panel of each player, then the
 * request for act 3 (General results, story, items). */
static const short kResultsActs[] = {
    0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x40, 0x80, 0xfe, 0x07, 0x77, 0x60, 0x48, 0x8d, 0x05, ANY, ANY,
    ANY, ANY, 0x48, 0x63, 0x0c, 0xb0, 0x48, 0x01, 0xc1, 0xff, 0xe1, 0x48, 0x89,
};
static const short kResultsAct2Case[] = {0x48, 0x89, 0xdf, 0xe8, ANY, ANY, ANY, ANY};
static const short kResultsAct2[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x48, 0x89, 0xfb, 0xbe, 0x01, 0x00,
    0x00, 0x00, 0x4c, 0x8d, 0x73, 0x50,
};
static const short kResultsLevelDone[] = {
    0x49, 0x8b, 0x06, 0xbe, 0x03, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xf7, 0x48, 0x8b, 0x80, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x83,
    0xc4, 0x08,
};
enum {
    RESULTS_VTABLE_ACT = 0xe0,
    kResultsActTableLea = 0x0f,
    RESULTS_ACT_LEVEL_UP = 2,
    kResultsLevelDoneInAct2 = 0x82,
    kResultsLevelDoneHook = 0x08,
    RESULTS_LEVEL_DONE_LENGTH = 10,
    RESULTS_ACT_CLOSING = 6 /* ends the window at once while no story ending is pending */
};

/* A character record's level setter, its stat pattern and the stat writer for a level, as the game's level-ups use. */
static const short kSetLevel[] = {0x40, 0x80, 0xfe, 0xff, 0x75, 0x06, 0x80, 0x7f, 0x27, 0xff, 0x75, 0x0a};
static const short kLevelPattern[] = {0x48, 0x0f, 0xbe, 0x47, 0x27, 0x48, 0x83, 0xf8, 0x05, 0x77, 0x0b, 0x48, 0x8d, 0x0d};
static const short kWriteStats[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x89, 0xcb, 0x49, 0x89, 0xff};

/* Stage-end step 25's request for Town00 by stage name, every end kind's way back, then the play act. */
static const short kTownReturn[] = {
    0x48, 0x83, 0xec, 0x08, 0x48, 0x8d, 0x35, ANY, ANY, ANY, ANY, 0xba, 0xff, 0xff, 0xff, 0xff, 0xb9, 0xff, 0xff, 0xff,
    0xff, 0x41, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xef, 0x41, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x6a, 0x00, 0x6a, 0x00,
    0x6a, 0xff, 0x6a, 0xff, 0x6a, 0x01, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x83, 0xc4, 0x30, 0x49, 0x8b, 0x06, 0xbe, 0x02,
    0x00, 0x00, 0x00, 0x4c, 0x89, 0xf7, 0xff, 0x90, 0xa8, 0x00, 0x00, 0x00,
};
enum { kTownReturnRoundLea = 0x04, kTownReturnCall = 0x2e };

/* The scene's act and step setter (level, value), its vtable's 0xa0. */
static const short kSetStep[] = {
    0x83, 0xfe, 0x03, 0x77, 0x20, 0x48, 0x63, 0xc6, 0x89, 0xf1, 0x88, 0x54, 0x07, 0x5b, 0xba, 0x00, 0x08, 0x00, 0x00,
};
enum { VTABLE_SET_STEP = 0xa0 };

/* The help line setter the Inn's frame ends in: the line's message is set by number, then the text is marked changed.
 * The message setter reaches the text's plain string setter through its by-number lookup. */
static const short kHelpSetter[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50, 0x41, 0x89, 0xd4, 0x41, 0x89, 0xf6,
    0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89, 0xc3, 0x4c, 0x8b, 0xab, 0xa8, 0x00, 0x00, 0x00,
};
static const short kHelpText[] = {
    0x49, 0x8d, 0x9f, 0xe0, 0x03, 0x00, 0x00, 0x44, 0x89, 0xf6, 0x48, 0x89, 0xdf, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x89,
    0xdf, 0xe8, ANY, ANY, ANY, ANY,
};
static const short kTextChanged[] = {0xc7, 0x87, 0x0c, 0x01, 0x00, 0x00, 0x00, 0xff, 0x7f, 0x47, 0xc3};
static const short kSetString[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38,
};
enum {
    kInnHelpTail = 0x2f,
    kHelpTextInSetter = 0x72,
    kHelpMessageCall = 0x0d,
    kHelpChangedCall = 0x15,
    kMessageByNumberJump = 0x119,
    kByNumberStringCall = 0x42
};

/* The all-down check: every occupied party slot's unit in the dead state, then the game over request (0x5f1). */
static const short kPartyWipe[] = {
    0x4c, 0x8d, 0xa8, 0xb8, 0x6b, 0x00, 0x00, 0x4c, 0x89, 0xef, 0xe8, ANY, ANY, ANY, ANY, 0x84, 0xc0, 0x74, 0x24, 0x4c,
    0x89, 0xef, 0xe8, ANY, ANY, ANY, ANY, 0x48, 0x85, 0xc0, 0x74, 0x17, 0x80, 0xb8, 0x03, 0x01, 0x00, 0x00, 0xd0, 0x74,
    0x07, 0xc7, 0x45, 0x84, 0x00, 0x00, 0x00, 0x00, 0x8b, 0x45, 0x94, 0xff, 0xc0, 0xeb, 0x03, 0x8b, 0x45, 0x94, 0x85, 0xc0,
    0x0f, 0x84, ANY, ANY, ANY, ANY, 0x8b, 0x45, 0x84, 0x24, 0x01, 0x0f, 0x84, ANY, ANY, ANY, ANY, 0x41, 0x8b, 0x04,
    0x24, 0x85, 0x83, 0x08, 0x69, 0x01, 0x00, 0x74, 0x1a, 0x49, 0x8b, 0x7f, 0x78, ANY, ANY, ANY, ANY, 0xff, ANY, ANY,
    ANY, ANY, 0xff, 0x45, 0x31, 0xed, 0x4c, 0x8d, 0x3d, 0x90, 0xa7, 0x32, 0x00, 0xeb, 0x7a, 0x31, 0xff, 0xe8, ANY, ANY,
    ANY, ANY, 0xb9, 0x01, 0x00, 0x00, 0x00, 0x83, 0xf8, 0x02, 0x7c, 0x09, 0x31, 0xff, 0xe8, ANY, ANY, ANY, ANY, 0x89,
    0xc1, 0xb8, 0x0a, 0x00, 0x00, 0x00, 0x31, 0xd2, 0xf7, 0xf9, 0x48, 0x8b, 0x7d, 0x88, 0xba, 0x01, 0x00, 0x00, 0x00, 0x89,
    0xc6, 0xe8, ANY, ANY, ANY, ANY,
};
static const short kGameOverRequest[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x40, 0x4c, 0x8b, 0x25, ANY, ANY,
    ANY, ANY, 0x41, 0x89, 0xf7, 0x48, 0x89, 0xfb, 0x48, 0x8d, 0x7d, 0xa8, 0xbe, 0xf1, 0x05, 0x00, 0x00,
};
enum { kPartyWipeCall = 0xa1 };

static const BuildSites kBuilds[] = {
    {"CUSA10454-1.00", 0x428690, 0x428923, 0x42ddc0, 0x42e330, 0x30c70, 0x4aa70, 0xb8cf30, 0xb177b0, 0x3a40d0, 0x3a60a0,
     0x3a442e, 0x3a6296, 0x3a6511, 0x4502e9, 0x4c4634, 0x435dab, 0x10da1d, 0x4af5d9, 0xbbbba0, 0xbad618, 0x3a2e20,
     0x3a3153, 0x8cc50, 0x7c8ce, 0x815a8, 0x3ac250, 0x3ac534, 0x4b0c30, 0x4294c3, 0xce7c6, 0x42e921, 0x42f644,
     0x4300c1, 0x3abf50, 0x507a5f, 0x507ae7, 0xbb84a8, 0x5074a2, 0x5075a2, 0xbb82b0, 0x578f40, 0xbb2720, 0x81fb0,
     0x81fe0, 0x82000},
};

static const struct {
    int index;
    const char *round;
} kTown = {BOSS_BATTLE_TOWN_ROUND, "round/Town00.brb"};

#define SHAPE(image, size, at, shape) shape_at(image, size, at, shape, sizeof(shape) / sizeof(*(shape)))

static int shape_at(const unsigned char *image, uint64_t size, uint64_t at, const short *shape, size_t count) {
    if (at > size || size - at < count) return 0;
    for (size_t i = 0; i < count; ++i)
        if (shape[i] != ANY && image[at + i] != (unsigned char)shape[i]) return 0;
    return 1;
}

static int64_t rel32_target(const unsigned char *image, uint64_t at, uint64_t end) {
    int32_t rel;
    memcpy(&rel, image + at, sizeof(rel));
    return (int64_t)end + rel;
}

static int read_u64_at(const unsigned char *image, uint64_t size, uint64_t at, uint64_t *value) {
    if (at > size || size - at < 8) return 0;
    memcpy(value, image + at, 8);
    return 1;
}

/* A pointer the image holds: a plain image offset before the loader relocates it, an address in the image after. */
static int image_pointer(const unsigned char *image, uint64_t size, uint64_t at, uint64_t *offset) {
    uint64_t value;
    if (!read_u64_at(image, size, at, &value)) return 0;
    const uint64_t base = (uint64_t)(uintptr_t)image;
    if (value >= base && value - base < size) value -= base;
    if (value >= size) return 0;
    *offset = value;
    return 1;
}

static int round_named(const unsigned char *image, uint64_t size, uint64_t table, int index, const char *round) {
    uint64_t name;
    const size_t length = strlen(round) + 1;
    return image_pointer(image, size, table + (uint64_t)index * ROUND_RECORD, &name) && size - name >= length &&
           !memcmp(image + name, round, length);
}

static const char *check_rounds(const unsigned char *image, uint64_t size, uint64_t round_entry) {
    if (!SHAPE(image, size, round_entry, kRoundEntry)) return "round table entry";
    const int64_t slot = rel32_target(image, round_entry + kRoundSlotDisp, round_entry + sizeof(kRoundEntry) / sizeof(*kRoundEntry) - 1);
    uint64_t table;
    if (slot < 0 || !image_pointer(image, size, (uint64_t)slot, &table)) return "round table address";
    if (!round_named(image, size, table, kTown.index, kTown.round)) return "round table: Hydeland";
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i) {
        const BossBattleEncounter *encounter = boss_battle_encounter(i);
        if (!round_named(image, size, table, encounter->round_index, encounter->round)) return "round table: an arena's round";
    }
    return NULL;
}

static const char *check_scene(const unsigned char *image, uint64_t size, const BuildSites *b) {
    if (!SHAPE(image, size, b->dispatcher, kDispatcher)) return "scene act handler";
    if (b->frame_tail <= b->dispatcher || !SHAPE(image, size, b->frame_tail, kFrameTail)) return "scene frame tail";
    if (!SHAPE(image, size, b->dispatcher + kActSwitchAt, kActSwitch)) return "scene act switch";
    const int64_t table = rel32_target(image, b->dispatcher + kActTableLea + 3, b->dispatcher + kActTableLea + 7);
    if (table < 0 || (uint64_t)table + 4 * (BOSS_BATTLE_ACT_STAGE_END + 1) > size) return "scene act table";
    int32_t entry;
    memcpy(&entry, image + table + 4 * BOSS_BATTLE_ACT_STAGE_END, sizeof(entry));
    const int64_t end_case = table + entry;
    if (end_case < 0 || !SHAPE(image, size, (uint64_t)end_case, kActCase) ||
        rel32_target(image, (uint64_t)end_case + 4, (uint64_t)end_case + 8) != (int64_t)b->stage_end)
        return "stage-end act";
    if (!SHAPE(image, size, b->stage_end, kStageEnd)) return "stage-end act entry";
    if (b->stage_end_steps <= b->stage_end || !SHAPE(image, size, b->stage_end_steps, kStageEndSteps))
        return "stage-end steps";
    uint64_t dispatch, context;
    if (!image_pointer(image, size, b->scene_vtable + VTABLE_DISPATCH, &dispatch) || dispatch != b->dispatcher)
        return "scene vtable";
    if (!image_pointer(image, size, b->scene_vtable + VTABLE_CONTEXT, &context) || !SHAPE(image, size, context, kContextGetter))
        return "scene context getter";
    return NULL;
}

static uint64_t call_target(const unsigned char *image, uint64_t at) {
    const int64_t target = rel32_target(image, at + 1, at + 5);
    return image[at] == 0xe8 && target > 0 ? (uint64_t)target : 0;
}

static int within(uint64_t at, uint64_t function, uint64_t span) {
    return at > function && at - function < span;
}

/* Every row call of the Inn reaches one of the list's three row entries, each the same for all its calls. */
static const char *check_inn_calls(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    uint64_t is_disabled;
    if (!image_pointer(image, size, b->list_vtable + LIST_VTABLE_IS_DISABLED, &is_disabled)) return "Inn list vtable";
    uint64_t set_disabled = 0, set_cursor = 0;
    for (int i = 0; i < INN_CALLS; ++i) {
        const InnCall *call = &kInnCalls[i];
        if (call->at > size || size - call->at < call->length + 4 || memcmp(image + call->at, call->bytes, call->length))
            return "Inn row call";
        const uint64_t at = call->at + call->length - 1, target = call_target(image, at);
        uint64_t *expected = target == is_disabled ? &is_disabled : SHAPE(image, size, target, kListSetCursor) ? &set_cursor
                                                                                                              : &set_disabled;
        if (!target || (*expected && *expected != target)) return "Inn row call target";
        *expected = target;
        sites->inn_calls[i] = at;
    }
    if (!SHAPE(image, size, set_disabled, kListSetDisabled) || !set_cursor) return "Inn list row functions";
    sites->set_disabled = set_disabled;
    sites->set_cursor = set_cursor;
    return NULL;
}

static const char *check_progression(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    uint64_t acts;
    if (!image_pointer(image, size, b->results_window_vtable + RESULTS_VTABLE_ACT, &acts) || !SHAPE(image, size, acts, kResultsActs))
        return "results screen acts";
    const int64_t table = rel32_target(image, acts + kResultsActTableLea + 3, acts + kResultsActTableLea + 7);
    if (table < 0 || (uint64_t)table + 4 * (RESULTS_ACT_LEVEL_UP + 1) > size) return "results screen act table";
    int32_t entry;
    memcpy(&entry, image + table + 4 * RESULTS_ACT_LEVEL_UP, sizeof(entry));
    if (table + entry < 0 || !SHAPE(image, size, (uint64_t)(table + entry), kResultsAct2Case)) return "results screen act 2";
    const uint64_t act2 = call_target(image, (uint64_t)(table + entry) + 3);
    if (!act2 || !SHAPE(image, size, act2, kResultsAct2) || !SHAPE(image, size, act2 + kResultsLevelDoneInAct2, kResultsLevelDone))
        return "results screen level-up end";
    if (!SHAPE(image, size, b->set_level, kSetLevel) || !SHAPE(image, size, b->level_pattern, kLevelPattern) ||
        !SHAPE(image, size, b->write_stats, kWriteStats))
        return "character level functions";
    sites->results_level_done = act2 + kResultsLevelDoneInAct2 + kResultsLevelDoneHook;
    sites->set_level = b->set_level;
    sites->level_pattern = b->level_pattern;
    sites->write_stats = b->write_stats;
    return NULL;
}

/* The Inn's Options row and mode, the page they open, and the page's frame and list, each reached from the one before. */
static const char *check_options(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    const int64_t table =
        rel32_target(image, b->inn_result_block + kInnResultTableLea + 3, b->inn_result_block + kInnResultTableLea + 7);
    if (table < 0 || (uint64_t)table + 4 * (INN_OPTIONS_RESULT_ENTRY + 1) > size) return "Inn result table";
    int32_t entry;
    memcpy(&entry, image + table + 4 * INN_OPTIONS_RESULT_ENTRY, sizeof(entry));
    if (table + entry < 0 || !SHAPE(image, size, (uint64_t)(table + entry), kInnOptionsCase)) return "Inn Options row";
    uint64_t mode, act, fill, up, down;
    if (!image_pointer(image, size, b->inn_vtable + INN_VTABLE_OPTIONS, &mode) ||
        b->options_open_block != mode + kOptionsOpenInMode || !SHAPE(image, size, b->options_open_block, kOptionsOpen))
        return "Inn Options mode";
    const uint64_t setup = call_target(image, b->options_open_block + kOptionsOpenCall);
    if (!setup || !within(b->options_setup_title, setup, OPTIONS_SETUP_SPAN) ||
        !within(b->options_setup_built, setup, OPTIONS_SETUP_SPAN) || !SHAPE(image, size, b->options_setup_title, kOptionsTitle) ||
        !SHAPE(image, size, b->options_setup_built, kOptionsBuilt))
        return "Options page setup";
    const uint64_t builder = call_target(image, b->options_setup_built + kOptionsBuilderCall);
    if (!builder || !SHAPE(image, size, builder, kPageBuilder)) return "Options first page";
    if (!image_pointer(image, size, b->options_window_vtable + OPTIONS_VTABLE_ACT, &act)) return "Options window";
    const uint64_t act2 = call_target(image, act + kOptionsAct2Call);
    if (!act2 || !SHAPE(image, size, act2, kOptionsAct2) || !within(b->options_update_block, act2, OPTIONS_ACT2_SPAN) ||
        !within(b->options_result_block, act2, OPTIONS_ACT2_SPAN) || !SHAPE(image, size, b->options_update_block, kOptionsUpdate) ||
        !SHAPE(image, size, b->options_result_block, kOptionsResult) ||
        rel32_target(image, b->options_update_block + kOptionsBackJump + 2, b->options_update_block + kOptionsBackJump + 6) !=
            (int64_t)b->options_result_block)
        return "Options page frame";
    if (!image_pointer(image, size, b->options_body_vtable + LIST_VTABLE_FILL, &fill) || !SHAPE(image, size, fill, kPageFill))
        return "Options row fill";
    if (!image_pointer(image, size, b->options_body_vtable + LIST_VTABLE_UP, &up) || !SHAPE(image, size, up, kListMove) ||
        !image_pointer(image, size, b->options_body_vtable + LIST_VTABLE_DOWN, &down) || !SHAPE(image, size, down, kListMove) ||
        up == down)
        return "list cursor moves";
    if (!SHAPE(image, size, b->row_frame, kRowFrame)) return "row frame setter";
    sites->options_title = b->options_setup_title + kOptionsTitleCall;
    sites->options_built = b->options_setup_built + kOptionsBuiltHook;
    sites->options_update = b->options_update_block + kOptionsUpdateCall;
    sites->options_result = b->options_result_block + kOptionsResultCall;
    sites->list_up = up;
    sites->list_down = down;
    sites->options_body_vtable = b->options_body_vtable;
    sites->row_frame = b->row_frame;
    return NULL;
}

/* The party the Inn sends on leaving, reached through its Leave mode. */
static const char *check_leave(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    uint64_t leave;
    if (!image_pointer(image, size, b->inn_vtable + INN_VTABLE_LEAVE, &leave) || leave != b->leave_mode)
        return "Inn Leave mode";
    if (!within(b->leave_party_block, leave, LEAVE_SPAN) || !SHAPE(image, size, b->leave_party_block, kLeaveParty))
        return "Inn Leave party step";
    const uint64_t party = call_target(image, b->leave_party_block + kLeavePartyCall);
    if (!party || !SHAPE(image, size, party, kInnParty) || !SHAPE(image, size, party + kInnPartyTailAt, kInnPartyTail))
        return "Inn party requests";
    sites->inn_party = party;
    return NULL;
}

static const char *check_outcomes(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    if (!SHAPE(image, size, b->gauge_getter, kGaugeGetter)) return "boss gauge window";
    if (!SHAPE(image, size, b->wipe_block, kPartyWipe)) return "party down check";
    const uint64_t game_over = call_target(image, b->wipe_block + kPartyWipeCall);
    if (!game_over || !SHAPE(image, size, game_over, kGameOverRequest)) return "game over request";
    if (!SHAPE(image, size, b->boss_clear_block, kBossClear)) return "boss clear";
    const uint64_t ending = call_target(image, b->boss_clear_block + kStageEndingUnitCall);
    const uint64_t round_end = call_target(image, b->boss_clear_block + kBossClearCall);
    if (!ending || !SHAPE(image, size, ending, kStageEndingUnit) || !round_end || !SHAPE(image, size, round_end, kRoundEndRequest))
        return "boss clear checks";
    sites->party_wiped = b->wipe_block + kPartyWipeCall;
    sites->boss_cleared = b->boss_clear_block + kBossClearCall;
    return NULL;
}

static int64_t stage_end_step(const unsigned char *image, uint64_t size, const BuildSites *b, int step) {
    const int64_t table =
        rel32_target(image, b->stage_end_steps + kStepTableLea + 3, b->stage_end_steps + kStepTableLea + 7);
    if (table < 0 || (uint64_t)table + 4 * STAGE_END_STEPS > size) return -1;
    int32_t entry;
    memcpy(&entry, image + table + 4 * step, sizeof(entry));
    return table + entry;
}

static uint64_t jump_target(const unsigned char *image, uint64_t at) {
    const int64_t target = rel32_target(image, at + 1, at + 5);
    return image[at] == 0xe9 && target > 0 ? (uint64_t)target : 0;
}

/* The stage-end act's screens and its way back to Town00, reached through its own step table. */
static const char *check_result_act(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    if (stage_end_step(image, size, b, STEP_CLEAR_SCREEN) + kClearScreenStepOffset != (int64_t)b->clear_screen_block ||
        !SHAPE(image, size, b->clear_screen_block, kClearScreen))
        return "stage clear screen";
    if (stage_end_step(image, size, b, STEP_RESULTS_SCREEN) != (int64_t)b->results_screen_block ||
        !SHAPE(image, size, b->results_screen_block, kResultsScreen))
        return "results screen";
    const uint64_t opener = call_target(image, b->clear_screen_block + kClearScreenCall);
    if (!opener || !SHAPE(image, size, opener, kWindowOpener) ||
        call_target(image, b->results_screen_block + kResultsScreenCall) != opener)
        return "UI window opener";
    if (!SHAPE(image, size, b->town_return_block, kTownReturn) ||
        call_target(image, b->town_return_block + kTownReturnCall) != b->request)
        return "Town00 return";
    const int64_t round = rel32_target(image, b->town_return_block + kTownReturnRoundLea + 3,
                                       b->town_return_block + kTownReturnRoundLea + 7);
    const size_t length = strlen(kTown.round) + 1;
    if (round < 0 || (uint64_t)round > size - length || memcmp(image + round, kTown.round, length))
        return "Town00 return round";
    uint64_t set_step;
    if (!image_pointer(image, size, b->scene_vtable + VTABLE_SET_STEP, &set_step) || !SHAPE(image, size, set_step, kSetStep))
        return "scene step setter";
    sites->clear_screen = b->clear_screen_block + kClearScreenCall;
    sites->results_screen = b->results_screen_block + kResultsScreenCall;
    sites->town_return = b->town_return_block + kTownReturnCall;
    sites->set_step = set_step;
    return NULL;
}

/* A text's message and plain string setters, as the Inn's help line reaches them. */
static const char *check_text(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    const uint64_t setter = jump_target(image, b->inn_help_block + kInnHelpTail);
    if (!setter || !SHAPE(image, size, setter, kHelpSetter)) return "help line setter";
    const uint64_t text = setter + kHelpTextInSetter;
    if (!SHAPE(image, size, text, kHelpText)) return "help line text";
    const uint64_t changed = call_target(image, text + kHelpChangedCall);
    if (!changed || !SHAPE(image, size, changed, kTextChanged)) return "help line change mark";
    const uint64_t message = call_target(image, text + kHelpMessageCall);
    const uint64_t by_number = message ? jump_target(image, message + kMessageByNumberJump) : 0;
    const uint64_t set_text = by_number ? call_target(image, by_number + kByNumberStringCall) : 0;
    if (!set_text || !SHAPE(image, size, set_text, kSetString)) return "text string setter";
    sites->set_text = set_text;
    sites->set_message = message;
    return NULL;
}

/* The Inn menu sites belong to the Inn's own builder and frame, both reached through its vtable. */
static const char *check_inn(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    uint64_t builder, update;
    if (!image_pointer(image, size, b->inn_vtable + INN_VTABLE_BUILD, &builder) || builder != b->inn_builder ||
        !image_pointer(image, size, b->inn_vtable + INN_VTABLE_UPDATE, &update) || update != b->inn_update)
        return "Inn vtable";
    if (!within(b->inn_built_block, builder, INN_BUILDER_SPAN) || !SHAPE(image, size, b->inn_built_block, kInnBuilt))
        return "Inn menu builder";
    if (!within(b->inn_result_block, update, INN_UPDATE_SPAN) || !SHAPE(image, size, b->inn_result_block, kInnResult))
        return "Inn menu result";
    if (!within(b->inn_help_block, update, INN_UPDATE_SPAN) || !SHAPE(image, size, b->inn_help_block, kInnHelp))
        return "Inn menu help";
    const uint64_t result = call_target(image, b->inn_result_block + kInnResultHook);
    if (!result || !SHAPE(image, size, result, kListResult)) return "Inn menu list result";
    const uint64_t add_item = call_target(image, b->inn_built_block + kInnFirstItemCall);
    for (int item = 1; item < 6; ++item)
        if (call_target(image, b->inn_built_block + kInnFirstItemCall + (uint64_t)item * kInnItemStride) != add_item)
            return "Inn menu items";
    const int64_t help_table = rel32_target(image, b->inn_help_block + kInnHelpLea + 3, b->inn_help_block + kInnHelpLea + 7);
    if (help_table < 0 || (uint64_t)help_table + 4 * (BOSS_BATTLE_INN_ROWS - 1) > size) return "Inn help table";
    sites->inn_built = b->inn_built_block + kInnBuiltHook;
    sites->inn_result = b->inn_result_block + kInnResultHook;
    sites->inn_help = b->inn_help_block + kInnHelpHook;
    sites->inn_help_table = (uint64_t)help_table;
    sites->allocate = call_target(image, b->inn_built_block + kInnAllocateCall);
    sites->widget = call_target(image, b->inn_built_block + kInnWidgetCall);
    sites->add_widget = call_target(image, b->inn_built_block + kInnAddWidgetCall);
    sites->add_item = add_item;
    if (!sites->allocate || !sites->widget || !sites->add_widget || !add_item) return "Inn menu functions";
    return check_inn_calls(image, size, b, sites);
}

static const char *check_special(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    if (!SHAPE(image, size, b->arrange_block, kArrangeStory)) return "placement group check";
    const uint64_t test = call_target(image, b->arrange_block + kArrangeStoryHook);
    if (!test || !SHAPE(image, size, test, kStoryTest)) return "story test";
    const uint64_t below = call_target(image, test + kStoryTestAccessorCall);
    if (!below || !SHAPE(image, size, below, kStoryBelow45)) return "story test accessor";
    if (!SHAPE(image, size, b->story_clear_block, kStoryClear)) return "final boss result";
    const uint64_t tier = call_target(image, b->story_clear_block + kStoryClearHook);
    if (!tier || !SHAPE(image, size, tier, kStoryTierApplies)) return "story tier check";
    if (!SHAPE(image, size, b->labyrinth_select, kLabyrinthSelect)) return "labyrinth floor choice";
    if (!SHAPE(image, size, b->dragon_block, kDragonTalismans)) return "Ancient Dragon construction";
    const uint64_t talismans = call_target(image, b->dragon_block + kTalismanCall);
    if (!talismans || !SHAPE(image, size, talismans, kTalismanCheck)) return "talisman check";
    sites->arrange_story = b->arrange_block + kArrangeStoryHook;
    sites->story_test = test;
    sites->story_clear = b->story_clear_block + kStoryClearHook;
    sites->dragon_talismans = b->dragon_block + kTalismanCall;
    return NULL;
}

static const char *check_title(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    if (!SHAPE(image, size, b->title_case, kTitleCase)) return "title scene creation";
    const uint64_t constructor = call_target(image, b->title_case + kTitleCall);
    if (!constructor || !SHAPE(image, size, constructor, kTitleConstructor) ||
        rel32_target(image, constructor + kTitleVtableLea + 3, constructor + kTitleVtableLea + 7) != (int64_t)b->title_vtable)
        return "title scene constructor";
    sites->title = b->title_case + kTitleCall;
    return NULL;
}

/* The campaign in memory: the save the writer copies, the play data the Inn commits into it, and the Inn constructor's
 * return of the party into the play data. */
static const char *check_campaign(const unsigned char *image, uint64_t size, const BuildSites *b, BossBattleSites *sites) {
    if (!SHAPE(image, size, b->inn_constructor, kInnConstructor) ||
        rel32_target(image, b->inn_constructor + kInnConstructorVtableLea + 3, b->inn_constructor + kInnConstructorVtableLea + 7) !=
            (int64_t)b->inn_vtable - 0x10)
        return "Inn constructor";
    if (!within(b->inn_flush_block, b->inn_constructor, INN_CONSTRUCTOR_SPAN) || !SHAPE(image, size, b->inn_flush_block, kInnFlush))
        return "Inn party return";
    if (!SHAPE(image, size, b->save_copy_block, kSaveCopy)) return "save writer";
    const uint64_t copy = call_target(image, b->save_copy_block + kSaveCopyCall);
    if (!copy || !SHAPE(image, size, copy, kSaveCopyEntry)) return "save copy";
    if (!SHAPE(image, size, b->save_allocation, kSaveAllocation)) return "save allocation";
    if (!SHAPE(image, size, b->play_data_allocation, kPlayDataAllocation)) return "play data allocation";
    const int64_t table =
        rel32_target(image, b->play_data_allocation + kPlayDataTableLea + 3, b->play_data_allocation + kPlayDataTableLea + 7);
    if (table < 0 || (uint64_t)table + 8 * 256 > size) return "play data table";
    sites->inn_flushed = b->inn_flush_block + kInnFlushHook;
    sites->save_copy = b->save_copy_block + kSaveCopyCall;
    sites->play_data_table = (uint64_t)table;
    return NULL;
}

static const BuildSites *listed_build(const char *game) {
    for (size_t i = 0; game && i < sizeof(kBuilds) / sizeof(*kBuilds); ++i)
        if (!strcmp(game, kBuilds[i].game)) return &kBuilds[i];
    return NULL;
}

int boss_battle_build_listed(const char *game) {
    return listed_build(game) != NULL;
}

int boss_battle_locate(const unsigned char *image, uint64_t size, const char *game, BossBattleSites *sites,
                       const char **why) {
    const BuildSites *b = listed_build(game);
    *why = "not a listed build";
    if (!b) return 0;
    if ((*why = check_scene(image, size, b))) return 0;
    if (!SHAPE(image, size, b->request, kRequest)) return *why = "scene request builder", 0;
    if ((*why = check_rounds(image, size, b->round_entry))) return 0;
    BossBattleSites found;
    memset(&found, 0, sizeof(found));
    found.hook = b->frame_tail + kHookInTail;
    found.request = b->request;
    found.scene_vtable = b->scene_vtable;
    if ((*why = check_inn(image, size, b, &found))) return 0;
    if ((*why = check_leave(image, size, b, &found))) return 0;
    if ((*why = check_special(image, size, b, &found))) return 0;
    if ((*why = check_title(image, size, b, &found))) return 0;
    if ((*why = check_campaign(image, size, b, &found))) return 0;
    if ((*why = check_outcomes(image, size, b, &found))) return 0;
    if ((*why = check_result_act(image, size, b, &found))) return 0;
    if ((*why = check_text(image, size, b, &found))) return 0;
    if ((*why = check_options(image, size, b, &found))) return 0;
    if ((*why = check_progression(image, size, b, &found))) return 0;
    *sites = found;
    return 1;
}

enum { FIXED_HOOKS = 22, HOOKS = FIXED_HOOKS + INN_CALLS };

size_t boss_battle_stub_bytes(void) {
    return HOOKS * guest_hook_stub_size();
}

typedef void(GUEST_HOOK_ABI *SceneRequestFn)(uint64_t server, const char *round, int stage, int entry,
                                             const char *stage_name, const uint8_t *route_flag, int force, int code,
                                             int slot, int option, const void *places);
typedef const uint8_t *(GUEST_HOOK_ABI *LeadSelectorFn)(uint64_t facade);

static _Atomic(SceneRequestFn) request_function;
static _Atomic(LeadSelectorFn) lead_selector;
static _Atomic uint64_t scene_vtable, inn_help_table;

enum {
    SCENE_CONTEXT = 0x90,
    SCENE_ENTRY = 0xf4,
    CONTEXT_PARTY = CONTEXT_SERVER,
    CONTEXT_UNITS = 0x90,
    CONTEXT_UI = 0xa8,
    PARTY_SLOTS = 0xa8,
    PARTY_SLOT_SIZE = 0x23b0,
    PARTY_SLOT_COUNT = 4,
    SLOT_UNIT = 0x21d8,
    SLOT_KIND = 0x21e0,
    SLOT_EMPTY = -1,
    SLOT_LOCAL = 0,
    UNITS_BY_SLOT = 0x120,
    UNITS_STRIDE = 0x20,
    UNIT_STATE = 0x103,
    UNIT_DEAD = 0xd0,
    UNIT_STATUS = 0x9b8,
    STATUS_HP = 0xb0,
    UI_BOSS_GAUGE = 0x4b8,
    GAUGE_BOSSES = 0xcb8,
    GAUGE_BOSS_COUNT = 0xcc0,
    GAUGE_NODE_BOSS = 0x10,
    UNIT_DOWN = 0xca,
    STATUS_LIVES = 0xce,
    SCENE_END_KIND = 0xe8,
    SCENE_CLEAR_CHOICE = 0x9314
};

static BossBattleScreenFunctions screen_functions;
static BossBattleLevelFunctions level_functions;

void boss_battle_set_level_functions(const BossBattleLevelFunctions *functions) {
    level_functions = functions ? *functions : (BossBattleLevelFunctions){0};
}

void boss_battle_set_screen_functions(const BossBattleScreenFunctions *functions) {
    screen_functions = functions ? *functions : (BossBattleScreenFunctions){0};
}

static uint64_t guest_u64(uint64_t address) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static int32_t guest_i32(uint64_t address) {
    int32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof(value));
    return value;
}

static uint8_t guest_u8(uint64_t address) {
    return *(const uint8_t *)(uintptr_t)address;
}

static int is_scene(uint64_t scene) {
    const uint64_t vtable = atomic_load(&scene_vtable);
    return scene && vtable && guest_u64(scene) == vtable;
}

static uint64_t scene_context(uint64_t scene) {
    const uint64_t context = guest_u64(scene + SCENE_CONTEXT);
    return context && guest_u64(context + CONTEXT_SCENE) == scene ? context : 0;
}

static uint64_t scene_server(uint64_t scene) {
    const uint64_t context = scene_context(scene);
    return context ? guest_u64(context + CONTEXT_SERVER) : 0;
}

static BossBattleScene scene_state(uint64_t scene) {
    return (BossBattleScene){guest_u8(scene + SCENE_ACT),        guest_u8(scene + SCENE_STEP),
                             guest_i32(scene + SCENE_ROUND),     guest_i32(scene + SCENE_STAGE),
                             guest_i32(scene + SCENE_NEXT_STAGE), guest_i32(scene + SCENE_ENTRY)};
}

static uint64_t party_slot(uint64_t context, int slot) {
    const uint64_t party = guest_u64(context + CONTEXT_PARTY);
    return party ? party + PARTY_SLOTS + (uint64_t)slot * PARTY_SLOT_SIZE : 0;
}

/* The slot's unit, as the game's own slot accessor finds it. */
static uint64_t slot_unit(uint64_t context, uint64_t slot) {
    const uint64_t units = guest_u64(context + CONTEXT_UNITS);
    const int32_t index = guest_i32(slot + SLOT_UNIT);
    if (!units || index < 0 || index >= PARTY_SLOT_COUNT) return 0;
    return guest_u64(units + UNITS_BY_SLOT + (uint64_t)index * UNITS_STRIDE);
}

static int unit_hp(uint64_t unit) {
    const uint64_t status = guest_u64(unit + UNIT_STATUS);
    return status ? guest_i32(status + STATUS_HP) : 1;
}

static uint64_t boss_gauge(uint64_t context) {
    const uint64_t ui = guest_u64(context + CONTEXT_UI);
    return ui ? guest_u64(ui + UI_BOSS_GAUGE) : 0;
}

/* Units and the gauge are read only in a set-up stage while the fight starts or runs, never while the game tears it
 * down. */
static int probe_objects(const BossBattleScene *scene) {
    const BossBattleState now = boss_battle_state();
    return scene->next_stage == -1 &&
           (now == BOSS_BATTLE_TRAVELLING || now == BOSS_BATTLE_INITIALIZING || now == BOSS_BATTLE_FIGHTING);
}

static BossBattleProbe probe_scene(uint64_t scene) {
    BossBattleProbe probe;
    memset(&probe, 0, sizeof(probe));
    probe.scene = scene_state(scene);
    probe.end_kind = guest_i32(scene + SCENE_END_KIND);
    probe.clear_choice = guest_i32(scene + SCENE_CLEAR_CHOICE);
    const uint64_t context = probe_objects(&probe.scene) ? scene_context(scene) : 0;
    if (!context) return probe;
    for (int i = 0; i < PARTY_SLOT_COUNT; ++i) {
        const uint64_t slot = party_slot(context, i);
        if (!slot) break;
        const int32_t kind = guest_i32(slot + SLOT_KIND);
        if (kind == SLOT_EMPTY) continue;
        const uint64_t unit = slot_unit(context, slot);
        ++probe.party_slots;
        probe.party_units += unit != 0;
        if (i != 0) continue;
        probe.player_record = kind == SLOT_LOCAL;
        probe.player_unit = unit != 0;
        probe.player_unit_address = unit;
        probe.player_dead = unit && guest_u8(unit + UNIT_STATE) == UNIT_DEAD;
        probe.player_down = unit && guest_u8(unit + UNIT_STATE) == UNIT_DOWN;
        const uint64_t status = unit ? guest_u64(unit + UNIT_STATUS) : 0;
        probe.player_lives = status ? (int8_t)guest_u8(status + STATUS_LIVES) : 0;
    }
    const uint64_t gauge = boss_gauge(context);
    if (gauge) {
        probe.bosses = (int)guest_u64(gauge + GAUGE_BOSS_COUNT);
        const uint64_t head = guest_u64(gauge + GAUGE_BOSSES), node = head ? guest_u64(head) : 0;
        if (node && node != head) {
            probe.boss_address = guest_u64(node + GAUGE_NODE_BOSS);
            if (probe.boss_address) probe.boss_hp = unit_hp(probe.boss_address);
        }
    }
    return probe;
}

static void log_party(const char *when, uint64_t context) {
    char text[160];
    size_t used = 0;
    for (int i = 0; i < PARTY_SLOT_COUNT && context; ++i) {
        const uint64_t slot = party_slot(context, i);
        if (!slot) break;
        const uint64_t unit = slot_unit(context, slot);
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%d:%d/%#llx", i ? " " : "",
                                 i, guest_i32(slot + SLOT_KIND), (unsigned long long)unit);
        if (used >= sizeof(text)) break;
    }
    printf("Boss Battle [%u]: party slots %s: %s\n", boss_battle_session_id(), when, context ? text : "no context");
}

static int printable_name(const char *name) {
    if (!*name) return 0;
    for (const unsigned char *c = (const unsigned char *)name; *c; ++c)
        if (*c < 0x20 || *c == 0x7f) return 0;
    return 1;
}

/* The game's own lead character selector reads only the facade's game manager; an empty session slot leads it to the
 * character loaded from the save list, which is where a single player's character is. */
static uint64_t lead_record(uint64_t manager) {
    const LeadSelectorFn selector = atomic_load(&lead_selector);
    if (!selector || !manager) return 0;
    _Alignas(16) uint8_t facade[LEAD_FACADE_SIZE] = {0};
    memcpy(facade + LEAD_FACADE_MANAGER, &manager, sizeof(manager));
    return (uint64_t)(uintptr_t)selector((uint64_t)(uintptr_t)facade);
}

static void read_character(uint64_t manager) {
    static const char *const kClasses[] = {"Fighter", "Amazon", "Elf", "Dwarf", "Sorceress", "Wizard"};
    const uint64_t record = lead_record(manager);
    if (!record) {
        boss_battle_set_character("Player 1", "Adventurer", 0);
        return;
    }
    const unsigned class_index = guest_u8(record + RECORD_CLASS);
    const int alternate = (guest_u8(record + RECORD_NAME_FLAGS) & 0x80) != 0;
    char name[RECORD_NAME_BYTES + 1];
    memcpy(name, (const void *)(uintptr_t)(record + (alternate ? RECORD_NAME_ALTERNATE : RECORD_NAME)), RECORD_NAME_BYTES);
    name[RECORD_NAME_BYTES] = 0;
    if (!printable_name(name)) snprintf(name, sizeof(name), "Player 1");
    /* The record keeps the level from 0. */
    boss_battle_set_character(name, class_index < sizeof(kClasses) / sizeof(*kClasses) ? kClasses[class_index] : "Adventurer",
                              guest_u8(record + RECORD_LEVEL) + 1);
}

enum {
    SCENE_LABYRINTH_RUN = 0xa4,
    SCENE_LABYRINTH_FLOOR = 0xa8,
    SCENE_LABYRINTH_SEGMENTS = 0xac, /* the floor's segments so far and the current one, 2 x 32 bits */
    SCENE_LABYRINTH_STAGES = 0xb4,
    CONTEXT_GAME_SCENE = 0x88,
    LOST_ZONE_ROUND = 10
};

/* As the game's own round change does: forced, no sender. The stage goes by index and by name. */
static void request_scene(uint64_t server, const char *round, int stage, int entry, const char *stage_name) {
    atomic_load(&request_function)(server, round, stage, entry, stage_name, NULL, 1, 0xff, -1, 0, NULL);
}

static int request_inn(uint64_t scene) {
    const uint64_t server = scene_server(scene);
    if (!server) return 0;
    request_scene(server, boss_battle_inn_entry.round, boss_battle_inn_entry.stage, boss_battle_inn_entry.entry,
                  boss_battle_inn_entry.stage_name);
    return 1;
}

/* As the screen's own return-to-town answer does, then the step after it; the act runs it from the next frame. */
static void supply_return_choice(uint64_t scene) {
    if (!screen_functions.set_step) return;
    const int32_t choice = BOSS_BATTLE_CHOICE_RETURN;
    memcpy((void *)(uintptr_t)(scene + SCENE_CLEAR_CHOICE), &choice, sizeof(choice));
    screen_functions.set_step(scene, 1, BOSS_BATTLE_STEP_AFTER_CHOICE);
}

/* At the scene frame's call into the menu handler: r15 is the scene manager. */
void GUEST_HOOK_ABI boss_battle_scene_hook(GuestRegs *regs) {
    const uint64_t scene = regs->r15;
    if (!boss_battle_session_active() || !is_scene(scene)) return;
    const BossBattleProbe probe = probe_scene(scene);
    const unsigned before = boss_battle_milestones();
    const BossBattleFrameAction action = boss_battle_frame(&probe);
    const unsigned reached = boss_battle_milestones() & ~before;
    if (reached & BOSS_BATTLE_ARENA_LOADED) log_party("at arena load", scene_context(scene));
    if (reached & BOSS_BATTLE_PLAYABLE) log_party("at Playable", scene_context(scene));
    if (action == BOSS_BATTLE_FRAME_REQUEST_INN && request_inn(scene)) boss_battle_inn_requested();
    if (action == BOSS_BATTLE_FRAME_ANSWER_CHOICE) supply_return_choice(scene);
}

/* The labyrinth's own floor choice writes these before its round request; the request carries them into the round. */
static void prepare_labyrinth_floor(uint64_t scene, int floor) {
    const uint8_t run = 1;
    const uint64_t segments = 0;
    const int32_t stages = 0, floor_index = floor;
    memcpy((void *)(uintptr_t)(scene + SCENE_LABYRINTH_RUN), &run, sizeof(run));
    memcpy((void *)(uintptr_t)(scene + SCENE_LABYRINTH_FLOOR), &floor_index, sizeof(floor_index));
    memcpy((void *)(uintptr_t)(scene + SCENE_LABYRINTH_SEGMENTS), &segments, sizeof(segments));
    memcpy((void *)(uintptr_t)(scene + SCENE_LABYRINTH_STAGES), &stages, sizeof(stages));
    printf("Boss Battle: labyrinth floor %d prepared\n", floor + 1);
}

enum {
    INN_LIST = 0x3ba0,
    LIST_WIDGETS_BEGIN = 0x68,
    LIST_WIDGETS_END = 0x70,
    LIST_CURSOR = 0x958,
    LIST_RESULT = 0x960,
    LIST_FLAGS = 0x964,
    LIST_RESULT_LATCH = 0x800,
    LIST_PENDING = 0xb8,
    LIST_ITEMS_BEGIN = 0xc78,
    LIST_ITEMS_END = 0xc80,
    LIST_ITEM_SIZE = 0x30,
    WIDGET_SIZE = 0x5c0,
    WIDGET_EXTENT = 0x350,
    WIDGET_SCALE = 0x320
};

static BossBattleInnFunctions inn_functions;

void boss_battle_set_inn_help_table(uint64_t table) {
    atomic_store(&inn_help_table, table);
}

void boss_battle_set_inn_functions(const BossBattleInnFunctions *functions) {
    inn_functions = functions ? *functions : (BossBattleInnFunctions){0};
}

static uint64_t list_items(uint64_t list) {
    return (guest_u64(list + LIST_ITEMS_END) - guest_u64(list + LIST_ITEMS_BEGIN)) / LIST_ITEM_SIZE;
}

static uint64_t list_widgets(uint64_t list) {
    return (guest_u64(list + LIST_WIDGETS_END) - guest_u64(list + LIST_WIDGETS_BEGIN)) / sizeof(uint64_t);
}

static int list_result(uint64_t list) {
    if ((guest_i32(list + LIST_FLAGS) & LIST_RESULT_LATCH) && guest_i32(list + LIST_PENDING) > 0) return -1;
    return guest_i32(list + LIST_RESULT);
}

/* Only the Inn's own menu ever holds the seventh row, so every translation is gated on it. */
static int has_boss_row(uint64_t list) {
    return list && list_items(list) == BOSS_BATTLE_INN_ROWS;
}

/* The appended Boss Battle item moves up to its row; items are plain 0x30-byte records without pointers to themselves. */
static void move_boss_item_up(uint64_t list) {
    uint8_t *items = (uint8_t *)(uintptr_t)guest_u64(list + LIST_ITEMS_BEGIN);
    uint8_t boss[LIST_ITEM_SIZE];
    memcpy(boss, items + (BOSS_BATTLE_INN_ROWS - 1) * LIST_ITEM_SIZE, LIST_ITEM_SIZE);
    memmove(items + (BOSS_BATTLE_INN_ROW + 1) * LIST_ITEM_SIZE, items + BOSS_BATTLE_INN_ROW * LIST_ITEM_SIZE,
            (BOSS_BATTLE_INN_ROWS - 1 - BOSS_BATTLE_INN_ROW) * LIST_ITEM_SIZE);
    memcpy(items + BOSS_BATTLE_INN_ROW * LIST_ITEM_SIZE, boss, LIST_ITEM_SIZE);
}

static void add_boss_row(uint64_t list) {
    inn_functions.add_item(list, BOSS_BATTLE_INN_ROW_RECORD, 0);
    if (list_items(list) == BOSS_BATTLE_INN_ROWS) move_boss_item_up(list);
}

/* After the Inn menu builder cut its item list to six (r14 the Inn): a seventh widget like the sixth and the Boss Battle
 * item, after Report adventurers. */
void GUEST_HOOK_ABI boss_battle_inn_built_hook(GuestRegs *regs) {
    if (!boss_battle_enabled() || !boss_battle_available()) return;
    const uint64_t inn = regs->r14, list = inn + INN_LIST;
    if (list_items(list) != BOSS_BATTLE_INN_ROWS - 1 || list_widgets(list) != BOSS_BATTLE_INN_ROWS - 1) {
        puts("Boss Battle: the Inn menu does not have its six rows; no Boss Battle row");
        return;
    }
    const uint64_t last = guest_u64(guest_u64(list + LIST_WIDGETS_END) - sizeof(uint64_t));
    const uint64_t widget = inn_functions.allocate(WIDGET_SIZE);
    if (!widget) return;
    inn_functions.widget(widget, hydeland_town_manager(inn));
    memcpy((void *)(uintptr_t)(widget + WIDGET_EXTENT), (const void *)(uintptr_t)(last + WIDGET_EXTENT), 8);
    memcpy((void *)(uintptr_t)(widget + WIDGET_SCALE), (const void *)(uintptr_t)(last + WIDGET_SCALE), 8);
    inn_functions.add_widget(list, widget);
    add_boss_row(list);
    if (list_items(list) != BOSS_BATTLE_INN_ROWS) {
        puts("Boss Battle: the Inn menu did not take the Boss Battle row");
        return;
    }
    puts("Boss Battle: Inn row available");
}

enum { RECORD_XP_POOL = 0x04, RECORD_XP_PROGRESS = 0x08 };

static uint64_t session_record;

static BossBattleProgress record_progress(uint64_t record) {
    return (BossBattleProgress){guest_u8(record + RECORD_LEVEL), guest_i32(record + RECORD_XP_POOL),
                                guest_i32(record + RECORD_XP_PROGRESS)};
}

/* As the game's own level-up does on a record: experience, level, then the stats of that level. */
static void apply_progress(uint64_t record, const BossBattleProgress *progress) {
    memcpy((void *)(uintptr_t)(record + RECORD_XP_POOL), &progress->xp_pool, sizeof(int32_t));
    memcpy((void *)(uintptr_t)(record + RECORD_XP_PROGRESS), &progress->xp_progress, sizeof(int32_t));
    level_functions.set_level(record, (uint32_t)progress->level);
    level_functions.write_stats(record, level_functions.level_pattern(record), 0, progress->level, 0);
}

/* After the snapshot and before the party is sent: the fight is the Boss Battle character's. */
static void begin_progression(uint64_t record) {
    BossBattleProgress effective;
    session_record = record;
    if (!record || !level_functions.set_level) return;
    const BossBattleProgress campaign = record_progress(record);
    if (boss_battle_progress_begin(&campaign, &effective)) apply_progress(record, &effective);
}

static void send_request(int index, uint64_t inn, uint64_t context) {
    const uint64_t scene = context ? guest_u64(context + HYDELAND_MANAGER_SCENE) : 0;
    const uint64_t server = is_scene(scene) ? scene_server(scene) : 0;
    if (!server) {
        boss_battle_request_failed(index, "the scene has no game server");
        return;
    }
    const BossBattleEncounter *encounter = boss_battle_encounter(index);
    const BossBattleScene from = scene_state(scene);
    log_party("before the party request", context);
    inn_functions.inn_party(inn);
    if (encounter->labyrinth_floor >= 0) prepare_labyrinth_floor(scene, encounter->labyrinth_floor);
    request_scene(server, encounter->round, encounter->stage, -1, encounter->stage_name);
    boss_battle_request_sent(index, &from, 1);
}

static void start_boss(uint64_t inn, uint64_t context, int index) {
    if (boss_battle_request(index) != BOSS_BATTLE_QUEUED) return;
    const uint64_t lead_before = lead_record(context), play_before = boss_battle_campaign_play_data();
    if (!boss_battle_begin_queued(&index)) return;
    const uint64_t lead_after = lead_record(context), play_after = boss_battle_campaign_play_data();
    printf("Boss Battle [%u]: lead record %#llx -> %#llx, play data %#llx -> %#llx (%s)\n", boss_battle_session_id(),
           (unsigned long long)lead_before, (unsigned long long)lead_after, (unsigned long long)play_before,
           (unsigned long long)play_after, lead_before == lead_after && play_before == play_after ? "unchanged" : "CHANGED");
    begin_progression(lead_after);
    send_request(index, inn, context);
}

static void no_choice(GuestRegs *regs) {
    regs->rax = 0xffffffffu;
    regs->skip = 1;
}

/* The native Options row opens the Inn's Options page, which becomes the Boss Battle screen. */
static void open_screen(GuestRegs *regs, uint64_t context) {
    read_character(context);
    boss_battle_screen_request();
    regs->rax = BOSS_BATTLE_INN_OPTIONS_ROW;
    regs->skip = 1;
}

/* At the Inn menu frame's read of the list result (r13 the Inn, rdi its list): the native switch sees native rows. The
 * menu frame runs again once the screen closed; a boss chosen there starts here, as from the Inn itself. */
void GUEST_HOOK_ABI boss_battle_inn_result_hook(GuestRegs *regs) {
    const uint64_t inn = regs->r13, list = regs->rdi, context = hydeland_town_manager(inn);
    boss_battle_campaign_use(context);
    if (boss_battle_inn_frame() == BOSS_BATTLE_INN_HOLD || boss_battle_screen_waiting()) {
        no_choice(regs);
        return;
    }
    int index = -1;
    if (boss_battle_screen_shown()) {
        if (boss_battle_screen_closed(&index)) start_boss(inn, context, index);
        no_choice(regs);
        return;
    }
    if (!has_boss_row(list)) return;
    if (boss_battle_take_menu_reopen()) {
        open_screen(regs, context);
        return;
    }
    const int row = list_result(list);
    if (row == BOSS_BATTLE_INN_ROW) {
        if (boss_battle_inn_row_wanted()) open_screen(regs, context);
        else no_choice(regs);
        return;
    }
    regs->rax = (uint32_t)boss_battle_inn_native_row(row);
    regs->skip = 1;
}

/* At the Inn menu frame's help text call (r13 the Inn, esi the native row's message, from the cursor read as a native
 * row): the help follows the displayed row. */
void GUEST_HOOK_ABI boss_battle_inn_help_hook(GuestRegs *regs) {
    const uint64_t list = regs->r13 + INN_LIST, table = atomic_load(&inn_help_table);
    if (!has_boss_row(list) || !table) return;
    const int native = boss_battle_inn_native_row(guest_i32(list + LIST_CURSOR));
    if (native == BOSS_BATTLE_INN_NO_ROW) regs->rsi = BOSS_BATTLE_INN_HELP_RECORD;
    else if (native >= 0 && native < BOSS_BATTLE_INN_ROWS - 1) regs->rsi = (uint32_t)guest_i32(table + 4 * (uint64_t)native);
}

/* At an Inn call that passes a native row (rdi the list, esi the row, edx the disabled flag of a set-disabled call). */
static void inn_row_call(GuestRegs *regs, InnCallKind kind) {
    if (!has_boss_row(regs->rdi)) return;
    if (kind != INN_SHIFT) inn_functions.set_disabled(regs->rdi, BOSS_BATTLE_INN_ROW, (int)(regs->rdx & 1));
    regs->rsi = (uint32_t)boss_battle_inn_display_row((int)(uint32_t)regs->rsi);
}

void GUEST_HOOK_ABI boss_battle_inn_shift_hook(GuestRegs *regs) {
    inn_row_call(regs, INN_SHIFT);
}

void GUEST_HOOK_ABI boss_battle_inn_mirror_hook(GuestRegs *regs) {
    inn_row_call(regs, INN_MIRROR);
}

/* At the placement group check's story test (r15 the scene, r13d the group): a story-gated boss room chooses its groups
 * by difficulty, as after Lucain's Tower. */
void GUEST_HOOK_ABI boss_battle_arrange_story_hook(GuestRegs *regs) {
    if (!is_scene(regs->r15) || !boss_battle_story_gated_round(guest_i32(regs->r15 + SCENE_ROUND))) return;
    regs->rax = 0;
    regs->skip = 1;
}

/* At the final boss result's story tier check: a Boss Battle clear never advances the story or starts its ending. */
void GUEST_HOOK_ABI boss_battle_story_clear_hook(GuestRegs *regs) {
    if (!boss_battle_session_active()) return;
    puts("Boss Battle: the clear is kept out of the story tier");
    regs->rax = 0;
    regs->skip = 1;
}

/* At the Ancient Dragon's talisman check (r15 the game context, esi the difficulty): without the nine talismans of the
 * difficulty no hit damages it, and a Boss Battle character need not have them. */
void GUEST_HOOK_ABI boss_battle_dragon_hook(GuestRegs *regs) {
    if (!boss_battle_session_active()) return;
    const uint64_t scene = guest_u64(regs->r15 + CONTEXT_GAME_SCENE);
    if (!is_scene(scene) || guest_i32(scene + SCENE_ROUND) != LOST_ZONE_ROUND) return;
    printf("Boss Battle: Ancient Dragon can be damaged (difficulty %d)\n", (int)(uint32_t)regs->rsi);
    regs->rax = 1;
    regs->skip = 1;
}

/* At the Inn constructor, once it moved the party records back into the play data and before anything reads it. */
void GUEST_HOOK_ABI boss_battle_inn_flushed_hook(GuestRegs *regs) {
    (void)regs;
    const unsigned session = boss_battle_session_id();
    boss_battle_screen_reset();
    boss_battle_inn_arrived();
    if (session && session_record && !boss_battle_session_active()) {
        const BossBattleProgress campaign = record_progress(session_record);
        printf("Boss Battle [%u]: campaign restored level %d xp %lld\n", session, campaign.level + 1,
               (long long)campaign.xp_progress + campaign.xp_pool);
        session_record = 0;
    }
}

/* At the results screen's act 2 asking for act 3 (esi): the closing act instead, once the level-ups are done. */
void GUEST_HOOK_ABI boss_battle_results_level_done_hook(GuestRegs *regs) {
    if (boss_battle_results_level_up_done()) regs->rsi = RESULTS_ACT_CLOSING;
}

/* At the save writer's copy of the save (rsi the save, rdx its size): the copy is taken from the kept save while the
 * campaign is set aside. */
void GUEST_HOOK_ABI boss_battle_save_copy_hook(GuestRegs *regs) {
    if (regs->rdx != BOSS_BATTLE_SAVE_BYTES) return;
    const uint64_t source = boss_battle_campaign_save_source(regs->rsi);
    if (source != regs->rsi) {
        puts("Boss Battle: save written from the campaign kept before the fight");
        regs->rsi = source;
    }
}

/* At the scene factory's call of the title constructor: the session's campaign is put back before the title runs. */
void GUEST_HOOK_ABI boss_battle_title_hook(GuestRegs *regs) {
    (void)regs;
    boss_battle_screen_reset();
    boss_battle_title_reached();
}

/* At the all-down check's game over request: in a session the defeat ends the fight instead of the game over. */
void GUEST_HOOK_ABI boss_battle_party_wiped_hook(GuestRegs *regs) {
    if (boss_battle_party_wiped()) regs->skip = 1;
}

/* At the round end request of a stage-ending boss's death (esi its end kind); the request goes on as the game made it. */
void GUEST_HOOK_ABI boss_battle_boss_cleared_hook(GuestRegs *regs) {
    boss_battle_boss_cleared((int)(int32_t)regs->rsi);
}

static void no_window(GuestRegs *regs) {
    regs->rax = 0;
    regs->skip = 1;
}

/* At the stage-end act's opening of the stage clear screen. */
void GUEST_HOOK_ABI boss_battle_clear_screen_hook(GuestRegs *regs) {
    if (boss_battle_skip_clear_screen()) no_window(regs);
}

/* At the stage-end act's opening of the results screen. */
void GUEST_HOOK_ABI boss_battle_results_screen_hook(GuestRegs *regs) {
    if (boss_battle_skip_results_screen()) no_window(regs);
}

/* At the stage-end act's Town00 request (rdx the stage, rcx the entry, r8 the stage name; r14 the scene). */
void GUEST_HOOK_ABI boss_battle_town_return_hook(GuestRegs *regs) {
    if (!is_scene(regs->r14)) return;
    const uint64_t context = scene_context(regs->r14), slot = context ? party_slot(context, 0) : 0;
    if (boss_battle_outcome() == BOSS_BATTLE_OUTCOME_WIN && slot && guest_i32(slot + SLOT_KIND) == SLOT_LOCAL) {
        const BossBattleProgress after = record_progress(slot);
        boss_battle_progress_won(&after);
    }
    if (!boss_battle_town_return()) return;
    regs->rdx = (uint32_t)boss_battle_inn_entry.stage;
    regs->rcx = (uint32_t)boss_battle_inn_entry.entry;
    regs->r8 = (uint64_t)(uintptr_t)boss_battle_inn_entry.stage_name;
}

enum {
    OPTIONS_HELP = 0xfa0,
    OPTIONS_PAGE = 0xc50,
    OPTIONS_ITEMS = 0x968,
    BOSS_PAGE = 4, /* no page of the game: its row logic and fill leave the rows alone */
    ROW_FRAME = 0x50,
    ROW_TYPE = 0x364,
    ROW_INDEX = 0x370,
    ROW_LABEL = 0x380,
    ROW_VALUE = 0x5c0,
    ROW_SECOND = 0x800,
    ROW_COLOURS = 0x330, /* label colours by row state (disabled, cursor, ...), 4 bytes each; +4 is the plain one */
    ROW_COLOUR_STATES = 5,
    ROW_PLAIN_COLOUR = ROW_COLOURS + 4,
    TEXT_COLOUR = 0x110, /* a two-option row greys its unchosen option here; a single-value row keeps the plain one */
    ROW_TITLE = 0,
    ROW_SINGLE = 1,
    ROW_BUTTON = 5,
    FRAME_SINGLE = 0xe0
};

static _Atomic uint64_t screen_body, screen_window, options_body_vtable;

static uint64_t screen_row(uint64_t body, int row) {
    return guest_u64(guest_u64(body + LIST_WIDGETS_BEGIN) + (uint64_t)row * sizeof(uint64_t));
}

static void set_row_text(uint64_t row, uint64_t text, const char *value) {
    screen_functions.set_text(row + text, value);
}

/* The rows of the Inn's first Options page, as built: the title bar, six rows, the bottom action. */
static int is_first_page(uint64_t body) {
    if (guest_u64(body) != atomic_load(&options_body_vtable) || list_widgets(body) != BOSS_BATTLE_SCREEN_ROWS ||
        guest_i32(body + OPTIONS_ITEMS) != BOSS_BATTLE_SCREEN_ROWS)
        return 0;
    for (int i = 0; i < BOSS_BATTLE_SCREEN_ROWS; ++i) {
        const uint64_t row = screen_row(body, i);
        if (!row || guest_i32(row + ROW_INDEX) != i) return 0;
    }
    return guest_i32(screen_row(body, BOSS_BATTLE_SCREEN_TITLE_ROW) + ROW_TYPE) == ROW_TITLE &&
           guest_i32(screen_row(body, BOSS_BATTLE_SCREEN_RETURN_ROW) + ROW_TYPE) == ROW_BUTTON;
}

/* The label column holds statistics: one colour in every state, so the selection shows only on the boss's value. */
static void keep_label_colour(uint64_t row) {
    uint8_t plain[4];
    memcpy(plain, (const void *)(uintptr_t)(row + ROW_PLAIN_COLOUR), sizeof(plain));
    for (int state = 0; state < ROW_COLOUR_STATES; ++state)
        memcpy((void *)(uintptr_t)(row + ROW_COLOURS + 4 * (uint64_t)state), plain, sizeof(plain));
}

static int is_screen(uint64_t body) {
    return body && body == atomic_load(&screen_body) && boss_battle_screen_shown();
}

/* The texts of the screen for the cursor row; the game's text setter keeps a text that did not change. */
static void refresh_screen(uint64_t body) {
    char text[160];
    const int cursor = guest_i32(body + LIST_CURSOR);
    for (int i = 1; i <= BOSS_BATTLE_SCREEN_BOSS_ROWS; ++i) {
        const uint64_t row = screen_row(body, i);
        boss_battle_screen_label(i, text, sizeof(text));
        set_row_text(row, ROW_VALUE, text);
        boss_battle_screen_statistic(i, cursor, text, sizeof(text));
        set_row_text(row, ROW_LABEL, text);
    }
    boss_battle_screen_title(cursor, text, sizeof(text));
    set_row_text(screen_row(body, BOSS_BATTLE_SCREEN_TITLE_ROW), ROW_VALUE, text);
    boss_battle_screen_help(cursor, text, sizeof(text));
    screen_functions.set_text(atomic_load(&screen_window) + OPTIONS_HELP, text);
}

/* At the page setup's title message (esi): the screen's title is Boss Battle's own record. */
void GUEST_HOOK_ABI boss_battle_options_title_hook(GuestRegs *regs) {
    if (boss_battle_screen_requested()) regs->rsi = BOSS_BATTLE_INN_ROW_RECORD;
}

/* After the page setup built the first page (r15 the page's list, rbx its window): the six rows become single-value
 * rows for the bosses, the bottom action returns to the tavern, and the page leaves the game's settings alone. */
void GUEST_HOOK_ABI boss_battle_options_built_hook(GuestRegs *regs) {
    const uint64_t body = regs->r15, window = regs->rbx;
    if (!boss_battle_screen_requested()) return;
    if (!screen_functions.set_text || !screen_functions.set_message || !screen_functions.row_frame ||
        !inn_functions.set_cursor || !is_first_page(body)) {
        puts("Boss Battle: the Options page is not as expected; it stays the game's Options");
        boss_battle_screen_reset();
        return;
    }
    const int32_t page = BOSS_PAGE, single = ROW_SINGLE;
    const int32_t value_colour = guest_i32(screen_row(body, 1) + ROW_VALUE + TEXT_COLOUR);
    memcpy((void *)(uintptr_t)(body + OPTIONS_PAGE), &page, sizeof(page));
    for (int i = 1; i <= BOSS_BATTLE_SCREEN_BOSS_ROWS; ++i) {
        const uint64_t row = screen_row(body, i);
        memcpy((void *)(uintptr_t)(row + ROW_TYPE), &single, sizeof(single));
        screen_functions.row_frame(row + ROW_FRAME, FRAME_SINGLE, 0.0f);
        set_row_text(row, ROW_SECOND, "");
        keep_label_colour(row);
        memcpy((void *)(uintptr_t)(row + ROW_VALUE + TEXT_COLOUR), &value_colour, sizeof(value_colour));
    }
    screen_functions.set_message(screen_row(body, BOSS_BATTLE_SCREEN_RETURN_ROW) + ROW_VALUE,
                                 BOSS_BATTLE_RETURN_TO_TAVERN_RECORD);
    atomic_store(&screen_body, body);
    atomic_store(&screen_window, window);
    inn_functions.set_cursor(body, boss_battle_screen_built(), 0, 1);
    refresh_screen(body);
}

/* At the page window's call of the page's row logic (rbx the page's list): the screen has none. */
void GUEST_HOOK_ABI boss_battle_options_update_hook(GuestRegs *regs) {
    if (!is_screen(regs->rbx)) return;
    regs->rax = 0;
    regs->skip = 1;
}

/* After the list read this frame's input (eax its result, rbx the list; r15b set closes the page as its row logic
 * would): the texts follow the cursor, a boss row starts its fight, the bottom action returns to the tavern. */
void GUEST_HOOK_ABI boss_battle_options_result_hook(GuestRegs *regs) {
    const uint64_t body = regs->rbx;
    if (!is_screen(body)) return;
    refresh_screen(body);
    int index = -1;
    if (boss_battle_screen_choose((int)(int32_t)regs->rax, &index) != BOSS_BATTLE_SCREEN_NONE) regs->r15 = 1;
}

/* At a list's cursor move (rdi the list): on the screen the boss rows scroll at their ends, as a moved cursor. */
static void list_move(GuestRegs *regs, int down) {
    const uint64_t body = regs->rdi;
    if (!is_screen(body)) return;
    const int scrolled = boss_battle_screen_move(guest_i32(body + LIST_CURSOR), down);
    refresh_screen(body);
    if (!scrolled) return;
    regs->rax = 1;
    regs->skip = 1;
}

void GUEST_HOOK_ABI boss_battle_list_up_hook(GuestRegs *regs) {
    list_move(regs, 0);
}

void GUEST_HOOK_ABI boss_battle_list_down_hook(GuestRegs *regs) {
    list_move(regs, 1);
}

int boss_battle_install(unsigned char *image, uint64_t size, uint64_t entry, const char *game, int online,
                        const DcpEarlyStorySites *town, unsigned char *stubs, size_t stubs_size) {
    boss_battle_set_available(0);
    if (online) {
        puts("Boss Battle: off, Online Play is enabled");
        return 0;
    }
    BossBattleSites sites;
    const char *why = NULL;
    if (!boss_battle_locate(image, size, game, &sites, &why)) {
        printf("Boss Battle: unavailable for %s: %s\n", game ? game : "an unknown game build", why);
        return 0;
    }
    if (!town || !town->selector || town->selector >= size) {
        printf("Boss Battle: unavailable for %s: the lead character selector was not found\n", game);
        return 0;
    }
    GuestHook hooks[HOOKS] = {
        {"scene frame", (uint32_t)sites.hook, HOOK_LENGTH, {0}, boss_battle_scene_hook},
        {"Inn menu built", (uint32_t)sites.inn_built, INN_BUILT_LENGTH, {0}, boss_battle_inn_built_hook},
        {"Inn menu result", (uint32_t)sites.inn_result, INN_HOOK_LENGTH, {0}, boss_battle_inn_result_hook},
        {"Inn menu help", (uint32_t)sites.inn_help, INN_HOOK_LENGTH, {0}, boss_battle_inn_help_hook},
        {"placement story test", (uint32_t)sites.arrange_story, HOOK_LENGTH, {0}, boss_battle_arrange_story_hook},
        {"story tier check", (uint32_t)sites.story_clear, HOOK_LENGTH, {0}, boss_battle_story_clear_hook},
        {"Ancient Dragon talismans", (uint32_t)sites.dragon_talismans, HOOK_LENGTH, {0}, boss_battle_dragon_hook},
        {"title scene", (uint32_t)sites.title, HOOK_LENGTH, {0}, boss_battle_title_hook},
        {"Inn party returned", (uint32_t)sites.inn_flushed, INN_FLUSHED_LENGTH, {0}, boss_battle_inn_flushed_hook},
        {"save copy", (uint32_t)sites.save_copy, HOOK_LENGTH, {0}, boss_battle_save_copy_hook},
        {"party down", (uint32_t)sites.party_wiped, HOOK_LENGTH, {0}, boss_battle_party_wiped_hook},
        {"boss clear", (uint32_t)sites.boss_cleared, HOOK_LENGTH, {0}, boss_battle_boss_cleared_hook},
        {"stage clear screen", (uint32_t)sites.clear_screen, HOOK_LENGTH, {0}, boss_battle_clear_screen_hook},
        {"results screen", (uint32_t)sites.results_screen, HOOK_LENGTH, {0}, boss_battle_results_screen_hook},
        {"Town00 return", (uint32_t)sites.town_return, HOOK_LENGTH, {0}, boss_battle_town_return_hook},
        {"Options title", (uint32_t)sites.options_title, HOOK_LENGTH, {0}, boss_battle_options_title_hook},
        {"Options rows built", (uint32_t)sites.options_built, OPTIONS_BUILT_LENGTH, {0}, boss_battle_options_built_hook},
        {"Options row logic", (uint32_t)sites.options_update, HOOK_LENGTH, {0}, boss_battle_options_update_hook},
        {"Options result", (uint32_t)sites.options_result, HOOK_LENGTH, {0}, boss_battle_options_result_hook},
        {"list cursor up", (uint32_t)sites.list_up, LIST_MOVE_LENGTH, {0}, boss_battle_list_up_hook},
        {"list cursor down", (uint32_t)sites.list_down, LIST_MOVE_LENGTH, {0}, boss_battle_list_down_hook},
        {"results level-up end", (uint32_t)sites.results_level_done, RESULTS_LEVEL_DONE_LENGTH, {0},
         boss_battle_results_level_done_hook},
    };
    for (int i = 0; i < INN_CALLS; ++i)
        hooks[FIXED_HOOKS + i] = (GuestHook){"Inn row call", (uint32_t)sites.inn_calls[i], HOOK_LENGTH, {0},
                                             kInnCalls[i].kind == INN_SHIFT ? boss_battle_inn_shift_hook
                                                                            : boss_battle_inn_mirror_hook};
    for (size_t i = 0; i < HOOKS; ++i) memcpy(hooks[i].original, image + hooks[i].offset, hooks[i].length);
    size_t bad = 0;
    const GuestHookStatus status = guest_hooks_install(image, size, entry, NULL, hooks, HOOKS, stubs, stubs_size, &bad);
    if (status != GUEST_HOOK_OK) {
        printf("Boss Battle: unavailable for %s: %s (%s)\n", game, guest_hook_status_text(status), hooks[bad].name);
        return 0;
    }
    atomic_store(&request_function, (SceneRequestFn)(uintptr_t)(image + sites.request));
    atomic_store(&lead_selector, (LeadSelectorFn)(uintptr_t)(image + town->selector));
    atomic_store(&scene_vtable, (uint64_t)(uintptr_t)(image + sites.scene_vtable));
    boss_battle_set_inn_help_table((uint64_t)(uintptr_t)(image + sites.inn_help_table));
    const BossBattleInnFunctions functions = {
        (uint64_t(GUEST_HOOK_ABI *)(uint64_t))(uintptr_t)(image + sites.allocate),
        (void(GUEST_HOOK_ABI *)(uint64_t, uint64_t))(uintptr_t)(image + sites.widget),
        (int(GUEST_HOOK_ABI *)(uint64_t, uint64_t))(uintptr_t)(image + sites.add_widget),
        (void(GUEST_HOOK_ABI *)(uint64_t, int, uint64_t))(uintptr_t)(image + sites.add_item),
        (void(GUEST_HOOK_ABI *)(uint64_t, int, int))(uintptr_t)(image + sites.set_disabled),
        (void(GUEST_HOOK_ABI *)(uint64_t, int, int, int))(uintptr_t)(image + sites.set_cursor),
        (void(GUEST_HOOK_ABI *)(uint64_t))(uintptr_t)(image + sites.inn_party),
    };
    boss_battle_set_inn_functions(&functions);
    const BossBattleScreenFunctions screen = {
        (void(GUEST_HOOK_ABI *)(uint64_t, int, int))(uintptr_t)(image + sites.set_step),
        (void(GUEST_HOOK_ABI *)(uint64_t, const char *))(uintptr_t)(image + sites.set_text),
        (void(GUEST_HOOK_ABI *)(uint64_t, int))(uintptr_t)(image + sites.set_message),
        (void(GUEST_HOOK_ABI *)(uint64_t, int, float))(uintptr_t)(image + sites.row_frame),
    };
    boss_battle_set_screen_functions(&screen);
    const BossBattleLevelFunctions levels = {
        (void(GUEST_HOOK_ABI *)(uint64_t, uint32_t))(uintptr_t)(image + sites.set_level),
        (int(GUEST_HOOK_ABI *)(uint64_t))(uintptr_t)(image + sites.level_pattern),
        (void(GUEST_HOOK_ABI *)(uint64_t, int, int, int, int))(uintptr_t)(image + sites.write_stats),
    };
    boss_battle_set_level_functions(&levels);
    atomic_store(&options_body_vtable, (uint64_t)(uintptr_t)(image + sites.options_body_vtable));
    boss_battle_campaign_configure((uint64_t)(uintptr_t)(image + sites.play_data_table));
    boss_battle_set_memory_guard(boss_battle_campaign_snapshot, boss_battle_campaign_restore);
    boss_battle_set_available(1);
    printf("Boss Battle: installed, %d of %d encounters, %s (%s)\n", boss_battle_startable_count(), BOSS_BATTLE_ENCOUNTERS,
           boss_battle_enabled() ? "enabled" : "disabled", game);
    return 1;
}
