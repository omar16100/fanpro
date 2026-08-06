/*
 * Curve tests.
 *
 * The shaping rules are asymmetric on purpose, and it would be easy to
 * "simplify" them into symmetry later, so each asymmetry is pinned here with
 * the reason it exists.
 */
#include "tinytest.h"

#include "fanpro/curve.h"

#include <math.h>
#include <string.h>

static void
make_curve(fanpro_curve_t *c)
{
	fanpro_curve_defaults(c);
	snprintf(c->name, sizeof(c->name), "test");
	c->n_points = 3;
	c->points[0].celsius = 45.0; c->points[0].rpm = 800.0;
	c->points[1].celsius = 70.0; c->points[1].rpm = 2000.0;
	c->points[2].celsius = 85.0; c->points[2].rpm_is_max = true;
}

TT_TEST(curve_interpolates_at_and_between_points)
{
	fanpro_curve_t c;

	make_curve(&c);

	TT_NEAR(fanpro_curve_interpolate(&c, 45.0, 5000.0), 800.0, 0.001);
	TT_NEAR(fanpro_curve_interpolate(&c, 70.0, 5000.0), 2000.0, 0.001);

	/* Midpoint of the first segment. */
	TT_NEAR(fanpro_curve_interpolate(&c, 57.5, 5000.0), 1400.0, 0.001);
	/* Quarter into the second segment: 2000 + 0.25 * (5000 - 2000). */
	TT_NEAR(fanpro_curve_interpolate(&c, 73.75, 5000.0), 2750.0, 0.001);
}

TT_TEST(curve_resolves_max_against_the_actual_fan)
{
	fanpro_curve_t c;

	make_curve(&c);

	/* "max" must mean this fan's maximum, not a number baked into config. */
	TT_NEAR(fanpro_curve_interpolate(&c, 85.0, 3625.0), 3625.0, 0.001);
	TT_NEAR(fanpro_curve_interpolate(&c, 85.0, 5000.0), 5000.0, 0.001);
}

TT_TEST(curve_holds_flat_outside_its_range)
{
	fanpro_curve_t c;

	make_curve(&c);

	/* Below the first point and above the last, hold rather than
	 * extrapolate: a curve should not invent speeds it was never given. */
	TT_NEAR(fanpro_curve_interpolate(&c, 10.0, 5000.0), 800.0, 0.001);
	TT_NEAR(fanpro_curve_interpolate(&c, -5.0, 5000.0), 800.0, 0.001);
	TT_NEAR(fanpro_curve_interpolate(&c, 120.0, 5000.0), 5000.0, 0.001);
}

TT_TEST(curve_rejects_unusable_input)
{
	fanpro_curve_t c;

	make_curve(&c);

	TT_TRUE(isnan(fanpro_curve_interpolate(&c, NAN, 5000.0)));
	TT_TRUE(isnan(fanpro_curve_interpolate(&c, 1.0 / 0.0, 5000.0)));
	TT_TRUE(isnan(fanpro_curve_interpolate(NULL, 50.0, 5000.0)));

	c.n_points = 0;
	TT_TRUE(isnan(fanpro_curve_interpolate(&c, 50.0, 5000.0)));
}

TT_TEST(curve_first_evaluation_is_not_slew_limited)
{
	fanpro_curve_t c;
	fanpro_curve_state_t st;

	make_curve(&c);
	memset(&st, 0, sizeof(st));

	/* Starting cold, the first answer must be the curve's answer.  Ramping
	 * up from an imaginary zero would take many ticks to reach the right
	 * speed on a machine that is already hot at startup. */
	TT_NEAR(fanpro_curve_eval(&c, &st, 70.0, 5000.0), 2000.0, 0.001);
}

TT_TEST(curve_hysteresis_blocks_small_dips_only)
{
	fanpro_curve_t c;
	fanpro_curve_state_t st;
	double rpm;

	make_curve(&c);
	c.hysteresis_c = 2.0;
	c.slew_up_rpm_per_tick = 0.0;   /* isolate hysteresis */
	c.slew_down_rpm_per_tick = 0.0;
	memset(&st, 0, sizeof(st));

	fanpro_curve_eval(&c, &st, 60.0, 5000.0);
	rpm = st.last_rpm;

	/* A 1 C dip is inside the band: hold, or the fan hunts at the knee. */
	TT_NEAR(fanpro_curve_eval(&c, &st, 59.0, 5000.0), rpm, 0.001);
	/* A 3 C dip is outside it: respond. */
	TT_ASSERT(fanpro_curve_eval(&c, &st, 57.0, 5000.0) < rpm);
}

