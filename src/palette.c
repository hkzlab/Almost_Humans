/* palette.c - fades and DAC.
 *
 * The original's machine, ported 1:1: a 256-entry 6-bit _working palette
 * (202F:0x4BBC) refilled by palette_load_6bit (1000:8d24) from each loaded
 * image - the RAM copy only, it never touches the DAC - and the DAC fade
 * state (_ramp 202F:0x4EBC, control 0x48B0..0x48B4) stepped once per tick by
 * palette_fade_step (1000:8bfd) from the ISR body.  palette_build_fade
 * (1000:8bac) reads the CURRENT DAC into the _ramp and stores per-component
 * deltas (target-current)<<8/div; each tick adds one delta, clamps at the
 * target and clears the active flag once every component reached it. */
#include "palette.h"
#include "osystem.h"
#include "width.h"

static const OSystem *_pal_osys;
static uint8_t _working[768];   // 202F:0x4BBC - the _working palette
static uint8_t _dac[768];       // the DAC's 6-bit shadow (8bac's IN 3C9h)

/* the fade state: 202F:0x48B0/48B2/48B6/48B8 + the 0x4EBC _ramp          */
static uint16_t _fade_first;    // [0x48B8]: the DAC's first entry
static uint16_t _fade_comps;    // [0x48B0]: components (entries * 3)
static const uint8_t *_fade_target;   // [0x48B6]: the target palette bytes
static uint8_t _fade_active;   // [0x48B4]
static uint16_t _ramp[768][2]; // [0x4EBC]: {cur<<8, delta<<8}

int32_t palette_init(const OSystem *osys)
{
    _pal_osys = osys;
    return 0;
}

void palette_load_working(const uint8_t *p6)
{
    for (uint16_t i = 0; i < 768u; i++) _working[i] = p6[i] & 0x3Fu;
}

const uint8_t *palette_working(void) { return _working; }

void palette_seal_triples(uint8_t *p6, uint32_t first, uint32_t entries)
{
    for (uint16_t i = 0; i < entries; i++) {
        uint16_t at = (uint16_t)((first + i) * 3u);
        uint8_t r = p6[at], g = p6[at + 1u], b = p6[at + 2u];
        uint8_t v = (77u * r + 151u * g + 28u * b) >> 9;

        p6[at] = p6[at + 1u] = p6[at + 2u] = v;
    }
}

void palette_set_dac(const uint8_t *p6, uint32_t first, uint32_t n)
{
    for (uint16_t i = 0; i < n * 3u; i++) _dac[first * 3u + i] = p6[i] & 0x3Fu;
    _pal_osys->videoSetPalette(p6, first, n);
}

void palette_build_fade(uint16_t first, uint16_t entries, uint16_t div,
                        const uint8_t *target)
{
    _fade_first = first;
    _fade_comps = u16(entries * 3u);
    _fade_target = target;
    for (uint16_t c = 0; c < _fade_comps; c++) {
        uint32_t cur = _dac[first * 3u + c];
        uint32_t tgt = target[c];
        _ramp[c][0] = (uint16_t)(cur << 8);
        _ramp[c][1] =
            (uint16_t)((((int32_t)tgt - (int32_t)cur) * 256) / (int32_t)div);
    }
    _fade_active = 1;
}

uint8_t palette_fade_active(void) { return _fade_active; }

const uint8_t *palette_dac_shadow(void) { return _dac; }
const uint8_t *palette_fade_target(void) { return _fade_target; }
uint16_t palette_fade_first(void) { return _fade_first; }
uint16_t palette_fade_comps(void) { return _fade_comps; }

void palette_tick(void)
{
    if (!_fade_active) return;
    uint16_t done = 0;
    for (uint16_t c = 0; c < _fade_comps; c++) {
        uint16_t cur = _ramp[c][0];
        uint8_t t = _fade_target[c];
        uint8_t v = (uint8_t)(cur >> 8);
        if (v != t) {
            cur = (uint16_t)(cur + _ramp[c][1]);
            _ramp[c][0] = cur;
            v = (uint8_t)(cur >> 8);
            if (_ramp[c][1] & 0x8000u) {           // negative delta
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
