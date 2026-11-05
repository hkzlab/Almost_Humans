/* palette.h — the interface of src/palette.c: fades/DAC (subsystem 4, §6) */

#ifndef HUMANSRE_PALETTE_H
#define HUMANSRE_PALETTE_H

#include "osystem.h"

int32_t palette_init(const OSystem *osys);
void palette_tick(void);   /* palette_fade_step 1000:8bfd, once per tick  */
/* palette_load_6bit (1000:8d24): the 202F:0x4BBC working palette := 768
 * 6-bit bytes.  The RAM copy only — the DAC moves through fades or
 * palette_set_dac. */
void palette_load_working(const uint8_t *p6);
const uint8_t *palette_working(void);   /* the 0x4BBC buffer              */
/* seal_triples/hash_three_bytes (1000:43C2/43D5): grey the given palette
 * entries in place, value (77R+151G+28B) >> 9. */
void palette_seal_triples(uint8_t *p6, uint32_t first, uint32_t entries);
/* set_palette_dac (1000:8CEE): an immediate DAC write (the DAC shadow is
 * kept for the next fade's current-colour read). */
void palette_set_dac(const uint8_t *p6, uint32_t first, uint32_t n);
/* palette_build_fade (1000:8bac): fade DAC entries first..first+entries-1
 * toward `target` (its triple for `first` at target[0]) in ~div ticks;
 * poll with palette_fade_active. */
void palette_build_fade(uint32_t first, uint32_t entries, uint32_t div,
                        const uint8_t *target);
int32_t palette_fade_active(void);       /* palette_fade_active 1000:8b9a  */
const uint8_t *palette_dac_shadow(void); /* the DAC shadow (read-only;
                                        * the off-line fade-ladder harness) */
const uint8_t *palette_fade_target(void); /* the active fade's target buffer */
uint32_t palette_fade_first(void); /* the active fade's first index   */
uint32_t palette_fade_comps(void); /* the active fade's component #   */

#endif /* HUMANSRE_PALETTE_H */
