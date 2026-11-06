/* test_smoke.c — full-game_main smoke test under the null backend.
 *
 * A scripted input sequence drives the whole flow: the attract dwell, fire
 * into the room, walk right into level 1, leave the party, the LOADING
 * screen, the goal page (dismissed by the held key) and the level; quit.
 * The newspaper is post-level, so it is not on this path.  Asserts
 * game_main exits cleanly with the level's rendered page on page 0.
 *
 * argv[1] = the game data directory holding HUMANS.000/001/002.
 * argv[2] = the extracted-tree fixture root (expected bytes).
 * Missing volumes = skip. */
#include "gameflow.h"
#include "menu.h"
#include "object.h"
#include "osystem.h"
#include "statusbar.h"
#include "compat.h"
#include "byteorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUMP_N (sizeof _dump_at / sizeof _dump_at[0])

enum {
    SCRIPT_N = 9500
};

extern void osys_null_input_script(const InputState *states, uint32_t n);
extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);
extern uint32_t osys_null_poll_count(void);

extern void osys_null_on_present(void (*fn)(uint32_t poll));

extern const OSystem g_osys_null;
extern uint32_t osys_null_audio_installs(void);
extern uint32_t osys_null_audio_loads(void);
extern uint32_t osys_null_audio_load_size(uint32_t i);
extern uint32_t osys_null_audio_starts(void);
extern uint32_t osys_null_audio_stops(void);
extern uint32_t osys_null_audio_ticks(void);
extern uint32_t osys_null_audio_sfx_count(void);
extern uint8_t osys_null_audio_tempo(void);

/* Optional frame evidence (argv[3] = a directory): page 0 + the DAC shadow
 * at the flow's milestones, so the room -> party -> .ANI -> level relay can
 * be looked at instead of guessed. */
static const char *_dump_dir;
static uint32_t _dumped;

/* the milestones: the room's exit, the party, the LOADING screen and the
 * level's first play frames (the goal page is dismissed by the held key) */
static const uint32_t _dump_at[] = { 7400, 7500, 7900, 8100, 8350, 8600,
                                     8750, 8800, 8900, 8950, 9100, 9250 };
static const char *_dump_name[] = {
    "flow_7400_roomexit", "flow_7500_room",    "flow_7900_party_a",
    "flow_8100_party_b",  "flow_8350_loading_a", "flow_8600_loading_b",
    "flow_8750_goal",     "flow_8800_level_a", "flow_8900_level_b",
    "flow_8950_level_c",  "flow_9100_level_d", "flow_9250_level_e"
};

static int32_t _saw_pause;
/* the goal page (scroll_map 1000:9E51): the actor must not move while it is
 * up even with a direction held, and the page must be seen at all */
static int32_t _saw_goal, _goal_moved, _goal_ticks, _goal_x;
static uint32_t _goal_poll;

/**
 * @brief Read a whole file into a malloc'd buffer; NULL on error.
 *
 * @param path the path
 * @param size the size
 * @return the pointer, or NULL when unavailable
 */
static uint8_t *_read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    int64_t n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (size) *size = (uint32_t)n;
    return buf;
}

/**
 * @brief Write a page and its palette as a binary PPM for inspection.
 *
 * @param name the name
 * @param page the page buffer
 */
