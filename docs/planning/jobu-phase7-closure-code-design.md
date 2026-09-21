# JobU Phase 7 closure — Cancellation rollback failure

## 1. Purpose and baseline

Completed: the merged Stage 7.21 correction passed Stage 7.22's fresh Debug,
SQLite-enabled Linux build and all 135 registered tests, with one internal
root-only case skipped. The cancellation rollback finding is resolved. This
correction has Linux-only execution evidence under the approved platform choice
in §7; earlier native evidence retains its original source scope. Exact tested
identities, the observed pre-fix failure, and verification results are retained
in the shared external `jobu-phase7-verification.md` record. Phase 8 is the next
planning boundary.

The defect description and implementation stages below preserve the original
audit baseline and correction contract.

This plan closes the single issue found in the final independent Phase 7 audit: scheduler cancellation can return an ordinary operation error after failed transaction cleanup has poisoned the database, without immediately closing scheduler completion acceptance or notifying the daemon.

Baseline: the merged Stage 7.20 tree, following the Stage 7.19 implementation and final verification. Refresh `main` and read the current root `AGENTS.md` before implementation. Confirm the cancellation boundary still lacks the connection-health check described below; reconcile any intervening fix before editing.

This is a corrective extension to `docs/planning/jobu-phase7-code-design.md`, using Stages **7.21–7.22**. The existing Phase 7 implementation and its evidence remain the baseline. The final audit checked the merged implementation and native source provenance but did not execute a new C++ regression; Stage 7.21 must establish the failure with a deterministic test.

Keep exact tested revisions and execution provenance in the existing standalone `jobu-phase7-verification.md` record. Follow the repository's current convention of omitting specific commit IDs and commit URLs from planning documents.

Implement one numbered stage at a time and stop for review after its handoff. This document authorizes no automatic continuation beyond the user's approved stage and no autonomous commits.

## 2. Defect and required behavior

`SchedulerCore::cancel_run()` calls `cancel_run_impl()`, then classifies only its returned error. The public `Scheduler::cancel_run()` also classifies that returned error to decide whether to enter Failed and emit `failed`.

For a pending recurring run, cancellation performs these operations inside an RAII transaction:

1. Re-read and validate the run.
2. Mark it Cancelled.
3. Generate and insert its recurring successor.
4. Complete eligible suspension transitions and commit.

A cron or UUID generation error can interrupt step 3. It is normally an operation error: successful rollback preserves the original pending run and lets the caller retry. If rollback also fails, `Database` poisons the connection, but the original cron/UUID error still reaches the cancellation boundary. Since that error is nonfatal under the shared classifier, the scheduler remains Running and its completion token remains valid.

The database itself refuses further operations once poisoned. This defect does **not** establish that later writes succeed or that external work launches without a durable claim. The missing guarantee is immediate terminal notification and shutdown of already active work, rather than waiting for a subsequent database operation to discover the poison.

Required invariant:

> After cancellation's local queries and transaction guard have unwound, a poisoned database must make the cancellation result fatal, invalidate scheduler completion acceptance, and reach the existing daemon failure path before the public call returns.

`ManagementService::Private::invoke()` already implements the equivalent post-cleanup check. Use its semantics as the reference; do not refactor management as part of this fix.

## 3. Scope and expected files

| File | Expected work |
| --- | --- |
| `src/jobu/scheduler_core_priv.cpp` | Check database health after `cancel_run_impl()` returns; select the appropriate fatal result and latch core failure |
| `test/scheduler-transaction-fault-test.cpp` | Add deterministic cancellation/rollback regressions using the existing real-SQLite fault decorator |
| `test/scheduler-core-test.cpp` or the existing fault-test target | Exercise the private core boundary directly, as well as the public scheduler behavior |
| `src/jobu/scheduler.cpp` | Inspect and retain the existing public failure propagation; change only if the regression demonstrates a necessary integration adjustment |
| Phase 7 planning status/index and standalone verification record | Record the correction and its actual verification |

