/* The resident synth-driver contract: one call per INT 60h command.
 *
 *   install       AX=0x60        create(rate)      driver_reset 1000:84DE
 *   cmd 0         destroy()                         game_exit 1000:8DD6
 *   cmd 2         tick()         repin_ints_and_music_tick 1000:8B1E
 *   cmd 5         startEvent()   music_resume 1000:83F7 / driver_call 1000:83B7
 *   cmd 6         releaseEvent() music_stop 1000:8389
 *   cmd 0x0A      setTempo(cl)   driver_reset 1000:84DE
 *   cmd 0x0C      loadTune()     play_tune 1000:8437
 *   cmd 0x0E      commandBlock() driver_reset 1000:84DE
 *
 * `generate()` is the port's sample sink.  Modules: adlib/, beeper/, mt32/,
 * null/.  Deviations: D-249, D-254..D-256, D-261 (per module).
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

#endif /* HUMANSRE_SOUND_DRIVER_H */
