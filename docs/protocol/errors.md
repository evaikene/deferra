# Operation errors

This page records implemented error identities as services are added. Secret
operations currently exist through the C++ `SecretService`; secret RPC methods
are not yet registered or advertised. Their wire codecs are a later stage.

## Secret service

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
