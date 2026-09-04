/* orouteragent - controller /api/info lookup (HTTPS 8043) */
#ifndef ORA_CONTROLLER_INFO_H
#define ORA_CONTROLLER_INFO_H

#include <stdbool.h>
#include <stddef.h>

/* Fetch GET /api/info and extract result.omadacId.
 * Returns false on network/parse errors. On success @omadac_id is a
 * NUL-terminated string (max 39 chars). */
bool ora_controller_info_get(const char *host, int port, bool verify_tls,
                             char *omadac_id, size_t outsz);

#endif