/*
 * fanpro - the live dashboard, `fanpro top`.
 *
 * The screen answers two questions, in this order: is the machine thermally
 * healthy, and WHO OWNS EACH FAN right now.  The second question is the one
 * that makes this tool different from any other monitor: on Apple Silicon the
 * firmware does not rescue a fan held in manual mode, so "fanpro is driving
 * fan 0" is a statement about responsibility, not a status flag.
 *
 * Hence the custody gauge.  Each fan gets a track spanning its real range,
 * with a marker for where it actually is and a second marker for where it has
 * been told to go.  When fanpro is driving, you watch the fan chase its
 * target; when the firmware is driving, there is no target marker at all.
 * A fan sitting at its minimum reads as "at the bottom of its range" rather
 * than as an empty bar, which is what the old meter looked like at idle.
 *
 * Reads sensors and fans directly, both unprivileged.  Asks the daemon for
 * its view when it is running, and works fine without it in read-only mode:
 * "is my machine hot" should not depend on having installed anything.  Every
 * mutating hotkey goes through the same IPC path as the CLI, so the TUI holds
 * no special privileges.
 */
#include "fanpro/cli.h"

#include "fanpro/fan.h"
#include "fanpro/power.h"
#include "fanpro/proto.h"
#include "fanpro/safety.h"
#include "fanpro/sensors.h"
#include "fanpro/smc.h"

#include <curses.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

#define SPARK_WIDTH 22
/* Degrees below which the trend line is drawn as flat rather than amplified. */
#define SPARK_MIN_SPAN_C 6.0
#define REFRESH_MS  1000
#define GAUGE_W     26
#define METER_W     18
#define LABEL_COL   2
#define VALUE_COL   8

/* Never draw onto the legend line or past the bottom of the window. */
#define ROOM_FOR(row) ((row) < LINES - 2)

/*
 * Colour roles, not colour names.  Muted instrument-panel tones: this is a
 * gauge cluster you glance at, not a chart you study, and saturated primaries
 * on every row destroy the glance.
 */
#define CP_NAME   1  /* the product mark                       */
#define CP_LABEL  2  /* section headings and field labels      */
#define CP_VALUE  3  /* numbers you actually read              */
#define CP_TRACK  4  /* the unfilled part of any gauge         */
#define CP_COOL   5  /* comfortably within limits              */
#define CP_WARM   6  /* approaching the limit                  */
#define CP_HOT    7  /* at or near panic                       */
#define CP_OWNED  8  /* fanpro is driving this fan             */
#define CP_ALERT  9  /* latched, or something needs attention  */

/*
 * Glyphs.  Unicode where the terminal can take it, ASCII where it cannot.
 *
 * EVERY glyph needs a fallback, not just the obvious ones: an earlier version
 * switched only the sparkline and rendered the meters and gauges as mojibake
 * under LC_ALL=C.  A row of replacement characters is worse than a plain ramp.
 */
static const char *SPARK[8] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
static const char *SPARK_ASCII[8] = { "_", ".", ".", "-", "-", "=", "^", "#" };
static bool g_unicode;

/* Pick between a Unicode glyph and its ASCII stand-in. */
#define GLYPH(uni, ascii) (g_unicode ? (uni) : (ascii))

#define G_TRACK   GLYPH("─", "-")
#define G_ACTUAL  GLYPH("●", "O")
#define G_TARGET  GLYPH("▲", "^")
#define G_FILL    GLYPH("█", "#")
#define G_EMPTY   GLYPH("·", ".")
#define G_RULE    GLYPH("─", "-")
#define G_DEGREE  GLYPH("°C", "C ")

static const char *
spark_glyph(int i)
{
	return g_unicode ? SPARK[i] : SPARK_ASCII[i];
}

static volatile sig_atomic_t g_resized;

static void
on_winch(int sig)
{
	(void)sig;
	g_resized = 1;
}

/* ---- rolling window for the sparkline ----------------------------------- */

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
 * Scale the sparkline to the window's own range, not to an absolute scale.
 * A fixed 30-100 C axis renders every idle machine as a flat line, which
 * tells you nothing; what you want to see is whether the trend is moving.
 */
