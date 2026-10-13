# JobU User Manual

JobU runs scheduled command-line programs and HTTP requests in the background.
You define the work once, and the `jobud` daemon starts it when it is due. JobU
keeps job definitions and execution history in SQLite, so they remain available
after a daemon restart.

Use `jobuctl` to manage JobU from a shell. It talks to `jobud` through a local
Unix-domain socket. Applications can use the same JSON-RPC protocol directly or
through the C++ control client.

## The basic model

| Term | What it means |
| --- | --- |
| **Queue** | Groups jobs and controls how much work can run at once. It also holds default job settings. |
| **Job** | A definition of a command or HTTP request, its schedule, and its execution settings. A job can run once or recur on a cron schedule. |
| **Run** | One scheduled or manually requested occurrence of a job. It keeps a snapshot of the definition used to create it. |
| **Attempt** | One try to execute a run. A retry adds another attempt to the same run. |

The daemon keeps the next scheduled run, waits for its due time and available
queue capacity, then starts an attempt. A failed attempt can be retried when
the job's settings allow it. Recurring jobs produce further runs at later
occurrences. You can change a job definition without rewriting the snapshots
held by earlier runs.

JobU supports timeouts, retries, cancellation, and bounded output capture. You
can inspect runs and attempts after execution, read retained output in chunks,
and query counts and timing statistics. Named secrets let a job refer to a
value that JobU resolves just before each attempt.

## Try a command-line job

As an ordinary non-root user, create a disposable private directory and start
the daemon in one terminal. Keep the printed directory path for the client:

```sh
umask 077
jobu_demo=$(mktemp -d /tmp/jobu-demo.XXXXXX)
printf 'Demo directory: %s\n' "$jobu_demo"
jobud --no-config --socket "$jobu_demo/jobud.sock" --database "$jobu_demo/jobu.sqlite3"
```

Wait for `jobud.ready` on stderr. The directory is mode 0700, owned by you;
database and socket files are owner-only. Raw paths in shared `/tmp` are unsafe.
This temporary location is for a demo, not persistent deployment or backups.

The daemon creates the current SQLite format (version 4) in an empty database
and validates it on later starts. It rejects older and newer formats without
changing them. If you have an earlier test database, keep it separately and
point the daemon at a fresh database file; changing its version marker does not
convert its contents.

Before opening the socket, the daemon validates current-format job and run
relationships and resolves work interrupted by a previous stop. Depending on
the queue policy, an interrupted run may wait for a retry or finish as failed.
When that run was the last outstanding work for a one-time job, the job also
becomes failed. A schema or recovery error prevents the daemon from serving.

On Linux, the daemon holds a separate `${socket.path}.lock` through shutdown,
so daemons using different databases cannot own the same endpoint. The lock
file remains after exit. Restart removes a crashed socket only after acquiring
both locks and proving that the unchanged, daemon-owned socket refuses a
connection. Live or ambiguous endpoints, symlinks and other file types are
preserved; deleting a lock pathname does not safely bypass another owner.

Socket mode defaults to `0600`. Configuring `socket.mode = 0660` and an
authorized `socket.group` grants that group's members full daemon authority,
including CLI work and secret management. This requires the protected runtime
directory to have the matching group and mode `0750`, with traversal through
its ancestors. Group access is an administrative trust boundary; it supplies
neither read-only monitoring nor per-user authorization.

In another terminal, set `jobu_demo` to the printed directory (replace the
example suffix below), then create a queue and a job due immediately:

```sh
jobu_demo=/tmp/jobu-demo.REPLACE
jobuctl --socket "$jobu_demo/jobud.sock" system info
jobuctl --socket "$jobu_demo/jobud.sock" queue create examples
jobuctl --socket "$jobu_demo/jobud.sock" job create \
    --queue-name examples --type cli --now \
    --command /bin/echo --arg='hello from JobU'
jobuctl --socket "$jobu_demo/jobud.sock" job list --all
jobuctl --socket "$jobu_demo/jobud.sock" run list
```

`jobud` must be running for remote `jobuctl` commands. This demo overrides the
endpoint, so its client commands use the same override. For an installation
using matching compiled defaults, `jobuctl system info` needs no `--socket`.
Stop the foreground daemon with Ctrl-C before removing its disposable directory.
`job list` alone shows only active definitions; `--all` also shows a one-time
job after it has finished.
Successful output is not retained by the default `output.capture=on_error`
policy. Set `output.capture` to `always` on a job if you need to inspect
successful output through `attempt output`.

Run `jobuctl --help`, `jobuctl job --help`, or a command such as
`jobuctl job create --help` for operands and options. Help works without a
running daemon.

## Where to go next

| Guide | Use it for |
| --- | --- |
| [Installation and SDK](installation.md) | Installing components and packages, provisioning accounts, and activating systemd deliberately |
| [Configuration](configuration.md) | Startup keys, defaults, units, precedence, and local validation |
| [Operations](operations.md) | Lifecycle, retention, backup/restore, recovery, and troubleshooting |
| [Observability](observability.md) | Logs, warning guarantees, timing quality, and retained statistics |
| [jobuctl command guide](jobuctl.md) | Creating and changing queues and jobs; inspecting runs, attempts, output, statistics, and cron previews |
| [Cron schedules](cron.md) | Writing recurring schedules, understanding aliases and timezones, and previewing occurrences |
| [Named secrets](secrets.md) | Supplying sensitive values and understanding storage, process, and output exposure |
| [JSON-RPC protocol](protocol/README.md) | Writing a client for the local socket, including method fields, errors, and examples |
| [C++ control client](cpp-client.md) | Using the typed asynchronous client from a C++ application |

JobU stores secret values as plaintext in its SQLite database. Protect the
database and its copies, and review the [secret guide](secrets.md) before
putting sensitive values into jobs. Retention automatically deletes eligible
terminal history, with a 30-day inherited default. Runnable-wait warnings are
active, with a 10-second default threshold and at-most-once delivery semantics;
see the operations and observability guides for their limits.
