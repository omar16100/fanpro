/*
 * fanpro - macOS thermal pressure level.
 *
 * The safety gate needs at least one signal that does not depend on fanpro's
 * own sensor sampling being correct.  This is it: macOS publishes its own
 * thermal verdict on a notify(3) key, so if our HID enumeration silently
 * starts returning stale values, this still tells us the machine is in
 * trouble.
 *
 * Pure C, no Objective-C.  NSProcessInfo exposes the same value, but reading
 * the notify state directly avoids linking Foundation into a root daemon.
 */
#include "fanpro/power.h"

#include "fanpro/log.h"
#include "fanpro/safety.h"

#include <notify.h>
#include <stdint.h>

/* kOSThermalNotificationPressureLevelName. */
#define THERMAL_PRESSURE_KEY "com.apple.system.thermalpressurelevel"

int
fanpro_thermal_pressure_read(void)
{
	static int token = -1;
	static bool registered = false;
	uint64_t state = 0;
	uint32_t rc;

	/* The token is registered once and reused: notify_register_check is
	 * comparatively expensive and the control loop calls this every tick. */
	if (!registered) {
		rc = notify_register_check(THERMAL_PRESSURE_KEY, &token);
		if (rc != NOTIFY_STATUS_OK) {
			FANPRO_WARN("thermal.register", "key=%s rc=%u",
			            THERMAL_PRESSURE_KEY, rc);
			return FANPRO_THERMAL_UNKNOWN;
		}
		registered = true;
		FANPRO_DEBUG("thermal.register", "key=%s ok", THERMAL_PRESSURE_KEY);
	}

	rc = notify_get_state(token, &state);
	if (rc != NOTIFY_STATUS_OK) {
		FANPRO_WARN("thermal.read", "rc=%u", rc);
		return FANPRO_THERMAL_UNKNOWN;
	}

	return fanpro_thermal_normalise((uint64_t)state);
}

/*
 * Normalise a raw notify value into fanpro's canonical scale.
 *
 * macOS 26.3 measured on this Mac Studio reports the compact enum: 0 nominal,
 * 1 light, 2 moderate, 3 heavy, 4 trapping, 5 sleeping.  Older systems report
 * 0/10/20/30/40.  Zero is nominal in both.  Exposed (not static) so the
 * mapping can be unit tested without a hot machine.
 */
int
fanpro_thermal_normalise(uint64_t raw)
{
	switch (raw) {
	case 0:  return FANPRO_THERMAL_NOMINAL;
	case 1:  return FANPRO_THERMAL_LIGHT;
	case 2:  return FANPRO_THERMAL_MODERATE;
	case 3:  return FANPRO_THERMAL_HEAVY;
	case 4:  return FANPRO_THERMAL_TRAPPING;
	case 5:  return FANPRO_THERMAL_SLEEPING;
	/* Legacy scale; 0 is handled above and means the same thing. */
	case 10: return FANPRO_THERMAL_MODERATE;
	case 20: return FANPRO_THERMAL_HEAVY;
	case 30: return FANPRO_THERMAL_TRAPPING;
	case 40: return FANPRO_THERMAL_SLEEPING;
	default:
		/* Unknown, never coerced: the safety gate compares this against
		 * a threshold, so a wrong guess either panics constantly or
		 * never panics at all. */
		FANPRO_WARN("thermal.read", "unexpected_level=%llu",
		            (unsigned long long)raw);
		return FANPRO_THERMAL_UNKNOWN;
	}
}

const char *
fanpro_thermal_level_name(int level)
{
	switch (level) {
	case FANPRO_THERMAL_NOMINAL:  return "nominal";
	case FANPRO_THERMAL_LIGHT:    return "light";
	case FANPRO_THERMAL_MODERATE: return "moderate";
	case FANPRO_THERMAL_HEAVY:    return "heavy";
	case FANPRO_THERMAL_TRAPPING: return "trapping";
	case FANPRO_THERMAL_SLEEPING: return "sleeping";
	default:                      return "unknown";
	}
}
