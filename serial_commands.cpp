/**
 * serial_commands.cpp - see serial_commands.h.
 */

#include "serial_commands.h"
#include "config.h"
#include "dsp_state.h"
#include "adc_capture.h"
#include "envelope_gdeq.h"
#include "envelope_ampeq.h"
#include "envelope_alc.h"
#include "envelope_softlimit.h"
#include "envelope_predistort.h"
#include "envelope_floor.h"
#include "envelope_output.h"
#include "envelope_interp.h"
#include "diagnostics.h"
#include "settings.h"
#include "ssb_dsp.h"
#include "test_signals.h"
#if AD9851_ATTACHED
#include "relative_delay.h"
#include "carrier_output.h"
#endif
#include <Arduino.h>
#include <cstdarg>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   // vTaskList() - see the 'L' handler below

// Every command reply in this file is sent through serial_reply() below,
// rather than calling Serial.print()/println()/printf() directly - this
// is what fixes a real, reproducible bug found on real hardware: the 'I'
// command would sometimes not reply when switching interpolation ON, and
// then send TWO replies together on the next 'I' (switching off).
//
// Root cause: "-> 4x envelope output interpolation ON (see
// envelope_interp.h)\r\n" is exactly 64 bytes long - and 64 bytes is the
// USB full-speed CDC bulk endpoint's max packet size on the ESP32-S3's
// native USB. USB CDC marks the end of a transfer with either a "short"
// packet (fewer bytes than the endpoint's max packet size) or an
// explicit trailing zero-length packet (ZLP). When a transfer's total
// length is an EXACT multiple of 64, neither happens on its own - the
// host's CDC-ACM driver can't tell whether that 64-byte chunk is the
// whole message or just the first packet of a longer one, so it holds
// the data back rather than handing it to the terminal application
// (Arduino IDE Monitor, PuTTY, or a custom logger - all three showed the
// identical symptom, which is what pointed away from anything
// app-specific and toward this transport-layer explanation). Only once
// MORE bytes are queued behind it - i.e. the NEXT reply - does the total
// stop being a clean 64-byte multiple, and both replies get delivered
// together. This is a widely-reported, generic USB CDC-ACM behavior, not
// anything specific to this board (see e.g. hathach/tinyusb#2041,
// STMicroelectronics/STM32CubeF3#2). Serial.flush() does NOT fix it: by
// the time flush() runs, the exact-64-byte packet has already gone out
// the door - flush() just waits for the TX queue to drain, it can't
// retroactively add a terminator to a transfer the device already
// considers complete.
//
// The 'I' "off" reply happened to be 65 bytes (not a multiple of 64), so
// it always flushed cleanly on its own - which is why the bug looked
// like it only affected switching ON. The actual trigger was just that
// one reply's coincidental byte count, not anything about ON vs off.
//
// Rather than pad that one string, every reply in this file is routed
// through here: it formats like printf, then pads the transmitted length
// by one harmless trailing space whenever it would otherwise land
// exactly on a 64-byte boundary. That means no future wording change,
// digit-width change (e.g. a different ENVELOPE_INTERP_FACTOR), or any
// other command's variable-length numeric field can silently
// reintroduce this same bug somewhere else in this file.
static void serial_reply(const char *fmt, ...)
{
    // 2026-09-02: bumped 256 -> 512 - the 'w' reply (serial_commands.cpp's
    // AUDIO_SRC_CHIRP handler) grew to ~500 bytes once it started reporting
    // 'g' state too, real hardware confirmed this silently TRUNCATED the
    // vsnprintf() output below (missing trailing \r\n - the exact symptom:
    // "I see first response but no \r\n"). The follow-up "second w gives no
    // response" is unrelated and NOT a bug: every source-switch command in
    // this file (t/s/m/p/y/h/w) guards on `src != TARGET`, so re-sending
    // 'w' while already in AUDIO_SRC_CHIRP is a deliberate, silent no-op -
    // same behavior 't' or 'm' would show if sent twice in a row. 512 gives
    // real headroom over every current reply in this file; the truncation
    // guard below still fires and the debug print makes it visible if any
    // future reply ever needs more.
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len < 0) {
        return;   // formatting error - nothing sane to send
    }
    if ((size_t)len >= sizeof(buf)) {
        // Was a SILENT truncation until 2026-09-02 (see this function's own
        // comment above for how that manifested) - now at least visible.
        Serial.printf("[serial_reply] WARNING: reply truncated, needed %d bytes, buf is %u\r\n",
                      len, (unsigned)sizeof(buf));
        len = (int)sizeof(buf) - 1;   // truncated - still send what fit
    }

    if (len > 0 && (len % 64) == 0 && (size_t)len < sizeof(buf) - 1) {
        // Insert the padding space just before the trailing "\r\n" every
        // reply in this file ends with (rather than appending after it),
        // so it never leaks onto the front of whatever gets printed next.
        int insert_at = (len >= 2 && buf[len - 2] == '\r' && buf[len - 1] == '\n')
                         ? len - 2 : len;
        memmove(&buf[insert_at + 1], &buf[insert_at], (size_t)(len - insert_at) + 1);
        buf[insert_at] = ' ';
        len++;
    }

    Serial.write((const uint8_t *)buf, (size_t)len);
}

// Returns the C symbol name (e.g. "AUDIO_SRC_MIC") for a given
// audio_source_t, for the 'P' settings-dump command below - distinct from
// dsp_state.h's audio_source_name(), which returns a human-readable label
// ("mic", "TWO-TONE TEST", ...) that isn't valid C and can't be pasted
// into a PersistentSettings initializer.
static const char *audio_source_enum_name(audio_source_t src)
{
    switch (src) {
        case AUDIO_SRC_MIC:        return "AUDIO_SRC_MIC";
        case AUDIO_SRC_TWOTONE:    return "AUDIO_SRC_TWOTONE";
        case AUDIO_SRC_SINGLETONE: return "AUDIO_SRC_SINGLETONE";
        case AUDIO_SRC_ENVSTEP:    return "AUDIO_SRC_ENVSTEP";
        case AUDIO_SRC_FMTEST:     return "AUDIO_SRC_FMTEST";
        case AUDIO_SRC_AMTEST:     return "AUDIO_SRC_AMTEST";
        case AUDIO_SRC_CHIRP:      return "AUDIO_SRC_CHIRP";
        default:                   return "AUDIO_SRC_MIC";
    }
}

// Working duty value for 'd'/'>'/'<'/'N'/'B' (direct duty override, see
// envelope_output.h) - lives here, not in envelope_output.cpp, since it's
// only ever touched from this file's own single-threaded command
// handling (loop()/Core 1); envelope_output.cpp only needs to know the
// override's on/off state, not the stepping value itself.
static uint32_t s_duty_override_value = 0;

// Added 2026-09-07 alongside 'E' - see duty_override_write_and_reply()'s
// own comment for the full rationale. false (default) = original 'd'
// behavior unchanged (s_duty_override_value IS the raw duty count,
// bypassing envelope_predistort/pwm_offset/pwm_scale entirely - this is
// what built envelope_predistort's LUT in the first place, REVISION 4/5,
// envelope_predistort.h, and stays available for re-characterizing that
// raw curve). true = s_duty_override_value is instead treated as a
// commanded envelope index, run through the SAME mapping dsp_task's
// normal per-tick path uses, so 'D' (predistort) actually has something
// to act on during a duty-style up/down sweep.
static bool s_duty_override_use_envelope_mapping = false;

// Shared by 'd' (on-enable) and '>'/'<'/'N'/'B' - computes and writes the
// actual RSET duty for the current s_duty_override_value/
// s_duty_override_use_envelope_mapping state, then reports it. Pulled out
// into one function (2026-09-07) so all five call sites stay in sync
// automatically rather than needing five copies of the same raw-vs-
// envelope-mapping branch kept manually consistent.
//
// Why this exists: 'd' direct duty override was built (see its own
// comment below) specifically to bypass the envelope pipeline - including
// 'D' predistort - so the raw, uncorrected duty->RF-output curve could be
// characterized on its own terms. That's the right tool for BUILDING the
// predistort LUT, but it means toggling 'D' while in plain 'd' override
// mode can never show any difference - there's no mapping stage left for
// it to correct. 'E' (see its own handler) flips s_duty_override_
// use_envelope_mapping so the SAME up/down stepper - and any existing
// automated test harness already driving those same keys - can instead
// validate 'D' against real hardware: the stepped index becomes a
// commanded envelope (0..1, same 0..max_duty granularity as before, so no
// change needed to how the index itself gets driven), mapped through
// envelope_predistort_process() if 'D' is on or the plain pwm_offset/
// pwm_scale linear mapping if it's off - genuinely exercising the same
// branch dsp_task's normal per-tick path takes - before being converted
// to a duty count and written. Requested directly: "check the distortion
// correction... put in loop (on/off) for the duty measurement ('d')" -
// user already has an automated harness driving the existing up/down
// stepper and plans to run it once with 'D' on and once off; this is what
// makes that comparison show a real difference instead of a guaranteed
// null result. See group_delay_fit_notes.md's matching entry.
//
// 2026-09-08: reply text made peripheral-aware - envelope_output_write_
// duty_raw()/get_max_duty() (envelope_output.h) now drive SDM's density
// instead of LEDC's duty count when SDM_COMPARISON_ENABLED is the one
// compiled in (the two are mutually exclusive), so a reply that always
// said "duty" would be actively misleading while running an SDM
// linearity sweep with this same stepper. `max_duty` is still the generic
// "max override index" either way; the density shown alongside it is
// recovered from the SAME index<->density affine mapping envelope_output.
// cpp's own write_duty_raw()/write_sdm() use (index - max_duty/2, exact
// since max_duty is always 2*SDM_DENSITY_CLAMP under SDM), purely so the
// printed number matches what's actually landing on the pin - it doesn't
// change what gets written, only what gets logged.
static void duty_override_write_and_reply(void)
{
    uint32_t max_duty = envelope_output_get_max_duty();
    if (!s_duty_override_use_envelope_mapping) {
        envelope_output_write_duty_raw(s_duty_override_value);
#if SDM_COMPARISON_ENABLED
        int32_t density = (int32_t)s_duty_override_value - (int32_t)(max_duty / 2u);
        serial_reply("-> index %lu/%lu -> density %ld (raw)\r\n",
                      (unsigned long)s_duty_override_value, (unsigned long)max_duty, (long)density);
#else
        serial_reply("-> duty %lu/%lu (raw)\r\n",
                      (unsigned long)s_duty_override_value, (unsigned long)max_duty);
#endif
        return;
    }
    // Envelope-mapping mode - same normalization/mapping/clamp order as
    // dsp_task's own per-tick path (ssb_mic_test.ino), just entered from a
    // manually-stepped index instead of the live envelope pipeline.
    float envelope_frac = (float)s_duty_override_value / (float)max_duty;
    float mapped;
    if (envelope_predistort_get_enabled()) {
        mapped = envelope_predistort_process(envelope_frac);
    } else {
        mapped = envelope_frac * envelope_output_get_pwm_scale() + envelope_output_get_pwm_offset();
    }
    if (mapped < 0.0f) mapped = 0.0f;
    if (mapped > 1.0f) mapped = 1.0f;
    uint32_t duty = (uint32_t)(mapped * (float)max_duty);
    envelope_output_write_duty_raw(duty);
#if SDM_COMPARISON_ENABLED
    int32_t density = (int32_t)duty - (int32_t)(max_duty / 2u);
    serial_reply("-> env index %lu/%lu (frac=%.4f) -> index %lu/%lu -> density %ld (%s)\r\n",
                  (unsigned long)s_duty_override_value, (unsigned long)max_duty, envelope_frac,
                  (unsigned long)duty, (unsigned long)max_duty, (long)density,
                  envelope_predistort_get_enabled() ? "predistort ON" : "linear mapping, 'D' off");
#else
    serial_reply("-> env index %lu/%lu (frac=%.4f) -> duty %lu/%lu (%s)\r\n",
                  (unsigned long)s_duty_override_value, (unsigned long)max_duty, envelope_frac,
                  (unsigned long)duty, (unsigned long)max_duty,
                  envelope_predistort_get_enabled() ? "predistort ON" : "linear mapping, 'D' off");
#endif
}

