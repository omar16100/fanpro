/*
 * fanpro - the live dashboard, `fanpro top`.
 *
 * Reads sensors and fans directly (both unprivileged) and asks the daemon for
 * its view when it is running.  It works with no daemon at all, in read-only
 * mode, because "is my machine hot" should never depend on having installed
 * anything.
 *
 * Every mutating hotkey goes through the IPC client, so the TUI has exactly
 * the same permissions as any other CLI invocation and never opens a write
 * path to the SMC itself.
 */
#include "fanpro/cli.h"

#include "fanpro/fan.h"
#include "fanpro/power.h"
#include "fanpro/proto.h"
#include "fanpro/sensors.h"
#include "fanpro/smc.h"

#include <curses.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SPARK_WIDTH 48
#define REFRESH_MS  1000

/*
 * Never draw onto the last line (the key legend lives there) or past the
 * bottom of the window.  ncurses clips, but silently losing the fan list in
 * a short terminal would look like fanpro had stopped seeing the fans.
 */
#define ROOM_FOR(row) ((row) < LINES - 2)

/* Colour pairs. */
#define CP_HEADER 1
#define CP_OK     2
#define CP_WARN   3
#define CP_HOT    4
#define CP_DIM    5

static volatile sig_atomic_t g_resized;

static void
on_winch(int sig)
{
	(void)sig;
	g_resized = 1;
}

/* A rolling window of one value, for the sparkline. */
typedef struct {
	double v[SPARK_WIDTH];
	int    n;
} spark_t;

static void
spark_push(spark_t *s, double value)
{
	int i;

	if (s->n < SPARK_WIDTH) {
		s->v[s->n++] = value;
		return;
	}
	for (i = 1; i < SPARK_WIDTH; i++)
		s->v[i - 1] = s->v[i];
	s->v[SPARK_WIDTH - 1] = value;
}

/*
 * Draw the sparkline with ASCII rather than Unicode blocks.  Terminals and
 * fonts vary far more than the value of pretty glyphs here, and a row of
 * replacement characters is worse than a plain ramp.
 */
static void
spark_draw(WINDOW *w, int y, int x, const spark_t *s, double lo, double hi)
{
	static const char ramp[] = "_.-~=^*#";
	const int levels = (int)sizeof(ramp) - 2;
	int i;

	if (s->n == 0 || hi <= lo)
		return;

	for (i = 0; i < s->n; i++) {
		double frac = (s->v[i] - lo) / (hi - lo);
		int idx;

		if (frac < 0.0)
			frac = 0.0;
		if (frac > 1.0)
			frac = 1.0;
		idx = (int)(frac * levels);
		mvwaddch(w, y, x + i, ramp[idx]);
	}
}

/* Colour by how close a temperature is to its class panic threshold. */
static int
temp_colour(double c, double panic_c)
{
	if (!isfinite(c) || !isfinite(panic_c))
		return CP_DIM;
	if (c >= panic_c - 10.0)
		return CP_HOT;
	if (c >= panic_c - 25.0)
		return CP_WARN;
	return CP_OK;
}

static void
draw_bar(WINDOW *w, int y, int x, int width, double frac, int colour)
{
	int filled, i;

	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	filled = (int)(frac * width);

	mvwaddch(w, y, x, '[');
	wattron(w, COLOR_PAIR(colour));
	for (i = 0; i < width; i++)
		mvwaddch(w, y, x + 1 + i, i < filled ? '|' : ' ');
	wattroff(w, COLOR_PAIR(colour));
	mvwaddch(w, y, x + width + 1, ']');
}

/* Ask the daemon for its view.  Absence is normal, not an error. */
static bool
poll_daemon(fanpro_response_t *resp)
{
	fanpro_request_t req;
	char err[256];

	fanpro_request_init(&req, FANPRO_VERB_STATUS);
	return fanpro_ipc_call(&req, resp, err, sizeof(err)) == 0;
}

static int
send_simple(fanpro_verb_t verb, int fan, double rpm, int ival, char *err,
            size_t err_len)
{
	fanpro_request_t req;
	fanpro_response_t resp;

	fanpro_request_init(&req, verb);
	req.fan_index = fan;
	req.rpm = rpm;
	req.ival = ival;
	return fanpro_ipc_call(&req, &resp, err, err_len);
}

