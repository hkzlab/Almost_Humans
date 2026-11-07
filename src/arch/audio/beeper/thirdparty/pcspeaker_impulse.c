/*
 * SPDX-FileCopyrightText: 2025-2026 The DOSBox Staging Team
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * PC-speaker impulse model, adapted from DOSBox Staging for the HumansRE
 * beeper driver.  The upstream files and commit are named in
 * thirdparty/README.md; the licence is copied there as LICENSE and
 * GPL-2.0.txt.
 *
 * The algorithm is upstream's: a raised-cosine-windowed sinc impulse,
 * accumulated into a running waveform, plus the 8254 mode-3 transition
 * scheduler.  Upstream renders on a 1 ms PIC callback through the mixer;
 * here the PIT advances once per output sample and the result goes straight
 * to the driver's interleaved-stereo sink.  The only PIT mode the game's
 * BEEPER.DRV uses is mode 3 (0xB6), so the other 8254 modes are not ported.
 */
#include "pcspeaker_impulse.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#    define M_PI 3.14159265358979323846
#endif

#define PIT_TICK_RATE           1193182.0f
#define MS_PER_TICK             (1000.0f / PIT_TICK_RATE)
#define SINC_FILTER_DURATION_US 3125
#define SINC_OVERSAMPLING       32
#define SINC_AMPLITUDE_FADE     0.999f
#define PWM_SCALAR              0.5f
#define POSITIVE_AMPLITUDE      ((int16_t)(32767.0f * PWM_SCALAR))
#define NEGATIVE_AMPLITUDE      ((int16_t)(-POSITIVE_AMPLITUDE))
#define NEUTRAL_AMPLITUDE       ((int16_t)0)

/* DOSBox's PC-speaker mixer channel filters (upstream SetFilterState):
 * 3rd-order Butterworth high-pass at 120 Hz, 3rd-order low-pass at 4300 Hz. */
#define SPEAKER_HPF_HZ 120.0
#define SPEAKER_LPF_HZ 4300.0

/* One first-order section and one biquad make each 3rd-order Butterworth. */
typedef struct OnePole {
    float b0;
    float b1;
    float a1;
    float x1;
    float y1;
} OnePole;

typedef struct Biquad {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float x1;
    float x2;
    float y1;
    float y2;
} Biquad;

struct PcSpeakerImpulse {
    uint32_t sample_rate;
    uint32_t samples_per_ms;
    double rate_per_ms;

    /* The impulse lookup table (upstream's impulse_lut). */
    uint32_t sinc_quality;
    uint32_t lut_size;
    double cutoff_hz;
    double lut_gain;
    float *lut;

    /* The running waveform (upstream's waveform deque + accumulator). */
    uint32_t wave_size;
    uint32_t head;
    float *wave;
    float accumulator;

    /* The mixer channel's Butterworth filters. */
    OnePole hpf1;
    Biquad hpf2;
    OnePole lpf1;
    Biquad lpf2;

    /* PIT channel 2, mode 3 (upstream's PitCounter mode-3 state). */
    int32_t active;
    int32_t gate;
    int32_t output_high;
    float phase_ms;
    float period_ms;
    float half_ms;
    float new_period_ms;
    float new_half_ms;

    /* Port 0x61 (upstream's PpiPortB speaker_output bit). */
    int32_t speaker_output;

    /* Upstream's prev_amplitude: re-emitting the same impulse level would
     * bias the output, so identical consecutive levels are skipped. */
    int16_t prev_amplitude;
};

/**
 * @brief Unnormalised sinc, exactly as upstream computes it.
 *
 * @param x the argument
 * @return sinc(x), with sinc(0) = 1
 */
static double _sinc(double x)
{
    if (x == 0.0) return 1.0;
    return sin(x) / x;
}

/**
 * @brief One raised-cosine-windowed sinc impulse sample (upstream CalcImpulse).
 *
 * @param spk the model
 * @param t seconds from the impulse start
 * @return the impulse amplitude at t
 */
static float _calc_impulse(const PcSpeakerImpulse *spk, double t)
{
    const double fs = (double)spk->sample_rate;
    const double q = (double)spk->sinc_quality;
    double midpoint;
    double window;
    double amplitude;

    if (!(t > 0.0) || (t * fs >= q)) return 0.0f;

    midpoint = q / (2.0 * fs);
    window = 1.0 + cos(2.0 * fs * M_PI * (midpoint - t) / q);
    amplitude = spk->lut_gain * window *
                _sinc(2.0 * spk->cutoff_hz * M_PI * (t - midpoint)) / 2.0;
    return (float)amplitude;
}

/**
 * @brief Fill the oversampled impulse table.
 *
 * @param spk the model
 */
