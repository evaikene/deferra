# JobU Phase 3 Code-Level Design

## 1. Status and purpose

This document defines Phase 3 against GitHub main at commit
75423800061ec7cc0b901171b8334f2aca2ab0cd. Phase 2 is treated as implemented
and fixed input: JobU has the generic database API and isolated SQLite driver,
project-owned JSON and JSON-RPC, local IPC on Linux and macOS, and a working
foreground jobud/jobuctl system.info round trip.

Phase 3 adds the durable application model and management control plane. It
does not execute external work. The phase is intentionally divided into small
approval boundaries: Codex implements and verifies one stage, reports the
result, and waits before starting the next stage.

Linux is the implementation platform for Stages 3.1 through 3.17. The macOS
stages are optional follow-up validation stages and may be skipped without
blocking Linux Phase 3 completion. The final stage is a clean Linux
verification and documentation audit.

Phase 3 is based on the overall v1 technical plan .codex/jobu-v1-technical-plan.md

## 2. Scope and exit criteria

Phase 3 includes:

- application-owned schema creation and version validation;
- a separate SQLite schema target with no SQLite leakage into the generic
  jobu or db targets;
- domain values for queues, job definitions, runs, attempts, and secret
  metadata;
- the built-in attribute registry, validation, persistence codec, and
  creation-time default materialization;
- private repositories for queues, jobs, runs, attempts, secrets,
  idempotency records, and bounded retention operations;
- a transaction-owning ManagementService for queue and job management;
- queue create/get/list/update/suspend/resume/delete;
- one-time job create/get/list/update/suspend/resume/move/delete;
- creation of the one schedule-owned run belonging to a one-time job;
- optimistic revision checks and pending-run snapshot refresh;
- soft deletion and cancellation of non-running work;
- optional idempotency keys for durable creates;
- typed JSON adapters and JSON-RPC handlers for the implemented methods;
- initial jobuctl queue and one-time-job commands;
- jobud startup with an explicit SQLite database path;
- deterministic repository, service, RPC, and Linux executable tests.

Linux exit criteria:

1. jobud creates or validates a Phase 3 SQLite database before opening its RPC
   socket.
2. jobuctl can exercise a durable queue lifecycle and a durable one-time job
   lifecycle through the foreground daemon.
3. Restarting jobud against the same database returns the same resources,
   revisions, lifecycle states, and schedule-owned run.
4. No external command or HTTP request is started.
5. Every mutation that spans several tables is atomic.
6. Every new public header is self-contained and every public declaration has
   Doxygen documentation.
7. The complete Linux test suite passes with both GCC and Clang.

## 3. Explicitly deferred

The following work remains outside Phase 3:

- cron parsing, timezone lookup, DST handling, and recurring-job creation;
- scheduler eligibility, timers, queue arbitration, capacity, fairness, and
  dispatch;
- Run Now and manual-run barriers;
- retry calculation and attempt execution state machines;
- HTTP and CLI runners;
- startup recovery of interrupted work;
- fail-closed scheduler shutdown on a persistence failure;
- attempt output capture and retrieval RPC;
- secret RPC/CLI commands and runtime secret resolution;
- run/attempt history RPC and opaque history cursors;
- statistics, automatic retention timers, configuration files, privilege
  changes, service packaging, and remote transports;
- a reusable generic migration framework or generic schema abstraction in
  jb::db;
- a production MariaDB/MySQL or PostgreSQL schema.

The schema includes the run, attempt, output, secret, idempotency, and
retention structures required by later phases. Phase 3 tests their persistence
primitives, but it does not expose behavior whose invariants depend on a
scheduler or runner.

## 4. Phase-boundary decisions

### 4.1 Application-level schema management

Phase 1 deliberately excluded migrations from jb::db. Phase 3 therefore owns a
small JobU-specific schema component. It has one current schema version and
one create path. It is not a callback-based migration framework.

Future schema changes append explicit application functions such as
apply_v1_to_v2(). Phase 3 does not implement backups, downgrade, migration
discovery, or driver-independent DDL.

### 4.2 SQLite remains isolated

The generic Database class continues to know only Driver. SQLite file paths,
DDL details, partial indexes, sqlite_schema inspection, and schema-version
logic live under src/jobu/sqlite and in the jobu-sqlite target.

The repositories use jb::db::Database and jb::db::Query only. Their DML uses a
conservative named-parameter SQL subset. If a future backend needs different
DML, its implementation can replace private repository classes without
changing domain or ManagementService APIs.

### 4.3 One-time jobs only at the Phase 3 API boundary

The public domain model reserves both OnceSchedule and CronSchedule, and the
database has columns for both. Phase 3 accepts only OnceSchedule in create and
update operations.

A cron request returns the stable unsupported error
jobu.schedule.cron_unavailable. This is preferable to storing unchecked cron
text or temporarily violating the invariant that every recurring definition
has exactly one schedule-owned non-terminal run. Phase 4 adds cron validation
and recurring creation without changing the stored representation.

### 4.4 One-time creation includes its run

Creating a one-time JobDefinition and its schedule-owned JobRun is one
transaction. The run starts in scheduled state and contains the definition
revision, queue, type, priority, fully materialized attributes, and runner
payload snapshot.

Phase 3 never moves that run to running. Phase 4 can begin selecting existing
scheduled rows without backfilling or repairing Phase 3 jobs.

### 4.5 Public API stays smaller than repository internals

Repositories are implementation details in *_priv.hpp files. The supported
public application boundary consists of documented domain values,
StandardAttributeRegistry, attribute materialization, ManagementService, JSON
adapters needed by jobuctl, and the SQLite schema entry point.

This avoids committing the scheduler and future backends to CRUD-shaped public
repository interfaces.

## 5. Architecture and dependency boundaries

The final direct target dependencies are:

| Target | Direct project dependencies |
| --- | --- |
| core | none |
| db | core |
| db-sqlite | db |
| net | core |
| rpc | core |
| jobu | core, net, db, rpc |
| jobu-sqlite | jobu, db-sqlite |
| jobud | jobu-sqlite |
| jobuctl | jobu |

Rules:

- retain jobu's existing public links to core, net, db, and rpc; Phase 3 adds
  no new use of OS-specific net types and does not perform an unrelated link
  interface refactor;
- jobu has no SQLite include, native handle, result code, PRAGMA, or file-path
  option;
- jobu-sqlite contains application DDL and links jobu plus db-sqlite;
- jobud explicitly constructs db::sqlite::Driver and calls the jobu-sqlite
  schema entry point;
- ManagementService borrows one already-open Database and uses it only on its
  owning event-loop thread;
- JSON-RPC handlers call ManagementService synchronously on that same thread;
- handlers do not expose SQL, database error detail, internal deletion names,
  or secret values;
- jobuctl depends on public domain/JSON adapters, not repository headers;
- nlohmann/json remains private to rpc;
- no public header exposes sqlite3, filesystem paths for generic database
  names, or backend-specific option types.

## 6. Targets and planned source layout

### 6.1 CMake targets

Keep the current targets. Add jobu-sqlite only when db-sqlite exists:

~~~cmake
if(TARGET db-sqlite)
    add_subdirectory(sqlite)
endif()
~~~

The new target has this link interface:

~~~cmake
target_link_libraries(jobu-sqlite
    PUBLIC
        jobu
        db-sqlite
)
~~~

jobud links jobu-sqlite privately when SQLite support is enabled. A build with
JB_BUILD_SQLITE_DRIVER=OFF must still build core, db, rpc, net, jobu, and
jobuctl plus all backend-independent tests. In that configuration CMake does
not create the jobud target or database-backed executable tests; the README
must state that the Phase 3 daemon requires the SQLite option. Do not add
conditional SQLite branches to jobud/main.cpp or a fake in-memory production
database merely to keep that target present.

### 6.2 Planned files

Private file boundaries may be refined, but public types and target boundaries
must not change without stopping for approval.

~~~text
src/jobu/
  CMakeLists.txt
  attribute.cpp                         # existing validation, extended
  attribute.hpp
  attribute_codec_priv.cpp
  attribute_codec_priv.hpp
  attribute_registry.cpp
  attribute_registry.hpp
  attempt.cpp
  attempt.hpp
  domain_storage_priv.cpp
  domain_storage_priv.hpp
  idempotency_repository_priv.cpp
  idempotency_repository_priv.hpp
  job.cpp
  job.hpp
  job_repository_priv.cpp
  job_repository_priv.hpp
  management.cpp
  management.hpp
  management_json.cpp
  management_json.hpp
  management_rpc.cpp
  management_rpc_priv.hpp
  queue.cpp
  queue.hpp
  queue_repository_priv.cpp
  queue_repository_priv.hpp
  retention_repository_priv.cpp
  retention_repository_priv.hpp
  run.cpp
  run.hpp
  run_repository_priv.cpp
  run_repository_priv.hpp
  secret.cpp
  secret.hpp
  secret_repository_priv.cpp
  secret_repository_priv.hpp
  utc_timestamp.cpp
  utc_timestamp.hpp

src/jobu/sqlite/
  CMakeLists.txt
  sqlite_schema.cpp
  sqlite_schema.hpp
  sqlite_schema_priv.hpp

src/jobud/
  main.cpp

src/jobuctl/
  main.cpp

test/
  support/
    sequence_uuid_generator.hpp
  attribute-materialization-test.cpp
  domain-storage-test.cpp
  jobu-sqlite-schema-test.cpp
  queue-management-test.cpp
  run-attempt-repository-test.cpp
  job-management-test.cpp
  management-lifecycle-test.cpp
  idempotency-secret-retention-test.cpp
  management-json-test.cpp
  management-rpc-test.cpp
  jobuctl-queue-management-test.cpp
  jobuctl-job-management-test.cpp
  jobu-management-public-headers-test.cpp
~~~

Repository tests may include private headers. Production code outside the jobu
target must not.

