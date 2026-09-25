# Queue methods

Queues group jobs and set scheduling capacity, recovery policy, and default
attributes. A queue has a stable ID and an exact user-facing name. See the
[queue result fields](../types.md#queue-and-job-definition-results) and the
[attribute reference](../types.md#attributes-and-defaults).

| Method | Purpose | Since | CLI | C++ client |
| --- | --- | --- | --- | --- |
| `queue.create` | Create an active queue | 1.1 | `queue create` (`add`) | `create_queue()` |
| `queue.get` | Read a queue | 1.1 | `queue get` | `get_queue()` |
| `queue.list` | List queues | 1.1 | `queue list` | `list_queues()` |
| `queue.update` | Replace selected settings | 1.1 | `queue update` | `update_queue()` |
| `queue.suspend` | Stop new work and drain running work | 1.1 | `queue suspend` | `suspend_queue()` |
| `queue.resume` | Admit work again | 1.1 | `queue resume` | `resume_queue()` |
| `queue.delete` | Soft-delete a suspended queue | 1.1 | `queue delete` | `delete_queue()` |
| `queue.stats` | Summarize retained work for one queue | 1.3 | `queue stats` | `queue_statistics()` |

Every method takes object params. Except for `queue.list`, select one queue
with exactly one of `queue_id` and `queue_name`; they cannot be combined.
`queue_id` is a canonical UUID. Ordinary get/mutation methods target
non-deleted queues. `queue.stats` also accepts a deleted queue by ID or an
unambiguous historical name.

## Create (`queue.create`)

| Params member | Type | Required | Default | Since | Meaning |
| --- | --- | --- | --- | --- | --- |
| `name` | string | Yes | — | 1.1 | 1–128 UTF-8 bytes; no ASCII controls or reserved deletion suffix |
| `weight` | integer | No | 1 | 1.1 | Positive scheduler weight |
| `concurrency_limit` | integer | No | 1 | 1.1 | Positive combined running limit |
| `recovery_policy` | string | No | `fail_interrupted` | 1.1 | `fail_interrupted` or `retry_interrupted` |
| `defaults` | attribute object | No | `{}` | 1.1 | Partial queue-default layer |
| `history_retention_seconds` | integer or null | No | null | 1.1 | Null inherits daemon policy; zero is unlimited |
| `runnable_wait_warning_ms` | integer | No | 10000 | 1.1 | Stored nonnegative warning threshold |
| `idempotency_key` | string | No | — | 1.1 | 1–128 UTF-8 bytes for durable replay |

The result is a full [queue object](../types.md#queue-and-job-definition-results).
The daemon stores retention and warning settings, but this version does not
automatically purge retained history or emit runnable-wait warnings.
Create commits the queue and any idempotency record before replying. The same
key and equivalent canonical request replay the original result while that
record remains; a different request with that key conflicts. A lost reply
can be reconciled by repeating the **same** keyed request.

```json
{"name":"reports","concurrency_limit":2,"history_retention_seconds":86400,"idempotency_key":"reports-create-1"}
```

## Get and list (`queue.get`, `queue.list`)

`queue.get` accepts exactly one selector and returns a full queue object.
`queue.list` accepts these optional params:

| Params member | Type | Default | Since | Meaning |
| --- | --- | --- | --- | --- |
| `include_deleted` | boolean | `false` | 1.1 | Include soft-deleted queues |
| `state` | `QueueState` | All states allowed by `include_deleted` | 1.1 | Filter one lifecycle state |
| `limit` | integer | 100 | 1.1 | 1–200 items |
| `after_id` | ID | Beginning | 1.1 | Exclusive ascending-ID boundary |

The result has `items` and nullable `next_after_id`, as defined in
[shared types](../types.md#queue-and-job-definition-results). Continue by
sending `next_after_id` as `after_id`; do not send JSON null. Lists are live
reads, so concurrent mutations can change later pages. A page can stop early
to fit the 512 KiB result budget.

```json
{"include_deleted":false,"limit":20}
```

```json
{"items":[],"next_after_id":null}
```

## Update (`queue.update`)

The request needs exactly one selector and at least one setting field, even if
the supplied value equals the current value:

| Params member | Type | Required | Since | Meaning |
| --- | --- | --- | --- | --- |
| `queue_id` or `queue_name` | ID or string | One | 1.1 | Target queue |
| `name` | string | No | 1.1 | Replacement exact name |
| `weight` | integer | No | 1.1 | Replacement positive weight |
| `concurrency_limit` | integer | No | 1.1 | Replacement positive capacity |
| `recovery_policy` | string | No | 1.1 | Replacement recovery policy |
| `defaults` | attribute object | No | 1.1 | Complete replacement of the partial default layer; `{}` clears it |
| `history_retention_seconds` | integer or null | No | 1.1 | Null restores daemon inheritance; zero is unlimited |
| `runnable_wait_warning_ms` | integer | No | 1.1 | Replacement nonnegative warning threshold |

Omitted settings remain unchanged. Queue defaults affect jobs created after
the update, not existing materialized job attributes. The result is the
committed full queue object. Queue updates do not use job revisions.

```json
{"queue_name":"reports","history_retention_seconds":null,"defaults":{}}
```

## Suspend, resume, and delete

Each takes exactly one selector, for example `{"queue_name":"reports"}`.
Suspend and resume return the full queue object. Suspension first stops new
eligibility; it can return `suspending` while active work drains. Resume
returns `active`. `queue.delete` requires a fully suspended queue and returns
JSON `null` after soft-deleting it and its current jobs. Retained history
remains addressable by ID. There is no force-delete option.

```json
null
```

## Retained statistics (`queue.stats`)

The initial request requires one queue selector. It also accepts `job_id`,
`type`, `origin`, `planned`, `group_by`, and `limit` with the same meanings,
defaults, and **Since 1.3** as [`system.stats`](system.md#retained-statistics-systemstats).
Its result is the same [statistics page](../types.md#statistics-page). The
selector is resolved to a stable queue ID before a cursor is issued; an ID
may select a soft-deleted queue. A continuation contains only `cursor` and
must not repeat the selector or filters.

```json
{"queue_name":"reports","group_by":"state","limit":20}
```

Unknown selectors return `jobu.queue.not_found`. A historical name matching
multiple deleted queues returns `jobu.queue.ambiguous_deleted_name` with
category `conflict`; use `queue_id` to choose one. A system-statistics cursor
cannot be used here. See [Errors](../errors.md) for other operation errors.

## Errors and response boundaries

Malformed params or extra members return JSON-RPC `-32602`. Normal not-found,
name, state, capacity, conflict, idempotency, and limit errors use application
`-32000` with safe `data.category` and `data.code`. A result too large for the
configured response body returns `jobu.response.too_large`; a preceding
mutation may already have committed. A lost mutation reply does not prove
failure. Query the queue or use the original idempotency key where supported.
Fatal storage and persisted-data errors close daemon admission.

For example, creating a queue with a name already in use can return:

```json
{"jsonrpc":"2.0","id":4,"error":{"code":-32000,"message":"A queue with that name already exists","data":{"category":"conflict","code":"jobu.queue.name_conflict"}}}
```
