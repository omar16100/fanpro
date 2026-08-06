/*
 * fanpro - SMC transport.
 *
 * Every operation is a single 80-byte struct round trip through the backend.
 * Two return conventions are in play and the distinction matters:
 *
 *   negative  the round trip itself failed (no user client, IOKit error)
 *   >= 0      the firmware answered; the value is its result code, so 0 is
 *             success and e.g. 0x84 means "no such key on this machine"
 *
 * Callers must never collapse those two.  0x84 on Ftst is how we learn the
 * machine does not need the unlock, and 0x87 on a fan target is a warning
 * whose write frequently landed anyway.
 *
 * Types are always read from the firmware at runtime.  Hardcoding "fan keys
 * are floats" is exactly the bug that makes tools silently command the wrong
 * speed on the wrong generation.
 */
#include "fanpro/smc.h"

#include "fanpro/log.h"
#include "fanpro/smc_codec.h"

#include <string.h>

/* The kernel ABI is positional; a padding mistake here corrupts every call. */
_Static_assert(sizeof(fanpro_smc_key_data_t) == 80, "smc key data must be 80 bytes");
_Static_assert(offsetof(fanpro_smc_key_data_t, key) == 0, "key at offset 0");
_Static_assert(offsetof(fanpro_smc_key_data_t, key_info) == 28, "key_info at 28");
_Static_assert(offsetof(fanpro_smc_key_data_t, result) == 40, "result at 40");
_Static_assert(offsetof(fanpro_smc_key_data_t, data8) == 42, "data8 at 42");
_Static_assert(offsetof(fanpro_smc_key_data_t, bytes) == 48, "bytes at 48");

int
fanpro_smc_open(fanpro_smc_t *smc, fanpro_smc_backend_t *be)
{
	int rc;

	if (smc == NULL || be == NULL || be->open == NULL || be->call == NULL)
		return -1;

	memset(smc, 0, sizeof(*smc));
	rc = be->open(be);
	if (rc != 0) {
		FANPRO_ERROR("smc.open", "backend=%s rc=%d", be->name, rc);
		return rc;
	}
	smc->be = be;
	smc->opened = true;
	FANPRO_DEBUG("smc.open", "backend=%s", be->name);
	return 0;
}

void
fanpro_smc_close(fanpro_smc_t *smc)
{
	if (smc == NULL || !smc->opened)
		return;
	if (smc->be->close != NULL)
		smc->be->close(smc->be);
	smc->opened = false;
	smc->be = NULL;
}

/* One round trip.  Returns negative on transport failure, else out->result. */
static int
smc_call(fanpro_smc_t *smc, const fanpro_smc_key_data_t *in,
         fanpro_smc_key_data_t *out)
{
	uint32_t io_err = 0;
	int rc;

	if (smc == NULL || !smc->opened)
		return -1;

	memset(out, 0, sizeof(*out));
	rc = smc->be->call(smc->be, in, out, &io_err);
	if (rc != 0) {
		char k[5];

		fanpro_fourcc_str(in->key, k);
		FANPRO_DEBUG("smc.call.fail", "key=%s cmd=%u rc=%d io_err=0x%08x",
		             k, in->data8, rc, io_err);
		return rc;
	}
	return (int)out->result;
}

int
fanpro_smc_key_info(fanpro_smc_t *smc, uint32_t key, fanpro_smc_key_info_t *info)
{
	fanpro_smc_key_data_t in, out;
	int rc;

	if (info == NULL)
		return -1;

	memset(&in, 0, sizeof(in));
	in.key = key;
	in.data8 = FANPRO_SMC_CMD_READ_KEYINFO;

	rc = smc_call(smc, &in, &out);
	if (rc != FANPRO_SMC_SUCCESS)
		return rc;

	if (out.key_info.data_size == 0 ||
	    out.key_info.data_size > FANPRO_SMC_MAX_DATA) {
		char k[5];

		fanpro_fourcc_str(key, k);
		FANPRO_WARN("smc.keyinfo.bad_size", "key=%s size=%u", k,
		            out.key_info.data_size);
		return -2;
	}

	*info = out.key_info;
	return FANPRO_SMC_SUCCESS;
}

