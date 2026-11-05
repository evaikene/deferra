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

## Read-only view operations

`StringView` exposes `data()`/`c_str()` (always zero-terminated, including for an
absent view), `size()`/`length()`, `empty()`, unchecked `operator[]`, `front()` and
`back()`, and const character iterators. Character indexing requires an index
less than `size()`; `front()`/`back()` require a nonempty view. Embedded NULs
remain part of the string: use its size when passing bytes to native APIs.

Equality, `compare()`, `starts_with()`, `ends_with()`, `contains()`, `find()` and
`rfind()` accept `std::string_view` or another `StringView`; prefix/suffix/search
helpers also accept a character. Searches return `StringView::npos` when absent.
`icompare()`, `iequals()`, `istarts_with()`, and `iends_with()` fold ASCII A-Z
only, without locale dependence or Unicode case folding. The prefix/suffix
variants accept strings, views, or characters; an empty prefix/suffix always
matches, including on an absent view. `starts_with()` and `ends_with()` remain
case-sensitive. Comparisons and searches include
bytes after embedded NULs. An absent view has the same byte operations as an
empty string, while its explicit boolean conversion still reports absence.

`ValueView` queries the exact stored type with `is_undef()`, `is_null()`,
`is_bool()`, `is_true()`, `is_false()`, `is_long()`, `is_double()`, `is_string()`,
`is_array()`, `is_object()`, `is_resource()`, and `is_reference()`.
`get_bool()`, `get_long()`, `get_double()`, `get_string()`, `get_array()`,
`get_object()`, and `get_resource()` are unchecked: the slot must be present
and have the corresponding type. They do not convert PHP values, retain
payloads, or raise exceptions. String/array getters return borrowed views;
object/resource getters return borrowed const Zend pointers.

`get_bool_or(fallback)`, `get_long_or(fallback)`, and `get_double_or(fallback)`
return the fallback on absence or an exact type mismatch. For example, a PHP
integer does not satisfy `get_double_or()`, and a numeric string does not
satisfy `get_long_or()`. Type queries and getters preserve PHP reference wrappers;
call `dereference()` explicitly to borrow the referenced slot. On an ordinary
or absent view, `dereference()` returns that view unchanged.

`is_truthy()` evaluates PHP boolean conversion through `zend_is_true()`,
including automatic reference dereferencing. Absent/undefined views, null,
false, numeric zero, `""`, `"0"`, and empty arrays are false; for example,
`"false"` and nonempty arrays are true. It does not retain or convert the stored
value. Zend may invoke object conversion handlers or emit PHP diagnostics, so
normal PHP error/exception handling applies. Keep the backing value alive
throughout the call, including any handler execution.

`ArrayView` supplies `empty()`, `find(key)`, `operator[](key)`, and `contains(key)`
for signed PHP integer keys and binary-safe `std::string_view` keys. Lookups
never insert. String lookup follows PHP's numeric-key normalization: `"1"`
addresses integer key `1`, while `"01"` remains a string key. A missing key or
absent array returns an absent `ValueView` (`get() == nullptr`), distinct from
a present PHP null. `contains()` reports a present null as present.

Array `begin()`/`end()` and `cbegin()`/`cend()` provide read-only input iterators
yielding `ArrayView::Entry` by value. Each entry contains a `key` of type
`std::variant<zend_long, StringView>` and a borrowed `ValueView value`.
Iteration preserves PHP insertion order, skips deleted slots, and handles
packed and mixed arrays. Iterator copies have independent cursors and do not
change PHP's internal array pointer. Any mutation or separation invalidates
iterators and borrowed entries; keep the backing array/request alive.

```cpp
auto const view = value.view().dereference();
if (view.is_array()) {
    auto const entries = view.get_array();
    auto const limit = entries["limit"].get_long_or(100);
    for (auto const [key, item] : entries) {
        // key and item borrow from entries; no reference counts change.
    }
}
```

## Results, extraction, and PHP errors

`<phpcxx/error.hpp>` and `<phpcxx/result.hpp>` are independent of Zend and JobU.
`Result<T>` contains a value or an owned `Error`; `Result<void>` represents an
operation with no payload. Test the result before calling `value()` or `error()`.
Accessing the wrong alternative violates a precondition. Move-only values remain
move-only, and moving an owner into/out of a result does not retain its payload.

`<phpcxx/conversion.hpp>` adds checked extraction over the existing views:

| Operation | Accepted value and result |
| --- | --- |
| `as_string` | PHP string → borrowed binary-safe `StringView` |
| `as_bool` | PHP boolean → `bool`, without truthiness conversion |
| `as_integer` | PHP integer → `zend_long` |
| `as_double` | PHP double → `double`, preserving NaN/infinity |
| `as_array` | PHP array → borrowed `ArrayView` |
| `as_object` | Instance of a requested class/interface, including subclasses → borrowed const object pointer |

