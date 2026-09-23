# Job creation schedules and idempotency

`job.create` accepts a creation-only immediate once schedule:

```json
{
  "queue_name": "default",
  "type": "cli",
  "schedule": {"kind": "once", "at": "now"},
  "payload": {"command": "/usr/bin/true"},
  "idempotency_key": "example-submit-1"
}
```

These are JSON-RPC params. The queue must already exist. `queue_id` can replace
`queue_name`; exactly one selector is required. The schedule and payload are
required. Existing explicit UTC once timestamps and cron schedules remain
supported. `"now"` is case-sensitive and permits no extra schedule members.
When changing a schedule, `job.update` requires a concrete once timestamp or
cron schedule and rejects `"now"` with the standard JSON-RPC invalid-params error (`-32602`).

For a new immediate creation, the service samples its injected UTC clock inside
the creation transaction, after ruling out idempotency replay. It rounds the
planned instant down to durable microsecond precision and stores a concrete once
schedule and one schedule-owned run with that planned/runnable
time. The result is the normal job definition, including its ID, revision, and
concrete schedule; it never returns `"now"` as a stored timestamp. Immediate
creation remains scheduled work subject to queue/job suspension, capacity, and
retry policy. It does not create a manual run or bypass the scheduler.

The optional idempotency key is a 1–128-byte UTF-8 string, scoped to the resolved
queue. The canonical request retains `"now"` and original secret references;
the recorded result retains the originally resolved timestamp and definition.
The definition, references, first run, and replay record commit atomically.

If a response is lost, retry with the same key and equivalent request. Clock
changes, secret rotation, and later definition updates do not replace the
recorded result or create another job/run. Replay does not allocate identities,
resolve secrets, or rewrite reference ownership. This guarantee applies while
the idempotency record is retained and the selected queue remains eligible for
creation. Without a key, another request is a new creation.

Reusing a retained key for different canonical input returns application error
`-32000`, category `conflict`, code `jobu.idempotency.conflict`. An explicit
once timestamp is different input from symbolic `"now"`, even if that timestamp
matches the first result. Existing key validation and retention rules apply.

The C++ service accepts `ImmediateSchedule{}` through `CreateJobRequest`'s
`JobCreationSchedule`. Persisted `JobSchedule` and `UpdateJobRequest` remain
once/cron-only. Startup recovery reads the concrete definition/run; a subsequent
replay validates the stored canonical request and result. Malformed replay data
fails closed through the existing persisted-data failure path.

CLI `job create --now` is not yet implemented. This page describes the implemented
creation schedule contract; the complete job-method reference will be expanded
as the remaining control protocol is delivered.

## Run Now (`job.run_now`)

`job.run_now` accepts `{"job_id":"<canonical UUID>","idempotency_key":"optional-key"}`.
Only `job_id` is required. Unknown fields and explicit null for either field
are invalid params (`-32602`). The optional key is 1–128 bytes of UTF-8 and is
scoped to the job. A matching retained key and equivalent request replay the
original run result, even after that run changes state; a different request
with the same key returns `jobu.idempotency.conflict`.

The result is the full run view described in [run.md](run.md). It contains the
durable payload template and materialized attributes, never resolved secret
bytes or attempt output. A newly created manual run has `origin:"manual"`,
`schedule_owned:false`, and its planned and runnable times equal the time
sampled by the creation transaction.

Run Now requires the job's existing schedule-owned occurrence to be scheduled
strictly in the future, with no running or retry-waiting work and no other
nonterminal manual run. It leaves that scheduled occurrence intact. Job
suspension does not prevent creating the manual run; queue suspension still
prevents dispatch. A successful new mutation commits before its reply and
requests a later scheduler rescan. A lost or oversized response can be
reconciled with an idempotent replay while the record is retained.

CLI and typed-client access to this method are planned for later Phase 8 stages.
