# Operating JobU

On Linux, use a dedicated non-root account for a managed deployment. The
[installation guide](installation.md) covers the package layout and systemd
setup; the [configuration reference](configuration.md) covers startup policy.
The [quick start](README.md#try-a-command-line-job) uses disposable private paths
for foreground operation as your ordinary user. macOS is a development host;
use the [foreground workflow below](#macos-foreground-operation).

## Startup, readiness and shutdown

On Linux, startup resolves arguments/configuration and validates the final
identity before constructing worker-capable objects. It prepares only permitted state/runtime
leaves, finalizes identity, acquires endpoint/database ownership, validates
format 5 and recovers interrupted work before admitting requests. A successful
post-recovery inventory is logged before runner construction and dispatch; it
reports loaded persisted state, independently of readiness. Telemetry
activates before dispatch; retention starts after listening. `jobud.ready`
means those startup steps completed. A bounded successful `jobuctl system info`
call proves the client can actually reach the endpoint.

```sh
jobuctl --timeout 1000 system info
jobuctl --socket /run/jobu/jobud.sock --timeout 1000 system info
```

Use the form matching the deployment. The client uses its compiled default
unless explicitly overridden; it neither reads daemon configuration nor searches
other installations. Systemd `Type=exec` or an active unit alone is not RPC
readiness. The installed `share/jobu/services/wait-ready.sh` supplies a bounded
readiness probe, documented in the systemd setup guide.

SIGTERM or SIGINT requests immediate shutdown: admission and maintenance stop,
then healthy shutdown settles open timing intervals at the captured stop time
after active callbacks/transactions unwind. It does not wait for every job to
finish naturally. Running work follows interruption/recovery policy on restart.
A fatal storage failure closes admission and performs no further best-effort
database writes. The service manager provides the final cleanup boundary for
children that exceed the stop timeout; deliberately escaped sessions are outside
the established cleanup evidence.

Stop a foreground daemon with Ctrl-C, or a managed one with:

```sh
sudo systemctl stop jobud.service
systemctl show jobud.service -p ActiveState -p SubState -p MainPID
```

Confirm it has stopped and no separately launched daemon owns the same data
before backup, replacement or restore. Do not use a missing socket pathname as
proof that no process owns the database. Restart deliberately after changing
configuration or binaries; installation does not activate or restart services.

## macOS foreground operation

First follow the [home-prefix installation](installation.md#macos-developer-installation).
Run as your ordinary user, with the default mode-0600 socket. Leave run-as and
socket-owner/group overrides unset and both unsafe root permissions false.
No service manager starts, restarts or stops this developer instance for you.

The Runtime install creates missing normal `etc`/`var` directories with mode
0755 and `var/lib`/`var/run` with mode 0700. A fresh installation at its configured
prefix needs no manual directory setup or active INI. Custom paths still need
explicit provisioning. Existing ownership, modes, configuration and data are
preserved; inspect unsuitable existing paths instead of recursively repairing
the tree. Darwin sets umask 0077 before
creating runtime resources, protecting newly created files; it does not repair
existing permissions or provide Linux's runtime-path admission checks.

An active config is optional. To create one, follow the repeat-safe initial-copy
command in `$HOME/jobu/etc/README.md`. It refuses an existing file or symlink,
creates the INI with owner-only permissions and scopes its umask to a subshell.
Edit the copy and validate it locally:

```sh
"$HOME/jobu/bin/jobud" --check-config --config "$HOME/jobu/etc/jobud.ini"
```

Review the sample before use. Its absolute paths were set during configuration;
the INI parser does not expand `$HOME` or `~`. Config reading checks the file and
protected parents on Darwin. A successful check or account-name lookup does not
establish privilege-transition or Linux runtime-path enforcement on macOS.

Start in one terminal, leaving it in the foreground:

```sh
"$HOME/jobu/bin/jobud"
```

It reads the compiled config path when present; a missing default config is
allowed. To deliberately ignore configuration, use `--no-config`. Wait for
`jobud.ready` on stderr, then confirm readiness from another terminal:

```sh
"$HOME/jobu/bin/jobuctl" --timeout 1000 system info
```

The two installed binaries share the compiled endpoint. If you override
`socket.path` or daemon `--socket`, give the client the same `--socket` path.
For disposable work, the [quick start](README.md#try-a-command-line-job) uses
a short private leaf beneath `/tmp`; substitute the installed executable paths.
Darwin's usual temporary-directory path can exceed the Unix socket length limit.
Never use a raw shared `/tmp` socket or delete an existing endpoint blindly.

Stop with Ctrl-C in the daemon terminal, or send SIGTERM to the known daemon
PID. Wait for process exit and ensure test-owned children have stopped before
restart, backup or fixture removal. Logs go to stderr; no managed log file or
rotation is installed. Normal shutdown/restart is supported within this workflow.
Darwin does not use Linux's separate endpoint guard or proven-stale socket
recovery. After a crash, preserve the old endpoint and establish that its owner
has stopped; use a new private socket path for a deliberate restart, with the
same explicit client override. A missing endpoint alone never proves that a
database has no owner.

macOS service deployment and packaging are excluded by user decision; not tested
or claimed. The Linux identity, group-access and endpoint guarantees below apply
to Linux. Retention, configuration semantics and the stopped-database backup
procedure also apply to the macOS foreground workflow.

## Identity and protected paths

This section describes Linux runtime enforcement and administrative setup.

The state directory must belong to the final daemon user and have mode 0700.
The database, WAL, SHM and lock artifacts are owner-only regular files. The
private runtime directory is mode 0700; a group-access runtime directory is
mode 0750 with the configured group and a mode-0660 socket. Members of that
group have full authority, including secret management and CLI execution.
There is no per-user authorization or read-only monitoring role.

Configuration must be protected against replacement through its entire parent
chain. Root invocations require root-owned configuration without group/other
write permission. Non-root invocation also accepts configuration owned by the
final user. Root-owned, group-readable mode 0640 is suitable for a managed
service's active file; private user configurations can use mode 0600.

Provision missing parents explicitly. JobU creates only approved missing leaf
directories; it validates existing trees instead of recursively changing their
ownership or permissions. Sensitive leaf symlinks, wrong file types and unsafe
owners/modes cause rejection. A private mode-0700 directory beneath sticky
`/tmp` is suitable for a disposable demo; raw database/socket paths in shared
`/tmp` are not. Processes sharing the daemon UID share its authority.

Root normally supplies a non-root `--run-as-user`; the daemon initializes groups,
drops GID then UID permanently and verifies Linux identity/capabilities before
resources or execution. `--allow-root-daemon` and `--allow-root-cli` are separate
unsafe overrides, disabled by default. A managed service already running as
its dedicated account needs neither. Linux CLI children retain privilege-gain
prevention; these Linux checks are not claims about macOS kernel facilities.

## Endpoint ownership and crash restart

On Linux, the daemon holds a database lock and a separate `${socket.path}.lock`
for the listener lifetime. Different databases cannot compete for the same endpoint.
Lock files can remain after exit; their presence alone does not mean a daemon
is running. Never unlink a held lock to obtain another inode with the same name.

After acquiring ownership, restart removes an old socket only if its protected
parent, owner and type are correct, a bounded probe definitively reports
connection refused, and the inode is unchanged. A live or ambiguous endpoint,
symlink or non-socket collision is preserved and startup fails. Investigate the
owner and configured paths; do not add unconditional pre-start unlink commands.
Clean shutdown removes only its own socket inode.

## Retention and replay lifetimes

Retention runs automatically. Its default is 30 days; a queue's null policy
inherits the daemon default, zero keeps history indefinitely, and a positive
value overrides the default. The run's persisted queue governs its history,
even when the definition has moved or that queue is soft-deleted.

Only terminal Succeeded, Failed, Interrupted or Cancelled runs whose completion
time is **strictly earlier** than the cutoff are deleted. Equality is retained.
Scheduled, Running and RetryWait runs keep their attempts/output. Cancellation
ages from completion, not planned time. UTC rollback postpones expiry; a forward
correction can make history eligible. Policy edits affect subsequent cleanup.

Deletion is irreversible and removes associated attempts, output and timing
atomically. Statistics then reflect the remaining rows. SQLite may reuse freed
pages without immediately shrinking the database file; JobU schedules no VACUUM.
An ordinary terminal one-time definition remains available for inspection.
Cleanup does not reopen it or permit a new Run Now request.

| Idempotency record | Retained lifetime |
| --- | --- |
| Queue creation | Live queue, or deleted ownership/history that still needs it. Retired with guarded physical queue removal. |
| Recurring job creation | Live definition, including suspension, or retained runs after deletion. Retired with eligible deleted definition removal. |
| One-time job creation | Any nonterminal or retained terminal run. Can retire after the last run expires while the terminal definition remains. |
| Run Now | Retained manual run; retired in its purge transaction. |

Keys do not expire merely because 30 days elapsed since creation. A live
recurring job or future one-time run still protects its creation replay.
Moving a job retains its original creation scope; that replay can keep the
original deleted queue alive. Deleted owners are removed only when their
history, definitions and replay references permit it.

After retirement, the same key is a new request under current eligibility.
Persist the original key/request to reconcile an uncertain response, and inspect
the resource if the replay lifetime may have ended. See the
[client guide](cpp-client.md#timeouts-cancellation-and-mutations) and
[job protocol](protocol/methods/job.md) for the exact request contract.

Each retention transaction limits parent records, not cascaded rows or output
bytes. It yields between batches and visits queues fairly. Large cascades,
reference validation and other synchronous owner-thread work can still delay
RPC/scheduling; there is no universal latency guarantee.

## Offline backup

Backups contain plaintext secrets, job payloads and captured output. Protect
the backup and its parents, retain only deliberately, and use your organization's
protected storage/encryption arrangements. JobU has no backup RPC or automatic
backup/export service. The `sqlite3` command-line utility is an additional
operator tool, not part of the JobU Runtime archive.

Stop the daemon and prevent another instance/service restart for the entire
backup. Run the following as the database owner, substituting the real database
and an already provisioned private backup parent. The parent must survive reboot
for a durable backup. Do not use these placeholder paths literally.

```sh
umask 077
database=/absolute/private-state/jobu.sqlite3
backup_parent=/absolute/private-backups
backup_dir=$(mktemp -d "$backup_parent/jobu-backup.XXXXXX")
(
    cd "$backup_dir" &&
    sqlite3 -readonly "$database" '.backup jobu.sqlite3' &&
    chmod 0600 jobu.sqlite3 &&
    sqlite3 -readonly jobu.sqlite3 'PRAGMA integrity_check;'
)
```

Require successful commands and an integrity result of `ok`. SQLite's backup
operation produces a consistent standalone destination and accounts for any
committed WAL content still associated with the source. Preserve the source's
main/WAL/SHM files together until SQLite has finished; do not manually discard
sidecars. Never copy only a live main database file or use an immutable connection
to pretend live WAL does not exist.

Record the backup path, timestamp, JobU project version and source package
metadata separately, and securely preserve the active configuration needed to
interpret that deployment. A config file can contain proxy credentials. Backups
do not include executable programs, external files or remote HTTP side effects.
An integrity check is not a JobU schema/domain check and does not authorize
editing format markers.

## Restore into a fresh state directory

Keep the daemon stopped throughout preparation. Preserve the current state
directory and its sidecars unchanged. As the intended database owner, select
the backup and an existing protected parent for a **new** state directory:

```sh
umask 077
backup_dir=/absolute/private-backups/jobu-backup.SELECTED
state_parent=/absolute/protected-parent
restore_dir=$(mktemp -d "$state_parent/jobu-restore.XXXXXX")
install -m 0600 "$backup_dir/jobu.sqlite3" "$restore_dir/jobu.sqlite3"
sqlite3 -readonly "$restore_dir/jobu.sqlite3" 'PRAGMA integrity_check;'
```

Require `ok`, verify final-user ownership and mode 0700/0600, then deliberately
change `database.path` to the restored file (or supply `--database` for foreground
use). Keep the existing protected runtime/socket setup. Run `--check-config`
under the intended identity, then start and inspect readiness and recovery logs.
The daemon validates format 5 and domain relationships before RPC admission;
check mode alone never opens or validates the database.

Probe `system info`, inspect known queues/jobs/runs and compare them with the
backup checkpoint. Recovery can interrupt/retry work that was Running in the
snapshot, and schedules/retention resume under current policy. Restoring an old
snapshot can repeat externally visible work or lose later local records; plan
the restart accordingly. Preserve the original state until the restored service
is accepted. If restore is rejected, stop and investigate before returning the
configuration to the preserved original path.

## Format rejection, recovery and troubleshooting

Current format is 5. Formats 1–4 and newer formats are rejected without migration
or marker rewriting. Keep old data; only deliberately disposable development
databases should be replaced with a fresh path. There is no automatic repair,
migration, secret export or backup conversion.

API 1.6 queue revisions require format 5. An existing format-4 installation
must deliberately select a fresh database before using this daemon; startup
never deletes, resets, or converts the old database. Preserve old data and
backups. Any separately maintained manual format-4-to-5 SQL utility is an
explicit offline operator procedure, outside daemon migration infrastructure.
It is not run by startup or by these verification commands.

Queue create replay retains the original revision-1 result, not the current
configuration. Read `queue.get` before editing and retain both UUID and revision.
Configuration updates and actual lifecycle transitions invalidate earlier
tokens; reads, counters, and ordinary job activity do not. See the
[protected update contract](protocol/methods/queue.md#update-queueupdate).

[Interactive queue editing](jobuctl.md#interactive-queue-updates) captures that
UUID/revision before opening the editor and submits once afterward. On conflict,
the retained minimal request keeps its old revision; reconcile against the same
UUID before preparing another update. After an unknown outcome, inspect current
state before any retry because the update may already have committed.

Recovery runs before readiness. Interrupted attempts follow the queue's
`fail_interrupted` or `retry_interrupted` policy. Abandoned open timing intervals
retain checkpointed wait and become partial; downtime is not charged. See
[observability](observability.md) for quality and warning limits.

| Symptom/code | What to inspect and required condition |
| --- | --- |
| `jobud.config.invalid`, `unknown_key`, `duplicate_key` | Check the selected file against the reference for spelling, grammar, ranges and cross-field constraints. Current command-line diagnostics report a fixed message/code, not rejected values or a full configuration dump. |
| `jobud.config.read_failed` | Check the selected file exists, is readable under the invocation identity, and has protected owners/parents. Explicit config is required to exist. |
| Linux `jobud.privilege.root_disallowed`, `drop_failed` | Use the dedicated account or a valid non-root run-as target; privilege verification must finish before resources are admitted. |
| Linux `jobud.path.unsafe`, `jobud.path.inspect_failed` | Inspect `resource`, the exact failing `path`, fixed `reason`, observed/required metadata and `action`. A missing custom-path ancestor needs provisioning; a wrong leaf mode or owner needs the specific correction described. Preserve collisions; do not recursively chmod/chown a supplied tree. |
| Linux `jobud.socket.in_use`, `jobud.socket.inspect_failed` | Find the endpoint owner or ambiguous collision. Preserve other resources and held lock files. |
| Schema/recovery failure before `jobud.ready` | Preserve database and sidecars; check package/format compatibility and sanitized failure code. Do not change the marker or attempt repair SQL. |
| Client cannot connect | Compare its attempted endpoint with daemon config, readiness and runtime traversal/socket permissions. Override with `--socket` when necessary. |
| Runtime `jobud.failed` | Inspect `subsystem` and stable `code`, storage capacity/availability and preceding events. Admission closes on fatal state; do not assume partial cleanup repaired it. |
| No successful output | Default `output.capture="on_error"` retains failed-attempt output. Select `"always"` deliberately if successful output is required. |
| No delay warning | Check strict threshold, zero-off, current eligibility, log level and previous durable claim; delivery is at most once. |

Usage/configuration failures normally exit 2; startup-resource/runtime failures
exit 1; successful local actions and healthy stop exit 0. Early configuration
diagnostics precede the configured logger and need not be JSON. `--check-config`
success, unit syntax validation and a passing unprivileged fixture are different
evidence from a verified managed deployment.

Path failures now explain the inspected resource, for example:
`State directory '/home/user/jobu/var/lib' has permissions 0755; required permissions are 0700.`
Missing-parent errors identify the missing component and the resource depending
on it. Modes are octal strings; owner/group IDs are numeric. Unknown facts are
omitted. Read the ownership advice before changing permissions: chmod alone
does not repair a wrong owner. Advice never requires recursive permission changes,
deleting collisions or unlinking an endpoint. Configuration syntax failures still
omit rejected values, and pre-logger path diagnostics escape control characters.

The existing `run.list` membership/filtering and pending-run update/move behavior
remain unchanged; issue #230 is deliberately deferred. Native macOS developer
acceptance and final Linux Phase 9 closure have separate gates. Documentation
and earlier focused checks do not establish a new full acceptance run.
