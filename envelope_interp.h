#pragma once

/**
 * envelope_interp.h
 *
 * ---- Envelope output interpolation (ZOH-imaging reduction) ----
 * Right now the commanded PWM duty only changes once per DSP tick
 * (SAMPLE_RATE_HZ = 16000) - a classic zero-order-hold staircase feeding
 * the analog Sallen-Key reconstruction filter. A ZOH held at Fs puts its
 * first spectral image at Fs itself, and per the real LTspice sweep behind
 * envelope_gdeq.h, the analog filter only provides -27dB of attenuation
 * there - not deeply buried at all. This mirrors exactly what QRP Labs'
 * G0UPL found and fixed in the QMX SSB firmware ("Bringing SSB to QMX",
 * see project history): before linearly interpolating the amplitude
 * stream, the oscilloscope showed clear overshoot on rising steps and
 * undershoot on falling ones; after interpolating, "NO visible trace of
 * any overshoot/undershoot". He went to 28x (driven by his own DAC/CPU
 * clock constraints); ENVELOPE_INTERP_FACTOR=4 here is a deliberately
 * modest first step, not a re-derivation of his number - see below.
 *
 * Checked against the same filter data before writing any of this: moving
 * the first ZOH image from Fs (16kHz, -27dB down) out to 4*Fs (64kHz,
 * -63dB down) is +36dB of "for free" suppression, just from relocating the
 * image into a part of the filter's stopband it was never asked to fight
 * in-band. NOT yet validated on real hardware - same status every other
 * not-yet-bench-confirmed feature here starts at (gdeq, predistort).
 *
 * ---- Design history (FOUR attempts - the first three each failed on real
 * hardware in a different way; recorded here because each rules out an
 * approach that looks reasonable on paper) ----
 *
 *   v1: a second, independent gptimer at 64kHz, whose ISR did the
 *       interpolation arithmetic AND called envelope_output_write_pwm()
 *       (ledc_set_duty()/ledc_update_duty()) directly from true interrupt
 *       context. REBOOTED the ESP32 - those are general ESP-IDF driver
 *       calls with no IRAM/ISR-safety guarantee.
 *   v2: kept the 64kHz ISR, deferred only the PWM write to a dedicated
 *       task via vTaskNotifyGiveFromISR. Still crashed - a "Coprocessor
 *       exception" panic, because the ISR still did the interpolation
 *       *arithmetic* (floats), and on this chip's Xtensa cores the FPU is
 *       itself a coprocessor whose context only survives a *task* switch;
 *       any float op in a true ISR is unsafe regardless of what's done
 *       with the result.
 *   v3: moved ALL the work (arithmetic + write) into a dedicated task, ISR
 *       reduced to an integer step-counter + notify. Stopped crashing, but
 *       waking a task 64,000 times/second was enough CPU overhead to
 *       starve handle_serial_commands() (loop(), priority 1, same Core 1
 *       the task was pinned to) - 'I' stopped rebooting the board but the
 *       board then stopped responding to serial input entirely. Then
 *       rebuilt around the LEDC peripheral's own hardware duty-fade engine
 *       (ledc_set_fade_with_step()+ledc_fade_start()) instead of a
 *       software timer/ISR/task at all - no more per-sub-step CPU cost.
 *       That fixed the crash and the starvation, but exposed a THIRD,
 *       different problem: the LEDC PWM clock (78125Hz) has no phase
 *       relationship to SAMPLE_RATE_HZ's own gptimer (78125/16000 isn't
 *       even an integer ratio), so every fade's first step waited for an
 *       essentially random PWM-clock boundary - up to ~12.8us of
 *       unsynchronized timing jitter per tick, visible on real hardware as
 *       the envelope's effective frequency jittering.
 *   v4 (current): the user's own suggestion - stop trying to interpolate
 *       from a SECOND clock domain entirely. Instead, speed up the
 *       EXISTING sample gptimer itself (the one dsp_task already blocks
 *       on) to ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ, and do the full,
 *       expensive DSP pipeline (Hilbert/atan2/sqrt/freq_dev/ADC/AD9851/
 *       diagnostics - everything) on only 1 in ENVELOPE_INTERP_FACTOR of
 *       those ticks ("full" ticks, still true SAMPLE_RATE_HZ), while the
 *       other 3 just walk a linear interpolation ramp and call the same
 *       plain envelope_output_write_pwm() the original code always used.
 *       This is a strictly simpler machine than v1-v3: ONE clock domain
 *       (no cross-clock jitter - fixes v3's bug), ZERO new ISRs or tasks
 *       of our own (everything happens inside dsp_task, already a normal,
 *       proven-safe FreeRTOS task - fixes v1/v2's bugs), and dsp_task has
 *       ALWAYS lived on Core 0, physically separate from loop()/Serial on
 *       Core 1 - so running it more often cannot starve serial commands
 *       the way pinning a NEW task onto Core 1 did in v3 (that was the
 *       actual mistake in v3, not "task priority too high" - no priority
 *       tweak within v3's architecture could have fixed it).
 *
 *       CORROBORATION, Fs-jitter-hunt: dsp_task was later TRIED on Core 1
 *       (to eliminate gptimer's crosscore-IPI wake cost - see the .ino's
 *       "TRIED, REVERTED" note on its xTaskCreatePinnedToCore() call) and
 *       REVERTED after real hardware starved Serial completely (output
 *       AND commands). Notably this happened with interp ('I') OFF, i.e.
 *       WITHOUT the 4x wake rate this paragraph's v3 warning is about -
 *       CPU-time budget alone said Core 1 had ample headroom (see
 *       diagnostics.cpp's [core1] busy breakdown, ~98-99% idle) yet
 *       loopTask still got starved outright, not just delayed. So
 *       whatever actually broke v3 may be a harder structural problem
 *       with sharing a core between dsp_task and loop() than "not enough
 *       spare CPU time" - root cause not yet understood either time.
 *
 * ---- v4 design ----
 * dsp_task (see the .ino) keeps a plain, single-threaded fast-tick counter
 * and treats tick (counter % ENVELOPE_INTERP_FACTOR == 0) as a "full"
 * tick - unchanged DSP pipeline, still running at the true SAMPLE_RATE_HZ
 * exactly as before this feature existed. Every other filter constant,
 * diagnostic counter, ADC capture call, and AD9851 write in the codebase
 * keeps assuming SAMPLE_RATE_HZ unchanged - the gptimer's rate changed,
 * SAMPLE_RATE_HZ itself deliberately did NOT. The counter itself is
 * advanced by ulTaskNotifyTake()'s own return value (how many hardware
 * ticks actually elapsed), not by a bare +1 per wake - see the .ino for
 * why: xClearCountOnExit=pdTRUE silently coalesces multiple real ticks
 * into one wake whenever dsp_task is even briefly late, and a naive +1
 * counter desyncs permanently the first time that happens.
 *
 * At each full tick, envelope_interp_on_full_tick(delayed_envelope,
 * tick_start_us) is called (replacing the old, unconditional
 * envelope_output_write_pwm() call at that point) - tick_start_us is
 * esp_timer_get_time() captured by dsp_task at the TRUE top of this tick,
 * before any DSP processing, and is what makes the ramp below time-
 * accurate rather than step-counted (see next paragraph for why that
 * distinction turned out to matter). When disabled, it's a direct pass-
 * through - IDENTICAL behavior to before this feature existed. When
 * enabled, it starts a new ramp from the previous full-tick's envelope
 * value to this one. On each of the ENVELOPE_INTERP_FACTOR-1 "interp"
 * ticks in between, envelope_interp_on_interp_tick() writes the next
 * point on that ramp via the same plain envelope_output_write_pwm().
 * No-op when disabled - PWM duty simply holds its last value between full
 * ticks, i.e. plain ZOH, matching pre-feature behavior exactly.
 *
 * v4.1 bug fix, found on real hardware: the envelope's timing still
 * looked jittery even AFTER the notification-coalescing fix above landed
 * (which only guaranteed full-tick PHASE stayed locked to the true 16kHz
 * grid - it said nothing about the RAMP's own shape). Root cause: the
 * first cut counted ramp position by a plain step counter (s_step,
 * incremented once per on_interp_tick() call, frac=s_step/FACTOR) rather
 * than by real elapsed time. That's fragile against exactly the same
 * coalescing effect described above, but now applied to "interp" ticks
 * specifically - and it turns out to fire on almost every group, not
 * rarely: a full tick's own DSP processing (tens of us) is comparable to
 * or LONGER than one fast sub-period (SAMPLE_PERIOD_US_F /
 * ENVELOPE_INTERP_FACTOR = ~15.6us), so by the time dsp_task finishes a
 * full tick's work and gets back to blocking, 1-2 of the FOLLOWING
 * interp ticks have usually already elapsed and coalesced away - losing
 * that many s_step increments, every cycle, by an amount that varies with
 * exactly how long that particular tick's DSP processing happened to
 * take. That made the rendered ramp shape irregular tick to tick - a real,
 * structural, sample-content-dependent source of jitter in the fine-
 * grained envelope shape, distinct from (and not fixed by) the earlier
 * full-tick-phase fix.
 *
 * The fix: stop counting steps: compute_ramp_value() (envelope_interp.cpp)
 * derives frac directly from esp_timer_get_time() - s_tick_start_us,
 * divided by the true sample period. Whatever calls DO happen - however
 * many, however irregularly spaced, even with some coalesced away - each
 * one computes the mathematically correct point on the prev->target line
 * for the instant it actually runs. Fewer calls in a given cycle just
 * means fewer visible steps in that cycle's staircase, not a wrong value
 * at the steps that do render - which is what actually mattered for the
 * jitter, since the analog reconstruction filter downstream only ever
 * sees the written VALUES and when they land, not how many software calls
 * produced them.
 *
 * Why ramp forward from the OLD value to the NEW one across the
 * FOLLOWING interval, rather than jumping straight to the new value
 * immediately: there is no way to interpolate TOWARD a sample that
 * hasn't been computed yet without introducing latency somewhere - any
 * causal (non-clairvoyant) smoothing of a step MUST spend the interval
 * AFTER a new value becomes known smoothing the transition INTO it, which
 * means the reconstructed output only reaches that true value at the far
 * end of the interval. This trades exactly one sample period
 * (1/SAMPLE_RATE_HZ, 62.5us) of pure added group delay for eliminating
 * the step discontinuity - a completely standard reconstruction-filter
 * tradeoff, and one already within what this project's existing relative-
 * delay line ('['/']', relative_delay.h) is built to compensate for
 * exactly this kind of envelope-path delay. v1-v3 all had this same
 * property already (look back at their own "arrives at the new target
 * right as the next tick fires" framing) - v4 doesn't change the
 * TIMING model, only how (and how safely) it's implemented.
 *
 * No locking anywhere in this version, unlike v1-v3 (which all needed a
 * spinlock because an ISR or second task shared the ramp state directly):
 * s_p0..s_p3/s_m1/s_m2/s_tick_start_us/s_last_value are dsp_task-PRIVATE -
 * on_full_tick()/on_interp_tick() are only ever called from dsp_task
 * itself, strictly sequentially, and no other task reaches into them.
 * envelope_interp_set_enabled() IS called from a different task (whichever
 * one processes serial commands - loop(), Core 1) - but rather than
 * having it reach into that dsp_task-private state directly (which would
 * make it shared again and need a lock), it only raises a single-word
 * volatile flag (s_reseed_pending); dsp_task itself applies the actual
 * reseed, in its own context, the next time on_full_tick() runs. Both
 * s_enabled and s_reseed_pending are plain volatile bools, same
 * convention already used elsewhere in this codebase for single-word
 * cross-task flags (envelope_gdeq/predistort's own enabled bools).
 *
 * Off by default (same convention as 'g'/'D'/'x'/'z'/'{'/'}') so existing
 * tuning isn't disturbed until deliberately opted into. Toggle via 'I'.
 * On an off->on transition, the ramp's prev/target are both seeded to the
 * last submitted value and marked already-arrived, so enabling never
 * produces a glide-from-zero (or glide-from-stale-value) transient - see
 * envelope_interp_set_enabled()'s own comment.
 *
 * Runs regardless of AD9851_ATTACHED - like envelope_output.cpp itself,
 * the PWM/RSET envelope path doesn't depend on the AD9851 phase path being
 * present.
 *
 * NOT yet checked on real hardware (this is a fresh rewrite): whether
 * Core 0 comfortably sustains the interp ticks' small extra register-write
 * work on top of the full tick's already-proven budget, at 4x the wake
 * rate. Expected to have ample margin (the full tick's own cost is
 * unchanged from before this feature existed; the interp ticks are
 * trivial - a few FLOPs and the same plain PWM write the original code
 * always made), but worth watching on the existing '[timing]' diagnostic
 * overlay ('v') after enabling 'I', same as every other not-yet-bench-
 * confirmed feature here. (dsp_task stays on Core 0 for this - see this
 * file's own "CORROBORATION, Fs-jitter-hunt" note above for why Core 1
 * isn't a safe alternative home for it.)
 *
 * ---- v4.2: linear ramp -> Catmull-Rom / cubic Hermite curve ----
 * Motivated by a real question: is there a cheaper way to improve
 * envelope-path fidelity than raising ENVELOPE_INTERP_FACTOR to 8?
 * Answer: raising it further is a dead end regardless of CPU budget -
 * RSET_MOD_LEDC_FREQ_HZ (the LEDC PWM carrier, fixed at 78125Hz) is a
 * hard ceiling on any useful envelope update rate, and 4x (64kHz) is
 * already the highest multiple of SAMPLE_RATE_HZ that stays under it; 8x
 * (128kHz) would write the duty register faster than the PWM carrier
 * itself can act on it, for zero real benefit.
 *
 * UPDATE 2026-09-08: "fixed at 78125Hz" above is now stale -
 * RSET_MOD_LEDC_FREQ_HZ was retuned to 64000Hz (see envelope_output.h's
 * own dated comment) to test whether putting the PWM carrier and the
 * envelope update rate on a commensurate footing removes the ~12.8us of
 * unsynchronized jitter the v3 hardware-fade attempt found between them
 * (both original problem and the retune's reasoning described in the
 * "CORROBORATION" section above). Doesn't reopen the 8x-factor question
 * this paragraph answers, though: 128kHz would still write faster than
 * whatever the carrier is now, so the "raising it further is a dead end"
 * conclusion stands regardless of this specific retune - only the
 * SPECIFIC ceiling number (78125) changed, not the shape of the argument.
 * Not yet bench-verified whether the retune actually fixes the jitter -
 * see group_delay_fit_notes.md's matching entry.
 *
 * Curve SHAPE, not update
 * RATE, was the remaining lever: v4's ramp is a straight line between
 * consecutive full-tick samples, which has a kink at every tick boundary
 * - real spectral content a smooth signal shouldn't have. Catmull-Rom
 * fits a smooth cubic through the same samples instead, with no extra
 * PWM/wake-rate cost at all (same 4 calls per full tick as before, still
 * ENVELOPE_INTERP_FACTOR=4 - a few more FLOPs per call is the entire
 * added cost).
 *
 * Validated first in an offline Python simulation (not this codebase)
 * against two of this project's own real test signals, not an arbitrary
 * synthetic one: the two-tone envelope (700/1900Hz, TWOTONE_AMPLITUDE=
 * 0.45 - closed-form analytic envelope = 2A*|cos(pi*(f2-f1)*t)|, a
 * full-wave-rectified 600Hz cosine with a hard null every 1/1200s) and
 * the AM-test envelope (AM_TEST_MOD_HZ=1200Hz smooth sine, no fold at
 * all). Result was a genuine split, not a clean win either way: ~23dB
 * interpolation-error reduction on the smooth AM-test envelope, but only
 * ~2dB on the two-tone envelope specifically - because that error is
 * dominated by the null's own hard fold (a derivative discontinuity),
 * which no smooth interpolant, linear or cubic, can fit well; null-region
 * behavior is envelope_floor's job, not this one's. So: worth having as a
 * general envelope-quality improvement (mic audio and most other test
 * modes should see something closer to the AM-test result), but don't
 * expect it to move the two-tone IMD3 number much on its own.
 *
 * Data layout: s_prev_env/s_target_env (2 points) became s_p0..s_p3 (4
 * consecutive full-tick samples, oldest to newest) plus two tangents
 * s_m1/s_m2, all recomputed once per full tick and reused across that
 * tick's interp ticks - same reuse pattern v4 already used, just wider
 * history. The segment actually RENDERED during any given full tick is
 * [s_p1,s_p2] - s_p0/s_p3 only shape the tangents at each end (standard
 * Catmull-Rom: m1=(p2-p0)/2, m2=(p3-p1)/2), they're never points the
 * curve itself passes through.
 *
 * Latency cost: v4 already added one full tick (~62.5us) of group delay
 * by design (see "arrives at the new value only at the far end of the
 * interval" above) - v4.2 adds ONE MORE full tick on top of that (~125us
 * total now), because the tangent at the segment's own right end (s_m2)
 * needs s_p3 - the NEXT full tick's value - which only becomes known the
 * instant that next tick's on_full_tick() call happens. This is the
 * standard Catmull-Rom look-ahead requirement, not a bug, and it's
 * exactly the same category of fixed envelope-path delay relative_delay
 * ('['/']') already exists to compensate - re-tune it after enabling,
 * same as 'g' and 'I' itself already ask you to.
 *
 * A cubic, unlike v4's straight line, can mathematically over/undershoot
 * slightly beyond its own segment endpoints on a sharp enough feature -
 * neither real test signal above triggered this, but a synthetic
 * worst-case check (two duty-0 samples flanked by two much higher ones -
 * close to what a two-tone null looks like) did produce a small negative
 * excursion. compute_ramp_value() clamps its result to [0,1] to close
 * this off unconditionally, since envelope_output_write_pwm() casts
 * straight to an unsigned duty with no clamp of its own downstream - an
 * unclamped negative excursion would become a huge duty via unsigned
 * wraparound, not a small negative one.
 *
 * NOT yet validated on real hardware - baseline already captured before
 * this change (linear 'I' x4, current 'D' predistort table) via the
 * user's own logger, specifically so this change has a direct before/
 * after to compare against once flashed.
 *
 * ---- v4.3: curve made runtime-switchable (linear vs. Catmull-Rom) ----
 * Added for a direct A/B, prompted by the group-delay-equalizer/new-filter
 * investigation (see group_delay_fit_notes.md's 2026-09-01 entries):
 * v4.2's own validation already showed a smooth cubic structurally cannot
 * reproduce a two-tone null's hard fold (a genuine derivative
 * discontinuity - see the v4.2 note above) the way a straight-line ramp
 * naturally can, and there was no way to test that on real hardware
 * without a rebuild/reflash between the two curves. compute_ramp_value()
 * now branches on envelope_interp_get_curve(): ENVELOPE_INTERP_CURVE_
 * CATMULL_ROM (=0, the v4.2 behavior, unchanged) or ENVELOPE_INTERP_CURVE_
 * LINEAR (=1, a straight line from s_p1 to s_p2).
 *
 * Deliberately does NOT restore v4's original 2-point/1-tick-latency
 * history - both curve choices still evaluate over the SAME [s_p1,s_p2]
 * segment from the existing v4.2 4-point/2-tick-latency pipeline
 * (s_p0..s_p3/s_m1/s_m2 unchanged either way; linear mode simply doesn't
 * use s_m1/s_m2). That's an intentional choice, not laziness: reverting to
 * v4's narrower window for "linear" mode would confound the comparison
 * with a one-tick group-delay difference between the two curves, on top of
 * whatever the curve SHAPE itself does - exactly the kind of extra
 * variable this project has had to tease apart before (see gdeq's own
 * fit-window-vs-wideband-excitation confusion). Holding the data window
 * and timing identical between LINEAR and CATMULL_ROM isolates curve shape
 * alone, so a straight before/after comparison at a FIXED relative_delay
 * is meaningful without needing to also re-tune delay between the two
 * curve choices - matching the project's actual purpose here (a direct
 * A/B), not a literal restoration of v4's original zero-look-ahead
 * implementation.
 *
 * Runtime toggle via 'C' (serial_commands.cpp) - cycles CATMULL_ROM ->
 * LINEAR -> HOLD -> CATMULL_ROM (v4.4 added the third state; see below),
 * independent of 'I' itself (the enable/disable toggle); only affects
 * rendered output while 'I' is ON (envelope_interp_get_enabled() true) -
 * with 'I' off, on_full_tick() takes its early-return plain-ZOH path
 * regardless of curve selection. No reseed needed on a curve switch
 * (unlike 'I' itself) - it only changes which formula reads the existing
 * s_p0..s_p3/s_m1/s_m2 state, not the state itself, so a mid-ramp switch
 * just changes the shape of the segment currently being rendered, not its
 * endpoints. Persisted in PersistentSettings as envelope_interp_curve
 * (settings.h), appended at the struct's end same as every other lever
 * added after presets already existed - defaults to ENVELOPE_INTERP_CURVE_
 * CATMULL_ROM (=0) so every existing preset keeps today's live behavior
 * unless explicitly set otherwise.
 *
 * ---- v4.4: third curve, HOLD - isolates write RATE from ramp SHAPE ----
 * User's own hypothesis, worth stating plainly because it's exactly right:
 * 'I' on/off doesn't just add smoothing, it changes the EFFECTIVE PWM
 * UPDATE RATE. With 'I' off, on_interp_tick() is a complete no-op (early
 * return, no register write) - the LEDC duty register is only touched once
 * per full tick, 16kHz. With 'I' on (either curve so far), it's touched
 * ENVELOPE_INTERP_FACTOR (4) times as often, 64kHz. LINEAR-vs-CATMULL_ROM
 * (v4.3) only ever compared two ways of filling that faster update rate
 * with a RAMP - it never tested whether the faster rate itself, independent
 * of any ramp, does anything (LEDC/driver-level timing effects from simply
 * re-issuing ledc_set_duty()/ledc_update_duty() 4x more often, even with an
 * UNCHANGED value each time).
 *
 * HOLD answers that: it writes each full tick's own envelope value,
 * unchanged, on all ENVELOPE_INTERP_FACTOR (4) of that tick's writes - a
 * genuine zero-order-hold at 64kHz instead of 16kHz, with NO ramp toward
 * the next value at all. Unlike LINEAR/CATMULL_ROM, which both deliberately
 * spend one full tick "arriving" at their target (the v4/v4.2 look-ahead
 * latency cost - see above), HOLD adds NO extra latency: on_full_tick()
 * writes the fresh value immediately (bypassing compute_ramp_value()'s
 * [s_p1,s_p2] segment logic entirely for this curve - see
 * envelope_interp.cpp), identical timing to 'I' off, just repeated 3 more
 * times per period instead of left untouched. This makes the three curves
 * a genuine 3-way decomposition of what "'I' on" actually changes:
 *   - 'I' off:                     16kHz updates, whatever value, once each
 *   - 'I' on, HOLD:                64kHz updates, SAME value 4x in a row
 *   - 'I' on, LINEAR/CATMULL_ROM:  64kHz updates, ramping toward next value
 * If HOLD measures/sounds the same as 'I' off, the update-rate variable is
 * cleared and whatever 'I' does is really about the ramp. If HOLD measures/
 * sounds different from 'I' off despite carrying byte-for-byte identical
 * VALUES (just written more often), that points at something in the LEDC
 * peripheral/driver's own behavior when re-triggered at 64kHz, independent
 * of envelope content entirely - a real, different finding from anything
 * this document has chased so far.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"
// 2026-09-29: added for ENVELOPE_ISR_INTERP_ENABLED's own #error guards
// below, which reference AD9851_ATTACHED/AD9851_ISR_WRITE_ENABLED
// (config.h). Real-hardware compile (2026-09-29) found this file gets
// pulled in via settings.h -> dsp_state.h -> dsp_state.cpp WITHOUT
// config.h already included in that translation unit, and separately via
// envelope_interp.cpp's own include order (envelope_interp.h before its
// own config.h include) - in both cases AD9851_ATTACHED/AD9851_ISR_
// WRITE_ENABLED read as undefined (0) at the #if below regardless of
// their real config.h values, misfiring the guard. config.h has #pragma
// once, so including it here is always safe no matter what already
// included it - same pattern envelope_output.h already uses for the same
// reason. This is the general fix: any header whose OWN #if logic
// references a config.h macro should include config.h itself, rather
// than relying on every caller's include order to happen to get it right
// first - envelope_interp.h just didn't need to until this flag added
// its first such reference.
#include "config.h"

// Deliberately modest first step (see header comment above for why this
// isn't just "copy Hans's 28x") - not currently exposed as a runtime-
// tunable the way the slew limiter is; revisit as a live '<'/'>'-style
// step if/once a fixed 4x is confirmed worthwhile on real hardware.
//
// 2026-09-08: TEMPORARILY set to 1 for a specific isolation test - see
// group_delay_fit_notes.md's matching 2026-09-08 "x1 16kFs with hardware
// fade" entry. Purpose: v5's hardware-fade result (same session, same day)
// showed the burble/noise-floor degradation persists at full severity even
// with dsp_task's wake-rate overhead apparently eliminated, pointing at the
// WRITE/UPDATE-RATE itself (4x more duty changes/sec) as the dominant cause
// rather than anything CPU/timing-related - but that conclusion didn't yet
// rule out one more variable: whether the LEDC hardware fade ENGINE itself
// (ledc_set_fade_with_step()/ledc_fade_start(), as opposed to a plain
// ledc_set_duty()/ledc_update_duty() call) adds anything on its own,
// independent of how often it's invoked. (See the CORRECTION note below
// this block, though - the factor=4 test that motivated this one turned out
// to have been run with 'I' OFF by mistake, which changes the read on all
// of this - kept here verbatim as the reasoning that was live at the time,
// not edited away.)
//
// At FACTOR=1, every tick is already a "full" tick (no sub-tick
// interpolation exists to test), so this doesn't test interpolation at
// all - it tests the fade MECHANISM at the SAME 16kHz write rate as the
// already-confirmed-clean plain-write baseline. With ENVELOPE_INTERP_
// USE_HW_FADE also 1 (see below), 'I' OFF still takes the plain
// envelope_output_write_pwm() path (the confirmed-clean x1 baseline,
// unchanged), while 'I' ON now routes that same once-per-tick write
// through envelope_output_start_hw_fade(envelope, 1) instead - a single-
// step fade (scale = the full duty distance, cycle_num=1) that lands at
// essentially the same PWM-period boundary a plain ledc_update_duty()
// would anyway. Since 'I' is a live runtime toggle, both conditions can
// be A/B'd in ONE flash - no rebuild needed between them. If 'I' ON stays
// clean here, the fade engine itself is exonerated and the earlier
// degradation is specifically about write RATE (or ramping through
// intermediate values, which also doesn't happen at steps=1); if 'I' ON
// degrades even here, that implicates the fade mechanism itself.
//
// RESULT, 2026-09-08: 'I' ON at this x1/steps=1 config produces real,
// audible noise too - clearly less severe than the factor=4 case, but "bad
// enough" (user's own words) to matter. This is the clean, decisive part
// of today's testing (unlike the factor=4 result below, this run's 'I'
// on/off states were confirmed correct): even at a MATCHED 16kHz write
// rate, with dsp_task's wake rate/ISR overhead genuinely identical to the
// confirmed-clean baseline (FACTOR=1 here means the physical gptimer
// itself only fires at 16kHz, no throttling needed or happening), routing
// a single-step write through ledc_set_fade_with_step()/ledc_fade_start()
// instead of plain ledc_set_duty()/ledc_update_duty() is measurably worse.
// The LEDC hardware fade engine is not just "a software-overhead-free way
// to do the same electrical thing" - it does something the plain duty-
// write path doesn't, and that something costs real noise even in the
// single-step case. See group_delay_fit_notes.md's matching entry for the
// full corrected picture (this finding plus the factor=4 correction
// combine to make hardware fade look like the wrong direction generally,
// independent of rate).
//
// CORRECTED, 2026-09-08, later still: the line above ("its normal permanent
// value (4)") was WRONG - caught by the user asking a basic, sharp question
// ("Presumably the PWM does load the cpu even if it is not written to?")
// that prompted re-checking this file's actual state instead of assuming.
// This constant does NOT just set ENVELOPE_INTERP_FACTOR for when 'I' is
// explicitly on - in the current (non-ENVELOPE_INTERP_USE_HW_FADE) build,
// on_timer_alarm() (ssb_mic_test.ino) calls vTaskNotifyGiveFromISR() on
// EVERY real timer alarm unconditionally, with no gate on 'I' at all - so
// at FACTOR=4 the sample gptimer fires and wakes dsp_task 64,000 times/sec
// regardless of whether 'I' is on or off. That exact combination (FACTOR=4,
// 'I' off) was already confirmed, independently, TWICE - on 2026-09-07 via
// a direct 1-vs-4 real-hardware comparison ("even setting x4 Fs for interp
// causes audible burble... with I-off"), and again this session via the
// accidentally-mislabeled hardware-fade test - to burble on its own, with
// zero LEDC/envelope_interp involvement. So leaving this at 4 while relying
// on 'I' off at runtime does NOT reproduce the genuinely clean, >30dB-IMD-
// confirmed baseline this project has actually verified - only FACTOR=1
// does, because only then does the physical gptimer itself run at plain
// 16kHz. This project's own established convention (per the user's own
// 2026-09-07 correction: "Whenever I reported that I was running with
// I-off now as default is when I also set ENVELOPE_INTERP_FACTOR=1") has
// always been to treat 1 as the real resting/production value and to bump
// this to 4 (or higher) ONLY for a deliberate, temporary interpolation-
// related test build, reflashing back to 1 afterward - not the other way
// around. Restored to that actual correct resting value now.
//
// 2026-09-29, later still: bumped BACK to 4 for a deliberate, temporary
// test - see envelope_interp.h's ENVELOPE_ISR_INTERP_ENABLED comment
// below and moving_forward_notes.md's matching dated correction entry.
// This was left at 1 (the resting value above) through v6's entire build
// and first real-hardware test - meaning that test's "regression" never
// actually exercised 64kHz interpolation at all, only the new ISR-write
// MECHANISM at the unchanged 16kHz rate. This is the first build that
// actually runs the feature at its intended rate. Reflash back to 1
// afterward, same convention as every other temporary bump of this
// define - don't leave it here as a new silent default.
#define ENVELOPE_INTERP_FACTOR 4

// 2026-09-08: v5 experiment - "get x4 interp working at 16kHz" by
// removing the wake-rate cost entirely instead of trying to shrink it.
// See this file's v3 history above and group_delay_fit_notes.md's
// matching 2026-09-08 entry for the full derivation. When 1:
//   - on_timer_alarm() (ssb_mic_test.ino) only cross-core-notifies
//     dsp_task once every ENVELOPE_INTERP_FACTOR real timer alarms,
//     instead of every alarm - dsp_task goes back to being woken at
//     plain SAMPLE_RATE_HZ (16kHz), the confirmed-clean baseline rate,
//     even though the underlying gptimer hardware still physically fires
//     at ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ (64kHz) exactly as
//     today - only the notify-out-of-every-N changed, not the timer
//     config itself.
//   - envelope_interp_on_full_tick() (envelope_interp.cpp) issues ONE
//     ledc_set_fade_with_step()+ledc_fade_start() hardware fade command
//     per full tick instead of writing the duty register directly and
//     leaving envelope_interp_on_interp_tick() to walk a software ramp -
//     the LEDC hardware autonomously steps duty across the interior
//     ENVELOPE_INTERP_FACTOR sub-periods on its OWN clock, with zero
//     further CPU/ISR/task involvement. envelope_interp_on_interp_tick()
//     is simply never called under this mode (there's no wake event left
//     to call it from).
//   - init_sample_timer() additionally calls ledc_timer_rst() right after
//     gptimer_start() (see that function's own dated comment) so the
//     LEDC's own free-running fade-step clock starts from a known phase
//     relative to the gptimer, instead of an arbitrary power-on offset -
//     this is the piece v3 never had, and only works now because
//     RSET_MOD_LEDC_FREQ_HZ was retuned to 64000 (exactly ENVELOPE_
//     INTERP_FACTOR x SAMPLE_RATE_HZ, see envelope_output.h's own dated
//     comment) instead of the old arbitrary, non-commensurate 78125Hz.
//
// KNOWN LIMITATIONS, both deliberate, both worth having a real opinion
// on before flipping this to 1:
//   1. Curve shape is no longer selectable - ledc_set_fade_with_step()
//      is inherently a linear ramp from wherever the hardware currently
//      sits to the new target; the 'C' curve toggle (CATMULL_ROM/LINEAR/
//      HOLD) has no effect while this is on, since there's no software
//      ramp left to shape. Worth noting real hardware has NOT shown
//      Catmull-Rom beating linear in every test - the 2026-09-02 write-
//      rate-isolation table found LINEAR (+11.5dB degradation) actually
//      BEAT Catmull-Rom (+14.2dB) at 10kHz+factor4, so losing curve
//      choice isn't obviously a downgrade, just an honest tradeoff.
//   2. INCOMPATIBLE with CHIRP test mode ('w', AUDIO_SRC_CHIRP) as
//      implemented - that mode's own raw waveform generation deliberately
//      runs on every fast (64kHz) tick regardless of is_full_tick (see
//      dsp_task's chirp branch comment - a 20kHz chirp needs more than
//      SAMPLE_RATE_HZ's own 8kHz Nyquist), and the throttled-notify ISR
//      change above would silently starve it down to 16kHz too. Revert
//      this flag to 0 before running any chirp/TF-analyzer sweep.
//   3. From this file's own v3->v4 history above ("CORROBORATION, Fs-
//      jitter-hunt"): a separate experiment once found dsp_task
//      co-located with loop() on Core 1 starved Serial completely even
//      with 'I' OFF (no 64kHz wake at all) - meaning v3's true failure
//      mode was never conclusively pinned to wake-rate/CPU overhead
//      alone. This experiment does NOT move dsp_task's core (stays on
//      Core 0, exactly as v4) and does NOT add a second task or ISR - it
//      only changes what the EXISTING gptimer/dsp_task pairing does - but
//      that corroboration finding means "high wake rate was never fully
//      confirmed harmless" either. Treat this as a real experiment to
//      validate on the bench, not a guaranteed fix.
//
// Off by default. Not yet bench-verified in any form - flip to 1 here
// only alongside the matching #if blocks in envelope_output.h/.cpp and
// ssb_mic_test.ino (search this same flag name).
//
// UPDATE 2026-09-08: all the matching #if blocks this comment refers to are
// now actually written - envelope_output_start_hw_fade()/
// envelope_output_sync_ledc_timer_now() are implemented in
// envelope_output.cpp (including the required one-time
// ledc_fade_func_install() call in init_rset_mod_pwm()), and
// envelope_interp_on_full_tick() (envelope_interp.cpp) now calls the
// former instead of the software ramp when this flag is 1. The .ino's
// on_timer_alarm()/dsp_task throttled-notify changes were already in place
// from the step before this one. So flipping this to 1 now exercises the
// complete path, not a partial one - but two things specifically remain
// UNVERIFIED against real hardware/toolchain before trusting it:
//   - envelope_output_start_hw_fade()'s ledc_set_fade_with_step() call
//     was written against a REMEMBERED signature (speed_mode, channel,
//     target_duty, scale, cycle_num) - check that against your actual
//     installed ESP-IDF driver/ledc.h before flashing; a mismatch is a
//     clean compile error, easy to fix, but see that function's own
//     comment in envelope_output.cpp for the semantic risk if some IDF
//     version's `scale` parameter means something subtly different.
//   - the two known limitations and the unresolved CORROBORATION caveat
//     listed above are unchanged by having the code written - this is
//     still "a real experiment to validate on the bench, not a guaranteed
//     fix."
// See group_delay_fit_notes.md's matching 2026-09-08 entry.
//
// RESULT, 2026-09-08 (factor=4 test): burble/noise-floor degradation
// persists at essentially FULL severity vs. the x1 baseline, despite
// dsp_task's wake-rate overhead being completely eliminated - see the
// notes-file entry linked above for the full read (points at write/
// switching-rate, not CPU/timing, as the dominant mechanism).
//
// CORRECTION, 2026-09-08, same day: the factor=4 test result immediately
// above was run with 'I' OFF by mistake ("Sorry I did the first test
// incorrectly in that I left I off") - meaning envelope_interp_on_full_
// tick()'s HW_FADE branch was never actually reached (it sits after the
// `if (!s_enabled) return;` early-out), so that run never called
// envelope_output_start_hw_fade() even once. What it actually measured
// was: FACTOR=4 physically configures the gptimer at 8,000,000Hz
// resolution/64kHz alarm rate regardless of this flag, and the ISR-
// throttle logic here only decides whether to NOTIFY dsp_task on a given
// alarm - the raw gptimer ISR (on_timer_alarm(), ssb_mic_test.ino) still
// physically FIRES at 64kHz either way, just skipping the cross-core
// notify on 3 of every 4 calls. So the "burble persists at full severity"
// result was really showing that the bare 64kHz timer-ISR entry/exit
// alone - with NO cross-core notify/IPI on 3 of 4 calls, NO dsp_task wake
// on those calls, and NO LEDC write of any kind beyond one plain
// envelope_output_write_pwm() per real 16kHz full tick - is enough on its
// own to reproduce something close to the full symptom. That's actually a
// SHARPER version of the pre-existing "wake-rate-alone" mechanism from the
// 2026-09-07 investigation (which still had full notify/IPI/dsp_task-wake
// overhead on every one of the 4 ticks) - it narrows the culprit down
// specifically to the timer ISR itself firing that often, not the
// downstream notify/task-wake machinery. The user then re-ran this exact
// config with 'I' correctly ON: "With I on it completely destroys the two
// tones - very high noise levels" - categorically worse than the
// (mislabeled) 'I'-off run, confirming the hardware fade engine adds a
// large ADDITIONAL cost on top of the bare-ISR-rate effect once it's
// actually engaged. See group_delay_fit_notes.md's matching correction
// entry for the full three-way picture (bare-64kHz-ISR cost + fade-engine-
// inherent cost + likely rate-scaling of the latter across 4 steps/tick).
//
// Combined with the separate, cleanly-isolated x1/steps=1 result above
// (real audible noise even at a genuinely matched 16kHz rate with zero
// extra ISR overhead), the fade engine itself now looks like a real,
// independent noise source - not merely "the same electrical write, done
// with less CPU cost." CORRECTED BACK to its normal default (0) now that
// both halves of this test are in - hardware-fade-based interpolation is
// not currently a promising direction regardless of wake-rate cleverness;
// see the notes file for candidate next steps (predominantly: accept x1 on
// this output path, or reconnect the DAC).
#define ENVELOPE_INTERP_USE_HW_FADE 0

// 2026-09-29: v6 - the real, genuinely-integer fast-tick envelope write,
// built after ISR_PWM_FIXED_TEST_ENABLED's bench probe (config.h,
// ssb_mic_test.ino) confirmed a raw per-fast-tick LEDC register write
// from true ISR context fits the timing budget cleanly (three rounds of
// real-hardware iteration: an initial two-writer race mis-diagnosed as an
// LEDC low-speed-commit-trigger issue, then correctly traced to
// envelope_output_write_pwm()'s competing task-context write never having
// been silenced - see moving_forward_notes.md's 2026-09-29 entries for
// the full story). Where v1-v3 crashed or starved Core 1 trying to move
// EITHER the interpolation arithmetic OR a general LEDC driver call into
// the ISR, and v4/v4.x instead sped up dsp_task's own full-tick rate and
// left interpolation as a TASK-side software ramp (compute_ramp_value(),
// below) - which stopped working once AD9851_ISR_WRITE_ENABLED (config.h)
// started gating dsp_task's notify to full-tick boundaries only, making
// the task-side "interp tick" wakes this ramp depends on unreachable - v6
// splits the work instead: dsp_task (float-safe TASK context) computes a
// per-fast-tick duty STEP once per full tick via envelope_output_isr_
// stage_step() (envelope_output.h/.cpp), and a tiny IRAM_ATTR integer-
// only function, envelope_output_isr_fasttick_step(), applies that step
// and writes the raw LEDC register EVERY fast tick, called directly from
// on_timer_alarm() (ssb_mic_test.ino) - see those two functions' own
// comments for the fixed-point design.
//
// Bypasses compute_ramp_value() and the CATMULL_ROM/LINEAR/HOLD curve
// selection entirely - this mode is LINEAR-ONLY (a plain per-tick delta
// toward each new full-tick target), and IGNORES the runtime 'I' toggle
// (envelope_interp_set_enabled()) - see envelope_interp_on_full_tick()'s
// own comment for exactly where in that function this branch sits and
// why 'I' has no effect on the PWM leg while this flag is on. This is a
// deliberate scope choice, not an oversight: once this compile-time flag
// owns the PWM leg, the OLD software curve system it bypasses has nothing
// left to select between.
//
// REQUIRES AD9851_ISR_WRITE_ENABLED (config.h) - see the #error guard
// below. Both rely on the SAME on_timer_alarm() full-tick counter/notify-
// gating machinery; rather than generalize that machinery to a third
// independent condition (AD9851_ATTACHED && AD9851_ISR_WRITE_ENABLED) ||
// ENVELOPE_ISR_INTERP_ENABLED, which would need touching more of
// on_timer_alarm()'s/dsp_task's existing conditionals than this staged,
// bench-validated feature justifies, this mode is simply nested INSIDE
// the existing AD9851_ISR_WRITE_ENABLED block - correct for this
// project's actual hardware anyway, since real fast-tick envelope
// interpolation without also having the AD9851 ISR write active isn't a
// configuration this transmitter actually uses.
//
// NOT YET BENCH-VALIDATED - built directly on top of the fixed-value
// probe's clean timing result, but that probe never wrote a REAL,
// changing value every tick, only a constant. The per-tick ISR write cost
// should be near-identical (same instruction count either way - the
// value written doesn't change how long the write takes), but this has
// not itself been confirmed on a scope/bench yet. CHIRP mode is NOT
// compatible with this flag, same reason as AD9851_ISR_WRITE_ENABLED/
// ENVELOPE_INTERP_USE_HW_FADE (dsp_task isn't woken on interp ticks under
// any of these, capping CHIRP's fast per-tick waveform generation to
// SAMPLE_RATE_HZ instead of its intended 20kHz sweep range).
//
// 2026-09-29, later still: re-enabled for a genuine first test at
// ENVELOPE_INTERP_FACTOR=4 (see that define's own matching comment
// above) - the previous "confirmed regression" run (moving_forward_
// notes.md, same date) turned out to have been at FACTOR=1, which made
// it a test of the ISR-write mechanism at the unchanged 16kHz rate, not
// of interpolation at all. Revert to 0 and FACTOR back to 1 if this
// fresh test also shows a real-hardware problem - same revert instructions
// as before, still correct.
#define ENVELOPE_ISR_INTERP_ENABLED 1

#if ENVELOPE_ISR_INTERP_ENABLED && !(AD9851_ATTACHED && AD9851_ISR_WRITE_ENABLED)
#error "ENVELOPE_ISR_INTERP_ENABLED requires AD9851_ISR_WRITE_ENABLED (and AD9851_ATTACHED) - both config.h flags - see this flag's own comment above for why: they share the same on_timer_alarm() full-tick counter/notify-gating machinery, nested rather than generalized."
#endif
#if ENVELOPE_ISR_INTERP_ENABLED && ENVELOPE_INTERP_USE_HW_FADE
#error "ENVELOPE_ISR_INTERP_ENABLED and ENVELOPE_INTERP_USE_HW_FADE both attempt to solve fast-tick envelope smoothing a different way - pick exactly one."
#endif

// v4.3: which curve compute_ramp_value() evaluates over the [s_p1,s_p2]
// segment - see the "v4.3" header note above for the full rationale.
// CATMULL_ROM is 0 (not LINEAR) specifically so a PersistentSettings preset
// that doesn't explicitly set envelope_interp_curve (relying on C's
// zero-fill of trailing struct initializers) keeps today's actual live
// behavior, not v4's older one.
//
// v4.4: added HOLD - plain zero-order-hold at the fast-tick (64kHz) rate,
// writing each full tick's own value unchanged ENVELOPE_INTERP_FACTOR
// times instead of ramping toward the next one. Isolates a DIFFERENT
// question than LINEAR-vs-CATMULL_ROM does: does merely writing the LEDC
// duty register 4x more often matter at all (LEDC/driver-level timing
// effects), independent of any ramp shape - see the "v4.4" header note
// above. Zero added latency, unlike the other two curves (which both
// spend a tick arriving at their target) - HOLD writes the fresh value
// immediately, same timing as 'I' off, just repeated at 64kHz instead of
// written once at 16kHz.
typedef enum {
    ENVELOPE_INTERP_CURVE_CATMULL_ROM = 0,  // v4.2 (current default) - smooth cubic Hermite
    ENVELOPE_INTERP_CURVE_LINEAR      = 1,  // v4's original straight-line ramp, for direct A/B
    ENVELOPE_INTERP_CURVE_HOLD        = 2,  // v4.4 - plain 64kHz ZOH, no ramp, zero added latency
} envelope_interp_curve_t;

// Resets the ramp state. Call once from setup(), after envelope_output_
// init() (harmless either order now that there's no LEDC/gptimer setup
// left in here, but kept for call-site consistency with every other
// module's _init()).
void envelope_interp_init(void);

// Call once per "full" DSP tick (see header comment for what that means)
// with the newly computed, already-relative-delayed envelope value, and
// tick_start_us = esp_timer_get_time() captured at the TRUE top of this
// tick (before any DSP processing) - the ramp's time-accuracy depends on
// this being the real tick start, not a timestamp taken at this call site
// (which is reached only after the full DSP pipeline has already run) -
// see header comment. In place of calling envelope_output_write_pwm()
// directly, which this REPLACES at that call site (doesn't add a second
// one).
void IRAM_ATTR envelope_interp_on_full_tick(float envelope, int64_t tick_start_us);

// Call once per "interp" tick (the ENVELOPE_INTERP_FACTOR-1 fast ticks
// between each pair of full ticks) - no arguments, walks the ramp one
// more sub-step internally. No-op when disabled.
void IRAM_ATTR envelope_interp_on_interp_tick(void);

bool envelope_interp_get_enabled(void);

// On an off->on transition, seeds the ramp's prev/target from the last
// submitted value (not zero) and marks it already-arrived, so the very
// next full tick starts a normal, correctly-anchored ramp instead of
// gliding from a stale or zero value - see header comment. A no-op
// transition (already in the requested state) does nothing, same
// convention as envelope_gdeq_set_enabled().
void envelope_interp_set_enabled(bool enable);

// v4.3: which curve is currently selected - see the "v4.3" header note
// above. Only affects rendered output while envelope_interp_get_enabled()
// is true.
envelope_interp_curve_t envelope_interp_get_curve(void);

// v4.3: switch curves live. No reseed/transient handling needed (unlike
// envelope_interp_set_enabled()) - see header note for why a mid-ramp
// switch is safe as a plain store.
void envelope_interp_set_curve(envelope_interp_curve_t curve);
