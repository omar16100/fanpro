/*
 * fanpro - power sampling state machine.
 *
 * The bug these exist for: fanpro_power_sample used to create a fresh IOReport
 * subscription on every call, and IOReportCreateSubscription leaks a measured
 * 235.7 KB per call on macOS 26.3 (Mac15,14).  At the daemon's ~13 s sampling
 * period that cost 1.6 GB/day, and fanprod reached 11.97 GB RSS over 8 days.
 *
 * The fix is a state machine, not a one-line change: subscribe once, resubscribe
 * only when sampling fails, and back off when resubscribing keeps failing (each
 * resubscribe costs another 235.7 KB, so a permanently broken SPI must not be
 * retried once per sample).  That state machine is what is tested here.
 *
 * These run through the injected-ops seam and never touch IOReport, so they are
 * hermetic and fast.  The real IOReport plumbing is covered by the live test in
 * test_sensors.c.
 */
#include "tinytest.h"

#include "fanpro/power.h"

#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

/* Resident size of this process, for the live diagnostic below. */
static size_t
tt_rss_kb(void)
{
	struct mach_task_basic_info info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
	              (task_info_t)&info, &count) != KERN_SUCCESS)
		return 0;
	return (size_t)(info.resident_size / 1024);
}

/* ---- fake IOReport ops -------------------------------------------------- */

static struct {
	int  subscribes;
	int  teardowns;
	int  samples;
	int  subscribe_rc;   /* what subscribe returns */
	int  sample_rc;      /* what sample_delta returns */
	int  fail_first_n;   /* sample_delta fails this many times, then succeeds */
} g_fake;

static int
fake_subscribe(void)
{
	g_fake.subscribes++;
	return g_fake.subscribe_rc;
}

static void
fake_teardown(void)
{
	g_fake.teardowns++;
}

static int
fake_sample_delta(unsigned interval_ms, fanpro_power_set_t *set)
{
	(void)interval_ms;
	g_fake.samples++;

	if (g_fake.fail_first_n > 0) {
		g_fake.fail_first_n--;
		return -5;
	}
	if (g_fake.sample_rc != 0)
		return g_fake.sample_rc;

	snprintf(set->channels[0].name, sizeof(set->channels[0].name), "CPU");
	set->channels[0].watts = 1.5;
	set->count = 1;
	set->total_watts = 1.5;
	set->available = true;
	return 0;
}

static const fanpro_power_ops_t FAKE_OPS = {
	fake_subscribe,
	fake_teardown,
	fake_sample_delta,
};

static void
fake_reset(void)
{
	memset(&g_fake, 0, sizeof(g_fake));
	fanpro_power_set_ops(&FAKE_OPS);
}

/* ---- the tests ---------------------------------------------------------- */

/*
 * The regression test for the leak itself.  Before the fix this would have
 * counted 20 subscriptions; it must now count exactly one.
 */
TT_TEST(power_subscribes_once_across_many_samples)
{
	fanpro_power_set_t set;
	int i;

	fake_reset();

	for (i = 0; i < 20; i++)
		TT_ASSERT(fanpro_power_sample(&set, 1) == 0);

	TT_EQ_INT(g_fake.subscribes, 1);
	TT_EQ_INT(g_fake.samples, 20);
	TT_EQ_INT(g_fake.teardowns, 0);
	TT_EQ_INT((int)fanpro_power_subscribe_count(), 1);
	TT_ASSERT(set.available);
	fanpro_power_set_ops(NULL);
}

/*
 * The case the retry exists for: a subscription that worked, then went stale
 * (the expected consequence of a sleep cycle).  One replacement, and the caller
 * still gets its reading rather than a gap.
 */
TT_TEST(power_resubscribes_once_on_stale_subscription)
{
	fanpro_power_set_t set;

	fake_reset();

	TT_ASSERT(fanpro_power_sample(&set, 1) == 0);
	TT_EQ_INT(g_fake.subscribes, 1);

	g_fake.fail_first_n = 1;   /* the held subscription has gone stale */

	TT_ASSERT(fanpro_power_sample(&set, 1) == 0);
	TT_EQ_INT(g_fake.subscribes, 2);
	TT_EQ_INT(g_fake.teardowns, 1);
	TT_ASSERT(set.available);
	fanpro_power_set_ops(NULL);
}

/*
 * A held subscription that fails is replaced exactly once, then the call gives
 * up.  Two sample attempts, one new subscription, no loop.
 */
TT_TEST(power_retry_is_bounded_not_looping)
{
	fanpro_power_set_t set;

	fake_reset();

	/* Establish a held (non-fresh) subscription first. */
	TT_ASSERT(fanpro_power_sample(&set, 1) == 0);
	TT_EQ_INT(g_fake.subscribes, 1);

	g_fake.sample_rc = -5;
	TT_ASSERT(fanpro_power_sample(&set, 1) != 0);

	TT_EQ_INT(g_fake.samples, 3);      /* 1 good, then 2 attempts */
	TT_EQ_INT(g_fake.subscribes, 2);   /* one replacement, not a loop */
	TT_EQ_INT(g_fake.teardowns, 2);
	TT_ASSERT(!set.available);
	fanpro_power_set_ops(NULL);
}

/*
 * A subscription created moments ago that already fails to sample means broken,
 * not stale, so it must not be rebuilt a second time in the same call.  Each
 * rebuild costs 235.7 KB.
 */
TT_TEST(power_fresh_subscription_failure_is_not_retried)
{
	fanpro_power_set_t set;

	fake_reset();
	g_fake.sample_rc = -5;

	TT_ASSERT(fanpro_power_sample(&set, 1) != 0);
	TT_EQ_INT(g_fake.subscribes, 1);
	TT_EQ_INT(g_fake.samples, 1);
	fanpro_power_set_ops(NULL);
}

