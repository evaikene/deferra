# JobU persisted-state baseline before startup recovery

## Scope and source

This audit records the persisted-state contracts of production writers at
`d41a5ff2`, before startup recovery is implemented. It distinguishes existing
storage capabilities from the recovery behavior specified by the Phase 7 design.
The invariant tables and fixture descriptions are a reference for implementation
and review across the phase; they do not change production behavior or the schema.

This is a historical Stage 7.1 record. Its future-tense recovery notes are resolved
by the merged Phase 7 implementation: recovery writes Interrupted history and
lost-capture metadata, and scheduler validation accepts that history for RetryWait.
The [Stage 7.19 audit](jobu-phase7-audit.md) maps the current contracts and evidence;
the writer inventory below continues to describe the named pre-recovery source.

The governing scope is [Phase 7 §§6–8 and Stage 7.1](jobu-phase7-code-design.md),
with [technical-plan §§15.2 and 19](jobu-v1-technical-plan.md) defining shutdown and
unknown outcomes. The distinctions below matter when implementing recovery scans:
schema acceptance, individual row decoding, and a reachable committed daemon state
are different guarantees.

## Writer inventory and transaction boundaries

| Writer | Committed effect and caller boundary |
|---|---|
| `RunRepository::insert_schedule_owned` (both overloads) | Management creation inserts an initial Scheduled snapshot in the definition transaction. Scheduler terminal completion/cancellation inserts a recurring successor in its terminal transaction. The domain-value overload also supports historical states for repository fixtures; these are not extra daemon writers. |
| `RunRepository::insert_manual` | Management Run Now inserts a separate Scheduled manual snapshot, with optional idempotency record in the same transaction. It requires an existing future Scheduled schedule-owned run, no Running/RetryWait work for the job, and no other nonterminal manual run. Public RPC delivery remains outside this phase. |
| `RunRepository::refresh_unstarted_schedule_owned` | Management definition editing refreshes only a Scheduled schedule-owned run with no attempts, atomically with the definition revision. An already claimed recurring snapshot stays unchanged; converting it to Once is rejected. |
| `RunRepository::move_non_terminal` | Management move requires a suspended job and atomically updates its definition plus every nonterminal run's queue and revision. Terminal history keeps its original queue/revision. |
| `RunRepository::cancel_pending_for_job/queue` | Management deletion cancels Scheduled/RetryWait rows, adds completion/result metadata, and preserves existing attempt history in the tombstone transaction. No cancellation attempt is synthesized. |
| `SchedulerRepository::cancel_pending_run` | Scheduler cancellation handles Scheduled/RetryWait runs without creating an attempt. Recurring successor and drained suspension changes share its transaction. |
| `AttemptRepository::insert_attempt` plus `SchedulerRepository::mark_dispatch_running` | `dispatch_attempt()` inserts the next attempt directly as Running, then marks its run Running; both commit before any external start. Failed external start is subsequently handled as a normal completion. The run's first start uses COALESCE and survives retries. |
| `SchedulerRepository::complete_attempt`, `set_run_retry_wait`, `set_run_terminal` | `process_completion()` completes the attempt, conditionally stores capture, updates the run, creates any recurring successor and finishes drained suspensions in one transaction. Only after commit is in-memory capacity released. Terminal setters accept Succeeded/Failed/Cancelled, not Interrupted. |
| `AttemptRepository::insert_or_replace_output` | Its only production caller is `process_completion()`, after the attempt completion update and before the run transition in the same transaction. The low-level API can replace an existing row; it is not a streaming-output writer. |
| `RunRepository::delete_selected_terminal` | Retention deletes selected terminal runs in bounded chunks inside its transaction. Foreign-key cascades delete attempts and output. Retention removes owner tombstones only once no retained rows refer to them. |

