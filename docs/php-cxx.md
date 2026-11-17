# php-cxx module-author guide

This guide is for C++ developers implementing PHP modules. For applications using
the JobU PHP module, see the [PHP API manual](php.md).

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

See the [root README](../README.md#build-and-install-php-cxx-independently) for
building and installing the library, and the
[development README](../src/php-cxx/README.md) for implementation and validation.

## Contents

- [Consume the library independently](#consume-the-library-independently)
- [Complete extension example](#complete-extension-example)
- [Public headers](#public-headers)
- [Ownership and borrowed lifetimes](#ownership-and-borrowed-lifetimes)
- [Read-only view operations](#read-only-view-operations)
- [Results, extraction, and PHP errors](#results-extraction-and-php-errors)
- [Native dates](#native-dates)
- [Embedded native objects](#embedded-native-objects)
- [Runtime and request lifetimes](#runtime-and-request-lifetimes)
- [Portability boundary](#portability-boundary)

## Consume the library independently

Build and installation commands, PHP selection, and ABI requirements are in the
[root README](../README.md#build-and-install-php-cxx-independently). An external
CMake project can use the source tree:

```cmake
add_subdirectory(/path/to/deferra/src/php-cxx php-cxx)
target_link_libraries(my_extension PRIVATE PhpCxx::PhpCxx)
```

Or consume an installed package:

```cmake
find_package(PhpCxx 0.1.0 EXACT CONFIG REQUIRED)
target_link_libraries(my_extension PRIVATE PhpCxx::PhpCxx)
```

Select the consumer's `PHP_CONFIG` and pass the SDK prefix through
`CMAKE_PREFIX_PATH`. The package requires the exact helper version and a matching
PHP module API, debug mode, NTS/ZTS mode, and integer width. Consumers do not need
the producer's source/build tree, JobU packages, or Embed.

## Complete extension example

This module exposes `demo_copy(string $bytes): string` and a final `DemoBox`
class with `__construct(mixed $value)` and `get(): mixed`. The function demonstrates
argument parsing and returning an owned binary string. The class stores a PHP value
in native C++ state and reports its owned reference to Zend's cycle collector.

Save the following files in one directory. The stub is the authoritative PHP
signature and registration description; it is not a PHP implementation to load.
Use PHP's `build/gen_stub.php` to generate `demo_arginfo.h` from it before compiling.
The generator's PHP-Parser dependency must already be installed. The legacy annotation
requests registration compatible with PHP 8.1+, even when generating with PHP 8.5.
Keep the generated header with the module sources so ordinary consumers do not need
the generator. See the [generation workflow](../src/php-cxx/README.md#generating-arginfo)
for the development command. No build-time generation step is needed.

### PHP declarations

`demo.stub.php`:

```php
<?php

/**
 * @generate-class-entries
 * @generate-legacy-arginfo 80100
 */

function demo_copy(string $bytes): string {}

/** @not-serializable */
final class DemoBox
{
    public function __construct(mixed $value) {}
    public function get(): mixed {}
}
```

### C++ implementation

`demo.cpp`:

```cpp
#include <phpcxx/arguments.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>
#include <phpcxx/string.hpp>
#include <phpcxx/value.hpp>
#include <phpcxx/zend.hpp>

#include <utility>

#include "demo_arginfo.h"

namespace {

class StoredValue {
public:
    explicit StoredValue(php::Value value) noexcept
        : _value(std::move(value))
    {}

    auto value() const noexcept -> php::ValueView { return _value.view(); }

    auto trace(php::GcVisitor& visitor) const noexcept -> void
    {
        // Zend must see the owned edge, including cycles back to this DemoBox.
        visitor.add(_value.view());
    }

private:
    php::Value _value;
};

using BoxBinding = php::ObjectBinding<StoredValue>;

auto copy_bytes(zend_execute_data* execute_data, zval* return_value) noexcept -> void
{
    php::Arguments const arguments(execute_data);
    auto                 count = arguments.count(1, 1);
    if (!count) {
        php::raise(count.error());
        return;
    }

    auto bytes = arguments.string(1);
    if (!bytes) {
        php::raise(bytes.error());
        return;
    }

    // Preserve embedded NUL bytes and transfer the new owned string to Zend.
    auto result = php::String::create(bytes.value().bytes());
    ZVAL_STR(return_value, result.release());
}

auto construct_box(zend_execute_data* execute_data) noexcept -> void
{
    php::Arguments const arguments(execute_data);
    auto                 count = arguments.count(1, 1);
    if (!count) {
        php::raise(count.error());
        return;
    }

    // Retain the argument before the call frame disappears. Native construction
    // only takes ownership; it performs no PHP calls or recoverable work.
    auto result = BoxBinding::construct(Z_OBJ(execute_data->This), php::Value::retain(arguments.get(1)));
    if (!result) {
        php::raise(result.error());
    }
}

auto read_box(zend_execute_data* execute_data, zval* return_value) noexcept -> void
{
    php::Arguments const arguments(execute_data);
    auto                 count = arguments.count(0, 0);
    if (!count) {
        php::raise(count.error());
        return;
    }

    auto native = BoxBinding::get(Z_OBJ(execute_data->This));
    if (!native) {
        php::raise(native.error());
        return;
    }

    // The active method keeps $this alive. Copy the value into the return slot;
    // ownership inside StoredValue stays intact and object identity is preserved.
    ZVAL_COPY(return_value, native.value()->value().get());
}

auto start_module([[maybe_unused]] int type, [[maybe_unused]] int module_number) noexcept -> zend_result
{
    // Register from generated arginfo first, then attach native storage handlers.
    // No PHP execution frame exists here, so report startup failure directly.
    auto result = BoxBinding::bind(register_class_DemoBox());
    return result ? SUCCESS : FAILURE;
}

zend_module_entry module{.size                = sizeof(zend_module_entry),
                         .zend_api            = ZEND_MODULE_API_NO,
                         .zend_debug          = ZEND_DEBUG,
                         .zts                 = USING_ZTS,
                         .name                = "phpcxx_demo",
                         .functions           = ext_functions,
                         .module_startup_func = start_module,
                         .version             = "0.1.0",
                         .build_id            = ZEND_MODULE_BUILD_ID};

} // namespace

ZEND_FUNCTION(demo_copy)
{
    copy_bytes(execute_data, return_value);
}

ZEND_METHOD(DemoBox, __construct)
{
    construct_box(execute_data);
}

ZEND_METHOD(DemoBox, get)
{
    read_box(execute_data, return_value);
}

extern "C" ZEND_DLEXPORT auto get_module() -> zend_module_entry*
{
    return &module;
}
```

The generated header declares the Zend callbacks, the `ext_functions` table, and
`register_class_DemoBox()`. `ZEND_FUNCTION` and `ZEND_METHOD` provide the matching
callbacks; `get_module()` is the exported entry point used by PHP's module loader.
The module entry has static storage because Zend borrows it until module shutdown.
No request-owned value is stored globally.

The callbacks delegate to non-throwing implementations. Each failure returns to Zend
after `php::raise()`; module startup instead returns `FAILURE`, since there is no
active PHP call in which to raise an exception. `StoredValue` only takes ownership
in its constructor. Its `trace()` method exposes the retained value without executing
PHP or changing reference counts.

`DemoBox::get()` copies the stored zval into the return slot. It preserves object
identity, PHP reference semantics, and array copy-on-write. The class uses the default
binding policy: cloning and serialization are rejected. Native state does not
participate in `==` unless a comparison policy is supplied; see
[embedded native objects](#embedded-native-objects).

### CMake integration

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(PhpCxxDemo LANGUAGES CXX)

find_package(PhpCxx 0.1.0 EXACT CONFIG REQUIRED)
add_library(phpcxx_demo MODULE demo.cpp)
target_link_libraries(phpcxx_demo PRIVATE PhpCxx::PhpCxx)
set_target_properties(phpcxx_demo PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
if(APPLE)
    target_link_options(phpcxx_demo PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()
```

`PhpCxx::PhpCxx` supplies the selected PHP headers and C++20 requirement. A loaded
extension resolves Zend symbols from its PHP host; it does not link PHP Embed.
The module uses hidden visibility with an explicitly exported `get_module()`.
The macOS linker option expresses dynamic symbol resolution; it does not establish
macOS runtime support. See the [portability boundary](#portability-boundary).

### PHP usage and cycle collection

`check.php`:

```php
<?php
declare(strict_types=1);

if (demo_copy("a\0b") !== "a\0b") {
    throw new RuntimeException('Binary bytes were not preserved.');
}

$value = new stdClass();
$box = new DemoBox($value);
if ($box->get() !== $value) {
    throw new RuntimeException('The stored object lost its identity.');
}

// Close a cycle through native storage, then prove Zend can collect it.
$value->box = $box;
$weak = WeakReference::create($value);
unset($value, $box);
gc_collect_cycles();
if ($weak->get() !== null) {
    throw new RuntimeException('The native-owned reference was not traced.');
}

echo "php-cxx demo ok\n";
```

Use the root README's [module build and load commands](../README.md#build-a-module-using-php-cxx)
with these files and a matching installed helper. Successful execution prints
`php-cxx demo ok`. The weak reference check demonstrates that the collector can follow
the edge from native storage into the PHP object and reclaim the cycle.

## Public headers

All library symbols below are in namespace `php`. Include the header for the API you
use; no umbrella header or extension-registration framework is required.

| Header | Public API | Reference |
| --- | --- | --- |
| [`phpcxx/zend.hpp`](../src/php-cxx/include/phpcxx/zend.hpp) | Selected PHP/Zend C declarations | [Consumption](#consume-the-library-independently) |
| [`phpcxx/build.hpp`](../src/php-cxx/include/phpcxx/build.hpp) | `BuildInfo`, `build_info()` | [Build metadata](#build-metadata) |
| [`phpcxx/string.hpp`](../src/php-cxx/include/phpcxx/string.hpp) | `String` | [Owners](#owner-api) |
| [`phpcxx/array.hpp`](../src/php-cxx/include/phpcxx/array.hpp) | `Array` | [Owners](#owner-api) |
| [`phpcxx/value.hpp`](../src/php-cxx/include/phpcxx/value.hpp) | `Value` | [Owners](#owner-api) |
| [`phpcxx/views.hpp`](../src/php-cxx/include/phpcxx/views.hpp) | `StringView`, `ArrayView`, `ValueView`, array entries and iterators | [Views](#view-api) |
| [`phpcxx/error.hpp`](../src/php-cxx/include/phpcxx/error.hpp) | `ErrorKind`, `Error` | [Errors and results](#error-and-result-api) |
| [`phpcxx/result.hpp`](../src/php-cxx/include/phpcxx/result.hpp) | `Result<T>`, `Result<void>` | [Errors and results](#error-and-result-api) |
| [`phpcxx/arguments.hpp`](../src/php-cxx/include/phpcxx/arguments.hpp) | `Arguments` | [Arguments](#argument-api) |
| [`phpcxx/conversion.hpp`](../src/php-cxx/include/phpcxx/conversion.hpp) | Checked extraction, numeric conversion, `Timestamp`, native dates | [Extraction](#checked-extraction-api), [dates](#date-api) |
| [`phpcxx/exception.hpp`](../src/php-cxx/include/phpcxx/exception.hpp) | `has_exception()`, `pending_exception()`, `raise()` | [Exception bridge](#exception-api) |
| [`phpcxx/object_binding.hpp`](../src/php-cxx/include/phpcxx/object_binding.hpp) | `GcVisitor`, `ObjectBindingPolicy<T>`, `ObjectBinding<T, Policy>` | [Bindings](#binding-api) |

### Build metadata

`build_info() noexcept -> BuildInfo const&` returns immutable metadata owned by this
copy of the static library. The reference remains valid while the containing module
or executable is loaded. This query is thread-safe and needs no initialized PHP
request. It describes the compiled archive; it does not inspect or validate a running
PHP host.

| `BuildInfo` field | Type | Meaning |
| --- | --- | --- |
| `version_id` | `int` | Selected PHP headers' `major * 10000 + minor * 100 + patch` |
| `module_api` | `int` | `ZEND_MODULE_API_NO`, distinct from PHP and helper package versions |
| `debug` | `bool` | Whether the selected Zend build enables debug mode |
| `zts` | `bool` | Whether the selected PHP build enables Zend Thread Safety |
| `zend_long_size` | `unsigned int` | Size in bytes; supported installations use eight-byte PHP integers |

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

### Owner API

These three owners default-construct empty, delete copy construction/assignment, and
support non-throwing moves. Self-move assignment leaves the owner unchanged. Use
`retain()` or `duplicate()` when a second owner is required. The table omits
`noexcept`: it applies to all listed operations except `String::create()`,
`Array::create()`, all three `duplicate()` methods, and `Array::writable()`.
Allocation failure is not a recoverable error contract.

| Type | Static factories | Instance operations |
| --- | --- | --- |
| `String` | `create(std::string_view) -> String`; `adopt(zend_string*) -> String`; `retain(StringView) -> String` | `duplicate() const -> String`; `release() -> zend_string*`; `get() const -> zend_string const*`; `view() const& -> StringView`; explicit `operator bool() const` |
| `Array` | `create() -> Array`; `adopt(zend_array*) -> Array`; `retain(ArrayView) -> Array` | `duplicate() const -> Array`; `release() -> zend_array*`; `get() const -> zend_array const*`; `view() const& -> ArrayView`; `writable() & -> zend_array*`; explicit `operator bool() const` |
| `Value` | `null() -> Value`; `adopt(zval&) -> Value`; `retain(ValueView) -> Value` | `duplicate() const -> Value`; `release() -> zval`; `get() const -> zval const*`; `view() const& -> ValueView`; `is_initialized() const -> bool`; `is_null() const -> bool` |

`get()` borrows raw read-only storage; it does not transfer ownership. `String` and
`Array` use their boolean conversion to report pointer presence, including a valid
empty value. `Value` has no boolean conversion: `is_initialized()` reports a value
other than `IS_UNDEF`, and `is_null()` tests PHP null. Its `get()` always points to its
internal slot, even when uninitialized. `view()` is deleted for rvalue owners, and
`Array::writable()` is deleted for rvalues, to prevent borrowing from a temporary.

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

### View API

Views are copyable borrows; constructing or copying one does not retain anything.
`StringView(zend_string* = nullptr)` and `ArrayView(zend_array* = nullptr)` accept
raw pointers but expose read-only access. `ValueView(zval const* = nullptr)` borrows
a slot. Each constructor is explicit and `noexcept`.

| Type | Basic operations |
| --- | --- |
| `StringView` | explicit `operator bool() const`; `get() -> zend_string const*`; `bytes() -> std::string_view`; `data()` / `c_str() -> char const*`; `size()` / `length() -> std::size_t`; `empty() -> bool` |
| `ArrayView` | explicit `operator bool() const`; `get() -> zend_array const*`; `size() -> std::size_t`; `empty() -> bool`; `find(key)` / `operator[](key) -> ValueView`; `contains(key) -> bool`; keys are `zend_long` or `std::string_view` |
| `ValueView` | `get() -> zval const*`; `type() -> int` (Zend's `IS_*` tag); `is_initialized() -> bool`; `dereference() -> ValueView`; type predicates, unchecked getters, fallback getters, and `is_truthy()` described above |

Methods in this table are `const noexcept`. All `ValueView` predicates and getters
are also `const noexcept` except `is_truthy() const`, which can invoke PHP handlers.
`ValueView` has no boolean conversion; use the appropriate presence, type, or
truthiness query. Its unchecked getter results are `bool`, `zend_long`, `double`,
`StringView`, `ArrayView`, `zend_object const*`, and `zend_resource const*`, respectively.
The three `_or` getters take and return the corresponding scalar type.

`StringView` has the following additional `const noexcept` operations:

| Operation | Parameters and result |
| --- | --- |
| `operator[]`, `front`, `back` | `operator[](std::size_t)` or no arguments; return `char const&` |
| `begin`, `end`, `cbegin`, `cend` | No arguments; return `const_iterator`, an alias for `char const*` |
| `rbegin`, `rend` | No arguments; return `const_reverse_iterator`, an alias for `std::reverse_iterator<const_iterator>` |
| `compare`, `icompare` | `std::string_view` or `StringView`; return negative/zero/positive `int` |
| `iequals` | `std::string_view` or `StringView`; returns `bool` |
| `starts_with`, `ends_with`, `istarts_with`, `iends_with`, `contains` | `std::string_view`, `StringView`, or `char`; return `bool` |
| `find` | Needle as `std::string_view`, `StringView`, or `char`; optional `std::size_t position = 0`; returns `std::size_t` |
| `rfind` | Same needle types; optional `std::size_t position = npos`; returns `std::size_t` |

The nonmember `operator==` supports two `StringView` values or one `StringView` and
one `std::string_view` in either order. It is `noexcept` and compares bytes.
`StringView::npos` is `std::string_view::npos`. `find` starts at the supplied offset;
`rfind` finds the last match starting at or before its offset, following
`std::string_view` search semantics.

`ArrayView::Key` is `std::variant<zend_long, StringView>`.
`ArrayView::Entry` contains `Key key` and `ValueView value`. Its input iterator,
`ArrayView::Iterator` (also named `iterator` and `const_iterator`), supports default
construction, `*`, `->`, prefix/postfix `++`, and `==`. Dereference returns an `Entry`
by value. Arrow access uses a temporary `ArrowProxy`; do not retain a pointer obtained
from it. Default iterators are absent-array end iterators, and end iterators from
different arrays are unequal. Dereference and increment require a non-end iterator.

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

### Error and result API

`Error` is a value with `ErrorKind kind`, owned `std::string message`, and
`std::uint32_t argument = 0`. Zero means no argument context; nonzero values are
one-based argument numbers. Its `kind` and `message` should describe a recoverable
failure without changing Zend's exception state.

| `ErrorKind` | Meaning when raised |
| --- | --- |
| `Type` | `TypeError` |
| `Value` | `ValueError` |
| `ArgumentCount` | `ArgumentCountError` |
| `Runtime` | `Exception`, or the supplied exception class |
| `PendingException` | Preserve the existing Zend exception; create nothing |

`Result<T>(T)` constructs success and `Result<T>(Error)` constructs failure, moving
the supplied value. `Result<void>()` constructs success without a payload;
`Result<void>(Error)` constructs failure. Copyability of `Result<T>` follows `T`.

| Accessor | Result and precondition |
| --- | --- |
| explicit `operator bool() const noexcept` | `true` for success |
| `value() & noexcept` | `T&`; requires success |
| `value() const& noexcept` | `T const&`; requires success |
| `value() && noexcept` | `T&&`; requires success; move into an owner before destroying the result |
| `error() const& noexcept` | `Error const&`; requires failure and a live result |
| `error() && noexcept` | `Error`; moves the failure out |

`Result<void>` provides the same boolean and error accessors, but no `value()`.
These containers need no PHP request themselves; a contained Zend owner or view still
has its usual lifetime restrictions.

### Argument API

Construct `Arguments(zend_execute_data* frame) noexcept` only for the current internal
call. All methods below are `const`. Typed methods return `Result` and require the
caller to handle failure; they do not turn a returned error into a PHP exception.

| Method | Return type |
| --- | --- |
| `size() noexcept` | `std::uint32_t` |
| `get(std::uint32_t index) noexcept` | `ValueView` |
| `count(std::uint32_t minimum, std::uint32_t maximum)` | `Result<void>`; requires `minimum <= maximum` |
| `string(std::uint32_t index)` | `Result<StringView>` |
| `boolean(std::uint32_t index)` | `Result<bool>` |
| `integer(std::uint32_t index)` | `Result<zend_long>` |
| `floating(std::uint32_t index)` | `Result<double>` |
| `array(std::uint32_t index)` | `Result<ArrayView>` |
| `object(std::uint32_t index, zend_class_entry const* expected)` | `Result<zend_object const*>`; `expected` identifies the required class/interface |

### Checked extraction API

Each `as_*` operation takes `ValueView value`. `as_object` additionally requires
`zend_class_entry const* expected`. Their return types are `Result<StringView>`,
`Result<bool>`, `Result<zend_long>`, `Result<double>`, `Result<ArrayView>`, and
`Result<zend_object const*>`, in the order listed in the extraction table above.
Use these exact-type operations for stored values, and `Arguments` when parsing a
PHP call under Zend's weak/strict argument rules.

| Numeric conversion | Return type |
| --- | --- |
| `checked_integer<T>(zend_long value)` | `Result<T>`; `T` must be a non-bool integral type |
| `integer_from_double(double value)` | `Result<zend_long>` |
| `double_from_integer(zend_long value) noexcept` | `double` |

### Exception API

| Function | Contract |
| --- | --- |
| `has_exception() noexcept -> bool` | Query the current request/thread's pending PHP exception |
| `pending_exception() -> Error` | Create a marker; requires `has_exception()` to be true |
| `raise(Error const& error, zend_class_entry* exception_class = nullptr) -> void` | Signal the error in an active PHP execution frame; preserve an existing exception |

These functions operate within an initialized request on its execution thread.
`raise()` returns normally to C++; it does not perform the caller's early return.
After a Zend operation that can execute PHP, check for a pending exception before
continuing successful work. Returning `pending_exception()` lets an outer callback
propagate the failure without replacing it.

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

### Date API

`Timestamp` has `std::int64_t seconds` and `std::int32_t microseconds`. Inputs must
use normalized microseconds in the range 0–999999; conversion to PHP also requires
an instant representable by PHP's native date implementation.

| Function | Return type |
| --- | --- |
| `as_timestamp(ValueView value)` | `Result<Timestamp>` |
| `datetime_immutable(Timestamp value)` | `Result<Value>` owning a PHP `DateTimeImmutable` |
| `unix_microseconds(Timestamp value)` | `Result<std::int64_t>` |
| `timestamp_from_microseconds(std::int64_t value) noexcept` | `Timestamp` |

The integer-only timestamp functions need no PHP request. The two PHP date functions
require a live request, may fail, and preserve existing PHP exceptions. Check their
results before using the returned timestamp or owner.

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
introducing a second registration system.

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
types fail compilation. Construction must not execute PHP, invoke GC, or publish partially
constructed state. Perform recoverable validation before construction, or use
`Result`-based initialization after construction while an owner protects the
object. A PHP constructor that raises after successful native construction leaves
cleanup to the object's eventual `free_obj`; it does not manually destroy `T`.

The [complete extension example](#complete-extension-example) demonstrates this
lifecycle with a `StoredValue` owning a `php::Value` and an explicit GC trace method.

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

### Binding API

`ObjectBinding<T, Policy = ObjectBindingPolicy<T>>` exposes three static entry points:

| Method | Result and lifetime |
| --- | --- |
| `bind(zend_class_entry* entry)` | `Result<void>`; call during module startup with the registered ordinary class |
| `construct(zend_object* object, Args&&... args)` | `Result<T*>`; forwards arguments to a non-throwing native constructor, once per object |
| `get(zend_object* object, zend_class_entry* expected = nullptr)` | `Result<T*>`; borrows initialized native state, optionally checking a class/interface |

`construct()` and `get()` require a live object; keep a PHP owner or active `$this`
call alive throughout use of the returned pointer. No result owns or retains `T`.

`GcVisitor::add(ValueView) const noexcept -> void` reports one owned PHP edge. The
binding passes a visitor to the policy's trace hook. Its explicit
`GcVisitor(zend_get_gc_buffer*) noexcept` constructor requires a non-null Zend GC
buffer; module authors normally use the supplied visitor instead of constructing one.

| Policy member | Requirement |
| --- | --- |
| `static constexpr bool cloneable` | Required; `false` in the default policy |
| `static trace(T const&, GcVisitor&) noexcept -> void` | Required; default forwards to `T::trace(GcVisitor&) const noexcept` |
| `static clone(T const&) noexcept -> Result<T>` | Required only when `cloneable` is true; `T` must also be nothrow-movable |
| `static compare(T const&, T const&) noexcept -> int` | Optional; native comparison before standard PHP properties |

For a scalar-only state, an empty trace hook is appropriate. For a state containing
`php::Value` owners, enumerate each owned edge as `StoredValue` does in the complete
example. Borrowed pointers are not owned edges. A clone policy must choose whether to
retain, duplicate, or independently reconstruct each owned value; C++ copy syntax
cannot substitute for that decision.

## Runtime and request lifetimes

PHP's module loader owns runtime and request startup/shutdown. Register persistent
functions/classes at module startup, and create request-owned values only within an
active request. Destroy external C++ owners before request shutdown starts; bound
objects release their native owners through Zend's normal object teardown. Module
startup and shutdown are not substitutes for request lifetime boundaries.

Register generated persistent functions/classes during module startup. Zend
borrows a registered module entry until module shutdown, so its storage must
outlive registration. Reinitialize its bookkeeping for a new runtime and restore
any replaced SAPI callbacks. Never cache request class entries, zvals, views, or
native pointers for reuse after shutdown. The library supplies no production
runtime lifecycle owner; an embedding host is responsible for these boundaries.

In ZTS, each owner and view belongs to one request on one execution thread. Do not
transfer Zend values or native object pointers to worker threads. Native results
without PHP ownership can cross threads using the application's synchronization.
For host-side Embed and ZTS test setup, see the
[development README](../src/php-cxx/README.md#native-storage-and-lifecycle-fixtures).
Helper ZTS support does not establish support for a ZTS JobU extension.

## Portability boundary

The helper's public API contains no JobU transport or POSIX types. It relies on
the selected Zend ABI, allocator alignment, and C++20 support. Linux is the
validated environment for the project's regression matrix. Windows and macOS
execution are not established by these results; Windows compiler, PHP discovery,
link/export, and Embed integration still require platform validation. Keep
platform-specific JobU event-loop and socket adapters outside this library.