static void _init_lut(PcSpeakerImpulse *spk)
{
    const double step = 1.0 / ((double)spk->sample_rate * SINC_OVERSAMPLING);
    uint32_t i;
    double t = 0.0;

    for (i = 0; i < spk->lut_size; i++) {
        spk->lut[i] = _calc_impulse(spk, t);
        t += step;
    }
}

/**
 * @brief The amplitude a speaker state produces (upstream OutputAmplitude).
 *
 * @param speaker_output port 0x61 bit 1
 * @param output_high PIT output state
 * @return the impulse level
 */
static int16_t _output_amplitude(int32_t speaker_output, int32_t output_high)
{
    if (!speaker_output) return NEGATIVE_AMPLITUDE;
    return output_high ? POSITIVE_AMPLITUDE : NEGATIVE_AMPLITUDE;
}

/**
 * @brief Add one impulse at a sub-millisecond offset (upstream AddImpulse).
 *
 * @param spk the model
 * @param index milliseconds from the current sample, clamped to [0, 1]
 * @param amplitude the impulse level
 */
static void _add_impulse(PcSpeakerImpulse *spk, float index, int16_t amplitude)
{
    double base;
    int phase_raw;
    int offset;
    int phase;
    int k_start;
    int k_end;
    int k;

    if (amplitude == spk->prev_amplitude) return;
    spk->prev_amplitude = amplitude;

    if (index < 0.0f) index = 0.0f;
    if (index > 1.0f) index = 1.0f;
    base = (double)index * spk->rate_per_ms;
    phase_raw = ((int)(base * SINC_OVERSAMPLING)) % SINC_OVERSAMPLING;
    offset = (int)base + (phase_raw != 0 ? 1 : 0);
    phase = phase_raw != 0 ? SINC_OVERSAMPLING - phase_raw : 0;
    k_start = phase_raw == 0 ? 1 : 0;

    if (offset >= (int)spk->wave_size) return;
    k_end = (int)spk->sinc_quality;
    if (k_end > (int)spk->wave_size - offset)
        k_end = (int)spk->wave_size - offset;

    for (k = k_start; k < k_end; k++) {
        uint32_t at =
            (spk->head + (uint32_t)offset + (uint32_t)k) % spk->wave_size;
        spk->wave[at] +=
            (float)amplitude *
            spk->lut[(uint32_t)phase + (uint32_t)k * SINC_OVERSAMPLING];
    }
}

/**
 * @brief Emit a PIT output transition if the level changed.
 *
 * @param spk the model
 * @param high the new output level
 * @param delay_ms when the transition happens, in ms from now
 */
static void _pit_emit(PcSpeakerImpulse *spk, int32_t high, float delay_ms)
{
    if (high == spk->output_high) return;
    spk->output_high = high;
    _add_impulse(spk, delay_ms, _output_amplitude(spk->speaker_output, high));
}

/**
 * @brief PIT control word: channel 2, mode 3 (upstream WriteControl).
 *
 * @param spk the model
 */
static void _pit_write_control(PcSpeakerImpulse *spk)
{
    spk->active = 0;
    spk->phase_ms = 0.0f;
    _pit_emit(spk, 1, 0.0f); /* mode 3 drives OUT high */
}

/**
 * @brief PIT count write (upstream WriteCount, mode 3).
 *
 * @param spk the model
 * @param count the channel-2 count; zero means 65536
 */
static void _pit_write_count(PcSpeakerImpulse *spk, uint16_t count)
{
    uint32_t effective = count ? count : 0x10000u;

    /* Upstream: period = count * MsPerTick, high half = ((count + 1) / 2)
     * * MsPerTick.  The effective count is used for the zero reload so a
     * zero count never produces a zero-length high half. */
    spk->new_period_ms = (float)effective * MS_PER_TICK;
    spk->new_half_ms = (float)((effective + 1u) / 2u) * MS_PER_TICK;

    if (!spk->active) {
        spk->period_ms = spk->new_period_ms;
        spk->half_ms = spk->new_half_ms;
        if (spk->gate) {
            spk->phase_ms = 0.0f;
            spk->active = 1;
            _pit_emit(spk, 1, 0.0f);
        }
    }
}

/**
 * @brief PIT gate change (upstream EnableGate, mode 3).
 *
 * @param spk the model
 * @param gate true when port 0x61 bit 0 is set
 */
static void _pit_enable_gate(PcSpeakerImpulse *spk, int32_t gate)
{
    int32_t rising = !spk->gate && gate;
    int32_t falling = spk->gate && !gate;

    spk->gate = gate;
    if (falling) {
        spk->active = 0;
        _pit_emit(spk, 1, 0.0f); /* gate low forces OUT high */
    } else if (rising) {
        spk->phase_ms = 0.0f;
        spk->period_ms = spk->new_period_ms;
        spk->half_ms = spk->new_half_ms;
        spk->active = 1;
        _pit_emit(spk, 1, 0.0f);
    }
}

