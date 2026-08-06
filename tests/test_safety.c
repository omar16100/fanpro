/*
 * Safety gate tests.
 *
 * This is the only thing standing between a bug anywhere else in fanpro and
 * a fan pinned at 0 RPM under load, so the tests are written as claims about
 * hardware outcomes rather than about return values.
 */
#include "tinytest.h"

#include "fanpro/safety.h"

#include <math.h>
#include <string.h>

static fanpro_fan_t
make_fan(void)
{
	fanpro_fan_t f;

	memset(&f, 0, sizeof(f));
	f.index = 0;
	f.min_rpm = 1000.0; /* this Mac Studio's real limits */
	f.max_rpm = 3625.0;
	f.mode_key_known = true;
	f.mode_readable = true;
	return f;
}

static void
add(fanpro_sensor_set_t *set, const char *name, double c)
{
	fanpro_sensors_add(set, name, c, FANPRO_SRC_HID);
}

TT_TEST(safety_passes_a_reasonable_target)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 55.0);

	r = fanpro_safety_check(&cfg, &fan, 2000.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_OK);
	TT_NEAR(r.rpm, 2000.0, 0.001);
}

TT_TEST(safety_clamps_to_the_fans_own_limits)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 55.0);

	r = fanpro_safety_check(&cfg, &fan, 99999.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_CLAMPED);
	TT_NEAR(r.rpm, 3625.0, 0.001);

	r = fanpro_safety_check(&cfg, &fan, 200.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_CLAMPED);
	TT_NEAR(r.rpm, 1000.0, 0.001);
}

TT_TEST(safety_never_stops_a_fan_unless_explicitly_allowed)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 55.0);

	/* A malformed curve producing 0 must never reach the hardware as a
	 * stopped fan.  The firmware permits it; fanpro does not, by default. */
	r = fanpro_safety_check(&cfg, &fan, 0.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_NEAR(r.rpm, 1000.0, 0.001);
	TT_ASSERT(r.rpm >= fan.min_rpm);

	r = fanpro_safety_check(&cfg, &fan, -500.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_NEAR(r.rpm, 1000.0, 0.001);

	/* Opting in is a deliberate act. */
	cfg.allow_fan_stop = true;
	r = fanpro_safety_check(&cfg, &fan, 0.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_NEAR(r.rpm, 0.0, 0.001);
}

TT_TEST(safety_panics_per_class_not_globally)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);

	/* 85 C is routine on SoC die and must not panic. */
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 85.0);
	r = fanpro_safety_check(&cfg, &fan, 2000.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_ASSERT(r.verdict != FANPRO_SAFETY_PANIC);

	/* The same 85 C on NAND is past its limit and must panic.  A single
	 * global threshold cannot express both of these. */
	memset(&set, 0, sizeof(set));
	add(&set, "NAND CH0 temp", 85.0);
	r = fanpro_safety_check(&cfg, &fan, 2000.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_PANIC);
	TT_EQ_INT(r.panic_class, FANPRO_CLASS_NAND);
	TT_NEAR(r.rpm, 3625.0, 0.001);
}

TT_TEST(safety_ignores_unvalidated_other_class_sensors)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 50.0);
	/* This machine really does report an opaque SMC key at 96 C while every
	 * die sensor reads 49 C.  Panicking on it would mean permanently
	 * full-speed fans. */
	add(&set, "Tf76", 96.5);

	r = fanpro_safety_check(&cfg, &fan, 1500.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_OK);
}

TT_TEST(safety_panic_beats_a_high_target)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 99.0);

	/* Even when the curve asks for near-idle, panic overrides it. */
	r = fanpro_safety_check(&cfg, &fan, 1000.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_PANIC);
	TT_NEAR(r.rpm, fan.max_rpm, 0.001);
	TT_EQ_INT(r.panic_class, FANPRO_CLASS_SOC);
	TT_NEAR(r.panic_temp, 99.0, 0.001);
}

