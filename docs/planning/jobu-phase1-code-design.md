# JobU Phase 1 Code-Level Design

## 1. Status and purpose

This document defines the reduced Phase 1 of JobU: a small Qt SQL-inspired database API, a backend-driver boundary, and the first SQLite driver.

It is based on:

- the JobU v1 technical plan;
- Phase 0 merged into `main` as commit `4e6724834dc49df447f9f9224c88a7e03acab207`;
- the current `jb::core::Result<T, E>`, `Error`, and `ByteBuffer` contracts;
- the repository's staged implementation rule.

Phase 1 deliberately stops below the application schema. It does not know how JobU stores queues, jobs, runs, attempts, migration versions, or any other application object.

## 2. Revised scope

Phase 1 delivers only:

1. Backend-independent `Value`, `Field`, and `Record` types.
2. A backend-independent `Driver` contract.
3. Backend-independent `Database` and forward-only `Query` classes modeled after Qt SQL.
4. Direct transaction methods on `Database`.
5. A move-only RAII `Transaction` guard.
6. An SQLite driver implemented outside the generic database classes.
7. Focused generic-contract and SQLite integration tests.

Phase 1 exit criteria are:

- all targets build on Linux and macOS;
- application code can select an SQLite driver, open a `Database`, execute prepared queries, read records, and use transactions;
- `database.cpp`, `query.cpp`, and other generic sources contain no SQLite includes, symbols, result codes, PRAGMAs, or filesystem assumptions;
- SQLite is linked only by the SQLite-driver target;
- a future MariaDB/MySQL or PostgreSQL driver can be added without modifying the public `Database`, `Query`, `Record`, `Value`, or `Transaction` APIs;
- no migration or JobU schema code exists in `jb::db`.

## 3. Explicitly deferred

The following are not Phase 1 work:

- migration descriptors or a migration runner;
- a schema-migrations table;
- schema version checks or newer-schema refusal;
- automatic database backups;
- JobU application tables and repositories;
- application-level database initialization;
- MySQL, MariaDB, or PostgreSQL drivers;
- runtime driver plugins or a global driver registry;
- named/default database connections;
- connection pooling;
- asynchronous database access;
- an ORM or SQL query builder;
- SQL-dialect translation;
- savepoints and nested transactions.

JobU schema creation and upgrades will be a separate application-level phase. That phase may use ordinary `Database`, `Query`, and `Transaction` operations, as ZendHQ does, without turning migration policy into a reusable database-library feature.

## 4. Architecture

### 4.1 Layering

```text
JobU repositories (later phase)
            |
            v
  Database / Query / Transaction
            |
            v
          Driver
            |
            v
       sqlite::Driver
            |
            v
          SQLite
```

The generic layer owns:

- API state and Qt-like behavior;
- thread-affinity checks;
- query lifecycle and current-record state;
- direct versus RAII transaction ownership;
- stable project error categories and codes that are not backend-specific.

The driver owns:

- opening and closing the actual backend connection;
- backend-specific configuration and options;
- prepared-statement implementation;
- translating backend errors into `jb::core::Error`;
- beginning, committing, and rolling back native transactions;
- any backend-specific resource ownership.

The SQLite driver additionally owns:

- the database filesystem path;
- adjacent process locking;
- SQLite open flags and API calls;
- WAL, foreign-key, synchronous, and busy-timeout configuration;
- SQLite result-code translation;
- `sqlite3*` and `sqlite3_stmt*` lifetimes.

### 4.2 Qt SQL correspondence

| Qt SQL | JobU | Notes |
| --- | --- | --- |
| `QSqlDatabase` | `jb::db::Database` | Move-only; no global/default connection registry |
| `QSqlDriver` | `jb::db::Driver` | Backend contract owned by `Database` |
| `QSqlQuery` | `jb::db::Query` | Prepare, bind, exec, next, record, value, finish |
| `QSqlResult` | `jb::db::DriverQuery` | Driver implementation contract, not an application-facing API |
| `QSqlRecord` | `jb::db::Record` | Read-only field metadata and current values |
| `QSqlField` | `jb::db::Field` | Field name and value |
| `QVariant` | `jb::db::Value` | Exact database alternatives, without implicit conversions |
| `QSqlError` | `jb::core::Error` | Stable category/code and backend detail |
| — | `jb::db::Transaction` | Move-only RAII guard layered over database transactions |

