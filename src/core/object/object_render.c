/* The object and terrain renderers: the sprite law, the class
 * overlays, the record partition and the TER slots.
 */

#include "object_internal.h"
#include "object_core.h"
#include "object_render.h"
#include "osystem.h"
#include "byteorder.h"
#include <string.h>

/* forward declarations */
static void _blit_sprite(uint8_t *page, const uint8_t *src, uint32_t rows,
                         uint32_t cols, int32_t x, int32_t y);
static uint32_t _obj_render_overlay_row(uint32_t frame);
static int32_t _obj_render_blit_g_frame(uint8_t *page, uint32_t frame,
                                        int32_t x, int32_t y);
static void _obj_render_blit_frame(uint8_t *page, uint32_t frame, int32_t x,
                                   int32_t y);
static void _obj_render_draw_player_overlay(uint8_t *page, int32_t scrollX,
                                            int32_t scrollY, const uint8_t *r,
                                            uint32_t i);
static void _obj_render_draw_class_overlay(uint8_t *page, int32_t scrollX,
                                           int32_t scrollY, uint8_t *rec);
static void _obj_render_draw_record(uint8_t *page, int32_t scrollX,
                                    int32_t scrollY, uint32_t i, int32_t scan);
static void _obj_render_partition(uint8_t *back, uint32_t *nb, uint8_t *front,
                                  uint32_t *nf);
static void _obj_render_span_draw(uint8_t *page, int32_t scrollX,
                                  int32_t scrollY);
static void _shade_row(const ObjSprite *sp, uint32_t pass, uint32_t row,
                       uint8_t *out, uint16_t *s);

static const uint8_t *_obj_bank; // APL\\SPRITES.DAT, the sprite image

static uint32_t _obj_bank_size;


/* --- the class/behaviour overlays of 1000:653C -------------------------
 *
 * draw_object_cell (1000:653C) draws the base (unless DS:0x3570[frame] bit 7)
 * and the class's carried sprite: the placed record the class names, moved by
 * a per-entry (dx, dy), plus one sprite at the record's own position.
 *
 *   0x3566  10 B     behaviour class -> overlay column (0 = none)
 *   0x3570  0x11A    frame -> overlay row | 0x80 ("no base tile")
 *   0x368A  75 x 3   row x column -> overlay entry index (0 = none)
 *   0x3766  144 x 4  entry: dx, dy, frame, trailing frame (0 = none)
 *
 * Frames past the rows read row 0 (the port ends at 0x11D).
 */
static const uint8_t _obj_cls_col[0x40] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x02, 0x00, 0x00, 0x01,
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
    0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x00, 0x12, 0x12, 0x12, 0x12, 0x12,
    0x00, 0x13, 0x13, 0x13, 0x13, 0x13, 0x14, 0x14, 0x15, 0x16, 0x16,
    0x16, 0xC3, 0xC4, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t _obj_frame_row[0x11A] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
    0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x00, 0x12, 0x12, 0x12, 0x12, 0x12, 0x00,
    0x13, 0x13, 0x13, 0x13, 0x13, 0x14, 0x14, 0x15, 0x16, 0x16, 0x16, 0xC3,
    0xC4, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC6, 0xC7,
    0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x18, 0x19, 0x00,
    0x00, 0x1A, 0x1B, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x1D, 0x1D, 0x1E, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x00, 0x00, 0x00, 0x2E, 0x2F, 0x30, 0x31,
    0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x37, 0x38, 0x39, 0x38, 0x3A, 0x3B,
    0x3C, 0x3B, 0x3D, 0x3E, 0x3F, 0x3E,
};

static const uint8_t _obj_overlay_idx[0xE1] = {
    0x40, 0x41, 0x42, 0x41, 0x01, 0x1C, 0x30, 0x02, 0x1D, 0x31, 0x03, 0x1E,
    0x32, 0x04, 0x1F, 0x33, 0x05, 0x20, 0x34, 0x06, 0x21, 0x35, 0x07, 0x22,
    0x36, 0x08, 0x23, 0x37, 0x09, 0x24, 0x38, 0x0A, 0x25, 0x39, 0x0B, 0x26,
    0x3A, 0x0C, 0x27, 0x3B, 0x0D, 0x28, 0x3C, 0x0E, 0x29, 0x3D, 0x0F, 0x2A,
    0x3E, 0x10, 0x2B, 0x3F, 0x11, 0x2C, 0x40, 0x11, 0x2C, 0x40, 0x12, 0x2D,
    0x41, 0x13, 0x2E, 0x42, 0x14, 0x2F, 0x43, 0x15, 0x8E, 0x8F, 0x16, 0x46,
    0x4C, 0x17, 0x47, 0x4D, 0x18, 0x48, 0x4E, 0x19, 0x49, 0x4F, 0x1A, 0x4A,
    0x50, 0x1B, 0x4B, 0x51, 0x00, 0x44, 0x00, 0x00, 0x45, 0x00, 0x52, 0x00,
    0x00, 0x53, 0x00, 0x00, 0x54, 0x00, 0x00, 0x55, 0x00, 0x00, 0x56, 0x00,
    0x00, 0x57, 0x00, 0x00, 0x58, 0x00, 0x00, 0x59, 0x00, 0x00, 0x5A, 0x00,
    0x00, 0x5B, 0x00, 0x00, 0x5C, 0x00, 0x00, 0x5D, 0x00, 0x00, 0x5E, 0x00,
    0x00, 0x5F, 0x00, 0x00, 0x60, 0x00, 0x00, 0x61, 0x00, 0x00, 0x62, 0x00,
    0x00, 0x63, 0x00, 0x00, 0x64, 0x00, 0x00, 0x65, 0x00, 0x00, 0x66, 0x00,
    0x00, 0x67, 0x00, 0x00, 0x68, 0x00, 0x00, 0x69, 0x00, 0x00, 0x6A, 0x00,
    0x70, 0x6B, 0x00, 0x71, 0x6C, 0x00, 0x72, 0x6D, 0x00, 0x73, 0x6E, 0x00,
    0x74, 0x6F, 0x00, 0x75, 0x76, 0x00, 0x00, 0x77, 0x00, 0x00, 0x78, 0x00,
    0x00, 0x79, 0x00, 0x00, 0x7A, 0x00, 0x00, 0x7B, 0x00, 0x00, 0x7C, 0x7F,
    0x82, 0x7D, 0x80, 0x83, 0x7E, 0x81, 0x84, 0x85, 0x88, 0x8B, 0x86, 0x89,
    0x8C, 0x87, 0x8A, 0x8D, 0x00, 0x00, 0x00, 0x00, 0x02,
};

