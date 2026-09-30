#pragma once

/**
 * config.h
 *
 * Shared compile-time configuration for the ssb_mic_test sketch. Split out
 * of ssb_mic_test.ino during the module-split refactor (the .ino was
 * getting hard to navigate at ~1900 lines) purely to collect the #defines
 * that more than one .cpp module needs in a single place - none of the
 * *values* below changed as part of that refactor. If you're hunting for a
 * define that used to live directly in the .ino, it's almost certainly
 * here now; a handful of genuinely module-private defines (ADC/PWM/DAC
 * pin & peripheral config, isolation-test-specific tuning that only one
 * .cpp touches) moved into that module's own header instead - see
 * adc_capture.h, envelope_output.h, carrier_output.h, relative_delay.h,
 * envelope_gdeq.h.
 *
 * IMPORTANT: include this before any other project header in every
 * translation unit that references AD9851_ATTACHED, PWM_COMPARISON_ENABLED,
 * etc. - several of those gate #if blocks in OTHER headers (in particular,
 * PWM_COMPARISON_ENABLED must be defined before "driver/ledc.h" is
 * included anywhere, same constraint the original .ino had; SDM_COMPARISON_
 * ENABLED below has the identical constraint against "driver/sdm.h").
 */

#include <stdint.h>

// ---- Set to 1 once the AD9851 board is wired up and its driver calls are
// filled in. Until then this runs mic->DSP->DAC standalone. ----
#define AD9851_ATTACHED 1

// ---- 2026-09-19: fold-back of the write-time-jitter fix - see
// null_bias_investigation.md/moving_forward_notes.md's 2026-09-19 entries
// for the full story. Short version: the long-standing ~7.7Hz two-tone
// comb was traced to the Hilbert/DSP chain's own execution time jittering
// (mode-dependently) at ~7.7Hz, which turns into RF-domain phase/frequency
// noise because the AD9851/envelope writes happen at the END of each tick,
// right after that jittery work finishes - the WALL-CLOCK MOMENT those
// writes occur (and therefore when the AD9851's frequency actually
// changes) jitters by the same amount relative to the one rock-solid
// periodic reference in this system, the gptimer alarm. Bench-confirmed on
// minimal_fs_test_step5_resync/step6_isr_write to eliminate the comb (no
// sign of it in EXACT/LEGACY/MOD160 alike) with no measured downside.
//
// Two independent compile-time flags, both default ON now that the fix is
// confirmed - see ssb_mic_test.ino's dsp_task()/on_timer_alarm() for where
// each is actually used:
//
// TX_WRITE_RESYNC_ENABLED - buffer this tick's already-delay-line-adjusted
// envelope+freq_dev result and write BOTH at the START of the next tick
// instead of the end of this one, re-syncing the write's wall-clock timing
// to the gptimer alarm edge rather than to the variable-duration DSP
// work's own completion time. Adds exactly one sample period
// (SSB_SAMPLE_PERIOD_US) of common output latency - envelope and frequency
// are delayed together so relative_delay_apply()'s existing calibration
// between them is unaffected, only their shared latency increases.
//
// AD9851_ISR_WRITE_ENABLED - goes a step further for the AD9851 leg only:
// write it directly from the gptimer alarm ISR (on_timer_alarm(), Core 1)
// instead of from dsp_task, removing the cross-core ISR-to-task hand-off
// latency entirely - this is what actually closed the gap on the bench
// (resync alone only helped ~10dB under LEGACY, ~1dB under MOD160; adding
// this fixed all three modes). Deliberately does NOT extend to
// envelope/PWM - see ssb_mic_test.ino's on_timer_alarm() for the
// project-history reasons (three prior Core0/Core1 ISR failures, one of
// them - the SDM stage/commit-from-ISR attempt - directly on point and
// REVERTED after real hardware measured WORSE, not better). Compile-time
// only, deliberately not a runtime toggle - if anything on Core 1
// misbehaves, Serial itself dying outright is the historical canary (see
// the two "TRIED, REVERTED" Core0/Core1 entries in dsp_task()'s own
// comment), and a runtime toggle sent over Serial might not even be
// reachable at that point. Set to 0 and reflash for a guaranteed,
// code-level revert - with it at 0, behavior is identical to
// TX_WRITE_RESYNC_ENABLED alone (task-side resync for both signals).
//
// 2026-09-29 UPDATE: used to REQUIRE ENVELOPE_INTERP_FACTOR==1 (envelope_
// interp.h), enforced by a hard #error guard in ssb_mic_test.ino - because
// with FACTOR>1 the ISR fires faster than the true per-sample rate and
// couldn't tell, by itself, which firing is a genuine full-tick boundary,
// so writing the AD9851 on every firing would both write stale values most
// of the time and risk the write not finishing before the next alarm. That
// restriction is LIFTED now: on_timer_alarm() (ssb_mic_test.ino) tracks its
// own local full-tick counter (s_isr_full_tick_counter, plain
// increment/compare/wrap, same ISR-safe shape as ENVELOPE_INTERP_USE_HW_
// FADE's own counter) and gates BOTH the AD9851 write and the cross-core
// notify-to-dsp_task on it - so this is correct at any FACTOR, not just 1.
// At FACTOR==1 the counter wraps every firing, so behavior is bit-for-bit
// identical to before this change. dsp_task's own is_full_tick check was
// extended to match (see its "ENVELOPE_INTERP_USE_HW_FADE ||
// AD9851_ISR_WRITE_ENABLED" condition) - dsp_task's fast_tick_count/
// last_full_group group-math is bypassed under this flag, same as it
// already was under ENVELOPE_INTERP_USE_HW_FADE, since there's nothing
// left for it to compute once the ISR itself only notifies on full ticks.
//
// Gating the notify to full-tick-only is also, as a side effect, the fix
// for the wake-rate "burble" confirmed on real hardware 2026-09-07 (simply
// waking dsp_task 64,000 times/sec - even doing nothing on most wakes -
// was itself costly, independent of any interpolation arithmetic) - see
// moving_forward_notes.md's 2026-09-29 entries for the full reasoning and
// null_bias_investigation.md's open item 5 for a possible connection to an
// earlier, separately-reported "'I' makes things worse" finding.
//
// IMPORTANT, staged scope: this fix does NOT touch envelope/PWM
// interpolation itself - envelope still updates only once per full tick
// (held between them), exactly as coarse as FACTOR==1 always gave it. The
// existing envelope_output_write_pwm() (envelope_output.cpp) does a float
// multiply AND general ledc_set_duty()/ledc_update_duty() driver calls -
// BOTH of the exact hazards that crashed v1 and v2 in envelope_interp.h's
// own documented history - so it is NOT safe to call from this ISR as-is.
// A real envelope fast-tick write needs its own new, genuinely integer-
// only implementation (precompute the per-step duty increment in dsp_task,
// where float is fine, then have the ISR do only an integer add plus a raw
// LEDC register poke) - not yet built. Until then, raising FACTOR gets you
// a faster, gated ISR and correct AD9851 timing, but no ZOH-imaging
// benefit from interpolation yet - that's the next, separate step.
//
// CHIRP mode (AUDIO_SRC_CHIRP) is NOT compatible with this flag at
// FACTOR>1, for the same reason ENVELOPE_INTERP_USE_HW_FADE already
// wasn't: its fast per-tick waveform generation only runs when dsp_task is
// actually woken, so gating the notify to full-tick boundaries caps it to
// SAMPLE_RATE_HZ (16kHz) instead of its intended 20kHz-sweep-capable rate.
#define TX_WRITE_RESYNC_ENABLED     1
#define AD9851_ISR_WRITE_ENABLED    1

