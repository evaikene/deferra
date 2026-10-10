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
| Development | Static libraries in the configured library directory; headers in `include/jb/<module>`; C++ example sources in `share/jobu/examples/jobu-client` |
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

Stage 9.22 stages the libraries, headers and example sources. `JobU` package
configuration/imported targets and installed example adaptation arrive in Stage
9.23. The installed example's current CMake file still describes its build-tree
target. CPack archives and deployable service profiles have separate later gates.
The `.service.in` source is not a generated or validated unit.

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
