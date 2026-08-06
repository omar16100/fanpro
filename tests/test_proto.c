/*
 * Protocol and peer-auth tests.
 *
 * This is the boundary between a root daemon and anything a local user can
 * send it, so the tests are mostly about what must be REFUSED.
 */
#include "tinytest.h"

#include "fanpro/peerauth.h"
#include "fanpro/proto.h"

#include <math.h>
#include <string.h>

TT_TEST(proto_valid_request_passes)
{
	fanpro_request_t r;
	const char *why = NULL;

	fanpro_request_init(&r, FANPRO_VERB_STATUS);
	TT_EQ_UINT(r.magic, FANPRO_PROTO_MAGIC);
	TT_EQ_INT(fanpro_request_validate(&r, 2, &why), 0);

	fanpro_request_init(&r, FANPRO_VERB_SET_FAN);
	r.fan_index = 0;
	r.rpm = 1800.0;
	TT_EQ_INT(fanpro_request_validate(&r, 2, &why), 0);
}

TT_TEST(proto_rejects_bad_magic_and_version)
{
	fanpro_request_t r;
	const char *why = NULL;

	fanpro_request_init(&r, FANPRO_VERB_STATUS);
	r.magic = 0xdeadbeef;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);

	/* A stale CLI must be refused, not reinterpreted: its fields would be
	 * read as entirely different things. */
	fanpro_request_init(&r, FANPRO_VERB_STATUS);
	r.version = FANPRO_PROTO_VERSION + 1;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);
	TT_ASSERT(strstr(why, "version") != NULL);
}

TT_TEST(proto_rejects_unknown_verbs)
{
	fanpro_request_t r;
	const char *why = NULL;

	fanpro_request_init(&r, FANPRO_VERB_STATUS);
	r.verb = FANPRO_VERB__COUNT;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);
	r.verb = 0xffffffffu;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);
}

TT_TEST(proto_rejects_an_unterminated_name)
{
	fanpro_request_t r;
	const char *why = NULL;

	fanpro_request_init(&r, FANPRO_VERB_APPLY_CURVE);
	r.fan_index = 0;
	/* No NUL anywhere: any str* call downstream would run off the end of
	 * the struct. */
	memset(r.name, 'A', sizeof(r.name));
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);
	TT_ASSERT(strstr(why, "unterminated") != NULL);
}

TT_TEST(proto_rejects_out_of_range_fan)
{
	fanpro_request_t r;
	const char *why = NULL;

	fanpro_request_init(&r, FANPRO_VERB_SET_FAN);
	r.rpm = 1500.0;

	r.fan_index = 99;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);
	r.fan_index = -2;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);

	/* Within the struct's bounds but beyond this machine's fan count. */
	r.fan_index = 5;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);

	/* -1 means "all fans" and is legal. */
	r.fan_index = -1;
	TT_EQ_INT(fanpro_request_validate(&r, 2, &why), 0);
}

TT_TEST(proto_rejects_absurd_rpm_but_allows_nan_as_auto)
{
	fanpro_request_t r;
	const char *why = NULL;

	fanpro_request_init(&r, FANPRO_VERB_SET_FAN);
	r.fan_index = 0;

	r.rpm = 1e9;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);
	r.rpm = -100.0;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);
	r.rpm = 1.0 / 0.0;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);

	/* NaN is the wire encoding of "return this fan to auto". */
	r.rpm = NAN;
	TT_EQ_INT(fanpro_request_validate(&r, 2, &why), 0);
}

TT_TEST(proto_apply_curve_needs_a_name_and_a_fan)
{
	fanpro_request_t r;
	const char *why = NULL;

	fanpro_request_init(&r, FANPRO_VERB_APPLY_CURVE);
	r.fan_index = 0;
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0); /* no name */

	snprintf(r.name, sizeof(r.name), "safe");
	TT_EQ_INT(fanpro_request_validate(&r, 2, &why), 0);

	r.fan_index = -1; /* "all fans" makes no sense for a curve binding */
	TT_ASSERT(fanpro_request_validate(&r, 2, &why) != 0);
}

