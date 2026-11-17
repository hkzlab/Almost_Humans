#include "video.h"
#include "osystem.h"

#include <stdint.h>
#include <string.h>

/* shortcut: the A000 model is always recomposed in software; a backend with
 * native planar output would move it behind an optional module (D-04). */

/* the A000 space, flat: pixel 4*addr + plane is one byte, so the four plane
 * writes of one address are four contiguous bytes. */
static uint8_t _vram[65536u * 4u];

/* The shared text/scratch arena: 200 rows x 80 bytes.  The original carves
 * the same region twice - the attract's 16000-B arena ([0xC9D], game_loop
 * 1000:1369) and the level's [0x1E20] (init_buffer_ptrs 1000:31DA) - and
 * every screen that draws text through it (the attract's effect 2, the room's
 * name entry, the briefing, the dialogs).  The port takes one block when a
 * screen first uses it and releases it when that screen exits
 * (video_arena_release). */
#define ARENA_BYTES 16000u
static uint8_t *_arena;

int32_t video_init(void) { return 0; }

void video_tick(void) { /* the flip is the backend's videoPresent */ }

uint8_t *video_page(int32_t which) { return g_osys->videoPage(which); }

uint8_t *video_vram_group(uint16_t addr) { return &_vram[(uint32_t)addr * 4u]; }

void video_vram_clear(void) { memset(_vram, 0, sizeof _vram); }

/**
 * @brief Copy or zero one row through the wrapping 64-KB address space.
 *
 * @param addr the row's first address
 * @param src the source row, or NULL to zero
 * @param bytes the byte count
 */
static void _vram_row(uint16_t addr, const uint8_t *src, uint32_t bytes)
{
    uint32_t d = (uint32_t)addr * 4u;
    uint32_t first = (uint32_t)sizeof _vram - d;
    uint32_t rest = 0u;

    if (first > bytes) first = bytes;
    else rest = bytes - first;
    if (src) {
        memcpy(&_vram[d], src, first);
        if (rest) memcpy(_vram, src + first, rest);
    } else {
        memset(&_vram[d], 0, first);
        if (rest) memset(_vram, 0, rest);
    }
}

void video_vram_blit(const uint8_t *src, uint32_t size, uint16_t base,
                     uint16_t pitch, uint16_t cols, uint16_t rows)
{
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t off = r * cols * 4u;
        uint32_t n = (off < size) ? size - off : 0u;
        uint16_t addr = (uint16_t)(base + r * pitch);

        if (n > cols * 4u) n = cols * 4u;
        n &= ~3u; // whole 4-pixel groups, the old per-group test's condition
        if (n) _vram_row(addr, src + off, n);
        if (n < cols * 4u)
            _vram_row((uint16_t)(addr + (uint16_t)(n / 4u)), NULL,
                      cols * 4u - n);
    }
}

void video_vram_wm2(uint16_t addr, uint8_t colour, uint8_t mask)
{
    uint8_t *px = &_vram[(uint32_t)addr * 4u];
    uint8_t keep = (uint8_t)~mask;

    for (uint16_t p = 0; p < 4u; p++) {
        uint8_t set = ((colour >> p) & 1u) ? mask : 0u;

        px[p] = (uint8_t)((px[p] & keep) | set);
    }
}

void video_vram_present(uint8_t *page, uint16_t base, uint16_t pitch,
                        uint16_t scroll, uint16_t split)
{
    if (!page) return;
    for (uint16_t y = 0; y < VIDEO_PAGE_H_MODE13H; y++) {
        uint8_t *d = page + y * VIDEO_PAGE_W;
        uint16_t row, pan;
        uint32_t src, first;

        if (y < split) { // the panned page
            row = (uint16_t)(base + y * pitch);
            pan = scroll;
        } else { // the fixed band from absolute address 0
            row = (uint16_t)((y - split) * pitch);
            pan = 0u;
        }
        /* the window is 4*row + pan contiguous pixels, wrapping at 64K */
        src = ((uint32_t)row * 4u + pan) & ((uint32_t)sizeof _vram - 1u);
        first = (uint32_t)sizeof _vram - src;
        if (first > VIDEO_PAGE_W) first = VIDEO_PAGE_W;
        memcpy(d, &_vram[src], first);
        if (first < VIDEO_PAGE_W)
            memcpy(d + first, _vram, VIDEO_PAGE_W - first);
    }
}

uint8_t *video_flat(void) { return g_osys->videoPage(1); }

uint8_t *video_arena(void)
{
    if (!_arena) {
        _arena = g_osys->memAlloc(ARENA_BYTES);
        if (!_arena && g_osys)
            g_osys->fatalError("text arena allocation failed");
    }
    return _arena;
}

void video_arena_release(void)
{
    g_osys->memFree(_arena);
    _arena = NULL;
}

void video_set_mode(VideoMode mode) { g_osys->videoSetMode(mode); }
