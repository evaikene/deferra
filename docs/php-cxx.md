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

External C++ owners must be destroyed before starting request shutdown. Members
of Zend-bound native objects may instead be released by their `free_obj` handler
during normal request teardown, while Zend still owns the object and its values.
Neither kind of owner may remain alive after the request has finished shutting down.

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

## Embedded native objects

`<phpcxx/object_binding.hpp>` supplies `ObjectBinding<T, Policy>` and `GcVisitor`.
The default `ObjectBindingPolicy<T>` requires an explicit
`T::trace(GcVisitor&) const noexcept` and disables cloning. Supply an empty trace
method only when the native value owns no PHP references. A custom policy can
adapt an existing type with `static trace(T const&, GcVisitor&) noexcept` and
`static constexpr bool cloneable`.

A policy may opt into native comparison with
`static compare(T const&, T const&) noexcept -> int`, returning a negative, zero,
or positive value. The hook must not mutate state, execute PHP, invoke GC, or
retain its arguments. It runs only for initialized objects of the same concrete
PHP class and binding. Native ordering takes precedence; when native values are
equal, Zend compares ordinary properties (including subclass properties).
Distinct uninitialized objects and incompatible classes/bindings are incomparable;
object identity still compares equal. A pending PHP exception is preserved without
calling the policy. Standard PHP object/scalar coercion and strict `===` identity
are unchanged. Without this optional hook, Zend's property-only comparison remains
in effect and does **not** inspect native state.

Call `Binding::bind(class_entry)` during module startup, after the authoritative
stub-generated registration function returns the internal class entry. Check its
`Result<void>`; nonordinary classes, custom allocators from another binding, and
custom serialization handlers are rejected. The helper installs handlers without
introducing a second registration system. Each template specialization has an
immutable handler table with no class pointers or request state.

Zend allocation creates an **uninitialized native object** with valid standard
properties. In the PHP constructor or a native factory, validate arguments and
then call `Binding::construct(object, arguments...)`. It placement-constructs `T`
and returns `Result<T*>`. Repeated construction and construction with a pending
PHP exception fail without replacing existing native state. `Binding::get(object,
optional_expected_class)` returns a borrowed initialized `T*`, checking handler
identity before applying an allocation offset and optionally checking a PHP
class/interface. An ordinary object, another binding, or uninitialized/destroying
native state returns an error. Neither operation implicitly retains the object.

`T` need not be movable or standard-layout. Its constructor and destructor must
be non-throwing, and its alignment cannot exceed `ZEND_MM_ALIGNMENT`; unsupported
types fail compilation. One allocation contains a C++ storage prefix followed by
Zend's object and variable-length property slots. There is no separate native
allocation. Construction must not execute PHP, invoke GC, or publish partially
constructed state. Perform recoverable validation before construction, or use
`Result`-based initialization after construction while an owner protects the
object. A PHP constructor that raises after successful native construction leaves
cleanup to the object's eventual `free_obj`; it does not manually destroy `T`.

For example, native state retaining another PHP value can use the existing owner:

```cpp
class NativeState {
public:
    explicit NativeState(php::Value owner) noexcept : _owner(std::move(owner)) {}

    auto trace(php::GcVisitor& visitor) const noexcept -> void
    {
        visitor.add(_owner.view());
    }

private:
    php::Value _owner;
};

using Binding = php::ObjectBinding<NativeState>;
// At MINIT: check Binding::bind(register_class_Example()).
// Inside a non-throwing PHP constructor callback:
auto initialized = Binding::construct(Z_OBJ(execute_data->This),
                                      php::Value::retain(arguments.get(1)));
if (!initialized) {
    php::raise(initialized.error());
    return;
}
```

Use `Value` to own an object/reference-valued zval, including results created with
`object_init_ex`; no separate owning object wrapper is needed. Keep that owner
alive throughout native pointer access. PHP subclasses inherit the allocation
layout and can add standard properties. A subclass that skips its parent's
constructor remains safely uninitialized until explicit native construction.
Replacing handlers or introducing a different native layout in that hierarchy
is unsupported.