/**
 * @brief Advance the PIT mode-3 oscillator (upstream AdvanceOscillator).
 *
 * @param spk the model
 * @param duration_ms the elapsed time
 */
static void _pit_advance(PcSpeakerImpulse *spk, float duration_ms)
{
    if (!spk->active) return;

    while (duration_ms > 0.0f) {
        if (spk->phase_ms >= spk->half_ms) {
            /* Low half, ending at the full-period boundary. */
            if (spk->phase_ms + duration_ms >= spk->period_ms) {
                float delay = spk->period_ms - spk->phase_ms;
                duration_ms -= delay;
                spk->phase_ms = 0.0f;
                spk->period_ms = spk->new_period_ms;
                spk->half_ms = spk->new_half_ms;
                _pit_emit(spk, 1, delay);
            } else {
                spk->phase_ms += duration_ms;
                duration_ms = 0.0f;
            }
        } else {
            /* High half, ending at the half-period boundary. */
            if (spk->phase_ms + duration_ms >= spk->half_ms) {
                float delay = spk->half_ms - spk->phase_ms;
                duration_ms -= delay;
                spk->phase_ms = spk->half_ms;
                spk->period_ms = spk->new_period_ms;
                spk->half_ms = spk->new_half_ms;
                _pit_emit(spk, 0, delay);
            } else {
                spk->phase_ms += duration_ms;
                duration_ms = 0.0f;
            }
        }
    }
}

/**
 * @brief Set up the two 3rd-order Butterworth sections (upstream's mixer
 * high-pass at 120 Hz and low-pass at 4300 Hz).
 *
 * @param spk the model
 */
static void _filter_init(PcSpeakerImpulse *spk)
{
    const double fs = (double)spk->sample_rate;
    const double q = 1.0; /* 3rd-order Butterworth's complex pole pair */
    double k;
    double norm;

    /* High-pass: first-order real pole + biquad pair. */
    k = tan(M_PI * SPEAKER_HPF_HZ / fs);
    norm = 1.0 / (1.0 + k / q + k * k);
    spk->hpf2.b0 = (float)norm;
    spk->hpf2.b1 = (float)(-2.0 * norm);
    spk->hpf2.b2 = (float)norm;
    spk->hpf2.a1 = (float)(2.0 * (k * k - 1.0) * norm);
    spk->hpf2.a2 = (float)((1.0 - k / q + k * k) * norm);
    spk->hpf1.b0 = (float)(1.0 / (1.0 + k));
    spk->hpf1.b1 = -spk->hpf1.b0;
    spk->hpf1.a1 = (float)((k - 1.0) / (k + 1.0));

    /* Low-pass: same pole layout with the low-pass numerator. */
    k = tan(M_PI * SPEAKER_LPF_HZ / fs);
    norm = 1.0 / (1.0 + k / q + k * k);
    spk->lpf2.b0 = (float)(k * k * norm);
    spk->lpf2.b1 = (float)(2.0 * k * k * norm);
    spk->lpf2.b2 = spk->lpf2.b0;
    spk->lpf2.a1 = (float)(2.0 * (k * k - 1.0) * norm);
    spk->lpf2.a2 = (float)((1.0 - k / q + k * k) * norm);
    spk->lpf1.b0 = (float)(k / (1.0 + k));
    spk->lpf1.b1 = spk->lpf1.b0;
    spk->lpf1.a1 = (float)((k - 1.0) / (k + 1.0));
}

/**
 * @brief One first-order filter section.
 *
 * @param f the section state
 * @param x the input sample
 * @return the output sample
 */
static float _one_pole(OnePole *f, float x)
{
    const float y = f->b0 * x + f->b1 * f->x1 - f->a1 * f->y1;
    f->x1 = x;
    f->y1 = y;
    return y;
}

/**
 * @brief One biquad filter section.
 *
 * @param f the section state
 * @param x the input sample
 * @return the output sample
 */
static float _biquad(Biquad *f, float x)
{
    const float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 -
                    f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    return y;
}

/**
 * @brief Create the impulse model for a sample rate.
 *
 * @param sample_rate the output rate; 0 selects 48000
 * @return the model, or NULL when out of memory
 */
