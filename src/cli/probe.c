/*
 * fanpro - `smc probe`: determine how manual fan control behaves here.
 *
 * This is the de-risking spike for the whole project.  The public research
 * validated the Ftst unlock on M4 and found M1/M5 accept a direct mode write,
 * but lists M3 and Mac Studio as untested.  Rather than assume either way,
 * this command tries the cheap path first, falls back to the unlock, drives
 * one fan to a modest speed, and checks that the measured RPM actually moves.
 *
 * "The write returned success" is not evidence.  A firmware in system mode
 * accepts target writes and ignores them, which is exactly why naive tools
 * appear to work.  Convergence of F%dAc is the only proof that counts.
 *
 * The fan is always released before this command returns, including on
 * SIGINT/SIGTERM, because leaving a fan pinned by a dead process is the one
 * outcome worse than not having manual control at all.
 */
#include "fanpro/cli.h"

#include "fanpro/fan.h"
#include "fanpro/power.h"
#include "fanpro/safety.h"
#include "fanpro/sensors.h"
#include "fanpro/smc.h"
#include "fanpro/smc_codec.h"

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define YIELD_POLL_MS     100
#define YIELD_TIMEOUT_MS  10000
#define SETTLE_TIMEOUT_MS 20000
#define SETTLE_POLL_MS    500
/* Fans coast; anything inside this band counts as having reached the target. */
#define CONVERGE_TOLERANCE_RPM 150.0

/*
 * The hold experiment only proves something if the firmware demonstrably
 * wanted more airflow during the run.  Absolute temperature is a poor test of
 * that: a run starting on an already-warm machine with fans spun up can stay
 * flat while the firmware is working hard.
 *
 * The control fan is the better instrument.  It is on auto, so how far the
 * firmware ramps it IS the firmware's stated intent.  If it ramps hard while
 * the held fan stays at the commanded speed, the firmware clearly wanted
 * airflow and could not get it from the fan we hold.  Temperature is kept as
 * a secondary route to the same conclusion.
 */
#define HOLD_CONTROL_RAMP_RPM  500.0
#define HOLD_MEANINGFUL_TEMP_C 70.0
#define HOLD_MEANINGFUL_RISE_C 15.0

/*
 * A fan does not reach a commanded speed instantly, and it may start the run
 * well above it if the machine was already warm.  Samples before the fan has
 * first settled at the commanded speed say nothing about whether the firmware
 * overrode us: an earlier version of this experiment reported a spin-down
 * reading of 1210 rpm as a firmware boost.  Require convergence first, then a
 * sustained excursion rather than one noisy sample.
 */
#define HOLD_SETTLE_SECONDS 30
#define HOLD_BOOST_SAMPLES  3

/* Release state, reachable from the signal handler. */
static struct {
	fanpro_smc_t *smc;
	uint32_t      mode_key;
	bool          mode_forced;
	bool          ftst_set;
	volatile sig_atomic_t interrupted;
} g_rel;

static void
sleep_ms(unsigned ms)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	nanosleep(&ts, NULL);
}

static bool
read_num(fanpro_smc_t *smc, uint32_t key, double *out)
{
	fanpro_smc_value_t v;

	if (fanpro_smc_read(smc, key, &v) != FANPRO_SMC_SUCCESS || !v.numeric)
		return false;
	*out = v.num;
	return true;
}

/*
 * Hand the fan back.  Safe to call more than once, and safe to call with
 * nothing acquired.  Writes are retried because this is the one path whose
 * failure leaves hardware in a bad state.
 */
