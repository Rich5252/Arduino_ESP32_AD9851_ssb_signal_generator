#pragma once

/**
 * diagnostics.h
 *
 * Everything written by dsp_task purely for visibility (never read back by
 * any control-path logic) plus the periodic [timing]/[adc]/[dsp] print
 * blocks that used to live directly in loop(). This module is allowed to
 * reach into every other module's small getters to print a coherent
 * status picture - nothing else should ever depend on diagnostics.h in
 * return, so it's safe to sit at the "top" of the include graph.
 *
 * The hot-path record_* functions are IRAM_ATTR and called once per
 * dsp_task tick, mirroring exactly where the equivalent inline volatile
 * writes sat in the original dsp_task - see ssb_mic_test.ino's dsp_task()
 * for the call sites.
 */

#include <stdint.h>
#include <stdbool.h>
#include "esp_attr.h"

// Captures the dsp-tick long-window average's starting timestamp. Call
// once from setup(), right after init_sample_timer() - mirrors exactly
// where the original captured s_dsp_tick_start_us.
void diagnostics_init(void);

// Worst-case timing diagnostics for dsp_task. Read/printed via
// diagnostics_service() only (never from dsp_task itself - no Serial
// calls on the real-time path). max_busy_us is a running high-water
// mark, never reset except via diagnostics_reset(), so it captures the
// worst case seen since boot (or since the last reset) even if it only
// happens once. overrun_count increments any sample whose processing
// took longer than one sample period - if this climbs, dsp_task is at
// risk of never yielding back to ulTaskNotifyTake, which starves IDLE0
// and trips the task watchdog.
//
// Called once per dsp_task tick, right after ulTaskNotifyTake() returns -
// updates the tick counter and the wake-up jitter stats (gap since the
// previous tick's t_start_us).
//
// REVERTED here to its pre-Fs-jitter-hunt-diagnostics signature (just
// t_start_us) - a fuller version briefly existed that also correlated
// this against ulTaskNotifyTake()'s elapsed-count return value (a
// diagnostics_record_elapsed_fast_ticks() sibling function, called on
// every wake to catch coalescing/missed wakes), but real hardware showed
// a regression in pin5's own period/pulse-width after that diagnostics
// work was added, and reverting the NEXT layer added on top of it
// (a per-wake residual-lateness measurement) did NOT fix it - so this
// whole layer is backed out down to the last confirmed-good checkpoint
// (the intr_priority=3 fix on its own, before any of this) to test
// whether ANY of it was the actual cause, rather than continuing to
// guess which specific piece. If pin5 comes back clean at THIS
// checkpoint, re-add the elapsed-count tracking piece by piece; if it's
// STILL bad here, the cause isn't in this diagnostics code at all.
void IRAM_ATTR diagnostics_record_tick_start(int64_t t_start_us);

// Called once per dsp_task tick, after computing this tick's phase
// timings (adc_us/dsp_us/write_us/busy_us) - updates the four running
// high-water marks and overrun_count.
// NOTE (2026-09-30): write_us is dsp_task's TAIL phase (after the DSP:
// tx_freq computation, diagnostics, staging stores, DAC submit). With
// AD9851_ISR_WRITE_ENABLED the AD9851 bit-bang runs in the gptimer ISR and
// is NOT part of it; it used to be, which is where the older name comes from.
// adc_us covers the top-of-tick work including the task-side envelope/PWM
// write (envelope_interp_on_full_tick()) as well as the ADC read.
void IRAM_ATTR diagnostics_record_phase_timings(uint32_t adc_us, uint32_t dsp_us,
                                                 uint32_t write_us, uint32_t busy_us);

