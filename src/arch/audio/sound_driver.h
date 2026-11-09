/* arch/audio/sound_driver.h - the common resident synth-driver interface.
 *
 * The game never touches a synth: it loads one of three resident drivers out
 * of the resource archive (ADLIB.DRV / MT32.DRV / BEEPER.DRV, chosen by the
 * launcher's [0x5AFE] sound_driver_id) and talks to it over INT 60h.
 * This header is that same conversation: every function below is one INT 60h
 * command the game sends, and the module behind it is the driver image's
 * replacement.
 *
 *   game wrapper (verified)              INT 60h        this interface
 *   driver_reset 1000:84DE, entry AX=0x60  install       create(rate)
 *   driver_reset 1000:84DE, AX=0x0A/CL=0   cmd 0x0A      setTempo(cl)
 *   driver_reset 1000:84DE, AX=0x0E        cmd 0x0E      commandBlock()
 *   play_tune    1000:8437,  AX=0x0C        cmd 0x0C      loadTune(image, n)
 *   music_resume 1000:83F7 / driver_call 1000:83B7
 *                                          cmd 5         startEvent(id)
 *   music_stop   1000:8389                 cmd 6         releaseEvent(voice)
 *   repin_ints_and_music_tick 1000:8B1E    cmd 2         tick()
 *   game_exit    (1000:8DD6 path)          cmd 0         destroy()
 *
 * The OSystem's audio members are these commands (osystem.h): audioInstall,
 * audioTempo, audioLoadTune, audioResume, audioSfx, audioStop, audioTick.
 *
 * The game sends only those commands (never 1, 3, 4, 7, 8, 9, 0x0B, 0x0D),
 * and all three shipped drivers implement the same 15-entry table: their
 * entries sit at ADLIB.DRV 0x1A2, MT32.DRV 0x19D and BEEPER.DRV 0x180, and
 * cmd 0x0E returns the driver's command block (AdLib 0x1577, MT-32 0x15D1,
 * Beeper 0x0E87) - empty in every observed session, and not consumed by the
 * port yet.
 *
 * Nothing here names a chip.  A module is free to emulate an OPL3, an
 * MPU-401 or a bit-banged speaker, as long as it answers these commands;
 * `generate()` is the one port-side addition, the sample sink the OSystem
 * audio backend pulls from.
 *
 * Modules (each exports its own `g_sound_driver_*`; a port links the ones
 * it offers and picks one at run time, the way the launcher picks the
 * resident driver through `[0x5AFE]`):
 *   adlib/sound_driver_adlib.c     ADLIB.DRV's sequencer over a vendored
 *                                  Nuked-OPL3
 *   beeper/sound_driver_beeper.c   BEEPER.DRV's sequencer over the PIT
 *                                  channel-2 square wave
 *   mt32/sound_driver_mt32.c       MT32.DRV's sequencer over a system
 *                                  mt32emu (needs its ROM images)
 *   null/sound_driver_null.c       no sound; prints every command it receives
 */
#ifndef HUMANSRE_SOUND_DRIVER_H
#define HUMANSRE_SOUND_DRIVER_H

#include <stdint.h>

typedef struct SoundDriver SoundDriver;

typedef struct SoundDriverModule {
    /* The driver's install entry (driver_reset's CALL FAR with AX=0x60).
     * The rate is the port's output detail; the DOS driver has none. */
    SoundDriver *(*create)(uint32_t sampleRate);
    /* cmd 0: uninstall.  NULL is accepted. */
    void (*destroy)(SoundDriver *drv);
    /* cmd 0x0A: CL = the game's tempo byte (clamped to 0x64 by the drivers). */
    void (*setTempo)(SoundDriver *drv, uint8_t tempo);
    /* cmd 0x0E: identify - the driver's 8-byte command block, ES:DI. */
    const uint8_t *(*commandBlock)(SoundDriver *drv);
    /* cmd 0x0C: DS:DX = the loaded .ALB/.RLD/.BEP image.  The size is the
     * port's buffer bound; the original only has the far pointer. */
    void (*loadTune)(SoundDriver *drv, const uint8_t *image, uint32_t size);
    /* cmd 5: CX = event id -> DI = voice handle (0xFFFF = refused). */
    uint16_t (*startEvent)(SoundDriver *drv, uint16_t event);
    /* cmd 6: DI = the handle startEvent returned. */
    void (*releaseEvent)(SoundDriver *drv, uint16_t voice);
    /* cmd 2: step the sequencer one tick (2 of every 3 game ticks). */
    void (*tick)(SoundDriver *drv);
    /* The port's output pull: interleaved signed 16-bit stereo (L,R). */
    void (*generate)(SoundDriver *drv, int16_t *dst, uint32_t frames);
} SoundDriverModule;

/* Defined by each sound-driver module; a port declares nothing and links the
 * ones it offers, then selects one with SoundDriverModule::create. */
extern const SoundDriverModule g_sound_driver_adlib;
extern const SoundDriverModule g_sound_driver_beeper;
extern const SoundDriverModule g_sound_driver_mt32;
extern const SoundDriverModule g_sound_driver_null;

#endif /* HUMANSRE_SOUND_DRIVER_H */
