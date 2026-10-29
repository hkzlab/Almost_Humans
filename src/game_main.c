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
#include <stddef.h>

int resources_init(const OSystem *osys) {
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
    statusbar_tick();
}

/* --- the flow state machine ------------------------------------------ */

/* The level the walk is currently on: the menu selection feeds it
 * (frontend_selectedLevel = the matched codes.bin index, 1-based, minus
 * one — the [0x22] := [0x5F09] hand-off). */
static unsigned level_current = 0u;

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
    static int partyTried, partyPlaying;
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
            if (prev != FLOW_LEVEL) {
                partyTried = 0; partyPlaying = 0;
                if (level_load(level_current) == 0) {
                    /* load_terrain_b (1000:5168): the level's own map
                     * spawns/joins the entities, once per load */
                    const LevelData *ld = level_data();
                    entity_spawnFromMap(level_map(), ld->style,
                                        ld->entityLife);
                }
            }
            const LevelData *lv = level_data();
            if (lv && !partyTried) {
                /* level_run's values: the level letter (the [0x1f41]
                 * 1-based), the need ([0x17fb]), the tribe members
                 * ([0x11fc], level A's [live] = 12), the score ([0x592],
                 * fresh load = 0) and the tribe name (load_level's
                 * 0x1189).  A no-data run skips (start returns 0). */
                partyTried = 1;
                party_intro_setValues(level_current + 1u, lv->goalCount,
                                      12u, 0u, "DARWIN");
                partyPlaying = party_intro_start();
            }
            if (partyPlaying) {
                if (party_intro_tick(&input))
                    goto present;          /* the screen plays      */
                partyPlaying = 0;          /* its exit finished      */
            } else if (lv && scene_playLevelIntro(lv->introIndex)) {
                /* the intro is playing — the level waits */
            } else {
                level_tick();          /* the party done: the level runs */
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
