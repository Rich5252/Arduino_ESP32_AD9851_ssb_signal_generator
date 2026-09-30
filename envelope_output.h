#pragma once

/**
 * envelope_output.h
 *
 * Everything that turns the computed envelope into an actual analog
 * signal: the LEDC/RC "PWM comparison" path (RSET modulation via a
 * BS170 gate), the MCP4725 I2C DAC path (currently disconnected
 * hardware, kept for side-by-side comparison), and (2026-09-08) a third
 * SDM (Sigma-Delta Modulation) comparison path, plus the envelope-to-PWM-
 * duty range mapping knobs ('u'/'j'/'i'/'k').
 *
 * Why a DAC instead of PWM+RC filter: at a -70dBc spurious target, a
 * switched (PWM) envelope needs either an impractically high switching
 * frequency or a multi-pole filter to get there on filter math alone, and
 * in practice parasitic coupling of the switching edges tends to dominate
 * at that level regardless of filter order. A true DAC output has no
 * switching-frequency energy to suppress in the first place.
 *
 * PWM_COMPARISON_ENABLED (config.h) adds the old LEDC+RC path back in,
 * driven from the same envelope value as the DAC, purely so the two can
 * be scoped side by side against the same source signal. Set to 0 once
 * you're done comparing.
 *
 * Why SDM, and why now: the whole PWM interpolation investigation
 * (pwm_envelope_interpolation_report.md) found two independent write-
 * rate-related noise costs baked into the LEDC peripheral (a wake-rate/
 * CPU-margin cost and a hardware-fade-engine-inherent cost), neither
 * fixed by any of five architectures tried. SDM is architecturally
 * different: its 1-bit output free-runs continuously at its own internal
 * comparator rate (SDM_SAMPLE_RATE_HZ below), decoupled from how often
 * software updates the target density - in principle sidestepping both
 * costs with a single plain register write per real (16kHz) dsp_task
 * tick, no interpolation architecture needed. It was set aside earlier
 * (2026-09-08) over its 8-bit signed resolution (vs. LEDC's 10-bit duty)
 * given this project's prior linearity/resolution battles - being tried
 * anyway now because the only way to really settle it is on the bench.
 * SDM_COMPARISON_ENABLED (config.h) gates this path; off by default,
 * not yet bench-verified in any form.
 */

#include <stdint.h>
#include "config.h"
#include "esp_attr.h"

// ---- MCP4725 DAC (RSET modulation output) ----
// NO LONGER CONNECTED (DAC hardware removed) - GPIO47/48 are the
// highest general-purpose pins on the S3: not strapping, not flash/
// PSRAM (even on Octal variants), not USB-JTAG, not shared with UART0/
// Serial. Note: if your exact module part number ends in "V" (e.g.
// N8R8V), these two run at 1.8V logic instead of 3.3V - check before
// this DAC (or anything else) actually gets wired back to them.
#define MCP4725_SDA_GPIO      47
#define MCP4725_SCL_GPIO      48
#define MCP4725_I2C_PORT      I2C_NUM_0
#define MCP4725_I2C_FREQ_HZ   400000        // fast mode - standard (100kHz) is too slow to fit the sample period
#define MCP4725_I2C_ADDR      0x61          // 0x60 with A0 tied low, 0x61 with A0 tied high
// Keep output codes off the 0/4095 rails - MCP4725 linearity degrades
// near the extremes (datasheet-recommended usable range).
#define DAC_CODE_MIN           100
#define DAC_CODE_MAX          4000

