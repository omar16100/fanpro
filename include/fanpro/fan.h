/*
 * fanpro - fan enumeration and state.
 *
 * Key naming is discovered, not assumed: the mode key is F%dMd on most
 * generations and F%dmd on M5, and Ftst exists only where the firmware needs
 * the diagnostic unlock.  Probing once at enumeration keeps every caller
 * above this layer generation-agnostic.
 */
#ifndef FANPRO_FAN_H
#define FANPRO_FAN_H

#include <stdbool.h>
#include <stdint.h>

#include "fanpro/smc.h"

#define FANPRO_MAX_FANS 8

/* Firmware fan modes. */
#define FANPRO_FAN_MODE_AUTO    0
#define FANPRO_FAN_MODE_MANUAL  1
#define FANPRO_FAN_MODE_SYSTEM  3
#define FANPRO_FAN_MODE_UNKNOWN (-1)

typedef struct {
	int      index;
	double   rpm;      /* F%dAc */
	double   min_rpm;  /* F%dMn */
	double   max_rpm;  /* F%dMx */
	double   target;   /* F%dTg */
	int      mode;     /* F%dMd or F%dmd, FANPRO_FAN_MODE_* */
	/*
	 * Latched at enumeration: this machine has a mode key for this fan.
	 * Deliberately NOT recomputed by refresh.  A transient read failure
	 * must never make the release path believe a fan is uncontrollable and
	 * skip it, because that leaves it pinned.
	 */
	bool     mode_key_known;
	/* Live: the last refresh actually read the mode back. */
	bool     mode_readable;
	bool     has_target;
	uint32_t key_ac, key_mn, key_mx, key_tg, key_md;
} fanpro_fan_t;

typedef struct {
	fanpro_fan_t fans[FANPRO_MAX_FANS];
	int          count;
	bool         lowercase_mode; /* F%dmd rather than F%dMd */
	bool         has_ftst;       /* firmware exposes the unlock key */
	int          ftst;           /* -1 unknown, else 0/1 */
} fanpro_fan_set_t;

/*
 * Discover fan count and key naming, then read current state.
 * Returns 0 on success, negative on failure.  A machine reporting FNum=0 is
 * success with count 0, not an error: fanless Macs exist.
 */
int fanpro_fan_enumerate(fanpro_smc_t *smc, fanpro_fan_set_t *set);

/* Re-read the mutable state (rpm, target, mode, Ftst) of an enumerated set. */
int fanpro_fan_refresh(fanpro_smc_t *smc, fanpro_fan_set_t *set);

/* Human-readable mode name; never NULL. */
const char *fanpro_fan_mode_name(int mode);

#endif /* FANPRO_FAN_H */
