/* orouteragent - protocol constants
 *
 * These values are part of the management protocol contract. Ports,
 * identity fields and message numbers are wire-visible compatibility data.
 */
#ifndef ORA_CONSTANTS_H
#define ORA_CONSTANTS_H

/* Ports */
#define ORA_DISCOVERY_UDP_PORT   29810
#define ORA_MANAGER_V2_TCP_PORT  29814
#define ORA_TRANSFER_TCP_PORT    29815
#define ORA_RTTY_TCP_PORT        29816
#define ORA_DEVICE_MONITOR_PORT  29817
#define ORA_MGMT_HTTPS_PORT      8043

/* Timing (seconds / milliseconds) */
#define ORA_DISCOVERY_COOLDOWN_MS 20000 /* announces older than this are dropped */
#define ORA_ANNOUNCE_INTERVAL_S   10
#define ORA_INFORM_INTERVAL_S     10
#define ORA_HEARTBEAT_INTERVAL_S  10
#define ORA_RECONNECT_DELAY_S     5
#define ORA_CONNECT_TIMEOUT_S     10
#define ORA_IO_TIMEOUT_S          2   /* per-recv timeout in handshake/serve loops */

/* Protocol identity */
#define ORA_DEVICE_TYPE_GATEWAY   "gateway"
#define ORA_PROTOCOL_VERSION      "2.2.0" /* gateway ECSP fit version */
#define ORA_PROTOCOL_VER_CAP      3       /* MUST be 3 */
#define ORA_TLS_SERVER_HOSTNAME   "localhost" /* SNI / expected CN */

/* Factory sentinel controller ID: announcing this makes the device show
 * up as "Pending" (adoptable). Any other ID => "managed by others". */
#define ORA_FACTORY_SENTINEL_ID   "c21f969b5f03d33d43e04f8f136e7682"

/* Message types (header.type) */
enum {
    ORA_MSG_UNKNOWN                    = -1,
    ORA_MSG_DISCOVERY                  = 1,
    ORA_MSG_PRE_ADOPT_REQUEST          = 2,
    ORA_MSG_PRE_CONNECT_INFO           = 3,
    ORA_MSG_NOTIFY_REQUEST             = 80,
    ORA_MSG_NOTIFY_REPLY               = 144,
    ORA_MSG_INFORM_REQUEST             = 256,
    ORA_MSG_INFORM_RESPONSE            = 512,
    ORA_MSG_SET_REQUEST                = 4096,
    ORA_MSG_SET_RESPONSE               = 8192,
    ORA_MSG_FORGET_REQUEST             = 16384,
    ORA_MSG_FORGET_RESPONSE            = 20480,
    ORA_MSG_GET_REQUEST                = 24576,
    ORA_MSG_GET_RESPONSE               = 28672,
    ORA_MSG_UPGRADE_REQUEST            = 32768,
    ORA_MSG_UPGRADE_RESPONSE           = 65536,

    /* adoption handshake (v2, on 29814) */
    ORA_MSG_PRE_CONNECT_INFO_RESPONSE  = 0x100000,
    ORA_MSG_DEVICE_VERIFY_INFO         = 0x100001,
    ORA_MSG_DEVICE_VERIFY_RESPONSE     = 0x100002,
    ORA_MSG_SYSTEM_VERIFY_RESULT       = 0x100003,
    ORA_MSG_DEVICE_NEGOTIATION         = 0x100004,
    ORA_MSG_SYSTEM_NEGOTIATION         = 0x100005,
    ORA_MSG_INIT_SYNC_RESULT           = 0x100006,
    ORA_MSG_NOTIFY_REQUEST_V2          = 0x100007, /* silently dropped by ctrl */
    ORA_MSG_NOTIFY_REPLY_V2            = 0x100008,
    ORA_MSG_VERIFY_RESULT_ACK          = 0x100009,
    ORA_MSG_INIT_SYNC_RESULT_ACK       = 0x10000A,

    /* file transfer (v2) */
    ORA_MSG_FILE_TRANSFER_REQUEST_V2   = 0x160000, /* ctrl -> dev, on 29814 */
    ORA_MSG_FILE_TRANSFER_RESPONSE_V2  = 0x170000, /* dev -> ctrl, on 29814 */
};

/* Device status codes reported by the controller (informational) */
#define ORA_STATUS_CONNECTED  14

/* File transfer partition size: 512 KiB */
#define ORA_TRANSFER_PARTITION_BYTES 524288

#endif /* ORA_CONSTANTS_H */