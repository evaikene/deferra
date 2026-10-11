#!/usr/bin/env bash

# Build deployment payloads on the target distribution. Source is an immutable
# archive of the selected revision; the harness is kept outside that source.
set -euo pipefail

if (( $# != 3 )) || [[ ! $2 =~ ^[0-9a-f]{40}$ ]]; then
    printf '%s\n' 'Usage: bash produce.sh SOURCE_TAR REVISION FRESH_DIRECTORY' >&2
    exit 2
fi

source_archive=$(realpath -- "$1")
source_revision=$2
producer_directory=$3
if [[ $producer_directory != /* || -e $producer_directory || -L $producer_directory ]]; then
    printf '%s\n' 'The producer directory must be a new absolute path.' >&2
    exit 2
fi

umask 0077
mkdir -- "$producer_directory"
mkdir -- "$producer_directory/source" "$producer_directory/artifacts" "$producer_directory/logs"
tar --extract --file "$source_archive" --directory "$producer_directory/source" --no-same-owner

{
    cat /etc/os-release
    uname -a
    c++ --version
    cmake --version
    ninja --version
    sha256sum -- "$source_archive"
    printf 'source-revision=%s\n' "$source_revision"
    df -h -- "$producer_directory"
} > "$producer_directory/logs/environment.txt"

build_profile()
{
    local profile=$1
    local prefix=$2
    local build_directory=$producer_directory/build-$profile
    local log_directory=$producer_directory/logs/$profile
    mkdir -- "$log_directory" "$producer_directory/artifacts/$profile"

    # Every profile gets a fresh cache so compiled, sample and service paths
    # cannot inherit operational directories from another producer.
    cmake -S "$producer_directory/source" -B "$build_directory" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DJB_JOBU_SYSTEMD_PROFILE="$profile" -DBUILD_TESTING=OFF -DJB_BUILD_EXAMPLES=OFF \
        -DJB_PACKAGE_REMAP_PATHS=ON -DJB_PACKAGE_SOURCE_REVISION="$source_revision" \
        -DJB_PACKAGE_SOURCE_STATE=clean > "$log_directory/configure.log" 2>&1
    cmake --build "$build_directory" --parallel 2 > "$log_directory/build.log" 2>&1
    cpack --config "$build_directory/CPackConfig.cmake" -G TGZ -C Release \
        -B "$producer_directory/artifacts/$profile" > "$log_directory/package.log" 2>&1
    printf 'Packaged %s profile at %s\n' "$profile" "$prefix"
}

build_profile system /usr
build_profile custom /opt/jobu926

# This support executable deliberately stays out of the runtime archives. It
# exercises permanent-drop/regain probes in an expendable process, alongside
# direct observations of the installed daemon's own credentials.
helper_build=$producer_directory/build-helpers
cmake -S "$producer_directory/source" -B "$helper_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DJB_BUILD_EXAMPLES=OFF \
    -DJB_PACKAGE_REMAP_PATHS=ON -DJB_PACKAGE_SOURCE_REVISION="$source_revision" \
    -DJB_PACKAGE_SOURCE_STATE=clean > "$producer_directory/logs/helpers-configure.log" 2>&1
cmake --build "$helper_build" --target jobud-identity-test-helper --parallel 2 \
    > "$producer_directory/logs/helpers-build.log" 2>&1
mkdir -- "$producer_directory/artifacts/support"
cp -- "$helper_build/test/jobud-identity-test-helper" "$producer_directory/artifacts/support/"
sha256sum -- "$producer_directory/artifacts/support/jobud-identity-test-helper" \
    > "$producer_directory/logs/identity-helper.sha256"

du -sh -- "$producer_directory" > "$producer_directory/logs/disk-use.txt"
printf 'Producer complete: %s\n' "$producer_directory"
