#!/usr/bin/env bash
# Compila Sound3DS QR (.3dsx) con la imagen de devkitARM, igual que la app principal.
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
podman run --rm \
    --security-opt label=disable \
    -v "${DIR}:/workspace:Z" \
    -w /workspace \
    docker.io/devkitpro/devkitarm:latest \
    bash -c ". /opt/devkitpro/3dsvars.sh && make $*"
