# Moving forward: CPU/timing roadmap and open threads

Running log for forward-looking architecture/timing questions that aren't
tied to a specific bug fix - started 2026-09-09 off the back of the README
work, when three "what's worth tackling next" questions came up: ESP-IDF
toolchain levers for CPU/core control, whether the noise floor could be
reference-clock related, and whether the AD9851 write time is worth
attacking directly. Keep adding dated entries here as this thread develops,
same convention as `group_delay_fit_notes.md`/`ssb_mic_test_commands.md`.

## 2026-09-09: three "what's next" questions, real profiling data reused, one new lead

**Q1: ESP-IDF toolchain features for CPU/core control.** Checked the
project's actual Arduino IDE board settings (screenshot) rather than assume
defaults. Findings:

- `CPU Frequency: 240MHz (WiFi)` - pinned, not "Auto", so dynamic frequency
  scaling is not in play and `esp_pm_configure()` frequency-locking isn't
  needed here; the concern is already structurally satisfied.
- `PSRAM: Disabled` - rules out the PSRAM cache-coherency class of issue
  (`esp_cache_msync`) entirely.
- `Flash Mode: QIO 80MHz` - no faster mode available on this module (no OPI
  wiring on the Super Mini boards), so nothing actionable, but it's mild
  supporting evidence for the existing cache-stall theory: a flash
  cache-line fetch at 80MHz QIO is meaningfully slower than internal SRAM,
  so any non-IRAM-resident code on the hot path pays a real penalty of
  about the right order of magnitude.
- `Arduino Runs On: Core 1` / `Events Run On: Core 1` - confirms Core 0 is
  actually left exclusively to `dsp_task` as designed; not a new lead, just
  verification.
- `USB Mode: Hardware CDC and JTAG`, `USB CDC On Boot: Enabled` - **new
  candidate**, not yet ruled out by anything checked so far. The native
  USB-Serial-JTAG peripheral is live and doing its own housekeeping
  (enumeration, SOF handling) continuously on Core 1, a source distinct
  from the ADC/timer/I2C activity already investigated in the cache-stall
  work. Worth an A/B against `USB Mode: TinyUSB (USB-OTG)` if the leading
  hypotheses below don't pan out.

Other levers discussed but not yet needed/actionable: `vTaskCoreAffinitySet()`
(FreeRTOS-SMP) as a finer-grained version of the `xTaskCreatePinnedToCore()`
already in use; `esp_intr_alloc(..., ESP_INTR_FLAG_IRAM, ...)` for any new
ISR added to the timing-critical path; ESP32-S3 cache-line-size Kconfig
options exist in principle but Arduino IDE's board support doesn't expose
sdkconfig editing, so that lever needs a build-system migration to reach -
not justified by the problem size yet.

**Q2: could the noise be reference-clock related (external xtal, dividing
down the AD9851's 30MHz to sync with the ESP32)?** Real-hardware two-tone
measurement (user's own bench report): AD9851 output is "about 10Hz wide at
-40dB and nearly 100Hz wide at -60dB", with small discrete sidebands at
roughly +/-2, 4, 6Hz, but mostly wideband noise/jitter. Two separate
conclusions from this one measurement:

- The wideband noise/jitter component matches, by signature, the
  already-diagnosed cross-core cache-stall/scheduling-jitter mechanism
  (the same class of bug behind the `dac_task_enabled` regression and the
  gptimer/ADC-ISR priority collision fix). It does not look like reference-
  oscillator phase noise. **Conclusion: the AD9851's own 30MHz reference
  is clean enough that a shared/external/synced clock is not indicated -
  shelve that idea** unless a future spectrum-analyzer session finds
  close-in skirts that scale with the 6x multiplier specifically.
- The discrete +/-2/4/6Hz sidebands are a distinct finding. Lines that
  closely spaced to the carrier are essentially never a reference-clock
  effect - they're the signature of something in the firmware perturbing
  the envelope/phase on roughly a 1-2 second cadence (2/4/6Hz reading as
  harmonics of a ~1Hz fundamental, or of a non-sinusoidal disturbance's own
  repetition rate). The one existing, already-instrumented candidate that
  fits: `diagnostics_service()`'s long-window print fires every 1000ms
  (`diagnostics.cpp:667`, `now - last_timing_print_ms >= 1000`). **This is
  exactly the test the project's own 2026-09-07 note already proposed and
  apparently never followed up on** ("Recommended next step: re-run muted
  ('v') to see if it's a smaller residual of the already-fixed Core-1/
  Serial stall mechanism" - `ssb_mic_test_commands.md:300`). Re-running the
  two-tone measurement with diagnostics muted (`'v'`) is a single,
  no-rebuild experiment that tests both the wideband-jitter hypothesis and
  the discrete-sideband hypothesis at once. **Not yet run as of this
  entry.**

**Q3: is the AD9851 write worth attacking directly, and where's the real
CPU budget going?** User confirmed: yes, but the ~32us unexplained stall
(already found and left unresolved in the 2026-09-07 entry) is the obvious
thing to tackle first, not the AD9851 write itself. Restating why, since
it reframes the original question: the `write_us=33` figure isn't all
AD9851 write - the profiled breakdown was `prep_us=2, spi_us=17` (19us
actually inside `ad9851_set_frequency()`), with the remaining 14us being
ampeq/predistort/relative_delay/LEDC-write/diagnostics bookkeeping that
happens to share that timestamp bucket. The true bit-bang cost is 17us for
40 bits, which is CPU-loop-bound (three `GPIO_OUT_W1TS/W1TC` register
writes per bit, no clock divider to "double" since this is bit-banged, not
hardware SPI) - shrinkable via fewer register writes per bit, or via moving
the whole transfer to the RMT peripheral (viable but nontrivial: RMT drives
one GPIO per channel, so DATA/W_CLK/FQ_UD would need three synchronized RMT
channels via the sync-manager feature, plus a per-call byte encoder for the
DATA channel since that data changes on every call - up to 16000x/sec).
That's real engineering effort for maybe 5-10us of headroom.

The 32us stall, by contrast, is confirmed to occur even with zero AD9851/
DSP/ADC activity in that code path (`max_busy_us=64` against a 62.5us
budget at 16kHz - the tick already overran in that capture), and its cause
was explicitly left open ("points at an external interrupt/scheduling
event"). Agreed target. Audit plan, in order:

1. **Mute-diagnostics test** (see Q2 above) - cheapest, already proposed,
   tests two symptoms at once. Do this first.
2. **Scope-bracket the exact window**, same technique that found the
   gptimer/ADC-ISR priority collision previously: the 32us currently lands
   inside the timestamp bucket labeled "adc" even in two-tone mode where no
   ADC code runs there, so a `TIMING_DEBUG_GPIO`-style toggle at that
   segment's real start/end will show whether it's one contiguous slow
   stretch or a gap (gap = preemption by something else; contiguous slow
   = something in that segment itself).
3. **`esp_intr_dump()`** - one call, full inventory of every registered
   interrupt's core/priority. Cheap sanity check before guessing further.
4. **USB Mode A/B** (see Q1) - `Hardware CDC and JTAG` vs `TinyUSB
   (USB-OTG)`, if 1-3 don't land it.

## 2026-09-09, later same day: USB Mode A/B attempted (item 4) - inconclusive, and a data-reading caveat worth flagging

Tried the `USB Mode: TinyUSB (USB-OTG)` A/B suggested above (item 4 of the
32us audit plan). Two results:

- **Practical problem, likely just a mode quirk, not a finding:** the board
  wasn't recognized as a COM port under this mode. This matches a known
  ESP32-S3 Arduino behavior difference - `Hardware CDC and JTAG` exposes a
  bootloader-level virtual COM port immediately (even before the sketch
  runs), while `TinyUSB (USB-OTG)` only enumerates once the firmware itself
  reaches the point of initializing the USB stack, and some boards need a
  manual boot-mode button sequence to be seen reliably during upload. Given
  this, and that `Hardware CDC and JTAG` is the known-good, stable mode for
  day-to-day work, **recommend reverting to `Hardware CDC and JTAG` for now**
  rather than fighting the enumeration quirk - this A/B isn't worth the
  daily friction unless items 1-3 come up empty.
- **Data captured despite the port issue:** the monitor did show
  `[core1]   idle hook calls/s=19712-19715 (cross-check)` once per second.
  Important caveat, from the code's own comments (`diagnostics.cpp:138-148`,
  `CORE1_IDLE_GAP_THRESHOLD_US=10`): this specific line is explicitly
  documented as an unreliable cross-check that **reads low** - it only
  counts an idle-hook call as "idle" when consecutive calls are ≤10us
  apart, so a real blocked wait (e.g. `loop()`'s `delay(10)`, which spaces
  idle-hook calls by the OS tick period, not microseconds) is mostly
  invisible to it. The code comment explicitly points at "`[core1]` busy
  breakdown's delay% below" as the trustworthy figure instead - which
  wasn't captured this time. **This single line, on its own, can't be used
  to conclude anything about whether TinyUSB helped, hurt, or made no
  difference to Core-1 headroom or the 32us stall.** To actually use this
  experiment, next time capture the full `[core1] busy breakdown` line
  (cmd/adc_svc/diag/delay%) and the `[timing]` busy/overrun/max_busy_us
  line from the same window, under both USB modes, for a real comparison.
- "RF default output was running and was no different to normal" - a
  qualitative/by-ear check, not a scoped measurement of the 32us stall or
  the +/-2/4/6Hz sidebands specifically. Treat as a weak, not rigorous,
  negative data point against the USB-Serial-JTAG-housekeeping hypothesis -
  worth remembering, not worth ruling the hypothesis out on.

**Net effect: item 4 stays open, but isn't the efficient next step.** Given
the enumeration hassle and the inconclusive readout, the mute-diagnostics
test (item 1, `'v'`, no rebuild, no USB-mode juggling, tests two hypotheses
at once) is still the right next experiment to actually run.

## 2026-09-09, later still: mute-test run, and a strong, clean hit - continuous Serial diagnostic output is a major noise source, 20Hz-spaced sidebands

**Correction (see next entry): the earlier +/-2/4/6Hz close-in sideband
measurement was actually taken with diagnostics MUTED ("v off"), not
streaming as first assumed here from the boot-default value.** That
measurement and the 20Hz broadband finding below are two independent
phenomena, not the same one at two intensities - see the correction entry
for the full picture. Keeping the reasoning below as originally written
since it's still the right explanation for the 20Hz broadband effect
itself, just not for the close-in lines.

This session's actual mute/unmute A/B (toggling `'v'` live) gives a much
cleaner and stronger result than the earlier close-in measurement did:

**With continuous Serial diagnostic output running, RF noise gets
noticeably worse - dominant sidetones appear at ~20Hz spacing, high enough
to swamp the underlying carrier noise.** This is a clear, reproducible,
user-confirmed A/B, not a hypothesis.

**Best-fit cause, checked against the actual code:** `diagnostics_service()`
has exactly two periodic Serial outputs, both gated by `s_diag_muted`
(`diagnostics.cpp:661` and `:667`):

- a fast status line every 45ms (`now - last_print_ms >= 45`) - 1/0.045 =
  **22.2Hz**, a close match to the observed ~20Hz spacing given normal
  by-eye/by-ear spacing precision;
- a slow "long-window" line every 1000ms - 1Hz, a poor match for a 20Hz
  spacing and not the likely cause here.

No other ~50ms-period activity exists in the codebase (checked `diagnostics.cpp`,
`ssb_mic_test.ino`, `serial_commands.cpp`, `adc_capture.cpp` for anything else
near that cadence - only the 45ms/1000ms diagnostic lines and `loop()`'s own
`delay(10)`, which runs faster still). **The 45ms fast diagnostic line is
the leading suspect for the 20Hz broadband sidetones** - see the correction
entry below for why this is now understood as a SEPARATE mechanism from the
close-in +/-2/4/6Hz lines, not a bigger version of the same thing.

This also plausibly ties into the still-open 32us stall investigation:
`diag_room_for()` exists specifically because unguarded `Serial.printf()`
calls can block on a full USB CDC TX buffer (the same class of bug behind
the earlier-fixed multi-ms Core-1 stall) - a 45ms-cadence burst of several
printf calls is exactly the kind of Core-1 Serial activity already suspected
of bleeding into Core-0 timing via cross-core interference, the same
mechanism family as the `dac_task_enabled`/I2C regression and the ADC-ISR
priority collision, both already confirmed causes elsewhere in this project.

**Recommended next steps, in order:**

1. **Adopt muted-by-default as standard practice for any noise-sensitive
   RF measurement** (two-tone, IMD, noise floor) - this alone looks like it
   removes a dominant noise source, not a marginal one.
2. **Slow the 45ms fast line down** (e.g. to 200-500ms) or gate it out of
   any noise-critical test mode entirely, then re-measure with diagnostics
   *unmuted but slowed* to confirm the 45ms cadence specifically (not
   Serial activity in general) is what's producing the 20Hz spacing - this
   isolates "printing at all" from "printing this often."
3. Re-run the 32us-stall scope-bracketing test (audit item 2, above) with
   diagnostics unmuted vs. muted, now that there's a concrete reason to
   suspect the 45ms line specifically rather than Serial output in general.

## 2026-09-09, later still: correction + new nuances - two INDEPENDENT noise mechanisms, not one, plus a fade-then-reappear behavior on the 20Hz effect

User clarified the mute state of the earlier close-in measurement directly,
which corrects the previous entry's assumption: **the +/-2/4/6Hz close-in
sideband measurement was taken with diagnostics MUTED ("v off").** So that
finding and the 20Hz broadband finding above are NOT the same mechanism at
different intensities - they're two separate things:

- **Close-in +/-2/4/6Hz lines**: present with diagnostics fully muted.
  Cannot be caused by the 45ms/1000ms Serial print lines (both gated off
  entirely by `s_diag_muted`) or anything else gated the same way. Still
  unexplained - the diagnostics-print hypothesis from the earlier entries
  does NOT apply here and should not be treated as having accounted for
  this. Needs its own investigation (see "Next" below).
- **~20Hz broadband bursts**: confirmed to require diagnostics UNMUTED
  ("if I stop output and use 'V' those outputs cause the higher noise") -
  the 45ms-print-cadence explanation from the previous entry stands for
  this one specifically.
- **The two affect different parts of the spectrum**: user reports the
  20Hz-spacing effect shows up in the broadband noise, while "the very
  narrow band noise around the carrier" (i.e. the close-in +/-2/4/6Hz
  region) doesn't change much when toggling diagnostics on/off. Consistent
  with them being genuinely independent mechanisms rather than one thing
  measured two ways.
- **Measurement caveat, noted by the user and worth keeping attached to
  this data going forward:** the narrowband (close-in) display is a
  0.18Hz-resolution FFT with averaging, "quite slow to respond" - so
  "doesn't change much" there is measured through a slow-responding,
  heavily-averaged instrument. That's good for confirming the close-in
  lines are a persistent/steady-state feature (not a fluke), but it also
  means a real small transient change in that region could be smoothed
  away by the averaging and simply not show up in this display. Don't
  over-read "no change" as "definitely zero effect" until checked with a
  faster-responding capture if it ever matters enough to chase.

**New, so-far-unexplained wrinkle on the 20Hz broadband effect itself:**
left running for long periods with diagnostics unmuted, the bursts "almost
disappear" - but toggling `'v'` off and back on makes them reappear. This
doesn't fit a simple "the 45ms print always costs a fixed amount and always
produces a fixed spur" model; it points at some state that drifts toward a
quieter equilibrium over time (a USB host-side buffer/backlog settling, a
queue depth stabilizing, `diag_room_for()`'s available-headroom check
behaving differently once the host's receive side has caught up, or
something similar) and gets reset back to a "fresh/bursty" condition by the
mute/unmute toggle itself. User noted this resembles "a few other things
I've seen" in this project - i.e. this may be another instance of a
recurring pattern here (something that changes character over a long run)
rather than a one-off. Not investigated further yet - flagging so it isn't
lost, since it complicates any future "confirm the 45ms line causes this"
experiment (a quick toggle-and-look test will see the effect; a long
steady-state run might not, and that's expected now, not a contradiction).

**Next, for the still-unexplained close-in +/-2/4/6Hz lines:** since these
persist muted, the diagnostics print theory is ruled out for them
specifically. Worth testing next with a plain, unmodulated, idle carrier
(no test signal, no envelope/PWM activity at all) under the same muted
conditions: if the +/-2/4/6Hz lines persist even then, that points outside
this project's own DSP/envelope chain entirely (measurement-receiver LO
drift, mains-related hum products, thermal/power-supply drift on the bench)
rather than at firmware; if they disappear, that points at something
running continuously regardless of mute state within the DSP/envelope path
itself (e.g. the Hilbert FIR, `envelope_floor`/`gdeq`/`ampeq`/`predistort`
processing, or - checked and considered unlikely by design, since the
ADC/DSP/PWM clock relationships were deliberately kept at exact integer
ratios - a beat between two of the project's own internal clock domains).
Not yet run.

## 2026-09-09, later still: cleanly isolated to the AM path - PM is spotless, AM carries a genuine ~1Hz-spaced disturbance, unrelated to Serial

Two controlled isolation-test measurements, much cleaner than the earlier
general two-tone captures:

- **Pure sine/CW tone (PM path only)**: very clean, >-70dB at +/-2Hz, and
  that floor is admittedly window-limited (Hann window sidelobes, not a
  real spur) - so the PM/frequency path is, as far as this measurement can
  tell, essentially spotless. Confirmed not affected by Serial.
- **`'h'` (`AUDIO_SRC_AMTEST`, pure sinusoidal AM isolation test,
  `AM_TEST_MOD_HZ=1200Hz`, `config.h:440`)**: carrier clean, the wanted AM
  sidebands (at fc+/-1200Hz) are good, but there's a real comb of
  **~1Hz-spaced satellite lines around the AM sideband complex** starting
  at about -40dB relative to the AM sideband (-55dB relative to carrier).
  **Confirmed not affected by Serial activity either** - a different,
  independent confirmation from the earlier muted-close-in measurement,
  now specifically isolated to the AM path.

**Why this is a strong result:** `test_signals.h` documents that
ENVSTEP/FMTEST/AMTEST all bypass `ssb_dsp_process_sample()` entirely -
`'h'` writes the envelope directly, so this signal never touches the
Hilbert FIR, `atan2`/`sqrt`, or the DC-blocking filter at all. A ~1Hz
disturbance appearing on this signal must therefore come from somewhere
downstream of raw envelope generation: `envelope_floor` (off by default -
floor starts at 0.0), `envelope_gdeq`, `envelope_ampeq`,
`envelope_predistort`, the PWM/LEDC write itself, or the analog RC/BS170/
RSET output stage - or it's genuinely external to the firmware (bench
power-supply ripple/slow control-loop "breathing", thermal drift, a
mains-related beat). Checked `envelope_gdeq.cpp`/`envelope_ampeq.cpp`/
`envelope_predistort.cpp`/`relative_delay.cpp` for any internal
timer/counter/periodic-recompute logic - found none; they're all pure
per-sample static functions with no notion of wall-clock time, so nothing
obviously self-explains a ~1Hz component from the DSP correction chain
itself. That pushes the likely explanation toward either the PWM-output/
hardware side, or something external to the board entirely - a real
possibility for RF work, since a slow ~1Hz "breathing" ripple on a supply
rail feeding the RSET/PWM/RC stage is a classic symptom of a switching
regulator's control loop or a marginal linear regulator, and wouldn't show
up in any firmware code review at all.

Also worth noting: since `AM_TEST_MOD_HZ=1200Hz` and these satellite lines
sit right around the 1200Hz AM sideband complex (not around the carrier
itself), what's being seen is a slow disturbance modulating the wanted
1200Hz AM tone - "modulation of a modulation" - which fits equally well
with either a firmware-side envelope disturbance or a power-rail ripple
affecting the RSET reference while the PWM duty is actively swinging.

**Recommended next steps, in order:**

1. **Static/unmodulated envelope test**: command a fixed, unchanging duty
   (no `'h'` sweep at all - e.g. a direct duty override) and check whether
   the ~1Hz lines are still present. If yes, that strongly implicates
   something external and continuous (supply ripple, thermal drift) rather
   than anything reacting to a changing envelope. If they disappear, that
   points at something that only manifests when the envelope is actively
   moving - narrowing back toward the correction chain or the PWM update
   mechanism itself.
2. **Toggle `'D'`/`'g'`/`'a'`/`'A'` one at a time during the `'h'` test** -
   a cheap way to check whether predistort, gdeq, or either ampeq shelf is
   involved, without new instrumentation.
3. **Scope the supply rail feeding the RSET/PWM/RC filter stage directly**,
   independent of firmware - if a ~1Hz ripple or slow oscillation shows up
   there, this is a power-supply/analog finding, not a code one.

Not yet run.

## 2026-09-09, later still: "no sidebands on the carrier" reframes the leading hypothesis toward real sample-timing jitter, not supply ripple; scope "jitter" is likely a separate, benign artifact

Two more pieces of information, and they point in a genuinely useful
direction - one revises the ranking of last entry's three next-step
hypotheses, the other answers a separate question about what the scope is
showing.

**"The sidebands do not occur on the carrier" is the important new
detail.** Worked through what this implies physically: if the ~1Hz
disturbance were a gain/amplitude-domain effect - supply ripple or thermal
drift multiplying the whole envelope, including its steady/DC part - it
would produce matching sidebands around the carrier too, since a
multiplicative ripple scales everything the envelope touches equally. It
does not do that here. What DOES naturally produce sidebands proportional
to a signal's own rate of change, while leaving a constant/DC term
essentially untouched, is jitter in *when* each envelope sample is
actually written out (a first-order Taylor argument: a timing error
delta(t) perturbs the reconstructed signal by approximately
delta(t) * (rate of change of the signal at that instant) - zero for a
flat/DC term, non-zero and growing with amplitude/frequency for a moving
AC term like the 1200Hz AM tone). **This matches the observation exactly**
and reframes the leading hypothesis: this looks like real per-sample
envelope-write TIMING jitter, not external amplitude/gain-domain ripple -
i.e. plausibly the SAME broad jitter/scheduling story already being chased
elsewhere in this thread (the gptimer/ADC-ISR history, the still-open 32us
stall), rather than a new, separate power-supply finding.

This sharpens the static-envelope test from the previous entry into an
actual falsification test, not just a fishing expedition: **if this really
is sample-timing jitter, a perfectly static/unmodulated envelope should
show essentially none of the ~1Hz artifact, even if the underlying tick
jitter is fully present** - because jitter can only smear content where
the signal has slope, and a static envelope has none anywhere. If the
static test still shows the ~1Hz lines, that would rule out simple
write-timing jitter and point back to a genuine amplitude/gain-domain
mechanism (supply ripple etc.) after all. **Revised priority order: run
the static-envelope test first** (previous entry's item 1); the supply-rail
scope check (previous entry's item 3) drops to "only if the static test
comes back clean" rather than being pursued in parallel.

Also worth reconciling why the PM-only sine test (previous entry) came back
spotless if the same shared per-sample tick could be jittery: a
frequency-word write landing slightly early/late doesn't produce a
proportional phase error the way a mistimed voltage/duty value does - the
AD9851 free-runs its own DDS core between updates, so the "sensitivity" of
the two paths to the same absolute timing error is genuinely very
different by the physics involved, not necessarily evidence that the PM
path's timing is jitter-free while the AM path's isn't.

**Separate question, separate (mostly benign) answer: the timing wobble
seen directly on the scope, on both the AM test's modulating sine and the
two-tone envelope.** This is very likely explained by the modulation
frequency not being an exact integer submultiple of `SAMPLE_RATE_HZ`
(16000Hz), not by anything in the firmware. The zero-order-hold PWM/RC
reconstruction produces a staircase that repeats bit-for-bit, at the same
phase, every single cycle ONLY when `Fs/F` is an exact integer - a scope
free-running relative to the ESP32's clock, triggered on a level/slope of
that same asynchronously-sampled waveform, will otherwise see the
staircase's phase walk cycle-to-cycle purely as a consequence of that
non-integer ratio, producing exactly the kind of apparent time-domain
"jitter" described here - completely independent of whether the underlying
sample clock has any real timing error at all. **The 1600Hz two-tone
difference-frequency experiment (`Fs/F = 16000/1600 = 10`, an exact
integer) coming back visibly more stable is a clean, correct, textbook
confirmation of exactly this mechanism** - a strong piece of evidence this
particular scope observation is a benign, well-understood viewing artifact,
not a project bug.

**Important: this scope-domain explanation does NOT explain away the
spectral ~1Hz sidebands from the `'h'` AM test.** A spectrum-analyzer/FFT
measurement doesn't care about scope trigger phase at all, so that finding
stands on its own and is the one worth chasing with the static-envelope
test above. Two genuinely separate questions, two different (and both
now reasonably well-understood) answers.

## 2026-09-09, later still: `'D'` toggle - no change; ruled out test-tone-generation accuracy; PWM quantization flagged as a plausible amplitude contributor (not a spacing explanation)

**`'D'` (predistort) toggled off during the `'h'` AM test: no change to the
~1Hz sidebands.** One correction-chain suspect cleared; `'g'`/`'a'`/`'A'`
not yet tried the same way.

**User asked whether this could be a test-tone-generation accuracy issue
(integer math?).** Checked the actual code
(`test_signals.cpp:211-216`, `test_signals_generate_amtest()`): not integer
- it's a plain running phase accumulator in float32 (`s_amtest_phase +=
two_pi * AM_TEST_MOD_HZ / (float)SAMPLE_RATE_HZ`, `sinf()` each sample,
wrap-by-subtraction past `two_pi`). **Ruled out as the cause**: float32
rounding in this accumulator (both the fixed bias in the per-sample
increment, since 1200/16000 isn't exactly representable in binary, and
running-sum round-off) is on the order of 1e-7 relative. The observed
sidebands are ~1% relative to the AM sideband (-40dB) / ~0.18% relative to
carrier (-55dB) - five to six orders of magnitude larger than tone-
generation float rounding could produce. Not the source.

**Adjacent detail worth keeping attached, not a rival explanation:**
`RSET_MOD_LEDC_RES` is 10-bit, so PWM duty quantizes in steps of about
1/1023 (~0.1% of full scale) - actually in the right order of magnitude for
the observed amplitude, unlike the float math. But a static quantization
step on its own produces distortion at harmonics of the modulating tone
(2400Hz, 3600Hz... for a 1200Hz AM tone), not a comb spaced at ~1Hz -
so quantization is a plausible source of there being visible products at
all, while the ~1Hz spacing still needs its own slow-periodic explanation
(the timing-jitter hypothesis from the previous entry) to be doing the
modulating/dithering. Consistent with, not competing against, that
hypothesis.

**Static-envelope test (previous entry's top next step) remains the
highest-value thing to actually run - still not yet done.**

## 2026-09-09, later still: RESOLVED - the ~1Hz sidebands were the measurement receiver's own RX IF AGC, not this project at all; a new, separate, well-understood item found underneath (100Hz PSU ripple)

**Root cause found: the receiver's RX IF AGC was pumping the level at 1Hz,**
producing exactly the ~1Hz-spaced comb described across the last several
entries. Turning the RX AGC off made it clean, "at least to measurement
error level." **This closes out the whole close-in/AM-path sideband thread
- it was never a firmware, timing, or PWM-quantization issue on this
project's side at all; it was an artifact of the measurement chain.**

Worth being honest about the process here: the "no sidebands on the
carrier" physical reasoning two entries up (pointing at real per-sample
envelope-write timing jitter over gain-domain ripple) was a sound deduction
*given the data available at the time*, and would have been the right
conclusion if the cause had genuinely been internal - but the actual
source was external to the DUT entirely, which no amount of reasoning about
this project's own code could have anticipated. The static-envelope test
that was queued up as the next step is now moot for this specific finding
(though it may still be worth keeping in mind as a general clean-test
technique for anything AM-related in the future). **AM path noise floor,
once the receiver's own AGC artifact is excluded, now looks clean to
measurement-error level - genuinely good news for the whole EER chain.**

**What's left underneath, now clearly resolved as a separate item: 100Hz-
spaced spurs at -50dB.** This is a textbook, well-understood signature -
50Hz mains rectified/smoothed to DC typically leaves ripple at 100Hz (twice
mains frequency, from full-wave rectification) on a supply rail. This is a
hardware/power-supply finding, not a code one - nothing in this project's
firmware runs anywhere near 100Hz in a way that would explain it. -50dB is
a fairly clean level as bench supplies go; not flagged as urgent unless a
future target spec needs better, in which case the usual levers are
additional decoupling/LC filtering on whichever rail feeds the RSET/PWM/RC
stage, or a cleaner/better-regulated supply for that stage specifically.
Not investigated further here - noting it as understood and low-priority
unless it becomes a limiting factor later.

## 2026-09-09, later still: RESOLVED - the 400Hz "sub-IMD IMD" family on the two-tone test, confirmed by direct falsification test; likely retroactively implicates the long-running "spur forest" investigation

**User question:** noticed a family of products at 400Hz spacing sitting
BETWEEN the main 1200Hz-spaced IMD ladder on the default 700/1900Hz
two-tone test, tracking the main ladder's strength up and down ("sub-IMD
IMDs").

**Diagnosis:** the 700/1900Hz pair's envelope beats at their difference,
1200Hz - the expected spacing for AM/envelope-domain sidebands around the
carrier. But `SAMPLE_RATE_HZ` (16000) is NOT evenly divisible by 1200Hz
(16000/1200 = 13.33...). It IS evenly divisible by 400Hz: `GCD(16000,
1200) = 400`, and 40 samples (2.5ms) contains exactly 3 whole cycles of
1200Hz. That means the digitally-sampled realization of the envelope's
distortion products can only be truly self-similar cycle-to-cycle at
400Hz, not 1200Hz - so any quantization/rounding nonlinearity in the chain
(PWM's 10-bit duty steps, the predistort LUT, envelope generation itself)
that's sensitive to exact sample-to-cycle phase will leak real spectral
energy at multiples of 400Hz, riding on top of the "intended"
continuous-domain 1200Hz harmonic ladder - the same underlying mechanism
producing both, which is exactly why the 400Hz family's strength tracks
the 1200Hz ladder's strength. Same principle as the earlier 1600Hz
single-tone scope experiment (`Fs/F` integer -> clean), but showing up here
as genuine spectral content rather than a scope-triggering artifact,
because there's a real per-sample quantization nonlinearity involved this
time, not just a display/triggering effect.

**CONFIRMED by direct test: switching the two-tone pair to 700/1700Hz
(1000Hz spacing, `16000/1000 = 16` exactly - Fs-commensurate) "killed them
dead."** Clean, decisive falsification-test result - exactly the outcome
the mechanism above predicts, and about as strong a confirmation as this
kind of hypothesis ever gets.

**Retrospective implication, worth flagging clearly:** the legacy
700/1900Hz pair (1200Hz spacing) is the ONE two-tone preset that is NOT
Fs-commensurate - the `'T'` command's other five band presets are all
200Hz-spaced, and `16000/200 = 80` exactly. This means the entire
multi-day "spur forest" / gdeq-refit investigation in
`group_delay_fit_notes.md` (2026-09-05 through 09-07 - the a+A candidate,
candidate B, the "group-delay theory doesn't fully explain the real
spectral behavior" conclusion, envelope-domain-nonlinearity-or-unidentified-
mechanism left as the two open candidates) **was very possibly
contaminated by this exact same non-commensurate-sampling artifact the
whole time**, without anyone knowing to suspect it. This isn't proof that
mechanism is wrong, but it's a real confound that was never accounted for
in that investigation and should be closed off before trusting any of its
conclusions further.

**Recommended next step, high value:** re-run the core two-tone gdeq/ampeq/
spur-forest characterization on an Fs-commensurate pair (e.g. 700/1700Hz,
already confirmed clean above, or another 16000-divisor spacing) instead of
the legacy 700/1900Hz pair. This should give a materially cleaner picture
of the correction chain's real behavior, stripped of this newly-identified
sampling-grid artifact - quite possibly resolving or substantially
simplifying the still-open "why doesn't group-delay theory fully predict
the real spectral behavior" question from that entire investigation. Not
yet re-run.

**General practical takeaway for all future two-tone/IMD testing:** always
pick a tone spacing that evenly divides `SAMPLE_RATE_HZ` (16000) to avoid
this class of artifact contaminating results - the existing 200Hz-spaced
`'T'` presets already satisfy this by construction; the legacy 700/1900Hz
pair does not and should probably be treated with caution (or retired as
the reference pair) going forward.

**Two further observations on the same 700/1700Hz (Fs-commensurate) test,
both consistent with rather than separate from the finding above:** (1) the
general/broadband noise floor reads roughly 10dB lower than on the
non-commensurate pair; (2) it's noticeably less affected by the Serial-
diagnostic interference (the 45ms-cadence ~20Hz-broadband effect from
earlier). Plausible, not rigorously quantified, explanation for both: a
general noise-floor sweep likely wasn't fully resolving the 400Hz-family
comb into distinct lines the way the dedicated hi-res FFT did for the AM
work, so some of what read as "floor" before may have actually been this
same artifact, unresolved by the sweep's bin width - removing it would
plausibly drag the aggregate reading down too. The reduced Serial
sensitivity most likely isn't Serial activity doing less; it's that with
the dominant non-commensurate artifact gone, whatever Serial does contribute
now stands out as a smaller, separable effect against a quieter baseline
rather than being compounded with a bigger, messier problem. **Reinforces
the "retire 700/1900Hz as the default reference pair" recommendation above
even more strongly** - that legacy pair was very likely conflating several
genuinely separate issues (this sampling-grid artifact, the Serial effect,
and any real analog/PSU content) into one confusing picture, and testing on
an Fs-commensurate pair going forward should make each of them much easier
to isolate individually.

**Process lesson, worth keeping for future work:** the evidence for this
was sitting in the codebase's own comments the whole time -
`ssb_mic_test_commands.md:16` already described the `'T'` command's
200Hz-spaced presets as "clean single-point probes" while separately
flagging the 700/1900Hz pair as different (wider spacing, "kept last for
reference") - a real, already-documented difference in behavior between
test conditions that was never cross-examined for *why*. This project
already has a strong habit of diffing known-good vs. known-bad source
files when something regresses (see the `dac_task_enabled` investigation
earlier this session); the same discipline applies to test conditions
themselves - when one test configuration is reliably cleaner than another,
that consistent difference is a diffable fact worth asking "what's actually
different here?" about, not just a convenient probe to keep using.

## 2026-09-09 (cont'd) - random ~40Hz two-tone frequency jump (open, unresolved)

After retiring 700/1900Hz in favour of 700/1700Hz, a new symptom appeared
that does **not** match either the null-bias mechanism above or anything
previously logged: the TX frequency sits rock-stable for many minutes, then
makes a sudden discrete **jump** (not a drift) of up to ~40Hz, at no
identifiable trigger. Reported as happening "hands off" - no command sent,
no mode change. Occurs with diagnostics muted (`'v'` off), so it isn't
something the Serial-print path itself is causing.

Ruled out: the deterministic null-bias mechanism (`null_bias_investigation.md`)
is, by its own established theory, a *repeatable, coherent* bias tied to the
exact tone-pair/sample-grid alignment - it cannot manifest as a random,
sporadic jump. Two on-demand diagnostic snapshots taken around suspected
jump events did not catch one in progress: `overruns=0`,
`late_ticks_total=0`, and all `freq_dev`/`tx_freq` values tight and stable
throughout both captures. So the mechanism causing the jump remains
uncaught - on-demand snapshots are single points in time and there's no
continuous record to correlate against when a jump is seen on the SDR.

Proposed next steps (not yet actioned):
1. Re-confirm `dac_task_enabled=0` on the actual hardware build in use (the
   earlier `dac_task_enabled` stray-`1` bug is exactly this class of
   symptom-shape - a stray Core-1 I2C task producing intermittent cache
   stalls - so it's worth a direct re-check even though the user already
   fixed their local copy once this session).
2. Switch from manual on-demand snapshots to passive continuous logging
   (Serial output piped to a timestamped file) so a visually-observed jump
   on the SDR can be correlated against the exact diagnostic state at that
   wall-clock moment, rather than hoping a manual snapshot lands on it.
3. Add a new high-water-mark diagnostic, `max_freq_dev_step_hz` (same
   pattern as the existing `max_prep_us`/`max_busy_us`/`max_unclamped
   freq_dev`) that tracks the largest sample-to-sample change in `freq_dev`
   seen since last reset - this would catch the jump automatically without
   needing to have a snapshot land on the exact tick, and would also
   distinguish a genuine single-tick discontinuity from a fast-but-smooth
   drift. Not yet implemented - offered, awaiting go-ahead since it's a
   firmware change.

New `null_bias2` data point for 700/1700Hz, logged for the record: two
separate captures gave `weighted_bias=-15.99Hz` and, later,
`weighted_bias=-21.07Hz` to `-21.76Hz` for what should be the same tone
pair and mechanism. That spread (-16 to -22Hz) is itself unreconciled - per
`null_bias_investigation.md`'s own theory this number should be
deterministic and repeatable for a fixed tone pair, so either (a) there's
sample-grid-alignment sensitivity in the weighted-mean estimator that
wasn't accounted for in the original characterization, or (b) something
external (like the AGC finding above) is perturbing the underlying
envelope-null statistics between runs. Not investigated further yet. Note
also there is no clean numerological significance to "16-22Hz" the way
there was for the 400Hz family - this is a continuous function of the
Hilbert FIR's response at the specific 700/1700Hz frequencies, not an
integer-arithmetic artifact, so a near-match to "16 samples/cycle" is very
likely coincidental.

## 2026-09-09 (cont'd) - ADC FIFO "anomaly" in the ~465s two-tone capture: explained, benign

The large diagnostic dump taken during the reported "instability
mode...drifting about with high noise level" episode contained a line that
looked seriously wrong on first read:

```
[adc]   fifo: available now min=4294967295 max=0 (want>=5,<64) starve_ticks_total=0 drop_total=37389248
```

(`drop_total` climbing further to `37492384` later in the same capture).
`4294967295` is exactly `0xFFFFFFFFu` and `0` is exactly `0` - both are the
debug counters' own untouched *initial/reset* values
(`adc_capture.cpp`: `s_dbg_adc_fifo_min_available = 0xFFFFFFFFu`,
`s_dbg_adc_fifo_max_available = 0`), and `adc_capture.h:112`'s own comment
documents the expected normal reading as "min=0...starve_ticks_total in the
tens of thousands" - the opposite of what this capture showed. A
`drop_total` in the tens of millions over ~465s is roughly the same order
of magnitude as the total sample count the ADC would produce in that time
(80000 sps x 465s = 37.2M) - i.e. it looked like the ADC FIFO was
overflowing on essentially *every* sample, continuously, for the whole run.

Traced to source and now fully explained, and it is **benign, not a bug**:

- `ssb_mic_test.ino`'s per-tick dispatch (~line 537-568) only calls
  `adc_capture_read_next_sample()` - the function that updates the
  min/max/starve stats - in the `else` branch that's reached when
  `audio_source_t` is none of `AUDIO_SRC_TWOTONE` /
  `AUDIO_SRC_SINGLETONE` / `AUDIO_SRC_ENVSTEP` / `AUDIO_SRC_FMTEST` /
  `AUDIO_SRC_AMTEST` - i.e. only in real mic mode. In `TWOTONE` mode the
  sample comes from `generate_twotone_sample()` instead, and the ADC
  reader is never called at all.
- But `adc_continuous_start()` (`adc_capture.cpp:218`) is called once,
  unconditionally, in `setup()`, and is never stopped or gated on the
  selected audio source - the ADC hardware/DMA/ISR keeps running and
  filling the FIFO from the live mic input the entire time, regardless of
  test mode.
- Put those two together: in two-tone mode the ISR fills the FIFO
  continuously from real hardware, nothing ever drains the tail side, so
  the FIFO fills once and then every subsequent ISR batch overflows -
  incrementing `drop_total` essentially every sample, forever. Since the
  *reading* function (the only place `min`/`max` get touched) is never
  called in this mode, those two stats simply stay frozen at their
  compile-time sentinels the whole run. Every part of the "anomaly"
  is explained by this one fact.

Cross-checked against `[adc] actual=80645-80795 sps (expected=80000)` and
`pool_ovf_total=0` elsewhere in the same capture: those numbers describe
the ADC/DMA driver's own health independent of the FIFO-reader stats, and
they're fine - consistent with "the hardware ADC pipeline itself is
running fine, just unconsumed" rather than any driver-level fault.

**Implication for the "instability/drifting/high noise" report:** this FIFO
behavior is a side effect of leaving the ADC free-running and unused during
two-tone test mode - it does not touch the two-tone signal path at all
(that path is 100% synthetic, generated in `dsp_task` from the phase
accumulators, never touches the ADC FIFO). So it cannot be the cause of the
reported instability, and is very likely present, identically, in *every*
two-tone-mode capture taken so far - it just hadn't been read closely
before. It also means this diagnostic line is not currently useful evidence
in either direction for the two-tone jump investigation above; if it's
worth cleaning up cosmetically (skip the ADC continuous driver, or skip
printing/updating this line, when not in mic mode) that's a small, safe,
optional firmware tidy-up rather than something diagnostic.

**Open discrepancy still not resolved:** the actual `freq_dev`/`tx_freq`
snapshot values recorded throughout this entire ~465s capture are tightly
clustered and stable (`freq_dev` roughly 1198-1202Hz, `tx_freq` locked to
within a few Hz) - i.e. the capture itself does not show the drifting/noisy
behavior that was reported as happening during it. Two possibilities: the
instability happened outside this capture's time window, or it happened
between on-demand snapshots and the spot-sampling simply didn't land on it
(same blind spot as the 40Hz-jump investigation above - reinforces the case
for switching to continuous passive logging rather than manual snapshots
for chasing intermittent events going forward).

## 2026-09-09 (cont'd) - firmware change: null_bias lines now exempt from mute

User's proposal for chasing the random 40Hz jump: force the null-bias
detector's output to always print, so it can be watched live and checked
for a coincident anomaly the moment a jump is seen on the SDR - without
re-enabling the full diagnostic stream and its known ~20Hz broadband noise
side effect (see the "'v' toggle / serial noise" findings earlier in this
log).

Implemented in `diagnostics.cpp`:
- Pulled the `null_bias`/`null_bias2`/`null_bias3` print block out of
  `print_timing_and_adc_block()` into its own function,
  `print_null_bias_block()`.
- `diagnostics_service()` now calls it on its own independent 1000ms timer,
  deliberately **not** gated by `s_diag_muted` - so it keeps printing once
  a second even with `'v'` off, while the 45ms fast line and the rest of
  the 1000ms block stay muted as before.
- `diagnostics_print_now()` (the on-demand snapshot command) updated to
  call it explicitly too, since it no longer comes along for free via
  `print_timing_and_adc_block()`.
- Left `diag_room_for()`'s per-line TX-buffer-safety guard in place on all
  three lines - only the mute gate was removed, not the buffer-overrun
  protection.

Rationale for expecting this to be safe: the ~20Hz broadband noise
mechanism was pinned down earlier to the 45ms fast status line (1/0.045 Hz
= 22.2Hz, the observed spacing) - a continuous, high-rate burst of Serial
traffic. The null_bias block is three short printf calls once per second;
that's a tiny fraction of the USB-CDC bandwidth the fast line stresses, so
it should not reproduce that artifact. **Not yet bench-verified** - worth
a quick spectral check with `'v'` off and just this line streaming, to
confirm no new sidebands appear, before relying on it for the jump hunt.

How to use it for the jump investigation: run two-tone with `'v'` off as
usual (avoiding the fast-line noise), watch the `null_bias2 weighted_bias`
line scroll by once a second, and note whether it makes a discrete,
persistent step at the same moment a jump is seen on the SDR. Since these
stats are cumulative sums since the last `'r'` reset, a single
badly-behaved near-null sample (e.g. a phase-unwrap edge case) could in
principle permanently shift the running weighted average even though it
only affected one 62.5us tick - so a coincident step in this number would
be meaningful evidence tying the jump to the null/Hilbert-phase mechanism,
whereas no change at all would point away from it and toward something
else (SPI write glitch, supply transient, cache stall, etc.).

**RESULT (2026-09-09, same day): tried on the bench - negative for this
hypothesis, but a real, useful finding in its own right.** User watched
`weighted_bias` continuously (after a couple of `'r'` resets and a preset
load) while comparing against the SDR: "No relation between output and
actual freq" - the real transmitted frequency stayed stable while
`weighted_bias` swung by tens of Hz on its own (e.g. -34Hz shortly after a
reset, drifting back toward ~0, then off again past -19Hz, all with no
external event). So `weighted_bias` is **not usable as a live jump
detector** - it doesn't track the real TX frequency at all.

Root cause of the swinging, now understood and logged in detail in
`null_bias_investigation.md`'s new 2026-09-09 update: `weighted_bias` is a
cumulative average since the last `'r'` reset that converges MUCH more
slowly than assumed - tens of seconds to minutes, not the "reset, wait
1-2s, read" recipe the original characterization used - and it responds to
preset changes (gdeq/ampeq/predistort settings shape the envelope through
each null, which shapes where the average is heading), not to the actual
carrier frequency. Bonus payoff: this also explains a previously-unresolved
puzzle from 2026-08-31 (`null_bias_investigation.md` item 4) about a ~15Hz
drift confounding an `'I'` on/off comparison "on the timescale of typing a
sentence" - almost certainly this same slow-convergence behavior, not a
separate mechanism. Practical fallout: the null-bias investigation's
"Confirmed measurement table" may need re-taking with a longer, fixed dwell
time before trusting those weighted_bias numbers as precise per-tone-pair
constants.

Net effect on the jump hunt: back to the two remaining proposed approaches
(re-confirm `dac_task_enabled`, and/or continuous passive Serial logging
correlated against wall-clock SDR observation, and/or the proposed
`max_freq_dev_step_hz` high-water-mark diagnostic) - those instrument the
real freq_dev/tx_freq signal directly rather than a derived slow statistic,
so they remain the more promising paths forward. Still awaiting user
direction on which to pursue.

## 2026-09-09 (cont'd) - leading theory for the random 40Hz jump: a corrupted persistent value, not a DSP/timing effect

`dac_task_enabled` re-confirmed as `0` on the actual hardware - the earlier
known stray-I2C-task bug is ruled out as the cause of this symptom.

Traced the entire `freq_dev_hz` -> TX-frequency pipeline end to end
specifically looking for anything that could hold a WRONG value rather than
just glitch for one tick, since "sits stable for minutes, then a discrete
step that locks and doesn't drift back" is not what a transient glitch
looks like - a transient should self-correct on the very next 62.5us tick.
Findings, all confirmed by reading the actual source:

- `ssb_dsp.c`: `dphi` is computed fresh every tick from that tick's own
  `atan2` phase difference. `prev_phase` stores the raw, wrapped `atan2`
  output (not a running unwrapped total), so one bad sample cannot compound
  into subsequent ticks - each tick's `freq_dev_hz` is fully independent of
  the last.
- `relative_delay.cpp`: the ring buffer is only `PHASE_DELAY_MAX_SAMPLES=8`
  deep and every slot is overwritten every single tick - nothing here can
  persist more than ~500us, and even a corrupted `s_relative_delay_samples`
  would only mistime which recent sample is read, not shift the carrier's
  absolute frequency.
- `AD9851.c`'s `ad9851_set_frequency()`: resends the FULL 40-bit frame -
  FTW bytes AND the control byte (6x-multiplier/power-down/phase bits) -
  freshly computed from the handle's own fields on every single call. A
  one-off SPI/EMI glitch on the wire should be corrected by the very next
  write, 62.5us later.

That leaves exactly two values anywhere in this pipeline that are written
ONCE at boot and never touched again by any command, preset, or reset:
`s_carrier_hz` (`carrier_output.cpp`, the calibrated 14200160Hz base) and
`handle->ftw_reciprocal` (a plain, non-`volatile` `uint64_t` inside the
AD9851 driver struct, computed once in `ad9851_init()`). **Leading theory:**
a one-off corruption of one of these two values would produce exactly the
observed signature - an instantaneous step, with nothing in the normal code
path ever re-deriving or refreshing either value, so it stays wrong
("locks") until another such event nudges it again, in either direction
("stepped -28 to +28... currently spot on correct" reads as two independent
glitches that happened to roughly cancel, not one thing self-correcting).

Two flavors of root cause under this theory, worth distinguishing:
1. **A genuine host-side memory-safety bug elsewhere in the firmware** (a
   stray pointer, an unbounded array write, a stack overrun) scribbling
   over that exact RAM location. If so, `s_carrier_hz`/`ftw_reciprocal`
   are just the most VISIBLE victims (a frequency shift shows up
   immediately on an SDR) - other persistent state elsewhere (gdeq/ampeq
   filter coefficients, envelope calibration constants, master gain) could
   in principle be getting silently corrupted the same way without any
   equally obvious symptom to notice it by.
2. **A hardware-level single-event upset** - an SRAM bit flip from ESD or
   RF pickup. Worth taking seriously specifically because this is a
   transmitter bench: the project's OWN RF PA/antenna field sitting right
   next to the ESP32's digital section is a very plausible coupling path,
   distinct from ordinary cosmic-ray-type soft errors.

**Open questions this raises, if the theory holds up** (per user's own
observation - logging for when this is picked back up):
- Self-inflicted RFI vs. external/cosmic single-event upset vs. a genuine
  firmware memory-safety bug - each points to a completely different fix
  (shielding/grounding/decoupling vs. nothing actionable vs. an audit for
  unbounded writes elsewhere in the codebase).
- If self-RFI: does jump frequency correlate with RF drive level, antenna
  proximity, or SWR? A low-power vs. full-power A/B would test this
  directly.
- Does the ESP32-S3 have any SRAM ECC/parity protection on the region these
  variables live in? If it does and a flip still gets through, that leans
  away from a simple cosmic-ray-style soft error and toward RFI or a real
  firmware bug (ECC would normally catch/correct single-bit soft errors but
  not a bug that legitimately writes to the wrong address).
- Is corruption confined to just these two variables, or could other
  silent, harder-to-notice persistent state (filter coefficients, envelope
  calibration) be affected too, just without an equally obvious symptom?
- Practical mitigation independent of root cause: could the firmware
  periodically re-assert the known-good value of `s_carrier_hz`/
  `ftw_reciprocal` (e.g. once a second) as a cheap self-healing measure,
  separate from ever finding the true root cause?

**Implemented (2026-09-09):** the canary diagnostic, across `AD9851.c`/`.h`,
`carrier_output.cpp`/`.h`, and `diagnostics.cpp`:

- `AD9851.c`/`.h`: added `ftw_reciprocal_known_good`, an independent shadow
  copy of `ftw_reciprocal` taken once in `ad9851_init()` right after the
  real value is computed, stored in a different struct field/RAM address.
  New `ad9851_get_canary()` returns both values for comparison. A
  mismatch between them is strong evidence of exactly the corruption this
  theory predicts (not airtight - a corruption event could in principle
  hit both fields at once - but a single-address bit flip or stray write
  hitting two different fields simultaneously is far less likely).
- `carrier_output.cpp`/`.h`: new `carrier_output_get_carrier_hz()` returns
  the live `s_carrier_hz`, checked against the compile-time `CARRIER_HZ`
  constant itself - immune to RAM corruption, since it's baked into the
  comparison code rather than sitting in a second variable. New
  `carrier_output_get_canary()` forwards to the AD9851 driver's canary
  check.
- `diagnostics.cpp`: new `print_canary_block()` prints two lines,
  `[canary] carrier_hz=... (boot=...) OK` and `[canary]
  ftw_reciprocal=0x... (boot=0x...) OK` (or `MISMATCH! first seen at
  t=...ms` if either check fails - a latched high-water-mark timestamp,
  same pattern as `max_busy_us` etc., that survives even if a later read
  happens to match again). Called from the SAME ungated (mute-exempt)
  1000ms timer as `print_null_bias_block()`, so it keeps checking even
  while running muted for the jump hunt, and also added to the on-demand
  snapshot (`diagnostics_print_now()`). Deliberately EXCLUDED from
  `diagnostics_reset()` (the `'r'` command) - a canary meant to catch a
  rare, possibly once-per-session event must not get silently cleared
  every time someone starts a fresh measurement window; it only clears on
  reboot.

Files delivered: `AD9851.c`, `AD9851.h`, `carrier_output.cpp`,
`carrier_output.h`, `diagnostics.cpp`. Next jump (if it recurs) should show
up directly as a `MISMATCH` line with a timestamp, confirming or refuting
the theory outright rather than by inference.

## 2026-09-09 (cont'd) - two new data points against the theory, both logged for when this resumes

1. **User has only ever seen this jump in TWO-TONE mode (not confirmed -
   just never noticed it elsewhere).** This is a real constraint on the
   theory if it holds up: a corrupted persistent value (stray firmware
   write or a physical RAM bit-flip) shouldn't care what audio source mode
   is active - it's mode-agnostic by nature. The one thing genuinely
   unique to two-tone versus mic/single-tone/AM-test is that its envelope
   hits an exact, repeated zero every cycle - real voice essentially never
   does, and the other test modes don't either. Went looking specifically
   at code that behaves differently right at an envelope null:
   - `test_signals.cpp`'s two-tone generator - no arrays/indexing at all,
     just two scalar phase accumulators. Clean.
   - `ssb_dsp.c`'s `fast_atan2`/`fast_sqrt` - `fast_atan2` explicitly
     guards `x==0 && y==0`, `fast_sqrt` explicitly guards `x<=0`. Both
     handle the degenerate null case safely, no undefined behavior found.
   - `envelope_predistort.cpp`'s LUT lookup - envelope is clamped to
     [0,1] before the index is computed, index is always 0..64 with an
     explicit early-return at the top bound. Bounds-safe, and read-only
     regardless.
   All three obvious null-adjacent suspects came back clean - doesn't rule
   out the "two-tone-specific" angle (something subtler in the Hilbert FIR
   or gdeq/ampeq filter state under near-zero envelope could still be it),
   but also doesn't confirm it. Equally plausible mundane explanation:
   two-tone is simply the mode run continuously for the longest unattended
   stretches while testing, so it's had more opportunities to show up,
   independent of any real mode-specific mechanism. Worth checking next
   time: was mic/single-tone ever run for a comparably long stretch without
   incident, or just never run that long?
2. **No jump seen on this run since the canary code was added.** Worth
   treating cautiously for now - the jump has historically been rare and
   unpredictable (stable for many minutes to longer, then one discrete
   event), so a short quiet stretch isn't strong evidence either way. BUT
   if a much longer soak stays quiet, that's actually a more specific clue
   than "problem fixed": adding a few new static variables/functions shifts
   where everything else lands in RAM at link time. A true external event
   (ESD, RF pickup, a cosmic-ray-style soft error) wouldn't care about the
   linker map and would still eventually corrupt SOMETHING, just maybe not
   a variable that produces a visible symptom next time. A bug that's
   sensitive to memory layout - a stray write landing wherever something
   happens to sit at a fixed offset - is the classic signature of a real
   buffer overrun or wild pointer elsewhere in the firmware, not a hardware
   event. So a long quiet stretch would paradoxically lean TOWARD "real
   firmware bug, now coincidentally not landing on s_carrier_hz/
   ftw_reciprocal" rather than "problem solved" - worth remembering not to
   declare victory even if this stays quiet for a while; the canary staying
   green is reassuring but not proof nothing is still happening elsewhere
   in memory.

**Follow-up same day:** user recalls switching between modes MIGHT have
been associated with the big jumps previously (not certain). Checked the
obvious candidates in the switching/preset code path itself for a
mode-switch-triggered bug:
- `serial_commands.cpp`'s digit-preset handler: `preset = c - '0'` for
  `'0'-'9'` indexes `settingsPresets[10]`, which has its own
  `static_assert(sizeof(settingsPresets)/sizeof(settingsPresets[0]) == 10,
  ...)` in `settings.h` - exactly matches the 10 possible digits, no
  off-by-one possible.
- `test_signals.cpp`'s `'T'` two-tone band cycling
  (`s_band_index = (s_band_index + 1) % TWOTONE_BAND_COUNT`) and `'R'`
  tone-ratio cycling (`s_tone_ratio_index = (s_tone_ratio_index + 1) %
  TONE_RATIO_COUNT`) both use proper modulo wraparound - never run past
  their array bounds either.

All clean - no smoking gun in the switching path, consistent with
everything else checked so far this session. Now also confirmed: since the
canary code was added, small (few-Hz) nudges on preset switch are
observed, but NO big/locked jumps - the few-Hz nudges are almost certainly
the SAME benign null-bias-mechanism response characterized earlier today
(different presets carry different relative_delay/gdeq/ampeq settings,
which legitimately reshape the near-null envelope trajectory and shift
where the null-crossing resolution lands by a few Hz) - not a red flag,
and worth clearly distinguishing from the big jumps this whole thread is
actually chasing.

Suggested next test, not yet run: deliberately cycle through several mode/
preset switches back-to-back for a few minutes (rather than switching once
and then sitting idle) while watching the canary lines, to actively try to
provoke a big jump rather than waiting passively for one. If that
reproduces it, the switch path becomes the prime suspect after all (worth
a second, closer look at the code above, e.g. for a race between Core 1's
handler and Core 0's dsp_task reading the same settings mid-update); if it
still never reproduces that way, that's evidence the earlier "hands off,
sits stable for minutes" description was the more accurate one, and
switching isn't the trigger.

**Follow-up same day:** user searched a ~2000-line capture (deliberate
preset-abuse testing plus general running) for `MISMATCH` - none found, and
no big jumps observed either. Canary and symptom continue to track each
other exactly as the theory would predict (if the corruption isn't
happening, the canary has nothing to flag) - consistent with, but not yet
confirming, either explanation from the earlier entry (genuinely
quiet/rare, or masked by the memory-layout shift from adding the canary
code itself). Still no positive detection to confirm the theory outright -
the canary has not yet actually caught anything, so it remains untested in
the sense that matters (it would need to fire at least once, coincident
with an observed jump, to move this from "plausible" to "confirmed").
Recommend continuing the same passive monitoring plus the earlier-suggested
mic/single-tone comparable-duration test (see the "only ever in two-tone"
entry above) - the longer this stays clean across a range of modes, the
more it either supports "actually fixed by the layout shift" or starts to
suggest the original trigger condition was rarer/more specific than
thought, rather than confirming the theory either way.

## 2026-09-09 (cont'd) - REGRESSION FOUND AND FIXED: my own mute-exemption change was likely causing a new "noisy" symptom

User reported a new symptom: the board would go into a "noisy" mode (high
close-in background noise) with no jumps, cleared by a reboot, and
described it as "looks like when 'v' is on but it isn't." That description
is a near-exact match for a self-inflicted regression from earlier today's
work: `print_null_bias_block()` and the (at-the-time) `print_canary_block()`
were both deliberately exempted from `s_diag_muted` so they could be
watched live while muted (see the null_bias entries above). That meant
there was now ALWAYS 5 lines/sec of Serial traffic happening regardless of
`'v'`'s actual state - i.e. "muted" no longer actually meant silent. Given
the earlier-established mechanism (continuous Serial diagnostic output ->
~20Hz broadband noise, tied to print rate/volume), this fits the reported
symptom exactly, and explains "looks like v is on but it isn't": part of
the diagnostic stream WAS effectively always unmuted, independent of what
`'v'` displayed.

**Fixed, same day:**
- `print_null_bias_block()`'s exemption reverted - it's back under the
  normal `!s_diag_muted` gate in `diagnostics_service()`, same as
  everything else in that 1000ms block. Its only reason for being exempt
  (watching it live while muted, to catch a real jump) is gone anyway -
  that experiment already ran and showed weighted_bias doesn't track the
  real jump at all (see the earlier RESULT entry above). Still reachable
  on demand via `diagnostics_print_now()` regardless of mute state.
- The canary was redesigned rather than just re-gated, since it's still
  genuinely useful to run while muted. Split into two functions:
  `canary_check_background()` (called unconditionally, every
  `diagnostics_service()` call, still outside the mute gate) only prints
  `[canary] ... MISMATCH! first seen at t=...ms` the FIRST time either
  check transitions from OK to bad - it is now a complete no-op (no Serial
  traffic at all) during normal, healthy operation, so it can stay
  mute-exempt without contributing any background noise. Checking every
  call instead of once a second is a free improvement while at it - lower
  latency to catching a real event, since there's no cost when nothing's
  wrong. `canary_print_status()` is the explicit "show current OK/MISMATCH
  state" version, called only from the on-demand snapshot
  (`diagnostics_print_now()`), where printing unconditionally is fine
  since it's a one-off requested read, not a background stream.

Net effect: `'v'` muted should now mean genuinely, completely silent again
(matching its original pre-2026-09-09 behavior), while the canary keeps
watching in the background for free and will still report a corruption
event immediately if one ever occurs. Files changed: `diagnostics.cpp`
only. **Not yet bench-verified that this actually fixes the reported
noisy-mode symptom** - next occurrence (or lack thereof) is the real test;
worth specifically trying to reproduce the noisy mode again now that the
mute-exemption regression is removed, to confirm this was really the
cause rather than a coincidence.

## 2026-09-10 - new jump occurrence: smaller (±8Hz), sideband-like, canary silent

User reports a new jump event this morning: TX frequency stepping ±8Hz
(smaller than the earlier ~28-40Hz events) and, distinctively, producing
audible/visible sidebands at an ~8Hz offset rather than (or in addition to)
a clean single-direction relocation. **No `[canary] ... MISMATCH` reported
at the time** - this is the most important new fact, since the 2026-09-09
canary was built specifically to catch corruption of `s_carrier_hz` or
`ftw_reciprocal`, the two leading suspects from that investigation. If this
event is confirmed to have happened with the canary genuinely silent
throughout, that's evidence against those exact two variables for THIS
occurrence - either this is a different/smaller mechanism than the original
40Hz jumps, corruption is hitting some other persistent value the canary
doesn't cover, or (see below) this isn't digital-state corruption at all.
**Not yet confirmed the user specifically checked the log for a MISMATCH
line at the jump timestamp** rather than just noting the absence of any
gross error/crash - worth nailing down before drawing conclusions.

Also reported: the two recovered audio tones shift by the same number of Hz
each event, which the user reads as ruling out a "scaling factor." Worth
flagging that this doesn't actually discriminate between the live theories
at these frequencies - both tones sit within ~1-2kHz of each other on a
14.2MHz carrier, so even a genuine proportional (ppm-level) error - whether
from `ftw_reciprocal` corruption or real REF_CLK frequency instability -
would produce two absolute Hz shifts too close to distinguish from an
additive shift given typical receiver/counter resolution. The equal-shift
observation is real and worth recording, but it doesn't yet separate
"something scaled the whole 14.2xxx MHz number" from "something added a
constant to it."

Separately, user re-confirmed the existing `null_bias`/`weighted_bias`
diagnostic ("reported freq bias") does not track this symptom either - it
sat reporting ~10Hz while the actual transmitted frequency (checked against
the receiver) was spot-on nominal. This matches the null-bias
investigation's own prior finding (`null_bias_investigation.md`,
2026-09-09 update: "this metric's wandering does NOT track the actual
transmitted frequency") - not new evidence about the jump mechanism, just a
second confirmation that `weighted_bias` is the wrong tool for chasing this
specific symptom and should be disregarded when characterizing it.

**New candidate mechanism worth taking seriously given today's specific
signature (small magnitude, sideband-like, canary silent, occurred "this
morning" specifically):** genuine REF_CLK reference-oscillator instability
rather than digital-state corruption - e.g. thermal drift/warm-up wobble in
the uncompensated XO (`carrier_output.h`'s own `CARRIER_HZ` comment already
establishes this crystal is "not a precision reference"), supply-rail
ripple reaching the oscillator, or old-fashioned crystal microphonics
(mechanical vibration on the bench modulating the XO). Any of these would
produce real, physical FM sidebands on the transmitted carrier - matching
"generates 8Hz sidebands" more literally than a discrete digital step would
- and would leave every digital value (canary included) completely
correct, since nothing in RAM actually changed; the reference frequency
itself just wavered. This is a different failure class from the
2026-09-09 "corrupted persistent value" theory and would need a different
kind of evidence to confirm (e.g. correlating jump timing with time-since-
power-on/thermal state, touching/tapping the board or XO can while
monitoring, or scoping the REF_CLK line itself if feasible) rather than
more firmware auditing.

**Same session, second occurrence, minutes later: -25Hz jump, canary
confirmed clean.** User explicitly checked this time - no `[canary]
MISMATCH` around either event, and `weighted_bias` again reported an
unrelated, stable-ish number (14Hz) while the real jump was -25Hz. Two
canary-clean events in one sitting, one -8Hz-ish and one -25Hz, is
meaningfully stronger than the single quiet stretch logged on 2026-09-09 -
that entry only had absence of jumps to go on; this is jumps actively
happening WITH the corruption detector watching and finding nothing.
Shifts weight further away from `s_carrier_hz`/`ftw_reciprocal` corruption
as the mechanism (at least as those two specific variables) and toward
either (a) corruption of some other, not-yet-instrumented persistent value,
or (b) the REF_CLK/analog-instability theory above, or (c) a digital
glitch that never lands in a persistent variable at all - e.g. a one-tick
corruption of `tx_freq` between computation and the SPI write, which the
canary (checking `s_carrier_hz`/`ftw_reciprocal` themselves) would never
see even if it happened every single time.

**Open questions / next steps, not yet actioned:**
1. Whether these jumps LOCK (persist until the next event, matching the
   original 2026-09-09 "40Hz jump" behavior) or self-correct on their own
   shortly after - not yet stated either way this session. This is the
   single most useful missing fact: a self-correcting wobble points at
   REF_CLK/analog or a transient one-tick glitch; a locked step that stays
   put points back at persistent-state corruption, just not the two
   variables currently instrumented.
2. The previously-proposed `max_freq_dev_step_hz` high-water-mark
   diagnostic (tick-to-tick delta on `delayed_freq_dev_hz`/`tx_freq`,
   flagged in the 2026-09-09 entries as "awaiting user direction") would
   directly settle whether an event this size ever shows up upstream, in
   the digital signal itself, before the SPI write - if it never does even
   while a real jump is observed at RF, that's strong evidence the fault is
   downstream of the digital math entirely (REF_CLK/analog/hardware, or
   something in the AD9851 SPI transfer itself). Worth implementing now
   given jumps are actively reproducing this session.
3. If genuinely not locking and not upstream, look for a thermal/
   mechanical/supply correlation ("this morning," cold bench) rather than
   continuing the memory-corruption audit.
4. Given the smaller, varying magnitude (-8ish, then -25) and different
   character (sidebands) versus the original ~28-40Hz jumps, consider
   explicitly whether this is the SAME symptom recurring or a second,
   distinct issue - don't merge the two without more data.
5. Reconfirm `weighted_bias`/`null_bias` stays excluded from this
   investigation's evidence base - two more data points (10Hz-reported/
   spot-on-actual, then 14Hz-reported/-25Hz-actual) both reconfirm it does
   not track this symptom.

**Same session, third occurrence: -25Hz -> +25Hz -> back to 0, over ~30
seconds, hands-off, low ambient noise floor.** This is a materially
different shape than anything logged before - not a step that locks
(the 2026-09-09 original description) and not even a single discrete jump,
but a smooth-ish symmetric excursion and return with zero user
interaction. This is very hard to explain with a one-off RAM/persistent-
value corruption model (would need two independent, oppositely-signed
corruption events, ~15s apart, of coincidentally similar magnitude, that
happen to land back on the original value - implausible as coincidence).
It fits a genuine physical/analog wander far more naturally - REF_CLK
thermal drift or warm-up creep, supply-rail-induced pulling, or crystal
microphonics - all of which would plausibly swing and relax back over a
timescale of seconds, especially on a low-cost, uncompensated XO
(`carrier_output.h`'s own `CARRIER_HZ` comment already establishes this
part is not a precision reference) sitting right next to this project's
own RF PA. Noted but not yet explained: why a 30-second period specifically
- worth watching whether repeats land near the same duration (would
suggest a specific thermal/electrical time constant somewhere) or vary
widely (would argue against a single clean mechanism).

**`max_freq_dev_step_hz` diagnostic implemented (2026-09-10),
`diagnostics.cpp` only** - direct answer to open item 2 above, and the
user independently arrived at wanting this at the same time. Tracks the
largest tick-to-tick delta in `tx_freq` (the exact ground-truth Hz value
handed to `ad9851_set_frequency()` every 62.5us tick, per
`carrier_output.h`'s own doc comment) since the last reset:
- New statics next to `s_dbg_tx_freq`: `s_dbg_prev_tx_freq`/
  `s_dbg_have_prev_tx_freq` (comparison state), `s_dbg_max_freq_dev_step_hz`
  (the high-water mark itself), `s_dbg_max_freq_dev_step_from_hz`/`_to_hz`
  (the two consecutive tx_freq values straddling the worst step, so the
  actual before/after Hz is visible, not just the delta), and
  `s_dbg_max_freq_dev_step_at_ms` (latched timestamp, same since-boot clock
  family as the `[canary]` timestamps, for cross-checking against wall-
  clock SDR observation).
- `diagnostics_set_tx_info()` (the existing per-tick hot-path hook, already
  IRAM_ATTR, already receiving `tx_freq` as its "ground truth" parameter -
  no new call site needed anywhere) now also computes `abs(tx_freq -
  prev_tx_freq)` and updates the high-water mark. Uses
  `esp_timer_get_time()`, not `millis()`, since this runs on the dsp_task
  hot path.
- New line in `print_timing_and_adc_block()`, right after the existing
  ad9851-breakdown line, inside the same `#if AD9851_ATTACHED` guard, same
  `diag_room_for()` per-line gating as everything else in this file:
  `[dsp]   max_freq_dev_step: <N>Hz (<from> -> <to> Hz, at t=<ms>ms)`.
  Automatically included in the on-demand snapshot (`diagnostics_print_now()`)
  too, since that already calls `print_timing_and_adc_block()`.
- **Resets with `diagnostics_reset()` ('r')**, unlike the canary - a
  deliberate difference: the canary must survive resets because it's
  chasing an assumed-extremely-rare, possibly-once event, but this stat is
  meant to be zeroed right before a monitoring stretch and read after,
  matching the "r, wait, read" pattern every other test in this
  investigation already uses. `s_dbg_have_prev_tx_freq` resets to `false`
  (not just zeroing `s_dbg_prev_tx_freq`) so the tick right after a reset
  never compares against a stale pre-reset value - same reasoning already
  used for `s_last_samples_total`/`s_last_callback_count` in this file.

**What this is for:** settles whether an event of this size ever appears in
the digital signal itself, before the SPI write. If the next real jump (or
the next slow excursion, given the -25/+25/0 shape above) shows 0 or
near-0 here while the SDR clearly shows the frequency moving, that's fairly
decisive evidence the fault is downstream of every bit of this firmware's
math - REF_CLK, the AD9851's internal 6x-multiplier PLL, or the SPI
transfer itself - and further firmware auditing of the freq_dev/delay-line/
carrier-addition path would not be the productive next step. If it DOES
show a matching step, that points back at something in this digital chain
after all, just not `s_carrier_hz`/`ftw_reciprocal` (already ruled out by
the canary staying clean across all three occurrences logged today) - a
`tx_freq` corruption between computation and the SPI write, or an
`ad9851_set_frequency()`/SPI-level fault, would become the next things to
look at specifically. Not yet bench-verified - awaiting the next jump.

## 2026-09-10 (cont'd) - USB supply theory sharpened: two-tone's active FTW churn is a plausible reason the jump/wobble is mode-specific, plus why "too-clean" step transitions don't argue against it

Follow-on reasoning session (no new bench data yet) tying the still-open
"only ever seen in two-tone" observation (2026-09-09 entry above) together
with the REF_CLK/analog-instability theory from the 2026-09-10 entries,
plus a specific plan for the next bench session.

**Mechanism proposed:** USB bus power is a plausible noise source for
exactly the failure signature seen so far - switching noise/ripple/ground
bounce on the 5V rail, which (if it reaches the AD9851's REF_CLK oscillator
or its internal 6x-multiplier PLL bias, even indirectly through shared
supply/ground) would perturb the transmitted frequency while leaving every
digital value this firmware computes untouched. That matches everything
logged so far: canary clean, `tx_freq` itself rock-stable in the diagnostic
stream, `weighted_bias` uncorrelated - because the fault sits after
everything the firmware computes, same conclusion the `max_freq_dev_step_hz`
diagnostic (2026-09-10, above) was built to test for directly.

**New piece: why two-tone specifically, tying back to the still-open
"only ever in two-tone" item.** In the sine/CW and AM/PWM-envelope test
modes, `freq_dev_hz` sits at (or very near) zero, so the FTW value sent to
the AD9851 over SPI is nearly identical tick to tick - low bit-toggling
activity on that bus. In two-tone mode, `freq_dev_hz` is actively swinging
every tick (the whole basis of the phase-modulation two-tone synthesis), so
the FTW - and therefore the SPI/bit-bang switching activity on `carrier_output.cpp`'s
write path - changes almost every tick. More bit-toggling means more
instantaneous current draw on the same 5V rail feeding the AD9851. If USB
power is marginal (higher source impedance, less transient headroom than a
bench supply), two-tone mode is uniquely positioned to provoke this kind of
supply-induced REF_CLK/PLL disturbance, while sine/AM's near-static FTW
writes wouldn't stress the rail the same way. This is a second variable
alongside the supply swap itself, not a competing theory - worth watching
during the test below.

**Objection considered and resolved: the discrete step-like transitions
(e.g. -46 -> +46 -> 0) seemed "too exact" to be ordinary analog noise.**
This doesn't argue against the hardware theory. A marginally-locked PLL
(the AD9851's 6x multiplier working off a noisy/borderline reference)
doesn't have to drift smoothly - it can snap between a small number of
discrete quasi-stable lock states as the loop re-acquires, producing clean,
repeatable steps rather than continuous wander, even though the root cause
is still analog/supply-side. A crisp step is consistent with "PLL hunting
under a noisy reference," not just with "something in firmware changed by
exactly one value" - so it does not discriminate against this theory the
way it first appears to.

**Planned test (not yet run):** swap the board to a clean external supply
(off USB bus power) and re-run the same 700/1700Hz two-tone setup
(2026-09-09's Fs-commensurate pair) for an extended, hands-off stretch -
ideally past whatever timescale produced jumps/excursions before (the
30-second excursion and the multi-minute-stable-then-jump pattern both
logged above). Watch both the receiver/SDR and this project's own
diagnostics (`max_freq_dev_step_hz`, the `[canary]` lines) concurrently.
Interpretation:
- If jumps/wobbles stop entirely on clean power: about as close to
  confirmation as this investigation gets without instrumenting REF_CLK
  directly - points the fix at power delivery (external supply, or
  decoupling/filtering on the rail feeding the AD9851/REF_CLK) rather than
  firmware.
- If they still occur on clean power: rules out USB-specific noise
  specifically, narrows the remaining candidates to general supply ripple
  from whatever regulator is used, thermal effects, or a genuine AD9851/PLL
  issue independent of supply quality - and reopens the digital-corruption
  and REF_CLK-instability theories from the 2026-09-09/09-10 entries above
  on more equal footing.

Also worth deliberately noting during this same test whether the
smooth-excursion-and-return shape (item 3, 2026-09-10 above) or the
sudden-locked-step shape (2026-09-09 original) recurs, and whether it
correlates with sine/AM-vs-two-tone mode as the bit-toggling theory above
would predict - both would be useful cross-checks to fold into the next
entry here.

## 2026-09-10 (cont'd) - regulator doesn't fix it; delay-mash reproducer captured, and the data KILLS the Core-1-serial-stall theory, pointing instead at the AD9851 SPI-clock/level-shifter margin

**External 5V regulator result:** reduced the general noise floor, did NOT
stop the frequency-shift/"stick" symptom. This substantially weakens the
USB-power theory (previous entry) as the cause of the jump/stick
specifically - the noise-floor improvement is real and separate, but the
actual symptom this thread is chasing survived a clean supply.

**New reproducer:** rapid repeated relative-delay keypresses (`'['`/`']'`/
`';'`/apostrophe) sometimes make the TX frequency change and then "stick" -
not settle back on its own even once the key-mashing stops. First real,
provokable trigger this whole investigation has had, versus "hands-off,
random, unpredictable."

**Bench test of the Core-1-serial-write-stall theory (previous entry):**
user ran 8 rapid fine-decrease presses (-1.92 -> -2.27 samples) followed by
~30 back-to-back on-demand (`'V'`) snapshots. Result: **theory does not
hold up.** Despite sustained, heavy USB-CDC TX buffer pressure the whole
time (`skip_total` climbed steadily from 27 to 459 across the capture -
`diag_room_for()`'s guard was firing constantly), `overruns` stayed 0 the
entire capture, `late_ticks_total` stayed 0, and wakeup jitter never
exceeded `max_gap_us=71` against a 62us nominal - nowhere near the
multi-ms stall the theory needed to disrupt an in-progress AD9851 write.
Canary (`carrier_hz`/`ftw_reciprocal`) stayed `OK` throughout. **Conclusion:
sustained Core-1 Serial/USB buffer congestion, even fairly severe, does not
measurably disrupt Core-0 dsp_task's real-time scheduling in this system** -
the unguarded-`serial_reply()` theory is not supported by this data and is
deprioritized as an explanation for THIS symptom (the unguarded-buffer-write
issue in `serial_commands.cpp` is still real on its own terms - see below -
just not confirmed as this symptom's cause).

**One genuine digital-signal event was captured:** `[dsp] max_freq_dev_step:
8562Hz (14193164 -> 14201726 Hz, at t=2186828ms)` - a real one-tick swing in
the ground-truth `tx_freq` value, computed before the SPI write even
happens. Cross-checking against the `[adc]` long-window-since-reset counter
(reading 51-54s across this same capture) places this event inside the
observed window, plausibly coincident with the delay-mashing. But every
snapshot immediately before and after shows `freq_dev`/`tx_freq` back at the
normal ~1198-1202Hz / 14201358-362Hz cluster - a single self-correcting
transient, not a locked value. Best-fit explanation: an ordinary (if
unusually large) near-envelope-null atan2/Hilbert transient - matches the
separately-logged `[dsp] freq_dev: max_unclamped=7991Hz (limit=20000Hz)
clip_count=0` reading elsewhere in the same capture, i.e. multi-kHz
transients near nulls aren't apparently rare in this signal, just usually
smaller. Consistent with dsp_task's already-established (2026-09-09)
per-tick-fresh-computation model - predicts exactly this spike-and-recover
shape, not a genuine stick, and doesn't need a corruption theory to explain
it.

**Important structural conclusion from this capture:** since the digital
`tx_freq` value shows no SUSTAINED anomaly anywhere in this window, if an
RF "stick" happened during this same test, the fault has to live downstream
of everything this firmware computes and sends over SPI - i.e. in the
AD9851 SPI/level-shifter/chip-latch chain itself, not in dsp_task's math,
its scheduling, or Core-1 Serial activity.

**This reopens an already-flagged, never-fully-verified risk:**
`carrier_output.cpp`'s own init comment documents bumping `spi_clock_hz`
from 2000000 to 4000000 "conservatively," through discrete BS170 inverting
level-shifter stages "whose real switching speed hasn't been characterized,"
citing `AD9851.h`'s own TIMING note that getting this wrong produces
"silently wrong output with no obvious symptom short of a spectrum
analyser" - and explicitly says to verify the transmitted frequency stays
exactly correct before pushing higher. That verification apparently never
got circled back on. A marginal W_CLK/FQ_UD edge through an uncharacterized
level-shifter stage occasionally failing to fully/reliably latch would be
completely invisible to every diagnostic this firmware can compute
(freq_dev, tx_freq, canary, weighted_bias all correct/clean - exactly what's
been observed across every occurrence logged this entire thread) while
still producing a real, possibly non-self-correcting, RF frequency error.
Fits the accumulated data better than any theory tried so far.

**Recommended next test, cheap and fully reversible:** revert
`carrier_output.cpp`'s `spi_clock_hz` from `4000000` back to `2000000` and
retry the same rapid delay-mash reproducer. If sticking stops or gets much
rarer at the lower, previously-safe clock rate, that's strong confirmation
of an SPI/level-shifter timing-margin cause - fix would be dropping the
clock back down permanently (or characterizing/upgrading the level-shifter
stage if the higher rate is wanted later). If it reproduces identically at
2MHz, that rules this out and points back toward genuine AD9851 PLL
quasi-stable-lock behavior (previous entry's theory) as the more likely
remaining explanation. Not yet run.

## 2026-09-10 (cont'd) - slew limiter confirmed OFF; every firmware-side candidate in the freq/phase chain is now eliminated by direct test - AD9851/PLL hardware hysteresis is the only theory left standing

Quick, decisive check: user confirmed the freq_dev slew-rate limiter
(`'{'`/`'}'`) is at `SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ` - off. Per
`ssb_dsp.c`'s own logic, at that sentinel the clamp branches never fire, so
`slew_limited_prev_freq_dev_hz` just tracks `freq_dev` exactly every tick -
a complete no-op. **The slew-limiter theory (previous entry) is dead.**

That closes out the last remaining candidate in the digital freq/phase
chain. Tallying everything eliminated by direct A/B test this session,
against the same "sticks after two-tone delay-sweeping" symptom:

- USB bus power -> external 5V regulator: noise floor improved, symptom
  unchanged. Weakened.
- SPI clock 2MHz vs 4MHz through the BS170 level-shifters: **no difference
  observed.** Eliminated.
- Generic "continuous FTW churn stresses the AD9851/SPI path" (independent
  of Hilbert/nulls) - tested via FMTEST, which drives a continuously,
  smoothly varying `freq_dev_hz` with zero DSP/Hilbert involvement:
  **no disruption seen on sine or FM.** Eliminated.
- Core-1 Serial-write stall disrupting dsp_task's real-time AD9851 write -
  tested via a rapid-keypress + heavy 'V'-spam capture: overruns/late-ticks/
  wakeup-jitter all stayed clean despite real, sustained USB-CDC buffer
  pressure. Eliminated.
- relative_delay.cpp's own two-ring implementation "getting out of step" -
  re-read specifically for this: both rings share one write index that
  dsp_task alone advances once per tick, and `freq_back`/`env_back` are
  both derived from the same single local `delay` read in the same
  function call - no code path exists for them to desync from each other.
  Eliminated as an implementation bug (though see below for what this
  question actually pointed at).
- freq_dev slew-rate limiter persistent state (this entry): confirmed off,
  therefore inert. Eliminated.

With those gone, **every stage in the digital freq/phase pipeline has now
been confirmed to recompute fresh every tick from its current inputs**,
same conclusion the 2026-09-09 "corrupted persistent value" audit reached
for `prev_phase`/the relative-delay ring/the AD9851 driver's own
full-frame-resend behavior, now extended to cover the slew limiter too (the
one stateful component that audit didn't check, and the one this thread's
"out of step" question productively surfaced - it just turned out to be
switched off). There is no remaining firmware-side mechanism in this chain
that could hold a WRONG value across multiple ticks once slew limiting is
ruled out - a genuine near-null atan2 transient (confirmed real, up to
8562Hz, previous entries) can only ever last one tick before every
downstream stage recomputes cleanly again.

**Conclusion: AD9851/PLL hardware hysteresis is now the only theory left
standing that isn't contradicted by a direct test.** Working picture: a
large near-null FTW step (the null-bias mechanism's known noisy-near-zero
atan2 behavior, amplified/relocated by whichever historical sample the
delay-line interpolation happens to be reading as delay is swept) kicks the
AD9851's internal 6x-multiplier PLL hard enough that it re-acquires to a
nearby but wrong lock point, which a subsequent numerically-correct FTW
write doesn't automatically clear (the PLL's own loop dynamics, not the
digital value sent, would govern whether/how it re-locks) - until another
large kick (further delay changes, which reshuffle where the interpolated
near-null noise lands) happens to knock it back toward the right point, or
doesn't. This fits "usually unsticks, not always" as well as anything
tried so far, and is now the leading explanation essentially by
elimination, not by direct confirmation.

**Decisive test, not yet run:** next time the RF output sticks during a
delay sweep, immediately pull an on-demand snapshot (`'V'`). If `freq_dev`/
`tx_freq` in that snapshot ALSO shows the stuck/wrong value, that resurrects
a firmware explanation somewhere not yet found (and would need
`max_freq_dev_step_hz`/canary cross-checked at the same instant). If
`freq_dev`/`tx_freq` reads back to normal (the expected ~1200Hz-ish cluster
for the loaded delay setting) while the RF output is still audibly/visibly
stuck at the wrong frequency, that's about as close to a direct confirmation
of the AD9851/PLL theory as this investigation can get without instrumenting
the chip's PLL lock/loop-filter node directly. Also worth trying as a
workaround probe: does a hard AD9851 reset (re-running `ad9851_init()`'s
full reset sequence, not just another `ad9851_set_frequency()` write) clear
a stuck state that repeated normal writes don't? If yes, that's strong,
practical confirmation this lives in the chip's internal load/PLL state,
and points toward a periodic self-healing hard-reinit as a pragmatic
mitigation independent of fully explaining the AD9851's internal behavior.

## 2026-09-10 (cont'd) - root cause found and fixed: the bit-bang transport was running unthrottled the whole time, `spi_clock_hz` was dead code under it

Two quick user answers settled this:

1. **The AD9851 has no built-in frequency rate limiter** - it's a pure
   phase-accumulator NCO (output = SYSCLK x FTW / 2^32, recomputed fresh
   from whatever FTW is currently latched), and the chip's only PLL is the
   fixed 6x REFCLK multiplier that generates SYSCLK from the reference
   oscillator - unrelated to the tuning word. The previous entry's "PLL
   settles to a nearby-but-wrong lock point" framing over-extended a
   generic PLL-hysteresis idea onto a chip that doesn't have a closed-loop
   output path at all - retracted.
2. User confirmed measuring the real bit-bang edge rate directly: **~7MHz
   unthrottled, ~4MHz measured as the safe limit** through this board's
   BS170 level shifters.

That sent a re-read of `AD9851.c`, which found the actual bug: **`AD9851_USE_BITBANG`
is `#define`d to 1** - the compiled transport is the raw-GPIO bit-bang
branch, not the hardware-SPI branch. `ad9851_init()`'s bit-bang branch never
read `cfg->spi_clock_hz` at all (only the dormant `#else` hardware-SPI
branch does) - so **the earlier "2MHz vs 4MHz, no difference" A/B test
(previous entries) was an unintentional null test.** Neither setting ever
reached the real signal path. The true edge rate was whatever raw
back-to-back `REG_WRITE()` GPIO toggles produce, with zero delay anywhere
in the loop - matching the user's own ~7MHz scope measurement.

This also gives a cleaner explanation for the two-tone-vs-FMTEST split than
the earlier "continuous churn" framing: it's not whether FTW changes every
tick (true for both), it's how ABRUPTLY. FMTEST sweeps smoothly, so
consecutive writes mostly flip a few low-order bits. Two-tone's near-null
atan2 noise (confirmed real, up to 8562Hz single-tick swings) can flip many
DATA bits at once, including many fresh 0-to-1 transitions - exactly the
demanding case for a BS170 inverting stage, whose LOW-to-HIGH edge is a
passive, pull-up-charged RC transition (much slower than the actively-
driven HIGH-to-LOW edge - see `AD9851_INVERTING_LEVEL_SHIFT`'s comment).

**Fix implemented, all in `AD9851.c`/`AD9851.h`/`carrier_output.cpp`:**
- New `ad9851_edge_delay()` (cycle-accurate busy-wait via
  `esp_cpu_get_cycle_count()` - `esp_rom_delay_us()`'s 1us granularity is
  two orders of magnitude too coarse for this, ~125ns half-period target).
- `cfg->spi_clock_hz` is now actually honored under the bit-bang transport
  too: `ad9851_init()` derives `half_period_cycles` from it
  (`esp_rom_get_cpu_ticks_per_us() * 1e6 / (2 * spi_clock_hz)` - 30 cycles
  / ~125ns at the existing 4000000 setting, which is already the user's
  measured-safe value, so no config number needed changing, just what it
  actually controls).
- `ad9851_set_frequency()`'s bit-bang loop now spends this delay in three
  places per transfer: after each DATA-line change (before W_CLK's
  sampling edge), after W_CLK's own falling call (the AD9851-side
  rising/sampling edge itself, before ending the pulse), and once after
  FQ_UD's final falling call (the actual latch edge).
- Updated `AD9851.h`'s doc comments (`spi_clock_hz` field + the
  `AD9851_USE_BITBANG` transport comment) so this dead-code trap can't
  silently reappear/mislead again.

**Not yet bench-verified, and one real risk flagged for the next test:**
this adds roughly 2 x 125ns x 40 bits = ~10us to the AD9851 write itself
(plus one more ~125ns at the very end) - `write_us` was already observed
around 25-31us in this session's captures, and total `max_busy_us` was
already sitting around 42-48us against the 62.5us tick budget (adc=4,
dsp=22 typical). **Adding ~10us risks pushing dsp_task into overrun** -
check `[timing] max_busy_us`/`overruns` immediately after reflashing. If
it's over budget, the cheapest lever is dropping one of the two per-bit
delay calls (accepting slightly less margin on one edge) or raising the
effective target somewhat (still well under the ~7MHz danger zone) - the
right trade-off needs the scope to confirm signal integrity is still fine
at whatever settles inside budget, which can't be verified from here.

**Next test:** reflash, confirm `overruns=0` still holds, then re-run the
exact two-tone delay-sweep reproducer (1-3 samples, sweep and hold) that
produced sticking before. If sticking stops or gets much rarer, this is
confirmed as the root cause. If it still happens identically, the DATA/
W_CLK/FQ_UD lines need to go on the scope directly to see what the real
edges look like now, since the theory would then need to be revisited.

## 2026-09-10 (cont'd) - CONFIRMED FIXED on real hardware, then trimmed back for a noise regression

**Confirmation:** user re-ran the exact reproducer (repeated relative-delay
key input while in two-tone) on the fixed firmware. Frequency swept
smoothly with the delay input as expected, and - the actual bug this whole
thread was chasing - **held exactly where it should the instant delay input
stopped, with no spontaneous jumping and no sticking away from the expected
value.** This is a clean, decisive real-hardware confirmation: the BS170
level-shifter edge-timing theory (bit-bang transport running unthrottled at
~7MHz through a level shifter only good for ~4MHz, `spi_clock_hz` silently
dead code the whole time under `AD9851_USE_BITBANG`) was the actual root
cause of the random TX-frequency jump/stick investigation that's run since
2026-09-09.

**Cost of the fix, and a real regression:** same test session reported
"very noisy" with diagnostics muted (`'v'` off, ruling out the already-known
45ms-print/20Hz mechanism). Diagnostics showed why: `spi_us` (the bit-bang
loop itself) went from 16-17us to 30us, pushing `max_busy_us` to 58 against
the 62.5us tick budget (adc=4 dsp=16 write=40) - only ~4us of headroom left,
and wakeup jitter crept up too (`max_gap_us` 71->78). Two plausible,
non-exclusive causes: (1) that shrunk real-time margin reopening this
project's own recurring cross-core-scheduling-jitter noise mechanism (the
`dac_task_enabled` regression, the ADC-ISR priority collision - same
family, different trigger); (2) the ~13-14us write-path slowdown is a
genuine, newly-introduced shift in group delay (~0.2 samples at 16kHz) that
the existing `relative_delay`/`gdeq` tuning was never calibrated against -
this thread has already shown fractional-sample misalignments this size
matter (group_delay_fit_notes.md's sub-0.1-sample compromise windows, the
0-50Hz shift over a 2-sample delay sweep).

**Trimmed same day:** the fix used two `ad9851_edge_delay()` calls per bit
(settle DATA before W_CLK's sampling edge, then settle the sampling edge
itself before ending the pulse) plus one after FQ_UD's final edge - belt-
and-suspenders. Removed the second per-bit one (`AD9851.c`,
`ad9851_set_frequency()`'s bit-bang loop) - the DATA-settle delay is the
load-bearing one (it's what stops the chip sampling a still-transitioning
bit); the sampling-edge-settle-before-ending-the-pulse one was the extra
margin. Should roughly halve the added cost (back toward spi_us~23-24us,
max_busy_us~51-52us, restoring ~6-7us of headroom) while keeping the
confirmed fix intact. FQ_UD's own final-edge delay (the actual latch, only
once per transfer - cheap) was left in place.

**Not yet re-verified on the bench.** Next test: reflash, re-check
`[timing] max_busy_us`/`overruns`/`spi_us` (expect meaningfully lower than
58/40/30 but still elevated vs. the original 25/16-17 baseline), re-run the
same delay-sweep reproducer to confirm sticking is STILL gone with only one
per-bit delay, and separately assess whether the reported noise increase
has resolved. If sticking reappears with only one delay, that means both
per-bit edges genuinely needed protection and the second delay should be
restored (accepting the tighter margin) rather than left out.

**2026-09-11: trim re-verified on the bench, then reverted pending a clean comparison.**
Reflashed with only the DATA-settle delay (second per-bit delay removed,
FQ_UD final-edge delay kept). `[timing]` came back as hoped:
`max_busy_us=46` (`adc=5 dsp=16 write=29`), `spi_us=20`, `overruns=0`,
`late_ticks_total=0`, `max_gap_us=71` - margin restored to ~16us against
the 62.5us budget, wakeup jitter back at the pre-fix 71us baseline (not the
elevated 78us seen with both delays in). So the margin regression from the
day before is resolved by the trim, as expected.

But the same capture also caught a real hands-off TX jump: delay control
untouched (not the active-sweep reproducer), `[dsp] max_freq_dev_step:
10387Hz (14192168 -> 14202555 Hz, at t=308407ms)` - a single-tick
transient, same family as the 8562Hz/8199Hz ones already attributed to
ordinary near-null atan2 noise (`freq_dev: max_unclamped=7994Hz,
clip_count=0` in the same capture; canary OK throughout; `freq_dev`/
`tx_freq` nominal in every other snapshot). The digital math computed
correctly and the receiver still saw a real shift - the same pattern every
jump in this thread has shown, before and after the SPI fix.

This matters for the trim's coverage specifically because the near-null
atan2 noise that produces these swings happens every cycle on its own, not
only when the delay-sweep reproducer is actively provoking it - so the
same "abrupt FTW step outruns the BS170's slow settling edge" mechanism the
fix targets could still fire spontaneously during ordinary two-tone
playback, just less often than under active sweeping. A single hands-off
capture can't tell whether (a) dropping the second per-bit delay reopened
part of the gap the fix closed, or (b) this is a separate mechanism that
was always going to survive the SPI fix regardless of which delays are in
place.

**Decision: restored the second per-bit delay** (`AD9851.c`,
`ad9851_set_frequency()`'s bit-bang loop - see that call site's comment for
the full reasoning) rather than leave the ambiguity open, since it's the
direct way to separate the two explanations and the cost (margin back down
toward the 2026-09-10 46(?)->~58us figures) is fully reversible. Plan:
run a similarly long hands-off two-tone stretch on this build.
  - If hands-off jumps stop with both delays restored: confirms both edges
    genuinely needed protection, the trim was premature, and the tighter
    margin is the real price of the fix (leave both delays in going
    forward).
  - If hands-off jumps still occur with both delays restored: strong
    evidence this specific jump is a separate mechanism the SPI fix was
    never going to touch - the trim can go back in (it wasn't the problem),
    and the -45Hz-class hands-off jump needs its own investigation
    (candidate next step: check whether it correlates with anything at
    t=308407ms - ADC/DSP task timing, a diagnostics print, cross-core
    activity - the same way the `dac_task_enabled`/ADC-ISR-priority jitter
    mechanisms were found).

Not yet re-verified on the bench with both delays back in - that's the
next test, alongside confirming `max_busy_us` lands back near the
2026-09-10 post-fix figures (~58us) rather than something worse.

**2026-09-11, later same day: user pushback on the hardware framing - reports the hands-off jump "moves with delay" and is worse near the optimum-IMD compromise window, and suspects overload-induced data corruption rather than an external/hardware cause. Investigation below finds two separate, unrelated explanations - one confirms a real diagnostic red herring, the other identifies the jump itself as an already-diagnosed, already-understood DSP/physics effect, not corruption.**

**(1) The `[adc] fifo drop_total` growth (millions of drops, `min=4294967295 max=0` frozen across every snapshot) is a dead/misleading diagnostic in TWO-TONE mode, not evidence of real data loss on the signal path being tested.** Traced via `adc_capture.cpp`/`ssb_mic_test.ino`: `adc_capture_read_next_sample()` - the only code that drains `s_adc_fifo_tail` and updates `s_dbg_adc_fifo_min_available`/`max_available` - is only called in the `else` branch of `ssb_mic_test.ino`'s audio-source dispatch (line ~550), which `AUDIO_SRC_TWOTONE` never reaches (it calls `generate_twotone_sample()` instead, line 541). Meanwhile `adc_conv_done_cb()` (the ISR) keeps pushing real mic-input samples into `s_adc_fifo` at ~80kHz regardless of mode - by design, per that file's own comment, since the ADC runs unconditionally. With nothing ever draining the tail, the FIFO fills within milliseconds of boot and from then on nearly every subsequent ISR sample hits the "FIFO full" branch and increments `s_dbg_adc_fifo_drop_count` (confirmed by the growth rate across this capture: ~78.6k drops/sec, essentially the full 80kHz ADC rate) - and `min_available`/`max_available` stay frozen at their init sentinels forever because the function that updates them is never called. So this large, alarming-looking number is just the unused mic path silently discarding a mic signal nobody is reading while two-tone testing runs - it says nothing about the integrity of the actual two-tone signal being measured. Real, but harmless in this mode; worth fixing only because it's actively misleading (this is what triggered the "data corruption" suspicion this round) - e.g. skip incrementing/printing it when the audio source isn't a mic-driven one, or gate `adc_conv_done_cb` itself off outside mic modes. Not fixed yet - flagging for a decision, not applying unrequested.

**(2) The jump itself (10349-10387Hz single-tick `tx_freq` steps, delay-position-sensitive, worse near the optimum-IMD window) matches this project's own already-completed EER/polar-transmitter null-crossing investigation from 2026-09-01 through 09-04 - it is very likely NOT new corruption, and NOT something the AD9851/BS170 edge-timing fix could ever touch.** Cross-referencing `group_delay_fit_notes.md` and `ssb_mic_test_commands.md`'s "Is `eq`'s IMD benefit the highpass or the presence boost?" section:

  - With equal-amplitude two-tone (`eq` off, tone ratio 0dB), the analytic-signal trajectory passes exactly through the origin at every destructive-interference null. That produces a genuine derivative discontinuity in `freq_dev` - not noise, not a bug - modeled at the time as a ~9640Hz worst-case single-sample swing for this project's 700/1900Hz pair (real bench measurement then showed a sign-flip: "+1300Hz either side, -6700Hz right at the null"). The two values captured this session (10349Hz, 10387Hz) are the same order of magnitude and same signature (one-tick, digitally-correct-but-large, canary OK, `clip_count=0` since it's well under `MAX_FREQ_DEV_HZ`=20000). This is the textbook Kahn/EER "bandwidth expansion when the trajectory passes near the constellation origin" effect (Zhuang/Waheed/Staszewski, IEEE TCAS-I 2010) - already researched and cited in this project's own notes on 2026-09-03, not a new mechanism.
  - **Already bench-confirmed fix exists and was NOT re-enabled for this session as far as the pasted capture shows**: `eq` (`'e'`) or the dedicated `'R'` tone-amplitude-ratio control (even ~1dB of tone mismatch) moves the trajectory off the true origin entirely, eliminating the sign-flip/large-swing condition at the source - confirmed on real hardware 2026-09-04 ("that proves the hypothesis... very close to `eq` on"). This is a DSP-domain fix for a DSP-domain (well, physics-domain) effect - unrelated to `AD9851.c`'s BS170 edge-settle delay, which only concerns how faithfully a given commanded FTW step reaches the chip, not whether that step should be small in the first place.
  - **Already investigated and explicitly REJECTED for this specific problem: tightening `freq_dev_slew_limit_hz`.** The 2026-09-01 entry in `group_delay_fit_notes.md` found tightening the slew limiter made things monotonically WORSE (8000Hz worse -> 12000 better -> 20000 better still, trend toward fully unlimited) because it just clips a legitimate large excursion rather than preventing the trajectory from needing one - confirmed every preset in `settings.h` currently sets `freq_dev_slew_limit_hz = SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ`, which now reads as the deliberate conclusion of that investigation, not an oversight. Do NOT reach for the slew limiter as a fix for this round's jump.
  - **The delay-position sensitivity the user is now reporting is also already-documented, not new**: the 2026-09-01 entry notes "which discrete sample lands nearest the true continuous-time null is alignment-dependent... the delay setting shifts exactly which sample lands closest" - i.e. moving `relative_delay` changes exactly how close some sample gets to the singularity, hence how large the worst-case one-tick swing is at that setting. This is consistent with, and likely fully explains, "more sensitive at particular delays especially where the optimum IMDs are."

**Net read:** the AD9851 second-delay restoration (this file's entry above) is still the right test for the SPI-write "sticking" symptom it targets, but is not expected to touch this specific jump - this jump looks like a separate, already-understood, already-solved-on-the-bench DSP/physics effect (true envelope null under equal-amplitude two-tone), not overload-induced corruption and not something more edge-settle margin can fix. **Suggested next step, cheaper than the long hands-off SPI comparison run**: check whether `eq` or `'R'` (tone ratio) was engaged for this session's capture; if both were off/equal, re-run with `'e'` on or `'R'` at +/-1-3dB and see whether the `max_freq_dev_step` events (and the "worse near the IMD-optimum delay" pattern) disappear - that would confirm this is the known null-crossing effect and not a new one, and separates it cleanly from the still-open hands-off-jump question the AD9851 comparison run is trying to answer.

**2026-09-11, later still: reverted bit-bang to fully unthrottled (original high-speed) pending a proper re-scope of the drive signals.** User realized the original "4MHz measured as the safe upper limit" bench conclusion (2026-09-10) may not have actually verified the toggle RATE - the scope session was focused on rise-time shape and didn't separately confirm the signal being measured was really throttled to 4MHz rather than the original unthrottled ~7MHz-equivalent rate. Since that measurement is the sole evidentiary basis for every `ad9851_edge_delay()` call this thread has added/removed/restored/trimmed since, it needs re-taking with both rate and rise time checked together before any of that throttling work can be trusted. Reverted via a new `AD9851_BITBANG_EDGE_DELAY_ENABLED` flag (`AD9851.c`, set to `0`) guarding all three call sites (DATA-settle, W_CLK-sampling-edge, FQ_UD-latch) rather than deleting them - a one-line flip back to `1` restores exactly the 2026-09-10/11 state (with both per-bit delays, per the most recent restore) once the re-scope is done, without re-deriving anything. `half_period_cycles` computation in `ad9851_init()` is left in place (harmless, cheap, one-time) so re-enabling needs no other change. **Next bench step: re-scope DATA/W_CLK/FQ_UD at this genuinely-unthrottled rate, this time explicitly confirming the real toggle rate on the scope (not just rise-time shape) before judging whether it's clean.** If it turns out the original "safe at 4MHz" rise-time measurement really was taken on the unthrottled signal all along, that would mean the unthrottled rate was fine on this hardware from the start, and the whole edge-delay mechanism (and its real busy_us/margin cost) may never have been needed - to be confirmed, not assumed, by the re-scope.

**2026-09-11, later still: clarifying two different "20k"/"slew limit" knobs the user asked about, since they're easy to conflate and have opposite tuning history.** There are two independent mechanisms in `ssb_dsp.c`, both are Hz-denominated, and only one is currently near "20k":
  - **`max_freq_dev_hz` (the magnitude CLAMP, `config.h:405` = `MAX_FREQ_DEV_HZ` = `20000.0f`)** - hard-clips `|freq_dev|` after everything else runs. Compile-time only (set once in `ssb_dsp_init()` from `cfg->max_freq_dev_hz`; no live serial command touches it - would need a rebuild+reflash to test a different value). **This is very unlikely to be involved in the current jumps**: both captured events this session (max_unclamped=7994-7997Hz, max_freq_dev_step=10349-10387Hz) sit under 20000Hz, and the log's own `clip_count=0` confirms the clamp never actually engaged during this capture. Lowering it wouldn't currently change anything; it isn't "causing" what's being seen.
  - **`freq_dev_slew_limit_hz` (the RATE limiter)** - limits how fast `freq_dev` can change tick-to-tick, live-adjustable via `'{'`/`'}'` (`ssb_dsp_lower_/raise_freq_dev_slew_limit()`). Every preset in `settings.h` sets this to `SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ` (effectively off) - and per the 2026-09-01 entry in `group_delay_fit_notes.md` (cross-referenced in this file's entry above), that's the deliberate result of prior bench testing that found TIGHTENING this made two-tone IMD monotonically WORSE (8000Hz worse -> 12000 better -> 20000 better still -> trend toward fully unlimited) because it clips a legitimate large excursion instead of preventing the trajectory from needing one. If "test the Hz slew limit" means this one: it's currently off, testing it means turning it ON (`'{'` a few times, starts at 2000Hz per `FREQ_DEV_SLEW_START_HZ`), and prior data already predicts it'll trade a smaller `max_freq_dev_step` for worse two-tone IMD - worth confirming directly against THIS specific jump rather than assuming the 2026-09-01 result transfers unchanged, but going in with that expectation already set.
  Which one the user meant when they said "currently at 20k" wasn't confirmed - the clamp is the only one actually AT 20k right now, so that's the more literal read, but flagged both since the fix history differs completely between them.

**2026-09-11, later still: added a `[dsp] post-step trace` diagnostic to directly answer "does it recover automatically, or stick?" instead of arguing it from first principles.** User's objection, and a good one: if this is a genuine one-tick DSP transient (the EER null-crossing theory above), `freq_dev` is recomputed fresh from `atan2(Q,I)` every single sample with no persistent memory (the one thing that WOULD carry state tick-to-tick, the slew limiter, is currently off per the entry above) - so it should recover within one 62.5us tick, not stay shifted. The existing `max_freq_dev_step` high-water-mark can't actually show this either way - it only ever records the two values straddling the single worst step, nothing about what happens afterward. Added a small ring capture (`diagnostics.cpp`, `FREQ_STEP_TRACE_LEN=8`) that arms on every new record-breaking step and records that tick's (post-jump) `tx_freq` plus the next 7 ticks, printed as `[dsp]   post-step trace: <8 values>` in the periodic block once filled. **This will show directly, on the next bench run, whether tx_freq snaps back within a tick or two (supports the EER-null-transient read) or stays elevated for many ticks (would instead point at something persistent - a latched/corrupted value, a stuck delay-line entry, or the PWM/envelope path desyncing from the phase path, all raised as live alternatives this round and none yet ruled out beyond the existing carrier_hz/ftw_reciprocal canary, which has stayed clean across every jump seen so far).** Note this trace is independent of the AD9851 bit-bang revert above - it captures at the DSP/`ad9851_set_frequency()`-call boundary regardless of transport timing, so it'll work identically whether or not the edge-delay flag is re-enabled later.

**Open, not yet answered:** whether `'e'`/`'R'` (tone-ratio) were engaged for the session that produced the pasted capture - still the fastest way to test the EER-null-crossing explanation directly (see this file's entry above). Also still open: the PWM/envelope-path desync possibility the user raised - not yet investigated at all; if the post-step trace shows a genuine "stick" rather than a recover, that becomes the next thing to check (envelope_output.cpp/envelope_interp.cpp's own tick bookkeeping against dsp_task's, looking for the same class of cross-context assumption issue already fixed once in `ssb_mic_test.ino`'s `audio_source_t src` single-read pattern).

**2026-09-11, later still: unthrottled bit-bang confirmed on the bench (spi_us=16, matches the original pre-2026-09-10 baseline exactly) - and the jump still occurs at full speed, with no edge delays active at all.** New capture: `spi_us=16` (`prep_us=2`, `prep+spi=18` vs `write_us=27`) - matches this project's own pre-throttling baseline ("spi_us 16-17us") almost exactly, confirming `AD9851_BITBANG_EDGE_DELAY_ENABLED=0` is doing what it's supposed to and the drive signals are now genuinely running at the original ~7MHz-equivalent unthrottled rate again, ready for the re-scope. A `max_freq_dev_step: 8003Hz (14193359 -> 14201362 Hz, at t=98071ms)` event was captured in this same run - i.e. **the jump still happens with the AD9851 driver back to its original, long-proven-fine (rise-time-wise, at whatever rate it actually was) unthrottled form, no `ad9851_edge_delay()` calls anywhere in the transfer.** This is independent, stronger evidence for this file's earlier read: this specific jump-type symptom lives entirely upstream of the AD9851 transport (confirmed now under BOTH the throttled and fully unthrottled transport), not in BS170 edge settling - consistent with the EER/polar-transmitter null-crossing explanation, not the bit-bang timing theory. `gdeq=off` in this capture (was `ON` previously) - per `ssb_mic_test_commands.md`'s 2026-09-07 entry ("`g` makes almost no difference" to the related AM-to-PM effect) this isn't expected to matter for this specific mechanism, noted for completeness only.

**2026-09-11, later still: the new `[dsp] post-step trace` line never printed in that capture - self-inflicted repeat of a lesson already documented in this exact file.** Root cause: it requested `diag_room_for(200)`, but `diag_room_for()`'s own header comment (real hardware measurement, already on record in `diagnostics.cpp`) says `Serial.availableForWrite()` maxes out around ~162 bytes even at this board's fully-drained resting state - a 200-byte request can structurally never succeed, the same failure this project already diagnosed once for the old all-or-nothing block guard. Confirmed on the bench: `max_freq_dev_step` (a 140-byte request) printed repeatedly in the same window the new line never appeared in once. Fixed: shortened the line and dropped the request to 130 bytes (worst case ~93 bytes actual), matching the sizing convention every sibling line in this file already uses. **Not yet re-verified - needs a re-flash and another capture to confirm it actually prints this time and to finally get real data on the "does it recover" question**, which is still open.

**2026-09-11, later still: "can an IIR filter's state get corrupted forever?" - yes structurally, no NaN/Inf guard exists anywhere in this DSP chain, and checking it turned up one real (if narrow) bug, now fixed.** Good question, and a genuinely different risk class from the freq_dev/atan2 computation discussed earlier in this file: that computation is memoryless (recomputed fresh from `atan2(Q,I)` every sample, so a bad tick can't outlive itself), but IIR filters (`biquad_process`/`ssb_allpass1_process`/`ssb_shelf_biquad_process` in `ssb_dsp.c`, `compressor_process`'s envelope follower, `ssb_adc_filter.c`'s ADC anti-alias biquads) all carry `x1/x2/y1/y2`-style feedback state that references its OWN past output - a bad value there doesn't get flushed by fresh input the way an FIR tap or a fresh atan2 call does; the feedback formula keeps citing it every subsequent sample. `flush_denorm()` (applied throughout) only catches near-zero subnormals, not NaN/Inf/absurdly-large-but-finite values - grepped the whole project, no `isnan`/`isinf` check exists anywhere in this DSP chain. So IF a bad (non-decaying) value ever got into one of these states by some means, it could genuinely persist indefinitely, unlike the phase/frequency math.

  - **Checked whether the actual null-crossing event could organically seed one**: `fast_atan2()`'s `x==0.0f && y==0.0f` case is explicitly handled (returns 0.0f, no division), and every other branch only ever divides `ay/ax` or `ax/ay` where the divisor's branch condition (`ax>=ay`) guarantees it's the larger of the two - no way to reach a 0/0 or divide-by-zero from this function. `fast_sqrt()` explicitly returns 0 for `x<=0`. So no NaN path was found from the specific null-crossing event this thread has been chasing - the "digital math computed correctly" read from earlier in this file still stands for that specific mechanism.
  - **Found instead, while checking this: `ssb_dsp_set_eq_enabled()` (`ssb_dsp.c`) didn't reset `eq_hpf`/`eq_presence`'s biquad state on re-enable**, unlike `ssb_dsp_set_compressor_enabled()` right next to it (which explicitly zeroes `comp.env` on re-enable, with its own comment explaining exactly why: "so it doesn't resume from a stale value... avoids a jump/thump") and unlike `envelope_gdeq_set_enabled()`'s identical off->on reset. Toggling `'e'` off then back on was feeding whatever `x1/x2/y1/y2` happened to be frozen at back into the very next sample - a real, demonstrable "resume from stale state" bug, exactly the shape of thing being asked about. **Fixed** to mirror the compressor's own established pattern. Not "forever" in practice, though - these are RBJ-cookbook biquads with normal (BIBO-stable) coefficients, so a stale-state transient decays within a few dozen samples, not indefinitely; the real-world symptom would be a brief click/thump right at the `'e'` toggle instant, not an ongoing corruption during steady playback.
  - **Net read**: this fixed bug only fires on an `'e'` toggle, so it's unlikely to be the direct cause of the spontaneous mid-run jumps this thread has been chasing (those aren't reported as coinciding with a toggle) - worth asking whether `'e'` was being toggled during any of the captures that showed a jump, just to rule it out. But it's a real bug regardless, worth having fixed, and it validates the general concern: this DSP chain genuinely has no defense against a persistent-state corruption once introduced, anywhere feedback state exists. `ssb_adc_filter.c`'s ADC LPF and `envelope_gdeq.cpp`'s allpass both already reset correctly on their own mode-transition paths (checked directly) - `eq_enable` was the one inconsistent case.
  - **Not yet done**: an actual live canary for this (e.g., an `isnan`/`isinf` check on `eq_hpf.y1`/`eq_presence.y1`/the gdeq allpass state/`comp.env`, printed the same way `ftw_reciprocal`'s canary is) - would settle "is this happening right now" empirically instead of by code inspection, same evidence-first approach as everything else in this file. Offered, not yet built - build it if the post-step trace doesn't cleanly explain the jump once that's re-tested.

**2026-09-11, later still: added the isnan/isinf canary for every IIR-style persistent filter state in this project, following the same latch pattern as the existing `carrier_hz`/`ftw_reciprocal` canary.** User confirmed `eq` wasn't in use for current testing (so the fixed `ssb_dsp_set_eq_enabled()` bug isn't the direct cause of the jumps being chased) but asked for the general test anyway - good call, since the underlying "can a filter's own feedback memory get stuck" question is independent of which stage happens to be enabled right now. Implemented via `isfinite()` (the standard single-call equivalent of `!isnan(x) && !isinf(x)` - false for either, true otherwise) on each stage's fed-back state:

  - `ssb_dsp.c`/`.h`: new `ssb_dsp_get_iir_canary()` checks `eq_hpf`/`eq_presence` (`y1`/`y2`) and the compressor's `comp.env`.
  - `envelope_gdeq.cpp`/`.h`: new `envelope_gdeq_get_canary()` checks both allpass sections' `y1`.
  - `envelope_ampeq.cpp`/`.h`: new `envelope_ampeq_get_canary()` checks both shelf biquads' `y1`/`y2`.
  - `adc_capture.cpp`/`adc_capture.h`: new `adc_capture_get_lpf_canary()` checks both the Butterworth and Chebyshev cascades' `z1`/`z2` (both stages) - not exercised by two-tone/synthetic modes (confirmed earlier this session that `adc_capture_read_next_sample()`, the only code that runs these filters, isn't called for those sources), but included for completeness/future mic-mode use.
  - `diagnostics.cpp`: wired all four into `canary_check_background()` (checked every tick unconditionally, latches into one of five new `s_dbg_canary_{eq,comp,gdeq,ampeq,adclpf}_bad_since_ms` fields the FIRST time any relevant stage goes non-finite, prints a `MISMATCH` line only on that transition - same silent-while-healthy design as the existing two canaries) and `canary_print_status()` (the `'v'`/on-demand path - prints one compact `[canary] iir_state: OK (eq/comp/gdeq/ampeq/adc_lpf)` line when everything's finite, expanding into per-module detail lines only when something isn't). Sized each new print line to 90-130 bytes, well under the ~162-byte ceiling `diag_room_for()`'s own header comment documents for this board - learned that lesson the hard way earlier today with the post-step trace line, not repeating it here. Latches are NOT reset by `diagnostics_reset()`/`'r'`, same reasoning as the existing two: a rare corruption event shouldn't silently vanish just because someone started a fresh measurement window.

**Not yet re-verified on the bench** - needs a reflash. Once it's running: a steady `[canary] iir_state: OK` (or the two individual OK lines it now sits alongside) across a long hands-off run would mean no IIR state has gone bad by any means checked so far, keeping the "IIR corruption" theory open only as a not-yet-observed possibility rather than a confirmed mechanism; any `MISMATCH` line would be a genuinely new, actionable data point - note its `first seen at t=...ms` against the `[dsp] max_freq_dev_step`/post-step-trace timestamps to see whether the two ever correlate.

**2026-09-11, later still: first real bench data from the reflashed build - ~4.7 hours unattended two-tone run, good news across the board.** The one `max_freq_dev_step` event on record (10401Hz at t=1076763ms, ~18 minutes after boot) never recurred for the remaining ~4.5+ hours - the high-water-mark register simply never updated again. The now-working post-step trace shows it recovering to steady state (14202562 -> 14201359 -> ... settling to +/-4Hz) within exactly one tick and staying there. Both canary families (`carrier_hz`/`ftw_reciprocal` and the new `iir_state`) read `OK` on every single snapshot across the whole run - no NaN/Inf, no carrier/FTW divergence, ever. Timing stayed rock solid the entire time: `overruns=0`, `late_ticks_total=0`, `max_busy_us=46` against a 62us budget throughout, `wakeup jitter` pinned at a steady `max_gap_us=70` (not growing). `spi_us=20` confirms the earlier full-speed-bit-bang revert is in effect and still well inside budget. Net read: a single-tick, fully-recovering, never-repeating excursion with every corruption canary clean the whole time is much better explained by the already-documented EER null-crossing physics than by an ongoing corruption mechanism - nothing in this run contradicts that theory, and several things (the clean canaries especially) argue against the "corrupted persistent value" theory from the 2026-09-09 entry above.

  - **Correction to this file's own dac_task_enabled note above**: re-checked and `dac_task_enabled` is confirmed `0` in the actual `config.h` on this build (not just "should be" - directly confirmed both by source and by live data: `dac_code` read exactly `0` on every single snapshot across the whole 4.7-hour run despite envelope moving between 0.663-0.664, which is only possible if `dac_task` genuinely never ran to pick up a value). Closes out the "re-confirm `dac_task_enabled=0` on the actual hardware build in use" item from the 2026-09-09 entry above with hard evidence, not just a source read.
  - New-to-this-run diagnostic output, not previously called out in this file: `null_bias`/`null_bias2`/`null_bias3` (from `null_bias_investigation.md`) show a stable `weighted_bias` of +73.24Hz for the active 700/1700Hz pair, and the user separately confirmed the SDR has read "within a few Hz" whenever they've checked - consistent with `null_bias_investigation.md`'s own confirmed-accurate `weighted_bias` predictor.

**2026-09-11, later still: added a test-tone dither ('Q', test_signals.cpp/.h) to try fixing the null-bias measurement artifact, in response to a direct question about dithering - untested.** Two different "null" problems exist in this project's notes and it's important not to conflate them: `group_delay_fit_notes.md` (2026-09-03) researched dithering for the OTHER one (the EER/polar "theoretically infinite phase bandwidth at the origin" problem) and found no literature precedent for it there - the established fix is an upstream I/Q trajectory reshape instead. This addition targets `null_bias_investigation.md`'s problem instead: that file's "Root mechanism identified" section already explains the bias is coherent specifically because the two-tone test tones are exact phase-accumulator multiples of `SAMPLE_RATE_HZ`, so every null in a run lands at an identical sample-grid position and whatever tiny bias one null produces, every null produces identically - a structurally different situation from the EER problem, and one that's a much more natural fit for dithering (same class of fix as breaking a limit-cycle/idle-tone with ADC dither).

  - **What it does**: `Q` toggles a small (+/-`TWOTONE_DITHER_MAX_HZ`, 0.5Hz default) frequency offset on tone2 only (tone1 stays exactly at nominal as an undithered reference), redrawn from `esp_random()` every `TWOTONE_DITHER_UPDATE_HZ` (4Hz default) and linearly ramped toward sample-by-sample in between so the instantaneous frequency varies continuously rather than in steps. Off by default (`TWOTONE_DITHER_ENABLED=0`, `config.h`) - `t`/`T`/`R` behave exactly as before unless `Q` is pressed. New config constants, `test_signals_get/set_twotone_dither_enabled()`, and the `'Q'` handler (mirrors `'T'`/`'R'`'s "switches into two-tone mode too" convention) are the only touch points - `ssb_dsp.c` and the rest of the real signal chain are completely untouched.
  - **Not yet compiled or bench-tested.** `esp_random()`/`<esp_random.h>` usage specifically needs verifying against the installed Arduino-ESP32 core - same "NOT COMPILER-VERIFIED, no toolchain available in this environment" caveat this whole project already carries.
  - **Validation plan** (documented in `null_bias_investigation.md` and `test_signals.h`): compare `[dsp] null_bias2` (`weighted_bias`) across several `r` resets on the same tone-pair preset, `Q` off vs. on, with a LONG dwell before reading each time - the 2026-09-09 entry above already found `weighted_bias` drifts for tens of seconds to minutes after a reset even with nothing changed, so a quick point-read would just compare noise to noise, not the effect of `Q`.

**2026-09-11, later still: first bench data with `Q` ON, +/-0.5Hz, 700/1900Hz pair - "a noisy board peak +/- about 6Hz from zero", compared against this file's own 700/1900 baseline (`null_bias_investigation.md`'s confirmed-measurement table: `weighted_bias` -20.72Hz, SDR ~8Hz, dither off).** `esp_random()`/`<esp_random.h>` compiled and ran without incident - first real confirmation the "not compiler-verified" caveat above wasn't hiding a build problem. Read as a real-time board display bouncing roughly symmetrically around 0Hz with about a 6Hz peak excursion, rather than settling on a fixed off-zero constant the way the undithered case does. Timing/canaries stayed clean through this capture (`overruns=0`, both canary families `OK`); the single `max_freq_dev_step` event on record recovered within one tick per the post-step trace, consistent with every prior capture. Read this cautiously, not as a clean isolated A/B: the tone pair was already sitting on 700/1900 before and after, but this was the first capture taken right after reflashing the `Q` build, so settle-time and any residual state from the previous session weren't independently controlled for. The magnitude (~6Hz peak) is in the same ballpark as the undithered baseline's ~8Hz SDR reading, not obviously smaller - consistent with the classic dither signature of trading a coherent, repeatable bias for a randomized spread of similar overall magnitude, rather than a magnitude reduction. Whether that's a net win depends on what the downstream use of the reading cares about (a fluctuating-but-zero-centered read vs. a stable-but-offset one) - not yet assessed.

**2026-09-11, later still: dither reduced to +/-0.05Hz (`TWOTONE_DITHER_MAX_HZ`, config.h), still 700/1900Hz pair - "continuous shifting -10 -> +3Hz" observed on the board, compared against the +/-0.5Hz capture immediately above.** `weighted_bias` itself barely moved between the two captures (-16.4Hz at 0.5Hz dither vs. -17.4Hz at 0.05Hz dither, essentially flat) despite the dither amplitude changing by 10x - if dithering worked by smoothly smearing the null bias in proportion to how far it pushes tone2 off-grid, the averaged bias should have shrunk toward the undithered baseline as amplitude shrank, and it didn't. The real-time observed spread also stayed roughly the same overall size at both amplitudes (~12-13Hz peak-to-peak at 0.05Hz dither vs. ~12Hz at 0.5Hz dither) - same rough magnitude of swing 10x apart in dither depth is the signature of a threshold/discontinuity effect, not a smoothly graded one: once dither is nonzero at all, it looks sufficient to occasionally knock a null onto the other side of whatever discrete sample-grid boundary drives the coherent bias in the first place (per "Root mechanism identified" in `null_bias_investigation.md`), producing a close-to-full-scale jump regardless of how small the nudge was. Timing/canaries clean throughout (`overruns=0`); the one `max_freq_dev_step` event on record again recovered within a tick. **Open discrepancy, not yet resolved**: `weighted_bias`'s -17.4Hz average sits well outside the user's reported real-time range (-10 to +3Hz, midpoint ~-3.5Hz) - two live hypotheses: (1) `weighted_bias` and whatever instrument produces "the board" reading are different statistics of the same signal (different averaging/settling behavior) and simply don't have to agree numerically; (2) the continuous re-dithering (a fresh random target every 250ms at `TWOTONE_DITHER_UPDATE_HZ=4.0`) may be preventing that instrument from ever reaching steady state, making some or all of the "continuous shifting" a measurement-methodology artifact rather than a property of the RF signal itself. Asked the user what "the board" is and how it derives its reading, and proposed a follow-up (slow `TWOTONE_DITHER_UPDATE_HZ` way down, e.g. to 0.2Hz, at small amplitude, to see whether the board's reading can settle between updates) to discriminate between the two - not yet run.

**2026-09-12: "the board" identified (SDRUno "Aux SP" FFT panadapter, 0.18Hz/bin, RX ref accurate to +/-1Hz) — resolves most of the `weighted_bias`-vs-observed-range discrepancy from the two entries above via ordinary integration-time/sampling-variance math, no instrument-settling mechanism needed.** Full derivation in `null_bias_investigation.md`'s "2026-09-12" entry. Short version: 0.18Hz/bin implies each Aux SP snapshot integrates roughly 5-10s of signal (~20-40 independent `Q` dither realizations, since a fresh target is drawn every 250ms per `TWOTONE_DITHER_UPDATE_HZ=4.0`), while `weighted_bias` (`env2_sum`/`env2_dphi_sum`, `ssb_dsp.c`) is a plain lifetime accumulator since the last `'r'` reset - by ~700s into a run it's averaged across ~2800 realizations, 70-130x more than one Aux SP snapshot. Given the already-established threshold-like (not smoothly graded) sensitivity of this effect, a small-N snapshot is expected to have much higher variance than the large-N firmware average even though both estimate the same underlying mean - ordinary sampling variance, not a sign either measurement is wrong. Cleanest next test (not yet run): toggle `Q` OFF and watch Aux SP on the same pair to see whether it's already scattering by several Hz snapshot-to-snapshot for unrelated reasons, or only does so with dither on.

**2026-09-12, later same day: correction to the "Aux SP identified" entry above (it's exponential-averaging, not a fixed-window FFT snapshot), plus two new hardware facts and a direct answer on whether dither should have centered the frequency on zero.** User corrected the earlier fixed-window/independent-snapshot model: Aux SP actually uses exponential averaging, which has a fixed noise floor that never shrinks with more observation time (unlike `weighted_bias`'s plain lifetime accumulator, whose variance keeps shrinking the longer it runs) - this still predicts persistent, non-settling scatter on Aux SP, just via a different mechanism than originally described. Two new facts: the user's own visual read of the moving trace is only reliable to about +/-5Hz (cursor-based static reads are much better); and the AD9851's reference is a plain 30MHz XO that shifts ~20Hz if physically disturbed while cooling, though stable once warmed - a real confound of comparable magnitude to the effect being chased, and one `weighted_bias` is immune to (it's computed purely in the digital/audio domain) while Aux SP is not (it reads the actual on-air RF, including any real XO drift). On the direct question - `weighted_bias` improved with `Q` on (-20.72Hz undithered -> -16.4Hz at 0.5Hz dither -> -17.4Hz at 0.05Hz dither) but plateaued well short of zero and was insensitive to a 10x change in dither amplitude, which reads as `Q` successfully doing its designed job (decorrelating which sample-grid alignment each null lands on) but landing on the genuine alignment-averaged mean of the underlying near-null bias mechanism, which this evidence suggests is NOT itself zero-mean - so full centering on zero was probably never a fully justified expectation, and closing the remaining ~17Hz gap likely needs one of the "Targeted"/"Principled" null-handling fixes already on record in `null_bias_investigation.md` rather than further dither tuning. Full reasoning and two proposed follow-up tests (repeat on an already-near-zero pair like 1500/1700; use Aux SP's cursor-based static reads instead of the live trace) logged in that file's "2026-09-12, later same day" entry.

**2026-09-12, later still: found the mechanism behind "changing relative_delay (or switching presets) shifts the measured tone frequency," and it exposes a real gap in every `weighted_bias` reading taken so far.** User reported (back on 700/1700, now reporting Aux SP as an offset from a 1000Hz nominal center) that `'['`/`']'` and preset switches (e.g. 1 & 3) repeatedly shift the measured tone frequency, sometimes a few Hz, sometimes ~25Hz, often snapping between two values rather than moving smoothly. Root cause: `relative_delay_apply()` (`relative_delay.cpp`) runs AFTER `ssb_dsp_process_sample()` and, for the positive delay every two-tone preset uses, holds `freq_dev_hz` back relative to an UNTOUCHED `envelope` - so it directly controls whether the well-documented near-null `freq_dev` spikes land on envelope~0 (suppressed, as `weighted_bias`'s whole env^2-weighting design assumes) or get exposed to non-negligible transmitted power (a real, coherent contamination of the actual radiated spectrum, not a diagnostic artifact). Since the spikes recur at the identical coherent alignment every cycle, a fixed delay produces a fixed "locked-in" bias, and because the spike is narrow/near-discontinuous, small delay changes near a good alignment barely matter while crossing the spike's window can swing the bias tens of Hz - matching everything the user described. Confirmed directly: preset 1 has `relative_delay_samples=0.00`, preset 3 has `2.00` (`settings.h`) - a 125us swing baked into the preset switch itself. Bigger finding: `null_bias`/`weighted_bias` are accumulated INSIDE `ssb_dsp_process_sample()`, i.e. pre-delay - `ssb_mic_test.ino` already had a comment flagging this blindness and reasoning "a pure sample delay can't change frequency content," which is true of `freq_dev_hz` in isolation but misses that the effect lives in the cross-alignment between two DIFFERENT signals. Net: every `weighted_bias` number gathered anywhere in this investigation (including both `Q`-dither results) is blind to whatever delay/preset is doing to the transmitted spectrum - Aux SP has been the only window into it. Also very plausibly the same mechanism behind this project's whole delay-tuning-for-IMD history. Proposed (not yet actioned, pending user decision): add a post-delay null-bias variant fed from `delayed_freq_dev_hz`/`delayed_envelope`. Full writeup in `null_bias_investigation.md`'s "2026-09-12, later still" entry.

**2026-09-12, later still: implemented a per-event jump log ('J') to directly test whether every random TX jump is really explained by null-crossing, per the user's explicit engineering doubt.** User's stated goal: eliminate the random freq jumps, and they're not convinced null-crossing is the only mechanism given how many "strange states" this investigation has turned up. Asked for diagnostics that would help settle it. Every jump diagnostic so far (`max_freq_dev_step`, the post-step trace) only remembers the single worst event per run - one data point isn't enough to prove "always this cause." Added: a `JUMP_LOG_LEN=8` ring capturing every qualifying step (threshold 300Hz, well below "new record") with the POST-delay envelope, `relative_delay_samples`, that tick's `busy_us`, audio source, and a `near_null` classification per event; a running near-null percentage printed automatically in the periodic block; and a new `'J'` command to dump the full ring on demand. Also added the matching post-delay envelope to the existing post-step `tx_freq` trace. Direct test: if `near_null` stays near 100% across many independently-logged events (not just one), that's real support for the existing theory; any consistent `near_null=false` population is hard evidence of a second mechanism. Also settles "does this happen on mic input too" for free, since every entry records its audio source. Full writeup, including the suggested test protocol (long two-tone run vs. comparable mic run, compare via 'J'), in `null_bias_investigation.md`'s "2026-09-12, later still" entry. Two incidental IRAM_ATTR fixes made along the way (`ssb_dsp_get_null_bias_threshold()`, `relative_delay_get_samples()` - both now called from the new hot-path code, neither previously marked, unlike their setters). Not yet bench-tested - no toolchain available in this environment, per this project's standing caveat on every firmware change made here.

**2026-09-12, later still: first 'J' bench data (503384 events, 0% near-null at relative_delay=+4.60) - found and fixed a real bug in the jump log's own near_null classification, plus a compelling unifying hypothesis not yet confirmed.** The eye-catching "0% near-null" turned out to be the diagnostic itself measuring the wrong thing, not evidence against the null-crossing theory: for `delay>0` (every two-tone preset), `relative_delay_apply()` reads its envelope ring at zero lag, so the `delayed_envelope` the jump log was comparing against was the CURRENT tick's envelope, not time-matched to the (4.60-sample-delayed) `freq_dev_hz` value each logged step actually came from. Fixed by adding a third, properly time-matched output (`envelope_at_freq_time`) to `relative_delay_apply()`, used now everywhere the jump log and post-step trace classify near_null. Separately, `+4.60` samples is more than double the largest relative_delay this whole project has ever bench-validated (everything else stays 0.00-2.10) - and the captured data (an exact, repeating 3-value tx_freq cycle, steps summing to zero, recurring every ~490us for 500K+ repetitions, with `busy_us` tracking the same 3-cycle pattern) is consistent with a compelling hypothesis: a badly-mistuned delay may expose EVERY beat's near-null spike instead of an occasional one, turning what's normally a rare, hours-apart glitch into a continuous, metronomic oscillation - i.e. still the same mechanism, just happening far more often due to the extreme delay setting. Not yet confirmed - proposed a direct test (reflash with the fix, compare 'J' at +4.60 vs. a previously-validated delay like 0-2.10) rather than reasoning further from one capture. Full writeup in `null_bias_investigation.md`'s new 2026-09-12 entry.

**2026-09-12, later still: +4.60 was incidental (scanning), and the user reports the most interesting jump activity concentrates around relative_delay~=2 - not "worse the further out you go."** Complicates the "badly-mistuned delay exposes every beat's spike" hypothesis from the previous entry: `relative_delay~=2.00-2.10` isn't a mistuned value in this project's own history - it's exactly the sub-0.1-sample window the delay-tuning saga settled on as the BEST two-tone IMD compromise. If jump activity is also most active right there, that's a possible, previously unrecognized tension between "best IMD" and "fewest random jumps" rather than one delay value fixing both. Asked for clarification on what "interesting" means precisely, and proposed a small delay sweep with the fixed 'J' (0.00/1.00/2.00/2.05/3.00, reset before each) to see whether activity peaks sharply at ~2 (pointing at something specific to landing on an integer sample, where interp_ring()'s interpolation weight stops blending two ring entries) or trends smoothly. Full reasoning in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, later still: second 'J' capture (delay=+0.90) decoded to a clean 3-state locked cycle, 2 of 3 states not near-null by the blended test - traced to a second diagnostic gap (lopsided fractional blending), now fixed.** 25693 events (~313/s - down from ~2038/s at +4.60, but still far from "rare"), 25% near-null overall. Decoding the 8 printed events showed a clean, repeating 3-state cycle (steps -6400/+7200/-800Hz, summing to zero) - same qualitative signature as the +4.60 capture. Only the +7200Hz step (env=0.036) is near-null; the other two (env=0.235, 0.354) aren't. The bug fix from the previous entry is confirmed working (near_null is no longer stuck at 0%), but this exposed a second, subtler gap: at delay=+0.90 (frac=0.90, a lopsided 90/10 blend), a genuinely near-null RAW sample contributing only 10% weight could easily produce a blended reading like 0.235 without the blended test ever catching it. Added a second output (`envelope_at_freq_time_min` - the smaller of the two raw samples actually being blended) and a second near-null classification (`near_null_either`) to distinguish "was the transmitted result near a null" from "did anything near a null contribute at all." Not yet re-tested - the next capture will show whether `near_null_either` comes back much higher (supporting the null-crossing theory after all) or stays similarly low (real evidence of a null-independent component in this specific cycle). Full writeup in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, later still: third 'J' capture (delay=+4.28) - `near_null_either` fix confirmed working (2% -> 34%), decoded to a clean 3-state cycle where 2 of 3 transitions ARE explained, but the largest is not by either test - added a third diagnostic (raw freq_dev pair) to dig into that one specifically before concluding a null-independent mechanism exists.** 134573 events, 2% near_null_blended, 34% near_null_either - a real, large jump from the +0.90 capture's numbers, confirming the lopsided-blend fix from the previous entry is doing real work, not just adding noise. Decoding the 8 printed events showed the same "clean repeating N-state cycle" signature as both prior captures: `~14195600 -> ~14199120 (+3520Hz) -> ~14201360 (+2240Hz) -> ~14195600 (-5761Hz)`, repeating. The two smaller transitions (+3520Hz, +2240Hz) both classify `near_null_either` (a near-null raw sample was hiding in a lopsided blend, same mechanism as the +0.90 fix targeted) - real, direct confirmation that the fix is catching genuine cases, not just relabeling noise. But the third and LARGEST transition (-5761Hz, env=0.277/0.247) does NOT classify as near-null by either test - both raw envelope samples sit comfortably above the 0.05 threshold. This is the first data point in this whole investigation where the more permissive test still can't explain a jump. Before treating that as proof of a second, null-independent mechanism, there's a more basic question worth answering first: were the two RAW (undelayed) `freq_dev_hz` values feeding that -5761Hz step already about that far apart in the raw signal (a genuine discontinuity that's real, just not one an envelope-near-null test is built to catch), or are both individually unremarkable and the large DELAYED step is actually an interpolation artifact from blending across a ~4.28-sample lag during a fast-changing part of the waveform? Added a third diagnostic output pair (`raw_freq_dev_near`/`raw_freq_dev_far` - the two individual raw ring values `interp_ring()` blends, not a derived statistic) threaded through `relative_delay_apply()` -> `diagnostics_set_tx_info()` -> the jump log's per-event struct and 'J' printout, to answer this directly on the next capture. Also corrected a stale code comment in `ssb_mic_test.ino` (right at the `diagnostics_set_tx_info()` call site) that argued "a pure sample delay can't change frequency content" as the reason the pre-delay null_bias/weighted_bias diagnostics didn't need to worry about the delay line - true of `freq_dev_hz`'s own spectrum in isolation, but the wrong question, since the whole mechanism this thread has been chasing lives in the CROSS-alignment between the (delayed) freq_dev and the (mostly undelayed, for positive delay) envelope, not in freq_dev's spectrum alone. Not yet re-bench-tested - no toolchain available in this environment, per standing caveat. Also worth flagging for the next round regardless of what the raw-freq_dev result shows: qualifying-jump rates are now measured at hundreds to low-thousands per second even at delay settings within this project's historically-validated range, which raises a calibration question about whether `JUMP_LOG_THRESHOLD_HZ=300` is catching the rare, dramatic "random jump" symptom that motivated this whole diagnostic, or routine two-tone FM modulation dynamics that were always there and simply hadn't been measured directly before. Full writeup in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: fourth/fifth 'J' captures (delay=+1.75) directly confirmed the user's own suspicion - the low-level jump log carries NO signal about when a human-perceptible frequency shift actually happens, because it's saturated by routine per-null churn regardless.** User's stated concern: "the J table changes all the time even if my observation would say steady state," and a capture taken "as soon as I could" after a `'['`/`']'` sweep-triggered jump - explicitly flagged as likely "after the horse bolted." A second capture, taken deliberately right after Aux SP showed a real, user-observed shift (1000Hz -> 962Hz, i.e. a ~38Hz change to what the user actually watches), came back showing the *exact same* repeating 3-state cycle, same Hz values (~14195358/14199363/14201360, cycling via -4007/+6003/-1996Hz steps), same near-null pattern (2 of 3 states `NEAR_NULL(either)`, one not), and the same climbing lifetime `near_null_either%` (32% -> 57% as total qualifying events climbed from 52407 to 520213) as the "nothing happened" capture immediately before it. Bonus finding from the new raw_freq_dev field: the third, non-near-null transition in this cycle (~-2000Hz delayed step) is fed by two RAW freq_dev values ~8000Hz apart (e.g. near=-6805Hz, far=+1206Hz) - i.e. `raw_delta` is roughly 4x the actual transmitted step, confirming the underlying raw signal really is discontinuous at that instant (not an interpolation artifact) even though the envelope reading (0.18-0.29) doesn't dip below the near-null threshold - the freq_dev signal is still visibly "in transition" from the same null-crossing event, just measured slightly off from the reading's deepest point. Net conclusion: this specific 3-state cycle recurs continuously (hundreds of thousands of events accumulate in minutes) regardless of whether the user's own observed/perceived frequency is stable or has just moved - the 300Hz-per-tick threshold is answering a completely different, much smaller and far more frequent question than "did the frequency someone is watching just change."

**Fix, per the user's own proposed direction ("track the mean freq and trigger off that, so it sees what I see"):** added a SEPARATE tool, deliberately independent of the `'J'` log's per-tick threshold. Tracks a fast (50ms tau - several beat-null cycles' worth) and slow (2s tau) EMA pair of `delayed_freq_dev_hz`; when they diverge by more than `SLOW_JUMP_TRIGGER_HZ` (set to 5Hz - the user's own independently-reported Aux SP visual-read tolerance, not a DSP-internal number), that's treated as a change a human would actually notice. On trigger, a coarse (1.25ms/bin) trace spanning ~50ms before and ~50ms after is LATCHED (frozen, unlike `'J'`'s ring which keeps sliding) so a human-reaction-time delay before checking it can't erase the answer. New `'K'` serial command prints a live "still watching" readout if nothing has triggered yet, or the full before/after trace if it has, then re-arms (resyncing the slow EMA to the fast one, to avoid an immediate re-trigger storm while the slow EMA is still catching up) for the next event. Not yet bench-tested - no toolchain available in this environment. Full design writeup and open questions in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: first real 'K' capture was itself a false positive - found and fixed the cause (EMA seeding/resync bias), added a warm-up hold.** Capture: `before=697.40Hz after=704.63Hz delta=+7.23Hz`, only 3 pre-bins filled (fired within ~4ms of boot/reset) - and the 40-bin post-trace shows the exact same repeating cycle (800/800/800/400Hz, period 4 bins, `env_min=0.001` throughout) both before AND after the "trigger," with no visible transition anywhere in the data. Root cause: both EMAs seed from a single RAW sample on the very first tick (here, 800Hz - one extreme of the ongoing periodic churn, not its ~700Hz time-average); the fast EMA (tau=50ms) converges to the true average within tens of ms while the slow EMA (tau=2s) is still sitting almost exactly at the biased seed - diverging past the 5Hz threshold from initialization bias alone, not a real event. The resync-on-re-arm has the same exposure in miniature, since the fast EMA it snaps the slow one to is itself only 50ms-smoothed. **Fixed**: added a warm-up hold (`FREQ_EMA_WARMUP_MS=8000`, ~4x the slow EMA's tau - the standard first-order settling heuristic, ~1.8% residual error left after 4 tau) on the trigger CHECK only (the EMAs keep updating throughout, so they're already converging on real dynamics) - armed at both first boot and every re-arm after a trigger is read. Side effect, also useful: this naturally debounces against another trigger within seconds of the last one. `'K'`'s "still watching" readout now also shows `warmup_left=...ms` so it's clear when the hold - not the threshold - is what's currently blocking a trigger. Not yet bench-tested. Full writeup in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: second 'K' capture (delay=+3.85, pre-warm-up-fix build) looks like the first genuinely real event this feature has caught - a two-stage transition between structurally different repeating cycles, user-flagged as "a spontaneous change."** Unlike the first (false-positive) capture, this one has a full 40/40 pre/post bins - it had been watching for the full 50ms pre-window before firing, not milliseconds after boot. The pre-trace shows the familiar ~800/800/800/400Hz cycle (mean 700.36Hz, matching `before=`), already slowly widening in amplitude even while "steady." At the trigger, one cycle state jumps from ~400Hz to ~1197Hz, and the post-trace settles into a NEW repeating cycle (800/800/~1197/~1603Hz) - then, partway through the SAME 40-bin post window (bin +26), a SECOND, distinct shift happens: the previously-steady 800/800 pair jumps to 1600/1600 (almost exactly double), while the other two cycle elements continue the same drift trend uninterrupted. So one capture shows two separate discrete jumps, both fully bracketed with clean before/after context - exactly the "before and during" shape this feature was built to deliver, in sharp contrast to what `'J'` produced for the same kind of question. Structural aside: nearly every distinct value across the whole capture sits close to an exact multiple of 400Hz (400/800/1200/1600) - a plausible (unconfirmed) link to the 2nd/4th/6th/8th harmonics of a 200Hz two-tone beat frequency, this project's usual tone spacing. Caveat: this build predates the warm-up fix above, so the exact trigger INSTANT could carry a small resync bias if this wasn't the first-ever trigger since boot - but the underlying two-stage cycle change is directly visible in the raw bin values regardless of the EMA math. Full analysis, including the harmonic-order hypothesis, in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: tone pair for the +3.85 'K' capture confirmed as 700/1700Hz - retracts the earlier 200Hz-beat-harmonic guess, but the corrected numbers line up more precisely.** `expected_center_hz=(700+1700)/2=1200Hz` exactly - and the post-trigger cycle's middle rung (~1197-1200Hz) sits almost exactly on it, a much cleaner match than the retracted guess. Re-reading the capture slot-by-slot (not just cycle means) shows both discrete jumps are ~800Hz steps: pre-cycle `[~800,800,800,~400]` -> first jump moves the `~400` slot to `~1197` and one `800` slot to `~1603` (both ~+800Hz) -> second jump (bin +26) moves the remaining two `800` slots to `1600` each (`+800Hz` exactly). All values visited sit on a ~400Hz grid, and 400/800 are exact multiples of 100Hz - the same common factor 700/1700/their 1000Hz beat all share (7x/17x/10x), consistent with this project's established phase-accumulator/SAMPLE_RATE_HZ tone-locking mechanism. Plausible structural link, not yet a full derivation of why 800Hz specifically. Full writeup in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: two more 'K' captures (one "[] scan induced", one "spontaneous") both came back showing a PERFECTLY flat, non-drifting repeating cycle across the entire 50ms pre + 50ms post trace, despite a real 8-11Hz fast/slow gap having triggered them - added two targeted diagnostics rather than guessing further from single captures.** Both traces are essentially bin-for-bin identical before and after (e.g. `800.0/800.0/800.3/399.7Hz` repeating exactly, no visible transition anywhere) - if the signal had truly been this stable for as long as it's been running, both EMAs should long since have converged to within a fraction of a Hz of each other (the cycle-repeat rate is far above either EMA's cutoff). An 8-11Hz gap surviving a locally-flat 100ms window is best explained by a real change that happened AND fully resolved on a timescale longer than 50ms but shorter than the slow EMA's ~2s memory - i.e. a continuous drift too gradual to show a visible slope over 50ms (this project already has direct precedent for continuous multi-second drift, from the +3.85 capture's own within-window creep) but fast enough to separate a 50ms average from a 2s one.

Added two things to test this directly rather than re-guessing from the next single capture: (1) `relative_delay_get_last_change_ms()` (`relative_delay.h`/`.cpp`) timestamps every `'['`/`']'`/`'''`/`';'`/preset-load delay change; `'K'` now reports "relative_delay was last changed Xms before this trigger" (or "has not been changed since boot"), directly correlating a trigger against the user's own recent actions without relying on memory/labels. (2) A second, much-longer-timescale companion trace (`EMA_TREND_LEN=80` samples every `EMA_TREND_SAMPLE_TICKS`=25ms, ~2s of history total, matching the slow EMA's own tau) that snapshots the fast/slow EMA VALUES themselves (not raw bin data) periodically, frozen at the trigger the same way the fine trace is - directly showing whether the gap built up as a smooth multi-second ramp or something more abrupt, without needing to grow the expensive fine-grained trace itself. Not yet bench-tested. Full writeup in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: the 8-second warm-up above was undersized, not wrong - replaced single-sample EMA seeding with a two-stage boot-settle-then-snap init.** A re-run of the "[] scan induced" capture (t=9239ms, well past the 8s hold) fired again, and the new TREND block (added specifically to catch this kind of thing) showed exactly why: `fast` sat flat at 699.07-699.08Hz for the ENTIRE visible 2-second history while `slow` was still visibly, smoothly climbing from 666.23Hz toward it (687.31Hz by the last sample) - a still-converging exponential curve, not noise. That only happens if the original single-raw-sample seed was itself a large outlier (plausibly one of the near-null freq_dev spikes this project has documented elsewhere reaching thousands of Hz) - for a seed error that large, "4 time-constants ~= 98% converged" still leaves a residual of tens of Hz, comfortably enough to keep tripping a 5Hz threshold for seconds past the hold. Root-cause fix: stop seeding `slow` from a raw sample at all. Now only `fast` seeds from the first raw tick; it runs alone for `FREQ_EMA_BOOT_SETTLE_MS=500` (~10x its own 50ms tau, so ~99.9995% converged even from a large seed error); THEN `slow` is snapped directly to `fast`'s already-converged value (a clean transfer, never touching a raw sample), with the existing `FREQ_EMA_WARMUP_MS=8000` hold kept afterward purely as residual insurance rather than doing the main work. New/renamed statics in `diagnostics.cpp`: `s_freq_ema_fast_inited` (was `s_freq_ema_inited`), `s_freq_ema_slow_inited` (new), `s_freq_ema_boot_settle_ticks_left`/`FREQ_EMA_BOOT_SETTLE_TICKS` (new). Both the trigger check and the trend-ring recording are now gated on `s_freq_ema_slow_inited` (in addition to the existing warm-up/state checks) so nothing is evaluated or recorded off a not-yet-meaningful slow value. The re-arm-on-print logic is unchanged (still resyncs `slow=fast` and re-arms the 8s hold) - remains valid since `s_freq_ema_slow_inited` stays true permanently once set at boot. Not yet bench-tested - no toolchain available in this environment, per standing caveat.

**2026-09-12, yet later still: three more real 'K' captures (all delay=+2.70, all env_min pinned at 0.238 for the full 100ms window, all with flat-to-near-flat 2s trends ruling out both the boot-seeding bug above and any delay-change confound) show the same underlying picture in much sharper detail - a cycle transition isn't one instantaneous jump, it's a CASCADE of several independent ~400/800Hz steps landing in different repeating-cycle "slots" at different times over a ~5-40ms window.** Three captures: `delta=+17.93Hz` (before=700.43, after=1093.40), `delta=+21.35Hz` (before=700.58, after=1043.65, delay unchanged for 181535ms beforehand), and `delta=+15.54Hz` (before=700.43, after=1013.41, delay unchanged for 335207ms beforehand, user-labeled "finished up at nominal zero"). The richest (`+21.35Hz`) shows this most clearly: its 4-slot repeating cycle has two "clean" slots that each take one 800Hz step at different times (one AT the trigger, matching how the trigger is defined; the other ~5ms later), and two "near-null-blended" slots whose bin-averaged values keep summing to a constant (1200Hz pre-trigger, matching `expected_center_hz` for 700/1700 exactly) but that constant ITSELF steps twice more, in the same 800Hz quantum - first to 2000Hz (~11ms post-trigger), then to 2800Hz (~26ms post-trigger). Same underlying event, every slot of the cycle eventually reflects it, just staggered in time - and even the "smooth drift" seen in earlier near-null-blended captures looks like it's really discrete 800Hz steps smeared across bins by the blend/average. The `+15.54Hz` capture shows the same staggering with cleaner (non-blended) values: `[800,800,800,400] -> [1200,800,1600,1600]` via four separate steps landing at the trigger, +4, +15, and +22 bins - including the first observed clean 400Hz-sized single step in this whole investigation, which reinforces rather than contradicts the established 400Hz-grid/800Hz-quantum picture. All three captures keep `env_min` pinned at a constant 0.238 across the entire 80-bin window, before and after - continuing to show zero correlation with any near-null envelope dip at the coarse per-bin level. Net implication for the 'K' feature itself: a trigger correctly marks the START of a real transition, but full settling can take several tens of ms longer than the trigger instant - worth remembering when reading any 'K' trace's "after" value, and itself a data point for the boot-settle fix above (multi-stage settling is a real property of this signal, not an artifact of bad EMA seeding). Full step-by-step decode of all three in `null_bias_investigation.md`'s newest 2026-09-12 entry. Not yet asked/confirmed: whether these three were also on 700/1700 - the numbers (1200Hz center, 800/400Hz steps) are consistent with that pair but not independently confirmed for these specific captures.

**2026-09-12, yet later still: confirmed - all three captures above were also on 700/1700Hz.** Closes the open question from the previous entry; the 1200Hz-center/400-800Hz-step grid decode stands as-is, now on a fully-confirmed tone pair rather than an inferred one.

**2026-09-12, yet later still: a genuinely scan-induced 'K' capture (delay=+5.23, "relative_delay was last changed 1ms before this trigger") - first real validation of the delay-timestamp diagnostic, and a qualitatively messier, more null-adjacent signature than the three +2.70 captures above.** User-labeled "scan induced now at 990" - and for the first time, the delay-change timestamp actually confirms it (1ms before the trigger, vs. the two earlier "scan induced"-labeled captures that came back "not changed since boot"/minutes-old, which is exactly why that diagnostic got built). `delay=+5.23` is well outside this project's historically-validated 2.00-2.10 IMD-compromise window - in the same "large/incidental" territory as the earlier +4.28/+4.60 'J' captures. Two things mark this as a different regime from the three clean +2.70 captures logged above: `env_min` is NOT pinned high this time - it climbs from 0.040 (near or below the 0.05 near-null threshold used elsewhere in this project) up to 0.086 over the 81-bin window, i.e. genuinely null-adjacent, not the comfortably-clear 0.238 seen throughout the +2.70 data; and the transition itself is messier - two of the four repeating-cycle slots jump immediately (at the trigger/+4) to non-quantized values that keep drifting for the whole 50ms post-window, while the other two hold clean 800.0Hz values for ~30ms before taking one clean +800Hz step near bin +26/+27. Reads as evidence that the clean, null-independent, 400Hz-grid cascade mechanism characterized above and the messier, genuinely-near-null-influenced behavior documented earlier (`+4.28`/`+4.60` 'J' captures) are two distinguishable regimes, separated by how far delay sits from the tuned ~2.0-2.1 window. Full decode in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: fifth real 'K' capture ("spontaneous," delay unchanged for 147312ms beforehand) reproduces the exact sum-conserved-pair-plus-two-clean-steps signature from the richest +2.70 capture above, and env_min sits at a THIRD, intermediate level (~0.09) not seen before.** Decodes identically in structure to the `+21.35Hz` capture: two "clean" slots each take one isolated +800Hz step at different times post-trigger (~12ms and ~28ms here), while a near-null-blended ADJACENT pair of bins (not same-phase-mod-4, but the two bins straddling a beat-null crossing each cycle) keeps its bin-averaged SUM pinned at 1200Hz pre-trigger (matching `expected_center_hz` for 700/1700 again) and then steps that sum by the same +800Hz quantum to 2000Hz post-trigger - with the very last printed bin (+40) hinting at a further step toward 2800Hz just starting as the 50ms capture window runs out, same cascade timing order-of-magnitude as before. `env_min` this time sits at a steady ~0.089-0.093 across the whole trace - comfortably above the 0.05 near-null threshold (so not "near-null" by the existing test) but well below the ~0.238 seen in the earlier clean captures, a third distinct env_min regime alongside the ~0.238 "clear" and ~0.04-0.09-climbing "scan-induced/messy" ones already on record. Worth watching whether this intermediate level turns out to correlate with anything (not yet investigated). Full decode in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: two more 'K' captures, both at relative_delay=+2.33 (a new value, not the usual +2.70), REPLICATE the intermediate ~0.09 env_min level exactly and surface a new finding - the single bin that defines a trigger is often a one-bin transient overshoot, not a real sample of either the old or new steady state.** Sixth capture (`delta=+14.77Hz`, delay unchanged 300985ms beforehand) and seventh (`delta=+18.65Hz`, delay unchanged 454666ms beforehand) both read `env_min~=0.089-0.093` throughout - matching the fifth capture's ~0.09 level almost exactly, while the three +2.70 captures logged earlier all read ~0.238 and the one +5.23 capture read 0.04-0.086. Two independent captures at the same non-2.70 delay landing on the same env_min level is a real replication, not a coincidence - `env_min` looks like it depends fairly directly on exactly what `relative_delay` is set to, which fits the already-established mechanism (delay controls how the envelope ring's read point aligns with wherever freq_dev's near-null spike falls).

The seventh capture is the cleanest, most complete transition decoded in this whole investigation - four cycle slots snap to new values with no blending noise at all: three settle at a new level +800Hz above their old one (one within ~5ms, two together around ~35ms post-trigger) and the fourth settles at a new level +400Hz above its old one - but that fourth slot's very FIRST post-transition reading (the trigger bin itself) shows 1600.0Hz, a full +800Hz jump, before its very next occurrence drops to the real, sustained +400Hz level (1200.0Hz) it holds for the rest of the trace. The sixth capture shows the identical shape: the trigger bin reads 1199.3Hz, but that slot's next occurrence is 800.0Hz - a real, clean +400Hz step from its ~399.5Hz baseline, with the trigger-instant reading itself never recurring. Read as strong evidence that the ONE bin `'K'` singles out as "the trigger" is specifically likely to straddle a rapid transition and can show a distorted, transient blended value that isn't representative of either the pre- or post-transition steady state - worth remembering when reading any 'K' trace's own trigger-bin line, not just its `before=`/`after=` summary. Full decode of both captures in `null_bias_investigation.md`'s newest 2026-09-12 entry.

**2026-09-12, yet later still: CORRECTION - found and fixed an off-by-one bug in my own slot-analysis, affecting several capture write-ups above; then decoded two more captures with the fixed method.** While decoding an eighth 'K' capture, a continuity check failed for a slot I expected to match cleanly - root cause: `'K'`'s trace labels bins `-40..-1` (pre) then `+1..+40` (post) with no "bin 0", so matching a post-trigger bin back to its pre-trigger slot needs `phase=(bin_index-1) mod 4`, not the same `(bin_index+40) mod 4` formula the pre-trigger side uses - I'd been using the latter for both, silently shifting every post-trigger slot's correspondence by one. This does NOT affect the sum-conservation findings (computed from direct adjacent-bin arithmetic, unaffected by the labeling) or the overall "cascade of clean 400Hz-grid steps over 5-40ms" conclusion, but it DOES retract three specific claims: the "+21.35Hz" capture's phantom second clean slot at bin+4 (that data point is actually the trigger slot's own continuation; the real fourth slot only starts moving at ~48ms, right at the edge of the window); the "nominal zero" capture's claimed second/400Hz-sized step on the trigger slot (the trigger slot actually just holds its one +800Hz step forever - the extra step belongs to a different, uninvolved slot); and, most notably, the entire "trigger bin is a one-bin transient overshoot that never recurs" finding from the two delay=+2.33 captures - re-verified, all four slots in each of those captures take clean, PERSISTING +800Hz steps with no reversion at all. Full corrected re-derivation of all four affected captures in `null_bias_investigation.md`.

**Same entry, continued: eighth and ninth 'K' captures (both delay=+2.33) give a third independent replication of the 1200->2000->2800Hz staged sum-progression, and one of them catches the near-null pair directly triggering 'K' itself.** Both read `env_min~=0.09` again (now four captures at this delay, all matching). The eighth shows the full three-stage progression (1200->2000Hz at the trigger, ->2800Hz about 33ms later) alongside one slot that never moves and one that takes an isolated +800Hz step at ~12ms. The ninth (`delta=+10.37Hz`, the smallest yet) shows the trigger itself firing on a NON-clean value (+532.8Hz) - explained once you check the pair-sum instead of either raw side: the sum holds a clean 1200Hz pre-trigger, steps to 2000Hz exactly at the trigger, and stays there for the rest of the trace, with the "unclean" trigger reading being just an asymmetric split of that clean step across the pair's two blended raw components. This directly explains why a trigger can sometimes land on a value that isn't itself a multiple of 400 - the quantized quantity is the near-null pair's SUM, not necessarily either individual reading. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: tenth 'K' capture (delay=+2.33, `delta=+20.99Hz`) - the cleanest, most fully-quantized transition yet, zero interpolation blending anywhere, including one slot that hops TWICE within the same event.** Fifth capture at this delay, `env_min` again ~0.089-0.093 (five for five now). Pre-cycle exact `[800,400,800,800]` with no drift at all (unlike most other captures, nothing here is fractionally blended near a null). Corrected-method decode: one slot never moves (flat 800.0Hz for the entire 81-bin trace); the trigger slot takes one clean +800Hz step and holds forever; a third slot takes one clean +800Hz step, but very late (~40ms post-trigger); and a fourth slot takes TWO clean +800Hz steps in the same capture - 400.0Hz -> 1200.0Hz almost immediately (~2.5ms post-trigger), then 1200.0Hz -> 2000.0Hz again later (~37ms post-trigger) - visiting three distinct grid levels within one 50ms window. Fully consistent with everything decoded so far, just unusually clean - no drift to disentangle from the discrete steps. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: eleventh 'K' capture (delay=+2.33, `delta=+15.32Hz`) - sixth-for-sixth on the env_min correlation, and this time the TRIGGER slot itself is the one that hops twice.** Another fully-quantized, zero-drift pre-cycle (`[800,800,800,400]`, exact). One slot never moves; one takes a quick clean +800Hz step (~7ms post-trigger); one takes a later clean +800Hz step (~35ms); and the trigger slot itself, having already jumped 400->1200Hz at the trigger, takes a SECOND +800Hz step to 2000.0Hz very late (~44ms post-trigger, right at the edge of the visible window) - mirroring the tenth capture's "one slot hops twice" finding, except this time it's the trigger's own slot doing the double hop rather than a bystander. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: twelfth 'K' capture, a new delay (+3.08, changed 4.6s before the trigger) - genuinely near-null (`env_min=0.025`, below the 0.05 threshold, the first "spontaneous"-labeled capture to cross it) yet the CLEANEST, most extended quantized cascade yet, and it complicates the delay-vs-env_min picture.** User flagged the delay move but wasn't sure it was the direct cause ("this happened later I think") - the delay change was 4614ms before the trigger, longer than the 2s TREND window can confirm or rule out either way. `env_min` sits at a flat 0.025 across the ENTIRE 81-bin trace - genuinely near-null by the existing threshold, unlike every other "spontaneous" capture this session. Despite that, the transition itself is exceptionally clean: the near-null-blended adjacent pair's SUM steps in the established 800Hz quantum not twice or three times but FOUR levels - 1200Hz (pre) -> 2000Hz (~7ms) -> 2800Hz (~27ms) -> 3600Hz (~42ms, right at the window's edge) - the longest staged cascade decoded so far, while two other slots (one silent, one holding the trigger's own single +800Hz step) round out the four-slot cycle. This directly contradicts the earlier working assumption that "near-null envelope = messy, non-quantized" (drawn from the one delay=+5.23 capture) - here, near-null and cleanly-quantized coexist. It also complicates the delay-vs-env_min correlation: `+3.08` is almost exactly as far from `+2.70` as `+2.33` is (0.38 vs 0.37), yet reads `env_min=0.025` versus `+2.33`'s consistent ~0.09 - not a simple function of distance from the +2.70 "sweet spot," more like a narrow, sharply-peaked alignment specific to +2.70 itself. Full decode and discussion in `null_bias_investigation.md`.

**2026-09-12, yet later still: thirteenth 'K' capture, second at delay=+3.08 - `env_min=0.025` again exactly (confirming near-null is a real, repeatable property of THIS delay, not a one-off), and a new step type: a slot jumping TWO quanta at once (+1600Hz) instead of two separate +800Hz hops.** Genuinely spontaneous (delay unchanged for 158246ms beforehand). Zero-drift, fully-quantized pre-cycle again (`[800,400,800,800]`). Two slots never move at all; the trigger slot takes its one +800Hz step and holds forever; the fourth slot holds its original 400.0Hz value for ~17ms post-trigger, then jumps DIRECTLY to 2000.0Hz (a single +1600Hz jump, skipping the intermediate 1200Hz grid point entirely) at ~22ms and holds steady. This is a new pattern - previous "double hop" captures (tenth, eleventh) showed a slot visiting an intermediate grid point via two separate steps at different times; here the slot skips straight past it in one clean jump. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: fourteenth 'K' capture, third at delay=+3.08 - `env_min=0.025` exactly again (third-for-third), and a SECOND replication of the 4-level 1200->2000->2800->3600Hz staged sum-cascade first seen in the twelfth capture.** Genuinely spontaneous (delay unchanged for 311888ms beforehand). One slot never moves; the trigger slot takes its one +800Hz step and holds forever; the near-null-blended adjacent pair's sum steps through all four established levels again - 1200Hz (pre) -> 2000Hz (~7ms) -> 2800Hz (~30ms) -> 3600Hz (~44ms, right at the window's edge) - matching the twelfth capture's progression almost exactly in both level count and rough timing. Between the three back-to-back +3.08 captures now on record, this delay setting looks like a genuinely distinct, repeatable regime: consistently near-null (`env_min=0.025`, unlike +2.33's ~0.09 or +2.70's ~0.238) yet consistently capable of the longest, cleanest quantized cascades decoded so far - reinforcing that near-null and clean-quantization are not opposites here. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: fifteenth 'K' capture, right after loading Preset 3 (`relative_delay=+2.00` exactly, matching that preset's documented value) - the deepest near-null reading yet (`env_min=0.001`, essentially AT the null, ~25x lower than the +3.08 captures' 0.025) yet STILL a fully clean, 800Hz-quantized cascade.** Delay was changed 28659ms (~29s) before the trigger - plausibly related to loading the preset, not certain. Because `+2.00` is an exact integer sample delay, `interp_ring()`'s fractional blend weight is zero - there's no interpolation smearing between adjacent ring entries at all, which likely explains why `env_min` reads a single, unwavering 0.001 across all 81 bins rather than the ~0.09-0.238 range with sub-percent wobble seen at fractional delays. Despite being far deeper into "near-null" territory than any capture so far, the decode is completely clean: one slot never moves, two slots each take an isolated +800Hz step (~7ms and ~33ms), and the trigger slot itself double-hops (400->1200Hz at the trigger, then ->2000Hz again at ~44ms) - same repertoire of behaviors seen throughout this investigation, no extra messiness. This strengthens the case (first raised with the twelfth capture) that near-null envelope readings do NOT imply messy, non-quantized transitions in general - the one capture that WAS messy (delay=+5.23, mid-scan) was very likely messy because of being a large, actively-changing FRACTIONAL delay specifically, not because of proximity to null. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: sixteenth 'K' capture, second at Preset 3's delay (+2.00), genuinely hands-off (~30 minutes since anything was touched) - confirms `env_min=0.001` exactly again, and shows the first capture with TWO completely silent slots.** Same clean, zero-drift, fully-quantized character as the previous capture: two of the four cycle slots never move at all across the entire 81-bin trace (the first time this session two slots have stayed silent in one capture, not just one); the trigger slot takes its +800Hz step and holds forever; the fourth slot takes two separate +800Hz hops (400->1200Hz at ~22ms, then 1200->2000Hz at ~27ms, only 5ms apart) - the "visits the intermediate grid rung via two steps" pattern from captures ten/eleven, not the "single +1600Hz jump" pattern from the thirteenth capture. Second-for-second confirmation that `relative_delay=+2.00` reliably produces `env_min=0.001`. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: seventeenth 'K' capture (delay=+2.00, `delta=-5.01Hz`) - a genuinely NEW category: a smooth, continuous, non-quantized drift, not a discrete jump.** Third confirmation of `env_min=0.001` at this delay. Unlike every capture so far, there is no discrete step anywhere in this trace - the near-null-blended pair simply keeps redistributing smoothly between its two components (one rising ~811.9->820.9Hz, one falling ~387.7->378.3Hz) across the ENTIRE 100ms window, with their SUM pinned at exactly 1200.0Hz throughout, never stepping to 2000 or any other level. The other two slots stay completely flat. This is the smallest-magnitude trigger captured so far (barely over the 5Hz threshold, matching the user's own reported +/-5Hz Aux SP visual tolerance almost exactly) and the first NEGATIVE delta decoded with the corrected method - both consistent with 'K' correctly catching a borderline, continuous drift rather than a discrete event. Confirms 'K' works as designed across two distinct underlying phenomena: the discrete 800Hz-quantized jumps decoded in every capture before this one, and this smooth, continuous, non-quantized redistribution within the near-null pair, likely the same slow beat-crossing-phase precession mechanism just not happening to cross a discrete grid boundary within the visible window this time. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: eighteenth 'K' capture, fourth at delay=+2.00 - fourth-for-fourth on `env_min=0.001`, nothing qualitatively new but a clean confirmation of the established repertoire.** `delta=+21.18Hz`, delay unchanged for 489689ms beforehand. One slot never moves; the trigger slot takes its one +800Hz step and holds forever; one slot double-hops via two separate steps visiting the intermediate grid rung (400->1200Hz at ~7ms, 1200->2000Hz at ~32ms, matching the pattern from captures ten/eleven/sixteen); one slot takes a single, very late +800Hz step (~43ms, near the edge of the window). Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: nineteenth and twentieth 'K' captures, fifth/sixth at delay=+2.00, user-labeled a "noisy mode" episode - both decode as ordinary clean, fully-quantized cascades, and the twentieth is the first capture where all four cycle slots step with none left static.** Nineteenth (`delta=+15.61Hz`, hands-off ~10.7min): one slot never moves, the trigger slot takes its one +800Hz step and holds, and two more slots each take an isolated +800Hz step independently but close together (~16ms and ~22ms post-trigger). Twentieth (`delta=+17.68Hz`, hands-off ~13.3min, user: "jumped to 1006 and noise gone"): same clean repertoire, but for the first time this session every one of the four slots takes exactly one +800Hz step somewhere in the window (~trigger, ~14ms, ~27ms, ~46ms) with none remaining static - still fully 400Hz-grid-quantized, just an unusually complete sweep. Both keep `env_min=0.001` (fifth/sixth-for-fifth/sixth at this delay). Neither trace's internal decode looks "noisy" on its own - most likely the user's higher-level "noisy mode" impression was a rapid SUCCESSION of individually-clean triggers like these firing over several minutes as the mean climbed 700 -> ~1018 -> ~1042Hz, rather than any single messy transition. Full decode of both in `null_bias_investigation.md`'s newest 2026-09-12 entries.

**Same episode, continued: twenty-first 'K' capture (`delta=-5.00Hz`, seventh at delay=+2.00, user: "back down to 997") closes out the "noisy mode" run with the second-ever smooth, continuous, non-quantized drift trigger (the category first seen in the seventeenth capture).** No discrete step anywhere - two slots stay flat at 800.0Hz, and the near-null-blended pair smoothly redistributes between its two components across the entire 100ms window (one rising ~814->827Hz, one falling ~386->371Hz) with their sum pinned at a constant ~1199-1200Hz throughout, matching `expected_center_hz` for 700/1700. `env_min=0.001` again (seventh-for-seventh). Smallest-magnitude trigger of the three (right at the 5Hz threshold, negative) - consistent with the smooth-drift mechanism only barely crossing the divergence threshold rather than blowing through it like a discrete 800Hz step. Together, the three captures read as two sharp discrete jumps followed by one small continuous settle, matching the user's own reported sequence (973 -> 1006 -> 997) closely enough to support the "rapid succession of clean events" explanation for the perceived noise. Full decode in `null_bias_investigation.md`.

**2026-09-12, yet later still: added a 'Z' serial command for a clean, on-demand reboot** (`serial_commands.cpp`) - flushes a confirmation reply, short delay, then `ESP.restart()`, so getting back to a known-clean starting point between bench captures no longer needs a physical power-cycle. Not bench-tested.

**Same day: checked the "relative_delay's own ring buffer explains the staggered multi-slot cascade timing" hypothesis against the actual numbers - REFUTED, and the same check surfaced a much bigger candidate finding about the period-4 structure itself.** `PHASE_DELAY_MAX_SAMPLES=8` at `SAMPLE_RATE_HZ=16000` gives the ring a full capacity of only 500 microseconds - two orders of magnitude too short to explain the 1.24-46ms staggered timing seen throughout this investigation. That hypothesis is dropped; what actually causes the staggered timing remains open. But looking for an alternative explanation surfaced something worth taking seriously: each printed 'K' bin is the MEAN of 20 raw ticks (`SLOW_TRACE_BIN_TICKS`), and the confirmed 700/1700Hz tone pair has a 1000Hz beat - a 16-tick period at this sample rate. `gcd(20,16)=4` means any 20-tick-window average of a genuinely 16-tick-periodic signal is arithmetically guaranteed to repeat with period 4 IN BIN-INDEX SPACE, independent of delay, magnitude, or anything else project-specific. That's a strong candidate explanation for why every single capture this whole session - all five delay values, every sign and size of jump - has shown the identical period-4 "slot" structure with zero exceptions: a pattern that universal usually points at the measurement process, not the thing being measured. A quick synthetic simulation supports the qualitative shape but doesn't cleanly reproduce the exact clean levels seen on real hardware, most likely because the simulation doesn't model the real ADC/filter chain feeding the actual atan2-based freq_dev computation. Importantly, this does NOT retract any specific value/timing logged in this investigation, and does NOT explain the staggered cascade timing itself (an alias artifact of this kind would only offset different slots by about one alias period, ~5ms, not the 30-40ms gaps repeatedly observed) - flagged as a testable candidate mechanism, not a settled conclusion. Full numerics writeup in `null_bias_investigation.md`.

**Twenty-second 'K' capture: FIRST at delay=+0.00 (the untouched boot default) - a new env_min data point (0.200) that further undercuts any simple "distance from +2.70" story, user-flagged as the biggest shift seen yet ("sending this as its the biggest shift Ive seen to 950").** `delta=+20.64Hz`, only ~155s since boot. `env_min=0.200` - a new level, and notably close to +2.70's own ~0.238 despite `+0.00` being roughly as far from +2.70 as the messy +5.23 capture was, while the numerically CLOSER +2.33/+3.08 read far more near-null (~0.09/0.025). Decode is otherwise fully in the established repertoire: two silent slots, the trigger slot's single persisting step, and one slot double-hopping via the intermediate grid rung at the same ~17ms/~32ms timing seen at other delays. Full decode in `null_bias_investigation.md`. **Addendum, same capture, re-sent with more context ("hands off from 950 -> 1026"):** the user resent the identical trace (same `t=154655ms`, identical bins - not a new trigger) purely to report that the frequency kept climbing hands-off well past this trace's own `after=1003.01Hz` reading, through ~950Hz and on to ~1026Hz, all without a second 'K' trigger firing. Consistent with the standing caveat that a trace's `after=` value only reflects the fast EMA at the moment the 50ms post-window closes, not necessarily the final settled value - full settling at delay=+0.00 (this session's least-tested setting) apparently continues for at least several more seconds beyond what any single trace can show, without ever diverging by another 5Hz from a shifting baseline sharply enough to re-trigger.

**2026-09-12, yet later still: user question via the new 'Z' reboot - repeating the same manual delay sweep through ~+4.5 sometimes triggers 'K' and sometimes only gives a small non-triggering wobble; is that a boot-initial-condition effect?** Two candidate explanations, not yet distinguished: (1) genuine boot-to-boot hardware timing jitter shifting where the tone's null crossings land relative to the sample grid - but this project's own prior finding (the ~264us first-tick `spi_us` spike, explicitly confirmed "reproducible across independent reboots... not a scheduling fluke") argues boot timing is actually quite deterministic, not jittery; or (2) the `'['`/`']'` sweep is manually keyed (confirmed - no automated delay-scan code exists), so the exact instant it passes through +4.5 isn't reproducible to millisecond precision, and the confirmed 700/1700Hz tone pair's 1000Hz beat means a full null-crossing cycle repeats every ~1ms - enough for ordinary human keypress timing jitter alone to land on either side of the same knife-edge this whole investigation has already established is very thin. One clean test distinguishes them: repeat the sweep several times WITHOUT rebooting in between - if the same mixed outcome still shows up, it's sweep-timing jitter, not the reboot. Flagged for the user to try. Full writeup in `null_bias_investigation.md`.

**2026-09-12, yet later still: the no-reboot repeat test came back, and it's more interesting than either candidate explanation predicted - many solid sweeps through ~+4.5, then a clean transition into "reliably" jumping, all within ONE boot (no 'Z' pressed at all).** This rules out pure boot-initial-condition jitter outright (nothing rebooted). But it doesn't fit pure keypress-timing coincidence cleanly either - a real coin-flip-per-attempt process should scatter solid/jumpy outcomes randomly through the session, not cluster into "long solid stretch, then reliably bad." That points at a third possibility not considered before: something in the system itself slowly drifted between the two stretches during a single boot - a physically plausible (but untested) candidate being oscillator/crystal thermal warm-up drift over the first several minutes shifting the tone/sample-clock alignment this whole investigation has shown is already razor-thin. The first trigger in this run landed at `t=308312ms` (~5.1 minutes after boot) - consistent with, though not proof of, a warm-up timescale. Worth tracking in future sessions whether "goes bad" tracks elapsed uptime specifically.

**Twenty-third capture, decoded from the same episode: first-ever negative/near-zero `relative_delay` (-0.02), and the first capture where `env_min` visibly changes WITHIN one trace, closely tracking a delay change caught mid-window.** `delta=+15.46Hz`, delay last changed 17ms before the trigger - and `env_min` reads 0.010 for the first ~33ms of the pre-window then drops to 0.001 right around where that delay change happened (~15-16ms before trigger, matching the 17ms timestamp within about a bin's resolution) and stays there through the whole post-trigger window. This is the cleanest direct evidence yet that `env_min` tracks whatever delay is CURRENTLY active, quickly. `env_min=0.001` for delay=-0.02 extends the "near-integer delay -> deep null" pattern to a third value (after `+2.00`) - but sits in real tension with the earlier `delay=+0.00` capture reading `env_min=0.200`, not similarly deep, despite also being an exact integer. Most likely explanation: an unexamined confound (that capture was on whatever boot-default OTHER settings were active, not Preset 3's) rather than a breakdown of the integer-delay mechanism - flagged as an open question, not resolved. The transition itself is a new sub-pattern: two slots step essentially simultaneously with the trigger, and the other two step together (also essentially simultaneously with each other) about 30ms later - still the same "one clean +800Hz step per slot" primitive, just an unusually tight paired timing structure. Full decode in `null_bias_investigation.md`.

**2026-09-13: mechanism reference - why does relative_delay shift the output frequency at all (moving on from the Z-reboot coincidence thread).** Short answer, and it's the same mechanism this project already established well before this session's 'K' work (SDR-confirmed via the pre-existing `null_bias2` diagnostic): `relative_delay` isn't a frequency control, it's a pure re-timing tap between the phase and envelope output paths, implemented as linear interpolation between two adjacent raw ring samples. `freq_dev_hz` is a phase-derivative computation that's genuinely, mathematically unstable right at a beat-envelope null (a known, expected property of this whole SSB technique, not a bug - two adjacent raw samples can differ by thousands of Hz there). Since `relative_delay` picks which two samples get blended, different delay values select a genuinely different value out of that instability, and that value is exactly what gets sent to the AD9851 - so it's really transmitted, not a diagnostic artifact. What's still open (and what this whole session's 'K' investigation has been circling): why a value selected at one null-crossing then persists cleanly for tens of milliseconds rather than reverting the next beat cycle. One candidate (a dsp_task tick-skip permanently offsetting the two-tone phase accumulators) was considered and doesn't cleanly hold up on inspection - flagged as still open, not resolved. Full writeup in `null_bias_investigation.md`.

**2026-09-13: answered from source - Preset 2<->3 live switches do NOT preserve AM/PM sync cleanly, because they silently swap group-delay-equalizer filter designs and force-reset that filter's state every time.** Checked `settings.h`'s actual preset table: Preset 2 and 3 are identical except `relative_delay_samples` (2.03 vs 2.00, trivial) and `env_gdeq_variant` (DEFAULT vs CANDIDATE_B - two genuinely different two-section allpass filter designs for the envelope-path group-delay equalizer, not a small tweak of the same one). `envelope_gdeq_set_variant()` unconditionally zeroes both filter sections' internal IIR state on every real value change - a documented, deliberate choice ("switching live never feeds a mismatched coefficient/state pair into the very next sample") but the alternative it picks (force-zeroing a running filter's memory mid-waveform) is itself a real, physical discontinuity in the envelope path, at the exact instant of every 2<->3 switch. This explains all three reported symptoms: something visibly happening on a live switch (yes, a real transient); the "~0.5 sample, not 1" read (consistent with this being a continuous group-delay DIFFERENCE between two distinct filter designs - candidate B's own docs give 136.0us vs default's 156.5us full-band dispersion - not a discrete sample-count artifact at all); and "sometimes 1 and back to 2/3 needed" (Preset 1 has gdeq off entirely, so routing through it also fires the separate off->on reset path in `envelope_gdeq_set_enabled()`, on top of the variant-change reset that already fires on a direct 2<->3 switch - not fully resolved which specifically matters, flagged as needing a bench comparison). Bottom line: these two presets are not safely swappable live without a brief, real envelope-path glitch - that's how the code is written, not a bug. Full writeup in `null_bias_investigation.md`.

**2026-09-13: user asked whether "reset fixes it" confirms a bad/desynced state - yes, but there look to be two separate effects bundled together, and one of them isn't transient at all.** (a) The already-documented gdeq filter-state zeroing is a real but purely transient effect, identical whether reached via Preset 1 or a direct 2<->3 switch - settles out in a handful of samples, says nothing about either preset's steady state. (b) A separate, genuinely NOT-transient structural mismatch specific to Preset 2: its gdeq variant (DEFAULT) was explicitly fit, per `envelope_gdeq.h`'s own history, "against the bare analog filter ALONE, before either [ampeq] shelf existed" and is documented as "known... to NOT flatten well once ampeq's shelf(s) are on" - but Preset 2 itself runs with BOTH ampeq shelves on. Preset 3's variant (candidate B) was specifically fit against the shelves-on curve, so it's actually matched to the config both presets run. This is a permanent mismatch baked into Preset 2's own definition, not something settling time fixes. Suggested a way to tell which the user is actually seeing: a brief, switch-triggered "bad" feeling that settles out is (a); a "bad" feeling that persists indefinitely while just sitting on Preset 2 untouched is (b), and would need Preset 2 re-paired to a shelf-matched gdeq variant, not a reset. Not yet distinguished which dominates. Full writeup in `null_bias_investigation.md`.

**2026-09-15: added automatic 'K' dump-on-capture so hands-off jumps land in the serial log without a keypress, then decoded the twenty-fourth through twenty-sixth captures - the last pair of which shows exactly why this matters.** `diagnostics_service()` (Core 1, once per `loop()` iteration) now checks for a LATCHED slow_trace on its own and prints+re-arms it automatically (header reads `AUTO-CAPTURED` instead of `TRIGGERED`), typically within ~10ms of the capture completing - 'K' still works exactly as before for its live "still watching" readout and as a manual read (the two paths race harmlessly; whichever gets there first wins, the other finds nothing to do). Not bench-tested. Still need something on the PC side actually logging the serial stream to a file for unattended runs to be reviewable later - the firmware change removes the keypress, not the need to capture the output somewhere. Twenty-fourth capture: a third instance of the smooth continuous-drift category, this time showing the near-null pair's sum isn't quite exactly conserved over a full 100ms window (~3Hz drift), just locally. Twenty-fifth: a clean two-then-two staggered cascade to a new {1600,1600,1600,1200}Hz resting cycle, completing within ~31ms. Twenty-sixth (only ~154s later): an unusual +1200Hz single-bin trigger jump (not an 800Hz multiple, the first of its kind logged) with a busier cascade including a very late final-slot step near the end of the window. The interesting part is the PAIR: the twenty-fifth's cascade was heading toward a ~1500Hz resting mean, but the twenty-sixth's own `before` value is back down near the original ~700Hz baseline - either that state genuinely reverted, or (more likely, given the ~66-minute gap before the twenty-fifth was actually read via manual 'K') the reference value silently absorbed whatever happened during that long gap. Exactly the ambiguity the new auto-dump feature removes going forward, since every future trigger's baseline will now be resynced within ~10ms of the previous capture instead of whenever a human next happens to press 'K'. Full decode and technical detail in `null_bias_investigation.md`.

**2026-09-15: first hands-off bench run with the new auto-dump feature - it worked (18/18 events auto-captured, zero manual 'K' needed), and the resulting log surfaced two significant new findings.** User reported "didn't spot any big jumps" in the captured log; checking the actual header lines shows 6 of the 9 excursion cycles in this 23-minute run WERE big (~300-385Hz swings, `before`~700Hz to `after`~1000-1085Hz) - just easy to miss skimming raw text, since the printed `delta=` field is only the modest at-trigger threshold-crossing amount (~5-25Hz), not the actual swing size (`|after-before|`). One new transition sub-type also showed up: a +81Hz event where two slots kept doing the established smooth continuous-drift thing while the OTHER two slots each took a discrete +800Hz step mid-window - the first capture where different slots visibly split into two different behavioral modes at once. **Two bigger findings**, both confirmed at the bin level: (1) every "correction" event that follows a big jump ~8.14s later has a completely FLAT trace (no transition anywhere) - meaning the jump had already fully reverted to baseline before the correction fired, and the correction is really just the slow (tau=2s) EMA catching up to a reversion the fast EMA had already completed; the near-exact ~8.14s timing (vs. `FREQ_EMA_WARMUP_MS=8000`) points to this being a side effect of the auto-dump's own instant resync-at-rearm now happening mid-relaxation instead of whenever a human used to eventually poll 'K'. (2) The underlying jump-cycles themselves repeat with an extremely regular ~153.6-second period (<0.3% spread across 9 consecutive cycles) - a dedicated code search found nothing in this firmware (no software timers exist in this project at all) that could produce that cadence. Since timestamps come from the ESP32's own clock, this rules out a PC-side artifact - something on/coupled to the device itself is running on a ~153.6s cycle, and it isn't already-known code. Flagged as the top open lead for next session. Full writeup, including the per-event table, in `null_bias_investigation.md`.

**2026-09-15, later same day: second segment of the same hands-off run (`K_out_2.txt`, same boot, picks up right where the first file's timestamps left off) - strongly reconfirms both findings from the first segment.** User reported seeing "jumps and holds at maybe up to 20Hz" - that number matches the printed `delta=` field exactly (this file's deltas run -5 to +23Hz), but same as last time, `delta` is only the modest at-trigger amount, not the real swing: 5 of this file's 6 cycles are again big, `after` landing 300-370Hz above the ~700Hz baseline, same magnitude as the first file. Every correction event is again a completely flat trace with `fast` sitting dead-flat while `slow` visibly decays toward it - and the ~153.6s cadence held rock-steady across all 5 cycles in this file too (153589-153758ms), now 14 consecutive cycles across both files/~38.6 minutes total, all in the same tight band. Combined total this run: 30/30 events auto-captured, zero missed, zero manual 'K' needed. One of this file's triggers is a repeat of the atypical +1200Hz single-bin jump type (first seen in the twenty-sixth capture) - confirms it's a real recurring member of the repertoire. Full table and detail in `null_bias_investigation.md`.

**2026-09-15, later still: user reports a live SDR-observed "-22Hz, stuck for a while" and asks if it matches the K log (third segment, `K_out_3.txt`, same run continues) - honest answer is "not obviously, and here's why that might not be a contradiction."** Their guessed timestamp (`t=2923625`) is a real event, but on its own it's just a modest ~5Hz dip, not a clear match for a sustained -22Hz shift. Two non-exclusive ways to reconcile: the printed `after=` value is only a 50ms-post-trigger snapshot (already established earlier this session as NOT necessarily the final settled value - this capture's own drift hadn't leveled off yet when the window closed), or the "-22Hz, stuck" state could be a slower, separate drift where fast and slow EMAs moved down TOGETHER without ever re-diverging by 5Hz - which the auto-dump system (a pure divergence detector) would never print anything for, no matter how long it lasted. That second possibility is a real blind spot worth keeping in mind. Cadence/correction-gap pattern otherwise holds exactly as the last two files. User is watching live and will send a follow-up once it reverts (or reports if it's still stuck) - that pairing would be the most direct data yet on why a selected value persists. Flagged as in-progress. Full detail in `null_bias_investigation.md`.

**2026-09-15, later still: added 'H', a second independent frequency-watchdog aimed at the case 'K' structurally can't see - a jump that STICKS.** User pointed out that what they're watching on the SDR is the real output sitting off-frequency for a long time (their example: "-22Hz, been there a while"), and asked how to detect that directly. Root cause of the gap: 'K' only fires on fast-vs-slow EMA divergence, and deliberately resyncs slow to fast every time it's read - so if fast and slow ever drift together slowly enough to never re-diverge from EACH OTHER, 'K' goes silent even while genuinely far from where the run started. Fix: a new, PERMANENT anchor (snapped once at boot-settle, never resynced) that the fast EMA is compared against instead. Fires `CONFIRMED STUCK` (with a coarse, always-on 60-second rolling trace of the actual raw `tx_freq` attached - deliberately un-smoothed, so it can't be confused with an SDR/FFT display's own exponential-average "trail") once a deviation has been continuous for 15+ seconds - chosen specifically to sit above how long this session's own data shows an ordinary reverting 'K'-style jump actually lasts, so it doesn't just duplicate what 'K' already catches - plus a periodic "still stuck" heartbeat and a `RECOVERED` print with total duration once it clears. New manual command `'H'` mirrors 'K's on-demand status read. Not bench-tested. Full design rationale in `null_bias_investigation.md`.

**2026-09-15, later still: real-world feedback on 'H' arrived before it was even bench-tested, and it caught a real calibration miss.** User reported the 700Hz tone drifting from -13Hz to +8Hz off nominal and still sitting there - exactly what 'H' was built to catch, but at only ~8Hz off nominal it would have missed the original 10Hz threshold entirely. Lowered `HELD_TRIGGER_HZ` to 5Hz (matching `SLOW_JUMP_TRIGGER_HZ`, the user's own already-established visual-read perceptual floor) - the 15-second minimum duration is what actually filters out ordinary reverting jumps, not the magnitude threshold, so there was never a good reason for it to be higher. The three K events sent alongside all decode within the already-established repertoire (a flat correction, a repeat of the +1200Hz-jump cascade, another flat correction) - nothing structurally new there, but the last correction event is a good candidate for being the actual transition into the "-13 to +8, still sitting" state, extending the "after= is just a 50ms snapshot" finding with a concrete real-world example. Full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: considered and ruled out - USB polling from the PC as the source of the ~153.6s cycle

User asked whether USB polling from the PC could explain the ~153.6s periodicity. Checked directly: a full-codebase grep for every USB/CDC-related symbol found only a one-time boot-enumeration delay, the already-known packet-boundary padding fix, TX-buffer backpressure throttling, and a reboot-comment note about USB CDC teardown - nothing with any periodic character. Combined with the earlier finding that no software timer subsystem exists in this firmware at all, that K/H timestamps come from the ESP32's own internal clock (independent of USB), that the two-tone signal path is entirely internal/tick-driven with no host-data dependency, and that no standard USB/OS polling interval is known to sit near 153.6s at the observed <0.3% precision - ruled out as a likely cause. The strongest single argument: the user has independently seen the same shift on the SDR (real transmitted RF, not just a log artifact), and USB here has no causal path into the actual DDS/phase output, only into diagnostics printing. Offered a concrete, definitive test if wanted: run the board on a separate supply with USB data disconnected for a controlled window, then reconnect and check whether the latched 'K'/'H' state shows the cycle continued through the blackout. Not run yet. Full reasoning in `null_bias_investigation.md`. The ~153.6s mechanism itself remains the top open question.

## 2026-09-15, later still: fresh-run capture decoded - why "-6Hz to +3Hz" didn't trigger 'H', plus a possible diagnostic data-race glitch and a new escalated +1200Hz-jump cascade variant

New boot/run, 6 K auto-captures + 2 'H' status checks over ~625s. All within the already-established repertoire: two more instances of the atypical +1200Hz single-bin jump (400->1600Hz), each followed by its usual ~8.14s flat correction, and the ~153.6s periodicity reconfirmed across 3 more up-to-up gaps (153655/153672/153672ms) - notably on a brand-new boot, meaning the cadence re-establishes from scratch each run rather than being tied to some absolute time-since-power-on.

**Directly answering "why didn't H trigger for -6 to +3Hz"**: every `fast` EMA value actually recorded in this log - both 'H' status snapshots and every K correction's settled value - stayed within about -3.1Hz to +3.1Hz of the anchor. It never reached the 5Hz bar, so 'H' correctly had nothing to fire on. The "+3Hz" half matches a real measured event (+3.08Hz); there's no clean match for "-6Hz" in the internal data, which leaves two honest possibilities: the visual SDR read overstated the dip, or 'H' (built on the same `fast` EMA as 'K') inherits the same open "is freq_dev_hz measuring what the SDR actually shows" concern raised earlier this session - not resolved here.

**Two new items flagged, not yet investigated further**: (1) one 'H' trace showed a single isolated 0.5s sample spiking to ~719Hz sandwiched between two flat plateaus - too fast to be a real transition by every other pattern seen this session, more likely a torn/non-atomic cross-core read of the `fast` EMA float; worth checking whether that variable needs volatile/atomic-safe access. (2) two of the +1200Hz-type jumps this run showed cascade tails climbing past the usual 1600Hz ceiling into the 1900Hz range - the highest values seen yet; not enough data to call it a distinct new type. Full detail in `null_bias_investigation.md`.

## 2026-09-15, later still: first direct SDR-vs-'fast' comparison in two-tone mode, at rest - checks out

User confirmed the SDR read "exactly on freq" while `'H'` showed `fast=703.41Hz` - the first real two-tone-mode comparison (as opposed to the earlier `'s'`-tone calibration check, which doesn't exercise the beat-null-crossing conditions this session's null-bias work is about). A ~3.4Hz reading counting as "on freq" fits the already-established 5Hz trigger bars nicely and confirms `fast` isn't obviously biased while sitting quiet at nominal. Doesn't yet answer whether `fast` stays trustworthy during/right after an actual jump - that's the harder, still-open half of the question, and the natural next data point if the user can catch an `'H'`-vs-SDR read during a live jump rather than only at rest. Full detail in `null_bias_investigation.md`.

## 2026-09-15, later still: cross-core race audit of the real DSP path (not just diagnostics) - two new hazards found, one old suspicion reopened

User asked whether any variables the actual DSP/filter code relies on (not just the diagnostic ring buffer) could suffer the same kind of cross-core race. Delegated a full audit: `dsp_task` (all per-sample processing) runs IRAM_ATTR on Core 0, `loop()`/serial commands/diagnostics run on Core 1, and there are no locks anywhere in the project - just bare volatile scalars. Found two genuinely new hazards sitting directly in the transmitted-signal path: `envelope_output.cpp`'s PWM offset/scale pair (read together every sample, written as two separate unsynchronized stores, including back-to-back during preset load) and `test_signals.cpp`'s tone-frequency/amplitude pairs (same pattern, read every sample by the two-tone generator that feeds straight into the freq_dev_hz chain this whole investigation is about). Also reopened a suspicion the project already had: the `envelope_gdeq`/`ssb_dsp` EQ coefficient-reinit pattern was flagged back on 2026-09-09 as a possible source of preset-switch weirdness, and a canary was added to check it - but that canary only checks `isfinite()`, which can't catch a real-but-torn (mismatched coefficient + stale filter state) value, so the earlier "no mismatch found" result doesn't actually clear this hypothesis. Caveat: all of these produce at most a single bad DSP tick (sub-millisecond) if they fire, which doesn't obviously explain the multi-second SUSTAINED jumps 'K'/'H' are built to catch - better candidates for an occasional brief glitch than for a held state. Full detail and file/line references in `null_bias_investigation.md`.

## 2026-09-15, later still: biggest SDR-vs-diagnostic mismatch yet - "-35Hz, still there" but 'H' shows +1.85Hz for the prior 60 seconds

User reported a sustained -35Hz jump, ran 'H' right after seeing it - and both 'H's live status (dev=-3.74Hz) and its 60-second rolling trace (flat at +1.85Hz the whole way back) show nothing remotely close to -35Hz. Every previous "didn't quite match" case this session was single-digit Hz; this one is off by an order of magnitude, and it's the first time BOTH of H's independent readouts (live status and trace) agree there's nothing there. Two possibilities, not yet resolved: either there was enough of a timing gap between seeing the jump and actually sending 'H' that the real excursion had already reverted and scrolled out of the 60s trace window (plausible, given how fast other transitions this session revert), or there's a genuine, previously undemonstrated gap between what freq_dev_hz measures and what's actually being transmitted during a real held jump - which would mean 'K'/'H' can't be trusted to catch every real event. The decisive test: next time, send 'H' within a couple seconds of seeing a jump and note the gap. If it still shows nothing, this becomes the top-priority open thread - bigger than the periodicity question, since it would mean this session's whole diagnostic apparatus has a real blind spot. Full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: -35Hz mystery sharpens (no K auto-capture either), and 'H' output now carries timestamps

User clarified two things about the -35Hz report: the K event right before the 'H' read (the correction at t=1880773, interleaved mid-print with the H command) means the true H-vs-event gap was provably tiny, not a long unaccounted delay - and no separate K auto-capture fired for the -35Hz event at all, which K's design shouldn't be able to miss silently (auto-dump fires every loop() iteration once a trigger latches). Both facts weaken the "just read too late" explanation and strengthen the possibility of a real, demonstrated disagreement between what freq_dev_hz measures and what the SDR shows at the same moment - though it's still not fully pinned down which exact K print corresponds to what the user saw.

**Fixed the actual complaint**: added explicit `t=%ums` timestamps to every 'H'-related print (the on-demand status read and all three auto-fired CONFIRMED STUCK/still-stuck/RECOVERED lines), directly comparable to K's own timestamps. Next time a jump and an H read happen close together, the exact gap between them will be computable from the log directly instead of inferred from how the serial output happened to interleave. Not bench-tested yet. Full reasoning in `null_bias_investigation.md`.

## 2026-09-15, later still: strongest lead yet - envelope-domain filter memory, raced cross-core, invisible to K/H by construction

User pushed back hard on the accumulating hedging: they've reported sustained held states multiple times and the internal data has never shown it - "we are not looking at the same reality" - and argued the recurring jump magnitudes look mechanistic/numeric, not random noise. They proposed a cross-core race corrupting something with real MEMORY (not freshly recomputed each tick), which could explain a state that stays wrong rather than self-healing in one sample.

Checked this against the code directly. The phase/frequency path itself (Hilbert delay line, phase accumulators, freq_dev slew-limiter) turns out to be single-core-owned and not raced - that part of the theory isn't supported. But the envelope-domain IIR filters (`envelope_gdeq`'s allpass memory, `envelope_ampeq`'s shelf biquad memory) ARE raced in exactly the shape described: Core 1 non-atomically reinitializes multi-field filter state while Core 0 keeps running the filter live every tick. Unlike the earlier races, this state genuinely carries forward as feedback, so a bad value doesn't automatically self-heal. Critically, because it's envelope-side, it would never show up in `freq_dev_hz`/K/H at all - and this project's own design notes already anticipated a path from envelope corruption to something that LOOKS like a frequency artifact on a receiver (AM-to-PM crosstalk, explicitly named in a settings.h comment as the explanation if FM-looking sidebands ever appeared with freq_dev_hz staying clean).

Whether the corrupted state is merely stale-but-finite (which would slowly decay via the filter's own math, over seconds) or truly poisoned (NaN/Inf, which never recovers on its own) matters a lot, and there's already a canary check in this codebase (added 9/11, runs every loop iteration) that would print a MISMATCH the first time the filter state goes non-finite - I don't recall ever seeing that line in a shared log, which argues against the sharpest version of this theory but not the milder one. Proposed concrete test: run `'c'` right after/during a live stuck event to check directly. This is now the strongest, most concrete lead in the whole investigation - full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: the theory completes - envelope masking/unmasking of the already-known near-null instability

User confirmed never seeing a bad canary, and asked a sharp question: is the RF output frequency essentially a power/energy-weighted function of freq(t) and envelope(t), such that an amplitude error alone (no real frequency error) could produce an apparent frequency shift? Answer: yes, and it completes today's theory. An SDR/receiver's frequency-domain view inherently weights each instant by its envelope-carried energy; freq_dev_hz/fast/K/H are a plain, envelope-blind average with no such weighting. This session already established freq_dev_hz is genuinely unstable near beat-envelope nulls, but normally invisible because envelope is naturally near-zero right when that happens - so it contributes almost no energy to what's actually transmitted. If the envelope-EQ filter state gets corrupted (the cross-core race flagged in the previous entry) and stays elevated instead of dipping during a null, that already-existing near-null excursion suddenly gets real energy weight in the output - explaining a real SDR-visible frequency shift with freq_dev_hz never actually being wrong, why the canary (isfinite-only) would never catch a merely wrong-but-finite envelope value, why the same quantized magnitudes keep recurring (unmasking an already-characterized deterministic pattern, not fresh noise), and why it can persist for seconds (filter state, not an instantaneous glitch). This is now the leading, most complete theory in the investigation - not yet directly confirmed, but consistent with everything found today. Full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: traced whether the null-region code preserves the joint freq+ampl relationship - it doesn't, and that's confirmed by the project's own history

User asked directly how the null "correction" code ensures the joint integral of frequency and amplitude corrections is right, given today's energy-weighting theory. Traced the actual pipeline: freq_dev_hz and envelope are generated together, then envelope alone passes through three independent envelope-only stages (envelope_floor, gdeq, ampeq) with zero corresponding adjustment to freq_dev_hz - only afterward does relative_delay bring them back together, purely for timing alignment, not amplitude/phase consistency. Nothing anywhere checks that the pair stays physically sane near a null.

This isn't an oversight - envelope_floor.cpp's own NOTE 1/NOTE 2 history shows the original design DID try correcting both sides together, found the frequency-side fix was actively wrong (removed it), and kept only the envelope-side fix. Per today's theory, envelope_floor/gdeq/ampeq - each independently, whenever engaged - can break the pairing between near-zero envelope and the phase's genuinely-required fast rotation at a null, which is exactly the shape of mismatch that would inject real spectral energy at an otherwise-masked frequency.

Scope caveat: all three (envelope_floor, gdeq, ampeq) default to off/no-op, and gdeq/ampeq fully no-op when disabled (so the earlier-flagged cross-core race in their reinit can't matter unless one is actually toggled on). Asked the user directly whether 'g'/'a'/'A'/'x' were active during the sessions where jumps were observed - this determines whether this mechanism is currently live or whether the search should stay on the phase/timing side instead. Full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: null depth varies and correlates with K triggers (user's direct scope observation) - checked a drift theory numerically, found and fixed a real print-precision blind spot

User directly watched the envelope waveform and reports the null's minimum level visibly varies over time and is "pretty sure" the deepest nulls are what trigger 'K' - which makes physical sense (a deeper null requires a more extreme phase swing to represent the zero-crossing, producing a bigger freq_dev_hz spike). They also confirmed all three envelope filters ('g'/'a'/'A') are ON and suspect filter-induced sample jitter explains why the null doesn't land at a fixed sample position cycle to cycle.

Tested one candidate explanation numerically: float32 rounding drift in the two-tone oscillators' own phase accumulators (a real, deterministic, per-boot-reproducible phenomenon that would fit the "resets identically each boot" fact already established). Simulated it directly - the actual drift rate implies a full-cycle period of ~2,460-6,300 seconds depending on which quantity you look at, 15-40x too slow to explain the observed ~153.6s cadence. Reported this honestly rather than force-fitting it. Since the beat period is an exact 16-sample integer, an unfiltered/undelayed signal would show zero cycle-to-cycle null jitter at all - so the jitter the user sees is itself evidence pointing at the same envelope filters already flagged today for a cross-core race and for having no mechanism to keep them consistent with the phase path near a null.

Also found and fixed a genuine, months-old blind spot: env_min has been printed at 3 decimal places this entire session, and since real null depths sit well below 0.001, every env_min value in every log ever shared has printed as an uninformative flat 0.000/0.001 - the print statement was hiding exactly the data needed to test the user's hypothesis. Bumped both print sites (K trace bins, jump log) to scientific notation. Not bench-tested. Next capture should finally show real env_min variation. Full writeup in `null_bias_investigation.md`.

## 2026-09-15, later still: K_out_4.txt (54 events, 72 minutes) - env_min fix confirmed working, null-depth hypothesis proven with hard numbers, and the active-filter list for this capture ruled out almost entirely

First capture taken since yesterday's env_min print-precision fix, answering "anything new?" directly: yes, a lot. The 54 K auto-captures split into two tight, reproducible clusters - 25 "deep" events (env_min~0.0016, always a big +15 to +24Hz jump toward a ~1000-1080Hz band) and 26 "shallow" events (env_min~0.090, always a smaller -5 to -14Hz dip back toward ~700Hz), plus 3 "solo" events where the null came up shallower than usual and only just cleared K's minimum threshold. This directly confirms, with numbers instead of a scope-side impression, the user's own hypothesis: deeper null -> bigger, positive-going K event; shallower null -> smaller, negative-going one.

The ~153.6s periodicity is now measured to within 0.03% (153,633-153,684ms across 24 gaps) and turns out to be a two-phase cycle, not a single event: every "deep" event is followed 8136-8154ms later by its paired "shallow" event. That level of clockwork regularity over more than an hour points at a deterministic numeric/timing mechanism rather than aperiodic race-condition corruption.

Checked which envelope filters were actually engaged during this specific capture, from the log's own command trace: gdeq, both ampeq stages, the pre-Hilbert EQ, and the compressor were all confirmed OFF - directly contradicting "all on" and ruling out, for this data, every filter-race hypothesis built up over the last several entries. What WAS active - envelope pre-distortion (`'D'`) - turned out to be a stateless lookup table (can't itself cause a slow drift) but a crucial interpretation lens: `env_min` is measured AFTER this table, inside its steepest bin, and inverting it shows the real raw-envelope null depths are ~0.005% ("deep") vs ~0.79% ("shallow") - a genuine ~150x difference, not a display artifact. This significantly narrows what's left to investigate: the two-tone generator itself and `ssb_dsp_process_sample()`'s own Hilbert/DC-block math are now the two remaining candidates for the ~153.6s pattern's root cause. Full numbers, LUT inversion math, and file/line references in `null_bias_investigation.md`.

## 2026-09-15, later still: user confirms filters-off was deliberate; traced the exact phase-to-freq_dev math and confirmed near-null noise is currently unmitigated

Confirmed the previous entry's filters-off state in `K_out_4.txt` was an intentional A/B test, not an accident. User then asked exactly how phase discontinuities get resolved and how phase converts to frequency. Traced `ssb_dsp_process_sample()` directly: `wrap_pi()` does standard phase unwrapping on the atan2-derived phase difference (picks the smallest-magnitude equivalent step, valid as long as real content stays under Nyquist/2), then `freq_dev = dphi * sample_rate_hz / (2*pi)` is a plain scaled phase-difference calculation.

The real finding is in the uncertainty question: near a null, tiny I/Q values make the phase angle hypersensitive to ordinary signal noise, and the code does nothing in the phase math itself to correct for that. Two downstream mechanisms could limit it - a slew-rate limiter on freq_dev (its own doc comment cites ~60Hz/sample for real content vs ~8000Hz/sample at a genuine null) and a magnitude clamp (20000Hz) - but the slew limiter is off by default in every preset checked so far, and the magnitude clamp is confirmed mathematically unable to ever bind on a single-sample event (wrap_pi's own pi bound already caps any single-sample freq_dev at 8000Hz at 16kHz, under the 20000Hz clamp). So near-null phase noise currently reaches freq_dev completely unmitigated - deliberate, per this project's own established history of trying and reverting a freeze-based fix. Concrete next experiment identified: deliberately engaging the slew limiter and checking whether it reduces K/jump-log magnitudes. Full trace and numbers in `null_bias_investigation.md`.

## 2026-09-15, later still: is there a resolution bias on I/Q near zero? Yes - quantified, and it's float32 cancellation noise, not ADC quantization, at nearly the same scale as the "deep" null cluster

User asked whether I/Q suffer a resolution bias approaching zero. Classic ADC-style quantization doesn't apply to any capture examined so far - two-tone test mode bypasses the ADC entirely (pure float32 synthetic generation). What does apply is floating-point catastrophic cancellation: both I (2-term sinf sum) and Q (65-tap Hilbert FIR sum) are computed as sums of much-larger terms that nearly cancel right at a null. Simulated both directly against float64 ground truth using the real filter coefficients: I's rounding floor is negligible (~1e-8), but Q's is not - median ~2.8e-5, 95th percentile ~1e-4, and in ~5% of near-null instants the rounding error actually exceeds the true value.

This lands in the same order of magnitude as the "deep" null cluster's back-computed raw envelope (~5e-5) from the K_out_4.txt decode two entries ago - meaning at the very deepest nulls actually observed, env_min may partly be measuring float32 arithmetic noise rather than a clean physical null depth, which is a plausible contributor to that cluster's ~15% cycle-to-cycle spread. Doesn't apply to the "shallow" cluster (2 orders of magnitude above this floor - a real physical value). Sharpens, rather than replaces, the still-open periodicity/null-depth-variation question. Full simulation numbers and methodology in `null_bias_investigation.md`.

## 2026-09-15, later still: is the Hilbert transform IIR (can errors lock in)? No - confirmed FIR, errors are self-limiting to a two-sample doublet

Checked whether the Hilbert transform is IIR (where the just-quantified cancellation rounding noise could persist). It's pure FIR - 65 fixed taps, plain weighted sum over raw input history, no feedback term anywhere - so every Q value is recomputed fresh each sample and a rounding-noise flicker vanishes completely once the relevant samples age out of the 65-tap window. Contrasts sharply with the already-flagged `envelope_gdeq`/`envelope_ampeq` IIR filters, whose real feedback state is exactly why THOSE can "stay corrupted" if raced. One nuance: `prev_phase` carries one sample forward, so a single bad phase value leaks into two consecutive freq_dev outputs as an equal-and-opposite doublet - ordinary derivative-estimator behavior, not feedback, and self-vanishing after that. Net: the earlier resolution-bias floor is real but transient, not a sustained corruption. Full reasoning in `null_bias_investigation.md`.

## 2026-09-16: decisive result - relative_delay-induced 20Hz jump is PROVABLY invisible to H/K; energy-weighting theory now fully proven, not just consistent

Two observations. First, watching both tones independently on the SDR, their separation has stayed a constant 1kHz - never the "upper up/lower down" spread the user expected from a jump. This is fully consistent with theory: a genuine sustained frequency bias is a pure spectral translation (Fourier shift theorem), shifting both tones by the same amount together - good corroborating evidence.

Second, and decisive: the user can reproducibly induce a real, sustained ~20Hz SDR-visible shift via the `'['`/`']'` relative_delay scan, hold it indefinitely, and `'H'` always reports ~700Hz nominal regardless - a controlled, repeatable experiment, not an ambiguous spontaneous event. Traced the full mechanism and it's airtight: H/K's fast/slow EMAs are fed by `delayed_freq_dev_hz`, which `relative_delay_apply()` produces via plain linear interpolation between two adjacent entries of the SAME raw freq_dev_hz ring buffer. Since that raw signal is periodic/stationary, blending its own nearby samples at any lag cannot change its own long-run average - proving H/K are structurally, permanently incapable of reflecting a relative_delay-induced shift, no matter how large or how long held. Meanwhile envelope is read at a DIFFERENT lag than freq_dev, so what `'['`/`']'` actually does is mis-time the envelope/freq_dev pairing in the real transmitted signal - a genuine energy-weighting effect that never touches freq_dev_hz's own statistics.

This completes (not just supports) the energy-weighting theory from several entries back with a full, code-verified proof from a controlled experiment. Implication: any real spontaneous jump whose root cause is a timing/pairing mismatch between envelope and freq_dev - i.e. every mechanism floated this session - would be exactly as invisible to H/K as this deliberately-induced one. Resolves the earlier "we are not looking at the same reality" disagreement with a concrete mechanism rather than an argument. Next step identified: build a real energy-weighted diagnostic from the already-present but never-read-back `env2_dphi_sum`/`env2_sum` accumulators in `ssb_dsp.c`. Full trace in `null_bias_investigation.md`.

## 2026-09-16: found a distinct candidate for the hands-off jumps (AD9851 bit-bang timing margin, currently disabled) and implemented the post-delay energy-weighted 'H' fix

User correctly cautioned against assuming the `relative_delay` blind-spot proof explains the spontaneous hands-off jumps too - `relative_delay` is a fixed setting hands-off, so a real spontaneous cause needs its own mechanism. Found one already sitting in `AD9851.c`'s history: `AD9851_BITBANG_EDGE_DELAY_ENABLED` (the settle-margin protecting against data-dependent bit errors during large single-tick FTW jumps - the code's own comment already names this "the leading theory for the two-tone-specific 'sticks' symptom") is currently DISABLED, reverted 2026-09-11 pending a re-scope that never happened. The exact comparison test needed (restore the margin, run a hands-off comparison) was planned in a 2026-09-11 comment and never completed - and that same comment records a prior hands-off "-45Hz jump... digital chain otherwise clean" event this mechanism would explain. This is a genuinely different failure mode from yesterday's finding: a physical bit-level SPI transmission error at the AD9851 itself, invisible to every software diagnostic (including a corrected energy-weighted one) since none of them read back what the chip actually latched. Concrete next step identified (needs the user's bench, not firmware): re-enable the edge delay and re-run the abandoned hands-off comparison.

Also implemented the diagnostic fix the user agreed to: a proper post-delay, envelope^2-weighted EMA added to `'H'`'s output, fed from `delayed_freq_dev_hz` and the correctly time-matched `envelope_at_freq_time` (not `delayed_envelope`, a trap the project already caught once before on 2026-09-12). This completes a fix proposed back on 2026-09-12 and never built, and directly addresses why the existing `weighted_bias` diagnostic never tracked real jumps (it was accumulating pre-delay, blind to relative_delay pairing exactly like plain `fast`/`slow` are). Not bench-tested. Full implementation details and reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: first bench test surfaces a stale config.h AND catches a real, sustained, hands-off jump with numbers attached

First bench report on the new energy-weighted `'H'` line: "SDR sitting at -22Hz and H reports 696." Chasing down which field "696" referred to turned up something bigger: this repo's `config.h` had `TWOTONE_F2_HZ` at `1900.0f` - the legacy, non-Fs-commensurate pair the 2026-09-09 entry already recommended retiring - not `1700.0f`. Git history traced it to commit `8fd8349` (2026-09-12, "Add dither on tone2... current 2tone set to 700/1900"), which silently reverted `de32ffd`'s earlier fix. The user confirmed their actual bench config is 700/1700; fixed `config.h` to match and documented the history so it can't drift again unnoticed. Also found `test_signals.cpp`'s `'T'` band-preset table has no 700/1700 entry at all - that pair is only reachable via the compile-time default, before any `'T'` press.

With the tone pair confirmed, a second, genuinely new finding fell out of the same bench session: two "always-on" diagnostics turned out to be aliasing artifacts against the exactly-periodic two-tone signal, and a real, sustained, hands-off jump got caught with full before/after numbers in a freshly-uploaded capture (`K_out_5.txt`). `held_trace`'s `tx_freq` column looked "stuck" near a single value for 60 seconds straight in two separate captures - not a frozen DDS, but its 500ms sample interval (8000 ticks) being an exact multiple of the 700/1700Hz pair's 16-tick period, so every snapshot lands on the identical phase of the repeating waveform. The fine `slow_trace` bins (20-tick, 1.25ms) show the same effect at a different scale - a suspiciously clean period-4 alternation between exactly 400.0Hz and 800.0Hz, explained by `LCM(16,20)/20 = 4` distinct bin-phase alignments.

The real find: `K_out_5.txt` contains a slow-jump auto-capture (`t=769436ms`, `before=700.44Hz after=1063.38Hz delta=+17.74Hz`) with `relative_delay` unchanged for the preceding 731 seconds - ruling out yesterday's relative_delay-pairing mechanism for this specific event. Working the fast EMA's own known time constant backward (the 40-post-bin capture window is exactly one fast tau) implies `freq_dev` sat near a ~1264Hz plateau for at least 50ms - a sustained excursion, not a single bad phase sample (a maximal single-tick spike, capped by `wrap_pi()` at 8000Hz, could only move `fast` by ~9Hz in one tick; this moved it by ~345Hz). This also explains two earlier, smaller "glitch" values the user separately spotted in `held_trace` snapshots (722.55Hz and 759.61Hz, each ~30-60Hz above baseline in a single 500ms-spaced sample) - too large for a single tick by the same arithmetic, so almost certainly smaller/shorter instances of the same underlying phenomenon that this coarse trace only occasionally catches mid-excursion. Full derivation, numbers, and the two competing explanations (AD9851 bit-bang latch vs. a DSP-level cause) in `null_bias_investigation.md`.

Also fixed a design flaw in the energy-weighted `'H'` line itself, caught on this same first bench read: its `dev` was being measured against the plain EMA's anchor, producing a large, static, meaningless offset (+410Hz) with no connection to any real event. Gave it its own anchor, seeded the same way at boot-settle.

## 2026-09-16, later still: first real-world hit on the energy-weighted 'H' fix (-38.37Hz vs SDR's -38Hz), a harder second data point, and the ~153.6s cycle reconfirmed with new numbers

`K_out_6.txt`'s first manual `'H'` read matched the user's independently-read SDR value almost exactly: energy_weighted `dev=-38.37Hz` against an SDR reading of -38Hz, while the plain line showed essentially nothing (+1.23Hz) at the same instant - the first real-world confirmation that yesterday's energy-weighted fix tracks what the SDR actually sees, not just an internally-consistent number. A second read 23 seconds later (after an untimed coffee-break gap) showed energy_weighted `dev=+63.65Hz` against an SDR reading of only +8Hz - a real mismatch, but not an unexplained one: pulling every auto-captured slow-jump event in the file showed a full rise/recovery pair happened in the gap between the two reads, and the second read landed 12 seconds after that recovery had already completed by the plain fast/slow measure. Since the energy-weighted EMA shares the plain one's exact time constant, a 63Hz gap surviving 12 seconds (240 tau's) after the plain signal fully settled can't be the same event just decaying slower - either a separate pairing-type deviation was still active, or the SDR was glanced at a few seconds off from the keystroke. Recommended follow-up: read `'H'` and the SDR together (seconds apart, not a coffee break) at several points across one full cycle, to get a clean correlation table.

Separately, the timeline of auto-captured events reproduces the ~153.6s/~8.15s two-phase periodic cycle originally found in `K_out_4.txt`, independently, with even tighter precision this time (rise-to-rise intervals of 153671ms and 153673ms; recovery lags of 8141/8143/8147ms) - strong cross-validation that whatever drives this cycle is still active and highly regular. Full timeline, numbers, and reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: user spots 8Hz-spaced SDR sidebands (elevated at 40Hz) - found an exact 40Hz match in firmware, and it may be the diagnostics themselves

User reported 8Hz-spaced sidebands around the 700Hz tone on the SDR, with the 40Hz (5th harmonic) elevated, and noted recent shift readings like -38Hz sit close to that 40Hz line given their own measurement uncertainty. A full codebase search found no 8Hz source anywhere, but a very clean match at 40Hz: `EMA_TREND_SAMPLE_TICKS`=400 ticks = exactly 25ms = 40Hz - the trend-ring bookkeeping added earlier today for `'K'`'s trend printout, which runs unconditionally every tick inside the hot DSP path, after that tick's own PWM/AD9851 writes but potentially eating into the NEXT tick's timing budget once every 40th-of-a-second. If real, this means the diagnostic instrumentation built to investigate hands-off jumps may itself be introducing a small periodic artifact into the transmitted signal - ironic, but a legitimate, traceable mechanism, not a stretch. Scoped to the smaller ~20-60Hz-class wobbles only - nowhere near big enough to explain the large auto-captured jumps already logged. No 8Hz firmware source found; one inactive-by-default candidate (`'Q'` dither's 4Hz update, 2nd harmonic at 8Hz) worth ruling out at the bench. No firmware change made yet - two concrete, cheap tests proposed (check dither state; try gating the trend-ring write off the hot path) pending the user's go-ahead. Full trace and reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: correction - the recurring ~153.6s "rise/recovery" auto-capture is very likely a null-noise artifact of the plain EMA, not a real transmitted jump

`K_out_7.txt` was `K_out_6.txt` continued (same un-cleared log, confirmed byte-identical prefix). The user watched the SDR through the extra ~23 minutes and saw no more than +/-4Hz of real movement, despite 12 more occurrences of the exact same ~153.6s rise/recovery pattern already logged several times this session - every single one showing `env_min` pinned at a genuine near-total null. This walks back the `K_out_5.txt` entry's conclusion that this pattern represents a "genuine, sustained, hands-off jump" needing a hardware explanation: comparing the one occurrence that DID coincide with real SDR movement (the -38Hz/+8Hz episode) against the 12 that didn't, the numbers are statistically indistinguishable - nothing in the auto-capture's own before/after/delta marks the "real" one as different. Most consistent explanation: near-null moments can swing the plain (unweighted) instantaneous-frequency average by hundreds of Hz while carrying almost no real transmitted power, exactly the failure mode the energy-weighting theory already predicts, and an SDR (reading actual radiated power) correctly doesn't see it. This de-prioritizes the AD9851 bit-bang theory as an explanation for THIS specific recurring pattern (a hardware bit-latch fault would affect the real output every time, not 1 time in 13) - the bit-bang theory and the still-unexplained rarer real events (the original -22Hz report, and whatever actually caused -38/+8 if not this pattern) remain open separately. Proposed next step: have a future auto-capture also print the energy-weighted reading alongside the plain one, so a real event can be told from an artifact without a coincidental manual `'H'` press. Full numbers and reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: built the proposed fix - auto-captures now print an energy-weighted before/after, and a new independent detector auto-logs energy-weighted holds/jumps

User confirmed both parts of the previous entry's proposal. Implemented in `diagnostics.cpp`, not yet bench-tested:

**Auto-capture printout**: every `AUTO-CAPTURED`/`TRIGGERED` slow-jump event (from `'K'` or automatic) now also prints an `energy_weighted before=... after=... anchor=... anchor_dev=...` line, computed at the same trigger-arm and post-capture-close instants as the existing plain before/after. This means a real event vs. a null-noise artifact (the distinction the previous entry needed 13 events and a statistical comparison to establish by hand) can now be read straight off a single future capture.

**New independent detector**: a full second copy of the existing `'H'` held-freq detector (`s_energy_held_*`, mirroring `s_held_*`'s state machine exactly), but keyed off the energy-weighted `dev` (`energy_weighted_fast_hz() - s_freq_energy_anchor_hz`) instead of the plain fast/anchor pair. Reuses the same tuned thresholds (`HELD_TRIGGER_HZ`=5Hz, `HELD_MIN_DURATION_MS`=15s, `HELD_REANNOUNCE_MS`=30s) rather than inventing new ones. Prints three new, clearly-labeled messages: `ENERGY-WEIGHTED CONFIRMED STUCK` (plus a `held_trace` dump), `ENERGY-WEIGHTED still stuck` (30s heartbeat), and `ENERGY-WEIGHTED RECOVERED`. Since this is keyed off the same anchor/function the corrected `'H'` line uses, a confirmed hold here should mean real transmitted power, not a plain-EMA null-noise wobble.

Brace/paren balance re-verified (155/155, 1133/1133) after all edits. Full code and reasoning in `null_bias_investigation.md`. Next bench capture should exercise both new printouts alongside a simultaneous SDR reading.

## 2026-09-16, later still: log_20260916_111946.txt has no diagnostic output - just a boot config dump that decodes to preset 1, not preset 3 as intended, but the mismatch itself is a useful data point

The uploaded file is one line - a `'P'`-style config dump, no `'H'`/`'K'`/held/auto-capture output at all - so there's nothing internal to cross-check this session's SDR readings (+3Hz, -7Hz, +2Hz) against. Decoding the dumped struct against `settings.h` confirms the user's own suspicion: it's byte-for-byte `settingsPresets[1]` ("TwoTone Base"), not preset 3 ("Shelf2 Baseline gdeq adj#4") - printed under the name "Live" since no preset had been loaded yet this boot.

The fields that differ between preset 1 (this run) and preset 3 (normal) are exactly the envelope-shaping/timing chain this session has repeatedly implicated in envelope/freq_dev pairing mismatches: `relative_delay_samples` (0.00 vs 2.00), `env_gdeq_enable` (off vs on), `env_predistort_enable` (off vs on), `env_ampeq_enable`/`env_ampeq_shelf2_enable` (both off vs on), and `master_gain_db` (-2.0 vs -1.4, unrelated to timing). With all of those off, the SDR shifts reported (+3/-7/+2Hz) were much smaller than prior preset-3 sessions' -38Hz/+8Hz reads or the 700->1000+Hz auto-captured jumps.

Treated as a data point, not a conclusion (three numbers, no trace, could easily be a shorter session or one that missed the ~153.6s cycle's rise phase by chance) - but it's the first natural A/B this session has seen on the exact mechanism the pairing-mismatch theory depends on. Recommended follow-up logged in `null_bias_investigation.md`: repeat this same preset-1/default config for a longer session with the log actually capturing `'H'`/`'K'`/auto-capture output, and compare directly against an equal-length preset-3 run.

## 2026-09-16, later still: the real version of that capture arrives - the new ENERGY-WEIGHTED detector shows the ~153.6s cycle is a smooth ~145s climb, not a sudden event (biggest finding of the session so far)

Same filename re-uploaded with its actual content this time (1365 lines). First real bench test of the two diagnostic pieces built earlier today. Three complete ~153.6s rise/recovery cycles are captured, matching the known cycle length almost exactly (145.0-145.1s "held" + ~8.6s "quiet" each time) - nothing new there. What's new: the independent energy-weighted held detector shows that between one "recovery" and the next "rise," the energy-weighted deviation from its own anchor doesn't sit flat waiting for a sudden event - it **climbs smoothly and continuously the entire ~145 seconds** (roughly +34Hz at 45s in, +50Hz at 75s, +64Hz at 105s, +78Hz at 135s, peaking at +120-133Hz right at the next rise), while the plain fast/slow EMA pair sits completely flat the whole time. That means whatever drives the well-known recurring cycle isn't a single instantaneous event - it's a slow, continuous process spanning almost the entire gap between events, invisible to the plain (unweighted) diagnostics and only now visible through the energy-weighted lens.

No SDR reading was taken during this capture, which is now the single most important missing piece: if the SDR shows a matching smooth climb over the same ~145s, this is a genuine and previously-invisible real transmitted-frequency drift (a major finding). If the SDR stays flat (as it did for `K_out_7.txt`'s null-noise events), the climb is a diagnostic-side artifact and the new detector's thresholds (currently reused from the plain detector, and effectively "triggered" 94% of the time under this config) need retuning. Proposed a concrete hypothesis for the real-drift case (a slow few-PPM beat between two independently-clocked periodic processes, with ~153.6s as its own beat period) - clearly flagged as unconfirmed pending that SDR check.

Also traced, with code (not guesswork this time), why `env_min` pinned at ~0.2 instead of near-zero under this preset: `envelope_at_freq_time` is captured *after* the active preset's envelope-shaping chain, and preset 1's linear offset/scale mapping (`envelope*0.90+0.20`) floors even a true DSP null at 0.20 - a structural property of the diagnostic, not a bug, but a reason energy-weighted numbers aren't directly comparable across presets with different shaping chains. Full numbers, table, and reasoning in `null_bias_investigation.md`. No firmware change made - next step is a bench SDR check, not a code change.

## 2026-09-16, later still: the SDR check comes back - the smooth ~145s climb was NOT real. Hardware-clock-beat theory de-prioritized; the preset-1 offset floor is the leading explanation instead

User confirms the SDR moved no more than a few Hz ("+3 -7 +2") across the entire session just logged - the three cycles where the energy-weighted metric climbed smoothly to +120-133Hz each time. So that climb has no real-world counterpart: it's a diagnostic-side phenomenon, not a genuine transmitted-frequency drift. This rules out the previous entry's "slow hardware clock beat" hypothesis as the main explanation (a real clock beat would show up on the SDR) and points back at the offset-floor finding from the same log: under preset 1, the envelope value used to WEIGHT the energy-weighted average never drops below 0.20 even at a genuine DSP null, while the underlying `dev` (frequency deviation) math still has its usual large null-driven excursions regardless of preset - so near-null noise that's supposed to get suppressed by near-zero weight instead leaks in at ~4% weight. Contrasted against `K_out_6.txt`'s clean -38.37Hz-vs-SDR's-38Hz match (very likely captured on preset 3, where predistort allows a true near-zero weight), this suggests the energy-weighted diagnostics are only trustworthy on presets without a nonzero linear PWM offset floor.

Practical takeaway: don't trust `ENERGY-WEIGHTED CONFIRMED STUCK`/`still stuck` messages on preset 1 (or similar offset-without-predistort presets) as real-event indicators - they fire almost continuously regardless of what the SDR shows. Recommended next step: repeat the same test on preset 3 with a running SDR check, to confirm the detector behaves properly there. Still unexplained: why the leak produces a smooth 145-second climb rather than settling immediately - flagged as open rather than guessed at. No firmware change made. Full reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: the requested preset-3 comparison arrives with manually-logged SDR readings - and turns up a completely new, striking finding: `held_trace`'s own `tx_freq` shows a discrete ~8000Hz "phantom" shift lasting one full ~153.6s cycle, confirmed fake by three independent checks

User ran the requested preset-3 test with the SDR watched live, logging a reading whenever it moved by roughly 4Hz or more ("if SDR is not logged, it didn't change"). The SDR's whole-session range was tiny - only 24Hz total across ~18 minutes and 7 cycles - consistent with everything else logged about this transmitter's real stability.

While checking the energy-weighted comparison, a bigger and completely unrelated finding turned up: `held_trace`'s `tx_freq` column - the literal value sent to the AD9851, previously assumed to be unimpeachable "ground truth" - sits at a fixed **~8000Hz below normal** for one entire, exact ~153.6-second cycle (three consecutive trace dumps line up end-to-end into one continuous episode, boundary-aligned with the known cycle). Three independent checks all say this never actually happened: the 2026-09-09 corruption canary stays silent the whole file (rules out a corrupted `s_carrier_hz`), `fast`/`slow` stay completely flat the entire time (rules out a real, sustained `freq_dev` change), and the SDR shows nothing at all during or around the window (rules out a real RF event).

Leading explanation: `held_trace` samples at an exact multiple of the two-tone's own period (a fact already known from way back this session - hence its normally "stuck-looking" values), so it always snapshots the identical relative phase of the periodic waveform. A single skipped or double-counted DSP tick could shift that sampling phase onto a different, more extreme point of the cycle (near where a genuine phase discontinuity produces large instantaneous swings) without touching the real, continuously-transmitted signal at all - explaining why nothing else noticed. The episode's start/end line up almost exactly with the well-known recurring cycle's own boundaries, hinting this and the classic rise/recovery pattern might share a root cause (an occasional missed tick), but that's unconfirmed - the log doesn't include the `[timing]` overrun/late-tick counters that would settle it directly.

Also closed the loop on the actual requested comparison: preset 3's energy-weighted readings are smaller than preset 1's (max swing ~75Hz vs ~130Hz) but still show a large, highly consistent, cycle-phase-locked bias unrelated to the real event size (rise events cluster at ~1026-1030Hz, recoveries at ~1145Hz, regardless of that cycle's actual SDR movement) - so the offset-floor theory is probably part of the story, not the whole story. Full numbers and reasoning in `null_bias_investigation.md`. No firmware change made yet.

## 2026-09-16, later still: added overrun/late-tick/max-busy_us data to held_trace, so the next capture can directly test the tick-slip theory

User asked whether the overrun data could be added to the logging - exactly the missing piece needed to confirm or rule out the previous entry's tick-slip theory for the `tx_freq` phantom shift. Added three new columns to `held_trace`'s existing 500ms rolling trace: `overruns`, `late` (late-tick count), and `max_busy_us`, all simple snapshots of counters that already exist and are already exercised every tick - deliberately not new tracking logic, to avoid adding a second novel mechanism right where a bug is being hunted for. All three only ever increase (they reset on a manual `'r'` command, same as before), so a step between one printed line and the next pinpoints exactly which 500ms bin saw a real DSP-timing hiccup. Next time the `tx_freq` phantom shift (or the classic rise/recovery boundary) recurs, lining it up against these three columns will either confirm the tick-slip theory directly or rule it out. Not yet bench-tested. Full details in `null_bias_investigation.md`.

## 2026-09-16, later still: the overrun test comes back clean - tick-slip theory is dead, but the phantom `tx_freq` shift turns out to be deterministic to single-digit milliseconds across two separate boots

Second preset-3 run with the new columns live: `overruns=0` and `late=0` on every one of 1727 samples, no exceptions, including right through the recurring ~8000Hz `tx_freq` phantom episode itself. `max_busy_us` stays comfortably under the 62.5us tick budget throughout (46-59us) and reads identically at the phantom's onset/recovery instants as everywhere else. The missed-DSP-tick theory is now cleanly ruled out.

The much bigger surprise: the phantom recurred with the same ~8000Hz magnitude, and its onset/recovery timestamps matched the previous capture to single-digit milliseconds (`t=615687ms` exactly in both files for the onset side, `t=769365ms` exactly in both for the recovery side) - as did every other cycle boundary across both ~18-minute sessions. That precision rules out a random cause (RFI, a cosmic-ray-style bit flip, a marginal SPI glitch) - it points at something deterministic and keyed to elapsed time since boot, not yet identified (no matching constant found in the codebase). Recommended next step: a much longer capture to see if it recurs again at further multiples of ~615s, and to stop looking at DSP-tick timing (now cleared) and start looking at other independently-timed processes in the firmware or the ESP-IDF/FreeRTOS runtime underneath it. Full reasoning in `null_bias_investigation.md`. No firmware change made this time.

## 2026-09-16, later still: `log_20260916_193030.txt` - new external audio-tone counter strongly confirms the energy-weighted swing isn't real, but its own baseline unexpectedly steps by large amounts every cycle; `tx_freq` phantom recurs a third time, still clean, still boot-locked to the same ms

User's new automated audio-frequency counter (measures the 700Hz tone's offset directly, triggered off the diagnostic log messages) gives the clearest confirmation yet: across all 7 cycles in this ~939s capture, the audio tone stays flat to ~1Hz while `energy_weighted` `dev` swings by 40-85Hz in the same window, and the plain fast/slow `held_freq` detector never fired once in the whole session. Strong new evidence the energy-weighted climb is a weighting-scheme artifact, not a real transmitted event.

New open puzzle, not yet explained: the audio tone's own flat baseline steps to a different value at every `~153.6s` cycle boundary - a sequence of -13.7, +2.0, -75.6, +81.5, -6.5(ish), 0, -9.7 Hz across the 7 cycles - with step sizes up to ~157Hz that don't match any confirmed-real event size and that the plain fast/slow EMA (silent throughout) would have caught if real. Leading candidate: the new counter may have its own version of the same fixed-phase-sampling susceptibility that produces the `tx_freq` phantom below, since it's triggered off the same cycle-boundary log messages - but this isn't confirmed, since there's no visibility yet into how the counter itself works. Recommended: either a description of the counter's own design, or a longer repeated capture to see if the plateau *sequence* itself repeats across boots (would point at the counter) or doesn't (would argue for something real).

`tx_freq` phantom shift recurred a third time (same 3-run signature, 241 lines, same magnitude) with `overruns=0`/`late=0` throughout on this third independent test, and its onset/recovery timestamps again match both previous captures to the millisecond (`t=615687ms`/`t=769365ms`). Tick-slip theory stays dead; boot-time determinism now confirmed across three separate boots. No firmware change made this turn. Full numbers and the plateau table in `null_bias_investigation.md`.

## 2026-09-16, later still: audio counter's own design (8192-pt FFT peak search @ 48kHz) explains the plateau-stepping puzzle - it's almost certainly being fooled by the already-known 8Hz/40Hz sideband comb, not measuring a real frequency shift

User described the counter: 8192 samples @ 48kHz (5.86Hz bins, 170.7ms window), peak search + bin interpolation, ~0.3Hz accurate "for a clean tone." Two numbers line up badly for this signal specifically: 700Hz itself sits 0.467 of a bin off-center (worst case is 0.5) so an unwindowed FFT leaks its own energy substantially into neighboring bins, and the already-documented 8Hz-spaced sideband comb (elevated at 40Hz) sits right in that same neighborhood (40Hz = 6.8 bins away) and also doesn't land on clean bin centers. A peak-search-and-interpolate algorithm built for a single clean tone has no guard against a nearby real comb of harmonic energy competing with it - exactly the kind of thing that would bias the reported frequency by tens of Hz in a way that steps once per measurement rather than drifting smoothly, matching what was observed. This ties the plateau puzzle directly to the already-open 8Hz/40Hz sideband item rather than pointing at anything new or real in the transmit chain. Not confirmed, no firmware change made - cheap next tests (windowing the FFT, narrowing the peak search, logging the winning bin) proposed in `null_bias_investigation.md`.

## 2026-09-16, later still: sideband-leakage theory for the audio-counter plateau puzzle is DEAD (proven by simulation) - real sidebands are ~100x too weak to matter; leading candidate moves to the SDR's own receive chain

User correctly flagged the real sideband levels (30-40dB down) and the 150Hz SDR filter. Simulated the counter's exact algorithm (8192-pt rectangular FFT + parabolic interpolation) against a -30/-40dB interferer at 8-40Hz offset, worst-case phase: bias stays within +/-0.05Hz of the no-interferer baseline (~0.4Hz), nowhere near the observed 15-157Hz plateau steps. Leakage theory falsified; also shows the counter's own math is already accurate enough that a bigger FFT block (which the user was weighing, but was rightly wary of - risk of smearing a real mid-block transition) wouldn't help and isn't recommended. With the counter's math and the transmit-side DSP both now cleared, the leading candidate shifts to the SDR receiver itself (LO/tracking/AGC stability) rather than anything in this firmware - consistent with the user's own much-earlier note that SDR tracking isn't reliable. Genuinely unresolved; doesn't change any transmit-side conclusion, since none of those rest on the SDR. Full simulation numbers in `null_bias_investigation.md`.

## 2026-09-16, later still: `log_20260916_200756.txt` ends in a "very noisy mode" the user compares to the known x4-interp burble - completely invisible to every diagnostic currently in place, and that's itself the finding

User reported the run ended in a noisy mode resembling the previously root-caused x4-interp burble (`ENVELOPE_INTERP_FACTOR=4` was confirmed twice on real hardware to cause audible noise via `dsp_task`'s wake rate, not per-tick compute time). Checked this ~1690s mid-session capture exhaustively - `overruns=0`/`late=0` on all 2640 samples, `max_busy_us` normal throughout, no canary hits, fast/slow flat, the usual `~153.6s` cadence runs cleanly right to the file's last, completely ordinary line. Nothing shows the reported noise at all.

That's informative rather than a dead end: every counter this project has (`overruns`/`late`/`max_busy_us`) measures per-tick busy duration, not how often `dsp_task` gets woken - exactly the dimension the x4-interp case's real root cause lived in. A transient wake-rate/ISR hiccup could produce the same audible symptom while leaving all of today's counters clean, simply because none of them watch that. One coincidence flagged, not explained: the user's own estimated ~155s recovery time sits close to this project's well-documented ~153.6s cycle, though a rough human estimate and a precise DSP-measured period aren't really comparable numbers. No firmware change made - would need either a description of what the noise actually sounds like (to check against the two already-known, distinct noise mechanisms in this project's history) or new instrumentation that watches wake-to-wake timing directly, which nothing currently does. Full reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: `log_20260916_205209.txt` - ADC disable ruled out as the noisy-mode cause; a second, fully-captured noisy episode (went noisy, snapped back on its own) again leaves zero trace anywhere in the diagnostics

User disabled the ADC as a candidate cause - no difference, so ADC is ruled out. This capture also contains a full noisy episode (onset and self-recovery both inside the ~550.7s window, ~847 `held_trace` samples) rather than cutting off mid-episode like last time. Same result as before: `overruns=0`/`late=0` throughout, `max_busy_us` normal (45-52us), no canary hits, no `tx_freq` phantom (expected - outside its usual boot-relative window), and the usual `~153.6s` cadence runs with completely normal timing straight through wherever the noise fell. Nothing marks it at all.

Two independent noisy episodes now, one candidate cause eliminated, both showing total silence from every current diagnostic - a real, audible symptom that current instrumentation structurally cannot see. This continues to point at either the envelope/PWM/analog chain (already-established theory: corruption there is invisible to every `freq_dev_hz`-based diagnostic, which is everything built so far) or a wake-rate/ISR-timing effect like the x4-interp precedent, rather than anything the frequency-side counters measure. No firmware change made. Full reasoning in `null_bias_investigation.md`.

## 2026-09-16, later still: rethink on why computed avg diverges from measured - this session's own new fast energy-weighted EMA likely uses the wrong envelope, and there's already a validated reference sitting unused to check it against

Stepped back from measurement-side theories to re-derive this from first principles, and found the answer was already sitting in this codebase's own history: `ssb_dsp.c` has a lifetime energy-weighted accumulator (`ssb_dsp_get_null_bias_stats()`), built and hardware-validated back on 2026-09-09/12, that pairs envelope² with the phase delta at the exact same raw tick using the RAW envelope - confirmed to match SDR readings to a few Hz across 6/6 tone pairs, and described as giving one STABLE number for a fixed configuration, not something that cycles through plateaus. This session's own new fast EMA (`diagnostics.cpp`, built from scratch this session) instead weights by a downstream, SHAPED envelope (post-ampeq/gdeq/predistort) - chosen earlier for being correctly time-aligned across the delay ring, but never checked for whether shaping also changes the envelope's VALUE right at the null, which is exactly the failure mode that would explain wild plateau-cycling on an unchanging bench setup.

Added a direct, same-instant cross-check (no bench-side action needed, no unmuting): every `ENERGY-WEIGHTED CONFIRMED STUCK` print now also prints the OLD, validated, raw-envelope lifetime average right below it, so the next capture settles this outright - if the validated reference stays small/stable while the new fast EMA claims tens of Hz, that confirms the fast EMA's envelope choice as the actual source of this whole session's central puzzle, not anything physical. Full reasoning in `null_bias_investigation.md`. Committed, not yet bench-tested.

## 2026-09-16, later still: cross-check confirms it on the first try - the shaped envelope is the source of the whole session's "computed vs measured" puzzle; added a second, correctly-paired fast EMA to nail it down further

The cross-check added last entry paid off immediately. Over a 45.7s window between two `ENERGY-WEIGHTED CONFIRMED STUCK` events, `ssb_dsp.c`'s validated, raw-envelope-paired lifetime average moved 3.2Hz (1184.87Hz -> 1188.07Hz - ordinary slow settling) while this session's own shaped-envelope fast EMA moved 83.0Hz (1056.37Hz -> 1139.40Hz) over the identical interval. The validated reference sees nothing; the new fast EMA reports a big swing anyway - confirms the shaped envelope (ampeq/gdeq/predistort/PWM mapping, all downstream of the raw DSP envelope) as the actual source of this whole session's puzzle, not the transmitted signal. Side note: even the fast EMA's own one-time boot anchor (1070.13Hz) sits 114.7Hz away from where the validated reference has read all session - the baseline itself was never trustworthy, not just the swings on top of it.

Added a second, correctly-paired fast EMA (same 50ms tau, but weighted by this tick's own raw envelope/freq_dev captured immediately after `ssb_dsp_process_sample()`, before any shaping) so the next capture gets a live-speed three-way comparison rather than relying on the slow lifetime accumulator alone. Two known simplifications flagged (pre-delay vs. the shaped EMA's post-delay; post-clamp vs. the validated accumulator's pre-clamp dphi) - likely minor, not yet proven so. If this new one also stays stable where the old one swings, the fix is clear: retire the shaped-envelope fast EMA. Full numbers and code changes in `null_bias_investigation.md`. Not yet bench-tested.

## 2026-09-16, later still: three-way cross-check comes back nuanced - shaping confirmed as ~65% of the swing, but a smaller residual remains even when correctly paired, most likely ordinary sampling variance rather than a second bug

First live data from the new raw-envelope-paired fast EMA (`cross-check2`): over the same 45.7s window, the old shaped-envelope EMA swung 83.07Hz, the validated lifetime average swung 4.02Hz, and the new correctly-paired fast EMA swung 29.31Hz - a real ~65% reduction, confirming shaping as the majority cause, but not a clean pass. Leading explanation for the remaining ~29Hz: ordinary sampling variance from running a fast (50ms) EMA on a quantity a lifetime accumulator needs millions of samples to average cleanly - this project already established exactly this reasoning back on 2026-09-12 for a different discrepancy, so it's a well-precedented, not ad-hoc, explanation.

Bigger-picture implication if this holds: a live, fast, 5Hz-threshold "stuck" detector may not be a workable instrument for this quantity at all, regardless of envelope pairing - the fix path is either a much longer EMA tau (trading responsiveness for the depth needed to tame the variance) or dropping the live detector in favor of periodic on-demand lifetime-accumulator reads (`'V'`), the way the original 2026-09-09/12 work already did it successfully. Not actioned yet - recommended a longer, unchanging-setup dwell test next, watching only `cross-check2`, to see whether its spread narrows with time (variance) or stays put (points back at the two remaining caveats: pre-delay vs. the active relative_delay, and post-clamp vs. pre-clamp dphi). Full numbers in `null_bias_investigation.md`.

## 2026-09-17: fresh `'J'` capture at delay=+2.00 reproduces the known null-crossing churn almost number-for-number, confirms a 5-day-old unconfirmed hypothesis about integer delay, and gives the still-open cross-check2 residual a concrete mechanism

New `'J'` jump-log dump at `relative_delay=+2.00` (this project's own established best two-tone IMD compromise, not an extreme test value): 1782960 qualifying (>300Hz) events since last reset, 40% near-null-blended, 45% near-null-either, an 8-entry ring showing a locked 2-state cycle alternating ~14201360Hz <-> ~14193359Hz. Raw near/far values (`near=-6802.0Hz far=1198.2Hz raw_delta=8000.1Hz`) are essentially identical to the 2026-09-12 capture at a completely different delay (+1.75) - same already-root-caused two-tone polar-null mechanism, not anything new, and `raw_delta` matching `step_hz` again confirms it's a real raw discontinuity, not a `relative_delay` interpolation artifact.

Two things worth flagging. First, every entry shows blended envelope identical to the min envelope (`env=X/X`) - the first-ever direct confirmation of a 2026-09-12 hypothesis that was proposed but never tested: at an exact integer sample delay (+2.00), `interp_ring()`'s blend weight collapses to 100%/0%, so there's no second raw sample to blend with. Second, and more relevant to the live investigation: roughly 55-60% of these jumps sit at a MODERATE envelope (~0.205, 4-5x the 0.05 null threshold) while still showing the full ~8000Hz raw discontinuity - meaning "envelope near null" is a narrower predictor than the current threshold assumes; the phase computation stays badly behaved over a wider envelope range around each beat minimum than the classification currently catches.

That last point gives the still-open `cross-check2` ~29Hz residual (previous entry) a concrete mechanism instead of a generic "sampling variance" label: `cross-check2` weights by `env2`, and at `env=0.205` that weight (`≈0.042`) isn't negligible, so this large, high-repetition-rate (hundreds-to-thousands/sec), moderate-envelope churn very plausibly isn't fully averaging out inside a 50ms-tau EMA's ~50-beat-cycle memory the way it does over the lifetime accumulator's millions of cycles. Reframes, doesn't contradict, the sampling-variance theory - and strengthens the case for lengthening the EMA tau substantially or dropping the live detector in favor of the periodic lifetime-accumulator read, since this is a structured, repeating signal component, not unstructured noise. Also confirmed the trailing `held_freq: ... dev=+40.00Hz from anchor=1070.13Hz` in this same capture is from the already-known-untrustworthy OLD shaped-envelope `'H'` detector (matches its known-bad boot anchor) - no new concern there. Not actioned - analysis only, no firmware change. Full derivation and two proposed next steps in `null_bias_investigation.md`.

## 2026-09-17, later still: the long-deferred hardware test finally happens - push/pull drivers fitted on the AD9851 lines, directly testing `AD9851.c`'s own leading "sticks" theory; hands-on impression is no change, hands-off run pending

The hardware test flagged weeks ago and never done: push/pull drivers now fitted on the AD9851 interface lines, speeding up the edges (especially the rising one). This directly targets the theory already written into `AD9851.c`'s own comments - that the old BS170-based inverting level shift gave a slow, passive, pull-up-charged LOW-to-HIGH edge (affecting both a fresh DATA 0-to-1 bit and, via the inversion, W_CLK's AD9851-side rising/sampling edge), and that a large one-tick FTW jump from two-tone near-null atan2 noise (flipping many bits at once) was "the leading theory for the two-tone-specific 'sticks' symptom."

First (hands-on) impression: no visible change to RF output. User's own added observation: there was already plenty of margin between the data change and the rising clock edge before this change, ahead of it independently. Consistent with `AD9851_BITBANG_EDGE_DELAY_ENABLED` having sat at `0` (no explicit settle delay) since 2026-09-11.

**Confirmed by the user**: the new drivers preserve the same inverting logic as before, just actively driven both directions now - `AD9851_INVERTING_LEVEL_SHIFT` correctly stays `=1`, no firmware/hardware mismatch. No firmware change made or needed.

Next: a hands-off run for a clean before/after comparison against the historical "sticks"/near-null-jump baseline - either the first real confirmation of a theory that's been open for weeks, or a clean way to rule out edge speed and redirect attention back to the phase/atan2 computation itself. Full reasoning in `null_bias_investigation.md`. Not actioned further, awaiting that capture.

## 2026-09-17, later still: `log_20260917_112714.txt` - the push/pull-driver hands-off run overturns "ordinary sampling variance" as cross-check2's residual explanation (it's aliasing of the well-documented ~153.6s cycle instead), shows no visible hardware-test improvement, and surfaces a real ~50Hz audio-tone excursion invisible to cross-check2

First hands-off capture on the new push/pull AD9851 drivers, ~14 minutes, 11 full `ENERGY-WEIGHTED CONFIRMED STUCK`/`RECOVERED` cycles with `cross-check`/`cross-check2` live throughout, plus the audio-tone counter running. Three findings.

**The STUCK/RECOVER cycle is confirmed as an exact, precisely repeating ~153.6-153.7s oscillation**: `STUCK+` (dev ~+68Hz) always lasts 89.7-89.8s, an ~18.2s gap, `STUCK-` (dev ~-16.5Hz) always lasts 45.7-45.8s, then immediately back to `STUCK+` - matching to within 0.1s across 5-6 repeats. This is the project's long-recurring `~153.6s` period, now pinned down as the already-known-buggy shaped-EMA detector's own deterministic hysteresis cycle. `relative_delay` stayed untouched at `+2.00` throughout.

**Big correction: cross-check2's "residual ~29Hz swing" is NOT sampling variance - it's aliasing.** Its 11 readings split into two near-identical clusters depending on which phase of the cycle they were sampled at: ~1168.6-1168.8Hz during every `STUCK+` (6/6, smoothly drifting down by hundredths of a Hz per cycle - a real slow trend, not noise) and ~1198.46-1198.48Hz during every `STUCK-` (5/5, same smooth drift). A value reproducible to within 0.02Hz across 11 independent events 14 minutes apart is not statistical variance - it's `cross-check2` only ever being sampled at ONE fixed phase point of the exact cycle just characterized (it's printed at the `'H'` detector's own trigger instant, always ~15s into a stuck period). That's textbook stroboscopic aliasing: sample a periodic process at a fixed phase and it always reads back the same value, regardless of how much it varies elsewhere in the cycle. Recommended fix path changes accordingly - lengthening the EMA's tau won't help (the problem was never insufficient averaging); the useful next step is printing `cross-check`/`cross-check2` on a fixed TIME cadence, decoupled from `'H'`'s trigger, across a full cycle to see the real waveform.

**The hardware test itself shows no visible improvement** - `overruns=0`/`late=0` throughout as always, and both the shaped-EMA cycle's signature and the audio-tone counter's plateau/drift character are indistinguishable from before the driver swap, matching the user's own hands-on impression. One wrinkle, now upgraded from "unexplained" to "confirmed real and significant": one `STUCK+` segment shows the audio-tone counter drifting to a sustained ~-50Hz, and the user directly confirms this was a real jump, not a counter artifact. `cross-check2`, sampled at the very same instant, reads its completely ordinary value - i.e. a genuine, user-confirmed ~50Hz shift left zero trace in EVERY frequency-side diagnostic this project has (`cross-check`, `cross-check2`, `'H'`, plain fast/slow, `'J'`). **Correction, same day**: the user also confirms the signal during this event was CLEAN, not noisy - so this is NOT the same mechanism as the "very noisy mode" symptom (that speculation is retracted); a clean-but-wrong-frequency signal points more specifically at something computing/encoding the wrong frequency cleanly (e.g. the AD9851 FTW itself) than at generic analog-chain corruption. Full numbers and next steps in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change.

## 2026-09-17, later still: a new, distinct jump class spotted hands-on - frequency can be "walked back" toward nominal by stepping `relative_delay` (~2-3Hz per coarse step), but only in a particular off-nominal state, and doing so costs IMD - strong circumstantial evidence it's the same delay-interpolation mechanism already characterized, not a new bug

User's own hands-on finding: in a particular state, nudging `relative_delay` with `'['`/`']'` measurably shifts the measured frequency (~2-3Hz per 0.05-sample coarse step) - but this delay-dependence disappears once back on nominal frequency. Unlike the swings this investigation has tracked so far (which change the frequency reading while IMD stays reasonable), pulling this mode's reading back toward nominal by moving delay away from `2.00` (Preset 3's established best-IMD point) visibly degrades IMD - the "fix" trades a reading symptom for a real physical cost rather than correcting anything.

Very likely the same mechanism already characterized in `null_bias_investigation.md` since 2026-09-12 (and reconfirmed there this same day via the fresh `'J'` capture): `relative_delay`'s ring interpolation linearly blends two adjacent raw `freq_dev_hz` samples, and a 2-3Hz shift per 0.05-sample step implies a local raw slope of ~40-60Hz/sample at that point in the cycle - far smaller than a full ~8000Hz null-crossing jump, but a real, non-zero slope, consistent with sampling a moderately-varying (not flat, not at a null) part of the signal. Explains the IMD cost too: `2.00-2.10` is the independently-established true physical optimum, so moving off it to chase a reading that's itself just an interpolation artifact gives up real IMD for no real correction.

**Sharpened same day, prompted by a direct user challenge**: "point in the cycle" needed unpacking, since 700/1700Hz are exactly locked to the 16000Hz sample rate and the ADC is disabled - there's no real-world jitter anywhere in this loop, and the raw DSP math must be exactly periodic every 10ms (the two-tone waveform's own repeat period) forever. Two different cycles were being conflated: the FAST, exactly-periodic 1ms/10ms two-tone beat (locked, no ambiguity - the user's point stands) and the SLOW ~153.6s macro cycle pinned down earlier today, which - since it can't come from a purely periodic, memoryless raw computation - must actually originate downstream, in the shaped-envelope IIR filters or the `'H'` detector's own hysteresis logic, not upstream. The delay-walk effect itself re-points at the FAST cycle: a human keypress isn't synced to a 1ms/10ms clock, so each attempt lands at an effectively arbitrary phase of the exactly-repeating beat (sometimes near a null, sometimes not) - explaining why it's only sometimes observed, with zero real randomness involved. Falsifiable refinement: triggering the delay-walk test at a controlled phase offset relative to the two-tone generator, rather than an arbitrary keypress, should make it fully predictable. Full reasoning in `null_bias_investigation.md`. Not actioned, no firmware change.

## 2026-09-17, later still: a rigorous user challenge (LTI stages + locked tones + no ADC = no room for variability) leads to a real bug - the two-tone generator's naive float32 phase accumulator - confirmed by simulation, but its drift rate is ~16x too slow to be THE ~153.6s mechanism

User correctly pointed out that the two-tone generator, Hilbert FIR, and shaping IIR filters are all deterministic LTI stages fed an exactly-periodic (10ms) input with no ADC involved - so an idealized version of this chain has no business containing anything at the ~153.6s timescale. Checked the actual generator code (`test_signals.cpp`): it's a naive floating-point NCO (`phase += increment; if (phase > two_pi) phase -= two_pi;`, all in 32-bit `float`) - a well-known class of implementation that accumulates rounding error over time rather than recomputing exactly each tick.

Simulated the exact recurrence to check whether this actually matters - and caught a mistake in my own first attempt before trusting it. A quick "count wraps over one window" method first suggested an almost-too-good ~162.6s match; re-running properly (tracking true unwrapped phase against an exact double-precision reference across 8 million ticks) showed the first result was a boundary-quantization artifact of that quick method, not real. The corrected measurement shows a genuine, linear drift: the 700Hz tone runs +1.8e-4 Hz fast, the 1700Hz tone runs -2.0e-4 Hz slow, making the true beat frequency off by ~-3.8e-4 Hz - a real precession, but with a ~2500s (~41-44 minute) period, about 16x too slow to be the ~153.6s cycle this investigation has been chasing.

Net result: a genuine, low-risk, worth-fixing floating-point bug in the test-tone generator (fix: exact modulo-based or integer phase accumulation instead of accumulate-and-subtract), but NOT the explanation for the exact 153.6s period - that likely still lives downstream (the shaped-envelope IIR filters or the `'H'` detector's hysteresis), with the ESP32's actual hardware timer rate (vs the assumed exact 16000.000000Hz) newly flagged as another candidate worth checking. Full simulation numbers and the self-caught methodology error in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change.

## 2026-09-17, later still: does the NCO drift matter more AT the null? Yes - refined math lands within ~7% of the observed 153.6s cycle; the user's own follow-up insight names the correct fix directly, now implemented and verified by simulation to give exactly zero drift

Answered the natural follow-up to the last entry: the small NCO drift found there is far more consequential AT the null than its "average beat frequency" effect suggested. With exactly 16 samples per 1kHz beat cycle, the earlier ~2639s full beat-phase-drift period, divided by those 16 discrete sample slots, gives ~165s - within ~7% of the precisely-measured 153.6-153.7s macro cycle. Not exact, but a much stronger, more mechanistically-grounded candidate than the raw beat-drift number, since it's the discrete "which sample lands closest to the true null" granularity that downstream near-null-sensitive computation actually sees.

The user's own follow-up nailed the fix directly: since the whole chain is locked and deterministic, every two-tone cycle should be reproduced bit-for-bit identically, not just "drift slowly." Implemented in `test_signals.cpp`: the old accumulate-and-subtract phase generator is replaced with an exact recompute from a wrapping integer sample index each tick (tone1 always, tone2 whenever dither is off) - verified by simulation to give exactly zero drift (0.0 deviation at every boundary across an 8M-tick run, vs. the old scheme's confirmed growing error). Dither keeps its own accumulator since its frequency is intentionally time-varying. Not yet bench-tested (no toolchain here). The decisive test still pending: whether the real ~153.6s cycle changes at all once every cycle is bit-exact on real hardware. Full reasoning and code details in `null_bias_investigation.md`.

## 2026-09-17, later still: `log_20260917_122337.txt` - first hardware data with the NCO fix flashed. The macro cycle didn't disappear, but its period changed dramatically (~153.6s -> ~8.15s, ~19x faster); the separately-reported "twice a second changing tone cycle" is not confirmed or explained by this file

First real capture since the `test_signals.cpp` phase-generator fix was flashed, same 700/1700 test setup. The `slow_trace: AUTO-CAPTURED` event's period is now **8148.5ms +/- 3.6ms** across 53 consecutive intervals - tight and clock-like, but about **19x faster** than the ~153.6-153.7s cycle characterized on every pre-fix capture. Not "the fix didn't work" - a real, substantial change in the mechanism's operating period on the same config, strong (if indirect) evidence the old NCO drift was feeding whatever triggers this hysteresis, even though the hysteresis logic itself (still most likely the shaped-envelope IIR filters or the `'H'` detector) evidently lives downstream and didn't disappear.

Separately: the user reported "a regular freq shift about twice a second," clarified as a changing tone CYCLE (character/timbre) rather than a discrete jump. Nothing in this file confirms, explains, or rules this out - the only fine-grained per-tick data is a ~17ms burst, far too short to see a ~500ms-scale pattern, and no other timestamped structure lands near that rate. Needs a continuous multi-second capture, or a marker press at the moment it's heard. Full reasoning in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change this turn.

## 2026-09-17, later still: `ToneWarble1.wav` - the "twice a second changing tone cycle" measures out to almost exactly a 1.000s period, not 2Hz, and the leading candidate is `diagnostics_service()`'s own 1000ms-gated Serial print block on Core 1 - a mechanism this project has already partially characterized as capable of disturbing dsp_task on Core 0

A short-time FFT analysis of the user's own audio recording of the symptom found a real, tightly repeating pattern throughout the whole 4.93s clip: a long (~430ms) dominant-tone dwell, a shorter (~265ms) dwell at a nearby frequency, then a busier flickering tail, repeating with measured inter-cycle gaps of 1000.0/994.6/997.4/997.3ms - essentially exactly 1 second, not the "twice a second" impression, likely because each 1Hz cycle contains two salient tone-character states that a listener would naturally count as two events.

Grepped the whole codebase for anything unconditionally periodic at ~1000ms. Found exactly one strong candidate: `diagnostics_service()` (Core 1, `diagnostics.cpp:2536`) fires an unconditional (whenever diagnostics aren't muted) `Serial.printf()` burst - `print_timing_and_adc_block()` + `print_null_bias_block()` - once per second, every second, for as long as the unit runs. This isn't a new theory being invented cold: this project has already measured and documented (same file's header comments) that this exact print block's cost is watched, and an earlier "Fs jitter hunt" already found that blocking `dsp_task` (Core 0) stretches its gptimer wake-up cost via cross-core IPI - i.e. this class of Core-1-disturbs-Core-0 interference is already a known, characterized risk in this codebase, just not previously tied to an audible, once-a-second symptom. The other 1000ms-gated code path found (`envelope_output.cpp`'s DAC I2C error log) was ruled out - it only fires on an actual bus failure, not as a steady background event.

**Open confound**: whether Serial/diagnostics output (`'v'`) was actually unmuted during this recording - if muted, this print block never runs and the candidate is eliminated. Cheap decisive test proposed: record with `'v'` toggled between clips and see whether the ~1.000s pattern tracks the mute state. If it does, this would be the first mechanism in the whole investigation shown to correlate with something audible in real time, rather than only with internal detector/diagnostic state. Full reasoning in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change.

## 2026-09-17, later still: the `diagnostics_service()` theory for the ~1.000s `ToneWarble1.wav` cycle is ruled out - `'v'` was already off during the recording and turning it on makes no difference; an exhaustive re-search finds no other unconditional ~1Hz timer anywhere in the firmware, pointing suspicion at the monitoring/recording chain instead

User confirms `'v'` (diagnostics) was already muted when the warble was recorded, and switching it on made no audible difference - this rules out the previous entry's leading candidate cleanly, since that whole print block (and `print_status_line()`) sit behind the same mute gate and literally never execute when `'v'` is off.

Re-searched the whole codebase for any other unconditional ~1Hz mechanism (no mute gate involved) and found none: no other hardware/software timer, no WiFi/BT/OTA code at all, `loop()` itself has no counter that could produce a ~1s cadence independent of `diagnostics_service()`'s own gated timer, and there's no preset auto-cycle anywhere (presets are static, keypress-selected only). This is a genuinely exhausted firmware-side search for this specific signature.

Balance of evidence now points away from this project's ESP32 firmware and toward the monitoring/recording chain itself (receiver AGC, SDR software's own AGC/averaging rate, USB audio buffering, etc.) as the source of the ~1.000s cycle. Next step: find out exactly how the recording was made (what's on the receiving end) before searching the TX firmware further for this. Full reasoning in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change.

## 2026-09-17, later still: recording-chain details point the `ToneWarble1.wav` ~1.000s cycle at the SDR receiver's own AGC, not the TX firmware - visible directly on the spectrum display's baseline noise (not just the audio), and only present on two-tone (sine/AM are clean)

Two key facts from the user: the recording is Audacity capturing an SDR's audio output, and the ~1.000s pattern is visible directly on the SDR's own spectrum display as modulating baseline noise - not just in the demodulated audio - which rules out an Audacity/USB-audio-buffering artifact. Critically, sine tone and AM test signals are clean; only two-tone shows it.

That's a strong pointer at classic receiver "AGC pumping": two-tone has a much higher peak-to-average ratio and genuine envelope nulls compared to a constant-envelope sine or gentle AM signal, and AGC loops are known to oscillate visibly/audibly on exactly that class of signal, at a rate set mostly by the AGC's own attack/decay time constants (which would also explain why the measured period is so tight - a control loop's self-timed oscillation is often more regular than the input driving it). This means the already-well-characterized two-tone envelope irregularities this investigation has spent weeks on may be sufficient to trigger this via the SDR's own AGC, with no new firmware bug required.

Cheap decisive test proposed, no firmware involved: switch the SDR to manual/fixed gain (AGC off) and see if the pumping disappears. If it does, this symptom moves out of the firmware investigation entirely. Full reasoning in `null_bias_investigation.md`. Not actioned - analysis only, no firmware change.

## 2026-09-17, later still: retracting the AGC theory - user has already checked it directly and is confident the ~1.000s cycle is coming from the ESP - made a temporary, clearly-marked causal-test change (1000ms -> 1700ms) to the one concrete "~1 second internal loop" this codebase has, per the user's direct request

The user has already checked the SDR's AGC and is confident this originates on the ESP32 side - retracting the previous AGC-pumping theory; direct hands-on verification overrides that inference.

Confirmed first that `diagnostics.cpp`'s fast/slow EMA detector (2s tau, backing the `'H'`/held-frequency detector) is read-only - it only ever watches `tx_freq`, never writes back into the live `freq_dev_hz`/`tx_freq` computation, so it has no path to affect the actual transmitted signal regardless of its tau value.

The one thing that IS both unconditional-when-unmuted and concretely ~1 second is `diagnostics_service()`'s own print-block gate (`diagnostics.cpp`) - the same one the earlier mute-toggle test seemed to rule out. But mute on/off only tests presence/absence, not the period itself. Per the user's direct request, retuned it from 1000ms to 1700ms (temporary, clearly commented, easy to revert) as a genuine causal test: if the audible/spectrum cycle shifts to ~1.7s with `'v'` ON and this build flashed, that proves this block (or something keyed to its exact timing) is the mechanism; if the cycle stays at ~1.000s regardless, that rules it out via direct parameter variation rather than just on/off, redirecting the search to the two-tone-specific DSP path instead. Note: this test REQUIRES `'v'` on to have any effect at all (unlike the original muted recording). Not yet bench-tested - ready to flash. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: the 1700ms causal test also comes back negative - `diagnostics_service()` conclusively cleared by two independent tests, reverted back to 1000ms; answered the user's direct question on 700/1700's interference periodicities, which turned up a striking exact-100-cycles-per-second coincidence shared by every preset in this codebase - rigorously checked against (and cleared) today's own NCO-fix wrap boundary by direct float32 simulation

`'v'` on/off still makes no difference with the print gate at 1700ms - same result as the original mute test on the 1000ms build. Combined with the earlier finding that this whole detector module only ever watches `tx_freq` and never feeds back into the live signal, `diagnostics_service()` is now cleared by two independent tests, not just reasoning. Reverted the gate back to 1000ms.

Answered the user's direct question on 700Hz/1700Hz's interference periodicities in full (difference/sum/3rd- and 5th-order IMD products, individual tone periods, and the GCD-based 10ms fundamental repeat, already established this session). None of these individually is near 1 second, but a striking exact relationship stands out: 100 repetitions of the 10ms fundamental, and separately all 16000 raw samples, both equal exactly 1.000000 second - and this isn't unique to 700/1700: every preset in `TWOTONE_BAND_PRESETS[]` shares the same 100Hz GCD by design. Also caught and flagged a small, separate, harmless bug while checking this: the "700/1700 legacy default" preset's own display string is mislabeled "700/1900."

Because today's own `test_signals.cpp` NCO fix wraps its sample index at exactly `SAMPLE_RATE_HZ` (exactly once per second), this exact coincidence made it the obvious thing to re-check rigorously rather than assume clean. Built a bit-accurate float32 simulation of the real phase-generation arithmetic and measured the actual discontinuity at each 1-second wrap vs. ordinary 10ms boundaries and arbitrary non-boundary samples: the wrap-boundary jump (~5e-08) is smaller than ordinary floating-point jitter elsewhere in the run (1e-05 to 3e-04) - the fix is cleared, confirmed by simulation rather than just the hand-argument in its own comment.

Mechanism remains unidentified. Proposed next test: since every built-in preset shares the same 100Hz-GCD grid, try a deliberately non-100Hz-spaced pair (e.g. 733/1717Hz) to see if breaking that exact alignment changes or removes the ~1.000s pattern - a real causal test of whether the "100 cycles per second" coincidence matters at all. Full reasoning and simulation numbers in `null_bias_investigation.md`. No firmware change this turn beyond the 1700->1000 revert.

## 2026-09-17, later still: answered directly - the 1-second wrap always lands exactly at the envelope PEAK (structurally, by construction), never near a null, and its 0.5ms distance to the nearest real null is no different from any ordinary peak's; `Warbletone3-300-500.wav` confirms the ~1s pattern also shows up on the 300/500 preset

Checked rigorously rather than assumed: at every wrap, both tones' phase resets to exactly 0 (by the fix's own recompute-from-index formula), which is precisely the two-tone envelope's maximum - verified numerically at exactly 2.000000 (the theoretical max) for both 700/1700 and 300/500. The nearest real null sits 8 samples (0.5ms) either side for 700/1700, which is completely unremarkable - nulls recur every 16 samples throughout the ENTIRE waveform regardless of the wrap, so the wrap is no closer to a null than any other ordinary peak. Combined with the earlier discontinuity-size result, today's NCO fix is now cleared twice over.

`Warbletone3-300-500.wav` (a different preset, same 100Hz-GCD family) shows the same ~1s pattern (autocorrelation peak at 0.979s), confirming this isn't unique to 700/1700. The proposed non-100Hz-spaced test pair is still the cleanest next step. Full reasoning and numbers in `null_bias_investigation.md`. Analysis only, no firmware change.

## 2026-09-17, later still: `warble733-1717.wav` (+ its `'v'`-on companion) still shows ~1.000s, but a math check caught a mistake in the test pair itself - 733/1717 are coprime, so their own TRUE fundamental period is exactly 1.000s by definition, and (bigger realization) EVERY integer-Hz two-tone pair is mathematically guaranteed to repeat a whole number of times per second, so no integer-Hz pair can ever be a clean "broken alignment" control. Added a temporary non-integer-Hz test preset to `test_signals.cpp`

Both the muted and unmuted `733/1717` recordings show the same ~1.000s autocorrelation peak (0.384/0.997s) as every prior capture. But checking the math first: `gcd(733,1717)=1`, so this pair's own correct, expected fundamental period IS exactly 1 second - it isn't a "broken alignment" control as intended, it's if anything MORE tied to 1 second than 700/1700 or 300/500. Bigger realization: since the GCD of any two integers is itself an integer, EVERY possible integer-Hz two-tone pair mathematically must repeat a whole number of times per second - this was never a special property of any specific preset, so it can't distinguish a real bug from normal behavior using integer-Hz tones at all.

The only way to actually break this alignment is a non-integer-Hz pair. No serial command exists to dial in custom frequencies, so added a temporary preset to `TWOTONE_BAND_PRESETS[]`: `700.37/1700.61 (TEMP non-integer-Hz control)`, reachable via repeated `'T'` presses, clearly marked for removal once tested. If the ~1.000s pattern still shows up with this pair (whose own natural repeat period doesn't land on any whole-second boundary), that's decisive proof the mechanism is something else entirely, not the tone generator's own periodic structure. Full reasoning in `null_bias_investigation.md`. Ready to flash and test.

## 2026-09-17, later still: the decisive non-integer-Hz test (`700.37/1700.61Hz`) is a genuine positive result - the clean single ~1.000s peak seen on every integer-Hz pair so far is gone, replaced by a smeared cluster of 8 comparably-strong candidates across 0.3-1.0s. The exact-whole-cycles-per-second alignment really does matter; leading theory shifts to a live in-DSP resonance/entrainment mechanism (same class as the already-known 153.6s/8.15s macro cycles, just not yet located) now that every specific timer candidate is cleared

Ran the decisive test: `700.37Hz/1700.61Hz`, `'v'` off. Confirmed with two independent autocorrelation methods (binary bin-match and continuous peak-frequency value) - both agree: no single dominant ~1.000s peak anymore. Every integer-Hz recording so far showed one clearly dominant peak at ~0.98-1.00s; this one shows eight comparably-strong candidates (0.312-1.000s, correlations 0.549-0.624) with no clear winner.

This is meaningful, not a null result - the underlying beat/null-crossing rate barely changed (1000.24Hz vs 1000Hz, a 0.024% difference), nowhere near enough to explain going from one sharp line to eight smeared candidates on its own. So it's specifically EXACT commensurability with 1 second that matters, not just approximate tone spacing.

With every specific timer/counter candidate now cleared (diagnostics_service() twice over, today's NCO fix twice over), this looks like a classic signature of a nonlinear/hysteresis mechanism being driven by a periodic input - forced into one clean resonant line when the input repeats exactly every cycle, smeared across nearby periods when it doesn't. That's the same CLASS of behavior already established this session for the 153.6s/8.15s macro cycles, just not yet located in the live DSP/envelope path itself (as opposed to diagnostics.cpp's already-cleared read-only copy). Next step: look at ssb_dsp.cpp's Hilbert/atan2 chain and envelope_gdeq.h's shaping IIR filters for anything with its own feedback/threshold behavior near a 1-second timescale. Full reasoning in `null_bias_investigation.md`. Analysis only, no firmware change this turn.

## 2026-09-17, later still: `733-1700_Preset1.wav` (every optional shaping/filter/compressor/EQ/predistortion/interpolation/ampeq stage OFF) - the pattern gets CLEANER and STRONGER, not weaker, revealing its true fundamental at ~0.485s (~2.06Hz) - almost exactly "twice a second," the user's original description from the start of this whole thread. Core `ssb_dsp_process_sample()` pipeline itself checked directly and has no candidate timer - leading theory is now the already-known near-null "sticks" mechanism recurring deterministically because the input is exactly periodic

Parsed the pasted preset line against `PersistentSettings`' field order to confirm "all filters off" really does disable every optional stage (gdeq shaping, ADC LPF, EQ, compressor, predistortion, null floor, slew limiter, envelope interpolation, ampeq). With all of that off, the autocorrelation signal got CLEANER (0.822 correlation, the strongest yet) and revealed harmonic structure matching a true ~0.485s (~2.06Hz) fundamental, with the previously-measured "1.000s" being its own 2nd harmonic. This is the first time the numbers land almost exactly on "twice a second" - the user's very first description of this symptom, weeks/entries ago - rather than needing the "two salient states per 1s cycle" hand-wave used earlier today.

This simultaneously rules out every disabled optional stage as the source, since the effect survived and strengthened. Read `ssb_dsp_process_sample()` end to end (the one pipeline stage that's always on, unconditionally) and found nothing with a ~0.5s-scale time constant - just a short (~8ms) Hilbert FIR, memoryless atan2, unconditional lifetime accumulators (no periodic reset), and a static clamp. Leading theory: this is the already-well-characterized near-null "sticks" phenomenon (atan2/Hilbert fragility at envelope zero-crossings) occurring deterministically and repeatably because the two-tone input itself is exactly periodic (100Hz GCD) - if only some fraction of the ~100 null-crossings/second hit the specific floating-point corner case that triggers a stick, and that subset recurs every ~48-49 cycles, you'd get exactly this signature, with no separate timer or filter needed. Also explains why the non-integer-Hz test smeared the pattern instead of removing it.

Proposed next step: capture a 'J' jump-log and an audio recording of the same run simultaneously (Preset 1, 700/1700) to directly check whether the jump-log's own near-null event timestamps recur at ~0.485s/~0.97s intervals - would confirm this theory outright rather than leave it plausible-but-unverified. Full reasoning in `null_bias_investigation.md`. Analysis only, no firmware change.

## 2026-09-17, later still: the requested simultaneous `'J'`-log + audio capture arrives (`log_20260917_142917.txt` + `700-1700_Preset1.wav`) - it complicates rather than confirms the "0.485s is the true fundamental" theory from the previous entry, and the overreach gets corrected

The log turned out to be a single `slow_trace: AUTO-CAPTURED` event plus one manually-triggered `'J'` snapshot (8 entries within a 5ms window) - a one-shot capture, not a continuous multi-trigger series, so it can't actually test a recurrence-interval theory (that needs many repeated, timestamped trigger events to measure an interval from). Noted for next time.

The paired `700-1700_Preset1.wav` (same Preset 1/all-filters-off condition as the previous entry, but 700/1700 instead of 733/1700) gave a clean single ~1.003s autocorrelation peak with NO 0.485s component - checked explicitly, correlation at 0.485s came back negative (-0.111). This directly contradicts the previous entry's "0.485s is the universal true fundamental, 1.000s is just its 2nd harmonic" claim.

Correcting that overreach rather than letting it stand: that claim was drawn from a single recording (733/1700) and shouldn't have been generalized. The pattern now looks like every 700/1700 (100Hz-GCD) test done today shows one clean ~1.000s peak with no 0.485s substructure, while both 733-based (coprime, GCD=1) tests have shown the extra ~0.485s component - suggesting it may be specific to the 733Hz/coprime-pair class rather than a universal property. Genuinely still open, just narrowed and honestly corrected.

This gap - a single log snapshot can't test a recurrence-interval theory - is exactly what a full-rate per-sample capture feature would solve; see the next entry for its design and implementation.

## 2026-09-17, later still: implemented the requested full-rate freq/env capture - new `'F'` serial command

Built the feature the user asked about ("would it be worth doing a dump of all freq/env values every sample for a second or so"). `'F'` arms a capture that lazily allocates a ~125KB buffer (16000 samples, ~1.0s at 16kHz - matching the sample rate and today's own NCO-fix wrap period) for `raw_freq_dev_current`/`raw_envelope_current`, fills it from the existing per-tick diagnostic hook (`diagnostics_set_tx_info()`), then dumps it as CSV in small chunks from `diagnostics_service()` (same TX-buffer-safety pattern as every other print in this file) once full, freeing the buffer afterward. Not a permanent static buffer - the board (ESP32-S3 Super Mini) is assumed to have no PSRAM, so it's allocated only while actually capturing. AD9851-only, same as `'J'`/`'K'`/`'H'`. Gives direct per-tick ground truth to check the still-open "does a near-null event recur at ~0.485s/~0.97s" theory against, instead of inferring it from SDR audio. Verified brace/paren/`#if` balance across all three edited files (`diagnostics.h`, `diagnostics.cpp`, `serial_commands.cpp`) - no ESP32 toolchain available here to do a real build, so this is unverified beyond that; not yet bench-tested, ready to flash. Full design in `null_bias_investigation.md`.

## 2026-09-17, later still: `'F'` works first try on real hardware (`log_20260917_153446.txt`, 700/1700 Preset 1) - it overturns one part of the leading theory and surfaces a new, more precise open question

Near-null "sticks" turn out to happen at essentially EVERY null crossing (1000 events across the 16000-sample/1.0s buffer, one every ~1ms) - not a rare subset recurring every ~48-49 cycles as the previous entry theorized; that specific claim is retracted. What actually varies is the glitch VALUE at each null: it mostly locks into an exact, bit-for-bit repeating 5-event (~5ms) template (70+ perfect repetitions seen in the first 0.346s of this capture), but 16 of the 1000 events (1.6%) briefly take on a different value instead, clustered into 6 short groups spread unevenly across the remaining ~0.65s of the capture - none in the first third, then a denser run later on. One second of data isn't enough to responsibly claim these 6 "slip" clusters recur at a specific interval (loosely near-matching either the ~0.485s or ~0.97s audio-derived numbers, but flagged explicitly as "worth watching for," not a finding, given how the last two entries had to walk back similar small-sample overreach). Next step: a longer single `'F'` capture (would need `FREQENV_CAPTURE_LEN` increased past 16000, RAM-budget permitting) to actually get enough slip events in one continuous window to measure their own recurrence directly. Full numbers in `null_bias_investigation.md`. Analysis only, no firmware change this turn.

## 2026-09-17, later still: independent from-scratch simulation of `ssb_dsp.c`'s real algorithm reproduces `log_20260917_153446.txt`'s exact glitch values, sub-Hz precision - the near-null "sticks" mechanism is now CONFIRMED math, not board noise

Built a Python port of `ssb_dsp_process_sample()`'s actual pipeline (real 65-tap Hilbert FIR, `fast_atan2`/`fast_sqrt`'s real approximations, float32 throughout) driven by the same deterministic 700/1700Hz two-tone generator, with nothing tuned to match the capture - every constant taken directly from the source. Result: every value in the capture's exact repeating template (`-6802.0, 6003.4, 6169.4, 6171.2, 6004.1` Hz) appears in the simulation's own glitch-value set to within ~1Hz, the rarer "slip" values match too (-7705, -7891, 7600 clusters all present), event rate matches (~1 stick per 16 samples in both), and even the slip RATE matches closely (1.4% sim vs 1.6% real). This confirms the near-null sticks - ubiquity and magnitude both - are expected, deterministic math (`fast_atan2`/`fast_sqrt` phase noise at envelope zero-crossings, exactly what the existing clamp comment already names), not board-specific noise or a hardware defect. Still open: what determines which specific slip value occurs when, and whether that connects to the ~0.485s/~0.97s audible warble - this explains the raw mechanism, not its higher-level timing. Simulation script kept out of the repo (ad hoc analysis, same as this session's other Python work) and delivered directly. Full numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: `'F'` capture length doubled (1.0s -> 2.0s, ~125KB -> ~250KB) after the user reported this board's real free-RAM headroom from an actual build

User reported the board's build output directly: ~292KB free at link time against a 320KB chip (no PSRAM, as assumed), comfortably more than the original 1s capture needed. Bumped `FREQENV_CAPTURE_LEN` 16000 -> 32000, updated every place that quoted the old size (`diagnostics.h`, `diagnostics.cpp`, `serial_commands.cpp`), and deliberately stopped at 2.0s rather than spending the whole reported headroom, since that pool is shared with task stacks and other runtime heap use the link-time report can't see - the existing malloc-failure fallback protects against guessing wrong. Gives room to catch 2-4 repetitions of the still-open ~0.485s-0.97s slip-timing question in one continuous window. Not yet bench-tested at the new size. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: `'F'` no longer interleaves other Serial output with its CSV - user's external logger was choking on it

The first capture parsed cleanly by hand but only because ~192 stray non-CSV lines (blank lines, one `slow_trace: AUTO-CAPTURED` print) had to be filtered out first - the user's own logger can't do that and flagged it as a real problem. Fixed by having `diagnostics_service()` skip everything else it normally does (status line, timing block, canary check, slow-trace/held-freq auto-dumps) for as long as a freqenv capture is armed, filling, or dumping - a hard early-return covering the capture's whole lifecycle, not just the CSV-printing phase. Tradeoff accepted: those background monitors go quiet for up to a couple seconds during a capture, same as this feature already being mute-exempt for the same "explicit, one-shot, user-requested" reasoning. Not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: MAJOR finding - the raw freq_dev/envelope signal is PROVABLY exactly periodic at 1.000s for any tone frequency (proven by simulation, confirmed on real hardware to 40-microsecond precision), which explains today's whole ~1.000s thread but puts the earlier 733-1700 "~0.485s" finding in tension

Second `'F'` capture (`log_20260917_160237.txt`) arrived truncated (user's logger kept only the last 0.54s of the 2.0s buffer - flagged, suggested a dedicated file-logging tool instead of terminal scrollback next time), but the CSV itself had zero interleaved lines - the suppression fix worked. That partial data's slip-cluster timing turned out identical (to 40 microseconds, across 5 independent points) to a segment of the FIRST capture, offset by a constant ~1.019s. Checked why via simulation: `ssb_dsp_process_sample()`'s output is exactly periodic at 16000 samples/1.000s to zero float32 difference, for EVERY tone-frequency pair tried (700/1700, 733/1700, and the non-integer-Hz control) - a direct, provable consequence of the tone generator's own NCO-fix recomputing phase from a sample index that always wraps at exactly 16000, regardless of f1/f2. This is a complete, hardware-confirmed explanation for why the raw digital signal repeats every ~1.000s - not a bug, a structural certainty of the current tone-generator design.

The catch: this same proof means the digital signal CANNOT produce the ~0.485s component recorded earlier today for `733-1700_Preset1.wav` (733/1700 is now confirmed exactly-16000-sample-periodic too) - that earlier finding isn't wrong exactly, but needs re-examination: either something downstream of the core DSP that Preset 1 doesn't fully silence, or the SDR/audio side, not this mechanism. Flagged as the concrete next step rather than resolved. Full numbers and reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: requested 733/1700 `'F'` capture comes back full and clean (`log_20260917_162817.txt`) - it's ALSO exactly periodic at 1.000s on real hardware (zero difference, whole 2.0s buffer), which settles the previous entry's open tension outright

Full 32000-sample capture this time, properly logged to a file (no truncation). Splitting the buffer at its midpoint and comparing the two halves directly: max difference = 0.0, bit-for-bit identical, across the entire 1-second period - for contrast, a non-period lag on the same data showed an 8.2kHz difference, so this isn't a degenerate/trivial result. This definitively rules out `ssb_dsp_process_sample()`'s own output as the source of the earlier ~0.485s finding in `733-1700_Preset1.wav` - that audio correlation was real, but it cannot come from the core DSP loop, now proven exactly periodic on the very same tone-pair/preset that produced it. Narrows the remaining mystery to two candidates, neither tested yet: something downstream (PWM mapping, relative_delay, envelope_interp, or the AD9851 chip's own analog behavior) that "Preset 1, all filters off" doesn't actually fully disable, or the SDR/audio-capture chain itself. Next step proposed: revisit the SDR-side test (manual/fixed gain) that was floated early today but never actually run, now that the ESP's own core DSP is doubly proven clean. Full numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: `'Q'` dither simulation looked like a fix (destroys the whole-second periodicity, 14677Hz cycle-to-cycle diff) but a real-hardware test came back NOISIER, not cleaner - dither now suspected to be the wrong tool, existing freq_dev slew-rate limiter proposed as the next candidate instead

Simulated the existing `'Q'` two-tone dither (unchanged, 0.05Hz/4Hz) against the newly-proven whole-second periodicity and confirmed it decisively breaks cycle-to-cycle repetition (max diff 0.0Hz -> ~14677.5Hz). Looked like confirmation `'Q'` already solves the problem with no retuning needed - user had asked for "test generator only" scope, which this respects.

Then the user actually tried `'Q'` on the bench and reported a NOISIER result, not an improvement - contradicting the simulation. Working theory: dither only shuffles WHEN/how correlated the ~5000-9000Hz near-null `freq_dev` spikes are, it doesn't shrink them - so a single coherent periodic buzz became the same total spike energy smeared randomly across the run, which likely sounds worse even though the coherent tone is gone. User then asked whether `fast_atan2`/`fast_sqrt` themselves should be randomized to kill "characteristic output patterns" - reviewed both functions and found neither is a lookup table/quantizer, both are smooth deterministic polynomial approximations, so the repeating glitch "template" seen earlier is a symptom of the tone generator's proven exact periodicity revisiting the same (I,Q) pairs, not something separately baked into the trig functions - randomizing them would very likely hit the same "redistributes but doesn't reduce" problem, and would also touch the always-on live-TX DSP path, a bigger scope change than agreed.

Proposed a different, already-built candidate instead: the freq_dev slew-rate limiter (`ssb_dsp_set_freq_dev_slew_limit_hz()`, `'{'`/`'}'`, off by default), which caps the raw SIZE of each sample-to-sample jump rather than trying to decorrelate timing - its own doc comment already suggests it was built with exactly this ~8000Hz-scale glitch in mind. Not yet tested against this specific problem. Documentation-only update so far (`test_signals.h`'s `'Q'` doc comment now carries this full story); no firmware behavior changed. Next step: an `'F'` capture with the slew limiter enabled, and clarifying with the user exactly what "noisier" meant in their bench test. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: decided to bench-test the existing freq_dev slew limiter next (not trig randomization); "noisier" clarified as audible on the demodulated signal

User confirmed: test the freq_dev slew-rate limiter (`'{'`/`'}'`) next rather than pursuing fast_atan2/fast_sqrt randomization, and clarified the earlier `'Q'` result was noisier specifically as HEARD on the demodulated/received audio (not a waterfall observation, not the null_bias diagnostic numbers). No firmware change needed - the limiter and its runtime toggle already exist and are already wired into the live DSP path. Wrote out a concrete bench procedure in `null_bias_investigation.md`: `'Q'` off for a clean comparison, one `'{'` press (0 -> 2000Hz limit, inside the range its own doc comment says should catch an ~8000Hz-scale null event), run the same 700/1700 or 733/1700 two-tone Preset-1 test, listen for whether the periodic buzz shrinks without new broadband noise appearing, and ideally take an `'F'` capture with the limiter on to directly confirm spike magnitude is actually being clamped. Documentation/planning only - no code changed this turn. Full procedure in `null_bias_investigation.md`.

## 2026-09-17, later still: simulation explains the bench result on the slew limiter - it really does shift the center frequency (a genuine, steady-state side effect), and doesn't remove the recurring warble pattern either

User reported from the bench: the slew limiter measurably shifts the perceived/measured center frequency depending on its setting, but the warble sounds about the same regardless of setting. Built a simulation (`sim_slew_limiter_test.py`, scratchpad, exact port of `ssb_dsp.c`'s real slew-limiter algorithm in its real pipeline position) to check both claims quantitatively on 700/1700 two-tone. Confirmed both, and explained the mechanism for each: (1) the frequency shift is real and steady-state (not a toggle glitch) - up to +16.75Hz at the loosest engaged setting (2000Hz), shrinking toward the raw baseline as the limit tightens (+0.98Hz at the 100Hz floor) - because forcing a near-null spike's excess deviation to ramp gradually spreads it into neighboring samples where envelope has already recovered, which DO count in the physically-correct envelope-weighted average frequency (unlike the single suppressed near-zero-envelope spike sample itself); (2) the warble stays similarly present because slew limiting doesn't change WHEN or how OFTEN the ~1000/second near-null events happen, only how big each one is allowed to get - if the audible character comes from the event RATE rather than peak size, no slew setting removes it. Peak magnitude does drop a lot with tighter settings (7912Hz raw down to 1403Hz at the tightest step) but that alone doesn't appear to fix the audible problem. Net: like `'Q'` dither, this is a real, testable, but ultimately unconvincing fix - both attack a proxy, not the null-crossing event itself. Makes the two already-drafted (never implemented) fix directions in `null_bias_investigation.md`'s open items - directly reworking the near-null phase/frequency computation - look like the more promising next step. Documentation/analysis only, no firmware changed. Full numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: CORRECTION - the slew limiter's real problem is intermodulation distortion of the two tones themselves, not a small center-frequency side effect; both tried mitigations (dither, slew) now ruled out on real hardware

User's actual bench readings (~500Hz shift at a very tight clamp, ~250Hz at a 4000Hz clamp) didn't match the previous entry's simulated ~17Hz weighted-average estimate, in size OR direction - a sign the earlier metric was measuring the wrong thing. Rebuilt the analysis properly: reconstructed the actual transmitted complex baseband signal from freq_dev/envelope and took its FFT to see exactly where the two tones land spectrally (sanity-checked against the known-clean raw case, which reproduces exact 700.0/1700.0 Hz peaks). Result: the slew limiter doesn't shift the two tones as a pair - at any setting tight enough to meaningfully cut the ~8000Hz null spikes, it smears them into a broad cluster of intermodulation products with no clean tone to even measure a "shift" against, because a 700/1700 two-tone's own 1000Hz beat frequency already needs real slew rates comparable to the very limits meant to only catch null glitches. Only very loose settings (6000-8000Hz) stay clean, but those barely touch the actual spikes.

**Net status after three real-hardware-tested angles**: both `'Q'` dither and the freq_dev slew limiter have now been tried and both made things measurably worse, not better - neither can tell a null-crossing glitch apart from legitimate signal content, so both process every sample identically. This points hard at the two never-implemented, envelope-gated fix directions already sitting in `null_bias_investigation.md`'s open items as the next real thing to try, since they're the only proposals that touch only the samples actually flagged as near-null. Documentation/analysis only, no firmware changed. Full spectral numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: user fixed a separate two-tone freq-gen bug on the bench; post-fix the ~1s periodicity reads as a "wobble"/raised noise floor rather than a narrow spur; pushed back on the proposed phase/freq dither fix and flagged that the "noise" may actually be the already-proven exact comb, just unmasked

User fixed an unrelated small bug in two-tone frequency generation on the bench (details not yet seen here). Post-fix, DSP math still confirmed correct, and the proven ~1.000s periodicity now shows up as a "wobble" with a much higher wideband SDR noise floor, instead of the previously-seen narrow jumping spur - user notes this resembles the `'Q'` dither result and proposes adding a small dither directly to phase/freq_dev near the fast-trig computation to break the periodicity.

Pushed back with two points: (1) this is mechanistically the same trade as `'Q'`, which already made real measured results worse, not better - dither redistributes glitch energy rather than reducing it, regardless of how small the perturbation is (the "small = safe" reasoning addresses a different risk than the one already observed); (2) the reported "noise floor" may not be genuine noise at all - since the raw signal is proven bit-for-bit periodic at exactly 1.000s, its true spectrum is a comb of discrete 1Hz-spaced lines, which most SDRs' resolution bandwidth can't resolve and would display as a raised noise floor even though it's fully deterministic. Proposed a cheap test (narrow the SDR's RBW / take a long high-res FFT) to check before any code change, and suggested the fixed freq-gen bug may have been inadvertently dithering things before today, which would explain the spur-character change. Recommended moving to the two already-drafted, envelope-gated near-null fix directions instead of another dither variant. Documentation/discussion only, no firmware changed. Still waiting on the freq-gen fix's details and the RBW test result. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: the "unresolved 1Hz comb" hypothesis is FALSIFIED at 0.05Hz real RF resolution - genuinely broadband, not a comb - which reopens the never-tested "downstream of the core DSP" candidates from earlier today

Two clarifications resolve open questions from the last entry. First, "today's freq-gen fix" is the already-known NCO fix (accumulate-based phase generator -> exact wrapping-index recompute-every-tick), not a separate bug - the user pasted its doc comment directly. This confirms the old generator's float32 accumulation drift was itself acting as an unintentional, tiny dither source. Second, and more importantly: the user CAN resolve the real RF spectrum to 0.05Hz (20x finer than a 1.000s-periodic comb's 1Hz line spacing) and it does NOT show a comb - genuinely broadband content dominates. Also confirmed the OLD, less-exact tone generator gave a measurably PURER tone (lower noise floor) despite jumping around, than the mathematically-exact NEW one does - the opposite of what "the core DSP's periodicity is the audible cause" would predict.

Since the digital freq_dev/envelope core is now proven exactly periodic three separate ways, an exactly-periodic input through any fixed deterministic chain must produce an exactly-periodic (comb) output - no exception, by basic Fourier theory. Real broadband content instead means the actual transmitted signal is not exactly periodic, and the gap can only be introduced somewhere downstream of `ssb_dsp_process_sample()` - reopening the "candidate 2" list flagged (and never tested) much earlier today: `relative_delay_apply()`, `envelope_interp_on_full_tick()`, the envelope PWM output mapping, or the AD9851 chip's own analog/SPI/PLL behavior. Recommendation now supersedes the previous entry's "design the near-null DSP fix next" - that DSP output is unusually well-verified at this point; the downstream chain is not, and is now the more promising target. Requested the user's actual capture/recording to verify directly with the same FFT tooling used earlier today, rather than relying on a description. No firmware changed. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: analyzed the real SDRuno IQ captures directly - it IS real comb structure, not smooth noise, but ~25-30dB stronger than the verified core DSP alone predicts, pointing the dominant cause downstream

Analyzed both uploaded SDRuno IQ captures (Preset 1 slew-off, Preset 3) directly as complex I/Q baseband signals. Confirmed the genuine two-tone signal in both (exact 1000Hz tone spacing), measured Preset 1's skirt (-3dB width ~5Hz growing to -30dB width ~204-214Hz - a real, large skirt matching "~150Hz-ish" by eye), and found that at native 0.054Hz resolution it resolves into a genuine discrete comb, not smooth noise - refining, not reversing, the earlier falsification of the "unresolved 1Hz comb" idea (there IS comb structure, just coarser than 1Hz spacing).

Decisive step: simulated the already-proven-exact core DSP signal alone (no downstream stages) over one exact 1.000s period and compared its own spectrum at the same kind of resolution. It shows a real but much lower (-53 to -60dB) broadband-looking floor from the near-null glitches - a genuine, now-directly-demonstrated contribution - but does NOT reproduce the sharp ~100-200Hz-spaced comb teeth real hardware shows reaching -25 to -30dB, a ~25-30dB gap. This means the core DSP contributes a real but minor floor; the DOMINANT structure seen on the bench must come from downstream (`relative_delay_apply()`, `envelope_interp_on_full_tick()`, PWM output mapping, or AD9851 SPI/PLL behavior) - the same candidate list from earlier today, now on a quantitative footing rather than a Fourier-theory argument alone. Preset 3's capture showed a visually smoother floor with no obvious discrete comb teeth, unlike Preset 1 - possibly its extra processing (EQ/compressor/AGC) decorrelating whatever produces Preset 1's sharper comb. No firmware changed - analysis only. Full numbers and plots in `null_bias_investigation.md`.

## 2026-09-17, later still: settings.h shows Preset 1 zeroes/disables relative_delay, gdeq, ampeq, and envelope_interp - removing them as comb candidates; user's AM/D-correction evidence narrows further; two new AD9851-path candidates raised (FQ_UD latch margin, known Fs/wakeup jitter)

Re-checked an assumption before handing it to the user as the leading candidate: `relative_delay_apply()` is NOT "always active" under Preset 1 - `settingsPresets[1]` stores `relative_delay_samples=0.00` (an identity pass-through at exactly zero), plus `env_gdeq_enable`, `envelope_interp_enable`, `env_predistort_enable`, `env_ampeq_enable`, and `env_ampeq_shelf2_enable` all false/zero-filled. Every envelope/phase-conditioning stage on the last two entries' downstream list is off or a no-op under Preset 1 - correcting, not extending, the previous entry.

User's own evidence points the same way: the comb-adjacent noise isn't present on AM (rules out generic mains/PSU coupling), and disabling predistort with zero PWM scale/mid offset (exercising the one live envelope-path lever hard) left the tone pair and its noise "very similar to before." Envelope/correction chain and analog PWM path both substantially ruled out; what's left live under Preset 1 is the bare PWM offset/scale mapping and the AD9851 SPI/DDS path.

Checked project history before re-offering the AD9851 BS170/level-shifter theory as fresh: already tested today via the push/pull driver fit, "no visible improvement" for closely related symptoms. Confirmed today's Preset 1 capture was taken WITH those drivers fitted, so that negative result applies directly here - not re-proposing that specific mechanism without new justification.

User raised two more specific, distinct candidates: (1) **FQ_UD's own latch-edge margin** - the AD9851 only actually latches new data on FQ_UD's edge, and that edge currently has zero settling margin (`AD9851_BITBANG_EDGE_DELAY_ENABLED=0` disables it along with the per-bit delays). History check: FQ_UD's own delay was once kept in place as a separately-justified "cheap" margin when the per-bit delay was trimmed back (2026-09-10), but got swept into the blanket disable the next day and never tested in isolation, and never at all since the push/pull driver fit. (2) **This project's already-documented Fs/wakeup jitter** (`max_gap_us` ~71-78us against the 62.5us tick budget, cross-core scheduling contention, per 2026-09-10/11 entries) - worked through why this plausibly matters here: the AD9851 free-runs its DDS core between updates, so jitter in WRITE timing distorts things in proportion to how fast `freq_dev` is changing at that moment (same delta(t)*rate-of-change mechanism already used on the AM side of this project) - and `freq_dev` changes fastest right at nulls, exactly where the comb is seen. The two candidates could stack (signal-integrity margin vs. firmware write timing).

No firmware changed. Next tests identified, not yet run: re-enable only the FQ_UD-latch delay and re-test hands-off; scope FQ_UD vs. the last W_CLK edge now that push/pull drivers are fitted; correlate `max_gap_us` timing against a comb-showing capture to see if comb timing tracks wakeup jitter rather than the two-tone signal itself. Documentation/discussion only. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: added a firmware toggle ('O') to revert to the pre-NCO-fix "pure but jumping" two-tone phase generator on demand, for a sanity-check A/B without a reflash

User asked whether the old accumulate-based two-tone phase generator was left in place after today's NCO fix (`a32c50c`), wanting to revert to it as a sanity check and confirm nothing else relevant changed since. Checked: it was NOT kept as a fallback - fully replaced for tone1 (always) and tone2 (dither off); the only surviving trace is tone2's own dither branch, a separate code path. Checked every commit since the fix: `ssb_dsp.c`, `AD9851.c`, `relative_delay`, the envelope/gdeq/ampeq/interp files, and `settings.h` are all untouched - everything else that changed is docs or diagnostics/capture instrumentation, none of it touching the signal/RF path. One caveat: `TWOTONE_BAND_PRESETS` gained one extra temporary test entry since then (doesn't change any existing pair, just adds one more `'T'` cycle stop).

Added the requested toggle: **`'O'`** (mirrors `'Q'`'s pattern) in `test_signals.cpp`/`.h` and `serial_commands.cpp`, plus a boot-banner line. Off by default (today's exact-recompute generator is unaffected); when ON, tone1 always reverts to the old accumulate-and-subtract phase update, and tone2 does too unless `'Q'` dither is on (dither's own tone2 path already used the accumulator regardless). `s_tone_sample_index` keeps advancing either way so toggling back off resumes cleanly. A small one-time phase discontinuity on the switching tick is expected/accepted, same as `'Q'`'s and `'T'`'s own transitions. Committed; not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: 'O' does not reset the tone generator; new Preset 3/ADC-off capture shows a dramatically cleaner floor - strong support for the Fs/wakeup-jitter candidate

Confirmed from the code: `test_signals_set_twotone_legacy_phase_enabled()` is a plain flag flip - it does NOT reset phase/sample-index state on either transition, by design (so the exact-recompute path resumes cleanly off the still-advancing index when 'O' goes back off). Same one-time-discontinuity tradeoff already documented for 'O'/'Q'/'T'.

Analyzed a new capture, "Preset 3 with ADC off" (`SDRuno_20260917_204619Z_14175kHz.wav`), against the two 19:2x captures from earlier today:

| capture | -30dB width | far floor | near-tone floor | near-far delta |
|---|---|---|---|---|
| Preset 1, slew off | 3.2 Hz | -89.6 dB | -62.1 dB | +27.5 dB |
| Preset 3 | 10.5 Hz | -84.5 dB | -49.9 dB | +34.7 dB |
| **Preset 3, ADC off** | **0.7 Hz** | **-100.6 dB** | **-88.1 dB** | **+12.4 dB** |

A large, unambiguous improvement with the ADC off: near-tone floor down 38dB vs. Preset 3/ADC-on (~6300x power), far floor down 16dB, skirt width down from 10.5Hz to 0.7Hz - close to the core-DSP-only prediction from several entries ago. This lines up directly with the Fs/wakeup-jitter candidate raised last entry: this project's own diagnostics already attribute part of `max_gap_us` scheduling jitter to ADC-ISR-priority contention, so removing the ADC removes that contention source - exactly the kind of broad floor/skirt improvement a write-timing-jitter mechanism would predict, not a narrow spectral change. First piece of evidence in this investigation that moves a candidate from "plausible mechanism" to "matches a real, controlled A/B."

Not yet established: exactly how "ADC off" was done on the bench (current mainline firmware's own comments say the ADC "always runs, no longer conditional," so this wasn't a stock toggle) - matters for interpreting the result precisely. Recommended next step: pull `[timing] max_gap_us`/`overruns` for both ADC-on and ADC-off conditions on the same preset to confirm jitter actually dropped, closing the loop causally. Documentation/analysis only, no firmware changed. Full numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: CORRECTION - ADC was already disabled in ALL recent captures, including the two "noisy" ones; the previous entry's ADC-off/wakeup-jitter causal story is retracted

User clarified: every recent capture with the new exact-recompute tone generator - both the "noisy" 19:2x captures AND the 20:46 "low noise benchmark" - was built with `ADC_CAPTURE_ENABLED=0` (config.h:142, a compile-time flag gating whether the ADC's continuous DMA driver starts at all - confirmed real by a 2026-09-02 hardware finding that this flag kills a measurable 5kHz DMA/ISR-driven noise pulse elsewhere in the system). ADC state was constant across all three captures, not the variable distinguishing clean from noisy - so **the previous entry's central claim (ADC-off explains the ~26-38dB improvement, confirming the wakeup-jitter candidate) is retracted.** That candidate returns to "plausible, untested" status, same as FQ_UD-latch-margin.

This reopens the real question: what DOES differ between the 19:31 Preset 3 capture (-49.9dB near-tone floor) and the 20:46 one (-88.1dB), a ~38dB gap between two nominally-identical preset selections? Since preset reload should set every DSP-side lever identically, something else must have changed between the two sessions - a runtime toggle a preset reload wouldn't undo (relative_delay, gain, a gdeq/ampeq/predistort state left on), or something receiver-side (SDR gain/attenuation, AGC, antenna/dummy-load connection). Asked the user which. A genuine ADC-on-vs-off A/B remains a worthwhile separate test given the flag's own confirmed real effect elsewhere - it just isn't what today's captures already show. Documentation only, no firmware changed. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: RESOLVED - the 19:31/20:46 Preset 3 captures differed ONLY in tone generator; old generator's own float rounding is a much smaller, more surgical decorrelator than 'Q' dither

User confirmed the two captures were identical except for which tone generator was active - old (pre-NCO-fix) vs. new (exact-recompute) - and that the new `'O'` toggle reproduces this exact ~38dB A/B live, no other change needed. A genuinely clean, single-variable result: the OLD, less-exact generator measurably cleans up the near-tone floor and comb/skirt, confirming the earlier qualitative "old generator sounded purer" observation with hard numbers.

Working theory (not yet confirmed by simulation): the old accumulator's already-quantified drift (~+/-2e-4Hz/500s) is far too slow to explain an 18s-scale difference by itself - but its `phase += increment; phase -= two_pi` update carries forward history-dependent float32 rounding noise every tick, unlike the exact generator's fresh-every-tick recompute (no memory at all). Near-nulls recur every ~16 samples and `fast_atan2` is extremely sensitive there, so even a rounding-noise perturbation far smaller than `'Q'`'s deliberate +/-0.05Hz wobble could be enough to decorrelate cycle-to-cycle null glitches - while the exact generator reproduces the identical glitch at the identical null every single cycle, which is exactly what makes it a coherent comb (the "exact periodicity -> exact comb" argument from several entries ago, now with a concrete "why it's audible" mechanism attached). This reconciles rather than contradicts the earlier negative `'Q'` result: both decorrelate periodicity, but `'Q'` is a large, deliberately audible-scale wobble with its own footprint, while the old generator's rounding noise is apparently a much smaller, more surgical perturbation.

If this holds, the right fix is neither reverting to the buggy accumulator (reintroduces a real drift bug) nor `'Q'` at its current magnitude - it's finding the smallest perturbation that reproduces the old generator's benefit without either liability. Next steps, none yet run: more `'O'` A/B runs across bands to build confidence; simulate the old accumulator's actual per-tick rounding-noise magnitude (distinct from its slow drift); test `'Q'` at a much smaller magnitude as a direct prediction of this theory. Documentation only, no firmware changed. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: simulation FALSIFIES the "old generator's rounding noise decorrelates glitches" theory; redirects to Preset 3's active gdeq/ampeq/relative_delay/predistort stack instead

Built and ran the direct test: exact old-accumulator generator vs. the verified exact-recompute generator, both through the full verified core-DSP simulation over 20 cycles. Two findings kill the previous entry's theory as stated: (1) the OLD generator's cycle-to-cycle `freq_dev` difference is a smooth, monotonically DECAYING sequence, not noise - the signature of the already-known slow systematic drift, not new per-cycle rounding noise; (2) the simulated near-tone floor for OLD vs. EXACT generator is -184dB vs. -226dB - OLD is 42dB WORSE in simulation (wrong sign vs. the real 38dB-better hardware result), and both are ~100dB below anything physically meaningful, meaning the core DSP itself is functionally identical between the two generators.

Redirected theory: the real effect must be downstream, and Preset 3 (unlike Preset 1) has its entire downstream stack live - gdeq (candidate B), both ampeq shelves, predistort, and a real 2.00-sample (exact-integer) relative_delay. Plausible mechanism: these are LTI filters whose resonant response builds up more energy the longer an exciting frequency dwells exactly on it - the EXACT generator's glitches recur at bit-identical instants forever (a rigid comb that can sit on a resonance indefinitely), while the OLD generator's glitches slowly slip cycle to cycle (never dwelling exactly on one frequency), potentially starving any downstream resonance of sustained excitation. Speculative, not yet simulated with the real downstream stages.

User is independently running the decisive discriminating test: `'O'` A/B on Preset 1 (nothing downstream active) instead of Preset 3. Little/no difference there would confirm the downstream stack as the mechanism; a comparably large difference would contradict this and point back at the core DSP/AD9851 path. Awaiting result. Documentation/simulation only, no firmware changed. Full reasoning and numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: Preset 1 'O' test comes back "basically the same" - FALSIFIES the downstream-stack theory, redirects to whatever's active on every preset

User's decisive test: `'O'` A/B on Preset 1 shows the same old-vs-new gap as Preset 3, just with a different settled "off" frequency (-10Hz vs -18Hz) - expected, since `relative_delay`'s value is already known to tune that offset (Preset 1=0.00 vs. Preset 3=2.00). Since Preset 1 has zero live downstream conditioning - confirmed by re-reading `relative_delay.cpp`: at `delay=0.00`, `interp_ring()` returns an exact bit-identical pass-through, no blending at all - the previous entry's "LTI resonance in gdeq/ampeq" theory is falsified. It predicted little/no gap on Preset 1; that's not what happened.

Redirects to mechanisms active on EVERY preset: the AD9851 SPI/FQ_UD write path (FQ_UD-latch-margin candidate, not yet tested), Fs/wakeup jitter (also not yet actually tested - ADC has been off in every capture so far), or the basic PWM/analog envelope path (partially discounted earlier, but not against this exact comparison). Recommending splitting FQ_UD's own edge-delay out of `AD9851_BITBANG_EDGE_DELAY_ENABLED` into its own flag, so it can be A/B'd via `'O'` without paying the per-bit delays' separate real-time cost - proposed, not yet implemented. Documentation only, no firmware changed. Full reasoning in `null_bias_investigation.md`.

## 2026-09-17, later still: DECISIVE - the two generators' raw freq_dev output statistics genuinely differ in exactly the way that stresses the AD9851 write path

Redid the comparison the right way, per direct pushback: compared the actual tick-to-tick `|freq_dev[n]-freq_dev[n-1]|` step (the same quantity as this project's own `max_freq_dev_step` diagnostic) over 60 cycles for both generators, instead of an idealized FFT reconstruction. Result is decisive: count of steps >8500Hz is 332/cycle for the EXACT generator vs. only 2.7/cycle for OLD (~124x); >9000Hz is 203/cycle vs. 0.83/cycle (~244x). The EXACT generator's per-cycle worst-case step is EXACTLY 10226.39Hz on every single cycle (std=0.0), always at the identical sample index (2968) - perfectly reproducible by construction. OLD's worst case averages only 8049Hz, varies cycle to cycle (std=289Hz), and its location wanders across a 768-sample span.

This reopens the AD9851/level-shifter family of theories on solid new footing: this project's own history already documents that jumps in the ~8562Hz-class range can flip enough DATA bits at once to outrun a level shifter's settling time. The EXACT generator routinely and reliably exceeds that zone every single cycle at fixed positions; OLD mostly doesn't. If AD9851 signal integrity degrades above that threshold, EXACT would trigger it with total reproducibility (a strong discrete comb) while OLD would trigger it rarely and unpredictably (lower, less coherent noise) - matching the real hardware result. Sample index 2968 is now a known, precisely reproducible trigger point for a targeted scope/`'J'` capture. Strengthens the case for the already-proposed FQ_UD-latch-margin flag split. Documentation/simulation only, no firmware changed. Full numbers in `null_bias_investigation.md`.

## 2026-09-17, later still: split FQ_UD's own latch-edge delay into an independent flag (`AD9851_FQUD_EDGE_DELAY_ENABLED`, starts at 1) for isolated bench testing

Implemented the split proposed last entry: `AD9851.c` gets a new, independent `AD9851_FQUD_EDGE_DELAY_ENABLED` flag guarding just the FQ_UD-latch `ad9851_edge_delay()` call (end of `ad9851_set_frequency()`'s bit-bang tail), separate from `AD9851_BITBANG_EDGE_DELAY_ENABLED`'s two per-bit DATA-settle delays and their own real, measured real-time-budget cost. Either flag alone now enables the FQ_UD site; the original flag still also covers the per-bit sites unchanged. `half_period_cycles` (the delay's duration) is already computed unconditionally whenever bit-bang mode is active, so no other change was needed - fully isolated, one-file edit.

Set to `1` (not the usual off-by-default) since the user wants to test it now rather than needing a second edit/reflash. Motivated directly by today's `'O'` A/B finding: the EXACT tone generator reliably drives freq_dev jumps past ~8500-9000Hz hundreds of times per cycle at fixed positions (squarely in this project's own documented AD9851-corruption range), while the OLD generator mostly doesn't. If enabling just this one delay narrows the `'O'` A/B gap, that confirms FQ_UD margin as a real contributor; if not, this candidate is cleared. Not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18: FQ_UD-latch-margin test comes back negative; deeper simulation characterizes the two generators' real difference and reframes the "dither" idea

`AD9851_FQUD_EDGE_DELAY_ENABLED=1` (added last entry) tested on the bench: "no noticeable diff" on the `'O'` A/B - this candidate is cleared by ear (a follow-up IQ capture would confirm it quantitatively, not yet done, not blocking). The per-bit DATA-settle delays (`AD9851_BITBANG_EDGE_DELAY_ENABLED`) remain a separate, still-open candidate.

Built a follow-up simulation (`sim_perfect_vs_real_and_dither_idea.py`) answering the user's three-part question. **Perfect-implementation comparison**: even a mathematically perfect (float64, real atan2/sqrt) DSP shows a comparably huge single-tick swing at the null (max step 8002Hz) vs. the real fast_atan2/float32 implementation (9796Hz, ~22% more) - the near-null jump is inherent to destructive interference itself, not a `fast_atan2`/float32 artifact, though the real approximations do add real extra amplification on top. **New finding**: the true fundamental period of the two-tone pair is actually 160 samples (gcd(700,1700)=100), not the 16000-sample period used throughout this investigation - and the EXACT generator is surprisingly NOT bit-exact at that true period (max diff 15285Hz in DSP output; confirmed at the raw-audio level too, max diff 0.000172 at 160-sample lag vs exactly 0.0 at 16000-sample lag), because its `fmodf(idx*f/Fs,1.0)` computation rounds slightly differently depending on `idx`'s magnitude even for mathematically-equivalent phases. This means each of the ~100 true sub-cycles per 1-second period gets a subtly different near-null "alignment quality," one of which (sample 2968, already known) is a fixed, especially severe knife-edge that recurs identically forever since `idx` always retraces the same sequence - while the OLD generator's continuously-drifting accumulator never revisits the same alignment twice, effectively resampling that landscape rather than freezing on the worst case.

**Dither idea, tested directly, gives a counterintuitive result**: a clean, deterministic, symmetric frequency ramp (both tones, unlike `'Q'`'s tone2-only wobble) on an otherwise near-perfect generator, even at a vanishingly small magnitude (0.001Hz/s), immediately created hundreds of extreme (>8500Hz) jumps per cycle where zero-ramp had none - consistent with `'Q'`'s own prior negative result, not the "dither helps" intuition. Reframed conclusion: the real lesson isn't "add dither" generically, it's "never let the near-null alignment landscape become perfectly, permanently fixed" - which the OLD generator's own unintended imprecision happens to achieve better than either `'Q'` or a clean ramp did here. Flagged as a hypothesis needing a dedicated perturbation-strategy sweep (not yet designed) against the tick-to-tick extreme-jump metric, not a ready answer. Documentation/simulation only, no firmware changed. Full reasoning and numbers in `null_bias_investigation.md`.

## 2026-09-18, later: MOD160 (true-period-native phase) simulates as a strong fix - eliminates the >8500Hz danger zone entirely; targeted near-null dither tested and doesn't help; AD9851 phase-offset modulation checked and confirmed unused

Tested three follow-up ideas directly (`sim_mod160_and_targeted_dither.py`). **MOD160**: wrapping the generator's index at the TRUE 160-sample period (from last entry's `gcd(700,1700)=100` finding) instead of 16000 - proper numerical range-reduction, not dither - drops the max tick-to-tick step from EXACT's 10226Hz down to 8001.55Hz (matching the mathematically-perfect floor almost exactly) and eliminates ALL excursions above 8500/9000Hz across 60 cycles (0/cycle, vs. EXACT's 332/cycle and even OLD's 2.7/cycle). Confirmed genuinely bit-exact periodic at 160 samples in steady state (0.0 diff after the startup transient). Tradeoff: its own (now-safe) worst case repeats 100x/sec instead of 1x/sec - more rigid, finer-grained periodicity - but since real hardware risk in this project ties to jump MAGNITUDE crossing a threshold, not periodicity per se, and MOD160 never crosses it, **this looks like the strongest candidate tested so far, better than either OLD or EXACT.** Recommending implementation (wrap `s_tone_sample_index` at the true period instead of `SAMPLE_RATE_HZ`) and bench test - needs a small design decision first on how the correct modulus is determined per band/preset (frequencies change via `'T'`), not yet implemented.

**Targeted near-null-only dither** (randomizing frequency only at the ~1146 known critical near-null sample positions, everywhere else bit-identical to EXACT) does NOT give a clean win: even a tiny +/-1Hz kick pushed the worst-case single step up ~50% (to ~14900-15200Hz) while only modestly reducing moderate-excursion counts (~15-20% fewer). Kicking frequency right at maximum sensitivity trades many moderate excursions for occasional larger ones - a worse hardware-risk profile, not a better one. MOD160 remains the clear winner of everything tested.

**AD9851 phase-offset modulation**: checked directly, confirmed never used - the chip's 5-bit phase field (part of its standard word, 32 steps at 11.25 degrees) is hardcoded to 0 in every write. A phase-word update would only touch 5 DATA bits instead of up to 32, directly relevant to the documented "too many simultaneous transitions overwhelms the level shifter" mechanism - but adopting it means redesigning the modulation architecture (this project uses frequency-deviation/chirp, not phase modulation of a fixed carrier) and 11.25-degree resolution's adequacy for audio SSB is unassessed. Flagged as a real, separate architectural option for a dedicated discussion, not a quick fix. Documentation/simulation only, no firmware changed yet. Full numbers and code details in `null_bias_investigation.md`.

## 2026-09-18, later still: 'O' extended to a 3-way cycle (EXACT/LEGACY/MOD160) for a direct on-hardware A/B/C

Per the user's request to bench-test MOD160 first: extended `'O'` from a plain bool toggle into a 3-way cycle (`EXACT -> LEGACY -> MOD160 -> EXACT...`), so all three tone generators from the last two entries' simulations can be A/B/C'd live without a reflash. `test_signals.cpp` gets a new `twotone_phase_gen_t` mode enum (replacing the old bool) and a second wrapping sample-index counter (`s_tone_sample_index_160`, wraps at the hardcoded true period of 160 samples) alongside the existing one; `generate_twotone_sample()`'s tail is now a 3-way switch instead of a 2-way branch, same structure otherwise. `serial_commands.cpp` and the boot banner updated to match, old bool getter/setter fully removed (no other file referenced them). Braces/parens verified balanced after the edit.

**Caveat worth remembering while testing**: the 160-sample modulus is only correct for tone pairs sharing the current 100Hz GCD structure - true of every real `'T'` band preset except the deliberately non-integer-Hz TEMP control entry (700.37/1700.61Hz), which MOD160 would corrupt with artificial phase discontinuities if selected together. Not yet bench-tested - implementation only, no hardware result yet. Full details in `null_bias_investigation.md`.

## 2026-09-18, later still: added 'M' - a separate mute for the three auto-dump watchers, distinct from 'v'

User asked for a mute covering all diagnostic auto-dumps. `'v'` already mutes the periodic status/[timing]/[adc]/[dsp]/null_bias block, but three separate watchers - canary mismatch detection, the `'K'` slow_trace jump-detector's automatic side, and the `'H'` held-frequency detector's automatic side - were deliberately built mute-EXEMPT from `'v'` so they'd keep catching rare events during unattended captures. Rather than fold these into `'v'` (which would silently weaken that existing safety property for anyone using `'v'` for its original purpose), added a new, separate toggle: `'M'`.

New `s_diag_autodump_muted` flag (independent of `'v'`'s own flag), same getter/toggle-with-always-printed-confirmation pattern as `'v'`. `diagnostics_service()`'s three previously-unconditional calls (canary/slow_trace-auto/held_freq-auto) now wrapped in one `if (!muted)` gate. Boot banner and serial handler updated. Braces/parens/`#if`/`#endif` verified balanced after the edit.

**Caveat to know before using it**: unlike `'v'`, these three do detection AND printing in the same gated call, so while `'M'` is on they don't even check their trigger condition - a canary's "first seen at" timestamp (if one fires while muted) will read as whenever `'M'` is turned back off, not the true onset, though the underlying latched/corrupted state itself isn't lost. A slow_trace/held_freq event similarly just waits, unprinted, until unmuted, then prints immediately - nothing is silently discarded, but reporting is delayed for as long as `'M'` stays on. Manual on-demand reads (`'K'`/`'H'`/`'D'`) are unaffected either way. Not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: first MOD160 bench result - broadband floor down to -80dB as predicted, but a new ~7.6Hz-spaced comb (~-20dB down) appears, not the ~100Hz spacing predicted; mechanism not yet identified

First real hardware test of MOD160 (added earlier today): broadband noise floor drops to -80dB, matching the simulation's prediction that eliminating the >8500/9000Hz jump zone would help a lot. But it's replaced by a discrete comb spaced ~7.6Hz apart at roughly -20dB down - NOT the ~100Hz spacing the audio-domain-only simulation predicted from the true 160-sample period. Frequency-stable once established (doesn't drift within a run), but its exact value seems to depend on Preset and/or start conditions - not yet characterized.

Checked the obvious "two clock domains beating" explanation by code inspection: the sample timer, ADC decimation, and envelope PWM carrier are all exact integer-ratio relationships with SAMPLE_RATE_HZ (verified in `ssb_mic_test.ino`/`config.h`/`envelope_output.h`) - none show the near-but-not-exact rational mismatch that would create a slow beat, so if this is a clock-beat it isn't one of these three. Leading (untested) hypotheses: `relative_delay`'s fractional-sample interpolation interacting with MOD160's now-rigid periodicity (consistent with "varies with Preset," since relative_delay differs by preset); AD9851 write-timing jitter imposing slow modulation (consistent with "varies with start conditions"); or a capture/FFT-resolution artifact, which is the cheapest to rule out first. Recommended next steps: get the actual capture to measure precisely and rule out an artifact; a controlled repeated-'O'-cycle test at fixed preset to characterize "start conditions"; a preset sweep to check whether comb spacing tracks relative_delay. Not yet run. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: measured the actual MOD160/Preset3 capture - confirms a real, precise 7.702Hz comb (not an artifact), AND the originally-predicted ~100Hz comb is also genuinely present but less prominent nearby

Measured the user's actual capture directly instead of leaving the prior entry's speculation unchecked. Confirmed real: a 7.702Hz-spaced comb (by far the most common spacing among 35 peaks found near the tone; several other gaps are clean small-integer multiples of it), with the very first sidebands at +/-7.702Hz being the tallest non-tone features (~-20dB, matching the user's reading). The capture's 14.7s length gives 0.068Hz FFT resolution, ruling out the "measurement artifact" hypothesis outright. Also confirmed the ORIGINALLY-PREDICTED ~100Hz comb (from MOD160's designed 160-sample period) is genuinely present too (checked directly at each 100Hz multiple: -26, -30, -43.5, -41.8, -41.1dB) - just less prominent near the tone than the unexpected finer 7.7Hz structure sitting on top of it.

Noted a numerically suggestive but UNCONFIRMED coincidence: 16000/7.702=2077.4 samples is close to 160x13=2080 (which would give 7.6923Hz, ~0.13% off measured) - if the true repeat period were actually 2080 samples instead of 160, that would explain both combs at once (13 fine teeth per coarse 100Hz cell). No firmware mechanism introducing a factor of 13 has been identified - flagged as a lead, not an explanation. Recommended next step: run the same MOD160 capture on Preset 1 (zero downstream stack, relative_delay=0, already proven a clean no-op chain) - if the 7.7Hz comb disappears there, it implicates the downstream stack or relative_delay; if it persists identically, the mechanism is upstream of all of that. Not yet run. Full numbers in `null_bias_investigation.md`.

## 2026-09-18, later still: the 7.7Hz comb's frequency is invariant across Preset/tone-pair and turns out to PRE-DATE MOD160 - found (much fainter) in yesterday's OLD-generator benchmark capture too; plus a nice practical result on relative_delay/IMD tuning

User's Preset 1 MOD160 test plus several `'T'` band tests: the comb stays at 7.7Hz regardless of Preset or tone-pair - only its amplitude varies. Went back and re-checked yesterday's PRE-MOD160 captures directly: the OLD-generator Preset-3/ADC-off "low noise benchmark" (20260917_204619Z) already shows the same feature, at -7.681Hz/-38.0dB and +7.681Hz/-37.3dB - same frequency as today's measurement, just ~50-60x fainter and previously masked below that capture's own floor. The two EXACT-generator captures from the same day show no comparable structure, just their already-documented broad incoherent skirt swamping anything this small.

**This changes the leading theory**: MOD160 and the OLD generator didn't create this comb, they revealed it by removing a larger, unrelated noise source. Since it appears identically (in frequency) under two completely different tone-generation schemes and across multiple presets/tone-pairs, last entry's "true period might be 2080 samples (160x13)" numerology - tied specifically to MOD160's own digital math - is now the weaker explanation. Something common to the whole system regardless of generator/preset - most plausibly a real hardware or environmental artifact elsewhere in the chain - looks more likely, though unconfirmed. Recommended next step: capture with a single unmodulated tone (`'s'`) to check whether the same sideband pair appears regardless of what's being transmitted - would cleanly separate "belongs to the two-tone/DSP path" from "present regardless of content." Not yet run.

Separately, a genuinely useful practical result: on Preset 3, tuning `relative_delay` so the already-known residual off-tone frequency reading hits exactly zero coincides with the point of optimum IMD - a simple, live tuning indicator for finding the IMD-optimal delay setting per preset/band, rather than needing a full spectral sweep each time. Full reasoning and numbers in `null_bias_investigation.md`.

## 2026-09-18, later still: single-tone test comes back - the 7.7Hz comb is ABSENT there; single tone gives an excellent -103.6dB floor but shows a different, much fainter ~16.5Hz comb instead - reframes the leading theory back to "requires two-tone interference"

Ran the recommended discriminating test: `'s'` single tone instead of two-tone. Result: no 7.7Hz feature at all (-84 to -85dB right at that offset, just the noise floor) - falsifies last entry's "generic hardware/environmental artifact, present regardless of content" theory. Single tone gives an excellent, near-ideal -103.6dB floor both near and far from the tone (expected - no destructive-interference nulls at all with a constant envelope), a useful new best-case reference point. A different, much fainter (~-65 to -73dB) comb IS present even here, at ~16.5Hz spacing with clean harmonics - about 45-50dB weaker than the two-tone case's 7.7Hz comb, and using a completely separate, untouched-by-any-of-today's-work code path (single-tone's own always-existing plain accumulator).

Updated synthesis (speculative, flagged as such): the 7.7Hz comb likely requires the two-tone signal's own near-null events to become significant, consistent with everything already established about near-null sensitivity. One coherent way to reconcile this with the Preset/generator invariance: a small, generic, low-level system perturbation (maybe related to whatever produces single-tone's own ~16.5Hz comb) could get amplified dramatically by two-tone's near-null sensitivity into the much more prominent 7.7Hz comb - though 7.7Hz and 16.5Hz aren't a clean multiple of each other, so this isn't simply "the same thing, quieter." Not confirmed. Recommended next steps: confirm the ~16.5Hz single-tone comb reproduces on a second capture before trusting it; test whether the 7.7Hz comb's level scales with null depth/severity (varying relative_delay or amplitude ratio) to test the amplification theory. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: AM-test-at-zero-depth (bypasses the Hilbert/atan2 DSP entirely) reproduces the same ~16.5Hz comb - rules out the audio-domain DSP math as its source; proposed AM-to-PM conversion in the EER output stage as a mechanistically well-grounded new lead

User's insight: AM isolation mode (`'h'`) hardcodes envelope and freq_dev directly, skipping `ssb_dsp_process_sample()`/the Hilbert transform/`fast_atan2`/`fast_sqrt` entirely - at zero mod depth, both are constant every tick (freq_dev=0.0f literally always, envelope=fixed AM_TEST_DEPTH). Measured directly: no 7.7Hz comb (matches the single-tone result), but the SAME ~16.5Hz comb from the single-tone capture reproduces here too, same levels and harmonic pattern, on an even better floor (-107dB). Since this capture never touches the audio-domain DSP math at all, the ~16.5Hz comb can't come from there - it must be introduced downstream: the freq_dev-to-AD9851 write path, the envelope-to-PWM/DAC path, or the analog RF stage itself.

Proposed a new, mechanistically well-grounded (given this is an EER/polar transmitter) hypothesis: AM-to-PM conversion, where envelope-path ripple leaks into apparent phase/frequency via the output stage's own bias-dependent phase response - notable because this capture's commanded frequency word never changes at all, yet a real sideband still appears. Flagged an inconsistency worth reconciling (single-tone's "real" envelope ripple vs. this test's intentionally-zero ripple predict a level difference not clearly seen) rather than treating the theory as settled. Recommended next steps: vary AM_TEST_DEPTH to see if the comb's level tracks envelope/PWM output level; find a way to isolate the envelope path from the AD9851 write path if possible, as the most direct discriminator. Not yet run. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: CLOSED - the ~16.5Hz comb tracks envelope/PWM level as predicted; attributed to mixed 3.3V-USB/5V-outboard power-supply domains, low level, not being chased further

User confirms the ~16.5Hz comb's level tracks envelope/PWM output level as the AM-to-PM theory predicted, and supplies the likely hardware root cause: PWM GPIO output runs off the USB-sourced 3.3V rail while the rest of the analog chain (including the PWM filter) is powered from a separate outboard 5V PSU/regulator - a plausible source of low-level cross-domain coupling. At -65 to -73dB, with a sound explanation and physical cause identified, this is being closed deliberately - not a firmware issue, and any fix would be a power-supply/grounding change, not DSP work. This closure is specific to the ~16.5Hz comb only (single-tone/AM-test, content-independent) - it does NOT apply to the separate, still fully open 7.7Hz two-tone-specific comb (-20dB, requires near-null conditions) from earlier today, which remains the headline unresolved item, alongside the positive relative_delay/IMD-nulling result. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: 7.7Hz comb - refined theory and a concrete plan for the next test, using tools already built today

Stepping back over everything established about the 7.7Hz comb: its FREQUENCY is invariant to Preset, tone-pair/beat-frequency, and generator (OLD vs MOD160) - only its AMPLITUDE varies. That pattern argues against anything intrinsic to the two-tone math itself (which would plausibly tie the frequency to the beat frequency or the generator's own period, and doesn't) and toward a fixed-frequency, always-present, otherwise-negligible real-world perturbation that gets AMPLIFIED specifically by near-null sensitivity (the same `fast_atan2` ill-conditioning at envelope->0 this whole investigation keeps landing on) - with the two-tone geometry setting how much amplification happens, not the perturbation's own rate. Since the amplification is phase/timing-sensitive rather than amplitude-sensitive (unlike the already-closed ~16.5Hz/AM-to-PM comb), a slow TIMING perturbation is the more likely candidate category.

**Concrete next test proposed, using tools already built today**: this project has previously found and fixed a real Core1 ISR-priority-contention timing bug, so Core1 background activity is a plausible, non-speculative mechanism class here. The `'v'`/`'M'` mute toggles added earlier today are exactly the tool needed to test it: capture the same strong-comb configuration (e.g. MOD160/Preset 3) twice back-to-back, muting both `'v'` and `'M'` for the second capture, and see whether the comb's level drops or disappears. If it does, that implicates Serial/diagnostic activity as a real, fixable contributor; if unchanged, it rules that out and points elsewhere (ADC peripheral idle behavior, thermal/PLL effects, etc). A cheap secondary check: whether the comb's phase/level tracks elapsed time since boot (thermal/PLL-settling) versus resets on mode/preset changes.

Not yet run - proposed now per the user's request to keep pursuing this rather than leave it unexplained. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: muted-diagnostics and loop()-delay tests both come back negative for the 7.7Hz comb; two new leads found instead

Ran the proposed `'v'`+`'M'`-muted A/B test: no difference in the 7.7Hz comb between muted and unmuted - rules out Serial/Core1 diagnostic-print contention (confirmed by code read that the whole 1000ms `[timing]/[adc]/[dsp]` block, not just its printing, is genuinely gated off by `'v'`, so this was a clean test). Also tried changing the `loop()` `delay()` value - no difference either. Diagnostic printing does have its own noise signature when active, but it's broadband/higher-level and clearly distinct from the narrow 7.7Hz comb.

Two new leads surfaced instead: (1) a previously-unnoticed ~1Hz noise component slowly "pumping" the background noise level between the IMD products, still present with everything muted, not yet characterized; (2) probing the 16kHz sample-rate signal directly shows ~8Hz sidebands - close to, but the user confirms not an exact match to, 7.702Hz. Possibly a resolution-limited quick look rather than a genuinely different rate - a longer/higher-resolution re-measurement is the natural next step, alongside characterizing the 1Hz pumping with the same content-independence test pattern used for the other two combs. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: CONFIRMED - 7.69Hz found directly on the 16kHz sample-rate signal at proper FFT resolution - substantiates the fixed-frequency-perturbation theory for the 7.7Hz comb

Re-measured the 16kHz signal with a 256k-sample FFT at 48kHz Fs (~0.183Hz resolution, an order of magnitude finer than the earlier quick look). 7.69Hz now appears clearly, matching the RF-side comb's 7.702Hz spacing - the earlier "~8Hz, not 7.7" reading was a resolution-limited quick look, not a different rate. This is the strongest evidence yet that the theory is right: a real, fixed ~7.7Hz perturbation exists directly on the sample-rate signal itself (independent of RF, two-tone content, generator, and diagnostic/Serial activity - all separately ruled out already), which two-tone's near-null sensitivity then amplifies into the visible comb while single-tone/AM-test stay largely unaffected.

Still open: what physically produces the ~7.7Hz component on that signal. No matching ~7.7Hz timer/watchdog/periodic task was found in the firmware, so the leading candidates are something off-chip/analog at whatever exact point was probed, or a crystal/PLL/switching-PSU-adjacent artifact outside firmware control. Next step: pin down exactly what "the 16kHz signal" test point is, then look for anything else in the system free-running near 7.7Hz as a candidate source. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: the "16kHz signal" turns out to be our OWN dsp-loop timing-marker GPIO, not an external clock - reopens the earlier mute-test negative and points at Core 1's real-time path

The signal that showed a clean 7.69Hz is one of this project's own `TIMING_DEBUG_GPIO` instrumentation pins (`config.h`) - `dsp_task`'s per-tick busy pulse (pin 4, toggles at the literal 16kHz sample rate) or the raw gptimer-ISR-entry pin (pin 5, toggles at ~64kHz under the current envelope-interp design) - user said "gpio5 IIRC," worth confirming which one exactly next time at the bench. Either way, this is squarely on Core 1's real-time path, and reconnects directly to this project's own documented history there: the `intr_priority=3` fix for pin5's ISR-contention jitter (confirmed ~7x reduction) left an explicit "next to check" note that was never closed out, and a separate documented case exists of extra per-tick Core-1 instrumentation measurably regressing this same pin's timing.

Important nuance: earlier today's "muting `'v'`/`'M'` made no difference" result only ruled out the periodic PRINT/auto-dump side of Core 1 activity - it does NOT touch the always-on, every-tick hot-path recording calls (`diagnostics_record_tick_start()`/`diagnostics_record_phase_timings()`), which run unconditionally regardless of mute state and have their own documented precedent for perturbing this exact pin. So that negative result still stands for print/auto-dump specifically, but doesn't clear this separate, untested layer. Proposed next step: a small firmware change to let that always-on per-tick recording be bypassed entirely, to test directly whether it's a contributor - not yet implemented, pending the user's go-ahead. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: pin4 shows the same ~7.7Hz as pin5 (implicates dsp_task's actual tick cadence, not one pin's toggle logic); added DIAG_HOTPATH_RECORDING_ENABLED for the next A/B

User re-measured on pin4 (had originally measured pin5) - same ~7.7Hz component on both. Since pin4 (task-context whole-tick pulse) and pin5 (raw ISR-entry pulse) are generated by completely different code, this points at the actual dsp_task sample-tick cadence itself carrying the ~7.7Hz, not an artifact of either pin's own instrumentation.

Per the user's go-ahead, added `DIAG_HOTPATH_RECORDING_ENABLED` (`config.h`, defaults to `1`, no behavior change yet) gating `diagnostics_record_tick_start()`/`diagnostics_record_phase_timings()` - the two always-on per-tick recording calls flagged last entry - inside `dsp_task()`. Set it to `0`, rebuild/reflash, and rescope pin4/pin5 to test whether this recording layer is a contributor (the `[timing]` block will show nothing meaningful while it's off - expected). Not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: DIAG_HOTPATH_RECORDING_ENABLED=0 test also negative; sharp/well-defined sidebands rule out jitter and point to a coherent, deterministic ~7.7-7.8Hz process instead - likely supply-ripple-driven clock-phase modulation

Compiling out the always-on per-tick diagnostics recording made no difference to the comb either - between this and the earlier `'v'`/`'M'` mute test, all firmware/Core-1 activity has now been tested and ruled out. More importantly, the user notes the sideband tones are sharp and well-defined, not broadened - which rules out random jitter (that would show up as phase noise/broadened skirts, not narrow lines) and points instead to a real, coherent, stable periodic process somewhere in the system.

With no firmware timer at that rate, the same effect on two independent GPIOs sharing only the clock reference, and jitter now ruled out both by the priority fix and by these two negative tests, the leading theory shifts to a physically real, external cause - most plausibly a switching-supply ripple or burst-mode cycle phase-modulating the crystal/clock reference itself (a close cousin of the already-closed 16.5Hz comb's AM-to-PM/mixed-power-domain explanation, but via the clock path rather than the envelope path). Proposed next tests: scope the 3.3V-USB and 5V-outboard rails directly (AC-coupled) for a genuine ~7.7-7.8Hz component, and/or try a different power source (linear supply, different USB port) to see if the comb shifts or disappears. Not yet run. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: 3.3V-USB rail scoped directly - no 7.7-7.8Hz found there, ruling out that supply domain for this comb (unlike the already-closed 16.5Hz one)

Scoped the 3.3V-USB rail (AC-coupled): saw the expected 16kHz digital-coupling noise plus two small, separate ~100Hz/~3.3Hz features (both near the noise floor, not yet chased) - but no 7.7-7.8Hz at all. This rules out the 3.3V-USB domain specifically for the 7.7Hz comb, distinguishing it clearly from the already-closed 16.5Hz comb (which WAS traced to that same domain). Next step: scope the other implicated domain - the outboard 5V rail/local regulator feeding the analog/PWM-filter chain - the same way, now the more promising candidate. Not yet run. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: outboard 5V domain has no electrical connection to the ESP side (only the PWM signal through 10kohm) - both supply-domain theories for the 7.7Hz comb are now closed off; proposing an RF-dependence test next

User clarifies the outboard 5V rail shares no power/ground path with the ESP32 side - the only link is the PWM signal through a 10kohm resistor. That removes any plausible route for 5V-rail ripple to reach back and phase-modulate the ESP32's own clock, which is what the "supply-ripple modulates the crystal" theory required. Combined with the 3.3V-USB rail already measuring clean of 7.7Hz, both previously-implicated supply domains are now closed off as the direct source of the pin4/pin5 comb - not just unconfirmed, but structurally implausible (5V) or directly ruled out (3.3V).

This reopens the core question with several mechanisms now eliminated: not a firmware timer, not diagnostic printing, not the per-tick recording calls, not random jitter, and now not either supply domain. Proposed next test: check whether the ~7.7Hz on pin4/pin5 persists with the AD9851/RF output stage disabled or the antenna disconnected - testing whether this depends on the transmitter actually being active (self-coupling/feedback from radiated RF) versus being intrinsic to the ESP32 module regardless. Not yet run. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: DECISIVE - 7.69Hz persists with everything but the bare ESP32 switched off; it's intrinsic to the module, leading theory is now regulator "motorboating"

With the RF output stage, outboard 5V supply, and everything else switched off - just the ESP32 board running - the ~7.69Hz component is still there. This closes off RF self-coupling and both external supply domains; the source is intrinsic to the ESP32 module itself. Leading theory: "motorboating," a well-known low-frequency (~1-10Hz) parasitic self-oscillation in an under-decoupled voltage regulator/feedback loop, often excited by a periodic load-current pattern - fits every constraint seen so far (sharp/coherent, not jitter; shows up identically on two independent signals sharing only the chip's own clock; consistent with a budget "Super Mini"-class board's typically minimal onboard decoupling; dsp_task's own 16kHz periodic current draw is exactly the kind of repetitive load transient that can excite it).

Cheapest, most informative next step: add extra bulk/bypass capacitance directly across the ESP32 board's own VCC/GND and rescope pin4/pin5 - if the comb weakens or disappears, that both confirms the theory and fixes it. Not yet run. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: "motorboating" theory falsified too (no PSU-rail ripple, extra decoupling made no difference) - every tested explanation for the 7.7Hz comb is now ruled out except something intrinsic to the SoC package itself

No 7.8Hz visible on the ESP32's own PSU rail, and added decoupling capacitance made no difference anywhere - both expected if the regulator-instability ("motorboating") theory were right, so it's now considered falsified rather than unconfirmed. A quick check for a known ESP32-S3 errata/quirk at this frequency found nothing (inconclusive, not ruled out, just not documented).

That closes out essentially every explanation raised in this thread - firmware timer, diagnostic printing, per-tick recording, random jitter, both external supply domains, RF self-coupling, and now external regulator instability. What's left is something intrinsic to the ESP32-S3 SoC package itself (internal core regulation, internal clock/PLL generation), which - if right - wouldn't be fixable from outside the chip at all. Flagged as a natural checkpoint: keep digging into SoC internals with no guarantee of an actionable answer, or document this as characterized-but-likely-intrinsic and park it, similar to how the 16.5Hz comb was eventually closed. No decision made yet. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: built minimal_fs_test/ - a standalone minimal reproducer for process-of-elimination on the 7.7Hz comb

Per the user's proposal, built a from-scratch, separate Arduino sketch (`minimal_fs_test/minimal_fs_test.ino` - its own folder, deliberately not a mode inside the main sketch) that reproduces only the timing skeleton: the same gptimer setup, same ISR, same cross-core notify hand-off to a Core-0 task, toggling the same two physical pins (4 and 5) - copied structurally verbatim from the main project's current production config (confirmed `ENVELOPE_INTERP_FACTOR=1`, plain 16kHz gptimer, `intr_priority=3`) - with absolutely nothing else: no ADC, no DSP, no AD9851, no envelope/PWM/DAC output, no diagnostics.

If this alone still shows ~7.7Hz on pin4/pin5, the cause is proven to be in the timing skeleton itself (or lower - the chip's own clock/scheduler). If it comes back clean, the sketch's header comment lays out a suggested bisection order (ADC capture, then DSP, then AD9851 writes, then envelope/PWM output, then diagnostics recording) to add back one piece at a time until the comb reappears, pinpointing the responsible subsystem directly. Not compiler-verified yet (same standing caveat as the main sketch) - needs an Arduino IDE build pass before flashing. Full details in `null_bias_investigation.md`.

## 2026-09-18, later still: DECISIVE - the bare minimal_fs_test skeleton is clean of 7.7Hz; proves it's introduced by code, not the timing hardware; built step 1 (real ADC capture, result discarded)

`minimal_fs_test.ino` alone (bare gptimer/ISR/notify/task-wake, nothing else) came back clean on real hardware - just the expected 16kHz plus small, almost-certainly-mains-related 50Hz/100Hz. No 7.7Hz at all. This is decisive: the comb isn't in the raw timing mechanism, it's introduced by something in the main project's own code - the ADC capture, the DSP/Hilbert chain, the AD9851 writes, or the envelope/PWM/DAC output.

Built step 1 of the bisection plan: `minimal_fs_test_step1_adc/` adds only the real ADC continuous-DMA capture (copied verbatim from the main project) to the clean skeleton, reading and discarding one sample per tick. If pin4/pin5 stay clean, ADC capture is cleared and DSP/Hilbert is next; if the comb reappears, the ADC driver is the confirmed culprit - which would connect to this project's own documented history of the ADC ISR causing GPIO-adjacent electrical coupling elsewhere, though a ~7.7Hz effect from it would still be new. Not compiler-verified yet, not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: widened pin4's pulse for 48kHz-capture visibility in the minimal test sketches

pin4's pulse was reported as too low-energy for a 48kHz audio-interface-based FFT capture (nothing between SET/CLR meant a near-instantaneous glitch). Added a 20us trailing delay (`esp_rom_delay_us`, after any real per-tick work, right before CLR) in both `minimal_fs_test.ino` and `minimal_fs_test_step1_adc.ino` - long enough to be energetic across the full audio band at 48kHz, short enough to leave plenty of margin in the 62.5us tick period. Deliberately not applied to pin5, since that would mean adding a busy-wait inside the real ISR itself; pin4 is the safe one to widen and should be the primary channel for 48kHz-capture work going forward, with pin5 kept for scope-based edge-timing checks. Not compiler-verified yet. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: pin4 pulse bumped to ~50% duty (31us) after 20us was still too quiet; flagged signal level upstream as the likely next bottleneck

20us wasn't enough - bumped `DEBUG_PULSE_STRETCH_US` to 31us (~50% duty of the 62.5us tick), the theoretical max for a boxcar's energy at the fundamental (16kHz). If that's still quiet, pulse width is unlikely to be the issue any further - more likely the GPIO's 3.3V logic level is being attenuated somewhere between it and the 48kHz capture device (a protective divider, AC-coupling cap, or the capture device's own gain setting). Asked the user what's actually in that signal path. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: bisection step 1 (ADC capture added) shows a new, separate low-level ~56.4Hz comb on pin4 - not the 7.7Hz target, and unclear yet whether 7.7Hz is also still present

Adding the real ADC capture (step 1 of the bisection) introduced a new, low-level ~56.4Hz comb that wasn't in the clean bare-skeleton result - useful confirmation the ADC subsystem CAN perturb pin4's timing, but it's a different frequency from the 7.7Hz target with no obvious digital-clock relationship found. Not yet clear whether the original 7.7Hz is also still present alongside it or has stayed absent - asked the user to clarify, since that determines whether ADC capture should be considered cleared (move to step 2, DSP/Hilbert) or a contributing-but-not-sufficient factor. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: step 1 (ADC) confirmed clear of 7.7Hz; found real two-tone mode never runs the per-tick ADC read at all; built step 2 (real two-tone generator + real DSP chain, matching production exactly)

Confirmed no 7.7Hz with ADC capture added (step 1) - only the separate new 56.4Hz comb. While building step 2, found that real two-tone mode (`dsp_task()`'s `AUDIO_SRC_TWOTONE` branch) never calls the per-tick ADC read at all - so step 1, while a useful result on its own, wasn't actually testing the code path the original comb occurs under.

Built `minimal_fs_test_step2_dsp/` to match production exactly instead: the real two-tone generator feeding the real Hilbert/DSP chain every tick (both outputs discarded), no ADC capture at all - `ssb_dsp_init()`'s config and EQ/compressor-off defaults copied verbatim from the main sketch's `setup()`. If pin4/pin5 stay clean, the DSP math is cleared and AD9851 writes (step 3) are next; if the comb reappears, this is the confirmed culprit on the exact real code path. Not compiler-verified yet. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: step 2 (real two-tone generator + real DSP chain) also clear of 7.7Hz - introduces its own small 1/2/3Hz harmonic family instead; building step 3 (AD9851 write)

Step 2 (`minimal_fs_test_step2_dsp` - real `generate_twotone_sample()` into real `ssb_dsp_process_sample()`, both outputs discarded, no ADC at all - matches production's `AUDIO_SRC_TWOTONE` path exactly) came back "some approx 1, 2 & 3Hz sidebands but nothing else of significance" - **still no 7.7Hz**. The Hilbert/DSP math (`fast_atan2`/`fast_sqrt`/near-null handling) is now cleared on the real code path, not an approximation.

Same pattern as step 1: each subsystem added introduces its own small, distinct, unrelated artifact (step 1: ~56.4Hz; step 2: a harmonically-related 1/2/3Hz family, suggesting a non-sinusoidal ~1Hz process in the DSP execution) without reproducing the 7.7Hz target itself. Possible, unconfirmed connection to the earlier-noted "~1Hz noise-floor pumping between the IMD products" lead from the real RF signal - worth keeping in mind if either is revisited.

Bisection scoreboard: raw timing skeleton - clean; ADC capture - clean of 7.7Hz (own 56.4Hz artifact); DSP/Hilbert chain - clean of 7.7Hz (own 1/2/3Hz artifact). Remaining untested: AD9851 SPI write, envelope/PWM/DAC output. Building `minimal_fs_test_step3_ad9851/` next, adding the real `carrier_output_set_freq_dev()`/`ad9851_set_frequency()` write fed by step 2's now-used `freq_dev_hz`, chip physically absent being fine for a timing-only test. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: built step 3 (real AD9851 write, fed by step 2's freq_dev_hz) - delay line deliberately excluded to keep it a single-variable step

`minimal_fs_test_step3_ad9851/` extends step 2 with exactly one more real piece: the real AD9851 write (`carrier_output_set_freq_dev()` -> `ad9851_set_frequency()`'s bit-bang transport), fed by step 2's previously-discarded `freq_dev_hz`. `relative_delay_apply()` (production's delay line between DSP and the AD9851 write) was deliberately left out of this step - adding it alongside the AD9851 write would confound two variables at once. `carrier_output.h/.cpp`/`AD9851.h/.c` copied verbatim, standard ESP-IDF deps only, no other project files needed. `AD9851_ATTACHED` is already 1 in the project's real config.h so this compiles/runs for real - chip need not be physically attached for a timing-only test. Not compiler-verified yet, awaiting bench test. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: step 3 crashes with a task watchdog (IDLE0) reboot every ~11s - isolating the AD9851 init vs. per-tick write, plus a real NaN/Inf-to-int32 UB hazard closed along the way

Step 3's first bench run reboots the board on a ~11s cycle. Serial log: `task_wdt: Task watchdog got triggered ... IDLE0 (CPU 0) ... Tasks currently running: CPU 0: tick_task` - `tick_task` is hogging core 0 and never yielding long enough for the idle task to run, a genuine hang, not just a noisy-but-live signal. Steps 0-2 (same tick_task/gptimer/notify structure, same real DSP chain) never showed this, so it's one of step 3's two additions: `carrier_output_init()` (setup-time) or the per-tick `carrier_output_set_freq_dev()` write. Added `AD9851_PER_TICK_WRITE_ENABLED` (default 1) to the sketch to isolate which without a new folder - set to 0 to keep AD9851 initialized but skip the per-tick write.

Also closed a real, independent hazard found while investigating: `carrier_output_set_freq_dev()` casts `freq_dev_hz` (float) to `int32_t` with no NaN/Inf guard - undefined behavior in C, and step 3 is the first bisection step to actually consume that value rather than discard it (given this project's own extensive near-null-math history). Added a defensive `isfinite()` clamp. Not assumed to be the crash's root cause on its own (Xtensa float-to-int saturates rather than traps), but worth fixing regardless. Next: bench-test with `AD9851_PER_TICK_WRITE_ENABLED=0` first. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: DECISIVE - step 3's reboot traced to our own 31us debug-pulse stretch pushing total tick time at/over the 62.5us budget once the real AD9851 write is added, not a real production issue

`AD9851_PER_TICK_WRITE_ENABLED=0` runs clean, `=1` reboots - confirms the per-tick write triggers it. The busy_us diagnostic shows why: with the write on, busy_us runs 59-67us, at/over the 62.5us tick period - near-100% CPU duty. Subtracting the known 31us `DEBUG_PULSE_STRETCH_US` (added only in this diagnostic sketch, purely for pin4's 48kHz-capture visibility - production has no such stretch) puts the real DSP+write cost at ~28-30us, matching this project's own earlier estimate for the AD9851 write. So the reboot is very likely a bisection-tooling artifact - our own visibility hack colliding with a real but not-excessive per-tick cost - not new evidence that the AD9851 write breaks the real-time budget in production (which runs this same write continuously without ever hitting this).

Fixed with `disableCore0WDT()` (standard Arduino-ESP32 fix for a real-time task pinned to a core running near/at 100% duty by design) and reducing `DEBUG_PULSE_STRETCH_US` to 10 (this step only) to restore real margin (~35-40% idle). Also reframes the earlier "pin4/5 and RF both noisy/unstable" report as likely the system already in extremis near a reboot, not a clean read of the write's actual effect - worth re-evaluating once a stable run is captured. Not yet re-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: reboot fixed, but a new anomaly - tick_task now runs in bursts of 3-4 samples every ~5ms, while the gptimer ISR (pin5) stays a clean, steady 16kHz throughout

With disableCore0WDT()/10us-stretch in place, no more reboot, but pin4 (tick_task's busy pulse) now shows bursts of 3-4 pulses every ~5ms with long LOW gaps in between - while pin5 (the real ISR) stays a clean steady 16kHz the whole time. This localizes the problem precisely: the timer/ISR mechanism is fine and tick_task isn't internally slow (pin4 is LOW, not stuck HIGH, during the gaps) - something is preventing an already-notified, very-high-priority task from being scheduled on Core 0 for ~5ms stretches, repeatedly. Not yet identified what. Two cheap next tests: re-check with `AD9851_PER_TICK_WRITE_ENABLED=0` to see if this is actually tied to the write at all, and grab the `[diag]` busy_us/ticks_this_sec serial output for hard numbers. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: SOLVED - the pin4 bursting traced to our own disableCore0WDT() fix, not the AD9851 write; removed it

Confirmed the bursty pin4 pattern persists with `AD9851_PER_TICK_WRITE_ENABLED=0` too, ruling out the AD9851 write. Root cause found in the serial log: `task_wdt: esp_task_wdt_reset(707): task not found`, repeating continuously - `disableCore0WDT()` (added in the prior fix) deletes IDLE0 from the watchdog's task list, but Arduino-ESP32's idle hook keeps calling `esp_task_wdt_reset()` on it anyway every idle iteration, and each failed call logs an error - a documented gotcha with that function. That log-spam was almost certainly what was eating the scheduling time meant for tick_task - a bug in our own fix, unrelated to dsp_task/AD9851/the 7.7Hz investigation. Removed `disableCore0WDT()`; the other half of that fix (reducing `DEBUG_PULSE_STRETCH_US` to 10us) should already restore enough real-time margin on its own to prevent the original reboot without touching the watchdog at all. Not yet re-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: step 3 finally runs clean - reveals a bigger 1Hz comb plus ~15.3Hz on pin5, rather than directly confirming/ruling out 7.7Hz

Both fixes confirmed correct: with `disableCore0WDT()` removed and the 10us pulse-stretch kept, step3 runs stably (no reboot, no bursting). First clean result: pin5 (real ISR) shows a 1Hz comb plus ~15.3Hz; pin4 shows a much bigger 1Hz comb, 15Hz suspected but too low to see. The 1Hz comb likely ties to this project's already-established two-tone whole-second periodicity (data-dependent near-null math timing recurring every identical 1.000s cycle) - matches step 2's own small 1/2/3Hz finding, just larger here given step 3's thinner real-time margin. Pin5 (the ISR itself) also showing it is more surprising and not yet explained - possibly tied to occasional real tick overruns changing the ISR's own notify-give timing on those specific ticks. Numerically interesting: ~15.3Hz is close to 2x the original ~7.69Hz target - flagged, not confirmed. Asked the user whether 7.7Hz itself showed up in this capture, since that's the open question this step exists to answer. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: DECISIVE - step 3 (AD9851 write) confirmed clear of 7.7Hz; every Core-0 subsystem now cleared, narrowing to the envelope/PWM/DAC output stage (step 4) - the only remaining piece, and the only one involving a second task on Core 1

Confirmed: nothing at 7.7Hz on either pin, pin5 quiet overall (-55dB from the 16k tone). Bisection scoreboard: raw skeleton, ADC capture, DSP/Hilbert chain, and now the AD9851 write are ALL clear of 7.7Hz, each introducing its own small distinct artifact instead (56.4Hz, 1/2/3Hz, and now a bigger 1Hz + ~15.3Hz respectively). Remaining: `relative_delay_apply()` (skipped in step 3 to stay single-variable) and the full envelope/PWM/DAC output chain, including `dac_task` - a separate task on **Core 1** talking to an MCP4725 DAC over I2C. This is the only remaining candidate that's a genuinely different class of mechanism (cross-core, I2C-driven) from everything tested so far, and this project's own code comments already flag documented historical concern about dac_task's I2C activity causing cache-line stalls - worth treating as the leading candidate now. Next: build step 4 (relative_delay + full envelope chain + dac_task on Core 1). Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: built step 4 (relative_delay + full envelope chain + real PWM) - the last untested piece; corrected the "dac_task" lead since it's not actually active in production

Checked config.h directly: `dac_task_enabled=0` in the real, currently-active firmware - the Core-1 I2C task flagged last entry as a promising lead does NOT actually run right now, correcting that framing. Real active output is the LEDC PWM leg (`PWM_COMPARISON_ENABLED=1`) on GPIO2. Built `minimal_fs_test_step4_envelope/`, adding `relative_delay_apply()` and the full envelope chain (floor/gdeq/ampeq/predistort, all default-disabled per production settings) feeding the real PWM hardware write - the last untested piece of dsp_task's real per-tick pipeline. Used a subagent to scope exact signatures/init order, then verified the highest-risk claims directly against the real source. If this comes back clean too, every individually-tested subsystem will be cleared, pointing toward either an SoC-intrinsic cause or a multi-subsystem interaction not yet tested. Not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: DECISIVE - step 4 (full envelope + PWM) also clear of 7.7Hz; the whole real pipeline is now individually cleared. RF noise traced (probably) to the already-known EXACT-generator near-null glitch, not MOD160

Step 4 runs as clean as step 3, same 1Hz comb on pin4/5, no 7.7Hz. This completes the bisection - every subsystem in dsp_task's real per-tick two-tone pipeline has now been individually tested and cleared. Separately: confirmed the RF noisiness the user asked about is NOT the MOD160 generator (test_signals.cpp defaults to EXACT, never switched in any of these sketches) - much more likely it's the already-documented EXACT generator's own near-null glitch (~10226Hz single-tick freq_dev jumps at destructive-interference nulls) becoming audible for the first time now that step 4 finally reconstructs both amplitude AND frequency modulation together. Cheap follow-up proposed: call `test_signals_next_twotone_phase_gen()` twice in setup() to select MOD160 and re-test - would be the first real bench confirmation of a theory so far only validated by simulation. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: BREAKTHROUGH (tentative) - 7.69Hz-like signal visible under BOTH LEGACY and MOD160 (pin4 and pin5); very likely EXACT's own 1Hz comb was masking it through the whole bisection

User tried the cheap MOD160 follow-up from the last entry, first said "Confirmed a feature of MOD160," then retracted: "No I was wrong. I can see it but lower level on legacy gen on pin5." Asked precise clarifying questions before trusting either version (this exact thread already has one documented near-miscount risk with `test_signals_next_twotone_phase_gen()`). Confirmed answer: on default EXACT, the already-known 1Hz comb is too dominant to tell if 7.69Hz is present or not - EXACT is masked/inconclusive, NOT confirmed clean. Under LEGACY it's there (lower level) on pin4 AND pin5. Under MOD160 it's ALSO there, on pin4 and pin5.

Checked `test_signals.cpp` directly: EXACT and MOD160 both recompute phase fresh each tick from a shared wrapping sample index (16000-sample/1-second period for EXACT, 160-sample/10ms period for MOD160); LEGACY uses two independent per-tone float accumulators with no shared index at all. LEGACY and MOD160 are about as structurally different as two phase generators can be, yet BOTH show the signal - while EXACT (the odd one out, using a much bigger 16000-sample shared wrap) is the one mode that can't be read, because of its own already-documented 1Hz artifact (seen in every one of steps 2/3/4's results) sitting right on top of 7.69Hz's neighborhood.

**Tentative conclusion, not yet nailed down with a proper FFT read**: 7.69Hz-like content may be present in the real chain independent of which phase-gen implementation drives the tones, and EXACT's own big 1Hz comb has likely been masking it throughout the ENTIRE bisection (steps 1-4) rather than those steps actually being clear of it. This would mean every "clear of 7.7Hz" conclusion so far is only clear *under EXACT specifically* - the bisection needs to be at least partially redone under LEGACY or MOD160, starting with the simplest candidate (step 2's DSP-only chain), to find out where 7.69Hz-like content first appears once the mask is removed.

Repurposed `minimal_fs_test_step2b_dsp_mod160/` for this: instead of step 4's hardcoded-call-count approach (which already caused one real mode mix-up), added a serial `'o'` command in `loop()` - same binding as the real firmware's `'O'` handler - that cycles EXACT/LEGACY/MOD160 and prints the confirmed active mode after every press, so a single flash can A/B/C all three with no ambiguity about which mode is actually running. Step 2's own real generator+DSP scope (no AD9851, no envelope, no ADC) is otherwise unchanged. Not yet bench-tested. Step 4's existing (hardcoded) MOD160 switch is left as-is for now; the on-device Downloads copy of step 4 predates that edit and is stale. Full reasoning in `null_bias_investigation.md`.

## 2026-09-18, later still: DECISIVE - step2b (bare generator+DSP, no hardware output at all) confirms 7.7Hz on pin4 (a pure CPU-timing pulse): EXACT -60dB/nothing, LEGACY -56dB, MOD160 -43dB; added a 'd' toggle to isolate generator vs DSP math next

Bench numbers from `minimal_fs_test_step2b_dsp_mod160`, pin4 only: EXACT shows no 7.7Hz (noise floor -60dB), LEGACY shows it at -56dB, MOD160 shows it at -43dB (strongest). Pin4 here is a pure CPU-timing pulse (fixed 31us stretch after the real work) - it has no signal content of its own, so a mode-dependent 7.7Hz component on it means the generator+DSP code's own execution time is being perturbed at ~7.7Hz, and how much depends on which phase-gen mode is active. This rules out the AD9851 write and envelope/PWM/ADC stages entirely (none exist in this sketch) and rules out "EXACT just masks an equally-present signal" (the level genuinely varies by mode, not just its visibility).

Not yet known why generator-mode choice would change execution time at ~7.7Hz - leading candidate is data-dependent near-null handling inside the Hilbert/atan2 chain reacting differently to each mode's phase trajectory. Added a `'d'` serial toggle to the same sketch (matching the existing `'o'` cycle) that turns the `ssb_dsp_process_sample()` call on/off while the generator always still runs - lets all 3 modes be tested with and without the DSP math in one flash, to find out whether the effect needs the Hilbert chain at all or is already present with just the raw generator. Not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-19: confirmed the 7.7Hz timing effect needs the DSP chain (vanishes with DSP off); new hypothesis - AD9851/PWM WRITE-TIME jitter, not signal content, may be how this reaches the real RF output; built step5_resync to test the fix

With MOD160 selected, DSP on gives the strongest pin4 comb yet; DSP off (via 'd') makes it disappear completely - pins the effect specifically inside the Hilbert/atan2 chain reacting to MOD160's phase trajectory.

User's question - a genuinely new angle: how does a pure CPU-timing artifact (proven on step2b, no hardware writes anywhere) turn into an actual RF-domain 7.7Hz comb? Because the AD9851/PWM writes happen at the END of each tick, right after the DSP chain's own (proven jittery) execution time - so the wall-clock MOMENT those writes occur, and therefore when the AD9851's frequency actually changes, jitters by the same amount relative to the one fixed clock in the system (the gptimer ISR). Write-time jitter converts directly to phase/frequency noise in a DDS system - independent of the AD9851 hardware itself being at fault.

User's proposed fix: buffer this tick's result and write it as the FIRST action of next tick instead of the end of this one, re-syncing the write to the ISR boundary. Built `minimal_fs_test_step5_resync/` (extends step4) implementing this as a one-tick buffered pipeline - both frequency and envelope deferred together so their existing relative_delay_apply() alignment is preserved, adding exactly one sample (62.5us) of shared latency. Toggle `'w'` (default RESYNC) A/B's this against step3/4's original write-immediately behavior; toggle `'o'` (step2b's convention) cycles EXACT/LEGACY/MOD160. Test: does the real RF/pin5 7.69Hz comb shrink under RESYNC+MOD160 vs LEGACY+MOD160? Not yet bench-tested. Full reasoning in `null_bias_investigation.md`.

## 2026-09-19, later still: pin5's falling edge confirmed rock-solid as the true Fs reference; found a THIRD prior "TRIED, REVERTED" failure in this exact area before building step6_isr_write (AD9851 write direct from the timer ISR)

User scoped pin5 directly: its falling edge (the true Fs reference - driven by gptimer's own hardware alarm, not software) is rock-solid; the AD9851 write's timing shift sits entirely in the cross-core ISR-to-task hand-off plus tick_task's own work, both downstream of that solid edge. Asked about writing the AD9851 directly from the ISR to remove that hand-off - tighter than step5's resync.

Before building, checked this project's own history per the user's question ("did we try something similar in the isr and failed previously??") and found THREE prior failures, not the two already known: (1) `ssb_mic_test.ino` - pinning dsp_task to Core 1 starved Serial completely, reverted; (2) `ssb_mic_test.ino` - moving init_sample_timer() into dsp_task's body caused reboots and worse timing, reverted; (3) NEW to this thread - `envelope_interp.cpp`/`group_delay_fit_notes.md`'s 2026-09-08 SDM stage-in-task/commit-from-ISR experiment, which measured WORSE not better on real hardware, with two suspected mechanisms: a possible flash-resident driver call inside the ISR, and a "staging race" where a slow tick_task causes the ISR to commit a value more than one tick stale.

Built `minimal_fs_test_step6_isr_write/` (extends step5) accounting for all of this: ONLY the AD9851 write moves into the ISR (judged safe - raw IRAM-resident register pokes, no driver call, confirmed by reading AD9851.c directly), envelope/PWM stays task-side unconditionally (the SDM precedent directly argues against moving it). Added a staging-race counter (`s_stage_seq`/`s_stale_commit_count`) to measure mechanism (3)'s risk directly rather than hope against it. Revert is a compile-time flag (`ISR_WRITE_ENABLED`, not runtime) since failure (1)'s canary was Serial dying outright - a runtime toggle can't be trusted if Serial is what breaks. Not yet bench-tested; the highest-risk sketch in this investigation so far, going into a seam with three prior undiagnosed failures. Full reasoning in `null_bias_investigation.md`.

## 2026-09-19, later still: step6's first bench run panicked (Coprocessor exception, EXCCAUSE 0x4) - a fourth documented ISR failure mode, missed in my own pre-flash audit; fixed and awaiting re-test

First flash crash-looped immediately: `Guru Meditation Error: Core 1 panic'ed (Coprocessor exception)`. Root cause: the ISR called `carrier_output_set_freq_dev(float)` directly, which does a float-to-int32 cast internally - float math needs Xtensa's FPU coprocessor, which isn't set up for ISR use by default and panics. This is exactly the "v2 ISR crash" failure mode `group_delay_fit_notes.md`'s SDM postmortem already named (and step6's own header had already quoted) - I checked `AD9851.c`'s integer bit-bang loop for ISR-safety but never checked `carrier_output.cpp`'s float-arithmetic wrapper sitting in front of it. A real gap in my own review, not a hardware surprise.

Fixed by splitting the float conversion (now done in `tick_task`, task context, safe) from the pure-integer register write (now what the ISR actually calls) - `carrier_output_compute_tx_freq()`/`carrier_output_set_freq_dev_raw()` in step6's own copy of `carrier_output.h`/`.cpp` only; other steps and the real firmware are untouched. Not yet re-bench-tested. Full postmortem and the resulting standing ISR-safety checklist (no blocking/FreeRTOS calls, no unconfirmed-IRAM driver calls, no floating point anywhere in the call chain - and check every wrapper, not just the leaf function) in `null_bias_investigation.md`.

## 2026-09-19, later still: BREAKTHROUGH CONFIRMED - step6 (AD9851 write from the ISR, envelope resync task-side) fixes the close-in noise floor on real hardware in every two-tone phase-gen mode, no sign of 7.7Hz anywhere; punch list for the clean skeleton before folding back into the full production system

Re-flash after the Coprocessor-exception fix (previous entry, `928e9a2`) came back clean on real hardware. User's own report: "Spectacular!!! that's the breakthrough I've been searching for. Noise floor very low close in now in all gen cases. No sign of 7.7Hz. Mod160 noise floor the best but not its raw IMDs. And the dsp jitter seems better too, the end of ISR is more stable linking to the start of the dsp - all look more controlled on scope." Three confirmations bundled in that one report: (1) close-in noise floor low under EXACT/LEGACY/MOD160 alike, not just LEGACY as step5's resync alone gave; (2) pin4/pin5's own timing relationship visibly tighter on the scope, independent of the RF read; (3) the original ~7.7Hz target - the reason this whole bisection (steps 1-6) exists - is gone. Full account and the new MOD160-best-noise-floor-but-worst-raw-IMD open question in `null_bias_investigation.md`.

User's own next move: fold the two fixes (AD9851 write from the timer ISR; envelope/PWM resync task-side, one-tick buffered) into the full production `ssb_mic_test.ino` rather than continuing isolated bisection, and asked directly - while step6's clean, well-characterized minimal skeleton still exists - "is there anything else we should look at for future ref?" Punch list, roughly in the order it's cheapest to do it in:

1. **Soak the diagnostics that already exist before trusting the result long-term.** `stale_commits` (step6's staging-race counter) read 0 on the first real run but only over a short session - worth a longer soak (minutes, not seconds) across all three phase-gen modes to make sure tick_task never falls far enough behind for the ISR to commit a stale value. Same for pin5's low-pulse width the user measured as "spot on stable, low for up to about 7us" - worth re-checking that stays true over a longer run and isn't sensitive to which mode is active, since MOD160/LEGACY/EXACT now measurably differ in DSP execution time.
2. **Chase the new MOD160-best-noise-floor-but-worst-raw-IMD puzzle directly on this rig, before it gets buried under the fold-back.** This is a brand new, unexplained finding surfaced by fixing the write-time jitter, and the step6 skeleton is the cleanest place that will ever exist to isolate it - once folded into the full firmware there are many more moving parts to control for. Worth capturing/saving each mode's spectrum now (close-in noise AND raw two-tone IMD numbers together) as a reference baseline, and thinking about why a phase generator with less numerical jitter (MOD160's short 160-sample wrap) would trade a better noise floor for worse raw IMDs - candidate angle: MOD160's short wrap makes the two tones' relative phase relationship itself perfectly periodic at 100Hz, which could be revealing a real IMD mechanism (rather than generator noise) that longer-period EXACT/LEGACY were partly averaging out or spreading across the spectrum.
3. **A clean, quantitative step5-vs-step6 side-by-side**, now that both are working and comparable: same numbers (noise floor, IMDs) measured back-to-back under identical conditions for the written record, since right now the comparison is qualitative ("10dB under LEGACY, ~1dB under MOD160" for step5; "very low in all gen cases" for step6).
4. **Flag before assuming this transfers directly to real voice/mic mode**: every step in this bisection, including step6, drives the DSP chain from the synthetic two-tone generator, which skips the real ADC read entirely. The write-time-jitter mechanism itself (DSP execution time jitter -> write timing jitter -> RF phase/frequency noise) should be generator-independent, but it's worth explicitly re-confirming the fix helps in real mic-input mode too rather than assuming it, since the ADC read is a genuinely different piece of Core-0 execution time than anything tested here.
5. **A possible further step, flagged as risky and not recommended without a fresh audit**: `envelope_output_write_pwm()` still goes through the full LEDC driver (`ledc_set_duty()`/`ledc_update_duty()`), deliberately kept OUT of the ISR in step6 specifically because of the SDM precedent (`envelope_interp.cpp`/`group_delay_fit_notes.md`, 2026-09-08) which measured WORSE results moving PWM-adjacent work into an ISR. If envelope/PWM write-time jitter turns out to matter as much as frequency did, a raw-register bypass of LEDC (mirroring AD9851's own bit-bang bypass of its driver) would be the analogous fix - but this is exactly the kind of change this project's own history says has already failed twice (SDM's ISR commit, and "v1's direct-ISR PWM attempt crashed outright" per `envelope_interp.cpp`'s header) and would need the same wrapper-by-wrapper ISR-safety audit that was missed once already this session (see the standing checklist added in the previous entry) before attempting it again.

None of the above blocks moving ahead with the fold-back into `ssb_mic_test.ino` - they're things worth doing on this rig opportunistically, in parallel with or shortly after that work starts, while it's still cheap to do them here.

## 2026-09-19, later still: first saved reference-baseline spectrum (MOD160 + ISR-write, off-air two-tone capture) - executes punch-list item 2/3; 7.7Hz down to ~-58dB, but a new unexplained ~0.33Hz close-in sidebband is now the dominant artifact

User supplied an 18.19s off-air SDRuno IQ capture (`SDRuno_20260919_102243Z_14175kHz.wav`, MOD160 + the new ISR-write fix) as a benchmark reference. This is the first real execution of punch-list items 2 and 3 from the previous entry (save each mode's spectrum as a baseline; get quantitative numbers rather than qualitative scope reads).

Numbers: two-tone IMD ladder (3rd order ~-27 to -28dB, 5th order ~-27dB, out to 7th/8th order around -30 to -36dB) - the first hard number for MOD160's "worst raw IMDs" side of the noise-floor-vs-IMD tradeoff flagged in the breakthrough entry, worth comparing directly once EXACT/LEGACY captures exist too. The original 7.7Hz/15.4Hz comb is still measurably present but now down around -58/-61dB - consistent with, not contradicting, the "no sign of 7.7Hz" scope read (it's just far below the IMD ladder's own level on a normally-scaled display). Full numbers in `null_bias_investigation.md`.

**New open item**: the dominant close-in artifact in this capture is actually a ±0.33Hz sideband pair at -23 to -25dB - bigger than the 7.7Hz residual and not something this bisection has seen or explained before. Candidate causes: something receive/SDRuno-side (this is the first off-air capture used in this investigation - everything before was bench GPIO/scope work, so the receive chain itself has never been ruled out as a source), or a genuine transmit-side periodicity around 3 seconds that only shows up with the full signal chain running continuously (unlike the earlier bare-generator/DSP-only bisection steps). Worth chasing on the next benchmark capture, and worth getting an EXACT/LEGACY off-air capture for comparison to see if it's mode-independent (points at receive chain or a shared code path) or mode-dependent (points back at the generator, similar to how 7.7Hz itself was first found).

## 2026-09-19, later still: folded the confirmed write-time-jitter fix (resync + AD9851-from-ISR) into ssb_mic_test.ino - awaiting first real-hardware test of the full system

Per the user's own plan ("need full system to fully test that" re: which generator mode gives the best real IMDs), moved past the minimal-sketch bisection: ported step6's full fix (not step5's resync-only version - confirmed via direct question) into the real production firmware, not another isolated test sketch.

Changes: `config.h` gained `TX_WRITE_RESYNC_ENABLED`/`AD9851_ISR_WRITE_ENABLED` (both default ON, compile-time only, with `#error` guards enforcing their dependency order and the `ENVELOPE_INTERP_FACTOR==1` precondition the ISR-write approach needs); `carrier_output.h`/`.cpp` gained the crash-fix split (`carrier_output_compute_tx_freq()`/`carrier_output_set_freq_dev_raw()`) for real this time, not just in a step folder; `ssb_mic_test.ino`'s `on_timer_alarm()` gained the AD9851-from-ISR write (with the staging-race guard) and `dsp_task()` gained the resync buffering at the top of its loop plus buffering-instead-of-immediate-write at the bottom; `diagnostics.cpp`/`.h` gained a small, properly-integrated extension (`diagnostics_record_isr_stale_commits()`) so the staging-race counter shows up in the existing `[timing]` periodic print rather than as a bolted-on side print.

Full write-up of the design decisions (ENVELOPE_INTERP_FACTOR==1 precondition, why diagnostics_set_tx_info()'s inputs didn't need to change, the IRAM_ATTR-consistency reasoning for the new diagnostics function) is in `null_bias_investigation.md`'s matching entry. Reviewed thoroughly (balanced-conditional check via script, full manual control-flow trace) but NOT bench-tested - no compiler/toolchain available here to verify against, and production's dsp_task is meaningfully more complex than step6's minimal skeleton (full envelope chain, real ADC, the currently-inert ENVELOPE_INTERP_FACTOR machinery), so treat this as a careful port awaiting its first real flash, not a second confirmed result.

**Next, once flashed**: (1) confirm it boots clean, no Serial/Core-1 disruption (the historical canary for this exact class of change); (2) check `stale_commits` stays at 0 in the real pipeline, not just the minimal one; (3) re-run the same off-air two-tone benchmark captures (EXACT/LEGACY/MOD160) now with the full mic-capable system in the loop, both to confirm the fix still holds and to finally get real numbers on which mode gives the best raw IMDs, per the user's own guess that LEGACY will win there even though MOD160 had the best noise floor.

## 2026-09-19, later still: real dsp-start variation is ~4.6us (not the ~300ns first reported), with a suspected occasional WCLK "double" pulse as the mechanism - reconnects to the old, never-fully-closed BS170/push-pull edge-quality thread

Corrected reading from the user's own scope work: dsp-start timing after the ISR varies by up to ~4.6us (floor 20.5us), not the tight ~300ns bimodal split first reported. Suspected cause: an occasional extra W_CLK pulse mid-transfer, which the user recalls seeing before "when investigating the edges" - directly reconnects to this project's long-running BS170 inverting-level-shifter edge-quality thread (`AD9851.c`'s own "leading theory for the two-tone-specific 'sticks' symptom," push/pull drivers fitted 2026-09-17 to fix it, result at the time "no visible improvement" on a related symptom - so this thread was never actually closed out, just deprioritized).

This could be more than a timing nuisance - if the "double" is a full valid-width pulse (not just ringing on one edge), it would shift an extra bit into the AD9851's register, corrupting that tick's whole frequency word alignment, not just delaying it. Worth determining which it is before assuming it's benign.

**Concrete next test, cheap because we built exactly this A/B path already**: flip `AD9851_ISR_WRITE_ENABLED` to `0` and reflash (same electrical write, task-side instead of ISR-side) and see if the WCLK double persists at the same rate - distinguishes "pre-existing hardware issue this fix made newly visible" from "a real new cost of writing from ISR context." Full reasoning and the other proposed checks (pulse-width/datasheet-minimum comparison, bit-pattern correlation, the still-never-tried FQ_UD-latch-only edge delay) in `null_bias_investigation.md`.

## 2026-09-19, later still: RESOLVED - "WCLK double" was a false lead; it's ordinary per-bit W_CLK pulse-width variation (70-126ns) from the un-delayed bit-bang loop, and that alone explains the write-duration and dsp-start jitter

User's scope work closed this out with two facts: WCLK positive pulses measure 70-126ns and are "quite clean" on a 100MHz scope (not noise/ringing), and - critically - "there are always 40 wclk pulses and when wider ones occur the whole seq is a little wider." Always 40: no doubled pulse, no data-integrity risk to the AD9851's shift register. What's varying is individual pulse WIDTH, not pulse count.

`AD9851.c` explains why on inspection: `AD9851_BITBANG_EDGE_DELAY_ENABLED=0` means the per-bit W_CLK toggle has no added software delay at all - just two back-to-back register writes, a raw GPIO pulse only a few ns wide. The 70-126ns the user actually sees is what the BS170 inverting level-shifter turns that into: its own already-documented slow, passive, pull-up-charged LOW-to-HIGH RC edge (vs. a fast, actively-driven HIGH-to-LOW edge) - a real physical effect whose exact time constant depends on the charge state going in, so it legitimately varies bit-to-bit without being noise. Arithmetically, a 56ns per-pulse range across ~20 of the 40 bits fully accounts for the ~1.2us AD9851 write-duration variation reported last entry, which in turn accounts for a real share of the ~4.6us dsp-start variation - no separate mechanism needed.

**Net effect on the to-do list**: the proposed `AD9851_ISR_WRITE_ENABLED=0` A/B test is no longer needed to chase this (it was never an ISR-vs-task-context effect - the un-delayed loop behaves the same either way), so that test is dropped from this thread. The BS170/push-pull edge-quality thread this reconnected to remains open in its own right (2026-09-17 push/pull fitting showed no visible improvement on the separate ~153.6s STUCK/RECOVER cycle) - this finding just confirms today's specific symptom is expected/benign under the current flags, not a new problem. If tighter WCLK pulse uniformity is ever wanted, re-enabling `AD9851_BITBANG_EDGE_DELAY_ENABLED` (or extending the `AD9851_FQUD_EDGE_DELAY_ENABLED`-style per-call delay to every bit) is the direct lever, at its previously-measured ~13-14us real-time-budget cost. Full derivation in `null_bias_investigation.md`.

## 2026-09-19, later still: CORRECTION - WCLK's 70-126ns pulse width is set mainly by the ESP32's own GPIO/APB bus round-trip latency, not the BS170 level-shifter's RC rise time as previously claimed

User cited real-world evidence (GitHub issue, Arduino/esp32.com forum threads) that back-to-back direct-register GPIO toggling on ESP32 parts is documented to be capped around 8-16MHz (~60-120ns per transition) by the APB bus/GPIO matrix round-trip itself, independent of CPU instruction speed - a far better match to the measured 70-126ns than the previous entry's "razor-thin raw pulse, stretched by BS170 RC charging" claim, which this corrects.

Revised understanding: the pulse width is very likely set mostly at the ESP32 pin itself by this bus latency, before the level-shifter is even involved; bit-to-bit variation is better explained by ordinary bus/pipeline jitter (shared dual-core GPIO/APB infrastructure) than by RC charge-state dependence. Every conclusion that mattered from the previous entry is unaffected - still always 40 pulses, still clean/not noise, and the arithmetic explaining the ~1.2us write-duration spread and downstream dsp-start jitter is mechanism-agnostic. What changes is the fix if tighter WCLK uniformity is ever wanted: not more level-shifter margin, but the ESP32-S3's dedicated/"fast" GPIO peripheral, which bypasses this exact APB round-trip (this project's own `fast_gpio_set()`/`fast_gpio_clr()` only skip `gpio_set_level()`'s call/critical-section overhead, not this bus path) - not yet evaluated. Full derivation in `null_bias_investigation.md`.

## 2026-09-19, later still: pivoting to analog/PWM-filter tuning - PSU rework may have shifted the DC-critical filter operating point; measurements pending

With the write-time-jitter fix stable and the WCLK pulse-width question closed out above, focus is shifting to the envelope PWM output filter's analog tuning. The 5V rail has since been reworked and is now "a proper 5V" (it wasn't before) - the PWM low-pass/reconstruction filter is DC-level-critical, so this could have shifted its operating point since it was last tuned. User will take fresh measurements before drawing any conclusion; nothing found yet, parked here as context for what's coming next in `null_bias_investigation.md`.

## 2026-09-19, later still: built REVISION 7 of the envelope predistort table from the new-PSU duty sweep - found a real, modest, non-uniform shift concentrated in the turn-on knee, plausibly linked to the PSU fix

User uploaded a full duty=0..1023 sweep (`DutyMeasNewPSU-1.txt`) taken after the 5V PSU rework and asked for the distortion table (`envelope_predistort.h`/`.cpp`, REVISION 6 previously current) to be checked and updated. Built REVISION 7 using the same pipeline as REVISIONS 4-6 (dBm->linear amplitude, isotonic-regression smoothing, PCHIP inversion at 65 envelope points) - this sweep is the first to include duty=0 itself and the first without a repeated ceiling reading.

Comparing against REVISION 6 duty-for-duty (reconstructing REVISION 6's curve from its stored LUT, then removing the uniform ceiling-anchored offset) found a real, non-uniform shift: floor moved only ~0.1dB but the ceiling moved ~0.42dB, and the corrected residual dips to about -1.6dB right in the turn-on knee (duty~200-210) while converging to near-zero at both the floor and the full-duty ceiling. That knee-concentrated, extremes-converge shape matches the signature REVISION 4->5's confirmed PNP-bias-starvation fix produced - consistent with, though not conclusive proof of, the PSU fix shifting the DC-critical filter's operating point. No controlled same-session A/B against the old PSU exists, so ordinary measurement variation can't be fully excluded either. The effect is real but modest (~1.6dB max, versus REVISION 5->6's tens-of-dB hardware-redesign shifts).

REVISION 7 adopted in `envelope_predistort.h`/`.cpp` (REVISION 6 now marked superseded); LUT[0]=0.0000 (duty=0, the true floor for the first time). Resolution check is excellent through 5-95% envelope, with the known "dead-zone-plus-turn-on collapses into the first bin" effect at the bottom (expected, not new) and moderate top-plateau error, both consistent with prior revisions' own documented limitations. Off by default, not yet validated via two-tone/IMD - matters more than usual this time since the moved region is the bias-sensitive knee. Full derivation in `null_bias_investigation.md`.

## 2026-09-20: checked new aAD/aADgB group-delay TFs (shelf1+shelf2+predistort, new PSU) - gdeq mechanism confirmed intact, but candidate B trades away more full-band flatness than before; no coefficient change recommended

User uploaded two hi-res TF sweeps with ampeq shelf1+shelf2 AND envelope predistort ('D') enabled for the first time - one with gdeq off (`aAD`), one with candidate B on (`aADgB`) - asking whether any group-delay improvement is available now.

Cross-validated the modeling pipeline first: predicted the aADgB curve as (measured bare aAD curve) + (candidate B's own exactly-known analytic allpass delay) and matched the real measurement to 3.8us RMS across 100-8000Hz, with the predicted vs. measured mean-delay addition agreeing to within 0.4us. Candidate B's basic mechanism is completely unaffected by predistort or the new PSU.

The real finding: candidate B now makes full-band (100-8000Hz) dispersion WORSE than gdeq off (119.7us bare -> 160.0us with candidate B) even though it still improves the 2800-4500Hz local slope it was actually designed for (-19.2 -> -11.1us/kHz) - the same full-band-vs-local-band trade-off this project already documented for candidate B, just sharper on this new curve. Searched directly against the real data for a better fit: the best full-band-only optimum (a1=a2=-0.182, 96.5us p-p) nearly doubles the local 2800-4500Hz dispersion relative to bare - the AA candidate's own already-rejected failure signature - so it's not being recommended. Swept the full coefficient range and confirmed a sharp, monotonic Pareto trade-off exists with no better simultaneous point than candidate B's existing a=0.09.

Flagged (not confirmed) that I can't do a clean historical before/after comparison for predistort's own effect on the curve without the old raw `HiRes_aA_TF.txt`/`ga_Trial2_TF.txt` file (only summarized numbers from memory, at slightly different fit bands) - avoided repeating this session's earlier lesson about asserting a specific mechanism without solid comparable data. Net recommendation: no coefficient change from this data alone; candidate B remains a reasonable compromise, and any real change should go through an actual two-tone IMD bench test, per this project's standing practice. Full derivation and chart in `null_bias_investigation.md` / scratchpad.

## 2026-09-20, later still: shelf1-only TF has no phase column (fit blocked, need a re-export), but its magnitude alone found a real lead on "A makes IMDs worse"

User's requested shelf1-only gdeq fit is blocked: `HiRes_aD_TF_New_5V_psu.txt` has magnitude only (no phase column, and a wider 0-24000Hz sweep than the previous mag+phase files) - no group delay can be extracted without real measured phase, so this needs a re-export in the same `FRF(Channel 2,Channel 1)` mag+phase format as before.

While checking the file, comparing its magnitude directly against yesterday's shelf1+shelf2 curve found shelf2 isn't showing anywhere near its designed +10dB (or previously-confirmed real +6.7dB) high-frequency boost - only ~0.6dB mean / ~2.2dB max difference anywhere in 6000-8000Hz. Since shelf2's group-delay cost is a direct, unavoidable consequence of the gain it's supposed to provide, this is a concrete, plausible explanation for the user's "A makes IMDs worse" observation: paying the full delay cost for little to none of the gain benefit. Not confirmed (could be a cross-session measurement artifact, same caution as the PSU/predistort comparisons) - recommended a same-session bench check (scope RSET output 6-8kHz with 'A' toggled, confirm shelf2's enable/coefficients are as expected) before concluding further. Full writeup and chart in `null_bias_investigation.md`.

## 2026-09-20, later still: shelf1-only TF re-export received and fit - all three existing gdeq sets make it worse, but a new candidate matches bare exactly on full-band p-p while halving local slope; not yet bench-tested

User re-exported the shelf1-only TF in the correct mag+phase format (`HiRes_aD_TF_New_5V_psu.txt`, second upload, "replacement") after the first attempt was magnitude-only. Extracted the bare (gdeq off) group delay with the established pipeline: 68.9us p-p / 68.2us mean over 100-8000Hz, 17.0us p-p / -7.55us/kHz local slope over 2800-4500Hz - a notably flatter, gentler curve than either the bare analog filter alone or the analog+shelf1+shelf2 curve the existing AA_CANDIDATE/CANDIDATE_B sets were fit against (156.5us p-p), confirming shelf2 is the dominant source of dispersion in those curves, not shelf1.

Checked all three existing coefficient sets against this new curve first, per this project's standing practice. All three make full-band p-p WORSE than gdeq off entirely: DEFAULT 68.9->84.7us, AA_CANDIDATE 68.9->101.0us, CANDIDATE_B 68.9->82.0us. Two of the three (DEFAULT, CANDIDATE_B) do flatten the local 2800-4500Hz slope close to zero (2.38 and 0.53us/kHz vs. bare's -7.55us/kHz) but only by overcorrecting past flat and only at that full-band cost - none is a free win on this curve shape.

Ran a fresh fit. A symmetric (a1=a2) search found almost no headroom (66.6us p-p, essentially bare). A full-band-optimal asymmetric search found a1=-0.3136/a2=+0.2255 (44.1us p-p, a real ~1.6x improvement) but worsened the local slope to -12.6us/kHz. A proper 2-parameter asymmetric Pareto sweep across several full-band budgets then found something the earlier candidate-B-era symmetric-only sweep never located for the shelf1+shelf2 curve: on this shelf1-only curve, asymmetric freedom lets local slope improve at essentially zero full-band cost. A budget-constrained optimization (grid multi-start + Nelder-Mead, penalizing any full-band p-p above bare's own 68.92us) converged on **a1=-0.110103, a2=0.286600**: full-band p-p 68.92us (matches bare almost exactly - no full-band cost at all) while cutting the local 2800-4500Hz slope from -7.55 to -3.39us/kHz (roughly halved, not fully flattened the way DEFAULT/CANDIDATE_B manage on this curve at their own cost). Mean added delay ~125.16us (2.003 samples @ 16000Hz), close to every other set's own ~2-sample addition.

Wired in as a fourth `'G'`-selectable variant, `ENV_GDEQ_VARIANT_A_CANDIDATE` (value 3, appended after the existing three so old persisted `env_gdeq_variant` preset fields keep their meaning), in `envelope_gdeq.h`/`.cpp` and `serial_commands.cpp`'s cycle/enum-name table, following the same "never silently run an unfit pair" and doc-comment conventions as every set before it. **Not yet validated on real hardware at all** - no bench re-sweep with gdeq switched on, no two-tone IMD check - the model-prediction-vs-measured-bare distinction is called out explicitly in the code comments and the `'G'` serial reply, same discipline this project used for candidate B before its own bench confirmation. Comparison chart (`gdeq_aD_shelf1only_analysis.png`) and code changes committed and delivered; full derivation in `null_bias_investigation.md`.

## 2026-09-20, later still: code review of shelf2 ('A') finds no implementation bug - but a simulation of the 'w' chirp test methodology reproduces the observed magnitude shortfall almost exactly, pointing at envelope clamping (clipping) during the TEST rather than a broken shelf

User asked for a code review of shelf2 given the magnitude-shortfall lead from the previous entry: is it "actually working as intended"? Read `envelope_ampeq.h`/`.cpp` and the underlying `ssb_shelf_biquad_set_highshelf()`/`_process()` in `ssb_dsp.c` line by line against the RBJ Audio EQ Cookbook high-shelf formula - the coefficient math, the S=1 simplification, and the Direct-Form-I process function all match the textbook formula exactly, and the enable/disable/reset/cascade wiring in both `envelope_ampeq.cpp` and `serial_commands.cpp`'s `'a'`/`'A'` handlers is correct. No bug found in the filter implementation itself.

Then simulated the actual test methodology that produces these TF files: `test_signals_generate_chirp()` swings the envelope across the FULL `AM_TEST_DEPTH=0.5` range (0 to 1, "100% depth AM, deliberately stressing the RSET path's linearity across its whole normalized range" per its own comment) at unity master gain by default - and `envelope_predistort_process()` hard-clamps ITS OWN input to [0,1] before the LUT lookup (separate from, and earlier than, the pipeline's own final safety clamp). Modeling the real RBJ shelf1/shelf2 cascade's exact magnitude+phase response, generating the resulting steady-state envelope waveform at each swept frequency, and applying that same [0,1] clamp reproduces the observed shortfall almost exactly: shelf1's own predicted +6dB collapses to a realized ~1.7dB by 6-8kHz, and shelf2's own predicted +5 to +10dB ADDITION on top of shelf1 collapses to a realized addition of only ~0.3-0.35dB - the same order of magnitude and same qualitative signature (both configs converge toward the same clamp-limited ceiling) as the ~0.6dB mean/2.2dB max shortfall found in the previous entry's real magnitude comparison. Chart: `shelf_clipping_hypothesis.png`.

**Open inconsistency, not yet resolved**: this doesn't cleanly explain why the ORIGINAL 2026-09-04 real-hardware test (`ga_Trial2_TF.txt`) measured a real +6.7dB gain from shelf2 (short of +10dB predicted, but nowhere near this simulation's ~0.3dB) - if the same 100%-depth chirp/unity-gain methodology applied then too, that test should have shown similarly crushed output. Checked git history: `AM_TEST_DEPTH` and the chirp generator's full-depth formula have been unchanged since the `'w'` test was first added, so this isn't a recent code change - the most likely explanation is that master gain (`'+'`/`'-'`) was set differently (lower) during that earlier session, which isn't recorded anywhere in the raw TF files or these notes. Flagging as an open question rather than asserting it, per this session's standing discipline.

**Recommendation**: before concluding shelf2 "doesn't work" or is somehow broken, re-run the shelf1-only vs shelf1+shelf2 `'w'` TF comparison at a clearly reduced master gain (several `'-'` presses - enough that even the full +16dB combined shelf boost near 8000Hz keeps the peak envelope inside [0,1]) and see whether shelf2's magnitude gain reappears close to its analytic prediction. If it does, this fully explains both the magnitude shortfall AND is a strong candidate explanation for "shelf2 makes real IMDs worse" - clipping introduces its own harmonic distortion directly, a much worse kind of nonlinearity than the smooth boost the shelf was designed to add, and would explain a real IMD regression independent of (in addition to) the already-documented group-delay-dispersion cost. Full derivation and simulation script in `null_bias_investigation.md` / scratchpad (`check_shelf_clipping.py`, `build_shelf_clip_chart.py`).

## 2026-09-20, later still: CONFIRMED by the user directly - shelf2's apparent gain shortfall is drive-level-dependent, and re-running the simulation through the real predistort LUT (not just a flat clip) matches both the level trend and the exact observed magnitude

User confirmed the previous entry's hypothesis directly from their own bench experience: the earlier (2026-09-04) shelf2 measurement was taken at a low drive level, while this session's new batch had predistort ('D') on at "quite a high level" - and reports the relationship is graded, not on/off: "the higher the level the more depressed the shelf2 gain becomes when D is on."

Re-ran the simulation using the ACTUAL REVISION 7 predistort LUT (not a simplified flat [0,1] clip) across a range of master-gain levels. Result matches both the trend AND the magnitude closely: at unity/full level (0dB, this session's setting), shelf2's realized added gain at 8000Hz is 0.61dB - matching the observed ~0.6dB almost exactly - and it climbs smoothly toward the full +10dB ideal as level is reduced, crossing +6.7dB (the original 2026-09-04 real measurement) right around -8 to -9dB down from unity. Chart: `shelf2_level_dependence.png`. This turns the previous entry's hypothesis into a confirmed, quantitatively-matched mechanism: shelf2 was never broken, and the earlier real +6.7dB vs. this session's ~0.6dB were both correct measurements of the same filter at two different, unrecorded drive levels.

Net implication: any future shelf2 (or shelf1+shelf2) TF/IMD comparison needs the drive level recorded and held constant, or explicitly backed off enough that even the full combined shelf boost (+16dB near 8000Hz) leaves headroom inside [0,1] - otherwise the measurement reports the clamp's behavior, not the filter's. Full derivation, the per-frequency-vs-level table, and the chart script are in `null_bias_investigation.md` / scratchpad (`check_shelf_clipping_v2.py`, `build_level_dependence_chart.py`).

## 2026-09-20, later still: new lead - a fixed 645Hz tone + harmonics appears on mic input, present even with the mic physically disconnected; not yet isolated to a cause

User reports mic input sounds best with both ampeq shelves off ('a'/'A' off, matching this session's shelf1/shelf2 investigation), but separately found a persistent 645Hz tone plus harmonics that's present even with the mic physically disconnected - meaning it's being injected somewhere internally rather than picked up acoustically.

Checked this project's own internal clock domains for a numeric match before assuming a hardware cause: SAMPLE_RATE_HZ=16000, ADC_CONT_SAMPLE_FREQ_HZ=80000 (ratio 5, exact), ADC ISR burst rate 80000/16=5000Hz, envelope PWM 64000Hz, gptimer alarm count 125 (2000000/16000, exact) - none of these divide into or relate cleanly to 645Hz or a simple multiple/submultiple of it, unlike this project's past ratio-mismatch bugs (the old 9600Hz/48000Hz cases both had clean, identifiable numeric explanations). No prior mention of "645" or 640-650Hz anywhere in either markdown log. This makes a firmware/digital-timing origin look unlikely, though not impossible.

**Recommended first test - zero hardware changes needed**: switch to `'s'` (AUDIO_SRC_SINGLETONE) or `'t'` (AUDIO_SRC_TWOTONE) instead of `'m'` (mic) - both bypass the ADC/mic path entirely with a synthetic source, which is exactly what `AUDIO_SRC_SINGLETONE`'s own design comment says it's for ("isolates the DSP/RF chain from mic-side confounds (preamp hum, mic nonlinearity, room noise)" - settings.h). If the 645Hz tone persists with a synthetic source, it's coming from somewhere in the shared envelope/PWM/AD9851/RF/power chain, not the mic/ADC front end at all - given this project's own precedent (the 16.5Hz comb traced to mixed PSU domains), that would point back at PSU/ground coupling first. If the tone disappears with a synthetic source, it's specific to the ADC/mic front end - point next at grounding the input (vs. floating) to distinguish EMI pickup by a high-impedance floating input from noise genuinely present on the ADC pin/bias supply itself. Not yet run. Full reasoning in `null_bias_investigation.md`.

## 2026-09-20, later still: strong candidate found for the 645Hz tone - the ADC/dsp-tick clock-rate mismatch this project already measured once before, dismissed as benign in a different investigation, lines up almost exactly

User asked directly whether the 645Hz tone could be "the offset between the 80kHz and what it actually is" - i.e. the ADC's real sample rate vs. its configured `ADC_CONT_SAMPLE_FREQ_HZ=80000` nominal value. Checking this project's own history: an earlier, unrelated investigation (two-tone-mode FIFO overflow false alarm) already logged a real measured reading of `[adc] actual=80645-80795 sps (expected=80000)` and judged it "fine" - but that judgement was about a completely different question (was the ADC driver healthy), not about whether this exact mismatch could produce an audible artifact in mic mode. 80645 sps is 645 sps over nominal - matching the reported tone almost exactly.

Read `adc_capture.cpp`'s FIFO drain logic (`adc_capture_read_next_sample()`, ~line 259-324) and found the mechanism explicitly, in the code's own words: it pops a fixed nominal `ADC_SAMPLES_PER_TICK=5` samples per tick, with a "+1 opportunistic bleed" branch specifically added because the true ADC-to-tick rate ratio is NOT exactly 5 - the comment there says this exists to avoid "periodic gulps that show up as regular visible/audible spikes" from a coarser catch-up threshold. Because both the ADC's sample clock and the main 16kHz dsp tick are fixed-rate (not random), any residual rate mismatch produces a deterministic, periodic correction event, not noise - straightforward accounting shows that event has to fire at a rate exactly equal to the ADC's absolute rate error (in Hz) to keep the FIFO balanced in steady state. With the historically-measured ~645Hz excess, that predicts a ~645Hz periodic timing hiccup in the decimated audio stream - matching the reported tone (and its harmonics, since each correction is a small timing step, not a pure sinusoid) closely, and fully explaining why it's present even with the mic disconnected (it's baked into the ADC-FIFO-to-tick decimation mechanics, independent of what's physically connected).

**Recommended verification - already-built diagnostics, no new code needed**: unmute diagnostics (`'v'`), let it run several seconds so the long-window average settles, and read the `[adc]   long-window avg=... sps (...% vs nominal 80000Hz)` and `[dsp]   long-window avg=... true_ratio=... (nominal=5)` lines (`diagnostics.cpp:2720-2757`). If the ADC long-window average comes back close to 80645 sps (or whatever this specific board's real offset is) and `true_ratio` deviates from 5 by a matching percentage, that confirms the mechanism directly and gives the exact number to check the tone frequency against. Not yet run this session.

**If confirmed, the fix is a proper fractional resampler** (accumulate a fractional sample-count error each tick, only advance the extra sample when it crosses 1.0, using linear interpolation between adjacent raw samples rather than an integer "pop 5 or 6") instead of the current opportunistic integer bleed, which would spread the correction perfectly smoothly rather than as a stable periodic event. Not yet designed or implemented - contingent on the diagnostic above confirming the mechanism first. Full derivation in `null_bias_investigation.md`.

## 2026-09-20, later still: user confirmed 80645 sps on the bench (closing out the 645Hz root-cause question as "known ESP32 ADC-continuous-mode quirk, exact mechanism unconfirmed"), then tried 65kHz ADC rate directly - tone gone, but a NEW, code-introduced decimation bug is the likely explanation for the "anti-alias needs sorting" symptom, not the LPF itself

User confirmed the previous entry's diagnostic prediction directly: read `sps` on the bench and saw 80645, matching the historically-logged `80645-80795 sps (expected=80000)` reading almost exactly. This closes out the 645Hz root-cause question as confirmed at the mechanism level (ADC-vs-dsp-tick rate mismatch, exact math already in `null_bias_investigation.md`).

User then asked WHY the ESP32-S3's ADC continuous mode doesn't hit exactly 80000Hz in the first place - is this a known issue? Web research (WebSearch/WebFetch) found real, Espressif-tracked GitHub issues describing exactly this class of problem (esp-idf#10612: configured 80000 sps, measured ~65287 actual, ~1.22x off, on ESP-IDF 5.0; esp-idf#11623: ADC1_DONE_INT firing at a fixed rate regardless of configured sample_freq_hz, "Selected for Development" i.e. acknowledged by Espressif) - so this does look like a known ESP32/ESP32-S3 ADC-continuous-mode driver characteristic, not something specific to this board. However, the exact internal mechanism (clock-divider granularity, APB clock quantization, etc.) could NOT be confirmed from available tools this session - WebFetch only surfaced issue summaries/labels, not the technical comment threads, `gh` CLI isn't installed here, direct GitHub API access is blocked by this sandbox's network proxy without an explicit grant, and the one esp32.com forum thread found was behind a bot-check wall. Flagging this honestly rather than asserting an unconfirmed mechanism: it's real and known, but Espressif's own root-cause explanation wasn't retrievable here.

**User then ran their own experiment**: recompiled with `ADC_CONT_SAMPLE_FREQ_HZ` changed from 80000 to ~65000. Result: the 645Hz tone is gone, sound is "nice and clean" - but reports "it looks like the anti-alias filtering needs sorting."

**Direct answer to "did we try 65kHz before":** no - checked `adc_capture.h`'s own "TRIED, REVERTED" history and only one alternate rate has ever been tried, 48000 (reverted for CPU-headroom/FIFO-starvation reasons, unrelated to today's investigation - see that same header comment). 65000 is new territory for this project.

**Important catch found while checking this - likely explains the "anti-alias needs sorting" symptom without it actually being an anti-aliasing/cutoff problem.** `ADC_SAMPLES_PER_TICK` (`adc_capture.h`) is a compile-time macro, `ADC_CONT_SAMPLE_FREQ_HZ / SAMPLE_RATE_HZ`, both unsigned ints - i.e. plain C integer division. 65000/16000 = 4.0625, so the macro silently truncates to **4**, discarding the .0625 remainder - directly hitting the exact hazard the macro's own comment warns about ("if either rate ever changes, check this stays an integer division with zero remainder"). That's not a small residual clock-inaccuracy error like the 80000 case (~0.8%) - it's a much bigger, code-introduced 1.5625% mismatch, baked in regardless of how accurate the ADC's real clock actually is at that setting. The same "+1 opportunistic bleed" logic in `adc_capture_read_next_sample()` has to absorb this, and would need to fire roughly 25x more often than before (~1000 times/sec, i.e. 16000 x 0.0625, if the ADC's true rate at that setting is close to 65000) - the same fundamental timing-skip artifact as the 645Hz tone, just happening so much more frequently it more likely reads as added noise/hash than a discrete tone, which fits "anti-alias needs sorting" better than an actual mistuned filter would.

Checked whether the anti-alias LPF's cutoff itself could be the real problem: `adc_capture.cpp` (`ssb_biquad4_lpf_init(&s_adc_lpf_butterworth, ADC_LPF_CUTOFF_HZ, (float)ADC_CONT_SAMPLE_FREQ_HZ)` and the Chebyshev equivalent) passes `ADC_CONT_SAMPLE_FREQ_HZ` in explicitly, so the biquad coefficients get recomputed correctly for whatever rate is configured - not hardcoded to 80000. The -3dB point should still land right at 3000Hz even at 65000. So the LPF's own tuning is not the likely culprit here - the decimation-truncation bug above is the better first suspect.

**Recommendation: try 64000Hz instead of 65000.** 64000/16000 = 4 exactly (zero remainder), avoiding the truncation bug entirely - same "clean division" property 80000 (ratio 5) has, just at ratio 4. One caution, not yet confirmed either way: the ISR burst-arrival gap at 64000 works out to 16/64000 = 250us (4 dsp ticks) between refills, versus 200us (3.2 ticks) at the working 80000 setting and 333us (5.33 ticks) at the previously-failing 48000 setting - 64000 sits between the two, so it isn't obviously safe from the same FIFO-starvation failure mode the 48000 experiment hit. Worth checking `[adc] fifo: min=...` / `starve_ticks_total` (same diagnostic used to catch the 48000 problem) after switching, before assuming it's clean. Separately, whether the ADC's true achieved rate at a requested 64000 lands close to nominal (vs. 80000's own ~0.8% offset) is itself unverified - check via the `[adc]`/`[dsp]` long-window diagnostics (`'v'`) at the new setting, same as recommended for 80000.

**Net status**: the 645Hz mechanism is now confirmed (both by measurement and by the user's own successful workaround). The underlying "why doesn't the ESP32-S3 hit its configured ADC rate exactly" question is answered at the "yes, known class of issue" level but not at the "here's Espressif's exact mechanism" level. The practical path forward doesn't need that deeper answer: pick a configured rate that (a) divides `SAMPLE_RATE_HZ` exactly, and (b) is confirmed clean via the existing long-window/FIFO diagnostics - 64000 is the next candidate to try, not yet bench-tested. Full derivation and the burst-gap arithmetic are in `null_bias_investigation.md`.

## 2026-09-20, later still: user actually tried 64000Hz (not 65000 - prior entry corrected), and it's not exact either - traced "other research said 64k is exact" to a real fact about a DIFFERENT peripheral in this same codebase, which doesn't transfer to the ADC

User corrected the previous entry: the "65000" experiment was a mistype - they actually tried `ADC_CONT_SAMPLE_FREQ_HZ=64000`, not 65000. They report it's "not exact either," despite having seen research suggesting 64k should be exact.

**This project already has that exact "64k is an exact clock divisor" fact on record - but for a different peripheral.** `group_delay_fit_notes.md` (2026-09-08 entry) documents retuning the envelope/RSET LEDC PWM carrier (`RSET_MOD_LEDC_FREQ_HZ`) from 78125Hz to 64000Hz specifically because "64000Hz... is *also* an exact integer division of the same 80MHz APB clock (80,000,000/1250)." That reasoning is real and correct - for the LEDC PWM peripheral's own clock-divider hardware. It does NOT automatically transfer to the ADC continuous-mode sample clock, which is driven by a completely separate clock-divider chain inside the SAR ADC digital controller, with its own (and, per the GitHub issues already found, apparently less well-behaved) rounding behavior. If "other research" here means this same 80MHz-divisor logic applied to the ADC by analogy, that's the likely source of the mismatch between expectation and bench result - it's a fact about the wrong clock domain.

**This is actually a clean, useful data point, not just a disappointment.** `64000/16000=4` exactly (zero remainder) - unlike the earlier 65000 mistake, there is NO code-side decimation-truncation bug at 64000 (`ADC_SAMPLES_PER_TICK` is genuinely, correctly 4 here). So whatever mismatch the user is seeing at 64000 is 100% pure ADC-driver/clock inaccuracy, cleanly isolated from any firmware bug on our side. Combined with the original 80000 measurement (80645-80795 sps against a configured 80000, also a "clean" APB divisor at ratio 1000), this is now TWO independent real bench data points, at two different configured rates that are both exact APB-clock divisors, both showing the driver still not landing exactly on target. That's stronger, more direct evidence than the cited GitHub issues alone that this chip/IDF combination's `adc_continuous` driver doesn't reliably hit an exact configured `sample_freq_hz` - picking a "cleaner" number doesn't appear to fix it.

**Still needed**: the actual measured number at 64000 (via `'v'` + the `[adc] long-window avg=...` line) - not yet reported. That number would say how far off 64000 is in absolute/percentage terms (better or worse than 80000's ~0.8%?) and, via the same conservation-math formula as before, exactly what residual periodic-artifact frequency (if any, and however faint) to listen for.

**Updated recommendation, now with two data points instead of one behind it**: stop trying to find a configured frequency that happens to be "exact" - the evidence increasingly suggests none reliably is, on this specific ADC-continuous driver. Instead, treat whatever true rate is actually measured at a given setting as a fixed, empirical constant, and build the previously-proposed fractional resampler (accumulate fractional sample error each tick, only advance an extra raw sample once it crosses 1.0, ideally with interpolation) around that real number - this makes the fix work regardless of which configured frequency is chosen or how the driver's internal rounding behaves at that value. Full reasoning in `null_bias_investigation.md`.

## 2026-09-20, later still: implemented a proper calibrated fractional resampler in adc_capture.cpp, replacing the old fixed-ratio integer "+1 bleed" scheme - not yet bench-tested

User confirmed the same GitHub research already logged (esp-idf#10612/#11623) as the debunked source, and asked to implement the fractional resampler that's been proposed (but not built) since the original 645Hz finding. Also asked directly: **do we know what the actual chip Fs is?** No - not as a single fixed number. All that's on record is the historical range at a configured 80000 (80645-80795 sps, i.e. roughly +0.8% to +1.0%) and the user's own "it varies a bit" observation; nothing precise has been logged yet at the 64000 setting. That "varies a bit" is itself important - it argues against hardcoding any one-time measurement into the fix.

**Design implemented in `adc_capture.h`/`adc_capture.cpp`, gated on that exact reasoning**: rather than assume a fixed true rate, the resampler now measures its own live Fs_adc/Fs_dsp ratio at runtime (`s_true_ratio`), recalibrating every `ADC_RATIO_CALIB_WINDOW_CALLS` (~4s) by comparing real ISR-counted samples against its own call count - so it tracks whatever the actual ratio is on THIS board, at THIS configured rate, right now, including any slow drift, rather than encoding one historical measurement that might not hold at a different `ADC_CONT_SAMPLE_FREQ_HZ` setting or a different board. On top of that measured ratio, `adc_capture_read_next_sample()` now tracks a continuously-advancing fractional virtual sample position and linearly interpolates between the two bracketing filtered raw samples for each tick's output, instead of the old "pop N or N+1 raw samples, return whichever was last" scheme - spreading the correction smoothly across every tick rather than concentrating it into periodic "+1 gulp" events. Full technical description in `null_bias_investigation.md` and in the new doc comment on `adc_capture_read_next_sample()` itself.

Added `adc_capture_get_true_ratio()`/`adc_capture_ratio_is_calibrated()` and a new `[resamp] calibrated true_ratio=...` diagnostic print (alongside the existing `[dsp] true_ratio=...` line) specifically so the new mechanism's own measurement can be checked against the pre-existing independent one as a cross-check before trusting it.

**Not yet bench-tested at all - flag this clearly before relying on it.** Needs, in order: (1) confirm it builds; (2) confirm mic audio still sounds correct (no new artifacts introduced by the interpolation/lookahead change); (3) read the new `[resamp]` line and confirm it converges close to the existing `[dsp] true_ratio` figure, as a sanity check that the two independent measurement methods agree; (4) listen specifically for whether the periodic tone (645Hz-class, or whatever the residual is at the currently-configured rate) is now gone or meaningfully reduced rather than just moved. None of this has been run yet.

## 2026-09-20, later still: noted for the record - classic ESP32 (not S3) has an I2S-based ADC path with a much finer clock divider; a possible future escape hatch if the software fractional resampler doesn't fully solve the rate-mismatch problem

User flagged that some ESP32 variants (not the S3 this project targets) have an "I2S fractional clock divider" on-chip, worth noting as a possible future board-change lever if the software fix isn't enough.

**Confirmed via ESP-IDF docs**: classic ESP32 has an I2S-based ADC mode (`i2s_set_adc_mode()`/`i2s_adc_enable()`, capped at 150kHz) that reads the SAR ADC through the I2S peripheral's own clock/DMA path - this does NOT exist on ESP32-S2/S3/C3 at all, which is plausibly why those chips (including the S3 this project uses) rely instead on the separate `adc_continuous` SAR-ADC-DMA driver this whole 645Hz investigation has been chasing. Also confirmed: ESP-IDF v5.x's migration guide states ADC/DAC modes were removed from the (new) I2S driver - "only supported in their own drivers and the legacy I2S driver" - so even on classic ESP32 this path is being phased out of the current driver and may need an older ESP-IDF/Arduino-core release or the legacy I2S driver specifically.

**The specific "finer fractional clock divider" claim**: plausible and consistent with general ESP32 I2S clock architecture (commonly cited as why I2S-based audio on ESP32 can hit standard rates like 44.1kHz precisely), but NOT independently re-confirmed from a fetchable, quotable source this session - the one forum thread most likely to spell it out was blocked by the same bot-check wall hit earlier in this same 645Hz investigation. Flagging as "plausible, not independently verified here" rather than asserting it as settled fact.

**Practical framing, not a recommendation to switch yet**: swapping from S3 to classic ESP32 (or another I2S-ADC-capable variant) would be a large undertaking, not a drop-in change - different pinout, no native USB (affects the Serial/USB-CDC diagnostics workflow this whole project leans on heavily), and this project's own considerable S3-specific real-time timing work (Core 0/Core 1 budget margins, gptimer wakeup jitter history, the AD9851 bit-bang SPI timing) would need re-establishing on different silicon. Given the fractional resampler was just implemented this session and hasn't even been bench-tested yet, the sensible order is: test whether the software fix actually solves the audible problem first; if it does, this becomes moot; if raw ADC clock precision turns out to matter for some reason the software fix can't cover, this is a legitimate escape hatch worth having on record. Full reasoning and sources in `null_bias_investigation.md`.

## 2026-09-20, later still: user bench feedback on the fractional resampler - 645Hz reduced but new noise/IMD-like products appeared, worse than plain 64k without resampling; found and fixed a real bug in the resampler itself (unbounded float precision loss); also answered "is there a closer-fit ADC Fs" honestly

User reported real bench results on the just-implemented resampler: the periodic tone is reduced but not gone, and a number of new noise products appeared (their own guess: IMD) - to the point that plain 64000Hz WITHOUT the resampler sounded better. Also asked directly whether some other `ADC_CONT_SAMPLE_FREQ_HZ` value might be a closer fit than 80000/64000.

**Found a real, concrete bug in the resampler that's very likely THE cause of the new noise, independent of anything about the ADC's own timing.** The first version tracked a single `float s_virtual_pos` that grew without bound for as long as mic mode ran, incremented by ~4-5 every dsp tick (~64,000-80,000/sec). float32 can only represent integers exactly up to 2^24 (16,777,216) - at that increment rate, the accumulator hits that ceiling in **under 5 minutes of continuous operation**, after which every addition silently rounds to the nearest representable step, corrupting both the per-tick sample count and the interpolation fraction derived from it. This is a real, physically-guaranteed source of new timing jitter/distortion that gets WORSE the longer the board has been running - a strong, concrete explanation for "new noise products" that's actually a fresh bug introduced by this session's own fix, not a property of the underlying hardware problem.

**Fixed** by replacing the unbounded position with a bounded [0,1) phase accumulator plus an exact-integer running target counter (uint32_t accumulation never loses precision short of a ~16.5-hour wraparound, which unsigned arithmetic handles correctly) - verified by hand to be mathematically equivalent to the original design's intent (see `null_bias_investigation.md` for the derivation), just numerically stable indefinitely instead of degrading after a few minutes. Also added a defensive sanity clamp on the calibrated ratio itself. **Still not bench-tested** - this is a fix for a bug found by reasoning through the float32 math, not yet confirmed to actually resolve the reported noise on real hardware.

**Direct answer: is there a closer-fit ADC Fs?** No known way to predict one without measuring it directly. Two "clean" candidates have now been tried (80000, both exact divisors of the 80MHz APB clock at different ratios) and BOTH came out measurably inexact, by different and not-obviously-related amounts - there's no evidence that picking a nicer-looking divisor helps, consistent with the Espressif-acknowledged driver issues already found. Finding a genuinely closer value would require an actual empirical sweep (flash a candidate, read `[adc] long-window avg=...`, repeat) rather than more clock-divisor arithmetic - a real time cost, and one the resampler is specifically meant to make unnecessary once its own bug is actually fixed. Recommended order: re-test the bugfixed resampler first (ideally at the same Fs as the last listening test, for a clean before/after) before spending time hunting for a different Fs.

## 2026-09-21: bugfixed resampler confirmed better; user's own Fs-error survey explains why 64k beat 80k; found and fixed a second resampler bug (mode-switch staleness) from an intermittent "funny mode" report

User confirmed the bugfixed resampler (float32 precision fix) is an improvement. Direct question answered: **yes, it's linear interpolation** - `s_filt_prev + s_phase*(s_filt_curr - s_filt_prev)`, a straight blend between the two bracketing filtered raw samples, exactly as designed.

**New bench data from the user**: some residual 645Hz remains at 80000 configured; switching to 64000 is "much better with no obvious disadvantages." They also report their own survey: all `x16` candidate rates (16000/32000/48000/64000) show ~0.16% error, while 80000 alone shows ~0.8% - roughly 5x worse. This is a genuinely useful new data point, not yet explained: it's NOT simply "clean APB-clock divisors do better" (48000 isn't a clean divisor of the 80MHz or 160MHz reference clocks the way 16000/32000/64000/80000 are, yet it groups with the good ones, not the bad one) - something specific to 80000 itself, not to "how cleanly the number divides," is responsible. One plausible (not confirmed) explanation: ESP32-S3's `adc_continuous` has a documented maximum continuous sample rate, and 80000 sits close to that ceiling - clock dividers commonly have coarser/larger relative quantization error near the low end of their achievable divisor range (i.e. at the HIGH end of the achievable sample-rate range), which would explain a rate close to the ceiling behaving disproportionately worse than rates comfortably below it. Flagged as a reasonable hypothesis, not verified - full reasoning in `null_bias_investigation.md`. Given this data, 64000 remains the better working choice for now.

**Found and fixed a second, distinct resampler bug from a new intermittent report**: user saw "a funny 'mode' with strange IMDs" on BOTH 64k and 80k (ruling out anything specific to the configured rate) - "OK today so far, maybe related to another setting." Traced this to a real mechanism: `adc_capture_read_next_sample()` (and the calibration window it drives) only runs in mic mode, but the ADC's own ISR keeps counting samples continuously regardless of audio source (`adc_capture_service()`'s own comment confirms this). So: spend time in any non-mic mode (`'s'`/`'t'`/etc.), switch back to `'m'`, and the next calibration window measures samples produced over the WHOLE non-mic interval against only that window's ~4s of calls - an inflated ratio. The existing defensive clamp catches the most extreme cases, but a shorter mode excursion can land the inflated value just inside the accepted range, corrupting `s_true_ratio` with a wrong number and producing exactly the kind of "funny mode" reported. **Fixed** by adding `adc_capture_resample_prime_calibration()`, called from both the `'m'` handler and the preset-apply path in `serial_commands.cpp` every time audio source switches into mic mode, so the calibration baseline is always fresh at the moment mic mode is (re-)entered rather than only at boot/`'r'`.

This is a strong, well-reasoned candidate for what was seen, but not confirmed by reproducing it - the user's own report was intermittent and not deliberately triggered via a mode switch, so it remains possible something else (their own guess: "another setting") also contributed. Worth watching for whether the "funny mode" recurs now that this specific mechanism is closed off. Full reasoning in `null_bias_investigation.md`.

## 2026-09-21, later: ADC clock-rate root cause CONFIRMED (not just plausible) - user found the exact formula, and it matches every measurement in this project's history to high precision

User found the missing piece this whole investigation's web research couldn't retrieve: the ESP-IDF v4.4.5 ESP32-S3 ADC docs (`adc.html`) state the continuous-mode clock formula directly: **`Fs = Fd / interval / 2`**, where `Fd` is the "digital controller frequency, no larger than 5M for better performance" and `interval` is (confirmed by fitting real data below) an integer register value.

Taking `Fd = 5,000,000` exactly and `interval = round(Fd / (2*Fs_nominal))`, computed for every rate this project has actually tried or measured:

| Fs nominal | ideal interval | rounded | Fs actual | error |
|---|---|---|---|---|
| 16000 | 156.25 | 156 | 16025.64 | +0.160% |
| 32000 | 78.125 | 78 | 32051.28 | +0.160% |
| 48000 | 52.083 | 52 | 48076.92 | +0.160% |
| 64000 | 39.063 | 39 | 64102.56 | +0.160% |
| 80000 | 31.25 | 31 | **80645.16** | **+0.806%** |

The 80000 prediction (80645.16 sps) matches the historically-measured 80645-80795 sps almost exactly, and the 0.160%/0.806% split matches the user's own bench survey (0.16% for 16k/32k/48k/64k, 0.8% for 80k) essentially exactly. **This is now a confirmed, not speculative, root cause** - both of the original 645Hz tone at 80000 AND of why 64000 measurably outperforms it. The earlier "proximity to the sample-rate ceiling" hypothesis is superseded by this exact mechanism: 80000 lands on a badly-rounded `interval` value (31.25 -> 31, a relatively large proportional rounding error) essentially by chance, not because of ceiling proximity.

**Practical implication: 64000 (and 16000/32000/48000/96000) are already at the floor this formula allows** - all four land on the same ~0.16% error, set by how `2,500,000 = Fd/2` factors relative to `SAMPLE_RATE_HZ=16000`'s own multiples (`2,500,000 = 2^5 x 5^7` has too few factors of 2 for any multiple of `16000 = 2^7 x 5^3` to divide it exactly - there's a hard floor here, not a config mistake). No further Fs-hunting is worthwhile; 64000 is a good, now fully-understood choice, and its predicted actual rate is 64102.56 sps - worth a final bench cross-check against the `[adc] long-window avg=...` reading as a closing confirmation.

This retires the earlier "known issue, exact mechanism unconfirmed despite GitHub/forum research" framing from prior entries - the mechanism is now known precisely, straight from Espressif's own documentation (just not from the GitHub issue threads this session's tools couldn't reach).

## 2026-09-21, later still: added two new envelope-path stages, ALC and Soft-Limit - graceful alternatives to the final hard clamp, inserted before predistort per the user's explicit placement instruction

With the ADC rate investigation closed out, the user asked for "a new feature or two - ALC and soft clipping." After clarifying placement (first answer was ambiguous; corrected to "needs to go before Distortion correction since that is needed 'always'"), both are now implemented as independent, off-by-default stages: `envelope_alc.h`/`.cpp` (ALC/leveler) and `envelope_softlimit.h`/`.cpp` (soft-knee saturation), called in `ssb_mic_test.ino`'s dsp_task right after `envelope_ampeq_process()` and before the `envelope_predistort_get_enabled()` check - i.e. AFTER gdeq/ampeq (so they see any real overshoot those stages introduce, shelf2 especially) and BEFORE predistort, which must keep running unconditionally.

**ALC** (`envelope_alc.h`, toggle `'l'`) is a gain-riding leveler, not a boost-quiet-audio ALC: it only ever attenuates peaks back toward `ENV_ALC_TARGET_LEVEL` (0.90), with a fast attack (2ms) and slow release (200ms), and never boosts gain above unity - boosting a polar-TX envelope above what the source audio actually contains would synthesize extra RF deviation, a materially different risk than a receive-audio ALC's usual "make quiet parts louder" job. One state variable (the smoothed gain itself), computed via the standard gain-computer-then-smooth topology, with a floor (`ENV_ALC_MIN_GAIN`=0.20) so a single outlier sample can't collapse gain toward zero, and a small-envelope epsilon guard against the target/envelope division blowing up near a null.

**Soft-Limit** (`envelope_softlimit.h`, toggle `'S'`) is a stateless tanh-based soft-knee saturator with independent knees near each rail (`ENV_SOFTLIMIT_KNEE_LO`=0.15, `ENV_SOFTLIMIT_KNEE_HI`=0.85): envelope passes through completely unchanged between the knees (so normal-level audio is unaffected - this doesn't retune predistort/pwm-scale operating points), and only beyond a knee does the curve bend toward its rail, asymptotically, never quite reaching 0 or 1. Verified algebraically C1-continuous at each knee (both value and slope match the unchanged region there), so there's no kink/discontinuity artifact right at the threshold. Handles BOTH rails deliberately - gdeq (all-pass) and ampeq (real filters, can ring) can both push the envelope transiently negative, not just over 1, and there's no ALC-equivalent protection for the lower rail.

Both are independently toggleable per the user's explicit request, both off by default, and both are first-cut implementations with parameters chosen by inspection (matching this project's established "modest first cut, refine on the bench" pattern already used for ampeq's shelf gains) - **NOT bench-validated**. `settings.h`'s `PersistentSettings` struct gained two trailing bool fields (`env_alc_enable`/`env_softlimit_enable`), appended at the end per the established zero-fill convention so none of the 10 existing presets needed editing. `'P'`'s pasteable settings-line dump and the numeric preset-apply path were both updated to include them. `diagnostics.cpp` gained a `[env]   alc: enabled=... gain=... canary_finite=...` periodic status line so ALC's live gain is visible on the bench without a separate debug build.

Recommended next step before trusting either on real audio: enable 'l' alone first (simpler, one moving part) with two-tone/mic input and check for any new IMD signature the leveler's own gain modulation might introduce (amplitude modulation of the envelope at the leveler's own attack/release rate is a plausible new spur mechanism, not yet checked for); then 'S' alone; then both together; comparing against the existing final hard clamp's own behavior (which becomes a true no-op once Soft-Limit reliably keeps the signal in [0,1], but stays in place as a backstop either way).

## 2026-09-21, later still: real Arduino IDE build error on the new ALC/Soft-Limit files - missing esp_attr.h include, fixed

First actual compile attempt (Arduino IDE, ESP32-S3 target) failed on both new headers: `error: expected initializer before 'envelope_alc_process'` at the `float IRAM_ATTR envelope_alc_process(float envelope);` declaration line. Root cause: `IRAM_ATTR` is a macro from ESP-IDF's `esp_attr.h`, not something the Arduino toolchain defines globally - `envelope_ampeq.h`/`envelope_gdeq.h` only get it by transitively including `ssb_dsp.h`, which itself `#include "esp_attr.h"`. `envelope_alc.h`/`envelope_softlimit.h` had no reason to include `ssb_dsp.h` and were only pulling in `config.h`/`stdbool.h`, so `IRAM_ATTR` was an undeclared identifier - the compiler parsed `float IRAM_ATTR envelope_alc_process` as "declare a float variable named IRAM_ATTR" followed by an unexpected second token, producing exactly this error shape rather than an "undefined macro" complaint (there's no such diagnostic for an unrecognized plain identifier used this way - it just gets misparsed as a declaration).

**Fix**: added `#include "esp_attr.h"` directly to both `envelope_alc.h` and `envelope_softlimit.h`, rather than pulling in the heavier `ssb_dsp.h` for a single macro neither module otherwise needs. Confirms the earlier compile-readiness check (brace/paren balance only) genuinely wasn't sufficient to catch this - this is the kind of macro-visibility issue that only surfaces on a real toolchain run, worth remembering before claiming new code "should compile" again in a future session without one.

## 2026-09-21, later still: real bench finding - Soft-Limit was raising the DC level of quiet/near-null signal; root cause found and fixed (lower-rail curve redesigned)

First bench test of the new Soft-Limit stage after the build fix above: user reported "at low signal level it raises the env level DC value." Confirmed immediately by direct calculation of the original formula (`ENV_SOFTLIMIT_KNEE_LO - span*tanhf((ENV_SOFTLIMIT_KNEE_LO - envelope)/span)` with `span = ENV_SOFTLIMIT_KNEE_LO = 0.15`): envelope=0.0 (a genuine envelope NULL) mapped to **0.0358**, envelope=0.05 mapped to 0.0626 - a real, provable DC floor across the entire 0-0.15 range, not a subtle or borderline effect.

**Root cause**: the lower knee was placed at a POSITIVE value (0.15) by naive mirroring of the upper knee's shape, but that treats ordinary low-but-legitimate signal (everything from 0 up to 0.15 - which includes every envelope-null crossing, the single most common and most closely-studied operating point in this whole project, per null_bias_investigation.md) as if it were undershoot needing correction. The upper knee's geometry doesn't have this problem because there's real headroom between the knee (0.85) and the rail (1.0) that's genuinely rare operating territory; the lower rail has no equivalent headroom - 0 IS the rail, so any positive-valued "knee" sits inside legitimate signal, not outside it.

**Fix**: re-derived the lower-rail curve from the actual rail (envelope=0.0), replacing the tanh mirror with `ENV_SOFTLIMIT_LOWER_SPAN * (1 - sech(envelope/ENV_SOFTLIMIT_LOWER_SPAN))` for envelope < 0.0 only. This gives exactly y=0 at envelope=0 (fixing the reported bug outright) and, because sech has no linear term near 0, a QUADRATIC (not linear) response to small negative excursions - deliberately gentler than a straight tanh mirror would be, which was shown separately to full-wave-rectify small ripples (worse average bias than even the existing hard clamp). Verified numerically: envelope=-0.01 now maps to ~0.00033 (previously the mirrored design would have given ~0.01, i.e. |x|). Full technical derivation, including why NO smooth/bounded/monotonic curve can perfectly match the passthrough region's unity slope at exactly envelope=0 (a real mathematical constraint, not a tuning gap), is in envelope_softlimit.h's 2026-09-21 bugfix entry and null_bias_investigation.md's matching entry.

Renamed `ENV_SOFTLIMIT_KNEE_LO` to `ENV_SOFTLIMIT_LOWER_SPAN` (same numeric default, 0.15, but now genuinely a different role - a saturation-plateau width, not a knee position) since the feature was same-day/never-pushed/not yet in any persisted preset data, so the rename carries no migration burden. Updated the 'S' serial-command handler's status line to match. Still NOT bench-validated end-to-end - this fixes the specific confirmed bug, but the header itself flags an open, honest question: if real gdeq/ampeq ringing rarely pushes envelope meaningfully negative in practice, this whole lower-rail branch may be solving a mostly-theoretical problem and could eventually simplify to a plain floor-at-0 - worth checking once more bench time is available.

## 2026-09-23: ssb_mic_test_commands.md brought up to date - added 'l'/'S', found and documented 8 other live commands that were never in the doc at all

User asked for `'l'`/`'S'` (ALC/Soft-Limit) to be added to the command reference "and check back for any others not already included." Cross-checked every `c == '<char>'` branch in `serial_commands.cpp` against every backtick-wrapped key already documented in `ssb_mic_test_commands.md` (both plain `` `X` `` and quoted `` `'X'` `` forms, since some existing entries use the quoted style inline in prose rather than a table row). Beyond `'l'`/`'S'`, found NINE keys with zero mentions anywhere in the file: `'O'` (two-tone phase-generator EXACT/LEGACY/MOD160 cycle - added 2026-09-17/18, genuinely just missed), and eight diagnostics/utility keys - `'L'` (one-shot FreeRTOS task list), `'M'` (separate mute for the auto-dump watchers), `'V'` (on-demand diagnostic snapshot), `'Z'` (clean reboot), and the AD9851-gated frequency-jump tracing tools `'J'`/`'K'`/`'H'`/`'F'` (jump log / slow-mean trigger / held-frequency detector / full-rate freq-env capture) - all four of the last group were built and used extensively during this project's frequency-jump investigation (see `null_bias_investigation.md`) but apparently never made it into this reference doc despite being live, working commands the whole time.

Added: a new "Envelope ALC / Soft-Limit" section (placed in pipeline order, right before "Envelope pre-distortion") covering `'l'`/`'S'` plus a condensed version of the same-day DC-floor bugfix writeup; an `'O'` row in the Signal source table; four new rows (`'L'`/`'M'`/`'V'`/`'Z'`) in the ADC/diagnostics table; and a new "Frequency-jump / null-bias tracing tools" subsection covering `'J'`/`'K'`/`'H'`/`'F'` together, since they're a closely related family best read as a set rather than four scattered rows. Re-ran the full cross-check afterward (every command letter/punctuation key in the code against the doc) and confirmed zero gaps remain. `'G'` (gdeq variant cycle) was checked too and found already covered - only in prose within the gdeq section, never given its own table row, but that's pre-existing and out of scope for this pass (the user asked about undocumented commands, not under-tabulated ones).

## 2026-09-23: ADC anti-alias filter widened to 6th/8th order (genuine higher-order designs, not a naive double-pass) - plus a live CPU-headroom reading that changes the risk picture from earlier in this project

User's mic input is currently bottlenecked by a poor electret capsule (better module on order), but separately flagged the ADC anti-alias filter's stopband (still 4th order at the time, `ssb_biquad4_t`/`ssb_biquad4_chebyshev_lpf_init()`) as limiting out-of-band rejection above ~3000Hz, and asked about "doubling up" the filter order - specifically proposing running the existing 4th-order filter through itself twice.

**That literal proposal was checked and is a worse choice than it sounds, for three concrete reasons**: (1) cascading an already-4th-order filter with itself squares its transfer function, and squaring a magnitude response doubles it in dB terms at *every* frequency - the existing Chebyshev design's ~1.4dB peak in-band ripple would become ~2.8dB, i.e. it compounds the exact ripple concern the user raised rather than managing it; (2) it shifts the *combined* -3dB point to a lower frequency than the single-pass filter's own -3dB point (two lossy stages each losing a little near 3000Hz reach the combined -3dB threshold sooner than either alone), so the passband edge would drift without a compensating retune; (3) it roughly doubles the filter's own group-delay contribution to the mic path.

**Implemented instead**: genuine 6th-order and 8th-order filters (`ssb_biquad6_t`/`ssb_biquad8_t` - three/four cascaded `ssb_biquad_t` stages respectively, in `ssb_adc_filter.h`/`.c`), extending the exact same closed-form design math the existing 4th-order filter already uses - just more pole pairs, not new theory. Butterworth uses the standard per-stage pole-angle Qs (`Q_k = 1/(2*cos(theta_k))`, `theta_k=(2k-1)*pi/(2N)`); Chebyshev uses the same ellipse-pole-pair derivation plus the same one-time init-time bisection (~40 iterations) to land the TRUE -3dB point exactly at `ADC_LPF_CUTOFF_HZ`, both patterns already validated for N=4 in this codebase. Numerically verified (Python replica of the exact bilinear-transform code, at the user's live fs=64000Hz/fc=3000Hz/1dB ripple) that going to higher order at a FIXED ripple spec does **not** multiply the ripple the way double-passing would: peak in-band ripple stayed ~0.99dB at N=4, N=6, AND N=8 alike, while stopband rejection at 8kHz went from -48.4dB (4th order Chebyshev) to -76.9dB (6th) to -105.8dB (8th) - each step roughly repeating the same "one more order roughly doubles/more-than-doubles the dB rejection" pattern this project already validated going from 2nd to 4th order. Butterworth showed the same clean pattern with pure linear scaling (as expected - Butterworth's dB attenuation scales linearly with N far into the stopband): -35.7dB (4th) -> -53.5dB (6th) -> -71.4dB (8th) at 8kHz.

Wired into `adc_capture.h`/`.cpp` as four new `adc_lpf_mode_t` values - `ADC_LPF_MODE_BUTTERWORTH6`/`_CHEBYSHEV6`/`_BUTTERWORTH8`/`_CHEBYSHEV8` - **appended** after the original three (values 3-6, not renumbered) so existing presets referencing `ADC_LPF_MODE_BUTTERWORTH`/`_CHEBYSHEV` by name keep meaning exactly the 4th-order filters they always did. The `'f'` toggle now cycles all seven modes (off -> 4th Butterworth -> 4th Chebyshev -> 6th Butterworth -> 6th Chebyshev -> 8th Butterworth -> 8th Chebyshev -> off) so each order/family combination can be A/B'd live on real hardware. Canary (`adc_lpf_canary_t`) and the `'P'`/`'0'-'9'` preset plumbing were all extended the same way.

**Why this isn't a free lunch this time, unlike the 2nd->4th jump**: a fresh on-demand `'V'` read taken specifically to answer this question came back `max_busy_us=60` against `period_us=62` - only ~2us of headroom, a much tighter margin than the ~16.5us seen in the last pre-resampler/pre-ALC/pre-Soft-Limit measurement this project had on file (2026-09-11, `max_busy_us=46` vs the same 62.5us budget). The `adc=` bucket specifically has grown from 5us then to 28us now, almost certainly dominated by the fractional resampler's calibration/interpolation math added since (see the 2026-09-20 entries), not the LPF itself - a back-of-envelope estimate puts one extra 4th-order-worth of biquad evaluations (the 4th->8th step) at roughly 1-2us, which is a meaningful fraction of a 2us margin, not the "negligible" cost the original 2nd->4th jump could fairly claim when headroom was ample. **This has NOT been bench-verified with the new filters actually running** - the very next step on real hardware should be cycling `'f'` through each new mode and reading `'V'`'s `max_busy_us`/`adc=` fields after each, watching specifically for `overruns` ticking up. 6th order (added as a genuine middle option, not just 4th vs 8th) is the safer first thing to try given how tight the margin reads right now.

Separately noticed while pulling the live `'V'` output for this: it reports `[adc] actual=64152 sps (expected=64000)`, but the repo's own `ADC_CONT_SAMPLE_FREQ_HZ` (adc_capture.h) is still `80000u` - the 64000Hz value this project settled on via the Fs-hunt investigation (see the 2026-09-21 entries above and null_bias_investigation.md) appears to exist only as a local, uncommitted edit on the user's actual board, not in this repo. Not acted on here (out of scope for this thread and the user hasn't raised it), but flagged so it doesn't quietly stay a source of repo-vs-bench divergence.

Also worth a mention, unprompted: the same `'V'` snapshot's `ad9851 breakdown: prep_us=2 spi_us=11` vs `write_us=37` leaves a ~24us unaccounted gap in the AD9851 write path - by itself larger than anything the filter-order question touches, and the more promising place to look for real CPU headroom if the new filter modes turn out to need it. Not investigated this session; parked here for whenever it's picked up.

## 2026-09-23, later: repo's `ADC_CONT_SAMPLE_FREQ_HZ` brought in sync with the user's live board (80000 -> 64000)

Noticed while pulling the `'V'` diagnostic for the ADC-filter-order thread above that the repo's `adc_capture.h` still had `ADC_CONT_SAMPLE_FREQ_HZ=80000u`, while the live board's own `[adc]` line read `actual=64152 sps (expected=64000)` - the 64000Hz setting this project settled on via the Fs-hunt investigation (2026-09-20/09-21 entries above; full mechanism in null_bias_investigation.md) had only ever been applied locally on the user's board, never committed here. User confirmed: "commit ADC_CONT_SAMPLE_FREQ_HZ=64000 your end. It works much better as previously discussed."

Changed the `#define` and updated the two comments that asserted the OLD value as current/live state so they don't mislead a future reader: `adc_capture.h`'s `ADC_SAMPLES_PER_TICK` comment (now correctly says 64000/16000=4, with the 80000/16000=5 and 48000/16000=3 history kept as dated context rather than deleted) and `config.h`'s `SAMPLE_RATE_HZ` comment, which had explicitly stated "Rate is back to 80000... live again as written, not just historical" - appended a dated update rather than rewriting that sentence, so the historical narrative around the reverted 48000 experiment stays intact. Left every OTHER historical mention of 80000 alone (the dated 2026-09-02 GPIO13 coupling investigation, the chirp-factor=7 clock-mismatch writeup in ssb_mic_test_commands.md, etc.) - those are measurements taken AT 80000 and describing what was true then, not claims about today's config, so rewriting them would corrupt the historical record rather than correct it.

No other code depends on the literal value 80000 (no `static_assert`/`#error` keyed to it) - `ADC_SAMPLES_PER_TICK = ADC_CONT_SAMPLE_FREQ_HZ / SAMPLE_RATE_HZ` stays a clean integer division (64000/16000=4) and the fractional resampler already measures its own true_ratio at runtime regardless, so this is a low-risk, mechanical sync rather than a behavior change in itself - the actual 64000 behavior has already been the user's live-tested configuration since the Fs-hunt work.

## 2026-09-24: audio_fx compressor's makeup gain redesigned to track the signal's actual peak, not assume it hits 0dBFS

User: "adc fx compressor should rescale data so peak value stays the same." Root issue: `compute_compressor_makeup_gain()` (now removed) computed a single FIXED makeup gain at init/reconfigure time, derived from a dB-domain formula that explicitly assumed the audio peaks at exactly 0dBFS (`envelope=1.0`) - its own header comment said as much ("for a signal peaking at full scale..."). That assumption is fine for a hot source but wrong for this project's current mic input specifically - a poor electret capsule producing envelope readings well under 1.0 (0.28-0.45 range seen in recent bench snapshots) - so the fixed makeup gain was calibrated to restore a peak the signal never actually reaches, either over- or under-compensating the REAL peak depending on threshold/ratio.

**Fix**: `compressor_t` gained a second, independent envelope follower (`peak_env`) alongside the existing fast gain-control envelope (`env`) - same fast attack (reuses `attack_coef`, since it must catch every peak the compressor itself reacts to), but a much slower release (`COMP_PEAK_HOLD_RELEASE_MS=2000`, a new fixed constant, not yet an exposed tunable) so it tracks the signal's sustained peak level across a couple of seconds of speech rather than decaying syllable-to-syllable - avoiding a new pumping artifact the fast envelope's own release time would otherwise introduce into the makeup gain itself. `makeup_gain` is now recomputed every sample directly from `peak_env`'s own gain-reduction curve (`makeup_gain = peak_env / peak_compressed`, the exact inverse of what the compressor would do to that observed peak) instead of a fixed formula - so whatever the signal's real peak actually is, that peak (not an assumed 0dBFS one) is what comes back out after compression. Below threshold (peak never reached it), `makeup_gain=1.0` - no artificial boost, consistent with this project's existing ALC design principle of only ever restoring/attenuating what the signal itself justifies, never manufacturing gain.

Reset-on-re-enable (`ssb_dsp_set_compressor_enabled()`) extended to also reset `peak_env`/`makeup_gain`, same reasoning as the existing `env` reset. `ssb_dsp_set_compressor()` (live threshold/ratio changes) no longer needs to recompute makeup_gain itself - the very next sample already reflects the change since makeup_gain is derived fresh every sample now. Canary (`ssb_dsp_iir_canary_t::compressor_env_finite`) extended to also cover `peak_env`/`makeup_gain`'s finiteness - same single bool, since all three represent one health question. **Not yet bench-validated** - the 2-second peak-hold release time is a first-cut choice (fast enough to adapt to e.g. moving closer to the mic within a couple seconds, slow enough not to visibly pump on individual syllables), worth listening for either too-slow adaptation or audible makeup-gain pumping and adjusting if so.

## 2026-09-24, later: added a second compressor mode - plain peak-normalize (linear scale, no squashing curve) as an alternative to the ratio compressor

Follow-up to the same-day compressor makeup-gain fix above. In discussing why makeup gain approaches (but doesn't equal) `comp_ratio` for a given peak, user's own framing was: assume voice's naturally high crest factor means real peaks get hit often enough to track directly, so a simple linear scale-to-target (no ratio-based squashing curve at all) avoids extra distortion risk from the squashing curve itself, with a side benefit of injecting less gain into the noise floor during loud passages than the ratio compressor's makeup gain does for the same target peak. Asked to add this as a selectable option alongside the existing ratio compressor, not a replacement for it.

**Implemented as a new `ssb_dsp_comp_mode_t` enum** (`ssb_dsp.h`): `SSB_DSP_COMP_MODE_RATIO` (value 0, the original behavior - default, so every existing preset/config that never sets this field keeps behaving exactly as before) and `SSB_DSP_COMP_MODE_PEAK_NORMALIZE` (new). Both modes share the exact same underlying `peak_env` slow peak-tracker (added earlier today for the makeup-gain fix) and the same `threshold` field (below it, both modes leave the signal untouched - no boost, matching this project's "never manufacture gain the signal doesn't justify" convention) - they just do different things with it once it's above threshold:

- **Ratio** (unchanged): squashes `env` toward threshold by the configured ratio, then restores `peak_env`'s own reduction via `makeup_gain = peak_env/peak_compressed` - actively changes crest factor, this is the whole point of a ratio compressor.
- **Peak-normalize** (new): NO squashing curve - `gain` stays fixed at 1.0, and `makeup_gain = COMP_PEAK_NORMALIZE_TARGET / peak_env` (a new fixed constant, 0.90, with a `COMP_PEAK_NORMALIZE_MAX_GAIN=10.0` safety ceiling for when `peak_env` decays toward zero during silence) is applied to the WHOLE signal uniformly. Crest factor is completely unchanged by this mode - quiet and loud content get multiplied by exactly the same factor at any instant.

**Verified the "less noise floor gain" claim numerically**, not just accepted on faith: for the same target peak level, Ratio mode's makeup gain is always LARGER than Peak-normalize's, because Ratio has to compensate for having squashed the peak down first (`makeup = peak/peak_compressed` where `peak_compressed < peak`), while Peak-normalize scales the untouched peak directly to the target (`makeup = target/peak`, no squashing to compensate for). E.g. threshold=0.3/ratio=3.5/peak=0.7: Ratio's makeup is 1.69x vs Peak-normalize's ~1.29x (0.90/0.7) for a comparable target - confirms the user's intuition was numerically correct, not just plausible-sounding.

Wired in as: a new `'W'` serial command (independent of `'c'`, which still just gates whether the compressor runs at all - same "separate enable vs mode-select" pattern `adc_lpf_mode`'s `'f'` already uses relative to whatever gates the ADC path as a whole), a new `PersistentSettings::comp_mode` field (appended at the end, zero-fills to `SSB_DSP_COMP_MODE_RATIO`=0 for every existing preset - zero behavior change), and the usual `'P'`/preset-apply plumbing. No reset-on-mode-switch needed (unlike enable/disable) - both `env` and `peak_env` keep running continuously regardless of mode, so there's no frozen state to clean up, just a different formula reading state that's already live.

**Honest caveat carried over from the underlying peak-tracker**: like any feed-forward (non-lookahead) gain-control scheme, both modes have the same finite-attack-time transient overshoot risk (the ~3ms attack time before `peak_env` catches up to a sudden onset) - this isn't a new risk Peak-normalize introduces, it's the same tradeoff Ratio mode's own `env` follower already has, just inherited by `peak_env` instead. Neither compressor mode is bench-validated yet.

## 2026-09-24, later still: added a third compressor mode - Limit-only (attenuate-only, no makeup gain at all), fixing a real-hardware symptom of the original design

Follow-up to both compressor-mode entries above. User clarified they were describing the ORIGINAL (pre-today) compressor's real-hardware behavior, not either of today's new modes: turning `'c'` on raised output by ~8dB (an instantaneous reading went from 0.01 to 0.024), including a corresponding rise in peak output, which forced turning the master gain down to avoid hitting the output end-stop. User's proposal: keep the exact same threshold/ratio squashing curve, but make the "global gain change within `c`" a fixed, simple, non-tracking one - specifically, remove it entirely - so 0.024 comes back down to 0.01 and the peak drops by the same factor.

**This is a genuinely different case from what "peak stays the same" meant in today's earlier fix.** The Ratio-mode fix earlier today made `makeup_gain` track `peak_env` so the compressed peak gets RESTORED to the signal's own real peak (by design, `compressed(peak) * makeup_gain(peak) = peak`, exactly). That's correct behavior for a loudness-raising compressor, but it's also exactly why the original (and today's Ratio-mode successor) always applies *some* makeup gain above 1.0 once anything crosses threshold - and the user's hardware observation is exactly what that gain increase looks like at the output. What the user is now asking for is a mode that does NOT restore/boost at all - a plain limiter.

**Implemented as `SSB_DSP_COMP_MODE_LIMIT_ONLY`** (`ssb_dsp.h`'s `ssb_dsp_comp_mode_t`, value 2 - appended after `RATIO`=0 and `PEAK_NORMALIZE`=1, so existing presets/configs are unaffected). Uses the identical squashing-curve math as Ratio mode (`gain = (threshold + (env-threshold)*inv_ratio) / env` once `env > threshold`, else `gain=1.0`), but `makeup_gain` is permanently fixed at `1.0` - never computed from `env`, `peak_env`, or anything else, in this mode. Below threshold: bit-identical passthrough, zero amplification, ever. Above threshold: only ever attenuates (`gain <= 1`), never restores. This directly matches the user's exact description ("no tracking, simple global level") and structurally cannot reproduce the end-stop symptom, because there is no makeup-gain step left to raise the peak (or the noise floor) back up.

**Confirmed the exact numeric mapping to the user's observation**: the original fixed makeup-gain factor for the user's threshold=0.3/ratio=3.5 settings is ≈2.363x (Python-verified via the now-removed `compute_compressor_makeup_gain()` formula - not the ≈1.828 figure mistakenly logged in `null_bias_investigation.md`'s first compressor entry, which was a mental-math slip; see that file's discrepancy note). 0.024/0.01 ≈ 2.4, which is a reasonably close match to 2.363x given the reading was an instantaneous `'V'` snapshot rather than a careful peak measurement - consistent with "the whole ~8dB rise is that fixed makeup-gain factor showing up in the output, on both quiet content and the peak alike." Removing the makeup-gain step entirely (this new mode) divides the compressed output - and specifically the compressed peak - by exactly that same factor, algebraically undoing it: `new_peak_output = old_peak_output / 2.363 = (peak_compressed * 2.363) / 2.363 = peak_compressed`, i.e. the peak lands back where the raw squashing curve alone would put it, with no restoration step riding on top.

Wired in via the same plumbing as the other two modes: `'W'` now cycles 3 values (modulo widened `%2`->`%3`, `k_comp_mode_enum_name[]` widened to 3 entries), the `'P'` preset-dump doc comment and `ssb_mic_test_commands.md`'s `'W'` row both updated to describe all three modes. **Tradeoff, stated plainly**: Limit-only does not raise RMS or reduce crest factor at all - it's a peak limiter, not a loudness-raising compressor, by design. If the user later wants both "no end-stop surprises" AND some RMS lift, that would need a different, smaller, hand-picked fixed makeup gain (not full peak-restoration) - not attempted here, since it wasn't what was asked for. Not bench-validated yet, like the other two modes.

## 2026-09-25: compressor scrapped and rebuilt - Mic Gain stage added, switchable modes replaced by a voice-calibrated Compression Level

Follow-up to the whole 2026-09-24 compressor-mode thread above, and directly triggered by the finding in that thread's final entries: the user's own real mic-to-ADC recording (`G4AHN.wav`, uploaded this session) showed peak=0.4178, rms=0.0357, crest=21.37dB - only 0.022% of samples ever exceeded the project's threshold (0.1-0.3 depending which config was checked). Every bit of curve-shape/threshold/ratio tuning done across the whole 2026-09-24 thread (three switchable modes, multiple threshold sweeps, log vs linear vs piecewise curve comparisons) was calibrated against a signal level the real hardware never actually produces. Separately, researching FLEX-6600/SmartSDR's gain-staging architecture (Mic Gain -> TX EQ -> Speech Processor -> RF/ALC, Mic Gain manual and NOT automatic) confirmed the real gap: `master_gain_db` is applied AFTER compressor/EQ in this codebase (confirmed by reading `ssb_dsp_process_sample()`), so it was always functioning as an RF-power control, not a mic-input control - there was no lever at all for the level going INTO the compressor.

User's decisive instruction: "lets scrap the recent switchable comp changes and replace the original with a 3 segment 'log curve' together with a new micr gain input. There also should be an input to set compression level, nice if that was dB's of power gain modelled on my voice profile when presented with a normalised input. A 0dB setting would make the comp a simple audio side limiter."

**Scrapped entirely**: `ssb_dsp_comp_mode_t` (RATIO/PEAK_NORMALIZE/LIMIT_ONLY) and everything built around it across the 2026-09-24 entries above - `ssb_dsp_set_compressor_mode()`/`get_compressor_mode()`, the `peak_env`/`peak_release_coef` slow peak-tracker and its `COMP_PEAK_HOLD_RELEASE_MS`/`COMP_PEAK_NORMALIZE_TARGET`/`COMP_PEAK_NORMALIZE_MAX_GAIN` constants, the `'W'` mode-cycling command, `k_comp_mode_enum_name[]`, `PersistentSettings::comp_mode`, and the free-running `comp_threshold`/`comp_ratio` config fields (whose actual deployed value, `comp_threshold=0.1f` in `ssb_mic_test.ino`, had quietly drifted from the `ssb_dsp.h` doc comment's illustrative `0.3f` example - itself never a measured/derived number, see this file's earlier "no principled derivation" finding).

**Added: Mic Gain** (`ssb_dsp_set_mic_gain_db()`/`get_mic_gain_db()`, new `'U'`/`'Y'` keys, `MIC_GAIN_STEP_DB=1.0` in `config.h`) - a manual dB trim applied at the very START of `ssb_dsp_process_sample()`, before compressor/EQ, mirroring `master_gain_db`/`master_gain_linear`'s exact pattern (human-friendly dB setter, precomputed linear multiply in the hot path) but at the opposite end of the chain. Deliberately manual, same reasoning as master gain: this module can't know the mic capsule's sensitivity or the operator's mic-to-mouth distance, so it can't self-normalize correctly. `PersistentSettings::mic_gain_db` added (zero-fills to 0.0dB/unity for every existing preset - no behavior change until deliberately dialed in).

**Replaced the compressor algorithm with ONE fixed-shape, 3-segment piecewise-linear approximation of a dB-domain (log) soft-knee curve**, per the user's explicit request. Fixed, shared threshold `SSB_DSP_COMP_THRESHOLD=0.30f` (an internal `ssb_dsp.c` constant now, deliberately NOT independently configurable - every calibrated level below was measured against this exact value, and letting it drift would silently invalidate both the achieved-dB numbers and the safety-margin analysis) and four fixed breakpoint x-locations (`0.30, 0.55, 0.80, 1.05` - the last one deliberately above 1.0 so mild over-drive still gets a continued curve instead of a hard corner at full scale).

**Added: Compression Level** (`ssb_dsp_set_compressor_level()`/`get_compressor_level()`, new `'X'`/`'q'` keys, integer `SSB_DSP_COMP_LEVEL_MIN=0`..`SSB_DSP_COMP_LEVEL_MAX=6`, `PersistentSettings::comp_level_db` added) - replaces free-running threshold/ratio with a small table of discrete, pre-calibrated levels. **This is a genuine behavior-change default**: unlike every other trailing `PersistentSettings` field added this project's life, zero-filling to level 0 does NOT reproduce the old compressor's behavior (which applied automatic makeup gain by default) - it deliberately reproduces the NEW "plain limiter, no boost" behavior instead, on the reasoning that shipping a silent, unvalidated boost by default was itself part of the original problem.

**Calibration methodology** (offline, Python, `/scratchpad/redesign/calibrate.py` this session, working from `G4AHN.wav`): normalized the real recording to a peak of 0.85 (the assumed Mic Gain target - "presented with a normalised input" per the user's request), ran the SAME envelope-follower math `compressor_process()` uses (one-pole, `attack_ms=3.0`/`release_ms=120.0`, this project's existing defaults) over the whole 8.64s recording, then for each target dB (1-6) bisected the curve's ratio so that applying `{threshold=0.30, ratio, makeup=peak_restoring_value_at_that_ratio}` to the real recording produces exactly that much measured RMS gain. **Level 0 is deliberately NOT part of that same search family** - bisecting for 0dB naturally converges to `ratio->1` (no compression at all, verified numerically), which is a bypass, not a limiter. Level 0 instead fixes `makeup=1.0` exactly with a separately-chosen, moderately firm `ratio=5.0`, and the resulting dB is MEASURED, not targeted - it came out to a slight (-0.44dB) RMS reduction, which is the textbook-correct behavior for a limiter (it only ever takes away, never adds).

Resulting table (ratio, makeup, and the ACHIEVED dB measured directly against the real recording using this exact envelope-follower+curve+fixed-makeup algorithm, cross-checked in a standalone C simulation reading `G4AHN.wav` directly - not just the Python model):

| level | ratio | makeup (dB) | measured gain | peak with clamp | peak WITHOUT clamp | safety-clamp engagement |
|---|---|---|---|---|---|---|
| 0 | 5.0000 | 1.0000 (+0.00dB) | -0.44dB | 0.6889 | 0.6889 | 0% |
| 1 | 1.1355 | 1.1331 (+1.08dB) | +1.00dB | 0.9145 | 0.9145 | 0% |
| 2 | 1.3123 | 1.2828 (+2.16dB) | +2.00dB | 0.9921 | 0.9921 | 0% |
| 3 | 1.5527 | 1.4509 (+3.23dB) | +3.00dB | 1.0000 | 1.0877 | 0.0036% (5 samples) |
| 4 | 1.8990 | 1.6399 (+4.29dB) | +3.99dB | 1.0000 | 1.2003 | 0.0101% (14 samples) |
| 5 | 2.4412 | 1.8522 (+5.35dB) | +4.98dB | 1.0000 | 1.3253 | 0.0311% (43 samples) |
| 6 | 3.4117 | 2.0907 (+6.40dB) | +5.96dB | 1.0000 | 1.4641 | 0.0767% (106 samples) |

Levels 7-8 were also computed during calibration (ratio 5.65/16.36, reaching +7/+8dB) but deliberately NOT shipped - this project's own listening-test finding from earlier this session (log-compression thread) found 6dB close to a sweet spot and 10dB already audibly hurting intelligibility, so the exposed range stops at a level already known to be reasonable rather than inviting one already known to sound worse.

**Important finding from the C cross-check that the Python-only calibration had NOT surfaced**: applying each level's fixed `{ratio, makeup}` to the real recording produces genuine full-scale overshoot BEFORE any output clamp - up to 1.4641 (+3.3dB over full scale) at level 6 - even though a "worst-case steady-state envelope=1.0" analysis of the same curve+makeup predicts a safe 0.89. The mechanism: `compressor_process()`'s gain decision is based on the SMOOTHED envelope (`env`), but that gain multiplies the INSTANTANEOUS raw sample - a fast attack transient can momentarily exceed `env` before the (3ms-attack) follower catches up, a standard feed-forward-compressor limitation (a look-ahead design avoids it at the cost of latency, not attempted here). **Added a hard output safety clamp** (±1.0) in `ssb_dsp_process_sample()`, right after the compressor+EQ block, specifically because of this measurement - not a theoretical nicety. The clamp engagement rate is low (0.08% of samples at the most aggressive shipped level) but genuinely nonzero, confirming it's a real, if rare, occurrence on real speech, not just a synthetic edge case.

**Also cross-validated the C implementation against the Python calibration bit-for-bit**: wrote the exact `piecewise3_interp()`/breakpoint-derivation code standalone in C, confirmed its breakpoint y-values match Python's to 5+ significant figures, and confirmed `curve(0.85) * makeup == 0.85` exactly for the peak-restoring levels - the C firmware code will reproduce the calibration's numeric behavior, not just something shaped like it.

Updated `ssb_mic_test_commands.md`'s "Audio processing" section and testing-defaults list; `serial_commands.cpp`'s `'P'` preset-dump (mic_gain_db/comp_level_db now the two trailing fields, comp_mode gone) and preset-load block. None of the 10 `settingsPresets[]` entries needed editing - all rely on positional-initializer zero-fill for trailing fields, same as every previous trailing-field addition.

**Not bench-validated** - this entire redesign (Mic Gain, the new curve, the compression-level table, the safety clamp) is built and numerically verified against ONE offline recording and a from-scratch C reimplementation, not yet tried on real hardware. The aliasing risk of an instantaneous nonlinearity at this project's baseband sample rate (flagged multiple times earlier this session, in the log/linear/piecewise curve-comparison thread) remains an open, unmitigated caveat - nothing in today's redesign addresses it either way.

## 2026-09-25, later: presets 4/5/8/9 in `settingsPresets[]` synced from the user's live-tuned `settings.h`

User uploaded their own working copy of `settings.h` (built on top of the compressor-redesign delivery above - the `PersistentSettings` struct itself, `comp_mode`'s removal, and `mic_gain_db`/`comp_level_db`'s addition all matched exactly) and asked to merge their latest presets in. Diffed the two files directly rather than trusting a description of the changes: the only differences were presets 4, 5, 8, and 9's values and names, replacing bench/diagnostic placeholders with real mic-tuned presets -

- Preset 4: `"AM-ButwGd"` (AMTEST, diagnostic) -> `"ReSync Micr"` (real mic preset, `ADC_LPF_MODE_CHEBYSHEV`, master_gain=33.3dB)
- Preset 5: `"V4 Microphone tuned"` -> `"V4 Microphone tunedD7"` (relative_delay 3.48->2.03 samples, master_gain 21.3->19.3dB, `envelope_interp_enable` true->false, gdeq variant DEFAULT->CANDIDATE_B)
- Preset 8: `"FM Env 2.2"` (FMTEST, diagnostic) -> `"Micr WhiteNoise"` (real mic preset, `ADC_LPF_MODE_CHEBYSHEV8`, master_gain=29.3dB, ALC+Soft-Limit both on)
- Preset 9: `"AM Env = 1.6 - 2.9"` (AMTEST, diagnostic) -> `"Micr Voice"` (real mic preset, `ADC_LPF_MODE_CHEBYSHEV8`, master_gain=6.3dB, ALC+Soft-Limit both on)

None of the four explicitly set `mic_gain_db`/`comp_level_db` (today's two newest trailing fields) - all rely on the same positional-initializer zero-fill every trailing field in this struct has always used, so they load with mic gain at unity and the compressor at level 0 (limiter only) until dialed in live, same as every other preset. Verified the merged file's presets are well-formed against the `PersistentSettings` layout with a standalone syntax check (stubbed enum headers, `-fsyntax-only`) before applying - no field-count mismatches, only the same "missing trailing initializer" warnings every other preset in this file already produces by design. Applied directly (diffed clean against the uploaded file afterward); no other file needed touching.

## 2026-09-25, later still: first real-hardware data on the redesigned compressor - CPU cost confirmed negligible, envelope rise roughly tracks the level-6 calibration, mic_gain=33dB flagged for a peak check

User ran live `'V'` snapshots on real hardware with `'c'` toggled off then on (compression level 6, per the `'P'` dump captured during the on-test: `mic_gain_db=33.0`, `comp_level_db=6`).

**CPU cost - the first genuine on-hardware measurement, not simulation.** `[timing] dsp breakdown: audio_fx=3 fir=12 atan2=9 sqrt=9` read IDENTICAL with the compressor off and on - `audio_fx`'s 3us high-water mark didn't move at all. `max_busy_us` ticked 42->43, but `adc`/`write` moved by the same 1us too (ordinary tick jitter, unrelated to the compressor). This directly answers the "what per-sample CPU time are we talking in us" question raised earlier this session, which until now only had a simulated/estimated answer (the ~2x-plain-linear-curve figure from the x86 benchmark) - confirms the redesigned 3-segment piecewise curve really is negligible against this project's DSP budget on the actual target hardware.

**Envelope rise roughly matches the level-6 calibration.** Averaging the three `c`-off `envelope=` readings (0.203, 0.218, 0.226 -> mean 0.216) against the two `c`-on readings (0.429, 0.505 -> mean 0.467) gives +6.71dB (`20*log10(0.467/0.216)`), close to the +5.96dB measured for level 6 against `G4AHN.wav` in the redesign's own C cross-check. Held loosely - these are single instantaneous snapshots, not an RMS average, and speech is bursty enough that a snapshot can land on a quiet or loud instant essentially at random. A real confirmation would need something closer to an RMS-over-time comparison, not attempted here.

**Flagged, not fixed: `mic_gain_db=33.0` is far higher than the calibration assumed.** The redesign's whole compression-level table was calibrated assuming mic gain normalizes real peaks to ~0.85 - for `G4AHN.wav` that needed +6.17dB (raw peak 0.4178 -> 0.85). 33dB is ~22x more linear gain than that. Not necessarily wrong (this board's analog front-end could genuinely be quieter than whatever chain recorded `G4AHN.wav`), but also exactly the number you'd get by carrying over the OLD `master_gain_db` value from the "ReSync Micr" preset (33.3dB) into the new `mic_gain_db` slot rather than re-deriving it fresh - the two aren't interchangeable now that there's a separate stage. If real peaks are landing well past 0.85 at this setting, the compression-level labels (1-6dB) won't mean quite what they say, since they're calibrated against that specific input level. Not acted on here - the concrete next step is watching envelope on a normal loud spoken syllable with `'c'` off to see where it's actually peaking, same "set mic gain first, by ear/scope" procedure the redesign's own doc comment describes.

## 2026-09-25, later still: mic_gain=33dB flag retracted - it's preset 8 (low-volume speaker/noise test), not preset 9 (voice); preset 9's own 6.3dB tuning independently corroborates the G4AHN.wav calibration

Direct follow-up to the previous entry's "mic_gain=33dB flagged for a peak check" caveat. User clarified: the `'V'` session that produced `mic_gain_db=33.0` was preset 8 ("Micr WhiteNoise"), used for a noise test with a speaker at deliberately low volume - a genuinely much quieter source than normal speech, for which 33dB of mic gain is plausible rather than an error. Preset 9 ("Micr Voice") is the actual speech preset, and its own bench-tuned gain is 6.3dB.

**Worth recording as an independent cross-check**: preset 9's 6.3dB (arrived at by the user's own bench tuning, with no reference to this session's calibration work) is remarkably close to the +6.17dB this session calculated from `G4AHN.wav` as the mic gain needed to bring that recording's raw peak (0.4178) up to the ~0.85 target the whole compression-level table is calibrated against. Two independently-derived numbers landing within 0.15dB of each other is a meaningfully stronger validation of the calibration's realism than either one alone.

**However**: preset 9 currently has `compressor_enable=false` and its 6.3dB sits in the OLD `master_gain_db` field, not the new `mic_gain_db` - it has not been migrated to the new Mic Gain / Compression Level architecture at all. With the compressor off, this makes no functional difference right now (no nonlinearity sits between mic_gain and master_gain's application points when the compressor doesn't run, so a linear gain is a linear gain regardless of which field holds it) - but it also means preset 9 isn't getting any of the redesign's RMS-raising benefit yet. Asked the user whether to migrate it (move 6.3dB to `mic_gain_db`, enable the compressor, pick a level) - **answer: not yet**. Left exactly as-is; this is a deliberate, informed deferral, not an oversight.

## 2026-09-25, later still: Compression Level extended from 0-6 to 0-10, and preset 9 migrated after all - but "level 10" is NOT +10dB, it's the honest maximum this threshold/recording can deliver (~+8.35dB)

User's request, verbatim: "values for preset 8 should be old gain 0, new micr 7dB and compdB 6. For preset 9 old gain 0, new mic 14 (louder speaker) and compdB 6. Also I think compdB could be extended to 10 max." Three things happened here, in the order worked:

**1. The compdB range extension surfaced a real physical ceiling, not a bug.** Re-running `calibrate.py` with its target loop extended from `range(1,9)` to `range(1,11)` (i.e. asking the bisection search for literal +9dB and +10dB targets) produced two IDENTICAL entries: both converged to `ratio=50.0` (the search's `ratio_hi` bound) and both measured only +8.36dB, not their nominal +9/+10dB labels. This is not a bisection-precision issue - it's a genuine asymptote. As `ratio -> infinity`, the 3-segment curve degenerates to a hard limiter fixed at `SSB_DSP_COMP_THRESHOLD` (0.30), so I computed that exact degenerate case directly (not via bisection: `makeup = cal_peak/threshold = 0.85/0.30`, applied through the real envelope-follower simulation) to find the TRUE ceiling rather than trusting a search bound: **+8.53dB measured RMS gain, and no ratio however large can exceed it, on this recording, at this threshold.** This closely matches an earlier-session finding (max achievable RMS gain via hard-clip at threshold=0.30 was computed as 8.43dB via a simpler synthetic-peak method) - two independent methods landing within 0.1dB of each other.

This also directly contradicts what `ssb_dsp.h`'s doc comment said before today: that levels above 6 were "computed but deliberately not shipped" because a listening test found +10dB (under the OLD, pre-this-table scheme) already hurt intelligibility, and 6dB was the sweet spot. Worth being explicit that the user's request to extend to 10 was made without that context surfacing again in this session until the recalibration ran - I'm recording it here rather than silently overriding a previously-documented decision.

**Resolution taken** (not asked back to the user first, since "compdB could be extended to 10 max" reads as "I want more headroom on the dial," not "the number must be literally accurate to the decibel," and the user is the one who owns this tradeoff): rather than ship two duplicate, mislabeled level-9/10 entries, I re-ran the bisection with explicitly chosen, comfortably-sub-ceiling targets for levels 7-10 (+7.0, +7.6, +8.1, +8.35dB) so each level is genuinely distinct and the search converges normally rather than saturating its bound:

```
level=7  ratio=5.6513   makeup=2.358638 (+7.45dB)  measured_gain=+7.00dB  peak=1.6184
level=8  ratio=9.3100   makeup=2.534993 (+8.08dB)  measured_gain=+7.60dB  peak=1.7191
level=9  ratio=20.1842  makeup=2.691641 (+8.60dB)  measured_gain=+8.10dB  peak=1.8080
level=10 ratio=48.4601  makeup=2.773442 (+8.86dB)  measured_gain=+8.35dB  peak=1.8543
```

Levels 7-9 still follow the same "target = label" convention every level below them uses. **Level 10 is the one exception: it is labelled "10" as a dial position, but delivers +8.35dB, not +10dB** - the last three steps (8/9/10) compress together more tightly than the +1dB/step pattern below them because they're genuinely approaching the ~8.5dB hard-clip asymptote, not because of a shortcut. This is now documented prominently in `ssb_dsp.h`'s `SSB_DSP_COMP_LEVEL_MAX` doc comment, in `ssb_dsp.c`'s table comment, and in `ssb_mic_test_commands.md`, specifically so nobody (including a future me) mistakes "level 10" for "+10dB" again.

Independently cross-checked in C (extended `/tmp/synstub/test_full.c`'s 7-entry table to all 11 levels, reading `G4AHN.wav` directly rather than trusting the Python numbers): levels 0-6 reproduced exactly as before (unchanged), and levels 7-10 landed close to their Python targets but with a small, growing float32-vs-float64 precision gap at the higher ratios - C measured +6.91/+7.47/+7.94/+8.16dB against Python's +7.00/+7.60/+8.10/+8.35dB targets (deltas of -0.09, -0.13, -0.16, -0.19dB, growing with ratio). This is larger than the ~0.04dB gap already seen at level 6 and is worth watching if levels 9-10 ever get bench-measured and come in noticeably under the nominal Python figures - it's consistent with compounding float32 rounding in the envelope follower/curve math at high ratios, not necessarily a real firmware bug, but not yet explained down to the last 0.1dB either.

`SSB_DSP_COMP_LEVEL_MAX` changed from 6 to 10 in `ssb_dsp.h`; `k_comp_level_table[]` in `ssb_dsp.c` extended with the four new entries (table auto-sizes off `SSB_DSP_COMP_LEVEL_MAX - SSB_DSP_COMP_LEVEL_MIN + 1`, so no separate size constant to update). No changes needed to `serial_commands.cpp`'s clamp/format logic (already reads the macros and uses `%d`), beyond one now-stale "0-6" in a comment, corrected to "0-10".

**2. Preset 8 ("Micr WhiteNoise") migrated to the new architecture, per the user's explicit numbers**: `compressor_enable` `false -> true`, `master_gain_db` `29.3f -> 0.0f`, new trailing `mic_gain_db=7.0f`, `comp_level_db=6`. This is the noise-test preset (speaker at low volume) - no independent calibration cross-check applies to 7.0dB the way preset 9's number gets one below; it's simply the user's stated setting, taken as given.

**3. Preset 9 ("Micr Voice") migrated too - reversing the "not yet" from two entries above, on new instructions.** Same architecture change: `compressor_enable` `false -> true`, `master_gain_db` `6.3f -> 0.0f`, new trailing `mic_gain_db=14.0f`, `comp_level_db=6`. Important: **14dB is NOT a refinement of the 6.3dB/6.17dB match logged above** - the user described this as "louder speaker," i.e. a different physical setup/source level than the voice-at-normal-distance tuning that produced 6.3dB. The earlier cross-validation (6.3dB bench value vs. +6.17dB calibration figure) remains true and worth keeping on record, but it doesn't say anything about whether 14dB is correct for this new, louder-source configuration - that would need its own bench check, not an inference from the old number.

**Not yet done, flagged for a future session**: `compressor_enable=true` for both presets was an INFERENCE on my part (comp_level_db has no effect with the compressor off), not something the user stated explicitly in this message - stated here plainly in case that inference turns out to be wrong. Also, none of levels 7-10, and neither preset 8's new 7dB/comp6 nor preset 9's new 14dB/comp6, have been bench-validated on real hardware yet - everything above is numerically verified against the offline `G4AHN.wav` recording (now cross-checked in both Python and standalone C) but has not been listened to or scoped on the actual board.

## 2026-09-25, later still: the 2026-09-23 "ADC filter CPU headroom" worry appears resolved - the `c`-off/`c`-on capture WAS running Chebyshev8, and it wasn't tight at all

User clarified after the fact: the real-hardware `c`-off/`c`-on timing capture analyzed two entries above (`[timing] dsp breakdown: audio_fx=3 fir=12 atan2=9 sqrt=9`, `max_busy_us` ticking 42->43) was taken with `ADC_LPF_MODE_CHEBYSHEV8` selected - consistent with preset 8's own settings.h entry (`"Micr WhiteNoise"` has run `ADC_LPF_MODE_CHEBYSHEV8` since the 2026-09-25 preset-merge earlier this session, and per the 2026-09-23 entry's own preset history, since before that).

This directly answers the open item the 2026-09-23 entry ("ADC anti-alias filter widened to 6th/8th order") explicitly left for later: *"This has NOT been bench-verified with the new filters actually running - the very next step on real hardware should be cycling `'f'` through each new mode and reading `'V'`'s `max_busy_us`/`adc=` fields."* That step has now effectively happened, via this capture, and the result is considerably better than the 2026-09-23 entry's own extrapolation feared: that entry's "fresh on-demand `'V'` read" measured `max_busy_us=60` against the 62.5us tick budget (~2us headroom, described as "a much tighter margin" than historical readings) BEFORE 8th order was confirmed running - the 2026-09-25 capture, WITH 8th order actually running, instead reads `max_busy_us=42-43` - comfortably under budget, ~19-20us of headroom, in the same ballpark as the pre-resampler/pre-ALC/pre-Soft-Limit 2026-09-11 baseline (`max_busy_us=46`) the 2026-09-23 entry used as its point of comparison.

**Flagging the gap rather than quietly smoothing over it**: 60us and 42-43us are both real numbers on record for what should be a similar configuration, and I don't have a confirmed explanation for the ~17-18us difference. The 2026-09-23 entry itself attributed most of ITS reading's `adc=28us` bucket to "the fractional resampler's calibration/interpolation math," not the LPF - one plausible, UNCONFIRMED explanation is that the 2026-09-23 reading caught the resampler mid-calibration-window (a state that bucket's own comment says recomputes only every `ADC_RATIO_CALIB_WINDOW_CALLS` ~4 seconds), while the 2026-09-25 capture happened to land in a steady-state window instead - `adc_capture_ratio_is_calibrated()` exists for exactly this kind of check but wasn't read in either case as far as this log shows. I don't have the 2026-09-25 capture's own `adc=`/`write=` per-bucket breakdown (only the `dsp breakdown` line and the `max_busy_us` headline were logged at the time), so this can't be nailed down further from what's on record - if the full `'V'` line from that capture still exists (serial log, terminal scrollback), it would settle this outright. Net read for now: the headline CPU-cost conclusion is more reassuring than 2026-09-23 feared, but the discrepancy between the two `max_busy_us` readings is itself a small open question, not yet explained down to the last microsecond.

## 2026-09-25, later still: chasing low-level mic crackle - ENV_FLOOR_STEP finer-grained to 0.001 (from 0.02) at user's request, plus a real preset-dump precision bug found and fixed along the way; `envelope_floor` is NOT a squelch, flagged clearly

**Context**: user reports the mic's own analog noise floor is now fixed (a prior hardware noise issue is "below the ADC threshold (just)" - i.e. fixed, but only just). Remaining issue: "spiky/crackly output at low micr input," hypothesized as "not enough dynamic range at low level," floating the idea of "a squelch to just zero out." Reported trying `envelope_floor` (`'x'`/`'z'`) but finding its only non-zero step, 0.02, is "lots of dBs too high," and asked about switching to 0.001-sized steps ("1 PWM res").

**Important distinction surfaced and flagged back to the user, not just implemented silently**: `envelope_floor` is NOT a squelch. Per its own header (`envelope_floor.h`) it does the opposite of gating-to-silence - `envelope_floor_apply()` is `floor + (1-floor)*raw_envelope`, a smooth affine remap that RAISES the bottom of the envelope range, never forces it to zero. It exists to keep the envelope out of `envelope_predistort.h`'s steepest, least-characterized LUT region (36-55% duty, 60-137dB/V measured) near true nulls - a different problem than "silence low-level noise." Two earlier, more squelch-like designs (freezing phase near null; a hard `min(envelope,floor)` clamp) were both tried and REVERTED after real-hardware testing showed real distortion (a phase-debt snap-back, and a derivative-discontinuity-driven rise in nearly every intermod product respectively) - see `envelope_floor.cpp`'s own header comment, already on file from before this session. A genuine squelch (force-to-zero below a threshold) is architecturally a THIRD, distinct thing, not built here, and would likely need the same "no hard threshold/kink" lesson applied (soft-knee gate + attack/release smoothing, not a binary cutoff) to avoid repeating either reverted design's failure mode - not attempted this session, flagged as a real option if wanted.

**The math behind "0.02 is lots of dBs too high," confirming the user's own read was right**: `floor` is literally the minimum output this remap can ever produce (`apply(0)=floor`), so it's directly an achievable-null-depth ceiling in dB: `20*log10(floor)`. 0.02 -> -34dBc - the ONLY non-zero value one `'x'` press could reach before this change - against a documented true-null baseline of ~-58dBc (`envelope_predistort.h`, deepest measured nulls). That's giving up ~24dB of null depth in a single keypress, with no way to dial in anything between "off" and "-34dBc." 0.001 -> -60dB, i.e. comparable to (very slightly deeper than) the true-null baseline itself - a much more proportionate first step.

**Also checked against the actual output hardware's own resolution**, since the user framed 0.001 as "1 PWM res": the active output path is `PWM_COMPARISON_ENABLED` via LEDC at `RSET_MOD_LEDC_RES=10` bits (`envelope_output.h`) - 1 raw duty LSB = `1/1023 = 0.000978` in duty-fraction terms. 0.001 is very close to this (slightly coarser, not finer) - the user's mental math was correct to within ~2%. Went with the clean 0.001 rather than the exact 0.000978 for readability; noted in the code comment so a future reader doesn't wonder why it's not the "exact" LSB value.

**Changes made**: `ENV_FLOOR_STEP` in `envelope_floor.cpp`: `0.02f -> 0.001f`, with the dB/LSB reasoning above written into the code comment directly (not just here) so it doesn't need re-deriving next time. Found and fixed two knock-on precision bugs while making this change, neither reported by the user, both would have silently undermined the finer step: (1) `serial_commands.cpp`'s `'x'`/`'z'` reply strings printed `envelope_floor_get()` at `%.2f` - every step below 0.01 would have printed as `0.00`, indistinguishable from off, making the whole finer-step change invisible at the terminal; bumped to `%.4f`. (2) the `'P'` preset-dump line ALSO printed `env_floor` at `%.2ff` (to emit a pasteable C float literal) - any floor value below 0.01, including literally every value this finer step now makes possible below the old 0.02 minimum, would have been silently rounded to `0.00f` in the pasted preset line, quietly discarding a deliberately-dialed-in setting the moment someone tried to save it. Bumped to `%.4ff`. Verified all three changes with a standalone compile (existing stub-header method) - clean, and a small test harness confirms 5x `'x'` presses now lands on `0.0050`, and `apply(0.0)`/`apply(1.0)` behave as expected (`0.0050`/`1.0000`).

**Not resolved, and worth being direct about**: neither this step-size fix nor `envelope_floor` at any value will "zero out" or squelch low-level crackle - it will at most reduce it, if the crackle's actual mechanism is envelope excursions into the predistort LUT's steep near-null region (plausible given the "noise floor is fixed, just barely" framing - marginal analog noise sitting right at the ADC's LSB is exactly the kind of thing that would produce small, sample-to-sample envelope jitter right in that region). This is a plausible hypothesis, not a confirmed diagnosis - nothing in this entry constitutes bench evidence that floor actually fixes the reported crackle, only that the tool for dialing it in finely now works and won't lose data when saved. If a real "make it silent" squelch is still wanted after trying finer floor values, that's new design work, not this change.

## 2026-09-25, later still: floor confirmed not sufficient ("as predicted"); built a genuine Mic Squelch instead - new `ssb_dsp.c` stage, `'W'`/`'('`/`')'`, ahead of the compressor

User confirmed the finer-grained `envelope_floor` step from the previous entry "didn't do what is needed" (as flagged when it shipped - that entry was explicit `envelope_floor` cannot zero anything out, structurally) and asked for a proper squelch.

**Design, and why it's a NEW stage in `ssb_dsp.c` rather than another `envelope_*.h` module**: every other post-processing knob this project has (`envelope_floor`/ALC/Soft-Limit) operates on the DEMODULATED ENVELOPE, downstream of the compressor. A squelch placed there would be gating a signal the compressor has ALREADY amplified - `compressor_process()`'s `return x * gain * makeup_gain` applies `makeup_gain` to every sample unconditionally, including ones below its own threshold (where `gain=1.0`, no reduction, but makeup still multiplies) - so residual mic noise gets the same fixed dB boost real speech does before it would ever reach an envelope-side gate, making noise and quiet-real-speech harder to tell apart right where a gate needs to tell them apart. Placed this squelch instead at the TOP of `ssb_dsp_process_sample()`, immediately after the Mic Gain multiply and before the compressor - gating the noise at its true, un-boosted level.

**Three-stage topology, chosen specifically to avoid two failure modes `envelope_floor.cpp`'s own header already documents from two EARLIER, reverted designs (a phase freeze, a hard envelope clamp)** - a single noisy sample tripping the gate open, and the gate itself introducing a click at every transition:
1. **Level detector** - one-pole follower on `fabsf(audio_sample)` (post mic-gain, pre-compressor), 5ms attack / 30ms release. The 5ms attack is deliberately long enough that an isolated single-sample (62.5us) click barely moves it - see the numeric verification below.
2. **Hysteresis (Schmitt trigger)** - opens above `threshold`, closes only once the detector drops below `threshold * 0.5` (~-6dB margin) - standard noise-gate practice, prevents chatter for a signal parked near one value.
3. **Gain ramp** - a SEPARATELY smoothed `[0,1]` gain (5ms attack, 150ms release) is the only thing that actually multiplies the sample - continuous by construction, so there's no hard clamp/kink anywhere in the signal path itself. Same "gain-computer + smoothed asymmetric attack/release" topology `envelope_alc.h` already uses safely, applied to a gate instead of a leveler.

**Numerically verified with two standalone test harnesses** (not just claimed):
- Synthetic 3-phase test (noise+clicks -> sustained 0.3-amplitude "speech" -> silence again): during phase 1 (noise floor 0.002 plus an 80x-amplitude single-sample click every 50ms, threshold=0.01), gate stayed CLOSED the entire time (`gate_open=0`, output exactly `0.00000` peak) - the clicks never tripped it. Phase 2's sustained tone opened the gate within 0.38ms and reached full unity gain. Phase 3 (silence resumes) closed the gate after ~124ms (matches the 150ms gain-release time constant's expected order of magnitude) and settled to a tiny residual (`max|out|=0.0006` after 300ms) - the small residual gain (`0.08` at that point) matches `exp(-2.5)` almost exactly for ~2.5 time constants elapsed since closing, a clean cross-check that the math is behaving as designed, not coincidentally.
- A duration x amplitude sweep, characterizing exactly what does and doesn't trip the gate (threshold=0.01, same time constants):

  | burst duration | 2x thr | 5x thr | 10x thr | 20x thr | 40x thr | 80x thr |
  |---|---|---|---|---|---|---|
  | 0.0625ms (1 sample) | closed | closed | closed | closed | closed | closed |
  | 0.5ms | closed | closed | closed | OPEN | OPEN | OPEN |
  | 1ms | closed | closed | OPEN | OPEN | OPEN | OPEN |
  | 2-3ms | closed | OPEN | OPEN | OPEN | OPEN | OPEN |
  | 5-10ms | OPEN | OPEN | OPEN | OPEN | OPEN | OPEN |

  This is the honest characterization of what this design will and won't catch: a genuinely brief (sub-millisecond) click needs to be very loud (>80x threshold) to get through, matching the ~80x figure derived analytically in the doc comment (`1/(1-exp(-Ts/tau)) ~= 80` at a 5ms tau, 16kHz Fs) almost exactly. But a burst lasting even a few milliseconds opens the gate at quite modest amplitude (2x threshold by 5-10ms). **If the reported crackle is actually short BURSTS rather than isolated single-sample glitches, this squelch may only partially help** - flagged explicitly, not glossed over, since it directly bears on whether this design will actually fix what was reported.

**Changes**: `ssb_dsp.h`/`.c` - new `squelch_t`, `squelch_process()`, `ssb_dsp_set/get_squelch_enabled()`, `ssb_dsp_set/get_squelch_threshold()`, called unconditionally (like Mic Gain, independent of `audio_fx_configured`) right after the Mic Gain multiply in `ssb_dsp_process_sample()`. `config.h` - `SQUELCH_THRESHOLD_STEP=0.001f` (matches `ENV_FLOOR_STEP`'s newly-adopted resolution). `serial_commands.cpp` - `'W'` toggle (reuses the letter freed by the old switchable-compressor-mode removal), `'('`/`')'` threshold step, both the `'P'` preset-dump line and the numeric preset-load block extended with the two new trailing fields. `settings.h` - `PersistentSettings` gains `squelch_enable`/`squelch_threshold` as two more append-only trailing fields; no existing preset needs editing (zero-fills to off/0.0, a genuine preserves-old-behavior default since squelch never existed before). `ssb_mic_test_commands.md` updated with the new commands and the same duration/amplitude limitation table.

**Not yet done**: real-hardware bench validation against the actual reported crackle - everything above is numerically verified against synthetic test signals, not measured against the user's actual noisy mic input. Default threshold (0.01) is a reasoned starting point, not a measured optimum, same epistemic status this project gives every other first-cut feature.

## 2026-09-25, later still: Mic Squelch gets its first real-hardware result - "works well," approach confirmed, more testing wanted

User, on real hardware: "That works well. Needs more testing but certainly the right approach." First actual bench signal on this feature - promotes it from "numerically verified against synthetic signals only" to "genuinely helps on the real reported crackle," though not yet to "fully characterized/tuned." No specific numbers, settings (threshold value used, which preset), or before/after comparison were given in this message, so nothing quantitative to log here beyond the qualitative result itself - worth getting the actual threshold value that worked, and whether default 0.01 needed adjusting, next time real numbers are available (matters for whether 0.01 stays the shipped default or gets revised).

Still open, per the design's own documented limitation (previous entry's duration x amplitude table): whether the remaining "needs more testing" territory includes the specific gap already flagged - short (few-ms) bursts vs. true single-sample clicks - is unknown from this message alone. No code changes made; this is a status update, not a fix.

## 2026-09-26: Mic Squelch's "works well" result had a side effect - a frozen-carrier tone when fully closed; fixed by gating envelope instead of the pre-Hilbert sample

Follow-up to the previous entry's real-hardware "works well" result. User: "One interesting feature is that the squelch drops to absolute zero input to dsp that results in no USB noise but only a small carrier tone. I guess this is a Hilbert feature? Would be better to keep the smallest noise output." Then, more precisely: **"Keep the hilbert fed with noise but reduce env to zero."**

**Root cause**: the shipped design (previous two entries) applied the squelch's gain directly to `audio_sample`, BEFORE the Hilbert transform - `squelch_process()` returned `x * gain`. When the gate is fully closed, `gain` decays (via `flush_denorm`) to an exact, bit-constant `0.0f`, which drives `audio_sample` to exact zero, which fills the Hilbert delay line with zeros once it flushes through, making I=Q=0. `atan2(0,0)` then returns a fixed, degenerate phase value every sample, collapsing computed frequency deviation to exactly 0 Hz - a genuinely unmodulated, frozen carrier. Any real analog RF leakage at that "envelope=0" operating point (a normal EER/polar-PA reality - finite off-isolation in the switching PA / RSET modulation, not something firmware can eliminate on its own) then presents as a discrete, easily audible CW tone, which is *more* perceptible than the incoherent noise it replaced, not less - exactly what the user reported.

**Confirmed with a standalone C test harness** (`/tmp/squelch_test/test_envelope_gate.c`), comparing the OLD and NEW architectures side by side on 0.5s of real (synthetic) noise-floor input that never trips the gate open: the OLD architecture (gate applied to the sample pre-"Hilbert") showed phase IDENTICAL to the previous sample on 100.0% of settled samples once gain flushed to exact zero - a true, complete freeze. The NEW architecture (below) showed 0.0% frozen - phase kept varying sample-to-sample off the real, ungated noise, exactly as it would with squelch disabled - while the gated envelope still reached an exact `0.000e+00` throughout. (Small but real methodological note while building this test: reproducing the OLD bug faithfully required also reproducing the real firmware's `flush_denorm()` call on the sample bound for the delay line - without it, `x * 0.0f` preserves `x`'s sign as a signed zero, and IEEE `atan2`'s sign-of-zero convention makes phase hop between `{0, pi, -pi}` instead of truly freezing at one value. `flush_denorm`'s ternary returns a literal, sign-free `0.0f`, which is what makes the real firmware's freeze a true single-value lock rather than that messier artifact.)

**Fix, per the user's exact instruction**: `squelch_process()` in `ssb_dsp.c` is renamed `squelch_update()` and now ONLY updates the detector/hysteresis/gain state machine (unchanged algorithm, still tapping post-mic-gain `audio_sample` for its level detector) - it no longer touches or returns a modified sample. The call site in `ssb_dsp_process_sample()` no longer reassigns `audio_sample`; the sample flows into the compressor/EQ/Hilbert path completely untouched by squelch, exactly as it would with squelch disabled. The resulting `handle->squelch.gain` is instead multiplied into the computed `envelope` (`envelope = fast_sqrt(I*I+Q*Q)` / `sqrtf(...)`), immediately after that computation and before it's used by the null-bias diagnostic accumulator or returned via `*out_envelope` - so both see the post-gate value, consistent with what the diagnostic is meant to measure (energy actually transmitted) and what the caller actually needs (true RF-power silence). Phase (`atan2(Q,I)`, computed just above from the same, now-always-real I/Q) is left completely untouched by squelch. This is sound because phase is mathematically scale-invariant to any uniform positive gain - `atan2(k*Q, k*I) == atan2(Q, I)` for all `k > 0` - while envelope scales exactly as `k*sqrt(I^2+Q^2)`, so gating envelope alone gives a true, exact RF-power zero without perturbing phase/frequency at all.

**Changes**: `ssb_dsp.c` - `squelch_process()` -> `squelch_update()` (void, state-only, doc comment rewritten with the full root-cause/fix explanation), call site updated, `envelope *= handle->squelch.gain;` inserted right after envelope is computed in `ssb_dsp_process_sample()`. `ssb_dsp.h` - Mic Squelch's doc comment gets a new "2026-09-26 CORRECTION" section explaining the bug and fix, plus the GAIN RAMP algorithm-stage description updated to say it multiplies envelope, not the sample. Syntax-checked clean (`gcc -fsyntax-only -Wall -Wextra`, stub headers, zero warnings) and verified with the new standalone test harness above; no change to the detector/hysteresis tuning itself (still the same, still not independently bench-validated against the user's actual crackle beyond the qualitative "works well" from the previous entry).

**Not yet done**: real-hardware confirmation that this specific fix actually replaces the reported "small carrier tone" with quiet noise as intended - the mechanism and the standalone-harness numbers support it strongly, but nothing here is bench evidence on the real transmitter. Worth listening for specifically next time the gate is exercised on air.

## 2026-09-26, later: real-hardware confirmation - the envelope-gating fix works

User, on real hardware, after the previous entry's fix (gating `envelope` post-Hilbert instead of `audio_sample` pre-Hilbert): "Yes working thankyou." First on-air confirmation that the frozen-carrier/"small carrier tone" side effect is actually resolved, not just fixed on paper/in a standalone test harness - promotes this fix from "mechanism understood + synthetic-harness-verified" to "confirmed fixed on the real transmitter."

No specific detail given (what it sounds like now vs. before, which preset/threshold, whether this was A/B'd against the old behavior) - message is a brief, unambiguous confirmation, not a detailed report, so nothing quantitative to add here beyond the result itself.

Still open, unchanged from before: the squelch's detector/hysteresis tuning (5ms attack, 0.01 default threshold, 0.5x hysteresis ratio) remains a reasoned first cut, not independently bench-validated against the user's original crackle, and the short-burst-vs-single-sample-click limitation documented earlier is still untested against real-world duration.

## 2026-09-29: two-tone IMD test (700/1700Hz, speaker-into-mic), comp on vs comp off - headline finding is a ~9.3dB IMD3 asymmetry, not a compressor effect

User ran a speaker-into-mic two-tone test at 14175kHz (SDRuno capture `SDRuno_20260929_083602Z_14175kHz.wav` uploaded alongside), framed as "thought this test was worth preserving," with envelope level set to ~10% below full scale (matched between conditions) and reporting it as "the IMD readings are very good - best I've seen," attributed tentatively to the elevated ADC-noise floor. Full numeric results (10-point wide-IMD sweep on each side of the two test tones, comp on and comp off) given in the user's message and preserved verbatim below; built into an HTML report with charts, delivered via SendUserFile.

**Settings decoded** from the supplied `PersistentSettings` literal (positional, cross-checked field-by-field against the current struct layout in `settings.h`): Mic source, Chebyshev-8 ADC anti-alias filter, envelope predistortion LUT on, envelope interpolation off, AmpEQ both shelves off, gdeq default, envelope ALC on, envelope Soft-Limit on, mic_gain=+34.0dB, comp_level=6 (toggled live for the A/B), Mic Squelch ON at threshold 0.0160, freq_dev slew unlimited. Squelch being enabled during an IMD test is noted but shouldn't affect these results - both tones are well above any plausible squelch threshold, so the gate stays open throughout a steady two-tone drive.

**Headline finding - a real, consistent IMD3 asymmetry, not a measurement artifact**: the two test tones are matched to within 0.02-0.03dB in both conditions (700Hz/1700Hz reference levels: -47.265/-47.289dBm comp on, -48.128/-48.131dBm comp off), ruling out uneven drive as the explanation for what follows. The classic closest-in third-order pair - 2f1-f2 (-300Hz offset) and 2f2-f1 (+2700Hz offset) - is NOT symmetric: 2f2-f1 (high side) measures -27.623dBc (comp on) / -27.234dBc (comp off), while 2f1-f2 (low side) measures -36.750dBc (comp on) / -36.707dBc (comp off) - a 9.1-9.5dB asymmetry, present almost identically in both conditions (so it's a property of the transmitter's nonlinearity itself, not the compressor). The high-side product is the worst spur in the ENTIRE sweep by a wide margin - if a single "IMD3" number is ever quoted for this transmitter, it should be the high-side (-27.2 to -27.6dBc) figure, not the more flattering low-side one.

**Comp on vs comp off, at matched envelope peak: no clear systematic effect on the wide-IMD comb.** Per-product deltas (comp_on - comp_off) range from -1.58dB to +2.97dB across the 20 measured offsets, with no consistent sign - comp on is sometimes slightly better, sometimes slightly worse, mostly within a fraction of a dB. The single largest delta (+2.97dB, comp on worse) is at +3700Hz; everything else is under ~1.6dB. Read as: at this specific drive point (both conditions deliberately matched to the same envelope peak), the compressor's makeup gain doesn't measurably change the IMD comb's shape either way - consistent with it operating well below the point where the envelope chain's own nonlinearity (predistort LUT, ALC, Soft-Limit, Hilbert/polar recombination) dominates.

**Both combs roll off cleanly and monotonically**, spaced at 1kHz (the two-tone difference frequency) - typical EER/polar spectral regrowth, not evidence of anything newly broken. High-side ladder reaches ~-58dBc by +11700Hz; low-side reaches ~-56dBc by -9300Hz.

**On "best I've seen" - not independently benchmarked against project history this entry.** This test used a 700/1700Hz two-tone pair (1kHz spacing); this project's earlier two-tone/IMD work (see entries from 2026-09-09 onward) mostly used a 700/1900Hz pair (1200Hz spacing) - different spacing changes where products fall relative to any passband edge, so a direct numeric comparison against older logged figures isn't apples-to-apples without deeper review. Taking the user's qualitative assessment at face value here; a dedicated historical comparison (if wanted) is separate follow-up work, not done as part of this entry.

**WAV spot-check, and why its numbers aren't quoted above**: independently loaded the uploaded IQ capture (stereo 16-bit, 388.888kHz sample rate, 4.45s, centred on 14175kHz) - confirmed a steady, non-transient envelope throughout (a single steady-state run, not a combined on-disk recording of both conditions) and located the two test tones close to their nominal 700/1700Hz offsets via FFT, confirming the general two-tone structure. A naive whole-file Blackman-Harris FFT over the full recording bandwidth, however, does NOT cleanly reproduce the reported per-product dB figures (some near-in products came back within a couple dB of the tones themselves, implausibly high) - almost certainly because a blind broadband transform over the entire IQ bandwidth also picks up other energy (adjacent activity on a shared 20m allocation, SDR passband/decimation-edge artifacts) that the user's own targeted "Wide IMD (250Hz bndw)" tool specifically excludes. Flagging this rather than silently either trusting or overriding either source: the report uses the user's harness numbers throughout, with the WAV kept as the archived reference capture, not as a second quantitative dataset.

**Full data** (dBc unless noted; also charted in the delivered report):

| offset | comp ON | comp OFF |
|---|---|---|
| 700Hz ref | -47.265 dBm | -48.128 dBm |
| 1700Hz ref | -47.289 dBm | -48.131 dBm |
| -300Hz | -36.750 | -36.707 |
| -1300Hz | -39.061 | -40.045 |
| -2300Hz | -41.544 | -41.440 |
| -3300Hz | -43.358 | -42.414 |
| -4300Hz | -45.064 | -44.739 |
| -5300Hz | -46.384 | -47.115 |
| -6300Hz | -50.545 | -48.969 |
| -7300Hz | -52.032 | -52.314 |
| -8300Hz | -53.720 | -54.171 |
| -9300Hz | -55.919 | -55.393 |
| +2700Hz | -27.623 | -27.234 |
| +3700Hz | -35.877 | -38.847 |
| +4700Hz | -41.643 | -41.499 |
| +5700Hz | -43.660 | -44.395 |
| +6700Hz | -47.608 | -47.504 |
| +7700Hz | -50.700 | -50.263 |
| +8700Hz | -53.280 | -53.924 |
| +9700Hz | -55.522 | -55.760 |
| +10700Hz | -57.160 | -56.577 |
| +11700Hz | -57.984 | -58.132 |

**Not yet done**: no root-cause investigation into WHY the high-side (2f2-f1) IMD3 product runs ~9.3dB hotter than the low-side (2f1-f2) one - that's a real, repeatable, and fairly large asymmetry that would be worth understanding (candidates not yet explored: asymmetric group-delay/EQ response across the audio band, an asymmetry in the Hilbert transform's own frequency response, or something in the predistort LUT/ALC/Soft-Limit chain that treats rising vs falling envelope excursions differently). No historical cross-check against the project's own earlier (700/1900Hz) IMD baselines.

## 2026-09-29, later: chasing the 9.3dB IMD3 asymmetry - ruled out audio-band noise and (mostly) ruled out the presence-EQ tone-mismatch mechanism; matches this project's own long-tracked relative_delay-sensitive Low/High 3rd IMD gap instead

User's hypothesis on the previous entry's headline finding: "the first IMD on high side is in the audio band noise I think - is that effect significant?" Worked through it rather than guessing.

**Audio-band noise, checked against the comb's own shape and found unlikely as the explanation**: a genuine elevated noise floor in the mic/ADC/DSP chain (which absolutely can reach the transmitted RF in this architecture, same path the wanted tones take) would be expected to also raise neighboring 1kHz-spaced bins, not just the one at +2700Hz - but the very next point, +3700Hz, drops 8-12dB straight back onto the comb's normal decay trend. That sharp a fall-off right after the peak reads as a discrete coherent product, not a raised noise floor local to one bin.

**Re-checked this project's own 2026-09-03 presence-EQ finding (`group_delay_fit_notes.md`) against TODAY's actual coefficients, rather than assume the old number still applies**: that entry (for the 700/1900Hz pair) found the presence peak boosts the higher tone several dB more than the lower one - a textbook route to asymmetric third-order products, since for a static cubic nonlinearity the ratio of the two IMD3 products' POWER equals `20*log10(a2/a1)` (a2/a1 = the two drive tones' amplitude ratio reaching the nonlinearity). Recomputed the actual RBJ HPF (300Hz, Q=0.707) + presence peak (2200Hz, Q=1.0, **+2.0dB** - today's configured gain in `ssb_mic_test.ino`, not necessarily the same gain the September entry used) response at 700Hz and 1700Hz: combined differential is only **+1.44dB** today, which would predict only ~1.4dB of IMD3 asymmetry via this mechanism alone - real, right direction (favors the higher tone, matching the observed high-side-worse pattern), but nowhere near sufficient to explain the observed ~9.3dB gap by itself.

**What actually matches, closely and repeatedly: the `relative_delay_samples`-sensitive Low/High 3rd IMD gap this project has tracked since 2026-09-01.** Every logged two-tone run in `group_delay_fit_notes.md` that reports both figures shows "High 3rd IMD" worse than "Low 3rd IMD," in every configuration tested: delay=1.00 (gdeq/interp off) -41.41/-34.17dB (7.24dB gap); delay=3.00 (gdeq+`I` on) -34.50/-29.97dB (4.53dB gap); delay=1.10 (gdeq alone, re-tuned for min 3rd IMD) -30.90/-27.17dB (3.73dB gap). This test's own figures (-36.7/-27.2 to -27.6dBc, ~9.3dB gap) are the same pattern, same direction, at `relative_delay=2.68` - a value that doesn't match any of those three previously re-tuned points, and the resulting gap here is larger than any of them, consistent with 2.68 not being specifically optimized against this asymmetry for today's EQ/gdeq/interp combination.

**Answer given to the user**: significant in the sense that it reliably sets the real worst-case IMD3 figure (quote the high-side number, not the low-side one) and is larger here than any previously-logged combination; not significant as evidence of anything newly broken - it's a repeated, structural signature of this transmitter (most likely the AM/envelope-vs-PM/phase path timing mismatch this project has already spent real effort on), not audio-band noise and not primarily the presence EQ. Recommended concrete next step: re-sweep `relative_delay_samples` on this exact 700/1700Hz pair while watching the Low/High 3rd IMD gap specifically - the same bench procedure already used multiple times in `group_delay_fit_notes.md`, not yet run for this pair/gain/gdeq combination.

**Not yet done**: the actual relative_delay sweep against this 700/1700Hz pair and today's settings (env_gdeq default/off, envelope_interp off, ampeq off, presence_gain=2.0dB) - everything above is reasoned from existing coefficients/history, not a fresh bench measurement targeting this specific asymmetry.

## 2026-09-29, later still: CORRECTION - the 9.3dB IMD3 asymmetry was mostly a noise-contaminated measurement bin, not the relative_delay/AM-PM mechanism the previous entry blamed

User re-ran the same two-tone IMD test with the pair pushed up to 700/1900Hz (from 700/1700Hz), specifically to move the near-in products off whatever frequency the earlier elevated reading sat at: "I pushed the 3rds up in freq by changing to 700/1900 and it shows that the power in 250Hz is picking up the background noise previously. There is asymmetry that we've always seen but the absolute levels are very good." Full data below; added to the delivered HTML report (both tone pairs now shown side by side, with a corrected "What this shows" narrative).

**This confirms the user's original hypothesis from two entries ago, and retracts this file's own previous conclusion.** At 700/1900Hz, the near-in products move from &minus;300/+2700Hz to &minus;500/+3100Hz, and the asymmetry collapses: low side (2f1&minus;f2, &minus;500Hz) reads &minus;35.272dBc (comp on) / &minus;35.071dBc (comp off); high side (2f2&minus;f1, +3100Hz) reads &minus;34.411dBc (comp on) / &minus;32.332dBc (comp off) - a residual gap of only 0.86dB (comp on) / 2.74dB (comp off), down from the original ~9.3dB. Reference tone balance stayed tight (700Hz vs 1900Hz: 0.175dB comp on, 0.659dB comp off). Since moving the measurement frequency is the only thing that changed between the two runs, the simplest and correct explanation is the user's own: the +2700Hz bin in the original 700/1700Hz test was picking up a background noise/spur contribution specific to that frequency, not (mainly) measuring a genuine transmitter third-order product - the earlier ~9.3dB "IMD3 asymmetry" finding overstated the transmitter's actual nonlinearity.

**This means the previous entry's diagnosis (matching the gap to `group_delay_fit_notes.md`'s documented relative_delay-sensitive Low/High 3rd IMD pattern) was the wrong call, or at best a minor contributor, not the dominant one.** Worth being explicit about why that reasoning went wrong in hindsight: the historical Low/High 3rd IMD table used the 700/1900Hz pair (its high-side product was always at +3100Hz, same frequency as today's re-test, not +2700Hz), so those OLDER figures (3.7-7.2dB gaps) were never affected by whatever sits near 2700Hz - they're likely still genuine relative_delay-sensitive asymmetry, and remain a real, small (few-dB) effect, consistent with today's residual 0.9-2.7dB gap at 700/1900Hz. But applying that same historical pattern to explain the NEW test's 700/1700Hz result was an error - that test's high-side product landed at a different, apparently noise-affected frequency (+2700Hz) never present in the historical comparison set, and the size of the discrepancy (9.3dB vs. the 3.7-7.2dB historical range) should have been treated as a bigger red flag against a clean match than it was at the time.

**Absolute levels, once measured away from the noisy bin, are good** - roughly &minus;32 to &minus;35dBc on the near-in products at 700/1900Hz, with the far rungs of the comb reaching &minus;59 to &minus;61dBc by the 9th/10th product (vs. &minus;56 to &minus;58dBc for the 700/1700Hz run) - consistent with the user's "the absolute levels are very good" assessment now that the inflated near-in reading is out of the picture.

**One secondary wrinkle noted in the updated report, not chased further here**: at 700/1900Hz, the near-in high side isn't perfectly clean either - both +3100Hz (3rd order) and +4300Hz (5th order, 3f2&minus;2f1) read within ~2dB of each other, rather than the 5th-order term dropping cleanly below the 3rd the way a simple nonlinearity model would predict. Flagged as a minor, secondary detail worth another look sometime, not this entry's main finding.

**Full data, 700/1900Hz** (dBc unless noted):

| offset | comp ON | comp OFF |
|---|---|---|
| 700Hz ref | -47.219 dBm | -47.302 dBm |
| 1900Hz ref | -47.394 dBm | -47.961 dBm |
| -500Hz | -35.272 | -35.071 |
| -1700Hz | -36.865 | -37.690 |
| -2900Hz | -38.889 | -38.770 |
| -4100Hz | -41.292 | -42.192 |
| -5300Hz | -45.109 | -46.474 |
| -6500Hz | -48.764 | -49.585 |
| -7700Hz | -53.247 | -54.280 |
| -8900Hz | -54.945 | -57.008 |
| -10100Hz | -57.481 | -58.751 |
| +3100Hz | -34.411 | -32.332 |
| +4300Hz | -32.668 | -34.596 |
| +5500Hz | -41.228 | -38.868 |
| +6700Hz | -42.859 | -42.763 |
| +7900Hz | -46.666 | -47.530 |
| +9100Hz | -51.038 | -51.562 |
| +10300Hz | -55.162 | -55.845 |
| +11500Hz | -57.690 | -59.094 |
| +12700Hz | -59.714 | -60.882 |

**Not yet done**: no direct identification of what specifically sits near +2700Hz in the 700/1700Hz configuration (candidates not checked: a PWM/switching-related spur, an ADC/DDS clock artifact, or simply elevated broadband noise that happens to be locally worse in that narrow band) - the conclusion here is "it's not mainly the transmitter's own IMD3," not a specific identified noise source. No re-measurement of the 700/1700Hz pair to see if the +2700Hz reading is repeatable/consistent run-to-run (which would help distinguish a stable discrete spur from transient interference).

## Open items carried from earlier sessions, still unresolved

- `MAX_FREQ_DEV_HZ` currently `20000.0f` (config.h:405) - a widened
  diagnostic value per its own comments, never re-optimized to a final
  number (comment nearby still says "8000Hz is being kept for now" from an
  earlier edit pass that didn't get updated). Not part of this thread but
  flagged here so it doesn't get lost.
- `dac_task_enabled` root-cause (stray `1` left in a local config.h,
  re-enabling Core-1 I2C activity) - user fixed locally; not written up in
  the project notes per user's explicit "don't worry about it" on
  2026-09-09, recorded here only for continuity of this log.
