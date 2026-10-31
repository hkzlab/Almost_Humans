/* test_pause.c — the in-level pause menu + SAVE GAME's writer (pass 9).
 *
 * The dialog behaviour is the original's (re_map.md §"The in-level pause
 * menu"): the 0-based rows of 202F:0x5BBC with up/down wrap and ACTION/Enter
 * select, the SAVE GAME grid (202F:0x5CD8, 4 rows x 5 columns column-major,
 * cell r,c = game 4c+r+1), and the writer's header read back by save.c.
 *
 * Drives `menu_tick` directly (the pause lives in game_main's level branch;
 * its five actions are five lines there, exercised by the smoke test's flow).
 * Asserts:
 *   - the pause's rows/wrap and the 0-based results 0..4 (row 3 opens the grid);
 *   - the grid's navigation (down/right/left, the wraps) and the 4c+r+1 law;
 *   - the renderer ran (the box fill 0xE4 + the text 0xFA on the page);
 *   - SAVE writes the 0x1800 file with the live header (level/time/members),
 *     zero state ranges, and save_loadSlot reads the identical values back;
 *   - save_consume (level_run 30B4) drops [0x34] so a restart takes the
 *     LEVEL board's pick.
 *
 * argv[1] = the game data directory (the volumes + the save location).
 * Missing volumes = skip; a pre-existing SAVEB is never clobbered. */
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"

extern const OSystem osys_null;
extern const uint8_t *osys_null_page(int which);
extern const uint8_t *osys_null_palette(void);

#define SAVE_LEN 0x1800u
#define SAVE_NAME "SAVEB"

static void dump_ppm(const char *dir, const char *name, const uint8_t *page,
                     const uint8_t *pal)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    for (unsigned i = 0; i < 320u * 200u; i++) {
        unsigned idx = page[i];
        for (unsigned c = 0; c < 3u; c++) {
            uint8_t v6 = pal[idx * 3u + c];
            fputc((uint8_t)((v6 << 2) | (v6 >> 4)), f);
        }
    }
    fclose(f);
}

/* draw_map_object's outline pass (1000:9FA7 -> 9FB1): a text pixel is any
 * byte in [0xF4,0xFB]; each of its four in-page neighbours is either text or
 * 0 (the black outline).  Counts the text and outline pixels and the number
 * of neighbours that are neither. */
static void text_outline(const uint8_t *page, unsigned *text,
                         unsigned *outline, unsigned *violations)
{
    static const int dx[4] = { 0, 0, -1, 1 }, dy[4] = { -1, 1, 0, 0 };

    *text = *outline = *violations = 0;
    for (unsigned y = 0; y < 200u; y++)
        for (unsigned x = 0; x < 320u; x++) {
            unsigned i = y * 320u + x;
            if (page[i] < 0xF4u || page[i] > 0xFBu) continue;
            (*text)++;
            for (unsigned k = 0; k < 4u; k++) {
                int nx = (int)x + dx[k], ny = (int)y + dy[k];
                unsigned n;
                if (nx < 0 || ny < 0 || nx > 319 || ny > 199) continue;
                n = (unsigned)ny * 320u + (unsigned)nx;
                if (page[n] == 0u) (*outline)++;
                else if (page[n] < 0xF4u) (*violations)++;
            }
        }
}

static InputState idle(void)
{
    InputState in;
    memset(&in, 0, sizeof in);
    return in;
}

/* one tick with the given joy bits; the release tick afterwards makes the
 * next one a fresh press (the original waits for the key release) */
static int press(unsigned bits)
{
    InputState in = idle();
    in.joyBits = bits;
    int up = menu_tick(&in);
    InputState rel = idle();
    (void)menu_tick(&rel);
    return up;
}

static int open_pause(void)
{
    menu_openPause();
    return menu_active();
}

