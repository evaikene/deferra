# JobU Phase 4 Code-Level Design

## 1. Purpose and repository baseline

This document defines Phase 4 of JobU: the cron engine and the centralized,
event-driven scheduler, verified with deterministic fake attempt execution.
It is based on the JobU v1 technical plan .codex/jobu-v1-technical-plan.md.

The design is based on GitHub `main` at commit
[`6f5e730`](https://github.com/evaikene/deferra/commit/6f5e7302ad72b9c970b2338136e4de96097fa416)
on 2026-07-23. That revision contains the completed Phase 3 implementation:

- application schema version 1;
- durable queue, job, run, attempt, output, secret, and idempotency tables;
- complete queue and one-time-job management lifecycle;
- schedule-owned one-time run creation;
- queue and job suspend/resume, move, and soft deletion;
- typed management JSON and JSON-RPC integration;
- `jobud` SQLite bootstrap and `jobuctl` queue/job commands;
- Linux and macOS management lifecycle coverage.

Phase 4 must build on those contracts. It must not replace the database API,
change the Phase 3 management wire shapes unnecessarily, move SQL into
`jb::db`, or expose SQLite details through the generic `jobu` target.

The project remains C++20. Linux is implemented and fully verified first.
macOS-specific production adjustments and verification are isolated in
optional final stages.

## 2. Phase 4 outcome

At the end of Phase 4:

- five-field cron expressions can be parsed and evaluated in IANA timezones;
- the agreed day-of-month/day-of-week AND rule is implemented;
- spring-forward gaps and fall-back overlaps follow the agreed policies;
- recurring jobs can be created and updated through the existing management
  service and `job.create`/`job.update` RPC paths;
- every active recurring definition maintains exactly one schedule-owned
  non-terminal run;
- a centralized scheduler selects and dispatches due work without creating a
  thread per queue;
- global CLI/HTTP limits, per-queue combined limits, strict job priority, and
  weighted queue fairness are enforced;
- dispatch commits the run and attempt as running before invoking an executor;
- fixed and exponential retry policies, deterministic jitter, blocking and
  reschedule modes, and executor-provided retry deadlines are implemented;
- Run Now creates a separate manual run and enforces its schedule barrier;
- pending and running run cancellation transitions are implemented;
- recurring completion or cancellation creates the next future occurrence
  atomically;
- scheduler tests use fake time, fake execution, and explicit completions,
  never real processes, HTTP requests, or sleeps.

The Phase 4 scheduler is a real reusable production component, but `jobud`
must not be wired to a fake executor. The executable continues to run the
management plane only until a real HTTP executor is introduced in Phase 5.
Phase 4 scheduler integration is exercised in dedicated in-process tests.

## 3. Explicitly deferred

The following work remains outside Phase 4:

- libcurl, `jb::net::HttpClient`, and real HTTP execution;
- `jb::core::Process`, `SIGCHLD`, process groups, pipes, and real CLI
  execution;
- runner-specific payload expansion beyond the Phase 3 structural checks;
- runner-specific retry selector attributes such as exit-code and HTTP-status
  ranges;
- bounded real stdout/stderr or HTTP body/header capture;
- startup recovery of persisted running attempts;
- `fail_interrupted` and `retry_interrupted` recovery execution;
- repair of corrupted or missing recurrence/manual-run invariants at startup;
- complete fail-closed daemon shutdown and nonzero exit coordination;
- immediate daemon signal handling and executor termination;
- `run.get`, `run.list`, `attempt.get`, and attempt-output retrieval;
- public `job.run_now`, `run.cancel`, `schedule.validate`, and `schedule.next`
  JSON-RPC/`jobuctl` commands;
- secret management and runtime secret resolution;
- automatic retention timers, history queries, statistics, and delay warnings;
- daemon INI configuration, privilege dropping, systemd, and `launchd`
  packaging;
- database schema migration machinery;
- remote transports or multi-daemon claiming, leases, and fencing.

Phase 4 may add service-level Run Now and scheduler cancellation APIs because
the scheduler needs those state transitions. Their public wire and CLI
exposure remains a later control-plane stage.

## 4. Non-negotiable design decisions

### 4.1 One event-driven scheduler

There is one scheduler on the `jobud` owner/event-loop thread. Queue count does
not allocate threads or pre-create runner objects.

The scheduler owns:

- in-memory weighted-fairness credits;
- in-memory global and queue capacity counters;
- active attempt-to-executor correlation;
- manual-run barrier cache;
- one event-loop wake timer;
- coalesced rescan state.

SQLite remains the durable source of truth. Fairness credits and timer handles
are intentionally not persisted.

### 4.2 No external work before durable running state

For every attempt:

1. re-read and validate the candidate in an immediate transaction;
2. allocate the next positive attempt number;
3. insert the attempt in `running` state with `started_at`;
4. update the run to `running`;
5. commit;
6. only then call the executor.

If any database operation or commit fails, no executor is called.

An executor start failure is an observed job attempt result, not permission to
erase the already committed attempt. The scheduler completes that attempt
through the normal terminal transition.

### 4.3 Phase 3 schema version 1 remains sufficient

Phase 4 adds no table or index and does not bump `current_schema_version`.
The existing schema already contains:

- cron definition columns;
- scheduled/running/retry-wait run states;
- attempt lifecycle columns;
- the due-work index;
- the active-attempt index;
- the partial unique schedule-owned-run index.

The manual-run uniqueness rule is enforced by an immediate transaction and the
single-active-`jobud` v1 ownership rule. A new partial index would require an
application migration without adding correctness under the v1 single-writer
model.

### 4.4 Repositories remain private

Scheduler-oriented SQL belongs in private `*_repository_priv.*` files.
Repositories borrow `Database`, do not own transactions, and return only
project-owned domain values.

`Scheduler`, `CronEngine`, and the attempt-executor seam are public integration
contracts. Candidate rows, fairness state, transition helpers, and concrete
fake executors remain private or test-only.

### 4.5 Fake execution is test-only

No production source may automatically report a CLI or HTTP attempt as
successful. The fake executor lives under `test/support` and is linked only
into tests.

`jobud` may construct the production cron engine so recurring definitions can
be validated and persisted. It does not construct or start `Scheduler` until a
real executor is available in a later phase.

### 4.6 Time is sampled explicitly

Durable deadlines use `jb::core::UtcTimePoint`. In-process waits use
`jb::core::TimePoint`.

Each scheduling cycle samples `utc_now()` and `monotonic_now()` once. A future
UTC deadline is converted to a monotonic delay from that pair. Long sleeps are
capped at 60 seconds by default so wall-clock jumps are re-evaluated.

Scheduler-core tests call the deterministic cycle directly with
`FakeTimeSource`; they do not wait for a real `Timer`.

### 4.7 Useful Doxygen is mandatory

Every new or changed public declaration must have useful Doxygen documenting:

- ownership and borrowed lifetimes;
- owner-thread/event-loop requirements;
- accepted states and preconditions;
- callback and completion rules;
- whether inputs are copied, moved, or retained;
- durable transaction boundaries;
- retry/cancellation semantics;
- error behavior;
- whether a method may invoke callbacks synchronously.

Each new public header must compile as the first include in a dedicated test.
Stage completion includes its Doxygen and public-header test; documentation is
not postponed to the final stage.

### 4.8 Selective `[[nodiscard]]`

Keep the established policy:

- use `[[nodiscard]]` for `Result` values whose runtime failure must be handled,
  state-returning queries, and values that are meaningless when ignored;
- do not add it to `void` notification/cleanup calls such as
  `request_rescan()` or `stop()`;
- do not use it merely because a method returns a value.

### 4.9 Useful code body documentation is mandatory

Every new or changed code body block must have useful comments documenting what the code block does:

- focus on intent, rationale, ordering, invariants, and failure behavior;
- avoid comments that merely restarte the function name or individual statements;
- no body comment is needed when the name and implementation already the behavior clear.

## 5. Target and dependency structure

No new production target is required.

```text
core
 └── jobu
      ├── db
      ├── rpc
      ├── CronEngine / SystemCronEngine
      ├── Scheduler
      └── AttemptExecutor interface

db-sqlite
 └── jobu-sqlite

test support
 └── FakeAttemptExecutor
```

`jobu` remains free of direct `sqlite3`, libcurl, and native process types.
The scheduler works through `jb::db::Database` and project-owned interfaces.

The timezone implementation is private to `jobu`. It reads operating-system
IANA TZif data and exposes no TZif structure in public headers.

Do not base Phase 4 on `std::chrono::locate_zone`. The current Ubuntu CI uses a
toolchain older than libstdc++'s complete C++20 timezone implementation, while
libc++ still documents P0355 timezone work as partial. A small project-owned
TZif/POSIX-rule reader preserves the current compiler matrix and adds no new
third-party dependency:

- [libstdc++ C++20 library status](https://gcc.gnu.org/onlinedocs/libstdc++/manual/status.html#status.iso.2020)
- [libc++ C++20 status](https://libcxx.llvm.org/Status/Cxx20.html)
- [libc++ timezone design notes](https://libcxx.llvm.org/DesignDocs/TimeZone.html)

## 6. Proposed source layout

Add:

```text
src/jobu/
    attempt_executor.hpp
    cron.cpp
    cron.hpp
    cron_expression_priv.cpp
    cron_expression_priv.hpp
    cron_timezone_priv.cpp
    cron_timezone_priv.hpp
    retry_policy_priv.cpp
    retry_policy_priv.hpp
    scheduler.cpp
    scheduler.hpp
    scheduler_core_priv.cpp
    scheduler_core_priv.hpp
    scheduler_repository_priv.cpp
    scheduler_repository_priv.hpp
```

Change:

```text
src/jobu/
    CMakeLists.txt
    attribute_registry.cpp
    attribute_registry.hpp
    attempt.hpp
    attempt_repository_priv.cpp
    attempt_repository_priv.hpp
    domain_storage_priv.cpp
    domain_storage_priv.hpp
    job.hpp
    job_repository_priv.cpp
    job_repository_priv.hpp
    management.cpp
    management.hpp
    management_json.cpp
    management_rpc.cpp
    queue_repository_priv.cpp
    queue_repository_priv.hpp
    run_repository_priv.cpp
    run_repository_priv.hpp
    sqlite/sqlite_schema.cpp       # no DDL change; only if validation tests need clarification
```

Add tests:

```text
test/
    cron-expression-test.cpp
    cron-timezone-test.cpp
    jobu-cron-management-test.cpp
    retry-policy-test.cpp
    scheduler-repository-test.cpp
    scheduler-dispatch-test.cpp
    scheduler-fairness-test.cpp
    scheduler-retry-test.cpp
    scheduler-recurrence-test.cpp
    scheduler-run-now-test.cpp
    scheduler-cancellation-test.cpp
    scheduler-event-loop-test.cpp
    scheduler-integration-test.cpp
    jobu-attempt-executor-public-header-test.cpp
    jobu-cron-public-header-test.cpp
    jobu-scheduler-public-header-test.cpp
    support/fake_attempt_executor.cpp
    support/fake_attempt_executor.hpp
    support/fake_cron_engine.hpp
```

Tests may include private JobU headers only when directly verifying a private
algorithm or repository. Public boundary tests include only installed public
headers.

## 7. Public cron contract

Add `src/jobu/cron.hpp`.

```cpp
/** @file cron.hpp
 * @brief Defines dependency-independent cron validation and occurrence calculation.
 */
#pragma once

#include "error.hpp"
#include "job.hpp"
#include "result.hpp"
#include "time_source.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace jb::jobu {

class CronEngine {
public:
    virtual ~CronEngine() = default;

    [[nodiscard]] virtual auto validate(CronSchedule const& schedule) const
        -> jb::core::Result<void, jb::core::Error> = 0;

    [[nodiscard]] virtual auto next_after(
        CronSchedule const& schedule,
        jb::core::UtcTimePoint exclusive_lower_bound) const
        -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error> = 0;
};

class SystemCronEngine final : public CronEngine {
public:
    SystemCronEngine();
    ~SystemCronEngine() override;

    SystemCronEngine(SystemCronEngine const&) = delete;
    SystemCronEngine(SystemCronEngine&&) noexcept;
    auto operator=(SystemCronEngine const&) -> SystemCronEngine& = delete;
    auto operator=(SystemCronEngine&&) noexcept -> SystemCronEngine&;

    [[nodiscard]] auto validate(CronSchedule const& schedule) const
        -> jb::core::Result<void, jb::core::Error> override;

    [[nodiscard]] auto next_after(
        CronSchedule const& schedule,
        jb::core::UtcTimePoint exclusive_lower_bound) const
        -> jb::core::Result<jb::core::UtcTimePoint, jb::core::Error> override;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};

[[nodiscard]] auto next_cron_occurrences(
    CronEngine const& engine,
    CronSchedule const& schedule,
    jb::core::UtcTimePoint exclusive_lower_bound,
    std::size_t count)
    -> jb::core::Result<std::vector<jb::core::UtcTimePoint>, jb::core::Error>;

} // namespace jb::jobu
```

The actual header must fully document the abbreviated declarations above.

### 7.1 `CronEngine`

`CronEngine` is injectable because:

- management and scheduler tests need deterministic occurrence sequences;
- timezone loading must remain replaceable;
- later schedule-preview RPC handlers should depend on a project-owned API;
- implementation-specific timezone data must not leak into domain objects.

`validate()` parses both the expression and timezone. It does not retain a
reference to the supplied strings.

`next_after()` is strictly exclusive. Returning the lower-bound instant itself
is always a bug. It either returns one representable future UTC minute or a
stable error.

`next_cron_occurrences()` accepts `count` from 1 through 200 and repeatedly
calls `next_after()`. It is the reusable implementation seam for the later
`schedule.next` method, but Phase 4 does not register that RPC method.

### 7.2 `SystemCronEngine`

`SystemCronEngine`:

- is safe to construct before an `EventLoop`;
- performs no network access;
- lazily loads and caches immutable zone data;
- is owner-thread only in Phase 4;
- treats `UTC` specially without a file lookup;
- searches the platform zoneinfo roots defined in Section 9;
- does not automatically reload tzdata during the process lifetime;
- uses pimpl so public headers expose no parser/storage details.

Move support is allowed because the pimpl has no external borrowers. Copying is
disabled to avoid accidentally duplicating a potentially large timezone cache.

## 8. Cron syntax and calendar rules

### 8.1 Accepted syntax

Support exactly five fields:

```text
minute hour day-of-month month day-of-week
```

Accepted field syntax:

- `*`;
- one numeric value;
- comma-separated values;
- inclusive ranges such as `1-5`;
- steps on `*` or a range, such as `*/15` or `1-10/2`, except that the
  day-of-week field requires an explicit range and rejects wildcard steps;
- case-insensitive English month names `JAN` through `DEC`;
- case-insensitive weekday names `SUN` through `SAT`;
- aliases `@hourly`, `@daily`, and `@weekly`.

The aliases expand before normal field parsing:

- `@hourly` is `0 * * * *`;
- `@daily` is `0 0 * * *`;
- `@weekly` is `0 0 * * SUN`.

Ranges:

| Field | Values |
| --- | --- |
| minute | 0–59 |
| hour | 0–23 |
| day of month | 1–31 |
| month | 1–12 or JAN–DEC |
| day of week | 0–7 or SUN–SAT; both 0 and 7 mean Sunday |

Both numeric values `0` and `7` mean Sunday. Before normalizing them, inspect
the complete day-of-week field and reject it if numeric `0` and numeric `7`
both occur anywhere, including separate list members, range endpoints, or
stepped ranges. Weekday names do not count as either numeric convention.
Therefore `0-6` and `1-7` are valid full-week ranges, while `0-7`, `7-0`,
`0,7`, and `0-3,5-7` are invalid.

Weekday ranges move forward inclusively through the cyclic sequence
`SUN, MON, TUE, WED, THU, FRI, SAT` until reaching the right endpoint. A range
may cross the Saturday-to-Sunday boundary. For example:

- `FRI-MON` selects `FRI,SAT,SUN,MON`;
- `SUN-SAT`, `MON-SUN`, `0-6`, and `1-7` select all seven days;
- `SUN-SUN` selects Sunday only.

Apply a range step to this ordered expansion, anchored at the left endpoint.
For example, `FRI-MON/2` selects `FRI,SUN`. Equal endpoints always select one
day before applying the step.

Plain `*` selects all seven weekdays. Reject every wildcard step in the
day-of-week field, including `*/1`, because its starting weekday would be
implicit. Require an explicit range such as `SUN-SAT/2`, `MON-SUN/2`,
`0-6/2`, or `1-7/2` when stepping through weekdays. Normalize accepted Sunday
values to one canonical Sunday bit only after validating and expanding the
complete field.

Whitespace is one or more ASCII spaces or tabs between fields. Leading and
trailing ASCII whitespace is accepted and ignored.

Reject:

- seconds/sixth fields;
- empty list members;
- descending ranges in the minute, hour, day-of-month, and month fields;
- a zero step;
- wildcard steps in the day-of-week field;
- a day-of-week field containing both numeric `0` and numeric `7`;
- values outside their field;
- mixed text in numeric fields;
- unknown names;
- Quartz `?`, `L`, `W`, and `#`;
- macros other than the three listed above;
- embedded NUL;
- expressions longer than 512 bytes.

### 8.2 Matching semantics

The day-of-month and day-of-week fields use AND semantics even when both are
restricted. A calendar date must satisfy every field.

An impossible calendar combination, such as day 31 restricted to February,
parses successfully but `next_after()` eventually returns
`jobu.schedule.no_future_occurrence` after its bounded search horizon.

The expression operates at minute precision. Seconds and subseconds in the
exclusive UTC lower bound are preserved only for exclusivity; the returned
occurrence is aligned to a UTC minute after timezone conversion.

### 8.3 Search implementation

Store parsed fields as fixed bitsets in `cron_expression_priv.hpp`.

Do not scan UTC one minute at a time across years. Advance local calendar
components from largest to smallest:

1. choose the next allowed month;
2. choose a valid date satisfying month, day-of-month, and weekday;
3. choose the next allowed hour;
4. choose the next allowed minute;
5. map the candidate local minute through the timezone;
6. verify the mapped local fields still match after applying gap/overlap rules;
7. require the resulting UTC instant to be strictly after the lower bound.

Use `std::chrono::year_month_day` and `weekday` for Gregorian calendar
arithmetic; these parts of C++20 are available on the supported toolchains and
do not require the standard timezone database.

The search horizon is 400 Gregorian years from the local year containing the
lower bound. The Gregorian weekday/leap-year cycle repeats over that interval.
Exhaustion returns `jobu.schedule.no_future_occurrence`.

## 9. Timezone and DST implementation

### 9.1 Zone lookup

Validate timezone names before filesystem access:

- 1 through 255 bytes;
- valid UTF-8;
- no absolute path;
- no empty, `.` or `..` component;
- no backslash or NUL;
- canonical resolved file must remain below an accepted zoneinfo root.

Linux roots, in order:

```text
/usr/share/zoneinfo
/usr/share/lib/zoneinfo
```

The optional macOS stage adds and verifies:

```text
/var/db/timezone/zoneinfo
/usr/share/zoneinfo
```

The first valid matching root wins. Symlink aliases are allowed only when the
resolved target remains inside the same accepted root.

### 9.2 TZif parsing

The private reader supports TZif versions 1, 2, 3, and 4:

- checked big-endian counts and timestamps;
- bounded allocation before reading arrays;
- monotonic transition timestamps;
- valid type indexes;
- UTC offsets in the representable TZif range;
- transition abbreviations treated as metadata only;
- leap-second records parsed and ignored for JobU's POSIX/UTC scheduling;
- v2+ 64-bit section preferred over the v1 compatibility section;
- POSIX footer parsed for recurring transitions beyond the final explicit
  transition.

Reject malformed/truncated files with `jobu.schedule.invalid_timezone_data`.
The error detail may contain the zone name and parser phase, never raw file
bytes.

Cap:

- file size at 16 MiB;
- transition count at 1,000,000;
- local-time type count at 256;
- abbreviation bytes at 64 KiB.

### 9.3 POSIX footer

Support the POSIX forms emitted by normal IANA `zic` output:

- standard and optional daylight abbreviations;
- signed standard/daylight offsets;
- `Mmonth.week.weekday/time` transition rules;
- `Jn/time` and `n/time` forms;
- omitted daylight offset using the POSIX one-hour default.

The parser need not accept arbitrary user-supplied POSIX timezone strings.
It consumes only a validated TZif footer.

### 9.4 Local-to-UTC mapping

For a valid local minute:

- one UTC mapping: use it;
- overlap/two mappings: use the earlier UTC occurrence;
- gap/no mapping: add the exact transition gap to the local candidate, then map
  that shifted local minute.

The gap rule is not hard-coded to one hour. It must handle half-hour and
historical non-hour transitions.

After shifting a gap candidate, do not re-run the cron field matcher. The
agreed behavior is "shift the intended local occurrence forward by the gap",
not "find a different expression match after the gap".

### 9.5 Deterministic timezone tests

Parser unit tests use generated minimal TZif byte fixtures committed as test
source data, not copies of system zone files.

Linux integration tests additionally use installed zones:

- `UTC`;
- `Europe/Tallinn`;
- `America/New_York`;
- `Australia/Lord_Howe`.

If a named integration zone is absent, the test must fail with a clear
dependency message; silently skipping DST coverage is not acceptable on the
supported CI images.

## 10. Attribute changes

Extend `StandardAttributeRegistry` from 9 to 11 definitions:

| Name | Type | Default | Constraints |
| --- | --- | --- | --- |
| `retry.multiplier` | Number | `2.0` | finite, 1.0 through 100.0 |
| `retry.jitter` | Number | `0.0` | finite, 0.0 through 1.0 |

Keep the existing definitions and meanings:

- `retry.max_attempts`;
- `retry.strategy`;
- `retry.initial_delay`;
- `retry.max_delay`;
- `retry.mode`;
- timeout and output attributes.

Both new attributes support daemon-default, queue-default, and job scopes.

The persisted attribute decoder must materialize these built-in defaults when
reading a Phase 3 document that contains the previous complete nine-attribute
set. It must not reapply current daemon or queue defaults.

Cross-field validation remains:

```text
retry.max_delay >= retry.initial_delay
```

No runner-specific retry selector is added in Phase 4. The fake executor
provides the retryable-versus-terminal classification through the executor
completion contract.

## 11. Public attempt-executor contract

Add `src/jobu/attempt_executor.hpp`.

```cpp
/** @file attempt_executor.hpp
 * @brief Defines asynchronous execution input and completion contracts used by Scheduler.
 */
#pragma once

#include "attempt.hpp"
#include "error.hpp"
#include "job.hpp"
#include "result.hpp"
#include "run.hpp"
#include "time_source.hpp"

#include <functional>
#include <optional>

namespace jb::jobu {

struct AttemptKey {
    jb::core::Uuid run_id;
    AttemptNumber  attempt_number{1};

    auto operator==(AttemptKey const&) const -> bool = default;
};

enum class FailureDisposition : std::uint8_t {
    Terminal,
    Retryable,
};

struct AttemptStartRequest {
    AttemptKey                   key;
    jb::core::Uuid               job_id;
    jb::core::Uuid               queue_id;
    JobType                      type{JobType::Cli};
    AttributeSet                 attributes;
    jb::rpc::JsonValue           payload;
    jb::core::UtcTimePoint       started_at;
};

struct AttemptCompletion {
    AttemptKey                              key;
    AttemptOutcome                         outcome{AttemptOutcome::Failed};
    std::optional<FailureDisposition>       failure_disposition;
    std::optional<jb::core::UtcTimePoint>   retry_not_before;
    jb::rpc::JsonValue                      result;
};

using AttemptCompletionHandler = std::function<void(AttemptCompletion)>;

class AttemptExecutor {
public:
    virtual ~AttemptExecutor() = default;

    [[nodiscard]] virtual auto is_available(JobType type) const noexcept -> bool = 0;

    [[nodiscard]] virtual auto start(
        AttemptStartRequest request,
        AttemptCompletionHandler completion)
        -> jb::core::Result<void, jb::core::Error> = 0;

    [[nodiscard]] virtual auto cancel(AttemptKey const& key)
        -> jb::core::Result<void, jb::core::Error> = 0;
};

} // namespace jb::jobu
```

The real header must fully document all fields and methods.

### 11.1 Ownership

`AttemptStartRequest` is owning. The scheduler moves the immutable run
snapshot into `start()`. The executor may retain or move any request field.

The completion handler is owning and may be retained until completion or
cancellation. It must be invoked:

- exactly once after `start()` returns success;
- never when `start()` returns failure;
- on the executor/scheduler owner event-loop thread;
- not synchronously from inside `start()`;
- with the same `AttemptKey`.

The handler must not be invoked after the executor has been destroyed.
`Scheduler` must outlive its borrowed executor.

### 11.2 Completion consistency

Validate before persistence:

| Outcome | Required/forbidden fields |
| --- | --- |
| `Succeeded` | no failure disposition; no retry deadline |
| `Failed` | failure disposition required; retry deadline allowed only when retryable |
| `Cancelled` | no failure disposition; no retry deadline |
| `Interrupted` | reserved for Phase 7 recovery; fake/normal executors must not emit it in Phase 4 |

`result` must be a JSON object whose deterministic serialization is at most
256 KiB. It must contain only user-safe metadata.

An executor `start()` failure is converted to a completed failed attempt with a
terminal disposition and a safe result containing the executor error code.
It is not a scheduler-fatal error.

### 11.3 Availability

The scheduler never marks a due run failed merely because no executor for its
`JobType` is installed. Unavailable types are left pending and excluded from
capacity/fairness accounting.

This permits Phase 5 to enable HTTP execution before the Phase 6 CLI executor
exists.

## 12. Public scheduler contract

Add `src/jobu/scheduler.hpp`.

```cpp
/** @file scheduler.hpp
 * @brief Defines JobU's centralized owner-thread scheduler.
 */
#pragma once

#include "attempt_executor.hpp"
#include "cron.hpp"
#include "error.hpp"
#include "object.hpp"
#include "result.hpp"
#include "signal.hpp"
#include "time_source.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace jb::db {
class Database;
}

namespace jb::jobu {

class AttributeRegistry;

struct SchedulerOptions {
    std::uint32_t         cli_concurrency{4};
    std::uint32_t         http_concurrency{16};
    std::size_t           candidate_batch_size{200};
    std::chrono::seconds  wall_clock_recheck{60};
};

enum class SchedulerState : std::uint8_t {
    Stopped,
    Running,
    Failed,
};

enum class CancelDisposition : std::uint8_t {
    Completed,
    Requested,
};

struct CancelRunResult {
    JobRun             run;
    CancelDisposition  disposition{CancelDisposition::Completed};
};

class Scheduler final : public jb::core::Object {
public:
    Scheduler(
        jb::db::Database& database,
        AttributeRegistry const& attributes,
        CronEngine const& cron,
        jb::core::UuidGenerator& uuid_generator,
        jb::core::TimeSource& time_source,
        AttemptExecutor& executor,
        SchedulerOptions options = {},
        jb::core::Object* parent = nullptr);

    ~Scheduler() override;

    Scheduler(Scheduler const&) = delete;
    Scheduler(Scheduler&&) = delete;
    auto operator=(Scheduler const&) -> Scheduler& = delete;
    auto operator=(Scheduler&&) -> Scheduler& = delete;

    [[nodiscard]] auto start()
        -> jb::core::Result<void, jb::core::Error>;

    void stop();
    void request_rescan();

    [[nodiscard]] auto cancel_run(jb::core::Uuid const& run_id)
        -> jb::core::Result<CancelRunResult, jb::core::Error>;

    [[nodiscard]] auto state() const noexcept -> SchedulerState;
    [[nodiscard]] auto failure() const -> std::optional<jb::core::Error>;

    jb::core::Signal<jb::core::Error> failed;

private:
    struct Private;
    std::unique_ptr<Private> _data;
};

} // namespace jb::jobu
```

The real header must fully document the abbreviated contract.

### 12.1 Construction and lifetimes

`Scheduler` borrows:

- one already-open, idle `Database`;
- the same immutable `AttributeRegistry` used by management and repositories;
- a `CronEngine`;
- a `UuidGenerator`;
- a `TimeSource`;
- an `AttemptExecutor`.

All must outlive the scheduler. All scheduler methods except destruction run on
the database/event-loop owner thread. The scheduler is non-copyable and
non-movable because callbacks and repositories borrow fixed addresses.

Construction validates options but performs no query and invokes no callback.
Invalid options are stored as an initialization error returned by `start()`,
matching the established `ManagementService` approach.

### 12.2 `start()`

`start()`:

1. verifies the database is open and owned by the calling thread;
2. rejects zero global limits, zero batch size, or nonpositive recheck time;
3. verifies no persisted run/attempt is already `running`;
4. rebuilds capacity and manual-barrier state from non-terminal rows;
5. enters `Running`;
6. performs an immediate scheduling cycle;
7. arms the next wake.

Phase 4 returns `jobu.scheduler.recovery_required` if running durable state
exists. Phase 7 replaces that refusal with startup recovery.

Calling `start()` while already running is an idempotent success. Calling it
after `Failed` returns the stored failure; a failed scheduler is not restartable
in place.

### 12.3 `stop()`

`stop()` disarms future dispatch and the wake timer. It does not cancel
executors or rewrite durable running state in Phase 4.

It is safe and idempotent when already stopped. If active attempts exist, their
completion handlers remain valid until scheduler destruction, but completions
must not trigger new dispatch. The Phase 4 integration suite normally stops
only after fake work drains.

Phase 7 strengthens this into immediate daemon shutdown semantics.

### 12.4 `request_rescan()`

This method is an owner-thread notification that durable management state may
have changed. Multiple calls before the next cycle coalesce.

When running it cancels/replaces a later timer with an immediate event-loop
cycle. When stopped or failed it does nothing. It invokes no callback
synchronously.

Future RPC handlers call this only after a successful mutation. Phase 4 direct
service tests call it explicitly after create/update/resume/Run Now.

### 12.5 Failure signal

A database, invariant, executor-protocol, UUID, or cron-successor failure after
`start()` moves the scheduler once to `Failed`, stops its timer, prevents all
new starts, stores the first safe `Error`, and emits `failed` once.

Phase 4 does not terminate `jobud`; the scheduler itself still fails closed.
Phase 7 connects this signal to active-work termination and process exit.

## 13. Management API changes

### 13.1 Required cron dependency

Change `ManagementService` construction to borrow `CronEngine`:

```cpp
ManagementService(
    jb::db::Database& database,
    AttributeRegistry const& attributes,
    CronEngine const& cron,
    jb::core::UuidGenerator& uuid_generator,
    jb::core::TimeSource& time_source,
    AttributeSet daemon_defaults = {});
```

Update every call site and test explicitly. Do not retain an overload that
silently rejects cron, because a Phase 4 `ManagementService` must have one
well-defined schedule policy.

`jobud` constructs `SystemCronEngine` before `ManagementService`.

### 13.2 Recurring creation

`create_job()` accepts `CronSchedule`.

Inside the existing immediate creation transaction:

1. validate the expression and timezone;
2. compute the first occurrence strictly after `time_source.utc_now()`;
3. create the definition with the requested cron schedule;
4. create one schedule-owned scheduled run at that occurrence;
5. snapshot the definition revision, queue, type, priority, complete
   attributes, and payload;
6. store the idempotency record;
7. commit.

The same idempotency key and normalized request replays the original definition
and original first occurrence. A changed cron expression/timezone conflicts.

Remove active use of `jobu.schedule.cron_unavailable`; retain compatibility
mapping only if older client tests or stored errors require it.

### 13.3 Recurring update

Extend `update_job()`:

- validate a replacement cron schedule before beginning writes;
- an unstarted scheduled schedule-owned run is refreshed atomically;
- its new planned/runnable time is the first occurrence strictly after the
  update transaction's sampled current time;
- a running or retry-waiting recurring run retains its old revision and
  snapshot;
- in that case update only the definition; the terminal-completion transaction
  later creates a successor from the newest definition;
- changing cron to once is allowed only while the current schedule-owned run is
  scheduled and has no attempt;
- changing once to cron follows the same unstarted requirement;
- a one-time definition remains immutable after its first attempt starts.

For a running/retry-waiting recurring run, type, payload, priority, attributes,
and schedule changes apply only to its successor.

### 13.4 Run Now

Add:

```cpp
struct RunNowRequest {
    jb::core::Uuid               job_id;
    std::optional<std::string>   idempotency_key;
};

[[nodiscard]] auto run_now(RunNowRequest request)
    -> jb::core::Result<JobRun, jb::core::Error>;
```

`run_now()` uses one immediate transaction and is allowed only when:

- the definition exists and is not deleted;
- its schedule-owned run exists in `scheduled`;
- that run's `planned_at` is strictly in the future;
- no run for the job is `running` or `retry_wait`;
- no non-terminal manual run already exists;
- the owning queue is not deleted; queue suspension does not prevent creation.

The operation:

- snapshots the current definition revision and complete execution values;
- creates a manual, non-schedule-owned run;
- sets `planned_at` and `runnable_at` to the sampled current time;
- preserves the schedule-owned run unchanged;
- stores/replays idempotency under method `job.run_now` and scope `job_id`.

A manual run may be created for a suspended job and bypasses job suspension at
dispatch. It never bypasses queue suspension.

Do not register the public RPC/CLI method in Phase 4.

## 14. Scheduler repository model

Add a private `SchedulerRepository` rather than expanding
`RunRepository` into a scheduler service.

It borrows `Database` and `AttributeRegistry`, owns no transaction, and
provides narrow SQL-backed operations.

### 14.1 Private values

```cpp
struct QueueRuntime {
    jb::core::Uuid  id;
    std::uint32_t   weight{1};
    std::uint32_t   concurrency_limit{1};
};

struct CapacityUsage {
    std::uint32_t cli_running{0};
    std::uint32_t http_running{0};
    std::uint32_t queue_slots{0};
};

struct DispatchCandidate {
    JobRun         run;
    JobState       job_state{JobState::Active};
    QueueState     queue_state{QueueState::Active};
};

struct DispatchContext {
    JobRun         run;
    JobState       job_state{JobState::Active};
    Queue          queue;
    AttemptNumber  next_attempt{1};
};

struct RecurrenceContext {
    JobDefinition  definition;
    JobRun         completed_run;
};
```

These remain private and require no Doxygen/public-header tests.

### 14.2 Queries

Provide:

- `list_runtime_queues(limit, after_id)`;
- `list_capacity_rows(limit, after_run_id)`;
- `list_runnable(queue_id, type, now, limit)`;
- `earliest_future_runnable(type, now)`;
- `find_dispatch_context(run_id, now)`;
- `next_attempt_number(run_id)`;
- `mark_dispatch_running(run_id, expected_state, started_at)`;
- `complete_attempt(...)`;
- `set_run_retry_wait(...)`;
- `set_run_terminal(...)`;
- `insert_recurring_successor(...)`;
- `find_run_for_cancel(run_id)`;
- `find_active_attempt(run_id)`;
- `cancel_pending_run(...)`;
- `has_non_terminal_manual_run(job_id)`;
- `has_running_or_retrying_run(job_id)`;
- `complete_drained_suspensions(queue_id, job_id, now)`;
- `has_any_running_state()`.

Every list is bounded and keyset-paginated. Startup state reconstruction pages
until end; one query never has an unbounded result.

### 14.3 Runnable SQL

Candidates must satisfy:

- queue state is `active`;
- run state is `scheduled` or `retry_wait`;
- `runnable_at_us <= now`;
- type matches the requested executor resource;
- for scheduled-origin work, job state is `active`;
- for manual-origin work, job state may be active, suspending, or suspended;
- job and queue are not deleted;
- a schedule-owned run has no non-terminal manual sibling;
- no running attempt exists for the run.

Order inside one queue:

```sql
ORDER BY
    priority DESC,
    runnable_at_us ASC,
    planned_at_us ASC,
    id ASC
```

The final UUID order is binary byte order.

Fetch more than one candidate so a row found ineligible after attribute/capacity
decoding does not conceal later work. `candidate_batch_size` bounds each fetch.

### 14.4 Revalidation

`find_dispatch_context()` repeats all state, due-time, manual-barrier, and
attempt checks inside the immediate dispatch transaction. It must not trust a
candidate returned before another management mutation.

A normal loss of eligibility is not fatal and starts no work. The scheduler
refreshes its state and continues.

A cross-table impossibility, malformed persisted document, duplicate running
attempt, or missing required schedule-owned relationship is
`jobu.storage.invariant` and fails the scheduler.

## 15. Dispatch and executor-start transitions

### 15.1 Capacity admission

Before the transaction:

- a global slot for the run type must be available;
- one combined queue slot must be available;
- the executor must report the type available.

Inside the transaction, revalidate state and recompute the relevant queue
running/blocking-retry count. In-memory counters optimize selection but do not
authorize a start.

### 15.2 Durable transition

The transaction:

1. determines `MAX(attempt_number) + 1` with checked overflow;
2. inserts `JobAttempt`:
   - `due_at = previous runnable_at`;
   - `started_at = now`;
   - `state = running`;
   - no completion/outcome/result;
3. changes the run from `scheduled` or `retry_wait` to `running`;
4. sets `started_at` only when the run has never started;
5. clears no immutable snapshot field;
6. commits.

Zero affected run rows means eligibility changed and rolls back normally.

After commit, increment in-memory capacity and call `AttemptExecutor::start()`
with an owning snapshot.

### 15.3 Start failure

If `start()` returns an error:

- do not roll back the already committed running record;
- synthesize a failed, terminal `AttemptCompletion`;
- include only the executor error code and safe message in the result object;
- persist through the same completion path as an asynchronous result;
- release capacity only after completion commit.

## 16. Retry policy

### 16.1 Policy decoding

Add private:

```cpp
struct RetryPolicy {
    std::uint64_t        max_attempts{1};
    RetryStrategy        strategy{RetryStrategy::Fixed};
    jb::core::Duration   initial_delay{};
    jb::core::Duration   max_delay{};
    double               multiplier{2.0};
    double               jitter{0.0};
    RetryMode            mode{RetryMode::Reschedule};
};

[[nodiscard]] auto retry_policy_from_attributes(AttributeSet const&)
    -> jb::core::Result<RetryPolicy, jb::core::Error>;

[[nodiscard]] auto retry_delay(
    RetryPolicy const&,
    jb::core::Uuid const& run_id,
    AttemptNumber next_attempt)
    -> jb::core::Result<jb::core::Duration, jb::core::Error>;
```

`RetryStrategy` and `RetryMode` may remain private because their durable/public
representation is the registered string attribute.

### 16.2 Delay

For the next attempt number `n`, where the first retry is `n = 2`:

```text
fixed:
    base = initial_delay

exponential:
    base = min(initial_delay * multiplier^(n - 2), max_delay)
```

Use saturating checked arithmetic and cap the base at `max_delay`.

Apply deterministic symmetric jitter:

```text
fraction = stable_fraction(run_uuid, next_attempt) in [0, 1]
factor   = 1 - jitter + (2 * jitter * fraction)
delay    = clamp(base * factor, 0, max_delay)
```

Use a documented stable integer mixing function over the run UUID bytes and
attempt number. This is not cryptographic. It makes retries reproducible in
tests and across scheduler rebuilds while spreading different runs.

When `initial_delay` is zero, the result remains zero.

### 16.3 Retry decision

Retry only when:

- outcome is `Failed`;
- executor disposition is `Retryable`;
- current attempt number is below `max_attempts`;
- the run/job/queue has not been deleted or explicitly cancelled.

The retry due instant is:

```text
max(completed_at + calculated_delay, executor.retry_not_before)
```

An executor `retry_not_before` is not capped by `retry.max_delay`; this is how
future HTTP 429/503 `Retry-After` is honored.

Retry transition:

- complete the attempt as failed;
- change run to `retry_wait`;
- set `runnable_at` to the retry due instant;
- keep the original run UUID, planned time, start time, and snapshot;
- do not create a new run or pending attempt.

When retry is not allowed, terminally fail the run.

### 16.4 Blocking versus reschedule

While queue and job permit execution:

- `blocking`: a `retry_wait` run consumes one queue combined slot;
- `reschedule`: it consumes no queue slot.

Neither mode consumes a global CLI/HTTP slot while waiting.

A blocking retry belonging to a suspending/suspended job or queue releases its
queue slot until resumed. Lowering capacity never cancels existing running or
blocking-retry occupancy; it only stops new admission.

## 17. Completion transaction

For every accepted executor completion:

1. validate the key and completion shape;
2. find the matching durable running attempt/run;
3. sample one completion UTC time;
4. begin one immediate transaction;
5. complete the attempt with outcome/result;
6. either:
   - mark the run `retry_wait` with a new `runnable_at`; or
   - mark it terminal with `completed_at` and result summary;
7. if a terminal schedule-owned recurring run, create its successor;
8. complete any queue/job `suspending -> suspended` transition whose running
   count is now zero;
9. commit;
10. only then release in-memory capacity;
11. request another scheduler cycle.

No output row is written in Phase 4. Real runner phases extend the same
transaction with capture persistence.

If persistence fails, capacity remains logically occupied, the scheduler enters
`Failed`, and no dependent work starts.

## 18. Recurring successor rules

### 18.1 Definition lookup

When a schedule-owned run becomes terminal, look up the current job definition
inside the completion transaction.

- deleted definition: no successor;
- one-time definition: no successor;
- recurring definition: validate its current cron schedule and create a
  successor;
- missing/malformed live definition: invariant failure.

The successor snapshots the newest definition revision and fields, not the
completed run's old snapshot.

### 18.2 Next time

Calculate:

```text
lower_bound = max(completed_or_cancelled_at, completed_run.planned_at)
next        = cron.next_after(current_definition.schedule, lower_bound)
```

This:

- never recreates the cancelled/completed occurrence;
- coalesces every missed historical occurrence;
- creates exactly one future occurrence;
- applies schedule changes made while the old run was running/retrying.

### 18.3 Atomicity and uniqueness

Terminalizing the old run and inserting its successor occur in the same
transaction. The existing partial unique index remains the final database
backstop.

A friendly pre-check maps a conflict to `jobu.run.schedule_conflict`. A raw
constraint error is never returned as the normal domain result.

The successor receives a new UUID from the injected generator. UUID exhaustion
or cron failure aborts the entire completion transaction and fails the
scheduler; the old run remains durably running for Phase 7 recovery.

## 19. Queue capacity and weighted arbitration

### 19.1 Capacity counters

Maintain:

- total running CLI attempts;
- total running HTTP attempts;
- per-queue running attempts;
- per-queue active blocking-retry occupancy.

Rebuild them from durable state in bounded pages at `start()` and after any
suspected drift. Update them only after successful transitions.

### 19.2 Separate type fairness

Maintain independent smooth weighted-round-robin credit maps for CLI and HTTP.

For each dispatch opportunity of one type:

1. form the currently eligible queue set for that type;
2. reset removed/ineligible queue credit to zero so idle queues do not bank
   unbounded credit;
3. add each eligible queue's positive weight to its current credit;
4. choose the highest credit, breaking ties by queue UUID;
5. subtract the total eligible weight from the chosen queue;
6. dispatch at most one candidate from that queue;
7. repeat while type and queue capacity permit.

Weight 2 must receive approximately twice the dispatch opportunities of weight
1 under sustained equivalent eligibility.

### 19.3 Coupled queue limit

CLI and HTTP have separate global capacity but share the queue's combined
limit.

To prevent a fixed type order from monopolizing a queue with limit 1, the
scheduler alternates the first type considered on each outer dispatch round:

```text
round 1: CLI, HTTP
round 2: HTTP, CLI
```

Each type dispatches at most one item before the next type is considered.
Continue rounds until neither type makes progress.

The alternating token is in-memory only and resets deterministically on
`start()`.

### 19.4 Strict job priority

Inside one queue and type, priority is absolute:

1. higher priority;
2. earlier runnable time;
3. earlier planned time;
4. lower UUID bytes.

Do not implement priority aging. A continuously runnable high-priority stream
may starve lower priority work in the same queue.

## 20. Wake scheduling

After a cycle, compute the earliest relevant future `runnable_at` among:

- scheduled runs eligible when due;
- retry-wait runs;
- currently unavailable-type work only if that executor type is available.

If immediately eligible work exists but capacity is full, no busy timer is
armed; executor completion or management notification causes the rescan.

For a future UTC deadline:

```text
utc_delay = max(deadline - sampled_utc_now, 0)
delay     = min(utc_delay, wall_clock_recheck)
wake_at   = sampled_monotonic_now + delay
```

Use one non-repeating `Timer`. Replacing an existing deadline stops/restarts
that timer. Timer failure or absence of an owning event loop makes
`Scheduler::start()` fail.

When a wall clock jumps forward, the capped reevaluation discovers overdue
work. When it jumps backward, the next cycle recomputes a longer monotonic
delay without changing persisted deadlines.

## 21. Run Now barrier

The scheduler blocks a schedule-owned candidate whenever the same job has a
non-terminal manual run.

The barrier applies while the manual run is:

- scheduled;
- running;
- retry-waiting.

After the manual run becomes terminal, the completion transaction and
subsequent rescan release the barrier. The schedule-owned run retains its
original planned/runnable time and may then be overdue.

Manual work:

- bypasses job active/suspended state;
- does not bypass queue active state;
- obeys normal global and queue capacity;
- participates in normal priority/fairness selection;
- follows the same retry policy snapshot;
- never creates a recurring successor.

Startup barrier reconstruction is implemented from durable non-terminal rows.
Phase 7 adds repair behavior; Phase 4 fails on impossible duplicate manual
runs.

## 22. Cancellation

### 22.1 Pending or retry-wait run

`Scheduler::cancel_run()` performs one immediate transaction:

- changes scheduled/retry-wait to `cancelled`;
- writes completion time and `{"reason":"cancelled"}`;
- if it is schedule-owned recurring work, inserts its next future occurrence;
- completes drained suspensions;
- commits and requests a rescan.

Return `CancelDisposition::Completed` and the terminal run.

### 22.2 Running run

For a run correlated to an active executor:

- call `executor.cancel(key)`;
- on success record an in-memory cancellation request;
- return `CancelDisposition::Requested` and the still-running run;
- when the executor completes, persist `Cancelled` regardless of whether the
  operation happened to exit normally during cancellation;
- retain capacity until completion commits.

Repeated cancellation while the request is in memory returns `Requested`
without invoking the executor again.

If executor cancellation fails, return its safe error and leave the run
running. Do not enter scheduler-failed state unless the executor violates its
completion protocol.

### 22.3 Invalid cancellation

Return stable errors for:

- run not found;
- already terminal run;
- a persisted running run not owned by this scheduler instance
  (`jobu.scheduler.recovery_required`);
- invalid cross-table state.

## 23. Fake executor and deterministic scheduler harness

`test/support/FakeAttemptExecutor` implements the public executor contract and:

- independently enables CLI and HTTP types;
- records owning start requests in order;
- exposes pending keys;
- completes a selected key only when the test asks;
- can return a configured start error;
- can return a configured cancel error;
- records cancel calls;
- rejects duplicate completion and unknown keys in the test helper itself.

It invokes completions on the test/scheduler thread and never starts a thread.

`FakeCronEngine`:

- validates configured schedules;
- returns a configured strictly increasing sequence;
- records every lower bound;
- can inject validation or next-occurrence errors.

Scheduler-core tests instantiate the private core directly with:

- temporary SQLite;
- `FakeTimeSource`;
- `SequenceUuidGenerator`;
- fake cron;
- fake executor.

They call one explicit `process_cycle()` after advancing fake time. The public
event-loop adapter has a smaller focused test proving notification coalescing
and timer arming.

## 24. Stable errors

Add at least:

| Code | Category | Meaning |
| --- | --- | --- |
| `jobu.schedule.invalid_expression` | InvalidArgument | cron syntax or field value is invalid |
| `jobu.schedule.invalid_timezone` | InvalidArgument | timezone name cannot be resolved safely |
| `jobu.schedule.invalid_timezone_data` | Internal | resolved TZif/POSIX data is malformed |
| `jobu.schedule.no_future_occurrence` | InvalidArgument | no matching occurrence exists in the bounded Gregorian horizon |
| `jobu.schedule.out_of_range` | ResourceExhausted | occurrence cannot be represented as `UtcTimePoint` |
| `jobu.retry.invalid_policy` | Internal | materialized persisted retry attributes violate the registry contract |
| `jobu.attempt.number_exhausted` | ResourceExhausted | next positive attempt number cannot be represented |
| `jobu.executor.invalid_completion` | Internal | executor completion violates its contract |
| `jobu.scheduler.invalid_options` | InvalidArgument | global limits, batch size, or wake cap is invalid |
| `jobu.scheduler.invalid_state` | Conflict | operation is not valid in current scheduler state |
| `jobu.scheduler.recovery_required` | Conflict | durable running work requires Phase 7 recovery |
| `jobu.scheduler.failed` | Internal | scheduler has already failed closed |
| `jobu.run.not_found` | NotFound | run ID does not exist |
| `jobu.run.state_conflict` | Conflict | run state forbids dispatch/cancellation |
| `jobu.run.manual_conflict` | Conflict | Run Now barrier/precondition is not satisfied |

Preserve existing database errors when no more precise domain mapping applies.
Never put payloads, attribute documents, executor result bodies, or secret
values in `Error::detail`.

## 25. Testing requirements

### 25.1 Cron expression

Cover:

- every field boundary;
- lists, ranges, steps, aliases, and names;
- Sunday `0`/`7` equivalence when either convention is used independently;
- acceptance and equivalence of `SUN-SAT`, `MON-SUN`, `0-6`, and `1-7` as
  full-week ranges;
- rejection of any complete day-of-week field that uses both numeric `0` and
  numeric `7`, including `0-7`, `7-0`, `0,7`, and `0-3,5-7`;
- forward cyclic ranges, including `FRI-MON` as `FRI,SAT,SUN,MON` and numeric
  equivalents under either Sunday convention;
- `SUN-SUN` as a Sunday-only range;
- stepped cyclic ranges anchored at the left endpoint, including
  `FRI-MON/2` as `FRI,SUN`;
- plain `*` as all weekdays, rejection of every weekday wildcard step, and
  distinct anchors for explicit ranges such as `SUN-SAT/2` and `MON-SUN/2`;
- case-insensitive names;
- AND day semantics, including Friday the 13th;
- leap days, month lengths, century/non-century leap years;
- impossible calendar combinations and horizon exhaustion;
- strict exclusivity at exact minute and sub-minute bounds;
- all rejected syntax classes and size limits.

### 25.2 Timezone

Cover:

- valid TZif v1/v2/v3/v4 fixtures;
- 32-bit versus 64-bit section selection;
- malformed counts, indexes, offsets, transitions, and footer;
- symlink/root traversal rejection;
- UTC fixed zone;
- spring gap shift by one hour;
- fall overlap earlier occurrence;
- Lord Howe half-hour transition;
- POSIX footer after the last explicit transition;
- dates before the first transition;
- out-of-range conversion.

### 25.3 Attributes and retry

Cover:

- both new defaults and every range/type failure;
- old nine-value document compatibility;
- fixed and exponential sequences;
- multiplier and max cap;
- deterministic jitter bounds and repeatability;
- overflow saturation;
- max-attempt exhaustion;
- retryable versus terminal completion;
- executor retry deadline later/earlier than computed due time;
- zero-delay retry;
- blocking/reschedule occupancy.

### 25.4 Recurring management

Cover:

- cron create and first occurrence;
- invalid expression/timezone before writes;
- atomic definition/run/idempotency creation;
- same-key replay;
- update of an unstarted recurring run;
- update while running/retry-wait changes only the definition;
- once-to-cron and cron-to-once preconditions;
- queue/default snapshot behavior;
- existing JSON and RPC cron round trip;
- no regression in one-time behavior.

### 25.5 Dispatch

Cover:

- transaction committed before fake start observation;
- no start on any injected pre-commit/commit failure;
- executor start failure completed normally;
- one running attempt per run;
- attempt-number monotonicity and exhaustion;
- strict in-queue order;
- unavailable type remains scheduled;
- lowering capacity prevents only later starts;
- state change between selection and transaction causes a normal skip.

### 25.6 Fairness and capacity

Cover:

- queue weights 1:1, 1:2, and 2:5 over long deterministic runs;
- no credit banking while idle;
- deterministic UUID tie-breaking;
- separate CLI/HTTP fairness;
- alternating type order under a combined queue limit of 1;
- per-queue and both global limits;
- dynamic suspend/resume and weight/concurrency updates;
- blocking retry occupancy;
- suspended blocking retry slot release.

### 25.7 Completion and recurrence

Cover:

- success, terminal failure, retry, cancellation;
- atomic attempt/run transition;
- capacity release only after commit;
- retry uses the same run and a new attempt;
- recurring successor uses newest definition revision/snapshot;
- missed occurrences coalesce;
- cancellation lower bound uses max(now, planned);
- no successor for once/manual/deleted definition;
- UUID/cron/commit failure leaves old run running and fails scheduler;
- partial unique index remains satisfied.

### 25.8 Run Now

Cover every precondition:

- future schedule required;
- overdue schedule rejected;
- running/retrying job rejected;
- duplicate manual run rejected;
- suspended job accepted;
- suspended queue creates but does not dispatch;
- manual barrier blocks scheduled work;
- terminal manual run releases barrier;
- schedule-owned time is unchanged;
- same/different idempotency request behavior.

### 25.9 Cancellation

Cover:

- scheduled and retry-wait immediate cancellation;
- running asynchronous cancellation;
- repeated running cancellation;
- executor cancel failure;
- terminal-run conflict;
- recurring successor;
- queue/global capacity retained until completion commit;
- unknown or recovered-running run errors.

### 25.10 Event-loop adapter

Cover:

- immediate first cycle;
- one timer for earliest future due time;
- 60-second wall reevaluation cap;
- earlier notification replaces later wake;
- notifications coalesce;
- callback after stop does not dispatch;
- failure signal emitted once;
- no real wait longer than a small test bound.

### 25.11 Full regression

Every Linux stage runs its focused tests and:

```sh
cmake --build .bld
ctest --test-dir .bld/test --output-on-failure
```

Also configure/build/test with:

```sh
-DJB_BUILD_SQLITE_DRIVER=OFF
```

Cron and backend-independent scheduler contracts must continue to compile when
SQLite support is disabled. SQLite-backed scheduler tests and `jobud` remain
conditional on `jobu-sqlite`.

## 26. Reviewable implementation sequence

Every stage is a separate approval, implementation, validation, and review
boundary. Codex implements exactly one stage, reports changed files and
focused/full validation, and waits for explicit approval.

Linux implementation is complete before optional macOS work begins.

### Stage 4.1: Cron expression parser and local calendar search <- DONE

- add fully documented `cron.hpp` with `CronEngine`;
- implement five-field parsing, aliases, bitsets, cyclic weekday ranges,
  numeric `0`/`7` exclusivity, weekday-specific step rules, AND day semantics,
  and dependency-free local calendar candidate search;
- add direct public-header coverage;
- add complete expression/calendar unit tests using a fixed UTC mapping;
- do not read zoneinfo or change management behavior.

Exit: expressions can be validated and their next matching local calendar
minute found deterministically.

### Stage 4.2: Checked TZif and POSIX-footer reader on Linux <- DONE

- add private timezone file/name validation and cache values;
- implement bounded TZif v1–v4 parsing;
- implement the required POSIX footer grammar;
- add generated byte-fixture tests for valid, malformed, and future-rule data;
- keep it disconnected from the public cron engine.

Exit: Linux IANA zone files can be converted into checked immutable transition
data without a new dependency.

### Stage 4.3: Linux DST-aware `SystemCronEngine` <- DONE

- connect expression search to timezone lookup and local-to-UTC mapping;
- implement gap shift, earlier overlap, strict exclusive UTC results, and
  400-year exhaustion;
- add Linux system-zone integration tests;
- complete all `SystemCronEngine` Doxygen and public-header coverage.

Exit: the production cron API implements the complete Linux timezone/DST
contract.

### Stage 4.4: Retry attribute extension and compatibility <- DONE

- add `retry.multiplier` and `retry.jitter`;
- update validation and standard-definition count/order;
- materialize new built-in values when decoding older Phase 3 documents;
- add attribute JSON/persistence compatibility tests;
- do not add retry scheduling yet.

Exit: every stored run snapshot can provide a complete Phase 4 retry policy.

### Stage 4.5: Retry policy calculation <- DONE

- add private retry-policy value/decoder;
- implement fixed/exponential delay, checked multiplier, cap, deterministic
  jitter, and executor retry-deadline combination;
- add exhaustive deterministic unit tests;
- do not touch run state.

Exit: a completion plus materialized attributes produces a checked retry
decision and due time.

### Stage 4.6: Recurring job create <- DONE

- inject `CronEngine` into `ManagementService`;
- enable CronSchedule validation and persistence;
- atomically create the first schedule-owned run;
- preserve normalized idempotency behavior;
- update `jobud` to construct `SystemCronEngine`;
- add service, in-memory RPC, restart, and public-Doxygen tests;
- keep the scheduler inactive in `jobud`.

Exit: the existing `job.create` RPC can durably create a valid recurring
definition with exactly one future run.

### Stage 4.7: Recurring job update <- DONE

- implement unstarted recurring snapshot/time refresh;
- implement definition-only update while a recurring run is running/retrying;
- implement once/cron conversion preconditions;
- add revision, snapshot, rollback, and newest-definition tests;
- keep one-time immutability unchanged.

Exit: recurring definitions can evolve without changing active execution
snapshots or violating the schedule-owned invariant.

### Stage 4.8: Attempt executor contract and fake executor <- DONE

- add and fully document `attempt_executor.hpp`;
- add direct public-header coverage;
- implement the test-only fake executor;
- validate completion shapes and callback/key rules in focused tests;
- do not add scheduler or production fake behavior.

Exit: later scheduler tests have a verified asynchronous, dependency-free
execution seam.

### Stage 4.9: Scheduler repository reads and startup reconstruction <- DONE

- add private scheduler repository values and bounded keyset queries;
- implement runtime queue, capacity, blocking-retry, manual-barrier, runnable,
  and earliest-wake reads;
- implement persisted-running detection;
- add SQLite repository tests for malformed/inconsistent rows and exact order;
- do not dispatch.

Exit: scheduler state and due candidates can be reconstructed from Phase 3
schema rows.

### Stage 4.10: Atomic dispatch transition <- DONE

- add dispatch-context revalidation;
- add checked next-attempt allocation;
- atomically insert running attempt and transition run;
- prove no executor call before commit with fake-driver/SQLite fault injection;
- add start-failure completion handoff scaffold;
- do not implement fairness or retry completion.

Exit: one explicitly selected candidate can be durably started safely.

### Stage 4.11: Single-queue scheduler core <- DONE

- add private scheduler core using fake time/executor;
- implement one queue, strict candidate order, global type limits, and combined
  queue capacity;
- implement explicit deterministic `process_cycle()` tests;
- leave weighted multi-queue arbitration for the next stage.

Exit: due work in one queue dispatches deterministically within all capacity
limits.

### Stage 4.12: Weighted multi-queue and mixed-type arbitration <- DONE

- add independent smooth weighted credits for CLI/HTTP;
- add idle-credit reset, UUID tie-breaking, and alternating type rounds;
- handle dynamic eligibility and coupled queue capacity;
- add long-run ratio and mixed-type tests;
- do not implement completion/retry yet.

Exit: multiple queues share available starts according to weight without a
fixed type-order bias.

### Stage 4.13: Attempt completion and retry wait <- DONE

- complete attempt/run atomically;
- implement terminal success/failure and retry-wait transitions;
- implement blocking/reschedule capacity accounting;
- release capacity only after commit;
- add executor start-error, retry exhaustion, deadline, and persistence-fault
  tests.

Exit: non-recurring runs execute through multiple fake attempts and reach a
correct durable terminal state.

### Stage 4.14: Recurring terminal successor <- DONE

- load the newest live definition on terminal completion;
- atomically terminalize the old run and create exactly one successor;
- implement coalescing and cancellation lower bounds;
- add schedule-update-while-running, missed-time, UUID/cron failure, and
  unique-index tests.

Exit: recurring execution can continue indefinitely without accumulating
missed occurrences.

### Stage 4.15: Suspension drain and resume interaction <- DONE

- complete queue/job `suspending -> suspended` after committed running-count
  drain;
- prevent starts while suspending/suspended;
- release blocking-retry occupancy while suspended;
- verify rescan after resume and dynamic queue policy updates;
- add service-plus-scheduler tests.

Exit: Phase 3 lifecycle mutations and Phase 4 execution obey one consistent
drain contract.

### Stage 4.16: Run Now and manual barrier <- DONE

- add and fully document `RunNowRequest` and `ManagementService::run_now`;
- implement all preconditions, snapshotting, and idempotency;
- implement scheduler manual barrier and job-suspension bypass;
- add public-header, service, barrier, retry, and release tests;
- do not register the later public RPC/CLI method.

Exit: a manual occurrence runs without changing or duplicating the scheduled
occurrence.

### Stage 4.17: Cancellation transitions <- DONE

- add and fully document scheduler cancellation result values/API;
- implement immediate pending/retry cancellation;
- implement running executor cancellation and repeated-request behavior;
- create recurring successors where required;
- add direct public-header and complete cancellation tests;
- do not add CLI/RPC exposure.

Exit: fake work can be cancelled in every non-terminal state without early
capacity release.

### Stage 4.18: Public scheduler and event-loop wake adapter <- DONE

- add fully documented `scheduler.hpp` and pimpl;
- wrap the deterministic core with one `Timer`;
- implement start/stop/rescan, wall-clock cap, state, stored failure, and
  one-shot failure signal;
- add direct public-header and focused EventLoop tests;
- do not wire a fake executor into `jobud`.

Exit: the complete scheduler can be hosted safely by an owner-thread event
loop.

### Stage 4.19: Linux scheduler integration and regression <- DONE

- add one SQLite/EventLoop/FakeTimeSource/FakeAttemptExecutor integration test;
- exercise cron, one-time work, fairness, retries, Run Now, suspend/resume,
  cancellation, and recurrence in one bounded scenario;
- update README status without claiming real CLI/HTTP execution;
- run clean GCC and Alpine-compatible Linux builds;
- run SQLite-disabled build/tests;
- audit all Phase 4 public Doxygen and include boundaries.

Exit: the complete deterministic Phase 4 behavior passes on Linux with no real
external work.

### Stage 4.20 (optional): macOS timezone roots and cron verification <- DONE

- build and run cron/TZif suites with Apple Clang;
- verify actual macOS zoneinfo roots, symlink aliases, and TZif footer behavior;
- add only the minimum Darwin-specific root/discovery source if required;
- keep cron expression and scheduler policy code shared;
- run the complete Linux suite after any production change.

Exit: production cron occurrence calculation has equivalent macOS behavior.

### Stage 4.21 (optional): macOS scheduler integration <- DONE

- run all backend-independent, SQLite, management, scheduler-core, and
  event-loop scheduler tests on macOS;
- use the existing `TMPDIR=/tmp` process-test convention where socket paths are
  involved;
- fix only Darwin-specific timer/platform behavior in this stage;
- rerun the complete Linux suite after any production fix.

Exit: the deterministic scheduler integration passes under Apple Clang.

### Stage 4.22: Final clean verification <- DONE

- configure a fresh Linux build directory;
- build with warnings visible;
- run the full suite with `--output-on-failure`;
- repeat with SQLite disabled;
- verify every new public header as first include;
- audit public Doxygen, `[[nodiscard]]`, dependency leakage, bounded queries,
  and absence of fake executor code from production targets;
- verify no real process, network request, or sleep-based scheduler test was
  introduced.

Exit: Phase 4 is ready to merge and provides a stable base for the Phase 5
asynchronous HTTP runner.

## 27. Phase 4 acceptance checklist

Phase 4 is complete only when:

- all cron syntax and timezone/DST rules are deterministic and tested;
- recurring create/update preserves exactly one schedule-owned non-terminal
  run;
- no database schema version change was needed;
- every external fake start is preceded by a committed running record;
- no failed pre-dispatch transaction calls the executor;
- retry attempts reuse the run UUID and increment attempt number;
- fixed/exponential delay, jitter, retry deadlines, and both retry modes pass;
- global and per-queue capacity never exceed configured limits;
- weighted fairness ratios and strict in-queue priority pass;
- manual runs never alter scheduled timing and always enforce the barrier;
- recurring completion/cancellation creates exactly one future successor;
- capacity releases only after the completion transaction commits;
- scheduler persistence/invariant failure stops all new dispatch;
- scheduler tests use injected time and explicit fake completions;
- `jobud` contains no production fake execution;
- all new public declarations have useful Doxygen;
- Linux full and SQLite-disabled builds pass;
- optional macOS stages, when selected, run only after Linux delivery and are
  followed by final Linux verification.