// ---- PWM comparison path: drives the same envelope value out via
// LEDC+RC as well as the DAC, so you can scope both side by side against
// the same source signal. Set PWM_COMPARISON_ENABLED (config.h) to 0 once
// you're done comparing - this was removed from the main design in favour
// of the DAC (imaging/spurious concerns discussed earlier), this is just
// for a direct side-by-side look.
#define RSET_MOD_LEDC_GPIO    2      // within the board's easy-access GPIO1-13 range; not otherwise used
#define RSET_MOD_LEDC_TIMER   LEDC_TIMER_0
#define RSET_MOD_LEDC_CH      LEDC_CHANNEL_0
// 2026-09-08: retuned from 78125 (max achievable at 10-bit res on the
// 80MHz APB clock, i.e. 80,000,000/1024 - the original reasoning, kept
// here for the record) to 64000 - ENVELOPE_INTERP_FACTOR(4) x
// SAMPLE_RATE_HZ(16000), i.e. the actual envelope update rate once 'I'
// interpolation is on. 78125 has no integer relationship to 64000 (v3's
// hardware-LEDC-fade attempt found ~12.8us of unsynchronized jitter
// between the two as a result - see envelope_interp.h's v1-v4 history);
// 64000 IS also an exact integer division of the same 80MHz clock
// (80,000,000/1250), so this is step 1 of testing whether putting the
// carrier and the update rate on a commensurate (ideally phase-lockable)
// footing removes that jitter - deliberately isolated, no other change
// yet (still the existing software-driven ledc_set_duty()/
// ledc_update_duty() path from envelope_interp.h v4, not v3's hardware
// fade engine - that's a separate, later step if this alone helps).
// NOTE: if either ENVELOPE_INTERP_FACTOR or SAMPLE_RATE_HZ changes, this
// needs revisiting by hand - deliberately not written as a formula here,
// same convention as ENVELOPE_INTERP_FACTOR itself being a plain literal
// rather than derived from anything.
#define RSET_MOD_LEDC_FREQ_HZ 64000
#define RSET_MOD_LEDC_RES     LEDC_TIMER_10_BIT

// ---- SDM comparison path, 2026-09-08 - see this file's header comment
// for the "why SDM, why now" reasoning. Third leg alongside PWM/RC and the
// DAC, driven from the same envelope value, gated by SDM_COMPARISON_ENABLED
// (config.h).
//
// 2026-09-08, later: SDM_OUT_GPIO changed from its own separate pin (was
// GPIO1) to REUSE RSET_MOD_LEDC_GPIO - user's call, since this is meant as
// an EITHER/OR comparison against PWM (one test point/filter on the bench,
// not two), not a simultaneous three-way A/B like the PWM+DAC pair above.
// Reusing the #define (rather than a second hardcoded "2") means the two
// can never silently drift apart if RSET_MOD_LEDC_GPIO itself ever moves.
//
// IMPORTANT - genuinely one-or-the-other, not just "usually": PWM_COMPARISON_
// ENABLED and SDM_COMPARISON_ENABLED must never both be 1 at the same time
// now that they share a pin - ledc_channel_config() and sdm_new_channel()
// would both try to route this same GPIO through the GPIO matrix to two
// different peripherals, which is a real conflict (undefined which one
// actually wins the pin, not a benign no-op). Enforced below with a
// build-time #error rather than left as a "remember not to" comment - see
// that #error's own text for how to fix it if it fires.
#define SDM_OUT_GPIO           RSET_MOD_LEDC_GPIO

#if PWM_COMPARISON_ENABLED && SDM_COMPARISON_ENABLED
#error "PWM_COMPARISON_ENABLED and SDM_COMPARISON_ENABLED both 1: they now share SDM_OUT_GPIO/RSET_MOD_LEDC_GPIO (same physical pin) and cannot both drive it at once. Set exactly one of these to 1 in config.h before building."
#endif

// SDM's own free-running comparator/carrier rate - NOT a rate anything in
// this codebase writes at (see envelope_output_write_sdm()'s call site,
// envelope_interp.cpp's on_full_tick(), which writes once per real
// SAMPLE_RATE_HZ dsp_task tick, currently 16kHz, regardless of this
// value - a brief ISR-commit variant tried writing on its own separate
// schedule instead and measured WORSE on real hardware, see that
// function's own comment below for the full story). 1MHz is Espressif's
// own driver/sdm.h reference example value, and an exact integer division
// of the 80MHz APB clock (80,000,000/80) -
// picked for the same "commensurate with a clean divisor" reasoning
// RSET_MOD_LEDC_FREQ_HZ's own comment used, though SDM's clock divider
// isn't chasing phase-lock with anything the way the LEDC retune was -
// there's no software-driven sub-stepping here to stay in phase with.
#define SDM_SAMPLE_RATE_HZ     1000000u

