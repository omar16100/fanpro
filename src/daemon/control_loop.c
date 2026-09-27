/*
 * fanpro - the control loop.
 *
 * The only thread that touches the SMC.  One pass per tick:
 *
 *   0. honour anything the other threads asked for (release, reload, wake)
 *   1. drain the command queue
 *   2. sample sensors and re-read fan state
 *   3. detect drift: has another SMC client moved what we wrote?
 *   4. per fan: curve -> safety gate -> write, past a deadband
 *   5. publish a snapshot and bump the heartbeat
 *
 * On this hardware the firmware does not rescue a fan we hold (measured; see
 * docs/smc_reference.md), so every path that loses confidence releases rather
 * than carries on.
 */
#include "daemon.h"

#include "fanpro/history.h"
#include "fanpro/log.h"
#include "fanpro/power.h"
#include "fanpro/safety.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Transient drift is tolerated; persistent drift means another tool owns the
 * fans and fanpro should stop competing for them.
 *
 * Strikes decay over CLEAN ticks rather than resetting on the first one.  A
 * drift tick releases the fan, so the very next tick sees no drift by
 * construction: resetting there made the counter unable to exceed 1 and the
 * latch unreachable, which hardware testing found.
 */
#define DRIFT_STRIKES_BEFORE_LATCH 3
#define DRIFT_CLEAN_TICKS_TO_DECAY 10

/* A panic stays engaged until the temperature falls this far below the
 * threshold, so it does not release/re-acquire once per second. */
#define PANIC_CLEAR_HYSTERESIS_C 5.0

/*
 * How far the firmware's reported target may sit from what we last wrote
 * before we call it interference.  Wide enough to absorb float rounding and
 * a fan that has not yet applied our write.
 */
#define TARGET_DRIFT_TOLERANCE_RPM 75.0

double
fanpro_now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
sleep_ms(unsigned ms)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	nanosleep(&ts, NULL);
}

int
fanpro_control_loop_init(fanpro_daemon_t *d)
{
	return fanpro_control_loop_init_backend(d, fanpro_smc_backend_iokit());
}

int
fanpro_control_loop_init_backend(fanpro_daemon_t *d, fanpro_smc_backend_t *be)
{
	char err[FANPRO_CONFIG_ERR_MAX];
	int i;

	if (fanpro_smc_open(&d->smc, be) != 0) {
		FANPRO_ERROR("daemon.init", "reason=smc_open_failed");
		return -1;
	}
	if (fanpro_fan_enumerate(&d->smc, &d->fans) != 0) {
		FANPRO_ERROR("daemon.init", "reason=fan_enumerate_failed");
		return -1;
	}

	fanpro_unlock_init(&d->unlock, &d->smc, &d->fans, fanpro_clock_system());

	/*
	 * Recovery FIRST, before anything else touches a fan.  Residue from an
	 * unclean exit is the only thing that can leave a fan pinned on this
	 * machine, because there is no Ftst and therefore no reclaim.
	 */
	{
		int stale = fanpro_unlock_recover(&d->unlock);

		if (stale > 0)
			FANPRO_WARN("daemon.init",
			            "recovered_fans=%d reason=unclean_previous_run",
			            stale);
	}

	/*
	 * Crash-loop latch.  Without it, a daemon that dies mid-run gets
	 * restarted by launchd and immediately re-pins the fans, alternating
	 * between "pinned by a dying daemon" and "pinned by a fresh one".
	 */
	if (!fanpro_daemon_previous_exit_was_clean(d->state_path)) {
		atomic_store(&d->latched, true);
		FANPRO_ERROR("daemon.latch",
		             "state=engaged reason=unclean_previous_exit "
		             "action=staying_in_auto hint=run 'fanpro daemon enable'");
	}
	fanpro_daemon_mark_running(d->state_path);

	if (fanpro_config_load(&d->cfg, d->config_path, err, sizeof(err)) != 0) {
		FANPRO_ERROR("daemon.config", "path=%s error=%s", d->config_path, err);
		return -1;
	}
	fanpro_log_set_level(d->cfg.log_level);

	for (i = 0; i < FANPRO_MAX_FANS; i++) {
		memset(&d->curve_state[i], 0, sizeof(d->curve_state[i]));
		d->last_target[i] = NAN;
	}
	d->start_time = fanpro_now_seconds();
	if (d->sample_sensors == NULL)
		d->sample_sensors = fanpro_sensors_enumerate;

	/* Published so other threads never read d->fans / d->cfg directly. */
	atomic_store(&d->pub_fan_count, d->fans.count);
	atomic_store(&d->pub_heartbeat_timeout_s, d->cfg.heartbeat_timeout_s);

	FANPRO_INFO("daemon.init",
	            "fans=%d mode=%s curves=%d latched=%d config=%s",
	            d->fans.count, fanpro_config_mode_name(d->cfg.mode),
	            d->cfg.n_curves, (int)atomic_load(&d->latched), d->config_path);
	return 0;
}

