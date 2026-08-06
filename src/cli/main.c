/*
 * fanpro - command line entry point.
 *
 * Read paths run in-process and unprivileged.  Anything that mutates fan
 * state is refused here and belongs to fanprod; this binary never opens a
 * write path, which is why it is safe to run as a normal user.
 */
#include "fanpro/cli.h"

#include "fanpro/fan.h"
#include "fanpro/log.h"
#include "fanpro/power.h"
#include "fanpro/sensors.h"
#include "fanpro/smc.h"
#include "fanpro/smc_codec.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(void)
{
	fputs(
	    "usage: fanpro <command> [args]\n"
	    "\n"
	    "read-only, no privileges required:\n"
	    "  fans                 per-fan rpm, limits, mode, target\n"
	    "  sensors [--json] [--all]\n"
	    "                       temperature sensors, grouped by class.\n"
	    "                       --all also shows the unvalidated SMC T* keys\n"
	    "  power                CPU/GPU/ANE watts\n"
	    "  disk                 NVMe temperature and health counters\n"
	    "  smc dump             every SMC key with type and value\n"
	    "  smc get <key>        one key\n"
	    "\n"
	    "talks to the fanprod daemon (admin group or sudo):\n"
	    "  status               daemon, fan and sensor state\n"
	    "  set <fan N|all> <rpm|auto>\n"
	    "  mode <auto|curve>    monitor only, or drive the fans\n"
	    "  daemon status|install|uninstall|reload|enable|release\n"
	    "  log tail [lines]     recent daemon log\n"
	    "  history [n|--csv]    recorded samples\n"
	    "\n"
	    "  top                  live dashboard (works without the daemon)\n"
	    "\n"
	    "requires root (writes to the SMC):\n"
	    "  smc probe [--fan N] [--rpm N]\n"
	    "                       determine how manual fan control behaves on\n"
	    "                       this Mac, then release it again\n"
	    "  smc hold [--fan N] [--rpm N] [--seconds N] [--load N]\n"
	    "                       hold a fan under load and report whether the\n"
	    "                       firmware still overrides it. makes the machine\n"
	    "                       hot; aborts on temperature or thermal pressure\n"
	    "\n"
	    "options:\n"
	    "  -v                   verbose (repeat for trace)\n"
	    "  -h                   this help\n",
	    stderr);
}

static int
open_smc(fanpro_smc_t *smc)
{
	if (fanpro_smc_open(smc, fanpro_smc_backend_iokit()) != 0) {
		fputs("fanpro: cannot open AppleSMC\n", stderr);
		return -1;
	}
	return 0;
}

static int
cmd_fans(fanpro_smc_t *smc)
{
	fanpro_fan_set_t set;
	int i;

	if (fanpro_fan_enumerate(smc, &set) != 0) {
		fputs("fanpro: fan enumeration failed\n", stderr);
		return 1;
	}

	if (set.count == 0) {
		puts("no fans reported by the SMC");
		return 0;
	}

	printf("%-4s %8s %8s %8s %8s  %s\n", "fan", "rpm", "min", "max",
	       "target", "mode");
	for (i = 0; i < set.count; i++) {
		const fanpro_fan_t *f = &set.fans[i];

		printf("%-4d %8.0f %8.0f %8.0f %8.0f  %s\n", f->index, f->rpm,
		       f->min_rpm, f->max_rpm, f->target,
		       fanpro_fan_mode_name(f->mode));
	}

	printf("\nmode key: %s   Ftst: %s",
	       set.lowercase_mode ? "F%dmd" : "F%dMd",
	       set.has_ftst ? "present" : "absent");
	if (set.ftst >= 0)
		printf(" (=%d)", set.ftst);
	putchar('\n');
	return 0;
}

