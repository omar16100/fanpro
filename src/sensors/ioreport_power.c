/*
 * fanpro - power draw via the private IOReport framework.
 *
 * IOReport is reached by dlopen rather than by linking.  That is the whole
 * point: this is undocumented SPI whose symbols and channel-group names have
 * moved between macOS releases, and a root daemon that fails to launch
 * because a private framework was renamed is a far worse outcome than one
 * that reports "power unavailable".
 *
 * Channel groups are discovered at runtime for the same reason.  The energy
 * counters are cumulative in millijoules, so watts come from a delta over a
 * measured interval, never from a single read.
 */
#include "fanpro/power.h"

#include "fanpro/log.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* IOReport SPI, declared here because there is no public header. */
typedef CFMutableDictionaryRef (*io_report_copy_all_channels_fn)(uint64_t, uint64_t);
typedef CFMutableDictionaryRef (*io_report_copy_group_fn)(CFStringRef, CFStringRef,
                                                          uint64_t, uint64_t, uint64_t);
typedef void *(*io_report_create_subscription_fn)(void *, CFMutableDictionaryRef,
                                                  CFMutableDictionaryRef *,
                                                  uint64_t, CFTypeRef);
typedef CFDictionaryRef (*io_report_create_samples_fn)(void *, CFMutableDictionaryRef,
                                                       CFTypeRef);
typedef CFDictionaryRef (*io_report_create_samples_delta_fn)(CFDictionaryRef,
                                                             CFDictionaryRef,
                                                             CFTypeRef);
typedef CFStringRef (*io_report_channel_get_group_fn)(CFDictionaryRef);
typedef CFStringRef (*io_report_channel_get_name_fn)(CFDictionaryRef);
/* The second parameter is an OUT pointer, not a flag.  Passing an integer
 * literal here segfaults the moment IOReport dereferences it. */
typedef int64_t (*io_report_simple_get_integer_value_fn)(CFDictionaryRef, int32_t *);

static struct {
	bool  tried;
	void *handle;
	/*
	 * Retained subscription.  IOReportCreateSubscription leaks a measured
	 * 235.7 KB per call on macOS 26.3 (Mac15,14), so calling it once per
	 * sample cost this daemon 1.6 GB/day.  Energy counters are cumulative,
	 * so one subscription serves every sample; only the delta pair is
	 * per-call.  See docs/15082026_ioreport_subscription_leak_plan.md.
	 */
	char                   group[FANPRO_POWER_NAME_MAX];
	CFMutableDictionaryRef subbed;
	void                  *sub;
	io_report_copy_all_channels_fn       copy_all_channels;
	io_report_copy_group_fn              copy_channels_in_group;
	io_report_create_subscription_fn     create_subscription;
	io_report_create_samples_fn          create_samples;
	io_report_create_samples_delta_fn    create_samples_delta;
	io_report_channel_get_group_fn       channel_group;
	io_report_channel_get_name_fn        channel_name;
	io_report_channel_get_name_fn        channel_unit;
	io_report_simple_get_integer_value_fn simple_value;
} g_ior;

/*
 * Where IOReport actually lives has moved.  On macOS 26.3 the framework
 * bundle does not exist at all and the code is at /usr/lib/libIOReport.dylib;
 * older releases used the PrivateFrameworks path.  Both are tried, newest
 * first, which is the concrete payoff of loading this at runtime.
 */
static const char *const IOREPORT_PATHS[] = {
	"/usr/lib/libIOReport.dylib",
	"/System/Library/PrivateFrameworks/IOReport.framework/IOReport",
	"/System/Library/PrivateFrameworks/IOReport.framework/Versions/A/IOReport",
};