TT_TEST(safety_uses_thermal_pressure_as_an_independent_signal)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 50.0); /* our sensors say everything is fine */

	/* macOS disagrees.  Trusting only our own sampling would miss this. */
	r = fanpro_safety_check(&cfg, &fan, 1200.0, &set, 0, FANPRO_THERMAL_TRAPPING);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_PANIC);
	TT_EQ_STR(r.reason, "thermal_pressure");

	/* Moderate pressure is normal under load and must not panic. */
	r = fanpro_safety_check(&cfg, &fan, 1200.0, &set, 0, FANPRO_THERMAL_MODERATE);
	TT_ASSERT(r.verdict != FANPRO_SAFETY_PANIC);

	/* An unknown level must not be read as level 0 nor as a panic. */
	r = fanpro_safety_check(&cfg, &fan, 1200.0, &set, 0, FANPRO_THERMAL_UNKNOWN);
	TT_ASSERT(r.verdict != FANPRO_SAFETY_PANIC);
}

TT_TEST(safety_releases_when_samples_go_stale)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	cfg.max_sample_failures = 3;
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 50.0);

	r = fanpro_safety_check(&cfg, &fan, 2000.0, &set, 2, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_OK);

	/* Past the limit, hand control back.  The firmware can still see the
	 * temperatures we have lost sight of; holding a stale target cannot. */
	r = fanpro_safety_check(&cfg, &fan, 2000.0, &set, 3, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_RELEASE);
	TT_EQ_STR(r.reason, "sensor_samples_stale");
	TT_TRUE(isnan(r.rpm));
}

TT_TEST(safety_panic_outranks_staleness)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 99.0);

	/* A slightly stale reading of 99 C is still worth acting on. */
	r = fanpro_safety_check(&cfg, &fan, 2000.0, &set, 10, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_PANIC);
}

TT_TEST(safety_releases_without_a_curve_opinion)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 50.0);

	/* NAN means "no opinion".  Substituting any number here would be the
	 * most dangerous thing the gate could do. */
	r = fanpro_safety_check(&cfg, &fan, NAN, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_RELEASE);
	TT_EQ_STR(r.reason, "no_curve_opinion");
}

TT_TEST(safety_releases_with_no_sensors_at_all)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&set, 0, sizeof(set));

	r = fanpro_safety_check(&cfg, &fan, 2000.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_RELEASE);
	TT_EQ_STR(r.reason, "no_sensors");
}

TT_TEST(safety_refuses_a_fan_with_unknown_limits)
{
	fanpro_safety_config_t cfg;
	fanpro_sensor_set_t set;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);
	memset(&set, 0, sizeof(set));
	add(&set, "PMU tdie1", 50.0);

	/* fan.c sets these to -1 when the limit keys could not be read. */
	fan.min_rpm = -1.0;
	r = fanpro_safety_check(&cfg, &fan, 2000.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_RELEASE);
	TT_EQ_STR(r.reason, "fan_limits_unknown");

	fan = make_fan();
	fan.max_rpm = NAN;
	r = fanpro_safety_check(&cfg, &fan, 2000.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_RELEASE);

	/* Inverted limits are corrupt data, not a narrow range. */
	fan = make_fan();
	fan.min_rpm = 4000.0;
	fan.max_rpm = 1000.0;
	r = fanpro_safety_check(&cfg, &fan, 2000.0, &set, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_RELEASE);
}

TT_TEST(safety_null_inputs_release)
{
	fanpro_safety_config_t cfg;
	fanpro_fan_t fan = make_fan();
	fanpro_safety_result_t r;

	fanpro_safety_defaults(&cfg);

	r = fanpro_safety_check(NULL, &fan, 2000.0, NULL, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_RELEASE);

	r = fanpro_safety_check(&cfg, NULL, 2000.0, NULL, 0, FANPRO_THERMAL_NOMINAL);
	TT_EQ_INT(r.verdict, FANPRO_SAFETY_RELEASE);
}