// sdm_channel_set_pulse_density()'s density argument is a signed 8-bit
// value, -128..127 (driver/sdm.h). Espressif's own docs recommend
// keeping to roughly +/-90 of that "for better randomness" (fewer near-
// fixed-density stuck/periodic patterns) rather than the full span.
// Starting at the FULL range here - this project has already fought
// resolution/dynamic-range battles elsewhere (predistort LUT, envelope-
// null floor) and the point of trying SDM at all is to see what it can
// actually do, not to hobble it pre-emptively. Drop to 90 (and re-test)
// if a spurious/stuck-pattern tone shows up on the spectrum analyzer that
// isn't there at the reduced range - not yet checked either way on real
// hardware.
#define SDM_DENSITY_CLAMP      127

// Target max DAC update rate. The envelope only carries content up to
// ~3.5-4kHz, so ~10kHz comfortably clears Nyquist. Deliberately throttling
// down from "as fast as the I2C bus allows" (~14kHz back-to-back) reduces
// how often the I2C driver's ISR fires - which we've confirmed is the
// actual source of the cross-core timing jitter on dsp_task, not flash
// cache eviction. This trades unneeded DAC update margin for reduced
// disruption, at no audio-quality cost. Applied on the SENDING side
// (envelope_output_submit_dac_sample() skips xQueueOverwrite itself) -
// see the .cpp for why.
#define DAC_TARGET_UPDATE_RATE_HZ 10000u
#define DAC_WRITE_DECIMATION ((SAMPLE_RATE_HZ + DAC_TARGET_UPDATE_RATE_HZ - 1) / DAC_TARGET_UPDATE_RATE_HZ)  // round up

// Envelope-to-PWM-duty range mapping - separate knob from master gain.
// Master gain (ssb_dsp_set_master_gain_db) scales the WHOLE signal chain
// (phase deviation and envelope together, inside ssb_dsp); this only
// remaps envelope's own [0,1] output into a duty-cycle range before it
// drives the BS170 gate via PWM. Controls where the envelope's quiet-to-
// loud excursion actually sits relative to the Vgs sweet spot already
// characterized on real hardware (~2.3V +/-0.75V) - a mismatch here is a
// plausible independent contributor to the observed nonlinearity,
// separate from overall drive level. Runtime-tunable via 'u'/'j'
// (offset) and 'i'/'k' (scale/span) so it can be swept empirically
// rather than needing a reflash per trial.
#define ENV_PWM_STEP 0.02f   // 2% duty per keypress

// Creates the envelope hand-off queue, brings up the I2C DAC driver and
// (if PWM_COMPARISON_ENABLED) the LEDC PWM channel, runs the MCP4725
// connectivity probe write, and (if dac_task_enabled) starts dac_task on
// Core 1. Call once from setup().
void envelope_output_init(void);

// Drives the PWM comparison output from the (already relative-delayed)
// envelope value. No-op if PWM_COMPARISON_ENABLED is 0, ALSO a no-op
// while duty-override mode is active (see below) - dsp_task calls this
// unconditionally every full tick, same as always; the no-op happens
// internally so dsp_task itself needed no changes for this feature.
void IRAM_ATTR envelope_output_write_pwm(float delayed_envelope);

