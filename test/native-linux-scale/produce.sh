#!/usr/bin/env bash
# Build native Runtime packages and test support from one immutable snapshot.
set -euo pipefail

if (( $# != 3 )) || [[ ! $2 =~ ^[0-9a-f]{40}$ ]]; then
    printf '%s\n' 'Usage: bash produce.sh SOURCE_TAR SNAPSHOT FRESH_DIRECTORY' >&2
    exit 2
fi

scale_archive=$(realpath -- "$1")
scale_snapshot=$2
scale_producer=$3
if [[ $scale_producer != /var/tmp/jobu927-* || -e $scale_producer || -L $scale_producer ]]; then
    printf '%s\n' 'Use a new absolute /var/tmp/jobu927- producer directory.' >&2
    exit 2
fi

# Sources/support binaries are readable by the dedicated account. Active config,
# databases and native evidence are protected separately by the gate.
umask 0022
mkdir -- "$scale_producer"
mkdir -- "$scale_producer/source" "$scale_producer/logs" "$scale_producer/artifacts"
tar --extract --file "$scale_archive" --directory "$scale_producer/source" --no-same-owner
{
    cat /etc/os-release
    uname -a
    c++ --version
    cmake --version
    ninja --version
    sha256sum -- "$scale_archive"
    printf 'support-source-snapshot=%s\n' "$scale_snapshot"
    df -h -- "$scale_producer"
} > "$scale_producer/logs/environment.txt"
cmake -S "$scale_producer/source" -B "$scale_producer/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DJB_BUILD_EXAMPLES=OFF \
    > "$scale_producer/logs/configure.log" 2>&1
cmake --build "$scale_producer/build" --target jobu-scale-test --parallel 2 \
    > "$scale_producer/logs/build.log" 2>&1
cp -- "$scale_producer/build/test/jobu-scale-test" "$scale_producer/artifacts/"
sha256sum -- "$scale_producer/artifacts/jobu-scale-test" > "$scale_producer/logs/helper.sha256"

# A product correction requires a fresh Runtime payload from this same source.
# Package files stay inactive; the separate reviewed gate performs installation.
cmake -S "$scale_producer/source" -B "$scale_producer/build-runtime" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/jobu926 \
    -DJB_JOBU_SYSTEMD_PROFILE=custom -DBUILD_TESTING=OFF -DJB_BUILD_EXAMPLES=OFF \
    -DJB_PACKAGE_REMAP_PATHS=ON -DJB_PACKAGE_SOURCE_REVISION="$scale_snapshot" \
    -DJB_PACKAGE_SOURCE_STATE=clean > "$scale_producer/logs/runtime-configure.log" 2>&1
cmake --build "$scale_producer/build-runtime" --parallel 2 \
    > "$scale_producer/logs/runtime-build.log" 2>&1
cpack --config "$scale_producer/build-runtime/CPackConfig.cmake" -G TGZ -C Release \
    -B "$scale_producer/artifacts/runtime" > "$scale_producer/logs/runtime-package.log" 2>&1
printf 'Support producer complete: %s\n' "$scale_producer"
