/*
 * fanpro - NVMe SMART health for the internal SSD.
 *
 * Reached through the IONVMeSMARTUserClient plugin interface, the same path
 * smartmontools uses on macOS.  The plugin is discovered at runtime and its
 * absence is reported, never fatal.
 *
 * Whether this needs root is deliberately not assumed: smartmontools reports
 * that the macOS SMART API does not, so the code attempts the read either
 * way and records what actually happened in `needed_root`.
 */
#include "fanpro/power.h"

#include "fanpro/log.h"

#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <string.h>
#include <unistd.h>

/* From IOKit/storage/nvme/NVMeSMARTLibExternal.h, which is not in the SDK. */
#define kIONVMeSMARTUserClientTypeID                                          \
	CFUUIDGetConstantUUIDWithBytes(NULL, 0xAA, 0x0F, 0xA6, 0xF9, 0xC2, 0xD6, \
	                               0x45, 0x7F, 0xB1, 0x0B, 0x59, 0xA1, 0x32, \
	                               0x53, 0x29, 0x2F)
#define kIONVMeSMARTInterfaceID                                               \
	CFUUIDGetConstantUUIDWithBytes(NULL, 0xCC, 0xD1, 0xDB, 0x19, 0xFD, 0x9A, \
	                               0x4D, 0xAF, 0xBF, 0x95, 0x12, 0x45, 0x4B, \
	                               0x23, 0x0A, 0xB6)

/* NVMe SMART / Health Information log page (log identifier 0x02). */
typedef struct {
	uint8_t  critical_warning;
	uint16_t temperature;            /* kelvin */
	uint8_t  available_spare;
	uint8_t  available_spare_threshold;
	uint8_t  percentage_used;
	uint8_t  reserved0[26];
	uint8_t  data_units_read[16];    /* 128-bit little-endian */
	uint8_t  data_units_written[16];
	uint8_t  host_read_commands[16];
	uint8_t  host_write_commands[16];
	uint8_t  controller_busy_time[16];
	uint8_t  power_cycles[16];
	uint8_t  power_on_hours[16];
	uint8_t  unsafe_shutdowns[16];
	uint8_t  media_errors[16];
	uint8_t  error_info_log_entries[16];
	uint8_t  reserved1[320];
} __attribute__((packed)) nvme_smart_log_t;

typedef struct IONVMeSMARTInterface {
	IUNKNOWN_C_GUTS;
	IOReturn (*SMARTReadData)(void *, nvme_smart_log_t *);
	IOReturn (*GetIdentifyData)(void *, void *, unsigned int);
	IOReturn (*GetFieldCounters)(void *, void *);
} IONVMeSMARTInterface;

/* The 128-bit counters never approach 2^64 in practice; take the low half. */
static uint64_t
le128_low64(const uint8_t b[16])
{
	uint64_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | b[i];
	return v;
}

int
fanpro_disk_health_read(fanpro_disk_health_t *out)
{
	io_iterator_t iter = IO_OBJECT_NULL;
	io_object_t service;
	kern_return_t kr;
	int rc = -1;

	if (out == NULL)
		return -1;

	memset(out, 0, sizeof(*out));

	kr = IOServiceGetMatchingServices(kIOMainPortDefault,
	                                  IOServiceMatching("IONVMeController"),
	                                  &iter);
	if (kr != KERN_SUCCESS || iter == IO_OBJECT_NULL) {
		FANPRO_DEBUG("disk.smart", "reason=no_nvme_controller kr=0x%08x", kr);
		return -2;
	}

	while ((service = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
		IOCFPlugInInterface **plugin = NULL;
		IONVMeSMARTInterface **smart = NULL;
		nvme_smart_log_t log;
		SInt32 score = 0;
		HRESULT hr;

		kr = IOCreatePlugInInterfaceForService(service,
		                                       kIONVMeSMARTUserClientTypeID,
		                                       kIOCFPlugInInterfaceID,
		                                       &plugin, &score);
		if (kr != KERN_SUCCESS || plugin == NULL) {
			/* Two distinct failures worth telling apart: a privilege
			 * problem the user can fix with sudo, and a controller
			 * that simply has no SMART user client, which is the case
			 * on Apple's internal ANS parts. */
			if (kr == kIOReturnNotPrivileged && geteuid() != 0)
				out->needed_root = true;
			else if (kr == kIOReturnUnsupported)
				out->smart_unsupported = true;
			FANPRO_DEBUG("disk.smart", "reason=no_plugin kr=0x%08x", kr);
			IOObjectRelease(service);
			continue;
		}

		hr = (*plugin)->QueryInterface(plugin,
		                               CFUUIDGetUUIDBytes(kIONVMeSMARTInterfaceID),
		                               (LPVOID *)&smart);
		IODestroyPlugInInterface(plugin);

		if (hr != S_OK || smart == NULL) {
			FANPRO_DEBUG("disk.smart", "reason=no_interface");
			IOObjectRelease(service);
			continue;
		}

		memset(&log, 0, sizeof(log));
		kr = (*smart)->SMARTReadData(smart, &log);
		if (kr == KERN_SUCCESS) {
			/* NVMe reports temperature in kelvin. */
			out->celsius = (double)log.temperature - 273.15;
			out->percent_used = log.percentage_used;
			out->critical_warning = log.critical_warning;
			out->power_on_hours = le128_low64(log.power_on_hours);
			out->unsafe_shutdowns = le128_low64(log.unsafe_shutdowns);
			out->data_units_read = le128_low64(log.data_units_read);
			out->data_units_written = le128_low64(log.data_units_written);
			out->available = true;
			rc = 0;

			FANPRO_DEBUG("disk.smart",
			             "temp_c=%.1f used_pct=%u poh=%llu unsafe=%llu root=%d",
			             out->celsius, out->percent_used,
			             (unsigned long long)out->power_on_hours,
			             (unsigned long long)out->unsafe_shutdowns,
			             (int)(geteuid() == 0));
		} else {
			if (kr == kIOReturnNotPrivileged && geteuid() != 0)
				out->needed_root = true;
			FANPRO_DEBUG("disk.smart", "reason=read_failed kr=0x%08x", kr);
		}

		(*smart)->Release(smart);
		IOObjectRelease(service);

		if (rc == 0)
			break; /* internal controller is enough */
	}

	IOObjectRelease(iter);
	return rc;
}
