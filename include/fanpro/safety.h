/*
 * fanpro - the safety gate.
 *
 * Every proposed fan target passes through fanpro_safety_check before it can
 * reach the SMC.  There is no second path.  The gate is a pure function so
 * its behaviour can be pinned by tests rather than argued about.
 *
 * Its bias is deliberate: when the inputs are not trustworthy it hands the
 * fans back to the firmware.  Apple's thermal management is competent, so
 * "release control" is always a safe answer, whereas "hold the last target"
 * is not.
 */
#ifndef FANPRO_SAFETY_H
#define FANPRO_SAFETY_H

#include <stdbool.h>

#include "fanpro/fan.h"
#include "fanpro/sensors.h"

/*
 * macOS thermal pressure, read via notify_get_state on
 * com.apple.system.thermalpressurelevel.  An independent signal: it does not
 * depend on fanpro's own sampling being correct, which is what makes it worth
 * consulting.
 *
 * These are fanpro's own canonical values, not the raw ones.  macOS reports
 * two different scales and the provider normalises into this one:
 *
 *   modern (measured on macOS 26.3):  0 1 2 3 4 5    nominal..sleeping
 *   legacy:                           0 10 20 30 40
 *
 * Assuming the legacy scale made a raw 2 ("moderate") read as unknown, which
 * silently removed this input from the safety gate exactly when the machine
 * was getting hot.
 */
#define FANPRO_THERMAL_NOMINAL   0
#define FANPRO_THERMAL_LIGHT     5
#define FANPRO_THERMAL_MODERATE  10
#define FANPRO_THERMAL_HEAVY     20
#define FANPRO_THERMAL_TRAPPING  30
#define FANPRO_THERMAL_SLEEPING  40
#define FANPRO_THERMAL_UNKNOWN   (-1)

typedef struct {
	/*
	 * Panic temperature per sensor class.  One global number would be
	 * wrong in both directions: 95 C is unremarkable on SoC die and
	 * dangerous on NAND.  NAN disables panic for a class, which is the
	 * default for OTHER because those readings are unvalidated.
	 */
	double panic_c[FANPRO_CLASS_COUNT];

	/*
	 * Fan stop is physically permitted by the firmware.  It stays behind
	 * an explicit opt-in so a malformed curve can never stop a fan.
	 */
	bool allow_fan_stop;

	/* Consecutive failed sensor samples before control is handed back. */
	int max_sample_failures;

	/* Thermal pressure at or above this level forces a panic. */
	int panic_thermal_level;
} fanpro_safety_config_t;

typedef enum {
	FANPRO_SAFETY_OK = 0,   /* use rpm as proposed */
	FANPRO_SAFETY_CLAMPED,  /* use rpm, adjusted to stay in range */
	FANPRO_SAFETY_PANIC,    /* drive rpm (== fan max) now, then release */
	FANPRO_SAFETY_RELEASE,  /* do not drive; hand the fan to the firmware */
} fanpro_safety_verdict_t;

typedef struct {
	fanpro_safety_verdict_t verdict;
	double                  rpm;
	const char             *reason;      /* stable slug, safe to log */
	fanpro_sensor_class_t   panic_class; /* meaningful when verdict is PANIC */
	double                  panic_temp;
} fanpro_safety_result_t;

/* Conservative defaults. */
void fanpro_safety_defaults(fanpro_safety_config_t *cfg);

/*
 * Judge a proposed target.
 *
 * `proposed_rpm` may be NAN, meaning the curve had no opinion; that is a
 * release condition, never an excuse to pick a number.
 * `sample_failures` is the count of consecutive failed sensor samples.
 * `thermal_level` is FANPRO_THERMAL_* or FANPRO_THERMAL_UNKNOWN.
 */
fanpro_safety_result_t fanpro_safety_check(const fanpro_safety_config_t *cfg,
                                           const fanpro_fan_t *fan,
                                           double proposed_rpm,
                                           const fanpro_sensor_set_t *sensors,
                                           int sample_failures,
                                           int thermal_level);

const char *fanpro_safety_verdict_name(fanpro_safety_verdict_t v);

#endif /* FANPRO_SAFETY_H */
