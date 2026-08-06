/*
 * fanpro - the daemon.
 *
 * Startup order is fixed and load-bearing:
 *
 *   1. open the SMC, enumerate fans
 *   2. recover: force back to auto anything an unclean previous run left
 *      pinned.  On this hardware nothing else ever will
 *   3. engage the crash-loop latch if the last exit was unclean
 *   4. load config, start the auxiliary threads, run the loop
 *
 * Shutdown always releases the fans, on every path we can catch.  SIGKILL we
 * cannot catch, which is exactly what steps 2 and 3 exist for.
 */
#include "daemon.h"

#include "fanpro/log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static fanpro_daemon_t g_daemon;
/* Set from the signal handler only; everything else happens in the loop. */
static volatile sig_atomic_t g_signal_seen;

#define LOG_PATH     "/var/log/fanpro/fanprod.log"
#define LOG_MAX_BYTE (8u * 1024u * 1024u)
#define LOG_KEEP     5

static void
on_signal(int sig)
{
	g_signal_seen = sig;

	switch (sig) {
	case SIGHUP:
		/* Conventional meaning: reload, not shut down. */
		atomic_store(&g_daemon.reload_requested, true);
		break;
	default:
		atomic_store(&g_daemon.stop, true);
		break;
	}
}

/*
 * Release on the way out no matter how we got here.  Registered with atexit
 * as well as being called from main, because an unexpected exit path leaving
 * a fan pinned is the worst outcome available on this hardware.
 */
static void
emergency_release(void)
{
	if (g_daemon.unlock.manual_count > 0 || g_daemon.unlock.ftst_asserted) {
		FANPRO_ERROR("daemon.atexit", "held=%d action=release",
		             g_daemon.unlock.manual_count);
		fanpro_unlock_release_all(&g_daemon.unlock);
	}
	fanpro_daemon_mark_clean_exit(g_daemon.state_path);
}

static void
install_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	/* A closed socket peer must not kill the daemon mid-release. */
	signal(SIGPIPE, SIG_IGN);
	/* Notification helpers are spawned and never waited on; this reaps
	 * them rather than accumulating zombies. */
	signal(SIGCHLD, SIG_IGN);

	atexit(emergency_release);
}

bool
fanpro_daemon_push_cmd(fanpro_daemon_t *d, const fanpro_request_t *req)
{
	int i;
	bool ok = false;

	pthread_mutex_lock(&d->cmd_lock);
	for (i = 0; i < FANPRO_CMD_QUEUE_LEN; i++) {
		if (!d->cmd[i].used) {
			d->cmd[i].req = *req;
			d->cmd[i].used = true;
			ok = true;
			break;
		}
	}
	pthread_mutex_unlock(&d->cmd_lock);
	return ok;
}

/* ---- single instance ---------------------------------------------------- */

#define PIDFILE_PATH "/var/run/fanprod.pid"

static int g_pidfile_fd = -1;

static bool
acquire_single_instance(void)
{
	char buf[32];
	int n;

	g_pidfile_fd = open(PIDFILE_PATH, O_RDWR | O_CREAT, 0600);
	if (g_pidfile_fd < 0)
		return true; /* cannot enforce it; do not refuse to start */

	if (flock(g_pidfile_fd, LOCK_EX | LOCK_NB) != 0) {
		close(g_pidfile_fd);
		g_pidfile_fd = -1;
		return false;
	}

	n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
	if (ftruncate(g_pidfile_fd, 0) == 0)
		(void)!write(g_pidfile_fd, buf, (size_t)n);
	/* The lock is released automatically when the process exits, which is
	 * the point: a crashed daemon does not block the next one. */
	return true;
}

/* ---- clean-exit marker, for the crash-loop latch ------------------------ */

bool
fanpro_daemon_previous_exit_was_clean(const char *path)
{
	char buf[16] = { 0 };
	int fd;
	ssize_t n;

	if (path == NULL)
		path = FANPRO_STATE_PATH;
	fd = open(path, O_RDONLY);

	/*
	 * No marker means either a fresh boot (/var/run is cleared) or a first
	 * run.  Both are clean starts.  The marker only ever says "a daemon was
	 * running and did not shut down properly".
	 */
	if (fd < 0)
		return true;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	/*
	 * The marker EXISTS, so a daemon was running and did not remove it.
	 * An empty or truncated marker means we crashed between creating it
	 * and writing it, which is the unclean case, not the clean one.
	 */
	if (n <= 0)
		return false;

	return strncmp(buf, "running", 7) != 0;
}

void
fanpro_daemon_mark_running(const char *path)
{
	int fd;

	if (path == NULL)
		path = FANPRO_STATE_PATH;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0) {
		/*
		 * Without this marker the crash-loop latch cannot work: the next
		 * start will see no marker, read that as a clean exit, and
		 * happily re-pin the fans after a crash.  Failing silently would
		 * disable a safety mechanism with no trace.
		 */
		FANPRO_ERROR("daemon.marker",
		             "path=%s reason=cannot_write err=%s "
		             "impact=crash_loop_latch_disabled",
		             path, strerror(errno));
		return;
	}
	if (write(fd, "running", 7) != 7)
		FANPRO_ERROR("daemon.marker", "path=%s reason=short_write", path);
	close(fd);
}