// 2026-09-30: ISR_NOTIFY_BEFORE_AD9851_ENABLED - EXPERIMENT, not bench-tested.
// Suggested by the user after the ISR's AD9851 bit-bang was measured at
// ~13.6us per full tick (moving_forward_notes.md, 2026-09-30 "is 13.6us a
// sensible time" entry). Until now on_timer_alarm() ran that write BEFORE
// vTaskNotifyGiveFromISR(), so dsp_task couldn't even start until the write
// finished - the write time was added straight onto dsp_task's start time,
// eating into the 62.5us tick budget (estimated worst-case margin only
// ~3-5us once cross-core wake latency is included - an estimate, not a
// measurement).
//   0 = previous behaviour: AD9851 write, then notify dsp_task.
//   1 = notify dsp_task FIRST, then do the AD9851 write (same value, same
//       stale-commit accounting, same pin-5 pulse - only the order changes).
//       dsp_task's start moves ~10us earlier, so its finish-before-next-tick
//       margin should grow by about that much.
// KNOWN SIDE EFFECT to watch (reasoned, not measured): dsp_task's task-side
// envelope/PWM write (envelope_interp_on_full_tick(), first thing it does)
// now lands BEFORE the AD9851's FQ_UD latch instead of ~10us AFTER it, i.e.
// the relative timing between the carrier-frequency update and the envelope
// update shifts by roughly the AD9851 write duration (~0.16 of a 62.5us
// sample). This project has been sensitive to that relationship before (see
// the #error text just below and the relative_delay notes) - compare
// IMD/noise before judging this on timing margin alone. Other risks: both
// cores now touch the GPIO/LEDC peripheral bus at the same time (ISR
// bit-bang on Core 1, dsp_task on Core 0), which could stretch the ISR's own
// ~13.6us (watch pin 5 low width / ad9851 max_spi_us) - unmeasured.
#define ISR_NOTIFY_BEFORE_AD9851_ENABLED   1

#if AD9851_ISR_WRITE_ENABLED && !AD9851_ATTACHED
#error "AD9851_ISR_WRITE_ENABLED requires AD9851_ATTACHED - there's no AD9851 to write to otherwise."
#endif
#if AD9851_ISR_WRITE_ENABLED && !TX_WRITE_RESYNC_ENABLED
#error "AD9851_ISR_WRITE_ENABLED requires TX_WRITE_RESYNC_ENABLED - the ISR write reads a buffer that's only ever staged/valid under the resync scheme, and writing AD9851 from the ISR without also resyncing envelope/PWM would reintroduce the two writes' calibrated relative timing being lost."
#endif

// ---- TEMPORARY DIAGNOSTIC, not a real feature: write a FIXED (constant,
// dummy) value to the RSET/PWM LEDC duty register on EVERY fast tick,
// directly from on_timer_alarm() (ssb_mic_test.ino), unconditionally -
// i.e. NOT gated on is_full_tick, since the point is to test the worst
// case "a register write on every single fast tick" scenario. This is a
// pure timing-budget probe, asked for 2026-09-29 as the deliberate next
// step before writing the real integer-only envelope fast-tick
// interpolation math: if the ISR can absorb this extra write and timing
// stays plausible, that's good evidence the real (integer add + same
// register poke) version will also fit.
//
// WHILE THIS FLAG IS 1, REAL ENVELOPE OUTPUT IS OVERRIDDEN WITH A
// CONSTANT ON EVERY FAST TICK - the RSET/PWM pin will NOT reflect actual
// modulation. Do not use for over-the-air testing. Pure-tone/timing
// checks only; confirm on a scope (RSET_MOD_LEDC_GPIO) that the pin
// actually sits at the fixed test duty, then turn this back off.
//
// Uses RAW LEDC peripheral struct access (soc/ledc_struct.h's LEDC.*),
// NOT the ledc_set_duty()/ledc_update_duty() driver calls - those are
// confirmed NOT ISR-safe (see envelope_output_write_pwm()'s own comment
// and envelope_interp.h's v1/v2 history: general driver calls from a
// true ISR is exactly what rebooted v1). The raw register field names
// used (LEDC.channel_group[...].channel[...].duty.duty, .conf1.
// duty_start, .conf0.low_speed_update) are from general ESP32/S2/S3 LEDC
// peripheral knowledge, NOT verified against this project's exact
// installed ESP-IDF/Arduino-ESP32 SDK version (no local toolchain
// available to check in the environment this was written in). If it
// fails to compile, that pinpoints exactly which field name needs
// correcting for this SDK version - share the compiler error and it can
// be fixed from that.
//
// 2026-09-29, IMPORTANT CORRECTION after the first two bench results:
// this flag now ALSO silences envelope_output_write_pwm()'s normal
// task-context write (see that function's own comment, envelope_output.
// cpp) while active. It didn't originally - the first two bench builds
// left dsp_task's real per-full-tick write running unchanged, so it kept
// fighting this ISR write for the same duty register, and the pin
// alternated between the fixed test value and real envelope content on a
// ~10ms cycle instead of holding solidly (far slower than either the
// 15.6us fast-tick or 62.5us full-tick period, so not a simple per-tick
// race - see moving_forward_notes.md's matching entries for the full
// investigation, including a since-refuted LEDC low-speed-commit-trigger
// theory that didn't change the ~10ms period at all). Silencing the
// competing writer here makes this a genuine single-writer test: if the
// pin now holds rock solid, the two-writer race was the whole story; if
// it's STILL alternating with only this ISR write active, something else
// (most likely the ISR/gptimer itself stalling for multi-ms stretches,
// not a register-arbitration issue) is going on and needs the two-
// channel TIMING_DEBUG_GPIO_ISR-vs-RSET_MOD_LEDC_GPIO scope correlation
// recommended in that entry.
#define ISR_PWM_FIXED_TEST_ENABLED   0   // 0 = normal operation (default); 1 = timing probe only, see above
#define ISR_PWM_FIXED_TEST_DUTY      512 // arbitrary mid-scale value, 0..1023 (RSET_MOD_LEDC_RES = 10-bit)