// Core 1 breakdown - "where does Core 1's time actually go". Called
// once per loop() iteration (Core 1, NOT a hot real-time path - safe to
// be a plain function, no IRAM_ATTR needed), right after
// handle_serial_commands()/adc_capture_service()/diagnostics_service()/
// delay(10) have all run, with each one's own wall-clock duration this
// iteration. Unlike diagnostics_record_phase_timings() above (worst-case
// high-water marks), these accumulate as SUMS over the reporting window
// - the question here is which of these is eating the most total time,
// not what any single call's worst case was.
//
// delay_us matters more than it looks: the very first version of this
// breakdown didn't bracket delay(10) at all, and the numbers it produced
// (idle=3.0-3.7% via the Core-1 idle hook, cmd/adc_svc/diag all under
// 1%, "other"=96.2%) looked like Core 1 was almost completely saturated
// by something unaccounted-for. Adding this bucket was the fix, not a
// vTaskList()-found rogue task (there wasn't one - see the 'L' serial
// command's task list, which came back with exactly the expected 8
// tasks and nothing resembling an Arduino "events" task, since this
// project never touches WiFi/BT and that task is only ever created
// lazily by the code paths that do). delay(10) SHOULD dominate every
// loop() iteration - it's the only other thing loop() does - so
// bracketing it directly should collapse "other" down near zero and
// show that the real idle time was there all along; the [core1] idle
// hook reading just wasn't seeing most of it (see core1_idle_hook()'s
// CORE1_IDLE_GAP_THRESHOLD_US comment - a real blocked wait's idle-hook
// calls are spaced by the OS tick period, not microseconds apart, so a
// 10us gap threshold excludes nearly all of a genuine 10ms block).
void diagnostics_record_core1_loop_timings(uint32_t cmd_us, uint32_t adc_svc_us,
                                            uint32_t diag_us, uint32_t delay_us);

// Called once per dsp_task tick with the final (post-gdeq, post-PWM-
// mapping) envelope and the pre-delay freq_dev_hz - same values the
// original stored in s_dbg_envelope/s_dbg_freq_dev.
void IRAM_ATTR diagnostics_set_envelope_freqdev(float envelope, float freq_dev_hz);

