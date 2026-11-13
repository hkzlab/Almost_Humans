/* The SDL3 video backend: window, renderer, texture and the two 8bpp pages.
 *
 * Indexed 320x200 (mode 13h) or 320x192 (mode X) through a palette LUT into
 * a 4:3 picture that is letterboxed and centred in a resizable window; the
 * loop is paced by the tick gate, not vsync.
 * Port-only: it replaces the VGA/CRTC services the original programs directly.
 * Deviation: D-04 (flat pages; the mode-X model is core-side).
 */
#include "osystem.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#if defined(HUMANSRE_TEST_PROBES)
#    include <stdlib.h> /* getenv: the present-rate probe */
#endif

static VideoMode _vid_mode = (VideoMode)-1;   // the core's last mode set

static SDL_Window *_win;
static SDL_Renderer *_ren;
static SDL_Texture *_tex;
static int32_t _tex_rows =
    VIDEO_PAGE_H_MODE13H; // the texture's height = the field
static uint8_t _pages[2][VIDEO_PAGE_BYTES];
static uint32_t _lut[256]; // the palette LUT, 6-bit expanded

#define WINDOW_DEFAULT_WIDTH 920
#define WINDOW_DEFAULT_HEIGHT 720

/* The adjusted picture is 320x240: the 4:3 box the field is stretched into.
 * VGA 320x200 and 320x192 pixels are ~1.2 tall
 */
#define VID_ADJUSTED_H 240
#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): HUMANSRE_RATE_PROBE also reports the
 * present rate.  Never part of a release build. */
static int32_t _present_probe;
#endif

/** @brief The field's row count: 192 in mode X, 200 in mode 13h.
 *
 * The rows past a mode-X field are stale (the level's status bar ends on
 * row 191), so nothing below them may be presented.
 *
 * @return VIDEO_PAGE_H_MODEX for mode X, VIDEO_PAGE_H_MODE13H otherwise
 */
static int32_t _mode_rows(void)
{
    return _vid_mode == VIDEO_MODE_X ? VIDEO_PAGE_H_MODEX
                                     : VIDEO_PAGE_H_MODE13H;
}

/**
 * @brief Expand a 6-bit DAC value to 8 bits (v * 4 + v / 16).
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
    /* The default (linear) blurs every pixel of a 320-wide field upscaled 3x
     * or more; PIXELART keeps the source pixels crisp. */
#if SDL_VERSION_ATLEAST(3, 4, 0)
    SDL_SetTextureScaleMode(_tex, SDL_SCALEMODE_PIXELART);
#else
    SDL_SetTextureScaleMode(_tex, SDL_SCALEMODE_LINEAR);
#endif
    _tex_rows = rows;
}

/**
 * @brief Create the window, renderer and texture on the first mode set.
 */
static void _vid_init_video(void)
{
    if (!SDL_CreateWindowAndRenderer(
            HUMANSRE_PROJECT_TITLE " " HUMANSRE_VERSION, WINDOW_DEFAULT_WIDTH, WINDOW_DEFAULT_HEIGHT,
            SDL_WINDOW_RESIZABLE, &_win, &_ren))
        SDL_Log("window: %s", SDL_GetError());

    /* No renderer vsync: the loop is paced by the tick gate (game_main's
     * tickSync, the [0x5AC8] equivalent).  The original's foreground never
     * blocks on the display either - the CRTC flip is latched at vsync by
     * the ISR - so a 60 Hz host display must not drag the 70 Hz flow. */

    _vid_make_texture(VIDEO_PAGE_H_MODE13H); // We'll start at mode 13H
    for (uint32_t i = 0; i < 256u; i++) _lut[i] = 0xFF000000u; // opaque black
#if defined(HUMANSRE_TEST_PROBES)
    _present_probe = getenv("HUMANSRE_RATE_PROBE") != NULL;
#endif
}

void sdl3_video_set_mode(VideoMode mode)
{
    if (!_win) _vid_init_video();
    if (mode == _vid_mode) return;
    _vid_mode = mode;
    if (_mode_rows() != _tex_rows) _vid_make_texture(_mode_rows());
    SDL_Log("video: mode switched to %s (%dx%d)",
            mode == VIDEO_MODE_X ? "VGA mode X" : "VGA mode 13h",
            (int32_t)VIDEO_PAGE_W, _mode_rows());
}

/**
 * @brief The i-th of the two display pages.
 *
 * @param which the page selector
 * @return the page, masked to 0 or 1
 */
static uint8_t *_vid_page(int32_t which) { return _pages[which & 1]; }

/**
 * @brief Present one page through the palette LUT.
 *
 * @param which the page selector
 */
static void _vid_present(int32_t which)
{
#if defined(HUMANSRE_TEST_PROBES)
    const Uint64 started = SDL_GetTicksNS();
#endif
    int outW = 0, outH = 0;
    SDL_GetRenderOutputSize(_ren, &outW, &outH);

    const uint8_t *p = _pages[which & 1];
    const int32_t rows = _mode_rows();
    static uint32_t argb[VIDEO_PAGE_BYTES];
    
    for (int32_t i = 0; i < (int32_t)VIDEO_PAGE_W * rows; i++)
        argb[i] = _lut[p[i]];

    SDL_UpdateTexture(_tex, NULL, argb, (int)(VIDEO_PAGE_W * 4u));
    SDL_SetRenderDrawColor(_ren, 0, 0, 0, 255);
    SDL_RenderClear(_ren);

    /* Fit the 4:3 picture into whatever window size the user picked: as large
     * as fits, centred, black bars on the free axis.  Scaling the destination
     * rect (not drawing at native size) keeps the source pixel grid intact for
     * PIXELART sampling; the draw is in pixels, so no render scale is set. */
    float scale = (float)outW / (float)VIDEO_PAGE_W;
    if (scale * (float)VID_ADJUSTED_H > (float)outH)
        scale = (float)outH / (float)VID_ADJUSTED_H;
    const float w = (float)VIDEO_PAGE_W * scale;
    const float h = (float)VID_ADJUSTED_H * scale;
    const SDL_FRect dst = { ((float)outW - w) / 2.0f, ((float)outH - h) / 2.0f,
                            w, h };
    SDL_RenderTexture(_ren, _tex, NULL, &dst);
    SDL_RenderPresent(_ren);
#if defined(HUMANSRE_TEST_PROBES)
    if (_present_probe) {
        static uint32_t frames;
        static Uint64 windowStart, total, worst;
        const Uint64 done = SDL_GetTicksNS();
        frames++;
        total += done - started;
        if (done - started > worst) worst = done - started;
        if (!windowStart) {
            windowStart = done;
            return;
        }
        double s = (double)(done - windowStart) / 1e9;
        if (s >= 5.0) {
            /* Scene playback presents from its own loops too, so this can
             * exceed the tick rate: it is not the loop rate. */
            SDL_Log("present rate: %.2f Hz (videoPresent calls), mean %.2f ms, "
                    "worst %.2f ms, output %dx%d",
                    (double)frames / s, (double)total / (double)frames / 1e6,
                    (double)worst / 1e6, outW, outH);
            frames = 0;
            total = 0;
            worst = 0;
            windowStart = done;
        }
    }
#endif /* HUMANSRE_TEST_PROBES */
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
