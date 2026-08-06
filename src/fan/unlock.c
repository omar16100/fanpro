/*
 * fanpro - manual fan control acquisition and release.
 *
 * See unlock.h for the shape of the problem.  The invariants this file must
 * never break:
 *
 *   - Ftst is only ever asserted after a direct write has actually failed.
 *   - Ftst is only ever cleared when no fan is still manual.
 *   - Release is retried, because it is the operation whose failure leaves
 *     hardware pinned.
 *   - Every state change is logged with the firmware's verdict.
 */
#include "fanpro/unlock.h"

#include "fanpro/log.h"
#include "fanpro/smc_codec.h"

#include <string.h>
#include <time.h>

#define DEFAULT_YIELD_POLL_MS    100
#define DEFAULT_YIELD_TIMEOUT_MS 10000
#define DEFAULT_RELEASE_RETRIES  3

/* ---- system clock ------------------------------------------------------- */

static uint64_t
sys_now_ms(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void
sys_sleep_ms(void *ctx, unsigned ms)
{
	struct timespec ts;

	(void)ctx;
	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	nanosleep(&ts, NULL);
}

static const fanpro_clock_t SYSTEM_CLOCK = {
	.now_ms = sys_now_ms,
	.sleep_ms = sys_sleep_ms,
	.ctx = NULL,
};

const fanpro_clock_t *
fanpro_clock_system(void)
{
	return &SYSTEM_CLOCK;
}

const char *
fanpro_unlock_path_name(fanpro_unlock_path_t p)
{
	switch (p) {
	case FANPRO_UNLOCK_PATH_DIRECT: return "direct";
	case FANPRO_UNLOCK_PATH_FTST:   return "ftst";
	case FANPRO_UNLOCK_PATH_NONE:   return "none";
	default:                        return "unknown";
	}
}

/* ---- helpers ------------------------------------------------------------ */

static bool
read_num(fanpro_smc_t *smc, uint32_t key, double *out)
{
	fanpro_smc_value_t v;

	if (fanpro_smc_read(smc, key, &v) != FANPRO_SMC_SUCCESS || !v.numeric)
		return false;
	*out = v.num;
	return true;
}

/* Did this write land?  0x87 errors but is known to apply the value. */
static bool
write_ok(int rc)
{
	return rc == FANPRO_SMC_SUCCESS || rc == FANPRO_SMC_KEY_SIZE_MISMATCH;
}

static int
fan_mode(fanpro_unlock_t *u, int idx)
{
	double v;

	if (!read_num(u->smc, u->fans->fans[idx].key_md, &v))
		return FANPRO_FAN_MODE_UNKNOWN;
	return (int)v;
}

static bool
valid_fan(const fanpro_unlock_t *u, int idx)
{
	return u != NULL && u->fans != NULL && idx >= 0 &&
	       idx < u->fans->count && u->fans->fans[idx].mode_key_known;
}

void
fanpro_unlock_init(fanpro_unlock_t *u, fanpro_smc_t *smc,
                   fanpro_fan_set_t *fans, const fanpro_clock_t *clock)
{
	if (u == NULL)
		return;

	memset(u, 0, sizeof(*u));
	u->smc = smc;
	u->fans = fans;
	u->clock = (clock != NULL) ? *clock : SYSTEM_CLOCK;
	u->yield_poll_ms = DEFAULT_YIELD_POLL_MS;
	u->yield_timeout_ms = DEFAULT_YIELD_TIMEOUT_MS;
	u->release_retries = DEFAULT_RELEASE_RETRIES;
	u->path = FANPRO_UNLOCK_PATH_UNKNOWN;
}

/*
 * Wait for thermalmonitord to drop a fan out of system mode after Ftst has
 * been asserted.  Returns true once the mode is no longer 3.
 */
static bool
wait_for_yield(fanpro_unlock_t *u, int idx)
{
	uint64_t start = u->clock.now_ms(u->clock.ctx);

	for (;;) {
		int mode = fan_mode(u, idx);
		uint64_t elapsed;

		if (mode != FANPRO_FAN_MODE_SYSTEM &&
		    mode != FANPRO_FAN_MODE_UNKNOWN) {
			FANPRO_DEBUG("unlock.yield", "fan=%d waited_ms=%llu", idx,
			             (unsigned long long)(u->clock.now_ms(u->clock.ctx) - start));
			return true;
		}

		elapsed = u->clock.now_ms(u->clock.ctx) - start;
		if (elapsed >= u->yield_timeout_ms) {
			FANPRO_WARN("unlock.yield.timeout", "fan=%d waited_ms=%llu",
			            idx, (unsigned long long)elapsed);
			return false;
		}
		u->clock.sleep_ms(u->clock.ctx, u->yield_poll_ms);
	}
}

/*
 * Write with retries.  Used on every path whose failure would leave hardware
 * in a bad state: release, and the cleanup after a failed acquire.
 */
static bool
write_retry(fanpro_unlock_t *u, uint32_t key, double value)
{
	int attempt;

	for (attempt = 0; attempt < u->release_retries; attempt++) {
		int rc = fanpro_smc_write_num(u->smc, key, value);

		if (write_ok(rc))
			return true;
		u->clock.sleep_ms(u->clock.ctx, 100);
	}
	return false;
}

int
fanpro_unlock_acquire(fanpro_unlock_t *u, int fan_index)
{
	uint32_t mode_key;
	int rc;

	if (!valid_fan(u, fan_index)) {
		FANPRO_ERROR("unlock.acquire", "fan=%d reason=no_mode_key", fan_index);
		return -1;
	}
	if (u->manual[fan_index])
		return 0; /* already held */
	if (u->path == FANPRO_UNLOCK_PATH_NONE)
		return -2;

	mode_key = u->fans->fans[fan_index].key_md;

	/* Phase 1: the cheap path.  On firmware that does not gate mode writes
	 * this is the whole story, and it never touches the diagnostic key. */
	rc = fanpro_smc_write_num(u->smc, mode_key, FANPRO_FAN_MODE_MANUAL);
	if (write_ok(rc) && fan_mode(u, fan_index) == FANPRO_FAN_MODE_MANUAL) {
		u->path = FANPRO_UNLOCK_PATH_DIRECT;
		u->manual[fan_index] = true;
		u->manual_count++;
		FANPRO_INFO("unlock.acquire", "fan=%d path=direct held=%d",
		            fan_index, u->manual_count);
		return 0;
	}
	if (rc < 0) {
		FANPRO_ERROR("unlock.acquire", "fan=%d reason=transport rc=%d",
		             fan_index, rc);
		return -3;
	}

	/* Phase 2: the unlock, but only if the firmware actually refused and
	 * only if the key exists.  Asserting Ftst speculatively would suppress
	 * thermalmonitord's reclaim for no reason. */
	if (rc != FANPRO_SMC_BAD_COMMAND) {
		/*
		 * The write reported success but the fan did not read back as
		 * manual.  The write may well have landed and the verifying
		 * read merely failed, which would leave the fan pinned with our
		 * bookkeeping clean and nothing to reclaim it: on a direct-path
		 * machine there is no thermalmonitord safety net.  Undo it.
		 */
		FANPRO_WARN("unlock.acquire",
		            "fan=%d reason=unverified result=0x%02x action=force_auto",
		            fan_index, (unsigned)rc);
		write_retry(u, mode_key, FANPRO_FAN_MODE_AUTO);
		return -4;
	}
	if (!u->fans->has_ftst) {
		u->path = FANPRO_UNLOCK_PATH_NONE;
		FANPRO_ERROR("unlock.acquire",
		             "fan=%d reason=rejected_and_no_ftst result=0x82",
		             fan_index);
		return -5;
	}

	if (!u->ftst_asserted) {
		rc = fanpro_smc_write_num(u->smc, fanpro_fourcc("Ftst"), 1.0);
		if (!write_ok(rc)) {
			FANPRO_ERROR("unlock.acquire", "fan=%d reason=ftst_write result=0x%02x",
			             fan_index, rc < 0 ? 0xffu : (unsigned)rc);
			return -6;
		}
		u->ftst_asserted = true;
		FANPRO_INFO("unlock.ftst", "state=asserted");
	}

	if (!wait_for_yield(u, fan_index)) {
		/* No mode write was attempted, so this fan cannot be pinned.
		 * Only Ftst needs undoing, and only if nobody else needs it. */
		if (u->manual_count == 0)
			fanpro_unlock_release_all(u);
		return -7;
	}

	rc = fanpro_smc_write_num(u->smc, mode_key, FANPRO_FAN_MODE_MANUAL);
	if (!write_ok(rc) || fan_mode(u, fan_index) != FANPRO_FAN_MODE_MANUAL) {
		FANPRO_ERROR("unlock.acquire",
		             "fan=%d reason=mode_write_after_yield result=0x%02x",
		             fan_index, rc < 0 ? 0xffu : (unsigned)rc);
		/* Always undo THIS fan: the write may have landed even though
		 * the verification failed.  Safe to do while other fans are
		 * held, since it touches only this fan's mode key. */
		write_retry(u, mode_key, FANPRO_FAN_MODE_AUTO);
		/* Only tear down Ftst if nobody else is relying on it. */
		if (u->manual_count == 0)
			fanpro_unlock_release_all(u);
		return -8;
	}

	u->path = FANPRO_UNLOCK_PATH_FTST;
	u->manual[fan_index] = true;
	u->manual_count++;
	FANPRO_INFO("unlock.acquire", "fan=%d path=ftst held=%d", fan_index,
	            u->manual_count);
	return 0;
}

int
fanpro_unlock_release(fanpro_unlock_t *u, int fan_index)
{
	bool ok;

	if (!valid_fan(u, fan_index))
		return -1;

	/*
	 * Write FIRST, then update bookkeeping.  Clearing our record before
	 * the hardware agrees has two costs: a failed release would leave the
	 * fan pinned while we believe we hold nothing, and any concurrent
	 * drift check in that window reports a false positive.
	 */
	ok = write_retry(u, u->fans->fans[fan_index].key_md, FANPRO_FAN_MODE_AUTO);

	if (!ok) {
		/* Keep holding it on paper so a later pass tries again and the
		 * drift detector still recognises this fan as ours. */
		FANPRO_ERROR("unlock.release",
		             "fan=%d reason=mode_write_failed action=retain_ownership",
		             fan_index);
		return -2;
	}

	if (u->manual[fan_index]) {
		u->manual[fan_index] = false;
		if (u->manual_count > 0)
			u->manual_count--;
	}

	/*
	 * Ftst is global.  Clearing it while another fan is still manual would
	 * let thermalmonitord reclaim and silently end that fan's control, so
	 * it only goes with the last one.
	 */
	if (u->ftst_asserted && u->manual_count == 0) {
		if (write_retry(u, fanpro_fourcc("Ftst"), 0.0)) {
			u->ftst_asserted = false;
			FANPRO_INFO("unlock.ftst", "state=cleared");
		} else {
			FANPRO_ERROR("unlock.ftst", "state=stuck reason=write_failed");
			ok = false;
		}
	}

	FANPRO_INFO("unlock.release", "fan=%d held=%d ok=%d", fan_index,
	            u->manual_count, (int)ok);
	return ok ? 0 : -2;
}

int
fanpro_unlock_release_all(fanpro_unlock_t *u)
{
	int failures = 0;
	int i;

	if (u == NULL || u->fans == NULL)
		return -1;

	/* Every fan, not just the ones we believe we hold: recovery from an
	 * unclean previous run depends on this being unconditional. */
	for (i = 0; i < u->fans->count; i++) {
		if (!u->fans->fans[i].mode_key_known)
			continue;
		u->manual[i] = false;
		if (!write_retry(u, u->fans->fans[i].key_md, FANPRO_FAN_MODE_AUTO))
			failures++;
	}
	u->manual_count = 0;

	if (u->fans->has_ftst) {
		if (write_retry(u, fanpro_fourcc("Ftst"), 0.0))
			u->ftst_asserted = false;
		else
			failures++;
	}

	FANPRO_INFO("unlock.release_all", "failures=%d", failures);
	return failures == 0 ? 0 : -2;
}

int
fanpro_unlock_recover(fanpro_unlock_t *u)
{
	int stale = 0;
	int i;

	if (u == NULL || u->fans == NULL || u->smc == NULL)
		return -1;

	/*
	 * Startup only.  Recovery cannot tell "left over from a dead process"
	 * from "held by this one", so calling it mid-run would rip control
	 * away from the live control loop.  Anything that wants an emergency
	 * stop wants release_all instead.
	 */
	if (u->manual_count > 0) {
		FANPRO_ERROR("unlock.recover",
		             "reason=called_while_holding held=%d action=refused",
		             u->manual_count);
		return -2;
	}

	for (i = 0; i < u->fans->count; i++) {
		if (!u->fans->fans[i].mode_key_known)
			continue;
		if (fan_mode(u, i) == FANPRO_FAN_MODE_MANUAL)
			stale++;
	}

	if (u->fans->has_ftst) {
		double v;

		if (read_num(u->smc, fanpro_fourcc("Ftst"), &v) && v != 0.0)
			stale++;
	}

	if (stale == 0)
		return 0;

	/* Something left the hardware pinned: a SIGKILL, a panic, or another
	 * tool.  Hand everything back before doing anything else. */
	FANPRO_WARN("unlock.recover", "stale=%d action=release_all", stale);
	fanpro_unlock_release_all(u);
	return stale;
}

bool
fanpro_unlock_detect_drift(fanpro_unlock_t *u)
{
	bool drift = false;
	int i;

	if (u == NULL || u->fans == NULL || u->smc == NULL)
		return false;

	for (i = 0; i < u->fans->count; i++) {
		int observed;

		if (!u->fans->fans[i].mode_key_known)
			continue;

		observed = fan_mode(u, i);
		if (u->manual[i] && observed != FANPRO_FAN_MODE_MANUAL) {
			FANPRO_WARN("unlock.drift", "fan=%d expected=manual observed=%s",
			            i, fanpro_fan_mode_name(observed));
			drift = true;
		} else if (!u->manual[i] && observed == FANPRO_FAN_MODE_MANUAL) {
			FANPRO_WARN("unlock.drift", "fan=%d expected=not_manual observed=manual",
			            i);
			drift = true;
		}
	}

	if (u->fans->has_ftst) {
		double v;

		if (read_num(u->smc, fanpro_fourcc("Ftst"), &v)) {
			bool observed = (v != 0.0);

			if (observed != u->ftst_asserted) {
				FANPRO_WARN("unlock.drift", "key=Ftst expected=%d observed=%d",
				            (int)u->ftst_asserted, (int)observed);
				drift = true;
			}
		}
	}

	return drift;
}
