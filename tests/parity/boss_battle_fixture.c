#include "../../src/boss_battle_hook.c"

#include "fixture.h"

/* The code variants of each listed build's compile. */
typedef struct {
    const char *game;
    int compact, inn_asia, wipe_compact;
} Variant;

static const Variant kVariants[] = {
    {"CUSA10454-1.00", 0, 0, 0}, {"CUSA10454-1.01", 0, 0, 0}, {"CUSA07001-1.00", 1, 1, 1},
    {"CUSA07001-1.02", 0, 1, 1}, {"CUSA07001-1.03", 0, 1, 0}, {"CUSA10209-1.00", 1, 1, 1},
};

static uint64_t lea_target(Fx *fx, uint64_t lea, size_t room) {
    return fx_rel(fx, lea + 3, lea + 7, room);
}

static void plant_scene(Fx *fx, const BuildSites *b, const Variant *v) {
    FX_SHAPE(fx, b->dispatcher, kDispatcher);
    FX_SHAPE(fx, b->frame_tail, kFrameTail);
    FX_SHAPE(fx, b->dispatcher + kActSwitchAt, kActSwitch);
    const uint64_t acts = lea_target(fx, b->dispatcher + kActTableLea, 4 * 10);
    const uint64_t end_case = fx_alloc(fx, 16), arrival_case = fx_alloc(fx, 16);
    FX_SHAPE(fx, end_case, kActCase);
    fx_call_to(fx, end_case + 3, b->stage_end);
    fx_entry(fx, acts, BOSS_BATTLE_ACT_STAGE_END, end_case);
    FX_SHAPE(fx, arrival_case, kActCase);
    fx_entry(fx, acts, ARRIVAL_ACT, arrival_case);
    if (v->compact) {
        FX_SHAPE(fx, b->stage_end, kStageEndCompact);
        FX_SHAPE(fx, b->stage_end_steps, kStageEndStepsCompact);
    } else {
        FX_SHAPE(fx, b->stage_end, kStageEnd);
        FX_SHAPE(fx, b->stage_end_steps, kStageEndSteps);
    }
    const uint64_t steps = lea_target(fx, b->stage_end_steps + (v->compact ? kStepTableLeaCompact : kStepTableLea), 4 * STAGE_END_STEPS);
    fx_entry(fx, steps, STEP_CLEAR_SCREEN, b->clear_screen_block - (v->compact ? kClearScreenStepOffsetCompact : kClearScreenStepOffset));
    fx_entry(fx, steps, STEP_RESULTS_SCREEN, b->results_screen_block);
    fx_pointer_to(fx, b->scene_vtable + VTABLE_DISPATCH, b->dispatcher);
    FX_SHAPE(fx, fx_pointer(fx, b->scene_vtable + VTABLE_CONTEXT, 16), kContextGetter);
    FX_SHAPE(fx, fx_pointer(fx, b->scene_vtable + VTABLE_SET_STEP, 32), kSetStep);
    FX_SHAPE(fx, b->request, kRequest);

    const uint64_t act = fx_call(fx, arrival_case + 3, 0x800), arrival = act + kArrivalStepsInAct;
    FX_SHAPE(fx, arrival, kArrivalSteps);
    const uint64_t arrival_table = lea_target(fx, arrival + kArrivalTableLea, 4 * 22);
    const uint64_t step = fx_alloc(fx, 0x100);
    fx_entry(fx, arrival_table, ARRIVAL_STEP_FADE_IN, step);
    fx_rel_to(fx, step + kArrivalStepLabel + 3, step + kArrivalStepLabel + 7, fx_text(fx, "STEP_FADEIN"));
    const uint64_t fade_in = step + kArrivalFadeInStep;
    FX_SHAPE(fx, fade_in, kArrivalFadeIn);
    const uint64_t leave = b->leave_mode + kLeaveFadeInLeave;
    FX_SHAPE(fx, leave, kLeaveFade);
    const uint64_t get_fader = fx_call(fx, leave + kLeaveFaderCall, 0x40), fade = fx_call(fx, leave + kLeaveFadeCall, 0x60);
    FX_SHAPE(fx, get_fader, kGetFader);
    FX_SHAPE(fx, fade, kFade);
    FX_SHAPE(fx, fx_call(fx, fade + kFadeLayerMapCall, 16), kLayerMap);
    FX_SHAPE(fx, fade + kFadeFramesStore, kFadeFrames);
    fx_call_to(fx, fade_in + kArrivalFaderCall, get_fader);
    fx_call_to(fx, fade_in + kArrivalFadeCall, fade);
}