void handle_serial_commands(void)
{
    // Runtime source switch: 't' -> two-tone, 's' -> single-tone, 'm' ->
    // live mic, 'p'/'y'/'h' -> isolation tests. dsp_task reads the audio
    // source fresh every tick, so this takes effect on the very next
    // sample - no glitch/restart needed. Unrecognized bytes (e.g. line
    // endings from some serial monitors) are silently ignored rather than
    // echoed as an error.
    while (Serial.available()) {
        char c = Serial.read();
        audio_source_t src = dsp_state_get_audio_source();

        if (c == 't' && src != AUDIO_SRC_TWOTONE) {
            dsp_state_set_audio_source(AUDIO_SRC_TWOTONE);
            serial_reply("-> two-tone test signal\r\n");
        } else if (c == 's' && src != AUDIO_SRC_SINGLETONE) {
            dsp_state_set_audio_source(AUDIO_SRC_SINGLETONE);
            serial_reply("-> single-tone test signal (%.0fHz)\r\n", SINGLETONE_HZ);
        } else if (c == 'm' && src != AUDIO_SRC_MIC) {
            dsp_state_set_audio_source(AUDIO_SRC_MIC);
            // 2026-09-21: re-baseline the ADC resampler's calibration
            // window on every entry into mic mode - adc_continuous keeps
            // running (and counting samples) regardless of audio source,
            // but the resampler's own call-count only advances in mic
            // mode, so a stale baseline here would corrupt the next
            // calibration's measured ratio. See adc_capture.h's own
            // comment on adc_capture_resample_prime_calibration().
            adc_capture_resample_prime_calibration();
            serial_reply("-> live mic input\r\n");
        } else if (c == 'p' && src != AUDIO_SRC_ENVSTEP) {
            dsp_state_set_audio_source(AUDIO_SRC_ENVSTEP);
            serial_reply("-> envelope step test (%.1fHz square wave, carrier fixed - "
                          "measure the RSET node's rise/settling time against this edge)\r\n", ENVSTEP_HZ);
        } else if (c == 'y' && src != AUDIO_SRC_FMTEST) {
            dsp_state_set_audio_source(AUDIO_SRC_FMTEST);
            serial_reply("-> FM isolation test (%.0fHz sine mod, %.0fHz peak deviation, beta=%.2f - "
                          "expect FM sidebands at fc+/-n*%.0fHz, no envelope content - "
                          "isolates AD9851/SPI chain from Hilbert/DSP math)\r\n",
                          FM_TEST_MOD_HZ, FM_TEST_DEV_HZ, FM_TEST_DEV_HZ/FM_TEST_MOD_HZ, FM_TEST_MOD_HZ);
        } else if (c == 'h' && src != AUDIO_SRC_AMTEST) {
            dsp_state_set_audio_source(AUDIO_SRC_AMTEST);
            serial_reply("-> AM isolation test (%.0fHz sine mod, %.0f%% depth, carrier fixed - "
                          "expect ONLY fc+/-%.0fHz sideband pair, no FM content - "
                          "isolates RSET/PWM/filter path from AD9851/DSP)\r\n",
                          AM_TEST_MOD_HZ, AM_TEST_DEPTH * 200.0f, AM_TEST_MOD_HZ);
        } else if (c == 'w') {
            // Sine-chirp test mode - see test_signals.h/config.h's CHIRP_*
            // constants and the .ino's dsp_task early-intercept block.
            // Resets the sweep's phase/elapsed-time state on every entry
            // so 'w' always starts a clean sweep from t=0 (mute period
            // first), never resuming mid-sweep from a previous session.
            //
            // 2026-09-02: deliberately NOT gated on `src != AUDIO_SRC_CHIRP`
            // the way every other source-switch command above is (t/s/m/p/
            // y/h all no-op if already in their target mode) - real
            // hardware use turned up a genuine reason 'w' needs to differ:
            // the 'g' group-delay-equalizer A/B workflow (toggle 'g', then
            // re-run 'w' to compare compensated vs raw TF) NEEDS a fresh,
            // synced restart every time, even while already mid-sweep -
            // otherwise you're stuck waiting up to the full sweep cycle
            // (CHIRP_SWEEP_SEC, or 2x that with CHIRP_BIDIRECTIONAL) for
            // the next natural mute/sync marker after toggling 'g', rather
            // than getting an immediate clean trigger point for
            // the TF rig. Re-sending 'w' while already sweeping is now a
            // real, useful "restart now" action, not a redundant re-select.
            // NOTE: CMD_DEBUG_PIN_ENABLED (config.h) must be 0 for the
            // chirp's square-wave reference on pin39 to be glitch-free -
            // it shares that physical pin with TIMING_DEBUG_GPIO_CMD.
            //
            // 2026-09-05: total cycle time is now CHIRP_MUTE_SEC +
            // CHIRP_SWEEP_SEC (up leg only) or CHIRP_MUTE_SEC +
            // 2*CHIRP_SWEEP_SEC (up+down, CHIRP_BIDIRECTIONAL - see
            // config.h) - the status line below reports total sweep time,
            // not just one leg, so it matches how long a full 'w' capture
            // actually takes on the bench.
            test_signals_chirp_reset();
            dsp_state_set_audio_source(AUDIO_SRC_CHIRP);
#if CHIRP_BIDIRECTIONAL
            float chirp_total_sweep_sec = 2.0f * CHIRP_SWEEP_SEC;
            const char *chirp_shape = "up+down";
#else
            float chirp_total_sweep_sec = CHIRP_SWEEP_SEC;
            const char *chirp_shape = "up only";
#endif
#if CHIRP_SWEEP_LOG
            const char *chirp_law = "log";
#else
            const char *chirp_law = "linear";
#endif
            serial_reply("-> sine chirp test (%.0fHz-%.0fHz %s sweep, %.1fs [%s], %.0fms mute/sync, ref pin%d, "
                          "%dx fast-tick); DC mapping ('u'/'j'/'i'/'k'/'D') still applies, gdeq ('g') "
                          "currently %s (%s coefficients), ampeq shelf 1 ('a') currently %s, ampeq shelf 2 "
                          "('A') currently %s - toggle 'g'/'a'/'A'/'G' + re-run 'w' to A/B compensated vs "
                          "raw TF (phase channel for 'g'/'G', amplitude channel for 'a'/'A')\r\n",
                          CHIRP_F0_HZ, CHIRP_F1_HZ, chirp_law, chirp_total_sweep_sec, chirp_shape,
                          CHIRP_MUTE_SEC * 1000.0f,
                          CHIRP_REF_GPIO, ENVELOPE_INTERP_FACTOR,
                          envelope_gdeq_get_enabled() ? "ON" : "OFF",
                          envelope_gdeq_variant_name(envelope_gdeq_get_variant()),
                          envelope_ampeq_get_enabled() ? "ON" : "OFF",
                          envelope_ampeq_shelf2_get_enabled() ? "ON" : "OFF");
        } else if (c == 'T') {
            // Steps the two-tone pair through TWOTONE_BAND_PRESETS
            // (test_signals.cpp) - lets you sweep the pair across the
            // audio band without a recompile, to empirically map how much
            // relative-delay retuning each band needs (see the
            // group-delay-equalizer refit discussion). Switches into
            // two-tone mode too if not already there, so 'T' alone is
            // enough to start a sweep.
            bool was_twotone = (src == AUDIO_SRC_TWOTONE);
            const char *band_name = test_signals_next_twotone_band();
            if (!was_twotone) {
                dsp_state_set_audio_source(AUDIO_SRC_TWOTONE);
            }
            serial_reply("-> two-tone band: %s (f1=%.0fHz f2=%.0fHz)%s - "
                          "re-tune relative delay ('['/']') for this band before capturing\r\n",
                          band_name, test_signals_get_twotone_f1_hz(), test_signals_get_twotone_f2_hz(),
                          was_twotone ? "" : ", two-tone mode enabled");
        } else if (c == 'R') {
            // Steps the two-tone pair's amplitude ratio
            // (TONE_RATIO_PRESETS, test_signals.cpp, +/-3dB in 1dB steps
            // as of 2026-09-04) - added to test directly whether AMPLITUDE
            // MISMATCH between the two tones, on its own, reproduces
            // 'eq's IMD benefit with 'eq' OFF (no HPF, no presence peak) -
            // see test_signals.h's doc comment and
            // ssb_mic_test_commands.md's dated entry this follows up on.
            // Independent of 'e'/'eq'/'a'/'A'/'g' - combine or compare
            // freely. Switches into two-tone mode too if not already
            // there, same convention as 'T'.
            bool was_twotone_r = (src == AUDIO_SRC_TWOTONE);
            const char *ratio_name = test_signals_next_tone_ratio();
            if (!was_twotone_r) {
                dsp_state_set_audio_source(AUDIO_SRC_TWOTONE);
            }
            serial_reply("-> two-tone amplitude ratio: tone2 %s relative to tone1 "
                          "(linear ratio %.4f)%s\r\n",
                          ratio_name, test_signals_get_tone2_gain(),
                          was_twotone_r ? "" : ", two-tone mode enabled");
        } else if (c == 'Q') {
            // Two-tone null-uncertainty dither - see test_signals.h's doc
            // comment for the full rationale (null_bias_investigation.md's
            // "Root mechanism identified" section: this test's own exact
            // phase-accumulator tone generation makes every null recur at
            // an identical sample-grid alignment, so the tiny per-null
            // dphi-resolution bias accumulates coherently in null_bias/
            // null_bias2 instead of averaging out the way real voice's
            // randomly-timed nulls do). Toggles a small, slow, continuous
            // frequency offset on tone2 only, meant to break that exact
            // recurrence. Different problem, and a different fix, from
            // the "theoretically infinite phase bandwidth at the origin"
            // question in group_delay_fit_notes.md - see test_signals.h.
            // UNTESTED ON REAL HARDWARE - see that same doc comment for
            // the validation plan (long-dwell 'r'-reset comparison of
            // null_bias2, on vs. off) before trusting results either way.
            // Switches into two-tone mode too if not already there, same
            // convention as 'T'/'R' (this toggle has no effect on any
            // other source).
            bool was_twotone_q = (src == AUDIO_SRC_TWOTONE);
            bool dither_now_on = !test_signals_get_twotone_dither_enabled();
            test_signals_set_twotone_dither_enabled(dither_now_on);
            if (!was_twotone_q) {
                dsp_state_set_audio_source(AUDIO_SRC_TWOTONE);
            }
            serial_reply("-> two-tone null-uncertainty dither %s (+/-%.2fHz on tone2, new target every "
                          "%.0fms)%s - UNTESTED, compare null_bias2 across several 'r' resets with a "
                          "long dwell (see null_bias_investigation.md)\r\n",
                          dither_now_on ? "ON" : "off", (double)TWOTONE_DITHER_MAX_HZ,
                          (double)(1000.0f / TWOTONE_DITHER_UPDATE_HZ),
                          was_twotone_q ? "" : ", two-tone mode enabled");
        } else if (c == 'O') {
            // Two-tone phase generator selector - see test_signals.h's doc
            // comment for the full rationale. Added 2026-09-17 as a plain
            // EXACT/LEGACY toggle so the user could revert to the
            // pre-NCO-fix "pure but jumping" tone behavior on demand for a
            // direct A/B; extended 2026-09-18 to a 3-way cycle
            // (EXACT -> LEGACY -> MOD160 -> EXACT...) adding MOD160, the
            // true-160-sample-period recompute that simulation shows
            // eliminates every simulated >8500/9000Hz single-tick jump
            // (null_bias_investigation.md's 2026-09-18 "later" entry).
            // EXACT (today's normal behavior) stays the default; has no
            // effect on tone2 while 'Q' dither is on, since dither's own
            // tone2 path already used the accumulator both before and
            // after the NCO fix. Same switches-into-two-tone-mode
            // convention as 'T'/'R'/'Q'.
            bool was_twotone_o = (src == AUDIO_SRC_TWOTONE);
            const char *gen_name = test_signals_next_twotone_phase_gen();
            if (!was_twotone_o) {
                dsp_state_set_audio_source(AUDIO_SRC_TWOTONE);
            }
            serial_reply("-> two-tone phase generator: %s%s%s\r\n",
                          gen_name,
                          test_signals_get_twotone_dither_enabled() ? " ('Q' dither is ON, so tone2 is "
                                          "unaffected by this selector either way)" : "",
                          was_twotone_o ? "" : ", two-tone mode enabled");
#if AD9851_ATTACHED
        } else if (c == ']') {
            relative_delay_increase();
            float d = relative_delay_get_samples();
            serial_reply("-> relative delay %+.2f samples (~%+.0fus) - %s\r\n",
                          d, d * 1000000.0f / SAMPLE_RATE_HZ,
                          d > 0.0f ? "phase held back" :
                          d < 0.0f ? "envelope held back" : "aligned");
        } else if (c == '[') {
            relative_delay_decrease();
            float d = relative_delay_get_samples();
            serial_reply("-> relative delay %+.2f samples (~%+.0fus) - %s\r\n",
                          d, d * 1000000.0f / SAMPLE_RATE_HZ,
                          d > 0.0f ? "phase held back" :
                          d < 0.0f ? "envelope held back" : "aligned");
        } else if (c == '\'') {
            // Added 2026-09-06 - fine-step counterpart to ']', same
            // coarse/fine pairing convention as '+'/'.' for master gain.
            // See relative_delay.h's DELAY_FINE_STEP_SAMPLES comment for
            // why this was needed: the candidate-B bench test found both
            // the default and candidate-B gdeq configs need their delay
            // chosen from within a sub-0.1-sample window where different
            // IMD orders trade off, finer than the plain ']' step can
            // resolve.
            relative_delay_increase_fine();
            float d = relative_delay_get_samples();
            serial_reply("-> relative delay %+.3f samples (~%+.2fus) - %s\r\n",
                          d, d * 1000000.0f / SAMPLE_RATE_HZ,
                          d > 0.0f ? "phase held back" :
                          d < 0.0f ? "envelope held back" : "aligned");
        } else if (c == ';') {
            // Fine-step counterpart to '[' - see the ''' handler above.
            relative_delay_decrease_fine();
            float d = relative_delay_get_samples();
            serial_reply("-> relative delay %+.3f samples (~%+.2fus) - %s\r\n",
                          d, d * 1000000.0f / SAMPLE_RATE_HZ,
                          d > 0.0f ? "phase held back" :
                          d < 0.0f ? "envelope held back" : "aligned");
        } else if (c == 'J') {
            // 2026-09-12: dumps the per-event jump log (diagnostics.h/.cpp) -
            // added in response to a direct engineering doubt that every
            // random TX-frequency jump is explained by the null-crossing
            // mechanism this project's whole null_bias/dither investigation
            // has been chasing. Grouped here with the relative-delay keys
            // rather than alphabetically/near 'Q' - the log's own
            // relative_delay_samples field makes it most useful checked
            // right after a '['/']'/preset change, and this keeps every
            // AD9851-only command in the same guarded block.
            serial_reply("-> jump log: env=blended/min, both time-matched to the SAME "
                          "original sample as the logged freq_dev value (not just the "
                          "current tick). NEAR_NULL = blended envelope was below "
                          "threshold; NEAR_NULL(either) = only the MIN of the two raw "
                          "samples the interpolation blended was. raw_freq_dev near/far "
                          "= the two RAW undelayed freq_dev_hz values blended together - "
                          "raw_delta close to step_hz means the discontinuity is real and "
                          "already in the raw signal, not an interpolation artifact - see "
                          "null_bias_investigation.md's 2026-09-12 entries:\r\n");
            diagnostics_print_jump_log();
        } else if (c == 'K') {
            // 2026-09-12, yet later still: the slow-mean trigger
            // (diagnostics.h/.cpp) - a deliberately SEPARATE tool from 'J'
            // above, built after direct bench confirmation that 'J' can't
            // answer "what changed to the frequency I can actually see" -
            // it fires on every beat-null crossing (hundreds-thousands/sec)
            // and a capture taken right after a real, observed Aux SP
            // shift (1000->962Hz) came back showing the same routine cycle
            // as every "nothing happened" capture. This instead triggers
            // off a much slower fast/slow EMA divergence, calibrated to
            // the user's own +/-5Hz visual-read tolerance, and freezes a
            // before/after trace so a human-reaction-time delay before
            // reading it can't erase the answer - see diagnostics.h's
            // declaration comment for the full design.
            serial_reply("-> slow-freq trigger: fires when fast/slow EMAs of the "
                          "actually-transmitted freq_dev diverge by more than the "
                          "threshold (calibrated to your own +/-5Hz Aux SP visual-read "
                          "tolerance), not on any single-tick step. Prints a live readout "
                          "if still watching, or the full before/after trace if "
                          "triggered, then re-arms for the next one:\r\n");
            diagnostics_print_slow_trace();
        } else if (c == 'H') {
            // 2026-09-15: held-frequency detector (diagnostics.h/.cpp) - a
            // second, independent tool alongside 'K', added after the user
            // pointed out 'K' can't reliably see a jump that STICKS around
            // for a long time (their own example: watching the SDR show
            // "-22Hz, been there a while" while 'K' stayed quiet). 'K'
            // only fires on fast-vs-slow divergence and resyncs slow to
            // fast every time it's read - so if fast and slow ever drift
            // together slowly enough to never re-diverge from EACH OTHER,
            // 'K' goes silent even while genuinely sitting far from where
            // this run started. This instead compares against a permanent,
            // never-resynced anchor snapped once at boot-settle, so a
            // sustained departure from "where this run actually settled"
            // is visible no matter how gradually it got there - and
            // auto-prints on its own (CONFIRMED STUCK / periodic still-
            // stuck heartbeat / RECOVERED) from diagnostics_service(),
            // same auto-dump pattern as 'K'. This key is a manual, on-
            // demand, non-destructive status+trace read - see
            // diagnostics_print_held_status()'s own declaration comment
            // (diagnostics.h) for the full design.
            serial_reply("-> held-freq detector: fires when the fast EMA has been more "
                          "than a fixed threshold away from this run's PERMANENT boot-"
                          "settle anchor (unlike 'K's slow EMA, this anchor is never "
                          "resynced) for a sustained duration - built to catch a jump "
                          "that STICKS, which 'K' can miss if fast and slow drift "
                          "together slowly enough to never re-diverge from each other. "
                          "Auto-prints CONFIRMED STUCK / still-stuck heartbeats / "
                          "RECOVERED on its own; this key is just an on-demand status + "
                          "rolling tx_freq trace (2 samples/sec, last 60s) read:\r\n");
            diagnostics_print_held_status();
        } else if (c == 'F') {
            // 2026-09-17: full-rate freq/env capture - a third, independent
            // tool alongside 'J'/'K'/'H' above, added after the same-turn
            // 'J' capture the user ran came back as a single point-in-time
            // snapshot (one AUTO-CAPTURED slow_trace event plus one manual
            // 'J' dump, both isolated instants) that couldn't test whether
            // the ~0.485s/~1.000s warble's underlying near-null events
            // actually recur at a specific interval - answering that needs
            // many precisely-timestamped samples in a row, not one snapshot.
            // This instead captures raw_freq_dev_current/raw_envelope_current
            // on EVERY dsp_task tick (~16kHz) for about two seconds, then dumps
            // the whole thing as CSV - direct per-tick ground truth instead
            // of an SDR-audio proxy. See diagnostics_freqenv_capture_arm()'s
            // own declaration comment (diagnostics.h) for the full design,
            // including why the buffer is lazily malloc'd/freed rather than
            // a permanent static array (this board is assumed to have no
            // PSRAM). 2026-09-17, later same day: bumped from ~1.0s/~125KB to
            // ~2.0s/~250KB after the user reported this board's actual build
            // output ("leaving 299220 bytes for local variables, maximum
            // 327680") - see FREQENV_CAPTURE_LEN's own comment (diagnostics.cpp)
            // for the full reasoning on why 2.0s and not more.
            serial_reply("-> freqenv capture: arming a full-rate (every dsp_task tick, ~16kHz) "
                          "capture of raw_freq_dev_current/raw_envelope_current for ~2.0s. Dumps "
                          "automatically as CSV (idx,raw_freq_dev_hz,raw_envelope) once full - "
                          "watch for the ARMED confirmation, then wait for the dump:\r\n");
            diagnostics_freqenv_capture_arm();
#endif
        } else if (c == 'u') {
            envelope_output_raise_pwm_offset();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            serial_reply("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'j') {
            envelope_output_lower_pwm_offset();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            serial_reply("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'i') {
            envelope_output_widen_pwm_scale();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            serial_reply("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'k') {
            envelope_output_narrow_pwm_scale();
            float off = envelope_output_get_pwm_offset(), scale = envelope_output_get_pwm_scale();
            serial_reply("-> PWM duty range %.0f%%-%.0f%% (offset=%.2f, scale=%.2f)\r\n",
                          off * 100.0f, (off + scale > 1.0f ? 1.0f : off + scale) * 100.0f, off, scale);
        } else if (c == 'g') {
            bool now_on = !envelope_gdeq_get_enabled();
            envelope_gdeq_set_enabled(now_on);   // internally resets state on an off->on transition
            serial_reply("-> envelope group-delay equalizer %s%s\r\n", now_on ? "ON" : "off",
                          now_on ? " - re-tune relative delay ('['/']') from scratch, "
                                   "theoretical starting point ~+2.65 samples (see envelope_gdeq.h)" : "");
        } else if (c == 'a') {
            // Mirrors the 'g' handler above exactly - see envelope_ampeq.h.
            // Shelf 1 (2500Hz/+6dB) ONLY - shelf 2 has its own independent
            // 'A' toggle below (split 2026-09-04 after real-hardware
            // testing showed shelf1-only vs shelf1+shelf2 needed to be
            // A/B-able without a reflash). VALIDATED on real hardware
            // 2026-09-03: magnitude tracked prediction closely; real IMD
            // benefit confirmed; does reintroduce measured group-delay
            // dispersion because gdeq hasn't been refit against it (see
            // envelope_ampeq.h). Off by default - currently this
            // project's recommended config (shelf 1 on, shelf 2 off).
            bool now_on = !envelope_ampeq_get_enabled();
            envelope_ampeq_set_enabled(now_on);   // internally resets shelf 1's state on an off->on transition
            serial_reply("-> envelope magnitude (insertion-loss) equalizer, shelf 1 (2500Hz/+6dB) %s%s\r\n", now_on ? "ON" : "off",
                          now_on ? " - validated on real hardware 2026-09-03 (magnitude tracks "
                                   "prediction, real IMD benefit, reintroduces some group-delay "
                                   "dispersion - see envelope_ampeq.h). Shelf 2 ('A') is separate." : "");
        } else if (c == 'A') {
            // Shelf 2 (6000Hz/+10dB), independent of 'a' above - see
            // envelope_ampeq.h's 2026-09-04 entries. VALIDATED on real
            // hardware 2026-09-04 (`ga_Trial2_TF.txt`): real magnitude
            // gain at 8000Hz (+6.7dB, short of the +10dB predicted), but
            // group-delay dispersion roughly DOUBLED again on top of
            // shelf 1's own increase, and real two-tone IMD came back
            // marginally WORSE with both stages on than shelf 1 alone -
            // in this still-unrefit-gdeq state. Off by default; current
            // recommendation is to leave this off until gdeq is refit
            // against the combined analog+shelf1+shelf2 phase response.
            bool now_on = !envelope_ampeq_shelf2_get_enabled();
            envelope_ampeq_shelf2_set_enabled(now_on);   // internally resets shelf 2's state on an off->on transition
            serial_reply("-> envelope magnitude (insertion-loss) equalizer, shelf 2 (6000Hz/+10dB) %s%s\r\n", now_on ? "ON" : "off",
                          now_on ? " - real hardware 2026-09-04: more magnitude recovery toward "
                                   "8000Hz, but dispersion roughly doubles again and two-tone IMD "
                                   "came back marginally WORSE combined with shelf 1 in this "
                                   "unrefit-gdeq state (see envelope_ampeq.h / group_delay_fit_notes.md) "
                                   "- re-run 'w' chirp/TFA to check the current combined response" : "");
        } else if (c == 'l') {
            // ALC (Automatic Level Control) - see envelope_alc.h. Added
            // 2026-09-21 alongside 'S' (Soft-Limit) below, per the user's
            // request for a graceful alternative to the final hard clamp,
            // independently toggleable from Soft-Limit. Runs after
            // gdeq/ampeq, before predistort/the DC mapping - NOT bench-
            // validated yet, first-cut defaults (envelope_alc.h).
            bool now_on = !envelope_alc_get_enabled();
            envelope_alc_set_enabled(now_on);   // internally resets gain to unity on an off->on transition
            serial_reply("-> envelope ALC (leveler) %s", now_on ? "ON" : "off");
            if (now_on) {
                serial_reply(" - target=%.2f attack=%.1fms release=%.1fms (envelope_alc.h) - "
                             "NOT bench-validated yet, first-cut defaults. Soft-Limit ('S') is separate.\r\n",
                             (double)ENV_ALC_TARGET_LEVEL, (double)ENV_ALC_ATTACK_MS, (double)ENV_ALC_RELEASE_MS);
            } else {
                serial_reply("\r\n");
            }
        } else if (c == 'S') {
            // Soft-Limit (soft-knee saturation) - see envelope_softlimit.h.
            // Independent of 'l' above, same reasoning. Runs immediately
            // after 'l' in the pipeline, still before predistort - NOT
            // bench-validated yet, first-cut defaults. Lower rail fixed at
            // envelope=0.0 (the actual rail, not a tunable knee) as of the
            // 2026-09-21 bugfix - see envelope_softlimit.h for why the
            // original ENV_SOFTLIMIT_KNEE_LO=0.15 design injected a DC
            // floor at low signal levels.
            bool now_on = !envelope_softlimit_get_enabled();
            envelope_softlimit_set_enabled(now_on);
            serial_reply("-> envelope Soft-Limit (soft-knee saturation) %s", now_on ? "ON" : "off");
            if (now_on) {
                serial_reply(" - upper knee=%.2f, lower rail=0.0 (span=%.2f) (envelope_softlimit.h) - "
                             "NOT bench-validated yet, first-cut defaults. ALC ('l') is separate; final "
                             "safety clamp becomes a no-op while this is on.\r\n",
                             (double)ENV_SOFTLIMIT_KNEE_HI, (double)ENV_SOFTLIMIT_LOWER_SPAN);
            } else {
                serial_reply("\r\n");
            }
        } else if (c == 'G') {
            // Runtime CYCLE between the fitted gdeq coefficient sets -
            // added 2026-09-05 as a plain two-way toggle after the a+A
            // candidate's real-hardware result turned out to be a
            // genuinely mixed one (better close-in two-tone IMD, worse
            // far-out intermodulation forest - see envelope_gdeq.h's
            // 2026-09-05 header entry and group_delay_fit_notes.md for the
            // full derivation); extended 2026-09-06 to a 3-way cycle (same
            // modulo-cycle convention 'C'/'f' already use) when the
            // mid-band local-slope-vs-p-p Pareto refit proposed a third
            // set, "candidate B" - see envelope_gdeq.h's matching header
            // entry. Skips over any variant this build never fitted
            // (envelope_gdeq_variant_available()) rather than landing on a
            // coefficient pair that was never fit for the active
            // filter/Fs - generalizes the old ENV_GDEQ_HAS_AA_CANDIDATE-
            // only compile-time guard this replaces, so no #if is needed
            // here any more.
            env_gdeq_variant_t cur_variant = envelope_gdeq_get_variant();
            env_gdeq_variant_t next_variant = cur_variant;
            for (int tries = 0; tries < ENV_GDEQ_VARIANT_COUNT; tries++) {
                next_variant = (env_gdeq_variant_t)((next_variant + 1) % ENV_GDEQ_VARIANT_COUNT);
                if (envelope_gdeq_variant_available(next_variant)) {
                    break;
                }
            }
            if (next_variant == cur_variant) {
                // Only ENV_GDEQ_VARIANT_DEFAULT is fitted on this build -
                // the loop above wrapped all the way back around without
                // finding anything else available.
                serial_reply("-> 'G' has no effect on this build - no gdeq candidate sets are fitted "
                              "for the active filter/Fs beyond the default pair (see envelope_gdeq.h)\r\n");
            } else {
                envelope_gdeq_set_variant(next_variant);   // re-inits both sections' coefficients + state
                const char *caveat;
                if (next_variant == ENV_GDEQ_VARIANT_AA_CANDIDATE) {
                    caveat = " - 2026-09-05: better close-in two-tone IMD (3rd-11th order), but a much "
                             "larger far-out intermodulation forest 4-16kHz from carrier, +11 to +19dB "
                             "vs. default - a genuinely mixed result, not currently recommended for "
                             "routine use (see envelope_gdeq.h). Re-tune '['/']' - bench optimum 2.83 "
                             "samples here vs. 2 samples for the default pair (g+a+A config).";
                } else if (next_variant == ENV_GDEQ_VARIANT_CANDIDATE_B) {
                    // Shortened + updated 2026-09-06 (same day, later still) -
                    // the original wording here (still "MODEL PREDICTION
                    // ONLY, NOT YET BENCH-VALIDATED... no bench optimum found
                    // yet") went stale within hours once the real
                    // HiRes_aAG4_TF.txt bench test came back, AND at 537
                    // bytes it combined with the ~46-byte variant name to
                    // overflow serial_reply()'s 512-byte buffer (607 bytes
                    // needed) - the "[serial_reply] WARNING: reply
                    // truncated" the user saw on real hardware. Replaced
                    // with the actual bench result instead, kept well under
                    // budget (see group_delay_fit_notes.md's matching entry
                    // for the full derivation - cross-checks, correlation/
                    // RMS numbers, etc).
                    caveat = " - 2026-09-06 bench test (HiRes_aAG4_TF.txt): group delay CONFIRMED on "
                             "real hardware (beats its own prediction in the 2800-4500Hz band), but "
                             "two-tone IMD gain is small and delay-tuning-dependent - both this and "
                             "the default pair need '['/']'/';'/apostrophe tuned within a 2.00-2.10 "
                             "sample compromise window (IMD orders trade off inside it). See "
                             "group_delay_fit_notes.md.";
                } else if (next_variant == ENV_GDEQ_VARIANT_A_CANDIDATE) {
                    // 2026-09-20: shelf1-only fit (new PSU, 'A' left off) -
                    // see envelope_gdeq.h's matching header entry for the
                    // full derivation. NOT YET bench-tested with gdeq
                    // actually switched on - keep this caveat honest until
                    // that happens, same discipline as candidate B's own
                    // introduction above.
                    caveat = " - 2026-09-20 fit (HiRes_aD_TF_New_5V_psu.txt, shelf1-only, new PSU): "
                             "matches bare full-band p-p exactly (no dispersion cost) while roughly "
                             "halving the local 2800-4500Hz slope. MODEL PREDICTION ONLY - not yet "
                             "bench-tested with gdeq on, no IMD check yet. Re-tune '['/']' from "
                             "scratch. See envelope_gdeq.h / group_delay_fit_notes.md.";
                } else {
                    caveat = " - re-tune '['/']' back toward the default's own bench optimum "
                             "(2 samples, g+a+A config).";
                }
                serial_reply("-> gdeq coefficients: %s%s\r\n", envelope_gdeq_variant_name(next_variant), caveat);
            }
        } else if (c == 'D') {
            bool now_on = !envelope_predistort_get_enabled();
            envelope_predistort_set_enabled(now_on);
            serial_reply("-> envelope pre-distortion %s%s\r\n", now_on ? "ON" : "off",
                          now_on ? " - REPLACES the 'u'/'j'/'i'/'k' linear offset/scale mapping "
                                   "while on (see envelope_predistort.h); those knobs have no "
                                   "effect until this is toggled off again" : "");
        } else if (c == 'x') {
            envelope_floor_raise();
            // 2026-09-25: %.2f -> %.4f alongside ENV_FLOOR_STEP's 0.02->0.001
            // drop - at 2 decimal places every step below 0.01 prints as
            // "0.00", indistinguishable from off.
            serial_reply("-> envelope-null floor raised to %.4f (see envelope_floor.h)\r\n",
                          envelope_floor_get());
        } else if (c == 'z') {
            envelope_floor_lower();
            serial_reply("-> envelope-null floor lowered to %.4f (see envelope_floor.h)\r\n",
                          envelope_floor_get());
        } else if (c == '}') {
            ssb_dsp_raise_freq_dev_slew_limit(dsp_state_get_ssb());
            float limit = ssb_dsp_get_freq_dev_slew_limit_hz(dsp_state_get_ssb());
            if (limit >= SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ) {
                serial_reply("-> freq_dev slew-rate limit: off (see ssb_dsp.h)\r\n");
            } else {
                serial_reply("-> freq_dev slew-rate limit loosened to %.0fHz/sample (see ssb_dsp.h)\r\n", limit);
            }
        } else if (c == '{') {
            ssb_dsp_lower_freq_dev_slew_limit(dsp_state_get_ssb());
            float limit = ssb_dsp_get_freq_dev_slew_limit_hz(dsp_state_get_ssb());
            serial_reply("-> freq_dev slew-rate limit tightened to %.0fHz/sample (see ssb_dsp.h)\r\n", limit);
        } else if (c == 'I') {
            bool now_on = !envelope_interp_get_enabled();
            envelope_interp_set_enabled(now_on);   // internally seeds prev/target on an off->on transition
            serial_reply("-> %dx envelope output interpolation %s (see envelope_interp.h)\r\n",
                          ENVELOPE_INTERP_FACTOR, now_on ? "ON" : "off");
        } else if (c == 'C') {
            // v4.3/v4.4: direct A/B between the three interp curves,
            // independent of 'I' itself - see envelope_interp.h's "v4.3"
            // and "v4.4" header notes. Only affects rendered output while
            // 'I' is ON; harmless (and remembered) to cycle with 'I' off.
            // Cycles CATMULL_ROM(0) -> LINEAR(1) -> HOLD(2) -> CATMULL_ROM,
            // same modulo-cycle convention as 'f' (adc_lpf_mode).
            envelope_interp_curve_t cur = envelope_interp_get_curve();
            envelope_interp_curve_t next = (envelope_interp_curve_t)((cur + 1) % 3);
            envelope_interp_set_curve(next);
            static const char *k_curve_desc[3] = {
                "Catmull-Rom (v4.2 cubic Hermite, default)",
                "LINEAR (v4 straight-line ramp)",
                "HOLD (v4.4 plain 64kHz ZOH, no ramp)"
            };
            serial_reply("-> envelope interp curve: %s%s (see envelope_interp.h v4.3/v4.4 notes)\r\n",
                          k_curve_desc[next],
                          envelope_interp_get_enabled() ? "" : " - no effect until 'I' is ON");
        } else if (c == 'f') {
            // Cycles off -> Butterworth(4th) -> Chebyshev(4th) ->
            // Butterworth6 -> Chebyshev6 -> Butterworth8 -> Chebyshev8 ->
            // off. See adc_capture.h's adc_lpf_mode_t / ADC_LPF_CUTOFF_HZ /
            // ADC_LPF_CHEBYSHEV_RIPPLE_DB for what each mode actually does.
            // 2026-09-23: widened from %3 to %7 when the 6th/8th-order
            // modes were added - CPU headroom was measured tight that same
            // day (see moving_forward_notes.md's 2026-09-23 entry), so this
            // is the live A/B tool for checking each order's actual
            // [timing] cost on real hardware before settling on one.
            adc_lpf_mode_t mode = adc_capture_get_lpf_mode();
            mode = (adc_lpf_mode_t)((mode + 1) % 7);
            adc_capture_set_lpf_mode(mode);
            serial_reply("-> ADC LPF: %s\r\n", adc_capture_lpf_mode_name(mode));
        } else if (c == 'n') {
            // Cycles the null-bias diagnostic's envelope threshold - see
            // ssb_dsp_set_null_bias_threshold() in ssb_dsp.h and the
            // '[dsp] null_bias' line in the periodic diagnostics block.
            // Tune this if near_null_samples% comes back 0 (threshold too
            // tight for this signal's actual peak envelope) or looks like
            // it's catching ordinary low-envelope content, not just
            // genuine two-tone nulls. Default (0.05f) is index 1 here, so
            // this starts by loosening it on the first press.
            static const float k_null_thresholds[] = {0.02f, 0.05f, 0.10f, 0.20f};
            static int s_null_threshold_idx = 1;
            s_null_threshold_idx = (s_null_threshold_idx + 1) % 4;
            float thr = k_null_thresholds[s_null_threshold_idx];
            ssb_dsp_set_null_bias_threshold(dsp_state_get_ssb(), thr);
            serial_reply("-> null_bias threshold=%.2f (see '[dsp] null_bias' diagnostic line)\r\n", thr);
        } else if (c == 'v') {
            diagnostics_toggle_muted();
        } else if (c == 'M') {
            // Separate mute for the three auto-dump watchers (canary,
            // slow_trace's automatic side, held_freq's automatic side) -
            // added 2026-09-18 on request, distinct from 'v' on purpose.
            // See diagnostics_toggle_autodump_muted()'s doc comment
            // (diagnostics.h) for why these were deliberately mute-exempt
            // from 'v' up to now, and the caveat that detection itself
            // pauses too while this is on, not just the print.
            diagnostics_toggle_autodump_muted();
        } else if (c == 'V') {
            // On-demand snapshot - see diagnostics_print_now()'s own
            // comment for why this exists (the periodic 1Hz block is
            // awkward to catch on purpose, and prints nothing at all
            // while muted by design). Deliberately does NOT require
            // unmuting first.
            diagnostics_print_now();
        } else if (c == 'r') {
            // Resets every diagnostic counter/watermark for a clean
            // measurement window, without needing a full reflash. Useful
            // after switching modes (e.g. 't' then 'm') so counters like
            // drop_total/starve_ticks_total reflect only what happens
            // AFTER the reset, not history carried over from a different
            // mode or an earlier test run in the same boot. Touches every
            // module that keeps its own diagnostic counters, same as the
            // original inline handler touched every counter directly.
            diagnostics_reset();
            adc_capture_reset_diag();
            ssb_dsp_reset_freq_dev_stats(dsp_state_get_ssb());
            serial_reply("-> diagnostics reset, clean window starting now\r\n");
        } else if (c == 'Z') {
            // Clean, deliberate reboot back to a known state - added
            // 2026-09-12, motivated directly by the 'K' slow-trace
            // investigation (see null_bias_investigation.md/
            // moving_forward_notes.md's 2026-09-12 entries): getting back
            // to a fresh, fully-known starting point (delay=0.00 at boot,
            // both freq_dev EMAs re-seeded from scratch via the
            // boot-settle path, every 'J'/'K' ring and diagnostic counter
            // cleared) previously meant physically power-cycling the
            // board or unplugging/replugging USB - this does the same
            // thing from the bench seat, one keystroke, no need to reach
            // the hardware at all.
            //
            // Single keystroke, no confirmation prompt - matches every
            // other one-shot command in this file ('r' diagnostics reset,
            // 'V' on-demand snapshot, 'L' task list): nothing here is a
            // destructive, hard-to-undo action the way e.g. a filesystem
            // erase would be, a stray press just costs a few seconds of
            // the board reconnecting and re-enumerating over USB.
            //
            // Serial.flush() + a short delay before restarting: without
            // this, ESP.restart() can tear down the USB CDC peripheral
            // before the "-> rebooting..." reply above has actually left
            // the TX buffer, so the confirmation can silently never reach
            // the terminal - same underlying USB CDC timing class as the
            // 64-byte packet issue documented at the top of this file for
            // serial_reply() (this is the opposite end of a transfer:
            // making sure output finishes going out, not making sure the
            // host can tell where it ends). Not IRAM_ATTR-relevant - only
            // ever reached from this on-demand serial command context,
            // never the dsp_task hot path.
            serial_reply("-> rebooting now...\r\n");
            Serial.flush();
            delay(100);
            ESP.restart();
        } else if (c == 'e') {
            bool now_on = !ssb_dsp_get_eq_enabled(dsp_state_get_ssb());
            ssb_dsp_set_eq_enabled(dsp_state_get_ssb(), now_on);
            serial_reply("-> EQ (HPF+presence) %s\r\n", now_on ? "ON" : "off");
        } else if (c == 'c') {
            bool now_on = !ssb_dsp_get_compressor_enabled(dsp_state_get_ssb());
            ssb_dsp_set_compressor_enabled(dsp_state_get_ssb(), now_on);
            serial_reply("-> compressor %s\r\n", now_on ? "ON" : "off");
        } else if (c == 'U') {
            // 2026-09-25: Mic Gain up - see ssb_dsp_set_mic_gain_db()'s doc
            // comment in ssb_dsp.h. Set this FIRST (by ear/scope, or
            // watching envelope stats via 'V') so ordinary speech peaks
            // land near the level ssb_dsp_set_compressor_level()'s
            // calibration table assumes (~0.85, see that function's doc
            // comment) - THEN dial in compression level with 'X'/'q'.
            float new_gain = ssb_dsp_get_mic_gain_db(dsp_state_get_ssb()) + MIC_GAIN_STEP_DB;
            ssb_dsp_set_mic_gain_db(dsp_state_get_ssb(), new_gain);
            serial_reply("-> mic gain %+.1f dB\r\n", new_gain);
        } else if (c == 'Y') {
            float new_gain = ssb_dsp_get_mic_gain_db(dsp_state_get_ssb()) - MIC_GAIN_STEP_DB;
            ssb_dsp_set_mic_gain_db(dsp_state_get_ssb(), new_gain);
            serial_reply("-> mic gain %+.1f dB\r\n", new_gain);
        } else if (c == 'X') {
            // 2026-09-25: Compression Level up - replaces the old 'W'
            // mode-cycling command entirely (freed up, not reassigned) -
            // see ssb_dsp_set_compressor_level()'s doc comment in
            // ssb_dsp.h for the full design (a small integer-dB table
            // calibrated against the user's own real voice recording,
            // 0=plain limiter through 6=~+6dB measured RMS gain).
            int level = ssb_dsp_get_compressor_level(dsp_state_get_ssb()) + 1;
            ssb_dsp_set_compressor_level(dsp_state_get_ssb(), level);
            level = ssb_dsp_get_compressor_level(dsp_state_get_ssb());   // re-read: setter clamps
            serial_reply("-> compression level %d%s%s\r\n", level,
                          level == SSB_DSP_COMP_LEVEL_MIN ? " (limiter only, no boost)" : "",
                          ssb_dsp_get_compressor_enabled(dsp_state_get_ssb()) ? "" : " - no effect until 'c' is ON");
        } else if (c == 'q') {
            int level = ssb_dsp_get_compressor_level(dsp_state_get_ssb()) - 1;
            ssb_dsp_set_compressor_level(dsp_state_get_ssb(), level);
            level = ssb_dsp_get_compressor_level(dsp_state_get_ssb());
            serial_reply("-> compression level %d%s%s\r\n", level,
                          level == SSB_DSP_COMP_LEVEL_MIN ? " (limiter only, no boost)" : "",
                          ssb_dsp_get_compressor_enabled(dsp_state_get_ssb()) ? "" : " - no effect until 'c' is ON");
        } else if (c == 'W') {
            // 2026-09-25: Mic Squelch on/off - reuses 'W', free since the
            // old switchable-compressor-mode cycling command it used to
            // mean was removed entirely (see 'X's own comment above). See
            // ssb_dsp_set_squelch_enabled()'s doc comment in ssb_dsp.h -
            // this is a genuine "silence it" gate, NOT envelope_floor.h's
            // 'x'/'z' (that one structurally can't reach zero, see its own
            // 2026-09-25 doc update). Threshold via '('/')'.
            bool now_on = !ssb_dsp_get_squelch_enabled(dsp_state_get_ssb());
            ssb_dsp_set_squelch_enabled(dsp_state_get_ssb(), now_on);
            serial_reply("-> mic squelch %s (threshold %.4f, see '('/')')\r\n", now_on ? "ON" : "off",
                          ssb_dsp_get_squelch_threshold(dsp_state_get_ssb()));
        } else if (c == '(') {
            float new_thr = ssb_dsp_get_squelch_threshold(dsp_state_get_ssb()) - SQUELCH_THRESHOLD_STEP;
            ssb_dsp_set_squelch_threshold(dsp_state_get_ssb(), new_thr);
            new_thr = ssb_dsp_get_squelch_threshold(dsp_state_get_ssb());   // re-read: setter clamps
            serial_reply("-> mic squelch threshold %.4f%s\r\n", new_thr,
                          ssb_dsp_get_squelch_enabled(dsp_state_get_ssb()) ? "" : " - no effect until 'W' is ON");
        } else if (c == ')') {
            float new_thr = ssb_dsp_get_squelch_threshold(dsp_state_get_ssb()) + SQUELCH_THRESHOLD_STEP;
            ssb_dsp_set_squelch_threshold(dsp_state_get_ssb(), new_thr);
            new_thr = ssb_dsp_get_squelch_threshold(dsp_state_get_ssb());
            serial_reply("-> mic squelch threshold %.4f%s\r\n", new_thr,
                          ssb_dsp_get_squelch_enabled(dsp_state_get_ssb()) ? "" : " - no effect until 'W' is ON");
        } else if (c == '+') {
            float new_gain = ssb_dsp_get_master_gain_db(dsp_state_get_ssb()) + MASTER_GAIN_STEP_DB;
            dsp_state_set_master_gain_db(new_gain);
            serial_reply("-> master gain %+.1f dB\r\n", new_gain);
        } else if (c == '-') {
            float new_gain = ssb_dsp_get_master_gain_db(dsp_state_get_ssb()) - MASTER_GAIN_STEP_DB;
            dsp_state_set_master_gain_db(new_gain);
            serial_reply("-> master gain %+.1f dB\r\n", new_gain);
        } else if (c == '.') {
            float new_gain = ssb_dsp_get_master_gain_db(dsp_state_get_ssb()) + MASTER_GAIN_FINE_STEP_DB;
            dsp_state_set_master_gain_db(new_gain);
            serial_reply("-> master gain %+.2f dB\r\n", new_gain);
        } else if (c == ',') {
            float new_gain = ssb_dsp_get_master_gain_db(dsp_state_get_ssb()) - MASTER_GAIN_FINE_STEP_DB;
            dsp_state_set_master_gain_db(new_gain);
            serial_reply("-> master gain %+.2f dB\r\n", new_gain);
        } else if (c == 'd') {
            // Direct duty/density override - see envelope_output.h's
            // header comment. Bypasses master gain/envelope/offset-scale/
            // predistort entirely so the RSET/filter/AD9851 chain can
            // be characterized against a KNOWN, exact commanded output
            // level instead of one inferred from a gate-voltage reading -
            // see envelope_predistort.h's REVISION 3 notes for why this
            // was added. The carrier/phase path is untouched - select a
            // steady source separately (e.g. 's', single-tone, phase
            // rock-steady - the same choice REVISION 1/2's own
            // characterization used).
            //
            // 2026-09-07: this bypasses 'D' (predistort) by design (see
            // above), which means it's the wrong tool for checking whether
            // 'D' actually does anything - see 'E' below for the
            // envelope-mapped alternative that shares this same stepper.
            //
            // 2026-09-08: now drives whichever of PWM/SDM is actually
            // compiled in (envelope_output_write_duty_raw()) - the message
            // below says "duty" or "density" accordingly so this reads
            // correctly whichever comparison path is active.
            bool now_on = !envelope_output_duty_override_get_enabled();
            envelope_output_duty_override_set_enabled(now_on);
            if (now_on) {
                s_duty_override_value = 0;
#if SDM_COMPARISON_ENABLED
                const char *item_label = "density";
                const char *raw_label = "raw density";
#else
                const char *item_label = "duty";
                const char *raw_label = "raw duty";
#endif
                serial_reply("-> %s override ON (%s mode - 'E' to switch), '>'/'<'=+-1, 'N'/'B'=+-16; "
                              "dsp_task's normal envelope pipeline is now locked out of the "
                              "RSET output until 'd' again\r\n",
                              item_label,
                              s_duty_override_use_envelope_mapping ? "envelope-mapped, see 'D'" : raw_label);
                duty_override_write_and_reply();
            } else {
                serial_reply("-> duty override off (dsp_task's normal envelope pipeline back in control)\r\n");
            }
        } else if (c == 'E') {
            // Added 2026-09-07 - see duty_override_write_and_reply()'s own
            // comment for the full rationale. Toggles whether the
            // '>'/'<'/'N'/'B' stepper below writes its index as a literal
            // raw duty count (off, original/default behavior) or as a
            // commanded envelope run through 'D' (predistort) or the
            // linear pwm_offset/pwm_scale mapping (on) - the only
            // difference is what happens to the SAME stepped index at
            // write time, so an existing automated test harness driving
            // '>'/'<'/'N'/'B' needs no changes beyond sending this once.
            // Works whether 'd' override is on or off right now - if it's
            // already on, the current index is immediately re-written
            // under the new interpretation so the output updates without
            // needing another step press; if it's off, this just selects
            // which mode 'd' will start in next.
            s_duty_override_use_envelope_mapping = !s_duty_override_use_envelope_mapping;
            serial_reply("-> duty override stepper now writes its index as %s%s\r\n",
                          s_duty_override_use_envelope_mapping
                              ? "a commanded ENVELOPE (mapped via 'D'/predistort, or the linear "
                                "pwm_offset/scale mapping while 'D' is off)"
                              : "the literal RAW duty count (bypasses 'D' entirely, same as before)",
                          envelope_output_duty_override_get_enabled() ? "" : " - takes effect once 'd' is on");
            if (envelope_output_duty_override_get_enabled()) {
                duty_override_write_and_reply();
            }
        } else if (c == '>' && envelope_output_duty_override_get_enabled()) {
            uint32_t max_duty = envelope_output_get_max_duty();
            if (s_duty_override_value < max_duty) s_duty_override_value++;
            duty_override_write_and_reply();
        } else if (c == '<' && envelope_output_duty_override_get_enabled()) {
            if (s_duty_override_value > 0) s_duty_override_value--;
            duty_override_write_and_reply();
        } else if (c == 'N' && envelope_output_duty_override_get_enabled()) {
            uint32_t max_duty = envelope_output_get_max_duty();
            s_duty_override_value = (s_duty_override_value + 16 > max_duty) ? max_duty : s_duty_override_value + 16;
            duty_override_write_and_reply();
        } else if (c == 'B' && envelope_output_duty_override_get_enabled()) {
            s_duty_override_value = (s_duty_override_value < 16) ? 0 : s_duty_override_value - 16;
            duty_override_write_and_reply();
#if AD9851_ATTACHED
        } else if (c == 'o') {
            bool now_on = !carrier_output_get_rf_enabled();
            carrier_output_set_rf_enabled(now_on);
            serial_reply("-> AD9851 RF output %s\r\n", now_on ? "ON" : "off (powered down)");
#endif
        } else if (c == 'L') {
            // One-shot FreeRTOS task list - added to directly settle the
            // "Events"/"Arduino" IDE core-affinity question (both showing
            // Core 1 in the board menu) instead of continuing to reason
            // from uncertain memory of what Arduino-ESP32's boot glue
            // actually creates. See the [core1] busy breakdown's 96.2%
            // unexplained "other" bucket - this either shows an
            // unaccounted-for task (e.g. an "arduino_events" task) sitting
            // on Core 1, or shows only the tasks we already know about, in
            // which case the "other" bucket points back at the ADC ISR (or
            // this module's own idle-hook threshold) instead.
            //
            // vTaskList() requires configUSE_TRACE_FACILITY=1 AND
            // configUSE_STATS_FORMATTING_FUNCTIONS=1 in this board's
            // FreeRTOSConfig.h - NOT verified from here, since this
            // environment has no ESP-IDF/Arduino-ESP32 checkout to check
            // against (only this project's own source files are available
            // to read). If either is off, this line simply won't compile -
            // same situation as the esp_freertos_idle_cb_t signature
            // mismatch earlier this session, where the real compiler error
            // was more reliable than guessing from memory. If that
            // happens, the fix is to switch to uxTaskGetSystemState()
            // instead (needs only configUSE_TRACE_FACILITY, not the
            // formatting half) and format each TaskStatus_t field by hand.
            //
            // The exact column layout (whether a per-task core/affinity
            // column is present) is also an ESP-IDF FreeRTOS-port
            // extension whose default here isn't verified - but even
            // without it, the task NAME list alone is enough to confirm or
            // rule out an extra Arduino-created task existing at all.
            //
            // Buffer budgeted per FreeRTOS docs (~40 bytes/task) for up to
            // ~20 tasks - this project's known tasks are dsp_task,
            // dac_task, loopTask, IDLE0, IDLE1, ipc0, ipc1, Tmr Svc, plus
            // headroom for whatever Arduino-ESP32's boot glue adds (the
            // very thing this command exists to check for).
            static char task_list_buf[900];
            serial_reply("-> FreeRTOS task list (columns per vTaskList(): name, state "
                          "B/R/D/S/X, priority, stack high-water mark, task number, then "
                          "core/affinity IF this board's FreeRTOSConfig.h includes it):\r\n");
            vTaskList(task_list_buf);
            Serial.write((const uint8_t *)task_list_buf, strlen(task_list_buf));
            serial_reply("-> end task list\r\n");
        } else if (c == 'b') {
            // 2026-09-30: new diagnostic, added after three real bugs in
            // ENVELOPE_ISR_INTERP_ENABLED's fast-tick PWM path (missing
            // duty-override guard, Q4->native duty scaling, carry-forward
            // telescoping) were each found and fixed in turn from the
            // user's real-hardware bench reports, yet output was STILL
            // reported "the same low level" on Preset 1 afterward, with
            // duty-override confirmed off and offset/scale confirmed sane
            // (0.20/0.90). Rather than propose a fourth unverified fix
            // blind - there's no ESP-IDF/Arduino-ESP32 toolchain in this
            // environment to compile or test against, only this project's
            // own source to read - this prints the actual running numbers
            // so the discrepancy (if any) between what dsp_task thinks it
            // staged and what the ISR/hardware actually did with it can be
            // seen directly. See envelope_output_isr_interp_get_debug()
            // (envelope_output.h/.cpp) for what each field means and why.
            //
            // hw_duty_now is the important cross-check: it's read back
            // live via ledc_get_duty(), completely independent of this
            // module's own software state - if it tracks accum_q4>>4
            // closely, the ISR's write is reaching the hardware as
            // expected and the remaining problem is further upstream
            // (envelope magnitude itself, offset/scale application order,
            // etc.); if it does NOT track accum_q4>>4, the raw peripheral
            // register write itself (or the assumed conf0.low_speed_update/
            // conf1.duty_start commit sequence - see that function's own
            // field-name-uncertainty caveat) is the next suspect instead.
#if PWM_COMPARISON_ENABLED && ENVELOPE_ISR_INTERP_ENABLED
            envelope_output_isr_interp_debug_t dbg;
            envelope_output_isr_interp_get_debug(&dbg);
            // Second snapshot 20ms later (blocks loop() for 20ms - it is
            // mostly idle, same tolerance as the 'L' handler) so the ISR
            // call RATE can be printed, not just a cumulative count.
            // Expected at FACTOR=4: ~1280 calls per 20ms.
            delay(20);
            envelope_output_isr_interp_debug_t dbg2;
            envelope_output_isr_interp_get_debug(&dbg2);
            serial_reply("-> ISR liveness: override_on=%lu  calls_total delta over ~20ms=%lu (expect ~1280 at 64kHz)  "
                          "calls_past_guard delta=%lu  accum_q4 before/after=%ld/%ld\r\n",
                          (unsigned long)dbg2.override_on,
                          (unsigned long)(dbg2.calls_total - dbg.calls_total),
                          (unsigned long)(dbg2.calls_past_guard - dbg.calls_past_guard),
                          (long)dbg.accum_q4, (long)dbg2.accum_q4);
            serial_reply("-> LEDC timer ACTUAL freq = %lu Hz (ledc_get_freq) vs write tick %lu Hz -> beat %ld Hz "
                          "(requested %lu Hz)\r\n",
                          (unsigned long)dbg2.ledc_freq_hz,
                          (unsigned long)(ENVELOPE_INTERP_FACTOR * SAMPLE_RATE_HZ),
                          (long)((int32_t)dbg2.ledc_freq_hz - (int32_t)(ENVELOPE_INTERP_FACTOR * SAMPLE_RATE_HZ)),
                          (unsigned long)RSET_MOD_LEDC_FREQ_HZ);
            uint32_t accum_native = (uint32_t)((dbg.accum_q4 + 8) >> 4);
            uint32_t target_native = (uint32_t)((dbg.last_target_q4 + 8) >> 4);
            serial_reply("-> ISR-interp debug: last_envelope=%.4f  last_target_q4=%ld (~%lu native)  "
                          "step_q4=%ld  accum_q4=%ld (~%lu native)  carry_q4=%ld  "
                          "hw_duty_now=%lu (native, ledc_get_duty() readback)  "
                          "pwm_offset=%.4f  pwm_scale=%.4f\r\n",
                          dbg.last_envelope,
                          (long)dbg.last_target_q4, (unsigned long)target_native,
                          (long)dbg.step_q4,
                          (long)dbg.accum_q4, (unsigned long)accum_native,
                          (long)dbg.carry_q4,
                          (unsigned long)dbg.hw_duty_now,
                          envelope_output_get_pwm_offset(), envelope_output_get_pwm_scale());
#else
            serial_reply("-> ISR-interp debug: unavailable - PWM_COMPARISON_ENABLED && "
                          "ENVELOPE_ISR_INTERP_ENABLED is not 1 in this build (config.h/"
                          "envelope_interp.h)\r\n");
#endif
        } else if (c == 'P') {
            // Prints every current lever as a single comma-separated line,
            // in exactly PersistentSettings's field order (name,
            // audio_source, relative_delay_samples, env_pwm_offset,
            // env_pwm_scale, env_gdeq_enable, adc_lpf_mode, eq_enable,
            // compressor_enable, master_gain_db, ad9851_output_enable,
            // env_predistort_enable, env_floor, freq_dev_slew_limit_hz,
            // envelope_interp_enable, envelope_interp_curve,
            // env_ampeq_enable, env_ampeq_shelf2_enable, env_gdeq_variant,
            // env_alc_enable, env_softlimit_enable, mic_gain_db,
            // comp_level_db) -
            // wrapped in braces with a trailing comma so the whole line
            // can be pasted directly into settingsPresets[] in settings.h
            // as a new preset entry.
            // Rename "Live" (and add a numbered comment above it, matching
            // the existing presets' style) after pasting - and remember
            // settings.h's static_assert ties the array size to the
            // '0'-'9' range in this file, so adding an 11th preset needs
            // that range widened too (see the static_assert's own comment
            // in settings.h).
            //
            // env_predistort_enable ('D', envelope_predistort.h) IS
            // included now, folded into the pasteable line - worth knowing
            // before pasting this into a preset: if 'D' is ON right now,
            // the env_pwm_offset/env_pwm_scale values in the line below
            // reflect the mapping that's currently INACTIVE (pre-distortion
            // is overriding them) - they'll only take effect again on
            // whichever of the two (env_predistort_enable=false, or 'D'
            // toggled off live) happens first.
            //
            // envelope_interp_curve ('C', envelope_interp.h v4.3/v4.4) -
            // prints as the enum constant name
            // (ENVELOPE_INTERP_CURVE_CATMULL_ROM/_LINEAR/_HOLD), same
            // convention as adc_lpf_mode below, so the pasted line compiles
            // directly.
            //
            // env_ampeq_enable ('a', envelope_ampeq.h, shelf 1) - a plain
            // bool, same as env_gdeq_enable. VALIDATED on real hardware
            // 2026-09-03 (magnitude tracks prediction, real IMD benefit,
            // reintroduces some group-delay dispersion pending a gdeq
            // refit).
            //
            // env_ampeq_shelf2_enable ('A', envelope_ampeq.h, shelf 2) -
            // added 2026-09-04 when shelf 1/2 were split into independent
            // flags. VALIDATED on real hardware the same day: more
            // magnitude recovery toward 8000Hz, but dispersion roughly
            // doubles again and two-tone IMD came back marginally WORSE
            // combined with shelf 1 in this still-unrefit-gdeq state -
            // pasting "true" here means that preset starts up with shelf 2
            // active; current project recommendation is "false" (shelf 1
            // only) until gdeq is refit.
            //
            // env_gdeq_variant ('G', envelope_gdeq.h) is the newest
            // trailing field - added 2026-09-05 as a bool (env_gdeq_use_
            // aa_candidate) when the a+A candidate gdeq fit became a live
            // runtime toggle, migrated 2026-09-06 to this 3-state enum
            // when "candidate B" made a second alternative set exist.
            // Prints as the enum constant name (ENV_GDEQ_VARIANT_DEFAULT/
            // _AA_CANDIDATE/_CANDIDATE_B), same convention as adc_lpf_mode/
            // envelope_interp_curve above, so the pasted line compiles
            // directly. 2026-09-05 real-hardware result for the a+A
            // candidate: better close-in two-tone IMD, but a much larger
            // far-out intermodulation forest - a mixed result, NOT
            // currently recommended. Candidate B (2026-09-06) is a MODEL
            // PREDICTION ONLY, not yet bench-validated at all. Pasting
            // either non-default constant here is a deliberate
            // bench-testing choice, not this project's default.
            //
            // env_alc_enable ('l', envelope_alc.h) and env_softlimit_enable
            // ('S', envelope_softlimit.h) were the newest trailing fields as
            // of 2026-09-21. Both plain bools, printed the same way as
            // env_ampeq_enable/env_ampeq_shelf2_enable above. NEITHER is
            // bench-validated yet - pasting "true" for either here means
            // that preset starts up with first-cut, unvalidated defaults
            // active; current project recommendation is "false" for both
            // until real-hardware testing says otherwise.
            //
            // comp_mode was here through 2026-09-24 - REMOVED 2026-09-25
            // along with the whole switchable-mode compressor (see
            // ssb_dsp.h's 2026-09-25 comment for why: the user's own real
            // mic recording showed the actual missing piece was a Mic Gain
            // stage upstream, not another compressor mode). The two
            // trailing fields are now mic_gain_db ('U'/'Y',
            // ssb_dsp_set_mic_gain_db() - a plain float dB, printed the
            // same way as master_gain_db) and comp_level_db ('X'/'q',
            // ssb_dsp_set_compressor_level() - a plain int 0-10, see that
            // function's doc comment in ssb_dsp.h for the full calibrated-
            // level design). Neither is bench-validated yet; comp_level_db
            // defaults to 0 (limiter only, no boost) - a deliberate change
            // from the old design's automatic makeup gain.
            //
            // 2026-09-25, later: squelch_enable/squelch_threshold appended
            // as two more trailing fields - ssb_dsp_set_squelch_enabled()/
            // ssb_dsp_set_squelch_threshold() ('W' toggle, '('/')' step).
            // Existing presets don't need editing (same positional-
            // zero-fill convention as every other trailing field) - but
            // note the zero-fill gives squelch_threshold=0.0, NOT
            // ssb_dsp_init()'s own 0.01 runtime default: loading ANY
            // existing preset resets threshold to 0.0 (harmless while
            // squelch is off, same as every other zero-filled field, but
            // worth knowing - '(' /')' after loading a preset starts from
            // 0.0, not the pre-preset-load value). NOT bench-validated.
#if AD9851_ATTACHED
            float rel_delay = relative_delay_get_samples();
            bool rf_enabled = carrier_output_get_rf_enabled();
#else
            // No relative-delay line or AD9851 output to read without
            // AD9851_ATTACHED - placeholders matching every existing
            // preset's own default (0 delay wasn't any preset's default,
            // but it's the only sane placeholder absent a real value;
            // true for ad9851_output_enable matches all 5 current presets).
            float rel_delay = 0.0f;
            bool rf_enabled = true;
#endif
            // adc_lpf_mode prints as the enum constant name (ADC_LPF_MODE_OFF
            // etc.), not a string literal - it's a valid C identifier, so
            // the pasted line compiles directly into settingsPresets[].
            // 2026-09-23: widened to 7 entries for the BUTTERWORTH6/
            // CHEBYSHEV6/BUTTERWORTH8/CHEBYSHEV8 modes - see adc_lpf_mode_t
            // in adc_capture.h.
            static const char *k_adc_lpf_mode_enum_name[7] = {
                "ADC_LPF_MODE_OFF", "ADC_LPF_MODE_BUTTERWORTH", "ADC_LPF_MODE_CHEBYSHEV",
                "ADC_LPF_MODE_BUTTERWORTH6", "ADC_LPF_MODE_CHEBYSHEV6",
                "ADC_LPF_MODE_BUTTERWORTH8", "ADC_LPF_MODE_CHEBYSHEV8"
            };
            // freq_dev_slew_limit_hz prints as the sentinel constant's own
            // name when off, same reasoning as adc_lpf_mode above - so the
            // pasted line compiles AND stays meaningful (a bare huge float
            // literal would compile fine too, but wouldn't self-document
            // as "off" the way the constant name does).
            char slew_str[40];
            float slew_limit = ssb_dsp_get_freq_dev_slew_limit_hz(dsp_state_get_ssb());
            if (slew_limit >= SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ) {
                snprintf(slew_str, sizeof(slew_str), "SSB_DSP_FREQ_DEV_SLEW_UNLIMITED_HZ");
            } else {
                snprintf(slew_str, sizeof(slew_str), "%.0ff", slew_limit);
            }
            static const char *k_interp_curve_enum_name[3] = {
                "ENVELOPE_INTERP_CURVE_CATMULL_ROM", "ENVELOPE_INTERP_CURVE_LINEAR",
                "ENVELOPE_INTERP_CURVE_HOLD"
            };
            // env_gdeq_variant prints as the enum constant name, same
            // convention as adc_lpf_mode/envelope_interp_curve above - see
            // this function's header comment's env_gdeq_variant note.
            static const char *k_gdeq_variant_enum_name[ENV_GDEQ_VARIANT_COUNT] = {
                "ENV_GDEQ_VARIANT_DEFAULT", "ENV_GDEQ_VARIANT_AA_CANDIDATE",
                "ENV_GDEQ_VARIANT_CANDIDATE_B", "ENV_GDEQ_VARIANT_A_CANDIDATE"
            };
            serial_reply("-> settings line (paste into settingsPresets[] in settings.h, then rename \"Live\"):\r\n");
            // env_floor's field (the 12th %f below) is %.4ff, not %.2ff like
            // its neighbors - 2026-09-25: ENV_FLOOR_STEP dropped to 0.001,
            // and %.2ff would silently round anything below 0.01 to "0.00f",
            // losing a deliberately-dialed-in floor value the moment it's
            // pasted into a preset.
            // 2026-09-25: two more trailing fields appended - squelch_enable
            // (%s, true/false) and squelch_threshold (%.4ff, same
            // rounding-loss reasoning as env_floor's field above - the
            // active step size is 0.001, %.2ff would round every
            // meaningful value to 0.00f).
            serial_reply("    { \"Live\", %s, %.2ff, %.2ff, %.2ff, %s, %s, %s, %s, %.1ff, %s, %s, %.4ff, %s, %s, %s, %s, %s, %s, %s, %s, %.1ff, %d, %s, %.4ff },\r\n",
                          audio_source_enum_name(dsp_state_get_audio_source()),
                          rel_delay,
                          envelope_output_get_pwm_offset(),
                          envelope_output_get_pwm_scale(),
                          envelope_gdeq_get_enabled() ? "true" : "false",
                          k_adc_lpf_mode_enum_name[adc_capture_get_lpf_mode()],
                          ssb_dsp_get_eq_enabled(dsp_state_get_ssb()) ? "true" : "false",
                          ssb_dsp_get_compressor_enabled(dsp_state_get_ssb()) ? "true" : "false",
                          ssb_dsp_get_master_gain_db(dsp_state_get_ssb()),
                          rf_enabled ? "true" : "false",
                          envelope_predistort_get_enabled() ? "true" : "false",
                          envelope_floor_get(),
                          slew_str,
                          envelope_interp_get_enabled() ? "true" : "false",
                          k_interp_curve_enum_name[envelope_interp_get_curve()],
                          envelope_ampeq_get_enabled() ? "true" : "false",
                          envelope_ampeq_shelf2_get_enabled() ? "true" : "false",
                          k_gdeq_variant_enum_name[envelope_gdeq_get_variant()],
                          envelope_alc_get_enabled() ? "true" : "false",
                          envelope_softlimit_get_enabled() ? "true" : "false",
                          ssb_dsp_get_mic_gain_db(dsp_state_get_ssb()),
                          ssb_dsp_get_compressor_level(dsp_state_get_ssb()),
                          ssb_dsp_get_squelch_enabled(dsp_state_get_ssb()) ? "true" : "false",
                          ssb_dsp_get_squelch_threshold(dsp_state_get_ssb()));
        } else if (c >= '0' && c <= '9') {
            int preset = c - '0';
            const PersistentSettings& p = settingsPresets[preset];

            dsp_state_set_audio_source(p.audio_source);
            if (p.audio_source == AUDIO_SRC_MIC) {
                // Same re-baseline as the 'm' handler above - a preset
                // can switch INTO mic mode just as directly as typing
                // 'm' can, and needs the same fresh calibration start.
                adc_capture_resample_prime_calibration();
            }
#if AD9851_ATTACHED
            // relative_delay only exists under AD9851_ATTACHED (see
            // relative_delay.h) - guarded the same way here, so this
            // still compiles standalone (mic->DSP->DAC, no AD9851 board
            // yet) per the AD9851_ATTACHED toggle documented in config.h,
            // not just in the currently-built configuration.
            relative_delay_set_samples(p.relative_delay_samples);
#endif
            envelope_output_set_pwm_offset(p.env_pwm_offset);
            envelope_output_set_pwm_scale(p.env_pwm_scale);
            // Same reset-on-enable reasoning as the 'g' handler - now
            // shared via envelope_gdeq_set_enabled() itself, so it only
            // has to be correct in one place: only reset state on an
            // off->on transition, not on every preset load (all 5 presets
            // currently have gdeq on, so switching between them keeps the
            // filter running continuously, not jumping every time).
            envelope_gdeq_set_enabled(p.env_gdeq_enable);
            adc_capture_set_lpf_mode(p.adc_lpf_mode);

            ssb_dsp_set_eq_enabled(dsp_state_get_ssb(), p.eq_enable);
            ssb_dsp_set_compressor_enabled(dsp_state_get_ssb(), p.compressor_enable);
            dsp_state_set_master_gain_db(p.master_gain_db);

#if AD9851_ATTACHED
            carrier_output_set_rf_enabled(p.ad9851_output_enable);
#endif

            // Both stateless/pure functions of their input (see
            // envelope_predistort_set_enabled()'s and
            // envelope_floor_set()'s own comments) - no reset-on-transition
            // concern the way envelope_gdeq_set_enabled() has, so a plain
            // set on every preset load is correct as-is.
            envelope_predistort_set_enabled(p.env_predistort_enable);
            envelope_floor_set(p.env_floor);

            // Stateless setter (a plain clamp-and-store on the handle,
            // same as master_gain_db) - no reset-on-transition concern,
            // safe to call unconditionally on every preset load.
            ssb_dsp_set_freq_dev_slew_limit_hz(dsp_state_get_ssb(), p.freq_dev_slew_limit_hz);

            // Same reset-on-enable reasoning as the 'g'/'I' handlers -
            // shared via envelope_interp_set_enabled() itself, so an
            // off->on transition on preset load seeds prev/target cleanly
            // too, not just when toggled live.
            envelope_interp_set_enabled(p.envelope_interp_enable);

            // v4.3: plain store, no reset-on-transition concern (see
            // envelope_interp_set_curve()'s own comment) - safe to call
            // unconditionally on every preset load, same as adc_lpf_mode
            // above.
            envelope_interp_set_curve(p.envelope_interp_curve);

            // Same reset-on-enable reasoning as the 'g'/'a'/'A' handlers -
            // shared via each shelf's own _set_enabled(), so an off->on
            // transition on preset load resets that shelf's state cleanly
            // too, not just when toggled live. Independent as of
            // 2026-09-04 (see envelope_ampeq.h) - each preset now controls
            // shelf 1 and shelf 2 separately.
            envelope_ampeq_set_enabled(p.env_ampeq_enable);
            envelope_ampeq_shelf2_set_enabled(p.env_ampeq_shelf2_enable);

            // Same reset-on-change reasoning as the 'G' handler - shared
            // via envelope_gdeq_set_variant() itself, so a preset
            // switching which coefficient set is active also gets a clean
            // filter-state reset, not just when toggled live. Falls back
            // to ENV_GDEQ_VARIANT_DEFAULT on any build where the preset's
            // requested variant wasn't fitted (see envelope_gdeq.h) - safe
            // to call unconditionally either way.
            envelope_gdeq_set_variant(p.env_gdeq_variant);

            // Same reset-on-enable reasoning as the 'l' handler - shared
            // via envelope_alc_set_enabled() itself, so an off->on
            // transition on preset load also resets gain to unity, not
            // just when toggled live. envelope_softlimit_set_enabled() is
            // a plain store (stateless module, no transition concern) -
            // added 2026-09-21 alongside env_alc_enable/env_softlimit_enable.
            envelope_alc_set_enabled(p.env_alc_enable);
            envelope_softlimit_set_enabled(p.env_softlimit_enable);

            // 2026-09-25: replaces the old comp_mode load here. Mic gain
            // is a plain manual trim (same pattern as master_gain_db just
            // above) - no reset-on-change concern. Compression level has
            // none either (see ssb_dsp_set_compressor_level()'s own
            // comment) - safe to call unconditionally on every preset load.
            ssb_dsp_set_mic_gain_db(dsp_state_get_ssb(), p.mic_gain_db);
            ssb_dsp_set_compressor_level(dsp_state_get_ssb(), p.comp_level_db);

            // 2026-09-25: Mic Squelch's two trailing fields - same
            // unconditional-on-every-load pattern as mic gain/compression
            // level just above. ssb_dsp_set_squelch_enabled() resets the
            // gate/detector state on enable, so this is safe even when a
            // preset switches squelch on that was previously off.
            ssb_dsp_set_squelch_threshold(dsp_state_get_ssb(), p.squelch_threshold);
            ssb_dsp_set_squelch_enabled(dsp_state_get_ssb(), p.squelch_enable);

            serial_reply("-> preset %d: %s\r\n", preset, p.name);
        }
    }
}
