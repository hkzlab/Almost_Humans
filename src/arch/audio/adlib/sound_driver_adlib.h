/* ADLIB.DRV's replacement: the sequencer over a vendored Nuked-OPL3-fast.
 *
 * Image-relative offsets (the driver is a CD resource):
 *   install   1E95:00DD   cmd 0     1E95:0140   cmd 2     1E95:05CA
 *   cmd 5     1E95:032C   cmd 6     1E95:048F   cmd 0x0A  1E95:01DD
 *   cmd 0x0C  1E95:02AA   cmd 0x0E  1E95:01C0
 *
 * cmd 0x0A is master volume ((0xA3*CL)>>8 at CS:0x1556); the register stream
 * goes to 0x388/0x389 through adlib_write_opl_reg (1E95:0E23).  The tune
 * image is copied; cmd 5 returns the record index.  Only the audio backend
 * and the tests name the module: the core drives it through SoundDriverModule,
 * selected at run time.
 * Deviations: D-249 (Nuked-OPL3-fast, not the YM3812), D-261 (bounded reads).
 */
#ifndef HUMANSRE_SOUND_DRIVER_ADLIB_H
#define HUMANSRE_SOUND_DRIVER_ADLIB_H

#include "audio/sound_driver.h"

extern const SoundDriverModule g_sound_driver_adlib;

#endif /* HUMANSRE_SOUND_DRIVER_ADLIB_H */
