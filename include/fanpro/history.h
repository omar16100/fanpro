/*
 * fanpro - sample history and alert rules.
 *
 * Both are split the same way: a pure decision function that can be tested
 * without a clock, a filesystem or a hot machine, and a thin layer that does
 * the IO.  The decisions are where the bugs live.
 */
#ifndef FANPRO_HISTORY_H
#define FANPRO_HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fanpro/config.h"
#include "fanpro/fan.h"
#include "fanpro/sensors.h"

#define FANPRO_HISTORY_DIR  "/var/log/fanpro"
#define FANPRO_HISTORY_FILE FANPRO_HISTORY_DIR "/history.jsonl"

/* One row of history.  Deliberately flat: it becomes one JSON object. */
typedef struct {
	int64_t unix_time;
	double  soc_c;
	double  gpu_c;
	double  nand_c;
	double  cpu_w;
	double  gpu_w;
	double  fan_rpm[FANPRO_MAX_FANS];
	double  fan_target[FANPRO_MAX_FANS];
	int     fan_mode[FANPRO_MAX_FANS];
	int     n_fans;
	int     thermal_level;
	bool    driving;
} fanpro_history_row_t;

/*
 * Render one row as a single JSON line, NUL-terminated, no trailing newline.
 * Returns the length written, or negative if it would not fit.  Pure, so the
 * exact output can be pinned by tests.
 */
int fanpro_history_format(const fanpro_history_row_t *row, char *out,
                          size_t out_len);

/*
 * Should the log roll over?  True when the day has changed since the file was
 * opened.  `day_of_year` values come from localtime; passing them in keeps
 * this testable without touching the clock.
 */
bool fanpro_history_should_rotate(int opened_day, int now_day);

int  fanpro_history_open(const char *path);
void fanpro_history_close(void);
/* Append one row, rotating first if the day changed. */
int  fanpro_history_append(const fanpro_history_row_t *row);

/* ---- alerts ------------------------------------------------------------- */

/*
 * Per-rule state.  An alert that re-fires every second is an alert nobody
 * reads, so a rule stays "firing" until the value falls back below the
 * threshold by a margin.
 */
typedef struct {
	bool   firing;
	double last_fired_s;
	int    fire_count;
} fanpro_alert_state_t;

#define FANPRO_ALERT_CLEAR_MARGIN_C 3.0
/* Even while still above the threshold, do not repeat more often than this. */
#define FANPRO_ALERT_REPEAT_S       300.0

/*
 * Decide whether this rule should fire now.  Pure: no clock, no IO, no
 * globals.  Updates `st` in place.
 */
bool fanpro_alert_should_fire(double value_c, double threshold_c,
                              fanpro_alert_state_t *st, double now_s);

/* Evaluate every configured rule and deliver whatever fires. */
void fanpro_alerts_evaluate(const fanpro_config_t *cfg,
                            const fanpro_sensor_set_t *sensors,
                            fanpro_alert_state_t *states, double now_s);

#endif /* FANPRO_HISTORY_H */
