/*
 * Control loop tests, driven tick by tick against the fake SMC.
 *
 * These matter more than usual.  Measured on this hardware: the firmware does
 * NOT rescue a fan held in manual mode, so any loop bug that leaves a fan
 * pinned, or that drives it from stale data, is a real thermal risk rather
 * than a cosmetic one.
 */
#include "tinytest.h"

#include "../src/daemon/daemon.h"

#include "fanpro/smc_fake.h"

#include <math.h>
#include <string.h>

/*
 * Sensor injection.
 *
 * The real HID provider talks to IOKit, so without replacing it these tests
 * would read this machine's actual 77 sensors: they would pass for the wrong
 * reason and could never exercise "the sensors went away".
 */
static double g_soc_c;
static bool   g_sensors_present = true;

static int
stub_sensors(fanpro_sensor_set_t *set, fanpro_smc_t *smc, bool force_smc)
{
	(void)smc; (void)force_smc;
	memset(set, 0, sizeof(*set));
	set->hid_available = true;
	if (g_sensors_present)
		fanpro_sensors_add(set, "PMU tdie1", g_soc_c, FANPRO_SRC_HID);
	return 0;
}

static void
set_soc(double soc_c)
{
	g_soc_c = soc_c;
	g_sensors_present = true;
}

typedef struct {
	fanpro_fake_t   *fake;
	fanpro_daemon_t  d;
	unsigned long long pass;
} drig_t;

static void
drig_up(drig_t *r, const char *conf)
{
	char err[FANPRO_CONFIG_ERR_MAX];

	memset(r, 0, sizeof(*r));
	pthread_mutex_init(&r->d.cfg_lock, NULL);
	pthread_mutex_init(&r->d.cmd_lock, NULL);
	pthread_mutex_init(&r->d.snap_lock, NULL);
	/* Power sampling blocks for its interval; irrelevant here. */
	r->d.sample_power = false;
	r->d.sample_sensors = stub_sensors;
	g_sensors_present = true;
	g_soc_c = 50.0;
	snprintf(r->d.config_path, sizeof(r->d.config_path), "/nonexistent");
	/*
	 * Never the real marker.  A live fanprod on this machine would
	 * otherwise make every test latch itself into monitor-only, and a
	 * writable temp path would let one test's marker latch the next.
	 * An unwritable directory makes both the read and the write no-ops.
	 */
	r->d.state_path = "/nonexistent-dir/fanpro-test.state";

	r->fake = fanpro_fake_create(FANPRO_FAKE_DIRECT_OK);
	fanpro_fake_add_fans(r->fake, 2, 1000.0, 3625.0, true);

	TT_EQ_INT(fanpro_control_loop_init_backend(&r->d,
	                                           fanpro_fake_backend(r->fake)), 0);
	TT_EQ_INT(fanpro_config_parse(&r->d.cfg, conf, err, sizeof(err)), 0);
}

static void
drig_down(drig_t *r)
{
	fanpro_smc_close(&r->d.smc);
	fanpro_fake_destroy(r->fake);
}

static void
tick(drig_t *r, int n)
{
	int i;

	for (i = 0; i < n; i++)
		fanpro_control_loop_tick(&r->d, r->pass++);
}

static double
mode_of(drig_t *r, const char *key)
{
	double v = -1.0;

	fanpro_fake_peek(r->fake, key, &v);
	return v;
}

static const char *CONF_CURVE =
    "[general]\n"
    "mode = curve\n"
    "deadband_rpm = 10\n"
    "[fan.0]\n"
    "curve = c\n"
    "[curve.c]\n"
    "source = class:soc\n"
    "points = 40:1000, 60:2000, 80:3000\n"
    "hysteresis_c = 0\n"
    "slew_up_rpm = 5000\n"
    "slew_down_rpm = 5000\n";

TT_TEST(daemon_auto_mode_never_touches_a_fan)
{
	drig_t r;

	drig_up(&r, "[general]\nmode = auto\n");
	tick(&r, 3);

	/* Monitor-only must mean exactly that: no mode write at all. */
	TT_NEAR(mode_of(&r, "F0md"), 0.0, 0.0);
	TT_EQ_INT(r.d.unlock.manual_count, 0);
	TT_EQ_UINT(fanpro_fake_write_count(r.fake, "F0md"), 0);

	drig_down(&r);
}

