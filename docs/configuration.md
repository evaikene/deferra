# Configuring jobud

`jobud` reads configuration once at startup. Restart to apply changes; there is
no SIGHUP reload. Start with the installed `share/jobu/jobud.ini.example`, whose
paths are generated for that build. Copy it deliberately to a protected active
file, review it, and validate it as the intended daemon account:

```sh
jobud --check-config --config /absolute/protected/jobud.ini
```

The check parses and resolves settings, accounts and timezones without changing
identity, creating directories, opening the database, acquiring locks, starting
workers or listening. It does not prove that subsequent resource access will
succeed. See [operations](operations.md) for ownership and startup failures.

## File selection and precedence

Values resolve in this order: **compiled defaults → configuration → explicit
command-line flags**. `jobud --help` shows that executable's compiled config,
database and socket paths. See [installation](installation.md#operational-paths)
for prefix and cache behavior.

| Invocation | Behavior |
| --- | --- |
| No config-selection flag | Read the compiled config path if present. Absence is allowed; other read/validation failures are errors. |
| `--config PATH` | Require this exact file. A missing, unreadable or invalid file fails. |
| `--no-config` | Skip file loading; conflicts with `--config`. |
| `--check-config` | Validate locally, print success or a safe diagnostic, and exit. |
| `--help`, `--version` | Exit before config or account lookup. |

CLI overrides are `--database`, `--socket`, `--cli-concurrency`,
`--http-concurrency`, `--http-proxy`, `--http-ca-bundle`, `--run-as-user`,
`--run-as-group`, `--allow-root-daemon` / `--no-allow-root-daemon`, and
`--allow-root-cli` / `--no-allow-root-cli`. Repeated or contradictory settings
are rejected. Maintenance and logging settings are configured through the file.
Relative CLI paths use the original invocation directory; file values must use
absolute paths. `jobuctl` does not read this file: pass `--socket` if the daemon
uses an endpoint different from the client's compiled default.

## Grammar and units

Use UTF-8 text, at most 65,536 bytes including comments, with flat, case-sensitive
dotted keys and `key = value` lines. Blank lines and full-line `#` or `;` comments
are allowed. Sections, includes, duplicate/unknown keys, embedded NUL, inline
comments and environment/tilde/command/secret substitutions are unsupported.
INI removes one surrounding single- or double-quote pair; it does not interpret
backslash escapes. Quote paths containing significant leading/trailing spaces.

| Value kind | Accepted spelling |
| --- | --- |
| Startup boolean | `true`, `1`, `on`, `yes`; `false`, `0`, `off`, `no`. Words are ASCII case-insensitive. |
| Startup interval | Unsigned integer seconds, optionally followed by lowercase `s`, `m`, `h`, `d`. `60`, `60s`, `1m` are equivalent. |
| Startup byte size | Unsigned integer bytes, optionally followed by lowercase `k`, `m`, `g` for powers of 1024. |
| Count | Unsigned decimal integer, without a suffix. |
| `defaults.*` | Strict JSON after INI unquoting; durations are integer **milliseconds**, sizes integer **bytes**, without suffixes. |

Signs, fractions in integer quantities, trailing garbage and overflow fail.
Optional account/proxy/CA fields listed below allow empty values where stated;
empty required paths, backend, timezone and user overrides are invalid.

## Fixed configuration keys

These are all 26 fixed keys. Omitted settings use the defaults shown. Account
and path checks also apply after explicit CLI overrides are resolved. The
identity, administrative group and runtime-path enforcement described below is
the Linux deployment contract. On macOS, use the
[ordinary-user foreground workflow](operations.md#macos-foreground-operation):
provision private directories yourself, keep socket mode 0600, leave run-as and
socket-owner/group overrides unset, and keep root permissions false. Parsing
these settings or resolving account names in check mode does not implement
Linux privilege-drop, runtime-path admission or stale-endpoint guarantees on
Darwin. Protected configuration-file reading does apply on Darwin.

### Storage, identity and socket

| Key | Default | Meaning and accepted values |
| --- | --- | --- |
| `database.backend` | `sqlite` | Only `sqlite` is supported. |
| `database.path` | Compiled state directory + `/jobu.sqlite3` | Absolute path in a final-user-owned mode-0700 directory; owner-only regular database and sidecars. |
| `socket.path` | Compiled runtime directory + `/jobud.sock` | Absolute path fitting the native Unix socket limit, in a protected directory. |
| `socket.owner` | Final user | Omitted/empty or an account resolving to that same UID. |
| `socket.mode` | `0600` | Exactly `0600` or `0660`, including the leading zero. |
| `socket.group` | Final primary group | Omitted/empty or a final primary/supplementary group name. |
| `daemon.run_as_user` | Unset | Nonempty account name. Root normally selects a non-root target; non-root callers may select only their existing identity. |
| `daemon.run_as_group` | Target user's primary group | Omitted/empty or a group name; requires a user override. |
| `daemon.allow_root` | `false` | Unsafe permission to retain root daemon identity. A requested user switch still applies. |
| `cli.allow_root` | `false` | Separate unsafe permission for root CLI execution. Does not authorize a root daemon. |

The default private runtime leaf is mode 0700. A mode-0660 socket requires a
mode-0750 runtime leaf with the matching group and safe ancestor traversal.
That group gains full administrative authority, including CLI execution and
secret management. Managed services normally start directly as their dedicated
account, leaving run-as settings unset and both root permissions false.

### Execution and maintenance

| Key | Default | Meaning and accepted values |
| --- | --- | --- |
| `cli.concurrency` | `4` | CLI runner capacity, 1–4294967295. |
| `http.concurrency` | `16` | HTTP runner capacity, 1–4294967295. |
| `http.proxy` | Unset | Omitted/empty or a validated HTTP/HTTPS proxy URL. Protect credentials; do not log this value. |
| `http.ca_bundle` | System trust | Omitted/empty or an absolute readable regular CA file; HTTP setup checks access under the final identity. |
| `schedule.default_timezone` | `UTC` | Supported IANA timezone for omitted cron input timezones. |
| `history.default_retention` | `30d` (2592000 seconds) | 0–9223372036854 seconds; zero is unlimited. A queue can inherit or override it. |
| `history.sweep_interval` | `1m` (60 seconds) | 1–86400 seconds after a completed retention sweep. |
| `history.batch_size` | `100` | 1–1000 parent records per batch; cascaded rows/output bytes are additional work. |
| `telemetry.checkpoint_interval` | `30s` | 1–86400 seconds between checkpoint sweeps, not a maximum crash-loss bound. |

Retention waits 10 ms between batches. Telemetry batches contain at most 200
rows. These two implementation limits are not configuration keys. See
[retention](operations.md#retention-and-replay-lifetimes) and
[measurement limits](observability.md#runnable-wait-and-coverage).

### RPC and logs

| Key | Default | Meaning and accepted values |
| --- | --- | --- |
| `rpc.header_limit_bytes` | `16k` (16384) | 1024–65536 bytes. |
| `rpc.body_limit_bytes` | `1m` (1048576) | 1048576–16777216 bytes; narrower method/result limits still apply. |
| `rpc.max_batch_entries` | `64` | 1–64 entries. |
| `rpc.max_connections` | `128` | 1–4096 simultaneous connections. |
| `rpc.queued_output_bytes` | `2m` (2097152) | At least body limit + header limit; at most 67108864 bytes. |
| `logging.level` | `info` | `fatal`, `error`, `warning`, `info`, `debug1`, `debug2`, `debug3`; includes all more severe levels. |
| `logging.format` | `json` | `json` or `text`, written to stderr. |

JSON nesting depth remains 64. Raising a frame limit also requires a sufficient
output budget; configuration rejects incompatible values. See the
[transport reference](protocol/transport.md) for framing and method budgets,
and [observability](observability.md) for events and logging ownership.

## Default job attributes

All 21 registered daemon-default attributes are listed below. Prefix an
attribute with `defaults.` in this file. Omission leaves the built-in layer;
the table shows that layer's values. Settings resolve as built-in → daemon →
queue → job, then materialize into stored definitions/run snapshots. Restarting
with changed defaults does not rewrite existing snapshots. Retained keyed
creation replays its original response before resolving today's defaults.

| Configuration key | Built-in JSON value | Range or constraint |
| --- | --- | --- |
| `defaults.job.timeout` | `120000` | 1–2592000000 ms per attempt. |
| `defaults.retry.max_attempts` | `1` | 1–100, including the first attempt. |
| `defaults.retry.initial_delay` | `0` | 0–86400000 ms; at most max delay. |
| `defaults.retry.strategy` | `"fixed"` | `"fixed"` or `"exponential"`. |
| `defaults.retry.multiplier` | `2.0` | Finite number 1–100; used by exponential retries. |
| `defaults.retry.jitter` | `0.0` | Finite fraction 0–1; symmetric variation within retry limits. |
| `defaults.retry.max_delay` | `86400000` | 0–2592000000 ms; also caps accepted HTTP Retry-After. |
| `defaults.retry.mode` | `"reschedule"` | `"reschedule"` releases the queue slot during backoff; `"blocking"` retains it. |
| `defaults.cli.retry_exit_codes` | `["0-255"]` | At most 64 normalized code/range strings in 0–255, no overlaps or duplicates; `[]` disables code retries. |
| `defaults.cli.termination_grace` | `5000` | 0–300000 ms before forced process-group cleanup; zero requests immediate cleanup after termination. |
| `defaults.http.follow_redirects` | `false` | JSON boolean; true requires max redirects ≥ 1. |
| `defaults.http.max_redirects` | `5` | 0–20; zero only with follow redirects false. |
| `defaults.http.tls_verify` | `true` | JSON boolean; false disables server authentication. |
| `defaults.http.idempotency_key` | `false` | JSON boolean; send the run ID as the HTTP key. Remote deduplication is not guaranteed. |
| `defaults.http.retry_errors` | `["resolve","connect","tls_handshake","timeout","send","receive"]` | At most 16 distinct known error names; `[]` disables transport retries. |
| `defaults.http.retry_statuses` | `["408","429","500-599"]` | At most 64 distinct status/range strings in 100–599; `[]` disables status retries. |
| `defaults.output.capture` | `"on_error"` | `"none"`, `"on_error"`, `"always"`. |
| `defaults.output.http_body_limit` | `1048576` | 0–67108864 bytes. |
| `defaults.output.http_headers_limit` | `65536` | 0–4194304 bytes. |
| `defaults.output.stderr_limit` | `1048576` | 0–67108864 bytes. |
| `defaults.output.stdout_limit` | `1048576` | 0–67108864 bytes. |

Selector strings use decimal codes without leading zeros or whitespace, or
inclusive ranges with increasing endpoints. Expected exit codes/statuses still
mean success. Retries require an available attempt. HTTP error names are
`resolve`, `connect`, `tls_handshake`, `tls_verification`, `timeout`, `send`,
`receive`, `redirect`, `protocol`. Zero output limits retain no bytes for that
stream; observation/truncation metadata can remain. Captured output and headers
can contain secrets. The [attribute reference](protocol/types.md#attributes-and-defaults)
owns the protocol contract.

Preserve JSON string quotes using an outer INI single-quote pair. This valid
example enables three attempts and retains output, without changing endpoints:

```ini
defaults.retry.max_attempts = 3
defaults.retry.initial_delay = 1000
defaults.retry.mode = '"reschedule"'
defaults.output.capture = '"always"'
```

## Timezone omission and replay

Cron input without `timezone` uses `schedule.default_timezone`. Explicit `UTC`
stays UTC; empty/null timezone is invalid. Responses and stored schedules always
have an explicit timezone. An update omitting the whole schedule preserves it;
a replacement cron schedule omitting timezone resolves the current default.
The CLI preserves omission when `--timezone` is absent.

A retained idempotency key replays the original resolved schedule and attributes
even after configuration changes. A new key uses the current defaults. This
does not alter the existing pending-run update/move behavior; the product
question in issue #230 remains deferred. See [job methods](protocol/methods/job.md).
