/*
 * fanpro - IPC server.
 *
 * Accepts on a unix socket, authenticates the peer, validates the request,
 * and either answers from the published snapshot (reads) or queues the
 * command for the control loop (writes).  It never touches the SMC itself.
 *
 * Socket creation is the fiddly part.  bind() applies the process umask, so
 * there is a window between the socket appearing and chmod tightening it.
 * The umask is set around bind and the mode fixed immediately after, before
 * listen() makes it reachable.
 */
#include "daemon.h"

#include "fanpro/log.h"
#include "fanpro/peerauth.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Long enough for any legitimate client, short enough that a stuck one
 * cannot hold the single accept loop. */
#define IPC_CLIENT_TIMEOUT_S 5

static pthread_t        g_thread;
static atomic_bool      g_running;
static int              g_listen_fd = -1;

static int
make_socket(void)
{
	struct sockaddr_un addr;
	mode_t old_umask;
	int fd;

	if (strlen(FANPRO_SOCKET_PATH) >= sizeof(addr.sun_path)) {
		FANPRO_ERROR("ipc.socket", "reason=path_too_long");
		return -1;
	}

	/* A stale socket from an unclean exit would make bind fail. */
	unlink(FANPRO_SOCKET_PATH);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		FANPRO_ERROR("ipc.socket", "reason=socket err=%s", strerror(errno));
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", FANPRO_SOCKET_PATH);

	/* Create it inaccessible, then widen deliberately.  The other order
	 * leaves a window in which anyone can connect. */
	old_umask = umask(0177);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		umask(old_umask);
		FANPRO_ERROR("ipc.socket", "reason=bind err=%s", strerror(errno));
		close(fd);
		return -1;
	}
	umask(old_umask);

	/* root:admin 0660.  Peer credentials are checked as well, since these
	 * permissions are only as trustworthy as /var/run. */
	if (chown(FANPRO_SOCKET_PATH, 0, FANPRO_ADMIN_GID) != 0)
		FANPRO_WARN("ipc.socket", "reason=chown err=%s", strerror(errno));
	if (chmod(FANPRO_SOCKET_PATH, 0660) != 0)
		FANPRO_WARN("ipc.socket", "reason=chmod err=%s", strerror(errno));

	if (listen(fd, 8) != 0) {
		FANPRO_ERROR("ipc.socket", "reason=listen err=%s", strerror(errno));
		close(fd);
		return -1;
	}

	FANPRO_INFO("ipc.socket", "path=%s mode=0660 group=%d",
	            FANPRO_SOCKET_PATH, FANPRO_ADMIN_GID);
	return fd;
}

