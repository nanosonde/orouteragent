/* orouteragent - pcap writer + AF_PACKET capture implementation */
#include "pcap.h"
#include "../util.h"

#include <mbedtls/md5.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* pcap headers are written in host byte order; the magic tells the
 * reader which order that was. */
struct pcap_file_header {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t linktype;
};

struct pcap_pkthdr_disk {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t caplen;
    uint32_t len;
};

bool ora_pcap_write_header(int fd)
{
    struct pcap_file_header h;

    memset(&h, 0, sizeof(h));
    h.magic = ORA_PCAP_MAGIC;
    h.version_major = ORA_PCAP_VERSION_MAJOR;
    h.version_minor = ORA_PCAP_VERSION_MINOR;
    h.thiszone = 0;
    h.sigfigs = 0;
    h.snaplen = ORA_PCAP_SNAPLEN;
    h.linktype = ORA_PCAP_LINKTYPE_ETHERNET;

    return write(fd, &h, sizeof(h)) == (ssize_t)sizeof(h);
}

bool ora_pcap_prepare_dir(void)
{
    struct stat st;

    if (mkdir(ORA_CAPTURE_DIR, 0700) != 0 && errno != EEXIST) {
        ora_log(ORA_LOG_ERR, "capture: cannot create %s: %s",
                ORA_CAPTURE_DIR, strerror(errno));
        return false;
    }
    /* lstat, not stat: a symlink planted here must be rejected rather
     * than followed. */
    if (lstat(ORA_CAPTURE_DIR, &st) != 0)
        return false;
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) {
        ora_log(ORA_LOG_ERR,
                "capture: %s is not a private directory owned by us; refusing",
                ORA_CAPTURE_DIR);
        return false;
    }
    return true;
}

int ora_pcap_open_read(const char *path)
{
    return open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}

ssize_t ora_pcap_fd_size(int fd)
{
    struct stat st;

    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    return (ssize_t)st.st_size;
}

size_t ora_pcap_capture(const char *path, const struct ora_capture_opts *opts,
                        volatile bool *stop)
{
    int fd = -1, sock = -1;
    size_t written = 0;
    uint64_t deadline;
    uint8_t *pkt = NULL;

    /* Drop any stale file, then refuse to follow whatever may appear in
     * its place: O_CREAT|O_EXCL fails on an existing path including a
     * symlink, so a planted link cannot redirect this root-owned write. */
    if (unlink(path) != 0 && errno != ENOENT) {
        ora_log(ORA_LOG_ERR, "capture: cannot replace %s: %s", path, strerror(errno));
        return 0;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        ora_log(ORA_LOG_ERR, "capture: cannot create %s: %s", path, strerror(errno));
        return 0;
    }
    if (!ora_pcap_write_header(fd)) {
        close(fd);
        return 0;
    }
    written = sizeof(struct pcap_file_header);

    sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0) {
        ora_log(ORA_LOG_ERR, "capture: AF_PACKET socket: %s", strerror(errno));
        close(fd);
        return written; /* header-only file is still valid */
    }

    if (opts->ifname[0]) {
        struct sockaddr_ll sll;
        unsigned idx = if_nametoindex(opts->ifname);

        if (idx == 0) {
            ora_log(ORA_LOG_WARN, "capture: unknown interface %s", opts->ifname);
        } else {
            memset(&sll, 0, sizeof(sll));
            sll.sll_family = AF_PACKET;
            sll.sll_protocol = htons(ETH_P_ALL);
            sll.sll_ifindex = (int)idx;
            if (bind(sock, (struct sockaddr *)&sll, sizeof(sll)) != 0)
                ora_log(ORA_LOG_WARN, "capture: bind %s: %s",
                        opts->ifname, strerror(errno));
        }
    }

    pkt = malloc(ORA_PCAP_SNAPLEN);
    if (!pkt) {
        close(sock);
        close(fd);
        return written;
    }

    deadline = ora_now_ms() +
               (uint64_t)(opts->duration_s > 0 ? opts->duration_s : 10) * 1000;

    while (!(stop && *stop)) {
        struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
        int64_t remain = (int64_t)deadline - (int64_t)ora_now_ms();
        struct pcap_pkthdr_disk ph;
        struct timespec ts;
        ssize_t n;

        if (remain <= 0)
            break;
        if (opts->total_size && written >= opts->total_size)
            break;
        if (poll(&pfd, 1, (int)(remain > 500 ? 500 : remain)) <= 0)
            continue;

        n = recv(sock, pkt, ORA_PCAP_SNAPLEN, MSG_TRUNC);
        if (n <= 0)
            continue;

        clock_gettime(CLOCK_REALTIME, &ts);
        ph.ts_sec = (uint32_t)ts.tv_sec;
        ph.ts_usec = (uint32_t)(ts.tv_nsec / 1000);
        ph.len = (uint32_t)n;                 /* wire length */
        ph.caplen = (uint32_t)(n > ORA_PCAP_SNAPLEN ? ORA_PCAP_SNAPLEN : n);

        if (opts->total_size &&
            written + sizeof(ph) + ph.caplen > opts->total_size)
            break;
        if (write(fd, &ph, sizeof(ph)) != (ssize_t)sizeof(ph))
            break;
        if (write(fd, pkt, ph.caplen) != (ssize_t)ph.caplen)
            break;
        written += sizeof(ph) + ph.caplen;
    }

    free(pkt);
    close(sock);
    close(fd);
    return written;
}

bool ora_pcap_fd_md5(int fd, char *out, size_t outsz)
{
    static const char hex[] = "0123456789abcdef";
    mbedtls_md5_context ctx;
    unsigned char digest[16];
    uint8_t buf[8192];
    ssize_t n;
    size_t i;
    bool ok = false;

    if (!out || outsz < 33 || fd < 0)
        return false;
    out[0] = '\0';
    if (lseek(fd, 0, SEEK_SET) == (off_t)-1)
        return false;

    mbedtls_md5_init(&ctx);
    if (mbedtls_md5_starts(&ctx) != 0)
        goto out;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (mbedtls_md5_update(&ctx, buf, (size_t)n) != 0)
            goto out;
    }
    if (n < 0 || mbedtls_md5_finish(&ctx, digest) != 0)
        goto out;
    for (i = 0; i < sizeof(digest); i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[32] = '\0';
    ok = true;
out:
    mbedtls_md5_free(&ctx);
    lseek(fd, 0, SEEK_SET);
    return ok;
}