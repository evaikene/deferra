# JobU Phase 8 — Secrets, complete control protocol, and jobuctl

## 1. Purpose, baseline, and implementation workflow

Phase 8 makes the existing scheduler usable through a complete, documented public control interface. It completes named-secret handling, history and output queries, remaining RPC operations, a reusable typed C++ client, and `jobuctl` command UX.

This document is based on the merged Stage 7.22 tree. Phase 7 is closed, including the Stage 7.21 cancellation/rollback correction and final Linux verification. The approved omission of the additional macOS closure run is not an outstanding Phase 8 prerequisite. Record exact source revisions and commands in the Phase 8 verification/handoff record; keep specific commit IDs and commit URLs out of this planning document.

Proposed repository location: `docs/planning/jobu-phase8-code-design.md`.

Before implementation, refresh `main`, read the current root `AGENTS.md`, and reconcile intervening changes with the source inventory below. Implement exactly one approved stage at a time, provide its handoff, and stop for review. Temporary incompleteness between stages is acceptable; final acceptance applies to the assembled phase. Do not commit automatically.

This is an implementation design, not a claim that the new APIs or tests already exist. The source review did not run a new full build. The existing Stage 7 verification remains baseline evidence, and the stages below specify new evidence to obtain.

### 1.1 Phase boundary

| Deliver in Phase 8 | Preserve or leave to the later roadmap |
| --- | --- |
| Named-secret write/metadata/delete operations, typed references, dispatch-time resolution, and safe generated diagnostics | Encrypted storage, external secret services, remote authentication, and per-user authorization |
| Complete listed v1 RPC families, plus bounded `attempt.list` | New transports, streaming subscriptions, and a general RPC framework rewrite |
| Existing create/Run Now idempotency and job revisions exposed consistently | A new submission scheduler or use of the reserved `RunOrigin::Submitted` |
| History filters, opaque cursors, separate bounded output reads | Phase 9 retention timers, physical purge, rollups, and percentiles |
| Statistics that can be derived from existing durable data | Phase 9 runnable-wait measurement, delayed-job warnings, and broader observability |
| Refactored CLI, command/subcommand help, complete request access, JSON output, typed C++ client | Interactive terminal UI, shell completion generation, and packaging |
| Protocol, CLI, and client documentation under `docs/` | Phase 9 installation, service-manager, configuration, and operations documentation |

Application submission uses `job.create` with a once schedule, including the creation-only `at: "now"` form specified below. Do not add another run origin or duplicate the existing creation/scheduling invariants.

## 2. Current implementation and constraints

| Existing source | Baseline behavior and Phase 8 consequence |
| --- | --- |
| `src/jobuctl/main.cpp` | Approximately 1,546 lines containing parsing, request construction, rendering, connection, handshake, timer, and dispatch. Split these responsibilities before adding command families. |
| `src/core/command_line_parser.*` | Reusable lexical option parser. Keep it; command hierarchy/help belongs in `jobuctl`, unless a small lexer correction is proven necessary. |
| `src/jobu/management.*`, `management_json.*`, `management_rpc.*` | Fifteen queue/job management RPC methods, typed conversion, idempotent creation, revisions, and mutation/failure signals already exist. Extend their contracts without duplicating transactions in adapters. |
| `src/jobu/management.cpp` | C++ Run Now implementation exists but lacks public RPC/CLI exposure. Its eligibility, manual barrier, and idempotency rules remain authoritative. |
| `src/jobu/scheduler.*`, `scheduler_core_priv.*` | Public cancellation and fatal/shutdown gates exist. RPC cancellation must use `Scheduler::cancel_run()`. |
| `src/jobu/scheduler_dispatch_priv.cpp` | Revalidates under a transaction, persists Running attempt/run state, commits, then calls the executor. A rejected executor start becomes an immediate terminal completion. Secret storage failures must not accidentally take that ordinary-job-failure path. |
| `src/jobu/secret_repository_priv.*`, `secret.hpp` | Secret BLOBs, metadata pagination, current-definition reference rows, and delete constraints exist. No public value-read RPC or complete execution resolver exists. |
| `src/jobu/run_repository_priv.*`, `attempt_repository_priv.*` | Durable history exists; the output repository currently fetches whole BLOBs. Add projection/pagination/slicing queries instead of exposing these private rows directly. |
| `src/jobu/job_validation_priv.*`, `cli_job_payload_priv.*`, `http_job_payload_priv.*` | Stored payload validation currently calls concrete literal-only runner decoders. Introduce template validation without weakening concrete execution validation or recovery validation. |
| `src/jobu/sqlite/sqlite_schema.*` | Application-owned schema version 1, including secrets, references, history, output, and idempotency. Phase 8 needs only an index-only version 2 upgrade, not new tables. |
| `src/rpc/client.*` | Transport-independent asynchronous calls; result/error signals can occur synchronously inside `call()`. Typed client correlation must handle this documented behavior. |
| `src/jobud/runtime_priv.*` | Serving admission, fatal notification, immediate shutdown, dependency lifetimes, and capabilities. Integrate new services into these existing gates. |

The generic `jobu` target already links `core`, `net`, `db`, and `rpc`; concrete SQLite, HTTP-executor, and CLI-executor code lives in separate targets. A client-only consumer must not acquire a concrete SQLite or runner dependency. This phase does not need a wholesale split of existing domain/codec targets.

### 2.1 Non-negotiable implementation conventions

- Use `jb::core::JsonValue` and `core.json.*` for general JSON behavior. Do not restore `rpc::JsonValue` or expose nlohmann types.
- Every stateful `Object` subclass extends the one `ObjectPrivate` allocation. No second pimpl or private implementation fields on the public object. Bind derived owner back-references after base construction, then install connections.
- Use receiver-aware signals for reusable observable events. Existing one-operation completion callbacks and private return-value strategies remain appropriate; do not replace them mechanically.
- Public headers need useful Doxygen: semantics, ownership, affinity, ordering, limits, error meaning, and reentrancy where relevant. New `.cpp` blocks need concise comments explaining non-obvious transaction, secrecy, lifetime, and ordering decisions.
- Use readable logical blocks and meaningful helpers. Neither the CLI refactor nor the client should replace one long file with another long dispatcher.
- Preserve selective `[[nodiscard]]`: checked runtime outcomes retain it; routine cleanup and local correlation cancellation need not force callers to inspect an always-expected success.
- Preserve exception policy. Do not catch allocation failures, add broad catch-all cleanup, or mechanically document `std::bad_alloc`.
- Keep SQL bindings mandatory. SQLite schema/version/index inspection stays under `src/jobu/sqlite`; repository operations use `jb::db`.

## 3. Source and target organization

Names below are the intended responsibility boundaries. A small adjacent helper may remain in its owning `.cpp`; do not manufacture classes just to populate this list.

| Location | Responsibility |
| --- | --- |
| `src/jobu/secret.hpp` | Existing metadata and new metadata request/page types |
| `src/jobu/secret_service.hpp/.cpp` | Checked secret set/list/delete operations and service gates |
| `src/jobu/secret_provider.hpp` | Trusted execution-only provider interface |
| `src/jobu/secret_provider_priv.*` | Database-backed provider; no public secret repository |
| `src/jobu/payload_template_priv.*` | Recognized reference grammar, reference extraction, template validation, transient resolution |
| `src/jobu/secret_json.*`, `secret_rpc.*` | Secret wire codecs and registration |
| `src/jobu/history.hpp`, `history_service.*` | Public history DTOs and checked read service |
| `src/jobu/history_repository_priv.*`, `history_cursor_priv.*` | Projection queries, output slicing, bounded cursor ownership |
| `src/jobu/history_json.*`, `history_rpc.*` | History/output wire conversion and registration |
| `src/jobu/statistics.hpp`, `statistics_service.*`, `statistics_repository_priv.*` | Typed retained-history aggregates |
| `src/jobu/statistics_json.*`, `statistics_rpc.*` | Statistics conversion/registration |
| `src/jobu/control_json.*`, `control_rpc.*` | Run Now/cancel and schedule validation/preview adapters; reuse management codecs where already present |
| `src/jobu/client/control_client.hpp/.cpp`, `control_client_priv.hpp` | Typed asynchronous `ControlClient`; new `jobu-client` target |
| `src/jobuctl/command_line_priv.*`, `command_registry_priv.*` | Parsed command model, hierarchy, option metadata, dispatch selection |
| `src/jobuctl/help_priv.*`, `input_priv.*`, `output_priv.*` | Local help, bounded file/stdin input, human/JSON/raw rendering |
| `src/jobuctl/session_priv.*` | Socket, RPC client, typed client, deadline and optional polling lifecycle |
| `src/jobuctl/commands/*_commands.cpp` | Separate system, queue, job, run, attempt, secret, and schedule request builders/renderers |
| `src/jobuctl/main.cpp` | Parse/select local actions, construct a session for remote work, return its exit status |

Add `src/jobu/client/CMakeLists.txt`: `jobu-client` publicly links `jobu` and `rpc`, exposes its own public include directory, and requires C++20. It does not link `jobu-sqlite`, `db-sqlite`, `jobu-http`, or `jobu-cli`. `jobuctl` links `jobu-client`; its private command implementation can be an internal static target so parser/help tests share production code. No public CLI framework is introduced.

New service objects borrow the already-open owner-thread database and required immutable registries/time/UUID dependencies. They do not open connections or own another scheduler. Constructors perform no database operations and emit no signals.

## 4. Named secrets and payload templates

### 4.1 Public write and metadata service

Retain `SecretMetadata { name, created_at, updated_at }`. Extend with these owning request/result types:

```cpp
struct SetSecretRequest {
    std::string name;
    jb::core::ByteBuffer value;
};

struct SecretListRequest {
    std::size_t limit{100};
    std::optional<std::string> after_name;
};

struct SecretPage {
    std::vector<SecretMetadata> items;
    std::optional<std::string> next_after_name;
};
```

`SecretService final : Object` exposes checked `set(SetSecretRequest) -> Result<SecretMetadata, Error>`, `list(SecretListRequest const&) -> Result<SecretPage, Error>`, and `erase(std::string_view) -> Result<void, Error>`, plus `stop_mutations()`, `failed`, and `mutation_committed`. The last two follow ManagementService signal ordering and owner-thread rules. Dependencies are `Database&`, `TimeSource&`, and optional parent. Put all instance state in its `Private : ObjectPrivate`.

Rules:

- Names follow the existing repository's canonical name validation, including its 128-byte limit. Preserve case; do not add normalization that changes existing identity.
- Each value is 0–65,536 raw bytes. Empty values are valid. Apply the same limit to direct C++ service calls and decoded RPC input.
- `set` is an upsert preserving `created_at`, replacing the bytes, and updating `updated_at` in one transaction. It returns metadata only. Repeating a set does not need an idempotency key; it may update the modification timestamp.
- `list` is lexicographic by name, uses limit 1–200, fetches one extra metadata row, and never selects the value column. Adjust the private repository lookahead limit to permit 201 rows internally; the existing private 200-row cap must not make a public maximum-size page fail.
- Delete returns null over RPC. A missing name is a normal not-found error; a referenced name is a conflict. There is no force flag.
- No API returns value length, digest, preview, or bytes as part of a metadata response. The trusted execution provider below is a separate in-process capability, not a read operation for administrators or clients.
- Values remain plaintext BLOBs in v1 SQLite. Filesystem access to the database is privileged access to those values. This phase does not promise encryption or secure erasure of every allocator/library copy.

Use stable new operation codes `jobu.secret.invalid_name`, `jobu.secret.too_large`, `jobu.secret.not_found`, and `jobu.secret.in_use` where an existing code does not already cover the condition. Preserve repository-owned codes at private boundaries and normalize once at the service boundary. Errors contain fixed safe text; never include supplied bytes, decoded text, or SQL diagnostics.

### 4.2 Reference syntax and accepted positions

A reference replaces one whole value and has exactly this shape:

```json
{"secret":"service.token"}
```

It is not interpolation. `${...}`, string concatenation, defaults, and transformations are outside this phase. For a bearer header, store the complete `Bearer ...` value in the secret.

| Payload position | Existing literal form | New reference form |
| --- | --- | --- |
| CLI `arguments[i]` | String | Reference object |
| CLI `environment[name]`, except `PATH` and reserved `JOBU_*` names | String or null removal | Reference object; null still means removal |
| HTTP `headers[i].value` | String | Reference object |
| HTTP `body` | Existing `{encoding, data}` object or absent | Reference object representing raw request-body bytes |

Executable name, working directory, environment variable names, `PATH`, URL, HTTP method, header names, expected statuses/exit codes, and schedule fields remain literal. Restricting these positions keeps secret handling focused on data rather than executable routing or ambient search behavior.

Examples:

```json
{
  "command":"/usr/local/bin/report",
  "arguments":["--account", "daily"],
  "environment":{"REPORT_TOKEN":{"secret":"reports.token"}}
}
```

```json
{
  "url":"https://api.example.test/events",
  "method":"POST",
  "headers":[{"name":"Authorization","value":{"secret":"events.authorization"}}],
  "body":{"secret":"events.request_body"}
}
```

Extract references only from these recognized positions. An arbitrary additive payload member containing `{"secret":...}` is not a binding and must not be recursively rewritten. Reject malformed reference objects at recognized positions, extra members in a reference object, invalid names, and more than 256 reference occurrences per template. Use deterministic JSON-pointer-style field paths, escaping `~` and `/` in environment keys; deduplicate exact `(secret_name, field_path)` pairs.

### 4.3 Separate template validity from concrete execution validity

`ValidatedJobPayload` continues to mean a valid, bounded persisted document, now allowing recognized references. Its serialized original JSON remains the authoritative snapshot.

Introduce explicit template parsing/validation helpers. Share primitive checks with existing concrete decoders where useful, but do not validate by substituting fake strings into the payload: placeholder lengths, PATH rules, header constraints, and body encodings would make that unreliable.

Template validation checks all literal fields immediately and all structural/count constraints independently of the unresolved values. The original serialized template remains limited to 256 KiB. The direct service, RPC, repositories' durable decode, idempotency replay validation, and Phase 7 recovery must all agree that a well-formed reference is a valid stored template.

Concrete runner decoders continue to accept execution-ready literals only. Resolution must produce a complete concrete payload and pass their existing validation before any executor starts:

- CLI argument/environment secret bytes must be valid UTF-8 and contain no NUL; all existing argument/environment/prepared-size limits still apply.
- HTTP header secret bytes must satisfy the normal header-value validation, including no CR/LF/NUL. A header produced from a reference is always marked sensitive, even if the caller supplied `sensitive: false` or used an otherwise ordinary header name.
- HTTP body secret bytes remain binary. Encode into the existing concrete body representation, without a text round trip.
- The resolved serialized payload is also limited to 256 KiB. Enforce runner-specific prepared-request limits, including injected `JOBU_*` data, afterward. A template that was small enough to store may fail preparation after secret expansion.

Unknown additive payload fields are retained exactly as today. Do not silently reject legacy literal payloads or weaken their validation to accommodate references.

### 4.4 Transactional reference ownership and deletion

`jobu_secret_refs` indexes current job definitions. Maintain it in the same transaction as the corresponding definition:

- Create: validate referenced names exist and insert reference rows with the definition/run/idempotency record.
- Update: inspect the complete replacement payload and type, validate all referenced names, then atomically replace the definition's reference rows. Revision conflict or any later failure restores the old rows.
- Move/suspend/resume: references follow the same definition and are unchanged.
- Job/queue delete: retain existing transactional reference cleanup. Older nonterminal snapshots still protect their references through the check below.
- Idempotent replay returns the stored original result without resolving values or revalidating against a later secret rotation. It must not rewrite references or allocate new identities.

A current-definition count is insufficient for deletion: a running/retry-wait run may still reference a secret removed by a newer definition revision. In the delete transaction:

1. Check the current-definition reference index.
2. If no current references exist, scan **all nonterminal run snapshots**, in bounded ID pages, using the same template extractor.
3. Refuse deletion if any snapshot references that name.
4. Otherwise delete and commit.

Keep one transaction across the check and deletion; do not issue nested transactions from repository helpers. The single owner/writer makes this atomic against create, update, dispatch, and completion. Each page excludes result/output BLOBs and releases its query before the next page. This is bounded-memory work, not a promise of constant query latency. Terminal historical snapshots do not prevent deletion. No additional snapshot-reference table or generic SQL JSON extension is required in v1.

Malformed stored templates are persisted-data failures, not evidence that a secret is unreferenced. Do not proceed with deletion after an incomplete scan. Existing foreign keys remain a final integrity safeguard, and expected `in_use` conflicts should be detected before a raw constraint reaches the fatal classifier.

### 4.5 Trusted provider and dispatch integration

Add a narrow public execution seam:

```cpp
class SecretProvider {
public:
    virtual ~SecretProvider() = default;
    [[nodiscard]] virtual auto resolve(std::string_view name)
        -> jb::core::Result<jb::core::ByteBuffer, jb::core::Error> = 0;
};
```

This interface explicitly grants access to bytes to trusted runner orchestration. It is synchronous, owner-thread-only in the database implementation, borrows the name for the call, returns an owning buffer, and must not retain caller references. It performs no callbacks or event processing. Its represented failures are a missing-name error, a database error, or an explicitly classified persisted-data error. Treat an unexpected provider error as a safe fatal internal preparation failure; do not silently classify an unknown provider failure as an invalid user value. A future external/asynchronous provider would require a separate lifecycle design; do not pretend this interface already supports it.

The private database provider borrows `Database&`, uses a new private value lookup, and does not open/commit a transaction. Scheduler construction gains a required borrowed `SecretProvider&` alongside the executor. Update daemon composition, private core/dispatch signatures, and fake fixtures together; no compatibility constructor is needed for nonexistent external consumers. A test provider that rejects unexpected lookups handles literal-only tests.

The dispatch transaction becomes:

1. Re-read eligibility and current availability as today.
2. Validate/extract the selected immutable run template and resolve each distinct secret name once into a **per-attempt** transient map.
3. Validate the concrete payload; retain either the prepared payload or a safe ordinary preparation failure.
4. Insert the Running attempt, mark the run Running, and commit as today.
5. Start the executor with the concrete transient payload, or return an immediate terminal Failed completion for the preparation failure.

Never persist the concrete payload in a run, job, idempotency request/result, or attempt metadata. Never resolve once at job creation or reuse a resolved map across attempts: a retry must observe rotation.

| Failure while preparing | Required path |
| --- | --- |
| Missing secret, invalid bytes for the destination, or resolved limits exceeded | Safe terminal attempt failure after the durable attempt-start transaction commits; no external executor start; normal completion/recurrence path |
| Database/provider storage failure | Abort dispatch; classify in **Dispatch** context; no executor call and no ordinary failed-job substitution |
| Malformed persisted template or invalid durable decode | PersistedData fatal path; no external start |
| Insert/transition/commit/rollback failure | Preserve Phase 7 fatal handling, including post-cleanup poison detection |

Use fixed preparation reason codes such as `jobu.secret.not_found`, `jobu.secret.invalid_value`, and `jobu.secret.resolved_payload_too_large`. A synthetic result may include a trusted code and fixed reason, not the provider's raw message/detail or the resolved request. Preparation failures are terminal rather than endlessly retrying invalid configuration; users can repair the definition/secret and create another run.

Ensure immediate completions continue through the scheduler's existing acceptance/accounting path. Do not invoke an external callback while a transaction is active or bypass the core's capacity ownership. Failure observers run only after transaction/query cleanup, with completion acceptance already closed where required.

### 4.6 Redaction and captured output

Audit generated error/result/log paths in CLI and HTTP executors, generic HTTP diagnostics, service adapters, and client/CLI error rendering. Tests use distinctive sentinels and inspect persisted job/run/idempotency/result data and generated diagnostics.

Required guarantees:

- Reads return original references, never resolved arguments, environment values, header values, or request body bytes.
- Redirect handling removes secret-derived sensitive headers on cross-origin redirects using the existing HTTP policy.
- Error text cannot include resolved values from rejected headers, URL/request diagnostics, provider errors, or process-start formatting.
- No request-body/request-header debug dump is introduced in CLI JSON mode or client errors.

Captured subprocess output and HTTP response bytes remain raw user-controlled data. A program can echo its environment, and a server can echo a request secret. JobU cannot promise those bytes are secret-free while also promising faithful binary capture. Document this boundary and the existing capture settings; do not add an unreliable global byte-substitution filter or claim complete redaction of arbitrary output. Prefer environment references over command arguments when OS process-list exposure matters.

## 5. Creation, idempotency, and remaining controls

### 5.1 Preserve existing semantics

Expose the existing canonical idempotency behavior for `queue.create`, `job.create`, and `job.run_now`: same key plus equivalent canonical request replays the stored original result; a different canonical request conflicts. Keep the existing key validation, scope, and retention rules. Responses must not be recomputed from the now-current queue/job/run.

Job update/move and existing revision-bearing operations require the exact expected revision. CLI must not fetch a new revision and silently retry a stale write. Queue operations retain their current contract; do not invent queue revisions.

The new templates are part of the canonical request as references, not resolved bytes. Secret rotation therefore does not change request identity. Validate idempotency request/result records containing templates during replay and recovery.

### 5.2 Immediate once creation

Add a creation-only schedule alternative:

```cpp
struct ImmediateSchedule {};
using JobCreationSchedule = std::variant<ImmediateSchedule, OnceSchedule, CronSchedule>;
```

