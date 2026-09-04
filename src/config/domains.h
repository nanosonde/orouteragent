/* orouteragent - per-domain configuration transform tables
 *
 * Each native package owns one translation unit. A key that spans packages is
 * owned by the domain holding its primary section (for example `network` also
 * writes `dhcp` pools, but lives with the network domain).
 */
#ifndef ORA_CONFIG_DOMAINS_H
#define ORA_CONFIG_DOMAINS_H

#include <stddef.h>

#include "registry.h"

extern const struct ora_config_transform ora_config_transforms_network[];
extern const size_t ora_config_transforms_network_count;

extern const struct ora_config_transform ora_config_transforms_dhcp[];
extern const size_t ora_config_transforms_dhcp_count;

extern const struct ora_config_transform ora_config_transforms_firewall[];
extern const size_t ora_config_transforms_firewall_count;

extern const struct ora_config_transform ora_config_transforms_system[];
extern const size_t ora_config_transforms_system_count;

extern const struct ora_config_transform ora_config_transforms_dropbear[];
extern const size_t ora_config_transforms_dropbear_count;

#endif
