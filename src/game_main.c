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
 * tick boundary.  Stub state: modules are initialised and the flow shell
 * runs; nothing renders yet (M4 starts here).
 */
#include "game.h"
#include <stdio.h>
#include <stddef.h>

int resources_init(const OSystem *osys) {
    menu_init(osys);
    int res_init_state =
        res_init(osys) || video_init(osys) || palette_init(osys) ||
        font_init(osys) || statusbar_init(osys) || scene_init(osys) ||
        level_init(osys) || entity_init(osys) || script_init(osys) ||
        audio_init(osys) || save_init(osys) || frontend_init(osys) ||
        attract_init(osys);

    if (res_init_state) osys->fatalError("module init failed");

    return res_init_state;
}

/* The ISR body: the fixed per-tick work, in the original's order (the flip
 * apply happens inside video/palette's charge once they are real; the
 * content modules are stubs today).  The entity update is NOT here: the
 * original runs it from the level's per-frame body (update_world
 * 1000:80E1, called once per display period from level_run), so it lives
 * in level_tick(). */
static void game_tick(void)
{
    video_tick();
    palette_tick();
    /* NOT statusbar_tick: the bar's tick+stamp is 1000:96AD, whose port
     * draws into the page after the world copy, so it runs from level_tick
     * (the foreground).  Calling it here as well ran the clock and the
     * slides at twice the tick rate. */
}

/* --- the level's foreground: the pause gate + the actions ------------- */

/* level_run's fresh-load party state (307A) — file scope so the pause's
 * restart can suppress the party (level_restore 35AF re-enters level_start,
 * not the party path). */
static int partyTried, partyPlaying;
/* entry LAB_00CD's level arms: levelReload re-runs the entry block for the
 * next level ([0x22]++); levelSkipped is level_run's need > members arm (the
 * level is not started); endPageUp is draw_level_text's page. */
static int levelReload, levelSkipped, endPageUp;

/* The level the walk is currently on: the menu selection feeds it
 * (frontend_selectedLevel = the matched codes.bin index, 1-based, minus
 * one — the [0x22] := [0x5F09] hand-off), and a loaded save overrides it
 * with its own +0x00 (entry:004C's POP AX). */
static unsigned level_current = 0u;

/* The pause menu's press latch (pause_toggle 1000:3523 opens on a held
 * ACTION; the port requires a release between opens). */
static int menuPrevAction;

/* [0x5BBA] != 0: the level's goal page (scroll_map 1000:9E51) is up. */
static int goalPage;

/* [0x5E44]: the time-up dialog has been shown for this countdown. */
static int timeoutShown;

/* level_restore (1000:35AF): [0x11FC] := [0x1970] — the members the level
 * started with — then JMP level_start (1000:3032), which re-picks the time
 * and re-enters the level WITHOUT the party path.  The port reloads the
 * record (the header is the same file record; the original skips the read)
 * and re-spawns the map's entities like the level entry, then puts the
 * members snapshot back. */
static void level_restart(void)
{
    if (level_load(level_current) == 0) {
        const LevelData *ld = level_data();
        entity_spawnFromMap(level_map(), ld->style, ld->entityLife);
        level_restoreMembers();
    }
    partyTried = 1;
    partyPlaying = 0;
}

/* dispatch_by_result_a167 (1000:38EA): the pause menu's five 0-based
 * results.  Returns the flow the action leaves (FLOW_MENU for RESTART THIS
 * GAME) and sets *quit for QUIT THIS GAME. */
GameFlow gameflow_pauseAction(int choice, int *quit)
{
    switch (choice) {
    case 0:                                     /* continue            */
        break;
    case 1:                                     /* restart this level  */
        level_restart();
        break;
    case 2:                                     /* restart this game   */
        frontend_restart();
        save_loadSlot(0u);                      /* entry 002B: [0x34]=0 */
        return FLOW_MENU;
    case 3:                                     /* save game           */
        (void)save_writeSlot(menu_slot(), level_current + 1u,
                             statusbar_timeWord(), level_tribeMembers());
        break;
    case 4:                                     /* quit this game      */
        *quit = 1;
        break;
    }
    return FLOW_LEVEL;
}

/* input_dispatch_loop 34F9's inline time-up dispatch: 0 restart this level
 * (level_restore), 1 restart this game (the room), 2 quit. */
GameFlow gameflow_timeUpAction(int choice, int *quit)
{
    switch (choice) {
    case 0:
        level_restart();
        break;
    case 1:
        frontend_restart();
        save_loadSlot(0u);
        return FLOW_MENU;
    case 2:
        *quit = 1;
        break;
    }
    return FLOW_LEVEL;
}

/* level_run 3060/3062: load_level leaves [0x17FB] (the humans needed) and
 * [0x11FC] (the tribe); need > members takes the briefing's GAME OVER
 * variant and the level is NOT started. */
int gameflow_tribeTooSmall(unsigned need, unsigned members)
{
    return need > members;
}