// AD9851-only: the delayed freq_dev_hz/envelope actually used (post
// relative_delay_apply()) and the exact integer Hz sent to the chip.
// delayed_envelope was added 2026-09-12 alongside the per-event jump log
// below and is kept here for a still-unimplemented future use (a
// post-delay null_bias variant, proposed in null_bias_investigation.md) -
// it is NOT what the jump log's near_null classification uses.
//
// envelope_at_freq_time (2026-09-12, same day, CORRECTION): the jump log's
// first bench result (503384 events, 0% near_null, at relative_delay=+4.60)
// exposed a real bug in using delayed_envelope for that classification -
// see relative_delay_apply()'s declaration comment (relative_delay.h) for
// why delayed_envelope isn't time-matched to delayed_freq_dev_hz once
// delay is large. envelope_at_freq_time IS time-matched (always read at
// the same lag freq_dev_hz was), and is what the near_null flag/trace/log
// below now use instead.
//
// envelope_at_freq_time_min (2026-09-12, later same day): the SECOND real
// bench capture (delay=+0.90, 25693 events, 25% near_null, a clean
// repeating 3-state cycle where only 1 of the 3 states classified
// near_null) exposed a further subtlety - at a lopsided fractional delay,
// the blended envelope_at_freq_time can miss a near-null RAW sample that
// only got a small minority weight in the interpolation. This is the
// smaller of the two raw samples that blend actually mixes together - see
// relative_delay_apply()'s declaration comment for the full reasoning.
// Used for a SECOND, more permissive near_null_either flag alongside the
// original (now near_null_blended) one, so both questions - "was the
// transmitted result near a null" and "did anything near a null
// contribute to it at all" - are answered per event, not just one.
//
// raw_freq_dev_near/_far (2026-09-12, later same day): the two RAW,
// undelayed freq_dev_hz ring values that interp_ring() blended together to
// produce delayed_freq_dev_hz - see relative_delay_apply()'s declaration
// comment (relative_delay.h) for the full reasoning. Added after a
// delay=+4.28 capture (134573 events, 2% near_null_blended, 34%
// near_null_either) decoded into a clean repeating 3-state cycle where 2
// of the 3 transitions had a near-null contributor caught only by the
// more permissive near_null_either test, but the THIRD and LARGEST
// transition (~5761Hz) showed no near-null involvement by either test.
// These two raw values answer the next question before jumping to "so
// there's a null-independent mechanism": do the two individual,
// undelayed samples already differ by roughly the logged step size (a
// real discontinuity exists in the raw signal itself, just not one an
// envelope-near-null test happens to flag), or are they both
// individually unremarkable (meaning the large DELAYED step is an
// interpolation artifact from blending across a multi-sample lag during
// a fast-changing part of the waveform, not evidence of any discrete
// event at all)? Logged and printed alongside the rest of the jump-log
// entry, not used for any near_null classification of their own.
// raw_freq_dev_current/raw_envelope_current (2026-09-16, later still): this
// tick's own freq_dev_hz/envelope EXACTLY as ssb_dsp_process_sample() (or
// the isolation-test modes) produced them - before envelope_floor/gdeq/
// ampeq/predistort/the PWM offset-scale mapping reshape `envelope`, and
// before relative_delay_apply() re-times either one. Added specifically to
// build a second, correctly-paired energy-weighted fast EMA alongside the
// existing one (see s_freq_ema_energy_num_fast's declaration comment,
// diagnostics.cpp): a same-instant cross-check against ssb_dsp.c's own
// already-validated env2_dphi_sum/env2_sum accumulator
// (log_20260916_212424.txt) showed that accumulator - which pairs envelope
// with dphi at the exact same raw tick - stays stable within a few Hz while
// the existing fast EMA (which pairs delayed_freq_dev_hz with the SHAPED
// envelope_at_freq_time) swings by 80+Hz between consecutive readings on an
// unchanging bench setup. These two raw values let a second fast EMA use
// the same raw-tick pairing ssb_dsp.c's own validated accumulator does,
// without waiting on that accumulator's own lifetime-averaged (slow-to-
// respond) convergence. Two known simplifications, not yet resolved:
// (1) this pairing is deliberately PRE-delay (matching ssb_dsp.c's own
// choice) rather than routed through relative_delay's ring the way the
// existing envelope_at_freq_time-based EMA is - correct at delay=0,
// an approximation otherwise, though relative_delay's typical values (a
// few samples) are small next to a full null-crossing width; (2)
// raw_freq_dev_current is this function's OUTPUT freq_dev_hz - already
// past ssb_dsp_process_sample()'s own slew-limit/clamp/LSB-sign-flip,
// unlike the true pre-clamp dphi ssb_dsp.c's own accumulator uses - a
// difference that should only matter on the rare tick where that clamp
// actually engages.
void IRAM_ATTR diagnostics_set_tx_info(float delayed_freq_dev_hz, float delayed_envelope,
                                        float envelope_at_freq_time, float envelope_at_freq_time_min,
                                        float raw_freq_dev_near, float raw_freq_dev_far,
                                        float raw_freq_dev_current, float raw_envelope_current,
                                        uint32_t tx_freq);

// 2026-09-12: second half of the per-event jump log - call once per
// dsp_task tick, AFTER busy_us for this tick is known (ssb_mic_test.ino,
// right after diagnostics_record_phase_timings()). No-op on every tick
// except the rare one where diagnostics_set_tx_info() just above flagged a
// qualifying jump (see JUMP_LOG_THRESHOLD_HZ in diagnostics.cpp) - this
// call supplies the one piece of context not yet available at that
// earlier point in the tick (this tick's own DSP busy time, which a
// timing-domain cause would show up in directly) and finalizes the log
// entry. AD9851-only, same as the rest of this feature.
void IRAM_ATTR diagnostics_record_jump_busy_us(uint32_t busy_us);