static void plant_rounds(Fx *fx, const BuildSites *b) {
    FX_SHAPE(fx, b->round_entry, kRoundEntry);
    const uint64_t slot = fx_rel(fx, b->round_entry + kRoundSlotDisp, b->round_entry + sizeof(kRoundEntry) / sizeof(*kRoundEntry) - 1, 8);
    const uint64_t table = fx_pointer(fx, slot, 64 * ROUND_RECORD);
    fx_pointer_to(fx, table + (uint64_t)kTown.index * ROUND_RECORD, fx_text(fx, kTown.round));
    for (int i = 0; i < BOSS_BATTLE_ENCOUNTERS; ++i) {
        const BossBattleEncounter *encounter = boss_battle_encounter(i);
        const uint64_t record = table + (uint64_t)encounter->round_index * ROUND_RECORD;
        const uint64_t name = fx_pointer(fx, record, 0x30);
        fx_put(fx, name, encounter->round, strlen(encounter->round) + 1);
    }
}

static const enum { TO_DISABLED, TO_CURSOR, TO_IS_DISABLED } kInnCallTargets[INN_CALLS] = {
    TO_DISABLED, TO_DISABLED, TO_CURSOR, TO_DISABLED, TO_DISABLED, TO_DISABLED,
    TO_DISABLED, TO_DISABLED, TO_DISABLED, TO_IS_DISABLED, TO_CURSOR};

static void plant_inn(Fx *fx, const BuildSites *b, const Variant *v) {
    fx_pointer_to(fx, b->inn_vtable + INN_VTABLE_BUILD, b->inn_builder);
    fx_pointer_to(fx, b->inn_vtable + INN_VTABLE_UPDATE, b->inn_update);
    if (v->inn_asia) FX_SHAPE(fx, b->inn_built_block, kInnBuiltAsia);
    else FX_SHAPE(fx, b->inn_built_block, kInnBuilt);
    fx_call(fx, b->inn_built_block + kInnAllocateCall, 16);
    fx_call(fx, b->inn_built_block + kInnWidgetCall, 16);
    FX_SHAPE(fx, fx_call(fx, b->inn_built_block + kInnAddWidgetCall, 0x80), kListAdd);
    const uint64_t add_item = fx_call(fx, b->inn_built_block + kInnFirstItemCall, 16);
    for (int item = 1; item < 6; ++item) fx_call_to(fx, b->inn_built_block + kInnFirstItemCall + (uint64_t)item * kInnItemStride, add_item);
    FX_SHAPE(fx, b->inn_result_block, kInnResult);
    FX_SHAPE(fx, fx_call(fx, b->inn_result_block + kInnResultHook, 0x40), kListResult);
    FX_SHAPE(fx, b->inn_help_block, kInnHelp);
    lea_target(fx, b->inn_help_block + kInnHelpLea, 4 * BOSS_BATTLE_INN_ROWS);
    fx_pointer_to(fx, b->inn_vtable + INN_VTABLE_DRAW, b->inn_draw_block - 0x40);
    FX_SHAPE(fx, b->inn_draw_block, kInnDraw);
    const uint64_t list_draw = fx_pointer(fx, b->list_vtable + LIST_VTABLE_DRAW, 0x60);
    fx_call_to(fx, b->inn_draw_block + kInnDrawCall, list_draw);
    FX_SHAPE(fx, list_draw, kListDraw);
    FX_SHAPE(fx, fx_jump(fx, list_draw + kListDrawTail, 0x40), kListDrawBody);
    const uint64_t destructor = fx_pointer(fx, b->inn_vtable, 0x40);
    FX_SHAPE(fx, destructor, kInnDestructor);
    fx_rel_to(fx, destructor + kInnDestructorVtableLea + 3, destructor + kInnDestructorVtableLea + 7, b->inn_vtable - 0x10);

    const uint64_t is_disabled = fx_pointer(fx, b->list_vtable + LIST_VTABLE_IS_DISABLED, 16);
    const uint64_t set_disabled = fx_alloc(fx, 0x20), set_cursor = fx_alloc(fx, 0x20);
    FX_SHAPE(fx, set_disabled, kListSetDisabled);
    FX_SHAPE(fx, set_cursor, kListSetCursor);
    for (int i = 0; i < INN_CALLS; ++i) {
        fx_put(fx, b->inn_calls[i], kInnCalls[i].bytes, kInnCalls[i].length);
        fx_call_to(fx, b->inn_calls[i] + kInnCalls[i].length - 1,
                   kInnCallTargets[i] == TO_CURSOR ? set_cursor : kInnCallTargets[i] == TO_DISABLED ? set_disabled : is_disabled);
    }
    FX_SHAPE(fx, b->inn_constructor, kInnConstructor);
    fx_rel_to(fx, b->inn_constructor + kInnConstructorVtableLea + 3, b->inn_constructor + kInnConstructorVtableLea + 7, b->inn_vtable - 0x10);
    FX_SHAPE(fx, b->inn_flush_block, kInnFlush);
}

