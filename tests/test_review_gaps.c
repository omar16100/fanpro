/*
 * Regression tests for the defects found in review.
 *
 * Each of these would have failed against the code as originally written.
 * They are kept together so the specific holes stay pinned: most are
 * "hardware pinned, bookkeeping clean" cases, which are silent by nature and
 * would otherwise only show up as a hot machine.
 */
#include "tinytest.h"

#include "fanpro/curve.h"
#include "fanpro/power.h"
#include "fanpro/safety.h"
#include "fanpro/smc_codec.h"
#include "fanpro/smc_fake.h"
#include "fanpro/unlock.h"

#include <math.h>
#include <string.h>

/* ---- shared rig --------------------------------------------------------- */

static uint64_t
gap_now(void *ctx)
{
	return fanpro_fake_now_ms((const fanpro_fake_t *)ctx);
}

static void
gap_sleep(void *ctx, unsigned ms)
{
	fanpro_fake_advance_ms((fanpro_fake_t *)ctx, ms);
}

typedef struct {
	fanpro_fake_t   *fake;
	fanpro_smc_t     smc;
	fanpro_fan_set_t fans;
	fanpro_unlock_t  unlock;
	fanpro_clock_t   clock;
} rig_t;

static void
rig_up(rig_t *r, fanpro_fake_personality_t p, int fans, bool lowercase)
{
	memset(r, 0, sizeof(*r));
	r->fake = fanpro_fake_create(p);
	fanpro_fake_add_fans(r->fake, fans, 1000.0, 3625.0, lowercase);
	fanpro_smc_open(&r->smc, fanpro_fake_backend(r->fake));
	fanpro_fan_enumerate(&r->smc, &r->fans);
	r->clock.now_ms = gap_now;
	r->clock.sleep_ms = gap_sleep;
	r->clock.ctx = r->fake;
	fanpro_unlock_init(&r->unlock, &r->smc, &r->fans, &r->clock);
}

static void
rig_down(rig_t *r)
{
	fanpro_smc_close(&r->smc);
	fanpro_fake_destroy(r->fake);
}

static double
peek(rig_t *r, const char *key)
{
	double v = -1.0;

	fanpro_fake_peek(r->fake, key, &v);
	return v;
}

/* ---- safety ------------------------------------------------------------- */

TT_TEST(gap_safety_null_sensor_set_releases)
{
	fanpro_safety_config_t cfg;
	fanpro_fan_t fan;
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&fan, 0, sizeof(fan));
	fan.min_rpm = 1000.0;
	fan.max_rpm = 3625.0;
	fan.mode_key_known = true;

	/* A NULL sensor set means we are blind, exactly as an empty one does.
	 * Passing it through as OK would drive fans with no data behind them. */
	r = fanpro_safety_check(&cfg, &fan, 2000.0, NULL, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_RELEASE);
	TT_EQ_STR(r.reason, "no_sensors");
}

/* ---- unlock: hardware pinned with clean bookkeeping ---------------------- */

TT_TEST(gap_acquire_undoes_a_write_it_could_not_verify)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 1, false);

	/*
	 * The mode write lands, then the verifying read fails.  Without a
	 * cleanup write the fan stays manual while we record holding nothing,
	 * and on a direct-path machine nothing ever reclaims it.
	 */
	fanpro_fake_force_result(r.fake, "F0Md", FANPRO_SMC_NOT_READABLE);
	TT_ASSERT(fanpro_unlock_acquire(&r.unlock, 0) < 0);
	fanpro_fake_clear_result(r.fake, "F0Md");

	TT_EQ_INT(r.unlock.manual_count, 0);
	TT_NEAR(peek(&r, "F0Md"), 0.0, 0.0); /* forced back to auto */

	rig_down(&r);
}

