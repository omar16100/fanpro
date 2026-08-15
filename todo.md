# fanpro todo

Running status log. Newest first.

## 2026-08-15 - IOReport subscription leak (branch `fix/ioreport-subscription-leak`)

**Done.**

- `fanpro_power_sample` no longer creates an IOReport subscription per call.
  `IOReportCreateSubscription` leaks a measured 235.7 KB per call on macOS 26.3
  (Mac15,14); at the daemon's ~13 s sampling period that was 1.6 GB/day, and the live
  `fanprod` had reached 11.97 GB RSS over 8 days.
- Added the subscription state machine: subscribe once, replace a held subscription that
  fails to sample, never rebuild one that was just created, widen a skip window
  (2,4,8,16,32,64) on repeated failure so a broken SPI cannot leak at full rate.
- Added `fanpro_power_invalidate` (called from the control loop's wake reprobe) and
  `fanpro_power_shutdown` (daemon exit).
- Added a `fanpro_power_ops_t` test seam and `tests/test_power.c` (8 hermetic tests) so
  the state machine is provable without IOReport.
- `make test` 3729 checks / 0 failures. `make check-live` +32 KB over 40 real samples,
  subscribe count 1.
- Docs: `c4model.md` component row, `engineering-log.md` measurements,
  `docs/15082026_ioreport_subscription_leak_plan.md`, `docs/index.md`.

**Verified live 2026-08-15 19:13.** Installed and restarted. Outgoing PID 29738 had
reached 13.28 GB after 9d 06h and released both fans cleanly. New PID 93421 held **flat at
14,864 KB for 21 consecutive 30s samples** (t+8:00 to t+18:01); the only growth was 144 KB
of warm-up. `power.subscribe` logged exactly once; `power.backoff`/`power.resubscribe`
never fired.

**Still untested in production:** the backoff and resubscribe paths (IOReport has not
failed) and sleep/wake invalidation (machine has 11 days uptime, has not slept since).

## Open, found during the same investigation

Both are real and neither belongs in the leak fix:

1. **fanprod is not driving the fans.** `driving=0 held=0` on every tick since 2026-08-06
   12:22, after `daemon.drift strikes=3 action=latched hint=another tool keeps rewriting
   the target`. It has been monitor-only for its entire 8-day run. Find the other writer
   or decide fanpro should yield to it explicitly.
2. **No GPU temperature sensor is ever resolved.** `gpu_c` is null in 55,389 of 55,389
   history samples for 2026-08-14. A fan curve with no GPU input on a GPU-loaded machine
   is a real gap: `registry.c` classification never assigns `FANPRO_CLASS_GPU` on this
   hardware.