#if ISR_PWM_FIXED_TEST_ENABLED && (ISR_PWM_FIXED_TEST_DUTY < 0 || ISR_PWM_FIXED_TEST_DUTY > 1023)
#error "ISR_PWM_FIXED_TEST_DUTY must fit RSET_MOD_LEDC_RES's 10-bit range (0..1023, see envelope_output.h)."
#endif

// ---- Two-tone test mode: bypass the mic ADC with a synthesized signal.
// Zero-hardware smoke test of the DSP chain. ----
#define TWOTONE_TEST_MODE   1  // testing default - two-tone on at boot
#define TWOTONE_F1_HZ        700.0f
// 2026-09-16: found this had drifted back to 1900.0f (the legacy, wide,
// NOT-Fs-commensurate pair - see TWOTONE_BAND_PRESETS[]'s own comment in
// test_signals.cpp and moving_forward_notes.md's 2026-09-09 "retire
// 700/1900Hz" entry) via commit 8fd8349 ("Add dither on tone2... current
// 2tone set to 700/1900", 2026-09-12), which silently undid de32ffd's
// earlier "Change tone pair to 700/1700 so syncs with Fs". This repo clone
// had been sitting on the stale 1900 value ever since - unnoticed because
// every capture analyzed in this file and null_bias_investigation.md since
// 2026-09-12 was actually run against the user's own bench firmware, whose
// config.h they confirmed (2026-09-16 bench session) still reads 700/1700.
// Restored here to match the bench and stop this repo clone silently
// disagreeing with reality - see this date's null_bias_investigation.md
// entry for the full story and why it mattered for that session's
// held_trace/slow_trace interpretation.
#define TWOTONE_F2_HZ        1700.0f
#define TWOTONE_AMPLITUDE    0.45f   // keep below 0.5 so peaks don't clip when summed

// ---- 2026-09-11: two-tone null-uncertainty dither ('Q') - see
// test_signals.h/.cpp for the mechanism and null_bias_investigation.md's
// "Root mechanism identified" section for why it might help: these test
// tones are exact phase-accumulator multiples of SAMPLE_RATE_HZ, so every
// null in a run recurs at an IDENTICAL alignment to the sample grid - the
// tiny per-null dphi-resolution bias then accumulates coherently instead
// of averaging out the way real voice's randomly-timed nulls do. Off by
// default so plain 't'/'T'/'R' two-tone behavior is completely unchanged
// unless 'Q' is explicitly pressed. UNTESTED ON REAL HARDWARE.
#define TWOTONE_DITHER_ENABLED   0     // 'Q' toggles this at runtime; this is just the boot default
#define TWOTONE_DITHER_MAX_HZ    0.05f  // +/- excursion applied to tone2 only - small enough to stay
                                        // well under typical two-tone spectrum-analyzer resolution
                                        // bandwidth, large enough to fully re-walk a null's sample-
                                        // grid alignment over a several-second integration window
#define TWOTONE_DITHER_UPDATE_HZ 4.0f  // how often a fresh random target is drawn; the offset then
                                        // ramps LINEARLY toward it sample-by-sample in between, so
                                        // the instantaneous tone2 frequency varies continuously, not
                                        // in steps - avoids adding a second, different exact
                                        // periodicity of its own on top of the one being broken up
#define SINGLETONE_HZ        1000.0f  // a clean, unambiguous default - see generate_singletone_sample()
#define SINGLETONE_AMPLITUDE 0.7f     // single tone alone - more headroom available than the
                                      // two-tone sum needs, comparable to a moderately hot mic level

// ---- Pre-Hilbert audio conditioning (HPF + presence EQ + compressor) ----
// See ssb_dsp.h's ssb_audio_fx_config_t for the individual parameters, set
// in dsp_cfg.audio_fx in setup(). Leave off with TWOTONE_TEST_MODE if you
// want to look at the raw DSP chain's spurious performance without any
// conditioning in the signal path.
#define AUDIO_FX_ENABLED 1  // was 0 (pending re-tuning) - now toggleable live per-stage via serial
                             // 'e' (EQ) / 'c' (compressor) commands, see serial_commands.cpp - flip
                             // back to 0 only if you want the whole subsystem compiled out entirely
#define MASTER_GAIN_STEP_DB 1.0f  // per '+'/'-' keypress - see ssb_dsp_set_master_gain_db()
#define MASTER_GAIN_FINE_STEP_DB 0.1f  // per '.'/',' keypress - same idea, finer resolution for
                                        // dialing in precise single-tone gain-sweep measurement
                                        // points (e.g. envelope_predistort.h's calibration table)
                                        // without needing 10 presses of '+'/'-' to move 1dB

// 2026-09-25: Mic Gain step, per 'U'/'Y' keypress - see
// ssb_dsp_set_mic_gain_db()'s doc comment in ssb_dsp.h for why this
// control exists (it's the "normalize input before the compressor" stage
// this project was missing). 1.0dB matches MASTER_GAIN_STEP_DB's
// resolution - no separate fine-step variant yet, add one the same way as
// '.'/',' above if bench use shows a need for finer control.
#define MIC_GAIN_STEP_DB 1.0f  // per 'U'/'Y' keypress - see ssb_dsp_set_mic_gain_db()