/* Forget everything we believed about one fan. */
static void
forget_fan(fanpro_daemon_t *d, int i)
{
	memset(&d->curve_state[i], 0, sizeof(d->curve_state[i]));
	d->manual_override[i] = false;
	d->panic_active[i] = false;
	d->last_target[i] = NAN;
}

/* Release one fan and forget it, so nothing re-acquires it next tick. */
static void
release_fan(fanpro_daemon_t *d, int i, const char *reason)
{
	FANPRO_WARN("fan.release", "fan=%d reason=%s", i, reason);
	fanpro_unlock_release(&d->unlock, i);
	forget_fan(d, i);
}

/* Hand every fan back and forget our curve state. */
static void
release_everything(fanpro_daemon_t *d, const char *reason)
{
	int i;

	FANPRO_WARN("daemon.release", "reason=%s", reason);
	fanpro_unlock_release_all(&d->unlock);
	for (i = 0; i < FANPRO_MAX_FANS; i++)
		forget_fan(d, i);
	/* Publish that a release completed, so the power thread can hold off
	 * acknowledging sleep until the fans are genuinely back. */
	atomic_fetch_add(&d->release_generation, 1);
}

static void
handle_command(fanpro_daemon_t *d, const fanpro_request_t *req)
{
	char err[FANPRO_CONFIG_ERR_MAX];

	FANPRO_INFO("daemon.cmd", "verb=%s fan=%d rpm=%.0f name=%s",
	            fanpro_verb_name(req->verb), req->fan_index, req->rpm,
	            req->name);

	switch (req->verb) {
	case FANPRO_VERB_SET_MODE:
		pthread_mutex_lock(&d->cfg_lock);
		d->cfg.mode = (req->ival == 1) ? FANPRO_MODE_CURVE : FANPRO_MODE_AUTO;
		pthread_mutex_unlock(&d->cfg_lock);
		if (d->cfg.mode == FANPRO_MODE_AUTO)
			release_everything(d, "mode_set_to_auto");
		break;

	case FANPRO_VERB_APPLY_CURVE: {
		int found = -1, k;

		pthread_mutex_lock(&d->cfg_lock);
		for (k = 0; k < d->cfg.n_curves; k++) {
			if (strcmp(d->cfg.curves[k].name, req->name) == 0) {
				found = k;
				break;
			}
		}
		/* The config parser rejects a fan bound to a curve that does not
		 * exist; the IPC path must not reintroduce that hole. */
		if (found >= 0)
			snprintf(d->cfg.fan_curve[req->fan_index],
			         sizeof(d->cfg.fan_curve[0]), "%s", req->name);
		pthread_mutex_unlock(&d->cfg_lock);

		if (found < 0) {
			FANPRO_ERROR("daemon.cmd",
			             "verb=apply-curve fan=%d name=%s reason=no_such_curve",
			             req->fan_index, req->name);
			break;
		}
		forget_fan(d, req->fan_index);
		break;
	}

	case FANPRO_VERB_SET_FAN:
		if (isnan(req->rpm)) {
			/*
			 * Back to firmware control for this fan.  Clearing the
			 * curve state is not optional: it holds the manual
			 * override, and leaving it primed would make the very
			 * next tick re-acquire the fan and re-apply the speed we
			 * were just asked to stop applying.
			 */
			if (req->fan_index < 0) {
				release_everything(d, "cli_set_all_auto");
			} else {
				int f = req->fan_index;

				pthread_mutex_lock(&d->cfg_lock);
				d->cfg.fan_curve[f][0] = '\0';
				pthread_mutex_unlock(&d->cfg_lock);
				release_fan(d, f, "cli_set_auto");
			}
		} else {
			/* A manual override is volatile by design: it lives in
			 * curve state, not config, so it does not survive a
			 * restart.  Persistence belongs in the config file. */
			int lo = (req->fan_index < 0) ? 0 : req->fan_index;
			int hi = (req->fan_index < 0) ? d->fans.count - 1
			                              : req->fan_index;
			int i;

			for (i = lo; i <= hi && i < d->fans.count; i++) {
				pthread_mutex_lock(&d->cfg_lock);
				d->cfg.fan_curve[i][0] = '\0'; /* drop any curve */
				pthread_mutex_unlock(&d->cfg_lock);
				d->manual_override[i] = true;
				d->curve_state[i].primed = true;
				d->curve_state[i].last_rpm = req->rpm;
				d->curve_state[i].last_temp = 0.0;
			}
		}
		break;

	case FANPRO_VERB_RELEASE_ALL:
		release_everything(d, "cli_release_all");
		break;

	case FANPRO_VERB_ENABLE:
		atomic_store(&d->latched, false);
		FANPRO_WARN("daemon.latch", "state=cleared reason=operator_request");
		break;

	case FANPRO_VERB_RELOAD: {
		char was[FANPRO_MAX_FANS][32];
		int k;

		pthread_mutex_lock(&d->cfg_lock);
		for (k = 0; k < FANPRO_MAX_FANS; k++)
			snprintf(was[k], sizeof(was[k]), "%s", d->cfg.fan_curve[k]);

		if (fanpro_config_load(&d->cfg, d->config_path, err, sizeof(err)) != 0) {
			FANPRO_ERROR("daemon.reload", "error=%s", err);
			pthread_mutex_unlock(&d->cfg_lock);
			break;
		}
		FANPRO_INFO("daemon.reload", "ok mode=%s curves=%d",
		            fanpro_config_mode_name(d->cfg.mode), d->cfg.n_curves);

		/*
		 * Alert state is indexed by rule position, so reordering,
		 * inserting or deleting an [alert.*] section would shift every
		 * slot onto a different rule.  The realistic failure is a rule
		 * inheriting firing=true and staying silent through its first
		 * genuine crossing.  Zeroing is cheap and correct.
		 */
		memset(d->alert_state, 0, sizeof(d->alert_state));

		/* Hysteresis and slew state belong to the curve that produced
		 * them; carrying it into a different curve shapes the new one
		 * from the old one's history. */
		for (k = 0; k < FANPRO_MAX_FANS; k++) {
			if (strcmp(was[k], d->cfg.fan_curve[k]) != 0)
				forget_fan(d, k);
		}
		pthread_mutex_unlock(&d->cfg_lock);
		fanpro_log_set_level(d->cfg.log_level);
		atomic_store(&d->pub_heartbeat_timeout_s, d->cfg.heartbeat_timeout_s);
		break;
	}

	default:
		break;
	}
}