static void _dump_ppm(const char *name, const uint8_t *page)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", _dump_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    const uint8_t *pal = osys_null_palette();
    for (uint32_t i = 0; i < 64000u; i++) {
        uint32_t v = page[i];
        uint8_t rgb[3] = { (uint8_t)(pal[v * 3] << 2),
                           (uint8_t)(pal[v * 3 + 1] << 2),
                           (uint8_t)(pal[v * 3 + 2] << 2) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    _dumped++;
}

/**
 * @brief The null backend's present hook: record the observed flow events.
 *
 * @param poll the poll counter
 */
static void _present_hook(uint32_t poll)
{
    /* D-15: the ESC at 9177 must have the pause block up on the next present;
     * the Enter at 9227 selects CONTINUE and closes it. */
    if (poll >= 9178u && poll <= 9228u && menu_active()) _saw_pause = 1;
    if (gameflow_goal_page_up()) {
        uint8_t *a = object_slot(object_action_entity());
        int32_t x = a ? (int32_t)(int16_t)(rd_le_u16(a + 0)) : -1;

        if (!_saw_goal) {
            _saw_goal = 1;
            _goal_poll = poll;
            _goal_x = x;
        } else if (x != _goal_x) _goal_moved = 1;
        _goal_ticks++;
    }
    if (!_dump_dir || _dumped >= DUMP_N) return;
    if (poll < _dump_at[_dumped]) return;
    while (_dumped < DUMP_N && poll >= _dump_at[_dumped]) {
        _dump_ppm(_dump_name[_dumped], osys_null_page(0));
        _dumped++;
    }
}

/**
 * @brief Run the end-to-end smoke test.
 *
 * @param argc the argument count
 * @param argv the argument vector
 * @return 0 on success, 1 on failure
 */
int main(int argc, char **argv)
{
    const char *vol_dir = argc > 1 ? argv[1] : ".";
    const char *fix_dir = argc > 2 ? argv[2] : vol_dir;

    char path[512];
    snprintf(path, sizeof path, "%s/HUMANS.002", vol_dir);
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

    int32_t bad = 0;

    /* the input script (per eventsPoll call):
     *   0..7399    idle     — the boot intro plays (~6687 ticks: the
     *                           gametek logo, the idi/first/itb cards, the
     *                           scene — ~5887 plus D-248's ~800 extra scene
     *                           ticks), then the attract cycle starts
     *                           (game_loop: effects 1->3->2->4)
     *   7400       fire     — the attract escape (attract_exit 1000:13E9):
     *                           the 13B2 fade (~35 ticks) then FLOW_MENU
     *                           (the room loads code2.raw + fades in)
     *   7500..7750 right    — the room's confirm: the rails walk
     *                           0x1D2 -> 0x208 (cell 6) -> 0x280, the
     *                           right-edge exit matches "DARWIN" (level 1
     *                           = levelId 0) and fades out -> FLOW_LEVEL
     *                           (disk probe -> party -> LOADING -> level;
     *                           the newspaper is POST-level, entry 00FF)
     *   7950       key      — leaves the party's SET1W window
     *   8000..8490 key      — the level's goal page waits on any key
     *                           (scroll_map 9E51); the page follows the
     *                           LOADING screen
     *   8502..9498 idle      — the level proper
     *   9499       quit     — game_main exits                                */
    static InputState script[SCRIPT_N];
    memset(script, 0, sizeof script);
    /* the attract cycle runs from the boot's end (~6687) until the fire
     * at 7400 escapes it; the room then walks right off the edge */
    script[7400].joyBits = 0x80u;         /* the attract escape */
    for (uint32_t i = 7500; i <= 7750u; i++)
        script[i].joyBits = 0x08u; /* hold RIGHT: the room's confirm */
    /* the party pre-screen (t≈7712..8371, per the probe's timeline): its
     * wait fires [0x48A3]-shaped aborts exactly like the original's
     * wait_frames_or_key — one key inside the first set's wait exits  */
    script[8100].lastKey = 1u; /* the party's SET1W window    */
    /* the LOADING screen follows, then the level's goal page.  Hold RIGHT
     * (no key) while the page is up: scroll_map (1000:9E51) polls input only,
     * so the actor must stay put; the key at 8495 dismisses the page and the
     * play loop follows. */
    for (uint32_t i = 8777u; i <= 8867u; i++) script[i].joyBits = 0x08u;
    script[8872].lastKey = 1u;
    /* D-15: ESC opens the pause block in the play loop; Enter on row 0
     * (CONTINUE) closes it again. */
    /* D-41/D-42 through the real flow: F2 selects record 1 (the second
     * human), SPACE steps the action row's selected cell. */
    script[8927].keyScan = 0x3Cu; /* F2 */
    script[8937].keyScan = 0x39u; /* SPACE */
    script[9177].keyScan = 0x01u; /* ESC: the key list's id 4 */
    script[9227].lastKey = 0x0Du;
    script[SCRIPT_N - 1].quit = 1u;

    _dump_dir = argc > 3 ? argv[3] : NULL;
    osys_null_input_script(script, SCRIPT_N);
    osys_null_on_present(_present_hook);

    if (game_main(&g_osys_null) != 0) {
        fprintf(stderr, "FAIL game_main returned nonzero\n");
        return 1;
    }
    /* D-01: the audio wiring ran through the whole flow — the driver
     * installed once, the boot/party/level tunes loaded, the sequencer
     * ticked and the master volume was driven. */
    if (osys_null_audio_installs() != 1u) {
        fprintf(stderr, "FAIL audio: installs %u, want 1\n",
                osys_null_audio_installs());
        bad = 1;
    }
    /* the tune images, recognised by their shipped byte sizes (the boot's
     * TITLE.ALB 17080, the party's PARTY.ALB 3192, the level's LEVA.ALB 7032) */
    if (osys_null_audio_loads() != 3u ||
        osys_null_audio_load_size(0u) != 17080u ||
        osys_null_audio_load_size(1u) != 3192u ||
        osys_null_audio_load_size(2u) != 7032u) {
        fprintf(stderr,
                "FAIL audio: tune loads %u [%u %u %u], want 3 "
                "[title 17080, party 3192, levA 7032]\n",
                osys_null_audio_loads(), osys_null_audio_load_size(0u),
                osys_null_audio_load_size(1u), osys_null_audio_load_size(2u));
        bad = 1;
    }
    if (osys_null_audio_starts() < 3u) {
        fprintf(stderr, "FAIL audio: only %u resumes\n",
                osys_null_audio_starts());
        bad = 1;
    }
    if (osys_null_audio_ticks() == 0u) {
        fprintf(stderr, "FAIL audio: the sequencer never ticked\n");
        bad = 1;
    }
    if (osys_null_audio_tempo() == 0u) {
        fprintf(stderr, "FAIL audio: the master volume never left 0\n");
        bad = 1;
    }
    if (!_saw_pause) {
        fprintf(stderr,
                "FAIL D-15: ESC did not open the pause menu in game_main\n");
        bad = 1;
    }
    /* D-40: the goal page was up (scroll_map) with RIGHT held and the actor
     * did not move: the page's loop polls input only. */
    if (!_saw_goal || _goal_ticks < 5) {
        fprintf(stderr, "FAIL the goal page was not reached (%d ticks)\n",
                _goal_ticks);
        bad = 1;
    } else if (_goal_moved) {
        fprintf(stderr, "FAIL the actor moved while the goal page was up\n");
        bad = 1;
    }
    /* D-01's timing tail: the level's first goal frame follows the party's
     * music fade-in and exit hold and the level's own music fade-in.  The
     * measured poll is ~8822; a missing hold shifts it by 150 and fails. */
    if (_goal_poll < 8780u || _goal_poll > 8900u) {
        fprintf(stderr,
                "FAIL the goal page arrived at poll %u, want ~8822 "
                "(the audio fade holds)\n", _goal_poll);
        bad = 1;
    }
    /* D-41: the F2 make at 7750 selected record 1; D-42: the SPACE make at
     * 7760 stepped the action row's cell from 1 to 2. */
    if (object_action_entity() != 1u) {
        fprintf(stderr, "FAIL F2 did not select record 1 (entity %u)\n",
                object_action_entity());
        bad = 1;
    }
    if (statusbar_action_cell(1u) != 2u) {
        fprintf(stderr, "FAIL SPACE did not step the action cell (cell %u)\n",
                statusbar_action_cell(1u));
        bad = 1;
    }

    /* the page: the level's rendered viewport (level_tick: BG4 + the terrain
     * tiles at scroll 0,0 + the status bar's band) — the byte-exact
     * composite is test_level/test_statusbar's; here the assertion is the
     * coverage (the measured nonzero count of the level-0 composite's
     * first 184 rows, which since the bar include rows 152..183 of its
     * 0xC0-tagged band, and since the object pass the three visible placed
     * cavemen at (48,24) (208,24) (256,136)) */
    {
        const uint8_t *page = osys_null_page(0);
        uint32_t nz = 0;
        for (uint32_t i = 0; i < 58880u; i++)
            if (page[i]) nz++;
        /* the BG now shows through the tiles' zero bytes (draw_terrain
         * 0x502d skips them) and its 0xFF fill is the 0x80..0xBF bank, so the
         * whole 184-row window is populated. */
        if (nz != 58880u) {
            fprintf(stderr, "FAIL page: the level render nz %u, want 58880\n",
                    nz);
            bad = 1;
        }
    }

    /* the palette after the level's fade-in = the working palette
     * 202F:0x14D3 (level_fade_in's final 256-entry fade, div 0x32):
     *   0..31   APL\SPRPAL.APL +0x18  (the UI)
     *   32..63  LEVELS\BS3.BLK +0x18  (the tiles)
     *   64..127 BG4.RAW's own palette
     *   128..191 zero
     *   192..255 LBM\PANEL4.BIT +0xFA00 */
    {
        uint32_t sp_size = 0, bs_size = 0, bg4_size = 0, p4_size = 0;
        snprintf(path, sizeof path, "%s/HUMANS.001/APL/SPRPAL.APL", fix_dir);
        uint8_t *sp = _read_file(path, &sp_size);
        snprintf(path, sizeof path, "%s/HUMANS.001/LEVELS/BS3.BLK", fix_dir);
        uint8_t *bs = _read_file(path, &bs_size);
        snprintf(path, sizeof path, "%s/HUMANS.001/BACKS/BG4.RAW", fix_dir);
        uint8_t *bg4 = _read_file(path, &bg4_size);
        snprintf(path, sizeof path, "%s/HUMANS.001/LBM/PANEL4.BIT", fix_dir);
        uint8_t *p4 = _read_file(path, &p4_size);
        if (!sp || sp_size < 0x18u + 0x60u || !bs || bs_size < 0x18u + 0x60u ||
            !bg4 || bg4_size != 163840u + 768u || !p4 ||
            p4_size < 0xFA00u + 768u) {
            fprintf(stderr, "FAIL palette fixtures unreadable\n");
            bad = 1;
        } else {
            const uint8_t *pal = osys_null_palette();
            const uint8_t *want[5] = { sp + 0x18, bs + 0x18, bg4 + 163840u,
                                       NULL, p4 + 0xFA00u };
            uint32_t off[5] = { 0u, 0x60u, 0xC0u, 0x180u, 0x240u };
            uint32_t len[5] = { 0x60u, 0x60u, 0xC0u, 0xC0u, 0xC0u };
            /* the BG block's entries 0x71..0x74 are the level's animated
             * range (BD3's field M: one rotation every 4 ticks), so they are
             * compared below as a rotation of the file's four entries; every
             * other byte is exact. */
            const uint32_t animLo = 0x93u, animHi = 0x9Fu; /* in the block */
            for (uint32_t r = 0; r < 5u; r++)
                for (uint32_t i = 0; i < len[r]; i++) {
                    if (r == 2u && i >= animLo && i < animHi) continue;
                    uint8_t w = want[r] ? want[r][i] : 0u;
                    if (pal[off[r] + i] != w) {
                        fprintf(stderr,
                                "FAIL palette 0x%X: got %02x want %02x\n",
                                off[r] + i, pal[off[r] + i], w);
                        bad = 1;
                        break;
                    }
                }
            {
                const uint8_t *f = want[2] + animLo; /* the 4 entries */
                uint32_t rot = 0;
                for (uint32_t k = 0; k < 4u && !rot; k++) {
                    uint32_t ok = 1;
                    for (uint32_t i = 0; i < 4u; i++)
                        for (uint32_t c = 0; c < 3u; c++)
                            ok &= pal[0xC0u + animLo + i * 3u + c] ==
                                  f[((i + 4u - k) % 4u) * 3u + c];
                    rot = ok;
                }
                if (!rot) {
                    fprintf(stderr, "FAIL palette: the animated range is not a "
                                    "rotation of the BG's 0x71..0x74\n");
                    bad = 1;
                }
            }
        }
        free(sp);
        free(bs);
        free(bg4);
        free(p4);
    }

    if (!bad)
        printf(
            "smoke test: OK (flow + the boot intro + the level + the palette)\n");
    return bad;
}
