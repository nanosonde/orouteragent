/* orouteragent - fuzz driver for the network-facing decode paths
 *
 * The daemon feeds these three decoders raw bytes from a network peer,
 * so none of them may crash, hang or read out of bounds on arbitrary
 * input. libFuzzer is not available in this toolchain, so this is a
 * self-contained driver: it mixes purely random buffers with mutated
 * copies of valid frames (the interesting cases are usually almost
 * valid) and runs everything under ASan/UBSan.
 *
 * Deterministic by default so a failure can be reproduced from the seed
 * printed on start.
 */
#include "base64.h"
#include "buf.h"
#include "dmp_proto.h"
#include "framing.h"
#include "rtty_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t rng_state;

static uint32_t rnd(void)
{
    /* xorshift64*: small, deterministic, good enough to shake out parsers */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 2685821657736338717ull) >> 32);
}

static size_t rnd_below(size_t n)
{
    return n ? (size_t)(rnd() % n) : 0;
}

/* ---- corpus of structurally valid frames to mutate ---- */

static void seed_ecsp_frame(struct ora_buf *b)
{
    const char *body = "{\"header\":{\"mac\":\"AA-BB-CC-DD-EE-FF\",\"type\":256},"
                       "\"body\":{\"deviceInfo\":{}}}";

    ora_buf_reset(b);
    ora_frame_encode(b, body, strlen(body));
}

static void seed_rtty_frame(struct ora_buf *b)
{
    char sid[ORA_RTTY_SID_LEN + 1];
    uint8_t req[16];

    memset(sid, 'a', ORA_RTTY_SID_LEN);
    sid[ORA_RTTY_SID_LEN] = '\0';
    memset(req, 0x11, sizeof(req));

    ora_buf_reset(b);
    switch (rnd_below(4)) {
    case 0:
        ora_rtty_pack_register(b, ORA_RTTY_MIN_VERSION, "AA-BB-CC-DD-EE-FF",
                               "ER605 (gateway)", "token");
        break;
    case 1:
        ora_rtty_pack_termdata(b, sid, "uname -a\n", 9);
        break;
    case 2:
        ora_rtty_pack_tcpdata(b, 3, req, "data", 4);
        break;
    default:
        ora_rtty_pack_heartbeat(b, 1234);
        break;
    }
}

static void seed_dmp_message(struct ora_buf *b)
{
    struct ora_dmp_header h;

    memset(&h, 0, sizeof(h));
    memcpy(h.mac, "\xAA\xBB\xCC\xDD\xEE\xFF", 6);
    snprintf(h.token, sizeof(h.token), "token");
    snprintf(h.path, sizeof(h.path), "/ping");
    h.version = 1;
    h.msg_type = ORA_DMP_MSG_EMPTY;
    h.seq = 7;
    h.epoch_ms = 1730000000000ull;

    ora_buf_reset(b);
    ora_dmp_encode(b, &h, "{\"target\":\"8.8.8.8\"}", 20);
}

/* Flip bytes, truncate, extend: keep the shape roughly intact so the
 * decoders are exercised past their first length check. */
static void mutate(struct ora_buf *b)
{
    size_t rounds = 1 + rnd_below(6);
    size_t i;

    if (!b->len)
        return;
    for (i = 0; i < rounds; i++) {
        switch (rnd_below(4)) {
        case 0:
            b->data[rnd_below(b->len)] = (uint8_t)rnd();
            break;
        case 1: /* truncate */
            b->len = rnd_below(b->len);
            break;
        case 2: /* corrupt the length prefix specifically */
            if (b->len >= 4) {
                size_t off = rnd_below(4);

                b->data[off] = (uint8_t)rnd();
            }
            break;
        default: /* append junk */
            {
                uint8_t extra[32];
                size_t n = 1 + rnd_below(sizeof(extra));
                size_t j;

                for (j = 0; j < n; j++)
                    extra[j] = (uint8_t)rnd();
                ora_buf_append(b, extra, n);
            }
            break;
        }
    }
}

static void random_bytes(struct ora_buf *b)
{
    size_t n = rnd_below(512);
    size_t i;

    ora_buf_reset(b);
    for (i = 0; i < n; i++)
        ora_buf_append_byte(b, (uint8_t)rnd());
}

