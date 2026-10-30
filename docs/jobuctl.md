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
| `run` | `get`, `list`, `cancel`, `output` |
| `attempt` | `get`, `list`, `output` |
| `secret` | `set`, `list`, `delete` |
| `schedule` | `validate`, `next` |

`queue add` and `job add` are aliases for `queue create` and `job create`. Each alias accepts the same options and performs the same operation as its canonical command.

## Connecting to the daemon

Remote commands use the socket path compiled into the application when `--socket` is omitted:

```sh
jobuctl system info
jobuctl queue list
```

The default is `${JB_JOBU_RUNDIR}/jobud.sock`, shared with `jobud`. In a fresh build,
`JB_JOBU_RUNDIR` defaults to `${CMAKE_INSTALL_PREFIX}/var/run`, normally giving
`/usr/local/var/run/jobud.sock`. Run `jobuctl --help` to see your executable's actual
compiled default. Changing installation destinations with `DESTDIR` or
`cmake --install --prefix` does not change that compiled path.

Use a nonempty `--socket PATH` to override it. The option can precede, appear
between, or follow the command path; supply it only once:

```sh
jobuctl --socket /run/jobu/jobud.sock system info
jobuctl queue list --socket /run/jobu/jobud.sock
jobuctl queue --socket /run/jobu/jobud.sock create reports
```

Choose the path used by your daemon. If its `socket.path` configuration or
`jobud --socket` selects a different endpoint, pass that path to `jobuctl` explicitly.
The client does not read daemon configuration, consult environment variables for
an endpoint, search alternate paths, or probe other installations. A failed
connection identifies the attempted endpoint safely and suggests `--socket PATH`.

Global options also work before or after the command path:

| Option | Behavior |
| --- | --- |
| `--json` | Print one compact JSON result on standard output for a successful remote command. |
| `--timeout MS` | Set the positive network deadline in milliseconds; default 5000. It includes connection, handshake, the command, and any requested wait. Interactive editing gives fetch and submission separate full budgets; editor time is excluded. |
| `--request-file FILE` | Read a complete JSON params object from `FILE`, or use `-` for standard input. |

`--request-file` is available for every command listed above except `run output`,
which composes existing RPCs and has no complete protocol request of its own.
Use `attempt output --request-file FILE` to supply an explicit output request.
Request files cannot be combined with command operands or command-specific
request options. Global options, supported `--wait`, and `attempt output`
delivery options remain available. The file must contain one JSON object, with
no trailing non-whitespace text, and is limited to 1 MiB, the client's default
RPC body limit. Request fields follow the public method's strict JSON
contract. For example:

```sh
printf '{"limit":20}\n' > queue-list.json
jobuctl queue list --request-file queue-list.json
```

Help and version finish without reading a request file or standard input. `--json` does not change help into JSON.

`queue get`, `update`, `suspend`, `resume`, `delete`, and initial `stats` accept
one positional `SELECTOR`, or the explicit aliases `--id UUID` and `--name NAME`.
These forms are mutually exclusive, even if they identify the same queue.

| Selector | Meaning |
|---|---|
| `reports` | Exact queue name |
| An exact 8-4-4-4-12 hexadecimal UUID, in either hex case | Queue ID, checked for an exact-name collision |
| `id:UUID` | Explicit ID; a malformed UUID is a local error |
| `name:NAME` | Explicit exact name, including UUID-shaped names |
| `name:id:reports` / `name:name:reports` | Literal `id:reports` / `name:reports` |
| `team:reports` | Exact name containing a colon |

Prefixes are case-sensitive and interpreted once. Names are not trimmed or
case-normalized; quote spaces as usual, for example `queue get 'night reports'`.
Use `queue get name:-reports` or `queue get -- -reports` for an option-looking
name. Alias values are literal: `--name id:reports` selects the name `id:reports`,
and `--id` requires the UUID itself. `queue create NAME` keeps a literal name,
including prefix-looking names; `queue list` takes no selector.

For a bare UUID, the CLI checks both the ID and the exact original name spelling.
If they select different queues, the command fails with
`jobuctl.queue.ambiguous_selector`; use `id:UUID` or `name:NAME` to state intent.
Matching the same queue both ways is allowed. If the ID is missing, a name match
does not become a fallback: use `name:NAME` explicitly. Prefixes and aliases
bypass this ambiguity check; explicit names still require resolution.

Names are resolved once and subsequent operations use the returned UUID, so a
later rename or name reuse cannot redirect the command. Sequential probes are
not an atomic snapshot of the namespace. Connection, handshake, selection, the
final RPC and any wait polling share one `--timeout` budget. Only the final
result is printed, including in JSON mode. A failure during read-only selection
does not imply an unknown mutation outcome. Selection reads require API 1.6 and
the `queue.get` capability; explicit IDs require no additional preflight read.

Request-file selectors remain raw
protocol values: `queue_name` is literal, `queue_id` must be a UUID, and neither
field undergoes CLI prefix interpretation or inferred selection.

Ordinary `queue get SELECTOR` reads only non-deleted queues. Since API 1.6,
`queue get --request-file FILE` can explicitly include retained deleted rows,
using strict JSON such as `{"queue_name":"archived-reports","include_deleted":true}`.
Omission or false stays live-only; there is no `--include-deleted` flag for get.
Historical name lookup prefers a live queue, then a unique original deleted
name. Multiple deleted matches fail with `jobu.queue.ambiguous_deleted_name`;
use `queue_id` to select the retained row explicitly. The option is rejected
on queue mutation requests.

