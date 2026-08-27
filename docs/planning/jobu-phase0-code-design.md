# JobU Phase 0 Code-Level Design

## 1. Status and purpose

This document defines the implementation contract for Phase 0 of JobU. It refines the Phase 0 entry in the JobU v1 technical plan into reviewable source changes.

Phase 0 establishes buildable module boundaries and small reusable contracts. It does not implement persistence, RPC framing, scheduling, job execution, or daemon operation.

The design is based on the current `main` branch of `evaikene/deferra`, where:

- the project uses C++20 and CMake 3.20 or newer;
- `src/core` and `src/net` build static libraries named `core` and `net`;
- public headers live directly in each module directory and use snake-case names;
- public C++ namespaces are `jb::core` and `jb::net`;
- tests are separate Catch2 executables in `test`, registered with CTest;
- `jb::core::Clock` already aliases `std::chrono::steady_clock`;
- `jb::core::ValueResult<T>` is already used by conversion helpers.

The new contracts must preserve those existing APIs.

## 2. Phase 0 deliverables

Phase 0 delivers:

1. Build skeletons for the `db`, `rpc`, and `jobu` static libraries and the `jobud` and `jobuctl` executables.
2. A general value-or-error result type.
3. A project-owned error representation suitable for module-specific stable codes.
4. Binary-buffer and read-only binary-view types.
5. A platform-neutral UUID value and an injectable UUIDv7 generator contract.
6. Injectable wall and monotonic time through a `TimeSource` contract.
7. JobU attribute names, values, scopes, definitions, and registry interfaces.
8. Fake-time and temporary-directory test helpers.
9. Contract tests and compile-time boundary tests.
10. The v1 invariants catalogued in Section 10 of this document.

Phase 0 exit criteria are:

- configuration and compilation succeed on Linux and macOS;
- all Phase 0 tests pass without real sleeps;
- none of SQLite, libcurl, or nlohmann/json is required yet;
- no scheduler or external job operation can run;
- public headers expose only project-owned and C++ standard-library types.

## 3. Non-goals

Phase 0 must not add placeholder behavior that will later be mistaken for working functionality. In particular, it does not:

- open or migrate a database;
- create a local socket;
- parse or serialize JSON;
- start an event loop in either executable;
- define the complete queue/job/run/attempt domain model;
- parse cron expressions;
- implement attribute default materialization;
- generate cryptographically secure UUIDs on unsupported platforms by silently using a weak fallback;
- add stub RPC methods that always return success.

The executables exist only to prove target composition. Each accepts `--version` and otherwise prints a clear `not implemented` diagnostic to stderr and exits with `EXIT_FAILURE`. `--version` prints the project version and exits successfully. No `jb::core::Application` is needed until an executable has event-driven behavior.

## 4. Target graph

The existing target names remain unchanged. Phase 0 adds the following dependency graph:

```text
core
  |-- net
  |-- db
  `-- rpc
        \
net -----+--> jobu
db ------+
rpc -----+
              |-- jobud
              `-- jobuctl
```

Concrete CMake relationships:

| Target | Kind | Public dependency | Private dependency |
| --- | --- | --- | --- |
| `core` | static library | `fmt::fmt` (existing) | none |
| `net` | static library | `core` (existing) | none |
| `db` | static library | `core` | none in Phase 0 |
| `rpc` | static library | `core` | none in Phase 0 |
| `jobu` | static library | `core`, `net`, `db`, `rpc` | none in Phase 0 |
| `jobud` | executable | none | `jobu` |
| `jobuctl` | executable | none | `jobu` |

`jobu` may expose types from its own headers but must not re-export dependency headers accidentally. CMake's public linkage is used because consumers of JobU contracts will need the linked libraries; includes in individual headers must still be minimal.

The root CMake project continues to own the version. Add a generated private header for the two executables rather than defining the version twice.

## 5. Proposed source layout

