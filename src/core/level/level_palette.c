#include "game_resources.h"
#include "level/level.h"
#include "level/level_internal.h"
#include "object/object.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "statusbar.h"

#include <stdint.h>
#include <string.h>

static uint8_t _level_pal[768];     // the working palette 202F:0x14D3

/* DS:0x1413 - the shipped 64-entry gradient that draw_level_text copies into
 * palette entries 0x80..0xBF (0x33F2, 0xC0 bytes); matches the live runtime
 * buffer (DS 0x123A:0x1413). */
static const uint8_t _level_end_gradient[0xC0] = {
    0x3Cu, 0x28u, 0x3Cu, 0x3Cu, 0x26u, 0x3Cu, 0x3Cu, 0x24u, 0x3Cu, 0x3Cu, 0x22u,
    0x3Cu, 0x3Cu, 0x20u, 0x3Cu, 0x3Cu, 0x1Eu, 0x3Cu, 0x3Cu, 0x1Cu, 0x3Cu, 0x3Cu,
    0x1Au, 0x3Cu, 0x3Cu, 0x18u, 0x3Cu, 0x3Cu, 0x16u, 0x3Cu, 0x3Cu, 0x14u, 0x3Cu,
    0x3Cu, 0x12u, 0x3Cu, 0x3Cu, 0x10u, 0x3Cu, 0x3Cu, 0x0Eu, 0x3Cu, 0x3Cu, 0x0Cu,
    0x3Cu, 0x3Cu, 0x0Au, 0x3Cu, 0x3Cu, 0x0Cu, 0x3Cu, 0x3Cu, 0x0Bu, 0x38u, 0x3Cu,
    0x0Bu, 0x35u, 0x3Cu, 0x0Bu, 0x32u, 0x3Cu, 0x0Bu, 0x2Fu, 0x3Cu, 0x0Au, 0x2Bu,
    0x3Cu, 0x0Au, 0x28u, 0x3Cu, 0x0Au, 0x25u, 0x3Cu, 0x0Au, 0x22u, 0x3Cu, 0x09u,
    0x1Eu, 0x3Cu, 0x09u, 0x1Bu, 0x3Cu, 0x09u, 0x18u, 0x3Cu, 0x09u, 0x15u, 0x3Cu,
    0x08u, 0x11u, 0x3Cu, 0x08u, 0x0Eu, 0x3Cu, 0x08u, 0x0Bu, 0x3Cu, 0x08u, 0x08u,
    0x3Cu, 0x07u, 0x07u, 0x3Cu, 0x06u, 0x06u, 0x3Cu, 0x05u, 0x05u, 0x3Cu, 0x04u,
    0x04u, 0x3Cu, 0x03u, 0x03u, 0x3Cu, 0x02u, 0x02u, 0x3Cu, 0x01u, 0x01u, 0x3Cu,
    0x00u, 0x00u, 0x39u, 0x00u, 0x00u, 0x37u, 0x00u, 0x00u, 0x34u, 0x00u, 0x00u,
    0x32u, 0x00u, 0x00u, 0x2Fu, 0x00u, 0x00u, 0x2Du, 0x00u, 0x00u, 0x2Au, 0x00u,
    0x00u, 0x28u, 0x00u, 0x00u, 0x25u, 0x00u, 0x00u, 0x23u, 0x00u, 0x00u, 0x20u,
    0x00u, 0x00u, 0x1Eu, 0x00u, 0x00u, 0x1Bu, 0x00u, 0x00u, 0x19u, 0x00u, 0x00u,
    0x16u, 0x00u, 0x00u, 0x14u, 0x00u, 0x00u, 0x11u, 0x00u, 0x00u, 0x0Fu, 0x00u,
    0x00u, 0x0Cu, 0x00u, 0x00u, 0x0Au, 0x00u, 0x00u, 0x07u, 0x00u, 0x00u, 0x05u,
    0x00u, 0x00u, 0x02u, 0x00u, 0x00u,
};

/** @brief Build the working palette 0x14D3 from the level's asset banks:
 * SPRPAL.APL's UI entries, the tile atlas, the BG palette, the BD row
 * block and the bar's bank (1000:4FBB/0x4EE9/0x4D58/0x4BBC).
 */