TT_TEST(proto_mutating_verb_classification)
{
	TT_FALSE(fanpro_verb_is_mutating(FANPRO_VERB_PING));
	TT_FALSE(fanpro_verb_is_mutating(FANPRO_VERB_STATUS));
	TT_TRUE(fanpro_verb_is_mutating(FANPRO_VERB_SET_FAN));
	TT_TRUE(fanpro_verb_is_mutating(FANPRO_VERB_RELEASE_ALL));
	TT_TRUE(fanpro_verb_is_mutating(FANPRO_VERB_ENABLE));

	/* An unknown verb must default to mutating: if a newer CLI invents one,
	 * an older daemon refusing it is the safe outcome. */
	TT_TRUE(fanpro_verb_is_mutating(FANPRO_VERB__COUNT + 7));
}

/* ---- peer auth ---------------------------------------------------------- */

static fanpro_peer_t
peer(uint32_t uid, uint32_t g0, int ngroups)
{
	fanpro_peer_t p;

	memset(&p, 0, sizeof(p));
	p.valid = true;
	p.uid = uid;
	p.n_groups = ngroups;
	if (ngroups > 0)
		p.groups[0] = g0;
	return p;
}

TT_TEST(peerauth_decision_table)
{
	fanpro_peer_t root = peer(0, 0, 1);
	fanpro_peer_t admin = peer(501, FANPRO_ADMIN_GID, 1);
	fanpro_peer_t plain = peer(502, 20, 1);
	const char *why = NULL;

	/* Reads are open to any peer we could identify. */
	TT_TRUE(fanpro_peer_may(&root, FANPRO_VERB_STATUS, &why));
	TT_TRUE(fanpro_peer_may(&admin, FANPRO_VERB_STATUS, &why));
	TT_TRUE(fanpro_peer_may(&plain, FANPRO_VERB_STATUS, &why));

	/* Writes need root or admin. */
	TT_TRUE(fanpro_peer_may(&root, FANPRO_VERB_SET_FAN, &why));
	TT_TRUE(fanpro_peer_may(&admin, FANPRO_VERB_SET_FAN, &why));
	TT_FALSE(fanpro_peer_may(&plain, FANPRO_VERB_SET_FAN, &why));
	TT_FALSE(fanpro_peer_may(&plain, FANPRO_VERB_RELEASE_ALL, &why));
}

TT_TEST(peerauth_denies_unreadable_credentials)
{
	fanpro_peer_t bad;
	const char *why = NULL;

	memset(&bad, 0, sizeof(bad));
	bad.valid = false; /* what fanpro_peer_read leaves on failure */
	bad.uid = 0;       /* even claiming uid 0 must not help */

	/* "Unknown" is not a lesser privilege level: if we cannot tell who is
	 * asking, we do not answer at all, not even a read. */
	TT_FALSE(fanpro_peer_may(&bad, FANPRO_VERB_STATUS, &why));
	TT_FALSE(fanpro_peer_may(&bad, FANPRO_VERB_SET_FAN, &why));
	TT_FALSE(fanpro_peer_may(NULL, FANPRO_VERB_STATUS, &why));
}

TT_TEST(peerauth_finds_admin_beyond_the_first_group)
{
	fanpro_peer_t p;
	const char *why = NULL;

	memset(&p, 0, sizeof(p));
	p.valid = true;
	p.uid = 501;
	p.n_groups = 4;
	p.groups[0] = 20;
	p.groups[1] = 12;
	p.groups[2] = FANPRO_ADMIN_GID;
	p.groups[3] = 61;

	/* Checking only cr_groups[0] is a classic bug here. */
	TT_TRUE(fanpro_peer_may(&p, FANPRO_VERB_SET_FAN, &why));

	/* And a peer with no groups at all is not admin. */
	p.n_groups = 0;
	TT_FALSE(fanpro_peer_may(&p, FANPRO_VERB_SET_FAN, &why));
}