int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *dump_dir = argc > 2 ? argv[2] : NULL;
    char path[512];
    int bad = 0;

    snprintf(path, sizeof path, "%s/HUMANS.000", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume files at %s (fixture data absent)\n", vol_dir);
        return 0;
    }
    fclose(f);
    if (compat_chdir(vol_dir)) { perror(vol_dir); return 1; }

    f = fopen(SAVE_NAME, "rb");
    if (f) {
        fclose(f);
        printf("skip: %s exists in %s (a real save; not overwriting)\n",
               SAVE_NAME, vol_dir);
        return 0;
    }

    video_init(&osys_null);
    palette_init(&osys_null);
    font_init(&osys_null);
    statusbar_init(&osys_null);
    level_init(&osys_null);
    menu_init(&osys_null);

    /* the dialog's text needs the shipped glyphs (the room publishes them
     * on the real path) */
    {
        uint8_t *text = res_loadChunk("APL\\TEXT.SPR", NULL, NULL);
        font_publish(text);
        if (!text) {
            fprintf(stderr, "FAIL TEXT.SPR unreadable\n");
            return 1;
        }
    }

    /* the level's assets: the dialog's box tiles (apl\box.spr, disc 1) and
     * the bar/level palette the page's indices ride */
    if (level_load(0) != 0) {
        fprintf(stderr, "FAIL level 0 did not load\n");
        return 1;
    }

    /* 1. the pause menu: 5 rows, wrap, the 0-based result = the row */
    for (unsigned r = 0; r < 5u; r++) {
        if (!open_pause()) { fprintf(stderr, "FAIL the menu did not open\n"); return 1; }
        for (unsigned i = 0; i < r; i++) (void)press(0x02u);   /* down */
        if (r > 0u && menu_result() != -1) {
            fprintf(stderr, "FAIL the result appeared early\n");
            bad = 1;
        }
        (void)press(0x80u);                                    /* ACTION */
        if (r == 3u) {
            /* SAVE GAME opens the grid instead of closing */
            if (!menu_active() || menu_result() != -1) {
                fprintf(stderr, "FAIL SAVE GAME did not open the grid\n");
                bad = 1;
            }
            menu_close();
        } else {
            if (menu_active()) {
                fprintf(stderr, "FAIL row %u stayed open\n", r);
                return 1;
            }
            if (menu_result() != (int)r) {
                fprintf(stderr, "FAIL row %u returned %d\n", r, menu_result());
                bad = 1;
            }
        }
    }

    /* wrap: UP from row 0 -> row 4 (QUIT) */
    menu_openPause();
    (void)press(0x01u);
    menu_draw();
    {
        const uint8_t *page = osys_null_page(0);
        unsigned fill = 0, text = 0, outline = 0, violations = 0;
        for (unsigned i = 0; i < 320u * 200u; i++)
            if (page[i] == 26u) fill++;          /* BOX.SPR's fill index */
        /* the text = the fill tile's EB..E4..EB ramp | 0x10 -> F4..FB, and
         * the black outline around it (1000:9FA7 -> 9FB1) */
        text_outline(page, &text, &outline, &violations);
        if (fill < 500u || text < 50u) {
            fprintf(stderr, "FAIL the dialog did not render (fill %u, text %u)\n",
                    fill, text);
            bad = 1;
        }
        if (outline == 0u) {
            fprintf(stderr, "FAIL the dialog's text has no black outline\n");
            bad = 1;
        }
        if (violations) {
            fprintf(stderr, "FAIL %u text-neighbour pixels are neither text nor "
                    "the outline\n", violations);
            bad = 1;
        }
    }
    (void)press(0x80u);
    if (menu_result() != 4) {
        fprintf(stderr, "FAIL the UP wrap gave %d, want 4\n", menu_result());
        bad = 1;
    }

    /* 2. the SAVE GAME grid: 4x5 column-major, cell r,c = 4c+r+1.
     *    Row 0 col 0 = game 1; DOWN once = game 2; RIGHT once = game 5. */
    menu_openPause();
    (void)press(0x02u); (void)press(0x02u); (void)press(0x02u);  /* row 3 */
    (void)press(0x80u);                                        /* the grid */
    if (!menu_active()) { fprintf(stderr, "FAIL the grid did not open\n"); return 1; }
    (void)press(0x02u);                                        /* row 1 */
    (void)press(0x80u);
    if (menu_slot() != 2u || menu_result() != 3) {
        fprintf(stderr, "FAIL grid (0,1) gave game %u (result %d)\n",
                menu_slot(), menu_result());
        bad = 1;
    }
    menu_close();

    menu_openPause();
    (void)press(0x02u); (void)press(0x02u); (void)press(0x02u);
    (void)press(0x80u);
    (void)press(0x08u);                                        /* right */
    (void)press(0x80u);
    if (menu_slot() != 5u) {
        fprintf(stderr, "FAIL grid (0,1) gave game %u, want 5\n", menu_slot());
        bad = 1;
    }
    menu_close();

    /* the grid page is the same draw_map_object, so it gets the outline too */
    menu_openPause();
    (void)press(0x02u); (void)press(0x02u); (void)press(0x02u);
    (void)press(0x80u);
    {
        const uint8_t *page = osys_null_page(0);
        unsigned text = 0, outline = 0, violations = 0;
        menu_draw();
        text_outline(page, &text, &outline, &violations);
        if (text < 50u || outline == 0u || violations) {
            fprintf(stderr, "FAIL the grid page (text %u, outline %u, %u bad "
                    "neighbours)\n", text, outline, violations);
            bad = 1;
        }
    }
    menu_openPause();
    (void)press(0x02u); (void)press(0x02u); (void)press(0x02u);
    (void)press(0x80u);
    (void)press(0x01u);                                        /* row wrap 0->3 */
    (void)press(0x04u);                                        /* left wrap 0->4 */
    (void)press(0x80u);
    if (menu_slot() != 20u) {
        fprintf(stderr, "FAIL the grid wrap gave game %u, want 20\n", menu_slot());
        bad = 1;
    }
    menu_close();

    /* 3. SAVE GAME writes the header; save_loadSlot reads it back */
    if (!save_writeSlot(2u, 2u, 0x0549u, 7u)) {
        fprintf(stderr, "FAIL save_writeSlot(SLOT 2) failed\n");
        return 1;
    }
    {
        uint8_t b[SAVE_LEN];
        f = fopen(SAVE_NAME, "rb");
        if (!f || fread(b, 1, sizeof b, f) != sizeof b) {
            if (f) fclose(f);
            fprintf(stderr, "FAIL the written %s is not 0x1800 bytes\n", SAVE_NAME);
            return 1;
        }
        fclose(f);
        if (b[0x00] != 2u || b[0x01] != 0u || b[0x06] != 1u ||
            b[0x07] != 0x49u || b[0x08] != 0x05u ||
            b[0x0B] != 7u || b[0x0C] != 0u) {
            fprintf(stderr, "FAIL the written header (level %u, state %u, "
                    "time %02x%02x, members %u)\n", b[0], b[6], b[8], b[7],
                    b[0x0B]);
            bad = 1;
        }
        for (unsigned i = 0x0Du; i < SAVE_LEN; i++) {
            if (b[i] != 0u) {
                fprintf(stderr, "FAIL the unported blob range is not zero at "
                        "0x%X\n", i);
                bad = 1;
                break;
            }
        }
    }
    if (!save_loadSlot(2u) || !save_present() ||
        save_level() != 2u || save_time() != 0x0549u || save_members() != 7u) {
        fprintf(stderr, "FAIL the written save does not load back\n");
        bad = 1;
    }

    /* 4. save_consume (level_run 30B4): a restart takes the board's pick */
    save_consume();
    if (save_present()) {
        fprintf(stderr, "FAIL save_consume left [0x34] set\n");
        bad = 1;
    }
    if (level_load(0) != 0) {
        fprintf(stderr, "FAIL the level did not load\n");
        bad = 1;
    } else {
        const LevelData *d = level_data();
        if (level_timeLimit() != d->timeA) {
            fprintf(stderr, "FAIL the restart did not take the board's time\n");
            bad = 1;
        }
    }

    /* 5. the five actions (dispatch_by_result_a167): CONTINUE, RESTART THIS
     *    LEVEL, RESTART THIS GAME, SAVE GAME, QUIT */
    {
        int quit = 0;
        if (gameflow_pauseAction(0, &quit) != FLOW_LEVEL || quit) {
            fprintf(stderr, "FAIL CONTINUE\n");
            bad = 1;
        }
        if (level_timeLimit() != level_data()->timeA) {
            fprintf(stderr, "FAIL CONTINUE changed the pick\n");
            bad = 1;
        }
        /* RESTART THIS LEVEL: the record's pick again (the save consumed) */
        if (gameflow_pauseAction(1, &quit) != FLOW_LEVEL) {
            fprintf(stderr, "FAIL RESTART THIS LEVEL\n");
            bad = 1;
        }
        if (level_timeLimit() != level_data()->timeA ||
            level_tribeMembers() != 12u) {
            fprintf(stderr, "FAIL the restart is not the board's pick\n");
            bad = 1;
        }
        /* SAVE GAME through the action: the menu's slot + the live state */
        menu_openPause();
        (void)press(0x02u); (void)press(0x02u); (void)press(0x02u);
        (void)press(0x80u);                    /* the grid */
        (void)press(0x02u);                    /* row 1 = game 2 */
        (void)press(0x80u);
        if (menu_slot() != 2u) {
            fprintf(stderr, "FAIL the action's slot is %u\n", menu_slot());
            bad = 1;
        }
        if (gameflow_pauseAction(3, &quit) != FLOW_LEVEL || quit) {
            fprintf(stderr, "FAIL SAVE GAME\n");
            bad = 1;
        }
        f = fopen(SAVE_NAME, "rb");
        if (!f) {
            fprintf(stderr, "FAIL SAVE GAME wrote nothing\n");
            bad = 1;
        } else {
            uint8_t b[SAVE_LEN];
            size_t n = fread(b, 1, sizeof b, f);
            fclose(f);
            if (n != sizeof b || b[0x00] != 1u || b[0x07] != 0x00u ||
                b[0x08] != 0x06u || b[0x0B] != 12u) {
                fprintf(stderr, "FAIL SAVE GAME's header (level %u, time "
                        "%02x%02x, members %u)\n", b[0], b[8], b[7], b[0x0B]);
                bad = 1;
            }
        }
        /* RESTART THIS GAME -> the room, [0x34] cleared */
        if (gameflow_pauseAction(2, &quit) != FLOW_MENU || save_present()) {
            fprintf(stderr, "FAIL RESTART THIS GAME\n");
            bad = 1;
        }
        /* QUIT THIS GAME */
        quit = 0;
        (void)gameflow_pauseAction(4, &quit);
        if (!quit) {
            fprintf(stderr, "FAIL QUIT THIS GAME did not request the exit\n");
            bad = 1;
        }
    }

    /* 6. the time-up dialog (202F:0x5C2B): the 3-option block + the inline
     *    dispatch (0 restart this level, 1 the room, 2 quit) */
    {
        int quit = 0;
        menu_openTimeUp();
        if (!menu_active() || !menu_timeUp()) {
            fprintf(stderr, "FAIL the time-up dialog did not open\n");
            bad = 1;
        }
        (void)press(0x02u);                    /* row 1 */
        (void)press(0x80u);
        if (!menu_timeUp() || menu_result() != 1) {
            fprintf(stderr, "FAIL the time-up dialog gave %d (kind %d)\n",
                    menu_result(), menu_timeUp());
            bad = 1;
        }
        menu_close();
        if (gameflow_timeUpAction(0, &quit) != FLOW_LEVEL) {
            fprintf(stderr, "FAIL the time-up RESTART THIS LEVEL\n");
            bad = 1;
        }
        if (gameflow_timeUpAction(1, &quit) != FLOW_MENU || save_present()) {
            fprintf(stderr, "FAIL the time-up RESTART THIS GAME\n");
            bad = 1;
        }
        quit = 0;
        (void)gameflow_timeUpAction(2, &quit);
        if (!quit) {
            fprintf(stderr, "FAIL the time-up QUIT\n");
            bad = 1;
        }
    }

    /* the port's dialog over the level page (the stand-in renderer) */
    if (dump_dir) {
        (void)level_load(0);
        /* level_fade_in (1000:3334) first — the dump is the settled level */
        level_begin();
        for (unsigned i = 0; i < 400u && level_starting(); i++) {
            level_tick();
            palette_tick();
            if (level_loadingScreen()) level_loadingTick();
        }
        level_tick();
        menu_openPause();
        menu_draw();
        dump_ppm(dump_dir, "menu_port_pause", osys_null_page(0),
                 osys_null_palette());
        level_tick();
        (void)press(0x02u); (void)press(0x02u); (void)press(0x02u);
        (void)press(0x80u);                    /* the save grid */
        level_tick();                          /* the fresh level frame */
        menu_draw();
        dump_ppm(dump_dir, "menu_port_grid", osys_null_page(0),
                 osys_null_palette());
        menu_close();
    }

    remove(SAVE_NAME);
    if (bad) {
        fprintf(stderr, "pause: FAIL\n");
        return 1;
    }
    printf("pause: ok\n");
    return 0;
}
