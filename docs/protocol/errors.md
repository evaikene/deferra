# Operation errors

This page records implemented error identities. Secret operations are available
through the C++ `SecretService` and the registered `secret.set`, `secret.list`,
and `secret.delete` RPC methods. See [secret method requests and
results](methods/secret.md).

## Secret service

Malformed RPC parameter shape, type, or encoding returns JSON-RPC invalid
params (`-32602`) with no application data. Base64 size checks precede syntax
validation: an apparently oversized value returns `jobu.secret.too_large`
even if its base64 is also malformed. Other malformed base64 returns `-32602`.
Secret operation errors use `-32000` and expose only safe `category` and
`code` data.

| Code | Category | Meaning |
| --- | --- | --- |
| `jobu.secret.invalid_name` | InvalidArgument | The name or list continuation is not a canonical lowercase attribute-style identifier of at most 128 bytes. Names are not normalized. |
| `jobu.secret.too_large` | ResourceExhausted | A set value exceeds 65,536 raw bytes. Empty and arbitrary binary values are accepted. |
| `jobu.storage.invalid_limit` | InvalidArgument | A metadata page limit is outside 1–200. The default is 100. |
| `jobu.secret.not_found` | NotFound | The secret to delete or a name referenced by a new/replacement job definition does not exist. The private execution lookup uses the same code for a missing row. |
| `jobu.secret.in_use` | Conflict | A current definition or nonterminal run snapshot references the secret and prevents deletion. There is no force option. |
| `jobu.service.stopping` | Unavailable | Secret mutations have been stopped by shutdown or a fatal failure. |

These ordinary operation errors do not emit `failed`. Failed transaction cleanup
can override an ordinary error: if rollback poisons the connection, the service
returns the cleanup failure, closes mutation admission, and emits `failed` after
all operation-local queries and transaction guards have unwound. If the operation
already had a fatal storage error, that original cause takes precedence.

Database errors during mutations are fatal. Metadata reads use the existing
Read policy: ordinary database I/O failures are operation errors, while
corruption, unexpected raw constraints, invalid durable metadata and a poisoned
connection are fatal. Storage errors retain their trusted code/category, with
fixed safe message/detail text. The service emits `failed` only for its first
fatal failure. The daemon then closes secret and management mutations and
scheduler completion acceptance before returning from that notification.

`stop_mutations()` is irreversible and leaves metadata reads available. Each
successful set or delete emits `mutation_committed` only after commit and cleanup;
reads and failed operations never emit it. A commit acknowledgement failure may
leave the mutation durable even though the operation reports failure. It does
not emit a success notification or imply that rollback undid the write.

Metadata consists only of name, creation time and update time; reads never select
secret values or return lengths, digests or previews. Values remain plaintext in
the database. Error messages never contain supplied bytes or backend diagnostics.

Job creation and update check referenced names using metadata and maintain the
current-definition index in the same transaction as the definition and run
changes. A stale revision or failed write leaves the previous references intact.
Idempotent creation replay returns the original recorded result without checking
current secret existence or recreating references.

Deletion checks the current-definition index, then scans all nonterminal run
snapshots in bounded ID pages within the same transaction. Older snapshots
continue to protect their references after definition changes, independently of
current owner state or run origin. Terminal history alone permits deletion.
Malformed stored templates or failed scans abort deletion and trigger the fatal
storage boundary. Bounded pages limit memory use, not total scan latency.
There is no public secret-value read API.

## History and statistics reads

Malformed RPC params return JSON-RPC invalid params (`-32602`). Validly shaped
requests that fail a service rule return application `-32000` with safe
`data:{category,code}`. No error includes a payload, output bytes, secret value,
stored filters, SQL, or token internals.

| Code | Category | Meaning |
| --- | --- | --- |
| `jobu.history.invalid_request` | InvalidArgument | A direct history read has an invalid limit, channel, or offset. The connection remains usable. |
| `jobu.history.invalid_cursor` | InvalidArgument | A cursor is malformed, unknown, expired, evicted, or belongs to another method. The current request fails; the connection remains usable. Start a new initial query. |
| `jobu.history.cursor_unavailable` | ResourceExhausted | The server could not issue a unique continuation token. The current query fails; the connection remains usable. |
| `jobu.run.not_found` | NotFound | A requested retained run does not exist. |
| `jobu.attempt.not_found` | NotFound | A requested retained attempt does not exist. |
| `jobu.statistics.invalid_request` | InvalidArgument | The resolved window, limit, scope, or grouping is invalid. |
| `jobu.statistics.invalid_cursor` | InvalidArgument | A statistics cursor is unknown, expired, evicted, or belongs to the other statistics scope. Start a new initial query. |
| `jobu.statistics.cursor_unavailable` | ResourceExhausted | The server could not issue a unique statistics continuation token. |
| `jobu.response.too_large` | ResourceExhausted | A result cannot fit the configured RPC response bound. No read data is truncated into a misleading success; a mutation may already have committed. |
| `jobu.service.stopping` | Unavailable | History or statistics read admission has closed during shutdown or fatal failure. |

Cursor lifetime is fixed at five minutes from the initial page. Earlier eviction
or server restart can invalidate it sooner. Ordinary read errors leave the
connection usable; malformed persisted data, storage corruption, and poisoned
connections close read admission and cause the daemon's fatal transition.

## Run controls and cron preview

The following existing service and engine errors are now reachable through
`job.run_now`, `run.cancel`, `schedule.validate`, and `schedule.next` as
application error `-32000`. Its `data` contains only `category` and `code`;
backend detail and submitted payloads are not echoed.

| Code | Category | Meaning |
| --- | --- | --- |
| `jobu.run.manual_conflict` | Conflict | Run Now's manual barrier or eligible schedule condition is not satisfied. |
| `jobu.idempotency.conflict` | Conflict | A retained key was reused with different canonical Run Now input. |
| `jobu.run.not_found` | NotFound | Cancellation's run ID does not exist. |
| `jobu.run.state_conflict` | Conflict | Cancellation found a run that is already terminal or otherwise cannot be cancelled. |
| `jobu.scheduler.stopping` | Unavailable | Cancellation was rejected after fatal failure or shutdown. |
| `jobu.schedule.invalid_expression` | InvalidArgument | Cron grammar is invalid or unsupported. |
| `jobu.schedule.invalid_timezone` | InvalidArgument | The timezone name is invalid or unavailable. |
| `jobu.schedule.invalid_count` | InvalidArgument | Preview count is outside 1–200. |
| `jobu.schedule.no_future_occurrence` | InvalidArgument | No later occurrence exists in the bounded calendar search. |
| `jobu.schedule.out_of_range` | ResourceExhausted | A requested occurrence cannot be represented. |

Malformed parameter shapes, wrong JSON types, and non-UTC `after` text return
JSON-RPC invalid params (`-32602`) without application `data`. Cancellation
storage failures pass through the scheduler's fatal gate; a poisoned rollback
closes daemon admission before the handler returns its safe error.
