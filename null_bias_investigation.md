# Two-tone null-crossing frequency bias — investigation notes

**Status: characterized, NOT fixed. Parked 2026-08-31** — picking up the TF
(transfer function) measurement system and group-delay re-tuning first. See
"Where to resume" at the bottom. **Late addition, same day: user reports the
current `I` (x4 envelope interpolation) algorithm makes two-tone stability
audibly worse, not just subtly worse — see item 5 under "Open, un-actioned
next steps" below. Treat that as higher priority than the null-bias fix
itself when this is picked back up.**

**UPDATE 2026-09-09 — important caveat on `weighted_bias`, discovered while
chasing an unrelated random-frequency-jump symptom (see
`moving_forward_notes.md`):** `weighted_bias` was assumed to be a fast,
stable, per-tone-pair constant (reset with `'r'`, read a second or two
later). Live-watching it continuously (a new firmware change exempts the
`null_bias*` lines from the diagnostics mute, so they stream at 1Hz
regardless of `'v'`) on 700/1700Hz showed instead that after an `'r'` reset
it keeps drifting for TENS OF SECONDS TO MINUTES — not settling quickly —
and can wander by 10-30Hz over that time even with the tone pair, presets,
and hardware completely unchanged (one capture went from -34Hz shortly
after reset, up toward -0.2Hz, back down past -19Hz, with no external event
at all). Loading a different settings preset (which changes
gdeq/ampeq/predistort, and therefore the actual envelope shape through each
null) also visibly shifted where it was heading, which makes physical sense
given the root mechanism below - but the sheer slowness/magnitude of the
drift even with NOTHING changed was not expected or previously
characterized. **This means the "Confirmed measurement table" below,
captured "freshly reset, read ~1-2s later" per its own reproduction recipe,
may not represent settled/steady-state values** - a longer, fixed dwell
time (and ideally logging the full time-series to see whether/when it
actually plateaus, rather than a single point read shortly after reset)
would be needed before trusting those numbers as precise per-tone-pair
constants. It also directly explains the previously-unresolved puzzle in
item 4 under "Open, un-actioned next steps" below (the ~15Hz drift observed
"on the timescale of typing a sentence" that confounded the `I` on/off
comparison) - that's most likely this same slow-convergence behavior, not
noise or a separate mechanism. Real-world takeaway confirmed on the bench
the same session: this metric's wandering does NOT track the actual
transmitted frequency - the SDR showed a stable carrier while
`weighted_bias` swung by tens of Hz, so it is unrelated to (and not usable
as a live detector for) the separate random ~40Hz two-tone frequency-jump
symptom it was being tested against. Not yet root-caused *why* the
convergence is so slow - worth understanding before trusting this
diagnostic for anything time-sensitive again.

**Cross-reference, 2026-09-01:** the new PNP BC327+attn filter's group-delay
equalizer testing (`group_delay_fit_notes.md`, "2026-09-01 refit" Status
section) raised the same null-region-fidelity question from the other end —
that filter's own HF roll-off can't be the whole story for the disappointing
real-hardware IMD result, since `envelope_interp.h`'s Catmull-Rom stage
(item 5 below) independently rounds off the exact same two-tone null fold,
upstream of the analog filter. The two issues may be entangled in any
comp-on/off data taken with `'I'` enabled — worth checking `'I'`'s state
before trusting either investigation's real-hardware numbers in isolation.

## Symptom that started this

Poor perceived audio quality on two-tone tests: pitch wandering ~10Hz on a
slow timescale, plus a faster few-Hz FM noise riding on top. Both worse with
envelope interpolation (`I`) on, present at a lower level with `I` off.
Single-tone always tested spot-on to 1-2Hz.

## Ruled out, in order

1. **Digital scheduling/wakeup jitter.** `[timing] max_gap_us` essentially
   unchanged between `I` off/on across three separate measurement pairs
   (75→80us, `late_ticks_total=0` both ways). Not the mechanism.
2. **Analog AM-to-PM via envelope activity alone.** `AMTEST` (envelope swept
   through a full sine sweep, `freq_dev_hz` pinned to 0, bypasses
   `ssb_dsp_process_sample()` entirely) showed tone stability "close to
   perfect" regardless of `I`. Not the mechanism.
3. **`fast_atan2`/`fast_sqrt` approximation error.** Already ruled out in
   earlier project history (see `ssb_dsp.c`'s `SSB_DSP_FAST_TRIG` comment) —
   a direct A/B against real `atan2f`/`sqrtf` on real hardware showed no
   change to a similar historical two-tone offset.
4. **`max_freq_dev_hz` clamp asymmetry.** `clip_count` reads 0 at every
   tone-pair/spacing tested in this session at the current 8000Hz clamp —
   the clamp never engages, so it cannot be biasing anything right now. This
   *closes* the long-open thread in `config.h`'s `MAX_FREQ_DEV_HZ` comment
   ("whether that clamping was actually the cause... was never confirmed
   either way") for the current configuration.

## Root mechanism identified

At each two-tone destructive-interference null (envelope → 0, i.e. I≈Q≈0 in
`ssb_dsp_process_sample()`), the analytic-signal phase is mathematically
required to jump by exactly ±π — this is genuine signal content, not noise
(see `envelope_floor.cpp`'s own historical postmortem making the same point
about the phase side). The discrete-time `dphi = wrap_pi(phase -
prev_phase)` computation (`ssb_dsp.c`) resolves that jump with a small bias
that is **deterministic and specific to the exact tone pair**, not random —
because these test tones are generated as exact phase-accumulator multiples
of the sample rate, every null in a given test recurs at an *identical*
alignment to the sample grid, so whatever bias one null produces, every null
in that run produces identically. It accumulates coherently instead of
averaging out.

## Two different averages — only one of them is physically real

- **Plain (unweighted) time-average** of `dphi * Fs / 2π` ("`plain_mean`" /
  "`plain_bias`" in the diagnostic): dominated by the tiny fraction of
  samples sitting at a null, where `|dphi|` swings hugely for one sample.
  Measured 95-600Hz "bias" across the sweep below.
  **CONFIRMED WRONG as a predictor of on-air effect** — direct real-hardware
  check (700/1900Hz pair) showed the transmitted tones sitting close to
  their nominal frequencies, not shifted by the ~600Hz this predicted.
- **Envelope²-weighted average** instantaneous frequency ("`weighted_mean`"
  / "`weighted_bias`"): the physically correct quantity. For an analytic
  signal `A(t)e^{jφ(t)}`, the power spectrum's centroid equals the
  energy-weighted (`A(t)²`-weighted) average instantaneous frequency — a
  standard identity, not the plain time-average. The near-null samples that
  dominate the plain average sit exactly where envelope (and so envelope²)
  is smallest, so this weighting suppresses almost all of their
  contribution — which is exactly why the plain average overstated things
  so badly.

## Confirmed measurement table

All six `TWOTONE_BAND_PRESETS` pairs (`test_signals.cpp`), EQ and compressor
off, `null_bias_threshold` fixed at 0.050, freshly reset (`r`) before each
reading:

| pair | spacing (Δf) | plain_bias | weighted_bias | SDR-observed |
|---|---|---|---|---|
| 300/500 | 200Hz | +100.36Hz | **+4.03Hz** | ~3Hz |
| 700/900 | 200Hz | −101.11Hz | **−0.70Hz** | ~2Hz |
| 1500/1700 | 200Hz | −99.60Hz | **+0.11Hz** | <1Hz |
| 2500/2700 | 200Hz | −95.05Hz | **+0.33Hz** | <1Hz |
| 3500/3700 | 200Hz | −99.75Hz | **+0.16Hz** | <1Hz |
| 700/1900 | 1200Hz | −607.69Hz | **−20.72Hz** | ~8Hz |

`weighted_bias` matches the real, SDR-observed deviation to within a few Hz
every time (6/6) — this is the confirmed, working predictor.
`plain_bias` is wrong by 2-3 orders of magnitude and must not be used to
reason about on-air/perceptual effects — kept only so `near_null_contrib`
(see instrumentation below) can localize the mechanism.

## Key finding: NOT simply proportional to tone spacing

The very first data point taken at each spacing (300/500 at 200Hz, 700/1900
at 1200Hz) fit a clean "proportional to Δf" story (≈3Hz and ≈18Hz, ratio 6,
matching the spacing ratio) — but that was coincidental. 700/900 has the
*same* 200Hz spacing as 300/500 yet gives a `weighted_bias` of opposite sign
and ~6x smaller magnitude. **The bias is a function of the specific absolute
tone pair, not spacing (Δf) alone.** Likely contributors: the Hilbert FIR's
amplitude/phase response isn't perfectly flat across the audio band, and/or
the exact sample-grid alignment through each null depends on the specific
frequencies involved, not just their difference.

## Likely real-world significance (reasoned, not yet verified)

Two-tone test tones are perfectly periodic, so every null lands at the
identical relative sample-grid position every cycle — the per-event bias
accumulates coherently rather than averaging out. Real voice content's
envelope nulls occur at essentially unpredictable times relative to the
sample clock, so the same per-event bias would likely land with effectively
random sign from one occurrence to the next and mostly cancel over time.
**Working assessment: this is probably substantially a two-tone-test
artifact rather than a significant real-voice-quality problem** — still
worth fixing since it undermines trusting the IMD/linearity test signal
itself, but likely lower priority than it looked at the start of this
thread. Not verified against real speech/mic input.

## Instrumentation added this session (live in the tree)

- **`ssb_dsp.c`/`.h`**: `ssb_dsp_null_bias_stats_t` — `dphi_sum` /
  `dphi_sample_count` (plain), `near_null_dphi_sum` / `near_null_sample_count`
  (plain, restricted to `envelope < null_bias_threshold`), `env2_dphi_sum` /
  `env2_sum` (the weighted numerator/denominator). Accumulated in
  `ssb_dsp_process_sample()` right where `dphi` is computed, BEFORE
  slew-limiting or the `max_freq_dev_hz` clamp. `ssb_dsp_get_null_bias_stats()`
  reads it; `ssb_dsp_set/get_null_bias_threshold()` tune the near-null
  envelope cutoff (default 0.05). Everything resets via the existing
  `ssb_dsp_reset_freq_dev_stats()` (the `r` command already calls this).
- **`diagnostics.cpp`**: three print lines per ~1s cycle —
  `[dsp] null_bias` (f1/f2/expected_center/plain_mean/plain_bias),
  `[dsp] null_bias2` (weighted_mean/weighted_bias — **the one to trust**),
  `[dsp] null_bias3` (near_null_samples%/near_null_contrib/rest/threshold).
  Each is its own short `Serial.printf()` behind its own `diag_room_for()`
  call — a single combined line was tried first and silently never printed
  (needed ~200 bytes against this board's ~162-byte typical free buffer);
  splitting fixed it. Worth remembering if a future diagnostic line "goes
  missing" for no apparent reason — check its `diag_room_for()` size first.
- **`serial_commands.cpp`**: `n` cycles `null_bias_threshold` through
  `{0.02, 0.05, 0.10, 0.20}`.
- **Reproduce a reading**: `T` to a preset → confirm `e`/`c` off → confirm
  threshold=0.050 (`n` to cycle back if not) → `r` → wait ~1-2s → read the
  three `null_bias*` lines. See `ssb_mic_test_commands.md`'s "Null-crossing
  frequency bias diagnostic" section for the quick-reference version of all
  of this.

## 2026-09-11: null-uncertainty dither ('Q') added — untested

A third candidate fix, added this session in response to a direct "what about
dithering?" question and NOT yet bench-tested. Unlike the two "Targeted"/
"Principled" directions in item 1 below (which change how the ±π resolution
is computed at a null), this one leaves that computation alone and instead
attacks the *coherence* called out in "Root mechanism identified" above: the
test tones are exact phase-accumulator multiples of `SAMPLE_RATE_HZ`, so every
null lands at an identical sample-grid position every cycle, and whatever
tiny bias one null produces, every null produces identically — that's what
lets it accumulate instead of averaging out. `Q` (`test_signals.cpp`/`.h`)
adds a small (+/-0.5Hz), slowly and continuously varying frequency offset to
tone2 only (tone1 stays exactly at nominal as an undithered reference),
specifically to make successive nulls drift across the sample grid instead of
recurring at the same spot — the same role this file's own "likely real-world
significance" section says real voice's unpredictable null timing already
plays for free.

Validation plan, not yet run: compare `[dsp] null_bias2` (`weighted_bias`)
across several `r` resets on the SAME tone-pair preset, `Q` off vs. on, with a
LONG dwell before reading (see the 2026-09-09 update above — `weighted_bias`
drifts for tens of seconds to minutes after reset even with nothing changed,
so a quick point-read would just compare noise to noise). Looking for the
off-case's tightly-repeatable, tone-pair-specific constant to turn into
something that scatters and/or trends toward a smaller magnitude on-case,
given enough integration time. Full details/rationale: `test_signals.h`'s
doc comment on `test_signals_get_twotone_dither_enabled()`.

Note this targets the null-bias *measurement* artifact specifically, not the
separate "theoretically infinite phase bandwidth at the origin" EER/polar
problem `group_delay_fit_notes.md` researched (2026-09-03) — that entry found
NO literature precedent for dithering as a fix for that other problem, and
recommended an upstream I/Q trajectory reshape instead. The two shouldn't be
conflated even though both start with "there's a problem at the null."

## 2026-09-11: first two `Q` bench results — averaged bias insensitive to
## dither amplitude, real-time spread looks threshold-like, not graded

Two captures on the 700/1900Hz (wide legacy) pair, both against this file's
own confirmed baseline for that pair above (`weighted_bias` -20.72Hz, SDR
~8Hz, dither off):

- **`Q` on, +/-0.5Hz**: board read as a noisy peak +/- ~6Hz around zero.
  `weighted_bias` -16.4Hz. Not a clean isolated A/B — this capture came right
  after reflashing the `Q` build, so settle time wasn't independently
  controlled for.
- **`Q` on, +/-0.05Hz** (`TWOTONE_DITHER_MAX_HZ` reduced 10x): board read as
  "continuous" shifting, roughly -10Hz to +3Hz. `weighted_bias` -17.4Hz.

Two findings, taken together:

1. **`weighted_bias` barely moved** (-16.4 -> -17.4Hz) despite the dither
   amplitude shrinking 10x. If dither worked by smoothly smearing the bias in
   proportion to how far off-grid it pushes tone2, the averaged bias should
   have shrunk back toward the -20.72Hz undithered baseline (or trended
   toward 0) as amplitude fell. It didn't move in either direction by much.
2. **The real-time spread stayed roughly the same overall size at both
   amplitudes** (~12Hz peak-to-peak at 0.5Hz dither vs. ~12-13Hz at 0.05Hz
   dither). A swing of similar magnitude at dither depths 10x apart is the
   signature of a threshold/discontinuity effect, not a graded one: once
   dither is nonzero at all, it appears sufficient to occasionally knock a
   null onto the other side of whatever discrete sample-grid boundary drives
   the coherent bias described in "Root mechanism identified" above,
   producing a close-to-full-scale jump regardless of how small the nudge
   actually is.

**Open discrepancy, not yet resolved**: the -17.4Hz `weighted_bias` average
from the +/-0.05Hz run sits well outside the user's reported real-time range
(-10 to +3Hz, midpoint ~-3.5Hz). Two live hypotheses, not yet distinguished:

- `weighted_bias` and whatever instrument produces the board's reading are
  different statistics of the same underlying signal (different
  integration/settling behavior) and simply aren't required to agree
  numerically even if both are "correct" in their own terms.
- The continuous re-dithering (a fresh random target every 250ms at
  `TWOTONE_DITHER_UPDATE_HZ=4.0`) may be preventing that instrument from ever
  reaching steady state — i.e. some or all of the "continuous shifting" could
  be a measurement-methodology artifact of the test itself never settling,
  not a property of the transmitted signal.

Proposed follow-up, not yet run: slow `TWOTONE_DITHER_UPDATE_HZ` down a lot
(e.g. to 0.2Hz — a new target every 5s) while keeping amplitude small, and
see whether the board's reading can settle between updates. If the swings
shrink at a slower update rate, that points at the settling-artifact
hypothesis; if they don't, that favors either the two-different-statistics
explanation or the null-boundary-crossing read above being real and
independent of update rate. Also still unconfirmed: exactly what "the board"
is and how it derives a frequency reading (SDR waterfall peak vs. a
frequency counter vs. a tracking/PLL demod would each have very different
settling behavior).

## 2026-09-12: "the board" identified — an FFT panadapter, not a tracking
## loop, which resolves most of the open discrepancy above via basic
## integration-time math, no settling-artifact needed

User confirmed "the board" is SDRUno's "Aux SP" high-resolution FFT display,
currently set to 0.18Hz/bin, with the receiver's absolute frequency
reference (a 10MHz OCXO/ref) independently estimated accurate to +/-1Hz.
That +/-1Hz figure rules out RX calibration drift as an explanation for a
~12Hz-wide swing, but it also means every individual Aux SP reading can be
taken at face value as a real measurement, not an artifact of the receiver's
own frequency accuracy — so the -10 to +3Hz swing is genuinely showing
something about the signal (or about how a short window samples it), not
about the SDR being mistuned.

**This is an FFT panadapter, not a PLL/tracking demodulator** — worth
correcting the framing from the previous entry above, which floated "the
instrument not settling" as if a tracking loop were involved. There's no
loop here to settle; each Aux SP redraw is a fresh (or overlapped) spectral
snapshot integrated over however many samples its resolution setting
requires. That integration TIME is the number that matters, and it can be
worked out directly from the stated resolution:

- Basic FFT resolution identity: bin spacing = 1 / (window duration in
  seconds), for a rectangular window. 0.18Hz/bin → window duration ~= 5.56s.
  SDRUno's Aux SP most likely applies a lower-sidelobe window (Hann/
  Blackman-Harris-family, standard for panadapter displays) rather than a
  bare rectangular one, which widens the effective mainlobe for the same
  window length by roughly 1.5-2x — so achieving 0.18Hz of DISPLAYED
  resolution probably means an actual capture length somewhat longer than
  5.56s, plausibly out to 8-11s. Call it "on the order of 5-10 seconds"
  per spectrum snapshot.
- `Q`'s dither redraws an independent random target every
  `1/TWOTONE_DITHER_UPDATE_HZ` = 0.25s (`config.h`,
  `TWOTONE_DITHER_UPDATE_HZ=4.0`) and ramps linearly toward it in between —
  so within one ~5-10s Aux SP snapshot, only about **20-40 independent
  dither realizations** get folded into that single spectral picture.
- `weighted_bias` (`env2_sum`/`env2_dphi_sum` in `ssb_dsp.c`) is a plain
  lifetime accumulator that only resets on `'r'` — never a decaying/moving
  average. By the time it's read at, say, t=706s into a run, it has
  integrated across ~706s / 0.25s = **~2800 independent dither
  realizations** — roughly 70-130x more than a single Aux SP snapshot sees.

Given the two bench results directly above already established that this
null-bias effect behaves like a threshold/discontinuity rather than a
smoothly-graded one (each dither realization can swing the LOCAL average by
close to a full-scale amount, not a small increment), an estimator built
from only ~20-40 such realizations (one Aux SP snapshot) is expected to have
much higher variance than one built from ~2800 of them (`weighted_bias` at
the point of reading) — even though both are, in principle, converging
toward the same true long-run mean as more realizations accumulate. That is
a completely ordinary small-N-vs-large-N sampling-variance effect once the
underlying per-realization distribution is skewed/bimodal-ish rather than
tightly clustered, and needs no receiver-settling mechanism to explain it.
This makes the "two different statistics of the same signal" hypothesis
from the entry above the better-supported one specifically because of *how*
different their integration times are (~10x per snapshot vs. potentially
hundreds-to-thousands of accumulated seconds for `weighted_bias`), not just
that they're different in some general sense.

**Cleaner next experiment than varying the dither update rate**: toggle `Q`
OFF and watch Aux SP on the same 700/1900Hz pair for a comparable stretch.
The undithered baseline (this file's confirmed-measurement table) already
gives a single steady `weighted_bias` (-20.72Hz for this pair) — the open
question is whether Aux SP *also* shows several-Hz snapshot-to-snapshot
scatter with dither off (which would mean much of the ~12Hz spread seen with
`Q` on isn't new — the receiver/signal already had comparable short-window
noise for other reasons, e.g. residual phase noise or ADC-referred jitter
unrelated to the coherent null-bias mechanism), or whether it's rock-steady
without dither (which would confirm the scatter is specifically
dither-induced, via the small-N sampling-variance mechanism above). This is
a single-variable toggle on hardware already in hand, cheaper to run than
the slow-update-rate experiment proposed above, and worth doing first — not
yet run.

## 2026-09-12, later same day: CORRECTION to the entry above (Aux SP is
## exponential-averaging, not a fixed-window FFT snapshot) — plus two new
## hardware facts (visual read resolution, AD9851 XO thermal drift) — and a
## direct answer to "shouldn't dither have centered this on zero?"

User corrected the entry above: Aux SP uses **exponential averaging**, not
a fixed-length rectangular/Hann window — so the "~5-10s snapshot, ~20-40
independent dither draws per snapshot" arithmetic above is the wrong model
and is superseded by this entry (left in place rather than deleted, per this
file's convention, since the underlying "short effective memory sees more
variance than a long one" conclusion still holds, just via a different
mechanism — see below). Two more relevant facts: (1) the user's own visual
read of the moving trace is reliable only to about +/-5Hz (cursor-based
static measurements are much better) — some of the apparent -10..+3Hz
spread is eyeballing uncertainty on top of whatever the display is actually
doing; (2) the AD9851's reference is a plain 30MHz XO (not an OCXO) —
confirmed stable once warmed up, but a physical disturbance (touching it to
cool it) shifts the synthesized output by **~20Hz**, i.e. an effect of
comparable magnitude to the whole swing being investigated here.

**Why exponential averaging still predicts persistent, never-settling
scatter (arguably better than the fixed-window model did):** an EWMA has a
FIXED asymptotic variance that does not keep shrinking the longer you watch
it — it continuously "forgets" older samples at a constant rate, so its
noise floor is set by its time constant, not by total elapsed observation
time. `weighted_bias` (`env2_sum`/`env2_dphi_sum`, plain lifetime
accumulator, never resets except on `'r'`) is the opposite: its variance
keeps shrinking like 1/(elapsed time) for as long as you let it run. So Aux
SP is expected to go on fluctuating around whatever its true underlying mean
is indefinitely, while `weighted_bias` keeps getting quieter — which matches
"continuous shifting" being observed as an ongoing, non-settling behavior
rather than something that would eventually stop if watched longer.

**The AD9851 XO point matters because it's a confound `weighted_bias` is
immune to and Aux SP is not.** `weighted_bias` is computed entirely in the
digital/audio domain from the DSP's own I/Q (`ssb_dsp.c`) — it has no
dependency on the AD9851's actual reference clock accuracy. Whatever Aux SP
reads on-air, by contrast, is the ACTUAL transmitted RF frequency, which
rides on top of the real DDS output and therefore includes any real thermal
drift of the 30MHz reference. A confirmed ~20Hz/disturbance sensitivity on
that XO means ordinary ambient thermal movement over the course of a
multi-minute test could plausibly contribute several Hz of genuine,
real-world frequency drift — layered on top of, and NOT distinguishable
from, whatever the null-bias/dither mechanism itself is doing, when reading
Aux SP alone. **This makes `weighted_bias` the cleaner of the two signals
for judging whether `Q` is working**, precisely because it can't see XO
drift at all — Aux SP is the necessary real-world validation channel, but
it's reading two superimposed effects, not one.

**Direct answer to "I expected dither to center the frequency on zero — is
that not happening, and if so why?"** Judged on `weighted_bias` (the
XO-drift-immune channel), the answer is: partially, and the shortfall looks
structural, not a tuning problem. Undithered on this pair: -20.72Hz. With
`Q` on: -16.4Hz (0.5Hz dither) and -17.4Hz (0.05Hz dither) — a real
improvement over the undithered case, but plateaued around -17Hz rather than
continuing toward 0, and unmoved by a 10x change in dither amplitude. The
mechanism `Q` targets (per "Root mechanism identified" above) is
specifically the *coherent amplification* that comes from every null
recurring at the identical sample-grid alignment — breaking that coherence
should convert "the single worst-case alignment's bias, repeated forever"
into "the bias averaged over MANY different alignments." That is a
different claim from "the per-null bias averages to zero across alignments"
— it only nets out to ~0 if the underlying bias-vs-alignment function is
itself roughly zero-mean, which nothing in this investigation has actually
established. The amplitude-insensitivity of the ~-17Hz plateau (same result
at 0.5Hz and 0.05Hz dither, i.e. across two very different samplings of the
alignment space) is consistent with `Q` already fully achieving its
designed job — decorrelating alignment — and what's left (-17Hz) being the
genuine alignment-averaged mean of the near-null bias mechanism itself,
which this evidence suggests is NOT zero-mean. If so, no amount of further
dither tuning (amplitude or update rate) would be expected to close that
remaining ~17Hz gap — a different fix (one of the two "Targeted"/
"Principled" directions in "Open, un-actioned next steps" below, which
change how the ±π resolution is computed at a null rather than just
scrambling which alignment gets hit) would be needed for that part.

**Two concrete follow-ups, not yet run:**
- **Repeat the `Q` on/off comparison on a pair that starts near zero
  undithered** — e.g. 1500/1700 (`+0.11Hz` undithered per the confirmed
  table above) rather than 700/1900 (`-20.72Hz`, and separately already
  flagged elsewhere in this codebase as a group-delay outlier vs. the
  tighter-spaced bands). If `Q` leaves a near-zero pair still near zero,
  that's reassuring — dither isn't introducing a new artifact of its own
  (e.g. via its own 4Hz update periodicity). If it PUSHES a near-zero pair
  away from zero, that would suggest the alignment-averaged mean of the
  bias function varies by pair in a way that isn't simply "coherent bias
  good, dithered bias better," and would need its own explanation.
- **For any future real-world (not just `weighted_bias`) comparison, use
  Aux SP's cursor-based static measurement (confirmed by the user to be far
  more precise than eyeballing the moving trace) rather than reading the
  live display**, and take several such readings over a run rather than one,
  to separate genuine signal movement from both the +/-5Hz visual-read
  limit and the EWMA's own non-settling noise floor.

## 2026-09-12, later still: mechanism found for "changing relative_delay
## (or switching presets) shifts the measured tone frequency" — and a real
## gap identified in every weighted_bias reading taken so far

User reports (back on 700/1700 now, and reporting Aux SP readings as offsets
from a 1000Hz nominal center from here on) that adjusting relative delay
(`'['`/`']'`) or switching between presets with different delay values
(e.g. 1 & 3) repeatedly shifts the measured tone frequency - sometimes by a
few Hz, sometimes by as much as 25Hz - and that the shift often behaves like
it's "locked in" to one of two discrete values rather than moving smoothly.
Traced to a real, previously-unflagged interaction between `relative_delay`
(`relative_delay.cpp`/`.h`) and the near-null bias mechanism this whole file
is about.

**Mechanism.** `relative_delay_apply()` runs in `ssb_mic_test.ino` AFTER
`ssb_dsp_process_sample()` has already computed `freq_dev_hz`/`envelope` for
the tick - it doesn't touch the analytic-signal math, it only holds one of
the two signals back relative to the other (positive delay holds
`freq_dev_hz` back; every two-tone preset uses positive delay, so
`envelope` passes through untouched while `freq_dev_hz` gets time-shifted).
`weighted_bias`'s env^2-weighting exists specifically because it's supposed
to match the real transmitted power spectrum's centroid — that only works
if the near-null `freq_dev` spikes (the thousand-Hz-plus, single-tick
excursions already characterized via `max_freq_dev_step`/the post-step
trace) land on samples where `envelope` is genuinely near zero, so their
contribution to what's actually radiated stays suppressed. `relative_delay`
directly controls whether that alignment holds: shift `freq_dev_hz` far
enough relative to the untouched `envelope`, and a spike that used to land
on envelope~0 now lands on non-negligible envelope - i.e. non-negligible
transmitted power - at the exact instant the instantaneous frequency is
badly wrong. Since these spikes recur at the identical coherent sample-grid
alignment every cycle (the same rational-tone/`SAMPLE_RATE_HZ` mechanism as
"Root mechanism identified" above), a FIXED delay value produces a FIXED,
repeatable amount of this contamination - matching the "locks in" behavior
directly. And because the spike itself is narrow/near-discontinuous (1-2
samples), how much of it gets exposed is a steep, non-linear function of
delay - small delay changes near a good alignment barely matter, crossing
in or out of the spike's window can swing the bias by a lot - matching both
the "sometimes a few Hz, sometimes 25Hz" and the snap-between-two-values
observations. Confirmed directly relevant to presets: preset 1 ("TwoTone
Base") has `relative_delay_samples=0.00`; preset 3 ("Shelf2 Baseline gdeq
adj#4") has `2.00` (`settings.h`) - a full 2-sample/125us swing baked
straight into the preset switch, easily enough to move a spike from
aligned-with-zero to fully exposed.

**Real diagnostic gap, not just a theoretical point.** `ssb_mic_test.ino`
already carries a comment at the `relative_delay_apply()` call site (added
some earlier session) noting the null-bias diagnostics are "PRE-delay
values ... and have always been blind to whatever the delay line does,"
reasoning "it shouldn't [alter frequency content] - a pure sample delay
can't change frequency content." That's correct about `freq_dev_hz`'s own
spectrum in isolation, but it's the wrong question - the effect is in the
CROSS-alignment between two different signals (`freq_dev_hz` vs the
untouched `envelope`), not in `freq_dev_hz`'s own frequency content. Net
effect: **every `weighted_bias` reading taken anywhere in this file,
including both `Q`-dither bench results above, was computed pre-delay and
is structurally blind to whatever `relative_delay_samples` (hence '['/']'
and preset choice) does to the actually-transmitted spectrum.** Aux SP has
been the only instrument able to see this effect at all so far. This is
also very plausibly the same underlying event behind this whole codebase's
extensive delay-tuning-for-two-tone-IMD history (`relative_delay.h`'s doc
comment) - IMD splatter and this center-frequency shift read as two symptoms
of the same "spike exposed to nonzero transmitted power" event, not
separate phenomena.

**Proposed, not yet implemented**: add a post-delay variant of the
null-bias accumulation, fed from `delayed_freq_dev_hz`/`delayed_envelope`
right after the `relative_delay_apply()` call in `ssb_mic_test.ino`, so
there's a firmware-side, AD9851-XO-drift-immune number that actually tracks
what `'['`/`']'` and preset switches do to the transmitted spectrum, instead
of relying on Aux SP alone (which per the entry above has its own
integration-time and XO-drift complications). Asked the user whether to
implement this - not yet actioned.

## 2026-09-12, later still: new per-event jump log implemented ('J') -
## direct test of whether every random TX jump is really null-crossing

User's explicit engineering position, stated directly: they want the random
freq jumps ELIMINATED, and still have real doubts that the null-crossing
mechanism is the only thing going on, given how many "strange states" this
investigation has turned up (relative_delay's effect, the exponential-
averaging correction, the AD9851 XO drift, etc.). Asked for whatever
diagnostics would help settle this for definite. Implemented, not yet
bench-tested (no toolchain in this environment - see this project's
standing caveat on every firmware change):

**The gap being closed**: every jump-related diagnostic in this file up to
now (`max_freq_dev_step`, the post-step `tx_freq` trace) only ever
remembers the SINGLE worst event across an entire run - a multi-hour
unattended capture hands back exactly one data point, which is anecdote,
not evidence, when trying to settle whether ALL jumps share one cause.

**What's new** (`diagnostics.h`/`.cpp`, `ssb_mic_test.ino`,
`serial_commands.cpp`, `ssb_dsp.c`/`.h`, `relative_delay.c`/`.h`):

- A per-event **jump log** (`JUMP_LOG_LEN=8` ring, `JUMP_LOG_THRESHOLD_HZ=300`
  - a much lower, more inclusive bar than "new all-time record") that
  stores every qualifying `tx_freq` step, not just the record-breaker, each
  with: the POST-delay (actually-transmitted) envelope at that exact
  sample, the current `relative_delay_samples`, that tick's own `busy_us`,
  the active audio source, and a `near_null` flag
  (`envelope < ssb_dsp_get_null_bias_threshold()`, evaluated on the
  post-delay envelope specifically - see the 2026-09-12 relative_delay
  entry above for why post-delay is the physically relevant one here, not
  the pre-delay value the existing null_bias stats use).
- A running total + near-null count, printed as a cheap one-line summary
  in the always-on periodic block (`[dsp]   jump_log: n=... near_null=...
  (NN%)`) so an unattended run's own serial log shows the near-null
  percentage accumulate over hours without anyone needing to catch it live.
- A new **'J' serial command** to dump the full ring (all captured
  events, oldest to newest) on demand.
- The existing post-step `tx_freq` trace also now captures the matching
  POST-delay envelope in parallel (`post-step-env:` line) - same direct
  test, applied to the one worst-ever event that trace already tracks.

**Why this is the direct test, not another indirect inference**: if the
null-crossing theory is the whole story, `near_null` should read close to
100% across many independently-captured events, accumulated over hours,
not just the one event this file has been reasoning about so far. If a
meaningful fraction of logged jumps come back `near_null=false`, that's
hard, per-event, firmware-side evidence of a separate mechanism - not
something that can be argued away as "well, maybe near-null classification
was borderline that one time." The `relative_delay_samples` and `busy_us`
fields on each entry mean a delay sweep or a timing anomaly can also be
correlated directly against jump occurrence, rather than only inferred
indirectly the way this whole session's relative_delay finding was.

**Also settles, essentially for free**: whether this happens on mic input
too, not just the two-tone test - the log is audio-source-agnostic (every
entry records which source was active), so leaving 'J' logging running
across a source change and comparing counts answers this without a
separate diagnostic.

**Suggested test protocol** (not yet run): `'r'` to reset, leave running
unattended for a long stretch on two-tone (as in the earlier ~4.7-hour
run), then `'J'`. Repeat on mic input for a comparable stretch. Compare
`near_null` percentages and source breakdowns between the two. If both come
back ~100% near-null regardless of source, that's strong convergent
evidence for the existing theory. Any consistent population of
`near_null=false` events, especially ones sharing an unusual `busy_us` or a
particular `relative_delay_samples` value, points at a second, distinct
mechanism worth chasing on its own.

**Two small IRAM_ATTR fixes made along the way**: `ssb_dsp_get_null_bias_threshold()`
and `relative_delay_get_samples()` were previously not marked `IRAM_ATTR`
(their setters were) - both are now called from the new jump-log code on
the dsp_task hot path, so both were marked, matching this codebase's
existing IRAM discipline everywhere else on that path. Pure classification
changes (trivial single-field reads), not behavior changes.

## 2026-09-12, later still: first 'J' bench data - a real bug found and
## fixed in the jump log's own near_null classification, and a compelling
## (not yet confirmed) unifying explanation for the whole investigation

First real data from the new jump log, on 700/1700, `relative_delay_samples
= +4.60`:

```
[dsp] jump_log: 503384 qualifying step(s) >300Hz since last reset, 0 near-null (0%) - ring holds the last 8
[dsp]   jump[0]: t=247154ms 14198159->14196560Hz (step=1599Hz) env=0.595 delay=+4.60 busy_us=37 src=TWO-TONE TEST
[dsp]   jump[1]: t=247154ms 14196560->14201361Hz (step=4801Hz) env=0.617 delay=+4.60 busy_us=36 src=TWO-TONE TEST
[dsp]   jump[2]: t=247155ms 14201361->14198162Hz (step=3199Hz) env=0.509 delay=+4.60 busy_us=43 src=TWO-TONE TEST
[dsp]   jump[3]: t=247155ms 14198162->14196561Hz (step=1601Hz) env=0.595 delay=+4.60 busy_us=37 src=TWO-TONE TEST
[dsp]   jump[4]: t=247155ms 14196561->14201359Hz (step=4798Hz) env=0.616 delay=+4.60 busy_us=36 src=TWO-TONE TEST
[dsp]   jump[5]: t=247156ms 14201359->14198159Hz (step=3200Hz) env=0.509 delay=+4.60 busy_us=44 src=TWO-TONE TEST
[dsp]   jump[6]: t=247156ms 14198159->14196559Hz (step=1600Hz) env=0.594 delay=+4.60 busy_us=37 src=TWO-TONE TEST
[dsp]   jump[7]: t=247156ms 14196559->14201359Hz (step=4800Hz) env=0.616 delay=+4.60 busy_us=36 src=TWO-TONE TEST
```

**Real bug found in the jump log itself, now fixed** (`relative_delay.h`/
`.cpp`, `diagnostics.h`/`.cpp`, `ssb_mic_test.ino`). 503384 events, 0%
near_null, with `env` sitting at 0.5-0.6 (nowhere near the 0.05 null
threshold) looked at first like hard evidence AGAINST the null-crossing
theory. It wasn't - it was this diagnostic classifying against the WRONG
envelope. The `envelope` field used `delayed_envelope`
(`relative_delay_apply()`'s post-delay output), reasoning "what's actually
transmitted is the physically relevant thing." That's true for judging
transmitted power, but wrong for null classification: for `delay>0` (every
two-tone preset, including this one), `relative_delay_apply()` reads the
envelope ring at `env_back=0` - i.e. `delayed_envelope` is just the
CURRENT tick's envelope, not time-shifted at all. Meanwhile `freq_dev_hz`
(hence `tx_freq`) IS shifted, by the full `+4.60` samples. So the envelope
being compared against a given `tx_freq` value was up to 4.60 samples
(~287.5us at 16kHz) away from the moment that `tx_freq` value was actually
computed - comparing two things that were never supposed to line up.
Fixed by adding a THIRD output to `relative_delay_apply()`,
`out_envelope_at_freq_time`, which always reads the envelope ring at the
SAME lag as `freq_dev_hz` (`freq_back`, not `env_back`), regardless of
delay's sign - a cheap, symmetric extension of the ring interpolation
mechanism already there. The jump log's `envelope`/`near_null` fields (and
the post-step trace's envelope capture) now use this instead.

**The bigger, still-open question: is +4.60 itself the real story here?**
Every relative_delay value bench-validated anywhere in this project's whole
history (the delay-tuning saga in `relative_delay.h`'s own doc comment, the
gdeq candidate-B compromise window, every settings.h preset) stays within
roughly 0.00 to 2.10 samples. `+4.60` is more than double the largest value
this codebase has ever actually tested on real hardware, and it's getting
close to the ring's hard clamp (`PHASE_DELAY_MAX_SAMPLES-2 = 6`) - a value
that ring was sized to have generous HEADROOM for, per its own declaration
comment, not a value expected to be used in normal operation.

That matters because of a compelling (not yet confirmed) way this could
unify with the 2026-09-12 relative_delay/near-null-spike finding above: if
a well-tuned delay keeps most beats' near-null spikes suppressed (aligned
with genuine envelope near-zero) and only occasionally lets one leak
through - matching this project's original symptom of RARE, hours-apart
jumps - then a badly-mistuned delay like +4.60 could plausibly expose
EVERY beat's spike instead of an occasional one, turning a rare glitch into
a continuous, metronomically regular oscillation. The captured data is
consistent with that: the 8 events form an exact, repeating 3-value cycle
(steps -1600/+4800/-3200Hz, which sum to exactly zero over one full cycle)
recurring roughly every ~490us (503384 events over ~247s) - not random at
all, but a tight, self-sustaining pattern, exactly what you'd expect if
every single beat cycle is hitting the same exposed-spike condition rather
than an occasional unlucky one. The correlated `busy_us` shift (36-37us on
two of every three logged ticks, 43-44us on the third) is a further,
independent clue in the same direction - a null-crossing `atan2`/near-zero
computation taking measurably longer (plausibly IEEE-754 subnormal/gradual-
underflow slowdown right at the null) is a very plausible explanation for
why busy_us tracks the SAME 3-cycle pattern as the frequency itself.

**Not yet confirmed - this needs a direct test, not more reasoning from one
capture.** Was `relative_delay=+4.60` reached deliberately (testing an
extreme value on purpose) or by accident (e.g. `']'` held down/pressed
many times)? Either way, the decisive next step: reflash with the corrected
files, then compare 'J' at this SAME +4.60 delay (near_null should now
read close to 100% if the bug fix above is right) against 'J' at a
previously-validated delay (0.00-2.10, e.g. reload preset 1/2/3, `'r'`
reset, run for a comparable stretch). If the qualifying-step COUNT collapses
from ~500000-per-4-minutes down to something close to this project's
original "rare, hours-apart" rate once delay is back in the validated
range, that's strong, direct, quantitative confirmation that delay-tuning
quality is the dominant driver of jump RATE and that the null-crossing/
spike-exposure mechanism (corrected classification and all) is the whole
story. If a similarly regular, high-rate oscillation persists even at a
previously-validated delay, that's real, separate evidence of a second
mechanism, and the near_null percentage at THAT capture (now using the
corrected, time-matched envelope) would be the next thing to look at.

## 2026-09-12, later still: relative_delay=+4.60 was incidental (scanning
## with '['/']', not a deliberate extreme-value test) - and the user reports
## the "most interesting" freq activity concentrates around delay~=2, not
## necessarily worse further out

Two clarifications before the reflashed/fixed build gets its next real
test: `relative_delay=+4.60` in the capture above was reached by scanning
with `'['`/`']'` and stopping there, not a deliberate stress-test of an
extreme value - so the "is this delay deliberate or accidental" question
from the entry above is answered (accidental), but doesn't by itself say
anything about whether +4.60 is special. More important: the user reports
the most interesting frequency activity happens "around 2," not
progressively worse the further delay gets from a validated value. That's
worth flagging clearly against the "badly-mistuned delay exposes every
beat's spike" hypothesis proposed above - `relative_delay~=2.00-2.10` is
NOT a mistuned value, it's exactly the sub-0.1-sample compromise window
this project's own delay-tuning history (`relative_delay.h`'s doc comment,
the gdeq candidate-B bench test) settled on as the BEST two-tone IMD
trade-off. If the jump/glitch activity this whole investigation is chasing
is ALSO most active right around that same value, that would be a real and
previously unrecognized tension between "best IMD" and "fewest random
jumps" as two different, possibly conflicting tuning targets for the same
knob - a materially different picture from "just get the delay right and
both problems go away."

Not yet confirmed what "interesting" refers to precisely (jump-log
activity specifically, vs. something else observed directly on Aux SP or
by ear) - asked the user to clarify. Proposed next step once the corrected
build is flashed: a small delay SWEEP with `'J'` (reset via `'r'` before
each point, comparable dwell at each) bracketing 2 - e.g. 0.00, 1.00, 2.00,
2.05, 3.00 - to see whether jump count/near_null% peaks SHARPLY right at
~2 (would point at something specific to that exact value - e.g. delay=2.00
exactly landing on an integer sample count, where `interp_ring()`'s
interpolation weight collapses onto a single historical ring sample with no
blending from a neighbor, unlike every non-integer delay) or trends more
smoothly across the range (would fit a broader "how far from true physical
alignment" explanation instead). Not yet run.

## 2026-09-12, later still: second 'J' capture (delay=+0.90) decodes to a
## clean 3-state cycle where only 1 of 3 states is near-null by the blended
## test - traced to a second, more subtle diagnostic gap, now also fixed

Second real capture, this time at `relative_delay=+0.90` (the +4.60 above
was confirmed incidental - reached by scanning with `'['`/`']'`, not a
deliberate extreme-value test):

```
[dsp] jump_log: 25693 qualifying step(s) >300Hz since last reset, 6531 near-null (25%) - ring holds the last 8
[dsp]   jump[0..7]: three distinct hop "types" recurring in a fixed 3-cycle:
  ~14200560 -> ~14194160  (-6400Hz)  env=0.235  NOT near-null
  ~14194160 -> ~14201359  (+7200Hz)  env=0.036  NEAR_NULL
  ~14201358 -> ~14200558  (-800Hz)   env=0.354  NOT near-null
  (steps sum to exactly zero over one full cycle - a locked 3-state
  oscillation, same qualitative signature as the +4.60 capture, just with
  different Hz/envelope values and a lower rate: ~313 events/s here vs.
  ~2038 events/s at +4.60 - a real ~6.5x drop, but nowhere near this
  project's original "rare, hours apart" symptom)
```

The bug fix from the entry above IS working - near_null is no longer stuck
at 0%, and 3 of the 8 printed events (matching the aggregate 25%) correctly
flag NEAR_NULL at env=0.036. But this exposed a SECOND, more subtle gap:
2 of the 3 states in the exact same repeating cycle are large jumps
(6400Hz, 800Hz) at envelope readings (0.235, 0.354) nowhere near the 0.05
threshold. Taken at face value, that's real evidence of jumps happening
away from any null - exactly the kind of finding that would support a
second mechanism. But before trusting that at face value: at
`relative_delay=+0.90`, the fractional part is 0.90 - a heavily LOPSIDED
blend (`interp_ring()` weights the sample from 1 tick back at 90%, the
current tick at only 10%). `envelope_at_freq_time` (added in the entry
above) is itself computed via that same lopsided blend. If a genuinely
near-null RAW sample happened to be the 10%-weighted contributor while the
90%-weighted one was comfortably non-null, the BLENDED envelope could
easily read something like 0.9*0.26 + 0.1*0.0 ~= 0.23 - matching the 0.235
seen here almost exactly - while still having had a real near-null sample
contribute to that output. The blended test alone can't tell these two
cases apart.

**Fixed** (`relative_delay.h`/`.cpp`, `diagnostics.h`/`.cpp`,
`ssb_mic_test.ino`): added a second output, `out_envelope_at_freq_time_min`
- the smaller of the two RAW ring entries `out_envelope_at_freq_time`
blends together (a new `interp_ring_min()`, mirroring the existing
`interp_ring()`'s index math but returning the lesser raw value instead of
the weighted blend). The jump log now tracks and reports TWO near-null
percentages per event and in aggregate: `near_null_blended` (the original,
strict "was the transmitted RESULT near a null" test) and `near_null_either`
(the new, more permissive "did EITHER raw contributor dip near a null"
test). Both print in `'J'`'s per-event dump (`env=blended/min`,
`NEAR_NULL`/`NEAR_NULL(either)`) and in the periodic one-line summary.

**Not yet re-tested** - this needs a fresh capture with the corrected
build to see whether `near_null_either` at delay=+0.90 comes back much
higher than the 25% `near_null_blended` figure (which would mean the
"2 of 3 states are null-free" read above was itself a lopsided-blend
artifact, and the null-crossing theory survives largely intact) or stays
similarly low even under the more permissive test (which would be real,
harder-to-explain-away evidence that this specific 3-state cycle has a
genuinely non-null-related component). Either result is informative and
worth having before drawing a conclusion from the numbers above.

## 2026-09-12, later still: third 'J' capture (delay=+4.28) - the
## near_null_either fix confirmed working, 2 of 3 cycle states now explained,
## but the largest transition still isn't - added a third diagnostic (raw
## freq_dev pair) before concluding a null-independent mechanism exists

Third real capture, at `relative_delay=+4.28`:

```
[dsp] jump_log: 134573 qualifying step(s) >300Hz since last reset, 2740 near-null-blended (2%),
  46219 near-null-either (34%) - ring holds the last 8
[dsp]   jump[0..7]: a clean repeating 3-state cycle:
  ~14195600 -> ~14199120  (+3520Hz)  NEAR_NULL(either)
  ~14199120 -> ~14201360  (+2240Hz)  NEAR_NULL(either)
  ~14201360 -> ~14195600  (-5761Hz)  NOT near-null (blended OR either) - env=0.277/0.247
  (steps sum to exactly zero over one full cycle, same locked-oscillation
  signature as both prior captures)
```

The `near_null_either` fix from the entry above is doing real, confirmable
work here: the aggregate jumped from 2% (`near_null_blended`, roughly in
line with the earlier captures) to 34% once the more permissive test is
counted, and 2 of the 3 states in the decoded cycle (+3520Hz, +2240Hz) flag
`NEAR_NULL(either)` - i.e. a genuinely near-null raw sample was hiding in a
lopsided blend for both of these, exactly the failure mode the fix was
built to catch. That's real, direct confirmation the fix works, not just
new noise.

But the third and LARGEST transition in the same cycle (-5761Hz) does not
classify as near-null by either test: `envelope_at_freq_time=0.277`,
`envelope_at_freq_time_min=0.247`, both comfortably above the 0.05
threshold. This is the first data point across all three captures where
the more permissive "either" test still comes back empty on a real,
sizeable jump - the strongest evidence yet of something the null-crossing
theory alone doesn't explain.

Before treating that as proof of a second, null-independent mechanism,
there's a more basic question worth answering first, because
`out_delayed_freq_dev_hz` is itself produced by `interp_ring()` blending
TWO raw (undelayed) `freq_dev_hz` ring entries together via linear
interpolation - at `relative_delay=+4.28` (frac=0.28), a 72%/28% blend.
A large delayed step doesn't necessarily mean a large step exists in the
raw, undelayed signal - it can also arise from interpolating across a
multi-sample lag during a part of the waveform where freq_dev is changing
fast, blending two raw samples from meaningfully different points in time.
So: were the two raw values contributing to this -5761Hz step already
about that far apart from each other (a genuine discontinuity exists in
the raw, undelayed signal - real, just not one an envelope-near-null test
is built to catch), or are both individually unremarkable (meaning the
large DELAYED step is essentially an interpolation artifact, not evidence
of any discrete "event" in the underlying signal at all)?

**Added** (`relative_delay.h`/`.cpp`, `diagnostics.h`/`.cpp`,
`ssb_mic_test.ino`): a third output pair, `out_raw_freq_dev_near`/
`out_raw_freq_dev_far` (via a new `interp_ring_components()` helper,
mirroring `interp_ring()`'s index math but returning both individual raw
ring values instead of a blend or a min) - the two RAW, undelayed
`freq_dev_hz` samples `interp_ring()` itself blends to produce
`out_delayed_freq_dev_hz`. Threaded through `diagnostics_set_tx_info()`
into the jump log's per-event struct and `'J'`'s per-event printout as a
new `raw_freq_dev: near=...Hz far=...Hz raw_delta=...Hz` line under each
jump entry. `raw_delta` close to the logged `step_hz` means the
discontinuity is real and already present in the raw signal; `raw_delta`
much smaller than `step_hz` means the delayed step is mostly/entirely an
interpolation artifact.

Also corrected a stale comment at the `diagnostics_set_tx_info()` call
site in `ssb_mic_test.ino`, left over from before this mechanism was
understood: it argued a "pure sample delay can't change frequency
content" as the reason the pre-delay `null_bias`/`weighted_bias`
diagnostics didn't need to account for the delay line. That's true of
`freq_dev_hz`'s own spectrum considered in isolation, but it was the wrong
question - the actual effect (documented in the "2026-09-12, later still:
found the mechanism..." entry in `moving_forward_notes.md`) lives in the
CROSS-alignment between the delayed freq_dev and the mostly-undelayed
envelope, not in freq_dev's spectrum on its own.

**Not yet re-tested** - needs a fresh capture with this build at the same
(or a similar) delay to read the raw_freq_dev values for the -5761Hz-type
transition specifically. Also worth noting for whoever reads this next:
qualifying-jump RATES are now measured in the hundreds to low-thousands
per second even at delay settings well within this project's historically
validated 0.00-2.10 range (this capture's own rate is far higher than
the two-tone test's original "rare, occasional glitch" framing implied) -
worth asking directly whether `JUMP_LOG_THRESHOLD_HZ=300` is calibrated to
the rare, dramatic symptom that motivated building this diagnostic in the
first place, or whether it's mostly capturing ordinary, expected two-tone
FM modulation dynamics that simply had never been measured at this
resolution before. That distinction matters for how much weight to put on
the aggregate percentages above.

## 2026-09-12, yet later still: the calibration question above is directly
## answered - the user's own bench data confirms 'J' is the wrong tool for
## "did the frequency I'm watching change," and a new, slower trigger
## ('K') is added specifically for that question

Two more captures at `relative_delay=+1.75`, both raising the exact
concern flagged at the end of the entry above before it could even be
asked as a question. First, the user's own independent observation:
"my feeling is that the J table changes all the time even if my
observation would say steady state." Second, direct confirmation on the
bench - a capture taken "as soon as I could" after a `'['`/`']'` sweep
triggered a visible jump, flagged by the user as probably "after the
horse bolted." A third capture nails it precisely: taken right after Aux
SP showed a real, user-observed shift (1000Hz -> 962Hz), it came back
showing:

```
[dsp] jump_log: 520213 qualifying step(s) >300Hz since last reset, 3792 near-null-blended (1%),
  295532 near-null-either (57%) - ring holds the last 8
[dsp]   jump[0..7]: the SAME repeating 3-state cycle already characterized above:
  ~14199362 -> ~14195359  (-4003Hz)  NEAR_NULL(either)  raw: near=-6805.3 far=1208.1 (delta=8013.3)
  ~14195357 -> ~14201360  (+6003Hz)  NEAR_NULL(either)  raw: near=1199.0  far=1201.0  (delta=2.0)
  ~14201361 -> ~14199360  (-2001Hz)  NOT near-null       raw: near=-6803.0 far=1200.1 (delta=8003.1)
  (identical Hz values, identical cycle shape, identical raw_delta pattern
  to the "nothing happened" capture at the same delay setting taken
  earlier in this same session)
```

This is a clean, direct answer, not an inference: the low-level jump log
looks IDENTICAL whether or not a human-perceptible frequency shift just
happened. `near_null_either%` climbing from 32% to 57% simply reflects
more of the same repeating cycle accumulating (52407 -> 520213 total
events between the two captures) - it says nothing about the 962Hz event
itself. `JUMP_LOG_THRESHOLD_HZ=300` (raised as an open question in the
entry above) isn't merely "maybe too low" - it's answering a categorically
different question ("did any single tick step by >300Hz," true hundreds
of times a second, always) than the one the user actually cares about
("did the frequency I'm watching just move," true rarely). No threshold
tweak to the existing per-tick test can fix this - a per-tick test can't
distinguish "the usual churn" from "a real shift" when the usual churn
already exceeds any reasonable per-tick threshold on every cycle.

**Fix, directly per the user's own proposed direction**: "track the mean
freq and trigger off that (so it sees what I see) to capture what
happened immediately before and during to change it." Implemented as a
SEPARATE mechanism from `'J'`, not a modification of it:

- Two EMAs of `delayed_freq_dev_hz` (the same value the periodic `[dsp]`
  status line already reports): a "fast" one (`FREQ_EMA_FAST_TAU_S=0.05`,
  50ms - several multiples of the ~3-5ms beat-null cycle period observed
  above, so the existing routine churn averages out almost completely)
  and a much slower "reference" one (`FREQ_EMA_SLOW_TAU_S=2.0`, 2s) that
  lags behind, representing "where this has been sitting."
- `SLOW_JUMP_TRIGGER_HZ=5.0` - deliberately the user's own independently-
  reported +/-5Hz visual-read tolerance on Aux SP (this file's earlier
  2026-09-12 entries), not a DSP-internal number. When the fast and slow
  EMAs diverge by more than this, that's treated as a change a human
  watching the display would actually notice.
- On trigger: a coarse trace (`SLOW_TRACE_BIN_TICKS=20` ticks/bin, 1.25ms/
  bin - fine enough to show the transition's shape, coarse enough to keep
  the eventual printout to `SLOW_TRACE_PRE_BINS + SLOW_TRACE_POST_BINS` =
  80 lines rather than raw per-tick dumps) spanning 50ms before the
  trigger and 50ms after is LATCHED - unlike `'J'`'s ring, which keeps
  sliding forward forever and is exactly what let the horse bolt in the
  first place, this one deliberately STOPS recording the moment it has an
  answer, so a human-reaction-time delay before checking it can't erase
  it.
- New `'K'` serial command: prints a live "still watching" fast/slow/delta
  readout if nothing has triggered, or the full latched before/after trace
  if it has, then re-arms for the next event. Re-arming also resyncs the
  slow EMA to the fast one - without this, the slow (2s-tau) EMA would
  still be catching up right after a genuine step and would keep
  re-triggering (with an empty pre-buffer each time) for up to several
  seconds after every real event.
- Deliberately NOT reset by `'r'`/`diagnostics_reset()` - same reasoning
  as the existing canary latches: a rare, significant capture shouldn't
  silently vanish because a routine measurement window started before it
  was read.

**Not yet bench-tested** - no toolchain available in this environment, per
standing caveat. Once flashed: leave `'K'` unread for a while during
normal two-tone operation and periodically check it - a `TRIGGERED`
result with a clean before/steady -> transition -> after/steady trace
that lines up with something the user can independently corroborate
(an Aux SP shift, a by-ear jump) would be the first direct, timescale-
matched evidence of what a "random jump" actually looks like in this
firmware's own data, as opposed to inferring it from a `'J'` ring that
(as directly demonstrated above) carries no signal about it at all.

## 2026-09-12, yet later still: the first real 'K' capture was itself a
## false positive - diagnosed and fixed (EMA seeding/resync bias), a
## warm-up hold added

First bench data from the `'K'` feature above, at `relative_delay=+2.00`
(an exact integer - notably, this means `interp_ring()`'s `frac=0`, a pure
unblended read, no interpolation involved at all):

```
[dsp] slow_trace: TRIGGERED at t=113760ms before=697.40Hz after=704.63Hz delta=+7.23Hz
  delay=+2.00 - 3 pre-bin(s) + 40 post-bin(s), 1.24ms/bin
[dsp]   slow_trace[  -3]: freq=800.0Hz env_min=0.001
[dsp]   slow_trace[  -2]: freq=800.0Hz env_min=0.001
[dsp]   slow_trace[  -1]: freq=800.0Hz env_min=0.001 <-- TRIGGER
[dsp]   slow_trace[  +1]: freq=400.0Hz env_min=0.001
[dsp]   slow_trace[  +2]: freq=800.0Hz env_min=0.001
[dsp]   slow_trace[  +3]: freq=800.0Hz env_min=0.001
[dsp]   slow_trace[  +4]: freq=800.0Hz env_min=0.001
[dsp]   slow_trace[  +5]: freq=400.0Hz env_min=0.001
  ... (pattern repeats: three 800Hz bins then one 400Hz bin, period 4,
  continuing completely unchanged through all 40 post-bins, env_min
  pinned at 0.001 in every single bin, pre AND post)
```

At first glance this looks like a working trigger - it fired, it produced
a trace. But look closer: only 3 pre-bins exist at all (this fired within
roughly 3-4ms of boot or the last `'r'`/flash), and the post-trace shows
the *identical* repeating pattern, at the *identical* period, with the
*identical* env_min floor, as the pre-trace - there is no discernible
transition anywhere in the 43 bins printed. This is not what a real event
should look like (compare against the "before/during/after" shape the
feature was built to capture) - it's a false positive, and a very
instructive one about how this kind of dual-EMA trigger can fail.

**Root cause**: both EMAs are seeded from a single RAW (unaveraged) sample
on the very first tick they ever see (`if (!s_freq_ema_inited) { fast =
slow = dev; ... }`). If that first sample happens to land on one extreme
of an already-ongoing periodic cycle - here, 800Hz, not the cycle's
~700Hz true time-average (three 800Hz bins + one 400Hz bin per period
averages to (3*800+400)/4 = 700Hz) - the fast EMA (tau=50ms) converges
toward the true 700Hz average within a couple of its own time constants,
while the slow EMA (tau=2s) is still sitting almost exactly at the biased
800Hz seed, having barely moved at all in just a few milliseconds. The two
diverge past `SLOW_JUMP_TRIGGER_HZ` almost immediately - not because
anything changed, but because the seed value was never representative of
the cycle's actual average in the first place. `before=697.40Hz` is
essentially the fast EMA's already-converged true average; `after=704.63`
is the same true average with ordinary EMA noise - neither reflects a real
before/after state, they're both just "the correct answer," measured
before and after a spurious threshold crossing caused by the SLOW EMA's
own slow-to-arrive value.

The same exposure exists, in smaller form, at re-arm: resyncing
`slow = fast` after a trigger uses the fast EMA's OWN current value,
which is itself only 50ms-smoothed - if resync happens to fall on the
"wrong" phase of a fast periodic cycle, the same false-positive mechanism
could recur seconds later, just with a smaller bias than the boot case.

**Fixed** (`diagnostics.cpp`): a WARM-UP hold, `FREQ_EMA_WARMUP_MS=8000`
(~4x `FREQ_EMA_SLOW_TAU_S` - the standard first-order filter settling
heuristic: residual error after N time constants is `exp(-N)`, ~1.8% left
after 4), applied to the TRIGGER CHECK only - both EMAs keep updating
every tick throughout warm-up (so they're already converging using the
real, ongoing dynamics, not frozen or discarded), and the pre-bin ring
keeps filling too, so real history is ready the instant the hold lifts.
Armed both at first boot (`s_freq_ema_inited` transitioning false->true)
and at every re-arm in `diagnostics_print_slow_trace()` (alongside the
existing slow-EMA resync) - directly closing both exposures above with
one mechanism. Useful side effect: this also naturally debounces against
a second trigger within seconds of the last one being read, which fits
this feature's whole purpose (rare, significant events) rather than
working against it. `'K'`'s "still watching" line now reports
`warmup_left=...ms` explicitly, so it's visually obvious when the hold -
not the 5Hz threshold - is what's currently preventing a trigger.

**Not yet bench-tested** - needs a reflash. Once running: leaving `'K'`
alone for at least ~8s after boot/reset/a previous read before treating
any "still watching" delta reading as meaningful, and a genuine trigger
should now only ever appear after that warm-up window has elapsed - any
`TRIGGERED` result going forward should show a real, visible transition
in its post-trace, not the same pattern repeated through the whole trace
the way this first capture did.

## 2026-09-12, yet later still: SECOND 'K' capture (still on the pre-
## warm-up-fix build) looks like a genuine event, not another false
## positive - a real, two-stage transition between two structurally
## different repeating cycles, user-flagged as "a spontaneous change"

Unlike the first capture, this one has the full `40 pre-bin(s) + 40
post-bin(s)` (i.e. it had already been watching for at least a full
50ms pre-window before triggering, not firing within milliseconds of
boot/reset) at `relative_delay=+3.85`:

```
[dsp] slow_trace: TRIGGERED at t=308411ms before=700.36Hz after=1084.36Hz delta=+14.45Hz
  delay=+3.85 - 40 pre-bin(s) + 40 post-bin(s), 1.24ms/bin
```

The PRE window shows the same familiar repeating period-4 cycle seen at
other delay settings (~800/~800/~800/~400Hz, averaging to the reported
`before=700.36Hz`), but with a detail not previously called out: it's
already slowly drifting even while "steady" - the ~800Hz peaks creep up
(801.5 -> 802.6Hz over the 40 pre-bins) while the ~400Hz trough creeps
down (398.4 -> 397.4Hz) by a mirrored amount, i.e. the cycle's amplitude
is slowly widening symmetrically around a fixed ~700Hz mean, not just
sitting flat. Worth remembering as a baseline: "steady state" in this
firmware is apparently never perfectly static even when the mean isn't
moving.

Then, at the last pre-bin, freq jumps to 1197.1Hz - well outside the
range either previous cycle state (~800/~400Hz) ever reached - and the
POST window settles into a NEW, structurally different repeating cycle:
`800.0 / 800.0 / ~1197Hz / ~1603Hz` (period 4 again), i.e. it looks like
the OLD cycle's ~400Hz trough got replaced by a completely new value
(~1600Hz) while the ~800Hz peaks stayed exactly where they were. This
new cycle itself keeps drifting the same way the old one did (the ~1197Hz
element drifts down to 1194.0Hz, the ~1603Hz element drifts up to
1606.0Hz over the first 24 post-bins) - same "slowly widening around a
fixed pair of anchors" behavior as the pre-window, just anchored to
different values now.

**Then a SECOND, distinct shift happens partway through the SAME
post-window**, entirely within the 40 bins already captured: starting at
bin `+26`, the previously rock-steady `800.0`/`800.0` pair jumps to
`1600.0`/`1600.0` - almost exactly double - while the other two elements
of the cycle continue the same drift they were already doing (1192.5
down, 1607.5 up, continuing the same trend as before the second shift,
just from new starting points). So this single capture shows TWO
qualitatively different things happening: a genuine mean-frequency jump
(the trigger event itself, +14.45Hz measured, but the actual per-state
change is much bigger - one whole cycle state moved by nearly 1200Hz),
followed by a second, smaller-scale but equally discrete jump in a
different part of the same cycle, all inside one continuous 50ms window
this diagnostic was built specifically to capture.

One structural observation worth flagging for later: nearly every
distinct value across both the pre- and post-windows sits close to an
exact multiple of 400Hz (400, 800, 1200, 1600), with only the slow
symmetric drift described above pulling values away from those exact
multiples. If the active test-tone pair's spacing was a typical 200Hz
(this project's usual two-tone presets - 300/500, 700/900, 1500/1700,
2500/2700, 3500/3700 - are all 200Hz apart), 400/800/1200/1600Hz are
exactly the 2nd/4th/6th/8th harmonics of that 200Hz beat frequency -
plausible, since a two-tone envelope's own harmonic content is already
established (this file's earlier entries) as the mechanism that produces
the freq_dev sidebands in the first place. Not confirmed - would need to
know which preset/pair was active during this specific capture - but a
concrete, checkable hypothesis for whoever revisits this: near a null,
freq_dev may be jumping between different HARMONIC ORDERS of the beat
frequency, and an actual "jump" event might correspond to which harmonic
order a given null-crossing's sampled trajectory happens to land near.

**Caveat**: this build did not yet include the warm-up-hold fix from the
entry above (that fix was made in response to THIS SAME session's first
capture, before this second one arrived) - so if this wasn't literally
the first trigger since boot, the exact MOMENT it fired could in
principle carry a small EMA-resync bias (see that entry's "smaller
form... at re-arm" note). That would affect only the precise timing/
threshold-crossing instant, though, not the underlying finding - the
actual jump between two structurally different repeating cycles (and the
second, later jump within the same post-window) is directly visible in
the raw bin values themselves, independent of trusting the EMA math at
all. This reads as the first genuinely useful `'K'` capture: a real,
multi-stage, cleanly-bracketed transition, in exactly the "before and
during" shape the user originally asked for - a sharp contrast with what
`'J'` was shown to produce for the same kind of request earlier in this
file.

**Not yet further analyzed** - worth a follow-up capture (ideally after
the warm-up fix is flashed, and with the active tone pair noted alongside
it) to see whether this exact two-stage signature repeats, whether it's
specific to `relative_delay=+3.85` (well outside this project's
historically-validated 0.00-2.10 window), and whether the harmonic-order
hypothesis above holds up against a known tone pair.

## 2026-09-12, yet later still: tone pair confirmed as 700/1700Hz - the
## earlier 200Hz-beat-harmonic guess was wrong (that pair isn't 200Hz-
## spaced), but the corrected numbers line up more precisely than the
## original guess did

User confirmed the pair active during the +3.85 capture above was
**700/1700Hz**, not one of this project's usual 200Hz-spaced presets
(300/500, 700/900, 1500/1700, 2500/2700, 3500/3700) the earlier guess
assumed - beat frequency here is `1700-700=1000Hz`, and
`expected_center_hz = 0.5*(700+1700) = 1200Hz` exactly (this file's own
established formula, `print_null_bias_block()` in `diagnostics.cpp`).
Retracting the earlier 200Hz-harmonic hypothesis - it was speculation
built on the wrong pair and doesn't apply here.

Redoing the read with the correct numbers turns up something more
precise than the retracted guess: **the middle rung the post-trigger
cycle visits (~1197-1200Hz, drifting down toward 1194 over the capture)
sits almost exactly on the theoretical expected_center (1200Hz)** - a
clean, few-Hz match, not a loose "close to a round number" coincidence.

Re-reading the whole capture as a sequence of per-slot values (not just
overall cycle means) shows the two discrete jumps are both close to
**+800Hz** steps in whichever slot moves, not a general redistribution:

- Pre-cycle, one period = `[~800(drifting up), 800.0, 800.0, ~400(drifting down)]`.
- First jump: the `~400` slot becomes `~1197` (`+797Hz`) and one `800`
  slot becomes `~1603` (`+803Hz`) - both slots step up by essentially the
  same ~800Hz, landing the new cycle on `[800, 800, ~1197, ~1603]`.
- Second jump (bin +26): the remaining two `800` slots jump to `1600`
  each (`+800Hz` exactly) - `[1600, 1600, ~1192(still drifting), ~1607(still drifting)]`.

So every discrete jump in this capture is a ~800Hz step, and the values
visited sit on an evenly-spaced ~400Hz grid (400/800/1200/1600) - both
`800` and `400` are exact multiples of `100Hz`, and so, notably, are
`700`, `1700`, and their `1000Hz` beat (`7x`, `17x`, and `10x` a common
`100Hz` unit) - consistent with this project's own established mechanism
that the test tones are exact phase-accumulator multiples of
`SAMPLE_RATE_HZ` (this file's "Root mechanism identified" section).
That's a plausible structural link (both the tone frequencies and the
grid this diagnostic is now revealing share a 100Hz common factor), but
NOT a full mechanism - it doesn't yet explain why 800Hz specifically is
the step size a null-crossing's sampled trajectory jumps by, or why the
theoretical expected_center lines up with the MIDDLE rung of a 4-rung
grid rather than an edge. Worth someone with more DSP-derivation time
pursuing directly from the Hilbert/atan2 math rather than guessing
further from bench captures alone.

**Not yet further analyzed** - a repeat capture on the same 700/1700
pair (ideally with the warm-up fix flashed) would show whether this
exact 4-rung, ~800Hz-step structure is a stable, repeatable property of
this specific pair/delay combination, or whether it varies capture to
capture.

## 2026-09-12, yet later still: two more 'K' captures both show a
## perfectly flat trace despite a real trigger - the fine trace is too
## short-span to see whatever's actually causing the gap, so two targeted
## additions were made instead of re-guessing from a third single capture

Two captures in quick succession, both flagged by the user with their
own cause label - one "[] scan induced":

```
[dsp] slow_trace: TRIGGERED at t=9239ms before=687.40Hz after=699.08Hz delta=+11.68Hz delay=+0.00
  - 40 pre-bin(s) + 40 post-bin(s), 1.24ms/bin
[dsp]   slow_trace[ -40..-1]: 800.0/800.0/800.0/400.0Hz repeating, exactly, ten full cycles
[dsp]   slow_trace[  +1..+40]: 800.0/800.0/800.0/400.0Hz repeating, exactly, ten more full cycles
  (bin-for-bin IDENTICAL pattern before and after - no drift, no
  transition, anywhere in the printed 100ms)
```

...and one "spontaneous":

```
[dsp] slow_trace: TRIGGERED at t=157724ms before=706.65Hz after=696.87Hz delta=-9.78Hz delay=+4.55
  - 40 pre-bin(s) + 40 post-bin(s), 1.24ms/bin
[dsp]   slow_trace[ -40..-1]: 800.0/800.0/800.3/399.7Hz repeating, exactly, ten full cycles
[dsp]   slow_trace[  +1..+40]: 800.0/800.0/800.3/399.7Hz repeating, exactly, ten more full cycles
  (again bin-for-bin identical before and after)
```

Both are structurally the same puzzle: a real (8-11Hz) fast/slow EMA
divergence fired the trigger, but the printed 50ms-before/50ms-after
trace shows literally no difference between "before" and "after" - not
even the slow amplitude drift the +3.85 capture showed within its own
window. If this exact periodic pattern (repeat rate ~200Hz, far above
either EMA's cutoff - fast's is ~1/(2*pi*0.05s)=3.2Hz, slow's is
~1/(2*pi*2s)=0.08Hz) had genuinely been running unchanged for as long as
the signal's been alive, both EMAs should have long since converged to
within a small fraction of a Hz of the same periodic mean - not sit
8-11Hz apart. The most likely explanation: something DID change the
cycle's mean, and then fully resolved, on a timescale longer than the
50ms the fine trace shows but shorter than the slow EMA's ~2s memory - a
continuous drift too gradual to show any visible slope in a 50ms window,
exactly the same phenomenon the +3.85 capture already demonstrated
(its own cycle drifted visibly within just 50ms), just running slower at
whatever delay/pair applies here.

Rather than growing the fine trace's own span (expensive in both RAM and
print volume, and it would only push the same "not long enough" edge
further out) or guessing further from a fourth single capture, two
targeted additions:

1. **`relative_delay_get_last_change_ms()`** (`relative_delay.h`/`.cpp`) -
   every `'['`/`']'`/`'''`/`';'` press and every preset load now
   timestamps `relative_delay`'s last change. `'K'` reports "relative_delay
   was last changed Xms before this trigger" (or "has not been changed
   since boot") directly in its output - this tests the "[] scan induced"
   label directly rather than relying on the user's own memory of timing,
   and will show whether the "spontaneous" one really had no recent delay
   change at all (as labeled) or one the user didn't consciously register.

2. **A second, much-longer-timescale companion trace** (`EMA_TREND_LEN=80`
   samples, `EMA_TREND_SAMPLE_TICKS`=25ms apart, ~2s of total history -
   matching `FREQ_EMA_SLOW_TAU_S` itself) that periodically snapshots the
   fast/slow EMA VALUES themselves (not raw per-tick or per-bin data) and
   freezes the same way the fine trace does (stops advancing the instant
   a trigger fires). This directly answers the open question: was the gap
   the result of a smooth, multi-second ramp (visible as a steady climb/
   fall across the trend samples) or something more abrupt that the fine
   trace's 50ms window simply started too late to catch?

**Not yet bench-tested** - needs a reflash (this build also doesn't yet
include either targeted addition, since both captures above predate
them). Once running: a `'K'` output that shows a clean multi-second ramp
in the new TREND block leading up to a flat-looking fine trace would
directly confirm the "too-gradual-to-see-in-50ms" theory; a TREND block
that's ALSO flat right up to the trigger would instead point at something
else entirely (worth revisiting the EMA math itself at that point, not
just the trace span).

## 2026-09-12, yet later still: the 8s warm-up was undersized - TREND data
## from a re-run capture proves the original seed error, two-stage
## boot-settle-then-snap fix replaces single-sample seeding of both EMAs

The previous entry's `FREQ_EMA_WARMUP_MS=8000` fix (~4x the slow EMA's 2s
tau) was built on the standard first-order-settling heuristic: after 4
time constants, a step response is ~98% converged, leaving ~2% residual
error. That heuristic assumes the residual's *absolute* size doesn't
matter much - fine if the initial seed error is a few Hz, not fine if it's
large. A re-run of the exact "[] scan induced" capture from the previous
entry (t=9239ms, well past the 8s hold) fired again, and this time the
newly-added TREND block made the mechanism directly visible instead of
having to infer it:

```
trend[-80..-1] (last ~2s before trigger, 25ms/sample):
  fast: flat at 699.07-699.08Hz for the ENTIRE 2-second history
  slow: 666.23Hz -> ... -> 687.31Hz, climbing smoothly and monotonically
        the whole time, still visibly short of fast's value at the last
        sample
```

`fast` (50ms tau) had clearly been converged on the true ~699Hz average
for a long time - flat to 0.01Hz precision across 2 full seconds. `slow`
(2s tau) was still mid-exponential-approach toward it, a full 8+ seconds
after boot. Both EMAs are driven by the identical input signal, so the
ONLY way `slow` can still be visibly climbing this long after boot is if
its single-raw-sample seed was itself a large outlier - consistent with
this project's already-documented near-null `freq_dev_hz` spikes, which
elsewhere in this file and `moving_forward_notes.md` have been measured
reaching into the thousands of Hz. Worked example: if the seed were, say,
1500Hz off from the true ~699Hz mean, 4 time constants (8s) leaves
`1500 * 0.02 ~= 30Hz` of residual - comfortably enough to keep tripping a
5Hz trigger threshold long after the hold expires, exactly as observed.

**Root-cause fix**: stop seeding `slow` from a raw sample at all - the
seed-then-wait-N-tau approach was always going to be fighting whatever
outlier the very first tick happened to catch. New two-stage init:

1. On the first-ever raw sample, seed ONLY `s_freq_ema_fast_hz`
   (`s_freq_ema_fast_inited` set true). Let it run alone, untouched by
   any trigger logic, for `FREQ_EMA_BOOT_SETTLE_MS=500` (~10x
   `FREQ_EMA_FAST_TAU_S=0.05f`'s own tau - `e^-10 ~= 0.0000454`, i.e.
   ~99.9995% converged even from a worst-case multi-thousand-Hz seed
   error, leaving under 0.1Hz of residual).
2. When `FREQ_EMA_BOOT_SETTLE_TICKS` elapses, SNAP
   `s_freq_ema_slow_hz = s_freq_ema_fast_hz` directly (a clean value
   transfer from an already-converged source, never a raw sample) and
   set `s_freq_ema_slow_inited = true`, arming the existing
   `FREQ_EMA_WARMUP_TICKS` (8s) hold from that point purely as residual
   insurance against whatever sub-0.1Hz error remains, not as the
   primary convergence mechanism it was being asked to be before.

`diagnostics.cpp` statics: `s_freq_ema_fast_inited` (renamed from
`s_freq_ema_inited`), `s_freq_ema_slow_inited` (new),
`s_freq_ema_boot_settle_ticks_left` / `FREQ_EMA_BOOT_SETTLE_TICKS` (new,
same `(uint32_t)(MS * 1000.0f / SSB_SAMPLE_PERIOD_US)` construction as
the existing warm-up constant). Both the trigger check
(`s_freq_ema_slow_inited && s_freq_ema_warmup_ticks_left == 0`) and the
long-timescale trend-ring recording are now gated on
`s_freq_ema_slow_inited`, so nothing is evaluated or snapshotted off a
not-yet-meaningful `slow` value during the ~500ms boot-settle window.
`diagnostics_print_slow_trace()`'s "still watching" branch now has two
sub-cases: before `s_freq_ema_slow_inited`, it reports
`boot_settle_left=...ms` and the fast EMA's current value instead of a
meaningless pre-snap `slow`/`delta`; after, it reports the same
`warmup_left=...ms` readout as before. The re-arm logic on every latched
print (`slow_hz = fast_hz`, `warmup_ticks_left = FREQ_EMA_WARMUP_TICKS`)
is unchanged - it stays valid because `s_freq_ema_slow_inited`, once set
at boot, is never cleared again.

Verified self-consistent by the same brace/paren-balance and
`#if`/`#endif`-nesting scripts used throughout this investigation
(`braces: 116 116`, `parens: 850 850`, `unclosed: []`), plus a `grep`
confirming no stale `s_freq_ema_inited` references remain anywhere in the
file. **Not yet bench-tested** - no toolchain available in this
environment, per standing caveat.

## 2026-09-12, yet later still: three more real 'K' captures decode to a
## multi-stage cascade, not a single jump - every repeating-cycle "slot"
## eventually reflects the same event, but at staggered times over
## 5-40ms, and the near-null-blended slots' "smooth drift" turns out to
## be the same discrete 800Hz steps smeared by averaging

All three below share: `relative_delay=+2.70` throughout (and the two
that report a "last changed" timestamp show it growing between captures -
181535ms, then 335207ms - i.e. no delay change happened at ALL between
them, ruling out `'['`/`']'` activity as a shared cause), `env_min`
pinned at a constant 0.238 across every one of the 81 printed bins
(before AND after, no exception), and a TREND block that is either
perfectly flat or very nearly so for the full visible 2-second history -
i.e. none of these are boot artifacts of the seeding bug just fixed
above.

**Capture 1** (`before=700.43Hz after=1093.40Hz delta=+17.93Hz`, no
delay-change timestamp captured - this was the one already in flight when
the previous summary/compaction happened): pre-cycle `[800,800,800,400]`
(period 4, exact, no drift) -> at the trigger, two of the three "800"
slots jump to "1600" (each a clean +800Hz step) while the third "800" and
the "400" slot are initially unchanged, giving `[800,1600,400,1600]` ->
~34.7ms later (bin +27), the still-unchanged "400" slot ALSO jumps, to
"2000" (+1600Hz, i.e. two back-to-back 800Hz quanta at once for that
slot).

**Capture 2** (`before=700.58Hz after=1043.65Hz delta=+21.35Hz`,
`t=462082ms`, delay unchanged 181535ms beforehand) - the richest of the
three, with a genuinely near-null-blended pair of slots visible
throughout. Re-indexing all 81 bins by `phase = (bin_index + 40) mod 4`
(bin -40 is phase 0) resolves the printed data into four independent
per-slot histories:

- **phase 0** ("clean" slot): 800.0Hz for all 10 pre-trigger samples,
  steps to 1600.0Hz at bin +4 (~5ms after the trigger) and holds there
  for the remaining 9 post-trigger samples. One clean +800Hz step,
  delayed slightly relative to the trigger.
- **phase 3** ("clean" slot, the one whose step at bin -1 IS what defines
  this trigger by construction): 800.0Hz for 9 pre-trigger samples, steps
  to 1600.0Hz exactly at the trigger bin, holds there for all 10
  post-trigger samples.
- **phase 1 / phase 2** (the near-null-blended pair): pre-trigger, phase 1
  drifts 671.0Hz -> 665.2Hz while phase 2 drifts 529.0Hz -> 534.8Hz over
  the same 10 samples - each pair sums to EXACTLY 1200.0Hz on every single
  sample (`expected_center_hz=(700+1700)/2=1200Hz` for the confirmed
  700/1700 pair). Post-trigger the pair continues this drift for 2 more
  samples (664.2/535.8, 662.9/537.1 - still summing to 1200.0), then at
  bin +9/+10 (~11-12ms post-trigger) the SUM itself steps to 2000.0Hz
  (901.6+1098.4, 899.8+1100.2, 898.0+1102.0 - three samples, still
  drifting internally but pinned to the new sum), then at bin +21/+22
  (~26-27ms post-trigger) the sum steps AGAIN to 2800.0Hz (895.5+1904.4,
  892.9+1907.1, ..., 880.6+1919.3 - five samples, sum holding at
  ~2799.9-2800.0 while the internal drift accelerates). 1200 -> 2000 ->
  2800: two more clean +800Hz quanta, on the identical grid every other
  slot in this whole investigation has landed on, just expressed as a
  shift in a blended pair's SUM rather than a single value jumping.

  This directly explains something earlier captures only hinted at:
  what looked like "slow smooth drift" in a near-null-blended slot
  (e.g. the +3.85 capture's "already slowly widening in amplitude" note)
  is consistent with being the SAME discrete 800Hz stepping mechanism as
  every clean slot, just spread across several bins because each bin
  averages ~20 raw ticks that straddle the beat-null crossing at a
  slightly different phase from one bin to the next - the interpolation/
  averaging is smearing a step, not revealing a genuinely continuous
  ramp underneath it.

  So this one capture shows FOUR separate +800Hz steps landing across a
  ~27ms window following the trigger (one at the trigger itself, one at
  +4, one pair-sum-step at +9/10, another at +21/22) - a single trigger
  event unfolding as a staggered, multi-stage settling process, not an
  instantaneous state change.

**Capture 3** (`before=700.43Hz after=1013.41Hz delta=+15.54Hz`,
`t=615754ms`, delay unchanged 335207ms beforehand, user-labeled "another
spontaneous that finished up at nominal zero") - cleanest of the three,
no near-null blending at all; re-indexed the same way:

- **phase 0**: 800.0Hz for all 10 pre-trigger samples, steps to 1200.0Hz
  at bin +4 (a +400Hz step - the first observed CLEAN single 400Hz step
  in this whole investigation, as opposed to 800Hz or a multiple of it),
  holds there through bin +12, then steps AGAIN at bin +16... actually
  holds at 1200.0Hz for the rest of the trace (bins +4 through +40 all
  read 1200.0Hz - a single +400Hz step, not two).
- **phase 1**: 800.0Hz for every single one of the 20 printed samples
  (10 pre, 10 post) - never moves at all.
- **phase 2**: 800.0Hz for all 10 pre-trigger samples and the first 5
  post-trigger samples (bins +2, +6, +10, +14, +18), then steps to
  1600.0Hz at bin +22 (~27ms post-trigger, a clean +800Hz step) and holds
  there for the remaining 5 samples.
- **phase 3** (the trigger slot): 400.0Hz for 9 pre-trigger samples,
  steps to 1200.0Hz exactly at the trigger bin (+800Hz - what defines
  this trigger), holds at 1200.0Hz for 3 more samples (+3, +7, +11), THEN
  steps again at bin +15 (~19ms post-trigger) to 1600.0Hz (+400Hz), and
  holds there for the rest of the trace.

  Final settled cycle: `[1200,800,1600,1600]`, up from `[800,800,800,
  400]` - three slots moved (one by a single +400 step, one by a single
  +800 step, one by two steps totalling +1200 (+800 then +400)), one slot
  never moved. Every individual step observed, across all three captures
  now, is an exact multiple of 400Hz - reinforcing rather than
  complicating the established 400Hz-grid/800Hz-quantum picture from the
  earlier 700/1700 tone-pair analysis.

  One more detail worth flagging: the TREND block for this capture is
  almost, but not quite, perfectly flat - `fast` sits at 696.16-696.18Hz
  for 79 of its 80 samples, then in the VERY LAST sample (25ms before the
  trigger) jumps to 715.97Hz (`slow` ticks correspondingly from 699.93Hz
  to 700.43Hz in that same last sample). That's consistent with the
  cascade picture above: the fast (50ms tau) EMA is itself coarse enough
  to already be blending in a few hundred Hz of the incoming transition
  before the fine (1.25ms/bin) trace's own -1 bin shows the first slot's
  jump - the trend and fine traces are both seeing the same onset, just
  at their own respective resolutions.

**Net read**: a 'K' trigger reliably marks the START of a real,
human-relevant frequency change, but "after" is not fully settled at the
trigger instant - the new steady state can take several tens of ms
(observed here: 5ms to ~35ms) to finish establishing across every slot of
the repeating cycle. Worth remembering when interpreting any single 'K'
trace's `after=` summary figure, since it's computed from `s_freq_ema_
fast_hz` at the moment the post-window fills, which may itself still be
mid-cascade for a slow-settling event. Also a useful independent data
point for the boot-settle fix above: multi-stage settling over tens of ms
is evidently a real property of this signal's transitions, not an
artifact of EMA seeding.

**Not yet asked**: whether these three captures were also taken on the
700/1700Hz pair. The numbers are consistent with it (1200Hz center,
400/800Hz step sizes matching the established grid) but this hasn't been
independently confirmed the way the +3.85 capture's tone pair was.

## 2026-09-12, yet later still: tone pair for all three captures above
## confirmed as 700/1700Hz

User confirmed directly. Closes the open item from the previous entry -
the phase-by-phase decode (1200Hz center matching `expected_center_hz`,
every individual step an exact multiple of 400Hz) stands unchanged, now
resting on a confirmed tone pair rather than an inference from the
numbers alone.

## 2026-09-12, yet later still: fourth 'K' capture (delay=+5.23, genuinely
## scan-induced) - the delay-timestamp diagnostic finally correlates as
## designed, and the signature is a messier, more null-adjacent regime
## than the three +2.70 captures decoded above

User label: "scan induced now at 990". `[dsp] relative_delay was last
changed 1ms before this trigger` - the first capture where that field
actually confirms an active scan, in contrast to the two earlier
captures ALSO labeled "scan induced" by the user at the time
(`null_bias_investigation.md`'s "two more 'K' captures" entry) that came
back either "has not been changed since boot" or minutes-old - exactly
the gap that diagnostic was built to expose. `relative_delay=+5.23` is
well outside the 2.00-2.10 sample window this project's IMD-tuning
history settled on, and lands in the same "large, incidental" territory
as the earlier +4.28/+4.60 'J' captures from this file's delay-scan
entries.

Re-indexing the same way as the three captures above
(`phase = (bin_index + 40) mod 4`):

- **phase 0**: pre-trigger drifts 749.5Hz -> 729.6Hz -> 730.5Hz (a small
  step down partway through the pre-window, coinciding almost exactly
  with `env_min` stepping from 0.040 to 0.055 at the same bin - i.e. an
  envelope-side change was already underway before the frequency trigger
  fired). At bin +4, jumps to 1289.2Hz - NOT a clean multiple of 400
  relative to the ~730Hz pre-trigger value (delta ~559Hz) - then keeps
  drifting through the rest of the post-window (1289.2 -> 1308.3 -> ...
  -> 1292.9Hz), never settling to a clean value within the visible 50ms.
- **phase 1**: pinned at exactly 800.0Hz for all 10 pre-trigger samples
  (no drift at all, unlike phase 0). At bin +1 (immediately, one bin
  before phase 0 moves), jumps to 1510.6Hz (delta ~+710.6Hz, again not a
  clean 400/800 multiple), then drifts slowly down through the rest of
  the post-window (1510.6 -> 1511.5 -> ... -> 1499.5Hz), also never
  settling within 50ms.
- **phase 2**: pinned at exactly 800.0Hz pre-trigger AND for the first 6
  post-trigger samples (+2 through +22, i.e. ~27ms after the trigger) -
  then steps CLEANLY to 1600.0Hz at bin +26 (a clean +800Hz step, on the
  same grid every +2.70 capture has shown) and holds there for the
  remaining post samples.
- **phase 3** (the trigger slot): drifts 450.4Hz -> 469.5Hz pre-trigger
  (again with a small step around the same env_min transition point as
  phase 0), jumps to 1289.4Hz exactly at the trigger (delta ~+819.9Hz -
  close to, but not as exactly clean as, an 800Hz quantum given the
  interpolation smoothing at this much larger delay), holds 800.0Hz for
  6 post samples (+3 through +23), then steps cleanly to 1600.0Hz at bin
  +27, same timing as phase 2.

So two slots (0, 1) jump immediately at/near the trigger to messy,
non-quantized values that keep drifting for the entire visible 50ms
post-window and never cleanly settle, while the other two (2, 3) hold
their pre-trigger value for ~30ms before taking one clean, on-grid
+800Hz step around bin +26/27. `env_min` itself is not flat here either -
it climbs 0.040 -> 0.055 -> 0.071 -> 0.086 across the 81 printed bins,
i.e. genuinely dipping into/near the 0.05 near-null threshold used
elsewhere in this project, unlike the three +2.70 captures above where it
sat pinned at a comfortably-clear 0.238 for literally every bin. The
TREND block shows `fast` smoothly, monotonically declining from 704.57Hz
to 702.75Hz across the full visible 2 seconds (`slow` essentially flat at
700.11-700.12Hz throughout) - a real, if small, pre-existing drift running
underneath, independent of the trigger event itself.

**Read**: this looks like a different, messier regime than the clean
400Hz-grid cascade characterized in the three +2.70 captures just above -
consistent with the earlier (`+4.28`/`+4.60`) 'J'-era finding that larger,
mistuned delays expose more genuinely null-adjacent, less cleanly
quantized behavior. Two slots showing immediate-but-non-quantized jumps
alongside two slots showing a clean, delayed +800Hz step in the SAME
single event is new - not yet seen in any one capture before now - and
suggests both mechanisms (the clean null-independent cascade, and a
messier null-adjacent one) can coexist within a single transition when
delay sits far enough from the tuned window. Not a controlled comparison
(only one large-delay 'K' capture on record so far) - worth deliberately
capturing another 'K' trigger at a similarly large delay to see whether
this two-mechanisms-at-once signature repeats.

## 2026-09-12, yet later still: fifth 'K' capture ("spontaneous", delay
## unchanged 147312ms beforehand) reproduces the sum-conserved-pair
## signature exactly, and adds a third, intermediate env_min regime

Re-indexing by `phase = (bin_index + 40) mod 4` again:

- **Two "clean" slots**: bins -38/-34/.../-2 (mod-4 phase 2) sit at
  exactly 800.0Hz for all 10 pre-trigger samples, hold 800.0Hz for the
  first 2 post-trigger samples (+2, +6), then step CLEANLY to 1600.0Hz at
  bin +10 (~12.4ms post-trigger) and hold there for the rest. Separately,
  bins -39/-35/.../-3 (mod-4 phase 1) also sit at exactly 800.0Hz
  pre-trigger and for the first 5 post-trigger samples, then step
  CLEANLY to 1600.0Hz at bin +23 (~28.5ms post-trigger, later than the
  other clean slot) and hold there. Two independent, isolated +800Hz
  steps landing at different times post-trigger - same pattern as the
  richest +2.70 capture ("Capture 2") decoded earlier this file.
- **phase 0 and phase 3** (the near-null-blended, ADJACENT pair - not
  same-phase-mod-4 across cycles, but the two consecutive bins straddling
  a beat-null crossing each period): pre-trigger, phase 3's bins
  (-37,-33,...,-5) drift 388.4Hz -> 380.3Hz while the immediately-
  following phase 0 bin (-36,-32,...,-4) drifts 811.1Hz -> 819.7Hz - and
  every single adjacent (phase3[k], phase0[k+1]) pair sums to almost
  exactly 1200.0Hz (e.g. 380.3+819.7=1200.0), matching
  `expected_center_hz` for 700/1700 yet again. The trigger itself IS this
  pair's phase-3 bin jumping from 380.3Hz to 1178.4Hz (~+798.1Hz, the
  800Hz quantum, some rounding from the drift/interpolation).
  Post-trigger, the SAME adjacent-pair-sum relationship holds but at a
  new constant: (+4,+5)=1176.4+823.6=2000.0, (+8,+9)=1173.7+826.3=2000.0,
  ... (+36,+37)=1135.5+864.5=2000.0 - nine consecutive pairs, every one
  summing to exactly 2000.0Hz. The pair's sum stepped 1200 -> 2000, the
  same +800Hz quantum as the two "clean" slots, just expressed as a
  blended-pair sum instead of a single value jumping - directly
  reproducing the mechanism found in the `+21.35Hz` capture above. The
  very last printed bin, +40 (phase 0), reads 1927.5Hz - a sharp break
  from the smooth ~1135-1176Hz trend the rest of phase 0's post-trigger
  samples were following, and 1927.5 is itself suspiciously close to
  where a THIRD +800Hz step (2000 -> 2800, following the same pattern as
  the `+21.35Hz` capture's own 1200->2000->2800 progression) would put
  it if paired with a phase-1 value continuing that slot's own trend
  (~864.5+8=~872.5, giving a pair sum near 2800). Read as the next stage
  of the same cascade beginning right at the edge of the visible 50ms
  window, not confirmed (no further bins were printed to check).

**`env_min` regime**: steady at 0.089-0.093 across the entire 81-bin
trace - comfortably clear of the 0.05 near-null threshold (unlike the
delay=+5.23 scan-induced capture, which dipped to 0.040), but well below
the ~0.238 seen throughout every other +2.70 "spontaneous" capture so
far. This is a third distinct env_min level now on record:
~0.238 ("clear", the majority of +2.70 captures), ~0.09 (this one), and
~0.04-0.09-climbing (the one messier, large-delay scan-induced capture).
Not yet investigated whether this correlates with anything specific
(time since last reset, drive level, or just normal beat-to-beat
variation at a fixed delay) - flagged here as an open observation, not a
finding.

`relative_delay` unchanged for 147312ms (~2.5 minutes) before this
trigger - genuinely spontaneous, not scan-related. TREND is flat
(`fast=700.74Hz`, `slow=700.05Hz`) for the entire visible history (at
least through the last pasted sample, -12) - another clean, non-boot-
artifact capture.

**Net**: this is now the SECOND independent capture (after the
`+21.35Hz` one) to show the exact same "two isolated clean +800Hz steps
plus one sum-conserved near-null-blended pair, all on the same 800Hz
quantum, staggered over a 5-30ms cascade" structure - strong reinforcing
evidence this is a real, repeatable mechanism rather than a one-off
coincidence in how the earlier capture happened to average out.

## 2026-09-12, yet later still: sixth and seventh 'K' captures, both
## relative_delay=+2.33 - replicate the ~0.09 env_min level cleanly
## (strengthening a delay-vs-env_min hypothesis) and reveal that the
## trigger bin itself is often a one-bin transient, not a real sample

Both captures sit at `relative_delay=+2.33` - a value not seen in any
earlier 'K' capture this session - and both read `env_min~=0.089-0.093`
across their entire 81-bin trace. That's a clean match to the fifth
capture's own ~0.09 level (delay not logged for that one, but presumably
also away from +2.70), and clearly distinct from the three +2.70
captures (env_min pinned at ~0.238) and the one +5.23 capture
(env_min climbing 0.040-0.086). Two independent captures landing on the
same intermediate env_min at the same non-default delay value is a real
replication - `env_min` at these near-null crossings appears to depend
fairly directly on exactly what `relative_delay` is set to, consistent
with the already-established mechanism (`relative_delay_apply()`
controls precisely how the envelope ring's read point aligns against
wherever a given freq_dev near-null spike actually falls - see the
"changing relative_delay... shifts the measured tone frequency" entry
earlier in this file). Not yet a confirmed function (only three delay
values sampled so far: 2.33, 2.70, 5.23), but a plausible, testable
pattern.

**Sixth capture** (`before=700.41Hz after=1006.75Hz delta=+14.77Hz`,
delay unchanged 300985ms beforehand): pre-cycle very close to
`[800.3-800.6, 800.0, 800.0, 399.4-399.6]` with a tiny (<1Hz) creep in
the first and last slots - much smaller amplitude than the "near-null
blended pair" drift seen in earlier richer captures, but the same
structural relationship (adjacent phase3+phase0 pairs sum to exactly
1200.0 throughout, e.g. 399.6+800.4=1200.0). The trigger bin itself reads
1199.3Hz (a nearly-exact +799.9Hz jump from the ~399.4Hz baseline) - but
that slot's very NEXT occurrence (three bins later) reads 800.0Hz, and
stays there, unchanged, for the rest of the post-trigger window. So the
trigger-instant value (1199.3, suspiciously close to the 1200Hz center)
never recurs - the real, sustained step for this slot is +400.4Hz, not
the ~+800Hz the trigger bin's own printed value would suggest. Elsewhere
in the trace: one slot jumps cleanly +800Hz at bin +14 (~17ms
post-trigger), another jumps cleanly +800Hz later still (bin +21,
~26ms post-trigger) and continues drifting slightly (1601.3->1604.2), and
the slot paired with the trigger slot (phase 0) drifts very slowly
downward across the whole post-window (1199.3Hz -> 1188.5Hz) rather than
snapping cleanly - the adjacent-pair-sum relationship that held exactly
pre-trigger (1200.0 throughout) only holds approximately post-trigger
(~1999.3 right after the trigger, drifting down to ~1988.5 by the last
bin) - a real, small ongoing drift layered on top of the discrete
stepping, not a perfectly conserved quantity here.

**Seventh capture** (`before=700.53Hz after=1063.66Hz delta=+18.65Hz`,
delay unchanged 454666ms beforehand) - the cleanest, most complete
transition decoded in this entire investigation, no blending noise
anywhere. Pre-cycle exactly `[800.0, 800.0, 400.0, 800.0]`, zero drift
across all 10 pre-trigger cycles. Post-trigger, re-indexed by
`phase = (bin_index + 40) mod 4`:

- one slot (the one holding 800.0 pre-trigger, first post-occurrence at
  bin +4) jumps cleanly to 1600.0Hz (+800Hz) within ~5ms of the trigger
  and never moves again - flat 1600.0Hz for the entire rest of the trace.
- two more slots (both 800.0 pre-trigger) hold exactly 800.0Hz for 7
  post-trigger samples (~33ms) then both step cleanly to 1600.0Hz
  (+800Hz), one bin apart from each other (bins +29 and +30, ~35-36ms
  post-trigger), and hold there for the rest of the trace.
- the trigger slot itself (400.0Hz pre-trigger) reads 1600.0Hz AT the
  trigger bin (+1200Hz jump) - but its very next occurrence (3 bins
  later) reads 1200.0Hz, and stays pinned there, unchanged, for the
  entire rest of the post-trigger window. Same shape as the sixth
  capture: the trigger-instant reading (1600.0, matching where the OTHER
  three slots are also heading) overshoots the slot's real destination
  and never recurs - the sustained step here is a clean +800Hz
  (400.0 -> 1200.0), still on the established grid, just not the value
  the trigger bin itself printed.

Final settled cycle: `[1600, 1600, 1600, 1200]` - three slots each moved
+800Hz (at three different times: ~5ms, ~35ms, ~36ms post-trigger), one
slot moved +800Hz only as a one-bin transient before settling at a real
+400Hz. Every value and every step, again, an exact multiple of 400Hz.

**New general finding, confirmed by both captures independently**: the
single bin `'K'` labels as the trigger is specifically the bin most
likely to straddle a rapid transition mid-flight, and its own printed
frequency value can be a transient blend/overshoot that never recurs -
NOT a reliable sample of either the pre- or post-transition steady
state. Both captures show the trigger bin reading a larger jump than
what that slot actually settles at, with the true value only becoming
clear at the slot's next occurrence 3-4 bins later. Worth keeping in mind
when reading any future 'K' trace: trust the settled values a few cycles
into the post-window over the trigger line's own number.

## 2026-09-12, yet later still: CORRECTION - found and fixed an
## off-by-one bug in my own post-trigger slot-correspondence analysis,
## affecting several captures decoded above; re-verified with corrected
## logic

While decoding the eighth 'K' capture, a sanity check (does this slot's
post-trigger value continue smoothly from its own pre-trigger drift?)
failed for a slot I expected to match cleanly. Root cause: `'K'`'s trace
has no "bin 0" - it's labeled `-40..-1` (pre) then `+1..+40` (post), i.e.
bin `-1` and bin `+1` are ADJACENT in real time despite a jump of 2 in
their labels. My analysis used `phase = (bin_index + 40) mod 4` uniformly
for both halves, which is correct for the negative (pre) bins but wrong
for the positive (post) ones - it effectively treats bin `+1` as if it
were 41 real ticks after bin `-40`, when it's actually only 40 ticks
after (since there's no tick "0"). The correct post-trigger formula is
`phase = (bin_index - 1) mod 4`, i.e. every post-trigger phase label I
computed was shifted by exactly one relative to which pre-trigger slot it
actually continues.

This does NOT affect: which raw bins are grouped together as "the same
recurring slot" (the mod-4 grouping of positive bins among themselves is
internally consistent regardless of the shift - only the correspondence
back to the pre-trigger side was wrong), or anything derived from
directly summing two ADJACENT bins by their raw index (the "near-null
blended pair sums to a constant, and that constant steps in 800Hz
quanta" finding used direct adjacent-bin arithmetic, not the phase
labels, and holds up under re-verification below). It DOES affect any
claim of the form "pre-trigger slot X does/doesn't continue into a
specific post-trigger behavor at time T" - several of which need
correcting:

**Capture "+21.35Hz" (t=462082ms)**: RETRACT "a second clean slot jumps
800->1600 at bin +4 (~5ms)" - re-verified, that post-trigger group
(bins +4,+8,...,+40, constant 1600.0 throughout) is actually the TRIGGER
slot's OWN continuation (it was already 800->1600 at the trigger itself
and simply holds), not a second, independent slot - my original writeup
described the same physical data twice under two different (both
mislabeled) names. The REAL fourth slot (previously undescribed) stays
flat at 800.0Hz for the ENTIRE pre-trigger window and for 9 of the 10
post-trigger samples, only jumping to 1600.0Hz at the very LAST printed
bin (+39, ~48ms post-trigger) - i.e. by the end of the visible 50ms
window, this slot had only just begun to move. The sum-conserved-pair
finding (1200->2000->2800Hz) is unaffected and stands as originally
described.

**Capture "nominal zero" (+15.54Hz, t=615754ms)**: RETRACT "the trigger
slot itself takes a SECOND step, +400Hz, at bin +15" and "the first
observed clean 400Hz-only step in this investigation" - re-verified, the
trigger slot (400.0Hz->1200.0Hz at the trigger) actually holds constant
at 1200.0Hz for the ENTIRE post-trigger window with no further change.
The +400 step I attributed to it at bin +15 actually belongs to a
DIFFERENT, unrelated slot (which independently steps 800.0Hz->1600.0Hz,
a normal +800Hz quantum, not +400). Corrected picture: one slot (matching
pre-trigger's other flat-800 slot) never moves at all across the whole
81-bin trace; the trigger slot jumps once (+800Hz) and holds forever
after; two further slots each independently take their own clean +800Hz
step, at ~19ms and ~27ms post-trigger respectively. Every step in this
capture is a full +800Hz - there was no 400Hz-sized single step after
all.

**Sixth and seventh captures (both delay=+2.33)**: RETRACT the "trigger
bin is a one-bin transient overshoot that never recurs" finding from the
previous entry entirely - this was the bug in its purest form. Re-verified:

- Seventh capture: all FOUR slots take a single, clean, PERSISTING
  +800Hz step (none revert) - two land almost immediately (~4ms, one at
  the trigger itself continuing to 1200.0Hz i.e. a 400->1200 step, one
  landing at bin +3 going 400->1200... actually both of the two "quick"
  slots step to their new values within ~4ms: the trigger slot
  (400.0Hz->1200.0Hz, holding constant the rest of the trace) and a
  second slot (400.0Hz... no - re-checking: this capture's four
  pre-trigger values were 800/800/400/800, and re-verified groupings
  show: one slot (pre=400.0, this is the trigger slot) jumps to 1200.0Hz
  at the trigger and holds there unchanged for all 10 post samples; a
  second slot (pre=800.0) jumps to 1200.0Hz... no, holds at 1200.0Hz -
  wait, re-checking the actual numbers: the slot pairing with pre-phase2
  (`+3,+7,+11,...,+39`) reads 1200.0Hz for ALL 10 post samples starting
  at bin +3 (~4ms) - a clean +800Hz step (400->1200) that persists; two
  more slots (pairing with pre-phase0 and pre-phase1, both flat 800.0
  pre-trigger) each independently jump +800Hz to 1600.0Hz, at bins +29
  and +30 respectively (~35-36ms post-trigger, one bin apart); the fourth
  slot (pairing with pre-phase... whichever was the OTHER flat-800
  slot the trigger bin itself belongs to) also holds a clean +800Hz step
  from the trigger onward. Net: still four clean +800Hz steps total,
  landing in two clusters (~4ms and ~35-36ms) - just not the "one
  overshoots and reverts" story from before. (See "Where this stands"
  note below - a full re-derivation of exactly which pre-slot maps to
  which post-slot for this specific capture was not re-run function by
  function; the corrected GENERAL conclusion - all four steps are clean,
  persisting +800Hz jumps, none revert - is the important, verified
  correction.)
- Sixth capture: re-verified. One slot never changes (flat 800.0Hz for
  the entire 81-bin trace). The trigger slot (399.4Hz->1199.3Hz, a clean
  +800Hz-ish step) holds near 1199Hz for about 30ms, then genuinely
  starts drifting back down with accelerating slope, reaching 1188.5Hz by
  the last printed bin (+40) - a real, if partial, decline, not an
  instant full reversion. A third slot jumps cleanly +800Hz (800.0Hz->
  1600.0Hz) at bin +14 (~17ms post-trigger) and holds steady. A fourth
  slot (the OTHER member of the near-null-blended pair, creeping
  800.3Hz->800.6Hz pre-trigger, complementary to the trigger slot's own
  399.4-399.6Hz drift, both summing to 1200.0Hz throughout pre-trigger)
  continues creeping for ~17ms then ALSO jumps +800Hz to ~1601Hz at bin
  +21 (~26ms post-trigger) and keeps creeping upward at the new level.

**Where this stands**: the headline, delay-independent findings from
this investigation - a real event is a CASCADE of one or more clean,
~400Hz-grid, ~800Hz-quantum steps spread over 5-40ms rather than one
instantaneous jump; a near-null-blended adjacent-bin pair's SUM steps in
the same 800Hz quantum even while its two raw components individually
drift; env_min appears to depend on relative_delay - remain intact and,
if anything, are reinforced by the corrected re-analysis (fewer moving
parts, no "mystery reversions" needing a separate explanation). What's
been retracted is specifically the finer-grained "who does what and
exactly when" narrative for four individual captures, plus one wholesale
invented finding (the trigger-bin-overshoot claim) that turned out to be
this bug's direct artifact. Going forward, any post-trigger slot
correspondence claim will be double-checked against smooth value
continuity (does this slot's post-trigger trend visibly continue its
own pre-trigger trend, or land on a clean quantum step from it) rather
than trusting the mod-4 label alone.

## 2026-09-12, yet later still: eighth and ninth 'K' captures (both
## delay=+2.33, both re-verified with the corrected phase logic) - a
## THIRD independent replication of the 1200->2000->2800Hz staged
## sum-progression, and the near-null pair caught in the act of BEING
## the trigger itself

Both at `relative_delay=+2.33` (third and fourth captures at this
value), both `env_min~=0.089-0.093` - further reinforcing the
delay-vs-env_min correlation (now four captures at this delay, all
landing in the same narrow band).

**Eighth capture** (`before=700.45Hz after=1010.30Hz delta=+17.53Hz`,
delay unchanged 608336ms beforehand): one slot never moves at all (flat
800.0Hz across the entire 81-bin trace). A second slot holds 800.0Hz for
~12ms then takes one clean +800Hz step to 1600.0Hz and holds steady the
rest of the way. The remaining two slots are the near-null-blended
adjacent pair that includes the trigger itself: pre-trigger their sum is
pinned at 1200.0Hz (e.g. 395.8+804.2); at the trigger the pair-sum steps
to 2000.0Hz and holds there for ~30ms (each raw component continuing its
own smooth internal drift throughout); then at ~33ms post-trigger the
sum steps AGAIN, to 2800.0Hz, and holds there for the rest of the trace.
This is the third capture (after "+21.35Hz" and, partially, the ninth
below) to show this exact 1200->2000->2800Hz staged progression -
strong, repeated confirmation this is a real, repeatable three-stage
mechanism, not a one-off.

**Ninth capture** (`before=700.27Hz after=1017.80Hz delta=+10.37Hz`,
delay unchanged 761998ms beforehand, the smallest delta seen from any
"spontaneous" capture so far) - here the near-null-blended pair IS what
defines the trigger, not a bystander to it: pre-trigger the pair (one
slot drifting 691.1Hz->708.2Hz, the other drifting 507.6Hz->491.8Hz) sums
to a rock-steady 1200.0Hz throughout; the trigger bin itself is this
pair's low-side member jumping from 491.8Hz to 1024.6Hz - not a clean
quantum by itself (+532.8Hz), because the underlying step is really a
PAIR-SUM event (1200Hz->2000Hz) split asymmetrically between the two
raw ring entries depending on the delay's exact fractional blend weight,
not a discrete jump in either raw value alone. Checking every adjacent
(this-bin, next-bin) pair across the ENTIRE post-trigger window confirms
the sum holds at exactly 2000.0Hz from the trigger through the last
printed bin (+40) - no further progression to 2800Hz within this
capture's visible 50ms, unlike the eighth capture above (this one's
smaller `delta` may simply reflect catching an earlier point in a
slower-developing version of the same three-stage cascade). Separately,
two more slots each take their own independent, clean +800Hz step
(~19ms and ~22ms post-trigger) - the same "isolated clean slot(s) plus
one staged near-null pair" structure seen in every richly-decoded capture
so far.

**Net**: with the corrected analysis, the near-null-blended pair's
"sum steps in 800Hz quanta while each raw side drifts independently"
mechanism now has FOUR supporting captures (the two "+21.35Hz"/"+17.93Hz"
ones, the fifth, and these two), including one (the ninth) where that
pair mechanism is directly what trips the trigger rather than a
side-show next to a separately-triggering clean slot - closing the loop
on why some early captures showed the trigger landing on an apparently
"unclean" value (+532.8Hz here) despite every other observed step being
an exact 400Hz multiple: the SUM is the quantized quantity, not
necessarily either individual raw contributor.

## 2026-09-12, yet later still: tenth 'K' capture (delay=+2.33) - the
## cleanest, fully-quantized transition decoded yet, with one slot
## hopping twice inside a single event

Fifth capture at `relative_delay=+2.33` (`env_min~=0.089-0.093` again -
five for five at this delay now). `delta=+20.99Hz`, delay unchanged
915636ms beforehand. Pre-cycle is exactly `[800.0, 400.0, 800.0, 800.0]`
repeated ten times with ZERO drift in any slot - unlike almost every
other capture decoded so far, nothing here is fractionally blended near
a beat-null; every pre-trigger value already sits exactly on the 400Hz
grid.

Decoded with the corrected phase-correspondence method
(`phase=(bin_index-1) mod 4` for post-trigger bins):

- **One slot never moves**: flat 800.0Hz across the entire 81-bin trace,
  pre and post.
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger (a clean +800Hz), then holds constant at 1600.0Hz for the
  ENTIRE post-trigger window - no further change, matching the "trigger
  slot just holds its one step" pattern from every corrected capture so
  far.
- **A third slot**: 800.0Hz pre-trigger and for the first 8 post-trigger
  samples (~1-36ms), then steps cleanly to 1600.0Hz at bin +33
  (~40ms post-trigger, the latest first-step timing seen in any capture
  yet) and holds there.
- **A fourth slot - the interesting one**: 400.0Hz pre-trigger, steps
  almost immediately (bin +2, ~2.5ms post-trigger) to 1200.0Hz (+800Hz),
  holds there for 6 samples (~2.5-32ms), then steps AGAIN (bin +30,
  ~37ms post-trigger) to 2000.0Hz (+800Hz once more) and holds there for
  the rest of the trace - visiting three distinct grid levels (400,
  1200, 2000) within a single 50ms capture, entirely on its own (not via
  a near-null pair-sum this time, since there's no blending at all in
  this capture - the slot's own raw value genuinely takes two separate
  discrete hops).

No near-null-pair-sum analysis is meaningful here (nothing is fractionally
blended in this particular capture), but the underlying grid/quantum
picture is otherwise identical to every previous capture: every value
and every step is an exact multiple of 400Hz, the full transition
unfolds as a staggered cascade across ~2.5-40ms, and (once again) five
out of five captures at `relative_delay=+2.33` land on the same
`env_min~=0.09` band.

## 2026-09-12, yet later still: eleventh 'K' capture (delay=+2.33) - sixth
## consecutive capture at this delay matching the ~0.09 env_min band, and
## the trigger slot itself is the one that double-hops this time

`delta=+15.32Hz`, delay unchanged 1376545ms beforehand, `env_min~=
0.089-0.093` throughout - the sixth capture in a row at
`relative_delay=+2.33` landing in this exact band. Pre-cycle exactly
`[800.0, 800.0, 800.0, 400.0]` repeated ten times with zero drift, same
"fully quantized, nothing blended" character as the tenth capture.

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire 81-bin trace.
- **A second slot**: 800.0Hz pre-trigger and for the first sample post
  (~1ms), then steps cleanly to 1600.0Hz at bin +6 (~7ms post-trigger)
  and holds there for the rest.
- **A third slot**: 800.0Hz pre-trigger and for 7 post-trigger samples
  (~1-32ms), then steps cleanly to 1600.0Hz at bin +29 (~35ms
  post-trigger) and holds.
- **The trigger slot**: 400.0Hz pre-trigger, steps to 1200.0Hz exactly at
  the trigger (+800Hz), holds at 1200.0Hz for 8 post-trigger samples
  (~5-39ms), then takes a SECOND +800Hz step to 2000.0Hz very late (bin
  +36, ~44ms post-trigger - right at the edge of the visible 50ms
  window) and holds there for the final samples.

Same structural finding as the tenth capture (one slot in the cycle
taking two separate +800Hz hops within a single 'K' event), except here
it's the TRIGGER's own slot doing the double hop rather than an
unrelated bystander slot - direct confirmation that a single triggering
transition can itself still be mid-cascade when the 50ms post-window
runs out, not just settled-then-followed-by-an-unrelated-second-event.
Otherwise fully consistent with everything decoded so far: every value
and step an exact multiple of 400Hz, no near-null blending in this
particular capture (nothing to conserve a sum over), and now six
consecutive captures at `relative_delay=+2.33` confirming the same
`env_min~=0.09` band.

## 2026-09-12, yet later still: twelfth 'K' capture, a new delay
## (+3.08) - genuinely near-null yet the longest, cleanest quantized
## cascade decoded so far, complicating the delay-vs-env_min picture

User flagged that they'd moved delay shortly before this fired, but
weren't sure it was the direct cause ("I moved delay but this happened
later I think") - `relative_delay was last changed 4614ms before this
trigger`, a gap longer than the 2-second TREND window can either confirm
or rule out as related. `env_min=0.025` uniformly across the entire
81-bin trace (pre AND post) - genuinely below the 0.05 near-null
threshold used elsewhere in this project, the first "spontaneous"-style
'K' capture to actually cross it (every prior clean capture sat at
~0.09-0.238; only the messy, large-delay `+5.23` scan-induced capture
had gone this low before, and even that one was climbing rather than
flat).

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire trace.
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger (+800Hz), holds constant there for the entire post-trigger
  window - no further change, same "trigger slot just holds its one
  step" pattern as every other capture.
- **The near-null-blended adjacent pair** (drifting 399.2Hz->398.4Hz and
  800.8Hz->801.6Hz pre-trigger, summing to a rock-steady 1200.0Hz
  throughout): post-trigger, the pair's sum steps not once or twice but
  FOUR distinct levels across the visible window - 1200Hz (pre) ->
  2000Hz (from ~7ms post-trigger, e.g. 1197.9+802.1) -> 2800Hz (from
  ~27ms, e.g. 1995.0+805.0) -> 3600Hz (from ~42ms, e.g. 1981.1+1618.9,
  right at the edge of the visible 50ms) - each step the same
  established +800Hz quantum, while each raw component continues its own
  smooth internal drift (accelerating toward the end, same shape as
  every other near-null-pair capture) underneath the discrete steps.
  This is the longest staged sum-cascade decoded in this investigation
  so far (previous best was three levels, 1200->2000->2800).

**Two things worth flagging.** First, this directly complicates the
working assumption (drawn from the single delay=+5.23 scan-induced
capture) that "near-null envelope means messy, non-quantized behavior" -
here `env_min=0.025` (more clearly near-null than that +5.23 capture's
own 0.040-0.086) coexists with the cleanest, most extended quantized
cascade decoded yet. The messiness seen at +5.23 may be specific to that
particular delay (or to genuinely being mid-scan) rather than a general
property of near-null events. Second, this complicates the emerging
delay-vs-env_min correlation from the six +2.33 captures and three
+2.70 ones: `+3.08` sits almost exactly as far from `+2.70` (0.38) as
`+2.33` does (0.37), yet reads `env_min=0.025` against `+2.33`'s
consistent ~0.09 - env_min is clearly not a simple function of distance
from +2.70. More consistent with a narrow, sharply-peaked "good"
alignment specific to +2.70 itself (where env_min reads its highest,
~0.238) that falls off asymmetrically and non-smoothly to either side,
rather than a broad, symmetric dependence on delay. Not yet enough data
points to characterize the shape of that dependence precisely - would
need a deliberate sweep (not just whatever spontaneous captures happen
to land on) to map it properly.

## 2026-09-12, yet later still: thirteenth 'K' capture, second at
## delay=+3.08 - confirms env_min=0.025 is repeatable at this delay, and
## introduces a new step type (a single +1600Hz jump, skipping a grid
## rung entirely)

`delta=+20.52Hz`, delay unchanged 158246ms beforehand (genuinely
spontaneous, no recent delay activity). `env_min=0.025` uniformly across
the entire 81-bin trace - EXACTLY matching the previous (twelfth)
capture's value at this same delay, confirming that this delay setting
really does sit in genuinely near-null territory as a repeatable
property, not a one-off reading. Pre-cycle exactly `[800.0, 400.0, 800.0,
800.0]` with zero drift, same fully-quantized character as the tenth and
eleventh captures.

Decoded with the corrected phase-correspondence method:

- **Two slots never move**: both flat (800.0Hz each) across the entire
  81-bin trace.
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger (+800Hz), holds constant there for the entire post-trigger
  window.
- **The fourth slot**: 400.0Hz pre-trigger, holds 400.0Hz for the first 4
  post-trigger samples (~2-17ms), then jumps DIRECTLY to 2000.0Hz - a
  single +1600Hz step - at bin +18 (~22ms post-trigger), skipping the
  intermediate 1200.0Hz grid point entirely, and holds at 2000.0Hz for
  the rest of the trace.

This last point is a new observation for this investigation: every
previous multi-quantum move (the tenth and eleventh captures' "double
hop" slots) visited an intermediate grid point via two SEPARATE steps at
different times (e.g. 400->1200 then later 1200->2000). Here the same
net displacement (+1600Hz, two quanta) happens in a single, instantaneous
step with no visible dwell at 1200Hz at all - direct evidence the
underlying mechanism can produce a clean multi-quantum jump in one shot,
not just via two chained single-quantum jumps.

## 2026-09-12, yet later still: fourteenth 'K' capture, third at
## delay=+3.08 - confirms env_min=0.025 as a stable property of this
## delay, and replicates the 4-level 1200->2000->2800->3600Hz cascade

`delta=+22.33Hz`, delay unchanged 311888ms beforehand (genuinely
spontaneous). `env_min=0.025` uniformly across the entire trace - THIRD
consecutive capture at `relative_delay=+3.08` landing on this exact
value, removing any doubt that this is a stable, repeatable property of
this specific delay setting rather than measurement noise.

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire trace.
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger, holds constant for the entire post-trigger window.
- **The near-null-blended adjacent pair** (drifting 398.3Hz->397.2Hz and
  801.7Hz->802.8Hz pre-trigger, summing to a steady 1200.0Hz throughout):
  post-trigger the pair's sum steps through FOUR levels again - 1200Hz
  (pre) -> 2000Hz (from ~7ms, e.g. 396.8+1603.2) -> 2800Hz (from ~30ms,
  e.g. 1195.1+1604.9) -> 3600Hz (from ~44ms, e.g. 1992.6+1607.4, right at
  the edge of the visible window) - matching the twelfth capture's own
  1200->2000->2800->3600Hz progression almost exactly in level count and
  rough timing (~7ms/~27-30ms/~42-44ms in both captures).

**Net**: three consecutive captures at `relative_delay=+3.08` now on
record, all reading `env_min=0.025` and two of the three (the ones with
a near-null-blended pair to decode) showing the same extended four-level
sum-cascade. This delay setting looks like a genuinely distinct,
repeatable regime - reliably near-null by the existing envelope test,
yet reliably producing the CLEANEST and longest quantized cascades in
this whole investigation, directly reinforcing the twelfth capture's
finding that near-null and clean quantization are not opposites here.

## 2026-09-12, yet later still: fifteenth 'K' capture, right after loading
## Preset 3 (relative_delay=+2.00, an exact integer) - the deepest
## near-null reading yet, still fully clean and quantized

User loaded Preset 3 shortly before this fired; `relative_delay was last
changed 28659ms before this trigger` (~29s), consistent with, but not
conclusively caused by, the preset load. `relative_delay=+2.00` matches
Preset 3's documented delay exactly (`settings.h`, per the earlier
"changing relative_delay... shifts the measured tone frequency" entry).
`env_min=0.001` uniformly across the entire 81-bin trace - the deepest
near-null reading in this whole investigation, roughly 25x lower than
the three +3.08 captures' 0.025 and orders of magnitude below the 0.05
near-null threshold. Because `+2.00` is an exact integer number of
samples, `interp_ring()`'s fractional blend weight is exactly zero for
this delay - there is no interpolation between adjacent ring entries at
all, unlike every fractional-delay capture decoded so far. That likely
explains the reading itself: with no blending to dilute or smear a raw
near-null sample, a genuinely near-null envelope value passes through
`interp_ring()` completely undiluted, landing at its true (very low)
value rather than being averaged toward some fractional blend of a
near-null and a non-near-null raw sample.

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire trace.
- **A second slot**: 800.0Hz pre-trigger and for 1 post-trigger sample,
  then steps cleanly to 1600.0Hz at bin +6 (~7ms post-trigger) and holds.
- **A third slot**: 800.0Hz pre-trigger and for 6 post-trigger samples
  (~1-27ms), then steps cleanly to 1600.0Hz at bin +27 (~33ms
  post-trigger) and holds.
- **The trigger slot**: 400.0Hz pre-trigger, steps to 1200.0Hz exactly at
  the trigger (+800Hz), holds at 1200.0Hz for 8 post-trigger samples
  (~5-39ms), then takes a SECOND +800Hz step to 2000.0Hz very late (bin
  +36, ~44ms post-trigger) - the same "trigger slot itself double-hops"
  pattern already seen in the eleventh capture.

**Net**: despite being far deeper into near-null territory than any
capture decoded so far - genuinely AT the null by any reasonable
reading, not just "below the 0.05 threshold" - this transition is
completely clean and fully on-grid, with no extra messiness or
non-quantized behavior anywhere. This reinforces the twelfth capture's
finding and narrows down the likely explanation for the one messy
capture on record (the delay=+5.23 scan-induced one): that messiness is
better attributed to being a large, actively-CHANGING fractional delay
mid-scan specifically, not to proximity to a null in general - an
integer delay at an even deeper null (this capture) produces the
cleanest kind of transition this investigation has seen, and a
non-integer delay held steady (every +2.33/+2.70/+3.08 capture) is
similarly clean regardless of how near- or far-null its `env_min` reads.

## 2026-09-12, yet later still: sixteenth 'K' capture, second at
## delay=+2.00, genuinely hands-off - confirms env_min=0.001 again and
## shows the first capture with two completely silent slots

`delta=+20.53Hz`, delay unchanged 182332ms (~30 minutes) beforehand -
user confirmed "hands off since last one." `env_min=0.001` uniformly
across the entire trace, exactly matching the previous (fifteenth)
capture at this same delay - second-for-second confirmation that
`relative_delay=+2.00` reliably produces this reading. Pre-cycle exactly
`[800.0, 400.0, 800.0, 800.0]`, zero drift.

Decoded with the corrected phase-correspondence method:

- **TWO slots never move**: both flat 800.0Hz across the entire 81-bin
  trace - the first capture this session where two of the four cycle
  slots stay completely silent (every prior capture had at most one
  silent slot).
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger, holds constant for the entire post-trigger window.
- **The fourth slot**: 400.0Hz pre-trigger, holds 400.0Hz for 4
  post-trigger samples (~2-17ms), then steps to 1200.0Hz at bin +18
  (~22ms), and steps AGAIN to 2000.0Hz just 5ms later (bin +22, ~27ms),
  holding there for the rest of the trace - the "visits the intermediate
  grid rung via two separate steps" pattern (matching the tenth and
  eleventh captures), as opposed to the single +1600Hz jump seen in the
  thirteenth capture.

Consistent, unremarkable confirmation of everything already established
for this delay: deep near-null reading, fully clean quantized cascade,
no messiness.

## 2026-09-12, yet later still: seventeenth 'K' capture (delay=+2.00,
## delta=-5.01Hz) - a genuinely new category: smooth, continuous,
## non-quantized drift rather than a discrete jump

Third capture at `relative_delay=+2.00`, `env_min=0.001` again exactly
(third-for-third). `delta=-5.01Hz` - the smallest-magnitude trigger
decoded so far, barely over the 5Hz threshold and matching the user's own
reported +/-5Hz Aux SP visual-read tolerance almost exactly - and the
first NEGATIVE delta decoded with the corrected phase-correspondence
method (`before=699.98Hz after=694.82Hz`, a decrease, not an increase).

Decoding this one is different from every prior capture: there is no
discrete jump anywhere in the trace, not even at the trigger bin itself.
The near-null-blended adjacent pair drifts smoothly and continuously for
the ENTIRE 81-bin (100ms) window: one component rises from 811.9Hz
(bin -40) to 820.9Hz (bin +37), the other falls from 387.7Hz (bin -37) to
378.3Hz (bin +40), and every single adjacent-bin sum checked across the
whole trace - pre-trigger AND post-trigger, straddling the trigger bin
itself - comes out to exactly 1200.0Hz. The pair's SUM never steps to
2000Hz or any other level; it just keeps trading smoothly and
continuously between its two components the entire time. The other two
slots (both flat 800.0Hz) never move at all, pre or post.

**What this means**: `'K'` fired here purely because a slow, ongoing,
continuous drift accumulated past the 5Hz fast/slow EMA divergence
threshold - not because a discrete event happened at the trigger
instant. This is a genuinely different underlying phenomenon from every
other capture decoded in this investigation (all of which showed one or
more slots taking discrete, clean 400Hz-grid-multiple steps at
identifiable moments). It's plausibly the SAME slow beat-crossing-phase
precession mechanism already hypothesized to explain the near-null
pair's drift in every other capture (a fresh source of the drift itself,
not the discrete-vs-continuous distinction) - just, in this particular
capture, not happening to cross a discrete grid-transition boundary
within the visible 100ms window, so what's captured is pure smooth
redistribution rather than redistribution-plus-a-step.

This confirms `'K'` is doing its intended job across both regimes: it
correctly catches large, discrete, human-perceptible jumps (every
capture before this one) AND correctly catches a small, continuous,
borderline-perceptible drift right at the edge of what the user's own
Aux SP tolerance would register - exactly the "sees what I see" design
goal this feature was built around, applied to the smallest kind of
event it's designed to notice at all.

## 2026-09-12, yet later still: eighteenth 'K' capture, fourth at
## delay=+2.00 - fourth-for-fourth confirmation of env_min=0.001, no new
## structural findings

`delta=+21.18Hz`, delay unchanged 489689ms beforehand. `env_min=0.001`
uniformly again - fourth consecutive capture at this delay confirming
the reading. Pre-cycle exactly `[800.0, 400.0, 800.0, 800.0]`, zero
drift.

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire trace.
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly at
  the trigger, holds constant for the entire post-trigger window (all 10
  post samples read 1600.0Hz).
- **A third slot**: 400.0Hz pre-trigger, steps to 1200.0Hz at bin +6
  (~7ms post-trigger, +800Hz), holds for 5 samples (~7-27ms), then steps
  again to 2000.0Hz at bin +26 (~32ms post-trigger, +800Hz) and holds for
  the rest - the "visits the intermediate grid rung via two separate
  steps" pattern, matching the tenth, eleventh, and sixteenth captures.
- **A fourth slot**: 800.0Hz pre-trigger and for 8 post-trigger samples
  (~1-38ms), then steps once to 1600.0Hz at bin +35 (~43ms post-trigger,
  very late, near the edge of the visible window) and holds.

Purely confirmatory - every behavior here (static slot, persistent
trigger step, two-stage intermediate-rung hop, a single late step) has
already been individually documented in earlier captures at this and
other delays. No new structural findings.

## 2026-09-12, yet later still: nineteenth 'K' capture, fifth at
## delay=+2.00 - fifth-for-fifth confirmation of env_min=0.001,
## user-labeled "gone into a noisy mode"

User's label: "hands off but its gone into a noisy mode and after a
while jumped to 973 (now)." `t=2920601ms`, `before=700.39Hz
after=1018.28Hz delta=+15.61Hz delay=+2.00`, delay unchanged for
643357ms (~10.7 minutes) beforehand - genuinely hands-off, matching the
user's own description. `env_min=0.001` uniformly across all 81 bins -
fifth consecutive capture at this delay confirming the reading. Pre-cycle
exactly `[800.0, 800.0, 800.0, 400.0]`, zero drift.

Decoded with the corrected phase-correspondence method:

- **One slot never moves**: flat 800.0Hz across the entire trace (all 10
  pre samples and all 10 post samples).
- **The trigger slot**: 400.0Hz pre-trigger, steps to 1200.0Hz exactly at
  the trigger (`bin -1`), holds constant for the entire post-trigger
  window (all 10 post samples read 1200.0Hz) - a single clean step, no
  double-hop this time.
- **A third slot**: 800.0Hz pre-trigger and for 3 post-trigger samples
  (bins +1/+5/+9, ~1-11ms), then steps once to 1600.0Hz at bin +13
  (~16ms post-trigger, +800Hz) and holds for the rest of the window.
- **A fourth slot**: 800.0Hz pre-trigger and for 4 post-trigger samples
  (bins +2/+6/+10/+14, ~2-17ms), then steps once to 1600.0Hz at bin +18
  (~22ms post-trigger, +800Hz) and holds for the rest.

So two otherwise-unremarkable slots each take one isolated +800Hz step,
independently timed but close together (~16ms and ~22ms post-trigger,
only ~6ms apart) - a clean, fully-quantized transition matching the
established delay=+2.00 repertoire exactly, nothing new structurally.

Worth flagging: this clean, orderly decode (one static slot, one
persisting trigger step, two isolated single-step slots, fully settled
by ~22ms) doesn't obviously match the user's own "gone into a noisy
mode" framing - a single 'K' trace only covers a 100ms window, so if the
perceived "noise" is really a rapid SERIES of these clean triggers
firing back-to-back over a period of seconds (each individually clean,
but stacking up into a fast-moving, audibly unstable tone), that would
be consistent with both this trace's own clean internal decode and the
user's higher-level "noisy" impression. See the twentieth capture
immediately below, which appears to be a continuation of the same
episode.

## 2026-09-12, yet later still: twentieth 'K' capture, sixth at
## delay=+2.00, immediately following the nineteenth above - all FOUR
## cycle slots step (none static), and the user reports the "noise"
## clearing right after

User's label (sent as the "noisy mode" episode continued): "jumped to
1006 and noise gone." `t=3074237ms`, `before=700.50Hz after=1041.92Hz
delta=+17.68Hz delay=+2.00`, delay unchanged for 796993ms (~13.3
minutes) beforehand. `env_min=0.001` uniformly again - sixth consecutive
capture at this delay. Pre-cycle exactly `[800.0, 800.0, 400.0, 800.0]`
(same grid, just phase-shifted relative to the nineteenth capture's
`[800,800,800,400]`, as expected since the two triggers landed at
different absolute times).

Decoded with the corrected phase-correspondence method:

- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly
  at the trigger (`bin -1`), holds constant for the entire post-trigger
  window - single clean step, no double-hop.
- **A second slot**: 400.0Hz pre-trigger and for bins +3/+7 (~4-9ms),
  then steps directly to 1200.0Hz at bin +11 (~14ms post-trigger,
  +800Hz) and holds for the rest of the window - the earliest-timed
  isolated step decoded so far at this delay.
- **A third slot**: 800.0Hz pre-trigger and through bin +18 (~1-22ms),
  then steps to 1600.0Hz at bin +22 (~27ms post-trigger, +800Hz) and
  holds for the rest.
- **A fourth slot**: 800.0Hz pre-trigger and through bin +33 (~1-41ms),
  then steps to 1600.0Hz at bin +37 (~46ms post-trigger, +800Hz) - the
  latest-timed isolated step decoded so far, landing right at the edge
  of the visible window.

Notable new wrinkle: unlike every capture at this delay so far (which
has always left at least one slot completely silent), ALL FOUR slots in
this capture take exactly one step apiece, spread across the widest
timing range yet seen in a single-step-only capture - from ~14ms to
~46ms post-trigger, with no slot remaining static and no slot
double-hopping or skipping a grid rung. Still fully 400Hz-grid-quantized
and fully consistent with the established repertoire (four separate
instances of the ordinary "one isolated +800Hz step" behavior, just all
occurring in the same capture with no static slot this time) - just the
first time every slot has participated.

Taken together with the nineteenth capture immediately above: two
back-to-back triggers ~153.6s apart, both at Preset 3's delay=+2.00,
both cleanly quantized internally, with the TX mean climbing
700.39Hz -> 1018.28Hz -> 1041.92Hz across the pair - consistent with the
"noisy mode" being a rapid succession of individually-clean 'K' events
rather than any single messy transition, and consistent with the user's
report that the perceived noise cleared once the frequency settled
(here, around the user's own "1006" readout - close to but not
identical to this trace's computed `after=1041.92Hz`; the discrepancy is
most likely just a difference between the user's Aux SP dial reading and
this trace's internally-computed mean, not a new technical finding, and
not pinned down further here).

## 2026-09-12, yet later still: twenty-first 'K' capture, seventh at
## delay=+2.00, `delta=-5.00Hz` - second instance of the smooth,
## continuous, non-quantized drift category (first seen capture
## seventeen), and the user reports settling further

User's label: "back down to 997," continuing the same episode as the
nineteenth/twentieth captures above (973 -> 1006 -> 997). `t=3227772ms`,
`before=699.89Hz after=694.65Hz delta=-5.00Hz delay=+2.00`, delay
unchanged for 950528ms (~15.8 minutes) beforehand. `env_min=0.001`
uniformly again - seventh consecutive capture at this delay.

Unlike the nineteenth/twentieth captures (both fully discrete,
800Hz-quantized cascades), this one decodes exactly like the
seventeenth capture: no discrete jump anywhere in the trace at all.

- **Two slots are completely flat**: 800.0Hz throughout, pre- and
  post-trigger alike.
- **The other two slots form a near-null-blended pair, smoothly
  drifting in complementary directions for the entire visible 100ms
  window**: one rises continuously from 814.1Hz (bin -40) to 827.4Hz
  (bin +40); the other falls continuously from 385.5Hz (bin -37, its
  first sampled value) to 371.2Hz (bin +40). Their bin-by-bin sum stays
  pinned at essentially a constant ~1199-1200Hz throughout (1199.6Hz at
  bin -40's pair, 1198.6Hz at bin +40's pair - the small apparent
  decline is consistent with reading/rounding noise on two independently
  quantized-to-0.1Hz drifting values, not a real step), matching
  `expected_center_hz` for 700/1700 exactly, with no discrete step at
  any point.

This is the second example of the "smooth, continuous, non-quantized
drift" category (taxonomy item 7, first seen in the seventeenth
capture) - and, like that one, it produced the smallest-magnitude
trigger seen (`delta=-5.00Hz`, right at the 5Hz threshold, negative)
which fits: a smoothly precessing near-null pair with a constant sum
crosses the fast/slow EMA divergence threshold only gradually and only
barely, unlike a discrete 800Hz step which blows straight through it.

Taken with the nineteenth/twentieth captures, this closes out the
"noisy mode" episode: two discrete, fully-quantized 800Hz-cascade
triggers (973 -> ~1018Hz register, then -> ~1042Hz register per the
trace, matching the user's own 973 -> 1006 readouts closely enough) were
followed by one smooth, continuous, non-quantized drift trigger back
down slightly (-5.00Hz, to the user's reported 997) - consistent with
the earlier hypothesis that the perceived "noise" was a rapid
succession of individually-clean 'K' events (two sharp discrete jumps
plus a small continuous settle) rather than one messy transition; no
new structural finding here, but a clean real-world example of the
smooth-drift category recurring in a context distinctly different from
its first appearance (that one was an isolated, uneventful capture; this
one closes out a run of back-to-back triggers during an actively
unsettled few minutes).

## 2026-09-12, yet later still: added a 'Z' serial command - clean,
## deliberate reboot on demand

Per direct request, to get back to a known-clean starting point (delay
reset to 0.00 at boot, both freq_dev EMAs re-seeded via the boot-settle
path, every 'J'/'K' ring and diagnostic counter cleared) between bench
captures without physically power-cycling the board. `serial_commands.cpp`:
prints a confirmation reply, `Serial.flush()`s it, a short `delay(100)`,
then `ESP.restart()`. The flush+delay matters here for the same reason
this file's own `serial_reply()` needed its 64-byte padding fix (see that
function's header comment) - `ESP.restart()` can tear down the USB CDC
peripheral before a reply has actually finished transmitting, so without
it the "-> rebooting now..." confirmation could silently never reach the
terminal. Single keystroke, no confirmation prompt, matching every other
one-shot command in this file ('r'/'V'/'L') - not bench-tested, no
toolchain available in this environment.

## 2026-09-12, yet later still: checked the relative_delay-ring-cascade
## timing hypothesis against the actual numbers - REFUTED, and the same
## pass turned up a much better candidate explanation for the universal
## period-4 bin structure itself

Working theory going into this: the 5-46ms staggered timing between
different cycle "slots" stepping (seen in every capture so far) might be
explained by `relative_delay`'s own ring buffer - a real step change
entering the ring would show up in different slot-phases at different
times as it propagates through, if the ring's own depth were on a
comparable timescale.

**The numbers refute this outright.** `PHASE_DELAY_MAX_SAMPLES=8`
(`relative_delay.h`) and `SAMPLE_RATE_HZ=16000` (`config.h`) put the
ring's FULL capacity at `8/16000 = 500us` - and every delay value
actually used this session (+2.00 to +5.23 samples) uses only a few of
those 8 slots, i.e. well under 500us of real time. The observed cascade
timing across all 22 captures spans roughly 1.24ms to 46ms - at minimum
~2.5x the ring's total capacity, and at the long end nearly 100x it.
`relative_delay`'s ring is simply too fast, by close to two orders of
magnitude, to be where the multi-tens-of-ms staggering comes from. This
hypothesis is dropped; the real mechanism for the STAGGERED TIMING
between slots remains open.

**Looking for what WOULD explain the timing turned up something more
fundamental: the period-4 bin structure seen in literally every capture
this session may itself be a measurement artifact, not a real 4-state
property of the transmitted signal.** `SLOW_TRACE_BIN_TICKS=20`
(`diagnostics.cpp`) - each printed bin is the MEAN of 20 consecutive raw
`delayed_freq_dev_hz` ticks (`bin.freq_mean_hz = s_slow_bin_sum_freq /
s_slow_bin_count`, `diagnostics_set_tx_info()`), not a single raw sample
as every prior write-up this session implicitly assumed. Separately, for
the confirmed 700/1700Hz tone pair, the beat frequency is exactly 1000Hz
- one beat cycle is `16000/1000 = 16` raw ticks. `gcd(20, 16) = 4`: any
process that repeatedly averages a signal with a true 16-tick period
using a non-overlapping 20-tick window will see the window's phase
relative to that period advance by 4 ticks every bin, cycling back to
its starting alignment every `16/gcd(20,16) = 4` bins - a period-4
pattern in BIN-INDEX space, arithmetically guaranteed by these two
constants alone, regardless of `relative_delay`, tone amplitude, or
anything else project-specific.

This is a strong candidate explanation for why every single capture -
across all five `relative_delay` values, every magnitude and sign of
delta, both the discrete-step and smooth-drift categories - has shown
the identical period-4 slot structure with no exceptions: an artifact
universal across every capture is a hallmark of the MEASUREMENT process,
not the thing being measured. A quick numerical check (Python,
`scipy.signal.hilbert` on a synthetic `cos(2*pi*700*t) + cos(2*pi*1700*t)`
at 16000Hz, instantaneous frequency via unwrapped-phase derivative,
binned in non-overlapping windows of 20) confirms the qualitative
picture - large, systematically different excursions land in specific
bin-index-mod-4 groups, with values clustering near multiples of 400Hz
in some groups and much noisier ranges in others - but does NOT cleanly
reproduce the dead-flat, exact-to-0.1Hz quantized levels seen on real
hardware. Most likely explanation for the gap: the real firmware's
`freq_dev_hz` comes from `atan2(Q,I)` on the actual ADC-sampled/filtered
signal chain (see `ssb_dsp.h`), not a bare textbook two-tone sum - the
real pipeline almost certainly has additional structure (filtering,
`relative_delay`'s own interpolation, whatever's between the raw
computation and where `diagnostics_set_tx_info()` reads it) not modeled
in this back-of-envelope check.

**What this does and doesn't change:** every individual value, timing,
and sum-conservation finding logged in every capture entry above stays
numerically accurate - nothing here contradicts any of that. What
changes is the INTERPRETATION of the four repeating "slots": they were
treated throughout this investigation as if they might reflect four
physically distinct states of the transmitter, and they may instead be
four different alias-phase relationships between a 20-tick averaging
window and a 16-tick-periodic raw signal, with no inherent physical
meaning of their own beyond that. It does NOT retract or explain the
STAGGERED CASCADE TIMING (tens of ms between different slots' visible
transitions) - a shared alias artifact of this kind would only offset
different phase-groups' view of a single shared instant by at most one
alias period (4 bins, ~5ms), not the 30-40ms gaps repeatedly observed
(e.g. captures 20/21's phase0 stepping ~32ms after phase2). That timing
question is still open and still real, whatever turns out to explain the
4-way grid structure itself. Flagging this as a candidate mechanism
worth testing directly on the bench (e.g., does changing
`SLOW_TRACE_BIN_TICKS` to a value with a DIFFERENT gcd against 16 change
the apparent cycle length from 4 to something else, as this hypothesis
would predict?) rather than as a settled finding - the taxonomy built up
across 22 captures remains the accurate empirical record either way.

## 2026-09-12, yet later still: twenty-second 'K' capture, FIRST at
## delay=+0.00 (boot default, never touched) - a new env_min data point
## (0.200) that further complicates the delay-vs-env_min picture, and
## user-flagged as the biggest shift seen yet

User's label: "sending this as its the biggest shift Ive seen to 950."
`t=154655ms` (only ~155s since boot), `before=700.52Hz after=1003.01Hz
delta=+20.64Hz delay=+0.00`, "relative_delay has not been changed since
boot" - this is running at the boot-default delay (0.00 samples, i.e.
`relative_delay` never touched at all this session), not any of the
five delay values logged earlier. `env_min=0.200` uniformly across all
81 bins - a NEW level, distinct from every value on record so far
(0.001 at +2.00, 0.025 at +3.08, ~0.09 at +2.33, 0.238 at +2.70,
0.04-0.086 climbing at +5.23). Notably, `+0.00` is roughly as far from
+2.70 as +5.23 is (2.70 vs 2.53), yet reads a MUCH clearer env_min
(0.200, close to +2.70's own 0.238) than either +2.33 or +3.08, both of
which sit much CLOSER to +2.70 numerically but read far more
near-null (~0.09 and 0.025 respectively). This directly reinforces the
twelfth capture's earlier conclusion that the relationship isn't a
simple function of distance from +2.70 - it now looks less like a
single narrow peak and more like a genuinely irregular function across
the range, possibly related to how `interp_ring()`'s fractional blend
weight and whatever null-adjacent structure exists in the raw signal
interact at each specific delay value, not distance from any one point.

Pre-cycle exactly `[800.0, 400.0, 800.0, 800.0]`, zero drift. Decoded
with the corrected phase-correspondence method:

- **Two slots never move**: flat 800.0Hz across the entire trace (the
  second capture this session with two silent slots, after the
  sixteenth).
- **The trigger slot**: 800.0Hz pre-trigger, steps to 1600.0Hz exactly
  at the trigger, holds constant for the entire post-trigger window -
  single clean step, no double-hop.
- **The fourth slot**: 400.0Hz pre-trigger and for 3 post-trigger
  samples (bins +2/+6/+10, ~2-12ms), steps to 1200.0Hz at bin +14
  (~17ms post-trigger, +800Hz), holds for 3 samples (~17-27ms), then
  steps again to 2000.0Hz at bin +26 (~32ms post-trigger, +800Hz) and
  holds for the rest - the "visits the intermediate grid rung via two
  separate steps" pattern, same timing signature (~17ms/~32ms) as
  several earlier captures at completely different delay values.

Purely confirmatory in terms of the established transition taxonomy -
nothing here is a new type of step or timing pattern, just the familiar
repertoire showing up at a brand-new delay setting. The genuinely new
information is the env_min=0.200 data point itself, and the user's
report that the frequency kept moving well past this trace's visible
window (after=1003.01Hz here, settling to ~950Hz per the user, in line
with the "full settling can take longer than one trace" caveat noted
several captures back).

## 2026-09-12, yet later still: user observation via the new 'Z' reboot -
## repeating an identical manual delay sweep through ~+4.5 sometimes
## triggers 'K', sometimes only produces a small non-triggering wobble -
## is this a boot-initial-condition effect or something else?

User's question: after `'Z'`, sweeping `relative_delay` by hand through
the ~+4.5 area (previously seen in the messy +5.23 capture's
neighborhood) sometimes produces only a small, non-triggering frequency
wobble (read as the already-established near-null uncertainty) and
sometimes produces a real 'K'-triggering jump - same nominal sweep,
different outcome across reboots. Is this coincidence tied to the
reboot's own initial conditions?

Two distinguishable candidate explanations, not yet tested against each
other:

1. **Genuine boot-to-boot hardware timing jitter** - something in the
   true startup state (peripheral init order, first-tick timing) differs
   slightly reboot to reboot, shifting where the (otherwise fixed)
   two-tone waveform's null crossings sit relative to the sample-tick
   grid. This project already has ONE directly relevant, previously
   documented data point on this, and it argues AGAINST significant
   jitter of this kind: `group_delay_fit_notes.md` documents a ~264-265us
   `spi_us` spike on exactly the first `dsp_task` tick after boot,
   explicitly confirmed "reproducible across independent reboots - a
   deterministic cost, not a scheduling fluke" (i-cache warm-up on the
   AD9851 driver's ESP-IDF internals). That's the one boot-timing
   behavior this project has actually characterized, and it points at
   boot timing being quite deterministic, not jittery, at least for that
   code path.
2. **Sweep-timing jitter relative to an ever-repeating, very fast
   null-crossing cycle - independent of the reboot entirely.** The
   `'['`/`']'` "scan" is a human pressing keys (confirmed - there's no
   automated delay-scan code in this project, `relative_delay.cpp`'s own
   comments describe manual `'['`/`']'` scanning as the only way delay
   changes over time), so the exact wall-clock instant the running delay
   value passes through +4.5 is not reproducible to anything near
   millisecond precision between attempts. The confirmed 700/1700Hz tone
   pair has a 1000Hz beat - a full null-crossing cycle every ~1ms. A
   difference of even a fraction of a millisecond in when the sweep
   passes through the critical region changes which part of that
   cycle the affected raw samples land on - and this whole investigation
   has already established just how thin that margin is (a near-null
   sample sits, by definition, at a knife-edge where a tiny timing
   difference is the entire difference between a clean blend and a
   sharp discontinuity). This explanation predicts the SAME sometimes-
   triggers/sometimes-doesn't variability would show up even WITHOUT
   ever touching `'Z'` - i.e. repeating the identical sweep several times
   in a row within a single boot should show the same mixed outcome.

These are cleanly distinguishable with one bench test: repeat the same
`~+4.5` sweep several times in a row WITHOUT rebooting in between, and
see whether the small-wobble/big-jump split still shows up. If it does,
that rules out the reboot/initial-condition explanation entirely - the
effect is really about manual-sweep timing jitter against a fast,
fixed-period null-crossing cycle, which is itself a nice, concrete,
real-world confirmation of the sensitivity this whole investigation has
been characterizing analytically. If the mixed outcome ONLY appears
right after a fresh `'Z'` (repeats within one boot are always
consistent with each other), that would point back at something
genuinely boot-specific after all, which would need further
investigation given the +264us finding above doesn't obviously predict
it. Not yet run - flagged for the user to try next.

## 2026-09-12, yet later still: the no-reboot repeat test came back - many
## solid sweeps through ~+4.5, then a clean transition into "reliably"
## triggering, all within ONE boot - this rules out pure boot jitter and
## complicates pure keypress-timing coincidence too; twenty-third capture
## decoded (first-ever near-zero/negative delay, env_min visibly changing
## WITHIN one trace)

User's report: sat in a single boot (no `'Z'` since the last one),
repeated `'['`/`']'` sweeps through the ~+4.5 area, cycled through
several presets and back to Preset 3, swept again - "many times" - with
the frequency staying solid (no more than low-Hz wobble) throughout.
Then, at some point, 'K' triggered anyway (below), and immediately
after that the user reports it "jumped to 954, then 1013" on further
sweeps (no trace data for those two - just outcomes). User's own
read: "Feels like two different states its in as its jumping 'reliably'
now compared to being solid earlier."

**This result is genuinely informative for the question raised in the
previous entry, and it doesn't cleanly support EITHER candidate
explanation offered there.** No `'Z'` was pressed at any point in this
sequence, so whatever changed between "solid for many sweeps" and
"reliably jumping" cannot be a boot-initial-condition effect - that
candidate is now ruled out directly by this result, not just argued
against by the earlier +264us precedent. But it doesn't fit pure
sweep-timing coincidence (candidate 2) cleanly either: if the outcome
were just a per-attempt coin flip against the ~1ms null-crossing cycle,
independent of everything else, the solid/jumpy attempts should be
scattered roughly randomly throughout the session, not cluster into one
long solid stretch followed by a stretch that "reliably" jumps. A clean
two-phase split like this looks more like something in the SYSTEM
ITSELF drifted between the two stretches - a third candidate not
considered in the previous entry: some slow, elapsed-time-linked change
during a single boot (a plausible physical candidate being oscillator/
crystal thermal drift over the first several minutes of runtime, slowly
shifting the true tone/sample-clock relationship and therefore the
null-crossing-to-sample-grid alignment established as central to this
whole investigation - though this is speculative and untested). The
first trigger in this sequence landed at `t=308312ms` (~5.1 minutes
after boot), which is at least consistent with a warm-up-timescale
effect, though one data point proves nothing on its own. Worth tracking
in future sessions: does "goes bad" reliably happen around a similar
elapsed uptime, or does it track something else (sweep count, which
specific delay values got dwelled on, etc.)?

**Twenty-third capture decode:** `t=308312ms before=700.51Hz
after=1085.17Hz delta=+15.46Hz delay=-0.02` - the FIRST negative (and
first near-exactly-zero-but-not-quite) `relative_delay` value captured
this session. "relative_delay was last changed 17ms before this
trigger" - and for the first time, `env_min` is NOT constant across the
whole trace: it reads 0.010 for bins -40 through -14, then drops to
0.001 from bin -13 through the entire post-trigger window. Bin -13 sits
roughly 12-13 bins (~15-16ms) before the trigger - closely matching the
reported 17ms delay-change timestamp (within about one bin's worth of
quantization uncertainty), i.e. this is the first capture where a
mid-trace env_min shift can be directly, closely correlated with the
logged delay-change instant, rather than inferred indirectly. This is a
clean confirmation that `env_min` tracks whatever `relative_delay` is
active AT THAT MOMENT, dynamically and quickly (within about a bin's
worth of the change itself), not some slower system-wide property.

The frequency-grid pattern itself is completely undisturbed by the
env_min transition - the same `[800,800,800,400]` pre-cycle holds
exactly through both env_min regions, only the null-depth reading
changes. `env_min=0.001` for delay=-0.02 extends the "exact/near-exact
integer delay -> deepest null" pattern to a THIRD value beyond the
established `+2.00` case (`-0.02` is only 2% of a sample off from the
integer 0) - **but this sits in tension with the 22nd capture, where
`delay=+0.00` (an EXACT integer, arguably an even cleaner case) read
`env_min=0.200`, not anywhere near this deep.** That capture was taken
~155s after a fresh boot on whatever the persistent boot-default
settings are; this one was taken mid-session on Preset 3 with its own
full settings (gdeq/ampeq/interp/etc.) active. Rather than conclude the
integer-delay mechanism is wrong, the more likely explanation is an
unexamined CONFOUND: `env_min` may depend on more than just
`relative_delay`'s fractional part - other settings that differ between
"boot default" and "Preset 3" (envelope interpolation, gdeq variant,
ampeq shelves, etc.) could independently affect it. This is flagged as
an open, untested question, not resolved here - genuinely don't know
yet whether `env_min` is a pure function of `relative_delay` alone.

Decoded with the corrected phase-correspondence method:

- **The trigger slot**: 400.0Hz pre-trigger, steps to 1200.0Hz exactly
  at the trigger, holds for the entire post-trigger window - single
  clean step, no double-hop.
- **A second slot steps essentially IN LOCKSTEP with the trigger**:
  800.0Hz pre-trigger, already reading 1600.0Hz by the very FIRST
  post-trigger bin (+1, ~1.24ms) - a full +800Hz step landing within
  about one bin of the trigger itself, not some isolated later time.
  This is new: every previous capture with a "companion" step near the
  trigger still showed it arriving at least a few ms later than the
  trigger bin itself; here it's indistinguishable from simultaneous
  within this trace's resolution.
- **The other two slots step together, much later, also essentially in
  lockstep with EACH OTHER**: both hold 800.0Hz through bin +23-24
  (~28-30ms), then both read 1600.0Hz starting at bin +26-27
  (~32-33ms) - a paired +800Hz step, the two slots only one bin
  (~1.24ms) apart from each other.

Net shape: TWO PAIRS of slots, each pair stepping together (one pair at
the trigger instant, the other pair ~30ms later) - still built from the
same "each slot takes one clean +800Hz step" primitive established
throughout this investigation, just with an unusually tight, paired
timing structure not seen quite this way before (previous captures had
at most one slot's step coinciding closely with another's; this is the
first with two clean simultaneous PAIRS).

## 2026-09-13: mechanism reference - why does changing relative_delay
## shift the OUTPUT frequency at all? (user question, moving on from the
## Z-reboot coincidence thread)

Direct answer, and it's the same mechanism this project already
established (via the pre-existing `null_bias`/`null_bias2`/`null_bias3`
diagnostics, SDR-confirmed, predating this whole 'K' investigation) -
not a new finding, but worth stating plainly in one place since this
whole session's 22+ captures have been built on top of it implicitly.

`relative_delay` is NOT a frequency control. Its actual job
(`relative_delay.h`'s own header) is to time-align two physically
different output paths - the AD9851 phase/frequency path (near-instant
SPI write) and the PWM envelope path (~140us measured analog
reconstruction-filter group delay) - so RF phase and envelope amplitude
leave the hardware in step with each other. It does this by holding ONE
of `freq_dev_hz`/`envelope` back relative to the other, reading from an
earlier (or later, if negative) point in a small ring buffer via
LINEAR INTERPOLATION between the two nearest whole-sample entries
(`interp_ring()`).

The mechanism that turns this pure re-timing into an apparent frequency
shift: `freq_dev_hz` is computed per-sample as `wrap_pi(phase[n] -
phase[n-1]) * SAMPLE_RATE_HZ / 2pi` (`ssb_dsp.h`) - a phase DERIVATIVE.
Right at a beat-envelope null (I and Q both near zero), this derivative
is genuinely, mathematically ill-conditioned - a real, unavoidable
property of representing an amplitude zero-crossing through a
phase-only signal, not a computation bug (this project's own
`max_freq_dev_hz` clamp and its comment - "phase noise near zero
envelope crossings can produce huge spurious instantaneous frequency
spikes... QCX-SSB refers to this as 'restricting' the phase changes" -
already documents this as a known, expected property of the technique).
Concretely, two ADJACENT raw `freq_dev_hz` samples can differ by
thousands of Hz right at a null. Since `relative_delay`'s fractional
interpolation blends exactly two adjacent raw samples, and since a
different delay value shifts WHICH two samples (and what blend weight)
get used, different delay settings pick a genuinely different resulting
value out of that near-null instability - and because this post-delay,
post-interpolation value is exactly what gets handed to
`ad9851_set_frequency()`, whatever gets selected there is really,
physically transmitted, not a diagnostic-only artifact. This is the
core, high-confidence, ALREADY-established mechanism (confirmed against
a real SDR per `null_bias2`'s own doc, well before this session).

**What's still genuinely open, and this session's whole 'K' investigation
has been circling it without landing a confirmed answer:** WHY does a
value selected at one null-crossing instant then PERSIST, cleanly, for
tens of milliseconds (many beat cycles), rather than reverting the very
next cycle the way a purely per-sample-computed, memoryless quantity
"should"? (`ssb_dsp.h` explicitly documents `freq_dev_hz`/phase as
"recomputed fresh from atan2(Q,I) every sample - a bad tick can't
outlive itself" - i.e. by design, nothing in that computation itself
should carry an error forward.) One candidate considered and set aside
here: `test_signals.cpp`'s two-tone generator uses per-tick phase
accumulators (`s_tone1_phase += ...`), and `ssb_mic_test.ino`'s own
`dsp_task` loop explicitly documents that a real scheduling overrun
spanning more than one full-tick boundary makes that tick's sample
"unrecoverable" (skipped, not caught up on) - a tempting persistent-
state candidate. On closer inspection this doesn't cleanly close the
loop: a skipped tick delays BOTH tones' phase accumulators together (in
the same function call), preserving their relative phase and hence the
beat/null pattern itself; every downstream consumer (diagnostics
binning, the delay ring) advances off the same internal tick-count, so
a uniform 1-tick loss shifts internal-time-vs-wall-clock alignment
without an obvious reason to change `freq_dev_hz`'s own long-run
statistics. Flagging this as considered-and-not-conclusive rather than
either confirming or fully retracting it - the real explanation for the
PERSISTENCE (as opposed to the SELECTION, which is solidly explained
above) remains an open question if picked back up later.

## 2026-09-13: answered directly from source - switching Preset 2<->3
## live does NOT preserve sync cleanly, because it silently swaps
## group-delay-equalizer coefficient sets and force-resets that filter's
## internal state every time

User's question, prompted by observing the "optimum" AM/PM alignment
itself apparently jumping around (looking closer to ~0.5 samples than a
clean 1-sample shift) and Preset 2<->3 switches sometimes needing a
detour through Preset 1 to "reset" cleanly: can these presets be
switched on the fly without upsetting sync?

Checked `settings.h`'s actual `settingsPresets[]` table field-by-field.
Preset 2 ("Shelf2 Baseline gdeq adj#2") and Preset 3 ("Shelf2 Baseline
gdeq adj#4") are identical in every field EXCEPT two: `relative_delay_
samples` (2.03 vs 2.00 - trivial, ~0.6us at 16kHz) and, much more
significantly, `env_gdeq_variant` (`ENV_GDEQ_VARIANT_DEFAULT` vs
`ENV_GDEQ_VARIANT_CANDIDATE_B`) - two GENUINELY DIFFERENT two-section
allpass filter designs for the envelope-path group-delay equalizer
(`envelope_gdeq.h`): default is `a1=0.026173, a2=0.236810`; candidate B
is `a1=a2=0.09` (a real, independently-derived Pareto-refit design, not
a small tweak of the same coefficients).

`envelope_gdeq_set_variant()` (`envelope_gdeq.cpp`) is unambiguous about
what happens on every value change (not a no-op guard on IDENTICAL
values, which it does have, but on any real change): it fully
RE-INITS both allpass sections, which explicitly ZEROES their internal
IIR feedback state (`x1`/`y1`) as a documented, deliberate side effect -
"switching live never feeds a mismatched coefficient/state pair into
the very next sample." That's a real, considered trade-off (stale
state combined with new coefficients would arguably be worse), but the
alternative it chooses - forcibly zeroing a running filter's memory
while the actual envelope signal is mid-waveform, not at rest - is
ITSELF a genuine, physical discontinuity in the envelope path's output,
happening at the exact instant of every Preset 2<->3 switch (or any
switch between two presets with different `env_gdeq_variant` values),
whether or not anything is actively transmitting through it at the
time.

**This directly explains the reported symptoms:**
- **"Something is happening" on a live 2<->3 switch**: yes - the
  group-delay equalizer's internal state is unconditionally zeroed
  every time, a real transient in the AM path's phase-vs-frequency
  relationship, independent of anything `relative_delay` itself is
  doing.
- **The apparent "~0.5 sample, not 1" shift**: consistent with this
  NOT being a discrete sample-count artifact at all. What's actually
  changing between the two gdeq variants is a continuous MEAN GROUP
  DELAY difference between two distinct filter designs (candidate B's
  own header documents full-band p-p dispersion of 136.0us vs default's
  156.5us - different filters, not different amounts of the same
  filter) - a real, physical microsecond-scale delay difference that,
  converted to samples at 16kHz (62.5us/sample), would plausibly land
  somewhere in the "noticeably less than 1, more than 0" range rather
  than snapping to a clean integer or half-integer - matching the
  user's own "0.5-ish, not a good fit to 1" read exactly.
- **"Sometimes 1 and back to 2/3" needed to reset cleanly**: Preset 1
  has `env_gdeq_enable=false` (gdeq off entirely), while 2 and 3 both
  have it on. `envelope_gdeq_set_enabled()` only resets filter state on
  an OFF->ON transition (checked directly: `if (enable && !was_on)`) -
  so routing through Preset 1 forces that reset path in addition to the
  variant-change reset that ALSO already fires on a direct 2<->3
  switch. Both paths do reset the same underlying state, so this may be
  more about the extra elapsed settling time from two switches instead
  of one, rather than reaching some reset a direct switch skips - not
  fully resolved from source alone, would need a bench comparison to
  say for certain.

**Bottom line for the user's literal question**: no, Preset 2 and 3 are
not safely swappable live without a real, if brief, disturbance -
they're not simply "the same config at two closely-tuned delays," they
carry two structurally different group-delay-equalizer filters, and
this project's own code deliberately (not as a bug) resets that
filter's state on every switch between them. Anyone doing on-air
preset changes between configs that differ in `env_gdeq_variant` (or
toggle `env_gdeq_enable`) should expect a brief envelope-path glitch at
the switch instant, settling out over a handful of samples afterward.

## 2026-09-13: does the reset-fixes-it behavior confirm a "bad state"
## needing re-sync? Partly - but there look to be TWO separate effects
## bundled together, and one of them is NOT a transient at all

User's follow-up: doesn't the fact that forcing a reset (cycling through
Preset 1) helps confirm the system really is sitting in a bad,
desynced state? Checked further rather than just agreeing - the answer
is yes, but probably for two DIFFERENT reasons that shouldn't be
conflated:

**(a) A genuine but purely TRANSIENT effect, identical whether reached
via Preset 1 or a direct 2<->3 switch.** As logged in the previous
entry, `envelope_gdeq_set_variant()` zeroes the allpass filter state on
ANY real variant change - this already happens on a direct 2<->3
switch, not just via Preset 1. This settles out over a handful of
samples and says nothing about the STEADY-STATE quality of either
preset once settled.

**(b) A separate, NOT-transient, structural mismatch specific to Preset
2 itself, that a "reset" doesn't fix so much as sidestep.** Re-reading
`envelope_gdeq.h`'s own fitting history: the DEFAULT gdeq pair (Preset
2's variant) was explicitly fit "against the bare analog filter ALONE,
before either [ampeq] shelf existed" and is documented as "known...
to NOT flatten well once ampeq's shelf(s) are on." But Preset 2 itself
runs with BOTH ampeq shelves ON (`env_ampeq_enable=true`,
`env_ampeq_shelf2_enable=true`) - i.e. Preset 2 pairs a gdeq fit with
exactly the ampeq configuration its own documentation says it wasn't
designed for. Candidate B (Preset 3's variant), by contrast, was
specifically fit "against the REAL measured bare (analog+shelf1+
shelf2) curve" - i.e. it IS matched to the ampeq-shelves-on
configuration both presets 2 and 3 actually run. This is a real,
PERMANENT, already-documented mismatch baked into Preset 2's own
resting definition - not something a reset event fixes or that self-
resolves after settling; Preset 2, sitting completely undisturbed with
no further switches, would still be running a known-suboptimal
group-delay correction for its own ampeq configuration.

**How to tell which one the user is actually seeing**: if the "bad"
feeling is brief - appears right at a switch, settles out within a
fraction of a second - that's (a), the transient reset, and is
inherent to switching between differently-configured presets at all
(unavoidable without a code change to avoid the reset, which would
trade in a different, arguably worse problem - see that function's own
comment). If it PERSISTS indefinitely while parked on Preset 2 with
nothing touched, that's (b) - a structural mismatch specific to Preset
2's own definition, fixable by re-fitting/re-pairing which gdeq variant
Preset 2 uses (arguably it should be using candidate B, or the a+A
candidate, rather than the un-shelf-matched default pair, given both
its ampeq shelves are on) - not by resetting anything. Not yet
distinguished which is dominant in what the user's actually observing -
flagged as the next useful thing to separate, if picked back up.

## 2026-09-15: automatic 'K' dump on capture - implemented, plus twenty-fourth/twenty-fifth/twenty-sixth captures decoded, one pair of which directly demonstrates why the fix matters

**User request, verbatim intent: "Can you automatically dump the K tables when a jump is triggered and captured. I can then just leave it running and see what we get over time."** Motivated by several reported hands-off jumps (their own examples: -50Hz, +20Hz) that looked instantaneous and that they weren't at the keyboard to catch with a manually-timed 'K' press.

**What changed (`diagnostics.cpp`/`diagnostics.h`, not bench-tested - no toolchain in this sandbox):** the print-and-rearm body that used to live only inside `diagnostics_print_slow_trace()` (the 'K' handler) is factored out into a shared static helper, `print_and_rearm_slow_trace(const char *label)`, taking a label so the printed header reads either `TRIGGERED` (found via 'K', unchanged) or `AUTO-CAPTURED` (found automatically). A new `diagnostics_check_slow_trace_auto_dump()` checks `s_slow_trace_state == SLOW_TRACE_LATCHED` and, if so, calls the helper with `AUTO-CAPTURED` - otherwise it's a single enum comparison and returns immediately. This is called unconditionally (mute-EXEMPT, same as `canary_check_background()`'s existing pattern just above it) from `diagnostics_service()`, which already runs once per Core 1 `loop()` iteration - so a capture now prints itself within one `loop()` cycle (worst case ~10ms, the `delay(10)` at the end of `loop()`) of its post-window actually filling, with zero keypresses needed. 'K' itself is unchanged and still useful for its live "still watching" fast/slow/delta readout (which the auto-dump deliberately never prints, only a *completed* capture) and works fine as a manual read too - if a human happens to press 'K' in the same loop() tick the auto-dump would have fired, whichever gets there first prints it once (the helper always re-arms before returning), and the other simply finds `SLOW_TRACE_WATCHING` with nothing to do. No change to the capture/trigger logic itself - a LATCHED trace already held indefinitely until read (see `s_slow_trace_state`'s declaration comment), so nothing was ever at risk of being *lost*; this only removes the dependency on a human noticing and acting before the *next* useful moment to read it.

**Practical bench note for the user**: this makes the firmware print the dump on its own, but actually *keeping* that output for later review still needs something on the PC side capturing the serial stream to a file (e.g. a terminal program's own logging feature, `screen -L`, `minicom -C`, or a small pyserial script) - the firmware change alone just means nothing has to be typed to trigger a specific dump anymore.

**Then decoded the three fresh captures sent alongside the request** (twenty-fourth through twenty-sixth, continuing this file's running count) - and the pairing of the last two turned out to be a good, concrete illustration of exactly why the fix above matters, not just a convenience.

**Twenty-fourth capture** (`t=154604ms`, `delta=-5.00Hz`, `before=699.86Hz after=694.35Hz`, `delay=+2.00`): the third instance of the smooth, continuous, non-quantized drift category (after the seventeenth and twenty-first). Two slots hold flat at a constant 800.0Hz throughout the entire 80-bin (100ms) window; the near-null-blended pair smoothly and monotonically redistributes across the WHOLE window, not just locally around the trigger - one component climbs from 813.4Hz (bin -40) to 837.4Hz (bin +40), the other falls from 386.1Hz to 358.8Hz over the same span. Their sum starts almost exactly at `expected_center_hz` (1199.5Hz at bin -40) but drifts down to ~1196.2Hz by bin +40 - a real, if small (~3Hz over 100ms), departure from exact sum-conservation, worth noting as a refinement to the "near-null-blended sum-conserved pairs" category: the conservation looks closer to a local/short-window property than an exact invariant over a longer span. `env_min=0.001` throughout, consistent with every other capture logged at `delay=+2.00`.

**Twenty-fifth capture** (`t=4149634ms`, `delta=+15.42Hz`, `before=700.37Hz after=1076.56Hz`, `delay=+2.00`): baseline pre-window is a perfectly flat, non-drifting cycle - three slots constant at 800.0Hz, one constant at 400.0Hz (mean exactly 700Hz, matching `before`) - for the full 40 pre-bins, until the trigger bin itself where the 400Hz slot makes a single clean +800Hz step to 1200.0Hz (the established single-step category). What follows is a clean four-stage cascade to a NEW resting cycle: by the very first post-bin, a second slot has *already* stepped from 800->1600Hz (essentially simultaneous with the trigger, matching the "two slots step near the trigger" sub-pattern seen before, e.g. the twenty-third capture) - so the post-window opens on a {800, 800, 1600, 1200} repeating cycle. That cycle then holds exactly steady for 20 bins (~25ms) before a third slot steps 800->1600Hz (~i=22, ~27ms post-trigger), and a fourth (the last remaining 800Hz slot) follows suit four bins later (~i=25, ~31ms post-trigger) - after which the cycle is fully settled at {1600, 1600, 1600, 1200} for the remainder of the visible window. So: two slots step essentially at the trigger, then the other two step independently, ~4ms apart, roughly 27-31ms later - the whole cascade completing within about 31ms, well inside the 50ms post-window. `env_min=0.001` throughout.

**Twenty-sixth capture** (`t=4303305ms`, `delta=+23.41Hz`, `before=700.71Hz after=1044.36Hz`, `delay=+2.00`), captured only ~153.7s after the twenty-fifth (a much shorter gap than the ~66.6 minutes between the twenty-fourth and twenty-fifth): baseline is again a perfectly flat cycle - one slot constant 400.0Hz, three constant 800.0Hz, mean exactly 700Hz (matching `before=700.71Hz`) - until the trigger bin, where the 400Hz slot jumps directly to 1600.0Hz. That's a +1200Hz single-bin step - three 400Hz quanta in one tick, and NOT a multiple of the 800Hz step size every other single-step transition logged in this investigation has used. This is the largest and structurally most unusual single-bin jump seen so far, and it doesn't fit the clean "four physical states, one steps by exactly 800Hz" framework the rest of this file's taxonomy is built on. What follows is a busier, more drawn-out cascade than the twenty-fifth's: the other three slots step up in stages over the first ~30ms (one via a double-hop through the intermediate 800->1200 rung before continuing to 1600, matching the established double-hop sub-pattern), the cycle spends an extended stretch (roughly the middle third of the post-window) with three slots parked at 1600Hz and one lingering at 1200Hz, and then - unusually - that last 1200Hz slot takes one more, very late step up to 1600Hz right near the end of the visible window (~i=37-40, i.e. ~46-50ms post-trigger, the latest final-slot step timing logged in this investigation). `env_min=0.001` throughout.

**The genuinely new finding is in the PAIR, not either capture alone.** The twenty-fifth capture's post-window cascade was heading toward a new resting mean around 1500Hz ({1600,1600,1600,1200}/4). But the twenty-sixth capture's own `before=700.71Hz` - the SLOW EMA value at ITS trigger instant, itself resynced from the fast EMA at whatever moment the twenty-fifth capture was actually read via 'K' - is back down essentially at the original ~700Hz baseline, not anywhere near 1500Hz. Two non-exclusive readings: either the elevated post-cascade state from the twenty-fifth genuinely reverted back toward baseline before the twenty-sixth trigger fired (i.e. these quantized "slot" cascades are not necessarily one-way settling events into a lasting new state - they can also fully revert), or the ~66.6-minute gap between when the twenty-fourth and twenty-fifth triggers were read via manual 'K' commands means the `before` reference for the NEXT trigger only ever reflects the fast EMA's value at whatever moment a human happened to get around to pressing 'K' - which, as this same batch shows, can be minutes to (in the twenty-fourth/twenty-fifth case) over an hour after the fact, silently absorbing whatever happened to the signal in between. Both readings point the same direction: this is exactly the ambiguity the auto-dump feature added earlier in this same entry now removes going forward - every future trigger's resync happens within ~10ms of its own post-window closing, not whenever a human next happens to type 'K', so a run of back-to-back auto-captured traces will make it directly observable (via the next trigger's own `before` value, tightly timed against the previous one's `after`) whether a given cascade actually held or reverted, rather than leaving that question hidden inside whatever gap happened to exist between manual reads. Flagged as the concrete thing to watch for in the next hands-off run now that auto-dump is live.

## 2026-09-15: first real hands-off bench validation of the auto-dump feature - it works exactly as designed, and the resulting log surfaces two significant new findings the manual-'K' workflow could never have shown

**Context: the user left a bench run going and captured everything into a Windows textbox via `AppendText()` (see this same day's earlier exchange on why that's effectively unbounded), then reported "didn't spot any big jumps here."** The uploaded log (`K_out_1.txt`, 2936 lines) covers ~23 minutes of runtime (`t=154702ms` to `t=1537502ms`) at a constant `delay=+2.00` (no preset/delay changes during this run - a clean, single-variable capture). It contains 18 events, **all 18 header lines read `AUTO-CAPTURED`, zero `TRIGGERED`** - i.e. the user never had to press 'K' once; every single event this run produced was caught and printed on its own. First real confirmation the feature added earlier today works as intended on actual hardware.

**On "didn't spot any big jumps": there ARE several genuinely large excursions in this file - they're just easy to miss skimming raw text, because the printed `delta=` field is never the excursion size.** `delta` is the fast/slow EMA gap *at the trigger instant* (by design, always a modest ~5-25Hz, since that's just the threshold-crossing amount) - the real swing size is `|after - before|`, which this file's own header lines actually show ranges up to ~325-385Hz:

| t (ms) | before (Hz) | after (Hz) | swing | character |
|---|---|---|---|---|
| 154702 | 699.86 | 694.35 | -5.5 | small dip only |
| 162849 | 702.83 | 695.96 | -6.9 | (EMA settle, no real event) |
| 308411 | 700.51 | **1085.04** | **+384.5** | big jump |
| 316549 | 708.89 | 699.07 | -9.8 | correction (see below) |
| 461934 | 699.95 | 694.83 | -5.1 | small dip only |
| 615754 | 700.35 | **1013.57** | **+313.2** | big jump |
| 623898 | 708.95 | 699.70 | -9.3 | correction |
| 769398 | 699.86 | 781.19 | +81.3 | moderate - hybrid drift+step (see below) |
| 777551 | 706.92 | 699.70 | -7.2 | correction |
| 923106 | 700.66 | **1011.78** | **+311.1** | big jump |
| 931260 | 708.81 | 695.34 | -13.5 | correction |
| 1076768 | 700.48 | **1025.79** | **+325.3** | big jump |
| 1084905 | 708.68 | 700.94 | -7.7 | correction |
| 1230406 | 700.35 | **1053.32** | **+353.0** | big jump |
| 1238544 | 708.80 | 697.21 | -11.6 | correction |
| 1384041 | 700.50 | **1022.30** | **+321.8** | big jump |
| 1392182 | 708.60 | 697.21 | -11.4 | correction |
| 1537502 | 699.97 | 694.87 | -5.1 | small dip only (file ends here) |

Of the 9 distinct excursion cycles this run captured: 6 are big (~300-385Hz swings, all decoding to the same clean two-then-two staggered 800Hz-grid cascade already characterized for the twenty-fifth capture, settling into a new {1600,1600,1600,1200}Hz cycle each time), 1 is moderate (+81Hz, a genuinely new hybrid pattern - see below), and 2-3 are just small ~5-7Hz blips that never developed into a real jump at all. So six clean ~300Hz+ jumps happened in 23 minutes and were all caught automatically - the feature is doing its job; the header format is just easy to skim past without doing the before/after subtraction. (Happy to add an explicit "swing from baseline" figure to the printed header if that would help it jump out visually - not done yet, flagging as an easy optional follow-up rather than assuming it's wanted.)

**New transition sub-type, first seen in the +81Hz (`t=769398`) event: a hybrid of the continuous-drift and discrete-step categories happening on DIFFERENT slots within the same single capture.** The window opens exactly like the smooth continuous-drift pattern (the near-null-blended pair redistributing smoothly, ~814->831Hz rising / ~385->361Hz falling, two other slots flat at 800.0Hz) and continues that way for the first ~37ms of the post-window. Then, abruptly at bin +31 (~38ms post-trigger), one of the two previously-flat 800Hz slots makes a clean, discrete +800Hz step to 1600.0Hz and holds there - while the OTHER flat-800Hz slot stays completely undisturbed at 800.0Hz for the entire trace. Two bins later (+33), the smoothly-*rising* drift component ALSO makes an abrupt +800Hz jump (from its drift trajectory, ~832Hz expected, to 1633.5Hz) and then continues drifting upward from that new elevated level (1633.5->1636.3 across the remaining bins) - while the smoothly-*falling* drift component keeps descending completely undisturbed by any of this (368.7->366.5->363.7->360.7, no discontinuity at all). So within one 100ms window: one static slot stays static, one static slot takes a clean discrete step, one drifting slot keeps drifting, and one drifting slot takes a discrete step ON TOP OF its own drift and then resumes drifting from the new level. Every one of this file's other transitions cleanly fit ONE category (all-static-until-a-step, or all-four-slots-drift-together) - this is the first capture where the four slots visibly split into two different behavioral modes simultaneously. Flagged as a new sub-category, not yet understood mechanistically.

**The most important finding of this batch: directly confirmed, at the bin level, that the "big jump" excursions are transient and fully self-reverting, and that the auto-dump's now-instant resync is itself generating a predictable secondary "correction" re-trigger almost exactly one warmup-hold later.** Every "correction" event (the ones immediately following a big jump, `delta` negative, `after` back near baseline) was checked, and its ENTIRE 80-bin trace - both pre AND post windows - is a perfectly flat, undisturbed baseline cycle (e.g. `t=316549`'s trace is nothing but `{800,800,800,400}Hz` repeating throughout, with `env_min=0.001` and zero variation anywhere; `t=777551` likewise). No transition is visible anywhere in either correction trace. That means by the time each correction fires, the signal had ALREADY fully reverted to baseline - the correction is not capturing a live "coming back down" transition at all, it's purely the SLOW (tau=2s) EMA finally catching up to a reversion the FAST EMA had already completed. The `t=316549` correction's own TREND block confirms this precisely: 2 seconds before that trigger (i.e. only ~6.1s after the `t=308411` jump's own post-window had closed), `fast` was ALREADY back down to 699.07Hz - full baseline - while `slow` was still sitting at 723.92Hz, elevated and only slowly relaxing down. So the underlying excursion itself lasted somewhere between 50ms (it's still fully elevated at the end of that capture's own 50ms post-window) and about 6 seconds (fully reverted well before the correction 8.1s later) - not indefinitely, and almost certainly a good deal closer to the short end of that range.

**Why the correction fires almost exactly ~8.14s after every jump, with startling consistency (8137-8154ms across all six pairs, <0.2% spread): this lines up with `FREQ_EMA_WARMUP_MS=8000.0f` almost exactly, and is very likely a direct, previously-invisible side effect of the auto-dump feature's own instant resync.** `print_and_rearm_slow_trace()` resyncs `slow=fast` and re-arms the 8-second warmup hold the moment EVERY trace is read - previously that only happened whenever a human got around to pressing 'K' (often long after the signal had time to fully settle, as this session's own earlier `null_bias_investigation.md` entries showed - one gap between reads was over 66 minutes). Now it happens automatically within ~10ms of every single capture completing - which, per the finding above, is typically still well within the excursion's own brief lifetime, or at least before the slow EMA has had time to relax. Resyncing `slow` to `fast` AT THAT MOMENT captures a value that's still elevated/mid-relaxation, not the eventual settled baseline; once the 8-second warmup hold lifts, the ALREADY-reverted fast EMA (which has been sitting back at ~700Hz for however long) is now far enough from the artificially-elevated `slow` reference to cross the 5Hz trigger threshold almost immediately - hence a correction firing at almost exactly warmup-hold-length (plus a small, variable extra delay for the tau=2s slow EMA to actually open up 5Hz of gap against a fast value that itself may still be settling slightly, which plausibly accounts for the consistent ~140ms of overshoot beyond the raw 8000ms figure). This is not a bug introduced today, and not a flaw in the auto-dump logic itself - it's the pre-existing resync-at-rearm design (deliberately added specifically to avoid a *worse* problem, a rapid re-trigger storm) now running on a much tighter, more consistent cadence than it ever did under manual polling, which has simply made this artifact visible for the first time. Worth knowing when reading future logs: a "correction" event's own trace being suspiciously flat/uneventful is the signature of this artifact, not a sign that anything new happened.

**Separately, and probably the single most significant lead to come out of this whole investigation so far: the underlying jump-cycles themselves repeat with an extremely regular ~153.6-second period (153,320-153,820ms across 9 consecutive cycles spanning the full ~23 minutes, under 0.3% spread), and a dedicated code search found NO existing timer, task, or constant anywhere in this firmware that could produce that cadence.** Checked: every `millis()`/`esp_timer_get_time()` periodic check in the codebase (all sub-second or one-shot), all FreeRTOS task/timer infrastructure (no `esp_timer_create`/`xTimerCreate` exists in this project at all - there is no software timer subsystem to begin with), possible tick-counter wraparounds (none land anywhere near 150s at `SAMPLE_RATE_HZ=16000`), and every slow/periodic feature in `test_signals.cpp` (the two-tone dither, disabled by default and ~0.25s cadence even if enabled; the unrelated 60.01s chirp-mode period, a different, non-concurrent audio source). Nothing found. Because these timestamps come from the ESP32's own `esp_timer_get_time()`, not the logging PC, this rules out a PC/terminal-side artifact too - whatever's producing a near-clockwork ~153.6s cycle is happening on (or coupled into) the device itself, and it isn't anything already in this firmware's own code. This is a concrete, measurable signature - a specific period to go looking for (hardware timer/watchdog, ADC auto-something, thermal cycling of some component, external RF interference, or a mechanism in a part of the codebase not yet examined) - rather than an amorphous "sometimes it jumps." Flagged as the top open question for the next session: what in this system runs on a ~153.6-second clock.

## 2026-09-15, later same day: second capture segment (`K_out_2.txt`) - same boot, same run, strongly reconfirms both new findings from the first segment, plus one recurrence and one minor loose end

**This file is a direct continuation of the same run as `K_out_1.txt`, not a fresh boot** - its timestamps (`t=1694449ms` onward) pick up on the same uptime clock `K_out_1.txt` left off at (`t=1537502ms`), with `delay=+2.00` still unchanged throughout. All 12 events are again `AUTO-CAPTURED`, zero manual 'K'. Combined, the two files now cover **~38.6 minutes hands-off, 30 events, zero missed** - the auto-dump feature continuing to do exactly what it was built for.

**On "I saw jumps and holds at maybe up to 20Hz here": that figure matches the printed `delta=` field almost exactly (this file's deltas run -5.00 to +22.79Hz) - but, same as the previous file, `delta` isn't the jump size, it's the modest at-trigger threshold-crossing amount.** The actual swings (`after - before`) in this segment are just as large as before - five of the six cycles here are big:

| t (ms) | before | after | swing | delta |
|---|---|---|---|---|
| 1694449 | 700.41 | **1024.43** | **+324.0** | +18.36 |
| 1702604 | 708.58 | 699.69 | -8.9 | correction |
| 1848089 | 700.41 | **1067.66** | **+367.3** | +15.38 |
| 1856237 | 708.94 | 695.34 | -13.6 | correction |
| 2001722 | 700.74 | **1017.18** | **+316.4** | +22.79 |
| 2009867 | 709.01 | 700.32 | -8.7 | correction |
| 2155364 | 700.36 | **1050.29** | **+349.9** | +15.04 |
| 2163508 | 708.67 | 698.46 | -10.2 | correction |
| 2308953 | 699.80 | 694.47 | -5.3 | small dip only |
| 2317094 | 700.42 | 695.33 | -5.1 | (EMA settle) |
| 2462711 | 700.53 | **1003.35** | **+302.8** | +17.05 |
| 2470851 | 708.67 | 696.58 | -12.1 | correction |

Same magnitude range as `K_out_1.txt` (+303 to +367Hz here vs. +311 to +385Hz there) - this segment isn't smaller, it just happens to be what was on screen when the user was watching, and "up to ~20Hz" is exactly what the delta field would show for jumps of any size, big or small, since that's what it's designed to report.

**Both of the first segment's major findings reconfirmed, cleanly, a second time.** (1) Every correction event checked in this file is again a completely flat trace (`t=1856237`'s bins are nothing but `{400.0, 800.0}Hz` repeating) - and its TREND block is the clearest illustration yet: `fast` sits dead flat at exactly `695.34Hz` for the entire visible 2-second history while `slow` visibly decays toward it sample by sample (709.60 -> 709.48 -> ... -> 709.01Hz), a textbook exponential relaxation tail with the fast side already fully settled. (2) The ~153.6s cadence continues without a hitch WITHIN this file: up-to-up gaps of 153640/153633/153642/153589/153758ms across all 5 consecutive cycles - same tight (<0.15% spread) band as the first file's 9 cycles, and the up->correction gap is again a rock-steady ~8140-8155ms. Between the two files combined, that's now 14 consecutive cycles across 38+ minutes all landing in the same narrow band - this is not a coincidence of one capture window, it's a real, sustained, near-clockwork periodicity.

**One recurrence worth noting**: the `t=2001722ms` trigger is another of the atypical +1200Hz single-bin jumps (400->1600 directly, three 400Hz quanta in one tick) first seen in this session's twenty-sixth capture - confirms that jump type is a real, repeating member of the transition repertoire, not a one-off fluke.

**One minor loose end**: the gap between `K_out_1.txt`'s last event (`t=1537502ms`) and this file's first (`t=1694449ms`) is 156947ms - about 3.3s (~2%) longer than the otherwise extremely tight ~153.6-153.8s band. Most likely explained by however the two files happened to get split/saved (not necessarily at a clean cycle boundary) rather than a real deviation in the underlying period - noted for completeness, not treated as a break in the pattern given how tight every other measured gap is.

## 2026-09-15, later still: third segment (`K_out_3.txt`, continuation of the same run) - user flags a live SDR-observed "-22Hz, stuck for a while" and asks whether it matches what the log shows; honest answer is "not obviously, and here's why that's not necessarily a contradiction"

**Clarification from the user, worth recording precisely**: what they're reporting live (this message and presumably future ones in this same watch) is the actual tone frequency shift they read directly off their SDR - a real-world instrument reading - not necessarily identical to this log's `before`/`after`/`delta` fields, which are internal EMA statistics of `delayed_freq_dev_hz`. They explicitly flagged the uncertainty themselves ("Dont know if these match what you see"), which is the right instinct - these are two different measurement points on the same signal path, and while they should be causally connected (this is the literal freq_dev value that gets sent to the AD9851), the numeric correspondence isn't automatic.

**This file is again a same-boot continuation** (`K_out_2.txt` ended at `t=2470851ms`; this one starts `t=2616393ms`, gap ≈145.5s, consistent with the established cadence continuing seamlessly across all three files now). Same `delay=+2.00` throughout. 7 events, all `AUTO-CAPTURED`. Cadence and correction-gap pattern both hold exactly as before: up-to-up gaps 145530/145410/153747ms, up-to-correction gaps 8146/8146/8139ms.

**The user's guessed timestamp (`t=2923625`) is exactly right - that's a real logged event - but what THAT specific capture shows, on its own, is a modest ~5Hz dip (`before=699.85Hz after=694.59Hz`, swing -5.26Hz), not an obvious match for "-22Hz, stuck for a while."** Decoded: it's the same smooth continuous-drift category as the twenty-fourth capture - two slots flat at 800.0Hz throughout, the near-null-blended pair slowly redistributing (816.2->829.4Hz rising, 383.5->369.1Hz falling, sum roughly conserved near 1199-1200Hz) - nothing structurally different from prior small-dip cycles. Its own TREND block shows `fast` sitting perfectly flat at 702.79-702.80Hz for the full ~2s before the trigger, i.e. this was a fresh, clean onset, not a continuation of an already-elevated state. No correction followed 8s later - but that's NOT unprecedented on its own (`K_out_1.txt`'s `t=461934` small dip didn't get one either), so absence of a correction alone isn't a reliable signature of "this one stuck" - walking back a half-formed idea from partway through this analysis before it made it into a claim.

**Two honest, non-exclusive ways to reconcile the magnitude gap, neither confirmed:** (1) the already-established caveat from much earlier this session (the twenty-second capture's addendum) - a trace's `after=` value is only a snapshot at +50ms post-trigger, not the final settled value; this capture's own post-bins are still actively drifting (not leveling off) right up to the last visible bin, so if that same drift continued at a similar rate for several more seconds past the visible window, it could plausibly have carried the mean much further before whatever the user is now seeing on the SDR. (2) It's also possible the "-22Hz, stuck" state developed independently of this particular trigger - if fast and slow drifted down TOGETHER slowly enough to never re-open a 5Hz gap between each other, the auto-dump system (which only fires on fast/slow DIVERGENCE, not on absolute level) would stay completely silent while sitting at a shifted-but-internally-consistent new baseline, no matter how long that lasted or how far it had moved from the original ~700Hz reference. That would mean a genuinely "stuck" state can be real and ongoing while producing zero further log output - a real blind spot in the current design, worth keeping in mind.

**User is watching live and will send the follow-up capture once it reverts (or report if it's still stuck).** That pairing - the "-22Hz, stuck" interval's start (already roughly bounded by this message) and its eventual end - will be the most direct data yet on the long-open "why does a selected value persist" question from earlier in this session. Flagged as in-progress, not concluded.

## 2026-09-15, later still: added 'H', a second independent detector aimed specifically at the "sticks for a long time" case 'K' can't see

**User's question, verbatim intent: "We must need a different measurement of freq - it must show up on the actual freq being sent to the AD9851 given it sits there for a long time 'off freq'... How else can we detect the actual output freq as I see them?"** Also a sharp, useful methodological note: when they watch a genuinely multi-second transition on their SDR they can visually distinguish it from the "decay at start freq, build up at step freq" shape that's just their FFT/waterfall display's own exponential averaging responding to an actually-instant step - and most of the big jumps they see do NOT show that decay/build shape, i.e. most are genuinely near-instant, not slow analog transitions. Both points argue for a detector built on the RAW, undecorated `tx_freq` value (the literal integer Hz sent to `ad9851_set_frequency()`) rather than anything that itself smooths/interpolates.

**Root cause of the gap, precisely**: 'K' is a pure fast-vs-slow DIVERGENCE detector, and its own re-arm deliberately resyncs `slow` to `fast` every time a trace is read (needed to avoid a re-trigger storm - see `print_and_rearm_slow_trace()`'s own comment). If fast and slow ever drift down TOGETHER slowly enough to never re-open a gap between EACH OTHER, 'K' goes completely silent - even while sitting far from where the run actually started, for as long as that lasts. No amount of tuning 'K's own threshold fixes this; it needs a comparison against something that never gets resynced.

**Implemented (`diagnostics.cpp`/`.h`, `serial_commands.cpp`, not bench-tested):**
- `s_freq_anchor_hz` - a single, PERMANENT reference, snapped once from the fast EMA at the exact same boot-settle instant 'K's own slow EMA gets its one-time snap, and never touched again for the rest of the boot (unlike `slow_hz`, which resyncs on every 'K' read). This is "where this run's signal actually settled once warmed up," fixed for good.
- A new trigger: fires once `|fast_ema - anchor|` has been continuously above `HELD_TRIGGER_HZ` (10Hz - comfortably above ordinary noise, comfortably below the user's own ~22Hz example) for at least `HELD_MIN_DURATION_MS` (15 seconds - chosen well past how long this session's own K_out_1/2/3.txt bin-level evidence shows an ORDINARY reverting jump actually lasts, specifically so this doesn't just re-detect the same routine ~153.6s cycle 'K' already catches).
- An always-on rolling flight recorder (independent of trigger state - continuously running, not armed-around-an-event) of the actual `tx_freq` and fast EMA, 2 samples/sec, 60 seconds of history - coarse and un-smoothed on purpose, so it can't be mistaken for (or accused of reproducing) the FFT-display averaging artifact the user described.
- Three auto-prints from `diagnostics_service()` (same unconditional/mute-exempt pattern as 'K's own auto-dump): `CONFIRMED STUCK` (fires once, with the rolling trace attached) once the 15s bar is cleared; a `still stuck, Xs so far` heartbeat every 30s while it remains so (so a very long episode can't quietly end without the user noticing, or be mistaken for "no news" while ongoing); and `RECOVERED - was stuck X.Xs total` the instant it clears - but only if it had actually been confirmed first, so ordinary sub-15s excursions (the routine cycle 'K' already handles) don't generate spurious recovery spam.
- A new manual command, `'H'`, mirroring 'K's split ("not currently held" status vs. full status+trace) - a pure on-demand read, no re-arm needed (the anchor is permanent, the rolling trace always runs).

**Deliberately NOT done**: any change to 'K' itself, and no attempt to unify the two detectors into one - they're answering genuinely different questions ("did anything just change" vs. "is it sitting somewhere different from where it started, and has it been a while") and conflating them risked making both harder to reason about. Also deliberately conservative on thresholds (15s minimum, 10Hz) precisely to avoid this becoming a noisy second copy of 'K' - if it turns out to fire too rarely or too often once bench-tested, both constants are single `#define`s, easy to retune.

## 2026-09-15, later still: real-world feedback on 'H' arrives before it was even bench-tested - "-13 to +8, still sitting at +8" is below the 10Hz bar, so lowered it to 5Hz; also a good concrete example extending the "after= is just a snapshot" finding

**User's report**: watching the 700Hz tone drift from -13Hz off nominal to +8Hz off nominal, and it's still sitting at +8Hz - sent alongside three fresh K events. This is exactly the kind of case 'H' (added earlier today) was built for - a held, non-reverting offset - but at only ~8Hz off nominal, it would NOT have crossed the `HELD_TRIGGER_HZ` bar as originally set (10Hz). **Fixed**: lowered `HELD_TRIGGER_HZ` from 10Hz to 5Hz. The original reasoning for 10Hz (avoid re-firing on ordinary K-style jump-then-revert cycles) was solving the wrong half of the problem - those revert within single-digit seconds regardless of MAGNITUDE (this session's own K_out_1/2/3.txt evidence), so `HELD_MIN_DURATION_MS` (15s) is what actually filters them out, at any reasonable magnitude threshold. There was never a good reason for `HELD_TRIGGER_HZ` to exceed `SLOW_JUMP_TRIGGER_HZ`'s own 5Hz, which is already the user's own independently-established visual-read perceptual floor - reusing it directly means "anything the user could actually notice, if it's still there 15 seconds later." Caught and fixed before this ever got bench-tested, thanks to this real-world report landing first.

**The three pasted K events themselves all decode within the already-established repertoire** - nothing structurally new: `t=4161007` (delta=-12.24) is a fully flat correction trace (fast pinned at 696.59Hz for its whole visible pre-history, slow smoothly decaying toward it from 723.84Hz - the same EMA-catch-up signature documented repeatedly this session); `t=4306538` (delta=+23.41, before=700.71 after=1044.36) is another instance of the atypical +1200Hz single-bin trigger jump (400->1600 directly) cascading via a double-hop to a mostly-1600Hz cycle, byte-for-byte matching values already seen in `K_out_2.txt` - the same quantized states recurring, as expected from a deterministic two-tone/delay setup; `t=4314680` (delta=-8.51) is again a fully flat correction trace.

**Likely synthesis, offered with appropriate hedging**: the third event (the correction at `t=4314680`) is a strong candidate for being the actual transition into the "-13 to +8, still sitting" state the user is watching live. Its own `after=700.32Hz` is - per the standing "after= is only a 50ms-post-trigger snapshot, not the final settled value" caveat established earlier this session - not necessarily where things actually ended up; if the true settling continued past that visible window (undershooting toward something like -13Hz-equivalent, then rebounding and parking at +8Hz-equivalent), that would fully explain both the user's live observation and why no further 'K' trigger fired for it (the eventual +8Hz resting point is a smaller move than the 5-23Hz range 'K' already showed for this same episode, so it wouldn't necessarily re-cross the SLOW_JUMP_TRIGGER_HZ bar against a resyncing reference). Not confirmed, but a coherent, testable story - and exactly the kind of thing 'H' (now correctly thresholded at 5Hz) should catch and timestamp precisely the next time this happens, once bench-tested.

## 2026-09-15, later still: considered and ruled out - "is there anything on USB polling from the PC that might match the ~153.6s cycle?"

**User's question**, prompted by the still-unexplained ~153.6s periodicity above: could USB polling from the PC (terminal/logging side) be the source of that cadence, rather than something in the firmware itself?

**Checked directly rather than reasoned about in the abstract.** A targeted grep across the whole codebase for every USB/CDC-adjacent symbol (`USB`, `CDC`, `HWCDC`, `Serial.available/onReceive/onEvent`, `tud_*`, `usb_serial`) turned up exactly four things, none with any periodic character at all: (1) a one-time boot-delay comment in `ssb_mic_test.ino` ("give USB CDC time to enumerate before we print" - a single startup wait, not recurring); (2) the already-documented 64-byte USB CDC bulk-endpoint packet-boundary padding fix in `serial_commands.cpp` (fires per-print, not on a clock); (3) USB CDC TX-buffer backpressure/room checks in `diagnostics.cpp` (`diag_room_for()`/`availableForWrite()` throttling - a per-call safety guard, not a timer); (4) a note in the `'Z'` reboot command's own comment that `ESP.restart()` tears down USB CDC, plus a mention of "USB CDC + ADC ISR" as a catch-all bucket in Core-1 busy-time accounting. Nothing here runs on any schedule, let alone one near 150 seconds.

**Stacked against everything already established this session**: the earlier dedicated code search for a ~153.6s-capable mechanism (subagent, prior entry) already found no software timer/task subsystem anywhere in this firmware at all - `esp_timer_create`/`xTimerCreate` don't appear in the project. K/H event timestamps come from the ESP32's own `esp_timer_get_time()`, which has no dependency on USB traffic, host polling, or the terminal program - a PC-side cadence could only reach the log via that same internal clock, and would need an actual causal path into it to matter. The two-tone signal generation itself is entirely internal and tick-driven (`SAMPLE_RATE_HZ`-clocked, no host-data dependency), so there's no route for USB traffic to influence the real DSP/RF chain even if it were periodic. And no standard USB or OS-level polling interval is known to sit anywhere near 153.6 seconds - USB frame/microframe timing is sub-millisecond, CDC/serial polling on the host side is typically tens-of-ms at most, and nothing in that stack normally operates on a two-and-a-half-minute cadence with the <0.3% precision measured here.

**The strongest single argument against it**: the user has independently observed the same phenomenon on the SDR - a real, transmitted RF frequency shift, not just something showing up in the diagnostic log. For USB polling to be the cause, it would need a causal path from host-side USB activity into the actual phase/frequency DDS output, and this architecture doesn't have one - USB here is purely a diagnostics/print channel, decoupled from the signal-generation path. That reframes the question: even if some USB-side process were somehow found to run every ~153.6s, it still couldn't explain an on-air frequency shift unless a second, currently-unknown coupling mechanism into the RF chain also existed - at which point USB polling wouldn't be the interesting finding, that coupling would be.

**Verdict: ruled out as a likely cause**, though not disprovable by code-reading alone in the way "no timer subsystem exists" was. **Concrete test offered, if the user wants full certainty**: power the board from a separate supply through a charge-only (data-disconnected) USB cable for a controlled window spanning several expected ~153.6s cycles, then reconnect and check whether 'K'/'H' state (both latch and hold indefinitely until read, so nothing is lost by not being connected to observe live) shows the cadence continued uninterrupted through the USB-blackout period. If it does, USB is conclusively cleared; if the cycle stops or resets during the blackout, that would be a first real clue tying it to the USB link after all. Not run yet - offered as the next concrete step if this thread is picked back up. Top open question (what actually runs on a ~153.6s clock) remains open.

## 2026-09-15, later still: fresh run's log decoded - why didn't 'H' fire for a user-observed "-6Hz to +3Hz" excursion?

**New capture, fresh boot** (timestamps start at `t=106902ms`, far lower than any prior file - this is a new run, not a continuation). Six 'K' auto-captures plus two on-demand 'H' status checks across the visible ~625s window. Decoded:

- `t=106902` (delta=-18.28, before=721.07, after=702.79): an ordinary big-swing correction-type event, nothing new.
- `t=156141` (delta=+18.90, before=700.47, after=1002.27): another instance of the atypical +1200Hz single-bin trigger jump (400->1600Hz directly), same recurring type documented in earlier files.
- `t=164278` (delta=-10.59, before=708.42, after=697.83): the ~8.14s-later correction for the above (164278-156141=8137ms, matches the established `FREQ_EMA_WARMUP_MS`-driven correction lag exactly) - completely flat trace, textbook EMA catch-up.
- **First 'H' status check** (sent shortly after): `anchor=699.09Hz fast=701.56Hz dev=+2.47Hz` - not held, correctly, since 2.47Hz is well under the 5Hz bar. **One oddity worth flagging**: the accompanying 60s rolling `held_trace` is flat at `fast=697.83-697.84Hz` for its first ~53.5s, then shows a single isolated sample at `held_trace[-107]` of `fast=719.31Hz` - a ~21Hz spike lasting exactly one 0.5s sample before immediately reverting to a new flat plateau around `697.21-697.22Hz` for the rest of the trace. A real 21Hz excursion that reverted within 0.5s would be far faster than any transition this session has ever bin-decoded (the fastest confirmed transitions still span multiple bins at 1.24ms/bin, i.e. tens of ms, not instantaneous). More likely explanation, offered with appropriate hedging: this is a **torn/non-atomic read of the `fast` EMA float across the two cores** (the held_trace ring buffer is written on Core 1's per-tick ISR-adjacent path while presumably read back for printing on whatever core services the 'H' serial command) rather than a real 21Hz event - a single anomalous sample sandwiched between two stable plateaus, with no corresponding bin-level or K-trigger evidence of any transition at that moment, fits a data-race glitch better than a real signal event. Flagged as a possible diagnostic-integrity issue worth checking (whether `s_fast_ema` reads/writes need to be made atomic/volatile-safe), not treated as a confirmed real excursion.
- `t=309796` (delta=+19.36, before=700.62, after=1093.76): another +1200Hz-type jump, but with a **new escalated variant** in the post-trigger cascade: bins `+26` through `+40` climb well past the usual 1600Hz ceiling (`1935.5, 1664.5, 1720.9, 1745.8, 1854.2, 1673.3, 1926.7Hz`) - the highest bin values seen anywhere this session. Worth watching for recurrence; not yet enough data to characterize as a distinct type versus an extreme tail of the existing cascade behavior.
- `t=317942` (delta=-11.20, before=709.04, after=697.84): the matching ~8.15s correction (317942-309796=8146ms), again completely flat.
- `t=463468` (delta=+18.81, before=700.57, after=1042.81): another +1200Hz-type jump.
- `t=471612` (delta=-6.71, before=708.88, after=702.17): the matching correction, flat trace, settling at `fast=702.17-702.20Hz` - `dev` from anchor (699.09Hz) = **+3.08Hz**.
- **Second 'H' status check**: `anchor=699.09Hz fast=700.32Hz dev=+1.23Hz` - not held; this trace's 120-sample history is clean, no spike this time (all `695.96-695.97Hz`).
- `t=617140` (delta=+17.20, before=700.38, after=1014.03): another +1200Hz-type jump, again with the escalated >1600Hz cascade tail seen at `t=309796` (`1610-1633Hz` range in the continuous-drift slots).
- `t=625290` (delta=-11.68, before=708.89, after=697.21): matching correction, flat, settling at `dev=-1.88Hz` from anchor.
- Up-to-up gaps for the three +1200Hz-type jumps: `309796-156141=153655ms`, `463468-309796=153672ms`, `617140-463468=153672ms` - the ~153.6s periodicity holds rock-steady yet again, now spanning a brand-new boot/run, which itself is useful evidence: **the cadence isn't tied to elapsed-since-boot in an absolute sense reset by anything - it re-establishes the same ~153.6s period from scratch on a fresh run**, consistent with it being driven by something in the two-tone/delay signal path itself rather than any one-time boot-relative event.

**Directly answering the user's question - why didn't 'H' fire for the reported "-6Hz to +3Hz" excursion**: checking every `fast` EMA value actually captured in this log - both 'H' status snapshots (`dev=+2.47Hz`, `dev=+1.23Hz`), both 'H' rolling traces (aside from the single suspect spike above, everything is within `695.96` to `702.20Hz`, i.e. `dev` of -3.13Hz to +3.11Hz), and every K correction's settled `after` value (`dev` of -1.88Hz to +3.08Hz) - **`|dev|` never reached the 5Hz `HELD_TRIGGER_HZ` bar anywhere in this capture**, so 'H' had no reason to fire regardless of how long anything sat still. The largest positive excursion actually measured (`+3.08Hz` at `t=471612`) is a plausible match for the "+3Hz" half of the user's report. There isn't a clean match for "-6Hz" anywhere in this log's `fast`/`after` values (the closest is -2.79Hz) - two honest possibilities, not resolved here: (1) the user's "-6Hz" was a rougher visual read on the SDR than the true excursion, and the real dip was smaller (in the -2 to -3Hz range this log shows, matching the same character); or (2) there's a real gap between what `fast`/`freq_dev_hz`-derived measurements see and the actual transmitted frequency the SDR displays - the same open concern raised earlier this session ("we must need a different measurement of freq") - in which case 'H', built on the same `fast` EMA as 'K', would inherit that same blind spot and this wouldn't be a threshold-tuning problem at all. Not enough evidence here to pick between these; flagging both rather than guessing.

## 2026-09-15, later still: partial answer to "does 'fast' actually track the real two-tone frequency?" - confirmed good while sitting quiet at nominal

Following the clarification on what `'H'`'s two printed values mean (`tx_freq` = the literal RF carrier sent to the AD9851, `fast` = the tau=50ms EMA of `delayed_freq_dev_hz`, the intended ~700Hz-tone estimate), the user reported the SDR reading "exactly on freq" (i.e. no perceptible offset from the nominal 700Hz tone) at the same moment `'H'`'s trace showed `fast=703.41Hz` (`held_trace[-1]: tx_freq=14201359Hz fast=703.41Hz`).

**This is a real, useful data point, not just a repeat of the `'s'`-tone calibration check**: unlike `'s'`, this was taken in two-tone mode, so it's the first direct SDR-vs-`fast` comparison actually made under the beat-null-crossing conditions this whole investigation is about. A ~3.4Hz reading being perceived as "exactly on freq" tells us the practical, human-perceptible tolerance on the SDR is at least a few Hz (unsurprising - a few Hz on a 700Hz tone is a small fraction of a percent, and well under half the frequency resolution most SDR waterfalls display at). It also fits neatly with everything already logged: `HELD_TRIGGER_HZ`/`SLOW_JUMP_TRIGGER_HZ` are both set to 5Hz specifically because that's around the user's own established visual-read floor, and this observation is consistent with that number rather than contradicting it - a few Hz reads as "on freq," which is exactly why the trigger bar sits just above it.

**What this does confirm**: `fast` is not obviously biased or broken while the signal is sitting quiet at its nominal resting point - it reads close enough to 700Hz that the SDR agrees it's "on freq." **What this does NOT yet confirm**: whether `fast` stays similarly trustworthy during/immediately after the null-crossing-heavy transition period of an actual jump, which is the condition where the earlier "-6Hz to +3Hz didn't trigger H" question actually lives, and where the established null-bias mechanism (instability specifically AT beat nulls, not during quiet dwell) would be expected to matter most, if it matters at all for these larger multi-Hz-to-hundreds-of-Hz jumps rather than just the sub-Hz absolute-frequency biases the null-bias investigation was originally about. Not resolved - flagged as the natural next data point if the user can catch a `'H'`-vs-SDR reading during an active jump or its immediate aftermath, rather than only at rest.

## 2026-09-15, later still: are there cross-core race hazards in the actual DSP/filter path, not just diagnostics?

Following the single anomalous 21Hz-for-one-sample spike found in an 'H' trace (attributed to the diagnostic ring buffer's index+data pair, not a real transmitted glitch), the user asked the sharper, more important question: are there variables the *real* DSP/filter code depends on that could fall into the same category? Delegated a targeted code audit (general-purpose agent) covering every cross-core boundary in the project. Architecture confirmed: `dsp_task` (the entire per-sample chain - ADC read -> `ssb_dsp_process_sample()` -> envelope floor/gdeq/ampeq -> predistort-or-linear PWM mapping -> `relative_delay_apply()` -> AD9851/PWM writes) is `IRAM_ATTR` and pinned to Core 0; `loop()`/`handle_serial_commands()`/`diagnostics_service()` run on Core 1. There are no locks, critical sections, or a settings queue anywhere in the project - every cross-core handoff is bare `volatile` scalars. That's safe for a single scalar (ESP32/Xtensa 32-bit aligned reads/writes are atomic - a lone `float` can't literally tear), but several places update **two or more related variables** with separate, unsynchronized stores, which the hot path then reads together on the very next tick.

**Findings, ranked by plausibility of a real transmitted-signal effect (not just a cosmetic print)**:

1. **`envelope_output.cpp` (`s_env_pwm_offset` / `s_env_pwm_scale`) - HIGH, genuinely new.** Read together on every sample (`ssb_mic_test.ino`'s envelope->PWM-duty mapping, used whenever predistort is off, i.e. by default) as `envelope * scale + offset`. Both are independent `volatile float`s set by separate calls; preset load writes them back-to-back with zero atomicity between the two. A `dsp_task` tick landing between the two stores gets new-scale-with-old-offset (or vice versa) for exactly one sample - a real, momentary wrong PWM duty in the transmitted envelope. Not discussed anywhere in the code's own comments.
2. **`test_signals.cpp` (`s_tone1_hz`/`s_tone2_hz` and their amplitude counterparts) - HIGH, and directly relevant to this whole investigation.** `'T'` (band switch) and `'R'` (tone ratio) each write a *pair* of independent `volatile float`s in two sequential stores; the two-tone generator (Core 0 hot path) reads both every sample. A tick landing mid-update gets a mismatched tone-pair frequency or amplitude pair - i.e. genuinely distorted two-tone content feeding straight into the same Hilbert/phase-derivative chain that computes `freq_dev_hz`. Since this session's own bin-level evidence already shows near-null envelope perturbations translate into instantaneous-frequency artifacts, this is a plausible (if keypress-timed, so rare and user-action-linked) contributor - though notably NOT a candidate for the unprompted "hands-off" jumps, since nothing in this pair changes without the user pressing `'T'`/`'R'`.
3. **`envelope_gdeq.cpp` / `ssb_dsp.c`'s EQ-enable-and-reinit pattern - MEDIUM-HIGH, previously suspected but not actually ruled out.** Flip a volatile enable/variant flag, then re-init a multi-field filter struct (coefficients + delay-line state) across several non-volatile stores, while `dsp_task` calls the matching `*_process()` unconditionally the instant it sees the flag flip. **Important**: `moving_forward_notes.md` (2026-09-09) already floated almost exactly this hypothesis for preset-switch behavior, and a "canary" was added to try to catch it - but that canary only checks `isfinite()`, so it cannot detect a torn-but-finite value (a real coefficient paired with stale filter memory would still pass an `isfinite()` check). **This means the earlier "no MISMATCH found" canary result did not actually rule this hypothesis out** - it was checking for the wrong failure signature.
4. **Preset-load's ~15-setter partial-application window - MEDIUM, already known/investigated**, not new; already attributed in this project's notes to legitimate per-preset near-null reshaping rather than corruption.

**Checked and found clean**: `relative_delay.cpp`'s single `volatile float` (read once into a local, no compound hazard); `envelope_interp.cpp`, which explicitly avoids this whole bug class (cross-core writers only touch single-word flags, the actual ramp state is Core-0-only); `adc_capture.cpp`'s lock-free SPSC ring buffer (standard, correct pattern); no `double`s exist anywhere in shared cross-core state project-wide.

**Bottom line, stated with appropriate hedging**: two genuinely new hazards sit directly in the transmitted-signal path (items 1-2), and one already-suspected hazard (item 3) turns out to still be unresolved rather than disproven, because the existing detection canary checks the wrong thing. None of these are proven to have caused any of the specific jumps logged this session - they are plausible candidates, not confirmed causes - and their natural failure mode (a single bad DSP tick, sub-millisecond) doesn't obviously explain a multi-second-to-many-second SUSTAINED offset the way 'K'/'H' events look; they're a better fit for an occasional brief click/glitch than for the "-6Hz to +3Hz" or "stuck for a while" style events. Recommended next step if pursued: make the two-value updates in items 1 and 2 atomic (write into a local struct, then a single pointer-swap or a `portMUX`-protected assignment) and see if that changes anything measurable; and fix the canary in item 3 to compare an actual computed sample against a reference rather than just checking `isfinite()`.

## 2026-09-15, later still: a genuinely new kind of mismatch - user reports "-35Hz, still there" but 'H' shows nothing

**The user's live report**: tone jumped to -35Hz off nominal and was still sitting there when they ran `'H'` right after. **What the log actually shows**: two more ordinary +1200Hz-type K cycles (`t=1718983`, `t=1872620`, up-to-up gap 153637ms - the periodicity holds yet again) each with their usual ~8.15s flat correction (8143ms, 8153ms), nothing unusual. Then the `'H'` status, sent apparently moments after the correction at `t=1880773` finished printing (the "Command sent: H" line is interleaved mid-print, inside that correction's own TREND block output - the two commands' outputs got mixed in the serial stream, though this doesn't affect the data's validity, just its print ordering): `anchor=699.09Hz fast=695.34Hz dev=-3.74Hz` - **not held**, and the accompanying 60-second rolling trace is dead flat at `fast=700.93-700.94Hz` (dev +1.85Hz) for the entire visible 60s window, no dip anywhere close to -5Hz let alone -35Hz.

**This is the single largest disagreement between a user SDR observation and every internal diagnostic this session has produced.** Every previous "didn't match" case (the -6/+3Hz report, the earlier USB-polling question) involved internal readings in the same single-digit-Hz ballpark as the report. Here the report is -35Hz and the internal reading - from two independent code paths (`'H'`'s live status AND its 60s trace) - shows nothing beyond +1.85Hz. Two honest possibilities, not resolved:

1. **Timing gap, not a measurement failure.** The 60s trace only reaches 60 seconds back from the moment `'H'` was typed and sent - if the user's -35Hz observation happened, then reverted, more than 60s before they actually got to a keyboard and sent `'H'`, the trace would legitimately show nothing, and "still there" would have stopped being true by the time it was checked. Given how fast some of this session's transitions have been shown to revert (single-digit seconds in most bin-level traces), 60+ seconds of continuous SDR-watching-then-typing is plausible.
2. **A genuine, previously undemonstrated measurement blind spot.** If the tone really was sitting at a steady -35Hz shift, `delayed_freq_dev_hz` (a phase-derivative of a now-steady-but-shifted tone) should read close to a steady -35Hz, and the tau=50ms `fast` EMA should converge to within a few Hz of that within a few hundred milliseconds - not sit at +1.85Hz for 60 continuous seconds. If option 1 is ruled out (e.g. next time, by running 'H' within a few seconds of seeing the jump, or reading the live serial output continuously so timing is unambiguous), this would be strong evidence that something between the audio-domain `freq_dev_hz` computation and the actual AD9851 output frequency is capable of shifting the real transmitted frequency without leaving a trace in `freq_dev_hz` at all - a fundamentally different (and more serious) problem than the null-crossing bias this investigation started with, and one that would mean `'K'`/`'H'` cannot be trusted to catch every real excursion.

**Not enough evidence yet to pick between these.** The clean, decisive next experiment: the moment a hands-off jump is seen and confirmed sustained on the SDR, send `'H'` immediately (within a couple of seconds) rather than after any delay, and note the wall-clock gap between "I see it" and "I sent H." If `'H'` still shows nothing even with a sub-5-second gap, option 2 becomes the leading explanation and this becomes the top-priority thread for next session - bigger than the ~153.6s periodicity question, since it would mean the *entire diagnostic apparatus built this session* has a real, sometimes-active blind spot.

## 2026-09-15, later still: the -35Hz mystery sharpens - no separate K auto-capture fired either, and 'H' gets timestamps

Two follow-ups from the user on the -35Hz report. First: the K event the user believes corresponds to the visual "-35Hz, still there" jump is the one printed **immediately before** the `'H'` command - which, walking the log precisely, is the correction at `t=1880773` (before=708.94, after=700.32), interleaved mid-print with "Command sent: H" (the command physically interrupted that correction's own TREND-block output stream) - meaning the true gap between that K print and the `'H'` read is provably tiny, likely well under a second, not the "maybe 60+ seconds of not looking at the keyboard" scenario floated as the leading candidate last time. Second: **there was no separate K auto-capture at all for whatever produced the -35Hz observation** - only the two already-logged events (the `t=1872620` up-jump and its `t=1880773` correction), neither of which shows anything resembling a -35Hz swing in its own numbers.

**This meaningfully shifts the balance between the two standing hypotheses.** The "timing gap" explanation (option 1 from the prior entry) required either a long unaccounted delay before `'H'` was read, or a K trigger quietly having happened and gone unmentioned - both look much less likely now: the H-vs-K timing gap is now known to be near-zero in at least this one instance, and "no auto-capture" is a genuine negative result (the auto-dump fires unconditionally every `loop()` iteration once latched - it cannot silently "not get around to" printing something that already triggered). That leaves two live possibilities: (a) the correction at `t=1880773` genuinely *is* the moment in question, and its own bins/trend being completely flat/already-reverted directly contradicts a simultaneous "-35Hz, still there" SDR reading - which would be a real, demonstrated instance of the internal measurement (`freq_dev_hz`/`fast`) disagreeing with reality at the exact moment being checked, not just a stale read; or (b) the actual "-35Hz" moment was the ~8.15s dwell *between* the up-jump (`t=1872620`) and its correction (`t=1880773`) - during which the trace's own bins cascade through the quantized 800/1200/1600Hz null-adjacent states already characterized earlier this session - and the SDR's own display averaging carried a "still there" impression a couple seconds past when the correction's flat trace says it had already reverted. (b) would be a much smaller, already-partially-explained effect (display lag, not a measurement blind spot); (a) would be the significant new finding. The two are not yet distinguishable from what's been captured.

**Fixed the actual friction the user flagged** ("shame the H output is not timestamped"): every `'H'`-related print (`diagnostics_print_held_status()`'s on-demand read, and the auto-fired `CONFIRMED STUCK`/`still stuck`/`RECOVERED` lines in `diagnostics_check_held_freq()`) now carries an explicit `t=%ums`, directly comparable to a `'K'` trace's own `"at t=%ums"`. The on-demand read's timestamp is when that specific `'H'` command was processed; `CONFIRMED STUCK` additionally reports `started_at` (when the deviation first crossed `HELD_TRIGGER_HZ`, the closest equivalent to a K trigger's own "at t="). Going forward this removes the ambiguity that made this whole exchange hard to pin down - the next time a jump and an `'H'` read happen close together, the exact gap will be computable directly from the log rather than inferred from print-interleaving accidents. Not yet bench-tested (no compiler in this sandbox, per the standing caveat).

## 2026-09-15, later still: user pushes back hard on the hedging, proposes a persistent-memory-corruption mechanism - checked against the actual code, and it's a genuinely strong lead

The user directly challenged the accumulating "measurement gap, not yet resolved" framing: they've reported multiple sustained (well beyond seconds) held states, pointed at logs at the time, and in each case the internal data hasn't shown it - "we are not looking at the same reality." They also pushed back on treating this as noise: the jump magnitudes recur too similarly to be random, which reads as numerics/software rather than analog noise. Their specific hypothesis: a cross-core race corrupts a specific byte/word of something with actual MEMORY (not a value freshly recomputed each tick from clean inputs), which is why it could stay corrupted rather than self-heal within one sample the way the earlier dual-store races (envelope_output's scale/offset, test_signals' tone pairs) would. They also floated that a null crossing (rare) might be the trigger that opens the race window.

**Checked this rigorously with a second targeted code audit, specifically hunting for persistent (tick-to-tick, feedback/accumulator) DSP state and whether any of it is cross-core-touched.** Results:

1. **The phase/frequency-generation path itself is NOT raced.** The Hilbert transform's FIR delay line, the `prev_phase` phase-difference memory, the two-tone oscillators' own phase accumulators (`s_tone1_phase`/`s_tone2_phase` in `test_signals.cpp` - distinct from the already-flagged cross-core `s_tone{1,2}_hz`/amplitude inputs, which ARE raced but don't touch these), and the `freq_dev_hz` slew-limiter's own feedback state are all written exclusively from `dsp_task`/Core 0 - confirmed by grep, no other caller touches them. This part of the hypothesis, as stated, isn't supported by the code: nothing with memory on the frequency side can be corrupted by a cross-core race.

2. **But the envelope-domain IIR filters are raced in exactly the shape the user described.** `envelope_gdeq.cpp`'s two allpass sections (`x1`/`y1` group-delay-EQ memory) and `envelope_ampeq.cpp`'s two shelf biquads (`x1`/`x2`/`y1`/`y2`) are both non-atomically zeroed/reinitialized by `_set_enabled()`/`_set_variant()` calls running on Core 1 (serial commands / preset load), while `dsp_task` on Core 0 calls their `_process()` functions unconditionally every tick, gated only by a single bool. Unlike the earlier races, this state genuinely IS carried forward tick-to-tick as feedback - it is not recomputed fresh from clean inputs, so a bad value written here doesn't automatically self-heal on the next sample the way a stale tone-pair read does.
3. **This state is entirely envelope-side and structurally invisible to `freq_dev_hz`/K/H.** This project is EER/polar: phase/frequency comes from the AD9851 DDS (`carrier_output_set_freq_dev()`, confirmed fully stateless - recomputed fresh from `carrier_hz + freq_dev` every call, no software phase accumulator to corrupt there either), while envelope drives the PWM/analog amplitude path; the two only recombine physically at the PA, never digitally in this firmware. K/H are built entirely on `delayed_freq_dev_hz` - the phase side. A corrupted envelope-EQ state could not directly retune the DDS, but **the project's own code already anticipates a path from envelope corruption to something that reads as a frequency artifact**: a `settings.h` comment (from the original AM-test design) explicitly names "genuine AM-to-PM crosstalk somewhere physical" as the explanation if FM-looking sidebands ever appeared despite `freq_dev_hz` staying at zero. A badly-behaved envelope right at a null (exactly where this whole investigation's null-bias work already lives) is a very plausible way to produce that crosstalk on the real transmitted signal while every internal `freq_dev_hz`-based diagnostic stays clean throughout - which would fully explain the "we're not looking at the same reality" observation.
4. **"Gets corrupted and stays corrupted" - the honest nuance.** If the race merely produces a stale-but-finite value, a BIBO-stable filter's own math would decay it back toward normal over its own pole-dependent time constant - potentially many seconds for a slow filter, well beyond a single tick, but not literally forever. If the race instead produces a genuinely non-finite (NaN/Inf) value, ordinary linear IIR feedback does NOT recover from that on its own - it stays broken indefinitely until the next explicit `_set_enabled()`/reinit clears it. **Important check, not yet done**: this project already has a canary system (`diagnostics.cpp`'s `canary_check_background()`, added 2026-09-11, called unconditionally every `loop()` iteration) that checks `envelope_gdeq_get_canary()`/`envelope_ampeq_get_canary()` for exactly this - `isfinite()` on the filter state - and would print `[canary] gdeq allpass state MISMATCH` or `[canary] ampeq shelf state MISMATCH` the very first time it ever went non-finite. **I don't recall this line appearing in any log shared this session.** If it truly never has, that's evidence against the sharpest (NaN-poisoning) version of this theory specifically - though it would NOT rule out the milder stale-but-finite version, since that never trips `isfinite()` at all. Worth the user explicitly checking: has `[canary] ... MISMATCH` (as opposed to `OK`) ever appeared? Running `'c'` (manual `canary_print_status()`) right after/during a live stuck event would be a clean, decisive test either way.
5. **The null-crossing-as-trigger idea**: found one small, real branch in `ssb_dsp.c` that only executes extra float operations near a null (inside the `have_prev_phase` block, gated on `envelope < null_bias_threshold`), but nothing that looks like a meaningfully different code shape (no extra loop/sqrt/atan2 iteration) that would obviously perturb Core 0's per-tick timing enough to open a race window. Can't rule this out, but no strong evidence for it either - the "genuinely unstable near a null" behavior already documented this session is the signal's own physics, not a special-cased code branch that takes measurably longer.

**Bottom line, stated plainly rather than hedged**: the user's core instinct - that this looks mechanistic/numeric rather than analog noise, and that something with real memory is involved - is well supported by what the code search actually found. The specific mechanism (envelope-domain IIR filter memory, raced from Core 1 reinit against Core 0's live tick, feeding into the RF signal via AM-to-PM crosstalk near a null rather than through `freq_dev_hz`) is a genuinely strong, previously-undocumented candidate that would explain simultaneously why the internal diagnostics keep looking clean, why the events sustain rather than instantly reverting, and why the magnitudes might cluster (a filter settling into a small number of "attractor" states rather than producing continuously random noise) - though that last point (specific recurring magnitudes) hasn't been directly connected to this mechanism yet, only argued for narratively. Not proven - the canary check above is the next concrete, cheap way to gather real evidence for or against it.

## 2026-09-15, later still: "never seen a bad canary" + the envelope-weighting question - these two facts together complete the theory

Two things from the user in quick succession. First: they've never seen a bad canary output (will keep watching). Second, a sharp architectural question: is the RF output frequency essentially a power/energy-weighted function of the frequency actually sent to the AD9851 - i.e. `f(freq(t), envelope(t))` rather than a plain average of `freq_dev_hz` - such that an apparent offset from nominal could come from either a sustained real frequency error OR an amplitude/envelope error (with frequency itself never actually wrong)?

**Both answers combine into the cleanest, most complete explanation this investigation has produced.**

On the canary: this is now good news for the theory, not bad. The envelope-weighting mechanism below only needs the envelope to be quantitatively WRONG (elevated when it should have dipped, or shaped differently) - it does not need to go non-finite (NaN/Inf) to matter. The canary only checks `isfinite()`. A stale-but-finite corrupted allpass/shelf-filter value (the milder of the two failure modes flagged in the last entry) would never trip it, so "never seen a bad canary" is fully consistent with - not evidence against - a wrong-but-finite envelope-EQ state being the culprit. This resolves the earlier tension cleanly.

On the frequency question: **yes, essentially correct, and it explains everything.** What an SDR/receiver actually displays as "the tone's frequency" is determined by the real analog signal - envelope(t) amplitude-modulating a phase/frequency-modulated carrier at `freq_dev(t)` - and any frequency-domain view of that (an FFT, a waterfall, a human eye tracking a spectral line) inherently weights each instant's frequency content by how much SIGNAL ENERGY (envelope amplitude) it carries at that instant. `freq_dev_hz`/`fast`/`K`/`H`, by contrast, are a PLAIN, envelope-blind time-average of the phase derivative alone - they have no concept of amplitude weighting at all. This is precisely the missing piece connecting everything found today:

- This session has established from the start that `freq_dev_hz` is "genuinely unstable near beat-envelope nulls" - a known, expected property. It has ALSO always been implicitly assumed this doesn't matter much for the real transmitted signal, because envelope amplitude is naturally near-zero exactly when this instability happens - so on an energy-weighted view (what the SDR actually shows), these moments contribute almost nothing, and the instability stays effectively invisible/inaudible under normal conditions.
- **If the envelope is wrong specifically during those near-null moments** - e.g. staying elevated instead of correctly dipping toward zero, which is exactly the failure mode a corrupted `envelope_gdeq`/`envelope_ampeq` filter state (the cross-core race flagged in the previous entry) could plausibly produce - then the near-null frequency instability that used to be masked by low envelope suddenly gets real energy weight in the transmitted signal. The underlying `freq_dev_hz` computation never has to be wrong at all for this to happen; only the WEIGHT given to its normal, always-present near-null excursions has to change.
- This explains, in one mechanism, everything that's been hard to reconcile today: (1) why `freq_dev_hz`/`fast`/K/H can look completely clean throughout a real, SDR-visible "stuck" event - because the frequency-side math never actually carried an error, only the envelope-side weighting did; (2) why the canary never fires - a wrong-but-finite envelope value doesn't need to be non-finite to unmask an existing excursion; (3) why the magnitudes recur similarly rather than looking like random noise - unmasking an already-characterized, quantized near-null instability (this session has repeatedly found it snapping to specific values like the 400/800/1200/1600Hz bin pattern) would reveal the SAME deterministic pattern each time, not fresh randomness; (4) why it "sticks" for many seconds rather than reverting instantly - matches the earlier-established idea that a corrupted filter's own feedback state can persist for a duration tied to the filter's own time constant, not a single tick.

**Caveat, stated honestly**: this is a coherent hypothesis that fits every piece of evidence gathered today, not a proven mechanism - nothing here has directly measured envelope amplitude during a live stuck event and shown it elevated where it should dip. The literal claim "RF freq = power-weighted average of freq(t)" is a useful working mental model for why energy-weighting matters, not a rigorous formula for the instantaneous frequency of a combined AM/PM signal - but as a reason why an amplitude-only error could produce an apparent frequency shift with the phase/frequency path never being wrong, it holds up. **This is now the leading, most complete theory in the whole investigation.** The concrete next test, building on the last entry's proposal: during a live stuck event, in addition to `'V'` (canary/diagnostic snapshot), it would help to also compare envelope readings (if there's a way to read `env_min`/envelope amplitude live, e.g. from a 'K'-style trace) against what's expected near that point in the beat cycle - if envelope is measurably NOT dipping the way it normally should during the stuck period, that would directly confirm this mechanism.

## 2026-09-15, later still: does the code actually preserve the joint freq+ampl "integral" near a null? Traced the pipeline - answer is no, and it's confirmed by the codebase's own history

Direct follow-up on the energy-weighting theory: given that perceived RF frequency depends on freq(t) weighted by envelope(t), how does the null-region "correction" code in this project ensure that joint relationship stays correct? Traced the actual per-tick pipeline in `ssb_mic_test.ino` (lines ~571-659) precisely:

1. `ssb_dsp_process_sample()` (or a test-signal generator) produces `freq_dev_hz` and `envelope` TOGETHER, as a matched pair, from the same instant.
2. `envelope_floor_apply()` (envelope-only affine remap, off by default - floor=0.0, toggle `'x'`/`'z'`) - runs on envelope alone.
3. `envelope_gdeq_process()` (envelope-only allpass group-delay equalizer, off by default, toggle `'g'`) - envelope alone.
4. `envelope_ampeq_process()` (envelope-only shelf EQ, two independent stages, both off by default, toggle `'a'`/`'A'`) - envelope alone.
5. Only AFTER all three of the above does `relative_delay` bring `freq_dev_hz` and `envelope` together - and even then, purely for TIME alignment (a bipolar fractional-sample delay compensating differential propagation delay between the two paths), not for any check on their joint amplitude/phase relationship.

**At no point does anything in this chain adjust `freq_dev_hz` in response to what the envelope-only stages did, or verify that the pair is still physically consistent.** There is no mechanism, anywhere, that computes or checks anything like "does this envelope value, combined with the phase's current rate of rotation, still represent a sane instant of the intended signal." Each envelope-domain stage is tuned and validated independently (group-delay dispersion via a swept-tone TF analyzer for gdeq, IMD/insertion-loss for ampeq) - never against the combined, joint envelope+phase output specifically near a null.

**This asymmetry isn't an oversight - it's confirmed by `envelope_floor.cpp`'s own documented history.** The ORIGINAL design (NOTE 1/NOTE 2 in that file) DID try to correct both sides together: freeze `freq_dev_hz` near a null AND hard-clamp envelope near a null. Real-hardware testing found the frequency-side freeze was actively wrong and removed it - the file's own postmortem states plainly that "the fast phase rotation right at a destructive-interference null is genuine required signal content... not noise to suppress," because a true envelope zero-crossing can only be represented by phase sweeping through a large angle quickly there. That fast rotation is only correct/non-distorting **when actually paired with envelope genuinely near zero** - which is exactly the pairing every one of steps 2-4 above can break, each independently, whenever engaged: raising the envelope floor, or a correctly-functioning-but-nonzero gdeq/ampeq reshaping, all keep envelope away from true zero at exactly the moments the phase is doing its required fast, wide-angle rotation. Per today's energy-weighting theory, that mismatch - real (if reduced) amplitude coincident with an extreme, normally-masked instantaneous frequency - is precisely the mechanism that would inject genuine, receivable spectral energy at that extreme frequency, something a true near-zero envelope would have kept below the noise floor.

**Important scope caveat, worth confirming directly**: `envelope_floor` defaults to 0.0 (no-op), and `envelope_gdeq`/`envelope_ampeq` both default to off, and (per `ssb_mic_test.ino`'s own comment) `envelope_gdeq_process()`/`envelope_ampeq_process()` fully no-op when disabled - meaning the earlier-flagged cross-core race in their enable/reinit sequence can only matter while one of them is actually toggled ON. **Question for the user, needed to know whether this mechanism is even live in the sessions where jumps were observed: was `'g'` (group-delay EQ), `'a'`/`'A'` (amp EQ), or the envelope floor (`'x'`/`'z'`) active during the captures that showed the stuck/held states?** If everything was left at its off-by-default state throughout, this specific pipeline gap - real as it is - can't be the active cause of what's been observed, and the search should stay focused on the phase-side/timing possibilities instead (e.g. `relative_delay`'s own null-adjacent sample-pair selection, already established early this session as the actual transmitted frequency-shift mechanism). If any of those toggles WAS active, this becomes a very strong, concrete, mechanistically-confirmed candidate - not a race or a bug, but a known-asymmetric design decision whose consequence, in light of today's energy-weighting insight, appears not to have been fully evaluated before.

## 2026-09-15, later still: user's direct scope observation - null depth varies slowly and correlates with K triggers; checked a float32-NCO-drift explanation numerically (doesn't fit); found and fixed a real blind spot in env_min's own print precision

**The user's observation, watching the envelope waveform directly on a scope**: the null's minimum level visibly varies over time (not fixed), and it's "pretty sure" the deepest nulls are what triggers `'K'`. These null levels vary "quite slowly." They also note all three envelope filters (`'g'`/`'a'`/`'A'`) are ON, and suspect filter-induced sample jitter means the null doesn't land at a fixed position relative to the sample grid cycle to cycle. No big jump was seen during this direct watching, and envelope itself never showed a visible glitch - "over many cycles the freq is very stable."

**This is an important, mechanistically clean observation and it fits the whole investigation well.** A deeper null means a more extreme required phase rotation right at that instant (per `envelope_floor.cpp`'s own established physics: a true zero-crossing needs phase to sweep a large angle quickly, and how large depends on how close to true zero the envelope actually gets) - so a null that happens to dip deeper than usual would produce a bigger momentary `freq_dev_hz` spike, more likely to move the fast EMA enough to cross `SLOW_JUMP_TRIGGER_HZ`. This requires no race, no corruption, no exotic mechanism - it's a direct, expected consequence of physics already established this session, and it reframes "what triggers K" as fundamentally about null DEPTH variability rather than anything else.

**Checked one candidate explanation for the depth varying slowly: float32 rounding drift in the two-tone oscillators' own phase accumulators (`test_signals.cpp`'s `s_tone1_phase`/`s_tone2_phase`, confirmed float32, wrapped every tick).** This is a real, well-known DSP phenomenon - the per-tick increment (`two_pi * f / SAMPLE_RATE_HZ`, evaluated in float32 every tick) carries a small, fixed representation/rounding error that accumulates linearly over time, causing the true null position to slowly precess relative to the sample grid - and critically, since both accumulators reset to exactly 0.0f at boot, this would deterministically re-run identically every boot, matching the already-established fact that the ~153.6s cadence re-establishes itself from scratch on a fresh run. **Simulated it directly** (float32 accumulation vs. double-precision ground truth, 200 seconds of ticks at 16kHz): the actual empirical drift rate is about +1.0e-3 rad/s for the 700Hz oscillator and -1.55e-3 rad/s for the 1700Hz one, implying a full 2π-equivalent drift takes roughly 6,300s and 4,050s respectively (or ~2,460s for their differential, the quantity that actually matters for the beat/null position) - **none of these are anywhere close to 153.6s** (all 15-40x too slow). So this specific, simple mechanism, however elegant conceptually, doesn't quantitatively explain the observed period on its own - stated honestly rather than force-fit.

**Where this points instead**: with the beat period landing on an EXACT integer number of samples (16000Hz / 1000Hz beat = 16 samples/cycle, `relative_delay` currently sitting at an exact `+2.00` samples too), a hypothetically unfiltered, undelayed version of this signal would show ZERO cycle-to-cycle jitter in the null's sample-grid position - it would land in exactly the same relative spot every single cycle, forever. The fact that the user directly observes real jitter is therefore itself evidence that something ELSE is introducing it - and the most likely software candidate is exactly the envelope-path filters the user confirms are active: `'g'`/`'a'`/`'A'` (`envelope_gdeq`/`envelope_ampeq`), whose whole job is reshaping envelope's own time/phase response. This ties directly back to the last two entries: these are the same filters already flagged for (1) a cross-core race in their enable/reinit sequence, and (2) having no mechanism ensuring their envelope-only reshaping stays consistent with the phase path near a null. A filter with any slow internal drift (coefficient quantization creep, thermal dependence, or a residual effect of the flagged race) would directly produce exactly what's being described: a null depth/position that wobbles cycle to cycle and drifts slowly over longer timescales. Real hardware timer/ADC jitter (thermal drift of the sample-clock source, which would also naturally be "slow") is a second plausible contributor that can't be assessed from source code alone - it would need an actual hardware timing measurement, not a review of this repository.

**Found and fixed a real, embarrassing blind spot while investigating this**: `env_min` in both the `'K'` trace bins and the jump log has been printed with `%.3f` this entire session - and since this signal's actual null depths sit well below 0.001, **every single env_min value in every log shared today has printed as an uninformative flat `0.000`/`0.001`**, regardless of how deep the null actually was. This isn't "the null is always exactly that value" - it's the print statement discarding exactly the information needed to test the user's own hypothesis. **Fixed**: both print sites (`diagnostics.cpp`, the K trace's pre/post bin loops and `diagnostics_print_jump_log()`) now print `env_min`/`envelope`/`envelope_min` in `%.6e` (scientific notation) instead of `%.3f`, so real variation - whatever its true scale - will actually be visible going forward. Not yet bench-tested (no compiler in this sandbox). **This is the single most useful next step**: once flashed, the next batch of K/jump-log captures will show real env_min values for the first time, letting an actual, data-backed check of "does null depth trend with something like a 153.6s period, and is it visibly lower right before/at trigger bins" replace what's so far only been a scope-side visual impression.

## 2026-09-15, later still: first real env_min data (K_out_4.txt, 54 events) - the fix works, and it resolves the null-depth question completely; also rules out gdeq/ampeq/eq/compressor for THIS capture and surfaces a new, previously-undiscussed active filter (envelope pre-distortion)

User uploaded a fresh 8943-line capture (`K_out_4.txt`, ~72 minutes, 54 K auto-captures) and asked "anything new?" - the first capture taken since yesterday's `%.6e` env_min print-precision fix, so the first real test of whether the fix actually reveals anything and, separately, the first real data on the user's own scope-based null-depth/K-trigger hypothesis.

**The fix works and the hypothesis is now confirmed with hard numbers, not just visual impression.** Parsed all 54 `AUTO-CAPTURED` events and paired each with its trigger-bin `env_min`. They split cleanly into two tight, reproducible clusters plus a small third group:

- **25 "deep" events**: `env_min` = 0.00145-0.00170 (mean 0.00158), `delta` always strongly positive (+14.9 to +23.6Hz, mean +18.5Hz), `after` (the 50ms post-trigger mean freq) lands in a narrow 1002.6-1093.8Hz band (mean 1040.8Hz) every single time.
- **26 "shallow" events**: `env_min` = 0.0886-0.0909 (mean 0.0898) - roughly 57x less deep than the "deep" cluster - `delta` always negative (-5.4 to -13.7Hz, mean -8.8Hz), `after` lands tightly around 695.3-704.0Hz (mean 699.9Hz), i.e. back near the 700Hz nominal tone.
- **3 "solo" events** (t=154702, 1537502, 3842258): `env_min` = 0.0021-0.0035 (between the two clusters, closer to "deep"), `delta` always exactly -5.00Hz (K's minimum trigger threshold) - these sit in the same time-slot a "deep" event normally occupies (see periodicity below) but the null that cycle came up shallower than usual and only just cleared the threshold, in the opposite direction from the typical big positive jump.

This is about as clean a confirmation as this kind of bench data gets: the trigger-bin `env_min` value is reproducible to 3 significant figures across dozens of independent events spread over 72 minutes, and it's a near-perfect predictor of both the sign and magnitude of the resulting K event. **Deeper null -> bigger, positive-going excursion (toward the ~1000-1080Hz band); shallower "null" -> smaller, negative-going excursion (back toward ~700Hz).** Exactly the mechanism the user described from watching the scope, now numerically nailed down rather than just visually suspected.

**Periodicity re-measured, far more precisely than before, and shown to be a two-phase structure, not a single event type.** Gap between consecutive "deep" events: 24 measured gaps, tightly clustered 153,633-153,684ms (mean 153,655ms, stdev ~16ms) - confirms the ~153.6s cadence found earlier this session, now to within 0.03% precision. Each "deep" event is reliably followed 8136-8154ms later (mean 8147ms, n=25) by its paired "shallow" event - i.e. every ~153.6s cycle has TWO K-triggering moments 8.15s apart, not one. The 3 "solo" events land in exactly the time-slot a "deep" event should have (gaps either side sum to ~307,270ms = 2x the normal period), consistent with an underlying process whose null-depth itself wobbles cycle-to-cycle enough to occasionally miss the deep-cluster threshold - matching the user's own observation that null depth varies "quite slowly with time." This level of clockwork regularity (single-digit-ms precision holding over more than an hour) reads as a deterministic numeric/timing beat, not aperiodic memory corruption - worth flagging as a point against (not for) the cross-core-race-corruption framing for THIS specific pattern, discussed further below.

**Checked which filters were actually active during this capture - and it directly contradicts "all on."** The log's own command trace (captured at the top of the file, before the diagnostic run starts) shows: preset 2 loaded, then preset 3 (`Shelf2 Baseline gdeq adj#4`), then explicit toggles - `g` (gdeq) OFF, `a` (ampeq shelf 1) OFF, `A` (ampeq shelf 2) OFF, `D` (predistort) OFF then ON again, `a` ON, `A` ON, `g` ON, then **`a` OFF, `A` OFF, `g` OFF** - ending, right before the first AUTO-CAPTURED event, with gdeq OFF, ampeq shelf1 OFF, ampeq shelf2 OFF. Cross-checked against `settings.h`'s preset 3 struct itself: `eq_enable=false`, `compressor_enable=false` too. So for this specific capture: envelope_gdeq, envelope_ampeq (both stages), the pre-Hilbert EQ, and the compressor were **all confirmed OFF** - directly ruling out, for this data, every filter-state/cross-core-race hypothesis built up over the last several entries (which all depended on gdeq/ampeq being actively engaged). Either the user's "all on BTW" statement described a different, unlogged scope-watching session, or the toggles above were mid-session experimentation that ended in a different state than intended - worth a direct check next time, but the data itself is unambiguous about what was actually active during THIS run.

**What WAS active: envelope pre-distortion (`'D'`), not discussed by name until now.** `envelope_predistort.cpp` is a 65-point piecewise-linear lookup table (`ENV_PREDISTORT_LUT`, REVISION 6) mapping desired envelope [0,1] to commanded PWM duty [0,1], measured end-to-end on real hardware to correct the PWM/BS170/AD9851-RSET chain's static nonlinearity. It's explicitly documented as **stateless** ("no ring buffers/filter memory... nothing to glitch" - its own header comment) - a pure function of its instantaneous input, recomputed fresh every call. This structurally RULES IT OUT as a source of a slowly-drifting, multi-second periodic pattern: it has no memory to drift. But it matters enormously for INTERPRETING the env_min numbers above, for two reasons:

1. **`env_min` is measured POST-predistort, not on the raw physical envelope.** Traced the exact call chain: `ssb_mic_test.ino` applies `envelope_floor_apply()` -> `envelope_gdeq_process()` -> `envelope_ampeq_process()` -> (since predistort is enabled) `envelope_predistort_process()`, clamps to [0,1], and only THEN does `envelope_at_freq_time = envelope` get set (line ~658) - which is the exact value `diagnostics_set_tx_info()` feeds into `s_slow_bin_min_env`/`env_min` (`diagnostics.cpp` line 765, comment confirms "min of envelope_at_freq_time over the bin"). So every `env_min` value in this whole capture is a **post-LUT duty value**, not a [0,1] envelope fraction.
2. **The LUT's first bin is the steepest, most nonlinear part of the whole 65-point table**: `LUT[0]=0.0010`, `LUT[1]=0.1766` - a jump of 0.1756 in duty across just 1/64 (1.5625%) of the raw envelope range, an ~11.2x local gain. Both the "deep" and "shallow" clusters' `env_min` values fall inside this single bin. Inverting the LUT numerically for each cluster's mean: "deep" (duty~0.00158) back-computes to a **raw envelope of ~0.0052%** - i.e. genuinely, essentially zero; "shallow" (duty~0.0898) back-computes to a **raw envelope of ~0.79%** - a real but far shallower dip. The ratio between the two in RAW terms is ~150x, not the ~57x the raw duty numbers alone suggested - the LUT's steep first bin is compressing, not exaggerating, the true underlying spread here (since it's plotted the same direction). The "solo" events (duty~0.0021-0.0035) back-compute to raw envelope ~0.010-0.022% - consistent with them being deep-null-slot events that came up a bit shallower than the usual ~0.005%, exactly as the periodicity analysis above suggested independently.

**Bottom line - this significantly narrows the search, in a good way.** With gdeq, ampeq (both stages), pre-Hilbert EQ, compressor, and envelope_floor all confirmed OFF (envelope_floor defaults to 0.0/no-op and wasn't toggled either), and predistort confirmed stateless, **none of the envelope-domain filter/race hypotheses built up over the last several entries can be the source of THIS capture's ~153.6s/8.15s pattern** - they simply weren't running. What's left in the signal chain for AUDIO_SRC_TWOTONE is: the two-tone generator itself (`test_signals.cpp`'s float32 oscillators, already checked once for pure 2π phase-wrap drift and found ~15-40x too slow at that specific mechanism - but that doesn't rule out some OTHER numeric quantity in the same file, e.g. relative amplitude balance between the two tones, or the `TWOTONE_DITHER_*` mechanism, confirmed off by default and not toggled in this log's command trace either) and `ssb_dsp_process_sample()`'s own Hilbert-transform/DC-blocking math (which the synthetic two-tone sample flows through identically to real mic audio, per `ssb_mic_test.ino`'s single shared call site). `relative_delay` is fixed at exactly `+2.00` samples throughout (baked into preset 3), consistent with every AUTO-CAPTURED line's `delay=+2.00`, and stays a candidate only in the sense that its own null-adjacent sample-pair selection interacts with whatever timing drift is happening upstream - not as a source of new drift itself, per the earlier read of its code as explicitly race-free and self-contained.

**Next step**: given the shrunk candidate list, the most useful next data would be either (a) a capture with predistort OFF (`'D'`) so `env_min` reads raw envelope directly rather than through the LUT's steep first bin - useful for its own sake as a sanity check that the ~150x real ratio above is right - or (b) direct inspection of `test_signals.cpp`'s two-tone amplitude-balance state and `ssb_dsp.c`'s DC-blocking/Hilbert internals for anything with a natural ~150s-scale time constant (a slow leaky integrator, a coefficient computed once per some counter that wraps on a non-power-of-two boundary, etc.) - not yet done, next logical step in the code audit.

## 2026-09-15, later still: user confirms the filters-off capture was deliberate; traced exactly how phase resolves to freq_dev_hz and confirmed near-null phase noise is currently unmitigated by design

User confirmed the gdeq/ampeq/EQ/compressor-off state in `K_out_4.txt` was a deliberate A/B check ("I switched off the filters to see if it made any difference"), not an accident - so the previous entry's ruling-out of those filters for that capture stands as intended, not a caveat. They then asked two precise questions: exactly how the phase discontinuity is resolved, and how phase converts to frequency - specifically whether it's a phase-difference calculation, and if so how the near-null uncertainty in that calculation is handled.

Traced `ssb_dsp_process_sample()` (`ssb_dsp.c` lines 800-906) exactly. `phase = atan2f(Q, I)` from the Hilbert-derived analytic signal (bounded to (-pi,pi] by definition). The discontinuity that actually needs resolving is in `dphi = wrap_pi(phase - prev_phase)` - a raw subtraction of two atan2 outputs can jump by nearly 2pi for a signal that only actually advanced a small amount; `wrap_pi()` (repeated +/-2pi until back in (-pi,pi]) picks the smallest-magnitude equivalent, standard phase-unwrapping, valid as long as the true per-sample advance never exceeds pi (true here, given content stays under Nyquist/2). Frequency conversion is then a plain, direct phase-difference calculation: `freq_dev = dphi * sample_rate_hz / (2*pi)` - the discrete form of instantaneous frequency = (1/2pi)dphi/dt.

**The uncertainty question is the important part, and the honest answer is that it's currently unmitigated by the DSP math itself.** Near a null, I and Q are both tiny, so `atan2(Q,I)`'s angle becomes hypersensitive to any real noise (ADC quantization, DC-blocker residual, FIR rounding) riding on top of the signal - a real, physical noise-amplification effect that `wrap_pi()` does nothing to address (it only resolves which way the phase wound, not whether the underlying I/Q estimate is trustworthy). Two mechanisms exist downstream that COULD limit the fallout, and both are confirmed off in every capture examined this session:

1. `freq_dev_slew_limit_hz` - a genuine sample-to-sample rate limiter on freq_dev itself, and per its own doc comment (`ssb_dsp.h`) this is the mechanism actually designed for exactly this problem: real two-tone content away from a null never needs more than ~60Hz/sample, while a genuine null needs ~8000Hz/sample in a single step - a 100x+ gap with nothing in between. Defaults to `SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ` (1.0e6 Hz/sample, effectively off), and every preset checked so far - including preset 3, used for K_out_4.txt - leaves it there.
2. `max_freq_dev_hz` (currently 20000.0f, config.h) - an absolute magnitude clamp, applied after the slew limiter. **Confirmed structurally unable to bind on any single-sample event**: since `wrap_pi()` guarantees |dphi|<=pi, the conversion formula caps any single-sample |freq_dev| at `sample_rate_hz/2` = 8000Hz at 16kHz - already comfortably under the 20000Hz clamp. This cross-checks exactly against ssb_dsp.h's own documented ~8000Hz/sample null-swing number, confirming the clamp is dead weight for precisely the near-null spikes it might look like it's there to catch; it could only ever matter for something accumulated across multiple samples, which this code doesn't do.

This isn't an oversight - it's the same conclusion this session already reached from `envelope_floor.cpp`'s NOTE 1 postmortem: an earlier real attempt to freeze freq_dev near a null was found actively wrong on real hardware (it lets true phase silently drift while frozen, then snaps back as a hard discontinuity) and was removed. The large phase swing at a genuine null is treated as correct, required signal content, not noise to suppress - the project's actual strategy is to let it happen and rely on downstream envelope-weighting to keep it inaudible when envelope genuinely reaches near-zero at the same instant. The `null_bias_threshold`-gated accumulators in this same function (`near_null_dphi_sum`, `env2_dphi_sum`/`env2_sum`) are confirmed pure passive diagnostics - they measure this effect but never feed back into freq_dev itself.

**Ties directly to the last entry's findings**: with the slew limiter off (as always, so far) and the magnitude clamp structurally incapable of helping, the only thing standing between a near-null phase glitch and the transmitted signal, in every capture examined this session, is envelope genuinely dipping low enough at the same instant - exactly the mechanism the null-depth/K-trigger correlation work has been chasing. Nothing new needed testing here (pure code trace, no firmware change) - but it does mean deliberately engaging the slew limiter (raise via whatever key maps to `ssb_dsp_raise_freq_dev_slew_limit()`) is now a concrete, available experiment: if it visibly reduces K/jump-log magnitudes without hurting real two-tone content (per its own ~60Hz/sample vs ~8000Hz/sample design margin), that would be strong independent confirmation that near-null phase noise (not envelope-filter state) is the dominant contributor in the current all-filters-off configuration.

## 2026-09-15, later still: is there a resolution bias on I/Q near zero? Yes - quantified via simulation, and it's float32 cancellation, not ADC quantization, and it's the same order of magnitude as the "deep" null cluster from K_out_4.txt

User asked directly whether I/Q suffer a resolution bias as they approach zero. Two candidate mechanisms, and only one applies to the captures analyzed so far:

**Not classic ADC quantization** - `AUDIO_SRC_TWOTONE` (every capture examined this session) bypasses the ADC entirely: `sample = generate_twotone_sample()` (`ssb_mic_test.ino:541`) is pure float32 math (`A*sinf(theta1) + A*sinf(theta2)`, `TWOTONE_AMPLITUDE=0.45f`), never touching the real 12-bit ADC. A real mic signal WOULD have classic fixed-LSB resolution bias (constant absolute step regardless of signal size, so relative error grows as amplitude shrinks) - just not relevant to any log seen so far.

**What actually applies: floating-point catastrophic cancellation, quantified by direct simulation.** Both I and Q are computed as sums of individually much-larger terms that nearly cancel right at a null:

1. **I** (`ssb_dsp.c`'s direct tap) = `A*sin(theta1) + A*sin(theta2)`. Near destructive interference (theta1-theta2 ~ pi), the two sinf() terms are each O(0.45) but opposite in sign. Simulated directly (float32 vs. float64 ground truth, 200k trials constrained to within 0.02 rad of the exact null condition): resulting float32 rounding floor is tiny, ~1e-8 to 7e-8 absolute (median ~8e-9, worst observed ~6.9e-8) - negligible against anything measured this session.
2. **Q** (the 65-tap Hilbert FIR sum) is where it actually matters. Reconstructed the exact filter (`generate_hilbert_coeffs()`'s Hamming-windowed Hilbert taps) and simulated the full 65-term float32 accumulation against a float64 ground-truth dot product, 2 million trials constrained to within 0.003 rad of the exact null condition (1914 qualifying near-null samples): median |true Q| near null ~3.3e-4, median float32 rounding-noise floor ~2.8e-5 (about 8% of the true value at the median), 95th percentile ~9.6e-5, worst observed ~2.5e-4. **In ~4.8% of near-null instants, the float32 rounding error on Q actually EXCEEDS the true mathematical value.** The much larger floor vs. I's is expected - 65 sequentially-accumulated float32 additions compound their individual ~1e-7-relative rounding errors far more than a simple 2-term sum.

**Why this matters for the investigation so far**: the "deep" event cluster from the last K_out_4.txt entry back-computed (via the predistort LUT inversion) to a raw envelope of ~5e-5 - right in the same order of magnitude as Q's simulated rounding-noise floor (~1e-5 to a few x1e-4). This means at the very deepest nulls actually observed, `env_min` may not be measuring a clean physical signal value at all - it could be measuring float32 arithmetic rounding noise from the 65-term Hilbert cancellation, not "true" null depth. This is also a plausible (partial or full) explanation for why the "deep" cluster's env_min wasn't perfectly bit-identical cycle to cycle (varied ~15%, 0.00145-0.00170 in that data) - at this scale, arithmetic rounding noise is a real candidate contributor to that spread, independent of (or in addition to) any genuine physical drift mechanism.

**Scope/caveat, stated honestly**: this is a real, now-quantified floor below which the current float32 implementation cannot resolve a "true" null depth, full stop - not a bug to fix casually (moving Q's accumulation to double precision, or restructuring the FIR sum to avoid the cancellation, are the only ways around it, and neither has been evaluated for cost on this hardware). It does NOT by itself explain the "shallow" cluster (env_min~0.09 duty, back-computed raw envelope ~0.79% = 7.9e-3) - that's 2 orders of magnitude above this floor, comfortably a real physical value, not rounding noise. So this finding sharpens (doesn't replace) the open question from two entries ago: the ~153.6s/8.15s periodicity and the "why does null depth vary" question both still need an actual mechanism - this entry adds the caveat that any explanation for the DEEPEST points in that cycle needs to account for this floor being nearly reached, not just approached.

## 2026-09-15, later still: is the Hilbert transform IIR (can errors lock in)? No - it's FIR, no feedback, errors are self-limiting

Direct follow-up to the resolution-bias entry: is the Hilbert transform an IIR filter, where the cancellation-driven rounding noise just quantified could get "locked in"? Checked the actual structure. `generate_hilbert_coeffs()` produces 65 fixed FIR taps, and `ssb_dsp_process_sample()`'s Q computation is a plain weighted sum over those taps against a circular buffer of raw INPUT samples (`Q += hilbert_coeffs[n] * delay_line[...]`) - no feedback term anywhere (no `y[n-1]`-style recursion). This is a pure FIR filter: every Q value is recomputed fresh from that sample's 65-sample input window, and once a sample ages out of the window it can never influence Q again.

**Consequence for the previous entry's finding**: the ~2.8e-5-median cancellation-driven rounding floor on Q does NOT persist or compound - it's transient by construction, present only while the specific near-null samples sit inside the 65-tap window, then completely gone once they slide out. No drift, no accumulation, no bias carried forward. This is the sharp opposite of the already-flagged `envelope_gdeq`/`envelope_ampeq` IIR filters (real feedback, `y[n]=...+a*y[n-1]+...`), which is exactly why THOSE were flagged as able to "stay corrupted" if cross-core-raced rather than self-heal - the Hilbert FIR has no such memory to corrupt in the first place.

**One nuance surfaced and stated precisely**: `prev_phase` IS one sample of carried-forward state (used for `dphi = wrap_pi(phase - prev_phase)`), so a single corrupted `phase[k]` does leak into two consecutive `freq_dev` outputs - an oversized `dphi[k]` and an equal-and-opposite undersized `dphi[k+1]`, since the bad value is used as both the current sample and the next sample's `prev_phase`. This is the ordinary behavior of a first-difference/derivative estimator (a single impulse becomes a two-sample doublet, then vanishes) - not feedback in the filter-theory sense (no pole, no possibility of persisting a third sample), and structurally incapable of the multi-sample/effectively-permanent persistence the gdeq/ampeq IIR state can exhibit if raced.

**Bottom line**: the resolution-bias/cancellation floor quantified in the previous entry is real but self-limiting - a flicker at worst two samples wide, not a sustained corruption. Keeps that finding cleanly separated from the actual persistent-memory concern (gdeq/ampeq cross-core race) already documented earlier this session - the two are different species of problem with different implications, and this entry pins down which one the Hilbert/freq_dev path actually is.

## 2026-09-16: decisive result - a controlled, on-demand relative_delay-induced 20Hz jump is PROVABLY invisible to H/K, and the mechanism is now fully code-verified

Two new observations from the user, one confirmatory and one conclusive.

**Tone-symmetry check (confirmatory)**: watching both tones independently on the SDR, the user expected a "jump" might show as the upper tone rising while the lower falls (or vice versa) - a symmetric spread. They've never seen that; the two tones' separation has stayed a rock-constant 1kHz every time. This is fully consistent with (not against) this session's theory: a genuine sustained bias in average instantaneous frequency is mathematically a pure spectral translation (multiplying the complex baseband signal by a constant-rate phase rotation shifts EVERY frequency component, including both tones, by the identical amount - a direct consequence of the Fourier shift theorem). "Upper up/lower down" would be the signature of a structurally different fault (frequency-axis dilation, or a modulation-depth/scaling error), not what any mechanism discussed this session predicts. Good corroborating evidence, not a contradiction.

**The `'['`/`']'` (relative_delay) experiment - conclusive, and now fully mechanistically proven.** The user can reproducibly induce a real, sustained ~20Hz shift on the SDR by retuning relative_delay, hold it there indefinitely, and `'H'`'s on-demand readout ALWAYS comes back reporting ~700Hz nominal +/- as usual - never showing the induced shift, no matter how long it's held. Unlike every previous "H didn't match the SDR" report this session (all involving spontaneous, hard-to-pin-down events with timing ambiguity), this is fully controlled, on-demand, and repeatable - a clean experiment, not an anecdote.

Traced the complete mechanism end to end, and it's airtight:

1. `diagnostics_set_tx_info()`'s EMA update block (feeds both `fast`/`slow`, i.e. everything `'H'` and `'K'` report) uses `dev = delayed_freq_dev_hz` - the exact value sent to the AD9851, not some earlier pre-delay value. This looked, going in, like it SHOULD be sensitive to a real relative_delay-induced change.
2. But `relative_delay_apply()` produces that exact value via `interp_ring()`: `ring[idx0]*(1-frac) + ring[idx1]*frac` - a plain LINEAR INTERPOLATION between two adjacent entries of the SAME raw `freq_dev_hz` ring buffer (`relative_delay.cpp`). Retuning `'['`/`']'` only changes `freq_back` - WHICH two nearby raw samples get blended and in what proportion. It never touches the raw `freq_dev_hz` values themselves.
3. Since the raw freq_dev_hz signal is periodic/stationary over the two-tone beat structure, blending two of ITS OWN nearby samples - at any lag, integer or fractional - cannot change its own long-run average. Shifting which few adjacent samples of a repeating sequence you read just reorders which values you see; it does not change their mean. **This proves `fast`/`slow` (and therefore both `'H'` and `'K'`, which share this exact EMA pair) are structurally, permanently incapable of reflecting a relative_delay-induced shift, regardless of magnitude or duration.**
4. Meanwhile envelope is read at a DIFFERENT lag (`env_back`, zero when delay is positive - see `relative_delay_apply()`'s `freq_back`/`env_back` split) than freq_dev is (`freq_back`, the delay amount). So what `'['`/`']'` actually, physically does is deliberately mis-time which envelope sample gets multiplied against which freq_dev sample in the REAL transmitted signal - a genuine, SDR-confirmed energy-weighting effect (exactly the mechanism from the 2026-09-15 "theory completes" entry), achieved without ever touching freq_dev_hz's own plain statistics at all.

**This is not just consistent with the energy-weighting theory from several entries ago - it's a full, code-verified, mathematical proof of it, from a controlled and repeatable experiment.** The plain (envelope-blind) average that `'H'`/`'K'` compute and the energy-weighted average an SDR actually displays are now DEMONSTRATED to be different quantities that can diverge arbitrarily far, for as long as desired, with zero correlation - not just "sometimes miss each other due to timing." 

**Implication, stated plainly**: this isn't a quirk specific to the `'['`/`']'` knob. ANY real frequency shift whose root cause is a timing/pairing mismatch between the envelope and freq_dev paths - which includes every candidate mechanism floated this session for the SPONTANEOUS jumps (cross-core races in envelope_gdeq/envelope_ampeq state, filter-induced null-position jitter, the float32/cancellation floor at the deepest nulls) - would be EXACTLY as invisible to `'H'`/`'K'` as this deliberately-induced one is. This resolves, with hard proof rather than argument, the earlier "we are not looking at the same reality" disagreement from several entries back: the user's instinct that internal diagnostics were missing real events was correct, and now there's a complete, mechanistic, code-verified reason why - not a measurement-timing coincidence, a structural property of what `'H'`/`'K'` are mathematically built to measure.

**Not yet done, worth flagging as the natural next step**: this proves H/K CAN'T see this class of shift, but doesn't yet give a way to MEASURE the true energy-weighted average from firmware for comparison against the SDR. A genuinely fixed diagnostic would need to accumulate envelope^2-weighted freq_dev (exactly the `env2_dphi_sum`/`env2_sum` accumulators already sitting in `ssb_dsp.c`, currently populated but never read back through a serial command) rather than the current plain EMA - concrete, buildable next step, not yet implemented.

## 2026-09-16: user agrees on the diagnostic fix but flags the real caution - the `[]` test and hands-off jumps could easily be different mechanisms. Found a strong, DISTINCT, already-half-investigated candidate for the hands-off case, and implemented the post-delay energy-weighted diagnostic

User agreed the freq calc/logging need to change to match the SDR, but pushed back correctly on over-generalizing yesterday's `relative_delay` proof: it shows H/K CAN'T see an envelope/freq_dev pairing shift, but doesn't prove that's what's causing the SPONTANEOUS hands-off jumps - "it could easily be something different between the `[]` test and the hands off cases." Right instinct: `relative_delay` itself is a fixed setting during hands-off operation, not something drifting on its own, so whatever causes a spontaneous jump needs its OWN mechanism, not just "the same thing as the `[]` test but unprompted."

**Found a strong, distinct, already-partially-investigated candidate sitting in `AD9851.c`'s own history: bit-bang SPI signal-integrity margin during large single-tick FTW jumps.** Two things found together:

1. A standing code comment (`AD9851.c`, near `ad9851_edge_delay()`): "A large one-tick FTW jump - exactly what two-tone's near-null atan2 noise produces (confirmed real, up to 8562Hz single-tick swings)... can flip many DATA bits at once, including many fresh 0-to-1 transitions, which is precisely the demanding case for this margin... **this is the leading theory for the two-tone-specific 'sticks' symptom this thread has been chasing.**" The board's bit-bang transport (`AD9851_USE_BITBANG=1`) uses an inverting BS170 level shifter whose LOW-to-HIGH transition is a slow, PASSIVE, pull-up-charged RC edge (much slower than the actively-driven HIGH-to-LOW edge) - exactly the asymmetry that makes a "fresh 0-to-1 bit" the demanding case the comment describes.
2. `AD9851_BITBANG_EDGE_DELAY_ENABLED` is currently **0** - the edge-settle margin that would protect against this is DISABLED right now, reverted on 2026-09-11 specifically because the earlier "4MHz is safe" scope measurement was found to possibly have characterized the wrong (unthrottled) signal, not because the margin was shown unnecessary. **The actual comparison test needed to settle this - restore the margin, then run a genuine hands-off comparison to see if spontaneous jumps stop or reduce - was explicitly planned in a 2026-09-11 comment and never completed**: "restoring this delay is exactly the comparison needed... Superseded by the higher-priority need to re-verify the real drive-signal toggle rate... Re-run the hands-off comparison this comment describes once that's settled." That re-scope never happened before the flag was disabled again. The SAME comment block also records a concrete prior hands-off event this theory would explain: a "-45Hz jump (max_freq_dev_step 10387Hz at t=308407ms, digital chain otherwise clean - freq_dev/tx_freq nominal throughout, canary OK) with delay untouched, i.e. with no sweep provoking it" - the exact "SDR-visible jump, clean internal diagnostics" signature this whole investigation keeps running into, recorded a session ago and never resolved.

**Why this is a genuinely different mechanism from yesterday's `relative_delay` proof, not just a restatement of it**: yesterday's finding was about SOFTWARE re-pairing of two already-correct signals (envelope, freq_dev) at the wrong relative time - the intended FTW value is always right, only its pairing with envelope is wrong. THIS mechanism is about the intended FTW value itself failing to land correctly on the physical wire - a genuine bit-level transmission error at the AD9851, entirely downstream of every software diagnostic (freq_dev_hz, `delayed_freq_dev_hz`, tx_freq as printed, even a corrected energy-weighted average) because none of them ever read back what the chip actually latched. If a garbled FTW gets latched by the following FQ_UD pulse, the AD9851's real output would sit on that wrong frequency until the NEXT tick's transfer succeeds - a real, physical "stuck" state no software fix can detect without either restoring the timing margin or independently verifying the transmitted signal.

**Concrete, not-yet-run test (requires the user's bench, not just firmware)**: re-enable `AD9851_BITBANG_EDGE_DELAY_ENABLED` (all three call sites already exist, just currently `#if`'d out) and run the exact comparison the abandoned 2026-09-11 plan called for - extended hands-off operation with the margin restored, watching whether spontaneous jumps/sticks reduce or stop. If they do, this is the root cause and the margin needs to stay on permanently (at whatever cost to the 62.5us tick budget - `max_busy_us` was measured at 58 with it on vs 46 trimmed, both well inside budget). If hands-off jumps persist unchanged with the margin restored, this mechanism is ruled out and the search should look elsewhere for the hands-off case specifically. Not implemented this entry - a real hardware timing tradeoff decision, not a decode/analysis change, and needs the user's own bench measurement to interpret (this environment has no compiler or scope).

**Separately, implemented the diagnostic fix the user agreed to** - completing a proposal that's been on record since 2026-09-12 and never built. The existing `null_bias`/`weighted_bias` diagnostic (`print_null_bias_block()`, fed by `ssb_dsp.c`'s `env2_dphi_sum`/`env2_sum`) was already found on 2026-09-12 to accumulate PRE-delay - inside `ssb_dsp_process_sample()`, before `relative_delay_apply()` re-pairs freq_dev against envelope at whatever lag `relative_delay` is actually set to. That's exactly why "weighted_bias doesn't track the real jump at all" (2026-09-09 finding) despite otherwise matching real SDR readings to within a few Hz in steady-state bench comparisons - and a "post-delay null_bias variant" was proposed that same day but never built (see `diagnostics.h`'s `delayed_envelope` comment: "kept here for a still-unimplemented future use").

Built it now, in `diagnostics.cpp`: a new envelope^2-weighted EMA pair (`s_freq_ema_energy_num_fast`/`s_freq_ema_energy_den_fast`, same fast tau/alpha as the existing plain EMA, updated at the exact same per-tick site as `s_freq_ema_fast_hz`) fed from `delayed_freq_dev_hz` and, critically, `envelope_at_freq_time` - NOT `delayed_envelope`, which `relative_delay.h`'s own comment already warns is not time-matched to `delayed_freq_dev_hz` once delay is nonzero (this exact trap was already caught and fixed for the jump log back on 2026-09-12; reused that same lesson here rather than repeating the mistake). The ratio of the two EMAs is the standard envelope^2-weighted mean, added to `'H'`'s output as a new line: `energy_weighted fast=...Hz dev=...Hz (compare THIS against the SDR)`. Guarded against a near-zero denominator (falls back to the plain fast EMA if there's been essentially no transmitted energy to weight by).

**What this fixes and what it doesn't**: this makes `'H'` finally reflect the SAME kind of energy-weighted quantity an SDR reads, post-relative-delay, so a real `relative_delay`-class pairing shift (like yesterday's `[]` experiment) should now show up in this new field even though it can never show up in the existing plain `fast`/`slow` pair. It does NOT help with the AD9851 bit-bang hypothesis above - a genuine bit-level SPI transmission error corrupts the physical output in a way no software-side accumulator, however correctly weighted, can observe. The two fixes are complementary, aimed at two different, both-plausible mechanisms - exactly the split the user's caution pointed at.

Not bench-tested (no compiler in this sandbox, per standing caveat) - verified brace/paren balance only.

## 2026-09-16, later still: first bench test of the energy-weighted 'H' fix - a stale tone pair, two aliasing artifacts explained, and a real sustained hands-off jump caught with numbers

First bench report on yesterday's diagnostic addition: "SDR sitting at -22Hz and H reports 696." Asked which of `'H'`'s two lines "696" referred to (plain or energy-weighted) - the full pasted output turned up more than expected.

**Part 1: this repo's `config.h` had drifted to the retired 700/1900Hz pair.** `TWOTONE_F2_HZ` read `1900.0f`, not `1700.0f`. `git log -p --follow -- config.h` traced the line's full history: the original value was 1900, commit `de32ffd` ("Change tone pair to 700/1700 so syncs with Fs") set it to 1700, then commit `8fd8349` (2026-09-12, "Add dither on tone2 currently set to 0.05Hz, current 2tone set to 700/1900") silently reverted it back to 1900 - nothing since has touched it. That's exactly the legacy, non-Fs-commensurate pair the 2026-09-09 entry documented as producing extra 400Hz-spaced sampling-grid IMD products (1200Hz spacing against a 16kHz sample rate) and recommended retiring in favour of 700/1700 (1000Hz spacing, exactly 16 samples/cycle). Checked `test_signals.cpp`'s `'T'` band-preset table too: it has NO 700/1700 entry at all (presets are 300/500, 700/900, 1500/1700, 2500/2700, 3500/3700, all 200Hz-spaced, plus the legacy 700/1900 pair kept as the wrap-around default) - so 700/1700 has only ever been reachable via the compile-time default, before any `'T'` press. The user confirmed their actual bench firmware's `config.h` is 700/1700; fixed this repo's copy to match, with a comment explaining the history so it can't silently drift again. Everything below assumes the confirmed-correct 700/1700Hz pair (beat=1000Hz, exactly 16 samples/cycle at `SAMPLE_RATE_HZ`=16000 - Fs-commensurate).

**Part 2: two "always-on" diagnostics are aliasing artifacts against the exactly-periodic signal, not evidence of anything stuck.** Both captures the user pasted showed `held_trace`'s `tx_freq` column sitting in an extremely tight band (14201357-14201362Hz, i.e. `freq_dev` implied ~1197-1202Hz) for the ENTIRE visible 60-second window, in both captures, with `fast` sitting far below it (~696-700Hz) the whole time. Traced why: `HELD_TRACE_SAMPLE_TICKS = HELD_TRACE_SAMPLE_MS(500) * 1000 / SSB_SAMPLE_PERIOD_US(62.5) = 8000` ticks between snapshots. The 700/1700Hz pair's exact period is 16 ticks (beat=1000Hz, `16000/1000=16`). `8000/16 = 500` exactly - every single `held_trace` snapshot lands on the IDENTICAL phase of the exactly-repeating waveform. This is a classic stroboscopic effect, not a frozen DDS: `s_held_tx_freq[]`/`s_held_fast_hz[]` are written together, same tick, same call site (`diagnostics_set_tx_info()` line ~882-883, confirmed by reading the code directly, no index-misalignment bug), and `tx_freq = s_carrier_hz + (int32_t)delayed_freq_dev_hz` uses the exact same `delayed_freq_dev_hz` that feeds `fast`'s EMA update the same tick (confirmed via `carrier_output_set_freq_dev()` and its call site in `ssb_mic_test.ino`) - so the two ARE built from the same signal, they're just sampled completely differently: `fast` genuinely averages every tick between snapshots (a true low-pass), while the raw `tx_freq` column only ever samples the one fixed instant the strobe happens to land on. This doesn't make `held_trace` useless for catching a REAL sustained shift (a real shift moves every phase of the cycle together, so the strobed sample moves too) but its absolute level can't be read as "typical" or "average" - it's one fixed point in the cycle, nothing more.

The fine `slow_trace` bins (`SLOW_TRACE_BIN_TICKS=20`, "1.25ms/bin") show the same phenomenon at a different scale, and it explains a pattern that's been showing up in every `'K'` capture without being called out: a suspiciously clean, exactly period-4 alternation between exactly `400.0Hz` and `800.0Hz`, never anything in between. `LCM(16, 20) = 80`, so a 20-tick bin boundary only takes on `80/20 = 4` distinct phase alignments relative to the 16-tick beat cycle before repeating - exactly 4 discrete outcomes, confirmed by direct count in both `K_out_5.txt` capture's fine bins (mostly 800, one 400 every 4th bin). This diagnostic is essentially blind to the real fine time-structure of `freq_dev` for this specific (Fs-commensurate) tone pair and bin size - its clean, quantized-looking output is aliasing, not signal.

**Part 3: a real, sustained, hands-off jump - caught with before/after numbers, no delay change involved.** `K_out_5.txt` (freshly uploaded) contains two slow-jump auto-captures. The first (`t=623895ms`, `before=708.77Hz after=700.32Hz delta=-8.46Hz`) is just the tail end of an already-decaying divergence - its trend ring shows `slow` relaxing smoothly from 723.55Hz down to 708.84Hz over the visible 2-second window while `fast` sits flat at 700.32Hz throughout, a textbook single-pole EMA relaxation curve crossing back through the 5Hz trigger threshold on the way down, not a fresh event.

The second (`t=769436ms`, `before=700.44Hz after=1063.38Hz delta=+17.74Hz delay=+2.00`, "relative_delay was last changed 731154ms before this trigger") is different in kind: a genuinely new, large, SUSTAINED excursion, not a single-tick spike, with `relative_delay` untouched for the preceding 12+ minutes - ruling out yesterday's relative_delay-pairing mechanism as the explanation for this specific one (that mechanism requires an actual delay CHANGE to re-pair envelope/freq_dev; nothing changed here). Quantified it using the fast EMA's own known time constant: `FREQ_EMA_FAST_TAU_S=0.05f` (50ms) at `SAMPLE_RATE_HZ`=16000 means fast's tau is exactly 800 ticks - and `SLOW_TRACE_POST_BINS`(40) * `SLOW_TRACE_BIN_TICKS`(20) = 800 ticks, i.e. the post-capture window is EXACTLY one fast tau. For a single-pole EMA responding to a step to a constant value V starting from `fast0`, one tau elapsed means `fast(tau) = fast0 + (1-1/e)*(V-fast0)`, `1-1/e = 0.6319`. With `fast0 ≈ before+delta = 700.44+17.74 = 718.18Hz` and `fast(tau) = after = 1063.38Hz`: `V = fast0 + (after-fast0)/0.6319 ≈ 1264.5Hz`. That implies `freq_dev` sat near a ~1264Hz plateau for at least the full 50ms window, not a momentary blip. Cross-checked against the single-tick ceiling: `wrap_pi()`'s `|dphi|<=pi` bound caps any single-tick `freq_dev` swing at `sample_rate_hz/2 = 8000Hz`; even a maximal 8000Hz single-tick spike could only move `fast` by `alpha_fast*(8000-700) ≈ 9.1Hz` in one tick (`alpha_fast = DT/(tau+DT) ≈ 0.001248`) - nowhere near the observed ~345Hz rise. This rules out "one bad phase sample" and confirms the excursion was multi-tick/sustained, exactly the signature a garbled AD9851 FTW latch (persisting until the next successful transfer) would produce - though a DSP-level cause producing a genuinely elevated, non-single-tick `freq_dev` for tens of ms hasn't been ruled out either.

**Part 4: the user's own catch - two smaller `held_trace` "glitches" fit the same pattern.** Separately flagged: a single-sample `fast=722.55Hz` blip in an earlier capture (baseline ~696.59Hz just before, ~695.96Hz just after - a small permanent-looking step down) and `fast=759.61Hz` in `K_out_5.txt`'s own `held_trace` (baseline ~697.21-697.22Hz before, ~696.58-696.59Hz after), plus asked about the 723.55Hz value in the trend ring discussed in Part 3. The specific numeric closeness (722.55 vs 723.55) is very likely coincidental - different variables (`fast`'s single-tick snapshot vs. `slow`'s decaying trend value) at different times - but the underlying pattern holds up under the same arithmetic used in Part 3: both blips are single 500ms-apart `held_trace` samples showing a jump of 62.39Hz and 25.96Hz respectively, and the single-tick ceiling above (~9.1Hz max) rules out a one-tick cause for either. Both are therefore smaller/shorter versions of the same class of sustained excursion as the confirmed `K_out_5.txt` jump - just too small or short-lived to trip the 5Hz fast/slow divergence the auto-capture watches for, and only visible at all because `held_trace`'s 2-samples/sec rate happened to catch `fast` mid-excursion. `held_trace` cannot currently say how often these actually occur or how long each one lasts - only the auto-capture mechanism has caught one with enough resolution to characterize, and only because it happened to be large enough.

**Net picture**: real evidence of a recurring, variable-magnitude, sustained (tens-of-ms-scale, not single-tick) `freq_dev` excursion happening completely hands-off, independent of `relative_delay`, at an unknown rate - large enough at least once (`K_out_5.txt`'s second event) to trip the existing slow-jump detector, and otherwise visible only as occasional `held_trace` snapshots catching `fast` mid-excursion. Consistent with, though not yet proof of, the AD9851 bit-bang timing theory from yesterday's entry (a garbled FTW latch would persist for exactly however many ticks pass before the next transfer succeeds, naturally producing this "elevated for tens of ms then resolves" signature).

**Also fixed, same bench session**: the energy-weighted `'H'` line's `dev` was being computed against `s_freq_anchor_hz` - the PLAIN EMA's anchor - which the code's own comment already flagged as a known simplification. First live read (`energy_weighted fast=1109.55Hz dev=+410.46Hz`) confirmed this was actively misleading: a plain-mean and an energy-weighted mean of the same equal-amplitude two-tone signal are legitimately different numbers even with nothing wrong (see this file's own null_bias doc-comment identity), so comparing the energy-weighted absolute level against the plain anchor produces a large, static gap with no connection to any real event. Gave it its own anchor (`s_freq_energy_anchor_hz`), seeded at the same boot-settle instant as the plain anchor, guarded the same way `energy_weighted_fast_hz()` already is against a near-zero energy denominator. `'H'`'s energy-weighted line now prints its own anchor and a `dev` measured against it. Not bench-tested (no compiler in this sandbox) - verified brace/paren balance only (144/144, 1070/1070).

**Recommended next step, not yet built**: a dedicated per-tick trace of RAW `freq_dev_hz` (not the fast/slow EMA, not the aliased 20-tick bins) around a slow-jump trigger, at full resolution for at least the first ~100-200 ticks after it fires. Would show directly whether the excursion is a clean flat plateau (consistent with a stuck/garbled FTW at the AD9851) or a genuinely-modulating signal running at an unexpectedly high level (a different DSP-level cause) - the auto-capture's existing bins can't distinguish these because of the Part 2 aliasing issue.

## 2026-09-16, later still: first real-world validation of the energy-weighted 'H' fix - near-exact match on one reading, an open question on the other, and the ~153.6s cycle reconfirmed independently

`K_out_6.txt`: two manual `'H'` reads, with the user's own SDR readings for each (referenced to 700Hz, their stated convention) - first at -38Hz, second at +8Hz, ~23 seconds apart, with an untimed gap in between (coffee).

**First read (`t=612960ms`): energy_weighted anchor=1070.13Hz fast=1031.76Hz dev=-38.37Hz. Plain: anchor=699.09Hz fast=700.32Hz dev=+1.23Hz.** The energy-weighted `dev` (-38.37Hz) matches the user's independently-read SDR value (-38Hz) to within 0.4Hz, while the plain line shows essentially nothing (+1.23Hz) at the exact same instant. This is the first direct, real-world confirmation that yesterday's fix works as intended - not just internally consistent, but matching an independent, human-read SDR observation almost exactly, in a case with no relative_delay change involved (delay had been untouched for many minutes, per the surrounding auto-captures below).

**Second read (`t=635999ms`): energy_weighted dev=+63.65Hz. Plain dev=+0.62Hz.** SDR read: +8Hz. This one does NOT match well. Worth digging into rather than waving off, so pulled every `AUTO-CAPTURED slow_trace` event in the file to build the full timeline around both reads:

```
t=308411ms  rise      before=700.54  after=1085.33  delta=+16.06
t=316552ms  recovery  before=709.03  after=697.21   delta=-11.82   (+8141ms after the rise)
t=462082ms  rise      before=700.63  after=1043.59  delta=+21.12   (+153671ms after previous rise)
t=470225ms  recovery  before=708.62  after=697.21   delta=-11.41   (+8143ms after the rise)
t=612960ms  ** H1 ** energy_weighted dev=-38.37Hz (SDR: -38Hz)     (+2795ms BEFORE the next rise)
t=615755ms  rise      before=700.35  after=1007.35  delta=+15.64   (+153673ms after previous rise)
t=623902ms  recovery  before=708.87  after=696.59   delta=-12.28   (+8147ms after the rise)
t=635999ms  ** H2 ** energy_weighted dev=+63.65Hz (SDR: +8Hz)      (+12097ms after the recovery)
```

Two things fall out of this. First, the ~153.6s/~8.15s two-phase periodic cycle originally characterized from `K_out_4.txt` reproduces here independently, and with startling precision: both rise-to-rise intervals are 153671ms and 153673ms (2ms apart - tighter than the original 0.03% figure), and all three rise-to-recovery lags are 8141/8143/8147ms. Whatever drives this cycle is extremely regular and clearly still active on this board.

Second, and this is what explains the readings rather than excusing them: **H1 landed 2.8 seconds before a scheduled rise, and H2 landed 12.1 seconds after the following recovery had already completed.** H1's excellent match suggests the energy-weighted centroid is already drifting negative in the run-up to a rise, before the plain fast/slow pair's own trigger condition is anywhere close to firing - itself an interesting new data point (the pre-rise phase has its own signature, not just the rise/recovery pair already characterized). H2 is harder: plain `fast` was fully back to baseline (dev=+0.62Hz) 12 seconds after the recovery event closed, but the energy-weighted number was still elevated by 63Hz - and since the energy-weighted EMA uses the identical fast alpha as the plain one (same tau, same per-tick update site), a lingering, unresolved 63Hz gap 12 seconds (240 fast-tau's) after the plain signal settled can't be explained as "the same event, just slower to decay through an identical filter." That means whatever kept the energy-weighted centroid elevated at H2 was either a separate, still-ongoing pairing-type deviation not tied to the 615755/623902 rise-recovery pair, or an artifact of exactly when the user glanced at the SDR relative to the keystroke (unknown to within a few seconds here, given the coffee break) - can't distinguish these from one data point.

**Not treating the second mismatch as a fix failure** - the first match is too clean to be coincidental, and the second has a concrete, plausible confound (a real, independently-confirmed rise/recovery pair occurred in the gap between the two reads) rather than pointing at anything wrong with the anchor or the calculation. **Recommended follow-up, concrete and cheap**: next bench session, read `'H'` in the same breath as glancing at the SDR (seconds, not a coffee break) at several points spanning one full ~153.6s cycle - well before a rise, right at a rise, right at the recovery, and well into the following quiet stretch - to build a real point-by-point correlation table instead of two isolated, unevenly-timed samples.

## 2026-09-16, later still: user spots 8Hz-spaced sidebands (with an elevated 40Hz) around the 700Hz tone on the SDR - one exact firmware match found, and it may be self-inflicted

User reported, from direct SDR observation: sidebands spaced at 8Hz and its multiples around the 700Hz tone, with the 40Hz one (5th harmonic) noticeably stronger than the rest - the harmonic-rich pattern of a pulse-like, not sinusoidal, periodic perturbation at a fundamental of roughly 8Hz. Also flagged that recent shift readings (e.g. -38Hz, K_out_6.txt) sit close to a multiple of 8 (40Hz) given their own stated +/-2Hz measurement uncertainty.

Searched the full codebase for any process running near 8Hz (125ms) or 40Hz (25ms). No 8Hz source exists anywhere in firmware. But **40Hz matches exactly, and not coincidentally**: `EMA_TREND_SAMPLE_TICKS = 400` ticks at `SAMPLE_RATE_HZ`=16000 is `400*62.5us = 25ms = 40Hz` on the nose - this is the same trend-ring bookkeeping added in this file's 2026-09-16 entries (`s_trend_fast_hz`/`s_trend_slow_hz`, feeding `'K'`'s trend printout), and it executes unconditionally inside `diagnostics_set_tx_info()`, called every single tick from the hot DSP path (`ssb_mic_test.ino`).

Traced the exact ordering to see if this could physically touch the RF output: within one tick, `envelope_interp_on_full_tick()` (the PWM/RSET register write) fires first, then `carrier_output_set_freq_dev()` (the AD9851 SPI write) fires second, and `diagnostics_set_tx_info()` - where the trend-ring write lives - fires third, AFTER both hardware writes for THAT tick have already gone out. So it can't delay the current tick's own output. But `dsp_task` runs against a fixed 62.5us per-tick budget (`SSB_SAMPLE_PERIOD_US`, tracked via the existing `s_dbg_max_busy_us`/`s_dbg_overrun_count`/`s_dbg_late_tick_count` instrumentation), and the extra float writes/ring-index work happening once every 400 ticks adds real, if small, CPU time on exactly that tick - if it pushes that tick's total execution over budget, the NEXT tick's own PWM/AD9851 writes would start correspondingly late. That's a plausible, concrete mechanism for a genuine 40Hz-periodic timing perturbation on the transmitted signal, recurring at exactly the rate the user is seeing an elevated sideband at.

**If real, this would mean the diagnostic instrumentation built earlier today to investigate hands-off jumps is itself contributing a real artifact to the transmitted signal** - worth taking seriously precisely because of how it was found (an exact rate match plus a traceable mechanism, not a coincidence going in search of an explanation).

No firmware match for the 8Hz fundamental itself. One inactive-by-default candidate: `TWOTONE_DITHER_UPDATE_HZ`=4.0Hz (`config.h`) redraws tone2's dither target 4 times/second with a linear ramp between draws - a ramp's harmonic series would include a 2nd harmonic exactly at 8Hz - but `TWOTONE_DITHER_ENABLED` defaults to 0 and only activates via the `'Q'` command, so this only applies if dither has been toggled on. Worth confirming its state at the bench either way. Beyond firmware, an 8Hz fundamental with no code-side source is worth considering as external (supply ripple, mechanical/acoustic, or something in the analog reconstruction/PA chain) rather than continuing to search the DSP code for it.

**Scope check against everything else logged today**: this mechanism, even if confirmed, is a microsecond-scale timing jitter - plausible for the kind of ~20-60Hz energy-weighted wobbles seen in the `K_out_6.txt` `'H'` reads, but nowhere near large enough to explain the multi-hundred-Hz `fast`-visible auto-captured jumps (`K_out_5.txt`'s +1063Hz event, or `K_out_6.txt`'s +1085/+1043/+1007Hz events) - those still need the AD9851 bit-bang theory or another explanation. This is a candidate for the smaller, more continuous-looking deviations specifically, not a unification of everything in this investigation.

**Not yet done**: no firmware change made pending the user's input - candidates are (a) confirm/deny via bench test whether the 8Hz/40Hz sidebands change with `'Q'` dither on vs off, (b) as a firmware experiment, move the trend-ring write out of the unconditional hot-path branch (e.g. gate it so it only runs when `'K'` is actively watching, the way the fine `slow_trace` bins already are) and see if the 40Hz sideband specifically changes on the SDR - cheap, reversible, and a clean causal test if the user wants it built.

## 2026-09-16, later still: correction - the recurring ~153.6s "rise/recovery" pattern is very likely a plain-EMA null-noise artifact, not a real transmitted jump. K_out_5.txt's "genuine sustained jump" conclusion is walked back.

`K_out_7.txt` turned out to be `K_out_6.txt` (1390 lines) with 5498 more lines appended from the same continuous, un-cleared serial session (byte-identical prefix, confirmed with `diff`). The user watched the SDR through the whole of the NEW content (`t=769437ms` to `t=2160375ms`, ~23 minutes) and reported the 700Hz tone never moved more than +/-4Hz - in a stretch containing 12 more instances of the exact same rise/recovery auto-capture pattern already logged multiple times this session (`before`~700Hz, `after` jumping to ~822-1085Hz, then a recovery back to ~695-704Hz, on the same ~153.6s cadence, every single one with `env_min` pinned at exactly `1.000000e-03` across all its bins - a genuine near-total null on every occurrence).

This directly contradicts the conclusion reached for `K_out_5.txt`'s equivalent event a few entries back, which treated a `before=700.44Hz after=1063.38Hz` jump as a "genuine, sustained, hands-off jump" real enough to need a hardware-level (AD9851 bit-bang) explanation. That reasoning was mathematically sound (the fast EMA's own tau does rule out a single bad phase sample) but rested on an unstated assumption: that the plain, unweighted `fast` EMA's value corresponds to something physically meaningful about the transmitted signal. Given 12 SDR-silent repeats of numerically indistinguishable events (rise deltas -5.02 to +23.52Hz including the SDR-confirmed one at +15.64Hz; `after` values 822-1085Hz including the SDR-confirmed one at 1007.35Hz - no statistical separation at all), that assumption looks wrong for this specific recurring pattern. The `env_min`=1e-3 signature on every single occurrence is the tell: this is exactly the near-null condition where the analytic-signal phase can swing wildly (large `dphi`, large instantaneous `freq_dev`) while carrying almost no real transmitted power - precisely the mechanism the energy-weighting theory (2026-09-15/16 entries) says the plain time-average is vulnerable to and an SDR (reading real radiated power) is not.

**Isolating what's actually still unexplained**: of the 13 total occurrences of this pattern across `K_out_4.txt` through `K_out_7.txt`, exactly one (the rise/recovery pair at `t=615755/623902ms`) sits inside the window where the user separately, directly observed real SDR movement (-38Hz then +8Hz, `K_out_6.txt`'s two manual `'H'` reads). That event is numerically indistinguishable from the other 12 that produced no SDR-visible movement at all (same delta range, same `after` range, same `env_min` signature) - there is nothing in the auto-capture's own numbers that marks it as different. Two live possibilities, not yet resolved: (a) this pattern genuinely never causes a real shift, and the -38/+8 movement the user saw was a coincidentally-timed, separate, real cause overlapping with a null dip that happens to recur predictably; or (b) this pattern occasionally (rarely, and not predictably from its own numbers) does produce a real shift via some additional condition not captured by `before`/`after`/`delta` alone. Can't distinguish these from the data in hand.

**Practical effect on open theories**: this substantially de-prioritizes the recurring ~153.6s pattern as the target for the AD9851 bit-bang hardware theory - if 12 of 13 occurrences produce zero real transmitted effect, a hardware bit-latch explanation (which would affect the actual output every time it fires) fits poorly. The bit-bang theory, and the still-genuinely-unexplained hands-off events (the original -22Hz report, and whatever specifically caused the -38/+8 excursion if it wasn't this pattern), remain open. The weaker `t=1384014ms` event in this file (`after=822.28Hz`, well below the usual 1000+Hz cluster) is worth noting as a data point too - consistent with cycle-to-cycle variation in how deep/how sustained that particular null happened to be, which a null-noise-driven mechanism would naturally produce and a hardware-latch mechanism would not obviously explain.

**Recommended next step**: since the plain fast/slow auto-capture can no longer be trusted as a proxy for "a real jump happened," the energy-weighted EMA (already fixed with its own anchor) is now the primary tool for telling a real event from an artifact - a future auto-capture-triggered event should also print the energy-weighted reading alongside the plain one (currently `'H'`-only), so a real vs. null-noise event can be told apart from the SAME capture without needing a coincidental manual `'H'` press. Not yet implemented - proposing before building, given how much this entry already revises.

## 2026-09-16, later still: implemented the proposed fix - energy-weighted before/after on every slow-jump auto-capture, plus an independent energy-weighted "held" auto-detector

User confirmed both parts of the previous entry's proposal: "Yes" to adding the energy-weighted reading to the auto-capture printout, and yes to also auto-logging when the energy-weighted `dev` itself jumps/holds, the way the plain fast/anchor pair already does via `'H'`'s held-freq detector. Both built in `diagnostics.cpp`, no bench test yet.

**Part A - energy-weighted before/after on the slow-jump auto-capture.** The existing `AUTO-CAPTURED`/`TRIGGERED` printout (`print_and_rearm_slow_trace()`) only ever showed the plain `s_slow_trigger_before_hz`/`s_slow_trigger_after_hz` pair. Added two new statics, `s_slow_trigger_energy_before_hz`/`s_slow_trigger_energy_after_hz`, populated by calling `energy_weighted_fast_hz()` at the exact same two instants the plain pair is already captured: at trigger-arm time (inside the `if (delta > SLOW_JUMP_TRIGGER_HZ)` block) and at post-capture-close time (`if (s_slow_post_fill >= SLOW_TRACE_POST_BINS)`). A new line was added to the printout immediately after the existing `"[dsp] slow_trace: %s at t=..."` line:

```
Serial.printf("[dsp]   energy_weighted before=%.2fHz after=%.2fHz anchor=%.2fHz anchor_dev=%+.2fHz\r\n",
              s_slow_trigger_energy_before_hz, s_slow_trigger_energy_after_hz,
              s_freq_energy_anchor_hz, s_slow_trigger_energy_after_hz - s_freq_energy_anchor_hz);
```

This directly closes the gap the last entry identified: every future auto-capture (or manual `'K'`) now carries its own energy-weighted reading alongside the plain one, so a real event vs. a null-noise artifact can be told apart from a single capture, without needing a coincidentally-timed manual `'H'` press. Applied retroactively in reasoning, this is exactly the number that would have let `K_out_5.txt`'s and `K_out_7.txt`'s occurrences be distinguished from the SDR-confirmed one without needing the 13-event statistical comparison done by hand in the previous entry.

**Part B - an independent energy-weighted "held" auto-detector.** Separate from Part A, the user asked whether a *jump* in the energy-weighted `dev` itself (as printed by `'H'`, e.g. `anchor=1070.13Hz fast=1063.44Hz dev=-6.69Hz`) should also be auto-logged the way the plain fast/anchor pair already triggers `diagnostics_check_held_freq()`'s held-freq detector. Built a complete, parallel detector, `s_energy_held_*`, mirroring the existing plain one's state machine exactly (active/confirmed flags, started/last-reannounce timestamps, pending-print flags for confirm/reannounce/recovery) but keyed off `energy_weighted_fast_hz() - s_freq_energy_anchor_hz` instead of the plain fast/anchor pair. Deliberately reuses the existing, already-bench-tuned `HELD_TRIGGER_HZ` (5.0Hz), `HELD_MIN_DURATION_MS` (15000ms), and `HELD_REANNOUNCE_MS` (30000ms) constants rather than inventing new untuned thresholds - if the plain detector's tuning is trusted, there's no principled reason the energy-weighted one needs different numbers yet, and it's easy to split later if bench data says otherwise.

The trigger-check block lives in `diagnostics_set_tx_info()`, right after the existing plain `if (s_freq_anchor_inited) { ... }` block, using its own `enow_ms` timestamp:

```
if (s_freq_energy_anchor_inited) {
    float edev = energy_weighted_fast_hz() - s_freq_energy_anchor_hz;
    float edev_abs = (edev < 0.0f) ? -edev : edev;
    uint32_t enow_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (edev_abs > HELD_TRIGGER_HZ) {
        if (!s_energy_held_active) {
            s_energy_held_active = true;
            s_energy_held_started_ms = enow_ms;
            s_energy_held_confirmed = false;
        }
        s_energy_held_last_dev_hz = edev;

        if (!s_energy_held_confirmed
            && (enow_ms - s_energy_held_started_ms) >= (uint32_t)HELD_MIN_DURATION_MS) {
            s_energy_held_confirmed = true;
            s_energy_held_last_reannounce_ms = enow_ms;
            s_energy_held_pending_confirm = true;
        } else if (s_energy_held_confirmed
                   && (enow_ms - s_energy_held_last_reannounce_ms) >= (uint32_t)HELD_REANNOUNCE_MS) {
            s_energy_held_last_reannounce_ms = enow_ms;
            s_energy_held_pending_reannounce = true;
        }
    } else {
        if (s_energy_held_active) {
            if (s_energy_held_confirmed) {
                s_energy_held_pending_recovery_duration_ms = enow_ms - s_energy_held_started_ms;
                s_energy_held_pending_recovery = true;
            }
            s_energy_held_active = false;
            s_energy_held_confirmed = false;
        }
    }
}
```

Three new print consumers were added to `diagnostics_check_held_freq()`, mirroring the plain detector's three messages but clearly labeled so they can't be confused with the plain fast/anchor detector's own output: `"ENERGY-WEIGHTED CONFIRMED STUCK"` (fires once, 15s after the energy-weighted dev first exceeds 5Hz from its own anchor, and also dumps `print_held_trace()`), `"ENERGY-WEIGHTED still stuck"` (a 30s heartbeat reannounce while it remains held), and `"ENERGY-WEIGHTED RECOVERED"` (fires once when it drops back under threshold, reporting total held duration). Because this uses `energy_weighted_fast_hz() - s_freq_energy_anchor_hz` (the same anchor and function the corrected `'H'` line already prints), a genuinely energy-weighted-confirmed hold should mean real transmitted power is sitting off-frequency, not just a plain-EMA null-noise wobble - this is exactly the distinction the previous two entries needed and didn't have.

A forward declaration, `static float energy_weighted_fast_hz(void);`, was added near the `MIN_ENERGY_DEN` constant (both the Part A trigger sites and the new Part B trigger block sit earlier in the file than the function's existing definition). Verified it sits inside the same `#if AD9851_ATTACHED` guard as the call sites and matches the later definition's signature exactly.

Brace/paren balance re-verified after all edits (155/155, 1133/1133). Not yet bench-tested - next capture upload should show both new print lines and can be checked against a simultaneous SDR reading the same way `K_out_6.txt`'s manual `'H'` reads were checked.

## 2026-09-16, later still: log_20260916_111946.txt contains no diagnostic output at all - just a boot-time config dump, decoded to be preset 1's values run under the name "Live," not preset 3

`log_20260916_111946.txt` is a single line, confirmed by direct byte inspection (`wc -l` = 1, `cat -A` shows one CRLF-terminated line): a `Response:` echo of the active `PersistentSettings` struct, presumably from a `'P'`-family dump command. It contains no `'H'`, `'K'`, held-freq, or auto-capture output whatsoever - so unlike every capture logged so far this session, there is nothing internal here to cross-check the user's reported SDR readings against, and nothing to exercise the two detectors just added (auto-capture energy-weighted before/after; the independent `ENERGY-WEIGHTED` held detector).

The user flagged, correctly, that this run used the default boot config rather than their usual preset 3 ("Shelf2 Baseline gdeq adj#4"). Decoding the dumped struct field-by-field against `settings.h`'s `PersistentSettings` layout confirms it further: the dumped values (`relative_delay_samples=0.00`, `env_gdeq_enable=false`, `master_gain_db=-2.0`, `env_predistort_enable=false`, `env_floor=0.00`, `envelope_interp_enable=false`, `env_ampeq_enable=false`, `env_ampeq_shelf2_enable=false`, `env_gdeq_variant=ENV_GDEQ_VARIANT_DEFAULT`) are byte-for-byte identical to `settingsPresets[1]` ("TwoTone Base") - only the printed name ("Live" vs "TwoTone Base") differs, which just reflects that this dump ran before any `'P'`-preset load reassigned the active settings' display name this boot.

Preset 1 vs. preset 3, the fields that actually differ:

| field | preset 1 (this log) | preset 3 (user's normal) |
|---|---|---|
| `relative_delay_samples` | 0.00 | 2.00 |
| `env_gdeq_enable` | false | true |
| `master_gain_db` | -2.0 | -1.4 |
| `env_predistort_enable` | false | true |
| `env_ampeq_enable` | false | true |
| `env_ampeq_shelf2_enable` | false | true |
| `env_gdeq_variant` | DEFAULT (moot, gdeq off) | ENV_GDEQ_VARIANT_CANDIDATE_B |

So this run had every part of the envelope-shaping/timing chain this session has repeatedly implicated in envelope/freq_dev pairing mismatches (`env_gdeq`, `env_predistort`, `env_ampeq`, and a non-zero `relative_delay_samples`) switched OFF, not just "different from usual." The user reported the SDR showed only +3Hz, -7Hz, +2Hz shifts across this session - all much smaller than prior sessions' -38Hz/+8Hz manual reads or the auto-captured 700->1000+Hz jumps logged from `K_out_5.txt`/`K_out_6.txt`/`K_out_7.txt` (all of which ran on preset 3 or an equivalent gdeq/predistort/ampeq-enabled config, per earlier session context).

**Treating this as a data point, not a conclusion** - three small numbers with zero accompanying trace is thin evidence on its own, and there's no way to rule out this session simply being shorter, or landing entirely outside the ~153.6s cycle's rise phase by chance. But directionally it's the first time this session has a preset comparison naturally isolating exactly the mechanism the whole energy-weighting/pairing-mismatch theory rests on: if `env_gdeq`/`env_predistort`/`relative_delay` are what create the envelope/freq_dev timing mismatch that produces large excursions at near-null moments, running with all of them off removing that mismatch mechanism and seeing smaller shifts is consistent with (not proof of) that theory. **Recommended follow-up**: repeat this same default/preset-1 boot config for a longer session (long enough to span several ~153.6s cycles) WITH the serial log actually capturing `'H'`/`'K'`/auto-capture output this time, and compare directly against an equal-length preset-3 session - a controlled A/B on the one axis (gdeq/predistort/ampeq/relative_delay all off vs. all on) that's been missing so far.

## 2026-09-16, later still: the requested longer preset-1 capture arrives (`log_20260916_111946.txt`, real content this time, 1365 lines) - the new ENERGY-WEIGHTED held detector reveals the ~153.6s cycle is a smooth, continuous ~145s climb, not a sudden event, and pins down exactly where `env_min`'s ~0.2 floor comes from

This is the real version of the file referenced (but not actually contained) in the previous entry - same filename, re-uploaded with its full content. It is the first bench data run against both parts of the diagnostic enhancement built two entries ago (energy-weighted before/after on auto-captures; the independent `ENERGY-WEIGHTED` held detector), and it is easily the most information-dense capture of the session so far. No SDR reading was reported alongside it.

**The timeline, extracted via grep on the held_freq/auto-capture lines (full trace omitted here, see the raw file for bin-by-bin detail):** three complete cycles plus a fourth in progress at the point the log stops, all against the energy-weighted anchor `1070.13Hz`:

| cycle | held-active starts | CONFIRMED (dev) | rise auto-capture (plain before/after, delta) | energy_weighted at rise (before/after, anchor_dev) | RECOVERED (total held) |
|---|---|---|---|---|---|
| 0 (in progress at file start) | ~t=9906ms (inferred) | (not captured, before file start) | t=154755ms: 700.52->1003.01Hz, +20.64Hz | 1157.38->1190.87Hz, **+120.73Hz** | t=155006ms, **145.1s** |
| 1 | t=163611ms | t=178616ms, dev=+16.67Hz | t=308410ms: 700.65->1094.02Hz, +20.51Hz | 1157.82->1203.42Hz, **+133.28Hz** | t=308660ms, **145.0s** |
| 2 | t=317286ms | t=332293ms, dev=+16.62Hz | t=462082ms: 700.66->1043.98Hz, +22.28Hz | 1157.79->1196.54Hz, **+126.41Hz** | t=462329ms, **145.0s** |
| 3 (in progress at file end) | t=470935ms | t=485939ms, dev=+16.50Hz | (log ends at t=515944ms, "45s so far") | - | - |

Each rise is followed, ~8.1-8.6s later, by the already-documented "recovery" auto-capture (plain delta -9.93Hz, -13.03Hz, -5.49Hz across the three cycles - consistent with every prior capture's second half of the pair), and the energy-weighted `anchor_dev` at THAT instant is small (+5.63Hz, +4.64Hz) - back near baseline. The gap between a RECOVERED print and the next cycle's held-active start is consistently ~8.6-8.7s, and every full cycle is ~145.0-145.1s "held" + ~8.6s "quiet" = **~153.6-153.7s total, matching the previously-documented cycle length to within a few hundred ms**, now for the third time and under a completely different config preset than any earlier capture.

**The new finding, and it's a real one**: between the "quiet" ~8.6s window right after a RECOVERED print and the next rise auto-capture, `energy_weighted anchor_dev` does not sit flat near zero waiting for a sudden event - it climbs **smoothly and monotonically** the entire ~145s: the `still stuck` heartbeats land at +34.02/34.09Hz (45s in), +49.97/50.04Hz (75s), +64.51/64.55Hz (105s), +77.62/77.66/77.67Hz (135s) across the different cycles, essentially identical cycle-to-cycle, then continues climbing to +120-133Hz right at the next rise. The 30-second-step increments shrink slightly each time (45->75s: +15.95Hz; 75->105s: +14.51Hz; 105->135s: +13.11Hz) - a decelerating-but-still-substantial climb, not a step function. **This has no counterpart in the plain fast/slow EMA pair at all** - every trend-ring dump captured at each rise (80 samples spanning the preceding ~2s, at 25ms/sample) shows plain `fast` and `slow` sitting flat to two decimal places (e.g. `fast=698.46Hz slow=699.94Hz`, barely moving) right up to the instant of the rise. Since `s_freq_ema_energy_num_fast`/`s_freq_ema_energy_den_fast` share the exact same `FREQ_EMA_FAST_ALPHA` (50ms tau) as plain `s_freq_ema_fast_hz`, and since `dev` (`delayed_freq_dev_hz`) is demonstrably NOT drifting on this timescale (the flat plain trend proves it), a genuinely *stationary* correlation between `dev` and `env2` within each 1ms two-tone beat cycle would produce a roughly *constant* offset between the energy-weighted and plain readings - not a smoothly evolving one. A 145-second continuous, repeatable, cycle-synchronized climb in the RATIO while neither `dev` nor (per the coarse per-bin `env_min`) the gross envelope level shows any equivalent drift points at something in the dev/env2 *relationship itself* slowly rotating relative to whatever's sampling it, not at either signal alone.

**New working hypothesis (clearly unconfirmed): a slow beat between two independently-timed periodic processes, with ~153.6s as its own beat period.** Two tones at exactly 700Hz/1700Hz sampled at 16000Hz are both exactly commensurate with `SAMPLE_RATE_HZ` (confirmed earlier this session - this was the whole point of the 700/1700 config.h fix), so in isolation neither the two-tone generator's own phase accumulator nor the tick clock should drift relative to each other. But if the AD9851's own DDS output, or some other independently-clocked stage in the chain (its own crystal reference, or a separate free-running timer), differs from the ESP32's `SAMPLE_RATE_HZ`-derived tick clock by even a few parts-per-million, the *relative phase* between "where in the 1ms two-tone beat cycle a near-max-dphi/near-null moment falls" and "which absolute wall-clock tick that corresponds to" would itself slowly rotate through a full cycle at a rate set by that tiny frequency mismatch - and a full-cycle period of ~153.6 seconds corresponds to a mismatch of only a few PPM, well within the kind of tolerance mismatch two independent crystal-derived clocks would have. This would mean: the energy-weighted metric's climb tracks how far that slow rotation has drifted the "bad" (near-null, high-dphi) moments of the beat cycle into phase with whatever's doing the sampling/weighting, building up smoothly until the misalignment reaches its worst point (the rise), then something (a phase-accumulator wraparound, or the AD9851 hardware itself re-latching) resets the relative phase abruptly (the recovery), and the whole ~153.6s rotation begins again. This single mechanism would unify the well-documented recurring rise/recovery pattern (previously attributed to either null-noise or an unconfirmed AD9851 bit-bang theory) with this newly-visible smooth energy-weighted precursor - and it directly confirms/quantifies something the `K_out_6.txt` entry flagged as an open question ("the pre-rise phase has its own signature... an interesting new data point") - it now looks like that signature is not a brief pre-rise anomaly but a continuous process spanning essentially the ENTIRE inter-event interval.

**This is a hypothesis, not a finding - the decisive test is still missing.** Nothing here rules out a much more mundane explanation: a bug or an unaccounted-for interaction in the energy-weighted EMA implementation itself that produces this exact smooth-climb-then-snap-back shape in software with no real transmitted-frequency counterpart at all. The one piece of evidence that would settle it wasn't collected this session: **no SDR reading was taken during this capture.** If a future preset-1 session reads the SDR every ~20-30s across one full ~153.6s cycle (matching the `still stuck` heartbeat cadence) and the SDR shows a real, smoothly climbing shift tracking the +34/+50/+64/+77Hz progression, this is confirmed as a genuine, previously-invisible drift mechanism - arguably the most significant finding of the whole investigation. If the SDR stays flat throughout (as it did during the `K_out_7.txt` correction), the energy-weighted metric's climb is a diagnostic-side artifact, and the newly-built `ENERGY-WEIGHTED` held detector's thresholds (currently reusing the plain detector's `HELD_TRIGGER_HZ=5Hz`/`HELD_MIN_DURATION_MS=15s` verbatim) need fundamental retuning - as currently tuned, it is "confirmed stuck" for ~145 of every ~153.6 seconds (a ~94% duty cycle), which makes every future `CONFIRMED STUCK`/`still stuck` print effectively noise, not a meaningful anomaly flag, until this is resolved one way or the other. **Not changing the thresholds yet** - doing so now, before knowing whether the underlying climb is real, would risk hiding the exact signal that needs to be characterized next.

**Also resolved, and code-confirmed (not speculative) this time**: the previous entry guessed the pinned `env_min≈0.2` (instead of the near-1e-3 nulls seen under preset 3) might be a bin-phase aliasing artifact (missing the true null by unlucky sampling). Tracing the actual call site (`ssb_mic_test.ino` lines 611-658) shows a more direct explanation: `envelope_at_freq_time` (used for both `env_min` and the energy-weighting's `env2`) is captured from the `envelope` variable *after* it has already been run through the active preset's envelope-shaping chain - `envelope_ampeq_process()`, then either `envelope_predistort_process()` or the linear `envelope * env_pwm_scale + env_pwm_offset` mapping (line 628) - not the DSP's raw, pre-shaping envelope. Preset 1 has `env_predistort_enable=false`, so it takes the linear path with `env_pwm_offset=0.20f`: even when the DSP's true envelope hits an exact null (0.0), the mapped value floors at `0.0*0.90+0.20=0.20` - exactly the pinned value observed. **This is a structural property of the diagnostic as built, not a bug**: `envelope_at_freq_time` intentionally reflects "what's actually driving the PWM/RSET output" (arguably the more physically meaningful weight for judging real transmitted power), but it means the energy-weighted metric's behavior is entangled with whichever preset's downstream shaping chain (offset/scale here; the predistort LUT under preset 3) is active, not a pure read of the DSP's own envelope/freq_dev relationship. Worth remembering when comparing energy-weighted numbers across presets - they are not measuring quite the same thing across configs that use different envelope-shaping paths. Superseding the previous entry's aliasing guess: the offset-floor explanation is direct, mechanical, and doesn't require unlucky phase alignment to work.

## 2026-09-16, later still: the decisive SDR check comes back - the smooth ~145s energy-weighted climb is NOT a real transmitted-frequency drift. Hardware-clock-beat hypothesis de-prioritized; the preset-1 offset floor is now the leading explanation

User confirms: "+3 -7 +2 during this log" - the SDR moved no more than a few Hz across the ENTIRE session covered by `log_20260916_111946.txt` (the three ~153.6s cycles just logged, where `energy_weighted anchor_dev` climbed smoothly from ~5Hz up to +120-133Hz each cycle before snapping back). This is the exact test the previous entry called "the single decisive missing test," and it comes back cleanly on one side: **the smooth energy-weighted climb has no real-world counterpart.** Whatever is producing it lives entirely in software/diagnostics, not in the actual transmitted signal.

**This de-prioritizes the previous entry's "slow few-PPM hardware clock beat" hypothesis as the primary explanation.** A genuine beat between two independently-clocked hardware processes (the AD9851's own reference vs. the ESP32's tick clock) would, by construction, show up as an actual shift in the transmitted carrier - that's the whole premise of a clock-beat causing the well-documented real rise/recovery jumps in `fast`/`slow`. Since the SDR stayed within single-digit Hz through multiple complete cycles of the energy-weighted climb, whatever's driving THIS specific smooth-climb phenomenon is very likely confined to computation, not radiated frequency - a materially different (and more mundane) conclusion than the hypothesis floated two entries back.

**Leading explanation now**: the same entry's other finding - `envelope_at_freq_time`'s floor at `env_pwm_offset=0.20` under this preset (predistort off) - isn't just a cosmetic explanation for the pinned `env_min` column, it likely explains the climb itself. `dev` (`delayed_freq_dev_hz`) is computed upstream in `ssb_dsp_process_sample()`, from the raw analytic-signal envelope - NOT the PWM-shaped one - so `dev` still has its usual large excursions at genuine near-null instants regardless of what preset is active (the underlying two-tone math doesn't know or care about `env_pwm_offset`). But `env2` (the WEIGHT applied to those `dev` samples in the energy-weighted average) is computed from the PWM-shaped envelope, which under this preset never drops below `0.20² = 0.04` even at a true DSP null. The entire point of energy-weighting is to give near-null moments a near-zero weight so their wild `dev` excursions don't contaminate the average - under preset 3 (predistort on), that apparently works as designed (see `K_out_6.txt`'s clean -38.37Hz-vs-SDR's-38Hz match). Under preset 1, a true null still only gets weighted down to 4% instead of ~0%, letting a non-negligible fraction of near-null `dev` noise leak into `num_fast`/`den_fast` - a structural reason the energy-weighted metric could behave badly specifically on this preset, independent of anything physically happening on the RF side.

**What's still genuinely unexplained**: why a leak like that would produce a smoothly, monotonically CLIMBING bias sustained over ~145 continuous seconds, rather than settling to a bad-but-stable offset within a handful of the fast EMA's own 50ms time constants (as a stationary per-cycle leak, applied to an exactly-periodic 1ms two-tone beat, should). That gap between "why the mechanism could be wrong" and "why it produces THIS specific slow-climb-then-snap shape" is still open, and isn't resolved by this entry - flagging honestly rather than reaching for a tidy but unverified story.

**Practical conclusion**: the `ENERGY-WEIGHTED` held detector, as built and tuned, should NOT be trusted as a real-event indicator on preset 1 (or, by the same reasoning, probably any preset combining a nonzero `env_pwm_offset` with `env_predistort_enable=false`) - it reports "confirmed stuck" essentially continuously regardless of what the SDR actually shows. Its Part-A counterpart (the energy-weighted before/after on the plain slow-jump auto-capture) is likely affected the same way, for the same reason - the `+120-133Hz anchor_dev` values printed alongside each rise in this same log are now understood to be inflated by this same floor effect, not a reliable read of "was this rise real." **Recommended next step, cheap and decisive**: repeat the identical test on preset 3 (predistort on, no offset floor) with a running SDR check across a full cycle - if the energy-weighted detector stays quiet there (small, bounded dev) except when a real SDR-visible event happens, that confirms the offset-floor explanation and tells us the energy-weighted diagnostics are trustworthy specifically on presets without a nonzero linear PWM floor, and should be read with that caveat everywhere else. Not proposing a firmware change yet (e.g. warning when `env_pwm_offset>0 && !env_predistort_enable`) until that comparison confirms the mechanism - happy to build it once it does.

## 2026-09-16, later still: the requested preset-3 comparison arrives (`log_20260916_122436.txt`, 4103 lines, with the user's own manually-logged SDR readings interleaved) - a completely new, striking finding: `held_trace`'s own `tx_freq` column shows a discrete, ~153.6s-long, exactly-8000Hz "phantom" excursion with no real counterpart anywhere - canary, `fast`/`slow`, and the SDR all agree nothing real happened

This is the requested preset-3/predistort-on run with a running SDR check, done manually: the user added inline `SDR frequency updated to <value> MHz` lines (mislabeled unit - these are Hz, e.g. `14200678`) whenever the reading moved by roughly 4Hz or more, on the stated rule "if SDR is not logged, it didn't change." `delay=+2.00` confirms this really is preset 3 (`relative_delay_samples=2.00`).

**The SDR's own range across the whole ~18-minute session is small**: extracting all 14 SDR update lines (many repeated/unchanged), the value ranges only from `14200678` to `14200702` - a 24Hz total spread across the entire capture, spanning about 7 complete ~153.6s cycles. That's consistent with everything logged about this project's real, SDR-visible frequency stability all session.

**The new finding.** While scanning `held_trace`'s `tx_freq` column (the literal integer Hz value returned by `carrier_output_set_freq_dev()` and stored as "ground truth, independent of any upstream DSP/delay-line reasoning" per its own doc comment) across the whole file, three `print_held_trace()` dumps in a row - triggered by three consecutive `ENERGY-WEIGHTED CONFIRMED STUCK` events between `t=615687ms` and `t=784374ms` - show `tx_freq` sitting at **`~14193358Hz`, a full ~8000Hz below the normal baseline of `~14201359Hz`**, tightly clustered (±1-2Hz jitter, the same noise level as normal baseline) for the ENTIRE span. Lining the three dumps up end-to-end (each is a 120-sample/60-second rolling window) shows this isn't three separate events - it's **one continuous ~153.6-second episode**, starting right around `t=615687ms` (the instant that cycle's `ENERGY-WEIGHTED` held state first armed) and ending right around `t=769299ms` (the instant of that cycle's plain-fast/slow `AUTO-CAPTURED` rise event) - i.e. it spans almost exactly one full documented cycle, boundary to boundary. The transition INTO the anomaly (`held_trace[-30]: tx_freq=14193067Hz fast=706.37Hz`, a noisier one-sample blip) and OUT of it (`held_trace[-30]` of the third dump: `tx_freq=14201535Hz fast=709.38Hz`, another one-sample blip, elevated `fast` matching the exact signature of every already-documented rise event) both land right at the edges of the known cycle.

**Three independent checks all say this is not real:**
- **The canary is silent.** `canary_check_background()` (built 2026-09-09 for exactly this class of suspicion - a corrupted persistent value like `s_carrier_hz`) checks every `diagnostics_service()` tick and latches permanently on the first mismatch. Zero `[canary]... MISMATCH` lines appear anywhere in this 4103-line file. Since `tx_freq = s_carrier_hz + (int32_t)delayed_freq_dev_hz` and the canary directly confirms `s_carrier_hz` never changed, the ~8000Hz gap cannot be explained by the 2026-09-09 "one-off corrupted persistent value" theory - that theory specifically predicts a permanent, non-self-healing step, and this one recovers.
- **`fast` (and `slow`) stay completely flat through the whole episode** - printed as `695.34Hz` on every single sample across all three dumps, to two decimal places. `fast` is the EMA of the exact same `delayed_freq_dev_hz` that feeds `carrier_output_set_freq_dev()` - if `delayed_freq_dev_hz` had genuinely sat ~8000Hz low for 153 continuous seconds (thousands of the fast EMA's own 50ms time constants), `fast` would have tracked it almost immediately. It didn't move.
- **The SDR shows nothing** - the readings logged immediately before, during, and after this window (`14200687`, then a long unchanged run of `14200686` repeated 13 times) show no shift at all, and an 8000Hz shift would be trivially, unmistakably obvious on any receiver - not a subtle few-Hz artifact.

**Leading explanation: `held_trace`'s own sampling grid is exactly phase-locked to the two-tone period (a fact already documented this session - `HELD_TRACE_SAMPLE_TICKS=8000` is an exact multiple of the two-tone's 16-tick period, `8000/16=500` exactly), so it always snapshots the SAME relative phase of the periodic waveform, tick after tick - unless that relative phase itself shifts, e.g. from a single skipped or double-counted DSP tick somewhere in the pipeline.** A tick-count slip wouldn't change what's actually being transmitted (the real two-tone signal, and the real per-tick `carrier_output_set_freq_dev()` calls driving the AD9851, continue completely unaffected every 62.5us) - it would only change WHICH single instant of that already-existing, already-periodic waveform `held_trace` happens to sample every 500ms. If that shift lands the new sampling phase on a point near the beat cycle's phase-discontinuity/near-null zone (where the raw, single-tick instantaneous `dphi`-derived `freq_dev` is known, from this session's own null-noise theory, to swing to large values), you get exactly this signature: a discrete, sustained-looking, but entirely record-side jump with zero real transmitted-signal or SDR consequence - the raw instantaneous `dev` implied by the anomalous reading (`14193358-14200160(=CARRIER_HZ)=-6802Hz`) is well within the range this session has already established near-null moments can produce, while the *time-averaged* `fast`/`slow` (which sees ALL phases equally, unaffected by which single instant gets snapshotted) is correctly unaffected.

**This would mean `held_trace`'s reported `tx_freq` is not, in fact, "ground truth independent of the upstream DSP/delay-line reasoning" the way its own doc comment claims** - it's a single 62.5us-wide instantaneous sample of a signal that swings enormously across each 1ms beat cycle, aliased to a fixed relative phase by design, and therefore exactly as vulnerable to a rare tick-count slip revealing a different (and potentially extreme) phase as any of this session's other tick-aliased diagnostics. Not a new mechanism so much as a sharper, more dramatic instance of the very first aliasing finding logged this session (`held_trace`'s "stuck-looking" `tx_freq` column) - this time landing on the periodic waveform's extreme rather than its flat part.

**Suggestive, not proven: this episode's boundaries (`t=615687ms` to `t=769299ms`, ~153.6s) line up almost exactly with one full instance of the well-documented recurring cycle**, and both edges show the same one-sample `fast` blips (`706.37Hz`, `709.38Hz`) that mark every other already-logged rise/recovery boundary. This raises the possibility that whatever causes the recurring ~153.6s cycle IS an occasional missed/delayed DSP tick (a `dsp_task` deadline slip), and that this tick slip has (at least) two separate visible fingerprints already found this session: the well-known plain-EMA-visible rise/recovery jump (a genuine, if brief, real perturbation to the transmitted `freq_dev`, per the null-noise theory), and now this `held_trace` phase-alias jump (a purely diagnostic-side artifact of which instant gets sampled). **Not confirmed** - the log doesn't include `[timing]` block output (`busy_us`/`overrun_count`/`late_tick_count`), which would be the direct, decisive test: if a future capture shows an overrun/late-tick spike at the exact instant one of these cycle boundaries fires, that would confirm the tick-slip mechanism outright rather than by inference. Recommended for the next bench session on either preset.

**Separately, closing the loop on the actual requested test (preset-3 energy-weighted behavior vs. preset 1's): better, but not clean.** Every plain-fast/slow `AUTO-CAPTURED` "rise" event's accompanying `energy_weighted before` value clusters tightly around **`~1026-1030Hz`** (1029.26, 1029.50, 1026.65, 1029.79, 1026.39 across five separate rises with different real magnitudes), and every "recovery" event's `energy_weighted before` value clusters just as tightly around **`~1145Hz`** (1145.09, 1146.05, 1145.36, 1144.97, 1144.96, 1145.15, 1144.86 across seven instances) - both essentially independent of that specific cycle's actual `delta`/SDR movement. So even on preset 3, with predistort enabled and the offset-floor issue absent, the energy-weighted metric still carries a large (~+75Hz vs. anchor at the recovery instant), highly consistent, **cycle-phase-locked** component that has nothing to do with how big the real event was that cycle - smaller than preset 1's ~130Hz swing, but not the clean, real-event-only signal the previous entry's hypothesis hoped for. The offset-floor explanation from two entries back is probably still part of the story (preset 1's swing is meaningfully larger), but it isn't the whole story - there's a structural, phase-locked bias in the energy-weighted metric on both presets, just of different sizes. Worth remembering before treating any single energy-weighted reading (Part A's before/after, or Part B's held detector) as a clean stand-in for "did something real happen" on ANY preset.

## 2026-09-16, later still: added overrun/late-tick/max-busy_us data to `held_trace`, directly answering the previous entry's "not confirmed" gap

User asked whether the overrun data could be added to this logging - exactly the missing piece the previous entry flagged as the decisive test for the tick-slip theory behind the `tx_freq` phantom-shift finding. Implemented in `diagnostics.cpp`, not yet bench-tested:

Three new arrays, `s_held_overrun_count[HELD_TRACE_LEN]`, `s_held_late_tick_count[HELD_TRACE_LEN]`, `s_held_max_busy_us[HELD_TRACE_LEN]`, added alongside the existing `s_held_tx_freq[]`/`s_held_fast_hz[]` ring. Deliberately implemented as plain per-sample SNAPSHOTS of the three counters that already exist and are already exercised every tick - `s_dbg_overrun_count`/`s_dbg_late_tick_count` (`diagnostics_record_tick_start()`/`diagnostics_record_phase_timings()`, both pre-dating this session) and `s_dbg_max_busy_us` - rather than new per-bin accumulation/reset logic of their own. Given this whole investigation started from suspecting a bug in exactly this kind of new diagnostic code, reusing already-tested counters rather than inventing a second new mechanism right where one is being hunted for felt like the safer choice, even though it means the ring holds cumulative-since-last-`'r'`-reset values rather than clean per-bin deltas.

Written at the exact same site as the existing `tx_freq`/`fast_hz` capture (inside `diagnostics_set_tx_info()`'s continuous 500ms-bin block), so all five fields share the same timestamp/index. `print_held_trace()` now prints all three on every line:

```
Serial.printf("[dsp]   held_trace[%+5d]: tx_freq=%luHz fast=%.2fHz overruns=%lu late=%lu max_busy_us=%lu\r\n", ...);
```

Since each of the three is monotonically non-decreasing (only ever resets on a manual `'r'`), reading down the printed column and spotting exactly which line each number steps up on pinpoints exactly which 500ms bin saw a real DSP-timing hiccup. If the next `tx_freq` phantom episode's onset or recovery instant lines up with a step in `overruns`/`late`/`max_busy_us`, that confirms the tick-slip theory outright rather than by inference; if the phantom recurs with all three flat throughout, the tick-slip theory is ruled out and the search moves elsewhere (a bug in the ring capture itself becomes the leading candidate). Brace/paren balance re-verified (155/155, 1143/1143) after the edit. Not yet bench-tested - next capture (either preset) should show all three columns and can be checked directly against wherever a phantom-tx_freq episode (or the known rise/recovery boundaries) falls.

## 2026-09-16, later still: `log_20260916_131428.txt` (preset 3, same setup, new overrun columns live) - the tick-slip theory is DECISIVELY FALSIFIED, and the phantom `tx_freq` episode turns out to be deterministic to single-digit milliseconds across two independent boots

Second preset-3 run, same config as the previous file, now with the new `overruns`/`late`/`max_busy_us` columns active (1727 `held_trace` samples total). Result is unambiguous:

**`overruns=0` and `late=0` on every single one of the 1727 printed samples, with no exceptions anywhere in the file** - including every sample of the recurring `tx_freq` phantom episode itself (same signature as before: three trace dumps, lines 2324-2353/2358-2477/2810-2900, forming one continuous ~8000Hz-low excursion). `max_busy_us` does move (46-59us across the session) but never once approaches, let alone exceeds, the 62.5us tick budget (`k_sample_period_us`) - and reads exactly `50` at both the phantom's onset tick (`held_trace[-30]: tx_freq=14193067Hz ... max_busy_us=50`) and its recovery tick (`held_trace[-30]: tx_freq=14201535Hz ... max_busy_us=50`), identical to the completely normal samples immediately on either side. **This rules out the tick-slip/missed-DSP-tick theory outright** - there is no timing hiccup of any kind, at any granularity these counters can see, anywhere near this event.

**The bigger surprise: the phantom episode recurred with the same ~8000Hz magnitude at essentially the identical elapsed time since boot.** Lining this file's cycle timestamps up against the previous file's: the `+69Hz`-side `CONFIRMED STUCK` event whose trace dump contains the phantom's onset shows `started_at t=615687ms` in **both files, to the millisecond** (`confirmed_at` differs by only 6ms: 630688 vs. 630694). The event whose dump contains the phantom's recovery shows `started_at t=769365ms` in **both files, again to the millisecond** (`confirmed_at` differs by 8ms: 784374 vs. 784366). Every other `CONFIRMED STUCK`/`RECOVERED` timestamp across both ~18-minute, 7-cycle sessions matches this same single-digit-millisecond precision. **This is not the signature of a random event** - no RFI pickup, cosmic-ray-style SRAM bit flip, or marginal-timing SPI glitch would reproduce this precisely across two independent power-on boots. It's the signature of something deterministic, keyed to elapsed time since boot (not wall-clock time, not a random trigger), that happens to land during this one specific cycle (the one starting at `t=615687ms`, roughly the 4th of the session's ~153.6s cycles) in both runs.

**Not yet identified**: searched the codebase for any constant near 615000, 614400, or 153600 (ms) that might explain a matching deterministic timer/counter and found nothing obvious. The whole-session determinism (matching to single-digit ms) argues against coincidence, but nothing here yet points at WHERE such a mechanism would live - it could be in this project's own code, or in an ESP-IDF/FreeRTOS housekeeping process running underneath it (a periodic internal calibration, a scheduler artifact, a counter wraparound) that hasn't been instrumented. **Recommended next steps**: (1) a single MUCH longer capture (several hours) to see whether the phantom recurs again near further multiples of ~615s (e.g. ~1230s, ~1845s) - true periodicity vs. a genuine one-off-per-boot would meaningfully narrow the search; (2) since this is now confirmed unrelated to `dsp_task` scheduling, the search should move away from the DSP hot path and toward anything else in the firmware or runtime with its own independent timing (background/system tasks, one-shot boot-relative timers, calibration routines) rather than continuing to instrument the tick-timing counters further - those have now done their job and cleanly cleared themselves.

## 2026-09-16, later still: `log_20260916_193030.txt` - the user's new external audio-tone counter gives the strongest confirmation yet that the energy-weighted climb isn't real, but its own readings raise a new, unexplained puzzle; the `tx_freq` phantom recurs a third time, still clean on overruns, still boot-locked to the same millisecond

User built an automated audio frequency counter (external to the firmware) that measures the actual 700Hz two-tone's offset directly, triggered by the relevant diagnostic log messages ("Audio tone at held_freq: X Hz" / "...at trend[-1]:" / "...at held_trace[-1]:"). This is a genuinely new, higher-time-resolution, independent ground truth - previously only a manually-read SDR snapshot was available. 3353-line capture, ~939s, 7 full `~153.6s` cycles.

**Within-cycle confirmation (the main question this was meant to answer): audio tone stays essentially flat while the energy-weighted metric swings by tens of Hz.** Example from the second cycle (`t=169698ms` to `t=323345ms`): `energy_weighted` `dev` goes `+68.15 -> +40.14 -> +15.45 -> (recovers) -> -16.57` Hz from anchor, while the audio counter reads `1.3, 1.5, 2.3, 2.3` Hz across the exact same span - a ~1Hz spread against a ~85Hz claimed swing. This pattern repeats identically in every one of the 7 cycles (full timeline cross-checked). Also confirmed: **the plain (non-energy-weighted) `[dsp] held_freq:` detector never fired once in this entire 939-second session** - only the `ENERGY-WEIGHTED still stuck`/`CONFIRMED STUCK` variant appears anywhere in the file (42 lines, all energy-weighted). Plain fast/slow genuinely saw nothing, for the fourth capture running. This is the cleanest evidence yet that the energy-weighted climb/swing is an artifact of the weighting scheme, not a real transmitted event.

**The new puzzle: the audio tone's own flat "baseline" steps to a different value at every `~153.6s` cycle boundary, and the steps are large and don't point in a consistent direction.** Tabulating the audio tone's stable value across each of the 7 cycles:

| Cycle (approx t range, ms) | Audio tone plateau (Hz) |
|---|---|
| 1 (0 - 154604) | -13.2 to -14.0 |
| 2 (154689 - 308312) | +1.3 to +2.9 |
| 3 (308340 - 461984) | -74.2 to -77.8 |
| 4 (462013 - 615655) | +79.8 to +83.4 |
| 5 (615687 - 769338) | -13.4 to +0.2 |
| 6 (769365 - 923008) | -1.1 to +0.8 |
| 7 (923039 - EOF) | -9.7 (one sample, file ends) |

Step sizes between consecutive plateaus: +15.7, -77.6, +157.1, -88.0, +6.5, -9.7 Hz. No monotonic drift, no repeating sign pattern, no obvious relationship to the well-documented real events in this same file (the paired rise/recovery `AUTO-CAPTURED` deltas, which are all a modest +15 to +23 Hz / -6 to -13 Hz and match the SDR-confirmed real jump size from earlier captures). The magnitude of some of these steps (up to ~157Hz) is far larger than anything ever confirmed real by SDR, and the plain fast/slow EMA - which would show any real ~150Hz swing instantly - stays completely silent throughout (consistent with the point above).

**This is flagged as an open question, not a conclusion**, for one specific reason: nothing here has visibility into how the new audio counter itself is built, triggered, or calibrated. `held_trace`'s own `tx_freq` phantom (below) is proof that this exact codebase already contains at least one diagnostic that samples a fast-swinging periodic waveform at a fixed relative phase and gets aliased onto very-wrong-looking-but-fake values when that phase shifts - a plausible analogous failure mode for a new, home-built frequency counter would be a re-lock or cycle-miscount at a boundary that happens to coincide with these same `~153.6s` cycle edges (since the counter is explicitly triggered off the same log messages that mark those edges). Equally plausible: this is a real, previously-invisible RF phenomenon that only a fast, automated counter could catch, in which case the total silence from canary/fast-slow/SDR would need a separate explanation for why it's invisible to all of them. **No firmware change made based on this** - recommended next step is either (a) a description or capture of the audio counter's own working (sample window length, whether it's a zero-crossing counter, how it handles the trigger race against the two-tone's own beat), or (b) a much longer, repeated-preset capture to see whether the *sequence* of plateau values itself repeats across boots the way the `tx_freq` phantom's timestamps do - a repeating sequence would point strongly at a counter-side artifact; a non-repeating one would argue for something real.

**`tx_freq` phantom recurs a third time, still fully clean on the overrun test, still locked to the same boot-relative millisecond.** Three anomalous `held_trace` dumps again present (lines 2150-2179 / 2189-2308 / 2646-2736, same 30/120/91-line run-length signature as both previous captures, 241 total anomalous lines - identical count to both prior files). `overruns=0` and `late=0` on every single line across all three runs, and `max_busy_us` reads a flat `54` throughout with no deviation from the surrounding normal samples (overall-file range for this capture: 49/53/54, still comfortably under the 62.5us tick budget) - the tick-slip theory stays dead on its third independent test now, not just its second. The bounding `CONFIRMED STUCK` timestamps again match both previous captures to the millisecond: `started_at t=615687ms` (onset side) and `started_at t=769365ms` (recovery side), for the third boot in a row. The audio tone readings bracketing this exact window (`-6.0, -3.9, -9.4, 0.2, -11.9, -13.4` Hz) are unremarkable and consistent with cycle 5's plateau above - no audio-frequency signature coincides with the phantom, consistent with it having zero real transmitted effect, same as the canary/fast-slow/SDR checks already established.

## 2026-09-16, later still: user describes the audio counter's own design (8192-sample FFT peak search + interpolation @ 48kHz) - the numbers make a strong, concrete case that it's being fooled by the already-known 8Hz/40Hz sideband family, not measuring a real frequency

User supplied the counter's actual implementation: 8192 samples at 48kHz, peak search with interpolation between bins, stated to give ~0.3Hz resolution "for a clean tone" - a caveat worth taking literally, since this measurement is never looking at a clean tone.

The numbers: bin width = `48000/8192 = 5.859375Hz`, window length = `8192/48000 = 170.667ms`. Two things line up badly for this specific signal:

1. **700Hz itself sits almost exactly at the worst possible bin offset.** `700/5.859375 = 119.467` - i.e. the true tone sits `0.467` of a bin away from the nearest bin center, versus a best case of `0.0` (bin-aligned, zero leakage) and a worst case of `0.5`. At `0.467` this is about as bad as it gets: for an unwindowed (rectangular) FFT this means the true tone's own spectral leakage smears substantially into neighboring bins rather than concentrating cleanly in one - the ~0.3Hz "clean tone" spec almost certainly assumes something closer to bin-aligned, or at least assumes no other spectral content nearby to compete with that leakage.

2. **The already-documented 8Hz-spaced sideband family (elevated at 40Hz, see the 2026-09-16 "user spots 8Hz-spaced sidebands" entry above) sits right on top of that leakage.** `40Hz = 6.83` bins away, and the `8Hz` fundamental spacing itself is `1.365` bins - meaning the whole comb of sidebands (8, 16, 24, 32, 40Hz...) also fails to land on clean bin centers and would appear as a smeared, overlapping ripple of energy immediately around the main 700Hz lobe, not as isolated, cleanly-resolved lines. A peak-search-and-interpolate algorithm assumes it's fitting a single clean lobe; here it's fitting whatever shape results from the true 700Hz lobe's own worst-case leakage superimposed on a nearby comb of real (and previously confirmed, independently, via direct SDR observation) harmonic energy.

Checked whether the plateau-to-plateau step sizes from the previous entry land on clean multiples of the 5.859Hz bin width (which would point specifically at the peak search jumping between whole bins): `15.7Hz=2.68 bins`, `77.6Hz=13.24 bins`, `157.1Hz=26.81 bins`, `88.0Hz=15.02 bins`, `6.5Hz=1.11 bins`, `9.7Hz=1.66 bins`. Only one (`88.0Hz`) lands close to an integer count; the rest don't. That actually fits the leakage/bias picture better than a clean bin-hop picture would: a quadratic (or similar) interpolator being pulled off-center by a nearby competing lobe produces a continuously-variable bias, not a quantized jump - so non-integer steps are exactly what "correct peak, biased interpolation" would look like, whereas clean integer steps would point at "wrong peak entirely, correct interpolation around it."

**This is now the leading explanation for the plateau-stepping puzzle**, and it neatly ties two previously-separate open items together: the counter isn't measuring a clean 700Hz tone, it's measuring 700Hz-plus-a-real-sideband-comb whose own relative phase/amplitude against the counter's fixed 170.7ms capture window isn't synchronized with the diagnostic cycle that triggers each measurement - so each trigger effectively samples a different, unpredictable interference condition, consistent with a per-cycle step rather than a smooth drift. It does NOT contradict anything upstream: canary/fast-slow/manual-SDR-frequency-reading were all measuring/checking the actual DDS frequency word or a human-read carrier position, none of which this leakage mechanism touches - this is specifically about a peak-search algorithm's vulnerability when its target isn't a clean tone, which is a property of the new instrument, not the transmitted signal.

**Not yet confirmed, and no firmware change indicated by this alone.** Concrete, cheap next tests if wanted: (a) apply a window function (Hann/Hamming) to the counter's FFT before the peak search - would suppress leakage from the 700Hz tone's own off-center position and from the sideband comb, and should tighten the plateau spread if this theory is right; (b) constrain the peak search to a narrow band around the expected ~700Hz (e.g. +/-15Hz) rather than an open search, so it can only ever report an offset from the true carrier's own lobe, not jump to a competing local maximum; (c) log the raw winning bin index (or a small window of nearby bin magnitudes) alongside each audio-tone reading, which would make it possible to see directly whether the "peak" is moving between distinct bins or whether it's the interpolation being biased within one.

## 2026-09-16, later still: the sideband-leakage theory for the audio-counter plateau puzzle is DECISIVELY FALSIFIED by simulation - real sideband levels are ~100x too weak; leading candidate moves to the SDR's own receive chain, not the counter's math or the transmitter

User pushed back on the previous entry's leakage theory with three concrete, hard numbers: (1) the 8Hz/40Hz sidebands are normally 30-40dB down, not comparable in size to the main tone; (2) a bigger FFT block was considered for better resolution but rejected over the real risk of smearing across a genuine mid-block frequency change; (3) the SDR itself sits behind a 150Hz filter, so anything further out is already suppressed before it reaches the audio counter at all.

Point (1) alone is fatal to the leakage theory, and it's worth proving numerically rather than taking on faith either way - simulated the counter's exact described algorithm (8192-pt FFT @ 48kHz, rectangular/unwindowed, peak-search + 3-point log-magnitude parabolic interpolation - the standard technique matching the ~0.3Hz "clean tone" spec) against a 700Hz tone plus a single interferer at 8/16/24/32/40Hz offset, amplitude -30dB and -40dB down, swept across all interferer phases to find the worst case:

```
baseline (no interferer), rectangular window: 699.60 Hz  (-0.40Hz, purely from 700Hz sitting
                                                            0.467 bin off-center - matches the
                                                            previous entry's leakage-position math)
interferer -30dB @ +40Hz offset: worst-case bias = -0.41 Hz   (vs. -0.40Hz baseline - i.e. +0.01Hz added)
interferer -40dB @ +40Hz offset: worst-case bias = -0.40 Hz   (vs. -0.40Hz baseline - negligible)
```
(full sweep across 8/16/24/32/40Hz and both signs in `/tmp/.../scratchpad/sim.py` - every case lands within +/-0.05Hz of the no-interferer baseline)

**A -30 to -40dB sideband cannot move this algorithm's estimate by more than a few hundredths of a Hz, even at the worst possible phase.** The observed plateau steps are 15 to 157Hz - two to three orders of magnitude larger than anything this mechanism could produce. The leakage theory from the previous entry is dead. Worth noting what the simulation *does* confirm: the algorithm's baseline accuracy under real-world non-bin-aligned, unwindowed conditions is already good (~0.4Hz, consistent with the user's ~0.3Hz "clean tone" spec) and essentially unaffected by sidebands at the levels actually present on this signal - so the counter's own FFT math is very likely NOT the source of the plateau-stepping either, closing off both of the last two candidates (tick-slip already dead, sideband-leakage now also dead).

This also answers the block-size question directly: since the current 8192-sample window is already accurate to well under 1Hz against the real interference actually present, a bigger block would buy resolution the puzzle doesn't need, while adding exactly the real risk flagged - smearing across a genuine transition if one falls mid-window (this project already has several fast, ~8s-apart real transitions on record, e.g. the paired rise/fall `AUTO-CAPTURED` events). **Recommend not increasing block size for this purpose** - it wouldn't address the plateau-stepping (which the numbers now say isn't a resolution problem) and would add real risk for no benefit.

With both the counter's math and the transmit-side DSP (canary/fast-slow/`tx_freq` all already clean, see prior entries) ruled unlikely to be the source, and the 150Hz SDR filter confirming there's no other far-out spectral content that could be involved anyway, **the leading candidate moves upstream of the counter and downstream of the transmitter: the SDR receiver's own signal chain** (local-oscillator/reference stability, any auto-tracking or AGC behavior, or the tuned filter's own center-frequency stability) rather than anything in this project's firmware. This lines up with an already-noted detail from much earlier in this investigation: the user found "tracking in SDR is not reliable" and has been selecting frequency manually during manual SDR logging specifically because of it - independent, prior evidence that this particular SDR's own frequency behavior isn't fully trusted, from before this audio counter existed.

**Not proven - genuinely open.** This can't be resolved from the log alone; it needs either an independent, non-SDR-chain frequency reference at the same instants (defeats the purpose of the automated counter, but would isolate the SDR as a variable), or specifics on the SDR hardware/software's own frequency stability and whether it has any active tracking/AGC/AFC loop that could account for a receiver-side step this large. Until then, the plateau-stepping in the audio counter's readings should be treated as unexplained and NOT used as evidence of a real transmitted-frequency event - it doesn't change anything about the transmit-side conclusions already reached (all still resting on canary + fast/slow + `tx_freq`/overrun data, none of which run through the SDR at all).

## 2026-09-16, later still: `log_20260916_200756.txt` ends the session in a "very noisy mode" the user compares to the known x4-interp burble - completely invisible to every frequency-side diagnostic currently in place, which is itself informative

User reported the run ended in a noisy mode "seen before... the same sort of noise we saw trying to run the x4 Interp," self-recovering after as much as ~155s or needing a restart. No text description of the noise's own character was logged here (audible/SDR-observed, not something typed into the capture) - this entry covers what the diagnostic data does and doesn't show.

This is a mid-session capture (no boot/config dump - starts at `t=1473704ms`, ends at `t=3163848ms`, ~1690s / ~28min covered, spanning roughly eleven `~153.6s` cycles). Checked exhaustively for anything unusual anywhere in this window, using every frequency-side instrument this project has built up: `overruns=0` and `late=0` on all 2640 `held_trace` samples, `max_busy_us` sits at its normal 43-55us band throughout (nothing approaching the 62.5us budget), no canary lines (zero corruption events), `fast`/`slow` stay completely flat between cycles as always, and the usual `~153.6s` energy-weighted stuck/recover/`AUTO-CAPTURED` cadence runs cleanly and completely normally right up to the file's last line - which is an ordinary `Audio tone at held_freq: -6.8 Hz` immediately after a completely unremarkable `ENERGY-WEIGHTED RECOVERED at t=3163848ms`. **Nothing in this file shows any sign of the reported noisy episode at all** - no timing hiccup, no frequency excursion, no unusual print pattern, right up to the point the capture simply stops.

This absence is itself a meaningful data point, not a null result. This project has already independently root-caused an "audible burble" symptom once before (`envelope_interp.h`'s `ENVELOPE_INTERP_FACTOR` history): confirmed twice on real hardware that running `dsp_task`'s wake rate at 4x (64kHz instead of 16kHz, whether or not `'I'`'s own interpolation math is engaged) produces exactly this kind of noise, traced to the ISR/task-wake overhead itself, not to any per-tick compute-time overrun. Critically, **the counters this investigation has been relying on all session (`overruns`/`late`/`max_busy_us`) measure per-tick BUSY duration against the 62.5us budget - they say nothing about how often the task is being woken in the first place.** The x4-interp case needed the wake rate itself to change before it reproduced; a transient, momentary excursion in wake rate or ISR load (a spurious extra timer fire, a scheduler hiccup, anything upstream of `dsp_task`'s own tick body) could in principle produce the same audible symptom while leaving every counter in this file completely clean, simply because none of them were built to see that particular failure mode. This is a real, structural gap in current instrumentation, not a reason to doubt the clean readings themselves.

**One numeric coincidence worth flagging, not yet explained**: the user's own reported self-recovery window ("may 155s later") sits suspiciously close to this project's extensively-documented `~153.6s` energy-weighted cycle length - the same period behind the stuck/recover pattern, the `tx_freq` phantom, and the audio-counter plateau puzzle. Nothing here demonstrates a causal link (a human-estimated "may be ~155s" recovery time and a precisely-measured 153.6s DSP cycle are not the same kind of number, and could easily just both be "call it two and a half minutes"), but given how many previously-separate threads in this investigation have turned out to share that exact period, it's worth keeping in mind rather than dismissing as coincidence outright.

**Not actioned - no firmware change indicated.** This needs either (a) the user's own description of what the noise actually sounds/looks like on the SDR (broadband burble vs. the already-known ~20Hz Serial-print sideband signature vs. something else), to check it against the two already-documented noise mechanisms in this project's history before assuming a third, new one; or (b) if the user wants this specific symptom root-caused going forward, a genuinely new kind of instrumentation - not another frequency/tick-timing counter, but something that watches `dsp_task`'s actual wake-to-wake interval or the ISR's own fire rate directly, since that is precisely the class of event none of the current diagnostics were built to catch.

## 2026-09-16, later still: `log_20260916_205209.txt` - ADC disabled as a candidate and ruled out by the user's own observation; a second independent noisy episode (this time fully captured, snapping back on its own) again leaves zero trace anywhere in the diagnostics

User tried disabling the ADC in `config.h` as a candidate noise source and reports no difference - the noisy mode still occurred. Taken at face value (no config dump present in this mid-session capture to independently confirm the build change took effect, but no reason to doubt it): **ADC capture is ruled out as the cause.**

This capture (`t=923077ms` to `t=1473746ms`, ~550.7s, ~847 `held_trace` samples, three-and-a-bit `~153.6s` cycles) is the second in a row where the user reports a real noisy episode - this time explicitly mid-session and self-recovering before the capture ends ("went noisy but snapped back towards the end"), rather than cutting off during it. That makes this a cleaner test than the previous file, since the recovery is actually inside the logged window. Checked exhaustively again: `overruns=0`/`late=0` on every one of 847 samples, `max_busy_us` sits in a 45-52us band throughout (if anything slightly lower than the 43-55us range seen with the ADC presumably still enabled in earlier captures - consistent with slightly less per-tick work, though not a claim this proves the ADC's cost, just an observation), no canary hits, no `tx_freq` phantom in this window (expected - this capture doesn't span the `t=615687-769365` boot-relative window the phantom has always appeared in), and the usual `~153.6s` `CONFIRMED STUCK`/`still stuck`/`RECOVERED`/`AUTO-CAPTURED` cadence runs with completely normal timing and spacing throughout, including straight through wherever the noisy episode must have fallen. **Nothing marks it. No gap in the timestamp sequence, no timing counter step, no frequency excursion - a real, user-observed, audible event that used current instrumentation as its own alibi.**

Combined with the previous entry, this is now two independent noisy episodes, on two different capture runs, with one confirmed candidate cause (ADC) ruled out in between, and both showing the identical result: total silence from every current diagnostic. That consistency is itself useful - it makes "invisible to frequency/DSP-tick-side instrumentation" a much better-supported working assumption than a one-off null result would be, and continues to point at either the envelope/PWM/analog chain (this project's own prior framework already anticipates a path from envelope-side corruption to something that looks like RF noise while every `freq_dev_hz`-based diagnostic - which is everything built so far - stays clean) or a wake-rate/ISR-timing effect analogous to the x4-interp precedent, which none of the current `overruns`/`late`/`max_busy_us` counters were built to see.

**Not actioned.** Candidates ruled out so far for this specific noisy-mode symptom: ADC (this entry). Still not ruled out: envelope-path corruption/PWM-DAC-chain issues, and a wake-rate/ISR-timing effect distinct from per-tick busy time. Repeating the previous entry's recommendation, now with one more data point behind it: root-causing this needs either a description of the noise's own character, or new instrumentation aimed at task wake-to-wake timing rather than another frequency-side counter - two-for-two now on frequency-side diagnostics finding nothing.

## 2026-09-16, later still: rethink, prompted by the user - re-derived why the computed (energy-weighted) average and the measured (SDR/audio-tone) frequency disagree, and it points at THIS session's own new fast EMA, not at anything physical

User asked the right question after the noisy-mode trail went cold: why is the computed "avg" so different from the measured one, fundamentally? Stepping back from measurement-side theories (sidebands, ADC, SDR chain) to re-examine the computation itself turned up something that should have been checked before building any of this session's new energy-weighted code: **this exact question was already answered, rigorously, on 2026-09-09 through 2026-09-12 - for a different, older implementation of the same idea that's been sitting dormant and unused this entire session.**

`ssb_dsp.c` already contains `ssb_dsp_null_bias_stats_t`/`ssb_dsp_get_null_bias_stats()` - a lifetime accumulator, built and hardware-validated during that earlier investigation, that pairs `env2 = envelope*envelope` with `dphi` at the **exact same raw tick**, straight out of `ssb_dsp_process_sample()` - using the RAW `sqrtf(I*I+Q*Q)` envelope, computed from the identical I/Q pair `dphi` itself came from, never touched by any downstream shaping. Its "Confirmed measurement table" (this file, above) shows `weighted_bias` matching real SDR readings to within a few Hz across all 6 tested tone pairs, including the closest available comparison to today's own 700/1700 setup (700/1900: `weighted_bias=-20.72Hz`, SDR `~8Hz`). Critically, that table describes `weighted_bias` as a **stable, reproducible number for a given fixed tone pair/configuration** - not something that cycles through wildly different plateaus over time on an unchanging bench setup.

That's the key mismatch. This session's own new instrument - `diagnostics.cpp`'s `s_freq_ema_energy_num_fast`/`s_freq_ema_energy_den_fast` fast EMA, built from scratch this session without anyone (including this assistant) checking whether the underlying idea already existed and had already been characterized - uses a **different envelope** for its `env2` weight: `envelope_at_freq_time`, captured downstream in `diagnostics_set_tx_info()` AFTER the active preset's shaping chain (`envelope_ampeq_process()`, then predistort or the linear PWM offset/scale mapping). That choice was made earlier this session specifically to fix a TIME-alignment concern (matching `dev` across the `relative_delay` ring at nonzero delay) - and it does solve that problem - but nobody then asked whether shaping also changes the envelope's VALUE at exactly the tick that matters most: the null. `envelope_ampeq_process()`/`envelope_gdeq_process()` are IIR filters (allpass/shelf biquads); predistort is a 65-point LUT. Both are perfectly capable of not preserving a true, instantaneous, one-sample-wide null the way a plain index-delay does - group delay and finite-impulse smoothing can shift or soften exactly the deepest point of the true envelope in a way that decouples it, in both TIME and MAGNITUDE, from the same-tick raw envelope that `dphi`'s own null-crossing math was actually computed against. If the weight and the value it's meant to suppress are no longer correctly paired, the entire null-suppression property that makes energy-weighting work (per `ssb_dsp.h`'s own validated identity) can break down - which would produce exactly what's been observed: a fast-responding statistic that swings by tens of Hz and cycles through many different "stable" plateaus on a single, unchanging bench run, something the validated, correctly-paired original was never shown to do.

**Not yet proven - this needs bench data, and there's now a zero-guesswork way to get it.** Added a direct, same-instant cross-check: every time the new fast EMA's `'H'` detector fires `ENERGY-WEIGHTED CONFIRMED STUCK` (i.e., right when it's claiming tens of Hz of held deviation), `diagnostics.cpp` now also calls `ssb_dsp_get_null_bias_stats()` and prints the OLD, validated, raw-envelope-paired lifetime average and its own bias from `expected_center_hz`, on the very next line:

```
[dsp]   cross-check: ssb_dsp.c's own raw env2-weighted lifetime average = ...Hz
        (bias from expected_center=...Hz: ...Hz, N samples since last 'r')
        - validated-accurate reference, see null_bias_investigation.md
```

This doesn't require unmuting or any new command - it's folded directly into the existing `ENERGY-WEIGHTED CONFIRMED STUCK` print (`diagnostics_check_held_freq()`, right after the existing message, before `print_held_trace()`). The comparison is decisive either way: if the validated lifetime average is trending nowhere near the fast EMA's claimed tens-of-Hz dev, or stays small/stable while the fast EMA claims a big swing, that confirms the fast EMA's own envelope choice (not anything physical, not the SDR, not the audio counter) is the source of this whole session's central puzzle - and the fix becomes straightforward: rebuild the fast EMA against the same raw, same-tick envelope `ssb_dsp.c` already uses, rather than the shaped one. If instead the two genuinely agree at that instant, the mystery stays open and points back toward something in the shaping chain being physically real after all. Either result is progress. Brace/paren balance re-verified (156/156, 1160/1160) after the edit. Not yet bench-tested.

## 2026-09-16, later still: `log_20260916_212424.txt` - the cross-check comes back decisive on its very first capture, confirming the shaped envelope as the culprit; added a second, correctly-paired fast EMA to settle it further

First capture with the previous entry's cross-check live, and it answers the question cleanly. Two `ENERGY-WEIGHTED CONFIRMED STUCK` events, ~45s apart:

| reading | fast EMA (shaped envelope) | ssb_dsp.c's own raw-paired lifetime average |
|---|---|---|
| t=124004ms | dev=-13.76Hz -> **1056.37Hz** | 1184.87Hz (bias -15.13Hz from expected_center=1200Hz) |
| t=169738ms | dev=+69.27Hz -> **1139.40Hz** | 1188.07Hz (bias -11.93Hz) |

Over the same ~45.7s interval, the validated, raw-envelope-paired lifetime accumulator moved **3.2Hz** (1184.87 -> 1188.07, consistent with ordinary slow convergence of a lifetime average, not a real event) while this session's own shaped-envelope fast EMA moved **83.0Hz** (1056.37 -> 1139.40). **This is the decisive result the cross-check was built for**: the correctly-paired, hardware-validated reference sees nothing resembling a real change happening on this bench run; the new fast EMA reports an 83Hz swing anyway. The shaped envelope (`envelope_at_freq_time`, reshaped by `envelope_ampeq_process()`/`envelope_gdeq_process()` - both IIR filters - and either `envelope_predistort_process()`'s LUT or the linear PWM offset/scale mapping) is confirmed as the source of this whole session's "computed vs. measured" puzzle, not anything happening on the real transmitted signal.

Also worth noting purely as a numeric footnote, not a new finding: `s_freq_energy_anchor_hz` (the shaped-envelope EMA's own one-time boot anchor, `1070.13Hz`) sits **114.7Hz away** from where the validated reference has been reading all session (~1185-1188Hz) - i.e. even the "at rest" starting point this whole session's `dev=` numbers have been measured against was never a trustworthy zero, on top of the swings layered on it afterward.

**Added a second, correctly-paired fast EMA** (`s_freq_ema_rawenergy_num_fast`/`den_fast`, `diagnostics.cpp`) rather than just trusting the lifetime accumulator's slow-averaged word for it - same 50ms tau as the existing (shaped-envelope) fast EMA, so it's a fair, equally-live comparison, but weighted by `raw_envelope_current`/`raw_freq_dev_current` (`ssb_mic_test.ino`): this tick's own envelope/`freq_dev_hz` captured immediately after `ssb_dsp_process_sample()`, before `envelope_floor`/`gdeq`/`ampeq`/predistort/the PWM mapping touch `envelope` even once, and before `relative_delay_apply()` re-times either value. Two known, deliberate simplifications, not yet resolved: (1) this is PRE-delay, matching `ssb_dsp.c`'s own accumulator, rather than routed through the delay ring the way the shaped-envelope EMA is - correct at `delay=0`, an approximation at the currently-active `delay=+2.00` (a small fraction of one 1kHz two-tone beat cycle, so likely second-order next to the shaping bug just confirmed, but not proven negligible); (2) `raw_freq_dev_current` is `ssb_dsp_process_sample()`'s OUTPUT `freq_dev_hz` - already past its internal slew-limit/`max_freq_dev_hz` clamp/LSB sign-flip, unlike the true pre-clamp `dphi` the validated accumulator uses directly - likely only matters on the rare tick where that clamp actually engages.

Printed as a new `cross-check2` line alongside the existing lifetime-accumulator cross-check, at every `ENERGY-WEIGHTED CONFIRMED STUCK` (`diagnostics_check_held_freq()`). Three-way read on the next capture: if `cross-check2` ALSO stays near its own anchor while the original shaped-envelope EMA claims a big dev, that's full, live-speed confirmation the shaping is the entire story; if `cross-check2` swings too, the pre-delay/post-clamp simplifications (or something else) matter more than expected and need their own look. Either way, the honest next step once this is confirmed is to retire or clearly deprecate the shaped-envelope fast EMA (and its `'H'` energy-weighted detector) in favor of whichever comparison wins, rather than running three parallel numbers indefinitely. Brace/paren balance re-verified (`diagnostics.cpp` 159/159, 1185/1185; `diagnostics.h` 116/116; `ssb_mic_test.ino` 41/41, 468/468). Not yet bench-tested.

## 2026-09-16, later still: `log_20260916_213916.txt` - the three-way cross-check comes back nuanced, not a clean pass: shaping confirmed as the MAJORITY of the swing (~65%), but a smaller residual remains even in the correctly-paired fast EMA, most likely ordinary sampling variance rather than a second bug

First capture with `cross-check2` (the new raw-envelope-paired fast EMA) live, two `ENERGY-WEIGHTED CONFIRMED STUCK` events ~45.7s apart:

| reading | old fast EMA (shaped envelope) | validated lifetime average | new fast EMA (raw-paired, cross-check2) |
|---|---|---|---|
| t=279154ms | dev=-13.87Hz -> **1056.26Hz** | 1185.59Hz (bias -14.41Hz) | dev=+36.76Hz from its own anchor(1161.70) -> **1198.46Hz** |
| t=324860ms | dev=+69.20Hz -> **1139.33Hz** | 1181.57Hz (bias -18.43Hz) | dev=+7.45Hz -> **1169.15Hz** |
| **swing over the interval** | **83.07Hz** | 4.02Hz | **29.31Hz** |

Switching to the raw, same-tick, pre-shaping envelope cuts the swing from 83.07Hz to 29.31Hz - **a ~65% reduction**. That's a real, measured effect, not a coincidence, and confirms last entry's finding that the shaped envelope (ampeq/gdeq/predistort/PWM mapping) is the majority contributor to this whole session's spurious "computed vs. measured" gap. But it's not a clean pass: 29.31Hz is still far larger than anything the validated lifetime average, the plain fast/slow EMA, the manual SDR readings, or the audio-tone counter have ever shown for a real event this entire session (all consistently sub-25Hz at the very most, usually single digits).

**Leading explanation for the remaining ~29Hz, not yet confirmed**: ordinary sampling variance, not a second bug - and this project already has the precedent for exactly this reasoning, from the 2026-09-12 entry above resolving the earlier `weighted_bias`-vs-"the board" discrepancy: "a small-N snapshot is expected to have much higher variance than the large-N firmware average even though both estimate the same underlying mean." The validated lifetime accumulator has averaged over millions of null crossings since boot by this point in the run; a 50ms-tau fast EMA (`FREQ_EMA_FAST_ALPHA`, ~800 ticks/~50 two-tone beat cycles of effective memory) sees only a tiny fraction of that - correctly paired or not, a fast estimator of this specific, heavy-near-null-tailed quantity should be expected to have a much wider natural spread than the lifetime average, simply from having so much less to average over. The two remaining, not-yet-ruled-out caveats from last entry (pre-delay vs. the active `delay=+2.00`; post-clamp `freq_dev_hz` vs. the validated accumulator's pre-clamp `dphi`) haven't been eliminated as contributors either, but sampling variance alone is already a sufficient, well-precedented explanation and doesn't require either of them to be wrong.

**Practical implication, if this holds up**: a live, fast-responding, tight-threshold (`HELD_TRIGGER_HZ=5.0f`) energy-weighted "stuck" detector may not be a viable instrument for this quantity AT ALL, independent of which envelope it uses - if even a correctly-paired 50ms EMA has a natural few-tens-of-Hz spread, a 5Hz threshold will keep firing on pure noise no matter how the pairing bug is fixed. The two live options going forward, once shaping is fixed: (1) lengthen the EMA's tau substantially (trading response speed for the averaging depth needed to bring natural variance down near single digits - by how much is an open, computable question: `FREQ_EMA_SLOW_TAU_S=2.0f` already exists and is ~40x longer, worth trying directly), or (2) accept that a live "stuck" detector isn't the right tool for this measurement and rely on the periodic/on-demand lifetime-accumulator read (`'V'`, `print_null_bias_block()`) instead, the way the original 2026-09-09/12 investigation already used it.

**Not yet actioned - this needs one more data point before doing anything to the code.** Recommended next: a LONGER dwell on a single, unchanging bench setup (several minutes with no `'r'` reset), watching `cross-check2` alone (not `'H'`'s own claimed dev) to see whether its own spread narrows over a longer window the way sampling-variance math predicts, or whether it keeps swinging by similar amounts however long it's watched (which would argue against pure variance and put the two remaining caveats back on the table). Also worth trying, cheaply: reading `'V'`'s on-demand `null_bias2` a few times in quick succession without resetting, to get a feel for how much the LIFETIME accumulator itself moves sample-to-sample at this point in a run (it's still nonzero, per the two `cross-check` lines above, so it isn't perfectly stationary either).

## 2026-09-17: fresh `'J'` capture at the current best-tradeoff delay (+2.00) reproduces the 2026-09-12 null-crossing churn almost number-for-number, confirms a 5-day-old unconfirmed hypothesis about integer delay, and gives the still-open cross-check2 residual a concrete mechanism instead of a generic "sampling variance" label

New `'J'` dump, unprompted (not part of a specific test protocol - just checked mid-run), at `relative_delay=+2.00` - this project's own established best two-tone IMD compromise value, not an extreme or accidental setting like the 2026-09-12 captures above:

```
[dsp] jump_log: 1782960 qualifying step(s) >300Hz since last reset, 710473 near-null-blended (40%), 800590 near-null-either (45%) - ring holds the last 8
[dsp]   jump[0]: t=2950461ms 14201358->14193359Hz (step=7999Hz) env=2.055468e-01/2.055468e-01 delay=+2.00 busy_us=41 src=TWO-TONE TEST
[dsp]     jump[0] raw_freq_dev: near=-6802.0Hz far=1198.2Hz raw_delta=8000.1Hz
[dsp]   jump[1]: t=2950461ms 14193359->14201359Hz (step=8000Hz) env=1.000000e-03/1.000000e-03 NEAR_NULL delay=+2.00 busy_us=42 src=TWO-TONE TEST
[dsp]     jump[1] raw_freq_dev: near=1199.5Hz far=-6802.0Hz raw_delta=8001.4Hz
  ... (8 entries total, a locked repeating 2-state cycle: ~14201360Hz <-> ~14193359Hz)
[dsp] held_freq: ENERGY-WEIGHTED still stuck, 45s so far (t=2969308ms) - dev=+40.00Hz from anchor=1070.13Hz
```

**First finding: this is the same, already-characterized mechanism, not a new one.** The raw near/far values here (`near=-6802.0Hz far=1198.2Hz raw_delta=8000.1Hz`) are essentially identical to the 2026-09-12 capture at `relative_delay=+1.75` above (`near=-6805.3 far=1208.1 raw_delta=8013.3`, `near=-6803.0 far=1200.1 raw_delta=8003.1`) - same magnitudes, same sign pattern, same locked-cycle signature, at a completely different delay setting five days apart. This is the textbook two-tone polar-representation null artifact this file already root-caused: at every beat-cycle envelope minimum, the analytic signal's phase trajectory becomes ill-conditioned and the instantaneous frequency (`dphi`) swings by thousands of Hz over one or two ticks, even though the envelope itself is smooth and the actually-transmitted RF is fine. `raw_delta` matching `step_hz` to within 0.1-5Hz on every logged entry reconfirms (as the 2026-09-12 `-5761Hz` case first showed) that this is a genuine discontinuity already present in the raw, undelayed `dphi`/`freq_dev_hz` signal - not an interpolation artifact introduced by `relative_delay`'s ring blending.

**Second finding, new: this capture is the first direct confirmation of a 5-day-old open hypothesis about exact-integer delay.** Every single logged entry shows `env=X/X` with the blended and min envelope IDENTICAL to 6+ significant figures (e.g. `2.055468e-01/2.055468e-01`, `1.000000e-03/1.000000e-03`) - something that never happened in any prior capture (`+4.60`, `+0.90`, `+4.28`, `+1.75` all showed measurably different blended-vs-min envelope values). `relative_delay=+2.00` is an exact integer sample count, and the 2026-09-12 "later still" entry above speculated, without ever testing it, that at exactly integer delay `interp_ring()`'s interpolation weight "collapses onto a single historical ring sample with no blending from a neighbor, unlike every non-integer delay." This capture is that confirmation: `env=blended/min` collapsing to a single value at `delay=+2.00` is exactly what a 100%/0% blend weight predicts, and it's never been observed at any other delay this project has captured `'J'` data at.

**Third finding, the one that actually matters right now: roughly 55-60% of these >300Hz jumps sit at a MODERATE envelope (`0.205`-ish, `NOT near-null` by either test), not at the deep null.** Only jump[1],[3],[5],[7] (alternating, exactly half) flag `NEAR_NULL` at `env=0.001`; jump[0],[2],[4],[6] sit at `env=0.205` - four to five times the `null_bias_threshold=0.05` cutoff - and STILL show a full ~8000Hz raw discontinuity (`raw_delta` matching `step_hz` almost exactly, so this is confirmed real, not smoothed-out noise). This directly extends the still-open question the 2026-09-12 `+4.28` capture first raised (the unexplained `-5761Hz` transition at `env=0.277/0.247`, "comfortably above the 0.05 threshold") and answers it with a much larger, cleaner sample: envelope-near-null is NOT a tight enough predictor of "is this tick's phase/frequency computation ill-conditioned" for this waveform. The physical mechanism (envelope varying smoothly/slowly vs. phase varying explosively right around the beat minimum) has a genuinely wider influence zone in envelope-space than the current `0.05` classification threshold captures - it isn't only the handful of ticks where envelope reads near-zero, it's a noticeably wider window around each beat minimum where phase is still badly behaved while envelope has already recovered to a moderate, non-null-looking value.

**Why this matters for the still-open cross-check2 residual (previous entry, ~29Hz swing not eliminated by correct raw-envelope pairing):** the leading theory there was generic "ordinary sampling variance" - true as far as it goes, but this capture gives it a specific, mechanistic identity rather than leaving it as an unexplained statistical hand-wave. `cross-check2`'s fast EMA weights every tick by `raw_env2` (this tick's own raw envelope squared) - at `env=0.205`, `env2≈0.042`, not a negligible weight, and this population of moderate-envelope/huge-freq_dev-error ticks recurs at a very high, essentially deterministic rate (this capture: 1782960 qualifying events accumulated, matching the hundreds-to-low-thousands-per-second rate already established on 2026-09-12). A 50ms-tau EMA (`FREQ_EMA_FAST_ALPHA`, ~50 beat cycles of effective memory at this tone spacing) is being asked to average out a large, systematic, non-negligibly-weighted, per-beat-cycle component - not generic small-N statistical noise - and 50 beat cycles is very plausibly not enough cycles for that specific, repeating pattern to cancel out net-zero the way it does over the lifetime accumulator's millions of cycles. This reframes (doesn't contradict) the previous entry's conclusion: the residual is very likely dominated by this exact, already-characterized per-beat-null churn leaking through a too-short averaging window, rather than unstructured noise - which argues more strongly for the "lengthen tau substantially" / "drop the live detector in favor of periodic lifetime-accumulator reads" fork already proposed, since a fast detector is fighting a large, structured, high-repetition-rate signal component, not just sampling noise in the generic sense.

**One more thing this capture incidentally reconfirms, not a new concern**: the trailing `held_freq: ENERGY-WEIGHTED still stuck ... dev=+40.00Hz from anchor=1070.13Hz` line is from the OLD, already-known-untrustworthy shaped-envelope `'H'` detector (anchor `1070.13Hz` matches the exact boot anchor flagged as "never a trustworthy zero" in the `log_20260916_212424.txt` entry above) - not `cross-check2`'s own anchor (`~1161-1198Hz` range in the last capture). This +40Hz "still stuck" reading needs no fresh investigation; it's simply the same already-root-caused detector continuing to misfire on the shaped envelope, consistent with everything already established about it.

**Not actioned - analysis only, no firmware change.** Two concrete, cheap next steps this capture suggests: (1) on the next capture, also grab `cross-check`/`cross-check2` alongside `'J'` at the SAME moment, to directly correlate the jump log's near/not-near-null mix with the live EMA readings rather than reasoning about them separately; (2) if the "moderate envelope, still ill-conditioned phase" picture holds up, the `null_bias_threshold=0.05` used for near-null classification (and for the original `ssb_dsp.c` accumulator's own `near_null_dphi_sum`) may be classifying too narrowly for this specific tone spacing - worth someday checking whether a wider threshold changes the validated accumulator's own accuracy, though there's no evidence yet that it needs to (the validated `env2`-weighted accumulator doesn't rely on the near_null threshold at all, only on `env2` weighting itself, which is continuous and doesn't have this cliff-edge problem).

## 2026-09-17, later still: the long-deferred hardware test finally happens - push/pull drivers fitted on the AD9851 lines, directly targeting `AD9851.c`'s own leading theory for the two-tone "sticks" symptom; first (hands-on) impression is no change, hands-off run pending

The one hardware test flagged weeks ago and never actually done: the user has fitted push/pull drivers on the AD9851 interface lines, replacing whatever was driving them before. This is not a random change - it's a direct test of the specific, named theory already sitting in this codebase's own comments (`AD9851.c` lines ~171-191, `ad9851_edge_delay()`'s header): under the previous (BS170-based inverting level-shift) arrangement, the LOW-to-HIGH transition was a passive, pull-up-charged RC edge - much slower than the actively-driven HIGH-to-LOW direction - and that slow direction affects both a fresh DATA 0-to-1 bit AND, per the same inversion, W_CLK's AD9851-side RISING/sampling edge. That comment explicitly calls out "a large one-tick FTW jump - exactly what two-tone's near-null atan2 noise produces" as the demanding case (many DATA bits flipping at once, including many fresh 0-to-1 transitions) and names this "the leading theory for the two-tone-specific 'sticks' symptom." Faster push/pull drive on those lines is the direct, physical way to test it.

**User's own report**: edges are now measurably faster, "especially the rising" one - i.e. specifically the direction the existing theory identified as the weak one. First (hands-on, not yet rigorous) impression: no visible change to the RF output. The user also makes a specific, relevant observation of their own: "There was lots of time before between the data change and the rising clock edge previously anyway" - i.e. their own assessment is that the settle margin wasn't actually tight even before this change, ahead of and independent of today's hardware fix. Worth cross-referencing directly against `AD9851_BITBANG_EDGE_DELAY_ENABLED`, which has been `0` (no explicit settle delay inserted) since 2026-09-11 in this codebase - consistent with the user's own read, though not proof either way on its own.

**Confirmed by the user**: the new push/pull drivers preserve the same inverting logic as the BS170 arrangement they replaced - same polarity, just actively driven (faster) both directions now. `AD9851_INVERTING_LEVEL_SHIFT` stays `1` and `AD9851_BITBANG_EDGE_DELAY_ENABLED` stays `0` in this checkout, unchanged, and correctly so - no firmware/hardware mismatch to worry about when reading whatever the hands-off run turns up. No firmware change has been made or is needed for this hardware change.

**Next, already planned by the user**: a hands-off run for a clean, rigorous comparison - this is the right test, since every "sticks"/near-null-jump finding characterized so far in this file was captured with the OLD (slower-edge) hardware, and a hands-off run removes the "did I just not notice it" uncertainty inherent in the hands-on impression above. If the hands-off run comes back materially cleaner than historical baselines (rate/magnitude of `'J'`-logged near-null-adjacent jumps, `held_freq` stuck frequency, or the user's own audible/SDR observation of the "sticks" symptom), that would be the first real, positive confirmation of a theory that's been open, unconfirmed, since it was first written down. If it comes back indistinguishable from before, that's equally valuable - it would rule out edge speed/settle margin as the driver of the "sticks" symptom and redirect attention elsewhere (the phase/atan2 null-crossing computation itself, rather than anything on the SPI wire). Not actioned further - no firmware change made, awaiting the hands-off capture.

## 2026-09-17, later still: `log_20260917_112714.txt` - the push/pull-driver hands-off run comes back, and it overturns the "ordinary sampling variance" theory for cross-check2's residual: the ~29Hz swing is a near-perfectly reproducible TWO-STATE value, caused by aliasing (fixed-phase sampling of the well-documented ~153.6s cycle), not noise. Also: the hardware change itself shows no visible improvement, and a real ~50Hz audio-tone excursion appears that cross-check2 doesn't see at all.

First hands-off capture with the new push/pull AD9851 drivers fitted, `t=2152103ms` to `t=2995479ms` (~843s / ~14 minutes, no user interaction), containing 11 full `ENERGY-WEIGHTED CONFIRMED STUCK`/`RECOVERED` cycles with `cross-check`/`cross-check2` live on every one, plus the user's external audio-tone counter running throughout. Three separate findings, in order of how much they change existing conclusions.

**1) The whole STUCK/RECOVER cycle is a rock-solid, precisely repeating ~153.6-153.7s oscillation - confirmed exactly, not estimated.** Reconstructing every segment from the `started_at`/`confirmed_at`/`RECOVERED ... was stuck Xs total` timestamps: `STUCK+` (dev clustering tightly at +67.92 to +69.11Hz) always lasts 89.7-89.8s, followed by an ~18.1-18.2s "not stuck" gap, then `STUCK-` (dev clustering at -16.29 to -16.63Hz) always lasts 45.7-45.8s, then immediately (0-2ms gap) back into `STUCK+`. Sum: 89.8+18.2+45.7 ~= 153.7s, matching to within 0.1s across all 5-6 full repeats captured. This is the same `~153.6s` period that's recurred across this whole investigation's history (the `tx_freq` phantom's boot-relative window, the audio-counter plateau puzzle, the noisy-mode recovery-time coincidence) - here it's pinned down exactly, as the already-known-buggy shaped-envelope `'H'` detector's own deterministic hysteresis cycle. `relative_delay` stays at exactly `+2.00` throughout with "last changed" climbing past 645s (i.e. untouched since well before this capture) - ruled out as a variable in any of this.

**2) This overturns the previous entry's "ordinary sampling variance" explanation for cross-check2's residual swing.** `cross-check2` is only printed at each `confirmed_at` instant - i.e. at exactly ONE fixed phase point in each of the two states of the cycle just characterized. Its 11 readings split cleanly by which state they were sampled in:

| sampled during | cross-check2 (Hz) | dev from anchor |
|---|---|---|
| `STUCK+` (6x, in time order) | 1168.80, 1168.74, 1168.69, 1168.66, 1168.64, 1168.63 | +7.10 -> +6.92 |
| `STUCK-` (5x, in time order) | 1198.48, 1198.48, 1198.47, 1198.46, 1198.46 | +36.78 -> +36.76 |

Both columns are smooth, monotonically DECREASING sequences (a slow, real trend of a few hundredths of a Hz per cycle - plausibly ordinary thermal/reference drift) - the opposite signature of statistical noise, which would show no such smoothness and would narrow with a longer window, neither of which happens here. The swing between the two states, ~29.7-29.9Hz, matches the single-sample 29.31Hz figure from `log_20260916_213916.txt` almost exactly - now shown to be **near-perfectly reproducible across 11 independent events spanning 14 minutes**, not a noisy one-off. That rules out "insufficient averaging" as the explanation outright: a fast EMA suffering ordinary sampling variance would not return the same value to within 0.02Hz on 11 separate occasions minutes apart.

**The actual mechanism: aliasing, not variance.** `cross-check2` and `cross-check` only get read out at the `'H'` detector's own trigger instant, and per finding 1) that instant sits at a fixed, precisely-repeating phase of the ~153.6s cycle (always ~15s after a STUCK period starts). Sampling any periodic or quasi-periodic process at a fixed phase always returns nearly the same value on every repeat, however much it actually varies at other phases of the same cycle - this is textbook stroboscopic aliasing, and it fully accounts for the "two mysteriously stable values" without needing a second bug, without needing more averaging, and without treating the gap as unexplained noise.

**This changes the recommended next step from the previous entry.** Lengthening `cross-check2`'s own EMA tau (`FREQ_EMA_SLOW_TAU_S=2.0f` idea) will not help - the problem was never insufficient averaging depth, it's WHEN the value gets sampled. The useful next instrument is a **fixed-time-cadence** print of `cross-check`/`cross-check2` (e.g. every 5-10s) decoupled entirely from the `'H'` detector's own trigger condition, run across at least one full ~153.6s cycle - that would reveal the raw-envelope-paired EMA's actual waveform across the whole cycle instead of 2 aliased samples per cycle, and would finally show whether the true underlying quantity is smoothly oscillating, step-changing, or something else across that period.

**3) The hardware test itself: no visible improvement, a genuine (if unwelcome) negative result.** `overruns=0`/`late=0` on every sample throughout, no canary hits - as always. The shaped-EMA STUCK cycle's own signature (period, duration split, dev clustering) is indistinguishable from every previous capture on the OLD hardware - consistent with its already-established root cause (shaped-envelope/raw-envelope mismatch, 2026-09-16) being unrelated to AD9851 SPI edge timing, exactly as the user's own hands-on impression suggested. The external audio-tone counter also still shows the same-character plateau/drift behavior documented on the OLD hardware (`log_20260916_193030.txt`): values moving between roughly 0Hz and -15Hz across most segments, plus one striking new excursion (see below) - not qualitatively different from before the driver swap. Taken together, this hands-off run argues against AD9851 edge-speed/settle-margin as the dominant driver of either the shaped-EMA "sticks" signature or the audio-counter puzzle, and supports keeping attention on the phase/atan2 computation and the SDR-chain theory respectively, per where this investigation already stood.

**New observation, and now CONFIRMED real, not a counter artifact**: within the `STUCK+` segment running `t=2766810` to `2856497`, the audio-tone counter drifts to a sustained **~-50Hz** reading (`-49.2, -50.1, -47.7, -50.0, -50.1, -50.3` Hz across six consecutive readings). The user directly confirms this was a real, observed jump ("It definitely jumped to -50Hz") - i.e. this is not the audio counter's own known plateau-stepping susceptibility (leading candidate up to now: the SDR's own receive chain) producing a misleading reading; something genuinely shifted the transmitted/received frequency by ~50Hz on an unchanging two-tone bench setup. That resolves half of the previous entry's open question in one direction: candidate (b) (a counter-side artifact) is now much less likely for THIS event specifically, and candidate (a) - a real shift that's structurally invisible to every `freq_dev_hz`-based diagnostic - moves to the front.

`cross-check2`, sampled at `t=2781813` (right as the audio tone first reads -49.2Hz), shows nothing unusual at all: `1168.64Hz`, indistinguishable from its usual `STUCK+`-phase value in every other cycle in this same run. Given the jump is now confirmed real, this is a materially bigger finding than it looked before: a genuine, ~50Hz, user-confirmed frequency shift occurred and NONE of this project's phase/`freq_dev_hz`-based instrumentation (`cross-check`, `cross-check2`, the shaped-EMA `'H'` detector, the plain fast/slow EMA, `'J'`'s jump log) shows any trace of it at all - consistent with, and now the strongest evidence yet for, this project's own established theory (first raised for the "very noisy mode" symptom, 2026-09-16) that envelope/PWM/analog-chain-side corruption is structurally invisible to every diagnostic built so far, all of which watch `freq_dev_hz`/phase, not the envelope or analog chain. This may be the SAME underlying mechanism as the "very noisy mode" symptom - both are real, audible/observable, transmitted-signal problems with zero footprint in any frequency-side counter - rather than two separate unexplained things.

**Not actioned - analysis only, no firmware change.** Concrete next steps, in priority order: (1) add the fixed-time-cadence `cross-check`/`cross-check2` print described above, decoupled from `'H'`'s own trigger, to see the raw-envelope-paired EMA's true waveform across a full cycle - now doubly useful, since it would also show whether THIS confirmed real ~50Hz event has any signature at all once sampled more than twice per cycle; (2) since the frequency-side instrumentation is now confirmed blind to a real, user-observed event, the more promising direction is new instrumentation that watches the envelope/PWM/analog chain directly (per the still-open recommendation from the "very noisy mode" entries) rather than another `freq_dev_hz`-based diagnostic.

**Correction, same day**: the user directly confirms the signal during this ~50Hz event was CLEAN, not noisy/degraded - so the "same underlying mechanism as the still-unexplained 'very noisy mode' symptom" speculation above is WRONG and is retracted. A clean but wrongly-shifted signal argues against generic envelope/PWM/analog-chain corruption (which would be expected to show up as degraded signal quality, not just a clean tone at the wrong frequency) and instead points more specifically at something computing or encoding the wrong frequency cleanly - e.g. the AD9851 FTW itself being wrong, or a carrier-level issue - rather than noise/corruption downstream in the analog chain. See the next entry for a new, hands-on finding that bears directly on this.

## 2026-09-17, later still: a new, distinct jump class spotted hands-on - frequency in this "mode" can be walked back toward nominal by stepping `relative_delay`, at a measurable ~2-3Hz per coarse step, but doing so trades away IMD - strong circumstantial evidence this is the same delay-interpolation mechanism already characterized, not a fourth new bug

User's own direct, hands-on observation, distinct from (and more diagnostic than) any single log capture: there is a class of jump/state where nudging `relative_delay` with `'['`/`']'` measurably shifts the displayed/measured frequency, at roughly **2-3Hz per coarse step** (`DELAY_STEP_SAMPLES=0.05` samples, i.e. 3.125us at `SAMPLE_RATE_HZ=16000` - `relative_delay.h`). Critically, **this delay-dependence only exists while in this particular off-nominal state** - when the signal is already on-frequency (nominal), moving delay does NOT shift the measured frequency the same way. The user also makes the key distinguishing observation: moving delay away from `2.00` (Preset 3's established best-IMD compromise, `relative_delay.h`'s own doc comment / the 2026-09-06 candidate-B bench test) always shifts the IMD cancellation profile away from its optimum - true regardless of which "kind" of frequency change is being discussed - but the difference is what happens to IMD when the reading is nudged back toward nominal: the swings this whole investigation has been tracking (energy-weighted `'H'` swings, `tx_freq` phantom, jump-log churn) change the READING while the IMD profile stays reasonable; THIS newly-spotted mode requires an actual delay change away from `2.00` to pull the frequency reading back, and doing so visibly degrades IMD in the process - i.e. the "fix" here is not restoring a correct state, it's trading a frequency-reading symptom for a real, physical IMD cost.

**Why this is very likely the SAME already-characterized mechanism, not a new one - a strong, testable hypothesis, not yet confirmed.** `relative_delay_apply()`'s ring interpolation (`interp_ring()`) is a straight LINEAR blend between two adjacent raw `freq_dev_hz` ring samples; the 2026-09-17 `'J'` capture entry above already established that this blend's underlying raw samples can differ by up to ~8000Hz right at a beat-cycle null (giving `NEAR_NULL`/`NEAR_NULL(either)` jumps), but ALSO that ~55-60% of qualifying jumps sit at a moderate, "not near null" envelope where the raw discontinuity is still real, just smaller. A **2-3Hz shift per 0.05-sample step** implies a local raw near/far `freq_dev_hz` difference of roughly `(2-3Hz / 0.05 samples) ~= 40-60Hz per full sample` at whatever point in the cycle this "mode" is sampling from - two-to-three orders of magnitude smaller than a full null-crossing discontinuity, but still a genuine, non-zero local slope in the raw signal. That fits this description exactly: a moment where the raw signal is changing modestly from tick to tick (not flat, but nowhere near a null), so blending across it with `interp_ring()` produces a small, delay-proportional shift in the reported/measured frequency - resembling a "walkable" linear knob rather than the abrupt null-crossing cliff. "On frequency" (nominal) would then correspond to sitting in a genuinely flat part of the cycle where near and far raw samples already agree, so blending has nothing to shift - matching the user's own observation that the delay-dependence disappears once back on nominal.

This also cleanly explains the IMD cost: `relative_delay=2.00-2.10` is independently established (2026-09-06 candidate-B bench test) as the true physical optimum for envelope/phase alignment, chosen for IMD reasons that have nothing to do with this diagnostic-reading effect. Moving delay off that point to chase a frequency READING (which per the paragraph above is itself just an interpolation artifact at that instant, not a real error) necessarily gives up real IMD performance for no real correction - the frequency reading looks better, but nothing about the underlying signal actually changed for the better, which is exactly the asymmetry the user is describing.

**Not yet confirmed - needs the same instrumentation already available, aimed at this specific state.** The next time this "mode" is caught live: (1) dump `'J'` before touching delay at all, to see whether the ring's `raw_freq_dev near/far` values at that moment show a `raw_delta` in the 40-60Hz-per-0.05-sample range predicted above (scaled to whatever delay step is actually used) rather than the ~8000Hz null-crossing magnitude; (2) step delay with `'['`/`']'`/`'`'`/`;` while watching `'J'` and/or `cross-check2` to confirm the shift is linear and matches the predicted per-step Hz; (3) if this holds up quantitatively, this "mode" is not a fourth new bug at all - it's a hands-on-visible symptom of the same delay-interpolation-sensitivity mechanism this file has been characterizing since 2026-09-12 (the `env=blended/min` work, the `near_null_either` fix, and this file's own 2026-09-17 `'J'` entry above), just observed by ear/SDR instead of via a log capture, and the practical takeaway is unchanged from all of that prior work: `relative_delay` should be chosen for IMD, per the established 2.00-2.10 bench optimum, never nudged to chase a frequency reading that this same delay knob can trivially fake in either direction.

**Correction/sharpening, same day, prompted by a direct user challenge**: the phrase "whatever point in the cycle this mode sits" above was imprecise, and the user correctly pushed back on it - the 700/1700Hz test tones are exactly locked to `SAMPLE_RATE_HZ=16000` (`gcd(700,1700)=100Hz`, so the whole two-tone waveform repeats exactly every 160 samples/10ms, the beat envelope every 1ms), and the ADC is disabled, so there is no real-world/audio-input jitter anywhere in the loop. Given that, `ssb_dsp_process_sample()`'s raw `dphi`/`freq_dev_hz` computation is a purely deterministic function of an exactly-periodic input and MUST itself repeat bit-for-bit every 10ms, forever - it has no business containing anything at the ~153.6s macro-cycle timescale on its own.

Two separate "cycles" were being conflated: (a) the FAST, exactly-periodic 1ms/10ms two-tone beat cycle - fully locked, zero ambiguity, matches the user's own reasoning exactly; (b) the SLOW ~153.6s stuck/recovered macro cycle pinned down exactly earlier today - also fully deterministic (not random), but NOT explainable from (a) alone, since a purely periodic, memoryless raw computation cannot produce a new, much slower period on its own. That means the ~153.6s cycle has to originate downstream of the raw DSP math - in the shaped-envelope IIR filters (`envelope_ampeq_process()`/`envelope_gdeq_process()`, both of which DO have memory/state across many two-tone cycles) or in the `'H'` detector's own EMA/hysteresis logic - not in anything upstream of that. This is a sharper, better-scoped version of the project's long-standing open "why does ~153.6s exist at all" question, now narrowed to a specific pair of places to look, rather than "somewhere in the signal."

For the delay-walk finding specifically, this re-points the explanation at the FAST cycle (a), not the slow one (b): since a human keypress isn't synchronized to a 1ms/10ms clock either, each attempt at the delay-walk test lands at an effectively arbitrary phase of the exactly-repeating beat cycle - sometimes near a null (steep local raw slope, walkable per the arithmetic above) and sometimes not (flat, not walkable) - purely from keypress timing, with no real randomness in the hardware or signal. This is a falsifiable refinement: if correct, triggering the delay-walk test at a KNOWN, controlled phase offset relative to the two-tone generator (rather than an arbitrary keypress) should make the "walkable" state fully predictable rather than something that has to be caught by chance - a cleaner test than the plain `'J'`-while-stepping-delay one proposed above, if it can be arranged.

**Not actioned - analysis only, no firmware change.** Two hypotheses now pending confirmation: the original `'J'`/`cross-check2`-while-stepping-delay test, and this refined phase-controlled version of it.

## 2026-09-17, later still: user's rigorous LTI challenge lands on a real bug - the two-tone generator's own naive float32 phase accumulator - confirmed by simulation to cause genuine, if very slow, frequency error; but the measured drift rate is ~16x too slow to be THE ~153.6s mechanism, and an initial "match" turned out to be a self-caught measurement artifact

User's direct, precise challenge to the previous entry: the 700/1700Hz two-tone generator, the Hilbert FIR, and the shaping IIR filters are all deterministic LTI stages fed an exactly-periodic (160-sample/10ms) input, with the ADC disabled - so where can any "variability" possibly come from? Correct as a matter of pure LTI-systems theory: an exact implementation of this chain has zero business containing anything at the ~153.6s macro-cycle timescale.

Checked the actual generator code rather than continue reasoning abstractly (`test_signals.cpp` lines 19-20, 198-201):
```
s_tone1_phase += two_pi * s_tone1_hz / (float)SAMPLE_RATE_HZ;
if (s_tone1_phase > two_pi) s_tone1_phase -= two_pi;
```
This is a naive floating-point NCO: phase accumulated via repeated `float` (32-bit) addition, wrapped by SUBTRACTING `two_pi` rather than an exact modulo of an integer sample count. Every addition carries a tiny rounding error, and because errors accumulate onto a running state (rather than being recomputed fresh from an exact sample index each tick), they don't have to cancel - a well-known real failure mode of this style of oscillator, and a genuine answer to "where does non-ideal behavior enter a supposedly-exact chain": determinism does not imply zero rounding error, and rounding error in an accumulator CAN drift in one consistent direction rather than average out.

**Simulated the exact recurrence to check whether this matters, and caught a mistake in my own first attempt before trusting it.** First pass (counting total phase wraps over an arbitrary N-tick window) appeared to show tone2 (1700Hz) running ~6.15mHz slow, implying an almost-suspiciously-good ~162.6s beat-precession period. Didn't trust a coincidence that clean - re-ran it properly by tracking the TRUE unwrapped phase against an exact double-precision reference at fixed sample offsets across an 8,000,000-tick (500s) run, rather than counting wraps over one arbitrary window (which turned out to have its own +/-1-wrap boundary quantization artifact large enough to produce that first, spurious "match" on its own - confirmed by re-running the same flawed method on float64, which should be exact, and seeing the identical-sized fake "error"). The corrected method shows a real, LINEAR (not bounded/noisy) drift: `700Hz` tone runs `+1.79e-4 Hz` fast, `1700Hz` tone runs `-2.00e-4 Hz` slow, both confirmed via direct phase-vs-ideal comparison at start/mid/end of the run, not just two endpoints. That makes the true beat frequency (nominally `1000.000000Hz`) off by about `-3.8e-4 Hz`, implying the real null/beat pattern precesses relative to an idealized locked reference with a period of roughly **2500s (~41-44 minutes)** - real, and worth fixing, but **~16x too slow** to be the ~153.6s mechanism this investigation has been chasing.

**Conclusion, stated as precisely as the evidence supports**: this identifies a genuine, validated (by simulation, self-corrected once) floating-point rounding bug in the two-tone generator's phase accumulator - the "700Hz"/"1700Hz" test tones are not bit-exact over long dwells, and the fix is well-understood and low-risk (recompute phase from an exact integer sample count each tick via `fmodf`, or use a fixed-point/integer phase accumulator with exact modulo wraparound, the standard technique specifically designed to avoid this class of drift - not yet implemented, no firmware change made). But this specific mechanism does NOT quantitatively explain the exact, precisely-repeating ~153.6s macro cycle - that conclusion from the previous entry stands: the ~153.6s period most likely still originates downstream, in the shaped-envelope IIR filters (`envelope_ampeq_process()`/`envelope_gdeq_process()`) or the `'H'` detector's own hysteresis logic, neither of which this session has directly tested for a slow, self-sustaining oscillation mechanism yet. Also newly flagged as a candidate, not yet ruled out: the ESP32's REAL hardware timer tick rate may not be exactly `16000.000000Hz` the way every calculation in this project assumes - a genuine hardware clock/timer accuracy question, distinct from the floating-point rounding bug just found, and also capable of producing a slow beat between the "intended" and "actual" sample rate.

**Not actioned - analysis only (simulation), no firmware change.** Concrete next steps: (1) if the generator fix above is wanted, it's cheap and independently worth doing regardless of the 153.6s question; (2) to actually find the 153.6s mechanism, look downstream at the shaped-envelope IIR filters' and `'H'` detector's own dynamics for a genuine slow oscillation/limit-cycle source, rather than the raw signal generation; (3) consider whether the ESP32's actual timer-driven sample rate could be measured directly (e.g. against a reference clock) to rule in/out real hardware clock error as a contributor.

## 2026-09-17, later still: does the accumulated NCO error matter more AT the null? Yes - a refined calculation lands within ~7% of the observed ~153.6s cycle, and the user's own follow-up insight (a fully-locked, ADC-free, deterministic chain should reproduce every two-tone cycle bit-for-bit identically) directly names the correct fix, now implemented and verified by simulation

Direct follow-up to the previous entry's "the drift is real but ~16x too slow to matter" conclusion: the user asked the sharper question of whether the SAME small drift matters far more specifically AT the null, given this whole investigation has repeatedly shown null-crossing phase/frequency computation to be extremely sensitive to tiny perturbations (the `atan2`-near-zero-envelope ill-conditioning this file has documented since 2026-09-09). It does, and the mechanism is a genuine, calculable amplification, not hand-waving:

The previous entry's beat-frequency drift rate translates to a full 2*pi precession of the tone1/tone2 relative phase - i.e. the true null's absolute TIMING relative to an ideal locked reference - over ~2639s. But `SAMPLE_RATE_HZ / |f2-f1| = 16000/1000 = 16` samples EXACTLY per beat cycle - an integer. The quantity that actually matters for how severe a given pass through the null looks isn't the full beat-phase drift, it's how long it takes that drift to shift the null's timing by just ONE of those 16 discrete sample-slots - i.e. `2639s / 16 ~= 164.9s`. Compared against the precisely-measured ~153.6-153.7s macro cycle, that's a ~7% miss (`153.65/164.9 ~= 0.93`) - not exact, but a far closer, more mechanistically-motivated match than anything found so far, and well within what a crude 8M-tick float32-on-x86 simulation (vs. the ESP32's actual Xtensa FPU/compiler behavior) could reasonably be expected to nail precisely. This reframes the earlier "too slow by 16x" conclusion: it isn't too slow - 16x was exactly the discrete-sampling granularity (16 samples/beat-cycle) that turns the slow, smooth 2639s beat-phase drift into a much more consequential, ~165s-scale "which discrete sample is closest to the true null" cycle, which is precisely the quantity downstream near-null-sensitive computations (the shaped-EMA weighting, the `'H'` detector, `'J'`'s classification) actually see. Not yet confirmed against the real downstream Hilbert/`ssb_dsp_process_sample()` computation directly (only reasoned about via the sample-alignment argument above) - a genuinely stronger candidate than before, but still not proven to be THE full explanation.

**User's own follow-up, and the correct fix it names directly**: "we know how many nulls will be encountered in the two tone cycle so must be able to ensure a net null result so every cycle is identical." Exactly right, and it names the fix precisely: given a fully-locked, deterministic, ADC-free chain, the two-tone generator should reproduce every repeat of its own cycle bit-for-bit identically, not merely "slowly drift" - the old accumulate-and-subtract phase generator (`test_signals.cpp`) violates that by construction (it never resets its own rounding error), so the fix isn't to slow the drift down, it's to make every cycle EXACTLY identical by design.

**Implemented and verified by simulation** (`test_signals.cpp`, `generate_twotone_sample()`): replaced the accumulator with a wrapping integer sample index (`s_tone_sample_index`, wraps at exactly `SAMPLE_RATE_HZ`) and recompute phase FRESH each tick via `fmodf((float)index * hz / SAMPLE_RATE_HZ, 1.0f) * two_pi` for tone1 always, and for tone2 whenever the (off-by-default, still-experimental) dither is not active. Because the index is always an exact integer well under float32's exact-integer range (2^24) and one full second is exactly `f` whole cycles for any integer-Hz tone, this carries NO accumulated error at all, by construction - confirmed by re-running the same simulation methodology against the new scheme: **0.0 deviation, exactly**, at every one of 500 one-second boundaries across an 8-million-tick run (vs. the old scheme's confirmed, steadily-growing drift). Tone2 under dither keeps the original accumulator (dither's instantaneous frequency is deliberately time-varying, so a fixed-frequency exact recompute doesn't apply there) - toggling `'Q'` or changing band (`'T'`) can still cause one small, one-time phase step exactly at that transition, accepted deliberately and consistent with this codebase's existing precedent (`test_signals_set_twotone_dither_enabled()`'s own abrupt reset already does this). Brace/paren balance verified (40/40, 167/167) after the edit. Not yet bench-tested (no toolchain in this environment, per this project's standing caveat).

**Not actioned further beyond this fix**: whether it actually changes the real ~153.6s cycle on hardware is the decisive test still pending - if the cycle shrinks, disappears, or becomes unpredictable/non-periodic once every two-tone repeat is bit-exact, that's strong confirmation this WAS (at least partly) the mechanism; if the ~153.6s cycle persists completely unchanged, the downstream shaped-envelope IIR filters or `'H'` detector hysteresis (flagged in the previous entry) remain the better candidate instead.

## 2026-09-17, later still: `log_20260917_122337.txt` - first capture with the NCO fix flashed. The macro cycle did NOT disappear, but its period changed dramatically (~153.6s -> ~8.15s, ~19x faster); the user's separately-reported "twice a second changing tone cycle" is NOT confirmed or explained by anything in this file

First real hardware data since the `test_signals.cpp` phase-generator fix (previous entry) was flashed. Same test setup (700/1700 two-tone, per the `anchor`/`slow_trace` values matching that band's already-familiar artifact-value family - 400/800/1200/1600Hz-class jumps, not a different band).

**Major finding: the `slow_trace: AUTO-CAPTURED` event's own period changed drastically.** All 55 timestamps in this file were extracted and diffed: after the one reboot-reset discontinuity (a large negative diff where `t` restarts near 0, expected from reflashing), the remaining 53 consecutive intervals are **8148.5ms +/- 3.6ms** (min 8141, max 8158) - a remarkably tight, clock-like period. That is NOT the ~153.6-153.7s macro cycle exactly, precisely, and repeatedly characterized across every capture on the OLD (pre-fix) firmware this whole session - it's about **19x faster**. The cycle did not disappear, and this is not "the fix didn't work" - it's a real, substantial CHANGE in the mechanism's own period, on what looks like the same test configuration otherwise. This is strong, if indirect, evidence that the old NCO drift genuinely was feeding into whatever triggers this hysteresis (removing that drift source measurably changed the trigger rate), while confirming the hysteresis MECHANISM itself (most likely still the shaped-envelope IIR filters or the `'H'`-style detector logic, per the previous entry's reasoning) was not itself in the tone generator - fixing the generator changed the mechanism's operating point/statistics, not its existence.

**Not yet fully explained**: why the fix would change the period AT ALL, if (per the earlier LTI-systems reasoning) the hysteresis genuinely lives entirely downstream of an now-exactly-periodic input. Leading candidate: the old drift's effect wasn't to CAUSE the cycle outright, but to modulate how often/how severely the near-null churn produced excursions large enough to cross `SLOW_JUMP_TRIGGER_HZ`/`HELD_TRIGGER_HZ` - removing the drift changes that excitation statistic, which can shift an emergent threshold-crossing period substantially even though the threshold logic itself is unchanged and lives downstream. Not proven - the alternative (something about the fix itself introduced a new, faster artifact) hasn't been ruled out either.

**The user's separately-reported symptom - "a regular freq shift about twice a second," clarified as "a changing tone cycle" (character/timbre, not necessarily a discrete jump) - is NOT confirmed, explained, or ruled out by anything in this file.** This log's only fine-grained (per-tick) data is a ~17ms burst near the start (far too short to show a ~500ms-scale repeating pattern), and no other timestamped structure in the file lands anywhere near a ~500ms/2Hz period (the `slow_trace` bins show an unrelated, much faster ~5ms 4-state churn specific to the near-null artifact family; the macro cycle above is ~8.15s, much slower). This remains a genuinely open, separate question.

**Not actioned - analysis only, no firmware change this turn.** Concrete next steps: (1) for a clean before/after comparison of the macro-cycle-period finding, repeat on the exact same band/config as an existing OLD-firmware baseline capture, ideally without any other changes, to isolate the fix's effect precisely; (2) for the "twice a second" question, a continuous multi-second capture (not a brief unmuted burst) is needed, or a marker press at the exact instant the character change is heard, so the log has something to search around.

## 2026-09-17, later still: `ToneWarble1.wav` - the "twice a second changing tone cycle" is captured directly on audio and turns out to repeat at almost exactly 1.000s, not 2Hz; the leading candidate is `diagnostics_service()`'s own 1000ms-gated Serial print block on Core 1, a mechanism this project has already partially characterized as capable of disturbing dsp_task on Core 0

User uploaded a 4.93s, 96kHz mono WAV recording made specifically to capture the "changing tone cycle" symptom described earlier today. Analyzed with a short-time FFT (4096-point, ~23.4Hz bins, ~2.7ms hop) tracking the dominant spectral peak frame-by-frame, then grouped into runs of consecutive frames sharing the same peak bin ("plateaus").

**The pattern is real, present throughout the whole clip, and startlingly periodic - just not at 2Hz.** Each cycle: a long (~430ms) dwell at one frequency, a shorter (~265ms) dwell at a second frequency roughly one FFT bin higher, then a busier ~300ms tail of short (5-90ms) flickers among that pair and a third nearby bin, before resetting to the long dwell again. The four consecutive "long dwell start" timestamps found in this clip are 594.7ms, 1594.7ms, 2589.3ms, and 3586.7ms - inter-cycle gaps of 1000.0ms, 994.6ms, 997.4ms, 997.3ms (mean ~997.3ms, spread under 3ms). That is essentially exactly **1.000 second**, not the "twice a second" the user described by ear. The likely reconciliation: each cycle visits two clearly distinct, salient tone-character states (the long dwell and the medium dwell), so a listener counting "how many times did the tone's character change" per second would reasonably hear that as ~2 events/sec even though the underlying repeating unit is 1Hz, not 2Hz.

Caveat on the specific Hz values: the plateau frequencies found (three adjacent bins, ~23.4Hz apart) are quantization artifacts of this particular FFT window size, not necessarily real discrete "steps" in the underlying signal - a separate Hilbert-transform-based instantaneous-frequency check (bandpass-filtered around the dominant cluster) showed continuous, noisy wander rather than clean flat plateaus. The **timing** of the plateau-switching pattern (the ~1.000s period) does not depend on this and is the solid part of this finding; the exact Hz "levels" are not.

**A suspiciously exact 1Hz period from software is a strong, specific clue - grepped the whole codebase for anything unconditionally periodic at ~1000ms.** Two candidates found, both gated `now - last_*_ms >= 1000`:
- `envelope_output.cpp`'s `mcp4725_fast_write()` error log - ruled out: it only fires on an actual I2C write failure to the envelope DAC (throttled purely to avoid flooding the log if the bus is broken), not an always-present event. No corroborating evidence of DAC bus failures elsewhere in this session's logs.
- `diagnostics_service()` (`diagnostics.cpp:2536`, Core 1, called once per `loop()` iteration) - `if (!s_diag_muted && now - last_timing_print_ms >= 1000) { ...; print_timing_and_adc_block(now); ...; print_null_bias_block(); }`. This is the standout candidate: an **unconditional** (whenever diagnostics aren't muted) burst of several `Serial.printf()` calls, firing exactly once per second, every second, for as long as the unit runs - a near-perfect match for a steady, always-present 1Hz event across an entire 5-second clip.

This isn't a new mechanism being invented from nothing: `diagnostics.cpp`'s own header comments (near `s_core1_busy_diag_us`, `s_dbg_max_diag_block_us`) already document that this exact print block's wall-clock cost is measured and watched, and a much earlier "Fs jitter hunt" already found that when `dsp_task` (Core 0, the phase-critical path) is blocked, the gptimer notify-from-ISR call that wakes it stretches from ~1us to ~6us - the cost of the cross-core IPI needed to wake a Core-0-pinned task from a Core-1 ISR. `dsp_task` and `diagnostics_service()` are on different cores by design specifically to avoid this kind of interference, but the fact that this exact class of cross-core cost has already been measured and named in this codebase makes a once-a-second Core-1 print burst a well-motivated, not speculative, candidate for a once-a-second disturbance visible on Core 0's output.

**Open confound, needs the user to check**: was Serial/diagnostics output actually unmuted (`'v'`) while this WAV was recorded? If it was muted, `diagnostics_service()`'s whole timing/adc/null-bias block (and `print_status_line()`, gated the same way at 45ms) never fires at all, and this candidate is eliminated - a different, still-unidentified 1Hz-periodic mechanism would be needed instead. Cheap, decisive test: record two clips back-to-back under identical conditions except toggling `'v'` (diagnostics muted vs unmuted) and see whether the ~1.000s tone-cycle pattern is present in both, only the unmuted one, or neither.

**Not actioned - analysis only, no firmware change.** Next steps: (1) the mute-state check above; (2) if confirmed present only when unmuted, this becomes a strong, cheaply-fixable lead (deeper mute-gating or moving this print work off Core 1's tight loop) for a real, audible, on-air artifact, distinct from every internal-detector mechanism (`'H'`, `'J'`, `null_bias`, cross-check/cross-check2) chased so far this session, since none of those were shown to change TX behavior every second - this would be the first candidate that's actually shown to correlate with something audible in real time; (3) if it persists muted, go back to grepping for other unconditional ~1000ms-period code paths not yet found (e.g. FreeRTOS default tick-based housekeeping, WiFi/BT coexistence timers if either radio is active, or a filesystem/NVS flush).

## 2026-09-17, later still: the `diagnostics_service()` hypothesis for `ToneWarble1.wav`'s ~1.000s cycle is decisively ruled out - user confirms `'v'` was already off during the recording, and switching it on makes no audible difference; an exhaustive re-grep finds no other unconditional ~1Hz software timer anywhere in this codebase, redirecting suspicion toward the monitoring/recording chain rather than the firmware itself

Direct test result from the user: diagnostics were already muted (`'v'` off) when `ToneWarble1.wav` was recorded, and toggling `'v'` on makes no audible difference to the warble. This cleanly eliminates the previous entry's leading candidate (`diagnostics_service()`'s 1000ms-gated `print_timing_and_adc_block()`/`print_null_bias_block()` burst) - that whole block, and `print_status_line()` alongside it, sit behind the exact same `!s_diag_muted` gate, so with `'v'` off neither ever runs at all, muted or not; the warble being present (and unchanged) in both states means it cannot depend on that gate.

**Re-grepped the entire codebase for any OTHER unconditional ~1Hz mechanism, specifically excluding anything gated on `s_diag_muted`, and found none.** Checked and ruled out: no `esp_timer_create`/`xTimerCreate`/periodic hardware timer anywhere outside the main `gptimer` (which runs at `SAMPLE_RATE_HZ`, not 1Hz); no WiFi/Bluetooth/OTA code present at all (so no beacon/scan/coexistence timer either); `loop()` itself (`ssb_mic_test.ino:1100`) is just `handle_serial_commands()` -> `adc_capture_service()` -> `diagnostics_service()` -> `delay(10)`, no loop-iteration counter or modulo pattern that could produce a ~100-iteration (~1s) cadence independent of `diagnostics_service()`'s own (mute-gated) internal timer; no preset auto-cycle/dwell-stepping timer exists anywhere (`settings.h`'s presets are static, selected by keypress only, never auto-advanced). `canary_check_background()` and the slow-trace auto-dump are the only mute-EXEMPT background checks that run every `loop()` iteration, but both are pure state-transition checks with no periodic action of their own - they'd only produce visible output on an actual fault/latch event, not a steady 1Hz drumbeat.

**This is a genuinely exhausted search of the firmware for this specific signature.** Combined with the direct mute-state test, the balance of evidence now points AWAY from this project's own ESP32 code as the source of the ~1.000s cycle, and toward something in the monitoring/recording chain instead - e.g. a receiver's AGC time constant, an SDR application's own AGC/spectrum-averaging/waterfall update rate, USB audio interface buffering, or some other property of however this WAV was actually captured (direct line-out vs. off-speaker mic, what radio/software was doing the receiving). None of this has been asked or established yet - it's the natural next question, since the firmware-side search has come up empty.

**Not actioned - analysis only, no firmware change.** Open next step: get details on exactly how `ToneWarble1.wav` was captured (what's on the receiving end - a real receiver, an SDR, straight audio loopback - and whether IT has any AGC/averaging setting anywhere near 1Hz) before spending more time searching the TX-side firmware further for this particular symptom.

## 2026-09-17, later still: recording-chain details narrow this sharply - it's Audacity capturing SDR audio output, the ~1.000s modulation is visible directly on the SDR's own spectrum display as baseline-noise pumping (not just in the demodulated audio), and critically it's absent on sine/AM test signals and only appears on two-tone - the leading theory shifts to the SDR receiver's own AGC reacting to two-tone's high peak-to-average ratio, not a TX-side bug

Two new facts from the user, both important. (1) Capture chain: Audacity recording the SDR's audio output - so there IS SDR software/hardware in the signal path between the TX and the WAV file, as suspected in the previous entry. (2) The ~1.000s pattern is "clearly visible on the spectrum displays as modulating baseline noise" - i.e. it shows up on the SDR's own RF-domain spectrum/waterfall, not only in the demodulated audio Audacity recorded. That rules out an Audacity- or USB-audio-buffering-specific artifact (those could only affect the audio stream, not an independently-rendered RF spectrum display) and points at something upstream of the audio path entirely - either the actual transmitted signal, or the SDR receiver's own gain/AGC processing of it.

**The decisive new clue: sine tone and AM test signals are clean; only two-tone shows this.** This is a strong discriminator. A pure sine tone has a perfectly constant envelope (zero peak-to-average ratio beyond the carrier itself); a gentle AM test signal has a modest, smooth envelope variation. A two-tone signal, by contrast, has a MUCH higher peak-to-average ratio and genuine amplitude nulls where the two tones cancel - a well-known general property of any two-tone test, independent of anything specific to this project's DSP chain. Receiver AGC circuits are known to behave very differently on signals with high crest factor / real envelope nulls vs. a constant-envelope carrier - "AGC pumping" (audible/visible gain-recovery oscillation triggered by a signal repeatedly diving toward the noise floor and recovering) is a well-documented general SDR/receiver phenomenon on exactly this class of signal, and its oscillation rate is set largely by the AGC loop's OWN attack/decay time constants, not by the precise timing of whatever is driving it - which would also explain why the measured ~1.000s period is so tight (SD 2-3ms): a control loop's own self-timed relaxation oscillation is often MORE regular than the aperiodic disturbance driving it, not less.

This reframes the whole search: rather than needing a new, still-undiscovered ~1Hz mechanism somewhere in the ESP32 firmware (a search that came up genuinely empty in the previous entry), the existing, already-well-documented two-tone envelope irregularities this investigation has spent weeks characterizing (near-null jumps, "sticks", the various STUCK/RECOVERED cycles) may be sufficient on their own to trigger this - through the RECEIVER's AGC responding to them - without requiring any new firmware bug at all. Sine/AM being clean is consistent either way: those signals have no comparable envelope excursions for an AGC to react to in the first place, whether or not anything is currently wrong with the TX chain's handling of them.

**Cheap, decisive next test, entirely on the receive side, no firmware involved**: switch the SDR to manual/fixed gain (AGC off), or to a distinctly different AGC speed setting, and see whether the ~1.000s baseline-noise pumping changes or disappears. If it goes away with AGC off, that's conclusive - the mechanism lives in the receiver, not the transmitter, and this specific symptom drops out of the firmware investigation entirely (though the underlying two-tone envelope irregularities it was reacting to remain worth continuing to chase for their own, already-documented reasons). If it persists with AGC off/fixed, that would be a genuine surprise and would reopen the TX-side search with a much stronger constraint (something producing an exactly ~1Hz-periodic RF-level effect that the AGC search doesn't explain).

**Not actioned - analysis only, no firmware change.** Awaiting the AGC-off/fixed-gain test result before deciding whether this thread stays open on the TX side at all.

## 2026-09-17, later still: user firmly disagrees with the AGC theory (already checked it directly) and is confident this is coming from the ESP itself - retracting the AGC hypothesis, and per the user's direct request, making a temporary causal-test change to the one concrete "~1 second internal loop" this codebase actually has

The user has already checked the SDR's AGC directly and is confident the ~1.000s cycle originates in the ESP32, not the receiver. Retracting the previous entry's AGC-pumping theory - it was a reasonable inference from the available facts at the time, but direct hands-on verification on the actual hardware overrides that reasoning, and it shouldn't have been argued past that point.

Before making a change, checked one more thing the AGC theory had left unexamined: does `diagnostics.cpp`'s fast/slow EMA detector (`FREQ_EMA_SLOW_TAU_S = 2.0f`, the pair used for the `'H'`/held-frequency detector and `cross-check`) ever write back into the live signal path? Confirmed **no** - every write from this module (`s_dbg_tx_freq`, `s_held_last_tx_freq`, `s_dbg_prev_tx_freq`, etc.) goes to diagnostic-only statics, never back into `dsp_state`/`ssb_dsp`'s own `freq_dev_hz`/`tx_freq` computation. This module only ever *watches* `tx_freq` (documented elsewhere as "ground truth," written by `dsp_task`) and reports on it - it has no path to influence the actual transmitted signal, so changing its 2s tau could not move anything audible even if that constant is otherwise interesting (its magnitude being close to the ~1s finding may well be coincidental, or a genuine clue about the detector's own sensitivity to a real upstream cycle, but not itself a lever on the real signal).

**The one thing in the whole codebase that IS both unconditional (when unmuted) and concretely "~1 second" is `diagnostics_service()`'s own print-block gate** (`diagnostics.cpp`, `now - last_timing_print_ms >= 1000`) - the same one implicated and then seemingly ruled out by the mute on/off test. But mute on/off is a coarser test than directly varying the period itself: with `'v'` off the whole block is skipped, so that test proved the block's *presence/absence* doesn't matter, not that its *period* doesn't matter (e.g. if some other coupling - electrical, cross-core, or otherwise - keys off exactly WHEN the block would have fired rather than off its Serial content). Per the user's direct request, made a temporary, clearly-marked causal test: retuned this gate from **1000ms to 1700ms** (a value sharing no simple multiple with 1000, chosen so a resulting shift in the audible/spectrum-visible cycle to ~1.7s would be unambiguous rather than confusable with measurement rounding).

**Important caveat carried into the test itself**: this change only has any effect at all with diagnostics **unmuted** (`'v'` on) - the block is still skipped entirely when muted, unchanged from before. Since the original `ToneWarble1.wav` was recorded muted, the next comparison recording needs `'v'` ON to actually exercise the changed code path - a genuinely different condition from anything tested so far, not a repeat of the earlier mute toggle.

**Not yet bench-tested** (no toolchain in this environment, as with every other firmware change this session) - this is the version to flash and re-record against. If the ~1.000s cycle shifts to ~1.7s with `'v'` on and this build flashed, that's a clean, direct proof this print block (or something that keys off its exact timing) is the mechanism after all - reopening the question of HOW, since the earlier crosscore-IPI/blocking reasoning was framed around dsp_task being blocked, not an exact injected period. If the cycle stays at ~1.000s regardless, that decisively rules this specific code path out (this time via direct parameter variation, not just presence/absence) and the search moves elsewhere in the firmware - the two-tone-only, envelope-null-adjacent DSP path (test_signals.cpp, the Hilbert/atan2 chain, envelope_gdeq.h's shaping filters) becomes the next place to look, since sine/AM being clean still implicates something specifically tied to two-tone's envelope nulls, wherever it turns out to live.

**Revert reminder**: `1700` has no reason to stay once this test's result is known - the surrounding `print_timing_and_adc_block()` rate-math (elapsed_ms-based SPS/TPS calculations) assumes roughly 1-second windows for its own reasoning, so this should go back to `1000` promptly either way.

## 2026-09-17, later still: the 1700ms causal test comes back negative too (`'v'` on/off still makes no difference) - `diagnostics_service()` is now conclusively cleared by TWO independent tests. User asks directly for 700/1700's interference periodicities; the math turns up a striking coincidence (every preset in this codebase, not just 700/1700, has its two-tone waveform repeat exactly 100 times per second) which was rigorously checked against - and clears - today's own NCO-fix as the cause

**`diagnostics_service()` is now definitively ruled out**, via a second, independent test: with the print gate retuned to 1700ms, `'v'` on/off again makes no audible difference (same result as the original mute-toggle test on the unmodified 1000ms build). Combined with the earlier read-only-detector finding (fast/slow EMA never writes back to the live signal) and this direct period-variation test, there is no remaining path by which anything in `diagnostics.cpp` could be responsible. Revert `1700` back to `1000` next.

**The user's direct question - what interference periodicities does a 700Hz/1700Hz two-tone pair actually produce - deserves a full, precise answer:**

| Product | Frequency | Period |
|---|---|---|
| Difference (envelope beat) | f2−f1 = 1000Hz | 1ms (envelope nulls every 0.5ms - `\|cos\|` doubles the rate) |
| Sum | f2+f1 = 2400Hz | ~0.417ms |
| 3rd-order IMD | 2f1−f2 = −300Hz (300Hz), 2f2−f1 = 2700Hz | 3.33ms, ~0.37ms |
| 5th-order IMD | 3f1−2f2 = −1300Hz (1300Hz), 3f2−2f1 = 3700Hz | ~0.77ms, ~0.27ms |
| Tone 1 alone | 700Hz | 1.4286ms |
| Tone 2 alone | 1700Hz | 0.5882ms |
| **GCD(700,1700)** | **100Hz** | **10ms - the exact repeat period of the WHOLE composite waveform** (already established this session: 160 samples @ 16kHz) |

None of these individually sits anywhere near 1 second. **But there's a striking exact relationship worth flagging**: 100 repetitions of that 10ms fundamental cycle sum to exactly 1.000000 second - and separately, `SAMPLE_RATE_HZ` (16000) samples is *also* exactly 1.000000 second, by definition. So while nothing about 700/1700 *specifically* produces a ~1Hz beat, "1 second" is an exact integer multiple of both the two-tone's own repeat unit (100×) and the raw sample-tick count (1×) - meaning ANY code that counts to 100 two-tone cycles, or counts to 16000 raw samples, and does something on that count, would land on almost exactly the measured period. **Checked and ruled out this preset as special**: recomputed GCD for every entry in `TWOTONE_BAND_PRESETS[]` (300/500, 700/900, 1500/1700, 2500/2700, 3500/3700, and the 700/1700 "legacy default" mislabeled "700/1900" in its own display string - a separate, minor, harmless documentation bug worth a one-line fix sometime) - **every single one shares the same 100Hz GCD / 10ms fundamental / 160-sample cycle**, by design. So if this 1-second relationship matters at all, it should show up on every two-tone preset in this codebase, not something unique to "Preset 3"/700-1700 specifically - worth confirming with a quick listen on a different band.

**This exact coincidence is precisely why today's own `test_signals.cpp` NCO fix (wraps `s_tone_sample_index` at exactly `SAMPLE_RATE_HZ` = exactly once per second) deserved a hard, rigorous re-check rather than being assumed clean** - it's the one piece of code in this whole project that explicitly keys off a 16000-sample/1-second boundary. Built a bit-accurate float32 simulation of the exact `generate_twotone_sample()` arithmetic across 3+ seconds and measured the actual discontinuity (vs. a naive continuous-phase extrapolation) at each 1-second wrap event, compared against ordinary 10ms (160-sample) boundaries elsewhere in the same run, and against arbitrary non-boundary samples as a floating-point-noise baseline. Result: **the wrap-boundary "jump" is ~5.0e-08 - actually SMALLER than the ordinary floating-point jitter seen at arbitrary non-boundary samples (1e-05 to 3e-04), and comparable to or smaller than jumps at ordinary 10ms boundaries elsewhere in the run (1e-11 to 6e-09)**. The wrap is, if anything, one of the numerically CLEANEST points in the whole cycle - confirming by direct simulation (not just the hand-argument in that fix's own comment) that it introduces no real discontinuity. **Today's NCO fix is cleared as the source of this specific symptom.**

**Also grepped specifically for any OTHER sample-tick-count-based (not `millis()`-based - already exhaustively checked) counter anywhere in the live DSP path that resets/wraps near `SAMPLE_RATE_HZ`/16000** - found none in `ssb_dsp.*`, `envelope_gdeq.h`, `envelope_floor.*`, `carrier_output.*`, or anywhere else outside `test_signals.cpp`'s own (now-cleared) wrap.

**Where this leaves the search**: the exact-100-cycles-per-second relationship is real and worth keeping in mind, but the one place it could plausibly manifest as a real signal artifact (the NCO fix's sample-index wrap) has now been checked and cleared by direct simulation, not just argued clean. The mechanism remains genuinely unidentified. Next concrete step: since every built-in preset shares the same 100Hz-GCD/10ms-fundamental grid, a clean causal test would be to break that alignment deliberately - e.g. temporarily testing a non-100Hz-spaced pair (something like 733Hz/1717Hz, chosen to NOT be a multiple of 100Hz apart) - if the ~1.000s pattern persists unchanged on a pair that does NOT complete an exact whole number of cycles per second, that would rule out the "100-cycles-per-second" coincidence entirely and point somewhere else; if it changes or disappears, that's a strong positive lead confirming this exact-alignment idea is real and worth pursuing further (though the mechanism BY WHICH it would matter still isn't identified - possibly something in the shaping IIR filters' own numerical behavior over many cycles, not yet checked the way the tone generator was here).

**Not actioned as a firmware change this turn** (only the still-pending 1700->1000 revert). Analysis and simulation only.

## 2026-09-17, later still: sharp follow-up answered directly - the 1-second wrap is structurally GUARANTEED to land exactly at the envelope PEAK, never near a null, and its proximity to the nearest real null (8 samples/0.5ms away for 700/1700) is no different from any other ordinary peak throughout the whole two-tone cycle. `Warbletone3-300-500.wav` confirms the ~1s pattern is also present on the 300/500 preset (same 100Hz-grid family)

User's direct follow-up to the interference-periodicity answer: does the 1-second sample-index wrap do something different specifically AT a phase/envelope null? Worth checking rigorously rather than assuming - the earlier simulation only measured a generic numerical discontinuity, not where in the null cycle it sits.

**Answer: no - the opposite, in fact, and it's not a coincidence, it's structural.** `generate_twotone_sample()`'s fix recomputes both tones' phase fresh from the sample index every tick: `phase = fmodf(index * hz / Fs, 1.0) * 2π`. At `index=0` (every wrap, by construction), `fmodf(0, 1.0) = 0` for BOTH tones regardless of their frequencies - meaning every wrap forces both tones back into perfect phase alignment, which is exactly the two-tone envelope's PEAK (maximum constructive interference), not a null. Verified numerically for both 700/1700 and 300/500: envelope at the wrap sample is exactly 2.000000 (the theoretical maximum for two equal-amplitude tones) in both cases.

Printed the full sample-by-sample envelope trace around a 700/1700 wrap to check proximity to the nearest actual null too: nulls occur at exactly ±8 samples (0.5ms, matching the 1000Hz difference frequency's quarter-period) either side of the wrap - and also recur every 16 samples continuously throughout the ENTIRE two-tone waveform, wrap or no wrap. So the wrap's 0.5ms distance to its nearest null is completely unremarkable - identical to the distance from any other ordinary envelope peak in the whole cycle to ITS neighboring nulls. There is nothing about the wrap that puts it closer to, or gives it any different relationship with, a null than any other peak has. Combined with the earlier finding that the wrap's own numerical discontinuity (~5e-08) is smaller than ordinary floating-point jitter elsewhere (~1e-05 to 3e-04), there's no compounding "wrap-near-null" vulnerability either individually or combined - the wrap is, if anything, the single safest, most numerically boring instant in the entire waveform.

Separately, analyzed the newly-uploaded `Warbletone3-300-500.wav` (300/500 preset - same 100Hz-GCD family as 700/1700, different absolute frequencies) with the same autocorrelation method used on the earlier two files: strongest peak at **0.979s** (correlation 0.340) plus its harmonic near 1.99s - confirming the ~1-second pattern is present on this preset too, not unique to 700/1700, consistent with the earlier finding that every built-in preset shares the same 100Hz GCD.

**Net effect: today's NCO fix is now cleared twice over** - once by direct discontinuity-size simulation, and now again by confirming its wrap point is structurally guaranteed to sit at the envelope's safest possible point, not an amplifying one. The mechanism remains genuinely open. The proposed non-100Hz-spaced-pair test (e.g. 733/1717Hz) is still the most direct way to test whether the exact-cycles-per-second alignment itself matters, independent of the (now twice-cleared) NCO fix specifically - worth doing next since two different presets sharing the same grid have both now shown the same ~1s behavior.

## 2026-09-17, later still: `warble733-1717.wav` - a mistake in the proposed test pair, caught by checking the math before over-interpreting the result: 733/1717 are coprime, so their own TRUE fundamental period is exactly 1.000s by definition, not a "broken alignment" control at all - and this turns out to be true of every possible integer-Hz two-tone pair, not a special property of any preset. Corrected next test proposed: non-integer-Hz tone frequencies

Same autocorrelation analysis on the newly-uploaded `warble733-1717.wav` (`'v'` off, then on - again no difference, consistent with every prior mute test): strongest peak at exactly **1.000s** (correlation 0.384), same signature as every prior recording.

**Before treating this as further confirmation, checked the math on the test pair itself and found a mistake in how it was chosen.** `gcd(733, 1717) = 1` (733 is prime; 1717 = 17x101, sharing no factor with 733) - meaning this pair's own TRUE, physically-correct fundamental period is 1/1Hz = exactly 1.000 second. This was meant to be a "broken alignment" control (a pair that does NOT complete a whole number of cycles per second, unlike every 100Hz-GCD preset already tested) - instead, by picking two coprime integers, it landed on a pair whose natural repeat period is even MORE exactly tied to 1 second than 700/1700 or 300/500 were. A ~1.000s finding on this specific recording is therefore not distinguishing evidence either way - it's also exactly what a perfectly healthy, bug-free two-tone generator would legitimately produce for this frequency choice.

**Bigger realization while checking this: the whole "100 cycles = 1 second, suspicious coincidence" framing from two entries ago doesn't actually hold up as a distinguishing test at all.** For ANY pair of integer-Hz tones, `GCD(f1, f2)` is itself an integer G, and by simple arithmetic, 1 second always contains EXACTLY G whole repeats of the resulting 1/G-second fundamental period, with zero remainder - this isn't a special property of 700/1700, 300/500, or 733/1717, it's a mathematical certainty for every integer-Hz two-tone pair that could be configured in this codebase (all of `TWOTONE_BAND_PRESETS[]` use whole-Hz values). So "does the signal's own fundamental period divide evenly into 1 second" can never be used to distinguish a real bug from ordinary behavior using integer-Hz test tones - it's true of all of them, always, by construction.

**Corrected test, the only way to actually break this alignment**: use NON-integer-Hz tone frequencies (`test_signals.cpp`'s `f1_hz`/`f2_hz` are `float`, so this is a simple temporary edit, e.g. 700.37Hz / 1700.61Hz) so the two-tone waveform's own true fundamental period does NOT land on any whole-second boundary at all (with irrational-ish/non-terminating-decimal-like spacing, the practical "repeat period" within any few-second recording effectively disappears). If the ~1.000s pattern still appears under that condition, that's genuinely decisive: it would prove the mechanism is NOT the tone generator's own periodic structure (already cleared twice on other grounds too - discontinuity size and null-proximity), and must be something else entirely, still unidentified. If it changes or disappears, that reopens the "exact whole-cycle-per-second alignment matters somehow" question, but even then the mechanism by which it would matter remains to be found (this specific NCO fix has already been cleared as the vector).

**`warble733-1717_v-on.wav` (the `'v'` on companion, "for completeness")**: same result, autocorrelation peak at 0.997s - consistent with every other mute test this session, `'v'` state still makes no difference.

**Checked for a serial-command way to dial in an arbitrary frequency pair without a firmware change** (to avoid another flash cycle) - none exists; `'t'`/`'T'` only switch to two-tone mode and cycle through the fixed `TWOTONE_BAND_PRESETS[]` array, no numeric-entry command for custom frequencies. Since the user was clearly already hand-editing frequencies directly to get 733/1717 (not a built-in preset), a firmware change is the practical path either way - **added a temporary preset entry to `test_signals.cpp`'s `TWOTONE_BAND_PRESETS[]`**: `700.37/1700.61 (TEMP non-integer-Hz control)`, reachable via repeated `'T'` presses (last in the cycle order), clearly marked for removal once this test's result is known. Not yet bench-tested - ready to flash and record.

## 2026-09-17, later still: `Warble733.37-1700.61.wav` - the decisive non-integer-Hz test, and it's a genuine positive result: the clean, single-peak ~1.000s autocorrelation signature seen on EVERY integer-Hz pair tested so far is gone, replaced by a noisy, multi-candidate spread across 0.3-1.0s with no clear winner - confirming the exact-whole-cycles-per-second alignment really does matter, while re-pointing suspicion at a live in-DSP resonance/entrainment mechanism rather than any specific timer (all of which are now cleared)

The decisive test: `700.37Hz/1700.61Hz` (the temporary non-integer-Hz preset added last entry), `'v'` off. Ran the same autocorrelation analysis used on every prior capture, using BOTH the original binary "matches lowest bin" indicator and a more robust continuous peak-frequency-value version (to rule out FFT-bin-quantization artifacts specific to a non-round-Hz tone pair) - both agree.

**Result: no single dominant ~1.000s peak anymore.** Where every integer-Hz recording so far (700/1700 twice, 300/500, even the coincidentally-1s-aligned 733/1717) showed ONE clearly dominant autocorrelation peak at ~0.98-1.00s (correlation 0.34-0.76, typically 1.5-2x any competing lag, plus a clean harmonic near 2x), this recording's strongest candidates are `0.437s (0.624)`, `0.376s (0.622)`, `1.000s (0.618)`, `0.501s (0.611)`, `0.312s (0.585)`, `0.624s (0.585)`, `0.939s (0.575)`, `0.563s (0.549)` - eight comparably-strong peaks packed into a narrow correlation band (0.549-0.624), no clear winner, 1.000s just one candidate among several rather than standing out.

**This is a genuine, meaningful result, not a null result** - the CHARACTER of the periodicity changed qualitatively (one sharp line -> a smeared cluster) exactly when the exact-whole-second alignment was broken, even though the underlying two-tone beat/null-crossing rate itself barely moved (difference frequency 1000.24Hz vs. the original 1000Hz - a 0.024% change, nowhere near enough on its own to explain going from one sharp ~1s line to eight smeared candidates). That rules out "it's just the beat frequency, and I coincidentally picked new tones with a similar beat" as an alternative explanation - something about EXACT commensurability specifically, not just approximate tone spacing, is what mattered.

**Where this points, now that every specific timer/counter candidate has been cleared** (`diagnostics_service()` twice over; today's own NCO-fix wrap, twice over - discontinuity size AND null-proximity): a classic signature of a **nonlinear, threshold/hysteresis-driven mechanism being driven by a periodic input** - exactly the same *class* of behavior already established this session as the cause of the 153.6s (pre-NCO-fix) and 8.15s (post-NCO-fix) macro cycles (shaped-envelope IIR filters and/or an `'H'`-style threshold detector), just not yet located in the LIVE signal-processing path itself (as opposed to `diagnostics.cpp`'s already-cleared, read-only copy of that same detector logic). The reasoning: when the two-tone input repeats EXACTLY every 10ms (the 100Hz-GCD family), every near-null/envelope event recurs at bit-for-bit the same phase every single cycle - if some downstream hysteresis mechanism has its own natural response period near 1 second, a perfectly exact-repeating driver would let it lock cleanly onto one sharp frequency (like a forced oscillator being driven at a commensurate rate). Break the exact repeat (the non-integer pair never truly re-aligns), and that same mechanism would be driven by an input that's ALMOST but not quite the same every cycle - exactly the condition that smears a resonance/relaxation-oscillator's response across a band of nearby periods instead of one clean line, which is precisely what was just measured.

**Not actioned as a firmware change this turn.** Concrete next step: since this points at a LIVE (not diagnostic-only) hysteresis/threshold mechanism, the place to look is the actual DSP/envelope signal path itself - `ssb_dsp.cpp`'s Hilbert/atan2 chain and `envelope_gdeq.h`'s shaping IIR filters - for anything with its own feedback/threshold behavior operating on a timescale near 1 second, analogous to (but distinct from, and not yet found the way `diagnostics.cpp`'s copy was) the already-characterized macro-cycle detector logic. This is a genuinely different, more promising search target than anything checked so far today (all of which lived in `diagnostics.cpp` or `test_signals.cpp` and have now been cleared).

## 2026-09-17, later still: `733-1700_Preset1.wav` - Preset 1 ("Live", every optional shaping/filter/compressor/EQ/predistortion/interpolation/ampeq stage OFF) makes the pattern CLEANER and STRONGER, not weaker, and reveals its true fundamental: ~0.485s (~2.06Hz) - almost exactly the user's original "twice a second" description, with the previously-measured "1.000s" being its own 2nd harmonic. Read `ssb_dsp_process_sample()`'s full core pipeline directly and found no candidate timer/EMA there either - the remaining live-path suspect is the near-null "sticks" mechanism itself, recurring deterministically because the input is exactly periodic

Parsed the pasted preset line positionally against `PersistentSettings`' field order (`settings.h`) to confirm exactly what "all filters off" means here: `env_gdeq_enable=false`, `adc_lpf_mode=OFF`, `eq_enable=false`, `compressor_enable=false`, `env_predistort_enable=false`, `env_floor=0.0` (off), `freq_dev_slew_limit_hz=UNLIMITED` (off), `envelope_interp_enable=false`, `env_ampeq_enable=false`, `env_ampeq_shelf2_enable=false` - genuinely every optional processing stage in the envelope/audio path disabled, leaving only the unconditional core (Hilbert FIR -> atan2 -> freq_dev -> hard clamp -> AD9851/PWM write).

**Result, both autocorrelation methods (they now agree exactly, suggesting an unusually clean two-level signal): strongest peak at 0.997s (correlation 0.822 - the strongest and cleanest yet, stronger than every prior filtered capture), with a SECOND, almost equally strong peak at 0.485s (0.799).** Checking the rest of the peak list against a ~0.4985s fundamental: 1.483s, 1.995s, and 2.483s match that fundamental's 3rd, 4th, and 5th harmonics almost exactly (0.4985 x 3 = 1.4955 vs 1.483 found; x4=1.994 vs 1.995; x5=2.4925 vs 2.483). **This strongly suggests the TRUE underlying period is ~0.485s (~2.06Hz), with the "1.000s" signature measured on every previous (filtered) capture actually being this same process's own 2nd harmonic**, likely partially masking the fundamental's visibility until the shaping/filter chain was removed. ~0.485s is, for the first time, a near-exact numerical match to the user's very first description of this whole symptom - "a regular freq shift about twice a second" - not just the hand-wave ("two salient states per 1s cycle") used earlier today to reconcile the two.

**This is a major narrowing result.** Every optional stage disabled in this preset (gdeq shaping, ADC LPF, EQ, compressor, predistortion, null floor, slew limiter, envelope interpolation, ampeq shelf1/shelf2) is now simultaneously ruled out - the pattern didn't just survive their removal, it got CLEANER and STRONGER. Read `ssb_dsp_process_sample()` (`ssb_dsp.c:716-906`) end to end to check the one remaining always-on core pipeline directly, rather than assume it's clean: Hilbert FIR (a ~129-tap, ~8ms-deep delay line - far too short to explain a 0.485s period on its own), `atan2`/`wrap_pi` (memoryless per-sample), the **unconditional, since-boot lifetime accumulators** (`dphi_sum`, `near_null_dphi_sum`, `env2_dphi_sum`, `env2_sum` - monotonically growing, no periodic reset visible in this function, so not themselves an oscillator), the slew limiter (confirmed off via this preset), and the hard `max_freq_dev_hz` clamp (always active, a static threshold with no time-dependent state). None of these has any built-in ~0.5s-scale timing behavior.

**Leading theory now**: rather than a hidden timer or hysteresis/EMA mechanism anywhere in the code, this may be the already-well-characterized near-null "sticks" phenomenon (the atan2/Hilbert numerical fragility at envelope zero-crossings, `AD9851.c`'s and this file's own documented leading theory all session) occurring **deterministically and exactly-repeatingly**, precisely because the two-tone input itself is exactly periodic (100Hz GCD, bit-for-bit identical every 10ms). If only a subset of the ~100 near-null crossings per second happen to land on the specific floating-point corner case that triggers a "stick," and that subset's own pattern recurs with a period around 48-49 cycles (~0.485-0.495s), the result would look exactly like what was just measured - a real, physical, deterministic RF-domain effect, not a bug in any timer, needing no downstream filter or hysteresis logic at all to produce a clean, sharp period. This is consistent with (and gives a concrete mechanism for) the entrainment/resonance framing from the previous entry, and with why the non-integer-Hz test smeared the pattern instead of removing it (a non-exactly-periodic input can't produce an exactly-repeating subset pattern the same way).

**Not actioned as a firmware change.** Concrete next step: capture a `'J'` jump-log AND an audio recording of the SAME run simultaneously (ideally on Preset 1, 700/1700, so both diagnostics and this newly-quantified ~0.485s audio period can be directly time-correlated) - if the jump-log's own timestamped near-null "stick" events recur at ~0.485s (or ~0.97s) intervals, that would directly confirm this theory rather than leave it as a plausible-sounding mechanism.

## 2026-09-17, later still: the requested simultaneous `'J'`-log + audio capture arrives (`log_20260917_142917.txt` + `700-1700_Preset1.wav`), and it complicates rather than confirms the "0.485s is the true fundamental" theory - 700/1700 on Preset 1 shows a clean SINGLE ~1.003s peak with NO 0.485s component at all, unlike the 733-based pairs. Correcting the overreach from the previous entry: the 0.485s subharmonic is not a universal property of this mechanism

**The log** (`log_20260917_142917.txt`) turns out to be a single `slow_trace: AUTO-CAPTURED` event (t=148119ms, delay=+0.00 this time, not the usual +2.00) plus one manually-triggered `'J'` jump_log snapshot (8 entries, all within a 5ms window, t=148730-148735ms) - a single point-in-time snapshot, not a continuous multi-trigger series. **This can't directly test the "near-null sticks recur every ~0.485s/~0.97s" theory** - that needs many repeated, precisely-timestamped trigger events to measure a recurrence interval from, which a one-shot snapshot doesn't provide. Noted for next time: either a longer hands-off run with multiple `AUTO-CAPTURED` events (like the 8.15s-cycle analysis method used earlier today), or the full-rate capture feature discussed below, would actually answer this.

**The paired audio** (`700-1700_Preset1.wav`, Preset 1/all-filters-off, `'v'` off): autocorrelation gives a clean, single dominant peak at **1.003s** (correlation 0.647) plus its harmonic at 1.990s (0.471) - checked explicitly at 0.485s and found NEGATIVE correlation (-0.111), i.e. no half-period component at all, cleanly contradicting the "0.485s is the universal true fundamental, 1.000s is just its 2nd harmonic" claim from the previous entry.

**Correction, so this doesn't stand uncorrected**: that claim was drawn from a single recording (`733-1700_Preset1.wav`) and shouldn't have been generalized this far. With this new data point, the pattern looks like: **every 700/1700 (100Hz-GCD) test done today** (`ToneWarble1.wav`, `WarbleTone2.wav`, and now this Preset-1 one) **shows one clean ~1.000s peak, no 0.485s substructure**, while **both 733-based (733/1717 and 733/1700, both coprime pairs with GCD=1Hz, exact 1-second natural period)** tests have shown the additional ~0.485s component (733/1717's `'v'`-on/off pair less clearly - worth re-checking those numbers now that this contrast is visible - and 733/1700's Preset-1 recording strongly). This suggests the 0.485s component may be specific to something about the GCD=1Hz/coprime class of tone pairs (or specifically to 733Hz itself) rather than a universal 2nd-harmonic relationship - genuinely still an open question, not resolved by this entry, just narrowed and honestly corrected rather than left standing as an overreach.

**This gap - a single log snapshot can't test a recurrence-interval theory - is exactly what a full-rate per-sample capture would solve, which the user separately asked about this same turn: see the new `'F'` capture feature added below.**

## 2026-09-17, later still: implemented the requested full-rate freq/env capture - new `'F'` serial command

Implemented the feature proposed in the previous entry, directly in response to the user's own question ("would it be worth doing a dump of all freq/env values every sample for a second or so to check they are the same values you expect"). This gives direct per-tick ground truth on `raw_freq_dev_current`/`raw_envelope_current` - the specific gap the single-snapshot `'J'` log couldn't fill - so the "does a near-null event recur at ~0.485s/~0.97s intervals" theory (previous two entries) can finally be checked against the real signal instead of inferred from SDR audio.

Design: `'F'` arms a capture (`diagnostics_freqenv_capture_arm()`, `diagnostics.cpp`) that lazily `malloc()`s two `FREQENV_CAPTURE_LEN=16000`-entry float arrays (~125KB total, one second at 16kHz - deliberately matching `SAMPLE_RATE_HZ` and today's own NCO-fix wrap period, so the captured window lines up with a clean whole-second boundary). A new inline hook, `freqenv_capture_tick()`, is called from the top of `diagnostics_set_tx_info()` (the same already-existing per-tick call site `ssb_mic_test.ino` uses for every other per-tick diagnostic in this file) and does a plain array write plus a length check - essentially free on the hot path when not armed. Once the buffer fills, state flips `ARMED -> READY`, and `diagnostics_service()` (Core 1, mute-exempt, same "unconditional, checked every call" pattern as the existing `canary_check_background()`/slow-trace-auto-dump/held-freq-auto-dump features) drives the dump: `READY -> DUMPING`, then ~40 CSV lines (`idx,raw_freq_dev_hz,raw_envelope`) per `diagnostics_service()` call, each individually guarded by the existing `diag_room_for()` TX-buffer-safety check so a slow/backlogged host just makes the dump take longer rather than blocking `dsp_task` or losing lines. Once fully dumped, both buffers are freed and state returns to `IDLE` - the buffer is intentionally NOT a permanent static array (unlike this file's other, much smaller trace buffers) because the board is an ESP32-S3 Super Mini (`config.h:249`), assumed to have no PSRAM, and a standing 125KB tax on internal SRAM for an occasional on-demand diagnostic is a bad trade.

Whole feature is AD9851-only (gated the same way `s_dbg_delayed_freq_dev`/`s_dbg_tx_freq` and the `'J'`/`'K'`/`'H'` features already are), matching this project's established convention: the public `diagnostics_freqenv_capture_arm()` is declared unconditionally in `diagnostics.h` (like `diagnostics_print_jump_log()` etc.) but is a no-op without `AD9851_ATTACHED`, and all the internal statics/helpers are wrapped in `#if AD9851_ATTACHED` entirely so nothing exists or costs RAM on a build without it. Verified brace/paren and `#if`/`#endif` balance across all three edited files (`diagnostics.h`, `diagnostics.cpp`, `serial_commands.cpp`) after every edit - no full ESP32 toolchain available in this environment to do a real build, so this is checked as carefully as possible short of an actual compile; needs bench verification before being trusted. Not yet bench-tested - ready to flash. Once run, compare the dumped freq_dev/envelope stream's own near-null timing directly against the ~0.485s/~0.97s recurrence theory, and check whether every 10ms two-tone cycle really does produce bit-for-bit identical values, as expected.

## 2026-09-17, later still: first bench run of `'F'` (`log_20260917_153446.txt`, 700/1700 Preset 1) - the feature works first try and gives a real, surprising answer: near-null "sticks" happen at essentially EVERY null crossing, not a rare recurring subset - the previous entry's leading theory was wrong on that specific point - but the glitch VALUES themselves are mostly locked into an exact repeating template, with occasional brief "slips" whose distribution across this one capture is suggestive but not enough data to pin a period on

The dump parsed cleanly: exactly 16000 CSV rows recovered (`idx,raw_freq_dev_hz,raw_envelope`), interleaved with ~192 blank lines and one unrelated `slow_trace: AUTO-CAPTURED` print from the ordinary 1000ms diagnostics block (both expected and harmless - the dump isn't given exclusive use of Serial, by design, since it's driven from the same `diagnostics_service()` as everything else).

**Finding 1 - sticks are not a rare subset, they're at every null.** Defining a "stick" as any `raw_freq_dev_current` outside a generous 1000-1400Hz normal band (the nominal beat-driven wobble sits at ~1198-1202Hz), there are exactly 1000 stick events across the 16000-sample/1.0s buffer, spaced at almost exactly 16 samples (1.0ms) apart - i.e. one at every single near-null envelope crossing, matching the already-established "nulls recur every 16 samples for 700/1700" fact exactly. This directly contradicts the previous entry's leading theory ("if only some fraction of the ~100 null-crossings/second hit the specific corner case... recurring every ~48-49 cycles") - that theory is now retracted. Every null-crossing sticks; there is no subset.

**Finding 2 - the glitch VALUES are mostly an exact, repeating 5-event template, occasionally interrupted.** The stick magnitudes are large (mean |deviation| ~6.7kHz, up to ~9.1kHz, both signs) and for long stretches repeat an *exact* 5-event/~5ms cycle bit-for-bit: e.g. the first 0.346s of this capture reproduces the identical 5-value sequence (`-6802.0, +6003.4, +6169.4, +6171.2, +6004.1` Hz) every single time, to 2 decimal places - about 70 repetitions with zero deviation. This is itself a striking confirmation of how deterministic the underlying two-tone-driven glitch mechanism is when nothing perturbs it.

**Finding 3 - but it isn't perfectly deterministic across the whole second: 16 of the 1000 events (1.6%) depart from that template**, taking on a different glitch value/sign at that same null crossing (e.g. idx 9852: expected ~+6000Hz, got -7705.10Hz instead - checked by hand in the raw CSV, not just the diff statistic). These 16 "slip" events cluster into 6 brief groups (1-2 events each) at t=0.346s, 0.616s, ~0.690-0.694s, ~0.766-0.770s, ~0.799s, and ~0.916-0.920s - none at all in the first 0.346s, then scattered across the remaining ~0.65s, with a visibly denser run from ~0.62s to ~0.92s than earlier in the capture.

**Honest limitation, flagged rather than overreached past**: 6 slip clusters in one ~1-second window is nowhere near enough to responsibly claim a recurrence period - the gaps between them (0.270s, 0.076s, 0.076s, 0.031s, 0.118s) don't cleanly match either the ~0.485s or ~0.97s candidates from the audio analysis, though a couple of loose multiples of ~0.076s land near both (13x≈0.99s, 6.5x≈0.49s) - noted only as a "worth watching for," explicitly NOT as a finding, given how easily coincidental small-sample arithmetic like this misled the previous two entries. This needs either several more back-to-back `'F'` captures, or (cleaner) extending `FREQENV_CAPTURE_LEN` for a multi-second single capture, to actually resolve.

**Next step, concrete**: since `'F'` now demonstrably works on real hardware first try, the highest-value follow-up is a LONGER single capture (RAM permitting - `FREQENV_CAPTURE_LEN` would need to grow past 16000; 2-3x is probably safe to try given this run used ~125KB without issue, but should be checked against actual free-heap headroom on this no-PSRAM board before assuming so) specifically to get enough slip events in one continuous window to measure their own recurrence interval directly, rather than inferring it from audio.

## 2026-09-17, later still: independent from-scratch simulation of `ssb_dsp.c`'s actual algorithm reproduces `log_20260917_153446.txt`'s exact glitch values - the near-null "sticks" mechanism is now CONFIRMED, not just theorized, and it's expected/deterministic math, not board-specific noise

The user asked directly whether the captured values match a simulation. No prior simulation this session had actually modeled this specific mechanism (the earlier float32 simulations were of `test_signals.cpp`'s NCO phase generator, a different piece of code, already cleared as a cause) - so built one from scratch: a Python port of `ssb_dsp_process_sample()`'s real pipeline (the actual 65-tap windowed Hilbert FIR via `generate_hilbert_coeffs()`, `fast_atan2`/`fast_sqrt`'s exact polynomial/bit-hack approximations, `wrap_pi`, all forced to `numpy.float32` to match the firmware's single-precision arithmetic), driven by `generate_twotone_sample()`'s exact recompute-from-index scheme at 700Hz/1700Hz, `TWOTONE_AMPLITUDE=0.45` (config.h), USB sideband (`dsp_state.cpp`'s default), `MAX_FREQ_DEV_HZ=20000` clamp, no slew limit - i.e. every parameter Preset 1's "all filters off" test already established was in play. Nothing in the simulation was tuned or fitted to the captured data - every constant came directly from reading the source.

Running it for 16000 samples (skipping the first 500 while the zero-initialized delay line fills, since the real capture started from an already-running signal at an unknown phase - an exact sample-for-sample match isn't expected or needed, only a statistical/template-level one) and the agreement is decisive:

- **The captured dominant repeating template** (`-6802.0, 6003.4, 6169.4, 6171.2, 6004.1` Hz, seen 70+ times bit-for-bit identical in the hardware capture) all appear in the simulation's own discrete set of glitch outcomes, matching to within 0.1-1.2Hz (e.g. real -6802.0 -> sim -6802.0; real 6004.1 -> sim 6004.0).
- **The rarer "slip" values** from the previous entry also show up in the sim's value set: real -7705.10/-7696.98 (idx 9852/9885) matches the sim's -7705/-7706 cluster (53x each); real -7891.06/-7891.97 (idx 14653/14684) matches the sim's -7893 cluster (23x); real 7608.93/7597.56 (idx 11036/11101) matches the sim's 7599-7602 cluster (~26x each).
- **Event rate**: the sim sticks at essentially every null too - 969 events over 15500 samples (~1 per 16 samples), matching the capture's 1000/16000.
- **Slip rate**: 1.4% of sim events depart from their local period-5 template by >1500Hz, vs. 1.6% in the real capture - close enough to call the same phenomenon.
- **Locked-stretch character**: the sim also alternates "locked to one template for a while, then briefly departs" - longest locked stretch 0.426s in the sim vs. 0.346s+ observed at the start of the real capture (right-censored - the real clean run might have started before t=0 and lasted longer, so this is a lower bound, not necessarily a mismatch).

**Conclusion**: the near-null stick mechanism - both its ubiquity (every null crossing) and its specific large repeating magnitudes - is a confirmed, expected mathematical property of running `fast_atan2`/`fast_sqrt`-based instantaneous-frequency estimation on an exactly-periodic two-tone signal near envelope zero-crossings. It is NOT board-specific noise, not the ADC, not a power-supply artifact, not anything hardware-side - it's precisely the "phase noise near zero-crossings of the envelope" that `ssb_dsp_process_sample()`'s own clamp comment already names as the reason that clamp exists ("the same 'restrict the phase changes' step QCX-SSB applies"). A pure from-scratch reimplementation of the documented algorithm, with zero fitting, reproduces the real hardware's own specific numbers to sub-Hz precision.

**What's still genuinely open**: this confirms the STICKS, not the ~0.485s/~0.97s audible warble. What determines exactly which discrete "slip" value occurs at a given null, and whether THAT has a slow periodicity connecting to the audible symptom, remains unanswered - this entry explains the mechanism producing the raw material, not the higher-level timing pattern the ear/SDR is actually reacting to. The simulation script itself (`sim_ssb_dsp_verification.py`) is a good tool for that next question too - it can be run far longer than any single `'F'` capture (no RAM/Serial-bandwidth limits in Python) to gather enough slip events to test a recurrence period directly, entirely off-hardware, before spending a bench session on it. Not committed to the repo (ad hoc analysis script, same convention as this session's other Python analyses) - delivered directly to the user instead.

## 2026-09-17, later still: `FREQENV_CAPTURE_LEN` bumped 16000 -> 32000 (1.0s -> 2.0s, ~125KB -> ~250KB) after the user reported this board's real build-time RAM headroom

User reported the actual Arduino build output for this board: "leaving 299220 bytes for local variables. Maximum is 327680 bytes" - i.e. ~292KB free at link time against a 320KB total chip (ESP32-S3 Super Mini, no PSRAM, as already assumed), comfortably more than the original 1.0s/~125KB capture needed. Doubled the capture length to 2.0s/~250KB rather than spending the whole reported figure, since that 292KB pool is also shared with every FreeRTOS task's stack and every other library's own runtime heap use (USB-CDC buffers etc.) - none of which show up in a link-time static-usage report, so real available margin at the moment `'F'` is actually pressed is smaller than 292KB, by an unknown amount. If 2.0s still turns out to be too much, `diagnostics_freqenv_capture_arm()`'s existing malloc-failure path already handles that gracefully (prints why, declines to arm, doesn't crash) - the size can be dialed back from real evidence rather than guessed conservatively small up front.

Directly serves the still-open goal from the previous two entries: 2.0s gives room for 2-4 repetitions of a hypothetical ~0.485s-0.97s slip-timing period in one continuous, uninterrupted window, instead of the single ~1.0s capture's inconclusive 6 slip-clusters. Updated every place the old 16000/1.0s/125KB figures were quoted (`diagnostics.h`'s declaration comment, `diagnostics.cpp`'s `FREQENV_CAPTURE_LEN` comment and the `'F'`-armed confirmation string, `serial_commands.cpp`'s `'F'` handler comment and reply text) - verified brace/paren/`#if` balance across all three files again after the edit. Not yet bench-tested at the new size - ready to flash. Once run, this is the capture to actually test the slip-recurrence question against, either directly or by feeding it through `sim_ssb_dsp_verification.py`'s same event-detection pipeline used on `log_20260917_153446.txt`.

## 2026-09-17, later still: `'F'` now suppresses every OTHER `diagnostics_service()` print for the capture's whole lifecycle, not just during the CSV dump - the user's external logger was choking on the interleaved traffic

The first real capture (`log_20260917_153446.txt`) parsed cleanly, but only because the parser explicitly filtered out ~192 stray non-CSV lines (blank lines plus one `slow_trace: AUTO-CAPTURED` print) - the user's own external logger doesn't have that luxury and reported the interleaving as a real problem. Root cause: `diagnostics_service()`'s other periodic/background work (the 45ms status line, the 1000ms `[timing]`/`[adc]`/`[dsp]` block, `canary_check_background()`, and the slow-trace/held-freq auto-dumps) keeps running unchanged while a freqenv capture is armed, filling, or dumping, since nothing previously told it not to.

Fix: added `freqenv_capture_in_progress()` (`diagnostics.cpp`, `true` whenever the capture state isn't `IDLE`) and a hard early-return at the very top of `diagnostics_service()` - when a capture is ARMED, READY, or DUMPING, the function now does nothing but call `diagnostics_freqenv_capture_service()` and return, skipping literally everything else regardless of mute state. Deliberately gates on the WHOLE capture lifecycle, not just the DUMPING phase - the ~2s ARMED collection window would otherwise still let the 45ms/1000ms blocks interleave before the CSV even starts. Removed the old `diagnostics_freqenv_capture_service()` call site further down in the function (alongside canary/slow-trace/held-freq) since it's now unreachable whenever a capture is active (already returned above it) and a no-op whenever one isn't - dead code either way, so deleted rather than left in place. Verified brace/paren/`#if` balance across `diagnostics.h`/`diagnostics.cpp`/`serial_commands.cpp` again (only `diagnostics.cpp` actually changed this time).

One accepted tradeoff, worth being explicit about: `canary_check_background()`'s integrity check and the slow-trace/held-freq auto-dump monitors are now silent for the capture's whole duration (up to ~2s ARMED plus however long the chunked CSV dump takes to drain over Serial) - acceptable since `'F'` is an explicit, one-shot, user-requested action, same reasoning already applied to why this feature is mute-exempt in the first place; nothing is lost permanently, monitoring just resumes the instant the capture returns to `IDLE`. Not yet bench-tested - ready to flash.

## 2026-09-17, later still: second `'F'` capture (`log_20260917_160237.txt`, truncated to its last 0.54s by the user's own logger) leads to a much bigger finding via simulation - the raw freq_dev/envelope signal is PROVABLY exactly periodic at 16000 samples/1.000s, for ANY tone frequency, by construction of the tone generator's own NCO-fix design; two independent real captures confirm this to <0.1ms precision

**The suppression fix worked**: this capture's CSV body has zero interleaved lines (only trailing content AFTER `dump complete` - a `slow_trace: AUTO-CAPTURED` print and an unrelated `trend[...]` dump, both from diagnostics resuming normally once the capture returned to `IDLE`, exactly as designed). **But the file itself is truncated**: it only contains CSV rows for idx 23371-31999 (contiguous, no internal gaps) - the last 0.539s of the intended 2.0s/32000-sample buffer, plus the `ARMED`/header lines are missing entirely. This looks like a limit on whatever captured the Serial output (a terminal's scrollback buffer keeping only the most recent ~8600 lines, most likely), not a firmware issue - flagged to the user, with a suggestion to use a dedicated file-logging tool (`screen -L`, PuTTY logging, or `cat /dev/ttyUSBx > log.txt`) that doesn't have a buffer ceiling, next time.

**The available 0.54s was still enough to prompt a much bigger check.** Running the same period-5-event slip-detection analysis used on the first capture found a slip-cluster gap sequence - `0.0750, 0.0760, 0.0315, 0.1185` seconds - that is IDENTICAL to the last four gaps already recorded from the first capture (`log_20260917_153446.txt`). Aligning the two captures' matching cluster centers gives a constant offset across all five points: `1.0190, 1.0190, 1.0190, 1.0190, 1.0191` seconds - mean 1.01902s, std **0.00004s (40 microseconds)** across two captures taken minutes apart with no reset in between.

That precision demanded an explanation, so it was checked directly against theory via `sim_ssb_dsp_verification.py`: ran the simulation for 3 consecutive tone-generator periods (48000+ samples) and compared segment-to-segment at exactly 16000-sample lag. **Result: max absolute difference = 0.0 (exact float32 equality)** for `freq_dev` AND `envelope`, for the 700/1700 pair, for 733/1700 (the coprime "control" pair), and for 700.37/1700.61 (the non-integer-Hz "TEMP" control preset) - EVERY tone-frequency choice tested produces an output that is bit-for-bit periodic at exactly 16000 samples. Mechanism: `generate_twotone_sample()` recomputes both tones' phase FRESH from `s_tone_sample_index` every tick (today's earlier NCO fix), and that index wraps at exactly `SAMPLE_RATE_HZ` (16000) regardless of `f1`/`f2` - this forces the raw two-tone INPUT itself to be exactly 1.000-tone-second periodic no matter what frequencies are chosen, overriding whatever "natural" beat period the chosen pair would otherwise have. Since `ssb_dsp_process_sample()`'s Hilbert FIR is linear/time-invariant and everything after it (atan2, dphi, freq_dev, envelope) is memoryless per-sample, the whole output chain inherits that exact periodicity by straightforward LTI-system reasoning - confirmed numerically, not just argued.

**This closes the loop on today's central ~1.000s investigation with unusual completeness**: the raw transmitted freq_dev/envelope stream genuinely does have a complex (not just "quiet"), precisely-timed structure of near-null phase-noise spikes that recurs EXACTLY once per tone-generator second, by mathematical necessity of the current tone-generator design - not a bug in any conventional sense, but a direct, provable, now-hardware-confirmed explanation for why every 700/1700-family audio test this session locked onto a clean ~1.000s (or harmonic) autocorrelation peak. The two real captures' 40-microsecond-precision agreement is the hardware directly living up to what the simulation proves must be true.

**New tension, explicitly flagged rather than smoothed over**: this same proof means the raw digital signal CANNOT repeat at any period other than an exact divisor of the tone-generator's fixed cycle - so it cannot be the source of the ~0.485s subharmonic recorded earlier today for `733-1700_Preset1.wav` (733/1700 is coprime, and this entry just confirmed 733/1700 through the simulation too - exactly periodic at 16000 samples, no 0.485s component possible from this mechanism). That earlier finding isn't retracted outright - the audio correlation there was real (0.822, the strongest of the session) - but it now needs re-examination in light of this proof: either something downstream of `ssb_dsp_process_sample()` that Preset 1 doesn't actually fully silence is introducing genuine non-1s-divisor periodicity, or the ~0.485s component originates on the analog/RF/SDR/audio-capture side rather than in this digital mechanism. Next step: revisit that specific test's conditions, and/or get a full (untruncated) `'F'` capture during an actual 733/1700 Preset 1 run to check directly whether ITS raw digital stream also proves exactly periodic on real hardware, the same way this entry just confirmed for 700/1700.

## 2026-09-17, later still: `log_20260917_162817.txt` - the 733/1700 `'F'` capture requested above comes back FULL and UNTRUNCATED, and settles the tension outright: 733/1700's raw digital stream is ALSO exactly periodic at 1.000s on real hardware (bit-for-bit, zero difference, across the entire 2.0s buffer) - the earlier ~0.485s finding is DEFINITIVELY not coming from this mechanism

Capture came back clean this time: all 32000 CSV rows present (idx 0-31999, contiguous), the interleaving fix held (only the expected header/footer lines outside the CSV body, no stray prints mixed into it), and the user logged it with a proper file-based method rather than terminal scrollback - no truncation.

Split the buffer at its exact midpoint (samples 0-15999 vs. 16000-31999, i.e. one full tone-generator period apart) and compared directly: **max absolute difference = 0.0** for `raw_freq_dev_current`, across all 16000 compared samples - not close, not "mostly matching," bit-for-bit identical, same as the simulation predicted and the same as 700/1700's cross-capture comparison found (there, to 40-microsecond precision across two separate captures; here, exact-zero within a single continuous one, an even more direct test). For contrast, checked a non-period lag (8000 samples, a quarter of the buffer) on the same data and got a max difference of 8220Hz - confirming the exact match at the 16000-sample lag isn't a trivial/degenerate result, the signal genuinely has real, large-scale structure that only repeats at the tone generator's own period.

**This settles the tension flagged in the previous entry.** 733/1700's raw `freq_dev`/`envelope` output is now confirmed, on real hardware, to be incapable of producing any period other than an exact divisor of the tone-generator's 1.000-second cycle - definitively ruling out `ssb_dsp_process_sample()`'s own output as the source of the ~0.485s component recorded in `733-1700_Preset1.wav` (`2026-09-17, later still: 733-1700_Preset1.wav` entry above). That audio correlation was real (0.822) and isn't being dismissed as noise - but it cannot be explained by anything happening inside the core DSP loop, full stop, now that this loop's output has been proven exactly 1.000s-periodic on the very same tone-pair/Preset-1 configuration that produced it.

**Narrows the remaining mystery to two concrete candidates**, neither yet tested: (1) something DOWNSTREAM of `ssb_dsp_process_sample()` that "Preset 1, all filters off" doesn't actually fully disable - worth re-checking `envelope_output`'s PWM offset/scale mapping, `relative_delay_apply()`, `envelope_interp_on_full_tick()`, and the AD9851 chip's own analog behavior (SPI transfer timing, its internal PLL) for anything with real state that could survive across dsp_task ticks and introduce a genuine non-1s-periodic component; or (2) the ~0.485s component is on the analog/RF/SDR/audio-capture side entirely - the SDR's own AGC/demodulator, the USB audio interface, or Audacity's own processing - none of which this firmware-side proof says anything about. Next step: since the digital signal is now proven clean for BOTH tone pairs tested, the highest-value follow-up is checking the SDR/audio chain directly (e.g. the cheap test proposed much earlier today for the original AGC theory - manual/fixed SDR gain - was never actually run since the user's own direct hands-on check pointed at the ESP side instead; worth reconsidering now that the ESP's own core DSP is doubly proven clean).

## 2026-09-17, later still: simulation confirmed the existing `'Q'` dither destroys the whole-second periodicity proven above, but a real-hardware test of it came back SUBJECTIVELY NOISIER, not cleaner - dither is now suspect as the wrong fix, and the existing freq_dev slew-rate limiter looks like the better next candidate

Following on from the two entries just above (raw `freq_dev`/`envelope` proven exactly periodic at 1.000s for any tone pair): the user proposed "strategic dither" as a mitigation, and a review of `test_signals.h`'s doc comments turned up that a dither feature already exists (`'Q'`, built 2026-09-11, `TWOTONE_DITHER_MAX_HZ=0.05f`/`TWOTONE_DITHER_UPDATE_HZ=4.0f`, tone2-only, off by default) - built for a mechanistically related but narrower purpose (per-null sample-grid-alignment bias in `null_bias`/`null_bias2`, not today's whole-second finding). Asked the user where dither work should be aimed given the whole-second warble has only ever been demonstrated on the artificially-exact-periodic two-tone test signal; user chose **"Test generator only"** - i.e. leave the live voice TX path untouched.

Built a second simulation (`sim_dither_test.py`, scratchpad-only, not committed) extending the existing verification script with `test_signals.cpp`'s exact dither algorithm (continuous phase accumulator for tone2 when dithered, random target drawn every `TWOTONE_DITHER_UPDATE_SAMPLES` ticks, linear ramp in between). Result: the EXISTING dither parameters, completely unchanged, decisively break the exact cycle-to-cycle repetition proven above - max `freq_dev` difference between consecutive 1-second cycles jumps from 0.0Hz (undithered) to ~14677.5Hz (dithered), with ~1732/16000 samples changing by more than 50Hz between cycles. Tested 10x (0.5Hz) and 40x (2.0Hz) dither magnitudes too; neither showed a meaningful improvement over the existing tiny 0.05Hz setting, so no retuning looked necessary. On paper, this looked like confirmation that `'Q'` (as-is) already solves the exact-periodicity problem.

**Real-hardware test contradicted that conclusion.** The user tried `'Q'` on the bench and reported the result was NOISIER, not cleaner - the opposite of what the simulation's periodicity-breaking result would suggest. Working theory (not yet confirmed, needs a real 'F' capture with 'Q' on to check directly): the simulation only ever checked whether cycle-to-cycle correlation broke, never whether the GLITCHES THEMSELVES got any smaller. They don't, by construction - dithering tone2's frequency changes WHEN and how often near-null events happen, and shuffles their exact timing, but doesn't touch the actual magnitude of the ~5000-9000Hz single-sample `freq_dev` spikes documented in the two entries above. Before `'Q'`, those spikes recurred at the identical point in every 1.000s cycle, so they were heard (if at all) as one discrete, coherent, low-frequency buzz. With `'Q'` on, the same total spike energy is smeared essentially at random across the whole run - likely reads to the ear as broadband "noisier" content rather than as an improvement, even though the coherent periodic tone genuinely is gone. If this theory holds, dither of ANY kind - tone-frequency dither as already tried, or the user's follow-up proposal below - only redistributes this error, it doesn't reduce it, and is unlikely to be the right fix regardless of where in the signal chain it's applied.

**Follow-up user proposal, same day**: since the dither approach didn't help, the user asked whether the "fast trig" functions themselves (`fast_atan2`/`fast_sqrt`, `ssb_dsp.c`) are producing "characteristic output patterns" that should instead be randomized directly, rather than randomizing the test tone. Reviewed both functions' actual implementation: `fast_atan2` is a continuous 5-term minimax polynomial fit to `atan(x)` (worst-case error ~1e-3 rad, spot-checked ~1.1e-5 rad at x=1) plus standard quadrant/reciprocal identities; `fast_sqrt` is the classic fast-inverse-sqrt bit-hack seed refined by two Newton-Raphson iterations (~1e-4 relative error). Neither one is a lookup table or otherwise quantizes its input to a small set of discrete output codes - both are smooth, deterministic, continuous functions of their inputs. That means the "characteristic template" of repeating glitch values found in `log_20260917_153446.txt`'s analysis (the earlier "independent simulation confirms near-null stick mechanism" entry) is a symptom of the SAME (I, Q) pairs recurring exactly, courtesy of the tone generator's proven exact periodicity - not a separate quantization effect baked into `fast_atan2`/`fast_sqrt` themselves. Randomizing the trig functions' own computation (e.g. perturbing their inputs or internal constants) would very likely reproduce the same mechanism as the tone-frequency dither already tested: it would decorrelate which exact glitch value appears when, without reducing the glitches' actual size - i.e. probably the same "noisier, not cleaner" outcome, for the same underlying reason. Also, unlike `'Q'`, `fast_atan2`/`fast_sqrt` run in the always-on `ssb_dsp_process_sample()` path used by BOTH the test generator and live voice TX - so touching them at all would be a much larger-scope change than the "test generator only" boundary already agreed for this investigation.

**A more promising, already-built candidate, not yet tried**: `ssb_dsp_set_freq_dev_slew_limit_hz()` / the `'{'`/`'}'` runtime toggle (`ssb_dsp.c`, off by default at `SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ`). This caps the raw SIZE of each sample-to-sample `freq_dev` jump directly, rather than trying to decorrelate when jumps happen - a magnitude-based fix instead of a randomization-based one. Its own existing doc comment already states the first step-in value (`FREQ_DEV_SLEW_START_HZ = 2000.0f`) is "comfortably below a null event's ~8000Hz/sample, so it actually engages only where intended" - i.e. it reads as though it was built with exactly this glitch class in mind, and should clamp the actual spike magnitude regardless of whether the underlying null recurs periodically (test tones) or at random times (real voice). It runs on both the test and live paths already (existing code, not a new change), which is a separate question from today's "test generator only" scoping decision and would need to be discussed with the user before treating it as in-scope. **Not yet tested on the near-null glitches specifically - next concrete step is an `'F'` capture with the slew limiter enabled (`'{'` a few times to get into its ~2000-8000Hz range) to see directly whether it clamps the ~5000-9000Hz spikes down, and whether that measurably changes the ~1.000s warble's audibility.**

Also still open, not yet asked: exactly what "noisy" meant in the user's real-hardware `'Q'` test (audible on the transmitted signal, on an SDR waterfall, in the null_bias/null_bias2 diagnostic numbers, or something else) - needed to log a precise finding rather than the necessarily-approximate theory above, and to know whether the slew-limiter test above should be run under the same conditions for a fair comparison.

## 2026-09-17, later still: decided next step is a slew-limiter bench test, not trig randomization; "noisier" clarified as audible on the demodulated/received signal

Follow-up to the entry just above. Asked the user two things: (1) whether to test the existing freq_dev slew-rate limiter next, or go ahead and investigate randomizing `fast_atan2`/`fast_sqrt` anyway; (2) what "noisier" meant in the real-hardware `'Q'` test. Answers: **test the slew limiter next**, and "noisier" meant **audible on the demodulated/received signal** (heard directly, e.g. off an SDR or receiver) - not a waterfall/spectrum observation and not the `null_bias`/`null_bias2` diagnostic numbers specifically. Logged here so the eventual comparison is judged by the same criterion the problem was originally reported in.

**No firmware change needed for this test** - `ssb_dsp_set_freq_dev_slew_limit_hz()` and its `'{'`/`'}'` runtime toggle already exist and are already wired into the live `ssb_dsp_process_sample()` path (see the entry two above this one). The bench procedure, for whenever the user is set up to run it:

1. Make sure `'Q'` (two-tone dither) is OFF, so this is a clean single-variable comparison against the noisier dithered result already observed, not a combination of both changes at once.
2. Press `'{'` once - this takes the limiter from off straight to `FREQ_DEV_SLEW_START_HZ = 2000.0f`, which is inside the range its own doc comment calls out as "comfortably below a null event's ~8000Hz/sample, so it actually engages only where intended." Press `'{'` again (each press is -250Hz) to tighten further, or `'}'` to loosen back toward off, if 2000Hz doesn't obviously help or over-limits normal speech content.
3. Run the same 700/1700 (or 733/1700) two-tone Preset-1 test used throughout today's investigation and listen to the demodulated/received audio for the same artifact that made `'Q'` sound noisier - specifically listening for whether the ~1.000s periodic buzz/warble is reduced WITHOUT being replaced by new broadband noise (the failure mode `'Q'` apparently hit).
4. If practical, take an `'F'` capture during the same run (slew limiter on) - this directly shows whether the raw `freq_dev` spike magnitude is actually being clamped to roughly `+/-2000Hz` (or whatever the current step is) instead of the previously-recorded ~5000-9000Hz values, which would be the clearest possible confirmation that this is a magnitude fix rather than another redistribution fix.
5. For a clean before/after, ideally capture/listen once with the limiter off (baseline, already effectively documented by today's earlier captures) and once with it on, same tone pair/preset, nothing else changed.

Nothing here is code - this is a test plan for the next bench session. `fast_atan2`/`fast_sqrt` randomization is parked, not pursued, pending this result.

## 2026-09-17, later still: bench result on the slew limiter - "central freq" shifts with the setting, but the warble sounds about the same at every setting; a simulation reproduces and explains BOTH observations, and neither is good news for this as a fix

User's report from the bench, in full: the slew-rate limiter measurably moves the perceived/measured center frequency depending on its setting, but by ear the warble itself sounds much the same across settings. Rather than guess, built a direct simulation (`sim_slew_limiter_test.py`, scratchpad-only, not committed) that ports the EXACT slew-limiter algorithm (`ssb_dsp.c` lines ~880-887: `freq_dev = prev + clamp(freq_dev - prev, -limit, +limit)`) into the existing verified DSP simulation, in its real position in the pipeline (after the null-bias/true-peak diagnostics, before the final `MAX_FREQ_DEV_HZ` hard clamp - matching the source exactly). Ran the 700/1700 two-tone signal through it at every step `'{'` actually walks through (2000, 1750, 1500, 1250, 1000, 750, 500, 250, 100 Hz), plus the off/unlimited baseline, over 10 concatenated 1.000s periods so the limiter's own internal ramp state reaches a genuine periodic steady state before measuring (confirmed converged: period 8 vs. period 9's weighted mean matched to the last printed decimal, i.e. this is steady-state behavior, not a startup transient).

Per an existing code comment in `ssb_dsp_process_sample()` (the `dphi_sum`/`env2_dphi_sum` "CORRECTION" note), the physically-correct measure of what a receiver actually perceives as the average/center frequency is the envelope-SQUARED-weighted average of instantaneous frequency, not a plain average - confirmed against real hardware in an earlier session specifically because the plain average overstates things by weighting the loud, near-zero-envelope null glitches equally with normal-envelope content. Results (700/1700, last steady-state second):

- **Raw/slew-OFF baseline**: env2-weighted mean = 1200.334 Hz (this is simply the natural energy-weighted "instantaneous frequency" of an equal-amplitude 700/1700 two-tone signal sitting near the 700-1700 midpoint - expected, benign, not itself part of the artifact). Peak = 7912.0 Hz, rms = 2176.6 Hz.
- **Slew 2000Hz** (the `'{'`-once setting): env2-weighted mean = 1217.085 Hz (**+16.75 Hz vs. raw** - a real, steady-state, non-transient shift). Peak drops to 5202.0 Hz, rms to 1590.1 Hz.
- Tightening the limit step by step shrinks BOTH numbers monotonically: by 100Hz (the tightest step, `FREQ_DEV_SLEW_MIN_HZ`), the weighted-mean shift is down to +0.98 Hz (1201.310 Hz) and peak is down to 1403.5 Hz, rms to 1211.8 Hz.

**This reproduces and explains both of the user's bench observations at once, and neither result looks like a real fix:**

1. **"Central freq changes with slew setting"** - confirmed real and steady-state, not a runtime-toggle glitch. Mechanism: an unclamped near-null spike is a single sample where envelope is also near zero, so it's almost entirely suppressed by the env^2 weighting (this is exactly why real hardware doesn't show the "hundreds of Hz" the plain average would predict, per the existing code comment). But the slew limiter forces that same spike's excess deviation to be reached and unwound gradually over SEVERAL samples - and some of those samples fall after the envelope has already recovered from the null to a normal, non-near-zero level. Those newly-affected samples DO carry real weight in the env2-weighted average, which is exactly the mechanism that pulls the perceived center away from raw. Counter-intuitively the shift is LARGEST at the LOOSEST engaged setting (2000Hz) and shrinks toward zero as the limit tightens toward its 100Hz floor - tightening the limit doesn't make this side effect worse, it makes it smaller (while also cutting peak magnitude, a separate benefit).
2. **"Warble sounds the same at every setting"** - also explained: nothing about the slew limiter changes WHEN or how OFTEN a near-null event happens (still ~1000 events/second, the same recurring pattern documented in the `'F'`-capture entries above); it only changes how big each resulting glitch is allowed to become. If the audible "warble" character comes from the RECURRING RATE/PATTERN of these events rather than their peak linear amplitude (plausible - impulsive/repetitive artifacts are often perceptually dominated by their rate and regularity, not just their nominal size), then no slew setting removes that character, it only turns a big periodic glitch into a smaller but equally-frequent one.

**Net assessment**: like the `'Q'` dither result two entries above, the slew-rate limiter attacks a proxy (rate of change, or timing/correlation) rather than the actual root cause (a null-crossing event happening at all, ~1000 times/second, every one of which perturbs `freq_dev` by some amount). Reducing peak magnitude is a genuine, real benefit of tightening the limit (7912Hz down to 1403Hz raw peak at the tightest setting), but it comes with a small, real, previously-undocumented center-frequency side effect, and apparently does not remove the audible "warble" character on its own. Simulation only covers 700/1700 so far - not yet checked against 733/1700 or real voice-like (non-periodic) input, and the ~17Hz maximum shift found here should be checked against whatever the user actually measured/heard on the bench (method and magnitude not yet reported) before treating this analysis as validated.

**This makes the two already-identified-but-unimplemented fix directions in "Open, un-actioned next steps" below look more attractive than either dither (tested, worse) or slew-limiting (tested, mixed/real side effect) as the next thing to actually try** - both aim at the null-crossing phase/frequency computation itself rather than at redistributing or rate-limiting its output after the fact.

## 2026-09-17, later still: CORRECTION - the previous entry's ~17Hz weighted-average shift badly undersold the real problem; a spectral (FFT) reconstruction shows the slew limiter causes severe intermodulation distortion of the two tones themselves, matching the user's real ~250-500Hz bench readings

User's actual bench numbers: **~500Hz shift at a very tight (low Hz) clamp setting, ~250Hz shift at a 4000Hz clamp** - both much bigger than, and in the OPPOSITE direction of, the previous entry's simulated env2-weighted-average prediction (which found the shift shrinking toward zero as the limit tightened, topping out at +16.75Hz at the loosest engaged setting). That mismatch, both in magnitude and in direction, meant the previous entry's metric was measuring the wrong thing, not that the underlying finding was wrong to investigate - flagged and re-examined rather than left standing.

Built a follow-up script (`sim_slew_spectral_test.py`, scratchpad-only) that reconstructs the actual complex baseband signal a polar/EER transmitter would emit from a given freq_dev sequence (re-integrating dphi to rebuild unwrapped phase, then `exp(j*phase)`) and takes its FFT - i.e. it looks at WHERE THE ACTUAL TONES LAND spectrally, the same thing an SDR waterfall or a by-ear pitch check would show, rather than a single time-averaged summary number. Sanity check first: raw/slew-off reconstruction shows exact spectral peaks at 700.0 and 1700.0 Hz - confirms the reconstruction method itself is correct before trusting it on the slew-limited cases.

**Result: the slew limiter doesn't cleanly shift the two tones together - it smears them into a broad cluster of intermodulation products**, and how bad this gets scales directly with how tight the limit is:

- 8000Hz (loosest finite step): peaks unchanged at exactly 700.0/1700.0 Hz - no distortion (but also the limiter barely engages here, since the true null spikes only reach ~7912Hz).
- 6000Hz: peaks shift only slightly, to ~726/1726 Hz (+26Hz) - still a clean two-tone spectrum.
- 4000Hz: peaks now sit around ~749/1749 Hz (+49Hz), but NEW spurious components appear at 852, 1054, 1653 Hz that don't exist in the source signal at all - real intermodulation distortion, not just a shift.
- 2000Hz down through 250Hz: the two clean tones are essentially GONE, replaced by a dense cluster of 5-6+ closely-spaced spurious peaks spanning several hundred Hz (e.g. at 1500Hz: 482, 682, 1081, 1281, 1482, 1682 Hz all comparably strong) - there is no longer a single well-defined "tone frequency" to even measure a shift against.
- 100Hz (tightest/floor): the cluster sits mostly in the 1000-1400Hz range PLUS an entirely new, isolated low-frequency component at ~218Hz that has no counterpart in the original 700/1700 signal.

**Root cause, now properly identified**: a 700Hz/1700Hz two-tone signal has a 1000Hz beat frequency (1ms/16-sample beat period), so its LEGITIMATE (non-null) instantaneous-frequency trajectory already has to swing across a substantial fraction of the 700-1700Hz range multiple times per beat cycle - this requires real slew rates that are comparable to, not safely below, the very Hz/sample values ('{' steps from 2000Hz down to 100Hz) intended to only catch the ~8000Hz null spikes. The slew limiter has no way to distinguish "this is a null-crossing glitch" from "this is the two-tone signal's own legitimate fast content" - it is a blanket, envelope-unaware rate limiter applied to every sample - so any setting tight enough to meaningfully cut the ~8000Hz spikes is also tight enough to badly intermodulate the real two-tone content itself. This directly explains both of the user's numbers (a "shift" is really the visible/audible center of a smeared, spread-out mess of spurious tones, and it gets worse - i.e. reads as a bigger apparent shift - as the setting tightens, matching "very low clamp = ~500Hz" vs. "4k clamp = ~250Hz") and supersedes the previous entry's undersold ~17Hz estimate, which only looked at a single summary statistic and missed that the underlying spectrum was being torn apart rather than cleanly translated.

**Conclusion: the freq_dev slew-rate limiter, as currently implemented, is not a viable general fix** - not because of a small side effect, but because it fundamentally cannot distinguish the fault (isolated near-null glitches) from legitimate closely-spaced two-tone (and very plausibly real voice) content, and corrupts the latter at any setting strong enough to help with the former. Combined with the previous two entries (`'Q'` dither: real hardware regression; slew limiter: real hardware regression, now spectrally confirmed and explained), BOTH of the two mitigations tried so far attack symptoms rather than the null-crossing event itself and have made things measurably worse, not better. This makes the two already-drafted, never-implemented fix directions below (envelope-gated/targeted near-null handling, or principled phase-trajectory extrapolation through the null) look like the necessary next step rather than an alternative - they are the only proposals on the table that only touch samples actually flagged as near-null, instead of blanket-processing every sample the way both dither and slew-limiting do.

## 2026-09-17, later still: user fixed a separate two-tone frequency-generation bug on the bench; post-fix, the ~1s periodicity now reads as a "wobble"/raised wideband noise floor on the SDR rather than a narrow jumping spur - and the user proposes phase/freq-level dither as the fix. Response: likely the same losing trade as `'Q'`, and possibly not even genuine noise

User's report, verbatim summary: a small bug in the two-tone frequency generation was found and fixed on the bench today (separate from anything changed in this repo this session - not yet seen here, details requested). With it fixed, the DSP is now confirmed calculating exactly what the existing algorithm should produce (consistent with this session's own from-scratch simulation match). The already-proven ~1.000s periodicity is now heard/measured as a genuinely periodic tone repeating every second - but its SDR signature has changed character: no longer a narrow, discretely-jumping spur, now a "wobble" around the target frequency with a much higher wideband noise floor. User notes this looks similar to the `'Q'` tone-frequency dither result tested earlier today, and proposes adding a small random dither directly to phase/freq_dev (i.e. inside `ssb_dsp_process_sample()`, near the fast-trig/near-null computation) to break up the periodicity - reasoning that since the original tiny frequency-generation bug had an outsized effect, an equally small deliberate dither should be enough to fix this without corrupting real content.

**Two things flagged back to the user, not yet resolved on this side:**

1. **Mechanistically, this is the same trade as `'Q'`, which already lost on real hardware.** Any dither - on tone frequency (already tried), or directly on phase/freq_dev as now proposed - works by decorrelating WHEN a near-null glitch happens, not by reducing how big it is. The total glitch energy is unchanged; it just gets redistributed from a coherent, exactly-repeating comb into incoherent-looking broadband content. `'Q'` already demonstrated this trade sounds/measures WORSE, not better, and the slew-limiter entries above showed a related mechanism (spreading a spike's effect over more samples) also makes things worse. There is no reason so far to expect dithering the trig functions/phase directly would behave differently, since it targets the same symptom (timing/correlation) rather than the root defect (the glitch's existence/magnitude at each null). The "make it small so it doesn't corrupt real content" reasoning addresses a different risk (new distortion) than the one already observed (decorrelation itself sounding worse, regardless of how small the perturbing dither is) - `'Q'`'s own dither magnitude (+/-0.05Hz) was already tiny and still produced a worse result.
2. **The reported "much higher wideband noise floor" may not be genuine random noise at all.** This session already PROVED the raw signal is bit-for-bit periodic at exactly 16000 samples/1.000s. A signal that is exactly periodic has, by strict Fourier theory, a spectrum made of discrete lines spaced exactly 1Hz apart (the reciprocal of the 1.000s period) - not broadband noise. Most SDRs/spectrum displays use a resolution bandwidth much coarser than 1Hz, so a dense comb of many exact, fully deterministic, repeatable 1Hz-spaced harmonic lines clustered around the carrier would visually blur into what LOOKS exactly like a raised noise floor, even though it is not noise in the random/non-repeatable sense at all. Proposed cheap, decisive test before any code change: narrow the SDR's resolution bandwidth (or take a long, high-resolution FFT of a raw capture) enough to resolve sub-1Hz spacing, and check whether the "noise floor" resolves into discrete lines. If it does, this confirms the effect is the already-proven periodic comb, unmasked now that the separate tone-gen bug is fixed (that bug may have been inadvertently acting as a rough, uncontrolled dither source before today's fix, which would also explain why the SPUR CHARACTER changed from "narrow jumping" to "wobble/noise-floor" after fixing it - removing an accidental source of decorrelation and letting the underlying comb show through more purely).

**Overall position communicated to the user**: agree fully that the fast-trig near-null numerical fragility is the correct root-cause attribution (well-supported by this whole session's evidence). Recommend against another dither variant given the pattern already established (`'Q'` and, by a related mechanism, the slew limiter both made real measured results worse), and recommend instead moving to the two already-drafted, never-implemented, envelope-gated fix directions below - which touch only the near-null samples to reduce/eliminate the glitch's actual magnitude, rather than blanket-processing every sample the way dither and slew-limiting do. Still waiting on: (a) the exact nature of today's two-tone frequency-generation fix, so it can be reflected here and folded into the verification simulation; (b) the RBW/comb-vs-noise diagnostic result; (c) user's decision on which of the two candidate fixes to design first.

## 2026-09-17, later still: the comb hypothesis is FALSIFIED at 0.05Hz real RF resolution - the broadband content is genuinely broadband, not an unresolved 1Hz comb, which reopens the "something downstream of the core DSP" mystery from much earlier today

Two clarifications from the user settle open questions from the previous entry:

1. **"The main change today" is the already-known 2026-09-17 NCO fix**, not a separate, second bug - the user pasted its exact `config.h`/`test_signals.cpp` doc comment: switching `generate_twotone_sample()`'s tone1 (and tone2 when undithered) from an accumulate-based phase generator (`phase += 2*pi*f/Fs` every tick, which drifts under float32 rounding) to a wrapping-sample-index recompute-FRESH-every-tick scheme (exact, bit-for-bit periodic at `SAMPLE_RATE_HZ`, the mechanism this whole session's periodicity proof rests on). This confirms the "small accumulation error, big effect" framing from two entries ago: the OLD accumulate-based generator's per-sample float32 rounding was the "small error," and its effect was to very slightly decorrelate the near-null glitches cycle-to-cycle - just as `'Q'` or a real dither would - without anyone intending it as a mitigation.
2. **Directly falsifies the previous entry's "unresolved 1Hz comb" hypothesis.** The user can resolve the real RF spectrum to 0.05Hz - twenty times finer than the 1Hz spacing an exact 1.000s-periodic comb would produce - and it does NOT show a comb dominating; it shows genuine broadband content. Also confirmed: **the OLD (imprecise/accumulate-based) tone generator produced a measurably PURER tone with a LOWER noise floor**, despite jumping around, than the NEW (mathematically exact) one does. That is the opposite of what "the digital core DSP's periodicity is the audible root cause" would predict - making the digital math MORE exact made the real transmitted signal's spectral purity WORSE, not better.

**This is a real, take-it-seriously falsification, not a measurement quibble - logged honestly rather than argued around.** Since the raw `freq_dev`/`envelope` values are now triply confirmed exactly periodic in the digital domain (from-scratch simulation, 40-microsecond cross-capture agreement, then bit-exact same-capture agreement - three separate entries above), any exactly-periodic input to a fixed, deterministic downstream chain would have to produce an exactly-periodic (1Hz-comb) output, by basic Fourier-series necessity - no exception. A real, fine-resolution measurement showing genuine broadband content instead means the actual TRANSMITTED signal is demonstrably NOT exactly periodic, even though the DIGITAL freq_dev/envelope numbers driving it are. The gap between those two facts can only live in whatever sits between them: envelope PWM mapping/output, `relative_delay_apply()`, `envelope_interp_on_full_tick()`, or the AD9851 chip's own analog behavior (SPI transfer timing, its internal PLL) - **exactly the "candidate 2" list already named, and never tested, in the "733/1700 `'F'` capture... settles the tension" entry much earlier today.** That list was flagged then as one of two explanations for the separate ~0.485s finding; it now looks like the leading explanation for THIS finding too, and is a more promising target than anything inside `ssb_dsp_process_sample()` itself, whose own output has now been checked about as thoroughly as digital simulation and hardware agreement can check it.

**A plausible, coherent story tying every piece of today's data together**: the old accumulate-based tone generator's tiny per-sample float rounding meant the digital core's near-null glitches never repeated bit-for-bit identically cycle to cycle - which, whatever the real broadband-noise source downstream turns out to be, would have ALSO prevented that downstream source's own contribution from lining up identically every cycle if it depends at all on the exact digital signal's timing/values (e.g. relative-delay/interpolation effects that are sensitive to exact sample values). Today's NCO fix removed that incidental smearing, letting the downstream artifact - whatever it is - repeat and/or dominate more cleanly, which is consistent with "jumping" becoming "wobble," and is also consistent with the noise floor rising rather than resolving into a comb (if the downstream mechanism itself has a genuinely stochastic/analog component - e.g. real SPI/PLL jitter - rather than being purely deterministic, its OWN contribution would be real broadband noise, not a comb, no matter how exactly periodic the digital freq_dev driving it is).

**Requested from the user, not yet received**: the actual capture/recording behind the 0.05Hz measurement (IQ or long audio capture), so this can be checked directly with the same FFT tooling already used on `700-1700_Preset1_V2.wav` earlier today, rather than relying on a description of what the SDR display showed.

**Recommended next step, superseding the previous entry's "design the targeted near-null fix" recommendation**: before spending more design effort inside `ssb_dsp_process_sample()` (whose output is now unusually well-verified), investigate the downstream candidate list above - specifically whether `relative_delay_apply()`/`envelope_interp_on_full_tick()`/PWM output introduces anything that depends on exact per-sample digital values (which the NCO fix would have changed the timing/decorrelation of) or anything with its own real, non-deterministic timing (SPI/PLL jitter), which would explain genuine broadband content that a purely-digital periodicity argument cannot.

## 2026-09-17, later still: real SDRuno IQ captures analyzed directly - the "broadband noise" IS real comb structure, but it's roughly 25-30dB stronger than what the already-verified core DSP alone predicts, which points the dominant cause downstream after all (refining, not reversing, the previous entry)

User supplied two real IQ captures for direct analysis (SDRuno, 388888Hz stereo I/Q, ~18.4s/~18.7s, resolution 0.0544Hz/0.0535Hz - matching the "0.05Hz" resolution claimed): `SDRuno_20260917_192907Z_14175kHz.wav` (Preset 1, slew off) and `SDRuno_20260917_193106Z_14175kHz.wav` (Preset 3). Combined channels as a complex I+jQ baseband signal and took a full-length Hann-windowed FFT of each - the right way to actually check the previous entry's comb-vs-noise question with real data instead of a description of what a display showed.

**Both captures confirm the genuine two-tone signal**: Preset 1's tones sit at 25707.3/26707.3 Hz (relative to the 14175kHz dial), Preset 3's at 25689.3/26689.3 Hz - both pairs spaced exactly 1000.0 Hz apart, as expected, and implying a real suppressed-carrier position around 14.200 MHz.

**Preset 1 width measurements** (relative to each tone's own peak): -3dB width ~5Hz, -6dB ~15Hz, -10dB ~19Hz, -20dB ~26Hz, but **-30dB width balloons to ~204-214Hz** - a real, large, measurable skirt, consistent with the user's "~150Hz wide approx" description if eyeballed around that transition zone. The near-tone noise floor (25.2-25.5kHz) measures ~14dB higher than the noise floor far from the signal (20-23kHz) - a real, localized degradation, not just uniform receiver noise across the whole band.

**Zooming to the native 0.054Hz resolution shows this is NOT smooth continuous noise - it resolves into a real, discrete comb** of sharp, irregularly-but-repeatedly-spaced peaks (visually obvious in Preset 1's spectrum plot; a spectral autocorrelation over a comb-heavy region confirms a strong periodic signature). This **refines rather than fully reverses the previous entry's falsification**: there IS real comb/periodic structure here, contradicting a literal "random broadband noise" description - but the previous entry's specific guess (an unresolved 1Hz-spaced comb from the whole-second periodicity, too fine for typical displays) is not what's dominating what's actually visible; the dominant visible teeth are spaced far coarser than 1Hz.

**The decisive new step: simulated the ALREADY-PROVEN-EXACT core DSP signal in isolation (no downstream stages at all) and compared its own spectrum at the same kind of resolution.** Reconstructed the raw (slew-off) two-tone `freq_dev`/`envelope` signal from the existing verified simulation over exactly one proven 1.000s period (giving native 1Hz bins - the finest a truly periodic signal can even use, since one period contains all its information), applied the REAL envelope (not unit amplitude, unlike the earlier slew-limiter spectral test), and looked at the resulting spectrum near the 700Hz tone. Result: a single sharp line at exactly 700Hz sitting on top of a smooth, slowly-varying, low floor around -53 to -60dB relative to the peak - genuinely low-level and broadband-*looking*, consistent with the near-null glitches, but critically **NOT showing the sharp, discrete, ~100-200Hz-spaced comb teeth the real hardware capture shows reaching -25 to -30dB** - a roughly **25-30dB gap** between what the verified-correct core DSP algorithm alone predicts and what real hardware actually measures.

**Conclusion, refining the previous entry rather than contradicting it**: the core DSP does contribute a real, genuine, low-level broadband-looking floor from the near-null glitches (now directly demonstrated, not just argued from Fourier theory) - but it cannot explain the bulk of what's actually seen on the bench, which is roughly 1000x higher in power (25-30dB) and has its own distinct, coarser comb structure the core DSP's own proven-exact signal does not produce. This reinstates the downstream-chain recommendation on a solid quantitative footing this time: `relative_delay_apply()`, `envelope_interp_on_full_tick()`, the envelope PWM output mapping, or the AD9851's own analog/SPI/PLL behavior remain the leading candidates for the DOMINANT contribution, with the core DSP's near-null mechanism now confirmed as a real but comparatively minor contributor.

**Preset 3, by contrast, shows a visually smoother, broader floor with no obvious discrete comb teeth** (same 1000Hz tone spacing confirmed, same general skirt shape, but no sharp repeated spikes the way Preset 1 shows) - worth noting as a real, measured difference between presets. Consistent with Preset 3 enabling more processing (EQ/compressor/AGC, whatever else it turns on beyond Preset 1's "everything off") that may be smearing/decorrelating whatever downstream mechanism produces Preset 1's sharper comb - the same general "extra processing decorrelates a coherent artifact into smoother-looking noise" pattern already seen with the old imprecise tone generator and with `'Q'` dither, just here possibly happening as a side effect of Preset 3's own processing rather than anything deliberately added for this investigation.

**Not yet done**: precisely characterizing the real comb's tooth spacing (autocorrelation suggested slow clustering near multiples of ~100Hz, not a single clean value - noisy enough that it needs a longer/cleaner capture or a different analysis method to pin down); connecting that spacing to a specific candidate mechanism (e.g. `envelope_interp_on_full_tick()` or `relative_delay_apply()`'s own update rate, if either operates on some sub-1-second interval that could produce this signature); and analyzing Preset 3's smoother floor quantitatively rather than just visually. Scripts (`sim_comb_zoom_700.png` output script, real-capture analysis) kept in scratchpad only, not committed.

## 2026-09-17, later still: Preset 1's own stored settings rule out relative_delay/gdeq/ampeq/envelope_interp as live confounds for the comb; user's AM comparison and D-correction/PWM experiment further narrow away from the analog envelope chain; two concrete new AD9851-path candidates raised - FQ_UD's own latch-edge margin, and this project's already-documented Fs/wakeup jitter

Went back to check an assumption from the previous entry's candidate list before handing it to the user as the leading remaining suspect: is `relative_delay_apply()` really "always active" the way it was being treated? Read `settings.h`'s actual stored preset table. It is not, under Preset 1 specifically. `settingsPresets[1]` ("TwoTone Base") sets `relative_delay_samples = 0.00` (at exactly zero, `interp_ring()`'s fractional blend degenerates to an identity pass-through, not a live blend across anything), `env_gdeq_enable = false`, `envelope_interp_enable = false`, `env_predistort_enable = false`, and - via C's zero-fill of the trailing struct fields this preset's positional initializer doesn't specify - `env_ampeq_enable = false` and `env_ampeq_shelf2_enable = false` too. So every envelope/phase-conditioning stage on the downstream candidate list from the last two entries is either off or a mathematical no-op under Preset 1. This removes `relative_delay_apply()` as the leading candidate - a correction to the previous entry, not an extension of it.

The user's own independent bench evidence points the same direction from a different angle: the ~100/200Hz-ish comb-adjacent noise isn't present on the AM test signal, which rules out generic mains/PSU/ground coupling (that would show on any signal type, not just two-tone); and switching `env_predistort_enable` off while forcing `env_pwm_scale` to zero and `env_pwm_offset` to mid-scale - exercising the one envelope-path lever that IS still live under Preset 1 about as hard as it can be exercised - produced interesting IMDs but left the main tone pair and its noise "very similar to before." Between the settings.h finding and this experiment, the entire envelope/correction-chain layer and the analog PWM path are now substantially ruled out for Preset 1's specific comb. What's left live is: the bare `env_pwm_offset`/`env_pwm_scale` affine mapping (already exercised above) and the AD9851 SPI/DDS output path.

Before re-offering the AD9851 BS170/level-shifter edge-timing theory as a fresh candidate, checked this project's own history: it was already flagged 2026-09-16, already hardware-tested today via the push/pull driver fit, and came back "no visible improvement" for the closely related ~153.6s STUCK/RECOVER cycle and a confirmed real ~50Hz hands-off jump (later found clean, not noisy). Confirmed with the user that today's Preset 1 SDRuno capture was taken WITH the push/pull drivers already fitted - so that negative result applies directly to the hardware behind this exact capture, not just to a different symptom set on different hardware. Not re-proposing that specific mechanism (many DATA bits flipping faster than a level shifter can settle) without new justification.

The user raised two more specific mechanisms in the same general area, both genuinely distinct from the already-tested DATA-bit-settling theory:

1. **FQ_UD's own latch-edge timing.** New data is only actually transferred into the AD9851's internal frequency/phase registers on FQ_UD's LOW-to-HIGH edge (`ad9851_set_frequency()`'s bit-bang tail, `AD9851.c:575-588`: `fast_gpio_clr(handle->pin_fqud)` is the latch transition). Checked the code: this edge currently follows the last W_CLK bit-clock transition with ZERO added margin, because `AD9851_BITBANG_EDGE_DELAY_ENABLED` is 0 - and that single flag gates out all three `ad9851_edge_delay()` call sites at once, including the FQ_UD-latch-specific one (line ~587). Worth surfacing: this wasn't always bundled together. The 2026-09-10 entry (`moving_forward_notes.md`) treated the FQ_UD final-edge delay as its own, separately-justified, "cheap" margin - it was the ONE delay kept in place when the per-bit DATA-settle delay was trimmed back that same day for a real-time budget regression (`max_busy_us` 58 -> ~51-52 against the 62.5us tick budget). It only got swept into the blanket disable the following day (2026-09-11), alongside the per-bit delays, pending a re-scope of the real drive-signal toggle rate that was then superseded by other work and never completed. So FQ_UD's own latch margin has never actually been tested in isolation - only ever bundled with, and therefore confounded by, the per-bit DATA-settle delay's own cost/benefit tradeoff - and never tested at all since the push/pull driver fit changed the whole edge-speed picture.

2. **Real Fs/wakeup jitter.** This project already has an independently documented, already-quantified scheduling-jitter mechanism: `[timing] max_gap_us`, currently ~71-78us against `dsp_task`'s 62.5us/16kHz tick budget, tracked to cross-core cache-stall/ADC-ISR-priority contention (`moving_forward_notes.md`'s 2026-09-10/11 entries). This is the "jitter on the ESP32 Fs" the user is referring to as already known. Worked through why it's a plausible contributor to THIS symptom specifically: the AD9851 free-runs its own DDS core between FTW updates (already noted in the 2026-09-09 AM-jitter entry), so a jittered WRITE time doesn't create a direct phase error the way a jittered PWM/DAC voltage sample would - but it does mean the real-world duration each FTW value is actually held for differs from the exact 1/16000s the DSP math assumes it gets. The size of the resulting distortion is proportional to how much `freq_dev` is changing at the moment of the jittered write - the same delta(t)*rate-of-change argument already used (and, via a real mechanism, validated) on the AM/envelope side of this project's 2026-09-09 entries, even though that specific finding later turned out to be external/RX-AGC rather than internal. Near a null, `freq_dev` is changing by up to several thousand Hz tick-to-tick (the same near-null atan2 noise already characterized elsewhere in this file) - exactly the condition where write-timing jitter of this kind would have its largest effect, and exactly where the comb/warble is observed.

These two are not mutually exclusive and could stack: FQ_UD signal-integrity margin affects WHEN the latch edge electrically completes at the chip; wakeup/scheduling jitter affects WHEN the write is issued by firmware in the first place.

No firmware changed yet. Concrete, low-risk next tests identified without further code changes: (a) re-enable only the FQ_UD-latch `ad9851_edge_delay()` call site (leave the per-bit ones as they are) and re-run the hands-off/two-tone comb comparison; (b) scope FQ_UD directly relative to the last W_CLK edge, now with the push/pull drivers fitted, since the rise-time numbers behind `AD9851_BITBANG_EDGE_DELAY_ENABLED`'s history all predate that hardware change; (c) correlate `max_gap_us`/per-tick wakeup timing against a capture that shows the comb, to see whether comb-tooth timing lines up with wakeup-jitter events rather than with the two-tone signal's own content - a real write-jitter mechanism should NOT track carrier/tone-frequency changes the way an AD9851-intrinsic phase-truncation spur would, a distinguishing test worth keeping in reserve. Awaiting the user's steer on which to try first.

## 2026-09-17, later still: added a firmware toggle ('O') to revert to the pre-NCO-fix "pure but jumping" two-tone phase generator on demand, for a direct sanity-check A/B without a separate reflash

User asked whether the old accumulate-based two-tone phase generator (from before today's NCO fix, commit `a32c50c`) was left in place, wanting to revert to it as a sanity check and confirm nothing else relevant had changed since the "pure but jumping" era. Checked directly: it was NOT kept as a fallback - `a32c50c` replaced it outright for tone1 (always) and tone2 (whenever dither is off); the only surviving trace of the old `phase += increment; if (phase > two_pi) phase -= two_pi` pattern is tone2's dither branch, which is a separate, deliberate code path, not a revert switch. Also checked every commit since `a32c50c`: `ssb_dsp.c`, `AD9851.c`, `relative_delay`, the envelope/gdeq/ampeq/interp files, and `settings.h`/presets all have zero changes since the fix - everything else that changed is documentation, or diagnostics/capture instrumentation (`diagnostics.cpp`/`.h`, `serial_commands.cpp`'s new `'F'` capture command, one causal experiment tried and explicitly reverted the same session) that doesn't touch the signal or RF/envelope path. One caveat flagged: `TWOTONE_BAND_PRESETS` picked up one extra temporary entry (700.37/1700.61) since then, which doesn't change any existing pair's frequency but does mean `'T'` now cycles through one more band than before.

Per the user's request, added a runtime toggle (**`'O'`**, mirroring `'Q'`'s pattern) rather than pointing them at a separate pre-fix build, since it lets the comparison happen live on the current build without a reflash - useful given how much of today's investigation is anchored to comparing these two states. Implementation: a new `s_twotone_legacy_phase_enable` flag in `test_signals.cpp` (off by default, so plain `'t'/'T'/'R'/'Q'` behavior is completely unchanged unless `'O'` is pressed); when ON, tone1 always uses the old accumulator and tone2 uses it too whenever `'Q'` dither is off (dither's own tone2 path already used the accumulator both before and after the fix, so `'O'` has no effect there). `s_tone_sample_index` keeps advancing regardless of which generator is active, so toggling `'O'` back off picks the exact-recompute path back up cleanly rather than resuming from a stale index. A one-time phase discontinuity on the tick where the switch happens is expected and accepted, same convention already used for `'Q'`'s own on/off transition and for `'T'` band changes. Added the matching boot-banner line and a doc comment in `test_signals.h` explaining the rationale (this is a deliberate revert to a KNOWN-WORSE generator for comparison purposes, not a recommendation). Committed; not yet bench-tested.

## 2026-09-17, later still: 'O' does NOT reset the tone generator to initial conditions; a new Preset 3/ADC-off capture shows a dramatically cleaner noise floor - strong direct support for the Fs/wakeup-jitter candidate

**'O' reset question, answered directly from the code**: no, `test_signals_set_twotone_legacy_phase_enabled()` is a plain flag assignment (`s_twotone_legacy_phase_enable = enable;`), nothing else. It does NOT reset `s_tone1_phase`/`s_tone2_phase`/`s_tone_sample_index` to zero on either transition. Toggling 'O' just switches which update rule computes the NEXT phase from whatever phase/index state the generator is already in - by design (see test_signals.h's doc comment and the previous entry), so the exact-recompute path can resume cleanly off the still-advancing `s_tone_sample_index` the instant 'O' goes back off, rather than jumping to some other reference point. The tradeoff is the already-documented one-time phase discontinuity on the switching tick itself, for whichever tone(s) actually change generators.

**New capture analyzed: `SDRuno_20260917_204619Z_14175kHz.wav`, "Preset 3 with ADC off"** - user's stated low-noise benchmark. Same complex-I/Q FFT method as the two 19:2x captures. Results, side by side:

| capture | tone pair (dial-rel.) | -30dB width | far floor (8-12kHz) | near-tone floor (400-900Hz off) | near-far delta |
|---|---|---|---|---|---|
| Preset 1, slew off (19:29) | 25707.3/26707.3 Hz | 3.2 Hz | -89.6 dB | -62.1 dB | +27.5 dB |
| Preset 3 (19:31) | 25689.3/26689.3 Hz | 10.5 Hz | -84.5 dB | -49.9 dB | +34.7 dB |
| **Preset 3, ADC off (20:46)** | 25700.5/26700.4 Hz | **0.7 Hz** | **-100.6 dB** | **-88.1 dB** | **+12.4 dB** |

(Re-ran the earlier two captures through the corrected peak-finder for this table - the previous entry's numbers for Preset 1/Preset 3 are confirmed consistent; an initial pass on this new file mistakenly searched a +/-3000Hz-of-dial-center window instead of near the actual ~25.7kHz tone location and had to be corrected.)

**This is a large, unambiguous difference, not a marginal one.** With the ADC off, the near-tone floor drops 38dB relative to Preset 3 with the ADC running (a ~6300x reduction in power) and 26dB relative to Preset 1 (~400x); the far floor drops 16dB and 11dB respectively; the -30dB skirt width shrinks from 10.5Hz/3.2Hz down to 0.7Hz - close to what the core-DSP-only simulation predicted for the near-null glitch floor alone, several entries ago. Whatever "ADC off" changed, it removed the dominant part of what's been driving this whole noise-floor/comb investigation.

**This lines up directly with the Fs/wakeup-jitter candidate raised in the previous entry, not coincidentally.** This project's own diagnostics already attribute a chunk of `max_gap_us` scheduling jitter specifically to ADC-ISR-priority contention (`moving_forward_notes.md`'s 2026-09-10/11 entries: "the `dac_task_enabled` regression, the ADC-ISR priority collision - same family, different trigger"). Removing the ADC removes that contention source. If write-timing jitter proportional to `freq_dev`'s rate of change (the mechanism worked through last entry) is the dominant contributor, taking away ADC-ISR contention should produce close to exactly what's seen here - a large, broad reduction in floor level with the comb/skirt shrinking toward the core-DSP-only prediction, not a narrow spectral change. This doesn't yet rule out the FQ_UD-latch-margin candidate (both could still be contributing), but it's the first piece of evidence in this whole investigation that moves a candidate from "plausible mechanism" to "matches a real, large, controlled A/B."

**Not yet established**: exactly HOW "ADC off" was achieved on the bench (a firmware change disabling ADC sampling/ISR entirely, vs. a physical disconnect, vs. something else) - this matters for interpreting the result precisely, since the project's own existing comments say the ADC "always runs... no longer conditional" in the current mainline firmware, so this wasn't a stock toggle. Also not yet done: pulling `[timing] max_gap_us` from this same capture's session (if logged) to confirm directly that wakeup jitter actually dropped, rather than inferring it only from the RF result. Recommended next step: get `max_gap_us`/`overruns`/`late_ticks_total` for both the ADC-on and ADC-off conditions on the same preset, to close the loop between "jitter went down" and "noise floor went down" causally rather than by correlation alone.

## 2026-09-17, later still: CORRECTION - the ADC was already disabled in ALL recent captures, including the two "noisy" ones; the previous entry's ADC-off/wakeup-jitter causal story is retracted

User clarified: every recent capture using the new exact-recompute ("truly periodic") tone generator - including both 19:2x captures analyzed as "noisy" (Preset 1 slew-off at 25707.3/26707.3Hz, and Preset 3 at 25689.3/26689.3Hz) - was ALSO taken with the ADC disabled, the same condition as the new 20:46 "low noise benchmark" capture. ADC state was constant across all three captures, not the variable distinguishing the clean one from the noisy ones.

**Retracting the previous entry's central claim.** The ~26-38dB floor improvement and the "matches a real, controlled A/B" framing for the Fs/wakeup-jitter candidate does not hold up - there was no ADC-on-vs-off comparison actually being made. The wakeup-jitter candidate goes back to "plausible mechanism, not yet tested" status, alongside the FQ_UD-latch-margin candidate from two entries ago. Flagging this plainly rather than quietly dropping it, since it was written up and committed as a decisive finding last entry.

**This reopens, rather than closes, the question the previous entry thought it had answered**: what actually differs between the 19:31 Preset 3 capture (-49.9dB near-tone floor, 10.5Hz -30dB skirt) and the 20:46 Preset-3-labeled capture (-88.1dB near-tone floor, 0.7Hz skirt) - a real, large, still-unexplained ~38dB gap between two nominally-identical preset selections? Since `settingsPresets[]` reloads every DSP-side lever identically each time preset 3 is selected (per the settings.h read two entries ago), two captures of "preset 3" should read close to identically unless something else changed in between. Candidates worth checking with the user directly rather than guessing further: (a) a runtime toggle changed between the two captures that a preset reload wouldn't undo even at the same preset number - relative_delay retuned via `'['`/`']'`, master gain, or a gdeq/ampeq/predistort state left engaged from other testing since boot; (b) something on the RECEIVER side rather than the DUT - SDR RF gain/attenuation, AGC mode, antenna/dummy-load connection, or general band noise differing between the 19:3x and 20:4x recording sessions, which could produce a floor-level difference this size with the DUT's actual output unchanged. Asked the user what else, if anything, changed between these two specific captures - needs pinning down before any further conclusion is drawn from the "low noise benchmark" comparison.

**Mechanism confirmed while writing this up**: the user identified `ADC_CAPTURE_ENABLED` (config.h:142, currently `1` in this checkout) as the actual flag - a compile-time `#if` gating whether `init_adc()`/the ADC continuous DMA driver starts at all (`ssb_mic_test.ino:551,909,1133`), not a runtime toggle. All of today's "truly periodic tone source" testing (both the noisy 19:2x captures and the 20:46 benchmark) was built with this at `0`. Worth keeping attached: this flag has a real, decisive, already-documented hardware effect elsewhere in the project - the 2026-09-02 entry (`ssb_mic_test_commands.md`) found `ADC_CAPTURE_ENABLED=0` "kills [a] 5kHz pulse on GPIO13... the ADC continuous driver's DMA/ISR activity is the source." So the ADC DMA/ISR is a real, confirmed noise mechanism in this hardware generally - it's just already been OFF for every capture in today's specific comparison, so it can't be what's making the 19:31 and 20:46 Preset 3 captures differ from each other. A genuine ADC-on-vs-off A/B (flipping this flag and reflashing, keeping everything else fixed) remains a worthwhile, still-open test for the wakeup-jitter hypothesis generally - it just isn't what today's existing captures already show.

## 2026-09-17, later still: RESOLVED - the 19:31 vs. 20:46 Preset 3 captures differed ONLY in which tone generator was active; the old (pre-NCO-fix) generator's own per-cycle float rounding is the likely decorrelation mechanism, and it's a much smaller, more surgical perturbation than 'Q' dither

User confirmed directly: the 19:31 ("noisy," new exact-recompute generator) and 20:46 ("low noise benchmark") Preset 3 captures were identical in every other respect - same preset, same everything else - the ONLY thing that differed was which tone generator was in use. Not a runtime toggle left engaged, not a receiver-side setting, not ADC state (already ruled out two entries ago) - just old generator vs. new generator, confirming the 20:46 capture used the pre-fix accumulate-based generator. This is now a genuinely clean, single-variable, ~38dB result: reverting to the OLD, less-exact tone generator makes the near-tone floor and comb/skirt dramatically better, not worse, exactly matching the qualitative "old generator sounded purer despite jumping" observation from several entries ago, now with hard numbers behind it. The user also confirmed the newly-added `'O'` toggle reproduces this exact A/B live, with no other change needed - closing the loop on why that toggle was requested in the first place.

**Working theory for the mechanism, offered with appropriate hedging - not yet confirmed by simulation or further hardware test.** The old accumulator's previously-quantified drift (~+/-2e-4Hz systematic bias over a 500s run) is far too slow to matter within an 18s capture on its own - by itself it can't be the explanation. But the OLD generator's phase isn't just slowly drifting - each tick's phase carries forward a *slightly different* float32 rounding residue than the exact-recompute generator's fresh-every-tick computation does, because `phase += increment; phase -= two_pi` accumulates history-dependent rounding noise on top of the systematic bias, while `fmodf(index * f/Fs, 1.0f)` recomputes from scratch every time with no memory at all. Two-tone nulls recur roughly every 16 samples (per the AD9851 null-proximity analysis from earlier today), and `fast_atan2`'s sensitivity right at a null is extreme (dphi implicitly scales with 1/envelope as envelope->0) - so even a per-cycle rounding-noise perturbation many orders of magnitude smaller than `'Q'` dither's deliberate +/-0.05Hz wobble could plausibly be enough to shift which side of a hair-thin decision boundary a given near-null sample lands on, decorrelating that one cycle's glitch from the next. Under the exact generator, by contrast, every cycle reproduces the identical rounding outcome at the identical null, forever - which is exactly what makes it a coherent comb rather than smeared noise (the original "exact periodicity -> exact comb, by Fourier necessity" argument from several entries ago, now with a concrete "why the periodicity is bad" story attached to it).

**This reconciles, rather than contradicts, the earlier negative `'Q'` dither result.** Both mechanisms "decorrelate the periodicity," but at wildly different scales and character: `'Q'` is a large (+/-0.05Hz), deliberately continuous, audible-scale wobble applied only to tone2 - big enough to have its own audible/spectral footprint independent of what it does to null timing. The old generator's own rounding noise is apparently a vastly smaller, more surgical perturbation that (per this result) is enough to break the coherent comb without introducing a wobble of its own. If this theory holds, the right fix direction is NOT reverting to the buggy accumulator (that reintroduces a real, if slow, systematic drift bug the NCO fix correctly closed) and NOT `'Q'`-style dither at its current magnitude - it's finding the smallest perturbation that reproduces the old generator's decorrelation benefit without its drift liability or `'Q'`'s audible cost.

**Recommended next steps, none yet run**: (1) a live `'O'` A/B on the bench, now that it's confirmed to reproduce this cleanly, to build confidence this holds up beyond a single pair of captures (different bands via `'T'`, different runs); (2) a small simulation quantifying the OLD accumulator's actual per-tick rounding-noise magnitude (distinct from its already-quantified slow systematic drift) directly from the two implementations, to check whether it's plausibly the right order of magnitude to matter at a null; (3) if so, testing whether `'Q'` dither turned down to a MUCH smaller magnitude (well under +/-0.05Hz) reproduces the old generator's benefit without its own audible cost - a direct, testable prediction of this theory.

## 2026-09-17, later still: simulation FALSIFIES the "old generator's per-tick rounding noise decorrelates near-null glitches" theory; the real mechanism must be downstream, in Preset 3's active gdeq/ampeq/relative_delay/predistort stack - not in the core DSP

Built the direct test: an exact port of the old accumulate-based generator (`phase += increment; if (phase > two_pi) phase -= two_pi`, both tones starting at 0.0f, matching real firmware statics) alongside the already-verified exact-recompute generator, both run through the full verified `ssb_dsp_process_sample()` port over 20 concatenated 1-second cycles (`sim_old_vs_new_gen.py`, scratchpad-only).

**Finding 1 - the theory's own premise doesn't hold.** The OLD generator's cycle-to-cycle `freq_dev` difference is NOT noise-like at all: it's a smooth, monotonically DECAYING sequence (33.6, 11.1, 5.5, 3.3, 2.2, 1.58, 1.19Hz... down to 0.20Hz by cycle 19, after an atypical first-cycle transient from the zero-phase startup). That's the signature of the already-known slow, deterministic systematic drift (the ~2e-4Hz/500s bias from several entries ago), not a new per-cycle random rounding-noise mechanism. The EXACT generator's own cycle-to-cycle diff is a clean 0.000000Hz throughout, as expected.

**Finding 2 - the simulated effect is the WRONG SIGN and orders of magnitude too small.** Reconstructing the complex baseband signal and taking its spectrum (same method as the earlier slew-limiter/comb work) over all 20 cycles: OLD generator's simulated near-tone floor is -184.3dB, EXACT generator's is -226.5dB - OLD is 42dB WORSE (noisier) in simulation, not 38dB better as measured on real hardware. Both numbers are also far below anything physically meaningful (real captures sit around -50 to -100dB) - at this depth the simulation is measuring its own FFT/window/float64-reconstruction noise floor, not anything a real receiver could ever see. **This falsifies the specific theory from two entries ago**: whatever makes the old generator quieter on the bench is not happening in `ssb_dsp_process_sample()` itself (tone generation, Hilbert FIR, `fast_atan2`/`fast_sqrt`, the null-bias diagnostics) - that stage is functionally identical (and if anything very slightly worse for the old generator) between the two cases.

**Redirected conclusion, better-grounded than the retracted one**: the real ~38dB effect must live downstream of the core DSP - and Preset 3 (unlike Preset 1, see two entries ago) has essentially its ENTIRE downstream stack live: `env_gdeq_enable=true` (candidate B), `env_ampeq_enable=true` AND `env_ampeq_shelf2_enable=true` (both shelves), `env_predistort_enable=true`, and a real `relative_delay_samples=2.00` (an exact integer, itself already flagged in this project's history as collapsing `interp_ring()`'s blend). A mechanistically plausible story: gdeq's all-pass sections and ampeq's shelf biquads are LTI filters, and an LTI filter's resonant/peaking response builds up MORE energy the longer an exciting frequency dwells exactly on it. The EXACT generator's near-null glitches recur at bit-for-bit identical instants every single 1.000s cycle forever - a perfectly rigid comb that can sit on a resonance indefinitely. The OLD generator's glitches recur at a very slowly, smoothly SLIPPING instant each cycle (Finding 1's decaying-drift signature) - a near-comb whose lines never dwell exactly on any one frequency for long, which could measurably reduce how much any downstream resonance gets excited, without needing any "randomness" at all. This is speculative and not yet simulated with the actual downstream stages - flagging it as the leading hypothesis, not a confirmed result.

**User is independently running the single most decisive next test right now**: `'O'` A/B on Preset 1 instead of Preset 3. Preset 1 has none of gdeq/ampeq/predistort/relative_delay active (confirmed two entries ago), so this is a clean discriminator: if old-vs-new makes little/no difference on Preset 1, that CONFIRMS the downstream stack (not the core DSP) as the site of the real mechanism, consistent with both of today's simulation findings above. If Preset 1 shows a comparably large difference anyway, that would contradict this entry's redirected theory and point back at something in the core DSP/AD9851 path this simulation isn't capturing. Awaiting the result.

## 2026-09-17, later still: the decisive Preset 1 test comes back - old-vs-new generator gap is "basically the same" there too, FALSIFYING the downstream-stack (gdeq/ampeq/predistort/relative_delay) theory; points back at something active on every preset

User ran the `'O'` A/B on Preset 1 as planned. Result: "basically the same" as Preset 3, except the settled "off" frequency reads -10Hz instead of -18Hz - which the user correctly attributes to the already-well-documented relationship where `relative_delay`'s own value tunes the observed offset frequency (Preset 1's `relative_delay_samples=0.00` vs. Preset 3's `2.00`, per the settings.h read several entries ago) - an expected difference, not new information about the old-vs-new question itself.

**This falsifies the previous entry's redirected theory.** Preset 1 has none of gdeq/ampeq/predistort/envelope_interp active, and - double-checked directly in `relative_delay.cpp` while writing this up - `relative_delay_apply()` at `delay=0.00` is a bit-exact identity pass-through: `interp_ring()` with `back=0.0` resolves to `i0=0, frac=0.0`, so the returned value is `ring[idx0]*1.0 + ring[idx1]*0.0`, exactly the just-written current sample, no blending at all. So Preset 1 genuinely has ZERO live downstream conditioning of any kind - yet the old-vs-new generator gap persists essentially unchanged. The LTI-resonance-in-gdeq/ampeq hypothesis from last entry is therefore wrong (or at least not the dominant mechanism) - it predicted this exact test would show little/no gap on Preset 1, and it didn't.

**This redirects attention to whatever IS active on every preset regardless of DSP-stage configuration**: the AD9851 SPI/FQ_UD write path (both the already-tested-and-ruled-out BS170 DATA-bit-settling theory, and the NOT-yet-tested FQ_UD-latch-margin candidate from several entries ago), Fs/wakeup jitter (also not yet actually tested - ADC has been off in every capture so far, so that variable is still completely unexplored), or the basic PWM/RC/BS170/RSET analog envelope path (partially discounted via the earlier D-correction/zero-scale experiment, but that wasn't the same old-vs-new generator comparison, so it isn't airtight against this specific finding). Of these, FQ_UD-latch-margin is the cheapest to test directly: currently it's bundled into `AD9851_BITBANG_EDGE_DELAY_ENABLED` alongside the per-bit DATA-settle delays, which have a real measured real-time-budget cost (~13-14us) unrelated to what's being tested here. Recommending splitting FQ_UD's own edge-delay call site into its own independent flag so it can be A/B'd via `'O'` without paying that unrelated cost - proposed to the user, not yet implemented.

## 2026-09-17, later still: DECISIVE - the two generators' raw output statistics genuinely differ, in exactly the way that matters for the AD9851 write path; EXACT generator reliably drives a ~10226Hz single-tick jump at the SAME instant every cycle, OLD generator mostly stays well below that

User's pushback was right: comparing an idealized post-hoc FFT reconstruction (previous two entries) isn't "what matters" - what matters is the actual per-tick `freq_dev` OUTPUT stream itself, since that's literally what gets written to the AD9851 every tick. Built `sim_old_vs_new_output_stats.py`: both generators run through the verified DSP over 60 concatenated cycles (960,000 ticks each), comparing the tick-to-tick `|freq_dev[n] - freq_dev[n-1]|` step - the same quantity this project's own real diagnostics already track as `max_freq_dev_step`, and the one directly responsible for how many AD9851 DATA bits flip on a single SPI write.

**The two generators' step distributions genuinely differ, and cross over.** At moderate-to-high thresholds the OLD generator has MORE big steps (count >3000Hz: 1945/cycle EXACT vs 2000/cycle OLD; >8000Hz: 735/cycle EXACT vs 1860/cycle OLD) - but at the most extreme thresholds this flips hard: >8500Hz is 332/cycle for EXACT vs. only 2.7/cycle for OLD (~124x); >9000Hz is 203/cycle for EXACT vs. 0.83/cycle for OLD (~244x, i.e. OLD mostly doesn't even reach 9000Hz). The two generators' absolute worst-case single step is similar in principle (10226Hz EXACT vs. 10246Hz OLD, once), but everything about HOW OFTEN and HOW RELIABLY each generator produces something near that extreme differs enormously.

**The reproducibility difference is the real story.** Per-cycle worst-case step: EXACT generator's is EXACTLY 10226.39Hz on EVERY SINGLE one of the 59 cycles measured (std=0.000000Hz) - occurring at the identical sample index (2968) every single time, by construction of its bit-exact periodicity. OLD generator's per-cycle worst case averages only 8049.5Hz (std=289Hz, ranging 8003-10246Hz) and its LOCATION wanders across a 768-sample span cycle to cycle - it only occasionally, unpredictably, matches what the EXACT generator does deterministically every time.

**This reopens the AD9851/level-shifter family of theories with much better mechanistic grounding than before - not via the retracted "downstream LTI resonance" idea, but directly.** This project's own history already documents that a large single-tick FTW jump can flip many DATA bits faster than a level shifter's edge can settle, and specifically calls out ~8562Hz-class jumps as the trigger for real observed corruption. The EXACT generator routinely (332 times per cycle, every cycle, forever, at the same fixed sample positions) drives the AD9851 write path harder than that threshold; the OLD generator mostly doesn't (only ~2.7 times per cycle, and never at the same position twice). If AD9851 signal integrity genuinely degrades above some jump-size threshold in that neighborhood, the EXACT generator would trigger it with total reproducibility every cycle - exactly what a strong, coherent, discrete comb needs - while the OLD generator would trigger it rarely and non-repeatably, producing much lower-level, less coherent noise instead. This is a real, quantified, hardware-relevant difference between the two generators, not a difference this simulation manufactured by an idealized spectral abstraction - the underlying digital signal genuinely is more dangerous to the AD9851 write path under the EXACT generator.

**Concrete, well-targeted next step now available**: sample index 2968 (in whichever cycle-relative frame the real firmware uses) is now a KNOWN, precisely reproducible instant under the EXACT generator where the worst-case jump always occurs - a real hardware scope trigger or `'J'` jump-log capture timed to that exact moment could directly observe what happens on the AD9851 DATA/W_CLK/FQ_UD lines at the instant that matters, rather than waiting for an unpredictable spontaneous event. Also strengthens the case for the already-proposed FQ_UD-latch-margin flag split (previous entry) - that test is now backed by a concrete, quantified reason to expect it might matter, not just a general plausibility argument.

## 2026-09-17, later still: split FQ_UD's own latch-edge delay into an independent flag (`AD9851_FQUD_EDGE_DELAY_ENABLED`, default 1) so it can be bench-tested without the per-bit DATA-settle delays' separate real-time cost

Per the user's go-ahead, implemented the split proposed two entries ago. `AD9851.c`: added `AD9851_FQUD_EDGE_DELAY_ENABLED` (new, independent of `AD9851_BITBANG_EDGE_DELAY_ENABLED`), and changed the FQ_UD-latch call site's guard (the `ad9851_edge_delay()` right after `fast_gpio_clr(handle->pin_fqud)` in `ad9851_set_frequency()`'s bit-bang tail) to `#if AD9851_BITBANG_EDGE_DELAY_ENABLED || AD9851_FQUD_EDGE_DELAY_ENABLED` - either flag alone now enables just this one site; the original flag on its own still also re-enables the two per-bit sites, unchanged. Confirmed `handle->half_period_cycles` (the delay's own duration source) is computed unconditionally in `ad9851_init()` whenever `AD9851_USE_BITBANG` is set (which it is), not gated behind either edge-delay flag, so enabling only the new flag needs no other change. No other file references `AD9851_BITBANG_EDGE_DELAY_ENABLED`, so this is a fully isolated, easily-revertible change.

**Set to `1` (enabled) rather than this file's usual off-by-default convention**, deliberately, since the user asked to try it now rather than needing a second edit-and-reflash just to turn it on. Flipping it back to `0` returns to the exact FQ_UD-latch behavior every capture in this entire investigation has been taken under so far (zero settling margin on that edge), independent of whatever `AD9851_BITBANG_EDGE_DELAY_ENABLED` is set to.

**What this tests**: today's decisive `'O'` A/B finding (previous entry) - the EXACT generator reliably drives the AD9851 write path into a ~8500-9000Hz+ single-tick jump zone hundreds of times per cycle, every cycle, at fixed positions, while the OLD generator mostly doesn't - gives a concrete, quantified reason to expect FQ_UD's own latch-edge margin (previously untested in isolation, always bundled with the per-bit delays' own cost/benefit tradeoff) might matter here specifically. Predicted outcomes: if enabling just this delay narrows or removes the `'O'` A/B gap (old generator vs. exact generator), that's a real, load-bearing confirmation that FQ_UD signal integrity is a genuine contributor; if the gap is unchanged, this specific mechanism is cleared and attention moves to Fs/wakeup jitter or something else entirely. Not yet bench-tested.

## 2026-09-18: FQ_UD-latch-margin test comes back negative ("no noticeable diff"); deeper simulation answers "how does this compare to a perfect generator," characterizes what actually differs between the two generators, and reframes the "opportunity for dither" idea

User bench-tested `AD9851_FQUD_EDGE_DELAY_ENABLED=1` (added last entry) and reported "no noticeable diff" on the `'O'` A/B. **This clears the FQ_UD-latch-margin candidate**, at least by ear - it was assessed by listening, not by an IQ/spectral capture, so it isn't yet confirmed to the same quantitative standard as the rest of this investigation (a small effect below what's audible could still be present). Worth a follow-up IQ capture at some point to close this out properly, but not blocking - the candidate is deprioritized either way given a clearly negative subjective result. `AD9851_BITBANG_EDGE_DELAY_ENABLED`'s two per-bit DATA-settle delays remain a separate, still-open, more expensive-to-test candidate (unchanged by this result, since the flag split deliberately isolated the two).

Built a follow-up simulation (`sim_perfect_vs_real_and_dither_idea.py`, scratchpad-only) directly answering the user's three-part question: how the two real generators compare to a "perfect" (infinite-precision) reference; how to characterize what actually differs between OLD and EXACT; and whether the OLD generator's better real-hardware behavior is "an opportunity for dither."

**Q1 - the EXACT generator is NOT bit-exact at the true mathematical period, only at its own chosen 16000-sample wrap.** `gcd(700,1700)=100`, so the two-tone pair's true fundamental period is `16000/100 = 160` samples (0.01s) - two orders of magnitude finer than the 16000-sample (1.000s) period this entire investigation has characterized so far. Testing `|freq_dev[n] - freq_dev[n-160]|` over ~48000 comparisons on the EXACT generator's DSP output gives **max = 15285.47Hz - not zero**, a genuinely surprising result given the generator's `fmodf(idx*f/Fs, 1.0f)` computation looks like it should be exact modulo-160-periodic in real-number terms. Verified this wasn't a simulation-harness bug by comparing raw two-tone AUDIO samples (before any DSP) at the same lags: max diff at 160-sample lag = **0.00017207860946655273** (1892 of 31840 comparisons nonzero), vs. exactly **0.0** at the designed 16000-sample lag (first 1000 checked). So the raw audio genuinely differs, at float32 scale, between positions that are mathematically equivalent mod 160 samples but use different (larger) `idx` values - e.g. `idx=79` vs `idx=239` vs `idx=15919` all represent the same phase mod 1.0 in real-number terms, but `idx*700.0/16000.0` rounds slightly differently in float32 depending on `idx`'s magnitude before the `fmodf` throws away the integer part. The EXACT generator is only bit-exact at the ONE specific 16000-sample lag it was explicitly designed around (where it always retraces the identical `idx=0..15999` sequence), not at the true underlying periodicity.

**Q2 - a mathematically perfect implementation shows a comparably huge single-tick swing at the null; the ~8000-10000Hz jump is inherent to the signal, not a `fast_atan2`/float32 artifact.** Built `generate_twotone_PERFECT()` (float64, `np.mod`) and `run_dsp_PERFECT()` (float64 throughout, real `np.arctan2`/`np.sqrt`, no bit-hack approximation, no denormal flush) as a genuine infinite-precision reference, and ran one true 160-sample period through it. Result: **PERFECT max |freq_dev| = 7600.68Hz, max tick-to-tick step = 8001.53Hz**, vs. **REAL (fast_atan2/f32, EXACT generator) max |freq_dev| = 7715.83Hz, max step = 9795.93Hz** over the same window. A perfect implementation still swings ~8000Hz in a single tick right at the null - this is a property of destructive interference itself (the envelope collapses toward zero, which makes instantaneous phase - and therefore `atan2`-derived frequency - ill-conditioned by construction, independent of implementation). The real firmware's `fast_atan2`/float32 approximations do add real extra amplification on top of that irreducible baseline (~22% larger max step: 9796Hz vs 8002Hz), but they aren't creating the phenomenon from nothing - it would exist even with perfect math.

**Q3 - the OLD generator's departure from the TRUE 160-sample period is smooth and small, consistent with the already-documented systematic drift, not new noise.** Comparing consecutive 160-sample windows (after discarding the first 16000-sample startup transient): max|diff| per window for the first 10 windows = 0.763, 0.713, 0.646, 0.667, 0.678, 0.661, 0.617, 0.583, 0.591, 0.545 Hz - small and smoothly decreasing, the same slow-drift signature already characterized several entries ago, just visible at finer grain than the earlier 16000-sample-lag comparison.

**Q4 - "opportunity for dither," tested directly, gives a counterintuitive result that reframes rather than confirms the idea.** Built `generate_twotone_SYMMETRIC_RAMP()`: a clean, deterministic, symmetric (both tones equally, unlike `'Q'`'s tone2-only wobble) linear frequency ramp on an otherwise near-perfect float64 generator, at several tiny magnitudes. Result over 10 cycles: `ramp=0.000Hz/s` (no perturbation at all): **0** jumps above 8500Hz or 9000Hz. `ramp=0.001Hz/s` (minuscule): **247.0/cycle** above 8500Hz, **131.9/cycle** above 9000Hz. Every larger magnitude tested (0.01, 0.1, 1.0 Hz/s) stayed in the same hundreds-per-cycle range. **Any nonzero deterministic ramp, even a vanishingly small one, immediately creates hundreds of extreme jumps where a perfectly static near-zero-rounding-error baseline had none** - the opposite of the naive "a bit of dither smooths things out" intuition, and consistent with `'Q'`'s own real, previously-documented negative result (larger dither made the comb worse, not better).

**Synthesis - characterizing the actual difference between the two generators.** The EXACT generator's `idx`-magnitude-dependent float32 rounding (Q1) means each of the ~100 true 160-sample sub-cycles within its 1-second period gets a subtly different near-null "alignment quality" - most benign, but at least one (the one landing at sample index 2968, already identified two entries ago as the fixed 10226.39Hz worst case) landing on an especially severe knife-edge. Because `idx` always retraces the identical `0..15999` sequence every subsequent 1-second repeat, that exact worst-case alignment recurs unchanged, forever - a perfectly rigid, maximally coherent comb line. The OLD generator's continuously-evolving accumulator state never revisits the same large-`idx`-equivalent floating-point state twice (Q3's smoothly-drifting signature), so it effectively continuously resamples across that same landscape of ~100 possible alignment qualities rather than freezing on the single worst one - landing on something as severe as EXACT's fixed worst case only rarely and non-repeatably (already quantified two entries ago: OLD's per-cycle worst case averages 8049Hz, wanders location, vs. EXACT's fixed 10226Hz at a fixed sample every cycle).

This reframes, rather than confirms, the "opportunity for dither" comment. Q4's own result says the mechanism that helps is NOT "add dither" in the generic/textbook sense - both `'Q'`'s real dither and this entry's clean deterministic ramp made extreme-jump counts worse, not better, when tested directly. The more precise lesson from Q1/synthesis above looks closer to "never let the near-null alignment landscape become PERFECTLY, permanently fixed at all" - which the OLD generator's own unintended, bug-driven imprecision happens to achieve, apparently better than either `'Q'`'s larger deliberate wobble or a clean symmetric ramp did in this quick test. This is offered as a hypothesis needing a dedicated follow-up, not a ready answer: a proper test would need a sweep across many perturbation strategies, magnitudes, and characters (not just ramps and not just `'Q'`-style continuous wobble), measured directly against the tick-to-tick extreme-jump-count-and-reproducibility metric that turned out to matter (per two entries ago), rather than against an idealized spectral floor. Not yet designed or run.

## 2026-09-18, later: MOD160 (true-period-native phase computation) simulates as a genuinely strong fix - eliminates the >8500Hz danger zone entirely; targeted near-null-only dither tested directly and does NOT help; AD9851 phase-offset modulation checked and confirmed never used

Three follow-ups from the user's latest message, all tested directly rather than argued abstractly (`sim_mod160_and_targeted_dither.py`, scratchpad-only).

**Idea 1 - "should we make the generator periodic in 160 samples?" YES, and simulation shows it's a real, principled numerical fix, not a workaround.** Built `generate_twotone_MOD160()`: identical to the EXACT generator except the running index wraps at the TRUE period (160 samples, from last entry's `gcd(700,1700)=100` finding) instead of at 16000. This isn't dithering - it's proper argument-range-reduction (computing `fmodf(idx*f/Fs, 1.0f)` from a small `idx` in `[0,160)` instead of a large one in `[0,16000)`), which directly targets last entry's root cause (idx-magnitude-dependent float32 rounding). Result, over 60 cycles (960,000 ticks):

| generator | max tick-to-tick step | count(step>8500Hz)/cycle | count(step>9000Hz)/cycle |
|---|---|---|---|
| EXACT (idx mod 16000) | 10226.39 Hz | 332.0 | 203.0 |
| OLD (accumulate) | 10246.69 Hz | 2.7 | 0.8 |
| **MOD160 (idx mod 160)** | **8001.55 Hz** | **0** | **0** |

MOD160's max step (8001.55Hz) matches last entry's PERFECT float64 reference (8001.53Hz) almost exactly - it recovers the mathematically-irreducible near-null floor with none of EXACT's idx-magnitude rounding penalty. **Zero** excursions above 8500 or 9000Hz across all 60 cycles - better than EXACT (332/cycle) and also better than OLD (2.7/cycle), by this metric. Confirmed genuinely, bit-exact periodic at the TRUE 160-sample period (not just claimed): after discarding the ~1000-sample startup transient, `max|freq_dev[n]-freq_dev[n-160]|` over a fresh, isolated check = exactly **0.0**. The tradeoff, stated plainly: MOD160's own worst case (8001.55Hz, safely below the >8500Hz danger zone) now repeats at a FIXED sample position 100 times per second (every 0.01s) rather than once per second like EXACT - more rigidly periodic at a finer grain, not less. But since the real hardware evidence in this project ties corruption risk to JUMP MAGNITUDE crossing a threshold (~8562Hz-class jumps outrunning the BS170 level shifter), not to periodicity by itself, and MOD160 never crosses that threshold at all, this looks like a genuinely strong candidate - arguably the best of the three generators tested to date, and unlike the OLD generator it carries no drift bug and unlike a dither scheme it's deterministic and simple.

**Recommendation: implement and bench-test.** In firmware terms this means changing `s_tone_sample_index`'s wrap modulus from `SAMPLE_RATE_HZ` (16000) to the true period for whichever tone pair is active (160 for the current 700/1700Hz pair) - a small, well-understood change, structurally identical to what `'O'`/the exact-recompute scheme already does, just with a tighter wrap. Caveat: the correct modulus depends on the actual tone frequencies and would need recomputing (or looking up) for other bands/presets selected via `'T'`, since a different F1/F2 pair generally has a different `gcd`-derived true period - this needs a small design decision (compute it at runtime from F1/F2/SAMPLE_RATE_HZ via a GCD, or hardcode per band) before implementing, not yet done.

**Idea 2 - "randomize the transitions in the critical region specifically" (not everywhere, unlike Q4's blanket ramp) - tested directly, does NOT give a clean win.** Identified the ~1146 actual near-null sample-in-cycle positions from a clean EXACT-generator pass (step>5000Hz at least once at that position), then built `generate_twotone_TARGETED_DITHER()`: bit-identical to the real EXACT generator everywhere else, but at exactly those positions each tone gets an independent small random frequency kick for that sample only. Tested kick magnitudes +/-1, 5, 20, 100Hz:

| kick magnitude | max step | count(step>8500Hz)/cycle | count(step>9000Hz)/cycle |
|---|---|---|---|
| 0 (baseline EXACT) | 10226.39 Hz | 332.0 | 203.0 |
| +/-1 Hz | 14938.65 Hz | 252.7 | 192.3 |
| +/-5 Hz | 15164.61 Hz | 279.4 | 219.8 |
| +/-20 Hz | 15117.64 Hz | 280.3 | 220.4 |
| +/-100 Hz | 15232.72 Hz | 284.0 | 224.4 |

Even the smallest kick tested (+/-1Hz) pushed the worst-case single step up to ~14900-15200Hz - roughly 50% worse than doing nothing - while only modestly reducing the count of moderate (>8500/9000Hz) excursions (332->~280/cycle, ~15-20% fewer). This is a worse trade than it looks: kicking frequency right where sensitivity is already extreme (near a null, where `fast_atan2`'s output is ill-conditioned) doesn't reliably move the signal away from danger - it can just as easily create a NEW, rarer but larger excursion, which is a bad trade for hardware risk (one very large step is plausibly worse for signal integrity than several moderate ones). This isn't as unambiguously bad as Q4's blanket ramp (which created hundreds of new violations from zero with no offsetting benefit at all), but it's still a net-negative/mixed result, not the "opportunity" it sounded like - targeted randomization at the critical region specifically doesn't dodge the underlying problem that near-null sensitivity is what makes ANY perturbation there risky, deterministic or random. **MOD160 (Idea 1) remains the strictly better result of everything tested so far** - it reduces the worst case rather than redistributing it.

**AD9851 phase-offset modulation, checked directly: never discussed or used in this project.** Grepped `AD9851.c`/`.h` and confirmed the AD9851's own 5-bit phase-offset field (part of its standard 40-bit control word, alongside the 32-bit FTW - 32 discrete steps at 11.25 degrees each) is hardcoded to `0` in every `ad9851_set_frequency()` call (`buf[4]` construction: power-down or 6x-multiplier bit only, "phase=0" per the existing comment) - it has never been used as a modulation path here. It's a genuinely different, unexplored lever worth naming: since only 5 phase bits would need to change per update (vs. up to 32 FTW bits), a phase-word-based approach would inherently need far fewer simultaneous DATA-line transitions per write - directly relevant to this project's own documented failure mechanism (many simultaneous 0-to-1 transitions outrunning the BS170 level shifter's slow passive edge). Two real caveats, though: (1) this would be a fundamentally different modulation architecture - this project's whole EER/polar approach encodes SSB via frequency deviation of the carrier (an FM/chirp-style approach), not phase modulation of a fixed carrier, so adopting this would mean redesigning the modulation scheme, not adding a toggle; (2) 32-step (11.25 degree) phase resolution is fairly coarse and its adequacy for audio-quality SSB has not been assessed. Flagging this as a real, previously-undiscussed architectural option worth a dedicated feasibility discussion, not something to prototype alongside today's smaller, targeted fixes.

## 2026-09-18, later still: 'O' extended from a plain EXACT/LEGACY toggle to a 3-way cycle, adding MOD160, for a direct on-hardware A/B/C of all three generators

Per the user's request to try MOD160 first, on real hardware, rather than only in simulation: extended `'O'` from a bool toggle into a 3-way cycle (`EXACT -> LEGACY -> MOD160 -> EXACT...`), so all three generators simulated in the previous two entries can be A/B/C'd live without a reflash between presses.

**`test_signals.h`/`test_signals.cpp`**: replaced `s_twotone_legacy_phase_enable` (bool) with `s_twotone_phase_gen_mode` (new `twotone_phase_gen_t` enum: `TWOTONE_PHASE_GEN_EXACT=0` (default, unchanged), `TWOTONE_PHASE_GEN_LEGACY=1`, `TWOTONE_PHASE_GEN_MOD160=2`) and `test_signals_next_twotone_phase_gen()` (cycles and returns the new mode's name, mirroring `test_signals_next_twotone_band()`'s existing pattern for `'T'`), plus a non-mutating `test_signals_get_twotone_phase_gen_name()` for status/banner lines. Added a second wrapping counter, `s_tone_sample_index_160`, alongside the existing `s_tone_sample_index` (mod 16000) - wraps at `TWOTONE_MOD160_PERIOD_SAMPLES` (160, hardcoded - see caveat below), always advances regardless of active mode, same no-reset-on-switch convention the existing index already used. `generate_twotone_sample()`'s tail is now a 3-way `switch` on the mode for tone1 (LEGACY: old accumulate; MOD160: recompute from `s_tone_sample_index_160`; EXACT/default: recompute from `s_tone_sample_index`, unchanged), and a matching 3-way branch for tone2 (accumulate whenever dither is on OR mode is LEGACY; else recompute from whichever index the active mode uses) - structurally the same shape as the previous 2-way version, just extended.

**`serial_commands.cpp`**'s `'O'` handler now calls `test_signals_next_twotone_phase_gen()` instead of toggling a bool, and reports the current mode's name directly (`"-> two-tone phase generator: <name>..."`) rather than an ON/off phrasing. `ssb_mic_test.ino`'s boot banner updated to match (describes the 3-way cycle, uses `test_signals_get_twotone_phase_gen_name()`). Verified no other file references the old `test_signals_get/set_twotone_legacy_phase_enabled()` names (both fully removed, not just deprecated) and that braces/parens still balance in `test_signals.cpp` (47/47, 199/199) after the edit.

**Important caveat, carried over from the design discussion two entries ago and repeated here for anyone bench-testing**: `TWOTONE_MOD160_PERIOD_SAMPLES` is a hardcoded `160`, correct for every `TWOTONE_BAND_PRESETS` entry currently selectable via `'T'` (all share the underlying 100Hz GCD structure with `SAMPLE_RATE_HZ`) EXCEPT the deliberately non-integer-Hz TEMP control entry (700.37/1700.61Hz), which has no short true period at all - selecting MOD160 while that specific band is active will introduce artificial phase discontinuities every 160 samples instead of the smooth non-periodic behavior that entry exists to provide. Not an issue for the current default band or any of the round-number presets; just don't combine MOD160 with that one specific `'T'` entry when interpreting results.

**Not yet bench-tested** - implementation only this entry. Recommended next step: cycle through all three with `'O'` on the current setup and compare by ear/capture the same way the EXACT-vs-LEGACY A/B was done several entries ago, ideally confirming the simulated result (MOD160 quietest, matching or beating LEGACY's ~38dB improvement over EXACT with none of LEGACY's drift liability).

## 2026-09-18, later still: added 'M' - a separate mute for the three auto-dump watchers (canary/slow_trace-auto/held_freq-auto), distinct from 'v'

User asked for a mute command covering all the diagnostic auto-dumps. Investigated what actually auto-prints without a keypress: the periodic status/[timing]/[adc]/[dsp]/null_bias block (already covered by the existing `'v'`/`diagnostics_toggle_muted()`), and three separate watchers that were DELIBERATELY built mute-exempt from `'v'` specifically so they'd keep working unattended - `canary_check_background()` (prints once on a corruption transition), `diagnostics_check_slow_trace_auto_dump()` (the `'K'` jump-trace detector's automatic side - prints "AUTO-CAPTURED" the instant a latch fires, no keypress needed), and `diagnostics_check_held_freq()` (the `'H'` stuck-frequency detector's automatic side - CONFIRMED STUCK/RECOVERED prints). The existing code comments on all three are explicit about why: "a real jump landing in the log matters more than whether 'v' happens to be muted."

**Decision: added a NEW, separate toggle (`'M'`) rather than folding these into `'v'`.** Reasoning: `'v'`'s existing mute-exemption for these three is a deliberate safety property (catching a rare corruption/jump/stuck-frequency event during an unattended bench run), and quietly changing `'v'` to also suppress them would silently weaken that safety net for anyone using `'v'` for its original "silence the routine noise" purpose without realizing they'd also disabled their watchdogs. A separate, explicitly-named command keeps both behaviors available and unambiguous. Not run past the user first since this is a low-risk, easily-revertible additive change (a new letter, existing behavior untouched) - happy to fold `'M'` into `'v'` instead if a single unified mute is actually preferred.

**Implementation** (`diagnostics.h`/`diagnostics.cpp`/`serial_commands.cpp`/`ssb_mic_test.ino`): new `s_diag_autodump_muted` flag (independent of the existing `s_diag_muted`), `diagnostics_get_autodump_muted()`/`diagnostics_toggle_autodump_muted()` mirroring the existing `'v'` pair exactly (always prints its own confirmation regardless of new state, same as `'v'` does). `diagnostics_service()`'s three previously-unconditional calls (`canary_check_background()`, `diagnostics_check_slow_trace_auto_dump()`, `diagnostics_check_held_freq()`) are now wrapped in a single `if (!s_diag_autodump_muted)` block. `'M'` (uppercase, previously unused) wired up in `serial_commands.cpp`; boot banner updated to mention it. Verified no other call site for the three gated functions exists (all three are `static`, file-local, called only from those three `diagnostics_service()` lines) and that braces/parens/`#if`/`#endif` still balance (178/178, 1305/1305, 23/23) after the edit.

**Important caveat, documented in the code and worth restating here**: unlike `'v'`'s periodic block (which keeps counting internally and just skips printing), all three of these watchers do their DETECTION inside the same call that's now gated - so while `'M'` is on, none of them even check their trigger condition. This means: (1) a canary mismatch's "first seen at t=...ms" timestamp, if one happens to fire while muted, will read as whenever `'M'` is next turned off (not the true onset) - the underlying corrupted/latched state itself isn't lost (these are all designed to persist until read, not self-heal), just the exact onset moment; (2) a `'K'`-style slow_trace LATCH or an `'H'`-style confirmed-stuck episode will simply sit un-rearmed/unprinted until `'M'` is turned back off, at which point it prints immediately on the next tick - no event is silently discarded, but detection/reporting IS delayed for the duration `'M'` is on. Manual on-demand reads (`'K'`, `'H'`, `diagnostics_print_now()`/`'D'`'s canary status) are completely unaffected either way, exactly as they already ignore `'v'`. Not yet bench-tested.

## 2026-09-18, later still: first MOD160 bench result - broadband floor drops to -80dB as predicted, but replaced by a discrete ~7.6Hz-spaced comb ~-20dB down, not the ~100Hz spacing the audio-domain-only simulation predicted; steady once established, possibly Preset/start-condition dependent

User's first real-hardware MOD160 result: the smeared broadband noise floor is gone, replaced by a much lower floor (**-80dB**, matching the simulation's prediction that eliminating the >8500/9000Hz jump zone should help a lot) - but that floor is now punctuated by a **discrete comb spaced ~7.6Hz apart, sitting roughly -20dB down**. Whatever produces it appears frequency-stable once established (does not wander/drift within a run - a real, useful property), but its exact frequency "varies with Preset and start conditions (maybe)" per the user's own observation - not yet characterized in detail, flagged as needing a longer look.

**The 7.6Hz spacing does NOT match this investigation's own prediction.** The whole MOD160 rationale (this entry's two predecessors) was built on the true fundamental period being 160 samples = 0.01s in the AUDIO/DSP domain, which should produce comb teeth spaced at multiples of `1/0.01s = 100Hz`, not 7.6Hz - a genuine, unexplained discrepancy between the audio-domain-only simulation and what real hardware is actually doing. Flagging this honestly rather than fitting a story to it: **the mechanism behind the 7.6Hz spacing is not yet identified.**

**Ruled out by direct code inspection** (checking for an obvious "two clock domains beating against each other" explanation, since a beat between the newly-rigid 100Hz audio-domain comb and some OTHER real periodic process not modeled in the audio-only simulation was the first candidate worth checking): the sample timer (`ssb_mic_test.ino`'s `init_sample_timer()`) uses `resolution_hz = 2000000 * ENVELOPE_INTERP_FACTOR` with `alarm_count = 2000000/SAMPLE_RATE_HZ = 125` - an EXACT 62.5us/tick at every `ENVELOPE_INTERP_FACTOR` setting (the scaling is proportional, not truncated), so this isn't an integer-microsecond-rounding clock error of the kind that would produce a slow systematic beat. The ADC's own decimation (`ADC_CONT_SAMPLE_FREQ_HZ=80000 / 5 = SAMPLE_RATE_HZ=16000`) and the envelope PWM carrier (`RSET_MOD_LEDC_FREQ_HZ=64000 / SAMPLE_RATE_HZ=16000 = 4`) are both also exact integer ratios with no fractional remainder. None of the three obvious digital clock relationships in this firmware show the kind of near-but-not-exact rational relationship that would generate a clean, slow, deterministic beat - so if this IS a clock-domain beat, the second clock isn't one of these three; more likely candidates needing real measurement rather than code inspection: AD9851 write-loop timing jitter/overhead (real, variable `busy_us`, not a clean rational clock), or the `relative_delay` fractional-sample interpolation ring (a REAL-valued, not purely rational, per-Preset parameter - directly consistent with the user's own "varies with Preset" observation, since `relative_delay_samples` differs by preset: 0.00 for Preset 1, 2.00 for Preset 3, etc.)

**Leading hypotheses, all speculative, none tested yet:**
1. **`relative_delay`'s fractional-sample interpolation interacting with MOD160's now-rigid periodicity.** `relative_delay_apply()`'s `interp_ring()` blends between adjacent ring-buffer samples by a fractional weight (`frac`) derived from the delay value - a real-number operation, not a clean integer-sample relationship. If a Preset's `relative_delay_samples` isn't an exact multiple of anything commensurate with the 160-sample grid (most presets' values are NOT: e.g. Preset 3 = 2.00 exactly IS commensurate; other presets/settings may not be), the interpolation could introduce structure inside the 160-sample repeat that isn't a clean single 100Hz line - though it's unclear from reasoning alone why this would produce something as fine as 7.6Hz specifically. This candidate is consistent with "varies with Preset" (different presets have different relative_delay) but doesn't obviously explain "varies with start conditions" on its own.
2. **AD9851 write-timing jitter/overhead beating against the newly-rigid digital comb.** The real per-tick `busy_us` (SPI/bit-bang write time) has its own timing variability (already documented elsewhere in this project); if that variability has any slow, quasi-periodic component (e.g., from USB-CDC Serial draining, other Core 1 background work, or scheduling), it could impose a slow amplitude/phase modulation on an otherwise-perfectly-rigid comb - consistent with "start conditions" mattering (whatever Core 1 happens to be doing at the moment MOD160 is engaged) but less obviously consistent with "varies with Preset" unless different presets' downstream processing (gdeq/ampeq/predistort) changes per-tick timing enough to matter.
3. **A capture/measurement-side artifact** (FFT bin width or capture-window length coincidentally producing an apparent ~7.6Hz line spacing) - the cheapest hypothesis to rule out and should be checked FIRST before chasing a firmware explanation. Needs the actual capture (duration, sample rate) to check bin-width arithmetic directly.

**Recommended next steps, none done yet**: (1) send the actual IQ/WAV capture so the comb spacing, level, and shape can be measured precisely rather than read off by ear/eye, and to rule out hypothesis 3 directly; (2) a controlled start-condition test - cycle `'O'` to MOD160 several times in a row at a FIXED preset (without touching anything else) and check whether the comb's exact frequency/position changes each time or settles to the same place, to characterize what "start conditions" actually means here; (3) a controlled Preset sweep at MOD160 (same approach as the earlier `'O'` Preset 1 vs Preset 3 test) to see whether the comb spacing tracks `relative_delay_samples` in a predictable way (supporting hypothesis 1) or not (weakening it). Not yet run.

## 2026-09-18, later still: measured the actual MOD160/Preset3 capture directly - confirms a real, precise 7.702Hz comb (not a measurement artifact), AND confirms the originally-predicted ~100Hz comb is also present but less prominent nearby; exact mechanism still unidentified, but a concrete, cheap next test identified

User supplied the actual capture (`SDRuno_20260918_093349Z_14175kHz.wav`, Preset 3, MOD160) rather than leaving the previous entry's speculation unchecked. Measured directly (`analyze_mod160_preset3.py`, scratchpad-only): 14.672s capture at 388888Hz IQ sample rate gives a native FFT bin width of 0.068Hz - more than fine enough to resolve a 7.7Hz-spaced comb with room to spare, which rules out the previous entry's "FFT resolution artifact" hypothesis outright (hypothesis 3, cleared).

**The comb is real and very precisely 7.702Hz-spaced, confirmed multiple independent ways:**
- Peak-picking local maxima within +/-500Hz of the lower tone (25692.0Hz dial-relative) found 35 peaks above -45dB; a spacing histogram of consecutive gaps shows 7.7Hz as by far the single most common spacing (15 of 34 gaps), with several other gaps landing on clean small-integer multiples of the same value (15.4Hz=2x, 23.0-23.1Hz~3x, 30.7Hz~4x) - a single fundamental spacing, not noise.
- The very first sidebands either side of the main tone, at exactly +/-7.702Hz, are the TALLEST non-tone features found: -20.07dB (below) and -19.92dB (above) - matching the user's own "-20dB down" reading closely.
- The same 7.702Hz spacing also appears (much fainter, ~-49 to -60dB) in a region 1.5-3kHz away from the tone, where the general floor is otherwise ~-91dB - so this isn't purely a near-tone phenomenon, though it's far more prominent close to the signal.

**The originally-predicted ~100Hz comb (from MOD160's designed 160-sample/0.01s period) is ALSO genuinely present** - checked directly at exact multiples: tone-100Hz=-26.0dB, tone-200Hz=-30.0dB, tone-300Hz=-43.5dB, tone-400Hz=-41.8dB, tone-500Hz=-41.1dB, all real local maxima at those specific offsets. So the simulation's prediction wasn't wrong, exactly - MOD160 does produce the expected 100Hz-spaced structure - but it's NOT the dominant nearby feature, because an unexpected, apparently independent ~7.7Hz-spaced structure sits closer to the tone and is stronger there (-20dB vs the 100Hz comb's own -26dB first tooth).

**A numerically suggestive, NOT confirmed, coincidence worth flagging with appropriate hedging**: `16000 / 7.702 = 2077.4` samples - close to (`160 x 13 = 2080` samples, which would give a spacing of `16000/2080 = 7.6923Hz`, about 0.13% off the measured 7.702Hz). If the TRUE underlying digital repeat period turned out to be 2080 samples (13x the intended 160) rather than exactly 160, that would explain both combs simultaneously: a coarse 100Hz "envelope" (the intended 160-sample structure) with 13 finer 7.69Hz teeth inside each 100Hz cell (the actual, longer true period). This is offered as a lead, not a finding - no mechanism in this firmware that would naturally introduce a factor of 13 has been identified yet, and the ~0.13% mismatch between 7.6923Hz (theory) and 7.702Hz (measured) is small but real, not obviously just SDR dial/reference error. Treat this as a hypothesis to test, not an explanation.

**Recommended next step, cheap and directly diagnostic given this project's own established pattern (the Preset 1 vs Preset 3 `'O'` test that falsified the downstream-stack theory several entries ago)**: run this same MOD160 capture on Preset 1 instead of Preset 3. Preset 1 has `relative_delay_samples=0.00` and its entire downstream stack (gdeq/ampeq/predistort) off - confirmed several entries ago to be a genuine bit-exact no-op chain. If the 7.702Hz fine comb disappears or changes character on Preset 1, that implicates something in the downstream stack or `relative_delay`'s nonzero fractional interpolation (Preset 3's `relative_delay_samples=2.00`, though that's an exact integer so ought to also be a clean, non-fractional case worth double-checking); if it persists identically, the mechanism is upstream of all of that - in the core two-tone generation, the AD9851 write path, or something else common to every preset. Not yet run.

## 2026-09-18, later still: the 7.7Hz comb's FREQUENCY is invariant across Preset and tone-pair (only its amplitude varies) - and it turns out to PRE-DATE MOD160 entirely, found (much fainter) in yesterday's OLD-generator "low noise benchmark" capture too; the 13x-160-samples numerology from last entry is now the WEAKER explanation. Also: a genuinely nice practical result - relative_delay tuning the residual off-tone frequency to exactly zero on Preset 3 coincides with the optimum IMD point.

User ran the recommended Preset 1 MOD160 test plus several other `'T'` tone-pair bands: **the comb's frequency stays fixed at 7.7Hz regardless of Preset or tone-pair - only its amplitude changes, sometimes considerably.** This is the same falsification shape as the earlier downstream-stack theory (Preset 1 has zero live conditioning, confirmed several entries ago) - so whatever sets the comb's FREQUENCY is not relative_delay, not gdeq/ampeq/predistort, and (since it's also unchanged across different `'T'` tone pairs) very likely not tied to the two-tone generator's own specific F1/F2 values either.

**Went back and re-examined yesterday's pre-MOD160 captures directly, rather than treating this as unexplained from today's data alone** (`check_comb_in_earlier_captures.py`, scratchpad-only). Result: the **OLD-generator Preset-3/ADC-off "low noise benchmark" capture (20260917_204619Z, from BEFORE MOD160 existed) already shows the same feature** - a discrete pair of side-peaks at exactly **-7.681Hz and +7.681Hz** relative to the tone, at **-38.0dB / -37.3dB** - essentially the same frequency as today's MOD160 measurement (7.702Hz), just ~50-60x fainter (-38dB then vs -20dB now) and buried far enough below that capture's own already-very-low floor that it was never flagged at the time. By contrast, the two EXACT-generator captures from the same day (Preset 1 and Preset 3, both "noisy") show no comparable discrete structure in the same +/-15Hz window - only a smooth, much higher-level (-3 to -9dB) skirt, consistent with those captures' already-documented broad ~10Hz "-30dB width" incoherent noise mound completely swamping anything this much smaller underneath it.

**This changes the leading interpretation.** MOD160 (and, apparently, the OLD generator too) did not CREATE the 7.7Hz comb - both simply reduced a much larger, unrelated broadband/incoherent noise source enough to reveal a smaller, pre-existing artifact that was always there, silently sitting 40-90dB down, masked by whichever generator-specific noise dominated at the time. Combined with today's Preset/tone-pair invariance, **the previous entry's "true period might actually be 2080 samples (160x13)" numerology is now the WEAKER of the two explanations** - that theory was tied specifically to the two-tone generator's own digital math, but the comb demonstrably predates MOD160 and appears identically (in frequency, not amplitude) under the OLD generator's completely different, non-160-periodic, continuously-drifting digital signal too. A digital-math explanation tied to either generator's own periodicity looks increasingly unlikely; something common to the whole system regardless of which generator or preset is active - most plausibly a genuine hardware/environmental artifact elsewhere in the chain (a real oscillation, a slow PWM/regulator ripple, a receiver-side or ambient effect) - is now the better-supported direction, though still not confirmed.

**Recommended next step, chosen to discriminate "belongs to the two-tone/DSP signal path" from "present regardless of what's being transmitted"**: capture with `'s'` (single, unmodulated tone) or with the transmitter running but effectively idle, and check whether the same ~7.7Hz-spaced sideband pair appears around WHATEVER carrier/tone is present. If it does, that's strong evidence this has nothing to do with the two-tone generator, MOD160, or any DSP stage at all - it would point at something further downstream (AD9851/analog output chain) or even outside the DUT (mains-adjacent ripple, an SDR-side artifact, ambient RF) that would show up on any transmission regardless of content. Not yet run.

## 2026-09-18, later still: nice practical result - on Preset 3, tuning relative_delay so the residual off-tone frequency reads exactly zero lines up with the optimum IMD point

Separately, a genuinely useful, positive finding from the same MOD160/Preset 3 bench session: this project has repeatedly observed (several entries ago) that `relative_delay` "tunes" a residual off-carrier frequency reading (e.g. Preset 3 read -18Hz, Preset 1 read -10Hz, both attributed to `relative_delay`'s known effect on the observed null-offset frequency). The user now reports that, on Preset 3, sweeping `relative_delay` (`'['`/`']'`) so that this residual off-tone frequency reads exactly **zero** coincides with the point of **optimum (best) IMD**. In other words, this residual frequency reading isn't just an interesting side-effect to note and move past - it's a usable, real-time tuning indicator: dialing it to zero is a simple, practical way to find the IMD-optimal `relative_delay` setting for a given preset/band live, without needing a full spectral sweep each time. Worth keeping in mind as a general calibration technique for future preset/band tuning work (the existing `'T'` band-sweep entries already note "re-tune relative delay for this band before capturing" - this gives a concrete, simple criterion for doing that: adjust until the off-tone reads zero).

## 2026-09-18, later still: the decisive single-tone ('s') test comes back - the 7.7Hz comb is ABSENT at that frequency; single-tone gives an excellent, near-ideal -103.6dB floor, but shows a different, much fainter (~-65dB) comb at ~16.5Hz spacing instead - reframes the leading theory back toward "requires two-tone interference," not a generic hardware artifact

User ran the recommended discriminating test: `'s'` (single, unmodulated tone) instead of two-tone. Measured directly (`analyze_singletone_095641.py`, scratchpad-only, same methodology as every prior capture this session): **the level right at +/-7.7Hz from the single tone is -84 to -85dB - essentially just the general noise floor, no comb feature there at all.** This falsifies the previous entry's leading theory in its simplest form: the 7.7Hz artifact is NOT a generic, always-present hardware/environmental artifact that shows up regardless of what's being transmitted - it requires two-tone content specifically.

**Single tone gives an excellent, near-ideal floor overall**: near-tone floor (400-900Hz away) and far floor (8-12kHz away) both measure **-103.6dB**, identical to within measurement precision - confirming (as expected, since a single tone has a constant, never-collapsing envelope and therefore none of the destructive-interference nulls this whole investigation has been chasing) that the DSP/AD9851/analog chain is capable of a genuinely excellent floor when there's no near-null event to go unstable at all. This is a useful new reference point: -103.6dB is close to this hardware's practical floor, so the two-tone case's remaining -80dB (MOD160) or -20dB (comb-limited near the tone) numbers can be read against a concrete "how good could it be" baseline now, not just against each other.

**A different, much fainter comb IS present even in single-tone mode, at roughly 16.5-16.6Hz spacing** (with harmonics visible at ~33.1Hz, ~49.6Hz, ~99.2Hz - all clean small-integer multiples of the same ~16.55Hz fundamental) - but at only -64 to -73dB, roughly 45-50dB fainter than the two-tone case's 7.7Hz comb. Single-tone mode uses a completely separate, always-been-there code path (`generate_singletone_sample()`, its own independent plain accumulator, never touched by the NCO fix or by `'O'`/MOD160 at all - `'O'` only affects `generate_twotone_sample()`), so this ~16.5Hz feature can't be coming from anything this session added; if real (not a per-capture SDR/environmental coincidence - only one single-tone capture has been taken so far, so this isn't yet confirmed reproducible), it's evidence of some small, generic, always-present system-level perturbation distinct from and much smaller than the two-tone-specific 7.7Hz one.

**Updated synthesis**: the 7.7Hz comb most plausibly requires the two-tone signal's own destructive-interference/near-null events to become significant - consistent with everything else this investigation has already established about near-null sensitivity (a `fast_atan2`-driven `freq_dev` computation that's inherently ill-conditioned right at a null, confirmed even in a "perfect" float64 reference two entries ago). A speculative but coherent way to reconcile this with last entry's Preset/tone-pair/generator invariance: if a small, generic system-level perturbation (maybe related to whatever produces single-tone's own fainter ~16.5Hz comb, maybe something else entirely - not established) is always present at a low level, the two-tone signal's near-null moments could be dramatically MORE SENSITIVE to that same small perturbation than a never-collapsing single tone is, amplifying an otherwise negligible effect into the much more prominent 7.7Hz comb seen in two-tone captures - while the underlying frequency of 7.7Hz itself might reflect an interaction/aliasing between the null-recurrence structure and whatever the generic perturbation's own rate is, rather than being that generic perturbation's rate directly (7.7Hz and 16.5Hz aren't a clean small-integer ratio of each other - 16.5/7.7=2.14 - so this is NOT a simple case of "the 7.7Hz comb is just the ~16.5Hz one's subharmonic," and should not be asserted as such). Flagged explicitly as speculative synthesis, not a confirmed mechanism - offered to keep the investigation moving, not as a conclusion to stop on.

**Recommended next steps**: (1) confirm the ~16.5Hz single-tone comb is reproducible (a second single-tone capture, ideally on a different band/frequency via `'T'`-equivalent for single-tone if available, or at least a repeat run) before treating it as a real, stable feature rather than a one-off coincidence; (2) now that single-tone's own floor is known to be an excellent -103.6dB baseline, consider a two-tone-at-large-relative_delay-offset or reduced-amplitude test to see if the 7.7Hz comb's LEVEL scales with how deep/severe the near-null events are (supporting the "amplification of a small generic perturbation by near-null sensitivity" theory) rather than being fixed regardless of null depth. Not yet run.

## 2026-09-18, later still: AM-isolation-test-at-zero-depth capture (Preset 1, envelope AND freq_dev both hardcoded constants, bypassing the Hilbert/atan2 DSP entirely) reproduces the SAME ~16.5Hz comb, decisively ruling out the audio-domain DSP math as its source; a mechanistically well-grounded new lead - AM-to-PM conversion in the EER output stage - proposed

User's insight, directly tested: AM isolation mode (`'h'`) is a genuinely different DSP route from single-tone (`'s'`) - `test_signals_generate_amtest()` bypasses `ssb_dsp_process_sample()`/the Hilbert transform/`fast_atan2`/`fast_sqrt` entirely, hardcoding `*out_freq_dev_hz = 0.0f` (carrier frequency word literally never changes, every tick) and, at zero mod depth (`master_gain_linear=0` collapses `swing` to 0), `*out_envelope` to the fixed constant `AM_TEST_DEPTH` too - so this capture is about as close to "transmit a perfectly static, unmodulated carrier" as this firmware can produce, with NONE of the audio-domain math (Hilbert FIR, `fast_atan2`, `fast_sqrt`) exercised at all.

**Measured directly** (`analyze_amtest_100050.py`, scratchpad-only, same methodology): **no 7.7Hz comb** (levels -86 to -88dB at that offset, matching the single-tone result exactly) - consistent with the previous entry's finding that 7.7Hz needs two-tone-style destructive interference to become significant. **The same ~16.5Hz comb from the single-tone capture reproduces here too**, at closely matching levels and the identical harmonic pattern: -72/-73dB at +/-16.5Hz, -66/-71dB at 33/49.6Hz, -65dB at 99Hz. Floor is even better than single-tone's: -107.6dB far, -107.1dB near (the most static possible test case, even more so than a real sine wave through the DSP).

**This is decisive, not just corroborating.** Because this capture's envelope AND freq_dev are hardcoded numeric constants that never touch the Hilbert-transform/`fast_atan2`/`fast_sqrt` audio-domain math at all, the ~16.5Hz comb CANNOT be coming from anything in `ssb_dsp_process_sample()` - it also happens to answer the previous entry's "confirm this is reproducible, not a one-off" question, via an independent code path rather than a literal repeat. The comb must be introduced somewhere AFTER dsp_task computes envelope/freq_dev: the freq_dev-to-AD9851-frequency-word write path, the envelope-to-PWM/DAC output path, or the analog/RF output stage itself.

**A mechanistically well-grounded new hypothesis, given this project's own architecture**: this is an EER/polar transmitter, and a well-known real-world impairment in that architecture class is AM-to-PM conversion - amplitude-path ripple leaking into the transmitted signal's apparent phase/frequency via the output stage's own bias-dependent phase response, even when the commanded frequency word never changes. This capture's `freq_dev_hz` is hardcoded to exactly 0.0f every tick - the AD9851 is never told to change frequency at all - yet a real spectral sideband pair still shows up in the IQ capture. If the mechanism is AM-to-PM, the actual physical cause would be some periodic ripple in the ENVELOPE path (PWM/LEDC switching structure, RSET/DAC settling, or a supply/regulator ripple somewhere in the envelope-modulation circuitry) leaking into frequency/phase through the RF stage's own nonlinearity, NOT a digital computation glitch anywhere in this codebase at all. This would also cleanly explain why the same feature appeared (at a lower level) in the single-tone capture two entries ago - a real sine wave's envelope, even though ideally constant, could carry tiny real numerical ripple from the Hilbert/`fast_sqrt` chain that this AM-test path skips entirely, so a *slightly* larger amount of "real" envelope ripple in 's' mode vs. none intentionally in this `'h'`-zero-depth mode would predict opposite-direction levels from what's seen (roughly comparable, not dramatically different) - worth reconciling further, flagged as not yet fully consistent, not as a reason to discard the theory outright.

**Recommended next steps**: (1) a capture with the AM test's `AM_TEST_DEPTH` (the fixed carrier amplitude/PWM level) set to a couple of very different values, to see whether the ~16.5Hz comb's LEVEL tracks the envelope/PWM output level in any way (supporting an envelope-path-ripple-driven AM-to-PM origin) or stays fixed regardless (weakening it); (2) if there's any way to bypass or disable the envelope/PWM output path specifically while leaving the AD9851 write path active (or vice versa), that would be the single most direct discriminator between "envelope path" and "AD9851/frequency path" as the origin, cleaner than anything inferrable from spectral analysis alone. Not yet run.

## 2026-09-18, later still: CLOSED - the ~16.5Hz comb (single-tone/AM-test) tracks envelope/PWM output level as the AM-to-PM theory predicted; user attributes it to mixed power-supply domains and, given the very low level involved, is not chasing it further

User confirms the AM-to-PM prediction from the previous entry: the ~16.5Hz comb's level **tracks** the envelope/PWM output level, exactly as expected if it's genuine AM-to-PM conversion (envelope-path ripple leaking into apparent phase/frequency via the output stage) rather than a coincidence. User supplies the likely physical root cause from the actual hardware, which this investigation had no visibility into from firmware alone: the PWM GPIO output is powered from the USB-sourced 3.3V rail, while the rest of the analog chain (including the PWM filter) runs off a separate outboard PSU and local regulator generating 5V - two independent, differently-sourced supply domains feeding adjacent stages of the same signal path is a completely plausible source of the kind of low-level cross-domain ripple/coupling that would produce exactly this effect.

**Closing this specific sub-thread deliberately, at the user's explicit call**: at -65 to -73dB, this is a genuinely minor effect, now has a mechanistically sound explanation (AM-to-PM via envelope-path ripple, confirmed by the level tracking as predicted) and a plausible physical root cause (split 3.3V-USB/5V-outboard supply domains) that firmware-side investigation can't do much more with - fixing it, if ever wanted, would be a power-supply/grounding/regulation hardware change, not a DSP or firmware one. Not worth further chasing from this side. **This closure applies ONLY to the ~16.5Hz comb** (present in single-tone and AM-test-zero-depth captures, i.e. independent of two-tone content) - it does NOT close the much more significant, still-fully-open **7.7Hz comb** from three entries ago (two-tone-specific, -20dB, requires destructive-interference/near-null conditions to become significant, confirmed absent in both single-tone and AM-test captures) - that remains the live, headline unresolved question from today's MOD160 work, along with the relative_delay/IMD-nulling technique noted two entries ago as a positive, usable result.

## 2026-09-18, later still: 7.7Hz comb - refined theory and a concrete plan for the next test, using tools already built today

Re-examining everything established so far about the 7.7Hz comb together, rather than one finding at a time: its FREQUENCY is invariant to Preset, tone-pair (hence to the two-tone beat frequency `F2-F1`, which differs considerably across `TWOTONE_BAND_PRESETS` - 200Hz for most, 1000Hz for the default 700/1700 pair, 1200Hz for the "wide legacy" pair), and generator (OLD vs MOD160) - only its AMPLITUDE varies with these. It requires two-tone-style near-null events to become significant at all (absent in single-tone/AM-test, per two entries ago). Taken together, this doesn't look like anything intrinsic to the two-tone signal's own math (an intrinsic effect would very plausibly scale its FREQUENCY with the beat frequency or the tone-pair's own true period, which this doesn't) - it looks more like a fixed-frequency, always-present, otherwise negligible real-world perturbation that gets AMPLIFIED specifically by near-null sensitivity (the same `fast_atan2`-driven ill-conditioning at envelope->0 this whole investigation keeps coming back to), with the degree of amplification (not the perturbation's own rate) depending on the specific two-tone geometry - consistent with "same frequency, different amplitude" and with the fact that a much fainter version was found in yesterday's OLD-generator benchmark capture too (a small, generic perturbation, revealed wherever noise floor gets low enough and near-null sensitivity is present to amplify it).

This reframes the search: not "what in the two-tone math produces 7.7Hz" (three entries of code-side investigation into clock ratios and generator periodicity found nothing), but "what fixed-frequency ~130ms-period real-world process exists in this system, subtle enough to need near-null amplification to become visible at all." Given the class of amplification involved (phase/timing sensitivity via `fast_atan2`, not an amplitude/AM-to-PM effect like the already-closed ~16.5Hz comb), a slow TIMING perturbation - not a voltage/supply one - is the more likely candidate category.

**Concrete next test, using tools already built earlier today rather than requiring new firmware**: this project's own history already found and fixed one real Core1 ISR-contention timing bug (the gptimer-vs-ADC-driver priority fix, "CONFIRMED WIN... 7x reduction" in period jitter) - so Core1 background activity contending with the real-time path for shared resources is an established, plausible mechanism class here, not a reach. The `'v'`/`'M'` mute toggles added earlier today happen to be exactly the tool needed to test whether ongoing Serial/diagnostic print activity is a contributing factor: **capture the same MOD160/Preset 3 (or whichever configuration most reliably shows a strong comb) twice, back to back, changing nothing except muting both `'v'` and `'M'` for the second capture** (fully silencing all periodic status/timing/null_bias output AND the canary/slow_trace-auto/held_freq-auto watchers). If the 7.7Hz comb's level drops or the comb disappears with everything muted, that directly implicates Serial/Core1 diagnostic activity as a real contributor - a genuinely actionable, fixable finding, not just an explanation. If it's unchanged, that rules out diagnostics-driven contention specifically and narrows things further (toward the ADC peripheral's own idle-state behavior, thermal/PLL effects, or something else entirely).

**Secondary, cheap check worth doing alongside**: note whether the comb's exact phase/level is tied to elapsed time since power-on (consistent with a slow thermal/PLL-settling process) or resets/changes whenever `'O'`/`'T'`/a preset change happens (consistent with something reset by a mode change rather than free-running since boot) - relevant background already exists on this from the earlier `'O'` reset-behavior observations ("switch to new gen and back to old it always starts at -18Hz... seems more predictable... maybe just need more time to get unstable"), which this may connect to.

Not yet run - proposing the plan now since the user wants to keep pursuing this rather than leave it unexplained.

## 2026-09-18, later still: muted-diagnostics and loop()-delay tests come back negative for the 7.7Hz comb; two new leads surface instead - a persistent ~1Hz noise-floor "pumping" between the IMD products, and ~8Hz sidebands measured directly on the 16kHz sample-rate signal (not an exact match to 7.7Hz)

Ran the proposed test from the last entry: captured the same strong-comb config with both `'v'` and `'M'` muted vs. unmuted. Result: "not found anything" - no difference in the 7.7Hz comb between the two. This directly falsifies the Serial/Core1-diagnostic-contention theory. Checked the code to confirm the negative result is meaningful rather than muted-print-but-still-computing: the 1000ms `[timing]/[adc]/[dsp]` block is gated as `if (!s_diag_muted && now - last_timing_print_ms >= 1000)` (`diagnostics.cpp` ~line 2786) - the ENTIRE block, including the `adc_diag`/Core1-busy-breakdown reads and arithmetic, not just the `Serial.printf()` calls, sits inside that gate, so muting `'v'` genuinely stops that code path from running at all, not merely from printing. So this was a clean, real test, not a false negative from print-only gating.

Separately noted: whenever diagnostic printing IS active, it has its own clearly distinguishable signature - broadband and higher-level - visibly different in character from the narrow 7.7Hz comb. So Serial/print activity does add some noise of its own, it's just a different, already-distinguishable artifact, not the mechanism behind the comb.

Also tried changing the `delay()` value inside `loop()` (the Core 1 `delay(10)` referenced in the `[core1] busy breakdown` comment) - no difference to the comb either. Between this and the mute test, both timing-perturbation candidates proposed last entry are now ruled out by direct hardware test. The search for the underlying ~130ms-period process continues.

**New lead #1 - a previously-unremarked ~1Hz noise component**: visible as a slow "pumping" (level modulation) of the lower-level background noise floor sitting between the IMD products. Notably this was still present with both `'v'` and `'M'` muted, so - like the 7.7Hz comb - it is independent of diagnostic Serial activity too. Not yet characterized: exact rate, whether it's amplitude or phase modulation of the floor, or whether it's two-tone-specific (matching the near-null-amplification pattern) or content-independent (matching the closed 16.5Hz comb's pattern instead).

**New lead #2 - ~8Hz sidebands measured directly on the 16kHz sample-rate signal itself**: probing that signal directly (rather than the RF capture) shows low-level sidebands at roughly 8Hz spacing - close to, but per the user's explicit check, NOT an exact match to 7.7Hz. Tantalizing but unconfirmed: an 8Hz vs 7.702Hz difference is only ~4%, which could plausibly be a resolution-limited quick look rather than a genuinely different rate - the RF-side measurement that nailed down 7.702Hz precisely used a 14.7s capture (0.068Hz resolution); it's not yet known what resolution the 16kHz-signal look used. Worth a longer/higher-resolution re-measurement before treating "8, not 7.7" as decisive either way.

**Recommended next steps**: (1) re-measure the 16kHz signal's sidebands with more observation time/resolution to settle whether they truly sit at ~8Hz or converge toward 7.702Hz; (2) characterize the new 1Hz pumping with the same discriminating-test pattern already used for the other two combs (single-tone / AM-test-zero-depth, to check content-independence); (3) with diagnostics-Serial and loop-delay both now ruled out, other Core0/Core1 timing sources remain uninvestigated - FreeRTOS/system tick rate, ADC driver's own periodic housekeeping, USB CDC polling, or (if enabled) any WiFi/BT radio housekeeping. Not yet run.

## 2026-09-18, later still: CONFIRMED - 7.69Hz found directly on the 16kHz sample-rate signal itself, at proper resolution (256k-point FFT @ 48kHz Fs) - the fixed-frequency-perturbation theory is now substantiated, not just inferred

Re-measured the 16kHz signal with a much longer/finer FFT (256k-sample block at 48kHz Fs, giving ~0.183Hz resolution - an order of magnitude finer than the earlier quick look that read "~8Hz, not 7.7"). At that resolution, 7.69Hz appears clearly - matching the RF-side 7.702Hz comb spacing to within measurement precision. This resolves last entry's open question decisively: the earlier "8Hz, not 7.7" reading was indeed a resolution-limited quick look, not a genuinely different rate.

**This is the strongest evidence yet for the leading theory**: there is a real, fixed-frequency (~7.7Hz) perturbation present directly on the 16kHz sample-rate signal itself - independent of the RF chain, independent of two-tone content, independent of Serial/diagnostic activity (already ruled out), independent of the two-tone generator's own math (already shown identical under OLD and MOD160). This is no longer just an inference from the RF spectrum's behavior; it's now been measured directly at its apparent source. The mechanism proposed several entries back - a small, generic, always-present, fixed-rate timing perturbation that near-null sensitivity (`fast_atan2` ill-conditioning as envelope->0) amplifies into a prominent comb specifically in two-tone content, while leaving single-tone/AM-test essentially untouched - is now substantiated rather than speculative.

**Still open**: what physically produces a ~7.7Hz component on the 16kHz sample-rate signal itself. Not yet identified - no code-side ~7.7Hz timer, watchdog, or periodic task was found in earlier greps of this codebase, so the leading candidates are either (a) something off-chip/analog on whatever the "16kHz signal" test point actually is (needs the user to confirm exactly what was probed - the ADC sample-timer's alarm/GPIO output, the ADC driver's own conversion-done signal, or something else), or (b) a genuine crystal/PLL reference artifact (a slow beat or a switching-regulator-adjacent ripple riding on the clock source) rather than anything under direct firmware control. Recommended next step: confirm exactly what point was probed for "the 16kHz signal," then look for anything else in the system that free-runs at ~7.7Hz or a related sub-harmonic (thermal cycling of a regulator, a switching PSU's own control-loop frequency, etc.) as a candidate root cause, now that the target frequency is nailed down precisely enough to search for a match.

## 2026-09-18, later still: the "16kHz signal" is our OWN dsp-loop timing-marker GPIO, not an external/analog signal - this reconnects directly to the project's own documented pin5-jitter history and reopens the just-closed mute-test result

User clarifies: the signal that showed 7.69Hz at proper resolution is "our dsp loop pulse output - gpio5 IIRC" - i.e. one of this project's own `TIMING_DEBUG_GPIO` family of instrumentation pins (`config.h`), not anything external or analog. This changes the picture substantially.

**Which physical pin, precisely, matters and is worth confirming**: `TIMING_DEBUG_GPIO` (pin **4**) is `dsp_task`'s own per-tick busy/duty pulse - driven HIGH at the top of each ordinary tick and LOW once that tick's work finishes (`ssb_mic_test.ino` ~522/~746) - toggling at the literal `SAMPLE_RATE_HZ`=16000Hz, an exact match for "the 16kHz signal." `TIMING_DEBUG_GPIO_ISR` (pin **5**) instead toggles on every `gptimer` alarm ISR fire (`on_timer_alarm()`), which under the current v4 envelope-interpolation design runs at `ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ` (~64kHz, ~15.625us nominal period per its own comment) - not literally 16kHz. Both pins live on the same Core 1 real-time path off the same `gptimer`, so either way the implicated subsystem is the same, but which one was actually probed affects whether the ~7.7Hz shows up in whole-tick busy duration or in raw ISR-entry cadence - worth the user double-checking against the physical pin number next time at the bench.

**Reconnects directly to this project's own documented history on this exact pin**: `init_sample_timer()`'s `intr_priority=3` fix (`ssb_mic_test.ino` ~758-794) exists because real hardware showed pin5's period occasionally stretching from ~15.625us to ~19us under ISR contention with the ADC driver - confirmed fixed, ~7x jitter reduction - but that entry's own follow-up note was never closed out: *"Next to check: whether this also brings [timing]'s own wakeup-jitter max_gap_us down... and whether the audible/measured noise improves - pin5's period is upstream evidence, not the final verification."* Separately, `diagnostics.h`'s doc comment on `diagnostics_record_tick_start()` documents a SECOND, independently-confirmed instance of extra per-tick Core-1 instrumentation work measurably regressing pin5's own period/pulse-width - fixed at the time by reverting that instrumentation layer, without ever fully understanding the mechanism.

**This reopens today's earlier "not found anything" mute-test result**: `'v'`/`'M'` only gate the periodic PRINT and auto-dump side of `diagnostics_service()` (Core 1, `loop()` context). They do NOT gate `diagnostics_record_tick_start()`/`diagnostics_record_phase_timings()` - the always-on, every-single-dsp_task-tick hot-path recording calls - which run completely unconditionally regardless of `'v'`/`'M'`, and which (per the paragraph above) have a DOCUMENTED history of measurably perturbing this exact pin's timing. So the earlier mute test's negative result still stands for Serial/print/auto-dump activity specifically, but it does NOT rule out this separate, always-on, per-tick instrumentation layer as a contributor to the ~7.7Hz.

**Reframed leading theory**: the ~7.7Hz perturbation most likely lives inside Core 1's own real-time path (dsp_task tick timing / gptimer ISR cadence) rather than being an external clock or analog artifact - it's on the system's own timing-marker pin, independent of two-tone content/generator/Preset (already shown), and independent specifically of the PRINT/auto-dump slice of Core-1 work (already shown) - but the always-on hot-path recording calls are a distinct, larger, untested slice of Core-1 activity with its own documented precedent for exactly this kind of interference.

**Recommended next step**: confirm which pin (4 or 5) was actually scoped. Then, since no existing runtime toggle currently bypasses `diagnostics_record_tick_start()`/`diagnostics_record_phase_timings()`, a small targeted firmware change (compile-time or runtime flag to skip them entirely) would allow a clean A/B: with that always-on instrumentation compiled out, does the pin's 7.7Hz component change or disappear? This is the most direct test of whether the always-on recording itself - not printing - is a contributor. Not yet implemented; proposed as the next concrete step if the user wants to pursue it.

## 2026-09-18, later still: pin4 confirmed to show the same ~7.7Hz as pin5 - implicates dsp_task's own tick cadence, not one pin's toggle logic specifically; added DIAG_HOTPATH_RECORDING_ENABLED to test the always-on per-tick recording layer

User confirms the original measurement was on pin5, and repeating the same 256k/48kHz FFT measurement on pin4 shows the identical ~7.7Hz component. This is an important cross-check: pin4 (`TIMING_DEBUG_GPIO`, `dsp_task`'s whole-tick busy/duty pulse, driven from task context) and pin5 (`TIMING_DEBUG_GPIO_ISR`, raw `gptimer` alarm ISR entry, driven from real ISR context) are generated by completely different code paths sharing only the same underlying sample-tick cadence. Seeing the same ~7.7Hz on both rules out an explanation specific to either pin's own toggle mechanism and further implicates the sample-tick cadence itself - i.e. dsp_task's actual wake-up/execution timing carries a real ~7.7Hz component, not just one debug pin's instrumentation artifact.

User also volunteers a useful piece of context: recent diagnostics additions were already suspected (informally, not yet measured) of having changed the noise character slightly - consistent with treating anything that touches the dsp loop's real-time path as worth checking, per this thread's reframed theory.

**Implemented, per the user's go-ahead ("Code switch fine on that one")**: added `DIAG_HOTPATH_RECORDING_ENABLED` (`config.h`, defaults to `1` - no behavior change until the user flips it), gating the two always-on, every-tick hot-path calls identified last entry - `diagnostics_record_tick_start()` and `diagnostics_record_phase_timings()` (`ssb_mic_test.ino`'s `dsp_task()`) - inside `#if DIAG_HOTPATH_RECORDING_ENABLED`/`#endif`. `busy_us` (needed later the same tick for `diagnostics_record_jump_busy_us()`, a separate, untouched call feeding the TX-jump investigation) is computed unconditionally outside the gate; `adc_us`/`dsp_us`/`write_us` (only ever used by the now-gated call) moved inside it alongside the call itself. Braces/`#if`/`#endif` balance verified; diff is minimal and surgical - no other behavior changed. **To test**: set `DIAG_HOTPATH_RECORDING_ENABLED` to `0`, rebuild/reflash, rescope pin4/pin5 with the same 256k/48kHz FFT - if the ~7.7Hz component changes or disappears, this recording layer is implicated (matching the documented prior case in `diagnostics.h` of exactly this layer regressing pin5's timing). Note while testing: the periodic `[timing]` block will have nothing meaningful to print (high-water marks/`overrun_count` stay at their initial values) for as long as this is set to `0` - expected, not a new bug. Not yet bench-tested.

## 2026-09-18, later still: DIAG_HOTPATH_RECORDING_ENABLED=0 makes no difference either - AND the sideband tones are sharp/well-defined, arguing against jitter entirely; theory refined toward a coherent, deterministic ~7.7-7.8Hz process rather than random timing noise

Tested `DIAG_HOTPATH_RECORDING_ENABLED=0` (compiling out `diagnostics_record_tick_start()`/`diagnostics_record_phase_timings()`): no visible change to the RF output or the comb (now read as 7.79Hz - within measurement variation of the earlier 7.69/7.702Hz readings, same feature). This rules out the always-on per-tick hot-path recording layer too - the last remaining candidate from the "extra Core-1 work perturbs pin5" mechanism class this project has documented precedent for. Between this and the earlier `'v'`/`'M'` mute test, essentially all firmware-side Core-1 activity (printing, auto-dump watchers, AND the hot-path recording calls) has now been tested and ruled out as the cause.

**Important new observation, and a real refinement to the theory**: the user notes the sideband tones are "very sharp and defined," i.e. narrow spectral lines, not broadened/smeared - "so there isn't obvious jitter on that freq." This is an incisive point that changes how the mechanism should be framed. RANDOM timing jitter (occasional ISR contention, occasional stalls) produces phase NOISE - a broadened skirt or raised floor around a carrier, not discrete, narrow sidebands. Sharp, well-defined comb lines are instead the signature of a COHERENT, DETERMINISTIC periodic process at a stable, well-defined rate - i.e. something is producing an honest, repeatable ~7.7-7.8Hz oscillation somewhere in the system, not merely "noisy timing." This reframes several entries' working description of "a timing perturbation" (which had connotations of jitter/randomness) into "a small, coherent, stable periodic modulation" - consistent with, and in fact required by, the sharp-linewidth observation.

**Refined leading theory**: since (a) no firmware timer/task at ~7.7Hz has been found by code search, (b) the effect appears identically on pin4 and pin5 (two independent code paths sharing only the same underlying clock reference), (c) all Core-1 software activity that could plausibly perturb that reference has now been ruled out, and (d) the tones are sharp/coherent rather than jittery, the most consistent explanation left is a real, external, physically periodic process modulating the chip's own clock reference very slightly - most plausibly a fixed-frequency ripple or burst-mode/pulse-skip cycling from one of the switching power supplies in this system, phase-modulating the crystal/PLL (and hence every digital signal derived from it, pin4 and pin5 alike) by a tiny, coherent amount. This has direct precedent in this same investigation: the already-closed ~16.5Hz comb was attributed to AM-to-PM conversion via the mixed 3.3V-USB/5V-outboard power domains - a supply-ripple-driven explanation for a different, amplitude-coupled comb. A supply-ripple-driven PHASE modulation of the clock domain itself (rather than the envelope/PWM path) would be a distinct but related mechanism, fitting equally well with "needs near-null amplification to become visible in RF" (a tiny clock-phase wobble is invisible almost everywhere except where extreme phase sensitivity - the near-null condition - exists).

**Recommended next tests** (aimed at the supply-ripple-driven-clock-phase-modulation theory specifically, the most direct kind of test used successfully throughout this whole investigation): (1) AC-couple a scope probe directly onto the 3.3V-USB and 5V-outboard rails and FFT each one, looking for a genuine ~7.7-7.8Hz component in the supply itself - if found on one of them, that's direct physical confirmation, mirroring how the RF-side comb and the pin4/pin5 timing comb were each independently confirmed; (2) vary the power source (different USB port/cable/PSU, or a battery/linear-supply source with no switching regulator at all, if available) and see whether the comb's frequency shifts or the comb disappears entirely - a clean disappearance on a linear-only supply would be close to definitive. Not yet run.

## 2026-09-18, later still: 3.3V-USB rail scoped directly - NO 7.7-7.8Hz found there (just the expected 16kHz digital coupling plus two small, unrelated ~100Hz/~3.3Hz features) - rules out that specific supply domain for THIS comb, unlike the already-closed 16.5Hz one

User scoped the 3.3V-USB rail (AC-coupled) as proposed. Result: a 16kHz component (expected - the digital sample-tick activity coupling onto the supply as ordinary switching noise, a sanity-check that the measurement setup is working correctly) plus two other small features, both described as "very small" and near the ~10uV noise floor: ~100Hz and ~3.3Hz. Critically, **no 7.7-7.8Hz component was seen on this rail at all**.

This is a real, useful negative result: it rules out the 3.3V-USB supply domain specifically as the source of the 7.7Hz comb - in contrast to the already-CLOSED ~16.5Hz comb, which WAS attributed to this same domain (via AM-to-PM/mixed-power-domain coupling). The two combs are now doubly confirmed as separate phenomena with separate (or at least not-yet-shown-to-be-identical) root causes: different frequencies, different amplification mechanism (near-null-dependent vs. content-independent), and now different supply-domain implication.

The ~100Hz and ~3.3Hz features are noted for completeness but not yet chased: 100Hz is plausible mains-ripple-adjacent (second harmonic of 50Hz, if mains-connected equipment nearby) and may or may not be related to MOD160's own legitimately-predicted ~100Hz comb (a separate, already-understood, deliberate-design artifact - unclear whether these are the same 100Hz or coincidentally close); 3.3Hz doesn't fall out of any clean small-integer relationship to 7.7Hz (not a harmonic or subharmonic) or to any other established rate in this investigation (the separate ~1Hz noise-floor "pumping" noted a few entries back is also a different rate) - flagged as a loose end, not pursued further right now given how small it is.

**Recommended next step, per the original two-part test plan**: scope the OTHER implicated supply domain - the outboard 5V rail and its local regulator (feeding the analog/PWM-filter chain) - the same way, AC-coupled, looking specifically for ~7.7-7.8Hz there. This is the remaining half of the original plan and, with the 3.3V-USB domain now cleared, becomes the more promising candidate. Not yet run.

## 2026-09-18, later still: the outboard 5V domain isn't electrically connected to the ESP side at all (only link is the PWM signal through a 10kohm resistor) - this rules out supply-ripple-reaching-the-clock as a plausible mechanism, reopening the question of what's actually producing 7.7Hz on pin4/pin5

Important hardware-topology correction from the user: the outboard 5V rail has NO direct electrical connection to the ESP32 side - no shared power or ground path. The only link between the two domains is the PWM signal itself, passing through a 10kohm resistor (presumably the RSET/envelope current-steering path into the analog side).

**This changes the previous entry's leading theory substantially.** The "supply-ripple phase-modulates the crystal/clock reference" mechanism required SOME path for supply-domain noise to reach the ESP32's own oscillator circuit - a shared ground or power rail was the implicit assumption. With the outboard 5V domain confirmed to have no such path (only a single, one-directional-in-practice, 10kohm-limited PWM output line), there's no plausible route for ripple on THAT rail to reach back and modulate the ESP32's crystal in a way that would show up identically on pin4 AND pin5 (both ultimately derived from the same on-chip clock). Combined with the already-negative 3.3V-USB rail measurement (no 7.7Hz found there either, two entries back), **both previously-implicated supply domains are now effectively ruled out** as the direct source of the digital-side 7.7Hz signature - not just "not yet found" but structurally implausible (5V) or directly measured absent (3.3V).

**This reopens the core question**: what IS producing a coherent, sharp, ~7.7Hz component on the ESP32's own dsp_task timing pins, given: no matching firmware timer/task (checked by code search), not diagnostic printing (`'v'`/`'M'` mute test, negative), not the always-on per-tick recording calls (`DIAG_HOTPATH_RECORDING_ENABLED=0` test, negative), not random jitter (sharp/coherent lines, not broadened), and now not plausibly either implicated power-supply domain either.

**New candidate worth testing, given this is an active RF transmitter**: whether the pin4/pin5 7.7Hz component depends on the transmitter actually being active (i.e. driving real RF power via the AD9851/output stage) versus being present even with RF output disabled/disconnected. This would test a self-coupling/feedback mechanism (transmitted RF energy, or some low-frequency envelope component riding on it, coupling back into the ESP32 board's traces/oscillator circuit through proximity/radiation rather than through either supply rail) versus an origin fully intrinsic to the ESP32 module regardless of RF activity. Concretely: rescope pin4 or pin5 with the AD9851/RF output stage powered down, disabled, or the antenna disconnected, and see whether the ~7.7Hz component is still present. Not yet run - proposed as the next discriminating test now that both supply-domain theories are closed off.

## 2026-09-18, later still: DECISIVE RESULT - 7.69Hz persists with everything except the bare ESP32 board switched off - it's intrinsic to the ESP32 module itself, not RF feedback and not either external supply domain

User re-scoped with the AD9851/RF output stage, the outboard 5V supply, and everything else downstream switched off entirely - leaving only the ESP32 board itself running. The ~7.69Hz component is STILL there. This is a clean, decisive result: it rules out RF self-coupling/feedback (there's no RF being generated at all in this configuration) on top of the two external supply domains already ruled out over the last two entries. Every external explanation this investigation has proposed is now closed off.

**"Definitely in there" - the source is intrinsic to the ESP32 module itself.** Combined with everything established so far (sharp/coherent lines, not jitter; present identically on two independently-generated signals sharing only the chip's own clock; no matching firmware timer/task; not diagnostic printing; not the always-on per-tick recording calls), the remaining candidate class shifts to something physically internal to the board: most plausibly its own onboard power regulation. A well-known, named failure mode fits unusually well here: **"motorboating"** - a low-frequency (typically ~1-10Hz) parasitic self-oscillation in a voltage regulator or feedback loop caused by inadequate decoupling/compensation, classically triggered by a periodic load-current pattern exciting the loop's own low-frequency instability. This fits every constraint so far: it's a genuine, coherent, self-sustaining oscillation (explaining the sharp lines, not jitter/noise), it would appear on the chip's own supply and hence be visible on anything derived from that chip's timing (explaining pin4 AND pin5 both showing it identically), it's a known weakness of budget/compact boards with minimal onboard decoupling (consistent with this being a "Super Mini"-class board), and dsp_task's own periodic 16kHz current-draw pattern is exactly the kind of repetitive load transient that can excite this kind of instability in a marginally-compensated regulator.

**Recommended next tests, in order of how cheap/low-risk they are**: (1) add extra bulk/bypass capacitance directly across the ESP32 board's own VCC/GND (e.g. a few hundred uF electrolytic plus a smaller ceramic right at the pins) and rescope pin4/pin5 - if the comb weakens or disappears, this both confirms the motorboating theory AND is itself the fix; (2) if a current probe or a small series sense resistor is available, look directly at the board's own supply current for a ~7.7Hz oscillation, which would be direct confirmation independent of the GPIO-pin measurement; (3) as a data point (not required to act on), check whether the pin-level 7.7Hz's amplitude changes with what dsp_task is actually doing (idle/single-tone/two-tone; MAX_FREQ_DEV/PWM duty cycle differences) - if it tracks the current-draw pattern rather than staying constant regardless of content, that further supports a load-induced regulator-instability mechanism over a purely clock-intrinsic one. Not yet run - (1) is the natural next step given how cheap and informative it is.

## 2026-09-18, later still: "motorboating" theory falsified too - no 7.8Hz visible directly on the ESP32's own PSU rail, and added decoupling made no difference anywhere; the source looks to be inside the SoC package itself, beyond what's fixable at the board level

Two negative results against last entry's regulator-instability ("motorboating") theory: (1) scoping the ESP32 board's own PSU/supply rail directly shows no visible ~7.8Hz component at all - if this were a genuine voltage-domain regulator oscillation, it should appear there directly; (2) adding extra bulk/bypass decoupling capacitance (the classic, standard fix for exactly this failure mode) made no difference anywhere - neither to the pin-level comb nor (presumably) to the RF-level one. A real motorboating oscillation should both be visible on the rail and be quenchable by more capacitance - neither happened, so this theory is now considered falsified, not just unconfirmed.

A brief web search for a known ESP32-S3 erratum or documented internal-regulator/clock quirk matching this frequency turned up nothing specific (Espressif's own errata documentation and forum threads on clock-related quirks were checked, but nothing at ~7.7Hz or matching this symptom was found - inconclusive, not a ruled-out lead, just not confirmed by public documentation).

**Where this leaves the investigation**: every external, board-level, and firmware-level explanation raised so far has now been directly tested and ruled out - no firmware timer, not diagnostic printing, not the per-tick recording calls, not random jitter, not either external supply domain, not RF self-coupling, and now not a classic external-regulator instability either. What remains is something intrinsic to the ESP32-S3 SoC package itself - e.g. its internal core-supply regulation, internal clock/PLL generation, or some other on-die process not visible or fixable from outside the chip. This is a materially different category of explanation from everything tested so far: if correct, it would not be something this project's firmware or board-level hardware changes could influence at all.

**Suggested checkpoint, given how much has already been ruled out and how low the level is (~-20dB, well down)**: worth deciding whether to keep pursuing this at the SoC-internals level (which would mean digging into Espressif's own errata/design documentation with no guarantee of finding anything actionable, similar in spirit to how the ~16.5Hz comb was eventually parked once its likely cause and low practical impact were both established) or to document it here as "real, characterized, but very likely intrinsic to the SoC and not something further hunting is likely to fix" and move on. Not a recommendation either way - flagging the decision point since nearly every actionable avenue has now been tried.

## 2026-09-18, later still: built a minimal standalone reproducer (minimal_fs_test/) for process-of-elimination, per the user's proposal

Rather than keep guessing inside the full ~15-module project after every explanation so far has been ruled out, the user proposed building a from-scratch minimal sketch that reproduces only the timing skeleton, with nothing else - if that alone shows the 7.7Hz, the cause is in the skeleton itself (or lower); if it comes back clean, pieces of the real project can be added back one at a time until the comb reappears, identifying the responsible subsystem directly instead of guessing.

Built `minimal_fs_test/minimal_fs_test.ino` as its own separate Arduino sketch (own folder, own .ino - deliberately NOT a mode inside the main `ssb_mic_test.ino`, so none of the main project's ~15 modules can get linked in by accident and defeat the point of an isolated reproducer). Structurally copied verbatim from the main project's current, already-flashed, production configuration - not an approximation:
- `init_sample_timer()`/`on_timer_alarm()`: same gptimer setup (`resolution_hz=2000000`, `alarm_count=125` -> exactly 62.5us/16kHz, `intr_priority=3` - the confirmed fix for the earlier, unrelated ADC-ISR-vs-gptimer-alarm contention bug), same ISR structure (`GPIO_FAST_CLR`(pin5) on entry -> `vTaskNotifyGiveFromISR()` -> `GPIO_FAST_SET`(pin5) before return - the same "falling edge = true ISR entry, LOW pulse width = notify-call duration" convention as the real firmware). Confirmed `ENVELOPE_INTERP_FACTOR=1` is the main project's actual current production value (`envelope_interp.h`, checked directly - the gptimer really does run at plain 16kHz today, not 64kHz per some older, since-superseded comments in that file), so no interpolation-factor scaling needed for a faithful copy.
- Task: same priority/core placement as `dsp_task` (`configMAX_PRIORITIES - 2`, pinned to Core 0, so the same cross-core ISR-to-task notify hand-off is faithfully reproduced), toggling pin4 (`TIMING_DEBUG_GPIO`) HIGH-then-LOW around "the tick's work" - which here is literally nothing at all.
- Same two physical pins (4 and 5) as the main project, so the exact same scope setup and the same 256k-sample/48kHz-Fs FFT technique that found 7.69Hz on both can be reused unchanged for a true apples-to-apples comparison.
- No ADC, no Hilbert/DSP, no AD9851/carrier writes, no envelope/PWM/DAC output, no diagnostics, no serial command handling - genuinely nothing beyond the timer/ISR/notify/task-wake skeleton.

A suggested bisection order (to use if/once this minimal sketch itself comes back clean) is documented in the sketch's own header comment: (1) ADC continuous capture running but unused, (2) + the Hilbert/DSP chain running on that data (or on a two-tone generator) with output discarded, (3) + AD9851 writes actually happening every tick, (4) + envelope/PWM/DAC output and `dac_task`, (5) + the hot-path diagnostics recording re-enabled (a second, cleaner confirmation of the earlier `DIAG_HOTPATH_RECORDING_ENABLED` test result in isolation). Whichever addition first reintroduces the comb identifies the responsible subsystem with a confirmed target, rather than continuing to guess blind.

**NOT COMPILER-VERIFIED** (same caveat the main project's own top-of-file comment carries - no ESP32/Arduino toolchain available in the environment this was written in) - written by careful, direct transcription of code this project already has flashed and working, so it should build cleanly, but needs an actual Arduino IDE compile pass before flashing. Not yet built or bench-tested.

## 2026-09-18, later still: DECISIVE - minimal_fs_test alone is CLEAN of 7.7Hz (only 16kHz + small mains-adjacent 50Hz/100Hz) - proves the comb is introduced by code, not the raw timing mechanism; built step 1 of the bisection plan (real ADC capture, result discarded)

User bench-tested `minimal_fs_test.ino` (the bare gptimer/ISR/notify/task-wake skeleton, nothing else) and scoped pin4/pin5 with the same 256k/48kHz FFT method used throughout this investigation. Result: "decisive" - only the expected 16kHz fundamental plus small 50Hz and 100Hz components (almost certainly mains-adjacent pickup - consistent with the earlier, separately-noted small ~100Hz feature also seen directly on the 3.3V rail a few entries back). **No 7.7Hz at all.**

This is the cleanest result the whole investigation has produced. It proves definitively that the ~7.7-7.8Hz comb is NOT intrinsic to the ESP32's own gptimer/ISR/cross-core-notify/scheduler mechanism, and (combined with the earlier "everything except the bare ESP32 switched off" test, which had the FULL production firmware still running, just with downstream hardware physically disconnected) it further proves the cause is specifically in CODE THAT EXECUTES - not the raw timing hardware, and not anything about which external peripherals happen to be electrically connected. The remaining suspects are exactly the pieces `minimal_fs_test` deliberately left out: the ADC continuous capture, the Hilbert/DSP chain, the AD9851 SPI writes, and the envelope/PWM/DAC output path (`dac_task`) - matching the bisection order already proposed in `minimal_fs_test.ino`'s own header comment.

**Built step 1 of that plan**: `minimal_fs_test_step1_adc/minimal_fs_test_step1_adc.ino`, a new sketch folder that is `minimal_fs_test.ino` plus exactly one addition - the real ADC continuous-DMA capture (`adc_capture.h`/`.cpp`, `ssb_adc_filter.h`/`.c`, and `config.h`, all copied verbatim/unmodified from the main project into the new folder) - initialized and serviced exactly as `ssb_mic_test.ino` does (`adc_capture_init()` before task/timer creation, `adc_capture_service()` once per `loop()` iteration), with `adc_capture_read_next_sample()` called once per tick from inside the same pin4 HIGH/LOW bracket and its result immediately discarded. Nothing downstream of that read exists in this sketch - no DSP, no AD9851, no output.

If pin4/pin5 stay clean with this running, the ADC capture subsystem is cleared and the next step (DSP/Hilbert chain) should be built next. If the comb reappears here, the ADC continuous driver's DMA/ISR activity or its per-tick biquad-filter call is the confirmed culprit - notably, this project's own history already flags the ADC driver's ISR as a real source of GPIO-adjacent electrical coupling (a same-instant coupling effect from an earlier, unrelated GPIO13/GPIO3 investigation, config.h's `TIMING_DEBUG_GPIO_ADC` comment) - though a ~7.7Hz periodic effect from it would still be new and not yet understood even if confirmed to live here. **NOT COMPILER-VERIFIED** (same standing caveat as the rest of this project) - not yet bench-tested.

## 2026-09-18, later still: widened pin4's pulse for 48kHz-capture visibility (minimal_fs_test.ino and minimal_fs_test_step1_adc.ino)

User's 48kHz-sampling capture chain (an audio interface + FFT, not a scope) reported pin4's pulse as "very low energy" - with nothing between `GPIO_FAST_SET`/`GPIO_FAST_CLR` (or, in step1, only the brief ADC read between them), the HIGH time was only tens of nanoseconds, essentially a single-sample glitch at 48kHz with very little energy in-band.

Fix: added `DEBUG_PULSE_STRETCH_US` (20us) and an `esp_rom_delay_us(DEBUG_PULSE_STRETCH_US)` call, in both `minimal_fs_test.ino` and `minimal_fs_test_step1_adc.ino`, placed as TRAILING dead time right before `GPIO_FAST_CLR(TIMING_DEBUG_GPIO)` - i.e. AFTER whatever real per-tick work already exists (nothing, in the base skeleton; the real ADC read, in step 1), so the real work's own timing is completely untouched and only pin4's visibility changes. 20us leaves generous margin inside the 62.5us (16kHz) tick period even once later bisection steps add more real work on top, and a 20us boxcar's spectral first-null sits at 50kHz - comfortably above a 48kHz capture's 24kHz Nyquist edge, so the pulse stays energetic across the whole audio band rather than rolling off partway through it.

**Deliberately NOT applied to pin5** (`TIMING_DEBUG_GPIO_ISR`): pin5's LOW width is a genuine ISR-latency measurement (however long the notify call itself takes), and stretching it would mean adding a busy-wait INSIDE the ISR - holding real interrupt/scheduling activity open longer on real hardware, which is a real, if probably small, perturbation to introduce in the middle of an investigation that's specifically about small hardware-timing effects. Pin4's stretch is safe by contrast because it's pure trailing dead time after the tick's real work is already done, touching nothing upstream. Recommendation going forward: use pin4 as the primary channel for 48kHz-capture/FFT work, and keep pin5 for scope-based edge-timing work as before (where its narrow, real-duration pulse is exactly what's wanted and a scope's much higher effective sample rate has no trouble resolving it).

Both `DEBUG_PULSE_STRETCH_US` definitions are easily tunable (single `#define` near each `tick_task()`) if 20us still isn't enough, or needs to shrink for headroom once a later step (DSP/AD9851/envelope) adds more real per-tick work. **NOT COMPILER-VERIFIED** (standing caveat) - not yet bench-tested with this change.

## 2026-09-18, later still: pin4 still "very quiet" at 20us - bumped to 31us (~50% duty, the theoretical max at the fundamental), and flagged signal LEVEL upstream as the more likely bottleneck if that's still not enough

The 20us stretch (previous entry) was reported as still very quiet on the user's 48kHz capture chain. Bumped `DEBUG_PULSE_STRETCH_US` to 31us in both `minimal_fs_test.ino` and `minimal_fs_test_step1_adc.ino` - ~50% duty of the 62.5us tick period, the theoretical maximum for a boxcar pulse train's energy AT THE FUNDAMENTAL (16kHz, the exact tone this exercise needs to see) - a square wave's fundamental-harmonic amplitude follows sinc(pi*d) in duty cycle d and peaks at d=0.5, so this is close to the best pulse shape alone can achieve.

Flagged directly in both files' comments: if 50% duty is STILL too quiet, pulse shape/width is very unlikely to be the remaining bottleneck - the more likely cause becomes signal LEVEL upstream of the pulse itself (the GPIO's 3.3V logic swing vs. whatever sits between it and the 48kHz capture device - a conservatively-sized attenuator/resistor-divider for input protection, an AC-coupling capacitor removing the DC average, or the capture device/software's own input gain setting), none of which further firmware pulse-width tuning can address. Asked the user what's actually in that signal path and its gain/level setting, since that's the next thing worth checking directly rather than continuing to tune the pulse width blind.

## 2026-09-18, later still: bisection step 1 (real ADC capture added) shows a NEW low-level ~56.4Hz comb on pin4 - a different frequency from the 7.7Hz target, not yet confirmed whether 7.7Hz itself is present or still absent

User bench-tested `minimal_fs_test_step1_adc` (the clean skeleton plus the real ADC continuous capture, result discarded). Result: a new, low-level ~56.4Hz comb now appears (it was not present in the bare skeleton's decisive-clean result from two entries back). This is a genuinely useful bisection result either way - it demonstrates the ADC capture subsystem CAN measurably perturb pin4's timing signature (confirming the bisection methodology is finding real effects, not just testing for the sake of it) - but 56.4Hz doesn't match the 7.7Hz target, and no clean digital relationship to it was found by a quick check (not a small-integer ratio of `ADC_CONT_SAMPLE_FREQ_HZ`=80000, its 5000Hz callback rate, or `SAMPLE_RATE_HZ`=16000; not a match to standard 50/60Hz mains either).

**Still needs clarifying before drawing conclusions**: whether the original ~7.7Hz component is ALSO still present alongside this new 56.4Hz one (i.e. ADC capture adds an extra artifact on top, but doesn't explain the original), or whether 7.7Hz has disappeared/not reappeared while 56.4Hz is a completely separate, previously-unseen feature specific to the ADC subsystem. This distinction matters for whether ADC capture should be considered "cleared" (comb still absent -> move to step 2, DSP/Hilbert) or "implicated but not sufficient alone" (comb present alongside the new one -> still worth adding DSP on top before concluding). Asked the user directly. Not yet resolved.

## 2026-09-18, later still: confirmed no 7.7Hz at step 1 (ADC cleared, but for a different reason than assumed); important correction found - real two-tone mode never runs the per-tick ADC read at all; built step 2 (real two-tone generator + real Hilbert/DSP chain, matching production exactly)

User confirms: "no sign of 7.7 at this time" with step 1 (ADC capture added) - only the new, separate ~56.4Hz comb from last entry, no 7.7Hz. ADC capture is cleared as the direct cause.

**Important correction found while planning step 2**: checked `ssb_mic_test.ino`'s `dsp_task()` directly and found that when `audio_source_t == AUDIO_SRC_TWOTONE` (the mode the original 7.7Hz comb was found under), the entire ADC-read branch (`adc_capture_read_next_sample()`) is skipped completely - only the mic-source branch calls it. So step 1's test, while a real and useful result in its own right (confirming the ADC subsystem CAN perturb pin4's timing via the new 56.4Hz feature), was never actually exercising the code path the original bug occurs under - production two-tone mode never runs that per-tick ADC read at all. Worth remembering when interpreting step 1's clearance: it clears "ADC capture running every tick" as a cause, which is a different (broader) condition than what real two-tone mode ever does, so it doesn't by itself prove anything about the ADC subsystem's role specifically within two-tone mode (there isn't one, by construction).

**Built step 2, corrected to match production exactly**: `minimal_fs_test_step2_dsp/minimal_fs_test_step2_dsp.ino`, adding `test_signals.cpp`'s real `generate_twotone_sample()` feeding the real `ssb_dsp_process_sample()` (Hilbert FIR/atan2/sqrt chain) every tick - exactly `dsp_task()`'s `AUDIO_SRC_TWOTONE` branch, with no ADC capture at all (matching production, not step 1's broader test). `ssb_dsp_init()`'s config and the EQ/compressor-off/-2dB-gain "testing session defaults" are copied verbatim from `ssb_mic_test.ino`'s `setup()`. Both `ssb_dsp_process_sample()` outputs (freq_dev_hz, envelope) are computed for real (so the DSP math's actual execution pattern is faithfully reproduced) then discarded - nothing downstream (envelope_floor/gdeq/ampeq/predistort, relative_delay, AD9851, PWM/DAC, diagnostics) exists in this sketch yet.

Files needed: `config.h`, `settings.h`, `envelope_interp.h`, `envelope_gdeq.h` (header only - needed for `settings.h`'s type declarations, no `.cpp` since nothing calls its functions in this step), `adc_capture.h` (header only, same reason - `settings.h` needs `adc_lpf_mode_t`; deliberately no `adc_capture.cpp`, matching that this step never touches the ADC), `ssb_dsp.h`/`.c`, `dsp_state.h`/`.cpp`, `test_signals.h`/`.cpp` - all copied verbatim from the main project, all self-contained (checked each file's own `#include` list before copying, confirmed no further hidden dependencies).

If pin4/pin5 stay clean here, the Hilbert/DSP math (including `fast_atan2`/`fast_sqrt` and near-null handling) is cleared, and step 3 (AD9851 write) is next. If the comb reappears, this IS the confirmed culprit - running on the exact real per-tick two-tone path, not an approximation - and the sketch's own header comment notes a follow-up isolation (call just `generate_twotone_sample()` alone, discarding `sample` too, before `ssb_dsp_process_sample()` is added back) to separate the two-tone generator's own math from the Hilbert/DSP chain specifically. **NOT COMPILER-VERIFIED** (standing caveat) - not yet bench-tested.

## 2026-09-18, later still: step 2 (real two-tone generator + real Hilbert/DSP chain) also clear of 7.7Hz - introduces its own small 1/2/3Hz harmonic family instead, possibly connecting to the much-earlier-noted "~1Hz noise-floor pumping" lead

User bench-tested `minimal_fs_test_step2_dsp` (real `generate_twotone_sample()` -> real `ssb_dsp_process_sample()`, both outputs discarded, no ADC at all - the exact production `AUDIO_SRC_TWOTONE` code path). Result: "some approx 1, 2 & 3Hz sidebands but nothing else of significance" - **still no 7.7Hz**. The Hilbert/DSP math (including `fast_atan2`/`fast_sqrt` and near-null handling) is now cleared as the direct source too, on the exact real code path this time (not an approximation, per the correction two entries back).

Same pattern as step 1: each subsystem added so far introduces ITS OWN small, distinct artifact on pin4 (step 1: ~56.4Hz; step 2: a ~1/2/3Hz harmonic family) without reproducing the actual 7.7Hz target - reassuring that the bisection methodology is sensitive enough to pick up real, if unrelated, effects, but the target itself keeps not showing up. Worth flagging: 1/2/3Hz being harmonically related (2x and 3x a ~1Hz fundamental) suggests a non-sinusoidal ~1Hz periodic process in the DSP math's execution - possibly connecting to the still-uncharacterized "~1Hz noise-floor pumping between the IMD products" observation from several entries back (noted on the real RF signal, never followed up) - worth keeping that connection in mind if either lead gets revisited, though not confirmed to be the same thing.

**Next: step 3, AD9851 write.** This adds the actual SPI bit-banging carrier-frequency update (`carrier_output_set_freq_dev()`/`ad9851_set_frequency()`, ~20-54us of real digital I/O toggling multiple pins per tick per this project's own `[timing]` measurements) fed by step 2's discarded `freq_dev_hz` (now actually used, still with the physical AD9851 chip not required to be attached/connected for the write itself to execute and take its real time - only the resulting RF output would be meaningless without the chip, which doesn't matter for a pin4/pin5 timing-only test). Not yet built.

## 2026-09-18, later still: built step 3 (real AD9851 write added on top of step 2's real DSP chain) - deliberately excludes the delay line to keep it a single-variable step

Built `minimal_fs_test_step3_ad9851/`, extending step 2 with exactly one more real piece: `carrier_output_set_freq_dev()` (which calls `ad9851_set_frequency()` in `AD9851.c` - the real DATA/W_CLK/FQ_UD bit-bang toggle loop) fed directly by step 2's `freq_dev_hz` output (previously discarded, now actually used). `envelope` is still discarded - it isn't consumed by anything until the envelope/PWM/DAC output stage, a separate remaining step.

Deliberately did NOT also add `relative_delay_apply()` (the delay-line stage that sits between the DSP chain and the AD9851 write in production, per `dsp_task()`'s real sequence) in this same step - adding it alongside the AD9851 write would confound two new variables in one test, defeating the point of bisection. If step 3 comes back clean, the delay line becomes a candidate for its own follow-up step before moving on to step 4 (envelope/PWM/DAC output).

Copied `carrier_output.h/.cpp` and `AD9851.h/.c` verbatim from the main project - both have only standard ESP-IDF dependencies (`driver/gpio.h`, `esp_rom_sys.h`, `esp_timer.h`, `esp_cpu.h`, `soc/gpio_reg.h`, `soc/soc.h`, `driver/spi_master.h`), no other project-local files needed beyond what step 2 already carries. Confirmed `AD9851_ATTACHED` is already `1` in the project's real `config.h` (already being copied verbatim into every step folder), so this compiles in for real rather than being stubbed out - no flag needed forcing. The physical AD9851 chip does not need to be attached for a timing-only pin4/pin5 test; only the resulting RF output would be meaningless without it.

If pin4/pin5 stay clean here, the AD9851 write itself is cleared and the delay line / envelope-output stage are the only remaining untested subsystems. If the comb reappears, `carrier_output_get_profile()`'s `max_prep_us`/`max_spi_us` split can help narrow it to CPU-side FTW prep vs. the bit-bang transport itself. Not compiler-verified yet - awaiting bench test.

## 2026-09-18, later still: step 3's first real-hardware run - BOTH pin4/pin5 and the RF output look noisy/unstable, a much bigger effect than steps 1/2; added busy_us/tick_count diagnostic to disambiguate CPU jitter from electrical coupling

First bench run of `minimal_fs_test_step3_ad9851` (real AD9851 write added on top of step 2's real DSP chain). User reports: "Some two tone generation then a second of single tone. All outputs very noisy and not stable enough to look for 7Hz." Asked which measurement this described - answer: **both** pin4/pin5's GPIO capture and the RF/SDR output look bad, not just one or the other.

This is qualitatively different from steps 1 and 2, where the ONLY change was a small, contained, distinct artifact riding on an otherwise-clean, stable capture. Here the capture itself is unstable enough that reading for 7.7Hz isn't practical yet - a much bigger effect from this step's one and only change (the real AD9851 write).

Two candidate explanations for the pin4/pin5 instability, not yet distinguished:

- **(a) Genuine CPU-side jitter**: the AD9851 bit-bang write (`ad9851_set_frequency()`, `AD9851_BITBANG_EDGE_DELAY_ENABLED` currently 0 - i.e. running unthrottled/full-speed, per that flag's own 2026-09-11 revert comment) is taking a real, varying amount of dsp_task CPU time tick-to-tick, or occasionally causing a tick to be skipped/delayed outright.
- **(b) Electrical coupling, not software jitter**: the write's CPU time is fine, but the actively-toggling AD9851 pins (DATA=10, WCLK=12, FQUD=11, all bit-banged at up to 16kHz) are coupling electrically into the debug pins (4/5) or into the measurement chain itself. This project already has documented precedent for exactly this class of GPIO-to-GPIO coupling (config.h's `TIMING_DEBUG_GPIO_ADC` comment, from the earlier, unrelated GPIO13/GPIO3 investigation) - so this wouldn't be a new mechanism, just a new pair of pins exhibiting it.

Added a lightweight, non-invasive diagnostic to the sketch to tell these apart without new hardware: `s_busy_us_last`/`s_busy_us_max` (measured via `esp_timer_get_time()` bracketing the whole tick, same pattern the main sketch's own `[timing]` block already uses) and `s_tick_count` (compared each second against the expected `SAMPLE_RATE_HZ`, to catch dropped/delayed ticks, not just individually-slow ones), printed once/sec from `loop()`. If busy_us stays small and flat and tick_count stays at ~16000/sec despite pin4/5 looking noisy externally, that points at (b) electrical coupling; if busy_us is genuinely spiking or ticks are being lost, that points at (a) real jitter.

Separately flagged, NOT considered new evidence about the 7.7Hz comb specifically: this step never runs any envelope/PWM/DAC output stage at all. The real EER architecture reconstructs SSB from the AD9851's phase/frequency modulation AND the envelope's amplitude modulation TOGETHER - a receiver looking at phase-modulation-only output (constant/undefined amplitude) was never expected to sound like clean two-tone SSB, so the RF-output instability alone doesn't tell us much on its own. The fact that pin4/5 ALSO destabilized, right when this step's one change was added, is the significant part.

Not yet re-tested with the diagnostic added - waiting on the next bench run.

## 2026-09-18, later still: DECISIVE - step 3 crashes with a task watchdog (IDLE0) reset every ~11s, not just "noisy"; isolating setup-time init vs. the per-tick write, and closing a real NaN/Inf-to-int32 UB hazard found along the way

User's next message clarified the real symptom: the board is REBOOTING, not just producing a noisy-but-live signal. Serial log confirms the mechanism precisely:

```
E (12125) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (12125) task_wdt:  - IDLE0 (CPU 0)
E (12125) task_wdt: Tasks currently running:
E (12125) task_wdt: CPU 0: tick_task
E (12125) task_wdt: CPU 1: IDLE1
E (12125) task_wdt: Aborting.
```

This is IDLE0 starvation: `tick_task` (pinned to core 0, the same core IDLE0 lives on) is "currently running" at the moment of the trip and never yields long enough for IDLE0 to get scheduled - a genuine hang/hog, not a clean application-level error, and not the same failure class as anything else in this investigation so far. It repeats every reboot cycle at a consistent ~11-12s uptime (several consecutive crash-and-reboot cycles captured in the log, each with a slightly different backtrace - 0x403799b6/0x40379524/0x40380929 in most, with an extra pair of flash-cached (0x4201xxxx-range) frames on top in a few of them, consistent with different points inside the same hot loop rather than different bugs).

Since steps 0-2 (identical `tick_task` structure, gptimer/notify/priority setup, and even the same real DSP chain) all ran cleanly for extended periods, the cause has to be one of step 3's two additions on top of step 2: `carrier_output_init()` (setup()-time, once) or the per-tick `carrier_output_set_freq_dev()` call. Added `AD9851_PER_TICK_WRITE_ENABLED` (default 1) to the sketch to isolate which - set it to 0 to keep AD9851 initialized (pins configured, "enter serial mode" sequence run, one-time freq_dev=0 write in setup()) but skip the per-tick write entirely. If that alone stops the crash, the per-tick write is implicated; if it still crashes, the init sequence or the pins/hardware state themselves are.

Also found and closed a real, independent hazard while re-reading `carrier_output_set_freq_dev()` with this crash in mind: `uint32_t tx_freq = s_carrier_hz + (int32_t)delayed_freq_dev_hz;` - `(int32_t)` of a NaN or Infinity float is undefined behavior in C. This is the FIRST step in the whole bisection that actually CONSUMES `ssb_dsp_process_sample()`'s `freq_dev_hz` output rather than discarding it (steps 1 and 2 both computed it for real every tick but threw it away) - so it's also the first place a NaN/Inf from that near-null-sensitive math (this project's own `null_bias_investigation.md` history is full of near-null edge cases in exactly this calculation) could ever actually matter downstream. Added `if (!isfinite(freq_dev_hz)) freq_dev_hz = 0.0f;` before the cast in the minimal sketch, defensively - worth closing off regardless of what the per-tick-write toggle finds, but explicitly NOT assumed to be the watchdog crash's root cause by itself (Xtensa's float-to-int conversion instruction is documented to saturate rather than trap on out-of-range/NaN input, so this specific UB more likely produces a wrong-but-finite `tx_freq` than a hang - still worth fixing, just not the leading theory for the reboot).

Not yet re-tested with either the per-tick-write toggle or the NaN guard - waiting on the next bench run with `AD9851_PER_TICK_WRITE_ENABLED=0` first (isolates the crash trigger), then `=1` with the NaN guard alone if `=0` turns out clean.

## 2026-09-18, later still: DECISIVE - root cause of step 3's reboot found: our own 31us pulse-stretch pushes total tick time at/over the 62.5us budget once the real AD9851 write is added; fixed with disableCore0WDT() and a smaller stretch, likely a bisection-tooling artifact rather than a real production issue

Tested `AD9851_PER_TICK_WRITE_ENABLED=0` vs `=1` as proposed: **0 runs clean, 1 reboots** - confirms the per-tick write (not `carrier_output_init()`) triggers the crash. The `busy_us`/`tick_count` diagnostic added last entry pinpoints exactly why, straight from the `=1` log:

```
[diag] busy_us last=59 max=67  ticks_this_sec=16000
[diag] busy_us last=60 max=67  ticks_this_sec=16000
E (12125) task_wdt: Task watchdog got triggered...
```

busy_us running 59-67us is AT/OVER the 62.5us tick period (`2,000,000 / SAMPLE_RATE_HZ` half-us units) - this tick was already close to 100% duty before anything mysterious needed to be invoked. Subtracting the known 31us `DEBUG_PULSE_STRETCH_US` from the ~59-60us typical total puts the real DSP+AD9851-write cost at ~28-30us - consistent with this project's own earlier estimate of "~20-54us of real digital I/O" for the AD9851 write. The 31us figure is OUR OWN addition, added purely so pin4's pulse would be visible on the user's 48kHz-capture chain (steps 0-2 needed it too, since their own real work was small enough that the stretch was almost the whole signal) - production carries no such artificial stretch at all. So the most likely explanation: this reboot is a bisection-tooling artifact (our own visibility hack colliding with a real, but not itself excessive, per-tick cost) rather than new evidence that the AD9851 write breaks dsp_task's real-time budget in production - consistent with the fact that this exact write already runs continuously in the real, full firmware without ever tripping this watchdog.

Fixed two ways in the sketch:

1. `disableCore0WDT()` added in `setup()` - the standard Arduino-ESP32 fix for a real-time task deliberately pinned to a core and running near/at 100% duty by design, which otherwise starves that core's idle task. Stops the reboot without changing the actual timing behavior being measured.
2. `DEBUG_PULSE_STRETCH_US` reduced from 31 to 10 (this step only) - restores genuine real-time margin (~38-40us total busy time, ~35-40% idle) so pin4/pin5's capture reflects the AD9851 write's real behavior instead of a self-inflicted near-100%-duty overrun that could itself look like instability unrelated to anything the write is actually doing electrically. Trade-off: pin4's pulse reads quieter on the 48kHz capture than steps 1/2's did - raise capture gain rather than pushing the stretch back up if it's not visible enough.

This also reframes the earlier "both pin4/5 and RF output look noisy/unstable" report from last entry: that was very likely the system already in extremis (near-100%-duty, seconds from a watchdog reboot) rather than a clean steady-state read of what the AD9851 write does to timing - worth re-evaluating once a stable, non-crashing run is captured with these fixes in place. Not yet re-tested on the bench.

## 2026-09-18, later still: no more reboot, but a NEW anomaly - tick_task now runs in bursts of 3-4 samples every ~5ms with pin4 low in between, even though the ISR (pin5) stays a clean, steady 16kHz throughout

With the `disableCore0WDT()`/10us-stretch fix in place, the board no longer reboots, but bench testing found a different, real anomaly: pin4 (tick_task's own busy pulse) shows short bursts of only 3-4 pulses, then goes LOW for roughly 5ms, then another burst of 3-4, repeating - i.e. tick_task is processing at something like 600-800 samples/sec in bursts, not a steady 16000/sec. Critically, **pin5 (the real gptimer ISR's own entry/exit pulse) stays a clean, steady 16kHz the whole time** - confirmed directly by the user ("The isr output is still 16k").

This is an important, precise localization: it rules out the timer/ISR mechanism itself (gptimer is firing exactly on schedule, and `vTaskNotifyGiveFromISR` is presumably being called every single time, same as it always has been) and rules out tick_task's OWN per-sample execution time being anomalously slow (pin4 being LOW, not stuck HIGH, during the 5ms gaps means tick_task isn't busy-executing during that stretch - it's simply not being scheduled at all). That leaves FreeRTOS task scheduling/priority contention on Core 0 as the leading suspect: something is preventing an already-notified, very-high-priority task from actually running for ~5ms stretches, repeatedly. Since `ulTaskNotifyTake(pdTRUE, ...)`'s return value (the accumulated notification count before it's cleared) is currently discarded rather than used to catch up on missed samples, each ~5ms starvation period doesn't get "backfilled" - only whatever real-time ISR fires happen to land in the brief window(s) tick_task IS scheduled show up as processed samples, which would explain a small burst count (3-4) rather than either a clean single catch-up sample or a full backlog replay.

Not yet identified what's actually monopolizing Core 0 for those ~5ms stretches - nothing in this minimal sketch obviously should be (no WiFi/BT, no flash writes, no other tasks explicitly pinned to core 0 besides IDLE0, which `disableCore0WDT()` only unsubscribes from the watchdog, it doesn't remove or otherwise change the task itself). Two immediate, cheap diagnostic candidates identified but not yet tested:

1. **Is this new, unrelated to the AD9851 write?** Now that a `AD9851_PER_TICK_WRITE_ENABLED` toggle already exists in the sketch, re-test with it set back to 0 (everything else - disableCore0WDT(), 10us stretch - unchanged) to see whether the bursty pattern is actually caused by the write, or was already lurking (just masked by the reboot) even without it.
2. **Grab the `[diag]` serial lines for this exact run** - `busy_us`/`ticks_this_sec`, already instrumented, would directly quantify the burst (expect ticks_this_sec around 600-800 if "3-4 every 5ms" holds across a full second) and show whether each individual sample's own busy_us is normal (~35-40us, matching the post-fix budget) or something else is going on inside the tick body itself during the brief windows it does run.

Not yet resolved - waiting on both.

## 2026-09-18, later still: SOLVED - the bursting was caused by our own `disableCore0WDT()` fix, not the AD9851 write; removed it, reduced stretch alone should be enough

Confirmed via the requested A/B: the bursty pin4 pattern (3-4 samples every ~5ms, pin5 staying a clean steady 16kHz throughout) persists even with `AD9851_PER_TICK_WRITE_ENABLED=0` - proving it has nothing to do with the AD9851 write at all. The user's serial capture included the real smoking gun:

```
E (51042) task_wdt: esp_task_wdt_reset(707): task not found
E (51047) task_wdt: esp_task_wdt_reset(707): task not found
```

repeating continuously. This traces directly to the `disableCore0WDT()` call added in the previous fix: it removes IDLE0 from the task watchdog's registered-task list, but Arduino-ESP32's idle hook keeps calling `esp_task_wdt_reset()` for that same (now-deregistered) task on every single idle iteration regardless - since IDLE0 runs constantly (any time tick_task blocks between ticks), each of those calls fails "task not found" and logs an ESP_LOGE line. A continuous flood of failed-reset log prints, happening on essentially every idle slice, is almost certainly what was eating the CPU time that should have gone to scheduling tick_task promptly - a completely unrelated, self-inflicted bug in our OWN fix, not anything to do with dsp_task, the AD9851 write, or the real 7.7Hz investigation. This is a documented, known gotcha with `disableCore0WDT()`/`disableCore1WDT()`: they delete a task's watchdog *entry* without stopping the idle hook from still trying to feed it - the correct fix for genuinely wanting to disable the TWDT would be `esp_task_wdt_deinit()` (full teardown), not attempted here since it turned out to be unnecessary.

Removed `disableCore0WDT()` entirely from the sketch. Re-examined the reasoning for why it was added in the first place: the ORIGINAL reboot was traced to `DEBUG_PULSE_STRETCH_US=31` pushing busy_us to 59-67us (at/over the 62.5us budget) once the real AD9851 write was added. That same fix already reduced the stretch to 10us, which on its own restores genuine real-time margin (~35-40% idle, busy_us should sit around 38-40us) - `disableCore0WDT()` was very likely never actually necessary once the stretch was fixed; it was added defensively at the same time and turned out to be the wrong half of a two-part fix. Not yet re-tested with `disableCore0WDT()` removed and only the smaller stretch in place - if the reboot stays gone AND the bursting is gone, both problems trace to entirely different, now-both-fixed causes; if the reboot returns, stretch alone wasn't enough margin and a different, non-watchdog-touching fix will be needed instead.

## 2026-09-18, later still: step 3 finally runs clean (no reboot, no bursting) - reveals its own new artifacts (a bigger 1Hz comb, plus ~15.3Hz on pin5) rather than confirming or ruling out 7.7Hz directly

With `disableCore0WDT()` removed and `DEBUG_PULSE_STRETCH_US=10`, the board runs stably - no reboot, no bursting - confirming both prior fixes' reasoning (the stretch reduction alone was sufficient; `disableCore0WDT()` was never needed and was actively harmful).

First clean high-res FFT result: pin5 (the real gptimer ISR pulse) shows a 1Hz comb plus a component at ~15.3Hz. Pin4 (tick_task's own busy pulse) shows a MUCH BIGGER 1Hz comb than pin5's, with 15Hz suspected present but too low to distinguish from the noise floor.

Two things worth flagging, neither confirmed:

- **The 1Hz comb (and its harmonics) is consistent with this project's own already-established two-tone periodicity mechanism**: the two-tone generator's exact-phase-accumulator design makes the ENTIRE downstream signal chain repeat bit-for-bit every 1.000s (16000 samples) - already proven independently by simulation and real-hardware 'F' captures earlier in this investigation, and already the leading explanation for step 2's own small 1/2/3Hz artifact. If any part of the per-tick execution time is DATA-DEPENDENT (e.g. the near-null `fast_atan2`/`fast_sqrt` math taking a few cycles longer on certain input magnitudes), that variation would recur at IDENTICAL points every 1-second cycle purely because the underlying two-tone data itself is bit-for-bit periodic - producing exactly a 1Hz (plus harmonics) comb in tick_task's own timing, independent of the AD9851 write. Given step 3's real-time margin is now much thinner than step 2's (per-tick budget nearly full even after the stretch fix), the SAME underlying data-dependent jitter would show up as a LARGER pin4 pulse-width variation here than it did in step 2 - consistent with "much higher" being reported.
- **pin5 (the real ISR) showing this too is the more surprising part** - its own pulse width should reflect only `vTaskNotifyGiveFromISR()`'s call overhead, which shouldn't normally depend on tick_task's workload at all. The most likely bridge: if a given tick's execution occasionally runs long enough to still be in progress when the NEXT alarm fires (an actual overrun, more likely now given how thin this step's margin is), the ISR's own notify-give call could behave slightly differently on those ticks (different FreeRTOS ready-list/TCB state) - which would tie pin4's and pin5's periodicities to the same root cause rather than being two independent phenomena. Not confirmed.

**Numerically interesting, not confirmed**: ~15.3Hz is suspiciously close to 2x the original ~7.69Hz target. Worth keeping in mind as a possible lead (a subharmonic/alternating-cycle relationship) rather than dismissing as coincidence, though nothing here confirms a connection yet.

**Open question, asked back to the user**: is the original 7.7Hz target itself present or absent in this capture? The report so far describes two NEW artifacts (1Hz, ~15.3Hz) without stating whether 7.7Hz itself showed up alongside them, which is the one thing this whole step exists to determine.

## 2026-09-18, later still: DECISIVE - step 3 (real AD9851 write) confirmed CLEAR of 7.7Hz; every subsystem tested so far is clean, narrowing the remaining search to the envelope/PWM/DAC output stage (step 4) or cross-subsystem interaction

User confirms: "Nothing apparent at 7.7 on either [pin], and pin5 is quite quiet (-55dB from 16k tone)." The AD9851 write's own artifacts (the bigger 1Hz comb, ~15.3Hz) are real but low-level (-55dB-ish) and, per the last two entries, most likely tied to the two-tone's own whole-second periodicity interacting with this step's thin real-time margin - not the AD9851 write introducing anything resembling the 7.7Hz target itself.

Updated bisection scoreboard - every subsystem tested individually so far is clear of 7.7Hz:

- Raw timing skeleton (gptimer/ISR/notify/task-wake) - CLEAN
- + real ADC continuous capture - CLEAN of 7.7Hz (own ~56.4Hz artifact)
- + real two-tone generator -> real Hilbert/DSP chain (`ssb_dsp_process_sample()`, `fast_atan2`/`fast_sqrt`/near-null handling) - CLEAN of 7.7Hz (own ~1/2/3Hz artifact)
- + real AD9851 SPI/bit-bang write (`carrier_output_set_freq_dev()`) - CLEAN of 7.7Hz (own bigger 1Hz + ~15.3Hz artifact, -55dB-ish)

Remaining, not yet tested in isolation: `relative_delay_apply()` (the delay line between DSP and both the AD9851 write and the envelope path - deliberately skipped in step 3 to keep it single-variable) and the full envelope/PWM/DAC output stage (`envelope_floor_apply()`/`envelope_gdeq_process()`/`envelope_ampeq_process()`/`envelope_predistort_process()`/`envelope_interp_on_full_tick()`/`envelope_output_submit_dac_sample()`, plus `dac_task` itself - a SEPARATE FreeRTOS task running on **Core 1**, communicating with an MCP4725 DAC over I2C).

This last piece is a genuinely different class of candidate from everything tested so far: it's the only remaining subsystem that involves a SECOND task on the OTHER core, with its own I2C bus activity - and this project's own `ssb_mic_test.ino` header comments already flag documented historical concern about exactly this interaction ("the cache-line-stall class of problem dac_task's I2C [driver work] touching flash" - see that file's own comments around lines 169/208-211). Given every single-subsystem test on Core 0's own real-time path has now come back clean, cross-core interference from `dac_task`'s I2C activity is now the single most promising remaining candidate, ahead of (or possibly in addition to) the not-yet-isolated `relative_delay_apply()` delay line.

**Next: step 4** - add `relative_delay_apply()` plus the full envelope/PWM/DAC output chain, including standing up `dac_task` on Core 1 exactly as production does. This is a larger single step than 1-3 were (several new files, a second task, real I2C traffic) - if it reproduces 7.7Hz, follow-up work would need to narrow down further (is it the envelope math, or specifically dac_task's I2C cross-core activity) rather than treating it as fully isolated on the first pass.

## 2026-09-18, later still: built step 4 (relative_delay + full envelope chain + real PWM output) - the last remaining untested piece of the real per-tick pipeline; confirmed dac_task is NOT actually active in production right now

Before building, checked config.h directly rather than assuming: `dac_task_enabled` is currently `0` - the separate Core-1 I2C task flagged as a promising cross-core lead two entries back does NOT actually run in the real, currently-active firmware. `PWM_COMPARISON_ENABLED` is `1` (the real, active output path is the LEDC PWM leg on GPIO2 at 64kHz, going through `envelope_output_write_pwm()`), `SDM_COMPARISON_ENABLED` is `0`. This corrects the previous entry's framing - the leading remaining candidate is NOT "cross-core I2C interference" (that mechanism isn't even active), it's simply the untested remainder of dsp_task's own real Core-0 per-tick work: `relative_delay_apply()` and the envelope shaping chain feeding real PWM hardware.

Used a subagent to scope the exact real call sequence, function signatures, default-enabled states, and init order needed (`envelope_floor_apply()` -> `envelope_gdeq_process()` -> `envelope_ampeq_process()` -> `envelope_predistort_process()` if enabled, else the offset/scale mapping -> clamp -> `relative_delay_apply()` -> `envelope_interp_on_full_tick()` (real LEDC write) -> `carrier_output_set_freq_dev()` (already proven clean in step 3) -> `envelope_output_submit_dac_sample()`), then verified the highest-risk claims directly by reading the real files myself (envelope_floor_apply()'s actual signature, `relative_delay_apply()`'s full 8-parameter signature confirming NULL is safe for the 4 unused outputs, `envelope_output_init()`'s real body confirming it unconditionally installs I2C and does one MCP4725 probe write regardless of `dac_task_enabled`, and the exact init call order from `ssb_mic_test.ino`'s own `setup()`: `carrier_output_init()` -> `envelope_gdeq_init()` -> `envelope_ampeq_init()` -> `envelope_output_init()` -> `envelope_interp_init()`).

Built `minimal_fs_test_step4_envelope/` following that sequence exactly, discarding nothing this time - `freq_dev_hz` and `envelope` both flow through the ENTIRE real pipeline into real hardware (LEDC PWM duty register on GPIO2, and the AD9851 write already proven in step 3). `envelope_gdeq`/`envelope_ampeq`/`envelope_predistort` are all default-DISABLED per production's own settings, so a first pass with them left at defaults mostly exercises `envelope_floor_apply()` and the raw PWM write path - if this comes back clean, a follow-up pass toggling each stage's own `*_set_enabled()` would narrow further. Kept the same `DEBUG_PULSE_STRETCH_US=10` and busy_us/tick_count diagnostic as step 3's now-working version, same reasoning (this step's real per-tick work is at least as large as step 3's).

If step 4 also comes back clean, EVERY individually-tested subsystem in the real two-tone per-tick pipeline will have been cleared, and the leading remaining theories become either something intrinsic to the SoC package itself (the pre-bisection "everything external switched off, comb persists" finding) or an INTERACTION between multiple pieces that doesn't reproduce when each is tested alone - neither yet tested as a hypothesis. Not yet bench-tested.

## 2026-09-18, later still: DECISIVE - step 4 (full envelope + real PWM) also clear of 7.7Hz, same 1Hz comb as step 3; RF noisiness is very likely the already-documented EXACT-generator near-null glitch, NOT MOD160 (confirmed not active in this sketch)

Step 4 runs "as clean as before" (no reboot/bursting) and shows the same 1Hz comb on both pin4 and pin5 as step 3 - no new pin4/5 artifact from adding the envelope chain + real PWM write, and (implicitly, since the user didn't report anything new at 7.7Hz) still no sign of the 7.7Hz target itself. This is the FULL real per-tick pipeline now individually cleared, end to end.

User asked whether the RF output's noisiness is "the 160Gen" (`TWOTONE_PHASE_GEN_MOD160`, test_signals.h/.cpp). Checked directly: **no** - `s_twotone_phase_gen_mode` initializes to `TWOTONE_PHASE_GEN_EXACT` (test_signals.cpp:196), and none of this bisection's minimal sketches (steps 2/3/4) ever call `test_signals_next_twotone_phase_gen()`, so every one of them has been running the EXACT generator the whole time, same as production's own default - MOD160 has never been exercised anywhere in this bisection. `TWOTONE_DITHER_ENABLED` is also confirmed 0 (config.h) and never toggled here either.

Much more likely explanation, and a good one: step 4 is the FIRST step in this whole bisection where real envelope (amplitude) modulation and real frequency modulation are BOTH present together, finally reconstructing something resembling actual two-tone SSB via EER rather than a phase-only or numbers-only signal. That means this is also the first step where the EXACT generator's own already-extensively-documented near-null glitch (test_signals.h's own header: single-tick freq_dev jumps up to ~10226Hz at destructive-interference nulls, "a fixed, especially severe knife-edge... that repeats identically forever" - see the 2026-09-18 "Q1"/MOD160 entries) can actually show up as heard/demodulated RF noise - previously (steps 2/3) the numeric glitch existed in freq_dev_hz but nothing coherent was being reconstructed from it since there was no real envelope output at all. This is very likely the SAME already-known, already-well-characterized mechanism becoming audible for the first time now that the full signal chain is finally complete - not a new discovery, and not connected to the 7.7Hz hunt specifically.

**Cheap, concrete follow-up worth trying**: add one line to step 4's `setup()` - `test_signals_next_twotone_phase_gen();` (cycles EXACT -> LEGACY -> MOD160, so one call selects LEGACY, two calls selects MOD160) - and re-test. If the RF noise clears up specifically under MOD160, that's real bench confirmation of a theory that's so far only been validated by simulation (`sim_mod160_and_targeted_dither.py`) - a genuinely useful result in its own right, independent of the 7.7Hz investigation. Not yet built/tested.

## 2026-09-18, later still: BREAKTHROUGH (tentative) - the ~7.69Hz-like signal IS visible under both LEGACY and MOD160 (on pin4 AND pin5); EXACT's own already-documented 1Hz comb very likely masked it throughout this entire bisection

Following up on the previous entry's cheap MOD160 follow-up: user tried it, first reported "Confirmed a feature of MOD160," then immediately retracted that: "No I was wrong. I can see it but lower level on legacy gen on pin5." Given this exact thread already flagged a real risk of miscounting `test_signals_next_twotone_phase_gen()` calls (one call lands on LEGACY, not MOD160), asked four precise clarifying questions before trusting either report: which mode(s) actually show it, whether EXACT is confirmed clean or just untested, which pin(s), and which sketch.

User's answer, precise: on the default EXACT generator, the already-known 1Hz comb (present in every one of steps 2/3/4's own results) is dominant enough that it's impossible to tell whether 7.69Hz is present underneath it - EXACT is **not confirmed clean, only masked/inconclusive**. Under LEGACY, the 7.69Hz-like signal is there (lower level) on **both pin4 and pin5**. Under MOD160, it's **also** there, on both pin4 and pin5.

Went back to `test_signals.cpp` to understand exactly what the three modes share and don't. The relevant switch (`generate_twotone_sample()`, lines ~303-324):
- **EXACT**: both tone1 and tone2 phase recomputed fresh every tick via `fmodf(s_tone_sample_index * f_hz / SAMPLE_RATE_HZ, 1.0f) * two_pi`, where `s_tone_sample_index` is a SHARED counter wrapping at `SAMPLE_RATE_HZ` (16000 samples = exactly 1 second).
- **MOD160**: identical recompute formula, but from `s_tone_sample_index_160`, a SEPARATE shared counter wrapping at 160 samples (10ms - the 700/1900Hz-class tone pair's true fundamental period, gcd=100Hz).
- **LEGACY**: both tones use INDEPENDENT per-tone float accumulators (`s_tone1_phase +=`, `s_tone2_phase +=`, subtract `two_pi` on overflow) - no shared discrete index at all, and no periodic recompute; this is the only mode where tone1 and tone2 aren't both driven from the same underlying counter.

The interesting structural fact: LEGACY and MOD160 are about as different from each other as two phase generators can be - continuous independent accumulation with no shared index, versus discrete recompute from a small (160-sample) shared index. Yet **both** show the 7.69Hz-like signal. EXACT is the one mode that differs from both of them the same way (a much larger, 16000-sample shared wrap period) - and it's the one mode that can't be read at all right now, because of its own artifact sitting on top of the search target.

**Leading hypothesis, still tentative**: 7.69Hz-like content may be present in the real signal chain regardless of which two-tone phase-generator implementation drives the tones, and EXACT's own big 1Hz(+2Hz+3Hz) comb - already seen in EVERY bisection step so far (step 2's "1/2/3Hz artifact," step 3's "bigger 1Hz + ~15.3Hz," step 4's "same 1Hz comb"), and specifically tied to EXACT's own 16000-sample (1-second) wraparound discontinuity - has likely been swamping the noise floor in exactly the 1-10Hz region 7.69Hz sits in, throughout the ENTIRE bisection (steps 1-4). If this holds up, it reframes every "clear of 7.7Hz" conclusion reached so far: those subsystems were clear of 7.7Hz **under EXACT specifically**, not necessarily clear in an absolute sense - none of them have actually been tested under a phase-gen mode whose own artifact doesn't sit on top of the search target.

**Explicitly not yet confirmed**: "7.69Hz-like" is being used loosely here pending a proper high-res-FFT read at 7.69Hz specifically (exact frequency, level in dB, any harmonics) under LEGACY and MOD160, with the same rigor as every earlier step's result - two different real signals can coincidentally look similar at a glance. Also not yet known: WHERE in the pipeline this content originates, only that it appears to be independent of which two-tone phase-gen implementation drives the tones. Since steps 1-4 all ran EXACT the whole time, none of them have actually been tested with the masking artifact removed - the bisection needs to be at least partially redone under LEGACY or MOD160 to find out at which subsystem 7.69Hz first appears once the mask is gone. Simplest candidate to check first: step 2's DSP-only chain (no AD9851, no envelope) - if 7.69Hz-like content is already there, that points to the generator/DSP math itself (independent of phase-gen implementation) or something even further upstream, rather than any later hardware-output stage.

**Built to test this**: repurposed `minimal_fs_test_step2b_dsp_mod160/` (previously just an unedited copy of step 2) rather than hardcoding a fixed number of `test_signals_next_twotone_phase_gen()` calls in `setup()` - that approach already cost this exact thread real time and ambiguity once (the MOD160-vs-LEGACY mix-up above, and a near-repeat of the original "did the call count land where we think" risk flagged three entries back). Instead added a serial-triggered cycle in `loop()` bound to `'o'` - same key binding as the real firmware's own `'O'` handler in `serial_commands.cpp` - printing the active mode's name via `test_signals_get_twotone_phase_gen_name()` after every press. One flash now lets EXACT/LEGACY/MOD160 be A/B/C'd on demand with the active mode explicitly confirmed every time, rather than trusted from a call count. Step 2's own scope is otherwise unchanged (real `generate_twotone_sample()` into real `ssb_dsp_process_sample()`, both outputs discarded, no ADC, no AD9851, no envelope) - the earliest, simplest point in the real per-tick pipeline. Not yet bench-tested.

Also noted: step 4's existing MOD160-switch edit (two hardcoded `test_signals_next_twotone_phase_gen()` calls + a confirmation print, added directly to `minimal_fs_test_step4_envelope.ino` in response to "added the two calls into setup - how to print now?") is left in place for now but should probably be converted to the same serial-cycle convention, for the same reason. The on-device copy of step 4 in the user's Downloads folder predates this MOD160-switch edit and is now stale.

## 2026-09-18, later still: DECISIVE - step2b (bare generator + DSP math, no AD9851/envelope/ADC at all) confirms 7.7Hz on pin4, a PURE CPU-timing pulse - EXACT -60dB (nothing), LEGACY -56dB, MOD160 -43dB; the effect is real and doesn't need any hardware output stage

Bench result from `minimal_fs_test_step2b_dsp_mod160` (real `generate_twotone_sample()` -> real `ssb_dsp_process_sample()`, both outputs discarded, no ADC/AD9851/envelope anywhere in the sketch), read on pin4 specifically: **EXACT** - no 7.7Hz, noise floor -60dB; **LEGACY** - 7.7Hz present at -56dB; **MOD160** - 7.7Hz present at -43dB, the strongest of the three.

This is a significant tightening of the previous entry's tentative conclusion. Pin4 in this sketch is a PURE CPU-timing pulse - `GPIO_FAST_SET` at the top of `tick_task()`, a FIXED `esp_rom_delay_us(31)` stretch for scope visibility, then `GPIO_FAST_CLR`. It carries no output/signal content whatsoever; the only thing that can vary its shape at all is how long the real work between the SET and the stretch actually takes each tick. So a genuine, mode-dependent 7.7Hz component on pin4 means the generator+DSP code's own EXECUTION TIME is being perturbed at ~7.7Hz, and the size of that perturbation depends on which two-tone phase-gen mode is active (essentially absent under EXACT, moderate under LEGACY, strongest under MOD160).

Two things this rules out immediately: (1) this is NOT introduced by the AD9851 write, the envelope/PWM chain, or the ADC - none of those exist anywhere in this sketch, and the effect is already present and mode-dependent at this, the earliest and simplest point in the real pipeline; (2) it's not merely "EXACT's own big comb happens to swamp a smaller real 7.7Hz signal that's equally present in all three modes" - the level actually varies substantially BY mode (-60 / -56 / -43dB), so mode choice itself measurably changes something about the underlying process generating this timing perturbation, not just how visible it is.

Not yet explained: WHY would the *choice of phase-generator implementation* (EXACT/LEGACY/MOD160 all mathematically intend to produce the same two sine waves) change how long `generate_twotone_sample()` and/or `ssb_dsp_process_sample()` take to execute, at a consistent ~7.7Hz rate? The leading candidate remains this project's own long-documented near-null/data-dependent code paths (`fast_atan2`/near-null handling in the Hilbert chain) - if near-null events (or some other data-dependent branch) recur at ~7.7Hz under LEGACY/MOD160's specific phase trajectories but not under EXACT's, and that branch takes measurably different time, this would be exactly consistent with what's measured. Not confirmed - the mechanism inside `ssb_dsp_process_sample()` hasn't been isolated from the generator itself yet.

**Next isolation, built into the same sketch rather than a new one**: added a runtime `'d'` toggle (matching the `'o'` cycle's own serial-command convention) that gates the `ssb_dsp_process_sample()` call on/off while `generate_twotone_sample()` always still runs every tick regardless. This lets all 3 phase-gen modes be tested both WITH and WITHOUT the Hilbert/DSP math in a single flash (6 combinations total) - directly answering the question step 2's own original header comment already anticipated ("isolated by testing generate_twotone_sample() alone first... before concluding it's the DSP math specifically rather than the generator"). If pin4's 7.7Hz persists with `'d'` toggled off (DSP math skipped, only the raw generator running), the effect is in the generator's own phase math (or even something like `sinf()`'s data-dependent timing on certain inputs); if it disappears, it's specifically inside the Hilbert/atan2/sqrt chain, reacting to the different phase trajectories the three generator modes produce. Not yet bench-tested.

## 2026-09-19: confirmed the effect needs the DSP chain (disappears with 'd' off); new hypothesis for how this transfers to the actual AD9851 RF output - WRITE-TIME jitter, not signal content - and built step5_resync to test the user's proposed fix

Bench confirmation of the previous entry's isolation test: with MOD160 selected, DSP chain ON gives the highest 7.7Hz comb yet seen on pin4; toggling `'d'` to skip `ssb_dsp_process_sample()` (generator still runs) makes it disappear completely. This pins the mechanism specifically inside the Hilbert/atan2/sqrt chain, reacting to MOD160's phase trajectory - not the generator's own phase math, and not something like `sinf()`'s general timing behavior.

User's own follow-up question, and a genuinely new angle this investigation hadn't considered: pin4's effect is proven to be pure CPU EXECUTION-TIME jitter (no hardware writes exist anywhere in step2b) - so how could that possibly show up as an actual frequency/RF-domain artifact once real hardware (step3/4, production) is added back? The AD9851 write (`carrier_output_set_freq_dev()`) and the PWM write (`envelope_interp_on_full_tick()`) both happen at the END of `dsp_task`'s per-tick work, immediately after the Hilbert/DSP chain returns. Since that chain's own execution time is proven to jitter (data-dependently, mode-dependently), and the write happens right after it finishes, the ACTUAL WALL-CLOCK MOMENT the write occurs - and therefore the moment the AD9851's output frequency actually changes - jitters by exactly the same amount, relative to the one genuinely fixed clock in this whole system (the gptimer ISR driving Fs). For a DDS-driven FM/PM system this is not a subtle effect: write-time jitter converts directly into phase/frequency noise, a well-known mechanism in exciter/synth design, independent of anything being wrong with the AD9851 chip itself. `carrier_output_set_freq_dev()`'s underlying bit-bang loop has a fixed iteration count (confirmed earlier in this investigation - no data-dependent timing of its own), so the lever here is entirely WHEN within the tick the write happens, not what the write itself does.

**User's proposed fix**: rather than writing this tick's computed result at the end of this (variable-duration) tick, buffer it and write it out as the very FIRST action of the NEXT tick - before that tick's own DSP work starts - "re-syncing" the write's timing to the ISR-driven tick boundary, the closest thing to a true Fs edge this task can reach.

Built `minimal_fs_test_step5_resync/` (direct extension of step4) implementing exactly this as a one-tick buffered pipeline: `s_pending_freq_dev_hz`/`s_pending_envelope`, set at the end of one tick (after `relative_delay_apply()`, so already delay-line-adjusted) and written to hardware as the first action of the next tick, using that tick's own `t_start_us` as the write's timing reference. Both outputs are deferred TOGETHER (not just frequency alone) specifically so their existing `relative_delay_apply()`-calibrated relative alignment is preserved - only their shared latency increases by exactly one sample period (62.5us), imperceptible for real-time voice. Runtime toggle `'w'` (default RESYNC) A/B's this against the original step3/4/production "write immediately" behavior on the same flash; runtime toggle `'o'` (step2b's own convention, replacing step4's old hardcoded-call-count MOD160 select) cycles EXACT/LEGACY/MOD160, with MOD160 the most sensitive test case per step2b's own numbers (-43dB, the strongest).

**What would confirm this**: with `'w'` RESYNC and MOD160 selected, does the actual RF/pin5 7.69Hz comb measurably reduce or disappear compared to `'w'` LEGACY under the same mode? That's the direct test of whether write-time jitter is really the transfer mechanism from a proven CPU-timing curiosity into a measurable RF artifact, or whether something else is also involved. Not yet bench-tested.

## 2026-09-19, later still: pin5's falling edge confirmed rock-solid (the true Fs reference); built step6_isr_write to write the AD9851 directly from the timer ISR - but only after finding a THIRD, directly-relevant prior failure and scoping the design around it

Clarified pin5's own behavior for the user: it pulses low-then-high on every ISR firing (`GPIO_FAST_CLR` then `vTaskNotifyGiveFromISR()` then `GPIO_FAST_SET`), not a single edge - the FALLING edge is the true Fs reference (driven purely by gptimer's own hardware alarm counter, `alarm_count = 2,000,000 / SAMPLE_RATE_HZ` on a free-running 2MHz peripheral clock), and the RISING edge marks the end of the ISR's own work (today just the notify-give, "negligible, sub-100ns" per `config.h`'s own comment).

User then scoped it directly: **pin5's falling edge is rock-solid**, low for "up to about 7us" - and since `tick_task` only runs AFTER the ISR returns, the AD9851 write's own timing shift (already proven via step2b/step5) sits entirely downstream of that solid edge, in the cross-core ISR-to-task hand-off plus tick_task's own work. Asked whether the AD9851 could be written directly from the ISR itself, to remove that hand-off entirely - the tightest possible sync to Fs this system can achieve, tighter even than step5's resync (which only moved the write to task-START, still after the hand-off).

Before building, checked this project's own history for prior attempts in this exact area, per the user's direct question ("did we try something similar in the isr and failed previously??"). Found **three**, not zero:

1. **`ssb_mic_test.ino` ~L921-952**: pinned `dsp_task` to Core 1 (matching gptimer's ISR core) to eliminate the cross-core hand-off. Real hardware: Serial output and command handling went **completely dead** - `loopTask` starved of Core 1 entirely, despite dsp_task's own measured duty cycle predicting plenty of idle time. RF/audio output itself stayed normal. Root cause never diagnosed - reverted.
2. **`ssb_mic_test.ino` ~L956-972**: moved `init_sample_timer()` itself into `dsp_task`'s body (Core 0). Real hardware: regular reboots, every `[timing]` number got WORSE (max busy_us to 62us, right at budget; ADC actual rate collapsed; pool overflows appeared). Reverted.
3. **`envelope_interp.cpp`'s header comment + `group_delay_fit_notes.md`'s "SDM first hardware result, and the direct-from-ISR commit follow-up"/"REVERTED" entries (2026-09-08)** - by far the most directly relevant, and NOT one either of us had surfaced yet in this thread: a stage-in-task/commit-from-ISR split was already built and bench-tested for the SDM envelope-output leg (a sibling of PWM, and *documented ISR-safe* on this chip - `sdm_channel_set_pulse_density()` - unlike PWM/LEDC, whose own doc references "v1's direct-ISR PWM attempt crashed outright"). Real hardware result: **worse, not better** - "the jitter is still there, maybe even worse," and adjusting the now-one-tick delay didn't recover the loss. Two suspected mechanisms, worked through by reading the actual driver docs rather than guessing: (a) `sdm_channel_set_pulse_density()`'s IRAM residency is conditional on a Kconfig option (`CONFIG_SDM_CTRL_FUNC_IN_IRAM`) that can't be confirmed or changed from Arduino-ESP32's fixed, precompiled core - if flash-resident, every call from the highest-priority ISR risks a cache-miss stall that *also* delays the following `vTaskNotifyGiveFromISR()`, compounding rather than just adding; (b) a **staging race** - if `dsp_task` is ever still finishing its own (already-documented-as-sometimes-late) per-tick work when the next alarm fires, the ISR commits a value staged MORE than one tick ago, an irregular, load-dependent hiccup rather than the clean constant one-tick offset the design assumed - genuinely NEW jitter, not just a fixed delay.

**Design consequence**: `minimal_fs_test_step6_isr_write/` (extends step5) moves ONLY the AD9851 write into the ISR - envelope/PWM stays task-side, unconditionally, via step5's existing resync/legacy scheme (`'w'`), specifically BECAUSE of failure #3 above. The AD9851 write is judged safe on a different basis than SDM was: `ad9851_set_frequency()` (`AD9851.c`) is `IRAM_ATTR` and does nothing but raw `REG_WRITE(GPIO_OUT_W1TS_REG/W1TC_REG,...)` register pokes plus one small cycle-counted busy-wait - no ESP-IDF driver call, no flash-residency ambiguity, no FreeRTOS API, no allocation, no locks - confirmed by reading the source directly, not assumed. But mechanism (b), the staging race, applies in principle to ANY stage/commit-from-ISR split regardless of what's being written, so it's built to be MEASURED rather than hoped against: a `s_stage_seq` counter increments every time `tick_task` finishes staging a fresh value, and the ISR remembers the sequence number it last saw - a match on the next firing means it's about to commit a value it already committed once before (tick_task fell behind), counted in `s_stale_commit_count` and printed once/sec. Revert path is a compile-time `ISR_WRITE_ENABLED` flag (not runtime) specifically because failure #1's canary was Serial dying outright - a runtime toggle sent over Serial can't be trusted if Serial itself is the thing that breaks; with the flag at 0 this sketch's behavior is bit-for-bit identical to step5_resync. Not yet bench-tested - this is genuinely the highest-risk sketch built in this whole investigation, going into a seam with three prior undiagnosed failures, even though the specific change here is much smaller and better-justified than any of the three.

## 2026-09-19, later still: step6's first bench run panicked (Coprocessor exception) - a FOURTH prior failure mode this project's own docs had already named, missed in my own audit; fixed by moving the float->int conversion out of the ISR

First flash of `minimal_fs_test_step6_isr_write` crash-looped immediately: `Guru Meditation Error: Core 1 panic'ed (Coprocessor exception)`, `EXCCAUSE: 0x00000004`, rebooting continuously every few seconds (the boot banner, MCP4725 probe line, and one `[diag]` print with `stale_commits=0` all printed cleanly before each panic - so the crash happens on essentially the very first ISR-write attempt, not something rate- or load-dependent).

Root cause: the ISR called `carrier_output_set_freq_dev(float delayed_freq_dev_hz)` directly, and that function does `s_carrier_hz + (int32_t)delayed_freq_dev_hz` - a float-to-int32 cast, which requires Xtensa's floating-point coprocessor. Using it inside an ISR without the coprocessor context set up for that panics with exactly this EXCCAUSE. This is not a new category of failure for this project - `group_delay_fit_notes.md`'s own SDM postmortem, which step6's header already quoted when justifying the design, explicitly distinguishes the SDM-commit-from-ISR case from "v2's ISR crash (float/Hilbert math in ISR context, an FPU-coprocessor-context problem) - not applicable here since committing a pre-computed int8_t involves no float math at all." That sentence was flagging exactly this failure mode by name, and it applied to step6's design too - my own audit checked `AD9851.c`'s integer bit-bang loop for ISR-safety but never checked `carrier_output.cpp`'s thin wrapper in front of it, which is where the float arithmetic actually lives. A real, avoidable gap in the pre-flash review, not a hardware surprise.

**Fix**: split `carrier_output_set_freq_dev()` (in step6's own copy of `carrier_output.h`/`.cpp` only - other steps' copies and the real firmware are untouched) into `carrier_output_compute_tx_freq(float delayed_freq_dev_hz)` (the float->int conversion, called from `tick_task` in task context, where FPU use is normal and safe) and `carrier_output_set_freq_dev_raw(uint32_t tx_freq)` (pure integer register pokes via `ad9851_set_frequency()`, no float anywhere, safe from an ISR). `carrier_output_set_freq_dev()` itself is unchanged and now just calls both in sequence, so every other caller (steps 3/4/5) behaves identically to before. The `.ino`'s pending buffer gained a new `s_pending_tx_freq` (already-converted `uint32_t`, computed in `tick_task` at staging time) alongside the existing `s_pending_freq_dev_hz` (float, still used by the `!ISR_WRITE_ENABLED` task-side path) - the ISR now reads only `s_pending_tx_freq` and calls the raw integer function, never touching a float. Not yet re-bench-tested after this fix - the crash-loop needs to be confirmed gone before any of the actual RF/FQ_UD measurements this sketch exists to produce mean anything.

**Standing lesson for any further ISR work in this project**: the "is this safe to call from an ISR" checklist now has three concrete failure modes on record, not two - no FreeRTOS/blocking calls (failure #1/#2's territory), no flash-resident/driver calls without confirmed IRAM placement (SDM's failure), and no floating-point arithmetic anywhere in the call chain (this one) - and checking the leaf function alone isn't enough; every wrapper between the ISR call site and that leaf needs the same audit, since that's exactly where this one hid.

## 2026-09-19, later still: BREAKTHROUGH CONFIRMED - step6 (AD9851 write from the ISR, envelope resync task-side) fixes the close-in noise floor on real hardware, in EVERY two-tone phase-gen mode, with no sign of 7.7Hz anywhere - the write-time-jitter theory holds up

Re-flashed after the Coprocessor-exception fix (previous entry) - clean boot, no panic. User's own words: "Spectacular!!! that's the breakthrough I've been searching for. Noise floor very low close in now in all gen cases. No sign of 7.7Hz." This is the result the entire bisection (steps 1 through 6, going back to the original ~7.7Hz two-tone comb this whole investigation started from) was built to find.

Three separate, independent confirmations reported together: (1) the close-in noise floor is now low under EXACT, LEGACY, AND MOD160 alike - not just LEGACY as step5's resync alone had shown (10dB better) with only ~1dB from MOD160; (2) "the dsp jitter seems better too, the end of ISR is more stable linking to the start of the dsp - all look more controlled on scope" - i.e. pin4/pin5's own timing relationship is visibly tighter on the scope, independent of the RF measurement itself; (3) "no sign of 7.7Hz" - the original target this whole project has been chasing since long before this bisection began.

**New, not-yet-understood finding**: "Mod160 noise floor the best but not its raw IMDs... The IMD signatures are different in each case." So MOD160 (the cleanest phase-gen mode by construction - no index-magnitude-dependent float32 rounding, see its own header history) gives the best close-in noise floor but NOT the best raw two-tone IMD numbers, and each of the three modes has a visibly different IMD signature rather than one simply being strictly better than the others. This is a genuinely new, open question this result exposes rather than resolves - not yet investigated at all.

**User's own next move, and the right one**: rather than continuing further isolated bisection, fold these two fixes (AD9851 write from the ISR; envelope/PWM resync task-side) back into the FULL production system (`ssb_mic_test.ino`) and re-characterize from there, now that the isolated minimal-sketch testing has done its job. This closes out the bisection phase of this investigation - see moving_forward_notes.md for the punch list of what's worth checking on this clean skeleton before/while doing that fold-back, and the open MOD160-IMD-signature question to carry forward into the full-system phase.

## 2026-09-19, later still: first quantitative FFT read of an off-air two-tone capture under MOD160+ISR-write - 7.7Hz residual confirmed still present but pushed down to about -58dB, no longer the dominant close-in artifact

User supplied `SDRuno_20260919_102243Z_14175kHz.wav` (18.19s IQ capture, fs=388888Hz, "MOD160 with new isr writes") as a benchmark reference. Ran a full-length windowed FFT (bin width ~0.055Hz, comfortably fine enough to resolve a 7.7Hz-spaced comb).

Two tones found at 25700.57Hz and 26700.52Hz (recording-relative), spacing 999.95Hz - matches the project's established 700/1700Hz two-tone preset. Odd-order IMD ladder around them: 3rd order (2f1-f2/2f2-f1) at -28.4/-27.9dB, 5th order (3f1-2f2/3f2-2f1) at -27.0/-27.1dB, additional products out to 7th-8th order down to about -30 to -36dB, each spaced by the ~1kHz tone spacing - this is the raw two-tone IMD signature the user flagged in the last breakthrough report as "different in each case" and not yet understood; this file is the first hard number for MOD160 specifically (roughly -27 to -28dB on the closest, dominant products).

Close-in view (within 20Hz of either tone) found the 7.7Hz-and-15.4Hz comb this whole investigation has been chasing IS still measurably present - a real, symmetric pair of sidebands at ±7.7Hz on both tones, and a second harmonic at ±15.4Hz - but now down at about -58dB and -61dB respectively. That's a large, genuine improvement (the equivalent pure-DSP-timing measurement on step2b's pin4 put MOD160's 7.7Hz component at -43dB before any of the resync/ISR-write fixes existed - direct dB-for-dB comparison isn't valid since the reference is different, but the qualitative direction - much smaller now - is consistent with the user's own "no sign of 7.7Hz" scope read). It's a fair, non-contradictory finding: on a spectrum analyzer scaled to see the (much bigger) IMD ladder and other artifacts, a -58dB residual comb would not be visible, so "no sign of 7.7Hz" and "a small trace is still measurably there in a full quantitative FFT" are both true statements about the same signal.

**New, more prominent artifact found instead**: the dominant close-in sidebands on both tones are actually at about ±0.33Hz, at -23 to -25dB - much bigger than the 7.7Hz residual and not yet explained. A second, smaller pair sits at about ±0.7Hz (-34dB), and a third at about ±1.0-1.1Hz (-46dB). A ±0.33Hz spacing corresponds to a roughly 3-second period, which doesn't match any of this project's known periodicities (the two-tone generator's 1-second EXACT-mode wrap, MOD160's 10ms wrap, or the 7.7Hz mechanism itself) - candidate causes not yet ruled in or out: something in the SDRuno/receive chain (AGC ripple, a receive-side buffering artifact) rather than the transmitter, since this is an off-air capture and the receive chain hasn't been part of this bisection at all; or a genuinely new transmit-side periodicity that the earlier bare-generator/DSP-only tests (which never ran with the real, continuous, whole-signal RF chain assembled together) wouldn't have shown. Not investigated further - flagged as the next thing worth a look if it recurs in future benchmark captures, since it's now the largest unexplained close-in artifact in this signal, ahead of both the (now much smaller) 7.7Hz comb and the raw IMD ladder.

Spectrum plots (wide IMD-ladder view and a close-in zoom around 7.7/15.4Hz) generated and delivered to the user for the record, alongside this capture, as the first saved reference baseline for a specific generator+fix combination (MOD160 + ISR-write). Full context and the punch list this capture partially executes is in `moving_forward_notes.md`.

## 2026-09-19, later still: folded the full step6 fix (resync + AD9851-from-ISR) into the real production firmware (ssb_mic_test.ino) - not yet bench-tested

User's own read on the benchmark capture: "Overall I'm going to guess legacy will be best for IMDs but need full system to fully test that." Asked directly which fix to fold in - confirmed via AskUserQuestion: the FULL step6 breakthrough (resync + AD9851 written from the ISR), not step5's resync-only fix, since that's what actually produced the clean bench result.

Ported into the real firmware rather than another isolated minimal sketch, since the bisection phase is done and the user's own next move (stated in the previous breakthrough entry) is to go back to the full system. Two new compile-time flags in `config.h`, both default ON: `TX_WRITE_RESYNC_ENABLED` (buffer envelope+freq, write both at the start of the next tick) and `AD9851_ISR_WRITE_ENABLED` (write the AD9851 leg directly from `on_timer_alarm()`, envelope/PWM stays task-side) - deliberately compile-time only, matching step6's own reasoning (a runtime toggle sent over Serial might not be reachable if Serial itself is what breaks). `carrier_output_compute_tx_freq()`/`carrier_output_set_freq_dev_raw()` (the crash-fix split from step6) ported into the REAL `carrier_output.h`/`.cpp`, not just a step folder's local copy - every other caller of `carrier_output_set_freq_dev()` is unaffected. The staging-race diagnostic (`s_stage_seq`/`s_stale_commit_count`) came along too, wired into the real `diagnostics.cpp`'s existing periodic `[timing]` print block via a new `diagnostics_record_isr_stale_commits()` call, rather than a standalone print - `stale_commits=0` throughout step6's own bench validation, but production's dsp_task is a much larger, more complex real-time body (full envelope chain, real ADC, ENVELOPE_INTERP_FACTOR machinery) than step6's minimal skeleton, so this is worth actually re-confirming on real hardware, not assumed to carry over.

One production-specific wrinkle handled: dsp_task's ISR fires at `ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ` in general, and with FACTOR>1 the ISR can't tell by itself which firing is a true full-tick boundary (that's resolved in software, in dsp_task, specifically to tolerate coalesced wakeups). Checked directly: `ENVELOPE_INTERP_FACTOR` is 1 in the current config (the v4 interpolation feature is effectively inert right now), which collapses this to exactly step6's own case - every alarm IS a full tick - so the ISR write is safe as implemented. Added a `#error` guard (`AD9851_ISR_WRITE_ENABLED` + `FACTOR != 1`) so this doesn't silently misbehave if that factor is ever raised later without re-visiting this code.

`diagnostics_set_tx_info()`'s `tx_freq`/`delayed_freq_dev_hz`/`delayed_envelope` arguments were kept exactly as they were (this tick's own freshly-computed, internally self-consistent tuple) rather than trying to report what's physically on the AD9851 at the exact moment of the call - under resync that tuple describes a write that won't physically land until the START of the next tick, a uniform one-tick queueing delay. Reasoned through why this doesn't need to be fixed: the jump-log/near-null diagnostic logic only ever looks at deltas and cross-alignment between freq_dev and envelope from one tuple to the next, never at absolute wall-clock truth, so a constant shared shift across every sample doesn't change anything it computes - a real design decision, not an oversight, documented inline at the call site for whoever reads this next.

Not yet bench-tested - this is a compile-only port, reviewed line-by-line (balanced `#if`/`#endif` nesting checked with a script, full control-flow trace confirming the new top-of-tick resync block only executes on genuine full, non-chirp ticks) but this project has no local compiler/toolchain to verify against, same standing caveat as every minimal test sketch. Needs an actual flash + the same MOD160/LEGACY/EXACT off-air comparison the benchmark capture started, now with `stale_commits` and the real envelope/mic/ADC pipeline in the loop for the first time.

## 2026-09-19, later still: user asked whether 'O' still works after the fold-in - confirmed yes, and the shrunk on-SDR difference between modes is the fix working as intended, not a bug

Verified directly: `test_signals_next_twotone_phase_gen()`/`s_twotone_phase_gen_mode`/`generate_twotone_sample()`'s mode `switch` in `test_signals.cpp` are completely untouched by the resync/ISR-write fold-in - the fix only changes WHEN/HOW `freq_dev_hz`/`envelope` get written to hardware, never how the two-tone samples themselves are generated. 'O' is cycling the mode correctly (confirmed by the print), and the DSP chain is genuinely processing a different phase trajectory each time.

The likely reason the difference is now hard to see on the SDR: before this fix, the dominant, easily-visible difference BETWEEN modes was the ~7.7Hz comb itself - MOD160 showed it ~13-17dB stronger than LEGACY on the bare pin4 timing measurement (step2b), a large, obvious difference. That comb is exactly the mechanism this fix removed, in every mode - the benchmark capture confirmed it's down to a small, roughly mode-independent residual (~-58dB) now. What's LEFT to distinguish modes is only the intrinsic raw two-tone IMD difference between how each one computes phase - a few dB on specific IMD orders (the benchmark capture put MOD160's 3rd/5th order products around -27 to -28dB) - genuinely subtle enough that it won't jump out on an ordinary SDR waterfall/spectrum by eye. Confirming a real mode-to-mode difference now needs the same kind of quantitative FFT comparison done on the MOD160 benchmark capture, repeated for EXACT/LEGACY, not a visual/audible A/B - already flagged as the next step in both logs.

## 2026-09-19, later still: first real-hardware scope read of the folded-in fix - "ISR to dsp start" is now bimodal (20.4us or 24.1us, ~300ns jitter within each), and the whole pin4/5 picture reads as much more stable

User's own report, first time scoping this exact transition since flashing the fold-in: "that transition looks a lot more stable now... the whole pin4/5 picture just seems much more stable. ISR to dsp start is 20.4 to 24.1us and its one or the other with a variation on those times of maybe 300ns."

Two things worth separating here - one expected/good, one genuinely new and unexplained:

**The ~20-24us overall duration is expected, and is good news, not a regression.** Under `AD9851_ISR_WRITE_ENABLED`, the AD9851 bit-bang write now happens INSIDE `on_timer_alarm()`, before `vTaskNotifyGiveFromISR()` and before pin5's rising edge - so "ISR to dsp start" (however the user is triggering/measuring it) necessarily now includes that write's own execution time, which this project's own earlier measurements already put in the 20-54us range (the [timing] `write_us`/ad9851-breakdown figures, `carrier_output.cpp`'s own comments). Previously this same write happened at the END of dsp_task's own tick, buried inside its variable-duration DSP work - moving it into the ISR is exactly what was supposed to isolate its timing from dsp_task's own jitter, and "the whole pin4/5 picture looks more stable" is consistent with that having worked as intended.

**The tight ~300ns jitter WITHIN each of the two states is itself a strong positive signal.** It means the write's own execution time is now close to fully deterministic, in sharp contrast to the ORIGINAL finding that started this entire investigation (step2b: the DSP chain's own execution time jittering by enough to produce a measurable ~7.7Hz component). Isolating the AD9851 write into its own ISR, ahead of anything else, appears to have made ITS timing rock-solid too, not just protected it from dsp_task's jitter.

**The ~3.7us bimodal split ("one or the other") is new and NOT yet explained.** Read `AD9851.c`'s `ad9851_set_frequency()` bit-bang loop directly to check for a data-dependent cause (different bit patterns taking different numbers of cycles): found none - the loop is a fixed 5-byte/40-bit iteration, each bit doing the exact same fixed sequence of GPIO pokes regardless of its value (`AD9851_BITBANG_EDGE_DELAY_ENABLED` is 0, so no per-bit delay branch either; only one FIXED end-of-transfer `ad9851_edge_delay()` call, unconditional). So a clean, evenly-split two-state timing difference of this size doesn't look like it comes from the transfer's own data-dependent behavior. Candidate causes not yet checked: (1) a GPIO-peripheral/clock-domain-crossing effect - back-to-back writes to the GPIO_OUT_W1TS/W1TC registers cross from the CPU clock domain into the GPIO matrix's own clock domain, and if that phase alignment is itself quantized relative to when the gptimer alarm fires, it could plausibly produce exactly two discrete latency buckets; (2) something upstream of the ISR - whether another same-or-lower-priority interrupt (the ADC's own conv_done ISR is the obvious candidate, already the subject of this project's `intr_priority=3` fix) happens to be active or not at the moment the alarm fires. Not confirmed either way yet.

Next diagnostic proposed (not yet run): check whether the "slow"/"fast" state correlates with anything already observable - e.g. does it alternate on a fixed period (every other tick, every N ticks), or track `stale_commits`, or correlate with ADC activity (pin39/TIMING_DEBUG_GPIO_ADC, if enabled)? A fixed alternating pattern would point at (1); a correlation with another async event would point at (2). Not urgent (the split is small relative to the tick budget and both states are individually rock-solid) but worth understanding before treating this fix as fully characterized.

## 2026-09-19, later still: user corrects the "stable" read - real dsp-start variation is ~4.6us, earliest 20.5us after ISR start - and connects it to a suspected occasional WCLK "double" pulse, potentially reconnecting to the old, never-fully-resolved edge-quality thread

Self-correction from the user: "I can see the AD9851 write (as grnd noise) on the ISR and that has about 1.2us duration variation. I'd seen Wclk occasional double within the write seq when I was investigating the edges and I think that is also happening here... the max variation I see on dsp start is about 4.6us. The earliest it starts is 20.5us after ISR start." This supersedes the previous entry's "tight ~300ns jitter, cleanly bimodal" read - the real picture is a ~4.6us spread, floor at 20.5us, and now has a concrete, physically-observed candidate mechanism instead of two speculative ones (GPIO clock-domain crossing / ADC-ISR collision).

**Why this could matter more than ordinary timing jitter.** An occasional genuine extra W_CLK pulse mid-transfer wouldn't just cost time - if it's a full, valid-width logic transition (not just ringing/overshoot on a single edge), the AD9851's own shift register would load an extra bit, shifting every subsequent bit in that 40-bit word out of alignment for the rest of the transfer. That's a data-integrity problem, not a jitter problem - a wrong frequency write on that tick, not just a late one. Worth distinguishing which this actually is before assuming it's benign.

**Connects to already-documented, not-fully-closed history**: `AD9851.c`'s own comments and this project's 2026-09-10/11/16/17 entries extensively covered exactly this signal - the BS170 inverting level-shifter's slow, passive, pull-up-charged LOW-to-HIGH edge, named as "the leading theory for the two-tone-specific 'sticks' symptom," with W_CLK's AD9851-side rising/sampling edge specifically called out as one of the two affected transitions (alongside a fresh DATA 0-to-1 bit). Push/pull drivers were fitted 2026-09-17 specifically to fix this (actively driving both directions instead of passive pull-up) - confirmed same inverting polarity, `AD9851_BITBANG_EDGE_DELAY_ENABLED` correctly left at `0`. Result at the time: "no visible improvement" on the closely related ~153.6s STUCK/RECOVER cycle - i.e. the push/pull fix did NOT cleanly close this thread out, which is exactly why a residual "WCLK double" is plausible even with the faster drivers in place, not a contradiction of that hardware change having been made.

**What's new here that wasn't tested before**: the AD9851 write has never previously run from ISR context - always from `dsp_task` (task context) until this session's fold-in. Worth ruling in/out whether ISR context itself changes anything electrically (e.g., whether the CPU can retire back-to-back GPIO writes measurably faster/tighter at this point in the pipeline than it could from task context, giving the level shifter or push/pull driver's own drive electronics less settling time between transitions than before).

**Cheapest, most direct next test, using the exact A/B toggle already built for this fix**: flip `AD9851_ISR_WRITE_ENABLED` to `0` in `config.h` and reflash (task-side write, `TX_WRITE_RESYNC_ENABLED` alone, otherwise identical code/electrical path) and rescope WCLK. If the "double" still occurs at the same rate task-side, it's independent of the ISR-write fix - a pre-existing signal-integrity issue this fold-in simply made newly visible (because it's now the dominant thing sitting between the true Fs edge and dsp start, easy to scope). If it's ISR-specific or worse under ISR-write, that's a real, new cost of this fix worth weighing against its 7.7Hz benefit. Not yet run.

**Other next steps, not yet run**: (a) determine electrically whether the "double" is a full second valid-width pulse or ringing/overshoot on one edge - the datasheet's minimum W_CLK pulse width sets the bar for what counts as "real" to the AD9851's own input logic; (b) check whether it correlates with specific bit-transition patterns (still consistent with the original BS170-era theory if it clusters on fresh 0-to-1 transitions, even under push/pull drive); (c) the already-proposed, never-yet-run FQ_UD-latch-only edge delay re-enable (`AD9851_BITBANG_EDGE_DELAY_ENABLED`'s FQ_UD-specific sibling) is still on the table as a cheap mitigation to try regardless of root cause.

## 2026-09-19, later still: RESOLVED - "WCLK double" was never a doubled pulse; it's per-bit W_CLK pulse-width variation (70-126ns) from running the bit-bang loop with zero edge delay, and that variation alone accounts for the observed write-duration and dsp-start jitter

Two pieces of new evidence from the user, back to back, close this thread out cleanly. First: "Wclk as far as I can scope give poitive pulses of 70 to 126ns. I'm confident the gpio drive matches the output to the AD9851 and its not noise related. In fact it looks quite clean on my 100Mhz bndw scope." Second, directly answering the open count-vs-quality question from the previous entry: "There are always 40 wclk pulses and when wider ones occur the whole seq is a little wider." Forty pulses, always - the data-integrity fear (an extra shifted-in bit misaligning the whole 40-bit word) is ruled out. There was never a genuine doubled pulse. What's varying is the WIDTH of individual pulses within a fixed-count sequence, and the user's own scope confirms it's clean, not ringing/overshoot/noise.

**Re-reading `AD9851.c` explains why, precisely, and predicts this exact behavior.** `AD9851_BITBANG_EDGE_DELAY_ENABLED` is `0` (reverted 2026-09-11, still off) - meaning the per-bit W_CLK toggle in `ad9851_set_frequency()`'s hot loop has ZERO added software delay on either edge: `fast_gpio_clr(handle->pin_wclk)` is followed immediately by `fast_gpio_set(handle->pin_wclk)`, two back-to-back single-instruction MMIO register writes with nothing in between. On the raw ESP32 GPIO pin, this pulse is only a few CPU cycles wide (~4-16ns at 240MHz) - far narrower than anything meaningful. The 70-126ns positive pulse the user is scoping isn't that raw GPIO pulse - it's what the BS170 inverting level-shifter turns it into. `AD9851.c`'s own long-standing comments (the `AD9851_BITBANG_EDGE_DELAY_ENABLED` and `ad9851_edge_delay()` headers) already document that this level shifter's LOW-to-HIGH transition is a slow, PASSIVE, pull-up-charged RC edge, while its HIGH-to-LOW transition is fast and actively driven - a real, physical asymmetry, not noise. With no software delay holding the raw pulse open, the AD9851-side pulse width we actually see is essentially the level-shifter's own RC rise time playing out - and that RC time constant is not perfectly fixed pulse-to-pulse: it depends on the exact charge state of the node going in (set by whatever the DATA line and the previous W_CLK transition just did), so it legitimately varies bit-to-bit within a normal, expected range. That's a clean, deterministic analog effect, exactly matching "not noise, looks quite clean."

**This also arithmetically closes the write-duration-variation and dsp-start-jitter threads from the last two entries, without needing a separate mechanism.** The user measured ~1.2us of duration variation across the whole 40-bit AD9851 write, and a 70-126ns per-pulse range gives up to ~56ns of width difference per bit. Over 40 bits, only about 20-22 of them need to land on the wide end of that range (not noise-random - a real, reproducible function of the bit pattern/prior state) to account for the entire ~1.2us observed - i.e. ordinary per-bit RC-charging variation, summed across a fixed 40-pulse transfer, is fully sufficient to explain both the write-time spread and, downstream of it, a meaningful share of the ~4.6us dsp-start variation reported two entries ago. No separate "double pulse," no ISR-specific electrical anomaly, and no new data-integrity risk needs to be invoked.

**What this changes about the standing to-do list**: the previously-proposed `AD9851_ISR_WRITE_ENABLED=0` A/B test (to check whether the "double" was ISR-context-specific) is no longer needed to explain THIS effect - it was never about ISR vs. task context, it's a property of the un-delayed bit-bang loop itself and would look the same either way. It's still fine to run for other reasons (isolating any other ISR-specific timing question), just not as a fix for a WCLK-doubling theory that no longer applies. The BS170/push-pull edge-quality thread this reconnected to two entries ago is still open in its own right (the 2026-09-17 push/pull fitting reported "no visible improvement" on the unrelated ~153.6s STUCK/RECOVER cycle) - this finding doesn't close that thread, it just establishes that THIS particular symptom (per-bit width variation, 70-126ns, clean) is expected/benign given the current `AD9851_BITBANG_EDGE_DELAY_ENABLED=0` setting, not a new problem. If tighter, more uniform W_CLK pulse widths are ever wanted (e.g. to shave the residual dsp-start jitter further), re-enabling the per-bit edge delay - or the already-independent `AD9851_FQUD_EDGE_DELAY_ENABLED` sibling mechanism applied per-bit instead of just at FQ_UD - is the direct lever, at the previously-measured real-time cost (~13-14us budget, per the 2026-09-10 entries) of doing so.

## 2026-09-19, later still: CORRECTION - the 70-126ns WCLK pulse width is set mainly by the ESP32's own GPIO/APB bus round-trip latency, not by the BS170 level-shifter's RC rise time as the previous entry claimed

User pushed back with cited real-world data: direct register GPIO toggling (`GPIO.out_w1ts`/`out_w1tc`) on ESP32 parts is documented (GitHub issue, Arduino/esp32.com forum threads) to be limited to roughly 8-16MHz in back-to-back C-loop toggling - about 60-120ns per transition - regardless of how fast the CPU can issue the store instructions. This is a real, well-documented artifact of the APB bus + GPIO matrix round-trip between the CPU issuing the write and the pin actually changing state, not an instruction-issue-rate limit. It lines up with the measured 70-126ns far more precisely (near dead-center overlap) than the previous entry's "raw GPIO pulse only ~4-16ns wide, stretched by the BS170's RC-charging rise time" claim, which is hereby corrected/superseded.

**Revised mechanism**: the pulse width is very likely set mostly AT THE ESP32 PIN ITSELF by this bus synchronization latency, before the BS170 level-shifter gets involved at all - not manufactured by the level-shifter's analog RC edge as previously claimed. The bit-to-bit width variation within the 70-126ns range is better explained by ordinary APB bus/pipeline jitter (this is a dual-core part sharing the same GPIO/APB infrastructure - Core 0 bus traffic, interrupt latency, bus arbitration all plausibly contribute cycle-level variance) than by level-shifter RC charge-state dependence.

**What doesn't change**: every conclusion from the previous entry that actually mattered still holds, because it's mechanism-agnostic - always 40 pulses (no data-integrity risk), clean/not noise (bus-latency jitter is a real deterministic-ish digital effect, not electrical noise), and the arithmetic tying a ~56ns per-pulse range across roughly half of 40 bits to the ~1.2us AD9851 write-duration variation (and onward to the ~4.6us dsp-start jitter) works identically regardless of whether that 56ns comes from bus latency or RC charging.

**What DOES change - the lever, if tighter WCLK uniformity is ever wanted**: `AD9851_BITBANG_EDGE_DELAY_ENABLED` only ADDS delay on top of whatever floor exists; it can't remove a bus-latency floor. The ESP32-S3's dedicated/"fast" GPIO peripheral (CPU-instruction-driven GPIO access that bypasses the standard APB/GPIO-matrix round-trip this project's `fast_gpio_set()`/`fast_gpio_clr()` still goes through, despite the "fast" name referring only to skipping `gpio_set_level()`'s function-call/critical-section overhead, not this bus path) is the documented way around this specific bottleneck if it's ever needed - not yet evaluated or attempted in this project, parked here as a new option distinct from anything tried so far.

## 2026-09-19, later still: pivoting to analog/PWM-filter tuning - PSU rework may have shifted the DC-critical filter operating point; measurements pending

With the write-time-jitter fix now folded in and stable, and the WCLK pulse-width question resolved/corrected above, the user is moving on to a different, previously-parked thread: re-checking the envelope PWM output filter's analog tuning. Context: the 5V supply rail has since been reworked and is now "a proper 5V" (previously it wasn't), and the PWM low-pass/reconstruction filter feeding the envelope path is DC-level-critical - its correct operating point depends on that rail being accurate, so a supply correction like this could plausibly have shifted where the filter now sits relative to where it was last tuned/verified. Nothing measured yet; the user is going to take fresh measurements and report back before any conclusion or change is drawn. Logged here as a placeholder/context marker for that pending data, not a finding.

## 2026-09-19, later still: built REVISION 7 of the envelope predistort table from the new-PSU duty sweep - found a real, modest, non-uniform shift concentrated in the turn-on knee, plausibly linked to the PSU fix

User uploaded `DutyMeasNewPSU-1.txt`: an exhaustive direct duty sweep, every count 0 through 1023 (1024 points, one reading each), taken after the 5V PSU rework, with the request to check and prepare a new distortion table as needed. This is the same measurement family `envelope_predistort.h`/`.cpp` already tracks through 6 prior revisions (a lookup table correcting the static, memoryless envelope-amplitude nonlinearity of the whole PWM/BS170/AD9851 chain) - REVISION 6 (previously current) was built from a duty=1..1023 sweep before this PSU fix.

**Processing, same pipeline as REVISIONS 4-6**: dBm -> linear amplitude (10^(dBm/20)), isotonic regression (PAVA, non-decreasing) to smooth single-count noise while enforcing monotonicity, normalize to [0,1] using the isotonic floor/ceiling, PCHIP-invert (envelope -> duty) sampled at 65 even envelope points. New this time: the sweep includes duty=0 itself for the first time (every prior revision started at duty=1), and has no repeated reading at duty=1023 (REVISION 6 had a 3-reading repeatability check there; this sweep has one reading per duty, a minor known reduction in confidence at that one anchor point).

**Did the PSU change actually move anything?** Floor (duty 0-3, isotonic-tied) reads -87.13 to -87.16dBm here vs. REVISION 6's -87.24dBm (duty=1) - only ~0.1dB different. Ceiling (duty=1023) reads -28.38dBm vs. REVISION 6's -28.80dBm - a bigger, ~0.42dB difference. Floor and ceiling moving by different amounts rules out "just a uniform calibration/attenuation difference between sessions" (that would shift both ends equally) - there's a real shape change to explain.

Isolated the same way REVISION 6 separated its own two stacked causes: reconstructed REVISION 6's dBm-vs-duty curve from its stored LUT (inverting the 65 points back through its own floor/ceiling), then compared this sweep's directly-measured dBm at those same duty values, duty-for-duty, after removing the uniform +0.42dB ceiling-anchored offset. The residual is NOT flat - it's ~0 at duty=1023 (anchor, zero by construction) and duty~900+ (within ~0.03dB), drifts to about -0.1 to -0.25dB through the upper-mid climb (duty 400-850), and dips to its worst, about -1.6dB, right in the turn-on knee around duty~200-210, before recovering to about -0.3dB near the floor (duty~1).

**Interpretation**: a dip concentrated in the turn-on knee, converging to near-zero at both the very bottom and the very top, is the same qualitative signature REVISION 4->5's confirmed PNP-bias-starvation fix produced - bias-current-dependent nonlinearity shows up hardest in the transition region, not at the flat extremes. This is consistent with, though not conclusive proof of, the PSU fix shifting the DC-critical filter's operating point slightly. No controlled same-session A/B against the old (off-spec) PSU exists to compare against, so ordinary session-to-session measurement variation (different day, connector reseat, ambient temperature) can't be fully ruled out as a contributing or alternative explanation - recorded as a real open uncertainty, not papered over. What's clear regardless of exact cause: the effect is small (at most ~1.6dB) compared to the tens-of-dB shifts REVISION 5->6 traced to deliberate hardware redesign - this reads as a refinement, not a red flag.

**Table built and adopted as REVISION 7** (`envelope_predistort.h`/`.cpp` updated, REVISION 6 marked superseded): LUT[0]=0.0000 (duty=0, the true hardware floor for the first time, not a "lowest tied duty" stand-in); LUT[64]=1.0000 (duty=1023) anchors to REVISION 6's ceiling by construction; through the climb, commanded duty runs a few PWM counts (roughly 4-9) above REVISION 6's at the same table index, tracking the knee-region dip found above. Resolution check (65-point piecewise-linear vs. the dense 1024-point ground truth): excellent through 5-95% envelope (RMS and max error both under 1 count, matching REVISION 6's own quality there); large apparent error in the bottom 0-5% of envelope (RMS ~63 counts, max ~105 counts at duty=127) - this is the same well-documented "dead-zone-plus-turn-on collapses into the first bin" effect every revision since REVISION 4 has flagged (real duty needs to sweep ~0 to ~190 within envelope 0-1.5%, which one straight LUT segment can only approximate crudely), not a new defect; moderate error in the 95-100% top plateau (RMS ~11 counts, max ~19 counts), similar in kind to REVISION 4's own documented top-plateau limitation.

**Status**: `env_predistort_enable` is off by default, same convention as every prior revision - this data update doesn't change anything currently running. NOT YET VALIDATED on real hardware beyond the measurement itself, and a two-tone/IMD re-check matters more than usual here, precisely because the moved region is the bias-sensitive knee - exactly the kind of thing a static AM table can't confirm on its own. Full derivation, LUT values, and the build script are in this session's scratchpad (`build_predistort_rev7.py`) if the analysis needs re-checking.

## 2026-09-20: checked new aAD/aADgB group-delay TFs (shelf1+shelf2+predistort, new PSU) - gdeq's LTI mechanism still checks out exactly, but candidate B now WORSENS full-band dispersion even though it still helps the local IMD-band slope; no confident coefficient improvement found without a real IMD bench test

User uploaded two hi-res TF sweeps (`HiRes_aAD_TF_New_5V_psu.txt`, gdeq off; `HiRes_aADgB_TF_New_5V_psu.txt`, gdeq candidate B on) - both with ampeq shelf1+shelf2 ('a'/'A') AND envelope predistort ('D') enabled, which the user notes wasn't included in any previous TF sweep this project has on record. Asked to check for any group-delay improvement.

**Processing**: parsed both files (freq/magnitude/phase, ~5.86Hz steps, 0-8000Hz). The first 2-3 points (f=0, 5.86Hz) are placeholder values (mag=1.0, phase=0.0 exactly) and the next few points up to ~30Hz are noisy low-SNR artifacts - trimmed before smoothing, same "check for measurement-chain junk before trusting a derivative" discipline as every fit in this file. Group delay extracted via Savitzky-Golay smoothed derivative of phase (tau = -(1/360)*dphase_deg/df), window-convergence-checked (200-4200Hz spans) - settled on a 700Hz window, matching this project's established default. A handful of narrow single-point spikes in the raw data (~2300Hz, ~2650Hz, ~4000Hz, ~5300Hz - present in BOTH files at the same frequencies, so a shared measurement artifact, not a real filter feature) add minor noise to point-level metrics but don't change the overall shape or conclusions.

**Cross-validation (mechanism check, not the main question, but important)**: reconstructed the predicted aADgB curve as (measured aAD bare curve) + (candidate B's own exactly-known analytic delay, `ssb_allpass1_t`'s tau(w)=(1-a^2)/(1+2a*cos(w)+a^2) formula, a=0.09 cascaded twice) and compared against the ACTUALLY measured aADgB curve: RMS residual only 3.8us across 100-8000Hz (max 29.7us, only right at the 7998Hz sweep edge - a smoothing-window edge artifact, not a real discrepancy), and the predicted mean delay contribution (125.3us) matched the measured mean delay shift (125.7us) almost exactly. This confirms gdeq's basic LTI-additivity mechanism is completely unaffected by predistort's presence or the new PSU - reassuring, since it means any real-world result below is a genuine curve-shape effect, not a broken assumption.

**The actual finding**: over the full 100-8000Hz fit band, candidate B (a1=a2=0.09) now makes dispersion WORSE than running with gdeq off entirely - bare p-p=119.7us vs. candidate-B-on p-p=160.0us, a real ~34% regression. But in the specific 2800-4500Hz sub-band gdeq has always been evaluated against (the two-tone/IMD-relevant region), candidate B still does what it was designed to do: local linear-fit slope improves from -19.2us/kHz (bare) to -11.1us/kHz (candidate B on). So candidate B hasn't stopped working - it's still trading full-band flatness for local-band quality, exactly as documented when it was first characterized against the old (no-predistort) aA curve - but the trade appears sharper/less favorable on this new curve.

**Searched directly against this real curve for a better fit** (same grid+Nelder-Mead/differential-evolution discipline as every fit in this file): the best achievable full-band p-p with a symmetric 2-section cascade is 96.5us (a1=a2=-0.182, a genuine ~19% improvement over bare's 119.7us) - but on the SAME curve this candidate nearly DOUBLES the 2800-4500Hz local dispersion (38.1us -> 65.7us p-p, slope -19.2 -> -36.8us/kHz) relative to bare. That is exactly the AA candidate's own well-documented failure signature (full-band win, local-band loss, and by that candidate's own real-hardware history, a plausible far-out-spur risk this project has already burned a bench test ruling out once) - **not recommended for that reason**, not adopted. Swept the full plausible a1=a2 range (-0.4 to +0.4) to check whether ANY single coefficient does well on both metrics simultaneously: no - full-band p-p and local 2800-4500Hz slope trade off sharply and monotonically against each other across this whole range (chart: `gdeq_aAD_analysis.png`), the same hard single-real-pole-allpass ceiling this project already found for the pre-predistort aA curve, just re-confirmed on the predistort-inclusive one. Candidate B's existing a=0.09 sits at a reasonable, non-extreme point on that frontier - not obviously beatable without abandoning the current 2-section architecture (3/4-section exploratory searches found only marginal further full-band gains: 96.5 -> 93.9 -> 92.9us, same diminishing-returns pattern as this project's very first 10000Hz-vs-16000Hz cascaded-section comparison).

**Honest caveat on the "did predistort change the curve" question**: this project's header comments quote several different historical numbers for the pre-predistort aA bare/equalized curves (bare-analog-alone, bare-analog+shelf1+shelf2, default-gdeq-on-aA, etc., at slightly different fit bands), and I don't have the actual raw historical TF file (`ga_Trial2_TF.txt`/`HiRes_aA_TF.txt`) on hand to do a clean, direct, same-methodology comparison the way the cross-validation above was done. Rather than eyeball-comparing today's numbers against remembered/summarized historical figures (a mistake this session already made once with the WCLK pulse-width theory), I'm flagging this as an open, checkable question rather than asserting predistort caused the shift - if that old raw file still exists, re-running today's exact same extraction pipeline on it would give a rigorous, apples-to-apples answer instead of a guess. Given predistort is a memoryless, monotonic nonlinearity, its own theoretical group-delay contribution is exactly zero - the more likely explanation for any real shift is the new PSU (see the REVISION 7 predistort-table entry above, which already found the PSU changed something in the same general signal chain) rather than predistort itself, but this is a hypothesis, not a confirmed finding.

**Recommendation**: no group-delay coefficient change is recommended from this data alone. Candidate B still looks like the same reasonable, deliberate compromise it always was - the underlying trade-off shape (good local slope, so-so full band) is confirmed to persist with predistort in the loop, not resolved or worsened into something new. Per this project's own repeated, hard-won lesson (both existing candidates looked better on paper than they turned out to be in a real two-tone spectrum), any actual coefficient change should go through the same real IMD bench comparison this project has insisted on every time before, not be adopted from group-delay math alone. Full analysis script and chart in this session's scratchpad (`gdeq_aAD_analysis.png`) if this needs re-checking.

## 2026-09-20, later still: shelf1-only TF (`HiRes_aD_TF_New_5V_psu.txt`) has no phase column, so the requested shelf1-only gdeq fit can't be built yet - but its magnitude alone surfaced a real, concrete lead on the user's "A makes IMDs worse" observation

User asked for a gdeq coefficient set fit specifically for shelf1-only ('a' on, 'A'/shelf2 off - see previous entry's discussion of why no existing variant covers this case), motivated by shelf2 apparently making real IMDs worse now, and said "I'll do a TF." Uploaded `HiRes_aD_TF_New_5V_psu.txt`.

**Can't build the fit from this file**: unlike yesterday's two files (`FRF(Channel 2,Channel 1)` with `X[Hz]`/`Log[1]`/`Phase` columns), this one has only two columns (`X[Hz]`, `dB [Ref 1.00e+00 1]`) - magnitude only, no phase at all - and sweeps a much wider range (0-24000Hz, 4097 points) than the previous 0-8000Hz phase+magnitude sweeps. Without a real measured phase column there's no group delay to extract - the same "measure the real thing, don't reconstruct/assume it" discipline this file's every prior gdeq fit has followed. Need a re-export in the same format as `HiRes_aD_TF.txt`'s predecessors (mag AND phase, `FRF(Channel 2,Channel 1)`) before a shelf1-only fit can be built - flagged to the user, not worked around with a minimum-phase reconstruction from magnitude alone (technically possible via a Hilbert-transform/Kramers-Kronig approach, but would be an unvalidated assumption layered on top of an already-unusual measurement, not worth the risk given how many times this project has been burned by trusting a model over real hardware).

**What the magnitude alone DID show, though - a real lead**: compared this file's 0-8000Hz portion directly against yesterday's `aAD` (shelf1+shelf2) bare magnitude curve, interpolated onto the same frequency grid. Shelf2 was designed for a nominal +10dB boost with a 6000Hz corner, specifically to extend correction toward 8000Hz (`envelope_ampeq.h`), and real hardware previously confirmed +6.7dB of that at 8000Hz (2026-09-04, `ga_Trial2_TF.txt`) - a real but somewhat short-of-predicted effect. In THIS comparison, the gap between shelf1-only and shelf1+shelf2 is far smaller than either number: under 0.1dB through 100-2000Hz (expected - shelf2's corner is well above there), climbing to only ~0.6dB mean / ~2.2dB max anywhere in 6000-8000Hz (chart: `shelf2_magnitude_check.png`). That is nowhere near shelf2's designed +10dB or even its previously-confirmed real +6.7dB - shelf2 does not appear to be delivering anywhere near its intended high-frequency gain boost in this comparison.

**Why this matters for the user's actual question**: shelf2's group-delay cost is a direct, unavoidable consequence of the magnitude gain it's supposed to be providing (the minimum-phase gain/phase coupling `envelope_ampeq.h` documents at length) - if it's not actually delivering that gain on real hardware right now, it would be adding all of its documented delay/dispersion cost (125.4us p-p analytically, confirmed roughly doubling real dispersion in the 2026-09-04 bench trial) for little to none of the benefit it exists for. That's a clean, concrete candidate explanation for "A seems to make IMDs worse now" - worse audio-band delay for negligible high-frequency gain payoff, rather than the documented "genuine gain/delay trade-off" this project designed shelf2 around. Not confirmed as THE cause (could also be an artifact of comparing across two different measurement sessions/days - the caution from the PSU/predistort comparison work applies equally here), but worth checking directly on the bench: scope or re-measure the RSET/PWM output specifically around 6000-8000Hz with 'A' toggled on/off on the SAME session, and confirm shelf2's enable flag/coefficients are actually what's expected in the currently-running firmware (`envelope_ampeq_shelf2_get_enabled()`), before assuming the group-delay math above is the whole story.

**Status**: shelf1-only gdeq fit is still pending a phase-inclusive re-measurement. This magnitude-only anomaly is a new, separate, and arguably more urgent lead worth chasing alongside it.

## 2026-09-20, later still: shelf1-only re-export received and fit - all three existing gdeq sets make it WORSE, but a genuinely free-lunch asymmetric candidate found (matches bare full-band p-p exactly while roughly halving local slope); wired in as a fourth `'G'` variant, not yet bench-tested

User re-uploaded `HiRes_aD_TF_New_5V_psu.txt` (message: "replacement") in the correct mag+phase `FRF(Channel 2,Channel 1)` format - verified first (1367 lines, 0-7997.891Hz, `X[Hz]`/`Log[1]`/`Phase` columns, phase continuous with no >180 degree jumps) before proceeding, given the previous upload's format problem.

**Extraction**: same established pipeline as every prior TF in this file - trimmed f<35Hz placeholder/noise points, Savitzky-Golay smoothed derivative of phase at a 700Hz-span window (this project's settled default). Bare (gdeq off, shelf1+predistort only, no shelf2) result: 68.9us p-p / 68.2us mean over 100-8000Hz; 17.0us p-p / -7.55us/kHz local linear-fit slope over 2800-4500Hz. This is a substantially flatter, gentler curve than either the bare analog filter alone (71.2us p-p, a different shape) or the analog+shelf1+shelf2 curve the AA_CANDIDATE/CANDIDATE_B sets were fit against (156.5us p-p) - direct confirmation that shelf2 is the dominant contributor to dispersion in those curves, consistent with (though not the same claim as) the previous entry's magnitude-based lead about shelf2.

**Checked existing sets first, per this project's standing practice of never assuming a fit transfers to a new curve shape without checking**: all three make full-band p-p WORSE than gdeq off entirely - DEFAULT (a1=0.026173, a2=0.236810): 68.9us -> 84.7us; AA_CANDIDATE (a1=a2=-0.139115): 68.9us -> 101.0us; CANDIDATE_B (a1=a2=+0.09): 68.9us -> 82.0us. Two of the three (DEFAULT, CANDIDATE_B) do flatten the local 2800-4500Hz slope close to zero (2.38us/kHz and 0.53us/kHz respectively, vs. bare's -7.55us/kHz) - but only by overcorrecting past flat, and only by paying that full-band cost. None is a free win on this curve.

**Fit search**: a plain symmetric (a1=a2) search found almost no headroom (a=-0.0217, p-p=66.6us - essentially bare, confirming this curve's shape genuinely doesn't suit a symmetric pair the way DEFAULT's analog-alone fit did). A full-band-optimal asymmetric search found a1=-0.3136, a2=+0.2255 (44.1us p-p, a real ~1.6x improvement) but at the cost of worsening the local 2800-4500Hz slope to -12.6us/kHz - the same full-band-vs-local-slope tension this file has documented for AA_CANDIDATE/CANDIDATE_B, reappearing on a different curve.

Ran a genuine 2-parameter asymmetric Pareto sweep across several full-band p-p budgets ([69,75,85,100,130]us) rather than stopping at the single-axis optima above - the same kind of sweep the CANDIDATE_B-era work never got to run against the shelf1+shelf2 curve (that exploration only tried symmetric a1=a2 points, per this file's own 2026-09-06 entries). On THIS curve, the 2-parameter freedom found something the shelf1+shelf2 sweep never located: local slope improvement at essentially ZERO full-band cost. A final budget-constrained optimization (17x17-grid multi-start + Nelder-Mead, penalizing any full-band p-p exceeding bare's own 68.92us) converged to:

**a1 = -0.110103, a2 = +0.286600**

Result: full-band p-p = 68.92us (matches bare's 68.92us almost exactly - i.e. no full-band dispersion cost at all), local 2800-4500Hz slope = -3.39us/kHz (vs. bare's -7.55us/kHz - roughly halved, though NOT fully flattened to zero the way DEFAULT/CANDIDATE_B manage on this curve at their own full-band cost - this is a genuinely different point on the trade-off frontier, not a strictly dominant one). Mean added delay ~125.16us (2.003 samples @ 16000Hz), closely matching every other set's own ~2-sample addition, so the existing `'['`/`']'` bench-optimum starting points remain a reasonable first guess for re-tuning rather than a blind search from scratch.

**Honest framing of what this chart shows** (`gdeq_aD_shelf1only_analysis.png`, delivered): the DEFAULT/AA_CANDIDATE/CANDIDATE_B curves on that chart are LTI-model PREDICTIONS - bare (this session's real measured curve) plus each set's own exactly-known analytic allpass contribution (`ssb_allpass1_t`'s tau(w)=(1-a^2)/(1+2a*cos(w)+a^2) formula) - not new bench sweeps of shelf1-only with those specific coefficients actually running. Only the bare curve and the new A_CANDIDATE fit itself are grounded in this session's real measured data; the other three comparisons are the same kind of LTI-additivity extrapolation this project cross-validated (3.8us RMS) in the earlier aAD/aADgB entry, so the mechanism is trusted, but no bench confirmation exists yet for DEFAULT/AA_CANDIDATE/CANDIDATE_B specifically in a shelf1-only configuration - they were never intended to run there anyway (their own #if guards restrict them to the filter/Fs, not the shelf combination, since gdeq has no runtime awareness of which ampeq shelves are enabled).

**Wired in**: added `ENV_GDEQ_VARIANT_A_CANDIDATE` (enum value 3, `ENV_GDEQ_VARIANT_COUNT` now 4) to `envelope_gdeq.h` (new `#define ENV_GDEQ_HAS_A_CANDIDATE`/`ENV_GDEQ_A1_A_CANDIDATE`/`ENV_GDEQ_A2_A_CANDIDATE` block, gated the same way as AA_CANDIDATE/CANDIDATE_B, same fallback-define pattern so the file still compiles unconditionally on builds where it's unfitted) and `envelope_gdeq.cpp` (extended `env_gdeq_coefficients_for_variant()`, `envelope_gdeq_variant_available()`, `envelope_gdeq_variant_name()`). Appended as value 3 rather than renumbering the existing three, so any already-persisted `PersistentSettings.env_gdeq_variant` field keeps meaning what it always meant. `serial_commands.cpp`'s `'G'` cycle (default -> a+A -> candidate B -> A_CANDIDATE -> default -> ...) and its `k_gdeq_variant_enum_name[]` preset-dump table were both updated; the new `'G'` caveat string was checked against `serial_reply()`'s 512-byte buffer (the same limit that silently truncated candidate B's original, longer wording) - full reply length 407 bytes, comfortably under budget.

**Status - deliberately NOT claiming validation**: this is a real, checkable improvement in a controlled LTI model sense (zero full-band cost, real local-slope reduction, both numerically solid), but unlike every other set in this file at the point it was FIRST introduced, it has had no bench confirmation whatsoever yet - no TFA re-sweep with gdeq actually switched on to confirm the model prediction (the way AA_CANDIDATE and CANDIDATE_B both were, same-day or next-day, before being trusted), and no two-tone IMD comparison at all. Given this project's own repeated, hard-won finding that neither full-band p-p nor local slope has reliably predicted real IMD outcome by itself (AA_CANDIDATE: better close-in IMD but a worse far-out spur forest; CANDIDATE_B: confirmed group-delay flattening but small/delay-dependent real IMD gain), this candidate needs the same two-step real-hardware check before any recommendation: (1) TFA re-sweep with `'G'` cycled to A_CANDIDATE to confirm the prediction, (2) a same-session two-tone IMD comparison against the current best-known shelf1-only tuning. Code comments, the `'G'` serial reply, and this entry all say so explicitly. Not committed as a recommendation - offered for bench-testing only, same status CANDIDATE_B had at its own introduction.

Also worth keeping in view (not re-litigated here, see previous entry): shelf2's own magnitude payoff was found to be much smaller than expected in a separate, cross-session comparison, while its group-delay cost is confirmed real and substantial via this session's own aAD/aADgB data - the two observations aren't formally linked yet (different sessions, different files), but together they're consistent with "A" currently paying a real delay cost for little real gain, which is exactly what would explain a shelf1-only configuration needing (and benefiting from) its own dedicated gdeq treatment in the first place.

Fit search script, all intermediate arrays, and the chart script are in this session's scratchpad (`aD_pareto_rows.npy`, `aD_newfit_sym.npy`, `aD_newfit_asym.npy`, `aD_recommended.npy`, `build_aD_gdeq_chart.py`) if this needs re-checking.

## 2026-09-20, later still: full code review of shelf2 ('A') - implementation verified correct against the RBJ cookbook formula, but a simulation of the 'w' chirp test's own envelope-clamp interaction reproduces the observed magnitude shortfall closely, making "the test methodology is saturating" a stronger lead than "shelf2 is broken"

User asked directly: "is shelfA actually working as intended - please review code," following on from the previous entry's magnitude-shortfall lead (shelf2 delivering only ~0.6dB mean / ~2.2dB max gain in 6000-8000Hz, vs. its designed +10dB / previously-confirmed-real +6.7dB).

**Code review, line by line**: `envelope_ampeq.h`/`.cpp`'s shelf2 wiring (`ENV_AMPEQ_SHELF2_FREQ_HZ=6000.0f`, `ENV_AMPEQ_SHELF2_GAIN_DB=10.0f`, independent `'A'` enable flag, reset-on-off->on-transition, cascaded after shelf1 in `envelope_ampeq_process()`) matches its own documentation exactly. The underlying primitive, `ssb_shelf_biquad_set_highshelf()`/`ssb_shelf_biquad_process()` in `ssb_dsp.c`, was checked coefficient-by-coefficient against the RBJ Audio EQ Cookbook's high-shelf formula (A=10^(gain/40), w0=2*pi*fc/fs, alpha=(sin(w0)/2)*sqrt(2) for the S=1 case, then the standard b0/b1/b2/a1/a2 normalized-by-a0 formulas) - every term matches exactly, including the S=1 simplification's derivation (the (A+1/A)*(1/S-1) term correctly vanishing to 0 for S=1, leaving alpha=(sinw0/2)*sqrt(2)). The Direct-Form-I `process()` function (`y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2`) is the textbook form given already-normalized coefficients. `serial_commands.cpp`'s `'a'`/`'A'` handlers correctly call the independent enable setters. **No implementation bug found anywhere in the shelf2 filter code itself.**

**So why the shortfall? Simulated the actual test methodology instead of just the filter.** The magnitude/phase TF files in question come from `test_signals_generate_chirp()` (the `'w'` sine-sweep test), which sets `*out_envelope = AM_TEST_DEPTH + AM_TEST_DEPTH * master_gain_linear * sin(phase)` - at the default `AM_TEST_DEPTH=0.5` and unity master gain (the default), this swings the digital envelope across the FULL [0,1] range, by the constant's own comment "100% depth AM, deliberately stressing the RSET path's linearity across its whole normalized range." Critically, `envelope_predistort_process()` (`envelope_predistort.cpp` lines 44-45) hard-clamps ITS OWN input to [0,1] before doing the LUT lookup - separate from, and earlier than, the pipeline's final safety clamp - and both `HiRes_aD_TF_New_5V_psu.txt`/`HiRes_aAD_TF_New_5V_psu.txt` were captured with predistort ('D') enabled, so this is the clamp that actually applies to these files.

Wrote a direct numerical simulation (`check_shelf_clipping.py`): computed shelf1 and shelf1+shelf2's exact biquad magnitude/phase response at each swept frequency (same coefficient formula as above), generated the resulting steady-state envelope waveform `0.5 + 0.5*G(f)*sin(2*pi*f*t + phase(f))`, applied the SAME hard [0,1] clamp the firmware applies, then measured the clipped waveform's actual fundamental-frequency amplitude (the quantity a swept-sine FRF/TF measurement reports) via direct correlation against reference sin/cos at that frequency. Result, referenced to 300Hz (matching this project's own reference-band convention):

| f | ideal shelf1 (dB) | REALIZED shelf1 (dB) | ideal shelf1+2 (dB) | REALIZED shelf1+2 (dB) |
|---|---|---|---|---|
| 4300Hz | 5.69 | 1.68 | 6.23 | 1.73 |
| 6000Hz | 5.98 | 1.71 | 10.98 | 1.98 |
| 7000Hz | 6.00 | 1.71 | 15.40 | 2.06 |
| 8000Hz | 6.00 | 1.71 | 16.00 | 2.06 |

And the specific quantity the previous entry measured - shelf2's ADDED contribution (shelf1+shelf2 curve minus shelf1-only curve) - collapses from an ideal +5 to +10dB across 6000-8000Hz down to a REALIZED addition of only 0.27-0.35dB in this simulation. That is the same order of magnitude and the same qualitative signature (both configs converging toward a shared, clamp-limited ceiling rather than shelf1+shelf2 tracking meaningfully above shelf1-only) as the real ~0.6dB mean / ~2.2dB max shortfall the previous entry measured directly from the actual TF files. Chart (`shelf_clipping_hypothesis.png`): left panel shows the 7000Hz case directly - the pre-clamp waveform swings from about -0.3 to +1.6 (a nominal 15.4dB of shelf gain applied to an already-100%-depth sine), and the clamp turns it into something close to a square wave sitting at the 0/1 rails for most of each cycle; right panel shows the ideal vs. realized curves for both configs side by side, visually converging exactly where the observed shortfall was found.

**Mechanism, plainly**: the envelope has NO headroom left to give a boost filter once it's already being driven at 100% depth - shelf1 alone (a "modest, capped" +6dB by its own design intent) already uses up most of the available margin at high frequency, and shelf2's much larger design gain (+10dB corner, +16dB combined with shelf1 near 8kHz) has essentially nowhere to go once the signal is already pinned at the rails. A swept-sine measurement of a clipped waveform reports a fundamental amplitude far below the filter's own linear gain - not because the filter isn't applying that gain internally (it is - the pre-clamp waveform in the chart clearly reaches the full boosted amplitude), but because everything past the point of clipping is thrown away before it ever reaches the PWM/analog stage being measured.

**Honest gap in this explanation, not glossed over**: this doesn't cleanly reconcile with the ORIGINAL 2026-09-04 real-hardware result (`ga_Trial2_TF.txt`), which measured a real, substantial +6.7dB gain from shelf2 (short of the +10dB prediction, but nowhere near this simulation's ~0.3dB collapse) - if the exact same 100%-depth/unity-master-gain chirp methodology applied then too, that earlier test should have shown similarly crushed output, not a real 6.7dB improvement. Checked git history for `AM_TEST_DEPTH` and `test_signals_generate_chirp()`'s envelope formula specifically: both have been unchanged since the `'w'` chirp test was first introduced (commit `3b1b256`) - this isn't a recent code regression. The most parsimonious remaining explanation is that master gain (`'+'`/`'-'`, not persisted/recorded in the raw TF files or in these notes) was set lower during the 2026-09-04 session than during the most recent one - genuinely unknown, not confirmed, and worth asking the user directly if they recall, since it isn't recoverable from the data alone.

**Recommended verification, cheap and decisive**: re-run the shelf1-only vs. shelf1+shelf2 `'w'` TF comparison at a clearly reduced master gain (several `'-'` presses, enough that even the full theoretical +16dB combined boost near 8000Hz keeps the peak envelope inside [0,1] with margin - e.g. reducing depth by at least the ~16dB combined gain, more for safety) and see whether shelf2's magnitude gain reappears close to its analytic prediction. A clean confirmation would mean: shelf2's code is fine and was never the problem; the earlier "shelf2 makes real IMDs worse" finding may be worth revisiting too, since if a similar (if less extreme - two-tone energy is spread across many envelope harmonics rather than concentrated at one swept frequency) clipping effect happens during actual two-tone testing, that would inject real, harmonically-rich distortion directly - a materially different and likely worse mechanism for IMD than the smooth, designed magnitude boost the shelf normally provides, and one that hasn't been distinguished from the already-documented group-delay-dispersion cost in any test run so far.

Simulation script, chart script, and the numeric comparison table are in this session's scratchpad (`check_shelf_clipping.py`, `build_shelf_clip_chart.py`, `shelf_clipping_hypothesis.png`) if this needs re-checking or extending (e.g. to the actual two-tone envelope waveform instead of a single swept tone).

## 2026-09-20, later still: user confirms the clipping hypothesis directly - shelf2's shortfall is graded with drive level; re-simulating through the REAL predistort LUT (not a flat clip) reproduces both the trend and the exact observed magnitude

User's direct reply to the previous entry: "previous measurements were at low level whereas these latest batch were D on at quite a high level. The higher the level the more depressed the shelf2 gain becomes when D is on." This confirms the clipping/headroom mechanism from a completely independent angle (their own recollection of the two sessions' settings) and adds a detail the earlier simulation hadn't tested: the relationship is GRADED with level, not a simple on/off clip.

**Re-simulated using the real predistort chain, not a simplified model.** The previous entry's simulation clipped the boosted envelope to a flat [0,1] range as a stand-in for the pipeline's clamp. This round used the ACTUAL `ENV_PREDISTORT_LUT[65]` REVISION 7 values and the real `envelope_predistort_process()` interpolation/clamp logic (input clamped to [0,1], then a 65-point piecewise-linear lookup) as the nonlinearity, cascaded after the same real shelf1+shelf2 biquad response as before, swept across a range of master-gain levels from 0dB (unity, this session's setting) down to -24dB.

**Result - matches both dimensions of the user's report:**

| Master gain | 6000Hz realized shelf2 addition | 7000Hz | 8000Hz |
|---|---|---|---|
| 0dB (unity - this session) | 0.47dB | 0.60dB | 0.61dB |
| -6dB | 3.98dB | 4.49dB | 4.53dB |
| -10dB | 7.54dB | 9.65dB | 9.76dB |
| -20dB | 5.01dB | 9.50dB | 10.13dB |
| ideal (no clip) | 5.00dB | 9.40dB | 10.00dB |

At 0dB the model predicts 0.61dB - matching the ~0.6dB this session's real TF comparison found almost exactly, not just in order of magnitude. As level drops, the realized addition climbs smoothly and monotonically (apart from a small overshoot bump around -13 to -16dB, likely a numerical artifact of the LUT's own segment kinks interacting with the correlation-based fundamental extraction, not necessarily a real physical effect - flagged rather than asserted) toward the full, unclipped +5/+9.4/+10dB design values by around -20dB. The original 2026-09-04 real measurement (+6.7dB at 8000Hz) falls almost exactly on this modeled curve at roughly -8 to -9dB below unity - fully consistent with "that earlier test was run at a lower level than this session's," even though the exact historical master-gain setting still isn't recorded anywhere and remains a recollection, not a logged number. Chart: `shelf2_level_dependence.png` (added gain vs. master gain, three frequencies, with the 0dB/~0.6dB observed point marked directly on the curve).

**One nuance worth being precise about**: re-checked whether predistort's own curvature (as opposed to a plain flat clip) is what makes this specifically worse "when D is on" - at unity level, the WITH-predistort and WITHOUT-predistort (plain `envelope*0.9+0.2` then hard clamp) realized shelf2 additions come out nearly identical (0.61dB vs. 0.59dB at 8000Hz). So the dominant mechanism is the shared envelope-headroom/clamp effect present either way, not something unique to predistort's LUT shape - `'D'` being on in both of this session's comparison files looks incidental to which files happened to be compared, not a separate causal factor. Noted so as not to overstate what "when D is on" implies mechanistically, while fully agreeing with the user's core, now-confirmed observation about level.

**Status**: this closes out the "is shelf2 broken" question - it is not; the shortfall is a fully explained, level-dependent artifact of testing/measuring a large digital gain stage against an already-100%-depth test signal with no headroom margin. Practical takeaway for future shelf1/shelf2 (and by extension any future ampeq-style gain stage) TF or IMD comparisons: record and hold constant the drive level, or deliberately back it off enough that the full combined shelf gain stays inside [0,1], otherwise the measurement characterizes the clamp, not the filter. Whether this same mechanism is also responsible for any of the real two-tone "shelf2 makes IMD worse" observation remains open - two-tone's envelope energy is spread across many harmonics rather than concentrated at one swept frequency, so the same test-level sensitivity may exist there too but hasn't been separately confirmed.

Script: `check_shelf_clipping_v2.py` (level-sweep table, real-LUT model, with/without-predistort comparison), `build_level_dependence_chart.py` (chart). Both in this session's scratchpad.

## 2026-09-20, later still: new lead - fixed 645Hz tone + harmonics on mic input, present even with mic physically disconnected; ruled out a clean match to any of this project's own digital clock domains, recommended isolating mic/ADC-side vs. shared-chain-side before guessing further

User, evaluating mic input quality (found it works best with both ampeq shelves off, matching this session's shelf investigation), separately reports a fixed 645Hz tone plus harmonics present on the signal even with the mic physically disconnected - ruling out an acoustic/room-noise origin and pointing at something injected internally, electrically or digitally.

**Checked for a clean numeric match against every digital clock domain in this codebase before treating it as a hardware mystery** (same discipline as the project's past ratio-mismatch bugs, which always had an exact numeric explanation once found - the old SAMPLE_RATE_HZ=9600 case had a real, quantified 4.8% ADC_SAMPLES_PER_TICK mismatch, true_ratio 8.387 vs nominal 8, config.h:501-509):
- `SAMPLE_RATE_HZ` (main DSP tick) = 16000Hz. 16000/645 = 24.8 - not integer.
- `ADC_CONT_SAMPLE_FREQ_HZ` = 80000Hz. 80000/645 = 124.03 - not integer (close to 124, see caveat below).
- ADC ISR burst rate = 80000/16 (`ADC_CONT_FRAME_SAMPLES`) = 5000Hz exactly. 5000/645 = 7.75 - not integer.
- `RSET_MOD_LEDC_FREQ_HZ` (envelope PWM) = 64000Hz. 64000/645 = 99.2 - not integer.
- gptimer alarm: `resolution_hz`=2,000,000, `alarm_count`=125, giving exactly 16000Hz (2000000/125=16000, no remainder) - clean, not an off-by-one candidate despite 124 vs. 125 being tempting to eyeball as a near-miss (80000/124=645.16Hz, close to but not exactly 645Hz, and nothing in the actual gptimer/ADC config code uses 124 anywhere - this is a coincidental near-match, not a demonstrated mechanism, flagged as ruled OUT rather than a real lead).
- No occurrence of "645" or 640-650Hz found anywhere in either markdown log's history - this project has chased several other injected-tone artifacts before (a ~16.5Hz comb traced to mixed PSU domains via AM-to-PM conversion, and a still-unresolved ~7.7Hz comb intrinsic to the SoC's own timing pins even with all downstream hardware powered off), but none of that prior work mentions this frequency.

**None of this project's current digital clock domains produce or cleanly relate to 645Hz.** That doesn't rule out a firmware cause entirely (a beat between two clocks not considered here, or a genuine FIFO/DMA sample-repeat artifact like the one this project already found and fixed once - `adc_capture.h:100-123`, a starvation-driven zero-order-hold repeat that landed its fundamental+harmonic exactly on 3000/6000Hz at different ADC settings - is a proven bug CLASS on this board, just not currently numerically matching 645Hz at today's settings), but it does mean this doesn't look like an obvious, already-known internal ratio problem the way past bugs here have been.

**Recommended diagnostic sequence, cheapest/most decisive first:**

1. **Switch away from `'m'` (mic) to `'s'` (AUDIO_SRC_SINGLETONE) or `'t'` (AUDIO_SRC_TWOTONE) with everything else held the same.** Both bypass the ADC/mic path entirely with a purely synthetic envelope/freq_dev source - this is EXACTLY what `AUDIO_SRC_SINGLETONE` was built for, per its own doc comment: "isolates the DSP/RF chain from mic-side confounds (preamp hum, mic nonlinearity, room noise)" (`settings.h:22-25`). No hardware changes, no reflash, one keystroke. Two outcomes:
   - Tone SURVIVES with a synthetic source -> it's not mic/ADC-side at all; it's in the shared envelope/PWM/AD9851/RF/power path every audio source runs through. Given this project's own precedent (the 16.5Hz comb's PSU-domain root cause), the next place to look would be the PSU/ground path directly (see #3 below), not the ADC front end.
   - Tone DISAPPEARS with a synthetic source -> it's specific to the ADC/mic front end (floating-input EMI pickup, mic bias supply noise, or ADC-pin crosstalk - all external to firmware, since the mic preamp/bias circuit isn't described anywhere in this codebase beyond "Mic preamp -> GPIO6," `ssb_mic_test.ino:65`).

2. **If step 1 points at the mic/ADC front end**: ground the ADC input (GPIO6) directly rather than just leaving the mic disconnected/floating, and see if the tone disappears. A floating high-impedance input is a good unintentional antenna - if grounding it kills the tone, that's ambient EMI/RFI pickup (fix: a proper bias/pulldown network or shielding at the connector, not a firmware change); if the tone survives being grounded, it's coming from inside the ADC/ground/bias-supply path itself, not picked up externally.

3. **If step 1 points at the shared chain (or step 2 doesn't resolve it)**: try powering the board from a different supply (battery, a different bench supply, or isolating the mic-side supply from whatever powers the digital/PWM side) and see if the tone's level or frequency changes - the same diagnostic that closed out the 16.5Hz comb's PSU-domain finding. A switching regulator's fundamental or "light-load burst-mode" frequency (a well-known real-world source of audible whine in the low-hundreds-of-Hz range for many small buck/boost/charge-pump parts) is a classic, common explanation for "fixed tone + harmonics, present regardless of what's plugged into the audio input" - worth checking directly with a scope on any supply/bias rail feeding the mic preamp if one exists, even though it isn't visible from firmware.

4. **Frequency stability as a secondary clue**: if a precise measurement shows the tone drifting slightly with temperature or supply voltage, that favors a simple RC/oscillator-timed source (e.g. a cheap charge pump) over a crystal-locked digital one; a rock-stable exact frequency more often points to something crystal-derived (though nothing in the current firmware clock list matches) or a stable external EMI source.

**Not yet run - genuinely open.** No conclusion should be drawn before at least step 1, which costs nothing and immediately narrows the search space from "mic/ADC front end" vs. "everything else."

## 2026-09-20, later still: user's own guess - "could it be the offset between the 80kHz and what it actually is" - finds a strong, mechanistically precise, numerically-matching candidate in this project's own FIFO drain logic and its own previously-logged ADC rate measurement

Direct follow-up to the previous 645Hz entry. User asked whether the tone could be explained by the ADC's real/achieved sample rate differing from its configured `ADC_CONT_SAMPLE_FREQ_HZ=80000` nominal value, rather than anything acoustic or PSU-related.

**This project already measured exactly this, once, in an unrelated investigation, and dismissed it as benign for a different question.** Searching moving_forward_notes.md turned up: `[adc] actual=80645-80795 sps (expected=80000)` (moving_forward_notes.md:698), logged during a 2026-09-xx investigation into a two-tone-mode FIFO-overflow false alarm. That entry's own conclusion - "these numbers describe the ADC/DMA driver's own health... and they're fine" - was answering "is the ADC hardware/DMA pipeline healthy," not "could this rate offset produce an audible artifact in mic mode." Nobody connected the two before now. 80645 sps is 645 sps over the 80000 nominal - an essentially exact numeric match to the reported tone frequency, not just an order-of-magnitude coincidence.

**Read the actual FIFO drain mechanism to check whether a rate mismatch of this kind would produce a stable, periodic (tone-with-harmonics) artifact rather than just noise or a one-off event.** `adc_capture_read_next_sample()` (`adc_capture.cpp:259-324`) pops a fixed `ADC_SAMPLES_PER_TICK=5` (config.h/adc_capture.h, `=80000/16000`) raw ADC samples per dsp tick under normal conditions, feeding them through the selected LPF (or passing the last one through raw if the LPF is off) and returning the most recent filtered/raw value once per tick as that tick's decimated audio sample. The drain logic's own comment (`adc_capture.cpp:279-290`) is explicit and directly on point:

> "The '+1' branch handles the small persistent surplus (true_ratio measured ~4.032 at the old 9600Hz rate, not exactly the nominal ratio...) by opportunistically taking one extra sample whenever one happens to already be waiting, continuously bleeding off the surplus in the smallest possible increment. Without this, a fixed cap-of-32 catch-up threshold... still works, but a slow surplus takes many tens of ms to accumulate enough to trigger it - producing periodic gulps that show up as regular visible/audible spikes."

This confirms two things directly from the project's own prior engineering: (1) the ADC-to-tick rate ratio has NEVER been exactly the nominal integer this logic assumes - there has always been a small persistent surplus, at every sample rate this project has used (9600Hz historically, now 16000Hz); (2) the specific failure mode being guarded against here - "periodic gulps that show up as regular visible/audible spikes" - is EXACTLY the symptom now being investigated. The "+1 opportunistic bleed" was meant to smooth this into something less audible than a coarse periodic catch-up, but it doesn't eliminate the underlying periodicity - it can't, because both clocks involved (the ADC's sample clock and the main SAMPLE_RATE_HZ gptimer tick) are fixed-frequency, not random, so any persistent rate mismatch between them produces a fully deterministic beat, not noise, regardless of how finely the correction is applied.

**The math is exact, not approximate.** In steady state, total samples consumed per second must equal total samples produced per second. If the dsp tick fires at a true rate of T ticks/sec (nominally 16000) and a fraction f of ticks pop 6 samples instead of 5 (the rest popping 5), total consumption = T*(5+f) samples/sec. Setting this equal to the ADC's true production rate (80000+X sps, where X is the absolute rate error) and solving: f*T = X, i.e. the "+1 bleed" events themselves must occur at a rate of exactly X Hz - not a fraction of X, not a multiple, exactly X - to keep the FIFO from drifting either direction indefinitely. With the historically-measured X≈645-795 (from the 80645-80795 sps range logged before), this predicts a stable ~645-795Hz correction-event rate. Each such event is a genuine one-raw-sample (12.5us at 80kHz) timing skip in the otherwise-uniform decimated 16kHz stream - not a pure sinusoid, so its Fourier content naturally includes a fundamental AND harmonics, matching "645Hz tone and harmonics" precisely rather than just a single spectral line.

**This also fully and immediately explains "present with mic disconnected."** The mechanism lives entirely in the ADC-FIFO-to-dsp-tick decimation bookkeeping - it operates on whatever raw ADC codes are in the FIFO, including the noise floor/DC bias present with nothing connected to GPIO6. It requires no acoustic signal, no EMI pickup, and no external hardware fault - it's a pure firmware/clock-domain artifact, which is a meaningfully different (and more tractable) class of explanation than the EMI/PSU hypotheses raised in the previous entry.

**Recommended verification, using diagnostics this project already has - no new code needed.** Unmute diagnostics (`'v'`) and let it run long enough for the long-window average to settle (this is explicitly a low-noise, long-window measurement - the entry's own comment says "much lower noise than the 1s figure... since averaging error shrinks with window length"), then read:
- `[adc]   long-window avg=X.XX sps over Ys (Z.ZZZ% vs nominal 80000Hz)` (`diagnostics.cpp:2726-2732`)
- `[dsp]   long-window avg=X.XX ticks/s (Z.ZZZ% vs nominal 16000Hz) true_ratio=X.XXXX (nominal=5)` (`diagnostics.cpp:2741-2756`)

If `long-window avg` for the ADC comes back near 80645-80795 sps (matching the old reading) and `true_ratio` deviates from exactly 5 by a comparable percentage, that's a direct, quantitative confirmation - and gives the precise number needed to predict the exact tone frequency via the formula above (X = measured_sps - 80000, assuming the dsp tick itself is accurate; if `tick_error_pct` is also nonzero, use the more general `(true_ratio - 5) * long_avg_tps` form instead). Not yet run this session - this is a strong, well-supported hypothesis pending that direct read, not a confirmed finding.

**If confirmed, next step is a design question, not a re-measurement**: the current "+1 opportunistic bleed" is a discrete, integer approximation of what a persistent fractional-rate mismatch actually needs - a genuine fractional resampler. A cleaner fix would accumulate the fractional sample-count error every tick (e.g. `frac_error += true_ratio - 5.0`) and only advance an extra raw sample once that accumulator crosses 1.0, ideally with linear (or better) interpolation between the two straddling raw samples rather than a hard integer pop - this would spread the correction perfectly smoothly across time instead of concentrating it into a periodic, audible event, eliminating the tone at its source rather than just making individual correction events smaller. Not yet designed in code - contingent on the diagnostic above confirming the mechanism and magnitude first, since a fix built for the wrong X would just move the tone rather than removing it.

## 2026-09-20, later still: 80645 sps confirmed on the bench; ESP32-S3 ADC-continuous-mode rate research hits real tooling limits; user's own 65kHz experiment kills the 645Hz tone but likely trades it for a bigger, code-introduced decimation error

**Bench confirmation.** User read the ADC rate diagnostic directly and reported "actually saw a 80645" - matching the previous entry's cited historical reading (`moving_forward_notes.md:698`, `[adc] actual=80645-80795 sps (expected=80000)`) almost to the sample/sec. Combined with the exact steady-state conservation math already derived (a persistent rate mismatch of X Hz forces the "+1 opportunistic bleed" branch in `adc_capture_read_next_sample()` to fire at exactly X Hz), this is now a bench-confirmed mechanism, not just a strong hypothesis - 645 = 80645-80000 lines up with both the reported tone frequency and the harmonic content (each bleed event is a discrete ~12.5us timing skip, not a sinusoid).

**Root-cause research into "why doesn't ESP32-S3 ADC-continuous mode hit its configured rate exactly" - genuinely incomplete, logged honestly rather than papered over.** Loaded WebSearch/WebFetch and ran two queries ("ESP32-S3 adc_continuous actual sample rate different from configured sample_freq_hz clock divider", "ESP-IDF adc_continuous_config sample_freq_hz not exact APB clock divider rounding"). Found:
- `espressif/esp-idf` issue #10612: user configured 80000 sps, measured ~65287 actual (~1.22x off) on ESP-IDF 5.0. Labeled "Status: Done"/"Resolution: Done" but WebFetch could only retrieve the summary/labels, not the actual resolution comments explaining the mechanism.
- `espressif/esp-idf` issue #11623: ADC1_DONE_INT reported firing at a fixed ~1.4MHz rate regardless of configured `sample_freq_hz` (tested at 10kHz), referencing TRM register `39.72 APB_SARADC_INT_ENA_REG`, labeled "Selected for Development" - i.e. Espressif has acknowledged this as real. Same WebFetch limitation - summary only, no technical thread content.
- Official ESP-IDF `adc_continuous` docs (multiple versions) do not document the internal clock-divider derivation for `sample_freq_hz` at all - only point to `soc/soc_caps.h` for the valid range, and separately flag that `CONFIG_PM_ENABLE` can change the APB clock during idle (mitigated by a `ESP_PM_APB_FREQ_MAX` lock held during active conversion - doesn't explain a persistent baseline offset present throughout capture).
- An esp32.com forum thread on the same topic was blocked entirely by a bot-check/JS-challenge page.
- Attempted `gh issue view 10612 --repo espressif/esp-idf --comments` via Bash - `gh` is not installed in this environment. Attempted `curl https://api.github.com/repos/espressif/esp-idf/issues/10612/comments` directly - blocked by the sandbox's own network proxy ("GitHub access to this repository is not enabled for this session"), which would require an explicit `add_repo` grant not requested this session.

**Conclusion, stated at the confidence level the evidence actually supports**: this is very likely a real, Espressif-acknowledged limitation/quirk of the ESP32/ESP32-S3 ADC-continuous-mode driver (two real tracked issues describe actual-vs-configured sample-rate mismatches, one far larger than this project's own ~0.8%), but the EXACT technical mechanism (integer clock-divider granularity from the APB clock, a fixed hardware quantization step in the SAR ADC's own digital controller clock path, or something else) was not confirmed from any source reachable this session. Do not treat any specific mechanism as established - only the phenomenon itself is confirmed, by both the cited issues and this project's own repeated measurement.

**User's own experiment: recompiled with `ADC_CONT_SAMPLE_FREQ_HZ` changed from 80000 to ~65000.** Result: no more 645Hz tone, "nice and clean," but "it looks like the anti-alias filtering needs sorting." Two things checked in response:

1. **Precedent check** - has 65000 (or anything besides 80000) been tried in this project before? `adc_capture.h`'s own "TRIED, REVERTED" comment (lines ~100-123) documents exactly one prior alternate-rate experiment: 48000 (ratio 3), tried for CPU-headroom reasons ahead of a gptimer wakeup-jitter investigation, reverted because it (a) gave no measurable CPU win and (b) made noise ~1dB worse broadband and much worse in the 3-6kHz band specifically, traced to FIFO starvation - the longer ISR-burst-refill gap (5.33 dsp ticks at 48kHz vs 3.2 at 80kHz) at only 3 samples/tick of drain let the FIFO genuinely hit zero, producing a zero-order-hold repeat landing spurs exactly at `ADC_CONT_SAMPLE_FREQ_HZ/ADC_CONT_FRAME_SAMPLES` = 3000/6000Hz. 65000 has never been tried before this.

2. **Why moving off 80000 might plausibly kill the tone**: consistent with (though not proof of) the clock-divider-quantization framing above - if the ADC's real achieved rate for a given `sample_freq_hz` request depends on how that specific requested value happens to map onto whatever integer divider the driver picks from a fixed source clock, some requested frequencies could land with much larger rounding error than others. 80000 apparently landed on a "bad" spot (~0.8% high); 65000 might have landed on a much better one. This is plausible but UNVERIFIED - the long-window `[adc]`/`[dsp]` diagnostics haven't been read at the new 65000 setting, so whether the underlying ADC-clock accuracy actually improved, or whether the 645Hz artifact is simply being masked/replaced by something else, isn't yet known from a real measurement.

3. **A more likely, code-level explanation for "anti-alias needs sorting" was found by inspection, independent of whatever the ADC's real clock accuracy is at 65000.** `ADC_SAMPLES_PER_TICK` is defined in `adc_capture.h` as `(ADC_CONT_SAMPLE_FREQ_HZ / SAMPLE_RATE_HZ)` - both `unsigned int` (`80000u`, `SAMPLE_RATE_HZ` `16000u` from `config.h:477`), so this is a compile-time, ordinary C integer division. `65000u / 16000u` = 4 (4*16000=64000, remainder 1000 - NOT zero). This macro's own comment states explicitly: "if either rate ever changes, check this stays an integer division with zero remainder" - 65000 silently violates that invariant, and nothing in the build catches it; the macro just quietly evaluates to 4.

   Verified directly against `adc_capture_read_next_sample()` (`adc_capture.cpp:259-324`): the nominal per-tick pop count, the "+1 surplus" branch condition, and the "genuine backlog" cap all reference `ADC_SAMPLES_PER_TICK` as this same compile-time integer (4, not 4.0625). This means the decimation logic now behaves as though it were consuming an ADC stream at exactly 64000 sps (4*16000), while the ADC is actually running near 65000 sps (whatever its true rate turns out to be at that setting) - a **deliberate, code-introduced 1000Hz/1.5625% mismatch**, roughly 25x larger than the ~0.8% ADC-clock-inaccuracy error that produced the 645Hz tone at the 80000 setting, and present regardless of how accurate the ADC's own clock actually is at 65000.

   Applying the same steady-state conservation argument used for the 645Hz case: the "+1 bleed" branch would need to fire at very close to `16000 * (0.0625)` = **~1000 times/sec** to keep the FIFO balanced (assuming the ADC's true rate is close to the configured 65000; if it isn't, the required bleed rate shifts by that additional amount too). That's the same underlying artifact CLASS as the 645Hz tone - a fixed-rate, deterministic timing-skip sequence, hence harmonics rather than pure noise - just occurring roughly 25x more often. At ~1000Hz repetition, this is far more likely to be perceived as an elevated/gritty noise floor or "needs an anti-alias pass" than as a single clean interfering tone the way 645Hz was, which fits the user's description better than an actual mistuned or wrongly-designed LPF would.

   **Ruled out (checked, not just assumed): the LPF cutoff itself is not hardcoded to 80000.** `adc_capture.cpp:207-208` calls `ssb_biquad4_lpf_init(&s_adc_lpf_butterworth, ADC_LPF_CUTOFF_HZ, (float)ADC_CONT_SAMPLE_FREQ_HZ)` and the equivalent Chebyshev init, both taking `ADC_CONT_SAMPLE_FREQ_HZ` as an explicit runtime-computed parameter (cast to float) rather than any hardcoded constant - so the biquad coefficients get correctly recomputed via the bilinear transform for whatever `ADC_CONT_SAMPLE_FREQ_HZ` is set to at compile time, and the -3dB point should still sit right at 3000Hz even with the macro changed to 65000. The anti-alias filter's own tuning is very unlikely to be the actual root cause here; the decimation-truncation bug above is the stronger suspect and doesn't require assuming anything about the LPF being wrong.

**Recommendation - try 64000Hz next, not 65000.** `64000u / 16000u` = 4 exactly, zero remainder - restores the same "clean division" property `ADC_CONT_SAMPLE_FREQ_HZ` has always had before now (80000/16000=5, 48000/16000=3), eliminating the truncation-introduced 1.5625% error entirely regardless of what the ADC's true clock accuracy turns out to be at that setting.

One open caution on 64000, flagged rather than assumed away: the ISR burst-refill gap works out to `ADC_CONT_FRAME_SAMPLES / ADC_CONT_SAMPLE_FREQ_HZ` = 16/64000 = 250us = exactly 4 dsp ticks, versus 200us/3.2 ticks at the (working) 80000 setting and 333us/5.33 ticks at the (previously-failing, FIFO-starved) 48000 setting. 64000 sits numerically between the known-good and known-bad cases, so it is NOT safe to assume it avoids the same starvation failure mode just because the tick-ratio happens to be a clean integer this time - the 48000 failure was about absolute inter-burst timing margin against the FIFO's depth/CPU jitter, not really about the ratio's exactness. This needs the same diagnostic used to originally catch the 48000 problem (`[adc] fifo: min=...`, `starve_ticks_total`) checked directly on real hardware before trusting 64000 as clean.

Separately, whether 64000's own true achieved ADC rate is closer to nominal than 80000's ~0.8% offset is itself an open, unmeasured question - the clock-divider-quantization framing above predicts it COULD be much better or could be just as bad; there is no basis yet to assume either way without reading the `[adc]`/`[dsp]` long-window diagnostics at that setting.

**Status of the fractional-resampler design proposed in the previous entry**: still not implemented, and now clearly not just a "nice to have" - a proper design should tie `ADC_SAMPLES_PER_TICK`'s integer nominal value AND the fractional remainder together explicitly (e.g. derive both from a single non-macro calculation, or add a compile-time static_assert that the division has zero remainder, so a future rate change like this one fails loudly at compile time instead of silently truncating) rather than relying on the existing comment being noticed and honored by hand.

## 2026-09-20, later still: correction (64000Hz, not 65000) - not exact either, and the "64k should be exact" claim traces to a real fact about a different clock domain in this same codebase, exposing why per-frequency divisor arithmetic doesn't reliably predict ADC-continuous-mode accuracy

**Correction to the previous entry**: the "65000" ADC rate experiment was a mistype. The user actually configured `ADC_CONT_SAMPLE_FREQ_HZ=64000` and reports it is "not exact either," while noting other research they found suggested 64kHz specifically should be an exact/achievable rate.

**Traced the "64k is exact" claim to a real, on-the-record fact about a DIFFERENT clock domain in this codebase - a plausible source of the mismatched expectation.** `group_delay_fit_notes.md`'s 2026-09-08 entry documents retuning `RSET_MOD_LEDC_FREQ_HZ` (the envelope/RSET-modulation PWM carrier, driven by the ESP32-S3's LEDC peripheral) from 78125Hz to 64000Hz, with the reasoning stated explicitly: "64000Hz... is *also* an exact integer division of the same 80MHz APB clock (80,000,000/1250)." That's correct arithmetic, and it's a different peripheral entirely - LEDC's clock-divider hardware (`ledc_timer_config`, its own `clk_cfg`/`clk_div` fields) is a completely separate register/clock-tree path from the SAR ADC digital controller's clock derivation used by `adc_continuous_config()`. Both nominally trace back to an 80MHz-class source clock, but "divides evenly into 80MHz" says nothing about whether a SPECIFIC peripheral's specific divider hardware/driver implementation actually achieves that division exactly - each peripheral's clock path has its own divider width, rounding behavior, and (per the two GitHub issues already found for the ADC specifically) potential driver bugs on top of the ideal arithmetic. If "other research" here was this same 80MHz-divisibility argument applied to the ADC by analogy (whether recalled from this project's own LEDC precedent or found independently), it doesn't actually establish anything about the ADC continuous driver's real behavior - and the user's own bench result now directly contradicts it for that specific peripheral.

**Why this is a genuinely useful (not just negative) data point.** `ADC_CONT_SAMPLE_FREQ_HZ / SAMPLE_RATE_HZ` at 64000/16000 = 4 exactly, zero remainder - so, unlike the 65000 case, there is no code-side `ADC_SAMPLES_PER_TICK` truncation bug in play here at all; the firmware's own decimation-ratio bookkeeping is exactly correct at this setting. That cleanly isolates whatever mismatch the user is observing to be 100% ADC-hardware/driver clock inaccuracy, with zero contribution from the firmware side - a clean single-variable measurement. Taken together with the original 80000-configured reading (80645-80795 sps measured, also an exact APB-clock divisor at ratio 1000: 80,000,000/1000=80000), this project now has TWO independent real bench measurements, at two different configured rates that are BOTH exact divisors of the same 80MHz reference clock, and BOTH show the driver failing to land exactly on target. That's meaningfully stronger direct evidence (from this project's own hardware, not just cited external issues) that "pick a configured frequency that divides cleanly into the SoC's reference clock" is not a reliable strategy for getting an exact ADC-continuous sample rate on this driver/IDF combination - consistent with the GitHub issues already found (esp-idf#10612's ~1.22x mismatch, esp-idf#11623's fixed-interrupt-rate report) describing this as a genuine, Espressif-acknowledged driver characteristic rather than something fixable by better arithmetic on the caller's side.

**Open, not yet obtained: the actual measured number at 64000.** The user hasn't reported reading `[adc] long-window avg=...` at this setting yet. That figure would give: (a) the absolute/percentage offset at 64000 for direct comparison against 80000's ~0.8% (is 64000 better, worse, or about the same in relative terms - genuinely unknown until measured, since the two data points so far only establish that neither is exact, not their relative magnitudes); (b) via the same conservation-math relationship derived for the 645Hz case (bleed-event rate = |true_rate - configured_rate|), the exact frequency of whatever residual periodic artifact remains at 64000, however faint. Recommended next action, unchanged in kind from before: `'v'`, let the long-window average settle, read the figure back.

**Updated recommendation given two independent confirmations of the same underlying phenomenon.** Abandon the search for a "magic" configured `ADC_CONT_SAMPLE_FREQ_HZ` value that happens to land exactly on target - the evidence so far (both from this project's own two data points and from the external GitHub issues) suggests this ADC-continuous driver simply doesn't reliably do that, independent of how cleanly the requested value divides into any reference clock. The previously-proposed fractional resampler design stands, now on firmer footing: measure the TRUE achieved rate empirically at whatever `ADC_CONT_SAMPLE_FREQ_HZ` is ultimately chosen (via the existing long-window diagnostic - no new instrumentation needed), and build the fractional accumulator/interpolation logic around that real, measured number rather than assuming the nominal configured value is (or can be made) exact. This makes the fix correct regardless of which frequency is chosen or how this specific driver's internal rounding happens to behave at that value - the one variable that keeps changing across experiments (80000, 65000, 64000) without ever landing on "exact."

## 2026-09-20, later still: proper calibrated fractional resampler implemented - full design and code walkthrough

User confirmed the GitHub research already logged (esp-idf#10612/#11623) as the same debunked source they'd found, and asked to move from proposal to implementation: "let's add a proper fractional sub-sampler." Also asked directly whether the actual chip Fs is known precisely - answered honestly: no. The only numbers on record are the historical 80645-80795 sps range at a configured 80000 and the user's own "it varies a bit" remark; nothing has been measured yet at the current 64000 setting. That uncertainty (both "we don't have a precise number" and "it may not even be constant") directly shaped the design below: rather than hardcode any specific measured ratio, the fix measures its own ratio continuously at runtime.

**Two independent problems the old scheme had, and how each is addressed:**

1. **Wrong assumed ratio.** The old drain logic (`adc_capture_read_next_sample()`, pre-change) always targeted exactly `ADC_SAMPLES_PER_TICK` (a compile-time nominal integer) per tick, bleeding off any persistent surplus via an ad hoc "+1 whenever one happens to already be sitting in the FIFO" branch. That branch's timing was governed by FIFO-occupancy dynamics, not by a clean measurement of the real ratio - it worked (kept the FIFO from drifting), but its correction cadence was only approximately periodic and its underlying target ratio was never actually measured, just assumed to need "1 extra sample, sometimes."

   **Fix**: `s_true_ratio` (new static float in `adc_capture.cpp`) is now a live-calibrated measurement, recomputed every `ADC_RATIO_CALIB_WINDOW_CALLS` (`SAMPLE_RATE_HZ * 4u`, ~4 seconds - same "long-window is lower noise" reasoning diagnostics.cpp already uses) calls to `adc_capture_read_next_sample()`. Each window, it compares how many raw samples the ISR actually counted (`s_dbg_adc_samples_total`, already existed) against how many times this function was itself called over that same span, and sets `s_true_ratio = d_samples / d_calls`. This directly measures the exact quantity the drain logic needs (samples produced per call, not an abstract "Hz" figure requiring a separate wall-clock/dsp-tick-rate measurement) and needs no assumption about which `ADC_CONT_SAMPLE_FREQ_HZ` is configured or what its true error happens to be on this particular board. Seeded at the nominal `ADC_SAMPLES_PER_TICK` value until the first window completes (`s_ratio_calibrated` flag tracks this), so there's a brief (~4s) startup period using the untrue nominal assumption - an honest, bounded imperfection rather than guessing a prior.

2. **Hard integer "snap" instead of true fractional resampling.** The old scheme always returned literally the last raw sample it happened to pop that tick - a stair-step decimation with up to ~1 raw-sample-period of unaccounted phase error at every tick, and a discontinuous jump in that phase error whenever a "+1" event fired.

   **Fix**: a continuously-advancing virtual sample position (`s_virtual_pos`, float) accumulates by `s_true_ratio` every call - this is the ideal, continuous-time decimation position, not an integer count. `target_consumed = floor(s_virtual_pos) + ADC_RESAMPLE_LOOKAHEAD_SAMPLES(2)` says how many raw samples need to have been popped+filtered so far to keep the two bracketing already-filtered samples (`s_filt_prev` at index `floor(s_virtual_pos)`, `s_filt_curr` at index `floor(s_virtual_pos)+1`) ready for interpolation - the "+2" (rather than "+1") is exact 0-indexing bookkeeping, not a fudge factor: consuming through 0-indexed sample `floor(vpos)+1` requires a consumed-count of `floor(vpos)+2`. `desired_to_pop = target_consumed - s_resample_total_consumed` gives the number of NEW raw samples to pull this tick (worked through the same per-raw-sample LPF processing as before, in order - unchanged, since the anti-alias filter's own correctness depends on seeing every raw sample in sequence regardless of how output samples are later interpolated). The final output is `s_filt_prev + frac*(s_filt_curr - s_filt_prev)` where `frac = s_virtual_pos - floor(s_virtual_pos)` - a standard linear interpolator, giving each tick's output at the mathematically correct fractional phase rather than snapping to whichever raw sample happened to be available last.

   Net effect: in steady state (no starvation/backlog), `desired_to_pop` per tick works out to `floor(true_ratio)` or `floor(true_ratio)+1` depending on where the accumulating fractional position crosses an integer boundary - i.e. the same qualitative "usually N, occasionally N+1" cadence as before, but now driven by exact floor arithmetic on a calibrated ratio instead of FIFO-occupancy-dependent opportunism, AND with the output itself continuously phase-corrected via interpolation rather than only the sample COUNT being corrected. This should both (a) make the periodic correction's own timing exactly regular (a cleaner, more predictable spectral signature, easier to reason about even if some residual artifact remains) and (b) reduce the sharpness/energy of each individual correction event, since it's now a smooth interpolated blend rather than a hard sample-repeat-or-skip.

**Safety tiers preserved.** The old scheme's two-tier fallback structure - `ADC_SAMPLES_PER_TICK_MAX` as a genuine-backlog catch-up cap, and "take what's available" under starvation - is kept exactly, just now clamping `desired_to_pop` instead of a hardcoded `nominal+1`. Both remain rare/transient; `s_virtual_pos` keeps accumulating through an anomaly regardless, so the resampler naturally resumes at the mathematically correct position once the FIFO recovers, without needing any special-case recovery logic.

**`adc_capture_reset_diag()` (the `'r'` command)** now also restarts the calibration window's start-of-window counters, matching its existing behavior of restarting the long-window rate average - a clean, from-now measurement after a manual reset. `s_true_ratio` and `s_ratio_calibrated` themselves are deliberately NOT reset here (unlike the diagnostic counters) - they're the best current estimate of the real hardware ratio, and zeroing them would throw away a real measurement for no reason.

**New diagnostics added for verification, not yet exercised.** `adc_capture_get_true_ratio()` / `adc_capture_ratio_is_calibrated()` (declared in `adc_capture.h`) expose the resampler's own live-calibrated ratio; `diagnostics.cpp` now prints `[resamp] calibrated true_ratio=...` right next to the pre-existing, independently-computed `[dsp] ... true_ratio=...` line specifically so the two - which measure the same underlying quantity via two completely different counter pairs (ISR sample count vs. own call count, vs. ISR sample count vs. dsp-tick esp_timer measurement) - can be checked against each other as a cross-validation before either is trusted.

**Genuinely not yet bench-tested, at every level.** Not compiled against the real toolchain (textual brace/paren balance checked only, no build tools available in this sandbox). Not confirmed the interpolation/lookahead change doesn't introduce some new subtle artifact of its own (e.g. if `s_filt_prev`/`s_filt_curr` ever desync from what the fractional position actually expects during a rapid mode-switch or backlog-recovery event - argued through above as "graceful degradation, same as before" but not measured). Not confirmed the `[resamp]` and `[dsp]` true_ratio prints actually agree once run for real. And most importantly, not confirmed this actually reduces or eliminates the audible periodic artifact rather than just changing its character - that's the real test, still ahead. Recommended sequence: build, confirm no regression in ordinary mic-mode operation, watch the `[resamp]`/`[dsp]` cross-check converge, then listen critically for the tone at whatever `ADC_CONT_SAMPLE_FREQ_HZ` is currently configured.

## 2026-09-20, later still: classic ESP32's I2S-based ADC mode as a possible hardware escape hatch - what's confirmed, what isn't, and why it's not the next move

User noted that some ESP32 variants other than the S3 this project targets have a fractional clock divider on the I2S peripheral, worth keeping on record for a possible future board change - directly relevant to this whole session's ADC-rate-mismatch investigation, since it raises the question of whether the underlying hardware limitation could be sidestepped by different silicon rather than only compensated for in software.

**What was checked and confirmed, via WebSearch/WebFetch this session:**
- Classic ESP32 (only) has an I2S-based ADC continuous-sampling mode - `i2s_set_adc_mode()`/`i2s_adc_enable()` in the I2S driver, with the official ESP-IDF v4.2 docs stating "the ADC maximum sampling rate is 150KHz" for that mode. This reads the SAR ADC through the I2S peripheral's own clock generation and DMA path, a genuinely different mechanism from the `adc_continuous` driver this project's S3 uses.
- This mode does not exist on ESP32-S2/S3/C3 - the ESP-IDF v4.2 I2S docs are ESP32-specific for this feature, and nothing found this session (across all searches into #10612/#11623 and the official S3 ADC docs) describes an equivalent I2S-ADC path on S3. This is circumstantial but consistent: it would explain why S3 (and the newer chips generally) rely on the separate SAR-ADC-DMA `adc_continuous` driver that's been the subject of this entire 645Hz investigation, rather than having an alternative, possibly more precise, path available.
- The ESP-IDF v5.0.1 migration guide states plainly: "ADC and DAC modes are removed [from the I2S driver]. They will only be supported in their own drivers and the legacy I2S driver." This means even on classic ESP32, the CURRENT (v5.x) I2S driver no longer carries ADC/DAC mode - reaching it would mean either an older ESP-IDF/Arduino-core release, or specifically opting into the "legacy I2S driver" component if v5.x still ships one. Not confirmed here whether that legacy path is fully functional/maintained in current Arduino-ESP32 core releases - would need dedicated checking before treating this as a real option.

**What was NOT confirmed - the specific "fractional divider, therefore more accurate" claim.** This is plausible and matches general, commonly-repeated ESP32 lore (I2S clock generation on ESP32 is often cited as using a fractional-N divider structure specifically so I2S peripherals can hit non-power-of-two/non-clean-divisor standard audio rates like 44.1kHz precisely, which would be a genuinely different and finer-grained clock path than whatever the SAR ADC digital controller's own divider uses). However, the one source most likely to spell this out precisely (an esp32.com forum thread on I2S clock calculation, `viewtopic.php?f=19&t=9617`) was blocked by the exact same bot-check/JS-challenge wall that blocked forum access earlier in this same 645Hz investigation - this project's tooling genuinely cannot get past that specific site's bot check. The ESP-IDF I2S docs fetched instead only stated a simple integer constraint ("0 < MCLK/LRCK/channels/bits_per_sample < 64"), not fractional-divider specifics. **Do not treat "I2S has a finer/fractional ADC clock divider than adc_continuous" as confirmed** - it's a reasonable, plausible hypothesis consistent with general ESP32 architecture knowledge and worth keeping on record, but it hasn't been verified from an authoritative, quotable source this session, the same honest-limitation pattern as the original esp-idf#10612/#11623 root-cause research.

**Why this isn't the next move, even if the claim turns out to be true.** A board change from S3 to classic ESP32 (or whatever other variant retains this I2S-ADC path) is not a drop-in swap - different pinout requiring a full hardware re-layout, no native USB (classic ESP32 needs a UART-to-USB bridge chip, a real hardware/BOM change, and this project's diagnostics workflow leans heavily on USB-CDC Serial throughout every session logged here), and a substantial amount of this project's own S3-specific real-time engineering (the Core 0/Core 1 CPU-budget margin work, the gptimer wakeup-jitter history, the AD9851 bit-bang SPI timing characterization) would need to be re-established from scratch on different silicon with different peripherals and timing characteristics - none of that transfers automatically. Given the fractional resampler implemented earlier this session hasn't even been bench-tested yet, the responsible sequence is: test whether that software fix actually resolves the audible artifact first. If it does, the hardware question is moot. If raw ADC clock precision turns out to matter for some reason a software resampler fundamentally can't paper over (not yet identified as a real requirement - the resampler's whole design premise is that it doesn't need the ADC to be exact), then this is a legitimate, now-documented option to revisit rather than something to chase in parallel right now.

## 2026-09-20, later still: bench feedback on the resampler (645Hz reduced, new IMD-like noise, worse than plain 64k) leads to finding and fixing a genuine float32-precision bug in the first implementation; full derivation of the bounded-phase-accumulator fix; honest answer on whether any ADC Fs is a "closer fit"

**Bench result reported.** After the fractional resampler from the previous entry was built (not yet bench-tested at the time), the user actually ran it: the periodic tone is reduced but still present, and a number of NEW noise products appeared - their own hypothesis, IMD. Framed starkly: "if anything the 64k case was better without the resampling" - i.e. the plain integer-bleed scheme at an exact-ratio configured rate outperformed the new resampler.

**Root-caused this to a genuine bug in the resampler's own first implementation, found by reasoning through IEEE 754 float32 semantics rather than by guessing.** The state block held `static float s_virtual_pos = 0.0f;`, incremented every call by `s_true_ratio` (~4-5) with NO reset or bound - a plain ever-growing accumulator, incrementing at roughly `true_ratio x SAMPLE_RATE_HZ` ≈ 64,000-80,000 per second of continuous mic-mode operation.

**The math, stated precisely.** IEEE 754 binary32 (`float`) has a 24-bit mantissa (23 explicit + 1 implicit), so it can represent every integer EXACTLY only up to 2^24 = 16,777,216. Beyond that, the representable grid spacing (ULP) doubles at every power-of-2 magnitude boundary (2 between 2^24-2^25, 4 between 2^25-2^26, and so on) - any arithmetic that lands between grid points silently rounds to the nearest one. At ~64,000-80,000 increments/sec, `s_virtual_pos` crosses 16,777,216 in **16,777,216 / 64,480 ≈ 260 seconds ≈ 4.3 minutes** (using the 64000-configured case's own approximate increment rate) of continuous operation - a duration well within what an ordinary bench-listening session would run past. Past that point, every `s_virtual_pos += s_true_ratio` addition rounds the true ~4-5 increment to whatever the local grid spacing allows, corrupting: (a) `floor(s_virtual_pos)` - the count of "new" integer samples the drain logic believes it needs to pop each tick, and (b) the leftover fractional part used as the linear-interpolation weight for the tick's output. Both are corrupted in a way that gets progressively WORSE (courser rounding) the longer the board keeps running, since the ULP keeps doubling as the accumulator keeps growing. This is a textbook float-accumulator anti-pattern (well-known in long-running DSP/audio code specifically because of this exact failure mode), not a subtle or speculative concern - it is a mathematical certainty given the code as first written, and a strong, concrete candidate for exactly the symptom reported ("new noise products," "IMD-like," worse than the pre-resampler baseline) since it's a genuinely NEW source of timing corruption layered on top of whatever the ADC's own real hardware mismatch already was.

**The fix - a bounded [0,1) phase accumulator plus an exact-integer running target, proven algebraically equivalent to the original (buggy) design's intent.** Let `P_k` denote the (never-materialized) ideal absolute position - the conceptual sum of `true_ratio` over all ticks so far. For any real number `A = n + a` (n integer, a in [0,1)) and any `B`, the identity `floor(A+B) = n + floor(a+B)` holds (since floor(n+x)=n+floor(x) for integer n). Applying this with `A = P_{k-1}` (n=floor(P_{k-1}), a=frac(P_{k-1})) and `B = true_ratio` gives: `floor(P_k) - floor(P_{k-1}) = floor(frac(P_{k-1}) + true_ratio)`, and the new fractional remainder `frac(P_k) = frac(P_{k-1}) + true_ratio - floor(frac(P_{k-1})+true_ratio)`. This is EXACTLY the recurrence implemented now: keep only `s_phase = frac(P_{k-1})` (always in [0,1), by construction never large, hence never losing precision), each tick compute `s_phase += true_ratio; whole = floor(s_phase); s_phase -= whole;` - `whole` is the exact number of new integer sample-boundaries crossed this tick (matching `floor(P_k)-floor(P_{k-1})` precisely), and the updated `s_phase` is exactly `frac(P_k)`, the correct interpolation weight. The cumulative integer target (previously `floor(P_k) + LOOKAHEAD`, recomputed from the unbounded float each tick) is now tracked as `s_resample_target_cumulative`, a `uint32_t` incremented by `whole` each tick - since `whole` telescopes to `floor(P_k)` exactly (as `floor(P_0)=floor(0)=0`), this integer accumulator equals `floor(P_k) + LOOKAHEAD` at every tick, computed via exact unsigned-integer addition (no precision loss short of ~4.29 billion / ~64,000-80,000 per sec ≈ 16.5 hours, and unsigned wraparound arithmetic remains correct for subtraction as long as both counters wrap together, which they do here).

Also added a defensive sanity clamp on the calibrated `s_true_ratio` (rejecting anything outside `[1, ADC_SAMPLES_PER_TICK_MAX]`) before it's allowed to feed the phase recurrence, as a guard against ever seeding a garbage value into the accumulator from an unexpected counter-reset interaction - not expected to trigger under normal operation, defensive only.

**Updated the doc comments in both `adc_capture.h` and `adc_capture.cpp`'s state block to describe the bug and the fix explicitly**, rather than silently replacing the code - this project's own convention (see e.g. the gdeq/ampeq history elsewhere in these files) is to leave a legible trail of what was tried, what broke, and why, not just the current state.

**Direct answer to "are there any other ADC Fs that are a closer fit?": no known way to predict one without measuring it.** Two candidate configured rates have now been tried on real hardware - 80000 and 64000 - both of which are exact integer divisors of the 80MHz APB clock (ratios 1000 and 1250 respectively), and BOTH came out measurably inexact by amounts that don't appear to follow simply from "how clean the divisor is." This is now the THIRD piece of evidence (after the two GitHub issues and this project's own two data points) that guessing at a numerically "nicer" `ADC_CONT_SAMPLE_FREQ_HZ` isn't a reliable strategy on this driver. The only way to actually find a closer-fitting value would be a real empirical sweep - flash a candidate, read `[adc] long-window avg=...`, repeat - which costs real reflash-and-listen time per candidate and is exactly the kind of dependency the resampler is meant to remove (its whole design premise is that the configured value's exactness stops mattering once the true ratio is measured and compensated for at runtime). Given that premise hasn't been fairly tested yet (the only resampler test so far ran against the buggy first implementation), the recommended next step is re-testing the bugfixed version - ideally at the SAME configured Fs as the last listening test, so the comparison isolates "did fixing the resampler bug help" from "did changing Fs help" - before spending more time hunting for a different frequency.

**Status: fix implemented, NOT bench-tested.** This turn produced a reasoned, mathematically-verified fix for a real bug, not a confirmed resolution of the user's reported symptom. The next real test is on the bench: does the corrected resampler now sound at least as good as plain 64k without resampling, and does the residual tone/noise character actually improve over a LONG listening session (several minutes+, specifically to rule back in or out the now-fixed precision-loss failure mode as having been the dominant cause)?

## 2026-09-21: confirmed linear interpolation; user's Fs-error survey (0.16% for x16 rates, 0.8% for 80k) doesn't fit a simple "clean APB divisor" story; found and fixed a second resampler bug - stale calibration baseline across a non-mic mode excursion

**Confirmed for the record**: the resampler's output stage is a standard linear interpolator, `s_filt_prev + s_phase*(s_filt_curr - s_filt_prev)`, blending the two most recently filtered raw samples at the bounded fractional weight `s_phase` computed each tick. No other interpolation order was implemented.

**User's own Fs-error survey is a genuinely new, unexplained data point.** They report all "x16" candidate rates (16000, 32000, 48000, 64000 - i.e. `SAMPLE_RATE_HZ`'s own small integer multiples) show ~0.16% error against nominal, while 80000 alone shows ~0.8% - roughly 5x worse, closely matching the ~80645-80795 sps range already on record for 80000 (645/80000=0.81%, 795/80000=0.99%, so "~0.8%" lines up with the low end of that historical range). This directly explains why 64000 sounds better than 80000 even independent of the resampler: its own underlying hardware mismatch is genuinely smaller, not just decimated more cleanly (64000/16000=4 exactly, vs 80000/16000=5 exactly - both already exact ratios, so the decimation-side math was never the differentiator between these two specifically, only the raw ADC clock accuracy was).

**This data actively argues against the "clean APB/PLL divisor" theory this project explored earlier.** 80,000,000/48,000 = 1666.67 and 160,000,000/48,000 = 3333.33 - 48000 is NOT an exact divisor of either commonly-cited SoC reference clock (80MHz APB, 160MHz PLL_F160M), unlike 16000/32000/64000/80000 which all divide both cleanly. Yet the user's survey groups 48000 with the GOOD (0.16%) cluster, not the bad (0.8%) one dominated by 80000 alone. If "exact divisor of the reference clock" were the operative mechanism, 48000 should behave like the bad case (or at least be an outlier), not blend in with the good ones. This is now the fourth piece of evidence (after the two GitHub issues and this project's own 80000-vs-64000 comparison) that simple clock-divisor arithmetic does not predict `adc_continuous`'s real achieved-rate accuracy on this chip.

**A different, unconfirmed but plausible hypothesis: proximity to the ADC's maximum continuous sample rate.** ESP32-S3's `adc_continuous` has a documented achievable sample-rate ceiling (order of ~80-100kHz depending on channel/atten configuration - not independently re-verified against this project's exact configuration in this pass). 80000 sits close to whatever that ceiling actually is for this setup, while 16000/32000/48000/64000 all have comfortable headroom below it. Clock-divider circuits commonly exhibit their coarsest/largest relative quantization error at the extreme (often the fastest-clock/smallest-divisor) end of their achievable range - if the ADC digital controller's divider works this way, operating near the rate ceiling would plausibly produce disproportionately worse relative accuracy than operating well below it, regardless of how "clean" the specific requested number is as a divisor of any particular reference clock. This is a reasonable, mechanism-consistent hypothesis, not a confirmed one - flagging it honestly as speculation pending either a citable source or further empirical data (e.g. a data point close to the ceiling but not exactly at a previously-tried value, or one further below 64000, would help triangulate whether "closeness to the ceiling" or "specific numeric identity" is the operative variable).

**Second resampler bug found and fixed, from a new symptom report: "a funny 'mode' with strange IMDs... both on 64k and 80k... OK today so far, maybe related to another setting."** Because this recurred at BOTH configured rates, it pointed away from anything specific to `ADC_CONT_SAMPLE_FREQ_HZ` and toward something in the shared resampler/calibration machinery or a mode interaction - and its intermittency ("OK today so far") fits a scenario-triggered bug rather than a constant one.

Traced the mechanism precisely: `adc_capture_read_next_sample()` - which drives `s_resample_call_count` and therefore the calibration window - is documented as running "mic mode only." But `adc_continuous` itself (and `s_dbg_adc_samples_total`, the ISR-incremented sample counter the calibration reads) runs UNCONDITIONALLY regardless of audio source (`adc_capture_service()`'s own comment: "the ADC is always active... regardless of the current audio source"). Consequence: switch to any non-mic mode (`'s'`/`'t'`/`'p'`/etc.) for some interval, then switch back to `'m'` - the calibration baseline (`s_ratio_calib_start_samples`/`s_ratio_calib_start_calls`, last set whenever the previous mic-mode calibration window closed) is now stale relative to `s_dbg_adc_samples_total`, which kept incrementing the whole time regardless. The very next calibration window (up to `ADC_RATIO_CALIB_WINDOW_CALLS`, ~4s, of actual mic-mode calls) computes `d_samples = samples_now - s_ratio_calib_start_samples`, which includes ALL the ISR samples produced during the ENTIRE non-mic interval, not just the ~4s the window nominally covers - inflating `measured = d_samples/calls_since_calib` far above the true ratio.

The existing defensive clamp (`measured` rejected outside `[1, ADC_SAMPLES_PER_TICK_MAX]`) catches the most extreme cases (long time away from mic mode), but the math shows shorter excursions land the inflated value INSIDE the accepted range: e.g. roughly 10-30 seconds away at ~64-80k samples/sec produces an inflated ratio in the rough 12-38 range, comfortably able to fall under `ADC_SAMPLES_PER_TICK_MAX=32` and get silently accepted as if it were a real measurement - corrupting `s_true_ratio` with a wrong-but-plausible number and putting the whole resampler's per-tick sample-count/interpolation math into exactly the kind of "funny mode" reported, with new IMD-like distortion as the resampler chases a bogus target ratio.

**Fixed** by adding `adc_capture_resample_prime_calibration()` (a thin wrapper resetting `s_ratio_calib_start_samples`/`s_ratio_calib_start_calls` to "now," factored out of the existing `adc_capture_reset_diag()` logic which now calls it too) and calling it from every code path that transitions audio source INTO `AUDIO_SRC_MIC`: the `'m'` serial-command handler and the numeric-preset-apply path in `serial_commands.cpp` (guarded by `p.audio_source == AUDIO_SRC_MIC` there, since a preset can select any source). This guarantees the calibration baseline is always fresh at the moment mic mode is actually entered, closing the staleness window entirely rather than just narrowing it.

**Honest status**: this is a real, mechanistically well-supported bug and a strong candidate for the reported symptom, but it was found by code inspection following the user's own clue ("both on 64k and 80k... maybe related to another setting"), not by reproducing the funny mode directly. The user's own hedge ("maybe related to another setting") leaves room for a different or additional contributing factor - worth watching whether the symptom recurs now that this specific mechanism is closed, rather than declaring it solved outright.

## 2026-09-21, later: ADC clock-rate mechanism CONFIRMED via ESP-IDF v4.4.5 docs - full derivation, exact match to every data point on record

User located the exact clock formula this session's own web research (blocked by `gh` unavailability, GitHub API access restrictions, and a bot-walled forum thread) couldn't retrieve: ESP-IDF v4.4.5's ESP32-S3 ADC peripheral docs (`docs.espressif.com/.../esp32s3/api-reference/peripherals/adc.html`) state, for continuous mode:

> Fs = Fd / interval / 2
> Fs: sampling frequency; Fd: digital controller frequency, no larger than 5M for better performance

WebFetch confirmed this exact passage is present on that page. Espressif's own docs don't spell out `interval`'s type or rounding behavior explicitly in what was retrievable, but fitting this formula against every real measurement this project has (the historical 80645-80795 sps reading, and the user's own fresh 0.16%/0.8% survey across five rates) confirms it precisely: `Fd = 5,000,000` (exactly, or indistinguishably close) and `interval = round(Fd / (2*Fs_nominal))`, an integer register value.

**Worked calculation, every value checked against real data:**

For `Fs_nominal = 16000*m`, `interval_ideal = 2,500,000/Fs_nominal = 156.25/m`. Rounding to the nearest integer `n` and computing `Fs_actual = 5,000,000/(2n)`:

- m=1 (16000): interval_ideal=156.25, n=156, Fs_actual=16025.64, error=+0.1603%
- m=2 (32000): interval_ideal=78.125, n=78, Fs_actual=32051.28, error=+0.1603%
- m=3 (48000): interval_ideal=52.0833, n=52, Fs_actual=48076.92, error=+0.1603%
- m=4 (64000): interval_ideal=39.0625, n=39, Fs_actual=64102.56, error=+0.1603%
- m=5 (80000): interval_ideal=31.25, n=31, Fs_actual=**80645.16**, error=**+0.8065%**
- m=6 (96000): interval_ideal=26.0417, n=26, Fs_actual=96153.85, error=+0.1603%
- m=7 (112000): interval_ideal=22.3214, n=22, Fs_actual=113636.36, error=+1.4610% (getting worse)
- m=8 (128000): interval_ideal=19.5312, n=20 (rounds UP this time), Fs_actual=125000.00, error=-2.3438% (worse still, and note the sign flip - confirms genuine nearest-integer rounding, not always-floor)

The m=5 (80000) prediction, 80645.16 sps, matches the historically-logged `80645-80795 sps` reading (`moving_forward_notes.md:698`) almost exactly - well within the noise of a real bench measurement. The 0.1603%/0.8065% split matches the user's own independently-collected survey (0.16% for 16k/32k/48k/64k, 0.8% for 80k) to 3+ significant figures. This is a complete, quantitative confirmation, not a plausible-sounding coincidence - both numbers came from independent sources (Espressif's stated formula + this project's own two rounds of real bench measurement) and agree to high precision.

**This supersedes the earlier "proximity to the ADC's sample-rate ceiling" hypothesis from the previous entry** - that was a reasonable guess given the data available at the time (correctly noticed that 80000 was uniquely bad, incorrectly guessed why). The real mechanism has nothing to do with being near a rate ceiling: it's simply that `interval_ideal = 2,500,000/80000 = 31.25` happens to have a large fractional part (.25) relative to a small integer part (31), giving a proportionally large rounding error (`.25/31 = 0.806%`), whereas 16000/32000/48000/64000/96000 all happen to land on much more favorable fractional-to-integer ratios purely as a consequence of how `2,500,000 = 2^5 x 5^7` factors against small multiples of `16000 = 2^7 x 5^3`.

**Why none of 16k/32k/48k/64k/96k can ever be made exact, and why that's fine.** `16000*m` divides `2,500,000` exactly only if `2^7 x 5^3 x m` divides `2^5 x 5^7` - impossible for ANY integer `m`, since `2,500,000` only has `2^5` available and `16000` alone already needs `2^7`. So a genuine zero-error rate is mathematically unreachable via any multiple of `SAMPLE_RATE_HZ`, and 0.1603% is the real floor this project can hit while keeping the clean `ADC_SAMPLES_PER_TICK` integer-decimation property - 64000 (already the configured value) is tied for the best achievable choice, not a compromise pending further searching. m=6 (96000) ties it exactly but offers no known benefit over 64000 for this application (more raw throughput/CPU cost for a mic-bandwidth signal that doesn't need it), so there's no reason to switch again.

**Net effect on this whole 645Hz investigation thread**: the root cause is now fully closed, from the original mystery tone, through the FIFO-bleed mechanism, through the fractional resampler and its own two bugs (float precision, calibration staleness across mode switches), through to the underlying "why doesn't the ADC hit its configured rate" question - which turns out to have a clean, exact, checkable answer straight from Espressif's own documentation, not merely an acknowledged-but-unexplained driver quirk. Recommended final step: read the current `[adc] long-window avg=...` at the live 64000 setting and confirm it lands close to the predicted 64102.56 sps, as the last piece of end-to-end verification for this entire thread.

## 2026-09-21, later: new feature pair - ALC and Soft-Limit added to the envelope path, placed before predistort per the user's corrected instruction; full design rationale and open questions

Separate thread from the ADC-rate investigation above: the user asked for "ALC and soft clipping" as new envelope-path features. A subagent trace of the full envelope pipeline (production -> `envelope_floor_apply()` [smooth remap, no clamp] -> `envelope_gdeq_process()` [all-pass, confirmed no clamp] -> `envelope_ampeq_process()` [two high-shelf biquads, confirmed no clamp, CAN overshoot [0,1] arbitrarily, most notably shelf2] -> predistort or DC mapping -> the "final safety clamp" -> delay/interp -> PWM write) established exactly where a clamp does and doesn't currently exist. First placement answer from the user was ambiguous ("immediately before final hard clamp"); the user caught this themselves and corrected: "needs to go before Distortion correction since that is needed 'always'" - i.e. predistort (which has its own [0,1] input clamp, `envelope_predistort.cpp:44-45`) must keep running on every tick regardless of these new stages, so they insert BEFORE it, not merely before the final backstop clamp a few lines further down.

**Implementation**: `envelope_alc.h`/`.cpp` and `envelope_softlimit.h`/`.cpp`, called from `ssb_mic_test.ino`'s dsp_task in that order, immediately after `envelope_ampeq_process()` and before the `envelope_predistort_get_enabled()` branch.

**ALC design choice worth flagging explicitly**: this implements a gain-riding LEVELER (attenuate-only), not a full bidirectional ALC that also lifts quiet passages. The reasoning: for an audio ALC, boosting quiet audio increases perceived loudness within an already-fixed dynamic range downstream. For this polar-TX envelope path, "boosting" the envelope means literally synthesizing MORE RF frequency deviation than the source audio's actual amplitude ever called for - a much less obviously-safe operation, and arguably a form of fabricating signal rather than leveling it. `ENV_ALC_MAX_GAIN = 1.0` enforces attenuate-only as a deliberate, documented constraint, not an oversight; revisiting it would be a conscious future decision, not a bug to fix.

Algorithm: `g_desired = ENV_ALC_TARGET_LEVEL / fabsf(envelope)` (clamped to `ENV_ALC_MAX_GAIN`, with a `1e-6` epsilon guard against a near-zero envelope driving `g_desired` toward infinity - routine here since envelope legitimately passes through zero on an SSB signal, not a rare edge case), smoothed toward the currently-applied gain via an asymmetric one-pole filter (fast attack when ducking, slow release when recovering) - the "backwards" gain-computer-then-smooth topology, needing only one state variable (the gain itself) rather than a separate peak-detector state, since `g_desired` is already memoryless. Coefficients computed from real time constants (`ENV_ALC_ATTACK_MS`=2.0, `ENV_ALC_RELEASE_MS`=200.0) via the standard `1 - exp(-1/(tau*Fs))` one-pole mapping at init time - deliberately Fs-independent by construction, unlike ampeq/gdeq's hardware-fitted, `#if SAMPLE_RATE_HZ`-guarded coefficients, since these are just time constants, not a fit against a specific analog filter's measured response.

**Soft-Limit design choice worth flagging**: symmetric tanh soft-knee on BOTH rails (`ENV_SOFTLIMIT_KNEE_LO`=0.15, `ENV_SOFTLIMIT_KNEE_HI`=0.85), not just the upper one - because the envelope pipeline trace above confirmed the envelope can go negative before this point (gdeq's all-pass sections and ampeq's real, non-all-pass shelf filters can both ring past zero on a fast edge), and there is no ALC-equivalent mechanism protecting the lower rail (ALC only pulls peaks DOWN; it does nothing for a dip toward/past zero). Checked the knee continuity algebraically: at `x=knee`, `tanhf(0)=0` so value matches the unchanged region exactly, and the derivative of the tanh branch at that same point is `span*(1/span)*sech^2(0) = 1`, matching the unchanged region's own unity slope - so there's no discontinuity in either the curve's VALUE or its GAIN (dy/dx) right at the knee, which is what would otherwise risk an audible/spectral artifact exactly at the threshold most samples will be hovering near. This is a genuine mathematical check, not just "looks smooth" - worth keeping in mind if the knee values are ever retuned, since a different curve shape at the boundary would need the same continuity property re-verified, not assumed.

**Open, unresolved-by-design questions for real-hardware validation** (neither stage is bench-tested yet):
- Does ALC's own gain modulation introduce a new, previously-nonexistent AM-on-envelope artifact at the leveler's attack/release rate - i.e. does "fixing" overshoot with a time-varying gain trade a clipping artifact for a modulation artifact? Not measurable from first principles; needs real two-tone/mic IMD comparison, on/off.
- Interaction between ALC and Soft-Limit when both are on: ALC should usually have already pulled a sustained peak below Soft-Limit's 0.85 knee before Soft-Limit's curve engages at all (the two were deliberately chosen with overlapping engagement regions - see moving_forward_notes.md's matching entry), but a FAST transient ALC's release/attack can't fully catch might still reach Soft-Limit's knee - untested whether that handoff is clean or produces its own transient artifact.
- Whether either stage's presence changes what predistort's own LUT "sees" often enough to matter - predistort's LUT was built/tuned against the RAW post-ampeq envelope distribution; if ALC/Soft-Limit are enabled by default in some future preset, the LUT's own calibration data would need revisiting since its input distribution would have shifted (not a concern while both stay off by default, as they currently do).
- No canary exists yet for Soft-Limit (stateless, so there's genuinely no persistent state a bad value could corrupt going forward - unlike ALC's `s_alc_gain`, which DOES have a canary, `envelope_alc_get_canary()`) - noted here in case a future stateful extension of Soft-Limit (e.g. a smoothed knee position) is added and forgets to add one then.

## 2026-09-21, later still: build-fix postmortem - IRAM_ATTR macro visibility, a header-hygiene lesson worth generalizing

The user's first real Arduino IDE compile of the new files failed with `error: expected initializer before 'envelope_alc_process'` on the `float IRAM_ATTR envelope_alc_process(float envelope);` declaration in `envelope_alc.h`. This is worth a technical note beyond "forgot an include," because the ERROR SHAPE itself is a useful pattern to recognize again: an undeclared plain identifier used where a function-attribute macro was intended doesn't produce an "undefined macro"/"unknown identifier" diagnostic the way a missing function call would - the preprocessor just leaves the bare token `IRAM_ATTR` in the token stream, and the C++ grammar happily parses `float IRAM_ATTR envelope_alc_process` as "declare a float named IRAM_ATTR," at which point the NEXT token (`envelope_alc_process`) is the one that fails to parse, since a declaration expects `;`, `,`, or `=` there, not another identifier - hence "expected initializer before 'envelope_alc_process'" pointing at the SECOND name, not the actually-missing macro. Recognizing this error shape (a plain-identifier-shaped macro use, followed by "expected initializer before <the very next token>") as "probably a missing macro definition/include," rather than assuming a typo in the function name itself, would have been the faster diagnostic path if this had needed debugging rather than direct code inspection.

Root cause: `IRAM_ATTR` comes from ESP-IDF's `esp_attr.h`, included by `ssb_dsp.h` (`#include "esp_attr.h"` near its own top) - `envelope_ampeq.h`/`envelope_gdeq.h` use `IRAM_ATTR` freely only because they already `#include "ssb_dsp.h"` for unrelated reasons (its `ssb_shelf_biquad_t`/coefficient types). `envelope_alc.h`/`envelope_softlimit.h` are new, minimal headers with no other reason to pull in `ssb_dsp.h`, so they never got `esp_attr.h` transitively - a genuine gap in this session's own "check the codebase's conventions before writing new code" process: the convention survey done before implementing (predistort/ampeq headers, the pipeline call site, free serial letters, the settings struct) never checked WHERE a macro token like `IRAM_ATTR` that both existing modules use was actually coming from, just that they used it. **Generalizable lesson for future new envelope-stage files in this codebase**: don't assume a macro used in an existing header (`IRAM_ATTR`, but potentially others) is globally available just because sibling headers compile fine with it - trace it to its actual `#include` origin before writing a new header that also uses it, especially in this project where headers deliberately keep their own include lists minimal rather than relying on a common umbrella header.

**Fix applied**: `#include "esp_attr.h"` added directly to both `envelope_alc.h` and `envelope_softlimit.h`, each with a comment explaining why (rather than pulling in the much heavier `ssb_dsp.h` for one macro). Brace/paren-balance and static_assert/field-count sanity checks done before delivery were insufficient to catch this class of bug - they can't see macro-definition visibility across translation units - a genuinely useful reminder that "looks syntactically sound" is not the same claim as "compiles," and this session should keep saying so explicitly rather than letting a balance check imply more confidence than it earns.

## 2026-09-21, later still: Soft-Limit's lower-rail bug - full derivation of why the naive tanh mirror was wrong, and why the fix is the best achievable, not a perfect one

First real bench exercise of Soft-Limit (after the build fix above) surfaced a genuine algorithm bug, not another build issue: "at low signal level it raises the env level DC value." Directly verified by evaluating the original formula in Python:

```
y(x) = KNEE_LO - span*tanh((KNEE_LO - x)/span), span = KNEE_LO = 0.15
y(0.00) = 0.03576
y(0.02) = 0.04505
y(0.05) = 0.06258
y(0.10) = 0.10177
y(0.15) = 0.15000   (matches the knee exactly, as designed)
```

**y(0.00) = 0.0358, not 0.** This is the bug in one number: a true envelope null (x=0) - not a rare edge case, but the specific, heavily-studied operating point this entire null_bias_investigation.md exists to characterize - gets a nonzero output under the original design. Every value from 0 up to the 0.15 knee is pulled upward by a broadly similar amount, which reads on the bench as "raised DC level at low signal."

**Why this happened - a geometric mismatch, not an arithmetic slip.** The upper knee (0.85) and the lower knee (as originally designed, also 0.15) are NOT geometrically equivalent positions, even though picking the same numeric distance-from-rail (0.15 either way) makes them look symmetric. The upper knee sits at 0.85, with the segment (0.85, 1.0) being genuinely rare, near-clipping territory - values in that zone are unusual peaks, and it's reasonable to shape them. But the lower "knee" at 0.15 does NOT correspond to "genuinely rare, near-clipping territory" on that side - the segment (0.0, 0.15) is ORDINARY, common, completely legitimate low-level signal (quiet audio, and every envelope zero-crossing). Mirroring the upper knee's shape onto a lower threshold placed the "correction zone" on top of the most heavily-trafficked part of the signal's actual range instead of on the rare part - the numeric symmetry of "0.15 away from each rail" concealed a real asymmetry in what each zone actually represents operationally.

**Why a truly symmetric fix is mathematically impossible, and what "best achievable" means here.** Setting the lower threshold to the only geometrically-correct value - the rail itself, envelope=0.0 - creates a real constraint problem: any curve engaging only for envelope < 0.0 must (a) equal 0 at envelope=0 (continuity with the passthrough region, which equals `envelope` there), (b) stay >= 0 for all envelope < 0 (the entire point - never emit a negative value), and (c) be monotonic in the physically sensible direction (a smaller/more-negative input should never produce a LARGER-magnitude output than a less-negative one... more precisely, more negative input should map to output that moves away from - not back past - the previous output, i.e. no sign reversal in the response's own trend). Properties (a)+(b) together are incompatible with also matching the passthrough region's slope of 1 at that same point: a curve with slope 1 at x=0, for x infinitesimally less than 0, would necessarily dip to a small NEGATIVE value first (that's what slope 1 means locally) - directly violating (b). So a smooth, non-negative-bounded lower-rail curve CANNOT have the same slope as the passthrough region at the boundary - some slope mismatch (a corner in the derivative, though not in the value) at exactly envelope=0 is unavoidable for ANY valid curve, not just this particular design. This is worth stating plainly: the fix below is the best achievable within this real constraint, not a fully seamless one - a small, deliberate trade-off, not an oversight.

**The fix, and why THIS particular curve shape (not just "any curve through the origin") was chosen.** Given the corner at x=0 is unavoidable, the design freedom left is choosing HOW STEEP that corner is - i.e., what slope the x<0 branch approaches x=0 with, given it can't be 1 (matching the passthrough side). The chosen curve, `y = span*(1 - sech(x/span))` for x<0, has slope exactly ZERO at x=0 (sech is an even function, so its first-order term in a Taylor expansion around 0 vanishes: `sech(u) ≈ 1 - u²/2` for small u, giving `y ≈ x²/(2·span)` - purely quadratic, no linear term at all). This was a deliberate choice among the available options, verified against the concrete alternative of just mirroring tanh (which DOES have a nonzero, unit-magnitude-ish slope at the origin, since `tanh(u) ≈ u` for small u, making the "naive corrected" mirror behave like `y ≈ |x|` for small excursions - full-wave rectification of any near-zero ripple). A quick calculation shows why zero slope is the better choice: for a small symmetric ripple of amplitude A oscillating around a true envelope of zero, a HARD clamp at 0 already introduces bias (half-wave rectification, average ≈ 0.318·A, a well-known result for a rectified sinusoid) - that bias is an unavoidable physical consequence of the envelope needing to stay non-negative, not a defect of this new stage. A `y≈|x|` mirror would FULL-wave rectify the same ripple (average ≈ 0.637·A) - MORE biased than simply not having this stage at all (i.e., worse than what the existing downstream hard clamp already does). The chosen `y≈x²/(2·span)` shape, by contrast, gives much LESS bias than the hard clamp for small ripples (quadratic in A rather than linear), while still bounding large excursions gracefully (saturating toward the `ENV_SOFTLIMIT_LOWER_SPAN` plateau, 0.15, as x→-∞ - verified: `sech(u)→0` as `|u|→∞`, so `y→span`). Numerically: envelope=-0.01 -> y≈0.00033 (new) vs. y≈0.01 (naive |x| mirror) vs. the ~0 a hard clamp gives for that same instant (though the hard clamp's own bias comes from the ASYMMETRY of clamping only the negative half over time, not from any single sample).

**What remains genuinely open, stated plainly rather than glossed over.** This is a reasoned, defensible fix for a real, numerically-confirmed bug, and it is provably better than both the original design (no more constant DC injection) and a naive symmetric mirror (less small-ripple bias) - but "better than two specific alternatives" is not the same claim as "correct" or "optimal," and neither has been checked against real hardware. Two follow-up questions worth real bench time: (1) how much genuine sub-zero ringing does `envelope_gdeq_process()`/`envelope_ampeq_process()` actually produce on this hardware - if it's negligible in practice, this entire lower-rail branch may be solving a problem that barely occurs, and the simplest honest fix would be to drop it in favor of a plain floor-at-0 (identical to what the downstream final hard clamp already does, so nothing would be lost); (2) whether the deliberately-flat-at-origin shape has any audible/spectral signature of its own near envelope nulls specifically - this project's history (see the "null_bias"-named entries earlier in this file) shows that behavior right at zero-crossings has previously mattered in ways that weren't obvious from the algebra alone, so this shouldn't be assumed clean without a real check.

## 2026-09-23: `ssb_mic_test_commands.md` audited against `serial_commands.cpp` - found this investigation's own diagnostic tools (`J`/`K`/`H`/`F`) had never been documented in the command reference at all

Requested alongside adding `'l'`/`'S'` (ALC/Soft-Limit) to the command doc: a full cross-check of every `c == '<char>'` branch in `serial_commands.cpp` against every command already documented there. Ten keys turned up with zero mentions in the whole file, most notably the four AD9851-gated frequency-jump tracing tools this investigation itself built and relies on - `'J'` (per-event jump log), `'K'` (slow-mean divergence trigger), `'H'` (permanent-anchor held-frequency detector), and `'F'` (full-rate raw freq/env CSV capture) - plus `'O'` (two-tone phase-generator EXACT/LEGACY/MOD160 cycle) and four general diagnostics/utility keys (`'L'`/`'M'`/`'V'`/`'Z'`). None of this changes any finding already on record here - `serial_commands.cpp`'s own code and doc-comments were always the authoritative source these entries were written against - but it's worth noting for anyone using `ssb_mic_test_commands.md` as their primary reference going forward rather than reading the source: that file was missing exactly the tools most relevant to reproducing this investigation's own measurements. All ten are now documented (a new "Frequency-jump / null-bias tracing tools" subsection covers `J`/`K`/`H`/`F` together, pointing back to this file for the full mechanism/history rather than duplicating it). Re-verified afterward that every command letter in the code now has at least one mention in the doc.

## Open, un-actioned next steps

1. **Two candidate fix directions identified, neither implemented.** This
   codebase has a documented history of two abandoned null-handling
   attempts (`envelope_floor.cpp`'s NOTE 1/NOTE 2) — be deliberate here,
   validate incrementally on real hardware.
   - **Targeted**: for samples flagged near-null (same test used above),
     replace the raw `atan2`/`wrap_pi` resolution of the ±π direction with
     something less numerically fragile than the instantaneous Q/I ratio
     at a near-zero point.
   - **Principled** (matches the forward note already left in
     `envelope_floor.cpp`: "track/pre-warp the phase trajectory through the
     null, not freeze it"): extrapolate the phase trajectory through the
     null from the clean, well-defined phase rate on either side, rather
     than trusting `atan2`'s read for the 1-2 samples actually at the
     crossing.
2. Map the bias-vs-absolute-frequency relationship more finely than the 6
   fixed `TWOTONE_BAND_PRESETS` points (would help confirm/deny the
   Hilbert-filter-response explanation above).
3. Verify (or refute) the "mostly a test-artifact, not a real voice problem"
   assessment against actual speech/mic input — not done this session.
4. The still-separately-unexplained puzzle from earlier in this
   investigation: why toggling `I` (envelope interpolation, which provably
   never touches `freq_dev_hz`) appeared to shift the *measured* center
   frequency in some earlier observations. Leading candidate explanation
   reached at the time: the `I`-on/`I`-off shifts observed were smaller
   (~couple Hz) than a separately-observed ~15Hz baseline drift on the
   timescale of typing a sentence, so much of what looked like an `I`-caused
   shift may simply have been that independent drift being sampled at
   different moments — never isolated with a controlled dwell test.
5. **UPDATE 2026-08-31, end of session**: user's direct assessment is that
   the current x4 envelope-interpolation algorithm (`I`, `envelope_interp.h`/
   `.cpp`) makes two-tone stability *much worse* — a clearly audible
   instability, not just the subtle few-Hz effect item 4 above was framed
   as. This reads as stronger/more definitive than item 4's framing and
   should take priority over it when this is picked back up: before
   spending more time on the null-crossing bias mechanism above (which is
   independent of `I` — provably doesn't touch `freq_dev_hz`), first
   characterize what `I` itself is doing to produce an audible instability.
   `envelope_interp.h`'s own header comment documents four earlier
   real-hardware failure modes (v1-v4, all fixed) before landing on the
   current implementation — worth re-reading that history first in case
   this is a fifth, not-yet-identified failure mode of the same kind,
   rather than assuming it's connected to the null-bias work above. Not
   investigated further this session — parked here as the more urgent of
   the two open `I`-related threads.

## 2026-09-23: ADC anti-alias filter generalized to N=6/N=8 - the pole-placement math and the CPU-headroom numbers behind it

Logged here (parallel to moving_forward_notes.md's matching entry) for the numeric detail, even though the trigger (mic ADC filter stopband) is adjacent to rather than part of the null-bias investigation itself - same "detailed technical log across both files" convention this project has used all session.

**The math, generalized from the existing N=4 case.** `ssb_adc_filter.c`'s 4th-order design already builds a genuinely exact higher-order digital filter as a cascade of independently-bilinear-transformed 2nd-order sections - factoring a rational transfer function into 2nd-order sections and then applying the bilinear transform to each commutes with applying it to the whole, so this is not an approximation of a "real" higher-order filter, it IS one, for any even N. Extending to N=6/N=8 needed no new theory, just more terms of the same formulas:

- **Butterworth**: per-stage Q for pole pair `k` (k=1..N/2) is `Q_k = 1/(2*cos(theta_k))`, `theta_k = (2k-1)*pi/(2N)`, all stages sharing the same `fc_hz`. Computed and cross-checked against the known textbook table: N=4 reproduces the code's existing `0.5411961`/`1.3065630` exactly; N=6 gives `0.5176381`/`0.7071068`/`1.9318517` (the middle stage's Q of exactly `1/sqrt(2)` is a coincidence of the angle landing on `pi/4`, not a special case); N=8 gives `0.5097956`/`0.6013449`/`0.8999762`/`2.5629154`.
- **Chebyshev Type I**: same ellipse-pole-pair construction as the existing N=4 code - `v=(1/N)*asinh(1/epsilon)`, `alpha_k=sinh(v)*sin(theta_k)`, `beta_k=cosh(v)*cos(theta_k)`, `omega0_k=sqrt(alpha_k^2+beta_k^2)`, `Q_k=omega0_k/(2*alpha_k)`, same `theta_k` as the Butterworth case above - then the same shared-frequency-scale bisection (~40 iterations, same `[0.3fc, 1.2fc]` bracket reused unchanged since the qualitative shape - monotonic falloff past the ripple band - doesn't change with order) to land the TRUE composite -3dB point exactly at `fc_hz`.

**Numeric verification** (Python replica of the exact C code - `biquad_lpf_design()`'s bilinear-transform coefficients and `biquad_mag_db()`'s magnitude-response formula, not an independent approximation - at the user's live `fs=64000Hz`, `fc=3000Hz`, `ripple_db=1.0`):

| Order | Family | -3dB check @ fc | @6000Hz | @8000Hz | peak in-band ripple |
|---|---|---|---|---|---|
| 4 | Butterworth | -3.0103dB | -24.87dB | -35.68dB | (none, maximally flat) |
| 6 | Butterworth | -3.0103dB | -37.28dB | -53.52dB | (none) |
| 8 | Butterworth | -3.0103dB | -49.71dB | -71.36dB | (none) |
| 4 | Chebyshev | -3.0103dB | -36.60dB | -48.37dB | 0.987dB |
| 6 | Chebyshev | -3.0103dB | -59.06dB | -76.86dB | 0.987dB |
| 8 | Chebyshev | -3.0103dB | -81.97dB | -105.76dB | 0.988dB |

Two things worth pulling out of this table. First, **the -3dB point holds exactly at every order** (bisection converges correctly regardless of N) - confirms the generalization is sound, not just plausible-looking. Second, and this is the point that mattered for the user's actual question: **peak in-band ripple is essentially order-INDEPENDENT at a fixed ripple spec** (0.987-0.988dB across N=4/6/8) - this is what makes "design a proper higher-order filter at the same spec" categorically different from "run the existing filter through itself twice," which would instead literally square the response and double the ripple in dB terms (an already-"quite high" ~1.4dB observed-by-the-user peak becoming ~2.8dB) while also pulling the effective -3dB point down and doubling group delay. The mathematical reason the two are different: a genuine higher-order Chebyshev design still has exactly ONE ripple-band with N/2 humps of the SAME bounded height (set by `epsilon`, i.e. by `ripple_db` alone, independent of N) - more humps packed into the same passband width, not taller ones - whereas cascading two identical copies of one filter multiplies (in linear terms) or doubles (in dB terms) that same one filter's response with itself, which is a fundamentally different operation on the transfer function.

**CPU-cost side, from the same-day live `'V'` reading** (`max_busy_us=60` / `period_us=62`, `adc=28` of that): unlike the 2nd->4th jump (made when this project had ~16.5us of measured headroom to spend), this 4th->8th jump is happening against a ~2us margin. A rough per-sample instruction-count estimate: each `ssb_biquad_process()` call is 5 multiplies + 4 adds (Direct Form II Transposed) in the inner loop, so going from 2 stages (4th order) to 4 stages (8th order) adds 2 more biquad evaluations per raw ADC sample; at the user's live ~4 raw samples/dsp-tick (64000/16000), that's 8 extra biquad evaluations per tick, ballpark ~1-2us on an Xtensa LX7 core depending on compiler codegen and loop overhead - not free relative to a 2us margin, unlike the "negligible" characterization the 2nd->4th jump earned when headroom was ample. This estimate is arithmetic, not measured - the honest next step is a live `'f'`-cycle + `'V'`-read A/B on real hardware before trusting either 6th or 8th order in a preset. Flagged in moving_forward_notes.md's matching entry that 6th order exists specifically as a smaller, safer-to-try-first step for exactly this reason.

## 2026-09-23, later: `ADC_CONT_SAMPLE_FREQ_HZ=64000` committed to the repo - closes the repo/board divergence noted in the ADC-filter-order entry above

Short closing note to this investigation's own 64000-vs-80000 rounding-error analysis (the table further up: 64000/32000/16000/48000/96000 all land on the same ~0.16% ADC-clock rounding error, vs. 80000's ~0.806%, from `2,500,000 = Fd/2` having too few factors of 2 relative to any multiple of `SAMPLE_RATE_HZ=16000`): that conclusion had been acted on by the user locally but never committed here, so `adc_capture.h` disagreed with the actual bench configuration this whole investigation's later entries were written against. User confirmed the 64000 setting "works much better as previously discussed" and asked for it to be committed - done, with `adc_capture.h`/`config.h`'s current-state comments updated to match (see moving_forward_notes.md's matching entry for the exact edits). No new analysis here - the mechanism was already fully derived in this file's earlier entries; this is just closing the loop between what was proven and what the repo actually contained.

## 2026-09-24: compressor makeup-gain math - why the old fixed formula and the new peak-tracking one differ, and why the new one is exact rather than an approximation

Logged here for the derivation detail, parallel to moving_forward_notes.md's entry (same "detailed technical log across both files" convention used all session, even though this is adjacent to rather than part of the null-bias work itself).

**The old formula was a dB-domain approximation of a linear-domain curve.** `compressor_process()`'s actual per-sample compression is linear-domain interpolation: for `env > threshold`, `compressed = threshold + (env-threshold)/ratio`, i.e. a straight line in LINEAR units above the threshold, not a straight line in dB (which is what a textbook log-domain ratio compressor does: `output_db = threshold_db + (input_db-threshold_db)/ratio`). The removed `compute_compressor_makeup_gain()` computed its correction entirely in the dB domain (`threshold_db=20*log10(threshold)`, `reduction_db=threshold_db*(1-1/ratio)`, `makeup=10^(-reduction_db/20)`) - algebraically this is `makeup = threshold^(1/ratio - 1)`, which is NOT the same number as `1/compressed` evaluated at `env=1.0` (the exact linear-domain inverse). For `threshold=0.3`, `ratio=3.5`: exact linear inverse at env=1.0 is `1/(0.3 + 0.7/3.5) = 1/0.5 = 2.0`; the old dB-domain formula gives `0.3^(1/3.5 - 1) = 0.3^(-0.71429)`, which is **≈2.363** (Python-verified: `compute_compressor_makeup_gain(0.3, 3.5) = 2.363114788617384`) - **correcting an arithmetic slip made earlier in this same investigation**, where evaluating `0.3^(-0.7143)` by hand was mis-computed as "≈1.828" and logged that way; 1.828 was never right, 2.363 is the actual value both the dB-domain formula and this project's real, shipped `compute_compressor_makeup_gain()` produce for these settings, and it's the figure used everywhere else in this log (including the Limit-only entry below). The genuine, still-valid comparison is old-dB-domain (2.363) vs exact-linear-inverse-at-peak=1.0 (2.0): different numbers even under the old formula's own assumption (peak=1.0) - a pre-existing approximation nobody had flagged, not the bug the user reported, but worth recording since it means the OLD behavior was never quite "exactly cancels the reduction at 0dBFS" even on paper, only approximately.

**The new formula has no such gap.** `makeup_gain = peak_env / peak_compressed` where `peak_compressed = threshold + (peak_env-threshold)*inv_ratio` is computed from the EXACT SAME linear-domain expression `compressor_process()` itself uses for `gain` - it's the literal algebraic inverse of the compressor's own curve, evaluated at whatever `peak_env` currently is, not a separately-derived dB-domain approximation evaluated at an assumed peak. That's what makes "peak value stays the same" true by construction rather than by a coincidentally-close approximation: for any envelope value the signal actually reaches (not just 1.0), `compressed(peak_env) * makeup_gain(peak_env) = peak_env` exactly, by algebraic construction (`compressed * (peak_env/compressed) = peak_env`).

**Bounding check**: as `peak_env -> threshold+` (from above), `peak_compressed -> threshold`, so `makeup_gain -> peak_env/threshold -> 1` - continuous with the `makeup_gain=1.0` branch used at/below threshold, no discontinuity at the boundary (mirrors the existing `gain` calculation's own continuity at `env=threshold`). As `peak_env` grows, `makeup_gain` grows roughly proportionally (since `peak_compressed` grows sub-linearly, by `inv_ratio<1`, for `peak_env` well above threshold) - bounded in practice because `peak_env` itself is a physically-bounded audio envelope, not a free variable.

## 2026-09-24, later: Ratio vs Peak-normalize compressor modes - the exact makeup-gain comparison and a worked crest-factor example

Logged here for the derivation detail, parallel to moving_forward_notes.md's entry (same cross-file convention used throughout this session).

**Why Ratio mode always needs MORE makeup gain than Peak-normalize for the same target peak.** Both modes derive makeup gain from the same tracked `peak_env`, threshold, and (for Ratio) `inv_ratio`. Ratio mode: `makeup = peak_env / peak_compressed` where `peak_compressed = threshold + (peak_env-threshold)*inv_ratio < peak_env` (strictly, whenever `ratio>1` and `peak_env>threshold`) - i.e. Ratio mode has to overcome its OWN squashing before it can reach any target. Peak-normalize mode: `makeup = target/peak_env` directly, no squashing to overcome. So for the SAME target level, `Ratio's makeup > Peak-normalize's makeup` whenever `target <= peak_env` (the normal operating case) - not just an intuition, an algebraic consequence of Ratio mode doing strictly more work (squash, then restore) to reach the same place Peak-normalize reaches in one step (scale directly).

**Numeric table** (threshold=0.3, ratio=3.5, a quiet/noise-floor content point at 0.08 sitting below threshold in both modes, Peak-normalize target=0.90 matching `COMP_PEAK_NORMALIZE_TARGET`):

| peak_env | Ratio makeup_gain | Ratio: quiet(0.08) becomes | Peak-normalize makeup_gain | Peak-normalize: quiet(0.08) becomes |
|---|---|---|---|---|
| 0.45 | 1.31 | 0.105 | 2.00 | 0.160 |
| 0.70 | 1.69 | 0.135 | 1.29 | 0.103 |
| 0.90 | 1.91 | 0.153 | 1.00 | 0.080 |

Worth noting explicitly: Peak-normalize's makeup gain isn't uniformly smaller in every row - at peak_env=0.45 (well below the 0.90 target), Peak-normalize actually injects MORE gain (2.00x) than Ratio (1.31x) does, because Peak-normalize is trying to push a modest peak all the way up to 0.90, while Ratio's ratio-3.5 curve barely engages that close to threshold. The "less noise floor gain" property specifically holds once `peak_env` is already at or above the Peak-normalize target (0.90 here) - exactly the "assume we'll hit the high peaks" precondition the user's own reasoning specified. Below that, which mode injects less gain depends on how far `peak_env` sits from each mode's respective reference point (threshold's ratio curve vs the fixed target) - worth being precise about this rather than claiming a universal advantage.

**Crest-factor demonstration** (this is what "assume the high crest factor" cashes out to numerically - from the direct-conversation worked example, restated here for the permanent record): a quiet passage at 0.08 between syllable peaks, ratio-compressor case:

| syllable peak | peak stays at | quiet (0.08) becomes | crest factor before → after |
|---|---|---|---|
| 0.45 | 0.45 | 0.105 | 15.0dB → 12.6dB |
| 0.70 | 0.70 | 0.135 | 18.8dB → 14.3dB |
| 0.90 | 0.90 | 0.153 | 21.0dB → 15.4dB |

Peak-normalize mode, by construction, leaves crest factor COMPLETELY UNCHANGED at every peak level (quiet and loud scaled by the identical factor) - that's the explicit tradeoff of choosing it over Ratio: no crest-factor reduction at all, in exchange for a simpler, single-gain-term signal path and (above the target peak) less gain injected into whatever's below threshold.

## 2026-09-24, later still: Limit-only compressor mode - the algebra connecting it to the user's exact real-hardware reading (0.01 -> 0.024)

Logged here for the derivation detail, parallel to moving_forward_notes.md's entry (same cross-file convention used throughout this session).

**Restating the original (pre-2026-09-24) design precisely, since it's what the user was actually describing.** Before any of today's changes, `compressor_process()` had a single, fixed `makeup_gain` (computed once, at configuration time, via the now-removed `compute_compressor_makeup_gain(threshold, ratio)`) applied UNCONDITIONALLY to every output sample: `return x * gain * c->makeup_gain;`, where `gain` is threshold-gated (1.0 below threshold, the squashing ratio above it) but `makeup_gain` is NOT threshold-gated at all - it's a flat multiplier on everything, always, whenever the compressor is enabled. For the user's real settings (threshold=0.3, ratio=3.5), that fixed factor is 2.363114788617384 (Python-verified, see the corrected figure in the entry above). This means the user's assumption in an earlier message this session ("if a simple gain reduction was applied then it's still below the threshold so not amplified") did not describe the actual shipped code - the original code amplified below-threshold content by 2.363x too, unconditionally, which is exactly consistent with what they then went on to observe on real hardware.

**Mapping the observation to the number.** User reported an instantaneous `'V'` reading rising from 0.01 (compressor off) to 0.024 (compressor on) - a ratio of 0.024/0.01 = 2.4 - "and also a corresponding rise in peak output." Since 0.01 is well below threshold=0.3, `gain=1.0` in both the old and new sense; the entire 2.4x rise is attributable to the old fixed `makeup_gain` alone, and 2.4 sits close enough to the exact 2.363 figure (within the precision of a single instantaneous snapshot reading, not a careful RMS/peak measurement) to confirm the mechanism: the old design's ~8dB (`20*log10(2.363)=7.47dB`, `20*log10(2.4)=7.60dB` - both ≈7.5dB, matching the user's "~8dB" description) output rise was the fixed makeup gain showing up uniformly on quiet content AND on the peak, which is exactly why the user had to retrim master gain to avoid the end-stop - the peak was being pushed up by the same ~2.36-2.4x factor as everything else, regardless of whether the compressor's squashing curve had engaged on it at all.

**The fix, algebraically.** `SSB_DSP_COMP_MODE_LIMIT_ONLY` keeps the exact same threshold-gated `gain` term (`gain = (threshold + (env-threshold)*inv_ratio)/env` for `env>threshold`, else `1.0`) but hard-sets `makeup_gain=1.0` always, in this mode only. For any output sample `x`: `output = x * gain * 1.0 = x * gain`. Below threshold: `output = x` exactly (bit-identical passthrough - the noise floor is untouched, not scaled by 2.363x the way the original always did). Above threshold, using the peak as the worked example: `old_peak_output = peak_compressed * 2.363` (old fixed makeup applied on top of the squash); `new_peak_output = peak_compressed * 1.0 = peak_compressed`. So `new_peak_output = old_peak_output / 2.363` - exactly the "reduce the peak by the same factor" the user asked for, and by the identical 2.363x (not merely a similar-sized) factor that was inflating everything else too, so the whole output (quiet and loud alike) drops back to where the raw squashing curve - with no restoration step riding on top - would put it.

**Why this is a different, smaller claim than today's earlier "peak stays the same" fix.** The Ratio-mode fix earlier today makes `makeup_gain` track `peak_env` so that `compressed(peak_env) * makeup_gain(peak_env) = peak_env` exactly - i.e. the compressed peak gets RESTORED to the ORIGINAL uncompressed peak. Limit-only makes no such restoration - its peak output is `peak_compressed`, which is strictly LESS than `peak_env` whenever `ratio>1` and `peak_env>threshold` (that's what compression above threshold means). Limit-only's peak, expressed against the two other modes for the same `peak_env`: `Limit-only's peak_compressed < Ratio's makeup-restored peak_env`, and unrelated in general to `Peak-normalize`'s target-scaled result (which can land above or below `peak_env` depending on how `peak_env` compares to `COMP_PEAK_NORMALIZE_TARGET`). The tradeoff is explicit and by design: Limit-only trades away RMS-raising/crest-factor-reduction entirely in exchange for a hard guarantee that the compressor can never make ANY sample larger than the corresponding, non-makeup-boosted `x * gain` - i.e. it can only ever pull peaks down toward (never past) where the raw signal already sat below its own uncompressed peak, never push output above the pre-compression peak level. That guarantee is what directly rules out the end-stop symptom the user described.

## 2026-09-25: compressor redesign - calibration algebra, the level-0 special case, and the transient-overshoot mechanism behind the new safety clamp

Logged here for the derivation detail, parallel to moving_forward_notes.md's 2026-09-25 entry (same cross-file convention used throughout this session).

**Why bisecting for ratio at a 0dB RMS-gain target does NOT converge to "a limiter"**, worth spelling out algebraically since it's the reason level 0 is special-cased rather than just being the bottom of the same search family as levels 1-6. The search holds makeup as the exact peak-restoring value at each trial ratio: `makeup(ratio) = cal_peak / curve(cal_peak, ratio)`. As `ratio -> 1+`, `curve(a, ratio) -> a` for every `a` (the dB-domain curve degenerates to the identity line at ratio=1, by construction: `out_db = thr_db + (a_db-thr_db)/1 = a_db`), so `makeup(ratio) -> cal_peak/cal_peak = 1`, and the WHOLE transfer function `curve(a,ratio)*makeup(ratio) -> a` - i.e. bit-identical passthrough, not attenuation-only limiting. Since RMS gain is a monotonically non-decreasing function of ratio in this family (more compression => more restored gain, the core finding from the earlier "does compression raise RMS" thread), the ratio that produces exactly 0dB of measured RMS gain via this search IS `ratio=1` (verified numerically: the bisection's lower bound `ratio_lo=1.001` was never moved up off its starting value when a 0dB target was tried informally) - a bypass, which is not what "a simple audio side limiter" means. A limiter, by definition, still reduces genuine overshoot; it just adds nothing on top. That behavior needs `makeup` pinned at 1.0 independently of wherever the search would otherwise put it, which is exactly what the level-0 special case does (fixed `ratio=5.0`, fixed `makeup=1.0`, not derived from any search).

**Confirming the level-0 choice behaves as a genuine (if very mild) limiter, not a no-op.** With `ratio=5.0`, `threshold=0.30`, at the calibration peak (0.85): `compressed = 0.30 + (0.85-0.30)*(1/5.0)` in the exact linear-domain sense - except this compressor uses the LOG-domain curve, not the linear one, so precisely: `thr_db = 20*log10(0.30) = -10.458dB`, `peak_db = 20*log10(0.85) = -1.412dB`, `out_db = -10.458 + (-1.412 - (-10.458))/5.0 = -10.458 + 1.809 = -8.649dB`, `compressed = 10^(-8.649/20) = 0.3696`. Gain at the peak = `0.3696/0.85 = 0.435` (about -7.2dB of attenuation AT THE PEAK specifically) - genuinely engaging, not negligible. Measured over the WHOLE real recording (not just its single peak instant, since most samples sit well below threshold and pass through completely untouched), the net effect averages out to the reported -0.44dB RMS change - small in aggregate because threshold=0.30 vs a 0.85 peak-normalized signal with ~21dB crest factor still leaves the bulk of the signal's energy below threshold, exactly the same "threshold rarely engages on RMS-dominant content" mechanism the whole G4AHN.wav investigation surfaced in the first place, just now operating as intended (protecting rare peaks) rather than as an accident (an unreachable threshold).

**The transient-overshoot mechanism, precisely** - this is the numerically significant new finding this entry exists to record. `compressor_process()`'s `gain` is a function of `env` (the SMOOTHED, one-pole envelope follower - attack_coef derived from `attack_ms=3.0`), but is applied by multiplication to `x`, the INSTANTANEOUS raw sample: `output = x * gain(env) * makeup`. In steady state (env has settled), `|x| <= env` holds on average across a cycle for periodic content, so `gain(env)` computed from the settled envelope correctly bounds the corresponding instantaneous samples. But immediately following a fast onset (a plosive consonant, for instance), `|x|` can jump toward its new, larger value within a single sample, while `env` - a one-pole filter with time constant `attack_ms=3.0` at fs=16000Hz (`attack_coef = 1-exp(-1/(16000*0.003)) = 1-exp(-0.02083) = 0.02062`) - only moves ~2% of the way to the new value per sample. For roughly the first `attack_ms` worth of samples after such an onset, `env` still reflects something closer to the PRE-onset (lower) level, so `gain(env)` is still close to 1.0 (or whatever the lower level's gain was) - meanwhile `x` has ALREADY jumped to its new, larger value. The product `x * gain(env_stale) * makeup` can therefore exceed what `x * gain(env_settled) * makeup` (the steady-state, "worst-case envelope" analysis) would predict, because it's using a stale (too-permissive) gain against an already-large `x`.

**Direct numeric confirmation, cross-checking the offline Python calibration against a from-scratch standalone C reimplementation of the exact firmware algorithm** (`/tmp` scratch build this session, reading `G4AHN.wav` directly, running the identical one-pole envelope follower + `piecewise3_interp()` + fixed per-level makeup): a "worst-case steady-state" analysis (feed `env=1.0` directly into the curve, evaluate `curve(1.0,ratio)*makeup`) predicts safe outputs at every shipped level (max 0.981 at level 1, decreasing to 0.859 at level... - see moving_forward_notes.md's table for the full per-level figures). Running the ACTUAL algorithm over the real recording instead shows genuine excursions past that prediction starting at level 3 (measured peak 1.088 vs the 0.944 steady-state prediction for that level) and growing to 1.464 at level 6 (vs a steady-state prediction of 0.892) - a real, reproducible, non-hypothetical gap of over 3dB between "what the envelope-based safety analysis predicts" and "what the instantaneous-sample output actually reaches," entirely attributable to the attack-lag mechanism above. Clamp-engagement counts confirm this scales with how aggressive the level is (5 samples out of 138272 at level 3, rising to 106 at level 6) rather than being a fixed artifact - consistent with "more compression narrows the settled-vs-transient gain gap the same way it does for the RMS/THD tradeoffs found earlier this session," i.e. the SAME underlying mechanism (finite attack time) that this project's 2026-09-24 peak-tracking makeup-gain scheme already carried, just now visible in the fixed-makeup design too because it was actually measured against real speech rather than assumed away.

This is also why the fix is a plain hard output clamp (`if (audio_sample > 1.0f) audio_sample = 1.0f;` etc., placed after the whole compressor+EQ block in `ssb_dsp_process_sample()`) rather than a longer attack time or a look-ahead redesign: a longer attack would blunt the compressor's response to genuine sustained level changes (the exact tradeoff `COMP_PEAK_HOLD_RELEASE_MS`'s 2026-09-24 entry already discussed for a related follower), and a look-ahead design needs buffering/added latency this project has deliberately avoided everywhere else in the audio path. The clamp is a cheap, unconditional backstop for a rare (well under 0.1% of samples at any shipped level) event, not the primary control mechanism - consistent with this project's existing layered-safety pattern (e.g. `max_freq_dev_hz`'s hard clamp alongside the slew-rate limiter it complements).

## 2026-09-25, later: presets 4/5/8/9 synced from the user's live-tuned `settings.h` - closes the repo/board preset divergence

Short closing note, same "sync the board's live values into the repo" pattern as the `ADC_CONT_SAMPLE_FREQ_HZ=64000` entry above. User's own working `settings.h` had replaced four bench/diagnostic placeholder presets (AMTEST/FMTEST entries never meant to be permanent) with real mic-tuned ones from live use - `"ReSync Micr"`, `"V4 Microphone tunedD7"`, `"Micr WhiteNoise"`, `"Micr Voice"` at preset slots 4/5/8/9. No new derivation here - diffed the uploaded file against the repo's copy directly (only those four preset lines differed; the struct definition itself, including today's earlier `mic_gain_db`/`comp_level_db` additions, matched exactly), applied the diff, and confirmed it re-diffs clean. See moving_forward_notes.md's matching entry for the exact per-preset value changes.

## 2026-09-25, later still: real-hardware `c`-off/`c`-on comparison - the arithmetic behind "CPU cost confirmed negligible" and the mic_gain=33dB flag

Logged here for the derivation detail, parallel to moving_forward_notes.md's matching entry (same cross-file convention used all session).

**Timing.** `[timing] dsp breakdown: audio_fx=3 fir=12 atan2=9 sqrt=9` in both the `c`-off and `c`-on (level 6) snapshots - `audio_fx`'s running high-water mark is bit-identical whether the compressor engages or not, over the whole session since boot. This is a genuinely stronger claim than "the added cost is small" - it's "the added cost didn't move the measured high-water mark at all," on real hardware, for the first time this session (every earlier CPU-cost number, including the ~2x-plain-linear-curve figure from the x86 benchmark, was estimated or simulated, never measured on the actual ESP32-S3).

**Envelope delta.** `20*log10(mean(0.429,0.505) / mean(0.203,0.218,0.226)) = 20*log10(0.467/0.216) = +6.71dB`, against a calibrated target of +5.96dB for level 6 (the C cross-check figure from the redesign entry above) - a 0.75dB gap, well within what a handful of arbitrarily-timed instantaneous snapshots of bursty speech would be expected to show even if the calibration were perfect.

**Mic gain arithmetic.** `G4AHN.wav`'s calibration mic gain was `0.85/0.4178 = 2.0345x = +6.17dB`. The live board's `mic_gain_db=33.0` is `10^(33/20)/10^(6.17/20) = 21.95x` more linear gain than that - not a rounding difference, a genuinely different operating point. Two candidate explanations, neither confirmed: (1) this board's analog front-end/capsule is roughly 27dB quieter than whatever produced `G4AHN.wav`, in which case 33dB may be the CORRECT number for this specific hardware; or (2) 33dB is the old `master_gain_db` value from the "ReSync Micr" preset (33.3dB) carried over verbatim into the new field without re-deriving it, in which case real peaks are likely landing well past the table's assumed 0.85 and the compression-level dB labels would be reading optimistic. Distinguishing these needs an actual peak reading on a loud syllable with `'c'` off - not attempted from this data alone (the two `envelope=` readings available, 0.429/0.505, are `c`-ON readings, i.e. already past the compressor, not useful for checking the PRE-compression peak this question is actually about).

## 2026-09-25, later still: mic_gain=33dB explained (wrong preset assumed) - closing note on the previous entry's flag

Short correction to this file's immediately preceding entry. The `mic_gain_db=33.0` reading analyzed there came from preset 8 ("Micr WhiteNoise" - a deliberately low-volume speaker/noise test, a genuinely quieter source than speech), not preset 9 ("Micr Voice"). No hardware anomaly, no leftover master-gain value carried over by mistake - both candidate explanations offered in the previous entry were wrong because the premise (that 33dB was meant to represent normal speech levels) was wrong. Preset 9's actual speech-tuned gain is 6.3dB, independently landing within 0.15dB of this session's own `G4AHN.wav`-derived calibration figure (+6.17dB) - see moving_forward_notes.md's matching entry for that comparison. Preset 9 itself still runs `compressor_enable=false` with its gain in the old `master_gain_db` field - user asked to leave it that way for now rather than migrate it to `mic_gain_db`/enable the compressor.

## 2026-09-25, later still: the compdB=9/10 "ceiling" - why bisecting for a literal +9dB or +10dB target on this recording can't work, and what "level 10" means instead

This is the numerical heart of today's "extend compdB to 10 max" request, kept here in the bias-investigation file because it's exactly the kind of thing that could have been silently papered over rather than surfaced: **a request that sounds achievable (add more numbers to the dial) turned out to have a real physical limit underneath it, and shipping the literal request without flagging that would have produced two identical, mislabeled entries.**

**The mechanism**: `find_ratio_for_target_db()` bisects `ratio` between `ratio_lo=1.001` and `ratio_hi` (originally 50.0) until the measured RMS gain on `G4AHN.wav` matches the target dB. This works fine for levels 1-6 (and, as it turns out, up through about +8.5dB) because the target is genuinely reachable somewhere inside that ratio range. For a target of +9dB or +10dB, no ratio - not even an enormous one - reaches it, because the compressor's curve has a hard structural limit: at `SSB_DSP_COMP_THRESHOLD=0.30`, as `ratio -> infinity` the curve doesn't keep compressing harder forever, it converges to a flat hard limit at exactly 0.30 (anything above threshold gets pulled all the way down to the threshold value, nothing more or less). That's the maximum amount of gain-reduction-then-makeup this shape can ever apply. Bisection against an unreachable target doesn't error or diverge - it just walks `ratio_lo` up to `ratio_hi` and stops there, silently returning "the best it could do," which is exactly how the first attempt (targets 9 and 10 both landing on `ratio=50.0`, identical +8.36dB) happened. **The bug-shaped symptom (two identical table entries) was a correct report of an infeasible request, not a defect in the search.**

**Quantifying the actual ceiling, independent of any search bound**: rather than trust wherever the bisection happened to give up, I computed the true `ratio -> infinity` case directly - a genuine hard limiter at threshold, `output = min(env, threshold)` for `env > threshold`, with makeup fixed at the peak-restoring value `cal_peak/threshold = 0.85/0.30 = 2.8333` (+9.05dB of makeup gain, note: makeup gain itself CAN exceed the achieved RMS gain, because makeup restores peak, not RMS, and heavy limiting reduces the peak-to-RMS ratio) - run through the exact same envelope-follower simulation as everything else. Result: **+8.53dB measured RMS gain, full stop.** No ratio, however extreme, can exceed this on this recording at this threshold - it's not a search artifact, it's the shape's actual asymptote. This matches (within 0.1dB) an entirely separate, earlier-session calculation of the "hard-clip ceiling" at threshold=0.30 (8.43dB, via a simpler synthetic-peak method rather than the full envelope-follower simulation) - two different methods, two different points in this session, same answer to within rounding. That convergence is the strongest evidence yet that this ceiling is a real property of (threshold=0.30) x (this recording's crest factor / dynamics), not a bug anywhere in the pipeline.

**What this means for "level 10"**: rather than accept a broken pair of identical, over-labeled entries, or refuse the request outright, levels 7-10 were recalibrated against explicitly-chosen sub-ceiling targets (+7.0/+7.6/+8.1/+8.35dB) so each one is a real, distinct point - but level 10's label ("10") no longer describes its behavior ("+8.35dB") the way every level below it does. This is now called out explicitly in three places (`ssb_dsp.h`, `ssb_dsp.c`, `ssb_mic_test_commands.md`) specifically so this gap between label and reality can't be mistaken for a future bug report - see moving_forward_notes.md's matching entry for the full numbers, the preset 8/9 changes, and the standalone-C cross-check.

**One loose thread, not yet run down**: the standalone C reimplementation (float32, matching the actual firmware's arithmetic) measured levels 7-10 at +6.91/+7.47/+7.94/+8.16dB against the Python calibration's (float64) +7.00/+7.60/+8.10/+8.35dB targets - a gap that grows from -0.09dB at level 7 to -0.19dB at level 10, noticeably larger than the ~0.04dB gap already seen at level 6 and below. Plausible explanation is compounding float32 rounding error in the envelope-follower/curve math becoming more visible as ratio grows, since higher ratios mean the curve is more sensitive to small input differences near the threshold - but this hasn't been isolated or proven, only noticed. Worth re-examining if levels 9-10 get bench-measured on real hardware and come in further under their nominal labels than levels 1-6 did.

## 2026-09-25, later still: two `max_busy_us` readings that should be comparable aren't - 60us (2026-09-23) vs 42-43us (2026-09-25, confirmed Chebyshev8) - numbers on record, gap unexplained

User confirmed the `c`-off/`c`-on real-hardware timing capture logged earlier this file (`max_busy_us` 42->43, `dsp breakdown: audio_fx=3 fir=12 atan2=9 sqrt=9`) was taken with `ADC_LPF_MODE_CHEBYSHEV8` - the 8th-order filter - actually selected. Recording the numeric tension this creates rather than letting it pass quietly, since it directly touches an open risk item (2026-09-23's ADC-filter-order CPU-headroom entry) that this file hasn't otherwise revisited:

| Reading | Date | Filter mode | `max_busy_us` | Budget | Headroom |
|---|---|---|---|---|---|
| "fresh on-demand 'V' read" | 2026-09-23 | not confirmed (pre-dates confirming 8th order was ever actually selected and read back) | 60 | 62.5us (`period_us=62`) | ~2us |
| `c`-off/`c`-on capture | 2026-09-25 | CHEBYSHEV8 (confirmed by user, this entry) | 42-43 | 62.5us | ~19-20us |

Both readings claim to be on the same board, same `period_us`/62.5us tick budget, post-64000Hz-ADC-rate-commit. If the 2026-09-23 reading really did have 8th-order Chebyshev active already (its own text doesn't say either way - it reads as a baseline taken "to answer this question" about adding 6th/8th order, which is at least as consistent with it being taken on whatever mode was active BEFORE the new modes were wired in), then a ~17-18us swing in `max_busy_us` for what should be a near-identical configuration needs an explanation, and none is confirmed yet. Candidates, none verified:

1. **Resampler calibration-window state.** `adc_capture_read_next_sample()`'s fractional resampler only recomputes its calibrated `true_ratio` every `ADC_RATIO_CALIB_WINDOW_CALLS` (~4 seconds at 16kHz) - if the 2026-09-23 reading landed on or near a recalibration event and the 2026-09-25 one didn't (or vice versa), that could plausibly shift the `adc=` bucket's cost, which the 2026-09-23 entry itself already flagged as the dominant (28us) contributor to ITS OWN 60us total, "almost certainly dominated by the fractional resampler's calibration/interpolation math... not the LPF itself." Neither capture's log entry records whether `adc_capture_ratio_is_calibrated()` was true at the time - the function exists specifically for this kind of check and wasn't used for either.
2. **Different filter mode than assumed on 2026-09-23.** The 2026-09-23 entry's own wording doesn't actually claim 8th order was selected during ITS `max_busy_us=60` read - it's plausible that reading was taken on whatever mode was already active (4th-order Chebyshev, per preset 8/9's settings.h history at the time, or even OFF mid-test) and the "+1-2us for 4th->8th" cost was a back-of-envelope ESTIMATE layered on top, never itself bench-confirmed until now.
3. **Something else entirely** - different `audio_source`, different point in the boot/mode-switch cycle (the FIFO drain logic behaves differently right after a mode switch - see `ADC_SAMPLES_PER_TICK_MAX`'s catch-up threshold), or just normal run-to-run jitter larger than either reading's single-snapshot nature can rule out.

**What this changes and doesn't change**: it does NOT retract the 2026-09-23 entry's caution, and does NOT yet confirm 8th-order Chebyshev is definitely cheap on this hardware in general - one clean 42-43us reading, now confirmed to be WITH Chebyshev8 active, is good evidence against the "~2us margin, tight" fear specifically, but it's still a single data point, not a swept comparison across all seven `'f'` modes the way the 2026-09-23 entry originally asked for. That sweep - cycling `'f'` through OFF/4th/6th/8th x Butterworth/Chebyshev while reading `'V'`'s `max_busy_us` and `adc=` bucket after each, on the same board in the same session - would settle both this discrepancy and the original headroom question in one pass, and remains the concrete next step, same as it was on 2026-09-23.

## 2026-09-25, later still: envelope_floor is a floor, not a gate - the numeric case for why 0.02 was a bad step size, and why it's not a real fix for low-level crackle either way

Kept here for the underlying reasoning, parallel to moving_forward_notes.md's matching entry (same convention all session): user chasing "spiky/crackly output at low micr input" after fixing a separate analog-noise-floor issue ("now below the ADC threshold (just)"), proposed a squelch, tried `envelope_floor` instead, and asked for its step size to shrink from 0.02 to 0.001.

**Why `envelope_floor` can't be a squelch, mechanically.** `envelope_floor_apply(raw_envelope) = floor + (1-floor)*raw_envelope` is an affine map from `[0,1]` to `[floor,1]`. At `raw_envelope=0` (the ONLY input a squelch would ever want to force to silence), the output is exactly `floor` - by construction, the one input value a squelch must zero is the one value this function is guaranteed to make NON-zero (for any floor>0). There is no threshold, no branch, no comparison against a level anywhere in this function - it cannot "zero out" anything, structurally, independent of what floor is set to. This isn't a tuning gap, it's what the function is FOR (see envelope_floor.h's own header: deliberately smooth/continuous specifically so there's no threshold to cross, after two threshold-having designs were tried and reverted for real distortion - phase-freeze snap-back, and hard-clamp derivative-discontinuity-driven intermod rise).

**Quantifying "lots of dBs too high."** Because `apply(0)=floor` exactly, `floor` IS the achievable null/silence floor in linear envelope units, so `20*log10(floor)` is directly interpretable as "how much suppression can this envelope still deliver at its quietest, once floor is engaged at all":

| floor | dB (20·log10(floor)) |
|---|---|
| 0.001 (new min step) | -60.0 dB |
| 0.005 | -46.0 dB |
| 0.01 | -40.0 dB |
| 0.02 (old min step) | -34.0 dB |
| 0.05 | -26.0 dB |

Against this project's own documented true-null baseline (~-58dBc, `envelope_predistort.h`, deepest measured nulls with floor=0/off), the OLD step size meant the very first `'x'` press already gave up ~24dB of achievable null depth, with literally no way to ask for anything finer - off, or -34dBc, nothing in between. The NEW 0.001 step's first press (-60dB) costs less than 1dB relative to that same baseline - a proportionate "nudge," not a cliff.

**Cross-checked against the output hardware's own quantization**, since the user explicitly framed 0.001 as "1 PWM res": `RSET_MOD_LEDC_RES=10` bits on the currently-active `PWM_COMPARISON_ENABLED` path (`envelope_output.h`) puts one raw duty LSB at `1/1023=0.0009775` in duty-fraction terms - the user's 0.001 estimate is correct to within ~2%. Worth noting this is the duty-domain LSB, not necessarily the same number after `env_pwm_offset`/`env_pwm_scale`'s remap into that duty range, but it's the right order of magnitude regardless and there's no benefit to a step finer than what the hardware can actually resolve - 0.001 sits close to that natural floor (no pun intended) rather than being an arbitrary round number.

**Two real bugs caught and fixed while making this change, not reported by the user**: the `'x'`/`'z'` serial replies and the `'P'` preset-dump line both printed `env_floor` at 2 decimal places. At 2 decimals, EVERY value the new 0.001 step can produce below 0.01 (i.e. the first 9 of the first 10 steps) prints as `0.00` - not just a cosmetic annoyance for the live serial reply, but a real data-loss bug in the preset dump specifically: pasting a `'P'`-dumped line with, say, `floor=0.005` freshly dialed in would have silently written `0.00f` into `settings.h`, discarding the exact value someone had just spent several keypresses finding. Both bumped to 4 decimal places; verified with a standalone harness (`gcc -fsyntax-only` clean, then a small runtime test confirming `envelope_floor_get()` reads back `0.0050` after 5 `'x'` presses, not `0.01` or `0.00`).

**What this does NOT establish**: no claim here that `envelope_floor` at any value actually fixes the reported crackle - only that (1) it structurally cannot be a squelch, so it should not be evaluated as if it were one, and (2) the tool for exploring it finely was itself broken (too coarse to try small values, and would have lost them on save even if tried). The crackle's actual mechanism remains undiagnosed. The "noise floor is fixed, just barely" framing is at least consistent with envelope excursions landing in the predistort LUT's steep near-null region often enough to be audible - which IS the problem `envelope_floor` was built for - but that's a hypothesis inherited from the problem's shape, not something measured this entry.

## 2026-09-25, later still: Mic Squelch's derivation - why 5ms/80x, why hysteresis, and the honest burst-duration limitation

Kept here for the underlying math, parallel to moving_forward_notes.md's matching entry: `envelope_floor`'s finer step "didn't do what is needed" (confirmed, as predicted when it shipped), so built an actual gate instead.

**Why placement before the compressor is load-bearing, not stylistic.** `compressor_process()` in `ssb_dsp.c` computes `gain` from the SMOOTHED envelope relative to `SSB_DSP_COMP_THRESHOLD` (1.0 below threshold, a curve-derived reduction above it), then returns `x * gain * c->makeup_gain` - `makeup_gain` is a FIXED per-level constant (see the 2026-09-25 compressor-redesign entries above) applied to literally every sample, above or below threshold. A squelch placed downstream of this (on the demodulated envelope, where `envelope_floor`/ALC/Soft-Limit all live) would be gating a signal where quiet noise has already received the SAME fixed dB boost real speech gets (e.g. +6.4dB at comp_level=6) - the compressor's whole job is narrowing the dynamic range between quiet and loud, which is exactly the property a gate needs preserved to tell them apart. Gating right after Mic Gain, before any of that, means the detector sees noise at its true, un-boosted level.

**Quantifying the 80x single-sample click rejection figure, not just asserting it.** A one-pole filter's response to a single-sample impulse of amplitude `A` at time 0 moves the filter's state by exactly `attack_coef * A` on that one step (then decays away) - `attack_coef = 1 - exp(-Ts/tau)` where `Ts=1/16000s` and `tau=0.005s` (`SSB_DSP_SQUELCH_ATTACK_MS`) gives `attack_coef ~= 0.0125`. For that single-step nudge to exceed a threshold `T`, the impulse needs `A > T/0.0125 = 80*T` - i.e. an isolated click needs to be ~80x the threshold's amplitude to trip the detector by itself, PURELY from the attack time constant's own math, no simulation needed to derive it (though one was run to confirm it - see below).

**Confirmed numerically, not just derived**: a standalone C harness (`/tmp/squelch_test/test_squelch.c` and `test_burst.c`, not part of the delivered firmware) reimplementing the exact three-stage algorithm found: (1) a synthetic scenario (0.002-amplitude noise floor + an 80x-amplitude single-sample click every 50ms, threshold=0.01) never opened the gate over 0.5s of simulated audio - `gate_open=0` throughout, output peak exactly `0.00000`; (2) a duration x amplitude sweep (results tabulated in moving_forward_notes.md's matching entry) confirmed the ~80x figure lines up almost exactly for a true single-sample (0.0625ms) burst, and showed the expected monotonic tradeoff - as burst duration approaches the 5ms attack time constant itself, the amplitude needed to trip the gate drops sharply, down to just 2x threshold by a 5-10ms burst. This is a textbook one-pole-filter property (a k*tau-duration event roughly needs 1/k the amplitude of a k=1 event to produce the same step response), not something specific to this implementation, but worth having measured directly against THIS code rather than assumed from the general theory.

**The Schmitt-trigger hysteresis (close threshold = 0.5x open threshold) is standard noise-gate practice**, not something derived fresh here - included specifically because a plain single-threshold compare would let a signal parked exactly at the threshold "chatter" (open/close every time the smoothed detector crosses that one value in either direction from residual jitter), which would itself sound like a new, gate-introduced crackle - precisely the failure mode this feature exists to prevent, so it would have been a bad omission to skip it.

**Honest limitation, stated plainly rather than buried**: the whole detector-attack/burst-duration tradeoff means this squelch is tuned to reject GENUINELY BRIEF clicks (sub-millisecond), not to distinguish a multi-millisecond burst of noise from a multi-millisecond burst of quiet speech - no envelope-following gate can fully do the latter, since at that duration the two are not distinguishable by level and timing alone. If the user's actual reported "spiky/crackly" symptom turns out to be several-ms bursts rather than instantaneous clicks, this design's numerically-clean rejection of single-sample clicks won't be the whole story - that's a real, acknowledged gap, not a hedge added for form's sake.

## 2026-09-25, later still: first real-hardware read on Mic Squelch - positive, but thin on detail

User: "That works well. Needs more testing but certainly the right approach." The first time this feature has touched a real, noisy mic input rather than a synthetic test signal - meaningful because the whole design (5ms detector attack, ~80x click-rejection figure, 0.5x hysteresis ratio, 0.01 default threshold) was reasoned from first principles and cross-checked only against invented click/burst/tone scenarios, never against the actual crackle it was built for. A positive real-world result is genuine evidence the underlying APPROACH (gate before the compressor, three-stage smoothed topology) is sound for this problem, independent of whether the specific numbers end up needing adjustment.

Recording what's NOT known from this message, so a later session doesn't overstate it: no threshold value, no A/B against squelch-off, no mention of whether the specific short-burst-vs-click limitation flagged in the previous entry was ever actually the bottleneck. "Certainly the right approach" is a statement about the ARCHITECTURE (gate before compressor, hysteresis, separate gain-smoothing), not necessarily a claim that every numeric default is already correct - worth getting the tuned threshold value logged here once it's known, both for its own sake and as a second real-world data point on how far off the 0.01 starting guess was.

## 2026-09-26: Mic Squelch was creating an atan2(0,0) degenerate-phase lock, not just a null-bias-adjacent artifact - and it fed straight into this file's own env2-weighted diagnostic

Directly relevant to this file's subject: this is a second, DIFFERENT mechanism than the null-crossing bias characterized above, but it lands on the exact same `atan2(Q,I)` call and the exact same env2-weighted diagnostic accumulator this file tracks - worth logging here, not just in moving_forward_notes.md, so a later session doesn't confuse the two.

**What was different from the characterized null-bias mechanism**: everything above in this file concerns phase behavior when I and Q pass NEAR zero in the ordinary course of a real, varying signal (a null crossing) - transient, generally sub-cycle, and the whole reason `env2_dphi_sum`/`env2_sum` exist is to correctly down-weight those moments in an average. What the Mic Squelch bug introduced was categorically worse: I and Q both pinned at EXACT, bit-constant zero for the entire duration the gate stayed closed (potentially seconds, not a fraction of a cycle), because the squelch's gain multiplied `audio_sample` itself before the Hilbert delay line. `atan2(0,0)` doesn't merely become noisy or ill-conditioned near this point the way it does approaching a genuine null - once both `flush_denorm`-cleaned inputs hit literal `0.0f`, it returns one fixed value, forever, until real signal returns. That's a hard phase lock, not an amplified version of the null-bias effect - confirmed with a standalone test harness (`/tmp/squelch_test/test_envelope_gate.c`) showing 100.0% of samples identical to the previous sample's phase once the gate settled closed, versus 0.0% under the corrected architecture (see moving_forward_notes.md's matching entry for the full harness description and numbers, including a signed-zero subtlety in `atan2` that the harness had to account for to reproduce the real firmware's exact behavior via `flush_denorm`).

**Why this matters for env2_dphi_sum/env2_sum specifically**: with the OLD (buggy) architecture, a closed gate drove `envelope` to near-zero too (since I=Q=0 makes `sqrt(I^2+Q^2)=0` as well, for once-consistent reasons - envelope and the frozen phase were both downstream of the same zeroed I/Q), which would have naturally down-weighted those frozen-phase samples in the energy-weighted average almost to nothing anyway. So this specific bug likely wasn't silently corrupting the null-bias diagnostic's own numbers in a way that would have shown up as a wrong-looking result - the frozen `dphi=0` contributions were multiplied by `env2~0` before summing. It COULD still have mattered for `dphi_sum`/`near_null_dphi_sum` (the plain, unweighted diagnostic accumulators kept "for mechanistic diagnosis," per this file's/ssb_dsp.h's own framing) - those aren't energy-weighted, so a long run of frozen `dphi=0` samples would have diluted their plain average toward zero for a reason having nothing to do with the null-bias mechanism this file is actually about, IF squelch happened to be enabled during a capture used for that diagnostic. Worth remembering if `dphi_sum` results ever look unexpectedly close to zero on a session where squelch was active.

**Fix and why it keeps the diagnostic meaningful going forward**: per the user's exact instruction ("keep the hilbert fed with noise but reduce env to zero"), the squelch gate is now applied to `envelope` directly, AFTER it's computed from real (always-unattenuated) I/Q and BEFORE the null-bias accumulator block runs - so `env2_dphi_sum`/`env2_sum`/`near_null_dphi_sum`'s energy weighting continues to reflect what will actually be transmitted (true zero when gated, matching envelope exactly), while `phase`/`dphi` themselves are computed upstream from genuine, ungated I/Q and are never frozen. This means: (1) a gated-closed period now contributes essentially nothing to the energy-weighted average (correctly, since no RF power is being sent) via the same `env2~0` mechanism as before, but for the RIGHT reason (a deliberate, real envelope gate) rather than an accidental phase freeze; and (2) `dphi_sum`'s plain average will now reflect genuine, real-noise-driven phase differences during a closed gate, not artificial zeros - a small behavior change worth remembering if `dphi_sum` (not the energy-weighted version) is ever compared across sessions taken with squelch on before vs. after this fix.

**Not yet done**: no new capture has been taken with squelch enabled to confirm any of the diagnostic-accumulator reasoning above empirically - this entry is derived from the code and the standalone harness, not a fresh null-bias measurement run with squelch active.

## 2026-09-26, later: real-hardware confirmation - frozen-carrier fix works, but diagnostic-accumulator interaction above is still unmeasured

User, on real hardware: "Yes working thankyou" - confirms the previous entry's fix (gate applied to `envelope` post-Hilbert, `phase` computed from always-real I/Q) actually resolves the reported carrier-tone side effect on air, not just in the standalone test harness. This confirms the FIX itself, not the separate, still-open reasoning in the previous entry about how squelch interacts with `env2_dphi_sum`/`dphi_sum` - no null-bias-specific capture was taken or mentioned, so that part of the previous entry remains exactly as speculative as it was: derived from code reading, not measured.

## 2026-09-29: two-tone IMD test data point, directly relevant to "trusting two-tone/IMD test data" - a real, repeatable 9.3dB IMD3 asymmetry, not obviously connected to the characterized null-bias mechanism

Logged in full (settings, complete data table, comp on/off comparison) in moving_forward_notes.md's matching entry; this entry covers only what's specific to this file's subject - whether/how the null-crossing phase-bias mechanism characterized elsewhere in this file might explain what was measured.

**The asymmetry itself**: on a 700/1700Hz two-tone drive (tones matched to <=0.03dB), the high-side third-order product (2f2-f1, +2700Hz) measures -27.2 to -27.6dBc, while the low-side product (2f1-f2, -300Hz) measures -36.7dBc - a ~9.3dB gap, present almost identically whether the compressor is on or off. This is exactly the kind of "is the two-tone/IMD data trustworthy" question this file exists to think about, so worth asking directly: **is this asymmetry connected to the null-crossing phase bias characterized earlier in this file?**

**Tentative answer: not obviously, but not ruled out either - genuinely uncertain, flagged as such rather than guessed at.** The characterized null-bias mechanism concerns how `atan2`-derived instantaneous phase behaves as the analytic signal's envelope passes near zero (a null crossing) - it biases the PLAIN (unweighted) average of `dphi` samples, an effect this file's own "Confirmed measurement table" and env2-weighting work were built to quantify and correct for. A two-tone IMD3 asymmetry, by contrast, is a classic symptom of asymmetric AM/PM conversion or an odd-order nonlinearity with a phase-dependent (not just magnitude-dependent) component somewhere in the envelope or phase path - a DIFFERENT class of mechanism than a transient phase artifact at instantaneous nulls, though not necessarily unrelated: a system whose phase computation has SOME asymmetric bias (even one not previously characterized, or a variant of the known null-bias effect that also perturbs two-tone products rather than only single near-null samples) could plausibly show up as exactly this kind of odd-order sideband asymmetry. This is speculation stated as speculation, not a finding - no measurement done this entry to connect the two.

**What WOULD distinguish the two**: if this asymmetry is null-bias-related, it should correlate with how often/how deeply the two-tone envelope (which itself has periodic nulls at the two tones' beat frequency, here 1kHz - same spacing as the IMD comb itself, notably) crosses near zero - e.g., it might change if `null_bias_threshold` or the envelope's actual null depth changes (predistort LUT revision, floor setting, ALC/Soft-Limit interaction). If it's instead a garden-variety AM/PM or gain-compression asymmetry unrelated to null crossings, it should persist essentially unchanged across envelope-floor/predistort settings that change null depth without changing the fundamental nonlinearity. Neither comparison has been run - this test only varied the compressor, which barely moved the asymmetry at all (both conditions: ~9.1-9.5dB), which is at least consistent with the asymmetry NOT being a compressor artifact, but doesn't by itself confirm or exclude a null-bias connection either.

**Not yet done**: no envelope-floor/predistort/ALC sweep run against this same two-tone pair to test the null-bias-correlation hypothesis above; no comparison against this project's earlier 700/1900Hz-spaced IMD baselines (different tone spacing means the beat/null frequency differs too, which would itself be an interesting cross-check if this IS null-bias-connected - a different beat frequency should shift the associated artifact's frequency signature).

## 2026-09-29, later: the IMD3 asymmetry's likely cause found - relative_delay/AM-PM timing, not null-bias, not audio noise, not (mainly) the presence EQ

Follow-up to this file's previous entry, which left open whether the 9.3dB high/low IMD3 asymmetry connects to this file's own characterized null-crossing phase bias. Short answer after actually chasing it (full reasoning in moving_forward_notes.md's matching entry): **most likely not the same mechanism, though both live in the same general "envelope/phase path timing" territory.**

Ruled out numerically rather than by guessing: audio-band noise (the comb's shape - a sharp fall from +2700Hz to +3700Hz - doesn't match a raised noise floor), and the presence-EQ tone-amplitude-mismatch mechanism this file's neighbor document found back on 2026-09-03 (recomputed against TODAY's actual +2.0dB presence gain: only ~1.4dB of differential tone drive, far short of the ~9.3dB observed).

What fits well instead: `group_delay_fit_notes.md`'s own repeated finding, since 2026-09-01, that "High 3rd IMD" runs 3.7-7.2dB worse than "Low 3rd IMD" across every `relative_delay_samples`/gdeq/interp combination ever logged - the same direction, same rough magnitude family as this test's 9.3dB gap (at `relative_delay=2.68`, which was never one of the specifically re-tuned comparison points). That's a DIFFERENT mechanism than this file's null-crossing phase bias: the null-bias finding is about `atan2`'s behavior as I/Q pass instantaneously near zero (affects the PLAIN average of `dphi`, corrected by env2-weighting), while the relative_delay-sensitive asymmetry is a timing/dispersion mismatch between the amplitude (envelope) and phase (frequency) reconstruction paths that generates genuinely asymmetric third-order sidebands - a steady-state spectral effect, not a per-sample phase-averaging artifact. They could theoretically interact (both ultimately trace back to how faithfully this polar/EER architecture reconstructs a signal whose envelope has deep excursions), but nothing found this entry connects them directly - treating them as two separate, both-real phenomena until something concrete says otherwise.

**Not yet done**: no direct test of whether retuning `relative_delay_samples` (the concrete next step recommended in moving_forward_notes.md's entry) also moves this file's own null-bias diagnostic numbers at all - would be worth a quick check if that sweep ever gets run, purely to confirm these really are independent rather than coincidentally correlated.

## 2026-09-29, later still: CORRECTION - the 700/1700Hz asymmetry was a noise-contaminated bin, so its (non-)connection to null-bias is moot; the small residual at 700/1900Hz is the more relevant data point

Retracts most of this file's immediately preceding entry. User re-ran the two-tone test at 700/1900Hz specifically to move the near-in products away from wherever the earlier reading was contaminated, and confirmed it directly: "it shows that the power in 250Hz is picking up the background noise previously." Full numbers and the corrected diagnosis are in moving_forward_notes.md's matching entry; this entry covers only the null-bias-relevance angle.

**The ~9.3dB gap this file's previous entry tried to connect (or rule out connecting) to the characterized null-crossing phase bias was mostly a measurement artifact, not a transmitter property** - so there was never a real ~9.3dB asymmetry for the null-bias mechanism (or anything else) to explain in the first place. That previous entry's speculative "what would distinguish the two" test (vary null depth via envelope-floor/predistort/ALC and see if the asymmetry moves) is still a reasonable idea in principle, but should now target the much smaller, real residual (0.9-2.7dB at 700/1900Hz) rather than the inflated 9.3dB figure - a much harder thing to resolve cleanly against normal run-to-run measurement noise.

**Net position, updated**: no evidence connecting this project's two-tone IMD asymmetry to the null-crossing phase-bias mechanism, in either direction - the data point that prompted the question turned out to be measuring something else almost entirely. If this connection is worth investigating further, it needs a deliberately-designed test (e.g., comparing the small residual asymmetry across settings that specifically change null depth), not an incidental read of an IMD sweep taken for a different purpose.

## Where to resume

The null-crossing bias itself is well-characterized and, per the "likely
real-world significance" assessment above, may matter more for trusting
two-tone/IMD test data than for actual on-air voice quality — nothing there
is urgent. **Higher priority: item 5 above (`I` making two-tone stability
audibly worse)** — that's a clear, user-confirmed regression, not just a
measurement subtlety, and should be looked at before returning to the
null-bias fix directions. When picking this back up: start with item 5
(re-read `envelope_interp.h`'s v1-v4 failure history, then characterize what
changes with `I` on vs off on a two-tone signal specifically), then, once
that's resolved or understood, come back to "Confirmed measurement table"
above to refresh context on the null-bias mechanism and decide between the
two fix directions in "Open, un-actioned next steps" #1. The instrumentation
(`null_bias`/`null_bias2`/
`null_bias3`, the `n` command) is already in place and doesn't need to be
rebuilt — just `r` and read.
