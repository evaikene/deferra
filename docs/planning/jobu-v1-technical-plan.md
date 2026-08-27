# JobU v1 Technical Plan

## 1. Purpose

JobU is a single-host background service that persistently schedules and executes jobs. A job may run immediately, once at a specified time, or repeatedly according to a cron schedule. Jobs are grouped into independently controllable queues.

JobU v1 supports two execution types:

- CLI commands executed on the JobU host;
- HTTP requests executed against reachable endpoints.

The target workloads are:

- roughly ten queues with a handful of recurring, long-running HTTP jobs per queue;
- two or three queues receiving bursts of one-time, run-now jobs from an application.

The design must remain portable and minimize external dependencies. Linux and macOS are functional v1 targets. Interfaces must leave room for Windows, but the Windows process backend is intentionally unimplemented and must fail clearly at build time.

## 2. Architectural decision

Use a centralized, event-driven scheduler rather than one scheduler thread and a fixed set of blocking runners per queue.

The main `jb::core::EventLoop` owns:

- in-memory scheduling state;
- queue arbitration;
- timers;
- Unix-domain RPC connections;
- short SQLite transactions;
- asynchronous CLI-process events;
- asynchronous HTTP-transfer events.

There is no generic worker thread pool in v1:

- CLI concurrency consists of supervised child processes;
- HTTP concurrency consists of active libcurl multi transfers;
- concurrency settings are admission counters, not pre-created runner objects;
- logical queue count is independent of thread count.

The scheduler must never execute a job body itself. It starts asynchronous work only after the transition to `running` is durably committed.

### 2.1 Why this model

- Idle execution capacity is shared rather than reserved for a queue.
- Queue priority is explicit and deterministic rather than delegated to OS thread priority.
- Long HTTP waits consume neither threads nor scheduler time.
- Retry delays consume no HTTP/CLI slot.
- Queue count can grow without allocating threads and runner sets.
- Global limits and per-queue concurrency are directly enforceable.

## 3. Scope

### 3.1 Included in v1

- One active `jobud` service per database.
- SQLite persistence in WAL mode.
- Multiple queues with weight, concurrency, suspension, recovery, defaults, retention, and warning policies.
- Immediate, one-time scheduled, and recurring cron jobs.
- CLI and HTTP job types.
- Retry policies with fixed/exponential templates.
- Persistent run and attempt history with bounded output capture.
- Queue/job suspend, resume, move, delete, cancel, and Run Now operations.
- Local JSON-RPC 2.0 API over a Unix-domain socket.
- `jobuctl` client.
- Named secrets.
- systemd and `launchd` service definitions.
- Privilege dropping and root-execution safeguards.
- Linux and macOS CLI runners.
- C++ client-side RPC building blocks and a documented wire protocol.

### 3.2 Explicitly outside v1

- Multiple active JobU instances sharing a database.
- Leader election, distributed claiming, leases, and fencing.
- Remote worker nodes.
- Windows process execution and Windows Service integration.
- TCP or WebSocket RPC transports.
- Remote authentication and authorization.
- Official PHP client library.
- Prometheus, OpenTelemetry, and live event subscriptions.
- Fixed-interval schedules distinct from cron.
- Cron seconds and Quartz-only extensions.
- Per-job OS user switching.
- Query/body builders and payload templating.
- Native HTTP implementation replacing libcurl.

## 4. Technology and dependencies

The project remains C++20 with CMake.

Existing building blocks:

- `jb::core::Application`, `EventLoop`, `EventThread`, `Timer`, `IODevice`, objects, signals, logging, and INI parsing;
- `jb::net::TcpSocket` and future network primitives.

V1 external runtime/build dependencies:

- SQLite;
- libcurl;
- nlohmann/json.

Catch2 remains the test dependency. Dependency-specific types must not escape public JobU or `jb::*` interfaces. SQLite, libcurl, and nlohmann/json must be replaceable implementation details.

## 5. Source and library structure

Extend the repository with these logical targets:

- `jb::core`: portable process interface plus POSIX backend and required signal/process support;
- `jb::net`: Unix local sockets/server and asynchronous HTTP client facade;
- `jb::db`: synchronous, thread-affine database primitives and SQLite backend;
- `jb::rpc`: transport-independent JSON-RPC client/server, framing, typed dispatch, and error model;
- `jobu`: domain objects, repositories, attribute registry, cron engine, scheduler, runners, and services;
- `jobud`: daemon executable;
- `jobuctl`: command-line client.

Keep SQL in JobU repositories. `jb::db` is a thin database API, not an ORM and not an attempt to erase SQL-dialect differences.

### 5.1 `jb::core` additions

Add an event-loop-driven `Process` abstraction with:

- explicit executable and argument array;
- clean environment input;
- working directory;
- asynchronous stdout and stderr pipes;
- lifecycle state and exit result;
- distinction between exit code, signal termination, timeout, cancellation, start failure, and interruption;
- process-group creation and termination;
- Linux and macOS implementations;
- a platform-neutral public contract;
- an explicit unsupported Windows backend/build failure.

