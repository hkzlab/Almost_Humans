/* The scene module's shared declarations: the ANI decoder, the fast-mode
 * flag the boot sets and the pre-frame fade both players use.  Nothing here
 * is part of the port's public API; include scene.h for that.
 */
#ifndef HUMANSRE_SCENE_INTERNAL_H
#define HUMANSRE_SCENE_INTERNAL_H

#include "scene/scene_ani.h"
#include "video.h"

#include <stdint.h>

#define PAGE_SIZE    VIDEO_PAGE_BYTES
#define OBJ_SEG_SIZE 65536u // the original decodes objects into a 64K segment

/* --- state shared between the players (defined in scene_play.c) ---------- */

/* [0x627C]: boot_intro_sequence's fast mode - its only writer (set at
 * 1000:B59F, cleared at 1000:B623, entry's caller only).  play_scene_sequence
 * reads it twice: the pre-frame palette fade is 4 entries div 2 instead of
 * 0x100 div 0x19 (1000:B263), and wait_flag_timeout waits 1 tick instead of
 * 6 per object (1000:B2D3).  The level's .ANI always runs with it clear. */
extern uint8_t _scene_fast;
extern uint16_t _dwell_ticks;

/** @brief play_scene_sequence's pre-frame palette fade (1000:B263), shared by the
 * level's .ANI and the boot's logo.
 *
 * @param target the target value
 */
void _scene_palette_fade(const uint8_t *target);

#endif /* HUMANSRE_SCENE_INTERNAL_H */