/* entry LAB_00CD's post-level step: [0x22]++ / [0x26]-- (0xA0 - [0x22]).
 * While the window has levels left the PLAY arm runs again — the next level
 * of the 160.  When they run out, COMPLETE: cd_file_op([0x1E]) (the ending
 * scene), draw_status_indicator, menu_loop (the MAP screen), then the
 * attract.  Neither the ending nor the MAP screen is ported: STUBS — the
 * port goes straight to the attract, where game_loop's prologue restarts. */
GameFlow gameflow_afterLevel(unsigned *levelCurrent)
{
    if (*levelCurrent + 1u < 160u) {
        (*levelCurrent)++;
        return FLOW_LEVEL;
    }
    return FLOW_ATTRACT;
}

/* --- the flow state machine ------------------------------------------ */

unsigned gameflow_levelForProbe(void) { return level_current; }

GameFlow gameflow_step(GameFlow state, const InputState *input)
{
    switch (state) {
    case FLOW_ATTRACT:
        /* game_loop 1000:131E never returns normally — the boot intro
         * (played via scene_playBootIntro, game_main's hook above) hands
         * into the four-effect cycle, whose only exit is attract_exit
         * 1000:13E9: the action key or any held key -> the 13B2 escape,
         * whose final fade completes inside attract_tick.  Done = the
         * hand-off to the front-end room (the original's entry LAB_002B).
         */
        if (attract_tick(input)) return FLOW_ATTRACT;
        return FLOW_MENU;

    case FLOW_MENU:
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
        level_current = frontend_selectedLevel() - 1u;
        /* entry:004C: the room's RESTORE GAME counter ([0x5E6F], carried
         * out in BL by the walk-out exit) loads the slot.  A hit pushes the
         * save's +0x00 level through the entry stack to [0x22] — it
         * OVERRIDES the room's code match — and the header seeds
         * level_load's time/members.  Slot 0 (GAME OFF) resets the state;
         * the 1..160 bound only rejects a corrupt saved id. */
        if (save_loadSlot(frontend_option(FRONTEND_OPT_GAME))) {
            unsigned lv = save_level();
            if (lv >= 1u && lv <= 160u) level_current = lv - 1u;
        }
        return FLOW_LEVEL;

    case FLOW_LEVEL:
        /* The level spine and the party pre-screen both live in game_main's
         * main-loop FLOW_LEVEL branch (the load + the tick gate are there). */
        return FLOW_LEVEL;
    }
    return state;
}

