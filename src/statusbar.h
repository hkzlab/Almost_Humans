/* statusbar.h - the interface of src/statusbar.c: the in-level status bar
 *
 * The level's bottom 40 rows (y 152..191): panel4.bit's top 40 rows
 * +0xC0-tagged, plus five digit marks (y 172..179) cut from the
 * PANELN1+PANELN2 glyph strip: the countdown's minutes / sec-tens /
 * sec-ones and the tribe counter's tens / ones (re_map.md → "The in-level
 * status bar, decoded end to end").  statusbar_tick is the INT 8 hook at
 * 1000:96AD: the 70-frame second, the packed-decimal countdown with its
 * slides, the tribe counter, and the per-frame stamp.
 */

#ifndef HUMANSRE_STATUSBAR_H
#define HUMANSRE_STATUSBAR_H

#include "osystem.h"

/**
 * @brief Bind the status bar to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t statusbar_init(const OSystem *osys);

/** @brief build_statusbar_layout (1000:961B) + level_start's time pick (1000:9589):
 * `packedTimeBcd` = the level's chosen limit, AH = minutes, AL = BCD
 * seconds.  The caller picks the header's timeA/B/C by the LEVEL board's
 * counter (`[0x5E65]`, `frontend_option(FRONTEND_OPT_LEVEL)`), and the
 * members 12/10/8 by the same byte (entry `1000:00B1`); a loaded save
 * replaces both with its `+0x07` word and `+0x0B` members (`level.c`).
 * The tribe counter = [0x11FC] - [0x17FB] (members - required), DIV/MOD 10 —
 * the caller owns both counts: the required = the map's 0xAA-tile count
 * (load_level 1000:315F).
 *
 * Loads its own assets from the CURRENT disc — the level flow's HUMANS.001
 * (level_load's res_set_disc); a failed load leaves the bar inactive (the
 * headless no-data case).
 *
 * @param packedTimeBcd the packed BCD time
 * @param tribeMembers the tribe member count
 * @param tribeRequired the required member count
 */
void statusbar_start(uint16_t packedTimeBcd, uint32_t tribeMembers,
                     uint32_t tribeRequired);

/** @brief The INT 8 hook at 1000:96AD (gated on [0x5B12]) + the JMP to the stamp:
 * the marks are stamped every frame — frozen at their last state once the
 * bar goes inactive, exactly as the original's page keeps them.
 */
void statusbar_tick(void);

/** @brief The live +0xC0 palette bank's source (level_video_init 1000:32CD's copy
 * of _panel4's own palette): install at DAC entries 0xC0..0xFF.
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *statusbar_palette(void);

/** @brief object_record_init's goal form (1000:F682: [0x5B22] := 0xFF): a human
 * is lost — the displayed tribe counter rolls down by one.
 */
void statusbar_arm_tribe_count(void);

/**
 * @brief Accessor: the countdown reached zero.
 *
 * [0x5E44]: the countdown reached zero (the time-up dialog's flag).
 *
 * @return The countdown reached zero.
 */
int32_t statusbar_time_up(void);

/**
 * @brief 1000:F5E3: the tribe counter is non-zero ([0x5B25] | [0x5B26] != 0).
 *
 * The level-end probe's two branches key on this test: a count takes the
 * goal form (type 0x21, the roll), an empty counter frees the record.
 *
 * @return 1 when the counter is non-zero, 0 otherwise
 */
int32_t statusbar_tribe_count_nonzero(void);

/** @brief The tribe counter's digits are [0x5B25]/[0x5B26]; the game only writes them through
 * statusbar_start (members - required) and the roll animation.  This setter is
 * a test seam for the object machine's level-end probe, whose two branches key
 * on that word.
 *
 * test seam: sets the tribe counter digits ([0x5B25]/[0x5B26]) that 1000:F5BC gates
 * on.
 *
 * @param tens the tens digit
 * @param ones the ones digit
 */
void statusbar_set_tribe_digits(uint32_t tens, uint32_t ones);

/**
 * @brief Accessor: one tribe-count digit ([0x5B25]/[0x5B26] by index).
 *
 * @param i the index
 * @return One tribe-count digit.
 */
uint32_t statusbar_tribe_digit(uint32_t i);

/** @brief 0x5B22..0x5B29, the save's +0x17F0 block: state, the two slide
 * countdowns, the two digits, the two phase bytes and the 0x5B28 gap.
 *
 * @param out the output buffer
 */
void statusbar_save_params(uint8_t *out);

/**
 * @brief Load the bar's per-level parameters.
 *
 * @param in the input state
 */
void statusbar_load_params(const uint8_t *in);

/** @brief The two digits as the number the check_highscore arithmetic reads (1000:41B8:
 * `[0x5B25]*10 + [0x5B26]`).
 *
 * @return the two-digit tribe value
 */
uint32_t statusbar_tribe_value(void);