TT_TEST(curve_hysteresis_never_delays_a_rise)
{
	fanpro_curve_t c;
	fanpro_curve_state_t st;
	double before, after;

	make_curve(&c);
	c.hysteresis_c = 10.0; /* deliberately huge */
	c.slew_up_rpm_per_tick = 0.0;
	memset(&st, 0, sizeof(st));

	fanpro_curve_eval(&c, &st, 60.0, 5000.0);
	before = st.last_rpm;

	/* Even a 1 C rise must produce more airflow immediately.  Applying
	 * hysteresis upward would delay the response to heat, which is the one
	 * direction where delay is dangerous. */
	after = fanpro_curve_eval(&c, &st, 61.0, 5000.0);
	TT_ASSERT(after > before);
}

TT_TEST(curve_slew_is_asymmetric)
{
	fanpro_curve_t c;
	fanpro_curve_state_t st;
	double rpm;

	make_curve(&c);
	c.hysteresis_c = 0.0;
	c.slew_up_rpm_per_tick = 400.0;
	c.slew_down_rpm_per_tick = 100.0;
	c.spike_temp_c = 0.0; /* disable the bypass for this test */
	memset(&st, 0, sizeof(st));

	fanpro_curve_eval(&c, &st, 45.0, 5000.0); /* 800 rpm */

	/* A big jump up is capped at the upward limit. */
	rpm = fanpro_curve_eval(&c, &st, 70.0, 5000.0);
	TT_NEAR(rpm, 1200.0, 0.001);

	/* Coming back down is capped much harder, so the machine does not
	 * audibly pulse. */
	rpm = fanpro_curve_eval(&c, &st, 45.0, 5000.0);
	TT_NEAR(rpm, 1100.0, 0.001);
}

TT_TEST(curve_spike_threshold_bypasses_the_upward_limit)
{
	fanpro_curve_t c;
	fanpro_curve_state_t st;

	make_curve(&c);
	c.hysteresis_c = 0.0;
	c.slew_up_rpm_per_tick = 100.0; /* very restrictive */
	c.spike_temp_c = 85.0;
	memset(&st, 0, sizeof(st));

	fanpro_curve_eval(&c, &st, 45.0, 5000.0); /* 800 rpm */

	/* At the spike threshold the limiter must not apply.  Creeping up 100
	 * rpm per second from 800 to 5000 would take 42 seconds, which is not
	 * a response to a thermal event. */
	TT_NEAR(fanpro_curve_eval(&c, &st, 85.0, 5000.0), 5000.0, 0.001);
}

TT_TEST(curve_input_selection)
{
	fanpro_curve_t c;
	fanpro_sensor_set_t set;

	memset(&set, 0, sizeof(set));
	fanpro_sensors_add(&set, "PMU tdie1", 48.0, FANPRO_SRC_HID);
	fanpro_sensors_add(&set, "PMU tdie2", 54.0, FANPRO_SRC_HID);
	fanpro_sensors_add(&set, "NAND CH0 temp", 42.0, FANPRO_SRC_HID);

	fanpro_curve_defaults(&c);
	c.use_class = true;
	c.cls = FANPRO_CLASS_SOC;
	/* A class is always judged by its hottest member. */
	TT_NEAR(fanpro_curve_input(&c, &set), 54.0, 0.001);

	c.cls = FANPRO_CLASS_NAND;
	TT_NEAR(fanpro_curve_input(&c, &set), 42.0, 0.001);

	c.use_class = false;
	snprintf(c.match, sizeof(c.match), "tdie");
	c.agg = FANPRO_AGG_MAX;
	TT_NEAR(fanpro_curve_input(&c, &set), 54.0, 0.001);

	c.agg = FANPRO_AGG_AVG;
	TT_NEAR(fanpro_curve_input(&c, &set), 51.0, 0.001);
}

TT_TEST(curve_input_is_nan_when_nothing_matches)
{
	fanpro_curve_t c;
	fanpro_sensor_set_t set;

	memset(&set, 0, sizeof(set));
	fanpro_sensors_add(&set, "PMU tdie1", 48.0, FANPRO_SRC_HID);

	fanpro_curve_defaults(&c);
	c.use_class = false;
	snprintf(c.match, sizeof(c.match), "nonexistent");

	/* NAN, not 0.  A curve fed 0 C would idle the fans on a hot machine. */
	TT_TRUE(isnan(fanpro_curve_input(&c, &set)));

	c.use_class = true;
	c.cls = FANPRO_CLASS_GPU;
	TT_TRUE(isnan(fanpro_curve_input(&c, &set)));
}

TT_TEST(curve_eval_propagates_nan)
{
	fanpro_curve_t c;
	fanpro_curve_state_t st;

	make_curve(&c);
	memset(&st, 0, sizeof(st));

	/* A missing reading must stay missing all the way to the caller, which
	 * treats it as a release condition. */
	TT_TRUE(isnan(fanpro_curve_eval(&c, &st, NAN, 5000.0)));
	TT_FALSE(st.primed);
}
