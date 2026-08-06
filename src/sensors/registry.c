/*
 * fanpro - sensor registry and classification.
 *
 * Classification exists so the safety layer can apply the right panic
 * threshold: 95 C is unremarkable on SoC die and dangerous on NAND.  Sensor
 * names are machine-specific, so this matches on substrings that have proven
 * stable across Apple Silicon rather than on exact names, and anything
 * unrecognised falls through to OTHER rather than being dropped or, worse,
 * guessed into a class with a low threshold.
 */
#include "fanpro/sensors.h"

#include "fanpro/log.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const CLASS_NAME[FANPRO_CLASS_COUNT] = {
	"soc", "gpu", "nand", "ambient", "power", "other",
};

const char *
fanpro_sensor_class_name(fanpro_sensor_class_t cls)
{
	if (cls < 0 || cls >= FANPRO_CLASS_COUNT)
		return "other";
	return CLASS_NAME[cls];
}

/* Case-insensitive substring search; strcasestr is not in C11. */
static bool
contains_ci(const char *haystack, const char *needle)
{
	size_t hn, nn, i, j;

	if (haystack == NULL || needle == NULL)
		return false;
	hn = strlen(haystack);
	nn = strlen(needle);
	if (nn == 0 || nn > hn)
		return false;

	for (i = 0; i + nn <= hn; i++) {
		for (j = 0; j < nn; j++) {
			unsigned char a = (unsigned char)haystack[i + j];
			unsigned char b = (unsigned char)needle[j];

			if (a >= 'A' && a <= 'Z')
				a = (unsigned char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = (unsigned char)(b - 'A' + 'a');
			if (a != b)
				break;
		}
		if (j == nn)
			return true;
	}
	return false;
}

fanpro_sensor_class_t
fanpro_sensor_classify(const char *name)
{
	if (name == NULL)
		return FANPRO_CLASS_OTHER;

	/* NAND first: an SSD sensor may also mention the SoC package, and
	 * misclassifying it upward would apply a threshold 15 C too high. */
	if (contains_ci(name, "NAND") || contains_ci(name, "SSD") ||
	    contains_ci(name, "ANS") || contains_ci(name, "flash"))
		return FANPRO_CLASS_NAND;

	if (contains_ci(name, "GPU"))
		return FANPRO_CLASS_GPU;

	if (contains_ci(name, "SOC") || contains_ci(name, "CPU") ||
	    contains_ci(name, "pACC") || contains_ci(name, "eACC") ||
	    contains_ci(name, "PMGR") || contains_ci(name, "die") ||
	    contains_ci(name, "MTR"))
		return FANPRO_CLASS_SOC;

	if (contains_ci(name, "PMU") || contains_ci(name, "VRM") ||
	    contains_ci(name, "power") || contains_ci(name, "charger"))
		return FANPRO_CLASS_POWER;

	if (contains_ci(name, "ambient") || contains_ci(name, "airflow") ||
	    contains_ci(name, "inlet") || contains_ci(name, "TA0") ||
	    contains_ci(name, "enclosure"))
		return FANPRO_CLASS_AMBIENT;

	return FANPRO_CLASS_OTHER;
}

bool
fanpro_sensors_add(fanpro_sensor_set_t *set, const char *name, double celsius,
                   fanpro_sensor_source_t source)
{
	char unique[FANPRO_SENSOR_NAME_MAX];
	fanpro_sensor_t *s;
	int dup = 0, i;

	if (set == NULL || name == NULL || name[0] == '\0')
		return false;

	if (set->count >= FANPRO_MAX_SENSORS) {
		set->dropped++;
		return false;
	}

	/* An M3 Ultra is two dies and reports each die sensor twice under one
	 * name.  Both readings are real and can differ by several degrees, so
	 * they are kept and disambiguated rather than deduplicated away. */
	for (i = 0; i < set->count; i++) {
		size_t n = strlen(name);

		if (strncmp(set->sensors[i].name, name, n) == 0 &&
		    (set->sensors[i].name[n] == '\0' || set->sensors[i].name[n] == '#'))
			dup++;
	}

	if (dup == 0)
		snprintf(unique, sizeof(unique), "%s", name);
	else
		snprintf(unique, sizeof(unique), "%s#%d", name, dup + 1);

	s = &set->sensors[set->count++];
	memset(s, 0, sizeof(*s));
	snprintf(s->name, sizeof(s->name), "%s", unique);
	s->celsius = celsius;
	s->cls = fanpro_sensor_classify(name);
	s->source = source;
	s->valid = true;
	return true;
}

int
fanpro_sensors_enumerate(fanpro_sensor_set_t *set, fanpro_smc_t *smc,
                         bool force_smc)
{
	int hid, smc_n = -1;

	if (set == NULL)
		return -1;

	memset(set, 0, sizeof(*set));

	/* Providers are independent on purpose: HID disappearing on a macOS
	 * update should cost us the HID sensors, not the whole tool. */
	hid = fanpro_hid_temp_read(set);
	set->hid_available = hid >= 0;

	/* The SMC T* space is insurance, not a peer.  Consulting it only when
	 * HID came back empty keeps opaque, unvalidated keys away from the fan
	 * curves while still leaving fanpro able to see temperatures if HID
	 * enumeration ever changes underneath us. */
	if (smc != NULL && (force_smc || hid <= 0)) {
		smc_n = fanpro_smc_temp_read(set, smc);
		set->smc_used = smc_n > 0;
	}
	set->smc_available = smc != NULL;

	/* DEBUG, not INFO: the daemon calls this every tick and an INFO line
	 * per second would bury everything that matters in the log. */
	FANPRO_DEBUG("sensors.enumerate", "hid=%d smc=%s total=%d dropped=%d", hid,
	             (smc_n < 0) ? "not_consulted" : "used", set->count,
	             set->dropped);

	if (set->count == 0)
		FANPRO_WARN("sensors.enumerate", "reason=no_sensors_from_any_provider");
	if (set->dropped > 0)
		FANPRO_WARN("sensors.enumerate",
		            "reason=table_full dropped=%d cap=%d", set->dropped,
		            FANPRO_MAX_SENSORS);

	return 0;
}

int
fanpro_sensors_refresh(fanpro_sensor_set_t *set, fanpro_smc_t *smc,
                       bool force_smc)
{
	/* Sensors can appear and disappear (an external drive, a sleeping
	 * subsystem), so a refresh is a full re-enumeration rather than an
	 * in-place update against a stale name list. */
	return fanpro_sensors_enumerate(set, smc, force_smc);
}

double
fanpro_sensors_max_of_class(const fanpro_sensor_set_t *set,
                            fanpro_sensor_class_t cls)
{
	double best = NAN;
	int i;

	if (set == NULL)
		return NAN;

	for (i = 0; i < set->count; i++) {
		const fanpro_sensor_t *s = &set->sensors[i];

		if (!s->valid || s->cls != cls)
			continue;
		if (isnan(best) || s->celsius > best)
			best = s->celsius;
	}
	return best;
}

double
fanpro_sensors_max(const fanpro_sensor_set_t *set)
{
	double best = NAN;
	int i;

	if (set == NULL)
		return NAN;

	for (i = 0; i < set->count; i++) {
		const fanpro_sensor_t *s = &set->sensors[i];

		if (!s->valid)
			continue;
		if (isnan(best) || s->celsius > best)
			best = s->celsius;
	}
	return best;
}

double
fanpro_sensors_max_matching(const fanpro_sensor_set_t *set, const char *substr)
{
	double best = NAN;
	int i;

	if (set == NULL || substr == NULL)
		return NAN;

	for (i = 0; i < set->count; i++) {
		const fanpro_sensor_t *s = &set->sensors[i];

		if (!s->valid || !contains_ci(s->name, substr))
			continue;
		if (isnan(best) || s->celsius > best)
			best = s->celsius;
	}
	return best;
}

double
fanpro_sensors_avg_matching(const fanpro_sensor_set_t *set, const char *substr)
{
	double sum = 0.0;
	int n = 0, i;

	if (set == NULL || substr == NULL)
		return NAN;

	for (i = 0; i < set->count; i++) {
		const fanpro_sensor_t *s = &set->sensors[i];

		if (!s->valid || !contains_ci(s->name, substr))
			continue;
		sum += s->celsius;
		n++;
	}
	return n > 0 ? sum / (double)n : NAN;
}
