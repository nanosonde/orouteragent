/* orouteragent - unit tests for the network-facing decoders
 *
 * These decoders parse bytes straight off the wire, so the cases below
 * concentrate on framing boundaries, truncation and malformed input
 * rather than happy paths alone. All vectors are static and derived
 * from the protocol specification.
 */
#include "harness.h"

#include "base64.h"
#include "buf.h"
#include "dmp_proto.h"
#include "framing.h"
#include "rtty_proto.h"

/* ---------------- buf ---------------- */

TEST(buf_grows_and_preserves_content)
{
    struct ora_buf b;
    size_t i;

    ora_buf_init(&b);
    for (i = 0; i < 5000; i++)
        CHECK(ora_buf_append_byte(&b, (uint8_t)(i & 0xFF)), "append %zu", i);
    CHECK_EQ_INT(b.len, 5000, "length after appends");
    for (i = 0; i < 5000; i++)
        CHECK_EQ_INT(b.data[i], (uint8_t)(i & 0xFF), "byte %zu", i);
    ora_buf_free(&b);
}

TEST(buf_be_helpers_are_big_endian)
{
    struct ora_buf b;
    const uint8_t want32[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    const uint8_t want16[2] = { 0x12, 0x34 };

    ora_buf_init(&b);
    CHECK(ora_buf_be32(&b, 0xDEADBEEFu), "be32");
    CHECK_EQ_MEM(b.data, want32, 4, "be32 byte order");
    ora_buf_reset(&b);
    CHECK(ora_buf_be16(&b, 0x1234), "be16");
    CHECK_EQ_MEM(b.data, want16, 2, "be16 byte order");
    ora_buf_free(&b);
}

TEST(buf_printf_handles_growth)
{
    struct ora_buf b;
    char expect[1200];
    size_t i;

    ora_buf_init(&b);
    for (i = 0; i < 100; i++)
        CHECK(ora_buf_printf(&b, "%08zu:", i), "printf %zu", i);
    for (i = 0; i < 100; i++)
        snprintf(expect + i * 9, 10, "%08zu:", i);
    CHECK_EQ_INT(b.len, 900, "printf total length");
    CHECK_EQ_MEM(b.data, expect, 900, "printf content");
    ora_buf_free(&b);
}

/* ---------------- framing ---------------- */

TEST(framing_prefix_is_4_byte_big_endian)
{
    struct ora_buf out;
    const char *payload = "hello";

    ora_buf_init(&out);
    CHECK(ora_frame_encode(&out, payload, 5), "encode");
    CHECK_EQ_INT(out.len, 9, "frame length");
    CHECK_EQ_INT(out.data[0], 0, "len byte 0");
    CHECK_EQ_INT(out.data[1], 0, "len byte 1");
    CHECK_EQ_INT(out.data[2], 0, "len byte 2");
    CHECK_EQ_INT(out.data[3], 5, "len byte 3");
    CHECK_EQ_MEM(out.data + 4, payload, 5, "payload");
    ora_buf_free(&out);
}

TEST(framing_reassembles_across_split_feeds)
{
    struct ora_frame_reader r;
    struct ora_buf out;
    const uint8_t *payload;
    size_t len, i;

    ora_buf_init(&out);
    ora_frame_encode(&out, "abcdefghij", 10);
    ora_frame_reader_init(&r, ORA_MAX_FRAME_PAYLOAD);

    /* one byte at a time: nothing must be returned until complete */
    for (i = 0; i + 1 < out.len; i++) {
        CHECK(ora_frame_reader_feed(&r, out.data + i, 1), "feed %zu", i);
        CHECK_EQ_INT(ora_frame_reader_next(&r, &payload, &len), 0,
                     "incomplete after %zu bytes", i + 1);
    }
    CHECK(ora_frame_reader_feed(&r, out.data + out.len - 1, 1), "feed last");
    CHECK_EQ_INT(ora_frame_reader_next(&r, &payload, &len), 1, "frame ready");
    CHECK_EQ_INT(len, 10, "payload length");
    CHECK_EQ_MEM(payload, "abcdefghij", 10, "payload content");

    ora_frame_reader_consume(&r);
    CHECK_EQ_INT(ora_frame_reader_next(&r, &payload, &len), 0, "no second frame");
    ora_frame_reader_free(&r);
    ora_buf_free(&out);
}

TEST(framing_handles_multiple_frames_in_one_feed)
{
    struct ora_frame_reader r;
    struct ora_buf out;
    const uint8_t *payload;
    size_t len;

    ora_buf_init(&out);
    ora_frame_encode(&out, "one", 3);
    ora_frame_encode(&out, "two", 3);
    ora_frame_encode(&out, "three", 5);

    ora_frame_reader_init(&r, ORA_MAX_FRAME_PAYLOAD);
    CHECK(ora_frame_reader_feed(&r, out.data, out.len), "feed all");

    CHECK_EQ_INT(ora_frame_reader_next(&r, &payload, &len), 1, "frame 1");
    CHECK_EQ_MEM(payload, "one", 3, "frame 1 content");
    ora_frame_reader_consume(&r);
    CHECK_EQ_INT(ora_frame_reader_next(&r, &payload, &len), 1, "frame 2");
    CHECK_EQ_MEM(payload, "two", 3, "frame 2 content");
    ora_frame_reader_consume(&r);
    CHECK_EQ_INT(ora_frame_reader_next(&r, &payload, &len), 1, "frame 3");
    CHECK_EQ_MEM(payload, "three", 5, "frame 3 content");
    ora_frame_reader_consume(&r);
    CHECK_EQ_INT(ora_frame_reader_next(&r, &payload, &len), 0, "drained");

    ora_frame_reader_free(&r);
    ora_buf_free(&out);
}

TEST(framing_rejects_oversized_declared_length)
{
    struct ora_frame_reader r;
    const uint8_t *payload;
    size_t len;
    /* declares 0xFFFFFFFF bytes: must be refused, not trusted */
    const uint8_t hdr[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

    ora_frame_reader_init(&r, ORA_MAX_FRAME_PAYLOAD);
    CHECK(ora_frame_reader_feed(&r, hdr, sizeof(hdr)), "feed header");
    CHECK_EQ_INT(ora_frame_reader_next(&r, &payload, &len), -1, "oversized rejected");
    ora_frame_reader_free(&r);
}

TEST(framing_accepts_exactly_max_payload)
{
    struct ora_frame_reader r;
    const uint8_t *payload;
    size_t len;
    uint8_t hdr[4];
    uint8_t *body;
    const size_t max = 4096;

    hdr[0] = (uint8_t)(max >> 24);
    hdr[1] = (uint8_t)(max >> 16);
    hdr[2] = (uint8_t)(max >> 8);
    hdr[3] = (uint8_t)max;
    body = calloc(1, max);
    CHECK(body != NULL, "alloc");

    ora_frame_reader_init(&r, max);
    CHECK(ora_frame_reader_feed(&r, hdr, 4), "feed header");
    CHECK(ora_frame_reader_feed(&r, body, max), "feed body");
    CHECK_EQ_INT(ora_frame_reader_next(&r, &payload, &len), 1, "boundary accepted");
    CHECK_EQ_INT(len, max, "boundary length");
    ora_frame_reader_free(&r);
    free(body);
}

TEST(framing_datagram_rejects_short_and_truncated)
{
    const uint8_t *payload;
    size_t len;
    const uint8_t too_short[3] = { 0, 0, 0 };
    const uint8_t truncated[6] = { 0, 0, 0, 10, 'a', 'b' }; /* claims 10, has 2 */
    const uint8_t good[7] = { 0, 0, 0, 3, 'a', 'b', 'c' };

    CHECK_EQ_INT(ora_frame_decode_datagram(too_short, sizeof(too_short),
                                           &payload, &len), -1, "short header");
    CHECK_EQ_INT(ora_frame_decode_datagram(truncated, sizeof(truncated),
                                           &payload, &len), -1, "truncated body");
    CHECK_EQ_INT(ora_frame_decode_datagram(good, sizeof(good), &payload, &len),
                 3, "valid datagram");
    CHECK_EQ_MEM(payload, "abc", 3, "datagram payload");
}

/* ---------------- RTTY ---------------- */

TEST(rtty_v1_and_v2_headers_have_the_right_widths)
{
    struct ora_buf out;

    /* V1: type + uint16 length */
    ora_buf_init(&out);
    CHECK(ora_rtty_pack(&out, ORA_RTTY_TERMDATA, "xy", 2), "pack v1");
    CHECK_EQ_INT(out.len, 5, "v1 frame size");
    CHECK_EQ_INT(out.data[0], ORA_RTTY_TERMDATA, "v1 type");
    CHECK_EQ_INT(out.data[1], 0, "v1 len hi");
    CHECK_EQ_INT(out.data[2], 2, "v1 len lo");

    /* V2: type + uint32 length */
    ora_buf_reset(&out);
    CHECK(ora_rtty_pack(&out, ORA_RTTY_TCPDATA, "xy", 2), "pack v2");
    CHECK_EQ_INT(out.len, 7, "v2 frame size");
    CHECK_EQ_INT(out.data[0], ORA_RTTY_TCPDATA, "v2 type");
    CHECK_EQ_INT(out.data[4], 2, "v2 len low byte");
    ora_buf_free(&out);
}

TEST(rtty_message_types_are_classified_correctly)
{
    CHECK(ora_rtty_is_v1(ORA_RTTY_REGISTER), "REGISTER is v1");
    CHECK(ora_rtty_is_v1(ORA_RTTY_LOGIN), "LOGIN is v1");
    CHECK(ora_rtty_is_v1(ORA_RTTY_TERMDATA), "TERMDATA is v1");
    CHECK(ora_rtty_is_v1(ORA_RTTY_WINSIZE), "WINSIZE is v1");
    CHECK(ora_rtty_is_v1(ORA_RTTY_HEARTBEAT), "HEARTBEAT is v1");
    CHECK(ora_rtty_is_v1(ORA_RTTY_ACK), "ACK is v1");
    CHECK(!ora_rtty_is_v1(ORA_RTTY_TCPDATA), "TCPDATA is v2");
    CHECK(!ora_rtty_is_v1(ORA_RTTY_HTTPSDATA), "HTTPSDATA is v2");
    CHECK(!ora_rtty_is_v1(ORA_RTTY_TUNNEL_ADD), "TUNNEL_ADD is v2");
    CHECK(!ora_rtty_is_v1(ORA_RTTY_STANDALONE_AUTH), "STANDALONE_AUTH is v2");
}

/* The controller splits the REGISTER tail on NUL and requires exactly
 * four segments; an extra trailing NUL makes it drop the connection. */
TEST(rtty_register_payload_has_exactly_four_nul_segments)
{
    struct ora_buf out;
    const uint8_t *p;
    size_t len, i, nuls = 0;

    ora_buf_init(&out);
    CHECK(ora_rtty_pack_register(&out, ORA_RTTY_MIN_VERSION, "AA-BB-CC-DD-EE-FF",
                                 "ER605 (gateway)", "tok123"), "pack register");
    p = out.data + 3; /* skip V1 header */
    len = out.len - 3;

    CHECK_EQ_INT(p[0], ORA_RTTY_MIN_VERSION, "version byte");
    for (i = 1; i < len; i++)
        if (p[i] == 0)
            nuls++;
    CHECK_EQ_INT(nuls, 3, "three NUL terminators => four split segments");
    CHECK_EQ_INT(p[len - 1], 0, "payload ends with a single NUL");
    CHECK_EQ_INT(p[len - 2] != 0, 1, "no double NUL at the end");
    ora_buf_free(&out);
}

/* An empty heartbeat payload makes the controller throw and drop the
 * channel, so it must always carry the 4-byte uptime. */
TEST(rtty_heartbeat_payload_is_four_bytes_big_endian)
{
    struct ora_buf out;

    ora_buf_init(&out);
    CHECK(ora_rtty_pack_heartbeat(&out, 0x01020304u), "pack heartbeat");
    CHECK_EQ_INT(out.len, 3 + 4, "heartbeat frame size");
    CHECK_EQ_INT(out.data[2], 4, "declared payload length");
    CHECK_EQ_INT(out.data[3], 0x01, "uptime byte 0");
    CHECK_EQ_INT(out.data[4], 0x02, "uptime byte 1");
    CHECK_EQ_INT(out.data[5], 0x03, "uptime byte 2");
    CHECK_EQ_INT(out.data[6], 0x04, "uptime byte 3");
    ora_buf_free(&out);
}

TEST(rtty_sid_bearing_frames_require_a_full_session_id)
{
    struct ora_buf out;
    char sid[ORA_RTTY_SID_LEN + 1];

    memset(sid, 'a', ORA_RTTY_SID_LEN);
    sid[ORA_RTTY_SID_LEN] = '\0';

    ora_buf_init(&out);
    CHECK(ora_rtty_pack_login_response(&out, sid, ORA_RTTY_LOGIN_OK), "login resp");
    CHECK_EQ_INT(out.len, 3 + ORA_RTTY_SID_LEN + 1, "login response size");
    CHECK_EQ_INT(out.data[out.len - 1], ORA_RTTY_LOGIN_OK, "login code");
    ora_buf_reset(&out);

    /* a short sid must be refused rather than padded or truncated */
    CHECK(!ora_rtty_pack_login_response(&out, "tooshort", 0), "short sid refused");
    ora_buf_free(&out);
}

TEST(rtty_parse_sid_rejects_short_payloads)
{
    char sid[ORA_RTTY_SID_LEN + 1];
    uint8_t payload[ORA_RTTY_SID_LEN];

    memset(payload, 'b', sizeof(payload));
    CHECK(!ora_rtty_parse_sid(payload, 4, sid, sizeof(sid)), "4 bytes rejected");
    CHECK(!ora_rtty_parse_sid(payload, ORA_RTTY_SID_LEN - 1, sid, sizeof(sid)),
          "31 bytes rejected");
    CHECK(ora_rtty_parse_sid(payload, ORA_RTTY_SID_LEN, sid, sizeof(sid)),
          "32 bytes accepted");
    CHECK_EQ_INT(strlen(sid), ORA_RTTY_SID_LEN, "sid is NUL terminated");
}

TEST(rtty_reader_rejects_oversized_v2_frame)
{
    struct ora_rtty_reader r;
    int type;
    const uint8_t *payload;
    size_t len;
    const uint8_t hdr[5] = { ORA_RTTY_TCPDATA, 0xFF, 0xFF, 0xFF, 0xFF };

    ora_rtty_reader_init(&r);
    CHECK(ora_rtty_reader_feed(&r, hdr, sizeof(hdr)), "feed");
    CHECK_EQ_INT(ora_rtty_reader_next(&r, &type, &payload, &len), -1,
                 "oversized v2 rejected");
    ora_rtty_reader_free(&r);
}

TEST(rtty_reader_round_trips_mixed_frames)
{
    struct ora_rtty_reader r;
    struct ora_buf out;
    char sid[ORA_RTTY_SID_LEN + 1];
    uint8_t req[16];
    int type;
    const uint8_t *payload;
    size_t len;

    memset(sid, 'c', ORA_RTTY_SID_LEN);
    sid[ORA_RTTY_SID_LEN] = '\0';
    memset(req, 0x5A, sizeof(req));

    ora_buf_init(&out);
    ora_rtty_pack_termdata(&out, sid, "ls -l\n", 6);      /* V1 */
    ora_rtty_pack_tcpdata(&out, 7, req, "payload", 7);    /* V2 */
    ora_rtty_pack_heartbeat(&out, 42);                    /* V1 */

    ora_rtty_reader_init(&r);
    CHECK(ora_rtty_reader_feed(&r, out.data, out.len), "feed all");

    CHECK_EQ_INT(ora_rtty_reader_next(&r, &type, &payload, &len), 1, "frame 1");
    CHECK_EQ_INT(type, ORA_RTTY_TERMDATA, "frame 1 type");
    CHECK_EQ_INT(len, ORA_RTTY_SID_LEN + 6, "frame 1 length");
    CHECK_EQ_MEM(payload + ORA_RTTY_SID_LEN, "ls -l\n", 6, "frame 1 data");
    ora_rtty_reader_consume(&r);

    CHECK_EQ_INT(ora_rtty_reader_next(&r, &type, &payload, &len), 1, "frame 2");
    CHECK_EQ_INT(type, ORA_RTTY_TCPDATA, "frame 2 type");
    CHECK_EQ_INT(payload[0], 7, "tunnel id");
    CHECK_EQ_INT(len, 1 + 16 + 7, "frame 2 length");
    ora_rtty_reader_consume(&r);

    CHECK_EQ_INT(ora_rtty_reader_next(&r, &type, &payload, &len), 1, "frame 3");
    CHECK_EQ_INT(type, ORA_RTTY_HEARTBEAT, "frame 3 type");
    ora_rtty_reader_consume(&r);

    CHECK_EQ_INT(ora_rtty_reader_next(&r, &type, &payload, &len), 0, "drained");
    ora_rtty_reader_free(&r);
    ora_buf_free(&out);
}

TEST(rtty_register_response_parse)
{
    const uint8_t ok[] = { 0, 'O', 'K' };
    const uint8_t bad[] = { 1, 'I', 'n', 'v', 'a', 'l', 'i', 'd' };
    int err = -1;
    char msg[64];

    CHECK(ora_rtty_parse_register_response(ok, sizeof(ok), &err, msg, sizeof(msg)),
          "parse ok");
    CHECK_EQ_INT(err, ORA_RTTY_REGISTER_OK, "ok code");
    CHECK_EQ_STR(msg, "OK", "ok message");

    CHECK(ora_rtty_parse_register_response(bad, sizeof(bad), &err, msg, sizeof(msg)),
          "parse reject");
    CHECK_EQ_INT(err, 1, "reject code");
    CHECK_EQ_STR(msg, "Invalid", "reject message");

    CHECK(!ora_rtty_parse_register_response(ok, 0, &err, msg, sizeof(msg)),
          "empty payload rejected");
}

/* ---------------- DMP protobuf ---------------- */

TEST(dmp_varint_round_trips)
{
    static const uint64_t values[] = {
        0, 1, 127, 128, 300, 16383, 16384, 0xFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull
    };
    size_t i;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        struct ora_buf b;
        size_t off = 0;
        uint64_t got = 0;

        ora_buf_init(&b);
        CHECK(ora_pb_put_varint(&b, 1, values[i]), "encode %zu", i);
        off = 1; /* skip the tag byte */
        CHECK(ora_pb_get_varint(b.data, b.len, &off, &got), "decode %zu", i);
        CHECK_EQ_INT(got == values[i], 1, "value %zu round trip", i);
        ora_buf_free(&b);
    }
}

TEST(dmp_varint_rejects_truncated_input)
{
    /* continuation bit set on the final byte: no terminator follows */
    const uint8_t truncated[] = { 0x80, 0x80, 0x80 };
    size_t off = 0;
    uint64_t v;

    CHECK(!ora_pb_get_varint(truncated, sizeof(truncated), &off, &v),
          "truncated varint rejected");
}

TEST(dmp_fixed64_is_little_endian)
{
    struct ora_buf b;

    ora_buf_init(&b);
    CHECK(ora_pb_put_fixed64(&b, 10, 0x0102030405060708ull), "put fixed64");
    CHECK_EQ_INT(b.len, 9, "tag + 8 bytes");
    CHECK_EQ_INT(b.data[1], 0x08, "least significant byte first");
    CHECK_EQ_INT(b.data[8], 0x01, "most significant byte last");
    ora_buf_free(&b);
}

TEST(dmp_message_round_trips)
{
    struct ora_dmp_header in, out;
    struct ora_buf b;
    const uint8_t *data;
    size_t data_len;
    const uint8_t mac[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };

    memset(&in, 0, sizeof(in));
    memcpy(in.mac, mac, sizeof(mac));
    snprintf(in.token, sizeof(in.token), "tok-abc");
    snprintf(in.path, sizeof(in.path), "/ping");
    in.version = 1;
    in.msg_type = ORA_DMP_MSG_JSON_COMPONENT_LIST;
    in.seq = 12345;
    in.dev_type = 2;
    in.error_code = 0;
    in.need_reply = 1;
    in.epoch_ms = 1730000000000ull;
    in.content_type = 3;

    ora_buf_init(&b);
    CHECK(ora_dmp_encode(&b, &in, "{\"a\":1}", 7), "encode");
    CHECK(ora_dmp_decode(b.data, b.len, &out, &data, &data_len), "decode");

    CHECK_EQ_MEM(out.mac, mac, 6, "mac");
    CHECK_EQ_STR(out.token, "tok-abc", "token");
    CHECK_EQ_STR(out.path, "/ping", "path");
    CHECK_EQ_INT(out.msg_type, ORA_DMP_MSG_JSON_COMPONENT_LIST, "msgType");
    CHECK_EQ_INT(out.seq, 12345, "seq");
    CHECK_EQ_INT(out.need_reply, 1, "needReply");
    CHECK_EQ_INT(out.epoch_ms == 1730000000000ull, 1, "epochMs");
    CHECK_EQ_INT(out.content_type, 3, "contentType");
    CHECK_EQ_INT(data_len, 7, "data length");
    CHECK_EQ_MEM(data, "{\"a\":1}", 7, "data content");
    ora_buf_free(&b);
}

TEST(dmp_decode_rejects_truncated_and_headerless_messages)
{
    struct ora_dmp_header hdr;
    struct ora_buf b;
    const uint8_t *data;
    size_t data_len;
    struct ora_dmp_header in;

    /* a length-delimited field claiming more bytes than are present */
    const uint8_t lying_length[] = { 0x0A, 0x7F, 0x01, 0x02 };

    CHECK(!ora_dmp_decode(lying_length, sizeof(lying_length), &hdr,
                          &data, &data_len), "over-long field rejected");

    /* well formed protobuf, but carrying no header submessage */
    ora_buf_init(&b);
    CHECK(ora_pb_put_bytes(&b, 2, "orphan", 6), "encode data only");
    CHECK(!ora_dmp_decode(b.data, b.len, &hdr, &data, &data_len),
          "message without header rejected");
    ora_buf_free(&b);

    /* truncating a valid message at every offset must never be accepted
     * as a complete header, and must never crash */
    memset(&in, 0, sizeof(in));
    in.msg_type = ORA_DMP_MSG_EMPTY;
    snprintf(in.path, sizeof(in.path), "/");
    ora_buf_init(&b);
    CHECK(ora_dmp_encode(&b, &in, NULL, 0), "encode empty");
    {
        size_t cut;

        for (cut = 0; cut < b.len; cut++)
            (void)ora_dmp_decode(b.data, cut, &hdr, &data, &data_len);
    }
    ora_buf_free(&b);
}

/* ---------------- base64 ---------------- */

TEST(base64_matches_rfc4648_vectors)
{
    static const struct {
        const char *plain;
        const char *encoded;
    } vectors[] = {
        { "", "" },
        { "f", "Zg==" },
        { "fo", "Zm8=" },
        { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" },
        { "fooba", "Zm9vYmE=" },
        { "foobar", "Zm9vYmFy" },
    };
    size_t i;

    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        char enc[32];
        uint8_t dec[32];
        size_t plain_len = strlen(vectors[i].plain);
        ssize_t dlen;

        CHECK_EQ_INT(ora_base64_encode(enc, sizeof(enc), vectors[i].plain,
                                       plain_len),
                     strlen(vectors[i].encoded), "encoded length %zu", i);
        CHECK_EQ_STR(enc, vectors[i].encoded, "vector %zu encoding", i);

        dlen = ora_base64_decode(dec, sizeof(dec), vectors[i].encoded,
                                 strlen(vectors[i].encoded));
        CHECK_EQ_INT(dlen, (ssize_t)plain_len, "decoded length %zu", i);
        if (plain_len)
            CHECK_EQ_MEM(dec, vectors[i].plain, plain_len, "vector %zu decoding", i);
    }
}

TEST(base64_round_trips_binary_data)
{
    uint8_t plain[256];
    char enc[512];
    uint8_t dec[256];
    size_t i;
    ssize_t dlen;

    for (i = 0; i < sizeof(plain); i++)
        plain[i] = (uint8_t)i;

    CHECK(ora_base64_encode(enc, sizeof(enc), plain, sizeof(plain)) > 0, "encode");
    dlen = ora_base64_decode(dec, sizeof(dec), enc, strlen(enc));
    CHECK_EQ_INT(dlen, (ssize_t)sizeof(plain), "decoded length");
    CHECK_EQ_MEM(dec, plain, sizeof(plain), "round trip");
}

TEST(base64_decode_rejects_malformed_input)
{
    uint8_t dec[64];

    CHECK_EQ_INT(ora_base64_decode(dec, sizeof(dec), "Zm9vYmF", 7), -1,
                 "length not a multiple of four");
    CHECK_EQ_INT(ora_base64_decode(dec, sizeof(dec), "Zm9v!!==", 8), -1,
                 "invalid characters");
    CHECK_EQ_INT(ora_base64_decode(dec, sizeof(dec), "Zg==Zg==", 8), -1,
                 "data after padding");
    CHECK_EQ_INT(ora_base64_decode(dec, 1, "Zm9vYmFy", 8), -1,
                 "output buffer too small");
}

TEST(base64_encode_refuses_undersized_output)
{
    char small[4];

    CHECK_EQ_INT(ora_base64_encode(small, sizeof(small), "foobar", 6), 0,
                 "undersized output refused");
}

int main(void)
{
    RUN_TEST(buf_grows_and_preserves_content);
    RUN_TEST(buf_be_helpers_are_big_endian);
    RUN_TEST(buf_printf_handles_growth);

    RUN_TEST(framing_prefix_is_4_byte_big_endian);
    RUN_TEST(framing_reassembles_across_split_feeds);
    RUN_TEST(framing_handles_multiple_frames_in_one_feed);
    RUN_TEST(framing_rejects_oversized_declared_length);
    RUN_TEST(framing_accepts_exactly_max_payload);
    RUN_TEST(framing_datagram_rejects_short_and_truncated);

    RUN_TEST(rtty_v1_and_v2_headers_have_the_right_widths);
    RUN_TEST(rtty_message_types_are_classified_correctly);
    RUN_TEST(rtty_register_payload_has_exactly_four_nul_segments);
    RUN_TEST(rtty_heartbeat_payload_is_four_bytes_big_endian);
    RUN_TEST(rtty_sid_bearing_frames_require_a_full_session_id);
    RUN_TEST(rtty_parse_sid_rejects_short_payloads);
    RUN_TEST(rtty_reader_rejects_oversized_v2_frame);
    RUN_TEST(rtty_reader_round_trips_mixed_frames);
    RUN_TEST(rtty_register_response_parse);

    RUN_TEST(dmp_varint_round_trips);
    RUN_TEST(dmp_varint_rejects_truncated_input);
    RUN_TEST(dmp_fixed64_is_little_endian);
    RUN_TEST(dmp_message_round_trips);
    RUN_TEST(dmp_decode_rejects_truncated_and_headerless_messages);

    RUN_TEST(base64_matches_rfc4648_vectors);
    RUN_TEST(base64_round_trips_binary_data);
    RUN_TEST(base64_decode_rejects_malformed_input);
    RUN_TEST(base64_encode_refuses_undersized_output);

    return ora_test_report("protocol decoders");
}