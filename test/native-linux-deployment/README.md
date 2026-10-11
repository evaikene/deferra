# Native Linux deployment gate

This is the opt-in Stage 9.26 gate for an approved disposable systemd VM. It is
not registered with ordinary CTest. `verify.py` installs real Runtime payloads,
creates two temporary probe accounts, starts/stops a real `jobud.service`, and
sends SIGKILL to its main process. Establish a rollback baseline first.

The VM needs Python 3.12 or newer, the project's existing build/runtime
dependencies, CMake, Ninja, compiler/binutils, strace, systemd, account tools and
root privileges. Missing prerequisites leave the gate incomplete. Neither script
installs dependencies. The producer uses two build jobs to bound memory usage.

Export an exact clean revision with read-only Git inspection in the colocated
jj repository. Record the archive checksum and verify the selected jj revision
before transfer. Do not export a modified checkout and call it a clean landed
revision. For example, with the merged Stage 9.25 revision:

```sh
git archive --format=tar -o /tmp/jobu926-source.tar \
    e313f7f2b8e6215c98e22e277b6b9ba9c123b890
sha256sum /tmp/jobu926-source.tar
```

Transfer the trusted source archive and these harness files into an isolated VM
input directory. `produce.sh` treats its input as that trusted source export;
it is not a general-purpose untrusted-archive importer. On the target VM:

```sh
bash produce.sh /absolute/input/jobu926-source.tar \
    e313f7f2b8e6215c98e22e277b6b9ba9c123b890 /var/tmp/jobu926-producer
```

The producer keeps source unchanged, uses fresh Release caches with tests/examples
off and path remapping on, and produces system (`/usr`) and custom
(`/opt/jobu926`) component archives with both checksum/inventory sidecars.
It separately builds the existing native identity helper from the same source.
That support executable is outside the Runtime packages and does not stand in
for observations of the installed daemon.

Review the generated metadata, logs and available disk before invoking the gate:

```sh
sudo python3 verify.py --disposable-vm \
    --system-runtime /var/tmp/jobu926-producer/artifacts/system/JobU-0.1.0-Linux-ubuntu26.04-x86_64-runtime.tar.gz \
    --custom-runtime /var/tmp/jobu926-producer/artifacts/custom/JobU-0.1.0-Linux-ubuntu26.04-x86_64-runtime.tar.gz \
    --identity-helper /var/tmp/jobu926-producer/artifacts/support/jobud-identity-test-helper \
    --source-revision e313f7f2b8e6215c98e22e277b6b9ba9c123b890 \
    --evidence /var/tmp/jobu926-evidence
```

Run all cases against installed package binaries. Checks cover private-prefix
startup, real/saved identities and failed root regain, retained capabilities,
independent unsafe root switches, protected paths, endpoint collisions and stale
restart, explicit managed configuration, owner/group/outsider access, endpoint
competition, SIGTERM/SIGKILL cleanup, both recovery policies, persisted history
and replay, and crash-visible partial timing. Native strace observations check
identity-before-worker/database/network ordering and restrictive mode-before-listen
ordering. Before the drop, the trace checks permit the absent local NSS-cache
probe and the observed systemd NSS membership lookup. The latter must use the
exact DynamicUser endpoint and read-only GetMemberships request for the service
account, receive NoRecordFound, and close the same PID/descriptor before the
permanent UID drop. Nonblocking EAGAIN receives may retry while awaiting that
reply; they cannot replace the successful response or close. Other early
worker/network operations remain failures. The
group variant grants full daemon authority, demonstrated by an administrator's
successful queue mutation.

Run the unprivileged harness regression checks before a native invocation:

```sh
python3 -B test/native-linux-deployment/harness_test.py
```

The gate refuses existing deployment paths, units and probe identities. It
preserves the dedicated `jobu` account and creates no login home. Package
installation is explicit file deployment; it has no account/config/state/service
side effects. A separate test operator step provisions configuration and activates
the service. Boot enablement is not part of the gate.

`result.json`, command logs, events and native traces remain in the new root-only
evidence directory. Copy them off the VM before removing producer scratch.
Success requires all cases and teardown. Normal failure stops immediately,
retains the failed observation and tears down registered resources; diagnose it
and obtain approval for a narrow correction before editing/rerunning.

Teardown stops the unit, removes only gate-created accounts/files/directories,
reloads systemd, and leaves the original `jobu` identity intact. If userdel has
already removed the probe's private group, cleanup verifies its absence; other
lookup/removal errors remain failures. Fixture roots are
checked by device/inode before removal. SIGKILL of the outer harness cannot run
its cleanup; use retained evidence and the rollback baseline for that case. Do
not infer escaped-session cleanup, scale acceptance, native macOS coverage or a
final full-suite pass from this gate.