Prefer extending existing test fixtures and targets. Changes to shared test helpers are permitted only where needed to arrange the cancellation scenario. No new production fault injection, CMake target, schema, API version, or public method is expected.

Preserve normal cancellation, resumable `stop()`, irreversible `shutdown()`, recovery, runner teardown, signal relay, process-group behavior, and error sanitization. This plan does not deliver the Phase 8 public cancellation RPC/CLI or reopen the completed Phase 6 review.

## 4. Implementation contract

### 4.1 Check health at the result boundary

Keep existing entry checks for prior core failure and terminal shutdown.

Immediately after `cancel_run_impl()` returns:

1. Classify its result using `StorageOperation::Mutation` as today.
2. Read `_database.is_poisoned()` before another database operation can replace diagnostic state.
3. If poisoned, force fatal handling even when the original result was nonfatal. A theoretically successful result must not bypass this health check either.
4. Select the error according to the table below.
5. Call the existing `fail(selected_error, false)` and return the selected failure result.

`cancel_run_impl()` remains responsible for its transaction scope. Do not test health before its RAII destructor runs, add a second rollback attempt, or close/reopen the database in the scheduler.

Add an explicit `database.hpp` include if required by the new calls, following configured include-cleaner diagnostics. A small local block is preferable to a new shared abstraction for this one boundary.

### 4.2 Error precedence

| Original operation result | Health after cleanup | Returned/latching behavior |
| --- | --- | --- |
| Success | Healthy | Existing success behavior |
| Nonfatal failure | Healthy | Preserve the original operation error; keep the scheduler usable |
| Fatal failure | Healthy | Existing fatal behavior |
| Nonfatal failure or success | Poisoned | Promote to the connection's fatal cleanup error, with a safe fallback |
| Fatal failure | Poisoned | Preserve the original fatal error; do not replace it with the later rollback error |

For promotion, copy `Database::last_error()` when present and classified Fatal in Mutation context. Otherwise construct a safe `core::Error` with category Internal, code `db.connection_failed`, and a fixed message such as “The database connection is unusable after transaction cleanup.” Do not invent a new `jobu.*` error domain for an existing database failure.

This fallback matters because the public scheduler currently decides fatality from the returned error identity. Setting the private core's failure flag while returning an ordinary cron/UUID error would leave public state and daemon notification inconsistent.

Do not query or parse backend text to establish severity. Preserve the existing public sanitization path: `Scheduler::Private::fail()` sanitizes the error, stores the first failure, and emits the safe public failure. Do not expose private messages/detail or concatenate the original nonfatal error into the public cleanup error.

### 4.3 Notification and lifetime ordering

Continue using `fail(error, false)` at the synchronous core cancellation boundary. It records the first error and invalidates the completion token without invoking the asynchronous completion-failure notification callback.

The public `Scheduler::cancel_run()` then observes the fatal returned error, calls its existing failure path, enters Failed, and emits `failed` exactly once. The daemon's existing receiver closes management admission and requests immediate shutdown.

By the time a public `failed` slot runs:

- cancellation transaction cleanup has completed;
- the core completion token is invalid;
- the public scheduler is Failed;
- a retained completion delivered reentrantly by that slot cannot enter persistence.

Do not add a second notification path or destroy scheduler/executor state from inside the failure slot. Repeated shutdown and later callbacks retain their existing behavior. The synchronous core result remains the notification mechanism for direct private-core tests.

### 4.4 Readability and contracts

Add a short comment explaining why the connection-health check follows `cancel_run_impl()`: a nonfatal operation error is safe only when transaction cleanup succeeds.

No new Object, pimpl, callback abstraction, or signal is required. Preserve ObjectPrivate ownership and existing `failed` signals. Do not introduce exception catches or routine `@throws std::bad_alloc` comments.

Review the public cancellation documentation against the corrected behavior. Update it only if needed to state the fatal-cleanup contract; no signature changes are expected.

