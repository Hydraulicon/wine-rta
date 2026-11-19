#!/usr/bin/env bash
#
# Fetch the MIT-licensed VST3 SDK, with its submodules, as a read-only reference.
# The SDK is not vendored into this repository; see rta/reference/vst3-sdk.md.
#
# Usage: rta/tools/fetch-vst3sdk.sh [destination]   (default: ../vst3sdk)

set -euo pipefail

VST3SDK_TAG=v3.8.1_build_84
dest=${1:-"$(dirname "$0")/../../../vst3sdk"}

if [[ -d $dest/.git ]]; then
    echo "$dest already exists, leaving it alone" >&2
    exit 0
fi

git clone --depth 1 --branch "$VST3SDK_TAG" --recurse-submodules --shallow-submodules \
    https://github.com/steinbergmedia/vst3sdk "$dest"