TT_TEST(daemon_curve_mode_drives_the_fan)
{
	drig_t r;
	double target = -1.0;

	drig_up(&r, CONF_CURVE);

	/* 50 C sits halfway between the 40 C and 60 C points: 1500 rpm. */
	set_soc(50.0);
	fanpro_control_loop_tick(&r.d, r.pass++);

	TT_NEAR(mode_of(&r, "F0md"), 1.0, 0.0);
	TT_EQ_INT(r.d.unlock.manual_count, 1);
	fanpro_fake_peek(r.fake, "F0Tg", &target);
	TT_NEAR(target, 1500.0, 1.0);

	/* Fan 1 has no curve bound, so it must be left alone. */
	TT_NEAR(mode_of(&r, "F1md"), 0.0, 0.0);

	drig_down(&r);
}

TT_TEST(daemon_deadband_suppresses_redundant_writes)
{
	drig_t r;
	uint32_t before, after;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);

	before = fanpro_fake_write_count(r.fake, "F0Tg");
	/* A change well inside the deadband must not be rewritten every
	 * second: that would be one SMC write per tick forever. */
	set_soc(50.05);
	tick(&r, 3);
	after = fanpro_fake_write_count(r.fake, "F0Tg");
	TT_EQ_UINT(after, before);

	/* A change past it does get written. */
	set_soc(60.0);
	tick(&r, 1);
	TT_ASSERT(fanpro_fake_write_count(r.fake, "F0Tg") > before);

	drig_down(&r);
}

TT_TEST(daemon_panic_holds_at_max_and_does_not_flap)
{
	drig_t r;
	double target = -1.0;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	/* Past the SoC panic threshold. */
	set_soc(99.0);
	tick(&r, 1);
	fanpro_fake_peek(r.fake, "F0Tg", &target);
	TT_NEAR(target, 3625.0, 1.0);

	/*
	 * And it STAYS at max while the panic lasts.  Releasing immediately
	 * made the mode flap once per second, and in every gap the firmware
	 * was free to slow the fan back down: the exact opposite of what a
	 * panic is for.
	 */
	tick(&r, 5);
	TT_NEAR(mode_of(&r, "F0md"), 1.0, 0.0);
	fanpro_fake_peek(r.fake, "F0Tg", &target);
	TT_NEAR(target, 3625.0, 1.0);
	TT_TRUE(r.d.panic_active[0]);

	/* Just below the threshold is NOT enough to clear it, or the two
	 * states would alternate around the boundary. */
	set_soc(93.0);
	tick(&r, 2);
	TT_TRUE(r.d.panic_active[0]);

	/* Comfortably below, and normal control resumes. */
	set_soc(50.0);
	tick(&r, 2);
	TT_FALSE(r.d.panic_active[0]);
	fanpro_fake_peek(r.fake, "F0Tg", &target);
	TT_NEAR(target, 1500.0, 1.0);

	drig_down(&r);
}

TT_TEST(daemon_releases_when_sensors_disappear)
{
	drig_t r;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	/* Sensors vanish entirely. sample_failures climbs past the limit and
	 * the gate hands the fan back, because the firmware can still see
	 * temperatures we have lost sight of. */
	g_sensors_present = false;
	tick(&r, 5);
	TT_NEAR(mode_of(&r, "F0md"), 0.0, 0.0);
	TT_EQ_INT(r.d.unlock.manual_count, 0);

	drig_down(&r);
}

TT_TEST(daemon_latch_keeps_fans_on_auto)
{
	drig_t r;

	drig_up(&r, CONF_CURVE);
	atomic_store(&r.d.latched, true);
	set_soc(50.0);
	tick(&r, 3);

	/* A latched daemon must behave exactly like monitor-only, or a
	 * crash-looping daemon would re-pin the fans on every restart. */
	TT_NEAR(mode_of(&r, "F0md"), 0.0, 0.0);
	TT_EQ_INT(r.d.unlock.manual_count, 0);

	/* Clearing the latch lets it drive again. */
	atomic_store(&r.d.latched, false);
	set_soc(50.0);
	tick(&r, 1);
	TT_NEAR(mode_of(&r, "F0md"), 1.0, 0.0);

	drig_down(&r);
}