static const uint8_t _obj_overlay_ent[0x240] = {
    0x00, 0x00, 0x00, 0x00, 0x02, 0x06, 0x59, 0x68, 0x04, 0x08, 0x5A, 0x69,
    0x06, 0x06, 0x5A, 0x6A, 0x04, 0x09, 0x5A, 0x6B, 0x01, 0x08, 0x5A, 0x6C,
    0x00, 0x09, 0x5A, 0x6D, 0xFC, 0x0A, 0x5B, 0x6E, 0xFF, 0x09, 0x5A, 0x6F,
    0xFF, 0x07, 0x66, 0x70, 0xFB, 0x05, 0x66, 0x71, 0xFC, 0x06, 0x66, 0x72,
    0xFC, 0x07, 0x66, 0x73, 0x00, 0x06, 0x66, 0x74, 0x01, 0x07, 0x66, 0x75,
    0x03, 0x07, 0x65, 0x76, 0x02, 0x08, 0x66, 0x77, 0x06, 0x08, 0x58, 0x78,
    0x00, 0x08, 0x66, 0x70, 0x00, 0x10, 0x66, 0x70, 0x00, 0x20, 0x66, 0x70,
    0x00, 0xFA, 0x5C, 0x00, 0xFE, 0xFB, 0x5B, 0x79, 0xFD, 0xFB, 0x5B, 0x7A,
    0xFC, 0xFC, 0x5A, 0x7B, 0x01, 0xF9, 0x65, 0x7C, 0x02, 0xF9, 0x65, 0x7D,
    0x05, 0xFB, 0x66, 0x7E, 0x00, 0x0C, 0x99, 0x68, 0x05, 0x0B, 0x9A, 0x69,
    0x07, 0x0A, 0x9B, 0x6A, 0x05, 0x0D, 0x9A, 0x6B, 0x01, 0x0C, 0x99, 0x6C,
    0xFF, 0x0D, 0x98, 0x6D, 0xFB, 0x0E, 0x97, 0x6E, 0xFE, 0x0D, 0x98, 0x6F,
    0x00, 0x0D, 0x99, 0x70, 0xFC, 0x0A, 0x98, 0x71, 0xFB, 0x0B, 0x97, 0x72,
    0xFC, 0x0D, 0x98, 0x73, 0x00, 0x0D, 0x99, 0x74, 0x02, 0x0D, 0x9A, 0x75,
    0x06, 0x0D, 0x9B, 0x76, 0x02, 0x0E, 0x9A, 0x77, 0x05, 0x0D, 0x99, 0x78,
    0x00, 0x0D, 0x99, 0x70, 0x00, 0x15, 0x99, 0x70, 0x00, 0x25, 0x99, 0x70,
    0x04, 0x05, 0xA2, 0x68, 0x06, 0x04, 0xA2, 0x69, 0x04, 0x03, 0xA3, 0x6A,
    0x03, 0x05, 0xA3, 0x6B, 0x00, 0x05, 0xA3, 0x6C, 0x03, 0x06, 0xA2, 0x6D,
    0xFE, 0x07, 0xA1, 0x6E, 0x02, 0x06, 0xA2, 0x6F, 0x00, 0x05, 0xA4, 0x70,
    0xFD, 0x03, 0xA4, 0x71, 0xFC, 0x02, 0xA4, 0x72, 0xFE, 0x05, 0xA4, 0x73,
    0x00, 0x05, 0xA4, 0x74, 0xFE, 0x05, 0xA5, 0x75, 0x02, 0x06, 0xA6, 0x76,
    0xFF, 0x06, 0xA5, 0x77, 0x06, 0x03, 0xAA, 0x78, 0x00, 0x05, 0xA4, 0x70,
    0x00, 0x0D, 0xA4, 0x70, 0x00, 0x1D, 0xA4, 0x70, 0xFC, 0x08, 0x96, 0x00,
    0x02, 0x08, 0x96, 0x00, 0xFD, 0xF9, 0x97, 0x79, 0xFC, 0xF9, 0x97, 0x7A,
    0xFB, 0xFA, 0x97, 0x7B, 0x03, 0xF9, 0x97, 0x7C, 0x04, 0xF9, 0x97, 0x7D,
    0x05, 0xFA, 0x97, 0x7E, 0x00, 0xF8, 0xA2, 0x79, 0xFF, 0xF8, 0xA2, 0x7A,
    0xFE, 0xF9, 0xA2, 0x7B, 0xFF, 0xF8, 0xA5, 0x7C, 0x00, 0xF8, 0xA5, 0x7D,
    0x01, 0xF9, 0xA5, 0x7E, 0x06, 0x09, 0x60, 0x00, 0x01, 0x10, 0x61, 0x00,
    0xF7, 0x08, 0x64, 0x00, 0xF9, 0x09, 0x60, 0x00, 0xFC, 0x10, 0x5F, 0x00,
    0x0A, 0x09, 0x5C, 0x00, 0xFD, 0x09, 0x5C, 0xEC, 0xFE, 0x09, 0x5C, 0xED,
    0x00, 0x08, 0x5C, 0xEE, 0x00, 0x06, 0x5B, 0xEF, 0xFE, 0x08, 0x5B, 0xF0,
    0xFF, 0x08, 0x5B, 0xF1, 0x00, 0x07, 0x5B, 0xF2, 0xFF, 0x06, 0x5B, 0xF3,
    0xFC, 0xFD, 0x5B, 0xF4, 0x02, 0x08, 0x64, 0xF5, 0x01, 0x08, 0x64, 0xF6,
    0xFF, 0x07, 0x64, 0xF7, 0x00, 0x06, 0x64, 0xF8, 0x03, 0x08, 0x64, 0xF9,
    0x01, 0x08, 0x64, 0xFA, 0x00, 0x07, 0x64, 0xFB, 0x02, 0x06, 0x64, 0xFC,
    0x04, 0xFB, 0x65, 0xFD, 0x04, 0x07, 0x5B, 0x68, 0x08, 0x07, 0x5B, 0x69,
    0x09, 0x06, 0x5B, 0x6A, 0xFC, 0x07, 0x64, 0x70, 0xF9, 0x07, 0x64, 0x71,
    0xF6, 0x03, 0x65, 0x72, 0x06, 0x04, 0xA2, 0x68, 0x08, 0x04, 0xA2, 0x69,
    0x0A, 0x03, 0xA2, 0x6A, 0xFB, 0x04, 0xA5, 0x70, 0xF8, 0x04, 0xA5, 0x71,
    0xF6, 0x03, 0xA5, 0x72, 0xFE, 0x09, 0x5C, 0xEC, 0xFF, 0x09, 0x5C, 0xED,
    0x01, 0x08, 0x5C, 0xEE, 0x02, 0x08, 0x64, 0xF5, 0x01, 0x08, 0x64, 0xF6,
    0x00, 0x06, 0x64, 0xF8, 0xFB, 0xFA, 0x59, 0x00, 0xFB, 0xFD, 0x58, 0x00,
    0xFB, 0x01, 0x58, 0x00, 0xFA, 0xFF, 0x97, 0x00, 0xFA, 0x05, 0x97, 0x00,
    0xFA, 0x0A, 0x97, 0x00, 0xFC, 0xF9, 0xA4, 0x00, 0xFB, 0xFE, 0xA4, 0x00,
    0xFC, 0x03, 0xA4, 0x00, 0xFA, 0xFD, 0x66, 0x00, 0xFD, 0xF9, 0x66, 0x00,
    0xFF, 0xF8, 0x66, 0x00, 0xF8, 0x05, 0x97, 0x00, 0xFB, 0x00, 0x97, 0x00,
    0xFD, 0xFE, 0x97, 0x00, 0xFB, 0xFC, 0xA4, 0x00, 0xFD, 0xF8, 0xA4, 0x00,
    0xFF, 0xF8, 0xA4, 0x00, 0x00, 0xF2, 0x96, 0x00, 0x00, 0xF7, 0xA1, 0x00,
};


