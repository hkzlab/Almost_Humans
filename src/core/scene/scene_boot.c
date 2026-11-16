#include "scene/scene.h"
#include "scene/scene_internal.h"

#include "audio.h"
#include "game_resources.h"
#include "osystem.h"
#include "palette.h"
#include "res.h"
#include "script.h"
#include "video.h"
#include "byteorder.h"

#include <stdint.h>
#include <string.h>

typedef enum {
    BOOT_START,
    BOOT_GT_PRE, // the boot fade-to-black wait; the list decode
    BOOT_GT_FADE0, // group 0 shown; the 4-entry fast fade + wait
    BOOT_GT_FRAME, // one group shown (b29b's LOOP)
    BOOT_GT_FLIP, // the flip-apply tick (87c7's ISR sync)
    BOOT_GT_BANK_HOLD, // the 30-tick hold before each bank cycle
    BOOT_GT_BANK_A, // bank fade -> gtpal+0x48 (BP=3) + wait
    BOOT_GT_BANK_B, // bank fade -> gtpal+0x3C (BP=0x1e) + wait
    BOOT_GT_LAST_F, // entries 0x20-0x2F -> gtpal+0x60 (BP=0x32)
    BOOT_GT_HOLD, // the 0x1a4-tick hold
    BOOT_GT_OUT, /* fade all -> 0x4BBC (zeroed: nothing loaded yet
                         * - to black), BP=0x32 (bf60)                   */
    BOOT_ABORT, /* c0c6 -> JMP b60a: a held key/fire unwinds the
                         * whole boot; the final fade (div 0x23) starts   */
    BOOT_ABORT_FADE, // its wait (8b8b - the non-aborting spin)
    BOOT_IDI_FADE, // idi.bit shown; fade to its own palette
    BOOT_IDI_HOLD, // the 0x1a4-tick hold
    BOOT_IDI_OUT, // fade to the zeroed ramp + wait
    BOOT_FIRST_BANK, // bank fade (16 entries at BP=0x23) + wait
    BOOT_FIRST_HOLD, // the 0x118-tick hold per bank
    BOOT_FIRST_LASTHOLD, // bd50: the extra 0x118-tick hold after bank 7
    BOOT_FIRST_OUT, // fade to the zeroed ramp (BP=0x23) + wait
    BOOT_ITB_SHOW, // the ITB card present + its fade-in start
    BOOT_CARD_FADE, // its wait
    BOOT_DWELL, // the 280-tick initial dwell
    BOOT_FADEOUT, // the post-dwell fade to the zeroed ramp (b6f2)
    BOOT_BLACK, // the 0x8C-tick black hold (c0b8)
    BOOT_FRAME, // cues + scroll + sprites + display-list blit
    BOOT_WAIT, // the frame wait (4 ticks in the port; D-248)
    BOOT_SCENE_HOLD, // the scene's final 0x8C-tick hold (b7a3)
    BOOT_TRANS, // the transition's white-flash fade (b854)
    BOOT_TRANS_HOLD, // the words redraw + 0xa-tick hold (b866-b871)
    BOOT_OUTRO, // the scene-tail fade to black (b7a9, BP=0x1e)
    BOOT_DONE
} BootPhase;

/* The boot's flag-style key tests (b2ba and c0c6) read the original's
 * level-triggered ISR flags ([0x48A3] / +0x5ACE bit-7) - not an event
 * queue.  They read the main loop's per-tick poll (scene_note_input):
 * extra eventsPoll calls here would consume input events the foreground
 * owns (the null backend's script is call-indexed). */
static InputState _scene_last_input;

static BootPhase _boot_phase = BOOT_START;
static CueScript _boot_script;
static uint8_t *_boot_bac; // intro\bac.bit - scroll source + palette
static uint8_t *_boot_treetop; // intro\treetop.bit 140x113
static uint8_t *_boot_treebot; // intro\treebot.bit 21x59
static uint8_t *_boot_apple; // intro\apple.spr 16x16
static uint8_t *_boot_words; // intro\words.bit - the bar's 1-bit text
static uint8_t _boot_bar[16u * VIDEO_PAGE_W]; // the bar, rows 182-197
static int8_t _boot_bar_arg = -1; // the transition the bar holds
static uint8_t *_boot_rsc; // intro\intro.rsc - the display list
static uint32_t _boot_rsc_size;
static uint32_t *_boot_entries;
static uint32_t _boot_entry_count;
static uint8_t *_boot_list_buf; // the decoded display-list object
static uint32_t _boot_list_size; // its decoded size
static uint8_t *_boot_obj; // the records' decoded overlay objects
static uint32_t _boot_list_start; // 0x6FEA: the records' start
static uint32_t _boot_list_pos; // [0x6468]: the current group
static uint16_t _boot_frame; // [0x6466]: the display-list group index
static uint16_t _boot_group_dwell; // [0x6460]: frames per group
static uint16_t _boot_dwell_count; // [0x6462]: the group dwell countdown
static uint16_t _boot_scroll_pos; // [0x6473]: the horizontal scroll
static uint16_t _boot_wait_ticks;
static uint8_t _boot_ramp[768]; // the fade-target scratch (202F:0x48BC)
static uint8_t _boot_trans_ticks; // the 0xa-tick words hold (b869)
static uint16_t _boot_bank_countdown; // [0x644E]: the b7c0 bank-fade timer
static uint8_t *_boot_gt; // intro\gametek.rsc - the logo display list
static uint32_t _boot_gt_size;
static uint32_t *_boot_gt_entries; // its u32 object table
static uint32_t _boot_gt_entry_count;
static uint8_t *_boot_gt_list; /* decoded object 0: [u16 count][768-byte  *
                                   * palette][u16 object index x count]      */
static uint16_t _boot_gt_group; // the group loop counter (b29b's CX)
static uint8_t _boot_gt_bank; // the 4-bank palette-cycle counter
static uint8_t *_boot_itb; // intro\itb.bit held until its stage
static uint16_t _boot_hold; // the stages' hold counters (c0b8's CX)
/**
 * @brief The boot intro's key gate: any key or fire.
 *
 * @return 1 when a key or fire is held, 0 otherwise
 */
