/*
 * fanpro - leveled structured logging.
 *
 * One line per event, key=value fields, so daemon logs are greppable.
 * Foreground writes to stderr; under launchd the daemon redirects to
 * /var/log/fanpro/fanprod.log with size-based rotation.
 */
#ifndef FANPRO_LOG_H
#define FANPRO_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
	FANPRO_LOG_ERROR = 0,
	FANPRO_LOG_WARN,
	FANPRO_LOG_INFO,
	FANPRO_LOG_DEBUG,
	FANPRO_LOG_TRACE,
} fanpro_log_level_t;

void               fanpro_log_set_level(fanpro_log_level_t level);
fanpro_log_level_t fanpro_log_get_level(void);
int                fanpro_log_level_from_str(const char *s, fanpro_log_level_t *out);

/* Route output to a file with size-based rotation.  Passing NULL reverts to
 * stderr.  Returns 0 on success. */
int  fanpro_log_open_file(const char *path, size_t max_bytes, int keep);
void fanpro_log_close(void);

void fanpro_log(fanpro_log_level_t level, const char *event, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#define FANPRO_ERROR(event, ...) fanpro_log(FANPRO_LOG_ERROR, (event), __VA_ARGS__)
#define FANPRO_WARN(event, ...)  fanpro_log(FANPRO_LOG_WARN,  (event), __VA_ARGS__)
#define FANPRO_INFO(event, ...)  fanpro_log(FANPRO_LOG_INFO,  (event), __VA_ARGS__)
#define FANPRO_DEBUG(event, ...) fanpro_log(FANPRO_LOG_DEBUG, (event), __VA_ARGS__)
#define FANPRO_TRACE(event, ...) fanpro_log(FANPRO_LOG_TRACE, (event), __VA_ARGS__)

#endif /* FANPRO_LOG_H */