**GC enumeration is an ownership contract.** Report each native-owned PHP
reference once with `visitor.add(value.view())`, including distinct owners of the
same referent. Do not dereference PHP reference wrappers, report borrowed links,
retain values, mutate native state, execute PHP, or invoke GC while tracing.
The visitor copies reference information into Zend's scratch buffer without
changing ownership and must not escape the trace call. The binding also reports
standard properties, whether stored inline or in a materialized property table.
Native embedding alone does not make references visible to GC.

The normal Zend destructor handler runs userland `__destruct` while native state
is still available. Later, `free_obj` marks the state unavailable, destroys a
successfully constructed `T` exactly once, and then destroys standard properties.
Reentrant cleanup cannot retrieve a partially destroyed `T`. Never-constructed
objects skip native destruction. These rules cover GC and normal request shutdown;
they are not a recovery guarantee after PHP bailout.

To opt into cloning, provide a policy with `cloneable = true` and
`static clone(T const&) noexcept -> Result<T>`, and make `T` nothrow-movable.
The policy defines native copy semantics explicitly, including any deep-copy or
detachment requirements. The binding constructs the clone's native state before
Zend copies PHP properties and invokes `__clone`. Standard PHP property copies
retain their normal copy-on-write/reference semantics. Errors preserve an existing
pending PHP exception; Zend owns cleanup of the failed clone. Targets that fail
before native initialization skip userland `__destruct` while retaining `free_obj`
cleanup. After native initialization, normal PHP destruction applies, including
when the PHP `__clone` hook throws. With the default
policy, cloning raises PHP's uncloneable-object error. PHP 8.5+ cloning with a
property-update list is unsupported. Bound ordinary classes and their subclasses
reject serialization/unserialization, including subclasses defining serialization
hooks. Native enums are outside this binding.

All native pointers, owners, and GC views remain confined to the owning PHP
execution thread and request in both NTS and ZTS. No helper owner may survive
request shutdown. Allocation failure remains process-fatal; no C++ exception may
unwind through a Zend callback.

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
Ownership, view, argument, conversion, exception, and native binding tests use the existing
Catch2 dependency and matching optional Embed library in dedicated executables.
The ownership/conversion and native binding harnesses each initialize one
request. Their external owners die before shutdown; native binding accounting
also checks objects deliberately retained in PHP globals until shutdown. The CLI
and relocated consumers exercise the same generated fixtures.

The lifecycle harness adds three complete runtime cycles, five request cycles
within one runtime, and (in ZTS builds) two concurrent workers with three requests
each. Each request exercises binary strings, array separation, conversions,
exception preservation, native bindings, clone failures, GC cycles, and shutdown
cleanup. User functions/classes and request globals must disappear between
requests. Thread-local native counters must balance after every shutdown; no
Zend pointers are retained to inspect them afterward. Barriers keep both ZTS
requests active while checking distinct globals and pending exception state.

Two additional CTest cases run deliberate bailout and frameless-error scenarios
in child processes. They require the expected bailout, a successful shutdown
marker, and normal child exit; a crash or timeout fails the test. Their jump
regions contain no C++ objects requiring destruction. These are narrow checks of
host teardown, not a guarantee of recovery after arbitrary PHP failures.

There are twelve helper checks. Missing Embed explicitly skips the six Embed
checks; compilation and the six CLI/build/install checks remain enabled. Only
test targets link Embed. Neither consumers nor production helper linkage acquire
an Embed or thread-library dependency from these fixtures.

The test-only `consumer/functions.stub.php` and `consumer/objects.stub.php` are
authoritative for their checked-in `*_arginfo.h`, generated with PHP's
`build/gen_stub.php`. The object stub requests PHP 8.1-compatible registration
through `@generate-legacy-arginfo 80100`; generation uses the existing PHP 8.5
generator and PHP-Parser installation. Normal builds
and tests consume that header directly; they do not need PHP sources, a stub
compiler, or a generated-header freshness check. The fixture deliberately uses
`mixed` input so helper parsing is exercised rather than preempted by arginfo.

