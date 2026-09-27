# Maintenance sweep, 27 Sep 2026

Status: in progress. Ships work that was sitting on a local branch and tidies the README and remote branches. No behaviour change beyond what the 18 Sep branch already contains.

## Scope

1. **Publish `fix/thermal-custody-findings-18092026`** (commit `ac1a9fa`). The 18 Sep fixes were committed locally and never pushed. They are described in [`18092026_thermal_custody_findings_plan.md`](18092026_thermal_custody_findings_plan.md): the mode/set race, the `Ta0*` placeholder ambient reading, dead panic guards, the atexit handler defeating the failed-release latch, and the narrowed "never hotter than leaving it alone" claim. Merged through a PR so CI runs on it.
2. **README em dashes.** `README.md` had 20 em dashes across 18 lines. Each was replaced with a colon, comma, parentheses or a full stop. No claim or number was changed.
3. **Architecture doc.** `c4model.md` now records two behaviours the 18 Sep commit introduced: the IPC server gates a manual speed on the effective mode (newest queued `set-mode`, else `cfg.mode`), and the `atexit` handler leaves the unclean marker when a release failed.
4. **Stale remote branch.** `fix/ioreport-subscription-leak` (`b7bb03d`) was merged into `main` by `92bdd15`. Ancestry is checked with `git merge-base --is-ancestor` before deletion. Restore with `git push origin b7bb03d4cf853d70e8e87bc26ea98100b72f1919:refs/heads/fix/ioreport-subscription-leak`.

## Verification

- `make` builds with no warnings; `make test`: 3760 checks, 0 failures.
- Not run: `make check-live`, `make install`, `fanpro daemon install`, or anything else that writes SMC or fan state or touches launchd. The installed daemon is unchanged, so deployment is still a separate decision (see the 18 Sep plan's deployment note).
- Outgoing history scanned with gitleaks and a personal-data grep (paths, hostnames, phone and ID patterns) before push.

## Not changed

- `CONTRIBUTING.md` and `docs/engineering-log.md` still contain one em dash each; out of scope for this pass.
- The deferred items in the 18 Sep plan (release bookkeeping, heartbeat latch policy, tick-phase instrumentation) are untouched.
