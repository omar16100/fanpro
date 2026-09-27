# Thermal custody findings and fixes, 18 Sep 2026

Status: fixes 1-5 implemented and unit tested on `fix/thermal-custody-findings-18092026`, published through the 27 Sep 2026 maintenance PR (see [`27092026_maintenance_sweep_plan.md`](27092026_maintenance_sweep_plan.md)). Not deployed. Items under "Deferred" are unstarted.

## What prompted this

macOS forced `Thermal Emergency Sleep` 15 times between 14:37:54 and 14:40:41, then the machine rebooted at 14:55. Cause was three LLM benchmark runs (one per test arm) at sustained 184-187 W, driving the SoC to 81.3, 83.1 and 83.2 °C.

**fanpro was not under-driving the fans when it happened.** At 14:37:54, the instant of the first thermal sleep, `history.jsonl` records `driving=true`, both fans `manual`, target 3625, actual 3621/3634. The fans were at the hardware maximum and the machine overheated anyway. This does not show that earlier, higher fan speeds would have made no difference; it shows the emergency was not caused by fanpro holding the fans low at that moment. Either this chassis cannot sustain ~185 W, or a component fanpro cannot read reached its limit first. The kernel was already cutting the CPMS thermal budget from 180 W at 14:37:20.

Two claims made during the investigation were wrong and are recorded here so they are not repeated:

- "80 heartbeat stalls during the benchmark, all in hour 13." Wrong. 80 is the whole-log count since 14 Aug. On 18 Sep there were **13**, all between 13:25:07 and 13:32:47, over an hour before the emergency and **not** under load (cpu 0.2-5.8 W, SoC 57-61 °C falling). Control was re-enabled at 13:53 and 14:30.
- "`ProcessType=Background` starves the control loop under a saturating load." Unproven and probably wrong. Stalls only ever occur when the machine is near idle, and the benchmark is GPU bound (cpu_w median 1.1 W during the 256k arm). A leaked-IOReport-subscription explanation was also tested and is not supported: stalls occur at pass 8640 as well as at 1,762,920, so they do not appear to track uptime. During the 18 Sep cluster the tick counter advanced 60 passes in ~7 minutes (~7 s/tick on average). An average cannot distinguish a uniformly slow loop from a few long blocking calls. **Root cause still unknown.**

## Implemented

1. **The mode/set race** (`src/daemon/ipc_server.c`, `src/daemon/fanprod.c`, `src/daemon/daemon.h`). The IPC thread acked a queued `set-mode` with status 0 before the control loop applied it, so the `set-fan` gate read the still-`AUTO` `cfg.mode` and refused the speed. `fanpro mode curve && fanpro set all 3625` therefore lost a 1.2-1.7 s race, and the refusal left the default curve to pull both fans from 2500 to 1180 RPM: cooling less than not intervening. Fixed by gating on a new `fanpro_daemon_effective_mode()`, which reads the newest queued `set-mode` under `cmd_lock` and falls back to `cfg.mode`. The control loop remains the sole writer of `cfg`, so the concurrency rule in `daemon.h` is unchanged. Refusal message also corrected: it used to tell the user to run the command they had just run.

   Known residual, accepted: a `set-mode` already drained into the loop's local batch but not yet applied is invisible to the queue scan. The window lasts as long as the commands ahead of it in that batch take to run, which is not bounded (they can log, reload config, or retry an SMC release). In that window a pending switch to auto can let a numeric `set` through against the stale curve mode, and a pending switch to curve can still refuse one. Closing it fully needs an applied-sequence counter and a bounded wait on the ack. A queued `reload` is handled separately: it can change the mode to whatever the file says, so when it is the newest mode-changing request the gate uses the applied mode (the pre-fix behaviour) rather than guessing.

