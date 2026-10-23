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
        audio_init(osys) || save_init(osys) || frontend_init(osys);

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

#define ATTRACT_DWELL_TICKS (5u * GAME_TICK_HZ)   /* 5 s after the boot intro */

static unsigned attract_dwell;

/* The level the walk is currently on: the menu selection feeds it (the
 * frontend stub hardcodes 0 until then). */
static unsigned level_current = 0u;

GameFlow gameflow_step(GameFlow state, const InputState *input)
{
    const unsigned fire = (input->joyBits & 0x80u) != 0;

    switch (state) {
    case FLOW_ATTRACT:
        /* The idle cycle: the boot intro (the gametek logo + the cards +
         * the scene) plays first via scene_playBootIntro, then the 5 s
         * dwell before the menu (the hi-score screens come later). */
        if (++attract_dwell >= ATTRACT_DWELL_TICKS) {
            attract_dwell = 0;
            return FLOW_MENU;
        }
        return FLOW_ATTRACT;

    case FLOW_MENU:
        /* The map/level-select room waits for the player: fire confirms
         * (the +0x5ACE bit-7 path), ESC drops back to attract. */
        if (fire) return FLOW_LEVEL;
        if (input->lastKey == 0x01u) return FLOW_ATTRACT;   /* ESC */
        return FLOW_MENU;

    case FLOW_LEVEL:
        /* The level is running (loading and the party pre-level screen are
         * M4 content; the 6-min countdown lives under level_init later). */
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
    for (;;) {
        /* the tick gate: one flow step per tick boundary, exactly like the
         * original's foreground spinning on the ISR counter [0x5AC8].  The
         * host display's refresh rate must not pace this loop. */
        osys->tickSync(&last_tick);

        InputState input;
        osys->eventsPoll(&input);
        if (input.quit) break;

        /* the attract phase plays the boot intro first (the original's
         * attract replays the intro); the dwell counts after it ends */
        if (state == FLOW_ATTRACT && scene_playBootIntro()) {
            osys->videoPresent(0);      /* the intro's rendered frame */
            prev = state;
            continue;
        }

        state = gameflow_step(state, &input);
        if (state == FLOW_LEVEL) {
            /* entering the level: the header + map load, then its intro
             * cutscene (cd_file_op); the level proper ticks after it */
            if (prev != FLOW_LEVEL) {
                if (level_load(level_current) == 0) {
                    /* load_terrain_b (1000:5168): the level's own map
                     * spawns/joins the entities, once per load */
                    const LevelData *ld = level_data();
                    entity_spawnFromMap(level_map(), ld->style,
                                        ld->entityLife);
                }
            }
            const LevelData *lv = level_data();
            if (lv && scene_playLevelIntro(lv->introIndex)) {
                /* the intro is playing — the level waits */
            } else {
                level_tick();          /* the intro done: the level runs */
            }
        }
        /* the intro/level draw into page 0; the CRTC-flip equivalent shows
         * the drawn page (the in-level page flip lands with the level
         * rendering) */
        osys->videoPresent(0);
        frontend_tick();                /* the menu/room content (stub)   */
        prev = state;
    }

    osys->audioStop();
    return 0;
}
