# Observing JobU

Use `jobuctl system info` for reachability/capabilities, retained statistics for
history, and stderr events for startup, execution and maintenance diagnostics. The
[CLI guide](jobuctl.md#retained-statistics) shows statistics commands; the
[protocol types](protocol/types.md#statistics-page) define the response schema.

## Logs and event fields

Defaults are JSON and level `info`. Each accepted record is one complete stderr
line. `logging.format=text` carries the same safe fields in readable text.
Levels are configured as `fatal`, `error`, `warning`, `info`, `debug1`, `debug2`,
`debug3`, including all more severe levels. Serialized names are `FATAL`,
`ERROR`, `WARN`, `INFO`, `DBG1`, `DBG2`, `DBG3` respectively.

The JSON envelope has `time` (UTC with milliseconds, or `unavailable`), `level`,
`event`, `message`, `thread` (opaque thread identity string), and nested `fields`.
`event` is the stable machine identifier; `message` is human text. Legacy
`log_event()` calls still use the identifier as their message, including
`jobud.ready`. `log_event_message()` permits independent explanatory text, used
for inventory, maintenance, path and execution diagnostics. Formatted records
have an empty event. Field order and English wording are not machine contracts.
For example, after loading one queue and two future jobs:

```json
{
  "time": "2026-10-16T10:00:00.000Z",
  "level": "INFO",
  "event": "jobud.inventory.loaded",
  "message": "Loaded 1 queue, 2 active job definitions and 2 scheduled runs.",
  "thread": "example",
  "fields": {
    "queues_active": 1, "queues_suspending": 0, "queues_suspended": 0,
    "jobs_active": 2, "jobs_suspending": 0, "jobs_suspended": 0,
    "runs_scheduled": 2, "runs_retry_wait": 0
  }
}
```

The example is expanded for reading; the sink writes each record on one line.

Duplicate field names retain the first value; malformed UTF-8 becomes U+FFFD,
nonfinite numeric values become null, and newlines are escaped. Synchronous
logger calls consume borrowed fields before returning; custom asynchronous sinks
must copy. Writes are serialized across threads. Write errors do not recursively
log or throw; a failed output can lose a record. The process's existing SIGPIPE
policy still applies to a broken stderr pipe—keep the sink available.

| Event | Level | Fields and interpretation |
| --- | --- | --- |
| `jobud.starting` | INFO | No fields; runtime startup has begun, not readiness. |
| `jobud.ready` | INFO | No fields; listener and runtime services are ready. |
| `jobud.stopped` | INFO | `exit_status`. |
| `jobud.unsafe.root_daemon`, `jobud.unsafe.root_cli` | WARN | No fields; independent unsafe root permissions in use. |
| `jobud.recovery.completed` | INFO if any repair count is nonzero, otherwise DBG2 | `interrupted_attempts`, `retrying_runs`, `terminal_runs`, `finished_jobs`, `inserted_successors`, `suspended_jobs`, `suspended_queues`, `repaired_timing_rows`. |
| `jobud.inventory.loaded` | INFO | Once per successful post-recovery snapshot; fixed counters shown above. Loaded state, not readiness or dispatch eligibility. |
| `jobud.run.delayed` | WARN | `run_id`, `job_id`, `queue_id`, `type` (`cli`/`http`), `runnable_wait_us`, `threshold_ms`, `measurement="monotonic_observed"`. |
| `jobud.retention.completed` | INFO if any purge count is nonzero, otherwise DBG2 | Completed-sweep totals: `runs`, `idempotency_records`, `jobs`, `queues`. Retention reports summary counts rather than individual deletions. |
| `jobud.failed` | ERROR | `subsystem`, stable `code`; startup filesystem failures can also carry typed path context described below. Fatal runtime failure closes admission. |
| `jobud.rpc.admission_failed`, `jobud.rpc.connection_failed`, `jobud.listener.accept_failed` | ERROR | `subsystem`, `code`; connection/admission diagnostics, not proof of whole-runtime failure. |
| `jobud.signal.cleanup_failed`, `jobud.signal.watch_cleanup_failed` | ERROR | `code` for signal-relay cleanup; main's explicit cleanup report also has `subsystem`. Destructor fallback reports carry only `code`. |
| `jobud.log.encoding_failed` | ERROR | Empty fields and a fixed fallback message if record encoding unexpectedly fails. |

Events intentionally omit job names, commands, arguments, environment, HTTP
URLs/headers, payloads, output, secret bytes and free-form runner/SQL/backend
diagnostics. IDs correlate
records without exposing execution content. Early argument/configuration errors
are safe stderr diagnostics before logger installation; do not require every
process output line to be JSON. Captured job output is a separate sensitive data
surface; see [secrets](secrets.md).

Filesystem diagnostics may include approved operational paths: `resource`, `path`,
optional `configured_path` when spelling differs, `reason`, `action`, and available
`actual_mode`/`required_mode`, `actual_uid`/`required_uid`,
`actual_gid`/`required_gid` and `errno`. Modes are strings such as `0700`; IDs and
errno are numeric. The path identifies the actual failing component or artifact,
using metadata from the check rather than a later inspection. Unknown metadata
is absent. Path text is escaped in both pre-logger diagnostics and the configured
sink. See [troubleshooting](operations.md#format-rejection-recovery-and-troubleshooting)
for correction examples; configuration contents and rejected values remain excluded.

## Inventory and quiet maintenance

Recovery reports repairs, then inventory reports a consistent persisted-state
snapshot before runner construction and first dispatch. `jobud.ready` follows at
the listening/services boundary. A later startup failure can follow a truthful
inventory without ever advertising readiness.

Queue counters exclude Deleted rows. Job counters describe live definitions,
excluding terminal one-time definitions and Deleted rows. `runs_scheduled` and
`runs_retry_wait` count persisted pending states, including future, suspended,
blocked or unavailable-runner work; neither count means immediately runnable.
Terminal run history is excluded. Suspended/suspending and retry-wait counts also
appear in the message when nonzero; all eight fields are always present.

At configured `info`, startup/inventory/readiness and meaningful nonempty
maintenance remain visible, along with warnings/errors. Configured `debug1` adds
ordinary execution progress. Empty recovery and retention reports remain `DBG2`,
so neither `info` nor `debug1` restores periodic all-zero chatter. Maintenance
frequency and public retention notifications do not depend on the logging level.

## Execution events

Every execution record carries `queue_id`, `job_id`, `run_id`, `type`
(`cli`/`http`) and `origin` (`scheduled`/`manual`/`submitted`). Correlate a run by
`run_id` and an attempt by that ID plus `attempt_number`.

| Event | Serialized level | Additional fields and meaning |
| --- | --- | --- |
| `jobud.attempt.started` | DBG1 | `attempt_number`, `started_at`; the executor accepted work after its durable claim. Child exec or HTTP connection establishment may still be pending. |
| `jobud.attempt.succeeded` | DBG1 | `attempt_number`, `completed_at`; successful effective outcome committed. |
| `jobud.attempt.failed` | DBG1 | `attempt_number`, `completed_at`, `start_phase` (`accepted`, `preparation_failed`, `executor_rejected`); retry or terminal outcome follows separately. |
| `jobud.attempt.cancelled` | DBG1 | `attempt_number`, `completed_at`; cancellation-normalized attempt completion committed. |
| `jobud.run.retry_scheduled` | DBG1 | `completed_attempt`, `next_attempt`, `runnable_at`, `retry_mode` (`blocking`/`reschedule`); the actual persisted due time, including jitter/Retry-After decisions. |
| `jobud.run.succeeded` | DBG1 | `completed_at`, optional `completing_attempt`; successful terminal run committed. |
| `jobud.run.failed` | WARN | `completed_at`, optional `completing_attempt`; terminal failure committed with no further retry. |
| `jobud.run.cancelled` | DBG1 | `completed_at`, optional `completing_attempt`; terminal cancellation committed. |

Finished attempt events may also carry `elapsed_ms`, `exit_code`, `signal_number`
and `http_status`. Unknown/unavailable optional values are omitted, never
invented as zero. Numeric runner metadata is type/range checked; arbitrary result
fields are not forwarded. The scheduler's effective outcome is authoritative:
an expected exit code 37 or configured HTTP 404 success still produces success.

`started_at`, `completed_at` and `runnable_at` use UTC with six fractional digits;
the envelope's `time` uses milliseconds. `elapsed_ms` measures local monotonic
time from executor acceptance to completion-handler entry, excluding completion
SQL. It is diagnostic attempt elapsed time, distinct from runnable wait, total
run time, wall-clock statistics, or exact child/HTTP transport duration. It is
absent for preparation/start rejection or a negative/unrepresentable clock difference.

For an accepted attempt, started precedes committed completion. A committed
completion emits attempt-finished followed by exactly one retry or terminal run
event. A failed attempt with a scheduled retry stays `DBG1` and produces no
terminal failure warning; an exhausted/terminal Failed run produces one `WARN`. Retry scheduling
does not mean another attempt has started. Preparation failure and executor
rejection produce no started event. Cancelling pending work produces only a run
cancellation record, with no `completing_attempt`; earlier attempts may still
exist. Active cancellation waits for normalized completion. Unconfirmed commits
produce no completion/retry/terminal diagnostics for that transition.

The four public Scheduler signals carry owning values on its owner thread.
Direct observers may inspect committed state, request a rescan or terminal
shutdown. Defer scheduler destruction, recursive start/cancel, management
mutation or injected completions by posting to the owner loop; they must not
occur during delivery.
Connect with an Object receiver, and copy arguments retained after direct
delivery. Subscribe before the first synchronous `start()` dispatch.

These are lossy live diagnostics, with no replay, durable subscription or complete
audit guarantee. Startup recovery uses its report and does not fabricate live
Interrupted events; management deletion/suspension is outside this stream.
Filtering, stderr failure, a crash or terminal shutdown can lose records. JSON
and text carry the same event identity, safe fields and human message. Runtime
infrastructure failures continue through the existing ERROR/fatal-runtime path.

## Log collection

JobU owns no log file or rotation service. With systemd, inspect the journal:

```sh
sudo journalctl -u jobud.service -n 50 --no-pager
sudo journalctl -u jobud.service -f
```

Configure journal retention/access in the host's logging policy. For foreground
redirection, create a protected destination and arrange external rotation, or
stop, rotate and restart deliberately. Redirection alone does not cap disk use.
There is no Prometheus/OTel endpoint or persisted statistics rollup.

## Runnable wait and coverage

Runnable wait sums **observed eligible monotonic intervals** per run across its
attempts. Capacity/fairness wait counts, including a due blocking retry waiting
for a runner. Future due time, retry backoff, queue suspension, automatic-job
suspension, manual barriers, unavailable executor types, execution time and daemon
downtime do not count. Accepted manual work preserves its existing job-suspension
bypass while still respecting queue eligibility.

Due eligibility begins when the scheduling pass observes it, not at a fabricated
monotonic crossing inferred from UTC. A wall-clock correction can change due
eligibility, but accumulated durations use the monotonic clock. State changes
settle/reconcile their affected intervals in the same transaction. This is why
the provenance is `monotonic_observed` rather than exact UTC-derived queue time.

| Coverage bucket | Meaning |
| --- | --- |
| `complete` | Terminal run observed from creation without a lost eligible interval. Included in runnable-wait duration aggregates. |
| `partial` | Terminal run with a lost open tail after a crash or instrumentation beginning late; known wait is only a lower bound. |
| `unmeasured` | Terminal run created/used without telemetry, such as an embedded caller. |
| `unfinished` | Any nonterminal run, classified here before measurement quality. |

The four mutually exclusive counts sum to the group's run count. Only complete
terminal runs contribute `runnable_wait_ms.samples`, `average`, `maximum`.
Zero samples produce null average/maximum, not zero latency. An entirely
uninstrumented embedded service can report unavailable provenance and a null
duration. Other timing measures retain `wall_clock_derived` provenance:
schedule lateness and execution wall duration have different scopes from wait.

Healthy shutdown closes intervals at its captured stop time. Crash recovery
preserves checkpointed sums, clears abandoned epochs and marks affected rows
partial; it neither reconstructs the missing tail nor adds downtime. Closed
complete rows remain complete across a clean restart. Partial never becomes
complete again. A checkpoint sweep can exceed its configured interval, so 30s
is not a guaranteed upper bound on measurement loss.

Logs express delayed wait in **microseconds** and thresholds in **milliseconds**;
statistics durations are **milliseconds**. Do not compare those raw numbers
without conversion.

## Delay warning guarantee

Queue `runnable_wait_warning` defaults to 10,000 ms. Zero disables warnings while
measurement continues. A warning is earned when accumulated known wait is
**strictly greater** than a positive threshold and the run is eligible. Lowering
a threshold can make the next observation warn. Equality does not warn.

The durable claim commits before signal/log delivery. One run claims at most
one warning across retries, moves, restarts and policy changes. A crash/stop
between claim and delivery, or log filtering/output failure, can lose the warning;
delivery is not guaranteed. Changing log level or threshold does not reset the
claim. A partial run's warning still reports a known lower bound, not complete
latency. A completed claim can be delivered immediately around dispatch, so
inspect current run state rather than assuming it is still waiting.

## Retained statistics and scale limits

Statistics describe a live view of retained rows in the existing planned-time
cohort, with the established grouping/cursor/response limits. They do not create
checkpoints or write timing rows. Retention or other mutations can change the
cohort between pages; this is not a historical snapshot. Attempt/output joins
must not be interpreted as additional run samples. See
[system statistics](protocol/methods/system.md) and
[queue statistics](protocol/methods/queue.md).

Scheduler cycles, backlog observation, affected-scope telemetry reconciliation,
recovery validation and selected-cohort statistics are synchronous and depend on
population size. Keyset pages limit projections, not the total work in a cycle.
Checkpoint callbacks yield between pages; retention parent limits exclude
reference validation and cascaded attempts/output/timing work.

The retained native Linux scale gate exercised 100 queues, 10,000 pending runs
and 100,000 terminal runs, plus separately sized fairness/cascade/fault cases.
Its costs are observations in the recorded environment, not a universal latency
or throughput promise. Maintenance crash tests killed synchronized helpers using
production repositories, SQLite and recovery; installed-daemon progress and
healthy stop were separate checks. These results do not establish power-loss,
boot, escaped-session cleanup, sanitizer or native macOS coverage. Exact commands,
producer revisions and measurements live in the separate Phase 9 verification
record, not in the installed manuals.