static void
drain_commands(fanpro_daemon_t *d)
{
	fanpro_request_t batch[FANPRO_CMD_QUEUE_LEN];
	int n = 0, i;

	pthread_mutex_lock(&d->cmd_lock);
	for (i = 0; i < FANPRO_CMD_QUEUE_LEN; i++) {
		if (d->cmd[i].used) {
			batch[n++] = d->cmd[i].req;
			d->cmd[i].used = false;
		}
	}
	pthread_mutex_unlock(&d->cmd_lock);

	for (i = 0; i < n; i++)
		handle_command(d, &batch[i]);
}

static void
publish_snapshot(fanpro_daemon_t *d, int thermal_level,
                 const fanpro_power_set_t *power)
{
	fanpro_response_t snap;
	int i, n = 0;

	fanpro_response_init(&snap);
	snap.status = 0;
	snap.mode = (int32_t)d->cfg.mode;
	snap.latched = (int32_t)atomic_load(&d->latched);
	snap.thermal_level = thermal_level;
	snap.uptime_s = fanpro_now_seconds() - d->start_time;
	snap.cpu_watts = 0.0;
	snap.gpu_watts = 0.0;

	if (power != NULL) {
		for (i = 0; i < power->count; i++) {
			if (strcmp(power->channels[i].name, "CPU") == 0)
				snap.cpu_watts = power->channels[i].watts;
			else if (strcmp(power->channels[i].name, "GPU") == 0)
				snap.gpu_watts = power->channels[i].watts;
		}
	}

	snap.n_fans = d->fans.count > FANPRO_PROTO_MAX_FANS
	                  ? FANPRO_PROTO_MAX_FANS : d->fans.count;
	for (i = 0; i < snap.n_fans; i++) {
		const fanpro_fan_t *f = &d->fans.fans[i];

		snap.fans[i].index = i;
		snap.fans[i].mode = f->mode;
		snap.fans[i].held = d->unlock.manual[i] ? 1 : 0;
		snap.fans[i].rpm = f->rpm;
		snap.fans[i].target = f->target;
		snap.fans[i].min_rpm = f->min_rpm;
		snap.fans[i].max_rpm = f->max_rpm;
		snprintf(snap.fans[i].curve, sizeof(snap.fans[i].curve), "%s",
		         d->cfg.fan_curve[i]);
	}

	/* Hottest sensor per class is what fits and what matters. */
	for (i = 0; i < FANPRO_CLASS_COUNT && n < FANPRO_PROTO_MAX_SENSORS; i++) {
		double hottest = fanpro_sensors_max_of_class(
		    &d->sensors, (fanpro_sensor_class_t)i);

		if (!isfinite(hottest))
			continue;
		snprintf(snap.sensors[n].name, sizeof(snap.sensors[n].name),
		         "%s", fanpro_sensor_class_name((fanpro_sensor_class_t)i));
		snap.sensors[n].celsius = hottest;
		snap.sensors[n].cls = i;
		n++;
	}
	snap.n_sensors = n;

	pthread_mutex_lock(&d->snap_lock);
	d->snapshot = snap;
	pthread_mutex_unlock(&d->snap_lock);
}

