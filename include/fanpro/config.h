/*
 * fanpro - configuration.
 *
 * A small INI dialect, hand-parsed.  The parser's contract is that a
 * malformed file is REJECTED with a message, never silently half-applied:
 * a config that half-loads could leave a fan bound to a curve that does not
 * exist, and the daemon would then drive it from defaults nobody chose.
 *
 * Parsing is separated from file IO so the whole of it is unit testable
 * without touching /etc.
 */
#ifndef FANPRO_CONFIG_H
#define FANPRO_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "fanpro/curve.h"
#include "fanpro/fan.h"
#include "fanpro/log.h"
#include "fanpro/safety.h"

#define FANPRO_MAX_CURVES     8
#define FANPRO_MAX_ALERTS     8
#define FANPRO_CONFIG_ERR_MAX 256
#define FANPRO_DEFAULT_CONFIG_PATH "/etc/fanpro/fanpro.conf"

typedef enum {
	/* Monitor only.  Fans are left entirely to macOS.  The default,
	 * because taking control removes the firmware's own protection. */
	FANPRO_MODE_AUTO = 0,
	/* Drive fans from the configured curves. */
	FANPRO_MODE_CURVE,
} fanpro_mode_t;

typedef struct {
	char   name[32];
	char   match[FANPRO_SENSOR_NAME_MAX];
	fanpro_sensor_class_t cls;
	bool   use_class;
	double above_c;
	bool   notify;
} fanpro_alert_t;

typedef struct {
	fanpro_mode_t          mode;
	fanpro_log_level_t     log_level;
	fanpro_safety_config_t safety;

	fanpro_curve_t curves[FANPRO_MAX_CURVES];
	int            n_curves;

	/* Curve bound to each fan by name, empty meaning "leave on auto".
	 * Binding is explicit so multi-fan behaviour is never implicit. */
	char fan_curve[FANPRO_MAX_FANS][32];

	fanpro_alert_t alerts[FANPRO_MAX_ALERTS];
	int            n_alerts;

	unsigned tick_ms;
	/* Don't rewrite the target for changes smaller than this. */
	double   deadband_rpm;
	/* Ticks without a usable sensor sample before handing fans back. */
	int      sensor_watchdog_ticks;
	/* Seconds without a control-loop heartbeat before forcing a release. */
	unsigned heartbeat_timeout_s;
} fanpro_config_t;

void fanpro_config_defaults(fanpro_config_t *cfg);

/*
 * Parse `text` into `cfg`.  Returns 0 on success, negative on error, with a
 * human-readable message (including the line number) in `err`.
 * `cfg` is left untouched on failure: all or nothing.
 */
int fanpro_config_parse(fanpro_config_t *cfg, const char *text, char *err,
                        size_t err_len);

/*
 * Load from disk.  A missing file is NOT an error: it yields defaults, which
 * are monitor-only, so a fresh install does nothing until configured.
 */
int fanpro_config_load(fanpro_config_t *cfg, const char *path, char *err,
                       size_t err_len);

/* The curve bound to a fan, or NULL when it should be left on auto. */
const fanpro_curve_t *fanpro_config_curve_for_fan(const fanpro_config_t *cfg,
                                                  int fan_index);

const char *fanpro_config_mode_name(fanpro_mode_t m);

#endif /* FANPRO_CONFIG_H */