// 2026-09-25: Mic Squelch threshold step, per '('/')' keypress - see
// ssb_dsp_set_squelch_enabled()'s doc comment in ssb_dsp.h. Threshold is
// linear (full-scale-referenced), not dB, so this is a plain additive
// step, not a multiplicative one - matches ENV_FLOOR_STEP's own
// resolution (envelope_floor.cpp), chosen there to sit close to the
// active PWM output path's own quantization step.
#define SQUELCH_THRESHOLD_STEP 0.001f  // per '('/')' keypress - see ssb_dsp_set_squelch_threshold()

// Mic-path DC-blocking single-pole filter's time constant, in seconds -
// dsp_task computes dc_alpha = expf(-1.0f / (SAMPLE_RATE_HZ *
// DC_BLOCK_TIME_CONSTANT_S)) from this at startup, instead of hardcoding
// the per-sample coefficient directly, so it keeps the same real-world
// cutoff regardless of SAMPLE_RATE_HZ. Value derived from the original
// hardcoded dc_alpha=0.995 @ 10000Hz: tau = -1/(Fs*ln(alpha)) =
// -1/(10000*ln(0.995)) ~= 0.019950s.
#define DC_BLOCK_TIME_CONSTANT_S 0.019950f

// ---- PWM comparison path enable flag. Defined here (before any header
// that depends on it) rather than down in envelope_output.h - #if needs
// this to already be known wherever "driver/ledc.h" gets included. ----
#define PWM_COMPARISON_ENABLED 1

// ---- SDM (Sigma-Delta Modulation) comparison path enable flag, 2026-09-08.
// Same reasoning/placement as PWM_COMPARISON_ENABLED above - defined here so
// it's already known wherever "driver/sdm.h" gets included (envelope_output.cpp).
// A third envelope-to-analog leg, alongside the PWM/RC path and the
// (currently disconnected) MCP4725 DAC path - see envelope_output.h's SDM
// section and pwm_envelope_interpolation_report.md's Section 3 for why this
// was originally set aside (8-bit signed density, recommended +/-90 usable
// range - a real resolution cut vs. LEDC's 10-bit duty) and why it's being
// tried on real hardware anyway now, on the reasoning that the only way to
// really know is to try it. Off by default - this is a fresh, NOT YET
// BENCH-VERIFIED driver; flip to 1 once SDM_OUT_GPIO (envelope_output.h) is
// wired to its own filter/scope point.
#define SDM_COMPARISON_ENABLED 0

#define dac_task_enabled 0

// ---- Master enable for the ADC continuous-mode driver (adc_capture_init()/
// adc_capture_service(), adc_capture.cpp). Set to 0 to skip starting the
// driver entirely - not just gate a debug pin, actually never call
// adc_continuous_start()/register the on_conv_done callback, so its ISR
// never fires at all. Added as an isolation test for the GPIO13 5kHz
// pulse investigation (see ssb_mic_test_commands.md): the pulse rate
// matches ADC_CONT_SAMPLE_FREQ_HZ/ADC_CONT_FRAME_SAMPLES (80000/16=5000Hz)
// exactly, and adc_capture.cpp's own pin13 debug writes are already
// confirmed compiled out (ADC_ISR_DEBUG_PIN_ENABLED=0) - so the working
// theory is electrical coupling from the ADC driver's real DMA/ISR
// activity onto the GPIO13 pad, independent of any code touching that pin.
// Flip this to 0 (with the source in a mode that doesn't need the mic -
// two-tone or chirp) and rescope the affected pin: if the 5kHz pulse
// disappears, that confirms the ADC driver as the source rather than some
// other 5kHz-ish activity - CONFIRMED on real hardware 2026-09-02, which is
// also why TIMING_DEBUG_GPIO_ADC/CHIRP_REF_GPIO moved off GPIO13 to GPIO39
// (see that define's own comment) - this flag stays for any future
// pin/coupling investigation of the same shape. Defaults to 1 (normal
// operation, no behavior change)
// - mic-source audio silently reads as 0.0f (matching the existing
// AMTEST/FMTEST/ENVSTEP "unused sample" convention) when disabled, rather
// than calling into a driver that was never started.
#define ADC_CAPTURE_ENABLED 1

// ---- Timing debug pin: toggled high at the start of dsp_task's real work
// and low at the end, so a scope on this pin directly measures the actual
// loop iteration time on real hardware - much more trustworthy than
// estimating it. Set to 0 to remove once you've got your measurement. ----
#define TIMING_DEBUG_ENABLED 1
#define TIMING_DEBUG_GPIO     4   // within the board's easy-access GPIO1-13 range; not otherwise used

// ---- Fs jitter hunt: two ADDITIONAL debug pins, both raw hardware-
// register toggles (GPIO.out_w1ts/w1tc, not digitalWrite - see the .ino's
// on_timer_alarm() and adc_capture.cpp's adc_conv_done_cb for why: these
// fire from REAL ISR context, unlike TIMING_DEBUG_GPIO above which only
// ever toggles from dsp_task, a task context) - purely diagnostic,
// intended to be scoped ALONGSIDE TIMING_DEBUG_GPIO to localize where the
// [timing] wakeup-jitter figure actually originates:
//   - rock-solid periodic on GPIO_ISR, jitter only shows up on
//     TIMING_DEBUG_GPIO -> the jitter is purely in the cross-core
//     notify/hand-off from gptimer's ISR (Core 1) to dsp_task (Core 0).
//     TRIED co-locating them on Core 1 instead to eliminate this hand-off
//     entirely (see the .ino's dsp_task creation site) - real hardware
//     starved Serial completely, reverted; root cause not yet understood,
//     so this hand-off is still live and still the leading jitter suspect.
//   - GPIO_ISR itself already jitters -> something on Core 1 is delaying
//     gptimer's alarm ISR from firing promptly in the first place, and
//     GPIO_ADC is there to test the leading suspect: adc_continuous's own
//     on_conv_done ISR (also Core 1, firing every ADC_CONT_FRAME_SAMPLES/
//     ADC_CONT_SAMPLE_FREQ_HZ = 16/80000 = 200us = 5000Hz right now) -
//     correlate its edges against GPIO_ISR's jitter to test ISR-to-ISR
//     contention on Core 1 as the root cause.
// Both pulse LOW-then-HIGH once per ISR firing (clear, then immediately
// set - not a toggle/flip) so every single ISR call produces the same
// falling-then-rising edge pair and a scope triggering on either edge
// catches every firing, not just every other one. It's the EDGE timing
// that matters here, not the pulse width (which is just however long the
// two back-to-back register writes take - negligible, sub-100ns).
#define TIMING_DEBUG_GPIO_ISR  5   // toggled every gptimer alarm ISR fire (on_timer_alarm(), Core 1)

