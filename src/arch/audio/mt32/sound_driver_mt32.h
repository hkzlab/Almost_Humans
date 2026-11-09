/* arch/audio/mt32/sound_driver_mt32.h - the MT-32 module's one port-side hook.
 *
 * SoundDriverModule::create() has no room for the ROM directory, and it is
 * not part of the INT 60h conversation, so the port hands the path over
 * before the driver install (audio(mt32)/sound_driver_mt32.c).
 */
#ifndef HUMANSRE_SOUND_DRIVER_MT32_H
#define HUMANSRE_SOUND_DRIVER_MT32_H

/**
 * @brief Select the directory the ROM images are read from before create().
 *
 * Must be called before the module's create(); the strings are not retained.
 *
 * @param path a directory holding the MT-32/CM-32L control and PCM ROMs
 */
void sound_driver_mt32_set_roms(const char *path);

#endif /* HUMANSRE_SOUND_DRIVER_MT32_H */
