/* game_main.c — the game core's entry: the original `entry` (1000:0000)
 * re-read.
 *
 * The arch layer owns main() and hands us the filled OSystem vtable
 * (port_structure.md §2).  The boot order below mirrors the original's
 * (game_structure.md §2):
 *
 *   entry -> apply_relocations (skipped: the port ships plain code)
 *         -> init_video/init_crtc_timer   (one unchained mode-X set)
 *         -> boot_intro_sequence          (the intro .BIT cards + the scene)
 *         -> the level walk               (the cutscenes, once per level)
 *         -> menu_loop                    (the map / level-select room)
 *
 * The loop structure copies the original's split (re_map.md, "Timer / INT 8"):
 * the tick callback is the ISR body — flip apply, palette step, then the
 * music/keyed events — and the foreground advances the flow exactly once per
 * tick boundary.  The modules are real; the open deviations are listed in
 * port_deviations.md.
 */
#include "gameflow.h"
#include "attract.h"
#include "audio.h"
#include "entity.h"
#include "font.h"
#include "frontend.h"
#include "game_strs.h"
#include "level.h"
#include "map.h"
#include "menu.h"
#include "object.h"
#include "osystem.h"
#include "palette.h"
#include "party_intro.h"
#include "res.h"
#include "save.h"
#include "scene.h"
#include "score.h"
#include "script.h"
#include "statusbar.h"
#include "textpage.h"
#include "video.h"

#include <stdio.h>
#include <stddef.h>

/* draw_level_text (0x33A3)'s level-end transition: caption -> grey fade ->
 * 0x23 hold -> black fade -> bar park + world redraw.  check_highscore's card
 * (won) or 0x438A's exit (lost) follows. */
enum {
    END_CAPTION,
    END_GREY_FADE,
    END_HOLD,
    END_BLACK_FADE
};

/* --- the level's foreground: the pause gate + the actions ------------- */

/* level_run's fresh-load party state (307A) — file scope so the pause's
 * restart can suppress the party (level_restore 35AF re-enters level_start,
 * not the party path). */
static int32_t _party_tried, _party_playing;
static int32_t _level_music_done; /* 0 not started, 1 fading, 2 playing */
static int32_t _level_start_fade; /* level_start 1000:303A: 0 idle, 1 fading */
static int32_t _menu_fade;        /* entry 0x0039: the room setup waits   */
static int32_t _attract_fade;     /* entry 0x001C: game_loop waits        */
static int32_t _score_fade;       /* check_highscore 0x43B6 waits         */
/* entry LAB_00CD's level arms: _level_reload re-runs the entry block for the
 * next level ([0x22]++); _level_skipped is level_run's need > members arm (the
 * level is not started); _end_page_up is draw_level_text's page. */
static int32_t _level_reload, _level_skipped, _end_page_up;
/* the level entry's disc probe (draw_status_indicator) and the post-level
 * newspaper (cd_file_op at entry 1000:00FF, the level we just finished) */
static int32_t _level_enter_pending, _disk_checked, _disk_up;
static int32_t _post_scene;
static uint32_t _post_scene_idx;
/* check_highscore's score screen between the end page and the post-level
 * step; _post_pending carries the step until the screen exits */
static int32_t _score_up, _post_pending;
/* [0x1809] = 0xFF: the pause/time-up "restart this game" arm (1000:390F /
 * 1000:3510), consumed by entry 1000:00C9 -> 1000:002B after the level's
 * lost-level caption and fades. */
static int32_t _restart_game;
/* 1000:35AF's prologue: the restart fades the play screen to black and
 * blocks on it (1000:35CA) before level_start re-enters; the reload and
 * the party screen wait in the FLOW_LEVEL branch until it finishes. */
static int32_t _restart_fade;
static int32_t _end_phase = END_CAPTION;
static uint32_t _end_hold;
static const uint8_t _end_black[768]; /* the 0x48BC ramp: zeroed = black */
/* the ending machine: 0 idle, 1 the 6/7 scene, 2 the disc probe, 3 the MAP */
static int32_t _end_phase, _end_disk_up;
static uint32_t _end_scene_index;

/* The level the walk is currently on: the menu selection feeds it
 * (frontend_selected_level = the matched codes.bin index, 1-based, minus
 * one — the [0x22] := [0x5F09] hand-off), and a loaded save overrides it
 * with its own +0x00 (entry:004C's POP AX). */
static uint32_t _level_current = 0u;

/* [0x13A4] lives in object.c (`object_action_latched`): it is the *only*
 * ACTION latch.  poll_input's tail (1000:8598..0x85AE) masks the ACTION bit
 * while it is set and clears it on the release (1000:85A9); pause_toggle
 * (1000:3538) is the only setter.  A charge never sets it, because
 * pause_toggle is blocked by [0x6F1D] (1000:3530) - that is what lets the
 * type-row fire column keep reaching 1000:EE07 while ACTION is held (D-80). */

/* [0x5BBA] != 0: the level's goal page (scroll_map 1000:9E51) is up. */
static int32_t _goal_page;
/* the page is being displayed (the flow is inside scroll_map's wait) - distinct
 * from _goal_page, which the level's entry sets long before the page shows. */
static int32_t _goal_showing;
/* scroll_map renders two frames and then polls input only; the port freezes
 * the [0xC] update gate for the page's lifetime (1 = it was ours to restore,
 * so a gate the claim already closed is left alone). */
static int32_t _goal_froze_updates;

/* [0x3954] (F10, the key list's id 9): a debug wait for key-up, key-down,
 * key-up with [0x5B12] suspended.  0 idle, 1 waiting for the release, 2 for
 * the press, 3 for the release. */
static int32_t _key_wait_stage;

/* [0x5E44]: the time-up dialog has been shown for this countdown. */
static int32_t _timeout_shown;