void
fanpro_daemon_mark_clean_exit(const char *path)
{
	unlink(path != NULL ? path : FANPRO_STATE_PATH);
}

/* ------------------------------------------------------------------------ */

static void
usage(void)
{
	fputs("usage: fanprod [-f] [-v] [-c /etc/fanpro/fanpro.conf]\n"
	      "  -f  run in the foreground, logging to stderr\n"
	      "  -v  more verbose (repeatable)\n"
	      "  -c  config file path\n",
	      stderr);
}

int
main(int argc, char **argv)
{
	fanpro_log_level_t level = FANPRO_LOG_INFO;
	bool released_cleanly = false;
	int opt, rc = 0;

	memset(&g_daemon, 0, sizeof(g_daemon));
	pthread_mutex_init(&g_daemon.cfg_lock, NULL);
	pthread_mutex_init(&g_daemon.cmd_lock, NULL);
	pthread_mutex_init(&g_daemon.snap_lock, NULL);
	g_daemon.sample_power = true;
	g_daemon.record_history = true;
	snprintf(g_daemon.config_path, sizeof(g_daemon.config_path), "%s",
	         FANPRO_DEFAULT_CONFIG_PATH);

	while ((opt = getopt(argc, argv, "fvc:h")) != -1) {
		switch (opt) {
		case 'f': g_daemon.foreground = true; break;
		case 'v': if (level < FANPRO_LOG_TRACE) level++; break;
		case 'c':
			snprintf(g_daemon.config_path,
			         sizeof(g_daemon.config_path), "%s", optarg);
			break;
		case 'h': usage(); return 0;
		default:  usage(); return 2;
		}
	}
	fanpro_log_set_level(level);

	if (geteuid() != 0) {
		fputs("fanprod: must run as root (it writes to the SMC)\n", stderr);
		return 1;
	}

	/*
	 * Exactly one daemon.  Two would each believe they were the sole SMC
	 * owner: each one's drift detector would see the other's mode writes,
	 * both would thrash between release and re-acquire, and whichever
	 * exited first would remove the shared state marker out from under the
	 * other.
	 */
	if (!acquire_single_instance()) {
		fputs("fanprod: another instance is already running\n", stderr);
		return 1;
	}

	/* Registered before the log file is opened: atexit is LIFO, so this
	 * ensures the handler runs before fanpro_log_close, not after it. */
	install_signals();

	if (!g_daemon.foreground) {
		mkdir("/var/log/fanpro", 0755);
		if (fanpro_log_open_file(LOG_PATH, LOG_MAX_BYTE, LOG_KEEP) != 0)
			fprintf(stderr, "fanprod: cannot open %s, using stderr\n",
			        LOG_PATH);
	}

	FANPRO_INFO("daemon.start", "pid=%d foreground=%d", (int)getpid(),
	            (int)g_daemon.foreground);

	if (fanpro_control_loop_init(&g_daemon) != 0) {
		FANPRO_ERROR("daemon.start", "reason=init_failed");
		rc = 1;
		goto out;
	}

	if (g_daemon.record_history &&
	    fanpro_history_open(FANPRO_HISTORY_FILE) != 0)
		FANPRO_WARN("daemon.start", "reason=history_unavailable");

	if (fanpro_ipc_start(&g_daemon) != 0) {
		FANPRO_ERROR("daemon.start", "reason=ipc_failed");
		rc = 1;
		goto out;
	}
	if (fanpro_heartbeat_start(&g_daemon) != 0) {
		rc = 1;
		goto out;
	}
	/* Not fatal: without it we lose sleep/wake handling, but the daemon is
	 * still safe because every other release path remains. */
	if (fanpro_power_notify_start(&g_daemon) != 0)
		FANPRO_WARN("daemon.start", "reason=power_notify_unavailable");

	fanpro_control_loop_run(&g_daemon);

	FANPRO_INFO("daemon.stop", "signal=%d", (int)g_signal_seen);

out:
	/*
	 * ORDER MATTERS.  Release the fans FIRST, before joining any thread.
	 *
	 * Joining the IPC thread can block indefinitely (a client that connects
	 * and never sends leaves it in read()).  If the release came after the
	 * joins, that hang would leave fans pinned at their last target with
	 * the process still alive, so launchd would not restart it and even the
	 * atexit handler would never run.  On this hardware nothing else frees
	 * a pinned fan, so release is the one shutdown step that must not be
	 * contingent on another thread dying cleanly.
	 */
	released_cleanly = (fanpro_unlock_release_all(&g_daemon.unlock) == 0);

	fanpro_power_notify_stop();
	fanpro_heartbeat_stop();
	fanpro_ipc_stop();
	fanpro_smc_close(&g_daemon.smc);

	/*
	 * Only claim a clean exit if the fans really went back.  Otherwise
	 * leave the marker so the next start latches: "we could not hand the
	 * fans back" is exactly the case the latch exists for.
	 */
	if (released_cleanly)
		fanpro_daemon_mark_clean_exit(g_daemon.state_path);
	else
		FANPRO_ERROR("daemon.stop",
		             "reason=release_failed action=leaving_unclean_marker");

	fanpro_history_close();
	fanpro_log_close();
	return rc;
}
