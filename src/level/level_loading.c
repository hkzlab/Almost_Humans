#include "game_resources.h"
#include "level/level.h"
#include "level/level_internal.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "statusbar.h"
#include "video.h"
#include "byteorder.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- load_sprite_pal's LOADING screen (1000:1DC0) ----------------------
 * 0xA0 frames of ten walkers (APL\LOADING.APL blocks 0..0x10, two 5-strong
 * groups converging at y=0x6E), then 0x56 frames of the eight "LOADING!"
 * letters (blocks 0x11..0x18) falling to y=0x56.  Both lists are the static
 * templates at 202F:0xD7E and 202F:0xDE4: [u16 count][{u16 x, u16 y,
 * s16 dx, s16 dy, u16 sprite}].  The sprite blit skips x outside
 * 0x40..0x1A0 (blit_sprite_scaled 1000:1F5A) and the templates' 0x25
 * sprite ids are past the 26-frame bank, so those entries draw nothing. */
static uint8_t *_loading_bank;
static uint32_t _loading_size;
static uint8_t _load_list[10u * 10u];
static uint8_t
    _load_walker_snap[10u * 10u];   // the walkers at the letters switch
static uint16_t _load_walker_count;
static uint8_t _load_pal[768];          // dicon+0x18: the screen's DAC
static uint8_t *_load_dic; // BACKS\DICON.APL: the preview card
static uint32_t _load_dic_size;
static uint16_t _load_entries;
static int16_t _load_cx; // the particle counter [0xD7A]
static uint16_t _load_reload; // the reload value [0xD7C]
static uint8_t _load_left; // particle_frame's CX
static uint8_t _load_phase; // 0 idle, 1 walkers, 2 letters, 3 done

static const uint8_t _load_walkers[2u + 10u * 10u] = {
    0x0A, 0x00, 0x40, 0x00, 0x6E, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x00, 0x6E, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00,
    0x6E, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x6E, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6E, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x25, 0x00, 0x90, 0x01, 0x6E, 0x00, 0xFF, 0xFF, 0x00, 0x00,
    0x08, 0x00, 0xA0, 0x01, 0x6E, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x08, 0x00,
    0xB0, 0x01, 0x6E, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x08, 0x00, 0xC0, 0x01,
    0x6E, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x08, 0x00, 0xD0, 0x01, 0x6E, 0x00,
    0xFF, 0xFF, 0x00, 0x00, 0x25, 0x00
};
static const uint8_t _load_letters[2u + 8u * 10u] = {
    0x08, 0x00, 0xB0, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x02, 0x00, 0x11, 0x00,
    0xC0, 0x00, 0x19, 0x00, 0x00, 0x00, 0x02, 0x00, 0x12, 0x00, 0xD0, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x02, 0x00, 0x13, 0x00, 0xE0, 0x00, 0x17, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x14, 0x00, 0xF0, 0x00, 0x16, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x15, 0x00, 0x00, 0x01, 0x15, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x16, 0x00, 0x10, 0x01, 0x14, 0x00, 0x00, 0x00, 0x02, 0x00, 0x17, 0x00,
    0x20, 0x01, 0x13, 0x00, 0x00, 0x00, 0x02, 0x00, 0x18, 0x00
};

/* --- load_sprite_pal's LOADING screen (1000:1DC0) ---------------------- */

/** @brief load_sprite_pal's palette step (1000:1DCF..1DF2): BACKS\DICON.APL's
 * 0x180 bytes at +0x18 are the screen's DAC (entries 0..0x7F).
 */