static void plant_background(Fx *fx, const BuildSites *b, const Variant *v) {
    if (v->compact) {
        FX_SHAPE(fx, b->town_background_block, kTownBackgroundGlobals);
        FX_SHAPE(fx, b->inn_cursor_art_block, kInnCursorArtGlobals);
        const uint64_t base = fx_rel(fx, b->town_background_block + 3, b->town_background_block + 7, 16);
        fx_rel_to(fx, b->town_background_block + 10, b->town_background_block + 14, base + 8);
        fx_rel_to(fx, b->inn_cursor_art_block + 3, b->inn_cursor_art_block + 7, base);
        fx_rel_to(fx, b->inn_cursor_art_block + 10, b->inn_cursor_art_block + 14, base + 8);
    } else {
        FX_SHAPE(fx, b->town_background_block, kTownBackground);
        FX_SHAPE(fx, b->inn_cursor_art_block, kInnCursorArt);
        fx_call_to(fx, b->inn_cursor_art_block, fx_call(fx, b->town_background_block + kTownBackgroundBase, 16));
        fx_call_to(fx, b->inn_cursor_art_block + kInnCursorArtLanguage, fx_call(fx, b->town_background_block + kTownBackgroundLanguage, 16));
    }
    FX_SHAPE(fx, b->inn_root_part_block, kInnRootPart);
    FX_SHAPE(fx, b->row_frame, kRowFrame);
    const uint64_t part_set = fx_jump(fx, b->row_frame + kPartSetJump, 0x60);
    FX_SHAPE(fx, fx_call(fx, part_set + kPartLayersCall, 0x80) + kPartLayersPastEntry, kPartLayers);
    FX_SHAPE(fx, b->inn_layer_block, kInnLayer);
    FX_SHAPE(fx, b->inn_layer_tail_block, kInnLayerTail);
    const uint64_t shown = fx_call(fx, b->inn_layer_block + kInnLayerCall, 0x20);
    FX_SHAPE(fx, shown, kLayerShown);
    fx_jump_to(fx, b->inn_layer_tail_block + kInnLayerTailJump, shown);
}

