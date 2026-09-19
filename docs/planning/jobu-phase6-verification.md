# Phase 6 Stage 6.19 verification record

Recorded 2026-09-19. Linux acceptance and source audits passed. Closure remains
pending recovery of the native macOS provenance described below and the final
clean-tree check after user-managed review/commit/merge. No production code,
tests, build configuration, dependencies, or diagnostic policy changed in 6.19.

## Tested source

Primary Linux and Alpine tested merged Stage 6.18 / PR #152:
`d637ba65f878f4fa0716d56d730be6b11a35f14a`, tracked Git tree
`80c3d32c558705f60ea124cdf1294040f7679200`. The working copy was clean before
these documentation edits. Audit baseline:
`11bf10c1c5378af790c55842210091eb53f14e4b`.

## Acceptance results

| Environment | Build | Tests |
| --- | --- | --- |
| Primary Linux, fresh Debug SQLite | 375 steps, no compiler/linker warnings or errors | 114/114 CTest targets, 15.21 s |
| Primary Linux, fresh Debug without SQLite | 288 steps, no compiler/linker warnings or errors | Compile-only, intentionally no second CTest run |
| Isolated Alpine UID 0 | 63 incremental daemon-test build steps, no warnings | 6/6 daemon cases, 5099 assertions, no skips |
| Native macOS, carried Stage 6.18 record | 194 remaining incremental Debug steps, no warnings | 115/115 CTest targets, 63.35 s |

Primary Linux and macOS each skipped the internal real-root daemon denial case
because their identities were UID 1001 and UID 501. Alpine executed that case
without `--allow-root-cli`, and the other five cases exercised real root helper
execution with the explicit override and asserted its unsafe startup warning.
All Linux runtime marker-leakage and HTTP regression assertions passed.

The no-SQLite build produced core, rpc, net, net-http, jobu, jobu-http, and
jobu-cli libraries. It exposed no jobud, db-sqlite, or jobu-sqlite targets and
produced no jobud executable. Both primary-Linux directories were absent before
configuration. No test retries or source fixes were required.

## Primary Linux environment

| Component | Observed version |
| --- | --- |
| Distribution | Arch Linux rolling |
| Kernel / architecture | Linux 7.2.6-zen2-1-zen / x86_64 |
| Effective identity | UID 1001, GID 1001 |
| Compiler | GCC 16.2.1 20260810; package 16.2.1+r23+gd564253eb6c8-1 |
| libc | glibc 2.44; package 2.44+r24+g16be1518495f-1 |
| CMake / Ninja | 4.4.3 / 1.13.2 |
| fmt | 12.2.0; jobud resolves libfmt.so.12 |
| SQLite | 3.53.4 configured headers/package and runtime library |
| libcurl | 8.22.0 configured headers/package and runtime library |
| nlohmann_json | 3.12.0 |
| Catch2 | 3.16.0; confirmed by the daemon-test executable output |
| OpenSSL | 3.6.4 configured/package and curl runtime backend |

Provenance includes the configure logs/caches, compiler/build-tool version
commands, pkg-config, installed package versions, jobud's ldd output, and
sqlite3_libversion/curl_version calls against the configured `/usr/lib`
libraries. curl additionally reports zlib 1.3.2, brotli 1.2.0, zstd 1.5.7,
libidn2 2.3.8, libpsl 0.21.5, libssh2 1.11.1, nghttp2 1.70.0, ngtcp2 1.25.0,
nghttp3 1.18.0, and mit-krb5 1.22.2. No dependency was added or upgraded.

## Primary Linux commands

```sh
cmake -S . -B .bld-phase6 -G Ninja -DCMAKE_BUILD_TYPE=Debug
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --build .bld-phase6 --verbose
ctest --test-dir .bld-phase6/test --output-on-failure \
  --output-log /home/enar/src/deferra/.bld/phase6-evidence/stage6.19.2-ctest.log
cmake -S . -B .bld-phase6-no-sqlite -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DJB_BUILD_SQLITE_DRIVER=OFF
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --build .bld-phase6-no-sqlite --verbose
```

## Source and diagnostics audit

All 16 production Object subclasses were inventoried. Process and the CLI
executor extend one Object-owned allocation and bind owners after base
construction. Process events use signals; executor slots are receiver-aware;
accepted-operation and router completions remain exact callbacks. The executor
group rejects parented Objects and owns accepted implementations exclusively.