TT_TEST(gap_release_keeps_ownership_when_the_write_fails)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 1, false);
	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 0), 0);
	TT_EQ_INT(r.unlock.manual_count, 1);

	/* Every release write fails.  Dropping the bookkeeping here would mean
	 * a pinned fan nobody believes they own; keeping it lets a later pass
	 * try again and keeps the drift detector honest. */
	fanpro_fake_force_result(r.fake, "F0Md", FANPRO_SMC_NOT_WRITABLE);
	TT_ASSERT(fanpro_unlock_release(&r.unlock, 0) < 0);
	TT_EQ_INT(r.unlock.manual_count, 1);
	TT_TRUE(r.unlock.manual[0]);

	/* Once the hardware cooperates the retry succeeds and ownership goes. */
	fanpro_fake_clear_result(r.fake, "F0Md");
	TT_EQ_INT(fanpro_unlock_release(&r.unlock, 0), 0);
	TT_EQ_INT(r.unlock.manual_count, 0);
	TT_NEAR(peek(&r, "F0Md"), 0.0, 0.0);

	rig_down(&r);
}

TT_TEST(gap_release_all_ignores_a_transient_mode_read_failure)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 2, false);

	/* A previous process pinned fan 1. */
	fanpro_fake_poke(r.fake, "F1Md", 1.0);

	/*
	 * A refresh during which the mode read fails must not convince the
	 * release path that this fan has no mode key.  Key existence is
	 * latched at enumeration precisely so a momentary read failure cannot
	 * cause a pinned fan to be skipped at shutdown.
	 */
	fanpro_fake_force_result(r.fake, "F1Md", FANPRO_SMC_NOT_READABLE);
	fanpro_fan_refresh(&r.smc, &r.fans);
	TT_FALSE(r.fans.fans[1].mode_readable);
	TT_TRUE(r.fans.fans[1].mode_key_known);

	fanpro_fake_clear_result(r.fake, "F1Md");
	TT_EQ_INT(fanpro_unlock_release_all(&r.unlock), 0);
	TT_NEAR(peek(&r, "F1Md"), 0.0, 0.0);

	rig_down(&r);
}

TT_TEST(gap_failed_second_acquire_keeps_ftst_for_the_first_fan)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_NEEDS_FTST, 2, false);

	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 0), 0);
	TT_TRUE(r.unlock.ftst_asserted);

	/* Fan 1's acquire fails.  Tearing down Ftst here would silently end
	 * fan 0's control, which is the whole point of the refcount. */
	fanpro_fake_force_result(r.fake, "F1Md", FANPRO_SMC_NOT_WRITABLE);
	TT_ASSERT(fanpro_unlock_acquire(&r.unlock, 1) < 0);

	TT_TRUE(r.unlock.ftst_asserted);
	TT_NEAR(peek(&r, "Ftst"), 1.0, 0.0);
	TT_EQ_INT(r.unlock.manual_count, 1);
	TT_TRUE(r.unlock.manual[0]);

	rig_down(&r);
}

TT_TEST(gap_recover_refuses_to_run_while_we_hold_fans)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 2, false);
	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 0), 0);

	/*
	 * recover() cannot distinguish "left by a dead process" from "held by
	 * this one", so calling it mid-run would rip control from the live
	 * control loop.  It must refuse rather than guess.
	 */
	TT_ASSERT(fanpro_unlock_recover(&r.unlock) < 0);
	TT_EQ_INT(r.unlock.manual_count, 1);
	TT_NEAR(peek(&r, "F0Md"), 1.0, 0.0);

	rig_down(&r);
}

TT_TEST(gap_ftst_fallback_writes_the_mode_exactly_twice)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_NEEDS_FTST, 1, false);
	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 0), 0);

	/* Exactly two: the rejected direct attempt, then the one that landed
	 * after the yield.  An "at least one" assertion would also pass a
	 * build that never retried, which is the bug worth catching. */
	TT_EQ_UINT(fanpro_fake_write_count(r.fake, "F0Md"), 2);

	rig_down(&r);
}

/* ---- codec -------------------------------------------------------------- */

TT_TEST(gap_codec_sp78_encodes_negative)
{
	uint8_t b[2] = { 0 };

	/* Decoding negatives was tested; encoding is the direction that
	 * actually reaches hardware. */
	TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_SP78, -1.0, b, 2));
	TT_EQ_UINT(b[0], 0xff);
	TT_EQ_UINT(b[1], 0x00);

	TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_SP78, -40.5, b, 2));
	{
		double back = 0.0;

		TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_SP78, b, 2, &back));
		TT_NEAR(back, -40.5, 0.001);
	}
}

