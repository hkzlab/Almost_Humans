/* statusbar.h — the interface of src/statusbar.c: the bottom bar (subsystem 13, §12.7)
 *
 * The level's bottom 40 rows (y 152..191): panel4.bit's top 40 rows
 * +0xC0-tagged, plus five digit marks (y 172..179) cut from the
 * PANELN1+PANELN2 glyph strip — the countdown's minutes / sec-tens /
 * sec-ones and the tribe counter's tens / ones (re_map.md → "The in-level
 * status bar, decoded end to end").  statusbar_tick is the INT 8 hook at
 * 1000:96AD: the 70-frame second, the packed-decimal countdown with the
 * odometer slides, the tribe odometer, and the per-frame stamp.
 */

#ifndef HUMANSRE_STATUSBAR_H
#define HUMANSRE_STATUSBAR_H

#include "osystem.h"

int32_t statusbar_init(const OSystem *osys);
/* build_statusbar_layout (1000:961B) + level_start's pick: packedTimeBcd =
 * AH minutes, AL BCD seconds (the header's timeA/B/C); the tribe digits =
 * members - required (DIV/MOD 10).  Loads its own assets from the CURRENT
 * disc — the level flow's HUMANS.001 (level_load's res_set_disc); a failed
 * load leaves the bar inactive (the headless no-data case). */
void statusbar_start(uint16_t packedTimeBcd, uint32_t tribeMembers,
                     uint32_t tribeRequired);
void statusbar_tick(void);
/* level_video_init's palette copy (1000:32CD): panel4's own palette —
 * install at DAC entries 0xC0..0xFF (the +0xC0 bank). */
const uint8_t *statusbar_palette(void);
/* object_record_init's arm (1000:F682): [0x5B22] := 0xFF — the displayed
 * tribe counter rolls down by one (a follower joined). */
void statusbar_arm_odometer(void);
/* [0x5E44]: the countdown reached zero (the time-up dialog's flag). */
int32_t statusbar_time_up(void);
/* 1000:F5E3: [0x5B25] | [0x5B26] != 0 - the odometer is showing a count. */
int32_t statusbar_tribe_active(void);
/* test seam: sets the odometer digits ([0x5B25]/[0x5B26]) that 1000:F5BC gates on. */
void statusbar_set_tribe_digits(uint32_t tens, uint32_t ones);
uint32_t statusbar_tribe_digit(uint32_t i); /* [0x5B25]/[0x5B26] by index */
void statusbar_save_params(uint8_t *out); /* the 0x5B22..0x5B29 block   */
void statusbar_load_params(const uint8_t *in);
uint32_t statusbar_tribe_value(void); /* [0x5B25]*10 + [0x5B26] */
/* [0x5B12]: the ticking gate (the in-level dialogs clear it). */
void statusbar_set_active(int32_t on);
void statusbar_set_split(uint32_t row);
/* the level end's [0x5B0A] = 0xC9 (1000:342B): stop composing the bar */
void statusbar_hide(void); /* [0x5B0A], 201..152 reveal */
uint32_t statusbar_split(void);
/* [0x5B56] (1..6): the current ACTION program — draws both the program and
 * the action row's group.  object.c owns it; the bar follows. */
/* 1000:3933's tail: advance the current action row's selected cell (the
 * 0x5B2B header's second byte) with the wrap to 1 past the count. */
void statusbar_cycle_action(void);
/* 1000:3747: the jump/throw charge bar, v = the acting record's +0xE. */
void statusbar_charge_bar(uint32_t v);
/* the group's selected-cell byte (1-based), for tests; 0 past the table. */
uint32_t statusbar_action_group(void); /* [0x5B56]: the bar's current group */
uint32_t statusbar_action_count(uint32_t group);
uint32_t statusbar_action_cell(uint32_t group);
/* The countdown word for the save's +0x07: minutes<<8 | BCD seconds. */
uint16_t statusbar_time_word(void);
/* the elapsed clock [0x5B0C..0E] repacked as check_highscore does (1000:42CE) */
uint16_t statusbar_elapsed_word(void);

#endif /* HUMANSRE_STATUSBAR_H */
