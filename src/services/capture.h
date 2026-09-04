/* orouteragent - packet capture service (controller Tools -> Packet Capture)
 *
 * Flow (ordering is protocol-critical):
 *   1. SET packageCapture{operation:"start", nid, captureInfo}
 *   2. SET_RESPONSE acking packageCapture is sent immediately
 *   3. the capture runs for captureInfo.duration
 *   4. NOTIFY (subject 6) announces fileName/fileSize/md5 for the nid --
 *      without it the controller drops every partition it is sent
 *   5. the controller pushes transferChannel{...}; the device connects and
 *      handshakes on 29815 BEFORE that SET_RESPONSE goes out
 *   6. partitions are served as FILE_TRANSFER_RESPONSE_V2 on 29814
 */
#ifndef ORA_CAPTURE_H
#define ORA_CAPTURE_H

#include <json-c/json.h>
#include <stdbool.h>

#include "../config.h"

/* Handle a packageCapture SET value. Returns immediately; the capture
 * itself runs on a worker thread which announces the file when done. */
void ora_capture_handle_set(const struct ora_config *cfg, json_object *pc);

/* Serve one FILE_TRANSFER_REQUEST_V2 body (partition/startIndex/
 * endIndex). Returns false when nothing can be served. */
bool ora_capture_handle_transfer_request(json_object *req_body, int64_t seq);

/* Stop any running capture, join its worker, and drop the pending file.
 * Call before shutdown or before replacing the active capture. */
void ora_capture_stop(void);

#endif