// 2026-09-29: ENVELOPE_ISR_INTERP_ENABLED (envelope_interp.h) support -
// the real, genuinely-integer fast-tick envelope write, built after the
// ISR_PWM_FIXED_TEST_ENABLED bench probe confirmed a raw per-fast-tick
// LEDC register write fits the timing budget. Two-function split, same
// task-stages/ISR-applies shape as the AD9851 ISR-write path
// (carrier_output.h) and the same reasoning: float math (the envelope-to-
// duty conversion and the per-step delta) only ever happens in
// envelope_output_isr_stage_step(), called from dsp_task (TASK context,
// float/coprocessor use is safe there) once per full tick; the actual
// per-fast-tick register write is envelope_output_isr_fasttick_step()
// (IRAM_ATTR, integer-only, no driver calls, no float) called directly
// from on_timer_alarm() on EVERY fast tick.
//
// Fixed-point: internally, both functions work in "Q4" units (RSET_MOD_
// LEDC_RES-bit duty count, left-shifted 4 bits) purely for SOFTWARE
// accumulation precision across many fast-tick steps - see envelope_
// output_write_pwm()'s max_duty for the same base unit without the shift.
//
// *** 2026-09-30 SUPERSEDED: the paragraph below (and the ">>4 to native"
// change it describes) was WRONG - real-hardware 'b' readout showed
// ledc_get_duty() reading back accum/16, i.e. duty.duty DOES carry 4
// fractional bits and the original Q4 write was correct. The write is
// restored to the Q4 value; see envelope_output.cpp's SECOND CORRECTION. ***
// 2026-09-30 CORRECTION: this comment originally claimed the <<4 also
// matched "the hardware's own fractional sub-duty resolution," i.e. that
// the raw `duty.duty` register itself wanted the value pre-shifted left by
// 4. Real hardware proved that wrong, not just unverified: with normal
// (non-duty-override) envelope content, the PWM output was reported
// "stuck at max env level" for anything above roughly 1/16 of full scale -
// exactly what happens when a value bigger than the native 0..1023 range
// is written into a comparator that only counts across that native range
// for this configured resolution/rate. The now-confirmed-correct 'd'
// duty-override path writes plain native-range values via ledc_set_duty()
// with NO shift. envelope_output_isr_fasttick_step() now converts its Q4
// accumulator back down to native units (>>4, rounded) at the point of
// writing to hardware - see that function's own dated comment in the .cpp
// for the full story. The Q4 representation itself stays useful (it's
// what lets envelope_output_isr_stage_step() spread a fractional-native-
// unit-sized per-tick delta across ENVELOPE_INTERP_FACTOR ticks without
// losing it to integer truncation every single tick) - only the
// assumption about what the HARDWARE register wants was wrong. Staging a
// PER-TICK STEP (not a target) keeps the ISR side integer-only.
// [SUPERSEDED 2026-09-30, see THIRD CORRECTION below: this paragraph used
// to say the step is always computed relative to the LAST STAGED TARGET, NOT
// by reading back the ISR's own accumulator, keeping the handoff purely
// one-directional. That was the open-loop design and it is why a timing slip
// could leave a permanent duty error.]
//
// 2026-09-30 SECOND CORRECTION: the paragraph that used to sit here
// claimed this "does NOT accumulate long-term error" because each step is
// computed fresh against the last true target every tick. That was wrong,
// not just optimistic - real hardware showed normal (non-override)
// two-tone content pinned very low even with offset/scale confirmed sane,
// and working through the algebra found why: with NO carry, whenever two
// consecutive targets differ by less than ENVELOPE_INTERP_FACTOR counts
// (routine for real, smoothly-varying content, not an edge case), integer
// division truncates the step to exactly zero and the difference is
// simply thrown away forever - it can never be recovered on a later tick,
// because every tick's step is computed only against the immediately
// preceding target, never against how far the accumulator has actually
// drifted. The resulting error is an unbounded random walk, not a bounded
// wobble. Fixed by carrying the integer-division remainder forward into
// the next tick's numerator (`s_isr_interp_carry_q4`, standard Bresenham/
// DDA-style error diffusion - see the .cpp for the implementation) so no
// fractional part is ever permanently lost. WITH that carry, the "at most
// (ENVELOPE_INTERP_FACTOR-1)-count wobble between full-tick boundaries,
// exact AT each boundary" property is actually true; without it, it
// wasn't. Not yet bench-verified against a scope that this fix is
// complete - flagged the same way envelope_output_start_hw_fade()'s own
// approximate rounding is flagged below.
//
// 2026-09-30 THIRD CORRECTION (closed loop): the step is now computed from
// where the ISR's accumulator ACTUALLY is - step = (target - accum)/FACTOR -
// instead of from the difference between consecutive targets. The old
// open-loop form assumed every staged step is applied exactly FACTOR times;
// any deviation (staging landing across a 64 kHz fast-tick boundary, a
// coalesced/overrun dsp_task tick) left a permanent error that later steps
// never corrected. Now such an error is removed within one period. The
// price is one task-side read of the ISR-owned accumulator (single aligned
// 32-bit volatile load; a one-fast-tick-stale read costs one step of
// transient error that the next stage corrects). The carry variable is now
// informational only (see carry_q4 below). Not bench-verified. A staging-
// error monitor (stage_err_* below, printed by 'b') counts how often the
// previous step was NOT applied the nominal number of times.
//
// Call once per full tick from TASK context (dsp_task, via
// envelope_interp_on_full_tick() - see envelope_interp.cpp). `envelope`
// is the plain [0,1] envelope value, same units every other envelope_
// output_* function in this header takes.
void envelope_output_isr_stage_step(float envelope);