/* entry 1000:002B's opening: [0xF7E] := 0, then draw_status_indicator
 * (1000:1FEF) probes volume 0; only a missing volume draws its page.  Every
 * room entry (the attract exit, the pause's restart-game, the time-up
 * dialog) goes through 002B, so the probe runs once before the room loads. */
static int32_t _room_disc_pending, _room_disc_up;

/* DS:0x1989 - scan_id_list's key list (1000:386E): {scancode, event id}
 * pairs, terminated by 0xFF in the image.  The id's bit 7 means "run even
 * when the world gate [0xC] is closed" (F10); every other entry is skipped
 * while [0xC] == 0 (1000:38AD).  The ids dispatch through 202F:19A4:
 *
 *   1 1000:38C3  select the record `scancode - 0x3B` (F1..F8)
 *   2 1000:38DE  the previous record (0x410A with AX = -0x12)
 *   3 1000:38E4  the next record       (0x410A with AX = +0x12)
 *   4 1000:38EA  the 0x5BBC pause block (ESC)
 *   8 1000:3933  the action-icon step / home state (SPACE)
 *   9 1000:3954  the key wait (F10)
 */
static const uint8_t _key_list[][2] = {
    { 0x44, 0x89 }, { 0x01, 0x04 }, { 0x39, 0x08 }, { 0x33, 0x02 },
    { 0x34, 0x03 }, { 0x3B, 0x01 }, { 0x3C, 0x01 }, { 0x3D, 0x01 },
    { 0x3E, 0x01 }, { 0x3F, 0x01 }, { 0x40, 0x01 }, { 0x41, 0x01 },
    { 0x42, 0x01 },
};

/**
 * @brief Load the shipped defaults and bind the resource modules.
 *
 * @param osys the backend interface
 * @return 0 on success, 1 when a shipped default failed to load
 */
int32_t resources_init(const OSystem *osys)
{
    menu_init(osys);
    int32_t res_init_state =
        res_init(osys) || video_init(osys) || palette_init(osys) ||
        font_init(osys) || statusbar_init(osys) || scene_init(osys) ||
        level_init(osys) || entity_init(osys) || script_init(osys) ||
        audio_init(osys) || save_init(osys) || frontend_init(osys) ||
        attract_init(osys);

    if (res_init_state) osys->fatalError("module init failed");

    return res_init_state;
}

/** @brief The ISR body: the fixed per-tick work, in the original's order (the flip
 * apply happens inside video/palette's charge; the content modules are
 * real).  The entity update is NOT here: the
 * original runs it from the level's per-frame body (update_world
 * 1000:80E1, called once per display period from level_run), so it lives
 * in level_tick().
 */
static void _game_tick(void)
{
    video_tick();
    palette_tick();
    /* 1000:8B1E: the music tick (2 of 3) and the master-volume fade steps,
     * both per display period. */
    audio_tick();
    /* NOT statusbar_tick: the bar's tick+stamp is 1000:96AD, whose port
     * draws into the page after the world copy, so it runs from level_tick
     * (the foreground).  Calling it here as well ran the clock and the
     * slides at twice the tick rate. */
}
/**
 * @brief Accessor: the goal page is showing.
 *
 * @return The goal page is showing.
 */
int32_t gameflow_goal_page_up(void) { return _goal_showing; }
/** @brief Test seam: level_setup armed the goal/title page ([0x5BBA]).
 *
 * @return 1 when the page is pending its first play frame.
 */
int32_t gameflow_goal_armed(void) { return _goal_page; }
/**
 * @brief Accessor: a scripted key wait is active.
 *
 * @return A scripted key wait is active.
 */
int32_t gameflow_key_wait_active(void) { return _key_wait_stage != 0; }

/** @brief one tick of 1000:3954's three wait loops; returns 1 while the wait owns the
 * input.
 *
 * @param in the input state
 * @return 1 while the wait owns the frame, 0 when it ends
 */
static int32_t _key_wait_step(const InputState *in)
{
    int32_t down = in->keyHeld != 0u || in->lastKey != 0u || in->keyScan != 0u;

    switch (_key_wait_stage) {
    case 1:
        if (!down) _key_wait_stage = 2;
        break;
    case 2:
        if (down) _key_wait_stage = 3;
        break;
    default:
        if (!down) {
            statusbar_set_active(1); /* 1000:396E */
            _key_wait_stage = 0;
        }
        break;
    }
    return 1;
}

/**
 * @brief test seam: the pause/time-up RESTART THIS GAME arm set [0x1809] = 0xFF.
 *
 * @return 1 when a restart is pending, 0 otherwise
 */
int32_t gameflow_restart_pending(void) { return _restart_game; }

/**
 * @brief test seam: the restart left the party screen pending (1000:3073).
 *
 * @return 1 when the party screen is still pending, 0 otherwise
 */
int32_t gameflow_party_pending(void) { return !_party_tried; }

/** @brief level_setup's title-page arm (1000:316A -> level_reset_view 1000:3185,
 * `1000:31BD MOV word ptr [0x5BBA],0x180C`).
 *
 * level_setup runs on every entry, the restart included: 1000:35AF jumps back
 * to level_start 1000:3032, which falls through to level_run 1000:3054 and its
 * 307D CALL level_setup.  So the level-title page is shown by the first play
 * frame each time, not only on a fresh load.
 */
static void _level_publish_goal(void)
{
    const LevelData *g = level_data();

    _goal_page = !_level_skipped && g && g->goal[0] != 0;
    _goal_showing = 0;
    _goal_froze_updates = 0;
    if (_goal_page) statusbar_set_active(0);
}

/** @brief level_restore (1000:35AF): [0x11FC] := [0x1970] — the members the level
 * started with — then JMP level_start (1000:3032), which re-picks the time,
 * re-runs present_frames_75 + music_stop and re-enters the level (the party
 * screen runs again, 1000:3073).  The port reloads the record (the header is
 * the same file record; the original skips the read) and re-spawns the map's
 * entities like the level entry, then puts the members snapshot back.
 */