// 2026-09-18: 7.7Hz two-tone comb hunt. Real hardware now shows a genuine
// ~7.69Hz component on BOTH TIMING_DEBUG_GPIO (pin4) and TIMING_DEBUG_GPIO_ISR
// (pin5) - confirmed by the user with a 256k-point FFT at 48kHz Fs on each
// pin in turn - i.e. dsp_task's own real-time cadence is what's carrying it,
// not something external. diagnostics_record_tick_start()/
// diagnostics_record_phase_timings() (diagnostics.cpp - the two always-on,
// every-tick hot-path calls feeding the [timing] block's high-water marks)
// are a documented prior suspect for exactly this kind of thing:
// diagnostics.h's own history notes a confirmed case of extra per-tick
// Core-1 instrumentation regressing pin5's period, fixed at the time by
// reverting it without ever pinning down why. Separately, `'v'`/`'M'`
// (2026-09-18, earlier) already ruled out the PRINT/auto-dump side of Core 1
// diagnostics as a cause - but those toggles never touched these two
// unconditional per-tick calls, so this is a genuinely untested layer.
// Set to 0, rebuild/reflash, and rescope pin4/pin5 for the ~7.7Hz component:
// if it changes or disappears, this recording layer is implicated. Leave at
// 1 for normal use - the periodic [timing] block has nothing to print
// without it (high-water marks/overrun_count all stay at their initial
// values). See moving_forward_notes.md/null_bias_investigation.md for the
// full investigation.
#define DIAG_HOTPATH_RECORDING_ENABLED 1

// 2026-09-02: GPIO_FAST_SET/CLR - IRAM-safe register-level GPIO set/clear
// that works across the FULL GPIO0-48 range on the S3. GPIO.out_w1ts/w1tc
// (used directly at every raw-register toggle site below and in
// adc_capture.cpp/the .ino) are only 32 bits wide and physically cover
// GPIO0-31 - `1UL << 39` doesn't address a real bit in that register at
// all, so a plain `GPIO.out_w1ts = (1UL << pin)` for any pin >= 32 silently
// does nothing (or hits the wrong pin, depending how the shift amount gets
// handled) rather than erroring at compile time. This is exactly what
// happened when CHIRP_REF_GPIO/TIMING_DEBUG_GPIO_ADC moved from pin13 to
// pin39 - the pin read permanently LOW on the scope because the toggle was
// silently a no-op, not because anything was electrically wrong. GPIO32+
// needs the separate GPIO.out1_w1ts/out1_w1tc register bank instead, at
// bit (pin-32). These macros pick the right register at compile time (pin
// is always a #define constant here, so the branch folds away - zero
// runtime cost, still ISR-safe) so this can't silently break again the
// next time a pin gets reassigned.
// NOTE: GPIO.out_w1ts/out_w1tc (low bank, pin<32) accept a plain integer
// assignment on this SDK, but GPIO.out1_w1ts/out1_w1tc (high bank, pin>=32)
// are anonymous UNIONS here, not plain uint32_t - real hardware compile
// (2026-09-02, esp32s3-libs 3.3.8) rejected `GPIO.out1_w1ts = mask` outright
// ("no match for operator=") until routed through the union's `.val` member
// instead. Left the low-bank assignments bare (already proven to compile)
// rather than risk "fixing" something that wasn't broken.
#define GPIO_FAST_SET(pin)  do { if ((pin) < 32) { GPIO.out_w1ts  = (1UL << (pin));      } \
                                  else            { GPIO.out1_w1ts.val = (1UL << ((pin)-32)); } } while (0)
#define GPIO_FAST_CLR(pin)  do { if ((pin) < 32) { GPIO.out_w1tc  = (1UL << (pin));      } \
                                  else            { GPIO.out1_w1tc.val = (1UL << ((pin)-32)); } } while (0)

