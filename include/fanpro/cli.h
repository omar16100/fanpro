/*
 * fanpro - CLI subcommands implemented outside main.c.
 */
#ifndef FANPRO_CLI_H
#define FANPRO_CLI_H

#include <stddef.h>

#include "fanpro/proto.h"
#include "fanpro/smc.h"

/*
 * `fanpro smc probe`: work out how manual fan control behaves on this
 * machine, prove it by driving a fan, then release it.  Requires root to do
 * anything beyond the survey.  Returns 0 only when a fan demonstrably
 * reached a commanded speed.
 */
int fanpro_cmd_probe(fanpro_smc_t *smc, int argc, char **argv);

/*
 * `fanpro smc hold`: the thermal authority experiment.
 *
 * Pins one fan at a chosen RPM, optionally applies an all-core load, and
 * samples fan RPM and temperatures once a second.  The question it answers is
 * whether this firmware still boosts a fan that we hold in manual mode.  TG
 * Pro asserts it does not, and refuses to offer manual RPM on Apple Silicon
 * for that reason; if they are right, fanpro's safety layer is the only
 * thermal protection while a fan is held, which decides the shipped defaults.
 *
 * Deliberately makes the machine hot, so it aborts in code rather than
 * waiting for a human: on the temperature limit, on thermal pressure, or on
 * any signal.  Requires root.
 */
int fanpro_cmd_hold(fanpro_smc_t *smc, int argc, char **argv);

/*
 * Send one request to fanprod and read the reply.  Returns 0 only when the
 * daemon reports success; otherwise `err` holds a message already suitable
 * for showing the user, including how to fix the common cases (daemon not
 * running, not in the admin group, version mismatch).
 */
int fanpro_ipc_call(const fanpro_request_t *req, fanpro_response_t *resp,
                    char *err, size_t err_len);

/* Daemon-facing subcommands: set, curve, mode, status, daemon, log. */
int fanpro_cmd_daemon(int argc, char **argv);
int fanpro_cmd_set(int argc, char **argv);
int fanpro_cmd_mode(int argc, char **argv);
int fanpro_cmd_status(int argc, char **argv);

/* Read the daemon log and the recorded history. These read files directly and
 * do not need the daemon alive, which is when you most want them. */
int fanpro_cmd_log(int argc, char **argv);
int fanpro_cmd_history(int argc, char **argv);

/*
 * `fanpro top`: the live dashboard. Works with or without the daemon; without
 * it, everything is read-only. Mutating hotkeys go through the same IPC path
 * as the CLI, so the TUI has no special privileges.
 */
int fanpro_cmd_top(fanpro_smc_t *smc, int argc, char **argv);

#endif /* FANPRO_CLI_H */