static void _level_restart_finish(void)
{
    /* level_start 1000:303A/303D: present_frames_75 then music_stop.  The
     * FLOW_LEVEL branch normally runs them; the restart jumps straight to
     * level_start, so start the audio fade here and let that branch wait it
     * out before the party.  Skipping it leaves [0x40BB] set, and the
     * party's play_tune loads a new image (MT-32 resets its voices) while
     * music_resume stays gated off — silence for the rest of the level. */
    _level_music_done = 0;
    _level_start_fade = 1;
    audio_fade_out(); /* 1000:303A */
    if (level_load(_level_current) == 0) {
        const LevelData *ld = level_data();
        entity_spawn_from_map(level_map(), ld->style, ld->entityLife);
        level_restore_members();
    }
    /* 1000:307D CALL level_setup: its 1000:31BD arm runs again after a
     * restart, so the title page is re-armed like a fresh load. */
    _level_publish_goal();
    /* 1000:3073: level_start tests [0x34] and plays the party screen when it
     * is zero.  A restart is not the RESTORE GAME path (the only writer of
     * 0xFF, 1000:008A), so the party runs again - live-confirmed in the
     * original (re_history.md, 2026-11-02). */
    _party_tried = 0;
    _party_playing = 0;
}

/** @brief 1000:35AF's prologue: park the bar, fade the play screen to black
 * (div 0x23) and stop there.  `gameflow_restart_fade_tick` does the reload
 * once the DAC is black, which is where 1000:35CA blocks.
 */
static void _level_restart(void)
{
    statusbar_set_active(0); /* 1000:35B6: [0x5B12] = 0 */
    palette_build_fade(0u, 0x100u, 0x23u, _end_black); /* 1000:35C7 */
    _restart_fade = 1;
}

/** @brief 1000:35CA `wait_palette_fade_done`: 1 while the restart's fade
 * runs, 0 when it is over (the level has then been reloaded).
 *
 * @return 1 while the fade is still stepping, 0 otherwise
 */
int32_t gameflow_restart_fade_tick(void)
{
    if (!_restart_fade) return 0;
    if (palette_fade_active()) return 1;
    _restart_fade = 0;
    _level_restart_finish();
    return 0;
}

/** @brief The briefing's YOUR SCORE IS value: the low word of [0x592]/[0x594],
 * which score_prepare maintains across levels.
 *
 * @return the party score
 */
uint32_t gameflow_party_score(void) { return score_total_lo(); }

/** @brief The briefing's code line (0x1189): load_level (1000:30BC) copies the level
 * record's author there (1000:312C), and level_intro_screen draws it.
 *
 * @return the pointer, or NULL when unavailable
 */
const char *gameflow_party_tribe(void)
{
    const LevelData *d = level_data();

    return d ? d->author : "";
}

/** @brief The title trio (1000:001C / 1000:0039): play_tune, music_resume,
 * timing_loop, with [0xF7E] = 0 as the entry sets it before the calls. */
static void _audio_title_cycle(void)
{
    res_set_disc(0u);
    audio_play_tune(audio_tune_title);
    audio_music_resume();
    audio_fade_in();
}

/** @brief level_status_update's music (1000:3267): the level's lettered tune
 * when the MUSIC board is on, the effects bank when it is off. */
static void _level_music(void)
{
    if (frontend_option(FRONTEND_OPT_MUSIC) == 0) {
        audio_play_level_tune(_level_current + 1u); /* 1000:327F */
        audio_music_resume();
        audio_fade_in();
    } else {
        audio_play_tune(audio_tune_humfx); /* 1000:328B */
        audio_fade_in();
        audio_sfx(0x0Eu); /* dropped for the AdLib driver id (1000:83D3) */
    }
}

/**
 * @brief Enter the frontend room.
 *
 * @return the next flow state
 */
static GameFlow _room_enter(void)
{
    _room_disc_pending = 1;
    _menu_fade = 0; /* the trio + the setup wait run in FLOW_MENU */
    return FLOW_MENU;
}

/** @brief dispatch_by_result_a167 (1000:38EA): the pause menu's five 0-based
 * results.  Returns the flow the action leaves (FLOW_MENU for RESTART THIS
 * GAME) and sets *quit for QUIT THIS GAME.
 *
 * @param choice the selected entry
 * @param quit the quit flag
 * @return the next flow state
 */
GameFlow gameflow_pause_action(int32_t choice, int32_t *quit)
{
    switch (choice) {
    case 0: /* continue            */ break;
    case 1: /* restart this level  */ _level_restart(); break;
    case 2: /* restart this game   */
        /* 1000:390F..1000:3923: [0x138E] = 0 (lost), [0x1809] = 0xFF and
         * [0x1979] = 2.  State 2 of the pending table (0x197B) is
         * 1000:36D7 (returns 0), so the play loop exits through the
         * "YOUR TRIBE IS EXTINCT!!" caption and its fades; the entry
         * machine's [0x1809] arm (1000:00C9 -> 1000:002B) then restarts. */
        level_set_end(0);
        _restart_game = 1;
        break;
    case 3: /* save game           */
        (void)save_write_slot(menu_slot(), _level_current + 1u,
                              statusbar_time_word(), level_tribe_members());
        break;
    case 4: /* quit this game      */ *quit = 1; break;
    }
    return FLOW_LEVEL;
}

/** @brief 1000:386E.  Returns 1 when an entry ran (a non-bit7 entry whose gate [0xC]
 * is closed returns 0 before the dispatch, 1000:38AD).
 *
 * @param in the input state
 * @return 1 when an entry ran, 0 otherwise
 */
