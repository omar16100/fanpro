/*
 * fanpro - CLI side of the IPC.
 *
 * Every mutation the CLI can perform goes through here.  The CLI never opens
 * a write path to the SMC itself, which is what makes it safe to run as an
 * ordinary user: the worst it can do is be refused by the daemon.
 *
 * A connect timeout matters because a wedged daemon must not wedge the CLI
 * too; a user who cannot get a prompt back is a user who reaches for kill -9.
 */
#include "fanpro/cli.h"

#include "fanpro/proto.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define CLIENT_TIMEOUT_S 5

int
fanpro_ipc_call(const fanpro_request_t *req, fanpro_response_t *resp,
                char *err, size_t err_len)
{
	struct sockaddr_un addr;
	struct timeval tv;
	int fd;

	if (req == NULL || resp == NULL || err == NULL)
		return -1;

	err[0] = '\0';

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		snprintf(err, err_len, "socket: %s", strerror(errno));
		return -1;
	}

	/* Never block indefinitely on a daemon that has stopped answering. */
	tv.tv_sec = CLIENT_TIMEOUT_S;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", FANPRO_SOCKET_PATH);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		if (errno == ENOENT)
			snprintf(err, err_len,
			         "fanprod is not running (no %s). start it with: "
			         "sudo fanpro daemon install",
			         FANPRO_SOCKET_PATH);
		else if (errno == EACCES || errno == EPERM)
			snprintf(err, err_len,
			         "permission denied on %s: you must be in the admin "
			         "group or run with sudo",
			         FANPRO_SOCKET_PATH);
		else
			snprintf(err, err_len, "connect: %s", strerror(errno));
		close(fd);
		return -1;
	}

	if (fanpro_proto_write_all(fd, req, sizeof(*req)) != 0) {
		snprintf(err, err_len, "sending request: %s", strerror(errno));
		close(fd);
		return -1;
	}
	if (fanpro_proto_read_all(fd, resp, sizeof(*resp)) != 0) {
		snprintf(err, err_len, "no reply from fanprod (timeout or hang up)");
		close(fd);
		return -1;
	}
	close(fd);

	if (resp->magic != FANPRO_PROTO_MAGIC ||
	    resp->version != FANPRO_PROTO_VERSION) {
		snprintf(err, err_len,
		         "reply from a different fanprod version; reinstall so the "
		         "CLI and daemon match");
		return -1;
	}

	if (resp->status != 0) {
		/* The daemon already explained itself; do not paraphrase it. */
		/* Bounded: this came off a socket and may not be terminated. */
		snprintf(err, err_len, "%.*s", (int)sizeof(resp->message),
		         resp->message[0] != '\0' ? resp->message : "request refused");
		return -1;
	}
	return 0;
}