/*
 * The leak guard, stated as the property that matters: under permanent failure
 * the number of subscriptions must grow far slower than the number of samples.
 * Without backoff this would be one subscription per sample, which is exactly
 * how the daemon reached 11.97 GB.
 */
TT_TEST(power_backs_off_after_repeated_failure)
{
	fanpro_power_set_t set;
	int i;

	fake_reset();
	g_fake.sample_rc = -5;

	for (i = 0; i < 100; i++)
		TT_ASSERT(fanpro_power_sample(&set, 1) != 0);

	/* Measured: 6 attempt cycles over 100 calls as the window widens
	 * 2,4,8,16,32,64. The bound is loose so the schedule can be retuned
	 * without rewriting the test, but 100 would mean no backoff at all. */
	TT_ASSERT(g_fake.subscribes <= 12);
	fanpro_power_set_ops(NULL);
}

/* Backoff must decay, not latch: once the SPI recovers, sampling resumes. */
TT_TEST(power_recovers_after_backoff_expires)
{
	fanpro_power_set_t set;
	int i;

	fake_reset();
	g_fake.sample_rc = -5;
	TT_ASSERT(fanpro_power_sample(&set, 1) != 0);

	g_fake.sample_rc = 0;

	/* Drain the skip window; the exact width is an implementation detail, so
	 * assert that it ends rather than how wide it is. */
	for (i = 0; i < 16; i++) {
		if (fanpro_power_sample(&set, 1) == 0)
			break;
	}

	TT_ASSERT(set.available);
	TT_ASSERT(i < 16);
	fanpro_power_set_ops(NULL);
}

/* A failed subscribe reports out and backs off without ever sampling. */
TT_TEST(power_subscribe_failure_does_not_sample)
{
	fanpro_power_set_t set;

	fake_reset();
	g_fake.subscribe_rc = -3;

	TT_ASSERT(fanpro_power_sample(&set, 1) != 0);
	TT_EQ_INT(g_fake.subscribes, 1);
	TT_EQ_INT(g_fake.samples, 0);
	TT_ASSERT(!set.available);
	fanpro_power_set_ops(NULL);
}

/* Wake invalidation drops the subscription so the next sample rebuilds it. */
TT_TEST(power_invalidate_forces_one_resubscribe)
{
	fanpro_power_set_t set;

	fake_reset();

	TT_ASSERT(fanpro_power_sample(&set, 1) == 0);
	TT_EQ_INT(g_fake.subscribes, 1);

	fanpro_power_invalidate();
	TT_EQ_INT(g_fake.teardowns, 1);

	TT_ASSERT(fanpro_power_sample(&set, 1) == 0);
	TT_EQ_INT(g_fake.subscribes, 2);
	fanpro_power_set_ops(NULL);
}

/*
 * Wake must defeat an active backoff.  Without this, a daemon that had been
 * failing before sleep would skip the next ~64 samples after wake, which is the
 * exact window the wake reprobe exists to cover.
 */
TT_TEST(power_invalidate_clears_active_backoff)
{
	fanpro_power_set_t set;

	fake_reset();
	g_fake.sample_rc = -5;

	TT_ASSERT(fanpro_power_sample(&set, 1) != 0);   /* arms the backoff */
	TT_ASSERT(fanpro_power_sample(&set, 1) != 0);   /* skipped, not attempted */

	g_fake.sample_rc = 0;
	fanpro_power_invalidate();

	/* The very next sample must work, not sit out the remaining skips. */
	TT_ASSERT(fanpro_power_sample(&set, 1) == 0);
	TT_ASSERT(set.available);
	fanpro_power_set_ops(NULL);
}

/* A NULL set is refused before anything is subscribed or sampled. */
TT_TEST(power_null_set_is_refused)
{
	fake_reset();

	TT_ASSERT(fanpro_power_sample(NULL, 1) == -1);
	TT_EQ_INT(g_fake.subscribes, 0);
	TT_EQ_INT(g_fake.samples, 0);
	fanpro_power_set_ops(NULL);
}

/* ---- live ---------------------------------------------------------------- */

/*
 * Real IOReport, FANPRO_LIVE=1 only.  This is a diagnostic rather than the
 * primary gate: RSS depends on the allocator, kernel caches and what else is
 * resident, so it is deliberately generous.  Its value is that it exercises the
 * real SPI end to end, which the injected-ops tests above cannot.
 *
 * The old code leaked ~235.7 KB per sample, so 40 samples moved RSS by ~9 MB.
 * The ceiling below is 4 MB: far under the broken behaviour, far over the
 * measured fixed behaviour (+32 KB total, all of it in the first few calls).
 */
TT_TEST(power_real_ioreport_rss_is_bounded)
{
	fanpro_power_set_t set;
	size_t before, after;
	int ok = 0;
	int i;

	fanpro_power_set_ops(NULL);
	fanpro_power_invalidate();

	/* Warm up: first call pays one-time dlopen and channel enumeration. */
	for (i = 0; i < 5; i++) {
		if (fanpro_power_sample(&set, 1) == 0)
			ok++;
	}
	if (ok == 0) {
		printf("    (skipped: IOReport unavailable)\n");
		return;
	}

	before = tt_rss_kb();
	for (i = 0; i < 40; i++)
		(void)fanpro_power_sample(&set, 1);
	after = tt_rss_kb();

	printf("    rss %zu -> %zu kb over 40 samples (%+zd)\n", before, after,
	       (ssize_t)after - (ssize_t)before);

	TT_ASSERT(after <= before + 4096);
	TT_EQ_INT((int)fanpro_power_subscribe_count(), 1);
}
