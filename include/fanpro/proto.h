/*
 * fanpro - IPC protocol between the CLI and the daemon.
 *
 * Fixed-size binary structs, not JSON.  A hand-rolled text parser sitting in
 * a root daemon reading input from any local admin is the worst risk/benefit
 * trade available here: the command set is about ten verbs, none of which
 * need a grammar.
 *
 * Everything is host byte order and fixed width.  Both ends are the same
 * binary on the same machine; there is no network and no cross-arch case.
 * The version field exists so a stale CLI is rejected rather than
 * misinterpreted after an upgrade.
 */
#ifndef FANPRO_PROTO_H
#define FANPRO_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FANPRO_SOCKET_PATH "/var/run/fanpro.sock"

#define FANPRO_PROTO_MAGIC   0x464E5052u /* "FNPR" */
#define FANPRO_PROTO_VERSION 1

/* Bounded so a response is always one fixed-size struct. */
#define FANPRO_PROTO_MAX_FANS    8
#define FANPRO_PROTO_MAX_SENSORS 24
#define FANPRO_PROTO_NAME_MAX    48
#define FANPRO_PROTO_TEXT_MAX    256

typedef enum {
	FANPRO_VERB_PING = 0,
	FANPRO_VERB_STATUS,      /* daemon + fan state */
	FANPRO_VERB_SET_FAN,     /* fan_index, rpm (or auto) */
	FANPRO_VERB_SET_MODE,    /* auto | curve */
	FANPRO_VERB_APPLY_CURVE, /* bind a named curve to a fan */
	FANPRO_VERB_RELOAD,      /* re-read the config file */
	FANPRO_VERB_RELEASE_ALL, /* hand every fan back to the firmware */
	FANPRO_VERB_ENABLE,      /* clear the crash-loop latch */
	FANPRO_VERB__COUNT,
} fanpro_verb_t;

/* Does this verb change hardware or daemon state? */
bool fanpro_verb_is_mutating(uint32_t verb);
const char *fanpro_verb_name(uint32_t verb);

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t verb;
	uint32_t flags;
	int32_t  fan_index;  /* -1 means "all fans" */
	int32_t  ival;       /* verb-specific integer */
	double   rpm;        /* NaN means "auto" */
	char     name[FANPRO_PROTO_NAME_MAX];
} fanpro_request_t;

typedef struct {
	int32_t index;
	int32_t mode;        /* FANPRO_FAN_MODE_* */
	int32_t held;        /* is the daemon driving this fan */
	int32_t pad;
	double  rpm;
	double  target;
	double  min_rpm;
	double  max_rpm;
	char    curve[FANPRO_PROTO_NAME_MAX];
} fanpro_fan_status_t;

typedef struct {
	char   name[FANPRO_PROTO_NAME_MAX];
	double celsius;
	int32_t cls;
	int32_t pad;
} fanpro_sensor_status_t;

typedef struct {
	uint32_t magic;
	uint32_t version;
	int32_t  status;   /* 0 ok, negative error */
	int32_t  mode;     /* fanpro_mode_t */

	int32_t  n_fans;
	int32_t  n_sensors;
	int32_t  latched;  /* control refused after an unclean exit */
	int32_t  thermal_level;

	double   uptime_s;
	double   cpu_watts;
	double   gpu_watts;

	fanpro_fan_status_t    fans[FANPRO_PROTO_MAX_FANS];
	fanpro_sensor_status_t sensors[FANPRO_PROTO_MAX_SENSORS];

	char     message[FANPRO_PROTO_TEXT_MAX];
} fanpro_response_t;

void fanpro_request_init(fanpro_request_t *r, fanpro_verb_t verb);
void fanpro_response_init(fanpro_response_t *r);

/*
 * Validate a received request.  Returns 0 if it is safe to act on, negative
 * otherwise, with a reason in `why`.  Rejects bad magic, wrong version,
 * unknown verbs, out-of-range fan indices, non-finite or absurd RPM, and
 * unterminated name fields.
 */
int fanpro_request_validate(const fanpro_request_t *r, int fan_count,
                            const char **why);

/* Blocking full read/write of exactly `len` bytes.  0 on success. */
int fanpro_proto_write_all(int fd, const void *buf, size_t len);
int fanpro_proto_read_all(int fd, void *buf, size_t len);

#endif /* FANPRO_PROTO_H */
