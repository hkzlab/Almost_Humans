/* main.c — the game core's entry: the original `entry` (1000:0000) re-read.
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
 * Stub state: the modules are initialised and the shell of the flow runs,
 * nothing renders yet (M4 starts here).
 */
#include "game.h"

int game_main(const OSystem *osys)
{
    osys->videoSetMode();

    if (res_init(osys) || video_init(osys) || palette_init(osys) ||
        font_init(osys) || statusbar_init(osys) || scene_init(osys) ||
        level_init(osys) || entity_init(osys) || script_init(osys) ||
        audio_init(osys) || save_init(osys) || frontend_init(osys))
        osys->fatalError("module init failed");

    /* TODO(M4): the intro sequence, then the level walk, then menu_loop's
     * event loop.  The heartbeat: the backend calls the tick below. */
    for (;;)
        frontend_tick();

    return 0;
}
