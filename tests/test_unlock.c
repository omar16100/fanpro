/*
 * Unlock state machine tests.
 *
 * The Ftst path cannot be exercised on the development machine (this Mac
 * Studio has no Ftst key at all), so these tests against the fake firmware
 * are the only coverage it will ever get here.  They are written to be
 * demanding for that reason.
 */
#include "tinytest.h"

#include "fanpro/smc_fake.h"
#include "fanpro/unlock.h"

#include <string.h>

/* Clock backed by the fake's virtual time, so waits cost nothing. */
static uint64_t
fake_now(void *ctx)
{
	return fanpro_fake_now_ms((const fanpro_fake_t *)ctx);
}

static void
fake_sleep(void *ctx, unsigned ms)
{
	fanpro_fake_advance_ms((fanpro_fake_t *)ctx, ms);
}

typedef struct {
	fanpro_fake_t   *fake;
	fanpro_smc_t     smc;
	fanpro_fan_set_t fans;
	fanpro_unlock_t  unlock;
	fanpro_clock_t   clock;
} rig_t;

static void
rig_up(rig_t *r, fanpro_fake_personality_t p, int fans, bool lowercase)
{
	memset(r, 0, sizeof(*r));
	r->fake = fanpro_fake_create(p);
	fanpro_fake_add_fans(r->fake, fans, 1000.0, 3625.0, lowercase);
	fanpro_smc_open(&r->smc, fanpro_fake_backend(r->fake));
	fanpro_fan_enumerate(&r->smc, &r->fans);

	r->clock.now_ms = fake_now;
	r->clock.sleep_ms = fake_sleep;
	r->clock.ctx = r->fake;
	fanpro_unlock_init(&r->unlock, &r->smc, &r->fans, &r->clock);
}

static void
rig_down(rig_t *r)
{
	fanpro_smc_close(&r->smc);
	fanpro_fake_destroy(r->fake);
}

static double
mode_of(rig_t *r, const char *key)
{
	double v = -1.0;

	fanpro_fake_peek(r->fake, key, &v);
	return v;
}

TT_TEST(unlock_direct_path_never_touches_ftst)
{
	rig_t r;

	/* This is the shape of the real Mac Studio: lowercase mode key, no
	 * Ftst, fans idle in auto.  The diagnostic key must not be involved. */
	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 2, true);

	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 0), 0);
	TT_EQ_INT(r.unlock.path, FANPRO_UNLOCK_PATH_DIRECT);
	TT_NEAR(mode_of(&r, "F0md"), 1.0, 0.0);
	TT_FALSE(r.unlock.ftst_asserted);
	TT_EQ_INT(r.unlock.manual_count, 1);

	rig_down(&r);
}

TT_TEST(unlock_falls_back_to_ftst_only_after_a_real_rejection)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_NEEDS_FTST, 2, false);
	fanpro_fake_set_yield_delay_ms(r.fake, 3500);

	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 0), 0);
	TT_EQ_INT(r.unlock.path, FANPRO_UNLOCK_PATH_FTST);
	TT_TRUE(r.unlock.ftst_asserted);
	TT_NEAR(mode_of(&r, "F0Md"), 1.0, 0.0);

	/* Exactly two mode writes: the rejected direct attempt and the one that
	 * landed after the yield.  ">= 1" would also pass a build that never
	 * retried, which is precisely the failure worth catching. */
	TT_EQ_UINT(fanpro_fake_write_count(r.fake, "F0Md"), 2);
	TT_EQ_UINT(fanpro_fake_write_count(r.fake, "Ftst"), 1);

	rig_down(&r);
}

TT_TEST(unlock_reports_failure_when_rejected_and_no_ftst_exists)
{
	rig_t r;

	/* Firmware refuses the mode write and offers no unlock key.  fanpro
	 * must conclude manual control is unavailable rather than retry. */
	rig_up(&r, FANPRO_FAKE_NEEDS_FTST, 1, false);
	fanpro_fake_remove_key(r.fake, "Ftst");
	fanpro_fan_enumerate(&r.smc, &r.fans);

	TT_ASSERT(fanpro_unlock_acquire(&r.unlock, 0) < 0);
	TT_EQ_INT(r.unlock.path, FANPRO_UNLOCK_PATH_NONE);
	TT_EQ_INT(r.unlock.manual_count, 0);
	TT_NEAR(mode_of(&r, "F0Md"), 3.0, 0.0);

	rig_down(&r);
}

TT_TEST(unlock_yield_timeout_leaves_nothing_asserted)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_NEEDS_FTST, 1, false);
	/* Firmware never yields within our patience. */
	fanpro_fake_set_yield_delay_ms(r.fake, 60000);
	r.unlock.yield_timeout_ms = 2000;

	TT_ASSERT(fanpro_unlock_acquire(&r.unlock, 0) < 0);
	TT_EQ_INT(r.unlock.manual_count, 0);

	/* Critically: Ftst must not be left set.  A stuck Ftst suppresses
	 * thermalmonitord's reclaim, which is worse than having no control. */
	TT_FALSE(r.unlock.ftst_asserted);
	TT_NEAR(mode_of(&r, "Ftst"), 0.0, 0.0);

	rig_down(&r);
}