// IRAM_ATTR, integer-only, no driver calls, no float - see
// envelope_output_isr_stage_step()'s own comment above for the overall
// design. Call on EVERY fast tick (every gptimer alarm, not just full
// ticks) from on_timer_alarm() (ssb_mic_test.ino) - adds whatever step
// envelope_output_isr_stage_step() last staged to a running Q4 fixed-point
// accumulator, clamps it defensively to the valid duty range, converts
// back down to plain native duty units (>>4, rounded - see the 2026-09-30
// correction above), and writes the result straight to the LEDC duty
// register via raw peripheral struct access (soc/ledc_struct.h) - NOT
// ledc_set_duty()/ledc_update_duty(),
// which are confirmed NOT ISR-safe (see envelope_output_write_pwm()'s own
// comment and envelope_interp.h's v1/v2 history). Includes the
// conf0.low_speed_update commit trigger found necessary during the
// ISR_PWM_FIXED_TEST_ENABLED bench investigation (moving_forward_notes.md,
// 2026-09-29) - ESP32-S2/S3 has no LEDC high-speed mode, so every channel
// needs that second commit step beyond conf1.duty_start before a written
// duty value actually latches. Same field-name-uncertainty caveat as that
// earlier diagnostic: reasoned from general ESP32/S2/S3 LEDC knowledge,
// not verified against this project's exact installed SDK version.
void IRAM_ATTR envelope_output_isr_fasttick_step(void);

// 2026-09-30: diagnostic-only snapshot of the ISR-interp internal state,
// added after three real bugs in this path (missing duty-override guard,
// Q4->native scaling, carry-forward telescoping) were each found and
// fixed in turn, yet real-hardware two-tone output was STILL reported
// "the same low level" afterward on Preset 1 with duty-override confirmed
// off - i.e. a fourth, not-yet-identified cause (or a still-incomplete
// understanding of one of the first three) may remain. Rather than
// guessing blind at a fifth hypothesis with no ESP-IDF toolchain to
// compile/test locally, this exposes the actual running numbers so the
// user can report concrete values via the new 'b' serial command
// (serial_commands.cpp) instead of a qualitative description.
//
// Called from task context only (serial command handling, loop()/Core 1),
// never from the ISR itself - same non-ISR-safety-relevant context
// envelope_output_write_duty_raw() already runs in. Reads of the
// individual int32_t statics are single aligned-word loads (same
// atomicity argument as envelope_output_isr_fasttick_step()'s own
// s_isr_interp_step_q4 read), so no lock is needed, but note this is a
// snapshot assembled from several independent reads, not one atomic
// transaction - the ISR can interleave between them. Fine for a
// diagnostic print; not something to build control logic on.
//
// hw_duty_now is read back live via ledc_get_duty() (same call
// envelope_output_isr_interp_reseed_from_hw() already uses) - this is
// "ground truth," independent of anything this module's own software
// state believes, and is the most direct way to confirm or rule out
// whether the ISR's raw register write is actually reaching the hardware
// at all versus dsp_task's own accum_q4/step_q4 math being wrong upstream
// of that write.
typedef struct {
    float    last_envelope;    // last envelope value passed into envelope_output_isr_stage_step(), post-clamp
    int32_t  last_target_q4;   // that value's Q4 target - staging overwrites this every full tick, so it IS this call's target, not a stale one
    int32_t  step_q4;          // per-fast-tick step currently staged for the ISR to apply
    int32_t  accum_q4;         // ISR's own running duty accumulator (Q4 units)
    int32_t  carry_q4;         // INFORMATIONAL since the closed-loop change: remainder (target - accum) - step*FACTOR at the last stage (was the Bresenham carry)
    uint32_t hw_duty_now;      // ledc_get_duty() read back right now, native (0..(1<<RSET_MOD_LEDC_RES)-1) units
    uint32_t calls_total;      // times envelope_output_isr_fasttick_step() was entered (wraps at 2^32) - should advance ~64000/s at FACTOR=4
    uint32_t calls_past_guard; // subset of calls_total that got PAST the duty-override early-return
    uint32_t override_on;      // s_duty_override_enabled right now (1 = ON = ISR write locked out)
    uint32_t ledc_freq_hz;     // ledc_get_freq() - the LEDC timer's ACTUAL frequency (requested: RSET_MOD_LEDC_FREQ_HZ); 0 if unavailable
    // Staging-error monitor (2026-09-30). At each dsp_task stage,
    // e = (ISR accumulator now) - (previous staged target). If the previous
    // step was applied exactly ENVELOPE_INTERP_FACTOR times, |e| <=
    // FACTOR-1 (integer-division remainder only). Larger |e| = the previous
    // step was applied a different number of times (timing slip); it is now
    // corrected by the closed-loop step, and counted here. Excludes the
    // first stage after boot / after an override-exit reseed / all stages
    // while duty-override is ON. Since boot or the last 'r'; not reset by
    // 'V' or 'b'.
    int32_t  stage_err_q4;         // most recent e
    int32_t  stage_err_max_abs_q4; // max |e| since boot
    uint32_t stage_err_events;     // stages with |e| > FACTOR-1
    uint32_t stage_err_samples;    // stages measured
} envelope_output_isr_interp_debug_t;

