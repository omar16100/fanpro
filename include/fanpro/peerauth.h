/*
 * fanpro - unix socket peer authentication.
 *
 * The daemon runs as root.  Socket permissions (root:admin 0660) are the
 * first gate, but they are a property of the filesystem and can be changed
 * by anyone who can already write to /var/run, so the daemon checks the
 * peer's credentials itself as well.
 *
 * The decision function is pure so the whole policy is unit testable without
 * a socket, a daemon, or several user accounts.
 */
#ifndef FANPRO_PEERAUTH_H
#define FANPRO_PEERAUTH_H

#include <stdbool.h>
#include <stdint.h>

/* gid of the `admin` group on macOS. */
#define FANPRO_ADMIN_GID 80

#define FANPRO_PEER_MAX_GROUPS 16

/* What we learned about the peer; filled from struct xucred. */
typedef struct {
	bool     valid;      /* credentials were readable and well-formed */
	uint32_t uid;
	int      n_groups;
	uint32_t groups[FANPRO_PEER_MAX_GROUPS];
} fanpro_peer_t;

/*
 * Read the peer credentials of a connected unix socket.
 * Returns 0 on success; on failure `out->valid` is false, which every caller
 * must treat as "deny".
 */
int fanpro_peer_read(int fd, fanpro_peer_t *out);

/*
 * May this peer issue this verb?
 *
 * Read-only verbs are allowed for any peer that got this far. Mutating verbs
 * require uid 0 or membership of the admin group. Invalid credentials are
 * always denied, including for reads: if we could not determine who is
 * asking, we do not answer.
 */
bool fanpro_peer_may(const fanpro_peer_t *peer, uint32_t verb,
                     const char **why);

#endif /* FANPRO_PEERAUTH_H */
