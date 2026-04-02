#!/usr/bin/env bash
# scripts/community_sim_run.sh
#
# Run the community-sim Docker image test harness.

set -euo pipefail

IMAGE_TAG="${1:-ducktinycc-sim}"

if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: docker is required but not found on PATH" >&2
    exit 1
fi

echo "[community-sim] running image '${IMAGE_TAG}'"
docker run --rm "${IMAGE_TAG}"
echo "[community-sim] run complete"
