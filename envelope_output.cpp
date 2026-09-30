/**
 * envelope_output.cpp - see envelope_output.h.
 */

#include "envelope_output.h"
// 2026-09-08: only needed for the ENVELOPE_INTERP_USE_HW_FADE flag itself
// (gates the ledc_fade_func_install() call below) - this file otherwise has
// no dependency on envelope_interp's own state/API. 2026-09-29: MOVED UP
// to before the ENVELOPE_ISR_INTERP_ENABLED-gated block just below - real-
// hardware compile found that with this include further down (its
// original position, right before the FreeRTOS includes), ENVELOPE_ISR_
// INTERP_ENABLED read as undefined (0) at that block's #if, silently
// skipping the soc/ledc_struct.h include even with the flag set to 1,
// while envelope_output_isr_fasttick_step()'s own body further down in
// this same file (reached AFTER this include) correctly saw the flag as
// 1 and tried to compile its LEDC.* references anyway - "'LEDC' was not
// declared in this scope". Moving this include up fixes it for good,
// rather than patching around one symptom of it.
#include "envelope_interp.h"
#include "driver/i2c.h"
#if PWM_COMPARISON_ENABLED
#include "driver/ledc.h"
#endif
#if PWM_COMPARISON_ENABLED && ENVELOPE_ISR_INTERP_ENABLED
// Raw LEDC peripheral struct access for envelope_output_isr_fasttick_
// step()'s ISR-context write - same idiom, same rationale, and same
// soc/ledc_struct.h header as ISR_PWM_FIXED_TEST_ENABLED's own raw write
// (ssb_mic_test.ino) used for its precursor timing probe.
#include "soc/ledc_struct.h"
#endif
#if SDM_COMPARISON_ENABLED
#include "driver/sdm.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <Arduino.h>

static volatile float s_env_pwm_offset = 0.2f;   // was a hardcoded constant
static volatile float s_env_pwm_scale  = 0.9f;   // was a hardcoded constant

static QueueHandle_t s_envelope_queue;   // length 1, "latest value wins" (xQueueOverwrite)
static TaskHandle_t s_dac_task;
static volatile uint16_t s_dbg_dac_code = 0;

// MCP4725 "Fast Write Command" - 2 data bytes after the address, updates
// the DAC register immediately, does NOT touch EEPROM (EEPROM writes take
// 25-50ms - never do that on this path). Byte layout:
//   byte0 = 0 0 PD1 PD0 D11 D10 D9 D8   (PD1:PD0 = 00 -> normal operation)
//   byte1 = D7 D6 D5 D4 D3 D2 D1 D0
static void mcp4725_fast_write(uint16_t code12)
{
    uint8_t buf[2] = {
        (uint8_t)((code12 >> 8) & 0x0F),
        (uint8_t)(code12 & 0xFF),
    };
    esp_err_t err = i2c_master_write_to_device(MCP4725_I2C_PORT, MCP4725_I2C_ADDR, buf, sizeof(buf), pdMS_TO_TICKS(10));
    if (err != ESP_OK) {
        // Throttled - at 9600Hz we'd otherwise flood the log if the bus is
        // genuinely broken (wrong address, no pull-ups, no ACK, etc.)
        static uint32_t last_err_log_ms = 0;
        uint32_t now = millis();
        if (now - last_err_log_ms >= 1000) {
            last_err_log_ms = now;
            Serial.printf("MCP4725 write failed: %s (check address 0x%02X, pull-ups, wiring)\r\n",
                esp_err_to_name(err), MCP4725_I2C_ADDR);
        }
    }
}

static void dac_task(void *arg)
{
    float envelope = 0.0f;
    while (1) {
        // Blocks here - this task's whole job is to wait for a value and
        // write it out. Whatever this ~70us I2C write costs, it only
        // delays how fresh THIS task's own output is, never dsp_task.
        // Throttling now happens on the SENDING side (envelope_output_
        // submit_dac_sample()) - see DAC_WRITE_DECIMATION. Doing it here
        // via "continue" instead made things worse: xQueueOverwrite wakes
        // a blocked receiver on every call regardless of whether work is
        // skipped, so skipping the write but still re-blocking
        // immediately just meant MORE frequent cross-core wake events,
        // not fewer - the opposite of what we wanted.
        xQueueReceive(s_envelope_queue, &envelope, portMAX_DELAY);

        uint16_t code = (uint16_t)(DAC_CODE_MIN + envelope * (float)(DAC_CODE_MAX - DAC_CODE_MIN));
        mcp4725_fast_write(code);

        s_dbg_dac_code = code;
    }
}

