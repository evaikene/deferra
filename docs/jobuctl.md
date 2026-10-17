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

`--request-file` is available for every command listed above. It cannot be combined with command operands or command-specific request options. Global options, supported `--wait`, and `attempt output` delivery options remain available. The file must contain one JSON object, with no trailing non-whitespace text, and is limited to the configured RPC body size (1 MiB by default). Request fields follow the public method's strict JSON contract. For example:

```sh
printf '{"limit":20}\n' > queue-list.json
jobuctl queue list --request-file queue-list.json
```

Help and version finish without reading a request file or standard input. `--json` does not change help into JSON.

Queue operations select a queue with exactly one of `--id UUID` or `--name NAME`. Job creation and moving use exactly one of `--queue-id UUID` or `--queue-name NAME`. Flag-based job updates, moves, and deletes require `--revision N`; use the current revision shown by `job get`. A bare `job update UUID` instead [opens an interactive draft](#interactive-job-updates) and captures the inspected revision.

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
jobuctl queue update --name reports --history-retention-seconds 0
jobuctl queue update --name reports --inherit-history-retention
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

The draft begins with `jobu:` instructions so they remain visible inside a
terminal editor. Every physical line starting with the exact case-sensitive
bytes `jobu:` in column 1 is ignored during draft parsing, wherever it occurs.
Indenting the prefix or changing its case makes it ordinary input. A `jobu:` key
or value inside JSON remains data. Instruction-only changes are a no-op; parsing
never rewrites the saved file. This convention applies only to interactive
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

The CLI creates a private mode-0700 directory and mode-0600 `draft.json`. It
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
| Invalid, missing, or unsafe saved draft | Retain available evidence; correct the draft into a valid minimal strict JSON request before submitting |
| Failure saving a complete minimal request | Retain the draft; no update is sent and no ready recovery command is advertised |
| Rejected or failed submission | Retain draft and minimal request; inspect the error before retrying |
| Revision conflict | Fetch the current definition and manually reconcile; the saved request still has the old revision |
| Unknown outcome | Inspect current server state before any retry; the update may already have succeeded |

Recovery messages identify the retained directory/draft and, only when complete,
the minimal request and a POSIX-shell-quoted command preserving socket and
timeout. For example:

```sh
'jobuctl' '--socket' '/private/jobud.sock' '--timeout' '5000' 'job' 'update' '--request-file' '/private/edit/update-request.json'
```

The CLI displays this command; it never executes it. A conflict is not fixed by
blindly retrying the old revision, and an unknown outcome needs inspection first.
For an invalid draft there may be no minimal request: remove `jobu:` instruction
lines and deliberately select the intended changed fields when preparing a
strict JSON request. Request-file mode sends all present fields.

Diagnostics do not print draft contents. Keep retained files private because
edited payloads can contain literal sensitive values. Paths and command text
escape terminal controls for display, separately from shell quoting; if a path
contains controls, displayed escapes are not literal filename bytes.
Cleanup removes only known files and attempts to remove the empty directory.
Unexpected editor backups remain, with the retained directory reported. Cleanup
failure returns nonzero even after a successful mutation has printed its result;
inspect the result/server state before deciding on another mutation.

### Manual editor verification checklist

This checklist prepares the maintainer-assisted Linux U.20 gate. Automated tests
use in-process editor seams and do not establish real terminal/editor acceptance.
Record source commit, OS, terminal, terminal-editor version, waiting GUI-editor
version (or unavailable), and pass/fail/pending plus observations for each case.
Use no real secrets. An unavailable prerequisite leaves its case pending.

As an ordinary user, start a disposable daemon in terminal A:

```sh
umask 077
jobu_edit=$(mktemp -d /tmp/jobu-edit-check.XXXXXX)
printf 'Checklist directory: %s\n' "$jobu_edit"
jobud --no-config --socket "$jobu_edit/jobud.sock" --database "$jobu_edit/jobu.sqlite3"
```

Wait for `jobud.ready`. In terminal B, replace the directory below with the
printed path, define the helper, and create a future job that will not execute
during editing. Copy its returned `id` into `job_id`:

```sh
jobu_edit=/tmp/jobu-edit-check.REPLACE
ctl() { jobuctl --socket "$jobu_edit/jobud.sock" "$@"; }
ctl queue create editor-check
ctl job create --queue-name editor-check --type cli --at 2099-01-01T00:00:00Z \
    --command /bin/true --name before --attribute retry.max_attempts=2 --json
job_id=REPLACE_WITH_RETURNED_ID
ctl job get "$job_id" --json
```

Use a future date if 2099 is no longer in the future. Repeat the helper setup in
any additional client terminal. Read the current revision from `job get` before
each explicit flag update; the checklist does not assume revisions stay at 1.

1. Run `ctl job update "$job_id" --editor vi` (or an available terminal editor).
   Confirm readable JSON and visible `jobu:` instructions, working input, fixed
   identity/revision, and wait/save behavior. Change priority to 50 and save.
   `ctl job get "$job_id" --json` must show one revision increment and priority 50.
2. Open again and leave values unchanged; repeat after reordering object keys,
   then after changing only a `jobu:` line. Each must report the no-op notice,
   leave revision unchanged, and clean its edit directory. Record the draft path
   from the editor's filename display to check cleanup after exiting.
3. Remove the top-level `name` property and save: name stays `before` with no
   mutation. Reopen and set `name` to null: it clears. Change
   `retry.max_attempts` to 3 and verify other attributes remain. Removing that
   attribute from another draft must not reset it.
4. Change payload to `{"command":"/bin/echo","arguments":["check"]}` and
   verify whole replacement. Separately create a future HTTP job with
   `ctl job create --queue-name editor-check --type http --at 2099-01-01T00:00:00Z --url http://127.0.0.1:9/ --method GET --json`.
   Edit its method to `pOsT` and confirm stored `POST`. On a disposable legacy
   HTTP fixture prepared from pre-admission data, change only priority: the old
   lowercase method must remain exact; explicitly replace the payload and check
   canonicalization. Do not weaken current admission to create this fixture;
   record the legacy case pending if no fixture is available.
5. Try `VISUAL='vi' EDITOR='/unavailable/editor' ctl job update "$job_id"`, then
   `VISUAL='' EDITOR='vi' ctl job update "$job_id"`. Confirm precedence and repeat
   with `--editor` overriding both. Use the quoted-path example with a locally
   available editor path containing spaces. With an available GUI editor, run
   `ctl job update "$job_id" --editor 'code --wait'` and confirm it waits for save
   and close. Record exact installed editors and any unavailable case.
6. Exit the terminal editor nonzero (for example vi `:cq`), then interrupt it
   with Ctrl-C as supported by that editor. Confirm no update, a nonzero status,
   restored terminal/signal behavior, and cleanup. Run with
   `--editor /unavailable/editor`: launch fails without mutation. Use `printf '%s\n' "$?"`
   immediately after each command to record its status.
7. Save invalid JSON, then a structurally valid draft with an invalid field in
   separate runs. Expect exit 2, empty stdout, unchanged server state, and retained
   draft bytes. Check that no complete minimal request/retry command is claimed.
   A zero-exit editor that empties or deletes the file must also fail usefully.
8. Run `ctl --timeout 2000 job update "$job_id"`, keep the editor open longer
   than two seconds, then save a priority change. Submission must receive a new
   full network budget and succeed against a responsive local daemon.
9. Keep an edit open in terminal B. In terminal C inspect the job and run
   `ctl job update "$job_id" --revision CURRENT_REVISION --priority 99`.
   Save a different priority in B: expect conflict, retained draft/minimal
   request with the old revision, and no overwrite of 99. In separate edits,
   changing or removing identity/revision must fail locally. Manually reconcile
   with a newly inspected definition before submitting again.
10. Open an edit, stop terminal A's daemon with Ctrl-C, then save a change.
    Expect nonzero, retained draft/minimal request, and an accurate recovery
    command carrying the selected socket/timeout. Restart A with the same command
    before continuing. Outcome-unknown behavior uses deterministic automated
    tests, not a manual connection-loss race. Optionally leave an editor backup
    in the protected edit directory: cleanup must preserve/report that directory.
11. Run `ctl job update "$job_id" --json > "$jobu_edit/result.json"` while
    stdin/stderr remain terminal descriptors. Confirm editor input still works
    and a successful change produces only one final JSON result in that file.
    Repeat unchanged: stdout must be empty and the notice on stderr. Recovery
    lines remain human stderr text even in JSON mode.
12. Run `printf '{}\n' | ctl job update "$job_id"`: expect terminal refusal,
    no editor, and no fetch. Run a flag mutation without revision: expect a
    syntax error. Redirect stdin from `/dev/null` for an explicit-revision update,
    and for `ctl job update --request-file FILE` with a fresh minimal strict JSON
    request containing `job_id`, current `expected_revision`, and `priority`:
    both remain noninteractive. Also try `--request-file - < FILE`. Adding a
    positional UUID to either request-file command remains a syntax error.

Stop the disposable daemon before removing its test data. Preserve the completed
record and any failed evidence; checklist preparation alone is not a passed gate.

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
can become a new creation after its key retires. The daemon currently performs
no automatic history cleanup.

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
initial selector, `--id UUID` or `--name NAME`. Both accept `--job-id UUID`,
`--type cli|http`, `--origin scheduled|manual`, `--planned-from UTC`,
`--planned-to UTC`, `--group-by none|queue|job|type|origin|state`, and `--limit N`
(1–200, default 100). `system stats` also accepts `--queue-id UUID` as a filter.
The planned window is half-open and at most 31 days. Omit its upper bound to use
the daemon's current UTC time; omit its lower bound to use 24 hours before the
resolved upper bound.

```sh
jobuctl system stats --group-by state --json
jobuctl queue stats --name reports \
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

Human output escapes control characters in data supplied by the daemon, including names and error text. JSON mode prints exactly one result value and a newline on standard output; successful delete commands print `null`. It prints no headings or progress text. Integer values remain integers, including large revisions.

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