Add the minimum reusable signal/process-watching infrastructure needed for `SIGTERM`, `SIGINT`, `SIGCHLD`, and child reaping. Keep platform-specific mechanisms behind private backends.

### 5.2 `jb::net` additions

Add:

- `LocalSocket` and `LocalServer`, integrated with `IODevice` and `EventLoop`;
- project-owned `HttpRequest`, `HttpResponse`, `HttpError`, and `HttpClient` types;
- a private libcurl-multi backend integrated through event-loop FD watches and timers.

No `CURL*`, libcurl enum, callback, or error type may appear in a public header. Backend selection may remain build-time/internal in v1.

Require a libcurl build with asynchronous DNS support (threaded resolver or c-ares), or put resolution behind a non-blocking JobU-owned resolver. DNS lookup must not block the scheduler event loop.

### 5.3 `jb::db` additions

Provide project-owned abstractions for:

- connection/backend creation;
- prepared statements;
- positional or normalized named binding;
- null, signed integer, floating-point, text, and blob values;
- rows/results;
- transactions with explicit commit and rollback;
- backend-neutral error category plus backend detail;
- schema version storage.

Connections are synchronous and thread-affine. JobU keeps transactions short on the scheduler thread. A future network database backend may be placed on a dedicated event thread without changing JobU repository contracts.

SQLite is the only v1 backend. Enable WAL, foreign keys, a bounded busy timeout, and appropriate durability settings. Enforce single active JobU ownership of a database.

Use an explicit adjacent lock file/process lock in addition to SQLite's normal database locking so a second scheduler fails at startup rather than opening the same database and competing intermittently.

### 5.4 `jb::rpc` additions

Provide:

- JSON-RPC 2.0 parsing, validation, dispatch, response, and client correlation;
- stable project-owned error codes with message and structured data;
- request cancellation/connection cleanup where relevant;
- operation-context storage for future authenticated identities;
- a transport adapter over byte-oriented `IODevice` objects;
- LSP-style stream framing.

nlohmann/json stays private to the implementation. Typed JobU handlers must not accept or return nlohmann/json types.

## 6. Domain model and terminology

### 6.1 Queue

A `Queue` is a persistent logical execution group with:

- UUID identity;
- unique active name;
- lifecycle state;
- positive scheduling weight, default `1`;
- combined concurrency limit, default `1`;
- recovery policy;
- job-creation defaults;
- history-retention override;
- runnable-wait warning threshold, default 10 seconds.

Queue-owned operational policies apply dynamically to all work currently assigned to the queue.

### 6.2 JobDefinition

A `JobDefinition` is a persistent definition with:

- UUIDv7 identity;
- optional non-unique display name;
- owning queue;
- revision number;
- lifecycle state;
- job type (`cli` or `http`);
- schedule type and data;
- signed integer priority, default `0`;
- complete, materialized attribute document;
- runner payload document;
- creation idempotency metadata;
- created/updated/deleted timestamps.

Use `JobDefinition` internally. User-facing commands may call it a job.

### 6.3 JobRun

A `JobRun` is one occurrence of a job:

- UUIDv7 identity;
- job-definition identity and revision;
- queue identity used for this run;
- origin such as `scheduled`, `manual`, or application submission;
- planned execution time in UTC;
- runnable/actual timing data;
- immutable execution/policy snapshot;
- lifecycle state;
- terminal result summary.

A recurring definition always has exactly one schedule-owned non-terminal run. A manual Run Now operation may temporarily add a separate manual run without replacing or recalculating the scheduled run.

### 6.4 JobAttempt

A `JobAttempt` is one execution attempt within a run:

- parent run UUID;
- monotonically increasing attempt number;
- due/start/completion timestamps;
- outcome and retry classification;
- exit code, terminating signal, HTTP status, or transport error as applicable;
- timing measurements;
- captured output metadata and blobs.

Retries create new attempts, not new runs. The run UUID remains stable across retries and is suitable for downstream idempotency.

### 6.5 Secret

A named `Secret` has a stable name, stored value, and metadata. Secret values are plaintext in SQLite v1 and rely on database filesystem permissions. RPC reads never return secret values.

Job value fields may contain:

- a literal;
- a named secret reference;
- where explicitly supported, a runtime metadata reference such as run ID.

## 7. Identifiers and revisions

- Use UUIDv7 for queues, jobs, and runs.
- Store UUIDs efficiently as 16-byte database values.
- Queue names remain the common human selector but are not durable references.
- Attempts use `(run_uuid, attempt_number)`.
- Job updates require the expected revision for optimistic concurrency.
- Updating a job increments its revision.
- Queue renames do not change queue identity.

## 8. Attribute model

Use fixed relational columns for identity, relationships, lifecycle state, schedule, type, priority, queue weight/concurrency, and indexed timestamps.

Use versioned JSON documents for extensible execution and retry/capture policy settings. Maintain a registry of known attributes containing:

- canonical name;
- type;
- valid scope;
- built-in default;
- constraints and validation;
- documentation;
- sensitive-value behavior.

Reject unknown attributes to catch errors.

