# jobuctl

`jobuctl` manages JobU queues, jobs, runs, retained attempts, named secrets, statistics, and cron previews through a running `jobud` daemon's local socket.

## Help and version

Help is available without a daemon or socket path:

```sh
jobuctl
jobuctl --help
jobuctl queue
jobuctl queue --help
jobuctl queue create --help
jobuctl queue add --help
jobuctl help queue add
jobuctl job create -h
jobuctl --version
```

With no arguments, `jobuctl` lists the command groups. A group without an action lists its commands. Command help lists operands, options, defaults, restrictions, and an example. `-h` and `--help` work even when required command operands are missing. Help and version do not contact the daemon or read files or standard input.

An unknown group, command, or option is an error, even alongside `--help`. An option that requires a value must still have one: `queue get --name --help` reports a missing value. Successful help/version exits with status 0; command syntax errors exit with status 2 and print an error and relevant usage to standard error.

## Available commands

| Group | Commands |
| --- | --- |
| `system` | `info`, `stats` |
| `queue` | `create` (`add`), `get`, `list`, `update`, `suspend`, `resume`, `delete`, `stats` |
| `job` | `create` (`add`), `get`, `list`, `update`, `suspend`, `resume`, `move`, `delete`, `run-now` |
| `run` | `get`, `list`, `cancel` |
| `attempt` | `get`, `list`, `output` |
| `secret` | `set`, `list`, `delete` |
| `schedule` | `validate`, `next` |

`queue add` and `job add` are aliases for `queue create` and `job create`. Each alias accepts the same options and performs the same operation as its canonical command.

## Connecting to the daemon

Remote commands require `--socket PATH`. This option can precede or follow the command path; supply it only once:

```sh
jobuctl --socket /run/jobu.sock system info
jobuctl queue list --socket /run/jobu.sock
jobuctl queue --socket /run/jobu.sock create reports
```

Choose the path used by your daemon. There is no automatic socket discovery.

Global options also work before or after the command path:

| Option | Behavior |
| --- | --- |
| `--json` | Print one compact JSON result on standard output for a successful remote command. |
| `--timeout MS` | Set the positive overall deadline in milliseconds; default 5000. It includes connection, handshake, the command, and any requested wait. |
| `--request-file FILE` | Read a complete JSON params object from `FILE`, or use `-` for standard input. |

`--request-file` is available for every command listed above. It cannot be combined with command operands or command-specific request options. Global options, supported `--wait`, and `attempt output` delivery options remain available. The file must contain one JSON object, with no trailing non-whitespace text, and is limited to the configured RPC body size (1 MiB by default). Request fields follow the public method's strict JSON contract. For example:

```sh
printf '{"limit":20}\n' > queue-list.json
jobuctl --socket /run/jobu.sock queue list --request-file queue-list.json
```

Help and version finish without reading a request file or standard input. `--json` does not change help into JSON.

Queue operations select a queue with exactly one of `--id UUID` or `--name NAME`. Job creation and moving use exactly one of `--queue-id UUID` or `--queue-name NAME`. Job update, move, and delete require `--revision N`; use the current revision shown by `job get`.

For example:

```sh
jobuctl --socket /run/jobu.sock queue create reports
jobuctl --socket /run/jobu.sock job create \
    --queue-name reports --type cli --at 2030-01-01T00:00:00Z \
    --command /bin/echo --arg=hello
```

Use a future UTC timestamp appropriate for your job. `job create` and `job add` also accept `--now` for a once job due at the daemon's current time, or `--cron EXPRESSION` for a recurring job. Choose exactly one of `--now`, `--at UTC`, and `--cron EXPRESSION`. `--timezone ZONE` applies only to cron and defaults to UTC. `job update` accepts `--at` or `--cron` with optional timezone; symbolic `--now` is creation-only.

An idempotency key makes an identical `--now` creation replay the original job and scheduled run. Supply the same key and request when retrying after a lost response; changing the request with the same key is a conflict.