TT_TEST(unlock_ftst_survives_releasing_one_of_two_fans)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_NEEDS_FTST, 2, false);

	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 0), 0);
	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 1), 0);
	TT_EQ_INT(r.unlock.manual_count, 2);

	/* Releasing fan 0 must NOT clear Ftst.  Clearing it here would let
	 * thermalmonitord reclaim and silently end fan 1's manual control:
	 * the exact bug the refcount exists to prevent. */
	TT_EQ_INT(fanpro_unlock_release(&r.unlock, 0), 0);
	TT_TRUE(r.unlock.ftst_asserted);
	TT_NEAR(mode_of(&r, "Ftst"), 1.0, 0.0);
	TT_NEAR(mode_of(&r, "F1Md"), 1.0, 0.0);
	TT_EQ_INT(r.unlock.manual_count, 1);

	/* The last fan takes Ftst with it. */
	TT_EQ_INT(fanpro_unlock_release(&r.unlock, 1), 0);
	TT_FALSE(r.unlock.ftst_asserted);
	TT_NEAR(mode_of(&r, "Ftst"), 0.0, 0.0);
	TT_EQ_INT(r.unlock.manual_count, 0);

	rig_down(&r);
}

TT_TEST(unlock_release_all_is_unconditional)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 2, false);

	/* Simulate a previous process having pinned a fan: our own bookkeeping
	 * says we hold nothing, but the hardware disagrees. */
	fanpro_fake_poke(r.fake, "F1Md", 1.0);
	TT_EQ_INT(r.unlock.manual_count, 0);

	TT_EQ_INT(fanpro_unlock_release_all(&r.unlock), 0);
	TT_NEAR(mode_of(&r, "F0Md"), 0.0, 0.0);
	TT_NEAR(mode_of(&r, "F1Md"), 0.0, 0.0);

	rig_down(&r);
}

TT_TEST(unlock_recover_detects_and_clears_an_unclean_exit)
{
	rig_t r;
	int stale;

	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 2, false);

	/* What a SIGKILLed daemon leaves behind. */
	fanpro_fake_poke(r.fake, "F0Md", 1.0);
	fanpro_fake_poke(r.fake, "F1Md", 1.0);

	stale = fanpro_unlock_recover(&r.unlock);
	TT_EQ_INT(stale, 2);
	TT_NEAR(mode_of(&r, "F0Md"), 0.0, 0.0);
	TT_NEAR(mode_of(&r, "F1Md"), 0.0, 0.0);

	/* A clean machine must report nothing to recover. */
	TT_EQ_INT(fanpro_unlock_recover(&r.unlock), 0);

	rig_down(&r);
}

TT_TEST(unlock_recover_notices_an_orphaned_ftst)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_NEEDS_FTST, 1, false);

	/* Ftst set with no fan in manual mode: a crashed process left the
	 * reclaim suppressed.  That is exactly the dangerous residue. */
	fanpro_fake_poke(r.fake, "Ftst", 1.0);

	TT_ASSERT(fanpro_unlock_recover(&r.unlock) > 0);
	TT_NEAR(mode_of(&r, "Ftst"), 0.0, 0.0);

	rig_down(&r);
}

TT_TEST(unlock_detects_an_external_client_stealing_control)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 2, false);
	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 0), 0);
	TT_FALSE(fanpro_unlock_detect_drift(&r.unlock));

	/* Stats or Macs Fan Control flips the mode underneath us. */
	fanpro_fake_poke(r.fake, "F0Md", 0.0);
	TT_TRUE(fanpro_unlock_detect_drift(&r.unlock));

	rig_down(&r);
}

TT_TEST(unlock_detects_another_client_pinning_a_fan_we_do_not_hold)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 2, false);

	fanpro_fake_poke(r.fake, "F1Md", 1.0);
	TT_TRUE(fanpro_unlock_detect_drift(&r.unlock));

	rig_down(&r);
}

TT_TEST(unlock_acquire_is_idempotent)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 1, false);

	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 0), 0);
	TT_EQ_INT(fanpro_unlock_acquire(&r.unlock, 0), 0);
	/* A double acquire must not inflate the refcount, or the last release
	 * would never clear Ftst. */
	TT_EQ_INT(r.unlock.manual_count, 1);

	rig_down(&r);
}

TT_TEST(unlock_rejects_an_out_of_range_fan)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_DIRECT_OK, 2, false);

	TT_ASSERT(fanpro_unlock_acquire(&r.unlock, 5) < 0);
	TT_ASSERT(fanpro_unlock_acquire(&r.unlock, -1) < 0);
	TT_EQ_INT(r.unlock.manual_count, 0);

	rig_down(&r);
}

TT_TEST(unlock_unprivileged_fails_without_pinning_anything)
{
	rig_t r;

	rig_up(&r, FANPRO_FAKE_NOT_PRIVILEGED, 1, false);

	TT_ASSERT(fanpro_unlock_acquire(&r.unlock, 0) < 0);
	TT_EQ_INT(r.unlock.manual_count, 0);
	TT_FALSE(r.unlock.ftst_asserted);

	rig_down(&r);
}