Use it in `CreateJobRequest`; persisted `JobSchedule` and update requests remain the existing once/cron types. Wire spelling: `{"kind":"once","at":"now"}`. For a new operation, resolve `now` from the injected UTC time source inside the creation transaction after idempotency replay has been ruled out. Store a normal concrete once schedule and its schedule-owned run.

The canonical idempotency request retains the symbolic `"now"`, while its stored result contains the resolved timestamp. A lost response followed by the same key/request must replay the original job/run, not conflict because the client's clock advanced. `job.update` rejects symbolic `now`; it accepts an explicit timestamp as today. CLI `job create --now` uses this wire form, not a freshly generated client timestamp.

Do not permit simultaneous `--now`, `--at`, and `--cron`. Immediate creation is still ordinary scheduled work and respects queue suspension, capacity, and retry rules.

### 5.3 Run Now and cancellation

`job.run_now` maps directly to `ManagementService::run_now(RunNowRequest)`. Preserve all current rules: a strictly future schedule-owned Scheduled occurrence, no Running/RetryWait work or nonterminal manual barrier, latest definition snapshot, unchanged scheduled tick, job-suspension bypass only, and no queue-suspension bypass.

`run.cancel` calls `Scheduler::cancel_run()` on the owner thread while Serving. It returns an owning cancellation result with `disposition: "completed" | "requested"` and the bounded run view. `requested` is not a durable terminal outcome: active capacity remains held until completion persists. Pending cancellation and a required recurring successor remain one transaction.

Do not implement RPC cancellation with direct repository writes or local `rpc::Client::cancel()`. Repeated cancellation of already terminal work retains the current operation-conflict behavior. A transport disconnect after submission can leave the mutation outcome unknown; clients query the run before deciding what to do.

### 5.4 Schedule methods

Both methods reuse the daemon's `CronEngine`, including the agreed cyclic weekday ranges, `0`/`7` exclusion, wildcard-step rejection, timezone, DST, and strict-next rules.

- `schedule.validate`: params `{"schedule":{"kind":"cron","expression":"...","timezone":"UTC"}}`; result `{"valid":true}`. Invalid schedules use the ordinary structured error path, not a successful `valid:false` variant.
- `schedule.next`: same schedule plus required `after` RFC3339 UTC timestamp and optional `count` default 5, range 1–200; result `{"occurrences":[...UTC timestamps...]}`.

Only cron schedules are accepted by these preview methods; once timestamps are validated by creation/update codecs. No new parser, local-time timestamp ambiguity, or cron normalization engine is introduced.

## 6. History, cursors, and output retrieval

### 6.1 Public DTOs and service

Define transport-facing types in `history.hpp`; they are owning values and contain no repository/query handles. Use UUID, UTC time, and existing enum types internally, with explicit wire conversion.

```cpp
struct UtcRange {
    std::optional<jb::core::UtcTimePoint> from; // inclusive
    std::optional<jb::core::UtcTimePoint> to;   // exclusive
};

struct RunFilters {
    std::optional<jb::core::Uuid> queue_id;
    std::optional<jb::core::Uuid> job_id;
    std::optional<RunState> state;
    std::optional<RunOrigin> origin;
    std::optional<JobType> type;
    UtcRange planned;
    UtcRange started;
    UtcRange completed;
};

struct RunQuery {
    RunFilters filters;
    std::size_t limit{100};
};

struct CursorRequest { std::string cursor; };
using RunListRequest = std::variant<RunQuery, CursorRequest>;
```

`RunSummary` contains `id`, `job_id`, `queue_id`, `job_revision`, `origin`, `type`, `state`, `schedule_owned`, `priority`, `planned_at`, `runnable_at`, nullable `started_at`, and nullable `completed_at`. It excludes attributes, payload, detailed result, and output. `RunDetails` adds the immutable `attributes`, original template `payload`, and nullable safe `result`. It does not inline all attempts.

`AttemptSummary` contains `run_id`, positive `attempt_number`, `due_at`, nullable `started_at`/`completed_at`, `state`, and nullable `outcome`. `AttemptDetails` adds nullable `result`; output is always separate. `RunPage` and `AttemptPage` contain `items` and nullable `next_cursor`.

`HistoryService final : Object` borrows database, attribute registry, cursor clock/UUID dependencies, and optional parent. Expose checked:

- `get_run(Uuid const&) -> Result<RunDetails, Error>`;
- `list_runs(RunListRequest const&) -> Result<RunPage, Error>`;
- `get_attempt(AttemptKey const&) -> Result<AttemptDetails, Error>`;
- `list_attempts(AttemptListRequest const&) -> Result<AttemptPage, Error>`;
- `read_output(AttemptOutputRequest const&) -> Result<AttemptOutputChunk, Error>`;
- `failed`, emitted once for a fatal read/persisted-data failure after query cleanup; `shutdown()` to close admission and discard cursor state.

`AttemptListRequest` is an initial `{run_id, limit}` or a cursor-only continuation. Read methods never invoke recovery, modify jobs, or advance the scheduler. Their errors use the shared Read/PersistedData classifier; ordinary not-found/invalid-request errors do not shut down the daemon. A poisoned connection always closes the service through the existing fatal path.

### 6.2 Filters and ordering

Wire `run.list` initial params are `queue_id`, `job_id`, `state`, `origin`, `type`, `planned`, `started`, `completed`, and `limit`, all optional. Each range uses optional `from`/`to` UTC strings. Empty ranges impose no filter; nonempty ranges require `from < to` when both bounds exist. Rows with null timestamps fail a corresponding nonempty timestamp filter. Filters combine with AND.

Queue/job filters use the identifiers stored on the run snapshot. Moving or renaming the current job must not rewrite historical queue membership. History for soft-deleted owners remains accessible by ID. Do not join against only active definitions and accidentally hide retained history.

Only the implemented `scheduled` and `manual` origins are accepted as filters; `submitted` remains reserved. Reject unknown enum values rather than treating them as no filter.

Run pages sort by `(planned_at_us DESC, id DESC)`. The immutable planned timestamp and UUID form the keyset boundary:

```sql
planned_at_us < :last_planned
OR (planned_at_us = :last_planned AND id < :last_id)
```

Combine this parenthesized expression with bound filters. Match UUID byte ordering to the repository's canonical representation. Attempt pages for one run sort by `attempt_number DESC` and continue below the last number. Do not use SQL OFFSET or fetch unbounded attempt histories.

Pagination is a live view, not a multi-request database snapshot. State changes and newly inserted backdated records can change which rows match later pages. Stable keys prevent returning an already-emitted row again during a forward traversal; the API does not promise that a mutable filter's result set stays frozen. Make no unsupported high-watermark consistency claim.

### 6.3 Opaque cursor ownership

Use a small server-owned `HistoryCursorStore`, a non-Object implementation value in the service's private data. A cursor is an opaque UUID token referring to stored, typed continuation state. It is not a serialized SQL fragment, signed authentication token, or offset.

Each entry binds method family, normalized filters, page size, last key, and a fixed expiry. Rules:

- Initial page limit defaults to 100, accepts 1–200.
- A continuation request contains **only** `cursor`; any additional filter/limit is rejected. The stored state supplies them.
- TTL is five minutes from the initial page, measured with a monotonic clock. Continuation does not extend it.
- At most 1,024 live entries; evict expired entries first, then least-recently-used entries. Eviction and restart can invalidate a cursor before its nominal expiry.
- An unknown, malformed, wrong-method, expired, or evicted token returns `jobu.history.invalid_cursor` with fixed text. No SQL or internal filters are revealed.
- Retrying a still-valid cursor is allowed and reads the live page again. Issued successor tokens have the original expiry; do not mutate the input token's last key.
- Use injectable UUID and monotonic-clock strategies in tests. The return-value clock strategy is an appropriate private callback, not an Object event.

Tokens grant no additional privilege: v1 socket access already grants access to these methods. Do not add a crypto dependency or per-user security model for cursor transport. Statistics pagination may reuse this store with its own typed entry variant and method discriminator.

### 6.4 Bounded responses

Run/attempt lists select summary columns only, never payload or output. Fetch at most `limit + 1` rows. Stop before exceeding a 512 KiB serialized result budget, returning a continuation based on the **last emitted** row. Never skip the first un-emitted row or return an empty page with a self-repeating cursor.

Apply an equivalent byte-budget safeguard to existing `queue.list` and `job.list`, preserving their ascending-ID ordering and `next_after_id` contract. Their request page size is an upper bound, not a guarantee of that many items. A list containing large job payloads must remain retrievable in smaller successive pages rather than breaking the connection at the frame limit.

Single-object get/create/update results must be checked against the configured RPC body limit with their actual envelope overhead. Existing 256 KiB document/result limits normally leave room in the 1 MiB frame; do not assume this without testing the largest accepted objects and JSON escaping. If an object cannot fit, return a bounded `jobu.response.too_large` operation error instead of failing the stream. Do not silently truncate stored fields. A response encoding failure after a committed mutation cannot roll it back; preserve the idempotency record and document replay/reconciliation.

### 6.5 Output requests and encoding

Add:

```cpp
enum class OutputChannel { Stdout, Stderr, Body, Headers };

struct AttemptOutputRequest {
    AttemptKey attempt;
    OutputChannel channel;
    std::uint64_t offset{0};
    std::size_t limit{16 * 1024};
};
```

Wire request fields: `run_id`, `attempt_number`, `channel`, optional `offset`, optional `limit`. Limit is 1–65,536 **raw retained bytes**. Reject unrepresentable SQL offsets before conversion. CLI attempts accept `stdout`/`stderr`; HTTP attempts accept `body`/`headers`. Internally body maps to the existing primary/stdout BLOB and headers to the diagnostic/stderr BLOB; these column names are not the public HTTP vocabulary.

The result contains:

| Field | Meaning |
| --- | --- |
| `run_id`, `attempt_number`, `channel` | Requested identity |
| `status` | `available`, `pending`, `not_captured`, or `lost`, as defined below |
| `offset`, `bytes_returned`, `next_offset` | Position/count within retained bytes; nullable continuation |
| `retained_bytes` | Stored channel length, excluding omitted content |
| `total_bytes` | Nullable total observed bytes from trustworthy capture metadata |
| `omitted_bytes` | Nullable `total_bytes - retained_bytes`, only when known and consistent |
| `truncated`, `capture_lost` | Existing capture flags, normalized without inventing successful capture |
| `encoding`, `data` | `utf8` plus valid UTF-8 text, or `base64` plus RFC 4648 padded data |

Define status from durable evidence:

- An incomplete attempt has `pending`; no live pipe/HTTP streaming is exposed.
- A completed attempt with a retained channel BLOB, including a zero-length BLOB, has `available`, unless capture is marked lost.
- A completed attempt with no retained channel and no loss indication has `not_captured`; capture policy or a pre-execution failure can explain it.
- Any explicit loss evidence makes status `lost`. Return any retained bytes that do exist and set `capture_lost`; do not manufacture missing data.

