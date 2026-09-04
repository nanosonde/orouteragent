#!/usr/bin/env bash
# Run gcc -fanalyzer over every source file using the target toolchain
# from the SDK container.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"${HERE}/sdk-run.sh" <<'ANALYZE'
set -eu

# The analyzer needs the dependency headers, which appear in staging_dir
# once the package has been built at least once.
make package/orouteragent/compile $ORA_MAKE_FLAGS -j"$(nproc)" >/dev/null 2>&1 || true

toolchain_bin="$(ls -d /builder/staging_dir/toolchain-*/bin | head -1)"
target_inc="$(ls -d /builder/staging_dir/target-*/usr/include | head -1)"
cc="$(ls "$toolchain_bin"/*-gcc | grep -vE -- '-gcc-[0-9]' | head -1)"

# The SDK's gcc is a wrapper that warns on every invocation without this.
export STAGING_DIR=/builder/staging_dir

echo "[analyze] $(basename "$cc")" >&2

cd package/orouteragent/src
log="$(mktemp)"
rc=0
for f in $(find . -name '*.c' | sort); do
    # -isystem for the SDK headers: warnings from inside libubox's own
    # macros are not ours and would drown out real findings.
    "$cc" -O2 -std=c11 -D_GNU_SOURCE \
        -I. -Iprotocol -Imodel -Iservices -isystem "$target_inc" \
        -fanalyzer -Wall -Wextra -Wshadow -Wpointer-arith -Wno-unused-parameter \
        -c -o /tmp/ora-analyze.o "$f" >>"$log" 2>&1 || rc=1
done
rm -f /tmp/ora-analyze.o

if grep -q 'warning:\|error:' "$log"; then
    cat "$log"
    echo "analyzer: findings above"
    exit 1
fi
[ "$rc" -eq 0 ] || { cat "$log"; exit 1; }
echo "analyzer: clean"
ANALYZE