static int
cmd_sensors(fanpro_smc_t *smc, bool as_json, bool force_smc)
{
	fanpro_sensor_set_t set;
	int cls, i, shown = 0;

	if (fanpro_sensors_enumerate(&set, smc, force_smc) != 0) {
		fputs("fanpro: sensor enumeration failed\n", stderr);
		return 1;
	}

	if (as_json) {
		putchar('[');
		for (i = 0; i < set.count; i++) {
			const fanpro_sensor_t *s = &set.sensors[i];

			printf("%s\n  {\"name\":\"%s\",\"celsius\":%.2f,"
			       "\"class\":\"%s\",\"source\":\"%s\"}",
			       i ? "," : "", s->name, s->celsius,
			       fanpro_sensor_class_name(s->cls),
			       s->source == FANPRO_SRC_HID ? "hid" : "smc");
		}
		puts(set.count ? "\n]" : "]");
		return 0;
	}

	if (set.count == 0) {
		puts("no temperature sensors available");
		printf("  hid provider: %s\n", set.hid_available ? "ok" : "unavailable");
		printf("  smc provider: %s\n", set.smc_available ? "ok" : "unavailable");
		return 1;
	}

	/* Grouped by class, because that is how the safety thresholds are
	 * grouped and it makes an outlier obvious at a glance. */
	for (cls = 0; cls < FANPRO_CLASS_COUNT; cls++) {
		bool header = false;

		for (i = 0; i < set.count; i++) {
			const fanpro_sensor_t *s = &set.sensors[i];

			if (!s->valid || (int)s->cls != cls)
				continue;
			if (!header) {
				printf("%s%s\n", shown ? "\n" : "",
				       fanpro_sensor_class_name((fanpro_sensor_class_t)cls));
				header = true;
			}
			printf("  %-40s %7.2f C  [%s]\n", s->name, s->celsius,
			       s->source == FANPRO_SRC_HID ? "hid" : "smc");
			shown++;
		}
	}

	printf("\n%d sensors (hid: %s%s)\n", shown,
	       set.hid_available ? "ok" : "unavailable",
	       set.smc_used ? ", smc fallback in use" : "");
	if (set.dropped > 0)
		printf("%d sensors dropped: table full at %d\n", set.dropped,
		       FANPRO_MAX_SENSORS);
	if (!set.smc_used && set.smc_available)
		puts("(SMC T* keys hidden; --all includes them, unvalidated)");
	return 0;
}

static int
cmd_power(void)
{
	fanpro_power_set_t set;
	int i;

	if (fanpro_power_sample(&set, 300) != 0) {
		puts("power readings unavailable");
		puts("  IOReport is private SPI loaded at runtime; it may have");
		puts("  moved or been renamed in this macOS release.");
		return 1;
	}

	printf("%-40s %8s\n", "channel", "watts");
	for (i = 0; i < set.count; i++)
		printf("%-40s %8.2f\n", set.channels[i].name, set.channels[i].watts);
	printf("\n%-40s %8.2f\n", "total", set.total_watts);
	return 0;
}

static int
cmd_disk(fanpro_smc_t *smc)
{
	fanpro_disk_health_t h;

	if (fanpro_disk_health_read(&h) != 0 || !h.available) {
		fanpro_sensor_set_t set;
		double nand;

		if (h.smart_unsupported)
			puts("SMART counters: unsupported by this NVMe controller.");
		else if (h.needed_root)
			puts("SMART counters: refused for lack of privilege; try sudo.");
		else
			puts("SMART counters: unavailable.");

		/* The controller may expose nothing, but the NAND temperature is
		 * still published as a thermal sensor, and that is the part that
		 * matters for fan control. */
		if (fanpro_sensors_enumerate(&set, smc, false) == 0) {
			nand = fanpro_sensors_max_of_class(&set, FANPRO_CLASS_NAND);
			if (isfinite(nand)) {
				printf("\ntemperature       %.1f C  (from thermal sensor)\n",
				       nand);
				return 0;
			}
		}
		puts("no NAND temperature sensor either");
		return 1;
	}

	printf("temperature       %.1f C\n", h.celsius);
	printf("endurance used    %u%%\n", h.percent_used);
	printf("power-on hours    %llu\n", (unsigned long long)h.power_on_hours);
	printf("unsafe shutdowns  %llu\n", (unsigned long long)h.unsafe_shutdowns);
	printf("data read         %.2f TB\n",
	       (double)h.data_units_read * 512000.0 / 1e12);
	printf("data written      %.2f TB\n",
	       (double)h.data_units_written * 512000.0 / 1e12);
	printf("critical warning  0x%02x%s\n", h.critical_warning,
	       h.critical_warning ? "  <- NOT CLEAR" : "");
	printf("read as root      %s\n", geteuid() == 0 ? "yes" : "no");
	return 0;
}

static void
print_value(uint32_t key, const fanpro_smc_value_t *v)
{
	char kname[5], tname[5];
	uint32_t i;

	fanpro_fourcc_str(key, kname);
	fanpro_fourcc_str(v->type, tname);

	printf("%-5s %-5s %2u  ", kname, tname, v->size);
	if (v->numeric)
		printf("%12.3f  ", v->num);
	else
		printf("%12s  ", "-");

	for (i = 0; i < v->raw_len; i++)
		printf("%02x", v->raw[i]);
	putchar('\n');
}

