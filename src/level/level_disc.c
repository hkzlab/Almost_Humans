#include "audio.h"
#include "font.h"
#include "game_resources.h"
#include "game_strings.h"
#include "level/level.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "textpage.h"
#include "video.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- draw_status_indicator (1000:1FEF) ---------------------------------
 *
 * The disc-presence screen.  It is NOT a normal level screen: at the head it
 * opens the volume file for [0xF7E] (1000:20CF); if the file opens, it
 * returns at once.  Only a failed open draws the "PUT THE DISK INTO DRIVE n /
 * PRESS FIRE WHEN READY" page, fades its palette in (0x100 div 0x1E),
 * animates, retries the open on a key/ACTION, and on success fades to
 * [0x48BC] (black), holds 0x4B frames and returns.
 *
 * The art: APL\DISKIMG1.DAT is consumed by blit_sprite_to_page
 * (1000:2105): 4 planes, source stride 4, 0x1E40 = 7744 bytes per plane,
 * written to the page at [0x5AC4] (= 0 here).  The port converts those
 * planes to its chunky page with the same bit order.  The prompt's text is
 * drawn with the game's font (APL\TEXT.SPR ships on every disc); the
 * original draws the same two lines through the text renderer.
 */
enum {
    DISK_OFF,
    DISK_MUSIC, // 1000:204B's timing_loop holds the page
    DISK_FADE_IN,
    DISK_WAIT,
    DISK_FADE_OUT,
    DISK_HOLD
};

static uint8_t _disk_phase = DISK_OFF;
static uint8_t _disk_disc;
static uint16_t _disk_hold;
static uint8_t _disk_pal[768];
static const uint8_t _disk_black[768];
static uint8_t *_disk_font;

/**
 * @brief Draw the insert-disc prompt for one disc.
 *
 * @param disc the disc number
 */
static void _disk_draw(uint16_t disc)
{
    uint8_t *page = video_page(0);
    memset(page, 0, VIDEO_PAGE_BYTES);

    uint8_t *img = NULL;
    uint32_t isz = 0;
    uint16_t used = disc;
    for (uint16_t k = 0; k < 3u && !img; k++) {
        used = (k == 0u) ? disc : (k == 1u ? 0u : (disc == 1u ? 2u : 1u));
        res_set_disc(used);
        img = res_load_chunk(game_resources[GAME_RES_APL_DISKIMG1_DAT], &isz,
                             NULL);
    }
    if (img) {
        if (isz >= 0x18u + 768u) memcpy(_disk_pal, img + 0x18, 768u);
        /* blit_sprite_to_page, 1000:2105: source byte 4g+p -> plane p of
         * destination byte g; each byte is 8 pixels, MSB left */
        for (uint32_t g = 0; 4u * g + 3u < isz && g * 8u < VIDEO_PAGE_BYTES;
             g++) {
            const uint8_t *q = img + 4u * g;
            for (uint16_t bit = 0; bit < 8u; bit++) {
                uint32_t px = ((q[0] >> (7u - bit)) & 1u) |
                              (((q[1] >> (7u - bit)) & 1u) << 1) |
                              (((q[2] >> (7u - bit)) & 1u) << 2) |
                              (((q[3] >> (7u - bit)) & 1u) << 3);
                page[g * 8u + bit] = (uint8_t)px;
            }
        }
        g_osys->memFree(img);
    }

    g_osys->memFree(_disk_font);
    _disk_font =
        res_load_chunk(game_resources[GAME_RES_APL_TEXT_SPR], NULL, NULL);
    font_publish(_disk_font);

    char line[32];
    textpage_draw_line(
        page, game_strings[g_game_language][GAME_STRING_LEVEL_PUT_DISK], 0x50u,
        0x0Fu);
    snprintf(line, sizeof line,
             game_strings[g_game_language][GAME_STRING_LEVEL_INTO_DRIVE],
             (char)('1' + disc));
    textpage_draw_line(page, line, 0x60u, 0x0Fu);
    textpage_draw_line(
        page, game_strings[g_game_language][GAME_STRING_LEVEL_PRESS_FIRE],
        0x88u, 0x0Fu);
    textpage_draw_line(
        page, game_strings[g_game_language][GAME_STRING_LEVEL_WHEN_READY],
        0x98u, 0x0Fu);
}

uint8_t level_disk_status_begin(uint16_t disc)
{
    if (_disk_phase != DISK_OFF) return 1;
    if (res_volume_present(disc)) return 0; // 1000:2005: the disc is in

    /* draw_status_indicator (1000:1fef) sets mode 13h on the missing-disc
     * page (its flat 320x200 image) */
    video_set_mode(VIDEO_MODE_13H);
    _disk_disc = (uint8_t)(disc);
    memset(_disk_pal, 0, sizeof _disk_pal);
    /* 1000:2042-204b: play_tune("mus\disk") / music_resume / timing_loop,
     * which blocks before the page is drawn (the page blits at 0x204E). */
    audio_play_tune(audio_tune_disk);
    audio_music_resume();
    audio_fade_in();
    _disk_phase = DISK_MUSIC;
    return 1;
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
void level_disk_status_reset(void)
{
    _disk_phase = DISK_OFF;
    _disk_hold = 0;
    g_osys->memFree(_disk_font);
    _disk_font = NULL;
    font_publish(NULL);
}
#endif /* HUMANSRE_TEST_PROBES */

uint8_t level_disk_status_tick(const InputState *in)
{
    switch (_disk_phase) {
    case DISK_MUSIC: // the timing_loop at 1000:204b
        if (audio_fade_busy()) return 1;
        _disk_draw(_disk_disc);
        palette_build_fade(0u, 0x100u, 0x1Eu, _disk_pal); // 1000:2057
        _disk_phase = DISK_FADE_IN;
        return 1;
    case DISK_FADE_IN:
        if (palette_fade_active()) return 1;
        _disk_phase = DISK_WAIT;
        return 1;
    case DISK_WAIT:
        if (in->lastKey || (in->joyBits & 0x80u)) {
            if (res_volume_present(_disk_disc)) {
                palette_build_fade(0u, 0x100u, 0x1Eu, _disk_black);
                _disk_phase = DISK_FADE_OUT;
            }
            /* else: still the wrong disc - stay on the prompt */
        }
        return 1;
    case DISK_FADE_OUT:
        if (palette_fade_active()) return 1;
        _disk_phase = DISK_HOLD;
        _disk_hold = 0x4Bu; // present_frames_75
        audio_fade_out(); // 1000:20BE
        return 1;
    case DISK_HOLD:
        if (_disk_hold > 0u) {
            _disk_hold--;
            return 1;
        }
        if (audio_fade_busy()) return 1; // 1000:20BE's blocking fade
        _disk_phase = DISK_OFF;
        g_osys->memFree(_disk_font);
        _disk_font = NULL;
        font_publish(NULL);
        return 0;
    default: return 0;
    }
}