/* The DEFS field-G sprites: level_setup_screen stores each G block's far
 * pointer into the frame table at 0x3CAC (slots 0xC1 + 4*layer + i), so the
 * F0/F1/F2 decoration records (frames 0xC1/0xC5/0xC9) draw the level's G art -
 * the water-hole sprites.  The image's own slots are zero there, which is why
 * the generator leaves them blank; the live table holds the G pointers. */
static const uint8_t *_obj_g_sprites;

static uint16_t _obj_g_blocks;

static uint8_t _obj_gn;

static const uint8_t *_obj_e_sprite, *_obj_h_sprites;


/* --- 1000:5C2F draw_objects_pass2: the two TER.DAT slots (DS:0x649A) ----
 *
 * `update_object_list` ends with a two-entry loop over `0x649A` (`0x14` bytes
 * each): `AX = [slot] - 0xC`, `BX = [slot+2]`, `CX = [slot+0xC]`, and the slot is
 * skipped when `word [slot+0x10]` has bit 15 set.  Otherwise `ES` becomes the
 * `apl\TER.dat` slot (`[0x13AF]`) and `0x5C2F` reads, for the index
 * `byte[0x2D7E + CX]`: `BE16(4*idx + 2) + 0x40` (the source), `BE16(2*idx + 0x20)`
 * (a y offset) and `BE16(2*idx + 0x30)` (the rows), then jumps to
 * `set_object_layer` (`0x5C5F`, mode 2) - the same blitter as the entity cells.
 *
 * A level start disables both slots: `object_array_init` (`0x4CAB`) writes
 * 0xFFFF into both flag words (`[0x64AA]`, `[0x64BE]`), which is the state the
 * live game reads on level 1 (`00 x16, ff ff, 00 00`).  The 0xAF/0xB1 tile
 * handlers clear that word again and fill the slot, so the slot is drawn (and
 * patrols, see `object_render_camera_slots_tick`) on every level carrying the pair:
 * 46 of the 160 shipped levels have 0xAF+0xB0 and 11 have 0xB1+0xB2.
 */
uint8_t _obj_ter_slot[2][0x14];


/* the index table at DS:0x2D7E (the caller's `+0xC` word goes through it).
 * The table is 16 bytes; the original reads the 0x2D92 follower pairs for
 * indexes 16..19, which the port drops: the slots' index is 0 at spawn and the
 * patrol moves it 0..11 (D-267). */