int
fanpro_cmd_top(fanpro_smc_t *smc, int argc, char **argv)
{
	fanpro_sensor_set_t sensors;
	fanpro_fan_set_t fans;
	fanpro_power_set_t power;
	fanpro_response_t daemon_status;
	spark_t soc_spark;
	struct sigaction sa;
	char message[256] = "";
	bool have_daemon = false;
	int selected = 0;
	int rc = 0;

	(void)argc; (void)argv;

	memset(&soc_spark, 0, sizeof(soc_spark));
	memset(&power, 0, sizeof(power));
	memset(&daemon_status, 0, sizeof(daemon_status));

	if (fanpro_fan_enumerate(smc, &fans) != 0) {
		fputs("fanpro: cannot enumerate fans\n", stderr);
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_winch;
	sigaction(SIGWINCH, &sa, NULL);

	if (initscr() == NULL) {
		fputs("fanpro: cannot initialise the terminal\n", stderr);
		return 1;
	}
	cbreak();
	noecho();
	curs_set(0);
	nodelay(stdscr, TRUE);
	keypad(stdscr, TRUE);

	if (has_colors()) {
		start_color();
		use_default_colors();
		init_pair(CP_HEADER, COLOR_CYAN, -1);
		init_pair(CP_OK, COLOR_GREEN, -1);
		init_pair(CP_WARN, COLOR_YELLOW, -1);
		init_pair(CP_HOT, COLOR_RED, -1);
		init_pair(CP_DIM, COLOR_BLUE, -1);
	}

	for (;;) {
		double soc, gpu, nand;
		int ch, row = 0, i;
		int elapsed_ms = 0;

		if (g_resized) {
			g_resized = 0;
			endwin();
			refresh();
			clear();
		}

		fanpro_sensors_enumerate(&sensors, smc, false);
		fanpro_fan_refresh(smc, &fans);
		have_daemon = poll_daemon(&daemon_status);

		soc = fanpro_sensors_max_of_class(&sensors, FANPRO_CLASS_SOC);
		gpu = fanpro_sensors_max_of_class(&sensors, FANPRO_CLASS_GPU);
		nand = fanpro_sensors_max_of_class(&sensors, FANPRO_CLASS_NAND);
		if (isfinite(soc))
			spark_push(&soc_spark, soc);

		erase();

		wattron(stdscr, COLOR_PAIR(CP_HEADER) | A_BOLD);
		mvprintw(row++, 0, "fanpro");
		wattroff(stdscr, COLOR_PAIR(CP_HEADER) | A_BOLD);

		if (have_daemon) {
			mvprintw(row++, 0, "daemon: running, mode %s%s",
			         daemon_status.mode == 1 ? "curve" : "auto",
			         daemon_status.latched ? "  [LATCHED]" : "");
		} else {
			wattron(stdscr, COLOR_PAIR(CP_DIM));
			mvprintw(row++, 0,
			         "daemon: not running (monitoring only, read-only)");
			wattroff(stdscr, COLOR_PAIR(CP_DIM));
		}
		row++;

		/* ---- temperatures ---- */
		if (!ROOM_FOR(row))
			goto legend;
		mvprintw(row++, 0, "temperatures");
		if (isfinite(soc) && ROOM_FOR(row)) {
			wattron(stdscr, COLOR_PAIR(temp_colour(soc, 95.0)));
			mvprintw(row, 2, "%-6s %6.1f C", "soc", soc);
			wattroff(stdscr, COLOR_PAIR(temp_colour(soc, 95.0)));
			draw_bar(stdscr, row, 22, 20, soc / 100.0,
			         temp_colour(soc, 95.0));
			row++;
		}
		if (isfinite(gpu) && ROOM_FOR(row)) {
			mvprintw(row, 2, "%-6s %6.1f C", "gpu", gpu);
			draw_bar(stdscr, row, 22, 20, gpu / 100.0,
			         temp_colour(gpu, 95.0));
			row++;
		}
		if (isfinite(nand) && ROOM_FOR(row)) {
			wattron(stdscr, COLOR_PAIR(temp_colour(nand, 80.0)));
			mvprintw(row, 2, "%-6s %6.1f C", "nand", nand);
			wattroff(stdscr, COLOR_PAIR(temp_colour(nand, 80.0)));
			draw_bar(stdscr, row, 22, 20, nand / 80.0,
			         temp_colour(nand, 80.0));
			row++;
		}
		row++;

		if (soc_spark.n > 1 && ROOM_FOR(row + 2)) {
			mvprintw(row++, 2, "soc, last %ds", soc_spark.n);
			spark_draw(stdscr, row++, 2, &soc_spark, 30.0, 100.0);
			row++;
		}

		/* ---- fans ---- */
		if (!ROOM_FOR(row))
			goto legend;
		mvprintw(row++, 0, "fans");
		for (i = 0; i < fans.count && ROOM_FOR(row); i++) {
			const fanpro_fan_t *f = &fans.fans[i];
			double span = f->max_rpm - f->min_rpm;
			double frac = (span > 0.0) ? (f->rpm - f->min_rpm) / span : 0.0;
			const char *held = "";

			if (have_daemon && i < daemon_status.n_fans &&
			    daemon_status.fans[i].held)
				held = " (fanpro)";

			if (i == selected)
				wattron(stdscr, A_REVERSE);
			mvprintw(row, 2, "fan %d %6.0f rpm  %-7s%s", i, f->rpm,
			         fanpro_fan_mode_name(f->mode), held);
			if (i == selected)
				wattroff(stdscr, A_REVERSE);

			draw_bar(stdscr, row, 40, 20, frac,
			         f->mode == FANPRO_FAN_MODE_MANUAL ? CP_WARN : CP_OK);
			row++;
		}
		row++;

		/* ---- power ---- */
		if (have_daemon && ROOM_FOR(row)) {
			/* Take it from the daemon, which is already sampling; a
			 * second sampler would block this loop for its interval. */
			mvprintw(row++, 0, "power   CPU %5.2f W   GPU %5.2f W",
			         daemon_status.cpu_watts, daemon_status.gpu_watts);
		} else if (power.available && ROOM_FOR(row)) {
			mvprintw(row++, 0, "power   total %5.2f W", power.total_watts);
		}
		row++;

		if (message[0] != '\0' && ROOM_FOR(row)) {
			wattron(stdscr, COLOR_PAIR(CP_WARN));
			mvprintw(row++, 0, "%s", message);
			wattroff(stdscr, COLOR_PAIR(CP_WARN));
		}

legend:
		wattron(stdscr, COLOR_PAIR(CP_DIM));
		mvprintw(LINES - 1, 0,
		         " up/down select fan   +/- adjust   a auto   c curve mode   "
		         "m monitor mode   q quit");
		wattroff(stdscr, COLOR_PAIR(CP_DIM));

		refresh();

		/* Poll for a keypress across the refresh interval, so the UI
		 * feels responsive without spinning. */
		while (elapsed_ms < REFRESH_MS) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000L };

			ch = getch();
			if (ch != ERR) {
				double step = 200.0;
				char err[256];

				message[0] = '\0';
				switch (ch) {
				case 'q':
				case 'Q':
					goto done;
				case KEY_UP:
					if (selected > 0)
						selected--;
					break;
				case KEY_DOWN:
					if (selected < fans.count - 1)
						selected++;
					break;
				case '+':
				case '=':
				case '-':
				case '_': {
					double now = fans.fans[selected].target;
					double want;

					if (!isfinite(now) || now <= 0.0)
						now = fans.fans[selected].rpm;
					want = (ch == '+' || ch == '=') ? now + step
					                                : now - step;
					if (send_simple(FANPRO_VERB_SET_FAN, selected, want,
					                0, err, sizeof(err)) != 0)
						snprintf(message, sizeof(message), "%s", err);
					else
						snprintf(message, sizeof(message),
						         "fan %d set to %.0f rpm (volatile)",
						         selected, want);
					break;
				}
				case 'a':
				case 'A':
					if (send_simple(FANPRO_VERB_SET_FAN, selected, NAN,
					                0, err, sizeof(err)) != 0)
						snprintf(message, sizeof(message), "%s", err);
					else
						snprintf(message, sizeof(message),
						         "fan %d released to the firmware",
						         selected);
					break;
				case 'c':
				case 'C':
					if (send_simple(FANPRO_VERB_SET_MODE, -1, NAN, 1,
					                err, sizeof(err)) != 0)
						snprintf(message, sizeof(message), "%s", err);
					else
						snprintf(message, sizeof(message),
						         "curve mode: fanpro is now driving the "
						         "fans and is their only protection");
					break;
				case 'm':
				case 'M':
					if (send_simple(FANPRO_VERB_SET_MODE, -1, NAN, 0,
					                err, sizeof(err)) != 0)
						snprintf(message, sizeof(message), "%s", err);
					else
						snprintf(message, sizeof(message),
						         "monitor-only: fans back to the firmware");
					break;
				default:
					break;
				}
				break; /* redraw immediately after any key */
			}
			nanosleep(&ts, NULL);
			elapsed_ms += 50;
		}
	}

done:
	endwin();
	return rc;
}
