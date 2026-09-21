# JobU Phase 7 — Stage 7.19 boundary audit

## Scope and implementation identity

This review covers Phase 7 changes through merged Stage 7.18 commit
`91cc7eb8e22565954dc9fdb62e96095d1daa5a92` (PR #175), compared with the phase
entry baseline named in the [design](jobu-phase7-code-design.md).
Stage 7.19 is implemented and validated in the working copy, ready for review.
It adds documentation, reconciles C++ comments/test descriptions, and corrects
one public error boundary with a regression test. The C++ source snapshot is
`6d4ccf9d7d5e52ce2373d741cd3e3fa7341c5dfe`; subsequent changes only finalize
this documentation. This is an uncommitted Jujutsu snapshot, not a merged
Stage 7.19 commit. Stage 7.20 final clean Linux verification remains pending.

## Reviewed contracts and boundaries

| Area | Implementation and contract reviewed |
| --- | --- |
| Startup recovery | `recovery.hpp`, `recovery.cpp`, and private recovery/retry/recurrence helpers: exclusive schema-prepared database ownership; synchronous bounded scans; no external execution; independent repair transactions; report counters after commit; cancellation rollback and restart convergence. |
| Scheduler lifecycle | `scheduler.hpp/.cpp` and `scheduler_core_priv.*`: resumable stop versus terminal shutdown; retained completion token invalidated before failure notification/destruction; first failure retained; dispatch checks between opportunities. Startup preflight errors are sanitized by the correction below. |
| Management | `management.hpp/.cpp`: all current mutation entry points, including direct Run Now, use the admission/result boundary; transaction guards unwind before failure notification; poisoned cleanup overrides ordinary conflicts; reads remain callable; failed mutations do not emit `mutation_committed`. |
| Object ownership and signals | Scheduler, ManagementService, and DaemonRuntime extend the single ObjectPrivate allocation. Scheduler/runtime owner back-references are bound after base construction. Object-capturing failure/admission slots are receiver-aware; context-free logging slots borrow no Objects. |
| Executors and callbacks | Group shutdown closes admission and destroys children before routes. Child executors must suppress and release completion wrappers during destruction; those wrappers do not have the scheduler token's post-destruction delivery guarantee. Dependencies remain alive during runner cleanup. |
| Event-loop exit | `request_quit()` is owner-thread-only, allocation-free, and nonsticky before `run()`. Existing final task/deferred-delete drains remain; terminal gates protect persistence through those drains. |
| Daemon composition | `main.cpp` and `runtime_priv.*`: relay before workers; schema/recovery before runner construction; failure receivers before synchronous scheduler start; listener readiness afterward. Stop only latches gates; cleanup closes RPC, tears down runners with the scheduler alive, then services/database, and finally the relay. |
| Signal retirement | Shared POSIX relay with Linux/Darwin pipe setup: monotonic request, one atomic admission/borrow word, late-handler safety, watch lifetime, and retirement without waiting for surviving library workers. SIGCHLD remains untouched. |
| Storage/error policy | Recovery, management result boundaries, and scheduler terminal failures use the shared sanitizer. Expected translated conflicts stay ordinary; raw constraints, corruption, and durable invariant failures are fatal. Daemon failure logging uses trusted subsystem/code tokens. |
| Generic database APIs | `Database::is_poisoned()`, protected Driver/DriverQuery forwarding operations, and Transaction rollback cleanup: decorators preserve ownership and generic checks; rollback failure poisons the connection without aborting unwinding. Commit errors do not prove rollback. |
| Test isolation | Fault-driver sources are under `test/support` and linked only into tests. `JOBUD_SIGNAL_TESTING` is defined only for the isolated signal helper; production CMake targets and daemon options contain no runtime fault switch. Production symbol and compile-command checks passed. |

## Corrected finding: startup preflight returned backend diagnostics

In [Scheduler::Private::start()](../../src/jobu/scheduler.cpp), failure from
`SchedulerRepository::has_any_running_state()` previously returned the error unchanged.
The repository forwards Query preparation/execution/fetch errors, and the
generic database API deliberately preserves driver errors. SQLite errors
include backend diagnostics in `Error::detail`. Unlike the scheduler's later
terminal-failure boundary, this public `Scheduler::start()` result did not
pass through `sanitized_storage_error()`.

This pre-existing startup path conflicted with the phase's safe-public-error
requirement. The daemon logged only its stable code, but direct C++ callers
received backend text.

The result now uses `sanitized_storage_error()` with Validation context,
preserving category/code and the existing Stopped state and no-signal behavior.
The new scheduler fault test injects prepare/execute/fetch failures with private
text in both message and detail, using `db.io` and `db.corrupt` identities. It
checks sanitized results, unchanged durable rows, no executor start or timer,
no failure notification/latch, and a successful later start once the synthetic
fault is consumed. The real backing database remains healthy in that control.
There is no lifecycle, schema, RPC, or CLI change. No unresolved finding remains
from this audit.

## Documentation reconciliation

The [README operator guide](../../README.md#recovery-and-shutdown) replaces the
obsolete recovery-unavailable claim. It explains unknown outcomes, lost capture,
both recovery policies, the default one-attempt limit, duplicate external
effects, suspension/barriers/recurrence, immediate shutdown, and commit ambiguity.
The existing queue-create policy option is documented; Phase 8 commands remain
deferred and API version 1.2 remains unchanged.

The planning index and technical-plan roadmap identify merged implementation
and pending closure separately. The design's original baseline table and
numbered implementation sequence remain historical context. The
[Stage 7.1 baseline](jobu-phase7-baseline.md) retains its named source inventory
with an explicit pointer to the later recovery implementation. Recovery fixture
and repair tests now name the implemented validation/service boundaries instead
of referring to future numbered stages.

## Native evidence provenance

The shared handoff resolves to `/home/enar/cloud/Projektid/JobU/HANDOFF.md`.
It identifies `phase7-macos/jobu-phase7-macos-evidence.md` in that shared directory
as the authoritative native Stage 7.18 record. That record was read for this
audit; its archived logs are prior evidence, not tests run during Stage 7.19.

The native record names tested snapshot
`fc05e20e86cec9fb7257340fec3bc9b1002a77c0`, macOS 26.6.2 arm64, Apple clang
21.0.0, Debug with SQLite enabled: 136/136 CTest executables passed, and signal
and shutdown suites each passed ten repetitions. The handoff records an empty
diff and 420 matching source hashes connecting that snapshot to merged
`91cc7eb8`; this audit has not repeated the manifest comparison. The native
root-only denial case was skipped under UID 501. Historical Linux/Alpine
Stage 7.17 evidence applies to its earlier source, not the Stage 7.18 adaptation.

The Stage 7.19 error-result correction has Linux validation below; it has not
been rerun natively on macOS. The native record remains evidence for its exact
Stage 7.18 source, not a claim that the new correction was tested on that host.

## Stage 7.19 Linux validation

Host: Linux 7.2.6-zen2-1-zen x86_64; GCC 16.2.1 20260810; CMake 4.4.3.
The existing `.bld` configuration uses Ninja, Debug, C++20 and
`JB_BUILD_SQLITE_DRIVER=ON`. These are focused working-copy checks, not the
clean-checkout full-suite gate reserved for Stage 7.20.

The new regression was first built against the unchanged startup return and
failed exactly its 12 message/detail privacy assertions across six injected
cases. After the correction, the same case passed all 708 assertions:

```sh
cmake --build .bld --parallel 4 --target scheduler-transaction-fault-test
.bld/test/scheduler-transaction-fault-test '[startup]' --reporter compact
```

The production daemon, affected public-header checks, and focused lifecycle /
transaction tests built successfully with:

```sh
cmake --build .bld --parallel 4 --target \
    scheduler-transaction-fault-test scheduler-event-loop-test jobud-runtime-test \
    recovery-service-test recovery-transaction-fault-test management-transaction-fault-test \
    management-admission-test storage-failure-test attempt-executor-group-test \
    event-loop-test db-transaction-test jobu-recovery-public-header-test \
    jobu-scheduler-public-header-test jobu-management-public-header-test \
    jobu-attempt-executor-group-public-header-test core-event-loop-public-header-test \
    db-driver-public-header-test db-driver_query-public-header-test public-headers-test jobud
ctest --test-dir .bld/test --output-on-failure -R '^(scheduler-transaction-fault-test|scheduler-event-loop-test|jobud-runtime-test|recovery-service-test|recovery-transaction-fault-test|management-transaction-fault-test|management-admission-test|storage-failure-test|attempt-executor-group-test|event-loop-test|db-transaction-test|jobu-recovery-public-header-test|jobu-scheduler-public-header-test|jobu-management-public-header-test|jobu-attempt-executor-group-public-header-test|core-event-loop-public-header-test|db-driver-public-header-test|db-driver_query-public-header-test|public-headers-test)$'
```

Result: **19/19 passed, 6.47 seconds**. CTest ran with native socket/process
access for the lifecycle fixtures. After final comment reflow and recovery-test
stage-note reconciliation, the following also built and passed:

```sh
cmake --build .bld --parallel 4 --target recovery-fixture-test recovery-repair-test
ctest --test-dir .bld/test --output-on-failure -R '^recovery-(fixture|repair)-test$'
```

Result: **2/2 passed, 0.57 seconds**. The final comment-only changes to headers
do not change the contracts exercised by the earlier 19 tests.

All nine changed C++ sources/headers have clean clangd diagnostics with the
Linux compilation database. Formatting and whitespace checks passed:

```sh
clang-format --dry-run --Werror src/db/transaction.hpp src/jobu/recovery.hpp \
    src/jobu/scheduler.hpp src/jobu/scheduler.cpp src/jobud/runtime_priv.cpp \
    test/scheduler-transaction-fault-test.cpp test/recovery-fixture-test.cpp \
    test/recovery-repair-test.cpp test/support/recovery_fixture.hpp
git diff --check
```

Local Markdown link targets and operator-guide anchors resolve. Source-token
comparison confirms the only production behavior change is the scheduler
preflight sanitizer; the other production edits are comments/whitespace.
`nm -C` inspection of `.bld/src/jobud/libjobud-signal.a` and
`.bld/src/jobud/jobud` found no `signal_test_checkpoint`, `FaultDatabaseDriver`,
or `DatabaseFaultState::checkpoint` symbols. Relay entries in
`.bld/compile_commands.json` define `JOBUD_SIGNAL_TESTING` only for the isolated
test helper. Generic JobU recovery/storage-policy files remain SQLite-free.

## Next stage

Stage 7.20 must record a clean supported SQLite-enabled Linux build and
full test run against the final implementation, including Linux-only changes
from Stage 7.18. No native rerun or phase-closure claim is made here.
