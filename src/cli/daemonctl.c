/*
 * fanpro - the subcommands that talk to fanprod.
 *
 * These are thin on purpose.  All policy lives in the daemon, which is the
 * only process that may touch the SMC; this file formats a request, prints
 * the reply, and gets out of the way.
 */
#include "fanpro/cli.h"

#include "fanpro/fan.h"
#include "fanpro/proto.h"
#include "fanpro/sensors.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLIST_SRC  "launchd/pro.fanpro.daemon.plist"
#define PLIST_DST  "/Library/LaunchDaemons/pro.fanpro.daemon.plist"
#define LAUNCH_LABEL "system/pro.fanpro.daemon"

static int
call(const fanpro_request_t *req, fanpro_response_t *resp)
{
	char err[256];

	if (fanpro_ipc_call(req, resp, err, sizeof(err)) != 0) {
		fprintf(stderr, "fanpro: %s\n", err);
		return 1;
	}
	return 0;
}

int
fanpro_cmd_status(int argc, char **argv)
{
	fanpro_request_t req;
	fanpro_response_t resp;
	int i;

	(void)argc; (void)argv;
	fanpro_request_init(&req, FANPRO_VERB_STATUS);
	if (call(&req, &resp) != 0)
		return 1;

	printf("mode        %s\n", resp.mode == 1 ? "curve" : "auto");
	printf("uptime      %.0f s\n", resp.uptime_s);
	printf("power       CPU %.2f W, GPU %.2f W\n", resp.cpu_watts,
	       resp.gpu_watts);

	if (resp.latched) {
		puts("");
		puts("LATCHED: the previous run exited uncleanly, so fanprod is");
		puts("         staying in monitor-only mode. re-enable with:");
		puts("           sudo fanpro daemon enable");
	}

	puts("");
	printf("%-4s %8s %8s %8s  %-7s %-6s %s\n", "fan", "rpm", "min", "max",
	       "mode", "held", "curve");
	for (i = 0; i < resp.n_fans; i++) {
		const fanpro_fan_status_t *f = &resp.fans[i];

		printf("%-4d %8.0f %8.0f %8.0f  %-7s %-6s %s\n", f->index, f->rpm,
		       f->min_rpm, f->max_rpm, fanpro_fan_mode_name(f->mode),
		       f->held ? "yes" : "no",
		       f->curve[0] ? f->curve : "-");
	}

	if (resp.n_sensors > 0) {
		puts("");
		for (i = 0; i < resp.n_sensors; i++)
			printf("%-10s %6.1f C\n", resp.sensors[i].name,
			       resp.sensors[i].celsius);
	}
	return 0;
}

int
fanpro_cmd_set(int argc, char **argv)
{
	fanpro_request_t req;
	fanpro_response_t resp;

	if (argc < 3) {
		fputs("usage: fanpro set <fan N|all> <rpm|auto>\n", stderr);
		return 2;
	}

	fanpro_request_init(&req, FANPRO_VERB_SET_FAN);

	if (strcmp(argv[1], "all") == 0) {
		req.fan_index = -1;
	} else {
		char *end = NULL;
		long v = strtol(argv[1], &end, 10);

		/* atoi would turn "banana" into fan 0 without complaint. */
		if (end == argv[1] || *end != '\0' || v < 0 || v > 7) {
			fprintf(stderr, "fanpro: '%s' is not a fan number\n", argv[1]);
			return 2;
		}
		req.fan_index = (int)v;
	}

	if (strcmp(argv[2], "auto") == 0) {
		req.rpm = NAN; /* the wire encoding of "hand it back" */
	} else {
		char *end = NULL;
		double v = strtod(argv[2], &end);

		/* And atof would turn it into 0 rpm, which is worse. */
		if (end == argv[2] || *end != '\0' || !(v >= 0.0) || v > 20000.0) {
			fprintf(stderr, "fanpro: '%s' is not an rpm value\n", argv[2]);
			return 2;
		}
		req.rpm = v;
	}

	if (call(&req, &resp) != 0)
		return 1;

	if (isnan(req.rpm)) {
		puts("released to firmware control.");
	} else {
		printf("set to %.0f rpm.\n", req.rpm);
		/* Say this every time: a user who expects it to stick and finds
		 * it gone after a reboot will assume fanpro is broken. */
		puts("note: manual overrides are volatile and do not survive a");
		puts("      daemon restart. put it in /etc/fanpro/fanpro.conf to");
		puts("      make it permanent.");
	}
	return 0;
}