static int32_t _boot_key_state(void)
{
    return _scene_last_input.keyHeld != 0u || _scene_last_input.lastKey != 0u ||
           (_scene_last_input.joyBits & JOYBITS_FIRE) != 0u;
}

void scene_note_input(const InputState *in) { _scene_last_input = *in; }

/** @brief Any key or fire from the live poll: the original's [0x48A3] / +0x5ACE
 * bit-7 checks.  The boot's waits use the cached _boot_key_state instead.
 *
 * @return 1 when a key or fire is held, 0 otherwise
 */
static int32_t _boot_gt_show(uint32_t idx)
{
    if (idx >= _boot_gt_entry_count) return 0;
    uint16_t chk = 0;
    int32_t sz =
        res_decode_lzw_mem(_boot_gt, _boot_gt_size, _boot_gt_entries[idx],
                           _boot_obj, OBJ_SEG_SIZE, &chk);
    if (sz < 0 || chk != 0) return 0; // 1000:B444: skip
    _scene_al_expand(_boot_obj, (uint32_t)sz, video_page(0), VIDEO_PAGE_BYTES);
    return 1;
}

/** @brief present_frame (1000:BDBC) for the idi/first cards: load, the file's own
 * palette into the working palette (8d24, RAM only), the image on the page.
 * The DAC moves when the caller's fade targets the working palette.
 *
 * @param name the name
 * @return 1 while the card shows, 0 when it finishes
 */
static int32_t _boot_card_show(const char *name)
{
    uint32_t size = 0;
    uint8_t *img = res_load_chunk(name, &size, NULL);
    if (!img || size < VIDEO_PAGE_BYTES) {
        g_osys->memFree(img);
        return 0;
    }
    memcpy(video_page(0), img, VIDEO_PAGE_BYTES);
    palette_load_working(img + VIDEO_PAGE_BYTES);
    g_osys->memFree(img);
    return 1;
}

/** @brief c0c6 (input_abort_check): every c0af/c0b8 wait polls the key each tick;
 * a held key or fire bit jumps b60a - the whole remaining boot (the cards,
 * the caveman scene) is skipped, the final fade runs, done.
 *
 * @return 1 when a key or fire aborts the wait, 0 otherwise
 */
static int32_t _boot_abort_poll(void)
{
    if (!_boot_key_state()) return 0;
    _boot_phase = BOOT_ABORT;
    return 1;
}

/**
 * @brief Release the boot intro's loaded assets.
 */
static void _boot_unload(void)
{
    g_osys->memFree(_boot_bac);
    _boot_bac = NULL;
    g_osys->memFree(_boot_treetop);
    _boot_treetop = NULL;
    g_osys->memFree(_boot_treebot);
    _boot_treebot = NULL;
    g_osys->memFree(_boot_apple);
    _boot_apple = NULL;
    g_osys->memFree(_boot_words);
    _boot_words = NULL;
    g_osys->memFree(_boot_rsc);
    _boot_rsc = NULL;
    _boot_rsc_size = 0;
    g_osys->memFree(_boot_entries);
    _boot_entries = NULL;
    _boot_entry_count = 0;
    g_osys->memFree(_boot_list_buf);
    _boot_list_buf = NULL;
    g_osys->memFree(_boot_obj);
    _boot_obj = NULL;
    g_osys->memFree(_boot_gt);
    _boot_gt = NULL;
    _boot_gt_size = 0;
    g_osys->memFree(_boot_gt_entries);
    _boot_gt_entries = NULL;
    _boot_gt_entry_count = 0;
    g_osys->memFree(_boot_gt_list);
    _boot_gt_list = NULL;
    g_osys->memFree(_boot_itb);
    _boot_itb = NULL;
}

/** @brief blit_sprite_transparent (1000:BAA5): a width x height sprite at page
 * (worldX - scroll, pageYOff); 0 bytes are transparent; the sprite clips
 * at the page's right edge and is off-screen once X wraps.
 *
 * @param spr the sprite index
 * @param width the width
 * @param height the height
 * @param worldX the world x
 * @param pageYOff the page y offset
 * @param page the page buffer
 */
static void _boot_blit_sprite(const uint8_t *spr, uint32_t width,
                              uint32_t height, uint32_t worldX,
                              uint32_t pageYOff, uint8_t *page)
{
    uint32_t x = (worldX - _boot_scroll_pos) & 0xFFFFu;
    if (x >= VIDEO_PAGE_W) return; // CMP AX,0x140 / JC
    uint32_t visible = VIDEO_PAGE_W - x;
    if (visible > width) visible = width;
    uint8_t *dst = page + pageYOff + x;
    const uint8_t *src = spr;
    for (uint16_t row = 0; row < height; row++) {
        for (uint16_t i = 0; i < visible; i++) {
            uint8_t b = src[i];
            if (b) dst[i] = b; // 0 = transparent
        }
        dst += VIDEO_PAGE_W; // the 320-byte row stride
        src += width; // the clipped part skipped
        if (dst + visible > page + PAGE_SIZE) return;
    }
}

/** @brief copy_rows_0x140 (1000:B9F8): the scroll render - the bac screen wrapped
 * horizontally by `scroll`: each of the 184 rows shows
 * bac[row][scroll..320) then bac[row][0..scroll), in 92 two-row pairs
 * (each pair: two DX-byte copies + two BP advances; DX + BP = 320).
 *
 * @param page the page buffer
 * @param bac the BAC image
 * @param bacSize the BAC image size
 * @param scroll the scroll amount
 * @param destOff the destination offset
 */
static void _boot_scroll_render(uint8_t *page, const uint8_t *bac,
                                uint32_t bacSize, uint32_t scroll,
                                uint32_t destOff)
{
    uint16_t ax = scroll % VIDEO_PAGE_W;
    uint32_t dx, bp, si, di = destOff;
    if (di != 0) {
        dx = VIDEO_PAGE_W - di;
        bp = di;
        si = 0;
    } else {
        dx = VIDEO_PAGE_W - ax;
        bp = ax;
        si = ax;
    }
    for (uint16_t i = 0; i < 0x5cu; i++) { // 92 two-row pairs
        for (uint16_t j = 0; j < dx; j++) {
            if (si >= bacSize || di >= PAGE_SIZE) return;
            page[di++] = bac[si++];
        }
        di += bp;
        si += bp;
        for (uint16_t j = 0; j < dx; j++) {
            if (si >= bacSize || di >= PAGE_SIZE) return;
            page[di++] = bac[si++];
        }
        di += bp;
        si += bp;
    }
}

