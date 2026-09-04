/* orouteragent - 4-byte BE length prefix framing */
#ifndef ORA_FRAMING_H
#define ORA_FRAMING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "buf.h"

/* ECSP framing: | u32 BE payload length | payload bytes | */

/* Encode a full frame (length prefix + payload) into @out. */
bool ora_frame_encode(struct ora_buf *out, const void *payload, size_t len);

/* Reader state for a stream (TCP). Append incoming bytes via
 * ora_frame_reader_feed(), then pull complete frames out.
 * Accepts frames up to max_frame bytes of payload. */
#define ORA_MAX_FRAME_PAYLOAD (256 * 1024)

struct ora_frame_reader {
    struct ora_buf in;      /* pending bytes */
    uint32_t declared;      /* declared payload length of frame in progress */
    bool have_declared;
    size_t max_frame;
};

void ora_frame_reader_init(struct ora_frame_reader *r, size_t max_frame);
void ora_frame_reader_free(struct ora_frame_reader *r);

/* Feed raw stream bytes. Returns false on OOM. */
bool ora_frame_reader_feed(struct ora_frame_reader *r, const void *data, size_t n);

/* Try to extract one complete frame.
 * Returns: 1 frame available (payload set, consumed from stream),
 *          0 need more data,
 *         -1 protocol error (oversized frame) - reader must be reset. */
int ora_frame_reader_next(struct ora_frame_reader *r,
                          const uint8_t **payload, size_t *len);

/* Drop the frame most recently returned by ora_frame_reader_next(). */
void ora_frame_reader_consume(struct ora_frame_reader *r);

/* Datagram mode: parse a single self-contained datagram.
 * Returns payload length or -1 on malformed input. */
ssize_t ora_frame_decode_datagram(const void *data, size_t n,
                                  const uint8_t **payload, size_t *len);

#endif