/*
 * fanpro - power draw and drive health.
 *
 * Both live behind runtime discovery.  IOReport is a private framework
 * reached by dlopen so that its disappearance on a macOS update costs us the
 * watt readings and nothing else; the NVMe SMART user client is probed the
 * same way.
 */
#ifndef FANPRO_POWER_H
#define FANPRO_POWER_H

#include <stdbool.h>
#include <stdint.h>

#define FANPRO_MAX_POWER_CHANNELS 32
#define FANPRO_POWER_NAME_MAX     64

typedef struct {
	char   name[FANPRO_POWER_NAME_MAX];
	double watts;
} fanpro_power_channel_t;

typedef struct {
	fanpro_power_channel_t channels[FANPRO_MAX_POWER_CHANNELS];
	int                    count;
	double                 total_watts;
	bool                   available;
} fanpro_power_set_t;

/*
 * Sample power over `interval_ms`.  Energy counters are cumulative, so a
 * single read means nothing: this takes two samples and divides the delta by
 * the elapsed time, which is why it blocks for the interval.
 *
 * Returns 0 on success, negative when IOReport is unavailable. `set->available`
 * reflects the same thing for callers that render rather than branch.
 */
int fanpro_power_sample(fanpro_power_set_t *set, unsigned interval_ms);

/* ---- NVMe / SSD health -------------------------------------------------- */

typedef struct {
	bool     available;
	bool     needed_root;    /* the read failed and we were not root */
	/*
	 * The controller exists but does not implement the SMART user client.
	 * Apple's internal ANS controllers on Apple Silicon answer
	 * kIOReturnUnsupported, so on those machines there are no SMART
	 * counters to be had at any privilege level.
	 */
	bool     smart_unsupported;
	bool     temp_from_hid;  /* celsius came from the HID NAND sensor */
	double   celsius;
	uint8_t  percent_used;   /* endurance consumed, 0-100+ */
	uint64_t power_on_hours;
	uint64_t unsafe_shutdowns;
	uint64_t data_units_read;
	uint64_t data_units_written;
	uint8_t  critical_warning;
} fanpro_disk_health_t;

/* Read SMART data from the internal NVMe controller. */
int fanpro_disk_health_read(fanpro_disk_health_t *out);

/* ---- thermal pressure --------------------------------------------------- */

/* Current macOS thermal pressure level, or FANPRO_THERMAL_UNKNOWN. */
int         fanpro_thermal_pressure_read(void);
const char *fanpro_thermal_level_name(int level);

/*
 * Map a raw notify value onto fanpro's canonical scale.  Exposed so the
 * mapping is unit testable without needing a hot machine.
 */
int fanpro_thermal_normalise(uint64_t raw);

#endif /* FANPRO_POWER_H */
