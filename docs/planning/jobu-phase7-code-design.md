# JobU Phase 7 — Recovery, fail-closed storage, and immediate shutdown

## 1. Status, baseline, and implementation rules

This is an implementation-ready design, not a report that Phase 7 is implemented.

Repository baseline: [`3bba9c58a7062fd3a10de43a91b07a3b39dfb7da`](https://github.com/evaikene/deferra/commit/3bba9c58a7062fd3a10de43a91b07a3b39dfb7da), the merged Stage 6.21 handoff. Its verified implementation baseline is `fc326b4d63796d363dff14acac26aff6f8416698`; the subsequent baseline changes record verification and planning. Read current `AGENTS.md` before implementation, and reconcile any later implementation changes before starting a stage.

Authoritative scope: `docs/planning/jobu-v1-technical-plan.md`, particularly §§15.2 and 19, and the Phase 7 roadmap entry. Preserve Phase 6 and its closure contracts for CLI execution, process ownership, output capture, and callback lifetime.

Proposed repository location: `docs/planning/jobu-phase7-code-design.md`.

Implement one numbered stage at a time. Each stage must have a reviewable diff, focused verification, and a handoff; stop at its review boundary. Temporary incompleteness between stages is acceptable. Do not demand that an intermediate stage deliver the final phase behavior. A stage must not claim features whose integration is still pending.

Every stage that changes public declarations also updates their Doxygen contracts and relevant public-header checks. Add concise code comments explaining transaction boundaries, state transitions, callback invalidation, and non-obvious platform behavior. Do not narrate straightforward assignments. Keep public errors free of SQL, job payloads, environment values, output, and credentials. Preserve selective `[[nodiscard]]`: checked recovery and runtime outcomes require handling; idempotent shutdown setters do not need artificial error results.

Object subclasses use a derived `ObjectPrivate` through the existing Object allocation and access pattern. Do not introduce a second pimpl or independent private-data pointer. Bind any owner-dependent state only after base construction. Use signals for reusable notifications; retain exact-operation completion handlers and private synchronous policy/test seams where signals do not fit.

## 2. Deliverables and exclusions

Phase 7 delivers:

1. Startup validation and durable recovery before mutation admission or external dispatch.
2. Both queue recovery policies, interrupted output metadata, retry reconstruction, recurrence repair, manual barriers, and drained suspension completion.
3. A shared fatal-storage policy and irreversible scheduler/management shutdown gates.
4. Coordinated immediate daemon shutdown on SIGTERM, SIGINT, scheduler/storage failure, and fatal HTTP infrastructure failure.
5. Deterministic transaction-failure tests and Linux subprocess evidence, followed by native macOS verification and final Linux verification.

The dispatch and completion transactions already exist. Preserve them and demonstrate their failure behavior; do not replace them with a new persistence architecture.

No schema migration is planned. Do not add a durable capacity counter, recovery journal, manual-barrier table, or attempt “abandoned” state. No changes to API version are required by these internal lifecycle additions. Secrets, expanded history/output/statistics RPC, remaining CLI features, and public Run Now/cancel delivery remain Phase 8. No graceful drain interval, retry of failed completion writes in the same daemon, exactly-once guarantee, recursive descendant tracking, or synchronous all-descendants-terminal barrier is introduced.

The accidentally deleted historical macOS evidence is accepted and is not a prerequisite for this phase. New macOS evidence concerns Phase 7 changes only.

## 3. Existing implementation and changes required

| Existing location | Current behavior | Phase 7 action |
|---|---|---|
| `src/jobu/scheduler.cpp` | `start()` rejects existing running state; `stop()` allows active completions to persist | Keep the startup guard and resumable stop; add irreversible `shutdown()` |
| `src/jobu/scheduler_core_priv.cpp` | Completion closure retains the core; atomic completion precedes capacity release | Gate all dispatch/completion entry and invalidate retained callbacks before teardown |
| `src/jobu/scheduler_repository_priv.cpp` | Retry transition exists; terminal setter accepts succeeded/failed/cancelled | Add a constrained interrupted transition usable by recovery |
| `src/jobu/retry_policy_priv.*` | Normal retry decision is for observed failed attempts | Share delay arithmetic, but implement recovery eligibility separately |
| `src/jobu/management.*` | Synchronous methods and `mutation_committed`; no shutdown gate | Add mutation admission latch and fatal-storage notification |
| `src/jobu/attempt_executor_group.*` | Owns runners; destruction suppresses completions | Add explicit idempotent shutdown with the same ownership ordering |
| `src/jobud/main.cpp` | Scheduler/HTTP failure slots log; no coordinated signal shutdown | Compose recovery and a daemon lifecycle controller |
| `src/core/event_loop.cpp` | `quit()` enqueues a task; shutdown drains generic tasks | Add owner-thread allocation-free quit request; keep gates valid through final drains |
| `src/db/sqlite/sqlite_error.cpp` | Already maps corruption to `db.corrupt`, I/O to `db.io` | Consume stable errors; keep SQLite details out of generic JobU |

New generic JobU files: `recovery.hpp`, `recovery.cpp`, `recovery_repository_priv.hpp/.cpp`, and small shared private recovery/recurrence/storage-failure helpers where justified. New daemon-private files: `runtime_priv.hpp/.cpp`, `shutdown_signal_priv.hpp`, and POSIX/platform implementation files chosen consistently with the current build layout. Test helpers remain in test targets.

## 4. Lifecycle model

The daemon progresses through `Starting → Recovering → Serving → Stopping → Stopped`. A fatal failure in any state latches a nonzero exit status. A normal signal requests stopping without turning a successful shutdown into a failure; a later fatal error takes precedence. Stopping is irreversible and idempotent.

Startup order:

1. Parse options; establish the daemon-owned SIGTERM/SIGINT relay before runner infrastructure can create threads.
2. Construct application/time dependencies; acquire database ownership, open the connection, and run existing schema preparation.
3. Run startup validation and recovery. No listener accepts clients and no executor starts work.
4. Construct HTTP infrastructure, executor group, scheduler, management service, and RPC objects; connect failure notifications and admission gates.
5. Register methods. Check any signal already requested during startup.
6. Start the scheduler, which retains its defensive no-running-state check. Only after successful startup admit normal RPC traffic. If listening fails, use immediate shutdown for any work already started.
7. Enter the event loop, unless shutdown has already been requested.

This ordering is deliberately explicit about `Scheduler::start()` performing synchronous dispatch. An implementation may bind an inactive socket earlier, but must not admit requests or advertise readiness before recovery and scheduler startup succeed. Do not run recovery from `Scheduler::start()` implicitly.

At the first terminal request, synchronously latch gates: management mutations rejected, scheduler dispatch and completion persistence disabled, new connection admission disabled. Request event-loop exit without allocating a posted task. Let the current stack unwind. Destructive teardown happens in the daemon owner scope after `Application::exec()` returns, or directly on a startup failure when no callback stack is active.

The event loop may still run already-ready watchers and drain generic tasks. Consequently, loop exit alone is not the safety boundary: every retained scheduler completion and management mutation checks the latched state. No callback may reopen admission, restart the scheduler, or persist a late result.

## 5. Recovery public API

Use a synchronous free function; recovery is not an Object and needs no signals:

```cpp
namespace jb::jobu {
struct RecoveryOptions {
    std::size_t scan_batch_size{256};
};
struct RecoveryReport {
    std::uint64_t interrupted_attempts{};
    std::uint64_t retrying_runs{};
    std::uint64_t terminal_runs{};
    std::uint64_t inserted_successors{};
    std::uint64_t suspended_jobs{};
    std::uint64_t suspended_queues{};
};

[[nodiscard]] auto recover_startup(
    jb::db::Database& database,
    AttributeRegistry const& attributes,
    CronEngine const& cron,
    jb::core::UuidGenerator& uuid_generator,
    jb::core::TimeSource& time_source,
    RecoveryOptions options = {})
    -> jb::core::Result<RecoveryReport, jb::core::Error>;
}
```

Use the existing abstract `UuidGenerator` and mutable `TimeSource` contracts, preserving deterministic test injection; do not require the concrete system generator. Document exclusive daemon database ownership, owner-thread affinity, absence of external execution, successful-report counters, partial progress across committed transactions, and restart safety. Reject zero or unreasonable batch sizes through a documented bounded range, proposed `1..4096`.

A private overload accepts a synchronous `should_stop` predicate used by the daemon and tests. It never escapes the call. Check it between pages and between transactions; also check before a transaction commits. Cancellation rolls back the current unit and returns `jobu.recovery.cancelled`. Earlier committed units remain valid. No arbitrary callback is exposed in the public API merely for daemon signal polling.

Sample one `recovery_time` at invocation start. Use it for interrupted timestamps and retry due calculations. Recurrence uses a fresh sampled time immediately before its transaction, with lower bound `max(recovery_time, fresh_now)`, so a long recovery does not deliberately generate already-missed occurrences. Checked timestamp conversion and delay arithmetic are mandatory. Never rewrite historical started/due/planned timestamps.

A successful return proves there are no persisted running attempts/runs, all required schedule-owned runs exist, manual barriers are unambiguous, and drained suspensions are complete. It does not promise that no new corruption can occur afterward.

## 6. Recovery transactions and invariants

Use generic `jb::db::Database`, `Query`, and RAII `Transaction`. Finish scans before beginning a write transaction if required by existing active-query restrictions. Use keyset pagination with stable UUID/composite keys, owning decoded row values, and bounded page memory. Do not use offset pagination while mutating the scanned predicate. Do not load entire attempt/output history.

Recovery consists of validation, interrupted-run units, missing-successor units, suspension units, and a final invariant validation. Each repair unit commits independently. A crash between units is supported: committed rows no longer match the repair predicate; interrupted/run/output/successor changes within one unit are atomic. Counters advance only after commit and are returned only on overall success.

Validate before modifying and revalidate transaction-local expectations:

- Run/job/queue ownership, UUIDs, typed columns, enum values, timestamps, immutable snapshots, and attributes decode correctly.
- Every Running run has exactly one Running attempt, its latest numbered attempt. Every Running attempt belongs to such a run. Attempt number and timestamp relationships match dispatch contracts.
- Non-running runs have no Running attempt. Terminal runs have no unfinished attempts and carry valid terminal metadata. RetryWait runs have a completed previous attempt and no speculative next attempt.
- The current scheduler does not durably precreate Pending attempts. Treat an unexpected Pending attempt as an invariant failure; do not execute it or silently discard it. Verify this against all baseline writers in Stage 7.1 and document any legitimate legacy shape before changing that rule.
- At most one nonterminal schedule-owned run and at most one nonterminal manual run exist per job; a manual run may coexist with the schedule-owned run and forms its barrier.
- Detect incompatible deleted-owner/nonterminal combinations according to existing lifecycle contracts. Never reactivate deleted definitions or repair malformed rows by guessing intent.

Validation is application-owned and backend-independent. SQLite-specific physical integrity/foreign-key checks, where used, stay under `jobu-sqlite`; do not issue PRAGMAs from `jobu`. Existing schema checks do not substitute for cross-row runtime invariants. Avoid a new unconditional whole-database integrity scan if normal schema checks plus focused row invariants suffice.

Invariant failures return `jobu.recovery.invariant` with a safe reason token and identifiers where appropriate. Overflow or invalid policy data must fail startup rather than wrap, reset retry counts, or fabricate a schedule. Database failures preserve their stable error identity for policy classification, with safe operation context.

## 7. Interrupted attempt recovery

Inside one transaction, re-read the run, its latest running attempt, current queue recovery policy, and current owning definition state. Mark the old attempt Completed with outcome Interrupted and completion time `recovery_time`. Record a small safe JSON result:

```json
{"reason":"daemon_interrupted","outcome_unknown":true}
```

Use `jb::core::JsonValue` and existing storage serialization. This is an internal result document, not a new runner completion wire format. It must not invent an exit code, HTTP status, timeout, or observed external failure.

Insert the corresponding attempt-output row with empty stdout/stderr bytes, false truncation flags, and `capture_lost=true`. The lost capture is an unknown outcome, not a claim that the command emitted nothing. Baseline completion writes output only as part of a terminal completion transaction; an existing output row for a still-running attempt is therefore an invariant failure unless Stage 7.1 identifies a supported writer establishing another shape. Never overwrite unexpected evidence silently.

| Recovery policy and owner condition | Recovered run |
|---|---|
| `fail_interrupted` | Terminal Interrupted, regardless of remaining retry allowance |
| `retry_interrupted`, attempts remain, owner permits retry | Same run becomes RetryWait |
| `retry_interrupted`, exhausted attempts | Terminal Interrupted |
| Retry forbidden by deleted ownership | No retry; terminal interruption where that persisted shape is permitted |

Suspended or suspending ownership delays eligibility; it does not consume retry allowance or erase a scheduled retry. Use the run's immutable effective retry attributes, not newly edited job defaults. Policy selection uses the current queue's recovery policy. A retry is a new attempt of the same run, created later by normal dispatch, with the original run snapshot and next attempt number.

Share existing `retry_delay` and checked due-time arithmetic. Do not route interruption through the normal failure helper by pretending outcome is Failed, and do not make every ordinary Interrupted completion automatically retryable. Recovery retry due time is `recovery_time + retry_delay(policy, run_id, next_attempt_number)` using existing deterministic jitter. Blocking versus Reschedule retains existing scheduler capacity/eligibility semantics. Validate attempt-number and timestamp overflow before writing.

Terminal interruption updates run state, completed timestamp, and result atomically with old attempt/output. A retry leaves run completed/result fields unset and preserves its existing started timestamp. No in-memory capacity is released during startup; runtime reconstructs capacity from committed rows afterward.

## 8. Recurrence, barriers, and suspension repair

Extract the existing recurring-successor builder from `scheduler_core_priv.cpp` into a private helper if needed. Keep normal completion semantics unchanged; make its time lower bound explicit for recovery.

A recovered terminal schedule-owned recurring run creates its successor in the same transaction. Use the latest persisted definition revision, payload, attributes, priority, and schedule, with the first cron occurrence strictly after the recovery lower bound. Skip missed ticks; no catch-up flood. A manual terminal run does not itself create a successor. One-time definitions get no successor.

After interrupted recovery, scan nondeleted recurring definitions and insert a missing nonterminal schedule-owned run. Include suspended/suspending definitions to preserve their pending schedule through resumption; queue suspension affects eligibility, not ownership of the schedule. Re-read the definition and the absence of a successor inside each transaction. Never replace an existing Scheduled or RetryWait run or refresh its immutable snapshot as an incidental repair. Keep the unique partial index as the final guard. A surprising uniqueness failure under exclusive ownership is fatal, not “already done” success.

If the current definition changed from recurring to once under a supported management transition, use the existing transition contract; do not reconstruct an old cron schedule from a historical run. An active Once definition with inconsistent missing work is not repaired by inventing another execution.

Manual barriers are derived from nonterminal manual runs. No barrier rows are written. A recovered manual RetryWait run keeps the barrier; a terminally interrupted manual run releases it. Validate duplicate barriers and reconstruct through existing scheduler queries before serving.

Complete every persisted job/queue Suspending transition whose relevant Running work is gone, including entities with no interrupted rows in this invocation. Preserve existing job revision increments, overflow checks, and queue timestamp behavior by sharing repository operations. RetryWait work does not prevent completion of a suspension. Do not resume anything automatically.

Leave overdue Scheduled/RetryWait timestamps intact. The ordinary scheduler's existing priority, fairness, capacity, suspension, and barrier rules decide dispatch after startup.

## 9. Fatal-storage policy and management admission

Add a private shared classifier with explicit operation context: validation/read, mutation, dispatch, completion, or recovery. Do not infer severity from `ErrorCategory` alone.

| Failure | Required behavior |
|---|---|
| Invalid user input, unknown entity, expected revision conflict | Ordinary operation/RPC error |
| Explicitly translated expected uniqueness conflict | Ordinary conflict; transaction rolls back |
| Storage begin/prepare/execute/fetch/commit failure during a mutation or scheduler state operation | Latch fatal shutdown |
| Unexpected constraints, affected-row mismatch, malformed durable data | Invariant failure; fatal |
| `db.corrupt` in any operation, including reads | Fatal |
| Ordinary non-corruption read failure outside scheduler/recovery | Return ordinary operation/RPC error |
| Any startup validation/recovery failure | No serving or dispatch; nonzero exit, except requested signal cancellation |

`db.busy`, `db.locked`, `db.io`, permission failures, and generic prepare/execute/transaction errors on state-changing paths are not invitations to keep scheduling. The baseline already supplies `db.corrupt`; do not parse SQLite error detail strings. Expected domain conflicts must be recognized at their owning operation before the fatal classifier. An unexpected raw database constraint is not automatically a user conflict.

Add to `ManagementService`:

```cpp
void stop_mutations() noexcept; // irreversible, idempotent, owner thread
// failed: receiver-tracked synchronous signal carrying core::Error const&
```

Follow the project's existing signal declaration syntax. Store gate/failure state in its existing Private. Wrap each public mutation's implementation in a private result boundary so transaction guards unwind before fatal notification. Gate before any mutation work, including direct C++ calls and requests already buffered by RPC. Rejections use `jobu.service.stopping` with Unavailable category. Read methods may finish during ordinary operation; corruption still requests fatal shutdown. During terminal teardown RPC connections are closed.

Latch first failure before emitting `failed`; emit at most once. Do not emit `mutation_committed` for failed or rejected operations. Slots may latch runtime state, but must not destroy/reenter the service while its method is on the stack. Audit all current mutation entry points rather than only create/update operations.

## 10. Scheduler terminal shutdown and callback ownership

Add `Scheduler::shutdown() noexcept` and a terminal Shutdown state distinct from Stopped and Failed. Preserve the first failure when shutdown follows failure; document whether `state()` retains Failed or reports Shutdown, choosing **Failed remains Failed, with a separate terminal gate**. A healthy shutdown reports Shutdown. `start()` after either terminal latch is rejected with `jobu.scheduler.stopping`; `stop()` remains resumable and retains its existing completion behavior.

`shutdown()` only latches state, disables wake/rescan, and invalidates acceptance of retained completions. It must be safe to call from the synchronous failure notification slot. It must not destroy the core or erase structures being traversed by an active callback. Final cleanup follows stack unwinding.

Give retained core completion closures a shared, small lifetime token containing an owner pointer and terminal flag. Shutdown clears/invalidates the pointer before core destruction; callbacks check it before touching the core. Object receiver tracking alone cannot protect an arbitrary `std::function` held by an executor. Do not capture the core raw pointer outside this checked token. No ownership cycle back to executors or Scheduler.

On the first completion/storage failure, invalidate persistence acceptance before notifying observers. Subsequent queued, synchronous-immediate, or destructor-adjacent completions become no-ops and cannot open transactions. Check the terminal/failure latch between dispatch opportunities, including after an immediate completion or a reentrant failure slot. No second external operation may start because the current cycle already loaded candidates.

Keep normal cancellation separate: accepted user cancellation still completes through normal persistence. Daemon shutdown deliberately does not call cancellation paths that store Cancelled outcomes. Durable Running rows are left for startup recovery.

## 11. Event-loop stop and daemon teardown

Add `EventLoop::request_quit() noexcept`, owner-thread only, setting the existing running flag directly. It does not enqueue, allocate, drain, block, or destroy objects. Existing thread-safe `quit()` remains available and retains its documented behavior. Document that a request made before `run()` is not sticky across a later `run()`; the daemon checks its own terminal latch and never enters `exec()` after shutdown.

Keep existing final generic-task/deferred-delete drains. Tests must show that gates remain effective during those drains. Do not broaden this change into event-loop dispatch ordering redesign.

Daemon runtime state can be an ordinary owner-thread private composition object. If implemented as Object for receiver tracking, use ObjectPrivate correctly. Fatal reusable notifications come from scheduler, management, and HTTP client signals; a private signal-relay notification may be a signal if Object-based, otherwise a narrow owned notification is acceptable.

After event-loop return:

1. Ensure all gates are latched, including when `exec()` itself failed.
2. Close listener and RPC connections with existing APIs; disable admission slots.
3. Shut down executor group while Scheduler still exists but its completion token is invalid.
4. Destroy runner-owned operations, then HTTP client/infrastructure, while required event-loop and dependency objects still exist.
5. Destroy service/scheduler/runtime dependents; close database after their queries/transactions are gone.
6. Restore/remove daemon signal relay safely; exit using the latched status.

Add `AttemptExecutorGroup::shutdown()` as an idempotent owner-thread operation. It first marks itself unavailable, rejects new starts/registrations with a safe stopping error, destroys owned executors, then clears routes. No retained completion is invoked. Its destructor delegates to the same path. Cancellation after shutdown reports stopping/inactive according to the documented contract, without reaching a destroyed child.

HTTP shutdown cancels active transfers immediately through existing ownership teardown. CLI teardown sends SIGKILL to owned process groups and reaps direct children using Phase 6 behavior. No grace timer and no durable finalization writes. Do not wait for proof that every descendant has terminated. Do not claim a hard wall-clock bound against an uninterruptible kernel wait; “immediate” means no application grace period or normal job-completion wait.

Safe first-failure logging records subsystem, stable code, and sanitized reason. Avoid dumping database `.detail`. Cleanup failures cannot change a previously fatal exit into success. Do not invent pervasive allocation-failure recovery or `std::_Exit` fallbacks that bypass runner cleanup.

## 12. SIGTERM/SIGINT relay

Keep signal ownership in `jobud`, not in reusable `jb::core::Process`. Do not install a SIGCHLD handler or change Process child-reaping policy.

Use a daemon-owned POSIX self-pipe relay, with nonblocking close-on-exec descriptors and an event-loop watch. The handler only preserves/restores `errno`, publishes a monotonic `volatile sig_atomic_t` shutdown flag through always-lock-free atomics, acquires/releases a descriptor borrow through an always-lock-free admission counter, and performs a best-effort async-signal-safe write. It performs no allocation, logging, JSON, Object access, or teardown. Full-pipe writes may be dropped because the flag and existing readability preserve the request. The flag is never cleared during this daemon lifetime.

Block target signals during relay installation; save/restore previous dispositions and mask; unwind partial setup. Establish handler/descriptor lifetime before unblocking. Signals received before event-loop entry are observed by startup/recovery polling. The owner-thread watcher drains a bounded amount and requests normal shutdown. Coalesce repeated signals; there is no second-signal bypass of cleanup.

Keep the relay alive until application-owned objects that may create threads are destroyed, but do not assume their destruction joins every library worker. In particular, supported libcurl threaded DNS resolvers can outlive client cleanup; `curl_global_cleanup()` is not a worker-join barrier. Install before worker creation and deliberately allow workers to inherit the enabled TERM/INT mask. During owner-thread retirement, block those signals on the owner and restore dispositions, then atomically close handler admission and wait only for already admitted descriptor borrowers before closing the pipe. Admission and the borrower count must share one atomic word so retirement cannot miss a handler between checking admission and acquiring a borrow. A handler selected before disposition restoration may enter late; it must touch only process-lifetime state and skip descriptor access after admission closes. The owner-thread wait has no hard wall-clock bound, but never waits for worker exit. Document that this is process-main infrastructure, not an embeddable global handler API.

Validate retirement with deterministic subprocess cases that pause real handlers before admission and after a descriptor borrow, including concurrent TERM/INT handlers, partial-restoration retry, surviving workers, and descriptor reuse after relay destruction. Any handler checkpoints must be compiled only into the isolated test helper; production has no handler hooks or runtime test switches.

Use platform descriptor setup helpers without weakening CLOEXEC on either end. Check every pipe/watch/handler installation failure and fail startup cleanly. Test with real signals in subprocesses, never by changing the test runner's signal dispositions.

## 13. Fault injection and evidence design

Use two complementary test layers:

- A test-only generic Driver/DriverQuery decorator injects failures at named operation boundaries while forwarding to the real driver. Cover begin, statement execution, fetch, commit, and rollback behavior without production fault switches. Tests refer to logical boundaries, not brittle global SQL-call ordinals.
- SQLite integration demonstrates real transactional rollback and reopen/recovery behavior. Where simulating physical I/O or commit ambiguity requires a test VFS, isolate it in SQLite tests; do not put sqlite3 types into generic JobU. Use deterministic targeted failures, not filesystem permission tricks that succeed under root.

A failure returned before a commit must leave prior durable state intact. A deliberately injected error after a successful commit models an ambiguous acknowledgement: assert fatal shutdown and validate reopened committed state; do not incorrectly insist that it remained Running. For ordinary precommit completion failure, the attempt/run remain Running with no partial output, retry, or successor.

Minimum fault matrix:

1. Dispatch begin/write/commit failure: zero external starts for that attempt.
2. Completion attempt/output/run/retry/successor/suspension write failures: full rollback, no capacity reuse or dependent dispatch.
3. A second queued completion after first fatal failure: no additional persistence calls.
4. Management mutation storage failure: no subsequent buffered/direct mutation; expected conflicts remain nonfatal.
5. Ordinary read I/O error versus corruption: ordinary response versus terminal shutdown.
6. Recovery failure before/within/after committed units: no serving; reopening and rerunning converges without duplicate retries/successors.
7. Rollback failure/poisoned connection: terminal shutdown, no reuse; preserve meaningful first failure.

Subprocess fixtures use temporary databases/sockets, deterministic child-ready handshakes, and parent-owned watchdogs only as test protection. Prove readiness before SIGKILL/SIGTERM. Avoid timing sleeps as the evidence that a child is running. Reopen only after the previous daemon released ownership.

For crash recovery test both policies and exhausted retries, manual RetryWait barrier, suspended queue/job, recurring next tick, and output-loss metadata. For immediate shutdown run active HTTP plus CLI work, send SIGTERM and SIGINT separately, verify no new dispatch/admission, direct-child cleanup, unchanged durable Running state, and correct next-start interruption handling. Process-group evidence uses controlled descendants; no assertion requires observing every arbitrary descendant terminal.

## 14. Individually reviewable implementation stages

Each stage includes documentation for its changed contracts, focused tests, and a handoff naming exact source revision, commands, outcomes, and known next-stage work.

### Stage 7.1 — Baseline and invariant fixtures

Audit all persisted run/attempt writers and schema constraints. Record valid Scheduled/Running/RetryWait/terminal row shapes, deletion/suspension cases, and whether Pending/output-before-completion can exist. Add reusable database fixture builders and assertions for Phase 7 tests. Confirm no schema migration is needed. Verify fixtures through real SQLite and existing domain decoders. No production recovery yet.

### Stage 7.2 — Shared fatal-storage classification

Add private operation-context classification and sanitized errors. Test expected domain conflicts, raw unexpected constraints, `db.corrupt`, read-only failures, and write-path failures. Do not change all callers in this stage. Verify generic JobU includes remain SQLite-free.

### Stage 7.3 — Recovery scan repository

Implement bounded keyset scans and cross-row validation helpers. Test multiple pages, UUID ordering, malformed rows, orphan/duplicate active attempts, invalid barriers, and unexpected pending/output shapes. No writes or external execution.

### Stage 7.4 — Interrupted transition and capture metadata

Implement transaction-local attempt/output/run interruption, including a constrained Interrupted run setter. Test atomic success and rollback; prove timestamps/snapshot/attempt number are preserved appropriately. Keep normal completion behavior unchanged.

### Stage 7.5 — Recovery retry policy

Implement recovery-specific eligibility and shared delay arithmetic. Test both policies, limit exhaustion, next-attempt overflow, fixed/exponential delay, jitter, blocking/reschedule, suspended owners, and deleted-owner rules. Assert no next attempt is precreated.

### Stage 7.6 — Recurrence and suspension helpers

Extract/reuse successor construction and implement missing-successor and drained-suspension repair. Test strict time lower bound, edited definition snapshots, suspended recurring definitions, deleted definitions, one-time exclusions, duplicate constraints, revision overflow, and manual barrier release/retention.

### Stage 7.7 — Complete synchronous recovery service

Add public API, orchestration, safe report counters, private stop predicate, and final validation. Test crash/restart between committed repair units, cancellation rollback, bounded pages, and idempotent second invocation. Successful recovery must satisfy Scheduler's existing startup running-state check.

### Stage 7.8 — Scheduler terminal gate and completion lifetime

Add irreversible shutdown/state behavior and invalidatable completion token. Test resumable stop remains unchanged; shutdown/restart rejection; retained callbacks after core destruction; first-failure suppression of later completions; and immediate-completion reentrancy between dispatch opportunities. Use exact-operation callbacks, not signals, for executor completions.

### Stage 7.9 — Management admission and failure signals

Add irreversible mutation gate and once-only fatal notification. Cover every current mutation entry point and result boundary after RAII unwinding. Test direct calls, buffered RPC requests, ordinary conflicts, corruption reads, and absence of mutation_committed on failure.

### Stage 7.10 — Executor-group explicit shutdown

Add idempotent group teardown and stopping behavior. Test mixed runners, start rejection, no callback delivery, child-before-route destruction, dependency lifetime, and repeated destructor/shutdown calls. Preserve concrete runner/process cleanup contracts.

### Stage 7.11 — Owner-thread event-loop exit request

Add `request_quit()` and Doxygen. Test exit during watcher/task dispatch, final task drains, existing cross-thread `quit()` behavior, and pre-run nonsticky semantics. No allocation/posting dependency in the new operation.

### Stage 7.12 — Linux daemon signal relay

Implement daemon-private relay and startup polling. Subprocess-test SIGTERM/SIGINT during startup and event-loop operation, repeated signals, pipe coalescing, setup failure cleanup, descriptor inheritance, and safe lifetime ordering. Keep SIGCHLD untouched.

### Stage 7.13 — Daemon startup and terminal lifecycle integration

Refactor composition around recovery and runtime gates. Connect scheduler, management, and HTTP failure signals. Implement safe unwind/teardown ordering after exec returns and on partial startup. Test recovery failure prevents listening/dispatch, scheduler startup failure, listener failure after startup, and event-loop failure. No asynchronous object destruction from fatal slots.

The daemon-private runtime owns services and runner infrastructure beneath the borrowed loop/database dependencies. Its synchronous runner factory is called only after recovery, and its event-loop entry callable only after scheduler/listener startup; these same boundaries support deterministic lifecycle tests without production fault options. Because the existing HTTP client can queue failed completions before its fatal signal, the daemon's scheduler-facing execution boundary also checks the client's stored failure before dispatch and completion forwarding. This leaves generic executor routing and HTTP callback contracts unchanged. A raw event-loop polling failure is observed through the nonzero `exec()` result; the runtime then latches every gate before teardown, retaining the existing loop drain behavior.

### Stage 7.14 — Dispatch/completion transaction fault matrix

Add generic test decorator and SQLite rollback cases. Cover each state transition boundary in §13, including output and recurrence. Prove no launch before dispatch commit, no release on failed completion, and no writes from later callbacks. Include commit-acknowledgement ambiguity as a separate expectation.

### Stage 7.15 — Recovery/management failure matrix

Exercise partial recovery progress, restart convergence, poisoned connections, mutation admission, and ordinary read versus corruption behavior. Verify safe errors and once-only fatal notification. Do not expose test controls in daemon production options.

### Stage 7.16 — Linux crash/restart integration

Run real daemon/CLI/HTTP fixtures with SIGKILL and reopen. Cover both recovery policies, exhaustion, recurrence, barriers, suspension, and capture loss. Assert committed database state rather than only logs. Preserve existing CLI privilege rules; use an isolated permitted test fixture where root opt-in is necessary.

### Stage 7.17 — Linux immediate-shutdown integration

Exercise SIGTERM, SIGINT, and injected fatal runtime errors with active mixed work. Verify admission gates, no post-latch persistence, CLI group kill/direct-child reaping, HTTP cancellation, exit status, durable Running leftovers, and successful subsequent recovery. Use watchdogs without claiming a production hard shutdown deadline.

### Stage 7.18 — Native macOS adaptation and verification

After Linux stages pass, implement any required POSIX relay/build differences and run native kqueue/process/signal lifecycle tests plus recovery integration. Keep platform code isolated. Record actual host/toolchain and commands; never describe Linux compilation as native macOS evidence. If no macOS host is available, report this stage pending explicitly; do not reuse or demand the deleted historical Phase 6 evidence.

### Stage 7.19 — Documentation, public APIs, and boundary audit

Review all new public declarations, signal contracts, ObjectPrivate use, code comments, safe errors, startup/teardown ordering, and roadmap statements. Add an operator-facing explanation of unknown outcomes and the two recovery policies without expanding Phase 8 CLI scope. Ensure all temporary stage notes are reconciled and no test fault switches shipped.

### Stage 7.20 — Final clean Linux verification and closure

From a clean checkout, build the normal supported SQLite-enabled configuration and run all registered Linux tests, including Phase 7 suites. Use repository-documented commands and record exact commands, compiler, configuration, counts, and tested implementation SHA. Run relevant formatting/include/documentation checks required by AGENTS and changed files. A SQLite-disabled full test pass is not a routine phase gate; use a compile-only configuration only if actual CMake/dependency changes require proving optional-target isolation.

Record macOS Stage 7.18 evidence separately, or its explicit pending status. Do not silently claim complete native coverage. A documentation-only closure commit may refer to the exact previously tested implementation SHA with a documented diff; do not create an endless requirement to retest and recommit the handoff itself.

## 15. Phase completion criteria

Phase 7 is functionally complete when recovery precedes admission/dispatch; both recovery policies converge safely after repeated crashes; interruption, capture metadata, retry/recurrence, and suspension changes are atomic; storage failures latch terminal behavior before subsequent work; and immediate daemon shutdown leaves unresolved durable work for the next startup.

Linux integration and transaction-failure evidence must pass. Native macOS status must be stated accurately. Public APIs retain the reusable core and generic database boundaries. Phase 8 planning can then build on these lifecycle guarantees without changing their semantics.
