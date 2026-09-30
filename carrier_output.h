#pragma once

/**
 * carrier_output.h
 *
 * AD9851 DDS carrier: init, per-tick frequency update (carrier + delayed
 * frequency deviation), and RF output on/off. Only compiled in when
 * AD9851_ATTACHED (see config.h) - mirrors the original .ino's guarding
 * exactly, so this still builds standalone (mic->DSP->DAC, no AD9851
 * board yet) with AD9851_ATTACHED set to 0.
 */

#include "config.h"

#if AD9851_ATTACHED

#include <stdint.h>
#include <stdbool.h>
#include "ad9851.h"

#define AD9851_PIN_DATA   10
#define AD9851_PIN_WCLK   12
#define AD9851_PIN_FQUD   11
#define AD9851_PIN_RESET  9
#define REF_CLK_HZ        30000000u
#define CARRIER_HZ        14200162u  // +162Hz calibration offset - this AD9851 module's actual
                                      // REF_CLK isn't precisely 30MHz (expected given it's an
                                      // uncalibrated XO, not a precision reference); this value
                                      // makes the real transmitted output land on 14200000 exactly,
                                      // confirmed against the user's calibrated receiver
                                      //
                                      // 2026-09-30: 14200160 -> 14200162. The 700/1900 SDRuno I/Q
                                      // capture (centre 14175kHz, so the carrier should sit at
                                      // +25000Hz baseband) put the carrier at ~24997.9Hz, i.e. ~2.1Hz
                                      // LOW, and the user confirmed the AD9851 absolute frequency
                                      // is slightly low. The carrier position was inferred as
                                      // (lower tone peak - 700Hz), so it is good to roughly
                                      // +/-0.25Hz (FFT bin), not better; the SDR's own reference
                                      // was not independently checked here. CARRIER_HZ is a whole-Hz
                                      // integer, so +2 (not +2.1) is the closest step, leaving about
                                      // 0.1Hz of residual. NOT re-measured after this change.

// Inits the AD9851 SPI driver and sets the initial carrier frequency
// (CARRIER_HZ). Call once from setup().
void carrier_output_init(void);

// tx_freq = carrier + (int32_t)delayed_freq_dev_hz, sent to the chip via
// ad9851_set_frequency(). Returns the exact integer Hz value sent, so the
// caller can feed it straight to diagnostics_set_tx_info() - ground truth
// for what the chip is actually asked to produce, independent of any of
// the upstream DSP/delay-line reasoning. Call once per dsp_task tick.
uint32_t IRAM_ATTR carrier_output_set_freq_dev(float delayed_freq_dev_hz);

// 2026-09-19: split OUT of carrier_output_set_freq_dev() above, folded in
// from minimal_fs_test_step6_isr_write after real-hardware bench
// confirmation - see config.h's AD9851_ISR_WRITE_ENABLED comment and
// null_bias_investigation.md/moving_forward_notes.md's 2026-09-19 entries
// for the full write-time-jitter story. carrier_output_set_freq_dev() does
// `s_carrier_hz + (int32_t)delayed_freq_dev_hz` - a float-to-int cast,
// which uses Xtensa's floating-point coprocessor. Calling it directly from
// an ISR panics with a Coprocessor exception (confirmed on real hardware,
// EXCCAUSE 0x4, during this fix's own bench validation) - this project's
// history had already named this exact failure mode ("v2's ISR crash", see
// group_delay_fit_notes.md) before step6 first repeated it. The fix: do
// the float arithmetic in TASK context (safe - FPU use is normal there)
// via carrier_output_compute_tx_freq(), and pass the ALREADY-INTEGER
// result to carrier_output_set_freq_dev_raw() - pure register writes
// underneath, no float, no coprocessor - which IS safe to call from an
// ISR (dsp_task's own on_timer_alarm() now does exactly this when
// AD9851_ISR_WRITE_ENABLED). carrier_output_set_freq_dev() above is
// unchanged and still does both steps together, for every other caller.
uint32_t IRAM_ATTR carrier_output_compute_tx_freq(float delayed_freq_dev_hz);
void IRAM_ATTR carrier_output_set_freq_dev_raw(uint32_t tx_freq);

bool carrier_output_get_rf_enabled(void);
void carrier_output_set_rf_enabled(bool enable);

// Thin wrapper over ad9851_get_profile() - see ad9851_profile_t (AD9851.h)
// for what max_prep_us/max_spi_us each cover. Exposed here so
// diagnostics.cpp doesn't need its own direct handle to the AD9851
// module, matching how everything else in [timing] is reached.
void carrier_output_get_profile(ad9851_profile_t *out);

// 2026-09-09: canary check for the random-TX-jump investigation (see
// moving_forward_notes.md). s_carrier_hz (this module's static, written
// once in carrier_output_init() and never touched again by any command or
// preset) is the other half of the two persistent, write-once values that
// theory points at, alongside ad9851_canary_t's ftw_reciprocal - returns
// the live value so the caller can compare it against the compile-time
// CARRIER_HZ constant itself (immune to RAM corruption, unlike a second
// shadow variable would be).
uint32_t carrier_output_get_carrier_hz(void);

// Thin wrapper over ad9851_get_canary() - see its doc comment (AD9851.h).
void carrier_output_get_canary(ad9851_canary_t *out);

#endif // AD9851_ATTACHED