Sources: [run repository](../../src/jobu/run_repository_priv.cpp),
[attempt repository](../../src/jobu/attempt_repository_priv.cpp),
[scheduler repository](../../src/jobu/scheduler_repository_priv.cpp),
[dispatch](../../src/jobu/scheduler_dispatch_priv.cpp),
[completion and recurrence](../../src/jobu/scheduler_core_priv.cpp),
[management](../../src/jobu/management.cpp), and
[retention](../../src/jobu/retention_repository_priv.cpp).
The audit includes all INSERT/UPDATE/DELETE sites for runs, attempts and output in
`src/`, including the foreign-key cascades. No daemon writer precreates Pending
attempts, writes output before completion, or writes Submitted runs.

## Run and attempt shapes

In this table, “history” means committed production history before recovery is
implemented. Attempts start at 1 and dispatch uses the next number after the latest
persisted attempt. Run snapshots retain runner type, payload, attributes, priority,
planned time, and definition revision, subject to the explicit unstarted-refresh
and move operations above. Recovery must not compare a historical snapshot with
the current definition for equality.

| Run state | Run metadata | Attempt history |
|---|---|---|
| Scheduled | No started/completed/result; original due time | None. No speculative Pending attempt. |
| Running | First start present; completed/result absent | Exactly one Running attempt, latest numbered; earlier attempts are completed failures. Current attempt due time equals the run's current runnable time; run first start can precede the latest attempt start. |
| RetryWait | First start present; completed/result absent; runnable time advances to retry due | One or more completed Failed attempts; no unfinished or next attempt. Blocking versus reschedule affects capacity, not row creation. |
| Succeeded / Failed | Started/completed present; production writes a result object | Latest attempt is Completed with the corresponding outcome; earlier retries remain in history. An external start failure still has durable start metadata. |
| Cancelled before first dispatch | No start; completed and cancellation result present | No attempts. |
| Cancelled while RetryWait | First start, completed and cancellation result present | Completed failures remain; no extra Cancelled attempt is created. |
| Cancelled while Running | First start, completed and cancellation result present | Latest attempt completes as Cancelled; earlier failures remain. |
| Interrupted | Started/completed present; result is representable | Storage supports a Completed/Interrupted attempt and terminal Interrupted run. No baseline normal completion writes them. Phase 7 recovery will add the writer. |

`AttemptRepository` accepts Pending with no start/completion/outcome/result, Running
with a start and no terminal metadata, and Completed with a completion/outcome and
a start (except that Cancelled may lack a start). Those broader low-level shapes
are used by existing repository tests; they do not establish a legitimate daemon
crash state. In particular, Pending must remain an explicit future recovery
invariant failure rather than something recovery executes or silently removes.

Production completions write result objects, but decoders accept absent result
objects for terminal runs and completed attempts. Row decoders validate types,
enums, UUIDs, representable timestamps, attributes, JSON objects, and local state
field relationships; they do not certify cross-row consistency. Do not invent a
strict monotonic ordering of all wall-clock timestamps: dispatch preserves its
sampled UTC times, while the clock can move. Validate the relationships guaranteed
by writers, not a synthetic always-increasing wall clock.

Output is optional at the scheduler completion boundary. No output row, a NULL
channel, and a present zero-length BLOB are distinct. Existing completion may
retain output for an earlier failure while the run is Running on a later attempt;
that is valid. An output row for the *currently Running attempt* is not emitted by
any baseline production writer. Recovery's empty blobs, false truncation flags,
and `capture_lost=true` fit the existing output table. They describe unknown capture,
not evidence of zero emitted bytes.

## Owners, recurrence, barriers, and suspension

- Queue/job suspension preserves Scheduled and RetryWait work. Running work leaves
  its owner Suspending until it drains; RetryWait alone does not prevent suspension
  completion. Each job state transition increments revision with overflow checks;
  queue transitions update time. Manual work may run for a suspended/suspending job
  while its queue remains Active, so a suspended **job** with Running manual work
  is not automatically corrupt. A suspended queue blocks dispatch.