int game_main(const OSystem *osys)
{
    // Initialize the video mode
    osys->videoSetMode();

    // Initialize resources
    if (resources_init(osys)) return 1;

    osys->timerStart(GAME_TICK_HZ, game_tick);

    GameFlow state = FLOW_ATTRACT, prev = FLOW_ATTRACT;
    unsigned last_tick = 0;
    int quitRequested = 0;
    for (;;) {
        /* the tick gate: one flow step per tick boundary, exactly like the
         * original's foreground spinning on the ISR counter [0x5AC8].  The
         * host display's refresh rate must not pace this loop. */
        osys->tickSync(&last_tick);

        InputState input;

        osys->eventsPoll(&input);
        scene_noteInput(&input);   /* the waits read this tick's state */
        if (input.quit) break;

        /* the attract phase plays the boot intro first (the original's
         * boot chain runs once at power-on); then gameflow_step runs the
         * real attract cycle — game_loop's four effects — until the exit */
        if (state == FLOW_ATTRACT && scene_playBootIntro()) {
            osys->videoPresent(0);      /* the intro's rendered frame */
            prev = state;
            continue;
        }

        state = gameflow_step(state, &input);
        if (state == FLOW_LEVEL) {
            /* entering the level: the header + map load, then the party's
             * pre-level screen (level_run's fresh-load 307a path — the
             * .ANI cutscene is not on this path, re_map.md); the level
             * proper ticks after both. */
            int entered = 0;
            if (prev != FLOW_LEVEL || levelReload) {
                levelReload = 0;
                endPageUp = 0;
                levelSkipped = 0;
                partyTried = 0; partyPlaying = 0;
                entered = 1;
                if (level_load(level_current) == 0) {
                    /* load_terrain_b (1000:5168): the level's own map
                     * spawns/joins the entities, once per load */
                    const LevelData *ld = level_data();
                    entity_spawnFromMap(level_map(), ld->style,
                                        ld->entityLife);
                    /* level_run 3060/3062: a tribe smaller than the level
                     * needs — the briefing's GAME OVER variant (not ported;
                     * the normal briefing stands in) and the level is
                     * skipped by the entry state machine's next-level arm. */
                    levelSkipped = gameflow_tribeTooSmall(level_required(),
                                                         level_members());
                }
                /* level_setup (1000:31BD): [0x5BBA] := the record's goal
                 * text; the first play frame shows it (scroll_map). */
                {
                    const LevelData *g = level_data();
                    goalPage = !levelSkipped && g && g->goal[0] != 0;
                    if (goalPage) statusbar_setActive(0);
                }
            }
            const LevelData *lv = level_data();
            if (lv && !partyTried) {
                /* level_run's values: the level letter (the [0x1f41]
                 * 1-based), the need ([0x17fb]), the tribe members
                 * ([0x11fc] = 12/10/8 by the LEVEL board, or the save's
                 * +0x0B), the score ([0x592], fresh load = 0) and the tribe
                 * name (load_level's 0x1189).  A no-data run skips (start
                 * returns 0). */
                partyTried = 1;
                /* level_run 3073: [0x34] != 0 takes the JNZ 307D path and
                 * skips the fresh-load party screen entirely. */
                if (!save_present()) {
                    party_intro_setValues(level_current + 1u, lv->goalCount,
                                          level_tribeMembers(), 0u, "DARWIN");
                    partyPlaying = party_intro_start();
                }
            }
            /* level_run 30B4: [0x34] := 0 once the level has started; a
             * later restart then takes the LEVEL board's time. */
            if (entered) save_consume();
            if (partyPlaying) {
                if (party_intro_tick(&input))
                    goto present;          /* the screen plays      */
                partyPlaying = 0;          /* its exit finished      */
            } else if (lv && !levelSkipped &&
                       scene_playLevelIntro(lv->introIndex)) {
                /* the .ANI cutscene (cd_file_op, ANIMATIONS-gated): the
                 * party screen -> the movie -> the level; a skipped level
                 * never reaches it (level_run 3065 JMPs out first) */
            } else if (levelSkipped) {
                /* level_run returned without the level (3065): the entry
                 * state machine's [0x22]++/[0x26]-- arm runs next tick. */
                state = gameflow_afterLevel(&level_current);
                levelReload = (state == FLOW_LEVEL);
            } else if (endPageUp) {
                /* draw_level_text (1000:33A3): [0x138E] selects "WELL DONE!
                 * YOU FINISHED LEVEL nnn" or "YOUR TRIBE IS EXTINCT!!"; the
                 * box waits for a key or ACTION.  check_highscore's score /
                 * record screen (1000:41AE) is NOT ported — STUB: the flow
                 * advances straight after the page. */
                char text[40];
                level_tick();
                if (level_outcome())
                    snprintf(text, sizeof text,
                             "WELL DONE! YOU FINISHED LEVEL %03u",
                             level_current + 1u);
                else
                    snprintf(text, sizeof text, "YOUR TRIBE IS EXTINCT!!");
                textpage_draw(osys->videoPage(0), text, -1, 0u, 0);
                if (input.lastKey || (input.joyBits & 0x80u)) {
                    endPageUp = 0;
                    state = gameflow_afterLevel(&level_current);
                    levelReload = (state == FLOW_LEVEL);
                }
            } else if (level_starting()) {
                /* level_fade_in (1000:3334): after the party and the intro,
                 * before the first play frame — the foreground blocks on the
                 * fades, so the goal page and the play loop wait here. */
                level_begin();
                level_tick();
                goto present;
            } else if (goalPage && lv) {
                /* scroll_map (1000:9E51): the level's goal page, the frame
                 * re-rendered behind it, [0x5B12]=0 while up (so the clock
                 * stands still after the start chain armed it); any key or
                 * ACTION dismisses it and [0x5BBA] is cleared. */
                statusbar_setActive(0);
                level_tick();
                textpage_draw(osys->videoPage(0), lv->goal, -1, 0u, 0);
                if (input.lastKey || (input.joyBits & 0x80u)) {
                    goalPage = 0;
                    statusbar_setActive(1);
                }
            } else {
                /* input_dispatch_loop (1000:349E): the pause gate (3523)
                 * then the world.  With a dialog up (A167) the world keeps
                 * animating behind it and the countdown is suspended. */
                if (statusbar_timeUp() && !timeoutShown && !menu_active()) {
                    timeoutShown = 1;
                    menu_openTimeUp();
                }
                if (!menu_active() && !menuPrevAction &&
                    (input.joyBits & 0x80u) && !(input.joyBits & 0x0Fu))
                    menu_openPause();
                menuPrevAction = (input.joyBits & 0x80u) != 0;

                level_tick();          /* the party done: the level runs */
                if (level_ended()) {
                    /* object_test_dispatch returned negative (1000:35D6):
                     * the play loop is over; [0x138E] keeps its outcome */
                    endPageUp = 1;
                    statusbar_setActive(0);
                }
                if (menu_active()) {
                    if (!menu_tick(&input)) {
                        int wasTimeUp = menu_timeUp();
                        state = wasTimeUp
                              ? gameflow_timeUpAction(menu_result(),
                                                      &quitRequested)
                              : gameflow_pauseAction(menu_result(),
                                                     &quitRequested);
                        menu_close();
                        if (wasTimeUp) timeoutShown = 0;
                        if (!quitRequested) statusbar_setActive(1);
                    }
                }
                if (quitRequested) break;
            }
        }
        /* the intro/level draw into page 0; the CRTC-flip equivalent shows
         * the drawn page (the in-level page flip lands with the level
         * rendering) */
present:
        osys->videoPresent(0);
        prev = state;
    }

    osys->audioStop();
    return 0;
}