/* ---- targets ---- */

static void fuzz_framing(const uint8_t *data, size_t len)
{
    struct ora_frame_reader r;
    const uint8_t *payload;
    size_t plen;
    size_t off = 0;
    int guard = 0;

    /* feed in random-sized slices, mimicking TCP segmentation */
    ora_frame_reader_init(&r, ORA_MAX_FRAME_PAYLOAD);
    while (off < len && guard++ < 1000) {
        size_t chunk = 1 + rnd_below(64);

        if (chunk > len - off)
            chunk = len - off;
        if (!ora_frame_reader_feed(&r, data + off, chunk))
            break;
        off += chunk;
        for (;;) {
            int rv = ora_frame_reader_next(&r, &payload, &plen);

            if (rv != 1)
                break;
            ora_frame_reader_consume(&r);
        }
    }
    ora_frame_reader_free(&r);

    (void)ora_frame_decode_datagram(data, len, &payload, &plen);
}

static void fuzz_rtty(const uint8_t *data, size_t len)
{
    struct ora_rtty_reader r;
    const uint8_t *payload;
    size_t plen, off = 0;
    int type, guard = 0;

    ora_rtty_reader_init(&r);
    while (off < len && guard++ < 1000) {
        size_t chunk = 1 + rnd_below(64);

        if (chunk > len - off)
            chunk = len - off;
        if (!ora_rtty_reader_feed(&r, data + off, chunk))
            break;
        off += chunk;
        for (;;) {
            int rv = ora_rtty_reader_next(&r, &type, &payload, &plen);

            if (rv != 1)
                break;
            /* exercise the payload accessors the service layer uses */
            if (type == ORA_RTTY_REGISTER) {
                int err;
                char msg[128];

                (void)ora_rtty_parse_register_response(payload, plen, &err,
                                                       msg, sizeof(msg));
            } else {
                char sid[ORA_RTTY_SID_LEN + 1];

                (void)ora_rtty_parse_sid(payload, plen, sid, sizeof(sid));
            }
            ora_rtty_reader_consume(&r);
        }
    }
    ora_rtty_reader_free(&r);
}

static void fuzz_dmp(const uint8_t *data, size_t len)
{
    struct ora_dmp_header hdr;
    const uint8_t *body;
    size_t body_len;

    (void)ora_dmp_decode(data, len, &hdr, &body, &body_len);
}

static void fuzz_base64(const uint8_t *data, size_t len)
{
    uint8_t *out = malloc(len + 4);

    if (!out)
        return;
    (void)ora_base64_decode(out, len + 4, (const char *)data, len);
    free(out);
}

int main(int argc, char **argv)
{
    unsigned long iterations = 20000;
    struct ora_buf b;
    unsigned long i;

    rng_state = 0x2545F4914F6CDD1Dull;
    if (argc > 1)
        iterations = strtoul(argv[1], NULL, 10);
    if (argc > 2)
        rng_state = strtoull(argv[2], NULL, 0);
    if (!rng_state)
        rng_state = (uint64_t)time(NULL);

    printf("fuzzing decoders: %lu iterations, seed 0x%016llx\n",
           iterations, (unsigned long long)rng_state);

    ora_buf_init(&b);
    for (i = 0; i < iterations; i++) {
        switch (i % 4) {
        case 0:
            seed_ecsp_frame(&b);
            mutate(&b);
            fuzz_framing(b.data, b.len);
            break;
        case 1:
            seed_rtty_frame(&b);
            mutate(&b);
            fuzz_rtty(b.data, b.len);
            break;
        case 2:
            seed_dmp_message(&b);
            mutate(&b);
            fuzz_dmp(b.data, b.len);
            break;
        default:
            random_bytes(&b);
            fuzz_framing(b.data, b.len);
            fuzz_rtty(b.data, b.len);
            fuzz_dmp(b.data, b.len);
            fuzz_base64(b.data, b.len);
            break;
        }
    }
    ora_buf_free(&b);

    printf("fuzz: %lu iterations completed without a crash\n", iterations);
    return 0;
}