static void
spark_draw(int y, int x, const spark_t *s)
{
	double lo = 1e9, hi = -1e9;
	int i;

	if (s->n < 2)
		return;

	for (i = 0; i < s->n; i++) {
		if (s->v[i] < lo) lo = s->v[i];
		if (s->v[i] > hi) hi = s->v[i];
	}
	/*
	 * Floor the span at a few degrees.  Scaling tightly to the window makes
	 * an idle machine's half-degree wobble fill the whole ramp, which reads
	 * as a thermal event when nothing is happening.  A real ramp still
	 * shows, because a real ramp is far wider than this floor.
	 */
	if (hi - lo < SPARK_MIN_SPAN_C)
		hi = lo + SPARK_MIN_SPAN_C;

	for (i = 0; i < s->n; i++) {
		int idx = (int)((s->v[i] - lo) / (hi - lo) * 7.0);

		if (idx < 0) idx = 0;
		if (idx > 7) idx = 7;
		attron(COLOR_PAIR(idx >= 6 ? CP_WARM : CP_COOL));
		mvprintw(y, x + i, "%s", spark_glyph(idx));
		attroff(COLOR_PAIR(idx >= 6 ? CP_WARM : CP_COOL));
	}
}

/* ---- shared drawing ----------------------------------------------------- */

/* Colour by proximity to the limit that actually applies to this sensor. */
static int
heat_pair(double c, double limit)
{
	if (!isfinite(c) || !isfinite(limit))
		return CP_TRACK;
	if (c >= limit - 10.0)
		return CP_HOT;
	if (c >= limit - 25.0)
		return CP_WARM;
	return CP_COOL;
}

