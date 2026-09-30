/**
 * envelope_interp.cpp - see envelope_interp.h.
 *
 * v4: folded into dsp_task's own fast tick, no ISR/task/timer/LEDC-fade of
 * our own left in this file at all - see header comment for the three
 * prior versions this replaced and why each failed on real hardware.
 *
 * v4.1: step-COUNTED ramp -> time-BASED ramp (see on_full_tick()/
 * on_interp_tick() and compute_ramp_value() below) - fixes real,
 * structural jitter found on hardware even after the notification-
 * coalescing fix. See envelope_interp.h for the full explanation.
 *
 * v4.2: straight-line ramp -> Catmull-Rom / cubic Hermite curve. Only the
 * CURVE compute_ramp_value() evaluates changed - the time-accurate frac
 * derivation from v4.1 is untouched. See envelope_interp.h for the
 * simulation results and latency tradeoff behind this.
 *
 * v4.3: curve made runtime-switchable (linear vs. Catmull-Rom), for a
 * direct A/B on real hardware - see envelope_interp.h's "v4.3" header note
 * for the full rationale, including why this deliberately keeps v4.2's
 * data window/timing identical for both curve choices rather than
 * reverting "linear" mode to v4's original narrower one.
 *
 * v4.4: added a third curve, HOLD - plain zero-order-hold at the fast-tick
 * (64kHz) rate, no ramp at all, zero added latency. Isolates a DIFFERENT
 * question than v4.3's LINEAR-vs-CATMULL_ROM did: does writing the LEDC
 * duty register 4x more often matter AT ALL, independent of what shape (if
 * any) is written - see envelope_interp.h's "v4.4" header note.
 */

#include "envelope_interp.h"
#include "envelope_output.h"
#include "config.h"
#include "esp_timer.h"

// Real microsecond length of one DSP sample period, as a float - computed
// once here rather than reusing config.h's SSB_SAMPLE_PERIOD_US, which is
// an integer macro (1000000UL / SAMPLE_RATE_HZ) that truncates (62 instead
// of 62.5 at 16000Hz) - fine for its own existing uses elsewhere, but this
// module's ramp fraction is sensitive to that last half-microsecond in a
// way nothing else was, so it gets its own float-precision constant.
static const float SAMPLE_PERIOD_US_F = 1.0e6f / (float)SAMPLE_RATE_HZ;

// v4.2: FOUR consecutive full-tick envelope samples now, not two - s_p0
// (oldest) through s_p3 (newest) - plus the two Hermite tangents for the
// segment currently being rendered (s_m1/s_m2), recomputed once per full
// tick and reused across that tick's interp ticks, same as v4's
// s_prev_env/s_target_env were. Still dsp_task-PRIVATE for the same
// reason as v4 (see below) - only on_full_tick()/on_interp_tick() ever
// touch them, both only ever called from dsp_task itself, strictly
// sequentially. No other task reaches into these, so no volatile/locking
// is needed for them at all (a genuine change from v1-v3, which all
// needed a spinlock because an ISR or second task shared this exact
// state).
static float   s_p0 = 0.0f, s_p1 = 0.0f, s_p2 = 0.0f, s_p3 = 0.0f;
static float   s_m1 = 0.0f, s_m2 = 0.0f;  // tangents for the current [s_p1,s_p2] segment
static int64_t s_tick_start_us = 0;   // esp_timer_get_time() at the TRUE start of the current full tick - see header comment
static float   s_last_value   = 0.0f; // whatever was last submitted, enabled or not

// The two things serial command handling (loop(), Core 1 - a DIFFERENT
// task/core than dsp_task) actually needs to touch. Both single-word
// volatile bools, same convention already used elsewhere in this codebase
// for cross-task flags (envelope_gdeq/predistort's own enabled bools) -
// deliberately NOT reaching into s_prev_env/s_target_env directly from
// set_enabled(), which would turn them back into genuinely shared multi-
// word state needing a lock again. Instead, set_enabled() just raises
// s_reseed_pending; dsp_task applies the actual reseed itself, in its own
// context, the next time on_full_tick() runs - see there.
static volatile bool s_enabled        = false;
static volatile bool s_reseed_pending = false;

// v4.3: which curve compute_ramp_value() evaluates - see envelope_interp.h's
// "v4.3" header note. Plain volatile enum, same single-word cross-task-flag
// convention as s_enabled above (set from loop()/Core 1 via
// envelope_interp_set_curve(), read from dsp_task/Core 0 inside
// compute_ramp_value()) - no lock needed for the same reason s_enabled
// doesn't need one.
static volatile envelope_interp_curve_t s_curve = ENVELOPE_INTERP_CURVE_CATMULL_ROM;