Intentional differences from Qt SQL:

- no global connection names or default database;
- no copyable/implicitly shared database or query handles;
- fallible operations return `Result` instead of relying only on `bool` and `lastError()`;
- all queries are forward-only in v1;
- every placeholder must be bound explicitly;
- `Record` is read-only because `jb::db` does not generate SQL from records;
- driver selection is compile-time composition, not a runtime plugin registry.

### 4.3 `[[nodiscard]]` policy

Use `[[nodiscard]]` selectively rather than on every `Result`-returning method.

Keep it on operations whose failure is a normal runtime outcome that must usually be handled immediately:

- database open and transaction begin/commit/rollback;
- query prepare, execution, and row fetch;
- RAII transaction begin/commit/rollback;
- getters whose returned value is the entire purpose of the call.

Do not use it on operations that normally succeed and mainly report programmer misuse or cleanup problems:

- `Database::close()`;
- `Query::bind_value()`;
- `Query::add_bind_value()`;
- `Query::finish()`.

These methods remain fallible and return `Result`; callers may check when useful. Ignored binding failure is retained in `last_error()` and leaves the slot unbound, so the next `exec()` also fails with `db.parameter_unbound`. Ignored `finish()`/`close()` failure remains observable through object state and `last_error()`.

The internal driver SPI may retain `[[nodiscard]]` on operations that generic `Database` or `Query` must always propagate. That does not impose checks on application call sites.

## 5. Targets and source layout

### 5.1 Target separation

Use two library targets:

| Target | Responsibility | Dependencies |
| --- | --- | --- |
| `db` | Generic values, records, driver contract, database, query, transaction | `core` |
| `db-sqlite` | SQLite driver | `db`, private SQLite library |

`db` must configure and compile without SQLite installed. Only configuring or building `db-sqlite` requires SQLite.

The existing top-level build may enable `db-sqlite` unconditionally for JobU v1, but its dependency remains isolated:

```cmake
# src/db/CMakeLists.txt
add_library(db STATIC ...)
target_link_libraries(db PUBLIC core)

option(JB_BUILD_SQLITE_DRIVER "Build the SQLite database driver" ON)
if (JB_BUILD_SQLITE_DRIVER)
    add_subdirectory(sqlite)
endif()
```

```cmake
# src/db/sqlite/CMakeLists.txt
find_package(SQLite3 REQUIRED)

add_library(db-sqlite STATIC ...)
target_link_libraries(db-sqlite
    PUBLIC db
    PRIVATE SQLite::SQLite3
)
```

### 5.2 Files

```text
src/db/
  CMakeLists.txt
  database.cpp
  database.hpp
  database_priv.hpp
  db.cpp
  db.hpp
  driver.hpp
  driver_query.hpp
  query.cpp
  query.hpp
  query_priv.hpp
  record.cpp
  record.hpp
  transaction.cpp
  transaction.hpp
  value.cpp
  value.hpp
  sqlite/
    CMakeLists.txt
    sqlite_driver.cpp
    sqlite_driver.hpp
    sqlite_driver_priv.hpp
    sqlite_query.cpp
    sqlite_query_priv.hpp

test/
  support/
    fake_database_driver.cpp
    fake_database_driver.hpp
  db-api-test.cpp
  db-query-test.cpp
  db-transaction-test.cpp
  db-sqlite-test.cpp
```

`db.hpp` remains a small module marker rather than an umbrella header.

## 6. Values, fields, and records

### 6.1 `Value`

Add `value.hpp`:

```cpp
namespace jb::db {

struct Null {
    auto operator==(Null const&) const -> bool = default;
};

using Value = std::variant<Null,
                           std::int64_t,
                           double,
                           std::string,
                           jb::core::ByteBuffer>;

[[nodiscard]] auto make_text(std::string_view value) -> Value;
[[nodiscard]] auto make_blob(jb::core::ByteView value) -> Value;

} // namespace jb::db
```

Rules:

- `Null` is SQL null; empty strings and blobs are not null.
- Integer values are signed 64-bit.
- Text and blobs own their bytes and preserve embedded NULs.
- No implicit conversion occurs between alternatives.
- Booleans, UUIDs, timestamps, enums, and JSON are encoded by application repositories.

These five alternatives are supported by SQLite, MariaDB/MySQL, and PostgreSQL without putting a backend-specific type in the public contract.

