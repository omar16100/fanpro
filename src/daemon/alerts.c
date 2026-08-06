/*
 * fanpro - threshold alerts.
 *
 * The interesting part is not the delivery, it is the hysteresis.  A rule
 * that fires on every tick it is over threshold produces a hundred
 * notifications a minute, which trains the user to ignore all of them.  So a
 * rule fires once on crossing, stays quiet while it remains over, repeats at
 * most every few minutes, and re-arms only after the value has fallen
 * meaningfully back below the line.
 *
 * That decision is a pure function so it can be tested exhaustively without a
 * clock or a hot machine.
 */
#include "fanpro/history.h"

#include "fanpro/log.h"

#include <math.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

bool
fanpro_alert_should_fire(double value_c, double threshold_c,
                         fanpro_alert_state_t *st, double now_s)
{
	if (st == NULL || !isfinite(value_c) || !isfinite(threshold_c))
		return false;

	if (value_c < threshold_c) {
		/*
		 * Re-arm only once it is clearly back below.  Clearing exactly
		 * at the threshold would let a value hovering on the line fire
		 * again every time it wobbles across.
		 */
		if (st->firing && value_c <= threshold_c - FANPRO_ALERT_CLEAR_MARGIN_C)
			st->firing = false;
		return false;
	}

	if (!st->firing) {
		st->firing = true;
		st->last_fired_s = now_s;
		st->fire_count++;
		return true;
	}

	/* Still over.  Repeat occasionally so a sustained problem does not go
	 * silent, but not often enough to become noise. */
	if (now_s - st->last_fired_s >= FANPRO_ALERT_REPEAT_S) {
		st->last_fired_s = now_s;
		st->fire_count++;
		return true;
	}
	return false;
}

/*
 * Post a macOS notification as the logged-in user.
 *
 * posix_spawn with an explicit argv, NOT system().  This runs as root, and
 * the body is built from a config-supplied string; routing that through a
 * shell was a root command-injection hole.  The earlier blocklist was worse
 * than useless: the text landed inside a single-quoted shell string, where
 * the ONLY character with meaning is the single quote, and that was the one
 * character it did not strip.
 *
 * With an explicit argv there is no shell, so no quoting rules to get wrong.
 * The AppleScript string literal still needs its own escaping, but that is a
 * small, well-defined problem rather than an open-ended one.
 *
 * Delivery is fire-and-forget.  Waiting would block the control loop, and a
 * wedged WindowServer can hang osascript for many seconds: long enough for
 * the heartbeat to conclude the loop had died and release the fans over a
 * notification.
 */
static void
applescript_quote(const char *in, char *out, size_t out_len)
{
	size_t o = 0;
	size_t i;

	for (i = 0; in[i] != '\0' && o + 2 < out_len; i++) {
		/* Inside an AppleScript double-quoted literal only these two
		 * need escaping; everything else is inert. */
		if (in[i] == '"' || in[i] == '\\')
			out[o++] = '\\';
		else if (in[i] == '\n' || in[i] == '\r')
			continue;
		out[o++] = in[i];
	}
	out[o] = '\0';
}

static void
notify_console_user(const char *title, const char *body)
{
	char script[768];
	char q_title[192];
	char q_body[384];
	char uid_str[32];
	struct stat st;
	pid_t pid;
	char *argv[8];

	/* Who is at the keyboard?  root has no GUI session of its own. */
	if (stat("/dev/console", &st) != 0) {
		FANPRO_DEBUG("alert.notify", "reason=no_console");
		return;
	}
	snprintf(uid_str, sizeof(uid_str), "%u", (unsigned)st.st_uid);

	applescript_quote(title, q_title, sizeof(q_title));
	applescript_quote(body, q_body, sizeof(q_body));
	snprintf(script, sizeof(script),
	         "display notification \"%s\" with title \"%s\"", q_body, q_title);

	argv[0] = (char *)"launchctl";
	argv[1] = (char *)"asuser";
	argv[2] = uid_str;
	argv[3] = (char *)"osascript";
	argv[4] = (char *)"-e";
	argv[5] = script;
	argv[6] = NULL;

	if (posix_spawn(&pid, "/bin/launchctl", NULL, NULL, argv, environ) != 0) {
		FANPRO_DEBUG("alert.notify", "reason=spawn_failed");
		return;
	}
	/* Deliberately not waited on.  The daemon sets SIGCHLD to SIG_IGN so
	 * these are reaped automatically rather than becoming zombies. */
}

void
fanpro_alerts_evaluate(const fanpro_config_t *cfg,
                       const fanpro_sensor_set_t *sensors,
                       fanpro_alert_state_t *states, double now_s)
{
	int i;

	if (cfg == NULL || sensors == NULL || states == NULL)
		return;

	for (i = 0; i < cfg->n_alerts && i < FANPRO_MAX_ALERTS; i++) {
		const fanpro_alert_t *rule = &cfg->alerts[i];
		double value;

		if (rule->use_class)
			value = fanpro_sensors_max_of_class(sensors, rule->cls);
		else
			value = fanpro_sensors_max_matching(sensors, rule->match);

		if (!fanpro_alert_should_fire(value, rule->above_c, &states[i], now_s))
			continue;

		FANPRO_WARN("alert.fired", "rule=%s value=%.1f threshold=%.1f count=%d",
		            rule->name, value, rule->above_c, states[i].fire_count);

		if (rule->notify) {
			char body[256];

			snprintf(body, sizeof(body), "%s is %.1f C (limit %.0f C)",
			         rule->use_class
			             ? fanpro_sensor_class_name(rule->cls)
			             : rule->match,
			         value, rule->above_c);
			notify_console_user("fanpro", body);
		}
	}
}