## 7. Mandatory public documentation and selective nodiscard

### 7.1 Doxygen gate

Every stage adding or changing a public header must complete documentation in
that same stage. A public header is incomplete unless it has:

- an @file block describing its boundary;
- a class/struct/enum comment for every public type;
- a comment for every enumerator and public data member;
- @param entries for every named parameter;
- @return text for every non-void operation;
- ownership, lifetime, thread-affinity, transaction, and error semantics where
  applicable;
- a note when a Phase 3 public form is reserved but not yet accepted, such as
  CronSchedule.

Each new header is compiled as the first include in a dedicated translation
unit. The aggregate public-header test is updated as well. Reviewing Doxygen
coverage is an explicit exit condition of every stage, not cleanup for the
final stage.

### 7.2 nodiscard policy

Continue the selective Phase 1/2 policy:

- use nodiscard for Result-returning operations whose runtime outcome must be
  handled, resource lookups, parsers, codecs, and getters;
- use nodiscard for ManagementService mutations because an ignored
  persistence or conflict result is a correctness bug;
- do not add nodiscard merely because a method returns bool for a
  normally-successful configuration or cleanup action;
- do not alter the existing Query binding/finish policy;
- do not cast away a service or transaction result in production code.

## 8. Domain values and canonical representation

### 8.1 Common storage rules

Use the following representations consistently:

| Value | C++ representation | Database representation | JSON/RPC representation |
| --- | --- | --- | --- |
| resource ID | jb::core::Uuid | exactly 16 BLOB bytes | canonical lower-case UUID text |
| revision | std::uint64_t | non-negative signed 64-bit INTEGER | unsigned integer |
| UTC time | jb::core::UtcTimePoint | signed Unix microseconds | RFC 3339 UTC text |
| duration | chrono duration | signed integer in documented unit | integer milliseconds unless field says otherwise |
| enum | enum class | stable lower-case snake-case TEXT | same stable text |
| boolean | bool | INTEGER 0 or 1 | JSON boolean |
| JSON document | jb::rpc::JsonValue | deterministic UTF-8 TEXT | JSON object/array |

Database adapters reject:

- UUID blobs whose length is not 16;
- negative or overflowing revisions and attempt numbers;
- invalid enum text;
- booleans other than 0 or 1;
- timestamps outside the UtcClock range supported by the adapters;
- malformed, non-object, or oversized stored JSON where an object is required.

Invalid persisted data returns an Internal error with a stable
jobu.storage.invalid_* code. The message is safe for logs/RPC; the underlying
column/value detail stays in Error::detail and is never transmitted.

### 8.2 UTC timestamp API

Add utc_timestamp.hpp:

~~~cpp
namespace jb::jobu {

[[nodiscard]] auto format_utc_timestamp(jb::core::UtcTimePoint value)
    -> jb::core::Result<std::string, jb::core::Error>;

[[nodiscard]] auto parse_utc_timestamp(std::string_view value)
    -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error>;

} // namespace jb::jobu
~~~

The canonical output is YYYY-MM-DDTHH:MM:SS.ffffffZ. Parsing accepts one
through six fractional digits or no fraction, requires Z, rejects timezone
offsets, leap seconds, impossible dates, trailing data, and values that cannot
be represented as signed Unix microseconds. Formatting and parsing are
implemented with portable civil-calendar arithmetic, not localtime, gmtime,
the process timezone, or C++20 timezone databases.

### 8.3 Queue domain values

queue.hpp defines:

~~~cpp
enum class QueueState : std::uint8_t {
    Active,
    Suspending,
    Suspended,
    Deleted,
};

enum class RecoveryPolicy : std::uint8_t {
    FailInterrupted,
    RetryInterrupted,
};

struct Queue {
    jb::core::Uuid                           id;
    std::string                              name;
    QueueState                               state{QueueState::Active};
    std::uint32_t                            weight{1};
    std::uint32_t                            concurrency_limit{1};
    RecoveryPolicy                           recovery_policy{RecoveryPolicy::FailInterrupted};
    AttributeSet                             defaults;
    std::optional<std::chrono::seconds>      history_retention;
    std::chrono::milliseconds                runnable_wait_warning{10000};
    jb::core::UtcTimePoint                   created_at;
    jb::core::UtcTimePoint                   updated_at;
    std::optional<jb::core::UtcTimePoint>    deleted_at;
};
~~~

A missing history_retention inherits the daemon value. Zero means unlimited;
positive values are retention durations. Negative values are invalid. Queue
names are UTF-8, 1 through 128 bytes, have no ASCII control characters, and
may not contain the reserved deletion suffix -deleted# followed by a UUID.

The Queue returned for a deleted row exposes its original user name. Its
internally rewritten unique name is storage-only.

### 8.4 Job and schedule domain values

job.hpp defines:

~~~cpp
using JobRevision = std::uint64_t;

enum class JobState : std::uint8_t {
    Active,
    Suspending,
    Suspended,
    Deleted,
};

enum class JobType : std::uint8_t {
    Cli,
    Http,
};

struct OnceSchedule {
    jb::core::UtcTimePoint planned_at;
};

struct CronSchedule {
    std::string expression;
    std::string timezone{"UTC"};
};

using JobSchedule = std::variant<OnceSchedule, CronSchedule>;

struct JobDefinition {
    jb::core::Uuid                        id;
    jb::core::Uuid                        queue_id;
    JobRevision                           revision{1};
    std::optional<std::string>            name;
    JobState                              state{JobState::Active};
    JobType                               type{JobType::Cli};
    JobSchedule                           schedule;
    std::int32_t                          priority{0};
    AttributeSet                          attributes;
    jb::rpc::JsonValue                    payload;
    jb::core::UtcTimePoint                created_at;
    jb::core::UtcTimePoint                updated_at;
    std::optional<jb::core::UtcTimePoint> deleted_at;
};
~~~

Job names are optional, non-unique UTF-8 strings up to 256 bytes without ASCII
control characters. Payload must be an object whose deterministic encoding is
at most 256 KiB.

Phase 3 applies only structural runner-payload validation:

- CLI requires a non-empty command string and, if present, an array of string
  arguments;
- HTTP requires a non-empty url string and, if present, a non-empty method
  string;
- unknown payload members are preserved for additive runner fields;
- phase-specific runner validation is added in Phases 5 and 6 before work can
  execute.

### 8.5 Run, attempt, and secret values

run.hpp defines RunOrigin with scheduled, manual, and submitted values and
RunState with scheduled, running, retry_wait, succeeded, failed, interrupted,
and cancelled values.

JobRun contains:

- run ID, job ID, job revision, and execution queue ID;
- origin and a schedule_owned flag;
- planned, runnable, started, and completed UTC times as applicable;
- snapshotted job type, priority, complete attributes, and payload;
- state and an optional terminal result object.

Phase 3 creates only origin=scheduled, schedule_owned=true, state=scheduled
runs. The broader representation is required so repository and schema
contracts do not change when Phase 4 begins transitions.

attempt.hpp defines JobAttempt with run ID, positive attempt number, due time,
optional start/completion times, state/outcome text represented by documented
enums, and an optional result object. Attempts use the composite identity
(run_id, attempt_number). Phase 3 repository tests round-trip fixtures but
production management operations create no attempts.

secret.hpp exposes only:

~~~cpp
struct SecretMetadata {
    std::string               name;
    jb::core::UtcTimePoint    created_at;
    jb::core::UtcTimePoint    updated_at;
};
~~~

Secret bytes remain private repository input/output and never appear in a
public read DTO, log message, Error, RPC result, or jobuctl output.

## 9. Attribute registry and materialization

### 9.1 StandardAttributeRegistry

Add a final StandardAttributeRegistry implementing the existing
AttributeRegistry interface. It owns fixed, process-lifetime definitions and
adds cross-field validation through a documented validate_materialized()
method.

Phase 3 built-ins are:

| Name | Type | Default | Constraints |
| --- | --- | --- | --- |
| job.timeout | Duration | 120 s | 1 ms through 30 days |
| retry.max_attempts | Integer | 1 | 1 through 100 |
| retry.strategy | String | fixed | fixed or exponential |
| retry.initial_delay | Duration | 0 | 0 through 24 hours |
| retry.max_delay | Duration | 24 hours | 0 through 30 days and not below initial_delay |
| retry.mode | String | reschedule | blocking or reschedule |
| output.capture | String | on_error | none, on_error, or always |
| output.stdout_limit | Integer | 1 MiB | 0 through 64 MiB |
| output.stderr_limit | Integer | 1 MiB | 0 through 64 MiB |

All definitions support DaemonDefault, QueueDefault, and Job scopes. Runner
specific attributes are added in the runner phases. A persisted older set that
lacks a newly introduced definition receives only that definition's built-in
default; it never re-applies current daemon or queue defaults.

### 9.2 Materialization API

attribute_registry.hpp defines:

~~~cpp
class StandardAttributeRegistry final : public AttributeRegistry {
public:
    StandardAttributeRegistry();

    [[nodiscard]] auto find(std::string_view name) const noexcept
        -> AttributeDefinition const* override;
    [[nodiscard]] auto validate(
        std::string_view name,
        AttributeValue const& value,
        AttributeScope scope) const
        -> jb::core::Result<void, jb::core::Error> override;
    [[nodiscard]] auto definitions() const
        -> std::span<AttributeDefinition const> override;
    [[nodiscard]] auto validate_materialized(AttributeSet const& values) const
        -> jb::core::Result<void, jb::core::Error>;
};

[[nodiscard]] auto materialize_attributes(
    AttributeRegistry const& registry,
    AttributeSet const& daemon_defaults,
    AttributeSet const& queue_defaults,
    AttributeSet const& job_values)
    -> jb::core::Result<AttributeSet, jb::core::Error>;

} // namespace jb::jobu
~~~

Materialization:

1. validates every supplied layer against its own scope;
2. starts with every built-in default applicable to Job;
3. overlays daemon defaults;
4. overlays queue defaults;
5. overlays supplied job values;
6. validates the complete set, including cross-field rules;
7. returns a complete lexicographically ordered AttributeSet.

Unknown names, wrong alternatives, invalid enum strings, invalid ranges, and
cross-field violations fail before a transaction writes anything.

### 9.3 Persisted attribute codec

The private persistence codec stores a versioned typed JSON document:

~~~json
{
  "version": 1,
  "values": {
    "job.timeout": {"type": "duration_ns", "value": 120000000000},
    "retry.max_attempts": {"type": "integer", "value": 1}
  }
}
~~~

Every AttributeValue alternative has an explicit type tag, including nested
list and map entries, so Duration, integer, string, and ByteBuffer cannot be
confused. Durations use signed nanoseconds. Bytes use lower-case hexadecimal.
The decoder rejects unknown tags, duplicate names, invalid hex, overflow,
excessive nesting, unknown attributes, and values that fail the registry.

The RPC form is friendlier and definition-directed:

- Boolean, Integer, Number, String, List, and Map use natural JSON;
- Duration uses an integer number of milliseconds;
- Bytes use lower-case hexadecimal;
- Phase 3 rejects nested Duration or Bytes in List/Map input because current
  definitions do not require them and no element schema exists yet.

Persisted encoding and RPC encoding are separate functions and tests.

## 10. SQLite application schema

### 10.1 Public entry point

src/jobu/sqlite/sqlite_schema.hpp defines:

~~~cpp
namespace jb::jobu::sqlite {

inline constexpr std::uint32_t current_schema_version{1};

struct SchemaStatus {
    std::uint32_t version{0};
    bool          created{false};
};

[[nodiscard]] auto ensure_schema(jb::db::Database& database)
    -> jb::core::Result<SchemaStatus, jb::core::Error>;

} // namespace jb::jobu::sqlite
~~~

The header documents that Database must be open, idle, owned by the calling
thread, and backed by the SQLite driver. ensure_schema() begins one immediate
RAII transaction and either:

- creates all version-1 objects and the version row;
- validates an existing version-1 schema;
- rejects a newer version with jobu.schema.newer_database;
- rejects a missing, duplicate, zero, negative, or malformed version row with
  jobu.schema.invalid;
- rolls back every partial change on failure.

If jobu_schema is absent, creation proceeds only when sqlite_schema contains
no user-defined table, index, trigger, or view. A non-empty unmarked database
returns jobu.schema.database_not_empty rather than being silently claimed as a
JobU database.

Repeated calls are idempotent. Version validation occurs before the RPC socket
is opened.

### 10.2 Schema objects

Use the jobu_ prefix for every application object. Stable enum CHECK
constraints and 16-byte UUID CHECK constraints belong in SQLite DDL.

jobu_schema:

| Column | Contract |
| --- | --- |
| singleton | INTEGER primary key, exactly 1 |
| version | INTEGER, exactly current_schema_version |

jobu_queues:

| Column | Contract |
| --- | --- |
| id | 16-byte BLOB primary key |
| name | current internal unique name |
| deleted_name | original user name after soft deletion, otherwise NULL |
| state | active, suspending, suspended, or deleted |
| weight | positive INTEGER |
| concurrency_limit | positive INTEGER |
| recovery_policy | fail_interrupted or retry_interrupted |
| defaults_json | versioned attribute document |
| retention_seconds | NULL inherit, 0 unlimited, positive duration |
| runnable_wait_warning_ms | non-negative INTEGER |
| created_at_us, updated_at_us | signed Unix microseconds |
| deleted_at_us | NULL until deletion |

jobu_jobs:

| Column | Contract |
| --- | --- |
| id | 16-byte BLOB primary key |
| queue_id | references jobu_queues(id), restricted |
| revision | positive INTEGER |
| name | nullable display name |
| state | active, suspending, suspended, or deleted |
| type | cli or http |
| schedule_kind | once or cron |
| scheduled_at_us | required only for once |
| cron_expression, cron_timezone | required only for cron |
| priority | signed 32-bit INTEGER |
| attributes_json | complete versioned attribute document |
| payload_json | deterministic JSON object |
| created_at_us, updated_at_us | signed Unix microseconds |
| deleted_at_us | NULL until deletion |

jobu_runs:

| Column | Contract |
| --- | --- |
| id | 16-byte BLOB primary key |
| job_id | references jobu_jobs(id), restricted |
| job_revision | positive INTEGER snapshot |
| queue_id | references jobu_queues(id), restricted |
| origin | scheduled, manual, or submitted |
| schedule_owned | INTEGER boolean |
| planned_at_us, runnable_at_us | signed Unix microseconds |
| started_at_us, completed_at_us | nullable signed Unix microseconds |
| type, priority | execution snapshot |
| attributes_json, payload_json | immutable execution snapshot |
| state | scheduled, running, retry_wait, succeeded, failed, interrupted, cancelled |
| result_json | nullable terminal summary object |

jobu_attempts:

| Column | Contract |
| --- | --- |
| run_id, attempt_number | composite primary key; run cascades on physical purge |
| due_at_us | signed Unix microseconds |
| started_at_us, completed_at_us | nullable signed Unix microseconds |
| state | pending, running, or completed |
| outcome | nullable stable result classification |
| result_json | nullable detailed result object |

jobu_attempt_output:

| Column | Contract |
| --- | --- |
| run_id, attempt_number | primary/foreign key to jobu_attempts, cascading |
| stdout_blob, stderr_blob | nullable byte-preserving output |
| stdout_truncated, stderr_truncated | INTEGER booleans |
| capture_lost | INTEGER boolean |

jobu_secrets:

| Column | Contract |
| --- | --- |
| name | TEXT primary key |
| value_blob | plaintext bytes protected by database filesystem permissions |
| created_at_us, updated_at_us | signed Unix microseconds |

jobu_secret_refs:

| Column | Contract |
| --- | --- |
| secret_name | references jobu_secrets(name), restricted |
| job_id | references jobu_jobs(id), cascading on physical purge |
| field_path | stable payload/attribute reference path |

jobu_idempotency:

| Column | Contract |
| --- | --- |
| method | stable RPC/service operation name |
| scope_id | 16-byte UUID; nil UUID denotes global scope |
| key | caller key, 1 through 128 UTF-8 bytes |
| request_json | canonical normalized request without the idempotency key |
| result_json | exact successful result returned by the first operation |
| resource_id | created resource UUID |
| created_at_us | signed Unix microseconds |
| expires_at_us | NULL until later retention policy assigns expiry |

The composite primary key is (method, scope_id, key).

### 10.3 Required indexes and SQLite-only enforcement

Create and validate at least:

- unique jobu_queues(name);
- jobs by (queue_id, state, type, priority DESC, id);
- runs by (queue_id, state, runnable_at_us, priority DESC, planned_at_us, id);
- runs by (job_id, state);
- attempts by (state, started_at_us);
- terminal runs by (completed_at_us DESC, id);
- idempotency by (resource_id);
- secret references by (job_id);
- a SQLite partial unique index on jobu_runs(job_id) where
  schedule_owned=1 and state is scheduled, running, or retry_wait.

The partial unique index is the final database backstop for the
one-schedule-owned-non-terminal-run invariant. ManagementService must still
check and return a domain error rather than relying on a raw constraint error.

### 10.4 Version-1 validation

For an existing version-1 database, ensure_schema() verifies:

- exactly one valid version row;
- every required table and index exists in sqlite_schema;
- foreign key checking is enabled by the driver;
- a zero-row SELECT naming every required column can prepare and execute;
- PRAGMA foreign_key_check returns no row.

Do not compare the original CREATE TABLE SQL text byte-for-byte. Equivalent
formatting is irrelevant; missing objects, columns, indexes, or broken foreign
keys are not.

No destructive repair is attempted. A failed validation leaves the database
unchanged and prevents jobud from listening.

## 11. Private repositories and transaction ownership

### 11.1 General repository rules

Every repository:

- borrows the ManagementService Database and never opens, closes, moves, or
  owns it;
- is used only on the database owner thread;
- creates short-lived Query objects per operation;
- accepts and returns only domain values, never sqlite3 or native values;
- does not begin, commit, or roll back a transaction;
- distinguishes no row, end of list, and database failure;
- decodes every selected column through checked shared adapters;
- maps expected constraint conflicts to stable jobu.* errors and preserves the
  original db error only in Error::detail;
- uses named placeholders and binds every value explicitly;
- places a deterministic upper bound on every list and delete query.

ManagementService owns transactions for multi-repository operations. Declare a
Transaction guard before every Query in the same scope so queries are
destroyed before guard cleanup.

### 11.2 QueueRepository

The private QueueRepository supports:

- insert(Queue plus internal_name);
- find_by_id(id, include_deleted);
- find_by_name(name, include_deleted);
- list(filter, limit, after_id);
- replace_mutable_fields(queue);
- set_state(id, expected current-state set, next state, updated time);
- count_running_attempts(queue_id);
- count_non_deleted_jobs(queue_id);
- mark_deleted(id, internal deletion name, original name, time);
- physically_purge_deleted(limit).

Selection always aliases columns to stable names and decodes the complete row.
No caller observes deleted_name or the internal rewritten name separately.

Queue creation performs a friendly name lookup before INSERT. Because jobud
owns the adjacent SQLite lock and one connection, no second JobU writer can
race it in Phase 3; INSERT constraint errors are still mapped defensively.

### 11.3 JobRepository

The private JobRepository supports:

- insert(JobDefinition);
- find_by_id(id, include_deleted);
- list(queue filter, state/type filters, limit, after_id);
- update_definition(id, expected_revision, replacement fields, next_revision);
- set_state(id, expected current-state set, next state, next_revision, time);
- move(id, expected_revision, target_queue_id, next_revision, time);
- mark_deleted(id, expected_revision, next_revision, time);
- mark_all_in_queue_deleted(queue_id, time);
- has_started_attempt(id);
- physically_purge_deleted(limit).