/** @brief [0x5B12]: the ticking gate.  The in-level dialogs clear it (A167's head)
 * and restore it on return; the band keeps drawing while it is clear.
 *
 * @param on the on/off flag
 */
void statusbar_set_active(int32_t on);

/**
 * @brief Set the bar's split row.
 *
 * @param row the row
 */
void statusbar_set_split(uint32_t row);

/** @brief draw_level_text 0x342B: park the split at 0xC9 (off-screen) so the bar is
 * not composed; the level's ISR hook (0x331E) is already uninstalled, so it
 * stays hidden ([0x5B0A], 201..152 reveal).
 */
void statusbar_hide(void);

/**
 * @brief Accessor: the bar's split row.
 *
 * @return The bar's split row.
 */
uint32_t statusbar_split(void);

/* [0x5B56]: the current ACTION program — a raw byte; draws both the program
 * and the action row's group.  An id outside 1..6 has no header (D-186).
 * object.c owns it; the bar follows. */

/** @brief 1000:3933's tail: step the selected cell - the program's cursor byte - with
 * the wrap to 1 past the count.  The bar is redrawn every tick, so the store is
 * the whole effect.
 *
 * 1000:3933's tail: advance the current action row's selected cell (the 0x5B2B
 * header's second byte) with the wrap to 1 past the count.
 */
void statusbar_cycle_action(void);

/** @brief 1000:3747: the jump/throw charge bar, drawn into the bar band from the
 * acting record's `+0xE`.  Two halves, differing in both art and position:
 *
 *  - `+0xE` < 0x100 (0x374B): a generated 2-px-wide, 10-row stroke at band row
 *    15, byte column `(0x60 + (+0xE >> 1)) >> 2`, plane `(0x60 + v>>1) & 3`,
 *    colours `DF E0 E1 E2 E3 E3 E2 E1 E0 DF`; the plane bit walks one further
 *    (wrapping into the next byte column) for the second of the two passes
 *    (`0x376D: CX = 2`).
 *  - `+0xE` >= 0x100 (0x37C3): the folded length `len = (0x1FF - v) >> 1`
 *    selects a 2-px-wide, 15-row slice of the **staged** band art at
 *    `0x3EE0 + len` in the +0x3200 staging (1000:99D8 copies panel4's first
 *    0xC800 bytes there), i.e. panel4's `0xCE0 + len` with the +0xC0 tag
 *    (source rows 0x140 apart, the two pixels 4 apart), placed at band row
 *    10, column `24 + (len >> 2)`, plane `len & 3`.
 *
 * The port is per-pixel, so a planar byte becomes the pixel `4*col + plane`.
 *
 * @param v the charge value
 */
void statusbar_charge_bar(uint32_t v);

/** @brief 1000:99D8 + 0x9B7D + 0x9A18 at the charge entry (0x36DB):
 * `shift_screen_up` prepares the clean band, the `0x9B7D` call with
 * `AX=0x5F/BX=0x0C/CX=0x82/DX=0x0F` draws the sunken 130x15 trough (the four
 * ±4 edge passes from the zeroed `0x48BC` ramp), and `blit_area_if_dirty`
 * carries the x 48..271 slice to the visible band.  The port's page is
 * recomposed each tick, so this runs before every charging frame's strokes.
 *
 * 1000:36DB's entry: 0x99D8 + 0x9B7D + 0x9A18 — restore the band's x 48..271
 * slice over the action row and draw the sunken trough.
 */
void statusbar_charge_begin(void);

/** @brief 1000:3738's exit: `0x98F8`/`0x990A` restore the clean band slice and
 * redraw the action row over it (the same closing `0x9A18` blit).
 */
void statusbar_charge_end(void);

/**
 * @brief [0x5B56] itself, for the tests: the group the bar's next draw uses.
 *
 * @return the action group index
 */
uint32_t statusbar_action_group(void);

/**
 * @brief the group's icon count (its header's first byte), for the tests.
 *
 * @param group the group
 * @return the icon count
 */
uint32_t statusbar_action_count(uint32_t group);

/**
 * @brief the group's selected-cell byte, for the tests (0 past the table).
 *
 * @param group the group
 * @return the selected cell byte
 */
uint32_t statusbar_action_cell(uint32_t group);

/** @brief The countdown word the save writes at +0x07: minutes << 8 | BCD seconds
 * (the original's [0x5B19] and [0x5B1A]*16 + [0x5B1B]).
 *
 * @return the countdown word
 */
uint16_t statusbar_time_word(void);

/** @brief [0x1E84] after check_highscore's repack (1000:42CE): the *_elapsed* clock as
 * minutes<<8 | tens<<4 | ones — the value the per-level best compares.
 *
 * the elapsed clock [0x5B0C..0E] repacked as check_highscore does (1000:42CE)
 *
 * @return the elapsed time word
 */
uint16_t statusbar_elapsed_word(void);

#endif /* HUMANSRE_STATUSBAR_H */
