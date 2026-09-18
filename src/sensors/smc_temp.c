/*
 * fanpro - temperature sensors from the SMC T* key space.
 *
 * Secondary provider.  On Apple Silicon the HID path carries far more
 * sensors and better names, so this exists as insurance: if a macOS update
 * changes HID enumeration, fanpro still reports temperatures rather than
 * going blind.  On Intel it would be the primary source.
 *
 * Keys are discovered by walking the key space rather than from a list, for
 * the same reason sensor names are not hardcoded elsewhere: the T* namespace
 * differs between machines that share a chip.
 */
#include "fanpro/sensors.h"

#include "fanpro/log.h"
#include "fanpro/smc_codec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEMP_MIN_PLAUSIBLE (-40.0)
#define TEMP_MAX_PLAUSIBLE 150.0

/*
 * A firmware placeholder, not a measurement.  Measured on Mac15,14 (Mac
 * Studio M3 Ultra): Ta09, Ta0H, Ta0L, Ta0P and ftA0 all return the identical
 * flt payload 00 00 20 41, which decodes exactly to 10.0.  flt-to-double is
 * exact, so comparing the decoded value is the same test as comparing those
 * four bytes, with less code.
 *
 * This matters beyond a cosmetic wrong reading.  registry.c classifies Ta0*
 * as the ambient class, and safety.c skips a class whose reading is not
 * finite but honours one that is.  Admitting a constant 10.0 would turn
 * panic_ambient_c into a guard that is permanently 60 C clear of its own
 * threshold: worse than having no ambient sensor, because it looks live.
 *
 * Dropping a genuine 10.0 C reading costs nothing by comparison: it is far
 * below every panic threshold, and the class falls back to fail-open.
 */
#define TEMP_FIRMWARE_PLACEHOLDER 10.0

/* Would this reading already have come from the HID provider? */
static bool
already_present(const fanpro_sensor_set_t *set, const char *name)
{
	int i;

	for (i = 0; i < set->count; i++) {
		if (strcmp(set->sensors[i].name, name) == 0)
			return true;
	}
	return false;
}

int
fanpro_smc_temp_read(fanpro_sensor_set_t *set, fanpro_smc_t *smc)
{
	uint32_t count = 0, i;
	int added = 0;

	if (set == NULL || smc == NULL)
		return -1;

	if (fanpro_smc_key_count(smc, &count) != FANPRO_SMC_SUCCESS) {
		FANPRO_DEBUG("sensors.smc", "reason=no_key_count");
		return -1;
	}

	for (i = 0; i < count; i++) {
		fanpro_smc_value_t v;
		char name[5];
		uint32_t key = 0;

		if (fanpro_smc_key_at(smc, i, &key) != FANPRO_SMC_SUCCESS)
			continue;

		/* The T namespace is the thermal one by convention. */
		if (((key >> 24) & 0xff) != (uint32_t)'T')
			continue;

		if (fanpro_smc_read(smc, key, &v) != FANPRO_SMC_SUCCESS)
			continue;
		if (!v.numeric)
			continue;
		if (!isfinite(v.num) || v.num <= TEMP_MIN_PLAUSIBLE ||
		    v.num >= TEMP_MAX_PLAUSIBLE || v.num == 0.0)
			continue;

		fanpro_fourcc_str(key, name);

		if (v.num == TEMP_FIRMWARE_PLACEHOLDER) {
			FANPRO_DEBUG("sensors.smc",
			             "key=%s reason=firmware_placeholder value=%.1f",
			             name, v.num);
			continue;
		}
		if (already_present(set, name))
			continue;

		if (fanpro_sensors_add(set, name, v.num, FANPRO_SRC_SMC))
			added++;
	}

	FANPRO_DEBUG("sensors.smc", "keys=%u accepted=%d", count, added);
	return added;
}
