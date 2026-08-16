# fanprod IOReport subscription leak: fix plan

Status: IMPLEMENTED on `fix/ioreport-subscription-leak`. Codex-reviewed before coding;
review outcomes folded in below.
Date: 2026-08-15.

## What review changed

The first draft of this plan would have shipped a bug and an untestable fix. Recorded
because both corrections are the interesting part:

1. **The retry could reproduce the leak at full rate.** "Tear down, resubscribe once,
   retry once, next tick try again" means a persistently failing `IOReportCreateSamples`
   costs a fresh 235.7 KB subscription on every sample, which is exactly the bug. Fixed by
   the widening skip window.
2. **A freshly created subscription that fails must not be rebuilt in the same call.**
   Found while writing the tests: retrying is only meaningful for a subscription that had
   been held and may have gone stale. This halves the cost of the failure path.
3. **Wake invalidation belongs on the control-loop thread**, in the `reprobe_requested`
   block of `control_loop.c`, not in `power_notify.c`, which correctly only sets atomics.
4. **RSS was the wrong primary gate.** It is allocator and hardware dependent. The
   subscribe-count property is what actually matters and is now testable without IOReport
   through a `fanpro_power_ops_t` seam. RSS survives as a live diagnostic only.
5. **Partial subscription creation must be cleaned up** (`sub` without `subbed` or the
   reverse), or the teardown path leaks the half that did get built.
6. Over-claims removed: "IOReport expects long-lived subscriptions" is not something I can
   source, so it is stated as measured-safe on this target rather than as a contract.

## Problem

`fanprod` grew to 11.97 GB RSS over 8 days of uptime (PID 29738, started 2026-08-06
12:21:50). Measured live growth: +1152 KB over 60 s = **19.2 KB/s = 1.6 GB/day**.

## Root cause (measured, not inferred)

`fanpro_power_sample` (`src/sensors/ioreport_power.c:294`) creates a fresh IOReport
subscription on every call at `:337`, and discovers the energy channel group from
scratch at `:318`.

Isolating repro (`build/powerleak.c`, 200 iterations, RSS sampled after a 5-iteration
warm-up):

| pattern | leak per iteration |
|---|---|
| `IOReportCopyAllChannels` only | +4.4 KB, decaying (fragmentation, not a leak) |
| `IOReportCreateSubscription` per sample | **+235.7 KB, dead-linear** |
| full sample (subscribe + sample) | +237.5 KB |

The slope over the final five checkpoints was 235.7, 235.8, 235.7, 235.7, 235.6, 235.4
KB/iter: constant to four significant figures, which is a leak rather than growth of a
live working set. `CFRelease(subbed)` and `CFRelease((CFTypeRef)sub)` at `:420-422` do
not reclaim it.

### The arithmetic closes

`control_loop.c:697` samples power every 10 passes. Measured tick period from
`/var/log/fanpro/fanprod.log` is ~1.3 s (60 passes per 78 s), so power is sampled every
13.0 s.

    235.7 KB / 13.0 s = 18.1 KB/s predicted
                        19.2 KB/s measured

Within 6%. The subscription accounts for essentially all of the observed growth, so
there is no second leak to hunt.

## Fix

Hoist both the energy-group discovery and the subscription into a lazily initialised,
process-lifetime context. Sample against the retained subscription.

Verified against the same repro (`build/powerfix.c`, subscribe once then 200 sample
pairs): **+0.16 KB/iter and decaying to flat** (+32 KB total, all of it in the first 50
iterations), versus +235.7 KB/iter today.

Re-subscribing per sample was never buying anything: energy counters are cumulative, so
one subscription serves every delta. Stated as what it is, a measured result on this
target (macOS 26.3, Mac15,14), not an IOReport contract. There is no public Apple
documentation for this SPI, so "long-lived subscriptions are the intended usage" is not a
claim this work can support.

### Shape

Extend the existing `g_ior` static with the retained state:

    static struct {
            ...                                   /* existing dlsym bindings */
            bool                    subscribed;
            char                    group[FANPRO_POWER_NAME_MAX];
            CFMutableDictionaryRef  subbed;
            void                   *sub;
    } g_ior;

`fanpro_power_sample` becomes: ensure-subscribed (once), then two `create_samples` calls
`interval_ms` apart, delta, parse. The two-sample-per-call blocking semantics stay
exactly as they are, so no caller changes and no behaviour change for the control loop.

### Open questions for review

1. **Sleep/wake invalidation.** `src/daemon/power_notify.c` re-probes on wake. Does a
   retained `IOReportSubscriptionRef` survive a sleep cycle? Plan: treat a NULL return
   from `create_samples` as invalidation, tear the subscription down, re-subscribe once,
   and retry a single time. If the retry also fails, report unavailable and let the next
   tick try again. This bounds a re-subscribe to once per failure rather than per sample,
   so a pathological wake cannot reintroduce the leak at full rate.
