/* orouteragent - shared small utilities: time, MAC handling, logging */
#ifndef ORA_UTIL_H
#define ORA_UTIL_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Stack size for the service threads. Each one only holds small frame
 * buffers, so this is set explicitly rather than inheriting whatever
 * the libc default happens to be. */
#define ORA_THREAD_STACK_SIZE (128 * 1024)

/* pthread_create with a bounded stack. Returns 0 on success. */
int ora_thread_create(pthread_t *thread, void *(*fn)(void *), void *arg);

/* Monotonic-ish wall time. */
uint64_t ora_now_ms(void);
uint64_t ora_now_s(void);

/* Uptime in seconds (from /proc/uptime). */
uint64_t ora_uptime_s(void);

/* Format uptime like the controller expects: "N days HH:MM:SS". */
void ora_format_uptime(uint64_t s, char *out, size_t outsz);

/* Normalize any MAC notation (":", "-", none, mixed case) to
 * hyphenated uppercase AA-BB-CC-DD-EE-FF. Returns false if not a MAC. */
bool ora_mac_normalize(const char *in, char *out, size_t outsz);

/* Increment the last octet pair of a hyphenated MAC (for
 * wanDefaultMacs). Overflow wraps within the last byte. */
void ora_mac_increment(const char *in, char *out, size_t outsz);

/* Parse "a.b.c.d" to uint32 (host order). */
bool ora_ip4_parse(const char *s, uint32_t *out);
/* Format uint32 (host order) to "a.b.c.d". */
void ora_ip4_format(uint32_t ip, char *out, size_t outsz);

/* Logging. Levels: 0=err 1=warn 2=info 3=debug. Goes to stderr (procd
 * forwards to syslog). */
enum {
    ORA_LOG_ERR = 0,
    ORA_LOG_WARN = 1,
    ORA_LOG_INFO = 2,
    ORA_LOG_DEBUG = 3,
};
void ora_log_set_level(int level);
int  ora_log_level(void);
void ora_log(int level, const char *fmt, ...);

#endif