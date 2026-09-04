/* orouteragent - bounds-checked growable buffer */
#ifndef ORA_BUF_H
#define ORA_BUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Growable byte buffer with bounds-checked append helpers. All protocol
 * frame/JSON assembly must go through these so overflow is impossible. */
struct ora_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool oom;
};

void ora_buf_init(struct ora_buf *b);
void ora_buf_free(struct ora_buf *b);
bool ora_buf_reserve(struct ora_buf *b, size_t extra);
void ora_buf_reset(struct ora_buf *b);

bool ora_buf_append(struct ora_buf *b, const void *src, size_t n);
bool ora_buf_append_str(struct ora_buf *b, const char *s);
bool ora_buf_append_byte(struct ora_buf *b, uint8_t c);
bool ora_buf_printf(struct ora_buf *b, const char *fmt, ...);
bool ora_buf_be32(struct ora_buf *b, uint32_t v);
bool ora_buf_be16(struct ora_buf *b, uint16_t v);

/* Append n zero bytes */
bool ora_buf_pad(struct ora_buf *b, size_t n);

#endif