static void level_loading_begin(void)
{
    if (!_level_is_loaded() || _load_phase) return;

    /* level_video_init (1000:329d) calls reset_display_mode (1000:82c6)
     * before load_sprite_pal's screen: the level's 320x192 field */
    video_set_mode(VIDEO_MODE_X);

    /* 1000:1DCF..1E3F: dicon+0x18 -> 0x14D3 entries 0..0x7F, then the DAC
     * FADES to them (0x80 entries, div 0x19) before the frames run */
    memset(_load_pal, 0, sizeof _load_pal);
    g_osys->memFree(_load_dic);
    _load_dic = NULL;
    _load_dic_size = 0;
    /* 1000:1E9B's style table f,s,c,w,d,p indexed by style-1; 0xEA8 is
     * the 'd' in "backs\dicon.apl" - the level's own preview card */
    {
        static const char letter[6] = { 'f', 's', 'c', 'w', 'd', 'p' };
        uint8_t st = _level_record()->style;
        char icon[32];
        char l = (st >= 1u && st <= 6u) ? letter[st - 1u] : 'd';
        snprintf(icon, sizeof icon, game_resource_name(GAME_RES_BACKS_ICON_FMT),
                 l);
        _load_dic = res_load_chunk(icon, &_load_dic_size, NULL);
    }
    if (_load_dic && _load_dic_size >= 0x18u + 0x180u) {
        memcpy(_load_pal, _load_dic + 0x18, 0x180u);
        palette_build_fade(0u, 0x80u, 0x19u, _load_pal);
    }
    {
        const uint8_t *bank = statusbar_palette();
        if (bank) palette_set_dac(bank, 0xC0u, 0x40u);
    }
    _loading_bank = res_load_chunk(game_resource_name(GAME_RES_APL_LOADING_APL),
                                   &_loading_size, NULL);
    if (!_loading_bank) {
        _load_phase = 3;
        return;
    }

    memcpy(_load_list, _load_walkers + 2, sizeof _load_list);
    _load_entries = rd_le_u16(_load_walkers);
    _load_cx = 3; // particle_frame(3, 0xA0)
    _load_reload = 3;
    _load_left = 0xA0u;
    _load_phase = 1;
}

uint8_t level_loading_screen(void)
{
    if (_level_phase() != LVSTART_LOADING) return 0;
    if (!_load_phase) level_loading_begin();
    return _load_phase == 1 || _load_phase == 2;
}

/** @brief The background: a clean black page plus the style's DICON.APL card
 * at (0x78,0x60), above the level's 40-row status bar (152..191).  The
 * 1000:1F10 blit writes 20 bytes per plane from the 80-pixel source with the
 * page row stride 0x58; the image is 4 pixels per byte (80 px, not 160 - the
 * party compose's x>>2 law), so it must NOT be doubled.  The card is
 * opaque (index 0 = its black frame) - the level's style preview.
 *
 * @param page the page buffer
 */
static void _load_draw_background(uint8_t *page)
{
    /* load_sprite_pal sets the vertical display scroll [0x2] := 0x32
     * (0x31F2), so every page-relative y loses 0x32: the card's source
     * DI=0x324E row 0x92 becomes 0x60, exactly above the 152-row bar. */
    memset(page, 0, VIDEO_PAGE_BYTES);
    if (_load_dic && _load_dic_size >= 0x400u + 80u * 56u) {
        const uint8_t *src = _load_dic + 0x400u;
        for (uint16_t r = 0; r < 56u; r++) {
            uint8_t y = (uint8_t)(0x60u + r);
            if (y >= VIDEO_PAGE_H_MODEX) break;
            memcpy(page + y * VIDEO_PAGE_W + 0x78u, src + r * 80u, 80u);
        }
    }
    statusbar_set_active(0);
    statusbar_tick();
}

/**
 * @brief Blit one 16x24 loading-screen sprite at (x, y).
 *
 * @param page the page buffer
 * @param off the sprite offset in the loading bank
 * @param x the x coordinate
 * @param y the y coordinate
 */
static void _load_sprite(uint8_t *page, uint32_t off, int32_t x, int32_t y)
{
    for (uint16_t r = 0; r < 24u; r++)
        for (uint16_t c = 0; c < 16u; c++) {
            int32_t px = x + (int32_t)c;
            uint8_t v = _loading_bank[off + r * 16u + c];

            if (px < 0 || px >= (int32_t)VIDEO_PAGE_W || !v) continue;
            page[(y + r) * VIDEO_PAGE_W + (uint32_t)px] = v;
        }
}

/** @brief blit_sprite_scaled (1000:1F5A): the list x is in the 416-px source
 * coordinate space; the visible 320-px window starts 0x50 pixels in
 * (x/4 - 0x14 bytes), so screen x = list x - 0x50.  The frames are the
 * 16x24 8bpp cells at bank + 0x400 + id*0x180; 0 = transparent.
 *
 * @param page the page buffer
 * @param list the list
 * @param entries the entry count
 * @param lo the left x bound
 * @param hi the right x bound
 * @param yoff the y offset
 */
