#include "map/map.h"
#include "map/map_internal.h"

#include "game_strings.h"
#include "hiscore.h"
#include "textpage.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void map_draw_list(uint8_t *page)
{
    uint16_t row = 0u;

    if (!page) return;
    memset(page, 0, MAP_PAGE); // 1000:0577
    for (uint16_t i = 0; i < hiscore_count(); i++) {
        char buf[24];

        snprintf(buf, sizeof buf, "%u", i + 1u);
        textpage_draw_line_mask_at(page, buf, 0x20u, row, 0x0Fu);
        textpage_draw_line_mask_at(page, hiscore_name(i), 0x82u, row, 0x0Fu);
        snprintf(buf, sizeof buf, "%" PRIu32, hiscore_score(i));
        textpage_draw_line_mask_at(page, buf, 0x102u, row, 0x0Fu);
        row += 0x14u;
    }
}

void map_draw_list_header(uint8_t *page)
{
    if (!page) return;
    for (uint16_t i = 0; i < 5u; i++) {
        uint8_t y = (uint8_t)(0x2Du + 0x14u * i);

        map_draw_rect(page, 0x12u, y, 0x1Cu, 0x0Fu); // 1000:0B5F
        map_draw_rect(page, 0x32u, y, 0xA0u, 0x0Fu);
        map_draw_rect(page, 0xD6u, y, 0x58u, 0x0Fu);
    }
    textpage_draw_line_mask_at(page, game_string(GAME_STRING_MAP_TOP_TEN),
                               0xA0u, 0x14u, 0x20u);
}

void map_draw_congrats(uint8_t *page)
{
    for (uint16_t i = 0; i < 7u; i++)
        textpage_draw_line_mask_at(
            page,
            game_string((enum GameString)(GAME_STRING_MAP_CONGRATS_0 + i)),
            0xA0u, 0x19u + 0x10u * i, 0x20u);
}

void map_draw_rect(uint8_t *page, uint32_t x, uint32_t y, uint32_t w,
                   uint32_t h)
{
    if (!page) return;
    for (uint16_t i = 0; i < w; i++) {
        if (y < VIDEO_PAGE_H_MODE13H && x + i < VIDEO_PAGE_W)
            page[y * VIDEO_PAGE_W + x + i] |= 0x40u;
        if (y + h < VIDEO_PAGE_H_MODE13H && x + i < VIDEO_PAGE_W)
            page[(y + h) * VIDEO_PAGE_W + x + i] |= 0x50u;
    }
    for (uint16_t i = 0; i < h; i++) {
        if (y + i < VIDEO_PAGE_H_MODE13H && x < VIDEO_PAGE_W)
            page[(y + i) * VIDEO_PAGE_W + x] |= 0x50u;
        if (y + i < VIDEO_PAGE_H_MODE13H && x + w < VIDEO_PAGE_W)
            page[(y + i) * VIDEO_PAGE_W + x + w] |= 0x40u;
    }
}