## 5. Deterministic regression design

### 5.1 Main reproducer

Use real SQLite through the existing `FaultDatabaseDriver` and `DatabaseFaultState`, with fake execution, time, cron, and UUID dependencies.

Arrange:

1. One active attempt whose completion handler is retained by the fake executor.
2. A separate recurring run still Scheduled, with a future due time so scheduler startup does not dispatch it.
3. A valid current recurring definition and configured future cron occurrences.
4. After successful scheduler startup, make successor generation return a recognizable nonfatal error. `FakeCronEngine::set_next_error()` is suitable; the existing core cancellation tests already use this seam.
5. Arm one failure at boundary `connection`, operation `Rollback`, phase `Before`, with code `db.rollback_failed` and private diagnostic markers.
6. Call the public scheduler's `cancel_run()` for the future recurring run.

Use deterministic fixture setup, not timing sleeps or a storage trigger that supplies the primary error. The primary error must remain nonfatal on its own; injecting `db.io` as the primary error would already activate the existing failure path and would not reproduce this defect.

Assert that the successor failure path was reached and the rollback injection fired. Before the production fix, the test must fail because the scheduler has not promoted the cleanup error and latched failure. Record that observed failure in the stage handoff.

### 5.2 Required assertions

After cancellation returns:

- The result is a fatal cleanup error, normally `db.rollback_failed`.
- The database is poisoned.
- `Scheduler::state()` is Failed and `failure()` agrees with the returned sanitized error.
- `failed` was emitted once, before the call returned.
- Neither the result nor notification contains the injected private diagnostics.
- The core completion token was already closed before notification: deliver the retained active completion from a receiver-aware `failed` slot and observe no additional fault-driver calls or notification.
- Later `start()` and `cancel_run()` are rejected by the existing terminal gate; no new executor start occurs.
- When the fixture safely closes and reopens its database, the cancelled run retains its pre-call durable state, no successor was committed, and the unrelated active attempt remains Running for recovery.

The poison flag makes database reads unavailable until reopen. Do not inspect rows through the poisoned connection. Respect the existing fixture's dependency lifetime rules during close/reopen.

Backend-call counts alone do not prove the completion token worked: a poisoned Database can reject calls before they reach the decorator. Pair those counts with assertions that failure was latched before the slot ran and that no later signal/state change occurs.

### 5.3 Companion cases

| Case | Required outcome |
| --- | --- |
| Same successor error, successful rollback | Original nonfatal error; healthy connection; no failure signal; pending run unchanged; clearing the injected error permits successful cancellation |
| Primary fatal database error during cancellation, then rollback failure | First fatal error preserved; poisoned connection; terminal gate closed; one sanitized notification |
| Direct `SchedulerCore::cancel_run()` with nonfatal successor error and failed rollback | Returned cleanup failure is stored in the core; a subsequent `process_cycle()` returns the same latched failure without new database work |

Reuse existing successful cancellation, missing-run, terminal-run, normal completion, and daemon failure-notification tests rather than duplicating them all. Both cron and UUID failures share this result boundary; one deterministic nonfatal successor failure is sufficient to reproduce the issue. Add a second generator variant only if it exercises materially different code.

Use the existing `jobud-runtime-test` case that verifies scheduler failure closes management admission before the notifying callback returns as integration coverage. A new daemon subprocess fixture is not required for this local correction.

## 6. Stage 7.21 — Correct cancellation cleanup and prove the failure gate

### Work

1. Confirm the current baseline and existing cancellation/management failure handling.
2. Add the main regression and execute it against the unfixed implementation. Confirm the intended failure, not a fixture/setup failure.
3. Add the connection-health check and error selection at the private core cancellation result boundary.
4. Add the companion assertions/cases in §5 and any necessary cancellation documentation.
5. Run focused Linux tests and changed-file checks.

Suggested commands, adapting only the build-directory path to the actual workspace:

```sh
cmake --build .bld --parallel 4 --target scheduler-transaction-fault-test scheduler-core-test scheduler-event-loop-test jobud-runtime-test
ctest --test-dir .bld/test --output-on-failure -R '^(scheduler-transaction-fault-test|scheduler-core-test|scheduler-event-loop-test|jobud-runtime-test)$'
git diff --check
```

Use the repository's configured SQLite-enabled Debug build. If no configured build exists, configure it with the documented normal options. Run changed-file formatting and clangd/include-cleaner checks required by `AGENTS.md`; no project-wide diagnostic scan or suppression is warranted.

### Acceptance

The reproducer fails before the fix and passes afterward. Nonfatal errors remain nonfatal when cleanup succeeds; cleanup poisoning immediately reaches the existing terminal failure path; the first fatal error remains authoritative. All focused tests and required changed-file checks pass.

### Handoff

Report changed files, the observed pre-fix failure, exact commands/results, implementation revision or source identity, and any unavailable checks. Stop for review before Stage 7.22. Do not mark the new closure issue resolved solely because a plausible code change was written.

## 7. Stage 7.22 — Verify the correction and record closure

Proceed after approval of Stage 7.21. Start from the reviewed implementation and confirm there are no unrecorded source/test/build changes.

### Native macOS coverage

The approved Stage 7.22 plan uses Linux-only execution evidence for this correction. The change selects a result after RAII cleanup through existing database-health and scheduler-failure paths; it changes no platform backend, event-loop primitive, signal handling, dependency, or build configuration. A separate native macOS session is not required for closure.

Keep the earlier native Stage 7.18 result under its original source scope; it does not verify this correction. A future native follow-up may build and run the same four focused targets, recording host, toolchain, tested source identity, commands, results, and skips. No full native suite, signal stress run, root-only test, or recovery of deleted historical evidence is required by this stage.

### Final Linux verification

Perform one fresh normal SQLite-enabled Debug build and one complete registered Linux test run:

```sh
cmake -S . -B .bld-phase7-closure -G Ninja -DCMAKE_BUILD_TYPE=Debug -DJB_BUILD_SQLITE_DRIVER=ON
cmake --build .bld-phase7-closure --parallel 4
ctest --test-dir .bld-phase7-closure/test --output-on-failure
```

Choose a new directory if that name already contains a build. Use the established authorized execution environment for socket/process/signal tests. Record actual counts and internal skips; do not assume the earlier 135-target count proves the new run succeeded.

No SQLite-disabled build or second full test matrix is required: this correction changes no optional-target or dependency boundary. Do not repeat broad testing after the stated acceptance checks pass unless a new concrete failure requires it.

### Documentation and evidence

Update the existing standalone `jobu-phase7-verification.md` with a clearly identified closure addendum, preserving its previous evidence and platform limits. Record:

- the audit finding and corrected boundary;
- the regression's observed failure before the fix;
- exact tested implementation identity and source equivalence to the published result;
- focused Linux and any native correction results;
- fresh full Linux commands, counts, skips, and outcome;
- status of the cancellation cleanup issue and any remaining execution limits.

Reconcile Phase 7 planning/index completion wording with this correction. Do not rewrite historical test runs as if they included the new regression. Do not create another evidence document merely to repeat the surviving verification record. If that record is external to the checkout, update the user-provided file or supply an explicit addendum for it; do not silently invent a repository copy.

A later documentation-only commit may reference the verified implementation. It does not require another full run solely to include its own commit identity.

### Acceptance and final disposition

Phase 7 may close when the cancellation cleanup regression and companion cases pass, the fresh Linux suite passes, the reviewed code contains the fix, and evidence accurately identifies the tested implementation and native coverage.

The completion report should state that the cancellation rollback finding is resolved, list the changed files and verification outcomes, disclose any native limitation, and identify Phase 8 as the next planning boundary. No remaining Phase 7 source change is anticipated by this plan.