### 8.1 Creation-time default materialization

At job creation, resolve:

```text
built-in defaults -> daemon INI defaults -> queue defaults -> supplied job values
```

Store the complete resolved job attribute set. Queue-default changes affect only jobs created afterward. Moving a job never reapplies defaults from the target queue.

At run creation, snapshot the current complete job attributes and runner payload. Running and retrying work is therefore insulated from later job edits.

A newly introduced attribute missing from an older persisted job receives its registry-defined built-in value through a migration or compatibility resolver.

### 8.2 Job update semantics

- An update requires the expected job revision.
- If the schedule-owned run is pending and has never started, refresh its revision, execution snapshot, and planned time from the updated definition.
- A running or retry-waiting run retains the old definition revision and snapshot through terminal completion.
- The next recurring run uses the newest definition.
- A one-time job becomes immutable once its first attempt starts.
- Secret references are snapshotted as references, not resolved values; secret rotation therefore affects later attempts.

### 8.3 Initial common attributes

Include at least:

- `job.timeout`, built-in default 120 seconds;
- capture policy and byte limits;
- retry policy and retry mode;
- retryable failure selectors;
- CLI working directory, environment, accepted exit codes, and termination grace;
- HTTP expected statuses, headers, body, TLS, redirects, proxy behavior, idempotency header, and capture controls.

Use one generic job timeout for CLI and HTTP in v1. More specialized timeouts may be added as attributes without schema changes.

## 9. Queue and job lifecycle

Queues and jobs use:

```text
active -> suspending -> suspended -> deleted
```

### 9.1 Suspend

- `suspend` atomically changes `active` to `suspending`.
- No new attempts start after that transaction.
- Running attempts continue.
- When the running-attempt count becomes zero, transition to `suspended`.
- A failed attempt may enter retry-wait, but retry-wait does not prevent suspension completion.
- Retries do not start while the queue/job is suspending or suspended.
- Retry-waiting work belonging to a suspended job consumes no queue slot.
- `resume` may cancel a pending suspension and return the object to `active`.
- On startup, recovery completes any persisted `suspending` transition after processing running attempts.

`jobuctl ... suspend --wait` may poll state until fully suspended. The mutation RPC itself returns the current/intermediate state immediately.

### 9.2 Resume

- Resume makes eligible one-time work runnable.
- An overdue recurring schedule-owned run becomes runnable once.
- Missed cron occurrences never accumulate as multiple runs.

### 9.3 Delete job

- Require the job to be fully suspended.
- Require no running attempt.
- Soft-delete the definition.
- Cancel pending scheduled and retry-waiting runs.
- Retain history until normal retention removes it.
- Physically purge the definition when no retained record references it.

### 9.4 Delete queue

- Require the queue to be fully suspended.
- Running attempts are already absent by construction.
- Soft-delete contained job definitions.
- Cancel pending scheduled and retry-waiting runs.
- Rename the queue internally to `<original>-deleted#<uuid>` to release its unique name.
- Preserve the original queue name in history views/snapshots.
- Hide deleted queues by default but allow explicit listing.
- Physically purge the queue and definitions after retained history no longer references them.

### 9.5 Move job

- Require the job to be fully suspended.
- Move its pending scheduled or retry-waiting run with it.
- Preserve complete job attributes unchanged.
- Increment the job revision.
- Completed history retains the actual execution queue.
- On resume, the work competes under target-queue operational policies.

## 10. Run state and scheduling invariants

Use run states equivalent to:

- `scheduled`;
- `running`;
- `retry_wait`;
- `succeeded`;
- `failed`;
- `interrupted`;
- `cancelled`.

Timeout and failure category are result details rather than additional run states.

Core invariants:

- A job attempt starts only after `running` is committed.
- One run has at most one running attempt.
- A recurring job has exactly one schedule-owned non-terminal run.
- A completed recurring run produces one next schedule-owned run.
- The next recurring time is the first matching cron time after the current completion/recovery time.
- Historical missed occurrences are coalesced and never replayed one by one.
- Scheduler fairness credit need not be persisted.

If a recurring run is cancelled, compute its successor strictly after the later of its planned time and current time so the cancelled occurrence is not recreated.

## 11. Cron and time semantics

### 11.1 Syntax

Support five fields:

```text
minute hour day-of-month month day-of-week
```

Support:

- `*`;
- comma-separated lists;
- ranges;
- steps;
- numeric fields;
- English month and weekday names;
- aliases such as `@hourly`, `@daily`, and `@weekly`.

Do not support a seconds field or Quartz-only `?`, `L`, and `#` in v1.

Day-of-month and day-of-week use AND semantics. For example, a job constrained to day 13 and Friday runs only on Friday the 13th.

Provide schedule validation and a command/RPC to display the next N occurrences.

### 11.2 Timezones and DST

- Accept one-time timestamps with an explicit UTC offset and persist UTC.
- Default recurring timezone is UTC.
- Allow a daemon default IANA timezone and a per-job IANA timezone override.
- Resolve and persist each concrete planned time as UTC.
- Spring-forward gap: shift forward by the gap to the corresponding valid local time.
- Fall-back overlap: use the first/earlier occurrence.