2. **Placeholder sensor admitted as a real reading** (`src/sensors/smc_temp.c`). `Ta09/Ta0H/Ta0L/Ta0P` and `ftA0` all return the identical `flt` payload `00 00 20 41`, exactly `10.0`. Decoding was correct; admitting the value was not. (Narrowed on 27 Sep to reject `10.0` only on `Ta0*` keys, so a genuine 10 °C elsewhere is kept.) `registry.c` files `Ta0*` under the ambient class, and `safety.c` honours a finite reading while skipping a non-finite one, so `panic_ambient_c = 70` became a guard permanently 60 °C clear of its own threshold. Now rejected the same way exactly `0.0` already was.

3. **Panic thresholds that guard nothing are now reported** (`src/daemon/control_loop.c`). One `safety.coverage ... reason=threshold_guards_nothing` warning per class with a finite `panic_c` and no enumerated sensors, emitted after the first *successful* enumeration (not at init, when `d->sensors` is still empty and every class would look unguarded). Catches both the dead ambient class and the long-known dead GPU class.

4. **The atexit handler defeated the failed-release latch** (`src/daemon/fanprod.c`). `main` deliberately leaves the unclean marker when its own release fails, so the next start latches into monitor-only; the registered `atexit` handler then called `mark_clean_exit` unconditionally and, running after `main`, always won. The "we could not hand the fans back" latch could never fire. Now gated on a flag `main` sets.

5. **`ProcessType` Background to Interactive** (`launchd/pro.fanpro.daemon.plist`). Not the proven cause of the stalls, but a thermal controller should not be on a throttled I/O and PRI 4 scheduling class while writing a history row every recorded tick. `Interactive` rather than `Adaptive`: Adaptive promotion is driven by XPC activity and this server listens on a unix socket.

6. **The false safety claim, narrowed** (`README.md:28`, `README.md:33`, `README.md:249`, `docs/engineering-log.md`, `etc/fanpro.conf.example`). See the correction section in the engineering log for the numbers. `panic_gpu_c` and `panic_ambient_c` are now commented out of the example config rather than shipped looking live.

Tests: `tests/test_daemon.c` gains three `effective_mode` cases (the key one asserts the pending mode with **zero** intervening ticks, so it fails against the old code) plus an unguarded-class case; `tests/test_sensors.c` gains a placeholder-rejection case and pins `Ta0*` to the ambient class. Suite: 3760 checks, 0 failures, up from 3733. The placeholder test was confirmed to fail when the rejection is disabled.

## Deferred, needing a decision

- **Release bookkeeping.** `fanpro_unlock_release_all` clears `manual[]` before the writes and zeros `manual_count` unconditionally, even on failure; the caller ignores the result and bumps `release_generation`, so the power thread can report a confirmed release while the hardware is still pinned. Highest-risk item found; not touched because it sits on the single-owner path and deserves its own change.
- **Heartbeat latch policy.** The two reviews disagree. One argues a scheduling stall should not latch (a stall is recoverable, and the latch disables control for the rest of the run); the other argues latching should stay until recovery is explicit, since a stale low manual target has no firmware rescue. Both note the deeper problem: the flag is only honoured by a loop that is *merely slow*, so a genuinely wedged loop never reads it and `KeepAlive` does not restart a hung process. A real fix escalates in the watchdog (`abort()` past 3x timeout, letting launchd restart and startup recovery free the fans).
- **Tick-phase instrumentation.** Without per-phase timings logged when a tick exceeds 2x `tick_ms`, the next stall cluster will be as undiagnosable as this one.
- **A live firmware reference for the curve floor**, per the engineering-log correction.
- `set all auto` and `daemon release` can reacquire in the same tick; sleep release discards `manual_override` with nothing logged; monitor-only still releases a fan another controller owns; panic hysteresis clears on SoC regardless of which class tripped; `fanpro status` labels every latch "previous run exited uncleanly".

## Deployment note

Not deployed. The installed `/usr/local/sbin/fanprod` is from 15 Aug 19:12 and `HEAD` is four commits ahead of it, including the IOReport subscription leak fix merged 16 Aug. Installing this branch would ship those commits too, so deploying is a separate decision.