/* One fan, one tick.  Returns true if it is currently being driven. */
static bool
drive_fan(fanpro_daemon_t *d, int i, int thermal_level)
{
	const fanpro_curve_t *curve;
	fanpro_fan_t *fan = &d->fans.fans[i];
	fanpro_safety_result_t verdict;
	double proposed = NAN;
	double temp;
	int rc;

	if (!fan->mode_key_known)
		return false;

	pthread_mutex_lock(&d->cfg_lock);
	curve = fanpro_config_curve_for_fan(&d->cfg, i);
	pthread_mutex_unlock(&d->cfg_lock);

	if (curve != NULL) {
		temp = fanpro_curve_input(curve, &d->sensors);
		proposed = fanpro_curve_eval(curve, &d->curve_state[i], temp,
		                             fan->max_rpm);
	} else if (d->manual_override[i]) {
		/* An explicit `fanpro set`, with no curve: hold what was asked
		 * for.  Gated on manual_override rather than curve_state.primed,
		 * because the curve evaluator primes that itself and an unbound
		 * curve would otherwise pin the fan at its last value forever. */
		proposed = d->curve_state[i].last_rpm;
	} else {
		/* Nothing wants this fan.  If we were holding it, let go. */
		if (d->unlock.manual[i])
			release_fan(d, i, "no_curve_or_override");
		return false;
	}

	/* The one gate.  Every target reaches the SMC through here. */
	verdict = fanpro_safety_check(&d->cfg.safety, fan, proposed, &d->sensors,
	                              d->sample_failures, thermal_level);

	if (verdict.verdict == FANPRO_SAFETY_RELEASE) {
		/* Forget the fan as well as releasing it.  Leaving the override
		 * primed made this flap: release, re-acquire next tick, release
		 * again, once per second for as long as the condition lasted. */
		if (d->unlock.manual[i])
			release_fan(d, i, verdict.reason);
		return false;
	}

	if (!d->unlock.manual[i]) {
		if (fanpro_unlock_acquire(&d->unlock, i) != 0) {
			FANPRO_ERROR("fan.acquire", "fan=%d reason=failed verdict=%s", i,
			             fanpro_safety_verdict_name(verdict.verdict));
			/* During a panic, try the target anyway: the fan may
			 * already be in manual from a previous pass, and a
			 * failed acquire is no reason to skip the one write that
			 * might save the hardware. */
			if (verdict.verdict == FANPRO_SAFETY_PANIC)
				fanpro_smc_write_num(&d->smc, fan->key_tg, verdict.rpm);
			return false;
		}
	}

	if (verdict.verdict == FANPRO_SAFETY_PANIC) {
		/*
		 * Drive it to maximum and STAY there while the panic lasts.
		 * Releasing immediately made the mode flap at 1 Hz, and in each
		 * gap the firmware was free to slow the fan back down: the
		 * opposite of what a panic should achieve.
		 */
		if (!d->panic_active[i]) {
			FANPRO_ERROR("fan.panic",
			             "fan=%d class=%s temp=%.1f action=hold_at_max", i,
			             fanpro_sensor_class_name(verdict.panic_class),
			             verdict.panic_temp);
			d->panic_active[i] = true;
		}
		fanpro_smc_write_num(&d->smc, fan->key_tg, verdict.rpm);
		d->last_target[i] = verdict.rpm;
		return true;
	}

	/* Panic clears only once the temperature is comfortably back down,
	 * so the two states cannot alternate around the threshold. */
	if (d->panic_active[i]) {
		double hottest = fanpro_sensors_max_of_class(&d->sensors,
		                                             FANPRO_CLASS_SOC);
		double limit = d->cfg.safety.panic_c[FANPRO_CLASS_SOC];

		if (isfinite(hottest) && isfinite(limit) &&
		    hottest > limit - PANIC_CLEAR_HYSTERESIS_C) {
			fanpro_smc_write_num(&d->smc, fan->key_tg, fan->max_rpm);
			return true;
		}
		FANPRO_WARN("fan.panic", "fan=%d state=cleared temp=%.1f", i, hottest);
		d->panic_active[i] = false;
	}

	/* Deadband: avoid rewriting the same target every second. */
	if (isfinite(d->last_target[i]) &&
	    fabs(verdict.rpm - d->last_target[i]) < d->cfg.deadband_rpm)
		return true;

	rc = fanpro_smc_write_num(&d->smc, fan->key_tg, verdict.rpm);
	if (rc == FANPRO_SMC_SUCCESS || rc == FANPRO_SMC_KEY_SIZE_MISMATCH) {
		d->last_target[i] = verdict.rpm;
		FANPRO_DEBUG("fan.target", "fan=%d rpm=%.0f verdict=%s", i,
		             verdict.rpm, fanpro_safety_verdict_name(verdict.verdict));
	} else {
		FANPRO_ERROR("fan.target", "fan=%d rpm=%.0f result=0x%02x", i,
		             verdict.rpm, rc < 0 ? 0xffu : (unsigned)rc);
	}
	return true;
}