## Queue and job configuration

Queue create/update accept `--recovery-policy fail_interrupted|retry_interrupted`, `--history-retention-seconds N`, `--runnable-wait-warning-ms N`, and `--defaults-file FILE`. Retention `0` means unlimited. Omit retention at creation to inherit the daemon policy; use `--inherit-history-retention` on update to restore inheritance. These two update options are mutually exclusive. The warning delay is nonnegative milliseconds. A defaults file contains a JSON object of registered queue default attributes; `{}` clears all queue defaults on update. Omitted update fields stay unchanged.

```sh
printf '{"retry.max_attempts":2}\n' > queue-defaults.json
jobuctl --socket /run/jobu.sock queue create reports \
    --defaults-file queue-defaults.json --history-retention-seconds 86400
jobuctl --socket /run/jobu.sock queue update --name reports --history-retention-seconds 0
jobuctl --socket /run/jobu.sock queue update --name reports --inherit-history-retention
```

Job create/update accept repeated `--attribute NAME=JSON_VALUE` for distinct registered job attributes. JSON values keep their types, so a numeric value is written as `--attribute retry.max_attempts=2`. On update, supplied attributes replace those named values; omitted attributes remain unchanged. `job update` still requires the current `--revision`, and `--clear-name` explicitly clears the name. A stale revision is returned as a conflict; the CLI does not fetch a newer revision and retry.

For CLI jobs, repeat `--arg`, `--env`, `--unset-env`, and `--expected-exit-code` as needed. For HTTP jobs, use `--url`, optional `--method`, repeated `--header NAME=VALUE`, and optional `--body TEXT` for a UTF-8 body. CLI and HTTP fields cannot be mixed. Use `--request-file` for complete nested configuration, binary HTTP bodies, secret references, or a full job type/payload replacement on update. For example, a job request file can contain a header value such as `{"secret":"service.token"}`; the secret name must already exist on the daemon.

`queue suspend --wait` and `job suspend --wait` return when the resource reaches `suspended`. They submit the suspension once and use read requests while waiting. The same overall `--timeout` applies. A timeout after an observed suspension reply means the final state was not confirmed; inspect the resource before deciding what to do next.

## Runs and attempts

`job run-now JOB_UUID` creates a manual run from the current job definition. Use `--idempotency-key KEY` when a retry after a lost response must return the original run. Run Now preserves the job's scheduled occurrence and follows the daemon's eligibility rules.

```sh
jobuctl --socket /run/jobu.sock job run-now JOB_UUID --idempotency-key manual-42 --json
jobuctl --socket /run/jobu.sock run get RUN_UUID --json
jobuctl --socket /run/jobu.sock run list --job-id JOB_UUID --state failed --limit 20
```

`run list` accepts `--queue-id`, `--job-id`, `--state`, `--origin scheduled|manual`, `--type cli|http`, and the UTC bounds `--planned-from/--planned-to`, `--started-from/--started-to`, and `--completed-from/--completed-to`. Lower bounds are inclusive and upper bounds are exclusive. Filters combine. `--limit` is 1–200, default 100. The result contains run summaries, without payload, result, attempts, or output. Full run details are available through `run get`.

When a run page has `next_cursor`, pass it as `run list --cursor TOKEN` with no filters or limit. A cursor can expire or become invalid after daemon restart. This cursor differs from the ID-based `--after` option on queue/job lists.

`run cancel RUN_UUID` succeeds when the daemon returns either `completed` or `requested`. A `requested` reply means active work is still settling. Add `--wait` to submit cancellation once and observe `run.get` until the run is durably `cancelled`, another terminal state causes a conflict, or the overall deadline expires. Set `--timeout` long enough for the job's process termination grace when waiting on active work. After a wait timeout, inspect the run before deciding what to do next. A successful `run get` can describe a failed run without changing the CLI's exit status.