void envelope_output_isr_interp_get_debug(envelope_output_isr_interp_debug_t *out);

// 2026-09-30: clears the stage_err_* statistics above. Called from
// diagnostics_reset() (the 'r' command). Safe to call from task context at
// any time; a stat update racing the reset can leave one value out by one
// event (diagnostic only). Always-safe no-op when ENVELOPE_ISR_INTERP_ENABLED
// is off.
void envelope_output_isr_interp_reset_stage_err_stats(void);

// 2026-09-08: ENVELOPE_INTERP_USE_HW_FADE (envelope_interp.h) support -
// starts a hardware LEDC fade from whatever duty the channel is CURRENTLY
// at (read internally by the driver, not passed in) toward target_envelope,
// autonomously subdividing into `steps` equal-ish increments on the LEDC's
// own clock, one PWM period apart - no further software/ISR involvement
// once started. `steps` is a plain parameter (not ENVELOPE_INTERP_FACTOR
// hardcoded here) so this module stays decoupled from envelope_interp's
// own constants, same reasoning as envelope_output_write_pwm() taking a
// plain float rather than reaching into envelope_interp's state itself.
// Respects the same duty-override early-return as envelope_output_write_
// pwm() above. Requires envelope_output_init() to have already called
// ledc_fade_func_install() - see that function's own comment. Approximate,
// not exact: ledc_set_fade_with_step()'s `scale` is a per-step duty COUNT,
// not a step COUNT, so the actual number of hardware steps taken to reach
// target_duty is round-trip via integer division and may come out to
// `steps` +/-1 depending on how evenly the current-to-target distance
// divides - not yet bench-verified against a scope whether this rounding
// is small enough to ignore or needs tighter handling.
void IRAM_ATTR envelope_output_start_hw_fade(float target_envelope, uint32_t steps);

// 2026-09-08: resets the LEDC timer's own internal counter to a known
// phase - called once from init_sample_timer() (ssb_mic_test.ino) right
// after gptimer_start(), so the LEDC's autonomous fade-step clock and the
// sample gptimer's alarm grid start from a common reference point instead
// of an arbitrary power-on-to-power-on offset. See envelope_interp.h's
// ENVELOPE_INTERP_USE_HW_FADE comment for why this matters and why it only
// works now that RSET_MOD_LEDC_FREQ_HZ is commensurate with the tick rate.
void envelope_output_sync_ledc_timer_now(void);

