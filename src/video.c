#include "video.h"
#include "mem.h"
#include "osystem.h"

#include <stdint.h>
#include <string.h>

static const OSystem *_video_osys;

/* shortcut: the A000 plane model is always recomposed in software; a backend
 * with native planar output would move it behind an optional module (D-04). */

/* the A000 planar space: one address byte = four pixels, one per plane */
static uint8_t _vram[65536][4];

/* the shared text arena: 200 rows x 80 bytes.  The original carves it from
 * the boot heap block ([0xC9D] = [0x48AC] + the attract's buffers, 16000 B),
 * so it is allocated with the session and released at exit. */
static uint8_t *_arena;

int32_t video_init(const OSystem *osys)
{
    _video_osys = osys;
    mem_bind(osys);
    _arena = mem_alloc(16000u);
    return _arena ? 0 : -1;
}

void video_shutdown(void)
{
    mem_free(_arena);
    _arena = NULL;
}

void video_tick(void) { /* the flip is the backend's videoPresent */ }

uint8_t *video_page(int32_t which) { return _video_osys->videoPage(which); }

uint8_t *video_vram_group(uint16_t addr) { return _vram[addr]; }

void video_vram_clear(void) { memset(_vram, 0, sizeof _vram); }

void video_vram_planarise(const uint8_t *src, uint32_t size, uint16_t base,
                          uint16_t pitch, uint16_t cols, uint16_t rows)
{
    for (uint32_t r = 0; r < rows; r++) {
        uint32_t a = base + r * pitch;
        const uint8_t *s = src + r * cols * 4u;

        for (uint32_t c = 0; c < cols; c++) {
            uint32_t off = r * cols * 4u + c * 4u;
            uint8_t *p = _vram[(a + c) & 0xFFFFu]; // Model a potential wrap

            if (off + 3u < size) {
                p[0] = s[c * 4u];
                p[1] = s[c * 4u + 1u];
                p[2] = s[c * 4u + 2u];
                p[3] = s[c * 4u + 3u];
            } else {
                p[0] = p[1] = p[2] = p[3] = 0u;
            }
        }
    }
}

void video_vram_wm2(uint16_t addr, uint8_t colour, uint8_t mask)
{
    uint8_t *px = _vram[addr];
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

        if (y < split) { // the panned page
            row = (uint16_t)(base + y * pitch);
            pan = scroll;
        } else { // the fixed band from absolute address 0
            row = (uint16_t)((y - split) * pitch);
            pan = 0u;
        }
        for (uint16_t x = 0; x < VIDEO_PAGE_W; x++) {
            uint16_t w = (uint16_t)(pan + x);

            d[x] = _vram[(uint16_t)(row + (w >> 2))][w & 3u];
        }
    }
}

uint8_t *video_flat(void) { return _video_osys->videoPage(1); }

uint8_t *video_arena(void) { return _arena; }

void video_set_mode(VideoMode mode) { _video_osys->videoSetMode(mode); }