For pending/not-captured/lost-without-bytes, return empty data, zero retained length, and null continuation. Unknown total/omitted counts are null, especially for interrupted recovery where the daemon could not observe final byte counts. Do not derive a measured zero from an absent row or unavailable result field.

Select `length(blob)` and a bound `substr(blob, :offset_plus_one, :limit)` projection in a private repository query; finish metadata queries before opening another query. Do not call the existing whole-BLOB `find_output()` from this endpoint. Keep binary semantics; validate query results as BLOB/nullable BLOB. Concrete backend SQL adaptation, if needed later, belongs at the backend boundary rather than in public DTOs.

Offsets refer to the concatenation of retained prefix and suffix after truncation. `omitted_bytes` describes the missing middle; clients must not treat retained offsets as original-output offsets. `offset == retained_bytes` is valid EOF; larger offsets are invalid. Compute `next_offset` from returned raw bytes and use null at EOF. A UTF-8 codepoint split by a requested chunk may make that chunk base64; clients reconstruct bytes before text decoding. Do not shift requested offsets or lose partial codepoints.

Completed output is immutable until retention deletes it. Future deletion between chunk requests returns not-found; the client must not silently append data from another identity. No retention worker is added here.

## 7. Application schema version 2

Add only indexes to support the new run history ordering and common owner-scoped queries:

```sql
CREATE INDEX jobu_runs_planned_id_idx
    ON jobu_runs(planned_at_us DESC, id DESC);
CREATE INDEX jobu_runs_queue_planned_id_idx
    ON jobu_runs(queue_id, planned_at_us DESC, id DESC);
CREATE INDEX jobu_runs_job_planned_id_idx
    ON jobu_runs(job_id, planned_at_us DESC, id DESC);
```

Keep existing scheduler, attempt, completion, reference, and idempotency indexes. Do not add every permutation of optional filters; use measured query plans to justify further indexing. The attempt composite primary key already supports per-run pagination. History queries can use a planned-order index with residual filtering; do not promise every combination has a dedicated covering index.

Extend `sqlite::ensure_schema()` to support an explicit version-1-to-version-2 application upgrade:

1. Under the existing exclusive database ownership and immediate RAII transaction, recognize the marker and validate the version-1 schema before changing it.
2. Create the three indexes using explicit DDL. Validate their table, column order, and descending flags using SQLite-owned inspection.
3. Update the singleton marker to version 2 in the same transaction.
4. Validate the version-2 manifest, commit, and return current version.

Fresh databases create version 2 directly. Existing valid version 2 databases are validated without repair. Reject newer versions, malformed markers, incompatible/missing objects, and same-name incorrect indexes. Do not use `IF NOT EXISTS` to hide an incompatible object or drop/recreate user data. A failed upgrade must leave a reopenable version-1 database after successful rollback; rollback poisoning must prevent daemon startup. Recovery and socket serving begin only after successful schema handling.

Extend `SchemaStatus` with `upgraded` (false for fresh/existing-v2, true for successful v1 upgrade); `created` retains its meaning of initializing an empty database. Update Doxygen that currently says version 1. Keep the upgrade code and manifest differences under `jobu-sqlite`; this is not a generic migration framework or a database-driver responsibility.

Test fresh, upgrade, reopen, missing/wrong index, newer marker, mid-upgrade failure, commit failure, and rollback failure using the existing private schema observer/fault infrastructure. Preserve actual job/run/attempt/output/secret/idempotency rows in an upgrade fixture, not merely an empty schema.

## 8. Statistics without inventing unmeasured values

The roadmap assigns statistics RPC names to Phase 8 and full delay observability to Phase 9. Phase 8 supplies retained-data counts and explicitly labeled wall-clock durations. Phase 9 remains responsible for runnable-wait accumulation and warnings. Update the roadmap wording to explain this split.

### 8.1 Request and population

`StatisticsRequest` contains optional `queue_id`, `job_id`, `type`, `origin`, a `planned` range, `group_by`, and `limit`; a cursor-only continuation uses the same rules as history. `queue.stats` uses a required existing QueueSelector on its initial request and omits `queue_id`; bind its resolved stable ID before producing a cursor. Its continuation is cursor-only, with no repeated selector. `system.stats` can cover all queues.

- `planned.to` defaults to server UTC now; `planned.from` defaults to 24 hours before the resolved upper bound. Explicit windows must be finite, increasing, and at most 31 days. Clients can query adjacent windows for longer retained periods.
- Capture resolved bounds in the first response/cursor so pagination does not slide the window.
- Population is runs whose immutable planned time is in the window, after optional filters, and **all retained attempts belonging to those runs**, including retries outside the planned window. This is a planned-run cohort, not an ambiguous mixture of started/completed windows.
- `group_by` is `none` by default or one of `queue`, `job`, `type`, `origin`, `state`. A job/queue breakdown is paginated, never an unbounded map. Group order is canonical UUID bytes for identities and documented enum wire spelling for enums; continuation resumes after the last key.
- `limit` defaults to 100 and is 1–200. With `group_by:none`, return one aggregate group, even for an empty population.

### 8.2 Result

Return `window`, `group_by`, `groups`, nullable `next_cursor`, and `measurement` metadata. Each group contains its nullable key and:

- `runs.total`, fixed counters for every implemented run state, and fixed type/origin counters;
- `attempts.total`, pending/running/completed counts, fixed terminal outcome counts, and `retries` (attempt number greater than one, counted once per retained attempt);
- `capture.truncated_attempts` (either channel truncated, counted once) and `capture.lost_attempts`;
- `schedule_lateness_ms: {samples, average, maximum}`;
- `execution_wall_duration_ms: {samples, average, maximum}`;
- `runnable_wait_ms: null`.

For lateness, use each run's first persisted start minus planned time, exclude negative/unrepresentable samples, and count each run once. For execution wall duration, use completed attempt start/end pairs, exclude missing/negative/unrepresentable pairs, and count each valid attempt once. Use null average/maximum for zero samples. The response identifies both measurements as wall-clock-derived and reports `runnable_wait: "unavailable"`; do not substitute `runnable_at` or retry delay for eligible capacity wait. Phase 9 may add a separately named measured duration metric rather than changing this one's meaning.

Aggregate runs and attempts separately before combining them, so retries/output joins do not multiply run counts. Use checked count conversions and sufficiently wide arithmetic; avoid overflowing integer `SUM` for timestamp differences. Counts and sums are never inferred by reading every payload/result document. Truncation/loss counts refer specifically to persisted output rows with their corresponding flags; Phase 7 recovery already writes a loss-marked output row for interrupted capture. An absent row contributes no flagged capture event and is not a claim that capture succeeded. Do not inspect every result JSON to invent additional events; malformed selected flags are a fatal decode error rather than a fabricated count. State this evidence basis in the measurement metadata.

Implement in `StatisticsService final : Object`, using a private repository and the shared service read/failure rules. Queries and returned groups are bounded in memory. SQL aggregation can still consume time proportional to the selected history; test representative populated query plans and record costs without imposing a flaky wall-clock test threshold or claiming constant time. Do not introduce a database worker thread, persisted rollups, or a nested event loop in this phase.

## 9. JSON-RPC surface and daemon composition

### 9.1 Required method matrix

All methods use object params and existing strict request decoding. Unknown response members are ignored by clients. `void` results remain JSON null.

| Family | Required methods after Phase 8 | Contract/source |
| --- | --- | --- |
| System | `system.info`, `system.stats` | Existing info; §8 statistics |
| Queue | `queue.create`, `queue.get`, `queue.list`, `queue.update`, `queue.suspend`, `queue.resume`, `queue.delete`, `queue.stats` | Existing management; bounded lists; §8 |
| Job | `job.create`, `job.get`, `job.list`, `job.update`, `job.suspend`, `job.resume`, `job.move`, `job.delete`, `job.run_now` | Existing management plus templates, immediate create, §5 |
| Run | `run.get`, `run.list`, `run.cancel` | §§5–6 |
| Attempt | `attempt.get`, `attempt.list`, `attempt.output` | §6; list is a small addition to the roadmap's minimum family |
| Secret | `secret.set`, `secret.list`, `secret.delete` | §4 |
| Schedule | `schedule.validate`, `schedule.next` | §5.4 |

`secret.set` params are `{"name":"...","value":{"encoding":"utf8|base64","data":"..."}}`. Decode strict base64 into bytes, enforce the raw-size limit, and reject malformed encoding objects. The result is SecretMetadata. `secret.list` params use `limit`/`after_name`; `secret.delete` uses `name`.

`job.run_now` params use `job_id` and optional `idempotency_key`, returning RunDetails. `run.get` and `run.cancel` use `run_id`. `attempt.get` uses `run_id` and `attempt_number`; `attempt.list` initially uses `run_id` and `limit`. Cancellation result is `{"disposition":"requested|completed","run":...RunDetails...}`. Build public views with the same codec as history; do not serialize an executor request containing resolved secrets.

For unchanged management methods, the current `management_json.*` field names, omission/null semantics, and error identities are authoritative. Document them explicitly from code; do not rename `revision`, selectors, or `after_id` merely to match a new family. Typed transport DTOs remain separate from private scheduler/repository representations.

### 9.2 Errors, framing, and capabilities

Retain JSON-RPC 2.0 envelope errors. Application failures continue to use numeric `-32000`, safe `message`, and `data: {category, code}` as the current adapter does. Do not flatten `core::Error::detail` into public data. Reuse an existing stable domain code where appropriate; new error names must be documented with their category and operation/terminal meaning in the same stage.

Document actual framing: UTF-8 JSON body, byte-counted mandatory `Content-Length`, CRLF header termination, existing optional/unknown-header handling, persistent stream, request IDs, notifications, batches, and error behavior. Defaults currently include 16 KiB headers, 1 MiB body, 2 MiB queued output, 64 batch entries, and 128 client pending requests. Library JSON nesting defaults to 64 containers. Check the corresponding server/client option declarations while writing normative tables; distinguish configurable library limits from daemon-configured limits.

Batches are not database transactions. A notification has no response, including for a mutation; application clients should use requests whenever they need an observed outcome. Response order for concurrent requests is not guaranteed. No JSON-RPC or framing rewrite is required.

`system.info` advertises sorted unique method names actually registered in the composed daemon. Add a single API minor increment from 1.2 to 1.3 when the Phase 8 surface is integrated. Keep major 1; there is no reason to add an incompatible-client migration layer. Handshake decisions use the major plus method capabilities, not exact daemon/minor equality.

### 9.3 Lifetime and fail-closed integration

Keep RPC adapters thin: strict decode, one service/scheduler call, encode a safe result. They do not begin their own transactions, resolve secrets, fetch revisions implicitly, or trigger a second mutation signal.

Expose per-family registration functions and method-name spans following `register_management_methods()`. Registration failure discards the partially configured server; never listen with an advertised but missing handler. `system.info` capabilities are assembled from those same registration lists.

