/* arch/sdl3/video.c — window/renderer + the two 8bpp pages.
 *
 * The pages are core-owned abstract 320x200x8 buffers; the present converts
 * through the palette LUT into an ARGB texture (the palette lives here so
 * the core keeps its 6-bit palette semantics untouched). */
#include "osystem.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static uint8_t       pages[2][320 * 200];
static uint32_t      lut[256];          /* the palette LUT, 6-bit expanded */

static void vid_setMode(void)
{
    if (!SDL_CreateWindowAndRenderer("HumansRE", 640, 400,
                                     SDL_WINDOW_RESIZABLE, &win, &ren))
        SDL_Log("window: %s", SDL_GetError());
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, 320, 200);
}

static uint8_t *vid_page(int which) { return pages[which & 1]; }

static void vid_present(int which)
{
    const uint8_t *p = pages[which & 1];
    uint32_t argb[320 * 200];
    for (int i = 0; i < 320 * 200; i++) argb[i] = lut[p[i]];
    SDL_UpdateTexture(tex, NULL, argb, 320 * 4);
    SDL_RenderTexture(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

/* 6-bit -> 8-bit: the original's values are the DAC's; the expand is << 2 | >> 4 */
static uint8_t expand6(uint8_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

void sdl3_videoSetPalette(const uint8_t *p6, unsigned first, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint8_t r = expand6(p6[i*3]), g = expand6(p6[i*3+1]), b = expand6(p6[i*3+2]);
        lut[first + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
}

void sdl3_videoInit(void) { vid_setMode(); }
uint8_t *sdl3_videoPage(int which) { return vid_page(which); }
void sdl3_videoPresent(int which) { vid_present(which); }