Wrap wall/steady clocks behind testable interfaces. Use monotonic timers for in-process waits but persist UTC deadlines. Cap long scheduler sleeps with periodic wall-clock reevaluation so clock jumps do not leave newly overdue jobs asleep indefinitely.

## 12. Capacity, fairness, and selection

Defaults:

- queue concurrency: `1`;
- global CLI concurrency: `4`;
- global HTTP concurrency: `16`.

All limits are positive. Lowering a limit below current usage never cancels work; it prevents new admissions until usage falls below the limit.

There is no separate global combined-job limit and no limit on the number of stored future jobs other than practical storage constraints.

### 12.1 Queue arbitration

- Use a positive queue weight, default `1`.
- Under sustained contention, weight `2` receives approximately twice the dispatch opportunities of weight `1`.
- Maintain separate weighted arbitration for CLI and HTTP capacity.
- Do not accumulate unbounded credit while a queue is idle.
- Apply the queue's combined concurrency cap after resource-type eligibility.

Implement a deterministic weighted round-robin/smooth weighted algorithm and test long-run ratios, dynamic eligibility, suspension, and coupled queue limits.

### 12.2 Selection inside a queue

Among eligible jobs for the available runner type, select by:

1. higher integer job priority;
2. earlier eligible/planned time;
3. stable insertion sequence/UUID ordering as final deterministic tie-breaker.

Job priority is strict. Lower-priority jobs may starve. Starvation is addressed through warnings/statistics and by moving jobs to separate queues, not by priority aging.

## 13. Retry model

### 13.1 Retry policy

Store the internal policy as:

- `max_attempts`, including the first attempt, default `1`;
- `initial_delay`;
- `backoff_multiplier`;
- `max_delay`;
- optional jitter, default off.

Approximate delay before attempt `n`:

```text
min(initial_delay * multiplier^(n - 2), max_delay)
```

Expose convenient user templates such as `none`, `fixed`, and `exponential`, while allowing advanced values.

### 13.2 Retry mode

Support:

- `blocking` (default): retry-waiting run retains a queue concurrency slot while active;
- `reschedule`: retry-waiting run releases the queue slot.

A retry-waiting run never consumes a global CLI or HTTP slot. Suspended jobs release a blocking retry's queue slot until resumed.

### 13.3 Failure classification

Defaults:

- CLI unexpected exit codes: retryable;
- CLI timeout or signal termination: retryable;
- CLI start/configuration errors: terminal;
- HTTP transport errors and timeouts: retryable;
- HTTP 408, 429, and 5xx: retryable;
- other unexpected HTTP statuses, especially other 4xx: terminal.

Jobs may override retryable exit codes, HTTP statuses/ranges, and error categories. For 429 and 503, honor `Retry-After` when it delays the next attempt beyond the computed policy delay.

## 14. Run Now

Run Now is an explicit test/manual action that creates a manual run without changing the scheduled run.

Allow it only when:

- the job has a schedule-owned run in `scheduled` state;
- that run's planned time is strictly in the future;
- the job is not running or retrying;
- no manual run is already pending or active.

Return a stable conflict error otherwise. This prevents Run Now from duplicating a job already overdue and merely blocked on capacity.

Behavior:

- snapshot the current job revision into a new manual run;
- leave the schedule-owned run and planned time unchanged;
- temporarily block the schedule-owned run until the manual run is terminal;
- bypass the job's suspended state;
- never bypass queue suspension;
- obey queue/global capacity and normal priority;
- remove the internal barrier without modifying the persisted job suspension state.

## 15. Cancellation and termination

### 15.1 Explicit cancellation and timeout

- HTTP cancellation/timeout stops the transfer immediately.
- CLI cancellation/timeout sends `SIGTERM` to the process group.
- Wait five seconds by default, configurable as an attribute.
- Send `SIGKILL` if the group remains.
- Keep the CLI/global and queue slot occupied until termination completes.
- Mark the run cancelled for explicit cancellation; record timeout as a failed attempt category.

Cancelling one recurring run does not suspend/delete the definition. Schedule its next future occurrence using the recurrence rules.

### 15.2 Daemon shutdown

Use no graceful drain period:

1. Stop accepting RPC mutations and dispatching new attempts.
2. Cancel active HTTP transfers immediately.
3. Send `SIGKILL` to all active CLI process groups immediately.
4. Close infrastructure and exit promptly.
5. Do not rewrite persisted running attempts during shutdown.

Startup treats the remaining `running` records exactly like crash leftovers.

## 16. CLI runner contract

CLI payload contains:

- executable path/name;
- argument array;
- optional working directory;
- environment additions/removals;
- expected exit-code set, default `{0}`.

Rules:

- Never parse an implicit shell command string.
- Shell execution is explicit, for example `/bin/sh` with `-c` and a command argument.
- Close stdin in v1.
- Use a completely clean environment.
- Apply daemon-defined job defaults, queue creation defaults already materialized into the job, then job environment values and secret resolution.
- Inject no implicit `PATH`, `HOME`, locale, or proxy variables.
- If executable is not absolute, require an explicit `PATH` in the resolved job environment and use it for deterministic lookup.
- Default working directory is `/` unless set.