static bool
ioreport_load(void)
{
	size_t i;

	if (g_ior.tried)
		return g_ior.handle != NULL;

	g_ior.tried = true;

	for (i = 0; i < sizeof(IOREPORT_PATHS) / sizeof(IOREPORT_PATHS[0]); i++) {
		g_ior.handle = dlopen(IOREPORT_PATHS[i], RTLD_LAZY | RTLD_LOCAL);
		if (g_ior.handle != NULL) {
			FANPRO_DEBUG("power.ioreport", "loaded=%s", IOREPORT_PATHS[i]);
			break;
		}
		/* dlerror() clears the error, so read it exactly once. */
		FANPRO_TRACE("power.ioreport", "path=%s miss=%s", IOREPORT_PATHS[i],
		             dlerror());
	}

	if (g_ior.handle == NULL) {
		FANPRO_WARN("power.ioreport", "reason=not_found paths_tried=%zu",
		            sizeof(IOREPORT_PATHS) / sizeof(IOREPORT_PATHS[0]));
		return false;
	}

	#define BIND(field, sym, type)                                        \
		g_ior.field = (type)dlsym(g_ior.handle, sym);                 \
		if (g_ior.field == NULL) {                                    \
			FANPRO_WARN("power.ioreport", "reason=missing_symbol sym=%s", sym); \
			dlclose(g_ior.handle);                                \
			g_ior.handle = NULL;                                  \
			return false;                                         \
		}

	BIND(copy_all_channels, "IOReportCopyAllChannels", io_report_copy_all_channels_fn)
	BIND(copy_channels_in_group, "IOReportCopyChannelsInGroup", io_report_copy_group_fn)
	BIND(create_subscription, "IOReportCreateSubscription", io_report_create_subscription_fn)
	BIND(create_samples, "IOReportCreateSamples", io_report_create_samples_fn)
	BIND(create_samples_delta, "IOReportCreateSamplesDelta", io_report_create_samples_delta_fn)
	BIND(channel_group, "IOReportChannelGetGroup", io_report_channel_get_group_fn)
	BIND(channel_name, "IOReportChannelGetChannelName", io_report_channel_get_name_fn)
	BIND(channel_unit, "IOReportChannelGetUnitLabel", io_report_channel_get_name_fn)
	BIND(simple_value, "IOReportSimpleGetIntegerValue", io_report_simple_get_integer_value_fn)

	#undef BIND

	return true;
}

static bool
cfstring_to_utf8(CFStringRef s, char *out, size_t len)
{
	if (s == NULL)
		return false;
	return CFStringGetCString(s, out, (CFIndex)len, kCFStringEncodingUTF8);
}

/*
 * Is this channel group an energy counter we can turn into watts?
 * Probed by name because the grouping has changed across releases; matching
 * loosely means a rename to something like "Energy Model v2" still works.
 */
static bool
is_energy_group(const char *group)
{
	return strstr(group, "Energy") != NULL || strstr(group, "energy") != NULL;
}

/*
 * Energy channels do NOT share a unit.  On this M3 Ultra the same group
 * carries mJ, uJ and nJ channels side by side, so treating everything as
 * millijoules reported the GPU at 60368 W instead of 0.06 W.  The unit label
 * is authoritative; an unrecognised one is dropped rather than guessed.
 */
static bool
joules_per_unit(const char *unit, double *scale)
{
	if (strcmp(unit, "J") == 0)   { *scale = 1.0;     return true; }
	if (strcmp(unit, "mJ") == 0)  { *scale = 1e-3;    return true; }
	if (strcmp(unit, "uJ") == 0)  { *scale = 1e-6;    return true; }
	if (strcmp(unit, "nJ") == 0)  { *scale = 1e-9;    return true; }
	if (strcmp(unit, "pJ") == 0)  { *scale = 1e-12;   return true; }
	return false;
}

/*
 * Energy Model channels are hierarchical: per-core, then cluster, then die
 * total, all in the same list.  Summing them multiple-counts the same
 * silicon several times over, so only the top-level aggregates (the ones
 * whose names end in "Energy") are reported.
 */
static bool
is_aggregate_channel(const char *name)
{
	size_t n = strlen(name);
	const char *suffix = " Energy";
	size_t sn = strlen(suffix);

	return n >= sn && strcmp(name + n - sn, suffix) == 0;
}

/* "DIE_0_CPU Energy" -> "CPU", so both dies fold into one figure. */
static void
tidy_name(const char *in, char *out, size_t out_len)
{
	const char *p = in;
	size_t n;

	if (strncmp(p, "DIE_", 4) == 0) {
		const char *underscore = strchr(p + 4, '_');

		if (underscore != NULL)
			p = underscore + 1;
	}

	snprintf(out, out_len, "%s", p);
	n = strlen(out);
	if (n > 7 && strcmp(out + n - 7, " Energy") == 0)
		out[n - 7] = '\0';
}

