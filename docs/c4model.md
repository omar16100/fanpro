# fanpro architecture (C4)

Source of truth for fanpro's structure. Read before any architecture change; update on every change to containers, components, services, dependencies, or data flows.

## Context

fanpro is a local macOS tool for Apple Silicon Macs that reports thermal and power state and, where the firmware permits, drives the fans. It is a command line tool, a terminal dashboard and a daemon; there is no GUI.

Actors and external systems:

- **Operator** (admin user): runs `fanpro` in a terminal.
- **AppleSMC firmware**: owns fan state. Enforces per-key write permission against uid.
- **thermalmonitord**: Apple's thermal daemon. Holds fans in mode 3 on some generations and reclaims control unless suppressed.
- **launchd**: supervises `fanprod`.

## Containers

| Container | Runs as | Responsibility |
|---|---|---|
| `fanpro` | invoking user | CLI and ncurses TUI. Reads sensors directly (unprivileged). Sends all mutations to `fanprod` |
| `fanprod` | root, under launchd | Sole owner of SMC writes. Control loop, curves, safety gate, watchdogs, history, alerts, IPC server |
| `libfanpro.a` | linked into both | Shared core: SMC transport and codecs, sensor providers, curve and safety logic, config, IPC protocol |

Boundary rule: **no SMC write occurs outside `fanprod`.** `fanpro` never opens a write path.

## Components

### libfanpro

| Component | Files | Responsibility |
|---|---|---|
| smc transport | `src/smc/smc.c` | 80-byte struct round trip, key info, read, write, enumeration |
| smc codec | `src/smc/smc_codec.c` | fourcc pack/unpack, type-directed encode/decode (flt, fpe2, sp78, ui*, si*) |
| smc backends | `src/smc/smc_backend_iokit.c`, `src/smc/smc_fake.c` | Real IOKit user client; in-memory fake with generation personalities |
| sensor registry | `src/sensors/registry.c` | Merges providers, classifies sensors by class, disambiguates duplicate names, reports overflow rather than truncating |
| sensor providers | `src/sensors/hid_temp.c`, `smc_temp.c`, `ioreport_power.c`, `nvme_smart.c`, `thermal_pressure.c` | One provider per data source, each degrading independently |
| power subscription | `src/sensors/ioreport_power.c` | Owns one process-lifetime IOReport subscription plus the state machine around it: subscribe once, replace a held subscription that fails to sample (the expected consequence of sleep), never rebuild a subscription that was just created, and widen a skip window on repeated failure. `IOReportCreateSubscription` leaks 235.7 KB per call on macOS 26.3, so the subscription count is a correctness property, not an optimisation. Sampled only from the control-loop thread; `fanpro_power_invalidate` is called from that thread on wake. A `fanpro_power_ops_t` seam lets the state machine be tested without IOReport |
| fan model | `src/fan/fan.c` | Fan enumeration and per-fan state from SMC keys |
| unlock | `src/fan/unlock.c` | Manual-control state machine and `Ftst` refcount |
| curve | `src/fan/curve.c` | Pure temperature-to-RPM evaluation, hysteresis, asymmetric slew |
| safety | `src/fan/safety.c` | The single gate every target passes through before it reaches the SMC |
| config | `src/common/config.c` | INI parse of `/etc/fanpro/fanpro.conf` |
| proto | `src/common/proto.c` | Fixed binary IPC request/response framing and validation |
| peerauth | `src/common/peerauth.c` | `xucred` peer credentials and the uid/gid decision |
| log | `src/common/log.c` | Leveled key=value logging with rotation |

### fanprod

| Component | Files | Responsibility |
|---|---|---|
| control loop | `src/daemon/control_loop.c` | 1 Hz: sample, detect drift, evaluate, gate, write, record |
| heartbeat | `src/daemon/heartbeat.c` | Independent thread; watches an atomic tick counter and raises a release-requested flag. Never touches the SMC itself |
| ipc server | `src/daemon/ipc_server.c` | Unix socket, `LOCAL_PEERCRED` peer auth, refuses a manual speed while in monitor-only mode. The mode it gates on is the effective mode (`fanpro_daemon_effective_mode()` in `fanprod.c`): the newest queued `set-mode`, else `cfg.mode`, so `fanpro mode curve && fanpro set ...` is not refused while the mode change waits for the next tick |
| power notify | `src/daemon/power_notify.c` | `IORegisterForSystemPower`: release on sleep and power-off, re-probe on wake |
| history | `src/daemon/history.c` | JSONL sample log, daily rotation, retention pruning |
| alerts | `src/daemon/alerts.c` | Threshold rules with hysteresis; notifications via `posix_spawn`, never a shell |