TT_TEST(daemon_heartbeat_release_latches)
{
	drig_t r;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	/* Exactly what the heartbeat watchdog raises when the loop stalls:
	 * release AND latch, because a stall is a fault. */
	atomic_store(&r.d.latch_requested, true);
	atomic_store(&r.d.release_requested, true);
	tick(&r, 1);

	TT_NEAR(mode_of(&r, "F0md"), 0.0, 0.0);
	TT_TRUE(atomic_load(&r.d.latched));

	/* And it stays released: silently re-taking the fans next tick would
	 * defeat the entire point of the watchdog. */
	tick(&r, 2);
	TT_NEAR(mode_of(&r, "F0md"), 0.0, 0.0);

	drig_down(&r);
}

TT_TEST(daemon_sleep_release_does_not_latch)
{
	drig_t r;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	/*
	 * What power_notify raises on sleep: release WITHOUT latching.  Sleep
	 * is routine, and latching on it left the daemon silently stuck in
	 * monitor-only after every single sleep/wake cycle.
	 */
	atomic_store(&r.d.release_requested, true);
	tick(&r, 1);
	TT_NEAR(mode_of(&r, "F0md"), 0.0, 0.0);
	TT_FALSE(atomic_load(&r.d.latched));

	/* Control resumes by itself on the next pass, as it should. */
	tick(&r, 1);
	TT_NEAR(mode_of(&r, "F0md"), 1.0, 0.0);

	drig_down(&r);
}

TT_TEST(daemon_unbinding_a_curve_does_not_pin_the_fan)
{
	drig_t r;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	/*
	 * Remove the binding, as a config reload would.  The curve evaluator
	 * primes its own state, so treating "primed" as "has a manual
	 * override" pinned the fan at whatever RPM the curve last produced,
	 * indefinitely and invisibly.
	 */
	pthread_mutex_lock(&r.d.cfg_lock);
	r.d.cfg.fan_curve[0][0] = '\0';
	pthread_mutex_unlock(&r.d.cfg_lock);

	tick(&r, 2);
	TT_NEAR(mode_of(&r, "F0md"), 0.0, 0.0);
	TT_EQ_INT(r.d.unlock.manual_count, 0);

	drig_down(&r);
}

TT_TEST(daemon_apply_curve_rejects_an_unknown_name)
{
	drig_t r;
	fanpro_request_t req;

	drig_up(&r, CONF_CURVE);

	fanpro_request_init(&req, FANPRO_VERB_APPLY_CURVE);
	req.fan_index = 1;
	snprintf(req.name, sizeof(req.name), "does-not-exist");
	fanpro_daemon_push_cmd(&r.d, &req);
	set_soc(50.0);
	tick(&r, 1);

	/* The config parser rejects a fan bound to a missing curve; the IPC
	 * path must not reintroduce that hole. */
	pthread_mutex_lock(&r.d.cfg_lock);
	TT_EQ_STR(r.d.cfg.fan_curve[1], "");
	pthread_mutex_unlock(&r.d.cfg_lock);
	TT_NEAR(mode_of(&r, "F1md"), 0.0, 0.0);

	drig_down(&r);
}

TT_TEST(daemon_drift_from_another_client_releases)
{
	drig_t r;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	/*
	 * Another fan-control tool takes the fan back.  fanpro must stand down
	 * rather than fight over the mode key,
	 * and crucially must NOT re-acquire later in the same tick: that
	 * tug of war is exactly what drift detection exists to prevent.
	 */
	fanpro_fake_poke(r.fake, "F0md", 0.0);
	set_soc(50.0);
	tick(&r, 1);

	TT_EQ_INT(r.d.unlock.manual_count, 0);
	TT_NEAR(mode_of(&r, "F0md"), 0.0, 0.0);

	/* Persistent drift latches, so fanpro stops competing entirely until
	 * an operator decides who owns the fans. */
	fanpro_fake_poke(r.fake, "F0md", 1.0);
	tick(&r, 1);
	fanpro_fake_poke(r.fake, "F0md", 1.0);
	tick(&r, 1);
	TT_TRUE(atomic_load(&r.d.latched));

	drig_down(&r);
}

