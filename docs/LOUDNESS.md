# Standalone loudness meter

`Source/LoudnessMeter.h` provides a C++17 `LoudnessMeter` that can be used by
the processor without adding parameters or state-tree data. Call `prepare(fs)`
before processing. It uses a fixed 30-entry ring of 100 ms energy buckets;
`prepare`, `process`, `reset`, and `readings` do not allocate, lock, or perform
I/O. The caller owns
the meter from one thread. `process(left, right, channels)` consumes one sample
frame; `channels == 1` counts only `left`, and `channels >= 2` sums the two
channel energies. Antiphase stereo therefore does not cancel. A channel-count
change does not implicitly clear accumulated readings; call `reset()` when a
new measurement run should begin. Processing continues while samples arrive,
including stopped-host tails; the meter does not inspect transport state.

Momentary and short-term values use 400 ms and 3 s K-weighted energy windows.
Readings update every 100 ms. Until a full window exists, its value is the
`-100 LUFS` sentinel. Integrated loudness adds each completed 400 ms window at
100 ms intervals (75% overlap), applies the absolute `-70 LUFS` gate and the
relative `-10 LU` gate, and uses the BS.1770 offset of `-0.691 LU`. The bounded
histogram has 0.01 LU bins from -70 to +80 LUFS. Relative gate selection is
quantized to those bin boundaries; 0.01 LU is the threshold classification
step, not a universal error bound for integrated loudness. Overflow above
80 LUFS retains the actual summed energy and block count. Values are accumulated
until `reset()`, which is intended for explicit meter reset/new-run actions.
Silence and not-yet-filled windows report the sentinel. NaN and infinity inputs
are treated as zero; finite float input amplitudes retain their represented
value when converted to double for filtering and energy accumulation.

The K-weighting pre-shelf (1681.974 Hz, +3.9998 dB) and RLB high-pass
(38.135 Hz, Q 0.500327) follow the Annex 1 bilinear transform and are
recalculated in `prepare()` at the supplied sample rate. The
finite range is 8–960 kHz; invalid rates fall back to 48 kHz. The standard
44.1, 48, 96 and 192 kHz rates are covered, with 384 kHz as an additional
high-rate test. This is an implementation and audit aid, not a claim of
BS.1770 certification.

## Validation

Build and run the standalone test without building the processor test target:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DMASTERLIMITER_COPY_PLUGIN_AFTER_BUILD=OFF
cmake --build build --target MasterLimiterLoudnessTests -j2
ctest --test-dir build -R '^MasterLimiterLoudness$' --output-on-failure
```

The test covers window fill, mono/stereo energy, +3.0103 LU stereo summing,
antiphase, gating, reset, the four sample rates, invalid numeric samples and
allocation counts around process/readings/reset. The external audit creates
deterministic 8–12 second WAV fixtures (steady loud, loud followed by quiet,
antiphase and mono), runs the C++ meter and compares its final M/S/I values
with FFmpeg's per-frame `ebur128` metadata to 0.1 LU. It checks 100 Hz, 1 kHz,
and 10 kHz steady tones at 44.1/48/96/192 kHz, plus separate absolute- and
relative-gate, antiphase and mono fixtures at 48 kHz. FFmpeg must be installed.

```sh
python3 tools/validate_loudness.py \
  --test-binary build/MasterLimiterLoudnessTests \
  --output-dir build/loudness-audit
```

The output directory must be absent or empty. WAV/raw fixtures, per-case logs,
and `audit.json` are retained there. Comparison uses per-frame metadata rather
than the rounded human-readable summary. The 0.1 LU comparison tolerance is
a regression criterion, not independent certification.

## Plugin integration

The processor feeds final host-output samples after limiting, TP/mode mix and
bypass into the meter. M/S/I are published through SC atomic payload and a
sequence counter; GUI reset requests an audio-owned clear and immediately
hides previous readings even when processing is stopped. RESET METERS clears
all accumulators; there is no transport/playhead access and no measurement
state in APVTS. The audio path, parameter IDs and latency do not change.

At rates not divisible by ten, the 100 ms bucket length is rounded to the
nearest sample, so the 4/30-bucket windows inherit that small rounding. The
44.1/48/96/192 kHz validation rates have exact integer bucket lengths.

Primary references: [ITU-R BS.1770-5, Annex 1](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.1770-5-202311-I%21%21PDF-E.pdf)
and [EBU Tech 3343](https://tech.ebu.ch/docs/tech/tech3343v4_1.pdf).