Revision-sensitive UPDATE statements include both id=:id and
revision=:expected_revision in the WHERE clause. Zero affected rows trigger a
second lookup:

- missing row becomes jobu.job.not_found;
- deleted row becomes jobu.job.deleted;
- different revision becomes jobu.job.revision_conflict;
- otherwise the state changed incompatibly and becomes
  jobu.job.state_conflict.

This diagnostic lookup is inside the same immediate transaction as the
attempted mutation.

### 11.4 RunRepository

The private RunRepository supports:

- insert_schedule_owned(JobRun);
- find_schedule_owned(job_id);
- find_by_id(run_id);
- refresh_unstarted_schedule_owned(job_id, new snapshot and planned time);
- move_non_terminal(job_id, target_queue_id);
- cancel_pending_for_job(job_id, completion time and reason);
- cancel_pending_for_queue(queue_id, completion time and reason);
- count_running_for_job(job_id);
- count_running_for_queue(queue_id);
- list terminal rows eligible for a retention batch;
- delete selected terminal rows.

refresh_unstarted_schedule_owned() updates only a run in scheduled state for
which no attempt exists. It updates the job revision, queue if required,
planned/runnable time, type, priority, attributes, and payload together.

cancel_pending_* changes scheduled or retry_wait to cancelled and writes a
small terminal result object. It never changes running. Phase 3 cannot create
retry_wait, but handling it here freezes the deletion contract required later.

### 11.5 AttemptRepository and output

The private AttemptRepository supports fixture and later scheduler primitives:

- insert_attempt();
- find(run_id, attempt_number);
- list_for_run(run_id, bounded limit);
- insert_or_replace_output() only while completing an attempt transaction;
- has_any_for_run() and has_started_for_job();
- delete only through cascading terminal-run purge.

Phase 3 production code does not call insert_attempt or output writes. Tests
verify composite keys, BLOB round trips including embedded zero bytes, result
documents, and cascade behavior.

### 11.6 SecretRepository

SecretRepository supports set, list_metadata, erase, replace_references_for_job,
and reference_count.

set() inserts or replaces a named value while preserving created_at and
updating updated_at. Names use the same canonical syntax as attribute names
but allow a single segment; maximum length is 128 bytes.

erase() refuses a referenced secret with jobu.secret.in_use. No public
ManagementService secret method or RPC handler is added in Phase 3. Tests call
the private repository to establish the storage contract for Phase 8.

### 11.7 IdempotencyRepository

The repository operates on:

~~~cpp
struct IdempotencyRecord {
    std::string                            method;
    jb::core::Uuid                         scope_id;
    std::string                            key;
    std::string                            request_json;
    std::string                            result_json;
    jb::core::Uuid                         resource_id;
    jb::core::UtcTimePoint                 created_at;
    std::optional<jb::core::UtcTimePoint>  expires_at;
};
~~~

It supports find(method, scope_id, key), insert(record), erase_for_resource(),
and erase_expired(limit).

Phase 3 stores canonical request text rather than only a hash. Byte-for-byte
comparison is collision-free and avoids adding a cryptographic dependency.
The request limit already bounds record size. This is an intentional,
stronger form of the technical plan's canonical-request-hash requirement.
A future migration may add a digest for indexing without changing semantics.

## 12. ManagementService public API

### 12.1 Selectors, pages, and requests

management.hpp defines a queue selector that is explicit and unambiguous:

~~~cpp
using QueueSelector = std::variant<jb::core::Uuid, std::string>;

struct PageOptions {
    std::size_t                    limit{100};
    std::optional<jb::core::Uuid>  after_id;
};

struct QueuePage {
    std::vector<Queue>             items;
    std::optional<jb::core::Uuid>  next_after_id;
};

struct JobPage {
    std::vector<JobDefinition>     items;
    std::optional<jb::core::Uuid>  next_after_id;
};
~~~

Limits must be 1 through 200. Resources are ordered by UUID bytes ascending.
The next_after_id is present only when another row exists. Repositories fetch
limit+1 rows to determine this without a separate count. UUIDv7 makes the
ordering creation-like while retaining a deterministic tie-free key.

The exact request values are:

~~~cpp
struct CreateQueueRequest {
    std::string                         name;
    std::uint32_t                       weight{1};
    std::uint32_t                       concurrency_limit{1};
    RecoveryPolicy                      recovery_policy{RecoveryPolicy::FailInterrupted};
    AttributeSet                        defaults;
    std::optional<std::chrono::seconds> history_retention;
    std::chrono::milliseconds           runnable_wait_warning{10000};
    std::optional<std::string>          idempotency_key;
};

struct UpdateQueueRequest {
    QueueSelector                                queue;
    std::optional<std::string>                   name;
    std::optional<std::uint32_t>                 weight;
    std::optional<std::uint32_t>                 concurrency_limit;
    std::optional<RecoveryPolicy>                recovery_policy;
    std::optional<AttributeSet>                  defaults;
    std::optional<std::optional<std::chrono::seconds>> history_retention;
    std::optional<std::chrono::milliseconds>     runnable_wait_warning;
};

struct QueueListRequest {
    bool                      include_deleted{false};
    std::optional<QueueState> state;
    PageOptions               page;
};

struct CreateJobRequest {
    QueueSelector                queue;
    std::optional<std::string>   name;
    JobType                      type{JobType::Cli};
    JobSchedule                  schedule;
    std::int32_t                 priority{0};
    AttributeSet                 attributes;
    jb::rpc::JsonValue           payload;
    std::optional<std::string>   idempotency_key;
};

struct UpdateJobRequest {
    jb::core::Uuid                         job_id;
    JobRevision                           expected_revision{0};
    std::optional<std::optional<std::string>> name;
    std::optional<JobType>                 type;
    std::optional<JobSchedule>             schedule;
    std::optional<std::int32_t>            priority;
    AttributeSet                           attribute_changes;
    std::optional<jb::rpc::JsonValue>      payload;
};

struct JobListRequest {
    std::optional<QueueSelector> queue;
    bool                         include_deleted{false};
    std::optional<JobState>      state;
    std::optional<JobType>       type;
    PageOptions                  page;
};
~~~

The nested optional for queue retention distinguishes unchanged, inherit, and
a concrete zero/unlimited or positive value. The nested optional job name
distinguishes unchanged, clear, and set.

UpdateJobRequest.attribute_changes is a patch: supplied names replace stored
values and omitted names remain unchanged. Attributes cannot be removed from a
fully materialized job. Queue defaults are a complete partial set and an
UpdateQueueRequest replacement may be empty to clear all queue defaults.

MoveJobRequest and DeleteJobRequest contain job_id and expected_revision.
State operations contain only the resource selector/ID because suspend and
resume are naturally idempotent.

### 12.2 Service contract

ManagementService is move-disabled, owns private repository objects through a
pimpl, and borrows all collaborators:

~~~cpp
class ManagementService final {
public:
    ManagementService(
        jb::db::Database& database,
        AttributeRegistry const& attributes,
        jb::core::UuidGenerator& uuid_generator,
        jb::core::TimeSource& time_source,
        AttributeSet daemon_defaults = {});
    ~ManagementService();

    [[nodiscard]] auto create_queue(CreateQueueRequest const&)
        -> jb::core::Result<Queue, jb::core::Error>;
    [[nodiscard]] auto get_queue(QueueSelector const&, bool include_deleted = false)
        -> jb::core::Result<Queue, jb::core::Error>;
    [[nodiscard]] auto list_queues(QueueListRequest const&)
        -> jb::core::Result<QueuePage, jb::core::Error>;
    [[nodiscard]] auto update_queue(UpdateQueueRequest const&)
        -> jb::core::Result<Queue, jb::core::Error>;
    [[nodiscard]] auto suspend_queue(QueueSelector const&)
        -> jb::core::Result<Queue, jb::core::Error>;
    [[nodiscard]] auto resume_queue(QueueSelector const&)
        -> jb::core::Result<Queue, jb::core::Error>;
    [[nodiscard]] auto delete_queue(QueueSelector const&)
        -> jb::core::Result<void, jb::core::Error>;

    [[nodiscard]] auto create_job(CreateJobRequest const&)
        -> jb::core::Result<JobDefinition, jb::core::Error>;
    [[nodiscard]] auto get_job(jb::core::Uuid const&, bool include_deleted = false)
        -> jb::core::Result<JobDefinition, jb::core::Error>;
    [[nodiscard]] auto list_jobs(JobListRequest const&)
        -> jb::core::Result<JobPage, jb::core::Error>;
    [[nodiscard]] auto update_job(UpdateJobRequest const&)
        -> jb::core::Result<JobDefinition, jb::core::Error>;
    [[nodiscard]] auto suspend_job(jb::core::Uuid const&)
        -> jb::core::Result<JobDefinition, jb::core::Error>;
    [[nodiscard]] auto resume_job(jb::core::Uuid const&)
        -> jb::core::Result<JobDefinition, jb::core::Error>;
    [[nodiscard]] auto move_job(MoveJobRequest const&)
        -> jb::core::Result<JobDefinition, jb::core::Error>;
    [[nodiscard]] auto delete_job(DeleteJobRequest const&)
        -> jb::core::Result<void, jb::core::Error>;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};
~~~

The actual header documents:

- Database, registry, generator, and time source lifetime requirements;
- database owner-thread affinity;
- that daemon defaults are validated and copied at construction;
- list bounds and selector behavior;
- transaction boundaries and possible error categories;
- revision behavior;
- that callbacks, background threads, and event-loop processing do not occur.

### 12.3 Read and write transaction policy

Single-row get and bounded list operations do not begin an explicit
transaction. They execute on the sole JobU connection and return one statement
snapshot.

