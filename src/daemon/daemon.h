/*
 * fanpro - internal daemon state.
 *
 * THE CONCURRENCY RULE, which the rest of this directory depends on:
 *
 *   The control loop thread is the SOLE owner of the SMC handle, the fan set,
 *   and the unlock state.  No other thread calls into smc/, fan/ or unlock.
 *
 * Other threads communicate only through the atomics below:
 *
 *   heartbeat  reads `tick`, sets `release_requested` if it stops advancing
 *   ipc        pushes validated commands onto `cmd`, reads `snapshot`
 *   power      sets `release_requested` / `reprobe_requested` on sleep/wake
 *
 * This is chosen over a mutex around the unlock state.  A watchdog calling
 * release while the loop sits between "Ftst asserted" and "manual_count++"
 * would clear Ftst from under a fan about to be marked held: exactly the
 * failure the refcount exists to prevent.  Single ownership removes the race
 * instead of guarding it.
 */
#ifndef FANPRO_DAEMON_H
#define FANPRO_DAEMON_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "fanpro/config.h"
#include "fanpro/curve.h"
#include "fanpro/fan.h"
#include "fanpro/history.h"
#include "fanpro/power.h"
#include "fanpro/proto.h"
#include "fanpro/sensors.h"
#include "fanpro/smc.h"
#include "fanpro/unlock.h"

/* Where the clean-exit marker lives.  /var/run is cleared on reboot, which is
 * what we want: a reboot is a clean slate, not an unclean exit. */
#define FANPRO_STATE_PATH "/var/run/fanpro.state"

#define FANPRO_CMD_QUEUE_LEN 16

typedef struct {
	fanpro_request_t req;
	bool             used;
} fanpro_cmd_t;

typedef struct {
	/* ---- owned exclusively by the control loop ---- */
	fanpro_smc_t         smc;
	fanpro_fan_set_t     fans;
	fanpro_unlock_t      unlock;
	fanpro_sensor_set_t  sensors;
	fanpro_curve_state_t curve_state[FANPRO_MAX_FANS];
	/*
	 * Set ONLY by an explicit `fanpro set`.  Distinct from curve_state
	 * .primed, which fanpro_curve_eval sets on its own first evaluation:
	 * conflating them meant that unbinding a curve left the fan pinned at
	 * whatever RPM that curve last produced, forever.
	 */
	bool                 manual_override[FANPRO_MAX_FANS];
	/* Latched while a panic is active, so it does not flap at 1 Hz. */
	bool                 panic_active[FANPRO_MAX_FANS];
	/* Last target we wrote, so the deadband has something to compare to. */
	double               last_target[FANPRO_MAX_FANS];
	fanpro_power_set_t   power;
	int                  sample_failures;
	/* Consecutive ticks on which another SMC client moved our state. */
	int                  drift_strikes;
	/* Clean ticks seen since the last drift, used to decay the strikes. */
	int                  drift_clean_ticks;
	fanpro_alert_state_t alert_state[FANPRO_MAX_ALERTS];
	/* History and alerts are off in tests: both do IO. */
	bool                 record_history;
	/*
	 * Where the clean-exit marker lives.  Injectable because the default
	 * is a real path in /var/run: with a live fanprod on the machine, unit
	 * tests were reading its marker and latching themselves.
	 */
	const char          *state_path;
	double               start_time;

	/* ---- config: written by the loop on reload, read under lock ---- */
	fanpro_config_t cfg;
	pthread_mutex_t cfg_lock;

	/* ---- cross-thread signals ---- */
	atomic_ullong tick;               /* bumped every control loop pass */
	atomic_bool   release_requested;  /* hand every fan back, now */
	/*
	 * Separate from release_requested on purpose.  Sleep is routine and
	 * must not latch; a heartbeat stall is a fault and must.  Conflating
	 * them left the daemon latched into monitor-only after every single
	 * sleep/wake cycle, silently.
	 */
	atomic_bool   latch_requested;
	/* Bumped by the control loop every time it completes a release, so the
	 * power thread can wait for one rather than assume it happened. */
	atomic_ullong release_generation;
	atomic_bool   reprobe_requested;  /* re-establish control after wake */
	atomic_bool   reload_requested;   /* SIGHUP */
	atomic_bool   stop;               /* shut down */
	atomic_bool   latched;            /* refuse control after unclean exit */

	/* ---- command queue: ipc thread -> control loop ---- */
	fanpro_cmd_t    cmd[FANPRO_CMD_QUEUE_LEN];
	pthread_mutex_t cmd_lock;

	/* ---- status snapshot: control loop -> ipc thread ---- */
	fanpro_response_t snapshot;
	pthread_mutex_t   snap_lock;
	/*
	 * Published for the IPC thread, which must not read d->fans directly:
	 * the loop memsets and rewrites that set on wake.
	 */
	atomic_int    pub_fan_count;
	atomic_uint   pub_heartbeat_timeout_s;

	/*
	 * Sensor sampling, injectable for the same reason the SMC backend and
	 * the clock are: the HID provider talks to real IOKit, so without this
	 * the daemon tests would silently read this machine's actual sensors
	 * and could never exercise "the sensors went away".
	 */
	int (*sample_sensors)(fanpro_sensor_set_t *set, fanpro_smc_t *smc,
	                      bool force_smc);

	/* ---- options ---- */
	bool foreground;
	/* Power sampling blocks for its measurement interval, which is fine
	 * once every ten seconds in production and intolerable in a test. */
	bool sample_power;
	char config_path[512];
} fanpro_daemon_t;

/* control_loop.c */
int  fanpro_control_loop_init(fanpro_daemon_t *d);
/*
 * Same, but against a caller-supplied SMC backend.  This is what makes the
 * daemon's logic testable: tests pass the in-memory fake and drive ticks by
 * hand instead of needing hardware, root, and real time.
 */
int  fanpro_control_loop_init_backend(fanpro_daemon_t *d,
                                      fanpro_smc_backend_t *be);
/* One pass of the loop.  Exposed so tests can step it deterministically. */
void fanpro_control_loop_tick(fanpro_daemon_t *d, unsigned long long pass);
void fanpro_control_loop_run(fanpro_daemon_t *d);
void fanpro_control_loop_shutdown(fanpro_daemon_t *d);

/* heartbeat.c */
int  fanpro_heartbeat_start(fanpro_daemon_t *d);
void fanpro_heartbeat_stop(void);

/* ipc_server.c */
int  fanpro_ipc_start(fanpro_daemon_t *d);
void fanpro_ipc_stop(void);

/* power_notify.c */
int  fanpro_power_notify_start(fanpro_daemon_t *d);
void fanpro_power_notify_stop(void);

/* Push a validated command.  Returns false when the queue is full. */
bool fanpro_daemon_push_cmd(fanpro_daemon_t *d, const fanpro_request_t *req);

/* Clean-exit marker, for the crash-loop latch. */
bool fanpro_daemon_previous_exit_was_clean(const char *path);
void fanpro_daemon_mark_running(const char *path);
void fanpro_daemon_mark_clean_exit(const char *path);

double fanpro_now_seconds(void);

#endif /* FANPRO_DAEMON_H */
