# JobU Phase 8 closure: code-level design — Revision 2

Revision: **2** (supersedes the unnumbered initial closure plan)  
Date: 2026-09-25  
Download filename: `jobu-phase8-closure-code-design-r2.md`  
Status: proposed implementation contract; no closure code implemented by this document  
Repository: <https://github.com/evaikene/deferra>  
Reviewed `main`: [`aaec33ca3a5d39d9d5bdd5af8c4c5c8474ad5efd`](https://github.com/evaikene/deferra/commit/aaec33ca3a5d39d9d5bdd5af8c4c5c8474ad5efd)  
Intended repository location: `docs/planning/jobu-phase8-closure-code-design.md` (replace the canonical plan with this revision; the download filename is revisioned to avoid stale downloads)

## 1. Purpose, authority, and scope

This document extends `docs/planning/jobu-phase8-code-design.md` with the work required to close Phase 8. It incorporates both findings in `jobu-phase8-final-audit.md` and the user's approved additions concerning one-time job states and CLI secret references. It takes precedence over the original Phase 8 plan only for the contracts it changes explicitly.

| Workstream | Required result |
| --- | --- |
| Audit F1 | Invalid attempt numbers produce ordinary validation failures; they cannot trigger the history service's fatal storage path. |
| Audit F2 | Destroying `ControlClient` from one of its public outcome signals cannot leave delivery code accessing freed private state. |
| One-time job lifecycle | Completed one-time definitions have durable `Succeeded`, `Failed`, or `Cancelled` states, with consistent completion, cancellation, recovery, and management behavior. Terminal definitions cannot accept new Run Now work. |
| Job listing | Ordinary `jobuctl job list` selects Active definitions; explicit options select terminal/suspended states or all definitions. |
| CLI secret references | `job create` and its `add` alias support ordered `--arg-secret` and named `--env-secret` values through the existing template format. |

**Excluded:** schema upgrades and legacy-data backfill; finished-job reruns and Clone; local-socket half-close/socat support; new transport events; installation/package targets; default socket discovery; retention scheduling or new deletion policy; new runner features; new database backends. Socket work was explicitly deferred by the user. Remote CLI commands continue requiring `--socket`; local help/version do not. Phase 9 remains the next feature-planning boundary after this closure.

Do not introduce a compatibility migration protocol, duplicate legacy API, or an API major-version change. There are no incompatible external clients to accommodate. Keep the existing API version `1.3` and the existing 30 RPC method names/capabilities; update their documented contracts. Keep only a **current-format marker of 3** because the job-state CHECK constraint changes. This marker is a mismatch guard, not an upgrade mechanism: create fresh databases directly in that format, validate current-format databases, and reject older/newer formats without converting them. Existing test installations must use a new database. Remove the existing v1→v2 upgrade implementation as well as the proposed v1/v2→v3 work.

Revision 2 incorporates the user's explicit scope corrections: no schema conversion, no reconstruction of old finished-job outcomes, and no reopening of terminal definitions through Run Now. Clone is outside v1. The two audit fixes and CLI secret-reference additions remain in scope. The stage sequence is reduced from 20 to **18 stages, 8.29–8.46**; this revision's numbering replaces the initial closure-stage numbering.

The final Phase 8 record reports 169/169 Linux and corrected 170/170 macOS CTest executables passing before these changes, with the root-only Catch2 case skipped on non-root hosts. Those results establish the baseline, not verification of this closure. The audit reproduced F1's codec/conversion/classification chain; F2 was source-traced. Neither should be dismissed because the earlier suite passed.

## 2. Repository map and implementation rules

### 2.1 Existing owners

| Files | Responsibility in this closure |
| --- | --- |
| `src/jobu/attempt.hpp`, `history_page_json.cpp`, `history_output_json.cpp`, `history_service.cpp` | Public attempt-number bound and validation at every public history-request entry. |
| `src/jobu/client/control_client.cpp`, `control_client_results.cpp`, `control_client_priv.hpp` | Deferred delivery, public signal boundaries, failure batches, cancellation, close/destruction. |
| `src/jobu/job.hpp`, `domain_storage_priv.cpp`, `management_json.cpp` | Job-state enum, durable text, result decoding, and request filters. |
| `src/jobu/sqlite/sqlite_schema.*`, `sqlite_schema_priv.hpp` | One current-format SQLite manifest, fresh creation, schema validation, and removal of legacy upgrade code. |
| `src/jobu/job_repository_priv.*`, proposed `job_lifecycle_priv.*` | Revision-checked job-state writes and common one-time lifecycle reconciliation. |
| `src/jobu/scheduler_core_priv.cpp`, `scheduler_repository_priv.*` | Terminal completion/cancellation transaction and manual-run relationship validation. |
| `src/jobu/recovery.cpp`, `recovery_repository_priv.*`, `recovery.hpp` | Interruption/retry transitions, current-format lifecycle validation, and committed transition counts. |
| `src/jobu/management.cpp`, `run_repository_priv.*` | Terminal Run Now rejection, management rules, idempotency replay, snapshots, soft deletion. |
| `src/jobuctl/commands/job_commands.cpp`, `command_registry_priv.cpp`, `command_line_priv.cpp` | List defaults/options; secret-reference construction; lexical compatibility; local help. |
| `src/jobuctl/session_priv.cpp` | Completion of an already-accepted suspend wait when a one-time job becomes terminal. |
| `src/jobu/payload_template_priv.*`, `cli_job_payload_priv.*` | Existing validation/reference grammar to reuse, not duplicate. |
| `docs/protocol/`, `docs/jobuctl.md`, `docs/secrets.md`, `docs/cpp-client.md`, `docs/planning/` | Same-stage contract documentation and final consistency audit. |

Prefer small helpers or focused source files over expanding the large management/scheduler files with another uninterrupted block. `job_lifecycle_priv.*` belongs to generic `jobu`; it uses `jb::db` and contains no SQLite-specific DDL or PRAGMAs. Schema code remains in `jobu-sqlite`. The typed client acquires no concrete driver or runner dependency.

### 2.2 Workflow and quality gates

Continue the repository's one-stage-at-a-time workflow. Stages start at **8.29**, following completed Stage 8.28. Implement only the approved stage, report its actual validation, and stop for the next approval. Temporary gaps between stages are accepted; do not inflate a foundational stage merely to make every later behavior work immediately. The assembled closure must satisfy every final gate.

For every changed public declaration, include useful Doxygen covering range/ownership/state/error semantics. Include comments and visual separation around non-obvious transaction ordering, borrowed lifetimes, revision changes, and transaction cleanup. Follow the selective `[[nodiscard]]` policy and the repository's exception rules; do not add routine `std::bad_alloc` catches or documentation.

Every Object subclass retains one ObjectPrivate-owned allocation. Use receiver-aware signals for reusable events. Existing per-operation correlation callbacks and private return-value strategies remain appropriate. Do not add callback-based substitutes for public client signals.

Run focused tests per stage and changed-file clangd diagnostics according to `AGENTS.md`. Public headers must compile through their supported standalone/owning translation units. Use full-suite runs for integration/final gates, not every small edit. Do not make `JB_BUILD_SQLITE_DRIVER=OFF` a second mandatory full test matrix.

## 3. Audit F1: bound public attempt numbers

### 3.1 Failure and intended contract

`AttemptNumber` is `std::uint64_t`; persisted attempts use positive signed 64-bit integers. Current request codecs accept `INT64_MAX + 1` and `UINT64_MAX`. Conversion then returns `jobu.storage.invalid_integer`, which HistoryService classifies as persisted-data corruption and forwards to the daemon's fatal-stop path.

The public domain is **1 through 9,223,372,036,854,775,807 inclusive**. Define the bound once in `attempt.hpp`, with an appropriately named public constexpr predicate:

```cpp
inline constexpr AttemptNumber maximum_attempt_number =
    static_cast<AttemptNumber>(std::numeric_limits<std::int64_t>::max());

[[nodiscard]] constexpr auto is_valid_attempt_number(AttemptNumber value) noexcept -> bool
{
    return value >= 1 && value <= maximum_attempt_number;
}
```

These are contract sketches: add the required includes, namespace, and Doxygen. The bound expresses the JobU domain; it must not require including a private storage header in a public codec/client.

### 3.2 Required validation boundaries

Use the common predicate in:

1. `attempt_get_request_to_json` and `attempt_get_request_from_json`.
2. `attempt_output_request_to_json` and `attempt_output_request_from_json`, alongside existing offset/channel/limit checks.
3. `HistoryService::get_attempt` and `HistoryService::read_output`, before any repository query or conversion.
4. Shared attempt summary/detail/output result codecs wherever they validate an attempt number. A malformed peer result is an invalid response, not a valid typed result with an impossible identity.

Preserve strict integer decoding: reject negative/zero, floating-point, boolean, null, and string values. Do not narrow an unchecked unsigned integer and test the narrowed result afterward. Preserve all existing UUID, channel, limit, and offset validation.

Invalid requests retain the existing codec invalid-parameters mapping (`-32602` at RPC). Direct service calls return an ordinary `InvalidArgument` domain error, using the existing `jobu.history.invalid_request` contract. The typed client rejects an invalid outgoing request before writing it. None of these paths emits `HistoryService::failed` or closes admission.

Keep the repository/storage bound as defense against internal misuse and corrupt persisted data. Do **not** weaken `classify_storage_failure`, reinterpret all `jobu.storage.*` failures as harmless input errors, or clamp the number to a valid value.

### 3.3 Tests

Cover both requests, both encoding directions, result codecs, and direct service entry points with `0`, `1`, `INT64_MAX`, `INT64_MAX + 1`, and `UINT64_MAX`, plus wrong JSON scalar kinds. A valid maximum with no matching attempt yields ordinary not-found. Use a valid non-nil run UUID for invalid-number tests so the intended check is exercised.

A daemon integration test must send each oversized raw JSON-RPC request, receive an invalid-parameters response, then successfully call `system.info` and another legitimate operation through the live daemon. Assert no fatal notification/shutdown. Retain a genuine persisted-corruption regression that still takes the fatal path.

## 4. Audit F2: safe client delivery across user code

### 4.1 Required public lifetime contract

A `ready`, `reply_received`, `failed`, or `call_failed` handler may destroy the ControlClient, including by destroying its owning parent. The borrowed raw RPC client, registry, device, and event loop still follow their existing outlives/affinity contracts. Destroying the wrapper suppresses further wrapper delivery and emits no new public outcomes from its destructor.

Every accepted call still has exactly one reply/failure unless the wrapper is destroyed. Reentrant `close()` and `cancel_call()` retain their existing semantics. No typed success/failure/ready signal may precede the accepting call's return for synchronous raw responses.

An emission already in progress retains core Signal's existing connection-snapshot semantics. The wrapper must stop subsequent outcome emissions after destruction; this is not a promise to cancel other slots or already-posted receiver deliveries belonging to the signal that was already emitted. Keep that distinction in lifetime tests.

### 4.2 Guard each public emission boundary

The existing weak lifetime/generation check at deferred-task entry is necessary but insufficient. A delivery method must not access `this`, `data`, `owner`, the timer, or member containers after a public emission unless it has first established that the private block still exists.

Take a **local weak token** before entering user code. Check that token before reading the generation or any other member afterward. Do not hold a new strong reference to the token across the callback: that would make `expired()` falsely suggest that the Object still exists after its destructor resets the member token.

Conceptual delivery-loop pattern:

```cpp
const auto lifetime = std::weak_ptr<int>{lifetime_guard};
const auto delivery_generation = generation;

// Remove this outcome from member state and finish internal bookkeeping first.
deliver_one(id, std::move(call));

if (lifetime.expired()) {
    return;
}
if (generation != delivery_generation) {
    return;
}
// Only here may the loop inspect member state again.
```

A helper is acceptable if it makes this ordering clearer and short-circuits before dereferencing freed data. It must not itself be a member call through a possibly dead pointer. A lifetime check does not justify dereferencing `data` before that check.

Audit and correct these paths together across the two client stages:

- `deliver_ready_outcomes()` after every `deliver_one()`, including ready and failed handshakes.
- The `on_terminated()` posted batch after `failed` and after each `call_failed`.
- `on_timeout()` between expired-call notifications.
- `close(true)` between failure notifications and before scheduling further deferred outcomes.
- Any equivalent post-emission member access in cancellation or decoded-result delivery.

Release correlation and update internal completion bookkeeping before each public callout, as today. Keep outgoing signal payloads in local owning values so destruction of the private block cannot invalidate arguments still borrowed during that emission. Do not keep map references/iterators across callouts.

A reentrant `close()` during terminal `failed` remains a no-op in the already-Failed phase; it must not accidentally discard all promised per-call failures while the wrapper remains alive. A close during ordinary ready/reply delivery settles the remaining pending calls itself; the obsolete delivery loop must then stop. Generation checks must respect these distinct cases rather than indiscriminately cancelling every batch.

This fix requires neither another pimpl nor changes to core Signal ownership. Keep the wrapper's existing receiver-aware raw-client/timer connections. Add Doxygen describing the supported handler destruction/reentrancy behavior.

### 4.3 Tests and sanitizer evidence

Extend `test/jobu-control-client-test.cpp` or split focused lifetime cases into a registered companion test. Destroy a heap-owned wrapper directly from each public signal with the raw client/device still alive. Cover:

- One handshake ready delivery and one ordinary reply; no second queued event is needed to reproduce the old loop read.
- Multiple queued replies; destruction on the first suppresses the rest.
- Handshake failure and terminal transport failure with pending calls.
- Multiple timeout failures and multiple explicit-close failures.
- Reentrant close/cancel without destruction; exact-once outcomes must remain unchanged.
- Receiver/parent destruction and destruction before deferred delivery.
- Continued use of the borrowed raw client where its own transport remains usable.

Execute the focused client suite under AddressSanitizer on Linux. Do not substitute an unexecuted test or a pre-delivery destruction case for the direct signal-handler destruction regression. If sanitizer tooling is unavailable, record that limitation and obtain the sanitizer run in the final verification environment before claiming F2 proven closed.

## 5. One-time job lifecycle

### 5.1 Public states and meaning

Extend `JobState`, preserving existing enum values by appending the three values after `Deleted`:

```cpp
enum class JobState : std::uint8_t {
    Active,
    Suspending,
    Suspended,
    Deleted,
    Succeeded,
    Failed,
    Cancelled,
};
```

Wire/storage spellings are lowercase: `succeeded`, `failed`, and `cancelled`. Keep `RunState` and `AttemptOutcome` unchanged. Add one shared predicate for terminal job states, with a name that cannot be confused with terminal run states, and use it in validation/management rather than scattered three-value comparisons.

| Job state | Definition-level meaning |
| --- | --- |
| Active | Unfinished definition; automatic work may execute subject to queue, time, capacity, and barrier rules. |
| Suspending | Suspension is draining running work. It is not a terminal result. |
| Suspended | Automatic execution is paused. Outstanding Scheduled/RetryWait work may still exist; existing manual bypass rules remain. |
| Succeeded | One-time definition has no nonterminal runs; the run that finished its last outstanding work succeeded. |
| Failed | One-time definition has no nonterminal runs; that final run failed or was permanently interrupted. |
| Cancelled | One-time definition has no nonterminal runs; that final run was cancelled. |
| Deleted | Soft-deleted definition retained for history; deletion takes precedence over execution outcomes. |

A terminal execution state is valid only with `OnceSchedule`. A recurring definition does not become terminal merely because one occurrence failed, succeeded, or was cancelled. Queues gain no new states.

Job state is durable metadata, not a computed field in `job.list`. Phase 9 retention may remove terminal run rows without changing a terminal job's state. There is no automatic purge. Terminal definitions are not reopened by any v1 operation, including Run Now; a possible future Clone operation is outside this plan.

### 5.2 Finish rule, including multiple runs

Nonterminal run states are exactly `Scheduled`, `Running`, and `RetryWait`.

After a run is made terminal, reconcile its owning definition in the **same transaction**:

1. Re-read the current job, not a stale dispatch snapshot.
2. If it is Deleted or recurring, make no execution-state transition.
3. If any run belonging to the job remains nonterminal, leave the job unfinished; normal suspension-drain rules may still apply.
4. Otherwise map the run just made terminal to the job result below, advance the current job revision once, and set `updated_at` using the transaction's transition time.

| Final run state | Job result |
| --- | --- |
| Succeeded | Succeeded |
| Failed | Failed |
| Interrupted, without an accepted recovery retry | Failed |
| Cancelled | Cancelled |
| Scheduled / Running / RetryWait | No terminal job transition |

**The result is the outcome of the run that drains the definition's outstanding work.** It is not “any failure ever recorded,” not the maximum wall-clock completion timestamp during live execution, and not the result of an intermediate retry attempt. This rule needs no new run-pointer or execution-generation column.

Examples defining otherwise ambiguous cases:

| Sequence | Required definition state |
| --- | --- |
| Initial one-time run fails, retry accepted, later succeeds | Unfinished throughout RetryWait; Succeeded after final success. |
| A manual Run Now finishes while the original future occurrence is still Scheduled | Unfinished; the original occurrence still exists. |
| Original future occurrence is cancelled while a manual run remains running | Unfinished until the manual run finishes; that final manual result determines the terminal state. |
| Manual run is cancelled while the original occurrence remains Scheduled | Unfinished; original schedule is unchanged. |
| Final successful completion arrives while the job is Suspending | Succeeded directly; do not commit an intermediate Suspended state. |
| A retry is accepted while the job is Suspending | It may become Suspended once no attempt is running; RetryWait is still unfinished work. |
| Work is soft-deleted by a management operation | Deleted remains authoritative; do not overwrite it with Cancelled. |

There is no new `Running` job-definition state. Run/attempt inspection remains the source of detailed execution state and outcome. `--state active` intentionally does not include Suspended or Suspending definitions.

### 5.3 Revisions and transaction ordering

Every actual durable job-state change advances `JobRevision` exactly once using the existing positive signed-64-bit bound and compare-and-update pattern. Completion must use the current revision; a run's snapshotted `job_revision` need not equal it after suspension/resume or other permitted control changes. Never rewrite historical run snapshots to match the new job revision.

Reconcile terminal job state **before** completing drained job suspension. The latter should find a terminal job and do nothing. Queue suspension can drain independently. A retry branch does not terminalize the job. A no-op reconciliation does not increment revision.

If revision exhaustion, an unexpected affected-row count, or another storage failure prevents the job transition, roll back the whole completion/cancellation/recovery unit. Never commit a terminal run while knowingly leaving its final job-state update uncommitted. Preserve the existing fatal storage policy and defer failure observers until transaction/query cleanup.

Clients with an old revision must get the existing revision-conflict result for revision-checked operations. An idempotent replay still returns its stored historical response; a caller uses `job.get` to observe the current state. Do not rewrite stored create/Run Now idempotency results merely because the definition later finishes.

## 6. Shared storage and runtime implementation

### 6.1 Private lifecycle owner

Add a small generic private component, for example `JobLifecycleRepository` in `job_lifecycle_priv.hpp/.cpp`, backed by the borrowed database and immutable attribute registry. It is a normal private repository object, not an Object and not another service.

Suggested operation shapes:

```cpp
// All mutating calls below require a caller-owned transaction.
auto finish_after_terminal_run(Uuid const& run_id, UtcTimePoint transition_time)
    -> Result<bool, Error>;

auto validate_job_lifecycle(JobDefinition const& job)
    -> Result<void, Error>;
```

Use fully qualified actual project types in the implementation. A returned `true` means one job changed state; `false` means a valid no-op. The component begins/commits no transaction, emits no signal, invokes no executor, and retains no Query between calls.

`finish_after_terminal_run` must verify that the supplied run exists, is terminal with valid terminal fields, and belongs to the job being updated. It uses an `EXISTS`/bounded relationship query for outstanding work, then `JobRepository::set_state` with the current state/revision. Do not load all attempt/output histories or all run payloads to count outstanding work.

If the current job is already terminal, validate that no nonterminal work remains and return a no-op; never replace its saved outcome or increment its revision by replaying reconciliation for an older terminal run. Existing stale-completion checks remain in place before runtime writes.

`validate_job_lifecycle` checks current-format relationship invariants separately from ordinary client-input validation. It does not have a legacy-repair mode. A terminal definition remains authoritative even when its history has been removed; an unfinished one-time definition with zero nonterminal runs is an invariant failure. Do not add a repair API that guesses a finished outcome from historical rows.

Extract a small common job/run relationship predicate or query helper if needed so scheduler discovery, dispatch recheck, cancellation, and recovery do not acquire inconsistent rules. Avoid duplicating the entire existing repository.

### 6.2 Scheduler completion and cancellation

Integrate the lifecycle write into `process_completion` in `scheduler_core_priv.cpp`, after terminal run persistence and before suspension drain/commit. The retry branch retains current capacity/retry/barrier handling. Completion signals/accounting only reflect committed state, as in the existing implementation.

Integrate the same helper into immediate cancellation of Scheduled/RetryWait runs. A cancellation request for a Running run does not mark the job Cancelled early; its accepted cancellation outcome is finalized through the normal completion transaction.

Do not make scheduler bookkeeping depend on ManagementService observing its own mutation signal. The durable transition belongs to the current transaction. Keep existing rescan/accounting behavior for automatic completion.

### 6.3 Manual-run relationships

Current scheduler and recovery checks require one nonterminal schedule-owned sibling for every nonterminal manual run. That rule must become schedule-kind aware:

| Current definition | Nonterminal schedule-owned runs | Nonterminal manual runs | Validity |
| --- | ---: | ---: | --- |
| Recurring | 1 | 0 or 1 | Existing valid relationship; manual barrier blocks automatic dispatch. |
| Recurring | 0 | 0 | Existing pre-admission missing-successor repair candidate only. |
| Recurring | 0 | 1 | Invalid; do not relax the recurring barrier. |
| One-time, unfinished | 1 | 0 or 1 | Existing automatic occurrence, optionally with a manual barrier. |
| One-time, unfinished | 0 | 1 | Valid only as already-accepted manual work remaining after the original occurrence became terminal, e.g. cancellation; this shape does not authorize creating new manual work. |
| One-time, unfinished | 0 | 0 | Invalid at every committed boundary; current-format completion/cancellation updates the final job state atomically. |
| One-time, terminal | 0 | 0 | Valid, including when old run history has been retained away. |
| One-time, terminal | More than 0 | Any | Invalid current durable state. |
| One-time, terminal | Any | More than 0 | Invalid current durable state. |
| Any live definition | More than 1 | Any | Invalid. |
| Any live definition | Any | More than 1 | Invalid. |

Deleted owners retain their existing shutdown/deletion validation rules; this table does not authorize executing their work. Nonterminal rows must retain valid ownership, origin, queue, snapshot, and attempt relationships. The zero-scheduled-sibling allowance is specific to a current one-time definition, not a blanket relaxation for manual runs.

Update `list_manual_barriers`, dispatch-context rechecks, scheduler candidate validation, and `RecoveryRepository::validate_barriers` together with tests. Join/decode the current schedule kind where the existing projection contains only job state. Already-accepted one-time manual work may keep the existing job barrier bookkeeping after its original occurrence is cancelled, even though there is no automatic sibling left to block. Run Now must still require the original future Scheduled occurrence when accepting a new request (§9.2); this validation allowance must not become a second creation path.

## 7. Fresh database format; remove all schema upgrades

### 7.1 One supported format

There are no installed databases to migrate. Maintain **one** current application schema definition. Preserve the existing tables, columns, indexes, and foreign-key relationships in that definition, including the three history indexes introduced during Phase 8. Change `jobu_jobs` constraints to admit the three new state strings and reject terminal execution states on recurring definitions:

```sql
state TEXT NOT NULL CHECK (
    typeof(state) = 'text'
    AND state IN ('active', 'suspending', 'suspended', 'deleted',
                  'succeeded', 'failed', 'cancelled')
),
-- Other existing columns/constraints remain in their existing form.
CHECK (
    state NOT IN ('succeeded', 'failed', 'cancelled')
    OR schedule_kind = 'once'
)
```

Preserve the existing Deleted/deleted_at relationship; Succeeded/Failed/Cancelled have `deleted_at_us IS NULL`. Add no execution timestamp, result blob, foreign run pointer, or queue state for this feature.

Set `current_schema_version` to 3 only to distinguish the fresh format from old test databases. Do not reset it to 1, accept an old marker as current, or advise changing the marker manually. The format number creates no promise of migration support. A deployment using an old test database must be deliberately pointed at a fresh database; startup must never delete, clear, or replace it automatically.

### 7.2 Remove the upgrade implementation

Delete the existing v1→v2 branch and its supporting code, not merely its invocation. In the audited source this includes `schema_v1_object_manifest()`, `version_one_object_count`, `FailurePhase::Upgrade`, the `upgraded` local/status field, and upgrade-only error construction. Remove any now-unused legacy fixture builders and upgrade-only fault cases. Retain creation fault injection and normal schema validation; narrow observer comments to their remaining creation purpose.

Do not introduce the initial closure draft's v1/v2→v3 conversion, historic manifests, table rebuild, copy/rename/drop sequence, foreign-key disable/restore logic, legacy outcome selection, or domain backfill. No SQL migration files or migration runner are needed.

The resulting public status shape is:

```cpp
struct SchemaStatus {
    std::uint32_t version{0};
    bool          created{false};
};
```

`ensure_schema(Database&)` remains the only public application-schema entry point. Its Doxygen says **creates or validates**, documents that the returned version is current, and explains rejection of unsupported formats. Update all callers, aggregate initializers, examples, tests, and current documentation that mention `upgraded` or automatic index upgrades. Preserve prior verification records as historical evidence; append a note that migration support was intentionally removed by this closure instead of rewriting old results.

### 7.3 Creation, validation, and rejection

Keep schema ownership in `jobu-sqlite`, with no SQLite-specific API added to generic Database or JobU repositories. The connection must be open, idle, on its owner thread, and exclusively owned under the existing startup contract. Use the existing immediate RAII transaction; keep foreign-key enforcement enabled throughout.

| Database on entry | Required action |
| --- | --- |
| Unmarked, empty | Create every current object and marker atomically; validate; commit; return `created=true`. |
| Current marker 3, supported schema | Validate without repair; commit; return `created=false`. |
| Older marker, including 1 or 2 | Return Unsupported / `jobu.schema.unsupported_version` with a safe message directing the operator to use a fresh database; do not modify schema, marker, or user rows. |
| Newer marker | Preserve Unsupported / `jobu.schema.newer_database`; do not modify it. |
| Malformed marker or unsupported current schema | Preserve `jobu.schema.invalid`; do not attempt repair. |
| Unmarked, nonempty | Preserve `jobu.schema.database_not_empty`; do not adopt existing objects. |

Validate required objects, columns, history indexes, and foreign keys using existing helpers. Also verify that the current `jobu_jobs` table contains the supported state constraints; a marker alone is insufficient because the column layout did not change. Use the single fresh-table DDL as the expected form and a focused comparison of the stored application-owned CREATE TABLE definition, accounting only for SQLite's known fresh-creation normalization. Do not retain historical DDL variants or implement a general SQL parser. Test that a current marker placed over an incompatible job table is rejected.

Preserve the existing creation/validation error cleanup rules: queries finish, failed creation rolls back, rollback failure poisons the connection, and an uncertain commit prevents startup until reopen/validation. There is no foreign-key restoration phase because this design never disables enforcement. No listener or executor is admitted after a schema error.

### 7.4 Focused schema tests

Cover fresh creation/reopen, exact accepted/rejected states, terminal-once constraint, unchanged indexes/FKs, creation/marker/validation/commit failures, and active-transaction misuse using existing test seams. Verify foreign keys remain enabled on successful return.

For unsupported-version tests, start with a controlled fixture, set its marker to 1 or 2, and assert rejection without logical schema/data/marker mutation. A genuine populated historical manifest is unnecessary because no historical format is opened or converted. Test a newer marker, malformed marker, unmarked nonempty database, and incompatible current table separately. Remove tests whose sole purpose was proving successful historical upgrades; do not replace them with an equally large historical-schema framework.

Fresh-schema success still precedes ordinary startup recovery. Recovery handles interrupted current-format execution, not conversion of old installations.

## 8. Current-format recovery and lifecycle invariants

### 8.1 Reconcile interrupted work in its existing transaction

Preserve the existing validate-before-repair flow, bounded keyset pages, per-unit RAII transaction, cancellation handling, recurring-successor repair, suspension drain, and final validation. Add the one-time lifecycle update to the existing interrupted-run unit; do not introduce a new finished-history scan.

For each interrupted run:

1. Complete its Interrupted attempt and lost-capture metadata as today.
2. If retry policy accepts a retry, persist RetryWait and keep the definition unfinished; do not briefly mark it Failed.
3. If the run becomes terminal Interrupted, call `finish_after_terminal_run` in the same transaction. It maps a one-time definition to Failed only when no nonterminal work remains. Deleted/recurring definitions retain their respective existing semantics.
4. Apply recurrence and queue/job suspension handling in the order required by §5. Terminalization precedes drained job suspension, so a Suspending definition goes directly to Failed where appropriate.
5. Commit the whole unit before counting or reporting its transitions.

This also covers a previously accepted manual run left running after its original future occurrence was cancelled. The definition is still unfinished on entry and becomes Failed only when recovery terminates its last outstanding work. It must never receive a replacement one-time occurrence.

### 8.2 No legacy reconstruction

There is no `repair_finished_once_job`, latest-terminal-result query, historical timestamp/UUID tie-break, old-state backfill, or allow-legacy-repair flag in the new lifecycle helper. Schema validation rejects old database formats before recovery starts.

For a supported current-format database, an unfinished one-time definition with zero nonterminal runs is inconsistent **even if terminal history exists**. Both initial and final lifecycle validation reject it. The new completion/cancellation/recovery paths must have committed the terminal definition together with the final run; recovery must not conceal a broken write path by guessing a historical outcome.

Both validation passes also reject terminal recurring definitions, terminal definitions with nonterminal work, invalid manual/scheduled multiplicities, and existing owner/attempt/output corruption. A terminal one-time definition without retained run history remains valid: its persisted state is authoritative and must not change during recovery.

Keep the pre-existing initial/final distinctions for legitimate recovery work: Running attempts may need interruption processing, recurring definitions may need a successor, and Suspending owners may need to drain. Removing legacy one-time backfill does not remove those established repairs. Use the existing job-owner validation traversal and bounded relationship queries; do not add another unbounded history scan.

### 8.3 Reporting, cleanup, and repeatability

Add `RecoveryReport::finished_jobs`, documented narrowly as the number of one-time definitions made terminal by interrupted-run processing during this invocation. Include it in checked accumulation. Do not count a direct Suspending-to-Failed transition as a suspended job too. A second recovery over unchanged data reports zero newly finished jobs and leaves their state, revision, and timestamp unchanged.

Use fixed safe invariant reasons, with no payload or secret bytes in errors. Finish queries before mutation, retain existing bounded page traversal, and count only acknowledged committed units. Immutable run snapshots and idempotent responses are not rewritten.

Prior recovery units may have committed before a later error. Preserve that existing contract; any error prevents listener/executor admission. Prove safe repeat recovery after partial progress and lost commit acknowledgement by reopening and validating current durable state. No migration/backfill resumption mechanism is added.

## 9. Management operations and Run Now eligibility

### 9.1 Operation matrix

| Operation | One-time terminal definition |
| --- | --- |
| `job.get` / explicitly filtered `job.list` | Return the terminal definition normally. |
| `job.update` | With a current revision, reject with `jobu.job.state_conflict`; never silently revive or reschedule it. Preserve existing revision-conflict precedence for stale revisions. |
| `job.suspend` / `job.resume` | Return ordinary state conflict. Resume is not a rerun command. |
| `job.move` | Continue requiring Suspended. Terminal definitions are not implicitly suspended or moved. |
| `job.delete` | Allow with the current expected revision and zero nonterminal work; soft-delete normally. |
| `job.run_now` | Reject new work with ordinary Conflict / `jobu.run.manual_conflict`; stored idempotent replay is described below. |
| Queue suspension/deletion | Existing semantics; terminal definitions do not count as running work. Queue deletion still soft-deletes their definitions when allowed. |

Keep current secret-reference bookkeeping unchanged until explicit definition deletion. Terminalization does not implicitly remove references or change the secret-deletion contract. Existing deletion logic removes definition references at its transaction boundary; snapshot protections remain in place. This preserves the existing reference policy without granting terminal definitions execution eligibility.

For an **already accepted** `job suspend --wait`, a subsequent `job.get` showing Succeeded, Failed, or Cancelled means the job has drained and the wait is complete. Return the actual terminal definition and a successful wait exit status; do not relabel it Suspended. A Failed job outcome does not mean the administrative wait operation failed. A fresh suspend call made after terminalization still receives the ordinary state conflict above. Preserve stable-ID checks, the single overall deadline, and read-only polling; Active after a concurrent resume remains a state-changed conflict. Do not change queue waits or `run cancel --wait` semantics. Update help/deadline text so it describes draining rather than promising that every successful job wait ends in Suspended.

### 9.2 Run Now tests unfinished definitions only

Run Now lets an operator test an eligible one-time or recurring definition before its original future occurrence. It does not reopen Succeeded, Failed, or Cancelled definitions. Deleted definitions retain their existing rejection. Clone, including cloning a terminal definition with changed attributes, is outside v1; add no Clone API or placeholder implementation.

Keep the current `run_now_impl()` acceptance path, which requires a future Scheduled schedule-owned row. Preserve its busy/manual-barrier checks, queue rules, and existing suspension bypass semantics for otherwise eligible unfinished definitions. In this contract, “active definition” means one that can still produce eligible work; it does not silently remove the existing ability to manually test a Suspended/Suspending definition when all other Run Now preconditions pass.

Required order:

1. Validate/canonicalize the request and begin the existing transaction.
2. Check an existing idempotency record **first**, using the existing scope/request validation and saved-result decoding.
3. For a new request, load the current definition. Preserve not-found/deleted handling, then reject any terminal execution state with `manual_run_conflict()` (`jobu.run.manual_conflict`, Conflict).
4. Continue the existing unfinished-definition queue, future schedule-owned occurrence, busy, and manual-barrier checks. Do not add an alternative path when the original occurrence is absent or terminal.
5. If eligible, create the existing single manual run with the current definition revision and immutable payload/attribute snapshot, store optional idempotency, commit, and request the existing rescan. Do not reactivate the definition, change its revision, or recreate its original occurrence.

A rejected terminal request allocates no run identifier, changes no rows/revisions/idempotency records, emits no mutation notification, and starts no executor. It is an ordinary client conflict, not a fatal storage error. Normal recovery/scheduler validation remains responsible for detecting contradictory persisted terminal/live-work relationships.

An exact replay of a **previously accepted** idempotency key may return its stored historical Run Now response after the definition has since become terminal. That is response replay, not acceptance of new execution: it creates no run, advances no revision, emits no rescan/mutation signal, and resolves no secrets. Preserve request-mismatch errors and replay validation. A fresh key, no key, or an expired/pruned key is a new request and must be rejected for a terminal definition.

Tests cover Succeeded, Failed, and Cancelled separately, with/without a fresh key; no mutation or UUID consumption on rejection; replay of an earlier accepted request after final completion; and unmodified accepted Run Now for a still-unfinished one-time or recurring definition. Include the weekend-style example: manual test completes while the original future occurrence remains Scheduled, so the definition remains unfinished and that original occurrence can later run.

## 10. Listing defaults and command-line contract

### 10.1 Keep the raw API filter semantics

`JobListRequest::state` remains optional. No state in a raw `job.list` request continues meaning all matching nondeleted definitions unless `include_deleted` is set. Extend accepted state strings to the new values. This avoids a new “all states” wire field and preserves the exact `--request-file` contract.

The **ordinary CLI builder** supplies `state=Active` by default. Do not implement the default by filtering received pages: filtering must happen in the repository query before limit/lookahead/cursor processing.

### 10.2 CLI option table

Add `--state STATE` and `--all` to job-list metadata/help and parsing. `--state` accepts one lowercase state; repeated occurrences are rejected. All existing queue/page options still compose normally.

| Ordinary CLI options | Encoded state | Encoded include_deleted |
| --- | --- | --- |
| none | `active` | false |
| `--state active/suspending/suspended/succeeded/failed/cancelled` | chosen state | false |
| `--state deleted` | `deleted` | true, because deletion was explicitly selected |
| `--all` | omitted | false |
| `--include-deleted` with no explicit state | omitted | true; preserves this existing option's broad listing behavior |
| `--all --include-deleted` | omitted | true |
| `--state STATE --include-deleted` | chosen state | true; also permits matching definitions in an explicitly selected deleted queue under existing API rules |
| `--all --state STATE` | local usage error | no RPC |

A raw request file is decoded exactly as supplied, without adding the CLI Active default. Keep request-file/options mutual exclusion. Explain this distinction in help and protocol/CLI documentation.

Retain existing page size, response budget, UUID ordering, and `after_id` semantics. Callers repeating a page must retain the same filter options. All-state pages remain bounded. `job.get` remains the direct way to inspect a known finished definition.

Examples:

```sh
jobuctl --socket /path/to/jobud.sock job list
jobuctl --socket /path/to/jobud.sock job list --state succeeded
jobuctl --socket /path/to/jobud.sock job list --state failed
jobuctl --socket /path/to/jobud.sock job list --state cancelled
jobuctl --socket /path/to/jobud.sock job list --state suspended
jobuctl --socket /path/to/jobud.sock job list --all
jobuctl --socket /path/to/jobud.sock job list --all --include-deleted
```

Update the human renderer's state switch, JSON result codecs, typed client decoders, and terminal/cron validation. Help remains local and needs no socket. No additional CLI rendering of attempts inside job-list rows is required.

## 11. CLI secret-reference options

### 11.1 Exact syntax and generated payload

Add repeatable options to CLI `job create` and its `job add` alias:

| Option | Meaning |
| --- | --- |
| `--arg-secret SECRET_NAME` | Append one complete argument reference at this exact argv position. |
| `--env-secret NAME=SECRET_NAME` | Assign one environment key to a complete secret reference. |

Support the parser's ordinary attached-value forms as well. For example:

```sh
jobuctl --socket /path/to/jobud.sock job create \
  --queue-name default --at now \
  --command /usr/local/bin/report \
  --arg=--token --arg-secret reports.token \
  --arg=--mode --arg=daily \
  --env MODE=daily \
  --env-secret REPORT_PASSWORD=reports.password
```

Relevant payload fragment:

```json
{
  "command": "/usr/local/bin/report",
  "arguments": ["--token", {"secret":"reports.token"}, "--mode", "daily"],
  "environment": {
    "MODE": "daily",
    "REPORT_PASSWORD": {"secret":"reports.password"}
  }
}
```

The CLI supplies **names**, never fetches secret values, never substitutes locally, and performs no extra RPC lookup before create. The server retains its transactional existence/reference validation. Per-attempt resolution, rotation, retry, binary/text restrictions, idempotency canonicalization, and secrecy boundaries remain unchanged.

### 11.2 Parser implementation

Build literal arguments and secret arguments in the same sequential argument loop. Do not collect separate lists and concatenate them later. Both count toward the existing argument limit; references also count toward the shared template-reference cap.

Build `--env`, `--env-secret`, and `--unset-env` in one environment map. A key appearing more than once across any of these options is a local error, in either ordering; there is no last-one-wins override. Split a secret environment assignment at its first `=`; reject empty/malformed variable names and empty/invalid secret names. Existing literal empty values and null removals retain their semantics.

Reuse `detail::is_valid_secret_name` and the existing CLI template validator, including the restrictions on `PATH`, `JOBU_*`, executable/working-directory fields, text size, UTF-8, and NUL. No secret-reference interpolation, prefix/suffix syntax, or implicit treatment of strings containing `secret` is added. Supplying these options for an HTTP job is a local usage error through the existing family checks.

Wire metadata, lexer/parser classification, `is_cli_creation_option`, payload construction, and help together. Preserve the existing special treatment of dash-leading literal `--arg` values. In particular, adding a newly recognized option must not reinterpret an old command whose literal argument is `--arg-secret` or `--env-secret`; extend the existing compatibility allowlist and test it. The unambiguous spelling for option-like literal values remains `--arg=VALUE`.

Keep existing request-file mutual exclusion; do not silently merge these options into a supplied request document. `job update` payload replacement remains available through its existing request-file route; this closure adds no separate argv-editing language.

### 11.3 Help, errors, and tests

Help describes names, ordered whole-value replacement, environment uniqueness, and examples. `job create --help` and `job add --help` still perform no file/stdin read and no connection. Preserve the existing distinction between lexical option errors and help bypassing semantic command construction.

Test mixed ordering, multiple references to the same secret name, attached/separate values, argument limits, reference limits, missing values, invalid secret names, malformed assignments, duplicate environment keys across all option pairs, forbidden environment references, HTTP-family rejection, request-file conflicts, aliases, and dash-leading literal compatibility.

A real daemon/CLI test must create a job using only these ordinary flags, verify retained templates contain the reference objects, rotate a referenced secret before a held attempt starts, and prove execution observes the current value. Also cover resolution on a later retry using the existing deterministic integration fixture. Avoid putting resolved values into diagnostic text or public job metadata. Captured child output retains its existing documented ability to contain what the child prints; this feature adds no output redaction mechanism.

## 12. Documentation, tests, and verification matrix

Update public Doxygen and relevant documents during their implementation stages. Final consolidation includes:

- `docs/protocol/types.md`: seven job states, terminal/once rule, attempt-number range, and durable-vs-snapshot meaning.
- `docs/protocol/methods/job.md`: state filters, terminal management matrix, Run Now rejection for terminal definitions, and observational idempotent replay.
- `docs/protocol/methods/attempt.md` and `errors.md`: validation bounds and ordinary error behavior.
- `docs/jobuctl.md`: Active default, `--state`, `--all`, `--include-deleted`, request-file distinction, secret flags and help.
- `docs/secrets.md`: ordinary CLI construction and unchanged reference ownership until explicit definition deletion.
- `docs/cpp-client.md`: safe destruction/reentrancy and the borrowed-dependency lifetime contract.
- `docs/planning/README.md`: closure pending until the final gate passes, then exact closure/evidence links.
- Existing schema/recovery documentation and the appended verification record: fresh-format marker 3, removal of upgrades, older-format rejection, current-format interruption recovery, source identities, actual commands/results, and skips. Update any startup/troubleshooting text promising automatic upgrade; keep old verification evidence historical.

Add/extend behavior-focused tests, reusing existing fixtures and `test/support/fault_database_driver.*` rather than introducing another fault framework:

| Area | Required coverage |
| --- | --- |
| F1 | Numeric boundaries through codecs, direct service, typed submission, raw RPC; healthy daemon survives; real corruption remains fatal. |
| F2 | Destruction at every public delivery boundary; multiple pending outcomes; reentrant close/cancel; ASan execution. |
| Domain/codecs | All seven states; invalid enum/string; terminal recurring rejection; unchanged run/attempt enums and historical replay snapshots. |
| Fresh schema | One current manifest; constraints/indexes/marker; terminal once accepted, terminal cron/unknown states rejected; reopen. |
| Format rejection | Older/newer/malformed markers and incompatible current schema rejected without repair or logical mutation; no legacy upgrade support remains. |
| Schema faults | Creation/marker/validation/commit/rollback failures; foreign keys remain enabled; no unsafe readiness and correct uncertain-commit handling. |
| Lifecycle | Success, terminal failure, retry then success/failure, timeout, preparation failure, pending/running cancellation, suspension drain, deleted precedence. |
| Manual work | Future-occurrence tests for unfinished once/cron definitions; cancellation of original while accepted manual work remains; terminal fresh-request rejection; saved-result replay without new work. |
| Recovery | FailInterrupted/RetryInterrupted, retry stays unfinished, atomic terminalization, repeated invocation, terminal owner with no history, unfinished owner with no work rejected, impossible relationships, partial progress/restart. |
| Management | Current/stale revisions; exhausted revisions; terminal get/delete; refused update/suspend/resume/move; accepted suspend wait ending in each terminal state; queue deletion interactions. |
| Listing | Default Active; each explicit state; all/deleted combinations; exact request-file semantics; filter-before-pagination/byte-budget. |
| Secret options | Ordered payloads, uniqueness/errors, help/alias/lexer behavior, real execution/rotation/retry, no extra secret-value access. |

Do not replace assertions of old behavior indiscriminately. For each changed expected state/revision, establish why the revised contract applies; recurring jobs, retry waits, historical snapshots, and idempotent responses often must retain their old values.

## 13. Individually reviewable implementation stages

Stages **8.29–8.44** deliver and verify the Linux closure. **8.45** is optional native macOS verification. **8.46** is the final clean Linux gate and closure decision. Every implementation stage includes its corresponding Doxygen/comments, relevant tests, and changed-file diagnostics; the final documentation stage is an audit, not permission to defer all documentation.

### Stage 8.29 — Attempt-number domain and history validation <- NEXT

**Implement:** §3 common bound/predicate, request encode/decode validation, direct service guards, and relevant result validation. Leave storage corruption classification intact.

**Verify:** Focused codec/service/client pre-write tests for all boundaries and wrong scalar types; no failed signal or poisoned/stopped service on bad input; normal missing maximum is not-found; public-header checks.

**Exit:** F1 cannot reach a repository conversion through any public history-request entry.

### Stage 8.30 — F1 daemon regression and protocol contract

**Implement:** Raw RPC integration cases for both history methods and range documentation; fix only a demonstrated gap in Stage 8.29.

**Verify:** Oversized requests receive ordinary invalid params, then `system.info` and a valid operation succeed. Run existing genuine corruption/fatal-admission tests.

**Exit:** Executed daemon evidence distinguishes invalid client input from fatal durable corruption.

### Stage 8.31 — Client ready/reply delivery lifetime

**Implement:** Post-callout weak lifetime/generation checks for deferred outcome delivery and handshake/decoded-result paths, with owning local payloads.

**Verify:** Direct destruction from ready/reply/handshake failure, one and multiple outcomes, destruction before tasks, close/cancel reentrancy, and focused ASan execution.

**Exit:** The primary F2 loop cannot touch freed private state after user code returns.

### Stage 8.32 — Client failure-batch lifetime and exactly-once audit

**Implement:** Guard terminal failure, timeout, and close batches; audit every public emission and document the complete lifetime contract.

**Verify:** Destruction from failed/call_failed, multiple pending calls, close during terminal failed without destruction, no duplicate/missing outcomes for live wrappers, borrowed raw-client reuse, ASan rerun.

**Exit:** All F2 paths are covered without a new allocation model or relaxed delivery guarantees.

### Stage 8.33 — Terminal job-state domain and codecs

**Implement:** Append enum values, shared terminal-job predicate, storage/wire mappings, terminal/once validation, human rendering, and public contract comments. Do not produce new persisted states before schema support.

**Verify:** Header/codec/domain tests for all states, invalid combinations, and unchanged historical result decoding; update only justified enum switches.

**Exit:** Public/domain representations are ready for v3 and runtime transitions.

### Stage 8.34 — Fresh schema and removal of upgrade support

**Implement:** §7 single current manifest, new job-state constraints, fresh marker 3, removal of the existing v1→v2 branch and all upgrade-only helpers/status fields/tests. Add explicit older-format rejection and current job-table validation; update schema/startup documentation in this stage.

**Verify:** Fresh/create/reopen; state constraints, indexes, columns, FKs; older/newer/malformed markers and unmarked nonempty rejection without logical mutation; incompatible table with current marker rejected; normal creation/commit/rollback faults. No migration or populated historical-format fixtures are required.

**Exit:** Only fresh creation and current-format validation are supported; existing test databases must be recreated deliberately, and no successful path disables foreign keys.

### Stage 8.35 — Shared one-time lifecycle repository and relationship rules

**Implement:** §6 private reconciliation/validation component; revision-checked writes; schedule-kind-aware manual relationship checks in scheduler/recovery projections. No service orchestration yet.

**Verify:** Direct repository fixtures for draining/non-draining outcomes, deleted/recurring no-ops, current-vs-snapshot revisions, invalid rows, revision exhaustion, zero-sibling once versus invalid zero-sibling cron, and transaction rollback.

**Exit:** One reusable implementation owns the terminal-state rule and relationship vocabulary.

### Stage 8.36 — Normal completion and retry integration

**Implement:** Add lifecycle reconciliation to terminal scheduler completion before suspension drain/commit; preserve retry and accounting behavior.

**Verify:** CLI/HTTP-shaped fake completions, success, exhausted/terminal failure, retry then terminal, preparation failure, queue/job suspension, deleted precedence, stale completion, write/commit/rollback faults.

**Exit:** Normal execution commits run and final job state atomically; retries never finish the job early.

### Stage 8.37 — Cancellation and remaining manual work

**Implement:** Pending cancellation reconciliation and complete running-cancellation coverage; apply the approved one-time manual relationship rules at every relevant recheck.

**Verify:** Scheduled/RetryWait cancellation, running cancellation acknowledgement, original cancelled while manual remains, manual cancelled while original remains, final manual outcome, suspension and faults.

**Exit:** Cancelling one run never finishes a definition while another nonterminal run remains or creates a false barrier-corruption failure.

### Stage 8.38 — Current-format interruption recovery

**Implement:** §8 common lifecycle call inside interrupted-run transactions, initial/final lifecycle validation, `finished_jobs` reporting, and repeatability. Preserve existing recurring-successor and suspension repairs; add no legacy backfill.

**Verify:** FailInterrupted/RetryInterrupted, remaining accepted manual work after original cancellation, terminal state without history, unfinished one-time owner with no work rejected, retry preservation, partial progress/cancellation, revision exhaustion, faults, and restart. Repeated recovery must not change terminal state/revision or count it again.

**Exit:** Runtime admission requires valid current-format relationships; interruption finalizes a one-time job atomically when it drains the last work.

### Stage 8.39 — Terminal management operation rules

**Implement:** §9 operation matrix, terminal soft deletion, preserved secret-reference ownership, revision precedence, resume/suspend/update/move conflicts, explicit terminal Run Now rejection after idempotency replay, and terminal-aware observation of an already-accepted CLI suspend wait.

**Verify:** Every matrix row, current/stale/exhausted revisions, get/list decoding, queue deletion, secret deletion protection, unchanged recurring operations, terminal Run Now with fresh/no key rejected without mutation or UUID consumption, accepted-key replay after terminalization, and a single accepted suspend followed only by reads until terminal completion or the original deadline.

**Exit:** Administrative actions cannot accidentally revive or erase the meaning of terminal states.

### Stage 8.40 — Active-default CLI listing and explicit filters

**Implement:** §10 CLI metadata/builders/help and all/deleted combination rules; ensure repository filtering precedes pagination. Preserve raw API and request-file defaults.

**Verify:** Mixed state dataset across multiple pages and byte-limited pages, all accepted/rejected combinations, deleted queue selection, JSON/human rendering, local help, raw `{}` versus ordinary CLI default, typed client decoding.

**Exit:** Ordinary job listing shows only Active definitions, with all other states explicitly reachable.

### Stage 8.41 — Secret-reference flags and parser parity

**Implement:** §11 metadata, same-pass argv/environment builders, shared validation, family/request-file checks, alias/help, and dash-leading literal compatibility.

**Verify:** Exact generated JSON and order, duplicate-key matrix, invalid names/values, limits, reserved/PATH restrictions, HTTP rejection, old literal option tokens, create/add help without I/O.

**Exit:** CLI jobs can use existing secret templates through ordinary flags without a JSON request file.

### Stage 8.42 — Secret flags through execution, rotation, and retries

**Implement:** Focused real daemon/CLI fixtures for the new construction route, reusing existing gate/runner fixtures and documentation examples.

**Verify:** Stored references remain symbolic; argument/environment positions are correct; rotation before dispatch and before retry is observed; idempotent creation and eligible unfinished-job Run Now remain correct; no resolved values appear in generated metadata/errors.

**Exit:** The convenience flags preserve all existing per-attempt secret guarantees.

### Stage 8.43 — Assembled Linux closure and failure integration

**Implement:** Fill demonstrated integration gaps only. Exercise fresh startup and current-format restart/recovery, real create/execute/list/cancel workflows, eligible Run Now and terminal rejection, F1 survival, and unchanged fatal/shutdown gates.

**Verify:** Full normal SQLite-enabled Linux suite on the assembled source; record exact count and internal skips. Include current-format restart with mixed once/cron, suspended queues, references, and idempotency, plus startup rejection of an old format. Confirm no executor/listener activity after schema or recovery failure.

**Exit:** Linux functional and failure integration is complete before the final contract audit.

### Stage 8.44 — Public-contract and documentation audit

**Implement:** Complete §12 consistency work, fixtures, roadmap closure-pending status, and verification-record entries. Fix only concrete discrepancies.

**Verify:** Production-codec JSON fixtures, method/help/client inventory, command examples, relative links/anchors, current state/error tables, header checks, clean changed-file diagnostics, and no accidental socket or Phase 9 scope.

**Exit:** Documentation describes the implemented behavior and the evidence still distinguishes baseline from closure results.

### Stage 8.45 — Optional native macOS closure verification

**Implement:** No new platform backend. Run native builds and relevant schema/lifecycle/recovery/client/CLI cases; correct only demonstrated shared-source portability defects in the approved scope.

**Verify:** Prefer the full registered native suite after focused checks. Record exact source/toolchain/commands/results and privilege skips. Obtain native sanitizer evidence only where supported; Linux ASan remains the required F2 evidence.

**Exit:** Record an executed native result or explicit approved/deferred platform status. The old Phase 8 macOS pass remains baseline evidence and must not be relabeled as validation of these new changes. An explicitly approved optional skip is not a Linux closure blocker.

### Stage 8.46 — Final clean Linux verification and closure

**Implement:** Evidence/documentation closure, except a demonstrated failure requires its own reviewed correction. Refresh the final merged source and use a genuinely fresh build directory.

**Verify:** Full configure/build/CTest; exact source identity and SQLite-enabled targets; public-header/example builds; F1 daemon and F2 ASan evidence; all-state/list/help/docs consistency; diagnostics under AGENTS. Record root-only/internal skips accurately. If final documentation changes only evidence/status, record that fact and the code-equivalent tested parent.

**Exit:** Every acceptance criterion below passes, the record is appended truthfully, and Phase 8 may be marked closed before Phase 9 planning.

## 14. Build commands and evidence requirements

Normal focused work uses the repository's current build configuration and relevant test targets. Final clean verification uses, for example:

```sh
cmake -S . -B .bld-phase8-closure-final -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld-phase8-closure-final
ctest --test-dir .bld-phase8-closure-final/test --output-on-failure
```

Verify the actual configured SQLite driver/application targets are enabled. Do not infer that from the command line or reuse an old final build cache.

A separate focused Linux ASan build can use:

```sh
cmake -S . -B .bld-phase8-closure-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address'
cmake --build .bld-phase8-closure-asan --target jobu-control-client-test
ctest --test-dir .bld-phase8-closure-asan/test \
  -R '^jobu-control-client-test$' --output-on-failure
```

Adjust the focused target selection if lifetime cases are split into a companion executable; instrument the linked project libraries too. Record compiler/runtime limitations rather than adding broad sanitizer suppressions. No complete sanitizer matrix or SQLite-disabled full-suite matrix is required.

Each handoff records its stage, source identity, changed files, actual tests/diagnostics, failures/skips, and next proposed stage. Append closure evidence to the existing Phase 8 verification record; do not overwrite the earlier passing or superseded entries. Counts may change as tests are added: obtain them from the configured suite rather than hard-coding 169/170.

## 15. Closure acceptance criteria

1. Every invalid attempt-number request is rejected before storage conversion, and a live daemon remains usable afterward; genuine durable corruption still fails closed.
2. ControlClient safely survives destruction by ending delivery at every public signal boundary; normal exactly-once/correlation semantics and ASan regressions pass.
3. One-time terminal states are persisted atomically with final work, retries remain unfinished, recurring definitions remain live, and terminal state survives retained-history removal.
4. Cancellation, suspension, deleted precedence, and already-accepted manual work follow the explicit state/relationship tables without extra scheduled occurrences.
5. One fresh schema format is created/validated with foreign keys enabled; old/newer formats are rejected without conversion. The existing v1→v2 upgrade code and upgrade-only helpers/status fields are removed; schema failure prevents readiness.
6. Startup interruption handling atomically finalizes one-time definitions or retains retries, validates current relationships without historical backfill, remains bounded/repeatable, and preserves immutable snapshots.
7. Management operations and revisions obey the terminal-state matrix. New Run Now work is rejected for every terminal state; replay of a previously accepted key only returns its saved response. No v1 Clone or terminal reactivation exists.
8. Ordinary `jobuctl job list` defaults to Active; explicit state/all/deleted options and raw request-file semantics are documented, bounded, and tested.
9. `--arg-secret` and `--env-secret` preserve argument order, environment uniqueness, symbolic templates, and per-attempt resolution without extra value-reading APIs.
10. Public Doxygen, readable implementation structure, one ObjectPrivate allocation, appropriate signals, diagnostics, protocol/help/client documentation, and final Linux verification all pass.
11. Optional macOS status and actual privilege skips are explicit. Socket half-close work, install targets/default sockets, and other Phase 9 features remain outside this closure.

After these criteria pass, Phase 8 can be closed and Phase 9 can be planned against the new schema and lifecycle contracts.
