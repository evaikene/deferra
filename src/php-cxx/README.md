# php-cxx development

This directory implements the independent `php-cxx` library. C++ module authors should
start with the [public library guide](../../docs/php-cxx.md). Generic [build and
installation instructions](../../README.md#build-and-install-php-cxx-independently) are
in the root README. All shell commands below run from the repository root.

## PHP discovery and test fixtures

Use separate build directories for different PHP installations. Discovery runs
`php-config` and compiles header probes; it does not execute PHP or require Embed. It
records the selected CLI, extension directory, PHP version, module API, debug/ZTS mode,
integer width, PHP link flags, and optional Embed paths. The PHP executable's link flags
are metadata, not helper dependencies.

`BUILD_TESTING=OFF` disables tests and their CLI availability check. Tests cover the
compiled ABI metadata, a small CLI-loadable consumer, independent build-tree
consumption, relocated installation, discovery failures, and Zend ownership. Ownership,
view, argument, conversion, exception, and native binding tests use the existing Catch2
dependency and matching optional Embed library in dedicated executables. The
ownership/conversion and native binding harnesses each initialize one request. Their
external owners die before shutdown; native binding accounting also checks objects
deliberately retained in PHP globals until shutdown. The CLI and relocated consumers
exercise the same generated fixtures.

The lifecycle owner is private test support, not a production lifecycle API.
The lifecycle harness adds three complete runtime cycles, five request cycles within one
runtime, and (in ZTS builds) two concurrent workers with three requests each. Each
request exercises binary strings, array separation, conversions, exception preservation,
native bindings, clone failures, GC cycles, and shutdown cleanup. User functions/classes
and request globals must disappear between requests. Thread-local native counters must
balance after every shutdown; no Zend pointers are retained to inspect them afterward.
Barriers keep both ZTS requests active while checking distinct globals and pending
exception state.

Two additional CTest cases run deliberate bailout and frameless-error scenarios in child
processes. They require the expected bailout, a successful shutdown marker, and normal
child exit; a crash or timeout fails the test. Their jump regions contain no C++ objects
requiring destruction. These are narrow checks of host teardown, not a guarantee of
recovery after arbitrary PHP failures.

There are twelve helper checks. Missing Embed explicitly skips the six Embed checks;
compilation and the six CLI/build/install checks remain enabled. Only test targets link
Embed. Neither consumers nor production helper linkage acquire an Embed or
thread-library dependency from these fixtures.

The test-only `consumer/functions.stub.php` and `consumer/objects.stub.php` are
authoritative for their checked-in `*_arginfo.h`, generated with PHP's
`build/gen_stub.php`. The object stub requests PHP 8.1-compatible registration through
`@generate-legacy-arginfo 80100`; generation uses the existing PHP 8.5 generator and
PHP-Parser installation. Normal builds and tests consume that header directly; they do
not need PHP sources, a stub compiler, or a generated-header freshness check. The
fixture deliberately uses `mixed` input so helper parsing is exercised rather than
preempted by arginfo.

## Native storage and lifecycle fixtures

Each `ObjectBinding` specialization has an immutable handler table with no class
pointers or request state. One Zend allocation contains the C++ storage prefix followed
by the Zend object and variable-length property slots; there is no separate native
allocation. The public [binding contracts](../../docs/php-cxx.md#embedded-native-objects)
cover the construction, tracing, clone, comparison and teardown rules module authors
must follow.

Runtime/module startup, request startup, and execution-thread initialization are
different boundaries in the embedding fixtures. `php_embed_init()` performs all
startup work, including an initial request; `php_embed_shutdown()` shuts that request
and the runtime down. For subsequent requests in the same runtime, pair
`php_request_shutdown()` with `php_request_startup()`. Destroy external C++ owners
before ending each request and resolve pending exceptions before normal teardown.

For ZTS workers, initialize the process/module once before starting workers, attach
each worker with `ts_resource(0)`, and run its request operations on that same thread.
After its final request shutdown, call `ts_free_thread()` before joining all workers
and shutting down the process runtime. Only native results and synchronization cross
threads. These host-side lifecycle operations belong to the fixtures, not to an
extension loaded by PHP, whose host already manages its runtime and requests.

## Generating arginfo

Keep PHP signatures in an authoritative `.stub.php` file and generated registration
in its sibling `_arginfo.h`. The [module-author example](../../docs/php-cxx.md#complete-extension-example)
uses `demo.stub.php` with `@generate-class-entries` and
`@generate-legacy-arginfo 80100`. To regenerate it during module development:

```sh
/path/to/php-8.5/bin/php -n -dextension=tokenizer \
    /path/to/php-src/build/gen_stub.php /path/to/phpcxx-demo/demo.stub.php
```

Use a compatible PHP source checkout with the generator's PHP-Parser dependency
already provisioned; the generator may otherwise try to download it. The command
writes `demo_arginfo.h` beside the stub. For this repository's consumer fixtures,
substitute `test/php-cxx/consumer/functions.stub.php` or
`test/php-cxx/consumer/objects.stub.php`. Commit the generated headers with their
stubs when publishing module sources. Generation is a developer operation, not a
normal build dependency or CI freshness gate.

## Compatibility validation

The functional matrix is PHP 8.2–8.6 NTS plus PHP 8.5 ZTS. Use each installation's
explicit `PHP_CONFIG` and a separate build directory, following the commands in the root
README; record the actual patch version, debug mode, and thread mode. PHP 8.1 remains
the minimum contract but is excluded from this execution matrix. The GCC 10 check below
compiles the independent helper and public-template consumers for the same six ABIs,
without running PHP or Embed.

## Memory checks

Use a separate sanitizer build. For example, with a compiler providing ASan, UBSan, and
LeakSanitizer:

```sh
cmake -S src/php-cxx -B .bld/php-cxx-asan-8.5 -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPHP_CONFIG="$HOME/php/8.5/bin/php-config" \
    '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build .bld/php-cxx-asan-8.5 --target \
    php-cxx-ownership-test php-cxx-conversion-test php-cxx-object-test \
    php-cxx-lifecycle-test php-cxx-bailout-test
USE_ZEND_ALLOC=0 ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
    ctest --test-dir .bld/php-cxx-asan-8.5/test --output-on-failure \
    -R 'php-cxx-(ownership|conversion|object|lifecycle)-test$'
USE_ZEND_ALLOC=1 ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
    ctest --test-dir .bld/php-cxx-asan-8.5/test --output-on-failure \
    -R 'php-cxx-(bailout|fatal)-isolation-test$'
```

Repeat with PHP 8.5 ZTS and a distinct build directory. These commands instrument the
helper and fixture code, including binding templates; an uninstrumented `libphp` does
not gain compiler instrumentation. Disabling Zend's allocator for normal cases exposes
individual Zend allocations to the memory checker. The bailout cases deliberately retain
the Zend allocator: unclean shutdown may leave individual blocks for request-arena
reclamation. Native destruction counters still apply, but this does not establish
individual-allocation safety after a bailout. No test deliberately jumps over a live
automatic C++ owner: doing so would violate C++ `longjmp` requirements, not just skip a
convenient cleanup. LeakSanitizer also needs a host environment that permits its process
inspection.

Valgrind can exercise the shared conversion and object fixtures through a matching,
**unsanitized** CLI module build:

```sh
USE_ZEND_ALLOC=0 valgrind --error-exitcode=97 --leak-check=full \
    --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect \
    "$HOME/php/8.2/bin/php" -n \
    "-dextension=$PWD/.bld/php-cxx-8.2/test/php-cxx-module-test.so" \
    test/php-cxx/consumer/objects-check.php
```

Repeat for `consumer/check.php` and matching ZTS paths. Inspect the complete report,
including reachable blocks and the tool's default suppressions; no project suppression
file is used. On the tested Linux installations, PHP CLI's built-in Valgrind
string-comparison replacement works, while Embed's shared `libphp` reports uninitialized
reads in its assembly `zend_string_equal_val`. Those Embed runs are not clean Valgrind
evidence; the sanitizer lifecycle runs provide the corresponding checks. Do not silence
that limitation with blanket suppressions or describe an uninstrumented runtime as fully
sanitizer-validated.

## GCC 10 compilation check

The helper must compile with GCC 10 and its standard-library headers, without C++20
ranges or concepts. Use the dedicated Ubuntu 22.04 image:

```sh
docker build -f containers/Dockerfile.php-cxx-gcc10 \
    -t deferra-php-cxx:gcc10-ubuntu22.04 containers
tools/php-cxx-gcc10 \
    "$HOME/php/8.2/bin/php-config" \
    "$HOME/php/8.3/bin/php-config" \
    "$HOME/php/8.4/bin/php-config" \
    "$HOME/php/8.5/bin/php-config" \
    "$HOME/php/8.6/bin/php-config" \
    "$HOME/php/8.5-zts/bin/php-config"
```

The wrapper mounts sources and selected PHP installations read-only and writes under
`.bld-php-cxx-gcc10`, using the invoking user's UID/GID. It compiles the helper archive
and consumer translation units. It runs no PHP, Embed, or functional tests. Functional
tests use the normal host compiler and matching PHP installation. The existing JobU
runtime is outside the GCC 10 check.

## GitHub CI and public PHP versions

The workflow in `.github/workflows/ci.yml` pins Ubuntu 26.04 and Alpine 3.24. The two
native jobs build and test JobU once per distribution, with PHP disabled. Two separate
PHP matrices build this helper and the [JobU module](../jobu-php/README.md) against the
public ZendPHP CLI, development, and Embed packages. Each row installs one PHP ABI and
runs all helper and module checks, including relocated helper consumers.
`PHPCXX_REQUIRE_EMBED=ON` makes a missing matching Embed installation a configuration
failure; the CI runner also rejects missing or skipped checks. Ordinary helper consumers
retain optional Embed discovery.

**Edit only the `&php-versions` list in the `php-ubuntu` job to change the PHP matrix.**
The `php-alpine` job references that list through `*php-versions`. Remove `'8.2'` from
that one list when PHP 8.6 is released and ZendPHP withdraws public PHP 8.2 access.
Package names and tool paths derive from each selected minor version; there is no second
version list in the scripts. Package access failures fail the affected row instead of
silently removing coverage. Patch updates within each minor version are accepted and the
selected version/build mode is printed in the job log.

`tools/ci/install-zendphp ubuntu|alpine VERSION` registers the pinned repository and its
signing key and installs the selected ABI. It is intended for disposable root-owned CI
environments, not an existing developer PHP installation. In particular, Alpine Embed
packages share `/usr/lib/libphp.so`: use separate jobs or containers, never install
several Embed versions into one environment. Ubuntu's versioned CLI selects its
corresponding `libphpVERSION-zend.so` without falling back to another ABI's generic
library.

After installing the tools listed in the workflow, reproduce helper-only checks with:

```sh
# Ubuntu 26.04; use /usr/bin/php-config85zend on Alpine 3.24 instead.
CCACHE_DIR="$PWD/.ccache" tools/ci/php-test 8.5 /usr/bin/php-config8.5-zend .bld/php
```

This three-argument command configures only `src/php-cxx`. CI adds the native SDK prefix
as a fourth argument to build and test the module as well. Each matrix cache is scoped
by distribution release, PHP minor, and compiler version. The workflow uploads CTest XML
and failure logs for inspection. Local ZTS, sanitizer, and GCC 10 validation remain
separate from this NTS package matrix.

For the module's native SDK artifact and daemon reuse, see the [JobU PHP development
README](../jobu-php/README.md#reusing-the-native-sdk-in-ci).