// 2026-09-08: SDM comparison path - see this file's header comment and the
// SDM_OUT_GPIO/SDM_SAMPLE_RATE_HZ/SDM_DENSITY_CLAMP defines above. Maps the
// [0,1] envelope linearly onto [-SDM_DENSITY_CLAMP,+SDM_DENSITY_CLAMP] and
// writes it straight to the SDM channel's pulse density - envelope=0 maps
// to the most-negative density (Vout nearest 0), envelope=1 to the most-
// positive (Vout nearest VDD_IO), the same sense as
// envelope_output_write_pwm()'s duty=0..max_duty mapping. Called once per
// real dsp_task tick from envelope_interp_on_full_tick() (envelope_interp.
// cpp), deliberately BEFORE that function's own 'I'/HW_FADE/curve
// branching - this path always writes the freshest full-tick value
// directly, completely independent of whatever interpolation the PWM leg
// is doing, since SDM's whole premise (see header comment) is that no
// interpolation should be needed for it at all. No-op if
// SDM_COMPARISON_ENABLED is 0, or if channel init failed (see
// envelope_output_init()'s SDM boot log line).
//
// 2026-09-08, later: BRIEFLY split into a stage/commit pair (dsp_task
// stages the density, on_timer_alarm() ISR commits it via
// sdm_channel_set_pulse_density(), which driver/sdm.h documents as ISR-
// safe) to try to shave the ~1dB IMD gap this single-function version
// measured against the best PWM/RC result, on the theory that dsp_task's
// own cross-core wake/scheduling latency was adding jitter to the write.
// REVERTED - real hardware came back WORSE (another ~1dB down, close-in
// jitter still present or worse), not better. Two suspected reasons, not
// mutually exclusive, both amounting to "the ISR became less
// trustworthy, not more": (1) sdm_channel_set_pulse_density()'s IRAM
// residency is gated by a separate Kconfig option
// (CONFIG_SDM_CTRL_FUNC_IN_IRAM) this project has no way to confirm is
// set in the installed Arduino-ESP32 core - if it isn't, that call is
// FLASH-resident, and calling flash-resident code from inside the
// project's single highest-priority ISR risks exactly the kind of cache-
// line stall this project already found and fixed once before (dac_task's
// I2C driver activity on Core 1 - see dsp_task's own IRAM_ATTR comment,
// ssb_mic_test.ino), except now inside the ISR itself, which also delays
// the vTaskNotifyGiveFromISR() call right after it - compounding rather
// than just adding. (2) the stage happens LATE in dsp_task's own per-tick
// work (after the full ADC/Hilbert/gdeq/ampeq/predistort/relative-delay
// chain - see the .ino's own "PWM write goes FIRST... before the AD9851
// SPI transfer" comment for why it's placed there), i.e. at the point in
// the tick where dsp_task's own accumulated timing variance is largest;
// if dsp_task ever finishes late enough to spill past the NEXT tick's
// alarm (a real, already-documented possibility - see dsp_task's own
// elapsed_fast_ticks/coalescing-fix comment), the ISR at that next tick
// commits a value stale by MORE than the intended one tick, an irregular
// hiccup rather than the clean fixed delay the design assumed. Full
// writeup: group_delay_fit_notes.md's matching entry. Reverted to this
// single synchronous function - real hardware evidence beats the
// theoretical benefit here, same standing project convention as
// everywhere else this happened (v5 hardware fade, the ENVELOPE_INTERP_
// FACTOR correction, etc.).
void IRAM_ATTR envelope_output_write_sdm(float envelope);

