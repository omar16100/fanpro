# AppleSMC reference

## Scope

The SMC wire protocol, key types, result codes, and the fan-control behaviour fanpro depends on. Facts verified on hardware are marked as such; everything else is cited.

## Facts

### Transport

- Service `AppleSMC`, user client connection type `0`, selector `2` via `IOConnectCallStructMethod`.
- Request and response are the same 80-byte struct: `key@0`, `key_info.data_size@28`, `result@40`, `data8@42` (command), `data32@44`, `bytes@48`.
- Commands: `1` key count, `5` read bytes, `6` write bytes, `8` key by index, `9` key info.
- Reads succeed for any user. Writes are gated by the firmware against uid and fail with `kIOReturnNotPrivileged` (`0xe00002c2`) below the firmware layer, so they surface as a transport failure rather than a result code.

Verified on Mac Studio M3 Ultra (`Mac15,14`), macOS 26.3: reads work unprivileged through exactly this path.

### Result codes

| Code | Name | Meaning |
|---|---|---|
| `0x00` | success | |
| `0x82` | SmcBadCommand | Firmware refused. Observed on mode writes while the fan is in mode 3 |
| `0x84` | SmcNotFound | Key absent on this machine. Information, not an error |
| `0x85` | SmcNotReadable | Write-only key |
| `0x86` | SmcNotWritable | Read-only key |
| `0x87` | SmcKeySizeMismatch | Size mismatch. On fan target keys the value is frequently applied anyway, so verify by reading `F%dAc` rather than treating this as failure |

### Value types

The type is a fourcc reported per key by the key-info command. It, not the platform, determines the encoding.

- `flt ` : IEEE-754 single, **little-endian**. Verified: `F0Mx` reads `00906245` = 3625.0.
- `fpXY` : unsigned fixed point, X integer and Y fraction bits summing to 16, big-endian. `fpe2` is 14.2, the Intel fan encoding.
- `spXY` : signed fixed point, X + Y = 15 plus a sign bit, big-endian. `sp78` is the classic Intel temperature encoding.
- `ui8 `, `ui16`, `ui32`, `si8 `, `si16` : big-endian integers.
- `flag` : single byte, 0 or 1.

fanpro derives the binary point from the type name rather than a lookup table, so an unseen `fpXY` variant still decodes correctly.

### Fan keys

| Key | Type | Meaning |
|---|---|---|
| `FNum` | ui8 | Fan count |
| `F%dAc` | flt / fpe2 | Actual RPM |
| `F%dMn` | flt / fpe2 | Minimum RPM |
| `F%dMx` | flt / fpe2 | Maximum RPM |
| `F%dTg` | flt / fpe2 | Target RPM |
| `F%dMd` or `F%dmd` | ui8 | Mode: 0 auto, 1 manual, 3 system |
| `Ftst` | ui8 | Diagnostic unlock. Present only on some firmware |

### Generation behaviour

Manual fan control is not uniform. Known profiles:

| Machine | Mode key | `Ftst` | Idle mode | Path to manual |
|---|---|---|---|---|
| M1 | `F%dMd` | present | 0 | Direct write |
| M4 (`Mac16,6`) | `F%dMd` | present | 3 | `Ftst` unlock required |
| M5 (`Mac17,7`), macOS 26.4 | `F%dmd` | absent | - | Direct write |
| **M3 Ultra (`Mac15,14`), macOS 26.3** | **`F%dmd`** | **absent** | **0** | **Direct write, confirmed working** |

The last row is measured on this machine, 2026-08-04, and was not in the public research, which listed M3 and Mac Studio as untested and treated lowercase `F%dmd` plus absent `Ftst` as an M5 trait.

Because the two lowercase machines are both on macOS 26.x while the uppercase-plus-`Ftst` machines were observed on earlier releases, the more likely explanation is that **this is a macOS 26 change rather than a silicon generation change**. fanpro does not depend on which explanation is right: it probes both spellings and treats an absent `Ftst` as "no unlock needed", so it works either way.

Measured on this Mac Studio: 2 fans, range 1000-3625 RPM, both idle at ~1000 RPM in mode 0.

Manual control confirmed end to end on 2026-08-04 with `sudo fanpro smc probe`:

```
write F0md = 1   -> 0x00, mode reads back manual
write F0Tg = 2050 -> 0x00
  0 ms  rpm=1007
500 ms  rpm=1037
1000 ms rpm=1797
1500 ms rpm=2108   converged
release: mode auto
```

No `Ftst` step was involved or possible. On this firmware the fan begins responding within about a second and reaches the commanded speed in roughly 1.5 s.

### The `Ftst` unlock, where it applies

1. Write `Ftst = 1`.
2. Poll the mode key until it drops from 3 to 0, roughly 3-4 s. Poll at 100 ms, give up at 10 s.
3. Write mode = 1.
4. Write the target RPM.

Release: set every fan's mode to 0, then clear `Ftst` **only once the last manual fan is released**. `Ftst` is global while mode is per-fan, so clearing it early lets thermalmonitord reclaim and silently kills any other fan's manual control.

The firmware is reported to clear `Ftst` on sleep, but the source hedges on this, so the wake path must re-probe rather than assume.

### Privileges

No code signature, entitlement, or TCC grant is required. The firmware enforces write permission against uid, not code identity, and SIP does not restrict this path for root. `SMJobBless` and `SMAppService` would impose a paid Developer ID for no benefit.

### Temperature sensors, measured on this machine

The HID path (`IOHIDEventSystemClient`, page `0xff00` usage `0x0005`) returns 77 sensors on this Mac Studio M3 Ultra. All six SPI symbols fanpro needs are exported from `IOKit.framework` on macOS 26.3, verified with `dyld_info -exports`.

Names repeat across dies: an M3 Ultra reports `PMU tdie1` four times with genuinely different values, so fanpro keeps every reading and suffixes repeats (`PMU tdie1#2`).

Idle readings, 2026-08-04: SoC die sensors 48-51 C, `NAND CH0 temp` 42 C, PMU device sensors 42-52 C.

The SMC `T*` key space returns roughly 600 more keys on the same machine, all typed `flt`, with opaque names (`Tp5y`, `Tf76`, `Ts5f`). Some are clearly plausible (`TB0p` 44.5 C), others are not comparable to the die sensors (`Tf76` reads 96.5 C at idle) and four `Ta0*` keys read exactly 10.000 C, which looks like a placeholder rather than a measurement. fanpro therefore treats this space as a fallback for the case where HID enumeration breaks, never as a fan-curve input.

### Power, thermal pressure, and disk, measured on this machine

**IOReport has moved.** On macOS 26.3 there is no `IOReport.framework` bundle on disk at all; the code lives at `/usr/lib/libIOReport.dylib`. fanpro tries that path first and the old `PrivateFrameworks` paths after, which is the concrete payoff of `dlopen` over linking.

Three things about IOReport that cost real debugging time and are pinned here so they are not rediscovered:

1. `IOReportCreateSubscription` **consumes** the channels dictionary. Releasing it afterwards is a double-release and segfaults.
2. Subscribing to all channels segfaults on this machine. It reports **11394** channels; subscribe to a single group instead. fanpro discovers the group by walking the channel list (safe) and looking for a name containing "Energy", then subscribes only to that.
3. `IOReportSimpleGetIntegerValue`'s second parameter is an **out pointer**, not a flag. Passing an integer literal segfaults. Pass `NULL`.

**Energy channels do not share a unit.** The `Energy Model` group on this machine carries `mJ`, `uJ` and `nJ` channels side by side. Assuming millijoules reported the GPU at 60368 W instead of 0.06 W. The `IOReportChannelGetUnitLabel` value is authoritative.

The group is also hierarchical: per-core (`DIE_0_EACC_CPU0`), then cluster (`DIE_0_EACC_CPU`), then die total (`DIE_0_CPU Energy`), all in the same list of 645 channels. Summing everything multiple-counts the same silicon. fanpro reports only the aggregates, whose names end in `" Energy"`, and folds `DIE_0`/`DIE_1` together. Idle result on this machine: CPU 0.2-0.5 W, GPU 0.06-0.11 W.

**Thermal pressure** reads from the notify key `com.apple.system.thermalpressurelevel` via `notify_register_check` / `notify_get_state`. Pure C, no Foundation link needed in the daemon.