// Time-based, not step-counted (see header comment for why): returns
// where the ramp SHOULD be right now, based on actual elapsed time since
// this tick group's true start, not on how many on_interp_tick() calls
// have happened to fire. Whatever calls DO happen - however many, however
// irregularly spaced, even after a coalescing event drops some - each one
// computes the mathematically correct point on the CURVE for the instant
// it actually runs, so the rendered envelope stays consistent tick to
// tick regardless of exactly how many discrete steps happened to render
// on any given cycle. That frac derivation is unchanged from v4.1 - v4.2
// only changed what's evaluated AT frac: a Catmull-Rom cubic Hermite
// through s_p1->s_p2 (using s_p0/s_p3 to shape the tangents at each end,
// not as points the curve itself passes through) instead of v4's straight
// line between the same two endpoints. Standard Hermite basis functions
// (h00/h10/h01/h11); see envelope_interp.h for why this needs one more
// full tick of look-ahead than v4's plain 2-point ramp did.
static float IRAM_ATTR compute_ramp_value(void)
{
    // v4.4: HOLD is a plain zero-order-hold at the fast-tick rate - no
    // ramp, no frac, doesn't touch s_p0..s_p3/s_m1/s_m2 at all. Only
    // reached from on_interp_tick() (on_full_tick() bypasses this function
    // entirely for HOLD - see there) - by the time it's called, s_last_value
    // already holds the CURRENT full tick's fresh envelope, so this simply
    // repeats it unchanged. See envelope_interp.h's "v4.4" header note.
    if (s_curve == ENVELOPE_INTERP_CURVE_HOLD) {
        return s_last_value;
    }

    float elapsed_us = (float)(esp_timer_get_time() - s_tick_start_us);
    float frac = elapsed_us / SAMPLE_PERIOD_US_F;
    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }
    // v4.3: curve is runtime-switchable - see envelope_interp.h's "v4.3"
    // header note. Both branches evaluate over the SAME [s_p1,s_p2]
    // segment (deliberate - see header note for why "linear" mode doesn't
    // revert to v4's narrower/lower-latency window).
    float value;
    if (s_curve == ENVELOPE_INTERP_CURVE_LINEAR) {
        value = s_p1 + frac * (s_p2 - s_p1);
    } else {
        float frac2 = frac * frac;
        float frac3 = frac2 * frac;
        float h00 =  2.0f * frac3 - 3.0f * frac2 + 1.0f;
        float h10 =         frac3 - 2.0f * frac2 + frac;
        float h01 = -2.0f * frac3 + 3.0f * frac2;
        float h11 =         frac3 -        frac2;
        value = h00 * s_p1 + h10 * s_m1 + h01 * s_p2 + h11 * s_m2;
    }

    // Unlike a straight line (which can never leave the [s_p1,s_p2]
    // range - so this clamp is a no-op in LINEAR mode), the Catmull-Rom
    // cubic CAN over/undershoot slightly beyond its own endpoints on a
    // sharp enough feature - confirmed with a synthetic worst-case dip
    // (two duty-0 samples flanked by two much higher ones, close to what
    // a two-tone envelope null looks like), even though neither real test
    // signal this was validated against (two-tone, AM-test) triggered it.
    // s_p1/s_p2 themselves are always in [0,1] (the .ino clamps envelope
    // before ever calling envelope_interp_on_full_tick()), so this clamp
    // only ever trims a genuine curve overshoot, never a legitimately
    // out-of-range input - and it matters here specifically because
    // envelope_output_write_pwm() casts straight to an unsigned duty with
    // no clamp of its own: a negative excursion left unclamped would
    // become a huge duty value via unsigned wraparound, not a small
    // negative one.
    if (value < 0.0f) {
        value = 0.0f;
    } else if (value > 1.0f) {
        value = 1.0f;
    }
    return value;
}

void envelope_interp_init(void)
{
    s_p0 = s_p1 = s_p2 = s_p3 = 0.0f;
    s_m1 = 0.0f;
    s_m2 = 0.0f;
    s_tick_start_us = 0;
    s_last_value = 0.0f;
    s_reseed_pending = false;
}