int32_t gameflow_key_list(const InputState *in)
{
    if (!in) return 0;
    if (_key_wait_stage) return _key_wait_step(in); /* 1000:3954 */
    /* 1000:386E's first test: a pending event skips the whole key walk. */
    if (object_action_pending() != 0u) return 0;
    if (in->keyScan == 0u) return 0;
    for (uint32_t i = 0; i < sizeof _key_list / sizeof _key_list[0]; i++) {
        uint32_t id = _key_list[i][1];

        if (_key_list[i][0] != in->keyScan) continue;
        if (!(id & 0x80u) && !level_updates()) return 0; /* 1000:38AD */
        switch (id & 0x7Fu) {
        case 1: /* F1..F8     */
            (void)object_select_by_index((uint32_t)in->keyScan - 0x3Bu);
            return 1; /* 1000:38C3  */
        case 2: (void)object_cycle_entity(-1); return 1; /* ','        */
        case 3: (void)object_cycle_entity(1); return 1; /* '.'        */
        case 4: /* ESC        */
            if (!menu_active() && !level_ended())
                menu_open_pause(); /* 1000:38EA  */
            return 1;
        case 8: (void)object_space_action(); return 1; /* SPACE      */
        case 9: /* F10        */
            statusbar_set_active(0); /* 1000:3954  */
            _key_wait_stage = 1;
            return 1;
        default: return 0;
        }
    }
    return 0;
}

/** @brief input_dispatch_loop 34F9's inline time-up dispatch: 0 restart this level
 * (level_restore), 1 restart this game (the room), 2 quit.
 *
 * @param choice the selected entry
 * @param quit the quit flag
 * @return the next flow state
 */
GameFlow gameflow_time_up_action(int32_t choice, int32_t *quit)
{
    switch (choice) {
    case 0: _level_restart(); break;
    case 1:
        /* 1000:3510/1000:3515: [0x138E] = 0, [0x1809] = 0xFF (DEC AX keeps
         * the restart arm) - the same lost-level caption + fades as the
         * pause menu's result 2. */
        level_set_end(0);
        _restart_game = 1;
        break;
    case 2: *quit = 1; break;
    }
    return FLOW_LEVEL;
}

/** @brief entry 14E6/14FF: the post-level newspaper.  A lost level goes to the
 * ending ([0x138E]==0 at 14E6); a won one plays its header +1 index unless
 * that byte is negative (0xFF = none; cd_file_op's own CMP AL,0x80 path).
 *
 * @param won the won flag
 * @param introIndex the intro scene index
 * @return the newspaper index, or 0xFFFF when none
 */
uint32_t gameflow_post_level_scene(int32_t won, uint32_t introIndex)
{
    if (!won) return 0xFFFFu;
    if (introIndex & 0x80u) return 0xFFFFu;
    return introIndex;
}

/** @brief entry 150C/1512: [0x1e]=7 when the levels run out (COMPLETE); the lost
 * path reaches 1512 with [0x1e]=6 (the restart-game arm set it at 14BE).
 *
 * @param won the won flag
 * @return the ending scene index (6 or 7)
 */
uint32_t gameflow_ending_scene(int32_t won) { return won ? 7u : 6u; }

/** @brief level_run 3060/3062: load_level leaves [0x17FB] (the humans needed) and
 * [0x11FC] (the tribe); need > members takes the briefing's GAME OVER
 * variant and the level is NOT started.
 *
 * @param need the required count
 * @param members the member count
 * @return 1 when the level needs more members, 0 otherwise
 */
int32_t gameflow_tribe_too_small(uint32_t need, uint32_t members)
{ return need > members; }

/** @brief entry LAB_00CD's post-level step: [0x22]++ / [0x26]-- (0xA0 - [0x22]).
 * While the window has levels left the PLAY arm runs again — the next level
 * of the 160.  When they run out, COMPLETE: cd_file_op([0x1E]) (the ending
 * scene), draw_status_indicator, menu_loop (the MAP screen), then the
 * attract.  The ending/MAP arm is game_main's FLOW_ENDING (scene 6/7, the
 * disc-0 probe, map_begin/map_tick).  Known deviations: port_deviations.md.
 *
 * @param levelCurrent the current level number
 * @return the next flow state
 */
GameFlow gameflow_after_level(uint32_t *levelCurrent)
{
    if (*levelCurrent + 1u < 160u) {
        (*levelCurrent)++;
        return FLOW_LEVEL;
    }
    return FLOW_ATTRACT;
}

/**
 * @brief --- the flow state machine ------------------------------------------
 *
 * @return the current level
 */
uint32_t gameflow_level_for_probe(void) { return _level_current; }

/**
 * @brief One flow step: run the current state and return the next.
 *
 * @param state the flow state
 * @param input the input state
 * @return the next flow state
 */