// ADC_ISR_DEBUG_PIN_ENABLED gates adc_conv_done_cb()'s own toggle of
// TIMING_DEBUG_GPIO_ADC (adc_capture.cpp) - set to 0 here because this pin
// is being TEMPORARILY REPURPOSED for the serial-activity-correlation test
// (TIMING_DEBUG_GPIO_CMD below) instead: the ADC-ISR-vs-gptimer-alarm-ISR
// contention question this pin was originally added to test is already
// answered and fixed (intr_priority=3). Two ISRs (adc_conv_done_cb() and
// the new loop()-context command-window marker) must never drive the same
// physical pin at once - that would corrupt both signals - so this flag
// keeps them mutually exclusive. Flip back to 1 (and TIMING_DEBUG_GPIO_CMD
// back to its own pin, once one becomes available) if the ADC-ISR
// contention question ever needs re-checking directly.
#define ADC_ISR_DEBUG_PIN_ENABLED 0
#define TIMING_DEBUG_GPIO_ADC  39  // toggled every ADC conv_done ISR fire (adc_conv_done_cb(), Core 1).
                                   // TRIED GPIO3 FIRST, REVERTED: real hardware confirmed the ADC
                                   // continuous driver produced ZERO conversions the moment this pin's
                                   // toggling was enabled (actual sps=0, callbacks=0, both mic and
                                   // two-tone mode) - setting TIMING_DEBUG_ENABLED to 0 (disabling all
                                   // three debug pins) immediately restored real ADC data, and pins 4/5
                                   // were already proven safe from earlier captures this same session,
                                   // isolating GPIO3 as the cause by elimination. No confirmed mechanism
                                   // for WHY - GPIO3 is ADC1_CH2 on the S3, a DIFFERENT channel from the
                                   // mic's ADC1_CH5 (GPIO6), so driving it digitally shouldn't in theory
                                   // disturb a different channel's sampling - but the correlation was
                                   // clean enough not to trust that theory over the real hardware result.
                                   // Moved off the whole ADC1 channel range (GPIO1-10) to GPIO13 (ADC2)
                                   // for that reason - but GPIO13 turned out to have the OPPOSITE problem:
                                   // 2026-09-02, real hardware confirmed a regular 5kHz pulse on GPIO13
                                   // matching ADC_CONT_SAMPLE_FREQ_HZ/ADC_CONT_FRAME_SAMPLES exactly
                                   // (80000/16=5000Hz) - the ADC continuous driver's own DMA/ISR activity
                                   // electrically coupling onto the pin, confirmed by adding
                                   // ADC_CAPTURE_ENABLED (below) and showing the pulse vanishes when the
                                   // driver never starts. So GPIO1-20 (ADC1 AND ADC2, both directions)
                                   // are now a demonstrated-bad pin class on this board for anything that
                                   // either drives noise into the ADC (GPIO3) or needs to stay quiet from
                                   // it (GPIO13) - see ssb_mic_test_commands.md for the full writeup.
                                   // Moved again, this time off the ADC entirely: GPIO39 has no ADC
                                   // channel on the S3 (ADC1 stops at GPIO10, ADC2 at GPIO20), isn't one
                                   // of the four strapping pins (0/3/45/46), isn't the UART0 console
                                   // (43/44), and isn't already wired to the I2C DAC (47/48) - it's the
                                   // default JTAG TCK line, but this project doesn't use external
                                   // hardware JTAG (debugs over USB CDC serial), so that's free to reuse.
                                   // Reachable on this board (ESP32-S3 Super Mini) via its small solder
                                   // pads (39-48), not the main 1-13 header.

// New leading suspect for the [timing] wakeup-jitter residual and the
// pin5-bad-edge sightings that showed up even at the intr_priority=3-only
// checkpoint (no elapsed_fast_ticks/coalescing/residual diagnostic code
// present at all): real hardware now shows pin5 running clean for minutes
// at a stretch, then throwing an occasional bad edge mid-period right
// around when something happens on the serial interface (typing a
// command), then going quiet again once the serial activity stops - a
// strong hint this was there all along and just wasn't sampled during the
// short post-fix checkpoint window, not something the diagnostics code
// caused or something that regressed. Mechanism theory, unconfirmed:
// same ISR-to-ISR contention pattern that intr_priority=3 fixed against
// the ADC's on_conv_done ISR, but this time against USB-CDC/UART RX
// interrupt activity (and/or a Serial.printf command-confirmation write)
// on Core 1 - either delaying gptimer's alarm ISR entry directly, or
// (more likely, since intr_priority=3 should already preempt a lower/
// equal-priority interrupt) briefly holding interrupts masked in a
// driver-level critical section that blocks even a higher-priority ISR
// from firing until it's released. Bracketing handle_serial_commands()
// with its own debug pin gives a precise "command being processed right
// now" window to scope alongside pin5, instead of relying on "roughly
// when I typed" - see the .ino's loop() and setup().
//
// Reuses TIMING_DEBUG_GPIO_ADC's physical pin (39, see that define's own
// comment for why it lives there now) rather than a new one.
// ADC_ISR_DEBUG_PIN_ENABLED=0 keeps adc_conv_done_cb() off this pin while
// it's doing this job, so the two signals never collide on the wire.
#define TIMING_DEBUG_GPIO_CMD  TIMING_DEBUG_GPIO_ADC   // toggled HIGH for the duration of
                                    // handle_serial_commands() each loop() iteration (Core 1,
                                    // task context - digitalWrite is fine here, same as
                                    // TIMING_DEBUG_GPIO).

// CMD_DEBUG_PIN_ENABLED gates the loop()-context toggle of
// TIMING_DEBUG_GPIO_CMD above (same physical pin as TIMING_DEBUG_GPIO_ADC,
// pin39) - set to 0 here for the same reason ADC_ISR_DEBUG_PIN_ENABLED is
// 0 above: this pin is being TEMPORARILY REPURPOSED again, this time as the
// sine-chirp test mode's square-wave reference output (CHIRP_REF_GPIO
// below) for the external ADC-based transfer-function measurement rig.
// Two things (the serial-activity-correlation marker this flag normally
// gates, and the chirp reference square wave) must never drive the same
// physical pin at once, same mutual-exclusivity reasoning as
// ADC_ISR_DEBUG_PIN_ENABLED. Flip back to 1 (and move CHIRP_REF_GPIO to
// its own pin) if the serial-activity-correlation marker is needed again.
#define CMD_DEBUG_PIN_ENABLED 0