Every create, update, lifecycle, move, delete, idempotency, and retention
operation begins one immediate Transaction guard. The order is:

1. validate pure input that needs no database;
2. begin transaction;
3. re-read and validate durable state;
4. perform all repository writes;
5. encode any idempotency result;
6. insert/update the idempotency record;
7. commit;
8. return the owned result.

UUIDs and the current UTC time are acquired before beginning the transaction
where practical. A generated-but-unused UUID after a conflict is harmless.
No transaction remains open while writing stdout, logging, waiting on I/O, or
running the event loop.

## 13. Queue and job lifecycle behavior

### 13.1 Queue create, update, and lookup

Queue creation:

- validates name, positive weight and concurrency, recovery policy, retention,
  warning threshold, defaults, and optional idempotency key;
- generates UUIDv7 and reads one UTC timestamp;
- starts active;
- stores created_at=updated_at and no deleted_at;
- uses global nil UUID idempotency scope for queue.create.

Queue get accepts exact ID or exact active name. A name never resolves a
deleted queue unless include_deleted is true; even then it matches the original
deleted_name, not the internal rewritten value. If several historical deleted
queues share that original name, name lookup returns
jobu.queue.ambiguous_deleted_name and requires an ID.

Queue update changes only supplied fields. An empty update is invalid.
Renaming checks active uniqueness. Changing defaults affects only later job
creation and never rewrites existing jobs or runs.

### 13.2 Job create

Job creation:

1. validates the queue selector and loads a non-deleted queue;
2. requires the queue to be active or suspended, not suspending;
3. rejects CronSchedule in Phase 3;
4. validates type, name, one-time timestamp, priority, payload, supplied
   attributes, and idempotency key;
5. materializes built-in, daemon, queue, and job attribute layers;
6. generates separate UUIDv7 values for the definition and run;
7. creates revision 1 active JobDefinition;
8. creates its scheduled schedule-owned JobRun with runnable_at=planned_at and
   a complete immutable snapshot;
9. stores the idempotency result in the same transaction;
10. commits.

A planned time in the past is valid. It becomes overdue scheduled work for
Phase 4; Phase 3 does not dispatch it.

The idempotency scope for job.create is the resolved queue UUID. A replay with
the same normalized request returns the originally encoded JobDefinition even
if the live definition was later updated. A different normalized request with
the same key returns jobu.idempotency.conflict.

### 13.3 Job update and revisions

Job update:

- requires expected_revision greater than zero;
- rejects a deleted definition;
- rejects any change after the first attempt has started for a one-time job;
- permits updates while active or suspended;
- rejects updates while suspending;
- applies supplied scalar fields and the attribute patch to the stored complete
  attributes;
- validates the resulting payload and complete attributes;
- increments the revision exactly once;
- refreshes the unstarted scheduled run with the new revision, planned time,
  type, priority, attributes, and payload in the same transaction.

No queue or daemon default is reapplied. Overflow at UINT64_MAX returns
jobu.job.revision_exhausted.

### 13.4 Suspend and resume

Suspend is naturally idempotent:

- deleted is an error;
- suspended returns the current value unchanged;
- suspending proceeds to the completion check;
- active changes to suspending;
- if no running attempt belongs to the resource, suspending changes to
  suspended in the same transaction;
- each actual job-state change increments the job revision once; a no-op does
  not.

Because Phase 3 creates no running attempts, API calls normally return
suspended. Repository tests seed running-attempt fixtures to prove that
suspending can remain durable for Phase 4/7.

Resume:

- deleted is an error;
- active is a no-op;
- suspending or suspended becomes active;
- a job revision increments once when its state changes;
- no planned time is recalculated and no missed work is created.

Queue suspension does not alter contained job states. Job suspension does not
alter its scheduled run; later eligibility requires both queue and job active.

### 13.5 Move

Moving a job:

- requires the job to be fully suspended;
- requires expected_revision to match;
- requires the target queue to exist, not be deleted or suspending, and differ
  from the source;
- preserves name, type, schedule, priority, complete attributes, and payload;
- never applies target queue defaults;
- moves the non-terminal run's queue_id;
- increments the job revision and refreshes the run's job_revision;
- preserves historical terminal run queue IDs.

The target queue may be active or suspended. The job remains suspended.

### 13.6 Job delete

Job deletion:

- requires expected_revision;
- requires the job to be fully suspended;
- requires no running attempt;
- changes the definition to deleted and increments its revision;
- cancels scheduled/retry_wait runs with a job_deleted result;
- removes current secret-reference rows;
- retains definition, run, and attempts for history/retention.

Repeated deletion by ID returns jobu.job.deleted rather than success because
the operation has a required revision. No hard delete occurs in this method.

### 13.7 Queue delete

Queue deletion:

- requires the queue to be fully suspended;
- requires no running attempt in the queue;
- soft-deletes every non-deleted job in the queue;
- increments each affected job revision once;
- cancels scheduled/retry_wait runs with a queue_deleted result;
- removes current secret-reference rows for those jobs;
- copies the original queue name to deleted_name;
- rewrites the unique internal name to
  original-deleted#canonical-queue-uuid;
- changes the queue to deleted and records deleted_at;
- commits all changes together.

The method is bounded by the number of definitions in one queue. Target
customer loads are small, but the implementation must still avoid one Query
per job: use set-based UPDATE statements for bulk state/cancellation changes.
If future loads make this transaction too large, batching is a later explicit
design change; Phase 3 must not leave a half-deleted queue.

## 14. Idempotency, secret references, and retention primitives

### 14.1 Canonical create requests

Canonical requests are serialized from validated typed requests, never from
the raw JSON object. Object keys and attributes are sorted by their owning map
types. Defaults omitted by the caller are written explicitly in canonical
form. The idempotency_key field itself is excluded.

Therefore semantically equivalent requests such as an omitted weight and
weight=1 compare equal. Unknown request fields are rejected before
canonicalization.

The service handles idempotency inside the create transaction:

1. find existing record;
2. if request_json matches, decode and return stored result_json without
   executing the mutation;
3. if it differs, return conflict;
4. otherwise execute the mutation and insert the record before commit.

Queue creation uses method queue.create and nil scope. Job creation uses
method job.create and the resolved queue ID. Creation without a key stores no
record.

### 14.2 Secret references

Phase 3 recognizes the canonical reference object:

~~~json
{"secret": "canonical.name"}
~~~

Only fields explicitly designated sensitive by an AttributeDefinition, plus
future runner payload fields registered by their validators, may use it.
Phase 3 built-in attributes are not sensitive, and the minimal CLI/HTTP
payloads define no secret-bearing field. Consequently normal Phase 3 jobs have
no secret refs.

The private repository and job-update transaction nevertheless support
replacement of reference rows so Phase 5/6 can activate secret-bearing fields
without changing deletion rules.

### 14.3 Retention primitives

RetentionRepository provides one bounded purge_batch(cutoff, limit) operation
with limit 1 through 1000. In one transaction it:

- selects terminal run IDs completed before cutoff, oldest first;
- deletes them, cascading attempts and output;
- removes expired idempotency rows in a separate bounded statement;
- physically deletes soft-deleted jobs that have no run or idempotency
  reference;
- physically deletes soft-deleted queues that have no job, run, or
  idempotency reference;
- returns counts by row category.

Phase 3 exposes this only to tests/private application code. It does not
schedule cleanup and does not derive per-queue cutoffs; Phase 9 does that.

## 15. JSON management protocol

### 15.1 General rules

management_json.hpp exposes documented to_json/from_json functions for Queue,
JobDefinition, pages, and every public request type used by jobuctl.

Rules:

- request params must be objects;
- request objects reject unknown members to catch mistakes;
- exactly one of queue_id or queue_name represents a QueueSelector;
- response decoders require known mandatory fields but ignore unknown fields;
- UUIDs and UTC timestamps use the canonical representations from Section 8;
- enum text is lower-case snake-case;
- attributes use the public definition-directed encoding;
- payload stays a JSON object;
- numeric values are range checked before narrowing;
- a missing optional differs from explicit JSON null only where the request
  contract documents clearing a value;
- decoder errors use stable jobu.protocol.invalid_* codes and never throw for
  peer-controlled data.

### 15.2 Queue wire shapes

queue.create params:

~~~json
{
  "name": "default",
  "weight": 1,
  "concurrency_limit": 1,
  "recovery_policy": "fail_interrupted",
  "defaults": {},
  "history_retention_seconds": null,
  "runnable_wait_warning_ms": 10000,
  "idempotency_key": "optional-key"
}
~~~

queue.get, suspend, resume, and delete use exactly one selector:

~~~json
{"queue_id": "018f..."}
~~~

or:

~~~json
{"queue_name": "default"}
~~~

queue.list accepts include_deleted, optional state, limit, and after_id.
queue.update combines a selector with at least one mutable field.

Queue results use:

~~~json
{
  "id": "canonical-uuid",
  "name": "default",
  "state": "active",
  "weight": 1,
  "concurrency_limit": 1,
  "recovery_policy": "fail_interrupted",
  "defaults": {},
  "history_retention_seconds": null,
  "runnable_wait_warning_ms": 10000,
  "created_at": "2026-07-21T08:00:00.000000Z",
  "updated_at": "2026-07-21T08:00:00.000000Z",
  "deleted_at": null
}
~~~

List results are objects with items and nullable next_after_id.

### 15.3 Job wire shapes

One-time job.create params:

~~~json
{
  "queue_name": "default",
  "name": "nightly-export",
  "type": "cli",
  "schedule": {
    "kind": "once",
    "at": "2026-07-21T21:00:00.000000Z"
  },
  "priority": 0,
  "attributes": {
    "job.timeout": 120000
  },
  "payload": {
    "command": "/usr/bin/example",
    "arguments": ["--dry-run"]
  },
  "idempotency_key": "create-export-1"
}
~~~