2. **Teardown.** Add `fanpro_power_shutdown(void)` for symmetry and call it from the
   daemon's exit path alongside the other releases. Process exit would reclaim it anyway,
   so this is hygiene, not correctness.
3. **Thread safety.** `c4model.md` needs to confirm power sampling is only ever called
   from the control-loop thread. If the TUI or CLI can call it concurrently the static
   needs a mutex. Believed single-threaded; must be verified, not assumed.
4. **Is `CFRelease` on `sub` even correct?** It is currently called on a pointer returned
   as `void *`. Since we now release it at most once per process this stops mattering in
   practice, but the teardown path should not introduce a new crash risk. Prefer leaving
   the release in place (unchanged semantics, just far rarer).

## Tests

`tests/test_sensors.c` (or a new `tests/test_power.c`), fast and hermetic where possible:

1. `power_group_discovered_once` - drive the ensure-subscribed path twice, assert the
   channel enumeration ran once. Needs a seam; the existing `smc_fake.h` pattern suggests
   the codebase already accepts injected backends for this.
2. `power_sample_rss_bounded` - live test behind `FANPRO_LIVE=1` (the existing
   `check-live` target): 50 samples at `interval_ms=1`, assert RSS growth stays under a
   generous ceiling (say 2 MB). Today's code fails this by ~11 MB; the fix passes with
   room to spare. This is the test that would actually have caught the bug.
3. `power_resubscribe_on_failure` - fault-inject a NULL `create_samples` and assert one
   re-subscribe plus one retry, not a loop.

Note honestly: 2 and 3 are the ones that matter and both need either a live GPU/IOReport
or a seam. If the seam is too invasive for this change, 2 alone behind `check-live` is
the minimum acceptable coverage, and that limitation goes in the doc.

## Logging (as built)

- `INFO power.subscribe group=%s` once, at each subscription. On the live daemon this
  appeared exactly once for the whole run, which is what makes the fix observable in the
  log rather than only in RSS.
- `WARN power.resubscribe reason=stale_sample rc=%d` when a held subscription is replaced.
- `WARN power.backoff consec_failures=%u skip_samples=%u` when the skip window widens, so
  a degraded SPI is visible rather than silent.
- The existing per-sample `FANPRO_DEBUG("power.sample", ...)` is unchanged.

## Docs to update in the same commit

- `docs/c4model.md`: the `ioreport_power.c` component row currently implies per-sample
  subscription. Update to describe the retained subscription and its invalidation rule.
- `docs/engineering-log.md`: the measurement table above, with the numbers.
- `docs/index.md`: register this doc.
- `todo.md`: task entry.

## Result

- `make test`: 3733 checks, 0 failures (10 new hermetic tests for the state machine).
- `make check-live`: real IOReport, **rss 13776 -> 13808 kb over 40 samples (+32 KB)**,
  subscribe count 1. The same 40 samples on the old code would have leaked ~9.4 MB.

### Verified live on the installed daemon (2026-08-15 19:13)

`make install` + `launchctl kickstart -k system/pro.fanpro.daemon`. The outgoing process
(PID 29738, up 9d 06h) had reached **13.28 GB RSS**; it released both fans cleanly on the
way out (`unlock.release_all failures=0`).

New process (PID 93421) started at 14 MB. RSS over the following 18 minutes:

    t+00:30   14720 kb     +0 kb
    t+03:30   14800 kb    +80 kb
    t+06:00   14832 kb   +112 kb
    t+08:00   14864 kb   +144 kb
    t+10:00   14864 kb   +144 kb
    ...
    t+18:01   14864 kb   +144 kb     <- 21 consecutive samples, zero growth

All 144 KB of growth is warm-up in the first 8 minutes; the last 10 minutes are
byte-for-byte flat. The old code at 19.2 KB/s would have added ~20 MB over the same 18
minutes.

Log confirms the mechanism, not just the outcome:

- `power.subscribe group=Energy Model` appears **exactly once** for the whole run
- `power.backoff` and `power.resubscribe`: **zero** occurrences, so the recovery paths are
  untested in production. They are covered only by the injected-ops tests.

### Still unverified

The backoff and resubscribe paths have not fired on real hardware, because IOReport has
not failed. Sleep/wake invalidation is likewise untested live: this machine has 11 days of
uptime and has not slept since the fix went in.

## Out of scope (found during investigation, filed separately)

Both are real and worth their own work, but neither belongs in a leak fix:

1. **fanprod is not driving the fans.** `driving=0 held=0` on every tick since
   2026-08-06 12:22, after `daemon.drift strikes=3 action=latched hint=another tool keeps
   rewriting the target`. It has been monitor-only for its whole 8-day run.
2. **No GPU temperature sensor is ever resolved.** `gpu_c` is null in 55,389 of 55,389
   history samples. A fan curve with no GPU input on a GPU-loaded machine is a real gap.
