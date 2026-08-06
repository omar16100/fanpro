/*
 * fanpro - the safety gate.
 *
 * Order of judgement matters and is fixed:
 *
 *   1. Is the fan itself sane?          no  -> RELEASE
 *   2. Is any sensor class in panic?    yes -> PANIC at max
 *   3. Is the machine in thermal        yes -> PANIC at max
 *      distress by macOS's own account?
 *   4. Are our samples trustworthy?     no  -> RELEASE
 *   5. Did the curve have an opinion?   no  -> RELEASE
 *   6. Clamp into the fan's own range.
 *
 * Panic is checked before sample staleness on purpose: a set that is one
 * tick stale but showing 100 C is still telling us something urgent.
 */
#include "fanpro/safety.h"

#include <math.h>
#include <string.h>

void
fanpro_safety_defaults(fanpro_safety_config_t *cfg)
{
	int i;

	if (cfg == NULL)
		return;

	memset(cfg, 0, sizeof(*cfg));

	for (i = 0; i < FANPRO_CLASS_COUNT; i++)
		cfg->panic_c[i] = NAN;

	/* Apple Silicon die sensors run hot by design; 95 C is where sustained
	 * operation stops being routine. */
	cfg->panic_c[FANPRO_CLASS_SOC] = 95.0;
	cfg->panic_c[FANPRO_CLASS_GPU] = 95.0;
	/* NAND throttles far lower than the SoC. */
	cfg->panic_c[FANPRO_CLASS_NAND] = 80.0;
	cfg->panic_c[FANPRO_CLASS_POWER] = 100.0;
	cfg->panic_c[FANPRO_CLASS_AMBIENT] = 70.0;
	/* OTHER stays NAN: those readings are unvalidated, and this machine
	 * has opaque keys sitting at 96 C while every die sensor reads 49 C.
	 * Panicking on them would mean permanent full-speed fans. */

	cfg->allow_fan_stop = false;
	cfg->max_sample_failures = 3;
	cfg->panic_thermal_level = FANPRO_THERMAL_TRAPPING;
}

const char *
fanpro_safety_verdict_name(fanpro_safety_verdict_t v)
{
	switch (v) {
	case FANPRO_SAFETY_OK:      return "ok";
	case FANPRO_SAFETY_CLAMPED: return "clamped";
	case FANPRO_SAFETY_PANIC:   return "panic";
	case FANPRO_SAFETY_RELEASE: return "release";
	default:                    return "unknown";
	}
}

static fanpro_safety_result_t
release(const char *reason)
{
	fanpro_safety_result_t r;

	memset(&r, 0, sizeof(r));
	r.verdict = FANPRO_SAFETY_RELEASE;
	r.rpm = NAN;
	r.reason = reason;
	r.panic_class = FANPRO_CLASS_OTHER;
	r.panic_temp = NAN;
	return r;
}

fanpro_safety_result_t
fanpro_safety_check(const fanpro_safety_config_t *cfg, const fanpro_fan_t *fan,
                    double proposed_rpm, const fanpro_sensor_set_t *sensors,
                    int sample_failures, int thermal_level)
{
	fanpro_safety_result_t r;
	double floor_rpm;
	int cls;

	if (cfg == NULL || fan == NULL)
		return release("no_config");

	/* A fan whose own limits are unreadable must never be driven: without
	 * them there is no meaningful clamp. */
	if (!isfinite(fan->min_rpm) || !isfinite(fan->max_rpm) ||
	    fan->min_rpm < 0.0 || fan->max_rpm <= fan->min_rpm)
		return release("fan_limits_unknown");

	memset(&r, 0, sizeof(r));
	r.panic_class = FANPRO_CLASS_OTHER;
	r.panic_temp = NAN;

	/* Panic first: an urgent reading outranks every other consideration,
	 * including a slightly stale sample set. */
	if (sensors != NULL) {
		for (cls = 0; cls < FANPRO_CLASS_COUNT; cls++) {
			double limit = cfg->panic_c[cls];
			double hottest;

			if (!isfinite(limit))
				continue;

			hottest = fanpro_sensors_max_of_class(
			    sensors, (fanpro_sensor_class_t)cls);
			if (!isfinite(hottest) || hottest < limit)
				continue;

			r.verdict = FANPRO_SAFETY_PANIC;
			r.rpm = fan->max_rpm;
			r.reason = "class_over_panic_temp";
			r.panic_class = (fanpro_sensor_class_t)cls;
			r.panic_temp = hottest;
			return r;
		}
	}

	/* macOS's own thermal verdict, independent of our sampling. */
	if (thermal_level != FANPRO_THERMAL_UNKNOWN &&
	    cfg->panic_thermal_level > 0 &&
	    thermal_level >= cfg->panic_thermal_level) {
		r.verdict = FANPRO_SAFETY_PANIC;
		r.rpm = fan->max_rpm;
		r.reason = "thermal_pressure";
		r.panic_temp = NAN;
		return r;
	}

	/* Holding a stale target is worse than handing back control, because
	 * the firmware can still see the temperatures we have lost. */
	if (cfg->max_sample_failures > 0 &&
	    sample_failures >= cfg->max_sample_failures)
		return release("sensor_samples_stale");

	/* NULL and empty mean the same thing: we are blind.  Treating NULL as
	 * "no objection" would let the gate pass a target with zero sensor
	 * data behind it, which is the one thing it exists to prevent. */
	if (sensors == NULL || sensors->count == 0)
		return release("no_sensors");

	/* NAN means the curve had no opinion.  Inventing one here would be
	 * the single most dangerous thing this function could do. */
	if (!isfinite(proposed_rpm))
		return release("no_curve_opinion");

	/* Clamp into the fan's own advertised range. */
	r.rpm = proposed_rpm;
	r.verdict = FANPRO_SAFETY_OK;
	r.reason = "ok";

	floor_rpm = cfg->allow_fan_stop ? 0.0 : fan->min_rpm;

	if (r.rpm < floor_rpm) {
		r.rpm = floor_rpm;
		r.verdict = FANPRO_SAFETY_CLAMPED;
		r.reason = "below_min";
	} else if (r.rpm > fan->max_rpm) {
		r.rpm = fan->max_rpm;
		r.verdict = FANPRO_SAFETY_CLAMPED;
		r.reason = "above_max";
	}

	return r;
}