static void plant_leave_and_save(Fx *fx, const BuildSites *b) {
    fx_pointer_to(fx, b->inn_vtable + INN_VTABLE_LEAVE, b->leave_mode);
    FX_SHAPE(fx, b->leave_party_block, kLeaveParty);
    const uint64_t party = fx_call(fx, b->leave_party_block + kLeavePartyCall, 0x200);
    FX_SHAPE(fx, party, kInnParty);
    FX_SHAPE(fx, party + kInnPartyTailAt, kInnPartyTail);
    FX_SHAPE(fx, b->save_copy_block, kSaveCopy);
    FX_SHAPE(fx, fx_call(fx, b->save_copy_block + kSaveCopyCall, 0x20), kSaveCopyEntry);
    FX_SHAPE(fx, b->save_allocation, kSaveAllocation);
    FX_SHAPE(fx, b->play_data_allocation, kPlayDataAllocation);
    const uint64_t play_data = lea_target(fx, b->play_data_allocation + kPlayDataTableLea, 8 * 256);
    FX_SHAPE(fx, b->leave_save_block, kLeaveSave);
    const uint64_t block = fx_call(fx, b->leave_save_block + kLeaveSaveCall, 0x100) + kInnSaveStepBlock;
    FX_SHAPE(fx, block, kInnSaveStep);
    const uint64_t commit = fx_call(fx, block + kInnSaveCommitCall, 0x40);
    FX_SHAPE(fx, commit, kSaveCommit);
    fx_rel_to(fx, commit + kSaveCommitTableLea + 3, commit + kSaveCommitTableLea + 7, play_data);
    FX_SHAPE(fx, fx_call(fx, block + kInnSaveRequestCall, 0x40), kSaveRequest);
}

static uint64_t plant_text(Fx *fx, const BuildSites *b, uint64_t text_changed) {
    const uint64_t setter = fx_jump(fx, b->inn_help_block + kInnHelpTail, 0x100), text = setter + kHelpTextInSetter;
    FX_SHAPE(fx, setter, kHelpSetter);
    FX_SHAPE(fx, text, kHelpText);
    fx_call_to(fx, text + kHelpChangedCall, text_changed);
    FX_SHAPE(fx, text_changed, kTextChanged);
    const uint64_t message = fx_call(fx, text + kHelpMessageCall, 0x140);
    const uint64_t by_number = fx_jump(fx, message + kMessageByNumberJump, 0x60);
    FX_SHAPE(fx, fx_call(fx, by_number + kByNumberStringCall, 0x60), kSetString);
    return message;
}

static void plant_special(Fx *fx, const BuildSites *b) {
    FX_SHAPE(fx, b->arrange_block, kArrangeStory);
    const uint64_t test = fx_call(fx, b->arrange_block + kArrangeStoryHook, 0x20);
    FX_SHAPE(fx, test, kStoryTest);
    FX_SHAPE(fx, fx_call(fx, test + kStoryTestAccessorCall, 16), kStoryBelow45);
    FX_SHAPE(fx, b->story_clear_block, kStoryClear);
    FX_SHAPE(fx, fx_call(fx, b->story_clear_block + kStoryClearHook, 0x40), kStoryTierApplies);
    FX_SHAPE(fx, b->labyrinth_select, kLabyrinthSelect);
    FX_SHAPE(fx, b->dragon_block, kDragonTalismans);
    FX_SHAPE(fx, fx_call(fx, b->dragon_block + kTalismanCall, 0x40), kTalismanCheck);
    FX_SHAPE(fx, b->title_case, kTitleCase);
    const uint64_t title = fx_call(fx, b->title_case + kTitleCall, 0x40);
    FX_SHAPE(fx, title, kTitleConstructor);
    fx_rel_to(fx, title + kTitleVtableLea + 3, title + kTitleVtableLea + 7, b->title_vtable);
    FX_SHAPE(fx, b->cue_player, kCuePlayer);
    const uint64_t lookup = fx_call(fx, b->cue_player + kCueLookupInPlayer, 0x1a0) + kCueLookupMessage;
    fx_rel_to(fx, lookup + 3, lookup + 7, fx_text(fx, "E2010040101:Can not find specified cue ID."));
}

