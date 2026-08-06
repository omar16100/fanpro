/*
 * fanpro - unix socket peer authentication.
 *
 * LOCAL_PEERCRED gives the credentials the peer had when it called connect(),
 * captured by the kernel.  They cannot be forged by the peer, which is what
 * makes this worth doing on top of socket permissions.
 *
 * Two details that are easy to get wrong and are checked explicitly:
 *   - cr_version must match XUCRED_VERSION, or the struct we just read is not
 *     laid out the way this build expects.
 *   - cr_ngroups is bounded by NGROUPS, and must be clamped before it is used
 *     as a loop bound.
 */
#include "fanpro/peerauth.h"

#include "fanpro/log.h"
#include "fanpro/proto.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ucred.h>
#include <sys/un.h>
#include <unistd.h>

int
fanpro_peer_read(int fd, fanpro_peer_t *out)
{
	struct xucred cred;
	socklen_t len = sizeof(cred);
	int i, n;

	if (out == NULL)
		return -1;

	memset(out, 0, sizeof(*out));

	if (fd < 0)
		return -1;

	memset(&cred, 0, sizeof(cred));
	if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &len) != 0) {
		FANPRO_WARN("ipc.peer", "reason=getsockopt_failed err=%s",
		            strerror(errno));
		return -1;
	}
	if (len < sizeof(cred) || cred.cr_version != XUCRED_VERSION) {
		/* The struct is not the shape this build expects, so none of
		 * its fields can be trusted. */
		FANPRO_WARN("ipc.peer", "reason=xucred_version got=%u want=%u",
		            cred.cr_version, XUCRED_VERSION);
		return -1;
	}

	n = (int)cred.cr_ngroups;
	if (n < 0)
		n = 0;
	if (n > FANPRO_PEER_MAX_GROUPS)
		n = FANPRO_PEER_MAX_GROUPS;

	out->uid = (uint32_t)cred.cr_uid;
	out->n_groups = n;
	for (i = 0; i < n; i++)
		out->groups[i] = (uint32_t)cred.cr_groups[i];
	out->valid = true;
	return 0;
}

static bool
in_group(const fanpro_peer_t *peer, uint32_t gid)
{
	int i;

	for (i = 0; i < peer->n_groups; i++) {
		if (peer->groups[i] == gid)
			return true;
	}
	return false;
}

bool
fanpro_peer_may(const fanpro_peer_t *peer, uint32_t verb, const char **why)
{
	const char *ignored = NULL;

	if (why == NULL)
		why = &ignored;

	/* If we could not establish who is asking, we do not answer at all,
	 * not even a read.  "Unknown" is not a lesser privilege level. */
	if (peer == NULL || !peer->valid) {
		*why = "peer credentials unavailable";
		return false;
	}

	if (!fanpro_verb_is_mutating(verb)) {
		*why = "read-only";
		return true;
	}

	if (peer->uid == 0) {
		*why = "root";
		return true;
	}
	if (in_group(peer, FANPRO_ADMIN_GID)) {
		*why = "admin group";
		return true;
	}

	*why = "not root and not in the admin group";
	return false;
}
