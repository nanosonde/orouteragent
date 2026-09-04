/* orouteragent - RTTY terminal service (TLS 29816)
 *
 * Backs the controller's Tools -> Terminal: each controller LOGIN opens a
 * real login shell on a PTY, and terminal I/O is relayed in both
 * directions until LOGOUT.
 */
#ifndef ORA_RTTY_H
#define ORA_RTTY_H

#include <stdbool.h>

#include "../config.h"

/* Settings pushed by the controller in a terminalSetting SET. */
struct ora_rtty_settings {
    bool enable;
    char token[128];
    char host[64];      /* controller address (from the management peer) */
    int port;           /* default 29816 */
    bool ssl;
};

/* Start/stop the RTTY client thread. Starting when already running
 * updates the token, which takes effect on the next reconnect. Stop joins
 * the persistent connection worker and must run before process shutdown. */
bool ora_rtty_start(const struct ora_config *cfg,
                    const struct ora_rtty_settings *set);
void ora_rtty_stop(void);
bool ora_rtty_running(void);

#endif