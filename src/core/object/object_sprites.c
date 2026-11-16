#include "object_internal.h"


/* The object frame table: what each of the 286 drawable frames looks like.
 *
 * A record in the level stores a 16-bit frame number; this table turns it into
 * a sprite (object_sprites.h has the word layout).  It is big because the game
 * has 262 used frames, each a hand-drawn bitmap in a sprite bank: no rule
 * computes a frame from its number, only the original's six bank manifests do.
 * The original reads those at boot (apply_relocations, 1000:569E); the port
 * would have to carry them (3192 bytes) to build the same table at run time,
 * which is more than this 1144-byte table, so it is precomputed by
 * tooling/sprite_frames/sprite_frames.py.  Do not hand-edit it.
 *
 * Deviation: D-265 (the original builds this table at boot; the port
 * precomputes it).
 */
static const uint32_t _obj_sprite_words[OBJ_FRAME_SLOTS] = {
    /* 000 */ 0x10303080, 0x10303200, 0x10AE3380, 0x10AE34F0,
    /* 004 */ 0x10303660, 0x103037E0, 0x10AE3960, 0x10AE3AD0,
    /* 008 */ 0x10303C40, 0x10303DC0, 0x10AE3F40, 0x10AE40B0,
    /* 00C */ 0x10304220, 0x103043A0, 0x10AE4520, 0x10AE4690,
    /* 010 */ 0x10304800, 0x1C004980, 0x10304980, 0x10304B00,
    /* 014 */ 0x10304C80, 0x10304E00, 0x10304F80, 0x111A0000,
    /* 018 */ 0x1C005100, 0x1C005100, 0x1C005100, 0x1C005100,
    /* 01C */ 0x1C005100, 0x1000D3F0, 0x1000D3F0, 0x1800D3F0,
    /* 020 */ 0x10305100, 0x102000D0, 0x102001D0, 0x10305280,
    /* 024 */ 0x102E5400, 0x10305570, 0x00000000, 0x00000000,
    /* 028 */ 0x2792D3F0, 0x249ED510, 0x2324D6F0, 0x2228D930,
    /* 02C */ 0x212CDBB0, 0x20ACDE70, 0x101C02D0, 0x101C03B0,
    /* 030 */ 0x101C0490, 0x101C0570, 0x00000000, 0x00000000,
    /* 034 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 038 */ 0x10200650, 0x10200750, 0x10200850, 0x10200950,
    /* 03C */ 0x2040E130, 0x20BEE530, 0x2040E910, 0x20BEED10,
    /* 040 */ 0x3000F0F0, 0x2040F0F0, 0x20BEF4F0, 0x2040F8D0,
    /* 044 */ 0x20BEFCD0, 0x204100B0, 0x20BF04B0, 0x30010890,
    /* 048 */ 0x20410890, 0x20BF0C90, 0x20411070, 0x20411470,
    /* 04C */ 0x20411870, 0x20411C70, 0x20412070, 0x20412470,
    /* 050 */ 0x20412870, 0x20412C70, 0x103056F0, 0x102E5870,
    /* 054 */ 0x103059E0, 0x10305B60, 0x10AE5CE0, 0x10305E50,
    /* 058 */ 0x10200A50, 0x10200B50, 0x101C0C50, 0x120E0D30,
    /* 05C */ 0x128A0DA0, 0x11160DF0, 0x10200EA0, 0x10200FA0,
    /* 060 */ 0x102010A0, 0x102011A0, 0x111C12A0, 0x128E1380,
    /* 064 */ 0x130A13F0, 0x11961440, 0x102014F0, 0x102015F0,
    /* 068 */ 0x168A5FD0, 0x168A6020, 0x160A6070, 0x170A60C0,
    /* 06C */ 0x168C6110, 0x170A6170, 0x178A61C0, 0x178A6210,
    /* 070 */ 0x168C6260, 0x168A62C0, 0x160A6310, 0x170A6360,
    /* 074 */ 0x170A63B0, 0x170A6400, 0x170A6450, 0x178864A0,
    /* 078 */ 0x178864E0, 0x100C6520, 0x100A6580, 0x108A65D0,
    /* 07C */ 0x100C6620, 0x100A6680, 0x108A66D0, 0x10306720,
    /* 080 */ 0x103068A0, 0x10306A20, 0x102016F0, 0x109E17F0,
    /* 084 */ 0x102018E0, 0x109E19E0, 0x10201AD0, 0x109E1BD0,
    /* 088 */ 0x10201CC0, 0x109E1DC0, 0x10201EB0, 0x10201FB0,
    /* 08C */ 0x10306BA0, 0x10306D20, 0x10306EA0, 0x1C007020,
    /* 090 */ 0x1C007020, 0x10307020, 0x103071A0, 0x10307320,
    /* 094 */ 0x1C0074A0, 0x1C0074A0, 0x150C20B0, 0x109A2110,
    /* 098 */ 0x109E21E0, 0x109E22D0, 0x109C23C0, 0x109824A0,
    /* 09C */ 0x12182560, 0x119A2620, 0x129626F0, 0x180027A0,
    /* 0A0 */ 0x180027A0, 0x138E27A0, 0x11942810, 0x101C28B0,
    /* 0A4 */ 0x101C2990, 0x11942A70, 0x138E2B10, 0x18002B80,
    /* 0A8 */ 0x18002B80, 0x12142B80, 0x109A2C20, 0x103074A0,
    /* 0AC */ 0x10307620, 0x103077A0, 0x10307920, 0x14207AA0,
    /* 0B0 */ 0x14207BA0, 0x10AE7CA0, 0x112C7E10, 0x10AE7F70,
    /* 0B4 */ 0x149E80E0, 0x159A81D0, 0x159A82A0, 0x00000000,
    /* 0B8 */ 0x00000000, 0x00000000, 0x00000000, 0x10AE8370,
    /* 0BC */ 0x103084E0, 0x102E8660, 0x10AE87D0, 0x10308940,
    /* 0C0 */ 0x102E8AC0, 0x00000000, 0x00000000, 0x00000000,
    /* 0C4 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0C8 */ 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    /* 0CC */ 0x00000000, 0x12962CF0, 0x12962DA0, 0x14102E50,
    /* 0D0 */ 0x14102ED0, 0x158A2F50, 0x158A2FA0, 0x13922FF0,
    /* 0D4 */ 0x10308C30, 0x10AE8DB0, 0x10308F20, 0x103090A0,
    /* 0D8 */ 0x10309220, 0x10AE93A0, 0x10309510, 0x10309690,
    /* 0DC */ 0x10AE9810, 0x10AE9980, 0x10309AF0, 0x10AE9C70,
    /* 0E0 */ 0x10309DE0, 0x10AE9F60, 0x1030A0D0, 0x1030A250,
    /* 0E4 */ 0x1030A3D0, 0x10AEA550, 0x1030A6C0, 0x1030A840,
    /* 0E8 */ 0x10AEA9C0, 0x10AEAB30, 0x1030ACA0, 0x10AEAE20,
    /* 0EC */ 0x1708AF90, 0x1788AFD0, 0x1688B010, 0x1608B050,
    /* 0F0 */ 0x170AB090, 0x1708B0E0, 0x1688B120, 0x1608B160,
    /* 0F4 */ 0x1108B1A0, 0x1708B1E0, 0x1788B220, 0x1688B260,
    /* 0F8 */ 0x1608B2A0, 0x170AB2E0, 0x1708B330, 0x1688B370,
    /* 0FC */ 0x1608B3B0, 0x1108B3F0, 0x24313070, 0x24313370,
    /* 100 */ 0x252D3670, 0x252D3930, 0x22B73BF0, 0x1030B430,
    /* 104 */ 0x1030B5B0, 0x1030B730, 0x1030B8B0, 0x1030BA30,
    /* 108 */ 0x1030BBB0, 0x1030BD30, 0x12A6BEB0, 0x1696BFE0,
    /* 10C */ 0x151CC090, 0x151CC170, 0x1030C250, 0x1030C3D0,
    /* 110 */ 0x1030C550, 0x1030C3D0, 0x1030C6D0, 0x1030C850,
    /* 114 */ 0x1030C9D0, 0x1030C850, 0x10AECB50, 0x10AECCC0,
    /* 118 */ 0x10AECE30, 0x10AECCC0, 0x10AECFA0, 0x10AED110,
    /* 11C */ 0x10AED280, 0x10AED110,
};


/** @brief Decode one frame slot; a frame past the table decodes as blank.
 *
 * @param frame the frame index
 * @return the frame's sprite descriptor
 */
ObjSprite object_sprite_at(uint32_t frame)
{
    ObjSprite sp = { 0, 0, 0, 0 };
    uint32_t w;

    if (frame >= OBJ_FRAME_SLOTS) return sp;
    w = _obj_sprite_words[frame];
    sp.off = w & 0x1FFFFu;
    sp.rows = (uint8_t)((w >> 17) & 0x3Fu);
    sp.ybias = (uint8_t)((w >> 23) & 0x1Fu);
    sp.mode = (uint8_t)((w >> 28) & 0x3u);
    return sp;
}