/** @brief One display-list record: the stored index INCs (16-bit - a 0xFFFF index
 * wraps to object 0, the display-list object itself), the object LZW-
 * decodes to [u16 width][u16 height][bytes] and transparent-blits at
 * `dest` with the 320-byte row stride (1000:B955's inner blit).
 *
 * @param rsc the resource
 * @param rscSize the resource size
 * @param entries the object offset table
 * @param entryCount the table's entry count
 * @param _obj_buf the object buffer
 * @param storedIdx the stored index
 * @param dest the destination
 * @param listObj the list object
 * @param listObjSize the list object size
 * @param page the page buffer
 */
static void _boot_blit_record(const uint8_t *rsc, uint32_t rscSize,
                              const uint32_t *entries, uint32_t entryCount,
                              uint8_t *_obj_buf, uint32_t storedIdx,
                              uint16_t dest, const uint8_t *listObj,
                              uint32_t listObjSize, uint8_t *page)
{
    uint32_t idx = (storedIdx + 1u) & 0xFFFFu; // INC AX: 16-bit wrap
    const uint8_t *src;
    uint32_t srcSize;
    uint16_t chk = 0;
    int32_t sz;
    if (idx == 0) { // object 0: the list itself
        src = listObj;
        srcSize = listObjSize;
    } else {
        if (idx >= entryCount) return;
        sz = res_decode_lzw_mem(rsc, rscSize, entries[idx], _obj_buf,
                                OBJ_SEG_SIZE, &chk);
        if (sz < 0 || chk != 0) return; // 1000:B444: skip
        src = _obj_buf;
        srcSize = (uint32_t)sz;
    }
    uint16_t width = rd_le_u16(src);
    uint16_t height = rd_le_u16(src + 2);
    const uint8_t *body = src + 4;
    uint32_t bodySize = srcSize - 4u;
    uint32_t di = dest;
    for (uint16_t r = 0; r < height; r++) {
        for (uint16_t i = 0; i < width && di + i < PAGE_SIZE; i++) {
            uint32_t so = (uint32_t)r * width + i;
            uint8_t b = so < bodySize ? body[so] : 0u; // past the object: 0
            if (b) page[di + i] = b; // 0 = transparent
        }
        di += VIDEO_PAGE_W;
        if (di >= PAGE_SIZE) return;
    }
}

/** @brief blit_display_list (1000:B955): blit the current group's records; returns
 * the position after the group's 0xFFFF terminator.  The records are the
 * decoded object 0's OWN TAIL: nobody builds the list at runtime,
 * `MOV SI,[0x6468]` ([0x6468] = obj0 base+2) walks the scratch segment, so
 * the records read from the DECODED buffer and bounds against its size.
 * The objects themselves still LZW-decode out of the raw RSC's table.
 *
 * @param listBuf the list buffer
 * @param listSize the list size
 * @param rsc the resource
 * @param rscSize the resource size
 * @param entries the object offset table
 * @param entryCount the table's entry count
 * @param _obj_buf the object buffer
 * @param listPos the list position
 * @param page the page buffer
 * @return the source cursor after the group
 */
static uint32_t _boot_blit_group(const uint8_t *listBuf, uint32_t listSize,
                                 const uint8_t *rsc, uint32_t rscSize,
                                 const uint32_t *entries, uint32_t entryCount,
                                 uint8_t *_obj_buf, uint32_t listPos,
                                 uint8_t *page)
{
    uint32_t si = listPos;
    while (1) { // the group's records
        if (si + 4 > listSize) break;
        uint16_t dest = rd_le_u16(listBuf + si);
        uint16_t idx = rd_le_u16(listBuf + si + 2);
        si += 4;
        _boot_blit_record(rsc, rscSize, entries, entryCount, _obj_buf, idx,
                          dest, _boot_list_buf, _boot_list_size, page);
        if (si + 2 > listSize) break;
        if (rd_le_u16(listBuf + si) == 0xFFFFu) {
            si += 2;
            break;
        } // the group end
    }
    return si;
}

/** @brief The seek (cmd 4): walk `frame` 0xFFFF group terminators from the list
 * start to the group for `frame`.
 *
 * The terminators live in decoded object 0's tail ([0x6468] runs in the
 * scratch segment), so the walk bounds against the DECODED size, not the raw
 * RSC file.
 *
 * @param listBuf the list buffer
 * @param listSize the list size
 * @param listStart the list start
 * @param frame the frame index
 * @return the list position of the requested frame
 */
static uint32_t _boot_seek(const uint8_t *listBuf, uint32_t listSize,
                           uint32_t listStart, uint32_t frame)
{
    uint32_t si = listStart;
    for (uint16_t g = 0; g < frame && si + 2 <= listSize;) {
        if (rd_le_u16(listBuf + si) == 0xFFFFu) {
            si += 2;
            g++;
        } else si += 4; // a [dest][idx] record
    }
    return si;
}

/** @brief the bar text (run_screen_transition's builder, 1000:B89D -> B8A8): the
 * words' 1-bit block at [0x8689 + arg*0x258] - 16 rows x 40 bytes, each
 * bit = one pixel, MSB first, colour 0xE0+row (bac's grey gradient bank)
 * or black - written into the visible bar, rows 182-197, once per
 * transition; the per-frame present never touches those rows so the bar
 * persists until the next transition.
 *
 * @param arg the argument
 */
static void _boot_bar_draw(uint32_t arg)
{
    for (uint16_t r = 0; r < 16u; r++) {
        uint8_t *dst = _boot_bar + r * VIDEO_PAGE_W;
        const uint8_t *src = _boot_words + arg * 600u + r * 40u;
        for (uint16_t x = 0; x < VIDEO_PAGE_W; x++) {
            uint8_t b = src[x / 8u];
            dst[x] = ((b >> (7u - (x % 8u))) & 1u) ? (uint8_t)(0xE0u + r) : 0u;
        }
    }
}