GameFlow gameflow_step(GameFlow state, const InputState *input)
{
    switch (state) {
    case FLOW_ATTRACT:
        /* game_loop 1000:131E never returns normally — the boot intro
         * (played via scene_play_boot_intro, game_main's hook above) hands
         * into the four-effect cycle, whose only exit is attract_exit
         * 1000:13E9: the action key or any held key -> the 13B2 escape,
         * whose final fade completes inside attract_tick.  Done = the
         * hand-off to the front-end room (the original's entry LAB_002B).
         */
        if (attract_tick(input)) return FLOW_ATTRACT;
        /* attract_exit 1000:13E9 -> the 13B2 escape -> RET to entry LAB_002B:
         * the room's setup runs again.  Without this the front-end keeps the
         * previous exit phase and hands straight back to the party/level
         * (the ending -> attract -> exit path showed exactly that). */
        return _room_enter();

    case FLOW_MENU:
        if (_room_disc_pending) {
            res_set_disc(0u); /* entry 002B: [0xF7E]=0 */
            if (!_room_disc_up) _room_disc_up = level_disk_status_begin(0u);
            if (_room_disc_up) {
                if (level_disk_status_tick(input)) return FLOW_MENU;
                _room_disc_up = 0;
            }
            _room_disc_pending = 0;
            /* 1000:0039-0042: play_tune(title) / music_resume / timing_loop
             * before setup_offscreen_buffers (1000:0048). */
            _audio_title_cycle();
            _menu_fade = 1;
        }
        if (_menu_fade) {
            if (audio_fade_busy()) return FLOW_MENU; /* timing_loop */
            _menu_fade = 0;
            frontend_restart(); /* 1000:0048 */
        }
        /* The front-end room (the walkable level-select): frontend_tick
         * runs the original's per-frame web — the walk on the rails, the
         * action key's option cycles, the name/code screen's key table.
         * The room's exits are the walk off the right edge (x=0x280) or
         * Enter with a codes.bin match; both fade out and hand the level
         * to the flow (entry's PUSH AX at 004B / POP AX at 00BA move
         * [0x5F09] into [0x22] — the level counter here).  ESC stays in
         * the room (it clears the typed name; the decode settled the
         * stub-era ESC-to-attract guess was wrong). */
        if (frontend_tick(input)) return FLOW_MENU;
        _level_current = frontend_selected_level() - 1u;
        /* entry:004C: the room's RESTORE GAME counter ([0x5E6F], carried
         * out in BL by the walk-out exit) loads the slot.  A hit pushes the
         * save's +0x00 level through the entry stack to [0x22] — it
         * OVERRIDES the room's code match — and the header seeds
         * level_load's time/members.  Slot 0 (GAME OFF) resets the state;
         * the 1..160 bound only rejects a corrupt saved id. */
        if (save_load_slot(frontend_option(FRONTEND_OPT_GAME))) {
            uint32_t lv = save_level();
            if (lv >= 1u && lv <= 160u) _level_current = lv - 1u;
        }
        return FLOW_LEVEL;

    case FLOW_LEVEL:
        /* The level spine and the party pre-screen both live in game_main's
         * main-loop FLOW_LEVEL branch (the load + the tick gate are there). */
        return FLOW_LEVEL;

    case FLOW_ENDING:
        /* handled by game_main (the ending scene, the disc probe, the MAP) */
        return FLOW_ENDING;
    }
    return state;
}

/** @brief entry's post-level step after check_highscore: the finished level's
 * newspaper (1000:00FF), the lost ending (14E6, scene 6), or the next
 * level / COMPLETE (7).
 *
 * @param state the flow state
 * @return the next flow state
 */
static GameFlow _post_level_step(GameFlow state)
{
    const LevelData *lv = level_data();

    /* entry 1000:00C9: [0x1809] != 0 takes the restart-game path
     * (1000:002B: [0xF7E] = 0, [0x34] = 0, the front-end room) instead of
     * the next level / the ending. */
    if (_restart_game) {
        _restart_game = 0;
        save_load_slot(0u);
        return _room_enter();
    }
    uint32_t cut =
        lv ? gameflow_post_level_scene(level_outcome(), lv->introIndex)
           : 0xFFFFu;
    if (cut != 0xFFFFu) {
        _post_scene_idx = cut;
        scene_reset_intro();
        _post_scene = 1;
        return state;
    }
    if (!level_outcome()) {
        _end_scene_index = gameflow_ending_scene(0);
        _end_phase = 0;
        return FLOW_ENDING;
    }
    state = gameflow_after_level(&_level_current);
    _level_reload = (state == FLOW_LEVEL);
    if (state != FLOW_LEVEL) {
        _end_scene_index = gameflow_ending_scene(1);
        _end_phase = 0;
        return FLOW_ENDING;
    }
    return state;
}

/**
 * @brief One foreground flow frame; 1 = present, 0 = quit.
 *
 * @param osys the backend interface
 * @param state the flow state
 * @param last_tick the last presented tick
 * @param prev the previous value
 * @param quit_requested the quit flag (in/out)
 * @return 1 to present the frame, 0 to quit
 */