static const uint8_t _obj_ter_index[16] = {
    0x00, 0x01, 0x02, 0x03, 0x03, 0x02, 0x04, 0x05,
    0x06, 0x07, 0x06, 0x05, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t *_obj_ter_bank;

static uint32_t _obj_ter_size;


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint32_t object_render_frame_slots(void) { return OBJ_FRAME_SLOTS; }
#endif /* HUMANSRE_TEST_PROBES */


#if defined(HUMANSRE_TEST_PROBES)
void object_render_frame_for_probe(uint32_t frame, uint32_t *off,
                                   uint32_t *rows)
{
    if (frame >= OBJ_FRAME_SLOTS) {
        *off = 0;
        *rows = 0;
        return;
    }
    *off = object_sprite_at(frame).off;
    *rows = object_sprite_at(frame).rows;
}
#endif /* HUMANSRE_TEST_PROBES */


void object_render_publish_bank(const uint8_t *bank, uint32_t size)
{
    _obj_bank = bank;
    _obj_bank_size = size;
}


/** @brief 1000:653C's base blit: the sprite at `src`, top-left (x,y), 0 = transparent.
 * `cols` is 16 for mode 1 (`1000:66EC`) and 32 for mode 2 (`1000:6C74`) - the
 * only difference between the two (the frame stride confirms it: mode-2 frames
 * advance rows*32 bytes).  The layout is derived, not probed: the blit is four
 * plane passes (`67BA: AH = 1 << ([0x6C32] & 3)`, `OUT 0x3C4`), each row-taking
 * `LODSW`/store/INC DI x2 (`67F9..681A`) = **4 bytes, one per pixel, 4 px per
 * byte of one plane**; between passes `6C15 ADD SI, [0x6C2C]` and [0x6C2C] is 0
 * unless clipped at the top, so the frame is consumed linearly in four blocks
 * of 4h (`block = col mod 4`, `byte = row*4 + col/4`).
 *
 * @param page the page buffer
 * @param src the source buffer
 * @param rows the row count
 * @param cols the column count
 * @param x the x coordinate
 * @param y the y coordinate
 */
static void _blit_sprite(uint8_t *page, const uint8_t *src, uint32_t rows,
                         uint32_t cols, int32_t x, int32_t y)
{
    uint32_t w = cols >> 2, block = rows * w;

    for (uint16_t row = 0; row < rows; row++) {
        int32_t py = y + (int32_t)row;
        if (py < 0 || py >= (int32_t)VIDEO_PAGE_H_MODE13H) continue;
        for (uint16_t col = 0; col < cols; col++) {
            uint8_t v = src[(col & 3u) * block + row * w + (col >> 2)];
            int32_t px = x + (int32_t)col;
            if (!v) continue;
            if (px < 0 || px >= (int32_t)VIDEO_PAGE_W) continue;
            page[(uint32_t)py * VIDEO_PAGE_W + (uint32_t)px] = v;
        }
    }
}


/**
 * @brief Accessor: the overlay row for one frame.
 *
 * @param frame the frame index
 * @return the overlay row (0 past the table)
 */
static uint32_t _obj_render_overlay_row(uint32_t frame)
{
    /* the image's table at DS:0x3570 runs to frame 0x119 (0x29A bytes before
     * the 0x368A index table); frames past it read row 0, as the port's
     * frames 0x11A..0x11D do. */
    return frame < 0x11Au ? _obj_frame_row[frame] : 0u;
}


void object_render_publish_g_sprites(const uint8_t *data, uint16_t blocks,
                                     uint8_t n)
{
    _obj_g_sprites = data;
    _obj_g_blocks = blocks;
    _obj_gn = n;
}


void object_render_publish_deh_sprites(const uint8_t *e, const uint8_t *h)
{
    _obj_e_sprite = e;
    _obj_h_sprites = h;
}


/**
 * @brief Blit one G-bank frame; 1 = inside the bank.
 *
 * @param page the page buffer
 * @param frame the frame index
 * @param x the x coordinate
 * @param y the y coordinate
 * @return 1 when the frame is inside the bank, 0 otherwise
 */
static int32_t _obj_render_blit_g_frame(uint8_t *page, uint32_t frame,
                                        int32_t x, int32_t y)
{
    uint32_t slot, layer, i, block;

    if (!_obj_g_sprites || !_obj_gn || frame < 0xC1u || frame >= 0xC1u + 12u)
        return 0;
    slot = frame - 0xC1u;
    layer = slot / 4u;
    i = slot % 4u;
    if (i >= _obj_gn) return 0;
    block = layer * _obj_gn + i;
    if (block >= _obj_g_blocks) return 0;
    _blit_sprite(page, _obj_g_sprites + block * 0x100u, 16u, 16u, x, y);
    return 1;
}


/**
 * @brief Blit one object frame.
 *
 * @param page the page buffer
 * @param frame the frame index
 * @param x the x coordinate
 * @param y the y coordinate
 */
static void _obj_render_blit_frame(uint8_t *page, uint32_t frame, int32_t x,
                                   int32_t y)
{
    ObjSprite sp;
    uint8_t cols;

    if (_obj_render_blit_g_frame(page, frame, x, y)) return;
    /* The DEFS field-D/H pointers land in these slots: 0x33..0x37 (the five
     * A9 logs, all the same 8x16 field-E sprite) and 0x26/0x27 (field H). */
    if (_obj_e_sprite && frame >= 0x33u && frame <= 0x37u) {
        _blit_sprite(page, _obj_e_sprite, 8u, 16u, x, y);
        return;
    }
    if (_obj_h_sprites && (frame == 0x26u || frame == 0x27u)) {
        _blit_sprite(page, _obj_h_sprites + (frame - 0x26u) * 0x100u, 16u, 16u,
                     x, y);
        return;
    }
    if (frame >= OBJ_FRAME_SLOTS) return;
    sp = object_sprite_at(frame);
    if (!sp.rows || sp.mode > 2u) return; // blank / mode 0 or 3
    cols = sp.mode == 2u ? 32u : 16u; // 1000:6C74 vs 1000:66EC
    if (sp.off + sp.rows * cols > _obj_bank_size) return;
    _blit_sprite(page, _obj_bank + sp.off, sp.rows, cols, x,
                 y + (int32_t)sp.ybias);
}


/* 202F:0x3541 (8 bytes): the per-frame-group bit that gates the selected
 * record's overlay sprite.  Frames 0x00..0x16 and 0x18..0x25 pass; 0x17,
 * 0x26..0x3F and 0x40+ do not (the original reads past the table above
 * 0x3F - the port's bound returns "no overlay"). */
static const uint8_t _obj_ovl_bits[8] = { 0xFF, 0xFF, 0xFE, 0xFF,
                                          0xFC, 0x00, 0x00, 0x00 };

/* 202F:0x351D (one word per clamped frame): the sprite index 0x2E..0x31 in
 * the low two bits, the x offset (twice SARed, i.e. /4) in the low byte and
 * the y offset (SAR 8) in the high byte. */
static const uint16_t _obj_ovl_word[0x12] = {
    0x04FC, 0x03FC, 0x01FC, 0x0200, 0x04FC, 0x03FC, 0x0100, 0x0200, 0xFE01,
    0xFF01, 0x0101, 0x0001, 0xFE01, 0xFF01, 0x0101, 0x0001, 0x0102, 0x0103,
};


/** @brief 1000:6495's second half: the SELECTED record (`[0x180A]`) draws one
 * extra 0x2E..0x31 sprite, gated by the 0x3541 bit for its frame.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 * @param r the record
 * @param i the record index
 */
static void _obj_render_draw_player_overlay(uint8_t *page, int32_t scrollX,
                                            int32_t scrollY, const uint8_t *r,
                                            uint32_t i)
{
    uint16_t f = rd_le_u16(r + 4u), idx, spr;
    int16_t w;
    int32_t ox, oy;

    if ((int32_t)i != _obj_player) return;
    idx = f >> 3;
    if (idx >= sizeof _obj_ovl_bits) return; // 0x40+: the original reads past
    if (!(_obj_ovl_bits[idx] & (uint8_t)(0x80u >> (f & 7u)))) return;
    if (f > 0x10u) f = (f > 0x22u) ? 0x11u : 0x10u; // 1000:64E1
    w = (int16_t)_obj_ovl_word[f];
    spr = ((uint32_t)w & 3u) + 0x2Eu; // 1000:650F
    /* 0x6523/0x6536: the high byte SARed 8, added to CX (x); 0x6515/0x6521:
     * the low byte twice SARed, added to DX (y) - 0x6625 is CX = x, DX = y. */
    ox = (int32_t)w >> 8;
    oy = (int32_t)(int8_t)(w & 0xFF) >> 2;
    _obj_render_blit_frame(page, spr,
                           (int32_t)(int16_t)rd_le_u16(r + 0u) - scrollX + ox,
                           (int32_t)(int16_t)rd_le_u16(r + 2u) - scrollY + oy);
}


/** @brief 1000:653C draw_object_cell's class half: the class id names both the
 * behaviour class (0x3E5E) and the placed record (0x3F17 + 0x12*(id-1)) that
 * carries the extra sprite.  That record's x/y/frame are overwritten with the
 * carrier-relative position, as the original does - it is a per-frame cache,
 * not a scratch copy.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 * @param rec the record
 */
static void _obj_render_draw_class_overlay(uint8_t *page, int32_t scrollX,
                                           int32_t scrollY, uint8_t *rec)
{
    uint8_t cls = rec[7];
    uint32_t col, row, idx, x, y, frame;
    const uint8_t *e;
    uint8_t *carried;

    if (!cls) return;
    /* 1000:6570: CX = rec[+7]; SUB CX,1; SI = 0x3E5F + CX, i.e. the byte at
     * 0x3E5E + class. */
    col = _obj_cls_col[_obj_core_class_byte_at(cls) & 0x3Fu]; // 1000:657F
    if (!col) return;
    frame = rd_le_u16(rec + 4u);
    /* 1000:65A7: `draw_object_cell` enters the class branch whenever the
     * frame's 0x3570 byte is NON-ZERO and only then masks it to 0x7F - a
     * 0x80 byte means row 0, whose 0x368A entry can still be live. */
    if (_obj_render_overlay_row(frame) == 0u) return;
    row = _obj_render_overlay_row(frame) & 0x7Fu;
    idx = _obj_overlay_idx[3u * row + col]; // 1000:65C2
    if (!idx) return;
    e = _obj_overlay_ent + 4u * idx; // 1000:65D5
    x = rd_le_u16(rec + 0u);
    y = rd_le_u16(rec + 2u);
    if (e[2]) { // 1000:65E4
        if (8u + cls < OBJ_SLOTS) {
            carried = _obj_array + (8u + cls) * OBJ_STRIDE;
            wr_le_u16(carried + 0u,
                      (uint16_t)(x + (uint16_t)(int16_t)(int8_t)e[0]));
            wr_le_u16(carried + 2u,
                      (uint16_t)(y + (uint16_t)(int16_t)(int8_t)e[1]));
            wr_le_u16(carried + 4u, e[2]);
            _obj_render_blit_frame(
                page, e[2], (int32_t)(int16_t)rd_le_u16(carried + 0u) - scrollX,
                (int32_t)(int16_t)rd_le_u16(carried + 2u) - scrollY);
        }
    }
    if (e[3]) // 1000:6605
        _obj_render_blit_frame(page, e[3], (int32_t)(int16_t)x - scrollX,
                               (int32_t)(int16_t)y - scrollY);
}


void object_render_blit_sprite(uint8_t *page, const uint8_t *src, uint32_t rows,
                               uint32_t cols, int32_t x, int32_t y)
{ _blit_sprite(page, src, rows, cols, x, y); }


/** @brief One placed/live record through the object drawer.  `scan` selects the
 * scan_object_rows path (slots 8+): its class slot 0x3E5E + i-8 can mark the
 * record taken, and a +7 == 0 record is drawn straight through
 * select_object_tile (1000:6625) with no 0x3570 gate.  The list passes
 * (1000:6495 -> 1000:653C) gate the base on 0x3570 instead.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 * @param i the record index
 * @param scan non-zero for the scan_object_rows pass
 */
static void _obj_render_draw_record(uint8_t *page, int32_t scrollX,
                                    int32_t scrollY, uint32_t i, int32_t scan)
{
    uint8_t *r = _obj_array + i * OBJ_STRIDE;
    int32_t x = (int32_t)(int16_t)rd_le_u16(r + 0u) - scrollX;
    int32_t y = (int32_t)(int16_t)rd_le_u16(r + 2u) - scrollY;
    uint16_t frame = rd_le_u16(r + 4u);

    /* 1000:653C's own gates.  They only skip the BASE draw: 0x6495 always
     * continues to the player-overlay half below. */
    if (!(r[6] & 0x80u) && // 1000:794C / 1000:6458
        !(scan && (_obj_slot_class[i - 8u] & 0x80u))) {
        /* 1000:6450/1000:6467: the record's class slot (0x3E5E + record-8)
         * skips it when bit 7 is set - the pickup's taken mark (1000:9D47).
         * The carried record is drawn by its carrier's class overlay only. */
        if (!r[7]) {
            /* 1000:653C skips only the EARLY base draw when 0x3570[frame] bit
             * 7 is set; its tail (1000:6610) draws the base at 1000:661C
             * instead.  With no class overlay the record is always drawn
             * exactly once, so the climb frames 0x23..0x25 (0x3570 =
             * 0xC3/0xC4/0xC5) must draw. */
            _obj_render_blit_frame(page, frame, x, y);
        } else {
            if (!(_obj_render_overlay_row(frame) & 0x80u))
                _obj_render_blit_frame(page, frame, x, y);
            _obj_render_draw_class_overlay(page, scrollX, scrollY, r);
            if (_obj_render_overlay_row(frame) & 0x80u)
                _obj_render_blit_frame(page, frame, x, y);
        }
    }
    _obj_render_draw_player_overlay(page, scrollX, scrollY, r, i);
}


/** @brief 1000:C23C partition_entities_by_type: the eight placed slots split by their
 * type word `+8`; 6..8 (the human-stack states - grab 6, push/stand 7,
 * step-off/register 8) go to the 0x651E list drawn **after** the live
 * records, everything else to the 0x650C list drawn **before** them.  The
 * acting record is skipped in the slot walk and appended last in its own
 * class, so it is the last drawn there.
 *
 * @param back the back list
 * @param nb the back-list count
 * @param front the front list
 * @param nf the front-list count
 */
static void _obj_render_partition(uint8_t *back, uint32_t *nb, uint8_t *front,
                                  uint32_t *nf)
{
    uint32_t b = 0, f = 0;

    for (uint16_t i = 0; i < 8u; i++) {
        uint8_t *r = _obj_array + i * OBJ_STRIDE;
        uint16_t t;

        if (r[6] & 0x80u) continue;
        if (i == _obj_entity) continue;
        t = rd_le_u16(r + 8u);
        if (t >= 6u && t <= 8u) front[f++] = (uint8_t)i;
        else back[b++] = (uint8_t)i;
    }
    {
        uint16_t t;

        if (_obj_entity >= OBJ_SLOTS) {
            *nb = b;
            *nf = f;
            return;
        }
        t = rd_le_u16(_obj_array + _obj_entity * OBJ_STRIDE + 8u);
        if (t >= 6u && t <= 8u) front[f++] = (uint8_t)_obj_entity;
        else back[b++] = (uint8_t)_obj_entity;
    }
    *nb = b;
    *nf = f;
}


/** @brief 1000:5D65 object_update_loop: the two handled-object slots ([0x6F92]).
 *
 * A non-free entry draws a 2-px vertical stripe of colour 0x18 from its seed
 * row down to +0xC.  The original paints the page and keeps the covered
 * background at 0x2DB0; the port rebuilds the page each frame and redraws
 * from `_obj_span_top`.
 *
 * 1000:5DD1 grows the stripe up to four rows an odd frame while +0xC/+6
 * advance, stopping when +6 reaches +8; 1000:5D9D retracts it the same way.
 * Either end writes +0x12 = 0xFFFF, and a finished stripe keeps its pixels.
 * The two pixels are planes p and p+1 (p = x & 3), i.e. x and x+1.
 *
 * @param page the page buffer
 * @param scrollX the scroll x
 * @param scrollY the scroll y
 */
static void _obj_render_span_draw(uint8_t *page, int32_t scrollX,
                                  int32_t scrollY)
{
    for (uint16_t i = 0; i < 2u; i++) {
        uint8_t *e = (uint8_t *)_obj_span[i];
        uint32_t state, cur, len, tgt, x, seed;

        if (rd_le_u16(e + 0u) & 0x8000u) continue; // 1000:5D71: free
        wr_le_u16(e + 0xEu, rd_le_u16(e + 0x6u)); // 1000:5D86: +0xE
        state = rd_le_u16(e + 0x12u);
        cur = rd_le_u16(e + 0xCu);
        len = rd_le_u16(e + 0x6u);
        tgt = rd_le_u16(e + 0x8u);
        if (state <= 1u && (_obj_frame & 1u)) {
            uint16_t e13 = rd_le_u16(e + 0xEu);
            uint8_t stop = 0;

            for (uint16_t k = 0; k < 4u && !stop; k++) {
                if (state != 0u) { // growing (1000:5DCE)
                    cur = (uint16_t)(cur + 1u);
                    len = (uint16_t)(len + 1u);
                    if (len == tgt) { // 1000:5DE2
                        state = 0xFFFFu;
                        break;
                    }
                    e13 = (uint16_t)(e13 + 1u);
                    if (e13 == tgt) stop = 1; // 1000:5DED
                } else { // retracting (1000:5D9D)
                    cur = (uint16_t)(cur - 1u);
                    len = (uint16_t)(len - 1u);
                    if ((int16_t)len < 0) { // 1000:5DAE -> 5DBB
                        len = 0u;
                        state = 0xFFFFu;
                        stop = 1;
                    } else {
                        e13 = (uint16_t)(e13 - 1u);
                        if ((int16_t)e13 < 0) stop = 1;
                    }
                }
            }
            wr_le_u16(e + 0xEu, e13);
            wr_le_u16(e + 0xCu, (uint16_t)cur);
            wr_le_u16(e + 0x6u, (uint16_t)len);
            wr_le_u16(e + 0x12u, (uint16_t)state);
        }
        seed = _obj_span_top[i];
        x = rd_le_u16(e + 0xAu);
        for (int16_t row = (int16_t)seed; row < (int16_t)cur; row++) {
            int32_t px = (int32_t)x - scrollX;
            int32_t py = row - scrollY;

            if (py < 0 || py >= (int32_t)VIDEO_PAGE_H_MODE13H) continue;
            if (px >= 0 && px < (int32_t)VIDEO_PAGE_W)
                page[(uint32_t)py * VIDEO_PAGE_W + (uint32_t)px] = 0x18u;
            if (px + 1 >= 0 && px + 1 < (int32_t)VIDEO_PAGE_W)
                page[(uint32_t)py * VIDEO_PAGE_W + (uint32_t)(px + 1)] = 0x18u;
        }
    }
}


void object_render(uint8_t *page, int32_t scrollX, int32_t scrollY)
{
    uint8_t back[9], front[9];
    uint32_t nb = 0, nf = 0;

    if (!page || !_obj_bank) return;
    /* render_objects (1000:5D56) also calls 1000:5D65, the two handled-object
     * slots' stripe pass, before the records. */
    _obj_render_span_draw(page, scrollX, scrollY);

    _obj_render_partition(back, &nb, front, &nf); // 1000:C23C

    for (uint16_t k = 0; k < nb; k++) // 1000:6429
        _obj_render_draw_record(page, scrollX, scrollY, back[k], 0);

    /* scan_object_rows (1000:6433): AX = [0x3E73] - 8, then `DEC AX / JNS`,
     * so the walk covers 0x3F05 .. the record at [0x3E73] INCLUSIVE -
     * `[0x3E73]` counts the top live index, not the number of records
     * (level 1: 9 draws slots 8 and 9). */
    for (uint16_t i = 8u; i <= _obj_live_count && i < OBJ_SLOTS; i++)
        _obj_render_draw_record(page, scrollX, scrollY, i, 1);

    for (uint16_t k = 0; k < nf; k++) // 1000:642E
        _obj_render_draw_record(page, scrollX, scrollY, front[k], 0);
}


void object_render_claim_draw_records(uint8_t *page, int32_t scrollX,
                                      int32_t scrollY)
{
    int32_t cur = object_core_current();

    if (!page || !_obj_bank) return;
    if (_obj_player >= 0 && (uint32_t)_obj_player < OBJ_SLOTS)
        _obj_render_draw_record(page, scrollX, scrollY, (uint32_t)_obj_player,
                                0);
    if (cur >= 0 && (uint32_t)cur < OBJ_SLOTS)
        _obj_render_draw_record(page, scrollX, scrollY, (uint32_t)cur, 0);
}


void object_render_ter_slot_init(void)
{
    /* 1000:4CAB writes ONLY the two +0x10 sentinels; the other 0x12 bytes keep
     * whatever the previous level left (the image starts them at zero, and the
     * 0xAF/0xB0 pair rewrites every field it uses). */
    wr_le_u16(_obj_ter_slot[0] + 0x10u, 0xFFFFu); // 1000:4CC3 [0x64AA]
    wr_le_u16(_obj_ter_slot[1] + 0x10u, 0xFFFFu); // 1000:4CC6 [0x64BE]
}


/* Test seam (HUMANSRE_TEST_PROBES): never part of a release build. */
#if defined(HUMANSRE_TEST_PROBES)
uint8_t *object_render_ter_slot_at(uint32_t i)
{ return i < 2u ? _obj_ter_slot[i] : NULL; }
#endif /* HUMANSRE_TEST_PROBES */


void object_render_publish_ter(const uint8_t *bank, uint32_t size)
{
    _obj_ter_bank = bank;
    _obj_ter_size = size;
}


void object_render_ter(uint8_t *page, int32_t scrollX, int32_t scrollY)
{
    if (!page || !_obj_ter_bank) return;
    for (uint16_t s = 0; s < 2u; s++) {
        uint8_t *e = _obj_ter_slot[s];
        uint32_t index, idx, src, yoff, rows;

        if (rd_le_u16(e + 0x10u) & 0x8000u) continue; // 1000:587F
        index = rd_le_u16(e + 0xCu);
        if (index >= sizeof _obj_ter_index) continue; // D-267
        idx = _obj_ter_index[index];
        if (4u * idx + 4u > _obj_ter_size || 2u * idx + 0x32u > _obj_ter_size)
            continue;
        src = (uint32_t)rd_be_u16(_obj_ter_bank + 4u * idx + 2u) + 0x40u;
        yoff = rd_be_u16(_obj_ter_bank + 2u * idx + 0x20u);
        rows = rd_be_u16(_obj_ter_bank + 2u * idx + 0x30u);
        if (src + rows * 8u > _obj_ter_size) continue;
        object_render_blit_sprite(
            page, _obj_ter_bank + src, rows, 32u,
            (int32_t)(int16_t)rd_le_u16(e + 0u) - 0xC - scrollX,
            (int32_t)(int16_t)rd_le_u16(e + 2u) + (int32_t)yoff - scrollY);
    }
}


void object_render_camera_slots_tick(void)
{
    for (uint16_t s = 0; s < 2u; s++) {
        uint8_t *e = _obj_ter_slot[s];
        int32_t curx = (int16_t)rd_le_u16(e + 0u);
        int32_t cury = (int16_t)rd_le_u16(e + 2u);
        int32_t tx = (int16_t)rd_le_u16(e + 4u);
        int32_t ty = (int16_t)rd_le_u16(e + 6u);
        int32_t t2x = (int16_t)rd_le_u16(e + 8u);
        int32_t t2y = (int16_t)rd_le_u16(e + 0xAu);
        uint16_t dx = rd_le_u16(e + 0xCu);
        uint16_t bp = rd_le_u16(e + 0xEu);
        uint16_t phase = rd_le_u16(e + 0x10u);
        uint16_t speed = 0u; // [0x4236]
        uint16_t mask;
        uint16_t bx, cx;
        int32_t oldx = curx, oldy = cury;

        if (phase & 0x8000u) { // 0xC7F2: disabled -> straight to the tail
            mask = (uint16_t)((rd_le_u16(e + 0x12u) >> 8) |
                              (rd_le_u16(e + 0x12u) << 8));
        } else {
            if (phase == 0u) { // 0xC7F9: zero phase -> the secondary point
                tx = t2x;
                ty = t2y;
            }
            if (curx == tx && cury == ty) {
                phase ^= 0x00FFu; // 0xC861: swap corners next frame
            } else {
                if (cury != ty) { // 0xC817: the y axis
                    if (ty > cury) cury++;
                    else cury--;
                }
                if (curx != tx) { // 0xC842: the x axis (post-swap)
                    if (tx > curx) {
                        curx++;
                        speed = 6u; // 0xC859
                    } else {
                        curx--;
                    }
                }
                if ((int16_t)(bp = (uint16_t)(bp - 1u)) < 0) { // 0xC875
                    bp = 5u;
                    dx = (uint16_t)(dx + 1u);
                    if (dx < speed) {
                        dx = speed;
                    } else if (dx >= (uint16_t)(speed + 6u)) {
                        dx = speed;
                    }
                }
            }
            mask = (uint16_t)((rd_le_u16(e + 0x12u) >> 8) |
                              (rd_le_u16(e + 0x12u) << 8));
        }

        wr_le_u16(e + 0x10u, phase);
        wr_le_u16(e + 0xEu, bp);
        wr_le_u16(e + 0xCu, dx);
        wr_le_u16(e + 0u, (uint16_t)curx);
        wr_le_u16(e + 2u, (uint16_t)cury);

        bx = (uint16_t)(curx - oldx); // 0xC8E0: the per-frame delta
        cx = (uint16_t)(cury - oldy);
        if ((mask & 0xFFu) == 0u) continue;
        for (uint16_t i = 0; i < 8u; i++) {
            uint8_t *r = _obj_array + i * OBJ_STRIDE;

            if (!(mask & (uint16_t)(0x80u >> i))) continue;
            wr_le_u16(r + 2u, (uint16_t)(rd_le_u16(r + 2u) + cx));
            wr_le_u16(r + 0u, (uint16_t)(rd_le_u16(r + 0u) + bx));
        }
        for (uint16_t i = 0; i < 8u; i++) {
            uint8_t *g = (uint8_t *)_obj_register[i];
            uint8_t index = g[5];
            uint16_t bit;

            if (index & 0x80u) continue;
            bit = (uint16_t)(1u << ((7u - index) & 0x1Fu));
            if (!(mask & bit)) continue;
            wr_le_u16(g + 0u, (uint16_t)(rd_le_u16(g + 0u) + bx));
            wr_le_u16(g + 2u, (uint16_t)(rd_le_u16(g + 2u) + cx));
        }
    }
}


/**
 * @brief Copy one four-byte shade group into its 16-byte row.
 *
 * @param sp the span entry
 * @param pass the pass index
 * @param row the row
 * @param out the output buffer
 * @param s the source cursor (in/out)
 */
static void _shade_row(const ObjSprite *sp, uint32_t pass, uint32_t row,
                       uint8_t *out, uint16_t *s)
{
    for (uint16_t j = 0; j < 4u; j++) {
        if ((sp->off + *s) < _obj_bank_size)
            out[row * 16u + pass + 4u * j] = _obj_bank[sp->off + *s];
        (*s)++;
    }
}


/* Build the 4-pass shade mask for one sprite frame. */
void _obj_render_build_shade(uint32_t frame, uint8_t *out)
{
    ObjSprite sp;
    uint16_t s = 0;

    memset(out, 0, 256u);
    if (frame >= OBJ_FRAME_SLOTS) return;
    sp = object_sprite_at(frame);
    for (uint16_t pass = 0; pass < 4u; pass++)
        for (uint16_t row = 0; row < 16u; row++) {
            if (row < sp.ybias || row >= sp.ybias + sp.rows) continue;
            _shade_row(&sp, pass, row, out, &s);
        }
}
