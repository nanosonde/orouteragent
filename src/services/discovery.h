/* orouteragent - UDP discovery service (port 29810) */
#ifndef ORA_DISCOVERY_H
#define ORA_DISCOVERY_H

#include <netinet/in.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../config.h"
#include "../state.h"

/* What the controller told us in its pre-adopt reply. */
struct ora_discovery_result {
    char controller[64];   /* controller address, learned from the peer */
    int adopt_port;        /* TLS management port to connect to */
};

/* Announce on UDP 29810 every 10s until the controller answers with a
 * PRE_ADOPT_REQUEST naming the management port, or *stop is set.
 *
 * The controller usually does not answer discovery at all - it connects
 * to the device on the management port once an operator adopts it. The
 * pre-adopt reply is the fast path. The device only ever sends its own
 * announce on this channel; it does not reply to the controller here. */
bool ora_discovery_run(const struct ora_config *cfg, struct ora_state *st,
                       volatile bool *stop, struct ora_discovery_result *out);

/* Build the DISCOVERY announce body (exposed for tests). */
json_object *ora_discovery_build_announce(const struct ora_config *cfg,
                                          struct ora_state *st);

/* Parse a controller datagram: fills @adopt_port when it is a
 * PRE_ADOPT_REQUEST. */
bool ora_discovery_parse_pre_adopt(const uint8_t *payload, size_t len,
                                   int *adopt_port);

#endif