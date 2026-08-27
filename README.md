# JobU

JobU (Job Orchestration and Broker Utility) is a portable C++20 service for
running scheduled command-line and HTTP jobs in the background. The planned v1
service includes independently managed queues, retries and timeouts, recurring
jobs, execution history, and output capture. `jobuctl` communicates with the
`jobud` daemon through a local JSON-RPC API over a Unix-domain socket, and
SQLite provides persistence.

> [!IMPORTANT]
> JobU is under active development and is not ready for production use. The
> repository currently provides the foundational libraries, SQLite support,
> local IPC and JSON-RPC, and deterministic scheduling tested with a fake
> executor. Real command-line and HTTP execution are not yet available, and
> the scheduler is not yet wired into `jobud`.

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

Linux is the primary development platform. Linux and macOS both support the
current local IPC, JSON-RPC, daemon, and control-client functionality. Windows
is not a v1 runtime target.

## Requirements

The build requires CMake 3.20 or newer, a C++20 compiler, fmt, SQLite, libcurl
7.85 or newer, nlohmann/json, Catch2 3.x for tests, and Ninja or another
CMake-supported build tool. The private system HTTP backend verifies its linked
libcurl runtime before construction; real HTTP job execution is not yet
available.

### Ubuntu 24.04

```sh
sudo apt-get update
sudo apt-get install --yes --no-install-recommends \
    catch2 cmake g++ libcurl4-openssl-dev libfmt-dev libsqlite3-dev \
    ninja-build nlohmann-json3-dev
```

### Alpine Linux 3.22

Alpine does not ship `sudo` by default, so run the following as root (or
install `sudo` first):

```sh
apk add --no-cache \
    catch2-3 cmake curl-dev fmt-dev g++ nlohmann-json samurai sqlite-dev tzdata
```

### macOS

Install the Xcode Command Line Tools and
[Homebrew](https://brew.sh/), then install the build dependencies:

```sh
xcode-select --install
brew install catch2 cmake curl fmt ninja nlohmann-json sqlite
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

## AI-supported development experiment

JobU is also an experiment in incremental, AI-supported software development:
each design stage is reviewed, built, and tested before the next stage begins.
The approved technical and phase-level design documents are available in
[docs/planning](docs/planning/README.md).

## License

JobU is released under the MIT License. See [LICENSE](LICENSE) for details.
