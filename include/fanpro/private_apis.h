/*
 * fanpro - declarations for Apple SPI that has no public header.
 *
 * Every symbol here is exported from a system framework but absent from the
 * SDK, so we declare it ourselves.  Verified exported from IOKit.framework on
 * macOS 26.3 (Mac15,14).  Two rules keep this contained:
 *
 *   1. Nothing outside src/sensors/ includes this header.
 *   2. Anything that might vanish across a macOS update is reached by dlopen
 *      rather than by linking, so its absence degrades a feature instead of
 *      breaking the process.  IOReport is in that category; the IOHID event
 *      system client symbols are stable enough to link directly.
 */
#ifndef FANPRO_PRIVATE_APIS_H
#define FANPRO_PRIVATE_APIS_H

#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>

/* ---- IOHIDEventSystemClient (IOKit.framework, SPI) ---------------------- */

typedef struct __IOHIDEvent             *IOHIDEventRef;
typedef struct __IOHIDServiceClient     *IOHIDServiceClientRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;

/*
 * Apple vendor HID usages, from IOHIDFamily/AppleHIDUsageTables.h.
 * Temperature sensors live on page 0xff00 usage 0x0005.
 */
#define FANPRO_HID_PAGE_APPLE_VENDOR        0xff00
#define FANPRO_HID_USAGE_TEMPERATURE_SENSOR 0x0005

/* kIOHIDEventTypeTemperature, from IOHIDEventTypes.h. */
#define FANPRO_HID_EVENT_TYPE_TEMPERATURE 15

/* Event fields are (type << 16) | index. */
#define FANPRO_HID_EVENT_FIELD_BASE(type) ((uint32_t)(type) << 16)

IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(CFAllocatorRef allocator);
int                       IOHIDEventSystemClientSetMatching(IOHIDEventSystemClientRef client,
                                                            CFDictionaryRef matching);
CFArrayRef                IOHIDEventSystemClientCopyServices(IOHIDEventSystemClientRef client);
CFTypeRef                 IOHIDServiceClientCopyProperty(IOHIDServiceClientRef service,
                                                         CFStringRef key);
IOHIDEventRef             IOHIDServiceClientCopyEvent(IOHIDServiceClientRef service,
                                                      int64_t type, int32_t options,
                                                      int64_t timestamp);
double                    IOHIDEventGetFloatValue(IOHIDEventRef event, int32_t field);

#endif /* FANPRO_PRIVATE_APIS_H */