Inject:

```text
JOBU_JOB_ID=<job UUID>
JOBU_RUN_ID=<run UUID>
JOBU_ATTEMPT=<attempt number>
```

Keep `JOBU_RUN_ID` stable across retries.

## 17. HTTP runner contract

HTTP payload contains:

- method;
- URL;
- headers;
- optional raw body;
- expected status values/ranges, default 200-299.

V1 sends URL/body in final wire form. Query, form, and JSON builders may be added later.

Rules/defaults:

- only `http` and `https` schemes;
- shared non-blocking connection engine;
- generic `job.timeout` covers connection, redirects, and response transfer;
- verify TLS certificate and hostname by default;
- allow per-job TLS verification disable with a prominent warning;
- support daemon CA-bundle override and prefer it for self-signed deployments;
- do not follow redirects by default;
- per-job redirect opt-in with maximum default `5`;
- strip sensitive headers on cross-origin redirects;
- use explicit daemon proxy configuration, not uncontrolled process environment;
- automatically decompress responses and apply limits to decompressed data;
- bound headers and body independently.

Inject non-overridable headers:

```text
X-JobU-Job-ID: <job UUID>
X-JobU-Run-ID: <run UUID>
X-JobU-Attempt: <attempt number>
```

Support opt-in `Idempotency-Key: <run UUID>`.

## 18. Output capture

Per-job capture modes:

- `none`: drain and discard;
- `on_error` (default): buffer while executing and persist for any non-success outcome;
- `always`: persist for all outcomes.

Always continue draining pipes/transfers after capture limits are reached.

Defaults per attempt:

- CLI stdout: 1 MiB;
- CLI stderr: 1 MiB;
- HTTP response body: 1 MiB;
- HTTP response headers: 64 KiB.

When truncated, retain the first and last halves and record total bytes plus omitted-byte/truncation metadata. Store raw bytes as BLOBs. RPC returns valid UTF-8 as text and otherwise uses base64 with explicit encoding metadata. Run summaries exclude output; clients fetch attempt output separately.

## 19. Persistence and crash consistency

### 19.1 Transaction boundaries

At dispatch:

1. Revalidate queue/job state and capacity on the scheduler thread.
2. Begin transaction.
3. Create/update attempt and run to `running`.
4. Commit.
5. Start the external operation.

If starting fails, persist a normal start-failure result.

At completion, persist attempt result, run transition, capture metadata/output, capacity effects, and next-run/retry state in one transaction. Only after commit may the scheduler release capacity and dispatch dependent work.

### 19.2 Unknown outcome

Exactly-once execution is impossible for arbitrary CLI/HTTP effects. A crash may happen after the effect completes but before the result commits.

Queue-level `recovery_policy`:

- `fail_interrupted` (default): running attempts found at startup become terminally interrupted;
- `retry_interrupted`: mark the old attempt interrupted and retry the same run if retryable attempts remain; otherwise leave the run interrupted.

Normal explicitly observed failures follow job retry policy independently of recovery policy.

After a recurring run becomes terminal through recovery, create its next run at the first cron time after recovery/current time.

### 19.3 Startup recovery

Within controlled transactions:

1. Validate schema and invariants.
2. Find all persisted running attempts/runs.
3. Convert attempts to interrupted outcomes.
4. Apply each queue recovery policy and remaining-attempt rules.
5. Re-establish manual-run barriers.
6. Repair the one-schedule-owned-run invariant for active recurring definitions.
7. Complete persisted suspending transitions when no attempt remains running.
8. Make overdue eligible work available.
9. Only then open normal RPC mutation service and dispatch.

### 19.4 Database failure policy

Fail closed:

- Never start work if the pre-dispatch transaction fails.
- Completion state and capture commit atomically; do not retry without output.
- If a scheduler-state write fails, stop dispatch and RPC mutations, terminate active work through immediate shutdown, and exit nonzero.
- A failed completion remains durably running and is recovered as interrupted at next startup.
- Ordinary history-read errors return RPC errors unless they indicate corruption.
- Corruption or invariant failure is fatal.

## 20. Database schema and migrations

Create normalized tables equivalent to:

- schema migrations/version;
- queues;
- job definitions;
- job runs;
- job attempts;
- attempt output blobs/metadata;
- named secrets;
- idempotency records.

Keep runner payload and extensible attributes as versioned JSON text. Keep indexed scheduler keys as columns. Add indexes for:

- eligible work by queue, type, state, priority, and planned/retry time;
- active attempts;
- recurring invariant lookup;
- history ordering/filtering;
- retention cleanup;
- idempotency scope/key uniqueness.

Migration policy:

- numbered forward-only JobU migrations;
- automatic startup migration before the RPC socket opens;
- transactional where supported;
- refuse a database newer than the binary;
- no automatic downgrade;
- create a SQLite backup before destructive/rebuild migrations;
- log progress and failures clearly;
- run recovery only after migration completes.