static void
release_all(void)
{
	int attempt;

	if (g_rel.smc == NULL)
		return;

	if (g_rel.mode_forced) {
		/* Only the mode is restored.  In auto the firmware owns the
		 * target and overwrites whatever we left there, so writing it
		 * back would be noise on the safety-critical path. */
		for (attempt = 0; attempt < 3; attempt++) {
			int rc = fanpro_smc_write_num(g_rel.smc, g_rel.mode_key,
			                              FANPRO_FAN_MODE_AUTO);

			if (rc == FANPRO_SMC_SUCCESS ||
			    rc == FANPRO_SMC_KEY_SIZE_MISMATCH)
				break;
			sleep_ms(100);
		}
		g_rel.mode_forced = false;
	}

	if (g_rel.ftst_set) {
		for (attempt = 0; attempt < 3; attempt++) {
			int rc = fanpro_smc_write_num(g_rel.smc,
			                              fanpro_fourcc("Ftst"), 0.0);

			if (rc == FANPRO_SMC_SUCCESS ||
			    rc == FANPRO_SMC_KEY_SIZE_MISMATCH)
				break;
			sleep_ms(100);
		}
		g_rel.ftst_set = false;
	}
}

static void
on_signal(int sig)
{
	(void)sig;
	g_rel.interrupted = 1;
	release_all();
	_exit(130);
}

static void
install_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	atexit(release_all);
}

/*
 * Wait for thermalmonitord to drop the fan out of system mode after Ftst is
 * asserted.  Returns true once the mode is no longer 3.
 */
static bool
wait_for_yield(fanpro_smc_t *smc, uint32_t mode_key, unsigned *waited_ms)
{
	unsigned elapsed = 0;

	while (elapsed < YIELD_TIMEOUT_MS) {
		double mode = -1.0;

		if (read_num(smc, mode_key, &mode) &&
		    (int)mode != FANPRO_FAN_MODE_SYSTEM) {
			*waited_ms = elapsed;
			return true;
		}
		if (g_rel.interrupted)
			return false;
		sleep_ms(YIELD_POLL_MS);
		elapsed += YIELD_POLL_MS;
	}
	*waited_ms = elapsed;
	return false;
}

/* Did the fan actually reach the commanded speed? */
static bool
wait_for_convergence(fanpro_smc_t *smc, uint32_t ac_key, double want,
                     double *final_rpm, unsigned *waited_ms)
{
	unsigned elapsed = 0;
	double rpm = -1.0;

	while (elapsed < SETTLE_TIMEOUT_MS) {
		if (read_num(smc, ac_key, &rpm)) {
			*final_rpm = rpm;
			if (rpm >= want - CONVERGE_TOLERANCE_RPM &&
			    rpm <= want + CONVERGE_TOLERANCE_RPM) {
				*waited_ms = elapsed;
				return true;
			}
		}
		if (g_rel.interrupted)
			return false;
		printf("    %5u ms  rpm=%.0f (target %.0f)\n", elapsed, rpm, want);
		fflush(stdout);
		sleep_ms(SETTLE_POLL_MS);
		elapsed += SETTLE_POLL_MS;
	}
	*waited_ms = elapsed;
	*final_rpm = rpm;
	return false;
}

