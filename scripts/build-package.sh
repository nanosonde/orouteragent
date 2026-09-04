#!/usr/bin/env bash
# Build the package with the official OpenWrt SDK container.
#
#   TARGET=mediatek/filogic scripts/build-package.sh    # the real router
#   TARGET=x86/64           scripts/build-package.sh    # the e2e VM
#
# See scripts/sdk-run.sh for the configuration knobs (registry, version,
# output directory).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"${HERE}/sdk-run.sh" <<'BUILD'
echo "[build] compiling with ${ORA_BUILD_JOBS} jobs" >&2
if ! make package/orouteragent/compile $ORA_MAKE_FLAGS -j"${ORA_BUILD_JOBS}" >/tmp/build.log 2>&1; then
    # Some feed packages are not parallel-safe; a serial retry usually
    # clears it. Only if that fails too is it worth the verbose output.
    echo "[build] parallel build failed, retrying serially" >&2
    if ! make package/orouteragent/compile $ORA_MAKE_FLAGS -j1 >/tmp/build.log 2>&1; then
        echo "[build] failed:" >&2
        tail -30 /tmp/build.log >&2
        exit 1
    fi
fi

found=0
for apk in bin/packages/*/base/orouteragent-*.apk; do
    [ -e "$apk" ] || continue
    rm -f /out/"$(basename "$apk")"
    cp "$apk" /out/
    echo "[build] -> $(basename "$apk")" >&2
    found=1
done
[ "$found" = 1 ] || { echo "[build] no package produced" >&2; exit 1; }
BUILD