- Job and queue deletion require suspended, drained ownership and reject Running
  work. Deletion cancels pending runs atomically; deleted definitions/queues retain
  terminal history only. A Running run under a deleted owner is not a supported
  baseline state. A retained terminal run can refer to a formerly owning queue
  after a move, including a deleted old queue. Apply current queue-equality checks
  to nonterminal work, not indiscriminately to historical rows.
- There is at most one nonterminal schedule-owned run per job. A nonterminal manual
  run is a separate barrier and coexists with one schedule-owned sibling.
  `list_manual_barriers()` checks both sibling counts; terminal manual history no
  longer forms a barrier. No durable barrier table exists.
- Normal terminal schedule-owned recurring completion creates a successor from
  the latest definition, strictly after `max(terminal_at, old_planned_at)`, even if
  the definition is suspended/suspending. Deleted and Once definitions get none;
  manual completion does not create a successor. Management cannot convert a
  claimed recurring run to Once. Recovery uses its own time lower bound as designed.
- Missing recurring work and drained Suspending owners are deliberate repair
  inputs for later stages, not ordinary committed outputs of the baseline atomic
  completion path. Active Once definitions with no nonterminal work can be valid
  after completion or retention; absence alone must not invent another execution.

## Schema guarantees and application responsibilities

[Schema v1](../../src/jobu/sqlite/sqlite_schema.cpp) enforces column storage types,
enum spellings, positive revisions/attempt numbers, UUID blob length, schedule and
tombstone field combinations, primary keys and owner foreign keys. The partial
unique index enforces one nonterminal schedule-owned run per job. Composite keys
ensure one attempt number per run and one output row per attempt; foreign keys
cascade history deletion. UUID byte length alone does not validate UUID semantics.

There is **no** unique manual-barrier index, one-Running-attempt index, state-based
output restriction, or cross-row owner/state/attempt-number relationship check.
The run repository enforces manual insertion conflicts in application code. The
schema accepts, and individual decoders can accept, multiple Running attempts,
missing active attempts, Pending attempts, early output, deleted-owner active work,
and duplicate manual barriers. Recovery scans must validate these relationships without
relying on `ensure_schema()` or adding generic JobU PRAGMAs.

No migration is needed: Interrupted already exists in both outcome/state enums;
capture-loss metadata, retry due times, current recovery policy, immutable
attributes, recurrence definitions, and suspension metadata already have storage.
Capacity and manual barriers remain derived. Tests round-trip the intended
interruption representation and reopen the unchanged version-1 schema.

## Fixtures and verification boundary

[RecoveryFixture](../../test/support/recovery_fixture.hpp) owns a temporary real
SQLite database, provides deterministic multi-byte UUIDs, owner/run/history
builders, and a repository-based comparison of every persisted run/attempt/output
field. Histories are intentionally small; future keyset tests can seed many runs
without using a one-byte ID space. Seeding uses existing serializers, a transaction,
and explicit row insertion, including manual terminal states that the new-manual
repository API cannot insert. It does not simulate management authorization.

[recovery-fixture-test](../../test/recovery-fixture-test.cpp) covers both runner
families, run origins and recovery policies; history and capture round trips;
owner tombstones and suspension; manual barriers; schema constraints; and clearly
marked inconsistent rows. Existing production decoders read the fixtures, but no
test claims a recovery scan or service has been implemented. Later suites can
reuse the support source without refactoring unrelated existing tests.

Recovery integration must account for the baseline `validate_candidate()` and
`capacity_usage()` requirement that all RetryWait history outcomes are Failed.
Recovery-created Interrupted history needs deliberate scheduler eligibility
integration before serving; merely writing RetryWait is insufficient. Likewise,
the baseline `set_run_terminal()` rejects Interrupted and recovery requires a
constrained transition. Neither change requires a schema migration.