/** @brief deplanarise_bitmap (1000:BB05) - the present.  The visible page (chain-4
 * mode 13h, pitch 80 bytes/row/plane) is filled from the pool's rows 4-183
 * (SI+0x500, 180 rows): visible row v = pool row v+4.  The visible rows
 * 180-199 are outside every present - the bar (run_screen_transition's
 * direct A000 writes, rows 182-197, colours 0xE0-0xEF = bac's grey
 * gradient) persists there until the next transition redraws it; rows
 * 180-181 and 198-199 stay black.  The port keeps one flat page, so the
 * compose folds both stages into it.
 *
 * @param page the page buffer
 */
static void _boot_compose_present(uint8_t *page)
{
    memmove(page, page + 0x500u, 180u * VIDEO_PAGE_W); // pool rows 4-183
    memset(page + 180u * VIDEO_PAGE_W, 0, 2u * VIDEO_PAGE_W); // rows 180-181
    memcpy(page + 182u * VIDEO_PAGE_W, _boot_bar, 16u * VIDEO_PAGE_W);
    memset(page + 198u * VIDEO_PAGE_W, 0, 2u * VIDEO_PAGE_W); // rows 198-199
}

/** @brief update_scene (1000:B9B5): the scrolled bac over the pool rows 0-183
 * plus the tree/apple sprites.  The caller advances the scroll when the cue
 * script's scroll gate is on.
 */
static void _boot_scene_render(void)
{
    uint8_t *page = video_page(0);
    _boot_scroll_render(page, _boot_bac, 0xE600u, _boot_scroll_pos, 0u);
    _boot_scroll_render(page, _boot_bac, 0xE600u, _boot_scroll_pos,
                        VIDEO_PAGE_W - (_boot_scroll_pos % VIDEO_PAGE_W));
    _boot_blit_sprite(_boot_treetop, 140u, 113u, 0x5A2u, 0x140u, page);
    _boot_blit_sprite(_boot_treebot, 21u, 59u, 0x608u, 0x8E80u, page);
    if (_boot_frame < 331u) // the apple leaves at frame 331
        _boot_blit_sprite(_boot_apple, 16u, 16u, 0x5B5u, 0x3700u, page);
}

/** @brief BOOT_START: load the boot's assets, build the two u32 object tables,
 * reset the fade scratch and start the title music (1000:B59F-b5FB).
 *
 * @return 1 while the boot intro shows, 0 when the assets cannot be loaded
 */
static int32_t _boot_start(void)
{
    _scene_fast = 1; // 1000:B59F
    _boot_unload();
    res_set_disc(0u); // the boot assets ship on disc 0
    _boot_gt = res_load_chunk(game_resource_name(GAME_RES_INTRO_GAMETEK_RSC),
                              &_boot_gt_size, NULL);
    _boot_bac =
        res_load_chunk(game_resource_name(GAME_RES_INTRO_BAC_BIT), NULL, NULL);
    _boot_treetop = res_load_chunk(
        game_resource_name(GAME_RES_INTRO_TREETOP_BIT), NULL, NULL);
    _boot_treebot = res_load_chunk(
        game_resource_name(GAME_RES_INTRO_TREEBOT_BIT), NULL, NULL);
    _boot_apple = res_load_chunk(game_resource_name(GAME_RES_INTRO_APPLE_SPR),
                                 NULL, NULL);
    _boot_words = res_load_chunk(game_resource_name(GAME_RES_INTRO_WORDS_BIT),
                                 NULL, NULL);
    _boot_rsc = res_load_chunk(game_resource_name(GAME_RES_INTRO_INTRO_RSC),
                               &_boot_rsc_size, NULL);
    _boot_itb =
        res_load_chunk(game_resource_name(GAME_RES_INTRO_ITB_BIT), NULL, NULL);
    if (!_boot_gt || !_boot_bac || !_boot_treetop || !_boot_treebot ||
        !_boot_apple || !_boot_words || !_boot_rsc || !_boot_itb) {
        _boot_unload();
        _boot_phase = BOOT_DONE;
        return 0;
    }
    _boot_entry_count = _boot_rsc_size / 4u;
    _boot_entries = g_osys->memAlloc(_boot_entry_count * sizeof(uint32_t));
    _boot_gt_entry_count = _boot_gt_size / 4u;
    _boot_gt_entries =
        g_osys->memAlloc(_boot_gt_entry_count * sizeof(uint32_t));
    _boot_list_buf = g_osys->memAlloc(OBJ_SEG_SIZE);
    _boot_obj = g_osys->memAlloc(OBJ_SEG_SIZE);
    _boot_gt_list = g_osys->memAlloc(OBJ_SEG_SIZE);
    if (!_boot_entries || !_boot_gt_entries || !_boot_list_buf || !_boot_obj ||
        !_boot_gt_list) {
        _boot_unload();
        _boot_phase = BOOT_DONE;
        return 0;
    }
    for (uint32_t i = 0; i < _boot_entry_count; i++)
        _boot_entries[i] = rd_le_u32(_boot_rsc + i * 4u);
    for (uint32_t i = 0; i < _boot_gt_entry_count; i++)
        _boot_gt_entries[i] = rd_le_u32(_boot_gt + i * 4u);

    /* the words stash (b7df: 4160 bytes copied to 202F:0x8689 - the
     * bar's 1-bit text, consumed by the transition handler) */

    /* b5d9-b5e8: the boot's fade to the zeroed ramp (to black), BP=2,
     * then b5f2-b5fb: play_tune / music_resume / timing_loop with
     * mus\title.ALB (the trio's music starts here, before the logo). */
    memset(_boot_ramp, 0, sizeof(_boot_ramp));
    _boot_bank_countdown = 0u;
    _boot_gt_group = 0u;
    _boot_gt_bank = 0u;
    palette_build_fade(0u, 256u, 2u, _boot_ramp);
    audio_play_tune(audio_tune_name(AUDIO_TUNE_TITLE)); // 1000:b5f2
    audio_music_resume(); // 1000:b5f8
    audio_fade_in(); // 1000:b5fb
    _boot_phase = BOOT_GT_PRE;
    return 1;
}

