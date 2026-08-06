/*
 * fanpro - temperature sensors via IOHIDEventSystemClient.
 *
 * This is the primary source on Apple Silicon.  Matching on the Apple vendor
 * usage page yields one HID service per thermal sensor, each carrying a
 * Product string and a temperature event in degrees Celsius.
 *
 * Two things this must not do:
 *
 *   - hardcode sensor names.  They are FourCC-derived and differ between
 *     machines that share an SoC, so we enumerate and classify by pattern.
 *   - trust every service to answer.  IOHIDServiceClientCopyEvent returns
 *     NULL for some services and 0.0 for others that are simply not
 *     populated; both must be dropped rather than fed to a fan curve.
 */
#include "fanpro/sensors.h"

#include "fanpro/log.h"
#include "fanpro/private_apis.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Plausible range for a real silicon or board sensor, in Celsius.  Readings
 * outside this are stale or unpopulated channels, not cold hardware. */
#define TEMP_MIN_PLAUSIBLE (-40.0)
#define TEMP_MAX_PLAUSIBLE 150.0

static CFDictionaryRef
create_matching(uint32_t page, uint32_t usage)
{
	CFMutableDictionaryRef dict;
	CFNumberRef page_num, usage_num;

	dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 2,
	                                 &kCFTypeDictionaryKeyCallBacks,
	                                 &kCFTypeDictionaryValueCallBacks);
	if (dict == NULL)
		return NULL;

	page_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &page);
	usage_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &usage);
	if (page_num == NULL || usage_num == NULL) {
		if (page_num != NULL)
			CFRelease(page_num);
		if (usage_num != NULL)
			CFRelease(usage_num);
		CFRelease(dict);
		return NULL;
	}

	CFDictionarySetValue(dict, CFSTR("PrimaryUsagePage"), page_num);
	CFDictionarySetValue(dict, CFSTR("PrimaryUsage"), usage_num);
	CFRelease(page_num);
	CFRelease(usage_num);
	return dict;
}

/* Copy a service's Product string into `out`.  Returns false when unnamed. */
static bool
service_name(IOHIDServiceClientRef svc, char *out, size_t out_len)
{
	CFTypeRef prop;
	bool ok = false;

	prop = IOHIDServiceClientCopyProperty(svc, CFSTR("Product"));
	if (prop == NULL)
		return false;

	if (CFGetTypeID(prop) == CFStringGetTypeID()) {
		ok = CFStringGetCString((CFStringRef)prop, out, (CFIndex)out_len,
		                        kCFStringEncodingUTF8);
	}
	CFRelease(prop);
	return ok && out[0] != '\0';
}

int
fanpro_hid_temp_read(fanpro_sensor_set_t *set)
{
	IOHIDEventSystemClientRef client;
	CFDictionaryRef matching;
	CFArrayRef services;
	CFIndex n, i;
	int added = 0;

	if (set == NULL)
		return -1;

	client = IOHIDEventSystemClientCreate(kCFAllocatorDefault);
	if (client == NULL) {
		FANPRO_WARN("sensors.hid", "reason=client_create_failed");
		return -1;
	}

	matching = create_matching(FANPRO_HID_PAGE_APPLE_VENDOR,
	                           FANPRO_HID_USAGE_TEMPERATURE_SENSOR);
	if (matching == NULL) {
		CFRelease(client);
		return -1;
	}

	IOHIDEventSystemClientSetMatching(client, matching);
	CFRelease(matching);

	services = IOHIDEventSystemClientCopyServices(client);
	if (services == NULL) {
		FANPRO_WARN("sensors.hid", "reason=no_services");
		CFRelease(client);
		return -1;
	}

	n = CFArrayGetCount(services);
	for (i = 0; i < n; i++) {
		IOHIDServiceClientRef svc =
		    (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
		IOHIDEventRef event;
		char name[FANPRO_SENSOR_NAME_MAX];
		double celsius;

		if (svc == NULL)
			continue;
		if (!service_name(svc, name, sizeof(name)))
			continue;

		event = IOHIDServiceClientCopyEvent(svc,
		                                    FANPRO_HID_EVENT_TYPE_TEMPERATURE,
		                                    0, 0);
		if (event == NULL)
			continue;

		celsius = IOHIDEventGetFloatValue(
		    event, (int32_t)FANPRO_HID_EVENT_FIELD_BASE(
		               FANPRO_HID_EVENT_TYPE_TEMPERATURE));
		CFRelease(event);

		/* Unpopulated channels report 0.0 or nonsense.  Feeding those to
		 * a fan curve would look like a very cold machine. */
		if (!isfinite(celsius) || celsius <= TEMP_MIN_PLAUSIBLE ||
		    celsius >= TEMP_MAX_PLAUSIBLE || celsius == 0.0) {
			FANPRO_TRACE("sensors.hid.skip", "name=%s value=%.3f", name,
			             celsius);
			continue;
		}

		if (fanpro_sensors_add(set, name, celsius, FANPRO_SRC_HID))
			added++;
	}

	CFRelease(services);
	CFRelease(client);

	FANPRO_DEBUG("sensors.hid", "services=%ld accepted=%d", (long)n, added);
	if (set->dropped > 0)
		FANPRO_WARN("sensors.hid", "reason=table_full dropped=%d",
		            set->dropped);
	return added;
}
