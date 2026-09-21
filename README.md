# JobU

JobU (Job Orchestration and Broker Utility) is a portable C++20 service for
running scheduled command-line and HTTP jobs in the background. The planned v1
service includes independently managed queues, retries and timeouts, recurring
jobs, execution history, and output capture. `jobuctl` communicates with the
`jobud` daemon through a local JSON-RPC API over a Unix-domain socket, and
SQLite provides persistence.

> [!IMPORTANT]
> JobU is under active development and is not ready for production use. The
> repository currently provides SQLite persistence, local IPC and JSON-RPC,
> deterministic scheduling, and real asynchronous CLI and HTTP execution through
> the scheduler composed in `jobud` on Linux and macOS.

## Components

- **`jobud`** — the background daemon that hosts the queues, schedules jobs,
  and executes them, exposing a local JSON-RPC API over a Unix-domain socket.
- **`jobuctl`** — the control client for managing queues and jobs from the
  command line; it talks to `jobud` over that same JSON-RPC API.

## Platforms

The current source tree is built and tested on:

- Ubuntu 24.04
- Alpine Linux 3.22
- macOS with Apple Clang and Homebrew dependencies

Linux is the primary development platform and supports CLI and HTTP scheduling,
local IPC, JSON-RPC, the daemon, and the control client. macOS also supports
these features, with native Process, CLI executor, mixed scheduler, client,
and daemon integration coverage. Final clean Linux verification after the
macOS changes passed. The merged Phase 6 disabled-capture correction also passed
integrated Linux and focused native macOS validation. Phase 6 is complete.
The [closure design](docs/planning/jobu-phase6-closure-code-design.md) records the
Phase 6 correction and accepted historical limitations.

Phase 7 recovery and immediate shutdown are complete. Final clean Linux
verification passed all 135 registered tests against the merged implementation.
Native Stage 7.18 passed 136 tests, with the real-root denial case skipped; the
later scheduler startup sanitizer has Linux evidence only. The single shared
`~/cloud/Projektid/JobU/jobu-phase7-verification.md` records exact revisions,
commands, platform limits, and the inherited Phase 6 closure summary.
Windows is not a v1 runtime target.

## Requirements

The build requires CMake 3.20 or newer, a C++20 compiler, fmt, SQLite, libcurl
7.85 or newer, nlohmann/json, Catch2 3.x and OpenSSL for tests, and Ninja or
another CMake-supported build tool. The private system HTTP backend verifies its
linked libcurl runtime before `jobud` enters its event loop.

Linux command execution requires a libc providing `_Fork()` (checked at
configure time), kernel pidfd support, and `PR_SET_NO_NEW_PRIVS`. A container
uses its host kernel and must permit these operations. There is no waiter
thread or polling fallback for unavailable process watches. The Ubuntu 24.04
and Alpine 3.22 CI jobs build and run the SQLite-enabled suite, including the
Linux daemon CLI test; no additional package is required for CLI execution.

macOS command execution uses kqueue process watches and public `fork()`.
Process-controlled child setup is async-signal-safe after `fork()` returns;
host-registered at-fork handlers remain outside that guarantee. macOS CLI
execution retains root denial but does not provide Linux NoNewPrivs hardening.
A strict `Process::prevent_privilege_gain` request fails as unsupported.

The daemon CLI test runs ordinary execution cases as a non-root user. Real
root-denial coverage runs only when the test itself is root. Root execution
with the unsafe override additionally requires `JOBU_TEST_ALLOW_ROOT_CLI=1`;
set it only inside a disposable, isolated test environment. Alpine CI opts in
inside its job container and checks the warning and real helper execution.
Non-root runs report the real-root case as skipped; injected identity tests
provide separate policy coverage, not evidence of a root daemon launch.
The macOS verification used a non-root account; actual root denial and unsafe
root-override execution remain unverified on macOS.

### Ubuntu 24.04

```sh
sudo apt-get update
sudo apt-get install --yes --no-install-recommends \
    catch2 cmake g++ libcurl4-openssl-dev libfmt-dev libssl-dev libsqlite3-dev \
    ninja-build nlohmann-json3-dev
```

### Alpine Linux 3.22

Alpine does not ship `sudo` by default, so run the following as root (or
install `sudo` first):

```sh
apk add --no-cache \
    catch2-3 cmake curl-dev fmt-dev g++ nlohmann-json openssl-dev samurai sqlite-dev tzdata
```

### macOS