Job creation and moving use exactly one of `--queue-id UUID` or `--queue-name NAME`. Flag-based job updates, moves, and deletes require `--revision N`; use the current revision shown by `job get`. A bare `job update UUID` instead [opens an interactive draft](#interactive-job-updates) and captures the inspected revision.

For example:

```sh
jobuctl queue create reports
jobuctl job create \
    --queue-name reports --type cli --at 2030-01-01T00:00:00Z \
    --command /bin/echo --arg=hello
```

Use a future UTC timestamp appropriate for your job. `job create` and `job add` also accept `--now` for a once job due at the daemon's current time, or `--cron EXPRESSION` for a recurring job. Choose exactly one of `--now`, `--at UTC`, and `--cron EXPRESSION`. `--timezone ZONE` applies only to cron; omission uses the daemon's `schedule.default_timezone` (normally UTC). Explicit UTC stays UTC. `job update` accepts `--at` or `--cron` with optional timezone; symbolic `--now` is creation-only.

An idempotency key makes an identical `--now` creation replay the original job and scheduled run. Supply the same key and request when retrying after a lost response; changing the request with the same key is a conflict.

## Listing jobs

Ordinary `job list` shows only `active` definitions. Use `--state STATE` to
select one of `active`, `suspending`, `suspended`, `succeeded`, `failed`,
`cancelled`, or `deleted`. State names are lowercase. `--state deleted` includes
deleted definitions automatically. `--all` lists every nondeleted state;
`--include-deleted` by itself lists every state, including deleted. The two
flags can be combined. `--state STATE --include-deleted` retains that one state
filter while also allowing selection of a deleted queue. `--all` and `--state`
cannot be combined.

```sh
jobuctl job list
jobuctl job list --state succeeded
jobuctl job list --state deleted --queue-name reports
jobuctl job list --all --include-deleted
```

Queue selectors, `--limit`, and `--after` work with these filters. Pages are
ordered by job UUID and bounded by the result size budget. Keep the same
filters when continuing with `--after`. Filtering happens before paging, so a
page contains only matching jobs. `job get` can inspect a known finished job.

A `job list --request-file` command uses its JSON object exactly as supplied.
In particular, `{}` leaves `state` absent and lists all nondeleted states,
following the raw `job.list` API. Request-file input cannot be combined with
`--state`, `--all`, `--include-deleted`, or other command options.

## Queue and job configuration

Flag-based queue updates require the inspected `--revision N` (1 through
`9223372036854775807`) and at least one replacement field. Use `--new-name NAME`
to rename; `--name NAME` always selects. Omitted fields stay unchanged. A bare
`queue update SELECTOR` instead [opens an interactive draft](#interactive-queue-updates)
and captures the inspected revision. The CLI does not fill in a missing revision
for a flag-based update or retry a stale one.
For identity continuity after a read, retain both its UUID and revision and use
`id:UUID`: revision equality alone cannot protect against name reuse.

Queue create/update accept `--recovery-policy fail_interrupted|retry_interrupted`, `--history-retention-seconds N`, `--runnable-wait-warning-ms N`, and `--defaults-file FILE`. Retention `0` means unlimited. Omit retention at creation to inherit the daemon policy; use `--inherit-history-retention` on update to restore inheritance. These two update options are mutually exclusive. The warning delay is nonnegative milliseconds. A defaults file contains a JSON object of registered queue default attributes; `{}` clears all queue defaults on update. Omitted update fields stay unchanged.

The daemon automatically purges eligible terminal history and emits runnable-wait
warnings. Retention inherits 30 days unless configured otherwise; zero is
unlimited. The default warning threshold is 10,000 ms; zero disables warnings
without disabling measurement. See [retention and replay lifetimes](operations.md#retention-and-replay-lifetimes)
and the [warning guarantee](observability.md#delay-warning-guarantee) before
changing these settings.

```sh
printf '{"retry.max_attempts":2}\n' > queue-defaults.json
jobuctl queue create reports \
    --defaults-file queue-defaults.json --history-retention-seconds 86400
# Use the revision from the preceding get; fetch again before a separate update.
jobuctl queue get reports
jobuctl queue update reports --revision 6 --history-retention-seconds 0
jobuctl queue get reports
jobuctl queue update reports --revision 7 --inherit-history-retention
```

Job create/update accept repeated `--attribute NAME=JSON_VALUE` for distinct registered job attributes. JSON values keep their types, so a numeric value is written as `--attribute retry.max_attempts=2`. On update, supplied attributes replace those named values; omitted attributes remain unchanged. Flag-based `job update` requires the current `--revision`, and `--clear-name` explicitly clears the name. Interactive editing captures the revision shown in its draft. A stale revision is returned as a conflict; the CLI does not fetch a newer revision and retry.

For CLI jobs, repeat `--arg`, `--arg-secret`, `--env`, `--env-secret`,
`--unset-env`, and `--expected-exit-code` as needed. For HTTP jobs, use `--url`,
optional `--method`, repeated `--header NAME=VALUE`, and optional `--body TEXT`
for a UTF-8 body. CLI and HTTP fields cannot be mixed.

HTTP methods are `GET`, `POST`, `PUT`, `PATCH`, `DELETE`, and `HEAD`.
Input is case-insensitive: `--method pOsT` stores and executes `POST`.
Omitting the method stores an explicit `GET`. Whitespace, empty values, and
other methods such as `OPTIONS` are rejected. The daemon enforces this policy
for flags, request files, and other clients. Some malformed payloads are rejected
locally before connecting. `HEAD` rejects a supplied body, including an empty
one, and receives no response body; other existing method/body behavior is
unchanged.

```sh
jobuctl job create --queue-name reports --type http --now \
    --url http://127.0.0.1:8080/events --method pOsT --body 'event=ready'
```

Use `--request-file` for complete nested configuration, binary HTTP bodies,
HTTP secret references, or a full job type/payload replacement on update. For
example, a job request file can contain a header value such as
`{"secret":"service.token"}`; the secret name must already exist on the daemon.

Older HTTP jobs retain their exact method spelling and execution behavior when
read or updated in unrelated fields. A same-type update without a replacement
payload also preserves it. Explicit payload replacement applies current
admission; changing priority does not repair a legacy lowercase or custom
method. Started and historical run snapshots remain unchanged.

For a complete replacement, put `job_id`, the current `expected_revision`, and
the complete `payload` in the file, following this
[HTTP update example](protocol/examples/job-update-http.params.json), then run:

```sh
jobuctl job update --request-file update-http.json
```

The file carries the job ID; this form takes no positional UUID. A stale
revision or rejected payload leaves the stored definition and revision intact.

`queue suspend --wait` returns when the queue reaches `suspended`. An accepted
`job suspend --wait` returns when the job reaches `suspended` or when its last
one-time run drains to `succeeded`, `failed`, or `cancelled`. The command returns
the actual observed job state; a `failed` job outcome does not make the wait
command fail. Both commands submit suspension once and use read requests while
waiting. The same overall `--timeout` applies. A timeout after an observed
suspension reply means the final state was not confirmed; inspect the resource
before deciding what to do next.

## Interactive queue updates

Run a bare update to inspect and edit a live queue:

```sh
jobuctl queue update reports
jobuctl queue update --name reports --editor 'code --wait'
jobuctl --timeout 10000 queue update reports --json > updated-queue.json
```

The positional selector and `--id UUID` / `--name NAME` aliases follow the
[queue selector rules](#connecting-to-the-daemon). This mode takes no mutable
fields, `--revision`, or `--request-file`. `--editor COMMAND` is valid only in
interactive mode. Fields without a revision, or a revision without fields,
remain usage errors. Scripts can use an explicit protected update or strict
request file:

```sh
jobuctl queue update id:019a0c70-1234-7000-8000-0123456789ab --revision 6 --weight 2
jobuctl queue update --request-file update-request.json
```

Interactive editing requires terminal stdin and stderr and an openable foreground
controlling terminal. Redirected stdout is allowed; editor display stays on the
terminal. Editor selection is `--editor`, nonempty `VISUAL`, nonempty `EDITOR`,
then `vi`. Commands are parsed into arguments without shell evaluation, and the
absolute draft path is appended as one argument. A GUI editor must wait for the
save/close, for example `code --wait`. See [editor selection and waiting](#editor-selection-and-waiting)
for quoting and cancellation details shared with job editing.

The initial fetch resolves the selector, checks any inferred-UUID ambiguity, and
captures the live queue's UUID, revision, and editable values under one network
budget. The connection closes before the editor starts.
Submission receives a separate full `--timeout`, including connection and
handshake, and uses the captured UUID/revision without another selector lookup.
File work and editor time consume neither network budget.

### Queue draft semantics

The protected `draft.jsonc` starts with queue-specific `//` instructions and the
current editable configuration, for example:

```jsonc
// JSONC draft: comments are ignored; trailing commas are not allowed.
// Keep queue_id and expected_revision unchanged.
// Omitted top-level fields leave server values unchanged.
// defaults replaces the whole queue-default layer; {} clears it.
// Set history_retention_seconds to null to inherit the daemon policy.
// Exit without effective changes to cancel the update.
{
  "queue_id": "019a0c70-1234-7000-8000-0123456789ab",
  "expected_revision": 6,
  "name": "reports",
  "weight": 1,
  "concurrency_limit": 1,
  "recovery_policy": "fail_interrupted",
  "defaults": {"retry.max_attempts": 2},
  "history_retention_seconds": 86400,
  "runnable_wait_warning_ms": 10000
}
```

The CLI fills these values from the fetched queue; do not copy this illustrative
configuration over another queue. `queue_id` and `expected_revision` must remain
present and unchanged. Adding `queue_name`, state, timestamps, unknown members,
or invalid retained values fails validation. The draft contains only the partial
queue-default layer, without daemon defaults or existing jobs' materialized
attributes. It never fetches secret values.

| Edit | Meaning |
| --- | --- |
| Leave a value unchanged, reorder object keys, or change only comments | Omit it from the update |
| Remove a writable top-level field | Leave that server value unchanged |
| Change name, weight, concurrency, recovery policy, or warning delay | Replace the complete changed value |
| Change `defaults` | Replace the entire partial queue-default layer |
| Remove a member inside a retained, changed `defaults` object | Remove that queue override |
| Set `defaults` to `{}` | Clear all queue-default overrides |
| Remove the top-level `defaults` field | Leave queue defaults unchanged |
| Set `history_retention_seconds` to `null` | Restore daemon-policy inheritance |
| Set retention to `0` or a positive integer | Unlimited retention or an explicit duration in seconds |
| Remove the top-level retention field | Leave retention unchanged |

`defaults: null` and null for other non-nullable fields are invalid. Integer-only
fields reject floating-point values such as `1.0`. All retained values are
validated before no-op detection. Queue defaults replace a whole layer; job
editing instead sends individual attribute patches, where removing an attribute
entry leaves its stored value unchanged. Queue-default changes affect newly
created jobs; existing materialized job attributes stay unchanged.

Drafts accept `//` and `/* ... */` comments wherever JSON permits whitespace.
Comment markers inside strings remain data; trailing commas are rejected.
Parsing preserves saved bytes. An unchanged draft prints
`No changes; no update submitted` on stderr, exits 0, and produces no stdout or
mutation, including with `--json`. A successful update prints the ordinary full
queue result with its new revision. Probe replies never appear on stdout.

### Queue files, conflicts, and recovery

Queue editing uses the same private mode-0700 directory, mode-0600 files, safe
atomic-save checks, and cleanup rules as [job editing](#retained-files-and-recovery).
The complete JSONC draft, including comments, is bounded by **16 MiB** and normal
JSON depth limits. Before submission, the CLI writes and closes a compact,
strict-JSON `update-request.json` containing only the captured UUID/revision and
changed top-level fields. That request has the separate **1 MiB** request limit.
Ordinary `--request-file` input rejects comments even with a `.jsonc` extension
and sends every supplied field without interactive diffing.

For example, clearing the displayed defaults and restoring retention inheritance
produces [these checked minimal params](protocol/examples/queue-update-recovery.params.json).
Other unchanged fields are absent. A retained complete request gets a
POSIX-shell-quoted recovery command preserving the resolved socket and timeout:

```sh
'jobuctl' '--socket' '/private/jobud.sock' '--timeout' '5000' 'queue' 'update' '--request-file' '/private/edit/update-request.json'
```

The CLI displays that command for manual recovery; it never runs it. A concurrent
configuration change, rename, suspend/resume, or completed suspension invalidates
the captured revision. On conflict, the draft and minimal request remain with the
**original revision**. Inspect the same UUID and reconcile before preparing
another update; blindly retrying the saved request does not fix the conflict.
Deletion or removal returns the ordinary lifecycle/not-found error. Reusing the
old name cannot redirect the saved UUID to a replacement queue.

After a lost mutation reply or other unknown outcome, inspect current server
state before any retry because the update may already have committed. There is
no automatic refresh, retry, merge, or editor relaunch. Invalid/unsafe drafts
retain available work but have no submit-ready request or recovery command;
prepare strict minimal JSON deliberately, removing comments and preserving the
original identity/revision. A missing draft requires a new interactive edit.

Success, no-op, and editor cancellation/launch failure clean known files.
Unexpected backup files remain, and cleanup failure returns nonzero even after
a successful mutation has printed its result. Inspect that result before another
mutation. Diagnostics report safely escaped paths without printing draft/default
contents; keep retained files private.

## Interactive job updates

Run a bare update to inspect and edit a job definition:

```sh
jobuctl job update JOB_UUID
jobuctl job update JOB_UUID --editor 'code --wait'
jobuctl --timeout 10000 job update JOB_UUID --json > updated-job.json
```

This mode takes no mutation flags, `--revision`, or `--request-file`.
`--editor COMMAND` is exclusive to interactive updates. A mutation flag without
a revision remains an error, and a revision without any mutation is also an
error. Scripts can use either existing noninteractive form:

```sh
jobuctl job update JOB_UUID --revision 7 --priority 50
jobuctl job update --request-file update-request.json
jobuctl job update --request-file - < update-request.json
```

A request file contains `job_id`, `expected_revision`, and the fields to update.
It takes no positional UUID and sends every explicitly present field through
the ordinary strict JSON decoder; it does not repeat the interactive diff.

Interactive editing requires terminal stdin and stderr and an openable
foreground controlling terminal. A pipe or redirected stderr is refused before
fetching the job. Redirected stdout is allowed: the editor uses the controlling
terminal for all three standard descriptors, keeping final stdout available for
the command result.

### Editor selection and waiting

Selection uses `--editor`, then nonempty `VISUAL`, then nonempty `EDITOR`, then
`vi`. An explicit empty command is invalid. For example:

```sh
VISUAL='code --wait' jobuctl job update JOB_UUID
jobuctl job update JOB_UUID --editor '"/opt/My Editor/bin/editor" --wait'
```

The outer quotes in these examples belong to your shell. The selected command
is split into arguments without running a shell. Single quotes preserve their
contents literally; double quotes preserve whitespace and allow backslash
escapes for `"`, `\`, `$`, backtick, and newline. Outside quotes, a backslash
escapes the next character; a backslash-newline continues the command. Adjacent
quoted and unquoted fragments form one argument. Unterminated quotes, a trailing
backslash, NUL, or an empty executable are rejected. Variables, substitutions,
globs, pipes, redirection, and shell operators are not evaluated. Use an explicit
wrapper executable if your editor needs shell behavior. The absolute draft path
is appended as one argument.

A graphical editor must wait until editing finishes, for example `code --wait`.
An editor process that immediately exits cannot tell the CLI when a background
window is saved. Nonzero editor exit, signal termination, or launch failure
cancels without submitting an update and returns nonzero. Exiting successfully
without effective changes is a successful no-op.

### Draft and update semantics

The CLI fetches one definition, retains its baseline, and destroys the network
session before opening the editor. Fetch and submission each receive the full
`--timeout`, including connection and handshake. File work and editor time use
neither budget. There is no automatic refresh, retry, merge, or second editor
pass.

The `draft.jsonc` file begins with `//` instruction comments so they remain visible
inside the editor. Interactive drafts accept `//` line comments and `/* ... */`
block comments wherever JSON permits whitespace. Comment markers inside strings
remain data. Trailing commas are rejected. Comment-only changes are a no-op;
parsing never rewrites the saved file. Comment support applies only to interactive
drafts. Request files and `update-request.json` remain strict JSON.

The JSON contains `job_id`, `expected_revision`, and writable definition fields.
Keep identity and revision present and unchanged. Response-only properties,
unknown fields, and invalid retained values are rejected. Secret references stay
references; the CLI does not fetch or expand secret values.

| Edit | Meaning |
| --- | --- |
| Leave a value unchanged or reorder object keys | Omit it from the update |
| Remove a writable top-level property | Leave the server value unchanged |
| Change `name`, `type`, `priority`, or `schedule` | Replace the complete changed value |
| Set `name` to `null` | Clear the name |
| Change or add an attribute | Send that entry as an attribute patch |
| Remove an attribute entry | Leave that server attribute unchanged |
| Change `payload` | Replace the whole payload; no recursive merge |

Array order matters. Schedule replacement uses the normal request schema;
symbolic `at: "now"` remains creation-only. Null is valid only where that schema
allows it. Queue moves and lifecycle changes use their separate commands.
An unchanged legacy HTTP payload is omitted from the minimal update, preserving
its exact method spelling. Explicit payload replacement uses current admission
and canonicalizes supported HTTP methods.

No effective change prints `No changes; no update submitted` on stderr, exits 0,
and produces no stdout or update RPC, including with `--json`. A successful
mutation prints the ordinary update result.

### Retained files and recovery

The CLI creates a private mode-0700 directory and mode-0600 `draft.jsonc`. It
accepts a safe editor atomic save, then reopens without following symlinks and
requires an owner-only regular file owned by the current user. Missing, empty,
unsafe, or invalid saved drafts fail without submission. Generated/read drafts,
including instructions, are bounded by 16 MiB and normal JSON depth limits.
Compact requests retain the ordinary 1 MiB request-file limit and existing
payload/RPC limits.

Once a valid minimal update exists, the CLI writes and closes
`update-request.json` before submitting. That strict JSON contains only the
original identity/revision and changed fields. It is the recovery request;
submitting the complete draft could replace an unchanged legacy payload.

| Outcome | Files and next action |
| --- | --- |
| Success, no-op, or editor cancellation/launch failure | Clean known files and the empty directory |
| Missing saved draft | Retain the directory and report the missing draft path; start a new interactive edit to recreate changes |
| Invalid or unsafe saved draft | Retain available evidence; correct the draft into a valid minimal strict JSON request before submitting |
| Failure saving a complete minimal request | Retain the draft; no update is sent and no ready recovery command is advertised |
| Rejected or failed submission | Retain draft and minimal request; inspect the error before retrying |
| Revision conflict | Fetch the current definition and manually reconcile; the saved request still has the old revision |
| Unknown outcome | Inspect current server state before any retry; the update may already have succeeded |

Recovery messages identify the retained directory and label the draft path as
retained or missing. Only a complete minimal request gets a path and a
POSIX-shell-quoted command preserving socket and timeout. For example:

```sh
'jobuctl' '--socket' '/private/jobud.sock' '--timeout' '5000' 'job' 'update' '--request-file' '/private/edit/update-request.json'
```

The CLI displays this command; it never executes it. A conflict is not fixed by
blindly retrying the old revision, and an unknown outcome needs inspection first.
For an invalid draft there may be no minimal request: remove all JSONC comments
and deliberately select the intended changed fields when preparing a
strict JSON request. Request-file mode sends all present fields.

Diagnostics do not print draft contents. Keep retained files private because
edited payloads can contain literal sensitive values. Paths and command text
escape terminal controls for display, separately from shell quoting; if a path
contains controls, displayed escapes are not literal filename bytes.
Cleanup removes only known files and attempts to remove the empty directory.
Unexpected editor backups remain, with the retained directory reported. Cleanup
failure returns nonzero even after a successful mutation has printed its result;
inspect the result/server state before deciding on another mutation.

## Runs and attempts

`job run-now JOB_UUID` creates a manual run from an eligible unfinished job
definition. It preserves the future scheduled occurrence; a new request for a
`succeeded`, `failed`, or `cancelled` job is rejected. Use `--idempotency-key KEY`
when a retry after a lost response must return the original run. A matching
retained key replays its saved response even if the job has since finished,
without creating new work.

Manual replay records are retained with their runs and retired atomically when
cleanup purges terminal history. After retirement, the same key is a new request
subject to current eligibility; it cannot reopen a terminal definition. Creation
keys also follow [resource lifetimes](protocol/methods/job.md), so an old retry
can become a new creation after its key retires. The daemon applies
[automatic history retention](operations.md#retention-and-replay-lifetimes);
replay records remain available only for their documented resource lifetimes.

```sh
jobuctl job run-now JOB_UUID --idempotency-key manual-42 --json
jobuctl run get RUN_UUID --json
jobuctl run list --job-id JOB_UUID --state failed --limit 20
jobuctl run list --state failed --state interrupted
jobuctl run list --all
```

Ordinary `run list` selects terminal execution history: `succeeded`, `failed`,
`interrupted`, and `cancelled` runs. This differs from `job list`, which selects
active job definitions by default. The terminal selection also applies with
other filters, such as `--job-id`, and in `--json` mode.

Use `--state STATE` to replace the default with one of `scheduled`, `running`,
`retry_wait`, `succeeded`, `failed`, `interrupted`, or `cancelled`. Repeat the
option for a union, for example `--state failed --state interrupted`. State names
are lowercase; duplicates and comma-separated values are rejected. `--all`
removes the state filter and cannot be combined with `--state`.

`run list` also accepts `--queue-id`, `--job-id`, `--origin scheduled|manual`,
`--type cli|http`, and the UTC bounds `--planned-from/--planned-to`,
`--started-from/--started-to`, and `--completed-from/--completed-to`. Lower bounds
are inclusive and upper bounds are exclusive. These filters combine with the
selected states using AND. `--limit` is 1–200, default 100. The result contains
run summaries, without payload, result, attempts, or output. Full run details
are available through `run get`.

Human `run get RUN_UUID` prints the run details, then retrieves its newest
attempt with `attempt.list` (limit 1) and `attempt.get`. The compact Last attempt
section shows its number, durable state/outcome, available start/completion
times, and bounded diagnostics such as a CLI exit code or HTTP status. Pending
and running attempts remain explicit. If the attempt changes between queries,
the fetched details supply the section's state. Captured output bytes and the
full attempt result are not included.

No attempts prints `Last attempt: none` and succeeds. Failure to retrieve the
attempt list or details preserves the run information already printed and any
available attempt summary, reports the normal error on stderr, and exits
nonzero. Connection, handshake, and all three queries share one overall
`--timeout`; enrichment does not start a new budget. Successfully inspecting a
failed execution still exits 0.

`run get RUN_UUID --json` performs only `run.get` and prints its existing result
schema, without attempt calls or additional fields. The human output hint
describes latest-completed output selection, which may refer to an older
attempt than a running retry.

`run output RUN_UUID --channel CHANNEL` retrieves one chunk from the latest
completed retained attempt. Use `stdout`/`stderr` for CLI runs or `body`/`headers`
for HTTP runs. Optional slice arguments are `--offset N` (default 0, at most
9,223,372,036,854,775,806) and `--limit N` (1–65,536, default 16,384).
`--raw`, `--json`, and `--output-file PATH` are mutually exclusive. The shared
delivery contract is the existing `AttemptOutputChunk`: human metadata identifies
the selected attempt, JSON keeps the protocol chunk shape, and raw/file delivery
writes only decoded bytes. Empty or unavailable chunks retain explicit attempt
output behavior and succeed when retrieved successfully.

The command checks run existence/type, then traverses bounded newest-first
attempt pages until it finds a completed attempt. Multiple newer pending/running
attempts and short pages are supported. Connection, handshake, run lookup, all
attempt pages, and the single output read share one overall `--timeout`.
No retained attempts and no completed retained attempt produce distinct errors
with exit status 1, without creating an output file. An incompatible channel is
a local input error (exit 2); a missing run retains the normal remote error.

Once selected, the exact attempt is fixed. A concurrently completing retry does
not trigger reselection. Retention removal or a read error fails normally;
`not_captured`, `lost`, or empty output never causes fallback to an older attempt.
These lookups observe live history rather than an atomic snapshot. One invocation
reads one chunk, even when `next_offset` indicates more bytes. Use an explicit
`attempt output RUN_UUID NUMBER` request to continue reading that same attempt.

There is no `run.output` RPC or capability. `--request-file` is rejected locally;
`run output --help` still succeeds without reading supplied files or standard input.

The terminal default and multiple explicit states require daemon API 1.5 or
newer. An older peer produces an unsupported-feature error before the query is
sent. A single explicit `--state` uses the compatible scalar form, and `--all`
uses an unfiltered form. The CLI does not split queries or filter fetched pages.

`run list --request-file FILE` preserves the supplied protocol selection through
the normal typed decode/encode path. In particular, `{}` selects all states,
including nonterminal runs; no CLI terminal default is added. Request files
cannot be combined with command-local options such as `--state` or `--all`.

When a run page has `next_cursor`, pass it as `run list --cursor TOKEN` with no
filters, `--all`, or limit. Continuation retains the original query selection;
no new default is added. A cursor can expire or become invalid after daemon
restart. This cursor differs from the ID-based `--after` option on queue/job lists.

`run cancel RUN_UUID` succeeds when the daemon returns either `completed` or `requested`. A `requested` reply means active work is still settling. Add `--wait` to submit cancellation once and observe `run.get` until the run is durably `cancelled`, another terminal state causes a conflict, or the overall deadline expires. Set `--timeout` long enough for the job's process termination grace when waiting on active work. After a wait timeout, inspect the run before deciding what to do next. A successful `run get` can describe a failed run without changing the CLI's exit status.

```sh
jobuctl run cancel RUN_UUID --wait --json
jobuctl attempt list RUN_UUID --limit 20 --json
jobuctl attempt get RUN_UUID 1 --json
```

`attempt list` returns newest attempt numbers first. Its summaries contain no
result or output. Continue with `attempt list --cursor TOKEN` and no run ID or
limit. `attempt get RUN_UUID NUMBER` returns one attempt's details; `NUMBER`
must be from 1 through 9,223,372,036,854,775,807 (`INT64_MAX`). The same range
applies to `attempt output`.

`attempt output RUN_UUID NUMBER --channel CHANNEL` reads one retained chunk. Use `stdout` or `stderr` for a CLI job, and `body` or `headers` for an HTTP job. `--offset N` addresses retained bytes, default 0; `--limit N` requests 1–65,536 raw bytes, default 16,384. Follow `next_offset` to read another chunk. Truncated output may have an omitted middle section; retained offsets do not recover omitted bytes. The response distinguishes available, pending, not-captured, and lost output, and reports retained size, known total/omitted size, truncation, and capture loss.

By default, output shows metadata and an escaped text or base64 preview. `--json` prints the complete protocol result. `--raw` writes only this chunk's decoded bytes to standard output, including binary bytes. `--output-file PATH` writes only this chunk to a newly created file and refuses an existing path. These three modes are mutually exclusive.

```sh
jobuctl attempt output RUN_UUID 1 --channel stdout --offset 0 --limit 4096 --json
jobuctl attempt output RUN_UUID 1 --channel stdout --raw > chunk.bin
jobuctl attempt output RUN_UUID 1 --channel stdout --output-file chunk.bin
```

## Retained statistics

`system stats` summarizes runs across queues; `queue stats` requires exactly one
initial selector, positional `SELECTOR`, `--id UUID`, or `--name NAME`. Both accept `--job-id UUID`,
`--type cli|http`, `--origin scheduled|manual`, `--planned-from UTC`,
`--planned-to UTC`, `--group-by none|queue|job|type|origin|state`, and `--limit N`
(1–200, default 100). `system stats` also accepts `--queue-id UUID` as a filter.
The planned window is half-open and at most 31 days. Omit its upper bound to use
the daemon's current UTC time; omit its lower bound to use 24 hours before the
resolved upper bound.

Initial CLI name resolution and bare-UUID ambiguity probes include retained
deleted queues. Names prefer a live queue, then a unique historical match;
ambiguous historical names fail with `jobu.queue.ambiguous_deleted_name` and
require an explicit ID. The final query retains the requested filters and
window and uses the resolved UUID. Explicit IDs can address retained deleted
queues directly. Cursor-only continuation performs no selector lookup.

```sh
jobuctl system stats --group-by state --json
jobuctl queue stats reports \
    --planned-from 2030-01-01T00:00:00Z --planned-to 2030-01-02T00:00:00Z \
    --group-by job --limit 20
```

The result includes the resolved window, groups, a nullable `next_cursor`, and
measurement provenance. Run and attempt counts cover the planned-run cohort and
all its retained attempts. Lateness and execution duration are derived from
wall-clock timestamps; averages and maxima are null when there are no samples.
Runnable wait uses Complete terminal runs once per run across all attempts,
with `monotonic_observed` provenance and milliseconds. Human output also
prints `complete`, `partial`, `unmeasured`, and `unfinished` coverage counts.
No Complete samples means zero samples and null average/maximum; a measured
zero wait is a real zero-valued sample. Partial lower bounds and unfinished
runs do not enter the wait average. Uninstrumented embedded services may
return null/unavailable duration. See the [statistics contract](protocol/types.md#statistics-page)
for quality, eligibility and retention limits. With `--json`, the CLI prints
the protocol result unchanged.

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
instant. Both accept `--timezone ZONE`; omission uses the daemon default
(normally UTC). `schedule next` also accepts `--count N` (1–200, default 5). The timezone controls calendar matching;
results are UTC. Invalid expressions and timezones are reported as errors.

```sh
jobuctl schedule validate '0 9 * * FRI-MON'
jobuctl schedule next '@daily' \
    --timezone Europe/Tallinn --after 2030-01-01T00:00:00Z --count 2 --json
```

The cyclic weekday range `FRI-MON` includes Friday through Monday. These
commands do not create or change a job. Both accept `--request-file` containing
their complete [schedule method](protocol/methods/schedule.md) params object.
See [Cron schedules](cron.md) for the five-field grammar, exact alias meanings,
timezone behavior, and examples.

## Named secrets

`secret set NAME` creates or rotates a secret. Supply exactly one of `--file PATH`
or `--stdin`; both read raw bytes, including NUL bytes and a final newline,
without text conversion or trimming. Empty values are valid; the maximum is
65,536 bytes. The command line has no literal secret-value option. Put the value
in a file with access restricted to trusted users, or pass it through standard
input. The CLI does not print the value, its size, or a preview.

```sh
jobuctl secret set reports.token --file /private/token.bin
jobuctl secret set reports.token --stdin < /private/next-token.bin
jobuctl secret list --limit 20 --json
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

## Secret references in CLI jobs

`job create` and `job add` accept `--arg-secret SECRET_NAME` and
`--env-secret NAME=SECRET_NAME` alongside literal CLI options. They pass secret
names in the job template; they do not read or put secret values in the create
request. The named secrets must exist on the daemon. Both options also accept
attached values, such as `--arg-secret=reports.token` and
`--env-secret=REPORT_TOKEN=reports.token`.

```sh
jobuctl job create \
    --queue-name reports --type cli --at 2030-01-01T00:00:00Z \
    --command /usr/local/bin/report \
    --arg=--token --arg-secret reports.token --arg=--mode --arg=daily \
    --env MODE=daily --env-secret REPORT_PASSWORD=reports.password
```

Arguments retain the order of literal `--arg` and `--arg-secret` options. Each
secret reference replaces one whole argument or environment value; the CLI does
not interpolate part of a string. Environment names must be unique across
`--env`, `--env-secret`, and `--unset-env`, regardless of order. References cannot
target `PATH` or a `JOBU_*` environment name. The existing argument and template
reference limits apply. These options cannot be mixed with `--request-file` or
used for HTTP jobs. Replace an existing payload through interactive editing or
a complete request file; these creation flags do not apply to `job update`.

## Literal arguments

Repeat `--arg` to pass multiple arguments to a CLI job. Each value becomes one argument, preserving order and empty strings. Use `--arg=VALUE` for values beginning with a dash, especially values that resemble `jobuctl` options:

```sh
jobuctl job create \
    --queue-name reports --type cli --at 2030-01-01T00:00:00Z \
    --command /bin/echo --arg=--help --arg=-h --arg= --arg='two words'
```

Here `--help` and `-h` are arguments to the scheduled program. They do not
request `jobuctl` help. Attached values such as `--name=--help` are also literal
values.
An existing `--arg --arg-secret` or `--arg --env-secret` still passes the option
spelling as a literal argument; use `--arg=VALUE` for any option-like value when
writing a new command.

`--` ends option handling. Following tokens are positional data where the command accepts them; otherwise they are extra-operand errors. For example, this creates a queue named `--help`:

```sh
jobuctl queue create -- --help
```

## Results and errors

Human output escapes control characters in data supplied by the daemon, including
names and error text. Queue, job, and secret names, name-based deletion selectors,
and secret name cursors use the same display format. Ordinary names and internal
ASCII spaces remain unquoted. Empty names, leading/trailing spaces, quotes,
backslashes, controls, and invisible Unicode characters trigger single quoting.
Quotes and backslashes are escaped inside the delimiters. Non-ASCII Unicode
whitespace and characters normally ignored when displaying text, including
zero-width spaces and bidi controls, appear as visible numeric escapes
(`\uXXXX` or `\UXXXXXXXX`).

The stored-name column below uses JSON notation to make spaces and invisible
characters explicit:

| Stored name (JSON notation) | Human name text |
| --- | --- |
| `"Default"` | `Default` |
| `"Default "` | `'Default '` |
| `" Default"` | `' Default'` |
| `" Default "` | `' Default '` |
| `"My Queue"` | `My Queue` |
| `"O'Reilly"` | `'O\'Reilly'` |
| `"path\\name"` | `'path\\name'` |
| `"Default\u00a0"` | `'Default\u00A0'` |
| `"Default\u200b"` | `'Default\u200B'` |

These delimiters and escapes are for display; they are not shell quoting.
Names retain their existing validation rules. Formatting never trims or
normalizes a name: lookup of `Default` still fails if the stored name is
`Default `, and the exact trailing space must be supplied to select that queue.

JSON mode preserves the original name, apart from standard JSON escaping. It
prints exactly one result value and a newline on standard output; successful
delete commands print `null`. It prints no headings or progress text. Integer
values remain integers, including large revisions.

For an ordinary command failure, standard output is empty. With `--json`, each
error is represented on stderr by an object with `error.kind` (`local` or
`remote`), a stable string `code`, nullable `rpc_code` and `category`, safe
`message`, and boolean `outcome_unknown`. A remote application error retains its
domain code and category. A represented remote RPC error without application
data uses `jobuctl.remote.rpc_error` as the string code.

Interactive no-op has no stdout result and prints a human stderr notice.
Interactive failures can add human recovery lines alongside the JSON error;
stderr as a whole is not necessarily one JSON document. If cleanup fails after
a successful update, stdout already contains the normal result and the exit
status is nonzero. See [retained files and recovery](#retained-files-and-recovery).

| Exit status | Meaning |
| --- | --- |
| 0 | Successful remote operation, interactive no-op, or local help/version. |
| 1 | Represented operation failure, such as not found, revision conflict, or an unsupported capability; editor cancellation/nonzero exit. |
| 2 | Syntax, local request decoding, request-file input, terminal preflight, editor-command parsing, or draft preparation failure. |
| 3 | Connection, protocol, deadline, or editor launch failure; cleanup failure after an otherwise successful operation. |

For a mutation, `outcome_unknown: true` means the request may have reached the daemon without an observed result. Check the resource before deciding whether to repeat it. Creation requests with a retained explicit idempotency key support replay; job updates have no such key. For an interactive update, inspect current state and manually reconcile before any retry. The CLI does not retry mutations automatically.