### 6.2 `Field` and `Record`

Add `record.hpp`:

```cpp
namespace jb::db {

class Field {
public:
    Field(std::string name, Value value);

    [[nodiscard]] auto name() const noexcept -> std::string const&;
    [[nodiscard]] auto value() const noexcept -> Value const&;
    [[nodiscard]] auto is_null() const noexcept -> bool;

private:
    std::string _name;
    Value       _value;
};

class Record {
public:
    Record() = default;
    explicit Record(std::vector<Field> fields);

    [[nodiscard]] auto count() const noexcept -> std::size_t;
    [[nodiscard]] auto is_empty() const noexcept -> bool;
    [[nodiscard]] auto contains(std::string_view name) const noexcept -> bool;
    [[nodiscard]] auto index_of(std::string_view name) const noexcept -> int;

    [[nodiscard]] auto field(std::size_t index) const -> Field const&;
    [[nodiscard]] auto field_name(std::size_t index) const -> std::string const&;
    [[nodiscard]] auto value(std::size_t index) const -> Value const&;
    [[nodiscard]] auto value(std::string_view name) const -> Value const*;
    [[nodiscard]] auto is_null(std::size_t index) const noexcept -> bool;
    [[nodiscard]] auto is_null(std::string_view name) const noexcept -> bool;

private:
    std::vector<Field> _fields;
};

} // namespace jb::db
```

Semantics follow the useful read side of `QSqlRecord`:

- indexed `field()`, `field_name()`, and `value()` throw `std::out_of_range` for an invalid index;
- `index_of()` returns `-1` and uses ASCII case-insensitive name matching;
- duplicate names resolve to the first match;
- named `value()` returns `nullptr` when absent;
- `is_null()` returns true for absent/out-of-range or null fields;
- records are ordinary owning copyable values.

## 7. Driver contracts

### 7.1 `Driver`

Add `driver.hpp`:

```cpp
namespace jb::db {

class DriverQuery;

enum class TransactionMode : std::uint8_t {
    Deferred,
    Immediate,
    Exclusive,
};

class Driver {
public:
    virtual ~Driver() = default;

    Driver(Driver const&)                    = delete;
    Driver(Driver&&)                         = delete;
    auto operator=(Driver const&) -> Driver& = delete;
    auto operator=(Driver&&) -> Driver&      = delete;

    [[nodiscard]] virtual auto name() const noexcept -> std::string_view = 0;

protected:
    Driver() = default;

private:
    friend class Database;
    friend class Query;

    [[nodiscard]] virtual auto open() -> jb::core::Result<void, jb::core::Error> = 0;
    [[nodiscard]] virtual auto close() -> jb::core::Result<void, jb::core::Error> = 0;
    [[nodiscard]] virtual auto is_open() const noexcept -> bool                  = 0;

    [[nodiscard]] virtual auto create_query()
        -> jb::core::Result<std::unique_ptr<DriverQuery>, jb::core::Error> = 0;

    [[nodiscard]] virtual auto begin(TransactionMode mode)
        -> jb::core::Result<void, jb::core::Error> = 0;
    [[nodiscard]] virtual auto commit()
        -> jb::core::Result<void, jb::core::Error> = 0;
    [[nodiscard]] virtual auto rollback()
        -> jb::core::Result<void, jb::core::Error> = 0;
};

} // namespace jb::db
```

The driver is created with backend-specific constructor options before being passed to `Database`. Therefore `Driver::open()` needs no backend-specific parameter object.

`Driver` is an implementation extension point, not an application service. Application code uses it only to choose and construct a backend.

### 7.2 `DriverQuery`

Add `driver_query.hpp`:

```cpp
namespace jb::db {

struct ExecutionInfo {
    bool         produces_records{false};
    std::int64_t rows_affected{-1};
    Record       record_metadata;
};

class DriverQuery {
public:
    virtual ~DriverQuery() = default;

    DriverQuery(DriverQuery const&)                    = delete;
    DriverQuery(DriverQuery&&)                         = delete;
    auto operator=(DriverQuery const&) -> DriverQuery& = delete;
    auto operator=(DriverQuery&&) -> DriverQuery&      = delete;

private:
    friend class Query;

    [[nodiscard]] virtual auto prepare(std::string_view sql)
        -> jb::core::Result<void, jb::core::Error> = 0;
    [[nodiscard]] virtual auto parameter_count() const noexcept -> std::size_t = 0;
    [[nodiscard]] virtual auto parameter_name(std::size_t index) const
        -> std::string_view = 0;
    [[nodiscard]] virtual auto bind(std::size_t index, Value const& value)
        -> jb::core::Result<void, jb::core::Error> = 0;
    [[nodiscard]] virtual auto exec()
        -> jb::core::Result<ExecutionInfo, jb::core::Error> = 0;
    [[nodiscard]] virtual auto next()
        -> jb::core::Result<std::optional<Record>, jb::core::Error> = 0;
    [[nodiscard]] virtual auto finish()
        -> jb::core::Result<void, jb::core::Error> = 0;
    virtual void clear() noexcept = 0;

protected:
    DriverQuery() = default;
};

} // namespace jb::db
```

The generic `Query` owns binding-mode checks, complete-binding enforcement, active/valid state, current record, `last_error()`, and Qt-like behavior. `DriverQuery` performs only backend operations.

Backends return owning `Record` values. No result buffer, native column pointer, or backend lifetime crosses the interface.

### 7.3 Generic errors

The generic layer produces stable errors before entering a driver:

| Code | Category | Meaning |
| --- | --- | --- |
| `db.invalid_database` | `InvalidArgument` | `Database` has no driver |
| `db.database_closed` | `Unavailable` | operation requires an open database |
| `db.database_open` | `Conflict` | operation requires a closed database |
| `db.wrong_thread` | `Internal` | thread-affine object used from another thread |
| `db.query_active` | `Conflict` | close/move attempted while queries exist |
| `db.invalid_query` | `InvalidArgument` | query has no prepared statement for the operation |
| `db.invalid_parameter` | `InvalidArgument` | unknown position or placeholder |
| `db.mixed_binding` | `InvalidArgument` | named and positional binding APIs mixed |
| `db.parameter_unbound` | `InvalidArgument` | execution attempted with an unbound slot |
| `db.transaction_active` | `Conflict` | nested top-level transaction attempted |
| `db.no_transaction` | `Conflict` | completion attempted without a transaction |
| `db.transaction_guard_active` | `Conflict` | direct completion/close attempted while a guard owns the transaction |
| `db.transaction_owner_mismatch` | `Internal` | stale guard token; database is poisoned |
| `db.unsupported_transaction_mode` | `Unsupported` | driver cannot implement the requested mode |
| `db.connection_failed` | `Internal` | database was poisoned by an unrecoverable failure |

The same error returned through `Result` is stored as `last_error()` on the generic object on which the operation occurred. Success clears that object's old error.

## 8. Generic `Database`

Add `database.hpp`:

```cpp
namespace jb::db {

class Query;
class Transaction;

class Database final {
public:
    Database() noexcept;
    explicit Database(std::unique_ptr<Driver> driver);
    ~Database();

    Database(Database const&)                    = delete;
    Database(Database&& other) noexcept;
    auto operator=(Database const&) -> Database& = delete;
    auto operator=(Database&&) -> Database&      = delete;

    [[nodiscard]] auto is_valid() const noexcept -> bool;
    [[nodiscard]] auto is_open() const noexcept -> bool;
    [[nodiscard]] auto driver_name() const noexcept -> std::string_view;
    [[nodiscard]] auto last_error() const -> std::optional<jb::core::Error>;

    [[nodiscard]] auto open() -> jb::core::Result<void, jb::core::Error>;
    auto close() -> jb::core::Result<void, jb::core::Error>;

    [[nodiscard]] auto transaction(TransactionMode mode = TransactionMode::Immediate)
        -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto commit() -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto rollback() -> jb::core::Result<void, jb::core::Error>;

private:
    friend class Query;
    friend class Transaction;
    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::db
```

Important properties:

- `Database` has no `open_sqlite()` method.
- `Database` has no filesystem path, hostname, port, user, password, database-name, SSL, or backend-option setter.
- `database.cpp` includes only generic headers.
- the default constructor creates an invalid handle with no driver;
- `is_valid()` means a driver is installed;
- `driver_name()` returns an empty view for an invalid database;
- `open()` delegates to the installed driver and records thread affinity only after success;
- `open()` on an already open database succeeds without reopening it;
- `close()` delegates to the driver after enforcing generic query/transaction lifetime rules;
- an already closed valid database closes successfully;
- closing successfully clears thread affinity, allowing a later reopen on another thread when no query/guard exists;
- `close()` with any live query returns `db.query_active` and leaves the database open;
- `close()` while an RAII guard owns a transaction returns `db.transaction_guard_active`;
- operations on an invalid database return `db.invalid_database`;
- operational calls from another thread return `db.wrong_thread` before entering the driver;
- move construction is allowed only when there are no live queries or RAII transaction guard; the source becomes invalid;
- move assignment is deleted so replacing an open database cannot hide close/rollback errors.

