#!/usr/bin/env bash
# Build the agent for the x86-64 e2e VM and stage it where the installer
# looks for it. Thin wrapper around scripts/build-package.sh.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

TARGET="x86/64" \
OPENWRT_VERSION="${E2E_OPENWRT_VERSION:-25.12.5}" \
OUT_DIR="${REPO_ROOT}/e2e-testing/artifacts" \
    "${REPO_ROOT}/scripts/build-package.sh"