A cron schedule is syntactically representable:

~~~json
{"kind": "cron", "expression": "0 * * * *", "timezone": "UTC"}
~~~

but create/update returns jobu.schedule.cron_unavailable in Phase 3.

job.get uses job_id. job.list accepts an optional queue selector,
include_deleted, optional state/type, limit, and after_id. job.update requires
job_id and expected_revision plus at least one change. job.move requires
job_id, expected_revision, and a target queue selector. job.delete requires
job_id and expected_revision. suspend/resume require only job_id.

JobDefinition results include id, queue_id, revision, name, state, type,
schedule, priority, complete attributes, payload, and timestamps. They do not
include the schedule-owned run. Run inspection remains a later method family.

## 16. JSON-RPC management API

### 16.1 Methods and capabilities

Register these exact methods:

- queue.create
- queue.get
- queue.list
- queue.update
- queue.suspend
- queue.resume
- queue.delete
- job.create
- job.get
- job.list
- job.update
- job.suspend
- job.resume
- job.move
- job.delete

system.info changes to API version 1.1 and advertises system.info plus exactly
the registered methods. Do not advertise job.run_now, run.*, attempt.*,
secret.*, schedule.*, stats, or cron support.

The absence of capability schedule.cron tells compatible clients that cron
creation is unavailable even though the reserved schedule object can be
decoded.

### 16.2 Handler registration

management_rpc_priv.hpp provides an application-internal function:

~~~cpp
auto register_management_methods(
    jb::rpc::Server& server,
    ManagementService& service) -> bool;
~~~

It registers all handlers or returns false. Because rpc::Server has no atomic
bulk-registration API, jobud treats any false return as fatal before listening
and destroys the server. It does not attempt to continue with a partial
capability set.

Each handler:

1. decodes strict params;
2. calls exactly one service method;
3. encodes the owned success value;
4. converts service Error with rpc::application_error();
5. returns Invalid Params for DTO shape/range errors;
6. returns Internal Error only if success encoding unexpectedly fails.

RequestContext peer identity is available for future audit, but Phase 3 does
not persist it or implement ownership/RBAC.

### 16.3 Error visibility

Expected domain and storage errors use JSON-RPC -32000 and data containing only
category and stable code. Invalid request shapes use -32602. Internal schema
startup failures never appear over RPC because the server does not listen.

No response contains:

- SQL text or placeholder names;
- SQLite diagnostics or result codes;
- filesystem database or socket paths;
- internal soft-deletion names;
- raw canonical idempotency request/result documents;
- secret values;
- Error::detail.

## 17. jobud and jobuctl integration

### 17.1 jobud startup

The Phase 3 foreground syntax is:

~~~text
jobud --socket <filesystem-path> --database <sqlite-file>
~~~

--version remains independent and requires no database. All other invocations
require exactly one socket and database option.

Startup order:

1. parse arguments without opening resources;
2. construct Application and SystemTimeSource;
3. construct db::sqlite::Driver with the explicit file and existing Phase 1
   defaults;
4. construct Database and open it;
5. call jobu::sqlite::ensure_schema();
6. construct UuidV7Generator, StandardAttributeRegistry, and
   ManagementService;
7. register system.info and every management handler;
8. construct/listen LocalServer;
9. accept connections into rpc::Server;
10. enter the event loop.

Any failure through Step 7 logs a safe error and exits nonzero without creating
the RPC socket. A listen failure closes the database during normal stack
unwinding. Phase 3 does not daemonize or react to signals beyond existing
process termination behavior.

The database object is declared before service and after driver ownership has
transferred, ensuring every Query/service is destroyed before Database.

### 17.2 jobuctl command session

Every management command:

1. connects one LocalSocket;
2. constructs one rpc::Client;
3. calls system.info;
4. verifies API major 1 and the required method capability;
5. sends the management request;
6. decodes the typed response;
7. prints a concise human-readable result;
8. exits zero;
9. uses one five-second timer covering handshake and command.

Remote application errors print their safe message and stable code to stderr.
Transport/protocol details go to the existing logger. There are no nested
event loops or blocking socket waits.

### 17.3 Initial queue commands

Implement:

~~~text
jobuctl --socket PATH queue create NAME
    [--weight N] [--concurrency-limit N]
    [--recovery-policy fail_interrupted|retry_interrupted]
    [--idempotency-key KEY]

jobuctl --socket PATH queue get (--id UUID | --name NAME)
jobuctl --socket PATH queue list [--include-deleted] [--limit N] [--after UUID]
jobuctl --socket PATH queue update (--id UUID | --name NAME)
    [--new-name NAME] [--weight N] [--concurrency-limit N]
jobuctl --socket PATH queue suspend (--id UUID | --name NAME)
jobuctl --socket PATH queue resume  (--id UUID | --name NAME)
jobuctl --socket PATH queue delete  (--id UUID | --name NAME)
~~~

Phase 3 CLI does not yet expose queue default attributes, retention, or warning
threshold updates even though RPC supports them. Phase 8 completes advanced
CLI coverage.

### 17.4 Initial job commands

Implement:

~~~text
jobuctl --socket PATH job create (--queue-id UUID | --queue-name NAME)
    --type cli --at UTC --command PATH [--arg VALUE ...]
    [--name NAME] [--priority N] [--idempotency-key KEY]

jobuctl --socket PATH job create (--queue-id UUID | --queue-name NAME)
    --type http --at UTC --url URL [--method METHOD]
    [--name NAME] [--priority N] [--idempotency-key KEY]

jobuctl --socket PATH job get UUID
jobuctl --socket PATH job list
    [--queue-id UUID | --queue-name NAME]
    [--include-deleted] [--limit N] [--after UUID]
jobuctl --socket PATH job update UUID --revision N
    [--name NAME | --clear-name] [--priority N] [--at UTC]
jobuctl --socket PATH job suspend UUID
jobuctl --socket PATH job resume UUID
jobuctl --socket PATH job move UUID --revision N
    (--queue-id UUID | --queue-name NAME)
jobuctl --socket PATH job delete UUID --revision N
~~~

Repeated --arg preserves argument order. An HTTP method defaults to GET.
Phase 3 CLI does not expose cron, arbitrary attributes, or arbitrary payload
members. RPC tests cover those generic DTO fields.

Create prints the created UUID and revision. Get/list print complete useful
summary lines without output or secret data. Mutations print the resulting
state/revision; delete prints the deleted UUID.

## 18. Stable errors

### 18.1 Schema and storage

| Code | Category | Meaning |
| --- | --- | --- |
| jobu.schema.invalid_database | InvalidArgument | database is closed, non-SQLite, or otherwise unusable for this schema |
| jobu.schema.database_not_empty | Conflict | an unmarked database already contains user schema objects |
| jobu.schema.newer_database | Unsupported | stored schema version is newer than the binary |
| jobu.schema.invalid | Internal | version row, table, index, column, or foreign-key validation failed |
| jobu.schema.create_failed | Internal | version-1 schema creation failed |
| jobu.storage.invalid_uuid | Internal | persisted UUID is not exactly 16 bytes |
| jobu.storage.invalid_enum | Internal | persisted stable enum text is unknown |
| jobu.storage.invalid_time | Internal | persisted UTC integer cannot be represented |
| jobu.storage.invalid_json | Internal | persisted JSON document has invalid syntax or shape |
| jobu.storage.invalid_attribute | Internal | persisted attribute document is invalid |
| jobu.storage.invariant | Internal | rows violate a cross-table domain invariant |

Schema/storage errors retain a safe generic message. Error::detail may contain
the underlying db code and object/column name for logs, but never persisted
values, JSON bodies, or secret bytes.

### 18.2 Attribute and protocol

| Code | Category | Meaning |
| --- | --- | --- |
| jobu.attribute.unknown | InvalidArgument | supplied attribute name is not registered |
| jobu.attribute.invalid_scope | InvalidArgument | value is not accepted at this default/job scope |
| jobu.attribute.invalid_type | InvalidArgument | value alternative does not match its definition |
| jobu.attribute.invalid_value | InvalidArgument | range, enum, or cross-field rule failed |
| jobu.attribute.invalid_document | Internal | persisted typed attribute document is malformed |
| jobu.protocol.invalid_request | InvalidArgument | typed request JSON has an invalid shape |
| jobu.protocol.invalid_response | InvalidArgument | typed response JSON has an invalid shape |
| jobu.protocol.value_too_large | ResourceExhausted | bounded name, payload, or document limit was exceeded |
| jobu.time.invalid_format | InvalidArgument | UTC input is not accepted RFC 3339 Z form |
| jobu.time.out_of_range | InvalidArgument | UTC input cannot be represented |

RPC request decoding converts these shape failures to Invalid Params. Response
decoding errors remain local client errors.

### 18.3 Queue, job, and idempotency