These functions dereference PHP references without changing their targets or
reference counts. Missing, undefined, null, and wrong-type values fail. They do
not execute user code or change pending PHP exception state. In particular,
`as_double` does not widen integers and `as_integer` does not parse strings.
Use `retain()` explicitly if a borrowed result must survive its backing value.
All Zend-backed values remain on their owning execution thread and request.

`checked_integer<T>(zend_long)` checks signedness and target range, for non-bool
integral types. `integer_from_double()` explicitly rejects fractional, non-finite,
and out-of-range input before casting. `double_from_integer()` explicitly allows
PHP-style floating-point rounding. Never use that rounding conversion for dates,
revisions, or exact offsets.

`<phpcxx/arguments.hpp>` supplies `Arguments(execute_data)` for the **currently
active internal PHP call**. Indices are one-based. `count(minimum, maximum)`
validates an inclusive range. `get(index)` borrows the raw slot: absence
(`get() == nullptr`), a present undefined slot, and PHP null are distinct.
The caller handles optional defaults and nullable values explicitly before
requesting typed extraction. Zend resolves named arguments and any materialized
defaults using generated arginfo; this library does not duplicate that mechanism.

The scalar `string`, `boolean`, `integer`, and `floating` argument methods use
Zend's parser and the caller's `strict_types` setting. Thus a weak call can accept
a numeric string as an integer, while a strict call rejects it. Integer-to-float
widening follows Zend even in strict calls. These methods require non-null input;
they reject null rather than relying on Zend's deprecated null coercion. `array`
and `object` check the required type/class without coercion. Parsing may modify
argument slots and execute object conversion handlers, so reacquire borrowed
views after a coercion and keep the call frame active throughout.

`<phpcxx/exception.hpp>` bridges returned errors to PHP. `raise(error)` maps type,
value/range, and count failures to native `TypeError`, `ValueError`, and
`ArgumentCountError`. Runtime errors use `Exception`, or a supplied class derived
from `Exception`/`Error`. A nonzero `Error::argument` adds native argument context
for type/value errors. Diagnostic messages should describe requirements rather
than interpolate sensitive submitted values; messages passed to Zend must not
contain embedded NULs.

Raising requires an active PHP execution frame. Without one, Zend may fatally
bail out. `raise` preserves an already pending exception, without chaining or
replacing it. `PendingException` is a marker for an exception still owned by Zend,
not an exception owner: do not clear the exception and later expect the marker
to recreate it. Argument parsing and date conversion return this marker if an
exception is already pending or their PHP operation raises one. Callers must
return to Zend after signalling failure. Keep C ABI callbacks non-throwing;
allocation failure is process-fatal, and PHP bailout can bypass destructors.

```cpp
// Inside an internal function's non-throwing implementation; arginfo is generated.
php::Arguments const arguments(execute_data);
auto count = arguments.count(1, 1);
if (!count) {
    php::raise(count.error());
    return;
}

auto text = arguments.string(1);
if (!text) {
    php::raise(text.error());
    return;
}
// text.value() borrows the current argument; retain explicitly before storing it.
```

## Native dates

`Timestamp` stores floor Unix seconds (`int64_t`) and microseconds (`int32_t`,
0–999999). For example, `{-1, 500000}` is half a second before the epoch.
`as_timestamp()` accepts native `DateTimeInterface` objects, including mutable and
immutable subclasses. It reads native date state rather than overridden userland
methods, and may update Zend's cached epoch seconds. An object whose constructor
did not initialize its date state returns a value error.

`datetime_immutable()` produces an owned UTC `DateTimeImmutable` with exact
microseconds, independent of the request's default timezone. It uses PHP's date
parser (and may update its last-error state), checks normalization, and verifies
the resulting instant. `unix_microseconds()` checks the signed 64-bit range;
`timestamp_from_microseconds()` handles the entire signed range, including
negative values, without floating-point arithmetic. Date objects still obey the
normal request/thread ownership rules. JobU schedules and durations are outside
this independent helper.

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
Ownership, view, argument, conversion, and exception tests use the existing
Catch2 dependency and matching optional Embed library in dedicated executables.
The minimal harness initializes one
request, runs the tests, destroys their local owners, and shuts Embed down.
Missing Embed registers explicit CTest skips for ownership and conversion
execution; helper compilation and the other tests remain enabled. Only the Embed test executables
link Embed. Comprehensive repeated lifecycle, threading, and bailout tests belong
to later stages.

The test-only `consumer/functions.stub.php` is authoritative for its checked-in
`functions_arginfo.h`, generated with PHP's `build/gen_stub.php`. Normal builds
and tests consume that header directly; they do not need PHP sources, a stub
compiler, or a generated-header freshness check. The fixture deliberately uses
`mixed` input so helper parsing is exercised rather than preempted by arginfo.

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