TT_TEST(daemon_set_fan_command_overrides_the_curve)
{
	drig_t r;
	fanpro_request_t req;
	double target = -1.0;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);

	fanpro_request_init(&req, FANPRO_VERB_SET_FAN);
	req.fan_index = 0;
	req.rpm = 2400.0;
	TT_TRUE(fanpro_daemon_push_cmd(&r.d, &req));

	set_soc(50.0);
	tick(&r, 1);
	fanpro_fake_peek(r.fake, "F0Tg", &target);
	TT_NEAR(target, 2400.0, 1.0);

	drig_down(&r);
}

TT_TEST(daemon_set_fan_auto_does_not_get_reacquired)
{
	drig_t r;
	fanpro_request_t req;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);

	fanpro_request_init(&req, FANPRO_VERB_SET_FAN);
	req.fan_index = 0;
	req.rpm = 2400.0;
	fanpro_daemon_push_cmd(&r.d, &req);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	/* Release it.  The bug this pins: leaving the override primed meant
	 * the very next tick re-acquired the fan and re-applied 2400, so
	 * "released to firmware control" was a lie. */
	fanpro_request_init(&req, FANPRO_VERB_SET_FAN);
	req.fan_index = 0;
	req.rpm = NAN;
	fanpro_daemon_push_cmd(&r.d, &req);

	set_soc(50.0);
	tick(&r, 3);

	TT_NEAR(mode_of(&r, "F0md"), 0.0, 0.0);
	TT_EQ_INT(r.d.unlock.manual_count, 0);

	drig_down(&r);
}

TT_TEST(daemon_switching_to_auto_releases_everything)
{
	drig_t r;
	fanpro_request_t req;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	fanpro_request_init(&req, FANPRO_VERB_SET_MODE);
	req.ival = 0; /* auto */
	fanpro_daemon_push_cmd(&r.d, &req);

	set_soc(50.0);
	tick(&r, 2);
	TT_NEAR(mode_of(&r, "F0md"), 0.0, 0.0);
	TT_EQ_INT(r.d.unlock.manual_count, 0);

	drig_down(&r);
}

TT_TEST(daemon_command_queue_rejects_overflow)
{
	drig_t r;
	fanpro_request_t req;
	int i;

	drig_up(&r, CONF_CURVE);
	fanpro_request_init(&req, FANPRO_VERB_PING);

	for (i = 0; i < FANPRO_CMD_QUEUE_LEN; i++)
		TT_TRUE(fanpro_daemon_push_cmd(&r.d, &req));

	/* Full means full: silently dropping a command would make the CLI
	 * report success for something that never happened. */
	TT_FALSE(fanpro_daemon_push_cmd(&r.d, &req));

	tick(&r, 1);
	TT_TRUE(fanpro_daemon_push_cmd(&r.d, &req));

	drig_down(&r);
}

TT_TEST(daemon_snapshot_reflects_state)
{
	drig_t r;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);

	pthread_mutex_lock(&r.d.snap_lock);
	TT_EQ_INT(r.d.snapshot.mode, FANPRO_MODE_CURVE);
	TT_EQ_INT(r.d.snapshot.n_fans, 2);
	TT_EQ_INT(r.d.snapshot.fans[0].held, 1);
	TT_EQ_INT(r.d.snapshot.fans[1].held, 0);
	TT_EQ_STR(r.d.snapshot.fans[0].curve, "c");
	pthread_mutex_unlock(&r.d.snap_lock);

	drig_down(&r);
}