static void plant_outcomes(Fx *fx, const BuildSites *b, const Variant *v) {
    FX_SHAPE(fx, b->gauge_getter, kGaugeGetter);
    if (v->wipe_compact) FX_SHAPE(fx, b->wipe_block, kPartyWipeCompact);
    else FX_SHAPE(fx, b->wipe_block, kPartyWipe);
    FX_SHAPE(fx, fx_call(fx, b->wipe_block + (v->wipe_compact ? kPartyWipeCompactCall : kPartyWipeCall), 0x40), kGameOverRequest);
    FX_SHAPE(fx, b->boss_clear_block, kBossClear);
    FX_SHAPE(fx, fx_call(fx, b->boss_clear_block + kStageEndingUnitCall, 0x40), kStageEndingUnit);
    FX_SHAPE(fx, fx_call(fx, b->boss_clear_block + kBossClearCall, 0x40), kRoundEndRequest);
    if (v->compact) {
        FX_SHAPE(fx, b->clear_screen_block, kClearScreenCompact);
        FX_SHAPE(fx, b->results_screen_block, kResultsScreenCompact);
        FX_SHAPE(fx, b->town_return_block, kTownReturnCompact);
    } else {
        FX_SHAPE(fx, b->clear_screen_block, kClearScreen);
        FX_SHAPE(fx, b->results_screen_block, kResultsScreen);
        FX_SHAPE(fx, b->town_return_block, kTownReturn);
    }
    const uint64_t opener = fx_call(fx, b->clear_screen_block + kClearScreenCall, 0x40);
    FX_SHAPE(fx, opener, kWindowOpener);
    fx_call_to(fx, b->results_screen_block + kResultsScreenCall, opener);
    fx_call_to(fx, b->town_return_block + kTownReturnCall, b->request);
    fx_rel_to(fx, b->town_return_block + kTownReturnRoundLea + 3, b->town_return_block + kTownReturnRoundLea + 7, fx_text(fx, kTown.round));
}