/** @brief BOOT_GT_PRE: wait out the fade to black and the title music's
 * fade-in, then decode the logo display list (entry 0), show its first group
 * and fade the first DAC entries into the embedded palette (1000:B24B-B26A).
 *
 * @return 1 while the boot intro shows
 */
static int32_t _boot_gt_pre(void)
{
    if (palette_fade_active()) return 1;
    /* b5f2-b5fb: the original blocks inside timing_loop while the title
     * music fades in, so the logo starts after the fade (~150 ticks).
     * The port's fade is non-blocking; wait for it here, in the flow. */
    if (audio_fade_busy()) return 1;
    /* b24b: decode the display-list object (entry 0) and relocate it:
     * [u16 group count][768-byte 6-bit palette][u16 object index per
     * group] - the original's fade targets (0x6FEA/0x7026/0x7032/
     * 0x704A) and idx array (0x72EA) are all offsets into this buffer */
    {
        uint16_t chk = 0;
        uint32_t off0 = rd_le_u32(_boot_gt);
        int32_t sz = res_decode_lzw_mem(_boot_gt, _boot_gt_size, off0,
                                        _boot_gt_list, OBJ_SEG_SIZE, &chk);
        if (sz < 0 || chk != 0) {
            /* b3cc's b446->b2c4: the failed decode unwinds
             * play_scene_sequence - init_level_scene still runs the
             * bank cycles (toward unrelocated-garbage palette bytes,
             * exactly as the asm reads 202F:0x6FE8's garbage) */
            _boot_hold = 30u;
            _boot_phase = BOOT_GT_BANK_HOLD;
            return 1;
        }
        (void)sz;
    }
    /* b24e-b260: the first group's frame shown, then the fade into the
     * embedded palette - the b26a branch ([0x627C] set for the boot):
     * DAC entries 0-3 only at BP=2.  The frames use entries 0-3 until
     * obj 28/29, whose 4-19 (TEK, blues) and 32-46 (Presents, gold)
     * pixels stay black until the bank cycles and the 0x20-0x2F fade
     * colour them - the original's sequence.  (The b27b branch - all
     * 256 entries at BP=0x19 - would show the 4-19 dark reds/teals
     * instead: the wrong colours.) */
    {
        uint16_t idx = rd_le_u16(_boot_gt_list + 0x302u);
        _boot_gt_show(idx);
    }
    /* b26a: the boot's first fade - the fast branch ([0x627C] is set for
     * the whole boot), i.e. entries 0-3 at BP=2; the b27b branch (all
     * 256 at BP=0x19) would show the 4-19 dark reds/teals */
    _scene_palette_fade(_boot_gt_list + 2u);
    _boot_phase = BOOT_GT_FADE0;
    return 1;
}

/** @brief BOOT_GT_FRAME: show the next gametek group (1000:B293-B2C2) and
 * arm the flip wait; a failed decode skips to the bank cycles.
 *
 * @return 1 while the boot intro shows
 */
static int32_t _boot_gt_frame(void)
{
    /* b293-b2c2: CX = count+1 iterations over the idx array (it holds
     * count+1 words, so the object tail is consumed exactly); the key test
     * lives in BOOT_GT_FLIP (b2ba, between the flip and the next frame).
     * The machine's count word reads 32, so the loop plays all count+1
     * frames including obj 29, whose expansion wipes the logo to a small
     * entry-8 blob. */
    uint16_t count = rd_le_u16(_boot_gt_list);
    if (_boot_gt_group > count) {
        _boot_hold = 30u; // bf19: the 30-tick pre-bank hold
        _boot_phase = BOOT_GT_BANK_HOLD;
        return 1;
    }
    {
        uint16_t idx = rd_le_u16(_boot_gt_list + 0x302u + _boot_gt_group * 2u);
        if (!_boot_gt_show(idx)) {
            /* the game's b446->b2c4: a failed decode (the checksum!)
             * unwinds play_scene_sequence - the remaining groups are
             * skipped, straight to the bank cycles */
            _boot_hold = 30u; // bf19: the 30-tick pre-bank hold
            _boot_phase = BOOT_GT_BANK_HOLD;
            return 1;
        }
    }
    _boot_gt_group++;
    _boot_hold = 1u; /* b2d3: the boot branch ([0x627C]
                                        * set, b59f): [0x5AC8] >= 1 */
    _boot_phase = BOOT_GT_FLIP;
    return 1;
}

/** @brief BOOT_BLACK: after the 0x8C-tick black hold, decode the scene's
 * display list (entry 0), reset the cue/frame state and present frame 0
 * (1000:B707-B748).
 *
 * @return 1 while the boot intro shows, 0 when the display list is corrupt
 */
static int32_t _boot_black(void)
{
    /* decode the display-list object (entry 0): [u16 ?][records...] -
     * the records start at object+2 ([0x6468] init 0x6FEA = 0x6FE8+2) */
    {
        uint16_t chk = 0;
        int32_t sz =
            res_decode_lzw_mem(_boot_rsc, _boot_rsc_size, _boot_entries[0],
                               _boot_list_buf, OBJ_SEG_SIZE, &chk);
        if (sz < 0 || chk != 0) {
            _boot_unload();
            _boot_phase = BOOT_DONE;
            return 0;
        }
        _boot_list_size = (uint32_t)sz;
        _boot_list_start = 2u;
    }
    _boot_list_pos = _boot_list_start;
    _boot_frame = 0u;
    _boot_group_dwell = 1u;
    _boot_dwell_count = 1u;
    _boot_scroll_pos = 0u;
    /* 1000:b71e-b739: render and present frame 0 before the cue loop,
     * so the frame-0 transition flash runs over the scene, as the
     * original does. */
    _boot_scene_render();
    (void)_boot_blit_group(_boot_list_buf, _boot_list_size, _boot_rsc,
                           _boot_rsc_size, _boot_entries, _boot_entry_count,
                           _boot_obj, _boot_list_pos, video_page(0));
    _boot_compose_present(video_page(0));
    g_osys->videoPresent(0);
    _boot_list_pos = _boot_list_start; // b748
    script_intro_start(&_boot_script);
    _boot_phase = BOOT_FRAME;
    return 1;
}

