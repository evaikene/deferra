# php-cxx

`php-cxx` is an independent C++20 static library for implementing PHP extensions.
Its public CMake target is `PhpCxx::PhpCxx`. It has no JobU, SQLite, fmt, or PHP
Embed link dependency. The archive uses PIC and hidden symbol visibility, so
each extension can embed its own copy.

`<phpcxx/zend.hpp>` includes the selected PHP/Zend headers with C linkage.
`<phpcxx/build.hpp>` supplies build metadata.
`php::build_info()` reports the archive's PHP version and ABI without requiring
an initialized PHP request. `<phpcxx/string.hpp>`, `<phpcxx/array.hpp>`, and
`<phpcxx/value.hpp>` supply move-only Zend owners; `<phpcxx/views.hpp>` supplies
their lightweight borrowed views.

## Ownership and borrowed lifetimes

Owners must be created, used, and destroyed on their owning PHP execution
thread, inside the lifetime of the initialized PHP request. This applies to
both NTS and ZTS. The library does not store mutable request state globally or
transfer Zend values between threads. A longer-lived interned allocation does
not authorize carrying its wrapper into another request.

| Operation | Contract |
| --- | --- |
| `adopt` | Consume one existing owned reference without retaining it. `Value::adopt(zval&)` also sets the source slot to `IS_UNDEF`. |
| `retain(view)` | Acquire an owned reference explicitly. String/array retention respects immutable Zend values; value retention uses `ZVAL_COPY`, preserving PHP reference wrappers. |
| `duplicate()` | Use Zend duplication semantics, described below. An absent/uninitialized owner produces the same state. |
| `release()` | Transfer ownership to the caller and empty the owner, without decrementing the transferred reference. |
| `view()` | Borrow without changing reference counts. Views from temporary owners are rejected. |

Default and moved-from `String`/`Array` owners hold no pointer. Their views
report no value; absent string bytes and array size are empty/zero.
`String::create({})` and `Array::create()` instead construct valid empty PHP
values. `Value` defaults/moves/releases to `IS_UNDEF`; `Value::null()` creates a
valid PHP null. `ValueView::is_initialized()` distinguishes these states.
An absent `ValueView` also reports `IS_UNDEF`.

Views expose read-only access. They never extend a slot, payload, or request's
lifetime. Keep the backing owner alive and reacquire views after mutations or
separation. Moving/releasing a `Value` invalidates views of its internal slot.
Array element views can be invalidated by table growth or replacement. Returned
string bytes are binary-safe and carry the same borrowed lifetime.

```cpp
// Run within an initialized PHP request. Every owner below dies before shutdown.
auto text = php::String::create(std::string_view("a\0b", 3));
auto shared_text = php::String::retain(text.view());
auto copied_text = text.duplicate();

auto array = php::Array::create();
zval entry{};
ZVAL_STR(&entry, shared_text.release());
zend_hash_index_update(array.writable(), 0, &entry); // HashTable consumes entry's value.
ZVAL_UNDEF(&entry); // The local slot no longer owns that reference.

zval slot{};
ZVAL_ARR(&slot, array.release());
auto value = php::Value::adopt(slot); // slot is now IS_UNDEF.
auto shared_value = php::Value::retain(value.view());
```

`String::duplicate()` copies ordinary string bytes; interned strings may stay
shared. `Array::duplicate()` duplicates the top-level PHP array using
`zend_array_dup`. `Value::duplicate()` uses `ZVAL_DUP`: a direct array receives
duplicated top-level storage, while other types retain/copy their PHP value.
These operations preserve Zend's nested copy-on-write, object identity, and
reference semantics, including Zend's handling of unshared reference slots.
They are not recursive graph copies or PHP object clones.

Call `Array::writable()` before mutating an array. It separates shared or
immutable top-level storage, then returns a borrowed mutable pointer; an absent
owner returns `nullptr`. Reacquire it after any operation that may share the
array, and separate nested arrays independently when modifying them. Do not
change the returned pointer's reference count or release it. `Array` accepts
regular request-allocated PHP arrays and immutable Zend arrays, not arbitrary
native or persistent HashTables.

`Value` accepts ordinary PHP values, PHP references, and `IS_UNDEF`; internal
engine-only slot types such as `IS_INDIRECT` are outside its contract. Releasing
a `Value` transfers a raw zval that the caller must consume or destroy with
`zval_ptr_dtor`. Releasing strings transfers responsibility for
`zend_string_release`. Consume a released array into another `Array` or a zval
so its eventual destruction also nominates surviving cycles for Zend GC;
`zend_array_release` alone does not nominate an untracked cycle.
Destroying an object/resource value may invoke PHP cleanup code. Native
destructors are non-throwing; PHP `zend_bailout()` can bypass C++ destructors.
RAII is not a bailout recovery mechanism.

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

`BUILD_TESTING=OFF` disables tests and their CLI availability check.
Tests cover the compiled ABI metadata, a small CLI-loadable consumer, independent build-tree
consumption, relocated installation, discovery failures, and Zend ownership.
Ownership tests use the existing Catch2 dependency and the matching optional
Embed library in a dedicated executable. Its minimal harness initializes one
request, runs the tests, destroys their local owners, and shuts Embed down.
Missing Embed registers an explicit CTest skip for ownership execution; helper
compilation and the other tests remain enabled. Only that executable links
Embed. Comprehensive repeated lifecycle, threading, and bailout tests belong
to later stages.

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
