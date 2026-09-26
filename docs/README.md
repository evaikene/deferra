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

Start the daemon with a socket and database path in one terminal:

```sh
jobud --socket /tmp/jobu.sock --database ./jobu.sqlite
```

The daemon creates the current SQLite format (version 3) in an empty database
and validates it on later starts. It rejects older and newer formats without
changing them. If you have an earlier test database, keep it separately and
point the daemon at a fresh database file; changing its version marker does not
convert its contents.

In another terminal, create a queue and a job due immediately:

```sh
jobuctl --socket /tmp/jobu.sock queue create examples
jobuctl --socket /tmp/jobu.sock job create \
    --queue-name examples --type cli --now \
    --command /bin/echo --arg='hello from JobU'
jobuctl --socket /tmp/jobu.sock job list
jobuctl --socket /tmp/jobu.sock run list
```

Use paths appropriate for your machine. `jobud` must be running for remote
`jobuctl` commands, and each command must use the daemon's socket path.
Successful output is not retained by the default `output.capture=on_error`
policy. Set `output.capture` to `always` on a job if you need to inspect
successful output through `attempt output`.

Run `jobuctl --help`, `jobuctl job --help`, or a command such as
`jobuctl job create --help` for operands and options. Help works without a
running daemon.

## Where to go next

| Guide | Use it for |
| --- | --- |
| [jobuctl command guide](jobuctl.md) | Creating and changing queues and jobs; inspecting runs, attempts, output, statistics, and cron previews |
| [Cron schedules](cron.md) | Writing recurring schedules, understanding aliases and timezones, and previewing occurrences |
| [Named secrets](secrets.md) | Supplying sensitive values and understanding storage, process, and output exposure |
| [JSON-RPC protocol](protocol/README.md) | Writing a client for the local socket, including method fields, errors, and examples |
| [C++ control client](cpp-client.md) | Using the typed asynchronous client from a C++ application |

JobU stores secret values as plaintext in its SQLite database. Protect the
database and its copies, and review the [secret guide](secrets.md) before
putting sensitive values into jobs. Queue retention and runnable-wait warning
settings are stored, but the daemon does not automatically purge history or
emit runnable-wait warnings.
