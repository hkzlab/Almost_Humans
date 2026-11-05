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
 *     zero state ranges, and save_load_slot reads the identical values back;
 *   - save_consume (level_run 30B4) drops [0x34] so a restart takes the
 *     LEVEL board's pick.
 *
 * argv[1] = the game data directory (the volumes + the save location).
 * Missing volumes = skip; a pre-existing SAVEB is never clobbered. */
#include "entity.h"
#include "font.h"
#include "gameflow.h"
#include "level.h"
#include "menu.h"
#include "object.h"
#include "osystem.h"
#include "palette.h"
#include "party_intro.h"
#include "res.h"
#include "save.h"
#include "score.h"
#include "statusbar.h"
#include "video.h"
#include "compat.h"
#include "byteorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAVE_LEN  0x1800u
#define SAVE_NAME "SAVEB"

extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);

extern const OSystem g_osys_null;

/**
 * @brief Store one little-endian 16-bit word.
 *
 * @param p the destination bytes
 * @param v the value
 */
static void _put16(uint8_t *p, uint32_t v) { wr_le_u16(p, (uint16_t)v); }

/**
 * @brief Write a page and its palette as a binary PPM for inspection.
 *
 * @param dir the direction
 * @param name the name
 * @param page the page buffer
 * @param pal the palette
 */
static void _dump_ppm(const char *dir, const char *name, const uint8_t *page,
                      const uint8_t *pal)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    for (uint32_t i = 0; i < 320u * 200u; i++) {
        uint32_t idx = page[i];
        for (uint32_t c = 0; c < 3u; c++) {
            uint8_t v6 = pal[idx * 3u + c];
            fputc((uint8_t)((v6 << 2) | (v6 >> 4)), f);
        }
    }
    fclose(f);
}

/** @brief draw_map_object's outline pass (1000:9FA7 -> 9FB1): a text pixel is any
 * byte in [0xF4,0xFB]; each of its four in-page neighbours is either text or
 * 0 (the black outline).  Counts the text and outline pixels and the number
 * of neighbours that are neither.
 *
 * @param page the page buffer
 * @param text the text
 * @param outline the outline buffer
 * @param violations the violation count
 */
static void _text_outline(const uint8_t *page, uint32_t *text,
                          uint32_t *outline, uint32_t *violations)
{
    static const int32_t dx[4] = { 0, 0, -1, 1 }, dy[4] = { -1, 1, 0, 0 };

    *text = *outline = *violations = 0;
    for (uint32_t y = 0; y < 200u; y++)
        for (uint32_t x = 0; x < 320u; x++) {
            uint32_t i = y * 320u + x;
            if (page[i] < 0xF4u || page[i] > 0xFBu) continue;
            (*text)++;
            for (uint32_t k = 0; k < 4u; k++) {
                int32_t nx = (int32_t)x + dx[k], ny = (int32_t)y + dy[k];
                uint32_t n;
                if (nx < 0 || ny < 0 || nx > 319 || ny > 199) continue;
                n = (uint32_t)ny * 320u + (uint32_t)nx;
                if (page[n] == 0u) (*outline)++;
                else if (page[n] < 0xF4u) (*violations)++;
            }
        }
}

/**
 * @brief A zeroed input state (no key, fire released).
 *
 * @return the zeroed input state
 */
static InputState _idle(void)
{
    InputState in;
    memset(&in, 0, sizeof in);
    return in;
}

/** @brief one tick with the given joy bits; the release tick afterwards makes the
 * next one a fresh _press (the original waits for the key release)
 *
 * @param bits the bit count
 * @return 1 when the room exited, 0 otherwise
 */
static int32_t _press(uint32_t bits)
{
    InputState in = _idle();
    in.joyBits = bits;
    int32_t up = menu_tick(&in);
    InputState rel = _idle();
    (void)menu_tick(&rel);
    return up;
}

/**
 * @brief Open the pause menu and report whether it took.
 *
 * @return 1 when the pause menu is up, 0 otherwise
 */
static int32_t _open_pause(void)
{
    menu_open_pause();
    return menu_active();
}