## Data flows

### Read path (no daemon required)

```
fanpro sensors
  -> registry
       -> hid_temp     -> IOHIDEventSystemClient  -> IOKit    (primary)
       -> smc_temp     -> smc transport           -> AppleSMC (fallback only)
       -> ioreport     -> dlopen IOReport         -> IOKit
       -> nvme_smart   -> IONVMeSMARTUserClient   -> IOKit
  -> render
```

The SMC `T*` provider is deliberately **not** a peer of the HID provider. It runs only when HID returns nothing, or when the operator passes `--all` for inspection. Measured on this Mac Studio: HID yields 77 well-named sensors that agree with each other, while the SMC `T*` space yields ~600 opaque FourCC keys including one reading 96 C when every die sensor reads 49 C. Those readings are not validated and must not reach a fan curve.

### Write path (daemon only)

```
fanpro set fan0 1500
  -> proto request over /var/run/fanpro.sock  (root:admin 0660)
  -> ipc_server: LOCAL_PEERCRED check (uid 0 or gid 80)
  -> control_loop: target proposal
  -> safety gate: clamp, floor, panic check
  -> unlock: direct write, else Ftst sequence; refcount
  -> smc transport -> AppleSMC (write)
  -> verify by reading F%dAc
```

### Release path (safety-critical, several triggers)

```
SIGTERM | SIGINT | atexit | heartbeat timeout | sleep | power-off
  | sensor watchdog | panic threshold | drift detected
  -> unlock: every fan mode -> 0
  -> if manual refcount == 0: Ftst -> 0   (retried with backoff)
  -> thermalmonitord reclaims, mode returns to 3
```

## Dependencies

Only system frameworks: `IOKit`, `CoreFoundation`, `ncurses`, and a runtime `dlopen` of the private `IOReport` framework. No third-party libraries, no package manager.

### Daemon startup order

Fixed and load-bearing, because on this hardware nothing else will free a pinned fan:

1. Open the SMC, enumerate fans.
2. `fanpro_unlock_recover()` **before anything else touches a fan**: force back to auto whatever an unclean previous run left pinned.
3. Engage the **crash-loop latch** if `/var/run/fanpro.state` shows the last run did not exit cleanly. Latched means monitor-only until an operator runs `fanpro daemon enable`. Without it, launchd's `KeepAlive` would have a crashing daemon re-pin the fans on every restart.
4. Load config, start the auxiliary threads, run the loop.

`/var/run` is cleared on reboot, so a reboot reads as a clean start rather than an unclean exit, which is what we want.

When `main`'s own release fails it leaves the unclean marker in place, and the `atexit` handler honours that: it skips the clean-exit mark when `main` flagged a failed release or its own release fails, so the next start latches.

## Concurrency

**The control loop thread is the sole owner of the SMC handle, the fan set, and the unlock state.** Nothing else calls into `smc`, `fan`, or `unlock`.

The heartbeat watchdog runs on its own thread but only reads an atomic tick counter and sets an atomic release-requested flag; the control loop notices the flag and performs the release. The IPC server accepts and parses requests on its own thread, then hands validated commands to the control loop through a queue. The IPC thread may also read that queue, under `cmd_lock`, to learn the effective mode; it never writes `cfg`, which stays owned by the control loop.

This is chosen over locking the unlock state. A watchdog calling release while the loop sits between asserting `Ftst` and incrementing the manual refcount would clear `Ftst` from under a fan about to be marked held, which is the exact failure the refcount exists to prevent. Single ownership removes the race rather than guarding it.

The residual risk is a control loop wedged inside a blocking SMC call, which no in-process watchdog can rescue. That is covered from outside: `launchd` `KeepAlive` restarts the process, and startup recovery releases every fan before anything else runs.

## Constraints

- Every source file stays well under 2000 lines.
- Private API access is probed at runtime; a subsystem that disappears degrades to unavailable rather than failing the process.
- Fan writes are never attempted from an unprivileged process.