int
fanpro_cmd_probe(fanpro_smc_t *smc, int argc, char **argv)
{
	fanpro_fan_set_t set;
	fanpro_fan_t *fan;
	double want_rpm = -1.0;
	double final_rpm = -1.0;
	double mode_now = -1.0;
	unsigned waited = 0;
	int fan_idx = 0;
	int rc, i;
	bool manual = false;
	const char *path = "none";

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fan") == 0 && i + 1 < argc)
			fan_idx = atoi(argv[++i]);
		else if (strcmp(argv[i], "--rpm") == 0 && i + 1 < argc)
			want_rpm = atof(argv[++i]);
		else {
			fputs("usage: fanpro smc probe [--fan N] [--rpm N]\n", stderr);
			return 2;
		}
	}

	if (fanpro_fan_enumerate(smc, &set) != 0) {
		fputs("fanpro: fan enumeration failed\n", stderr);
		return 1;
	}

	printf("machine survey\n");
	printf("  fans        : %d\n", set.count);
	printf("  mode key    : %s\n", set.lowercase_mode ? "F%dmd" : "F%dMd");
	printf("  Ftst        : %s\n", set.has_ftst ? "present" : "absent");
	if (set.ftst >= 0)
		printf("  Ftst value  : %d\n", set.ftst);
	for (i = 0; i < set.count; i++) {
		const fanpro_fan_t *f = &set.fans[i];

		printf("  fan %d       : %.0f rpm, range %.0f-%.0f, target %.0f, mode %s\n",
		       i, f->rpm, f->min_rpm, f->max_rpm, f->target,
		       fanpro_fan_mode_name(f->mode));
	}
	putchar('\n');

	if (set.count == 0) {
		puts("verdict: no fans, nothing to control");
		return 0;
	}
	if (fan_idx < 0 || fan_idx >= set.count) {
		fprintf(stderr, "fanpro: fan %d out of range (0-%d)\n", fan_idx,
		        set.count - 1);
		return 2;
	}

	fan = &set.fans[fan_idx];
	if (!fan->mode_key_known) {
		puts("verdict: no mode key for this fan; manual control is not "
		     "available on this machine");
		return 1;
	}
	if (fan->min_rpm < 0.0 || fan->max_rpm <= fan->min_rpm) {
		puts("verdict: fan limits unreadable; refusing to drive a fan "
		     "whose safe range is unknown");
		return 1;
	}

	if (geteuid() != 0) {
		puts("verdict: reads work, but SMC writes need root. "
		     "re-run with sudo to test manual control.");
		return 1;
	}

	/* Pick a deliberately modest speed: 40% into the fan's own range, so
	 * the test is audible-but-harmless and always inside firmware limits. */
	if (want_rpm < 0.0)
		want_rpm = fan->min_rpm + 0.40 * (fan->max_rpm - fan->min_rpm);
	if (want_rpm < fan->min_rpm)
		want_rpm = fan->min_rpm;
	if (want_rpm > fan->max_rpm)
		want_rpm = fan->max_rpm;

	g_rel.smc = smc;
	g_rel.mode_key = fan->key_md;
	install_handlers();

	/* --- phase 1: the cheap path ------------------------------------ */
	printf("phase 1: direct mode write\n");
	rc = fanpro_smc_write_num(smc, fan->key_md, FANPRO_FAN_MODE_MANUAL);
	printf("  write %s = 1 -> result 0x%02x%s\n",
	       set.lowercase_mode ? "F%dmd" : "F%dMd",
	       rc < 0 ? 0xff : (unsigned)rc,
	       rc < 0 ? " (transport failure)" : "");

	if (rc >= 0 && read_num(smc, fan->key_md, &mode_now) &&
	    (int)mode_now == FANPRO_FAN_MODE_MANUAL) {
		manual = true;
		path = "direct";
		g_rel.mode_forced = true;
		puts("  mode reads back as manual");
	} else if (rc == FANPRO_SMC_BAD_COMMAND) {
		puts("  rejected with 0x82 (firmware holds the fan in system mode)");
	} else if (rc < 0) {
		puts("  transport failure; is this running as root?");
		return 1;
	} else {
		printf("  write reported 0x%02x but mode reads %s\n",
		       (unsigned)rc, fanpro_fan_mode_name((int)mode_now));
	}

	/* --- phase 2: the Ftst unlock ----------------------------------- */
	if (!manual) {
		putchar('\n');
		printf("phase 2: Ftst unlock\n");
		if (!set.has_ftst) {
			puts("  Ftst is absent on this machine; no fallback available");
		} else {
			rc = fanpro_smc_write_num(smc, fanpro_fourcc("Ftst"), 1.0);
			printf("  write Ftst = 1 -> result 0x%02x\n",
			       rc < 0 ? 0xff : (unsigned)rc);
			if (rc == FANPRO_SMC_SUCCESS ||
			    rc == FANPRO_SMC_KEY_SIZE_MISMATCH) {
				g_rel.ftst_set = true;

				if (wait_for_yield(smc, fan->key_md, &waited)) {
					printf("  firmware yielded after %u ms\n",
					       waited);
					rc = fanpro_smc_write_num(smc, fan->key_md,
					                          FANPRO_FAN_MODE_MANUAL);
					printf("  write mode = 1 -> result 0x%02x\n",
					       rc < 0 ? 0xff : (unsigned)rc);
					if (rc >= 0 &&
					    read_num(smc, fan->key_md, &mode_now) &&
					    (int)mode_now == FANPRO_FAN_MODE_MANUAL) {
						manual = true;
						path = "ftst";
						g_rel.mode_forced = true;
						puts("  mode reads back as manual");
					}
				} else {
					printf("  firmware never yielded (waited %u ms)\n",
					       waited);
				}
			}
		}
	}

	if (!manual) {
		putchar('\n');
		puts("verdict: MANUAL CONTROL NOT AVAILABLE on this machine.");
		puts("         monitoring works; fan control does not.");
		release_all();
		return 1;
	}

	/* --- phase 3: does the fan actually move? ----------------------- */
	putchar('\n');
	printf("phase 3: convergence test at %.0f rpm\n", want_rpm);
	rc = fanpro_smc_write_num(smc, fan->key_tg, want_rpm);
	printf("  write F%dTg = %.0f -> result 0x%02x%s\n", fan_idx, want_rpm,
	       rc < 0 ? 0xff : (unsigned)rc,
	       rc == FANPRO_SMC_KEY_SIZE_MISMATCH ? " (0x87: often applied anyway)" : "");

	if (wait_for_convergence(smc, fan->key_ac, want_rpm, &final_rpm, &waited)) {
		printf("  converged to %.0f rpm after %u ms\n", final_rpm, waited);
		putchar('\n');
		printf("verdict: MANUAL CONTROL WORKS via the %s path.\n", path);
		printf("         fan %d reached %.0f rpm on command.\n", fan_idx,
		       final_rpm);
		rc = 0;
	} else {
		printf("  did NOT converge: still %.0f rpm after %u ms\n",
		       final_rpm, waited);
		putchar('\n');
		puts("verdict: writes are ACCEPTED BUT IGNORED.");
		puts("         the mode key flipped, the target was written, and the");
		puts("         fan did not respond. this is the silent no-op case.");
		rc = 1;
	}

	/* --- phase 4: hand it back -------------------------------------- */
	putchar('\n');
	printf("phase 4: release\n");
	release_all();
	sleep_ms(500);
	if (fanpro_fan_refresh(smc, &set) == 0) {
		printf("  fan %d mode is now %s, %.0f rpm\n", fan_idx,
		       fanpro_fan_mode_name(set.fans[fan_idx].mode),
		       set.fans[fan_idx].rpm);
		if (set.has_ftst)
			printf("  Ftst is now %d\n", set.ftst);
	}
	return rc;
}