## 21. Idempotent mutations

Support optional operation idempotency for durable resource creation, including:

- one-time, scheduled, and recurring job creation;
- Run Now;
- future create-like operations.

Scope keys to RPC method and relevant parent resource. Store the canonical request hash and original result atomically with the mutation.

- Same key and same canonical request: return original result.
- Same key and different request: conflict.
- Recurring creation keys remain for the definition lifetime.
- One-time/manual keys may expire when retained history is purged.

Use revision checks, not idempotency keys, for updates. Suspend/resume are naturally idempotent.

## 22. Secrets

Provide RPC/CLI operations to set, list metadata for, and delete named secrets.

- Never return secret values through read APIs.
- Never log resolved secrets or include them in attempt output metadata.
- Store references in job definitions and resolve values immediately before an attempt.
- Secret rotation affects future attempts, including retries, without revising job definitions.
- Reject deletion while a secret is referenced unless an explicit safe force workflow is later designed.
- Warn that secrets used as CLI arguments may be visible through OS process inspection; prefer environment values where possible.

Design the secret provider contract so encrypted database storage or external stores can replace the plaintext SQLite implementation later.

## 23. JSON-RPC control plane

### 23.1 Transport and framing

V1 uses a local Unix-domain stream socket. Filesystem ownership/mode is the complete authorization boundary: any connecting client has full access.

Use LSP-style framing:

```text
Content-Length: <UTF-8 body byte count>\r\n
<optional future headers>\r\n
\r\n
<JSON-RPC 2.0 body>
```

Requirements:

- mandatory `Content-Length`;
- bounded header and body sizes;
- ignore unknown headers for forward compatibility;
- persistent connections;
- multiple sequential/outstanding request IDs;
- strict JSON-RPC envelope validation;
- human-testable with `socat`, `nc`, and shell tools.

Future raw TCP reuses this framing. Future WebSocket carries one JSON body per WebSocket message.

### 23.2 Security context

V1 authorizes solely through socket filesystem permissions. Record peer UID/PID for audit where supported, but do not implement per-user ownership or RBAC.

Keep an authenticated-principal/authorization context in RPC interfaces for future TCP/WebSocket. Remote access will require authentication and method-level authorization; a connection-level JWT handshake is preferable to adding JWT to every business request, but remains outside v1.

### 23.3 Compatibility

- `system.info` reports daemon version, API major/minor, and capabilities.
- Do not require exact client/server version match.
- Clients ignore unknown response fields.
- Additive methods/fields increment minor API version.
- Breaking changes require a new major API version.
- `jobuctl` checks command-required capabilities.
- Stable errors include code, message, and structured data.

### 23.4 Initial method families

Define typed methods equivalent to:

- `system.info`, `system.stats`;
- `queue.create`, `queue.get`, `queue.list`, `queue.update`, `queue.suspend`, `queue.resume`, `queue.delete`, `queue.stats`;
- `job.create`, `job.get`, `job.list`, `job.update`, `job.suspend`, `job.resume`, `job.move`, `job.delete`, `job.run_now`;
- `run.get`, `run.list`, `run.cancel`;
- `attempt.get`, `attempt.output`;
- `secret.set`, `secret.list`, `secret.delete`;
- `schedule.validate`, `schedule.next`.

Use explicit request/response schemas and revision/idempotency fields. Keep transport DTOs separate from scheduler/domain objects.

## 24. History and pagination

- Retain terminal run/attempt history for 30 days by default.
- Allow global default and per-queue override.
- Retain all non-terminal records.
- Purge output with its attempt.
- Use incremental small cleanup batches.
- Allow unlimited retention explicitly.

History queries:

- newest first by default;
- bounded configurable page size;
- opaque, short-lived keyset cursor;
- no offset pagination;
- filter by queue, job, state, origin, job type, and planned/start/completion ranges;
- summaries separate from output.

Deleted queue/definition rows are physically purged only after history and idempotency retention permit it.

## 25. Observability

### 25.1 Delay definitions

- Schedule lateness: actual start minus planned execution time; includes downtime and suspension.
- Runnable wait: time eligible while daemon, queue, and job permit execution but capacity/scheduling has not selected it.

Emit at most one delayed-job warning when runnable wait exceeds the queue threshold, default 10 seconds. A threshold of zero disables the warning.

### 25.2 V1 statistics

Expose through JSON-RPC/`jobuctl`:

- counts by queue, job, type, origin, and outcome;
- average and maximum schedule lateness;
- average and maximum runnable wait;
- average and maximum execution duration;
- retries, interruptions, cancellations, and capture truncations.

Compute from indexed retained history in v1. Add persisted rollups/percentiles only after query cost is measured.

Use structured `jb::core` logging. Do not implement Prometheus/OTel endpoints in v1.

## 26. Privilege and process security

