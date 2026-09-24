# System methods

`system.info` returns the daemon version, API version, and sorted unique names
of registered methods. The complete Phase 8 server surface uses API 1.3.
Clients should use the major version and advertised capabilities to decide
which methods they can call.

## Retained statistics (`system.stats`)

An initial object may contain `queue_id`, `job_id`, `type` (`cli`/`http`),
`origin` (`scheduled`/`manual`), `planned`, `group_by`, and `limit`. A `planned`
object has optional inclusive `from` and exclusive `to` UTC timestamps. The
server defaults `to` to its current UTC time and `from` to 24 hours earlier.
The resolved window must be increasing and at most 31 days. `group_by` defaults
to `none` and accepts `queue`, `job`, `type`, `origin`, or `state`; `limit`
defaults to 100 and accepts 1–200.

```json
{"planned":{"from":"2026-01-01T00:00:00Z","to":"2026-01-02T00:00:00Z"},"group_by":"queue","limit":2}
```

The result has resolved `window`, `group_by`, `groups`, nullable `next_cursor`,
and `measurement`. Each group has a nullable `key`; `runs` with `total`, fixed
`states`, `types`, and `origins` counters; `attempts` with `total`, fixed
`states` and `outcomes` counters plus `retries`; `capture` with
`truncated_attempts` and `lost_attempts`; `schedule_lateness_ms` and
`execution_wall_duration_ms` with `samples`, nullable `average`, and nullable
`maximum`; and `runnable_wait_ms:null`. For example, an empty cohort with
`group_by:none` still has one zero-count group with `key:null`.

Runs enter the cohort by immutable planned time. Counts include all their
retained attempts, even retries outside the planned window. Lateness and
execution duration are derived from wall-clock timestamps; samples with
missing, negative, or unrepresentable differences are excluded. The
`measurement` object reports `timing:"wall_clock_derived"`,
`runnable_wait:"unavailable"`, and `capture:"persisted_output_flags"`.
Absent output rows do not assert successful capture.

Groups sort by UUID bytes or enum wire spelling. Resume with only
`{"cursor":"<opaque token>"}`. The resolved window and filters stay bound to
the cursor, but retained counts remain a live view. Tokens expire five minutes
after the initial page and can disappear earlier through eviction or restart.
An invalid cursor returns `jobu.statistics.invalid_cursor`.

Malformed request fields return JSON-RPC `-32602`; represented service errors
use application `-32000` with safe `data:{category,code}`. Fatal persisted-data
or storage errors close daemon read admission. `system stats` CLI and typed
client access are added in later Phase 8 stages.
