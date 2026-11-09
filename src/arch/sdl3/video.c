/* arch/sdl3/video.c — window/renderer + the two 8bpp _pages.
 *
 * The original's video: two VGA modes, 320x200 (mode 13h) and 320x192
 * (reset_display_mode's mode X), 8bpp with a 6-bit DAC palette, displayed
 * 4:3 on a CRT.  The SDL3 equivalent here: an ARGB8888 texture carrying the
 * indexed page through the palette LUT per present, rendered into the
 * current mode's logical size (320x200 or 320x192) stretched over the same
 * 960x720 window, so both fields keep the 4:3 display geometry.
 * The loop is paced by the tick gate (timer.c's tickSync), not by vsync. */
#include "osystem.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

static VideoMode _vid_mode = (VideoMode)-1;   /* the core's last mode set */

static SDL_Window *_win;
static SDL_Renderer *_ren;
static SDL_Texture *_tex;
static int32_t _tex_rows = VIDEO_PAGE_H_MODE13H; /* the texture's height = the field */
static uint8_t _pages[2][VIDEO_PAGE_BYTES];
static uint32_t _lut[256]; /* the palette LUT, 6-bit expanded */
static int32_t _present_probe; /* HUMANSRE_RATE_PROBE: report the
                                         * loop/present rate every 5 s    */

/** @brief The field each mode scans out (osystem.h's VideoMode): mode 13h is the
 * full 320x200 page, mode X shows only the top 192 rows — the rest of a
 * mode-X page is stale (the level's status bar ends on row 191).
 *
 * @return VIDEO_PAGE_H_MODEX for mode X, VIDEO_PAGE_H_MODE13H otherwise
 */
static int32_t _mode_rows(void)
{ return _vid_mode == VIDEO_MODE_X ? VIDEO_PAGE_H_MODEX : VIDEO_PAGE_H_MODE13H; }

/**
 * @brief 6-bit -> 8-bit: the original's values are the DAC's; the expand is << 2 | >> 4
 *
 * @param v the 6-bit colour
 * @return the expanded 8-bit colour
 */
static uint8_t _expand6(uint8_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

/** @brief The texture is exactly the field's height: a taller texture with a
 * sub-rect would let the linear filter sample the hidden rows (visible as
 * a one-row bleed of stale pixels at the bottom edge in mode X).
 *
 * @param rows the row count
 */
static void _vid_make_texture(int32_t rows)
{
    if (_tex) SDL_DestroyTexture(_tex);
    _tex = SDL_CreateTexture(_ren, SDL_PIXELFORMAT_ARGB8888,
                             SDL_TEXTUREACCESS_STREAMING, VIDEO_PAGE_W, rows);
    _tex_rows = rows;
}

/**
 * @brief window + texture, created on the boot's first videoSetMode call
 */
static void _vid_init_video(void)
{
    if (!SDL_CreateWindowAndRenderer(HUMANSRE_PROJECT_TITLE " " HUMANSRE_VERSION, 960, 720,
                                     SDL_WINDOW_RESIZABLE, &_win, &_ren))
        SDL_Log("window: %s", SDL_GetError());
    /* No renderer vsync: the loop is paced by the tick gate (game_main's
     * tickSync, the [0x5AC8] equivalent).  The original's foreground never
     * blocks on the display either — the CRTC flip is latched at vsync by
     * the ISR — so a 60 Hz host display must not drag the 70 Hz flow. */
    _vid_make_texture(VIDEO_PAGE_H_MODE13H);
    for (uint32_t i = 0; i < 256u; i++)
        _lut[i] = 0xFF000000u; /* opaque black */
    _present_probe = getenv("HUMANSRE_RATE_PROBE") != NULL;
}

void sdl3_video_set_mode(VideoMode mode)
{
    if (!_win) _vid_init_video();
    if (mode == _vid_mode) return;
    _vid_mode = mode;
    if (_mode_rows() != _tex_rows) _vid_make_texture(_mode_rows());
    SDL_SetRenderScale(_ren, 960.0f / (float)VIDEO_PAGE_W,
                       720.0f / (float)_mode_rows());
    SDL_Log("video: mode switched to %s (%dx%d)",
            mode == VIDEO_MODE_X ? "VGA mode X" : "VGA mode 13h",
            (int32_t)VIDEO_PAGE_W, _mode_rows());
}

/**
 * @brief Accessor: one of the two display pages.
 *
 * @param which the page selector
 * @return One of the two display pages.
 */
static uint8_t *_vid_page(int32_t which) { return _pages[which & 1]; }

/**
 * @brief Present one page and apply its 6-bit palette.
 *
 * @param which the page selector
 */
static void _vid_present(int32_t which)
{
    const uint8_t *p = _pages[which & 1];
    static uint32_t argb[VIDEO_PAGE_BYTES];
    const int32_t rows = _mode_rows();
    for (int32_t i = 0; i < (int32_t)VIDEO_PAGE_W * rows; i++)
        argb[i] = _lut[p[i]];
    SDL_UpdateTexture(_tex, NULL, argb, (int)(VIDEO_PAGE_W * 4u));
    SDL_SetRenderDrawColor(_ren, 0, 0, 0, 255);
    SDL_RenderClear(_ren);
    const SDL_FRect dst = { 0, 0, (float)VIDEO_PAGE_W, (float)rows };
    SDL_RenderTexture(_ren, _tex, NULL, &dst);
    SDL_RenderPresent(_ren);
    if (_present_probe) {
        static uint32_t frames;
        static Uint64 windowStart;
        frames++;
        Uint64 now = SDL_GetPerformanceCounter();
        if (!windowStart) {
            windowStart = now;
            return;
        }
        double s =
            (double)(now - windowStart) / (double)SDL_GetPerformanceFrequency();
        if (s >= 5.0) {
            /* note: scene playback presents from inside its own loops too,
             * so this can exceed the tick rate; it is not the loop rate */
            SDL_Log("present rate: %.2f Hz (videoPresent calls)",
                    (double)frames / s);
            frames = 0;
            windowStart = now;
        }
    }
}

void sdl3_video_set_palette(const uint8_t *p6, uint32_t first, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t r = _expand6(p6[i * 3]), g = _expand6(p6[i * 3 + 1]),
                b = _expand6(p6[i * 3 + 2]);
        _lut[first + i] =
            0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
}

uint8_t *sdl3_video_page(int32_t which) { return _vid_page(which); }
void sdl3_video_present(int32_t which) { _vid_present(which); }