```text
src/
  CMakeLists.txt
  core/
    CMakeLists.txt
    byte_buffer.hpp
    error.hpp
    result.hpp
    time_source.cpp
    time_source.hpp
    uuid.cpp
    uuid.hpp
  db/
    CMakeLists.txt
    db.cpp
    db.hpp
  rpc/
    CMakeLists.txt
    rpc.cpp
    rpc.hpp
  jobu/
    CMakeLists.txt
    attribute.cpp
    attribute.hpp
    jobu.cpp
    jobu.hpp
  jobud/
    CMakeLists.txt
    main.cpp
  jobuctl/
    CMakeLists.txt
    main.cpp
test/
  CMakeLists.txt
  support/
    fake_time_source.hpp
    temporary_directory.cpp
    temporary_directory.hpp
  attribute-test.cpp
  byte-buffer-test.cpp
  result-test.cpp
  time-source-test.cpp
  uuid-test.cpp
doc/
  jobu-phase0-code-design.md
```

`db.hpp`, `rpc.hpp`, and `jobu.hpp` are module marker/version headers in Phase 0, not umbrella headers that include every future API. Each exposes only a module version constant and namespace. Their `.cpp` files ensure that the skeletons are real linkable libraries rather than CMake `INTERFACE` targets.

## 6. Core contracts

### 6.1 `Result<T, E>`

Add `jb::core::Result<T, E>` in `src/core/result.hpp`. It is a value type implemented with an indexed `std::variant`, supporting `T` and `E` even when they are the same type.

Required public behavior:

```cpp
namespace jb::core {

struct UninitializedResult {};

template <typename T, typename E>
class [[nodiscard]] Result {
public:
    Result() noexcept;

    static auto success(T value) -> Result;
    static auto failure(E error) -> Result;

    [[nodiscard]] auto is_initialized() const noexcept -> bool;
    [[nodiscard]] auto has_value() const noexcept -> bool;
    explicit operator bool() const noexcept;

    auto value() & -> T&;
    auto value() const& -> T const&;
    auto value() && -> T&&;
    auto error() & -> E&;
    auto error() const& -> E const&;
    auto error() && -> E&&;

    auto operator*() & -> T&;
    auto operator*() const& -> T const&;
    auto operator->() -> T*;
    auto operator->() const -> T const*;

    template <typename U>
    auto value_or(U&& fallback) const& -> T;
};

template <typename E>
class [[nodiscard]] Result<void, E>;

} // namespace jb::core
```

Semantics:

- default construction creates an explicitly uninitialized result;
- `operator bool()` is equivalent to `has_value()`;
- accessing the wrong alternative or an uninitialized result is a programmer error and throws `std::logic_error` in Phase 0;
- `Result<void, E>::success()` has no argument and supports `error()` but not dereference, arrow, or `value_or()`;
- equality is provided when the contained types are equality comparable;
- no implicit construction from `T` or `E` is permitted;
- no exception is used to report the represented operational failure.

`ValueResult<T>` remains unchanged in Phase 0. Migrating existing parsing utilities to `Result` is a separate, optional cleanup because it changes their field-based API and is unrelated to the JobU foundation.

### 6.2 Error representation

Add `jb::core::Error` in `src/core/error.hpp`:

```cpp
enum class ErrorCategory : std::uint8_t {
    InvalidArgument,
    NotFound,
    Conflict,
    PermissionDenied,
    Unavailable,
    ResourceExhausted,
    Cancelled,
    Timeout,
    Io,
    Unsupported,
    Internal,
};

struct Error {
    ErrorCategory category{ErrorCategory::Internal};
    std::string   code;
    std::string   message;
    std::string   detail;
};
```

Rules:

- `category` supports generic control-flow decisions and later RPC mapping;
- `code` is a stable, module-owned machine identifier such as `db.schema_too_new`; it is never generated from display text;
- `message` is safe for users and logs;
- `detail` is optional backend/diagnostic text and must not be required for program decisions;
- sensitive values must not be placed in any field;
- future structured RPC error data is produced by an adapter and is not added here as a JSON-shaped map.

Modules may use `Result<T, Error>` directly. A module-specific richer error may wrap or contain `Error`, but must preserve the generic category and stable code.

### 6.3 Binary data

Add `src/core/byte_buffer.hpp`:

```cpp
using ByteBuffer = std::vector<std::byte>;
using ByteView   = std::span<std::byte const>;
```

Also provide explicit helpers:

```cpp
auto as_bytes(std::string_view value) noexcept -> ByteView;
auto as_string_view(ByteView value) noexcept -> std::string_view;
```

The helpers are byte-preserving and perform no UTF-8 validation. `as_string_view()` is a non-owning view. Callers must not retain it beyond the source buffer lifetime.

