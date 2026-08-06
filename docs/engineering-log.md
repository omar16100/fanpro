# Engineering log

Why fanpro is built the way it is, and what had to be measured to find out. Everything here was observed on a **Mac Studio M3 Ultra (`Mac15,14`), macOS 26.3 (build 25D125)**, on 2026-08-04 and 2026-08-06.

Read this if you are wondering why the defaults are so cautious, or why a fan-control tool ships with a hard safety gate and three separate watchdogs.

## The finding that shapes everything

**On this hardware, the firmware does not override a fan held in manual mode.** Not at any temperature.

Measured with `fanpro smc hold`, from a cold start, 32 workers drawing ~144 W, 420 seconds. Fan 0 was commanded to its minimum of 1000 RPM; fan 1 was left on auto as a control.

| | Fan 0 (held at 1000) | Fan 1 (auto, the control) |
|---|---|---|
| Start | 1162 RPM | 1167 RPM |
| Peak after settling | **1021 RPM** | **2519 RPM** |
| Samples above commanded + 150 | **0 of 390** | n/a |

SoC went 49.6 → 72.1 °C and thermal pressure climbed nominal → light → moderate. The firmware more than doubled the fan it still controlled, so it plainly wanted airflow. It never touched the fan fanpro held.

Raw trace: [`data/06082026_thermal_authority.log`](../data/06082026_thermal_authority.log).

The consequence is the whole reason the safety layer exists: **while fanpro holds a fan, fanpro is the only thermal protection that fan has.** Taking manual control is not "the fan does what I ask", it is "this fan leaves the system's thermal management". Hence monitor-only defaults, a curve derived from the firmware's own behaviour rather than invented, and a bias toward releasing whenever confidence is lost.

### Getting that measurement right took three attempts

Recorded because each failure is a trap worth avoiding:

1. **The load was too weak.** A `volatile double x; x += 1.0;` loop draws 47 W on this machine and moved the SoC 7 °C in three minutes. Independent multiply-add chains over a 512 KB buffer draw **144 W**. Measured with `fanpro power`, which incidentally cross-checked the IOReport work.
2. **Two false "the firmware boosts it" verdicts** came from counting the fan *spinning down* from its previous state as a firmware override. Judging now begins only once the fan has settled at the commanded speed, and requires a sustained excursion.
3. **Absolute temperature is the wrong instrument.** A run starting on an already-warm machine with fans at 2500 RPM stays flat while the firmware works hard. The control fan's ramp *is* the firmware's statement of intent, and is what the tool now judges on.

`fanpro smc hold` refuses to return a verdict when the data cannot support one, and did so twice before producing the result above.

## The firmware's own fan curve, measured

The control fan's response during that run is macOS's curve. The shipped `safe` curve sits at or above it at every point, so enabling fanpro never makes the machine hotter than leaving it alone would.

| SoC | Firmware RPM |
|---|---|
| 55 °C | ~1130 |
| 60 °C | ~1160 |
| 64 °C | ~1374 |
| 67 °C | ~1782 |
| 70 °C | ~2298 |
| 71 °C+ | ~2510 |

It also ramps down far more slowly than up: after the load stopped, the fan took roughly 16 minutes to fall from 2500 back to 1180 RPM.

## Hardware discoveries

**The mode key is lowercase and there is no `Ftst`.** Public research described `F%dMd` plus an `Ftst` diagnostic unlock for M3/M4, with lowercase `F%dmd` and no `Ftst` as an M5 trait. This M3 Ultra on macOS 26.3 shows the M5 profile: `F%dmd`, no `Ftst`, and fans idling in mode 0 rather than mode 3. Since both lowercase machines are on macOS 26.x, this is more likely a macOS change than a silicon one. fanpro probes both spellings and treats a missing `Ftst` as "no unlock needed", so it does not depend on the answer.

**IOReport has moved.** On macOS 26.3 there is no `IOReport.framework` bundle on disk; the code lives at `/usr/lib/libIOReport.dylib`. This is the concrete payoff of `dlopen` over linking.

Three IOReport details that each cost real debugging time:

1. `IOReportCreateSubscription` **consumes** the channels dictionary. Releasing it afterwards is a double-release and segfaults.
2. Subscribing to all channels segfaults here: the machine reports **11394** of them. Subscribe to a single group instead.
3. `IOReportSimpleGetIntegerValue`'s second parameter is an **out pointer**, not a flag. Passing an integer literal segfaults. Pass `NULL`.

**Energy channels do not share a unit.** The `Energy Model` group carries mJ, µJ and nJ channels side by side. Assuming millijoules reported the GPU at 60368 W instead of 0.06 W. The group is also hierarchical (per-core → cluster → die total, 645 channels), so summing everything multiple-counts the same silicon; only the `" Energy"` aggregates are reported.

**Thermal pressure uses the compact scale.** `notify_get_state` on `com.apple.system.thermalpressurelevel` returns **0-5** on macOS 26.3, not the legacy 0/10/20/30/40. Assuming the legacy scale made a raw 2 ("moderate") decode as "unknown", which silently removed this input from the safety gate exactly when the machine was heating. Both scales are now normalised.

**NVMe SMART is unavailable.** The internal controller is `AppleANS3CGv2Controller` and `IOCreatePlugInInterfaceForService` returns `kIOReturnUnsupported`. Root does not help. `fanpro disk` says so and falls back to the HID NAND sensor, which is the part fan control actually needs.