static void
add_channel(fanpro_power_set_t *set, const char *name, double watts)
{
	int i;

	if (!isfinite(watts) || watts < 0.0)
		return;

	/* Several channels can share a name; fold them together rather than
	 * present the same label twice with different numbers. */
	for (i = 0; i < set->count; i++) {
		if (strcmp(set->channels[i].name, name) == 0) {
			set->channels[i].watts += watts;
			set->total_watts += watts;
			return;
		}
	}

	if (set->count >= FANPRO_MAX_POWER_CHANNELS)
		return;

	snprintf(set->channels[set->count].name,
	         sizeof(set->channels[set->count].name), "%s", name);
	set->channels[set->count].watts = watts;
	set->count++;
	set->total_watts += watts;
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void
sleep_ms(unsigned ms)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ms / 1000u);
	ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
	nanosleep(&ts, NULL);
}

/*
 * Find the channel group carrying energy counters.
 *
 * Subscribing to every channel is not an option: this machine reports 11394
 * of them and IOReportCreateSubscription segfaults on a set that size.  So
 * the group name is discovered from the channel list (which is safe to walk)
 * and only that group is subscribed to.  Discovering rather than hardcoding
 * "Energy Model" means a rename in a future macOS still works.
 */
static bool
find_energy_group(char *out, size_t out_len)
{
	CFMutableDictionaryRef all;
	CFArrayRef chans;
	CFIndex n, i;
	bool found = false;

	all = g_ior.copy_all_channels(0, 0);
	if (all == NULL)
		return false;

	chans = (CFArrayRef)CFDictionaryGetValue(all, CFSTR("IOReportChannels"));
	if (chans == NULL) {
		CFRelease(all);
		return false;
	}

	n = CFArrayGetCount(chans);
	for (i = 0; i < n && !found; i++) {
		CFDictionaryRef ch = (CFDictionaryRef)CFArrayGetValueAtIndex(chans, i);
		char group[FANPRO_POWER_NAME_MAX] = { 0 };

		if (ch == NULL)
			continue;
		if (!cfstring_to_utf8(g_ior.channel_group(ch), group, sizeof(group)))
			continue;
		if (!is_energy_group(group))
			continue;

		snprintf(out, out_len, "%s", group);
		found = true;
	}

	/* We never handed this dictionary to a subscription, so we own it. */
	CFRelease(all);

	if (found)
		FANPRO_DEBUG("power.group", "found=%s", out);
	else
		FANPRO_WARN("power.group", "reason=no_energy_group channels=%ld",
		            (long)n);
	return found;
}

/*
 * Release the retained subscription.  Safe to call on a partially built one:
 * create_subscription can hand back a subscription with no `subbed` dictionary
 * or vice versa, and leaking either half would defeat the point of this file.
 */
static void
real_teardown(void)
{
	if (g_ior.subbed != NULL) {
		CFRelease(g_ior.subbed);
		g_ior.subbed = NULL;
	}
	if (g_ior.sub != NULL) {
		CFRelease((CFTypeRef)g_ior.sub);
		g_ior.sub = NULL;
	}
}

/* Discover the energy group and subscribe to it.  0 on success. */
static int
real_subscribe(void)
{
	CFMutableDictionaryRef group_chans;
	CFStringRef group_cf;

	/* The state machine never calls this while holding one, but subscribing
	 * over the top of a live subscription would leak 235.7 KB with no way to
	 * notice, so make the precondition explicit rather than assumed. */
	real_teardown();

	if (!ioreport_load())
		return -2;

	if (!find_energy_group(g_ior.group, sizeof(g_ior.group)))
		return -3;

	group_cf = CFStringCreateWithCString(kCFAllocatorDefault, g_ior.group,
	                                     kCFStringEncodingUTF8);
	if (group_cf == NULL)
		return -3;

	group_chans = g_ior.copy_channels_in_group(group_cf, NULL, 0, 0, 0);
	CFRelease(group_cf);
	if (group_chans == NULL) {
		FANPRO_WARN("power.subscribe", "reason=no_group_channels group=%s",
		            g_ior.group);
		return -3;
	}

	/*
	 * IOReportCreateSubscription CONSUMES the channels dictionary.
	 * Releasing it afterwards is a double-release; ownership passes here.
	 */
	g_ior.sub = g_ior.create_subscription(NULL, group_chans, &g_ior.subbed, 0,
	                                      NULL);
	if (g_ior.sub == NULL || g_ior.subbed == NULL) {
		/* group_chans is deliberately not released here.  Ownership passes
		 * to create_subscription on success; on failure the SPI is
		 * undocumented and releasing a dictionary it may already have
		 * consumed would be a double-release, which is worse than leaking
		 * one dictionary on a path the backoff already rate-limits. */
		FANPRO_WARN("power.subscribe", "reason=subscription_failed group=%s",
		            g_ior.group);
		real_teardown();
		return -4;
	}

	FANPRO_INFO("power.subscribe", "group=%s", g_ior.group);
	return 0;
}