To include the helper in a normal JobU build, use `-DJB_BUILD_PHP_CPP=ON` and
the selected `PHP_CONFIG`. The option defaults to `OFF`; default configuration
does not discover PHP or execute `php-config`. The JobU PHP extension is a
separate, later-stage target.

## Embed lifecycle and compatibility validation

Runtime/module startup, request startup, and execution-thread initialization are
different boundaries. `php_embed_init()` performs all startup work, including an
initial request; `php_embed_shutdown()` shuts that request and the runtime down.
For subsequent requests in the same runtime, pair `php_request_shutdown()` with
`php_request_startup()` instead. Destroy external C++ owners before ending each
request, and resolve pending exceptions before normal teardown.

Register generated persistent functions/classes during module startup. Zend
borrows a registered module entry until module shutdown, so its storage must
outlive registration. Reinitialize its bookkeeping for a new runtime and restore
any replaced SAPI callbacks. Never cache request class entries, zvals, views, or
native pointers for reuse after shutdown. The fixture's lifecycle owner is
private test support, not a production lifecycle API.

For ZTS workers, initialize the process/module once before starting workers,
attach each worker with `ts_resource(0)`, and run its request operations on that
same thread. After its final request shutdown, call `ts_free_thread()` before
joining all workers and shutting down the process runtime. Only native test
results and synchronization cross threads. A passing ZTS helper test does not
establish support for a ZTS JobU extension.

The functional matrix is PHP 8.2–8.6 NTS plus PHP 8.5 ZTS. Use each installation's
explicit `PHP_CONFIG` and a separate build directory, following the commands
above; record the actual patch version, debug mode, and thread mode. PHP 8.1
remains the minimum contract but is excluded from this execution matrix. The
GCC 10 check below compiles the independent helper and public-template consumers
for the same six ABIs, without running PHP or Embed.

## Memory checks

Use a separate sanitizer build. For example, with a compiler providing ASan,
UBSan, and LeakSanitizer:

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

Repeat with PHP 8.5 ZTS and a distinct build directory. These commands instrument
the helper and fixture code, including binding templates; an uninstrumented
`libphp` does not gain compiler instrumentation. Disabling Zend's allocator for
normal cases exposes individual Zend allocations to the memory checker. The
bailout cases deliberately retain the Zend allocator: unclean shutdown may leave
individual blocks for request-arena reclamation. Native destruction counters
still apply, but this does not establish individual-allocation safety after a
bailout. No test deliberately jumps over a live automatic C++ owner: doing so
would violate C++ `longjmp` requirements, not just skip a convenient cleanup.
LeakSanitizer also needs a host environment that permits its process inspection.

Valgrind can exercise the shared conversion and object fixtures through a
matching, **unsanitized** CLI module build:

```sh
USE_ZEND_ALLOC=0 valgrind --error-exitcode=97 --leak-check=full \
    --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect \
    "$HOME/php/8.2/bin/php" -n \
    "-dextension=$PWD/.bld/php-cxx-8.2/test/php-cxx-module-test.so" \
    test/php-cxx/consumer/objects-check.php
```

Repeat for `consumer/check.php` and matching ZTS paths. Inspect the complete
report, including reachable blocks and the tool's default suppressions; no
project suppression file is used. On the tested Linux installations, PHP CLI's
built-in Valgrind string-comparison replacement works, while Embed's shared
`libphp` reports uninitialized reads in its assembly `zend_string_equal_val`.
Those Embed runs are not clean Valgrind evidence; the sanitizer lifecycle runs
provide the corresponding checks. Do not silence that limitation with blanket
suppressions or describe an uninstrumented runtime as fully sanitizer-validated.

## Portability boundary

The helper's public API contains no JobU transport or POSIX types. It relies on
the selected Zend ABI, allocator alignment, and C++20 support. Linux is the
validated environment for this regression matrix. Windows and macOS execution
are not established by these results; Windows compiler, PHP discovery,
link/export, and Embed integration still require platform validation. Keep
platform-specific JobU event-loop and socket adapters outside this library.

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

