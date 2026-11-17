# JobU PHP examples

These CLI scripts use the public `jobu.so` API. They require an enabled module, a
local disposable `jobud`, and permission to connect to its Unix socket. See the
[root README](../../README.md#php-builds-and-installation) for building and loading
the module, and the [PHP reference manual](../../docs/php.md) for API contracts.
All commands below run from the repository root.

Each creation script creates a uniquely named queue and leaves it for inspection.
The HTTP script also creates a uniquely named synthetic secret. No script cleans up
or changes pre-existing queues. Use a disposable database; repeating an example
creates additional resources. Executables and URLs are evaluated on the daemon's
host. The CLI examples require `/usr/bin/printf` and `/usr/bin/true` there.

## Start a disposable daemon

In one terminal, create a private directory and run the daemon in the foreground:

```sh
example_dir=$(mktemp -d /tmp/jobu-php-examples.XXXXXX)
printf 'Example directory: %s\n' "$example_dir"
.bld/src/jobud/jobud --no-config \
    --socket "$example_dir/daemon.sock" --database "$example_dir/daemon.sqlite"
```

Keep the printed directory path. In another terminal set these paths to the daemon
socket, the selected PHP executable, and its matching module. For an already enabled
module, omit the `-n -d extension=...` options from subsequent commands.

```sh
example_socket=/tmp/jobu-php-examples.REPLACE/daemon.sock
example_php=/path/to/php/bin/php
example_module="$PWD/.bld/src/jobu-php/jobu.so"
```

Run the daemon as a normal user. Root execution has separate daemon/CLI restrictions;
see the [container instructions](../../containers/README.md) for disposable test
containers. Do not weaken a production daemon's execution policy to try these examples.

## Submit and inspect CLI work

```sh
"$example_php" -n -d extension="$example_module" \
    examples/jobu-php/enqueue-cli.php "$example_socket"
```

The script configures queue defaults and schedules one immediate run. It prints a
queue UUID and job UUID. Substitute them below to observe that existing run; the
output script does not enqueue another run:

```sh
"$example_php" -n -d extension="$example_module" \
    examples/jobu-php/output.php "$example_socket" QUEUE_ID JOB_ID
```

`output.php` waits up to approximately 30 seconds plus one in-progress refresh
(timeout two seconds). It reports the selected run's state, a convenience output tail,
and the first 4096 bytes of stdout (CLI) or body (HTTP), together with capture status,
truncation, loss, and the next retained offset. It displays bytes as base64, preserving
NUL and non-UTF-8 data. The CLI example produces `Hello from JobU` followed by a newline.
Null output means unavailable; captured empty output remains an empty string.
For jobs with multiple runs, the script selects the first run in the daemon's default
history ordering; it does not follow future occurrences or wait for every run.

## Submit an HTTP request with a secret reference

Start this local, synthetic endpoint in another terminal:

```sh
"$example_php" -n -S 127.0.0.1:8080 examples/jobu-php/http-endpoint.php
```

Then submit the request:

```sh
"$example_php" -n -d extension="$example_module" \
    examples/jobu-php/enqueue-http.php "$example_socket" http://127.0.0.1:8080/events
```

The job sends JSON and repeated `X-Tag` headers. Its Authorization header refers to
a uniquely named secret containing `synthetic-demo-token`. The endpoint accepts that
synthetic value and returns HTTP 201 with the received JSON. Use `output.php` with the
printed IDs to read the response bytes. JobU does not automatically interpret response
JSON. Both the daemon and endpoint must share the relevant loopback network namespace.

The endpoint is only a local example; use synthetic data. Secret references remain
symbolic in payload getters. The secret is intentionally retained because the stored
job references it; stopping this disposable daemon and discarding its private data
provides cleanup without demonstrating unsafe deletion/retry patterns.

## Cron, revisions, and pages

```sh
"$example_php" -n -d extension="$example_module" examples/jobu-php/cron.php "$example_socket"
"$example_php" -n -d extension="$example_module" examples/jobu-php/revisions.php "$example_socket"
"$example_php" -n -d extension="$example_module" examples/jobu-php/pages.php "$example_socket"
```

| Script | Demonstration |
| --- | --- |
| `cron.php` | Validate a weekday 09:00 Europe/Tallinn schedule, preview three UTC occurrences, and create a recurring job in a suspended queue. Resume it explicitly if ongoing execution is desired. |
| `revisions.php` | Two cached views compete to update one new queue. The stale writer keeps its proposal, discards locally, refreshes, and reports the winning value without overwriting it. |
| `pages.php` | Create three jobs in a suspended queue and list them in two-item pages. Call `next()` before reading each page; an invalid cursor is surfaced rather than silently restarting. |

Queue defaults affect newly created jobs. Local mutations need `save()`. Discarding
changes restores the old local snapshot; `refresh()` fetches the current server state.
Creation already schedules the initial run; `runNow()` would add a separate run.

## Reconnect after a daemon restart

```sh
"$example_php" -n -d extension="$example_module" examples/jobu-php/reconnect.php "$example_socket"
```

When the script prompts, stop **this disposable daemon** with Ctrl-C and restart the
same command in its original terminal, retaining the socket/database paths. Wait for
`jobud.ready`, then press Enter in the PHP terminal. The next read uses the same Client,
which reconnects and rechecks compatibility within its timeout. If recovery exhausts
the connection deadline, create a new Client for a later operation. Explicitly closed
clients never reconnect.

The examples contain no application-level retry loop for mutations. The module may
retry reads and keyed creation according to its [recovery contract](../../docs/php.md#recovery-contract).
An `OutcomeUnknownException` can mean a mutation committed; inspect state separately
before deciding what to do. `common.php` prints stable error metadata and any operation
or idempotency key, without printing exception traces. PHP 8.1 lacks
`SensitiveParameter` trace redaction; see [secret handling](../../docs/php.md#secrets-and-cron-helpers).

After inspection, stop the endpoint and daemon with Ctrl-C. Remove only the private
directory created for this run when its database/history is no longer needed.