// ---- Direct duty/density override ('d' + '>'/'<'/'N'/'B', serial_commands.
// cpp) ----
// For characterizing the RSET/filter/AD9851 chain directly against a
// KNOWN, exact commanded output level, bypassing master gain, envelope,
// offset/scale, AND the predistort LUT entirely - see
// envelope_predistort.h's REVISION 3 notes for why this exists (the
// gate-voltage-inference chain that REVISION 2/3 relied on to back out
// duty from a DC voltage reading is no longer needed at all once duty can
// just be commanded and read back directly). The carrier/phase (AD9851)
// path is completely unaffected - whatever audio source is selected
// keeps running normally; 's' (single-tone, phase held rock-steady) is
// the natural choice while sweeping this.
//
// 2026-09-08: generalized from PWM-only to ALSO drive SDM, whichever of
// the two is actually compiled in (PWM_COMPARISON_ENABLED/SDM_COMPARISON_
// ENABLED are mutually exclusive - see the build-time #error above) - see
// envelope_output_write_duty_raw()'s own comment in the .cpp for the exact
// index<->duty/density mapping. This means the SAME 'd'/'>'/'<'/'N'/'B'/'E'
// UI, and any existing automated sweep harness already driving those keys
// for PWM's own duty linearity characterization, can be reused unchanged
// to run the equivalent characterization against SDM instead, just by
// swapping which comparison flag is set to 1 and reflashing - the whole
// point being: SDM shares the exact same downstream RC filter/BS170 gate/
// RSET modulation as PWM now (see SDM_OUT_GPIO's own comment), so any
// nonlinearity found there is a direct, load-bearing comparison against
// the already-measured PWM predistort LUT, not a fresh unknown.
//
// Writes an EXACT raw override index [0, envelope_output_get_max_duty()]
// straight to whichever peripheral is active - no float conversion, no
// offset/scale, nothing else in between. Called only from serial_commands.
// cpp (loop()/Core 1, not dsp_task/Core 0 - same non-ISR context
// envelope_output_write_pwm()/write_sdm() themselves are always called
// from, so this needs no new ISR-safety consideration).
void IRAM_ATTR envelope_output_write_duty_raw(uint32_t duty);

// The highest valid raw override index - under PWM, (1<<RSET_MOD_LEDC_RES)-1
// (a literal LEDC duty count); under SDM, 2*SDM_DENSITY_CLAMP (see
// envelope_output_write_duty_raw()'s .cpp comment for the index<->density
// mapping); 0 if neither comparison path is compiled in. A getter rather
// than making callers reach for RSET_MOD_LEDC_RES/SDM_DENSITY_CLAMP
// themselves, since RSET_MOD_LEDC_RES in particular is an ledc_timer_bit_t
// enum from driver/ledc.h, which only THIS file's .cpp includes;
// serial_commands.cpp (the only other caller) has no reason to need that
// header itself.
uint32_t envelope_output_get_max_duty(void);

// While enabled, envelope_output_write_pwm()/write_sdm() above become a
// no-op every tick - the active peripheral just continues outputting
// whatever value envelope_output_write_duty_raw() last wrote (both LEDC
// and SDM hold their last-commanded output between explicit updates, so
// dsp_task doesn't need to keep re-writing it - and by skipping the write
// entirely rather than trying to race it, dsp_task's normal envelope
// pipeline can never fight the override). 'd' toggles this; entering it
// resets the working index to 0 - see serial_commands.cpp.
bool envelope_output_duty_override_get_enabled(void);
void envelope_output_duty_override_set_enabled(bool enable);

// Hands the (un-delayed) envelope value to dac_task via the 1-deep
// "latest value wins" queue, decimated to DAC_TARGET_UPDATE_RATE_HZ. Call
// once per dsp_task tick, after the PWM/AD9851 writes.
void IRAM_ATTR envelope_output_submit_dac_sample(float envelope);

uint16_t envelope_output_get_last_dac_code(void);

float envelope_output_get_pwm_offset(void);
float envelope_output_get_pwm_scale(void);

// 'u'/'j'/'i'/'k' handlers - clamp to [0,1] internally, same bounds the
// original inline handlers used.
void envelope_output_raise_pwm_offset(void);
void envelope_output_lower_pwm_offset(void);
void envelope_output_widen_pwm_scale(void);
void envelope_output_narrow_pwm_scale(void);

// Sets offset/scale directly (used by preset loading) - no clamping,
// matching the original preset loader's direct assignment (presets are
// trusted to contain sane values, same as before).
void envelope_output_set_pwm_offset(float offset);
void envelope_output_set_pwm_scale(float scale);
