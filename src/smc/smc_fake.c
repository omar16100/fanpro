/*
 * fanpro - in-memory fake SMC backend.
 *
 * This is the test rig for everything that writes to fans.  It models the
 * firmware behaviours that actually bite:
 *
 *   - keys are typed, and reads/writes go through the same codecs as hardware
 *   - absent keys answer 0x84, which is how we detect a machine with no Ftst
 *   - in mode 3 a direct mode write is rejected with 0x82
 *   - setting Ftst=1 makes mode 3 drop to 0 after a delay, on a virtual clock
 *   - clearing Ftst hands control back: every fan mode snaps to 3
 *
 * That last rule is deliberately unforgiving.  It is what turns "clear Ftst
 * while another fan is still manual" from a subtle production bug into a
 * failing test.
 */
#include "fanpro/smc_fake.h"

#include "fanpro/smc_codec.h"

#include <stdlib.h>
#include <string.h>

#define FAKE_MAX_KEYS 128
#define NO_FORCED_RESULT 0xffu

typedef struct {
	uint32_t key;
	uint32_t type;
	uint32_t size;
	double   value;
	uint8_t  forced_result;
	uint32_t write_count;
	bool     used;
} fake_key_t;

struct fanpro_fake {
	fake_key_t                keys[FAKE_MAX_KEYS];
	int                       n_keys;
	fanpro_fake_personality_t personality;
	uint64_t                  now_ms;
	uint64_t                  yield_delay_ms;
	bool                      ftst_pending;   /* Ftst=1, waiting to yield */
	uint64_t                  ftst_set_at_ms;
	bool                      lowercase_mode;
	int                       n_fans;
	fanpro_smc_backend_t      backend;
};

/* ---- key table ---------------------------------------------------------- */

static fake_key_t *
find_key(fanpro_fake_t *f, uint32_t key)
{
	int i;

	for (i = 0; i < f->n_keys; i++) {
		if (f->keys[i].used && f->keys[i].key == key)
			return &f->keys[i];
	}
	return NULL;
}

static uint32_t
fan_key(const fanpro_fake_t *f, int idx, const char *suffix)
{
	char name[5];

	name[0] = 'F';
	name[1] = (char)('0' + idx);
	name[2] = suffix[0];
	name[3] = suffix[1];
	name[4] = '\0';
	(void)f;
	return fanpro_fourcc(name);
}

static uint32_t
mode_key(const fanpro_fake_t *f, int idx)
{
	return fan_key(f, idx, f->lowercase_mode ? "md" : "Md");
}

/* ---- personality -------------------------------------------------------- */

/*
 * Apply time-dependent firmware behaviour before answering any request.
 * Once Ftst has been set for long enough, thermalmonitord yields and every
 * fan sitting in mode 3 drops to mode 0.
 */
static void
settle(fanpro_fake_t *f)
{
	int i;

	if (!f->ftst_pending)
		return;
	if (f->now_ms - f->ftst_set_at_ms < f->yield_delay_ms)
		return;

	for (i = 0; i < f->n_fans; i++) {
		fake_key_t *k = find_key(f, mode_key(f, i));

		if (k != NULL && k->value == 3.0)
			k->value = 0.0;
	}
	f->ftst_pending = false;
}

/* thermalmonitord reclaiming: every fan goes back to system mode. */
static void
reclaim_all(fanpro_fake_t *f)
{
	int i;

	for (i = 0; i < f->n_fans; i++) {
		fake_key_t *k = find_key(f, mode_key(f, i));

		if (k != NULL)
			k->value = 3.0;
	}
	f->ftst_pending = false;
}

static bool
is_mode_key(const fanpro_fake_t *f, uint32_t key)
{
	int i;

	for (i = 0; i < f->n_fans; i++) {
		if (mode_key(f, i) == key)
			return true;
	}
	return false;
}

static bool
is_target_key(const fanpro_fake_t *f, uint32_t key, int *fan_idx)
{
	int i;

	for (i = 0; i < f->n_fans; i++) {
		if (fan_key(f, i, "Tg") == key) {
			*fan_idx = i;
			return true;
		}
	}
	return false;
}