Install the Xcode Command Line Tools and
[Homebrew](https://brew.sh/), then install the build dependencies:

```sh
xcode-select --install
brew install catch2 cmake curl fmt ninja nlohmann-json openssl@3 sqlite
```

Homebrew's curl formula may be keg-only. If CMake selects the system curl
instead, configure with `-DCMAKE_PREFIX_PATH="$(brew --prefix curl)"` to select
the Homebrew installation without hard-coding its architecture-specific path.

## Build and test

Clone the repository and move into its root:

```sh
git clone https://github.com/evaikene/deferra.git
cd deferra
```

Configure and build:

```sh
cmake -S . -B .bld -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld
ctest --test-dir .bld/test --output-on-failure
```

The SQLite driver is enabled by default and is required to build `jobud`.
Disable it when only the backend-independent libraries and `jobuctl` are
needed:

```sh
cmake -S . -B .bld -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DJB_BUILD_SQLITE_DRIVER=OFF
cmake --build .bld
```

### Containerized Linux builds

Ready-made development images avoid reinstalling Linux dependencies for every
local validation run. Each environment keeps a separate incremental build tree
under the repository root; these `.bld-*` directories are ignored by Git.

Build an image once, then configure, build, and test with it:

```sh
tools/container image ubuntu
tools/container configure ubuntu
tools/container build ubuntu
tools/container test ubuntu

tools/container image alpine
tools/container configure alpine
tools/container build alpine
tools/container test alpine
```

The wrapper mounts the source tree read-only at its existing absolute path and
mounts only `.bld-ubuntu-2404` or `.bld-alpine-322` read-write. Containers run
with the invoking user's UID and GID, so persistent build output remains owned
by that user. The stable absolute path also keeps generated
`compile_commands.json` entries usable by host tools.

Use `run` for an arbitrary command or `debug` when a debugger or tracer needs
`SYS_PTRACE`:

```sh
tools/container run alpine cmake --build .bld-alpine-322 --target process-linux-test
tools/container debug alpine gdb --args .bld-alpine-322/test/process-linux-test
```

The wrapper deliberately does not add an init process, preserving container
PID 1 behavior relevant to the Linux process tests. Rebuild an image when its
Dockerfile changes, and recreate that environment's build tree after material
compiler or system-library changes. These local images complement rather than
replace the clean Ubuntu and Alpine jobs in GitHub Actions.

## Run the daemon

Start `jobud` with its required local socket and SQLite database paths:

```sh
.bld/src/jobud/jobud --socket /tmp/jobud.sock --database ./jobu.sqlite
```

HTTP scheduling accepts these optional daemon settings:

- `--http-concurrency N` sets the positive global HTTP concurrency limit and
  defaults to 16.
- `--http-proxy URL` configures one explicit HTTP or HTTPS proxy. When omitted,
  JobU ignores proxy environment variables.
- `--http-ca-bundle PATH` selects an explicit certificate-authority bundle.

HTTP jobs verify certificate trust and host identity by default.

CLI jobs execute asynchronously alongside HTTP jobs. `--cli-concurrency N`
sets the global CLI limit (default `4`), independently of the HTTP limit;
each queue's concurrency limit covers both runner types together. CLI
execution is denied when the daemon runs as root unless the unsafe
`--allow-root-cli` flag is supplied. Prefer a dedicated unprivileged account.
The override logs a warning and grants targets the daemon's root privileges;
`NoNewPrivs` does not remove privileges already held.

For example, create a queue and a one-shot command (replace the timestamp
with the desired UTC time):

```sh
.bld/src/jobuctl/jobuctl --socket /tmp/jobud.sock queue create commands
.bld/src/jobuctl/jobuctl --socket /tmp/jobud.sock job create \
    --queue-name commands --type cli --at 2030-01-01T12:00:00Z \
    --command /usr/bin/printf --arg '%s\n' --arg 'hello from JobU' \
    --working-directory / --env LANG=C --unset-env HOME --expected-exit-code 0
```

Commands and arguments are passed as an argv array, with no implicit shell
parsing or expansion. Use an absolute executable path, or supply an explicit
payload `PATH` through `--env PATH=/usr/bin:/bin` for a bare command name.
PATH entries must be nonempty absolute directories. The working directory
defaults to `/`, and stdin immediately reaches EOF.

The environment starts empty and receives only explicit `--env NAME=VALUE`
entries plus `JOBU_JOB_ID`, `JOBU_RUN_ID`, and `JOBU_ATTEMPT`. `--env NAME=`
sets an empty value. Repeat `--env`, `--unset-env`, and `--expected-exit-code`
as needed; duplicate names/codes and set/unset conflicts are rejected.
Expected exit codes default to `[0]`. Environment values are ordinary,
API-visible job data, not protected secrets.

Stdout and stderr are drained separately, including binary bytes. Retention
is bounded and keeps the first and last bytes when a limit is exceeded.
The default capture policy retains output on errors. The existing JSON/RPC
attribute model supports `output.capture`, `output.stdout_limit`,
`output.stderr_limit`, `job.timeout`, `cli.termination_grace`, and
`cli.retry_exit_codes`; a general attribute editor is not yet in `jobuctl`.
Timeout and scheduler cancellation terminate the attempt's process group,
with TERM followed by KILL after the configured grace period.

### Recovery and shutdown

On Linux and macOS, `jobud` validates and recovers its database before starting
jobs or accepting RPC connections. Use one daemon owner per database. After a
crash or shutdown, restart with the same database and let recovery finish;
if validation or storage fails, the daemon exits without serving work.
Recovery commits repairs in separate transactions, so a later startup can
continue safely after an interrupted recovery.

A command or HTTP request may have produced its external effect before the
daemon could commit its result. Recovery records that attempt as `Interrupted`
with an unknown outcome. It cannot infer success, failure, an exit code, or an
HTTP status. Lost in-memory output is recorded as `capture_lost=true`; empty
recovered output does not mean the target emitted nothing.

Each queue selects the treatment of its interrupted runs:

| `recovery_policy` | Startup behavior |
| --- | --- |
| `fail_interrupted` (default) | Make the run terminally Interrupted, even if retry allowance remains. |
| `retry_interrupted` | Schedule a new attempt of the same run when its saved retry policy has attempts remaining; otherwise make it terminally Interrupted. |

The existing queue-create option selects the policy, for example:

```sh
.bld/src/jobuctl/jobuctl --socket /tmp/jobud.sock queue create recoverable \
    --recovery-policy retry_interrupted
```

Selecting `retry_interrupted` does not increase the attempt limit. The
`retry.max_attempts` attribute defaults to **1**, including the original
attempt. Recovery uses the run's saved retry attributes and payload, together
with the queue's current recovery policy. A retry waits for its calculated
delay and normal suspension, barrier, and capacity checks. The next attempt
is created only when dispatched. Ordinary observed failures continue to use
normal job retry policy.

Retries can repeat external side effects. Use `retry_interrupted` when the
operation can tolerate repetition or the target supports an idempotency key.
JobU's creation idempotency keys do not make arbitrary command or HTTP effects
exactly once. With `fail_interrupted`, reconcile any unknown external effect
before deciding to submit replacement work.

Recovery preserves suspended owners and pending work. It completes drained
`Suspending` transitions and repairs missing recurring work, including for
suspended definitions. A recurring run made terminal receives its next future
occurrence; missed ticks are skipped. A manual run waiting for retry keeps its
barrier against schedule-owned work; terminal interruption releases it.

`SIGTERM` and `SIGINT` request immediate shutdown: stop admitting mutations and
dispatching attempts, disable late completion writes, cancel HTTP transfers,
and kill owned CLI process groups with `SIGKILL`, reaping direct children.
There is no application grace period or wait for normal job completion, and
no hard wall-clock deadline for kernel cleanup or proof that every descendant
has terminated. Repeated signals do not bypass cleanup. Unresolved durable
Running rows remain for the next startup's recovery; shutdown does not mark
them Cancelled. Signal-only shutdown exits successfully; a fatal runtime
failure or a reported database/signal cleanup failure makes the exit unsuccessful.

State-changing storage failures and detected corruption/invariant failures
trigger the same shutdown with a nonzero exit status. Ordinary management
validation/conflict errors and non-corruption read errors remain operation
errors. A failed commit acknowledgement may follow a successful durable
commit: after restart, recovery uses the actual stored state. Diagnose the
reported stable error code and address the storage problem before retrying
startup; do not assume an error proves the external effect or commit failed.

Phase 8 owns protected secret resolution, public cancellation, and run/output
management commands. The existing API is version 1.2 with the same management
method set.

## AI-supported development experiment

JobU is also an experiment in incremental, AI-supported software development:
each design stage is reviewed, built, and tested before the next stage begins.
The approved technical and phase-level design documents are available in
[docs/planning](docs/planning/README.md).

## License

JobU is released under the MIT License. See [LICENSE](LICENSE) for details.