// ---- Sine-chirp test mode ('w', AUDIO_SRC_CHIRP) - characterizes the
// analog envelope/PWM (RSET) reconstruction filter's transfer function
// against the user's own 2-channel ADC-based TF measurement rig. Sweeps
// logarithmically from CHIRP_F0_HZ to CHIRP_F1_HZ over CHIRP_SWEEP_SEC,
// then repeats, with a brief CHIRP_MUTE_SEC silence at each restart as a
// sync marker the measurement system can trigger on. Runs at the full
// ENVELOPE_INTERP_FACTOR x SAMPLE_RATE_HZ fast-tick rate (see
// envelope_interp.h), NOT the normal SAMPLE_RATE_HZ full-tick rate -
// SAMPLE_RATE_HZ=16000's 8kHz Nyquist can't represent a 20kHz chirp, but
// the fast-tick infrastructure already runs unconditionally at 64kHz
// regardless of whether 'I' interpolation is toggled on, so no new timer
// infrastructure is needed. CHIRP_F1_HZ=20kHz matches the user's stated
// measurement range ("I can measure to about 20kHz").
#define CHIRP_F0_HZ      20.0f
#define CHIRP_F1_HZ   8000.0f
// 2026-09-05: slowed from 5.0s - the gdeq mid-band (2.8-4.5kHz) redesign
// work needs cleaner phase data than the original fits used (see
// group_delay_fit_notes.md's matching entry: a coefficient search against
// noisy reconstructed data gave an unstable answer). A slower sweep means
// more fast-ticks (more sine cycles) elapse per Hz of frequency change
// everywhere in the band, giving the external rig's own
// measurement/averaging more settled data per point - directly reduces
// the kind of point-to-point phase jitter that made the mid-band redesign
// search sensitive to the smoothing window used. Tune to taste against
// the rig's own integration time; nothing else depends on this value.
#define CHIRP_SWEEP_SEC  30.0f
// 2026-09-05: sweep now goes UP then back DOWN (CHIRP_BIDIRECTIONAL,
// below) each cycle, so CHIRP_SWEEP_SEC is the duration of ONE leg (up OR
// down), not the full up+down cycle - a full cycle is
// CHIRP_MUTE_SEC + 2*CHIRP_SWEEP_SEC.
#define CHIRP_MUTE_SEC    0.01f
// 2026-09-05: sweep up (CHIRP_F0_HZ->CHIRP_F1_HZ) then back down
// (CHIRP_F1_HZ->CHIRP_F0_HZ) before muting/restarting, instead of the old
// one-directional sweep that jumped straight from CHIRP_F1_HZ back to
// CHIRP_F0_HZ at the top of every repeat. NOTE: that old jump happened
// right as the envelope was about to go silent for CHIRP_MUTE_SEC anyway
// (mute forces the envelope to 0 unconditionally, regardless of sweep
// direction), so it was never an audible/analog discontinuity in the
// actual RSET output - the real benefit here is TWO independent phase
// readings of the same frequency per cycle (once on the way up, once on
// the way down), which is a direct, free check for any timing/sync lag in
// the measurement chain itself (up-leg and down-leg readings should
// agree; a consistent gap between them points at the rig, not the
// filter). Set to 0 to restore the old one-directional sweep (e.g. if a
// downstream analysis script assumes a single up-only pass and hasn't
// been updated to split the two legs).
#define CHIRP_BIDIRECTIONAL 1
// 2026-09-05: sweep law - 1 = logarithmic (original, f(t)=f0*(f1/f0)^(t/T),
// equal TIME PER OCTAVE), 0 = linear (f(t)=f0+(f1-f0)*(t/T), equal time
// per Hz). CAUTION before flipping this to 0 with the CHIRP_F0_HZ/
// CHIRP_F1_HZ values above unchanged: a log sweep from 20Hz-20000Hz
// already spends ~87% of its dwell time below 8000Hz (since most of the
// 20-20000Hz range's ~10 octaves fall below 8kHz) - exactly the audio
// band this project cares about. A LINEAR sweep over the same 20-20000Hz
// endpoints would spend only ~40% of its time below 8000Hz (8000-20 is
// under 40% of the full 20-20000 span) - i.e. switching to linear WITHOUT
// also narrowing CHIRP_F1_HZ would give WORSE resolution in the band that
// matters, not better, despite "linear = more even" intuition. If linear
// is wanted specifically to get more even dwell time across a target band
// (e.g. to resolve the gdeq mid-band 2.8-4.5kHz zone better relative to
// the rest of 100-8000Hz), lower CHIRP_F1_HZ to match that band (e.g.
// 8000Hz) at the same time - don't just flip this flag alone.
#define CHIRP_SWEEP_LOG 0
#define CHIRP_REF_GPIO  TIMING_DEBUG_GPIO_ADC   // pin39 (was pin13, moved 2026-09-02 - see
                                    // TIMING_DEBUG_GPIO_ADC's own comment above) - square-wave
                                    // reference channel for the
                                    // TF measurement rig, in sync with the chirp's own
                                    // instantaneous frequency (see test_signals_generate_chirp()).
                                    // Mutually exclusive with TIMING_DEBUG_GPIO_CMD's use of this
                                    // same pin - see CMD_DEBUG_PIN_ENABLED above.

// ---- Analog envelope/RSET reconstruction filter hardware variant ----
// Selects which LTspice-fitted group-delay-equalizer coefficient set
// envelope_gdeq.h uses: ENV_FILTER_BC337 is the original 2-pole Sallen-Key
// + BC337 NPN buffer design (SallenKey_LP_filter_BC337.txt);
// ENV_FILTER_PNP_BC327_ATTN is the replacement PNP BC327 RSET driver +
// gate attenuator now under test (SallenKey_LP_filter_PNP_BC327__RSET_
// Driver__Gate_Attn.txt) - measurably slower (~25-30us more group delay
// through the audio band) and lossier (~2-7dB more insertion loss,
// growing with frequency) than the BC337 design; see
// group_delay_fit_notes.md's 2026-09-01 refit entry for the full
// comparison, including the still-open question of whether that extra
// high-frequency loss hurts IMD3/IMD5 performance in a way the group-delay
// equalizer alone can't fix (it's all-pass - unity magnitude by
// construction, so it flattens delay only, never touches amplitude).
// envelope_gdeq.h #error's out for any unrecognized value here, same
// defense as SAMPLE_RATE_HZ's own #error just below - silently running
// one filter's fit against the other filter's real hardware would be a
// subtle dispersion/IMD regression, not a crash.
#define ENV_FILTER_BC337            0
#define ENV_FILTER_PNP_BC327_ATTN   1
#define ENV_FILTER_VARIANT   ENV_FILTER_PNP_BC327_ATTN