void IRAM_ATTR envelope_interp_on_full_tick(float envelope, int64_t tick_start_us)
{
    if (s_reseed_pending) {
        // An off->on transition happened since our last tick - anchor ALL
        // FOUR history points at the last real value (not just two, like
        // v4 did) instead of gliding from whatever stale/zero history
        // they held from before it was last disabled. Tangents computed
        // from four equal points are both zero, so the first rendered
        // segment is flat too - same "already-arrived" guarantee v4 had,
        // extended to the wider history v4.2 now keeps. Applied here, in
        // dsp_task's own context, rather than inside set_enabled() itself
        // (a different task/core) - see the statics' own comment above
        // for why.
        s_p0 = s_p1 = s_p2 = s_p3 = s_last_value;
        s_m1 = 0.0f;
        s_m2 = 0.0f;
        s_reseed_pending = false;
    }

    // tick_start_us - captured by the caller at the TRUE top of this full
    // tick, before any DSP processing - not esp_timer_get_time() taken
    // HERE, which would already be tens of microseconds late (this call
    // site is reached only after the full Hilbert/atan2/sqrt/etc. pipeline
    // has already run). Using the true start as the ramp's t=0 reference
    // is what makes compute_ramp_value() time-accurate regardless of how
    // long this tick's own DSP processing took - see header comment.
    s_tick_start_us = tick_start_us;

    // 2026-09-08: SDM comparison path (envelope_output.h) - written here,
    // once per real full tick, BEFORE any of the 'I'/HW_FADE/curve
    // branching below that exists purely to interpolate the PWM/LEDC leg
    // faster than SAMPLE_RATE_HZ. SDM's own free-running internal
    // comparator (SDM_SAMPLE_RATE_HZ) is what's supposed to relocate its
    // switching image, not a faster software write rate - see
    // envelope_output.h's header comment - so this call deliberately does
    // NOT sit inside any of the interpolation paths below, and always
    // fires exactly once per tick regardless of 'I'/curve/HW_FADE state.
    //
    // 2026-09-08, later: briefly split into stage-here/commit-from-ISR (to
    // try to remove dsp_task's own scheduling latency from the write's
    // timing) and then REVERTED back to this single synchronous call - real
    // hardware measured WORSE with the ISR-commit version, not better. See
    // envelope_output.h's own comment on envelope_output_write_sdm() and
    // group_delay_fit_notes.md's matching entry for the two suspected
    // mechanisms (possible flash-resident driver call inside the ISR;
    // dsp_task occasionally staging too late relative to the next tick).
    // No-op when SDM_COMPARISON_ENABLED (config.h) is 0.
    envelope_output_write_sdm(envelope);

#if ENVELOPE_ISR_INTERP_ENABLED
    // 2026-09-29: v6 - see envelope_interp.h's own comment on this flag
    // for the full design/history. Placed BEFORE the s_enabled ('I')
    // check below, deliberately: once this compile-time flag owns the PWM
    // leg, it owns it unconditionally - 'I' toggled the OLD software
    // curve system (HOLD/LINEAR/CATMULL_ROM via compute_ramp_value()),
    // which this mode bypasses entirely, so there is nothing left for 'I'
    // to turn on or off here. envelope_output_isr_stage_step() (envelope_
    // output.h/.cpp) computes this tick's per-fast-tick duty step (float-
    // safe, TASK context) from the plain envelope value - the actual
    // register write happens every fast tick from ISR context via
    // envelope_output_isr_fasttick_step(), called directly from
    // on_timer_alarm() (ssb_mic_test.ino), not from here. s_p0..s_p3/
    // s_m1/s_m2 above are kept updated anyway (same "a few wasted FLOPs"
    // tradeoff ENVELOPE_INTERP_USE_HW_FADE's own branch already accepts)
    // so nothing goes stale if this is ever flipped off without a
    // reflash - though, like HW_FADE, that scenario doesn't actually
    // arise yet since this is a compile-time-only flag.
    envelope_output_isr_stage_step(envelope);
    s_last_value = envelope;
    return;
#endif

    if (!s_enabled) {
        s_last_value = envelope;
        envelope_output_write_pwm(envelope);
        return;
    }

    // Shift the four-sample history and bring in this tick's own value as
    // the NEWEST point (s_p3) - not as the segment about to be rendered.
    // The segment rendered THIS tick is [s_p1,s_p2], one tick further
    // back than v4's [old target, this tick's value] was, because the
    // Catmull-Rom tangent at that segment's own right end (s_m2) needs
    // the NEXT point (s_p3) to already be known - which it only just
    // became, this instant. Arriving at s_p2 only at the far end of the
    // interval is the same intentional v4 property (see header comment);
    // v4.2 just adds one more full tick of that same kind of delay on top
    // of it, for the reason above.
    //
    // v4.4: always done regardless of which curve is selected, even HOLD
    // (which doesn't read s_p0..s_p3/s_m1/s_m2 at all - see below) - a few
    // wasted FLOPs while HOLD is active, but it keeps LINEAR/CATMULL_ROM's
    // history always fresh, so switching curves live via 'C' never renders
    // from stale data regardless of which curve was active a moment ago.
    s_p0 = s_p1;
    s_p1 = s_p2;
    s_p2 = s_p3;
    s_p3 = envelope;
    s_m1 = 0.5f * (s_p2 - s_p0);
    s_m2 = 0.5f * (s_p3 - s_p1);

#if ENVELOPE_INTERP_USE_HW_FADE
    // 2026-09-08: v5 - bypass compute_ramp_value() and the whole curve
    // machinery (CATMULL_ROM/LINEAR/HOLD alike - not just LINEAR, see
    // envelope_interp.h's "known limitation" note) entirely. Under this
    // mode, dsp_task's on_timer_alarm()/is_full_tick logic (see the .ino)
    // guarantee every wake IS a full tick, so there is no separate
    // on_interp_tick() call left to hand sub-steps to - instead, the LEDC
    // hardware fade engine takes this tick's freshly-arrived envelope value
    // and autonomously subdivides the ramp toward it into
    // ENVELOPE_INTERP_FACTOR hardware steps on its own clock (one PWM
    // period apart, now that RSET_MOD_LEDC_FREQ_HZ is commensurate with the
    // tick rate - see that #define's comment), with zero further software/
    // ISR involvement until the NEXT full tick starts a new fade toward
    // whatever value arrives then. s_p0..s_p3/s_m1/s_m2 above are kept
    // updated anyway (same "a few wasted FLOPs" tradeoff v4.4's HOLD mode
    // already accepts) so nothing goes stale if this flag is ever flipped
    // off again at runtime without a reflash.
    envelope_output_start_hw_fade(envelope, ENVELOPE_INTERP_FACTOR);
    s_last_value = envelope;
    return;
#endif

    if (s_curve == ENVELOPE_INTERP_CURVE_HOLD) {
        // v4.4: no ramp, no look-ahead needed for this mode - write the
        // fresh value immediately (zero added latency, same as 'I' off
        // has) rather than going through compute_ramp_value()'s [s_p1,s_p2]
        // segment logic. s_last_value is set here BEFORE returning, so the
        // 3 on_interp_tick() calls that follow this one this period read
        // the correct (this tick's) value via compute_ramp_value()'s own
        // HOLD branch, not last tick's.
        s_last_value = envelope;
        envelope_output_write_pwm(envelope);
        return;
    }

    envelope_output_write_pwm(compute_ramp_value());

    s_last_value = envelope;
}