```sh
jobuctl --socket /run/jobu.sock run cancel RUN_UUID --wait --json
jobuctl --socket /run/jobu.sock attempt list RUN_UUID --limit 20 --json
jobuctl --socket /run/jobu.sock attempt get RUN_UUID 1 --json
```

`attempt list` returns newest attempt numbers first. Its summaries contain no result or output. Continue with `attempt list --cursor TOKEN` and no run ID or limit. `attempt get RUN_UUID NUMBER` returns one attempt's details; `NUMBER` must be positive.

`attempt output RUN_UUID NUMBER --channel CHANNEL` reads one retained chunk. Use `stdout` or `stderr` for a CLI job, and `body` or `headers` for an HTTP job. `--offset N` addresses retained bytes, default 0; `--limit N` requests 1–65,536 raw bytes, default 16,384. Follow `next_offset` to read another chunk. Truncated output may have an omitted middle section; retained offsets do not recover omitted bytes. The response distinguishes available, pending, not-captured, and lost output, and reports retained size, known total/omitted size, truncation, and capture loss.

By default, output shows metadata and an escaped text or base64 preview. `--json` prints the complete protocol result. `--raw` writes only this chunk's decoded bytes to standard output, including binary bytes. `--output-file PATH` writes only this chunk to a newly created file and refuses an existing path. These three modes are mutually exclusive.

```sh
jobuctl --socket /run/jobu.sock attempt output RUN_UUID 1 --channel stdout --offset 0 --limit 4096 --json
jobuctl --socket /run/jobu.sock attempt output RUN_UUID 1 --channel stdout --raw > chunk.bin
jobuctl --socket /run/jobu.sock attempt output RUN_UUID 1 --channel stdout --output-file chunk.bin
```

## Retained statistics

`system stats` summarizes runs across queues; `queue stats` requires exactly one
initial selector, `--id UUID` or `--name NAME`. Both accept `--job-id UUID`,
`--type cli|http`, `--origin scheduled|manual`, `--planned-from UTC`,
`--planned-to UTC`, `--group-by none|queue|job|type|origin|state`, and `--limit N`
(1–200, default 100). `system stats` also accepts `--queue-id UUID` as a filter.
The planned window is half-open and at most 31 days. Omit its upper bound to use
the daemon's current UTC time; omit its lower bound to use 24 hours before the
resolved upper bound.

```sh
jobuctl --socket /run/jobu.sock system stats --group-by state --json
jobuctl --socket /run/jobu.sock queue stats --name reports \
    --planned-from 2030-01-01T00:00:00Z --planned-to 2030-01-02T00:00:00Z \
    --group-by job --limit 20
```

The result includes the resolved window, groups, a nullable `next_cursor`, and
measurement provenance. Run and attempt counts cover the planned-run cohort and
all its retained attempts. Lateness and execution duration are derived from
wall-clock timestamps; averages and maxima are null when there are no samples.
`runnable_wait_ms` is null and marked unavailable. Human output prints these
null and unavailable values explicitly. With `--json`, the CLI prints the
protocol result unchanged.

Continue a page with `system stats --cursor TOKEN` or
`queue stats --cursor TOKEN`. A continuation must contain only the cursor: omit
the queue selector, filters, window, grouping, and limit. The token belongs to
its original method and can expire or be evicted; the returned groups reflect a
live retained-history view. `--request-file` accepts the complete strict params
object for either method; see the [system](protocol/methods/system.md) and
[queue](protocol/methods/queue.md) contracts.

## Cron validation and preview

`schedule validate EXPRESSION` checks a five-field cron expression or supported
alias using the daemon's cron engine. `schedule next EXPRESSION --after UTC`
returns future occurrence timestamps strictly after the supplied RFC 3339 UTC
instant. Both accept `--timezone ZONE` (default UTC); `schedule next` also
accepts `--count N` (1–200, default 5). The timezone controls calendar matching;
results are UTC. Invalid expressions and timezones are reported as errors.