static int
real_sample_delta(unsigned interval_ms, fanpro_power_set_t *set)
{
	CFDictionaryRef first = NULL, second = NULL, delta = NULL;
	CFArrayRef items;
	uint64_t t0, elapsed;
	double seconds;
	CFIndex n, i;
	int found = 0;
	int rc = -5;

	/* Energy counters are cumulative, so a single read is meaningless:
	 * watts come from a delta over a measured interval. */
	t0 = now_ms();
	first = g_ior.create_samples(g_ior.sub, g_ior.subbed, NULL);
	sleep_ms(interval_ms);
	second = g_ior.create_samples(g_ior.sub, g_ior.subbed, NULL);
	elapsed = now_ms() - t0;

	if (first == NULL || second == NULL) {
		FANPRO_WARN("power.sample", "reason=sampling_failed");
		goto done;
	}

	delta = g_ior.create_samples_delta(first, second, NULL);
	if (delta == NULL) {
		FANPRO_WARN("power.sample", "reason=delta_failed");
		goto done;
	}

	/* Divide by the measured elapsed time, not the requested interval: the
	 * sleep can overrun and the wrong divisor inflates every reading. */
	seconds = (double)elapsed / 1000.0;
	if (seconds <= 0.0)
		goto done;

	items = (CFArrayRef)CFDictionaryGetValue(delta, CFSTR("IOReportChannels"));
	if (items == NULL)
		goto done;

	n = CFArrayGetCount(items);
	for (i = 0; i < n; i++) {
		CFDictionaryRef ch = (CFDictionaryRef)CFArrayGetValueAtIndex(items, i);
		char name[FANPRO_POWER_NAME_MAX] = { 0 };
		char unit[32] = { 0 };
		char tidy[FANPRO_POWER_NAME_MAX] = { 0 };
		double scale = 0.0;
		int64_t raw;

		if (ch == NULL)
			continue;
		if (!cfstring_to_utf8(g_ior.channel_name(ch), name, sizeof(name)))
			continue;
		if (!is_aggregate_channel(name))
			continue;
		if (!cfstring_to_utf8(g_ior.channel_unit(ch), unit, sizeof(unit)))
			continue;
		if (!joules_per_unit(unit, &scale)) {
			FANPRO_TRACE("power.unit", "channel=%s unknown_unit=%s", name,
			             unit);
			continue;
		}

		raw = g_ior.simple_value(ch, NULL);
		if (raw <= 0)
			continue;

		tidy_name(name, tidy, sizeof(tidy));
		add_channel(set, tidy, ((double)raw * scale) / seconds);
		found++;
	}

	set->available = set->count > 0;
	FANPRO_DEBUG("power.sample",
	             "group=%s channels=%d raw=%d total_w=%.2f elapsed_ms=%llu",
	             g_ior.group, set->count, found, set->total_watts,
	             (unsigned long long)elapsed);
	rc = set->available ? 0 : -5;

done:
	/*
	 * Only the per-sample objects are released here.  g_ior.sub and
	 * g_ior.subbed are retained across calls and are freed by real_teardown
	 * alone; releasing them here is what leaked 235.7 KB per sample.
	 */
	if (delta != NULL)
		CFRelease(delta);
	if (second != NULL)
		CFRelease(second);
	if (first != NULL)
		CFRelease(first);

	return rc;
}

/* ---- state machine ------------------------------------------------------ */

/*
 * Injected ops, or NULL for the real IOReport ones.  Only the tests set this.
 */
static const fanpro_power_ops_t *g_ops;

/*
 * Authoritative for the state machine, and deliberately separate from
 * g_ior.subscribed so the injected-ops path obeys the same subscribe-once rule
 * as the real one.  That rule is the entire fix, so the tests must be able to
 * observe it without IOReport.
 */
static bool     g_subscribed;
static unsigned g_subscribe_count;
static unsigned g_consec_failures;
static unsigned g_skip_remaining;

