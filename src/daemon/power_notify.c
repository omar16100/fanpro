/*
 * fanpro - sleep and wake notifications.
 *
 * Releases the fans before the machine sleeps or powers off, and asks the
 * control loop to re-probe on wake.
 *
 * Re-probe rather than assume: the claim that firmware clears Ftst on sleep
 * is inferred rather than confirmed in the source research, and this machine
 * has no Ftst at all.  Either way, what the firmware did while we were asleep
 * is not knowable from here, so the loop re-enumerates.
 *
 * Runs its own CFRunLoop thread because IORegisterForSystemPower needs one.
 * It only sets atomic flags; the control loop owns the SMC.
 */
#include "daemon.h"

#include "fanpro/log.h"

#include <stdio.h>

#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

/* IOKit allows roughly 30 s to acknowledge; this is a small slice of it. */
#define POWER_RELEASE_WAIT_MS 3000

static pthread_t        g_thread;
static atomic_bool      g_running;
static fanpro_daemon_t *g_daemon;
static io_connect_t     g_root_port;
static CFRunLoopRef     g_runloop;

static void
power_callback(void *refcon, io_service_t service, natural_t type, void *arg)
{
	fanpro_daemon_t *d = (fanpro_daemon_t *)refcon;

	(void)service;

	switch (type) {
	case kIOMessageSystemWillSleep:
	case kIOMessageSystemWillPowerOff: {
		unsigned long long before = atomic_load(&d->release_generation);
		int waited_ms = 0;

		FANPRO_WARN("power.notify", "event=%s action=release",
		            type == kIOMessageSystemWillSleep ? "will_sleep"
		                                              : "will_power_off");
		atomic_store(&d->release_requested, true);

		/*
		 * Wait for the release to actually complete before acknowledging.
		 * Previously this acked immediately, so the machine could suspend
		 * up to a tick before the fans were handed back, and the comment
		 * claimed a guarantee the code did not provide.  IOKit allows
		 * roughly 30 s here, so a couple of seconds is cheap.
		 */
		while (waited_ms < POWER_RELEASE_WAIT_MS) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000L };

			if (atomic_load(&d->release_generation) != before)
				break;
			nanosleep(&ts, NULL);
			waited_ms += 50;
		}
		if (atomic_load(&d->release_generation) == before)
			FANPRO_ERROR("power.notify",
			             "reason=release_not_confirmed waited_ms=%d",
			             waited_ms);
		else
			FANPRO_INFO("power.notify", "release_confirmed_ms=%d", waited_ms);

		IOAllowPowerChange(g_root_port, (long)arg);
		break;
	}

	case kIOMessageCanSystemSleep:
		IOAllowPowerChange(g_root_port, (long)arg);
		break;

	case kIOMessageSystemHasPoweredOn:
		FANPRO_INFO("power.notify", "event=powered_on action=reprobe");
		atomic_store(&d->reprobe_requested, true);
		break;

	default:
		break;
	}
}

static void *
power_main(void *arg)
{
	fanpro_daemon_t *d = (fanpro_daemon_t *)arg;
	IONotificationPortRef port = NULL;
	io_object_t notifier = 0;

	pthread_setname_np("fanpro-power");

	g_root_port = IORegisterForSystemPower(d, &port, power_callback, &notifier);
	if (g_root_port == MACH_PORT_NULL) {
		FANPRO_ERROR("power.notify", "reason=register_failed");
		return NULL;
	}

	g_runloop = CFRunLoopGetCurrent();
	CFRunLoopAddSource(g_runloop, IONotificationPortGetRunLoopSource(port),
	                   kCFRunLoopDefaultMode);

	FANPRO_INFO("power.notify", "registered=1");
	while (atomic_load(&g_running) && !atomic_load(&d->stop))
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);

	CFRunLoopRemoveSource(g_runloop, IONotificationPortGetRunLoopSource(port),
	                      kCFRunLoopDefaultMode);
	IODeregisterForSystemPower(&notifier);
	IOServiceClose(g_root_port);
	IONotificationPortDestroy(port);
	return NULL;
}

int
fanpro_power_notify_start(fanpro_daemon_t *d)
{
	g_daemon = d;
	atomic_store(&g_running, true);
	if (pthread_create(&g_thread, NULL, power_main, d) != 0) {
		FANPRO_ERROR("power.notify", "reason=pthread_create_failed");
		atomic_store(&g_running, false);
		return -1;
	}
	return 0;
}

void
fanpro_power_notify_stop(void)
{
	if (!atomic_load(&g_running))
		return;
	atomic_store(&g_running, false);
	if (g_runloop != NULL)
		CFRunLoopStop(g_runloop);
	pthread_join(g_thread, NULL);
	g_daemon = NULL;
}