static void plant_options(Fx *fx, const BuildSites *b) {
    const uint64_t results = lea_target(fx, b->inn_result_block + kInnResultTableLea, 4 * 8), options = fx_alloc(fx, 16);
    fx_entry(fx, results, INN_OPTIONS_RESULT_ENTRY, options);
    FX_SHAPE(fx, options, kInnOptionsCase);
    const uint64_t mode = b->options_open_block - kOptionsOpenInMode;
    fx_pointer_to(fx, b->inn_vtable + INN_VTABLE_OPTIONS, mode);
    FX_SHAPE(fx, b->options_open_block, kOptionsOpen);
    FX_SHAPE(fx, mode + kOptionsEnterInMode, kOptionsEnter);
    FX_SHAPE(fx, mode + kOptionsLeaveInMode, kOptionsLeave);
    const uint64_t entered = fx_call(fx, mode + kOptionsLeaveInMode + kOptionsLeaveCall, 0x20);
    FX_SHAPE(fx, entered, kStateEntered);
    fx_call_to(fx, mode + kOptionsEnterInMode + kOptionsLeaveCall, entered);
    FX_SHAPE(fx, mode + kOptionsStepsInMode, kOptionsSteps);
    FX_SHAPE(fx, mode + kOptionsStepPageClosedInMode, kOptionsStepPageClosed);
    fx_call_to(fx, b->options_open_block + kOptionsOpenCall, b->options_setup_title - 0x40);
    FX_SHAPE(fx, b->options_setup_title, kOptionsTitle);
    FX_SHAPE(fx, b->options_setup_built, kOptionsBuilt);
    FX_SHAPE(fx, fx_call(fx, b->options_setup_built + kOptionsBuilderCall, 0x40), kPageBuilder);
    const uint64_t act = fx_pointer(fx, b->options_window_vtable + OPTIONS_VTABLE_ACT, 0x60), act2 = b->options_update_block - 0x252;
    fx_call_to(fx, act + kOptionsAct2Call, act2);
    FX_SHAPE(fx, act2, kOptionsAct2);
    FX_SHAPE(fx, b->options_update_block, kOptionsUpdate);
    FX_SHAPE(fx, b->options_result_block, kOptionsResult);
    fx_rel_to(fx, b->options_update_block + kOptionsBackJump + 2, b->options_update_block + kOptionsBackJump + 6, b->options_result_block);
    FX_SHAPE(fx, fx_pointer(fx, b->options_body_vtable + LIST_VTABLE_FILL, 0x20), kPageFill);
    FX_SHAPE(fx, fx_pointer(fx, b->options_body_vtable + LIST_VTABLE_UP, 0x20), kListMove);
    FX_SHAPE(fx, fx_pointer(fx, b->options_body_vtable + LIST_VTABLE_DOWN, 0x20), kListMove);
    fx_pointer_to(fx, b->options_body_vtable + LIST_VTABLE_UPDATE, b->network_back_query - 0x584);
    FX_SHAPE(fx, b->network_back_query - 0x584, kListUpdate);
    const uint64_t list_play = fx_pointer(fx, b->options_body_vtable + LIST_VTABLE_PLAY_SE, 0x20);
    FX_SHAPE(fx, list_play, kListPlaySe);
    const uint64_t play = fx_jump(fx, list_play + kListPlaySeJump, 0x20);
    FX_SHAPE(fx, play, kPlaySe);
    const uint64_t sounds = lea_target(fx, play + kPlaySeTableLea, SE_ENTRY_BYTES * 64);
    fx_pointer_to(fx, sounds + SE_ENTRY_BYTES * BOSS_BATTLE_SE_SELECT, fx_text(fx, "COMMON_SELECT"));
    fx_pointer_to(fx, sounds + SE_ENTRY_BYTES * BOSS_BATTLE_SE_DECIDE, fx_text(fx, "COMMON_DECIDE"));
    fx_pointer_to(fx, sounds + SE_ENTRY_BYTES * BOSS_BATTLE_SE_CANCEL, fx_text(fx, "COMMON_CANCEL"));
}

static void plant_pause(Fx *fx, const BuildSites *b, const Variant *v, uint64_t set_message) {
    FX_SHAPE(fx, b->pause_start_gate, kStartGate);
    FX_SHAPE(fx, b->pause_level_gate - 4, kLevelGate);
    const uint64_t start = fx_call(fx, b->pause_start_gate, 0x20), level = fx_call(fx, b->pause_level_gate, 0x20);
    FX_SHAPE(fx, start, kStartGateFn);
    FX_SHAPE(fx, level, kLevelGateFn);
    fx_call_to(fx, level + 4, fx_call(fx, start + 4, 16));
    FX_SHAPE(fx, b->pause_mode, kPauseMode);
    FX_SHAPE(fx, b->pause_mode + kPauseModeStoreAt, kPauseModeStore);
    FX_SHAPE(fx, b->pause_setup, kPauseSetup);
    if (v->compact) {
        const uint64_t dispatch = b->pause_row - kPauseDispatchJump, plain = b->pause_row + PAUSE_DISPATCH_LENGTH;
        FX_SHAPE(fx, dispatch, kPauseDispatchInline);
        FX_SHAPE(fx, plain, kPausePlainRowInline);
        FX_SHAPE(fx, b->pause_quit_row, kPauseQuitRowInline);
        fx_entry(fx, lea_target(fx, dispatch + kPauseDispatchTableLea, 16), 0, plain);
        lea_target(fx, plain + kPausePlainRowVtableLea, 0x40);
    } else {
        FX_SHAPE(fx, b->pause_row, kPauseRow);
        FX_SHAPE(fx, b->pause_quit_row - 0x12, kPauseQuitRow);
        fx_call_to(fx, b->pause_quit_row, b->pause_row);
    }
    FX_SHAPE(fx, b->pause_floor_reward - kFloorRewardCallAt, kFloorRewardCall);
    FX_BYTES(fx, fx_call(fx, b->pause_floor_reward, 0x40) + 0x10, kFloorRewardLabel);
    for (int i = 0; i < 3; ++i) {
        if (i) FX_SHAPE(fx, b->pause_setup_calls[i] - 3, kPauseSetupCallR13);
        else FX_SHAPE(fx, b->pause_setup_calls[i] - 3, kPauseSetupCallRbx);
        fx_call_to(fx, b->pause_setup_calls[i], b->pause_setup);
    }
    FX_SHAPE(fx, b->pause_confirm_text - 0x3f, kPauseConfirmMode);
    FX_SHAPE(fx, b->pause_confirm_text - 5, kPauseConfirmText);
    fx_call_to(fx, b->pause_confirm_text, set_message);
    FX_SHAPE(fx, b->pause_result - kPauseResultCallAt, kPauseResult);
    const uint64_t result = fx_call(fx, b->pause_result, 0x60);
    const int32_t field = v->inn_asia ? 0xa964 : 0xadf4;
    FX_SHAPE(fx, result, kPauseResultFn);
    FX_SHAPE(fx, result + kPauseResultReadAt, kPauseResultRead);
    fx_put(fx, result + kPauseResultReadAt + kPauseResultField, &field, sizeof(field));
}

