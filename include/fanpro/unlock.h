/*
 * fanpro - acquiring and releasing manual fan control.
 *
 * Three generations of firmware behave differently and fanpro must handle
 * all of them without knowing which machine it is on:
 *
 *   direct   a plain mode write to 1 is accepted (M1, M5, and this M3 Ultra
 *            on macOS 26.3, where the mode key is lowercase and Ftst does
 *            not exist at all)
 *   ftst     the mode write is refused with 0x82 while thermalmonitord holds
 *            the fan in mode 3; asserting Ftst makes it yield after a few
 *            seconds, then the write lands (M4)
 *   none     neither works; manual control is unavailable and fanpro says so
 *
 * The cheap path is always tried first, so a machine that does not need the
 * diagnostic key never touches it.
 *
 * Ftst is GLOBAL while fan mode is PER-FAN.  Clearing Ftst while another fan
 * is still manual silently kills that fan's control, so releases are
 * refcounted and the key is only cleared with the last fan.
 *
 * Time is injected.  The yield wait is several seconds on real hardware and
 * zero in tests.
 *
 * THREADING CONTRACT: none of this is thread-safe, by design.
 *
 * A fanpro_unlock_t must be touched by exactly one thread: in the daemon,
 * the control loop, which is the sole owner of the SMC handle.  The heartbeat
 * watchdog does NOT call into this module.  It watches an atomic counter the
 * loop bumps each tick and, on expiry, sets an atomic release-requested flag;
 * the control loop performs the release itself.
 *
 * That is a deliberate choice over adding a mutex.  A watchdog calling
 * release_all() while the loop sits between "Ftst asserted" and
 * "manual_count++" would clear Ftst out from under a fan about to be marked
 * held: precisely the failure the refcount exists to prevent.  Making the
 * SMC single-owner removes that race structurally rather than guarding it.
 *
 * The cost is that a control loop wedged inside a blocking SMC call cannot be
 * rescued from outside.  launchd's KeepAlive plus the startup recovery path
 * covers that case: the process is killed and the next start releases
 * everything before doing anything else.
 */
#ifndef FANPRO_UNLOCK_H
#define FANPRO_UNLOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "fanpro/fan.h"
#include "fanpro/smc.h"

/* Injected clock, so the state machine has no wall-clock dependency. */
typedef struct {
	uint64_t (*now_ms)(void *ctx);
	void     (*sleep_ms)(void *ctx, unsigned ms);
	void      *ctx;
} fanpro_clock_t;

/* The real clock: CLOCK_MONOTONIC and nanosleep. */
const fanpro_clock_t *fanpro_clock_system(void);

typedef enum {
	FANPRO_UNLOCK_PATH_UNKNOWN = 0,
	FANPRO_UNLOCK_PATH_DIRECT,
	FANPRO_UNLOCK_PATH_FTST,
	FANPRO_UNLOCK_PATH_NONE, /* proven unavailable on this machine */
} fanpro_unlock_path_t;

typedef struct {
	fanpro_smc_t         *smc;
	fanpro_fan_set_t     *fans;
	fanpro_clock_t        clock;

	fanpro_unlock_path_t  path;
	bool                  ftst_asserted;
	bool                  manual[FANPRO_MAX_FANS];
	int                   manual_count;

	/* Tunables, exposed so tests do not have to wait seconds. */
	unsigned yield_poll_ms;
	unsigned yield_timeout_ms;
	int      release_retries;
} fanpro_unlock_t;

void fanpro_unlock_init(fanpro_unlock_t *u, fanpro_smc_t *smc,
                        fanpro_fan_set_t *fans, const fanpro_clock_t *clock);

/*
 * Take manual control of one fan, discovering the path if not yet known.
 * Returns 0 on success, negative otherwise.  On failure nothing is left
 * asserted that was not already.
 */
int fanpro_unlock_acquire(fanpro_unlock_t *u, int fan_index);

/* Release one fan.  Clears Ftst only when it was the last manual fan. */
int fanpro_unlock_release(fanpro_unlock_t *u, int fan_index);

/* Release every fan and clear Ftst.  Safe to call when nothing is held. */
int fanpro_unlock_release_all(fanpro_unlock_t *u);

/*
 * Recovery for an unclean previous run: if the hardware shows a fan in
 * manual mode or Ftst asserted while this process holds nothing, force
 * everything back to firmware control.  Returns the number of fans it had to
 * take back, or negative on failure.
 *
 * STARTUP ONLY.  It cannot tell "left by a dead process" from "held by this
 * one", so it refuses to run while manual_count > 0.  Anything wanting an
 * emergency stop mid-run wants fanpro_unlock_release_all instead.
 */
int fanpro_unlock_recover(fanpro_unlock_t *u);

/*
 * Has the hardware drifted from what we last wrote?  Another fan-control tool
 * holding the SMC at the same time can flip a mode or clear Ftst underneath
 * us.  Returns true when the observed state disagrees with our record.
 */
bool fanpro_unlock_detect_drift(fanpro_unlock_t *u);

const char *fanpro_unlock_path_name(fanpro_unlock_path_t p);

#endif /* FANPRO_UNLOCK_H */