TT_TEST(gap_codec_fpe2_boundary)
{
	uint8_t b[2] = { 0 };

	/* 14.2 unsigned tops out at exactly 16383.75. */
	TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_FPE2, 16383.75, b, 2));
	TT_EQ_UINT(b[0], 0xff);
	TT_EQ_UINT(b[1], 0xff);
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FPE2, 16384.0, b, 2));
}

TT_TEST(gap_codec_flt_refuses_values_that_narrow_to_infinity)
{
	uint8_t b[4] = { 0 };

	/* 1e300 is a finite double but overflows a float.  Checking only the
	 * double would send +Inf to the hardware as a fan target. */
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FLT, 1e300, b, 4));
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_FLT, -1e300, b, 4));
	TT_TRUE(fanpro_smc_encode(FANPRO_TYPE_FLT, 3.0e38, b, 4));
}

TT_TEST(gap_codec_validates_integer_width)
{
	const uint8_t two[2] = { 0x01, 0x00 };
	uint8_t out[4] = { 0 };
	double v = 0.0;

	/* data_size comes from the firmware.  If it ever disagreed with the
	 * type, decoding a ui32 as a ui16 would yield a plausible wrong
	 * number rather than an error. */
	TT_FALSE(fanpro_smc_decode(FANPRO_TYPE_UI32, two, 2, &v));
	TT_FALSE(fanpro_smc_decode(FANPRO_TYPE_UI8, two, 2, &v));
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_UI32, 1.0, out, 2));
	TT_FALSE(fanpro_smc_encode(FANPRO_TYPE_UI8, 1.0, out, 2));

	/* Matching widths still work. */
	TT_TRUE(fanpro_smc_decode(FANPRO_TYPE_UI16, two, 2, &v));
	TT_NEAR(v, 256.0, 0.0);
}

/* ---- curve -------------------------------------------------------------- */

TT_TEST(gap_curve_refuses_an_overlong_point_list)
{
	fanpro_curve_t c;

	fanpro_curve_defaults(&c);
	c.n_points = FANPRO_CURVE_MAX_POINTS + 1; /* malformed struct */

	/* Reading past the array would be undefined behaviour in the one
	 * function documented as the trustworthy core. */
	TT_TRUE(isnan(fanpro_curve_interpolate(&c, 50.0, 5000.0)));
}

TT_TEST(gap_curve_degenerate_span_behaves_as_a_step)
{
	fanpro_curve_t c;

	fanpro_curve_defaults(&c);
	c.n_points = 3;
	c.points[0].celsius = 40.0; c.points[0].rpm = 800.0;
	/* Two points at the same temperature: a step, not a division by zero. */
	c.points[1].celsius = 60.0; c.points[1].rpm = 1500.0;
	c.points[2].celsius = 60.0; c.points[2].rpm = 3000.0;

	TT_NEAR(fanpro_curve_interpolate(&c, 60.0, 5000.0), 1500.0, 0.001);
	TT_NEAR(fanpro_curve_interpolate(&c, 70.0, 5000.0), 3000.0, 0.001);
	TT_TRUE(isfinite(fanpro_curve_interpolate(&c, 60.0, 5000.0)));
}

/* ---- fan enumeration ---------------------------------------------------- */

TT_TEST(gap_fan_zero_fans_is_success_not_failure)
{
	fanpro_fake_t *f = fanpro_fake_create(FANPRO_FAKE_DIRECT_OK);
	fanpro_smc_t smc;
	fanpro_fan_set_t set;

	fanpro_fake_add_fans(f, 0, 0.0, 0.0, false);
	fanpro_smc_open(&smc, fanpro_fake_backend(f));

	/* Fanless Macs exist; reporting zero fans is a fact, not an error. */
	TT_EQ_INT(fanpro_fan_enumerate(&smc, &set), 0);
	TT_EQ_INT(set.count, 0);

	fanpro_smc_close(&smc);
	fanpro_fake_destroy(f);
}

TT_TEST(gap_fan_count_is_clamped_to_the_array)
{
	fanpro_fake_t *f = fanpro_fake_create(FANPRO_FAKE_DIRECT_OK);
	fanpro_smc_t smc;
	fanpro_fan_set_t set;

	fanpro_fake_add_fans(f, 2, 1000.0, 3625.0, false);
	/* Firmware claims more fans than we have room for. */
	fanpro_fake_poke(f, "FNum", 99.0);
	fanpro_smc_open(&smc, fanpro_fake_backend(f));

	TT_EQ_INT(fanpro_fan_enumerate(&smc, &set), 0);
	TT_EQ_INT(set.count, FANPRO_MAX_FANS);

	fanpro_smc_close(&smc);
	fanpro_fake_destroy(f);
}

