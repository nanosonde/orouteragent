#!/usr/bin/env bash
# List the devices the controller knows about, so adoption state can be
# checked without opening the UI.
set -euo pipefail

BASE="${E2E_CONTROLLER_URL_BASE:-https://127.0.0.1:8043}"
USER_NAME="${E2E_CONTROLLER_USER:-admin}"
PASSWORD="${E2E_CONTROLLER_PASS:-}"

if [[ -z "$PASSWORD" ]]; then
    cat >&2 <<'EOF'
[devices] set E2E_CONTROLLER_PASS (and optionally E2E_CONTROLLER_USER) to the
          controller admin account created during first-time setup, e.g.

              E2E_CONTROLLER_PASS='...' just e2e-devices

Until the controller has been through its setup wizard there is no account to
query with; open https://127.0.0.1:8043 (or the proxy on :8090) to create one.
EOF
    exit 2
fi

jar="$(mktemp)"
trap 'rm -f "$jar"' EXIT

omadac_id="$(curl -sk "${BASE}/api/info" | sed -n 's/.*"omadacId":"\([^"]*\)".*/\1/p')"
if [[ -z "$omadac_id" ]]; then
    echo "[devices] could not read omadacId from ${BASE}/api/info" >&2
    exit 1
fi

login="$(curl -sk -c "$jar" -X POST \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"${USER_NAME}\",\"password\":\"${PASSWORD}\"}" \
    "${BASE}/${omadac_id}/api/v2/login")"

token="$(printf '%s' "$login" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
if [[ -z "$token" ]]; then
    echo "[devices] login failed: $login" >&2
    exit 1
fi

curl -sk -b "$jar" -H "Csrf-Token: ${token}" \
    "${BASE}/${omadac_id}/api/v2/sites/Default/devices" > "${jar}.devices"

python3 - "${jar}.devices" <<'PY'
import json
import sys

with open(sys.argv[1]) as fh:
    try:
        payload = json.load(fh)
    except json.JSONDecodeError:
        sys.exit("could not parse the controller response")

devices = payload.get("result") or []
if not devices:
    print("no devices known to the controller yet")
    raise SystemExit(0)

header = "{:<22} {:<10} {:<20} {:<15} {}"
print(header.format("NAME", "MODEL", "MAC", "IP", "STATUS"))
for d in devices:
    print(header.format(
        str(d.get("name", "?"))[:22],
        str(d.get("model", "?"))[:10],
        str(d.get("mac", "?"))[:20],
        str(d.get("ip", "?"))[:15],
        d.get("statusCategory", d.get("status", "?")),
    ))
PY
rm -f "${jar}.devices"