static void _load_draw_one(uint8_t *page, const uint8_t *list, uint16_t entries,
                           int32_t lo, int32_t hi, int32_t yoff)
{
    for (uint16_t e = 0; e < entries; e++) {
        const uint8_t *p = list + e * 10u;
        int32_t x = (int32_t)rd_le_u16(p) - 0x50;
        int32_t y = (int32_t)rd_le_u16(p + 2) + yoff;
        uint16_t spr = rd_le_u16(p + 8);
        if (x <= lo || x >= hi) continue; // the screen window
        uint32_t off = 0x400u + spr * 0x180u;
        if (off + 0x180u > _loading_size) continue; // the 0x25 sentinel
        if (y < 0 || y + 24 > (int32_t)VIDEO_PAGE_H_MODE13H) continue;
        _load_sprite(page, off, x, y);
    }
}

/**
 * @brief Draw the loading screen's progress list.
 *
 * @param page the page buffer
 */
static void _load_draw_list(uint8_t *page)
{
    /* the arms-up group is the eight under the eight letters (the two edge
     * walkers at screen x=0x50/0xE0 are outside the LOADING! span) */
    if (_load_phase == 2 && _load_walker_count)
        _load_draw_one(page, _load_walker_snap, _load_walker_count, 0x60 - 1,
                       0xD0 + 1, -0x32);
    _load_draw_one(page, _load_list, _load_entries, -16, VIDEO_PAGE_W, -0x32);
}

void level_loading_tick(void)
{
    uint8_t *page;

    if (_load_phase != 1 && _load_phase != 2) return;
    page = g_osys->videoPage(0);

    if (--_load_cx < 0) _load_cx = _load_reload;
    for (uint16_t e = 0; e < _load_entries; e++) {
        uint8_t *p = _load_list + e * 10u;
        int32_t x = (int32_t)rd_le_u16(p), y = (int32_t)rd_le_u16(p + 2);
        int32_t dx = (int32_t)(int16_t)rd_le_u16(p + 4),
                dy = (int32_t)(int16_t)rd_le_u16(p + 6);
        uint16_t spr = rd_le_u16(p + 8);
        x += dx;
        if (dy) {
            y += dy;
            if (y > 0x56) y = 0x56;
        }
        /* 1000:1EBD..1EC7: on the batch's last pass (BP == 1, the
         * 0xA0th/0x56th tick) every frame below 0x10 is raised to 0x10
         * before the cycle tests it. */
        if (_load_left == 1u && spr < 0x10u) spr = 0x10u;
        if (_load_cx == 0 && spr < 0x10u) {
            spr++;
            if (spr == 8u) spr = 0u;
            else if (spr == 0x10u) spr = 8u;
        }
        wr_le_u16(p, (uint16_t)((uint32_t)x));
        wr_le_u16(p + 2, (uint16_t)((uint32_t)y));
        wr_le_u16(p + 8, spr);
    }
    if (page) {
        _load_draw_background(page);
        _load_draw_list(page);
    }

    if (--_load_left == 0) {
        if (_load_phase == 1) {
            if (page) {
                _load_draw_background(page);
                _load_draw_list(page);
            } // the extra 1000:1EF8 draw
            memcpy(_load_walker_snap, _load_list, sizeof _load_walker_snap);
            _load_walker_count = _load_entries;
            for (uint16_t e = 0; e < _load_walker_count; e++)
                wr_le_u16(_load_walker_snap + e * 10u + 8u,
                          0x10u); // arms-up hold
            memcpy(_load_list, _load_letters + 2, 8u * 10u);
            _load_entries = rd_le_u16(_load_letters);
            _load_cx = 1;
            _load_reload = 1; // particle_frame(1, 0x56)
            _load_left = 0x56u;
            _load_phase = 2;
        } else {
            _load_phase = 3;
            g_osys->memFree(_load_dic);
            _load_dic = NULL;
            _load_dic_size = 0;
        }
    }
}

void _level_loading_reset(void)
{
    g_osys->memFree(_loading_bank);
    _loading_bank = NULL;
    _loading_size = 0;
    _load_phase = 0; // the LOADING screen repeats
}
