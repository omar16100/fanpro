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

/*
 * Drop the retained IOReport subscription.  The next sample resubscribes.
 *
 * Call this after wake: nothing in the IOReport SPI promises a subscription
 * survives a sleep cycle, and a stale one reports nothing rather than failing
 * loudly.  Must be called from the same thread that samples.
 */
void fanpro_power_invalidate(void);

/* Release the retained subscription at exit.  Idempotent. */
void fanpro_power_shutdown(void);

/*
 * Test seam.
 *
 * fanpro_power_sample owns a small state machine (subscribe once, resubscribe
 * on failure, back off when resubscribing keeps failing) that must be provable
 * without IOReport: the bug this seam exists for was a subscription leaked once
 * per sample, and an RSS assertion is too allocator-dependent to gate on.
 *
 * Passing NULL restores the real IOReport implementation.  Tests only; the
 * daemon never calls this.
 */
typedef struct {
	/* Establish a subscription. 0 on success, negative on failure. */
	int  (*subscribe)(void);
	/* Release whatever `subscribe` established. Must tolerate no-op. */
	void (*teardown)(void);
	/* Sample a delta pair. 0 on success, negative on failure. */
	int  (*sample_delta)(unsigned interval_ms, fanpro_power_set_t *set);
} fanpro_power_ops_t;

void fanpro_power_set_ops(const fanpro_power_ops_t *ops);

/* Observability for the tests and for `fanpro probe`: how many times the
 * subscription has been established since the process started. */
unsigned fanpro_power_subscribe_count(void);

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
