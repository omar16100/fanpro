/*
 * fanpro - configuration parsing.
 *
 * Strict on purpose.  An unknown key, a malformed curve point or a fan bound
 * to a curve that does not exist all fail the whole parse.  The alternative,
 * skipping what it cannot understand, would let a typo silently leave a fan
 * on a default curve the operator never chose, and on this hardware the
 * firmware will not correct that mistake.
 *
 * Parsing happens into a scratch config and is only committed on success, so
 * a failed reload leaves the running daemon on its previous settings.
 */
#include "fanpro/config.h"

#include "fanpro/sensors.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONFIG_BYTES (256 * 1024)

void
fanpro_config_defaults(fanpro_config_t *cfg)
{
	int i;

	if (cfg == NULL)
		return;

	memset(cfg, 0, sizeof(*cfg));

	/* Monitor only.  Taking control of a fan removes the firmware's own
	 * thermal protection from it, so that is never the default. */
	cfg->mode = FANPRO_MODE_AUTO;
	cfg->log_level = FANPRO_LOG_INFO;
	fanpro_safety_defaults(&cfg->safety);

	cfg->tick_ms = 1000;
	cfg->deadband_rpm = 25.0;
	cfg->sensor_watchdog_ticks = 3;
	cfg->heartbeat_timeout_s = 10;

	for (i = 0; i < FANPRO_MAX_FANS; i++)
		cfg->fan_curve[i][0] = '\0';
}

const char *
fanpro_config_mode_name(fanpro_mode_t m)
{
	switch (m) {
	case FANPRO_MODE_AUTO:  return "auto";
	case FANPRO_MODE_CURVE: return "curve";
	default:                return "unknown";
	}
}

/* ---- small helpers ------------------------------------------------------ */