// 2026-09-19: fold-back of the write-time-jitter fix's staging-race
// diagnostic (config.h's AD9851_ISR_WRITE_ENABLED comment; ssb_mic_test.ino's
// s_stage_seq/s_stale_commit_count) - call once per dsp_task full tick with
// the CURRENT cumulative count (not a delta - this just overwrites the
// printed value each call, same "latest snapshot" convention as the
// wakeup-jitter high-water marks). Deliberately NOT reset by
// diagnostics_reset()/'r' - this is a since-boot "has this ever happened at
// all" signal, not a per-capture performance metric, so a reset can't make
// it stale-looking while dsp_task's own real, un-reset counter keeps
// climbing underneath it. Printed in the periodic [timing] block - see
// print_timing_and_adc_block(). IRAM_ATTR: called once per full tick from
// dsp_task, which is itself IRAM_ATTR - every function it calls needs to be
// too, same reasoning as dsp_task's own declaration comment
// (ssb_mic_test.ino), even though this one's own body is just a volatile
// store with no real-time sensitivity of its own.
void IRAM_ATTR diagnostics_record_isr_stale_commits(uint32_t stale_commit_count);

// 'J' serial command - dumps the aggregate near-null/total jump counts
// plus each of the last JUMP_LOG_LEN qualifying events (oldest to newest)
// to Serial. Only ever called from Core 1's handle_serial_commands()
// context, never the dsp_task hot path - see diagnostics.cpp for format.
void diagnostics_print_jump_log(void);

// 2026-09-12, yet later still: 'K' serial command - a SEPARATE, much
// slower-timescale trigger from the 'J' log above, built after the user
// reported (and then directly confirmed on the bench) that 'J' is the
// wrong tool for "what changed to the frequency I can actually see/hear."
// 'J' fires on every beat-null crossing - hundreds to thousands of times
// a second, per the captures in moving_forward_notes.md/
// null_bias_investigation.md's 2026-09-12 entries - so by the time a
// human reacts to an observed shift and reads 'J', its ring has wrapped
// many times over with unrelated routine churn; a capture taken
// deliberately right after Aux SP showed a real 1000->962Hz shift came
// back showing the exact same 3-state cycle as every "nothing happened"
// capture before it, carrying no signal at all about when the
// human-perceptible shift actually occurred.
//
// This instead tracks a fast/slow EMA pair of delayed_freq_dev_hz (fast
// tau long enough to average out one beat-null cycle several times over;
// slow tau ~2s, lagging behind as "where this has been sitting") and
// LATCHES a coarse, before-and-after binned trace the moment they diverge
// by more than SLOW_JUMP_TRIGGER_HZ (diagnostics.cpp) - calibrated to the
// user's own independently-reported +/-5Hz visual-read tolerance on Aux
// SP, not a DSP-internal number, i.e. tuned to "would a human watching
// the display actually see this." Prints a live "still watching"
// fast/slow/delta readout if nothing has triggered yet, or the full
// latched pre/post trace if it has, then re-arms (and resyncs the slow
// EMA to the fast one, to avoid an immediate re-trigger storm while the
// slow EMA is still catching up) so the next event isn't missed while
// this one's being read. Only ever called from Core 1's
// handle_serial_commands() context - see diagnostics.cpp for format.
//
// 2026-09-15: a latched trace no longer strictly needs this keypress to be
// read - diagnostics_service() now also drains a LATCHED trace on its own,
// every loop() iteration, printing the identical trace with an
// "AUTO-CAPTURED" header instead of "TRIGGERED" (see
// diagnostics_check_slow_trace_auto_dump(), file-local to diagnostics.cpp,
// for the full rationale: leaving a bench run unattended and letting hard-
// to-catch hands-off jumps land in the serial log by themselves). This
// function's own live "still watching" readout is unaffected and remains
// 'K'-only - the auto-dump never prints that, only a completed capture.
void diagnostics_print_slow_trace(void);

