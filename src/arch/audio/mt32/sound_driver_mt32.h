/* The MT-32 module's exported symbol and its one port-side hook: the ROM
 * directory, handed over before create().  The path is not retained; not
 * part of INT 60h.  Deviations: D-255, D-256.
 */
#ifndef HUMANSRE_SOUND_DRIVER_MT32_H
#define HUMANSRE_SOUND_DRIVER_MT32_H

#include "audio/sound_driver.h"

extern const SoundDriverModule g_sound_driver_mt32;

/**
 * @brief Select the directory the ROM images are read from before create().
 *
 * Must be called before the module's create(); the strings are not retained.
 *
 * @param path a directory holding the MT-32/CM-32L control and PCM ROMs
 */
void sound_driver_mt32_set_roms(const char *path);

#endif /* HUMANSRE_SOUND_DRIVER_MT32_H */