/* ---- request handling --------------------------------------------------- */

static int
handle_keyinfo(fanpro_fake_t *f, const fanpro_smc_key_data_t *in,
               fanpro_smc_key_data_t *out)
{
	fake_key_t *k = find_key(f, in->key);

	if (k == NULL) {
		out->result = FANPRO_SMC_NOT_FOUND;
		return 0;
	}
	/* Forced results model per-access firmware verdicts (0x87 on a write,
	 * 0x86 on a read-only key).  Key info is a metadata lookup the
	 * firmware answers regardless, so it deliberately ignores them. */
	out->key_info.data_size = k->size;
	out->key_info.data_type = k->type;
	out->result = FANPRO_SMC_SUCCESS;
	return 0;
}

static int
handle_read(fanpro_fake_t *f, const fanpro_smc_key_data_t *in,
            fanpro_smc_key_data_t *out)
{
	fake_key_t *k = find_key(f, in->key);

	if (k == NULL) {
		out->result = FANPRO_SMC_NOT_FOUND;
		return 0;
	}
	if (k->forced_result != NO_FORCED_RESULT) {
		out->result = k->forced_result;
		return 0;
	}
	if (!fanpro_smc_encode(k->type, k->value, out->bytes, k->size)) {
		out->result = FANPRO_SMC_KEY_SIZE_MISMATCH;
		return 0;
	}
	out->key_info.data_size = k->size;
	out->key_info.data_type = k->type;
	out->result = FANPRO_SMC_SUCCESS;
	return 0;
}

static int
handle_write(fanpro_fake_t *f, const fanpro_smc_key_data_t *in,
             fanpro_smc_key_data_t *out, uint32_t *io_err)
{
	fake_key_t *k = find_key(f, in->key);
	double value = 0.0;
	int fan_idx;

	if (k == NULL) {
		out->result = FANPRO_SMC_NOT_FOUND;
		return 0;
	}
	if (f->personality == FANPRO_FAKE_NOT_PRIVILEGED) {
		if (io_err != NULL)
			*io_err = 0xe00002c2u; /* kIOReturnNotPrivileged */
		return -1; /* never reached the firmware, so it is not a write */
	}

	/*
	 * Count every write the firmware actually saw, including ones it
	 * rejects.  Counting only applied writes would hide the difference
	 * between "tried once and gave up" and "tried, was refused, retried
	 * after the unlock", which is exactly what the Ftst tests check.
	 */
	k->write_count++;
	if (k->forced_result != NO_FORCED_RESULT) {
		/* 0x87 is the documented "errored but applied anyway" case, so
		 * the fake applies the value too.  Anything else rejects. */
		if (k->forced_result == FANPRO_SMC_KEY_SIZE_MISMATCH &&
		    fanpro_smc_decode(k->type, in->bytes, k->size, &value))
			k->value = value;
		out->result = k->forced_result;
		return 0;
	}
	if (!fanpro_smc_decode(k->type, in->bytes, k->size, &value)) {
		out->result = FANPRO_SMC_KEY_SIZE_MISMATCH;
		return 0;
	}

	/* Mode writes are the contested ones. */
	if (is_mode_key(f, in->key) && value == 1.0) {
		if (f->personality == FANPRO_FAKE_NEEDS_FTST && k->value == 3.0) {
			out->result = FANPRO_SMC_BAD_COMMAND;
			return 0;
		}
	}

	/* Clearing Ftst hands control straight back to thermalmonitord. */
	if (in->key == fanpro_fourcc("Ftst")) {
		k->value = value;
		if (value == 1.0) {
			f->ftst_pending = true;
			f->ftst_set_at_ms = f->now_ms;
		} else {
			reclaim_all(f);
		}
		out->result = FANPRO_SMC_SUCCESS;
		return 0;
	}

	k->value = value;

	/* A commanded target only reaches the blades while the fan is manual. */
	if (is_target_key(f, in->key, &fan_idx)) {
		fake_key_t *mode = find_key(f, mode_key(f, fan_idx));
		fake_key_t *ac = find_key(f, fan_key(f, fan_idx, "Ac"));

		if (mode != NULL && ac != NULL && mode->value == 1.0)
			ac->value = value;
	}

	/* Releasing a fan to auto lets it drop back toward its minimum. */
	if (is_mode_key(f, in->key) && value == 0.0) {
		int i;

		for (i = 0; i < f->n_fans; i++) {
			if (mode_key(f, i) != in->key)
				continue;
			fake_key_t *mn = find_key(f, fan_key(f, i, "Mn"));
			fake_key_t *ac = find_key(f, fan_key(f, i, "Ac"));

			if (mn != NULL && ac != NULL)
				ac->value = mn->value;
		}
	}

	out->result = FANPRO_SMC_SUCCESS;
	return 0;
}