The generic class stores the last generic/driver error returned by a database operation. A successful operation clears it.

The destructor requires queries and transaction guards to have been destroyed first. It rolls back a directly owned transaction and closes the driver best-effort. Failure is logged as fatal; destructors never throw.

## 9. Generic `Query`

Add `query.hpp` with the Qt-like application API:

```cpp
namespace jb::db {

class Query final {
public:
    explicit Query(Database& database);
    ~Query();

    Query(Query const&)                    = delete;
    Query(Query&& other) noexcept;
    auto operator=(Query const&) -> Query& = delete;
    auto operator=(Query&&) -> Query&      = delete;

    [[nodiscard]] auto prepare(std::string_view sql)
        -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto exec() -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto exec(std::string_view sql)
        -> jb::core::Result<void, jb::core::Error>;

    auto bind_value(std::size_t position, Value value)
        -> jb::core::Result<void, jb::core::Error>;
    auto bind_value(std::string_view placeholder, Value value)
        -> jb::core::Result<void, jb::core::Error>;
    auto add_bind_value(Value value)
        -> jb::core::Result<void, jb::core::Error>;

    [[nodiscard]] auto next() -> jb::core::Result<bool, jb::core::Error>;
    [[nodiscard]] auto is_active() const noexcept -> bool;
    [[nodiscard]] auto is_valid() const noexcept -> bool;
    [[nodiscard]] auto is_select() const noexcept -> bool;

    [[nodiscard]] auto record() const -> Record const&;
    [[nodiscard]] auto value(std::size_t index) const -> Value const&;
    [[nodiscard]] auto value(std::string_view name) const -> Value const*;
    [[nodiscard]] auto is_null(std::size_t index) const noexcept -> bool;
    [[nodiscard]] auto is_null(std::string_view name) const noexcept -> bool;

    [[nodiscard]] auto num_rows_affected() const noexcept -> std::int64_t;
    [[nodiscard]] auto last_query() const noexcept -> std::string_view;
    [[nodiscard]] auto last_error() const -> std::optional<jb::core::Error>;

    auto finish() -> jb::core::Result<void, jb::core::Error>;
    void clear() noexcept;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::db
```

### 9.1 Preparation and binding

`Query` retains a reference to its `Database` and registers its lifetime immediately. It obtains a `DriverQuery` lazily on the first `prepare()`/`exec(sql)` call. This allows construction before `Database::open()`, like Qt SQL, provided the database is open when preparation begins. An invalid or still-closed database returns `db.invalid_database` or `db.database_closed` without calling a driver.

Generic rules:

- `prepare()` accepts one non-empty statement; the backend validates whether trailing text contains another statement.
- positional indexes are zero-based.
- named placeholders include their prefix, for example `:queue_id`.
- `add_bind_value()` binds the next position.
- named and positional binding calls may not be mixed for one prepared query.
- every backend-reported parameter must be explicitly bound, including null.
- an incomplete set returns `db.parameter_unbound` before driver execution.
- bound `Value` objects are owned by generic `Query` and passed to the driver by const reference.
- successful repeated `exec()` retains bindings.
- `finish()` retains prepared SQL and bindings.
- `clear()` discards driver-query state, SQL, bindings, current record, and last error.

Backend placeholder syntax is not parsed in generic `query.cpp`; `DriverQuery::parameter_count()` and `parameter_name()` provide normalized metadata after preparation.

`parameter_count()` reports distinct bind slots. `parameter_name(index)` returns the backend's complete named placeholder, including its prefix, or an empty view for an anonymous positional slot. Repeated occurrences of one named placeholder refer to one slot. This is sufficient for generic named lookup and complete-binding checks without teaching `Query` SQL syntax.

### 9.2 Execution and cursor

After successful `exec()`:

- the query is active and positioned before the first record;
- `is_select()` reflects `ExecutionInfo::produces_records`;
- non-result queries expose `num_rows_affected()`;
- `last_query()` contains SQL with placeholders, never interpolated values.

`next()` maps the driver result to:

- success with `true` and a new current record;
- success with `false` at end-of-results;
- failure for a backend fetch error.

At end, the query stays active but becomes invalid. Record field metadata remains available with null values. `finish()` releases the backend cursor and makes the query inactive.

`record()` is available for an active result query. Indexed `value()` requires a valid row and otherwise throws `std::logic_error`. `is_null()` is a forgiving probe and returns true for inactive/invalid/absent/null.

Only forward navigation is supported in v1.

## 10. Transactions

### 10.1 Direct transaction methods

`Database::transaction()`, `commit()`, and `rollback()` maintain generic transaction state and delegate native work to the driver.

- one top-level transaction is allowed;
- nested begin returns `db.transaction_active`;
- commit/rollback without an active transaction returns `db.no_transaction`;
- failed commit leaves the transaction active so rollback remains possible;
- failed rollback poisons the database;
- close/destruction rolls back a directly owned active transaction best-effort;
- active query cursors are not silently finished.

Drivers map `TransactionMode` to backend semantics. For SQLite these are deferred, immediate, and exclusive. A future driver may return `db.unsupported_transaction_mode` for a mode it cannot implement.

### 10.2 RAII transaction guard

Add `transaction.hpp`:

```cpp
namespace jb::db {

class Transaction final {
public:
    Transaction() noexcept;
    ~Transaction();

    Transaction(Transaction const&)                    = delete;
    Transaction(Transaction&& other) noexcept;
    auto operator=(Transaction const&) -> Transaction& = delete;
    auto operator=(Transaction&&) -> Transaction&      = delete;

    [[nodiscard]] static auto begin(Database& database,
                                    TransactionMode mode = TransactionMode::Immediate)
        -> jb::core::Result<Transaction, jb::core::Error>;

    [[nodiscard]] auto is_active() const noexcept -> bool;
    [[nodiscard]] auto commit() -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto rollback() -> jb::core::Result<void, jb::core::Error>;

private:
    Transaction(Database& database, std::uint64_t token) noexcept;

    Database*     _database{nullptr};
    std::uint64_t _token{0};
};

} // namespace jb::db
```

The generic database assigns a nonzero ownership token to guarded transactions.

- guard destruction rolls back if active;
- successful commit/rollback deactivates it;
- failed commit leaves it active;
- failed rollback deactivates it and poisons the database;
- move construction transfers ownership and clears the source;
- move assignment is deleted;
- direct database completion or close while a guard owns the transaction returns `db.transaction_guard_active`;
- a stale/mismatched token returns `db.transaction_owner_mismatch` and poisons the database;
- there is no `release()` in v1;
- the guard must be declared before queries in its scope so queries are destroyed first on early return.

This entire ownership mechanism is generic and contains no backend code.

## 11. SQLite driver

### 11.1 Public backend selection

Add `sqlite/sqlite_driver.hpp`:

```cpp
namespace jb::db::sqlite {

enum class Durability : std::uint8_t {
    Normal,
    Full,
};

struct Options {
    std::filesystem::path     database_file;
    std::chrono::milliseconds busy_timeout{1000};
    Durability                durability{Durability::Normal};
};

class Driver final : public jb::db::Driver {
public:
    explicit Driver(Options options);
    ~Driver() override;

    [[nodiscard]] auto name() const noexcept -> std::string_view override;

private:
    struct Private;
    std::unique_ptr<Private> _data;

    [[nodiscard]] auto open() -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto close() -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto is_open() const noexcept -> bool override;
    [[nodiscard]] auto create_query()
        -> jb::core::Result<std::unique_ptr<jb::db::DriverQuery>, jb::core::Error> override;
    [[nodiscard]] auto begin(jb::db::TransactionMode mode)
        -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto commit() -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto rollback() -> jb::core::Result<void, jb::core::Error> override;
};

} // namespace jb::db::sqlite
```

Application composition is explicit but does not add SQLite methods to `Database`:

```cpp
auto driver = std::make_unique<jb::db::sqlite::Driver>(
    jb::db::sqlite::Options{
        .database_file = path,
    });

jb::db::Database database{std::move(driver)};
if (auto opened = database.open(); !opened) {
    return jb::core::Result<void, jb::core::Error>::failure(opened.error());
}
```

