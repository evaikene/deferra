# JobU PHP extension

The optional `jobu` extension currently provides native string-backed enums and
an exception hierarchy. The private native layer now owns request-scoped event loops and independent
session bundles. Public Client connections, resources, and management methods
are not implemented yet. It supports PHP 8.1+ with 64-bit integers in NTS mode
on Linux/macOS. Runtime validation covers PHP 8.2–8.6 NTS; PHP 8.1 retains the
minimum build/API contract but is not in the test matrix. macOS runtime coverage
must be recorded separately. The independent [php-cxx helper](php-cxx.md) also
supports ZTS; the JobU extension rejects ZTS during configuration.

## Build and load

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
has a production Embed dependency. Embed is optional for production builds. The module lifecycle regression needs
matching Embed and a matching built daemon; CI requires both to prevent skipped
checks.

To reuse a previously built native SDK, configure the standalone module entry
point. This rebuilds only PHP-dependent sources and imports the Client SDK:

```sh
cmake -S src/jobu-php -B .bld-php-module \
    -DPHP_CONFIG=/path/to/php/bin/php-config \
    -DCMAKE_PREFIX_PATH=/path/to/relocated/sdk \
    -DJOBU_PHP_TEST_DAEMON=/path/to/matching/jobud
cmake --build .bld-php-module
/path/to/php/bin/php -n -dextension="$PWD/.bld-php-module/jobu.so" -m
ctest --test-dir .bld-php-module/test --output-on-failure
```

The SDK must match the source revision, distribution, architecture, compiler,
and system libraries. Its `Client` component provides PIC static libraries and
requires fmt; it does not import the SQLite backend or daemon. The module exports
the PHP loader entry point and hides its own and linked archive implementation
symbols. Release installation/packaging is deferred; use the explicit built
module path for now.

## Implemented PHP surface

All types are in namespace `JobU`. Enum case names use PascalCase and backing
values match the wire vocabulary:

| Enum | Cases |
| --- | --- |
| `QueueState` | Active, Suspending, Suspended, Deleted |
| `JobState` | Active, Suspending, Suspended, Deleted, Succeeded, Failed, Cancelled |
| `RunState` | Scheduled, Running, RetryWait, Succeeded, Failed, Interrupted, Cancelled |
| `AttemptState` | Pending, Running, Completed |
| `AttemptOutcome` | Succeeded, Failed, Interrupted, Cancelled |
| `RetryMode` | Blocking, Reschedule |
| `RecoveryPolicy` | FailInterrupted, RetryInterrupted |
| `JobType` | Cli, Http |
| `OutputChannel` | Stdout, Stderr, Body, Headers |

For example, `JobU\RunState::RetryWait->value` is `retry_wait`, and
`JobU\JobType::from('cli')` returns `JobU\JobType::Cli`. Enums retain PHP's
native `cases()`, `from()`, `tryFrom()`, and serialization behavior. An unfinished
attempt's outcome is represented by null, not an extra enum case.

`JobU\Exception` extends PHP's `\Exception`. Its direct subclasses are
`ConnectionException`, `TimeoutException`, `NotFoundException`,
`RevisionConflictException`, `InvalidCursorException`, `UnsavedChangesException`,
`OutcomeUnknownException`, and `UnsupportedFeatureException`.

The inherited constructor accepts `message`, integer `code`, and `previous`.
`getErrorCode(): ?string` and `getCategory(): ?string` read separate daemon
metadata, initially null for user-created exceptions. `getCode()` keeps its
standard integer meaning. Operational error translation and outcome-identity
metadata will accompany the RPC bridge. PHP `TypeError` and `ValueError` remain
native engine errors. Exceptions and their subclasses reject serialization and
unserialization, including user-defined serialization hooks.

Registration happens during MINIT. Zend owns the registered classes and
exception properties. RINIT creates a private request owner; its event loop is attached
lazily when the first native session is created. Each session owns a separate
socket, raw RPC client, and typed client. The request registry borrows sessions
and holds no Zend references. All native and Zend access stays on the PHP
calling thread; there is no worker thread or process-wide connection cache.

RSHUTDOWN clears the active request pointer, closes and detaches every remaining
session, then releases the loop and restores the previous thread binding.
Session close first removes callback receivers, then destroys typed/raw clients
and the socket. Cleanup is synchronous and does not dispatch pending work.
Later native wrapper destruction is safe even when Zend frees object storage
after RSHUTDOWN. Explicit close is idempotent and affects only that session.
The loop stays attached until request shutdown, even after all sessions close.

Production `JobU\Client` remains deferred until its constructor can eagerly
connect and complete the handshake. Test-only client/holder bindings exercise
this ownership layer without introducing placeholder production methods.
No C++ exception may unwind into Zend. Allocation failure remains fatal, and
PHP bailout does not guarantee C++ stack cleanup.

## Stub maintenance and validation

`src/jobu-php/jobu.stub.php` is authoritative. Generate and commit
`jobu_arginfo.h` when changing that surface, using PHP's `build/gen_stub.php` and
its matching PHP-Parser installation. The stub requests class entries and
PHP 8.1-compatible registration through `@generate-legacy-arginfo 80100`:

```sh
/path/to/php-8.5/bin/php -n -dextension=tokenizer \
    /path/to/php-8.5-source/build/gen_stub.php src/jobu-php/jobu.stub.php
```

Generation is a maintainer operation. Normal CMake builds and CI consume the
committed header without PHP sources, a generator, or a freshness check. Diagnose
the generated header through `module.cpp` with the selected ABI, and format only
handwritten C++.

CTest runs configuration rejection, module listing/information, Reflection and enum contracts, exception
behavior, and repeated child-process load/shutdown checks. An additional Embed
regression runs the production module hooks through three runtime lifetimes
with five requests each. It checks two connected sessions, close-one/use-other,
strong resource-to-client ownership, GC cycles, partial construction, pending
work, late native wrapper destruction, and reentrancy rejection. Linux also
checks descriptor accounting. This models reused PHP-FPM request boundaries;
it is not a claim of testing an actual PHP-FPM deployment.

The fixture classes and their generated arginfo are linked only into the test
executable. The production module exports no test API and does not link Embed.
Integrated builds use the `jobud` target. Standalone tests take
`JOBU_PHP_TEST_DAEMON`; missing Embed or daemon produces an explicit lifecycle
skip, which the CI gate rejects. The new lifecycle regression covers normal
request shutdown, not arbitrary recovery after Zend bailout or a process kill.
Standalone module tests also run the independent helper suite, including
relocated helper consumers.

The Ubuntu 26.04 and Alpine 3.24 PHP CI rows consume their native job's SDK from
**the same workflow run**. The archive also contains `fixtures/jobud`, staged
from that native build outside the installed Client SDK. PHP rows install the
existing daemon's runtime library dependencies and pass its path to the lifecycle
test. Each row rebuilds only helper/module/test sources.
The single `&php-versions` list in `.github/workflows/ci.yml` controls both
matrices. After extracting the matching archive, reproduce a row with:

```sh
CCACHE_DIR="$PWD/.ccache" tools/ci/php-test 8.5 \
    /usr/bin/php-config8.5-zend .bld/php /path/to/sdk
```

On Alpine use `/usr/bin/php-config85zend`. Omitting the SDK argument retains
helper-only validation. Both modes reject missing or skipped required checks.
