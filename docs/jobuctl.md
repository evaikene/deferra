# jobuctl

`jobuctl` manages JobU queues and job definitions through a running `jobud` daemon's local socket.

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
| `system` | `info` |
| `queue` | `create` (`add`), `get`, `list`, `update`, `suspend`, `resume`, `delete` |
| `job` | `create` (`add`), `get`, `list`, `update`, `suspend`, `resume`, `move`, `delete` |

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

`--request-file` is available for the system, queue, and job commands listed above. It cannot be combined with command operands or command-specific request options. Global options and `queue/job suspend --wait` remain available. The file must contain one JSON object, with no trailing non-whitespace text, and is limited to the configured RPC body size (1 MiB by default). Request fields follow the public method's strict JSON contract. For example:

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