/* ---- thermal authority experiment --------------------------------------- */
/*
 * `fanpro smc hold`.  See cli.h for why this exists.
 *
 * It reuses g_rel and release_all() above deliberately: there is exactly one
 * release path in this binary, and it already runs from atexit and from the
 * signal handlers.  Adding a second one would be the easiest way to leave a
 * fan pinned.
 */

/* Load generators, so the experiment is one command rather than two. */
static pid_t g_load_pids[64];
static int   g_load_count;

static void
stop_load(void)
{
	int i;

	for (i = 0; i < g_load_count; i++) {
		if (g_load_pids[i] > 0) {
			kill(g_load_pids[i], SIGKILL);
			waitpid(g_load_pids[i], NULL, 0);
			g_load_pids[i] = 0;
		}
	}
	g_load_count = 0;
}

static void
start_load(int workers)
{
	int i;

	if (workers <= 0)
		return;
	if (workers > (int)(sizeof(g_load_pids) / sizeof(g_load_pids[0])))
		workers = (int)(sizeof(g_load_pids) / sizeof(g_load_pids[0]));

	for (i = 0; i < workers; i++) {
		pid_t pid = fork();

		if (pid == 0) {
			/*
			 * Child: saturate the FP units until killed.
			 *
			 * A naive `volatile double x; x += 1.0;` loop is not
			 * enough.  Measured on this M3 Ultra: 32 such workers
			 * draw 47 W, while the independent multiply-add chains
			 * below over a 512 KB buffer draw 144 W.  The first
			 * cannot heat a Mac Studio meaningfully, which made an
			 * earlier run of this experiment inconclusive.
			 */
			size_t n = 1u << 16;
			double *buf = malloc(n * sizeof(double));
			double s0 = 1, s1 = 2, s2 = 3, s3 = 4;
			double s4 = 5, s5 = 6, s6 = 7, s7 = 8;
			size_t j;

			if (buf == NULL)
				_exit(1);
			for (j = 0; j < n; j++)
				buf[j] = (double)j;

			for (;;) {
				/* Eight independent chains keep the pipelines
				 * full; one dependent chain would stall. */
				for (j = 0; j < n; j += 8) {
					s0 = s0 * 1.0000001 + buf[j];
					s1 = s1 * 1.0000001 + buf[j + 1];
					s2 = s2 * 1.0000001 + buf[j + 2];
					s3 = s3 * 1.0000001 + buf[j + 3];
					s4 = s4 * 1.0000001 + buf[j + 4];
					s5 = s5 * 1.0000001 + buf[j + 5];
					s6 = s6 * 1.0000001 + buf[j + 6];
					s7 = s7 * 1.0000001 + buf[j + 7];
				}
				if (s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7 > 1e300)
					s0 = s1 = s2 = s3 = s4 = s5 = s6 = s7 = 1.0;
			}
		}
		if (pid > 0)
			g_load_pids[g_load_count++] = pid;
	}
	atexit(stop_load);
	printf("  started %d load workers\n", g_load_count);
}

