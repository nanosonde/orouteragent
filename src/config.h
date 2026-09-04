/* orouteragent - UCI configuration */
#ifndef ORA_CONFIG_H
#define ORA_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "model/profiles.h"

enum ora_discovery_mode {
    ORA_DISC_AUTO = 0,
    ORA_DISC_UNICAST,
    ORA_DISC_BROADCAST,
};

struct ora_portmap_entry {
    int index;                  /* emulated port number */
    char interface[32];         /* real OpenWrt interface name */
};

struct ora_config {
    bool enabled;
    const struct ora_model_profile *profile;
    bool profile_is_default;    /* UCI model unknown -> fell back to ER707-M2 */

    char hw_version[16];
    char fw_version[64];
    char mac[24];               /* hyphenated uppercase; from UCI/board */
    bool mac_from_board;
    int wireless;               /* -1 = auto (from profile) */
    bool wireless_warned;       /* wifi model on non-wifi hardware */

    char controller[64];
    int controller_port;        /* HTTPS /api/info */
    enum ora_discovery_mode discovery_mode;
    bool verify_tls;

    char device_username[64];
    char device_password[64];
    char managed_username[64];
    char managed_password[64];

    int adopt_port;
    int inform_interval;

    char state_file[256];
    size_t max_config_kb;

    int log_level;

    struct ora_portmap_entry portmap[16];
    size_t n_portmap;
    char default_wan_if[32];
    char default_lan_if[32];
};

/* Load configuration from UCI (/etc/config/orouteragent).
 * Returns false on fatal errors (bad UCI file). Non-fatal issues
 * (unknown model, wifi model on non-wifi hardware) are logged and
 * defaulted. */
bool ora_config_load(struct ora_config *cfg);

/* Resolve the real interface for an emulated port. Falls back to the
 * devices backing the logical wan/lan interfaces. */
const char *ora_config_port_ifname(const struct ora_config *cfg, int emu_port);

/* Free dynamically allocated parts (currently none; symmetry). */
void ora_config_free(struct ora_config *cfg);

#endif