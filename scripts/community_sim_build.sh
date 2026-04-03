#!/usr/bin/env bash
# scripts/community_sim_build.sh
#
# Build the community-sim Docker image used to reproduce
# community-extension CI build + bare runtime execution.

set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE_TAG="${1:-ducktinycc-sim}"
DOCKERFILE="${PROJ}/Dockerfile.community-sim"

if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: docker is required but not found on PATH" >&2
    exit 1
fi

echo "[community-sim] building image '${IMAGE_TAG}'"
docker build -t "${IMAGE_TAG}" -f "${DOCKERFILE}" "${PROJ}"
echo "[community-sim] build complete"