/* Largest number of samples to skip after repeated failure.  At the daemon's
 * ~13 s sampling period this is roughly 14 minutes between attempts, which
 * bounds the damage if a future macOS breaks the SPI: one subscription per 14
 * minutes instead of one per sample. */
#define POWER_MAX_SKIP 64u

static int
ops_subscribe(void)
{
	return g_ops != NULL ? g_ops->subscribe() : real_subscribe();
}

static void
ops_teardown(void)
{
	if (g_ops != NULL)
		g_ops->teardown();
	else
		real_teardown();

	g_subscribed = false;
}

static int
ops_sample_delta(unsigned interval_ms, fanpro_power_set_t *set)
{
	return g_ops != NULL ? g_ops->sample_delta(interval_ms, set)
	                     : real_sample_delta(interval_ms, set);
}

static void
record_failure(void)
{
	if (g_consec_failures < 6u)
		g_consec_failures++;

	g_skip_remaining = 1u << g_consec_failures;
	if (g_skip_remaining > POWER_MAX_SKIP)
		g_skip_remaining = POWER_MAX_SKIP;

	FANPRO_WARN("power.backoff", "consec_failures=%u skip_samples=%u",
	            g_consec_failures, g_skip_remaining);
}

void
fanpro_power_set_ops(const fanpro_power_ops_t *ops)
{
	ops_teardown();
	g_ops = ops;
	g_subscribe_count = 0;
	g_consec_failures = 0;
	g_skip_remaining = 0;
}

unsigned
fanpro_power_subscribe_count(void)
{
	return g_subscribe_count;
}

void
fanpro_power_invalidate(void)
{
	ops_teardown();

	/*
	 * Clearing the backoff is the point, not a side effect.  Wake is new
	 * information: the reason sampling was failing before the machine slept
	 * says nothing about whether it will fail now, and leaving a widened
	 * skip window in place would silently swallow the next 64 samples
	 * (roughly 14 minutes) of the very reprobe this call exists to trigger.
	 */
	g_consec_failures = 0;
	g_skip_remaining = 0;
}

void
fanpro_power_shutdown(void)
{
	ops_teardown();

	/* Symmetric with fanpro_power_invalidate.  Unreachable in the daemon,
	 * where shutdown runs after the control loop has exited, but leaving the
	 * skip window armed would mean a restarted sampler silently refusing to
	 * sample for up to 64 calls. */
	g_consec_failures = 0;
	g_skip_remaining = 0;
}

int
fanpro_power_sample(fanpro_power_set_t *set, unsigned interval_ms)
{
	int attempt;
	int rc = -4;

	if (set == NULL)
		return -1;

	memset(set, 0, sizeof(*set));

	if (interval_ms == 0)
		interval_ms = 200;

	/*
	 * Backing off after repeated failure is not politeness, it is the leak
	 * guard: every resubscribe costs 235.7 KB whether or not it then works,
	 * so a permanently broken SPI must not be retried once per sample.
	 */
	if (g_skip_remaining > 0u) {
		g_skip_remaining--;
		return -6;
	}

	/*
	 * Two attempts, never recursive: the first uses whatever subscription we
	 * hold, and if sampling fails the subscription is torn down so the second
	 * establishes a fresh one.  A stale subscription after wake is the
	 * expected reason to land here.
	 */
	for (attempt = 0; attempt < 2; attempt++) {
		bool fresh = false;

		if (!g_subscribed) {
			rc = ops_subscribe();
			if (rc != 0) {
				record_failure();
				return rc;
			}
			g_subscribed = true;
			g_subscribe_count++;
			fresh = true;
		}

		rc = ops_sample_delta(interval_ms, set);
		if (rc == 0) {
			g_consec_failures = 0;
			g_skip_remaining = 0;
			return 0;
		}

		ops_teardown();
		memset(set, 0, sizeof(*set));

		/*
		 * Only a subscription we had been holding is worth replacing: it
		 * may have gone stale across a sleep, which is the case this
		 * retry exists for.  If a subscription created moments ago
		 * already failed to sample, IOReport is broken rather than
		 * stale, and building another one would just leak 235.7 KB to
		 * learn the same thing.
		 */
		if (fresh)
			break;

		FANPRO_WARN("power.resubscribe", "reason=stale_sample rc=%d", rc);
	}

	record_failure();
	return rc;
}
