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

A terminal user interface (TUI) is available separately in
[JobuTU](https://github.com/rukkirahu/jobutu).

The optional [PHP extension](docs/php.md) provides a synchronous, typed API for
queues, CLI/HTTP jobs, schedules, retained history and output, secrets, and
statistics. The independent [php-cxx library](docs/php-cxx.md) supports C++
developers writing PHP modules. See [PHP builds and installation](#php-builds-and-installation)
for either component.

## Supported platforms

- Arch Linux (rolling release; primary development platform)
- Ubuntu 24.04 and 26.04
- Alpine Linux 3.22, 3.23, and 3.24
- macOS with Apple Clang and Homebrew dependencies

Windows is not a runtime target.

## Requirements

Building JobU requires CMake 3.20 or newer, a C++20 compiler, fmt, SQLite,
libcurl 7.85 or newer, nlohmann/json 3.9.0 or newer, Catch2 3.x, OpenSSL for tests,
and Ninja or another CMake-supported build tool.

Linux command execution requires a libc providing `_Fork()`, kernel pidfd
support, and `PR_SET_NO_NEW_PRIVS`. macOS command execution uses kqueue process
watches and public `fork()`.

### Ubuntu 24.04 and 26.04

```sh
sudo apt-get update
sudo apt-get install --yes --no-install-recommends \
    catch2 cmake g++ libcurl4-openssl-dev libfmt-dev libssl-dev libsqlite3-dev \
    ninja-build nlohmann-json3-dev
```

### Alpine Linux 3.22, 3.23, and 3.24

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

## PHP builds and installation

The [PHP API manual](docs/php.md) describes using `jobu.so`. The
[php-cxx guide](docs/php-cxx.md) describes writing other PHP modules with the
independent helper. Run the commands below from the repository root.

| Requirement | JobU PHP module | Independent php-cxx library |
| --- | --- | --- |
| CMake / language | CMake 3.20+, C++20 | CMake 3.20+, C++20; GCC 10 compile compatibility |
| PHP development installation | PHP 8.1+, 64-bit integers, NTS | PHP 8.1+, 64-bit `zend_long`, NTS or ZTS |
| Runtime validation matrix | PHP 8.2–8.6 NTS on Linux | PHP 8.2–8.6 NTS, plus focused PHP 8.5 ZTS on Linux |
| Platform boundary | Linux/macOS target; macOS runtime validation remains outstanding; no Windows runtime | No JobU/POSIX API dependency; macOS and Windows execution remain unvalidated |
| Production dependencies | Helper and matching JobU Client SDK; no Embed dependency | Selected PHP headers; no JobU or Embed link dependency |

PHP 8.1 is the minimum build/API contract and is excluded from the runtime test
matrix. GCC 10 compatibility applies to the independent helper, not the JobU
runtime. These are project compatibility boundaries; the development READMEs
record [module validation](src/jobu-php/README.md#stub-maintenance-and-validation)
and [helper validation](src/php-cxx/README.md#compatibility-validation).

### Build and load the JobU PHP module

PHP features default to off. Enabling the extension also builds the helper,
without requiring `JB_BUILD_PHP_CPP=ON`:

```sh
cmake -S . -B .bld-php -DJB_BUILD_PHP_EXTENSION=ON \
    -DPHP_CONFIG=/path/to/php/bin/php-config
cmake --build .bld-php
/path/to/php/bin/php -n -dextension="$PWD/.bld-php/src/jobu-php/jobu.so" --ri jobu
ctest --test-dir .bld-php/test --output-on-failure
```

Use a separate build directory for each PHP ABI and select `PHP_CONFIG`
explicitly. Its headers and CLI must belong to the same installation. PHP
version, debug mode, thread mode, and integer width are discovered from the
selected development files; configuration does not execute PHP or link Embed.
Zend checks the module ABI when loading it. Neither the module nor the helper
requires Embed in production. Full extension tests need matching Embed and a
matching built daemon; missing dependencies cause explicit skips. Use `-DBUILD_TESTING=OFF` for a production-only build. See the
[extension development notes](src/jobu-php/README.md) for validation requirements.

To reuse a previously built native SDK, first install it from the matching
native build:

```sh
cmake --install .bld --prefix /path/to/sdk --component Development
```

The SDK must match the source revision, distribution, architecture, compiler,
and system libraries. Its Client component requires fmt and excludes the SQLite
backend and daemon. Configure the standalone module entry point to rebuild only
PHP-dependent sources and import that Client SDK:

```sh
cmake -S src/jobu-php -B .bld-php-module \
    -DPHP_CONFIG=/path/to/php/bin/php-config \
    -DCMAKE_PREFIX_PATH=/path/to/sdk \
    -DJOBU_PHP_TEST_DAEMON=/path/to/matching/jobud
cmake --build .bld-php-module
/path/to/php/bin/php -n -dextension="$PWD/.bld-php-module/jobu.so" -m
ctest --test-dir .bld-php-module/test --output-on-failure
```

### Install and enable the JobU PHP module

Install the explicit `PhpExtension` component from either build entry point:

```sh
cmake --install .bld-php-module --component PhpExtension
/path/to/php/bin/php --ri jobu
```

This installs `jobu.so` into the selected `php-config --extension-dir` and
`jobu.ini` into `php-config --ini-dir`. The INI file contains
`extension = jobu.so`; new PHP processes using that scan directory load the
module automatically. Restart existing PHP-FPM or other persistent PHP workers
after deployment. PHP must use that installation's extension directory and scan
configuration; `php -n` deliberately disables INI loading.

The component is excluded from ordinary full installation and from the existing
Runtime/Development/Documentation archives. Select `--component PhpExtension`
explicitly. It installs neither native applications nor the independent helper
SDK, and does not create distribution-specific packages or modify a main php.ini.
The installed `jobu.ini` is a package-owned file; reinstalling restores its directive.

Use `JB_PHP_EXTENSION_DIR` and `JB_PHP_INI_DIR` at configuration time to override
the destinations. Both accept absolute or prefix-relative directories, without
parent traversal. An empty INI scan directory requires an explicit override and
corresponding PHP scan configuration. A relative extension override likewise
requires PHP's `extension_dir` to point to the deployed module directory.

Absolute destinations from `php-config` are unaffected by `cmake --install
--prefix`. Use `DESTDIR` to stage both files safely without writing into a live
PHP installation:

```sh
DESTDIR="$PWD/.bld/php-package-root" \
    cmake --install .bld-php-module --component PhpExtension
```

For prefix-relative packaging, configure for example
`-DJB_PHP_EXTENSION_DIR=lib/php/modules -DJB_PHP_INI_DIR=etc/php/conf.d`, then use
`--prefix /desired/prefix` at installation. `DESTDIR` may also stage that prefix.
Deploy the staged tree at its intended paths; staging alone does not enable the
module in the running system.

Build one module per PHP module API, debug/release mode, NTS mode and 64-bit
integer ABI. Zend checks compatibility when loading. The module version is the
JobU software version; it is distinct from the daemon RPC API version. Match the
native SDK revision, architecture, C++ runtime and system libraries as described
above; neither the module nor the unreleased C++ SDK promises cross-version C++
ABI compatibility. Relocation does not make a glibc artifact usable on musl.

### Build and install php-cxx independently

Select a PHP installation explicitly and use a separate build directory for
each PHP ABI and compiler configuration:

```sh
cmake -S src/php-cxx -B .bld/php-cxx-8.5 -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPHP_CONFIG=/path/to/php-8.5/bin/php-config
cmake --build .bld/php-cxx-8.5
ctest --test-dir .bld/php-cxx-8.5/test --output-on-failure
cmake --install .bld/php-cxx-8.5 --prefix /path/to/php-cxx-sdk \
    --component PhpCxxDevelopment
```

Production-only builds may use `-DBUILD_TESTING=OFF`; tests require the selected
CLI and use matching Embed when available. The standalone helper needs none of
the native JobU dependencies; tests use Catch2. To include it in a normal JobU
build instead, set `-DJB_BUILD_PHP_CPP=ON` and select `PHP_CONFIG`. Both PHP build
options default to `OFF`; normal JobU builds do not discover PHP.

Pass the install prefix through `CMAKE_PREFIX_PATH` and select the consumer's
`PHP_CONFIG`. Installed exports rediscover PHP rather than retain the producer's
PHP include paths. The module API, debug mode, NTS/ZTS mode, and integer width
must match the archive; a mismatch requires a matching PHP installation or a
separate library build. This unreleased package provides no cross-version C++
ABI guarantee. The package version is distinct from the Zend module API:
`find_package` requires the exact helper version, then checks the selected PHP
module API, debug mode, thread mode and integer width. Archive consumers must also
match architecture and the compiler's C++ ABI/runtime. Moving an SDK prefix does
not make its archive portable between libc implementations or architectures.

`PhpCxxDevelopment` remains separate from JobU's `Development` and `PhpExtension`
components. It installs the PIC static archive, public headers and CMake files;
it does not install a PHP runtime, an INI file or a shared helper library. Installed
consumers rediscover their PHP headers through their own `PHP_CONFIG`, and do not
need the producer's checkout/build tree, JobU packages or Embed. Keep SDK prefixes
separate for incompatible PHP ABIs; installing two ABI variants at the same prefix
would overwrite the archive and its compatibility metadata.

See the [consumer CMake examples](docs/php-cxx.md#consume-the-library-independently)
for `add_subdirectory`, `find_package`, and `PhpCxx::PhpCxx` usage.

### Build a module using php-cxx

The [complete module example](docs/php-cxx.md#complete-extension-example) provides
`demo.stub.php`, `demo.cpp`, `CMakeLists.txt`, and `check.php`. Save them together in
`/path/to/phpcxx-demo` and generate `demo_arginfo.h` using the linked development
workflow. After installing a matching helper as above, configure and load the module:

```sh
cmake -S /path/to/phpcxx-demo -B .bld/phpcxx-demo-8.5 \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH=/path/to/php-cxx-sdk \
    -DPHP_CONFIG=/path/to/php-8.5/bin/php-config
cmake --build .bld/phpcxx-demo-8.5
/path/to/php-8.5/bin/php -n \
    -d extension="$PWD/.bld/phpcxx-demo-8.5/phpcxx_demo.so" \
    /path/to/phpcxx-demo/check.php
```

Use a separate module build directory and helper SDK for each incompatible PHP ABI.
The example resolves Zend symbols from the loading PHP process and needs no Embed
library. Its CMake target intentionally builds only the module; it does not install
or enable it in a PHP configuration.

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

`--command` accepts a full command with shell-like quoting and escapes. Arguments
from it precede literal `--arg` values. JobU passes the resulting argv directly
to the program without shell evaluation or expansion. Quote or escape executable
paths containing spaces inside the command value. Use an absolute executable
path, or set an explicit `PATH` with `--env PATH=/usr/bin:/bin` for a bare name.

Run `jobud --help` or `jobuctl --help` to see the available options.

## Documentation

- [User Manual](docs/README.md)
- [`jobuctl` command guide](docs/jobuctl.md)
- [C++ control client guide](docs/cpp-client.md)
- [PHP API manual](docs/php.md)
- [Runnable PHP examples](examples/jobu-php/README.md)
- [php-cxx module-author guide](docs/php-cxx.md)
- [JobU PHP extension development](src/jobu-php/README.md)
- [php-cxx development](src/php-cxx/README.md)
- [JSON-RPC protocol](docs/protocol/README.md)

## AI-supported development experiment

JobU is also an experiment in incremental, AI-supported software development:
changes are designed, reviewed, built, and tested in small steps. Design
documents are available in [docs/planning](docs/planning/README.md).

## License

JobU is released under the MIT License. See [LICENSE](LICENSE) for details.
