# fanpro todo

Running status log. Newest first.

## 2026-09-27 - maintenance sweep (PR from `fix/thermal-custody-findings-18092026`)

Plan: [`docs/27092026_maintenance_sweep_plan.md`](docs/27092026_maintenance_sweep_plan.md).

- Publishing the 18 Sep thermal custody fixes (below) through a PR to `main`.
- `README.md`: replaced all 20 em dashes (18 lines) with colons, commas,
  parentheses or full stops. No wording or claims changed.
- `docs/c4model.md`: recorded the effective-mode gate in the IPC server and the
  atexit handler honouring a failed release, both from the 18 Sep change.
- Deleting remote branch `fix/ioreport-subscription-leak` (b7bb03d), already merged
  into `main` via 92bdd15.
- Review fix: `fanpro_daemon_effective_mode()` now treats a queued `reload` newer
  than any queued `set-mode` as unknown and falls back to the applied mode, so
  starting in auto, `mode curve`, `reload` (file says auto), `set 3625` in one tick is refused
  rather than accepted and silently dropped. Regression test added.
- Coverage-warning test now asserts the logged warning, not just the flag.
- Corrected wording in `etc/fanpro.conf.example`, the launchd plist comment and
  the 18 Sep plan (see the sweep plan for the list).
- Narrowed the `10.0` placeholder rejection to the measured `Ta0*` keys, so a
  genuine 10 °C on another key is kept. Test added.
- `make test`: 3775 checks, 0 failures. Nothing that touches SMC, fans or launchd
  was run.
- Open: no test covers the `atexit` marker path (`emergency_release` is static).

## 2026-09-18 - thermal custody findings (branch `fix/thermal-custody-findings-18092026`)

Prompted by 15 `Thermal Emergency Sleep` events, 14:37:54-14:40:41, then a reboot.
Cause was three ~185 W LLM benchmark arms taking the SoC to 83.2 °C. fanpro was
holding both fans at 3625 (max) at the instant of the first sleep, so it was not
under-driving the fans at that moment (whether earlier, higher speeds would have
helped is not established). Full write-up:
[`docs/18092026_thermal_custody_findings_plan.md`](docs/18092026_thermal_custody_findings_plan.md).

**Done.**

- Fixed the mode/set race: `fanpro mode curve && fanpro set all 3625` was unwinnable
  because the IPC thread acked a queued set-mode before the loop applied it, so the
  set-fan gate read the stale `cfg.mode` and refused. Worse than a refusal: the mode
  change still landed, and the `safe` curve then pulled both fans 2500 -> 1180 RPM,
  cooling less than not intervening. Gate now uses `fanpro_daemon_effective_mode()`.
  Loop remains the sole writer of `cfg`. Refusal message no longer tells the user to
  run the command they just ran.
- Rejected the `Ta0*` firmware placeholder (`00 00 20 41`, exactly 10.0) in
  `smc_temp.c`, the same way exactly 0.0 already was. It was being filed under the
  ambient class as a finite reading, making `panic_ambient_c = 70` a guard permanently
  60 °C clear of its threshold.
- Added a one-shot `safety.coverage` warning for any panic threshold on a class with no
  enumerated sensors, after the first successful enumeration. Catches the dead ambient
  class and the dead GPU class below.
- Fixed the atexit handler unconditionally clearing the clean-exit marker that `main`
  deliberately leaves when its release fails, which meant that latch could never fire.
- `ProcessType` Background -> Interactive in the plist.
- Narrowed the false "sits at or above the firmware at every temperature" claim in
  `README.md`, `docs/engineering-log.md` and `etc/fanpro.conf.example`. The firmware's
  RPM is path dependent: 1374 rising vs 2510 settled at the same 64.0 °C in our own
  trace. Commented out `panic_gpu_c` and `panic_ambient_c` in the example config.
- `make test` 3760 checks / 0 failures (was 3733). Placeholder test verified to fail
  when the rejection is disabled.

**Not deployed.** Installed `/usr/local/sbin/fanprod` is from 15 Aug; HEAD is four
commits ahead, including the IOReport leak fix. Deploying ships those too.

**Next, needs a decision.**

- `fanpro_unlock_release_all` clears `manual[]` before the writes and zeros
  `manual_count` even on failure; caller ignores the result and bumps
  `release_generation`. A failed release can be reported as confirmed while the
  hardware stays pinned. Highest-risk item found.
- Heartbeat latch policy: the two reviews disagree on whether a stall should latch.
  Deeper issue both raise: the flag is only read by a loop that is merely slow, so a
  wedged loop never sees it and `KeepAlive` will not restart a hung process.
- Root cause of the stalls is still unknown. Neither the CPU-starvation nor the
  leaked-subscription hypothesis is supported by the data (stalls happen only near
  idle, and at pass 8640 as well as 1,762,920), though neither is ruled out. Needs per-phase tick timings.
- `set all auto` / `daemon release` can reacquire in the same tick; sleep release
  silently discards `manual_override`; monitor-only still releases a fan another
  controller owns; panic hysteresis clears on SoC whichever class tripped;
  `fanpro status` labels every latch "previous run exited uncleanly".

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
- `make test` 3733 checks / 0 failures (10 hermetic power tests). `make check-live` +32 KB
  over 40 real samples, subscribe count 1.
- Docs: `c4model.md` component row, `engineering-log.md` measurements,
  `docs/15082026_ioreport_subscription_leak_plan.md`, `docs/index.md`.

**Verified live 2026-08-15 19:13.** Installed and restarted. Outgoing PID 29738 had
reached 13.28 GB after 9d 06h and released both fans cleanly. New PID 93421 held **flat at
14,864 KB for 21 consecutive 30s samples** (t+8:00 to t+18:01); the only growth was 144 KB
of warm-up. `power.subscribe` logged exactly once; `power.backoff`/`power.resubscribe`
never fired.

**Known gaps (from pre-push review):** when IOReport returns zero usable channels the
daemon reports 0.00 W rather than "unavailable" in the UI and history, because
`publish_snapshot` zero-fills; `set->available` catches "no channels" but not "channels
present but semantically wrong". A cumulative counter that wraps between the two samples
would pass the `raw <= 0` guard and yield an absurd watt figure. Both are observability
hardening, not leak issues.

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