- systemd/`launchd` runs `jobud` as a dedicated `jobu` account by default.
- If started as root, require `run_as_user` and optional `run_as_group` unless an explicit unsafe root-daemon override is configured.
- Open only resources that require privilege, then call `initgroups`, drop group/user IDs permanently, and verify before opening normal service/RPC execution.
- Fail startup if dropping privileges fails.
- CLI jobs use the daemon's final unprivileged identity.
- Do not support per-job users in v1.
- `allow_root_cli = false` by default and recheck immediately before spawning.
- Unsafe root overrides produce prominent startup warnings.
- Enable Linux `no_new_privs` for CLI children where supported.
- Service files and installation create restrictive configuration, data, database, and socket paths.

## 27. Daemon configuration

Use `jb::core::IniFile`. Load configuration at startup only in v1; changes require restart.

Configuration includes:

- database path/backend;
- Unix socket path, owner, group, and mode;
- daemon privilege drop identity;
- CLI/HTTP concurrency defaults;
- job-attribute defaults;
- default timezone (UTC by default);
- output and history defaults;
- RPC framing/size limits;
- logging;
- HTTP proxy and CA settings;
- unsafe root overrides.

Queues, jobs, schedules, queue defaults, and secrets remain database entities managed through RPC. The database is their single source of truth. Declarative queue/job configuration is outside v1.

## 28. Service packaging

`jobud` always runs in the foreground.

Provide:

- systemd unit for Linux;
- `launchd` plist for macOS;
- direct foreground mode for development and containers.

Service manager configuration must use a dedicated account, restrictive umask/paths, restart-on-failure policy appropriate for fail-closed exits, and a short stop timeout compatible with JobU's immediate shutdown.

## 29. Testing strategy

### 29.1 Unit tests

- Cron parsing, AND day semantics, timezone resolution, spring gap, and fall overlap.
- Attribute validation/materialization and new-attribute compatibility.
- Retry delay, jitter bounds, `Retry-After`, and retry classification.
- Run/attempt state transitions and invariants.
- Queue/job suspension draining and resume.
- Run Now preconditions/barrier.
- Strict job ordering.
- Weighted queue fairness ratios and no starvation between queues.
- Blocking/reschedule retry capacity accounting.
- Output first/last truncation and binary encoding.
- LSP framing fragmentation, coalescing, malformed headers, limits, and multiple messages.
- RPC schema/error/capability behavior.
- Idempotency key same/different request behavior.
- Revision conflicts.

Use injected wall and monotonic clocks; never make scheduler tests depend on real sleeps.

### 29.2 Repository tests

- SQLite schema/migrations and newer-schema refusal.
- Transactional dispatch and completion.
- Startup recovery for both recovery policies.
- Recurring one-active-scheduled-run invariant.
- Retention and soft-deletion purge.
- Queue synthetic rename and name reuse.
- Keyset pagination under concurrent inserts.
- Fail-closed fault injection.

### 29.3 Runner integration tests

CLI:

- stdout/stderr draining;
- clean environment and metadata variables;
- PATH resolution rules;
- expected exit codes;
- start failure;
- timeout/cancel TERM-to-KILL escalation;
- process-group descendant termination;
- immediate shutdown kill;
- Linux and macOS coverage.

HTTP:

- methods, headers, raw bodies, success ranges;
- connection/total timeout through common job timeout;
- transport and status retry classification;
- `Retry-After`;
- redirect policy and sensitive-header stripping;
- TLS verification/custom CA/unsafe disable;
- decompression and capture bounds;
- JobU metadata and optional idempotency headers;
- concurrency and connection reuse.

Use a local deterministic HTTP test server.

### 29.4 End-to-end tests

- Start `jobud` with temporary SQLite and socket.
- Drive all operations through `jobuctl`/RPC.
- Submit immediate, scheduled, recurring, CLI, and HTTP jobs.
- Exercise suspend/resume/move/delete/cancel/Run Now.
- Kill the daemon during active work and verify both recovery policies.
- Verify restart after schema migration.
- Verify privilege/root refusal where CI permits.
- Verify service files syntactically on their target OS.

## 30. Phased implementation plan

Keep every phase independently reviewable. Do not combine unrelated foundation and scheduler changes in one large patch.

### Phase 0: Contracts and test scaffolding

- Add target/module skeletons for `db`, `rpc`, `jobu`, `jobud`, and `jobuctl`.
- Define project-owned error/result, UUIDv7, clock, binary-data, and attribute contracts needed by later layers.
- Add fake clock and temporary-resource test helpers.
- Record v1 invariants in code-level design documentation.

Exit criteria: all targets build on Linux/macOS; no scheduler behavior yet.

### Phase 1: `jb::db` and SQLite

- Implement connection, statement, value, row, transaction, and errors.
- Implement SQLite backend with WAL/foreign keys/busy timeout.
- Add migration runner and schema-version checks.
- Test transaction and failure behavior.

Exit criteria: JobU can create/migrate/open its empty schema through project-owned APIs, with no SQLite type leakage.

### Phase 2: Local IPC and `jb::rpc`

- Implement `LocalSocket`/`LocalServer`.
- Implement LSP framing and bounded parsing.
- Implement JSON-RPC client/server and stable error model.
- Add `system.info` round-trip and capability negotiation.

