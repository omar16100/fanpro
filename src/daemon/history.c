/*
 * fanpro - sample history.
 *
 * One JSON object per line, appended once a second.  JSONL rather than CSV
 * because the column set will change and a reader that ignores unknown keys
 * survives that; `fanpro history --csv` converts on demand.
 *
 * Rotation is by day.  A machine left running for months should not end up
 * with one unbounded file, and daily boundaries are what anyone reasoning
 * about thermals actually wants to slice on.
 */
#include "fanpro/history.h"

#include "fanpro/log.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Keep this many rotated files.  At ~150 bytes a row and 86400 rows a day
 * that is roughly 12 MB per file, so a fortnight is a couple of hundred MB. */
#define HISTORY_KEEP_DAYS 14

static FILE *g_file;
static char  g_path[512];
static int   g_opened_day = -1;

/* Emit a double, or JSON null when it is not a real measurement.  Writing 0
 * for "no reading" would look like a very cold machine to anything plotting
 * this later. */
static int
put_num(char *out, size_t out_len, size_t off, const char *key, double v)
{
	if (off >= out_len)
		return -1;
	if (isfinite(v))
		return snprintf(out + off, out_len - off, "\"%s\":%.2f,", key, v);
	return snprintf(out + off, out_len - off, "\"%s\":null,", key);
}

int
fanpro_history_format(const fanpro_history_row_t *row, char *out, size_t out_len)
{
	size_t off = 0;
	int n, i;

	if (row == NULL || out == NULL || out_len == 0)
		return -1;

	#define APPEND(...)                                                   \
		do {                                                          \
			n = snprintf(out + off, out_len - off, __VA_ARGS__);   \
			if (n < 0 || (size_t)n >= out_len - off)               \
				return -1;                                    \
			off += (size_t)n;                                     \
		} while (0)

	APPEND("{\"t\":%lld,", (long long)row->unix_time);

	/*
	 * Every one of these must check truncation, not just a negative
	 * return.  snprintf reports the length it WOULD have written, so on
	 * truncation `off` runs past out_len; the next unguarded write then
	 * computes `out_len - off` as a huge size_t and writes out of bounds.
	 */
	#define PUT(key, val)                                                 \
		do {                                                          \
			n = put_num(out, out_len, off, (key), (val));         \
			if (n < 0 || (size_t)n >= out_len - off)               \
				return -1;                                    \
			off += (size_t)n;                                     \
		} while (0)

	PUT("soc_c", row->soc_c);
	PUT("gpu_c", row->gpu_c);
	PUT("nand_c", row->nand_c);
	PUT("cpu_w", row->cpu_w);
	PUT("gpu_w", row->gpu_w);

	#undef PUT

	APPEND("\"fans\":[");
	for (i = 0; i < row->n_fans && i < FANPRO_MAX_FANS; i++) {
		APPEND("%s{\"rpm\":%.0f,\"target\":%.0f,\"mode\":%d}",
		       i ? "," : "", row->fan_rpm[i], row->fan_target[i],
		       row->fan_mode[i]);
	}
	APPEND("],");

	APPEND("\"thermal\":%d,\"driving\":%s}", row->thermal_level,
	       row->driving ? "true" : "false");

	#undef APPEND
	return (int)off;
}

bool
fanpro_history_should_rotate(int opened_day, int now_day)
{
	if (opened_day < 0)
		return false;
	return opened_day != now_day;
}

/* "history.jsonl.20260806", optionally with a ".N" collision suffix. */
static bool
is_rotated_name(const char *name)
{
	const char *prefix = "history.jsonl.";
	size_t n = strlen(prefix);
	int i;

	if (strncmp(name, prefix, n) != 0)
		return false;
	for (i = 0; i < 8; i++) {
		if (name[n + i] < '0' || name[n + i] > '9')
			return false;
	}
	return name[n + 8] == '\0' || name[n + 8] == '.';
}

/* Delete rotated files older than the retention window. */
static void
prune_old(void)
{
	DIR *dir = opendir(FANPRO_HISTORY_DIR);
	struct dirent *ent;
	time_t cutoff = time(NULL) - (time_t)HISTORY_KEEP_DAYS * 86400;

	if (dir == NULL)
		return;

	while ((ent = readdir(dir)) != NULL) {
		char full[768];
		struct stat st;

		/*
		 * Match only our own rotated files, "history.jsonl.YYYYMMDD".
		 * A looser "history.*" match would happily delete an editor
		 * backup or anything else a user had left in this directory.
		 */
		if (!is_rotated_name(ent->d_name))
			continue;

		snprintf(full, sizeof(full), "%s/%s", FANPRO_HISTORY_DIR,
		         ent->d_name);
		if (stat(full, &st) == 0 && st.st_mtime < cutoff) {
			unlink(full);
			FANPRO_INFO("history.prune", "removed=%s", ent->d_name);
		}
	}
	closedir(dir);
}

