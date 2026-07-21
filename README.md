# JobU

JobU is a job scheduling and execution service for running command-line and HTTP jobs in the background.

The planned v1 service provides multiple independently managed queues, immediate and scheduled jobs, cron-based recurring jobs, retry and timeout policies, job history, output capture, and queue/job lifecycle operations. A local JSON-RPC API over a Unix-domain socket is exposed by the `jobud` daemon and used by the `jobuctl` command-line client.

JobU is designed as a small, portable C++20 service with a limited set of external dependencies. SQLite is the v1 persistence backend.

> [!IMPORTANT]
> JobU is under active development and is not ready for production use. The repository currently contains the foundational libraries, SQLite database support, local IPC and JSON-RPC infrastructure, and early `jobud`/`jobuctl` functionality. The complete scheduling and job-execution behavior described above is still being implemented.

## Platforms

Current build and test coverage:

- Ubuntu 24.04
- Alpine Linux 3.22

Linux is the primary development platform. macOS is a planned functional v1 target; its core event-loop backend exists, but the complete local IPC and JobU runtime are not yet available there.

Windows is not a v1 runtime target. Public interfaces should remain portable enough for a future Windows backend, but Windows process execution and service integration are intentionally outside the v1 scope.

## Requirements

Building the current source tree requires:

- CMake 3.20 or newer
- a C++20 compiler
- [fmt](https://github.com/fmtlib/fmt)
- [SQLite](https://sqlite.org/)
- [nlohmann/json](https://github.com/nlohmann/json)
- [Catch2](https://github.com/catchorg/Catch2) 3.x for tests
- Ninja or another CMake-supported build tool

The planned HTTP runner will also require libcurl. It is not yet a build dependency of the current source tree.

### Ubuntu 24.04

Install the build dependencies:

```sh
sudo apt-get update
sudo apt-get install --yes --no-install-recommends \
    catch2 \
    cmake \
    g++ \
    libfmt-dev \
    libsqlite3-dev \
    ninja-build \
    nlohmann-json3-dev
```

### Alpine Linux 3.22

Install the build dependencies:

```sh
sudo apk add --no-cache \
    catch2-3 \
    cmake \
    fmt-dev \
    g++ \
    nlohmann-json \
    samurai \
    sqlite-dev
```

## Building

Configure and build a Debug build from the repository root:

```sh
cmake -S . -B .bld -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld
```

The build produces the `jobud` daemon and `jobuctl` client together with the supporting static libraries and test executables. At this stage the executables expose only the functionality implemented so far; they are not yet a complete job scheduler.

The SQLite driver is enabled by default. To build without it:

```sh
cmake -S . -B .bld -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DJB_BUILD_SQLITE_DRIVER=OFF
cmake --build .bld
```

## Running the tests

After building, run all registered tests with:

```sh
ctest --test-dir .bld/test --output-on-failure
```

## AI-supported development experiment

JobU is also an experiment in AI-supported software development. The project is being designed and implemented incrementally with AI assistance, with each stage reviewed, built, and tested to see how far a substantial C++ system can be developed effectively using this workflow.