static char *
trim(char *s)
{
	char *end;

	while (*s != '\0' && isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return s;
	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

static bool
parse_double(const char *s, double *out)
{
	char *end = NULL;
	double v;

	errno = 0;
	v = strtod(s, &end);
	if (end == s || errno != 0)
		return false;
	while (*end != '\0' && isspace((unsigned char)*end))
		end++;
	if (*end != '\0')
		return false;
	*out = v;
	return true;
}

static bool
parse_bool(const char *s, bool *out)
{
	if (strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0 ||
	    strcmp(s, "1") == 0) {
		*out = true;
		return true;
	}
	if (strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0 ||
	    strcmp(s, "0") == 0) {
		*out = false;
		return true;
	}
	return false;
}

static bool
parse_class(const char *s, fanpro_sensor_class_t *out)
{
	int i;

	for (i = 0; i < FANPRO_CLASS_COUNT; i++) {
		if (strcasecmp(s, fanpro_sensor_class_name(
		                      (fanpro_sensor_class_t)i)) == 0) {
			*out = (fanpro_sensor_class_t)i;
			return true;
		}
	}
	return false;
}

/*
 * "class:soc" selects a whole sensor class; "match:tdie" selects sensors by
 * name substring.  A bare word is treated as a class so the common case stays
 * short.
 */
static bool
parse_source(const char *val, fanpro_curve_t *c)
{
	if (strncasecmp(val, "class:", 6) == 0) {
		c->use_class = true;
		return parse_class(val + 6, &c->cls);
	}
	if (strncasecmp(val, "match:", 6) == 0) {
		c->use_class = false;
		snprintf(c->match, sizeof(c->match), "%s", val + 6);
		return c->match[0] != '\0';
	}
	c->use_class = true;
	return parse_class(val, &c->cls);
}

/*
 * "45:1000, 70:2000, 85:max".  Points must be strictly increasing in
 * temperature: an out-of-order list would make interpolation silently return
 * the wrong RPM rather than fail.
 */
static bool
parse_points(const char *val, fanpro_curve_t *c, const char **why)
{
	const char *p = val;

	c->n_points = 0;

	while (*p != '\0') {
		double temp, rpm = 0.0;
		char tbuf[32], rbuf[32];
		const char *colon, *comma;
		size_t tn, rn;
		bool is_max = false;

		while (*p != '\0' && (isspace((unsigned char)*p) || *p == ','))
			p++;
		if (*p == '\0')
			break;

		colon = strchr(p, ':');
		if (colon == NULL) {
			*why = "expected temp:rpm";
			return false;
		}
		comma = strchr(colon, ',');

		tn = (size_t)(colon - p);
		rn = (comma != NULL) ? (size_t)(comma - colon - 1) : strlen(colon + 1);
		if (tn == 0 || tn >= sizeof(tbuf) || rn == 0 || rn >= sizeof(rbuf)) {
			*why = "malformed point";
			return false;
		}

		memcpy(tbuf, p, tn);
		tbuf[tn] = '\0';
		memcpy(rbuf, colon + 1, rn);
		rbuf[rn] = '\0';

		if (!parse_double(trim(tbuf), &temp)) {
			*why = "bad temperature";
			return false;
		}
		if (strcasecmp(trim(rbuf), "max") == 0) {
			is_max = true;
		} else if (!parse_double(trim(rbuf), &rpm)) {
			*why = "bad rpm";
			return false;
		}

		if (temp < -50.0 || temp > 150.0) {
			*why = "temperature out of range";
			return false;
		}
		if (!is_max && (rpm < 0.0 || rpm > 20000.0)) {
			*why = "rpm out of range";
			return false;
		}
		if (c->n_points >= FANPRO_CURVE_MAX_POINTS) {
			*why = "too many points";
			return false;
		}
		if (c->n_points > 0 &&
		    temp <= c->points[c->n_points - 1].celsius) {
			*why = "points must increase in temperature";
			return false;
		}

		c->points[c->n_points].celsius = temp;
		c->points[c->n_points].rpm = rpm;
		c->points[c->n_points].rpm_is_max = is_max;
		c->n_points++;

		if (comma == NULL)
			break;
		p = comma + 1;
	}

	if (c->n_points == 0) {
		*why = "no points";
		return false;
	}
	return true;
}

/* ---- section dispatch --------------------------------------------------- */

typedef enum {
	SEC_NONE = 0,
	SEC_GENERAL,
	SEC_FAN,
	SEC_CURVE,
	SEC_ALERT,
} section_t;

static int
find_curve(fanpro_config_t *cfg, const char *name)
{
	int i;

	for (i = 0; i < cfg->n_curves; i++) {
		if (strcmp(cfg->curves[i].name, name) == 0)
			return i;
	}
	return -1;
}

#define FAIL(fmt, ...)                                                        \
	do {                                                                  \
		snprintf(err, err_len, "line %d: " fmt, lineno, __VA_ARGS__); \
		return -1;                                                    \
	} while (0)

#define FAIL0(msg)                                                            \
	do {                                                                  \
		snprintf(err, err_len, "line %d: %s", lineno, (msg));         \
		return -1;                                                    \
	} while (0)

static int
apply_general(fanpro_config_t *cfg, const char *key, const char *val,
              int lineno, char *err, size_t err_len)
{
	double d;
	bool b;

	if (strcmp(key, "mode") == 0) {
		if (strcasecmp(val, "auto") == 0)
			cfg->mode = FANPRO_MODE_AUTO;
		else if (strcasecmp(val, "curve") == 0)
			cfg->mode = FANPRO_MODE_CURVE;
		else
			FAIL("unknown mode '%s' (auto|curve)", val);
		return 0;
	}
	if (strcmp(key, "log_level") == 0) {
		if (fanpro_log_level_from_str(val, &cfg->log_level) != 0)
			FAIL("unknown log level '%s'", val);
		return 0;
	}
	if (strcmp(key, "allow_fan_stop") == 0) {
		if (!parse_bool(val, &b))
			FAIL("expected a boolean, got '%s'", val);
		cfg->safety.allow_fan_stop = b;
		return 0;
	}
	if (strcmp(key, "max_sample_failures") == 0) {
		if (!parse_double(val, &d) || d < 1.0 || d > 1000.0)
			FAIL("max_sample_failures out of range: '%s'", val);
		cfg->safety.max_sample_failures = (int)d;
		cfg->sensor_watchdog_ticks = (int)d;
		return 0;
	}
	if (strcmp(key, "tick_ms") == 0) {
		if (!parse_double(val, &d) || d < 100.0 || d > 60000.0)
			FAIL("tick_ms out of range (100-60000): '%s'", val);
		cfg->tick_ms = (unsigned)d;
		return 0;
	}
	if (strcmp(key, "deadband_rpm") == 0) {
		if (!parse_double(val, &d) || d < 0.0 || d > 1000.0)
			FAIL("deadband_rpm out of range: '%s'", val);
		cfg->deadband_rpm = d;
		return 0;
	}
	if (strcmp(key, "heartbeat_timeout_s") == 0) {
		if (!parse_double(val, &d) || d < 2.0 || d > 300.0)
			FAIL("heartbeat_timeout_s out of range (2-300): '%s'", val);
		cfg->heartbeat_timeout_s = (unsigned)d;
		return 0;
	}

	/* panic_<class>_c */
	if (strncmp(key, "panic_", 6) == 0) {
		char cls_name[32];
		fanpro_sensor_class_t cls;
		size_t n = strlen(key + 6);

		if (n < 3 || strcmp(key + strlen(key) - 2, "_c") != 0)
			FAIL("unknown key '%s'", key);
		n -= 2;
		if (n >= sizeof(cls_name))
			FAIL("unknown key '%s'", key);
		memcpy(cls_name, key + 6, n);
		cls_name[n] = '\0';

		if (!parse_class(cls_name, &cls))
			FAIL("unknown sensor class '%s'", cls_name);
		if (!parse_double(val, &d) || d < 20.0 || d > 130.0)
			FAIL("panic temperature out of range (20-130): '%s'", val);
		cfg->safety.panic_c[cls] = d;
		return 0;
	}

	FAIL("unknown key '%s' in [general]", key);
}

static int
apply_curve(fanpro_curve_t *c, const char *key, const char *val, int lineno,
            char *err, size_t err_len)
{
	const char *why = "invalid";
	double d;

	if (strcmp(key, "source") == 0) {
		if (!parse_source(val, c))
			FAIL("bad source '%s' (class:NAME or match:TEXT)", val);
		return 0;
	}
	if (strcmp(key, "points") == 0) {
		if (!parse_points(val, c, &why))
			FAIL("bad points: %s", why);
		return 0;
	}
	if (strcmp(key, "aggregate") == 0) {
		if (strcasecmp(val, "max") == 0)
			c->agg = FANPRO_AGG_MAX;
		else if (strcasecmp(val, "avg") == 0)
			c->agg = FANPRO_AGG_AVG;
		else
			FAIL("unknown aggregate '%s' (max|avg)", val);
		return 0;
	}
	if (strcmp(key, "hysteresis_c") == 0) {
		if (!parse_double(val, &d) || d < 0.0 || d > 20.0)
			FAIL("hysteresis_c out of range (0-20): '%s'", val);
		c->hysteresis_c = d;
		return 0;
	}
	if (strcmp(key, "slew_up_rpm") == 0) {
		if (!parse_double(val, &d) || d < 0.0 || d > 20000.0)
			FAIL("slew_up_rpm out of range: '%s'", val);
		c->slew_up_rpm_per_tick = d;
		return 0;
	}
	if (strcmp(key, "slew_down_rpm") == 0) {
		if (!parse_double(val, &d) || d < 0.0 || d > 20000.0)
			FAIL("slew_down_rpm out of range: '%s'", val);
		c->slew_down_rpm_per_tick = d;
		return 0;
	}
	if (strcmp(key, "spike_temp_c") == 0) {
		if (!parse_double(val, &d) || d < 0.0 || d > 150.0)
			FAIL("spike_temp_c out of range: '%s'", val);
		c->spike_temp_c = d;
		return 0;
	}

	FAIL("unknown key '%s' in [curve.%s]", key, c->name);
}

static int
apply_alert(fanpro_alert_t *a, const char *key, const char *val, int lineno,
            char *err, size_t err_len)
{
	double d;
	bool b;

	if (strcmp(key, "source") == 0) {
		if (strncasecmp(val, "class:", 6) == 0) {
			a->use_class = true;
			if (!parse_class(val + 6, &a->cls))
				FAIL("unknown sensor class in '%s'", val);
		} else if (strncasecmp(val, "match:", 6) == 0) {
			a->use_class = false;
			snprintf(a->match, sizeof(a->match), "%s", val + 6);
		} else {
			a->use_class = true;
			if (!parse_class(val, &a->cls))
				FAIL("bad source '%s'", val);
		}
		return 0;
	}
	if (strcmp(key, "above_c") == 0) {
		if (!parse_double(val, &d) || d < -50.0 || d > 150.0)
			FAIL("above_c out of range: '%s'", val);
		a->above_c = d;
		return 0;
	}
	if (strcmp(key, "notify") == 0) {
		if (!parse_bool(val, &b))
			FAIL("expected a boolean, got '%s'", val);
		a->notify = b;
		return 0;
	}

	FAIL("unknown key '%s' in [alert.%s]", key, a->name);
}

int
fanpro_config_parse(fanpro_config_t *cfg, const char *text, char *err,
                    size_t err_len)
{
	/*
	 * Parse into scratch and commit only on success, so a bad reload cannot
	 * leave the daemon half-configured.  Deliberately NOT static: a static
	 * buffer would make this function quietly non-reentrant, which is a
	 * landmine in a library whose header promises nothing of the sort.
	 */
	fanpro_config_t scratch;
	section_t section = SEC_NONE;
	int cur_curve = -1, cur_alert = -1, cur_fan = -1;
	int lineno = 0;
	const char *p;
	char line[512];

	if (cfg == NULL || text == NULL || err == NULL || err_len == 0)
		return -1;

	err[0] = '\0';
	fanpro_config_defaults(&scratch);

	for (p = text; *p != '\0';) {
		const char *nl = strchr(p, '\n');
		size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
		char *s, *eq, *key, *val;

		lineno++;
		if (len >= sizeof(line))
			FAIL0("line too long");
		memcpy(line, p, len);
		line[len] = '\0';
		p = (nl != NULL) ? nl + 1 : p + len;

		/* Comments and blanks. */
		s = trim(line);
		if (*s == '\0' || *s == '#' || *s == ';')
			continue;

		if (*s == '[') {
			char *close = strchr(s, ']');
			char *name;

			if (close == NULL)
				FAIL0("unterminated section header");
			*close = '\0';
			name = trim(s + 1);

			cur_curve = cur_alert = cur_fan = -1;

			if (strcasecmp(name, "general") == 0) {
				section = SEC_GENERAL;
			} else if (strncasecmp(name, "fan.", 4) == 0) {
				double idx;

				if (!parse_double(name + 4, &idx) || idx < 0 ||
				    idx >= FANPRO_MAX_FANS)
					FAIL("fan index out of range in [%s]", name);
				section = SEC_FAN;
				cur_fan = (int)idx;
			} else if (strncasecmp(name, "curve.", 6) == 0) {
				if (scratch.n_curves >= FANPRO_MAX_CURVES)
					FAIL0("too many curves");
				if (find_curve(&scratch, name + 6) >= 0)
					FAIL("duplicate curve '%s'", name + 6);
				cur_curve = scratch.n_curves++;
				fanpro_curve_defaults(&scratch.curves[cur_curve]);
				snprintf(scratch.curves[cur_curve].name,
				         sizeof(scratch.curves[cur_curve].name),
				         "%s", name + 6);
				scratch.curves[cur_curve].n_points = 0;
				section = SEC_CURVE;
			} else if (strncasecmp(name, "alert.", 6) == 0) {
				if (scratch.n_alerts >= FANPRO_MAX_ALERTS)
					FAIL0("too many alerts");
				cur_alert = scratch.n_alerts++;
				memset(&scratch.alerts[cur_alert], 0,
				       sizeof(scratch.alerts[cur_alert]));
				snprintf(scratch.alerts[cur_alert].name,
				         sizeof(scratch.alerts[cur_alert].name),
				         "%s", name + 6);
				scratch.alerts[cur_alert].above_c = NAN;
				section = SEC_ALERT;
			} else {
				FAIL("unknown section '[%s]'", name);
			}
			continue;
		}

		eq = strchr(s, '=');
		if (eq == NULL)
			FAIL0("expected key = value");
		*eq = '\0';
		key = trim(s);
		val = trim(eq + 1);
		if (*key == '\0')
			FAIL0("empty key");

		switch (section) {
		case SEC_GENERAL:
			if (apply_general(&scratch, key, val, lineno, err, err_len) != 0)
				return -1;
			break;
		case SEC_FAN:
			if (strcmp(key, "curve") != 0)
				FAIL("unknown key '%s' in [fan.N]", key);
			if (strlen(val) >= sizeof(scratch.fan_curve[0]))
				FAIL0("curve name too long");
			snprintf(scratch.fan_curve[cur_fan],
			         sizeof(scratch.fan_curve[cur_fan]), "%s", val);
			break;
		case SEC_CURVE:
			if (apply_curve(&scratch.curves[cur_curve], key, val, lineno,
			                err, err_len) != 0)
				return -1;
			break;
		case SEC_ALERT:
			if (apply_alert(&scratch.alerts[cur_alert], key, val, lineno,
			                err, err_len) != 0)
				return -1;
			break;
		default:
			FAIL("key '%s' outside any section", key);
		}
	}

	/* Cross-checks that only make sense once everything is read. */
	{
		int i;

		for (i = 0; i < scratch.n_curves; i++) {
			if (scratch.curves[i].n_points == 0) {
				snprintf(err, err_len,
				         "curve '%s' has no points",
				         scratch.curves[i].name);
				return -1;
			}
		}
		for (i = 0; i < FANPRO_MAX_FANS; i++) {
			if (scratch.fan_curve[i][0] == '\0')
				continue;
			/* A fan bound to a curve that does not exist would
			 * otherwise silently fall back to defaults. */
			if (find_curve(&scratch, scratch.fan_curve[i]) < 0) {
				snprintf(err, err_len,
				         "fan %d references unknown curve '%s'",
				         i, scratch.fan_curve[i]);
				return -1;
			}
		}
		for (i = 0; i < scratch.n_alerts; i++) {
			if (isnan(scratch.alerts[i].above_c)) {
				snprintf(err, err_len,
				         "alert '%s' has no above_c",
				         scratch.alerts[i].name);
				return -1;
			}
		}
	}

	*cfg = scratch;
	return 0;
}

int
fanpro_config_load(fanpro_config_t *cfg, const char *path, char *err,
                   size_t err_len)
{
	char *buf;
	FILE *f;
	long size;
	size_t got;
	int rc;

	if (cfg == NULL || err == NULL || err_len == 0)
		return -1;

	err[0] = '\0';
	if (path == NULL)
		path = FANPRO_DEFAULT_CONFIG_PATH;

	f = fopen(path, "rb");
	if (f == NULL) {
		/* Absent config is not an error.  Defaults are monitor-only, so
		 * a fresh install does nothing until deliberately configured. */
		fanpro_config_defaults(cfg);
		if (errno == ENOENT)
			return 0;
		snprintf(err, err_len, "%s: %s", path, strerror(errno));
		return -1;
	}

	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		snprintf(err, err_len, "%s: cannot determine size", path);
		fclose(f);
		return -1;
	}
	if (size > MAX_CONFIG_BYTES) {
		snprintf(err, err_len, "%s: file too large (%ld bytes)", path, size);
		fclose(f);
		return -1;
	}

	buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		snprintf(err, err_len, "out of memory");
		fclose(f);
		return -1;
	}
	got = fread(buf, 1, (size_t)size, f);
	buf[got] = '\0';
	fclose(f);

	rc = fanpro_config_parse(cfg, buf, err, err_len);
	free(buf);
	return rc;
}

const fanpro_curve_t *
fanpro_config_curve_for_fan(const fanpro_config_t *cfg, int fan_index)
{
	int i;

	if (cfg == NULL || fan_index < 0 || fan_index >= FANPRO_MAX_FANS)
		return NULL;
	if (cfg->fan_curve[fan_index][0] == '\0')
		return NULL;

	for (i = 0; i < cfg->n_curves; i++) {
		if (strcmp(cfg->curves[i].name, cfg->fan_curve[fan_index]) == 0)
			return &cfg->curves[i];
	}
	return NULL;
}
