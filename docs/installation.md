# Installing JobU

JobU requires CMake 3.20+, a C++20 compiler, fmt, nlohmann_json and libcurl
7.85+. SQLite3 is required when `JB_BUILD_SQLITE_DRIVER=ON` (the default), which
also builds `jobud`. Developer tests additionally require Catch2 and OpenSSL.
The default build enables tests and the C++ client example.

Use a fresh build directory when choosing an installation prefix:

```sh
cmake -S . -B .bld-install -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/opt/jobu \
    -DBUILD_TESTING=OFF -DJB_BUILD_EXAMPLES=OFF
cmake --build .bld-install --parallel
DESTDIR=/tmp/jobu-stage cmake --install .bld-install
```

`DESTDIR` stages files beneath `/tmp/jobu-stage`; it is never compiled into
runtime defaults. Omit it for installation at the configured prefix. No install
step creates users, active configuration, state/runtime directories, databases,
locks or sockets, or activates/restarts a service. Binaries are installed without
setuid/setgid permissions. External libraries are not bundled; use the runtime
dependencies appropriate to the build host and toolchain.

## Install components and files

`cmake --install` installs all components. Select one with `--component Runtime`,
`--component Development` or `--component Documentation`:

| Component | Default destination relative to the installation prefix |
| --- | --- |
| Runtime | `bin/jobud`, `bin/jobuctl`; `share/jobu/jobud.ini.example`, LICENSE and third-party notices; inactive systemd template sources in `share/jobu/services` |
| Development | Static libraries and `cmake/JobU` package metadata in the configured library directory; headers in `include/jb/<module>`; buildable C++ example sources in `share/jobu/examples/jobu-client` |
| Documentation | This guide, CLI/client/cron/secret guides, and protocol references/examples in the configured documentation directory |

GNUInstallDirs controls the binary, library, include, data and documentation
destinations. Explicit manifests exclude planning documents, shared evidence,
test helpers and application-private libraries. Reinstalling refreshes the
sample and templates while leaving an operator's active `jobud.ini` and data
untouched. Copy and secure the sample deliberately before using it.

Headers preserve the module layout, including `jb/jobu/client`, `jb/jobu/cli`,
`jb/jobu/http`, `jb/jobu/sqlite`, `jb/db/sqlite` and `jb/net/http`. Core also
installs `event_loop_backend.hpp`, `object_priv.hpp`, `signal_priv.hpp`,
`thread_context.hpp` and `timer_heap.hpp`, because public headers need them
transitively. These are implementation support, not independently supported APIs;
include their owning public headers. Static reusable libraries are built with PIC.

CPack archives and deployable service profiles have separate later gates. The
`.service.in` source is not a generated or validated unit.

## Using the installed SDK

Install the Development component and select the required package components:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_application LANGUAGES CXX)
find_package(JobU CONFIG REQUIRED COMPONENTS Core)
add_executable(my_application main.cpp)
target_link_libraries(my_application PRIVATE JobU::core)
```

Configure with `-DCMAKE_PREFIX_PATH=/opt/jobu` (or the actual staged SDK prefix).
Use namespaced headers such as `<jb/core/json.hpp>` and
`<jb/jobu/client/control_client.hpp>`. Targets supply C++20, include directories
and transitive link dependencies; do not add checkout paths or individual
archives to the consumer. The libraries are static and PIC, including Core for
use in another shared library. No PHP bindings or stable binary ABI are promised.

| Package component | Imported targets | Additional requirements |
| --- | --- | --- |
| Core (default when omitted) | `JobU::core` | fmt |
| Net | `JobU::net` | Core |
| Db | `JobU::db` | Core |
| Rpc | `JobU::rpc` | Core |
| Jobu | `JobU::jobu` | Core, Net, Db, Rpc |
| Client | `JobU::jobu-client` | Jobu, Rpc |
| SQLite | `JobU::db-sqlite`, `JobU::jobu-sqlite` | Db, Jobu, SQLite3; built with `JB_BUILD_SQLITE_DRIVER=ON` |
| HTTP | `JobU::net-http`, `JobU::jobu-http` | Net, Jobu, CURL 7.85+ |
| CLI | `JobU::jobu-cli` | Jobu, Core |

Package components select dependency discovery; they are distinct from the
Runtime/Development/Documentation installation components. Core and Client do
not discover SQLite, CURL, Catch2 or nlohmann_json. The producer still needs its
documented build dependencies. HTTP consumers using the system client link both
HTTP targets; the executor's generic HTTP-client interface does not itself
require libcurl. Unknown or unavailable required components reject the package
lookup. Optional components expose `JobU_<Component>_FOUND` without rejecting
otherwise satisfied required components. Repeated lookups can request additional
components.

Metadata uses project version `0.1.0`, independently of RPC API 1.4. An explicit
version request must match exactly; compiled archives retain CMake's architecture
check. Consumers also need a compatible compiler, C++ runtime and external
libraries. The SDK does not bundle these dependencies.

The default prefix-relative SDK layout can be copied or moved and discovered
from its new prefix. Absolute GNUInstallDirs destinations outside that prefix
are not relocatable. SDK relocation does not rewrite compiled daemon/client
operational defaults. See [the client guide](cpp-client.md) to build the installed
example and supply its endpoint explicitly.

## Operational paths

Operational defaults use separate absolute cache variables:

| Variable | Default | Application path |
| --- | --- | --- |
| `JB_JOBU_SYSCONFDIR` | `${CMAKE_INSTALL_PREFIX}/etc` | `jobud.ini` |
| `JB_JOBU_STATEDIR` | `${CMAKE_INSTALL_PREFIX}/var/lib` | `jobu.sqlite3` |
| `JB_JOBU_RUNDIR` | `${CMAKE_INSTALL_PREFIX}/var/run` | `jobud.sock` |

These values are shared by daemon/client defaults and the generated sample.
Changing `cmake --install --prefix` moves installation destinations, including
the sample in the data directory; it does not rewrite compiled operational
defaults or the paths inside that sample. Reconfiguring an existing build with
a different prefix retains initialized operational cache values. Use a fresh
build or explicitly reset all three `JB_JOBU_*DIR` variables.

A future Linux system profile uses `/usr` with explicit `/etc/jobu`,
`/var/lib/jobu` and `/run/jobu` overrides. A private foreground deployment can
use a short writable prefix. Provision protected parents and final-user-owned
state/runtime leaves with mode 0700. A mode-0660 socket needs a matching trusted
group, a mode-0750 runtime leaf and safe traversal; that group has full daemon
authority. Do not use raw database/socket paths in shared `/tmp`.

The sample documents every startup directive and daemon-default attribute.
Optional account/proxy/CA settings and attribute overrides are commented out.
Attribute duration values are JSON integer milliseconds; JSON strings use an
outer INI single-quote wrapper, for example `defaults.retry.mode = '"reschedule"'`.
There are no inline comments or environment substitutions. Run
`jobud --check-config --config /absolute/protected/jobud.ini` under the intended
identity before startup. Check mode performs validation without creating daemon
resources; successful validation does not prove later resource access succeeds.

Use [the CLI guide](jobuctl.md) for shared default endpoint behavior and explicit
`--socket` overrides, and [the client guide](cpp-client.md) for borrowed lifetimes
and the caller-supplied endpoint. Managed-service activation, privileged
deployment evidence and the complete operations/configuration reference remain
their designated Phase 9 stages.