static int
handle_index(fanpro_fake_t *f, const fanpro_smc_key_data_t *in,
             fanpro_smc_key_data_t *out)
{
	uint32_t want = in->data32;
	uint32_t seen = 0;
	int i;

	for (i = 0; i < f->n_keys; i++) {
		if (!f->keys[i].used)
			continue;
		if (seen == want) {
			out->key = f->keys[i].key;
			out->result = FANPRO_SMC_SUCCESS;
			return 0;
		}
		seen++;
	}
	out->result = FANPRO_SMC_NOT_FOUND;
	return 0;
}

static int
fake_open(fanpro_smc_backend_t *be)
{
	(void)be;
	return 0;
}

static void
fake_close(fanpro_smc_backend_t *be)
{
	(void)be;
}

static int
fake_call(fanpro_smc_backend_t *be, const fanpro_smc_key_data_t *in,
          fanpro_smc_key_data_t *out, uint32_t *io_err)
{
	fanpro_fake_t *f = (fanpro_fake_t *)be->ctx;

	if (f == NULL || in == NULL || out == NULL)
		return -1;

	settle(f);
	memset(out, 0, sizeof(*out));
	out->key = in->key;

	switch (in->data8) {
	case FANPRO_SMC_CMD_READ_KEYINFO:
		return handle_keyinfo(f, in, out);
	case FANPRO_SMC_CMD_READ_BYTES:
		return handle_read(f, in, out);
	case FANPRO_SMC_CMD_WRITE_BYTES:
		return handle_write(f, in, out, io_err);
	case FANPRO_SMC_CMD_READ_INDEX:
		return handle_index(f, in, out);
	default:
		out->result = FANPRO_SMC_BAD_COMMAND;
		return 0;
	}
}

/* ---- public API --------------------------------------------------------- */

fanpro_fake_t *
fanpro_fake_create(fanpro_fake_personality_t p)
{
	fanpro_fake_t *f = calloc(1, sizeof(*f));

	if (f == NULL)
		return NULL;

	f->personality = p;
	f->yield_delay_ms = 3500;
	f->backend.name = "fake";
	f->backend.ctx = f;
	f->backend.open = fake_open;
	f->backend.close = fake_close;
	f->backend.call = fake_call;

	/* #KEY is how the key count is read on real hardware. */
	fanpro_fake_add_key(f, "#KEY", "ui32", 4, 0);
	return f;
}

void
fanpro_fake_destroy(fanpro_fake_t *f)
{
	free(f);
}

fanpro_smc_backend_t *
fanpro_fake_backend(fanpro_fake_t *f)
{
	return f != NULL ? &f->backend : NULL;
}

void
fanpro_fake_add_key(fanpro_fake_t *f, const char key[4], const char type[4],
                    uint32_t size, double value)
{
	fake_key_t *k;
	uint32_t id;

	if (f == NULL || f->n_keys >= FAKE_MAX_KEYS)
		return;

	id = fanpro_fourcc(key);
	k = find_key(f, id);
	if (k == NULL)
		k = &f->keys[f->n_keys++];

	k->key = id;
	k->type = fanpro_fourcc(type);
	k->size = size;
	k->value = value;
	k->forced_result = NO_FORCED_RESULT;
	k->write_count = 0;
	k->used = true;

	/* Keep #KEY honest as the table grows. */
	k = find_key(f, fanpro_fourcc("#KEY"));
	if (k != NULL)
		k->value = (double)f->n_keys;
}

