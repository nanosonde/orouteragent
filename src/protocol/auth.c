/* orouteragent - adoption authentication implementation */
#include "auth.h"

#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

static void hex_upper(char *out, size_t outsz, const uint8_t *digest, size_t dlen)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;

    if (outsz < dlen * 2 + 1)
        return;
    for (i = 0; i < dlen; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[dlen * 2] = '\0';
}

bool ora_auth_random_bytes(void *out, size_t n)
{
    uint8_t *p = out;
    size_t got = 0;

    while (got < n) {
        ssize_t r = getrandom(p + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            /* fall back to /dev/urandom */
            int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                return false;
            while (got < n) {
                ssize_t k = read(fd, p + got, n - got);
                if (k < 0) {
                    if (errno == EINTR)
                        continue;
                    close(fd);
                    return false;
                }
                got += (size_t)k;
            }
            close(fd);
            return true;
        }
        got += (size_t)r;
    }
    return true;
}

bool ora_auth_compute_md5(char *out, size_t outsz,
                          const char *username, const char *password_md5,
                          const char *random_key)
{
    char inner[ORA_MD5_HEX_LEN + ORA_AUTH_HEX_LEN + 128]; /* pw hex + name */
    unsigned char sha_inner[32];
    char sha_inner_hex[ORA_AUTH_HEX_LEN + 1];
    char concat[ORA_AUTH_HEX_LEN + 256];
    unsigned char sha_final[32];

    if (!out || !username || !password_md5 || !random_key)
        return false;
    if (outsz < ORA_AUTH_BUF_LEN)
        return false;

    /* 2. SHA256_UPPER(username + MD5_UPPER(password)) */
    if (snprintf(inner, sizeof(inner), "%s%s", username, password_md5) >= (int)sizeof(inner))
        return false;
    mbedtls_sha256((const unsigned char *)inner, strlen(inner), sha_inner, 0);
    hex_upper(sha_inner_hex, sizeof(sha_inner_hex), sha_inner, sizeof(sha_inner));

    /* 3. SHA256_UPPER(inner_hex + random_key) */
    if (snprintf(concat, sizeof(concat), "%s%s", sha_inner_hex, random_key) >= (int)sizeof(concat))
        return false;
    mbedtls_sha256((const unsigned char *)concat, strlen(concat), sha_final, 0);
    hex_upper(out, outsz, sha_final, sizeof(sha_final));
    return true;
}

bool ora_auth_compute(char *out, size_t outsz,
                      const char *username, const char *password,
                      const char *random_key)
{
    unsigned char md5_raw[16];
    char md5_hex[ORA_MD5_HEX_LEN + 1];

    if (!password)
        return false;
    mbedtls_md5((const unsigned char *)password, strlen(password), md5_raw);
    hex_upper(md5_hex, sizeof(md5_hex), md5_raw, sizeof(md5_raw));
    return ora_auth_compute_md5(out, outsz, username, md5_hex, random_key);
}

bool ora_auth_uuid_v4(char *out, size_t outsz)
{
    uint8_t b[16];

    if (!out || outsz < ORA_UUID_LEN + 1)
        return false;
    if (!ora_auth_random_bytes(b, sizeof(b)))
        return false;
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40); /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80); /* variant 10xx */
    snprintf(out, outsz,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return true;
}