/** @brief BOOT_FRAME: one cue-driven scene frame - the cues, the seek, the
 * transition trigger, the caption bank countdown, the scroll render, the
 * group blit and the present (1000:B74E-B780).
 *
 * @return 1 while the boot intro shows
 */
static int32_t _boot_frame_step(void)
{
    script_fire(&_boot_script, _boot_frame);
    /* the dwell cue (cmd 7, 1000:B94E): both [0x6460] and [0x6462] take the
     * argument, restarting the group countdown at the cue's frame */
    if (script_dwell_pending(&_boot_script)) {
        _boot_group_dwell = script_group_dwell(&_boot_script);
        _boot_dwell_count = _boot_group_dwell;
        script_dwell_done(&_boot_script);
    }
    if (script_quit(&_boot_script) || script_done(&_boot_script)) {
        /* b791-b7b8: the scene's tail - the final frame stays up for
         * 0x8C ticks, then everything fades to the zeroed ramp (to
         * black) at BP=0x1e.  (The asm's own re-render + re-present of
         * the last frame and its b611 boot-tail fade - from black to
         * the same zeroed ramp, BP=0x23 - are visual no-ops here.) */
        _boot_hold = 0x8Cu;
        _boot_phase = BOOT_SCENE_HOLD;
        return 1;
    }
    if (script_seek_pending(&_boot_script)) {
        _boot_list_pos =
            _boot_seek(_boot_list_buf, _boot_list_size, _boot_list_start,
                       script_seek_frame(&_boot_script));
        _boot_frame = script_seek_frame(&_boot_script);
        script_seek_done(&_boot_script);
    }
    /* the transition cue (cmd 1): run_screen_transition (1000:B82E) -
     * the white flash (the 0x48BC ramp at BP=2), then the words redraw
     * and the fade back.  It blocks the frame loop in the original. */
    {
        uint16_t ti = script_transition_index(&_boot_script);
        if ((int32_t)ti != _boot_bar_arg) {
            _boot_bar_arg = (int8_t)ti;
            /* b840-b852: the white ramp - 0x3F everywhere but entry 0
             * (bytes 0x48BC-0x48BE are never filled) and the 0xE0-0xEF
             * bank (zeroed: the captions stay black through the flash) */
            memset(_boot_ramp, 0x3Fu, sizeof(_boot_ramp));
            _boot_ramp[0] = _boot_ramp[1] = _boot_ramp[2] = 0u;
            memset(_boot_ramp + 0x2A0u, 0, 0x30u);
            palette_build_fade(0u, 256u, 2u, _boot_ramp);
            _boot_phase = BOOT_TRANS;
            return 1;
        }
    }
    /* fade_countdown (b7c0), each frame: 0x28 ticks after the
     * transition, fade the 0xE0-0xEF bank toward 202F:0x4DCC - nothing
     * in the EXE writes that region, so the target is whatever
     * palette_load_6bit left at working+0x211 */
    if (_boot_bank_countdown != 0u && --_boot_bank_countdown == 0u)
        palette_build_fade(0xE0u, 0x10u, 0x28u, palette_working() + 0x211u);
    /* update_scene (1000:B9B5): the scroll render + the sprites; the
     * scroll position advances after the render (when scroll is on) */
    _boot_scene_render();
    if (script_scroll_on(&_boot_script))
        _boot_scroll_pos += script_scroll_delta(&_boot_script);
    /* blit_display_list: the group's overlays; the dwell counts down
     * per frame - when it expires, the next group + frame */
    {
        uint8_t *page = video_page(0);
        uint32_t next = _boot_blit_group(
            _boot_list_buf, _boot_list_size, _boot_rsc, _boot_rsc_size,
            _boot_entries, _boot_entry_count, _boot_obj, _boot_list_pos, page);
        if (--_boot_dwell_count == 0) {
            _boot_dwell_count = _boot_group_dwell;
            _boot_list_pos = next;
            _boot_frame++;
        }
    }
    /* the present: the pool's rows 4-183 into the visible rows 0-179
     * (deplanarise_bitmap), the bar (rows 182-197) and the black
     * margins composed in - the original's bar lives in the visible
     * page below every present, untouched until the next transition */
    _boot_compose_present(video_page(0));
    g_osys->videoPresent(0);
    /* 1000:b780 waits `[0x5AC8] >= 2`; doubled only for this scene -
     * deviation D-248 (requested slowdown) */
    _boot_wait_ticks = 4u;
    _boot_phase = BOOT_WAIT;
    return 1;
}

