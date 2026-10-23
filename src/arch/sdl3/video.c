/* arch/sdl3/video.c — window/renderer + the two 8bpp pages.
 *
 * The original's video: VGA mode X, 320x200x8bpp with a 6-bit DAC palette,
 * displayed 4:3 on a CRT (the pixels 1.2x taller than wide).  The SDL3
 * equivalent here: an ARGB8888 texture (320x200) carrying the indexed page
 * through the palette LUT per present, rendered into a 320x240 logical
 * size stretched over the window — the 1.2x vertical stretch preserved.
 * The loop is paced by the tick gate (timer.c's tickSync), not by vsync. */
#include "osystem.h"
#include <SDL3/SDL.h>
#include <stdlib.h>

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static uint8_t       pages[2][320 * 200];
static uint32_t      lut[256];          /* the palette LUT, 6-bit expanded */
static int           presentProbe;      /* HUMANSRE_RATE_PROBE: report the
                                         * loop/present rate every 5 s    */

/* 6-bit -> 8-bit: the original's values are the DAC's; the expand is << 2 | >> 4 */
static uint8_t expand6(uint8_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

static void vid_setMode(void)
{
    if (!SDL_CreateWindowAndRenderer("Humans RE", 960, 720,
                                     SDL_WINDOW_RESIZABLE, &win, &ren))
        SDL_Log("window: %s", SDL_GetError());
    /* the 320x200 pixels on a 4:3 display: the pixels 1.2x tall — the
     * window 960x720 = 3x wide, 3.6x tall, the draw space 320x200 */
    SDL_SetRenderScale(ren, 960.0f / 320.0f, 720.0f / 200.0f);
    /* No renderer vsync: the loop is paced by the tick gate (game_main's
     * tickSync, the [0x5AC8] equivalent).  The original's foreground never
     * blocks on the display either — the CRTC flip is latched at vsync by
     * the ISR — so a 60 Hz host display must not drag the 70 Hz flow. */
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, 320, 200);
    for (unsigned i = 0; i < 256u; i++) lut[i] = 0xFF000000u;   /* opaque black */
}

static uint8_t *vid_page(int which) { return pages[which & 1]; }

static void vid_present(int which)
{
    const uint8_t *p = pages[which & 1];
    static uint32_t argb[320 * 200];
    for (int i = 0; i < 320 * 200; i++) argb[i] = lut[p[i]];
    SDL_UpdateTexture(tex, NULL, argb, 320 * 4);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    const SDL_FRect dst = { 0, 0, 320.0f, 200.0f };
    SDL_RenderTexture(ren, tex, NULL, &dst);
    SDL_RenderPresent(ren);
    if (presentProbe) {
        static unsigned frames;
        static Uint64 windowStart;
        frames++;
        Uint64 now = SDL_GetPerformanceCounter();
        if (!windowStart) { windowStart = now; return; }
        double s = (double)(now - windowStart) / (double)SDL_GetPerformanceFrequency();
        if (s >= 5.0) {
            /* note: scene playback presents from inside its own loops too,
             * so this can exceed the tick rate; it is not the loop rate */
            SDL_Log("present rate: %.2f Hz (videoPresent calls)", (double)frames / s);
            frames = 0;
            windowStart = now;
        }
    }
}

void sdl3_videoSetPalette(const uint8_t *p6, unsigned first, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint8_t r = expand6(p6[i*3]), g = expand6(p6[i*3+1]), b = expand6(p6[i*3+2]);
        lut[first + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
}

void sdl3_videoInit(void)
{
    vid_setMode();
    presentProbe = getenv("HUMANSRE_RATE_PROBE") != NULL;
}
uint8_t *sdl3_videoPage(int which) { return vid_page(which); }
void sdl3_videoPresent(int which) { vid_present(which); }