TT_TEST(daemon_tick_counter_advances_for_the_watchdog)
{
	drig_t r;
	unsigned long long before;

	drig_up(&r, "[general]\nmode = auto\n");
	before = atomic_load(&r.d.tick);
	tick(&r, 3);

	/* The heartbeat watchdog decides the loop is wedged by watching this.
	 * If a tick can complete without bumping it, the watchdog fires
	 * spuriously and releases fans for no reason. */
	TT_EQ_UINT(atomic_load(&r.d.tick), before + 3);

	drig_down(&r);
}

TT_TEST(daemon_repeated_drift_eventually_latches)
{
	drig_t r;
	int i;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	/*
	 * A rival tool takes the fan back over and over.  This was unreachable
	 * before: a drift tick releases the fan, so the next tick sees no
	 * drift by construction, and resetting the strike counter there capped
	 * it at 1 forever.  Hardware testing found it; this pins it.
	 */
	for (i = 0; i < 6 && !atomic_load(&r.d.latched); i++) {
		fanpro_fake_poke(r.fake, "F0md", 0.0); /* rival steals it */
		tick(&r, 1);                            /* drift, release      */
		tick(&r, 1);                            /* re-acquire          */
	}

	TT_TRUE(atomic_load(&r.d.latched));
	TT_EQ_INT(r.d.unlock.manual_count, 0);

	drig_down(&r);
}

TT_TEST(daemon_isolated_drift_does_not_latch)
{
	drig_t r;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);

	/* One transient glitch must not disable fanpro: strikes decay over a
	 * run of clean ticks. */
	fanpro_fake_poke(r.fake, "F0md", 0.0);
	tick(&r, 1);
	TT_FALSE(atomic_load(&r.d.latched));

	tick(&r, 15); /* well past the decay window */
	TT_FALSE(atomic_load(&r.d.latched));
	TT_EQ_INT(r.d.drift_strikes, 0);

	drig_down(&r);
}

TT_TEST(daemon_foreign_target_write_is_noticed)
{
	drig_t r;
	double target = -1.0;
	uint32_t writes_before;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	fanpro_fake_peek(r.fake, "F0Tg", &target);
	TT_NEAR(target, 1500.0, 1.0);

	/*
	 * A rival tool rewrites the TARGET while we still own the mode key.
	 * Drift detection only watched mode keys, so nothing noticed, and the
	 * deadband then compared the next verdict against our own stale
	 * last_target and skipped the write: the foreign speed would persist
	 * indefinitely with nothing logged.
	 */
	fanpro_fake_poke(r.fake, "F0Tg", 3000.0);
	writes_before = fanpro_fake_write_count(r.fake, "F0Tg");

	set_soc(50.0);
	tick(&r, 1);

	/* The corrective write must actually happen, deadband notwithstanding. */
	TT_ASSERT(fanpro_fake_write_count(r.fake, "F0Tg") > writes_before);
	fanpro_fake_peek(r.fake, "F0Tg", &target);
	TT_NEAR(target, 1500.0, 1.0);

	drig_down(&r);
}

TT_TEST(daemon_release_is_observable_to_other_threads)
{
	drig_t r;
	unsigned long long before;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	/*
	 * The power thread waits on this before acknowledging sleep.  Without
	 * it, the machine could suspend up to a tick before the fans were
	 * actually handed back.
	 */
	before = atomic_load(&r.d.release_generation);
	atomic_store(&r.d.release_requested, true);
	tick(&r, 1);

	TT_ASSERT(atomic_load(&r.d.release_generation) != before);
	TT_EQ_INT(r.d.unlock.manual_count, 0);

	drig_down(&r);
}

TT_TEST(daemon_persistent_target_theft_latches)
{
	drig_t r;
	int i;

	drig_up(&r, CONF_CURVE);
	set_soc(50.0);
	tick(&r, 1);
	TT_EQ_INT(r.d.unlock.manual_count, 1);

	/*
	 * A rival rewrites the TARGET every tick while we still own the mode
	 * key.  Correcting it forever with no escalation would be an endless
	 * tug of war, so target drift counts toward the same strike total as
	 * mode drift and eventually makes fanpro stand down.
	 */
	for (i = 0; i < 8 && !atomic_load(&r.d.latched); i++) {
		fanpro_fake_poke(r.fake, "F0Tg", 3000.0);
		set_soc(50.0);
		tick(&r, 1);
	}

	TT_TRUE(atomic_load(&r.d.latched));
	TT_EQ_INT(r.d.unlock.manual_count, 0);

	drig_down(&r);
}

