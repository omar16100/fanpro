/*
 * fanpro - leveled structured logging with size-based rotation.
 *
 * Format: ISO-8601 timestamp, level, event name, then caller-supplied
 * key=value fields.  Every SMC write goes through here, so the log alone is
 * enough to reconstruct what the daemon told the firmware and what it said
 * back.
 */
#include "fanpro/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static fanpro_log_level_t g_level = FANPRO_LOG_INFO;
static FILE              *g_file;
static char               g_path[512];
static size_t             g_max_bytes;
static int                g_keep;
static pthread_mutex_t    g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *const LEVEL_NAME[] = {
	"ERROR", "WARN", "INFO", "DEBUG", "TRACE",
};

void
fanpro_log_set_level(fanpro_log_level_t level)
{
	g_level = level;
}

fanpro_log_level_t
fanpro_log_get_level(void)
{
	return g_level;
}

int
fanpro_log_level_from_str(const char *s, fanpro_log_level_t *out)
{
	size_t i;

	if (s == NULL || out == NULL)
		return -1;
	for (i = 0; i < sizeof(LEVEL_NAME) / sizeof(LEVEL_NAME[0]); i++) {
		if (strcasecmp(s, LEVEL_NAME[i]) == 0) {
			*out = (fanpro_log_level_t)i;
			return 0;
		}
	}
	return -1;
}

int
fanpro_log_open_file(const char *path, size_t max_bytes, int keep)
{
	FILE *f;

	pthread_mutex_lock(&g_lock);
	if (g_file != NULL) {
		fclose(g_file);
		g_file = NULL;
	}
	if (path == NULL) {
		g_path[0] = '\0';
		pthread_mutex_unlock(&g_lock);
		return 0;
	}

	f = fopen(path, "a");
	if (f == NULL) {
		pthread_mutex_unlock(&g_lock);
		return -errno;
	}
	setvbuf(f, NULL, _IOLBF, 0);
	g_file = f;
	snprintf(g_path, sizeof(g_path), "%s", path);
	g_max_bytes = max_bytes;
	g_keep = keep > 0 ? keep : 1;
	pthread_mutex_unlock(&g_lock);
	return 0;
}

void
fanpro_log_close(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_file != NULL) {
		fclose(g_file);
		g_file = NULL;
	}
	g_path[0] = '\0';
	pthread_mutex_unlock(&g_lock);
}

/* Caller holds g_lock. */
static void
rotate_if_needed(void)
{
	char from[600], to[600];
	long size;
	int i;

	if (g_file == NULL || g_max_bytes == 0 || g_path[0] == '\0')
		return;

	size = ftell(g_file);
	if (size < 0 || (size_t)size < g_max_bytes)
		return;

	fclose(g_file);
	g_file = NULL;

	for (i = g_keep - 1; i >= 1; i--) {
		snprintf(from, sizeof(from), "%s.%d", g_path, i);
		snprintf(to, sizeof(to), "%s.%d", g_path, i + 1);
		rename(from, to);
	}
	snprintf(to, sizeof(to), "%s.1", g_path);
	rename(g_path, to);

	g_file = fopen(g_path, "a");
	if (g_file != NULL)
		setvbuf(g_file, NULL, _IOLBF, 0);
}

void
fanpro_log(fanpro_log_level_t level, const char *event, const char *fmt, ...)
{
	char ts[32];
	struct tm tmv;
	time_t now;
	va_list ap;
	FILE *out;

	if (level > g_level)
		return;

	now = time(NULL);
	localtime_r(&now, &tmv);
	strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tmv);

	pthread_mutex_lock(&g_lock);
	rotate_if_needed();
	out = (g_file != NULL) ? g_file : stderr;

	fprintf(out, "%s %-5s %s", ts, LEVEL_NAME[level], event);
	if (fmt != NULL && fmt[0] != '\0') {
		fputc(' ', out);
		va_start(ap, fmt);
		vfprintf(out, fmt, ap);
		va_end(ap);
	}
	fputc('\n', out);
	pthread_mutex_unlock(&g_lock);
}
