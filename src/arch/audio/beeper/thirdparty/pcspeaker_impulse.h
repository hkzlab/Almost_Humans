/*
 * SPDX-FileCopyrightText: 2025-2026 The DOSBox Staging Team
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * PC-speaker impulse model, adapted to C99 for the HumansRE beeper driver.
 *
 * Upstream (DOSBox Staging, GPL-2.0-or-later, commit 27b1b7c):
 *   src/hardware/audio/pcspeaker_impulse.cpp
 *   src/hardware/audio/pcspeaker_pit.cpp
 *   src/hardware/audio/private/pcspeaker_impulse.h
 *   src/hardware/audio/private/pcspeaker_pit.h
 * See thirdparty/README.md for the exact provenance and the licence copy.
 *
 * The upstream code is driven by a 1 ms PIC callback and the mixer.  This
 * adaptation keeps the impulse/lookup-table algorithm, the sinc window and
 * the PIT mode-3 transition scheduler, but advances the PIT per output sample
 * and renders directly into the port's interleaved-stereo sink (the beeper
 * driver's only PIT use is mode 3 with the gate following port 0x61).
 */
#ifndef HUMANSRE_PCSPEAKER_IMPULSE_H
#define HUMANSRE_PCSPEAKER_IMPULSE_H

#include <stdint.h>

typedef struct PcSpeakerImpulse PcSpeakerImpulse;

/* The upstream model is tied to a sample rate; 48000 is the port's rate. */
PcSpeakerImpulse *pcspeaker_impulse_create(uint32_t sample_rate);
void pcspeaker_impulse_destroy(PcSpeakerImpulse *spk);

/* Port 0x61 bits 0 (timer 2 gate) and 1 (speaker data), set together by
 * BEEPER.DRV's speaker_on/speaker_off (1E95:07ED/0809). */
void pcspeaker_impulse_set_speaker(PcSpeakerImpulse *spk, int32_t on);

/* PIT control word 0xB6 (channel 2, lobyte/hibyte, mode 3), 1E95:0806. */
void pcspeaker_impulse_set_control(PcSpeakerImpulse *spk);

/* PIT channel-2 count, 1E95:081D.  A zero count is the 65536 reload. */
void pcspeaker_impulse_set_counter(PcSpeakerImpulse *spk, uint16_t count);

/* Render interleaved signed-16-bit stereo (the speaker is mono). */
void pcspeaker_impulse_generate(PcSpeakerImpulse *spk, int16_t *dst,
                                uint32_t frames);

#endif /* HUMANSRE_PCSPEAKER_IMPULSE_H */
