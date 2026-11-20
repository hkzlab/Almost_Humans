# DOSBox Staging PC-speaker impulse model (vendored)

`pcspeaker_impulse.c` and `pcspeaker_impulse.h` are a C99 adaptation of the
PC-speaker emulation in **DOSBox Staging**:

* upstream: <https://github.com/dosbox-staging/dosbox-staging>
* commit: `27b1b7ca719c21ef3444f7ad1cf2d9cd439527cc` (2026-11-03)
* source files:
  * [`src/hardware/audio/pcspeaker_impulse.cpp`](https://github.com/dosbox-staging/dosbox-staging/blob/27b1b7ca719c21ef3444f7ad1cf2d9cd439527cc/src/hardware/audio/pcspeaker_impulse.cpp)
  * [`src/hardware/audio/private/pcspeaker_impulse.h`](https://github.com/dosbox-staging/dosbox-staging/blob/27b1b7ca719c21ef3444f7ad1cf2d9cd439527cc/src/hardware/audio/private/pcspeaker_impulse.h)
  * [`src/hardware/audio/pcspeaker_pit.cpp`](https://github.com/dosbox-staging/dosbox-staging/blob/27b1b7ca719c21ef3444f7ad1cf2d9cd439527cc/src/hardware/audio/pcspeaker_pit.cpp)
  * [`src/hardware/audio/private/pcspeaker_pit.h`](https://github.com/dosbox-staging/dosbox-staging/blob/27b1b7ca719c21ef3444f7ad1cf2d9cd439527cc/src/hardware/audio/private/pcspeaker_pit.h)

`LICENSE` is the DOSBox Staging project licence (GPL-2.0-or-later, including
the upstream attribution text); `GPL-2.0.txt` is the full GNU GPL version 2
text it refers to. Both are copied verbatim from the commit above.

## What is unchanged

The raised-cosine-windowed sinc impulse (`CalcImpulse`), the oversampled
lookup table (`InitializeLut`), the impulse placement and level dedup
(`AddImpulse`), the running accumulator with `SincAmplitudeFade`, the output
level map (`OutputAmplitude`), and the 8254 mode-3 transition scheduler
(`PitCounter::WriteControl`, `WriteCount`, `EnableGate`,
`AdvanceOscillator`).

## What was adapted

* C++ classes and containers became one C struct with a fixed float ring
  buffer instead of `std::deque`.
* Upstream renders on a 1 ms PIC callback through the mixer; this adaptation
  advances the PIT once per output sample and writes interleaved stereo
  directly. The impulse offset (ms within the current step) is still passed
  to `AddImpulse`, so its oversampled phase interpolation is preserved.
* Only PIT mode 3 (the square-wave mode BEEPER.DRV writes as `0xB6`) is
  ported; the other 8254 modes are not reachable from this driver.
* The mixer's high-pass/low-pass filters are implemented in this file's
  `_filter_init`: DOSBox's `SetFilterState` configures a 3rd-order
  Butterworth high-pass at 120 Hz and a 3rd-order Butterworth low-pass at
  4300 Hz.  The port applies the same sections to the model output, so the
  steady-note harmonic series matches the live DOSBox capture.
* A zero PIT count is treated as the 65536 reload for the high half as well
  (upstream uses the raw `count` there).

Copyright for the adapted code remains with the DOSBox Staging Team; the
adaptation is distributed under the same GPL-2.0-or-later terms.