**NVMe SMART is not available on this machine.** The internal controller is `AppleANS3CGv2Controller`, and `IOCreatePlugInInterfaceForService` with `kIONVMeSMARTUserClientTypeID` returns `kIOReturnUnsupported` (`0xe00002c7`). This is a property of Apple's internal ANS parts, not a privilege problem: root does not help. fanpro reports that plainly and falls back to the HID `NAND CH0 temp` sensor (42 C idle) for the temperature, which is the part fan control actually needs.

### Does the firmware still protect a fan held in manual mode? No.

Measured 2026-08-06 on this Mac Studio M3 Ultra with `fanpro smc hold`, from a cold start (both fans ~1160 RPM, SoC 49.6 C), 32 FMA workers drawing ~144 W, 420 s. Full trace in `data/06082026_thermal_authority.log`.

| | Fan 0 (held at 1000) | Fan 1 (auto, the control) |
|---|---|---|
| Start | 1162 RPM | 1167 RPM |
| Peak after settling | **1021 RPM** | **2519 RPM** |
| Samples above commanded + 150 | **0 of 390** | n/a |

SoC went 49.6 -> 72.1 C and thermal pressure climbed nominal -> light -> moderate. The firmware more than doubled the fan it controlled, so it plainly wanted airflow. It never touched the fan fanpro held.

**Once a fan is in mode 1, the firmware will not rescue it, at any temperature.** Manual control is therefore not simply "the fan does what you ask": it removes the fan from the system's own thermal management entirely.

While fanpro holds a fan, fanpro's safety layer is the only thermal protection that fan has. That is why the shipped defaults are monitor-only, why the default curve is derived from the firmware's own measured response rather than invented, and why every path that loses confidence releases the fan rather than carrying on.

Getting this measurement right took three attempts, and the failures are worth recording:

1. A `volatile double x; x += 1.0;` load draws only 47 W on this machine and moved SoC just 7 C in 3 minutes. Independent multiply-add chains over a 512 KB buffer draw 144 W. Verified with `fanpro power`.
2. The first two verdicts were false positives: the peak RPM included the fan spinning *down* from its previous state before reaching the commanded speed. Judging now begins only after the fan has settled, and requires a sustained excursion.
3. Absolute temperature is a poor test of whether the firmware wanted airflow: a run starting on an already-warm machine with fans at 2500 RPM stays flat while the firmware is working hard. **The control fan's ramp is the firmware's own statement of intent** and is the right instrument.

### The firmware's own fan curve, measured

The control fan's response is effectively the firmware's curve, and is what fanpro's shipped default is derived from so the default is never less aggressive than macOS would be:

| SoC | Firmware fan RPM |
|---|---|
| 55 C | ~1130 |
| 60 C | ~1160 |
| 64 C | ~1374 |
| 67 C | ~1782 |
| 70 C | ~2298 |
| 71+ C | ~2510 |

It ramps down far more slowly than up: after the load stopped, the fan took roughly 16 minutes to fall from 2500 back to 1180 RPM.

### Thermal pressure uses the compact scale on macOS 26

`notify_get_state` on `com.apple.system.thermalpressurelevel` returns **0-5** (nominal, light, moderate, heavy, trapping, sleeping) on macOS 26.3, not the legacy 0/10/20/30/40. Measured: 0 at idle, 1 under moderate load, 2 sustained. Assuming the legacy scale made a raw 2 decode as "unknown", which silently dropped this input from the safety gate exactly when the machine was heating. fanpro normalises both scales in `fanpro_thermal_normalise`.

## Sources

- [agoodkind/macos-smc-fan](https://github.com/agoodkind/macos-smc-fan) - unlock mechanism, struct layout, result codes, generation table
- [hholtmann/smcFanControl](https://github.com/hholtmann/smcFanControl/tree/master/smc-command) - original SMC protocol implementation
- [tw93/Mole #1119](https://github.com/tw93/Mole/issues/1119), [exelban/stats #2928](https://github.com/exelban/stats/issues/2928) - M3/M4 fan control failures
- Direct measurement on `Mac15,14` / macOS 26.3 build 25D125, 2026-08-04