| Code | Category | Meaning |
| --- | --- | --- |
| jobu.queue.invalid_name | InvalidArgument | name syntax/length is invalid or reserved |
| jobu.queue.not_found | NotFound | selector does not resolve an allowed queue |
| jobu.queue.name_conflict | Conflict | active/internal unique name already exists |
| jobu.queue.ambiguous_deleted_name | Conflict | historical name resolves more than one deleted queue |
| jobu.queue.state_conflict | Conflict | lifecycle state forbids the operation |
| jobu.queue.not_suspended | Conflict | move/delete prerequisite is not met |
| jobu.queue.has_running_attempt | Conflict | deletion cannot proceed while work is running |
| jobu.job.invalid_name | InvalidArgument | optional name syntax/length is invalid |
| jobu.job.invalid_payload | InvalidArgument | payload is not structurally valid for its type |
| jobu.job.not_found | NotFound | job ID does not resolve an allowed definition |
| jobu.job.deleted | Conflict | operation targets a soft-deleted definition |
| jobu.job.revision_conflict | Conflict | expected revision differs from stored revision |
| jobu.job.revision_exhausted | ResourceExhausted | revision cannot be incremented |
| jobu.job.state_conflict | Conflict | lifecycle state forbids the operation |
| jobu.job.not_suspended | Conflict | move/delete prerequisite is not met |
| jobu.job.immutable | Conflict | a one-time attempt has already started |
| jobu.job.has_running_attempt | Conflict | deletion cannot proceed while work is running |
| jobu.schedule.cron_unavailable | Unsupported | Phase 3 cannot validate/create recurring jobs |
| jobu.run.schedule_conflict | Conflict | schedule-owned non-terminal run already exists/missing unexpectedly |
| jobu.idempotency.invalid_key | InvalidArgument | key is empty, too large, or invalid UTF-8 |
| jobu.idempotency.conflict | Conflict | same scope/key was used for a different canonical request |
| jobu.idempotency.invalid_record | Internal | stored canonical request/result cannot be validated |
| jobu.secret.invalid_name | InvalidArgument | secret name is invalid |
| jobu.secret.not_found | NotFound | secret does not exist |
| jobu.secret.in_use | Conflict | a current job reference prevents deletion |

Expected Database errors that do not have a more precise domain mapping retain
their existing db.* stable code and category. Do not collapse busy,
permission-denied, resource-exhausted, I/O, or corruption into an
InvalidArgument domain error.

## 19. Testing design

### 19.1 Deterministic collaborators

Add SequenceUuidGenerator to test/support. It returns a configured sequence and
then a stable exhaustion error. Use the existing FakeTimeSource for every
service test. Tests must never depend on actual wall time, UUID randomness,
row order without ORDER BY, or a sleep.

Each SQLite test uses TemporaryDirectory and a unique database path. Database,
service, Transaction, and Query declarations follow their required destruction
order. Tests explicitly close Database where they need to assert close
results.

### 19.2 Domain, time, and attribute tests

Cover:

- UTC epoch, pre-epoch, leap-day, fractional precision, min/max selected
  boundaries, canonical formatting, and every rejected syntax class;
- UUID/time/enum/value adapter round trips and malformed persisted values;
- every built-in attribute default, scope, type, range, and enum;
- precedence built-in to daemon to queue to job;
- queue-default changes not affecting an already materialized job;
- persistence typed tags for every AttributeValue alternative;
- malformed tags, overflow, invalid hex, excessive nesting, and unknown names;
- public attribute JSON conversion and rejection of unsupported nested
  Duration/Bytes.

### 19.3 SQLite schema tests

Cover:

- fresh file creation and SchemaStatus(created=true);
- second ensure on the same open connection and after close/reopen;
- all expected tables, columns, indexes, CHECK constraints through behavior,
  and foreign keys;
- schema version zero, duplicate/missing row, newer version, missing table,
  missing column, missing partial index, and foreign-key violation;
- failure partway through creation rolls back every object that belongs to the
  attempted schema;
- schema code includes no direct SQLite API even though its SQL/inspection is
  in the SQLite-specific target;
- Database driver_name mismatch is rejected before DDL.

Where deliberately malformed schemas must disable foreign keys to construct a
fixture, re-enable them before ensure_schema().

### 19.4 Queue management tests

Cover:

- create defaults and explicit fields;
- duplicate/reserved/invalid names;
- get by ID/name and bounded keyset list;
- update each mutable field and an invalid empty update;
- renamed selector behavior;
- defaults replacement and no rewriting of existing jobs;
- active to suspending to suspended, durable suspending with a seeded running
  attempt, resume from both states, and idempotent no-ops;
- delete prerequisites, internal name release, recreation of the original
  active name, deleted lookup/list behavior, and ambiguous historical name;
- transaction rollback when one part of bulk deletion conflicts.

### 19.5 Run and attempt repository tests

Cover:

- one schedule-owned run insertion and partial-index rejection of a second;
- complete immutable snapshot round trip;
- scheduled snapshot refresh and refusal after an attempt exists;
- queue move of non-terminal versus preservation of terminal history;
- scheduled/retry_wait cancellation and running preservation;
- attempt composite keys, ordered bounded listing, result JSON, output blobs,
  truncation flags, and cascade deletion.

### 19.6 Job management tests

Cover:

- CLI and HTTP one-time creation;
- atomic definition plus run insertion;
- past planned time acceptance;
- materialized attribute and payload snapshot equality;
- absent queue, deleted/suspending queue, invalid payload, and cron rejection;
- get/list filters, bounds, and continuation;
- update scalar fields, name clear, attribute patch, payload, and planned time;
- exact one-step revision increments and stale revision conflicts;
- no reapplication of changed daemon/queue defaults;
- atomic pending-run refresh;
- immutability after a seeded started attempt;
- suspend/resume and revisions;
- move prerequisites, target states, unchanged attributes, and run queue;
- job deletion and run cancellation;
- queue deletion's set-based job/run effects.

### 19.7 Idempotency, secret, and retention tests

Cover:

- same normalized create with omitted versus explicit defaults;
- replay returning the original result after the live resource changes;
- same key/different request conflict;
- method and queue scope isolation;
- no record after a rolled-back create;
- invalid stored result detection;
- secret set/update metadata, binary value preservation, metadata-only list,
  reference replacement, and delete refusal;
- bounded oldest-first terminal purge;
- attempt/output cascade;
- expired idempotency cleanup;
- physical job/queue purge only after references disappear;
- zero, maximum, and over-maximum batch limits.

### 19.8 JSON and in-memory RPC tests

Use the existing MemoryIODevice; no local socket is needed.

Cover:

- every request/result round trip;
- required, optional, null-clearing, unknown, duplicate logical, range, UUID,
  timestamp, enum, and size cases;
- response forward compatibility through ignored unknown fields;
- every registered management method's success and domain error;
- Invalid Params versus application-error mapping;
- capability list/API version;
- no registration of deferred methods;
- two clients issuing sequential mutations and observing shared durable state;
- handlers retaining no borrowed JSON or RequestContext references.

### 19.9 Linux executable tests

Use bounded child-process helpers and unique temporary database/socket paths.
Never use the user's home directory, a shared /tmp socket name, or an
unbounded wait.

Queue process test:

1. start jobud with new database;
2. create, list, update, suspend, resume, and suspend/delete a queue;
3. stop and restart jobud;
4. verify deleted state and name reuse;
5. verify system.info capabilities.

Job process test:

1. create a queue and one CLI plus one HTTP job;
2. inspect/list/update/suspend/move/resume/suspend/delete;
3. stop and restart between mutations;
4. verify IDs/revisions/state persisted;
5. directly inspect the SQLite file through project Database/Query APIs to
   prove exactly one scheduled run and zero attempts;
6. verify no external payload command or HTTP request ran.

All child processes have hard startup and completion deadlines and are
terminated/reaped on every assertion path. Socket, database, WAL, SHM, and lock
files are cleaned.

### 19.10 Optional macOS tests

The backend-independent and SQLite repository suites should compile and run
unchanged under Apple Clang. Process tests reuse the Darwin-compatible Phase 2
helpers and set TMPDIR=/tmp for short Unix-socket paths.

No macOS-specific production source is expected in Phase 3. A platform fix is
permitted only in an optional macOS stage and must be followed by the complete
Linux suite.

## 20. Reviewable implementation sequence

Every stage is a separate approval, implementation, validation, and review
boundary. Codex implements exactly one stage, reports changed files and
focused/full validation, and waits for explicit approval.

### Stage 3.1: Domain values, UTC text, and storage adapters

- add and fully document queue, job, run, attempt, secret, and UTC public
  headers;
- implement portable RFC 3339 UTC parsing/formatting;
- add private UUID, timestamp, enum, JSON, and checked-field adapters;
- add SequenceUuidGenerator test support;
- add direct-include public-header coverage and focused tests.

Exit: all Phase 3 domain values have portable, checked in-memory/database/wire
primitives, with no repositories or schema yet.

### Stage 3.2: Built-in attributes and materialization

- add and fully document StandardAttributeRegistry and materialize_attributes;
- implement built-in definitions, constraints, and cross-field validation;
- add private typed persistence codec and public JSON attribute conversion;
- add precedence, compatibility, codec, and malformed-input tests.

Exit: a complete job AttributeSet can be deterministically materialized and
round-tripped without a database.

### Stage 3.3: SQLite JobU schema version 1

- add jobu-sqlite target and fully documented sqlite_schema.hpp;
- implement transactional fresh creation, version checks, object/column/index
  validation, and foreign_key_check;
- add fresh/reopen/malformed/newer/rollback integration tests;
- verify jobu and db public interfaces remain free of SQLite details.

Exit: an open SQLite Database can create or validate an empty complete JobU
schema.

### Stage 3.4: Queue create/get/list/update

- add private QueueRepository;
- add and fully document queue request/page types and the initial
  ManagementService queue methods;
- implement validation, bounded listing, create, lookup, and mutable updates;
- add deterministic SQLite service tests;
- do not add RPC or executable changes.

Exit: queue CRUD except lifecycle/deletion works transactionally through the
public service.

### Stage 3.5: Run, attempt, and output persistence primitives

- add private RunRepository and AttemptRepository;
- implement schedule-owned invariant, snapshot round trips, refresh, move,
  cancellation, attempts, and output persistence primitives;
- add focused repository integration tests;
- keep production service behavior unchanged.

Exit: later job lifecycle stages have verified run/attempt storage operations.

### Stage 3.6: One-time job create/get/list

- add private JobRepository;
- extend and fully document ManagementService job requests/pages/methods;
- implement CLI/HTTP payload structural validation;
- atomically create revision-1 definition plus scheduled run;
- implement bounded get/list filters and explicit cron-unavailable failure;
- add focused service tests without RPC.

Exit: durable one-time jobs and their immutable scheduled snapshots can be
created and read.

