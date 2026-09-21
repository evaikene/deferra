# JobU

JobU (Job Orchestration and Broker Utility) is a portable C++20 service for
running scheduled command-line and HTTP jobs in the background. It provides
independently managed queues, retries and timeouts, recurring jobs, execution
history, and output capture. `jobuctl` communicates with the `jobud` daemon
through a local JSON-RPC API over a Unix-domain socket, and SQLite provides
persistence.

> [!IMPORTANT]
> JobU is under active development and is not ready for production use.

## Components

- **`jobud`** — the background daemon that hosts queues, schedules jobs, and
  executes them. It exposes a local JSON-RPC API over a Unix-domain socket.
- **`jobuctl`** — the command-line client for managing queues and jobs through
  `jobud`.

## Supported platforms

- Ubuntu 24.04
- Alpine Linux 3.22
- macOS with Apple Clang and Homebrew dependencies

Windows is not a runtime target.

## Requirements

Building JobU requires CMake 3.20 or newer, a C++20 compiler, fmt, SQLite,
libcurl 7.85 or newer, nlohmann/json, Catch2 3.x, OpenSSL for tests, and Ninja
or another CMake-supported build tool.

Linux command execution requires a libc providing `_Fork()`, kernel pidfd
support, and `PR_SET_NO_NEW_PRIVS`. macOS command execution uses kqueue process
watches and public `fork()`.

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

Configure, build, and test:

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

## Run JobU

Start `jobud` with a local socket and SQLite database path:

```sh
.bld/src/jobud/jobud --socket /tmp/jobud.sock --database ./jobu.sqlite
```

Use `jobuctl` with the same socket to manage queues and jobs. For example,
create a queue and schedule a one-shot command (replace the timestamp with the
desired UTC time):

```sh
.bld/src/jobuctl/jobuctl --socket /tmp/jobud.sock queue create commands
.bld/src/jobuctl/jobuctl --socket /tmp/jobud.sock job create \
    --queue-name commands --type cli --at 2030-01-01T12:00:00Z \
    --command /usr/bin/printf --arg '%s\n' --arg 'hello from JobU'
```

Commands and arguments are passed directly as an argv array; JobU does not
perform shell parsing or expansion. Use an absolute executable path, or set an
explicit `PATH` with `--env PATH=/usr/bin:/bin` for a bare command name.

Run `jobud --help` or `jobuctl --help` to see the available options.

## AI-supported development experiment

JobU is also an experiment in incremental, AI-supported software development:
changes are designed, reviewed, built, and tested in small steps. Design
documents are available in [docs/planning](docs/planning/README.md).

## License

JobU is released under the MIT License. See [LICENSE](LICENSE) for details.
