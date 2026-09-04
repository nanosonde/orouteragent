/* orouteragent - libpcap file writer and AF_PACKET capture */
#ifndef ORA_PCAP_H
#define ORA_PCAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* libpcap 2.4 global header */
#define ORA_PCAP_MAGIC 0xA1B2C3D4u
#define ORA_PCAP_VERSION_MAJOR 2
#define ORA_PCAP_VERSION_MINOR 4
#define ORA_PCAP_LINKTYPE_ETHERNET 1
#define ORA_PCAP_SNAPLEN 65535

struct ora_capture_opts {
    char ifname[32];    /* interface to capture on ("" = any) */
    int duration_s;     /* stop after this many seconds */
    size_t total_size;  /* stop after this many bytes of file */
    int vlan_id;        /* informational; 0 = unfiltered */
};

/* Directory the agent keeps capture files in. Created private (0700)
 * and verified before use so a world-writable /tmp cannot be used to
 * redirect a root-owned write. */
#define ORA_CAPTURE_DIR "/tmp/orouteragent"

/* Create ORA_CAPTURE_DIR if needed and verify it is a real directory
 * owned by us and not writable by anyone else. */
bool ora_pcap_prepare_dir(void);

/* Open a capture file for reading without following symlinks. Returns
 * -1 on error. */
int ora_pcap_open_read(const char *path);

/* Size of an already-open file (no path lookup, so no TOCTOU). */
ssize_t ora_pcap_fd_size(int fd);

/* Lowercase hex MD5 of an already-open file. @out needs 33 bytes. The
 * file offset is restored to the start. */
bool ora_pcap_fd_md5(int fd, char *out, size_t outsz);

/* Run a capture, writing a pcap file to @path. Blocks until the
 * duration/size limit is reached or *stop becomes true. Returns the
 * number of bytes written, or 0 on error. */
size_t ora_pcap_capture(const char *path, const struct ora_capture_opts *opts,
                        volatile bool *stop);

/* Write only the pcap global header (used when a capture yields no
 * packets: the controller still expects a valid file). */
bool ora_pcap_write_header(int fd);

#endif