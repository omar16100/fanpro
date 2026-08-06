/*
 * fanpro - fan enumeration and state.
 *
 * Everything here is discovery.  We ask the firmware how many fans there
 * are, which spelling of the mode key it answers to, and whether the
 * diagnostic unlock key exists at all, then read state through those keys.
 * No generation is special-cased in code; the probe results carry it.
 */
#include "fanpro/fan.h"

#include "fanpro/log.h"
#include "fanpro/smc_codec.h"

#include <string.h>

static uint32_t
build_key(int index, char c2, char c3)
{
	char name[4];

	name[0] = 'F';
	name[1] = (char)('0' + index);
	name[2] = c2;
	name[3] = c3;
	return fanpro_fourcc(name);
}

const char *
fanpro_fan_mode_name(int mode)
{
	switch (mode) {
	case FANPRO_FAN_MODE_AUTO:   return "auto";
	case FANPRO_FAN_MODE_MANUAL: return "manual";
	case FANPRO_FAN_MODE_SYSTEM: return "system";
	default:                     return "unknown";
	}
}

/* Read a numeric key, leaving *out untouched unless the read fully succeeds. */
static bool
read_num(fanpro_smc_t *smc, uint32_t key, double *out)
{
	fanpro_smc_value_t v;

	if (fanpro_smc_read(smc, key, &v) != FANPRO_SMC_SUCCESS)
		return false;
	if (!v.numeric)
		return false;
	*out = v.num;
	return true;
}

static bool
key_exists(fanpro_smc_t *smc, uint32_t key)
{
	fanpro_smc_key_info_t info;

	return fanpro_smc_key_info(smc, key, &info) == FANPRO_SMC_SUCCESS;
}

/*
 * Which spelling of the mode key does this firmware use?
 * M5 (Mac17,7) answers to F%dmd; everything older to F%dMd.  Probing beats
 * a model-name table that goes stale with every new Mac.
 */
static bool
probe_mode_case(fanpro_smc_t *smc, bool *lowercase)
{
	if (key_exists(smc, build_key(0, 'M', 'd'))) {
		*lowercase = false;
		return true;
	}
	if (key_exists(smc, build_key(0, 'm', 'd'))) {
		*lowercase = true;
		return true;
	}
	return false;
}

int
fanpro_fan_enumerate(fanpro_smc_t *smc, fanpro_fan_set_t *set)
{
	double count = 0.0;
	int i;

	if (smc == NULL || set == NULL)
		return -1;

	memset(set, 0, sizeof(*set));
	set->ftst = -1;

	if (!read_num(smc, fanpro_fourcc("FNum"), &count)) {
		FANPRO_ERROR("fan.enumerate", "reason=no_FNum");
		return -2;
	}

	set->count = (int)count;
	if (set->count < 0)
		set->count = 0;
	if (set->count > FANPRO_MAX_FANS) {
		FANPRO_WARN("fan.enumerate", "FNum=%d clamped=%d", set->count,
		            FANPRO_MAX_FANS);
		set->count = FANPRO_MAX_FANS;
	}

	if (set->count > 0 && !probe_mode_case(smc, &set->lowercase_mode)) {
		/* Reads still work; only manual control is off the table. */
		FANPRO_WARN("fan.enumerate", "reason=no_mode_key fans=%d",
		            set->count);
	}

	set->has_ftst = key_exists(smc, fanpro_fourcc("Ftst"));

	for (i = 0; i < set->count; i++) {
		fanpro_fan_t *f = &set->fans[i];

		f->index = i;
		f->mode = FANPRO_FAN_MODE_UNKNOWN;
		f->key_ac = build_key(i, 'A', 'c');
		f->key_mn = build_key(i, 'M', 'n');
		f->key_mx = build_key(i, 'M', 'x');
		f->key_tg = build_key(i, 'T', 'g');
		f->key_md = build_key(i, set->lowercase_mode ? 'm' : 'M', 'd');

		/* Latched once, here.  Everything on the release path gates on
		 * this rather than on a live read, so a momentary read failure
		 * can never cause a pinned fan to be skipped. */
		f->mode_key_known = key_exists(smc, f->key_md);

		/* Bounds are fixed per fan, so read them once here.  The safety
		 * layer clamps against these, and a fan whose limits we could
		 * not read must never be driven. */
		if (!read_num(smc, f->key_mn, &f->min_rpm))
			f->min_rpm = -1.0;
		if (!read_num(smc, f->key_mx, &f->max_rpm))
			f->max_rpm = -1.0;
	}

	FANPRO_INFO("fan.enumerate", "count=%d mode_key=%s ftst=%d", set->count,
	            set->lowercase_mode ? "F%dmd" : "F%dMd", (int)set->has_ftst);

	return fanpro_fan_refresh(smc, set);
}

int
fanpro_fan_refresh(fanpro_smc_t *smc, fanpro_fan_set_t *set)
{
	double v;
	int i;

	if (smc == NULL || set == NULL)
		return -1;

	for (i = 0; i < set->count; i++) {
		fanpro_fan_t *f = &set->fans[i];

		if (!read_num(smc, f->key_ac, &f->rpm))
			f->rpm = -1.0;

		f->has_target = read_num(smc, f->key_tg, &f->target);
		if (!f->has_target)
			f->target = -1.0;

		f->mode_readable = read_num(smc, f->key_md, &v);
		f->mode = f->mode_readable ? (int)v : FANPRO_FAN_MODE_UNKNOWN;
	}

	if (set->has_ftst && read_num(smc, fanpro_fourcc("Ftst"), &v))
		set->ftst = (int)v;
	else
		set->ftst = -1;

	return 0;
}