void IRAM_ATTR envelope_interp_on_interp_tick(void)
{
#if ENVELOPE_INTERP_USE_HW_FADE || ENVELOPE_ISR_INTERP_ENABLED
    // 2026-09-08, extended 2026-09-29 to also cover ENVELOPE_ISR_INTERP_
    // ENABLED: defensive no-op only - should never actually be reached
    // under either mode. dsp_task's is_full_tick is unconditionally true
    // when either flag is on (see the .ino's on_timer_alarm()/dsp_task
    // changes), so on_timer_alarm() never generates the "interp-only"
    // (sub-tick) wakes this function exists to handle in the software-ramp
    // modes - ENVELOPE_ISR_INTERP_ENABLED's own fast-tick stepping happens
    // entirely in envelope_output_isr_fasttick_step(), called directly
    // from on_timer_alarm(), never through this function.
    return;
#endif
    if (!s_enabled || s_reseed_pending) {
        // Either disabled (hold - matches pre-feature ZOH behavior
        // exactly), or a reseed is waiting for the next full tick to
        // apply it (see on_full_tick()) - hold rather than ramp from
        // stale state in the meantime. Either way: no output change.
        return;
    }

    envelope_output_write_pwm(compute_ramp_value());
}

bool envelope_interp_get_enabled(void)
{
    return s_enabled;
}

void envelope_interp_set_enabled(bool enable)
{
    if (enable == s_enabled) {
        return;   // no-op transition, same convention as envelope_gdeq_set_enabled()
    }

    if (enable) {
        s_reseed_pending = true;   // dsp_task applies this on its next full tick - see on_full_tick()
    }

    s_enabled = enable;
}

envelope_interp_curve_t envelope_interp_get_curve(void)
{
    return s_curve;
}

void envelope_interp_set_curve(envelope_interp_curve_t curve)
{
    // Plain store, deliberately no reseed/transient handling - see
    // envelope_interp.h's "v4.3" header note. compute_ramp_value() reads
    // s_curve fresh on every call (interp tick or full tick), so a switch
    // takes effect on the very next write; the only visible effect
    // mid-ramp is that the CURRENTLY-RENDERING segment's remaining samples
    // switch shape (still landing on the same s_p1/s_p2 endpoints either
    // way), not a discontinuity in the underlying state.
    s_curve = curve;
}