void
fanpro_fake_remove_key(fanpro_fake_t *f, const char key[4])
{
	fake_key_t *k = find_key(f, fanpro_fourcc(key));

	if (k != NULL)
		k->used = false;
}

void
fanpro_fake_force_result(fanpro_fake_t *f, const char key[4], uint8_t result)
{
	fake_key_t *k = find_key(f, fanpro_fourcc(key));

	if (k != NULL)
		k->forced_result = result;
}

void
fanpro_fake_clear_result(fanpro_fake_t *f, const char key[4])
{
	fake_key_t *k = find_key(f, fanpro_fourcc(key));

	if (k != NULL)
		k->forced_result = NO_FORCED_RESULT;
}

bool
fanpro_fake_peek(fanpro_fake_t *f, const char key[4], double *out)
{
	fake_key_t *k;

	settle(f);
	k = find_key(f, fanpro_fourcc(key));
	if (k == NULL || out == NULL)
		return false;
	*out = k->value;
	return true;
}

void
fanpro_fake_poke(fanpro_fake_t *f, const char key[4], double value)
{
	fake_key_t *k = find_key(f, fanpro_fourcc(key));

	if (k != NULL)
		k->value = value;
}

void
fanpro_fake_advance_ms(fanpro_fake_t *f, uint64_t ms)
{
	if (f == NULL)
		return;
	f->now_ms += ms;
	settle(f);
}

uint64_t
fanpro_fake_now_ms(const fanpro_fake_t *f)
{
	return f != NULL ? f->now_ms : 0;
}

void
fanpro_fake_set_yield_delay_ms(fanpro_fake_t *f, uint64_t ms)
{
	if (f != NULL)
		f->yield_delay_ms = ms;
}

void
fanpro_fake_add_fans(fanpro_fake_t *f, int count, double min_rpm,
                     double max_rpm, bool lowercase_mode)
{
	double initial_mode;
	int i;

	if (f == NULL || count < 0 || count > 10)
		return;

	f->n_fans = count;
	f->lowercase_mode = lowercase_mode;

	/* On a machine that needs the unlock, fans sit in system mode. */
	initial_mode = (f->personality == FANPRO_FAKE_NEEDS_FTST) ? 3.0 : 0.0;

	fanpro_fake_add_key(f, "FNum", "ui8 ", 1, (double)count);
	if (f->personality == FANPRO_FAKE_NEEDS_FTST)
		fanpro_fake_add_key(f, "Ftst", "ui8 ", 1, 0);

	for (i = 0; i < count; i++) {
		char name[5];

		name[0] = 'F';
		name[1] = (char)('0' + i);
		name[4] = '\0';

		name[2] = 'A'; name[3] = 'c';
		fanpro_fake_add_key(f, name, "flt ", 4, min_rpm);
		name[2] = 'M'; name[3] = 'n';
		fanpro_fake_add_key(f, name, "flt ", 4, min_rpm);
		name[2] = 'M'; name[3] = 'x';
		fanpro_fake_add_key(f, name, "flt ", 4, max_rpm);
		name[2] = 'T'; name[3] = 'g';
		fanpro_fake_add_key(f, name, "flt ", 4, min_rpm);

		name[2] = lowercase_mode ? 'm' : 'M';
		name[3] = 'd';
		fanpro_fake_add_key(f, name, "ui8 ", 1, initial_mode);
	}
}

uint32_t
fanpro_fake_write_count(const fanpro_fake_t *f, const char key[4])
{
	const fake_key_t *k;
	uint32_t id;
	int i;

	if (f == NULL)
		return 0;
	id = fanpro_fourcc(key);
	for (i = 0; i < f->n_keys; i++) {
		k = &f->keys[i];
		if (k->used && k->key == id)
			return k->write_count;
	}
	return 0;
}
