# Maintenance sweep, 27 Sep 2026

Status: implemented in the PR that adds this file. Ships work that was sitting on a local branch, closes one ordering gap a review found in it, and tidies the README and remote branches.

## Scope

1. **Publish `fix/thermal-custody-findings-18092026`** (commit `ac1a9fa`). The 18 Sep fixes were committed locally and never pushed. They are described in [`18092026_thermal_custody_findings_plan.md`](18092026_thermal_custody_findings_plan.md): the mode/set race, the `Ta0*` placeholder ambient reading, dead panic guards, the atexit handler defeating the failed-release latch, and the narrowed "never hotter than leaving it alone" claim. Published through a PR so CI runs on it.
2. **README em dashes.** `README.md` had 20 em dashes across 18 lines. Each was replaced with a colon, comma, parentheses or a full stop. No claim or number was changed.
3. **Architecture doc.** `c4model.md` now records two behaviours the 18 Sep commit introduced: the IPC server gates a manual speed on the effective mode (newest queued `set-mode`, else `cfg.mode`), and the `atexit` handler leaves the unclean marker when a release failed.
4. **Review fixes to the 18 Sep change** (codex review, 27 Sep):
   - `fanpro_daemon_effective_mode()` ignored a queued `reload`, which replaces `cfg.mode` with the file's value. Starting in auto, `mode curve`, `reload` (file says auto), `set 3625` queued inside one tick was accepted and then landed in monitor-only. A reload newer than any queued `set-mode` now makes the gate use the applied mode, which is what it did before the fix. New test `effective_mode_falls_back_to_applied_mode_behind_a_queued_reload`, confirmed to fail without the change.
   - The coverage-warning test now captures the log and asserts exactly one `safety.coverage` line for the empty ambient class across two ticks, and none for `soc`. Confirmed to fail when the one-shot guard is removed.
   - The `10.0` placeholder rejection applied to every numeric `T*` key. It is now limited to the measured `Ta0*` keys, so a genuine 10 °C elsewhere is kept (dropping it could empty the class a curve reads and force a release). New test `smc_temp_keeps_a_genuine_ten_degree_reading_on_other_keys`.
   - Wording: the example config no longer implies that commenting out `panic_gpu_c` / `panic_ambient_c` disables them (built-in defaults 95 and 70 still apply); the plist comment no longer claims a log line per tick or that the watchdog itself releases the fans; the 18 Sep plan no longer claims a sub-millisecond residual window or that fanpro "could not have prevented" the emergency, and no longer lists local benchmark log paths.
5. **Stale remote branch.** `fix/ioreport-subscription-leak` (`b7bb03d`) was merged into `main` by `92bdd15`. Ancestry is checked with `git merge-base --is-ancestor` before deletion. Restore with `git push origin b7bb03d4cf853d70e8e87bc26ea98100b72f1919:refs/heads/fix/ioreport-subscription-leak`.

## Verification

- `make` builds with no warnings; `make test`: 3775 checks, 0 failures (3760 before the review fixes).
- Not run: `make check-live`, `make install`, `fanpro daemon install`, or anything else that writes SMC or fan state or touches launchd. The installed daemon is unchanged, so deployment is still a separate decision (see the 18 Sep plan's deployment note).
- Outgoing history scanned with gitleaks and a personal-data grep (paths, hostnames, phone and ID patterns) before push.

## Not changed

- No test yet covers the `atexit` marker handling, because `emergency_release` is static in `fanprod.c` and has no seam for a fake release.

- `CONTRIBUTING.md` and `docs/engineering-log.md` still contain one em dash each; out of scope for this pass.
- The deferred items in the 18 Sep plan (release bookkeeping, heartbeat latch policy, tick-phase instrumentation) are untouched.