TT_TEST(gap_fan_without_a_mode_key_is_readable_but_not_controllable)
{
	fanpro_fake_t *f = fanpro_fake_create(FANPRO_FAKE_DIRECT_OK);
	fanpro_smc_t smc;
	fanpro_fan_set_t set;
	fanpro_unlock_t u;

	fanpro_fake_add_fans(f, 1, 1000.0, 3625.0, false);
	fanpro_fake_remove_key(f, "F0Md");
	fanpro_smc_open(&smc, fanpro_fake_backend(f));

	/* Monitoring must still work on a machine with no controllable fans. */
	TT_EQ_INT(fanpro_fan_enumerate(&smc, &set), 0);
	TT_EQ_INT(set.count, 1);
	TT_FALSE(set.fans[0].mode_key_known);
	TT_NEAR(set.fans[0].min_rpm, 1000.0, 0.001);

	fanpro_unlock_init(&u, &smc, &set, NULL);
	TT_ASSERT(fanpro_unlock_acquire(&u, 0) < 0);

	fanpro_smc_close(&smc);
	fanpro_fake_destroy(f);
}

/* ---- thermal pressure scale --------------------------------------------- */

TT_TEST(gap_thermal_pressure_normalises_both_scales)
{
	/*
	 * macOS 26.3 reports the compact enum; this machine returned a raw 2
	 * under load, which the original 0/10/20/30/40 assumption mapped to
	 * "unknown". That silently removed thermal pressure from the safety
	 * gate at exactly the moment it mattered.
	 */
	TT_EQ_INT(fanpro_thermal_normalise(0), FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(fanpro_thermal_normalise(1), FANPRO_THERMAL_LIGHT);
	TT_EQ_INT(fanpro_thermal_normalise(2), FANPRO_THERMAL_MODERATE);
	TT_EQ_INT(fanpro_thermal_normalise(3), FANPRO_THERMAL_HEAVY);
	TT_EQ_INT(fanpro_thermal_normalise(4), FANPRO_THERMAL_TRAPPING);
	TT_EQ_INT(fanpro_thermal_normalise(5), FANPRO_THERMAL_SLEEPING);

	/* Legacy scale still works. */
	TT_EQ_INT(fanpro_thermal_normalise(10), FANPRO_THERMAL_MODERATE);
	TT_EQ_INT(fanpro_thermal_normalise(20), FANPRO_THERMAL_HEAVY);
	TT_EQ_INT(fanpro_thermal_normalise(30), FANPRO_THERMAL_TRAPPING);
	TT_EQ_INT(fanpro_thermal_normalise(40), FANPRO_THERMAL_SLEEPING);

	/* Never guess: a wrong guess either panics constantly or never. */
	TT_EQ_INT(fanpro_thermal_normalise(7), FANPRO_THERMAL_UNKNOWN);
	TT_EQ_INT(fanpro_thermal_normalise(999), FANPRO_THERMAL_UNKNOWN);
}

TT_TEST(gap_thermal_heavy_still_trips_the_panic_threshold)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan;
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&fan, 0, sizeof(fan));
	fan.min_rpm = 1000.0;
	fan.max_rpm = 3625.0;
	fan.mode_key_known = true;
	memset(&set, 0, sizeof(set));
	fanpro_sensors_add(&set, "PMU tdie1", 50.0, FANPRO_SRC_HID);

	/* A raw 3 from the modern scale is "heavy" and must reach the gate as
	 * a real level, not as unknown. */
	r = fanpro_safety_check(&cfg, &fan, 1200.0, &set, 0,
	                        fanpro_thermal_normalise(3));
	TT_ASSERT(r.verdict != FANPRO_SAFETY_PANIC); /* heavy < trapping */

	r = fanpro_safety_check(&cfg, &fan, 1200.0, &set, 0,
	                        fanpro_thermal_normalise(4));
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_PANIC);
	TT_EQ_STR(r.reason, "thermal_pressure");
}
