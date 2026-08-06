# Contributing to fanpro

fanpro drives fans on hardware where, as measured, **the firmware will not step in if fanpro gets it wrong**. That single fact sets the tone for everything below: this is a small C project with an unusually low tolerance for "probably fine".

## Building and testing

```bash
make            # fanpro + fanprod
make test       # unit tests: no hardware, no root, well under a second
make check-live # read-only sanity checks against real hardware
```

`make test` must pass before anything else is worth discussing. It needs no Mac in particular, so CI runs it on a plain macOS runner.

## What is most useful

**Reports from other Macs.** The project is developed on a Mac Studio M3 Ultra (`Mac15,14`), and several behaviours are known to vary by machine and macOS version: the fan mode key spelling, whether an `Ftst` unlock is needed, and whether the firmware overrides a fan held in manual mode. If you run fanpro anywhere else, please open an issue with:

```bash
sysctl -n hw.model
sw_vers
sudo fanpro smc probe
```

If you are feeling brave and your machine is not doing anything important, `sudo fanpro smc hold --seconds 300` answers the thermal-authority question for your hardware. It deliberately heats the machine and aborts on its own at 95 °C, on heavy thermal pressure, or on any signal.

**Untested paths.** Several are listed in [`docs/engineering-log.md`](docs/engineering-log.md): the `Ftst` unlock, Intel `fpe2` encoding, NVMe SMART decoding. Anyone with the hardware to exercise these would be doing the project a real favour.

## House rules

**Anything that touches the real system must be injectable.** The SMC backend, the clock and the sensor sampler are all function pointers with an in-memory fake behind them. This is not architectural taste: twice during development, tests silently read the real machine's sensors and the real daemon's state file, passed for the wrong reason, and stopped testing anything. If you add a system dependency, add a seam with it.

**Tests assert behaviour, not implementation.** A test that restates what the code does will pass a rewrite that breaks the contract. Prefer "the fan must end up in auto" over "this function was called".

**Comments explain why.** Especially where a plausible-looking simplification would be unsafe. Most of the comments in `src/fan/` exist because someone (usually me) got it wrong first, and the comment is there so the next person does not undo the fix.

**Safety changes need evidence.** If you change the safety gate, the unlock state machine or any release path, say what you measured. `data/` holds raw traces for exactly this reason.

## Style

- C11, kernel-ish. Tabs for indentation, `snake_case`, `fanpro_` prefix on public symbols.
- Files stay well under 2000 lines. If one is growing past that, it is doing too much.
- No external dependencies. macOS system frameworks only; private API reached by `dlopen` or an `extern` declaration in `include/fanpro/private_apis.h`.
- Builds clean under `-Wall -Wextra -Wshadow -Wmissing-prototypes`. Warnings are errors in practice.

## Pull requests

Small and focused beats large and sweeping. Include:

- What you changed and why
- Which Mac and macOS version you tested on, and whether you exercised fan control or only monitoring
- `make test` output

If a change affects the architecture, update [`docs/c4model.md`](docs/c4model.md) in the same PR. If it produces a hardware finding, add it to [`docs/engineering-log.md`](docs/engineering-log.md) — that document is the project's memory.

## Reporting a security issue

The daemon runs as root and accepts input from any admin-group process on the machine. If you find something exploitable there, please open a GitHub security advisory rather than a public issue.
