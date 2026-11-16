/* MT32.DRV's replacement: the sequencer over mt32emu.
 *
 * Image-relative offsets (the driver is a CD resource):
 *   install   0x00DE   cmd 0     0x0178   cmd 1     0x0209   cmd 2  0x0792
 *   cmd 5     0x02A7   cmd 6     0x03FA   cmd 0x0A  0x01CD   cmd 0x0C 0x0236
 *   cmd 0x0E  0x01BB
 *
 * cmd 0x0A is master volume (DT1 10 00 16); cmd 2 is the only clock the game
 * uses.  cmd 1 re-runs the install reset and the voice reset; no game path
 * sends it.  MIDI goes through mt32emu's byte-stream parser; the ROM images
 * are read here.  The tune image is copied, not aliased.  The one port-side
 * hook below hands over the ROM directory before create(); the path is not
 * retained and it is not part of INT 60h.
 * Deviations: D-255 (bounded reads), D-256 (no MPU-401/INT 8/envelope).
 */
#ifndef HUMANSRE_SOUND_DRIVER_MT32_H
#define HUMANSRE_SOUND_DRIVER_MT32_H

#include "audio/sound_driver.h"
#include "fs/fs.h"

extern const SoundDriverModule g_sound_driver_mt32;

/**
 * @brief Select the directory the ROM images are read from before create().
 *
 * Must be called before the module's create().  The directory is read through
 * the filesystem module (the driver never opens a path itself); the path is
 * copied, the module pointer is not owned.
 *
 * @param fs the filesystem module that lists and reads the images
 * @param path a directory holding the MT-32/CM-32L control and PCM ROMs
 */
void sound_driver_mt32_set_roms(const FSModule *fs, const char *path);

#endif /* HUMANSRE_SOUND_DRIVER_MT32_H */