Inspection covered per-registration anchor invalidation before unwatch/close,
non-signallable post-reap Finishing, group KILL before retained-leader reap,
gated launch, parent-prepared child data, signal/descriptor hygiene, bounded
output callbacks and final drain, and no waiter thread or repeated wait timer.
The full Linux Process, watch, policy, executor, scheduler, and daemon suites
then passed. Runner destruction evidence does not prove daemon signal shutdown.

Of 143 changed C++ files, 139 direct clangd queries passed with compiler,
include-cleaner, and enabled clang-tidy diagnostics. process_priv.hpp was
covered through process.cpp. Three macOS-only translation units require native
coverage; Linux checks of portable kqueue helpers do not cover Apple-only code.
Stage 6.18 records clean native diagnostics for its seven changed C++ files;
the earlier native Core diagnostics record is not available in this checkout.

All 55 changed public headers passed first-include compilation with configured
compiler flags and the native/dependency-type boundary scan. Public Doxygen
and non-obvious ownership/ordering comments were audited. An initial probe
adding -Wall -Wextra -Werror found unchanged Signal aggregate-initializer
warnings at signal_priv.hpp:141 and :159 through Object's include chain. The
user approved using configured flags. No private Signal header was compiled
separately, no suppression was added, and Signal was not modified.

Schema/DDL, management RPC implementation, persistence implementations, and
HTTP decoder/executor implementations are unchanged from the baseline. Schema
is v1, management remains 15 methods plus system.info, and API is 1.2. The
planned runner-neutral diagnostic limit is 64 MiB; HTTP headers remain 4 MiB.
Payload, PATH, environment, exit policy, capture metadata, durable-before-start,
atomic completion, concurrency, and safe-marker assertions passed in the suite.

## Isolated root evidence

| Component | Observed version |
| --- | --- |
| Alpine | 3.22.5 |
| Kernel / architecture | Host Linux 7.2.6-zen2-1-zen / x86_64 |
| Test identity | UID 0, GID 0 |
| Compiler | Alpine GCC 14.2.0; package 14.2.0-r6 |
| libc | musl 1.2.5-r12 |
| CMake | 3.31.7-r1 |
| Ninja-compatible builder | samurai 1.2-r7; ninja --version reports 1.9 |
| fmt | 11.2.0-r0 |
| SQLite | sqlite-libs 3.49.2-r1 |
| curl | libcurl 8.14.1-r3 |
| nlohmann_json | 3.11.3-r0 |
| Catch2 | catch2-3 3.8.1-r0 |
| OpenSSL libraries | libssl3/libcrypto3 3.5.8-r0 |

Package versions were read from the image's installed-package database. These
container versions are separate from primary-host versions and do not provide
native macOS coverage. Containers share the host kernel.

The existing image was deferra-dev:alpine-3.22, immutable ID
`sha256:ddd3de474fdc048e82cab15c454a235640e03f1a14fd3f597745c4c577da8a78`.
Configuration/build used tools/container with the host identity. Tests used a
disposable UID-0 container, read-only root/source, no network, and a writable
256 MiB /tmp, with JOBU_TEST_ALLOW_ROOT_CLI=1. No privileged mode, additional
capabilities, or Docker socket mount was used. The exact command and harness
are retained in the local Stage 6.19 record and evidence directory.

An initial package-metadata probe stopped before tests because the openssl
executable package was absent. Reading installed libssl3/libcrypto3 records
resolved the probe; no package or test change was needed.

## Carried native macOS evidence

| Component | Measured value |
| --- | --- |
| OS | macOS 26.6.2, build 25G83 |
| Kernel / architecture | Darwin 25.6.0 / arm64 |
| Identity | effective UID 501 |
| Compiler | Apple Clang 21.0.0, clang-2100.3.34.2 |
| CMake / generator | 4.4.3 / Ninja 1.13.2 |
| Build | Debug, SQLite enabled, existing .bld; incremental, not clean |
| SQLite | configured SDK headers 3.54.0; loaded /usr/lib/libsqlite3.dylib reports 3.51.0 |
| curl | configured SDK headers 8.7.1; loaded /usr/lib/libcurl.4.dylib reports 8.7.1 |
| curl runtime backend | SecureTransport, LibreSSL 3.3.6, zlib 1.2.12, nghttp2 1.68.1 |
| fmt | headers and linked library 12.2.0 |
| nlohmann_json | headers 3.12.0 |
| Catch2 | test executable reports 3.5.3 |
| OpenSSL for tests | Homebrew 3.6.4; libcrypto runtime reports 25 Aug 2026 build date |