**The SMC `T*` space is not trustworthy here.** It offers ~600 opaque keys, one reading 96 °C at idle while every HID die sensor reads 49 °C. It is therefore a fallback for the case where HID enumeration breaks, never a fan-curve input.

## Bugs worth recording

Every change set was reviewed adversarially before being trusted. These are the ones that mattered, all fixed and pinned by regression tests.

**Fan-pinning holes.** On hardware with no `Ftst`, nothing reclaims a fan fanpro forgets about, so these are thermal risks rather than tidiness issues:

- Acquire could leave a fan pinned with clean bookkeeping when the mode write landed but the verifying read failed.
- `release_all` skipped fans whose mode key had just failed to read, because it gated on a value that a refresh recomputed each tick. Key existence is now latched at enumeration and never recomputed.
- Releasing a single fan did not clear its override, so the next tick re-acquired it: `set 0 auto` reported success and the fan was manual again a second later.
- Unbinding a curve pinned the fan at whatever RPM that curve last produced, forever, because the curve evaluator primes its own state and that was conflated with "the user set an override".
- Shutdown released the fans *after* joining threads, and the IPC join could block forever on a client that connected and never sent. Release now happens first.
- A failed wake re-enumeration left `count == 0` while fans were still held: the drive loop, the drift check and even `release_all` would all iterate zero fans and do nothing, silently, forever.

**Safety gate holes.** The gate returned OK for a `NULL` sensor set, so fans could be driven with no data behind them. Panic released the fan immediately, making the mode flap at 1 Hz with the firmware free to slow it in every gap; it now holds at maximum until the temperature is comfortably clear.

**Every sleep permanently latched the daemon.** Sleep raised the same flag as a heartbeat stall, so after any sleep/wake cycle the daemon sat in monitor-only until someone intervened. Release and latch are now separate decisions.

**A root shell injection.** Notifications were delivered by building a shell command with `system()` as root. The sanitiser stripped `"` `\` `$` `` ` `` — but the text landed inside a *single-quoted* shell string, where `'` is the only character that matters, and `'` was the one thing not stripped. The blocklist was exactly inverted. Replaced with `posix_spawn` and an explicit argv, so there is no shell and no quoting rules to get wrong.

In fairness to the old code, exploitability was low: the only config string reaching the command must be a substring of a real sensor name to fire, and sensor names contain no shell metacharacters. That was luck, not design.

**A buffer overflow in history formatting.** Four of five numeric fields checked only for a negative return. Since `snprintf` returns the length it *would* have written, truncation ran the offset past the buffer and the next write computed `out_len - off` as a huge `size_t`. Now covered by a test that walks every buffer size from 1 to 512 with guard bytes.

**The drift latch was unreachable.** Found by hardware testing, not by unit tests. A drift tick releases the fan, so the next tick sees no drift by construction and reset the strike counter; it could never exceed 1. Strikes now decay over a run of clean ticks.

**Drift detection watched modes but not targets.** Another tool rewriting `F%dTg` while fanpro held the mode key persisted indefinitely, because the deadband compared against fanpro's own stale record and skipped the corrective write.

## Testing notes

The control plane is testable without hardware because the SMC backend, the clock and the sensor sampler are all injectable. The in-memory fake models the firmware behaviours that actually bite: typed keys, absent keys answering `0x84`, mode-3 rejection with `0x82`, the `Ftst` yield delay on a virtual clock, and `Ftst` being cleared taking every manual fan with it.

Two test-isolation bugs are worth repeating because they were the same mistake twice:

- The daemon tests read this machine's **real 77 HID sensors** through IOKit, so they passed for the wrong reason and could never exercise "the sensors went away".
- Once a real `fanprod` was installed, `control_loop_init` read the live `/var/run/fanpro.state`, so every daemon test latched itself into monitor-only and failed.

Anything that touches the real system has to be injectable, or the tests quietly stop testing.

## Untested paths

Stated plainly rather than implied:

- **The `Ftst` unlock path.** This machine has no such key, so that code cannot execute here. Fake-backend tests only.
- **Intel `fpe2` fan encoding.** No Intel Mac available.
- **NVMe SMART log-page decoding.** Unreachable while the controller answers `kIOReturnUnsupported`.
- **The SMC `T*` fallback provider**, as an actual fallback: HID has never failed here.
- **Heartbeat stall on real hardware.** `kill -STOP` suspends the watchdog thread too, so it cannot observe the stall it exists to catch. Unit-tested only; verifying it for real would need a debug-only hook to wedge just the control loop.
- **Reboot persistence and sleep/wake re-probe.** Not yet run.

## End-to-end verification

Run 2026-08-06 against the installed daemon on real hardware.

| Check | Result |
|---|---|
| Installs, starts under launchd, answers over IPC | pass |
| `set 0 1900` converges (measured 1900 RPM) | pass |
| Two fans manual, release one, other keeps its target | pass |
| `SIGTERM` returns both fans to firmware control | pass |
| `kill -9` → launchd restart → fans recovered → crash-loop latch engaged | pass |
| `daemon enable` clears the latch and control resumes | pass |
| Rival SMC client steals a fan → fanpro stands down | pass |
| Repeated theft → latches out of the fight | pass |
| Rival rewrites the target → detected and corrected | pass |
| Persistent target theft → latches | pass |
| Panic threshold drives the fan to maximum and holds | pass |
