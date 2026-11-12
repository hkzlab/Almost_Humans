/* test_pan_input - menu_flag_tick's win pan blocks input_dispatch_loop.
 *
 * The original's pan (1000:3540, the 354E..359B loop) runs inside
 * input_dispatch_loop, so pause_toggle (34BD), scan_id_list (34C0) and the
 * [0x1979] dispatch (34C3) wait for it to return; a key pressed during the
 * pan is never seen.  The port advances one pan frame per tick, so its play
 * loop must skip those same steps while level_goal_pan_running().
 *
 * The test drives game_main through the room into level 26 (PARKVIEW, entered
 * through save slot 20 so the room's match can be overridden), walks the
 * player record onto the level's 0xBA cell to start the pan, and injects ESC
 * on the first pan frame: the pause must not open during the pan, and a fresh
 * ESC after it must.
 *
 * argv[1] = the game data directory (the volumes + SAVET).  Missing volumes =
 * skip; a pre-existing SAVET is never clobbered. */
#include "frontend.h"
#include "font.h"
#include "level.h"
#include "menu.h"
#include "object.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "save.h"
#include "statusbar.h"
#include "video.h"
#include "compat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SAVE_SLOT 20u
#define SAVE_NAME "SAVET"
#define SCRIPT_N  10000u

extern const OSystem g_osys_null;
extern void osys_null_input_script(const InputState *states, uint32_t n);
extern void osys_null_on_present(void (*fn)(uint32_t poll));

static InputState _script[SCRIPT_N];
static uint8_t _moved, _saw_pan, _esc_armed, _pan_done, _checked;
static uint32_t _pan_done_at;
static int32_t _menu_during_pan, _menu_after_pan;

/** @brief The frame hook: seed the goal, inject keys around the pan.
 *
 * `poll` is the index of the NEXT script entry (the backend bumps it in
 * _null_poll before the present), so writing `_script[poll]` sets the next
 * frame's input.
 *
 * @param poll the next script index
 */
static void _hook(uint32_t poll)
{
    if (level_data() && level_data()->levelId == 25u && !_moved &&
        object_player() >= 0 && poll >= 8900u) {
        uint8_t *r = object_slot((uint32_t)object_player());

        /* the level's one 0xBA cell: (col 2, row 13) */
        if (r) {
            r[0] = 32u;
            r[1] = 0u;
            r[2] = 201u;
            r[3] = 0u;
            _moved = 1;
        }
    }
    if (level_goal_pan_running()) {
        _saw_pan = 1;
        if (menu_active()) _menu_during_pan = 1;
        if (!_esc_armed) { // ESC on the next frame, inside the pan
            _esc_armed = 1;
            _script[poll].keyScan = 0x01u;
        }
    } else if (_saw_pan && !_pan_done) {
        _pan_done = 1;
        _pan_done_at = poll;
        _script[poll].keyScan = 0x01u; // a fresh ESC once the pan returned
    }
    if (_pan_done && !_checked && poll > _pan_done_at + 30u) {
        _checked = 1;
        _menu_after_pan = menu_active();
    }
}

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    char path[512];
    int32_t bad = 0;

    snprintf(path, sizeof path, "%s/HUMANS.002", vol_dir);
    {
        FILE *f = fopen(path, "rb");

        if (!f) {
            printf("skip: no volume files at %s\n", vol_dir);
            return 0;
        }
        fclose(f);
    }
    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }
    {
        FILE *f = fopen(SAVE_NAME, "rb");

        if (f) {
            fclose(f);
            printf("skip: %s exists in %s (a real save; not overwriting)\n",
                   SAVE_NAME, vol_dir);
            return 0;
        }
    }

    /* the timeline test_smoke pins: attract escape (7400), the room's
     * walk-out (7500..7750), the party key (8100), the goal page's key
     * (8872), then the play loop - the pan follows the 9000 seed below. */
    memset(_script, 0, sizeof _script);
    _script[7400].joyBits = 0x80u;
    for (uint32_t i = 7500u; i <= 7750u; i++) _script[i].joyBits = 0x08u;
    _script[8100].lastKey = 1u;
    for (uint32_t i = 8777u; i <= 8867u; i++) _script[i].joyBits = 0x08u;
    _script[8872].lastKey = 1u;
    _script[SCRIPT_N - 1].quit = 1u;

    osys_bind(&g_osys_null);
    res_init();
    video_init();
    palette_init();
    font_init();
    statusbar_init();
    level_init();
    save_init();
    frontend_init();
    menu_init();

    /* the save's state ranges come from a loaded level, so its object table
     * carries the 0xC0 player record the test moves onto the 0xBA cell */
    if (level_load(25) != 0) {
        printf("skip: level 26 is not readable\n");
        return 0;
    }
    if (!save_write_slot(SAVE_SLOT, 26u, 0x0600u, 12u)) {
        fprintf(stderr, "FAIL cannot write %s\n", SAVE_NAME);
        return 1;
    }
    frontend_option_set_for_probe(FRONTEND_OPT_GAME, SAVE_SLOT);

    osys_null_input_script(_script, SCRIPT_N);
    osys_null_on_present(_hook);
    (void)game_main(&g_osys_null);

    if (!_moved || !_saw_pan) {
        fprintf(stderr, "FAIL the win pan never ran (moved %u, pan %u)\n",
                _moved, _saw_pan);
        bad = 1;
    }
    /* D-266: the ESC pressed inside the pan is not seen (the original's
     * scan_id_list waits for the blocking loop), the same key after it is */
    if (_menu_during_pan) {
        fprintf(stderr, "FAIL the pause opened while the win pan ran\n");
        bad = 1;
    }
    if (!_pan_done || !_menu_after_pan) {
        fprintf(stderr, "FAIL the pause did not open after the pan\n");
        bad = 1;
    }

    remove(SAVE_NAME);
    if (bad) {
        fprintf(stderr, "pan_input: FAIL\n");
        return 1;
    }
    printf("pan_input: ok\n");
    return 0;
}