Database blobs, captured output, and framing payloads use these types. Existing `IODevice` remains string-based in Phase 0; converting it is out of scope.

### 6.4 Time

Do not introduce a type named `Clock`, because `jb::core::Clock` already denotes the event loop's steady clock.

Add `src/core/time_source.hpp`:

```cpp
using UtcClock     = std::chrono::system_clock;
using UtcTimePoint = UtcClock::time_point;

class TimeSource {
public:
    virtual ~TimeSource() = default;

    [[nodiscard]] virtual auto utc_now() const noexcept -> UtcTimePoint = 0;
    [[nodiscard]] virtual auto monotonic_now() const noexcept -> TimePoint = 0;
};

class SystemTimeSource final : public TimeSource {
public:
    [[nodiscard]] auto utc_now() const noexcept -> UtcTimePoint override;
    [[nodiscard]] auto monotonic_now() const noexcept -> TimePoint override;
};
```

Persisted timestamps use `UtcTimePoint`; in-process deadlines use the existing steady `TimePoint`. Conversion between them is always an explicit policy decision at the scheduler boundary.

Production components receive a `TimeSource&` in their constructors. They must not call `system_clock::now()` or `steady_clock::now()` directly except inside `SystemTimeSource` and the existing event-loop implementation.

### 6.5 UUID and UUIDv7 generation

Add a 16-byte `jb::core::Uuid` value in `src/core/uuid.hpp`.

Required API:

```cpp
class Uuid {
public:
    using Storage = std::array<std::byte, 16>;

    constexpr Uuid() noexcept = default;
    explicit constexpr Uuid(Storage bytes) noexcept;

    [[nodiscard]] static auto parse(std::string_view text) -> Result<Uuid, Error>;
    [[nodiscard]] auto to_string() const -> std::string;
    [[nodiscard]] constexpr auto bytes() const noexcept -> Storage const&;
    [[nodiscard]] constexpr auto is_nil() const noexcept -> bool;

    auto operator<=>(Uuid const&) const = default;
};
```

Also provide `std::hash<jb::core::Uuid>`. Canonical output is lower-case `8-4-4-4-12`; parsing accepts either case but requires the canonical hyphen positions and exactly 32 hexadecimal digits. Database adapters will later bind the 16-byte storage directly.

Generation is separated from the value:

```cpp
class UuidGenerator {
public:
    virtual ~UuidGenerator() = default;
    [[nodiscard]] virtual auto generate() -> Result<Uuid, Error> = 0;
};

class UuidV7Generator final : public UuidGenerator {
public:
    explicit UuidV7Generator(TimeSource& time_source);
    [[nodiscard]] auto generate() -> Result<Uuid, Error> override;
};
```

`UuidV7Generator` uses an operating-system cryptographic random source, sets RFC 9562 version and variant bits, and is safe for calls from multiple threads. Within one process it must produce monotonically increasing UUID byte order, including multiple UUIDs in the same millisecond and small wall-clock regressions. Random-source failure is returned; there is no pseudo-random fallback. Exact entropy/backend selection is an implementation detail isolated in `uuid.cpp`.

Unit tests use a deterministic test generator where stable IDs are preferable. They do not make the production generator accept a public random callback.

## 7. Attribute contracts

Attribute contracts belong to `jb::jobu`, not `jb::core`, because their scopes and validation rules are JobU domain policy.

### 7.1 Names and scopes

```cpp
using AttributeName = std::string;

enum class AttributeScope : std::uint8_t {
    DaemonDefault = 0x01,
    QueueDefault  = 0x02,
    Job           = 0x04,
};
using AttributeScopes = jb::core::enum_bitmask<AttributeScope>;
```

Canonical names are lower-case dotted ASCII identifiers, for example `job.timeout` and `retry.max_attempts`. A name segment begins with `[a-z]` and continues with `[a-z0-9_]*`. Unknown names are errors.

### 7.2 Values

Use a recursive project-owned value rather than a JSON type:

```cpp
struct AttributeValue {
    using List = std::vector<AttributeValue>;
    using Map  = std::map<std::string, AttributeValue, std::less<>>;
    using Data = std::variant<bool,
                              std::int64_t,
                              double,
                              std::string,
                              jb::core::Duration,
                              jb::core::ByteBuffer,
                              List,
                              Map>;

    Data data;
};

using AttributeSet = std::map<AttributeName, AttributeValue, std::less<>>;
```

