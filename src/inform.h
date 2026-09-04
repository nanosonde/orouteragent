/* orouteragent - INFORM body builder */
#ifndef ORA_INFORM_H
#define ORA_INFORM_H

#include <json-c/json.h>

#include "config.h"
#include "state.h"

/* negotiation deviceInfo (subset of INFORM deviceInfo — INFORM-only
 * fields must NOT leak into negotiation). */
json_object *ora_inform_negotiation_device_info(const struct ora_config *cfg);

/* INFORM-only deviceInfo (sm/cerVer/ipv6List/fac/temp/fan/rps/txRate/
 * rxRate etc.). */
json_object *ora_inform_device_info(const struct ora_config *cfg);

/* Full INFORM body. Caller owns the returned object. */
json_object *ora_inform_build_body(const struct ora_config *cfg,
                                   struct ora_state *st);

/* Live answer for a GET key the controller queries directly
 * (arptable, dhcpClient, dnsCache, dpiProtocols, sessionLimit,
 * radioStatus). Returns NULL when @key is not one of them, in
 * which case the caller echoes the stored SET blob instead. */
json_object *ora_inform_get_key(const struct ora_config *cfg,
                                struct ora_state *st, const char *key);

#endif