int
fanpro_cmd_mode(int argc, char **argv)
{
	fanpro_request_t req;
	fanpro_response_t resp;

	if (argc < 2) {
		fputs("usage: fanpro mode <auto|curve>\n", stderr);
		return 2;
	}

	fanpro_request_init(&req, FANPRO_VERB_SET_MODE);
	if (strcmp(argv[1], "auto") == 0) {
		req.ival = 0;
	} else if (strcmp(argv[1], "curve") == 0) {
		req.ival = 1;
	} else {
		fputs("fanpro: mode must be auto or curve\n", stderr);
		return 2;
	}

	if (call(&req, &resp) != 0)
		return 1;
	printf("mode set to %s.\n", argv[1]);
	if (req.ival == 1) {
		puts("");
		puts("fanpro now drives the fans. on this hardware the firmware does");
		puts("NOT override a fan held in manual mode, so fanpro's safety");
		puts("layer is the only protection those fans have.");
	}
	return 0;
}

static int
run(const char *fmt, ...)
{
	char cmd[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(cmd, sizeof(cmd), fmt, ap);
	va_end(ap);
	return system(cmd);
}

int
fanpro_cmd_daemon(int argc, char **argv)
{
	fanpro_request_t req;
	fanpro_response_t resp;

	if (argc < 2) {
		fputs("usage: fanpro daemon <status|install|uninstall|reload|"
		      "enable|release>\n", stderr);
		return 2;
	}

	if (strcmp(argv[1], "status") == 0)
		return fanpro_cmd_status(argc - 1, argv + 1);

	if (strcmp(argv[1], "install") == 0) {
		if (geteuid() != 0) {
			fputs("fanpro: daemon install needs sudo\n", stderr);
			return 1;
		}
		if (access(PLIST_SRC, R_OK) != 0) {
			fprintf(stderr, "fanpro: %s not found; run from the source "
			        "tree or install it by hand\n", PLIST_SRC);
			return 1;
		}
		run("install -m 0644 -o root -g wheel %s %s", PLIST_SRC, PLIST_DST);
		run("launchctl bootstrap system %s 2>/dev/null", PLIST_DST);
		run("launchctl enable %s", LAUNCH_LABEL);
		puts("installed. fanprod starts at boot.");
		puts("it is in monitor-only mode until you set mode = curve in");
		puts("/etc/fanpro/fanpro.conf, which is deliberate.");
		return 0;
	}

	if (strcmp(argv[1], "uninstall") == 0) {
		if (geteuid() != 0) {
			fputs("fanpro: daemon uninstall needs sudo\n", stderr);
			return 1;
		}
		/* Full cleanup: a half-removed daemon is worse than none. */
		run("launchctl bootout %s 2>/dev/null", LAUNCH_LABEL);
		run("rm -f %s", PLIST_DST);
		run("rm -f %s", FANPRO_SOCKET_PATH);
		puts("uninstalled. fans are back under firmware control.");
		puts("logs left in /var/log/fanpro/.");
		return 0;
	}

	if (strcmp(argv[1], "reload") == 0) {
		fanpro_request_init(&req, FANPRO_VERB_RELOAD);
	} else if (strcmp(argv[1], "enable") == 0) {
		fanpro_request_init(&req, FANPRO_VERB_ENABLE);
	} else if (strcmp(argv[1], "release") == 0) {
		fanpro_request_init(&req, FANPRO_VERB_RELEASE_ALL);
	} else {
		fprintf(stderr, "fanpro: unknown daemon command '%s'\n", argv[1]);
		return 2;
	}

	if (call(&req, &resp) != 0)
		return 1;
	/* Bounded: the reply comes off a socket, so do not assume the
	 * message field is NUL terminated. */
	printf("%.*s\n", (int)sizeof(resp.message),
	       resp.message[0] ? resp.message : "ok");
	return 0;
}
