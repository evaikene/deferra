# JobU Phase 9: code-level design — Revision 3

- Revision: **3**
- Date: 2026-09-28
- Status: proposed implementation contract; no implementation is claimed by this document
- Repository: <https://github.com/evaikene/deferra>
- Baseline: [`cebb4cb278942eb5eb939d2dc64ab9129e035841`](https://github.com/evaikene/deferra/commit/cebb4cb278942eb5eb939d2dc64ab9129e035841)
- Download: `jobu-phase9-code-design-r3.md`
- Repository destination: `docs/planning/jobu-phase9-code-design.md`

## 1. Phase boundary and authority

Phase 9 completes the v1 operational layer: retention, measured runnable wait and delay warnings, startup configuration, privilege handling, installation, reusable library exports, managed services, and operations documentation. It builds on the completed Phase 8 closure, including durable one-time terminal definitions, bounded attempt numbers, safe ControlClient delivery, Active-default CLI listing, and secret-reference flags.

The supplied Phase 8 record reports a fresh 170/170 Linux pass, 171/171 native macOS pass, and client ASan passes on this baseline. Both platforms skipped their root-only case on non-root hosts. These establish provenance; they are not Phase 9 evidence.

The original v1 technical plan defines the broad scope. This document supplies the implementation contract and supersedes older wording where subsequent user decisions or current APIs differ:

- Keep the centralized owner-thread scheduler and bounded runners. Do not introduce a worker thread per queue, a second database writer, or a new scheduling algorithm.
- Keep **fresh database creation only**. The Phase 9 storage additions use format marker 4. Reject older formats without migration, backup automation, marker rewriting, or legacy-data backfill. Existing development databases may be deliberately recreated.
- Terminal one-time definitions remain terminal. New Run Now requests cannot reopen them; previously accepted idempotency responses can still replay while their records are retained. Clone is outside v1.
- [Issue #230](https://github.com/evaikene/deferra/issues/230) is an **accepted deferred product question**. Preserve current `run.list` membership, filtering, pending-run updates, and move behavior. Do not resolve or close that issue through Phase 9, including through documentation changes disguised as a behavior fix.
- Installation now owns a shared default socket path. Explicit `--socket` remains an override. Do not add socket half-close support, socket activation, remote transports, JWT, or RBAC.
- Statistics stay based on retained rows. No persisted rollups, percentiles, Prometheus/OTel endpoint, or broad logging framework rewrite is required without measured evidence.

### 1.1 Deliverables

| Area | Phase 9 result |
| --- | --- |
| Retention | Incremental terminal-run cleanup, per-queue/inherited/unlimited policies, safe idempotency retirement, and deleted-owner purge. |
| Timing | Monotonic observed runnable-wait accumulation, persisted quality information, aggregate statistics, and one warning per run. |
| Configuration | Strict startup-only flat INI, safe defaults, CLI precedence, validated materialized defaults, and local help/check mode. |
| Security | Dedicated-account operation; checked root-to-user drop; separate unsafe daemon/CLI switches; protected database, config, and socket paths. |
| Installation | CMake install target, DESTDIR staging, explicit public-header closure, reusable component exports, and external consumer builds. |
| Packaging | CPack runtime/development TGZ artifacts, Linux systemd assets, macOS launchd assets, and explicit activation instructions. |
| Operations | Installation, configuration, retention, observability, backup/restore, failure recovery, and troubleshooting documents under `docs/`. |

Distro-specific DEB/RPM maintenance, Homebrew formula publication, signed/notarized macOS installers, Windows service implementation, automatic account creation during CMake installation, and release publication are outside this phase. TGZ artifacts must be described honestly as platform/toolchain-specific packages with external runtime dependencies, not self-contained binaries.

### 1.2 Implementation discipline

Implement one approved stage at a time. Small intermediate gaps are acceptable; each stage must have a focused verification boundary. Linux delivery precedes macOS-specific work. The final Linux gate follows the optional native macOS gate.

Apply repository `AGENTS.md`: useful public Doxygen; comments explaining ownership, ordering, transactions and failure behavior; clear logical sections; selective `[[nodiscard]]`; no routine catches or `@throws` for allocation failure. Every stateful Object subclass extends the single ObjectPrivate block, with owner binding after base construction. Reusable observable events use receiver-aware signals. Transaction strategies and one-operation completion callbacks remain appropriate.

Do not repeat a full SQLite-disabled test matrix. Use an SDK consumer check to prove component independence and focused target checks when build boundaries change. Do not rename errors or public APIs unrelated to this phase.

## 2. Current code and proposed ownership

### 2.1 Important baseline facts

1. `RetentionRepository::purge_batch(cutoff, limit)` exists, but applies one global cutoff to runs and idempotency expiry and is not driven by a daemon maintenance timer. It does not implement per-queue policy. Do not connect it to a timer unchanged.
2. Creation records currently use `expires_at = nullopt`. Retention needs a resource-lifetime policy, not an arbitrary expiry measured from creation.
3. `SchedulerCore::dispatch_visit()` stops candidate discovery when resource/queue capacity is full. Instrumenting only its selected candidates would miss precisely the work whose capacity wait needs measuring.
4. `TimeSource` already provides both UTC and monotonic clocks. No new clock abstraction is needed.
5. Statistics already expose optional `runnable_wait_ms` and provenance `runnable_wait="unavailable"`. Keep existing schedule-lateness and execution-wall-duration meanings.
6. `IniFile` supports flat keys, repeated values, and file includes; its boolean accessor treats unrecognized values as false. Daemon security options require stricter parsing.
7. `jobud/main.cpp` currently requires socket/database flags and constructs Application/runtime before opening the database. Privilege finalization must precede all worker-capable objects.
8. `LocalServer` rejects existing paths and applies mode before listening. Crash-restart stale-path handling belongs to daemon ownership code, not an unconditional unlink in this reusable class.
9. CLI execution already requests Linux privilege-gain prevention through Process. Preserve this check; do not redesign Process or claim an equivalent macOS kernel feature.
10. Project targets are static and expose source directories and PUBLIC source files. Public Object/Signal templates include internal support headers. Installation must audit their transitive header closure rather than copying only the visibly public list.

### 2.2 Files and target boundaries

| Owner | Files to add or extend |
| --- | --- |
| Generic retention | `src/jobu/retention.hpp`, `retention.cpp`, `retention_repository_priv.*`, existing run/idempotency repositories. |
| Generic telemetry | `src/jobu/execution_telemetry.hpp/.cpp`, `execution_telemetry_priv.hpp`, `wait_repository_priv.hpp/.cpp`. |
| Timing integration | `management.*`, `scheduler.*`, `scheduler_core_priv.*`, `scheduler_dispatch_priv.*`, `scheduler_repository_priv.*`, `recovery.*`, run insertion paths. |
| SQLite format | `src/jobu/sqlite/sqlite_schema.*`, current manifest and index validation. |
| Public statistics | `statistics.*`, `statistics_repository_priv.*`, result codecs, CLI statistics renderer, typed client tests. |
| INI parsing primitive | `src/core/ini_file.*`: small text-input entry point; existing file constructor remains available. |
| Daemon configuration | `src/jobud/configuration_priv.*`, `startup_priv.*`, generated `jobu_paths_priv.hpp`, `main.cpp`. |
| Process identity | `src/jobud/privileges_priv.hpp`, `privileges_posix.cpp`, Linux/macOS verification helpers. |
| Runtime filesystem | `src/jobud/runtime_paths_priv.*`, platform helpers where native APIs differ. |
| Runtime composition | `src/jobud/runtime_priv.*`: maintenance/timing ownership, signals, startup and shutdown order. |
| Logging | Small structured-field addition to `core/logging.*`; `src/jobud/daemon_logger_priv.*` for JSON/text stderr formatting. |
| Local listener | `src/net/local_server.*`: optional checked group assignment before listen; same ObjectPrivate allocation. |
| Installation | `cmake/JobUPaths.cmake`, `cmake/JobUConfig.cmake.in`, export/install helpers, target CMake files. |
| Service/package assets | `packaging/systemd/`, `packaging/launchd/`, `packaging/jobud.ini.in`, `cmake/JobUPackaging.cmake`. |
| Documentation | `docs/installation.md`, `configuration.md`, `operations.md`, `observability.md`, plus existing CLI/protocol/client guides. |

Private repository code uses `jb::db`; DDL and SQLite PRAGMAs remain in `jobu-sqlite`. POSIX account/path policy stays in `jobud`, not in generic core or database contracts. No timer or service reads raw secret values for observability.

### 2.3 Errors and checked boundaries

Reuse `jb::core::Error` and the existing storage failure classifier/sanitizer. Do not classify failures by searching diagnostic text. Maintenance writes use the existing Mutation context; timing writes use their enclosing Mutation/Dispatch/Completion/Recovery context. Durable timing/key relationship failures have PersistedData origin. Non-storage telemetry invariants still explicitly fail the telemetry/runtime owner.

New stable codes should use these module-owned families; refine suffixes when a concrete call requires a distinct actionable outcome, without exposing raw values:

| Code | Category / handling |
| --- | --- |
| `jobu.retention.invalid_options` | InvalidArgument; reject service start/configuration. |
| `jobu.retention.invalid_relationship` | Internal, persisted-data invariant; stop runtime. |
| `jobu.telemetry.invalid_options` | InvalidArgument; reject start. |
| `jobu.telemetry.invalid_state` | Internal; mixed ownership, missing row or illegal interval; stop runtime. |
| `jobu.telemetry.clock_regression` / `jobu.telemetry.counter_overflow` | Internal; stop runtime rather than report wrapped/clamped measurements. |
| `jobud.config.invalid` / `unknown_key` / `duplicate_key` | InvalidArgument; local startup/check failure with safe key/line metadata. |
| `jobud.config.read_failed` | Io or PermissionDenied according to the checked operation; never echo file contents. |
| `jobud.privilege.root_disallowed` / `drop_failed` | PermissionDenied; fail before resource admission. |
| `jobud.path.unsafe` | PermissionDenied; fixed reason token, no automatic destructive repair. |
| `jobud.socket.in_use` | Conflict; preserve existing endpoint/lock. |
| `jobud.socket.inspect_failed` | Io; preserve ambiguous endpoint. |

Propagate real backend error codes through the existing sanitizer instead of replacing every I/O failure with a telemetry code. No new RPC method/error transport is needed. Service `start()` and healthy `finish_stop()` return checked `Result<void, Error>`; `stop()`/`request_stop()` are idempotent, non-SQL void operations. No user signal is emitted before transaction cleanup.

## 3. Fresh format 4

### 3.1 Persistent additions

Advance the single current-format marker to **4**. Keep all format-3 domain constraints, including terminal-once rules and the single nonterminal schedule-owned occurrence index. Add one row per run for timing, separate from immutable payload/attribute snapshots:

```sql
CREATE TABLE jobu_run_timing (
    run_id BLOB PRIMARY KEY NOT NULL
        CHECK (typeof(run_id) = 'blob' AND length(run_id) = 16),
    runnable_wait_us INTEGER NOT NULL DEFAULT 0
        CHECK (typeof(runnable_wait_us) = 'integer' AND runnable_wait_us >= 0),
    measurement_status TEXT NOT NULL DEFAULT 'unmeasured'
        CHECK (measurement_status IN ('unmeasured', 'complete', 'partial')),
    open_epoch BLOB CHECK (open_epoch IS NULL OR
        (typeof(open_epoch) = 'blob' AND length(open_epoch) = 16)),
    open_tick_us INTEGER CHECK (open_tick_us IS NULL OR
        (typeof(open_tick_us) = 'integer' AND open_tick_us >= 0)),
    delay_warned INTEGER NOT NULL DEFAULT 0
        CHECK (typeof(delay_warned) = 'integer' AND delay_warned IN (0, 1)),
    CHECK ((open_epoch IS NULL) = (open_tick_us IS NULL)),
    CHECK (measurement_status <> 'unmeasured' OR
        (runnable_wait_us = 0 AND open_epoch IS NULL AND delay_warned = 0)),
    FOREIGN KEY (run_id) REFERENCES jobu_runs(id) ON DELETE CASCADE
);
```

Epoch is one freshly generated UUID per telemetry activation; ticks are elapsed microseconds from that activation's monotonic origin. They are private accounting state, never public timestamps and never compared across epochs. These values are not a reconstruction of elapsed downtime.

Insert timing rows with runs in their existing transactions, including manual runs, recurring successors, recovery-created successors, and test fixtures. A missing row in format 4 is an invariant failure. An embedded caller without telemetry creates an explicitly unmeasured row, not a fabricated zero-duration sample.

Production runs covered by telemetry **from creation** begin as complete with zero accumulated wait, even if future-dated and currently ineligible. Mark this in the insertion transaction, not by later converting an old unmeasured row. A successor created during pre-admission recovery can likewise begin complete: no eligible online interval has elapsed since its creation. Existing unmeasured runs first observed later become partial. This distinction is essential; otherwise every newly created run would incorrectly enter the partial bucket.

Add an index on `jobu_runs(queue_id, completed_at_us, id)` suitable for per-queue terminal retention, and `jobu_idempotency(scope_id, method, resource_id)` for ownership guards. Reuse the existing resource-id index. Index current open timing rows and not-yet-warned rows only if the concrete checkpoint/warning queries need it; a small application-owned partial index is acceptable. Verify query plans before adding redundant indexes.

### 3.2 Validation and faults

Validate tables, required columns, index shapes, foreign keys, and timing constraints using the existing schema owner. Preserve current job-table constraint verification. Add initial/final domain checks for timing-row presence, nonnegative counters, valid status, paired open fields, and absence of an open interval on a terminal/running run after reconciliation.

Current-format creation, marker write and validation commit atomically. Formats 1–3 and newer formats are rejected without mutation. Remove no existing safe cleanup or uncertain-commit behavior. Do not add an upgrade path merely because a test fixture still creates format 3: update that fixture to the current manifest unless it is testing rejection.

## 4. Retention and idempotency

### 4.1 Exact retention rule

`history.default_retention` defaults to **2,592,000 seconds (30 days)**. Queue `history_retention` retains its existing meaning: null inherits the current daemon default, zero is unlimited, positive is an override.

For each retained run, use its own persisted `queue_id` and that queue's current retention policy, including a deleted queue. Do not use the job definition's current queue after a move. Policy edits affect future cleanup immediately; payload and execution attributes remain snapshotted as before.

Delete only Succeeded, Failed, Interrupted or Cancelled runs with a valid `completed_at` **strictly earlier** than the effective cutoff. At equality retain the row. Keep all Scheduled, Running and RetryWait runs and their attempts/output, however old the earlier attempts are. Cancellation ages from terminal completion, not original planned time. Zero/unlimited must not be converted to a cutoff of now.

Compute the UTC cutoff in C++ with checked/saturating arithmetic at the representable durable minimum. A retention duration longer than the available timestamp range means nothing is old enough; do not overflow seconds-to-microseconds conversion. Sample UTC once for a sweep. Clock rollback postpones cleanup; a forward correction may make history expire. This is a wall-clock retention policy, not monotonic elapsed lifetime.

Deleting a run cascades its attempts, outputs and timing row in the same transaction. Preserve the durable terminal job definition. Terminal definitions are physically purged only after explicit soft deletion and the guards below; retention does not silently delete a finished definition or its current secret references.

### 4.2 Bounded and fair selection

Replace the single-cutoff repository entry point with a paged sweep interface. Suggested private shapes:

```cpp
struct RetentionSweepCursor {
    std::optional<jb::core::Uuid> after_queue;
    // Also tracks the keyset for the current queue and the cleanup phase.
};

struct RetentionBatchResult {
    RetentionPurgeCounts purged;
    RetentionSweepCursor next;
    bool sweep_complete{false};
};

auto purge_next_batch(UtcTimePoint sweep_now,
                      std::chrono::seconds daemon_retention,
                      std::size_t limit,
                      RetentionSweepCursor const& cursor)
    -> Result<RetentionBatchResult, Error>;
```

Use queue UUID keysets and `(completed_at_us, id)` run keysets. Apply the terminal/cutoff predicate in SQL before LIMIT. Skip unlimited queues while advancing the cursor. Give each queue at most one run-delete batch per sweep visit so a large queue cannot starve others; reset sweep cursors after the final owner/metadata phase. A later sweep reaches rows inserted or made eligible behind the current cursor.

One invocation performs one bounded phase transaction, with at most `batch_size` parent deletions. It returns to the event loop before another batch. Do not hold a query or transaction across yields. Recheck state/policy and ownership in the deletion transaction; the single owner thread prevents interleaving within it.

The bound is on parent records, not the total number of cascaded attempts or BLOB bytes. Document that distinction and measure a run with many attempts/large retained output. Do not deserialize output or complete run payloads to decide expiry.

### 4.3 Resource-lifetime idempotency policy

Do not expire every creation key after 30 days from creation. A job scheduled a year ahead must still replay its creation response after a month, and live recurring creation keys must remain valid.

| Record | Retain while | Retirement point |
| --- | --- | --- |
| `queue.create` | The queue remains live, or deleted ownership/history still needs it. | Deleted queue is otherwise physically purgeable; remove its creation record in the guarded owner-purge transaction. |
| `job.create`, recurring | Definition remains live, including Suspended, or retained runs remain after deletion. | Explicitly deleted definition has no retained runs; retire keys and definition together. |
| `job.create`, one-time | Definition has any nonterminal or retained terminal run. | Last retained run has expired and definition is terminal/deleted; creation key may then be retired even if a terminal definition is kept for inspection. |
| `job.run_now` | The referenced manual run remains retained. | Remove its replay record in the same transaction that purges that terminal run. |

Keep the original request/result JSON unchanged until retirement. Canonical comparison and exact replay semantics do not change. After retirement, a reused key is a new request and current eligibility rules apply; terminal Run Now is still rejected. Document this boundary for retrying clients.

The existing nullable `expires_at` column is not a substitute for these guards. New production records may remain null and be retired by resource lifetime. If the generic `erase_expired` helper is retained for tests/other callers, the daemon must not use it to bypass lifetime rules. Prefer removing unused expiry plumbing over introducing a second production policy.

Validate method/scope/resource relationships before deletion; unknown or inconsistent owned records are corruption, not permission to delete unrelated data. Add internal helpers for deleting matching method+scope+resource records rather than a broad resource-only delete where IDs have different meanings.

The valid relationships are concrete: `queue.create` uses the nil UUID scope and queue resource; `job.create` uses its **original creation queue scope** and job resource; `job.run_now` uses the owning job scope and manual-run resource. Validate a job creation record against its stored canonical request/replay queue, not the definition's current queue after a move. Moving a definition does not rewrite its creation key or make that original scope corrupt. A deleted original queue may consequently remain retained while a moved live definition's creation record still protects it. Check run-key ownership before deleting the referenced run, within the same transaction.

### 4.4 Deleted definitions and queues

Purge a soft-deleted job only when no runs reference it, no current secret-reference rows remain unexpectedly, and no retained idempotency record protects either its `resource_id` **or its `scope_id`**. Retire eligible creation records and the owner atomically; preserve records associated with another resource/scope.

Purge a soft-deleted queue only when no definitions and no runs reference it, including historical runs of moved definitions, and no surviving idempotency record protects its resource or scope. Keep current synthetic-name reuse semantics. Never purge a live queue, a live recurring definition, or a terminal one-time definition that has not been explicitly deleted.

Run deletion, key retirement and owner cleanup must not create a circular pin that retains everything forever. The repository tests must demonstrate eventual removal of a deleted queue with once/cron/manual history after the applicable finite retention expires.

### 4.5 Service and runtime contract

Add `RetentionService final : jb::core::Object` in `jobu` with one Private block and borrowed database, attribute registry and TimeSource. Public options:

```cpp
struct RetentionOptions {
    std::chrono::seconds default_retention{2'592'000};
    std::chrono::seconds sweep_interval{60};
    std::chrono::milliseconds inter_batch_delay{10};
    std::size_t batch_size{100}; // accepted range 1..1000
};
```

`start()` validates options/affinity and arms one timer; it does not purge synchronously. `stop()` is idempotent and prevents further batches. Emit `batch_completed(RetentionPurgeCounts)` after commit and `failed(Error)` after transaction cleanup. Signals carry owning values. Destruction disarms work; queued callbacks have an Object lifetime guard.

Start after normal runtime readiness, not before recovery. A completed sweep waits `sweep_interval`; an incomplete one yields at least `inter_batch_delay`. Unlimited default does not disable deleted-owner or independently eligible key cleanup, nor finite queue overrides.

Any storage/invariant failure in maintenance uses the mutation failure policy and closes daemon admission through `DaemonRuntime::fail`. Do not continue after poisoned rollback or uncertain commit. Stop maintenance before shutdown cleanup; it writes nothing after failure. No separate cleanup thread, VACUUM timer, manual purge RPC or automatically scheduled backup is added.

## 5. Runnable-wait accounting

### 5.1 Measurement definition

Keep existing schedule lateness and execution wall duration unchanged. New runnable wait is **the sum of monotonic intervals during which a run is observed eligible for selection, ignoring capacity and fairness selection**. It is measured per run across its attempts, not per queue and not by subtracting `runnable_at` from `started_at`.

| Condition | Accumulate wait? |
| --- | --- |
| Scheduled/RetryWait, due, queue Active, automatic job Active, type available, no manual barrier | Yes, including queue/global capacity exhaustion and fairness/priority competition. |
| Due accepted manual run, job Suspended/Suspending but queue Active | Yes, under existing manual suspension bypass. |
| Future planned/runnable time or retry backoff not yet due | No. |
| Queue Suspended/Suspending/Deleted | No. |
| Automatic work for a Suspended/Suspending/terminal/deleted job | No. |
| Schedule-owned run blocked by accepted nonterminal manual work | No. |
| Running attempt, terminal run, unavailable executor type, daemon not admitting work | No. |
| Due blocking retry that already owns a queue slot but cannot get a runner | Yes. Its retained slot does not remove runner-capacity wait. |

“Observed” is deliberate: UTC deadlines become eligible when the owner-thread scheduling pass observes them. Do not retroactively infer a monotonic crossing time across a wall-clock correction. This includes at most the scheduler's observation latency at such boundaries; expose the provenance as `monotonic_observed`, not an exact reconstruction from UTC. Ordinary state changes use their transaction's shared time sample, so suspension and barrier changes do not wait for the next periodic scan to stop charging.

### 5.2 Shared telemetry owner

Add `ExecutionTelemetry final : Object`, borrowed by ManagementService and Scheduler and owned by DaemonRuntime. It owns the activation epoch/origin, maintenance timer state, and warning delivery. It uses a private `WaitRepository`; it neither starts executors nor decides which queue wins.

Public surface includes options, `start()` after recovery, `request_stop()` (latch/disarm only), checked `finish_stop()` for healthy cleanup, and `Signal<DelayedRun> delayed` / `Signal<Error> failed`. Transaction-local helpers are private, accessed through one narrow private adapter used by management and scheduler. Do not expose a callback for every row or duplicate this service's state inside Scheduler.

```cpp
struct TelemetryOptions {
    std::chrono::seconds checkpoint_interval{30};
    std::size_t batch_size{200}; // 1..1000
};

struct DelayedRun {
    jb::core::Uuid run_id;
    jb::core::Uuid job_id;
    jb::core::Uuid queue_id;
    JobType type;
    std::chrono::microseconds runnable_wait;
    std::chrono::milliseconds threshold;
};
```

Add an optional borrowed telemetry pointer to the service/scheduler construction options, documented to outlive them. Production runtime always supplies it. Embedded users may omit it; their measurements remain explicitly unmeasured/partial. This avoids making every generic management-only test create a daemon. No operation may leave an open telemetry interval when invoked without an active telemetry owner; close such intervals as partial or reject invalid mixed ownership before mutation.

The owner borrows Database, AttributeRegistry, TimeSource and UuidGenerator; options are copied. Runtime's executor set is fixed before telemetry activation. Give the private adapter a validated snapshot of available job types from that actual set, including root-policy unavailability; management and scheduler consult the same snapshot. No generic service invents an available CLI runner merely because a CLI concurrency limit is positive. Embedded construction through Scheduler supplies the same registration step. Replacing executor registrations after activation is outside v1; stop/recreate the owner rather than silently changing eligibility.

### 5.3 Transaction-local interval algorithm

Sample `(utc_now, epoch, tick_us)` once per logical transaction/cycle boundary. Reject negative/overflowing or regressing monotonic deltas as an internal clock/invariant failure; do not wrap or silently clamp accumulated values.

Private operations should have shapes equivalent to:

```cpp
settle_scope(scope, sample);    // add tick-open_tick for open current-epoch rows; close them
reconcile_scope(scope, sample, available_types); // reopen only rows eligible in current durable state
checkpoint_batch(sample, cursor, limit);        // add deltas and rebase still-open intervals
```

They require a caller-owned transaction, finish all queries before returning, and return effects/warnings as owning values. They emit no signals and do not begin nested transactions. `scope` can be one run, one job, affected queues, or a keyset page. An old epoch is never subtracted from a new epoch's tick.

For a mutation that changes eligibility, settle the affected old scope **before** the domain write, apply the write, then reconcile the affected new scope using the same sample. Commit all counters and domain state together. On rollback, keep the old open interval intact so a failed suspension does not lose wait time. A no-op/replayed mutation need not write telemetry or emit warnings.

Cover queue suspend/resume/update/delete; job suspend/resume/update/move/delete; creation and Run Now; dispatch; pending cancellation; completion/retry; recurrence/barrier release. Queue scope includes all affected pending runs; manual creation/completion/cancellation includes its job's schedule-owned sibling. A move settles against the old queue before evaluating the new one. Payload/schedule replacement of a still-unstarted run preserves its measured operational wait so far; this does not resolve #230's snapshot question.

The helper's eligibility predicate must share the scheduler's rules and include validated schedule kind and manual relationships. It deliberately omits capacity limits. Add a light projection rather than loading payloads, outputs or complete attribute documents for every waiting row.

### 5.4 Discovery, wakes, and bounded memory

Before capacity arbitration, reconcile due pending work even if every runner or queue slot is full. Scan with keyset pages of small projections. Do not stop after the first candidate page or include only jobs with a free queue slot. This is mandatory for accurate warnings on a backlog.

Existing startup/owner mutations and scheduler cycles are serialized. A cycle may traverse multiple bounded pages synchronously; memory remains bounded, but its total work is proportional to the affected pending set. Do not claim an O(1) cycle or a fixed latency bound. Measure this cost in the scalability stage and optimize shared eligibility/index queries before inventing rollups or per-run timers.

Use existing future-runnable UTC wake logic for due boundaries, with the wall-clock recheck cap. Track the next warning deadline in monotonic time and combine it with dispatch and maintenance wakes. Extend the private cycle result to carry a monotonic warning deadline separately; never convert a monotonic accumulated wait into a fabricated UTC scheduled time. Keep at most a small fixed number of service timers, not one per run or queue.

Checkpoint open intervals in bounded transactions every sweep, yielding between pages. Checkpoint progress does not alter scheduling order or fire `mutation_committed`. Large backlogs can extend a checkpoint sweep; there is no promise that a crash loses at most exactly 30 seconds.

### 5.5 Dispatch, retry, cancellation and shutdown

Settle and close a run's wait interval in the same transaction that commits its Running attempt, before external start. Keep that interval closed through preparation/start failure handling. A rollback retains the prior waiting interval; an uncertain commit fails closed. Preparation failure still has a measured wait if the run was eligible.

When a completed attempt retries, retain the accumulated run total and keep the interval closed through backoff. Reopen when due and otherwise eligible. Count no child execution time, HTTP time, queue suspension or daemon downtime as runnable wait. Finish cancellation and its timing changes atomically; never reopen terminal runs.

At ordinary shutdown, `request_stop()` captures the owner-thread stop sample and disarms observation without doing SQL on a signal/failure stack. Once the runtime unwinds, healthy `finish_stop()` settles/closes remaining open intervals at that sample in bounded transactions before database close. It must not finalize Running attempts or change existing immediate-shutdown recovery semantics. A fatal/poisoned shutdown does no best-effort persistence.

### 5.6 Crash/restart quality

Before telemetry activation, recover all timing rows with an old open epoch in bounded recovery units: retain the last checkpointed sum, clear open fields, and mark **partial**. Never subtract an old tick, add wall-clock downtime, or claim the uncheckpointed tail was recovered. Preserve `delay_warned`.

Closed complete rows remain complete across a clean restart. A run previously unmeasured that becomes instrumented is partial, because its earlier wait is unknown. Partial never becomes complete again. An unmeasured row remains excluded from measured averages until observation begins.

Keep this cleanup inside pre-admission recovery, before ordinary scheduler startup. Integrate current interruption handling without changing job outcomes or immutable snapshots. Partial progress is safe to repeat after a crash; each row is repaired once. A current-epoch open row must correspond to pending eligible work at validated boundaries; final recovery has no open old-epoch rows.

Recovery may use a static/private repository routine before the new telemetry owner activates its epoch. The cleanup never needs an old epoch's monotonic origin. Keep a complete row closed during an interrupted Running attempt; execution and downtime are not eligible waiting intervals, so interruption alone does not make an otherwise fully observed wait partial.

### 5.7 Delay warning guarantee

Use the queue's existing `runnable_wait_warning`, default **10,000 ms**. Zero disables warnings without disabling measurement. Warn when the cumulative known runnable wait is **greater than** the positive threshold and the run is currently eligible; lowering the threshold can make the next observation warn immediately.

For an open interval below the threshold, schedule the next check at the first representable monotonic tick strictly beyond the threshold, rounded safely to timer resolution. Do not re-arm a zero-delay loop at equality. If a dispatch and warning become due together, settle/evaluate the pre-dispatch eligible interval inside the claim transaction and deliver any warning only after successful commit; beginning Running must not erase an already-earned warning.

One run gets at most one warning across retries, moves and restarts. Set `delay_warned` durably in a compare-and-update transaction before queuing the owning `DelayedRun` signal. Only the successful claimant emits. This provides at-most-once emission, not guaranteed delivery: a crash between commit and logging can lose a warning, which is acceptable for diagnostics. Do not reset the flag on retry, threshold change or log-level change.

Never emit a user-callable signal while a database transaction/query remains active. The scheduler/runtime must tolerate a delayed-event receiver requesting shutdown. Log fixed event names and IDs/counts only; no job name, command, arguments, environment, HTTP URL, headers, payload or secret bytes.

## 6. Statistics and protocol changes

Extend the existing statistics repository joins to use timing rows once per run, without multiplying totals by attempt or output joins. Keep the existing planned-cohort window, group dimensions, cursors, limits, response budget and live-view semantics. Retention may remove members between pages exactly as other concurrent changes can.

`runnable_wait_ms` uses only **complete, terminal, measured runs** for its `StatisticsDuration` samples/average/maximum. Waiting/running totals are not finalized samples. Add explicit per-group coverage counts:

```json
{
  "runnable_wait_ms": {"samples": 2, "average": 1250.0, "maximum": 2000.0},
  "runnable_wait_coverage": {"complete": 2, "partial": 1, "unmeasured": 0, "unfinished": 3}
}
```

The four coverage buckets are mutually exclusive and sum to group run count: classify nonterminal runs as unfinished first, then terminal runs by status. Do not include partial lower bounds in an apparently exact average. For no complete terminal samples, return a duration with zero samples and null average/maximum; do not report zero latency. A wholly uninstrumented embedded service may keep `runnable_wait_ms=null` with `runnable_wait="unavailable"`.

Set production provenance `measurement.runnable_wait="monotonic_observed"`; keep `measurement.timing="wall_clock_derived"` for the two pre-existing wall-clock measures. Document their distinct scopes. No SQL writes or forced checkpoints occur merely because a statistics RPC is read.

Keep all 30 existing RPC methods. Use API **1.4** for the new measured statistics fields and optional-timezone request support in §8; retain major 1, capability-based method checks, and unknown-response-field tolerance. No dual protocol implementation or old-client compatibility scaffolding is needed. Update `system.info`, codecs, CLI renderer, client fixtures, and introduction-version tables together. Explicit timezone requests retain their meaning.

## 7. Structured operational logging

Extend the existing `jb::core` facade with a small typed field facility rather than embedding unescaped JSON in ConsoleLogger text. Suggested types: `LogField{name, variant<bool,int64_t,uint64_t,double,string_view>}` and an optional borrowed field span in `LogMessage`; add `log_event(level, event_name, fields, source_location)`. The existing formatted `log_*` API remains intact.

Fields and string views are borrowed only during the synchronous Logger call; asynchronous/custom sinks must copy. Do not put owning Object pointers or callbacks in records. Add Doxygen and tests for escaping, finite numbers, thread-safe output and legacy logs without fields. The daemon's JSON logger writes one complete JSON object per stderr line; text mode prints the same safe fields in a readable form. Output errors do not recursively log or throw.

Use fixed envelope keys for time, level, event/message and thread identity, with user fields in a nested `fields` object to avoid envelope collisions. Duplicate field names retain the first value; nonfinite numbers serialize as null, and malformed UTF-8 is replaced with U+FFFD so a record remains valid JSON. Do not interpolate unescaped field names/values into a format string. These formatting fallbacks must not log another error recursively.

Events include startup/ready/stop, unsafe overrides, recovery summary, delayed run, retention sweep summary, and fatal subsystem/code. Use severity Info for lifecycle/summary, Warning for delays/unsafe choices, Error for failures; keep Debug diagnostic detail sanitized. Avoid one Info record per purged run or per scheduler scan. IDs are useful correlations; arbitrary request values are not trusted event names or fields.

Default format is JSON and level Info. Development can choose text. Service managers or administrator-configured logging own rotation and retention; do not add a daemon log-file/rotation subsystem. In particular, launchd file redirection alone does not provide rotation. No secret values, full config dump, backend SQL/error detail or unsanitized HTTP credentials enter logs. Peer UID/GID/PID already captured by runtime may be included in connection audit events where available, but do not add per-user authorization.

## 8. Configuration and defaults

### 8.1 Types and parsing boundaries

Keep `main.cpp` a composition function. Split startup handling into three private steps:

1. `parse_startup_arguments(...) -> Result<StartupArguments, StartupError>`: parse explicit flags, preserving absence separately from false/zero, plus local help/version/check actions.
2. `load_configuration(...) -> Result<ConfigurationInput, StartupError>`: securely read one bounded file, parse flat INI, reject unknown/duplicate keys, then apply typed validation.
3. `resolve_startup_options(arguments, input, compiled_paths) -> Result<StartupOptions, StartupError>`: precedence, cross-field checks, absolute paths, and immutable options for every service.

`StartupError` carries a stable code, optional safe key name and line number, and a fixed diagnostic. It must not retain the rejected raw value. Avoid echoing existing `IniFile` conversion errors that include values. Startup failures use stderr and a nonzero exit; usage/configuration failures use exit 2, runtime/startup-resource failures exit 1, and successful local actions exit 0.

Add `IniFile::from_text(std::string_view)` returning an `IniFile` whose `ok()/error()` behavior matches file construction. Share the existing lexical parser; do not create a second INI grammar. The text entry point performs no filesystem access and rejects `include` directives. Preserve file-constructor include support for other core users. Add a documented value-free parse diagnostic accessor if necessary to avoid logging source text. Security policy remains in `jobud`.

The daemon opens the selected file with checked ownership/type and no final-component symlink following, reads at most **64 KiB**, and passes the owned text to `from_text`. Repeated keys are errors in daemon configuration even though the generic parser permits them. Section headings, includes, unknown keys, embedded NULs, malformed UTF-8, and trailing number garbage are rejected. Full-line `#`/`;` comments remain supported; there are no inline comments. No environment-variable, tilde, command, or secret substitution is performed.

Daemon booleans accept `true`, `1`, `on`, `yes` and `false`, `0`, `off`, `no`, with ASCII case-insensitive words; reject every other token. Do not use the generic permissive boolean accessor, which maps unknown tokens to false. Bare interval values count seconds, while lowercase `s`, `m`, `h`, and `d` suffixes select seconds, minutes, hours, and days. Bare byte quantities count bytes, while lowercase `k`, `m`, and `g` suffixes use 1024-based multipliers. Parse the unsigned number with full consumption; reject signs, trailing garbage, and overflow before multiplying or checking each key's range. Paths and URLs have their own validators. Configuration is read once at startup; no SIGHUP reload or mutable shared configuration object is introduced.

### 8.2 Precedence and command-line behavior

Precedence is **compiled defaults < configuration < explicitly supplied flags**. Parse flags before reading the file so `--config`, `--no-config`, `--help`, and `--version` have predictable behavior.

- `--config PATH` requires that exact file. A missing/unreadable/invalid explicit file fails startup.
- Without it, use the compiled default config path if present; an absent default permits foreground operation with defaults and flags. Other read failures remain errors.
- `--no-config` skips config loading and conflicts with `--config`. It is useful for deterministic tests and explicit development invocations.
- `--check-config` resolves and validates configuration locally, including readonly account/timezone checks, then exits. It creates no directories, changes no identity, opens no database, starts no Application/HTTP worker, acquires no daemon lock, and opens no socket. Its success does not promise the later resource operations will succeed.
- `--help` and `--version` exit before config or account lookup. Help documents defaults, precedence, units, security switches and check mode.
- Preserve existing `--socket`, `--database`, concurrency, HTTP proxy/CA and `--allow-root-cli` flags. Add `--run-as-user`, `--run-as-group`, `--allow-root-daemon`, and explicit `--no-allow-root-cli` / `--no-allow-root-daemon` overrides for configuration booleans. Contradictory or repeated instances of one setting are errors.

Do not expose every maintenance limit as another CLI flag. The config file is the complete startup policy surface. No option prints the full resolved configuration with values. A safe check result can report the selected config filename and success only.

### 8.3 Configuration keys

The following table is the complete initial key set, apart from the registry-driven `defaults.*` namespace. Omitted values use the defaults shown. Empty optional values mean unset; an empty required path/user override is invalid.

| Key | Default | Validation / consumer |
| --- | --- | --- |
| `database.backend` | `sqlite` | Only `sqlite` in v1; reject unsupported names. Generic db API remains backend-independent. |
| `database.path` | compiled database path | Absolute config path; protected regular SQLite file/parent. |
| `socket.path` | compiled socket path | Absolute, fits native UNIX path limit, protected parent. |
| `socket.owner` | final user | Optional account name; it must resolve to the daemon's final UID. Arbitrary socket ownership different from daemon identity is rejected in v1. |
| `socket.mode` | `0600` | Exactly `0600` or `0660`; parse as explicit octal spelling, never decimal 600. |
| `socket.group` | final primary group | Optional group name; must be a final primary/supplementary group. Used with checked group assignment before listen. |
| `daemon.run_as_user` | unset | Account name resolving to final identity; root startup requires a non-root target unless unsafe override is explicit. |
| `daemon.run_as_group` | target account's primary group | Optional group name, resolved before privilege drop; cannot be set without a user override. |
| `daemon.allow_root` | `false` | Explicit unsafe permission to retain root daemon identity. |
| `cli.allow_root` | `false` | Separate existing unsafe permission for root CLI execution; does not permit a root daemon by itself. |
| `cli.concurrency` | `4` | Existing positive uint32 range, 1..4294967295; zero is invalid. |
| `http.concurrency` | `16` | Existing positive uint32 range, 1..4294967295; zero is invalid. |
| `http.proxy` | unset | Existing validated explicit HTTP proxy contract; never log credentials/URL. |
| `http.ca_bundle` | system trust | Absolute readable regular file when supplied; checked again under final identity by HTTP setup. |
| `schedule.default_timezone` | `UTC` | Valid IANA zone supported by the existing CronEngine. |
| `history.default_retention` | `30d` (2592000 seconds) | Nonnegative; zero unlimited; checked durable-time range. |
| `history.sweep_interval` | `1m` (60 seconds) | 1..86400 seconds. |
| `history.batch_size` | `100` | 1..1000 parent records. |
| `telemetry.checkpoint_interval` | `30s` | 1..86400 seconds; a sweep interval, not a guaranteed crash-loss bound. |
| `rpc.header_limit_bytes` | `16k` (16384 bytes) | 1024..65536 bytes. |
| `rpc.body_limit_bytes` | `1m` (1048576 bytes) | 1048576..16777216 bytes; existing narrower method-specific request/result limits remain. |
| `rpc.max_batch_entries` | `64` | 1..64; keep the existing application budget. |
| `rpc.max_connections` | `128` | 1..4096. |
| `rpc.queued_output_bytes` | `2m` (2097152 bytes) | At least body limit + header limit; at most 64 MiB, with checked addition. |
| `logging.level` | `info` | `fatal`, `error`, `warning`, `info`, `debug1`, `debug2`, `debug3`. |
| `logging.format` | `json` | `json` or `text`. |
| `defaults.<registered attribute name>` | existing built-in registry layer | One strict JSON value per key, decoded and validated by the existing attribute registry. |

Fixed implementation bounds remain fixed: JSON depth 64, telemetry batch size 200, retention inter-batch delay 10 ms. Derive accepted-socket read-buffer capacity from the configured frame bound with checked arithmetic, so transport defaults do not silently defeat a larger RPC limit. Keep outgoing response and queue limits consistent; resource exhaustion must use existing RPC error behavior, not truncate valid JSON.

Do not invent a parallel default-attribute type system. Assemble the `defaults.*` entries into the same attribute object accepted by existing codecs, then validate both individual and cross-field rules. Defaults are materialized through the existing built-in → daemon → queue → job layering. Restarting with different defaults does not mutate already stored job/run snapshots. Output capture limits remain governed by those attributes and existing hard caps.

JSON values are interpreted **after the existing INI unquoting rule**. For a JSON string use an outer INI single-quoted wrapper, e.g. `defaults.retry.mode = '"reschedule"'`; the parser removes only the outer single quotes, preserving the JSON double quotes. Numbers/booleans can be unquoted, and objects/arrays retain their normal JSON syntax. Test this two-layer boundary and show it in the configuration reference. Do not change the generic INI quote behavior or invent escaping that it does not implement.

Illustrative excerpt of the installed template, using substituted absolute paths; the exhaustive commented sample is a Stage 9.22 deliverable specified in §11.1:

```ini
# Flat dotted keys; no section headers or inline comments.
database.backend = sqlite
database.path = @JB_JOBU_STATEDIR@/jobu.sqlite3
socket.path = @JB_JOBU_RUNDIR@/jobud.sock
socket.mode = 0600
cli.concurrency = 4
http.concurrency = 16
cli.allow_root = false
daemon.allow_root = false
schedule.default_timezone = UTC
history.default_retention = 30d
history.sweep_interval = 1m
history.batch_size = 100
telemetry.checkpoint_interval = 30s
rpc.header_limit_bytes = 16k
rpc.body_limit_bytes = 1m
rpc.queued_output_bytes = 2m
logging.level = info
logging.format = json
```

The managed-service template runs as the dedicated account itself; it does not require root in the daemon or populate a misleading root-drop setting. Document root-to-user flags separately for deliberate foreground launches. Config paths must be absolute; explicitly relative command-line paths are resolved against the original invocation directory before identity/path changes, then subject to the same trust checks.

### 8.4 Default timezone without ambiguity

Stored `CronSchedule` continues to contain an explicit, nonempty timezone. Introduce an input-only `CronScheduleInput { expression, optional<string> timezone }`, with corresponding creation/update/schedule-validation input variants. Do not overload the string `"UTC"` to mean omitted: an explicit UTC request must remain UTC even when daemon default is Europe/Tallinn.

The request codec accepts omitted timezone for cron input and rejects an explicit empty timezone. The server resolves omission using validated `schedule.default_timezone` for new work and returns the fully resolved schedule. Stored-domain and response codecs still require an explicit timezone. A schedule update that omits the entire schedule keeps the old schedule; an explicitly supplied replacement cron schedule with omitted timezone uses the current daemon default.

Add a `ManagementServiceOptions` value carrying daemon attributes, default timezone and optional borrowed telemetry owner. Adapt call sites explicitly; do not retain two competing sets of defaults. The same immutable timezone default is supplied to `schedule.validate` and `schedule.next` registration. Public methods that currently take a stored CronSchedule as request input gain the distinct input contract, with ordinary overloads/conversion conveniences only if useful to existing in-tree callers.

`jobuctl` must stop eagerly inserting UTC when the user omits `--timezone`. Preserve explicit timezone flags and request-file values. Apply the input contract consistently to create, update and schedule commands, their help, typed ControlClient wrappers, and examples.

For idempotent creation, preserve omission in the canonical request and check for an existing replay **before resolving the current timezone default**. A request accepted before a configuration change replays its original fully resolved response. A new key resolves the current default. Do not re-canonicalize old requests with a new configuration or change existing definitions on restart. Add this exact restart/config-change replay test.

Preserve the current ordering that also resolves replay before materializing today's daemon/queue attributes or checking today's secret existence. Test changed daemon attributes alongside changed timezone; neither may overwrite a stored response for the same canonical input.

## 9. Identity, filesystem ownership and startup order

### 9.1 Final process identity

All account resolution and privilege changes happen before Application, HTTP/libcurl worker facilities, database open, recovery, RPC admission or CLI spawn. POSIX identity policy is a daemon concern; no job-level `uid`, `gid`, `sudo` or impersonation feature is added.

| Invocation | Required result |
| --- | --- |
| Ordinary non-root user, no override | Run as current user/groups. |
| Non-root user, configured same identity | Validate consistency; do not attempt a privilege transition. |
| Non-root user, different user/group | Reject before resources. |
| Root with non-root `run_as_user` | Prepare only approved leaf directories if needed, initialize target groups, permanently drop group/user, verify, then continue. |
| Root without target and without `daemon.allow_root` | Reject, even if only HTTP execution is intended or `cli.allow_root` is true. |
| Root with `daemon.allow_root=true` | Continue with a visible unsafe warning; CLI still unavailable unless its separate root override is true. |

Reject setuid/setgid-style mixed real/effective startup identities; binaries are not installed with such bits. If a run-as target is supplied, it takes precedence as the requested final identity even when the unsafe daemon flag is set. Reject a target resolving to UID 0 unless the explicit unsafe daemon override is also present.

Resolve names and copy NSS results before changes. For root-to-user drop, use target-group initialization followed by permanent GID then UID changes, with platform-specific checked calls. Linux must verify real/effective/saved IDs, supplementary groups and absence of retained effective/permitted/ambient capabilities capable of undoing the drop. Do not set keep-caps. macOS verifies the strongest native equivalent and permanent-drop behavior in its native test. Failure exits before resources; do not continue partially dropped.

Test inability to regain root in an isolated helper process. Do not implement a production best-effort privilege regain/repair path. Keep Linux Process `prevent_privilege_gain` checks in the CLI runner and the managed service's `NoNewPrivileges` setting. macOS documentation must not claim `PR_SET_NO_NEW_PRIVS` support.

### 9.2 Directory and file contract

Use an owner-aware path helper with RAII descriptors, `openat`/`fstatat`-style checks where appropriate, close-on-exec descriptors, and native equivalent operations. Avoid check-then-open through attacker-writable parents. Resolve known system parent aliases such as macOS `/var` safely; rejecting every symlink in the entire absolute prefix is not portable. Final sensitive files, endpoint locks and daemon-created leaf directories must not be symlinks.

- Config used by a root invocation must be root-owned and not writable by group/others; its controlling parent chain must not let an untrusted user replace it. A non-root invocation accepts its own or root-owned config under equivalently protected parents. No world-writable config.
- State directory defaults to mode 0700, owned by the final user. Database, WAL, SHM and lock artifacts must remain owner-only. Set umask 0077 before creation. Validate existing files instead of recursively chowning or chmodding a supplied tree.
- Runtime socket directory is owned by the final user, with 0700 for private use or 0750 with the configured trusted administrative group. It must not be group-writable or world-writable.
- A private final-user-owned directory beneath a sticky `/tmp` may be used in tests/development. A raw socket or database in shared `/tmp` does not satisfy this contract. Check the sticky boundary and protected leaf, rather than rejecting all temporary paths or trusting arbitrary shared parents.
- Root may create missing **explicit state/runtime leaf directories** beneath verified parents and assign final ownership before dropping. It must not recursively create/chown arbitrary ancestor trees. Installation instructions provision missing parents. Non-root operation creates only leaves it can safely own.
- Existing insecure ownership/type/mode is an actionable startup error, not a reason to unlink a database, recursively repair permissions or proceed with a warning.

This protects against other users replacing operational paths. Processes with the same final UID already share the daemon's authority; do not claim isolation from a malicious same-UID administrator. Database driver internals remain generic/SQLite-specific as currently separated; do not introduce application account policy into `Database`.

### 9.3 Socket group and stale endpoint ownership

Add an optional numeric group field to `LocalServerOptions`. On a newly bound filesystem socket, set the authorized group, apply the requested mode, verify, then listen. On failure, close and remove only the socket inode created by that server. Keep default group behavior unchanged. This is a small reusable listener capability; it does not change peer credential APIs.

Daemon endpoint ownership uses a separate `${socket_path}.lock` file, opened safely and held with a nonblocking exclusive advisory lock for the entire listener lifetime. This supplements the database exclusive lock: two daemons configured with different databases must not compete for one socket path. Never unlink a held lock file to acquire another inode under the same name.

After endpoint and database ownership are established, an existing socket can be treated as stale only if all of the following hold: protected parent; socket file type; expected final owner; no live listener from a bounded nonblocking connect probe; and unchanged device/inode/type when unlinking. A successful connect, ambiguous/in-progress/backlog-full outcome, or any unexpected error means refuse startup. Only a definite connection-refused stale case permits removal. Symlinks and non-sockets are never stale sockets. Do not remove a path on general connection failure.

Keep this in daemon startup code. `LocalServer` itself continues to reject an existing path. Clean shutdown preserves its current owned-inode-only unlink behavior. Release the endpoint guard only after listener teardown. Crash restart leaves a harmless lock file, reacquires its lock and removes only a proven stale socket.

Mode 0660 gives members of the socket group full daemon authority, including creating CLI work and managing secrets. Describe it as an administrative trust boundary, not read-only monitoring or RBAC. Default 0600 is deliberate.

Group access also requires directory traversal. For mode 0660 require the runtime leaf to be mode 0750 with the configured group; reject an inconsistent inaccessible configuration. In the standard systemd profile, use the dedicated primary group for socket administrators and set RuntimeDirectoryMode=0750 through the documented service variant/drop-in. A different supplementary socket group needs matching administrator-provisioned directory ownership/custom-profile setup. Do not promise group access based on socket mode alone.

### 9.4 Composition and failure order

The final main/runtime sequence is:

1. Local argument actions; bounded config read; typed merge; validate accounts/paths/timezone/options without starting workers.
2. Install the early termination-signal relay, set umask, prepare permitted leaves, finalize and verify process identity.
3. Construct Application and final logger/runtime collaborators. Acquire endpoint ownership and open/lock the database under the final identity; validate/create format 4.
4. Run existing bounded recovery and old-epoch timing repair before admission. Construct telemetry, management, runners, scheduler and their receiver-aware connections with explicit destruction order.
5. Activate telemetry, run scheduler startup, register RPC methods and listen using checked ownership/mode. Preserve existing early-signal and synchronous-completion handling throughout.
6. Enter Serving and start retention maintenance. Emit ready only after the actual listener and services are ready.

A termination/failure request immediately closes management/dispatch admission, stops retention, and latches telemetry stop time without reentrant SQL. After the current stack/transaction unwinds, healthy shutdown settles timing, destroys dependents, closes database and releases endpoint resources. Fatal storage shutdown performs no further persistence. Borrowed telemetry/database collaborators must outlive their borrowers; all timers and queued signal deliveries must be safe during teardown.

Check every early exit with the signal relay and partial construction. Do not make configuration or service support weaken Phase 7's immediate shutdown, uncertain-commit, no-reentrant-destruction, or restart recovery contracts.

## 10. Installed paths and jobuctl

### 10.1 One source of path defaults

Use GNUInstallDirs for binaries, libraries, headers, package metadata and documentation. Define explicit absolute cache variables for operational paths because these must be shared with clients/service templates and should not change implicitly with GNU special-prefix rules:

| Cache variable | Default for a configured prefix | Result |
| --- | --- | --- |
| `JB_JOBU_SYSCONFDIR` | `${CMAKE_INSTALL_PREFIX}/etc` | default config `jobud.ini` |
| `JB_JOBU_STATEDIR` | `${CMAKE_INSTALL_PREFIX}/var/lib` | default database `jobu.sqlite3` |
| `JB_JOBU_RUNDIR` | `${CMAKE_INSTALL_PREFIX}/var/run` | default socket `jobud.sock` |

Generate a private `jobu_paths_priv.hpp` consumed by both applications through a small common build interface. Generate service/config templates from the same values. Do not place daemon installation policy in `jb::core` or the generic ControlClient constructor.

A Linux system-service profile explicitly uses prefix `/usr`, config `/etc/jobu`, state `/var/lib/jobu`, runtime `/run/jobu`. The flat generic defaults avoid repeating `jobu` under a dedicated prefix such as `/opt/jobu` or `$HOME/jobu`. State and runtime directories still require the protected ownership and modes in §9.2; a shared prefix needs explicit operational-path overrides or administrator-provisioned private directories.

Native socket length is checked at configuration/startup; reject unusable paths rather than truncate them. Escape generated C++ strings, INI paths and service syntax correctly; reject newline/NUL and service-profile paths that cannot be represented safely.

`DESTDIR` stages files and is never compiled into runtime defaults. `cmake --install --prefix` changes installation destinations, not previously compiled absolute operational defaults. Cache variables initialized from the prefix retain their values when an existing build directory is reconfigured with a different prefix; use a fresh build directory or explicitly reset all three operational paths. State this clearly in installation help and packaging checks.

### 10.2 CLI behavior

`jobuctl` now uses the compiled socket path when `--socket` is absent. An explicit nonempty `--socket PATH` wins; keep all existing parsing/help rules. `jobud` uses the same default unless its config or explicit flag overrides it.

The client does not read protected daemon configuration, search alternate socket paths, probe other installations or use an implicit environment-variable fallback. If an administrator changes daemon socket configuration, use `jobuctl --socket` for that deployment. Error output identifies the attempted endpoint safely and suggests the override without leaking configuration.

Keep command/subcommand help and local validation ahead of connection attempts. Help shows the actual compiled default; all existing command-specific help and documented aliases remain available. Installed examples show the default form and an explicit override. Generic library clients continue to supply their endpoint explicitly.

## 11. CMake installation and reusable SDK

### 11.1 Build and installation components

Add `include(CTest)` and honor `BUILD_TESTING` around the test subtree so packaging consumers do not need Catch2. Add a documented `JB_BUILD_EXAMPLES` option and disable it in package builds. Preserve ordinary developer defaults where practical. Do not require SQLite OFF testing to prove installation correctness.

Provide standard `install` rules with components:

- **Runtime:** `jobud`, `jobuctl` in `${CMAKE_INSTALL_BINDIR}` (`${CMAKE_INSTALL_PREFIX}/bin` with its default `bin` value), LICENSE/notices, example config, and service templates. No live database, lock, socket, secret, account, enable/start action or writable runtime directory belongs in the package.
- **Development:** static libraries, the audited header closure, imported CMake targets/config/version files, and buildable SDK examples.
- **Documentation:** protocol/client/CLI/operations guides and configuration reference under the configured documentation directory. Planning/handoff records need not be installed as user manuals.

Install configuration as `jobud.ini.example`, not by overwriting an operator's active `jobud.ini`. Put complete service examples under `${CMAKE_INSTALL_DATADIR}/jobu/services`; an explicit system packaging profile may also stage the unit to the vendor unit directory. Activation and initial config copying remain administrator actions.

Stage 9.22 must generate and install an **exhaustive, commented INI sample** from `packaging/jobud.ini.in`. Include every accepted fixed configuration key and a concrete `defaults.<name>` entry for every registered attribute accepting `DaemonDefault` scope. Full-line comments explain each directive's purpose, default or omission behavior, value type, units, accepted values/ranges, and important cross-field or security constraints. Generate operational paths from the shared CMake values. Keep deployment-specific optional examples commented out where enabling them would require an account, credential or existing external file; unsafe root permissions remain false. Document built-in attribute defaults and valid JSON spellings, including the INI single-quote wrapper for JSON strings; commented attribute examples may preserve the omitted daemon-default layer. The sample must be usable without enabling optional features merely to demonstrate their directives.

### 11.2 Exported components and dependencies

Use package name `JobU` and namespaced imported targets matching existing target names, e.g. `JobU::core`, `JobU::rpc`, `JobU::jobu-client`. Components describe dependency discovery, not a second implementation of the libraries:

| Component | Principal targets | External dependency discovery |
| --- | --- | --- |
| `Core` | core | fmt |
| `Net` | net | Core |
| `Db` | db | Core |
| `Rpc` | rpc | Core |
| `Jobu` | jobu | Core, Net, Db, Rpc |
| `Client` | jobu-client | Jobu, Rpc |
| `SQLite` | db-sqlite, jobu-sqlite | Db, Jobu, SQLite3 |
| `HTTP` | net-http, jobu-http | Net, Jobu, CURL |
| `CLI` | jobu-cli | Jobu, Core |

Inspect actual final link interfaces and include any additional real requirements. Use component-aware `find_dependency` and load dependencies before their export sets. `find_package(JobU CONFIG REQUIRED COMPONENTS Core)` must not require SQLite, libcurl, Catch2 or source-tree JSON headers. Default unspecified components to Core, document it, and use `check_required_components`. A requested backend not built/installed is a clear package-not-found error, not a dangling imported target.

Public compiled JsonValue hides nlohmann types. Keep a compile-only JSON implementation target out of the installed link interface when it is not a link dependency; do not force SDK consumers to locate that target through a stale build-tree reference. Conversely, static-library consumers must receive real link requirements such as fmt/CURL/SQLite. Do not strip needed dependencies merely to make export configuration pass.

### 11.3 Header and ABI boundary

Replace PUBLIC absolute source-file listings with PRIVATE build sources or correctly scoped build-interface expressions. Include directories use separate BUILD_INTERFACE and INSTALL_INTERFACE roots. No installed imported target may contain the source/build checkout path.

Install headers in an explicit `include/jb/<module>/...` layout, preserving relative includes. Include the internal support headers required by public inline/template code, such as Object/Signal support. Audit this transitively; do not install only files labeled public and then require the source tree to compile, and do not install every private repository/runtime header indiscriminately. Document installed implementation-support headers as non-API.

Ensure both namespaced includes and existing transitive includes compile with exported target usage requirements. Examples should prefer `<jb/core/json.hpp>` and `<jb/jobu/client/control_client.hpp>` or the verified final equivalent layout. Build static libraries with position-independent code so Core can be linked into another shared library, including a future PHP extension. Phase 9 does not create PHP bindings or promise stable binary ABI across unreleased versions.

Install CMake version metadata reflecting the project version; RPC API version is a separate number. Do not silently declare final v1 release version/signing merely because Phase 9 gates pass.

### 11.4 Mandatory external consumer checks

Install to a fresh staging prefix and configure small consumers outside the repository:

1. Core-only executable using JsonValue plus Object/Signal public headers; no SQLite/CURL/Catch2 discovery.
2. Shared library linked against Core, proving PIC and public transitive headers.
3. Typed ControlClient executable linked only through `JobU::jobu-client` and its exported dependencies.
4. Optional installed SQLite/HTTP/CLI component smoke builds for components included in the full package.

Disable CMake user/system package registries in these checks. Prevent source/build include paths and build-tree exports from satisfying missing files. Inspect generated target properties for checkout paths and verify a copied/moved SDK prefix can still be found; compiled application operational defaults retain the separately documented absolute-path behavior. Do not confuse SDK relocation with daemon data-directory relocation.

## 12. Packages and managed services

### 12.1 Artifact contract

Use CPack TGZ component packaging for the first supported distributable artifacts: runtime, development, and documentation, with a documented way to produce the complete set. Package names include project version, OS and architecture; build metadata records compiler/runtime, external library dependencies, source commit and build options. Generate manifests/checksums locally. Signing, uploading and release creation are separate user-authorized actions.

Release packaging uses a fresh, clean checkout of the selected revision and a fresh build directory. Keep source contents and revision unchanged from CMake configuration through compilation and packaging; after a change, start again with a fresh checkout and build directory. Source revision/state are a configuration-time snapshot, with no build-time refresh or later source-change detection. Package producers use a single-config generator with an explicit `CMAKE_BUILD_TYPE` (Release for the documented workflow). Reject multi-config producers and an empty build type at packaging preflight; these restrictions do not disable ordinary developer multi-config builds.

Include LICENSE and relevant third-party notices; do not claim bundled dependencies that are merely dynamically linked on the build host. State minimum tested OS/runtime and how fmt/libcurl/SQLite runtime requirements are satisfied. Check dynamic dependencies on the native host. An archive is not automatically portable across Linux distributions or macOS deployment targets.

No package contains a development database, secret values, developer config, `.bld*` trees, handoff evidence, test logs, source checkout paths or temporary certificates/private keys. Use explicit install manifests rather than broad repository globs. Extraction and CMake installation do not create users, change existing data, enable services or start a daemon.

Keep active config untouched across repeated installs. Install/update examples and service assets reproducibly; document how an administrator compares their active configuration with a new example. There is no database upgrade script. A binary that rejects an old format must fail before service readiness and report the supported/current marker without deleting data.

### 12.2 Linux systemd profile

Generate `jobud.service` for the explicit Linux system layout. The reviewed baseline uses:

```ini
[Unit]
Description=JobU scheduler
After=network.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=exec
User=jobu
Group=jobu
UMask=0077
RuntimeDirectory=jobu
RuntimeDirectoryMode=0700
StateDirectory=jobu
StateDirectoryMode=0700
ExecStart=@JOBU_BINDIR@/jobud --config @JB_JOBU_SYSCONFDIR@/jobud.ini
Restart=on-failure
RestartSec=5
KillSignal=SIGTERM
KillMode=mixed
TimeoutStopSec=15
SendSIGKILL=yes
NoNewPrivileges=yes
LimitCORE=0
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

Validate substitutions and unit syntax; paths with spaces or `%` require systemd-aware quoting/escaping, not shell escaping. The system profile must enforce the `/run/jobu` and `/var/lib/jobu` relationship with RuntimeDirectory/StateDirectory. Do not emit a superficially valid unit whose managed directory differs from the compiled/configured path. A custom-prefix deployment can use explicitly provisioned protected directories and a separately generated matching unit without those directory directives.

Systemd starts the already unprivileged account; the daemon's own root-drop path is an independent foreground capability. Provision the dedicated `jobu` account/group explicitly before activation, without a login shell or fixed numeric UID/GID assumption. Account creation commands belong in administrator documentation, not `cmake --install` side effects.

`Type=exec` confirms executable startup, not RPC readiness. A deployment check must query `system.info` with a bounded wait. No sd_notify dependency or socket-activation protocol is added. `KillMode=mixed` lets the daemon receive its normal stop signal first and leaves the service manager a final process-group/cgroup cleanup boundary. Validate this alongside the existing Process group cleanup; do not make promises for deliberately escaped sessions.

Do not add an unconditional ExecStartPre unlink, PID file, double fork or a shell wrapper. Avoid blanket filesystem/home/network restrictions that silently break the product's arbitrary configured CLI jobs or HTTP calls. Administrators may add narrower hardening appropriate to their job set; document the tradeoff. `NoNewPrivileges` and owner-only state remain required defaults.

Service activation is an explicit operator sequence: install package, create account/parents, copy and secure initial config, run config check under the intended identity, install/verify the reviewed unit, daemon-reload, enable/start if desired, then check RPC readiness. Reinstalling binaries must not automatically start or restart the daemon.

### 12.3 Linux managed-service evidence

Use a disposable VM/container with systemd and deliberately provisioned test identities. Record the environment and commands. Never repurpose an arbitrary host account or recursively alter existing `/var/lib` data for a test.

Required cases include a private prefix foreground smoke test, system-profile install, owner-only socket access, explicitly configured group access, unauthorized-user rejection by filesystem permissions, root startup rejection, successful permanent root-to-user drop, independent root CLI guard, normal stop, process crash/restart, stale socket recovery, and two daemons competing for the same endpoint with different databases.

Inspect ownership/modes of database/WAL/SHM/lock/socket, actual process/child IDs, core-dump limit and service process cleanup. Verify no CLI/HTTP execution starts before identity verification and no socket becomes visible/listening with an intermediate permissive mode. Test a stale symlink/non-socket/live-listener collision without destroying the other resource.

Generic unit tests with injected syscall results remain useful for failure branches, but they do not replace this native privileged evidence. If the environment cannot run it, mark the gate pending; do not describe root/security or managed-service deployment as verified.

### 12.4 macOS launchd profile

Implement after Linux acceptance. Reuse portable policy and isolate native account/path/identity details. Generate `org.jobu.jobud.plist` using an XML/plist-safe generator or escaping helper, with an absolute executable and separate `ProgramArguments` entries for `--config` and its absolute path.

Use a dedicated non-root `jobu` user/group, `RunAtLoad`, `KeepAlive` with `SuccessfulExit=false`, `ThrottleInterval=5`, `ExitTimeOut=15`, and umask 0077 represented correctly for plist integer semantics. Verify supported key spelling/behavior against native `launchd.plist(5)` during the macOS stage. Do not set `AbandonProcessGroup=true`; do not daemonize or use a PID file. Confirm shutdown/process-group behavior on the supported macOS release rather than assuming systemd equivalence.

The account, protected state/runtime directories, active config and `/Library/LaunchDaemons` placement are explicit administrator setup. A plist installed there is root-owned and not group/world-writable. No fixed free UID/GID is assumed. Package the reviewed template under the installation data directory; bootstrap/enable/start are documented operator actions, never CMake side effects.

Route stderr to an explicitly provisioned owner-only service log, by default `jobud.log` in the protected state directory, or to an administrator-supplied native logging arrangement. Plist redirection has no automatic rotation guarantee. Document a bounded retention/rotation procedure, including a safe stop/rotate/restart option; do not claim journald semantics or add a daemon file-rotation subsystem. The runtime itself continues to log only to stderr.

Run native plist validation, installation/SDK consumer tests, service bootstrap/readiness/stop/restart, permissions, account handling and process cleanup. Linux cross-compilation or XML parsing does not count as native launchd evidence. The optional native macOS verification stage may be explicitly skipped, but then the release record says **macOS service deployment unverified** rather than claiming full two-platform deployment acceptance.

## 13. Documentation and public API audit

All protocol and operational documents live under `docs/`. Update existing documents in place where they own a contract; link rather than duplicate protocol schemas across guides.

| Document | Required content |
| --- | --- |
| `docs/configuration.md` | Exact keys/defaults/units, precedence, text grammar, omitted timezone, boolean strictness, immutable materialized defaults, check mode, examples. |
| `docs/installation.md` | Build prerequisites, install components, prefix/cache/DESTDIR semantics, external dependencies, SDK consumption, accounts, systemd/launchd activation and verification. |
| `docs/operations.md` | Lifecycle, identity/filesystem trust, stale endpoint handling, retention/idempotency boundary, offline backup/restore, old-format rejection, recovery and troubleshooting. |
| `docs/observability.md` | Structured event fields, safe values, logs, measurement units/provenance, complete/partial/unmeasured coverage, crash limitations, warning guarantee and retained statistics. |
| Existing JSON-RPC reference | API 1.4, optional input timezone, statistics coverage/provenance, idempotency lifetime, unchanged method list and current `run.list` behavior. |
| Existing CLI guide/help | Default socket, explicit override, all local help/check paths, useful startup diagnostics and changed schedule examples. |
| Existing C++ client/SDK guide | Installed targets/components, endpoint supplied by caller, input/response timezone distinction, event/lifetime semantics and example build. |

Verification records are kept separately in user sepcified shared folder.

| Phase 9 verification record | Per-stage commits, commands/results, native environment, skips, fault tests, query plans, scale measurements, package manifests and known limitations. |

Backup/restore guidance must respect SQLite WAL and exclusive ownership. Prefer a stopped daemon and a SQLite-consistent backup operation, or a carefully documented coherent offline file set. Never recommend copying only a live main database file while ignoring WAL. Restore while stopped into a protected location after preserving the operator's existing data; verify format and identity before restart. Do not present removing a lock pathname as a way to bypass another running owner.

Explain that retention is irreversible history deletion and does not necessarily shrink the database file immediately. No automatic VACUUM, migration, repair, live backup RPC or secret export is introduced. An old-format development database can be recreated only by explicit operator choice.

Add public Doxygen for all new/changed declarations, including units, ranges, ownership, thread affinity, signals, shutdown behavior, observational limitations, and default-timezone input semantics. Add internal comments at transaction/commit-to-signal boundaries, epoch repair, privilege ordering and endpoint ownership checks. Comments should explain the reason for a rule, not paraphrase every statement.

Audit each Object-derived type for one ObjectPrivate allocation and receiver-aware signals. Audit installed public-header closure independently from what Doxygen labels public. Keep #230 listed as deliberately deferred, with unchanged current behavior; do not turn this phase's document sweep into its resolution.

## 14. Staged implementation plan

Stages 9.1–9.28 deliver the portable/Linux implementation. Stages 9.29–9.30 add macOS-specific deployment work; 9.31 is optional native macOS verification. Stage 9.32 is the final clean Linux and closure gate. These are review boundaries, not permission to land all work in one change. Temporary incompleteness between them is acceptable.

Every stage updates a single `jobu-phase9-verification.md` record with scope, commit, commands/results, and any explicit deferral in user specified shared folder. Tests below are focused requirements, not instructions to rerun the entire matrix after every edit. A stage with new public APIs also includes its Doxygen/include-boundary check in that stage.

### Stage 9.1 — In-memory INI parsing primitive

**Implement:** `IniFile::from_text`, shared lexical parsing and value-free daemon-usable diagnostics. Keep file includes and existing accessors unchanged. Text parsing has no filesystem access and rejects include directives.

**Verify:** Existing INI tests plus text/file lexical equivalence, comments, duplicate retention at the generic level, malformed input, include rejection, and diagnostics that do not expose values. Compile the public header independently.

**Exit:** Core supplies a small reusable primitive; no daemon security policy is embedded in it.

### Stage 9.2 — Typed daemon configuration

**Depends:** 9.1. **Implement:** Private configuration DTOs, complete key table, registry-backed defaults, strict converters, duplicate/unknown-key checks, cross-limit validation and safe diagnostics. Use test-provided text; defer privileged file opening.

**Verify:** Boundary/overflow/boolean/duplicate/NUL/UTF-8 cases, flat grammar, invalid timezone, incompatible attribute defaults, RPC transport/output budgets and no secret-bearing errors.

**Exit:** A valid immutable configuration can be produced without Application, database or network activity.

### Stage 9.3 — Startup precedence and local commands

**Depends:** 9.2. **Implement:** Split CLI parse/merge/actions; default-path CMake variables and generated shared header; `--config`, `--no-config`, `--check-config`, daemon help, identity overrides and explicit negative unsafe flags. Use secure-file interface with an unprivileged implementation; root hardening completes later.

**Verify:** Precedence/absence versus false, contradictory flags, missing explicit versus absent default file, help/version before file lookup, check-mode no side effects, configured-prefix/default values and CLI-relative path resolution.

**Exit:** Existing explicit-socket/database invocations remain usable; configuration decisions are reviewable separately from privilege mechanics.

### Stage 9.4 — Configured schedule and attribute defaults

**Depends:** 9.2–9.3. **Implement:** Input-only optional timezone DTOs, ManagementServiceOptions, request/client/CLI codecs, default resolution and attribute materialization. Update schedule.validate/next registration and relevant help. Do not change stored CronSchedule semantics.

**Verify:** Omitted versus explicit UTC, invalid/empty zone, create/update/validation/next equivalence, unchanged existing definitions after restart, request-file preservation, and replay of an omitted-timezone request after the daemon default changes.

**Exit:** Configuration actually controls new schedules/defaults without mutating existing work or breaking exact replay. Final API version publication is consolidated in 9.14.

### Stage 9.5 — Fresh format 4 and timing rows

**Implement:** Current manifest/table/index additions, timing insertion at all run creation sites, row-presence validation, fixture updates and format rejection. No migration code.

**Verify:** Fresh creation/rollback, current reopen, malformed timing/foreign-key constraints, all once/cron/manual/recovery insertions, cascaded deletion, and formats 1–3/newer rejected without mutation.

**Exit:** Storage can express telemetry without changing payload snapshots or current run.list behavior.

### Stage 9.6 — Per-queue retention selection

**Depends:** 9.5. **Implement:** Sweep cursor/batch API and queue-specific terminal selection/deletion, inherited/finite/unlimited policy and fair queue visitation. Keep owner/idempotency retirement disabled until 9.7 makes the whole deletion transaction safe.

**Verify:** Completion-time strict cutoff, historical queue after moves, deleted queues, nonterminal preservation, zero/unlimited and overflow, pagination/fairness, transaction rollback and no output deserialization. Inspect the per-queue query plan.

**Exit:** Repository-only behavior is proved; do not enable a production timer before safe replay/owner handling.

### Stage 9.7 — Idempotency retirement and deleted owners

**Depends:** 9.6. **Implement:** Resource-lifetime matrix, matching scope/method/resource checks, run-key retirement, eventual deleted-job/queue cleanup and guard against scope references. Remove obsolete production expiry assumptions.

**Verify:** Live recurring creation replay, future once replay, manual replay until run purge, terminal-definition retention, expired once creation record, unrelated-key preservation, unknown relationship failure, and eventual deleted queue removal with moved/cron/manual history. Fault each multi-delete transaction and prove atomicity.

**Exit:** No dangling retained replay record and no circular retention pin. No implicit terminal job deletion or Run Now reopening.

### Stage 9.8 — Retention service

**Depends:** 9.7. **Implement:** Object-private service, timer/batch progression, post-commit owning signals, stop/lifetime handling and runtime failure connection. Start only after Serving when runtime composition enables it.

**Verify:** Injected time, yield between batches, no transaction across callbacks, stop/destruction/reentrant signal stop, unlimited-default finite overrides, bounded parent counts and fatal rollback/commit behavior.

**Exit:** Maintenance can run without adding a database writer thread or blocking admission during startup.

### Stage 9.9 — Telemetry accounting primitives

**Depends:** 9.5. **Implement:** WaitRepository, epoch/sample/counter arithmetic, timing quality transitions and ExecutionTelemetry single-Private owner with transaction adapter. Separate observed-from-birth complete runs from later-instrumented partial runs.

**Verify:** Add/settle/rebase/close arithmetic, monotonic regression/overflow, unknown epoch rejection, rollback, complete/partial/unmeasured transitions, no nested transaction, no signals while SQL is active, and no Object secondary private allocation.

**Exit:** Deterministic clock tests prove arithmetic and ownership before scheduler integration.

### Stage 9.10 — Management eligibility boundaries

**Depends:** 9.4, 9.9. **Implement:** Shared eligibility projection/predicate and same-transaction timing hooks for queue/job changes, move, create, Run Now and pending cancellation, including manual sibling barriers.

**Verify:** Suspension exclusion; accepted manual bypass; queue suspension enforcement; old/new queue policy; pending payload/schedule changes; failed/no-op/replayed mutations; same-sample settle/reopen; and embedded service with no telemetry.

**Exit:** Durable user mutations cannot leave stale eligible intervals or lose time on rollback.

### Stage 9.11 — Dispatch, completion and retries

**Depends:** 9.9–9.10. **Implement:** Transaction-local settle/close in Running claim, completion/retry/barrier release and terminalization. Preserve existing completion callback protocol, generation keys and dispatch fairness.

**Verify:** Full global/queue capacity, due blocking retry, preparation/start failure, sync completion, completion rejection, fixed/exponential retry backoff, cancellation and uncertainty. Assert execution time never contributes to waiting and totals survive retries.

**Exit:** Attempt/run outcomes and timing persist atomically with existing lifecycle semantics.

### Stage 9.12 — Backlog observation and delay warnings

**Depends:** 9.10–9.11. **Implement:** All-eligible keyset observation before capacity short-circuit, separate monotonic warning deadline, durable warning claim and owning delayed signal. No per-run timers.

**Verify:** Waiting rows beyond the first page and full queues/runners all count; threshold equality/strict crossing; zero-disable; threshold edits; pause/backoff/barrier exclusion; UTC jumps; retry/move/restart deduplication; dispatch at warning deadline; and receiver-triggered shutdown after commit.

**Exit:** Capacity-starved work cannot disappear from observation, and warning scheduling cannot spin at equality.

### Stage 9.13 — Checkpoints, recovery quality and stop

**Depends:** 9.12. **Implement:** Bounded checkpoint sweeps, old-epoch repair in recovery, captured stop boundary and healthy finish_stop after unwind; fatal stop makes no writes. Connect lifetime order to runtime.

**Verify:** Crash between checkpoints, crash during repair, preserved partial lower bound, no downtime addition, clean restart, uninterrupted closed/Running timing, created-during-recovery complete successor, stop during mutation, fatal poisoned DB, and interrupted healthy cleanup.

**Exit:** Complete measurements have a defensible meaning; crash loss is visible as partial coverage rather than hidden as zero.

### Stage 9.14 — Statistics and API 1.4

**Depends:** 9.4, 9.13. **Implement:** One-run timing joins, complete-terminal duration aggregation, mutually exclusive coverage, provenance, client decoding/CLI rendering, API version and method introduction documentation.

**Verify:** Mixed states/qualities, zero samples versus zero wait, no attempt multiplication, per-dimension sums, current planned-cohort filtering, cursors/retention between pages, response budget, read-only statistics, and typed/raw/CLI equivalent results.

**Exit:** Same 30 methods; accurately documented measured fields and optional request timezone. No compatibility framework or #230 behavior change.

### Stage 9.15 — Structured core log fields

**Implement:** Small LogField/log_event addition, span lifetime contract and legacy logging compatibility. Avoid a general formatter/serializer/plugin framework.

**Verify:** Typed fields, borrowed lifetime/copying expectations, disabled-level behavior, public include closure and existing logger/thread tests. No race introduced when installing a logger or changing level.

**Exit:** Reusable core facility supports daemon events without knowing JobU types or leaking nlohmann types.

### Stage 9.16 — Daemon operational logger

**Depends:** 9.2, 9.8, 9.12, 9.15. **Implement:** JSON/text stderr logger, lifecycle/recovery/retention/delay/failure events and safe field selection. Honor configured format/level.

**Verify:** Parse every JSON line, escaping/UTF-8/nonfinite handling, concurrent writes not interleaved, write failure without recursion, legacy message records, and redaction assertions using sentinel secrets in CLI/HTTP/config failures.

**Exit:** Events contain useful IDs/counts/provenance with no command, environment, URL credentials, payload or secret values.

### Stage 9.17 — Linux identity finalization

**Depends:** 9.3. **Implement:** Account/group resolution, root/unsafe policy, permanent verified Linux drop and main ordering before worker-capable objects. Preserve the CLI root guard and Process no-new-privileges behavior.

**Verify:** Focused injected syscall failure tests plus isolated native root helper for real/effective/saved IDs, supplementary groups, capabilities, inability to regain root and independent daemon/CLI overrides. Non-root same/different identity cases run normally.

Update existing daemon integration fixtures that intentionally run in a root test container to supply their explicit unsafe test override, or run them under the fixture's unprivileged identity. Do not weaken production defaults to keep such tests passing.

**Exit:** No database, HTTP worker, RPC listener or child work before verified final identity. Mark native privileged evidence pending if unavailable.

### Stage 9.18 — Protected config and state paths

**Depends:** 9.17. **Implement:** Descriptor-owned config read, trusted-parent checks, safe leaf creation, umask, final-user state files and checked CLI-relative-to-original-CWD path normalization. No recursive repair.

**Verify:** Symlink/file-type/mode/owner substitutions, oversized config, sticky temporary parent with private leaf, missing parent, existing insecure tree, safe system aliases, restrictive SQLite sidecars, and complete descriptor cleanup on failures.

**Exit:** Configuration and database setup obey the filesystem trust contract under the final identity.

### Stage 9.19 — Socket group and endpoint restart safety

**Depends:** 9.18. **Implement:** LocalServer pre-listen group option, endpoint ownership lock, bounded stale probe and checked inode cleanup. Keep generic listen's existing-path refusal.

**Verify:** Clean close, crash stale socket, live/refused/ambiguous connect results, non-socket/symlink collision, same-endpoint different-DB competition, lock lifetime, group membership/mode, no permissive listen window and descriptor leaks.

**Exit:** A managed daemon can restart safely without deleting another daemon's endpoint.

### Stage 9.20 — Full configured runtime composition

**Depends:** 9.8, 9.13–9.14, 9.16–9.19. **Implement:** Wire immutable options, defaults, RPC frame/read-buffer limits, logger, telemetry, retention, identity and endpoint guards into the final startup/stop state machine.

**Verify:** Foreground end-to-end config startup, mixed CLI/HTTP work, default attributes/timezone, maintenance/warning events, early signals in every startup phase, synchronous callback shutdown, failure gating, poisoned DB and teardown ownership.

**Exit:** Actual jobud behavior matches the config table and Phase 7 failure guarantees; no orphan timers or second private blocks.

### Stage 9.21 — Shared CLI default endpoint

**Depends:** 9.3, 9.20. **Implement:** Optional jobuctl `--socket` with generated fallback, current help/guide updates and connection diagnostics. No daemon-config or environment discovery in the client.

**Verify:** Configured-prefix default connection, explicit override, empty/duplicate flags, missing default endpoint, help without any socket/config, all command/subcommand help, and unchanged raw/typed request behavior.

**Exit:** Installed daemon/client defaults agree; explicit existing invocations continue to work.

### Stage 9.22 — Install targets and header closure

**Depends:** 9.20–9.21. **Implement:** GNUInstallDirs, install components, BUILD_TESTING/examples switches, explicit headers including required inline support, PIC and build/install include separation. Generate and install the exhaustive, commented `jobud.ini.example` specified in §11.1. Stage examples/config/service assets without activation.

**Verify:** Fresh `cmake --install`, DESTDIR file manifest, no user/config/data modification, repeated install preserving active config, header include audit, no installed source-tree paths and package build with tests/examples off. Check sample completeness against the accepted fixed keys and daemon-default attribute registry; validate the generated sample with `jobud --check-config` in a protected fixture and validate documented `defaults.*` examples through the existing parser/registry.

**Exit:** Standard install target works and delivers the exhaustive, commented configuration sample before package generators or service activation are involved.

### Stage 9.23 — Exported SDK and external consumers

**Depends:** 9.22. **Implement:** Component-aware JobU package, dependency discovery and namespaced targets. Adapt installed examples to the real header layout.

**Verify:** All external consumers in §11.4, Core without backend/test discovery, shared-library PIC, missing optional component error, clean/copy-relocated prefix and disabled package registries.

**Exit:** Reuse is proved from installed files, including Core's future shared-library/extension use, with no repository include workaround.

### Stage 9.24 — CPack artifacts

**Depends:** 9.22–9.23. **Implement:** Runtime/development/documentation TGZ generators, manifests, metadata and notices. No release publication or automatic activation.

**Verify:** Extract into fresh staging directories, inspect contents/dependencies, install/run artifact smoke tests, repeated package generation, no development data/secrets/test artifacts, and actual project/RPC version distinction. Verify explicit Release metadata from the unchanged-checkout workflow and reject multi-config or unspecified-configuration producers before packaging.

**Exit:** Reviewable artifacts have truthful platform/dependency requirements and usable contents.

### Stage 9.25 — Linux systemd assets

**Depends:** 9.24. **Implement:** Standard/custom-prefix service generation, account/setup instructions, config template, operational probes and staged unit path. No installer account/service side effects.

**Verify:** `systemd-analyze verify` where available, substitutions/escaping, profile/path agreement, explicit config requirement, non-root identity, restart/stop directives and absence of unconditional stale-path unlink.

**Exit:** Reviewed unit can be activated deliberately; syntax checks are not yet described as a managed-service runtime pass.

### Stage 9.26 — Native Linux deployment/security verification

**Depends:** 9.25. **Implement only fixes found by:** the disposable privileged/systemd test gate in §12.3. Record both successful and rejected cases, process IDs/ownership, readiness, restart and teardown evidence.

**Verify:** Dedicated account and group modes, root-drop/guard matrix, two endpoint owners, SIGTERM/SIGKILL service behavior, persisted history/recovery and no child work before security checks. Test package binaries, not only build-tree executables.

**Exit:** Linux installation and service operation have real native evidence. Missing privileges/systemd is a stated incomplete gate, not a passing skip.

### Stage 9.27 — Linux scale and fault verification

**Depends:** 9.20, 9.26. **Implement:** Deterministic and native integration fixtures exercising large waiting/history sets, batching, event-loop responsiveness, warnings and retention/statistics interactions. Fix concrete regressions only.

**Verify:** At least 100 queues/10,000 pending runs and 100,000 retained terminal runs in a documented environment; warnings reach backlog tails; finite/unlimited queues remain fair; bounded projection memory; query plans; measured cycle/checkpoint/purge latency; large cascaded output deletion; commit/rollback faults; crash during checkpoint/purge and restart.

**Exit:** Report observed costs and limitations rather than inventing a universal millisecond SLA. Address pathological repeated scans or starvation before closure; a large synchronous affected-set scan must not be described as constant work.

### Stage 9.28 — Linux documentation/API closure

**Depends:** 9.27. **Implement:** Complete documents in §13, generated/help agreement, public/internal comment audit, ObjectPrivate/signal review and verification traceability. Preserve #230 as deferred.

**Verify:** Executable examples against installed binaries, config reference against key registry, protocol samples against codecs, backup/restore instructions on disposable data, SDK examples and links. No old mandatory `--socket` or migration claims remain in current user guides.

**Exit:** Linux behavior and documentation agree before macOS-specific work begins.

### Stage 9.29 — macOS identity and filesystem adaptation

**Depends:** 9.28. **Implement:** Native permanent-drop verification, account/group/path differences, socket ownership checks and platform conditionals. Keep shared policy and one Private state layout; do not simulate Linux capability/no-new-privileges APIs.

**Verify:** Compile and focused native tests when available; otherwise document the unverified branch. Include safe `/var` alias handling, protected temporary paths, socket group and all failure cleanup.

**Exit:** macOS-specific source is isolated and testable, with limitations stated. No change to accepted Linux policy.

### Stage 9.30 — launchd/package documentation

**Depends:** 9.29. **Implement:** Generated plist, safe substitutions, account/directory/config/log setup, explicit bootstrap/bootout steps, package dependencies and administrator log retention procedure.

**Verify:** Native plist/key checks where available; XML correctness, absolute ProgramArguments, identity/path agreement, no daemonization/PID file, no automatic service activation and no promise of automatic launchd log rotation.

**Exit:** Reviewed macOS deployment assets and instructions exist; native runtime verification is tracked separately.

### Stage 9.31 — Optional native macOS verification

**Depends:** 9.30. **Verify:** Clean native build/tests, installed SDK consumers, artifacts, privileged identity tests, launchd bootstrap/readiness/normal stop/crash restart, stale socket, logging, filesystem ownership and child cleanup. Record OS/architecture/toolchain and actual commands.

**Exit:** Either a complete native pass, or an explicit user-accepted skip with `macOS service deployment unverified`. Do not infer a pass from Linux or from Phase 8's earlier macOS evidence.

### Stage 9.32 — Final clean Linux and Phase 9 closure

**Depends:** 9.28 and documented disposition of 9.29–9.31. **Verify:** Fresh default SQLite-enabled full build/test run, final installed/package consumers, same-revision deployment evidence, CLI/help/protocol/config audit and retained Phase 7/8 regression gates. Recheck only relevant ownership/lifetime paths under sanitizers where changed.

**Exit:** Produce final handoff with exact commit, commands/results, all staged evidence, package manifests, remaining documented limitations and #230 deferral. No mandatory full SQLite-OFF rerun. A claimed Linux deployment pass requires 9.26 evidence; a claimed two-platform v1 deployment pass additionally requires native macOS evidence. Do not publish artifacts or begin additional product work automatically.

## 15. Verification commands and closure checklist

### 15.1 Clean build and staged installation

These are implementation-time command patterns, **not results already obtained for Phase 9**. Use fresh directories and record the actual configured paths. Keep the repository's current CTest registration layout unless that layout is deliberately changed alongside the documented commands.

```bash
cmake -S . -B .bld-phase9-final \
  -DCMAKE_BUILD_TYPE=Debug \
  -DJB_BUILD_SQLITE_DRIVER=ON \
  -DBUILD_TESTING=ON
cmake --build .bld-phase9-final --parallel
ctest --test-dir .bld-phase9-final/test --output-on-failure
```

When `include(CTest)` is introduced at the root, verify whether root discovery now includes the existing test subtree. If it does, `ctest --test-dir .bld-phase9-final` is also valid; do not accidentally run an empty suite and report success. Record test count and intentional skips.

For a system-layout package, configure the real deployment paths but stage the install under an isolated directory:

```bash
cmake -S . -B .bld-phase9-package \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DJB_JOBU_SYSCONFDIR=/etc/jobu \
  -DJB_JOBU_STATEDIR=/var/lib/jobu \
  -DJB_JOBU_RUNDIR=/run/jobu \
  -DBUILD_TESTING=OFF \
  -DJB_BUILD_EXAMPLES=OFF
cmake --build .bld-phase9-package --parallel
DESTDIR="$PWD/.stage-phase9" cmake --install .bld-phase9-package
cpack --config .bld-phase9-package/CPackConfig.cmake -G TGZ
```

Staging does not authorize service activation on the build machine. Use a disposable deployment host for Stage 9.26. For a directly runnable private-prefix smoke test, configure an actual writable short absolute prefix and matching operational cache values rather than executing staged `/usr` binaries and expecting DESTDIR to become their runtime root.

An installed Core consumer should need only a normal package lookup:

```cmake
cmake_minimum_required(VERSION 3.20)
project(core_consumer LANGUAGES CXX)
find_package(JobU CONFIG REQUIRED COMPONENTS Core)
add_executable(core_consumer main.cpp)
target_link_libraries(core_consumer PRIVATE JobU::core)
```

The imported target supplies C++20 and real transitive usage requirements. Configure the consumer with the installed prefix in `CMAKE_PREFIX_PATH` and package registries disabled. Inspect logs to prove it did not discover a build-tree export. The shared-library and typed-client consumers use analogous tiny standalone projects.

### 15.2 Required fault/semantic matrix

| Boundary | Evidence needed |
| --- | --- |
| Retention cutoff | Equality retained, strictly older terminal removed, nonterminal attempts/output retained, inherited/zero/override all correct. |
| Retention ownership | Run's persisted queue governs; moved definitions and deleted queues cannot orphan history or keys. |
| Replay lifetime | Live cron and future once replay; manual run replay until purge; owner cleanup eventually completes without dangling scope/resource records. |
| Transaction faults | Deletion, timing/state change and warning claim roll back together; uncertain commit/fatal rollback closes admission. |
| Clock behavior | Monotonic intervals ignore UTC jumps for accumulated duration; observed due eligibility follows current scheduler UTC policy; no downtime/backoff/suspension charge. |
| Capacity/fairness | Observation includes full queues and global capacity exhaustion, tail pages and blocking retries without changing dispatch order. |
| Warning delivery | Strict threshold, zero-off, at most once durably claimed, safe post-commit emission, no equality spin or duplicate on restart. |
| Measurement quality | Complete from birth, partial on lost open tail or late instrumentation, unmeasured embedded runs, clean restart and complete-only statistics. |
| Configuration | Strict typed grammar, default layering, safe diagnostics, readonly check mode, no implicit timezone override of explicit UTC, replay across default changes. |
| Identity/filesystem | Native permanent drop, separate root switches, safe file ownership/modes, no symlink replacement, no worker/external effect before identity finalized. |
| Socket ownership | Only proven stale owned sockets removed, live/ambiguous endpoints refused, endpoint lock survives pathname reuse attempts. |
| Runtime stop | Maintenance/timing stop without reentrant SQL, healthy final settle, fatal no writes, dependencies outlive borrowers. |
| SDK/package | No checkout path dependency; Core needs no SQLite/CURL/Catch2; PIC shared consumer; explicit config preservation and no install side effects. |
| Managed service | Native readiness/stop/restart/crash evidence, restrictive identity/paths, expected child cleanup, actual installed binaries. |

Use existing fault drivers, fake TimeSource, fake executors, local HTTP fixtures and process helpers wherever they exercise the real boundary. Do not replace production repository logic with a mock that merely repeats the expected result. Native timing/performance tests supplement deterministic arithmetic tests; avoid assertions that depend on arbitrary host scheduling speed.

### 15.3 Closure decision

Phase 9 can close for Linux when its implemented behavior, final clean tests, installed consumers, privileged service evidence and operational documentation all match this contract. The final record must state the separate macOS result or accepted skip. Keep prior v1 guarantees covered by regression evidence: durable Running before external start; bounded runners/no queue threads; no cron catch-up backlog; suspension draining; manual-run barriers; terminal-once immutability; retry/cancellation/recovery; snapshot materialization; output limits; secret isolation; RPC/CLI/client behavior.

Explicit limitations are part of the delivered contract: observations begin at scheduler detection; crashes can lose an open timing tail and produce partial coverage; delayed warnings are at-most-once rather than guaranteed delivery; retained statistics are a live view; batch bounds count parent deletions rather than cascade bytes; synchronous affected-set reconciliation has measured size-dependent cost; packages depend on a documented host runtime; older database formats are rejected; and #230 remains deferred.

No incomplete mandatory gate is silently converted into success. Distinguish a design artifact, a compiled implementation, a verified Linux deployment, and a verified two-platform deployment in handoffs.

## 16. Evidence and technical references

Repository evidence below is pinned to the baseline; implementation should re-read changed files if main advances before Stage 9.1:

- [Original v1 technical plan, particularly §§21 and 24–31](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/docs/planning/jobu-v1-technical-plan.md).
- [Current retention repository](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/src/jobu/retention_repository_priv.cpp), [scheduler core](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/src/jobu/scheduler_core_priv.cpp), and [statistics contracts](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/src/jobu/statistics.hpp).
- [Current schema owner](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/src/jobu/sqlite/sqlite_schema.cpp), [management contracts](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/src/jobu/management.hpp), and [runtime](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/src/jobud/runtime_priv.cpp).
- [INI API](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/src/core/ini_file.hpp), [logging API](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/src/core/logging.hpp), [LocalServer API](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/src/net/local_server.hpp), and [build root](https://github.com/evaikene/deferra/blob/cebb4cb278942eb5eb939d2dc64ab9129e035841/CMakeLists.txt).
- [Deferred issue #230](https://github.com/evaikene/deferra/issues/230); deferral is the user's explicit decision, not a new Phase 9 acceptance defect.

Primary platform references used for the deployment design:

- [CMake GNUInstallDirs](https://cmake.org/cmake/help/latest/module/GNUInstallDirs.html), [Importing and Exporting Guide](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html), and [CPack](https://cmake.org/cmake/help/latest/module/CPack.html). Use features available with the project's CMake 3.20 minimum or explicitly justify a minimum-version change; this plan requires no such change.
- [systemd execution settings, official source](https://github.com/systemd/systemd/blob/main/man/systemd.exec.xml) and [service settings, official source](https://github.com/systemd/systemd/blob/main/man/systemd.service.xml). Native installed-version verification governs supported directives.
- [Apple: Creating Launch Daemons and Agents](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html), supplemented by native `launchd.plist(5)` and `launchctl(1)` during macOS implementation.
- [Linux kernel no_new_privs documentation](https://www.kernel.org/doc/html/latest/userspace-api/no_new_privs.html).

The operational choices and proposed C++/SQL contracts above are project design decisions derived from the current code and roadmap. They are not claims that the cited platform documentation already implements JobU policy.
