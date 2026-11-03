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
#include "game.h"
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUMP_N (sizeof dumpAt / sizeof dumpAt[0])

enum {
    SCRIPT_N = 8700
};

extern void osys_null_inputScript(const InputState *states, uint32_t n);
extern const uint8_t *osys_null_page(int32_t which);
extern const uint8_t *osys_null_palette(void);
extern uint32_t osys_null_pollCount(void);

extern void osys_null_onPresent(void (*fn)(uint32_t poll));

extern const OSystem osys_null;

/* Optional frame evidence (argv[3] = a directory): page 0 + the DAC shadow
 * at the flow's milestones, so the room -> party -> .ANI -> level relay can
 * be looked at instead of guessed. */
static const char *dump_dir;
static uint32_t dumped;

/* the milestones: the room's exit, the party, the LOADING screen and the
 * level's first play frames (the goal page is dismissed by the held key) */
static const uint32_t dumpAt[] = { 6600, 6900, 6930, 7100, 7300, 7500,
                                   7650, 7800, 7900, 8100, 8300, 8600 };
static const char *dumpName[] = { "flow_6600_room",      "flow_6900_roomexit",
                                  "flow_6930_party_a",   "flow_7100_party_b",
                                  "flow_7300_loading_a", "flow_7500_loading_b",
                                  "flow_7650_level_a",   "flow_7800_level_b",
                                  "flow_7900_level_c",   "flow_8100_level_d",
                                  "flow_8300_level_e",   "flow_8600_level_f" };

static int32_t sawPause;
/* the goal page (scroll_map 1000:9E51): the actor must not move while it is
 * up even with a direction held, and the page must be seen at all */
static int32_t sawGoal, goalMoved, goalTicks, goalX;

static uint8_t *read_file(const char *path, uint32_t *size)
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

static void dump_ppm(const char *name, const uint8_t *page)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", dump_dir, name);
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
    dumped++;
}

static void present_hook(uint32_t poll)
{
    /* D-15: the ESC at 8000 must have the pause block up on the next present;
     * the Enter at 8050 selects CONTINUE and closes it. */
    if (poll >= 8001u && poll <= 8051u && menu_active()) sawPause = 1;
    if (gameflow_goalPageUp()) {
        uint8_t *a = object_slot(object_actionEntity());
        int32_t x = a ? (int32_t)(int16_t)(a[0] | (a[1] << 8)) : -1;

        if (!sawGoal) {
            sawGoal = 1;
            goalX = x;
        } else if (x != goalX) goalMoved = 1;
        goalTicks++;
    }
    if (!dump_dir || dumped >= DUMP_N) return;
    if (poll < dumpAt[dumped]) return;
    while (dumped < DUMP_N && poll >= dumpAt[dumped]) {
        dump_ppm(dumpName[dumped], osys_null_page(0));
        dumped++;
    }
}

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
     *   0..6599    idle     — the boot intro plays (~5887 ticks: the
     *                           gametek logo, the idi/first/itb cards, the
     *                           scene), then the attract cycle starts
     *                           (game_loop: effects 1->3->2->4)
     *   6600       fire     — the attract escape (attract_exit 1000:13E9):
     *                           the 13B2 fade (~35 ticks) then FLOW_MENU
     *                           (the room loads code2.raw + fades in)
     *   6700..6950 right    — the room's confirm: the rails walk
     *                           0x1D2 -> 0x208 (cell 6) -> 0x280, the
     *                           right-edge exit matches "DARWIN" (level 1
     *                           = levelId 0) and fades out -> FLOW_LEVEL
     *                           (disk probe -> party -> LOADING -> level;
     *                           the newspaper is POST-level, entry 00FF)
     *   7150       key      — leaves the party's SET1W window
     *   7250..7700 key      — the level's goal page waits on any key
     *                           (scroll_map 9E51); the page follows the
     *                           LOADING screen
     *   7702..8698 idle      — the level proper
     *   8699       quit     — game_main exits                                */
    static InputState script[SCRIPT_N];
    memset(script, 0, sizeof script);
    /* the attract cycle runs from the boot's end (~5887) until the fire
     * at 6600 escapes it; the room then walks right off the edge */
    script[6600].joyBits = 0x80u;         /* the attract escape */
    for (uint32_t i = 6700; i <= 6950u; i++)
        script[i].joyBits = 0x08u; /* hold RIGHT: the room's confirm */
    /* the party pre-screen (t≈6912..7571, per the probe's timeline): its
     * wait fires [0x48A3]-shaped aborts exactly like the original's
     * wait_frames_or_key — one key inside the first set's wait exits  */
    script[7150].lastKey = 1u; /* the party's SET1W window    */
    /* the LOADING screen follows, then the level's goal page.  Hold RIGHT
     * (no key) while the page is up: scroll_map (1000:9E51) polls input only,
     * so the actor must stay put; the key at 7695 dismisses the page and the
     * play loop follows. */
    for (uint32_t i = 7200u; i <= 7690u; i++) script[i].joyBits = 0x08u;
    script[7695].lastKey = 1u;
    /* D-15: ESC opens the pause block in the play loop; Enter on row 0
     * (CONTINUE) closes it again. */
    /* D-41/D-42 through the real flow: F2 selects record 1 (the second
     * human), SPACE steps the action row's selected cell. */
    script[7750].keyScan = 0x3Cu; /* F2 */
    script[7760].keyScan = 0x39u; /* SPACE */
    script[8000].keyScan = 0x01u; /* ESC: the key list's id 4 */
    script[8050].lastKey = 0x0Du;
    script[SCRIPT_N - 1].quit = 1u;

    dump_dir = argc > 3 ? argv[3] : NULL;
    osys_null_inputScript(script, SCRIPT_N);
    osys_null_onPresent(present_hook);

    if (game_main(&osys_null) != 0) {
        fprintf(stderr, "FAIL game_main returned nonzero\n");
        return 1;
    }
    if (!sawPause) {
        fprintf(stderr,
                "FAIL D-15: ESC did not open the pause menu in game_main\n");
        bad = 1;
    }
    /* D-40: the goal page was up (scroll_map) with RIGHT held and the actor
     * did not move: the page's loop polls input only. */
    if (!sawGoal || goalTicks < 5) {
        fprintf(stderr, "FAIL the goal page was not reached (%d ticks)\n",
                goalTicks);
        bad = 1;
    } else if (goalMoved) {
        fprintf(stderr, "FAIL the actor moved while the goal page was up\n");
        bad = 1;
    }
    /* D-41: the F2 make at 7750 selected record 1; D-42: the SPACE make at
     * 7760 stepped the action row's cell from 1 to 2. */
    if (object_actionEntity() != 1u) {
        fprintf(stderr, "FAIL F2 did not select record 1 (entity %u)\n",
                object_actionEntity());
        bad = 1;
    }
    if (statusbar_actionCell(1u) != 2u) {
        fprintf(stderr, "FAIL SPACE did not step the action cell (cell %u)\n",
                statusbar_actionCell(1u));
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
        uint8_t *sp = read_file(path, &sp_size);
        snprintf(path, sizeof path, "%s/HUMANS.001/LEVELS/BS3.BLK", fix_dir);
        uint8_t *bs = read_file(path, &bs_size);
        snprintf(path, sizeof path, "%s/HUMANS.001/BACKS/BG4.RAW", fix_dir);
        uint8_t *bg4 = read_file(path, &bg4_size);
        snprintf(path, sizeof path, "%s/HUMANS.001/LBM/PANEL4.BIT", fix_dir);
        uint8_t *p4 = read_file(path, &p4_size);
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