/*
 * A panic threshold configured for a class that no sensor reports is dead:
 * safety.c skips a class whose hottest reading is not finite, so the limit
 * silently guards nothing.  That is the right fail-open behaviour, but it
 * must not be invisible, which is how panic_ambient_c = 70 and the GPU class
 * both came to look live while protecting nothing on this hardware.
 *
 * Deliberately after the first SUCCESSFUL enumeration rather than at init:
 * d->sensors is empty until then, so every class would look unguarded.
 */
static void
warn_once_on_unguarded_classes(fanpro_daemon_t *d)
{
	int cls;

	if (d->warned_unguarded_classes)
		return;
	d->warned_unguarded_classes = true;

	for (cls = 0; cls < FANPRO_CLASS_COUNT; cls++) {
		double limit;

		pthread_mutex_lock(&d->cfg_lock);
		limit = d->cfg.safety.panic_c[cls];
		pthread_mutex_unlock(&d->cfg_lock);

		if (!isfinite(limit))
			continue;
		if (isfinite(fanpro_sensors_max_of_class(
		        &d->sensors, (fanpro_sensor_class_t)cls)))
			continue;

		FANPRO_WARN("safety.coverage",
		            "class=%s panic_c=%.1f sensors=0 "
		            "reason=threshold_guards_nothing",
		            fanpro_sensor_class_name((fanpro_sensor_class_t)cls),
		            limit);
	}
}