/*
 * rename(2) silently replaces an existing target.  Two rotations landing on
 * the same date (daemon down for a day, restarted, crosses midnight) would
 * then destroy the older file with no warning, so pick a free name instead.
 */
static int
rename_no_clobber(const char *from, const char *to)
{
	char candidate[800];
	struct stat st;
	int i;

	if (stat(to, &st) != 0)
		return rename(from, to);

	for (i = 1; i < 100; i++) {
		snprintf(candidate, sizeof(candidate), "%s.%d", to, i);
		if (stat(candidate, &st) != 0) {
			FANPRO_WARN("history.rotate",
			            "reason=target_exists using=%s", candidate);
			return rename(from, candidate);
		}
	}
	return -1;
}

static int
open_file(const char *path)
{
	struct tm tmv;
	time_t now = time(NULL);

	localtime_r(&now, &tmv);

	g_file = fopen(path, "a");
	if (g_file == NULL) {
		FANPRO_WARN("history.open", "path=%s reason=cannot_open", path);
		return -1;
	}
	setvbuf(g_file, NULL, _IOLBF, 0);
	g_opened_day = tmv.tm_yday;
	snprintf(g_path, sizeof(g_path), "%s", path);
	return 0;
}

int
fanpro_history_open(const char *path)
{
	struct stat st;

	if (path == NULL)
		path = FANPRO_HISTORY_FILE;

	mkdir(FANPRO_HISTORY_DIR, 0755);

	/*
	 * Rotation is only noticed while running.  If the daemon was stopped
	 * across midnight, the existing file holds another day's rows and
	 * appending to it would mix days into one file that then gets labelled
	 * with the wrong date.  Roll it over before opening.
	 */
	if (stat(path, &st) == 0 && st.st_size > 0) {
		struct tm file_tm, now_tm;
		time_t now = time(NULL);

		localtime_r(&st.st_mtime, &file_tm);
		localtime_r(&now, &now_tm);
		if (file_tm.tm_yday != now_tm.tm_yday ||
		    file_tm.tm_year != now_tm.tm_year) {
			char dated[768];

			snprintf(dated, sizeof(dated), "%s.%04d%02d%02d", path,
			         file_tm.tm_year + 1900, file_tm.tm_mon + 1,
			         file_tm.tm_mday);
			/* Name it for the day it actually contains, taken from
			 * its mtime rather than assumed to be yesterday. */
			if (rename_no_clobber(path, dated) != 0)
				FANPRO_WARN("history.open",
				            "reason=stale_rotate_failed to=%s", dated);
			else
				FANPRO_INFO("history.open", "rotated_stale=%s", dated);
		}
	}

	if (open_file(path) != 0)
		return -1;

	prune_old();
	FANPRO_INFO("history.open", "path=%s keep_days=%d", path,
	            HISTORY_KEEP_DAYS);
	return 0;
}

void
fanpro_history_close(void)
{
	if (g_file != NULL) {
		fclose(g_file);
		g_file = NULL;
	}
	g_opened_day = -1;
}

static void
rotate(void)
{
	char dated[768];
	struct tm tmv;
	time_t now = time(NULL);

	/* Name the rotated file for the day it CONTAINS, which is yesterday
	 * by the time we notice the boundary. */
	now -= 86400;
	localtime_r(&now, &tmv);

	snprintf(dated, sizeof(dated), "%s.%04d%02d%02d", g_path,
	         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);

	fclose(g_file);
	g_file = NULL;

	if (rename_no_clobber(g_path, dated) != 0)
		FANPRO_WARN("history.rotate", "reason=rename_failed to=%s", dated);
	else
		FANPRO_INFO("history.rotate", "to=%s", dated);

	open_file(g_path);
	prune_old();
}

int
fanpro_history_append(const fanpro_history_row_t *row)
{
	char line[1024];
	struct tm tmv;
	time_t now;
	int n;

	if (g_file == NULL || row == NULL)
		return -1;

	now = time(NULL);
	localtime_r(&now, &tmv);
	if (fanpro_history_should_rotate(g_opened_day, tmv.tm_yday))
		rotate();
	if (g_file == NULL)
		return -1;

	n = fanpro_history_format(row, line, sizeof(line));
	if (n < 0) {
		FANPRO_WARN("history.append", "reason=row_too_long");
		return -1;
	}

	if (fprintf(g_file, "%s\n", line) < 0) {
		/* A full disk must not take the daemon with it: history is the
		 * least important thing this process does. */
		FANPRO_WARN("history.append", "reason=write_failed");
		return -1;
	}
	return 0;
}