There is deliberately no null alternative. Absence from a partial input means `not supplied`; a materialized job attribute set contains every registered job attribute. Secrets and runtime references will use explicit domain value-reference types in the runner payload rather than magic strings or generic null values.

### 7.3 Registry

```cpp
enum class AttributeType : std::uint8_t {
    Boolean,
    Integer,
    Number,
    String,
    Duration,
    Bytes,
    List,
    Map,
};

struct AttributeDefinition {
    AttributeName   name;
    AttributeType   type;
    AttributeScopes scopes;
    AttributeValue  built_in_default;
    std::string     description;
    bool            sensitive{false};
};

class AttributeRegistry {
public:
    virtual ~AttributeRegistry() = default;

    [[nodiscard]] virtual auto find(std::string_view name) const noexcept
        -> AttributeDefinition const* = 0;
    [[nodiscard]] virtual auto validate(std::string_view name,
                                        AttributeValue const& value,
                                        AttributeScope scope) const -> jb::core::Result<void, jb::core::Error> = 0;
    [[nodiscard]] virtual auto definitions() const
        -> std::span<AttributeDefinition const> = 0;
};
```

The built-in registry implementation and complete initial attribute list are Phase 3 work. Phase 0 supplies a small test registry only, proving type, scope, unknown-name, and constraint error contracts. Serialization is intentionally absent until the private JSON adapter is introduced.

Validation errors use stable codes:

- `jobu.attribute.invalid_name`;
- `jobu.attribute.unknown`;
- `jobu.attribute.wrong_scope`;
- `jobu.attribute.wrong_type`;
- `jobu.attribute.constraint`.

## 8. Test support

### 8.1 Fake time

`test/support/fake_time_source.hpp` defines a final `FakeTimeSource` implementing `TimeSource`. It stores UTC and monotonic points separately and provides:

```cpp
void set_utc(UtcTimePoint value);
void set_monotonic(TimePoint value);
void advance(Duration duration);
```

`advance()` moves both clocks by the same duration. Separate setters allow tests for wall-clock jumps. The helper is single-threaded; scheduler tests own and mutate it only from the test thread.

Phase 0 does not modify `EventLoop` to consume `TimeSource`. Future scheduler tests drive scheduler deadlines through the fake while event-loop timer integration receives separate focused tests.

### 8.2 Temporary resources

`TemporaryDirectory` is a move-only RAII helper that:

- creates a uniquely named directory under `std::filesystem::temp_directory_path()`;
- exposes its path;
- recursively removes only that exact directory at destruction;
- reports creation failure through an exception because test setup cannot continue;
- offers `release()` for a failing test that needs to preserve artifacts.

The generated name combines a fixed `jobu-test-` prefix with a UUID or equivalent collision-resistant suffix. The destructor never throws. Cleanup failure is reported through Catch assertions only when `cleanup()` is called explicitly; destructor cleanup is best effort.

Do not move the existing ad hoc test-directory helpers during Phase 0. New JobU tests use the shared helper, and older tests may migrate in a separate cleanup.

### 8.3 Test registration

Keep the repository's one-executable-per-feature convention. Add a small CMake helper function only if it reduces repeated registration without rewriting existing test declarations. New tests link the narrowest target they exercise.

Required Phase 0 coverage:

- all three `Result` states, same `T`/`E` type, move-only values, `void`, dereference, arrow, and wrong-state access;
- error value construction and stable-code preservation;
- byte conversion preserves embedded NUL and non-UTF-8 bytes;
- fake UTC and monotonic time can advance together and jump independently;
- UUID parse/format round trip, invalid forms, version/variant bits, uniqueness, and monotonic byte order;
- attribute name grammar, value types, scopes, unknown attributes, and constraint errors;
- temporary directories are unique and removed after scope exit;
- both executables return success for `--version` and failure for unsupported invocation.

## 9. Layering and public-header rules

The following rules are enforceable from Phase 0 onward:

- `core` depends only on the C++ standard library and its existing `fmt` dependency.
- `net`, `db`, and `rpc` may depend on `core` but not on one another.
- `jobu` may depend on all four foundation modules.
- `jobud` and `jobuctl` contain composition and presentation code; reusable behavior stays in libraries.
- a public header must not include SQLite, libcurl, nlohmann/json, POSIX, or macOS framework headers;
- dependency-specific handles may appear only in `.cpp` or private headers;
- errors cross a public boundary as project-owned values;
- persisted or wire representations are adapters around domain types, never the domain types themselves.