```sh
jobuctl --socket /run/jobu.sock schedule validate '0 9 * * FRI-MON'
jobuctl --socket /run/jobu.sock schedule next '@daily' \
    --timezone Europe/Tallinn --after 2030-01-01T00:00:00Z --count 2 --json
```

The cyclic weekday range `FRI-MON` includes Friday through Monday. These
commands do not create or change a job. Both accept `--request-file` containing
their complete [schedule method](protocol/methods/schedule.md) params object.

## Named secrets

`secret set NAME` creates or rotates a secret. Supply exactly one of `--file PATH`
or `--stdin`; both read raw bytes, including NUL bytes and a final newline,
without text conversion or trimming. Empty values are valid; the maximum is
65,536 bytes. The command line has no literal secret-value option. Put the value
in a file with access restricted to trusted users, or pass it through standard
input. The CLI does not print the value, its size, or a preview.

```sh
jobuctl --socket /run/jobu.sock secret set reports.token --file /private/token.bin
jobuctl --socket /run/jobu.sock secret set reports.token --stdin < /private/next-token.bin
jobuctl --socket /run/jobu.sock secret list --limit 20 --json
```

`secret list` returns names and creation/update timestamps only, ordered by
name. Its `--limit` is 1–200, default 100. When a JSON page contains a non-null
`next_after_name`, continue with `secret list --after-name NAME`; the boundary
is exclusive and is separate from queue/job `--after` IDs and history cursors.
`secret delete NAME` returns JSON `null` on success. A current job definition
or nonterminal run snapshot that refers to the name prevents deletion.

Every secret command also accepts `--request-file` with its complete JSON params
object. For `secret set`, this is an alternative to `NAME` and `--file`/`--stdin`;
the public [secret method contract](protocol/methods/secret.md) defines the
`utf8` and base64 value forms. Keep a request file containing a value private.
Input and generated errors do not echo secret bytes. Secret values remain
plaintext in the database, and application-generated output can contain them;
see [Named secrets](secrets.md) for those exposure boundaries.

## Literal arguments

Repeat `--arg` to pass multiple arguments to a CLI job. Each value becomes one argument, preserving order and empty strings. Use `--arg=VALUE` for values beginning with a dash, especially values that resemble `jobuctl` options:

```sh
jobuctl --socket /run/jobu.sock job create \
    --queue-name reports --type cli --at 2030-01-01T00:00:00Z \
    --command /bin/echo --arg=--help --arg=-h --arg= --arg='two words'
```

Here `--help` and `-h` are arguments to the scheduled program. They do not request `jobuctl` help. Attached values such as `--name=--help` are also literal values.

`--` ends option handling. Following tokens are positional data where the command accepts them; otherwise they are extra-operand errors. For example, this creates a queue named `--help`:

```sh
jobuctl --socket /run/jobu.sock queue create -- --help
```

## Results and errors

Human output escapes control characters in data supplied by the daemon, including names and error text. JSON mode prints exactly one result value and a newline on standard output; successful delete commands print `null`. It prints no headings or progress text. Integer values remain integers, including large revisions.

On failure, standard output is empty. With `--json`, standard error contains one object with `error.kind` (`local` or `remote`), a stable string `code`, nullable `rpc_code` and `category`, safe `message`, and boolean `outcome_unknown`. A remote application error retains its domain code and category. A represented remote RPC error without application data uses `jobuctl.remote.rpc_error` as the string code.

| Exit status | Meaning |
| --- | --- |
| 0 | Successful remote operation or local help/version. |
| 1 | Represented operation failure, such as not found, revision conflict, or an unsupported capability. |
| 2 | Syntax, local request decoding, or request-file input failure. |
| 3 | Connection, protocol, or deadline failure. |

For a mutation, `outcome_unknown: true` means the request may have reached the daemon without an observed result. Check the resource or replay with an explicit idempotency key before deciding whether to repeat it. The CLI does not retry mutations automatically.