int
fanpro_smc_read(fanpro_smc_t *smc, uint32_t key, fanpro_smc_value_t *out)
{
	fanpro_smc_key_info_t info;
	fanpro_smc_key_data_t in, resp;
	int rc;

	if (out == NULL)
		return -1;

	rc = fanpro_smc_key_info(smc, key, &info);
	if (rc != FANPRO_SMC_SUCCESS)
		return rc;

	memset(&in, 0, sizeof(in));
	in.key = key;
	in.key_info.data_size = info.data_size;
	in.data8 = FANPRO_SMC_CMD_READ_BYTES;

	rc = smc_call(smc, &in, &resp);
	if (rc != FANPRO_SMC_SUCCESS)
		return rc;

	memset(out, 0, sizeof(*out));
	out->type = info.data_type;
	out->size = info.data_size;
	out->raw_len = info.data_size;
	memcpy(out->raw, resp.bytes, info.data_size);
	out->numeric = fanpro_smc_decode(info.data_type, resp.bytes,
	                                 info.data_size, &out->num);

	if (fanpro_log_get_level() >= FANPRO_LOG_TRACE) {
		char k[5], t[5];

		fanpro_fourcc_str(key, k);
		fanpro_fourcc_str(info.data_type, t);
		FANPRO_TRACE("smc.read", "key=%s type=%s size=%u value=%.3f numeric=%d",
		             k, t, info.data_size, out->num, (int)out->numeric);
	}
	return FANPRO_SMC_SUCCESS;
}

int
fanpro_smc_write_num(fanpro_smc_t *smc, uint32_t key, double value)
{
	fanpro_smc_key_info_t info;
	fanpro_smc_key_data_t in, resp;
	char k[5], t[5];
	int rc;

	rc = fanpro_smc_key_info(smc, key, &info);
	if (rc != FANPRO_SMC_SUCCESS)
		return rc;

	memset(&in, 0, sizeof(in));
	in.key = key;
	in.key_info.data_size = info.data_size;
	in.data8 = FANPRO_SMC_CMD_WRITE_BYTES;

	if (!fanpro_smc_encode(info.data_type, value, in.bytes, info.data_size)) {
		fanpro_fourcc_str(key, k);
		fanpro_fourcc_str(info.data_type, t);
		FANPRO_ERROR("smc.write.encode_failed",
		             "key=%s type=%s size=%u value=%.3f", k, t,
		             info.data_size, value);
		return -3;
	}

	rc = smc_call(smc, &in, &resp);

	/* Every write is logged with its exact wire bytes and verdict: the log
	 * alone must be enough to reconstruct what we told the firmware. */
	fanpro_fourcc_str(key, k);
	fanpro_fourcc_str(info.data_type, t);
	if (rc < 0) {
		FANPRO_ERROR("smc.write",
		             "key=%s type=%s size=%u value=%.3f bytes=%02x%02x%02x%02x result=transport rc=%d",
		             k, t, info.data_size, value, in.bytes[0], in.bytes[1],
		             in.bytes[2], in.bytes[3], rc);
	} else {
		FANPRO_INFO("smc.write",
		            "key=%s type=%s size=%u value=%.3f bytes=%02x%02x%02x%02x result=0x%02x",
		            k, t, info.data_size, value, in.bytes[0], in.bytes[1],
		            in.bytes[2], in.bytes[3], (unsigned)rc);
	}
	return rc;
}

int
fanpro_smc_key_count(fanpro_smc_t *smc, uint32_t *count)
{
	fanpro_smc_value_t v;
	int rc;

	if (count == NULL)
		return -1;

	rc = fanpro_smc_read(smc, fanpro_fourcc("#KEY"), &v);
	if (rc != FANPRO_SMC_SUCCESS)
		return rc;
	if (!v.numeric)
		return -2;

	*count = (uint32_t)v.num;
	return FANPRO_SMC_SUCCESS;
}

int
fanpro_smc_key_at(fanpro_smc_t *smc, uint32_t index, uint32_t *key)
{
	fanpro_smc_key_data_t in, out;
	int rc;

	if (key == NULL)
		return -1;

	memset(&in, 0, sizeof(in));
	in.data8 = FANPRO_SMC_CMD_READ_INDEX;
	in.data32 = index;

	rc = smc_call(smc, &in, &out);
	if (rc != FANPRO_SMC_SUCCESS)
		return rc;

	*key = out.key;
	return FANPRO_SMC_SUCCESS;
}
