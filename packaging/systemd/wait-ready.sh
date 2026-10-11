#!/bin/sh
# Usage: sh wait-ready.sh /absolute/jobuctl /absolute/socket [attempts [timeout-ms]]
# Each RPC has the CLI's overall deadline. A finite retry budget and one-second
# pauses bound the wait; the defaults allow 15 RPCs of at most one second each.
if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
    printf '%s\n' 'Usage: sh wait-ready.sh /absolute/jobuctl /absolute/socket [attempts [timeout-ms]]' >&2
    exit 2
fi

client=$1
socket=$2
attempts=${3-15}
timeout_ms=${4-1000}
for path in "$client" "$socket"; do
    case "$path" in
        /*) ;;
        *) printf '%s\n' 'The client and socket paths must be absolute.' >&2; exit 2 ;;
    esac
done
for value in "$attempts" "$timeout_ms"; do
    case "$value" in
        '' | 0* | *[!0-9]*) printf '%s\n' 'Attempts and timeout must be positive decimal integers without leading zeros.' >&2; exit 2 ;;
    esac
done
# Check lengths before shell arithmetic, including on shells with narrow integers.
if [ "${#attempts}" -gt 3 ] || [ "${#timeout_ms}" -gt 5 ] ||
    [ "$attempts" -lt 1 ] || [ "$attempts" -gt 300 ] ||
    [ "$timeout_ms" -lt 1 ] || [ "$timeout_ms" -gt 60000 ]; then
    printf '%s\n' 'Attempts must be 1..300 and timeout must be 1..60000 milliseconds.' >&2
    exit 2
fi

remaining=$attempts
while [ "$remaining" -gt 0 ]; do
    if result=$("$client" --socket "$socket" --timeout "$timeout_ms" --json system info 2>&1); then
        printf '%s\n' "$result"
        exit 0
    fi

    remaining=$((remaining - 1))
    if [ "$remaining" -gt 0 ]; then
        sleep 1
    fi
done

printf 'JobU RPC readiness failed after %s attempts. Last diagnostic:\n%s\n' "$attempts" "$result" >&2
exit 1
