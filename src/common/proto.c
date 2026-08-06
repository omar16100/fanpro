/*
 * fanpro - IPC framing and request validation.
 *
 * The daemon runs as root and this is the only thing standing between it and
 * whatever any local admin sends down the socket, so validation is
 * deliberately paranoid: every field is bounded before any of it reaches the
 * control loop.
 *
 * Framing is trivially simple by design.  Both structs are fixed size, so
 * "read exactly sizeof(request)" is the entire protocol; there is no length
 * prefix to disagree with the payload and no partial-message state machine.
 */
#include "fanpro/proto.h"

#include "fanpro/fan.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

/* Nothing sane commands a fan beyond this; the safety gate clamps to the
 * fan's real limits afterwards, but absurd input is rejected outright. */
#define PROTO_MAX_RPM 20000.0

static const char *const VERB_NAME[FANPRO_VERB__COUNT] = {
	"ping", "status", "set-fan", "set-mode", "apply-curve",
	"reload", "release-all", "enable",
};

const char *
fanpro_verb_name(uint32_t verb)
{
	if (verb >= FANPRO_VERB__COUNT)
		return "unknown";
	return VERB_NAME[verb];
}

bool
fanpro_verb_is_mutating(uint32_t verb)
{
	switch (verb) {
	case FANPRO_VERB_PING:
	case FANPRO_VERB_STATUS:
		return false;
	default:
		/* Unknown verbs count as mutating: if a future version adds one
		 * and an old daemon sees it, refusing is the safe default. */
		return true;
	}
}

void
fanpro_request_init(fanpro_request_t *r, fanpro_verb_t verb)
{
	if (r == NULL)
		return;
	memset(r, 0, sizeof(*r));
	r->magic = FANPRO_PROTO_MAGIC;
	r->version = FANPRO_PROTO_VERSION;
	r->verb = (uint32_t)verb;
	r->fan_index = -1;
	r->rpm = NAN;
}

void
fanpro_response_init(fanpro_response_t *r)
{
	if (r == NULL)
		return;
	memset(r, 0, sizeof(*r));
	r->magic = FANPRO_PROTO_MAGIC;
	r->version = FANPRO_PROTO_VERSION;
}

/* Is this char array NUL-terminated within its bounds? */
static bool
terminated(const char *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (buf[i] == '\0')
			return true;
	}
	return false;
}

int
fanpro_request_validate(const fanpro_request_t *r, int fan_count,
                        const char **why)
{
	const char *ignored = NULL;

	if (why == NULL)
		why = &ignored;

	if (r == NULL) {
		*why = "null request";
		return -1;
	}
	if (r->magic != FANPRO_PROTO_MAGIC) {
		*why = "bad magic";
		return -1;
	}
	if (r->version != FANPRO_PROTO_VERSION) {
		/* Reject rather than reinterpret: a stale CLI sending an older
		 * layout would otherwise have its fields read as the wrong
		 * things entirely. */
		*why = "protocol version mismatch";
		return -1;
	}
	if (r->verb >= FANPRO_VERB__COUNT) {
		*why = "unknown verb";
		return -1;
	}
	/* An unterminated name would run off the end of the struct in any
	 * str* call downstream. */
	if (!terminated(r->name, sizeof(r->name))) {
		*why = "unterminated name";
		return -1;
	}

	if (r->fan_index < -1 || r->fan_index >= FANPRO_PROTO_MAX_FANS) {
		*why = "fan index out of range";
		return -1;
	}
	if (fan_count >= 0 && r->fan_index >= fan_count) {
		*why = "no such fan on this machine";
		return -1;
	}

	switch (r->verb) {
	case FANPRO_VERB_SET_FAN:
		/* NaN is the wire encoding of "auto", so it is allowed here and
		 * nowhere else. */
		if (!isnan(r->rpm)) {
			if (!isfinite(r->rpm)) {
				*why = "rpm is not finite";
				return -1;
			}
			if (r->rpm < 0.0 || r->rpm > PROTO_MAX_RPM) {
				*why = "rpm out of range";
				return -1;
			}
		}
		break;
	case FANPRO_VERB_APPLY_CURVE:
		if (r->name[0] == '\0') {
			*why = "curve name is empty";
			return -1;
		}
		if (r->fan_index < 0) {
			*why = "apply-curve needs a fan";
			return -1;
		}
		break;
	case FANPRO_VERB_SET_MODE:
		if (r->ival != 0 && r->ival != 1) {
			*why = "mode must be auto or curve";
			return -1;
		}
		break;
	default:
		break;
	}

	*why = "ok";
	return 0;
}

int
fanpro_proto_write_all(int fd, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t done = 0;

	if (fd < 0 || buf == NULL)
		return -1;

	while (done < len) {
		ssize_t n = write(fd, p + done, len - done);

		if (n > 0) {
			done += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

int
fanpro_proto_read_all(int fd, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;
	size_t done = 0;

	if (fd < 0 || buf == NULL)
		return -1;

	while (done < len) {
		ssize_t n = read(fd, p + done, len - done);

		if (n > 0) {
			done += (size_t)n;
			continue;
		}
		/* A short read means the peer went away mid-message.  Acting on
		 * a partially filled struct would mean acting on uninitialised
		 * fields, so this is always a hard failure. */
		if (n == 0)
			return -1;
		if (errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}