Extend daemon private state with provider/services. Construction and teardown order must make all borrowed dependencies outlive their consumers: database/registry/clock/provider before scheduler, services before handler registration, stop RPC admission before releasing services, scheduler before executor/provider teardown. Do not destroy objects from inside a synchronous `failed` slot.

Connect new service `failed` signals into the existing Phase 7 fatal transition using receiver-aware connections. The transition closes management and secret mutation gates, history/statistics admission, scheduler completion acceptance, and listener/session admission in the established order. New mutation boundaries must classify after transaction cleanup and promote a poisoned connection even if the original returned error was ordinary; preserve the first fatal error and sanitize it once. Add tests for this exact interaction rather than assuming the Stage 7 cancellation fix automatically covers new services.

Only successful newly committed mutations emit `mutation_committed`; preserve existing replay behavior. Connect the signal to the existing coalesced scheduler rescan where needed. Secret rotation needs no synchronous dispatch and must not reenter an active transaction.

## 10. Reusable typed C++ client

### 10.1 Ownership and public API

Add `jb::jobu::ControlClient final : jb::core::Object` in `src/jobu/client/control_client.hpp`. It wraps a borrowed, already-connected `jb::rpc::Client&` and borrowed `AttributeRegistry const&`; both outlive it and share its event-loop thread. It owns neither the socket nor an event loop. It must work with a suitable arbitrary IODevice through the underlying RPC client, including the in-memory test transport.

The public shape is:

```cpp
using ControlCallId = std::uint64_t;

struct ControlCallOptions {
    std::chrono::milliseconds timeout{5000};
};

// Owning variant of documented result DTOs: SystemInfo, Queue, QueuePage,
// JobDefinition, JobPage, RunDetails, RunPage, AttemptDetails, AttemptPage,
// AttemptOutputChunk, CancelRunReply, SecretMetadata, SecretPage,
// ScheduleValidationReply, ScheduleNextReply, StatisticsPage, and EmptyReply.
using ControlReply = std::variant</* the distinct types listed above */>;

// Owns either a safe local Error or a remote RpcError, with a failure kind
// and a conservative flag for an unobserved mutation outcome.
struct ControlFailure;

class ControlClient final : public jb::core::Object {
public:
    // Constructor borrows rpc and attributes; implementation uses ObjectPrivate.
    [[nodiscard]] auto initialize(ControlCallOptions options = {})
        -> jb::core::Result<void, jb::core::Error>;

    [[nodiscard]] auto create_job(CreateJobRequest const&, ControlCallOptions = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;
    [[nodiscard]] auto list_runs(RunListRequest const&, ControlCallOptions = {})
        -> jb::core::Result<ControlCallId, jb::core::Error>;
    // One correspondingly typed member for every method in §9.1.

    void cancel_call(ControlCallId); // local observation only
    void close();

    jb::core::Signal<SystemInfo> ready;
    jb::core::Signal<ControlCallId, ControlReply> reply_received;
    jb::core::Signal<ControlCallId, ControlFailure> call_failed;
    jb::core::Signal<jb::core::Error> failed;
};
```

This is a contract sketch, not a compilable header to copy literally. Complete constructors, deleted copy/move declarations, destructor, private declaration, all method signatures, and Doxygen during the client stages. Do not expose `std::variant<...>` placeholders in committed public code. Use one `EmptyReply` alternative for all null-result methods; the call ID supplies operation identity.

Typed methods accept the same owning request DTOs/codecs as the server. Add client request encoders and result decoders alongside existing family codecs, avoiding a second handwritten wire format. Do not force applications to assemble raw method strings or inspect private JSON just to use a supported operation. Keep the lower-level `rpc::Client` available for custom methods; this wrapper is not a replacement for it.

### 10.2 Lifecycle and completion contract

State is `Uninitialized`, `Initializing`, `Ready`, or `Closed`/`Failed`. `initialize()` sends one `system.info` request, validates major 1 and the result, caches capabilities, and emits `ready` once. It does not require every Phase 8 capability to be present: an older daemon may serve the methods it advertises. Commands reject a missing required capability locally with `jobu.client.unsupported_method`. Calls before Ready fail locally without being queued or written.

- Successfully accepted typed calls get a monotonically increasing nonzero local ID independent of the wire RequestId; no ID reuse within the instance. Reject exhaustion rather than wrap.
- At most 128 pending typed calls, or the smaller underlying configured limit. Reject excess work without writing a frame.
- Timeouts must be positive and representable; use monotonic deadlines and one earliest-deadline timer. A timeout removes the correlation and reports one local timeout outcome.
- Every accepted call produces exactly one reply or failure unless the wrapper is destroyed. Explicit `close()` fails pending calls once and closes local RPC correlation without closing the borrowed device. Destruction disconnects and cancels local correlations; it emits no new public signals from a partially destroyed object.
- `cancel_call` forgets one call and reports one cancellation outcome through `call_failed`; unknown/already-finished IDs are no-ops. It invokes underlying correlation cancellation only. It never sends `run.cancel` or asserts that remote work stopped.
- An unexpected result shape is `jobu.client.invalid_response`; fail that call. A raw RPC terminal protocol/device error closes the wrapper and fails all pending calls once. Define and test ordering: latch terminal state, clear pending ownership, emit safe terminal `failed`, then per-call failures in local-ID order.
- The client never retries a mutation automatically. A timeout, disconnect, or local cancellation after a request may have been transmitted marks `outcome_unknown` for mutations. A remote represented application error is an observed failure; a known pre-write local validation/capability failure is not an unknown mutation.

Most importantly, the underlying `rpc::Client::call()` can emit a reply synchronously before it returns its RequestId. Reserve local operation state first, then use its per-call acceptance hook to bind the wire ID after a complete write and before buffered raw replies are dispatched. Ignore unrelated raw completions, including chains of reentrant custom calls; the number of sequential completions is not bounded by the simultaneous pending limit. Deliver typed outcomes through the owner event loop so typed reply/failure/ready signals do not precede the accepting public call's return. Do not add nested event processing or an unbounded queue.

Use receiver-aware raw-client and timer connections. Deferred work must carry a lifetime/generation guard so close/destruction makes it harmless. Test synchronous success, synchronous remote error, terminal failure during a write, close from a signal slot, timeout versus response, cancel versus response, and destruction with deferred delivery. A reply must never be misattributed to another method's decoder.

If the underlying call fails after an indeterminate write, preserve that uncertainty in an accepted local failure outcome rather than reducing it to an ordinary preflight validation error. The public immediate `Result` failure is reserved for failure paths known not to have transmitted a request; the local-ID/`ControlFailure` path can represent uncertain writes consistently.

### 10.3 Client example and tests

Provide a small buildable example under `examples/jobu-client/` and explain it in `docs/cpp-client.md`. Use the actual public includes/constructors and normal Application/LocalSocket lifecycle. Demonstrate:

1. Open a local socket, construct raw RPC and typed clients with explicit lifetimes.
2. Connect receiver-aware signals before `initialize()`.
3. Read `system.info`, issue a filtered run list, and follow a continuation.
4. Handle a typed reply, a remote application error, and a local timeout distinctly.
5. Show an idempotent creation example separately with an explicit user-selected key; do not make the default example mutate a running daemon.

A CMake target builds the example without linking concrete database/runner targets. Documentation snippets are extracted from or checked against the buildable example where practical. Do not claim an independently installed SDK/package yet; Phase 9 owns installation/export packaging.

## 11. Refactor jobuctl and complete command UX

### 11.1 Separate parsing from effects

Refactor in two steps: first move the existing behavior into focused files with regression coverage; then introduce hierarchy/help. The first step preserves supported invocations and output as far as practical. Avoid mixing the extraction with all new command behavior.

`ParsedCommand` is an owning variant describing a local help/version action or a typed remote command plus global options. Parsing never connects, reads a database, constructs a runner, resolves a secret, or submits a mutation. Separate bounded file/stdin loading from syntax selection so help can exit before any input I/O.

A static `CommandSpec` registry contains command names/aliases, summary, positional arguments, options, defaults, examples, required RPC capabilities, and the selected private request-builder strategy. Help and dispatch both read this metadata. Do not maintain an unrelated giant usage string that drifts from parsing. Validation not expressible by option metadata remains in the family's builder, with concise help text for mutual exclusions.

Keep `core::CommandLineParser` as the lexical primitive. The CLI layer can parse the selected command path and apply global/local option descriptors without turning the core parser into a JobU-aware registry.

### 11.2 Local help contract

Support all of these without a daemon:

```text
jobuctl --help
jobuctl queue --help
jobuctl queue create --help
jobuctl queue add --help
jobuctl job run-now --help
jobuctl attempt output --help
jobuctl help queue add
```

Canonical existing actions remain `create`; add `add` as a documented alias for queue/job creation. Both aliases have identical parsing, required capability, help content, and execution. `queue add --help` identifies the canonical action and alias. The wire method remains `queue.create`; do not create `queue.add`.

Root help lists groups and global options. Group help lists its actions and short descriptions. Leaf help includes a synopsis, required values, selectors, all local options/defaults, mutual exclusions, shared options, and at least one useful example. Do not overwhelm root help with every leaf flag.

Rules:

- `-h` and `--help` select help at the deepest recognized command path. Missing required operands for that command do not prevent help.
- Root with no arguments shows root help and exits 0. A recognized group with no action shows group help and exits 0. Unknown group/action/option is a syntax error, exit 2, with relevant usage on stderr; adding `--help` does not make an unknown path valid.
- Help is a parsed option, not a raw global search for the string `--help`. `--arg=--help`, an option's explicitly attached value, and tokens after `--` remain data. Missing option values remain lexical errors; do not swallow a help-looking token unpredictably.
- Preserve current support for command arguments beginning with `-`; document `--arg=VALUE` as the unambiguous spelling. Cover current accepted repeated `--arg` forms with regression tests before changing lexer behavior.
- Global options may precede or follow the command path until `--`. After `--`, tokens are positional data for a command that accepts them; otherwise they are invalid extra operands.
- Help/version must not construct Application/LocalSocket, read a request file, read stdin, resolve paths beyond syntax, or validate a live queue/job selector. For example, `secret set --stdin --help` returns immediately without consuming input.
- `--json` does not turn help into a protocol result; explicit help is always local text. Document this so scripts do not expect a JSON help schema.

### 11.3 Global options and process results

| Option | Meaning |
| --- | --- |
| `--socket PATH` | Required for remote commands in Phase 8, as in the current implementation; now accepted before or after the command path. Help/version need no socket. Default/config discovery remains Phase 9 work. |
| `--json` | Exactly one compact JSON result on stdout for a successful remote command |
| `--timeout MILLISECONDS` | Positive overall command deadline, default 5000 ms, including connection, handshake, mutation, and optional wait polling |
| `--help`, `-h` | Local help |
| `--version` | Local executable version at root; no server handshake |

