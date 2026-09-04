/* orouteragent - utility implementation */
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static int g_log_level = ORA_LOG_INFO;

void ora_log_set_level(int level)
{
    if (level >= ORA_LOG_ERR && level <= ORA_LOG_DEBUG)
        g_log_level = level;
}

int ora_log_level(void)
{
    return g_log_level;
}

uint64_t ora_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

uint64_t ora_now_s(void)
{
    return ora_now_ms() / 1000;
}

uint64_t ora_uptime_s(void)
{
    FILE *f = fopen("/proc/uptime", "re");
    double up = 0;

    if (!f)
        return 0;
    if (fscanf(f, "%lf", &up) != 1)
        up = 0;
    fclose(f);
    return (uint64_t)up;
}

void ora_format_uptime(uint64_t s, char *out, size_t outsz)
{
    unsigned days = (unsigned)(s / 86400);
    unsigned rem = (unsigned)(s % 86400);
    unsigned hh = rem / 3600;
    unsigned mm = (rem % 3600) / 60;
    unsigned ss = rem % 60;

    snprintf(out, outsz, "%u days %02u:%02u:%02u", days, hh, mm, ss);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ora_mac_normalize(const char *in, char *out, size_t outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    int nib[12];
    int n = 0;
    size_t i;

    if (!in || !out || outsz < 18)
        return false;
    for (i = 0; in[i] && n < 12; i++) {
        char c = in[i];
        if (c == ':' || c == '-' || c == '.' || c == ' ')
            continue;
        int v = hexval(c);
        if (v < 0)
            return false;
        nib[n++] = v;
    }
    if (n != 12 || in[i] != '\0')
        return false;
    for (i = 0; i < 6; i++) {
        out[i * 3 + 0] = hex[nib[i * 2]];
        out[i * 3 + 1] = hex[nib[i * 2 + 1]];
        out[i * 3 + 2] = '-';
    }
    out[17] = '\0';
    return true;
}

void ora_mac_increment(const char *in, char *out, size_t outsz)
{
    /* in/out are hyphenated uppercase MACs */
    uint8_t b[6];
    int i;

    if (!in || !out || outsz < 18)
        return;
    for (i = 0; i < 6; i++)
        b[i] = (uint8_t)(hexval(in[i * 3]) * 16 + hexval(in[i * 3 + 1]));
    b[5]++;
    for (i = 0; i < 6; i++) {
        out[i * 3 + 0] = "0123456789ABCDEF"[b[i] >> 4];
        out[i * 3 + 1] = "0123456789ABCDEF"[b[i] & 15];
        out[i * 3 + 2] = '-';
    }
    out[17] = '\0';
}

bool ora_ip4_parse(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    uint32_t ip;

    if (!s || !out)
        return false;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    ip = (a << 24) | (b << 16) | (c << 8) | d;
    *out = ip;
    return true;
}

void ora_ip4_format(uint32_t ip, char *out, size_t outsz)
{
    snprintf(out, outsz, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

int ora_thread_create(pthread_t *thread, void *(*fn)(void *), void *arg)
{
    pthread_attr_t attr;
    int rc;

    if (pthread_attr_init(&attr) != 0)
        return -1;
    if (pthread_attr_setstacksize(&attr, ORA_THREAD_STACK_SIZE) != 0) {
        pthread_attr_destroy(&attr);
        return -1;
    }
    rc = pthread_create(thread, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

void ora_log(int level, const char *fmt, ...)
{
    static const int syslog_level[] = {
        LOG_ERR, LOG_WARNING, LOG_INFO, LOG_DEBUG
    };
    static bool opened;
    va_list ap;

    if (level < ORA_LOG_ERR || level > ORA_LOG_DEBUG)
        level = ORA_LOG_INFO;
    if (level > g_log_level)
        return;

    if (!opened) {
        openlog("orouteragent", LOG_PID, LOG_DAEMON);
        opened = true;
    }
    va_start(ap, fmt);
    vsyslog(syslog_level[level], fmt, ap);
    va_end(ap);

    /* Also echo to the terminal when run interactively; under procd
     * stderr is not a tty and syslog already has the message. */
    if (isatty(STDERR_FILENO)) {
        static const char *names[] = { "ERR", "WARN", "INFO", "DEBUG" };

        fprintf(stderr, "%s: ", names[level]);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fprintf(stderr, "\n");
    }
}