Versions came from sw_vers, uname, compiler/build-tool version commands,
configured header macros, CMake cache paths, otool -L on jobud, and ctypes calls
to sqlite3_libversion, curl_version, and OpenSSL_version on the exact libraries.
The SQLite SDK header/runtime difference is recorded explicitly; all database
and daemon tests passed against the stated runtime. No dependency was added.

Counts below are from the final full run, not sums of repeated focused runs.
Assertions involving readiness loops can vary between runs.

| Area | Native evidence |
| --- | --- |
| Process exit monitoring | event-loop-process-watch-test: 13 cases; shared dispatch plus native one-shot kqueue cases |
| Core lifecycle, output, cleanup | process-posix-test: 34 cases / 5267 assertions; process-macos-test: 4 / 136 |
| CLI payload / exit policy / capture | cli-job-payload-test: 9 / 324; cli-exit-policy-test: 11 / 132; cli-capture-test: 5 / 75 |
| Real CLI executor and fake seams | cli-attempt-executor-test: 15 / 677, including five formerly Linux-only native cases |
| Real mixed scheduler | cli-scheduler-integration-test: 10 / 7658 |
| Daemon startup and composition | jobud-startup-test: 6 / 131 |
| Real jobuctl CLI creation | jobuctl-cli-creation-test: 7 / 1011 |
| Real CLI daemon | jobud-cli-integration-test: 5 passed, 1 root case skipped / 4745 assertions passed |
| HTTP and service regressions | Full suite includes HTTP executor/scheduler/daemon, RPC/local sockets, client management, and standalone public headers |

The native Stage 6.18 report tested working-copy revision
`f76b7f29c4db02b66f0ad1d9bd55f5d787377544` above
`06de1c38c2c162962dd1126ff2a82751ca49c90a`. The pre-6.19 handoff records a
zero-file jj diff from that tested revision to merged main. That historical
comparison is carried evidence: the tested revision cannot be resolved in this
checkout, so it was not independently repeated. Its patch was recorded with
SHA-256 d9f497e872713216ff1414a0e8a7a565c5b22134714077fdad5f3c8b874617fd.

The local Stage 6.18 report is available, but the referenced Stage 6.17 Core
child-path/at-fork audit and original native logs are not present here. No
remote macOS execution tool is available in this session. Recover those records
and the tree comparison before claiming independently evidenced final closure.
The final native full suite revalidated the registered Core tests according to
the Stage 6.18 report; no new macOS run was performed in 6.19.

Actual macOS root denial and unsafe root override remain unverified, as allowed
by Stage 6.18 when no safe isolated root environment exists. Injected identity
tests and Linux root runs do not fill that native gap. macOS uses public fork;
host at-fork handlers are outside the Process-controlled post-return safety
guarantee. Strict privilege-gain prevention remains unsupported on macOS.
Known zombie-only group EPERM diagnostics remain visible in passing tests.

## Artifacts and remaining closure work

The ignored `.codex/jobu-phase6-stage6.19-validation.md` contains the detailed
source audit, commands, test counts, and initial probe failures. Generated
`.bld/phase6-evidence/stage6.19.*` artifacts retain per-file diagnostics, header
probes, build/CTest logs, caches, dependency versions, and the root harness.
Native artifacts were recorded on the macOS workspace; their present availability
has not been checked. Preserve them before removing its .bld directory.

After the missing native provenance is recovered and these documentation
changes are reviewed and committed by the user, check jj status, record the
final revision, and compare against the tested source. Documentation-only
changes need no full rebuild. Any source or build-configuration delta requires
appropriate renewed validation. Until that check passes, Phase 6 remains open.

Phase 7 owns recovery and coordinated daemon signals, admission stop, active
runner termination, and infrastructure shutdown. Default signal termination
can bypass Object destruction. Phase 8 owns protected secrets, expanded
history/output/statistics RPC, public Run Now/cancel, and remaining client APIs.
Literal Phase 6 environment values are persisted job data, not protected secrets.
