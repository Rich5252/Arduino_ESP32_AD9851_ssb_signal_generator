# Envelope group-delay equalizer — fitting notes

**2026-09-01: now covers TWO analog filter hardware variants** — see the
"2026-09-01 refit" section below for the new PNP BC327 + gate-attenuator
filter's fit, how it compares to the original BC337 filter this document
started with, and an open question about whether its extra insertion loss
could hurt IMD3/IMD5 despite the improved delay flatness. Also see the
final section for a related (separate) finding: this filter's TF itself
shifts with DC operating point.

Source data (original entry below): `SallenKey_LP_filter_BC337.txt`
(LTspice AC sweep of the actual original 2-pole Sallen-Key RSET
reconstruction filter circuit, including the BC337 buffer stage — not an
idealized 2-pole formula).

## Method

1. Parsed the sweep's magnitude (dB) and phase (degrees) vs frequency.
2. Built a cubic spline of unwrapped phase vs frequency and differentiated
   it analytically to get group delay: `tau(f) = -(1/2*pi) * dphi/df`.
   Cross-checked against raw central-difference derivatives on the sweep's
   own (non-uniform, ~10 points/decade) frequency points — agreement within
   ~0.02us in-band, ~0.2us worst case near 3.2kHz. The spline is fit only
   over 100Hz-20kHz (the sweep's own phase reverses direction above ~65kHz,
   an artifact of the transistor buffer stage's own high-frequency rolloff,
   irrelevant to the audio band and excluded from the fit).
3. Fit two cascaded first-order digital all-pass sections (`ssb_allpass1_t`,
   Fs=10000Hz to match `SAMPLE_RATE_HZ`) by minimizing the peak-to-peak
   spread of (analog + digital) combined group delay over a dense grid from
   100-4300Hz (covers the 700/1900Hz two-tone fundamentals, the 1200Hz beat
   and its harmonics through the 4th, and the +4300Hz 5th-order IMD
   product), using Nelder-Mead (multiple starting points, all converged to
   the same optimum).

## Result

| | Peak-to-peak dispersion, 100-4300Hz |
|---|---|
| Analog filter alone | 29.8us |
| Best possible with **one** all-pass section | 19.2us (barely better — the analog curve is non-monotonic, one section can't match that shape) |
| **Two** sections, `a1=0.194594`, `a2=-0.136698` | **0.85us** |
| (for reference: the Bessel filter redesign, measured on real hardware) | 2.7us |

Mean combined delay after equalization: ~265us (2.65 samples @ 10kHz) — up
from ~66-78us analog-alone, because an all-pass filter can only add delay,
never remove it. The phase/envelope relative-delay line
(`s_relative_delay_samples`) needs to be re-tuned from roughly **+2.65
samples** once this is enabled, a different regime from the Bessel filter's
-0.20 to -0.25 samples.

See `group_delay_equalizer_fit.png` for the delay-vs-frequency plot.

## Status

Numerically fit against simulated (LTspice) data only — **not yet validated
on real hardware**. Implemented as `ssb_allpass1_t` in `ssb_dsp.h`/`.c`
(generic reusable primitive) and instantiated with these coefficients in
`ssb_mic_test.ino`, toggled via serial command `g`. Recommended validation
path: `p` (envelope step test) with `g` off/on on a scope first (cleaner
step edge = working), then real two-tone/spectrum-analyzer testing after
re-tuning `[`/`]`.

## 2026-08-16 refit: SAMPLE_RATE_HZ raised to 16000

Once the AD9851 write path was rewritten to bit-bang (see `AD9851.c`),
`SAMPLE_RATE_HZ` was raised from 10000 to 16000 (see `config.h`'s header
comment). All-pass coefficients don't rescale proportionally with Fs, so
this fit was re-run at Fs=16000 — same source data
(`SallenKey_LP_filter_BC337.txt`), same method (steps 1-3 above,
unchanged), same 100-4300Hz fit grid (that's an absolute-Hz criterion
covering the two-tone fundamentals/beat/IMD product, unrelated to Fs), only
`Fs=16000` substituted into the digital all-pass group-delay formula.
Global optimum confirmed via a coarse (a1,a2) grid scan before polishing
with Nelder-Mead (multiple starts), not just taken on the first
convergence.

| | Peak-to-peak dispersion, 100-4300Hz |
|---|---|
| Analog filter alone (Fs-independent) | 29.8us |
| **Two** sections @ 10000Hz, `a1=0.194594`, `a2=-0.136698` | 0.85us (see above) |
| **Two** sections @ 16000Hz, `a1=-0.023900`, `a2=0.447131` | **14.7us** |
| (exploratory, not wired in) Three sections @ 16000Hz | 10.1us |
| (exploratory, not wired in) Four sections @ 16000Hz | 5.2us |

**Why two sections do noticeably worse at 16000Hz**: a first-order digital
all-pass section's delay-vs-frequency curve is shaped over the section's
full 0-Nyquist range. At 10000Hz the 100-4300Hz fit band spans up to
`4300/5000 = 86%` of Nyquist; at 16000Hz the same absolute Hz band only
spans `4300/8000 = 54%` of Nyquist — the same two sections have
proportionally less curvature available across that narrower fraction of
their own range, so they can't bend to match the analog filter's
non-monotonic peak as tightly. Confirmed not an optimizer-convergence
artifact (grid search found the same optimum the Nelder-Mead polish did);
confirmed it's a real structural limit of the 2-section architecture at
this Fs, not a bug. Adding a third and fourth cascaded section (cheap —
`ssb_allpass1_process` is one multiply-add per sample, and the current
[timing] budget at 16000Hz has margin to spare) recovers most of the
difference if 14.7us turns out to matter once this is validated on real
hardware; not implemented yet since it wasn't asked for.

Mean combined delay after equalization at 16000Hz: ~163.2us (2.611 samples
@ 16000Hz) — down from ~265us (2.65 samples @ 10000Hz) even though both are
"2.6-ish samples", because the coefficients themselves are different, not
because of the sample-rate conversion. `relative_delay_samples` in
`settings.h` was rescaled by the Fs ratio (1.6x) as a first-order
approximation when this change went in, but that rescale doesn't know
about this ~102us shift in the digital filter's own contribution — see
that file's header note for the expected direction of the residual
re-tuning error.

## 2026-09-01 refit: new PNP BC327 + gate-attenuator filter

The RSET analog output stage was redesigned — PNP BC327 driver with a gate
attenuator, replacing the original BC337 NPN buffer. New source data:
`SallenKey_LP_filter_PNP_BC327__RSET_Driver__Gate_Attn.txt`. Same method as
above (steps 1-3, unchanged), same 100-4300Hz fit grid, Fs=16000 only so
far (10000Hz not refit for this filter — not needed, `SAMPLE_RATE_HZ` is
16000 in the current build). Global optimum confirmed via grid scan +
Nelder-Mead polish, same cross-check discipline as the 16000Hz BC337 refit.
Both filters now live side by side in `envelope_gdeq.h`, selected at
compile time by `ENV_FILTER_VARIANT` (`config.h`) — currently set to the
new filter, since that's what's on the bench.

### Analog filter comparison (LTspice, 100-4300Hz)

| | BC337 (old) | PNP BC327 + attn (new) | Delta |
|---|---|---|---|
| Group delay, mean | 68.8us | 93.4us | +24.6us |
| Group delay, peak-to-peak | 29.8us | 41.1us | +11.3us |
| Insertion loss @ 200Hz | -0.84dB | -2.53dB | -1.7dB |
| Insertion loss @ 4300Hz | -6.33dB | -10.42dB | -4.1dB |
| Insertion loss @ 8000Hz | -15.72dB | -22.65dB | -6.9dB |

The new filter is both slower and lossier than the old one — the gate
attenuator is adding delay and loss on top of what the buffer alone cost.
The loss grows with frequency, so it bites hardest exactly where the
equalizer below can't help (see the all-pass caveat further down).

### Digital equalizer fit result

| | Peak-to-peak dispersion, 100-4300Hz |
|---|---|
| Analog filter alone (PNP BC327 + attn) | 41.1us |
| **Two** sections @ 16000Hz, `a1=a2=0.622515` | **13.7us** |
| (for reference: BC337's own 16000Hz fit, above) | 14.7us |

`a1=a2` is a genuine optimum here, not a stuck/degenerate Nelder-Mead
result — a coarse (a1,a2) grid scan independently converges to the same
point (13.69us at the grid resolution, 13.67us after polishing), same
discipline as the BC337 16000Hz refit above. Two IDENTICAL cascaded
sections happen to flatten this filter's particular dispersion shape
better than any opposite-sign pair does — worth remembering as a real
possible outcome of this fit method, not a bug, if it recurs on a future
filter revision.

Mean combined delay after equalization: ~131.6us (2.105 samples @
16000Hz) — LESS than the BC337 filter's 163.2us/2.611 samples, despite
this filter's analog stage being slower to start with, because `a1=a2`
here needs less cumulative added delay to flatten this particular
dispersion shape. `relative_delay_samples` needs re-tuning from roughly
**+2.105 samples** for this filter (a different regime from BOTH the
Bessel filter's -0.20 to -0.25 samples AND the BC337 Sallen-Key fit's
+2.611 samples) — the presets in `settings.h` still reflect the BC337
filter's by-ear/scope tuning and have not been re-tuned for this hardware
change; expect to redo that tuning pass on the bench, not just carry the
old numbers over.

### Open question: does the extra insertion loss hurt IMD3/IMD5?

An all-pass equalizer is unity-magnitude by construction — the digital fit
above flattens DELAY only and does nothing whatsoever about the amplitude
loss in the table above. For the 700/1900Hz two-tone pair specifically,
the IMD3 offsets are 2f1-f2=500Hz and 2f2-f1=3100Hz, and the IMD5 offsets
are 3f1-2f2=1700Hz and 3f2-2f1=4300Hz (the latter being the same 4300Hz
the fit grid's upper bound was originally chosen around).

**Correction, 2026-09-01:** the raw old-vs-new dB gap includes the new
design's own resistive gate attenuator (~2.5dB nominal), which shows up in
the LTspice sim as a flat ~-1.68dB offset already present at 100-300Hz,
well below the filter's own corner. A flat, frequency-independent loss
like that is just headroom/gain to trim back with master gain — it isn't
a filter-shape effect and isn't relevant to edge-rounding/IMD. The number
that actually matters here is the *excess above that flat baseline* —
extra loss minus ~-1.68dB:

| Offset | Product | BC337 | PNP+attn | Raw extra | **Excess (shape only)** |
|---|---|---|---|---|---|
| 500Hz | IMD3 | -0.85dB | -2.56dB | -1.7dB | **-0.03dB (negligible)** |
| 1700Hz | IMD5 | -1.17dB | -3.27dB | -2.1dB | **-0.42dB** |
| 3100Hz | IMD3 | -3.19dB | -6.29dB | -3.1dB | **-1.41dB** |
| 4300Hz | IMD5 | -6.33dB | -10.42dB | -4.1dB | **-2.41dB** |

So the genuine frequency-shape penalty is roughly half what the raw
numbers suggested, and at the IMD3 lower offset (500Hz) it's essentially
zero — the two filters track almost identically there. The concern
doesn't disappear (3100/4300Hz still show 1.4-2.4dB of real extra
roll-off past what the attenuator alone accounts for), but it's smaller
and more concentrated above ~3kHz than first framed, so this leg of the
"why doesn't the equalizer help IMD" argument (see Status below) should
be weighted accordingly — a real but secondary factor, not the dominant
one.

Physical concern, not yet confirmed either way: the same envelope-null
events that drive `freq_dev_hz` toward the `MAX_FREQ_DEV_HZ` (8000Hz)
clamp (see `null_bias_investigation.md`) also produce the envelope's own
fastest, most broadband transient — content that plausibly extends well up
into the range where this filter now attenuates harder. Flattening delay
doesn't stop that transient from being rounded off by the extra loss up
there, and edge-rounding at a null is exactly the kind of envelope-domain
distortion that shows up as IMD. Whether this matters in practice hasn't
been measured — the direct check is a real two-tone IMD3/IMD5
spectrum-analyzer comparison, BC337 vs. PNP+attn, each with its OWN
properly-retuned `relative_delay_samples`, using the project's existing
`imd_comparison_spectrum.png`/`imd_comparison_table.csv` workflow. Not run
yet — parked here alongside the null-bias and `I`-regression items as
something to chase once the group-delay retune itself is validated on
the bench.

### Simulated (mid-DC) vs. measured hardware (mid-DC) — sanity check

Comparing this LTspice run's phase shape (zeroed at 200Hz, since the sim's
reference node isn't the same point as the full measured chain — offset/
scale mapping, PWM, etc. aren't in the sim) against the real `EnvFilterTF_
mid_DC.txt` hardware sweep taken with the sine-chirp test mode: the two
track reasonably well but aren't identical. The sim over-predicts phase
lag by up to ~11° around 2-4kHz, closing to within a few degrees by
6-8kHz — a decent guide, not an exact match, consistent with real
component tolerances the sim doesn't model. Only the mid-DC bias point has
been simulated so far; the DC-operating-point-dependence itself (measured
on real hardware — see below) hasn't been independently checked against
LTspice at the Lo/Hi bias points yet.

**Explained artifact, 2026-09-01: small blips at 5kHz and its harmonics in
the raw chirp-based TF sweeps (`EnvFilterTF*.txt`) are a measurement-rig
artifact, not real filter behavior.** Traced to short (0.25-7µs), ~5kHz
negative-going glitches on `CHIRP_REF_GPIO` (pin13) - confirmed present in
EVERY audio mode (`t`/`m`, not chirp-specific), at a rate matching
`adc_capture.h`'s `ADC_CONT_SAMPLE_FREQ_HZ`/`ADC_CONT_FRAME_SAMPLES` =
80000/16 = 5000Hz exactly - i.e. `adc_conv_done_cb()`'s own DMA/ISR firing
rate, most likely coupling electrically onto that specific pin rather than
a deliberate GPIO write (both other users of the same physical pin are
compiled out - `ADC_ISR_DEBUG_PIN_ENABLED`/`CMD_DEBUG_PIN_ENABLED` are both
0). **Confirmed NOT present on the actual RSET/PWM signal line (GPIO2)** -
scoped directly, nothing detected - so this only affects the reference/sync
channel the external TF rig reads, never the signal actually being
measured, and none of the group-delay/DC-dependence conclusions elsewhere
in this document are affected by it. Should mostly be filtered out easily
(a 0.25-7µs blip is tiny against most of the sweep's own edge-to-edge
interval - e.g. ~1ms at 1kHz), possibly interacting with the reference's
own high-frequency sampling-jitter limitation (see the chirp mode's
`ssb_mic_test_commands.md` entry) right at the top of the sweep, where the
true reference period itself approaches 200µs. Not chased further - the
actual TF data was fine, this only explains a previously-unexplained
cosmetic wrinkle in it.

### Status

**2026-09-01, real-hardware result: far from decisive — leans toward "off."**
`p` step test with `g` toggled: a slight steepening of the edge as
predicted, but the lead-in is slower and the lead-out shows noticeable
ripple. Two-tone and mic/white-noise IMD testing: with `[`/`]` re-tuned by
ear in both states, comp-off gives similar or slightly *better* IMD than
comp-on. This directly contradicts the narrowband delay-flatness
prediction above, so it's worth being explicit about why, not just
recording the result:

**Quantified, same day — but confounded with `'I'`.** Two logged `Live`
presets, delay re-tuned in each to minimize 3rd-order IMD: `relative_delay=
1.00, env_gdeq_enable=false, envelope_interp_enable=false` gave Low 3rd IMD
-41.41dB / High 3rd IMD -34.17dB; `relative_delay=3.00, env_gdeq_enable=
true, envelope_interp_enable=true` gave -34.50dB / -29.97dB — a real
4.2-6.9dB degradation, reference levels matched to within 0.5dB so it isn't
a normalization artifact. **But `envelope_interp_enable` flipped ON
together with `env_gdeq_enable` between these two runs** (see the
Catmull-Rom/null-flattening cross-reference two sections up), so this pair
does not isolate the equalizer's own effect — it measures "gdeq+`I`
together" vs "neither." The +2.0 sample delay swing between the two runs
is a good sanity check (matches the fitted equalizer's own +2.105 sample
mean delay closely), but doesn't help separate the two enabled features.
**Still needed: `env_gdeq_enable=true` with `envelope_interp_enable=false`
(delay re-tuned) vs. the Run-1 baseline, and the reverse
(`env_gdeq_enable=false`, `envelope_interp_enable=true`)**, to find out
whether the ~4-7dB hit is coming from the equalizer, from `'I'`'s
already-known null-rounding issue, or both. Until that decomposition is
run, don't treat this pair as a decisive indictment of the equalizer
specifically.

**Decomposition, same day — first isolating run in.**
`env_gdeq_enable=true`, `envelope_interp_enable=false`, delay re-tuned for
min 3rd IMD, landed at **relative_delay=1.10**: Low 3rd IMD -30.90dB, High
3rd IMD -27.17dB (ref levels within 0.6dB of the other two runs, not a
normalization effect). Full three-way table:

| | Off (delay=1.00) | gdeq+`I` (delay=3.00) | gdeq alone (delay=1.10) |
|---|---|---|---|
| Low 3rd IMD | -41.41dB | -34.50dB | -30.90dB |
| High 3rd IMD | -34.17dB | -29.97dB | -27.17dB |

gdeq alone is the WORST of the three, not an intermediate case — 10.5dB
worse than off on Low, 7.0dB worse on High, worse even than gdeq+`I`
together. So `'I'` is not masking gdeq's damage; gdeq itself is the bigger
single contributor of the two.

**But flag before treating this as final:** the tuned delay (1.10) sits
only +0.10 samples above the off-state optimum (1.00), nowhere near the
fitted equalizer's own predicted +2.105 sample mean delay addition that
`envelope_gdeq.h`'s "IMPORTANT SIDE EFFECT" note documents as the correct
operating point with gdeq on. The gdeq+`I` run's own tuned delay (3.00)
landed much closer to that predicted region. A search converging at +0.10
instead of near +2.1 looks like it found a shallow local minimum close to
the off-state value rather than the theoretically-predicted one — possibly
because the search didn't sweep far enough out. **Before trusting -30.90/
-27.17dB as gdeq's true best case, re-run the `[`/`]` search for gdeq-alone
specifically probing delay ≈ 1.8-2.4 samples** (centered on +2.105), not
just refining near 1.10. If that region turns out worse still, this is a
genuinely decisive result against the equalizer for this filter; if it's
meaningfully better, the -30.90/-27.17dB numbers above were an artifact of
an incomplete delay search, not the equalizer's real ceiling.

**Follow-up, same day: delay=1.40 tuned for min Low 3rd IMD specifically.**
Low improved to -33.34dB (+2.44dB vs. the 1.10 run), but High barely moved
(-27.40dB, 0.24dB worse - within noise). Both still well short of the
off-state baseline (-41.41/-34.17dB). Two takeaways: (1) Low kept
improving as delay increased from 1.10->1.40, consistent with the search
not having reached far enough yet - worth continuing toward the fitted
+2.105 region rather than stopping here; (2) High did NOT track Low's
improvement, which raises the separate possibility that Low and High 3rd
IMD don't share one jointly-optimal delay for this filter+equalizer
combination - a genuine low/high delay tradeoff, not just an unfinished
search. Continuing the sweep to ~1.8-2.4 while watching BOTH sidebands
together (not just Low) should distinguish the two: both improving further
= search wasn't finished; Low improving while High keeps drifting worse =
a real tradeoff.

**Follow-up, same day: delay=2.00 tuned for min High 3rd IMD specifically.**
High improved only modestly to -28.18dB (+0.78dB vs. the 1.40 run) - the
best High result yet, and notably this delay (2.00) sits almost exactly at
the fitted equalizer's own predicted +2.105 sample operating point. But
Low did NOT keep improving out to this delay - it got substantially WORSE,
-33.34dB (at 1.40) -> -27.36dB (at 2.00), a 6.0dB drop. Full gdeq-alone
sweep now:

| Delay | Low 3rd IMD | High 3rd IMD |
|---|---|---|
| 1.10 | -30.90dB | -27.17dB |
| 1.40 | -33.34dB (best Low so far) | -27.40dB |
| 2.00 | -27.36dB | -28.18dB (best High so far) |

This resolves the "unfinished search" question for Low - it has a real
interior minimum near delay~1.4 within the range tested, not a monotonic
trend still climbing toward +2.105. The low/high tradeoff from the
previous entry is now well supported: Low's best point (~1.4) and High's
best point so far (~2.0, right at the theoretical prediction) are
different operating points, and **neither sideband's individually-best
case gets remotely close to the off-state baseline** (-41.41/-34.17dB) -
Low's best is still 8.1dB worse, High's best is still 6.0dB worse, picked
independently and generously (not even simultaneously achievable at one
delay). Significant: 2.00 is essentially the theoretically "correct" delay
for this filter+equalizer and it still doesn't recover anywhere near
baseline on either sideband - this is no longer well-explained by "hadn't
reached the right delay," since the right delay (by the fit) has now been
tested directly.

**Follow-up, same day: sweep closed out.** Confirmed on the bench: past
delay=2.00, Low 3rd, High 3rd, AND the higher-order products all get
WORSE, not just one sideband trading against another. This resolves the
last open thread above - there's no point further out worth chasing, the
gdeq-alone delay sweep is now bracketed on both sides of a genuine
interior optimum region (~1.4-2.0 samples), and even the best achievable
point within that region for each sideband individually stays 6-8dB worse
than off. **This is no longer explainable as an unfinished or mistuned
delay search - the fit's own predicted +2.105 sample operating point has
been tested directly (delay=2.00) and does not recover anywhere near
baseline performance.**

Re: the gdeq+`I` run (delay=3.00) beating gdeq-alone's best on both
sidebands (-34.50/-29.97 vs -33.34/-28.18) - since pushing gdeq-alone's
own delay toward 3.00 makes everything worse (confirmed above), that
combined run's better numbers can't be "more delay helping." `'I'` must be
doing something genuinely independent of gdeq there - plausibly not the
purely-harmful factor it was assumed to be for 3rd-order IMD specifically,
even though it's still a documented, real problem for OTHER symptoms (the
`'p'` step-response ripple, the audibly-worse two-tone stability in
`null_bias_investigation.md`). Those are different failure modes and don't
have to move together - worth keeping `'I'`'s effect on 3rd-order IMD as
its own separate open question rather than assuming it's uniformly bad.

**Working conclusion, real-hardware-confirmed:** gdeq's IMD3 degradation on
this filter is a genuine property of the equalizer itself, not a
delay-tuning artifact - bracketed on both sides of its own predicted
operating point and consistently 6-8dB worse than leaving it off, even at
each sideband's individually-best delay. Combined with the three
mechanisms discussed earlier (fit-window/wideband mismatch, the
DC-operating-point dependence, and the smaller-than-first-estimated but
real amplitude penalty above ~3kHz), the most likely explanation is that
group-delay-only correction simply isn't addressing this filter's dominant
real-world distortion mechanism - **recommendation stands and is now
stronger: leave `env_gdeq_enable` off by default for
`ENV_FILTER_PNP_BC327_ATTN`**, and don't invest further bench time
re-tuning its delay for this filter revision.

**Real (mic-path) audio, same day: gdeq's effect all but vanishes into a
much bigger existing problem.** All the numbers above came from
`AUDIO_SRC_TWOTONE` (digitally-generated, clean two-tone injected straight
into the DSP chain, EQ/compressor bypassed). Repeating with `AUDIO_SRC_MIC`
(real ADC front end, `eq_enable=true`, `compressor_enable=true`, ADC LPF
in Chebyshev mode - i.e. the actual on-air signal path) gives a very
different picture:

| Run (mic path) | Low 3rd IMD | High 3rd IMD |
|---|---|---|
| Off (delay=0.98) | -30.16dB | -10.25dB |
| gdeq alone (delay=1.48) | -29.27dB (-0.89dB) | -10.43dB (-0.18dB) |
| gdeq+`I` (delay=2.88) | -29.56dB (-0.53dB) | -10.58dB (-0.33dB) |
| Off, repeat (delay=0.93) | -30.02dB | -10.24dB |

Repeatability is excellent (the two off-state runs agree to 0.14dB/0.01dB),
so this is a clean measurement, and against that noise floor gdeq's real
effect here is small - still consistently in the "worse" direction on Low
(~0.5-0.9dB) as in every other test, but close to a rounding error on High
(~0.2-0.3dB), nothing like the 6-10dB hit on the synthetic two-tone test.

Far more significant: the ABSOLUTE levels. High 3rd IMD sits at only
**-10.2 to -10.6dB across every mic-path run, gdeq on or off** - 24dB worse
than the -34.17dB synthetic-two-tone-off baseline; Low 3rd is ~11dB worse
too (-30dB vs -41dB). Since this preset adds `eq_enable`/`compressor_
enable` (both off in every prior two-tone run) and switches the ADC LPF to
Chebyshev, something in that real chain - the compressor is the leading
suspect (a classic dominant IMD3 source), possibly compounded by the EQ or
the Chebyshev ADC filter - is producing an IMD3 floor far worse than
anything the envelope filter or its equalizer contributes. -10dB IMD3 is
a genuinely poor number for SSB (normal targets are -30dB or better).

**Net effect on the gdeq decision: unchanged (still leave it off - it
never helped, costs nothing to disable), but it's now clearly not where
the real audio-quality problem lives.** The compressor/EQ/ADC-chain
IMD3 floor is a much bigger, separate issue worth chasing next - start by
toggling `eq_enable`/`compressor_enable`/ADC LPF mode independently on
`AUDIO_SRC_MIC` (same delay-tuned-per-config approach used throughout this
sweep) to find which stage is actually responsible for the ~24dB gap.

**Follow-up, same day: compressor and EQ both ruled out as the dominant
cause.** `delay=0.98`, `gdeq`/`I` off throughout, ref levels matched to
within ~1.3dB across all three runs (not a level-mismatch artifact):

| Run | Low 3rd IMD | High 3rd IMD |
|---|---|---|
| Comp ON, EQ ON (17.3dB gain, baseline avg) | -30.09dB | -10.25dB |
| Comp OFF, EQ ON (30.3dB gain) | -31.20dB (1.1dB better) | -10.83dB (0.6dB better) |
| Comp OFF, EQ OFF (30.3dB gain) | -31.79dB (1.7dB better than baseline) | -9.88dB (0.4dB WORSE than baseline) |

The compressor accounts for ~0.6-1.1dB - real, but tiny next to the ~24dB
gap. EQ's effect is mixed and small: helps Low a little further but makes
High slightly worse when removed (mildly protective there, if anything -
the opposite of "EQ is a distortion source"). Both now ruled out as the
dominant cause; High 3rd IMD stays pinned near -10dB regardless.

**Leading remaining hypothesis: the injected two-tone stimulus itself**,
upstream of everything this firmware controls (external generator/
soundcard/cabling/mic preamp feeding the mic input) - if that source
already carries ~-10 to -12dB IMD3 of its own, no internal DSP option can
move a floor set before the ADC ever sees it. Direct check: spectrum-
analyze the injected two-tone signal itself, or loop it back ahead of the
mic preamp, before chasing more internal toggles. Remaining untested
internal variable: `ADC_LPF_MODE_OFF` on `AUDIO_SRC_MIC` (every mic-path
run so far has been Chebyshev) - worth one data point, but a weaker bet
than the source-purity hypothesis given how little compressor/EQ moved
things.

- **The fit window doesn't cover what these tests excite.** The 100-4300Hz
  grid was chosen for two-tone/IMD spectral relevance, but a step edge and
  white noise both carry energy well outside it. Outside the fit window
  the combined (analog + digital) delay curve was never constrained and
  can diverge — plausibly exactly the lead-out ripple being seen. The
  digital side's contribution is well-behaved on its own (a real,
  non-oscillatory pole pair, `a1=a2=0.622515`, confirmed as a genuine
  optimum, not degenerate), so this points at analog/digital delay
  *mismatch above ~4.3kHz* rather than the digital fit misbehaving by
  itself.
- **An all-pass equalizer cannot touch the amplitude problem, and part of
  the amplitude problem is real (though smaller than first estimated).**
  Correcting for the new design's own ~2.5dB gate attenuator (a flat
  offset, not a shape effect — see the table above), the genuine
  frequency-dependent excess loss at the IMD3/IMD5 offsets is more like
  ~0dB at 500Hz growing to ~1.4-2.4dB at 3100/4300Hz, not the 1.7-4.1dB
  raw gap quoted earlier. Rounding/attenuating envelope content above
  ~3kHz is still a plausible secondary distortion mechanism a delay-only
  fix can't touch, but it's no longer the strongest leg of this
  explanation — the delay-dispersion-outside-fit-window and DC-bias-point
  mismatch factors below are doing more of the work.
- **The fit is only exact at one DC bias point.** The equalizer's
  coefficients were fit against the mid-DC analog TF; the DC-dependence
  finding below shows the real filter's phase (and presumably amplitude)
  shifts by 7-14% in crossing frequency across the duty range that real
  audio actually sweeps through. A fixed narrowband fit at one bias point
  can't track that, whereas by-ear `[`/`]` tuning implicitly compromises
  across the whole swept range — which is a plausible reason hand-tuned
  "off" can match or beat the "theoretically correct" fixed comp.
- **Possible confound, not yet isolated: `envelope_interp.h`'s Catmull-Rom
  stage (`'I'`) rounds off the exact same null transient, upstream of the
  analog filter.** Its own v4.2 validation note already found a smooth
  cubic can't represent a two-tone null's hard fold (a genuine derivative
  discontinuity — `2A|cos(...)|` looks locally like a V there) and rounds
  it instead, unlike the plain straight-line ramp it replaced, which
  reproduces a V-shaped fold naturally. `envelope_interp_on_full_tick()`
  is the last digital stage before the PWM write, i.e. immediately
  upstream of this filter's own extra HF roll-off — so if `'I'` was
  enabled during the two-tone/mic comp-on/off comparison above, the null
  transient may already have been smoothed digitally before the analog
  filter or the equalizer ever saw it, which would mask/confound the
  filter-only question this Status section is trying to answer. `'I'` is
  already separately parked in `null_bias_investigation.md` (2026-08-31)
  as an unresolved, user-confirmed "audibly worse" regression — not
  something to treat as a known-neutral background setting. Not yet
  confirmed whether `'I'` was on or off during the tests behind this
  Status entry. Recommended: repeat the comp-on/comp-off two-tone/mic
  comparison with `'I'` forced off, to get a clean read on the filter/gdeq
  question in isolation from this still-open interpolation issue.

None of these four are mutually exclusive; the honest read is that several
are plausibly working against the equalizer at once for this filter, and
the delay-flatness gain it does deliver in the fit band isn't large enough
to outweigh them. **Current recommendation: leave `env_gdeq_enable` off by
default for `ENV_FILTER_PNP_BC327_ATTN`** (already true — off is the
compiled-in default in `envelope_gdeq.cpp`, unaffected by this finding)
and don't spend further effort re-fitting the all-pass sections for this
filter. If this filter's IMD is to be improved further, the insertion-loss
table above says the amplitude penalty is the more promising thing to
chase — either in the analog design itself (less loss in the 3-4.3kHz
region) or with an actual IMD3/IMD5 spectrum-analyzer comparison (comp on
vs. off, each re-tuned) to replace this by-ear read with numbers, rather
than more delay-equalizer iteration. The coefficients and
`ENV_FILTER_VARIANT` plumbing stay in the firmware since they're correct
for what they do (narrowband delay flattening) and cost nothing while
disabled — this is a "don't reach for this tool on this filter" finding,
not a "the fit was wrong" finding.

## 2026-09-03 refit: real-hardware TFA data (Hi-Z buffered), fit band widened to 8000Hz

Requested after two developments: (1) the TFA front end had a loading issue
that was making measured phase inconsistent across the envelope's DC
operating range — this is very plausibly the same effect the "DC-operating-
point dependence" section below documents from LTspice-vs-hardware
comparisons, or at least a contributor to it. Hi-Z buffers added to the TFA
front end fixed it — phase now reads consistently across the envelope
range. (2) Real `freq_dev_hz` excursions have been observed out to the
`MAX_FREQ_DEV_HZ` clamp (8000Hz), well past the old 100-4300Hz fit grid's
upper edge — the fit had never been asked to behave out there.

Source data: `Group_delay_off__on_F_Phase.txt` (the same real-hardware TFA
sine-chirp sweep already used for the 2026-09-03 same-day dispersion
report in `ssb_mic_test_commands.md` — see the correction paragraph below).
~1000 points, 0–24kHz, unwrapped phase, `g` off and `g` on both captured
(the `g` on curve reflects the OLD/superseded 0.622515/0.622515
coefficients, still compiled in at measurement time).

### Method

Same structure as every prior fit in this document (two cascaded
`ssb_allpass1_t` sections, peak-to-peak minimization via grid search +
Nelder-Mead polish, multiple starts) — only the source data and fit band
changed:

1. Parsed the raw TFA sweep's `g`-off phase column.
2. Extracted group delay via `tau(f) = -(1/360) dPhase/dFreq`, Savitzky-
   Golay smoothed before differentiating (same technique the earlier
   same-day dispersion report used — but see the correction below on
   smoothing-window choice).
3. **Smoothing-window sensitivity check (new this round).** The real TFA
   sweep is far denser (~1000 points/24kHz) and noisier point-to-point
   than the LTspice sweeps every earlier fit in this document used. A
   light window (31 points, ~700Hz span — matching this project's other
   TFA chart work) leaves the extracted group-delay curve with tens-of-us
   swings above ~2kHz that don't shrink monotonically the way a real
   2-pole-plus-buffer-stage filter's phase should. Computed the residual
   (raw phase minus smoothed) and its RMS (0.5–2.6° depending on band,
   worst near 3–4.3kHz) — enough, once differentiated, to plausibly
   produce swings that size from noise alone, not real filter structure.
   Re-ran the fit's own analog-curve peak-to-peak across a range of
   smoothing spans (61/91/121/151/181/221/261 points ≈ 1.4–6.1kHz span):
   it stabilizes to within ~2us of its converged value from a 181-point
   (~4.2kHz) span onward. Used 181 points as the fit target — wide enough
   to be clear of the noise floor, not so wide it would smooth away a
   genuine single in-band hump if the real filter has one.
4. Fit grid: 100–8000Hz (dense, 800 points), replacing the old
   100–4300Hz grid. `Fs=16000Hz` throughout (no other Fs refit this
   round).

### Result

| | Peak-to-peak dispersion, 100–8000Hz | Mean delay |
|---|---|---|
| Analog filter alone (real hardware, Hi-Z-buffered TFA) | 71.2us | 71.2us |
| **Two** sections, `a1=0.026173`, `a2=0.236810` | **18.5us** (~3.9x) | 196.5us |

Mean added delay: ~125.4us (**2.006 samples @ 16000Hz**) — close to, and
slightly less than, the superseded LTspice fit's 2.105 samples.
`relative_delay_samples` needs re-tuning to roughly this new starting
point if/when `g` is re-enabled for testing, same as every prior
coefficient change in this file.

Checked over the OLD 100–4300Hz sub-band with the NEW coefficients: 18.5us
p-p — identical to the full-band figure (the equalized curve's worst-case
points both happen to fall inside the old band already), so this refit is
a strict widening of validated coverage, not a regression within the
previously-fitted range.

### Old coefficients applied to the new real-hardware analog data (context)

Useful sanity check on why "just extend the old fit's claimed range"
wouldn't have worked: applying the OLD (2026-09-01, LTspice-fit)
`a1=a2=0.622515` to this same real analog curve —

| Band | Analog alone (measured) | With OLD coefficients |
|---|---|---|
| 100–4300Hz (old fit's own band) | 36.8us p-p | 18.5us p-p (tracks the LTspice prediction of 41.1→13.7us reasonably well) |
| 100–8000Hz (new band) | 71.2us p-p | **449.2us p-p** |

The old coefficients still behave inside the band they were actually fit
for. Outside it, they diverge badly — a first-order all-pass section's
delay curve keeps changing all the way to Nyquist, and `a=0.622515` is
steep enough that the un-fit region past 4300Hz was always going to blow
up once anyone looked. This is the concrete argument for why extending
frequency coverage needed a real re-fit, not a documentation change.

### Correction to the same-day (2026-09-03) real-hardware dispersion report

The dispersion numbers first delivered this session (146.1us→147.2us p-p
over 100–4300Hz, "essentially no improvement from `g`", written up in
`ssb_mic_test_commands.md`'s earlier 2026-09-03 entry and the accompanying
chart) used the lighter 31-point/~700Hz smoothing window, the same one
this project's other TFA analyses have used against much sparser LTspice
data. Applied to this denser, noisier real-hardware sweep, that window
was too fine — re-running the SAME `g`-off/`g`-on measured curves (not a
model, the actual measured phase in both states) at the 181-point window
justified above gives a materially different picture:

| Band | `g` off (measured) | `g` on (measured, OLD coeffs) | |
|---|---|---|---|
| 100–4300Hz, 31-pt window (original report) | 146.1us p-p | 147.2us p-p | "no improvement" |
| 100–4300Hz, 181-pt window (this correction) | 36.6us p-p | 14.3us p-p | **real ~61% reduction, close to the 41.1→13.7us LTspice prediction** |

So the equalizer's OLD coefficients were doing closer to what they were
designed to do within their own fit band all along — the original "no
improvement" finding was substantially a smoothing-window artifact on
noisy real data, not a genuine equalizer failure at 100–4300Hz. This does
**not** overturn the separate, independently-confirmed 2026-09-01
real-hardware IMD finding (that document's own repeatable, delay-swept
3rd-order IMD measurements never depended on this phase-noise/smoothing
question) — it only revises the group-delay/TFA side of the picture. Left
the original entry in `ssb_mic_test_commands.md` in place with this
correction appended rather than edited away, per this project's usual
convention. Going forward, TFA group-delay analyses on real (not LTspice)
hardware sweeps should default to a wider smoothing window and a
convergence check like the one in the Method section above, not the
31-point window carried over from the LTspice-fitting era.

### Status

Numerically fit, not yet IMD-validated on real hardware (see the
group-delay validation immediately below, which is a different, narrower
claim than IMD validation). This refit directly addresses two of the four
suspected causes behind the 2026-09-01 negative IMD result (the narrow
100-4300Hz fit window, and — pending confirmation — the DC-bias-point
measurement inconsistency the Hi-Z buffers were added to fix); it does
nothing about the other two (the unity-magnitude all-pass structure still
can't correct this filter's real insertion-loss penalty; the
`envelope_interp`/`'I'` confound). **`g` stays off by default** until the
same real-hardware IMD3/IMD5 delay-sweep procedure documented above is
re-run against these new coefficients — a better-fitted equalizer is not
a re-validated one.

### 2026-09-03, same day: group-delay side confirmed on real hardware

New coefficients (`a1=0.026173`, `a2=0.236810`) flashed to the board and
re-measured with the TFA — a genuine third data point, not the same sweep
re-analyzed. Source: `Group delay off  on F Phase Refit on TF meas.txt`.

**Column-order note:** this file's two phase columns come out swapped
relative to the earlier file's off-then-on convention — its first column
matches the earlier *predicted* equalized curve almost exactly, and its
second matches the earlier *measured* analog-only curve, not the labels
the column order would otherwise suggest. Caught by a physical sanity
check: interpreting the columns at face value gives a NEGATIVE mean added
delay, which an all-pass network cannot produce (it can only add delay).
Swapping the interpretation fixes that and lines up near-exactly with the
independently-derived prediction — strong enough agreement that this
isn't ambiguous, but worth checking the TFA channel routing before the
next sweep so this doesn't have to be re-diagnosed from the data every
time.

| | Peak-to-peak, 100-8000Hz | Mean delay |
|---|---|---|
| Analog alone (this sweep) | 71.3us | 73.3us |
| Analog alone (previous sweep, for comparison) | 71.2us | 71.2us |
| **g on, new coefficients (measured)** | **20.3us** | 198.4us |
| g on, new coefficients (predicted, from the fit above) | 18.5us | 196.5us |

Mean added delay, measured: 125.2us (**2.003 samples @ 16000Hz**) — within
1us of the fit's own 125.4us/2.006-sample prediction. The measured "on"
curve tracks the predicted curve to within ~2.1us RMS (4.4us worst-case)
across the whole 100-8000Hz band. The two independent analog-alone
measurements (this sweep vs. the previous one) agree to within ~2.3us
RMS too — good evidence the Hi-Z buffer fix is giving repeatable
analog-filter measurements session to session, not just within one
sweep.

**What this does and doesn't confirm:** the group-delay/dispersion
prediction from the refit above is now validated directly on the bench,
not just modeled — both the shape (RMS agreement with the predicted
curve) and the headline numbers (added delay within 1us, p-p within
~2us) check out. This is NOT the same as IMD validation — see the Status
note above and `envelope_gdeq.h`'s header comment: the 2026-09-01 finding
that the OLD coefficients hurt 3rd-order IMD involved mechanisms (real
insertion loss, the possible `'I'` confound) that a group-delay TFA sweep
can't see either way. **`g` stays off by default** until the IMD
delay-sweep is re-run against these coefficients specifically.

## 2026-09-03, later same day: IMD delay-sweep with the NEW coefficients — real improvement, at a delay never tested before

**New real-hardware finding, user-reported:** with the refit coefficients
(`a1=0.026173`, `a2=0.236810`) on the bench, the optimum `relative_delay`
now sits at roughly **4 samples** — and unlike the old-coefficient sweep,
this optimum is consistent between two-tone and mic-noise stimuli (the
2026-09-01 sweep never achieved that consistency). Other delay values
still null specific higher-frequency products better, but the spread
across delay settings is much narrower than it was with the old
coefficients. Separately: **with `g` on at two-tone, the higher-order IMD
products are now significantly reduced** — a real, user-observed
improvement, not predicted or claimed by anything in this document before
now.

**This does not contradict the 2026-09-01 "closed-out" sweep.** That
sweep (a1=a2=0.622515) tested delay 1.10→1.40→2.00 and confirmed
everything got monotonically worse past 2.00 — but that result is
specific to THOSE coefficients. The new coefficients are a materially
different digital filter (much milder: 0.026/0.237 vs. 0.622/0.622), with
its own delay-vs-IMD landscape. A ~4-sample optimum for this filter is a
new, independent result, not a re-test of previously-explored territory —
the old sweep's "past 2.00 always worse" finding was never in a position
to rule this out, because it was never testing this filter.

### Is this a magnitude (insertion-loss) compensation effect? No — mechanism ruled out by construction

User's hypothesis, worth taking seriously: could the IMD improvement be
`g` incidentally compensating for the analog filter's own gain roll-off,
rather than (or in addition to) flattening group delay? **Ruled out
analytically, not just empirically**: `ssb_allpass1_t` implements
`H(z) = (a + z⁻¹)/(1 + a·z⁻¹)`, and for any real `|a| < 1` this has
`|H(e^jω)| = 1` at every frequency — a true all-pass network, unity
magnitude BY CONSTRUCTION, independent of the coefficient value. Two
cascaded sections are still unity magnitude (product of two unity-
magnitude responses). There is no mechanism by which `envelope_gdeq_process()`
can be altering the envelope's amplitude spectrum, only its phase — so it
cannot be responsible for compensating the insertion-loss shape measured
below, on this filter or any other. (A real DSP implementation has
floating-point rounding, but that's a many-orders-of-magnitude-too-small
effect to explain an audible/measurable IMD change — not a real
candidate mechanism.)

**Working explanation instead:** the ~4-sample delay region simply wasn't
part of the 2026-09-01 sweep (which used a different, steeper filter and
stopped exploring past 2.00 samples once every metric was getting worse
there). The new, milder coefficients' own optimum landscape is different,
and this real-hardware result says the group-delay flattening this
equalizer does IS paying off for IMD now that the right operating delay
has been found for it — consistent with, not contradicting, the original
design intent. Left as a working explanation rather than a closed case:
the mechanism is plausible and the magnitude explanation is ruled out,
but a full re-run of the old delay-sweep/IMD-table methodology (this time
around the ~4-sample region, both two-tone and mic-noise, both sidebands)
would turn this from "user observed it and here's why it's probably real"
into the same level of confirmed-and-quantified result the earlier
(negative) finding had. Not yet done — recommended next step whenever
there's bench time, since it would let `g`'s default flip from off to on
with actual evidence behind it, not just a promising anecdote.

### Real-hardware insertion-loss measurement (amplitude+phase TF, `g` off)

Separate from the IMD question but raised in the same message: the
analog filter's gain roll-off itself, now measured directly (not just
LTspice-predicted). Source: `TF meas.txt` — same sweep as the very first
group-delay TFA file (phase columns match point-for-point), now exported
with its amplitude channel included. Amplitude column is a linear ratio;
converted to dB and referenced to the 100–300Hz passband average (a flat
gain offset there reads as 0dB, so this table is already "excess loss
above the passband," no separate attenuator-baseline correction needed
the way the LTspice comparison in the 2026-09-01 section required).
Light smoothing (31-point Savitzky-Golay — residual RMS 0.05–0.48dB
depending on band, much cleaner than the phase channel needed):

| Frequency | Role | Measured loss (real hardware) | LTspice prediction (2026-09-01, PNP+attn) |
|---|---|---|---|
| 700Hz | two-tone f1 | -0.45dB | — |
| 1900Hz | two-tone f2 | -1.85dB | — |
| 500Hz | IMD3 lower offset | -0.28dB | ~-2.56dB raw / -0.03dB excess-only |
| 1700Hz | IMD5 lower offset | -1.55dB | ~-3.27dB raw / -0.42dB excess-only |
| 3100Hz | IMD3 upper offset | -4.52dB | ~-6.29dB raw / -1.41dB excess-only |
| 4300Hz | IMD5 upper offset, old fit-band edge | -8.75dB | ~-10.42dB raw / -2.41dB excess-only |
| 8000Hz | `MAX_FREQ_DEV_HZ` limit | **-21.70dB** | ~-22.65dB raw |

Real hardware and the LTspice sim agree well at the high end (8000Hz:
-21.7 measured vs. -22.65 predicted) and are in the same ballpark
through the middle of the band, with real hardware showing somewhat less
loss at the low-to-mid frequencies than the sim's raw (non-excess-
corrected) numbers — consistent with the sim's raw figures including the
gate attenuator's flat offset, which this measurement's passband-relative
referencing already excludes by construction. Bottom line: the roll-off
is real, confirmed on the bench, and substantial — nearly 22dB down by
8000Hz, the same frequency the null-transient content and `freq_dev`
excursions live at.

**Not yet compensated.** A magnitude correction would need an actual
gain-shaping filter (shelf/peaking IIR, or similar) — `ssb_allpass1_t`
structurally cannot do this (see above). A full inverse response would
need up to ~+22dB of boost at 8000Hz relative to the passband, which
raises real questions before building it: how much of that boost is
actually recoverable headroom vs. how much just raises the noise floor
and quantization/PWM-resolution noise at exactly the frequencies where
`freq_dev` already pushes hardest, and whether a full inverse or a
capped/partial shelf is the better real-world tradeoff. Parked here as
the next concrete step — see the accompanying conversation for the
scoping question before implementation starts.

## Related: DC-operating-point dependence (envelope filter TF, not group delay)

Separate from the group-delay/equalizer work above: real hardware testing
with the sine-chirp test mode (`w`, see `ssb_mic_test_commands.md`) found
that this analog filter's transfer function itself shifts with the
envelope's DC operating point (duty-range offset/scale, not master gain,
which only scales swing around a fixed mean) — the -90° phase-crossing
frequency moved from 2626Hz (low DC) to 2441Hz (high DC), a -7.1% shift,
growing to -14% by the -180° crossing (6899Hz -> 5930Hz). Confirmed as a
real, monotonic, hardware-measured effect, not noise. Not yet incorporated
into the equalizer fit above (which assumes one fixed analog TF) — the
group-delay equalizer's own coefficients would, in principle, need to vary
with DC operating point too if this turns out to matter enough to chase
further. Parked here since it's the same underlying analog filter this
whole document is about, even though it surfaced from the TF measurement
work rather than the group-delay refit itself.

## 2026-09-03, later same day: envelope magnitude equalizer built (`envelope_ampeq.h`/`.cpp`, key `a`) — NOT YET VALIDATED

Follow-up to the insertion-loss measurement section above. Discussed the
scoping question directly: a full inverse response needs up to ~+22dB of
boost at 8000Hz, which risks trading a well-characterized amplitude
problem for an unknown noise-floor/PWM-quantization/headroom problem —
agreed to start with something more modest at the high end and measure
before going further.

**Why a shelf, not the same shape as the existing presence EQ.** Before
picking a filter shape, checked whether the existing pre-Hilbert
audio-path presence EQ (`biquad_set_peaking`, `ssb_dsp.c`, an RBJ
peaking/bell biquad — currently fc=2200Hz, Q=1.0, +4dB, the "4dB presence
increase" setting) could do this job. Computed its response directly
(not assumed): it peaks at +4.00dB at 2200Hz, crosses half-gain (+2dB) at
roughly 1412Hz and 3264Hz, and is back to +0.00dB by 8000Hz. A peaking
filter is symmetric and returns to unity gain on both sides of its
center by construction — it cannot address a loss that keeps climbing
monotonically out to 8000Hz. The measured insertion loss (previous
section) is monotonic, not a dip, so the right shape is a shelf: constant
plateau gain above its corner, not a bump that decays away again.

**The new primitive.** Added a public `ssb_shelf_biquad_t` (Direct-Form-I,
RBJ cookbook coefficients) to `ssb_dsp.h`/`.c`, alongside the existing
`ssb_allpass1_t` — `ssb_shelf_biquad_set_highshelf()`,
`ssb_shelf_biquad_reset()`, `ssb_shelf_biquad_process()`. Named
`ssb_shelf_biquad_t` rather than the more obvious `ssb_biquad_t` because
`ssb_adc_filter.h` already defines its own, differently-shaped
`ssb_biquad_t` (Direct-Form-II-Transposed, used for the ADC low-pass
filters) — the first version of this code used `ssb_biquad_t` and hit a
"multiple definition" link error at compile time, caught immediately when
the user tried to flash it; renamed to fix. Kept separate from the private peaking-EQ biquad
already in `ssb_dsp.c` (that one stays wired one specific way into the
pre-Hilbert audio_fx chain; this one is the general-purpose public
primitive, reused for the envelope-path shelf below). Unlike
`ssb_allpass1_t`, this is explicitly NOT unity-gain — that's the whole
point.

**The chosen shelf.** `envelope_ampeq.h`/`.cpp` (mirrors
`envelope_gdeq.h`/`.cpp`'s structure — same `#if SAMPLE_RATE_HZ ==
16000 && ENV_FILTER_VARIANT == ENV_FILTER_PNP_BC327_ATTN ... #error`
gating, so this shelf can't silently get applied to a different
filter/Fs it wasn't chosen for). Corner `ENV_AMPEQ_SHELF_FREQ_HZ =
2500Hz`, plateau gain `ENV_AMPEQ_SHELF_GAIN_DB = 6.0dB`, RBJ high-shelf
S=1. Chosen by inspection against the measured loss table, not by
numerical optimization (there's no obvious single objective for a
2-parameter shelf the way there was for the 2-parameter all-pass
group-delay fit). Net (measured loss + shelf gain) at the table's key
frequencies:

| Frequency | Measured loss alone | Shelf gain | Net (residual) loss |
|---|---|---|---|
| 500Hz | -0.28dB | +0.01dB | -0.27dB |
| 700Hz | -0.45dB | +0.03dB | -0.42dB |
| 1700Hz | -1.55dB | +0.95dB | -0.60dB |
| 1900Hz | -1.85dB | +1.38dB | -0.47dB |
| 3100Hz | -4.52dB | +4.41dB | -0.11dB |
| 4300Hz | -8.75dB | +5.69dB | -3.06dB |
| 8000Hz | -21.70dB | +6.00dB | -15.70dB |

Recovers most of the loss through the two-tone/IMD-offset region
(500–3100Hz), meaningfully reduces it at the old 4300Hz gdeq fit-band
edge, and deliberately leaves most of the 8000Hz-region loss
uncorrected — the conservative "modest" step, not a full inverse.

**Wiring.** New key `a` (serial_commands.cpp) toggles it, mirroring `g`
exactly, including reset-on-off→on-transition. Applied in the main
`dsp_task` pipeline right after `envelope_gdeq_process()` (order doesn't
matter — both LTI — placed there purely for code locality) and in the
`AUDIO_SRC_CHIRP` early-intercept path right after gdeq's own chirp-path
call, so the `w` chirp/TFA workflow's amplitude channel can validate this
shelf's actual on-bench correction directly, the same way the phase
channel already validated gdeq's refit. `PersistentSettings` gained a
new trailing field `env_ampeq_enable` (off by default, same zero-fill-
safe convention as every other trailing field); the `P` dump and preset-
load paths were updated to match.

**Interaction with gdeq.** Both LTI, so cascade order doesn't change the
combined response mathematically. Unlike `ssb_allpass1_t`, a high-shelf
biquad's magnitude isn't flat, so it does have its own (uncharacterized)
small group-delay contribution near its corner — not yet folded into
gdeq's fit. If the combined on-bench group delay measurably diverges from
the gdeq-alone prediction once `a` is enabled, that's the first place to
look.

**Status: NOT YET VALIDATED ON REAL HARDWARE.** Off by default. Next
step is a `w` chirp/TFA sweep with `a` ON to check the amplitude channel
against the predicted net-correction table above, then real two-tone/mic
IMD testing — same validation sequence gdeq went through.

## 2026-09-03, later same day: `a` validated on real hardware — magnitude tracks prediction, but it reintroduces real group-delay dispersion

Source: `ga_Trial1_TF.txt`, a real TFA sweep with both `g` and `a` on
(amplitude+phase, same instrument/convention as `TF meas.txt`). First
real-hardware two-tone/mic IMD feedback on `a` came in alongside it: a
genuine positive effect — it balances the upper/lower sideband IMD levels
and brings the LF-region higher-order IMDs down to match the HF-region
ones. `eq` (`e`, the pre-Hilbert HPF+presence chain) still has the
biggest overall positive effect on IMD, separately discussed below.

**Magnitude side: tracks the predicted net-correction table well.**
Referenced to its own 100–300Hz passband average, same method as the
insertion-loss measurement:

| Frequency | Predicted net (design table above) | Measured net (this sweep) |
|---|---|---|
| 500Hz | -0.27dB | -0.41dB |
| 700Hz | -0.42dB | -0.54dB |
| 1700Hz | -0.60dB | -0.54dB |
| 1900Hz | -0.47dB | -0.85dB |
| 3100Hz | -0.11dB | +0.12dB |
| 4300Hz | -3.06dB | -2.43dB |
| 8000Hz | -15.70dB | -16.03dB |

Agreement is within a few tenths of a dB at every point except 1900Hz
(0.38dB off) — well within normal real-hardware measurement variation,
nothing here suggests the shelf isn't doing what it was designed to do.

**Group-delay side: a real, quantified side effect, not just the
theoretical caveat `envelope_ampeq.h` flagged.** Compared against the
earlier `g`-only validation sweep (100–8000Hz, same 181-point
Savitzky–Golay convention as every other group-delay figure in this
document):

| | `g` only (measured, earlier validation) | `g`+`a` (measured, this sweep) |
|---|---|---|
| p-p dispersion | 19.4µs | **81.7µs (~4×)** |
| mean absolute group delay | 198.6µs | 197.2µs (essentially unchanged) |

The shelf's own group delay, computed directly from its RBJ coefficients
(fc=2500Hz, +6dB, S=1, no measurement involved): swings from about
-37µs just below its 2500Hz corner to +20µs above it (p-p ≈65µs, i.e.
almost the entire size of the measured increase) while averaging only
≈0.4µs across the band (i.e. almost the entire reason the absolute mean
barely moved). Adding this theoretical shelf-alone curve to the `g`-only
measured curve (simple LTI superposition) reproduces the measured `g`+`a`
curve's shape to within ~7µs RMS — not as tight as gdeq's own ~2µs
RMS fit-to-measurement match (expected: this combines two separately-
smoothed noisy real sweeps rather than smoothing one already-combined
measurement), but more than enough to confirm the mechanism: **the shelf
is not phase-transparent, and gdeq's fit has no way to know about it**
(gdeq was fit purely against the analog filter's own phase response,
before ampeq existed). Chart: `gdeq_ampeq_delay_chart_v4.html`.

**Not a contradiction of the magnitude result or the user's IMD report**
— both stand as measured. It's a second, independent effect worth
tracking: right now the shelf's amplitude-symmetry benefit is winning on
the bench, but if `a`'s gain is ever pushed higher (getting closer to the
full ~22dB inverse this was deliberately capped short of), the shelf's
own delay swing will grow too — it scales with how much correction the
shelf is asked to make, the same way its magnitude effect does. Worth
re-running the IMD delay-sweep (the one that found ~4 samples optimal for
`g` alone) with `a` also on, since its own group-delay contribution could
shift where that optimum sits.

## 2026-09-03, later same day: is `eq`'s IMD benefit the HPF or the presence boost?

User's real-hardware report: `eq` (`e`) still has the single biggest
positive effect on IMD of everything tried so far — cleans up
inter-modulation tones broadly, not just the sideband-symmetry effect
`a` produces. Asked whether this is specifically the 300Hz highpass
stage's influence.

**Can't be isolated with the current code — `eq_enable` gates both
stages as one unit.** `ssb_dsp.c`'s `ssb_dsp_process_sample()` runs
`biquad_process(&h->eq_hpf, ...)` then `biquad_process(&h->eq_presence,
...)` back to back, both inside the single `if (handle->eq_enable)`
block — there's no way to toggle the 300Hz HPF and the 2200Hz/+4dB
presence peak independently via serial command today.

**Working hypothesis, not yet confirmed: more likely the HPF.** This
project has already established (`null_bias_investigation.md`,
2026-08-31) that `wrap_pi()`'s resolution of the two-tone envelope's
phase behavior right at destructive-interference nulls carries a real,
measurable bias — and that investigation's own reproduction workflow
explicitly calls for `e` (and `c`) to be off before taking a clean
`null_bias`/`null_bias2` reading, i.e. this project already treats `eq`
as a confound for null-crossing phase behavior, not just a tone-coloring
effect. A 300Hz 2nd-order highpass removes near-DC content (mic/ADC
offset, sub-audio drift, mains hum) from the audio *before* the Hilbert
transform — exactly the kind of content that would show up as an
envelope-baseline/near-null error once split into envelope+phase, and
that's a plausible mechanism for a broad, general IMD cleanup rather than
just a narrowband tone-coloring effect. The 2200Hz/+4dB presence peak, by
contrast, has no obvious general IMD mechanism at the two-tone
frequencies in use (500–1900Hz) — it's there for perceived voice
intelligibility, not distortion.

This is a hypothesis worth testing directly, not a conclusion — the
cleanest way is to split `eq_enable` into two independently-toggleable
flags (HPF-only, presence-only) so both can be A/B'd on the bench the
same way `g`/`a` are. Not yet done — parked here pending the user's
go-ahead, see the accompanying conversation.

## 2026-09-03, later same day: why `eq` can't just be added to the `w` chirp/TFA test loop the way `g`/`a` were

User asked whether `eq` could be added into the `w` sine-chirp/TFA test
loop the same way `g` and `a` were. Structurally it can't be done the
same trivial way, for an architectural reason worth recording:

`g` (`envelope_gdeq_process()`) and `a` (`envelope_ampeq_process()`) both
operate *downstream*, on an already-extracted envelope AMPLITUDE value —
that's exactly the domain `test_signals_generate_chirp()` synthesizes
directly, so calling them in the `AUDIO_SRC_CHIRP` fast-path (which
completely bypasses `ssb_dsp_process_sample()` — see
`ssb_mic_test.ino`'s own comment on this) was a same-domain, one-line
addition each time.

`eq` (`ssb_dsp_set_eq_enabled()`, the HPF+presence chain) operates
*upstream* — on the raw single-channel AUDIO sample, *before* the
Hilbert transform that splits it into envelope and phase in the first
place. It lives entirely inside `ssb_dsp_process_sample()`, which the
chirp fast-path bypasses by design. There is no "envelope value" for
`eq` to run on at the point the chirp injects its sweep — `eq`'s effect
is on the audio signal that DETERMINES both the envelope and the phase
channels together, not a post-processing step on either one alone.

Characterizing `eq` with a TFA-style sweep the way `g`/`a` were
characterized would need a genuinely different test mode: a swept sine
fed in as the raw AUDIO sample (i.e. through `ssb_dsp_process_sample()`
itself, `eq` on vs. off), with the resulting `envelope` AND `freq_dev_hz`
outputs captured as the TFA's two channels — a legitimately new test
signal generator (`test_signals.h` pattern), not a one-line addition to
the existing envelope-domain chirp. This would actually be a more
complete tool than the current chirp, since it would show `eq`'s
combined effect on both the envelope and phase paths together (which is
exactly the domain a real two-tone/mic signal lives in) rather than
either path in isolation. Not yet built — parked here as a scoped,
concrete next step pending the user's go-ahead, see the accompanying
conversation.

## 2026-09-03, later same day: what is `eq` actually doing to the 700/1900Hz two-tone signal?

Follow-up to the HPF-vs-presence question above — computed the two
stages' actual RBJ response (exact coefficients from `biquad_set_highpass`/
`biquad_set_peaking` in `ssb_dsp.c`) at the default two-tone pair
(`TWOTONE_F1_HZ`/`TWOTONE_F2_HZ`, 700/1900Hz, `config.h`) and at the
65-tap Hilbert FIR's (`HILBERT_TAPS`, `generate_hilbert_coeffs()`) own
frequency response, to see what's mechanistically available to explain
the reported IMD benefit.

**Ruled out: tone-level balancing.** At 700Hz/1900Hz the 300Hz HPF
removes essentially nothing (-0.14dB / -0.00dB) — it's not shaping the
wanted tones at all. The 2200Hz presence peak, if anything, works
*against* balance: +0.40dB at 700Hz vs. **+3.60dB at 1900Hz** — a real
asymmetric boost of the higher tone, the opposite of what a "levels the
tones" explanation would need.

**Found: the HPF imposes a real, frequency-dependent phase-delay
difference between the two tones, even where it barely touches their
amplitude.** Phase-delay (each pure tone's own time-shift through the
filter, `-phase(f)/(2πf)` — not the same "group delay" this document uses
elsewhere for envelope dispersion, so naming it distinctly here to avoid
confusion) at 700Hz is **-144µs**, at 1900Hz only **-18µs** — a
**~126µs differential shift between f1 and f2**, imposed on the raw
audio sample *before* it ever reaches the Hilbert transform. That's the
same order of magnitude as the delay `g` itself was built to compensate
for downstream, just happening upstream instead. Shifting the relative
timing between the two input tones changes the exact shape of the
resulting envelope/phase trajectory the whole downstream chain
(quantization, PWM, the analog reconstruction filter) has to reproduce —
a concrete, testable candidate mechanism, and one that applies even to
the firmware's own purely-synthetic `'t'`/`'T'` two-tone generator
(`test_signals.cpp` — no real audio front end involved there at all).

**Still relevant if the two-tone signal is ever fed through the real mic
front end instead of the synthetic generator:** the Hilbert FIR's own
frequency response, computed directly from its 65 taps, is flat to
better than 0.1dB from ~500Hz up to Nyquist, but is NOT flat below that —
already -0.72dB at 300Hz, -2.68dB at 200Hz, -7.65dB at 100Hz, and
mathematically forced to exactly zero at DC (the antisymmetric
windowed-sinc construction always nulls there). Any real-world content
down there — mic/ADC DC offset, mains hum, thermal drift — would get an
inaccurate quadrature component once Hilbert-transformed, and since
envelope and phase are both derived from that same analytic signal, the
error leaks into both channels at once — a broadband effect, not tied to
one IMD product, matching a "cleans up IMDs generally" description. This
mechanism has nothing to act on for the *synthetic* two-tone signal
specifically (it's two clean sinusoids, no sub-300Hz content to strip),
so it only applies if the two-tone test in question actually runs through
the ADC/mic path.

**Which mechanism is live depends on which test path was used** — parked
both here since it isn't yet known which (or both) explain the reported
result; see the accompanying conversation. Splitting `eq_enable` into two
independently-toggleable flags (proposed above) remains the clean way to
settle this on the bench rather than reasoning about it further from
coefficients alone.

## 2026-09-03, later same day: confirmed synthetic-only — and a stronger candidate mechanism found

User confirmed all `eq` A/B testing to date uses only the firmware's
synthetic `'t'` two-tone generator, never the mic/ADC path. **This
cleanly rules out the Hilbert-FIR-low-frequency-conditioning mechanism
above** — a mathematically pure two-tone sum has no DC offset, hum, or
drift for the HPF to strip, so there's nothing for that mechanism to act
on here. That leaves the differential phase-delay finding, plus a third,
considerably stronger candidate found while re-checking the combined
(HPF+presence, cascaded, matching the actual code order) response rather
than each stage in isolation:

**The presence peak's asymmetric boost keeps the two-tone envelope from
ever reaching a true zero null.** For two tones of amplitude A1, A2, the
envelope's minimum is exactly `|A1-A2|` and its maximum is `A1+A2` —
textbook two-tone algebra. With `eq` off, `TWOTONE_AMPLITUDE` (0.45,
`config.h`) is applied equally to both tones, so A1=A2 and the envelope
hits a **literal, exact zero** at every destructive-interference null —
the single hardest condition this whole project has spent significant
effort characterizing (the predistort LUT's steepest, most sparsely-
characterized region sits at 36–55% duty specifically because of this;
`null_bias_investigation.md`'s whole subject is a discrete artifact at
exactly this condition; the `'x'`/`'z'` envelope-floor feature exists
specifically to avoid driving the envelope this low).

With `eq` on, the combined HPF+presence response (computed at the exact
coefficients, cascaded in the real code order) is +0.26dB/-176µs
phase-delay at 700Hz and **+3.60dB**/-30µs phase-delay at 1900Hz — almost
all of that dB difference comes from the presence peak (the HPF's own
amplitude effect at both tones is under 0.15dB, negligible). That's a net
**+3.34dB (~1.47×) amplitude mismatch between the two tones**. Redone
through the actual `TWOTONE_AMPLITUDE=0.45` numbers: A1→0.4636,
A2→0.6809 — envelope minimum rises from an exact 0 to **0.217**, i.e.
**-14.4dB relative to the new peak (1.145)**. In other words: `eq`, as
currently wired, is *inadvertently* doing something close to what the
dedicated `'x'`/floor feature does deliberately — keeping the envelope
out of its worst, most-nonlinear region — except it does it as an
unplanned side effect of the presence peak's asymmetric gain, tied to the
specific two-tone pair in use, rather than as a controlled, symmetric,
tunable floor.

**This also reframes the earlier HPF-vs-presence guess — the opposite
way round.** Correcting the working hypothesis from two entries above (that
guessed the HPF was more likely responsible, reasoning from the
null-crossing bias investigation's own test protocol): with the synthetic
generator confirmed as the only signal path in use, the presence peak
looks like the more likely dominant contributor via this null-floor
mechanism, not the HPF. The HPF's own contribution is real but smaller —
the ~126µs (HPF-alone) to ~146µs (combined) differential phase-delay
between the two tones remains a genuine, separate effect, just evidently
a second-order one next to a >14dB null-floor change.

**Falsifiable prediction for the split-toggle test (still the right next
step):** presence-only should reproduce most of `eq`'s current IMD
benefit; HPF-only should show much less. If that holds, it also predicts
that dialing in the existing `'x'` envelope-floor control (with `eq` off
entirely) might reproduce a similar benefit in a cleaner, independently-
tunable way — worth trying directly on the bench, no code changes needed
for that particular check.

## 2026-09-03, later same day: `'x'` makes IMDs worse — resolved, and confirms amplitude (not tone timing) is the mechanism

User tried the `'x'` floor prediction above directly: **it makes IMDs
worse in every case tried, not better.** Separately, and unprompted, the
user reports scoping the envelope signal directly: `eq` off shows real
zeros at the nulls with bad IMDs (particularly the low-order inter-IMD
tones already flagged before, around 200/400Hz); `eq` on shows a visibly
higher envelope floor and cleaner IMDs — an independent, direct
hardware confirmation of the envelope-floor-height finding above, not
just a math prediction anymore. Question asked: is the benefit from the
tones' relative *phase* (the ~126–146µs timing-shift finding) or just
their relative *amplitude*?

**Answer: amplitude — and the reasoning explains why `'x'` backfires.**
Modeled the analytic signal directly (`A1*e^{jw1t} + A2*e^{jw2t}`,
exact for a sum of two positive-frequency tones) and computed the
per-sample instantaneous frequency (`freq_dev`) through one full
700/1900Hz beat cycle at 16kHz, for both conditions:

- **`eq` off (equal amplitude, A1=A2=0.45):** the sample landing nearest
  the null shows `freq_dev` **flip sign** right there — +1300Hz on the
  samples either side, -6700Hz on the one nearest the null. That's not
  just a big number, it's a genuine derivative discontinuity: the
  complex trajectory is passing essentially through the origin, and the
  phase has nowhere to go but reverse direction abruptly. (The exact
  peak value is sample-grid-dependent — how close a given sample lands
  to the true continuous-time null, which is a genuine, unbounded
  singularity in continuous time — which is very likely why the earlier
  delay-sweep found some delay values "null out higher freq better" than
  others with real spread: the delay setting shifts exactly which sample
  lands closest to the true null.)
- **`eq` on (mismatched amplitude, A1=0.4636, A2=0.6809 from the
  computed ampl. response above):** `freq_dev` through the same region
  rises and falls smoothly — 1744→2435→4071→2953→1882→1587→1483Hz — no
  sign flip, no discontinuity, bounded.

The reason is geometric, not about timing: the envelope minimum is
exactly `|A1-A2|`, which depends only on the two amplitudes — a pure
relative-*phase*/timing shift between the tones (what the HPF alone
contributes) only moves *when* the null occurs, never *whether* the
complex trajectory actually reaches the origin. Only an amplitude
mismatch keeps the trajectory bounded away from the origin, which is
what keeps `freq_dev`'s derivative bounded and sign-consistent. So the
answer to "phase or amplitude" is **amplitude** — the ~126–146µs timing
shift from the HPF is real but doesn't touch null *depth*, only its
timing, and null depth is what the phase-derivative singularity actually
depends on.

**This also fully explains why `'x'` fails.** `envelope_floor_apply()`
runs on the envelope value only, after it's already been computed — it
never touches `freq_dev_hz` (confirmed both by re-reading
`envelope_floor.h`'s own doc comment and by the REVISION-1 postmortem
in the "Envelope-null floor" section of `ssb_mic_test_commands.md`,
which describes exactly why freezing `freq_dev_hz` too was tried and
reverted). So with `'x'` engaged and `eq` off, the *actual* input tones
are still exactly equal amplitude — the true analytic-signal trajectory
still passes through the origin, `freq_dev` still takes that same
violent, sign-flipping excursion right at the null — but now the
envelope value being reconstructed at that exact instant has been
artificially forced UP by the floor remap. That means the violent phase
transient now rides on a higher-amplitude carrier instead of a
naturally near-zero one — normally, a deep null's low RF amplitude
somewhat masks how much that transient's spectral spread actually
contributes to the output; `'x'` removes that masking without touching
the transient itself, which is a plausible, mechanistically clean reason
it measures worse rather than better. `eq`'s mismatch, by contrast, fixes
the actual root cause upstream (the trajectory never approaches the
origin in the first place), so there's no transient left to mask.

**Net conclusion:** the improvement `eq` provides is real, amplitude-
driven, and now confirmed on the bench (envelope floor height) as well
as by the math. The HPF's own ~126–146µs tone-timing shift is a real,
separate, and much smaller effect that doesn't touch null depth at all.
The split-toggle test (HPF-only vs. presence-only) is still the cleanest
way to confirm the relative sizes on the bench, but is no longer needed
to answer the phase-vs-amplitude question itself — that one's settled.

## 2026-09-03, later same day: this is a known, named problem in the EER/polar-transmitter literature — research summary

User asked whether this null/phase-flip issue is something the field has
already tackled, and specifically whether noise injection/dithering
could help. Had this researched properly (web search, not recalled from
memory) rather than guessed at. Findings, each with a real source:

- **This is THE well-known Achilles-heel of Kahn-technique/EER/polar
  transmitters**, not a quirk specific to this project's architecture.
  Traces back to L. Kahn's original 1952 EER paper (*Proc. IRE*), is
  covered in the standard survey series (Raab, Asbeck, Cripps, et al.,
  "RF and Microwave Power Amplifier and Transmitter Technologies," Parts
  1–5, *High Frequency Electronics*, 2003–2004), and is precisely
  characterized in **J. Zhuang, K. Waheed, R. B. Staszewski, "A
  Technique to Reduce Phase/Frequency Modulation Bandwidth in a Polar RF
  Transmitter," IEEE Trans. Circuits and Systems I, vol. 57, no. 8, Sept.
  2010, pp. 2196–2207** — which states the phase/magnitude split causes
  bandwidth expansion of "~10× the original signal bandwidth, or
  theoretically infinite... when the signal trajectory passes through or
  near the constellation origin." That "theoretically infinite" framing
  matches this session's own finding almost exactly (the discrete
  `freq_dev` sign-flip at the null sample).
- **The established fix matches what `eq` stumbled into, generalized
  correctly**: Zhuang et al. propose "altering the signal trajectory
  such that it avoids crossing (and proximity of) the constellation
  origin," done in the Cartesian (I/Q) domain **before** the
  envelope/phase split — at a small accepted EVM/ACLR cost. This
  independently validates the direction found this session by
  first-principles reasoning (the fix has to happen on the complex
  signal upstream of the split, not as a post-hoc scalar remap of the
  extracted envelope — exactly why `'x'` failed and `eq`'s upstream
  tone-amplitude mismatch worked). Important nuance: naive radial
  magnitude clamping (push `|z|` up, preserve its angle unchanged) does
  **not** by itself fix anything — `angle(k·z) = angle(z)` for any real
  `k>0`, so scaling magnitude alone leaves the angle/`freq_dev` trajectory
  completely untouched. Whatever "alter the trajectory" means in
  practice, it has to change the angle sequence too, not just rescale
  radius — `'x'` is a clean real-world demonstration of exactly this
  distinction going wrong.
- **Commercial precedent for envelope floor limiting**: US Patent
  7,412,213, "Envelope Limiting for Polar Modulators" (Sequoia
  Communications, filed 2006) describes low-side envelope floor limiting
  specifically to prevent envelope collapse at nulls. Flagging a caveat
  though, not a contradiction: this project's own `'x'` is exactly this
  idea (envelope floor limiting) and it measured worse, not better —
  strongly suggesting that if the patented technique works, it isn't
  doing a naive post-hoc scalar remap of an already-extracted envelope
  value the way `envelope_floor_apply()` does; it's likely applied
  somewhere that also reshapes the phase trajectory. Patent claims not
  independently verified in this pass.
- **Phase-path bandwidth/slew-rate limiting is the other standard,
  complementary technique class** (general EER practice — the phase
  modulator's own finite bandwidth naturally does some of this in analog
  Kahn implementations). This project already has a directly applicable
  tool for it: the `freq_dev` slew-rate limiter (see that section of
  `ssb_mic_test_commands.md`) — not yet specifically tested against this
  null-crossing spike. Cheapest next experiment, no new code needed.
- **Dithering/noise injection: NOT FOUND as an established or precedented
  technique for this specific problem.** Targeted search (EER/polar +
  dithering, phase-discriminator zero-crossing dithering, CORDIC
  near-origin literature) turned up nothing proposing noise injection to
  avoid this exact near-origin angle singularity. CORDIC literature
  treats near-origin error as an accuracy/analysis topic, not something
  dithered away. This doesn't mean it wouldn't work — the general
  principle (statistically avoiding a rare deterministic worst-case
  coincidence, at the cost of a small broadband noise floor, same logic
  as ADC dither) is sound elsewhere in DSP — but it should be treated as
  an untested idea worth a real bench measurement, not an established
  fix being reused.
- **Correction to an assumption made earlier this session**: had
  speculated the exact-zero null might be mainly an artifact of the
  deliberately worst-case, equal-amplitude two-tone IMD test rather than
  a real operational concern. The Zhuang et al. paper demonstrates the
  same bandwidth-expansion problem using a real WCDMA signal, not a
  synthetic two-tone — no support found for the "mostly a torture-test
  artifact" framing, so retracting it. Treat this as a genuine concern
  worth solving properly, not just a two-tone benchmark curiosity.

**Concrete next steps, roughly in cost order:** (1) test the existing
`freq_dev` slew-rate limiter specifically against the null-crossing
spike — already built, zero new code; (2) design a deliberate version of
what `eq` does by accident — a small, controlled adjustment to the
complex analytic signal (after the Hilbert transform, before
magnitude/phase extraction) that keeps the trajectory off the origin
independent of `presence_gain_db`/voice-EQ settings — not yet designed,
needs real thought about what specifically to add/reshape given radial
clamping alone is proven not to work; (3) if wanted, a real bench test of
noise injection, going in with clear eyes that it's unprecedented for
this exact problem, not a known fix being applied.

## 2026-09-03, later same day: slew-rate limiter real-hardware result — and the code's own assumption behind `FREQ_DEV_SLEW_MAX_FINITE_HZ` was wrong

Tested the "cheapest next experiment" from the list above. Real-hardware
result: **tightening the slew limit below 8000Hz/sample makes 3rd-order
IMD worse, even the smallest step down** — but loosening it ABOVE the
firmware's current 8000Hz/sample ceiling, to 12000 and then 20000,
gained 1-2dB in 3rd-order IMD with no other products getting worse (some
improved too). This directly falsifies the assumption written into
`ssb_dsp.c`'s own comment next to `FREQ_DEV_SLEW_MAX_FINITE_HZ`
(currently 8000.0f): *"raw freq_dev itself never exceeds max_freq_dev_hz,
~8000Hz, so a same-sign single-sample swing that large is already the
largest possible."* That reasoning only covers a same-sign swing. It
misses the actual worst case: a swing that crosses through/near zero at
a null flips SIGN, and a sign-crossing swing between two large-magnitude
opposite-sign values can be close to double a same-sign swing's size.

Modeled this directly: simulated the analytic signal for the 700/1900Hz
pair at 16kHz across many different sample-grid phase alignments (since
which discrete sample lands nearest the true continuous-time null is
alignment-dependent — the true continuous-time singularity is unbounded,
but sampling quantizes how close any real sample can get to it, which is
also very likely why the 2026-09-01 delay-sweep found some delay values
"null out higher freq better" than others with real spread — the delay
setting shifts exactly which sample lands closest):

- **`eq` off (true null, equal amplitude):** worst-case single-sample
  `|Δfreq_dev|` over many alignments ≈ **9640Hz** — comfortably explains
  why 8000Hz/sample wasn't enough (still clipping the worst alignments
  hard), why tightening further only clips progressively more of the
  large-but-otherwise-legitimate excursions near a null (hence
  monotonically worse), and why loosening to 12000 then 20000
  progressively reduced how hard that worst-case event gets clipped.
- **`eq` on (mismatched amplitude, no true null):** worst-case
  single-sample `|Δfreq_dev|` over the same alignments ≈ only **1650Hz**
  — again confirms `eq` and a sufficiently loose slew limiter are
  addressing the *same* underlying problem from two different ends: `eq`
  prevents the trajectory from ever approaching the danger zone; a loose
  enough slew limiter just stops harshly clipping the rare event when the
  trajectory does approach it (with `eq` off).

**Practical firmware gap:** `FREQ_DEV_SLEW_MAX_FINITE_HZ = 8000.0f` is
also the ceiling the interactive `'}'` key steps up to before snapping to
fully unlimited — so the empirically useful 12000–20000+ range the user
found **cannot currently be reached from the serial interface at all**,
only by setting `freq_dev_slew_limit_hz` some other way (the setter
itself, `ssb_dsp_set_freq_dev_slew_limit_hz()`, has no upper clamp, only
the lower one at `FREQ_DEV_SLEW_MIN_HZ` — so this was already reachable
without a recompile, just not via `'{'`/`'}'`). Worth raising
`FREQ_DEV_SLEW_MAX_FINITE_HZ` well past 20000 (with real margin over the
~9640Hz worst case found above — the 700/1900Hz pair specifically; a
different two-tone spacing or sample rate would shift that number) so
`'}'` can explore this range interactively, and correcting the stale
comment. Not yet done — see the accompanying conversation for the
proposed value and whether to also touch `FREQ_DEV_SLEW_STEP_HZ`/count
for reasonable button-press granularity over the wider range.

**Not yet done:** a finer sweep between 8000 and 24000+ (only 8000, 12000,
20000 tried so far) to find the true optimum rather than just confirming
the direction of improvement; also worth trying fully unlimited directly
for comparison, since the trend so far (8k worse → 12k better → 20k
better still) is monotonically improving toward looser, and it isn't yet
known whether that keeps improving all the way to off or peaks somewhere
finite.

---

## 2026-09-04 — extending `ampeq`'s magnitude correction toward 8000Hz: a second shelf stage, and its group-delay price

**Request:** "I'd like to extend the gain comp higher in freq. What can we
do there without breaking the group delay. I want to see if this would
improve the higher IMDs further." — a direct follow-on from the 2026-09-03
`ga_Trial1_TF.txt` work, which left the shelf-1-only design still -15.70dB
net at 8000Hz (measured -21.70dB loss, shelf-1 only clawing back +6.00dB
at its plateau).

**Design.** Added a second RBJ high-shelf stage (`ENV_AMPEQ_SHELF2_FREQ_HZ
= 6000.0f`, `ENV_AMPEQ_SHELF2_GAIN_DB = 10.0f`) cascaded after the existing
shelf (`envelope_ampeq.h`/`.cpp`, both under the one `'a'` enable flag —
no new key). Three candidates were compared against the measured
insertion-loss table before picking this one:

| shelf2 (fc, gain) | NET@500 | NET@1900 | NET@3100 | NET@4300 | NET@8000 |
|---|---|---|---|---|---|
| 5000Hz / +8dB  | -0.27 | -0.42 | +0.30 | -1.02 | -7.70 |
| 5500Hz / +9dB  | -0.27 | -0.45 | +0.09 | -1.89 | -6.70 |
| **6000Hz / +10dB** | **-0.27** | **-0.46** | **-0.02** | **-2.52** | **-5.70** |

All three leave 500-1900Hz essentially untouched (shelf2 contributes
<0.02dB there in every case — its corner is high enough not to disturb the
already-good midband). 6000Hz/+10dB was picked because it clears the
3100Hz IMD3-upper offset almost exactly (-0.02dB, vs. the others'
overshoot/undershoot) while recovering the most at 8000Hz of the three.
Full net table with both shelves stacked:

| Freq | Measured loss alone | shelf1+shelf2 correction | NET (was, shelf1-only) |
|---|---|---|---|
| 500Hz  | -0.28dB  | +0.01dB  | -0.27dB (-0.27dB) |
| 700Hz  | -0.45dB  | +0.03dB  | -0.42dB (-0.42dB) |
| 1700Hz | -1.55dB  | +0.96dB  | -0.60dB (-0.60dB) |
| 1900Hz | -1.85dB  | +1.39dB  | -0.46dB (-0.47dB) |
| 3100Hz | -4.52dB  | +4.50dB  | -0.02dB (-0.11dB) |
| 4300Hz | -8.75dB  | +6.23dB  | -2.52dB (-3.06dB) |
| 8000Hz | -21.70dB | +16.00dB | -5.70dB (-15.70dB) |

Poles for the new stage at (6000Hz, +10dB): magnitude 0.654 — comfortably
stable, well inside the unit circle despite the corner sitting closer to
Nyquist (8000Hz at this Fs) than shelf1's.

**Group-delay cost — computed the same way as every gdeq fit in this
project (analytic RBJ shelf phase, `-dphase/dw`, converted to µs), over
50-7999Hz:**

| Stage | p-p | mean | worst points |
|---|---|---|---|
| shelf1 alone (2500Hz/+6dB) | 64.9µs | 0.03µs | -41.6µs @ 1333Hz, +23.3µs @ 3586Hz |
| shelf2 alone (6000Hz/+10dB) | 125.4µs | 0.01µs | -39.1µs @ 5029Hz, +86.3µs @ 7037Hz |
| shelf1+shelf2 combined | 148.6µs | 0.04µs | (delays sum directly — both LTI) |

At the two-tone/IMD-offset frequencies specifically (combined shelf1+shelf2
delay only, not counting the analog filter or gdeq): 500Hz -42.7µs, 700Hz
-45.6µs, 1700Hz -49.5µs, 1900Hz -43.2µs, 3100Hz +0.2µs, 4300Hz -12.4µs,
8000Hz +70.5µs — roughly a 120µs swing across exactly the band this
project has spent the most effort flattening (envelope_gdeq.h).

**Why this can't be designed away.** This is the second time this session
a shelf's own delay contribution has come up (`a`'s original single-shelf
already reintroduced 19.4→81.7µs p-p dispersion when validated against
`ga_Trial1_TF.txt`, see the 2026-09-03 entry above), and it generalizes: a
causal, minimum-phase magnitude filter's gain and phase responses are
locked together by a Hilbert-transform-type relationship — it's the same
reason gdeq's own all-pass sections had to be built as UNITY-magnitude by
construction to get delay correction with zero gain side-effect. There is
no shelf *shape* that recovers more high-frequency gain without incurring
more nearby phase/delay distortion — bigger gain and/or a corner closer to
Nyquist (there is no headroom to push the corner further out; 8000Hz IS
Nyquist at this Fs, so this stage is already about as close to the correction
target as a shelf's own corner can usefully sit) means more delay cost,
full stop. So "extend the correction without breaking group delay" cannot
mean "find a magically phase-transparent shelf" — it means accepting this
stage's delay contribution and then **refitting `envelope_gdeq.h`'s
all-pass coefficients against the new combined (analog + shelf1 + shelf2)
phase response** — the identical numerically-fit-against-real-data method
already used twice in this project for smaller reasons (the BC337→
PNP_BC327_ATTN filter swap; then the LTspice→real-hardware data swap, see
`envelope_gdeq.h`'s history). **Not yet done.** gdeq's current coefficients
(a1=0.026173, a2=0.236810) were fit against the analog filter ALONE,
before shelf1 even existed, and are already known (2026-09-03 g+a trial)
to let shelf1's dispersion straight through uncorrected — adding shelf2 on
top without refitting will make the delay side worse, not better, even if
it helps IMDs.

**Recommended next step (not yet run):** flash this two-stage design,
re-run the `'w'` chirp/TFA workflow to measure the REAL combined
magnitude+phase response (don't trust the analytic prediction alone —
shelf1's magnitude prediction tracked real hardware to a few tenths of a
dB, but gdeq's own history shows real measurements occasionally diverge
from prediction in ways worth catching before committing further), THEN
decide whether the resulting two-tone IMD change is worth doing the gdeq
refit to recover flat delay. Only after that refit would this be a fair
like-for-like test of "does more high-frequency amplitude correction
improve the higher IMDs" — right now, testing shelf1+shelf2 on two-tone
IMD without refitting gdeq would conflate two effects (more amplitude
correction vs. more delay dispersion) and wouldn't cleanly answer the
question asked.

---

## 2026-09-04, later same day — shelf2 validated on real hardware (`ga_Trial2_TF.txt`): magnitude gain confirmed, dispersion roughly doubled, IMD marginally worse

**What was measured.** Same TFA sweep method as `ga_Trial1_TF.txt`
(`'g'`+`'a'` both on), this time with the two-stage `ampeq` (shelf1+shelf2)
flashed. Re-parsed both files with an identical pipeline (100–300Hz
passband-average magnitude reference; phase unwrapped, then 181-point
Savitzky–Golay smoothed, then `-dphase/domega` for group delay — same
convention as every other real-hardware analysis in this project) so
Trial1 and Trial2 are a clean apples-to-apples comparison, not just each
compared separately to its own prediction. Note: re-running this pipeline
from scratch on Trial1 gives 75.7µs p-p (vs. the 81.7µs previously
reported) — a small, expected difference from minor smoothing/derivative
implementation details between passes, not a real change in the
measurement; flagging this explicitly per this project's own past mistake
of conflating two different metric definitions (see the 2026-09-03 "own
analytical error" entry above) — the important number here is the
Trial1-vs-Trial2 RATIO, computed with one consistent pipeline, not the
absolute p-p figure compared across different analysis passes.

**Magnitude — real, substantial, a bit short of predicted.** Net loss at
8000Hz: Trial1 (shelf1-only) measured -16.03dB (matches the -15.70dB
prediction to 0.33dB, consistent with the original validation). Trial2
(shelf1+shelf2) measured -9.30dB — a real **+6.7dB** improvement from
adding shelf2, genuine and large, but short of the +10.00dB/net -5.70dB
the analytic model predicted (a ~3.6dB shortfall at the single most
aggressive point in the design — expected some gap here given shelf2 sits
closest to Nyquist of anything tried yet, but worth a closer look if this
stage is revisited). Mid-band (500–1900Hz) essentially unchanged between
trials, as designed. 3100Hz measured net actually improved slightly beyond
prediction here too (Trial1 +0.12dB overshoot → Trial2 -0.43dB, prediction
was -0.02dB) — real point-to-point measurement noise at the few-tenths-dB
level, not a concern.

**Group delay — dispersion roughly doubled, and the top end grew MORE than
predicted.** p-p over 100–8000Hz: 75.7µs (Trial1) → 157.5µs (Trial2), a
**2.1×** increase — matches the qualitative prediction (combining Trial1's
own measured dispersion with shelf2's analytic contribution projected to
~167µs, see `gdeq_ampeq_delay_chart_v5.html`) closely. Point-by-point delta
(Trial2 minus Trial1) tracks shelf2's own predicted delay curve well in
shape and sign across most of the band (500Hz -8.4µs vs. predicted
-10.85µs; 1900Hz -11.4 vs. -13.34; 3100Hz -21.0 vs. -19.44 — a very close
match; 4300Hz -36.0 vs. -32.34), but **at 8000Hz the real jump (+85.1µs)
notably exceeds the analytic prediction (+62.27µs)** — real hardware shows
more top-end delay growth than the idealized shelf model right at the edge
of the band, worth remembering if this stage's gain is ever pushed higher
still. Mean delay barely moved either time (197.1µs → 196.4µs), consistent
with both shelf stages' near-zero predicted mean contribution.

**IMD — marginally worse, and that result is itself informative.** User's
real two-tone report: shelf1+shelf2 IMD was marginally worse than
shelf1-only, not dramatically worse. Read against the numbers above, that
means roughly +6.7dB of amplitude correction and roughly +82µs of extra
p-p dispersion landed close to a wash on two-tone IMD in this UNREFIT
state — neither effect cleanly dominated. That's consistent with (not
proof of) this project's working model built up over this session's
earlier findings that IMD depends on both amplitude symmetry (the `eq`
finding, 2026-09-03) and delay/phase behavior near envelope
nulls/transients (the null-floor/`freq_dev` findings, same day) — it's
exactly why testing shelf2 without a gdeq refit first was flagged in the
entry above as not a clean test of "does more amplitude correction help":
both variables moved together, and a genuinely fair test needs the delay
side held flat.

**Decision, as stated by the user 2026-09-04:** revert to shelf1-only as
the better real-world compromise for now, while recording a wider range of
IMD products to compare between the two configurations before deciding
anything further. Shelf2 itself is NOT being removed from the firmware —
still selected via the same `'a'` flag alongside shelf1 (see
`envelope_ampeq.h`/`.cpp`) — this is a config/testing decision, not a code
reversion, so shelf2 stays available to re-test later, ideally after the
still-outstanding gdeq refit against the combined analog+shelf1+shelf2
phase response. Chart: `gdeq_ampeq_delay_chart_v6.html` (real Trial1 vs.
real Trial2, magnitude and group delay side by side).

Shortly after, shelf1 and shelf2 were split into independent enable flags
(`'a'`/`'A'`, see `envelope_ampeq.h`'s matching entry and
`serial_commands.cpp`) specifically so this comparison could continue
without reflashing between configs.

---

## 2026-09-04, later still — `A` (shelf2) preliminary IMD report: a candidate mechanism, unconfirmed

**The report, verbatim:** "A mixed bag so far. Reduces HF IMDs with eq off
but otherwise not a lot of change." First real use of the newly-independent
`'A'` toggle. Two observations to explain: (1) `eq` OFF + `A` ON reduces
HF IMDs; (2) otherwise (implicitly, `eq` ON, or other conditions not yet
specified) `A` does little.

**Why this is surprising at first glance.** `envelope_ampeq` (both shelves)
only ever touches the `envelope` value - the amplitude signal that drives
the RSET/PWM/analog-filter path. It has no access to, and cannot affect,
`freq_dev_hz` - the instantaneous-frequency signal that goes straight to
the AD9851 over SPI, entirely bypassing `envelope_ampeq`/`envelope_gdeq`.
The two are computed from the same analytic signal `z(t)` but diverge
immediately after (`envelope = |z(t)|`, `freq_dev` from `angle(z(t))`'s
derivative) and are never recombined in the digital domain - they only
combine physically, at the RF output, as (RSET-driven PA gain) ×
(AD9851's instantaneous-frequency-modulated carrier). This project's
established null/"nemesis" mechanism (see the dated entries elsewhere in
this file) is that with `eq` OFF, a true envelope null causes `freq_dev`
to violently sign-flip - a PHASE-domain event. `envelope_ampeq` cannot
prevent that flip; it never sees `freq_dev` at all. So how could enabling
`A` change the resulting IMDs?

**Candidate mechanism (NOT YET CONFIRMED - needs a real test, not just
this reasoning):** the RF output's actual amplitude at the null instant is
set by the PHYSICAL RSET/PWM/analog-filter chain's response to the digital
`envelope` value, not the digital value itself. The analog filter's own
severe high-frequency roll-off (the entire reason `envelope_ampeq` exists)
doesn't just attenuate steady-state high frequencies - it also SLOWS the
physical envelope's response to FAST transients, and a null's cusp
(a brief, sharp dip when `eq` is off) is exactly this kind of fast,
high-frequency-rich event. Before `A`: the physical RSET-driven PA gain
may not be dropping all the way to the digital envelope's true near-zero
value AT the null instant - filter lag smears/softens the dip in time,
so the gain trace sits somewhat higher, for somewhat longer, around the
null than the digital envelope itself specifies. If that's true, the
erratic `freq_dev` energy occurring at that same instant gets multiplied
by a not-quite-zero gain and leaks into the RF output as IMD. Enabling `A`
restores high-frequency fidelity to the envelope path (that's its whole
purpose), which could let the physical RSET-driven gain track the true
digital envelope more precisely and more quickly - meaning it actually
DROPS closer to zero, and does so more promptly, right when the null (and
its `freq_dev` transient) occurs. That would tighten the physical
"gate" on the erratic phase energy at exactly the moment it matters,
without `A` ever touching `freq_dev` directly - an AMPLITUDE-domain fix
for a problem whose ROOT CAUSE is in the phase domain, working by
tightening how well amplitude suppresses phase garbage at the RF stage,
not by preventing the phase garbage from existing.

**Why this predicts observation (2) as well, which is a good consistency
check (not proof):** with `eq` ON, there's no true null and no `freq_dev`
sign-flip to begin with (the presence-peak amplitude asymmetry already
keeps the envelope minimum well above zero - see the 2026-09-03 entries).
If the mechanism above is right, `A`'s benefit specifically depends on
there being erratic phase energy AT a null for the tightened gate to
suppress - with `eq` on, there isn't any, so `A` would be expected to do
"not a lot" - matching the user's second observation. A hypothesis that
predicts BOTH halves of an odd-looking report from one mechanism is worth
taking seriously, but two data points is not confirmation - still needs a
real test.

**What would confirm or kill this, not yet run:**
- Same two-tone test, `eq` off, `A` off vs on, scoping the RSET/envelope
  output directly right at a null crossing (same technique already used to
  see the null-floor phenomenon in the `eq` investigation) - if the
  mechanism is right, `A` on should show the physical envelope trough
  reaching visibly closer to zero, faster, than `A` off.
- A direct look at which IMD PRODUCTS specifically improved ("HF IMDs" is
  currently just the user's own characterization, not a specific
  frequency/order list) - the mechanism above predicts improvement should
  be concentrated at higher-order products (further from carrier), which
  are the ones most sensitive to a brief, wideband instant of leakage,
  rather than a broad, even improvement across all orders.
- Whether `g` was on or off during these particular runs, and at what
  relative-delay setting - not recorded yet, and relevant because `g`'s
  timing changes exactly when the envelope path's own event (including a
  null) lines up against `freq_dev`'s event in time.
- A repeat under identical conditions to rule out run-to-run/level-setting
  noise before treating "reduces HF IMDs" as a settled real effect.

**2026-09-04, confirmed on the bench:** user scoped the RSET/envelope
output directly and reports the null IS quicker and deeper with shelf 1,
and quicker/deeper still with shelf 2 - exactly the first prediction
above. This is real, independent confirmation of the candidate mechanism
(better HF envelope-path fidelity -> the physical RSET-driven gain tracks
the true digital envelope more precisely and faster at a null, tightening
how well amplitude gates the `freq_dev` transient) - upgraded from
hypothesis to bench-confirmed. Still open: the IMD-product-specificity and
same-conditions `eq`-on comparison items above.

---

## 2026-09-04 — refitting gdeq for the `a`+`A` case: real improvement, but a genuine trade-off, not a clean win

**Why:** user's own framing, following the null-depth confirmation above -
"Maybe we should refine the grp delay again to optimise that for the a+A
case." This is the gdeq refit flagged as outstanding since the shelf2
work began (envelope_ampeq.h's "Interaction with gdeq" note, and the
2026-09-04 entries above) - gdeq's current coefficients
(a1=0.026173, a2=0.236810) were fit against the bare analog filter alone,
before either shelf existed, and are known to pass both shelves'
dispersion straight through uncorrected.

**Method - no new hardware sweep needed.** Rather than requesting a fresh
`'g'` OFF + `'a'`+`'A'` ON TFA sweep, the "bare" (analog+shelf1+shelf2,
NO gdeq) phase curve was reconstructed directly from the existing
`ga_Trial2_TF.txt` measurement: since gdeq is a purely digital cascade
applied earlier in the same signal chain the TFA sweep measures end-to-end,
and its current coefficients are exactly known, `phase_bare = phase_measured
- phase_gdeq_analytic` is valid by straightforward LTI cascade algebra (all
stages are linear time-invariant, so phases simply add in a cascade).
**Validated by self-consistency**, not just asserted: re-adding the
current coefficients' analytic phase to the reconstructed bare curve
reproduces the real Trial2 measurement almost exactly - p-p 157.5us and
mean 196.4us either way, matching the direct measurement from the
2026-09-04 Trial2 entry above to the digit. Reconstructed bare curve: p-p
88.4us, mean 71.1us over 100-8000Hz - the mean matching the previously-
established analog-alone mean (71.2us) almost exactly is a second,
independent consistency check (both shelves have near-zero predicted mean
delay contribution, so total mean should track analog-alone's, and it
does).

**Optimization result - a real ceiling, not a coefficient-search failure.**
Grid search + Nelder-Mead (same method as every prior gdeq fit), 2/3/4
cascaded `ssb_allpass1_t` sections, minimizing p-p group delay over
100-8000Hz against the reconstructed bare curve:

| Sections | Best p-p achievable | Coefficients |
|---|---|---|
| 2 | 72.6us | a1=a2=-0.139115 |
| 3 | 71.3us | all three ≈ -0.08750 |
| 4 | 70.7us | all four ≈ -0.06398 |

More sections barely help - each additional section converges to a
smaller, near-identical coefficient rather than a meaningfully different
curve shape, meaning the achievable floor for THIS filter type (cascaded
single-real-pole all-pass) sits around ~71-73us p-p for this particular
bare curve shape, confirmed via both a coarse grid search and a global
differential-evolution search (both landed on the same floor
independently). **2 sections is therefore the right choice** - same
architecture already in envelope_gdeq.h, just new coefficients, and
extra sections buy essentially nothing here.

**Why the ceiling is so much higher than the original analog-alone fit's
(18.5us):** the bare curve here has a materially harder shape - it rises
from 100Hz to a first local peak near 3100Hz (~82us), FALLS to a local
minimum near 5000Hz (~39us), then rises sharply to a second, higher peak
near 7800Hz (~125us) right at the edge of Nyquist (8000Hz at this Fs).
That's two interior extrema plus a steep near-Nyquist edge, vs. the
original analog-only curve's single smooth hump. A first-order all-pass
section's own delay curve (`(1-a^2)/(1+2a*cos(w)+a^2)`) is smooth and can
only produce ONE hump over the full band - cascading sections helps when
each section can be given a genuinely different shape/location, but here
the optimizer keeps landing on near-identical coefficients per section,
meaning this curve's shape doesn't offer that flexibility to exploit. The
steep near-Nyquist rise is shelf2's own signature (its predicted delay
peaks at +86.3us right at 7037Hz, see the 2026-09-04 shelf2 entry above) -
consistent with shelf2 being the harder half of this curve to flatten.

**The trade-off - improvement is NOT uniform across frequency, read this
before flashing anything:**

| Freq | current (old gdeq coeffs) | candidate refit (a1=a2=-0.139115) | delta |
|---|---|---|---|
| 500Hz | 142.9us | 209.0us | +66.1us (worse) |
| 700Hz | 147.4us | 212.0us | +64.6us (worse) |
| 1700Hz | 167.1us | 218.7us | +51.6us (worse) |
| 1900Hz | 170.4us | 218.7us | +48.3us (worse) |
| 3100Hz | 191.3us | 214.6us | +23.3us (worse) |
| 4300Hz | 179.3us | 174.3us | -5.0us (~same) |
| 8000Hz | 290.5us | 217.9us | -72.6us (much better) |

The refit lowers OVERALL p-p by flattening the curve around a higher
common level, which concretely means it fixes the 8000Hz peak dramatically
but ADDS 24-66us of delay at 500-3100Hz relative to what's flashed right
now - and 500-1900Hz are the two-tone's own fundamental frequencies (700,
1900Hz) plus the IMD3-lower offset (500Hz). Since real IMD very plausibly
depends on the exact relative TIMING between the envelope and `freq_dev`
paths at the frequencies that carry the actual signal power (not just on
minimizing an abstract p-p number across the whole band equally), this
redistribution needs a real two-tone IMD comparison on the bench to judge
- a smaller p-p number is not automatically a "better" result for IMD, only
a flatter one on paper. A restricted-band variant (fit only over
100-4300Hz) was also tried for comparison: it flattens that sub-band
beautifully (27.3us p-p there) but drives 8000Hz UP to 311us - WORSE than
doing nothing - confirming the same fundamental tension from the opposite
direction and ruling that option out as a serious candidate.

**Mean delay barely moves** (196.4us -> 195.5us, about -1us), so `'['`/`']'`
relative-delay re-tuning should need only a small nudge if this is tried,
not a from-scratch search the way the very first gdeq introduction needed.

**Not yet done:** implementing this in `envelope_gdeq.h` (would need a
decision on HOW - see the open question below), a real `'g'` OFF +
`'a'`+`'A'` ON TFA sweep to directly confirm the reconstructed bare curve
(the self-consistency check above is strong, but every other gdeq fit in
this project has been validated against a fresh direct sweep before
trusting it, not just reconstructed from an existing one), and the actual
two-tone IMD comparison once flashed. Chart:
`gdeq_refit_a_A_candidate_v7.html`.

**Open architectural question, not yet resolved:** gdeq's coefficients are
currently a single fixed compile-time pair per filter/Fs combination
(`#if`/`#elif` on `SAMPLE_RATE_HZ`/`ENV_FILTER_VARIANT`). The RIGHT
coefficients now depend on ampeq's shelf state too - analog-alone (current
values, correct for `'a'`/`'A'` both off), analog+shelf1 (`'a'` on, `'A'`
off - never separately refit, still uses the analog-alone values, known
suboptimal per the 2026-09-03 g+a entry), and analog+shelf1+shelf2 (`'a'`+
`'A'` both on - this entry's new candidate). Three genuinely different
optimal pairs for three reachable configurations. Options for handling
this, not yet decided: (a) keep one fixed pair as a compromise (accepting
it's not optimal for every ampeq state); (b) make gdeq's coefficients
runtime-selectable, chosen automatically from `a`/`A`'s current state
(more correct, more code); (c) leave the `#define`s as a manually-edited
value swapped in only when specifically bench-testing the `a`+`A`
combination, same way different firmware variants have already been
swapped in and out through this session.

**Implemented, 2026-09-04, same day: option (c).** `envelope_gdeq.h` now
has both coefficient pairs in the file at once, gated by a new
`ENV_GDEQ_USE_AA_CANDIDATE` compile-time flag (0 = default, the existing
2026-09-03 analog-alone fit; 1 = this entry's a+A candidate). Chosen over
(a)/(b) because the a+A candidate is still an unconfirmed, non-uniform
trade-off (see the table above) that would actively regress the
currently-recommended default config (`'a'` on, `'A'` off) if it silently
replaced the existing values - a manual, obvious, one-line flip keeps the
validated default safe while making the candidate one edit away to
bench-test, consistent with how every other A/B comparison in this session
(slew-rate limits, ampeq shelf states) has been done. Revisit (b) - real
runtime switching - if the a+A candidate proves out on the bench and ends
up wanted as a standing option rather than an occasional test.

**2026-09-04, same day: bench IMD result — no measurable difference, candidate does NOT prove out.** `aAgdeq_optimise_comparison.xlsx`, two two-tone spectrum-analyzer sweeps at identical `PersistentSettings` (`g`, `a`, `A` all on; `eq`/`comp` off), run 1 = default coefficients (`ENV_GDEQ_USE_AA_CANDIDATE=0`), run 2 = the a+A candidate (`=1`). Noise floor agreed to 0.08dB between runs (-108.976 vs -108.9dBm) and every reference level agreed to ~0.1dB, confirming the two runs were a genuinely controlled A/B, not a different RF setup.

Checked every IMD/spread product's absolute level against the measured noise floor rather than trusting the raw relative-dB readings at face value. Every product sitting >13dB clear of the floor (-500 through -6500Hz, +3100 through +10300Hz - i.e. essentially the whole set that matters for judging real IMD) moved by only 0.05-1.1dB between runs, split roughly evenly between "better" and "worse" with no consistent direction - ordinary spectrum-analyzer read repeatability, not a filter effect. The handful of larger swings (-8900Hz, -10100Hz, +11500Hz, +12700Hz, all 1.5-5.3dB) are exactly the products whose absolute level sits at or below the noise floor in one or both runs (several read *below* the stated floor, which is only possible as floor-noise fluctuation, not a real spectral line) - meaningless for this comparison. "Low 10k leakage" (-6.0dB) is the one exception that doesn't fit the floor-proximity explanation (both readings 8-14dB clear of floor) and is being left as an unexplained single outlier rather than force-fitted to a story, pending a repeat measurement.

"Pwr mid 750Hz band" showed a real, floor-clear 9.95dB difference (-26.282dBm run1 -> -36.228dBm run2, candidate quieter) - the only large, unambiguous swing in the whole comparison. But this specific metric has its own history in this project (see the "GPIO13 5kHz pulse" / interp-tick section, 2026-09-02 entries): it previously tracked interp-tick float-ramp cost, FPU-context switching, and wake-rate effects - none of which changing `ENV_GDEQ_A1`/`A2`'s numeric *value* touches, since both coefficient pairs run through the exact same `ssb_allpass1_process()` multiply-adds at identical cost. No mechanism connects a coefficient-value swap to a 10dB change on that specific reading, so this is being treated as session-to-session environmental drift (PSU, warm-up, ambient pickup - the user's own hypothesis), not a real effect of the candidate, and is not being counted as evidence either way.

**Conclusion: the a+A candidate is IMD-indistinguishable from the current default on this bench, despite predicting ~2.2x better group-delay p-p dispersion (157.5us -> 72.6us).** This is another data point - alongside the 2026-09-01 case where `g` made 3rd-order IMD worse despite improving predicted dispersion - that the p-p group-delay figure on its own is not a reliable predictor of real two-tone IMD outcome on this hardware. **Decision: `ENV_GDEQ_USE_AA_CANDIDATE` stays at its default (0).** The candidate's known non-uniform cost (worse at 500-3100Hz, see the entry above) isn't bought back by any measured IMD benefit, so there's no reason to carry the trade-off. The flag itself stays in the file (harmless, one line, already validated to compile either way) in case a future ampeq/gdeq change reopens the question, but this specific candidate is not recommended for use.

## 2026-09-04 — closing the ampeq/gdeq optimization line: practical limits reached on this hardware

Stepping back across the whole `a`/`A`/`g` line of work in this file: every stage found a real, measurable, directionally-correct improvement in the physical quantity being targeted (shelf1/shelf2's magnitude flattening toward 8000Hz; the original `g` fit's ~3.5-3.9x group-delay p-p reduction; the a+A-specific refit's further ~2.2x reduction on top of that), each backed by real hardware TFA/spectrum measurements, not just simulation. But translating those into IMD wins has been inconsistent and, in the two most recent, most-refined attempts (shelf2, and now the a+A gdeq refit), essentially a wash or a net negative once actually bench-tested. That's a real, repeatable pattern across this project, not one bad result: `g` alone made 3rd-order IMD worse on 2026-09-01 despite improving predicted dispersion; shelf2 came back "marginally worse" on 2026-09-04 despite a genuine +6.7dB real magnitude gain; the a+A gdeq candidate came back indistinguishable despite a ~2.2x p-p improvement. Meanwhile the single biggest, most reliable IMD win found in this entire project (the `eq`/amplitude-mismatch mechanism, see the 2026-09-03 sections above) came from a completely different lever - not magnitude flatness, not delay flatness, but keeping the envelope off a true zero at destructive-interference nulls.

**User's summary, and the working conclusion this project is adopting:** envelope accuracy matters in both time (group delay) and shape (amplitude/magnitude vs. frequency) - both are real, both have been shown to move things - but on THIS hardware (this analog reconstruction filter, this ADC/DAC/PWM chain, these achievable coefficient/filter-order constraints), continued refinement of `a`/`A`/`g` specifically has reached the point of diminishing, inconsistent returns rather than clear wins. Further gains are more likely to come from the amplitude-mismatch/true-zero-null mechanism (see the new section below) than from squeezing more precision out of the shelf/all-pass filters. Not a decision to revert or remove any of `a`/`A`/`g` - all three remain in the firmware, independently toggleable, still individually the best-known settings for what they each target - just a decision to stop spending further optimization effort specifically on refitting them further.

## 2026-09-04, same day — testing the amplitude-mismatch hypothesis directly: a runtime tone-ratio control (`'R'`)

**Motivation.** The 2026-09-03 finding (see "`'x'` makes IMDs worse... confirms amplitude" above) established that `eq`'s IMD benefit comes from the presence peak's ~3.34dB net amplitude mismatch between the two tones keeping the envelope minimum `|A1-A2|` off a true zero, which prevents `freq_dev` from flipping sign at the null (a genuine derivative discontinuity when the tones are exactly equal). That finding came from an INCIDENTAL side effect of `eq` - a filter built for a completely different purpose (HPF + presence peak) happened to also fix this. The natural next question, raised this turn: does deliberately controlling the tone-amplitude ratio directly, with `eq` fully off, reproduce (or exceed) the benefit on its own - confirming the mechanism cleanly, without `eq`'s other effects (the ~126µs differential HPF phase-delay documented in the 2026-09-03 "what is `eq` actually doing" section, or the presence peak's own frequency-response shaping) as confounds?

**Why this is a clean test, mathematically.** With the two-tone signal written as the real part of `A1*e^(j*w1*t) + A2*e^(j*w2*t)`, the instantaneous envelope is `|A1*e^(j*w1*t) + A2*e^(j*w2*t)|`, which reaches its minimum value `|A1-A2|` whenever the two phasors are exactly opposed (once per beat period, at the classic two-tone "null"). When `A1=A2` exactly, that minimum is a literal zero - the trajectory passes through the origin, `freq_dev` (proportional to `d(phase)/dt` of the analytic signal) is undefined in the limit and flips sign discontinuously in the discrete-time implementation (the 2026-09-03 entry measured +1300Hz either side, -6700Hz at the null sample for the 700/1900Hz pair at 16kHz). Any `A1 != A2` makes that minimum strictly positive, so the trajectory passes NEAR the origin but never through it - `freq_dev` still swings hard near a null (this is a real, unavoidable EER/polar-transmitter characteristic per the Zhuang/Waheed/Staszewski citation, not something a tone-ratio choice removes entirely) but does so continuously, without the sign-flip discontinuity that most directly drives spectral splatter. This depends ONLY on the amplitude ratio - not on which tone is scaled, not on relative phase/timing (a pure delay shift moves WHEN a null occurs, never whether it's a true zero) - so it isolates the exact mechanism identified on 2026-09-03 with nothing else riding along.

**Implementation.** Added a new `'R'` serial command (`test_signals.cpp`/`.h`), cycling `TONE_RATIO_PRESETS`: equal (0dB, unchanged default), -1dB, -3dB, -6dB, -10dB, -20dB, scaling tone2 down relative to a fixed tone1 (`TWOTONE_AMPLITUDE`). Chose to scale DOWN only, never up, so the combined constructive-interference peak (`TWOTONE_AMPLITUDE * (1 + gain)`) never exceeds today's existing 0.9 peak at any step - no new headroom/clipping risk introduced, unlike boosting one tone which would have pushed peaks toward or past 1.0 at the larger ratios. -3dB (`0.707946` linear) is deliberately included as the step closest to the ~3.34dB mismatch `eq`'s presence peak was measured imposing on this exact 700/1900Hz pair, so an `'R'` @ -3dB (eq off) vs. `eq`-on comparison at the same pair, same everything else, is the single most direct falsification test available - if the hypothesis is right, those two should come back close to equally clean; if `'R'` at -3dB doesn't reproduce it, the HPF or some other `eq` side effect is doing more of the work than the 2026-09-03 analysis credited. Independent of `e`/`a`/`A`/`g`, matching this project's consistent convention of orthogonal, freely-combinable toggles for isolating one variable at a time. Not persisted in `PersistentSettings` (same precedent as `'T'`'s band selection - a bench-testing tool, resets to equal/0dB on boot, not a saved-preset lever). Verified via a standalone syntax/logic check (extracted the exact generator + preset-cycling code into a stub harness, compiled clean with `gcc -fsyntax-only -Wall -Wextra`) and manual brace-balance check of both edited files; not yet bench-tested on real hardware.

**Practical motivation, if confirmed: a built-in "Tune" signal that sidesteps the EER null problem by construction.** The conventional two-tone test/tune signal is defined as equal-amplitude specifically to give a clean, symmetric, standardized IMD reading - but on a polar/EER architecture like this one, equal amplitude is exactly the condition that maximizes the null-crossing sign-flip and its spectral splatter. If a small, fixed, deliberate amplitude offset between the two tones (no `eq`, no HPF, no presence-peak coupling, no other DSP correction) reliably cleans up the splatter the way `eq` does, the practical implication is straightforward: any built-in tune/test signal on this hardware should be GENERATED with that offset from the start (a firmware constant, not a live control), rather than inheriting the traditional equal-level convention from architectures that don't have this problem. That would make a clean tune signal available with zero dependence on `g`/`a`/`A`/`eq` all being correctly tuned first - useful specifically because a tune signal's whole point is being a known-clean reference independent of other settings. This does not solve the general problem for real speech/music (which also passes near the phasor origin at times, per the cited literature) - it is specifically a win for a purpose-built test/tune tone, where the amplitude ratio is a free design parameter with no meaning to preserve.

**Not yet done:** any real-hardware measurement with `'R'` - this section documents the reasoning and the implementation, not a result. Recommended workflow: `T` to a tone pair, `e`/`c`/`g`/`a`/`A` all off, `'r'` to reset diagnostics, step `'R'` through its presets and read IMD at each, compare directly against the same pair with `eq` on (`'R'` back to equal/0dB) as the reference point this is trying to match or beat.

## 2026-09-04, later same day — hypothesis confirmed on the bench, `'R'` reworked to +/-3dB symmetric

**Result.** User confirmed on the bench: `'R'`'s amplitude mismatch alone, with `eq` fully off, reproduces the IMD benefit - "that proves the hypothesis." This closes the falsification test proposed in the previous section: the amplitude-mismatch mechanism identified on 2026-09-03 is now confirmed as sufficient on its own, independent of `eq`'s HPF or its ~126µs differential phase-delay side effect, using nothing but a deliberate tone-amplitude offset.

**Nuance the user caught: `eq` boosts, it doesn't attenuate.** `eq`'s presence peak makes tone2 (1900Hz, the upper tone) LOUDER than tone1, not quieter - the +3.60dB@1900Hz vs +0.40dB@700Hz split from the 2026-09-03 "what is `eq` actually doing" section nets to tone2 being the bigger of the two, not the smaller. The original `'R'` implementation could only test the opposite direction (tone2 scaled down, 0 to -20dB) - it happened to still confirm the underlying mechanism (which depends only on the magnitude of `|A1-A2|`, not on which tone is larger - see the derivation in the previous section), but wasn't actually replicating `eq`'s own direction. Worth having on record as a small but genuine gap between "confirms the mechanism" and "reproduces `eq` exactly" - the two are related but not identical claims, and this session's own falsifiability standard is why the distinction surfaced at all rather than being glossed over.

**Rework: range narrowed to a symmetric +/-3dB, both directions now reachable.** Per the user's request ("+/- 3dB is enough"), `TONE_RATIO_PRESETS` (`test_signals.cpp`) is now seven 1dB steps: `-3, -2, -1, 0 (equal), +1, +2, +3` dB, replacing the previous six-step 0-to-(-20dB) down-only list. The wider steps (-6/-10/-20dB) are dropped as unnecessary - the effect was already established well inside +/-3dB, and there's no remaining open question those wider points would have answered. Positive dB entries now let `'R'` reach `eq`'s own boost-tone2 direction directly, closing the nuance above.

**Implementation change: constant combined peak instead of a fixed tone1 + multiplier.** The original down-only version kept tone1 fixed at `TWOTONE_AMPLITUDE` and multiplied tone2 by a gain <= 1.0, which was safe (peak could only shrink) but can't extend to a boost direction without risking `TWOTONE_AMPLITUDE*(1+gain)` exceeding 1.0 and clipping. Reworked to hold the COMBINED constructive-interference peak constant (`2*TWOTONE_AMPLITUDE = 0.9`, unchanged from today) at every ratio, splitting it unevenly instead: for a desired linear ratio `r = tone2/tone1 = 10^(dB/20)`, `tone1 = peak_budget/(1+r)`, `tone2 = r*tone1`. At `r=1` (equal/0dB) this reduces to exactly `0.45/0.45`, bit-identical to today's fixed values, so boot behavior and every existing preset/measurement remain unaffected. At the extremes (+3dB/-3dB) the split is `0.373/0.527` or `0.527/0.373` - always summing to exactly `0.900`, verified numerically (a standalone harness printed the full 7-step cycle: peak stayed 0.90000 to 5 decimal places at every step, and the equal-index split matched `0.45000/0.45000` exactly, as expected from the algebra). This also has a nice side benefit for the comparison itself: every ratio step now drives the same overall RF level, so `'R'` sweeps are directly comparable in level as well as ratio, not just individually clipping-safe.

**API note:** `test_signals_get_tone2_gain()` now derives its return value from the two live amplitude statics (`tone2_amplitude/tone1_amplitude`) rather than being a separately-cached variable, so it can't drift out of sync with what `generate_twotone_sample()` is actually doing - a small robustness improvement made while reworking this, not a behavior change (nothing else in the codebase calls this getter yet).

**Verification:** standalone harness (extracted generator + preset-cycling logic, `gcc -Wall -Wextra`) run end-to-end through all seven presets, confirming: combined peak invariant (0.90000 at every step), correct sign/magnitude at each dB point, and exact match to today's legacy `0.45/0.45` at the equal preset. Not yet re-flashed/re-measured on real hardware with the new range specifically - the earlier down-only range is what was bench-confirmed; the new symmetric range is expected to behave the same (same mechanism, same or smaller now-max magnitude at 3dB vs the old range's much larger extremes) but hasn't itself been re-verified on the bench.

## 2026-09-04, later still — reworked `'R'` re-flashed, real IMD comes back very close to `eq` on: hypothesis closed

**Result.** User re-tested on real hardware with the reworked (peak-conserving, symmetric +/-3dB) `'R'`, in `eq`'s own boost-tone2 direction, and reported IMD "very close to eq on." This is the direct, matching-direction version of the falsification test proposed earlier this session - tone2 louder than tone1, same sense as `eq`'s presence peak actually applies, rather than the earlier confirmatory-but-opposite-direction (tone2-quieter) test. **User's own conclusion: "that answers that one."** Closing this question: the amplitude-mismatch mechanism identified 2026-09-03 fully accounts for `eq`'s IMD benefit on this two-tone signal - reproducible with a plain, independent amplitude offset, no HPF, no presence-peak frequency shaping, no other `eq` side effect required.

**Where this leaves things.** `eq`'s real-world benefit on THIS synthetic two-tone signal is now understood essentially completely: it is the presence peak's incidental amplitude-mismatch side effect, not its intended HPF or tone-shaping function - confirmed both by isolating the mechanism analytically (2026-09-03) and now by reproducing it independently with `'R'` alone, in `eq`'s own direction, on real hardware. Doesn't reopen or contradict anything about `eq`'s value for real mic/speech content (where the HPF's hum/rumble rejection and the presence peak's intended tonal shaping both still matter for reasons unrelated to this two-tone-specific mechanism) - this closes the question specifically for why `eq` measured as the single biggest IMD win on the synthetic two-tone test. The practical "Tune signal" implication from the earlier section now has direct bench support behind it, not just the mechanism-confirms-in-principle status it had before this entry.

**Still open, not addressed by this result:** the minimum mismatch actually needed (this test used +3dB; whether +1dB or +2dB gets most of the way there, useful for picking the smallest offset for a built-in tune signal, hasn't been checked), and whether this generalizes beyond the specific 700/1900Hz pair `'R'` was tested at (a different tone spacing changes the null geometry, though the underlying `|A1-A2|` mechanism should hold regardless).

**2026-09-04, later still: 1dB judged sufficient for practical use, pending a comparative bench check.** User's working expectation is that +1dB (the smallest nonzero step `'R'` offers) is already enough for a practical tune-signal offset, well short of the +3dB used to confirm the mechanism above - consistent with the theory, since ANY nonzero mismatch removes the true-zero condition, and the falsifiable question was always "is the remaining near-zero dip still small enough to matter," not "does a small mismatch work at all." Comparative tests across `'R'`'s range - and likely across other `T` band pairs, per the second open item above - planned for the next session, for completeness rather than because this result is in doubt. No firmware change implied either way: `'R'`'s existing +1dB preset already covers this directly, nothing new needs building to run the comparison.

## 2026-09-05 — the promised comparative sweep, and a genuinely separate finding: 300/500Hz leaks USB into LSB

**The planned comparison.** `Tone_Level_Ratio_IMDs.xlsx` - 3rd-order IMD across all seven `'R'` presets at the 700/1900Hz pair (the default `'t'` pair, `g`/`a`/`A`/`eq`/comp all off). Both sidebands' 3rd-order product improve monotonically moving away from 0dB in either direction (Low: -32.2dB@0dB -> -40.7dB@+3dB -> -34.0dB@-3dB; High: -31.4dB@0dB -> -34.3dB@+3dB -> -42.7dB@-3dB) - each direction preferentially cleans up the IMD product on ITS OWN side (boosting tone2 helps the low-side product more, attenuating tone2 helps the high-side product more), both clearly better than equal at every step in both directions. Consistent with, and a nice quantitative confirmation of, the `|A1-A2|`-only mechanism established 2026-09-03/04 - no new mechanism needed to explain this table.

**Separately, the user's own side note: "all 200Hz spaced tone pairs generate low IMDs at all `'R'` values ~-50dB EXCEPT 300/500 that has a high output at -300Hz from carrier - different issue."** Investigated this because "USB leak to LSB?" is a specific, checkable hypothesis, not just a curiosity - and it checks out cleanly, with a mechanism already half-documented in this file.

**Mechanism, confirmed by direct computation of the live 65-tap Hilbert FIR (`generate_hilbert_coeffs()`, `ssb_dsp.c`, `HILBERT_TAPS=65`, `SAMPLE_RATE_HZ=16000` - both current values).** `ssb_dsp_process_sample()`'s I (direct) branch is a single tap off the same delay line (`float I = handle->delay_line[i_idx]`) - a PURE integer-sample delay, with magnitude exactly 1.0 at every frequency, no exceptions. The Q (Hilbert) branch is the actual 65-tap FIR convolution, which only approximates a unity-magnitude 90-degree phase shift - and, being a Type-III antisymmetric linear-phase FIR, its phase is EXACTLY -90 degrees at every frequency by construction (confirmed numerically: -90.000 degrees at every frequency checked, 100-3700Hz) - so the entire opposite-sideband leakage in this design is a pure amplitude-imbalance problem between I and Q, with no phase-error contribution at all. Computed `|H(f)|` for the Q branch directly from the live coefficients (referenced to the same `center`-tap delay the I branch uses, matching the actual code path exactly):

| Tone (Hz) | `\|H\|` (dB) | Predicted image (opposite-sideband) rejection |
|---|---|---|
| 100 | -7.65dB | 7.7dB |
| 200 | -2.68dB | 16.3dB |
| **300** | **-0.72dB** | **27.6dB** |
| **500** | **+0.03dB** | **54.3dB** |
| 700 | +0.02dB | 61.3dB |
| 900 | -0.01dB | 66.6dB |
| 1500-3700 (all `T` pairs above 700/900) | -0.02 to +0.02dB | 58-67dB |

(Image rejection from amplitude-only I/Q imbalance: `IRR = 20*log10((1+a)/|1-a|)` for linear amplitude ratio `a = |H(f)|` - the standard phasing-method formula, applicable here without a phase term since phase is exact.)

**This exactly explains the reported pattern.** Every other `T` band pair (700/900 through 3500/3700) has BOTH its tones sitting in the >=61dB-rejection region - clean, matching the reported "~-50dB" floor (measurement/noise-floor limited, not Hilbert-FIR limited). The 300/500 pair is the only one with a tone (300Hz specifically, not 500Hz - 500Hz alone is already excellent at 54dB) sitting in the FIR's genuine low-frequency rolloff region, ~27dB predicted image rejection - 25-30dB worse than every other pair, easily enough to produce a distinctly "high" spur report. And the direction matches exactly: this firmware defaults to `SSB_SIDEBAND_USB` (`dsp_state.cpp`), where a positive audio frequency commands a positive `freq_dev` (RF tone above carrier) with no sign flip (`ssb_dsp.c`'s `if (sideband == SSB_SIDEBAND_LSB) freq_dev = -freq_dev;` only applies in LSB mode) - so the 300Hz tone's imperfectly-cancelled image genuinely lands on the LSB side, below carrier, at exactly the reported -300Hz offset. **Confirmed: this is a USB-into-LSB leak, caused by the Hilbert FIR's own magnitude error at 300Hz, not by anything `'R'`/`eq`/`a`/`A`/`g` touch.**

**Correctly a separate, pre-existing issue, exactly as the user's own note called it.** This has nothing to do with the amplitude-mismatch/two-tone-null mechanism this session has been chasing - it's a structural limit of the current 65-tap Hilbert design's low-frequency edge, present since `HILBERT_TAPS` was set, unrelated to `eq`, `'R'`, `a`/`A`, or `g`. It would affect ANY real content near 300Hz and below (a real low male voice fundamental, not just this synthetic test pair) with the same opposite-sideband penalty, regardless of any of this session's other settings.

**Not yet done / options, not yet decided on:** (1) confirm the predicted ~28dB figure against the actual measured spur level (the spreadsheet's note is qualitative - "high output" - no dB figure given); (2) if this matters in practice, `HILBERT_TAPS` could be raised (config.h already notes 129 was "briefly tested" for an unrelated question, so there's precedent this doesn't come free - more taps push the good-flatness point lower in frequency at the cost of more group delay and more per-sample compute, the same tradeoff `relative_delay.h` already discusses); (3) simplest no-code option: avoid placing a `'T'`-style reference/tune tone at or below ~300Hz on this firmware, since image rejection there is genuinely, structurally worse than everywhere else in the band - relevant to the "Tune signal" design discussion in the section above, which should now also account for tone PLACEMENT (avoid <=300Hz), not just amplitude ratio.

## 2026-09-05 — Trial3 ("optimised gd") TFA sweep: does NOT show the a+A candidate's predicted improvement

**Context.** `gaA_Trial3_Optimised_gd_TF.txt` - a real-hardware `'w'` chirp/TFA sweep, presumably captured with `ENV_GDEQ_USE_AA_CANDIDATE=1` (the a+A-specific gdeq refit from the 2026-09-04 entries above) and `a`+`A` both on, following up on the earlier "not yet validated on real hardware for the a+A case" gap. User asked: "does this look as expected?"

**Short answer: no.** Processed with the exact same pipeline validated against Trial1/Trial2 (Savitzky-Golay smoothing, window=181/~4.2kHz span, polyorder=3, applied to the FULL 0-24kHz sweep before restricting to the 100-8000Hz analysis band - restricting first and smoothing second was tried initially and produces a spurious edge artifact right at 8000Hz that inflates p-p to ~228us; smoothing first, as established, is the correct order and is what all figures below use):

| | p-p, 100-8000Hz | Mean delay |
|---|---|---|
| Trial2 (real hardware, default coeffs `a1=0.026173/a2=0.236810`) | 157.5us | 196.4us |
| **Trial3 ("optimised", real hardware)** | **154.8us** | **196.8us** |
| Candidate's own prediction (`a1=a2=-0.139115`, from the 2026-09-04 refit) | 73.0us | 197.1us |

Trial3's p-p (154.8us) is statistically indistinguishable from Trial2's (157.5us) - nowhere near the candidate's predicted 73.0us. Checked further: Trial3's group-delay curve tracks Trial2's MEASURED curve to an RMS of only 2.8us across the whole 100-8000Hz band (well within normal measurement repeatability - compare to the ~2us RMS the original 2026-09-03 fit itself achieved against its own real-hardware validation), while it diverges from the candidate's OWN predicted curve by RMS 48.6us - worst right at 8000Hz, exactly where the candidate was supposed to help most (predicted 217.9us, measured 357.5us - a 140us miss, in the WRONG direction). Directly comparing Trial3's raw phase against Trial2's raw phase (interpolated onto a common grid) confirms this at the source: mean phase difference over 100-8000Hz is only -1.06 degrees (std 1.46 degrees) - far too small to be a genuinely different pair of `ssb_allpass1_t` coefficients (`a1=a2=-0.139115` vs `0.026173/0.236810` predicts tens of degrees of differential phase shift by 8000Hz, not ~1 degree).

**Conclusion: Trial3's phase/group-delay response matches the DEFAULT gdeq coefficients, not the a+A candidate.** This looks like `ENV_GDEQ_USE_AA_CANDIDATE` was NOT actually 1 in the firmware that produced this sweep - most likely the flag wasn't flipped before building, or the build was compiled but not actually reflashed to the board, or the wrong binary was flashed. **Recommended before re-testing:** confirm `ENV_GDEQ_USE_AA_CANDIDATE` reads `1` in the checked-out `envelope_gdeq.h`, do a clean rebuild, reflash, and re-run `'w'` - ideally with a `'P'`/boot-banner check first to confirm `g`/`a`/`A` are all actually on for the sweep, since a config mismatch on any of those would also change the curve shape in ways that could be mistaken for "the candidate didn't work."

**Separate observation, not yet explained: Trial3's magnitude runs ~6-7dB below Trial2's from 300Hz up to ~5500Hz, narrowing to ~1.6-2.8dB by 6700-8000Hz.** This does NOT look like an ampeq shelf-state difference (shelf1/shelf2 are both high-shelves with ~0dB effect below their corners - a shelf-state change would show ~0dB difference at 300-1900Hz and a GROWING gap toward 8000Hz, not a roughly flat gap that SHRINKS toward the top of the band). More consistent with an overall level/gain difference between the two capture sessions (master gain, chirp drive level, or TFA/ADC channel calibration) than with any DSP coefficient change - worth checking that both sweeps used the same `PersistentSettings` (particularly master gain) and the same measurement-chain configuration before drawing any conclusion from the magnitude channel specifically. Not yet investigated further - flagging it here since it's a second reason this file doesn't read as a clean, isolated re-test of the gdeq coefficient change alone.

**Chart:** `gdeq_trial3_candidate_check.html` - five series (bare/no-gdeq reconstruction, default-predicted, Trial2-measured, candidate-predicted, Trial3-measured), makes the mismatch immediately visible: the two measured curves (solid) sit on top of each other and on the default's dashed prediction, while the candidate's own dashed prediction sits well apart from both, especially above 5kHz.

**2026-09-05, confirmed: `ENV_GDEQ_USE_AA_CANDIDATE` was not `1` in the flashed build.** User confirmed directly - the diagnosis above was correct. Trial3 is therefore not a real test of the a+A candidate at all; it's effectively a third repeat measurement of the default coefficients (consistent with how closely it tracked Trial2). **The a+A candidate remains untested on real hardware** - the "not yet validated" status from the 2026-09-04 entries still stands unchanged; nothing about the candidate's predicted 73us p-p or its non-uniform trade-off has been confirmed or refuted by this trial. Next step, when there's a reason to revisit it: flip the flag, verify it reads `1` in the built source before flashing, reflash, and re-run `'w'` - the same reconstructed-curve prediction (`gdeq_refit_a_A_candidate_v7.html`) is still the thing to check the new sweep against.

## 2026-09-05, later same day — corrected Trial3 re-run: a+A candidate CONFIRMED on real hardware

**Context.** User rebuilt/reflashed with `ENV_GDEQ_USE_AA_CANDIDATE` actually set to `1` this time and re-ran `'w'`, producing `Corrected_gaA_Trial3_Optimised_gd_TF.txt` (same format, 1025 points, 0-23998Hz). Question: "better?"

**Yes - this is a clean, positive real-hardware validation.** Processed with the same validated pipeline (smooth full 0-24kHz sweep first with Savitzky-Golay window=181/polyorder=3, then restrict to 100-8000Hz, then differentiate):

| | p-p, 100-8000Hz | Mean delay |
|---|---|---|
| Trial2 (real hardware, default coeffs `a1=0.026173/a2=0.236810`) | 157.5us | 196.4us |
| First Trial3 attempt (flag not actually set - see above, effectively a repeat of Trial2) | 154.8us | 196.8us |
| Candidate's own prediction (`a1=a2=-0.139115`) | 73.0us | 197.1us |
| **Corrected Trial3 (real hardware, `ENV_GDEQ_USE_AA_CANDIDATE=1` confirmed active)** | **87.7us** | **195.7us** |

Corrected Trial3 tracks the candidate's own predicted curve closely: RMS difference from the candidate's predicted curve is just 2.7us across the full 100-8000Hz band, versus RMS 50.1us from the default coefficients' curve (Trial2) and RMS 50.1us from the default coefficients' own prediction - i.e. it now sits firmly with the candidate's prediction and firmly apart from the default's, the reverse of the first Trial3 attempt. Confirmed at the phase level too (not just derived group delay): raw phase for corrected Trial3 differs from Trial2's raw phase by a real, frequency-shaped amount this time - mean -42.4 degrees, std 19.2 degrees, ranging from -67.4 degrees to +11.4 degrees, peaking around -64.5 degrees near 4300Hz. That's on the right order of magnitude for genuinely different `ssb_allpass1_t` coefficients (`a1=a2=-0.139115` vs `0.026173/0.236810`), unlike the first attempt's <2-degree difference which gave away that the flag hadn't taken effect.

Mean delay also matches closely (195.7us measured vs 197.1us predicted, within 1.4us) - so this isn't just a p-p-shape coincidence, the whole curve including its DC-ish offset lines up with the candidate's analytic prediction.

**Conclusion: the a+A gdeq candidate (`a1=a2=-0.139115`) is now validated on real hardware for GROUP DELAY.** It delivers a genuine, substantial dispersion improvement over the default coefficients - measured p-p 87.7us vs Trial2's measured 157.5us, roughly a 1.8x reduction - matching its own predicted 73.0us closely enough (within measurement noise/smoothing bandwidth) to trust the underlying refit math. This is the first successful real-hardware confirmation of this candidate after the earlier misflashed attempt.

**Important scope caveat - this validates group delay, not IMD.** The earlier `aAgdeq_optimise_comparison.xlsx` comparison (2026-09-04/09-05, "No difference!") found no measurable IMD benefit from `ENV_GDEQ_USE_AA_CANDIDATE`. That comparison predates this session's discovery that a flag can silently fail to take effect in a build - it has NOT been checked for the same flag-not-set problem, and given how easily it happened here, that earlier "no IMD difference" finding should now be treated as unconfirmed rather than settled. If the IMD question matters going forward, it's worth re-running that comparison with an explicit flag/build check (e.g. a boot-banner or serial readback confirming which coefficient set is actually compiled in) before trusting the result either way. Until then: group delay dispersion is confirmed improved; whether that translates to an audible/measurable IMD improvement remains open.

**Chart updated:** `gdeq_trial3_candidate_check.html` now shows the corrected Trial3 data (title and legend updated to "Trial3, corrected"; verdict box rewritten with the numbers above). The corrected measured curve (orange solid) now visually tracks the candidate's predicted curve (orange dashed) closely across the whole band, clearly separated from Trial2's measured curve (blue solid) - re-verified via Playwright screenshot in both light and dark mode, including a working hover tooltip showing all five series' values at a given frequency.

## 2026-09-05, later still — bench finding: the a+A candidate needs a real, large `relative_delay_samples` retune (2 → 4.5 samples), bigger than the TFA curve predicts

**Context.** User's own bench report with the a+A candidate now flashed and confirmed active: "The new gdeq makes quite big changes but one is the delay has to be set to 280us (4.5samps) to get near to minimum IMDs." Previous (default-coefficient) bench-tuned optimum for the same `g`+`a`+`A`+`eq` config: 2 samples (125us). **This corrects the previous entry's claim that no `relative_delay_samples` retune would be needed** - that claim was based on the two curves' near-identical MEAN delay over the full 100-8000Hz band (196.4us default vs 195.7us candidate), which turns out not to be the right predictor for this quantity. Real hardware, not the TFA mean, is authoritative here.

**Size of the shift:** 2 samples -> 4.5 samples = **+2.5 samples (+156us)** at SAMPLE_RATE_HZ=16000 (62.5us/sample).

**How far the TFA curve difference goes toward explaining it - checked two ways, both fall well short:**

1. *Local delay at the fundamental tone frequencies (700/1900Hz, the default two-tone pair).* Interpolating both real-hardware TFA-derived group-delay curves (Trial2 default vs. corrected-Trial3 candidate) at the actual tone frequencies:

   | Freq | Default (Trial2) | Candidate (Trial3, corrected) | Diff |
   |---|---|---|---|
   | 500Hz | 143.1us | 206.8us | +63.8us |
   | 700Hz | 147.3us | 210.6us | +63.2us |
   | 1900Hz | 170.4us | 222.2us | +51.8us |
   | 2156Hz | 175.2us | 233.6us | +58.4us |
   | 4300Hz | 178.9us | 174.3us | -4.6us |

   Predicts roughly **+1 sample (+52 to +64us)** - right direction, well short of the +2.5 samples measured.

2. *Local delay at the beat frequency and its harmonics.* Physically more correct for a two-tone signal: the envelope `|z(t)|` and `freq_dev` (`d(angle(z(t)))/dt`) of a two-tone analytic signal are periodic at the DIFFERENCE frequency f2-f1 (1200Hz for 700/1900Hz) and its harmonics, not at f1/f2 themselves - so this is arguably the more relevant place to read the TFA curve. Checked at 1200/2400/3600/4800/6000/7200Hz:

   | Harmonic | Default | Candidate | Diff |
   |---|---|---|---|
   | 1200Hz (1st) | 157.4us | 217.6us | +60.2us |
   | 2400Hz (2nd) | 179.1us | 218.2us | +39.1us |
   | 3600Hz (3rd) | 190.6us | 201.9us | +11.3us |
   | 4800Hz (4th) | 172.8us | 155.8us | -17.1us |
   | 6000Hz (5th) | 214.6us | 175.4us | -39.2us |
   | 7200Hz (6th) | 273.6us | 206.5us | -67.2us |

   This is a WORSE predictor, not better - the sign flips by the 4th harmonic and the plain average across harmonics comes out near zero (-2us unweighted, +25us with 1/n amplitude-falloff weighting). Doesn't get anywhere near +156us, and doesn't cleanly pick a direction either.

**This isn't a new failure mode - the same gap already existed for the default coefficients.** The default gdeq's own bench-optimal relative delay (125us) already sat 71us BELOW its own curve's full-band TFA mean (196.4us) - a ~1.1-sample discrepancy that predates this session. For the candidate, the bench-optimal (281us) now sits 85us ABOVE its curve's TFA mean (195.7us) - a similarly-sized discrepancy, but with the sign flipped. So "read the IMD-optimal relative delay off the TFA group-delay curve" was never a tight prediction even for the already-shipped default coefficients - consistent with `settings.h`'s own header comment describing the TFA-derived figure as "a starting hint... not a substitute for" bench re-tuning.

**Working explanation (not fully confirmed): the thing being time-aligned isn't a stationary tone.** TFA group delay characterizes a linear filter's small-signal delay of a swept SINE - a stationary, narrowband concept. What actually needs aligning between the phase and envelope paths is the sharp `freq_dev` transient at the destructive-interference null (the same broadband, non-stationary event this project's null-crossing/EER investigation has already characterized at length - see the 2026-09-03 entries above). A short, sharp transient has spectral support spread across a wide band, not concentrated at any single frequency or narrow harmonic set, so no single point (or small set of points) on a linear TFA curve should be expected to predict how its alignment optimum moves when the allpass coefficients reshape the delay curve. This is offered as the most plausible mechanism, not a confirmed one - no direct measurement of the null-transient's own effective group-delay-weighting has been done.

**Conclusion:** the a+A candidate's real bench-optimal relative delay is genuinely, substantially different from the default's (+156us / +2.5 samples) - correctly anticipated in DIRECTION by the local-tone-frequency TFA reading, but only explains about a third of the MAGNITUDE. Bench re-tuning (not the TFA curve) remains the authoritative source for `relative_delay_samples` any time `envelope_gdeq`'s coefficients change - this project's own `settings.h` header comment already said as much; this is now a second, larger confirming data point. `relative_delay_samples` for any a+A-candidate preset should be set to the bench value (4.5 samples / 281us), not derived from the TFA mean or local-tone reading.

**2026-09-05, correction (same day, shortly after): the 4.5-sample figure was a measurement artifact - `ENVELOPE_INTERP_FACTOR` (x4 interp) had been left on during that retune.** User: "realised that the x4interp had got switched on. With that off a more sensible 2.83 samples now." **This retracts the "broadband/non-stationary transient" explanation above** - it isn't needed. With the confound removed, the true bench-optimal shift is 2 samples -> 2.83 samples (+0.83 samples, +52us), which matches the LOCAL-TONE-FREQUENCY TFA prediction from earlier in this entry almost exactly: interpolating the real Trial2-vs-corrected-Trial3 group-delay curves at 1900Hz gave +51.8us (+0.829 samples) - predicting 2+0.829 = **2.829 samples**, against the measured 2.83. The simple "read the local TFA group delay at the actual tone frequency, add the difference to the old bench-tuned value" approach was the right one all along; the earlier beat-harmonic reading (which predicted ~0 net shift) was the less relevant one, and the apparent factor-of-2.5 residual that prompted the broadband-transient speculation was entirely explained by the interp-factor mismatch, not a real gap in the group-delay-based prediction method. **`relative_delay_samples` for an a+A-candidate preset: 2.83 samples (~177us), not the earlier 4.5.**

## 2026-09-05, later still — full-spectrum comparison: close-in IMD better, but a much larger far-out spur forest appears with the a+A candidate

**Context.** User uploaded `2gaA_spectra.txt` (default gdeq) and `3gaA_spectra.txt` (a+A candidate), both `g`+`a`+`A`+`eq` on, 700/1900Hz two-tone, real-hardware RF spectra (6740 points, 11.868Hz resolution, 0-80kHz, carrier at a ~25kHz bench test frequency - AD9851 output set low enough to capture directly). User's own read: "the new gdeq fit gives a complex result. In general I would rate it worse even though the close-in IMDs are better. I see this pattern with the white noise into mic too although less obvious."

**Confirmed quantitatively - both halves of that read are real, and they're visible in the same two spectra.** Converted both to dBc (relative to each file's own two-tone level) and located the carrier by the two main tones' spacing (1198.67Hz measured, matching 1900-700=1200Hz almost exactly in both files; fc2=24994.3Hz, fc3=25006.2Hz).

**Close-in classic two-tone IMD (3rd-11th order, `f1-nS`/`f2+nS`, S=1200Hz spacing) - candidate is better at every order checked:**

| Order | Low side | Default (2gaA) | Candidate (3gaA) | Diff | High side | Default | Candidate | Diff |
|---|---|---|---|---|---|---|---|---|
| 3rd | -500Hz | -34.2dBc | -41.5dBc | -7.3dB | 3100Hz | -29.8dBc | -35.7dBc | -5.9dB |
| 5th | -1700Hz | -29.4dBc | -32.9dBc | -3.4dB | 4300Hz | -26.8dBc | -30.3dBc | -3.5dB |
| 7th | -2900Hz | -30.2dBc | -31.9dBc | -1.7dB | 5500Hz | -28.5dBc | -30.3dBc | -1.8dB |
| 9th | -4100Hz | -34.1dBc | -34.6dBc | -0.5dB | 6700Hz | -32.8dBc | -33.1dBc | -0.3dB |
| 11th | -5300Hz | -37.1dBc | -37.2dBc | -0.1dB | 7900Hz | -36.3dBc | -37.9dBc | -1.6dB |

Every one of these ten products is equal or better (more negative) with the candidate - confirms "close-in IMDs are better" directly.

**Far out (roughly 4-16kHz from carrier, both sides) - a much larger, denser forest of additional lines, and it's substantially worse with the candidate.** Binning both spectra's local maxima in 1kHz windows across -20kHz to +14kHz and comparing: in the -16kHz to -8kHz window the candidate's peak levels run **+11 to +19dB higher** than the default's in the same windows (e.g. -15000..-14000Hz: default max -59.0dBc vs. candidate -47.9dBc; -9000..-8000Hz: default -62.0dBc vs. candidate -43.1dBc); similar-sized jumps appear from +4kHz to +10kHz above carrier. The BROADBAND floor between these lines (median level per bin) is essentially unchanged or even very slightly better with the candidate - this is not a raised noise floor, it's specifically that many more discrete lines reach much higher levels in that region. Direct peak-finding in the affected band turned up dozens of these lines in the candidate's spectrum that don't reach the same threshold in the default's (e.g. candidate has resolved lines at -8901, -8094, -7703, -6896, -6504, -5697, -5293, -4902, -4498, -4095Hz offset all above -45dBc, where the default's corresponding region is comparatively sparse).

**These are real intermodulation products of the same two tones, not an unrelated spur.** GCD(700,1900)=100Hz, so every genuine nonlinear product of a 700/1900Hz two-tone signal must land on the 100Hz grid - and the observed fine spacing in both spectra (many lines roughly 95-210Hz apart, superimposed on the familiar n\*1200Hz classic-IMD comb) matches that grid closely. This is consistent with a much richer intermodulation spectrum than the classic low-order polynomial picture predicts - expected from a genuinely sharp, near-discontinuous nonlinearity (the `freq_dev` sign-flip at the destructive-interference null, this project's own long-documented mechanism - see the 2026-09-03 entries above) rather than a smooth polynomial one. A sharp, repeating (once per beat cycle, 1200Hz) transient generates a dense harmonic/intermodulation comb, not just the handful of low-order products a mild nonlinearity would produce.

**Working read: this fits the same trade-off this session already found in the group-delay data, not a new, unrelated problem.** The a+A candidate's refit reduces PEAK-TO-PEAK dispersion over the full 100-8000Hz band, but it does so by moving delay OFF the 5-8kHz region and ONTO the 100-3000Hz region (confirmed twice already - the `relative_delay` local-tone-frequency analysis above, and the beat-harmonic table before that). A worse/different local delay match around the actual null-transient's frequency content could plausibly sharpen or reshape that transient in a way that spreads its intermodulation energy into more and higher lines further from the carrier, even while the classic near-in products (which sit closest to the region the candidate's curve improved) get better. This is offered as a plausible connection to already-established mechanisms, not a proven one - no direct check has been done of how the far-out comb responds to `relative_delay` retuning specifically (as opposed to the classic near-in IMD, which is what the 2.83-sample retune was optimized for).

**Open question, resolved:** were these two spectra captured with `relative_delay_samples` already at each configuration's own bench optimum (2 samples for 2gaA, 2.83 samples for 3gaA)? **User confirmed: "e&oe they were both at their optimised delay settings."** So the far-out forest is NOT a delay-misalignment artifact - it's a genuine, apples-to-apples property of the candidate coefficients at their own correctly-tuned operating point.

**Bottom line so far: not a clean win.** The a+A candidate measurably helps the classic near-in two-tone IMD metric this project has used throughout, and measurably hurts a broader intermodulation comb further out - a genuinely mixed, two-sided result, matching the user's own qualitative read exactly. User also reports the same pattern, "less obvious," with white noise into the mic - consistent with this being a real spectral-shaping trade-off (not a two-tone-specific artifact) rather than a plain regression. **Not recommending the a+A candidate for adoption on this evidence** - the group-delay dispersion improvement and close-in IMD improvement are real, but so is the far-out spectral cost, and this project's own working principle (any single scalar metric - p-p dispersion, or classic low-order IMD - has repeatedly failed to predict overall real-world quality on this architecture) applies here again.

**Chart:** `gaA_spectra_comparison.html` - full overlaid spectra (-20kHz to +14kHz from carrier, dBc), shaded bands marking the close-in (better) and far-out (worse) zones, hover tooltip, plus the close-in IMD table above. Verified via Playwright in both light and dark mode.

## 2026-09-05, later still — can the gdeq filter itself be improved? Checked the design space directly: no, not within this architecture

**Question.** With the delay-misalignment explanation for the far-out spur forest ruled out, user asked: "So can gdeq filter be improved? It obviously now has a big influence."

**First checked whether the far-out degradation is even visible to the linear group-delay model this whole refit is built on.** Computed local slope (d&tau;/df) on the two MEASURED curves (Trial2 default, Trial3-corrected candidate) at several Savitzky-Golay smoothing windows (121/151/181/221/261 points) to check robustness: at every window tested, the candidate's max local slope in the 2.8-4.5kHz zone is 1.2-2.7x steeper than the default's (e.g. at window=181, the validated pipeline's own setting: default 33.6&micro;s/kHz vs. candidate 72.4&micro;s/kHz). This is a real, robust feature of the MEASURED curves, not a smoothing artifact.

**But the ANALYTIC prediction (the same "bare analog+shelf1+shelf2 curve + 2-section allpass" model this whole design process is built on) does NOT show this.** Computed RMS and max local slope of the predicted total curve for both coefficient sets on the actual fit grid used for the a+A refit:

| | p-p | mean | RMS slope | max\|slope\| |
|---|---|---|---|---|
| Default (predicted) | 157.2&micro;s | 196.5&micro;s | 38.1&micro;s/kHz | 156.5&micro;s/kHz |
| a+A candidate (predicted) | 72.4&micro;s | 195.6&micro;s | 34.1&micro;s/kHz | 135.0&micro;s/kHz |

The model says the candidate is EQUAL OR BETTER on every one of these metrics, including local smoothness - the opposite of what the measured curves show in the 2.8-4.5kHz zone specifically. So the group-delay model doesn't reproduce the effect that's apparently driving the far-out spur growth.

**Searched the design space anyway, to see if a smoother alternative exists that the original p-p-only optimization might have missed.** Re-ran the same grid+polish optimization method already used for this project's fits, against the identical target curve, with several different objectives:

- p-p + &lambda;&times;RMS(slope), &lambda; = 0 to 2.0, 2 sections (a1, a2 free, not tied): converges to essentially the SAME point (a&approx;-0.1386) at every &lambda; tested - p-p and smoothness aren't in tension in this parameter space, so there's nothing to trade off.
- Pure RMS-slope minimization (ignoring p-p entirely): best found a&asymp;-0.0604, RMS slope 33.6&micro;s/kHz (barely better than the candidate's own 34.1) but at the cost of WORSE p-p (78.0 vs. 72.4&micro;s) - the candidate is already close enough to this smoothness-only optimum that there's no meaningful headroom left.
- 3-section and 4-section cascades, pure p-p minimization: p-p improves only marginally (72.4 &rarr; 71.1 &rarr; 70.4&micro;s, ~1-2&micro;s each) while MEAN delay balloons (195.6 &rarr; 258.2 &rarr; 320.7&micro;s) - a bad trade, not a free improvement; more sections buy essentially nothing here.

**Conclusion: within this filter architecture (cascaded first-order allpass sections fit against the analog+shelf1+shelf2 phase response), the a+A candidate is already at or extremely close to the Pareto-optimal point for both p-p dispersion and local smoothness simultaneously - there's no better coefficient choice available for a same-kind refit to find.** This means the far-out spur forest is NOT something explainable or fixable by more/better group-delay-domain filter design - the theory this whole design process is built on doesn't predict a difference in the 2.8-4.5kHz zone, so a "smarter" refit using the same theory can't be expected to find or fix whatever is actually happening there.

**What's actually going on is therefore outside this model - most likely one of:** (1) a genuine real-hardware non-ideality or nonlinear interaction specific to these coefficient values that pure linear group-delay theory doesn't capture (something about how the null-crossing transient's actual instantaneous waveform, not just its average delay, interacts with this particular allpass response), or (2) measurement variability between the two separate sweep sessions (Trial2 and Trial3-corrected were captured on different occasions, not back-to-back in one sitting) - this project has already documented real session-to-session/environmental effects elsewhere (PSU noise, interp-tick timing) that could plausibly contribute here too.

**Recommended next step, if this is worth chasing further:** a same-session, same-sitting A/B - flip only `ENV_GDEQ_USE_AA_CANDIDATE`, change nothing else (same warm-up, same bench setup, same few minutes), and re-capture both spectra back-to-back. If the far-out forest gap survives that tighter control, it's confirmed as a real coefficient-dependent effect worth its own investigation (at the waveform level, not the filter-design level, since filter design has been checked and is already near-optimal). If the gap shrinks substantially, session-to-session variability was a bigger contributor than assumed. Either way, redesigning gdeq itself is very unlikely to be the fix - that avenue is now checked and effectively exhausted for this architecture.

## 2026-09-05, later still — `ENV_GDEQ_USE_AA_CANDIDATE` replaced with a live `'G'` runtime toggle, for exactly that recommended same-sitting A/B

**Request:** "Yes make it flipable between gdeq curves. g & G?" - directly enabling the recommended next step above (a same-session A/B) without a reflash between the two coefficient sets, which is what made the earlier delay-misalignment and flag-not-set mixups (this file's own 2026-09-05 entries) possible in the first place - a runtime toggle removes both failure modes at once (no rebuild/reflash to get wrong, and both configs can be captured in one sitting).

**Implementation.** `envelope_gdeq.h`/`.cpp`: the old compile-time-exclusive `#if ENV_GDEQ_USE_AA_CANDIDATE` block (which `#define`d only ONE of `ENV_GDEQ_A1`/`A2` depending on the flag) is replaced with BOTH coefficient pairs always defined - `ENV_GDEQ_A1`/`A2` (default) and `ENV_GDEQ_A1_AA_CANDIDATE`/`A2_AA_CANDIDATE` (candidate) - plus a new `ENV_GDEQ_HAS_AA_CANDIDATE` macro (1 only on `ENV_FILTER_PNP_BC327_ATTN` @ `SAMPLE_RATE_HZ`=16000, the only filter/Fs this candidate was ever fit for; 0 everywhere else, with a harmless fallback definition so the .cpp still compiles). New runtime API: `envelope_gdeq_get_use_aa_candidate()` / `envelope_gdeq_set_use_aa_candidate(bool)` - the setter re-initializes both `ssb_allpass1_t` sections via `ssb_allpass1_init()` (documented cheap/task-context-safe, and it zeroes state as a side effect - same no-stale-x1/y1 reasoning `envelope_gdeq_set_enabled()`'s off->on reset already uses, so switching live never feeds a mismatched coefficient/state pair into the next sample) and is a no-op on builds where `ENV_GDEQ_HAS_AA_CANDIDATE` is 0.

**`'G'`** (serial_commands.cpp, mirrors the `'g'`/`'a'`/`'A'` toggle pattern exactly) switches between the two live, printing the key 2026-09-05 finding (better close-in IMD, worse far-out spur forest, not currently recommended) and the correct `relative_delay` bench-optimum reminder (2 vs. 2.83 samples, g+a+A config) every time it's used, so the caveat is unavoidable on the bench rather than something to remember from a doc. Guarded to a plain "no effect on this build" reply when `ENV_GDEQ_HAS_AA_CANDIDATE` is 0. The `'w'` chirp status line and boot banner were updated to report which coefficient pair is active, same as they already report `'g'`/`'a'`/`'A'`'s on/off state.

**Persisted** via a new trailing `PersistentSettings` field, `env_gdeq_use_aa_candidate` (off/false by default, same convention as every trailing field before it - existing presets' positional initializers zero-fill it, and it's included in the `'P'`-style paste-a-preset-line dump in the correct trailing position). Applied on preset load via `envelope_gdeq_set_use_aa_candidate()`, same reset-on-change handling as the live toggle.

**Not a recommendation change** - `env_gdeq_use_aa_candidate` defaults to off/false, same as every existing preset; this is purely a mechanism change (compile-time flag -> runtime toggle) motivated by the bench workflow, not a signal that the candidate is now trusted. The mixed real-hardware result from the entries above stands.

## 2026-09-05, later still — re-examined the "already Pareto-optimal" conclusion: found a real (if modest) p-p/local-smoothness trade-off the earlier search missed, by making the objective local instead of full-band

**Question:** "Doesn't the gdeq result suggest it's more important to fit the lower freq end more accurately?"

**Short answer: not quite "the low end" specifically, but the intuition behind the question is right - and re-running the design search with a differently-shaped objective did turn up something the previous pass missed.**

**First, checked what the candidate actually does band-by-band**, using the same reconstructed "bare" (analog+shelf1+shelf2, no gdeq) curve from `gdeq_refit_a_A_candidate_v7.html`'s embedded chart data, split into three windows:

| Band | Default p-p | Candidate p-p | Default max slope | Candidate max slope |
|---|---|---|---|---|
| Low, 100-3000Hz | 57.1us | **17.5us** (better) | 28.8us/kHz | 20.3us/kHz (better) |
| Mid, 2800-4500Hz | 20.1us | **53.6us** (worse) | 39.5us/kHz | 63.5us/kHz (worse) |
| High, 4500-8460Hz | 124.9us | **73.0us** (better) | 94.9us/kHz | 73.5us/kHz (better) |

So the candidate isn't uniquely more accurate at the low end - it's dramatically flatter at **both** ends (low AND high), and it buys that by dumping essentially all of its remaining curvature into the 2.8-4.5kHz transition in between (p-p there roughly *2.7x worse* than default, max slope ~1.6x worse) - the same zone this project's own design-search entry above already flagged as where the measured curves' local-slope discrepancy shows up (33.6 -> 72.4us/kHz at the validated Savitzky-Golay window). That's a direct, mechanical consequence of a 2-section same-shape allpass being asked to flatten a full 100-8460Hz band: it has one smooth transition to give, and squeezing both plateaus flatter necessarily narrows and steepens that transition. Not "low end matters more" - more like "the full-band p-p objective already prioritizes both extremes automatically, and that's exactly what's pushing the cost into the middle."

**But this reframes the earlier "no better coefficient exists" conclusion, and it's worth revisiting why.** That conclusion (entry above) came from testing p-p + lambda*RMS(slope-over-the-WHOLE-100-8000Hz-band) and pure RMS-slope-over-the-whole-band objectives, both of which found no meaningful tension with p-p. But a whole-band RMS average dilutes a narrow local spike: the 2.8-4.5kHz zone is only ~1.7kHz wide out of the ~7.9kHz band, so a spike concentrated there barely moves a full-band RMS number even when it's large in absolute terms locally. **Re-ran the search with the objective made local instead of global: minimize max|slope| specifically inside 2800-4500Hz, subject to a p-p budget**, using the same reconstructed bare curve (Savitzky-Golay smoothed, window swept 15/21/31/41 points for robustness), free (a1, a2), grid+polish (Nelder-Mead from a 7x7 grid of starting points):

| p-p budget | Best mid-band max slope found | vs. candidate's own (same window) |
|---|---|---|
| <= candidate's own p-p (~66-70us, window-dependent) | modest gain only (~7% better) | barely moves - candidate is already near-optimal *at its own p-p* |
| <= 85us (roughly +15-20us, candidate's p-p +20-25%, still ~1.7-1.8x better than default's ~153us) | **20-30% lower** than the candidate's own mid-band max slope, consistent across all four smoothing windows tested | e.g. window=21: candidate 48.3 -> 35.9us/kHz; window=41: candidate 39.7 -> 26.7us/kHz |

**So there is a real, previously-missed trade-off - it was hidden by the earlier search's whole-band-averaged objective, not absent.** Giving up a modest slice of the candidate's full-band p-p advantage (roughly 20-25% of it, while still keeping ~1.7-1.8x better p-p than default) buys a genuine 20-30% reduction in local slope specifically in the 2.8-4.5kHz zone implicated in the far-out spur growth. It is not a dramatic fix (the underlying single-transition-shape constraint of a 2-section allpass is still real - this doesn't get anywhere near default's own much-gentler 15.6-31.5us/kHz mid-band slope without giving up most of the p-p win), but it is a genuine, non-trivial lever this project hadn't checked yet.

**Caveats before taking this further:** (1) this used the *reconstructed-from-real-Trial2-sweep* "bare" curve (noisy, then Savitzky-Golay smoothed), not the clean closed-form analog+shelf1+shelf2 model used for the original fits - the resulting best-(a1,a2) pair moved around noticeably across the four smoothing windows tested (e.g. (+0.031,-0.026) at window=15 vs. (-0.116,+0.120) at window=41), so it is NOT yet a stable, bench-ready candidate the way the original a+A fit was (that one was confirmed via both grid search and Nelder-Mead polish converging to the same point). (2) This only checks the linear group-delay MODEL's local slope - same caveat as the entry above: the model doesn't fully reproduce the measured 2.8-4.5kHz discrepancy's *size*, only its *direction*, so a coefficient pair chosen purely to minimize modeled local slope there is a reasonable hypothesis, not a guarantee it fixes the real spur forest.

**If this is worth pursuing:** redo this same local-max-slope-vs-p-p Pareto search against the clean analytic analog+shelf1+shelf2 model (not this noisy reconstruction) to get a stable (a1,a2) pair at a chosen p-p budget (~85-90us looks like a reasonable knee), then bench-validate it the same way the current a+A candidate was (TFA sweep, `relative_delay` retune, and - now that `'G'` exists - a same-sitting spectrum comparison against both existing configs) before considering it a third `'G'`-style option. Not done yet - parked pending go-ahead.

## 2026-09-05, later still — same-sitting `'G'` A/B confirms the far-out spur gap is real (not session variability); chirp sweep improved ahead of the next fit attempt

**Same-sitting A/B result.** User: "the g/G switch clearly confirms previous data. Nothing else in the way." This resolves the open question the design-search entry above left hanging - option (2) (session-to-session measurement variability between the separately-captured Trial2/Trial3-corrected sweeps) is ruled out. **The far-out spur forest is a genuine, real, coefficient-dependent property of the a+A candidate**, not an artifact of comparing two different bench sessions. This sharpens the case for another fit attempt (the mid-band Pareto trade-off found above) being worth pursuing on real hardware once a stable coefficient pair is found - the effect it would be trying to fix is now confirmed real, not a maybe.

**User offered to improve the TFA sweep for the next fit attempt:** "I'd say its worth another fit search. Do you need smoother TF data? I could work on that. A slower sweep would be helpful and also do away with the zero wait, maybe sweep up and down rather than the big jump."

**Yes - cleaner TF data would directly help.** The mid-band Pareto search above used the 89-point downsampled curve embedded in `gdeq_refit_a_A_candidate_v7.html`'s chart data (~93Hz spacing), coarser than a full raw TFA sweep (`Corrected_gaA_Trial3_Optimised_gd_TF.txt` was 1025 points over 0-23998Hz, ~23Hz spacing) - and even at that coarser resolution, the best-fit (a1,a2) pair moved around visibly depending on the smoothing window applied to tame the real measurement noise. A cleaner sweep (and ideally the full-resolution raw sweep file, not a downsampled chart export) directly attacks the reason that fit was unstable.

**Implemented all three chirp-sweep improvements requested** (`config.h`, `test_signals.cpp`, status lines in `serial_commands.cpp`'s `'w'` handler and the boot banner):

1. **Slower sweep**: `CHIRP_SWEEP_SEC` 5.0s -> 15.0s (per leg - see below). A slower sweep means more fast-ticks (more sine cycles) elapse per Hz of frequency change everywhere in the band, giving the external rig's own measurement/averaging more settled data per point. Purely a `#define`, easy to retune further against the rig's own integration time.
2. **Sweep up AND down** (`CHIRP_BIDIRECTIONAL`, new macro, default 1): the sweep now goes `CHIRP_F0_HZ` -> `CHIRP_F1_HZ` -> back down to `CHIRP_F0_HZ` before muting/restarting, instead of jumping straight back to `CHIRP_F0_HZ` at the top of every repeat. Implementation mirrors the same logarithmic time-to-frequency mapping on the way down (`test_signals_generate_chirp()`'s new `t_frac_sec`), so instantaneous frequency is continuous across the whole up+down cycle. **Correction to first-pass reasoning:** the old one-directional jump was never actually an audible/analog discontinuity in the RSET output, since it happened right as the envelope was about to be forced to 0 for `CHIRP_MUTE_SEC` anyway (mute doesn't care which direction the sweep was going) - so this isn't fixing a glitch. The real benefit is **two independent phase readings of the same frequency per cycle** (once going up, once going down) - comparing them is a free, direct check for any timing/sync lag baked into the measurement chain itself (a real filter effect should give the same reading both directions; a consistent up-vs-down gap points at the rig/pipeline, not the filter). Set `CHIRP_BIDIRECTIONAL` to 0 to restore the old single-leg sweep if a downstream analysis script assumes one.
3. **"Zero wait"**: `CHIRP_MUTE_SEC` was already a tunable `#define` (currently 0.03s / 30ms) - no architecture change needed to adjust it. **Not removed outright**, because the code comment (and this file's own history) documents it as the sync marker the external rig triggers on to detect "sweep restarting here" - I don't have visibility into the specific ADC-based TF rig's own trigger logic, so whether it's safe to shrink or zero this is a call for whoever operates that rig, not something firmware alone can confirm. Worth trying a reduced value (rather than a hard 0) first and confirming the rig still syncs reliably.

**Status lines updated to match:** the `'w'` reply and boot banner now report total sweep time (both legs when bidirectional) and whether the sweep is "up+down" or "up only", instead of silently reporting only one leg's duration.

**Not yet done:** the actual re-fit against a clean/full-resolution sweep once new data comes in - still pending the improved `'w'` capture, then redoing the local-max-slope-vs-p-p Pareto search from the entry above (ideally against the clean analytic model rather than a reconstructed-from-sweep curve) to get a stable (a1,a2) pair before it's bench-tested as a third `'G'`-style option.

## 2026-09-05, later still — added a linear sweep option; flagged a real gotcha before using it as-is

**Question:** "is there a linear sweep option?"

**Added `CHIRP_SWEEP_LOG`** (config.h, default 1 = log, same as before; set to 0 for linear). Log law is `f(t) = f0*(f1/f0)^(t/T)` (equal time per octave); linear is `f(t) = f0 + (f1-f0)*(t/T)` (equal time per Hz). Both bidirectional legs (up/down) use whichever law is selected, mirrored the same way as before.

**Worth flagging before flipping it: with the current `CHIRP_F0_HZ`/`CHIRP_F1_HZ` endpoints (20Hz-20000Hz), switching to linear makes resolution in the band that matters WORSE, not better.** A log sweep across 20-20000Hz already spends ~87% of its dwell time below 8000Hz, because most of that range's ~10 octaves fall below 8kHz - it's already fairly well-matched to the 100-8000Hz audio band this project cares about. A linear sweep across the SAME 20-20000Hz endpoints would spend only ~40% of its time below 8000Hz, since 8000-20Hz is under 40% of the full 20-20000Hz linear span - most of the sweep would go to the 8-20kHz region, which is mostly outside the audio band and not where the mid-band (2.8-4.5kHz) refit work needs resolution. **So if the goal is more even (or specifically more mid-band) dwell time for the gdeq refit, `CHIRP_F1_HZ` should come down to match the actual band of interest (e.g. 8000Hz) at the same time `CHIRP_SWEEP_LOG` is set to 0** - flipping the law alone, with the wideband endpoints left in place, would hand back less useful data than the existing log sweep, not more. Left as two independent `#define`s (not coupled) since the wideband log sweep is still useful for general filter characterization work outside this specific refit.

## 2026-09-06 — found and fixed a real bug: gdeq/ampeq in the `'w'` chirp path were running at 4x their design rate, silently shrinking `a`/`A`/`g`'s effect on any sweep

**Report:** "I enabled the x4 interp in code (off for I cmd) to get higher Fs for w test but dont have the a+A in w test circuit?"

**Root cause found and confirmed by direct calculation.** `envelope_gdeq_process()`/`envelope_ampeq_process()`'s coefficients (`ssb_allpass1_t`'s `a`, the ampeq shelf biquads) are fit assuming they're clocked at `SAMPLE_RATE_HZ` - an IIR filter's real-Hz response depends on normalized frequency (real Hz / the ACTUAL clock rate it's run at), not on the coefficient alone. `ssb_mic_test.ino`'s `AUDIO_SRC_CHIRP` branch was calling both on **every fast tick** (`ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ`, nominally 64kHz at the stock `ENVELOPE_INTERP_FACTOR=4`) rather than only on `is_full_tick` (`SAMPLE_RATE_HZ`, 16kHz) the way the NORMAL production pipeline's own gdeq/ampeq calls already correctly do. Quantified directly (same `gd_samples(f,a)` model used throughout this file):

| | Fs=16000 (intended) | Fs=64000 (what the `'w'` path was actually running at) |
|---|---|---|
| a+A candidate p-p, 100-8000Hz | 70.9us | **4.1us** |
| Default coefficients p-p, 100-8000Hz | 69.3us | **1.2us** |

Running the exact same coefficients 4x faster than designed doesn't just make them "faster" - it collapses their effect on a 100-8000Hz sweep by roughly 17-58x. The ampeq shelf biquads' real corner frequency shifts by the same 4x factor, pushing their boost mostly outside a 100-8000Hz sweep window too. **This fully explains the reported symptom without needing `a`/`A` to have been literally disconnected from the circuit** - both calls are still there; their effect on the sweep just gets rate-warped down toward invisible once the chirp path's fast-tick rate rises (which is exactly what raising `ENVELOPE_INTERP_FACTOR`/enabling "x4 interp" does, regardless of the separate, unrelated `'I'` runtime toggle - `'I'` only controls whether the interp-tick PWM-smoothing math does anything for the NORMAL pipeline, it has no bearing on the chirp path's own call rate).

**Fixed** (`ssb_mic_test.ino`): the chirp branch's `envelope_gdeq_process()`/`envelope_ampeq_process()` calls (and the DC mapping right after them) now run only on `is_full_tick`, handing off to `envelope_interp_on_full_tick()`/`envelope_interp_on_interp_tick()` - the same PWM-smoothing machinery the normal pipeline already uses - instead of a direct `envelope_output_write_pwm()` call every fast tick. This also means `'I'` now controls the `'w'` test's PWM smoothness the same way it does real playback, cleanly decoupled from gdeq/ampeq's own fixed `SAMPLE_RATE_HZ` processing rate - the two concerns this bug had conflated. The raw chirp waveform generation itself is unchanged (still every fast tick, so sweeping to `CHIRP_F1_HZ`=20kHz is unaffected) - only what gdeq/ampeq actually see is now decimated to match their design rate.

**Honest open question, not yet resolved:** this bug's mechanism (chirp calling gdeq/ampeq every fast tick) has been in the code since the 2026-09-02 comment that first wired gdeq into the chirp path - yet the historical `'w'`-based validations since then (Trial1/Trial2/Corrected-Trial3, all 2026-09-03 through 09-05) matched the `Fs=16000`-based predictions extremely closely (e.g. corrected Trial3: 87.7us measured vs. 73.0us predicted, RMS 2.7us off the predicted curve shape) - which shouldn't have been possible if those specific sweeps were already running gdeq at 64kHz, per the table above. Two ways to reconcile, neither confirmed: (1) `ENVELOPE_INTERP_FACTOR` may have been a different, smaller value (plausibly 1) during those earlier captures, only later raised to 4 as part of the (separately still-unvalidated) `envelope_interp.h` interpolation feature; or (2) something else about how those specific builds were configured differed in a way this file doesn't have a record of. Flagging this rather than asserting a clean story - if it matters, checking build history/dates for when `ENVELOPE_INTERP_FACTOR` became 4 would settle it, but isn't necessary to move forward: the fix above is correct regardless of how the older sweeps happened to avoid the symptom.

**Not yet bench-validated** - same not-yet-confirmed status as `envelope_interp.h`'s own interpolation feature, which this fix now shares plumbing with. Recommended check: re-run `'w'` with `'a'`/`'A'` toggled and confirm their effect on the amplitude channel is visibly restored (and no longer changes when `ENVELOPE_INTERP_FACTOR` is raised further) before trusting any new sweep captured through this path for the mid-band gdeq refit work above.

## 2026-09-06, later same day — a+A confirmed restored, but a genuine bug found in the fix's first draft; also the most likely explanation for the reported waveform discontinuity

**User confirmed:** "It could be that we only measured a+A at 16kFs but it was making the higher res capture more difficult." - agreeing the 4x-rate bug above is the likely explanation for why the a+A candidate's historical validation happened to still look clean (i.e. probably captured at `ENVELOPE_INTERP_FACTOR`'s original, smaller value, before it was raised for a higher-res capture attempt). Then: "a+A in circuit now but the sweep wave form has discontinuities, looks likes it is skipping sample count?"

**Found and fixed a real bug in yesterday's fix.** `envelope_interp_on_full_tick(envelope, tick_start_us)` requires `tick_start_us` to be `esp_timer_get_time()` captured at the TRUE top of the tick, BEFORE any DSP work - `envelope_interp.h`'s own v4.1 design note is explicit that this is a real, previously-hunted-down bug class here: a late timestamp makes `compute_ramp_value()`'s ramp timing wrong (not just imprecise), because the ramp fraction is derived from `esp_timer_get_time() - s_tick_start_us`. The first draft of the chirp fix captured `chirp_tick_start_us` AFTER running gdeq/ampeq/DC-mapping - exactly the mistake that note warns against. **Fixed:** the timestamp is now captured at the very top of the `AUDIO_SRC_CHIRP` branch, before `test_signals_generate_chirp()` or anything else that tick, matching how the normal pipeline's own `t_start_us` is captured.

**However, this bug only matters when `'I'` is ON** - checked `envelope_interp.cpp` directly: when disabled, `envelope_interp_on_full_tick()` returns early (`s_last_value = envelope; envelope_output_write_pwm(envelope); return;`) without touching any ramp math, and `envelope_interp_on_interp_tick()` does nothing at all on interp ticks when disabled - no ramp is ever computed, so a mistimed `tick_start_us` couldn't have caused a visible glitch while `'I'` is off (as reported - "off for I cmd" in the earlier message). So the late-timestamp bug is now fixed as a real latent issue for whenever `'I'` gets turned on, but it's very unlikely to be the actual cause of the reported discontinuity right now.

**More likely explanation for the reported symptom: this is the fix working as designed, not a new bug.** Before yesterday's fix, the chirp path wrote a fresh PWM value on EVERY fast tick (64kHz) unconditionally, regardless of `'I'`. After the fix, with `'I'` off, PWM only updates once per full tick (`SAMPLE_RATE_HZ`, 16kHz) - a real, intentional 4x coarsening of the output staircase, because that's what correctly matches the rate gdeq/ampeq actually process at. Going from a 64kHz-updated to a 16kHz-updated ZOH staircase would look exactly like new "discontinuities" or "skipped samples" on a scope, even though it's the intended, correct behavior now (this is literally the SAME baseline ZOH the normal production pipeline has always had with `'I'` off - see `envelope_interp.h`'s own header: "Right now the commanded PWM duty only changes once per DSP tick... a classic zero-order-hold staircase"). **The fix for this is `'I'` ON** - before yesterday's change `'I'` had zero effect on the chirp test (chirp bypassed it entirely); now it correctly restores smooth, interpolated PWM output for `'w'` too, the same way it already works for real playback, at the cost of one extra sample period (~62.5us) of added delay per `envelope_interp.h`'s own documented tradeoff.

**Not fully closed:** haven't ruled out that "the sweep waveform" in the report refers to something other than the RSET/PWM analog output on a scope (e.g. the reference square wave, or discontinuities visible in the recorded/captured TF data itself rather than the live analog signal) - if turning `'I'` on doesn't resolve what's being seen, that would point at something else and is worth a fresh, specific description of exactly what's being viewed and where.

## 2026-09-06, later same day — a sharper, more complete explanation: gdeq/ampeq CANNOT be meaningfully characterized above 8000Hz at all, and that's very likely the actual discontinuity

**User's challenge, verbatim:** "Are we kidding ourselves here? If the a+A filters are at 16kFs surely we are not going to get any sense out of them at 20kHz sine input?" **Yes - correct, and this is sharper/more complete than the 'I'-off ZOH-coarsening explanation offered a moment ago.**

**This is a hard Nyquist limit, not a bug, and no firmware fix can get around it.** `SAMPLE_RATE_HZ`=16000 means gdeq/ampeq's Nyquist is 8000Hz, full stop - a digital filter clocked at 16kHz cannot meaningfully respond to real signal content above 8kHz, by definition, regardless of how carefully it's rate-matched. Yesterday's fix correctly gated gdeq/ampeq's calls to `is_full_tick` (genuinely 16kHz) instead of every fast tick (64kHz) - but that fix only corrects the RATE gdeq/ampeq are clocked at; it does nothing (and cannot do anything) about what happens once the swept chirp's real frequency crosses 8kHz, which `CHIRP_F1_HZ`=20000 sweeps well past. Above 8kHz, the full-tick-sampled version of the chirp envelope genuinely ALIASES - the samples gdeq/ampeq actually see fold back to a spurious, jagged, discontinuous-looking apparent frequency, not the true swept one. This is very likely a MORE COMPLETE explanation for the reported waveform discontinuity than the plain "16kHz ZOH now visible with 'I' off" explanation given a moment ago: the folding/aliasing artifact should appear specifically in the upper portion of the sweep (above 8kHz - with the current log sweep, `CHIRP_F0_HZ`=20/`CHIRP_F1_HZ`=20000, that's roughly the last ~13% of each leg's sweep time), not smoothly throughout - worth checking whether the discontinuity is confined to (or much worse in) that region, which would confirm this is the actual mechanism rather than (or in addition to) the ZOH-coarsening explanation.

**The `'w'` test has always actually had TWO distinct, previously-conflated purposes, and this finally separates them cleanly:**

1. **Characterizing the bare ANALOG reconstruction filter (Sallen-Key/RSET) alone**, `'g'`/`'a'`/`'A'` all OFF. This circuit is continuous-time, not bound by `SAMPLE_RATE_HZ`'s digital Nyquist at all - it can and does respond meaningfully to real frequencies up to 20kHz and beyond. `CHIRP_F1_HZ`=20000 and the whole reason for wanting a higher chirp "Fs" (pushing the PWM ZOH's own spectral images further out) exists FOR THIS purpose - every "bare curve" reconstruction this project has done (e.g. `gdeq_refit_a_A_candidate_v7.html`'s dashed series) is exactly this kind of measurement.
2. **Characterizing gdeq/ampeq's own digital correction** (`'g'`/`'a'`/`'A'` ON), which is hard-capped at 8000Hz by `SAMPLE_RATE_HZ` - this was ALREADY true of every fit and analysis in this entire project (the 100-8000Hz band used throughout `refit_chart_data`/all TFA analyses is not an arbitrary choice, it's exactly this Nyquist limit), it's just that until yesterday's fix, the firmware was silently producing WRONGLY-SCALED-BUT-SMOOTH data above 8kHz (the 4x-rate bug) instead of correctly-ALIASED-BUT-JAGGED data - neither is informative about gdeq above 8kHz, but the new one looks obviously broken on a scope, which is arguably a feature (it stops looking deceptively clean) even though it's alarming to see.

**Practical recommendation: when `'g'`/`'a'`/`'A'` are engaged, only trust `'w'` data below 8000Hz - full stop, no code fix changes this.** For a sweep aimed at the mid-band gdeq refit work (2.8-4.5kHz), the 8-20kHz portion is now not just useless but actively confusing to look at, so it's worth lowering `CHIRP_F1_HZ` to 8000 (or a bit under, e.g. 7900, to stay clear of the Nyquist edge itself) specifically for gdeq/ampeq-focused sweeps - this also ties back to the earlier linear-sweep-endpoint discussion (a narrower `CHIRP_F1_HZ` was already flagged there as the right move for concentrating resolution in the useful band). Keep `CHIRP_F1_HZ`=20000 for bare-analog-filter-alone sweeps (`'g'`/`'a'`/`'A'` off), where the higher ceiling is genuinely meaningful. Not yet implemented as a firmware toggle (e.g. auto-selecting the ceiling based on `'g'`/`'a'`/`'A'` state) - parked pending go-ahead, since manually editing `CHIRP_F1_HZ` between the two use cases is simple enough for now.

## 2026-09-06, later same day — a THIRD, independent cause found: reverting to x1 interp for the 8kHz-capped sweep reintroduced a different Nyquist problem, this time in the raw chirp generator itself

**Report:** "So I'm now back to x1 interp and 20-8k sweep. I now see a strange signal on the digital output - it is jumping in freq, like pwm changes, whereas it should be nice and steady as the sweep progresses. Worse at HF."

**Going back to `ENVELOPE_INTERP_FACTOR=1` was the wrong move, and it's a different mechanism from either of the last two entries.** `test_signals_generate_chirp()` generates the chirp sine directly, sample by sample, via a phase accumulator: `s_chirp_phase += 2*pi*f_inst/fs_fast`, where `fs_fast = SAMPLE_RATE_HZ * ENVELOPE_INTERP_FACTOR`. This is the RAW WAVEFORM GENERATION step - it happens before gdeq/ampeq ever see anything, and it has nothing to do with either of the two Nyquist limits already documented in this file (gdeq/ampeq's own 8kHz digital-EQ ceiling, or the analog filter's 20kHz continuous-time-only ceiling). It has its OWN Nyquist limit: `fs_fast/2`.

**Verified numerically** (samples/cycle and phase increment at the sweep's own top frequency, `CHIRP_F1_HZ=8000Hz`, for different `ENVELOPE_INTERP_FACTOR` values):

| `ENVELOPE_INTERP_FACTOR` | `fs_fast` | samples/cycle @ 8000Hz | `fs_fast` Nyquist | margin below Nyquist | phase increment @ 8000Hz |
|---|---|---|---|---|---|
| 1 (what the report used) | 16000Hz | **2.00** | 8000Hz | **0%** | 180.0 deg/sample |
| 2 | 32000Hz | 4.00 | 16000Hz | 50% | 90.0 deg/sample |
| 4 (stock) | 64000Hz | 8.00 | 32000Hz | 75% | 45.0 deg/sample |

At factor=1, the top of the (now correctly 8kHz-capped) sweep sits EXACTLY on `fs_fast`'s own Nyquist - only 2 samples per cycle, zero margin. A direct phase-accumulator sine at exactly 2 samples/cycle is a degenerate case (alternating +1/-1, `phase increment = 180 deg` exactly) - any tiny perturbation in `f_inst` from one sample to the next (which a continuously-sweeping chirp always has, by design) swings the apparent instantaneous frequency wildly, because there's no averaging margin left. Confirmed by simulating the actual generator (float32, matching the firmware's `sinf()`/phase-accumulator arithmetic) near the sweep top and measuring zero-crossing-interval-based instantaneous frequency: at factor=1, the estimated frequency's standard deviation was **1257Hz** (min 4000Hz, max 8000Hz - i.e. wildly unsteady) vs. **480Hz** at factor=4 (min 6400Hz, max 8000Hz - far tighter, though not perfectly steady either, which is expected near any sweep's endpoint even with margin). This directly matches the reported symptom: "jumping in freq, like pwm changes... worse at HF" - worse at HF is exactly what a Nyquist-margin problem predicts (the closer `f_inst` gets to `fs_fast/2`, the worse it gets), and "like pwm changes" matches a jittery instantaneous-frequency reading showing up as apparent duty-cycle noise on the digital output.

**This is a genuinely separate, independent problem from gdeq/ampeq's own ceiling - and importantly, there is no longer any reason to trade one off against the other.** Before the 2026-09-06 `is_full_tick` fix (two entries above), raising `ENVELOPE_INTERP_FACTOR` for a cleaner raw sweep silently broke gdeq/ampeq (ran them 4x too fast, collapsing their effect). That coupling is now gone: `is_full_tick` gating means gdeq/ampeq always see genuine `SAMPLE_RATE_HZ`-rate samples regardless of what `ENVELOPE_INTERP_FACTOR` is set to. So `ENVELOPE_INTERP_FACTOR` can be raised back up purely for the raw generator's own oversampling margin, with zero effect on gdeq/ampeq's correctness.

**Recommendation: `ENVELOPE_INTERP_FACTOR=4` (its stock value) + `CHIRP_F1_HZ=8000` (already the right value for gdeq/ampeq-focused sweeps, per the entry above) - both at once, not a trade-off between them.** This gives 8 samples/cycle (75% Nyquist margin) at the sweep's top for a clean raw waveform, AND correctly rate-matched gdeq/ampeq processing - resolving the aliasing concern from two entries above and this frequency-jumping symptom simultaneously. No firmware code change needed for this specific fix (the `is_full_tick` gating already handles the gdeq/ampeq side) - purely a matter of the user's own local `ENVELOPE_INTERP_FACTOR` edit (`envelope_interp.h`) going back to 4. `config.h`'s `CHIRP_F1_HZ` updated to 8000.0f (from 20000.0f) to match, with a comment explaining both this and the earlier Nyquist-ceiling finding; bump it back to 20000.0f only for a bare-analog-filter-only sweep (`'g'`/`'a'`/`'A'` all off).

**Secondary, likely minor, compounding factor also present at factor=1:** the LEDC PWM timer's own `resolution_hz = 2000000UL * ENVELOPE_INTERP_FACTOR` (`ssb_mic_test.ino`, line ~672) also scales with `ENVELOPE_INTERP_FACTOR` - at factor=1 the PWM duty register's quantization step is 4x coarser than at factor=4, which could add its own small amount of "steppy"/quantized-looking artifact on top of the primary Nyquist-margin cause above. Not separately quantified (the Nyquist-margin effect is large enough on its own to explain the reported symptom), but worth knowing about if any residual jumpiness remains after restoring factor=4.

**General principle worth keeping in mind going forward:** `ENVELOPE_INTERP_FACTOR` and `CHIRP_F1_HZ` are now two independent knobs serving two independent purposes - `ENVELOPE_INTERP_FACTOR` controls how well-oversampled the RAW chirp waveform generation is (higher is better, no downside found so far for the chirp path specifically), while `CHIRP_F1_HZ` controls how far the sweep goes before gdeq/ampeq's fixed 8kHz digital ceiling (when `'g'`/`'a'`/`'A'` are on) or the analog filter's own bandwidth (when they're off) makes going further pointless. There's no longer a reason to lower `ENVELOPE_INTERP_FACTOR` for any chirp-test purpose.

## 2026-09-06, later still — `HiRes_aA_TF.txt`: the fix worked (clean sweep, no in-band jitter), but the resulting curve's SHAPE matches the DEFAULT gdeq coefficients, not the a+A candidate

**User uploaded `HiRes_aA_TF.txt` and asked "OK how does this look?"** - the first sweep captured after the `ENVELOPE_INTERP_FACTOR=4` + `CHIRP_F1_HZ=8000` fix above.

**Good news first: the fix is confirmed working.** 1365 points at ~5.86Hz spacing (0-7998Hz) - 4x finer than any prior sweep in this project (~23.4Hz spacing). Checked the raw phase/magnitude sample-by-sample through the 35-7900Hz range: smooth, monotonic, no discontinuities, no jitter anywhere in that span - a dramatic improvement over the "jumping in freq" symptom two entries above. The only rough regions are right at the two ends: below ~35Hz (huge phase/magnitude swings - 0deg/-132deg/-59deg/-66deg/-33deg over the first five points, magnitude bouncing 0.30-0.55) and above ~7900Hz (magnitude sinking to -22 to -24dB and phase getting visibly noisier in the last ~15 points, one point even reversing back upward to -20.7dB right at 7997.9Hz). **Both of these are classic TF-rig near-edge coherence artifacts already seen on every previous sweep in this project (near-DC leakage, and reduced SNR right at a sweep's own boundary)** - not a new problem, and not related to the chirp generator's own Nyquist margin fix. Naively running a ~700Hz-span Savitzky-Golay filter across the FULL 0-8000Hz array let this edge garbage bleed into the derivative for a good ~350-450Hz on each side (the filter's boundary-extrapolation mode uses the polynomial fit through the edge samples, including the bad ones) - produced a nonsense p-p of over 800us until the raw array was trimmed to the clean 35-7900Hz sub-range *before* smoothing, not just after. Worth remembering for next time: trim known-bad edge samples from the array first, then smooth/differentiate, then trim again for the reporting band - trimming only at the end doesn't stop contamination.

**Less good news: the resulting group-delay SHAPE doesn't match the a+A candidate.** Compared the trimmed/smoothed curve (100-8000Hz windowed Savitzky-Golay, same pipeline used throughout this project) against the two known real-hardware reference shapes, mean-removed (so only shape counts, not any constant rig/cable offset):

| Reference (real hardware, same a+A ampeq state) | Correlation with `HiRes_aA_TF.txt` | RMS difference (mean-matched) |
|---|---|---|
| `Corrected_gaA_Trial3_Optimised_gd_TF.txt` (a+A **candidate** gdeq, confirmed-good capture, 2026-09-05) | **0.463** | 34.0us |
| `ga_Trial2_TF.txt` (**default** gdeq coefficients, a+A on, 2026-09-04) | **0.877** | 22.8us |

The new sweep's shape tracks the DEFAULT gdeq curve far better than the candidate's - not a subtle difference, 0.877 vs. 0.463 correlation over the same 398-7547Hz shared band. Measured p-p (edge-trimmed, ~114-119us depending on smoothing window used) also sits closer to the ballpark of default-coefficient runs (Trial2: 157.5us on the old coarser-resolution pipeline) than the candidate's (Corrected_Trial3: 87.7us) - though not an exact match to either, which is expected given genuinely finer smoothing/resolution here. **Working conclusion: `'G'` (the live gdeq coefficient toggle) most likely was NOT actually switched to the a+A candidate for this capture** - the same class of mistake as the 2026-09-05 "Flag wasnt '1' in code" incident, just via the newer runtime `'G'` toggle instead of the old compile-time `ENV_GDEQ_USE_AA_CANDIDATE` flag. Not 100% certain from the data alone (can't rule out some other real difference in this session's config), but the shape evidence is strong enough to be worth checking before reading anything into this sweep's mid-band behavior.

**Recommendation before drawing any conclusions from this sweep for the mid-band refit work:** re-check `'G'`'s state directly - the `'w'` reply text already prints which gdeq set is active - and recapture if it turns out to have been on the default set. Chart comparing all three curves (mean-removed, hover tooltip): `hires_aA_shape_check.html`.

Magnitude was also checked as a secondary signal but is less conclusive either way: net rolloff from ~200Hz to ~7500Hz was -7.3dB for the new sweep vs. -6.5dB for Trial2 (default gdeq) vs. only -3.6dB for Corrected_Trial3 (candidate) - roughly consistent with the same "probably default gdeq" read, but gdeq itself is a pure allpass (unity magnitude by construction) and shouldn't touch magnitude at all, so this difference more likely reflects ampeq shelf state or session-to-session rig calibration (already flagged as a real, unexplained effect in the 2026-09-05 Trial3 entry) rather than being directly diagnostic of which gdeq coefficients were active. The phase-shape correlation above is the reliable signal here, not the magnitude.

## 2026-09-06, correction, shortly after — user confirmed: `'g'` was OFF entirely, only `'a'`+`'A'` were on. Re-checked against the right reference (the bare/no-gdeq curve) - excellent match.

**User: "the sweep was only a+A enabled."** Retracting the "'G' probably didn't get toggled to the candidate" conclusion just above - `'g'` wasn't just on-the-wrong-coefficients, it was off entirely, so the correct comparison target isn't `ga_Trial2_TF.txt` (default gdeq) or `Corrected_gaA_Trial3_Optimised_gd_TF.txt` (candidate gdeq) - it's the **bare** analog+shelf1+shelf2 curve with no gdeq allpass processing at all, i.e. `gdeq_refit_a_A_candidate_v7.html`'s own `DATA.bare` array (the reconstructed-from-real-Trial2-data target curve every gdeq fit in this project has been trying to flatten).

**Re-ran the shape comparison against `DATA.bare` instead - a genuinely good match:** correlation **0.940**, RMS **12.3us** (mean-removed, same 700Hz-window Savitzky-Golay pipeline), over the shared 387-7547Hz band - robust across smoothing window choice too (0.90-0.95 correlation across 350-900Hz windows tried). For contrast, correlation against the a+A candidate's predicted POST-equalization shape (`DATA.refit`) is only 0.383 (RMS 30.0us) - exactly what should happen when gdeq isn't running: nothing has flattened the bare curve toward that shape, so it shouldn't match it.

**This also explains why the (mistaken) first-pass comparison leaned toward "default gdeq" rather than "candidate gdeq":** checked directly - `DATA.bare` vs `DATA.current` (bare + default gdeq) correlate at 0.925, while `DATA.bare` vs `DATA.refit` (bare + candidate gdeq) correlate at only 0.506. That's because the ORIGINAL/default gdeq coefficients were fit against the filter *before* the a+A shelves existed, so once the shelves are added they don't meaningfully flatten the combined curve any more (current's own p-p, 157.9us, is actually LARGER than bare's own 88.8us - the default coefficients' delay curve happens to add constructively with the shelves' delay rather than canceling it). The a+A candidate coefficients, by contrast, were fit specifically against the bare+shelf1+shelf2 combined curve and do genuinely flatten it (refit p-p 73.0us, less than bare's 88.8us). So a gdeq-OFF bare curve was always going to look more like a barely-corrected "default" curve than a genuinely-flattened "candidate" curve, shape-wise - the earlier read wasn't unreasonable given the (wrong) assumption that some gdeq was active, just built on the wrong premise.

**Bottom line - good news, not a problem:** `HiRes_aA_TF.txt` is exactly what it should be: a clean, high-resolution, DIRECTLY-MEASURED capture of the bare analog+shelf1+shelf2 target curve, with none of the noise/instability that made the earlier reconstructed-from-Trial2-data `DATA.bare` array sensitive to smoothing-window choice (see the 2026-09-05 "already Pareto-optimal" entry's caveats). **This is precisely the input the mid-band local-slope-vs-p-p Pareto search (2026-09-05 entry, parked pending go-ahead) needed** - a real, clean, high-SNR measurement of the target curve instead of a noisy reconstruction, which should finally let that search converge on a stable coefficient pair instead of one that moves around with the smoothing window. Not yet re-run - parked pending go-ahead, same as before, but the blocking data-quality problem that entry flagged is now solved. Updated comparison chart (now plotted against the correct bare-curve reference): `hires_aA_shape_check.html`.

## 2026-09-06, later still — two more HiRes sweeps cross-checked before use: `HiRes_aAg2_TF.txt` (default gdeq) and `HiRes_aAG3_TF.txt` (candidate gdeq) - both confirmed correctly configured, three independent ways

**User: "Should cross check these first?"** - yes, given the immediately preceding mixup (thinking a sweep had gdeq on when it didn't), verifying configuration before trusting any new sweep is now the right default, not an extra step. Ran the exact same group-delay pipeline (same edge-trim-before-smoothing fix from the entry above - both files have the identical near-DC/near-8kHz edge character as every other HiRes sweep, clean in between) and checked each against all three known references (bare, default-gdeq-corrected `current`, candidate-gdeq-corrected `refit`):

| Sweep | vs. bare (no gdeq) | vs. `current` (default gdeq) | vs. `refit` (candidate gdeq) | Verdict |
|---|---|---|---|---|
| `HiRes_aAg2_TF.txt` | corr 0.854 | **corr 0.972, RMS 12.7us** | corr 0.005 | Confirmed: **default gdeq** |
| `HiRes_aAG3_TF.txt` | corr 0.637 | corr 0.208 | **corr 0.903, RMS 12.5us** | Confirmed: **a+A candidate gdeq** |

Both land decisively on their claimed configuration (matching their filenames' `g2`/`G3` suffixes) - no repeat of the earlier mixup.

**Stronger, independent cross-check - doesn't rely on the bare-curve reconstruction being right at all:** since both sweeps share the identical analog+shelf1+shelf2 path (only the gdeq coefficients differ between them), subtracting the two measured curves point-by-point should cancel that shared path out completely, leaving just the DIFFERENCE between the two gdeq coefficient sets' own group delay - a quantity computable in closed form from the two-section-allpass model alone, with no bare-curve/reconstruction involved whatsoever. Measured `(aAG3 - aAg2)` p-p = 142.2us vs. the pure analytic `candidate_gdeq(f) - default_gdeq(f)` model's p-p = 137.7us; **correlation 0.997, RMS 3.2us** (mean-matched) over the shared 387-7547Hz band. This is about as clean a confirmation as this project has produced - it validates both sweeps simultaneously and is immune to any error in the bare-curve reconstruction, since that term cancels algebraically before the comparison even happens.

**Conclusion: all three HiRes sweeps (bare, default-gdeq, candidate-gdeq) are confirmed clean and correctly configured.** This is a complete, mutually-consistent, high-resolution (5.86Hz spacing) real-hardware dataset - a genuine upgrade over every curve used for fitting in this project so far (all previously either reconstructed from noisy 23.4Hz-spacing data, or subject to the smoothing-window instability flagged repeatedly since 2026-09-05). Three-way comparison chart (measured vs. predicted, both configs, plus the bare reference): `hires_three_way_validation.html`. **Ready to use as the real input for the mid-band local-slope-vs-p-p Pareto refit search** - offered to the user, not yet started pending go-ahead.

## 2026-09-06, later still — mid-band local-slope-vs-p-p Pareto refit redone against real, validated HiRes data ("yes go ahead")

**User: "yes go ahead."** Redo of the 2026-09-05 Pareto search, this time against `HiRes_aA_TF.txt`'s real, cross-validated bare curve instead of the old noisy `DATA.bare` reconstruction. Same 387-7547Hz shared band, same `ssb_allpass1_t` two-section allpass model (`gd_two_section_us(f,a1,a2)`, `Fs=16000`) used throughout this project.

**Fixed a units bug in the local-slope metric before trusting any search result.** First attempt computed "slope of group delay" by taking a raw `np.diff` across a group-delay curve that had already been Savitzky-Golay-smoothed once (window ~700Hz) and then finely interpolated to a 10Hz grid. Result: 700-750us/kHz for the candidate config — 15-20x the historical 26-48us/kHz scale from the 2026-09-05 entry, an obvious red flag before it was ever reported anywhere. Traced it directly: printing consecutive `bare` samples in the 2800-3000Hz range showed real ~1-3us sample-to-sample wiggle surviving the once-smoothed curve — residual measurement noise, catastrophically amplified by dividing by only a 10Hz step. **Fix:** compute the local slope as a SECOND, independent Savitzky-Golay pass with `deriv=2` applied directly to the original unwrapped phase (not to the once-differentiated tau curve), using a substantially wider window than the one used for the primary group-delay/p-p curve. Converged in the 2000-3000Hz window range (candidate: 49.3-50.3us/kHz across that whole range, bare: 36.8-38.4us/kHz) - stable and consistent with the historical scale, unlike the 2026-09-05 entry's own window-dependence caveat. Settled on 2500Hz as the primary window. `slope_us_per_kHz = -(1/360) * d2Phase/dFreq^2 [deg/Hz^2] * 1e9`.

**Validated the additive model against real measured data before using it to explore new coefficient pairs.** `combined_predicted(f) = tau_bare_measured(f) + gd_two_section_us(f,a1,a2)`, using the real `HiRes_aA_TF.txt` bare curve as `tau_bare_measured`, compared directly against the real measured `HiRes_aAG3_TF.txt` (candidate gdeq) and `HiRes_aAg2_TF.txt` (default gdeq) curves: correlation 0.970 (RMS 7.1us) and 0.982 (RMS 8.6us) respectively - both substantially tighter than the old reconstruction-based model's 12.5-12.7us RMS (see the entry two above), because the real bare curve is a more accurate target than the noisy reconstruction it replaces. This step was necessary and done before trusting the model to explore untested `(a1,a2)` pairs.

**Full 2D grid search, `(a1,a2) in [-0.30,0.30]^2` at 0.01 step, objective = max abs local slope in 2800-4500Hz subject to a full-band p-p budget.** Key result, and the sharpest version yet of the far-out spur-forest explanation flagged 2026-09-05:

| Config | `a1`, `a2` | Full-band p-p | 2800-4500Hz local max slope |
|---|---|---|---|
| Bare (no gdeq) | n/a | 114.6us | 36.8us/kHz |
| **Current a+A candidate** | `-0.139115, -0.139115` | 105.2us | **49.6us/kHz — worse than doing nothing** |
| Default | `0.026173, 0.236810` | 156.5us | 26.1us/kHz — best achievable slope, at a much higher p-p cost |

The current candidate is nearly p-p-optimal for its own p-p level (consistent with the 2026-09-05 finding that it's "already Pareto-optimal" on the full-band p-p metric), but it makes the local 2800-4500Hz slope MEASURABLY WORSE than doing nothing at all - not just a smaller improvement than hoped, an actual regression relative to bare. This gives a quantitative, now real-data-backed explanation for the far-out two-tone spur forest the candidate was implicated in on 2026-09-05, sharper than that entry's rougher window-unstable estimate.

**Proper Pareto envelope, not a naive sort.** The symmetric (`a1=a2`) sweep's own p-p(a) relationship is U-shaped (minimum p-p near a=-0.16, rising again for more negative a), so sorting scattered `(a,pp,slope)` points by p-p and connecting them produces a nonsensical sawtooth - points from both branches of the U interleave with different slopes. Fixed by computing, for a fine grid of p-p budgets (100-239us in 1us steps), the TRUE minimum slope achievable among all 2D grid points (not just the symmetric subset) with p-p <= budget - a properly monotonic, non-decreasing-as-budget-tightens envelope. Confirmed the current candidate and default both sit close to but not exactly on this envelope (each was optimized for a different criterion), while a symmetric point near `a1=a2=+0.09` sits almost exactly on it at its own p-p level (envelope value at budget=136us: 28.11us/kHz, matching the point's own computed slope to 2 decimal places).

**Proposed "candidate B": `a1 = a2 = +0.09`.** Nelder-Mead-polished (penalty-function form, `obj(x) = slope(x) + max(0, pp(x)-budget)*penalty`) around that grid point to confirm it's a local optimum, not just the best of the 0.01-step grid. Result: p-p = 136.0us, local max slope = 28.1us/kHz.

| Config | Full-band p-p | 2800-4500Hz local max slope | vs. current candidate |
|---|---|---|---|
| Current candidate | 105.2us | 49.6us/kHz | — |
| **Candidate B** | 136.0us | **28.1us/kHz** | **43% slope reduction**, for +30.8us p-p (still 1.15x better p-p than default) |
| Default | 156.5us | 26.1us/kHz | best achievable slope, but candidate B gets within 2us/kHz of it at 20.5us less p-p |

Candidate B trades away much of the current candidate's p-p advantage but lands close to default's own best-achievable local slope while remaining meaningfully better than default on full-band p-p. It's a symmetric single-parameter choice (`a1=a2`), same design pattern as the existing candidate, so it drops mechanically into the same `ENV_GDEQ_A1_AA_CANDIDATE`/`ENV_GDEQ_A2_AA_CANDIDATE` macro slot in `envelope_gdeq.h` with no architecture change.

**This is a model-based prediction against real, cross-validated bare-curve data - NOT yet a bench result.** It has not been measured on real hardware, and in particular the actual two-tone spur-forest behavior (the practical thing this whole investigation is chasing) has not been re-tested with it. Needs a real TFA sweep (to confirm the predicted group-delay shape actually matches hardware, the same cross-check discipline used on all three HiRes sweeps above) and a two-tone spur-forest A/B against the current candidate before being trusted for anything beyond further modeling.

**Open implementation question, not yet resolved with the user:** the current architecture (`envelope_gdeq.h`/`.cpp`, `settings.h`, `serial_commands.cpp`) only supports two coefficient sets - default and ONE candidate - selected by a plain boolean (`s_env_gdeq_use_aa_candidate`), toggled by `'G'`. Bench-testing candidate B needs either (a) a quick, low-risk swap of the existing `ENV_GDEQ_A1_AA_CANDIDATE`/`A2_AA_CANDIDATE` macro values in `envelope_gdeq.h` to `0.09`/`0.09` - reuses the existing `'G'` toggle immediately, but overwrites the current validated aggressive candidate, losing the ability to instantly A/B back to it without a second reflash - or (b) a proper three-way extension of the boolean to an enum/int selector across all four of those files - bigger, more invasive, and (with no compiler available in this environment) requires careful manual re-reading rather than a build to catch mistakes. Not yet decided.

Chart (Pareto frontier + four-config group-delay overlay with the 2800-4500Hz band highlighted): `mid_band_pareto_refit.html`.

## 2026-09-06, later still — candidate B wired into the firmware as a third `'G'` option, not just a macro-value swap

**User: "Probably should add the new one to the toggle. In my list it will be G#4."** Went with the more invasive of the two options raised in the entry above (a proper multi-way extension) rather than overwriting the existing candidate's macro values, since the point of a live toggle is exactly to A/B all three sets without a reflash between them - overwriting would have cost that for the currently-validated aggressive candidate.

**What changed, across the four files that referenced the old boolean:**
- `envelope_gdeq.h`: added `env_gdeq_variant_t` (`ENV_GDEQ_VARIANT_DEFAULT=0`, `ENV_GDEQ_VARIANT_AA_CANDIDATE=1`, `ENV_GDEQ_VARIANT_CANDIDATE_B=2`, `_COUNT=3` - DEFAULT deliberately 0, same zero-fills-safely convention as `envelope_interp_curve_t`), plus `ENV_GDEQ_A1_CANDIDATE_B`/`ENV_GDEQ_A2_CANDIDATE_B` (0.09f/0.09f) and a new `ENV_GDEQ_HAS_CANDIDATE_B` guard, fitted/defined alongside the existing a+A candidate's macros in the same `ENV_FILTER_PNP_BC327_ATTN @ SAMPLE_RATE_HZ==16000` branch (the only combination either candidate has ever been fit for). The old `envelope_gdeq_get_use_aa_candidate()`/`envelope_gdeq_set_use_aa_candidate(bool)` API is retired, replaced by `envelope_gdeq_get_variant()`/`envelope_gdeq_set_variant(env_gdeq_variant_t)`/`envelope_gdeq_variant_available()`/`envelope_gdeq_variant_name()`.
- `envelope_gdeq.cpp`: internal state (`s_env_gdeq_variant`) changed from a bool to the enum; `envelope_gdeq_set_variant()` keeps the same "no-op transition doesn't reset state, unfit variant silently falls back to DEFAULT" discipline the old setter had, just generalized to 3 options via a small coefficient-lookup helper shared with `envelope_gdeq_init()`.
- `serial_commands.cpp`: `'G'` now CYCLES (default -> a+A candidate -> candidate B -> default -> ...), same modulo-cycle convention already used by `'C'` (interp curve) and `'f'` (ADC LPF mode), skipping any variant `envelope_gdeq_variant_available()` says isn't fitted for the active build - this replaces the old `#if ENV_GDEQ_HAS_AA_CANDIDATE` compile-time branch entirely, since the skip logic now handles "nothing else fitted" at runtime instead. The `'w'` chirp status line and the `'P'` settings-dump line (which now prints the enum constant name, e.g. `ENV_GDEQ_VARIANT_CANDIDATE_B`, same convention as `adc_lpf_mode`/`envelope_interp_curve`) were both updated to match.
- `settings.h`: the trailing `PersistentSettings` field was migrated in place from `bool env_gdeq_use_aa_candidate` to `env_gdeq_variant_t env_gdeq_variant` - safe because none of the 10 existing presets ever set that field explicitly (confirmed by reading every preset's positional initializer list: all top out at `envelope_interp_curve`, three fields short of this one), so every preset already relied on C's zero-fill, which lands on `ENV_GDEQ_VARIANT_DEFAULT` (0) exactly as before. Same kind of migration `adc_lpf_mode` went through earlier (bool -> 3-state enum for the Chebyshev filter option) - a direct precedent for doing this safely without a compiler available to check it.
- `ssb_mic_test.ino`: boot banner's `'G'` line updated to describe the cycle and both candidates' status (a+A: real, mixed result; candidate B: model-only, not bench-validated), guarded by `#if ENV_GDEQ_HAS_AA_CANDIDATE || ENV_GDEQ_HAS_CANDIDATE_B` instead of just the first flag.

**Verified by careful re-reading, not a build** (no compiler available in this environment, per this project's standing constraint) - grepped the whole project for every remaining reference to the retired `use_aa_candidate` names and confirmed the only survivors are inside dated historical prose (the 2026-09-05 header entry describing the API as it was AT THAT TIME, left alone per this project's "don't rewrite history" convention) plus the new entry above explicitly documenting the retirement - no live code path still calls the old functions or reads the old struct field. Traced the modulo-cycle pattern against the two existing precedents (`'C'`/`envelope_interp_curve_t`, `'f'`/`adc_lpf_mode_t`) to confirm `(env_gdeq_variant_t)((next + 1) % ENV_GDEQ_VARIANT_COUNT)` is exactly their established idiom, not a new one.

**Still not bench-tested** - this only wires candidate B up so it CAN be A/B'd live via `'G'`; the model-vs-hardware validation the entry above called for (a real TFA sweep, then a two-tone spur-forest A/B against the current a+A candidate) hasn't happened yet. `'G'`'s reply text repeats the "MODEL PREDICTION ONLY, NOT YET BENCH-VALIDATED" caveat every time it's selected specifically so that isn't easy to forget mid-session.

## 2026-09-06, later still — candidate B bench-tested: group delay confirmed (and better than modeled), real IMD gain small and delay-dependent

**Real TFA sweep captured: `HiRes_aAG4_TF.txt`** (user's own numbering: "G4" - the fourth gdeq state in their list, after default/a+A candidate/off). Same pipeline as every other HiRes sweep this project (edge-trim 35-7900Hz before smoothing, then 700Hz-window Savitzky-Golay for group delay, 2500Hz-window `deriv=2` pass for local slope - see the entries above).

**Configuration cross-check first, same discipline as every HiRes sweep before it:** compared the trimmed/smoothed measured curve against the bare, default, a+A-candidate, and candidate-B predicted shapes (mean-removed, 387-7547Hz shared band):

| Reference | Correlation | RMS |
|---|---|---|
| bare (no gdeq) | - | not the target; shape differs as expected |
| default gdeq predicted | 0.991 | 8.6us |
| a+A candidate predicted | 0.388 | 38.0us |
| **candidate B predicted** | **0.995** | **3.8us** |

Confirmed correctly configured as candidate B - clearly not the a+A candidate (corr 0.388), and a tighter match to candidate B specifically than even to default (RMS 3.8us vs. 8.6us, despite both showing high correlation since the shared bare-curve backbone dominates raw-curve correlation more than RMS does).

**Group-delay result: confirmed, and slightly BETTER than the model predicted.**

| Metric | Predicted (2026-09-06 refit) | Measured (`HiRes_aAG4_TF.txt`) |
|---|---|---|
| Full-band p-p (387-7547Hz) | 136.0us | 131.5us |
| 2800-4500Hz local max slope | 28.1us/kHz | **22.6us/kHz** |

The measured local mid-band slope (22.6us/kHz) beats not just the prediction for candidate B itself but even the model's own best-achievable prediction for the DEFAULT pair (26.1us/kHz) - real hardware validates the group-delay hypothesis behind candidate B more strongly than the model search itself claimed. **Independent differential check** (measured `HiRes_aAG4_TF.txt` minus measured `HiRes_aA_TF.txt` bare curve, canceling the shared analog+shelf path algebraically, same technique used for the aAG3/aAg2 cross-check): corr 0.968 vs. the pure analytic candidate-B-allpass-alone model, RMS 3.8us (identical to the overall RMS by construction - both share the same bare-curve subtraction). Three independent confirmations, same as every other HiRes sweep this project has validated.

**User's real bench IMD report (verbatim): "Change in IMDs very small - maybe highest IMDs see an improvement but it depends on 1 or 2 steps on delay setting to choose a compromise in either the default or new candidate, ie 2.00 to 2.10. In compromise I mean some go up and some go down in either candidate."**

This is the important, humbling half of the result. Despite candidate B's group-delay behavior landing right on - even slightly better than - prediction, that did **NOT** translate into a clean two-tone IMD win:
- The overall IMD change vs. default is small either way.
- Any improvement is concentrated in the highest-order (farthest-out) products - directionally consistent with candidate B's design intent (fix the mid-band slope the far-out spur forest was pinned on), but modest in size, not a clear win.
- **Both** default and candidate B need their own fine relative-delay compromise within a narrow 2.00-2.10 sample window - some IMD orders improve, others worsen, moving inside that range. Neither config has one unambiguous best point the way the ORIGINAL a+A candidate did (a clean, much bigger shift to ~2.83 samples, see the 2026-09-05 entry) - this is a qualitatively different, fussier kind of trade-off than that one was.

**This is the SAME lesson the 2026-09-05 spur-forest finding already flagged, now confirmed a second time with an independently-designed coefficient set:** flattening group delay in the 2800-4500Hz sub-band is necessary but evidently not sufficient for a clean real-world IMD win. The model (additive bare+allpass group delay) has now been validated to within a few microseconds against real hardware THREE separate times (aAg2, aAG3, aAG4) - it is not a shaky model. What it does NOT capture is whatever else determines real IMD: the unity-magnitude-only nature of an all-pass equalizer (it can't touch any AM-to-PM or nonlinearity-driven component of the spur forest), envelope-interpolation interaction, or a mechanism not yet identified. Group-delay theory alone has now been shown twice not to fully predict real two-tone spectral behavior in this filter/shelf combination - a recurring, load-bearing caveat for any future gdeq refit attempt in this project, not a one-off surprise.

**Practical read:** candidate B is real, and its own design goal (flattening the mid-band slope that the a+A candidate made worse than nothing) is genuinely achieved and bench-confirmed. But it is not a clear net upgrade over either the default or the a+A candidate for actual two-tone IMD - it's a third, comparably-mixed option, not a resolution. No firmware default change is warranted from this result alone. Chart (measured vs. predicted candidate B, plus context curves, plus the IMD note): `hires_candidateB_validation.html`.

## 2026-09-06, later still — finer relative-delay step added (`;`/`'`), directly prompted by the compromise-window finding above

**User: "is there a cmd to inc/dec the delay by <0.05 samps?"** No - `'['`/`']'` only ever stepped by the fixed `DELAY_STEP_SAMPLES` (0.05 samples), which was already the finest available. Added a fine-step pair, `;`/`'` (`DELAY_FINE_STEP_SAMPLES` = 0.01 samples, `relative_delay.h`/`.cpp`), directly usable now that the entry above found both default and candidate B need their delay chosen from inside a sub-0.1-sample compromise window (2.00-2.10 samples) rather than having one clean optimum - two presses of the old 0.05-sample step covered that whole window with no intermediate point to try. Same coarse/fine pairing convention already established for master gain (`'+'`/`'-'` vs `'.'`/`','`); `;`/`'` were picked over an unrelated key because they sit right next to `[`/`]` on a US QWERTY layout - the natural shifted-bracket pair (`'{'`/`'}'`) and `.`/`,` were both already taken (freq_dev slew limiter and master gain respectively). `serial_commands.cpp`'s `'w'` chirp status line and boot banner were left as-is other than the boot banner now also mentioning the new pair; no settings.h/preset changes needed since this only adds two new step-size increments, not a new persisted value.

## 2026-09-06, later still — preset load (`'0'`-`'9'`) confirmed to already apply the gdeq variant; then a real bug found and fixed - `'G'`'s candidate-B reply overflowed `serial_reply()`'s buffer

**First half - confirmation, no code change needed.** User asked "Please also make Preset # cmd pick up the gdeq candidate selection." This was already wired in during the earlier 2026-09-06 "candidate B wired into `'G'`" entry above: the `'0'`-`'9'` preset-load block in `serial_commands.cpp` calls `envelope_gdeq_set_variant(p.env_gdeq_variant)` unconditionally (no `AD9851_ATTACHED` guard needed - gdeq is independent of the AD9851 board), getting the same off/on-transition state reset as the live `'G'` toggle, and `'P'`'s settings-dump already round-trips the field as its enum constant name. Re-verified by re-reading the relevant block rather than assuming the prior turn's summary was accurate. Flagged one real caveat while confirming this: none of the 10 current `settingsPresets[]` entries set `env_gdeq_variant` explicitly (all rely on zero-fill to `ENV_GDEQ_VARIANT_DEFAULT`), so loading ANY preset currently resets the gdeq variant back to default, same as it would for a live `'G'` selection - not a bug, just a fact worth knowing while bench-testing candidate B across preset switches. User confirmed: "Yes it is working - I got confused (again!)."

**Second half - a real bug, found via the user's own bench use.** Same message continued: "BTW the 'B' message is too long and is truncated with warning." Root cause, confirmed by direct length calculation (Python, not guessed): the `'G'` handler's reply when landing on candidate B concatenates `envelope_gdeq_variant_name(ENV_GDEQ_VARIANT_CANDIDATE_B)` (46 bytes, `"candidate B (a1=a2=+0.09, NOT bench-validated)"`) with that variant's own caveat string (537 bytes - by far the longest of the three caveats, because it was written before the `HiRes_aAG4_TF.txt` bench test above landed and was explaining, at length, why candidate B was untested) plus the `"-> gdeq coefficients: "` prefix and `"\r\n"` suffix: **607 bytes total against `serial_reply()`'s 512-byte buffer** (`serial_commands.cpp`, added 2026-09-02 - see that function's own header comment for why the buffer exists and why truncation is now visible instead of silent). The other two variants' replies (default's short fallback caveat, and the a+A candidate's 406-byte reply) were never close to the limit - this was specific to candidate B's reply being written back when it still needed to carry a long "not tested yet" disclaimer.

**Fix - shortened AND updated, not just shortened.** Two problems compounded here: the reply was too long, and separately its content was already stale (the candidate-B bench test three entries above had already superseded "MODEL PREDICTION ONLY, NOT YET BENCH-VALIDATED... no bench optimum found yet"). Fixed both together rather than just trimming words to fit:
- `envelope_gdeq.cpp`'s `envelope_gdeq_variant_name()`: candidate B's string shortened to `"candidate B (a1=a2=+0.09)"` (25 bytes), matching the plain "name + coefficients" style the other two variants already use - the status/caveat text now lives in exactly one place (the `'G'` handler's caveat, plus the header comment), so a future status change only needs updating once, not synchronized across three call sites (`'G'`, `'w'`, boot banner) that all share this same name string.
- `serial_commands.cpp`'s `'G'` handler: candidate-B caveat rewritten to report the actual bench result - group delay CONFIRMED (beats its own prediction in the 2800-4500Hz band), real IMD gain small and delay-tuning-dependent, both configs needing the 2.00-2.10 sample compromise window from the entry above. New total: 418 bytes (name + caveat + prefix/suffix) - 82% of budget used by the LONGEST of the three replies now, comfortable headroom versus the exact-fit risk of a bare word-trim.
- `ssb_mic_test.ino`'s boot banner: same stale "MODEL PREDICTION ONLY, not yet bench-validated" wording existed here too (not the actual overflow bug, since `Serial.printf()` doesn't route through `serial_reply()`'s length guard - but stale and wordy regardless), updated to the same shortened, accurate summary and mentioning the `';'`/`'''` fine-step pair.
- `envelope_gdeq.h`: rather than editing the original "THIS IS A MODEL PREDICTION, NOT YET A BENCH RESULT" header paragraph (per this project's own "corrections are appended, never silently edited away" convention, used consistently for every prior gdeq finding in this file), appended a new "UPDATE, 2026-09-06 (same day, later still): bench-tested" paragraph directly after it, and a short pointer sentence at the `ENV_GDEQ_A1_CANDIDATE_B`/`A2_CANDIDATE_B` `#define` block's own "NOT YET VALIDATED ON REAL HARDWARE AT ALL" comment, both pointing forward to this entry and the candidate-B bench-test entry above rather than restating the whole result inline.

No `settings.h`/`PersistentSettings` changes needed - this was a reply-formatting/staleness fix only, not a change to any persisted value or the coefficients themselves. Verified by direct byte-count computation for all three `'G'` replies (Python), not by eyeballing string literals - same discipline as every numeric claim in this file.

## 2026-09-07 — the "top-priority" AM-to-PM crosstalk asymmetry item (`envelope_gdeq.h`'s undated header note, -2.4kHz nulls / +2.4kHz stuck at -40dB) is NOT currently reproducible - `'h'` (AMTEST) now behaves like textbook symmetric AM

**Background, for context:** `envelope_gdeq.h`'s header comment has called an unexplained AM-to-PM crosstalk asymmetry seen on `'h'` (AUDIO_SRC_AMTEST) "the current top-priority open item" since early in the project - no date attached to that paragraph, and (checked carefully before writing this entry) neither this file nor `ssb_mic_test_commands.md` nor `null_bias_investigation.md` contains a dated entry actually recording when/how that asymmetry was originally measured, or a completed `'g'` on/off comparison against it. `settings.h`'s own `AUDIO_SRC_AMTEST` comment explains why the asymmetry mattered: ideal linear AM (envelope multiplying a fixed carrier) is symmetric about the carrier by construction, no matter how distorted the envelope waveform is - a REAL asymmetry between fc+f and fc-f can only come from something also modulating phase in step with the envelope, i.e. genuine AM-to-PM crosstalk somewhere physical, not ordinary amplitude-domain nonlinearity.

**User's report today, re-running `'h'`: "AM test now seems well behaved and symmetric IMDs go up and down with modulation level in a sensible looking way, only getting large (-40) towards 100% modulation."** That's the textbook-expected pattern for a clean AM path: harmonic/IMD content scales with modulation depth and only becomes significant as the envelope swing approaches full-scale (100% modulation drives the PWM/RSET/transistor chain toward its own saturation/clipping limits - an expected, benign nonlinearity-vs-headroom trade-off, not a crosstalk signature). No asymmetry reported this time - "symmetric" was used explicitly, in contrast to the old one-side-nulls/other-side-stuck-at-40dB description.

**Read: the specific asymmetric AM-to-PM signature this project has been carrying as its top-priority open item is not currently present on real hardware.** Two things are NOT known and shouldn't be assumed: (1) whether this was cleared by one of the many unrelated fixes already landed since that undated note was written (the GPIO13 ADC-continuous-driver 5kHz coupling fix and the GPIO39 stuck-low addressing fix are both in the right general class - digital/GPIO activity physically leaking into the RF chain - though neither was ever specifically tied to this symptom) or was never as reproducible as the original note suggested; (2) whether today's check specifically compared `'g'` on vs `'g'` off (the original recommended workflow) or was run at whatever `'g'`/`'a'`/`'A'` state happened to be active - worth confirming before fully retiring the item, though the CORE finding (symmetric, sensibly-scaling AM behavior) stands regardless of gdeq state.

**Why this matters beyond just closing an old item:** the 2026-09-05 and 2026-09-06 spur-forest/candidate-B writeups both named "AM-to-PM or nonlinearity-driven" effects as the leading unidentified explanation for why flattening group delay (validated to a few microseconds RMS, twice, on real hardware) hasn't produced a clean two-tone IMD win. Today's result is evidence AGAINST the AM-to-PM half of that hypothesis, at least in the form this project has been watching for - which narrows, but doesn't close, that still-open mystery: "nonlinearity-driven" (envelope-domain, amplitude-only distortion - which `'h'` was never able to rule out, since ordinary AM harmonic distortion is exactly what a clean, symmetric, modulation-depth-scaling result looks like) or a mechanism not yet identified remain the live candidates.

`envelope_gdeq.h`'s header comment updated (see change note there) with a pointer to this entry rather than an edit to the original "top-priority" paragraph, per this project's append-don't-erase convention. `ssb_mic_test_commands.md`'s "Quick workflow reminders" line describing the recommended `'h'`+`'g'` workflow also updated with a pointer to this result.

## 2026-09-07, later same day — closing the loop: `'g'` on/off on `'h'` (AMTEST) makes almost no difference, completing the recommended workflow this project had been carrying as an open item

**User, having now specifically run the `'g'` off/on comparison the entry above flagged as the one remaining loose end: "g makes almost no difference - a dB or two on high order imd that are already at -60dB, some up, some down."** This is the completed version of the exact workflow `envelope_gdeq.h`'s original header note called for ("AMTEST with this on/off is a direct check of whether it has any effect...") - now actually run and logged, closing that open thread properly rather than leaving it inferred from the symmetric-AM result alone.

**Read:** a 1-2dB change on IMD products already sitting at -60dB, split roughly evenly between improving and worsening across orders, is consistent with measurement noise/scatter around the analyzer's own noise floor at that level - not a systematic effect in either direction. Combined with the entry above (no asymmetry present to fix in the first place), this is a clean, doubly-confirmed null result: `'g'` has no meaningful effect on the AMTEST path, in either direction, at any level worth acting on.

**This is also the physically expected outcome, not just an empirical curiosity** - `gdeq` is a unity-magnitude allpass filter that only reshapes the relative delay of different frequency components in the digital envelope, applied to a test signal (AMTEST, per `test_signals_generate_amtest()`) that's a single-tone AM whose only harmonic content comes from whatever nonlinearity already exists downstream in the physical RSET/PWM/transistor path - content gdeq cannot create, remove, or reshape in amplitude, only shift in time. With the earlier asymmetric AM-to-PM signature also confirmed absent, there's no longer any known mechanism on the `'h'` path a delay-only digital filter would be expected to influence either way.

**Net effect on the still-open "what's really limiting real IMD beyond group delay" question (2026-09-05/09-06 entries):** AM-to-PM crosstalk is now doubly ruled out as a factor specifically observable via AMTEST - both the asymmetry itself and any `'g'`-sensitivity of it. This doesn't touch the OTHER remaining candidate from that question (envelope-domain amplitude nonlinearity feeding the two-tone spur forest through a different path than AMTEST exercises), which stays open. `ssb_mic_test_commands.md`'s matching entry updated with a pointer to this result.

## 2026-09-07, later still — `'E'` added: lets the existing duty-override up/down stepper (`'d'`/`'>'`/`'<'`/`'N'`/`'B'`) validate `'D'` (predistort) against real hardware, since plain `'d'` mode structurally can't

**User: "One thing I'd like to do is check the distortion correction. can this be put in loop (on/off) for the duty measurement ('d')"** - directly targets the open item flagged in the 2026-09-06 status assessment: envelope pre-distortion (`'D'`) has real measured-curve backing (`envelope_predistort.h`'s REVISION 1-5 history) but has never been validated against real hardware output with it on vs off. Clarified via a quick question first: the user already has an automated test harness driving the existing up/down stepper and plans to just run it twice - once with `'D'` on, once off - rather than wanting a new self-toggling firmware loop.

**Caught before building anything: that plan, run against plain `'d'` override as-is, would show NO difference regardless of `'D'`'s state - not because predistort doesn't work, but because `'d'` direct duty override is BY DESIGN a bypass of the entire envelope pipeline** (see its own comment, and REVISION 3/4/5's own history - this is literally the mechanism used to characterize the raw, uncorrected duty->RF-output curve that the predistort LUT was built FROM). `envelope_output_write_duty_raw()` writes the stepped value straight to the LEDC register; `envelope_predistort_process()` never runs. Toggling `'D'` while in that mode is a genuine no-op, and running the two automated passes as originally planned would have produced a misleading null result - indistinguishable from "predistort has no effect" when the real explanation would have been "predistort was never in the loop to begin with."

**Fix: added `'E'`, a mode toggle for the SAME `'>'`/`'<'`/`'N'`/`'B'` stepper**, rather than a new/different stepping interface - specifically so the user's existing automated harness needs no changes to how it drives those keys. `'E'` off (default, unchanged behavior) keeps the stepped value as the literal raw duty count. `'E'` on reinterprets the exact same stepped value (same 0-1023 range, same step sizes) as a commanded envelope index: normalized to a [0,1] fraction, then run through `envelope_predistort_process()` if `'D'` is on or the plain `envelope_output_get_pwm_scale()`/`get_pwm_offset()` linear mapping if it's off - the identical branch `dsp_task`'s own per-tick path takes (`ssb_mic_test.ino`) - clamped to [0,1], then converted to a duty count and written. So the user's plan now works exactly as stated: send `'E'` once, then run the same automated up/down sweep twice (`'D'` on, `'D'` off), and the resulting duty (and whatever real RF/gate measurement the harness reads at each step) will actually differ between the two passes if predistort is doing something.

**Implementation notes:** pulled the actual compute-and-write-and-reply logic into one shared `duty_override_write_and_reply()` function, called from all five sites (`'d'`'s on-enable branch, `'>'`, `'<'`, `'N'`, `'B'`) instead of five independent copies of a raw-vs-mapped branch - avoids the five call sites silently drifting out of sync with each other the way `'G'`'s three near-duplicate caveat strings did before an earlier fix this project made. Reply format extended (not just `-> duty NNN/1023` any more) to show the raw index, the normalized fraction, the resulting duty, AND which mapping was actually used (`predistort ON` / `linear mapping, 'D' off`) - so the serial log alone gives a numeric sanity check alongside whatever the user's own instrument reads, without needing to cross-reference `'D'`'s separate status line. `'E'` works regardless of whether `'d'` override is currently on, and immediately re-writes the current index under the new mode if it is, rather than requiring another step press to see the switch take effect. No `settings.h`/`PersistentSettings` changes - this is a serial-command/test-tooling addition only, nothing persisted. Verified by manual re-reading (no compiler available) - traced all five call sites, confirmed `max_duty`'s only remaining uses are in the two handlers (`'>'`, `'N'`) that still need it for their own clamp check after the shared write moved out, and confirmed brace/paren balance across the file.

## 2026-09-07, later still — first real-hardware run of the new `'E'` sweep (`DistortionLinearisationEcmd.txt`, predistort ON): confirms `envelope_predistort_process()` genuinely linearizes envelope->RF-amplitude, to well under 0.5dB, across the whole usable range

**User's first automated run with `'E'` on and `'D'` on: a clean 1024-point sweep, env index 0-1023, one dBm reading appended per step by the user's own logger.** Parsed programmatically (not eyeballed): monotonically increasing dBm end to end, zero drops >0.5dB anywhere in the sequence - a clean, well-behaved capture with no dropped/garbled lines, confirming `'E'`'s reply format (added the same day, see the entry above) round-trips through the user's automated harness exactly as intended.

**Linearity check: fit `dBm = A + 20*log10(frac)` (the textbook shape for RF output amplitude directly proportional to commanded envelope) to the measured data, anchoring only the constant A (least-squares over frac 0.10-1.0), then measure the residual across the full run.** Result: RMS residual 0.134dB over frac 0.10-1.0 (921 points), 0.144dB over frac 0.02-1.0 (1003 points, i.e. essentially the whole run above the analog noise floor's turn-on knee), max absolute residual 0.545dB - and critically, the per-decade residual means (bucketed by frac in 0.1-wide bands) show NO systematic trend in either direction (largest bucket mean +0.145dB at frac~0.4, smallest -0.155dB at frac~0.6, no monotonic bow or drift across the range) - this is small, symmetric ripple consistent with real measurement noise, not leftover systematic nonlinearity the LUT failed to correct. Below about frac 0.02 (duty <~180, matching this project's known dead-zone/turn-on-knee history from `envelope_predistort.h`'s REVISION 4/5 notes) residual grows sharply, as expected and unavoidable - no correction curve can make output track linear amplitude below the point real RF output is still buried in the noise floor.

**Read: this is the first real-hardware confirmation that predistort's LUT correction actually does what it's supposed to on THIS board** - not just self-consistent against its own fitting data (which REVISION 3/4/5's own "resolution check" paragraphs in `envelope_predistort.h` already established), but genuinely linearizing envelope->RF-amplitude end to end, to within half a dB, across essentially the entire usable dynamic range (~-76dBm to -40dBm in this run). This directly closes the "envelope pre-distortion ('D') ... not yet validated beyond the measurement itself" open item from the 2026-09-06 status assessment - for the STATIC linearization question specifically. It does NOT touch predistort's effect on real IMD (a separate, dynamic question `'D'` on/off with a live two-tone signal would answer, not this static duty sweep) - that stays open.

**Important caveat, and the other half of the originally-requested comparison: this run only has `'D'` ON.** The user's original plan was two passes (`'D'` on, `'D'` off) - this is the first of those. Without the `'D'`-off companion pass (which would show the RAW, uncorrected duty->dBm curve for direct comparison), this result establishes that the CORRECTED curve is good, but doesn't yet quantify how much worse the uncorrected one is - that comparison is still pending. Chart (measured vs. ideal-proportional reference, plus the residual trace): `predistort_e_validation.html`.

**Same day, addendum: added a lin/lin panel to `predistort_e_validation.html`, per request ("Please add a lin/lin chart to report").** The two panels above are both dB-domain (measured dBm vs. frac, and residual-in-dB vs. frac) - dB compresses ratios at the top of the range and expands them near the noise floor, which can hide a proportional scale error or bury it in a small-looking dB number. New middle panel plots the same run on plain linear axes: measured amplitude, normalized so the fitted ideal line lands exactly on y=x (a straight diagonal through the origin is what a perfectly linearized path looks like here), against commanded envelope fraction. Derived directly from the existing per-point residual without needing to know the fitted constant A explicitly: since `residual = measured_dBm - (A + 20*log10(frac))` by construction, `amp_norm = 10^((measured_dBm - A)/20) = frac * 10^(residual/20)` - the `A` cancels out of the normalization. Confirmed the JS parses cleanly (`node --check`) before delivering. No new data or measurement involved - purely a different view of the same `DistortionLinearisationEcmd.txt` run analyzed above.

## 2026-09-07, later still — the 'D'-OFF companion pass (`DistortionLinearisationEcmdDoff028054.txt`): quantifies exactly what the predistort LUT buys over a well-chosen linear offset/scale mapping

**Background:** the earlier discussion (same day) walked through why offset/scale needed to come from real REVISION 6 duty-sweep data rather than the old 20%/90% compiled-in default, and landed on 28%/54% as the window with the best trade-off (duty 281-828, predicted RMS ~0.7% of full-scale amplitude / ~2% max, covering 53% of the real duty range while staying clear of both the dead zone and the near-saturation compression). User set offset/scale to that value, sent `'E'` on / `'D'` off, and ran the SAME automated 1024-point sweep used for the `'D'`-on pass. This is the second half of the originally-planned two-pass comparison.

**Data quality: clean.** Parsed programmatically - 1024 rows, monotonically increasing dBm end to end, zero drops >0.5dB anywhere, same clean round-trip through the user's logger as the `'D'`-on run.

**Headline result 1 - the floor/off-state gap is large and structural, not noise.** At commanded frac=0, `'D'` on (LUT) reaches -98.43dBm (down at the analog noise floor - a real "off"); `'D'` off with the 28/54 linear mapping only reaches -59.08dBm, because duty=286 (the window's own floor, chosen specifically to sit past the nonlinear dead zone) is nowhere near the true off-state duty. That's a **39.4dB worse floor** - not a fitting artifact, not measurement noise, but the direct, unavoidable consequence of choosing ANY single linear offset+scale mapping that is also required to be linear in its working range: a straight line that stays flat/accurate through the working range physically cannot also pass through true zero output at frac=0, because the real duty->amplitude curve has a dead zone/turn-on knee that isn't part of that straight-line region at all. This is precisely the problem the nonlinear LUT exists to solve - reach a deep off state AND stay linear across the working range simultaneously, something no single linear map can do.

**Headline result 2 - in-range, the well-chosen linear window is genuinely close to the LUT.** Fitting the physically appropriate model for this data - amplitude = m*frac + c (an affine straight line, not a proportional-through-origin curve, since the mapping was never intended to reach zero) - gives RMS residual 0.210dB / max |residual| 1.34dB across the FULL frac 0-1 range. That's only modestly worse than the LUT's own 0.144dB RMS / 0.545dB max (fit against the true proportional model, `dBm = A + 20*log10(frac)`, over frac 0.02-1.0). Restricting to frac 0.5-1.0 and fitting the SAME proportional model used for the LUT gives 0.213dB RMS / 0.489dB max for the linear-mapped run - essentially matching the LUT's own numbers. This confirms the 28/54 window (picked from the smoothed REVISION 6 duty sweep, not from the real-time run itself) really did land close to linear on this actual board, close to what that earlier data-driven prediction called for (predicted ~0.7% amp-normalized RMS / ~2% max; measured 0.744% RMS / 2.650% max in amplitude terms, 0.210dB/1.336dB in dB terms) - a good real-hardware validation of that window-picking method, not just a lucky guess.

**Where the affine fit's residual is largest:** frac 0-0.1 (mean +0.453dB, the largest single bucket) - the very bottom of the chosen window isn't quite as linear on this real board as the smoothed V6 curve predicted, a modest but real deviation worth noting if this window is refined further. Applying the full proportional model (rather than affine) to the whole 0-1 range obviously fails badly at low frac (RMS 2.567dB, max 14.1dB) precisely because the linear mapping structurally can't approach zero output - this isn't a new finding, it's the same floor-gap problem restated in the proportional model's terms, included here only to show the metric is model-dependent and the floor-gap framing above is the more honest one.

**Read:** predistort's LUT is not just "a bit better" than a well-chosen linear mapping - it wins specifically and almost entirely on being able to reach a genuine off-state while ALSO staying linear across the working range. Within the working range alone, a carefully-chosen linear offset/scale mapping is a legitimate, nearly-as-good substitute - useful context if a future design ever needs to trade the LUT's complexity for simplicity in an application that doesn't need deep envelope nulls. For this project (SSB envelope nulls between syllables genuinely need deep suppression), the LUT's value is confirmed to be real, quantified, and specifically located at the low end of the range, not spread evenly across it. Chart: `predistort_doff_vs_don.html` (dBm overlay of both runs plus the D-off affine-fit residual, same conventions as the D-on-only chart).

## 2026-09-07, later still — reopening the interp/wake-rate question: `ENVELOPE_INTERP_FACTOR`=4's real 64kHz wake rate causes audible two-tone burble even with `'I'` fully OFF, a combination the 2026-09-02 "settled" verdict never actually tested

**User's report (live listening, current hardware): "even setting x4 Fs for interp causes audible burble on the two tone with I-off."** This looked at first like a direct contradiction of the 2026-09-02 "decisive" finding (`ssb_mic_test_commands.md`'s "Envelope output interpolation" section) that `'I'` off was the *clean baseline* at a raised interp factor - but re-reading that finding's exact conditions shows it isn't a contradiction at all, it's a genuinely untested combination.

**The gap: the decisive test and the "settled" default point at two different absolute wake rates, and nobody re-checked `'I'`-off at the higher one.** The 2026-09-02 decisive `'I'`-off/HOLD/LINEAR/Catmull-Rom comparison (the one showing `'I'` off as a clean -35.4dB baseline) was explicitly run at `SAMPLE_RATE_HZ=10000`, `ENVELOPE_INTERP_FACTOR=4` - a 40kHz wake rate. Later that same day, `SAMPLE_RATE_HZ` was reverted to 16000 as the permanent operating default (the Fs-margin investigation below it). Because `ENVELOPE_INTERP_FACTOR` is a compile-time constant that unconditionally sets the real hardware `gptimer` rate regardless of the runtime `'I'` toggle (confirmed by re-reading `init_sample_timer()`, `ssb_mic_test.ino` - `resolution_hz`/`alarm_count` are computed straight from `ENVELOPE_INTERP_FACTOR`/`SAMPLE_RATE_HZ` with no `'I'`-related gate anywhere), the *actual* wake rate that has shipped as the default ever since has been `4 x 16000 = 64kHz`, not the 40kHz the "`'I'` off is clean" conclusion was drawn from. **The `'I'`-off isolation test was never repeated at the real 64kHz rate** - the "settled" verdict's own text already flagged this as a loose end ("Combined with the earlier `[timing]` margin findings (16kHz leaves ~16-17.5us of headroom vs 10kHz's ~55-58us)... worth revisiting if the underlying mechanism... ever gets pinned down"), but nobody had reason to revisit it until now.

**Why this matters mechanistically:** with `'I'` fully off, `envelope_interp_on_interp_tick()` is a confirmed bare early-return (`envelope_interp.cpp` lines ~239-250) - it never calls `envelope_output_write_pwm()`, so the LEDC duty register is untouched on 3 of every 4 fast ticks, exactly as before this feature existed. That rules out every LEDC-write-rate theory from the interpolation brainstorm two turns ago (the redundant-write hypothesis, the LEDC/gptimer clock-phase-mismatch idea, sigma-delta as an LEDC alternative) as an explanation for THIS specific symptom - none of them can fire if the register is never written. What's left, running unconditionally at 64kHz regardless of `'I'`, is: the `gptimer` ISR itself firing 64,000 times/sec (Core 1), the cross-core `vTaskNotifyGiveFromISR`/IPI wake it sends into `dsp_task` (Core 0) every time, and `dsp_task`'s own wake-check-sleep-again cycle on the 3-of-4 ticks that turn out to be no-ops. Any of those three costs real, nonzero time even when "nothing" happens - and this project has already run into real, still-not-fully-explained trouble specifically from cross-core/IPI activity before (the Core-1 co-location experiment, `ssb_mic_test.ino`'s "TRIED, REVERTED" note, starved `loop()`/serial completely despite CPU-time math saying there was ample headroom).

**Recommended next steps, in order of how decisive/cheap they are:**
1. **True zero-overhead baseline: rebuild with `ENVELOPE_INTERP_FACTOR=1`** (real 16kHz wake, no extra ISR/notify/no-op-check cycle at all - not just `'I'` off at runtime). This has apparently never actually been tried; every "`'I'` off" test on record still had the compile-time factor at 4 (64kHz real wake) or, in the one directly-comparable controlled test, 4 at a 10kHz base (40kHz wake). If the burble disappears at true factor=1, that confirms this is purely a raised-wake-rate effect with a threshold somewhere between 16kHz and 64kHz, independent of any PWM write. If it does NOT disappear at factor=1, that's a much bigger deal - it would mean the burble isn't about this feature at all and something else changed.
2. **Cheaper, no-rebuild check: drop back to `SAMPLE_RATE_HZ=10000` with the existing `ENVELOPE_INTERP_FACTOR=4`** (restores the exact 40kHz wake rate the original decisive test used) and re-run the same by-ear check. If the burble goes away at 40kHz but is present at 64kHz, that pins down a real wake-rate threshold between the two without needing a rebuild, and matches the margin numbers already on record (16kHz's ~16-17.5us headroom vs. 10kHz's ~55-58us).
3. **Check the `[timing]` diagnostic line** (`max_busy_us`, `max_gap_us`, `overruns`, `late_ticks_total`) in the exact burbling configuration (`SAMPLE_RATE_HZ=16000`, factor=4, `'I'` off). If real overruns/late ticks are present now (unlike the zero-overrun reading from the original 16kHz/10kHz comparison, which was taken at a different point in the investigation), this has a much more mundane explanation - `dsp_task` intermittently missing its own deadline - than an electrical/EMI mechanism, and points straight at the same margin problem as the LEDC-write-rate findings, just manifesting through a different path (ISR/notify overhead instead of register writes).

**Correction, same day: step 1 above was already done historically - "`'I'` off" has always meant `ENVELOPE_INTERP_FACTOR=1` in every prior real-hardware report, not factor=4 with the runtime toggle off.** User: "When ever I reported that I was running with I-off now as default is when I also set ENVELOPE_INTERP_FACTOR=1." So the assumption above - that the settled default has secretly been running the real timer at 64kHz all along - was wrong. The 2026-09-02 "settled, `'I'` off" operating point genuinely was a true 16kHz wake with none of this feature's infrastructure running at all, exactly as it was always described. Good news for the story so far: it means that finding was never actually shaky, just under-specified in how it got restated here two entries ago.

**But the real, decisive test just got run anyway, and it's cleaner than anything on record before it.** User: "Recently I retried =4 to check and burble was there albeit at a lower level then when enabling interp writes." This is a genuine, controlled `ENVELOPE_INTERP_FACTOR=1` vs. `=4` (`'I'` off both times, so zero LEDC writes either way) comparison, and it comes back positive: **raising the wake rate alone, with the interp-tick payload a confirmed no-op and not a single extra register write anywhere, still produces audible burble** - lower in level than any `'I'`-on mode, but real and present. This is a materially cleaner isolation than the earlier HOLD-curve "wake-rate-alone" table (which still wrote the LEDC register every tick, just with an unchanging value) - this new result has literally nothing touching the PWM peripheral on the extra ticks, and the effect survives anyway.

**This rules out the leading "digital switching crosstalk from the register writes" hypothesis as the WHOLE story, though it may still be a real, additive second contributor.** The 2026-09-02 HOLD investigation's working theory was "crosstalk from the 4x-more-frequent register writes coupling onto some other node" - that can explain the GAP between HOLD and `'I'`-off-at-40kHz (register written vs. not), but it cannot explain the gap this new test just found between factor=1 and factor=4-with-`'I'`-off (neither writes the register on the extra ticks, yet factor=4 is worse). So there are now two independent, apparently stacking contributors: (1) a wake-rate-alone cost with zero peripheral involvement - gptimer ISR + cross-core notify/IPI overhead perturbing something else time-sensitive, most plausibly the timing of the full tick's own AD9851 SPI phase-update or the `tick_start_us` capture feeding the Hilbert/atan2/envelope chain a few microseconds off from where it should be - and (2) an additional, larger cost specifically from writing the LEDC register at the higher rate, on top of (1). "Burble" (an audible warble/instability) fits a phase/frequency-timing-jitter mechanism at least as well as a broadband-noise-floor one, which favors (1) as a real, physically distinct effect and not just measurement noise around the HOLD result.

**2026-09-07, later still - the bracket test comes back conclusive and identifies the mechanism precisely: this is `dsp_task` intermittently missing its own deadline at 16kHz, not a broadband EMI/crosstalk effect.** User: "10kFs + x4 works, sweet tone and low noise floor immediately around tone. 16k+x4 -> burble, tone freq shifts, 10-15dB higher noise floor around tone." Both runs are `'I'` off (zero LEDC writes on the extra ticks either way) - the only variable is `SAMPLE_RATE_HZ`, 10000 vs 16000, with `ENVELOPE_INTERP_FACTOR=4` held fixed at both.

**The "tone freq shifts" detail is the decisive piece of evidence, not just another symptom.** TWOTONE is a pure software oscillator (`test_signals.cpp`'s `generate_twotone_sample()`) - `s_tone1_phase`/`s_tone2_phase` advance by a fixed `2*pi*f/SAMPLE_RATE_HZ` on every call, with NO external timing reference (no ADC, no clock input) - the actual frequency that comes out is entirely a function of how regularly that function gets CALLED. There is no electrical/EMI/crosstalk mechanism that can shift the frequency of a purely-synthesized software oscillator - the only way its real output frequency can differ from nominal is if `dsp_task`'s full-tick calls to it happen at an irregular (not-exactly-16000/s) effective rate: some ticks late, some skipped, some doubled-up. Observing an actual frequency shift is therefore direct, first-principles proof that `dsp_task` is missing/mistiming its own full-tick deadline at 16k+x4 - not a hypothesis this time, a logical necessity given what the code does.

**Ruled out: a repeat of the factor=7 gptimer clock-rounding bug.** Recomputed both configurations' `gptimer` numbers directly (Python, not from memory): at factor=4, `resolution_hz = 2,000,000*4 = 8,000,000`, and `80,000,000 / 8,000,000 = 10` exactly - a clean APB divisor, same as it's always been confirmed to be at factor=4. `alarm_count = 2,000,000/SAMPLE_RATE_HZ` is also an exact integer at both rates tested: 200 at 10000Hz, 125 at 16000Hz. So neither configuration has any fractional-prescaler rounding error the way factor=7 did - the timer itself is genuinely, provably clock-clean at both 10kHz and 16kHz. The difference is real-time margin, not clock accuracy: full-tick period is 100us at 10kHz vs. 62.5us at 16kHz (matching this project's own earlier `[timing]` measurements exactly), with previously-documented headroom of ~55-58us at 10kHz vs. only ~16-17.5us at 16kHz. The SAME fixed per-extra-wake overhead (gptimer ISR + cross-core notify, 3 extra no-op wakes per real tick either way) is comfortably absorbed by the 55-58us cushion at 10kHz and evidently is NOT absorbed by the much thinner 16-17.5us cushion at 16kHz - intermittently pushing some full ticks late enough to measurably perturb the phase accumulators' effective rate, which shows up as both the frequency shift (a >> few-Hz-scale error visible by ear) and the 10-15dB near-carrier noise floor rise (classic sampling-jitter phase noise - irregular sample timing on a synthesized tone spreads spectral energy into sidebands around the carrier, a well-understood DSP effect, not a new hypothesis needed to explain it).

**Read: the mechanism for the wake-rate-alone component is now essentially closed, and it's the mundane one.** Not EMI, not electrical crosstalk, not something exotic - `ENVELOPE_INTERP_FACTOR=4` simply does not fit inside 16kHz's real-time budget on this hardware, while it fits comfortably inside 10kHz's. This doesn't retroactively change the write-rate component (HOLD/LINEAR/Catmull-Rom on top of a working wake rate) - that was independently measured AT 10kHz+factor=4 (the original "decisive" table) and showed its own real, separate degradation (HOLD +2.0dB, LINEAR +11.5dB, Catmull-Rom +14.2dB) even in a config with ample margin - so a clean wake rate is necessary but not sufficient for `'I'` to be worth using.

**Still worth doing, to fully close the loop with hard numbers rather than by-ear inference:** pull the `[timing]` line's `overruns`/`late_ticks_total`/`max_gap_us` at 16k+x4/`'I'`-off (expect nonzero this time, unlike the original factor=1 zero-overrun baseline) and, for symmetry, confirm they read clean at 10k+x4 too. Not yet done - the conclusion above is solid on the frequency-shift logic alone, but the counters would pin down exactly how often ticks are actually being missed.

**Forward-looking implication for the interpolation feature itself:** 10kHz+x4 being clean with `'I'` off reopens interpolation as viable AT 10kHz specifically, something 16kHz never allowed room for - but this is not the same as "10kHz solves interpolation": the original decisive HOLD/LINEAR/Catmull-Rom degradation numbers were measured in exactly this 10kHz+factor=4 configuration and already showed real, separate write-rate cost on top of a clean wake rate. If interpolation's imaging benefit is ever worth chasing again, 10kHz base is the promising direction to do it in (the wake-rate obstacle is now understood and avoidable there) - but the write-rate cost inside `'I'` itself would still need its own separate resolution, unrelated to this finding.

**Refined next steps:** the `ENVELOPE_INTERP_FACTOR=1` step from the numbered list above is done (that's what "`'I'` off, historically" already was) - drop it. Still worth doing: (2) the `SAMPLE_RATE_HZ=10000`+factor=4 check, to see whether the wake-rate-alone burble is present at 40kHz too (if it disappears at 40kHz but is present at 64kHz, that brackets a real threshold in between; if it's present even at 40kHz, the original 2026-09-02 "clean baseline" reading at that exact configuration needs a second look - possibly a difference in listening conditions/attentiveness rather than the hardware). And (3) the `[timing]` overrun/late_ticks check at the current burbling config remains valuable, but with a sharper question now: since the effect is audible with a clean-looking CPU-time margin most likely intact (this is a few-microsecond-scale jitter question, not a gross overrun), a clean `overruns=0` reading wouldn't rule the mechanism out - it would just mean the disturbance is too fine-grained for that particular counter to see, and pointing more strongly at real interrupt-latency/IPI jitter than at `dsp_task` grossly missing its deadline.

**Not yet run** - this is a live discussion item, not yet a bench result. Chart/data pending whichever of the above the user runs next.

## 2026-09-07, later still — found and fixed why `[timing] ad9851 breakdown:` never appears on the bench: it's being silently starved by the per-line USB-CDC buffer guard, not missing/broken

User pushed back on the "10kHz is the way forward" framing above as defeatist ("we definitely don't want to go back to 10kFs so where can we save time and give ourselves more cpu... You mentioned SDM - any less overhead?"). Before answering that, went looking for the two already-built finer diagnostics (`[timing] ad9851 breakdown: prep_us=/spi_us=` and `[timing] dsp breakdown: audio_fx=/fir=/atan2=/sqrt=`) mentioned in the response, so real numbers could replace guessing about what's actually inside the "write=28us" bucket. User reported: "dont see any [timing] ad9851 breakdown:".

**Root cause, found by re-reading `diagnostics.cpp`'s `print_timing_and_adc_block()` and `diag_room_for()`:** `AD9851_ATTACHED` is `1` (`config.h:29`), so the line is genuinely compiled in — this isn't a config/build-flag gap. The real cause is the per-line TX-buffer guard the whole diagnostics block already runs on: every `Serial.printf()` call in this function is wrapped in `if (diag_room_for(N)) { ... }`, where `diag_room_for()` just checks `Serial.availableForWrite() >= N` and silently increments a skip counter (`s_dbg_diag_block_skip_count`) if not — by design, per that function's own header comment, "the cost is a patchier print... rather than an all-or-nothing block." That guard exists precisely because a real hardware measurement showed the USB-CDC TX buffer's resting `availableForWrite()` is only on the order of ~150-200 bytes, nowhere near enough for the whole block to fit at once.

The problem: every one of these `Serial.printf()` calls in the block fires back-to-back, synchronously, with no yield in between — so the buffer has no chance to drain between lines within one call to the block; each line's print immediately eats into the room the NEXT line's own guard checks. Before this fix, the order was: main `[timing]` line (guard 160B) → wakeup jitter (100B) → `[core1]` diag (90B) → `[dsp]` breakdown (90B) → `[timing] ad9851 breakdown` (150B) — last, and needing the single largest reservation of any line in the whole block. By the time execution reached its guard, the previous four lines (up to ~440B worth, if they all printed) had already been queued into a buffer that only had ~150-200B free to begin with. It was structurally the least likely line in the entire function to ever win its own room check — not a bug in the sense of wrong logic, just an ordering/priority artifact of a guard that was never designed to give any one line priority over another.

**Fix applied (`diagnostics.cpp`, `print_timing_and_adc_block()`):** moved the `#if AD9851_ATTACHED` / `ad9851 breakdown` block to run immediately after the main `[timing]` line, ahead of the wakeup-jitter/`[core1]`/`[dsp]` breakdown lines, so it gets first claim on whatever room is left after the (unavoidably-first) main line, instead of last. This doesn't fix the underlying scarcity — on a sufficiently busy cycle some line will still lose the race — but it moves the newly-important line to the front of that queue instead of the back. Purely a print-order change; no timing/DSP/logic code touched. Confirmed brace-balanced and `#if`/`#endif` intact by re-reading the edited function; no compiler available in this sandbox so this needs a real build/flash to confirm the line now appears.

**Aside, worth checking on the bench regardless:** the `[diag] skip_total=` line (`diagnostics.cpp` ~line 636-639) reports the running total of lines dropped by every `diag_room_for()` guard in the whole diagnostics module. If that number is nonzero and climbing, it directly confirms the starvation mechanism above is real and ongoing (and is itself deliberately the last, most-starved line in its own block, so it can go missing too on a bad cycle — but if it's ever visible with a nonzero count, that's confirmation).

**Not yet bench-verified** — the reorder is a logical fix based on re-reading the exact guard/print-order code, not a rebuild-and-flash confirmation. Updated `diagnostics.cpp` delivered; next real step is flashing it and confirming the `ad9851 breakdown` line actually appears now.

## 2026-09-07, later still — the reorder worked, and the first real numbers reframe the whole CPU-margin question: the actual budget-eater is a ~32us stall that exists even at the true zero-overhead baseline, not the AD9851 write, not the DSP math, and not (solely) `ENVELOPE_INTERP_FACTOR`

First real capture after the `diagnostics.cpp` reorder, confirmed run at `ENVELOPE_INTERP_FACTOR=1`, `'I'` off (the true zero-overhead 16kHz baseline — no second clock domain, no extra ISR/notify, nothing this feature added running at all), `'D'`/`'a'`/`'A'` all on (confirmed against the "Live" preset's positional struct fields in `settings.h`: `env_predistort_enable=true`, `env_ampeq_enable=true`, `env_ampeq_shelf2_enable=true`), `gdeq=off` per the printed line itself (the "Live" preset defaults `env_gdeq_enable=true`, so this was presumably toggled off live — worth confirming, not asserted as fact here):

```
[timing]   ad9851 breakdown: prep_us=2 spi_us=17 (prep+spi=19 vs. write_us=33 above - gap is remaining driver/call overhead)
[timing] mode=TWO-TONE TEST gdeq=off max_busy_us=64 (adc=32 dsp=15 write=33) period_us=62 overruns=1
[timing]   wakeup jitter: max_gap_us=70 (nominal=62) late_ticks_total=0
[timing]   dsp breakdown: audio_fx=2 fir=6 atan2=2 sqrt=2
```

**AD9851 write and DSP math are both confirmed lean, real numbers now, not inference.** `prep_us=2 spi_us=17` (19us total) against `write_us=33` leaves a 14us residual for everything else that bucket covers — `envelope_floor_apply` (off, `env_floor=0.00`), `envelope_gdeq_process` (off per the printed line), `envelope_ampeq_process` (both shelves ON in this capture), `envelope_predistort_process` (ON), `relative_delay_apply` (always runs under `AD9851_ATTACHED` regardless of the specific delay value — `relative_delay_samples=2.00` here), `envelope_interp_on_full_tick`'s bookkeeping (curve compute skipped since `'I'` off, but some accounting may still run), the actual `ledc_set_duty()`+`ledc_update_duty()` pair, and the diagnostics/DAC-queue bookkeeping at the tail. 14us split across all of that, with two full ampeq shelves and the predistort LUT active, is already tight — no single big undiscovered lever in there. Similarly, `dsp breakdown: audio_fx=2 fir=6 atan2=2 sqrt=2` sums to 12us against a 15us `dsp` total — only 3us unaccounted, confirming the Hilbert FIR + fast-math atan2/sqrt optimizations already fully exploit that phase; nothing left to gain there either.

**The real finding: `adc=32` in TWO-TONE mode is not ADC cost — it's a stall, and it survives at the compile-time baseline that has none of the interp feature's extra ISR/notify machinery running at all.** TWO-TONE mode (confirmed via `test_signals.cpp`/`ssb_mic_test.ino`) never touches the ADC — the entire "adc" bracket at `ENVELOPE_INTERP_FACTOR=1` is `esp_timer_get_time()`, `diagnostics_record_tick_start()` (both `IRAM_ATTR`, the latter just a counter increment + gap calc, no locks), a plain `volatile` read via `dsp_state_get_audio_source()` (confirmed `IRAM_ATTR`, no critical section/mutex), and `generate_twotone_sample()` (confirmed `IRAM_ATTR`) — nanoseconds of real work, fully IRAM-resident, lock-free. A 32us high-water mark there is real time lost to something entirely outside this bracket's own code — these three phase numbers are independent high-water marks (never proven simultaneous), so this doesn't say the ADC bracket IS slow, it says *something* stalls `dsp_task` for ~32us often enough to get caught, and whichever bracket happens to be executing when it lands gets blamed. This lines up with the same-capture `overruns=1`: a normal tick here is roughly adc(~0)+dsp(15)+write(33)≈48us, comfortably inside the 62us budget — but a ~32us stall landing on any one phase blows straight through it.

**This is the decisive piece: the previously-leading suspect (the `gptimer` ISR + cross-core `vTaskNotifyGiveFromISR` overhead from the interp feature's 64kHz wake) cannot be the cause of THIS specific stall, because at `ENVELOPE_INTERP_FACTOR=1` none of that infrastructure exists — the real hardware tick rate here is the plain 16kHz `gptimer`, nothing extra.** This doesn't overturn the earlier factor=1-vs-4 finding (burble was real but lower-level at factor=1 than at factor=4 — see the entry above) — it refines it into a cleaner two-part picture: there is a **baseline stall mechanism, unrelated to `ENVELOPE_INTERP_FACTOR` entirely**, that's been present all along (this ~32us reading is presumably it, caught for the first time only because this capture happened to land on it), and the previously-measured wake-rate-alone effect from raising the factor to 4 is an *additional* cost stacked on top of this pre-existing baseline, not the sole cause of instability. Both the 10kHz-vs-16kHz bracket test's conclusion (raising the wake rate eats into a thin margin) and this new baseline-stall finding can both be true simultaneously — they're two different, both-real contributors to overruns at 16kHz, one scaling with `ENVELOPE_INTERP_FACTOR` and one not.

**Best-supported hypothesis for the baseline stall's actual source (not yet confirmed):** since the entire sampled code path is IRAM-resident and lock-free, this isn't a bug in this project's own hot-path code — it points at a genuine external interrupt/scheduling event preempting `dsp_task`, most plausibly either (a) a residual, smaller cousin of the already-fixed Core-1/Serial stall (see `timing_architecture.html`'s note on the confirmed 5ms Core-1 stall the per-line TX-buffer guard was built to kill — the guard stopped the worst all-or-nothing case, but an individual `Serial.printf()` call that DOES have buffer room can still cost real microseconds to enqueue, and Core 1 activity has already been shown capable of disrupting Core 0/`gptimer` timing on this hardware), or (b) ESP32-S3 cross-core flash-cache/bus arbitration when Core 1 executes flash-resident (non-IRAM) code concurrently with Core 0's real-time tick, or (c) ordinary FreeRTOS tick-ISR/scheduler housekeeping. Not distinguished yet.

**Recommended next step, cheap and decisive: re-run this identical capture with diagnostics muted (`'v'`) and see whether the 32us max/overrun disappears.** If muting Serial output makes it vanish, that confirms hypothesis (a) — a smaller residual version of the already-diagnosed Core-1/Serial interaction — and the fix is the same family as the just-applied reorder (reduce/throttle Core-1 Serial activity further). If it persists with diagnostics fully muted, that rules Serial out and points at (b) or (c) instead, which would need a different investigation (e.g., checking WiFi/BT radio state, `esp_timer` callback count, or Core 1 idle% via the existing `core1_idle_hook`).

**Reframes the "where's the CPU" question directly: it's not in the AD9851 write, not in the DSP math, and not really in the other write-bucket stages either — all three are confirmed lean with real numbers. The actual margin to be found is in hunting down this ~30us intermittent stall.** This also means SDM doesn't apply here at all — it would only ever address a per-call write-time cost, and this stall occurs in a bracket with zero register writes in it.

**Not yet resolved** — this is the next open item, pending the muted-diagnostics re-test.

## 2026-09-07, later still — added an on-demand diagnostics snapshot command (`'V'`), since the periodic 1Hz `[timing]` block turned out to be impractical for the proposed mute/unmute A-B test

User: "its not printing the timing data very often so hard to capture" — correctly identifying that the mute-based A/B test proposed above is awkward in practice: `diagnostics_service()`'s periodic block only prints once a second even when active, and `'v'` (mute) suppresses it entirely rather than just slowing it — so un-muting to read a result resumes an ongoing stream rather than giving one clean, deliberate read at exactly the moment wanted.

The fix doesn't touch the recording side at all — `diagnostics_record_tick_start()`/`diagnostics_record_phase_timings()` already run unconditionally from `dsp_task` every tick regardless of `s_diag_muted` (mute only gates the `Serial.printf()` side in `diagnostics_service()`), so the high-water marks were never actually unavailable, just hard to read on demand. Added `diagnostics_print_now()` (`diagnostics.cpp`/`.h`) — calls the same `print_status_line()`/`print_timing_and_adc_block()` used by the periodic path, but bypasses both the mute flag and the 45ms/1000ms throttle intervals, without touching either's own timers (so it doesn't perturb the periodic schedule) and without resetting any counters (a read, not a `'r'`). Wired to a new keystroke, `'V'` (paired with lowercase `'v'`'s mute toggle) — deliberately works even while muted, since that's the whole point: mute to keep Core 1 quiet, then hit `'V'` for one exact snapshot whenever wanted. Still goes through the same per-line `diag_room_for()` TX-buffer guards internally, so it can't block any more than the periodic path could.

Help banners updated in both `ssb_mic_test.ino` and `ssb_mic_test_preview.cpp` (both share `serial_commands.cpp`'s command dispatch, so only one place needed the actual `'V'` handler, but each `.ino`/`.cpp` prints its own inline banner text at boot).

**This also gives a cleaner version of the proposed A/B test than the mute/unmute one:** `'r'` (reset) → `'v'` (mute) → wait ~10s → `'V'` (one exact on-demand read of everything accumulated during the silent window, without ending the mute) → compare against a second `'r'` → ~10s unmuted/normal → `'V'` read. Same comparison as before (does `adc=`/`overruns=` differ between a Serial-silent window and a normal one), just no longer dependent on catching or waiting for the periodic stream.

**Not yet bench-verified** — added by re-reading and extending the existing throttle/mute logic exactly, no compiler available in this sandbox. Files delivered; next step is flashing and confirming `'V'` produces a clean immediate snapshot both muted and unmuted, then running the actual A/B comparison this was built for.

## 2026-09-07, later still — a new, better-grounded lead surfaces on its own: the ADC continuous driver runs unconditionally in EVERY audio mode including TWOTONE, and there's already a real-hardware-validated flag to isolate it

First `'v'`/`'V'` capture batch using the new command came back with `overruns=0` on all three periodic `[timing]` reads (`max_busy_us=45, adc=4, dsp=16, write=27` - identical, clean, no repeat of the earlier 32us stall) despite `skip_total` climbing 121->132->145->155->197 over the same short window - direct, hard confirmation that the per-line TX-buffer guard really is dropping print lines under load exactly as designed (Core-1 output side), while giving a genuine negative result on the "heavy Serial activity causes the dsp_task-side stall" hypothesis: this run had unusually heavy Serial/mute-toggle/on-demand-print traffic and the stall still didn't reproduce. Doesn't rule the hypothesis out (the stall is rare enough that one clean window proves nothing either way), but doesn't strengthen it either.

**What did turn up something concrete: `[adc]   fifo: available now min=4294967295 max=0 ... drop_total=893664`.** `min=4294967295` (`0xFFFFFFFF`) is the literal sentinel `s_dbg_adc_fifo_min_available` is initialized to in `adc_capture.cpp` and only ever overwritten once something actually drains that FIFO - confirming nothing does in TWOTONE mode. `drop_total=893664` is real and explained directly by re-reading `adc_capture_init()`/`adc_capture.cpp`: `adc_continuous_start()` is called **unconditionally at boot** regardless of `audio_source` (confirmed by the code's own comment on `adc_capture_service()`: "Runs unconditionally - the ADC is always active... regardless of the current audio source"). The ADC hardware, its DMA, and the `on_conv_done` ISR callback (`adc_conv_done_cb`, confirmed `IRAM_ATTR`, integer-only) all run continuously at the full ADC rate (~80kHz, matching the same capture's `[adc] actual=80638 sps` line) whether or not `dsp_task` ever reads the result. TWOTONE mode never calls `adc_capture_read_next_sample()` (confirmed earlier this session), so this project's own downstream software ring-buffer FIFO (distinct from the ESP-IDF driver's own internal pool) fills once at boot and then every subsequent ISR callback hits the "FIFO full" branch and drops the entire frame - continuously, at full ISR rate, for the whole time TWOTONE is active. `pool_ovf_total=0` only confirms the separate Core-1 polling drain (`adc_capture_service()`, called every `loop()` iteration) is successfully keeping the *driver's* internal pool from overflowing - it says nothing about whether the ISR firing thousands of times a second while completely unused costs anything on the real-time side.

**There's already a built-in, real-hardware-validated way to isolate this: `config.h`'s `ADC_CAPTURE_ENABLED` flag.** Its own comment states it was added for exactly this kind of isolation - a prior GPIO13 5kHz electrical-coupling investigation - and skips `adc_continuous_start()`/callback registration entirely so the ISR never fires at all (not just gating a debug pin). **Confirmed on real hardware 2026-09-02** to eliminate that ADC-driver side effect when set to 0 in a non-mic mode (two-tone/chirp); mic-source audio just reads as 0.0f while off, matching the existing AMTEST/FMTEST/ENVSTEP "unused sample" convention, and TWOTONE doesn't touch that path regardless.

**Recommended next test, free (no new code): set `ADC_CAPTURE_ENABLED=0`, stay in TWOTONE, `ENVELOPE_INTERP_FACTOR=1`/`I` off/`D`+`a`+`A` on/`gdeq` off (the exact config that caught the 32us stall/overrun=1 originally), and repeat the capture.** If the stall/overrun stops appearing (or drops meaningfully in frequency across several captures - a single clean window doesn't prove anything given how rare it is), that identifies a real, physically sensible, already-fixable mechanism: a continuously-firing, completely wasted ISR/DMA stream in every non-mic test mode. If it persists unchanged with the ADC driver fully off, that rules this out cleanly and the Serial-print hypothesis (or something else) moves back up the list.

**Superseded below, same day - a much simpler, already-on-record explanation fits the actual pattern of evidence better than either the Serial-print or ADC-driver hypothesis.**

## 2026-09-07, later still — correction: the 32us `adc` stall + `overruns=1` was very likely the project's own already-documented one-time cold-boot/first-tick warm-up cost, not an ongoing per-tick mechanism at all

Three more capture batches since the original 32us/overrun=1 reading - a heavy `'v'`/`'V'`-toggle burst (skip_total climbing 121->197 under real load) and a fresh-reset batch on `Preset3`/"Live" (`D`+`a`+`A` on, `gdeq`=ON this time) - have ALL come back completely clean: `overruns=0`, `adc=4`, repeatedly, identically, regardless of Serial load or toggle state. The 32us stall has not recurred even once since the original reading.

**The pattern across all the evidence now on hand: the stall happened exactly once, on the very first reading captured immediately after reflashing with the diagnostics.cpp fix, and never again since - across multiple later windows, under both light and heavy load.** This exact qualitative shape - one overrun right at the start, on literally the first real tick after boot, never recurring afterward - is something this project already has on record and already understands: `ssb_mic_test.ino`'s/`ssb_mic_test_preview.cpp`'s own setup() comment documents "a fresh boot showed a single ~264-265us `spi_us` spike (reproducible across independent reboots - a deterministic cost, not a scheduling fluke) on exactly the first `dsp_task` tick, causing exactly one overrun/late tick, never recurring afterward," attributed there to i-cache warm-up on the AD9851 driver's ESP-IDF internals not yet having run on `dsp_task`'s own core at that point. That specific case landed in the `write`/`spi_us` bucket; this case landed in `adc` instead - but the underlying mechanism (a one-time i-cache/driver-init cost on the very first tick, landing wherever the first real work for that particular code path happens to fall) is the same general, already-known phenomenon, just manifesting in a different bucket because a different combination of first-time code paths got exercised on this particular boot.

**This is a better fit for every data point collected than either of the two hypotheses chased above.** It explains why the stall never recurred despite deliberately heavy Serial/print load (a cold-start cost has nothing to do with ongoing Serial activity) and why disabling the ADC driver was never actually tested against a genuine repeat occurrence (there wasn't one to test against). It also means the ADC-continuous-driver-runs-unconditionally finding, while still true and still worth cleaning up eventually as a matter of hygiene (an 80kHz ISR doing nothing useful in every non-mic test mode isn't good practice), is probably NOT what caused this specific reading.

**Good news for the actual open question:** this means the true steady-state `ENVELOPE_INTERP_FACTOR=1`/16kHz baseline looks genuinely clean on the evidence collected so far - `adc=4`, `overruns=0`, repeatable across several windows and load conditions - which reinforces rather than undermines the original margin story from the 10kHz-vs-16kHz bracket test: the baseline itself isn't secretly flaky, and raising the factor to 4 is what eats the already-thin 16kHz headroom, not some hidden baseline-level defect.

**Decisive, cheap way to confirm this cleanly:** reset (`'r'`) while the board has already been running a while (NOT immediately after a fresh flash/boot) in the exact `D`+`a`+`A`-on, `gdeq`-off config that originally showed the stall, and watch whether it ever reappears. If it's genuinely cold-boot-only, it should not, regardless of toggle state or Serial load.

**CONFIRMED, 2026-09-07, same day.** User ran exactly this - a warm `'r'` reset (explicitly confirmed "genuine same run", no reflash/power-cycle in between) on the "Live" preset (`D`+`a`+`A` on, `gdeq`=ON this time), several `'V'` on-demand reads plus one periodic capture. All came back clean: `overruns=0`, `adc=4`, matching every other warm-reset capture since the original reading. That's now three separate warm sessions with zero recurrence against exactly one occurrence on the literal first tick after a fresh reflash. **Closing this out: the 32us `adc` stall/`overruns=1` reading is confirmed a one-time cold-boot/first-tick i-cache-warmup cost, not an ongoing per-tick mechanism.** The `ENVELOPE_INTERP_FACTOR=1`/16kHz baseline is genuinely clean. (Also worth noting for the record: one of the later snapshots in this run showed `delayed=-6657.7Hz`/`tx_freq` dropping ~7955Hz below nominal - not a new issue, this is the same already-documented two-tone null-crossing excursion `MAX_FREQ_DEV_HZ`=8000Hz was raised to accommodate, matching the ~6698Hz `max_unclamped` figure already on record; it just landed in the `delayed` field this time instead of the raw `freq_dev` field.)

## 2026-09-08 — first concrete step back toward `ENVELOPE_INTERP_FACTOR=4` at 16kHz: retuned the LEDC PWM carrier from 78125Hz to 64000Hz, isolated, no other change

With the cold-boot question closed out above, came back to the user's standing objective ("get back the x4 interp") and worked through candidate levers grounded in this project's own history rather than generic advice. Three were identified: (1) do the interp-tick's ramp+duty-write directly inside the gptimer ISR on Core 1 (raw-register LEDC write + fixed-point math, avoiding the two mechanisms that crashed v1/v2 - the LEDC driver isn't ISR-safe, float isn't ISR-safe on this chip), cutting the cross-core notify rate back from 64kHz to 16kHz; (2) revisit v3 (the LEDC hardware fade engine, abandoned over ~12.8us of unsynchronized jitter against the sample gptimer) on the theory that the jitter's real cause was an arbitrary, non-commensurate carrier frequency (78125Hz = 80MHz/1024, chosen only as "fastest achievable at 10-bit resolution," not for any relationship to the tick rate) rather than something inherent to hardware-driven fades - 64000Hz (`ENVELOPE_INTERP_FACTOR`(4) x `SAMPLE_RATE_HZ`(16000)) is *also* an exact integer division of the same 80MHz APB clock (80,000,000/1250), so putting both clocks on commensurate footing might remove the drift instead of just reducing it, and doesn't cost the interpolation feature's own justification - the -63dB reconstruction-filter figure that motivated it was already computed AT 64kHz, not 78125Hz; (3) free background cleanup, `ADC_CAPTURE_ENABLED=0` in non-mic test modes (a confirmed continuously-running, entirely-wasted 80kHz ISR - see the earlier entry - real overhead even though it wasn't this stall's cause).

User chose to test (2) first, but deliberately isolated to just the carrier retune alone - no hardware-fade-engine work yet, still the existing v4 software-driven `ledc_set_duty()`/`ledc_update_duty()` path. Changed `RSET_MOD_LEDC_FREQ_HZ` (`envelope_output.h`) from `78125` to `64000`, with the reasoning dated inline. Cross-checked for stale references to the old fixed value elsewhere: `envelope_interp.h`'s v4.2 "hard ceiling" paragraph (the one arguing `ENVELOPE_INTERP_FACTOR=8` is a dead end because "`RSET_MOD_LEDC_FREQ_HZ`... fixed at 78125Hz... is a hard ceiling") explicitly called out 78125Hz as fixed - appended a dated update there rather than editing it away: the specific ceiling NUMBER is now stale, but the shape of that argument (raising the factor further would write duty faster than whatever the carrier is) still holds regardless of this particular retune, so the 8x conclusion itself doesn't need revisiting. The v1-v4 history paragraphs describing v3's ORIGINAL 78125-vs-64000 jitter finding were left untouched - that's accurate history of what was true at the time, not a claim about current config.

**Not yet bench-verified either way** - purely a code change grounded in the clock-divisor arithmetic (78125=80e6/1024, 64000=80e6/1250, both exact integer divisions of the same reference), not yet confirmed against real hardware whether: (a) the ESP-IDF LEDC driver's `LEDC_AUTO_CLK` path actually lands on exactly 64000.000Hz at 10-bit resolution given its fractional-divider precision (a residual few-Hz error is possible even if the target is achievable in principle - worth reading back the driver's actual achieved frequency via `ledc_get_freq()` if the retune alone doesn't fully resolve things), (b) this alone (with the software-driven update path unchanged, no hardware fade engine yet) has any measurable effect at all on the 16kHz+factor=4 burble, since the original diagnosed mechanism there was cross-core notify/wake-rate overhead, not carrier-vs-tick-rate mismatch - this test is really aimed at candidate (2)'s hardware-fade-engine idea and needs that follow-up step to be a fair test of the full theory; changing the carrier alone with the current software path is a cheap, safe, single-variable first look, not expected by itself to fix the wake-rate mechanism. Next: flash, confirm no regression on the existing software-driven PWM output (scope GPIO2/RSET_MOD_LEDC_GPIO, listen for the two-tone signal as a sanity check), then decide whether to proceed to reintroducing v3's `ledc_set_fade_with_step()`/`ledc_fade_start()` hardware path with a synchronized timer start.

**2026-09-08, same day: confirmed on real hardware - no regression.** User: "OK that works." The 64000Hz carrier retune builds and runs cleanly through the existing software-driven `ledc_set_duty()`/`ledc_update_duty()` path with no reported issue - step 1 of candidate (2) is a safe, confirmed no-op change on its own (as expected, since it doesn't yet touch the wake-rate/cross-core-notify mechanism). Cleared to proceed to the actual test of the theory: reintroducing v3's hardware LEDC fade engine (`ledc_set_fade_with_step()`/`ledc_fade_start()`), now with a carrier that's commensurate with the 64kHz tick rate, plus a synchronized timer start (`ledc_timer_rst()` at the same instant `init_sample_timer()` starts the gptimer) so the two count from a known common phase rather than just matching in nominal frequency. This is the step that actually tests whether the ~12.8us v3 jitter goes away - not yet done.

## 2026-09-08, later still — v5: reintroduced v3's hardware LEDC fade engine, gated behind a new `ENVELOPE_INTERP_USE_HW_FADE` flag (off by default, not yet bench-verified)

User: "yes please", confirming the step proposed at the end of the previous entry. Full design and rationale is written up as a dated comment block in `envelope_interp.h` right after `#define ENVELOPE_INTERP_FACTOR 4` (search `ENVELOPE_INTERP_USE_HW_FADE` there for the complete version, including the three known limitations) - short version here for the notes-file record:

**The core idea:** stop trying to shrink the 64kHz cross-core wake-rate cost (v4's actual mechanism) and instead remove it entirely, now that step 1 above (the commensurate 64000Hz carrier retune) makes v3's original hardware-fade approach worth a second look. Three coordinated changes, all gated behind the same flag so the entire path compiles out cleanly to today's confirmed-working v4 behavior when off:

1. **`ssb_mic_test.ino`'s `on_timer_alarm()` ISR** (already done in the previous implementation step, before this session's summary) - throttles `vTaskNotifyGiveFromISR()` to fire once every `ENVELOPE_INTERP_FACTOR` real timer alarms instead of every alarm, so `dsp_task` goes back to being woken at plain 16kHz even though the underlying gptimer hardware still physically fires at 64kHz exactly as today (deliberately NOT slowing the timer itself back down - see limitation #2 below, CHIRP mode still needs the raw 64kHz timer rate at the config level even though it can't use this flag's notify-throttling).
2. **`dsp_task`'s `is_full_tick`** (also already done) - unconditionally `true` under this flag, since every wake is now guaranteed to be a full tick.
3. **New this step - the actual output-path hookup:**
   - `envelope_output.h`/`.cpp`: two new functions, `envelope_output_start_hw_fade(float target_envelope, uint32_t steps)` and `envelope_output_sync_ledc_timer_now(void)`. The first computes a target raw duty count the same way `envelope_output_write_pwm()` does, reads the LEDC channel's CURRENT duty via `ledc_get_duty()`, and calls `ledc_set_fade_with_step(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, target_duty, scale, 1)` + `ledc_fade_start(..., LEDC_FADE_NO_WAIT)`, where `scale` (a per-step DUTY COUNT, not a step count) is backed out as `|target_duty - current_duty| / steps` (floored, minimum 1) - so the real number of hardware steps taken is only approximately `steps`, not exact, same caveat already flagged when the function was first declared. `cycle_num=1` (advance every LEDC PWM period) is what actually spreads the fade across `steps` full 64kHz periods, which is exactly one `ENVELOPE_INTERP_FACTOR`-wide gptimer interval given the retuned commensurate carrier. Respects the same `s_duty_override_enabled` early-return as `envelope_output_write_pwm()`. The second function just wraps `ledc_timer_rst(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_TIMER)`.
   - `init_rset_mod_pwm()` (`envelope_output.cpp`) now calls `ledc_fade_func_install(0)` once at init, gated by the flag - required by ESP-IDF before any `ledc_set_fade_*()`/`ledc_fade_start()` call works at all; no such call existed anywhere in the codebase before this, since nothing used LEDC hardware fades previously.
   - `envelope_interp_on_full_tick()` (`envelope_interp.cpp`) now has a flag-gated early-return branch, placed right after the existing `s_p0..s_p3`/`s_m1`/`s_m2` history update (kept updating regardless, same "few wasted FLOPs" tradeoff v4.4's HOLD mode already accepts, so nothing goes stale if the flag is ever flipped off again without a reflash): calls `envelope_output_start_hw_fade(envelope, ENVELOPE_INTERP_FACTOR)` and returns, bypassing `compute_ramp_value()` and the whole curve (CATMULL_ROM/LINEAR/HOLD) machinery entirely - not just overriding LINEAR, since a hardware fade is inherently a straight-line ramp regardless of which curve `'C'` has selected.
   - `envelope_interp_on_interp_tick()` got a defensive flag-gated early-return too (should never actually be called under this mode, since there's no wake event left to call it from once every alarm notifies a full tick - but a cheap defensive no-op costs nothing).
   - `init_sample_timer()`'s call to `envelope_output_sync_ledc_timer_now()` right after `gptimer_start()` was already added in the previous implementation step.

**Explicitly documented, unresolved risks (all already called out in `envelope_interp.h`'s flag comment, repeated here for the notes-file record):**
- `ledc_set_fade_with_step()`'s exact signature/semantics were written from memory against ESP-IDF's `driver/ledc.h` (`speed_mode, channel, target_duty, scale, cycle_num`), not verified against the user's actually-installed toolchain version - a signature mismatch is a clean compile error (easy), but a semantic mismatch in what `scale` means on some IDF version would silently mis-shape the ramp instead (harder to catch, not verified on this bench).
- Curve shape (`'C'`) becomes moot while this flag is on - hardware fade is inherently linear. Real hardware from 2026-09-02's write-rate-isolation table already showed LINEAR beating Catmull-Rom in one test, so this isn't obviously a downgrade, just an honest tradeoff.
- Incompatible with CHIRP test mode (`'w'`) as implemented - that mode needs every 64kHz tick regardless of `is_full_tick`, which this flag's throttled notify would silently break. Revert the flag to 0 before any chirp/TF-analyzer sweep.
- The unresolved "CORROBORATION" caveat from this file's v3-history entries: a separate experiment once found co-locating `dsp_task` with `loop()` on Core 1 starved Serial completely even with `'I'` off (no 64kHz wake at all), meaning v3's true failure mode was never conclusively pinned to wake-rate/CPU overhead alone. This design doesn't move `dsp_task`'s core and doesn't add a new task/ISR (sidestepping the SPECIFIC mechanism that broke v3's crash-prone stages), but that's not the same as a guaranteed fix for whatever the Serial-starvation experiment actually hit.

**Off by default (`ENVELOPE_INTERP_USE_HW_FADE 0`), not yet bench-verified in any form.** Flip to 1, reflash, and check: (1) it actually compiles against the real installed `driver/ledc.h`; (2) real hardware confirms/refutes the ~12.8us jitter is gone (scope the RSET/PWM output directly, and/or watch two-tone IMD3 the way the 2026-09-02 write-rate table did); (3) whether `'I'` can now stay on with `ENVELOPE_INTERP_FACTOR=4` at 16kHz without the burble/starvation that originally forced this whole investigation - the actual, original objective.

## 2026-09-08, later still — DECISIVE negative result: v5 compiles and runs, but the burble/raised-noise-floor is back at essentially the same severity, with dsp_task's own wake-rate overhead now completely eliminated. This falsifies v5's entire premise and reframes the whole investigation.

**User's report:** "Compiles and runs. Twotone IMD test is the same but the burble is back with the same raised noise levels immediately around the tone. I'm looking +/-100Hz with ~+20dB higher noise floor compared with best 16kFs (x1) case)."

**Why this result is unusually clean and decisive, more so than any single measurement so far in this investigation:** re-reading the full 2026-09-07 "reopening the interp/wake-rate question" thread above (search that heading) turns up that this project had already isolated TWO independent, stacking contributors to the factor=4 burble:

1. **A wake-rate-ALONE cost** (zero LEDC writes, `'I'` off, only the extra no-op gptimer/cross-core-notify wakes running) - confirmed via the tone-frequency-shift logic (a pure software oscillator can only drift if its calling rate is irregular) to be `dsp_task` intermittently missing its own ~62.5us deadline at 16kHz specifically (16-17.5us of headroom, eaten by 3 extra no-op wake/check cycles per real tick) - NOT present at 10kHz (55-58us headroom absorbs the same fixed overhead comfortably). Measured as **present but the SMALLER of the two effects** ("burble was there albeit at a lower level than when enabling interp writes").
2. **A separate write-rate cost** (register writes / changing duty values at 64kHz, independent of any margin problem) - measured at 10kHz+factor=4 (ample margin, zero overrun risk) via the 2026-09-02 HOLD/LINEAR/Catmull-Rom table: HOLD +2.0dB, LINEAR +11.5dB, Catmull-Rom +14.2dB degradation, all just from touching/re-triggering the LEDC duty register more often, even with `dsp_task` timing itself perfectly clean.

**v5's entire design was built to eliminate mechanism (1) and ONLY (1)** - `dsp_task` is back to being woken at a genuine, uninflated 16kHz (zero extra no-op wakes, zero cross-core notify overhead on the interior sub-ticks), with the interior stepping handed to the LEDC hardware fade engine, which runs entirely autonomously with **zero CPU/ISR/software involvement per step** - a strictly cleaner elimination of mechanism (1) than even the "`'I'` off, factor=4" isolation test achieved, since that older test still paid the no-op wake/notify/IPI cost on every extra tick, while v5 pays literally nothing (no wake happens at all on the interior ticks).

**Result: the burble came back at essentially the SAME severity as the full software-ramp `'I'`-on case (`+20dB` near-tone noise floor), not the smaller magnitude the wake-rate-alone isolation test found (mechanism (1) alone).** This is the decisive triangulation point: with mechanism (1) now fully and cleanly eliminated (not just reduced - genuinely zero extra software/CPU activity happening on the interior ticks), the residual burble can ONLY be mechanism (2) - and it's showing up at (or near) FULL severity, not scaled down. That means mechanism (2) - the write/switching-activity-rate cost - was always the dominant contributor to the audible/measured degradation at 16kHz+factor=4, not mechanism (1) as this whole v5 architecture assumed. Removing all CPU/timing overhead from the picture did essentially nothing, because CPU/timing overhead was never the main story here.

**This also reframes what "mechanism (2)" most likely physically is.** `ledc_set_fade_with_step()`'s hardware fade still does exactly what the software ramp did electrically - update the RSET LEDC channel's duty register at ENVELOPE_INTERP_FACTOR (4) times the real sample rate - just via the peripheral's own autonomous state machine instead of `ledc_set_duty()`/`ledc_update_duty()` calls from `dsp_task`. The fact that moving that same duty-register activity from software-driven to fully hardware-autonomous changed essentially nothing about the resulting noise floor is strong evidence this was never a software/firmware timing problem to begin with - it points at something intrinsic to switching the RSET/PWM analog path's duty more often, REGARDLESS of what drives it. This is not a new or exotic idea for this specific project: **`envelope_output.h`'s own top-of-file header comment already documented this exact concern as the reason PWM+RC was abandoned in favor of the (currently disconnected) DAC path in the first place** - "at a -70dBc spurious target, a switched (PWM) envelope needs either an impractically high switching frequency or a multi-pole filter to get there on filter math alone, and in practice parasitic coupling of the switching edges tends to dominate at that level regardless of filter order." Four hardware-fade steps at 64kHz vs. one plain write at 16kHz is a real difference in how often (and how densely-spaced) analog switching transients occur near the RSET node - if THAT coupling is the mechanism (rather than anything about how the digital duty value is computed), then no firmware architecture at all - software ramp, hardware fade, or a hypothetical ISR-driven raw-register write - can fix it, because it isn't a firmware problem.

**What this does NOT (yet) rule out, in fairness to the single-test caveat:** this is one bench observation, not yet cross-checked against the `[timing]` overrun/late-tick counters in this exact HW_FADE configuration (worth doing - if `overruns`/`late_ticks_total` are also clean now, that would further corroborate mechanism (1) being genuinely gone rather than just assumed gone from the design). It's also not yet confirmed via a HOLD-equivalent hardware-fade test (fading toward the SAME value repeatedly, isolating "touching the register" from "the value changing") - if that distinction still matters at all given the software HOLD result already showed some (smaller, +2.0dB) degradation from touch-count alone even without a changing value.

**Read against the original, standing objective ("get back the x4 interp"):** this is the strongest evidence yet that `ENVELOPE_INTERP_FACTOR=4` on the PWM/RSET output path specifically (not the DSP/timing side, which v4/v5 have now both shown can genuinely be made clean) runs into a real, likely electrical/parasitic-coupling ceiling that isn't a matter of finding a cleverer firmware architecture - v1 through v5 have now tried, in order: ISR-direct (crashed), deferred-task (crashed), hardware-fade-v3 (starved serial), pure-software-ramp-v4 (burbles, margin-limited at 16kHz), and now hardware-fade-v5 with the margin problem provably solved (burbles anyway, same severity). **Candidates worth considering next, not yet acted on:**
1. Directly test the "parasitic coupling of switching edges" theory: try `ledc_set_fade_with_step()` fading toward the SAME value repeatedly (a hardware-equivalent of the old software HOLD test) at 64kHz, to see whether just re-triggering fades (with no target CHANGE) reproduces the same ~+20dB, a smaller amount, or none at all - would further isolate "touch-rate" from "value-change-rate" as the specific culprit.
2. Accept `ENVELOPE_INTERP_FACTOR=1` (16kHz, x1) as the practical ceiling for the PWM/RSET path specifically, and treat the -63dB-at-64kHz ZOH-imaging benefit that originally motivated this whole feature as unreachable on this hardware without a different output stage - i.e., stop trying to fix interpolation on PWM/RSET and instead ask whether the existing envelope_gdeq/predistort machinery (already built, already partially validated) gets "close enough" within the 16kHz budget.
3. The DAC path already exists in this codebase (`envelope_output.h`/`.cpp`'s MCP4725 support) specifically because it has "no switching-frequency energy to suppress in the first place" - currently physically disconnected ("DAC hardware removed"), but if the parasitic-coupling read above is correct, reconnecting it may be the only way to actually reach a genuine x4 (or higher) envelope update rate cleanly. A real hardware change, not a firmware-only fix - a bigger step than anything tried so far in this thread.

**Not yet decided which of these (if any) to pursue** - this is a live discussion point, not a completed action.

## 2026-09-08, later still — user's own follow-up test proposed and set up: hardware fade at `ENVELOPE_INTERP_FACTOR=1` (x1 16kFs), isolating the fade MECHANISM from the write RATE

User: "We havent tried x1 16kFs with fade?" - a sharper isolation than candidate 1 in the previous entry's list. That candidate (repeated-fade-to-same-value at 64kHz) would isolate "value changing" from "register touched," but still at the elevated 64kHz rate. This test instead holds the WRITE RATE fixed at the already-confirmed-clean x1 16kHz baseline and changes only the MECHANISM used to perform that one write per tick - plain `ledc_set_duty()`+`ledc_update_duty()` (`'I'` off) vs. `ledc_set_fade_with_step()`+`ledc_fade_start()` with `steps=1` (`'I'` on) - a genuinely different variable than anything tested so far in this thread (rate has been the only variable varied until now; this varies mechanism at matched rate).

**Why `steps=1` is a fair single-write comparison, not still an interpolation test:** at `ENVELOPE_INTERP_FACTOR=1` there is no sub-tick interpolation to do at all (every tick is already a "full" tick) - confirmed by re-reading `dsp_task`'s `is_full_tick` logic and `on_timer_alarm()`'s ISR-throttle counter, both already exact no-ops at factor=1 regardless of `ENVELOPE_INTERP_USE_HW_FADE` (`s_isr_fast_tick_counter >= 1` fires the notify on literally every alarm, same as the always-notify path). With `ENVELOPE_INTERP_USE_HW_FADE=1`, `envelope_interp_on_full_tick()`'s hardware-fade branch calls `envelope_output_start_hw_fade(envelope, ENVELOPE_INTERP_FACTOR)` = `envelope_output_start_hw_fade(envelope, 1)` - `scale` (the fade engine's per-step duty count) comes out equal to the FULL current-to-target distance in one step, `cycle_num=1` (one PWM period per step) - so the fade reaches its target after exactly one PWM period, essentially the same instant a plain `ledc_update_duty()` would take effect anyway (LEDC's own duty register is itself double-buffered and applies at the next PWM period boundary regardless of which API loaded it). So `'I'` OFF and `'I'` ON at this build are the SAME write rate, SAME target values, SAME approximate timing - the only real difference is which LEDC API path is used to apply each write.

**Confirmed this needed zero new code** - purely a two-constant config change, both in `envelope_interp.h`:
- `ENVELOPE_INTERP_FACTOR`: `4` -> `1` (temporary test value, dated comment added, marked for revert).
- `ENVELOPE_INTERP_USE_HW_FADE`: `0` -> `1` (temporary test value, dated comment added, marked for revert).

Cross-checked every other reference to `ENVELOPE_INTERP_FACTOR` in the codebase (`test_signals.cpp`'s chirp fast-rate, `config.h`'s CHIRP_F1_HZ-vs-factor warning, `init_sample_timer()`'s `resolution_hz`/`alarm_count`, `serial_commands.cpp`'s banner prints) for anything that would break or silently misbehave at `FACTOR=1` combined with the flag on - nothing does. `resolution_hz`/`alarm_count` at factor=1 come out identical to the long-proven, already-shipped factor=1 timer config (2,000,000Hz / 125 counts = clean 16kHz, same as the historical "`'I'` off" baseline always used). CHIRP mode's factor>=4 requirement (`config.h`) is irrelevant here since CHIRP is already independently declared incompatible with this flag and isn't part of this test (TWOTONE is). The one minor inconsistency, noted but harmless: `init_sample_timer()`'s `envelope_output_sync_ledc_timer_now()` call still fires (gated only by the flag, not by `FACTOR`), and its own comment says it's "only meaningful now that `RSET_MOD_LEDC_FREQ_HZ` is commensurate with `ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ`" - at `FACTOR=1` that specific relationship (64000 = 1x16000) no longer literally holds, but since there's no multi-step interior ramp happening at `steps=1` for phase-locking to matter for, the sync call is a functional no-op either way here, not a source of any expected artifact.

**Test setup delivered; not yet run.** `'I'` OFF = confirmed-clean x1 baseline (unchanged code path). `'I'` ON = same write rate/target values, routed through the hardware fade engine instead. A/B both live via the `'I'` toggle, no rebuild needed between them, after one flash of this build. **If `'I'` ON stays clean:** the fade engine itself is exonerated, and the earlier factor=4 degradation is specifically about write RATE (and/or ramping through intermediate values, neither of which happens at `steps=1`) - strengthens the parasitic-switching-coupling read from the previous entry. **If `'I'` ON degrades even here:** that's a different, more specific finding - something about the fade mechanism itself (its own internal duty-scaler/gate timing, or the fact that it's driven by a different internal LEDC state machine than a plain register write) adds noise independent of rate, which would redirect the investigation away from "raw switching-edge count" and toward "this specific peripheral feature has its own cost."

**Remember: revert both `ENVELOPE_INTERP_FACTOR` (back to 4) and `ENVELOPE_INTERP_USE_HW_FADE` (back to 0) once this test is done** - both dated comments in `envelope_interp.h` flag this explicitly as a temporary test build, not a new default.

## 2026-09-08, later still — CORRECTION: the factor=4 hardware-fade result above was run with `'I'` OFF by mistake, which changes its meaning; the corrected re-run plus the x1-with-fade result together give a much clearer (and more negative) picture. Both temporary `#define`s reverted to their normal values.

**User: "Sorry I did the first test incorrectly in that I left I off. With I on it completely destroys the two tones - very high noise levels (delay adjusted). If I goto x1 clock and apply I I get noise again, not as bad as x4 case but bad enough."** Three pieces of information here, all real and all needing to be folded in without deleting the earlier (now-understood-to-be-mislabeled) entry - per this project's own convention, corrected in place with a dated note, not edited away.

**What "the first test" (factor=4, `ENVELOPE_INTERP_USE_HW_FADE=1`, `'I'` OFF) actually measured, now that the mislabeling is known:** `envelope_interp_on_full_tick()`'s hardware-fade branch sits AFTER the `if (!s_enabled) { ...; return; }` early-out (confirmed by re-reading `envelope_interp.cpp`) - so with `'I'` off, `envelope_output_start_hw_fade()` was never called even once during that run. What WAS still true in that config: `ENVELOPE_INTERP_FACTOR=4` unconditionally configures the physical `gptimer` at `resolution_hz=8,000,000`/`alarm_count=125`, i.e. a genuine 64,000/sec hardware alarm rate, regardless of this flag - the flag only changes whether `on_timer_alarm()`'s ISR bothers to call `vTaskNotifyGiveFromISR()` on any given alarm (throttled to 1-in-4 when the flag is on). So that "burble at full severity" reading was produced by: the raw `gptimer` ISR physically firing 64,000 times/sec (entry, counter compare/increment, exit - nothing else) with the cross-core notify AND `dsp_task`'s own wake skipped on 3 of every 4 of those firings, and exactly one plain `envelope_output_write_pwm()` call per real 16kHz full tick (same call the confirmed-clean x1 baseline always makes) - genuinely nothing LEDC-fade-related happened at all.

**This sharpens, rather than overturns, the pre-existing 2026-09-07 "wake-rate-alone" finding (`ENVELOPE_INTERP_FACTOR=4`, `'I'` off, no HW_FADE flag at all) - it narrows down WHICH part of that bundle is doing the damage.** That older isolation test had the FULL bundle running at 64kHz every tick: the raw ISR entry, the cross-core `vTaskNotifyGiveFromISR`/IPI, AND `dsp_task`'s own wake-check-return cycle on the 3 no-op ticks - and concluded "one or more of these three costs real time." This new (mislabeled but still informative) result has the cross-core notify and the `dsp_task`-side wake genuinely ELIMINATED on 3 of 4 ticks (not just cheap - actually skipped), leaving only the bare timer-ISR entry/exit itself running at 64kHz - and still reproduced something close to the full-severity symptom. That's a real, useful, more specific finding even though it was reached by accident: **the bare 64kHz gptimer ISR firing that often, by itself, with nothing else added, is sufficient (or very close to sufficient) to cause the burble at 16kHz+factor4** - not the cross-core notify/IPI overhead specifically, and not `dsp_task`'s own wake-check cost, both of which were absent in this run. Most plausible remaining mechanism: ISR entry/exit itself (register save/restore, i-cache line touches for the ISR's own code) stealing enough cycles/latency from Core 0's real-time budget at 16kHz's already-thin 16-17.5us margin - consistent with everything already on record about that margin being the actual scarce resource here.

**The corrected, properly-labeled re-run (factor=4, HW_FADE=1, `'I'` genuinely ON) came back categorically worse: "completely destroys the two tones - very high noise levels."** This is real hardware-fade engagement this time - `envelope_output_start_hw_fade()` now actually called, ramping duty across 4 LEDC-hardware-driven sub-steps per real tick, autonomously, at 64kHz. Going from "burble, +20dB near-tone" (bare-ISR-only, previous mislabeled run) to "completely destroys the two tones" (bare-ISR PLUS actual hardware fade engagement) is a large, clearly additive jump - confirming the hardware fade mechanism itself contributes a large, separate cost on top of whatever the bare-ISR-rate effect above already costs, not a smaller or comparable one.

**The clean, correctly-labeled x1-with-fade test (this entry's other half, set up in the previous session turn) gives the sharpest single data point of the whole thread: "If I goto x1 clock and apply I I get noise again, not as bad as x4 case but bad enough."** At `ENVELOPE_INTERP_FACTOR=1`, the physical `gptimer` itself only fires at 16kHz - identical to the confirmed-clean baseline's own ISR rate, with NO throttling needed or present (every alarm already notifies). `'I'` OFF here is the literal unchanged baseline path (`envelope_output_write_pwm()`). `'I'` ON routes that same once-per-tick, same-target-value write through `envelope_output_start_hw_fade(envelope, 1)` instead - a single-step fade landing at essentially the same PWM-period boundary a plain write would. Every other variable (wake rate, ISR rate, write rate, target values, approximate write timing) is matched between the two conditions; the ONLY difference is which LEDC API performs the write. **Real, audible noise appeared anyway - smaller than the factor=4 case, but "bad enough."** This is the cleanest possible isolation available on this hardware, and it says something new and specific: the LEDC hardware fade peripheral is not simply "a lower-CPU-cost way to do the same electrical duty write" - something about how it operates (a different internal state machine/comparator path than the plain double-buffered duty register `ledc_set_duty()`/`ledc_update_duty()` uses, possibly extra internal transitions even for a single logical step) costs real noise on its own, independent of rate.

**Composite picture now, corrected and much clearer than before this exchange:** at least two independent, additive contributors to the factor=4+`'I'`-on degradation, now separable:
1. **Bare 64kHz timer-ISR-entry cost** - real on its own (the mislabeled run's actual finding), present any time `ENVELOPE_INTERP_FACTOR=4` configures the physical timer that fast, regardless of what (if anything) happens on the notified ticks.
2. **LEDC hardware-fade-engine-inherent cost** - real on its own even at a genuinely matched 16kHz rate (the x1-with-fade result), independent of #1 entirely.
Both stack at factor=4+`'I'`-on (likely with #2 also scaling with the 4x-higher step rate on top of its own base cost), which is presumably why that combination reads as "completely destroys the two tones" rather than merely additive-sounding.

**This changes yesterday's "parasitic switching-edge coupling, mechanism-agnostic" framing in a specific way worth being honest about:** that read was built on the (mistaken) premise that #1 (wake-rate/CPU cost) had been cleanly eliminated in the factor=4 test, leaving only a rate-dependent write/coupling effect to explain the result. That premise was wrong - #1 was never actually eliminated in that specific run, it just wasn't going through the fade engine. The corrected picture doesn't rule out switching-edge parasitic coupling as PART of #2's mechanism (the x1-with-fade result is still consistent with that, just now also possibly explained by something more specific to the fade peripheral itself, e.g. its own internal ISR/step-controller behavior even for one step) - but it does mean #1 (bare ISR rate) is real, separate, and was never actually tested away in this thread, contrary to what the previous entry concluded.

**Practical read for the standing objective ("get back the x4 interp"):** hardware fade (v5) is now looking like a WORSE direction than the plain software ramp (v4) it was meant to replace, not a fix - it inherits the wake-rate-margin problem in some form (via the timer still needing to physically run at 64kHz for the fade steps to land on) AND adds its own inherent noise on top, confirmed present even divorced from rate. **`ENVELOPE_INTERP_USE_HW_FADE` reverted to `0` and `ENVELOPE_INTERP_FACTOR` reverted to `4`** (both back to their normal, `envelope_interp.h`-documented values - the "OK that works" v4/64kHz-carrier state from earlier this same day) - this whole hardware-fade branch of the investigation (v5) is not recommended to pursue further as currently understood. The implementation stays in the codebase, gated behind the now-off flag, in case the fade-engine-inherent-cost finding is ever worth chasing on its own terms (e.g. trying different `scale`/`cycle_num` parameterizations to see if some combination is cleaner - not yet tried), but it's not currently a promising path to x4 interpolation.

**CORRECTION, 2026-09-08, later still: the "reverted to normal" state logged above was itself wrong - `ENVELOPE_INTERP_FACTOR` was left at 4, not the actual clean resting value of 1.** User asked a sharp, basic question: "Presumably the PWM does load the cpu even if it is not written to?" Answering it properly required re-reading `on_timer_alarm()` again rather than trusting the earlier "reverted" summary, and that turned up a real mistake: in the non-`ENVELOPE_INTERP_USE_HW_FADE` build, the ISR's `vTaskNotifyGiveFromISR()` call is unconditional on every real timer alarm - there is no gate on the runtime `'I'` toggle at all. So leaving `ENVELOPE_INTERP_FACTOR` at 4 (as the previous correction entry did, mislabeling it "its normal permanent value") means the physical gptimer still fires and wakes `dsp_task` 64,000 times/sec regardless of whether `'I'` is on or off - exactly the `FACTOR=4`+`'I'`-off combination independently confirmed to burble on its own, both on 2026-09-07 (direct 1-vs-4 comparison) and again this session (the mislabeled hardware-fade test). The actual answer to the question this session has been chasing all along - "is 16kFs genuinely clean and workable at >30dB IMD" - only holds at `ENVELOPE_INTERP_FACTOR=1`, matching this project's own long-standing convention (the user's 2026-09-07 statement: "Whenever I reported that I was running with I-off now as default is when I also set ENVELOPE_INTERP_FACTOR=1"). `ENVELOPE_INTERP_FACTOR` has now been corrected to `1`. Separately: the PWM/LEDC peripheral ITSELF, once configured, is genuinely autonomous hardware - it costs zero ongoing CPU/ISR time to keep outputting an unchanged duty cycle. The 64kHz cost this whole investigation found is entirely attributable to the SEPARATE sample gptimer this project uses to pace `dsp_task`, not to the PWM output hardware itself - a distinction worth keeping straight now that a DAC is being evaluated as the next output stage, since that same sample-timer/wake-rate cost would apply identically regardless of which peripheral sits at the end of the chain, if the same "wake N times per real tick" architecture were ever reused.

**The delivered `pwm_envelope_interpolation_report.md` paper's conclusion section has been corrected to state `ENVELOPE_INTERP_FACTOR=1` explicitly** rather than the ambiguous "architectural default" phrasing it used before, which could have been (and, in the actual delivered code, WAS) read as compatible with leaving the constant at 4.

**Remaining live candidates for the standing objective, updated:** (1) accept `ENVELOPE_INTERP_FACTOR=1` (16kHz, x1, plain software write) as the practical ceiling for the PWM/RSET output path specifically - the DSP/timing side has now been shown fixable (v4/v5 both demonstrate a clean 16kHz wake rate is achievable), but the OUTPUT side (LEDC, whether driven by software ramp or hardware fade) itself doesn't tolerate being exercised faster than 16kHz cleanly on this hardware; (2) reconnect the MCP4725 DAC path already in the codebase - genuinely no switching-frequency energy to leak, sidesteps this entire class of problem rather than trying to out-clever it in firmware; (3) not yet tried: does the bare-64kHz-ISR-alone cost (mechanism #1 above) reproduce independently of any fade/write mechanism if deliberately isolated on purpose this time (rather than by accident) - e.g. a build with `ENVELOPE_INTERP_FACTOR=4` but `ENVELOPE_INTERP_USE_HW_FADE=0` AND `'I'` off (the ORIGINAL, oldest software-ramp-era config) - if that STILL burbles at full severity, it would independently confirm mechanism #1 without needing the HW_FADE flag's throttle logic at all, since that specific combination was actually already tested back on 2026-09-07 ("burble was there albeit at a lower level") - worth re-reading that entry's exact wording again given how today's mislabeling happened, to make sure THAT one was labeled correctly too.

## Repo sync check and `ssb_dsp.c`/`.h` CRLF false alarm, 2026-09-08

Full compare of the sandbox against `github.com/Rich5252/Arduino_ESP32_ssb_signal_generator`, branch `MainProject`, HEAD `0786ff6` (full non-shallow clone, `git log --pretty=format:"%h %ad %s" --date=short` for dated history). Requested by the user to confirm phase, with `settings.h` (Presets 1/2/3/5/6) flagged as authoritative on the repo side.

**Files pulled into the sandbox (repo was ahead on all of these):** `config.h`, `settings.h`, `AD9851.h`, `AD9851.c` - all now byte-identical to the repo. `AD9851.h`/`.c` in particular were stale by several weeks (last touched by the repo's `a90e6b6`, 2026-08-16, "Bit Bang SPI drive for AD9851 - big win now down to 25us" - a bit-bang SPI rewrite this sandbox never received, since this session's own work was entirely on the envelope/PWM side, not the phase/SPI path).

**`ssb_dsp.c`/`.h` - investigated further given the user's stated uncertainty ("I was expecting these to be your latest sandbox files") and an oddity found during the compare: `diff` reported a huge 1738/924-line difference (looking like a near-total rewrite) despite both files having IDENTICAL line counts in each copy (868/461). Manually inspecting the raw diff output showed genuinely different bytes from line 1 - different `#include` ordering, different comment text around the `biquad_t` typedef - which briefly looked like two independently-written, functionally-divergent implementations of the same feature (the `ssb_shelf_biquad_t` envelope-magnitude-equalizer machinery, both added the same day, 2026-09-03, by the repo's commit `1fa9ae9`).**

**Turned out to be a false alarm.** `file` showed the sandbox copies had CRLF line terminators, the repo copies plain LF. `diff --strip-trailing-cr` on both files returns **zero** differences - the content is byte-for-byte identical once line endings are normalized. Confirmed by hand-comparing the actual math (`ssb_shelf_biquad_set_highshelf()`'s coefficient formulas) side by side - identical. Also confirmed `envelope_ampeq.h`/`.cpp` (the caller of this machinery) were already byte-identical between sandbox and repo before this check even started, which is consistent with there never having been a real functional divergence. Pulled `ssb_dsp.c`/`.h` into the sandbox too (a line-ending normalization only, zero functional change) so all six files match the repo exactly.

**Shelf1/shelf2 date correction:** the user recalled the shelf1/shelf2 switch landing around the "last repo commit, Sep 3." The actual commit is `e41dfdc`, dated **2026-09-04** ("Added switch for shelf 1 or 2 (\"a\" env level comp)"). The real Sep 3 commits were `4420bc5` (MAX_FREQ_DEV_HZ experiment to 20k) and `1fa9ae9` (the ampeq/biquad addition discussed above - same commit that also touched `settings.h`/`config.h`, which is why those three files' histories converge on that date). The repo's actual most recent commit is `0786ff6` (2026-09-08, "Tweak Preset descriptions") - i.e. the repo has kept moving and is current through today, not stalled at Sep 3.

**Open discrepancy, not acted on:** the `_1`-suffixed duplicate files (`envelope_interp_1.h`, `group_delay_fit_notes_1.md`, `ssb_mic_test_commands_1.md`) are still present in a freshly-fetched clone of the repo (`git fetch` + `git pull` confirmed no new commits beyond `0786ff6`), despite the user's statement that the `_1` swap had been done. Most likely explanation: the swap was made locally (in the Arduino IDE working folder) but not yet committed/pushed to GitHub. Flagged for the user to check on their end rather than guessed at or worked around here.

## SDM (Sigma-Delta Modulation) driver, 2026-09-08

User reversed the earlier same-day SDM rejection ("Ah yes we spoke about the resolution issue before... My engineering gut says thats not going to work") with "if we dont try we will never fully know" - asked for a driver to actually try it on real hardware rather than continuing to reason about it from the datasheet alone. Re-confirmed the ESP-IDF `driver/sdm.h` API via WebFetch against Espressif's docs (v5.2 ESP32-S3 page) before writing anything, since nothing here can be compiled to check:

- `sdm_config_t` fields used: `clk_src` (`SDM_CLK_SRC_DEFAULT`), `gpio_num`, `sample_rate_hz` - assigned as a plain `int`/`uint32_t`-shaped struct (matching this same file's existing `ledc_channel_config_t.gpio_num = RSET_MOD_LEDC_GPIO` precedent, a plain #define int, not an enum cast) rather than risking a wrong guess at whether `invert_out`/`io_loop_back` are top-level fields or nested under a `.flags` sub-struct (the two web sources disagreed on this) - left everything else at its zero-initialized default via `sdm_config_t sdm_cfg = {}` instead of guessing, since the safe default (no inversion, no loopback) is what's wanted either way.
- `sdm_new_channel()` / `sdm_channel_enable()` / `sdm_channel_set_pulse_density(chan, int8_t density)` - confirmed `density` is signed 8-bit, -128..127, `Vout = VDD_IO/256 * density + VDD_IO/2`, and confirmed (this time, more precisely than the same-day earlier research) that `sdm_channel_set_pulse_density()` is explicitly documented as **"allowed to run within ISR context"** - ISR-safety was never actually the concern with SDM (unlike LEDC, which crashed outright in v1 for exactly this reason); the concern was always resolution/dynamic range.
- Espressif's own reference example uses `sample_rate_hz = 1,000,000` - kept as-is (`SDM_SAMPLE_RATE_HZ`), also a clean 80MHz/80 APB division.

**Design decision - where this hooks in:** rather than scattering a new call at every place `envelope_output_write_pwm()` is already called (which would tie its rate to whatever interpolation state 'I'/HW_FADE/curve happen to be in), `envelope_output_write_sdm()` is called from exactly ONE place - the top of `envelope_interp_on_full_tick()`, before ANY of that function's own branching. This means SDM always gets written once per real `SAMPLE_RATE_HZ` (16kHz) tick, with the same fully-processed (post relative-delay/offset/scale/predistort) envelope value PWM gets on an un-interpolated tick, completely decoupled from whatever the PWM leg's own interpolation experiments are doing. This is deliberate: the entire point of trying SDM (see `envelope_output.h`'s header comment) is that its own free-running ~1MHz internal comparator should relocate the switching image without needing any software interpolation architecture at all - if that's true, wiring it into the interpolation machinery at all would be pointless extra risk for zero benefit.

**GPIO choice:** GPIO1 - audited every pin already claimed elsewhere in this project first (RSET_MOD_LEDC_GPIO=2, AD9851 DATA/FQ_UD/W_CLK/RESET=10/11/12/9, TIMING_DEBUG_GPIO=4, TIMING_DEBUG_GPIO_ISR=5, TIMING_DEBUG_GPIO_ADC=39, MCP4725 SDA/SCL=47/48, mic ADC=GPIO6) plus the S3's strapping pins (0/3/45/46) and the two pins already flagged as ADC-adjacent trouble spots this project hit before (GPIO3's ADC1_CH2 noise coupling, GPIO13's mysterious 5kHz pulse - see `ssb_mic_test_commands.md`'s writeup on both). GPIO1 is clear of all of that.

**Resolution knob left wide open on purpose:** `SDM_DENSITY_CLAMP=127` (full signed-8-bit range) rather than Espressif's own recommended +/-90 "for better randomness." Given this project has already spent real effort on resolution/linearity elsewhere (predistort LUT, envelope-null floor), starting at the full range and only backing off to 90 if a stuck-pattern/spurious tone actually shows up on the spectrum analyzer seemed more honest than pre-emptively hobbling the one variable this whole experiment is supposed to be testing.

**Status: implementation complete, NOT YET BENCH-VERIFIED in any form** - no compiler available in this sandbox, so correctness here rests on careful manual re-reading against the fetched API docs, same standing limitation as every other driver-level change this session. `SDM_COMPARISON_ENABLED` defaults to 0. To test: wire GPIO1 to a filter (own RC, or a bench copy of the existing Sallen-Key network - do NOT tie onto the same node PWM/RC already drives), flip the flag to 1, reflash, confirm the boot log's "SDM channel OK on GPIO1, sample_rate_hz=1000000" line (or the FAILED variant, which would point at either a channel-allocation conflict - shouldn't happen, nothing else on this project uses SDM - or a config field mismatch against the actual installed driver/sdm.h version), then run the same two-tone IMD test this whole PWM/LEDC investigation used throughout, directly comparable against the `pwm_envelope_interpolation_report.md` results table.

**UPDATE, 2026-09-08, later same day: `SDM_OUT_GPIO` changed from its own dedicated pin (was a plain `#define SDM_OUT_GPIO 1`) to `#define SDM_OUT_GPIO RSET_MOD_LEDC_GPIO`** - user asked to reuse the PWM output pin rather than wire a second one, since this comparison is meant as either/or (one filter/scope point on the bench, swap which driver feeds it) rather than a simultaneous side-by-side like the PWM+DAC pair. Aliasing the #define rather than hardcoding a second literal `2` means the two pins can never silently drift apart if `RSET_MOD_LEDC_GPIO` itself is ever moved later.

This does introduce a real hazard that didn't exist with two separate pins: if `PWM_COMPARISON_ENABLED` and `SDM_COMPARISON_ENABLED` were ever both left at 1, `init_rset_mod_pwm()`'s `ledc_channel_config()` and `init_sdm()`'s `sdm_new_channel()` would both try to route the SAME GPIO through the GPIO matrix to two different peripherals - not a benign no-op, a genuine conflict over which peripheral actually owns the pin's output driver, with the specific outcome dependent on init order and not something worth relying on. Rather than leave this as a "remember not to" comment, added a build-time guard directly in `envelope_output.h`:

```c
#if PWM_COMPARISON_ENABLED && SDM_COMPARISON_ENABLED
#error "PWM_COMPARISON_ENABLED and SDM_COMPARISON_ENABLED both 1: they now share SDM_OUT_GPIO/RSET_MOD_LEDC_GPIO (same physical pin) and cannot both drive it at once. Set exactly one of these to 1 in config.h before building."
#endif
```

placed right after `SDM_OUT_GPIO`'s definition (both flags are already known at that point, both `#define`d in `config.h`, which `envelope_output.h` includes before this line) - a genuine compile-time failure rather than a runtime surprise if this is ever gotten wrong, consistent with this project's existing `settings.h` preset-count `static_assert` convention for the same "fail at build time, not on the bench" reasoning.

## SDM first hardware result, and the direct-from-ISR commit follow-up, 2026-09-08

**First real hardware A/B, single-write version:** user's own words - "remarkable... much better than expected." Two-tone IMD came back only about 1dB worse than the best 16kFs PWM/RC baseline this whole investigation has been measuring against, and the close-in noise profile (the specific +/-100Hz-ish region around each tone that showed the +20dB burble on every failed PWM interpolation attempt) was described as "much the same" as PWM's clean case - i.e. SDM's own free-running ~1MHz comparator really does appear to sidestep the wake-rate/write-mechanism costs the whole `pwm_envelope_interpolation_report.md` investigation spent the day characterizing on the LEDC side, on the very first real-hardware try, with a single plain write per tick and no interpolation architecture at all. Notable given SDM was rejected twice earlier the same day purely on a resolution/dynamic-range worry (8-bit signed density vs. LEDC's 10-bit duty) that turned out not to dominate the result in practice, at least not by much.

**Follow-up question, and the direct-ISR-commit change it prompted:** user asked whether there was "scope to write direct to SDM rather than ISR" - recalling that `envelope_output_write_sdm()`'s own doc comment had noted `sdm_channel_set_pulse_density()` is documented (re-confirmed via WebFetch against Espressif's v5.2 ESP32-S3 docs when the driver was first written) as safe to call from ISR context, a genuinely different situation from LEDC (whose lack of ISR-safety is why v1's direct-ISR PWM attempt crashed outright) and from v2's ISR crash (float/Hilbert math in ISR context, an FPU-coprocessor-context problem - not applicable here since committing a pre-computed int8_t involves no float math at all).

**The change:** split the single `envelope_output_write_sdm()` into two functions (`envelope_output.h`/`.cpp`):
- `envelope_output_stage_sdm(float envelope)` - unchanged conversion/clamp math, called from the same site (`envelope_interp_on_full_tick()`, before its 'I'/HW_FADE/curve branching), but now only stores the resulting `int8_t` density into a new file-static `volatile int8_t s_sdm_pending_density` rather than writing it to hardware immediately.
- `envelope_output_commit_sdm_from_isr(void)` - new, called from `on_timer_alarm()` (`ssb_mic_test.ino`) itself, true ISR context on Core 1. Does nothing but a null-handle check and the actual `sdm_channel_set_pulse_density()` call using whatever was last staged - deliberately kept to exactly that (no float math, no Serial, no other driver calls) so it stays inside the same ISR-safety boundary the HW_FADE-era `vTaskNotifyGiveFromISR()`-only ISR additions were already deliberately kept inside (see that flag's own "as close to v1/v2's ISR-safety boundary as this file gets, on purpose" comment in `ssb_mic_test.ino`).

Placed as early as possible in `on_timer_alarm()` - right after the existing `TIMING_DEBUG_GPIO_ISR` debug-pin edge, before the `vTaskNotifyGiveFromISR()` call - so its own execution sits as close to the true hardware alarm instant as this ISR can put it, which is the entire point: removing whatever latency exists between the real tick boundary and dsp_task actually getting scheduled to do the write.

**The unavoidable trade-off, and why it's not actually a new problem:** the ISR commits the density `dsp_task` finished staging on the PREVIOUS tick, not the one currently arriving - dsp_task's own full-tick DSP processing (ADC/Hilbert/atan2/etc.) hasn't run yet by the time this ISR fires for the CURRENT tick. That's a fixed, deterministic one-tick delay (62.5us at the current 16kHz `SAMPLE_RATE_HZ`) added to the SDM leg specifically, not jitter - and this project already has a purpose-built lever for exactly this kind of constant envelope-vs-phase timing offset (`'['`/`']'`, `relative_delay.h`), so it's a calibration item, not architecturally new.

**Why this is expected to be safe despite touching the highest-risk file in the project (the timer ISR):** `on_timer_alarm()` already established, via the HW_FADE-era isolation testing earlier the same day, that a plain integer counter plus `vTaskNotifyGiveFromISR()` is safely within ISR bounds on this hardware. This addition follows the identical pattern - one more ISR-documented-safe driver call, reading one more plain volatile byte, no float, no LEDC, no new call into anything not already proven safe in this exact ISR.

**Cross-core synchronization note:** `s_sdm_pending_density` uses the same "plain volatile, no lock" convention already trusted throughout this codebase for single-word cross-core/cross-task flags (`envelope_interp.cpp`'s `s_enabled`/`s_reseed_pending`/`s_curve`, this file's own `s_env_pwm_offset`/`s_env_pwm_scale`) - a single aligned byte can't tear, and the ISR only ever wants "whatever the latest staged value is," never a value that needs to be coordinated atomically with anything else read at the same instant, so nothing stronger is needed.

**Status: implementation complete, NOT YET BENCH-TESTED.** No compiler available in this sandbox - correctness rests on the same manual-re-reading standing limitation as every other driver-level change this session, though this one is a smaller, more contained change than the original SDM bring-up (two short functions, one new ISR call site, no new peripheral config). Next: reflash, confirm the boot log's SDM line still reads OK (channel bring-up itself is unchanged), then re-run the same two-tone IMD test and see whether the ~1dB gap versus PWM narrows, stays the same (pointing at SDM's resolution as the real limit rather than timing), or - worth checking for, given a systematic one-tick delay was just added to only one signal path relative to whatever reference the two-tone measurement compares against - whether anything about the comparison's own delay alignment needs re-checking first.

## Direct-from-ISR SDM commit: REVERTED, 2026-09-08

**Result, verbatim:** "Surprisingly that is worse. Adjusting the delay doesnt quite get back to benchmark and another dB or so is gone. The jitter is till there, maybe even worse." So the hypothesis this change was built to test - that dsp_task's own cross-core wake/scheduling latency was responsible for (some of) the ~1dB SDM was already down relative to PWM - came back the wrong way: moving the write into the ISR made the result WORSE, not better, and the close-in jitter this was specifically trying to remove is "still there, maybe even worse." First confirmed `SAMPLE_RATE_HZ` (16000), `ENVELOPE_INTERP_FACTOR` (1), and `ENVELOPE_INTERP_USE_HW_FADE` (0) were all still exactly as they'd been throughout the whole PWM investigation - the regression is real, not an artifact of some other setting having drifted.

**Two suspected mechanisms, worked through by re-reading the actual code and driver docs rather than guessing - not mutually exclusive, likely both contributing:**

**1. Possible flash-resident driver call inside the highest-priority ISR.** `envelope_output_commit_sdm_from_isr()`'s doc comment (written when this was added) already noted `sdm_channel_set_pulse_density()` is documented ISR-safe, and separately that its IRAM placement is conditional on `CONFIG_SDM_CTRL_FUNC_IN_IRAM` - re-confirmed via WebFetch against Espressif's ESP-IDF v5.0 docs: "controls where to place the SDM channel control functions (IRAM or Flash)... enabling this option allows sdm_channel_set_duty() to execute when the cache is disabled." What wasn't checked carefully enough the first time: whether that option is actually ON in the specific Arduino-ESP32 core installed for this project. It's a narrow, single-peripheral IRAM-budget option, off by default in most ESP-IDF configurations reviewed, and Arduino-ESP32 ships a fixed, non-user-editable sdkconfig baked into the precompiled core (no accessible menuconfig from the Arduino IDE) - so there's no way to confirm from this sandbox, or easily change it from the Arduino IDE, whether this call is IRAM-resident or flash-resident on this exact build.

If it's flash-resident: calling it from inside `on_timer_alarm()` means the project's single highest-priority, most timing-critical ISR now potentially takes a flash-cache-miss stall on every single alarm (16,000 times/second) whenever that code's cache line has been evicted by other Core 1 flash activity (Arduino's own `loop()`, Serial formatting, anything else sharing Core 1). This is not a new class of problem for this project - it's the EXACT mechanism `dsp_task`'s own `IRAM_ATTR` tagging was added to prevent (see `dsp_task`'s own header comment: "immune to cache-line stalls caused by Core 1 activity... touching flash... every module function called from here is itself IRAM_ATTR for the same reason"), except now happening a level up, inside the interrupt itself, which is worse in one specific way: a stall here delays not just the SDM write's own timing but ALSO the `vTaskNotifyGiveFromISR()` call that follows it, since it happens first in program order - so a stall here pushes back dsp_task's own wake time too, compounding rather than adding.

**2. The staging race under dsp_task's own real, already-documented scheduling variance.** `envelope_output_stage_sdm()` was called from deep inside `envelope_interp_on_full_tick()`, itself called at the point in `dsp_task`'s per-tick pipeline placed deliberately LAST among the register-facing work but AFTER the entire ADC/DC-block/audio_fx/Hilbert/atan2/gdeq/ampeq/predistort/relative-delay chain (see the .ino's own comment: "PWM write goes FIRST now, before the AD9851 SPI transfer" - i.e. this is the first hardware-facing action after ALL the DSP math, not before it). That's the point in the tick where dsp_task's own cumulative timing variance has had the most opportunity to build up. The design assumed dsp_task reliably finishes staging tick N's value comfortably before tick N+1's alarm fires, so the ISR always commits something exactly one tick old. But this project's OWN `dsp_task` code already documents, in detail, that this isn't a safe assumption on this hardware - the `elapsed_fast_ticks`/coalescing-event handling in `dsp_task`'s main loop exists specifically because a "cache stall, a moment of contention, anything" can occasionally make dsp_task arrive back at its wait point late enough that multiple real hardware periods have elapsed by the time it gets there. If dsp_task is ever that late finishing a given tick's processing, the ISR for the NEXT tick can end up committing a value that's stale by MORE than the intended one tick (whatever was staged two, or more, ticks ago) - an irregular, load-dependent hiccup rather than the clean constant offset the design assumed, i.e. genuinely NEW jitter, precisely matching "the jitter is still there, maybe even worse."

**Both point the same direction:** converting a plain, synchronous, always-fresh-but-occasionally-slightly-late write into a decoupled stage/commit pair traded a well-understood, bounded, already-characterized timing cost (dsp_task's own scheduling latency, present on the PWM leg too and never worse than "the write is a bit late this tick") for two NEW, less-bounded failure modes (an ISR that might occasionally stall on flash, and a commit that might occasionally read stale-by-more-than-one-tick data) - a worse trade on this specific hardware, empirically, even though the underlying "SDM is ISR-safe unlike LEDC" fact that motivated trying it is still true and not itself in question.

**Reverted.** `envelope_output_write_sdm()` is back to a single synchronous function, called directly from `dsp_task` (`envelope_interp.cpp`), exactly matching the version that produced the good first result (~1dB down vs. best PWM, close-in noise "much the same"). `envelope_output_stage_sdm()`/`envelope_output_commit_sdm_from_isr()` and the shared `s_sdm_pending_density` handoff variable are removed entirely, along with the `on_timer_alarm()` call site. Consistent with this project's standing practice throughout (v5 hardware fade, the `ENVELOPE_INTERP_FACTOR` correction): a plausible, well-reasoned hypothesis still loses to a real, negative hardware result - no further ISR-side optimization attempt is planned for SDM for now. The ~1dB gap vs. PWM, and whatever residual close-in jitter remains, should now be re-characterized against THIS (reverted, single-write) configuration as the actual current baseline, not against the ISR-commit variant that's no longer in the code.

**Confirmed, 2026-09-08:** re-flashed the reverted (single-write) version - "Yes as before," reproducing the original good result. The revert is clean; no new regression introduced by undoing the ISR-commit experiment.

## Generalizing the direct duty override to SDM's density, for a linearity characterization, 2026-09-08

User asked "should we run a duty linearity test?" with SDM now the active comparison path. This project already has a purpose-built tool for exactly this class of measurement: `'d'` direct duty override (`envelope_output_write_duty_raw()`/`envelope_output_get_max_duty()`, stepped via `'>'`/`'<'`/`'N'`/`'B'`, `'E'`-switchable between raw and envelope-mapped modes) - built specifically to characterize the RSET/filter/AD9851 chain against a KNOWN, exact commanded level, which is what produced every revision of `envelope_predistort.h`'s LUT and the `'E'`-cmd validation runs (0.134dB RMS residual confirming predistort genuinely linearizes envelope->RF-amplitude on real hardware). Rather than building a parallel SDM-specific tool, generalized this existing one.

**Why generalizing rather than building new was the obvious choice:** `SDM_OUT_GPIO` already aliases `RSET_MOD_LEDC_GPIO` (same physical pin, same downstream RC filter/BS170 gate/RSET node, mutually exclusive with PWM at compile time via the build-time `#error` from earlier today). So whichever comparison path is compiled in is unambiguous at any given time - there's no runtime state where both PWM and SDM might legitimately want control of the same override stepper simultaneously. That made it safe to have `envelope_output_write_duty_raw()`/`envelope_output_get_max_duty()` branch internally (`#if PWM_COMPARISON_ENABLED ... #elif SDM_COMPARISON_ENABLED ... #else ...`) on which peripheral to actually drive, rather than adding new functions or new serial commands.

**The mapping:** the override index stays `0..envelope_output_get_max_duty()` either way - under PWM that's still a literal LEDC duty count (unchanged behavior); under SDM, `envelope_output_get_max_duty()` now returns `2*SDM_DENSITY_CLAMP` (254) and `envelope_output_write_duty_raw(index)` maps it onto signed density via `density = index - SDM_DENSITY_CLAMP`, i.e. index 0 -> -127, index 254 -> +127 - algebraically identical to `envelope_output_write_sdm()`'s own envelope-to-density affine mapping (`density = -CLAMP + frac*2*CLAMP`, and `frac = index/max_index` here), so a swept index means the same physical output level regardless of which function path produced it.

**One correctness gap closed along the way:** `envelope_output_write_sdm()` never had the duty-override early-return `envelope_output_write_pwm()`/`start_hw_fade()` already had, because when it was written the whole concept of a "direct duty override" was PWM/LEDC-specific by construction. Now that `'d'` can genuinely command SDM's density directly, `envelope_output_write_sdm()` needed the same `if (s_duty_override_enabled) return;` guard, or `dsp_task`'s normal per-tick write would have kept fighting the override on every tick during a sweep - added.

**Practical payoff - the existing automated sweep harness needs zero changes.** The user already has a harness driving `'>'`/`'<'`/`'N'`/`'B'` for the PWM predistort characterization; since the override's wire protocol (same keys, same 0..max_duty index range, same up/down/jump semantics) is unchanged, that harness can drive an SDM sweep just by reflashing with `SDM_COMPARISON_ENABLED=1`/`PWM_COMPARISON_ENABLED=0` instead - only the serial reply's wording changed (says "density"/"index" instead of "duty" under SDM, purely cosmetic/logging, not protocol).

**The engineering question this answers, and why it's worth running:** SDM's own digital-to-voltage relationship is a simple documented linear formula (`Vout = VDD_IO/256 * density + VDD_IO/2` per driver/sdm.h). Everything downstream of that (RC filter, BS170 gate transfer curve, AD9851 RSET-to-output-amplitude relationship) is now IDENTICAL, unchanged hardware shared with PWM - the same physical node the existing predistort LUT was measured against. So this sweep directly tests a sharp, falsifiable question: does SDM's measured density->RF-amplitude curve match PWM's already-measured duty->RF-amplitude curve (predistort LUT), given both drive the identical downstream chain? A match would mean the nonlinearity is entirely a property of the shared analog stage, independent of modulation scheme - and the existing LUT could likely be reused for SDM outright, no new characterization work needed. A mismatch would mean SDM introduces its own nonlinearity on top (plausible candidates: near-rail density quantization behavior, the ±90-vs-±127 randomness recommendation being about noise not DC accuracy, or some other SDM-specific effect not yet considered). Either outcome moves the investigation forward. **Not yet run** - next step is the same workflow already used for `'E'`'s predistort validation (`'s'` for a steady carrier, sweep `'>'`/`'<'`/`'N'`/`'B'` or the automated harness, log RF amplitude per step, fit against the existing predistort curve).

## SDM density linearity sweep: results and analysis, 2026-09-08

User ran the sweep set up above and supplied two files: `Distortion raw sweep SDM.txt` (255 points, `'d'` on / `'E'`+`'D'` off, index 0-254 -> density -127..+127, raw dBm) and `Distortion Corrected sweep SDM.txt` (255 points, `'E'`+`'D'` on, env index 0-254 -> frac 0.0000-1.0000 -> the EXISTING PWM-calibrated predistort LUT's mapped index/density, resulting dBm). Both parsed programmatically (regex over the exact reply-line format, not eyeballed) and cross-checked against a same-chain PWM raw duty sweep already on hand (`distortion_raw_sweep.csv`, duty 1-1023, dBm - captured 2026-08-24, a different session but the same physical GPIO2/RC-filter/BS170/RSET node, since that's the pin SDM now shares with PWM). Chart delivered as `sdm_density_linearity_sweep.html` (raw-sweep shape overlay, corrected-sweep vs. ideal-line + residual, stats table) - not persisted as a durable artifact, same one-off-measurement-report convention as `predistort_e_validation.html`/`predistort_doff_vs_don.html` before it.

**Data-quality check first, since two `ReadLine() ERROR: Timeout waiting for reply` lines appear mid-file in the corrected sweep (around env index 96/97 and 159/160):** both are harness retry artifacts, not corrupted measurements. Line 98 (`ReadLine() ERROR:..., (frac=0.3819) -> index 117/254 -> density -10 (predistort ON), -47.683291 dBm`) is immediately followed by line 99, a clean, properly-formatted read at the SAME env index/frac/density (`env index 97/254 (frac=0.3819) -> index 117/254 -> density -10 (predistort ON), -47.769186 dBm`) - the two dBm readings differ by only 0.086dB, well inside the ~0.05-0.3dB step-to-step noise seen throughout this sweep (see the non-monotonic-step analysis below). Same pattern at line 162/163 (env index ~159/160, density 27, dBm differing by 0.041dB). Treated as duplicate/garbled-then-retried reads of the same step and discarded (kept the clean follow-on line), not as evidence of a dropped or wrong data point. `'E'`'s reply format continues to round-trip cleanly through the user's automated harness, consistent with the 2026-09-07 1024-point PWM sweep that first validated it.

**Raw sweep characterization (dead zone / turn-on / plateau), computed identically on both curves for a fair comparison** (floor = mean of first 10 points; "1% of full swing" = 1% of the floor-to-ceiling LINEAR amplitude span, i.e. `10^(dBm/20)`, matching the convention `envelope_predistort.h`'s REVISION 4-6 notes already use; plateau = last contiguous run within 0.3dB of the measured ceiling, same convention REVISION 5/6 used for their own "how much of the range is wasted near saturation" figures):

| | SDM (density, this sweep) | PWM (duty, `distortion_raw_sweep.csv`, 2026-08-24) |
|---|---|---|
| Floor (mean, first 10 pts) | -97.99dBm | -97.78dBm |
| Ceiling (max) | -39.69dBm (density +127) | -39.36dBm (duty 1023) |
| Total span | 58.37dB | 58.42dB |
| First >1dB above floor | density -110 (6.7% into range) | duty 173 (16.9% into range) |
| First >3dB above floor | density -103 (9.4% into range) | duty 225 (22.0% into range) |
| 1% of full linear swing reached | density -86 (16.1% into range) | duty 296 (28.9% into range) |
| Plateau (within 0.3dB of ceiling) | last 15.0% of range (from density +89) | last 10.8% of range (from duty 913) |
| Non-monotonic steps >0.05dB, in the real-signal region (excluding the flat floor) | 11 of 226 steps (4.9%) | 22 of 797 steps (2.8%) |

Floor and ceiling match to within 0.2-0.3dB, and total span matches to within 0.05dB - strong, clean confirmation that both curves are genuinely bottoming out on and saturating against the SAME physical RC-filter/BS170-gate/AD9851-RSET chain, exactly as expected from `SDM_OUT_GPIO` aliasing `RSET_MOD_LEDC_GPIO`. (Caveat: the PWM reference sweep is from a different session two weeks earlier, not a same-sitting A/B - the ~0.2-0.3dB floor/ceiling gap is plausibly just ordinary session-to-session drift, e.g. cable/connector reseating or attenuator retuning, not a real hardware difference; worth a same-session PWM re-sweep if a tighter comparison is ever needed.)

But the SHAPE is not identical. SDM's real signal emerges considerably earlier, proportionally, in its own command range than PWM's does in duty terms - roughly 16% into the density range vs. ~29% into the duty range by the "1% of linear swing" measure, and the gap is even larger by the coarser ">1dB above floor" measure (6.7% vs. 16.9%). At the top end the relationship reverses slightly: SDM's final approach to ceiling is a touch more compressed (last 15.0% of range within 0.3dB of max) than PWM's (last 10.8%). One plausible (unconfirmed - no scope capture done to check) physical explanation for the earlier SDM turn-on: SDM's pulse density is a genuinely random/dithered bitstream at any given average density (that's the whole point of its ~1MHz internal comparator), so even at a nominally low average density there will be occasional short bursts of consecutive 1-bits: the BS170 gate may catch and respond to those local bursts (which briefly look like a much higher instantaneous duty cycle) even when the long-run average is still deep in what would be PWM's dead zone, effectively pulling SDM's apparent turn-on point earlier than a smooth, non-dithered PWM ramp at the same nominal average duty. This is a hypothesis, not a confirmed mechanism - flagged for a possible future scope-across-the-RC-filter check, not acted on further here.

Non-monotonicity: SDM shows 11 small (0.06-0.32dB) backward steps scattered through its climb and plateau (the single largest, 0.32dB, is the density 81->82 blip visible in the raw sweep - this is the "one small non-monotonic blip" flagged when the raw file was first read). PWM's own raw sweep shows a comparable RATE of similar-sized backward steps through its own real-signal climb region (22 of 797 steps, vs. SDM's 11 of 226 - both in the 3-5% range), so this reads as ordinary spectrum-analyzer read noise at this bench setup rather than a defect specific to either modulation scheme. (PWM's dead-zone region shows many MORE non-monotonic steps in absolute count, but that's expected and not comparable - there's no real signal there for either device, so any two floor-level readings can appear to go "backward" by pure noise; restricting the count to each curve's own real-signal region, as done here, is what makes the comparison fair.)

**Corrected sweep (existing PWM predistort LUT, unmodified, now driving SDM's density): fit against the same ideal proportional-amplitude target used for PWM's own `'E'`-validation** (`dBm = A + 20*log10(frac)`, least-squares fit for `A`, same frac windows: 0.10-1.0 and 0.02-1.0):

| | SDM through PWM's LUT (this sweep) | PWM's own `'E'`-validation (2026-09-07, for reference) |
|---|---|---|
| RMS residual, frac 0.10-1.0 | 0.209dB | 0.134dB |
| RMS residual, frac 0.02-1.0 | 0.256dB | 0.144dB |
| Max \|residual\|, frac 0.02-1.0 | 1.139dB | 0.545dB |
| Usable range covered | -72.1 -> -39.7dBm | -75.9 -> -40.1dBm |

So: it works, in the sense that there's no gross mistracking or runaway error anywhere in the sweep - the unmodified PWM LUT gets SDM's output into roughly the right proportional-amplitude shape across its whole usable range. But it's measurably worse than PWM achieves with its own native calibration: ~1.5-1.8x the RMS residual, ~2x the max residual. The worst individual residuals (frac 0.0276: -1.14dB; 0.0709: -0.98dB; 0.0669: -0.84dB; 0.0394: -0.81dB; 0.0551/0.0512: -0.79/-0.74dB; a couple of positive outliers around frac 0.14 and 0.33 too) cluster tightly in the steep turn-on region, frac roughly 0.03-0.14 - exactly the region the raw-sweep shape comparison above flags as where SDM and PWM diverge most (PWM's LUT, calibrated to expect PWM's later/slower turn-on, ends up commanding density values that sit in a different part of SDM's own, earlier-turning-on curve for the same envelope fraction).

**Answer to the falsifiable question this test was designed around:** partial match, not a clean yes or no. Same floor, same ceiling, same overall S-curve family (flat dead zone -> steep turn-on -> compressing climb -> plateau) - confirming the OVERALL dynamic range and general shape really are properties of the shared downstream analog chain, independent of modulation scheme, as hypothesized. But the exact proportional mapping differs enough (turn-on point, degree of top-end compression) that reusing PWM's LUT unchanged leaves real, measurable linearity headroom on the table, concentrated right where the two curves' shapes diverge most.

**Recommendation:** keep the existing PWM LUT driving SDM for now - it's a working stopgap, not a broken one, and a fresh two-tone/IMD check with predistort ON hasn't been run yet (worth doing before investing in a new table, in case the existing LUT's residual is already small enough not to matter for IMD purposes even though it doesn't hit PWM's own best-case linearity number). If a dedicated SDM table is wanted later, the path is well-trodden: reuse `envelope_predistort.h`'s own REVISION 4-6 methodology exactly (exhaustive `'d'`-driven density=-127..127 direct sweep - already have it, this file's raw sweep - dBm -> linear amplitude via `10^(dBm/20)`, isotonic-regression/PAVA smoothing, normalize to [0,1], PCHIP inversion onto an even envelope grid), just built from the SDM density axis instead of the PWM duty axis. Not yet acted on.