void _level_palette_build(void)
{
    /* The working palette 0x14D3 (the level's DAC base): entries
     * 1..31 = APL\SPRPAL.APL+0x1B (the UI; entry 0 keeps the loading
     * screen's dicon triple - 1000:4FD9's SI = 0x1B / DI = 0x14D6 / CX =
     * 0x5D, D-142), 32..63 =
     * LEVELS\BS<style>.BLK+0x18 (the tiles), 64..127 = the BG's own
     * palette, 128..191 = zero, 192..255 = panel4's (level_video_init's
     * 0xC0-byte 0x1713 copy).  Then the level's fade-in (level_fade_in
     * 1000:337E/338A: the final 256-entry fade, div 0x32). */
    memset(_level_pal, 0, sizeof _level_pal);
    {
        uint32_t spSize = 0;
        uint8_t *sp = res_load_chunk(
            game_resource_name(GAME_RES_APL_SPRPAL_APL), &spSize, NULL);
        if (sp && spSize >= 0x1Bu + 0x5Du)
            memcpy(_level_pal + 3u, sp + 0x1Bu, 0x5Du);
        g_osys->memFree(sp);
    }
    {
        const uint8_t *atlasPal = _level_atlas_palette();
        const uint8_t *bgPal = _level_bg_palette();

        if (atlasPal) memcpy(_level_pal + 32u * 3u, atlasPal, 0x60u);
        if (bgPal) memcpy(_level_pal + 64u * 3u, bgPal, 0xC0u);
    }
    /* copy_level_row 0x4EE9: the BD row block fills 0x80..0xBF. */
    memcpy(_level_pal + 0x80u * 3u, _level_row_block(), 0xC0u);
    {
        const uint8_t *bank = statusbar_palette();
        if (bank) { memcpy(_level_pal + 0xC0u * 3u, bank, 0xC0u); }
    }
}

const uint8_t *level_palette(void) { return _level_pal; }

void level_palette_score_prepare(const uint8_t *cardPalette)
{
    palette_seal_triples(_level_pal, 0x80u, 0x60u);
    if (cardPalette) memcpy(_level_pal + 0xC0u * 3u, cardPalette, 0xC0u);
}

void level_end_palette(void)
{
    palette_seal_triples(_level_pal, 0u, 0x80u);
    memcpy(_level_pal + 0x80u * 3u, _level_end_gradient,
           sizeof _level_end_gradient);
}

/** @brief palette_rotate_table (1000:8C78), from the level's ISR hook
 * (1000:339D): the per-range countdown and the DAC re-send.
 */
void _level_palette_rotate(void)
{
    /* palette_rotate_table (1000:8C78), from the level's ISR hook
     * (1000:339D): per range, a signed countdown reloaded from `reload`;
     * when it wraps negative the range rotates toward the higher entries
     * (entry 0 takes the last one) and those DAC entries are re-sent.
     * The original reads the table from 202F:0x1FC6, i.e. field M.  The
     * hook is installed only after level_fade_in's fade completes (the
     * fade chases this same buffer), so no rotation while one is running.
     * The claim also freezes it (`[0x410C] = 0xFF` at 1000:3B1E). */
    uint16_t count = 0;
    uint8_t *table = _level_anim_ranges(&count);

    for (uint16_t e = 0;
         e < count && _level_phase() == LVSTART_PLAYING &&
         !palette_fade_active() && !object_sequence_claim_active();
         e++) {
        uint8_t *a = table + 4u * e;
        uint8_t first = a[2], n = a[3];
        if (n < 2u || first + n > 256u) continue;
        a[0] = (uint8_t)(a[0] - 1u);
        if (!(a[0] & 0x80u)) continue; // still non-negative
        a[0] = a[1];
        uint8_t *p = _level_pal + first * 3u;
        uint8_t last[3];
        memcpy(last, p + (n - 1u) * 3u, 3);
        for (uint16_t i = n - 1u; i > 0u; i--)
            memcpy(p + i * 3u, p + (i - 1u) * 3u, 3);
        memcpy(p, last, 3);
        palette_set_dac(p, first, n);
    }
}