Do not print connection banners, progress lines, or human headings to stdout in JSON mode. Successful null-result methods print `null\n`. Preserve integer precision and explicit null/absent distinctions from the protocol. Errors produce no stdout; with `--json`, stderr contains exactly one object:

```json
{"error":{"kind":"remote","code":"jobu.job.revision_conflict","rpc_code":-32000,"category":"conflict","message":"...safe text...","outcome_unknown":false}}
```

The revision-conflict code/category above match the current management error; use the server's safe message rather than the ellipsis. Fields are always present: `kind` is `local` or `remote`, `code` is a stable string (use a documented generic remote code when application data is absent), `rpc_code` and `category` are nullable, and `outcome_unknown` is boolean. Do not echo the original request or secret input while formatting errors.

| Exit code | Meaning |
| --- | --- |
| 0 | Successful remote operation, or successful local help/version |
| 1 | Represented operation failure, including revision conflict/not-found/unsupported capability |
| 2 | Command syntax, local request/input decoding, or local file input/output error |
| 3 | Connection, protocol, or deadline failure; inspect `outcome_unknown` before retrying a mutation |

An observed remote cancellation result is success even when `disposition` is `requested`; users can request waiting below. A run whose eventual outcome is Failed is still a successful `run.get` query; do not reuse process exit status as an implicit job-outcome code.

### 11.4 Complete command mapping

| CLI command path | RPC or local behavior | Key operands/options |
| --- | --- | --- |
| `system info` | `system.info` | Existing behavior, human/JSON rendering |
| `system stats` | `system.stats` | Window, owner/type/origin filter, grouping, limit/cursor |
| `queue create` / `queue add` | `queue.create` | NAME, weight, concurrency, recovery, defaults, retention, warning, key |
| `queue get` | `queue.get` | Existing mutually exclusive ID/name selectors |
| `queue list` | `queue.list` | Existing include-deleted/state/page options |
| `queue update` | `queue.update` | Selector and explicit changed fields |
| `queue suspend` / `queue resume` / `queue delete` | Corresponding method | Selector; suspend optionally waits |
| `queue stats` | `queue.stats` | Selector plus statistics query options |
| `job create` / `job add` | `job.create` | Queue selector; once/now/cron; type, name, priority, attributes, runner payload, key |
| `job get` / `job list` | Corresponding method | Existing selectors/filters/pages |
| `job update` | `job.update` | ID, required revision, explicit definition changes |
| `job move` | `job.move` | ID, required revision, target queue selector |
| `job suspend` / `job resume` / `job delete` | Corresponding method | Existing operation operands; suspend optionally waits |
| `job run-now` | `job.run_now` | ID and optional idempotency key |
| `run get` / `run cancel` | Corresponding method | Run ID; cancel optionally waits |
| `run list` | `run.list` | Queue/job/state/origin/type, timestamp ranges, limit or cursor |
| `attempt get` | `attempt.get` | Run ID and positive attempt number |
| `attempt list` | `attempt.list` | Run ID and limit, or cursor |
| `attempt output` | `attempt.output` | Run ID, attempt number, channel, offset, limit; optional raw/file output |
| `secret set` | `secret.set` | NAME and exactly one of `--file PATH` or `--stdin` |
| `secret list` | `secret.list` | Limit and after-name |
| `secret delete` | `secret.delete` | NAME |
| `schedule validate` | `schedule.validate` | Expression and optional timezone, default UTC |
| `schedule next` | `schedule.next` | Expression, timezone, required after timestamp, count |

Use hyphenated command/action spellings and existing hyphenated option style. Preserve `--id`, `--name`, `--queue-id`, `--queue-name`, `--revision`, `--after`, and other currently supported flags; new family-specific flags must appear in that command's help. Use `--cursor` exclusively with cursor-based endpoints; do not reinterpret management `--after` IDs as history cursors.

### 11.5 Structured input and complete configuration access

Retain convenient literal CLI/HTTP payload flags and complete the commonly used ones: `--now`, `--at`, `--cron`, `--timezone`, HTTP headers/body, CLI arguments/environment/expected exits, and attributes. Do not implement a separate miniature JSON language for every nested schema.

Every remote command accepts `--request-file FILE` (or `-` for stdin) containing its complete JSON params object. It is mutually exclusive with command-specific request-building operands/options, but compatible with global transport/output options and local waiting/output-delivery options where defined. It uses the exact same strict typed request codec as the public protocol. Help must state this escape hatch explicitly; all attribute/retry/capture/HTTP/template fields must be reachable even when they lack a convenience flag.

Input limits: read at most the configured frame-body bound plus one byte before rejecting; then enforce decoded field/document/raw-secret limits. Reject trailing non-whitespace JSON and non-object params. Do not read arbitrary files just to print help. Never report secret-containing input excerpts in parse errors.

For attributes, support repeated `--attribute NAME=JSON_VALUE` on job create/update and a structured queue-defaults file or `--request-file` for nested defaults. Parse the value with `core::parse_json`, validate against the registry, and preserve update clear/remove semantics. Do not treat the JSON token `null` as equivalent to omission everywhere; use the existing attribute patch contract. Exact convenience flags for queue retention/warning/recovery must match current units and inheritance rules; include examples of inherit, zero/unlimited, and actual duration values.

For secrets, `--file`/`--stdin` reads raw bytes without trimming a trailing newline, then emits base64 wire data. Do not introduce a command-line literal `--value` option. Structured `secret set --request-file` remains available and has the same protection limits. Secret reference creation is available immediately through `job create/update --request-file`; optional convenience `--env-secret NAME=SECRET` may be added only with the same binding rules, not through interpolation.

### 11.6 Waiting and output delivery

`queue suspend --wait`, `job suspend --wait`, and `run cancel --wait` issue the mutation once, then poll the corresponding get method using a non-repeating timer until the requested state is observed or the overall deadline expires. Poll at 100 ms initially, increasing to at most 500 ms; no blocking sleeps or nested loop. Polling does not hold a server transaction.

A timeout after the mutation is an unconfirmed final state, not evidence the mutation failed. JSON success for a waited command is still one result: for suspend, the final suspended object; for cancellation, a `completed` cancellation-shaped result containing the observed terminal run, only if the terminal state is Cancelled. If another terminal state is observed during cancellation reconciliation, report it accurately as a documented operation conflict, never relabel it Cancelled. Read-only polling transport failure sets the command's mutation uncertainty appropriately.

`attempt output` defaults to a human metadata header plus a safe escaped text/base64 preview of the requested chunk. `--json` returns the protocol result exactly. `--raw` writes only decoded raw bytes for that requested chunk and is incompatible with `--json`; it may write binary stdout deliberately. `--output-file PATH` writes that chunk to a newly created file using exclusive creation (an existing path is an input/output error), and is incompatible with JSON/raw-stdout modes. Do not silently concatenate every chunk, append to an existing file, or imply truncated content has been recovered. A future full-download option can be separate; current scripts/client code follow `next_offset` explicitly.

For all human output, escape control characters in names, payload previews, server text, and output previews. Normal administrative reads must not execute terminal escape sequences. Explicit `--raw` is the byte-preserving opt-in.

The session owns socket/raw client/typed client/timer through one ObjectPrivate lifetime if it is an Object. Signal handlers borrow their receiver safely. Mutation submission happens only after handshake/capability checks. Expiring a deadline closes local observation and exits; it must not send a compensating delete/cancel operation.

## 12. Documentation under docs

“Publish protocol documentation” means commit the JSON-RPC wire contract and usage documentation to the repository's `docs` tree. It does not authorize publishing a website or modifying an external documentation service.

Use this structure:

```text
docs/
  README.md
  planning/
    jobu-phase8-code-design.md
  protocol/
    README.md
    transport.md
    types.md
    errors.md
    methods/
      system.md
      queue.md
      job.md
      run.md
      attempt.md
      secret.md
      schedule.md
    examples/
      ...small request/response JSON fixtures...
  jobuctl.md
  cpp-client.md
  secrets.md
```

`docs/README.md` is the user-facing index. Keep planning links separate from the normative protocol entry point. The root repository README gets short links to the docs index, CLI guide, and C++ client guide; do not duplicate complete protocol tables there. Public Doxygen remains in headers as well as these guides.

Each method page specifies:

- capability name, request fields, types, required/optional status, defaults, omission versus null, selectors, integer/range/size limits;
- result fields and nullability, including exact empty-list/void shapes;
- normal validation/not-found/conflict errors and fatal connection behavior;
- revision and idempotency rules, transaction/observation boundary, and how to reconcile lost responses;
- pagination order, live-view/expiry rules, or output encoding/retention semantics;
- at least one valid request/result example and a representative error example;
- corresponding CLI commands and typed client member names.

Document every implemented attribute/default, CLI and HTTP payload shape, secret reference position, retry/capture policy, UTC representation, cron aliases/weekdays/DST behavior, run/attempt lifecycle, and method-specific enum vocabulary. Clearly mark reserved `submitted` as unavailable. Distinguish scheduled-run history from current job definitions.

`transport.md` describes JobU's framing profile and limits based on current code, with the [JSON-RPC 2.0 specification](https://www.jsonrpc.org/specification) and [LSP base-protocol framing](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#baseProtocol) as background references. JobU is not an implementation of LSP application methods. Examples calculate Content-Length in UTF-8 bytes; avoid hand-maintained incorrect lengths for non-ASCII data.

`secrets.md` describes plaintext-at-rest v1 storage, filesystem trust, metadata-only reads, execution-time resolution/rotation, deletion protection, sensitive redirects, safe CLI input, OS argument exposure, and the raw-output boundary. Do not claim a secret vault or reliable erasure/automatic filtering of application-generated output.

Example JSON fixtures are valid, small, and contain fictitious values. Reuse them in codec/CLI tests so docs and implementation cannot silently disagree. Include negative fixtures only when tied to a meaningful contract. Do not introduce a schema-validation package just to duplicate the existing typed decoders. If JSON Schema files are added later, they must be validated against the same fixtures and not become a second unmaintained authority.

Maintain `jobu-phase8-verification.md` as a standalone handoff/evidence artifact following the established workflow. Repository protocol/user documentation belongs under `docs`; external execution evidence can keep its existing artifact location. Record stage, source identity, environment, commands, results, skips, and concrete limitations accurately.

## 13. Required verification cases

The following are acceptance requirements, not a mandate to create a new test executable for every bullet. Extend appropriate existing fixtures/targets, especially transaction-fault, scheduler, capture, RPC, and CLI integration tests.

### 13.1 Secrets and durable state