MariaDB or PostgreSQL will later provide their own driver classes and option structures while using this exact generic `Database` call sequence.

### 11.2 SQLite responsibilities

`sqlite::Driver::open()`:

1. Validates `Options`.
2. Requires a non-empty file-backed path; URI and `:memory:` forms are outside the production driver contract.
3. Requires the parent directory to exist.
4. Acquires a non-blocking exclusive adjacent `<database-file>.lock` process lock.
5. Opens SQLite read/write/create with thread-affine private-connection flags.
6. Enables extended result codes.
7. Sets the bounded busy timeout, accepted range zero through five seconds.
8. Enables and verifies foreign keys.
9. enables and verifies WAL mode.
10. Sets and verifies `synchronous=NORMAL` or `FULL`.

The adjacent lock belongs here because it is a property of JobU's file-backed SQLite deployment, not of generic databases.

The SQLite driver retains its path and lock descriptor privately. Neither is exposed by generic `Database`. Backend-specific diagnostics may mention the path in user-safe messages, but generic application logic must not depend on it.

`sqlite::Driver::create_query()` creates an SQLite-specific `DriverQuery` implementation owning `sqlite3_stmt*`. All statement preparation, binding, stepping, row extraction, and finalization remain in `src/db/sqlite`.

No SQLite include appears above the SQLite-driver source/private-header boundary.

### 11.3 SQLite errors

The driver maps SQLite primary and extended codes into stable generic database codes. Required codes include:

| Code | Category |
| --- | --- |
| `db.sqlite.already_in_use` | `Conflict` |
| `db.sqlite.open_failed` | `Io` |
| `db.permission_denied` | `PermissionDenied` |
| `db.busy` | `Unavailable` |
| `db.locked` | `Unavailable` |
| `db.constraint` | `Conflict` |
| `db.constraint.unique` | `Conflict` |
| `db.constraint.foreign_key` | `Conflict` |
| `db.corrupt` | `Internal` |
| `db.io` | `Io` |
| `db.out_of_memory` | `ResourceExhausted` |
| `db.prepare_failed` | mapped from cause |
| `db.exec_failed` | mapped from cause |
| `db.fetch_failed` | mapped from cause |

`Error::detail` may contain SQLite primary/extended numbers and its diagnostic message. SQL text and bound values are not copied into errors.

Generic errors such as `db.invalid_database`, `db.wrong_thread`, `db.parameter_unbound`, `db.transaction_active`, and transaction-guard misuse are produced by the generic layer.

## 12. Testing

### 12.1 Fake driver

Add a deterministic fake driver under `test/support`. It implements both driver contracts without SQLite and allows tests to inject:

- open/close failures;
- prepared parameter metadata;
- execution metadata;
- records and end-of-results;
- fetch failures;
- transaction success/failure;
- call counters and ordering.

The fake proves that generic database/query/transaction behavior is independent of SQLite.

### 12.2 Generic API tests

`db-api-test.cpp` and `db-query-test.cpp` cover:

- invalid/default database behavior;
- driver ownership, open, close, and name delegation;
- thread-affinity rejection before driver calls;
- all five values and record lookup/null behavior;
- prepare/direct exec/repeated exec;
- positional, named, sequential, mixed, missing, and incomplete bindings;
- active/valid/select states;
- rows, end-of-results, and fetch errors;
- field metadata before the first and after the last row;
- finish versus clear;
- query/database lifetime enforcement;
- last-error storage and clearing.

### 12.3 Transaction tests

Using the fake driver, `db-transaction-test.cpp` covers:

- direct begin, commit, rollback, and nesting rejection;
- failed commit followed by rollback;
- poisoning after rollback failure;
- automatic rollback on database close/destruction;
- RAII rollback on early scope exit;
- guarded commit and rollback;
- guard move construction;
- direct completion/close rejection while guarded;
- token mismatch handling;
- active-query behavior during transaction completion.

### 12.4 SQLite integration tests

`db-sqlite-test.cpp` covers only driver/backend behavior:

- file creation and adjacent locking;
- second-open rejection and later reopen;
- option validation;
- WAL, foreign keys, busy timeout, and durability;
- prepared positional and named binding;
- null, integer, floating, text, and blob round trips;
- embedded NULs and arbitrary bytes;
- affected rows and forward cursor behavior;
- unique and foreign-key error mapping;
- commit, rollback, and destructor cleanup;
- corrupt/not-a-database handling;
- cleanup of database, WAL, shared-memory, and lock files inside `TemporaryDirectory`.