static int32_t _game_frame(const OSystem *osys, GameFlow *state,
                           uint32_t *last_tick, GameFlow prev,
                           int32_t *quit_requested)
{
    /* the tick gate: one flow step per tick boundary, exactly like the
         * original's foreground spinning on the ISR counter [0x5AC8].  The
         * host display's refresh rate must not pace this loop. */
    osys->tickSync(last_tick);

    InputState input;
    int32_t rawFire;

    osys->eventsPoll(&input);
    rawFire = (input.joyBits & 0x80u) != 0;
    /* 1000:8598-0x85AE (poll_input's tail): while [0x13A4] is set the
         * fire bit is masked out of [0x5ACE]; the release clears the latch
         * (1000:85A9).  One flag, owned by object.c.  The event-4 charge is a
         * blocking loop in the original and never polls, so its update_world
         * keeps the press frame's fire; do not mask it here. */
    if (object_action_latched() && !object_charge_active()) {
        if (rawFire) input.joyBits &= 0x0Fu;
        else object_set_action_latched(0);
    }
    scene_note_input(&input); /* the waits read this tick's *state */
    level_note_input(&input); /* 1000:8526: the play loop's [0x5ACE] */
    if (input.quit) return 0;

    /* the attract phase plays the boot intro first (the original's
         * boot chain runs once at power-on); then gameflow_step runs the
         * real attract cycle — game_loop's four effects — until the exit */
    if (*state == FLOW_ATTRACT && scene_play_boot_intro())
        return 1; /* the intro's rendered frame */
    /* entry 0x001C's timing_loop: the attract waits for the title fade */
    if (*state == FLOW_ATTRACT && _attract_fade) {
        if (audio_fade_busy()) return 1;
        _attract_fade = 0;
    }

    *state = gameflow_step(*state, &input);
    if (*state == FLOW_LEVEL) {
        /* 1000:35AF's restart prologue blocks on the fade to black before
             * level_start re-enters; hold the whole branch while it runs. */
        if (gameflow_restart_fade_tick()) return 1;
        /* entering the level (entry 14CD): draw_status_indicator's disc
             * probe, then level_run's header/map load and the party's
             * fresh-load screen (307A).  The .ANI newspaper is NOT here: it
             * is cd_file_op at entry 00FF, after the level. */
        int32_t entered = 0;
        if (prev != FLOW_LEVEL || _level_reload) {
            _level_reload = 0;
            _level_enter_pending = 1;
            _end_page_up = 0;
            _restart_game = 0;
            _level_skipped = 0;
            _party_tried = 0;
            _party_playing = 0;
            _post_scene = 0;
            _disk_checked = 0;
            _disk_up = 0;
            _level_music_done = 0;
            _level_start_fade = 0; /* level_start 1000:303A starts it below */
        }
        /* entry 14CD: draw_status_indicator (1000:1FEF) before level_run;
             * only a missing volume draws the prompt. */
        if (!_disk_checked) {
            if (!_disk_up) _disk_up = level_disk_status_begin(1u);
            if (_disk_up) {
                if (level_disk_status_tick(&input)) return 1;
                _disk_up = 0;
            }
            _disk_checked = 1;
        }
        /* level_start 1000:303A/303D: present_frames_75 then music_stop.
         * The original blocks inside the fade-out, so the level load and the
         * party screen follow it; the fade-out also clears the [0x40BA]
         * latch, which is what lets the party's fade-in run. */
        if (_level_start_fade == 0) {
            _level_start_fade = 1;
            audio_fade_out();
        }
        if (_level_start_fade == 1) {
            if (audio_fade_busy()) return 1;
            audio_music_stop();
            _level_start_fade = 2;
        }
        if (_level_enter_pending) {
            _level_enter_pending = 0;
            entered = 1;
            if (level_load(_level_current) == 0) {
                /* load_terrain_b (1000:5168): the level's own map
                     * spawns/joins the entities, once per load */
                const LevelData *ld = level_data();
                entity_spawn_from_map(level_map(), ld->style, ld->entityLife);
                /* entry:004C / load_level_record_a+_b: the save's *state
                     * ranges replace the level's fresh spawn. */
                if (save_present()) save_restore_state();
                /* level_run 3060/3062: a tribe smaller than the level
                     * needs — party_intro_set_game_over plays the briefing's
                     * GAME OVER variant — and the level is skipped by the
                     * entry *state machine's next-level arm. */
                _level_skipped =
                    gameflow_tribe_too_small(level_required(), level_members());
            }
            /* level_setup's title arm (1000:31BD): [0x5BBA] := the record's
             * goal text; the first play frame shows it (scroll_map). */
            _level_publish_goal();
        }
        const LevelData *lv = level_data();
        if (lv && !_party_tried) {
            /* level_run's values: the level letter (the [0x1f41]
                 * 1-based), the need ([0x17fb]), the tribe members
                 * ([0x11fc] = 12/10/8 by the LEVEL board, or the save's
                 * +0x0B), the score ([0x592], fresh load = 0) and the
                 * briefing's code line ([0x1189], load_level's author
                 * string).  A no-data run skips (start returns 0). */
            _party_tried = 1;
            /* level_run 3073: [0x34] != 0 takes the JNZ 307D path and
                 * skips the fresh-load party screen entirely. */
            if (!save_present()) {
                /* level_intro_screen's values: the level number
                     * ([0x1F41]), the humans needed ([0x17FB] = the map's
                     * 0xAA count — NOT the record's goal-count byte), the
                     * tribe ([0x11FC]) and the score ([0x592]; the port has
                     * no running score yet — 0, documented). */
                party_intro_set_game_over(_level_skipped);
                party_intro_set_values(_level_current + 1u, level_required(),
                                       level_tribe_members(),
                                       gameflow_party_score(),
                                       gameflow_party_tribe());
                _party_playing = party_intro_start();
            }
        }
        /* level_run 30B4: [0x34] := 0 once the level has started; a
             * later restart then takes the LEVEL board's time. */
        if (entered) save_consume();
        if (!_party_playing && _level_music_done == 0 && lv) {
            _level_music_done = 1;
            _level_music(); /* 1000:30A8 -> 1000:3267 */
        }
        if (_level_music_done == 1) {
            /* level_status_update's timing_loop: the world waits for the
             * fade-in, as the original's blocking call does. */
            if (audio_fade_busy()) return 1;
            _level_music_done = 2;
        }
        if (_party_playing) {
            if (party_intro_tick(&input)) return 1; /* the screen plays      */
            _party_playing = 0; /* its exit finished      */
        } else if (_score_up) {
            /* check_highscore's score/record screen (1000:41AE) */
            if (score_tick(&input)) return 1;
            _score_up = 0;
            audio_fade_out(); /* 1000:43B6 */
            _score_fade = 1;
        } else if (_score_fade) {
            /* 1000:43B6-43B9: present_frames_75 blocks before the post
             * scene; `_score_fade` is the port's non-blocking hold. */
            if (audio_fade_busy()) return 1;
            _score_fade = 0;
            if (_post_pending) {
                _post_pending = 0;
                *state = _post_level_step(*state);
            }
        } else if (_post_scene) {
            /* entry 1000:00FF: cd_file_op([0x17EF]) — the newspaper for
                 * the level just finished, after check_highscore. */
            if (scene_play_level_intro(_post_scene_idx)) return 1;
            _post_scene = 0;
            *state = gameflow_after_level(&_level_current);
            _level_reload = (*state == FLOW_LEVEL);
            if (*state != FLOW_LEVEL) {
                _end_scene_index = gameflow_ending_scene(1); /* 150C */
                *state = FLOW_ENDING;
                _end_phase = 0;
            }
        } else if (_level_skipped) {
            /* level_run 3065 + entry 1000:00DC: [0x1809] takes the
                 * restart-game path (1000:002B: [0xF7E]=0, [0x34]=0, the
                 * room), not the next level */
            frontend_restart();
            save_load_slot(0u);
            *state = FLOW_MENU;
            _level_reload = 0;
        } else if (level_loading_screen()) {
            /* load_sprite_pal (1000:1DC0): the LOADING screen, between
                 * the party and the level's fade-in (a skipped level never
                 * reaches it — level_run 3065 returns first) */
            level_loading_tick();
        } else if (_end_page_up) {
            /* draw_level_text (1000:33A3).  [0x138E] selects "WELL DONE!
                 * YOU FINISHED LEVEL nnn" or "YOUR TRIBE IS EXTINCT!!" and the
                 * box waits for a key or ACTION; then the palette chain runs
                 * (0x33E2 seal + gradient, 0x33FD fade 0..0xBF div 0x1E with
                 * a wait, 0x340F 0x23-frame hold, 0x3419 fade 0x100 to black
                 * div 0x28), the bar is parked (0x342B) and the world is
                 * re-rendered (0x344C).  check_highscore (0x41AE) then draws
                 * the card, but only when [0x138E] != 0; a lost level jumps to
                 * 0x438A and never opens it.  entry's post-level step (the
                 * newspaper, the ending, or the next level) follows. */
            if (_end_phase == END_CAPTION) {
                char text[48]; /* the widest column: FRE's two-line caption
                                * is 42 bytes with a three-digit level */

                level_tick();
                if (level_outcome())
                    snprintf(text, sizeof text,
                             game_strs[GAME_LANG_EN]
                                      [GAME_STR_GAMEFLOW_LEVEL_DONE],
                             _level_current + 1u);
                else
                    snprintf(text, sizeof text, "%s",
                             game_strs[GAME_LANG_EN][GAME_STR_GAMEFLOW_EXTINCT]);
                textpage_draw(osys->videoPage(0), text, -1, 0u, 0);
                if (input.lastKey || (input.joyBits & 0x80u)) {
                    /* scroll_map's exit 0xA0E1 re-renders the page twice:
                         * the caption box and its text are gone before the
                         * fades start (the original's grey/gradient frames
                         * show only the world). */
                    level_redraw_page();
                    /* 0x33E2/0x33F2: seal the level palette to grey and
                         * load the shipped gradient into 0x80..0xBF. */
                    level_end_palette();
                    /* 0x33FD: fade the DAC's 0..0xBF to that palette. */
                    palette_build_fade(0u, 0xC0u, 0x1Eu, level_palette());
                    _end_phase = END_GREY_FADE;
                }
            } else if (_end_phase == END_GREY_FADE) {
                if (!palette_fade_active()) { /* 0x340C wait */
                    _end_hold = 0x23u; /* 0x340F */
                    _end_phase = END_HOLD;
                }
            } else if (_end_phase == END_HOLD) {
                if (_end_hold > 0u) {
                    _end_hold--;
                } else {
                    /* 0x3419: every entry to the zeroed 0x48BC ramp. */
                    palette_build_fade(0u, 0x100u, 0x28u, _end_black);
                    _end_phase = END_BLACK_FADE;
                }
            } else if (!palette_fade_active()) { /* END_BLACK_FADE */
                /* 0x342B/0x344C: park the split, re-render the world. */
                statusbar_hide();
                level_end_redraw_page(); /* 0x3437: the 0x78 clamp */
                _end_phase = END_CAPTION;
                _end_page_up = 0;
                if (!level_outcome()) {
                    /* 1000:41B5 -> 1000:438A: a lost level skips the score
                     * card but still runs present_frames_75 + music_stop
                     * (0x43B6/0x43B9) before entry's post-level step.  The
                     * port used to jump straight to the step, leaving the
                     * level record live; a RESTART THIS GAME then loaded
                     * mus\title over it and the old record played the new
                     * image (the wrong menu tune). */
                    _post_pending = 1;
                    audio_fade_out(); /* 0x43B6 */
                    _score_fade = 1;
                } else {
                    _post_pending = 1;
                    _score_up = score_begin(score_level_score());
                    if (!_score_up) {
                        _post_pending = 0;
                        *state = _post_level_step(*state);
                    }
                }
            }
        } else if (level_starting()) {
            /* level_fade_in (1000:3334): after the party and the intro,
                 * before the first play frame — the foreground blocks on the
                 * fades, so the goal page and the play loop wait here. */
            level_begin();
            level_tick();
            return 1;
        } else if (_goal_page && lv) {
            /* scroll_map (1000:9E51): two rendered frames, the box, then
                 * a loop that only runs 1000:8B56 + 1000:8526 (poll_input)
                 * and tests [0x48A3]/the fire bit - **no update_world**, so
                 * the world is frozen and the direction keys cannot move
                 * anyone.  The port freezes the [0xC] gate (level_tick still
                 * renders and keeps the ISR's palette rotation running, so
                 * the box is redrawn over each frame). */
            if (!_goal_froze_updates) {
                _goal_froze_updates = level_updates();
                level_set_updates(0);
            }
            _goal_showing = 1;
            statusbar_set_active(0);
            level_tick();
            textpage_draw(osys->videoPage(0), lv->goal, -1, 0u, 0);
            if (input.lastKey || (input.joyBits & 0x80u)) {
                _goal_page = 0;
                _goal_showing = 0;
                if (_goal_froze_updates) level_set_updates(1); /* 1000:9E9D */
                _goal_froze_updates = 0;
                statusbar_set_active(1);
            }
        } else {
            /* input_dispatch_loop (1000:349E): the pause gate (3523)
                 * then the world.  With a dialog up (A167) only the frozen
                 * frame is redrawn: menu_open_* clears the world gate, so
                 * update_world (marker, entity and object animations) stops;
                 * the countdown is suspended too (D-232). */
            if (gameflow_key_wait_active()) { /* 1000:3954 blocks the world */
                (void)gameflow_key_list(&input);
                return 1;
            }
            if (statusbar_time_up() && !_timeout_shown && !menu_active()) {
                _timeout_shown = 1;
                menu_open_time_up();
            }
            /* ACTION with no direction held runs the acting record's
                 * ACTION program (pause_toggle 1000:3523 -> script_op_dispatch
                 * 1000:3979); a dialog blocks it through [0x6F1D].  The
                 * pause block (202F:0x5BBC) is scan_id_list's ESC event (id
                 * 4 -> 1000:38EA), not ACTION. */
            /* pause_toggle (0x3523): a fresh fire press with no direction
                 * runs the ACTION program.  [0x42F0] is never set in the image,
                 * so the 0xE3F9 ACTION edge is dead and the same frame's
                 * command dispatch still sees the fire half of the row. */
            int32_t pressed =
                rawFire && !object_action_latched() && !object_charge_active();

            /* the original's order (1000:34B5 -> 0x34BD): update_world
                 * runs first - the record command dispatch inside it sees the
                 * press (the type row's fire half selects the turn) - and
                 * pause_toggle runs after it.  The event-4 loop blocks
                 * pause_toggle until it ends (1000:36DB). */
            level_tick(); /* 1000:34B5 update_world */
            if (!menu_active() && pressed && !(input.joyBits & 0x0Fu))
                object_action_run(); /* 1000:34BD pause_toggle */
            /* the latch is not assigned here: object_action_run sets it
                 * (1000:3538) and the release above clears it (1000:85A9). */
            if (level_end_holding()) {
                /* 1000:34E6: the play loop's exit hold - the end sound
                     * (0x83B7, AX = 0xC) and 15 vsyncs (0x8B56) on the frozen
                     * world, then draw_level_text's chain. */
                statusbar_set_active(0); /* [0x5B12] = 0: the clock stops */
                return 1;
            }
            if (level_ended()) {
                /* object_test_dispatch returned negative (1000:35D6):
                     * the play loop is over; [0x138E] keeps its outcome */
                _end_page_up = 1;
                _end_phase = END_CAPTION; /* draw_level_text's chain */
                statusbar_set_active(0); /* [0x5B12] = 0: the clock stops */
            }
            /* scan_id_list (1000:386E) runs after update_world and before
                 * the pending-event table. */
            if (!menu_active() && !level_ended() && !object_charge_active())
                (void)gameflow_key_list(&input); /* scan_id_list is blocked by
                                                  * the event-4 loop (1000:36DB) */
            if (menu_active()) {
                if (!menu_tick(&input)) {
                    int32_t wasTimeUp = menu_time_up();
                    *state = wasTimeUp ? gameflow_time_up_action(menu_result(),
                                                                 quit_requested)
                                       : gameflow_pause_action(menu_result(),
                                                               quit_requested);
                    menu_close();
                    if (wasTimeUp) _timeout_shown = 0;
                    if (!*quit_requested) statusbar_set_active(1);
                }
            }
            /* 1000:34C3: the [0x1979] dispatch, after scan_id_list and
                 * pause_toggle (both of which may have armed a code); event
                 * 4's charge loop then takes its per-frame share (1000:3702)
                 * with the world frame already updated.  A negative
                 * object_test_dispatch (level_ended) leaves the play loop
                 * before the dispatch (JS 0x34E6). */
            if (!level_ended()) {
                object_pending_run();
                (void)object_charge_tick();
            }
            if (*quit_requested) return 0;
        }
    }

    if (*state == FLOW_ENDING) {
        /* entry 150C/1512: cd_file_op(6 lost / 7 won), then
             * draw_status_indicator([0xF7E]=0) and menu_loop (the MAP) */
        if (_end_phase == 0) {
            scene_reset_intro();
            _end_phase = 1;
        }
        if (_end_phase == 1) {
            if (scene_play_level_intro(_end_scene_index)) return 1;
            _end_phase = 2;
        }
        if (_end_phase == 2) {
            if (!_end_disk_up) {
                _end_disk_up = level_disk_status_begin(0u);
                if (!_end_disk_up) _end_phase = 3;
            }
            if (_end_disk_up) {
                if (level_disk_status_tick(&input)) return 1;
                _end_disk_up = 0;
                _end_phase = 3;
            }
            if (_end_phase == 3) map_begin();
        }
        if (_end_phase == 3) {
            if (map_tick(&input)) return 1;
            _end_phase = 0;
            _audio_title_cycle(); /* entry 0x001C's trio before game_loop */
            _attract_fade = 1;    /* game_loop waits for its timing_loop */
            *state = FLOW_ATTRACT;
        }
        return 1;
    }

    /* the intro/level draw into page 0; the CRTC-flip equivalent shows
         * the drawn page (the in-level page flip lands with the level
         * rendering) */
    return 1;
}

/**
 * @brief The application entry: boot, then the foreground flow loop.
 *
 * @param osys the backend interface
 * @return 0 when the quit request ends the loop
 */
int32_t game_main(const OSystem *osys)
{
    // Initialize the video mode
    osys->videoSetMode(VIDEO_MODE_13H); /* bios_set_mode_13h (1000:88fb) */

    // Initialize resources
    if (resources_init(osys)) return 1;

    /* driver_reset (1000:84DE): install the resident driver once, before the
     * first tick and the boot tune. */
    audio_install();

    osys->timerStart(GAME_TICK_HZ, _game_tick);

    GameFlow state = FLOW_ATTRACT, prev = FLOW_ATTRACT;
    uint32_t last_tick = 0;
    int32_t quitRequested = 0;
    while (1) {
        if (!_game_frame(osys, &state, &last_tick, prev, &quitRequested)) break;
        osys->videoPresent(0);
        prev = state;
    }

    osys->audioStop();
    return 0;
}
