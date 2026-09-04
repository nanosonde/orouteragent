FROM alpine:3.20

RUN apk add --no-cache dnsmasq

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

HEALTHCHECK --interval=5s --timeout=3s --retries=10 \
    CMD pidof dnsmasq >/dev/null || exit 1

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