## GitHub CI and public PHP versions

The workflow in `.github/workflows/ci.yml` pins Ubuntu 26.04 and Alpine 3.24.
The two native jobs build and test JobU once per distribution, with PHP disabled.
Two separate PHP matrices build this helper and the [JobU module](php.md) against
the public ZendPHP CLI, development, and Embed packages. Each row installs one
PHP ABI and runs all helper and module checks, including relocated helper consumers. `PHPCXX_REQUIRE_EMBED=ON`
makes a missing matching Embed installation a configuration failure; the CI
runner also rejects missing or skipped checks. Ordinary helper consumers retain
optional Embed discovery.

**Edit only the `&php-versions` list in the `php-ubuntu` job to change the PHP
matrix.** The `php-alpine` job references that list through `*php-versions`.
Remove `'8.2'` from that one list when PHP 8.6 is released and ZendPHP withdraws
public PHP 8.2 access. Package names and tool paths derive from each selected
minor version; there is no second version list in the scripts. Package access
failures fail the affected row instead of silently removing coverage. Patch
updates within each minor version are accepted and the selected version/build
mode is printed in the job log.

`tools/ci/install-zendphp ubuntu|alpine VERSION` registers the pinned repository
and its signing key and installs the selected ABI. It is intended for disposable
root-owned CI environments, not an existing developer PHP installation. In
particular, Alpine Embed packages share `/usr/lib/libphp.so`: use separate jobs
or containers, never install several Embed versions into one environment.
Ubuntu's versioned CLI selects its corresponding `libphpVERSION-zend.so` without
falling back to another ABI's generic library.

After installing the tools listed in the workflow, reproduce helper-only checks with:

```sh
# Ubuntu 26.04; use /usr/bin/php-config85zend on Alpine 3.24 instead.
CCACHE_DIR="$PWD/.ccache" tools/ci/php-test 8.5 /usr/bin/php-config8.5-zend .bld/php
```

This three-argument command configures only `src/php-cxx`. CI adds the native SDK
prefix as a fourth argument to build and test the module as well. Each
matrix cache is scoped by distribution release, PHP minor, and compiler version.
The workflow uploads CTest XML and failure logs for inspection. Local ZTS,
sanitizer, and GCC 10 validation remain separate from this NTS package matrix.

### Reusing the native SDK for the JobU module

After each native job passes, `tools/ci/prepare-sdk` installs the existing
`Development` component, relocates it, builds/runs the independent Client SDK
consumer, and packages `jobu-sdk.tar.gz`. It recompiles no JobU library. The
workflow publishes `jobu-sdk-ubuntu-26.04-x64` and `jobu-sdk-alpine-3.24-x64` as
artifacts of that workflow run; the archives contain the `sdk/` prefix and its
PIC static libraries, public headers, and CMake exports.

The module's standalone build uses
`find_package(JobU CONFIG REQUIRED COMPONENTS Client)` and links
`JobU::jobu-client`, while building `php-cxx` and the module for the selected
PHP ABI. Each PHP matrix depends on its corresponding native job, downloads the
SDK artifact **from the same workflow run**, extracts it, and passes the relocated
`sdk/` prefix through `CMAKE_PREFIX_PATH`. The consumer uses the matching
distribution, architecture, compiler, and system-library environment. Never
substitute a cached SDK from a different revision, mix glibc and musl artifacts,
or configure the top-level project in each PHP row.

```sh
CCACHE_DIR="$PWD/.ccache" tools/ci/php-test 8.5 /usr/bin/php-config8.5-zend \
    .bld/php /path/to/relocated/sdk
```

Configuration, module load, Reflection, exception, and process restart checks run alongside all
twelve helper checks. CI rejects missing or skipped checks in either suite and
uploads the compilation database to show which sources were rebuilt. Later
daemon integration can reuse separately staged native outputs. The standalone
helper remains independent of the SDK; its original three-argument test command
and `src/php-cxx` entry point remain available.
