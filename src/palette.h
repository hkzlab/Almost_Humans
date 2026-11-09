/* palette.h - the interface of src/palette.c: fades/DAC (subsystem 4) */

#ifndef HUMANSRE_PALETTE_H
#define HUMANSRE_PALETTE_H

#include "osystem.h"

/**
 * @brief Bind the palette module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t palette_init(const OSystem *osys);

/** @brief palette_fade_step (1000:8bfd): one step per tick (the ISR body's palette
 * work, game_main's game_tick).
 */
void palette_tick(void);

/**
 * @brief palette_load_6bit (1000:8d24): the 202F:0x4BBC working palette :=
 * 768 6-bit bytes.
 *
 * The RAM copy only - the DAC moves through fades or palette_set_dac.
 *
 * @param p6 the 6-bit palette
 */
void palette_load_working(const uint8_t *p6);

/**
 * @brief Accessor: the working palette (the 0x4BBC buffer).
 *
 * @return The working palette.
 */
const uint8_t *palette_working(void);

/** @brief seal_triples (1000:43C2) + hash_three_bytes (1000:43D5): turn `entries`
 * triples of the given palette buffer into grey.  The hash is
 * (77R + 151G + 28B) >> 8 and the tail's SHR CH,1 halves it, so the grey
 * value is (77R + 151G + 28B) >> 9 (0..31 on 6-bit components); all three
 * components become it.  draw_level_text (0x33E2) seals the level palette's
 * first 0x80 entries; the attract's decode_page_in_place (0x15F5) uses the
 * same value per page byte.
 *
 * @param p6 the 6-bit palette
 * @param first the first entry
 * @param entries the entry count
 */
void palette_seal_triples(uint8_t *p6, uint32_t first, uint32_t entries);

/** @brief set_palette_dac (1000:8CEE) route: an immediate DAC write that keeps the
 * shadow honest for the next fade's current-colour read.
 *
 * @param p6 the 6-bit palette
 * @param first the first entry
 * @param n the count
 */
void palette_set_dac(const uint8_t *p6, uint32_t first, uint32_t n);

/** @brief palette_build_fade (1000:8bac): fade DAC entries
 * first..first+entries-1 toward `target` (its triple for `first` at
 * target[0]) in ~div ticks; poll with palette_fade_active.
 *
 * @param first the first entry
 * @param entries the entry count
 * @param div the divisor
 * @param target the target value
 */
void palette_build_fade(uint16_t first, uint16_t entries, uint16_t div,
                        const uint8_t *target);

/**
 * @brief palette_fade_active (1000:8b9a)
 *
 * @return 1 while a fade runs, 0 otherwise
 */
uint8_t palette_fade_active(void);

/** @brief the DAC shadow, read-only: the fade-ladder harness compares the port's
 * per-tick DAC progression against the original's live IN 3C9h reads
 * (2026-10-30: window 0x80+n <- buffer entry n, verified in the hardware).
 * No engine logic lives here.
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *palette_dac_shadow(void);

/**
 * @brief Accessor: the fade's target palette.
 *
 * @return The fade's target palette.
 */
const uint8_t *palette_fade_target(void);

/**
 * @brief Accessor: the fade's first entry.
 *
 * @return The fade's first entry.
 */
uint16_t palette_fade_first(void);

/**
 * @brief Accessor: the fade's component count.
 *
 * @return The fade's component count.
 */
uint16_t palette_fade_comps(void);

#endif /* HUMANSRE_PALETTE_H */