Exit criteria: `jobuctl system info` communicates with a foreground test daemon over a Unix socket.

### Phase 3: Domain schema and management API

- Implement queue/job/run/attempt/secret/idempotency repositories.
- Implement attribute registry and creation-time default materialization.
- Implement queue/job CRUD, revisions, suspend/resume state storage, move, soft delete, and retention primitives.
- Implement initial `jobuctl` management commands.

Exit criteria: durable management lifecycle works without executing jobs.

### Phase 4: Cron engine and scheduler with fake runner

- Implement cron parser/timezone/DST rules.
- Implement schedule-owned run invariant.
- Implement eligibility indexes/queries and in-memory scheduler state.
- Implement weighted queue arbitration, strict job priority, capacity accounting, and timers.
- Implement retry state machine/modes/classification contracts using deterministic fake runners.
- Implement Run Now barrier and cancellation state transitions.

Exit criteria: complete deterministic scheduler test suite passes without real external work.

### Phase 5: Asynchronous HTTP foundation and runner

- Add project-owned `jb::net` HTTP API.
- Integrate private libcurl multi backend with `EventLoop`.
- Implement HTTP payload, security defaults, headers, timeouts, capture, and retry result mapping.
- Add local-server integration tests.

Exit criteria: concurrent HTTP jobs run through the real scheduler with no blocking runner threads.

### Phase 6: Asynchronous CLI foundation and runner

- Implement `jb::core::Process` POSIX backend.
- Implement clean environment, path lookup, working directory, pipes, process groups, exit/signal reporting, timeout/cancel escalation, and shutdown kill.
- Implement Linux `no_new_privs` support.
- Integrate CLI runner with scheduler and capture.

Exit criteria: concurrent CLI jobs and descendant termination work on Linux and macOS; Windows fails explicitly as unsupported.

### Phase 7: Recovery, fail-closed behavior, and shutdown

- Implement startup recovery for `fail_interrupted` and `retry_interrupted`.
- Repair recurrence/manual/suspension invariants.
- Implement atomic dispatch/completion boundaries.
- Add SQLite fault injection and fatal transition.
- Implement immediate service shutdown.

Exit criteria: kill/restart and write-failure end-to-end tests prove no job starts without a durable running record.

### Phase 8: Secrets, complete RPC, and `jobuctl`

- Finish named-secret resolution/redaction.
- Implement all v1 RPC method families.
- Add idempotent creation semantics and optimistic revisions.
- Implement history filtering/cursors and separate output retrieval.
- Complete command UX and machine-readable output mode.
- Publish protocol documentation and a reusable C++ client API.

Exit criteria: all v1 administrative and application submission workflows are possible through the public protocol.

### Phase 9: Retention, observability, security, and packaging

- Implement incremental 30-day cleanup.
- Implement delay tracking/warnings and history-derived statistics.
- Implement privilege dropping/root safeguards.
- Finalize INI parsing/validation and safe defaults.
- Add systemd unit and `launchd` plist.
- Add installation documentation and operational recovery guide.

Exit criteria: Linux/macOS packages can run securely as managed services and satisfy the full v1 acceptance suite.

## 31. V1 acceptance criteria

V1 is complete when:

- Queue count does not allocate scheduler/runner threads.
- Different queues execute concurrently while respecting weights and all limits.
- Queue concurrency defaults to sequential execution and can be increased.
- Long HTTP requests do not block scheduling or consume worker threads.
- Every external start has a prior durable running record.
- Crash/proper-shutdown recovery produces the configured interrupted behavior.
- Recurring jobs never build a historical catch-up backlog.
- Suspension drains running attempts and prevents new starts.
- Run Now never duplicates overdue capacity-blocked work or changes the schedule.
- Job moves preserve materialized attributes.
- Idempotency prevents duplicate create/manual operations after lost responses.
- CLI jobs have a clean environment and cannot run as root by default.
- TLS verification is secure by default and explicitly overridable.
- Captured output is bounded and never blocks child processes.
- History is paginated, retained for 30 days by default, and exposes delay statistics.
- The daemon and CLI interoperate across additive minor-version differences.
- Linux and macOS integration tests pass; Windows is clearly unsupported at build time.

## 32. Deferred design hooks

The v1 interfaces must leave clean extension points for:

- MySQL/MariaDB and PostgreSQL `jb::db` backends;
- dedicated DB event thread for network databases;
- native/replacement HTTP backend;
- Windows event-loop/process/service backends;
- TCP and WebSocket RPC transports;
- connection-level JWT authentication and method-level authorization;
- official PHP client;
- external/encrypted secrets;
- Prometheus/OpenTelemetry and subscriptions;
- query/form/JSON builders and runtime value sources;
- traditional cron OR semantics as an explicit option;
- persisted statistics rollups and percentiles;
- distributed execution, only as a separate architecture phase with leases and fencing.

These hooks must not add unused runtime abstraction or plugin machinery to v1. Public ownership boundaries and project-owned types are sufficient until a second implementation exists.