PcSpeakerImpulse *pcspeaker_impulse_create(uint32_t sample_rate)
{
    PcSpeakerImpulse *spk;
    const double reference_rate = 32000.0;
    const double cutoff_margin_2 = 2.0 + 0.2;

    if (sample_rate == 0) sample_rate = 48000;
    spk = calloc(1, sizeof(*spk));
    if (!spk) return NULL;

    spk->sample_rate = sample_rate;
    spk->samples_per_ms = (sample_rate + 999u) / 1000u;
    spk->rate_per_ms = (double)sample_rate / 1000.0;

    /* ceil(sample_rate * 3125 / 1000000), upstream's SincFilterQuality. */
    spk->sinc_quality =
        (uint32_t)(((uint64_t)sample_rate * SINC_FILTER_DURATION_US + 999999u) /
                   1000000u);
    spk->lut_size = spk->sinc_quality * SINC_OVERSAMPLING;
    spk->wave_size = spk->sinc_quality + spk->samples_per_ms;

    /* Upstream keeps the cutoff frequency constant instead of scaling it
     * with the rate, so the two models are tonally equivalent. */
    spk->cutoff_hz =
        (double)sample_rate /
        (2.0 + ((double)sample_rate / reference_rate) * cutoff_margin_2 - 2.0);
    spk->lut_gain = reference_rate / (double)sample_rate;

    spk->lut = malloc((size_t)spk->lut_size * sizeof(*spk->lut));
    spk->wave = calloc((size_t)spk->wave_size, sizeof(*spk->wave));
    if (!spk->lut || !spk->wave) {
        free(spk->lut);
        free(spk->wave);
        free(spk);
        return NULL;
    }
    _init_lut(spk);
    _filter_init(spk);

    spk->output_high = 1;
    spk->prev_amplitude = NEUTRAL_AMPLITUDE;
    spk->new_period_ms = 65536.0f * MS_PER_TICK;
    spk->new_half_ms = spk->new_period_ms / 2.0f;
    spk->period_ms = spk->new_period_ms;
    spk->half_ms = spk->new_half_ms;
    return spk;
}

/**
 * @brief Destroy the model.
 *
 * @param spk the model; NULL is accepted
 */
void pcspeaker_impulse_destroy(PcSpeakerImpulse *spk)
{
    if (!spk) return;
    free(spk->lut);
    free(spk->wave);
    free(spk);
}

/**
 * @brief Port 0x61 change: gate and speaker output move together.
 *
 * @param spk the model
 * @param on 1 = both bits set, 0 = both clear
 */
void pcspeaker_impulse_set_speaker(PcSpeakerImpulse *spk, int32_t on)
{
    int32_t gate_changed;

    if (!spk) return;
    gate_changed = (on != spk->gate);
    spk->speaker_output = on;
    if (gate_changed) _pit_enable_gate(spk, on);
    _add_impulse(spk, 0.0f,
                 _output_amplitude(spk->speaker_output, spk->output_high));
}

/**
 * @brief PIT control word 0xB6: channel 2, mode 3.
 *
 * @param spk the model
 */
void pcspeaker_impulse_set_control(PcSpeakerImpulse *spk)
{
    if (!spk) return;
    _pit_write_control(spk);
}

/**
 * @brief PIT channel-2 count write.
 *
 * @param spk the model
 * @param count the divisor; zero is the 65536 reload
 */
void pcspeaker_impulse_set_counter(PcSpeakerImpulse *spk, uint16_t count)
{
    if (!spk) return;
    _pit_write_count(spk, count);
}

/**
 * @brief Render the impulse model to interleaved stereo.
 *
 * @param spk the model
 * @param dst the destination, 2 * frames int16_t
 * @param frames the frame count
 */
void pcspeaker_impulse_generate(PcSpeakerImpulse *spk, int16_t *dst,
                                uint32_t frames)
{
    const float dt_ms = 1000.0f / (float)spk->sample_rate;
    uint32_t i;

    if (!spk) {
        memset(dst, 0, (size_t)frames * 2u * sizeof(*dst));
        return;
    }

    for (i = 0; i < frames; i++) {
        float value;
        int32_t sample;

        _pit_advance(spk, dt_ms);

        spk->accumulator += spk->wave[spk->head];
        spk->wave[spk->head] = 0.0f;
        spk->head = (spk->head + 1u) % spk->wave_size;

        value = spk->accumulator;
        /* DOSBox's PC-speaker mixer channel: HPF 3rd-order 120 Hz, then
         * LPF 3rd-order 4300 Hz. */
        value = _one_pole(&spk->hpf1, value);
        value = _biquad(&spk->hpf2, value);
        value = _one_pole(&spk->lpf1, value);
        value = _biquad(&spk->lpf2, value);
        if (value > 32767.0f) value = 32767.0f;
        if (value < -32768.0f) value = -32768.0f;
        sample = (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);

        dst[2u * i] = (int16_t)sample;
        dst[2u * i + 1u] = (int16_t)sample;

        spk->accumulator *= SINC_AMPLITUDE_FADE;
    }
}