static void
serve_one(fanpro_daemon_t *d, int fd)
{
	fanpro_request_t req;
	fanpro_response_t resp;
	fanpro_peer_t peer;
	struct timeval tv;
	const char *why = "ok";

	/*
	 * Bound every read and write on this connection.  Without it a client
	 * that connects and sends nothing parks this thread in read() forever:
	 * the accept loop is single-threaded, so one silent client would block
	 * every other request including `status`, and would also hang the
	 * shutdown join.
	 */
	tv.tv_sec = IPC_CLIENT_TIMEOUT_S;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	fanpro_response_init(&resp);

	if (fanpro_peer_read(fd, &peer) != 0)
		peer.valid = false;

	if (fanpro_proto_read_all(fd, &req, sizeof(req)) != 0) {
		FANPRO_DEBUG("ipc.request", "reason=short_read");
		return;
	}

	/* The published count, never d->fans: that set belongs to the control
	 * loop and is rewritten wholesale on wake. */
	if (fanpro_request_validate(&req, atomic_load(&d->pub_fan_count), &why) != 0) {
		FANPRO_WARN("ipc.request", "reason=invalid why=%s uid=%u", why,
		            peer.valid ? peer.uid : 0);
		resp.status = -1;
		snprintf(resp.message, sizeof(resp.message), "invalid request: %s",
		         why);
		fanpro_proto_write_all(fd, &resp, sizeof(resp));
		return;
	}

	if (!fanpro_peer_may(&peer, req.verb, &why)) {
		FANPRO_WARN("ipc.request", "reason=denied verb=%s uid=%u why=%s",
		            fanpro_verb_name(req.verb),
		            peer.valid ? peer.uid : 0, why);
		resp.status = -2;
		snprintf(resp.message, sizeof(resp.message), "permission denied: %s",
		         why);
		fanpro_proto_write_all(fd, &resp, sizeof(resp));
		return;
	}

	/*
	 * Refuse a manual speed while the daemon is in monitor-only mode.
	 * The control loop does not drive fans in auto, so queueing this would
	 * report success and do nothing: the user would see "set to 1600 rpm"
	 * and an unchanged fan.  Taking control also removes the firmware's own
	 * protection from that fan, so it must be an explicit act, not a side
	 * effect of a speed request.
	 */
	if (req.verb == FANPRO_VERB_SET_FAN && !isnan(req.rpm)) {
		fanpro_mode_t mode;

		pthread_mutex_lock(&d->cfg_lock);
		mode = d->cfg.mode;
		pthread_mutex_unlock(&d->cfg_lock);

		if (mode == FANPRO_MODE_AUTO) {
			resp.status = -4;
			snprintf(resp.message, sizeof(resp.message),
			         "fanprod is in monitor-only mode, so this would have "
			         "no effect. run 'fanpro mode curve' first to let "
			         "fanpro drive the fans.");
			FANPRO_WARN("ipc.request",
			            "verb=set-fan reason=refused_in_auto_mode");
			fanpro_proto_write_all(fd, &resp, sizeof(resp));
			return;
		}
	}

	if (req.verb == FANPRO_VERB_STATUS || req.verb == FANPRO_VERB_PING) {
		pthread_mutex_lock(&d->snap_lock);
		resp = d->snapshot;
		pthread_mutex_unlock(&d->snap_lock);
		resp.magic = FANPRO_PROTO_MAGIC;
		resp.version = FANPRO_PROTO_VERSION;
		resp.status = 0;
	} else if (!fanpro_daemon_push_cmd(d, &req)) {
		resp.status = -3;
		snprintf(resp.message, sizeof(resp.message),
		         "daemon busy: command queue full");
	} else {
		resp.status = 0;
		snprintf(resp.message, sizeof(resp.message), "%s queued",
		         fanpro_verb_name(req.verb));
	}

	fanpro_proto_write_all(fd, &resp, sizeof(resp));
}

static void *
ipc_main(void *arg)
{
	fanpro_daemon_t *d = (fanpro_daemon_t *)arg;

	pthread_setname_np("fanpro-ipc");

	while (atomic_load(&g_running) && !atomic_load(&d->stop)) {
		int fd = accept(g_listen_fd, NULL, NULL);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			if (!atomic_load(&g_running))
				break;
			FANPRO_WARN("ipc.accept", "err=%s", strerror(errno));
			continue;
		}
		/* One request per connection keeps this trivially simple; there
		 * is no session state to get wrong. */
		serve_one(d, fd);
		close(fd);
	}
	return NULL;
}

int
fanpro_ipc_start(fanpro_daemon_t *d)
{
	g_listen_fd = make_socket();
	if (g_listen_fd < 0)
		return -1;

	atomic_store(&g_running, true);
	if (pthread_create(&g_thread, NULL, ipc_main, d) != 0) {
		FANPRO_ERROR("ipc.start", "reason=pthread_create_failed");
		atomic_store(&g_running, false);
		close(g_listen_fd);
		g_listen_fd = -1;
		return -1;
	}
	return 0;
}

void
fanpro_ipc_stop(void)
{
	if (!atomic_load(&g_running))
		return;
	atomic_store(&g_running, false);
	if (g_listen_fd >= 0) {
		shutdown(g_listen_fd, SHUT_RDWR);
		close(g_listen_fd);
		g_listen_fd = -1;
	}
	pthread_join(g_thread, NULL);
	unlink(FANPRO_SOCKET_PATH);
}