static void plant_network(Fx *fx, const BuildSites *b, uint64_t set_message) {
    const uint64_t add = fx_call(fx, b->inn_built_block + kInnAddWidgetCall, 0);
    FX_SHAPE(fx, b->network_return_row, kNetworkReturnRowMessage);
    fx_call_to(fx, b->network_return_row + 5, add);
    FX_SHAPE(fx, b->network_end_row, kNetworkEndRowMessage);
    fx_call_to(fx, b->network_end_row + 5, add);
    const uint64_t close = b->network_close_path, confirm = b->network_confirm;
    FX_SHAPE(fx, close, kNetworkClosePath);
    fx_rel_to(fx, close + kNetworkTooHighJump, close + kNetworkTooHighJump + 4,
              fx_rel(fx, close + kNetworkNoRowJump, close + kNetworkNoRowJump + 4, 16));
    const uint64_t row_at = fx_call(fx, close - kNetworkCallBeforeClosePath, 16);
    FX_SHAPE(fx, confirm, kNetworkConfirmPath);
    fx_call_to(fx, confirm, row_at);
    fx_rel_to(fx, confirm + kConfirmTooHighJump, confirm + kConfirmTooHighJump + 4,
              fx_rel(fx, confirm + kConfirmNoRowJump, confirm + kConfirmNoRowJump + 4, 16));
    FX_SHAPE(fx, b->network_back_query - kBackQueryCall, kNetworkBackQuery);

    FX_SHAPE(fx, b->room_info_block, kRoomInfoSlot);
    FX_SHAPE(fx, b->room_info_block + kRoomInfoCopyAt, kRoomInfoCopy);
    fx_call(fx, b->room_info_block + kRoomInfoCharacterCall, 16);
    fx_call(fx, b->room_info_block + kRoomInfoIndexCall, 16);
    fx_call(fx, b->room_info_block + kRoomInfoControllableCall, 16);
    fx_call(fx, b->room_info_block + kRoomInfoMatchCall, 16);

    FX_SHAPE(fx, b->join_state_call - kJoinStateCall, kJoinState);
    fx_call(fx, b->join_state_call, 16);
    const uint64_t unit = b->join_state_call - kJoinUnitBefore;
    FX_SHAPE(fx, unit - kJoinUnitCall, kJoinUnit);
    fx_call(fx, unit, 16);
    const uint64_t lookup = fx_call(fx, b->party_slot_calls[0], 16);
    for (int i = 0; i < 2; ++i) {
        fx_call_to(fx, b->party_slot_calls[i], lookup);
        FX_SHAPE(fx, b->party_slot_calls[i] + 5, kPartySlotTail);
        FX_SHAPE(fx, b->party_slot_calls[i] + 7, kPartySlotImul);
    }
    FX_SHAPE(fx, b->add_player, kAddPlayer);
    FX_SHAPE(fx, b->slot_request, kSlotRequest);
    FX_SHAPE(fx, b->location_card, kCardLocation);
    fx_call_to(fx, b->location_card + kCardLocationCall, set_message);
    FX_SHAPE(fx, b->location_card + kCardLocationCall + 5, kCardLocationTail);
}

