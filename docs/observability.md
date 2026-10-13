# Observing JobU

Use `jobuctl system info` for reachability/capabilities, retained statistics for
history, and stderr events for lifecycle and maintenance diagnostics. The
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
Structured events also supply the event name as the message for legacy sinks;
formatted records have an empty event. Field order is not a contract. For example:

```json
{"time":"2026-10-13T10:00:00.000Z","level":"INFO","event":"jobud.ready","message":"jobud.ready","thread":"example","fields":{}}
```

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
| `jobud.recovery.completed` | INFO | `interrupted_attempts`, `retrying_runs`, `terminal_runs`, `finished_jobs`, `inserted_successors`, `suspended_jobs`, `suspended_queues`, `repaired_timing_rows`. |
| `jobud.run.delayed` | WARN | `run_id`, `job_id`, `queue_id`, `type` (`cli`/`http`), `runnable_wait_us`, `threshold_ms`, `measurement="monotonic_observed"`. |
| `jobud.retention.completed` | INFO | Completed-sweep totals: `runs`, `idempotency_records`, `jobs`, `queues`. No per-run log stream. |
| `jobud.failed` | ERROR | `subsystem`, stable `code`; selected startup filesystem failures also have a fixed `reason`. Fatal runtime failure closes admission. |
| `jobud.rpc.admission_failed`, `jobud.rpc.connection_failed`, `jobud.listener.accept_failed` | ERROR | `subsystem`, `code`; connection/admission diagnostics, not proof of whole-runtime failure. |
| `jobud.signal.cleanup_failed`, `jobud.signal.watch_cleanup_failed` | ERROR | `code` for signal-relay cleanup; main's explicit cleanup report also has `subsystem`. Destructor fallback reports carry only `code`. |
| `jobud.log.encoding_failed` | ERROR | Empty fields and a fixed fallback message if record encoding unexpectedly fails. |

Events intentionally omit job names, commands, arguments, environment, HTTP
URLs/headers, payloads, secret bytes and raw SQL/backend errors. IDs correlate
records without exposing execution content. Early argument/configuration errors
are safe stderr diagnostics before logger installation; do not require every
process output line to be JSON. Captured job output is a separate sensitive data
surface; see [secrets](secrets.md).

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
