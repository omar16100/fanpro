# fanpro documentation

| Document | What it covers |
|---|---|
| [`../README.md`](../README.md) | Install, usage, configuration, safety design. Start here |
| [`c4model.md`](c4model.md) | Architecture: containers, components, data flows, concurrency rules. The source of truth for structural changes |
| [`smc_reference.md`](smc_reference.md) | AppleSMC wire protocol, key types, result codes, per-generation fan behaviour, and the private APIs fanpro uses |
| [`engineering-log.md`](engineering-log.md) | What was measured on real hardware, which bugs review caught and why they mattered, and what remains untested |
| [`15082026_ioreport_subscription_leak_plan.md`](15082026_ioreport_subscription_leak_plan.md) | The plan and measurements behind the IOReport subscription leak fix: 235.7 KB leaked per sample, 1.6 GB/day, and why the fix needed a state machine rather than a hoisted variable |
| [`18092026_thermal_custody_findings_plan.md`](18092026_thermal_custody_findings_plan.md) | The thermal-emergency incident of 18 Sep 2026 and what it exposed: the mode/set race, a firmware placeholder admitted as an ambient reading, panic thresholds guarding empty classes, an atexit handler defeating the failed-release latch, and the correction to the "never hotter than leaving it alone" claim |
| [`27092026_maintenance_sweep_plan.md`](27092026_maintenance_sweep_plan.md) | The 27 Sep 2026 maintenance pass: publishing the thermal custody branch, removing em dashes from the README, and deleting the merged `fix/ioreport-subscription-leak` remote branch |
| [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | Build, test, house rules, and what kinds of contribution help most |

Raw measurement traces live in [`../data/`](../data/).

## Conventions

- Evergreen documents are named `topic.md`. Anything dated uses `DDMMYYYY_topic.md`.
- Architecture changes update `c4model.md` in the same commit.
- Hardware findings go in `engineering-log.md` with the numbers, not a summary of them.
