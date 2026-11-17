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

#if HUMANSRE_VIEW_SCALE > 1
/* The view scale: 1 = the original's 320x200 framing, 2 = the doubled level
 * viewport.  Read once from the backend in video_init, clamped to the
 * compiled maximum. */
static int32_t _scale = 1;
/* The logical 320x200 screen, used at scale 2 (at scale 1 video_screen()
 * hands out page 0 itself, so nothing is copied and the buffer is not
 * compiled in). */
static uint8_t _screen[VIDEO_SCREEN_BYTES];
/* The surface of the frame being drawn: 1 = the logical screen, so
 * video_present_frame must expand it. */
static uint8_t _screen_active;
#endif
/* The current video mode and the field's scan-out rows: the mode's default,
 * or the current screen's own (the level's whole page at scale 2). */
static VideoMode _mode = VIDEO_MODE_13H;
static uint32_t _field_rows;

/** @brief The mode's own field height, doubled by the view factor. */
static uint32_t _field_default(void)
{
    return (uint32_t)(_mode == VIDEO_MODE_X ? VIDEO_FIELD_H_MODEX
                                            : VIDEO_SCREEN_H) *
           (uint32_t)video_scale();
}

int32_t video_init(void)
{
#if HUMANSRE_VIEW_SCALE > 1
    _scale = g_osys->videoScale() == 2 ? 2 : 1;
    _screen_active = 0;
#else
    (void)g_osys->videoScale(); // the classic build always frames 320x200
#endif
    _mode = VIDEO_MODE_13H; // until the first mode set
    _field_rows = _field_default();
    return 0;
}

void video_tick(void) { /* the flip is the backend's videoPresent */ }

int32_t video_scale(void)
{
#if HUMANSRE_VIEW_SCALE > 1
    return _scale;
#else
    return 1;
#endif
}

uint32_t video_page_w(void) { return VIDEO_SCREEN_W * (uint32_t)video_scale(); }

uint32_t video_page_h(void) { return VIDEO_SCREEN_H * (uint32_t)video_scale(); }

uint8_t *video_page(int32_t which)
{
#if HUMANSRE_VIEW_SCALE > 1
    if (which == 0) _screen_active = 0;
#endif
    return g_osys->videoPage(which);
}

uint8_t *video_screen(void)
{
#if HUMANSRE_VIEW_SCALE > 1
    if (_scale == 1) return g_osys->videoPage(0);
    _screen_active = 1;
    return _screen;
#else
    /* the classic build has one surface: the screen *is* page 0 */
    return g_osys->videoPage(0);
#endif
}

#if HUMANSRE_VIEW_SCALE > 1
/** @brief Expand the logical screen into page 0 by the view scale. */
static void _screen_expand(void)
{
    uint8_t *page = g_osys->videoPage(0);
    uint32_t w = video_page_w();
    uint32_t s = (uint32_t)_scale;

    if (!page) return;
    for (uint32_t y = 0; y < VIDEO_SCREEN_H; y++) {
        const uint8_t *src = _screen + y * VIDEO_SCREEN_W;
        uint8_t *dst = page + y * s * w;

        for (uint32_t x = 0; x < VIDEO_SCREEN_W; x++) {
            uint8_t v = src[x];

            dst[2u * x] = v;
            dst[2u * x + 1u] = v;
            dst[w + 2u * x] = v;
            dst[w + 2u * x + 1u] = v;
        }
    }
}
#endif

void video_present_frame(void)
{
#if HUMANSRE_VIEW_SCALE > 1
    if (_scale > 1 && _screen_active) _screen_expand();
    _screen_active = 0;
#endif
    g_osys->videoPresent(0, _field_rows);
}

void video_set_field(uint32_t rows)
{
#if HUMANSRE_VIEW_SCALE > 1
    _field_rows = rows ? rows : _field_default();
#else
    (void)rows; // the classic field is always the mode's
#endif
}

uint32_t video_field_rows(void) { return _field_rows; }

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
    for (uint16_t y = 0; y < VIDEO_SCREEN_H; y++) {
        uint8_t *d = page + y * VIDEO_SCREEN_W;
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
        if (first > VIDEO_SCREEN_W) first = VIDEO_SCREEN_W;
        memcpy(d, &_vram[src], first);
        if (first < VIDEO_SCREEN_W)
            memcpy(d + first, _vram, VIDEO_SCREEN_W - first);
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

void video_set_mode(VideoMode mode)
{
    /* The original's field: 192 rows in mode X, 200 in mode 13h, doubled by
     * the view factor.  A screen with a different field (the level) calls
     * video_set_field after its mode set. */
    _field_rows =
        (mode == VIDEO_MODE_X ? VIDEO_FIELD_H_MODEX : VIDEO_SCREEN_H) *
        (uint32_t)video_scale();
    g_osys->videoSetMode(mode);
}
