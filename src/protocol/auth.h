/* orouteragent - adoption authentication and nonce generation */
#ifndef ORA_AUTH_H
#define ORA_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* auth = SHA256_UPPER( SHA256_UPPER( username + MD5_UPPER( password ) )
 *                      + random_key )
 * All intermediate digests are UPPERCASE hex strings. */
#define ORA_AUTH_HEX_LEN  64 /* SHA256 hex, uppercase */
#define ORA_MD5_HEX_LEN   32 /* MD5 hex, uppercase */
#define ORA_AUTH_BUF_LEN  65 /* auth hex + NUL */

/* Compute the auth token. @random_key is the controller's nonce.
 * Writes ORA_AUTH_BUF_LEN bytes to @out. Returns false on bad args. */
bool ora_auth_compute(char *out, size_t outsz,
                      const char *username, const char *password,
                      const char *random_key);

/* Compute auth when the controller supplied MD5_UPPER(password) as its
 * compatiblePassword value. */
bool ora_auth_compute_md5(char *out, size_t outsz,
                          const char *username, const char *password_md5,
                          const char *random_key);

/* Generate a random UUID v4 string (36 chars + NUL) using /dev/urandom. */
#define ORA_UUID_LEN 36
bool ora_auth_uuid_v4(char *out, size_t outsz);

/* Fill @n bytes with random bytes from /dev/urandom. */
bool ora_auth_random_bytes(void *out, size_t n);

#endif