// 2026-09-15: 'H' serial command - a second, independent detector alongside
// 'K' above, added after the user pointed out a real gap: they're watching
// the actual transmitted frequency on an SDR and reporting cases where it
// jumps and then SITS at the new value for a long time (their own example:
// "-22Hz, been there a while") - which 'K' can't reliably see, because 'K'
// only fires on FAST-vs-SLOW divergence and its own re-arm resyncs slow to
// fast every time it's read. If fast and slow ever drift together slowly
// enough to never re-open a gap between EACH OTHER, 'K' goes silent even
// while sitting far from where the run actually started.
//
// This instead compares the fast EMA against a PERMANENT anchor - snapped
// once from fast at the same boot-settle instant 'K's slow EMA gets its own
// one-time snap, and never touched again - so a genuine sustained
// departure from where this run settled is visible no matter how gradually
// it got there. Fires a "CONFIRMED STUCK" print (plus a dump of an always-
// running rolling trace of the actual tx_freq/fast_hz, coarse - 2
// samples/sec over the last 60s - specifically so it isn't fooled by an
// SDR/FFT display's own exponential-average "trail" the way a fine
// per-tick trace might invite comparison against) once the deviation has
// been continuous for at least HELD_MIN_DURATION_MS (diagnostics.cpp,
// currently 15s - well past how long this session's own data shows an
// ordinary reverting 'K'-style jump actually lasts, so this doesn't just
// re-detect the same routine cycles 'K' already catches), a periodic
// "still stuck" heartbeat every HELD_REANNOUNCE_MS while it remains so,
// and a "RECOVERED" print with total duration once it clears. Auto-prints
// on its own from diagnostics_service() exactly like 'K's auto-dump (see
// diagnostics_check_held_freq(), file-local to diagnostics.cpp) - 'H'
// itself is a manual, on-demand, non-destructive status/trace read (no
// re-arm needed - the anchor is permanent and the rolling trace always
// runs regardless of whether anyone reads it).
//
// 2026-09-15, later same day: every line this prints (and every auto-fired
// CONFIRMED STUCK/still-stuck/RECOVERED line) now carries an explicit
// t=%ums, directly comparable to a 'K' trace's own "at t=%ums" - added
// after a live case where it mattered whether an 'H' read genuinely showed
// nothing during a real sustained jump, or was just read too long after
// the jump to still see it; without a timestamp on the 'H' side there was
// no way to tell those two apart from the log alone.
void diagnostics_print_held_status(void);

// 2026-09-17: 'F' serial command - full-rate (every dsp_task tick, ~16kHz)
// capture of raw_freq_dev_current/raw_envelope_current for about one
// second, dumped to Serial as CSV afterward. Added directly in response
// to the user's own question ("would it be worth doing a dump of all
// freq/env values every sample for a second or so to check they are the
// same values you expect") after the same-turn 'J' jump-log capture
// turned out to be a single point-in-time snapshot (one AUTO-CAPTURED
// slow_trace event + one manual 'J' dump, both isolated instants) -
// unable to test whether the ~0.485s/~1.000s warble's underlying
// near-null events actually recur at a specific interval, which needs
// many precisely-timestamped samples in a row to check, not one snapshot
// (see null_bias_investigation.md's 2026-09-17 "log correction" entry).
// This instead captures the ACTUAL per-tick freq_dev/envelope stream
// ground truth, so that recurrence (or its absence) can be measured
// directly instead of inferred from an SDR-audio proxy.
//
// Lazily malloc's a ~250KB buffer (2 floats/sample x 32000 samples, ~2.0s
// at 16kHz - bumped from an original 1.0s/~125KB after the user reported
// this board's actual build output, "leaving 299220 bytes for local
// variables, maximum 327680" - ~292KB free at link time against a 320KB
// chip, comfortably more than the original size needed, though this pool
// is also shared with every task's stack and other libraries' own heap
// use at runtime, so 2.0s deliberately leaves real margin rather than
// spending the whole reported figure) only once armed, and frees it again
// once the dump completes - deliberately NOT a permanent static array
// like this file's other trace buffers (HELD_TRACE_LEN/FREQ_STEP_TRACE_LEN,
// both well under 1KB), since this board (ESP32-S3 Super Mini, config.h)
// is assumed to have no PSRAM and a standing quarter-megabyte tax on
// internal SRAM for a diagnostic used only occasionally, on demand, would
// be a bad trade. If armed while already armed/ready/dumping, or if the
// malloc fails (not enough free heap right now - the graceful fallback
// this sizing choice is allowed to lean on, see the FREQENV_CAPTURE_LEN
// comment in diagnostics.cpp), prints a short status/error line and does nothing
// destructive - never double-allocates or leaks a half-succeeded pair.
//
// The actual per-tick capture write lives inline in
// diagnostics_set_tx_info() just above (same AD9851-only hot-path hook
// every other per-tick feature in this file uses), and the dump itself is
// chunked across diagnostics_service() calls (see
// diagnostics_freqenv_capture_service(), file-local to diagnostics.cpp) -
// same diag_room_for() TX-buffer-safety pattern as every other print in
// this file, so a slow/backlogged host just makes the dump take longer
// wall-clock rather than blocking dsp_task or being lost.
void diagnostics_freqenv_capture_arm(void);

