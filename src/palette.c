/* palette.c — fades and DAC (game_structure.md §1 subsystem 4, §6).
 *
 * The original's machine, ported 1:1: a 256-entry 6-bit _working palette
 * (202F:0x4BBC) refilled by palette_load_6bit (1000:8d24) from each loaded
 * image — the RAM copy only, it never touches the DAC — and the DAC fade
 * state (_ramp 202F:0x4EBC, control 0x48B0..0x48B4) stepped once per tick by
 * palette_fade_step (1000:8bfd) from the ISR body.  palette_build_fade
 * (1000:8bac) reads the CURRENT DAC into the _ramp and stores per-component
 * deltas (target-current)<<8/div; each tick adds one delta, clamps at the
 * target and clears the active flag once every component reached it. */
#include "palette.h"
#include "osystem.h"

static const OSystem *_pal_osys;
static uint8_t _working[768];   /* 202F:0x4BBC — the _working palette       */
static uint8_t _dac[768];       /* the DAC's 6-bit shadow (8bac's IN 3C9h) */

/* the fade state: 202F:0x48B0/48B2/48B6/48B8 + the 0x4EBC _ramp          */
static uint32_t _fade_first;    /* [0x48B8]: the DAC's first entry    */
static uint32_t _fade_comps;    /* [0x48B0]: components (entries * 3) */
static const uint8_t *_fade_target;   /* [0x48B6]: the target palette bytes */
static int32_t _fade_active;   /* [0x48B4]                           */
static uint16_t _ramp[768][2]; /** [0x4EBC]: {cur<<8, delta<<8}       */

/**
 * @brief Bind the palette module to the backend.
 *
 * @param osys the backend interface
 * @return 0 on success
 */
int32_t palette_init(const OSystem *osys)
{
    _pal_osys = osys;
    return 0;
}

/**
 * @brief palette_load_6bit (1000:8d24): the _working palette := 768 6-bit bytes.
 *
 * @param p6 the 6-bit palette
 */
void palette_load_working(const uint8_t *p6)
{
    for (uint32_t i = 0; i < 768u; i++) _working[i] = p6[i] & 0x3Fu;
}

/**
 * @brief Accessor: the working palette.
 *
 * @return The working palette.
 */
const uint8_t *palette_working(void) { return _working; }

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
void palette_seal_triples(uint8_t *p6, uint32_t first, uint32_t entries)
{
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t at = (first + i) * 3u;
        uint32_t r = p6[at], g = p6[at + 1u], b = p6[at + 2u];
        uint32_t v = (77u * r + 151u * g + 28u * b) >> 9;

        p6[at] = p6[at + 1u] = p6[at + 2u] = (uint8_t)v;
    }
}

/** @brief set_palette_dac (1000:8CEE) route: an immediate DAC write that keeps the
 * shadow honest for the next fade's current-colour read.
 *
 * @param p6 the 6-bit palette
 * @param first the first entry
 * @param n the count
 */
void palette_set_dac(const uint8_t *p6, uint32_t first, uint32_t n)
{
    for (uint32_t i = 0; i < n * 3u; i++) _dac[first * 3u + i] = p6[i] & 0x3Fu;
    _pal_osys->videoSetPalette(p6, first, n);
}

/** @brief palette_build_fade (1000:8bac): fade DAC entries first..first+entries-1
 * toward `target` (the 6-bit palette whose triple for entry `first` is
 * target[0]) in ~div ticks.
 *
 * @param first the first entry
 * @param entries the entry count
 * @param div the divisor
 * @param target the target value
 */
void palette_build_fade(uint32_t first, uint32_t entries, uint32_t div,
                        const uint8_t *target)
{
    _fade_first = first;
    _fade_comps = entries * 3u;
    _fade_target = target;
    for (uint32_t c = 0; c < _fade_comps; c++) {
        uint32_t cur = _dac[first * 3u + c];
        uint32_t tgt = target[c];
        _ramp[c][0] = (uint16_t)(cur << 8);
        _ramp[c][1] =
            (uint16_t)((((int32_t)tgt - (int32_t)cur) * 256) / (int32_t)div);
    }
    _fade_active = 1;
}

/**
 * @brief palette_fade_active (1000:8b9a)
 *
 * @return 1 while a fade runs, 0 otherwise
 */
int32_t palette_fade_active(void) { return _fade_active; }

/** @brief the DAC shadow, read-only: the fade-ladder harness compares the port's
 * per-tick DAC progression against the original's live IN 3C9h reads
 * (2026-10-30: window 0x80+n <- buffer entry n, verified in the hardware).
 * No engine logic lives here.
 *
 * @return the pointer, or NULL when unavailable
 */
const uint8_t *palette_dac_shadow(void) { return _dac; }
/**
 * @brief Accessor: the fade's target palette.
 *
 * @return The fade's target palette.
 */
const uint8_t *palette_fade_target(void) { return _fade_target; }
/**
 * @brief Accessor: the fade's first entry.
 *
 * @return The fade's first entry.
 */
uint32_t palette_fade_first(void) { return _fade_first; }
/**
 * @brief Accessor: the fade's component count.
 *
 * @return The fade's component count.
 */
uint32_t palette_fade_comps(void) { return _fade_comps; }

/** @brief palette_fade_step (1000:8bfd): one step per tick (the ISR body's palette
 * work, game_main's game_tick).
 */
void palette_tick(void)
{
    if (!_fade_active) return;
    uint32_t done = 0;
    for (uint32_t c = 0; c < _fade_comps; c++) {
        uint16_t cur = _ramp[c][0];
        uint8_t t = _fade_target[c];
        uint8_t v = (uint8_t)(cur >> 8);
        if (v != t) {
            cur = (uint16_t)(cur + _ramp[c][1]);
            _ramp[c][0] = cur;
            v = (uint8_t)(cur >> 8);
            if (_ramp[c][1] & 0x8000u) {           /* negative delta        */
                if (v < t) {
                    v = t;
                    _ramp[c][0] =
                        (uint16_t)(((uint32_t)t << 8) | (cur & 0xffu));
                }
            } else {
                if (v > t) {
                    v = t;
                    _ramp[c][0] =
                        (uint16_t)(((uint32_t)t << 8) | (cur & 0xffu));
                }
            }
        } else {
            done++;
        }
        _dac[_fade_first * 3u + c] = v;
    }
    if (done == _fade_comps) _fade_active = 0;
    _pal_osys->videoSetPalette(_dac + _fade_first * 3u, _fade_first,
                               _fade_comps / 3u);
}