static int
cmd_smc_dump(fanpro_smc_t *smc)
{
	uint32_t count = 0, i;

	if (fanpro_smc_key_count(smc, &count) != FANPRO_SMC_SUCCESS) {
		fputs("fanpro: cannot read #KEY\n", stderr);
		return 1;
	}

	printf("%-5s %-5s %2s  %12s  %s\n", "key", "type", "sz", "value", "raw");
	for (i = 0; i < count; i++) {
		fanpro_smc_value_t v;
		uint32_t key = 0;

		if (fanpro_smc_key_at(smc, i, &key) != FANPRO_SMC_SUCCESS)
			continue;
		if (fanpro_smc_read(smc, key, &v) != FANPRO_SMC_SUCCESS)
			continue;
		print_value(key, &v);
	}
	printf("\n%u keys\n", count);
	return 0;
}

static int
cmd_smc_get(fanpro_smc_t *smc, const char *name)
{
	fanpro_smc_value_t v;
	char padded[5] = "    ";
	size_t n;
	int rc;

	n = strlen(name);
	if (n == 0 || n > 4) {
		fputs("fanpro: key must be 1-4 characters\n", stderr);
		return 2;
	}
	memcpy(padded, name, n);

	rc = fanpro_smc_read(smc, fanpro_fourcc(padded), &v);
	if (rc == FANPRO_SMC_NOT_FOUND) {
		printf("%s: not present on this machine\n", padded);
		return 1;
	}
	if (rc != FANPRO_SMC_SUCCESS) {
		printf("%s: read failed (result 0x%02x)\n", padded, rc);
		return 1;
	}
	print_value(fanpro_fourcc(padded), &v);
	return 0;
}

int
main(int argc, char **argv)
{
	fanpro_log_level_t level = FANPRO_LOG_WARN;
	fanpro_smc_t smc;
	int opt, rc;

	while ((opt = getopt(argc, argv, "+vh")) != -1) {
		switch (opt) {
		case 'v':
			if (level < FANPRO_LOG_TRACE)
				level++;
			break;
		case 'h':
			usage();
			return 0;
		default:
			usage();
			return 2;
		}
	}
	fanpro_log_set_level(level);

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage();
		return 2;
	}

	if (open_smc(&smc) != 0)
		return 1;

	if (strcmp(argv[0], "fans") == 0) {
		rc = cmd_fans(&smc);
	} else if (strcmp(argv[0], "sensors") == 0) {
		bool js = false, all = false;
		int a;

		for (a = 1; a < argc; a++) {
			if (strcmp(argv[a], "--json") == 0)
				js = true;
			else if (strcmp(argv[a], "--all") == 0)
				all = true;
		}
		rc = cmd_sensors(&smc, js, all);
	} else if (strcmp(argv[0], "status") == 0) {
		rc = fanpro_cmd_status(argc, argv);
	} else if (strcmp(argv[0], "set") == 0) {
		rc = fanpro_cmd_set(argc, argv);
	} else if (strcmp(argv[0], "mode") == 0) {
		rc = fanpro_cmd_mode(argc, argv);
	} else if (strcmp(argv[0], "top") == 0) {
		rc = fanpro_cmd_top(&smc, argc, argv);
	} else if (strcmp(argv[0], "log") == 0) {
		rc = fanpro_cmd_log(argc, argv);
	} else if (strcmp(argv[0], "history") == 0) {
		rc = fanpro_cmd_history(argc, argv);
	} else if (strcmp(argv[0], "daemon") == 0) {
		rc = fanpro_cmd_daemon(argc, argv);
	} else if (strcmp(argv[0], "power") == 0) {
		rc = cmd_power();
	} else if (strcmp(argv[0], "disk") == 0) {
		rc = cmd_disk(&smc);
	} else if (strcmp(argv[0], "smc") == 0 && argc >= 2 &&
	           strcmp(argv[1], "dump") == 0) {
		rc = cmd_smc_dump(&smc);
	} else if (strcmp(argv[0], "smc") == 0 && argc >= 3 &&
	           strcmp(argv[1], "get") == 0) {
		rc = cmd_smc_get(&smc, argv[2]);
	} else if (strcmp(argv[0], "smc") == 0 && argc >= 2 &&
	           strcmp(argv[1], "probe") == 0) {
		rc = fanpro_cmd_probe(&smc, argc - 1, argv + 1);
	} else if (strcmp(argv[0], "smc") == 0 && argc >= 2 &&
	           strcmp(argv[1], "hold") == 0) {
		rc = fanpro_cmd_hold(&smc, argc - 1, argv + 1);
	} else {
		usage();
		rc = 2;
	}

	fanpro_smc_close(&smc);
	return rc;
}
