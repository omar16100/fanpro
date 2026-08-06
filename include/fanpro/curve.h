/*
 * fanpro - temperature to RPM curves.
 *
 * Evaluation is a pure function of (config, state, temperature).  No SMC
 * access, no clock, no globals, so the whole of it is testable and its
 * behaviour is reproducible from the inputs alone.
 *
 * Two shaping rules exist because a raw interpolation drives fans badly:
 *
 *   hysteresis  a small temperature wobble around a curve knee would
 *               otherwise make the fan hunt audibly.  Applied only on the
 *               way DOWN; a rising temperature always gets a response.
 *
 *   slew limit  caps how fast the target may change per tick, ASYMMETRICALLY.
 *               Ramping down slowly is comfort; ramping up slowly is a
 *               thermal risk, so the upward limit is loose and is bypassed
 *               entirely above the spike threshold.
 */
#ifndef FANPRO_CURVE_H
#define FANPRO_CURVE_H

#include <stdbool.h>

#include "fanpro/sensors.h"

#define FANPRO_CURVE_MAX_POINTS 16
#define FANPRO_CURVE_NAME_MAX   32

typedef enum {
	FANPRO_AGG_MAX = 0, /* hottest matching sensor, the safe default */
	FANPRO_AGG_AVG,     /* mean, useful for noisy multi-die sensor groups */
} fanpro_agg_t;

typedef struct {
	double celsius;
	double rpm;
	bool   rpm_is_max; /* the config said "max"; resolved against the fan */
} fanpro_curve_point_t;

typedef struct {
	char name[FANPRO_CURVE_NAME_MAX];

	fanpro_curve_point_t points[FANPRO_CURVE_MAX_POINTS];
	int                  n_points;

	/* Input selection: either a sensor-name substring or a whole class. */
	char                  match[FANPRO_SENSOR_NAME_MAX];
	fanpro_sensor_class_t cls;
	bool                  use_class;
	fanpro_agg_t          agg;

	/* Shaping.  Zero disables the corresponding limit. */
	double hysteresis_c;
	double slew_up_rpm_per_tick;
	double slew_down_rpm_per_tick;
	double spike_temp_c; /* at or above this, upward slew is not limited */
} fanpro_curve_t;

/* Per-fan evaluation state.  Zero-initialise for a fresh start. */
typedef struct {
	double last_rpm;
	double last_temp;
	bool   primed;
} fanpro_curve_state_t;

/* Sensible defaults for a curve before config is applied. */
void fanpro_curve_defaults(fanpro_curve_t *c);

/*
 * Raw piecewise-linear lookup, before hysteresis or slew.
 * Below the first point returns the first RPM, above the last returns the
 * last: extrapolating past a curve's stated range is not something a fan
 * controller should invent.
 * Returns NAN if the curve has no points or the temperature is not finite.
 */
double fanpro_curve_interpolate(const fanpro_curve_t *c, double celsius,
                                double fan_max_rpm);

/*
 * Full evaluation: interpolate, then apply hysteresis and slew against the
 * previous state.  `state` is updated in place.  Returns NAN when the input
 * temperature is unusable, which the caller must treat as "no opinion"
 * rather than as zero RPM.
 */
double fanpro_curve_eval(const fanpro_curve_t *c, fanpro_curve_state_t *state,
                         double celsius, double fan_max_rpm);

/*
 * Pick the temperature this curve should be driven by.
 * Returns NAN when no sensor matches, which is a release condition upstream,
 * not a reason to guess.
 */
double fanpro_curve_input(const fanpro_curve_t *c,
                          const fanpro_sensor_set_t *sensors);

#endif /* FANPRO_CURVE_H */
