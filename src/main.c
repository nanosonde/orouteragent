/* orouteragent - daemon entry point / orchestrator
 *
 * Main loop: load UCI -> load state -> [ discovery until adopted ->
 * management session ] repeat, with reconnect backoff. reload via
 * SIGHUP (re-read config), stop via SIGTERM/SIGINT.
 */
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "config.h"
#include "protocol/constants.h"
#include "protocol/message.h"
#include "services/capture.h"
#include "services/controller_info.h"
#include "services/discovery.h"
#include "services/dmp.h"
#include "services/manage.h"
#include "services/rtty.h"
#include "state.h"
#include "system_info.h"
#include "util.h"

static volatile bool g_stop = false;
static volatile bool g_reload = false;

static void on_signal(int sig)
{
    switch (sig) {
    case SIGTERM:
    case SIGINT:
        g_stop = true;
        break;
    case SIGHUP:
        g_reload = true;
        break;
    }
}

/* Resolve the controller address: UCI 'controller' if set; discovery
 * learns the address when broadcasting (stored via PRE_ADOPT peer).
 * Kept for phase 2: remember the discovery peer IP in state. */

int main(int argc, char **argv)
{
    struct ora_config cfg;
    struct ora_state st;
    struct sigaction sa;
    int rc = 0;

    (void)argc;
    (void)argv;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (!ora_config_load(&cfg)) {
        ora_log(ORA_LOG_ERR, "fatal: cannot load configuration");
        return 1;
    }
    ora_log_set_level(cfg.log_level);

    if (!cfg.enabled) {
        ora_log(ORA_LOG_INFO, "disabled (uci orouteragent.agent.enabled=0); exiting");
        return 0;
    }

    ora_log(ORA_LOG_INFO, "orouteragent starting: model %s hw %s fw %s mac %s",
            cfg.profile->model, cfg.hw_version, cfg.fw_version, cfg.mac);

    ora_state_init(&st, cfg.state_file, cfg.max_config_kb);
    ora_state_load(&st);
    ora_msg_seq_set((uint64_t)ora_now_ms() & 0xFFFFFFFFull);

    while (!g_stop) {
        if (g_reload) {
            g_reload = false;
            struct ora_config ncfg;
            if (ora_config_load(&ncfg) && ncfg.enabled) {
                ora_config_free(&cfg);
                cfg = ncfg;
                ora_log_set_level(cfg.log_level);
                ora_log(ORA_LOG_INFO, "configuration reloaded");
            } else {
                ora_log(ORA_LOG_WARN, "reload failed or agent disabled");
                if (!ncfg.enabled) {
                    ora_config_free(&ncfg);
                    break;
                }
            }
        }

        /* Phase A: announce until the controller answers with a
         * pre-adopt reply naming the management port. */
        {
            struct ora_discovery_result disc;
            struct ora_config session;

            if (!ora_discovery_run(&cfg, &st, &g_stop, &disc)) {
                /* An adopted agent resumes from UCI/State directly;
                 * false here just means no controller is known yet. */
                if (g_stop)
                    break;
                if (st.adopted && cfg.controller[0]) {
                    snprintf(disc.controller, sizeof(disc.controller),
                             "%s", cfg.controller);
                    disc.adopt_port = cfg.adopt_port;
                    ora_log(ORA_LOG_INFO,
                            "adopted; resuming management session with %s",
                            disc.controller);
                } else {
                    ora_log(ORA_LOG_WARN,
                            "discovery ended without adoption; retrying");
                    continue;
                }
            }
            if (g_stop)
                break;

            /* Phase B: management session against the controller that
             * answered (its address is authoritative even when none was
             * configured). */
            session = cfg;
            if (disc.controller[0])
                snprintf(session.controller, sizeof(session.controller), "%s",
                         disc.controller);
            if (disc.adopt_port > 0)
                session.adopt_port = disc.adopt_port;

            if (!session.controller[0]) {
                ora_log(ORA_LOG_ERR, "no controller address known; retrying");
                sleep(ORA_RECONNECT_DELAY_S);
                continue;
            }

            switch (ora_manage_run(&session, &st, &g_stop)) {
            case ORA_MANAGE_OK:
                ora_log(ORA_LOG_INFO, "management session ended; rediscovering");
                break;
            case ORA_MANAGE_RECONNECT:
                ora_log(ORA_LOG_WARN, "management session failed; retry in %ds",
                        ORA_RECONNECT_DELAY_S);
                sleep(ORA_RECONNECT_DELAY_S);
                break;
            case ORA_MANAGE_FATAL:
                ora_log(ORA_LOG_ERR, "fatal management error; exiting");
                rc = 1;
                goto out;
            }
            if (g_stop)
                break;
        }
    }
out:
    /* worker threads must be gone before the state they borrowed is */
    ora_rtty_stop();
    ora_dmp_stop();
    ora_capture_stop();
    ora_state_save(&st);
    ora_state_free(&st);
    ora_config_free(&cfg);
    ora_log(ORA_LOG_INFO, "orouteragent stopped");
    return rc;
}