SQLite integration tests link `db-sqlite`; generic tests link only `db`.

## 13. Public-boundary rules

Extend the generic public-header test with:

```cpp
#include "database.hpp"
#include "driver.hpp"
#include "driver_query.hpp"
#include "query.hpp"
#include "record.hpp"
#include "transaction.hpp"
#include "value.hpp"
```

Add a separate SQLite-driver public-header test:

```cpp
#include "sqlite/sqlite_driver.hpp"
```

Review invariants:

- `database.cpp`, `query.cpp`, `transaction.cpp`, `record.cpp`, and `value.cpp` contain no SQLite references.
- Generic public headers contain no SQLite, POSIX, filesystem-path database identity, network-database option, or JSON type except that the explicitly SQLite-specific options may use `std::filesystem::path`.
- Only `src/db/sqlite` includes SQLite or platform file-lock headers.
- Only `db-sqlite` links SQLite.
- Generic tests pass using the fake driver when SQLite is unavailable.
- The SQLite driver can be removed from the build without changing generic sources.
- No migration or JobU schema SQL appears anywhere in Phase 1.

## 14. Reviewable implementation sequence

Each stage is a separate approval and review boundary.

### Stage 1.1: Values, records, and driver contracts

- add `Value`, `Field`, and `Record`;
- add `Driver` and `DriverQuery` interfaces;
- add fake-driver scaffolding;
- add value/record and public-boundary tests.

Exit: the generic data and backend-extension contracts compile with no SQLite dependency.

### Stage 1.2: Generic database and query

- implement `Database` using an owned `Driver`;
- implement Qt-like `Query` using `DriverQuery`;
- implement generic thread affinity, binding enforcement, state, and errors;
- test entirely with the fake driver.

Exit: the complete non-transactional API works without SQLite.

### Stage 1.3: Generic transactions

- implement direct database transactions;
- implement RAII `Transaction` and ownership tokens;
- implement poisoned-state handling;
- test entirely with the fake driver.

Exit: transaction semantics are complete and backend-independent.

### Stage 1.4: SQLite driver

- create the separate `db-sqlite` target;
- implement SQLite driver and driver-query classes only under `src/db/sqlite`;
- implement SQLite configuration, adjacent locking, and error translation;
- add SQLite integration tests.

Exit: SQLite works through the already-tested generic API. No generic source file changes should be needed except corrections to a driver contract proven insufficient during implementation; such a change requires stopping for revised approval.

## 15. Validation

Run after each stage:

```sh
cmake -B .bld -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld
ctest --test-dir .bld/test --output-on-failure
```

Additional checks:

- after Stages 1.1-1.3, configure and test the generic `db` target with SQLite discovery disabled or unavailable;
- after Stage 1.4, test GCC and Clang on Linux and Apple Clang on macOS;
- inspect target link interfaces to confirm SQLite belongs only to `db-sqlite`;
- search generic source directories for `sqlite`, `SQLITE_`, `sqlite3`, PRAGMA strings, and platform lock APIs;
- format changed C++ files with the checked-in `.clang-format`;
- ensure tests use unique temporary paths and no real sleeps.

## 16. Later application-level database phase

A separate later phase will define:

- JobU schema SQL and version storage;
- application-level forward migrations;
- startup ordering around schema checks and recovery;
- newer-schema refusal;
- any backup policy actually required by a destructive JobU migration;
- queue/job/run/attempt/secret/idempotency repositories.

That work will consume the Phase 1 API. It will not add migration policy to `jb::db` unless repeated application experience demonstrates a reusable need.

## 17. Qt SQL reference points

The API shape follows these Qt 6 concepts:

- [QSqlDatabase](https://doc.qt.io/qt-6/qsqldatabase.html);
- [QSqlDriver](https://doc.qt.io/qt-6/qsqldriver.html);
- [QSqlQuery](https://doc.qt.io/qt-6/qsqlquery.html);
- [QSqlResult](https://doc.qt.io/qt-6/qsqlresult.html);
- [QSqlRecord](https://doc.qt.io/qt-6/qsqlrecord.html).

These are design references only. JobU does not link Qt and does not promise source compatibility with Qt SQL.