// Zeros every counter/high-water-mark this module owns and restarts the
// dsp-tick long-window average from now. Does NOT touch any other
// module's diagnostics - the 'r' serial handler calls
// adc_capture_reset_diag() and ssb_dsp_reset_freq_dev_stats() separately,
// same as the original 'r' handler touched every counter inline.
void diagnostics_reset(void);

bool diagnostics_get_muted(void);
void diagnostics_toggle_muted(void);

// 2026-09-18: SEPARATE mute for the three "auto dump" background watchers -
// canary_check_background(), diagnostics_check_slow_trace_auto_dump()
// ('K's automatic side), and diagnostics_check_held_freq() ('H's automatic
// side) - deliberately NOT folded into diagnostics_toggle_muted()/'v'
// above. Those three were built specifically to be mute-EXEMPT (see each
// one's own declaration comment in diagnostics.cpp): they exist to catch
// rare, hands-off events (a corrupted canary, a big jump, a stuck
// frequency) during an unattended capture, and silently disabling that
// safety net as a side effect of muting the routine 1Hz/45ms status noise
// would be an easy way to lose exactly the event you were trying to catch.
// This toggle is the deliberate, explicit opt-in for "I know what these
// are for and I still want them quiet right now" - added on request to let
// the user silence ALL auto-printed diagnostic output (both categories)
// during normal, uneventful operation.
//
// Caveat worth knowing before using this: for the three watchers this
// covers, detection and the print happen in the same gated call (unlike
// the periodic block, which keeps counting while just skipping the print).
// While this is on, none of the three even CHECK for their trigger
// condition, so a canary mismatch's "first seen at t=...ms" timestamp (if
// one fires while muted) will read as whenever this is next turned off and
// the check resumes, not the true onset - the underlying corrupted/latched
// state itself isn't lost (these conditions are all designed to persist
// until read, not to self-heal), just the exact onset time. Same
// unmute-and-it-appears behavior for slow_trace/held_freq: a LATCHED
// slow_trace or a confirmed-stuck held_freq episode simply waits, un-
// rearmed/unprinted, until this is turned back off. Manual on-demand reads
// ('K', 'H', diagnostics_print_now()/'D') are untouched by this flag either
// way, same as they already ignore diagnostics_get_muted().
bool diagnostics_get_autodump_muted(void);
void diagnostics_toggle_autodump_muted(void);

// Call once per loop() iteration. Internally throttles: prints the
// envelope/freq_dev/dac_code status line at ~45ms intervals and the full
// [timing]/[adc]/[dsp] block at ~1000ms intervals, both gated on the mute
// flag - exactly the two timed blocks the original loop() had inline,
// just moved here so loop() doesn't need its own timing-gate statics.
void diagnostics_service(void);

// Prints one status line + one [timing]/[adc]/[dsp] block immediately,
// ignoring both the mute flag and the 45ms/1000ms throttle intervals -
// 'V' (see serial_commands.cpp) wires this to a keystroke. For grabbing
// an exact reading on demand (e.g. right after toggling something, or
// right after un-muting from a deliberately-silent measurement window)
// instead of waiting for/scrolling through the periodic stream. Doesn't
// reset any counters and doesn't disturb diagnostics_service()'s own
// independent timing - purely an extra read.
void diagnostics_print_now(void);