### Stage 3.7: Job update and optimistic revision

- implement expected-revision update diagnostics;
- implement attribute patching without default reapplication;
- refresh the unstarted scheduled run atomically;
- enforce one-time immutability after attempt start;
- add stale revision, rollback, snapshot, and overflow tests.

Exit: a pending one-time definition can evolve without losing snapshot or
concurrency correctness.

### Stage 3.8: Queue and job suspend/resume

- implement active/suspending/suspended transitions;
- query running attempts through repositories;
- increment job revisions only for actual changes;
- test durable suspending with seeded running fixtures, normal immediate
  completion, resume, and idempotent no-ops.

Exit: lifecycle state storage is ready for later scheduler coordination.

### Stage 3.9: Move and soft deletion

- implement job move with revision and run-queue update;
- implement job soft delete and pending-run cancellation;
- implement atomic set-based queue deletion, internal rename, contained-job
  deletion, and cancellation;
- add prerequisite, history-preservation, name-reuse, and rollback tests.

Exit: the complete non-executing queue/job lifecycle works through
ManagementService.

### Stage 3.10: Idempotency, secrets, and retention primitives

- add private IdempotencyRepository, SecretRepository, and
  RetentionRepository;
- integrate optional canonical idempotency into queue.create and job.create;
- implement private secret metadata/value/reference persistence;
- implement bounded retention and physical purge primitives;
- add replay/conflict/rollback/secret/cascade/purge tests.

Exit: all Phase 3 repository families and durable-create idempotency are
verified without expanding RPC.

### Stage 3.11: Queue JSON contracts

- add and fully document management_json.hpp queue request/result adapters;
- implement strict request and forward-compatible response conversion;
- add exhaustive queue JSON tests;
- update public-header/Doxygen coverage.

Exit: queue service values have a frozen transport-independent JSON shape.

### Stage 3.12: Job JSON contracts

- add fully documented job request/result/page adapters;
- implement schedule, payload, attributes, revision, timestamp, and selector
  conversion;
- add exhaustive job JSON tests including reserved cron representation;
- keep RPC registrations unchanged.

Exit: job service values have a frozen JSON shape suitable for RPC and CLI.

### Stage 3.13: Linux jobud database bootstrap

- add explicit --database parsing while preserving --version;
- construct/open SQLite Database and call ensure_schema before listening;
- construct StandardAttributeRegistry, generators, and ManagementService;
- keep system.info as the only registered method in this stage;
- update Linux process smoke tests for fresh/reopen/newer-schema startup.

Exit: Linux jobud owns a validated durable database but exposes no management
mutation yet.

### Stage 3.14: Queue RPC methods

- register all queue handlers against ManagementService;
- update API version/capabilities for queue methods only;
- add MemoryIODevice RPC tests for success, invalid params, application
  errors, persistence, and registration failure;
- do not add job handlers or jobuctl queue commands yet.

Exit: a generic RPC client can complete the full durable queue lifecycle.

### Stage 3.15: Job RPC methods

- register all Phase 3 job handlers;
- advertise exact job capabilities and retain schedule.cron absence;
- add in-memory RPC lifecycle, revision, idempotency, and restart/database
  tests;
- verify no run/attempt/secret methods are registered.

Exit: the complete Phase 3 management service is available over JSON-RPC.

### Stage 3.16: Linux jobuctl queue commands

- refactor the existing one-shot client only as needed for system.info
  handshake plus one command;
- implement initial queue parsing, requests, response printing, capability
  checks, and bounded timeout;
- add the Linux queue executable lifecycle/restart test.

Exit: operators can manage durable queues through jobuctl on Linux.

### Stage 3.17: Linux jobuctl job commands and Phase 3 integration

- implement the initial one-time CLI/HTTP job command set;
- add job capability checks, request construction, response decoding, and
  concise output;
- add the Linux job lifecycle/restart test;
- inspect the resulting database for one scheduled run per job and zero
  attempts;
- run full GCC and Clang suites.

Exit: all Linux Phase 3 exit criteria pass. No macOS change is made before this
stage is complete.

### Stage 3.18 (optional): macOS schema and repository verification

- configure/build with Apple Clang and Homebrew dependencies;
- run public-header, UTC, attribute, schema, queue, job, run/attempt,
  idempotency, secret, retention, JSON, and in-memory RPC tests;
- correct only portable-core, DML, or SQLite schema defects found on macOS;
- do not redesign frozen public APIs.

Exit: backend-independent and SQLite Phase 3 behavior passes on macOS.

### Stage 3.19 (optional): macOS daemon/CLI verification

- run jobud bootstrap/fresh/reopen tests on macOS;
- run queue and one-time job executable lifecycle/restart tests;
- preserve short TMPDIR socket paths and bounded child cleanup;
- correct only macOS integration defects;
- run the complete Linux suite after any source change.

Exit: the optional macOS Phase 3 path matches Linux-visible behavior.

### Stage 3.20: Final verification and documentation audit

- perform a clean Linux configure and build with SQLite enabled;
- run the full test suite under GCC and Clang;
- perform a SQLite-disabled library build;
- run ASAN/UBSAN on repository, service, JSON, and in-memory RPC tests where
  available;
- inspect every new public header for direct-include completeness and Doxygen;
- inspect target link interfaces and source includes for SQLite leakage;
- inspect the final version-1 schema and indexes against this document;
- run both Linux executable lifecycle tests from a clean temporary directory;
- if optional macOS stages ran, record their exact commands/results without
  making them a Linux completion prerequisite.

Exit: Phase 3 is ready to merge and Phase 4 can begin from scheduled,
persisted one-time runs.

## 21. Validation commands

Run after every Linux stage:

~~~sh
cmake -S . -B .bld -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld
ctest --test-dir .bld/test --output-on-failure
~~~

Run focused executables before the full suite. Use the exact target names
introduced by the active stage.

For a SQLite-disabled boundary build:

~~~sh
cmake -S . -B .bld-no-sqlite -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DJB_BUILD_SQLITE_DRIVER=OFF
cmake --build .bld-no-sqlite
ctest --test-dir .bld-no-sqlite/test --output-on-failure
~~~

Also after each stage:

- format every changed C++ file with the checked-in .clang-format;
- run compiler warnings at the repository's configured level;
- ensure test queries have explicit ordering;
- ensure list/purge limits and process deadlines are finite;
- inspect changed public headers for Doxygen and include completeness;
- search public generic headers for sqlite3, sqlite_, PRAGMA, sqlite_schema,
  native database handles, and SQLite result constants;
- search RPC/CLI output paths for Error::detail, SQL, canonical idempotency
  documents, internal deleted names, and secret data;
- verify no test leaves database, WAL, SHM, lock, socket, descriptor, or child
  process resources behind.

Phase 3 Linux completion requires GCC and Clang. Apple Clang is required only
if the optional macOS stages are performed.

## 22. Review invariants

Before Phase 3 is complete, review these invariants directly:

- generic Database and Driver APIs remain unchanged and backend-neutral;
- all SQLite application DDL and schema inspection live in jobu-sqlite;
- jobu public headers expose no SQLite or native database type;
- schema creation/version validation happens before jobud listens;
- there is no generic migration framework, automatic downgrade, or backup
  subsystem;
- every accepted one-time job is created atomically with exactly one
  schedule-owned scheduled run;
- no Phase 3 production path creates an attempt or starts external work;
- cron create/update is rejected until Phase 4, not stored unchecked;
- every job update requiring a revision includes it in the SQL WHERE clause;
- existing job attributes never receive changed daemon/queue defaults;
- job move preserves complete attributes and historical terminal run queues;
- suspend prevents later eligibility through durable state and completes
  immediately only when no running attempt exists;
- job/queue deletion is soft, requires suspension/no running attempt, and
  cancels only scheduled/retry_wait work;
- queue deletion releases the active name through the exact internal rename
  rule while returning the original name in views;
- multi-table mutations and idempotency records commit or roll back together;
- idempotent replay returns the original stored result;
- secret values are absent from every public read type, RPC result, error, and
  log;
- every list and purge is bounded and keyset/ordered;
- strict request decoders reject unknown fields while response decoders ignore
  unknown fields;
- system.info advertises exactly implemented methods and API 1.1;
- no run/attempt/secret/schedule/stats RPC is accidentally exposed;
- every new public declaration is documented with Doxygen;
- optional macOS work appears only after Linux Stage 3.17;
- final Linux verification still passes after any optional macOS correction.

## 23. Handoff to Phase 4

Phase 4 may rely on these completed contracts:

- validated schema version 1 and the documented eligibility indexes;
- fully materialized immutable run snapshots;
- one schedule-owned scheduled run for every accepted Phase 3 job;
- durable queue/job states and revision behavior;
- private run/attempt primitives that can be extended inside jobu;
- fake time and deterministic UUID generation in tests;
- no real work having started.

Phase 4 adds cron parsing and occurrence calculation first, then permits
CronSchedule creation while atomically creating its first schedule-owned run.
It must not reinterpret Phase 3 one-time rows, reapply defaults, or change
Phase 3 management wire shapes without a separately reviewed compatibility
change.

## 24. Repository reference points

This design is grounded in:

- main commit 75423800061ec7cc0b901171b8334f2aca2ab0cd;
- the merged generic Database, Query, Record, Value, Transaction, and isolated
  db::sqlite::Driver APIs from Phase 1;
- the merged JsonValue, JSON codec, framing, JSON-RPC client/server, local IPC,
  SystemInfo, jobud, and jobuctl APIs from Phase 2;
- the JobU v1 technical plan's domain, persistence, idempotency, lifecycle, and
  phased-delivery decisions;
- the repository AGENTS.md one-stage-at-a-time implementation rule.

If implementation evidence contradicts an API or invariant in this document,
Codex must stop the active stage, report the contradiction, and request
approval for a design revision instead of silently changing the contract.