void
fanpro_control_loop_tick(fanpro_daemon_t *d, unsigned long long pass)
{
	int thermal_level;
	bool driving = false;
	bool active;
	int i;

	/* --- 0. what did the other threads ask for? --- */
	if (atomic_exchange(&d->release_requested, false)) {
		release_everything(d, "release_requested");
		/*
		 * Latching is a SEPARATE decision.  A heartbeat stall is a fault
		 * and must latch; a sleep is routine and must not, or the daemon
		 * would sit in monitor-only after every sleep/wake cycle and the
		 * user would never be told why.
		 */
		if (atomic_exchange(&d->latch_requested, false))
			atomic_store(&d->latched, true);

		/*
		 * End the tick here.  Falling through would re-acquire the fans
		 * further down the same pass, which for a sleep release means
		 * suspending with the fans pinned: the precise thing the
		 * release was asked to prevent.
		 */
		publish_snapshot(d, fanpro_thermal_pressure_read(), &d->power);
		atomic_fetch_add(&d->tick, 1);
		return;
	}
	if (atomic_exchange(&d->reload_requested, false)) {
		fanpro_request_t r;

		fanpro_request_init(&r, FANPRO_VERB_RELOAD);
		handle_command(d, &r);
	}
	if (atomic_exchange(&d->reprobe_requested, false)) {
		/* After wake, assume nothing about what the firmware did while
		 * we were asleep; re-read it. */
		FANPRO_INFO("daemon.wake", "action=reprobe");
		/*
		 * Same reasoning for the IOReport subscription: nothing in that
		 * SPI promises one survives a sleep cycle, and a stale one
		 * reports no channels rather than failing loudly.  Dropped here,
		 * on the control-loop thread, because this is the only thread
		 * that samples; power_notify.c only sets the flag.
		 */
		fanpro_power_invalidate();
		{
			fanpro_fan_set_t probe;

			/*
			 * Enumerate into a scratch set.  fanpro_fan_enumerate
			 * zeroes its target first and can then fail, which would
			 * leave us believing the machine has no fans while the
			 * hardware still had them pinned: the drive loop, the
			 * drift check and even release_all would all iterate
			 * zero fans and do nothing, silently, forever.
			 */
			if (fanpro_fan_enumerate(&d->smc, &probe) == 0 &&
			    probe.count > 0) {
				d->fans = probe;
				atomic_store(&d->pub_fan_count, probe.count);
			} else {
				FANPRO_ERROR("daemon.wake",
				             "reason=reenumerate_failed "
				             "action=release_using_previous_set");
				release_everything(d, "reenumerate_failed");
				d->sample_failures++;
			}
		}
		for (i = 0; i < FANPRO_MAX_FANS; i++)
			d->last_target[i] = NAN;
	}

	/* --- 1. commands --- */
	drain_commands(d);

	/* --- 2. sample --- */
	if (d->sample_sensors(&d->sensors, &d->smc, false) != 0 ||
	    d->sensors.count == 0)
		d->sample_failures++;
	else {
		d->sample_failures = 0;
		warn_once_on_unguarded_classes(d);
	}

	fanpro_fan_refresh(&d->smc, &d->fans);
	thermal_level = fanpro_thermal_pressure_read();

	/*
	 * --- 3. drift ---
	 *
	 * Two kinds.  The mode key changing is the obvious one.  The subtle one
	 * is another tool rewriting the TARGET while we still hold the fan:
	 * our deadband compares the new verdict against our own stale
	 * last_target, so we would keep skipping the write and the foreign
	 * speed would persist indefinitely with nothing logged.
	 *
	 * The read is free: fan_refresh above already fetched the target.
	 */
	{
		bool target_drift = false;
		int k;

		for (k = 0; k < d->fans.count; k++) {
			double reported = d->fans.fans[k].target;

			if (!d->unlock.manual[k] || !isfinite(d->last_target[k]))
				continue;
			if (!d->fans.fans[k].has_target || !isfinite(reported))
				continue;
			if (fabs(reported - d->last_target[k]) <=
			    TARGET_DRIFT_TOLERANCE_RPM)
				continue;

			FANPRO_WARN("unlock.drift",
			            "fan=%d key=target wrote=%.0f observed=%.0f", k,
			            d->last_target[k], reported);
			/*
			 * Re-assert rather than release: a foreign target on a fan
			 * whose mode we still own is most likely a stray write.
			 *
			 * But count it toward the same strike total as mode drift.
			 * Without that, a rival rewriting the target every second
			 * and fanpro correcting it every second would fight forever
			 * with no escalation and no way out.
			 */
			d->last_target[k] = NAN;
			target_drift = true;
		}

		if (target_drift) {
			d->drift_strikes++;
			d->drift_clean_ticks = 0;
			if (d->drift_strikes >= DRIFT_STRIKES_BEFORE_LATCH) {
				release_everything(d, "persistent_target_drift");
				atomic_store(&d->latched, true);
				FANPRO_ERROR("daemon.drift",
				             "strikes=%d action=latched "
				             "hint=another tool keeps rewriting the target",
				             d->drift_strikes);
			}
		}
	}

	if (fanpro_unlock_detect_drift(&d->unlock)) {
		/*
		 * Another SMC client moved what we wrote.  Only one tool can own
		 * the fans, so hand back rather than fight.  Critically, also
		 * skip driving for this tick: releasing and then re-acquiring
		 * further down the same pass would be exactly the tug of war
		 * this check exists to avoid.
		 */
		release_everything(d, "drift_detected");
		d->drift_strikes++;
		d->drift_clean_ticks = 0;

		if (d->drift_strikes >= DRIFT_STRIKES_BEFORE_LATCH) {
			/* Persistent, so this is not a transient: stand down
			 * until an operator decides who owns the fans. */
			atomic_store(&d->latched, true);
			FANPRO_ERROR("daemon.drift",
			             "strikes=%d action=latched "
			             "hint=another tool is controlling the fans",
			             d->drift_strikes);
		}

		publish_snapshot(d, thermal_level, &d->power);
		atomic_fetch_add(&d->tick, 1);
		return;
	}
	/* Decay slowly: a single quiet tick is not evidence the other tool has
	 * gone away, because our own release is what made it quiet. */
	if (d->drift_strikes > 0 &&
	    ++d->drift_clean_ticks >= DRIFT_CLEAN_TICKS_TO_DECAY) {
		d->drift_strikes = 0;
		d->drift_clean_ticks = 0;
	}

	/* --- 4. drive --- */
	pthread_mutex_lock(&d->cfg_lock);
	active = (d->cfg.mode == FANPRO_MODE_CURVE) && !atomic_load(&d->latched);
	pthread_mutex_unlock(&d->cfg_lock);

	if (active) {
		for (i = 0; i < d->fans.count; i++) {
			if (drive_fan(d, i, thermal_level))
				driving = true;
		}
	} else if (d->unlock.manual_count > 0) {
		release_everything(d, "not_active");
	}

	/* Power sampling blocks for its measurement interval, so it runs once
	 * every ten passes rather than every one. */
	if (d->sample_power && pass % 10 == 0)
		fanpro_power_sample(&d->power, 150);

	/* --- 5. record --- */
	if (d->record_history) {
		fanpro_history_row_t row;
		int k;

		memset(&row, 0, sizeof(row));
		row.unix_time = (int64_t)time(NULL);
		row.soc_c = fanpro_sensors_max_of_class(&d->sensors, FANPRO_CLASS_SOC);
		row.gpu_c = fanpro_sensors_max_of_class(&d->sensors, FANPRO_CLASS_GPU);
		row.nand_c = fanpro_sensors_max_of_class(&d->sensors, FANPRO_CLASS_NAND);
		for (k = 0; k < d->power.count; k++) {
			if (strcmp(d->power.channels[k].name, "CPU") == 0)
				row.cpu_w = d->power.channels[k].watts;
			else if (strcmp(d->power.channels[k].name, "GPU") == 0)
				row.gpu_w = d->power.channels[k].watts;
		}
		row.n_fans = d->fans.count;
		for (k = 0; k < d->fans.count && k < FANPRO_MAX_FANS; k++) {
			row.fan_rpm[k] = d->fans.fans[k].rpm;
			row.fan_target[k] = d->fans.fans[k].target;
			row.fan_mode[k] = d->fans.fans[k].mode;
		}
		row.thermal_level = thermal_level;
		row.driving = driving;
		fanpro_history_append(&row);

		pthread_mutex_lock(&d->cfg_lock);
		fanpro_alerts_evaluate(&d->cfg, &d->sensors, d->alert_state,
		                       fanpro_now_seconds());
		pthread_mutex_unlock(&d->cfg_lock);
	}

	/* --- 6. publish and prove liveness --- */
	publish_snapshot(d, thermal_level, &d->power);
	atomic_fetch_add(&d->tick, 1);

	if (pass % 60 == 0)
		FANPRO_INFO("daemon.tick",
		            "pass=%llu driving=%d held=%d thermal=%s failures=%d",
		            pass, (int)driving, d->unlock.manual_count,
		            fanpro_thermal_level_name(thermal_level),
		            d->sample_failures);
}

void
fanpro_control_loop_run(fanpro_daemon_t *d)
{
	unsigned long long pass = 0;

	while (!atomic_load(&d->stop)) {
		fanpro_control_loop_tick(d, pass);
		pass++;
		sleep_ms(d->cfg.tick_ms);
	}
}

void
fanpro_control_loop_shutdown(fanpro_daemon_t *d)
{
	release_everything(d, "shutdown");
	fanpro_smc_close(&d->smc);
}