- Literal payloads remain accepted and execute unchanged; recognized references round-trip through create/update/get, run snapshots, idempotency replay, and restart recovery.
- Invalid reference positions/shapes, invalid names, reference-count limits, and original/resolved size limits are rejected at the correct boundary.
- CLI binary/NUL and HTTP CRLF header misuse fail safely; binary body bytes survive exactly.
- Create/update reference rows and job/run/idempotency changes are atomic, including stale revision and injected rollback/commit failures.
- Deletion is blocked by a current definition and separately by an older nonterminal snapshot after update. Terminal history alone permits deletion. Queue/job deletion cannot bypass a remaining live snapshot.
- An idempotent replay after secret rotation/deletion returns its recorded result without resolving/recreating references.
- Each attempt resolves anew; repeated references in one attempt read once. Rotate between first attempt and retry and observe new bytes only in the second executor request.
- Resolution database failure prevents external start and closes the daemon through Dispatch failure; missing/invalid value produces one safe terminal attempt through the normal durable completion path.
- Failed transaction cleanup after an ordinary new-service error is promoted to fatal before return. A synchronous failure slot delivering a retained completion cannot reopen persistence.
- Sentinel values are absent from stored templates, generated attempt metadata, logs, and protocol errors; original references remain present. Tests deliberately distinguish application-echoed raw output from generated diagnostics.
- Secret-derived headers are removed on cross-origin redirects. Request/response logging cannot leak resolved request data.

### 13.2 History, output, and aggregates

- Equal planned timestamps, page-size boundaries, UUID ordering, and descending attempt numbers give no duplicate/omitted stable rows.
- Combined filters, half-open ranges, null start/completion times, moved jobs, and deleted owners follow snapshot semantics.
- Cursor-only validation, expiry using a fake monotonic clock, eviction, wrong-method use, restart, and repeated-token reads work without offset pagination.
- Changing state between pages follows the documented live-view contract; tests do not demand a frozen snapshot.
- List byte limits return a usable continuation; large existing job-list items remain reachable. Largest single-object responses either fit or return a bounded error without stream failure.
- Output slices preserve arbitrary bytes, zero-length BLOB versus null, split UTF-8, base64, EOF, truncation/omitted accounting, and unavailable/lost capture.
- A small output request does not materialize a large BLOB in application memory; inspect the projection query and use a large fixture with a one-chunk assertion. Do not assert an allocator's incidental call count.
- Statistics use the documented planned-run cohort, avoid join multiplication, handle zero samples and clock reversal, bound group pages, and report runnable wait as unavailable.
- Upgrade fixtures retain durable rows and secrets exactly, reopen correctly, and roll back partial index/version changes.

### 13.3 RPC, typed client, and CLI

- Every required method is registered and advertised once; unavailable methods are not advertised during partial staged implementation.
- Strict request validation and tolerant response decoding remain consistent; no remote errors echo secret input/backend detail.
- Run Now predicates and pending/running cancellation match the existing C++ implementation, including recurrence and capacity behavior.
- Create-now idempotency survives a changed clock and a lost response without duplicate job/run/UUID allocation.
- Typed client handles immediate raw responses, out-of-order replies, write failure, timeout, cancel, close, and receiver destruction exactly once.
- Local help works with no socket, nonexistent request file, closed stdin, and a blocked stdin source. `queue --help` and `queue add --help` are explicit process-level cases.
- Unknown paths/options, missing values, aliases, global option positions, `--`, `--arg=--help`, and existing dash-leading argument forms are deterministic.
- JSON stdout/stderr and exit codes follow §11.3; no progress/handshake text contaminates stdout. Human output escapes control sequences; raw output remains byte-exact.
- `--request-file` reaches full HTTP/CLI/attribute/reference schemas; secret stdin preserves final newlines and binary bytes, with no help-time input read.
- Wait mode sends one mutation, then only reads, and honors the overall monotonic deadline.
- The C++ example builds against public headers and client target; CLI code does not include repositories, concrete drivers, or runner internals.

### 13.4 Linux end-to-end evidence

Use temporary private directories/databases/sockets, local HTTP fixtures, deterministic subprocess fixtures, and existing unprivileged execution helpers. No internet server, production credential, or external side effect is needed.

Exercise a complete flow: create queue; set secret; create once-now and cron jobs; retrieve history; inspect attempts/output; rotate a secret before a retry; invoke Run Now; cancel pending and active runs; suspend/resume; perform stale-revision and idempotency replays; delete references/secret appropriately; stop/restart and verify durable history. Include both normal responses and a deliberately lost mutation response.

Retain Phase 7 shutdown/recovery/fault regressions as gates. New RPC/service paths must not reopen admission during failure or allow execution before a committed claim. Record any root-only skip exactly; do not convert skipped privilege tests into passing evidence.

## 14. Individually reviewable implementation stages

Stages **8.1–8.26** deliver and verify Linux functionality. **8.27** is optional native macOS verification. **8.28** is final clean Linux verification and the phase closure decision.

Every stage that changes public declarations includes Doxygen, standalone-header/include-boundary checks, focused behavior tests, and changed-file diagnostics under the current `AGENTS.md`. Every implementation handoff lists changed files, observed tests/diagnostics, any limitation, and the next proposed stage. Do not run the whole suite after every tiny stage; use it at integration/final gates or to resolve an actual risk.

### Stage 8.1 — Extract existing jobuctl responsibilities

**Implement:** Split current main into parsing/request builders, human rendering, and a private session, preserving current command behavior. Put family handling in separate system/queue/job files. Add an internal target only if needed for direct parser tests. No new RPC methods.

**Verify:** Existing CLI/integration tests; focused parity cases for selectors, repeated arguments, negative/dash-leading values, revisions, socket override, handshake, and timeout. `main.cpp` has orchestration rather than command-specific formatting/large request branches. Audit any new Object's one-private-block ownership.

**Exit:** Existing users can issue the same commands; subsequent command additions have clear owning files.

### Stage 8.2 — Command registry and hierarchical local help

**Implement:** Registry-driven group/leaf parsing/help, root/group defaults, `help` path, `-h`, queue/job `add` aliases, and lexical help rules. Register currently implemented commands; later stages add their own entries.

**Verify:** Root/group/leaf/alias help without a daemon; missing required operands, invalid paths/options, help-looking argument data, `--`, and no input-file/stdin access. Prove `queue --help` and `queue add --help` at executable level.

**Exit:** Help and dispatch share command metadata; help does not require Application/socket construction.

### Stage 8.3 — Index-only application schema upgrade

**Implement:** §7 version 2 manifest, explicit v1 upgrade, SchemaStatus/Doxygen, private fault seam reuse, and startup integration.

**Verify:** Fresh/upgrade/reopen, populated preserved rows, bad/newer schema, incorrect same-name indexes, DDL/marker/commit/rollback faults. Run existing schema and recovery startup tests.

**Exit:** Current data upgrades atomically; SQLite schema policy stays in `jobu-sqlite`.

### Stage 8.4 — Secret template grammar and shared validation

**Implement:** Reference parsing/extraction, allowed positions, template/concrete validation split, and new validation reason mapping. Update storage/recovery/idempotency decoders to accept valid templates structurally. Do not resolve bytes yet.

**Verify:** Literal regressions, malformed/unsupported references, pointer escaping, limits, all literal validations, and stored template/recovery round trips without provider calls.

**Exit:** One definition of template validity is used throughout persistence and execution preparation.

### Stage 8.5 — Secret metadata/write service

**Implement:** Public request/page types, checked set/list/delete service, raw-value lookup private seam, value limits, and ObjectPrivate/failure gates. Wire the new service's failure/shutdown participation into runtime composition before advertising RPC methods.

**Verify:** Binary/empty/upsert metadata semantics, metadata-only SELECT, name/value/page limits, missing/in-use errors, mutation signal ordering, storage faults, and post-rollback poisoning.

**Exit:** Secret service operations are safe independently of transport; no public value-read endpoint exists.

### Stage 8.6 — Transactional references and complete deletion protection

**Implement:** Same-transaction reference maintenance in job create/update/delete and existing queue deletion. Complete secret deletion's bounded nonterminal-snapshot scan.

**Verify:** Definition updates/old snapshots, terminal versus nonterminal ownership, stale revisions, missing names, idempotent replay, malformed durable data, and injected failures at every new transaction boundary. No partial reference replacement persists.

**Exit:** Current definitions and older executable snapshots both prevent unsafe deletion.

### Stage 8.7 — Trusted provider and transient concrete payload preparation

**Implement:** Provider interface/database implementation and pure preparation helper with per-attempt name caching, concrete decoding, sensitive-header marking, and byte/size/error contracts. Document synchronous borrowing and the future-provider limitation.

**Verify:** Repeated-name resolution, text/binary destinations, UTF-8/NUL/CRLF, size expansion, sensitive flag override, safe reason codes, and no mutation of original template.

**Exit:** A validated template can become a bounded transient executor payload without persisting resolved data.

### Stage 8.8 — Scheduler resolution and recovery integration

**Implement:** Required provider injection into scheduler/core/dispatch/runtime and fake fixtures; resolution ordering and immediate preparation failure path; confirm recovery reads templates without resolving.

**Verify:** Resolve on each retry, literal jobs perform no lookup, provider DB errors fail Dispatch, malformed stored templates fail closed, preparation errors complete once, rollback poison is detected, and no external start precedes commit. Run targeted Phase 7 transaction/shutdown tests.

**Exit:** Real scheduler dispatch uses references safely and retains all durable-start/fatal boundaries.

### Stage 8.9 — Real-runner redaction and rotation verification

**Implement:** Only concrete redaction/sensitive-header corrections proven necessary in CLI/HTTP runner diagnostics; add local fixtures exercising resolved execution. Begin `docs/secrets.md` from the implemented contract.

**Verify:** CLI environment/argument and HTTP header/body resolution, redirect removal, rotation before retry, sentinel scan of generated diagnostics/durable metadata, and explicit raw-output echo behavior.

**Exit:** End-to-end secret use works without undocumented generated-data leaks or false raw-output guarantees.

### Stage 8.10 — Immediate creation and idempotency completeness

**Implement:** Creation-only ImmediateSchedule/`at:"now"`, canonical symbolic request handling, concrete persisted schedule, request/result codecs, replay/recovery validation.

**Verify:** Same key after clock change/lost response yields the original resource and timestamp; mismatched key/request conflicts; no extra identities/reference writes on replay; update rejects now; all existing create/Run Now revision/idempotency tests pass.

**Exit:** Applications can submit due-now once jobs with safe retryable request identity.

### Stage 8.11 — History DTOs and bounded cursor store

**Implement:** Public history/filter/page types, shared codecs for their scalar types, private typed cursor store with method binding, fixed TTL, and size cap. Introduce read-service lifetime/failure skeleton.

**Verify:** Filter ranges/enums, summaries exclude heavy fields, fake-clock expiry, eviction, wrong-method tokens, retryable input cursors, and continuation request exclusivity.

**Exit:** The pagination contract is deterministic before SQL integration.

### Stage 8.12 — Run and attempt history queries

