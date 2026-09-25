# Job definition methods

A job definition chooses a queue, runner payload, schedule, and attributes.
It is distinct from each [scheduled run](run.md): a run retains the values
captured when that occurrence was created. See [shared result fields](../types.md#queue-and-job-definition-results),
[payload shapes](../types.md#cli-and-http-payloads), and [attributes](../types.md#attributes-and-defaults).

| Method | Purpose | Since | CLI | C++ client |
| --- | --- | --- | --- | --- |
| `job.create` | Create a definition and first scheduled run | 1.1 | `job create` (`add`) | `create_job()` |
| `job.get` | Read a definition | 1.1 | `job get` | `get_job()` |
| `job.list` | List definitions | 1.1 | `job list` | `list_jobs()` |
| `job.update` | Replace selected definition fields | 1.1 | `job update` | `update_job()` |
| `job.suspend` | Stop eligibility and drain work | 1.1 | `job suspend` | `suspend_job()` |
| `job.resume` | Admit scheduled work again | 1.1 | `job resume` | `resume_job()` |
| `job.move` | Move a suspended job to another queue | 1.1 | `job move` | `move_job()` |
| `job.delete` | Soft-delete a suspended job | 1.1 | `job delete` | `delete_job()` |
| `job.run_now` | Create a separate manual run | 1.3 | `job run-now` | `run_now()` |

Every method takes object params and rejects unknown members. `job.get`,
`job.suspend`, and `job.resume` take exactly `{"job_id":"<ID>"}` and return
one full job definition. The ID remains stable across updates and moves.

## Create (`job.create`)

| Params member | Type | Required | Default | Since | Meaning |
| --- | --- | --- | --- | --- | --- |
| `queue_id` or `queue_name` | ID or string | Exactly one | — | 1.1 | Existing non-deleted queue |
| `name` | string | No | null | 1.1 | Non-unique display name, at most 256 UTF-8 bytes |
| `type` | `cli` or `http` | No | `cli` | 1.1 | Runner family |
| `schedule` | schedule object | Yes | — | 1.1 | Once or cron schedule; `at:"now"` since 1.3 |
| `priority` | integer | No | 0 | 1.1 | Signed 32-bit scheduler priority |
| `attributes` | attribute object | No | `{}` | 1.1 | Partial job layer materialized at creation |
| `payload` | object | Yes | — | 1.1 | CLI or HTTP payload, up to 256 KiB encoded |
| `idempotency_key` | string | No | — | 1.1 | 1–128 UTF-8 bytes for durable replay |

The result is the committed [job definition](../types.md#queue-and-job-definition-results)
with revision 1. Creation also commits its first schedule-owned run. A once
schedule uses an explicit UTC `at` time, or the creation-only symbolic
`{"kind":"once","at":"now"}`. The daemon resolves `now` in the creation
transaction to a concrete microsecond timestamp and returns that concrete
schedule. A cron schedule names a timezone and computes its next occurrence.

```json
{"queue_name":"reports","type":"cli","schedule":{"kind":"once","at":"now"},"payload":{"command":"/usr/bin/true"},"idempotency_key":"submit-42"}
```

A matching key and equivalent canonical request replay the original result
while its record remains, even after the clock advances or a secret rotates.
The key is scoped to the resolved queue. Reusing it with different input
returns `jobu.idempotency.conflict`. The canonical request retains symbolic
`now` and original secret references. After a lost reply, repeat the **same**
keyed request; an unkeyed retry is a new creation.

## Get and list (`job.get`, `job.list`)

`job.get` requires `job_id` (Since 1.1). `job.list` accepts:

| Params member | Type | Default | Since | Meaning |
| --- | --- | --- | --- | --- |
| `queue_id` or `queue_name` | ID or string | All queues | 1.1 | Optional single queue selector |
| `include_deleted` | boolean | `false` | 1.1 | Include soft-deleted definitions |
| `state` | `JobState` | All allowed states | 1.1 | Lifecycle filter |
| `type` | `JobType` | Both | 1.1 | Runner-family filter |
| `limit` | integer | 100 | 1.1 | 1–200 items |
| `after_id` | ID | Beginning | 1.1 | Exclusive ascending-ID boundary |

The result has `items` (full definitions) and nullable `next_after_id`.
Continue using that ID as `after_id`. These lists are live and can stop early
at the 512 KiB result budget. Use `run.list` to inspect occurrence history;
`job.list` describes current definitions.

```json
{"queue_name":"reports","type":"cli","limit":20}
```

## Update (`job.update`)

`job_id`, `expected_revision`, and at least one effective change are required.
The supplied revision must equal the current one; the service never fetches a
new revision and retries for you.

| Params member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `job_id` | ID | Yes | 1.1 | Definition to change |
| `expected_revision` | positive integer | Yes | 1.1 | Optimistic revision |
| `name` | string or null | No | 1.1 | Replace or clear; omission leaves unchanged |
| `type` | `JobType` | No | 1.1 | Replacement runner family |
| `schedule` | schedule object | No | 1.1 | Concrete once or cron; `at:"now"` is invalid |
| `priority` | signed integer | No | 1.1 | Replacement priority |
| `attributes` | attribute object | No | 1.1 | Partial patch; supplied names replace values, omitted names stay unchanged |
| `payload` | object | No | 1.1 | Complete replacement payload |

The result is the new full definition with an advanced revision. A changed
schedule can replace an unstarted scheduled occurrence; a running or
retry-waiting occurrence keeps its immutable snapshot. A stale revision
returns `jobu.job.revision_conflict` without modifying either definition or
secret-reference ownership. Updates commit before replying.

```json
{"job_id":"00112233-4455-6677-8899-aabbccddeeff","expected_revision":1,"priority":5}
```

## Suspend, resume, move, and delete

`job.suspend` and `job.resume` require only `job_id` and return a full
committed definition. Suspension can report `suspending` while running work
drains; resume reports `active`. `job.move` requires a suspended job and these
params:

| Params member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `job_id` | ID | Yes | 1.1 | Job to move |
| `expected_revision` | positive integer | Yes | 1.1 | Current revision |
| `target_queue_id` or `target_queue_name` | ID or string | Exactly one | 1.1 | Existing destination queue |

The move returns the full definition with its new `queue_id` and revision.
Earlier runs retain their original queue ID. `job.delete` takes exactly
`job_id` and `expected_revision`, requires the job to be suspended, and
returns JSON `null` after a soft delete. Retained run history remains
queryable. Both operations use revision checks.

For example, move a job with revision 2 to another queue:

```json
{"job_id":"00112233-4455-6677-8899-aabbccddeeff","expected_revision":2,"target_queue_name":"archive"}
```

A successful `job.delete` result is:

```json
null
```

## Create a manual run (`job.run_now`)

| Params member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `job_id` | ID | Yes | 1.3 | Existing definition |
| `idempotency_key` | string | No | 1.3 | 1–128 UTF-8 bytes, scoped to this job |

The result is the [full run view](../types.md#run-and-attempt-history), with
`origin:"manual"` and `schedule_owned:false`. Run Now snapshots the current
definition but leaves its future schedule-owned occurrence intact. It requires
that occurrence to be strictly in the future, with no running/retry-waiting
work or other nonterminal manual run. Job suspension does not prevent creating
the manual run; queue suspension still prevents dispatch.

```json
{"job_id":"00112233-4455-6677-8899-aabbccddeeff","idempotency_key":"manual-42"}
```

A matching retained key replays the originally recorded run, even if its
state has since changed. A conflicting key returns `jobu.idempotency.conflict`;
an ineligible manual barrier returns `jobu.run.manual_conflict`. A lost reply
can be reconciled with `run.list` or the same keyed request.

## Errors and observation

Malformed shapes, unsupported enum values, extra members, and `at:"now"` in
`job.update` return JSON-RPC `-32602`. Domain errors, including missing
resources, state/revision conflicts, invalid attributes or payloads, and
idempotency conflicts, use application `-32000` with safe `data.category` and
`data.code`. A successful mutation commits before its reply and requests a
scheduler rescan where needed. `jobu.response.too_large` can be returned after
such a commit; do not infer rollback from a missing or oversized reply.
Fatal storage/persisted-data failures close daemon admission. See
[Errors](../errors.md) and [Named secrets](../../secrets.md).

For example, an update with a stale revision can return:

```json
{"jsonrpc":"2.0","id":7,"error":{"code":-32000,"message":"Job revision does not match the expected revision","data":{"category":"conflict","code":"jobu.job.revision_conflict"}}}
```