int
fanpro_cmd_hold(fanpro_smc_t *smc, int argc, char **argv)
{
	fanpro_fan_set_t set;
	fanpro_sensor_set_t sensors;
	fanpro_fan_t *fan;
	double want_rpm = -1.0;
	double abort_temp = 95.0;
	double peak_soc = -1.0, peak_held = -1.0, peak_control = -1.0;
	double start_soc = -1.0;
	double start_control = -1.0;
	/* Peak of the held fan counting ONLY judged samples.  peak_held
	 * includes the spin-down at the start and is useless as evidence. */
	double peak_held_judged = -1.0;
	int    boost_first_sec = -1;
	int    converged_sec = -1;
	int fan_idx = 0, control_idx = -1;
	int seconds = 300;
	int workers = -1;
	int elapsed = 0;
	int rc, i;
	bool boosted = false;
	bool converged = false;
	int boost_run = 0;
	const char *stop_reason = "duration elapsed";

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--fan") == 0 && i + 1 < argc)
			fan_idx = atoi(argv[++i]);
		else if (strcmp(argv[i], "--rpm") == 0 && i + 1 < argc)
			want_rpm = atof(argv[++i]);
		else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (strcmp(argv[i], "--abort-temp") == 0 && i + 1 < argc)
			abort_temp = atof(argv[++i]);
		else if (strcmp(argv[i], "--load") == 0 && i + 1 < argc)
			workers = atoi(argv[++i]);
		else if (strcmp(argv[i], "--no-load") == 0)
			workers = 0;
		else {
			fputs("usage: fanpro smc hold [--fan N] [--rpm N] "
			      "[--seconds N]\n"
			      "                       [--abort-temp C] "
			      "[--load N | --no-load]\n", stderr);
			return 2;
		}
	}

	if (geteuid() != 0) {
		fputs("fanpro: smc hold needs root\n", stderr);
		return 1;
	}
	if (fanpro_fan_enumerate(smc, &set) != 0) {
		fputs("fanpro: fan enumeration failed\n", stderr);
		return 1;
	}
	if (fan_idx < 0 || fan_idx >= set.count) {
		fprintf(stderr, "fanpro: fan %d out of range (0-%d)\n", fan_idx,
		        set.count - 1);
		return 2;
	}

	fan = &set.fans[fan_idx];
	if (!fan->mode_key_known || fan->min_rpm < 0.0 ||
	    fan->max_rpm <= fan->min_rpm) {
		fputs("fanpro: fan is not controllable or its limits are unknown\n",
		      stderr);
		return 1;
	}

	/* Hold at the minimum by default: the point is to starve this fan of
	 * airflow and see whether anything underneath us intervenes. */
	if (want_rpm < 0.0)
		want_rpm = fan->min_rpm;
	if (want_rpm < fan->min_rpm)
		want_rpm = fan->min_rpm;
	if (want_rpm > fan->max_rpm)
		want_rpm = fan->max_rpm;

	/* A second fan left on auto is the control: if it ramps while ours
	 * does not, the firmware is clearly still managing thermals and is
	 * simply not allowed to touch the fan we hold. */
	for (i = 0; i < set.count; i++) {
		if (i != fan_idx) {
			control_idx = i;
			break;
		}
	}

	if (workers < 0)
		workers = (int)sysconf(_SC_NPROCESSORS_ONLN);

	printf("thermal authority experiment\n");
	printf("  holding fan %d at %.0f rpm (range %.0f-%.0f)\n", fan_idx,
	       want_rpm, fan->min_rpm, fan->max_rpm);
	if (control_idx >= 0)
		printf("  fan %d left on auto as the control\n", control_idx);
	printf("  duration %d s, abort at %.0f C or thermal pressure heavy\n",
	       seconds, abort_temp);

	g_rel.smc = smc;
	g_rel.mode_key = fan->key_md;
	install_handlers();

	rc = fanpro_smc_write_num(smc, fan->key_md, FANPRO_FAN_MODE_MANUAL);
	if (rc < 0 || read_num(smc, fan->key_md, &(double){ 0 }) == false) {
		fputs("fanpro: could not take manual control\n", stderr);
		release_all();
		return 1;
	}
	{
		double mode = -1.0;

		if (!read_num(smc, fan->key_md, &mode) ||
		    (int)mode != FANPRO_FAN_MODE_MANUAL) {
			fputs("fanpro: mode did not read back as manual\n", stderr);
			release_all();
			return 1;
		}
	}
	g_rel.mode_forced = true;

	rc = fanpro_smc_write_num(smc, fan->key_tg, want_rpm);
	if (rc < 0) {
		fputs("fanpro: could not set the target\n", stderr);
		release_all();
		return 1;
	}

	start_load(workers);
	printf("\n%6s %10s %10s %8s %8s  %s\n", "sec", "held_rpm", "ctrl_rpm",
	       "soc_C", "nand_C", "pressure");

	while (elapsed < seconds && !g_rel.interrupted) {
		double held = -1.0, control = -1.0, soc, nand;
		int pressure;

		read_num(smc, fan->key_ac, &held);
		if (control_idx >= 0)
			read_num(smc, set.fans[control_idx].key_ac, &control);

		fanpro_sensors_enumerate(&sensors, smc, false);
		soc = fanpro_sensors_max_of_class(&sensors, FANPRO_CLASS_SOC);
		nand = fanpro_sensors_max_of_class(&sensors, FANPRO_CLASS_NAND);
		pressure = fanpro_thermal_pressure_read();

		if (held > peak_held)
			peak_held = held;
		if (control > peak_control)
			peak_control = control;
		if (isfinite(soc) && soc > peak_soc)
			peak_soc = soc;
		if (start_soc < 0.0 && isfinite(soc))
			start_soc = soc;
		if (start_control < 0.0 && control > 0.0)
			start_control = control;

		/*
		 * Only start judging once the fan has actually settled at the
		 * commanded speed.  Before that, a high reading is the fan
		 * spinning down from wherever it was, not the firmware
		 * intervening.
		 */
		if (!converged && held <= want_rpm + CONVERGE_TOLERANCE_RPM) {
			converged = true;
			converged_sec = elapsed;
		}

		if (converged && elapsed >= HOLD_SETTLE_SECONDS) {
			if (held > peak_held_judged)
				peak_held_judged = held;

			if (held > want_rpm + CONVERGE_TOLERANCE_RPM) {
				/* Sustained, not a single noisy sample. */
				if (++boost_run >= HOLD_BOOST_SAMPLES && !boosted) {
					boosted = true;
					boost_first_sec = elapsed -
					                  (HOLD_BOOST_SAMPLES - 1);
				}
			} else {
				boost_run = 0;
			}
		}

		printf("%6d %10.0f %10.0f %8.1f %8.1f  %s%s\n", elapsed, held,
		       control, soc, nand, fanpro_thermal_level_name(pressure),
		       converged ? "" : "  (settling)");
		fflush(stdout);

		/* Abort in code, not by watching. */
		if (isfinite(soc) && soc >= abort_temp) {
			stop_reason = "SoC reached the abort temperature";
			break;
		}
		if (pressure != FANPRO_THERMAL_UNKNOWN &&
		    pressure >= FANPRO_THERMAL_HEAVY) {
			stop_reason = "thermal pressure reached heavy";
			break;
		}

		sleep_ms(1000);
		elapsed++;
	}

	if (g_rel.interrupted)
		stop_reason = "interrupted";

	stop_load();
	release_all();

	printf("\nstopped: %s after %d s\n", stop_reason, elapsed);
	printf("  commanded          %.0f rpm\n", want_rpm);
	printf("  peak held fan      %.0f rpm  (includes spin-down at start)\n",
	       peak_held);
	printf("  settled at         %d s\n", converged_sec);
	printf("  peak held, judged  %.0f rpm  (after settling: the evidence)\n",
	       peak_held_judged);
	if (control_idx >= 0)
		printf("  control fan        %.0f -> %.0f rpm  (the firmware's intent)\n",
	       start_control, peak_control);
	printf("  peak SoC           %.1f C\n", peak_soc);

	putchar('\n');

	/*
	 * A verdict is only meaningful if the machine actually got hot.  On an
	 * idle run nothing would ever override the fan, and reporting "the
	 * firmware does not override" from that would be a confident claim
	 * resting on no evidence.  Require either a real temperature rise or
	 * an absolute temperature high enough that the firmware would have
	 * every reason to act.
	 */
	if (!converged) {
		puts("verdict: INCONCLUSIVE.");
		puts("         the fan never settled at the commanded speed, so");
		puts("         nothing here can be attributed to the firmware.");
		return 2;
	}

	if (boosted) {
		puts("verdict: THE FIRMWARE STILL BOOSTS A HELD FAN.");
		printf("         from %d s it held fan %d at up to %.0f rpm against "
		       "a commanded\n", boost_first_sec, fan_idx, peak_held_judged);
		printf("         %.0f, for %d or more consecutive samples.\n",
		       want_rpm, HOLD_BOOST_SAMPLES);
		puts("         it remains a backstop while fanpro holds a fan.");
	} else if ((start_control >= 0.0 && peak_control >= 0.0 &&
	            peak_control - start_control >= HOLD_CONTROL_RAMP_RPM) ||
	           (isfinite(start_soc) && peak_soc >= 0.0 &&
	            (peak_soc >= HOLD_MEANINGFUL_TEMP_C ||
	             peak_soc - start_soc >= HOLD_MEANINGFUL_RISE_C))) {
		puts("verdict: THE FIRMWARE DOES NOT OVERRIDE A HELD FAN.");
		printf("         the control fan went %.0f -> %.0f rpm and SoC %.1f -> "
		       "%.1f C,\n", start_control, peak_control, start_soc, peak_soc);
		puts("         so the firmware plainly wanted more airflow. yet after");
		printf("         settling at %d s, fan %d never exceeded %.0f rpm "
		       "against a\n", converged_sec, fan_idx, peak_held_judged);
		printf("         commanded %.0f.\n", want_rpm);
		puts("");
		puts("         while fanpro holds a fan, fanpro's safety layer is the");
		puts("         only thermal protection that fan has.");
	} else {
		puts("verdict: INCONCLUSIVE.");
		printf("         SoC went %.1f -> %.1f C and the control fan %.0f -> "
		       "%.0f rpm.\n", start_soc, peak_soc, start_control, peak_control);
		puts("         the firmware never showed signs of wanting more airflow,");
		puts("         so this says nothing either way. re-run with more load");
		puts("         or a longer duration.");
		return 2;
	}
	return 0;
}