/*
 * The mode/set race.  `fanpro mode curve && fanpro set all 3625` used to be
 * unwinnable: the IPC thread acked the mode change before the control loop
 * applied it, so the set gate read the still-AUTO cfg.mode and refused the
 * speed.  Observed live on 2026-09-18, where the refusal dropped the pin and
 * left the default curve to pull both fans from 2500 to 1180 rpm, i.e.
 * cooling LESS than not intervening at all.
 *
 * Zero ticks between push and query is the whole point: a test that ticks
 * first would pass against the broken code.
 */
TT_TEST(effective_mode_sees_a_queued_switch_before_any_tick)
{
	drig_t r;
	fanpro_request_t req;

	drig_up(&r, "[general]\nmode = auto\n");

	TT_TRUE(fanpro_daemon_effective_mode(&r.d) == FANPRO_MODE_AUTO);

	fanpro_request_init(&req, FANPRO_VERB_SET_MODE);
	req.ival = 1; /* curve */
	TT_TRUE(fanpro_daemon_push_cmd(&r.d, &req));

	/* Pending, with no tick having run. */
	TT_TRUE(fanpro_daemon_effective_mode(&r.d) == FANPRO_MODE_CURVE);
	/* Still only pending: the loop remains the sole writer of cfg. */
	TT_TRUE(r.d.cfg.mode == FANPRO_MODE_AUTO);

	drig_down(&r);
}

TT_TEST(effective_mode_uses_the_latest_queued_switch)
{
	drig_t r;
	fanpro_request_t req;

	drig_up(&r, "[general]\nmode = auto\n");

	fanpro_request_init(&req, FANPRO_VERB_SET_MODE);
	req.ival = 1; /* curve */
	TT_TRUE(fanpro_daemon_push_cmd(&r.d, &req));

	fanpro_request_init(&req, FANPRO_VERB_SET_MODE);
	req.ival = 0; /* back to auto before either is applied */
	TT_TRUE(fanpro_daemon_push_cmd(&r.d, &req));

	/* Slot order is arrival order, so the last request wins. */
	TT_TRUE(fanpro_daemon_effective_mode(&r.d) == FANPRO_MODE_AUTO);

	drig_down(&r);
}

TT_TEST(effective_mode_matches_applied_mode_once_the_queue_drains)
{
	drig_t r;
	fanpro_request_t req;

	drig_up(&r, "[general]\nmode = auto\n");

	fanpro_request_init(&req, FANPRO_VERB_SET_MODE);
	req.ival = 1;
	TT_TRUE(fanpro_daemon_push_cmd(&r.d, &req));

	set_soc(50.0);
	tick(&r, 1);

	/* Queue empty now, so this reads through to the applied value. */
	TT_TRUE(r.d.cfg.mode == FANPRO_MODE_CURVE);
	TT_TRUE(fanpro_daemon_effective_mode(&r.d) == FANPRO_MODE_CURVE);

	drig_down(&r);
}

/*
 * panic_ambient_c = 70 shipped in the default config while this hardware
 * reports no ambient sensor at all, so the threshold guarded nothing and
 * said nothing.  Warn once, after the first successful enumeration.
 */
TT_TEST(a_panic_threshold_guarding_no_sensor_is_reported_once)
{
	drig_t r;

	/* stub_sensors supplies a soc reading only; ambient stays empty. */
	drig_up(&r, "[general]\nmode = auto\npanic_ambient_c = 70\n");

	TT_TRUE(!r.d.warned_unguarded_classes);
	set_soc(50.0);
	tick(&r, 1);
	TT_TRUE(r.d.warned_unguarded_classes);

	/* Second pass must not re-warn. */
	set_soc(50.0);
	tick(&r, 1);
	TT_TRUE(r.d.warned_unguarded_classes);

	drig_down(&r);
}
