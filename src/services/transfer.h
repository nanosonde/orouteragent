/* orouteragent - file-transfer channel client (TLS 29815)
 *
 * The controller pushes a transferChannel SET (port/token/aesKey/iv);
 * the device must connect and complete the channel handshake BEFORE the
 * corresponding SET_RESPONSE is sent, otherwise the controller starts
 * requesting partitions on a channel that does not exist yet.
 */
#ifndef ORA_TRANSFER_H
#define ORA_TRANSFER_H

#include <json-c/json.h>
#include <stdbool.h>

#include "../config.h"

/* Connect + handshake synchronously, then keep the channel open on a
 * worker thread. Returns true once the channel is established. */
bool ora_transfer_open(const struct ora_config *cfg, json_object *tc);

/* Stop the persistent channel and join its worker before shutdown. */
void ora_transfer_stop(void);
bool ora_transfer_running(void);

#endif