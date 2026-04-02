#!/usr/bin/env bash
# scripts/community_sim.sh
#
# Build and run the community-sim Docker workflow.

set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE_TAG="${1:-ducktinycc-sim}"

"${PROJ}/scripts/community_sim_build.sh" "${IMAGE_TAG}"
"${PROJ}/scripts/community_sim_run.sh" "${IMAGE_TAG}"
