# php-cxx

`php-cxx` is an independent C++20 static library for implementing PHP extensions.
Its public CMake target is `PhpCxx::PhpCxx`. It has no JobU, SQLite, fmt, or PHP
Embed link dependency. The archive uses PIC and hidden symbol visibility, so
each extension can embed its own copy.

The initial build foundation supplies `<phpcxx/zend.hpp>`, which includes the
selected PHP/Zend headers with C linkage, and `<phpcxx/build.hpp>`.
`php::build_info()` reports the archive's PHP version and ABI without requiring
an initialized PHP request. Zend ownership wrappers are added in later stages.

## Select a PHP installation

PHP 8.1 or newer with development headers and 64-bit `zend_long` is required.
The helper supports NTS and ZTS. Select one installation explicitly:

```sh
cmake -S src/php-cxx -B .bld/php-cxx-8.5 -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPHP_CONFIG="$HOME/php/8.5/bin/php-config"
cmake --build .bld/php-cxx-8.5
ctest --test-dir .bld/php-cxx-8.5/test --output-on-failure
```

Use separate build directories for different PHP installations. Discovery runs
`php-config` and compiles header probes; it does not execute PHP or require
Embed. It records the selected CLI, extension directory, PHP version, module
API, debug/ZTS mode, integer width, PHP link flags, and optional Embed paths.
The PHP executable's link flags are metadata, not helper dependencies.

`BUILD_TESTING=OFF` disables the CLI tests and their CLI availability check.
The Embed lifecycle harness belongs to a later stage. Current tests cover the
compiled ABI metadata, a small CLI-loadable consumer, independent build-tree
consumption, relocated installation, and discovery failures.

To include the helper in a normal JobU build, use `-DJB_BUILD_PHP_CPP=ON` and
the selected `PHP_CONFIG`. The option defaults to `OFF`; default configuration
does not discover PHP or execute `php-config`. The JobU PHP extension is a
separate, later-stage target.

## Consume the library independently

An external CMake project can use the source tree:

```cmake
add_subdirectory(/path/to/deferra/src/php-cxx php-cxx)
target_link_libraries(my_extension PRIVATE PhpCxx::PhpCxx)
```

Or install just the helper, then consume the package:

```sh
cmake --install .bld/php-cxx-8.5 --prefix /tmp/php-cxx-sdk \
    --component PhpCxxDevelopment
```

```cmake
find_package(PhpCxx 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_extension PRIVATE PhpCxx::PhpCxx)
```

Pass the install prefix through `CMAKE_PREFIX_PATH` and select the consumer's
`PHP_CONFIG`. Installed exports rediscover PHP rather than retain the producer's
PHP include paths. The module API, debug mode, NTS/ZTS mode, and integer width
must match the archive; a mismatch requires a matching PHP installation or a
separate library build. This unreleased package provides no cross-version C++
ABI guarantee.

## GCC 10 compilation check

The helper must compile with GCC 10 and its standard-library headers, without
C++20 ranges or concepts. Use the dedicated Ubuntu 22.04 image:

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

The wrapper mounts sources and selected PHP installations read-only and writes
under `.bld-php-cxx-gcc10`, using the invoking user's UID/GID. It compiles the
helper archive and consumer translation units. It runs no PHP, Embed, or
functional tests. Functional tests use the normal host compiler and matching
PHP installation. The existing JobU runtime is outside the GCC 10 check.
