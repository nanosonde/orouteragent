/* orouteragent - Device Monitor Platform service (TLS 29817)
 *
 * Backs the controller's Tools -> Network Check: the controller sends
 * ping/traceroute probes, the agent runs them for real and replies with
 * the measured results.
 */
#ifndef ORA_DMP_H
#define ORA_DMP_H

#include <stdbool.h>

#include "../config.h"

/* Settings pushed by the controller in a monitorServer SET. */
struct ora_dmp_settings {
    char token[128];
    char host[64];
    int port;           /* default 29817 */
    char path[128];     /* register path, default "/" */
    bool tls;
};

bool ora_dmp_start(const struct ora_config *cfg,
                   const struct ora_dmp_settings *set);
void ora_dmp_stop(void);
bool ora_dmp_running(void);

#endif