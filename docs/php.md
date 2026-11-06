# JobU PHP extension

The optional `jobu` extension currently provides native string-backed enums and
an exception hierarchy. Client connections, resources, and management methods
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
has a production Embed dependency. Embed is optional for helper regression
execution; CI requires it to prevent skipped checks.

To reuse a previously built native SDK, configure the standalone module entry
point. This rebuilds only PHP-dependent sources and imports the Client SDK:

```sh
cmake -S src/jobu-php -B .bld-php-module \
    -DPHP_CONFIG=/path/to/php/bin/php-config \
    -DCMAKE_PREFIX_PATH=/path/to/relocated/sdk
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
exception properties; the current request hooks allocate no session resources
and preserve no request state. No C++ exception may unwind into Zend. Allocation
failure remains fatal, and PHP bailout does not guarantee C++ stack cleanup.

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
behavior, and repeated child-process load/shutdown checks. These do not claim
reused PHP-FPM request/session coverage. Standalone module tests also run the
independent helper suite, including relocated helper consumers.

The Ubuntu 26.04 and Alpine 3.24 PHP CI rows consume their native job's SDK from
**the same workflow run**. Each row rebuilds only helper/module/test sources.
The single `&php-versions` list in `.github/workflows/ci.yml` controls both
matrices. After extracting the matching archive, reproduce a row with:

```sh
CCACHE_DIR="$PWD/.ccache" tools/ci/php-test 8.5 \
    /usr/bin/php-config8.5-zend .bld/php /path/to/sdk
```

On Alpine use `/usr/bin/php-config85zend`. Omitting the SDK argument retains
helper-only validation. Both modes reject missing or skipped required checks.
