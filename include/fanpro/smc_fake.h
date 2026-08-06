/*
 * fanpro - in-memory fake SMC backend.
 *
 * Models enough firmware behaviour to test the control plane with no hardware:
 * per-key type/size/attributes, per-key forced result codes, and a switchable
 * "personality" that reproduces the generation differences we care about
 * (direct mode write accepted vs. rejected with 0x82 until Ftst is set).
 */
#ifndef FANPRO_SMC_FAKE_H
#define FANPRO_SMC_FAKE_H

#include <stdbool.h>
#include <stdint.h>

#include "fanpro/smc.h"

typedef enum {
	/* M1 / M5 style: a direct F%dMd (or F%dmd) write to 1 just works. */
	FANPRO_FAKE_DIRECT_OK = 0,
	/* M4 style: mode reads 3, direct write returns 0x82; setting Ftst=1
	 * makes the mode drop to 0 after a delay, then the write is accepted. */
	FANPRO_FAKE_NEEDS_FTST,
	/* Writes always fail with kIOReturnNotPrivileged. */
	FANPRO_FAKE_NOT_PRIVILEGED,
} fanpro_fake_personality_t;

typedef struct fanpro_fake fanpro_fake_t;

fanpro_fake_t        *fanpro_fake_create(fanpro_fake_personality_t p);
void                  fanpro_fake_destroy(fanpro_fake_t *f);
fanpro_smc_backend_t *fanpro_fake_backend(fanpro_fake_t *f);

/* Define a key.  `type` and `size` are what key-info will report. */
void fanpro_fake_add_key(fanpro_fake_t *f, const char key[4], const char type[4],
                         uint32_t size, double value);
void fanpro_fake_remove_key(fanpro_fake_t *f, const char key[4]);

/* Force a firmware result code for every access to `key` until cleared. */
void fanpro_fake_force_result(fanpro_fake_t *f, const char key[4], uint8_t result);
void fanpro_fake_clear_result(fanpro_fake_t *f, const char key[4]);

/* Direct state access, bypassing firmware rules, for arranging and asserting.
 * fanpro_fake_poke also models an external SMC client mutating state. */
bool fanpro_fake_peek(fanpro_fake_t *f, const char key[4], double *out);
void fanpro_fake_poke(fanpro_fake_t *f, const char key[4], double value);

/* Virtual clock, in milliseconds.  The Ftst yield delay is measured against
 * this, so tests advance time instead of sleeping. */
void     fanpro_fake_advance_ms(fanpro_fake_t *f, uint64_t ms);
uint64_t fanpro_fake_now_ms(const fanpro_fake_t *f);

/* How long after Ftst=1 the fake takes to drop mode 3 -> 0.  Default 3500. */
void fanpro_fake_set_yield_delay_ms(fanpro_fake_t *f, uint64_t ms);

/* Convenience: populate FNum plus F<n>{Ac,Mn,Mx,Tg} and the mode key.
 * `lowercase_mode` selects F%dmd (M5 style) over F%dMd. */
void fanpro_fake_add_fans(fanpro_fake_t *f, int count, double min_rpm,
                          double max_rpm, bool lowercase_mode);

/*
 * Writes the firmware saw for this key, INCLUDING ones it rejected.  Lets a
 * test tell "gave up after one refusal" from "retried after the unlock".
 */
uint32_t fanpro_fake_write_count(const fanpro_fake_t *f, const char key[4]);

#endif /* FANPRO_SMC_FAKE_H */