static void plant_progression(Fx *fx, const BuildSites *b) {
    const uint64_t acts = fx_pointer(fx, b->results_window_vtable + RESULTS_VTABLE_ACT, 0x40);
    FX_SHAPE(fx, acts, kResultsActs);
    const uint64_t act_case = fx_alloc(fx, 16);
    fx_entry(fx, lea_target(fx, acts + kResultsActTableLea, 4 * 8), RESULTS_ACT_LEVEL_UP, act_case);
    FX_SHAPE(fx, act_case, kResultsAct2Case);
    const uint64_t act2 = fx_call(fx, act_case + 3, 0x100);
    FX_SHAPE(fx, act2, kResultsAct2);
    FX_SHAPE(fx, act2 + kResultsLevelDoneInAct2, kResultsLevelDone);
    FX_SHAPE(fx, b->set_level, kSetLevel);
    FX_SHAPE(fx, b->level_pattern, kLevelPattern);
    FX_SHAPE(fx, b->write_stats, kWriteStats);
}

static void plant_handoff(Fx *fx) {
    const uint64_t tail = fx_alloc(fx, 0x20), reset = fx_alloc(fx, 0x40), teardown = fx_alloc(fx, 0x20);
    FX_BYTES(fx, tail, kPartyResetTail);
    fx_rel_to(fx, tail + sizeof(kPartyResetTail), tail + sizeof(kPartyResetTail) + 4, reset);
    FX_BYTES(fx, reset, kPartyResetEntry);
    FX_BYTES(fx, reset + PARTY_RESET_CONNECTED_AT, kPartyResetConnected);
    for (size_t i = 0; i < sizeof(kSeatTeardown); ++i)
        if (!((i >= 4 && i < 8) || (i >= 17 && i < 21))) fx_put(fx, teardown + i, &kSeatTeardown[i], 1);
    fx_rel(fx, teardown + 4, teardown + 8, 16);
    fx_rel(fx, teardown + 17, teardown + 21, 16);
    fx_rel(fx, teardown + sizeof(kSeatTeardown), teardown + sizeof(kSeatTeardown) + 4, 16);
}

void fixture_boss_battle(Fx *fx, const char *layout, uint64_t text_changed) {
    const BuildSites *b = listed_build(layout);
    const Variant *v = NULL;
    for (size_t i = 0; i < sizeof(kVariants) / sizeof(*kVariants); ++i)
        if (!strcmp(kVariants[i].game, layout)) v = &kVariants[i];
    if (!b || !v) return;
    fx_context(fx, "Boss Battle");
    plant_scene(fx, b, v);
    plant_rounds(fx, b);
    plant_inn(fx, b, v);
    plant_background(fx, b, v);
    plant_leave_and_save(fx, b);
    plant_special(fx, b);
    plant_outcomes(fx, b, v);
    const uint64_t set_message = plant_text(fx, b, text_changed);
    plant_options(fx, b);
    plant_pause(fx, b, v, set_message);
    plant_network(fx, b, set_message);
    plant_progression(fx, b);
    plant_handoff(fx);
}

void fixture_room_leave(Fx *fx, uint64_t room_wish) {
    const uint64_t at = room_wish + 0x130;
    FX_BYTES(fx, at, kRoomLeave);
    fx_rel(fx, at + sizeof(kRoomLeave), at + sizeof(kRoomLeave) + 4, 16);
}