#define SAMPLE_RATE_HZ     16000u  // was 9600 (see below), then 10000 for a long stretch, now
                                    // raised to 16000 once the AD9851 write path (bit-bang +
                                    // fast register writes, see AD9851.c) freed up enough
                                    // real-time budget: at 10000 write_us alone was ~50-52us of
                                    // the 100us period; the bit-bang rewrite cut that to ~25us,
                                    // and with adc+dsp added (~45us total, measured via
                                    // [timing]) there was real headroom to spend. 16000 is the
                                    // next value up from 10000 that still divides both 80000
                                    // (ADC) and 2000000 (gptimer resolution_hz) evenly - the
                                    // only other clean option below 20000 - see below for why
                                    // that divisibility matters, and why 20000 itself was ruled
                                    // out (period drops to 50us, leaving too little margin
                                    // against the ~12-20us of wakeup jitter already measured).
                                    //
                                    // IMPORTANT: SAMPLE_RATE_HZ isn't just this #define - see
                                    // envelope_gdeq.h's ENV_GDEQ_A1/A2 (fitted separately per
                                    // Fs AND per ENV_FILTER_VARIANT above, #error's out at
                                    // compile time for any unfitted combination), settings.h's
                                    // relative_delay_samples presets
                                    // (rescaled by the Fs ratio when this last changed, but only
                                    // an approximation - see that file's header note), and
                                    // dsp_task's dc_alpha (now derived from DC_BLOCK_TIME_CONSTANT_S
                                    // below rather than hardcoded, so it doesn't need touching).
                                    //
                                    // Original 9600->10000 change: 9600 didn't divide either
                                    // 80000 (ADC) or 2000000 (gptimer resolution_hz) evenly,
                                    // causing a genuine ~4.8% ADC_SAMPLES_PER_TICK mismatch
                                    // (true_ratio measured 8.387 vs nominal 8) plus imprecise
                                    // gptimer alarm timing. 10000 divided both cleanly
                                    // (80000/10000=8, 2000000/10000=200) - restored the FIFO's
                                    // exact-ratio assumption the catch-up logic depends on for
                                    // smooth operation; 16000 keeps that same property
                                    // (80000/16000=5, 2000000/16000=125).
                                    //
                                    // A drop to ADC_CONT_SAMPLE_FREQ_HZ=48000 (adc_capture.h) was
                                    // tried, to free Core-0 headroom for the wakeup-jitter work
                                    // below - reverted after real hardware showed no clear CPU
                                    // win and a real noise regression (see that #define's own
                                    // "TRIED, REVERTED" comment for the full story). Rate was back
                                    // to 80000 at the time this note was written - the divisibility
                                    // numbers above were live as written then, not just historical.
                                    //
                                    // 2026-09-23 UPDATE: ADC_CONT_SAMPLE_FREQ_HZ is now 64000 (see
                                    // that define's own 2026-09-23 comment in adc_capture.h) - a
                                    // DIFFERENT change from the reverted 48000 experiment above,
                                    // confirmed on real hardware as an improvement (lower ADC-clock
                                    // rounding error - see moving_forward_notes.md's 2026-09-20/
                                    // 09-21 entries). Divisibility still holds cleanly with
                                    // SAMPLE_RATE_HZ=16000 (64000/16000=4, vs 80000's old 5;
                                    // 2000000/16000=125 is unaffected, since gptimer's
                                    // resolution_hz doesn't depend on the ADC rate).
#define HILBERT_TAPS       65   // was briefly tested at 129 to check whether Hilbert filter
                                 // approximation accuracy was the source of the IMD floor that
                                 // tracks 1:1 with signal level below -6dB - real hardware A/B
                                 // showed no significant difference, ruling that hypothesis out
                                 // cleanly. Reverted to 65 since 129 bought nothing but extra FIR
                                 // cost. The floor's more likely explanation is now the sub-sample
                                 // timing residual - see relative_delay.h's fractional delay
                                 // line, added specifically to test that instead. Must stay ODD
                                 // if changed again.
#define MAX_FREQ_DEV_HZ    20000.0f  // Originally raised from 2800.0f for a diagnostic A/B test -
                                     // real hardware showed a consistent ~+100Hz offset on BOTH
                                     // tones of a 700/1900Hz two-tone test (landed at 800/1999Hz)
                                     // while a single 1000Hz tone was exactly on frequency;
                                     // fast_atan2/fast_sqrt already ruled out via direct A/B
                                     // (SSB_DSP_FAST_TRIG=0 test, no change). Raising the clamp
                                     // let ssb_dsp.c's max_unclamped_freq_dev_hz diagnostic (see
                                     // diagnostics.cpp's '[dsp] max_unclamped=' line) measure the
                                     // TRUE peak deviation without the clamp masking it - came
                                     // back ~4800-4900Hz for real two-tone signals (see
                                     // FM_TEST_DEV_HZ below), well above the old 2800Hz ceiling,
                                     // meaning that clamp was routinely engaging on real peaks,
                                     // not just as a rare edge-case safety limit. Whether that
                                     // clamping was actually the CAUSE of the +100Hz offset was
                                     // never confirmed either way - that thread was left open.
                                     //
                                     // Separately, real hardware mic white-noise testing showed
                                     // the freq-dev clamp performs better set higher - so 8000Hz
                                     // is being KEPT for now rather than reverted to 2800.0f, but
                                     // still isn't a deliberately-chosen final value (no attempt
                                     // yet to find where between ~4900Hz and 8000Hz is actually
                                     // optimal, vs. just "high enough to stop hurting").

// ---- Isolation test signal parameters (ENVSTEP / FMTEST / AMTEST) - see
// audio_source_t in settings.h for what each mode does. ----
#define ENVSTEP_HZ   4.0f   // square wave rate - slow enough for easy scope triggering/viewing,
                             // fast enough not to be tedious to observe
#define FM_TEST_MOD_HZ  1200.0f  // matches the 700/1900Hz two-tone pair's beat frequency, for
                                  // direct comparability - spurs at n*1200Hz offsets would mean
                                  // something in AD9851 chain itself, not the DSP/Hilbert path
#define FM_TEST_DEV_HZ  3000.0f  // peak deviation - comparable order to real two-tone peak
                                  // deviations seen in testing (up to ~4800-4900Hz observed).
                                  // Gives modulation index beta=3000/1200=2.5 - a reasonably rich
                                  // sideband spectrum, good for comparing against the predicted
                                  // Bessel-function pattern
#define AM_TEST_MOD_HZ  1200.0f  // same rate as FM_TEST_MOD_HZ, for direct comparability between
                                  // the two isolation tests
#define AM_TEST_DEPTH   0.5f     // envelope swings the FULL available [0,1] range (0.5 +/- 0.5) -
                                  // 100% depth AM, deliberately stressing the RSET path's linearity
                                  // across its whole normalized range, same as real two-tone peaks
                                  // routinely reach in practice

// Worst-case timing diagnostics period, shared by diagnostics.cpp - one
// sample period in microseconds.
#define SSB_SAMPLE_PERIOD_US (1000000UL / SAMPLE_RATE_HZ)