static void init_i2c_dac(void)
{
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = MCP4725_SDA_GPIO;
    conf.scl_io_num = MCP4725_SCL_GPIO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;   // belt-and-braces; use real
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;   // external ~4.7k pull-ups too
    conf.master.clk_speed = MCP4725_I2C_FREQ_HZ;
    i2c_param_config(MCP4725_I2C_PORT, &conf);
    i2c_driver_install(MCP4725_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

#if SDM_COMPARISON_ENABLED
static sdm_channel_handle_t s_sdm_chan = NULL;

// 2026-09-08: brings up the SDM channel on SDM_OUT_GPIO - see
// envelope_output.h's SDM section for the pin/rate/density reasoning.
// `sdm_config_t sdm_cfg = {}` zero-initializes everything not explicitly
// set below (in particular invert_out/io_loop_back/flags, whichever of
// those turns out to be this IDF version's actual field layout for them -
// not yet checked against the installed driver/sdm.h, same "CHECK THIS
// against your actual installed driver" caution as envelope_output_start_
// hw_fade()'s own comment) to their safe off/false defaults: no output
// inversion, no loopback debug mode.
static void init_sdm(void)
{
    sdm_config_t sdm_cfg = {};
    sdm_cfg.clk_src = SDM_CLK_SRC_DEFAULT;
    sdm_cfg.gpio_num = SDM_OUT_GPIO;
    sdm_cfg.sample_rate_hz = SDM_SAMPLE_RATE_HZ;

    esp_err_t err = sdm_new_channel(&sdm_cfg, &s_sdm_chan);
    if (err != ESP_OK) {
        Serial.printf("SDM channel init FAILED on GPIO%d: %s\r\n", SDM_OUT_GPIO, esp_err_to_name(err));
        s_sdm_chan = NULL;
        return;
    }
    err = sdm_channel_enable(s_sdm_chan);
    if (err != ESP_OK) {
        Serial.printf("SDM channel enable FAILED: %s\r\n", esp_err_to_name(err));
        return;
    }
    // Start at density 0 (~50% average -> mid-scale after filtering) as a
    // known, safe boot value, same "explicit known state at boot" idea as
    // the MCP4725 probe's own mid-scale write just below in
    // envelope_output_init().
    sdm_channel_set_pulse_density(s_sdm_chan, 0);
    Serial.printf("SDM channel OK on GPIO%d, sample_rate_hz=%u\r\n", SDM_OUT_GPIO, (unsigned)SDM_SAMPLE_RATE_HZ);
}
#endif

#if PWM_COMPARISON_ENABLED
static void init_rset_mod_pwm(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = RSET_MOD_LEDC_RES,
        .timer_num = RSET_MOD_LEDC_TIMER,
        .freq_hz = RSET_MOD_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t timer_err = ledc_timer_config(&timer_cfg);
    // 2026-09-30: boot-time readback of the LEDC timer's ACTUAL frequency.
    // The 700/1900 I/Q capture showed +/-102.2Hz sidebands on the IMD lines
    // (moving_forward_notes.md, 2026-09-30); leading hypothesis is that 10-
    // bit resolution at 64000Hz needs a 312.5 divider (1/256 units), which
    // the driver must round, so the real carrier is ~63898Hz (div 313) or
    // ~64103Hz (div 312), not 64000, beating against the 64kHz write tick.
    // ledc_get_freq() derives the frequency from the divider the driver
    // actually programmed, so this settles it. Also prints the (previously
    // ignored) ledc_timer_config() return code.
    uint32_t real_hz = ledc_get_freq(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_TIMER);
    Serial.printf("LEDC timer: requested %u Hz, ACTUAL %u Hz (ledc_get_freq), write tick %u Hz, beat %d Hz, "
                  "ledc_timer_config()=%s\r\n",
                  (unsigned)RSET_MOD_LEDC_FREQ_HZ, (unsigned)real_hz,
                  (unsigned)(ENVELOPE_INTERP_FACTOR * SAMPLE_RATE_HZ),
                  (int)((int32_t)real_hz - (int32_t)(ENVELOPE_INTERP_FACTOR * SAMPLE_RATE_HZ)),
                  esp_err_to_name(timer_err));

    ledc_channel_config_t ch_cfg = {
        .gpio_num = RSET_MOD_LEDC_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = RSET_MOD_LEDC_CH,
        .timer_sel = RSET_MOD_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch_cfg);

#if ENVELOPE_INTERP_USE_HW_FADE
    // 2026-09-08: required once before ANY ledc_set_fade_*()/
    // ledc_fade_start() call - installs the LEDC driver's own fade ISR
    // service, which is what actually steps the duty register forward on
    // its own clock once envelope_output_start_hw_fade() (below) kicks a
    // fade off. Argument 0 = no ESP_INTR_FLAG_* fade-ISR allocation flags
    // needed here (default behaviour is fine - this ISR doesn't need to be
    // IRAM-resident/shared/etc. for our purposes). Only installed at all
    // when the flag is on, so the fade ISR isn't silently running unused
    // in the default (software ramp) build.
    ledc_fade_func_install(0);
#endif
}
#endif

void envelope_output_init(void)
{
    init_i2c_dac();
#if PWM_COMPARISON_ENABLED
    init_rset_mod_pwm();
#endif
#if SDM_COMPARISON_ENABLED
    init_sdm();
#endif
    // Explicit connectivity probe - writes mid-scale once so success/failure
    // is obvious in the log immediately at boot, rather than inferred later
    // from DAC behavior.
    {
        uint8_t probe_buf[2] = { 0x08, 0x00 };  // code 0x800 = mid-scale
        esp_err_t probe_err = i2c_master_write_to_device(MCP4725_I2C_PORT, MCP4725_I2C_ADDR,
            probe_buf, sizeof(probe_buf), pdMS_TO_TICKS(50));
        if (probe_err == ESP_OK) {
            Serial.printf("MCP4725 probe OK at address 0x%02X\r\n", MCP4725_I2C_ADDR);
        }
        else {
            Serial.printf("MCP4725 probe FAILED at address 0x%02X: %s - check wiring/pull-ups/address before proceeding\r\n",
                MCP4725_I2C_ADDR, esp_err_to_name(probe_err));
        }
    }

    s_envelope_queue = xQueueCreate(1, sizeof(float));

    // dac_task on Core 1 (with Arduino's own loop(), which is mostly idle
    // here) at low priority - keeps it fully off Core 0, no scheduling
    // interaction with dsp_task at all.
    //
    // Currently compiled out (dac_task_enabled). Note for whenever this
    // is revisited: the Fs-jitter hunt briefly tried moving dsp_task
    // itself onto Core 1 (see the .ino's "TRIED, REVERTED" note on its
    // xTaskCreatePinnedToCore() call) and found real hardware starved
    // Serial completely when dsp_task shared Core 1 with loop() - reverted,
    // dsp_task is back on Core 0. So this comment's premise (dsp_task is
    // on Core 0, dac_task on Core 1, no interaction) still holds today,
    // but if dsp_task's core ever changes again, re-check dac_task's
    // placement against it too rather than assuming this stays apart.
#if dac_task_enabled
    xTaskCreatePinnedToCore(dac_task, "ssb_dac_task", 3072, NULL,
                           tskIDLE_PRIORITY + 1, &s_dac_task, 1);
#endif
}

static volatile bool s_duty_override_enabled = false;

void IRAM_ATTR envelope_output_write_pwm(float delayed_envelope)
{
#if PWM_COMPARISON_ENABLED
#if ISR_PWM_FIXED_TEST_ENABLED
    // 2026-09-29: same "someone else owns the duty register right now"
    // idiom as the s_duty_override_enabled check right below - added
    // after the first two ISR_PWM_FIXED_TEST_ENABLED bench results
    // (config.h/on_timer_alarm() in ssb_mic_test.ino) showed the pin
    // alternating between the fixed test value and the real envelope
    // instead of holding solidly, on a suspiciously stable ~10ms cycle
    // that a simple per-tick register race (15.6us/62.5us periods)
    // doesn't explain. This task-context write was never actually
    // disabled by that flag - it kept running at its normal full-tick
    // rate the whole time, fighting the new raw ISR write for the same
    // register. Silencing it here turns the bench test into a genuinely
    // single-writer experiment: if the pin now holds rock solid, that
    // confirms the two writers really were racing (and the "why did the
    // less-frequent writer often win" question becomes moot once this is
    // the real fast-tick write's replacement, not a real feature); if the
    // pin STILL alternates with this writer fully silenced, the ISR/raw
    // write itself (or something external stalling it) is the next
    // suspect, not a two-writer race - see moving_forward_notes.md's
    // 2026-09-29 entries for the fuller reasoning and the recommended
    // two-channel scope correlation (TIMING_DEBUG_GPIO_ISR vs
    // RSET_MOD_LEDC_GPIO) for that case.
    return;
#endif
#if ENVELOPE_ISR_INTERP_ENABLED
    // 2026-09-29: defense-in-depth only - envelope_interp_on_full_tick()/
    // on_interp_tick() (envelope_interp.cpp) already bypass this function
    // entirely while ENVELOPE_ISR_INTERP_ENABLED is on (they're this
    // function's only callers anywhere in the project - confirmed by
    // grep), routing through envelope_output_isr_stage_step()/
    // _isr_fasttick_step() instead. This early-return exists only so a
    // future caller added here without knowing that history can't
    // accidentally reintroduce the exact two-writer race
    // ISR_PWM_FIXED_TEST_ENABLED's own bench investigation just spent
    // three rounds diagnosing - see that flag's own comment right above.
    return;
#endif
    if (s_duty_override_enabled) {
        // Direct duty-set command (envelope_output_write_duty_raw(), via
        // serial_commands.cpp's 'd'/'>'/'<'/'N'/'B') owns the LEDC duty
        // register right now - see envelope_output.h's header comment.
        return;
    }
    uint32_t max_duty = (1u << RSET_MOD_LEDC_RES) - 1u;
    uint32_t duty = (uint32_t)(delayed_envelope * (float)max_duty);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
#else
    (void)delayed_envelope;
#endif
}

#if PWM_COMPARISON_ENABLED && ENVELOPE_ISR_INTERP_ENABLED
// Q4 fixed point throughout - i.e. the LEDC hardware duty register's own
// native units (RSET_MOD_LEDC_RES-bit duty count, << 4 for the hardware's
// fractional sub-duty bits). See envelope_output.h's own comment on these
// two functions for the full design/reasoning.
static volatile int32_t s_isr_interp_step_q4  = 0;   // written by dsp_task, read by the ISR
static volatile int32_t s_isr_interp_accum_q4 = 0;   // ISR-owned running duty accumulator
static int32_t          s_isr_interp_last_target_q4 = 0;   // dsp_task-local only - never touched by the ISR
// 2026-09-30: see the CORRECTION comment inside envelope_output_isr_stage_
// step() below - this carries the integer-division remainder forward
// instead of discarding it every tick. dsp_task-local only, same as
// s_isr_interp_last_target_q4 - never touched by the ISR.
static int32_t          s_isr_interp_carry_q4 = 0;
// 2026-09-30: diagnostic-only - see envelope_output_isr_interp_get_debug()
// (envelope_output.h) for why this exists. dsp_task-local only, same as
// s_isr_interp_last_target_q4/s_isr_interp_carry_q4 - never touched by the
// ISR, so no atomicity concern beyond the same "one tick stale is fine"
// tolerance already accepted for the rest of this staged handoff.
static float             s_isr_interp_last_envelope = 0.0f;
// 2026-09-30: diagnostic-only ISR liveness counters, see envelope_output_isr_
// interp_get_debug(). calls_total increments on EVERY entry to envelope_
// output_isr_fasttick_step() (before the duty-override guard); calls_past_
// guard only when the guard did NOT early-return. Written by the ISR only,
// read from task context - plain aligned 32-bit, same tolerance as the rest.
static volatile uint32_t s_isr_interp_calls_total = 0;
static volatile uint32_t s_isr_interp_calls_past_guard = 0;

// 2026-09-30: staging-error monitor for the closed-loop stage step below -
// see envelope_output_isr_stage_step()'s THIRD CORRECTION comment. At each
// stage, e = (ISR accumulator right now) - (the PREVIOUS staged target).
// If the previous step was applied exactly ENVELOPE_INTERP_FACTOR times,
// e is only the integer-division remainder of the previous stage, i.e.
// |e| <= FACTOR-1. A larger |e| means the previous step was applied a
// different number of times (staging landed across a fast-tick boundary, a
// coalesced/overrun tick, ...) - exactly the event that used to leave a
// permanent duty error. dsp_task writes these; the 'b' command (Core 1)
// reads them: plain aligned 32-bit, diagnostic-only, so no lock.
// s_isr_interp_stage_err_valid excludes the first stage after boot, after a
// duty-override exit reseed, and every stage while override is ON (the ISR
// is locked out then, so the accumulator is frozen and e is meaningless).
static volatile int32_t  s_isr_interp_stage_err_q4 = 0;          // most recent e
static volatile int32_t  s_isr_interp_stage_err_max_abs_q4 = 0;  // since boot: max |e|
static volatile uint32_t s_isr_interp_stage_err_events = 0;      // since boot: stages with |e| > FACTOR-1
static volatile uint32_t s_isr_interp_stage_err_samples = 0;     // since boot: stages that were measured
static bool              s_isr_interp_stage_err_valid = false;

void envelope_output_isr_stage_step(float envelope)
{
    if (envelope < 0.0f) envelope = 0.0f;
    if (envelope > 1.0f) envelope = 1.0f;
    s_isr_interp_last_envelope = envelope;

    const int32_t max_duty_q4 = (int32_t)(((1u << RSET_MOD_LEDC_RES) - 1u) << 4);
    // Manual round-to-nearest rather than roundf()/lroundf() - avoids
    // pulling in an extra libm call from a function that, while it runs
    // in ordinary task context (float is fine here), is still on
    // dsp_task's real-time full-tick path. envelope is already clamped
    // non-negative above, so a plain +0.5f is exact - no need for the
    // sign-dependent rounding a general-purpose round() has to handle.
    int32_t target_q4 = (int32_t)(envelope * (float)max_duty_q4 + 0.5f);
    if (target_q4 > max_duty_q4) target_q4 = max_duty_q4;   // guards the envelope==1.0f/rounding-up edge case

    // 2026-09-30 CORRECTION - this originally computed
    // `step_q4 = (target_q4 - s_isr_interp_last_target_q4) / FACTOR`
    // and discarded the integer-division remainder completely every tick.
    // That's a real bug, not just rounding wobble: real hardware showed
    // output "pinned to zero"-ish under normal (non-duty-override) two-tone
    // content even with confirmed-sane offset/scale, and the reason is
    // this - whenever two CONSECUTIVE full-tick targets differ by LESS
    // than ENVELOPE_INTERP_FACTOR counts (extremely common for real,
    // continuously-varying content, not a rare edge case - most of a
    // smooth signal's sample-to-sample change is small relative to a
    // ~16368-count Q4 range), integer division truncates that step to
    // EXACTLY ZERO, and since s_isr_interp_last_target_q4 is updated to
    // the fresh target regardless, the difference is simply thrown away -
    // it is NEVER added to a future tick's calculation, because every
    // future step is computed only against the immediately preceding
    // target, not against how far the accumulator has actually drifted
    // from the true trajectory. Worked through the algebra: the drift
    // this leaves behind is a genuine unbounded random walk (e_n = e_0 -
    // sum of each tick's discarded remainder), NOT the "at most
    // (FACTOR-1)-count wobble, never accumulates" property the original
    // version of this comment claimed - that claim was simply wrong,
    // verified by hand against a small worked example that happened to
    // self-cancel by coincidence, not by a real proof.
    //
    // Fix: standard Bresenham/DDA-style error diffusion - carry the
    // remainder forward into the NEXT tick's numerator instead of
    // discarding it, so no fractional part is ever permanently lost. This
    // is what actually delivers the "telescopes exactly to the true
    // target trajectory, only a bounded wobble along the way" property in
    // reality, not just in a comment.
    //
    // 2026-09-30 THIRD CORRECTION - CLOSED LOOP (supersedes the
    // Bresenham-carry numerator above; the paragraph above is kept as
    // history of why plain truncation was wrong). Found by reading the
    // handoff, not by a scope: the ISR accumulator was an OPEN-LOOP
    // integrator - each staged step is supposed to be applied exactly
    // FACTOR times, but if it is applied 3 or 5 times (dsp_task's stage
    // lands on the other side of a 64 kHz fast-tick boundary because of
    // wake jitter, or a tick is coalesced/overrun) the accumulator ends up
    // off by (n-FACTOR) x step, and since every later step was computed
    // from target-to-target differences only, that error was NEVER
    // corrected (bounded per event, but a random walk over many events; a
    // model simulation with an invented 0.1 % late-stage rate reached
    // ~100 native duty counts on a 1.2 kHz tone; an earlier 'b' snapshot
    // showed accum 846 vs target 23 at idle, consistent with, but not
    // proof of, this). Fix: derive each step from where the accumulator
    // ACTUALLY is - step = (target - accum_now)/FACTOR - so any error is
    // removed within one period instead of persisting. This deliberately
    // adds a read-back (ISR-owned accum, one aligned 32-bit volatile load)
    // to what used to be a one-directional handoff; a read that is one
    // fast tick stale costs at most one step of transient error which the
    // NEXT stage corrects, it cannot accumulate. The old carry is no longer
    // needed: the integer-division remainder is left in the accumulator's
    // own state and is included automatically in the next (target - accum).
    // carry_q4 is kept only as a diagnostic: it now reports that remainder.
    // dsp_task's stage runs in the quiet window between the ISR's fast-tick
    // writes (the ISR touches the accumulator for ~1us at the start of each
    // 15.6us fast tick), so a torn/interleaved read is not expected.
    const int32_t accum_now = s_isr_interp_accum_q4;

    if (s_duty_override_enabled) {
        // ISR write is locked out, accum is frozen - e would be garbage.
        s_isr_interp_stage_err_valid = false;
    } else {
        if (s_isr_interp_stage_err_valid) {
            int32_t e = accum_now - s_isr_interp_last_target_q4;
            int32_t ae = (e < 0) ? -e : e;
            s_isr_interp_stage_err_q4 = e;
            if (ae > s_isr_interp_stage_err_max_abs_q4) s_isr_interp_stage_err_max_abs_q4 = ae;
            if (ae > ((int32_t)ENVELOPE_INTERP_FACTOR - 1)) s_isr_interp_stage_err_events = s_isr_interp_stage_err_events + 1u;
            s_isr_interp_stage_err_samples = s_isr_interp_stage_err_samples + 1u;
        }
        s_isr_interp_stage_err_valid = true;
    }

    int32_t numerator = target_q4 - accum_now;
    int32_t step_q4 = numerator / (int32_t)ENVELOPE_INTERP_FACTOR;
    s_isr_interp_carry_q4 = numerator - step_q4 * (int32_t)ENVELOPE_INTERP_FACTOR;   // diagnostic only now - see above
    s_isr_interp_last_target_q4 = target_q4;

    // Single aligned 32-bit store - atomic (no tearing) on this core by
    // construction of the Xtensa ISA, which is all this needs: dsp_task is
    // the only writer, the ISR is the only reader, and reading a step
    // that's one fast-tick stale (the ordinary cross-core race window
    // every staged-value handoff in this codebase already accepts, e.g.
    // the AD9851 path's own s_pending_tx_freq) costs at most one tick of
    // slightly-off interpolation, not a correctness bug - a much looser
    // tolerance than the AD9851 frequency path had, so the lighter-weight
    // plain-volatile handoff used there for that same reason is used here
    // too, without that path's extra sequence-number staleness counting.
    s_isr_interp_step_q4 = step_q4;
}

void IRAM_ATTR envelope_output_isr_fasttick_step(void)
{
    s_isr_interp_calls_total = s_isr_interp_calls_total + 1u;
    if (s_duty_override_enabled) {
        // 2026-09-30: MISSING GUARD, found via a real-hardware bug report
        // ("duty mode is not updating the pwm"). envelope_output_write_
        // duty_raw() (the 'd'+'>'/'<' override path) writes the LEDC duty
        // register via the normal ledc_set_duty()/ledc_update_duty()
        // driver calls, but this function was running unconditionally on
        // EVERY fast tick regardless, and immediately overwriting that
        // write with its own envelope-derived accumulator value on the
        // very next 15.6us tick - the exact same "two writers fighting
        // over one register" shape as ISR_PWM_FIXED_TEST_ENABLED's own
        // precursor bug (see that flag's comment, config.h), just never
        // guarded here because this function never got the same
        // duty-override check envelope_output_write_pwm()/start_hw_fade()/
        // write_sdm() all already have. See envelope_output_duty_override_
        // set_enabled()'s own comment for how the ISR's accumulator state
        // is reseeded when override mode is exited, so returning early
        // here doesn't leave stale interpolation state behind.
        return;
    }
    s_isr_interp_calls_past_guard = s_isr_interp_calls_past_guard + 1u;
    const int32_t max_duty_q4 = (int32_t)(((1u << RSET_MOD_LEDC_RES) - 1u) << 4);
    int32_t accum = s_isr_interp_accum_q4 + s_isr_interp_step_q4;
    // Defensive clamp only - dsp_task's own target clamp (above) should
    // already keep this in range; cheap insurance against this ISR ever
    // writing a garbage/out-of-range value to real hardware if that
    // invariant is ever violated by a future change.
    if (accum < 0) accum = 0;
    if (accum > max_duty_q4) accum = max_duty_q4;
    s_isr_interp_accum_q4 = accum;

    // Raw peripheral struct write, NOT ledc_set_duty()/ledc_update_duty()
    // - see envelope_output.h's comment on this function and config.h's
    // ISR_PWM_FIXED_TEST_ENABLED comment for why those driver calls are
    // NOT ISR-safe. conf0.low_speed_update is the commit-trigger step
    // found necessary during that flag's own bench investigation - ESP32-
    // S2/S3 has no LEDC high-speed mode, so every channel needs it beyond
    // conf1.duty_start before a written duty value actually latches. Same
    // field-name-uncertainty caveat as that diagnostic: reasoned from
    // general ESP32/S2/S3 LEDC knowledge, not verified against this
    // project's exact installed SDK version - a wrong name fails to
    // compile rather than misbehaving silently.
    //
    // *** SUPERSEDED - the conclusion of this paragraph was WRONG, see the
    // SECOND CORRECTION directly below it. Kept only as history. ***
    // 2026-09-30 CORRECTION: `accum` is kept in Q4 (<<4) internally for
    // software accumulation precision (see the file-scope comment above
    // these statics), but real hardware confirmed `duty.duty` itself wants
    // the PLAIN native duty count (0..(1<<RSET_MOD_LEDC_RES)-1), NOT that
    // value pre-shifted left by 4 - real-hardware bug report: with normal
    // envelope content (two-tone, not duty-override), the PWM output was
    // "stuck at max env level" the moment the true envelope exceeded
    // roughly 1/16 of full scale, which is exactly what happens if
    // anything above native-max (1023) is being written into a comparator
    // that only ever counts 0..1023 for this configured resolution/rate -
    // values above that just latch the output high for the whole period.
    // The now-confirmed-correct 'd'+'>'/'<' duty-override path
    // (envelope_output_write_duty_raw(), via plain ledc_set_duty()) writes
    // exactly that same native 0..1023 range with NO shift, and IS
    // correct on the bench - so the raw write here now matches it by
    // converting back down (>>4, rounding to nearest) at the point of
    // writing to hardware, rather than assuming (as originally written,
    // unverified) that the register wants the Q4 value pre-embedded. This
    // reverses the original "matching the LEDC hardware's own fractional
    // bits" assumption in envelope_output.h's comment on this function -
    // that assumption is now known wrong for this build, not merely
    // unverified.
    // 2026-09-30 SECOND CORRECTION (reverts the ">>4 to native units" change
    // above): the real-hardware 'b' readout (Preset 1, override off) showed
    // ledc_get_duty() reading back almost exactly accum/16 in every sample
    // (accum_q4>>4 ~542 -> hw_duty_now 33-35; ~514 -> 32-33), against an
    // expected ~457 for envelope 0.446. So duty.duty DOES carry 4 fractional
    // bits, and ledc_get_duty() returns that register divided by 16 - which
    // matches my recollection of ESP-IDF's own ledc_ll_set_duty_int_part()
    // (writes duty_val << 4) but that recollection is NOT verified against
    // this SDM install; the 16:1 readback ratio is the actual evidence. The
    // original Q4 write was therefore CORRECT, and the ">>4" change made
    // the output 16x too small. The earlier "stuck at max" report was NOT
    // proven to be a scaling bug - see moving_forward_notes.md for what is
    // and isn't known about it.
    uint32_t reg_duty = (uint32_t)accum;   // already clamped to [0, max_duty_q4] above
    LEDC.channel_group[LEDC_LOW_SPEED_MODE].channel[RSET_MOD_LEDC_CH].duty.duty = reg_duty;
    LEDC.channel_group[LEDC_LOW_SPEED_MODE].channel[RSET_MOD_LEDC_CH].conf1.duty_start = 1;
    LEDC.channel_group[LEDC_LOW_SPEED_MODE].channel[RSET_MOD_LEDC_CH].conf0.low_speed_update = 1;
}

// 2026-09-30: called from envelope_output_duty_override_set_enabled() on
// the ON->OFF transition (leaving duty-override mode) - see that
// function's own comment for why. Re-anchors the ISR's running
// accumulator/last-staged-target state to whatever duty the override path
// actually left on the hardware, read back via the verified ledc_get_
// duty() driver call (task context only - this is never called from the
// ISR itself, only from the same non-ISR context envelope_output_write_
// duty_raw() already runs in). Without this, the accumulator would still
// hold whatever it was frozen at the INSTANT override was entered, and
// resuming from that stale value on the next full tick would step toward
// the wrong place rather than from wherever the hardware actually sits.
// Setting s_isr_interp_step_q4 to 0 as well means the very first fast
// tick after reseeding holds still (no step yet) rather than acting on a
// leftover step computed against the old, no-longer-relevant target.
static void envelope_output_isr_interp_reseed_from_hw(void)
{
    uint32_t raw_duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
    int32_t q4 = (int32_t)(raw_duty << 4);
    s_isr_interp_accum_q4 = q4;
    s_isr_interp_last_target_q4 = q4;
    s_isr_interp_step_q4 = 0;
    // 2026-09-30: zero the carry too (see envelope_output_isr_stage_step()'s
    // own dated comment on why the carry exists) - an intentional jump is
    // being introduced right here (last_target snapped to the override's
    // leftover value, not the real envelope's actual last target), so a
    // stale carry from before override was entered has nothing meaningful
    // left to correct and should not be combined with it.
    s_isr_interp_carry_q4 = 0;
    // The first stage after this jump is not a valid staging-error sample.
    s_isr_interp_stage_err_valid = false;
}

// 2026-09-30: see envelope_output.h's own comment on this function for why
// it exists. Plain task-context reads - ledc_get_duty() is the same driver
// call envelope_output_isr_interp_reseed_from_hw() already uses from this
// same (non-ISR) context, so no new ISR-safety question here.
void envelope_output_isr_interp_get_debug(envelope_output_isr_interp_debug_t *out)
{
    if (!out) return;
    out->last_envelope  = s_isr_interp_last_envelope;
    out->last_target_q4 = s_isr_interp_last_target_q4;
    out->step_q4         = s_isr_interp_step_q4;
    out->accum_q4         = s_isr_interp_accum_q4;
    out->carry_q4         = s_isr_interp_carry_q4;
    out->hw_duty_now      = ledc_get_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
    out->calls_total      = s_isr_interp_calls_total;
    out->calls_past_guard = s_isr_interp_calls_past_guard;
    out->override_on      = s_duty_override_enabled ? 1u : 0u;
    out->ledc_freq_hz     = ledc_get_freq(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_TIMER);
    out->stage_err_q4         = s_isr_interp_stage_err_q4;
    out->stage_err_max_abs_q4 = s_isr_interp_stage_err_max_abs_q4;
    out->stage_err_events     = s_isr_interp_stage_err_events;
    out->stage_err_samples    = s_isr_interp_stage_err_samples;
}

// 2026-09-30: clears the staging-error statistics (called from
// diagnostics_reset(), i.e. the 'r' command) so a window can be measured
// AFTER start-up, which the user reports is when odd things happen. Also
// clears the valid flag so the first stage after the reset is not measured
// (no previous-stage reference to compare against is guaranteed). Plain
// stores from Core 1 while dsp_task (Core 0) may be incrementing the same
// counters: a lost update at the instant of the reset can leave one stale
// value that is at most one stage / one event out - acceptable for a
// diagnostic, and not worth a lock in a path dsp_task runs every tick.
void envelope_output_isr_interp_reset_stage_err_stats(void)
{
    s_isr_interp_stage_err_valid      = false;
    s_isr_interp_stage_err_q4         = 0;
    s_isr_interp_stage_err_max_abs_q4 = 0;
    s_isr_interp_stage_err_events     = 0;
    s_isr_interp_stage_err_samples    = 0;
}
#else
void envelope_output_isr_stage_step(float envelope)
{
    (void)envelope;   // ENVELOPE_ISR_INTERP_ENABLED (envelope_interp.h) is 0 - nothing to stage
}

// 2026-09-30: no-op stand-in, same "always-safe-to-call" convention as the
// reseed stand-in right below - zero-fills so a caller that forgets to
// check the compile-time flag first still gets well-defined (if useless)
// output rather than reading uninitialized memory.
void envelope_output_isr_interp_get_debug(envelope_output_isr_interp_debug_t *out)
{
    if (!out) return;
    out->last_envelope  = 0.0f;
    out->last_target_q4 = 0;
    out->step_q4        = 0;
    out->accum_q4        = 0;
    out->carry_q4        = 0;
    out->hw_duty_now      = 0;
    out->calls_total      = 0;
    out->calls_past_guard = 0;
    out->override_on      = 0;
    out->ledc_freq_hz     = 0;
    out->stage_err_q4         = 0;
    out->stage_err_max_abs_q4 = 0;
    out->stage_err_events     = 0;
    out->stage_err_samples    = 0;
}

// 2026-09-30: no-op stand-in, same always-safe-to-call convention as the
// other stand-ins in this #else block (diagnostics_reset() calls it
// unconditionally).
void envelope_output_isr_interp_reset_stage_err_stats(void)
{
}

// 2026-09-30: no-op stand-in so envelope_output_duty_override_set_enabled()
// can call this unconditionally regardless of ENVELOPE_ISR_INTERP_ENABLED,
// same "always-safe-to-call no-op" convention as the other stand-ins in
// this #else block.
static void envelope_output_isr_interp_reseed_from_hw(void)
{
}

void IRAM_ATTR envelope_output_isr_fasttick_step(void)
{
    // ENVELOPE_ISR_INTERP_ENABLED (envelope_interp.h) is 0 - no-op. Kept
    // as a callable no-op rather than compiling the call site out in
    // on_timer_alarm() (ssb_mic_test.ino), so that call site doesn't need
    // its own #if - matches this project's general preference for a
    // cheap always-safe-to-call no-op over spreading the same #if guard
    // across multiple files (see e.g. envelope_output_write_sdm()'s own
    // no-op-when-disabled behavior for the same reasoning).
}
#endif

// 2026-09-08: see envelope_output.h's own comment on this function for the
// overall design (why `steps` is a plain parameter, the approximate/not-
// yet-bench-verified rounding). Implementation notes specific to THIS body:
//
// ledc_set_fade_with_step()'s signature, per the ESP-IDF driver/ledc.h this
// was written against:
//   esp_err_t ledc_set_fade_with_step(ledc_mode_t speed_mode,
//       ledc_channel_t channel, uint32_t target_duty, uint32_t scale,
//       uint32_t cycle_num)
// - CHECK THIS against your actual installed driver/ledc.h before trusting
// this compiles/behaves as written; a signature mismatch here would be a
// clean compile error (easy to fix), but a semantic mismatch (e.g. if some
// IDF version's `scale` means something other than "duty counts per step")
// would silently mis-shape the ramp instead - not verified on this bench.
//
// `scale` is a per-step DUTY COUNT (not a step count) and `cycle_num` is
// how many LEDC PWM periods each step holds before advancing - so to land
// on approximately `steps` hardware steps, back `scale` out as the total
// current-to-target duty distance divided by `steps` (floor via integer
// division - see envelope_output.h for why this makes the real step count
// only approximately `steps`, not exact). cycle_num=1 (advance every PWM
// period) is what actually spreads the fade across `steps` full
// RSET_MOD_LEDC_FREQ_HZ periods - the entire point of retuning
// RSET_MOD_LEDC_FREQ_HZ to be commensurate with the tick rate (see that
// #define's own comment): ENVELOPE_INTERP_FACTOR periods at 64kHz line up
// with one gptimer tick interval, which is exactly how envelope_interp.cpp
// calls this (steps == ENVELOPE_INTERP_FACTOR).
void IRAM_ATTR envelope_output_start_hw_fade(float target_envelope, uint32_t steps)
{
#if PWM_COMPARISON_ENABLED
    if (s_duty_override_enabled) {
        // Same early-return as envelope_output_write_pwm() above - direct
        // duty-set command owns the LEDC duty register right now.
        return;
    }
    if (steps < 1) {
        steps = 1;   // defensive - avoid a divide-by-zero below; callers
                      // are expected to always pass ENVELOPE_INTERP_FACTOR (>=1)
    }

    uint32_t max_duty = (1u << RSET_MOD_LEDC_RES) - 1u;
    uint32_t target_duty = (uint32_t)(target_envelope * (float)max_duty);
    if (target_duty > max_duty) {
        target_duty = max_duty;
    }

    uint32_t current_duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
    uint32_t distance = (target_duty > current_duty) ? (target_duty - current_duty)
                                                       : (current_duty - target_duty);
    uint32_t scale = distance / steps;
    if (scale < 1) {
        // Distance smaller than `steps` (or zero) - still take at least
        // one real hardware step of size 1 rather than passing scale=0,
        // which ledc_set_fade_with_step() would likely reject/no-op.
        scale = 1;
    }

    ledc_set_fade_with_step(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, target_duty, scale, 1);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, LEDC_FADE_NO_WAIT);
#else
    (void)target_envelope;
    (void)steps;
#endif
}

void envelope_output_sync_ledc_timer_now(void)
{
#if PWM_COMPARISON_ENABLED
    // See envelope_output.h's own comment on this function for why this
    // matters - resets the LEDC timer's internal counter to a known phase
    // so its autonomous fade-step clock and the sample gptimer's alarm
    // grid share a common reference point instead of an arbitrary power-
    // on-to-power-on offset.
    ledc_timer_rst(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_TIMER);
#endif
}

// 2026-09-08: see envelope_output.h's own comment on this function for the
// call-site/timing reasoning (called once per real dsp_task tick,
// independent of 'I'/HW_FADE/curve state), including the 2026-09-08 "later"
// note on the stage/commit-from-ISR variant this reverted FROM - real
// hardware came back worse, not better, with that split (see the header
// comment and group_delay_fit_notes.md's matching entry for the two
// suspected mechanisms).
//
// 2026-09-08, later still: gained the SAME duty-override early-return
// envelope_output_write_pwm()/start_hw_fade() already had - see
// envelope_output_write_duty_raw()'s own comment below for why. 'd' now
// drives whichever comparison path is actually compiled in (PWM or SDM,
// they're mutually exclusive - see the SDM_OUT_GPIO #error above), so this
// path needs to defer to it too, exactly like PWM's write function does.
void IRAM_ATTR envelope_output_write_sdm(float envelope)
{
#if SDM_COMPARISON_ENABLED
    if (s_duty_override_enabled) {
        // Direct override command ('d' + '>'/'<'/'N'/'B', now density-
        // aware - see envelope_output_write_duty_raw()) owns the SDM
        // channel right now.
        return;
    }
    if (s_sdm_chan == NULL) {
        // Either init_sdm() failed (see its own error log at boot) or
        // SDM_COMPARISON_ENABLED was flipped on without a successful
        // channel bring-up - fail silent/no-op rather than dereferencing
        // a null handle.
        return;
    }
    if (envelope < 0.0f) {
        envelope = 0.0f;
    } else if (envelope > 1.0f) {
        envelope = 1.0f;
    }
    int32_t density = (int32_t)(-(float)SDM_DENSITY_CLAMP + envelope * (2.0f * (float)SDM_DENSITY_CLAMP));
    if (density < -128) {
        density = -128;
    } else if (density > 127) {
        density = 127;
    }
    sdm_channel_set_pulse_density(s_sdm_chan, (int8_t)density);
#else
    (void)envelope;
#endif
}

// 2026-09-08: generalized from PWM-only to dual-purpose - see this
// function's own comment in envelope_output.h for the full reasoning.
// `duty` is really "the raw override INDEX," 0..envelope_output_get_max_
// duty() - under PWM_COMPARISON_ENABLED it's a literal LEDC duty count as
// it always was; under SDM_COMPARISON_ENABLED it's linearly remapped onto
// signed density [-SDM_DENSITY_CLAMP,+SDM_DENSITY_CLAMP], index 0 ->
// -SDM_DENSITY_CLAMP, index max -> +SDM_DENSITY_CLAMP - the exact same
// affine mapping envelope_output_write_sdm() uses for envelope=0/1, so a
// swept index means the same thing to both output paths. This is what
// lets 'd'/'>'/'<'/'N'/'B'/'E' (serial_commands.cpp) - and any existing
// automated sweep harness already driving those same keys - work
// unchanged regardless of which comparison path is compiled in; only one
// of PWM_COMPARISON_ENABLED/SDM_COMPARISON_ENABLED can be 1 at a time (see
// envelope_output.h's build-time #error), so there's no runtime ambiguity
// about which peripheral this actually writes to.
void IRAM_ATTR envelope_output_write_duty_raw(uint32_t duty)
{
#if PWM_COMPARISON_ENABLED
    uint32_t max_duty = (1u << RSET_MOD_LEDC_RES) - 1u;
    if (duty > max_duty) {
        duty = max_duty;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, RSET_MOD_LEDC_CH);
#elif SDM_COMPARISON_ENABLED
    if (s_sdm_chan == NULL) {
        return;
    }
    uint32_t max_index = 2u * (uint32_t)SDM_DENSITY_CLAMP;
    if (duty > max_index) {
        duty = max_index;
    }
    int32_t density = (int32_t)duty - (int32_t)SDM_DENSITY_CLAMP;
    sdm_channel_set_pulse_density(s_sdm_chan, (int8_t)density);
#else
    (void)duty;
#endif
}

// 2026-09-08: generalized alongside envelope_output_write_duty_raw() above -
// see that function's comment. Returns the max valid override INDEX for
// whichever comparison path is compiled in (LEDC duty count under PWM,
// 2*SDM_DENSITY_CLAMP under SDM), 0 if neither is enabled.
uint32_t envelope_output_get_max_duty(void)
{
#if PWM_COMPARISON_ENABLED
    return (1u << RSET_MOD_LEDC_RES) - 1u;
#elif SDM_COMPARISON_ENABLED
    return 2u * (uint32_t)SDM_DENSITY_CLAMP;
#else
    return 0;
#endif
}

bool envelope_output_duty_override_get_enabled(void)
{
    return s_duty_override_enabled;
}

void envelope_output_duty_override_set_enabled(bool enable)
{
    // 2026-09-30: entering override (false->true) needs nothing extra -
    // envelope_output_isr_fasttick_step() now checks s_duty_override_
    // enabled itself (see its own comment/fix, same date) and simply stops
    // touching the register, exactly like envelope_output_write_pwm()
    // already did. EXITING override (true->false) is the case that needs
    // help: while override was active, envelope_output_write_duty_raw()
    // was driving the hardware duty register directly, but the ISR
    // interpolation accumulator/last-target state was frozen at whatever
    // it held the instant override was entered - resuming from that stale
    // state would jump toward the wrong place instead of continuing from
    // wherever the hardware actually sits. Checked BEFORE updating
    // s_duty_override_enabled itself, so this only fires on a genuine
    // on->off transition, not every call.
    if (s_duty_override_enabled && !enable) {
        envelope_output_isr_interp_reseed_from_hw();
    }
    s_duty_override_enabled = enable;
}

void IRAM_ATTR envelope_output_submit_dac_sample(float envelope)
{
    // Non-blocking, always succeeds - overwrites whatever was there.
    // dac_task will pick up the latest value whenever it next runs; this
    // call never waits on the I2C bus.
    //
    // Throttled to DAC_TARGET_UPDATE_RATE_HZ: xQueueOverwrite wakes
    // dac_task's blocked receiver on every call, so calling it every
    // sample means waking the other core at the full DSP rate even when
    // most of those wakes would do nothing but immediately re-block.
    // Skipping the call itself (not just the write on the receiving end)
    // genuinely reduces cross-core wake frequency, which is what we've
    // confirmed actually causes the jitter. dac_skip_count persists
    // across calls the same way it did as a dsp_task-local variable in
    // the original .ino (that variable was declared once outside
    // dsp_task's infinite while(1) loop and never reset, so a file-static
    // counter here is exactly equivalent).
    static uint32_t dac_skip_count = 0;
    dac_skip_count++;
    if (dac_skip_count >= DAC_WRITE_DECIMATION) {
        dac_skip_count = 0;
        xQueueOverwrite(s_envelope_queue, &envelope);
    }
}

uint16_t envelope_output_get_last_dac_code(void)
{
    return s_dbg_dac_code;
}

float envelope_output_get_pwm_offset(void)
{
    return s_env_pwm_offset;
}

float envelope_output_get_pwm_scale(void)
{
    return s_env_pwm_scale;
}

void envelope_output_raise_pwm_offset(void)
{
    if (s_env_pwm_offset < 1.0f) s_env_pwm_offset += ENV_PWM_STEP;
}

void envelope_output_lower_pwm_offset(void)
{
    if (s_env_pwm_offset > 0.0f) s_env_pwm_offset -= ENV_PWM_STEP;
}

void envelope_output_widen_pwm_scale(void)
{
    if (s_env_pwm_scale < 1.0f) s_env_pwm_scale += ENV_PWM_STEP;
}

void envelope_output_narrow_pwm_scale(void)
{
    if (s_env_pwm_scale > 0.0f) s_env_pwm_scale -= ENV_PWM_STEP;
}

void envelope_output_set_pwm_offset(float offset)
{
    s_env_pwm_offset = offset;
}

void envelope_output_set_pwm_scale(float scale)
{
    s_env_pwm_scale = scale;
}