An optional CMake compile test may include every new public header in one translation unit. This catches missing direct includes and accidental backend leakage early.

## 10. V1 invariant catalogue

The following invariants are recorded now and become executable checks in the indicated later phase.

| Invariant | Owning layer | First enforcement phase |
| --- | --- | --- |
| Only one active JobU process owns a database. | daemon/database bootstrap | 1 |
| Unknown attributes are rejected. | attribute registry | 3 |
| Job creation stores the fully materialized attribute set. | job service/repository | 3 |
| Queue-default changes affect only future jobs. | job service | 3 |
| Moving a job never reapplies target-queue defaults. | job service | 3 |
| Job updates require the expected revision and increment it. | job repository | 3 |
| A one-time job is immutable after its first attempt starts. | job service/repository | 4 |
| One run has at most one running attempt. | run repository | 3 |
| A recurring definition has exactly one schedule-owned non-terminal run. | scheduler/repository | 4 |
| A manual run never replaces or retimes its schedule-owned run. | scheduler | 4 |
| An attempt starts externally only after its `running` transition commits. | scheduler/repository | 4 |
| Capacity is released only after completion state commits. | scheduler/repository | 4 |
| Retry creates an attempt, not a run; the run UUID stays stable. | scheduler | 4 |
| Retry wait consumes no CLI/HTTP slot. | scheduler | 4 |
| Blocking retry retains queue capacity only while the job is active. | scheduler | 4 |
| Suspension prevents new attempts after the state transaction commits. | scheduler/service | 4 |
| Lowered capacity never cancels active work. | scheduler | 4 |
| Missed recurring occurrences coalesce and are not replayed individually. | cron scheduler | 4 |
| CLI and HTTP bodies never execute on the scheduler thread. | runners | 5/6 |
| CLI receives a clean environment plus only resolved and JobU-owned values. | CLI runner | 6 |
| JobU metadata environment variables and HTTP headers cannot be overridden. | runners | 5/6 |
| Capture limits never stop draining the external stream. | runners | 5/6 |
| Completion state and captured output commit atomically. | scheduler/repository | 7 |
| A scheduler-state write failure stops mutation and dispatch and exits nonzero. | daemon/scheduler | 7 |
| Startup recovery completes before mutation service and dispatch open. | daemon/scheduler | 7 |
| Secret values are never returned by read RPCs or written to logs. | secret service/RPC | 8 |

Phase 0 tests cannot enforce later behavior. The catalogue prevents later phases from silently redefining the agreed semantics.

## 11. Reviewable implementation sequence

Repository instructions require approval before edits and one approved stage at a time. Implement Phase 0 as these independent patches:

### Stage 0.1: Target skeletons

- add module directories and CMake targets;
- add `--version`/not-implemented executable behavior;
- add executable smoke tests;
- verify configure, build, and CTest.

### Stage 0.2: Result and error

- add `result.hpp` and `error.hpp`;
- add focused tests, including same-type and `void` results;
- do not migrate `ValueResult<T>`.

### Stage 0.3: Bytes, time, and UUID

- add byte buffer/view helpers;
- add `TimeSource` and `SystemTimeSource`;
- add UUID value and UUIDv7 generator;
- add deterministic contract tests.

### Stage 0.4: Attribute and test-support contracts

- add attribute value, definition, and registry contracts;
- add fake time and temporary-directory helpers;
- add tests and the invariant catalogue to the repository documentation;
- perform the final Linux build/test pass and a macOS CI build/test pass.

Each stage stops after reporting changed files, validation results, and the next proposed stage. Later-phase implementation must not be pulled forward merely to make a skeleton appear useful.

## 12. Validation commands

Run after every stage:

```sh
cmake -B .bld -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld
ctest --test-dir .bld/test --output-on-failure
```

Run formatting on changed C++ files using the checked-in `.clang-format`. At the end of Phase 0, configure with both Clang and GCC on Linux where CI capacity permits, and build/test with Apple Clang on macOS.

No Phase 0 test may rely on a real-time sleep, network access, a writable fixed system path, or test ordering.