static void
draw_meter(int y, int x, int width, double frac, int pair)
{
	int filled, i;

	if (!isfinite(frac) || frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	filled = (int)(frac * width + 0.5);

	for (i = 0; i < width; i++) {
		bool on = i < filled;

		attron(COLOR_PAIR(on ? pair : CP_TRACK));
		mvprintw(y, x + i, "%s", on ? G_FILL : G_EMPTY);
		attroff(COLOR_PAIR(on ? pair : CP_TRACK));
	}
}

/*
 * The custody gauge.  A track across the fan's own range, the actual speed as
 * a filled marker, and the commanded target as a second marker when someone
 * is commanding one.  Two markers that disagree means the fan is still
 * spinning toward where it was told to go.
 */
static void
draw_fan_gauge(int y, int x, int width, const fanpro_fan_t *f, bool owned)
{
	double span = f->max_rpm - f->min_rpm;
	int actual = -1, target = -1, i;

	if (span > 0.0 && isfinite(f->rpm)) {
		double frac = (f->rpm - f->min_rpm) / span;

		if (frac < 0.0) frac = 0.0;
		if (frac > 1.0) frac = 1.0;
		actual = (int)(frac * (width - 1) + 0.5);
	}
	if (owned && span > 0.0 && f->has_target && isfinite(f->target)) {
		double frac = (f->target - f->min_rpm) / span;

		if (frac < 0.0) frac = 0.0;
		if (frac > 1.0) frac = 1.0;
		target = (int)(frac * (width - 1) + 0.5);
	}

	for (i = 0; i < width; i++) {
		const char *ch = G_TRACK;
		int pair = CP_TRACK;

		if (i == actual) {
			ch = G_ACTUAL;
			pair = owned ? CP_OWNED : CP_COOL;
		} else if (i == target) {
			ch = G_TARGET;
			pair = CP_LABEL;
		}
		attron(COLOR_PAIR(pair));
		mvprintw(y, x + i, "%s", ch);
		attroff(COLOR_PAIR(pair));
	}
}

static void
draw_rule(int y, int width)
{
	int i;

	attron(COLOR_PAIR(CP_TRACK));
	for (i = 0; i < width; i++)
		mvprintw(y, i, "%s", G_RULE);
	attroff(COLOR_PAIR(CP_TRACK));
}

static void
label(int y, int x, const char *text)
{
	attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
	mvprintw(y, x, "%s", text);
	attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
}

/* ---- daemon plumbing ---------------------------------------------------- */

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

static void
machine_identity(char *out, size_t out_len)
{
	char model[64] = "Mac";
	size_t n = sizeof(model);

	if (sysctlbyname("hw.model", model, &n, NULL, 0) != 0)
		snprintf(model, sizeof(model), "Mac");
	snprintf(out, out_len, "%s", model);
}

int
fanpro_cmd_top(fanpro_smc_t *smc, int argc, char **argv)
{
	fanpro_sensor_set_t sensors;
	fanpro_fan_set_t fans;
	fanpro_response_t daemon_status;
	fanpro_safety_config_t limits;
	spark_t soc_spark;
	struct sigaction sa;
	char message[200] = "";
	char model[64];
	bool have_daemon = false;
	int selected = 0;

	(void)argc; (void)argv;

	memset(&soc_spark, 0, sizeof(soc_spark));
	memset(&daemon_status, 0, sizeof(daemon_status));
	fanpro_safety_defaults(&limits);
	machine_identity(model, sizeof(model));

	if (fanpro_fan_enumerate(smc, &fans) != 0) {
		fputs("fanpro: cannot enumerate fans\n", stderr);
		return 1;
	}

	/* Needed before initscr for wide glyphs to reach the terminal. */
	setlocale(LC_ALL, "");
	g_unicode = strcasestr(nl_langinfo(CODESET), "UTF-8") != NULL;

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
		if (COLORS >= 256) {
			init_pair(CP_NAME,  81, -1);
			init_pair(CP_LABEL, 245, -1);
			init_pair(CP_VALUE, 252, -1);
			init_pair(CP_TRACK, 238, -1);
			init_pair(CP_COOL,  108, -1);
			init_pair(CP_WARM,  179, -1);
			init_pair(CP_HOT,   167, -1);
			init_pair(CP_OWNED, 80,  -1);
			init_pair(CP_ALERT, 173, -1);
		} else {
			init_pair(CP_NAME,  COLOR_CYAN, -1);
			init_pair(CP_LABEL, COLOR_BLUE, -1);
			init_pair(CP_VALUE, -1, -1);
			init_pair(CP_TRACK, COLOR_BLUE, -1);
			init_pair(CP_COOL,  COLOR_GREEN, -1);
			init_pair(CP_WARM,  COLOR_YELLOW, -1);
			init_pair(CP_HOT,   COLOR_RED, -1);
			init_pair(CP_OWNED, COLOR_CYAN, -1);
			init_pair(CP_ALERT, COLOR_MAGENTA, -1);
		}
	}

	for (;;) {
		double soc, gpu, nand;
		int ch, row = 0, i;
		int elapsed_ms = 0;
		int width;

		if (g_resized) {
			g_resized = 0;
			endwin();
			refresh();
			clear();
		}
		width = COLS < 96 ? COLS : 96;

		fanpro_sensors_enumerate(&sensors, smc, false);
		fanpro_fan_refresh(smc, &fans);
		have_daemon = poll_daemon(&daemon_status);

		soc = fanpro_sensors_max_of_class(&sensors, FANPRO_CLASS_SOC);
		gpu = fanpro_sensors_max_of_class(&sensors, FANPRO_CLASS_GPU);
		nand = fanpro_sensors_max_of_class(&sensors, FANPRO_CLASS_NAND);
		if (isfinite(soc))
			spark_push(&soc_spark, soc);

		erase();

		/* ---- masthead: what this is, and who is in charge ---- */
		attron(COLOR_PAIR(CP_NAME) | A_BOLD);
		mvprintw(row, 0, "fanpro");
		attroff(COLOR_PAIR(CP_NAME) | A_BOLD);
		attron(COLOR_PAIR(CP_LABEL));
		mvprintw(row, 7, "%s", model);
		attroff(COLOR_PAIR(CP_LABEL));

		{
			const char *state;
			int pair;

			if (!have_daemon) {
				state = "no daemon  read only";
				pair = CP_LABEL;
			} else if (daemon_status.latched) {
				state = "LATCHED  control refused";
				pair = CP_ALERT;
			} else if (daemon_status.mode == 1) {
				state = "fanpro driving";
				pair = CP_OWNED;
			} else {
				state = "monitor only";
				pair = CP_LABEL;
			}
			attron(COLOR_PAIR(pair));
			mvprintw(row, width - (int)strlen(state), "%s", state);
			attroff(COLOR_PAIR(pair));
		}
		row++;
		draw_rule(row++, width);
		row++;

		/* ---- thermal ---- */
		if (!ROOM_FOR(row))
			goto legend;
		label(row++, 0, "THERMAL");

		#define TEMP_ROW(name, value, limit)                                  \
			do {                                                          \
				int p = heat_pair((value), (limit));                  \
				if (!isfinite(value) || !ROOM_FOR(row)) break;         \
				attron(COLOR_PAIR(CP_LABEL));                         \
				mvprintw(row, LABEL_COL, "%s", (name));               \
				attroff(COLOR_PAIR(CP_LABEL));                        \
				attron(COLOR_PAIR(p) | A_BOLD);                       \
				mvprintw(row, VALUE_COL, "%5.1f", (value));           \
				attroff(COLOR_PAIR(p) | A_BOLD);                      \
				attron(COLOR_PAIR(CP_TRACK));                         \
				mvprintw(row, VALUE_COL + 6, "%s", G_DEGREE);        \
				attroff(COLOR_PAIR(CP_TRACK));                        \
				draw_meter(row, VALUE_COL + 10, METER_W,              \
				           (value) / (limit), p);                     \
				attron(COLOR_PAIR(CP_TRACK));                         \
				mvprintw(row, VALUE_COL + 10 + METER_W + 2,           \
				         "limit %.0f", (limit));                      \
				attroff(COLOR_PAIR(CP_TRACK));                        \
				row++;                                                \
			} while (0)

		TEMP_ROW("soc", soc, limits.panic_c[FANPRO_CLASS_SOC]);
		TEMP_ROW("gpu", gpu, limits.panic_c[FANPRO_CLASS_GPU]);
		TEMP_ROW("nand", nand, limits.panic_c[FANPRO_CLASS_NAND]);
		#undef TEMP_ROW

		if (soc_spark.n > 1 && ROOM_FOR(row)) {
			attron(COLOR_PAIR(CP_TRACK));
			mvprintw(row, LABEL_COL, "trend");
			attroff(COLOR_PAIR(CP_TRACK));
			spark_draw(row, VALUE_COL + 10, &soc_spark);
			attron(COLOR_PAIR(CP_TRACK));
			mvprintw(row, VALUE_COL + 10 + SPARK_WIDTH + 2, "%ds soc",
			         soc_spark.n);
			attroff(COLOR_PAIR(CP_TRACK));
			row++;
		}
		row++;

		/* ---- fans: the custody question ---- */
		if (!ROOM_FOR(row))
			goto legend;
		label(row, 0, "FANS");
		attron(COLOR_PAIR(CP_TRACK));
		mvprintw(row, VALUE_COL + 10, "%s", "min");
		mvprintw(row, VALUE_COL + 10 + GAUGE_W - 3, "%s", "max");
		mvprintw(row, VALUE_COL + 10 + GAUGE_W + 3, "%s", "owner");
		attroff(COLOR_PAIR(CP_TRACK));
		row++;

		for (i = 0; i < fans.count && ROOM_FOR(row); i++) {
			const fanpro_fan_t *f = &fans.fans[i];
			bool owned = have_daemon && i < daemon_status.n_fans &&
			             daemon_status.fans[i].held;
			const char *owner = owned ? "fanpro" : "firmware";

			if (i == selected) {
				attron(COLOR_PAIR(CP_VALUE) | A_REVERSE);
				mvprintw(row, 0, " ");
				attroff(COLOR_PAIR(CP_VALUE) | A_REVERSE);
			}
			attron(COLOR_PAIR(CP_LABEL));
			mvprintw(row, LABEL_COL, "fan %d", i);
			attroff(COLOR_PAIR(CP_LABEL));

			attron(COLOR_PAIR(owned ? CP_OWNED : CP_VALUE) | A_BOLD);
			mvprintw(row, VALUE_COL - 1, "%5.0f", f->rpm);
			attroff(COLOR_PAIR(owned ? CP_OWNED : CP_VALUE) | A_BOLD);

			draw_fan_gauge(row, VALUE_COL + 10, GAUGE_W, f, owned);

			attron(COLOR_PAIR(owned ? CP_OWNED : CP_TRACK));
			mvprintw(row, VALUE_COL + 10 + GAUGE_W + 3, "%s", owner);
			attroff(COLOR_PAIR(owned ? CP_OWNED : CP_TRACK));
			row++;
		}
		row++;

		/* ---- power and the system's own thermal opinion ---- */
		if (ROOM_FOR(row)) {
			label(row, 0, "POWER");
			attron(COLOR_PAIR(CP_VALUE));
			if (have_daemon)
				mvprintw(row, VALUE_COL + 1, "cpu %5.2f W   gpu %6.2f W",
				         daemon_status.cpu_watts, daemon_status.gpu_watts);
			else
				mvprintw(row, VALUE_COL + 1, "%s",
				         "start the daemon for power readings");
			attroff(COLOR_PAIR(CP_VALUE));

			if (have_daemon) {
				const char *lv =
				    fanpro_thermal_level_name(daemon_status.thermal_level);
				int pair = daemon_status.thermal_level >= FANPRO_THERMAL_HEAVY
				               ? CP_HOT
				               : (daemon_status.thermal_level > 0 ? CP_WARM
				                                                  : CP_TRACK);

				attron(COLOR_PAIR(CP_TRACK));
				mvprintw(row, width - 26, "system pressure");
				attroff(COLOR_PAIR(CP_TRACK));
				attron(COLOR_PAIR(pair));
				mvprintw(row, width - 10, "%s", lv);
				attroff(COLOR_PAIR(pair));
			}
			row++;
		}

		if (message[0] != '\0' && ROOM_FOR(row + 1)) {
			row++;
			attron(COLOR_PAIR(CP_ALERT));
			mvprintw(row, 0, "%.*s", width, message);
			attroff(COLOR_PAIR(CP_ALERT));
		}

legend:
		draw_rule(LINES - 2, width);
		attron(COLOR_PAIR(CP_TRACK));
		mvprintw(LINES - 1, 0,
		         g_unicode
		             ? " ↑↓ fan    +/− speed    a release    c control    m monitor    q quit"
		             : " up/dn fan  +/- speed   a release    c control    m monitor    q quit");
		attroff(COLOR_PAIR(CP_TRACK));

		refresh();

		/* Poll for input across the refresh interval so the UI stays
		 * responsive without spinning. */
		while (elapsed_ms < REFRESH_MS) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000L };

			ch = getch();
			if (ch != ERR) {
				char err[200];

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
					want = (ch == '+' || ch == '=') ? now + 200.0
					                                : now - 200.0;
					if (send_simple(FANPRO_VERB_SET_FAN, selected, want, 0,
					                err, sizeof(err)) != 0)
						snprintf(message, sizeof(message), "%s", err);
					else
						snprintf(message, sizeof(message),
						         "fan %d set to %.0f rpm. volatile: put it "
						         "in fanpro.conf to keep it.",
						         selected, want);
					break;
				}
				case 'a':
				case 'A':
					if (send_simple(FANPRO_VERB_SET_FAN, selected, NAN, 0,
					                err, sizeof(err)) != 0)
						snprintf(message, sizeof(message), "%s", err);
					else
						snprintf(message, sizeof(message),
						         "fan %d released. the firmware has it back.",
						         selected);
					break;
				case 'c':
				case 'C':
					if (send_simple(FANPRO_VERB_SET_MODE, -1, NAN, 1, err,
					                sizeof(err)) != 0)
						snprintf(message, sizeof(message), "%s", err);
					else
						snprintf(message, sizeof(message),
						         "fanpro is driving the fans. it is now the "
						         "only thermal protection they have.");
					break;
				case 'm':
				case 'M':
					if (send_simple(FANPRO_VERB_SET_MODE, -1, NAN, 0, err,
					                sizeof(err)) != 0)
						snprintf(message, sizeof(message), "%s", err);
					else
						snprintf(message, sizeof(message),
						         "monitor only. the firmware has the fans.");
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
	return 0;
}
