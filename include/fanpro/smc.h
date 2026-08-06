/*
 * fanpro - SMC transport, key types, and backend abstraction.
 *
 * The AppleSMC user client speaks a fixed 80-byte struct over
 * IOConnectCallStructMethod(selector 2).  Everything above this header is
 * written against fanpro_smc_backend so it can run against the in-memory fake
 * backend with no hardware present.
 */
#ifndef FANPRO_SMC_H
#define FANPRO_SMC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- SMC firmware result codes (returned in smc_key_data_t.result) ------- */
#define FANPRO_SMC_SUCCESS            0x00
/* Firmware rejected the command.  Observed on F%dMd writes while in mode 3. */
#define FANPRO_SMC_BAD_COMMAND        0x82
/* Key does not exist on this machine.  Not an error: it is how we detect that
 * a generation lacks e.g. Ftst. */
#define FANPRO_SMC_NOT_FOUND          0x84
#define FANPRO_SMC_NOT_READABLE       0x85
#define FANPRO_SMC_NOT_WRITABLE       0x86
/* Size mismatch.  Documented quirk: on F%dTg writes the value is frequently
 * applied anyway, so callers must verify by reading F%dAc rather than treat
 * this as a hard failure. */
#define FANPRO_SMC_KEY_SIZE_MISMATCH  0x87

/* ---- Wire commands (smc_key_data_t.data8) ------------------------------- */
#define FANPRO_SMC_CMD_READ_BYTES     5
#define FANPRO_SMC_CMD_WRITE_BYTES    6
#define FANPRO_SMC_CMD_READ_INDEX     8
#define FANPRO_SMC_CMD_READ_KEYINFO   9
#define FANPRO_SMC_CMD_READ_KEYCOUNT  1

/* Maximum payload the SMC struct can carry. */
#define FANPRO_SMC_MAX_DATA           32

/* ---- Wire structs ------------------------------------------------------- */
/* Field offsets are load-bearing; see the static asserts in smc.c.
 * key@0, key_info.data_size@28, result@40, data8@42, bytes@48, total 80. */

typedef struct {
	uint32_t data_size;
	uint32_t data_type; /* fourcc, e.g. 'flt ', 'ui8 ', 'fpe2' */
	uint8_t  data_attributes;
} fanpro_smc_key_info_t;

typedef struct {
	uint32_t key;
	uint32_t vers;
	uint8_t  p_limit_data[16];
	uint8_t  padding0[4];
	fanpro_smc_key_info_t key_info;
	uint8_t  result;
	uint8_t  status;
	uint8_t  data8;
	uint8_t  padding1;
	uint32_t data32;
	uint8_t  bytes[FANPRO_SMC_MAX_DATA];
} fanpro_smc_key_data_t;

/* ---- Backend vtable ----------------------------------------------------- */
/*
 * open()  -> 0 on success, negative errno-ish on failure.
 * call()  -> 0 if the IOKit round trip itself succeeded (inspect out->result
 *            for the firmware verdict); negative on transport failure.
 *            io_err, when non-NULL, receives the raw IOReturn so callers can
 *            distinguish kIOReturnNotPrivileged (0xe00002c2).
 */
typedef struct fanpro_smc_backend {
	const char *name;
	void       *ctx;
	int (*open)(struct fanpro_smc_backend *be);
	void (*close)(struct fanpro_smc_backend *be);
	int (*call)(struct fanpro_smc_backend *be,
	            const fanpro_smc_key_data_t *in,
	            fanpro_smc_key_data_t *out,
	            uint32_t *io_err);
} fanpro_smc_backend_t;

/* ---- Handle ------------------------------------------------------------- */
typedef struct {
	fanpro_smc_backend_t *be;
	bool                  opened;
} fanpro_smc_t;

/* A decoded SMC value.  raw/raw_len always hold the wire bytes; num holds the
 * decoded numeric value when the type is one fanpro understands. */
typedef struct {
	uint32_t type;                        /* fourcc */
	uint32_t size;
	uint8_t  raw[FANPRO_SMC_MAX_DATA];
	uint32_t raw_len;
	double   num;
	bool     numeric;                     /* false if type is unrecognised */
} fanpro_smc_value_t;

int  fanpro_smc_open(fanpro_smc_t *smc, fanpro_smc_backend_t *be);
void fanpro_smc_close(fanpro_smc_t *smc);

/* Key info: fills type/size.  Returns the firmware result code (0 on success),
 * or a negative value on transport failure. */
int fanpro_smc_key_info(fanpro_smc_t *smc, uint32_t key,
                        fanpro_smc_key_info_t *info);

/* Read a key, decoding by its runtime-reported type.  Never hardcode types. */
int fanpro_smc_read(fanpro_smc_t *smc, uint32_t key, fanpro_smc_value_t *out);

/* Write a numeric value, encoding by the key's runtime-reported type.
 * Returns the firmware result code; FANPRO_SMC_KEY_SIZE_MISMATCH (0x87) is
 * returned verbatim and must be treated as applied-with-warning by callers. */
int fanpro_smc_write_num(fanpro_smc_t *smc, uint32_t key, double value);

/* Key-space enumeration, for `fanpro smc dump`. */
int fanpro_smc_key_count(fanpro_smc_t *smc, uint32_t *count);
int fanpro_smc_key_at(fanpro_smc_t *smc, uint32_t index, uint32_t *key);

/* Backends. */
fanpro_smc_backend_t *fanpro_smc_backend_iokit(void);

#endif /* FANPRO_SMC_H */
