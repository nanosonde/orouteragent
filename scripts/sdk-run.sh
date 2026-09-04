#!/usr/bin/env bash
# Run a command inside the official OpenWrt SDK container for a target.
#
# The script to run is read from stdin and executed in /builder, with the
# repository available read-only at /pkg and an output directory at /out.
#
#   echo 'make package/orouteragent/compile' | TARGET=x86/64 scripts/sdk-run.sh
#
# Configuration (all optional):
#   TARGET             target to build for            (mediatek/filogic)
#   OPENWRT_VERSION    release to build against       (25.12.5)
#   ORA_SDK_REGISTRY   registry holding the images    (ghcr.io)
#   ORA_SDK_IMAGE      full image ref, overrides both registry and tag
#   ORA_SDK_VOLUME     docker volume caching the SDK
#   ORA_BUILD_JOBS     parallel make jobs             (host cpu count)
#   OUT_DIR            where artifacts are written
#
# The SDK lives in a named docker volume rather than the container, so
# feeds and object files survive between runs and rebuilds are quick.
# Discard it with 'just sdk-clean' if a build ever gets into a bad state.
#
# The container runs as root. Nothing here needs root privileges as such,
# but being root lets the build skip fakeroot; see ORA_MAKE_FLAGS below.
# Only the SDK volume and the output directory are writable, and the
# repository itself is mounted read-only.
set -euo pipefail

TARGET="${TARGET:-mediatek/filogic}"
VERSION="${OPENWRT_VERSION:-25.12.5}"
REGISTRY="${ORA_SDK_REGISTRY:-ghcr.io}"

SLUG="${TARGET//\//-}"
IMAGE="${ORA_SDK_IMAGE:-${REGISTRY}/openwrt/sdk:${SLUG}-${VERSION}}"
VOLUME="${ORA_SDK_VOLUME:-orouteragent-sdk-${SLUG}-${VERSION}}"
JOBS="${ORA_BUILD_JOBS:-$(nproc)}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${OUT_DIR:-${REPO_ROOT}/dist/${SLUG}}"

log() { echo "[sdk] $*" >&2; }

command -v docker >/dev/null || { log "docker is required"; exit 1; }

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    log "pulling ${IMAGE} (once, ~800 MB)"
    docker pull "$IMAGE"
fi

mkdir -p "$OUT_DIR"

# The caller's script runs after the SDK is prepared. Preparation is
# marked done in the volume so it only happens on the first run.
{
    cat <<'PREP'
set -eu
cd /builder

# Everything below runs as root, so hand the artifacts back to the user
# who started the build.
trap 'chown -R "${ORA_HOST_UID}:${ORA_HOST_GID}" /out 2>/dev/null || true' EXIT

# OpenWrt wraps the packaging step in fakeroot so that files are recorded
# as owned by root. We already are root, so the pretence is unnecessary --
# and expensive: intercepting every syscall of 'apk mkpkg' takes ~110s for
# this package, against ~5s without. Callers pass this to make.
ORA_MAKE_FLAGS="FAKEROOT=env"

# Snapshot images ship a setup.sh instead of the unpacked SDK.
[ -d ./scripts ] || ./setup.sh

if [ ! -f .ora-prepared ]; then
    echo "[sdk] preparing the SDK (first run for this target)" >&2
    grep -v '^#' feeds.conf.default > feeds.conf || true
    ./scripts/feeds update base >/dev/null
    ./scripts/feeds install -p base uci ubus libubox libjson-c mbedtls libmnl >/dev/null
    make defconfig >/dev/null
    grep -q '^CONFIG_PACKAGE_orouteragent=y' .config || \
        echo 'CONFIG_PACKAGE_orouteragent=y' >> .config
    touch .ora-prepared
fi

# The SDK default config enables the lua bindings of uci/ubox/ubus, which
# drag in lua itself. Nothing here links against it, and lua's makefile
# fails under parallel make, so keep every lua package out of the
# dependency chain.
sed -i -E 's/^CONFIG_PACKAGE_([A-Za-z0-9_.+-]*lua[A-Za-z0-9_.+-]*)=.*/# CONFIG_PACKAGE_\1 is not set/' .config

# Refresh the package source from the read-only repo mount.
rm -rf package/orouteragent
mkdir -p package/orouteragent
cp -a /pkg/Makefile /pkg/src /pkg/files package/orouteragent/
PREP
    cat
} | docker run --rm -i \
        --user 0:0 \
        -v "${VOLUME}:/builder" \
        -v "${REPO_ROOT}:/pkg:ro" \
        -v "${OUT_DIR}:/out" \
        -e "ORA_BUILD_JOBS=${JOBS}" \
        -e "ORA_HOST_UID=$(id -u)" \
        -e "ORA_HOST_GID=$(id -g)" \
        -w /builder \
        "$IMAGE" bash -s