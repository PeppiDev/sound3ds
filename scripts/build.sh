#!/usr/bin/env bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> [Sound3DS] Compilando con devkitpro/devkitarm..."
podman run --rm \
    --security-opt label=disable \
    -v "${PROJECT_DIR}:/workspace:Z" \
    -w /workspace \
    docker.io/devkitpro/devkitarm:latest \
    bash -c ". /opt/devkitpro/3dsvars.sh && make $*"
