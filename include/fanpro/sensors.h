/*
 * fanpro - sensor registry.
 *
 * Providers publish readings into one table.  Callers above this layer never
 * know or care which subsystem a reading came from, which is what lets a
 * provider disappear on a macOS update without taking the tool with it.
 *
 * Sensor names are machine-specific and FourCC-derived, so nothing here
 * hardcodes a name.  Classification is by prefix and keyword, and an
 * unrecognised sensor is reported as OTHER rather than dropped.
 */
#ifndef FANPRO_SENSORS_H
#define FANPRO_SENSORS_H

#include <stdbool.h>
#include <stdint.h>

#include "fanpro/smc.h"

/* This Mac Studio M3 Ultra reports ~680 temperature sensors across two dies
 * plus the SMC T* space, so the cap is set well above real hardware.  A set
 * that hits it reports `dropped` rather than silently truncating. */
#define FANPRO_MAX_SENSORS   1024
#define FANPRO_SENSOR_NAME_MAX 64

/*
 * Sensor class drives the safety layer: a single global panic temperature is
 * wrong, because 95 C is normal for SoC die and dangerous for NAND.
 */
typedef enum {
	FANPRO_CLASS_SOC = 0,   /* CPU/SoC die, performance and efficiency cores */
	FANPRO_CLASS_GPU,
	FANPRO_CLASS_NAND,      /* SSD/NAND, throttles far lower than the SoC */
	FANPRO_CLASS_AMBIENT,
	FANPRO_CLASS_POWER,     /* PMU, power delivery */
	FANPRO_CLASS_OTHER,
	FANPRO_CLASS_COUNT,
} fanpro_sensor_class_t;

typedef enum {
	FANPRO_SRC_HID = 0,     /* IOHIDEventSystemClient */
	FANPRO_SRC_SMC,         /* SMC T* key space */
} fanpro_sensor_source_t;

typedef struct {
	char                   name[FANPRO_SENSOR_NAME_MAX];
	double                 celsius;
	fanpro_sensor_class_t  cls;
	fanpro_sensor_source_t source;
	bool                   valid;   /* false when the last sample failed */
} fanpro_sensor_t;

typedef struct {
	fanpro_sensor_t sensors[FANPRO_MAX_SENSORS];
	int             count;
	/* Sensors a provider found but could not store because the table was
	 * full.  Never let a cap look like a complete reading: a truncated set
	 * silently missing the hottest sensor is a safety problem. */
	int             dropped;
	bool            hid_available;
	bool            smc_available;
	bool            smc_used;   /* the SMC fallback actually contributed */
} fanpro_sensor_set_t;

const char *fanpro_sensor_class_name(fanpro_sensor_class_t cls);

/* Classify a sensor by its reported name.  Never fails; unknown maps to OTHER. */
fanpro_sensor_class_t fanpro_sensor_classify(const char *name);

/*
 * Discover temperature sensors.
 *
 * HID is the primary provider.  The SMC T* space is a FALLBACK and is only
 * consulted when HID yields nothing, because on Apple Silicon that namespace
 * carries many opaque keys whose readings are not comparable to the die
 * sensors (this Mac Studio reports an SMC key at 96 C while every HID die
 * sensor reads 49 C).  Driving fans from unvalidated readings is not
 * justifiable, so they stay out of the default set.
 *
 * `smc` may be NULL to skip the SMC provider entirely.  Pass
 * `force_smc` to include the T* space anyway, which is what `fanpro sensors
 * --all` does for inspection.
 *
 * Returns 0 on success.  A run where no provider is available is still 0
 * with count 0: that is a reportable state, not a crash.
 */
int fanpro_sensors_enumerate(fanpro_sensor_set_t *set, fanpro_smc_t *smc,
                             bool force_smc);

/* Re-sample an already-enumerated set in place. */
int fanpro_sensors_refresh(fanpro_sensor_set_t *set, fanpro_smc_t *smc,
                           bool force_smc);

/* Hottest valid sensor of a class, or NAN when the class has no valid reading. */
double fanpro_sensors_max_of_class(const fanpro_sensor_set_t *set,
                                   fanpro_sensor_class_t cls);

/* Hottest valid sensor overall, or NAN. */
double fanpro_sensors_max(const fanpro_sensor_set_t *set);

/* Mean of valid sensors whose name contains `substr` (case-insensitive). */
double fanpro_sensors_avg_matching(const fanpro_sensor_set_t *set,
                                   const char *substr);

/* Hottest valid sensor whose name contains `substr`, or NAN. */
double fanpro_sensors_max_matching(const fanpro_sensor_set_t *set,
                                   const char *substr);

/*
 * Append one reading, disambiguating a name that already exists.
 * Machines with more than one die report the same sensor name several times
 * (an M3 Ultra reports "PMU tdie1" once per die), so occurrences after the
 * first get a "#N" suffix.  Returns false when the table is full, and bumps
 * set->dropped so the caller can report the shortfall instead of hiding it.
 */
bool fanpro_sensors_add(fanpro_sensor_set_t *set, const char *name,
                        double celsius, fanpro_sensor_source_t source);

/* ---- providers, used by the registry ------------------------------------ */

/* Returns the number of sensors appended, or negative when unavailable. */
int fanpro_hid_temp_read(fanpro_sensor_set_t *set);
int fanpro_smc_temp_read(fanpro_sensor_set_t *set, fanpro_smc_t *smc);

#endif /* FANPRO_SENSORS_H */
