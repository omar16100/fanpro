/*
 * fanpro - heartbeat watchdog.
 *
 * Covers the gap launchd cannot: KeepAlive only restarts on process EXIT, so
 * a control loop that deadlocks or live-locks stays alive holding fans at a
 * stale target forever.  On this hardware nothing underneath will correct
 * that, because the firmware does not override a fan we hold.
 *
 * This thread deliberately does NOT touch the SMC.  It watches the tick
 * counter and raises a flag; the control loop, which is the sole SMC owner,
 * performs the release.  That makes the acquire/release race structurally
 * impossible rather than merely guarded.  The cost is that a loop wedged
 * inside a blocking SMC call cannot be rescued from here: launchd restarting
 * the process, plus startup recovery, covers that.
 */
#include "daemon.h"

#include "fanpro/log.h"

#include <stdio.h>
#include <time.h>

static pthread_t      g_thread;
static atomic_bool    g_running;
static fanpro_daemon_t *g_daemon;

static void *
heartbeat_main(void *arg)
{
	fanpro_daemon_t *d = (fanpro_daemon_t *)arg;
	unsigned long long last_tick = 0;
	unsigned stalled_s = 0;
	unsigned timeout_s;
	bool fired = false;

	pthread_setname_np("fanpro-heartbeat");

	while (atomic_load(&g_running) && !atomic_load(&d->stop)) {
		struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
		unsigned long long now_tick;

		nanosleep(&ts, NULL);
		now_tick = atomic_load(&d->tick);

		if (now_tick != last_tick) {
			last_tick = now_tick;
			stalled_s = 0;
			fired = false;
			continue;
		}

		stalled_s++;
		/* The published atomic, not d->cfg: the control loop rewrites
		 * all of cfg under its lock during a reload. */
		timeout_s = atomic_load(&d->pub_heartbeat_timeout_s);
		if (timeout_s == 0)
			timeout_s = 10;
		if (stalled_s < timeout_s || fired)
			continue;

		/*
		 * Raise the flag, do not act.  If the loop is merely slow it
		 * will see this and release itself; if it is truly wedged the
		 * flag goes unread and only a restart helps, which is what
		 * launchd is for.
		 */
		FANPRO_ERROR("heartbeat.stall",
		             "no_tick_for_s=%u timeout_s=%u action=request_release",
		             stalled_s, timeout_s);
		/* A stall IS a fault, so this one latches as well as releasing.
		 * Sleep, which also releases, deliberately does not. */
		atomic_store(&d->latch_requested, true);
		atomic_store(&d->release_requested, true);
		fired = true;
	}
	return NULL;
}

int
fanpro_heartbeat_start(fanpro_daemon_t *d)
{
	g_daemon = d;
	atomic_store(&g_running, true);
	if (pthread_create(&g_thread, NULL, heartbeat_main, d) != 0) {
		FANPRO_ERROR("heartbeat.start", "reason=pthread_create_failed");
		atomic_store(&g_running, false);
		return -1;
	}
	FANPRO_INFO("heartbeat.start", "timeout_s=%u",
	            atomic_load(&d->pub_heartbeat_timeout_s));
	return 0;
}

void
fanpro_heartbeat_stop(void)
{
	if (!atomic_load(&g_running))
		return;
	atomic_store(&g_running, false);
	pthread_join(g_thread, NULL);
	g_daemon = NULL;
}
