/*
 * fanpro - curve evaluation.
 *
 * Deliberately pure.  Everything that could make fan behaviour hard to
 * reason about (time, hardware, config parsing) lives elsewhere, so a
 * surprising fan speed can always be reproduced from a temperature and a
 * previous state.
 */
#include "fanpro/curve.h"

#include <math.h>
#include <string.h>

void
fanpro_curve_defaults(fanpro_curve_t *c)
{
	if (c == NULL)
		return;

	memset(c, 0, sizeof(*c));
	c->agg = FANPRO_AGG_MAX;
	c->cls = FANPRO_CLASS_SOC;
	c->use_class = true;

	/* 2 C of hysteresis is enough to stop knee hunting without making the
	 * fan feel unresponsive. */
	c->hysteresis_c = 2.0;

	/* Asymmetric on purpose.  Down is slow so the machine does not
	 * audibly pulse; up is fast because that is the direction that
	 * protects the hardware. */
	c->slew_up_rpm_per_tick = 400.0;
	c->slew_down_rpm_per_tick = 100.0;

	/* Above this the upward limiter is bypassed entirely. */
	c->spike_temp_c = 85.0;
}

double
fanpro_curve_interpolate(const fanpro_curve_t *c, double celsius,
                         double fan_max_rpm)
{
	int i;

	/* Bound the upper end too: this function is meant to be the part you
	 * can trust, so it must not read past the array on a malformed struct. */
	if (c == NULL || c->n_points <= 0 ||
	    c->n_points > FANPRO_CURVE_MAX_POINTS || !isfinite(celsius))
		return NAN;

	/* Resolve a point's RPM, expanding a "max" entry against this fan. */
	#define POINT_RPM(p) ((p).rpm_is_max ? fan_max_rpm : (p).rpm)

	if (celsius <= c->points[0].celsius)
		return POINT_RPM(c->points[0]);

	for (i = 1; i < c->n_points; i++) {
		const fanpro_curve_point_t *lo = &c->points[i - 1];
		const fanpro_curve_point_t *hi = &c->points[i];
		double span, frac;

		if (celsius > hi->celsius)
			continue;

		span = hi->celsius - lo->celsius;
		if (span <= 0.0)
			return POINT_RPM(*hi); /* degenerate: treat as a step */

		frac = (celsius - lo->celsius) / span;
		return POINT_RPM(*lo) + frac * (POINT_RPM(*hi) - POINT_RPM(*lo));
	}

	/* Past the last point.  Hold the last RPM rather than extrapolate: a
	 * curve should not invent speeds outside the range it was given. */
	return POINT_RPM(c->points[c->n_points - 1]);

	#undef POINT_RPM
}

double
fanpro_curve_eval(const fanpro_curve_t *c, fanpro_curve_state_t *state,
                  double celsius, double fan_max_rpm)
{
	double want, out, delta;

	if (c == NULL || state == NULL)
		return NAN;

	want = fanpro_curve_interpolate(c, celsius, fan_max_rpm);
	if (!isfinite(want))
		return NAN;

	if (!state->primed) {
		state->primed = true;
		state->last_rpm = want;
		state->last_temp = celsius;
		return want;
	}

	/*
	 * Hysteresis, downward only.  Reducing fan speed on a small dip means
	 * the next small rise reverses it, and the fan hunts.  A rise never
	 * waits: that is the direction that matters for the hardware.
	 */
	if (want < state->last_rpm && c->hysteresis_c > 0.0) {
		if (celsius > state->last_temp - c->hysteresis_c)
			return state->last_rpm;
	}

	out = want;
	delta = out - state->last_rpm;

	if (delta > 0.0) {
		/* Above the spike threshold the limiter is bypassed: a thermal
		 * event is exactly when a rate limit is most harmful. */
		bool spiking = c->spike_temp_c > 0.0 && celsius >= c->spike_temp_c;

		if (!spiking && c->slew_up_rpm_per_tick > 0.0 &&
		    delta > c->slew_up_rpm_per_tick)
			out = state->last_rpm + c->slew_up_rpm_per_tick;
	} else if (delta < 0.0) {
		if (c->slew_down_rpm_per_tick > 0.0 &&
		    -delta > c->slew_down_rpm_per_tick)
			out = state->last_rpm - c->slew_down_rpm_per_tick;
	}

	state->last_rpm = out;
	state->last_temp = celsius;
	return out;
}

double
fanpro_curve_input(const fanpro_curve_t *c, const fanpro_sensor_set_t *sensors)
{
	if (c == NULL || sensors == NULL)
		return NAN;

	if (c->use_class) {
		/* Averaging a class is not offered: the class aggregate that
		 * matters for safety is the hottest member. */
		return fanpro_sensors_max_of_class(sensors, c->cls);
	}

	if (c->match[0] == '\0')
		return NAN;

	if (c->agg == FANPRO_AGG_AVG)
		return fanpro_sensors_avg_matching(sensors, c->match);

	return fanpro_sensors_max_matching(sensors, c->match);
}