uint8_t scene_play_boot_intro(void)
{
    if (_boot_phase == BOOT_DONE) {
        _scene_fast = 0;
        return 0;
    } // 1000:B623

    switch (_boot_phase) {
    case BOOT_START: return (uint8_t)(_boot_start());

    case BOOT_GT_PRE: return (uint8_t)(_boot_gt_pre());

    case BOOT_GT_FADE0:
        if (palette_fade_active()) return 1;
        _boot_gt_group = 0u;
        _boot_phase = BOOT_GT_FRAME;
        return 1;

    case BOOT_GT_FRAME: return (uint8_t)(_boot_gt_frame());

    case BOOT_GT_FLIP:
        /* the cadence's remaining tick: wait_flag_timeout (b2d3)'s boot
         * branch waits for the first ISR tick ([0x5AC8] >= 1), then 87c7's
         * flip spins until the ISR applies it at the next boundary - 2 ticks
         * per group with BOOT_GT_FRAME's.  (The 6-tick wait is the
         * [0x627C]-clear branch - the level scene players, not the boot.) */
        if (--_boot_hold != 0u) return 1;
        /* b2ba: the key test sits between the flip and the next frame.
         * b2c4 is play_scene_sequence's OWN epilogue - the RET lands back
         * in init_level_scene (bf0f) and the bank cycles still run.  A
         * TAPPED key skips only the remaining frames; a HELD key aborts
         * the whole boot in the bank hold's first c0c6 poll. */
        if (_boot_key_state()) {
            _boot_hold = 30u; // bf19: the 30-tick pre-bank hold
            _boot_phase = BOOT_GT_BANK_HOLD;
            return 1;
        }
        _boot_phase = BOOT_GT_FRAME;
        return 1;

    case BOOT_GT_BANK_HOLD:
        if (_boot_abort_poll()) return 1; // c0b8's c0c6 poll (bf1c)
        if (--_boot_hold != 0u) return 1;
        /* bf21: this bank's 4 entries fade into gtpal+0x48 (entry 24),
         * BP=3 - the banks start at DAC entry 4, +4 per cycle */
        palette_build_fade(4u + _boot_gt_bank * 4u, 4u, 3u,
                           _boot_gt_list + 2u + 0x48u);
        _boot_phase = BOOT_GT_BANK_A;
        return 1;

    case BOOT_GT_BANK_A:
        if (_boot_abort_poll()) return 1; // c0af (bf2e)
        if (palette_fade_active()) return 1;
        /* bf32: the same bank into gtpal+0x3C (entry 20), BP=0x1e */
        palette_build_fade(4u + _boot_gt_bank * 4u, 4u, 0x1Eu,
                           _boot_gt_list + 2u + 0x3Cu);
        _boot_phase = BOOT_GT_BANK_B;
        return 1;

    case BOOT_GT_BANK_B:
        if (_boot_abort_poll()) return 1; // c0af (bf3e)
        if (palette_fade_active()) return 1;
        if (++_boot_gt_bank < 4u) {
            _boot_hold = 30u;
            _boot_phase = BOOT_GT_BANK_HOLD;
            return 1;
        }
        /* bf48: entries 0x20-0x2F into gtpal+0x60 (entry 32), BP=0x32 */
        palette_build_fade(0x20u, 0x10u, 0x32u, _boot_gt_list + 2u + 0x60u);
        _boot_phase = BOOT_GT_LAST_F;
        return 1;

    case BOOT_GT_LAST_F:
        if (_boot_abort_poll()) return 1; // c0af (bf57)
        if (palette_fade_active()) return 1;
        _boot_hold = 0x1A4u; // bf5a: the 420-tick hold
        _boot_phase = BOOT_GT_HOLD;
        return 1;

    case BOOT_GT_HOLD:
        if (_boot_abort_poll()) return 1; // c0b8's c0c6 poll (bf5d)
        if (--_boot_hold != 0u) return 1;
        /* bf60: fade everything to 0x4BBC - the GT stage never calls
         * palette_load_6bit, so that buffer is still zeroed here: to
         * black.  (palette_working() is the port's 0x4BBC and is zeroed
         * for the same reason.) */
        palette_build_fade(0u, 256u, 0x32u, palette_working());
        _boot_phase = BOOT_GT_OUT;
        return 1;

    case BOOT_GT_OUT:
        if (_boot_abort_poll()) return 1; // c0af (bf6f)
        if (palette_fade_active()) return 1;
        g_osys->memFree(_boot_gt);
        _boot_gt = NULL;
        _boot_gt_size = 0;
        g_osys->memFree(_boot_gt_entries);
        _boot_gt_entries = NULL;
        _boot_gt_entry_count = 0;
        g_osys->memFree(_boot_gt_list);
        _boot_gt_list = NULL;
        if (!_boot_card_show(game_resource_name(GAME_RES_INTRO_IDI_BIT))) {
            _boot_unload();
            _boot_phase = BOOT_DONE;
            return 0;
        }
        /* bd87: fade everything to idi's own palette, BP=0x32 */
        palette_build_fade(0u, 256u, 0x32u, palette_working());
        _boot_phase = BOOT_IDI_FADE;
        return 1;

    case BOOT_IDI_FADE:
        if (_boot_abort_poll()) return 1; // c0af (bd96)
        if (palette_fade_active()) return 1;
        _boot_hold = 0x1A4u; // bd99: the 420-tick hold
        _boot_phase = BOOT_IDI_HOLD;
        return 1;

    case BOOT_IDI_HOLD:
        if (_boot_abort_poll()) return 1; // c0b8's c0c6 poll (bd9c)
        if (--_boot_hold != 0u) return 1;
        /* bd9f: fade everything to the zeroed ramp, BP=0x32 */
        palette_build_fade(0u, 256u, 0x32u, _boot_ramp);
        _boot_phase = BOOT_IDI_OUT;
        return 1;

    case BOOT_IDI_OUT:
        if (_boot_abort_poll()) return 1; // c0af (bdae)
        if (palette_fade_active()) return 1;
        if (!_boot_card_show(game_resource_name(GAME_RES_INTRO_FIRST_BIT))) {
            _boot_unload();
            _boot_phase = BOOT_DONE;
            return 0;
        }
        /* bd27-bd3c: the staggered fade-in, bank 0: DAC entries 16-31 into
         * the working palette's own bank, BP=0x23 (7 banks, +16 per bank) */
        _boot_gt_bank = 0u;
        palette_build_fade(0x10u, 0x10u, 0x23u, palette_working() + 0x30u);
        _boot_phase = BOOT_FIRST_BANK;
        return 1;

    case BOOT_FIRST_BANK:
        if (_boot_abort_poll()) return 1; // c0af (bd3c)
        if (palette_fade_active()) return 1;
        _boot_hold = 0x118u; // bd3f: the 280-tick hold per bank
        _boot_phase = BOOT_FIRST_HOLD;
        return 1;

    case BOOT_FIRST_HOLD:
        if (_boot_abort_poll()) return 1; // c0b8's c0c6 poll (bd42)
        if (--_boot_hold != 0u) return 1;
        if (++_boot_gt_bank < 7u) {
            /* bd48: the next bank - target = working+0x30+bank*0x30 */
            palette_build_fade(0x10u + _boot_gt_bank * 0x10u, 0x10u, 0x23u,
                               palette_working() + 0x30u +
                                   _boot_gt_bank * 0x30u);
            _boot_phase = BOOT_FIRST_BANK;
            return 1;
        }
        _boot_hold = 0x118u; // bd50: one more 0x118-tick hold
        _boot_phase = BOOT_FIRST_LASTHOLD;
        return 1;

    case BOOT_FIRST_LASTHOLD:
        if (_boot_abort_poll()) return 1; // c0b8's c0c6 poll (bd53)
        if (--_boot_hold != 0u) return 1;
        /* bd56-bd62: fade everything to the zeroed ramp, BP=0x23 */
        palette_build_fade(0u, 256u, 0x23u, _boot_ramp);
        _boot_phase = BOOT_FIRST_OUT;
        return 1;

    case BOOT_FIRST_OUT:
        if (_boot_abort_poll()) return 1; // c0af (bd65)
        if (palette_fade_active()) return 1;
        _boot_phase = BOOT_ITB_SHOW;
        return 1;

    case BOOT_ITB_SHOW:
        /* the ITB title card (present_frame 1000:BDBC: the 64768-byte card
         * = a 64000-byte image + palette).  present_frame loads the card's
         * OWN palette into the working palette (bdbc, palette_load_6bit
         * 8d24, SI=0xFA00 - the RAM copy only) and the fade below drives
         * the DAC to it, so the card displays through its own colours.
         * bac's palette_load_6bit (b6bc, SI=0xE600) refills the working
         * palette before the dwell but the DAC keeps the card's colours
         * until the post-dwell fade - the port does the same. */
        memcpy(video_page(0), _boot_itb, VIDEO_PAGE_BYTES);
        palette_load_working(_boot_itb + VIDEO_PAGE_BYTES);
        g_osys->memFree(_boot_itb);
        _boot_itb = NULL;
        /* b68f-b69e: fade the DAC to the working palette (= the card's own
         * palette) and wait; the card fades in.  The zeroed _boot_ramp is
         * the 202F:0x48BC scratch - clear_fade_ramp (b90f) zeroes it
         * after every transition, the port boots it zeroed. */
        palette_build_fade(0u, 256u, 0x23u, palette_working());
        _boot_phase = BOOT_CARD_FADE;
        return 1;

    case BOOT_CARD_FADE:
        if (_boot_abort_poll()) return 1; // c0af (b69e)
        if (palette_fade_active()) return 1;
        /* b6c3: bac's palette_load_6bit - the working palette only; the
         * DAC keeps the card's colours through the dwell (bac.bit =
         * [58880-byte screen][768-byte palette at +0xE600]) */
        palette_load_working(_boot_bac + 0xE600u);
        _dwell_ticks = 280u; // 0x118: the initial dwell
        _boot_phase = BOOT_DWELL;
        return 1;

    case BOOT_DWELL:
        if (--_dwell_ticks > 0) return 1; // no key skip in the original
        /* b6f2-b701: fade the DAC to the zeroed 0x48BC ramp (to black) and
         * wait - the card fades out */
        palette_build_fade(0u, 256u, 0x23u, _boot_ramp);
        _boot_phase = BOOT_FADEOUT;
        return 1;

    case BOOT_FADEOUT:
        if (_boot_abort_poll()) return 1; // c0af (b7b8)
        if (palette_fade_active()) return 1;
        _dwell_ticks = 0x8Cu; // b704/c0b8: the 140-tick hold
        _boot_phase = BOOT_BLACK;
        return 1;

    case BOOT_BLACK:
        if (_boot_abort_poll()) return 1; // c0b8's c0c6 poll (b707)
        if (--_dwell_ticks > 0) return 1;
        return (uint8_t)(_boot_black());

    case BOOT_FRAME: return (uint8_t)(_boot_frame_step());

    case BOOT_WAIT:
        if (--_boot_wait_ticks == 0) {
            if (_boot_abort_poll()) return 1; // b787: c0c6
            _boot_phase = BOOT_FRAME;
        }
        return 1;

    case BOOT_SCENE_HOLD:
        /* b7a3-b7b8: the scene's final hold, then the BP=0x1e fade to the
         * zeroed ramp; BOOT_OUTRO waits for it and unloads */
        if (_boot_abort_poll()) return 1; // c0b8's c0c6 poll (b7a6)
        if (--_boot_hold != 0u) return 1;
        palette_build_fade(0u, 256u, 0x1Eu, _boot_ramp);
        _boot_phase = BOOT_OUTRO;
        return 1;

    case BOOT_TRANS:
        if (_boot_abort_poll()) return 1; // c0af (b863)
        if (palette_fade_active()) return 1;
        /* b866: the words' block expands into BOTH visible pages - the
         * port folds it into its one page via the compose */
        _boot_bar_draw(_boot_bar_arg);
        _boot_compose_present(video_page(0));
        _boot_trans_ticks = 0xAu; // b869: the 10-tick hold
        _boot_phase = BOOT_TRANS_HOLD;
        return 1;

    case BOOT_TRANS_HOLD:
        if (--_boot_trans_ticks != 0u) return 1;
        /* b873-b896: fade back to the working palette, clear the ramp,
         * start the caption bank fade countdown */
        palette_build_fade(0u, 256u, 0xAu, palette_working());
        memset(_boot_ramp, 0, sizeof(_boot_ramp)); // clear_fade_ramp b90f
        _boot_bank_countdown = 0x28u; // [0x644E]
        _boot_phase = BOOT_FRAME;
        return 1;

    case BOOT_OUTRO:
        if (_boot_abort_poll()) return 1; // c0af (b7b8)
        if (palette_fade_active()) return 1;
        _boot_unload();
        _boot_phase = BOOT_DONE;
        return 0;

    case BOOT_ABORT:
        /* the c0c6 -> b60a unwind: clear_fade_ramp (b90f) zeroes 0x48BC -
         * the transition may have left its white ramp there - then the
         * final fade to the zeroed ramp, div 0x23 (b611-b61d), waited with
         * the NON-aborting 8b8b.  The remaining boot stages (the cards, the
         * caveman scene) are skipped and the boot ends black. */
        memset(_boot_ramp, 0, sizeof(_boot_ramp));
        palette_build_fade(0u, 256u, 0x23u, _boot_ramp);
        _boot_phase = BOOT_ABORT_FADE;
        return 1;

    case BOOT_ABORT_FADE:
        if (palette_fade_active()) return 1;
        _boot_unload();
        _boot_phase = BOOT_DONE;
        return 0;

    case BOOT_DONE: return 0;
    }
    return 0;
}

#if defined(HUMANSRE_TEST_PROBES)
/* Test probe (HUMANSRE_TEST_PROBES): never part of a release build. */
uint32_t scene_boot_phase_for_probe(void) { return (uint32_t)_boot_phase; }
#endif /* HUMANSRE_TEST_PROBES */