**Implement:** Get/detail and summary-page projections, filters/keyset ordering, deleted-owner visibility, service read/fatal handling, and result byte budgets. Apply safe page truncation/continuations to existing management lists without changing their cursor format.

**Verify:** Equal timestamps, moved jobs, combined/null filters, descending attempts, large job-list payloads, live updates, limits, not-found, malformed stored rows, and generated SQL/query plans using the version-2 indexes.

**Exit:** Bounded history and legacy management pages are complete and do not fetch output.

### Stage 8.13 — Separate sliced output API

**Implement:** Channel mapping, projected BLOB slice query, typed chunk/status/metadata, UTF-8/base64 conversion, checked offsets, and capture availability normalization.

**Verify:** Large BLOB/small slice, split UTF-8, binary/empty/null/lost capture, interrupted recovery, truncation, metadata consistency, offsets/EOF, channel mismatch, and no full-output materialization.

**Exit:** All retained CLI/HTTP channels can be fetched incrementally with honest availability/encoding metadata.

### Stage 8.14 — Retained-data statistics

**Implement:** Request/result DTOs, planned-cohort aggregates, grouping/cursors, read service, runtime failure gate, and explicit unavailable runnable-wait metrics. Update the Phase 8/9 roadmap split.

**Verify:** Retry join multiplication, group pagination, window defaults/limits, deleted/moved owners, no-data/null samples, negative wall deltas, overflow handling, and capture flag counts. Record representative query plans/cost observations.

**Exit:** `system.stats`/`queue.stats` have implementable, honest results without Phase 9 measurement machinery.

### Stage 8.15 — Run controls and schedule RPC methods

**Implement:** Run Now/cancel/cron preview request/result codecs and registration. Call existing management/scheduler/cron services, apply response limits, integrate capabilities for these methods.

**Verify:** Run Now manual barriers/idempotency, pending/active cancellation and fatal rollback, schedule aliases/cyclic weekdays/timezones/DST, count/range errors, and safe structured RPC errors.

**Exit:** These controls are public through the protocol without duplicate scheduling logic.

### Stage 8.16 — Secret RPC methods

**Implement:** Metadata-only secret codec/registration, strict base64 input/raw-size limits, and corresponding method documentation/examples.

**Verify:** Set/list/delete round trips, empty/binary values, invalid encoding, oversized input, no value/digest echo, deletion protection, service shutdown/admission, and exact capabilities.

**Exit:** Named secrets can be administered entirely through public RPC.

### Stage 8.17 — History/statistics RPC and complete capabilities

**Implement:** Run/attempt/output/statistics registrations and full response decoders/encoders, completed method-name inventory, and the API 1.3 minor stamp.

**Verify:** Every §9.1 method is registered/advertised once; fixtures round-trip; output/pages stay within frame bounds; cursor-only resumes and statistics selectors work; fatal read errors close admission correctly. Check no adapter selects secret bytes.

**Exit:** The complete Phase 8 server protocol is available.

### Stage 8.18 — Typed client lifecycle and correlation

**Implement:** `jobu-client` target, ControlClient ObjectPrivate, handshake/capabilities, local/wire correlation through the raw acceptance hook, deferred synchronous responses, bounded pending calls, monotonic deadlines, cancel/close/failure ordering. Initially exercise representative info/read/mutation methods.

**Verify:** In-memory synchronous responses, out-of-order replies, immediate/partial write failure, timeout/cancel races, destruction, reentrant cleanup, unsupported capabilities, and no concrete driver/runner dependency.

**Exit:** Client lifetime and failure semantics are proven before adding every typed method.

### Stage 8.19 — Complete typed client method coverage

**Implement:** Typed members for every method in §9.1 using shared codecs, distinct result variants, complete public contracts, and the buildable read-only client example.

**Verify:** Method-to-request/result mapping, remote versus local errors, nullable/void results, large output/page responses, unknown response fields, and example/public-include build.

**Exit:** Applications need no private APIs or handwritten JSON for supported workflows.

### Stage 8.20 — CLI typed session and machine output

**Implement:** Switch extracted session to ControlClient, overall deadline, global JSON option and exit/error schema, human control-character escaping, and shared bounded request-file loader. Keep all existing command handlers functional.

**Verify:** Existing commands via real local RPC, handshake/capability failures, one JSON stdout value, errors on stderr, integer fidelity, safe errors, timeout uncertainty, and help still bypassing session/input I/O.

**Exit:** CLI transport/output behavior is consistent before command completion.

### Stage 8.21 — Complete queue/job command inputs

**Implement:** Add now/cron/timezone/attribute/configuration options, full typed `--request-file` access, current revision/null/default semantics, and documented aliases. Add queue/job suspend wait behavior using shared session polling.

**Verify:** Both runner request shapes including secret references, configuration inheritance, incompatible options, stale revision behavior, immediate-create replay, alias equivalence, and one-mutation wait polling.

**Exit:** Full definition and queue administration can be expressed through CLI without editing the database.

### Stage 8.22 — Run and attempt CLI commands

**Implement:** Run Now, run get/list/cancel/wait, attempt get/list/output, cursor/filter options, raw/file chunk delivery, and command-specific help/examples.

**Verify:** Pending/active cancellation, wait uncertainty, filter/cursor exclusivity, binary stdout/file bytes, non-overwriting file behavior, explicit truncation metadata, and no output in summaries.

**Exit:** Execution control and history/output inspection are complete at the CLI.

### Stage 8.23 — Secret CLI commands

**Implement:** Set from file/stdin, list/delete, structured input path, raw-byte limits, safe error messages, and leaf help. No literal secret-value option.

**Verify:** Binary input/trailing newline, empty/oversized values, blocked stdin plus help, no input echo, rotation, and deletion conflict. Inspect process command line for the fixture to ensure bytes were passed through stdin/file rather than argv.

**Exit:** Secret administration is usable with documented input/exposure behavior.

### Stage 8.24 — Statistics and schedule CLI commands

**Implement:** System/queue statistics flags/grouping/cursor behavior, cron validate/next, and human/JSON output. Complete the registry-to-method coverage inventory.

**Verify:** Time windows/defaults/cursors, null measurement rendering, cyclic weekday examples, UTC/timezone preview, and help for every group/leaf/alias. Compare advertised capability requirements with actual RPC methods.

**Exit:** Every public method has a documented CLI route and full request-file access.

### Stage 8.25 — Protocol, CLI, and client documentation audit

**Implement:** Finish §12 document tree, root links, all method/type/error/limit tables, client usage, secret caveats, examples, and roadmap links/status. Consolidate per-stage documentation rather than rewriting it from memory.

**Verify:** JSON fixtures through production codecs, buildable client example, command/help inventory, working relative links, UTF-8 Content-Length examples, and no accidental documentation outside `docs` except established verification artifacts/public Doxygen.

**Exit:** A third-party client author can implement the protocol and a user can discover each CLI operation without reading implementation files.

### Stage 8.26 — Complete Linux workflow and failure integration

**Implement:** Focused missing integration/fault fixtures; only fix defects within this phase's contracts. Run §13.4 workflow using real local transport, SQLite, CLI and HTTP fixtures, plus lost-response/restart/fatal-admission cases.

**Verify:** Full registered Linux suite on the assembled implementation; document actual results, environment, root-only skips, and public-contract audit. No external service/credential is needed.

**Exit:** Linux functional/failure evidence is complete, with any remaining defect explicitly resolved before optional platform/final stages.

### Stage 8.27 — Optional native macOS verification

**Implement:** No new feature or platform backend is planned. Run native configure/build, relevant client/help/socket/secret/history/schema tests, and the assembled suite where available. Correct only demonstrated shared-source portability defects within an approved stage.

**Verify:** Native source identity/environment and actual commands; distinguish build-only from executed tests. Linux results do not establish native macOS behavior.

**Exit:** Record native evidence or an explicit user-approved skip. An approved skip is not a Linux closure blocker and does not reopen Phase 7 evidence.

### Stage 8.28 — Final clean Linux verification and closure

**Implement:** Documentation/evidence closure only unless a concrete failed gate requires a separately reviewed correction. Refresh the final merged implementation and verify from a fresh build directory.

**Verify:** Full configure/build/CTest, final method/help/client/docs consistency, largest-response and secret/fatal invariant coverage from prior stages, changed/public-header diagnostics as required by AGENTS, and source provenance. Update the roadmap's Phase 8 status and the standalone verification record truthfully.

**Exit:** All §15 criteria pass; optional macOS status is explicit; Phase 9 is the next planning boundary.

## 15. Build policy and final acceptance

Normal implementation build:

```sh
cmake -S . -B .bld -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld
ctest --test-dir .bld/test --output-on-failure
```

For final verification, choose a genuinely fresh directory such as `.bld-phase8-final` and use the same SQLite-enabled normal configuration:

```sh
cmake -S . -B .bld-phase8-final -DCMAKE_BUILD_TYPE=Debug
cmake --build .bld-phase8-final
ctest --test-dir .bld-phase8-final/test --output-on-failure
```

Verify the configured SQLite driver/application targets are enabled rather than assuming a reused cache is correct. Record compiler, dependency versions, configuration, source identity, exact registered test count/results, and skips. Use repository formatting and changed-file diagnostics rules.

Do **not** make a full `JB_BUILD_SQLITE_DRIVER=OFF` build-and-test run a routine or final Phase 8 gate. A targeted configure/compile-only boundary check may be useful after the new `jobu-client` CMake work to prove it does not require concrete SQLite/runner targets; perform it only for that concrete dependency risk and record its limited purpose. It is not a second required full test matrix.

Phase 8 closes when:

1. Every listed v1 method plus `attempt.list` is implemented, advertised, strictly decoded, safely encoded, and documented.
2. Named references remain durable templates, resolve freshly per attempt, protect old nonterminal snapshots from deletion, and never enter generated public metadata as resolved values.
3. No resolution/storage/cleanup failure bypasses Phase 7 failure gates or permits external execution before durable Running state.
4. Create/Run Now idempotency, symbolic-now replay, optimistic revisions, cancellation, suspension, recurrence, and restart semantics remain correct through RPC.
5. History filters/cursors and separate output slicing are bounded and accurate; statistics identify unmeasured runnable wait explicitly.
6. `jobuctl` is organized by responsibility and command family; root/group/leaf/alias help works locally; all supported requests are reachable; JSON and exit behavior are stable.
7. The typed C++ client uses public APIs, safe ObjectPrivate/signal lifetimes, correct synchronous-response correlation, and no concrete backend/runner dependency.
8. All protocol/user/client documentation is under `docs`, with validated examples and links.
9. The fresh Linux build/full test suite and public-contract audit pass, and platform evidence/skips are recorded honestly.

Phase 9 then adds retention cleanup, measured runnable-wait/delay warnings and remaining observability, final configuration/security/installation packaging, and operational documentation. No extra Phase 8 closure work is implied merely because those later features remain unimplemented.
