/* arch/sdl3/video.c — window/renderer + the two 8bpp pages.
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

static VideoMode vidMode = (VideoMode)-1;   /* the core's last mode set */

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int32_t texRows = 200;      /* the texture's height = the field */
static uint8_t pages[2][320 * 200];
static uint32_t lut[256];          /* the palette LUT, 6-bit expanded */
static int32_t presentProbe;      /* HUMANSRE_RATE_PROBE: report the
                                         * loop/present rate every 5 s    */

/* The field each mode scans out (osystem.h's VideoMode): mode 13h is the
 * full 320x200 page, mode X shows only the top 192 rows — the rest of a
 * mode-X page is stale (the level's status bar ends on row 191). */
static int32_t modeRows(void) { return vidMode == VIDEO_MODE_X ? 192 : 200; }

/* 6-bit -> 8-bit: the original's values are the DAC's; the expand is << 2 | >> 4 */
static uint8_t expand6(uint8_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

/* The texture is exactly the field's height: a taller texture with a
 * sub-rect would let the linear filter sample the hidden rows (visible as
 * a one-row bleed of stale pixels at the bottom edge in mode X). */
static void vid_makeTexture(int32_t rows)
{
    if (tex) SDL_DestroyTexture(tex);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, 320, rows);
    texRows = rows;
}

/* window + texture, created on the boot's first videoSetMode call */
static void vid_initVideo(void)
{
    if (!SDL_CreateWindowAndRenderer("Humans RE", 960, 720,
                                     SDL_WINDOW_RESIZABLE, &win, &ren))
        SDL_Log("window: %s", SDL_GetError());
    /* No renderer vsync: the loop is paced by the tick gate (game_main's
     * tickSync, the [0x5AC8] equivalent).  The original's foreground never
     * blocks on the display either — the CRTC flip is latched at vsync by
     * the ISR — so a 60 Hz host display must not drag the 70 Hz flow. */
    vid_makeTexture(200);
    for (uint32_t i = 0; i < 256u; i++) lut[i] = 0xFF000000u; /* opaque black */
    presentProbe = getenv("HUMANSRE_RATE_PROBE") != NULL;
}

/* The mode set: the 320x200 board fills 4:3 as 3x/3.6x; the 320x192 board
 * fills the same window as 3x/3.75x, so the pixels keep the 4:3 display
 * geometry and the rows past the field are never presented. */
void sdl3_videoSetMode(VideoMode mode)
{
    if (!win) vid_initVideo();
    if (mode == vidMode) return;
    vidMode = mode;
    if (modeRows() != texRows) vid_makeTexture(modeRows());
    SDL_SetRenderScale(ren, 960.0f / 320.0f, 720.0f / (float)modeRows());
    SDL_Log("video: mode switched to %s (%dx%d)",
            mode == VIDEO_MODE_X ? "VGA mode X" : "VGA mode 13h", 320,
            modeRows());
}

static uint8_t *vid_page(int32_t which) { return pages[which & 1]; }

static void vid_present(int32_t which)
{
    const uint8_t *p = pages[which & 1];
    static uint32_t argb[320 * 200];
    const int32_t rows = modeRows();
    for (int32_t i = 0; i < 320 * rows; i++) argb[i] = lut[p[i]];
    SDL_UpdateTexture(tex, NULL, argb, 320 * 4);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    const SDL_FRect dst = { 0, 0, 320.0f, (float)rows };
    SDL_RenderTexture(ren, tex, NULL, &dst);
    SDL_RenderPresent(ren);
    if (presentProbe) {
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

void sdl3_videoSetPalette(const uint8_t *p6, uint32_t first, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t r = expand6(p6[i * 3]), g = expand6(p6[i * 3 + 1]),
                b = expand6(p6[i * 3 + 2]);
        lut[first + i] =
            0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
}

uint8_t *sdl3_videoPage(int32_t which) { return vid_page(which); }
void sdl3_videoPresent(int32_t which) { vid_present(which); }
