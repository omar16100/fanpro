/*
 * fanpro - reading back history and logs.
 *
 * Both are plain files the daemon appends to, so these commands are
 * deliberately dumb readers.  Nothing here talks to the daemon: if fanprod
 * has died, being able to look at what it recorded just before is exactly
 * when you need it most.
 */
#include "fanpro/cli.h"

#include "fanpro/history.h"
#include "fanpro/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DAEMON_LOG_PATH "/var/log/fanpro/fanprod.log"

/* Print the last `n` lines of a file without reading all of it into memory. */
static int
tail_file(const char *path, int n)
{
	char **ring;
	char line[2048];
	FILE *f;
	int count = 0, i;

	f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "fanpro: cannot read %s\n", path);
		if (strcmp(path, DAEMON_LOG_PATH) == 0)
			fputs("  fanprod only writes this file when running under "
			      "launchd.\n  in the foreground (-f) it logs to "
			      "stderr instead.\n", stderr);
		else
			fputs("  no history recorded yet; is fanprod running?\n",
			      stderr);
		return 1;
	}

	ring = calloc((size_t)n, sizeof(*ring));
	if (ring == NULL) {
		fclose(f);
		return 1;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		int slot = count % n;

		free(ring[slot]);
		ring[slot] = strdup(line);
		if (ring[slot] == NULL) {
			fputs("fanpro: out of memory reading the log\n", stderr);
			break;
		}
		count++;
	}
	fclose(f);

	for (i = (count > n) ? count - n : 0; i < count; i++) {
		char *s = ring[i % n];

		if (s != NULL)
			fputs(s, stdout);
	}
	for (i = 0; i < n; i++)
		free(ring[i]);
	free(ring);

	if (count == 0)
		puts("(empty)");
	return 0;
}

/*
 * Convert the JSONL history to CSV.
 *
 * A deliberately shallow scan rather than a JSON parser: the daemon writes
 * these lines and the shape is fixed, so pulling known keys out is honest
 * about what this is. Anything it cannot find becomes an empty cell rather
 * than a zero.
 */
static bool
field(const char *line, const char *key, char *out, size_t out_len)
{
	char pat[64];
	const char *p, *q;
	size_t n;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	p = strstr(line, pat);
	if (p == NULL)
		return false;
	p += strlen(pat);

	q = p;
	while (*q != '\0' && *q != ',' && *q != '}' && *q != ']')
		q++;

	n = (size_t)(q - p);
	if (n == 0 || n >= out_len)
		return false;
	memcpy(out, p, n);
	out[n] = '\0';
	/* "null" means the sensor was absent; an empty cell says that without
	 * pretending the machine was at 0 C. */
	if (strcmp(out, "null") == 0)
		out[0] = '\0';
	return true;
}

static int
history_csv(const char *path)
{
	char line[2048];
	FILE *f = fopen(path, "r");

	if (f == NULL) {
		fprintf(stderr, "fanpro: cannot read %s\n", path);
		return 1;
	}

	/* Deliberately a flat subset: CSV cannot express the per-fan array, so
	 * fan 0 stands in for it and the JSONL remains the complete record. */
	puts("unix_time,soc_c,gpu_c,nand_c,cpu_w,gpu_w,fan0_rpm,thermal,driving");
	while (fgets(line, sizeof(line), f) != NULL) {
		char t[32] = "", soc[32] = "", gpu[32] = "", nand[32] = "";
		char cw[32] = "", gw[32] = "", rpm[32] = "", th[32] = "", dr[32] = "";

		field(line, "t", t, sizeof(t));
		field(line, "soc_c", soc, sizeof(soc));
		field(line, "gpu_c", gpu, sizeof(gpu));
		field(line, "nand_c", nand, sizeof(nand));
		field(line, "cpu_w", cw, sizeof(cw));
		field(line, "gpu_w", gw, sizeof(gw));
		/* Scope the search to the fans array and take its FIRST entry.
		 * A bare search for "rpm" happens to find fan 0 today only
		 * because of the order the daemon writes fields in; one
		 * reordering and this would silently report fan 1 as fan 0. */
		{
			const char *fans = strstr(line, "\"fans\":[");

			if (fans != NULL)
				field(fans, "rpm", rpm, sizeof(rpm));
		}
		field(line, "thermal", th, sizeof(th));
		field(line, "driving", dr, sizeof(dr));

		if (t[0] == '\0')
			continue;
		printf("%s,%s,%s,%s,%s,%s,%s,%s,%s\n", t, soc, gpu, nand, cw, gw,
		       rpm, th, dr);
	}
	fclose(f);
	return 0;
}

int
fanpro_cmd_log(int argc, char **argv)
{
	int n = 40;

	if (argc >= 2 && strcmp(argv[1], "tail") == 0) {
		if (argc >= 3)
			n = atoi(argv[2]);
		if (n <= 0 || n > 10000)
			n = 40;
		return tail_file(DAEMON_LOG_PATH, n);
	}

	fputs("usage: fanpro log tail [lines]\n", stderr);
	return 2;
}

int
fanpro_cmd_history(int argc, char **argv)
{
	int n = 20;

	if (argc >= 2 && strcmp(argv[1], "--csv") == 0)
		return history_csv(FANPRO_HISTORY_FILE);

	if (argc >= 2)
		n = atoi(argv[1]);
	if (n <= 0 || n > 100000)
		n = 20;

	return tail_file(FANPRO_HISTORY_FILE, n);
}