/**
 * @brief Run the pause menu tests.
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *dump_dir = argc > 2 ? argv[2] : NULL;
    char path[512];
    int32_t bad = 0;

    snprintf(path, sizeof path, "%s/HUMANS.000", vol_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("skip: no volume files at %s (fixture data absent)\n", vol_dir);
        return 0;
    }
    fclose(f);
    if (compat_chdir(vol_dir)) {
        perror(vol_dir);
        return 1;
    }

    f = fopen(SAVE_NAME, "rb");
    if (f) {
        fclose(f);
        printf("skip: %s exists in %s (a real save; not overwriting)\n",
               SAVE_NAME, vol_dir);
        return 0;
    }

    video_init(&g_osys_null);
    palette_init(&g_osys_null);
    font_init(&g_osys_null);
    statusbar_init(&g_osys_null);
    level_init(&g_osys_null);
    menu_init(&g_osys_null);

    /* the dialog's text needs the shipped glyphs (the room publishes them
     * on the real path) */
    {
        uint8_t *text = res_load_chunk("APL\\TEXT.SPR", NULL, NULL);
        font_publish(text);
        if (!text) {
            fprintf(stderr, "FAIL TEXT.SPR unreadable\n");
            return 1;
        }
    }

    /* D-18: the briefing's score is the live [0x592] low word. */
    {
        char out[8];

        score_set_total(1234u, 1u);
        party_intro_set_values(1u, 4u, 12u, gameflow_party_score(), "DARWIN");
        party_intro_format_score(party_intro_score(), out);
        if (party_intro_score() != 1234u || strcmp(out, "1234") != 0) {
            fprintf(stderr, "FAIL D-18: briefing score/digits ('%s')\n", out);
            bad = 1;
        }
    }

    /* D-15: scan_id_list (1000:386E) binds ESC (scancode 0x01) to event 4,
     * whose handler (1000:38EA) opens the 202F:0x5BBC block.  The list's
     * entries are scancodes and the world gate [0xC] must be open for them
     * (1000:38AD), so drive it the way the play loop does. */
    {
        InputState k;

        memset(&k, 0, sizeof k);
        menu_close();
        level_set_updates(1);
        k.keyScan = 0x01u;                       /* ESC */
        if (gameflow_key_list(&k) != 1 || !menu_active() || menu_time_up()) {
            fprintf(stderr, "FAIL D-15: ESC does not open the pause menu\n");
            bad = 1;
        }
        /* a second ESC while the block is open: the dialog owns the input in
         * the original, so the list must not reopen or close it */
        (void)gameflow_key_list(&k);
        if (!menu_active() || menu_time_up()) {
            fprintf(stderr,
                    "FAIL D-15: a second ESC disturbed the open menu\n");
            bad = 1;
        }
        menu_close();
        k.keyScan = 0u;                          /* a non-listed key */
        k.lastKey = 'Z';
        if (gameflow_key_list(&k) != 0 || menu_active()) {
            fprintf(stderr, "FAIL D-15: a non-ESC key opens the pause menu\n");
            bad = 1;
        }
        /* the [0xC] gate: a non-bit7 entry is skipped while the world is
         * frozen (the claim holds it at 0) */
        k.keyScan = 0x01u;
        level_set_updates(0);
        if (gameflow_key_list(&k) != 0 || menu_active()) {
            fprintf(stderr, "FAIL the [0xC] gate stopped a key event\n");
            bad = 1;
        }
        level_set_updates(1);
    }

    /* the level's assets: the dialog's box tiles (apl\box.spr, disc 1) and
     * the bar/level palette the page's indices ride */
    if (level_load(0) != 0) {
        fprintf(stderr, "FAIL level 0 did not load\n");
        return 1;
    }

    /* 1. the pause menu: 5 rows, wrap, the 0-based result = the row */
    for (uint32_t r = 0; r < 5u; r++) {
        if (!_open_pause()) {
            fprintf(stderr, "FAIL the menu did not open\n");
            return 1;
        }
        for (uint32_t i = 0; i < r; i++) (void)_press(0x02u);   /* down */
        if (r > 0u && menu_result() != -1) {
            fprintf(stderr, "FAIL the result appeared early\n");
            bad = 1;
        }
        (void)_press(0x80u);                                    /* ACTION */
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
            if (menu_result() != (int32_t)r) {
                fprintf(stderr, "FAIL row %u returned %d\n", r, menu_result());
                bad = 1;
            }
        }
    }

    /* wrap: UP from row 0 -> row 4 (QUIT) */
    menu_open_pause();
    (void)_press(0x01u);
    menu_draw();
    {
        const uint8_t *page = osys_null_page(0);
        uint32_t fill = 0, text = 0, outline = 0, violations = 0;
        for (uint32_t i = 0; i < 320u * 200u; i++)
            if (page[i] == 26u) fill++;          /* BOX.SPR's fill index */
        /* the text = the fill tile's EB..E4..EB ramp | 0x10 -> F4..FB, and
         * the black outline around it (1000:9FA7 -> 9FB1) */
        _text_outline(page, &text, &outline, &violations);
        if (fill < 500u || text < 50u) {
            fprintf(stderr,
                    "FAIL the dialog did not render (fill %u, text %u)\n", fill,
                    text);
            bad = 1;
        }
        if (outline == 0u) {
            fprintf(stderr, "FAIL the dialog's text has no black outline\n");
            bad = 1;
        }
        if (violations) {
            fprintf(stderr,
                    "FAIL %u text-neighbour pixels are neither text nor "
                    "the outline\n",
                    violations);
            bad = 1;
        }
    }
    (void)_press(0x80u);
    if (menu_result() != 4) {
        fprintf(stderr, "FAIL the UP wrap gave %d, want 4\n", menu_result());
        bad = 1;
    }

    /* 2. the SAVE GAME grid: 4x5 column-major, cell r,c = 4c+r+1.
     *    Row 0 col 0 = game 1; DOWN once = game 2; RIGHT once = game 5. */
    menu_open_pause();
    (void)_press(0x02u);
    (void)_press(0x02u);
    (void)_press(0x02u); /* row 3 */
    (void)_press(0x80u); /* the grid */
    if (!menu_active()) {
        fprintf(stderr, "FAIL the grid did not open\n");
        return 1;
    }
    (void)_press(0x02u); /* row 1 */
    (void)_press(0x80u);
    if (menu_slot() != 2u || menu_result() != 3) {
        fprintf(stderr, "FAIL grid (0,1) gave game %u (result %d)\n",
                menu_slot(), menu_result());
        bad = 1;
    }
    menu_close();

    menu_open_pause();
    (void)_press(0x02u);
    (void)_press(0x02u);
    (void)_press(0x02u);
    (void)_press(0x80u);
    (void)_press(0x08u); /* right */
    (void)_press(0x80u);
    if (menu_slot() != 5u) {
        fprintf(stderr, "FAIL grid (0,1) gave game %u, want 5\n", menu_slot());
        bad = 1;
    }
    menu_close();

    /* the grid page is the same draw_map_object, so it gets the outline too */
    menu_open_pause();
    (void)_press(0x02u);
    (void)_press(0x02u);
    (void)_press(0x02u);
    (void)_press(0x80u);
    {
        const uint8_t *page = osys_null_page(0);
        uint32_t text = 0, outline = 0, violations = 0;
        menu_draw();
        _text_outline(page, &text, &outline, &violations);
        if (text < 50u || outline == 0u || violations) {
            fprintf(stderr,
                    "FAIL the grid page (text %u, outline %u, %u bad "
                    "neighbours)\n",
                    text, outline, violations);
            bad = 1;
        }
    }
    menu_open_pause();
    (void)_press(0x02u);
    (void)_press(0x02u);
    (void)_press(0x02u);
    (void)_press(0x80u);
    (void)_press(0x01u); /* row wrap 0->3 */
    (void)_press(0x04u); /* left wrap 0->4 */
    (void)_press(0x80u);
    if (menu_slot() != 20u) {
        fprintf(stderr, "FAIL the grid wrap gave game %u, want 20\n",
                menu_slot());
        bad = 1;
    }
    menu_close();

    /* 3. SAVE GAME writes the header and the state ranges; save_load_slot
     *    reads them back. */
    score_set_total(0x1234u, 1u);
    statusbar_set_tribe_digits(0u, 9u);
    {
        uint8_t *r0 = object_slot(0);

        if (r0) {
            r0[0] = 0x34;
            r0[1] = 0x12;
            r0[2] = 0x56;
            r0[3] = 0x78;
        }
        object_set_slot_class(1u, 6u);
    }
    if (!save_write_slot(2u, 2u, 0x0549u, 7u)) {
        fprintf(stderr, "FAIL save_write_slot(SLOT 2) failed\n");
        return 1;
    }
    {
        uint8_t b[SAVE_LEN];
        f = fopen(SAVE_NAME, "rb");
        if (!f || fread(b, 1, sizeof b, f) != sizeof b) {
            if (f) fclose(f);
            fprintf(stderr, "FAIL the written %s is not 0x1800 bytes\n",
                    SAVE_NAME);
            return 1;
        }
        fclose(f);
        if (b[0x00] != 2u || b[0x01] != 0u || b[0x02] != 0x34u ||
            b[0x03] != 0x12u || b[0x04] != 0x01u || b[0x05] != 0x00u ||
            b[0x06] != 1u || b[0x07] != 0x49u || b[0x08] != 0x05u ||
            b[0x09] != 0u || b[0x0A] != 9u || b[0x0B] != 7u || b[0x0C] != 0u) {
            fprintf(stderr,
                    "FAIL the written header (level %u, state %u, "
                    "time %02x%02x, members %u)\n",
                    b[0], b[6], b[8], b[7], b[0x0B]);
            bad = 1;
        }
        /* 0x3E20's fixed pointer table and 0x5B37's first program */
        if (b[0x0D + 4u] != 0x75u || b[0x0D + 5u] != 0x3Eu ||
            b[0x1636] != 0x02u || b[0x1637] != 0x01u || b[0x17F0] != 0u ||
            b[0x17F4] != 9u || b[0x17F5] != 0x10u || b[0x17F7] != 0x90u) {
            fprintf(stderr, "FAIL the written state ranges\n");
            bad = 1;
        }
    }
    if (!save_load_slot(2u) || !save_present() || save_level() != 2u ||
        save_time() != 0x0549u || save_members() != 7u) {
        fprintf(stderr, "FAIL the written save does not load back\n");
        bad = 1;
    }
    save_restore_state();
    if (score_total_lo() != 0x1234u || score_total_hi() != 1u ||
        statusbar_tribe_digit(0u) != 0u || statusbar_tribe_digit(1u) != 9u) {
        fprintf(stderr, "FAIL the saved score/counter did not restore\n");
        bad = 1;
    }
    {
        const uint8_t *r0 = object_slot(0);

        if (!r0 || r0[0] != 0x34u || r0[1] != 0x12u || r0[2] != 0x56u ||
            object_slot_class(1u) != 6u) {
            fprintf(stderr, "FAIL the saved object table did not restore\n");
            bad = 1;
        }
    }
    /* the object table and entity array round-trip their fields */
    {
        static uint8_t tbl[0x25F];
        static uint8_t ent[0x72];
        uint8_t *r = object_slot(0);

        if (!r) {
            fprintf(stderr, "FAIL no object slot 0\n");
            bad = 1;
        } else {
            r[0] = 0x34;
            r[1] = 0x12;
            r[2] = 0x56;
            r[3] = 0x78;
            object_set_slot_class(0u, 5u);
            object_save_table(tbl);
            r[0] = 0;
            object_set_slot_class(0u, 0u);
            object_load_table(tbl);
            if (r[0] != 0x34 || r[1] != 0x12 || r[2] != 0x56 ||
                object_slot_class(0u) != 5u) {
                fprintf(stderr, "FAIL the object table did not round-trip\n");
                bad = 1;
            }
        }
        {
            static uint8_t pool[0x13CA];

            object_save_pool(pool);
            if (pool[0] != (uint8_t)level_special_tile(0u) ||
                pool[1] != (uint8_t)level_special_tile(1u)) {
                fprintf(stderr, "FAIL D-143: the pool's two tile bytes\n");
                bad = 1;
            }
        }
        memset(ent, 0, sizeof ent);
        ent[0] = 1; /* [0x64C2] = 1 */
        ent[2] = 0x34;
        ent[3] = 0x12; /* slot 0 +0x0 */
        entity_load_fixed(ent);
        if (!entity_at(0u) || entity_at(0u)->x0 != 0x1234u) {
            fprintf(stderr, "FAIL the entity array did not load\n");
            bad = 1;
        }
        entity_save_fixed(ent);
        if (ent[0] != 1u || ent[2] != 0x34u || ent[3] != 0x12u) {
            fprintf(stderr, "FAIL the entity array did not save\n");
            bad = 1;
        }
    }

    /* 3b. byte-exact original-format fixture: load every state range, then
     * write it back and compare all 0x1800 bytes. */
    {
        static const uint8_t prog[0x20] = {
            0x02, 0x01, 0x02, 0x01, 0x04, 0x01, 0x03, 0x04, 0x05, 0x06, 0x04,
            0x01, 0x03, 0x07, 0x08, 0x06, 0x04, 0x01, 0x03, 0x05, 0x09, 0x06,
            0x04, 0x01, 0x09, 0x0A, 0x0B, 0x0C, 0x01, 0x01, 0x06, 0x01,
        };
        static uint8_t fx[SAVE_LEN], got[SAVE_LEN];

        memset(fx, 0, sizeof fx);
        _put16(fx + 0x00, 2u);
        _put16(fx + 0x02, 0x1234u);
        _put16(fx + 0x04, 1u);
        fx[0x06] = 1u;
        _put16(fx + 0x07, 0x0549u);
        fx[0x09] = 0u;
        fx[0x0A] = 9u;
        _put16(fx + 0x0B, 7u);
        /* 0x3E20 */
        _put16(fx + 0x0D + 0u, 5u);
        _put16(fx + 0x0D + 2u, 0u);
        for (uint32_t i = 0; i < 29u; i++)
            _put16(fx + 0x0D + 4u + 2u * i, (uint32_t)(0x3E75u + 0x12u * i));
        fx[0x0D + 0x3E + 1u] = 6u; /* class slot 1 */
        fx[0x0D + 0x53u] = 9u; /* [0x3E73] */
        _put16(fx + 0x0D + 0x55u, 0x1234u); /* record 0 x */
        _put16(fx + 0x0D + 0x57u, 0x5678u); /* record 0 y */
        /* 0x6F20: the two single-byte tile ids (D-143), then the register
         * and span pools. */
        memset(fx + 0x026C + 2u, 0xFF, 0x70u);
        memset(fx + 0x026C + 0x72u, 0xFF, 0x38u);
        fx[0x026C] = (uint8_t)level_special_tile(0u);
        fx[0x026D] = (uint8_t)level_special_tile(1u);
        /* 0x5B37 and its [0x5B56] */
        memcpy(fx + 0x1636, prog, sizeof prog);
        /* 0x649A TER slots zero, then 0x64C2 count + 7 records */
        _put16(fx + 0x167E, 1u);
        _put16(fx + 0x167E + 2u, 0x1234u);
        _put16(fx + 0x167E + 4u, 0x5678u);
        /* 0x6F92 copy: spans + zero */
        memset(fx + 0x16F0, 0xFF, 0x38u);
        /* 0x5B22 block */
        fx[0x17F0] = 1u;
        fx[0x17F1] = 2u;
        fx[0x17F2] = 3u;
        fx[0x17F3] = 0u;
        fx[0x17F4] = 9u;
        fx[0x17F5] = 0x12u;
        fx[0x17F6] = 0u;
        fx[0x17F7] = 0x34u;

        f = fopen(SAVE_NAME, "wb");
        if (!f || fwrite(fx, 1, sizeof fx, f) != sizeof fx) {
            if (f) fclose(f);
            fprintf(stderr, "FAIL cannot write the state fixture\n");
            return 1;
        }
        fclose(f);
        if (!save_load_slot(2u)) {
            fprintf(stderr, "FAIL the state fixture did not load\n");
            bad = 1;
        } else {
            save_restore_state();
            if (!save_write_slot(2u, 2u, 0x0549u, 7u)) {
                fprintf(stderr, "FAIL the state fixture did not write\n");
                bad = 1;
            }
            f = fopen(SAVE_NAME, "rb");
            if (!f || fread(got, 1, sizeof got, f) != sizeof got) {
                if (f) fclose(f);
                fprintf(stderr, "FAIL cannot read the state fixture back\n");
                bad = 1;
            } else {
                fclose(f);
                if (memcmp(fx, got, sizeof fx) != 0) {
                    uint32_t at = 0;
                    while (at < sizeof fx && fx[at] == got[at]) at++;
                    fprintf(stderr,
                            "FAIL the state fixture differs at 0x%X "
                            "(want %02x, got %02x)\n",
                            at, fx[at], got[at]);
                    bad = 1;
                }
            }
        }
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
        if (level_time_limit() != d->timeA) {
            fprintf(stderr, "FAIL the restart did not take the board's time\n");
            bad = 1;
        }
    }

    /* 5. the five actions (dispatch_by_result_a167): CONTINUE, RESTART THIS
     *    LEVEL, RESTART THIS GAME, SAVE GAME, QUIT */
    {
        int32_t quit = 0;
        if (gameflow_pause_action(0, &quit) != FLOW_LEVEL || quit) {
            fprintf(stderr, "FAIL CONTINUE\n");
            bad = 1;
        }
        if (level_time_limit() != level_data()->timeA) {
            fprintf(stderr, "FAIL CONTINUE changed the pick\n");
            bad = 1;
        }
        /* RESTART THIS LEVEL: the record's pick again (the save consumed) */
        if (gameflow_pause_action(1, &quit) != FLOW_LEVEL) {
            fprintf(stderr, "FAIL RESTART THIS LEVEL\n");
            bad = 1;
        }
        if (level_time_limit() != level_data()->timeA ||
            level_tribe_members() != 12u) {
            fprintf(stderr, "FAIL the restart is not the board's pick\n");
            bad = 1;
        }
        /* D-82: 1000:3073 plays the party screen after a restart (only the
         * RESTORE GAME path sets [0x34]); the port must leave it pending. */
        if (!gameflow_party_pending()) {
            fprintf(stderr,
                    "FAIL RESTART THIS LEVEL skipped the party screen\n");
            bad = 1;
        }
        /* SAVE GAME through the action: the menu's slot + the live state */
        menu_open_pause();
        (void)_press(0x02u);
        (void)_press(0x02u);
        (void)_press(0x02u);
        (void)_press(0x80u); /* the grid */
        (void)_press(0x02u); /* row 1 = game 2 */
        (void)_press(0x80u);
        if (menu_slot() != 2u) {
            fprintf(stderr, "FAIL the action's slot is %u\n", menu_slot());
            bad = 1;
        }
        if (gameflow_pause_action(3, &quit) != FLOW_LEVEL || quit) {
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
                fprintf(stderr,
                        "FAIL SAVE GAME's header (level %u, time "
                        "%02x%02x, members %u)\n",
                        b[0], b[8], b[7], b[0x0B]);
                bad = 1;
            }
        }
        /* RESTART THIS GAME -> the room, [0x34] cleared */
        /* 1000:390F: [0x138E] = 0 (extinct) and [0x1809] = 0xFF - the
         * level's caption + fades run and postLevelStep takes the room. */
        if (gameflow_pause_action(2, &quit) != FLOW_LEVEL || !level_ended() ||
            level_outcome() != 0 || !gameflow_restart_pending()) {
            fprintf(stderr, "FAIL RESTART THIS GAME\n");
            bad = 1;
        }
        /* QUIT THIS GAME */
        quit = 0;
        (void)gameflow_pause_action(4, &quit);
        if (!quit) {
            fprintf(stderr, "FAIL QUIT THIS GAME did not request the exit\n");
            bad = 1;
        }
    }

    /* 6. the time-up dialog (202F:0x5C2B): the 3-option block + the inline
     *    dispatch (0 restart this level, 1 the room, 2 quit) */
    {
        int32_t quit = 0;
        menu_open_time_up();
        if (!menu_active() || !menu_time_up()) {
            fprintf(stderr, "FAIL the time-up dialog did not open\n");
            bad = 1;
        }
        (void)_press(0x02u); /* row 1 */
        (void)_press(0x80u);
        if (!menu_time_up() || menu_result() != 1) {
            fprintf(stderr, "FAIL the time-up dialog gave %d (kind %d)\n",
                    menu_result(), menu_time_up());
            bad = 1;
        }
        menu_close();
        if (gameflow_time_up_action(0, &quit) != FLOW_LEVEL) {
            fprintf(stderr, "FAIL the time-up RESTART THIS LEVEL\n");
            bad = 1;
        }
        /* 1000:3510: the same lost-level arm as the pause menu's result 2 */
        if (gameflow_time_up_action(1, &quit) != FLOW_LEVEL || !level_ended() ||
            level_outcome() != 0 || !gameflow_restart_pending()) {
            fprintf(stderr, "FAIL the time-up RESTART THIS GAME\n");
            bad = 1;
        }
        quit = 0;
        (void)gameflow_time_up_action(2, &quit);
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
        for (uint32_t i = 0; i < 400u && level_starting(); i++) {
            level_tick();
            palette_tick();
            if (level_loading_screen()) level_loading_tick();
        }
        level_tick();
        menu_open_pause();
        menu_draw();
        _dump_ppm(dump_dir, "menu_port_pause", osys_null_page(0),
                  osys_null_palette());
        level_tick();
        (void)_press(0x02u);
        (void)_press(0x02u);
        (void)_press(0x02u);
        (void)_press(0x80u); /* the save grid */
        level_tick(); /* the fresh level frame */
        menu_draw();
        _dump_ppm(dump_dir, "menu_port_grid", osys_null_page(0),
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
