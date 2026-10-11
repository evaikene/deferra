"""Package-based foreground identity, path and endpoint checks."""

import json
import os
from pathlib import Path
import re
import signal
import socket as sockets
import sys

from common import Foreground, digest, metadata, require


def failed_nss_lookup(lines, index):
    # Native glibc tries its absent local NSS cache before reading account
    # files. This readonly identity lookup is explicitly allowed before drop;
    # recognize only the observed ENOENT connection and immediate close.
    opened = re.search(r"socket\(AF_UNIX,.*\)\s+= (\d+)$", lines[index])
    if opened is None or index + 2 >= len(lines):
        return False
    descriptor = opened.group(1)
    connected = lines[index + 1]
    closed = lines[index + 2]
    return (f"connect({descriptor}," in connected
            and 'sun_path="/var/run/nscd/socket"' in connected
            and "= -1 ENOENT" in connected
            and f"close({descriptor})" in closed and closed.endswith("= 0"))


def nss_membership_lookup(lines, index, account, before):
    # systemd NSS resolves supplementary groups through a local userdb service.
    # Allow only the observed read-only query and response on one PID/descriptor,
    # completed before the permanent UID drop; a socket path alone proves little.
    opened = re.fullmatch(r"(\d+)\s+socket\(AF_UNIX, SOCK_STREAM\|SOCK_CLOEXEC\|SOCK_NONBLOCK, 0\)\s+= (\d+)",
                          lines[index])
    if opened is None or index + 1 >= before:
        return False
    pid, descriptor = opened.groups()
    connected = rf'{pid}\s+connect\({descriptor}, {{sa_family=AF_UNIX, sun_path="/run/systemd/userdb/io.systemd.DynamicUser"}}, \d+\)\s+= 0'
    if re.fullmatch(connected, lines[index + 1]) is None:
        return False

    request = json.dumps({"method": "io.systemd.UserDatabase.GetMemberships",
                          "parameters": {"userName": account, "service": "io.systemd.DynamicUser"},
                          "more": True}, separators=(",", ":"))
    response = json.dumps({"error": "io.systemd.UserDatabase.NoRecordFound"}, separators=(",", ":"))
    # strace renders the terminating wire NUL as \0 rather than JSON's \u0000.
    request_text = re.escape(json.dumps(request)[:-1] + r'\0"')
    response_text = re.escape(json.dumps(response)[:-1] + r'\0"')
    request_size = len(request.encode()) + 1
    response_size = len(response.encode()) + 1
    expected = [
        rf"{pid}\s+sendto\({descriptor}, {request_text}, {request_size}, MSG_DONTWAIT\|MSG_NOSIGNAL, NULL, 0\)\s+= {request_size}",
        rf"{pid}\s+recvfrom\({descriptor}, {response_text}, \d+, MSG_DONTWAIT, NULL, NULL\)\s+= {response_size}",
        rf"{pid}\s+close\({descriptor}\)\s+= 0",
    ]

    phase = 0
    descriptor_call = rf"{pid}\s+\w+\({descriptor}(?:,|\))"
    would_block = (rf"{pid}\s+recvfrom\({descriptor}, 0x[0-9a-f]+, \d+, MSG_DONTWAIT, NULL, NULL\)"
                   rf"\s+= -1 EAGAIN \(Resource temporarily unavailable\)")
    for line in lines[index + 2:before]:
        if re.match(rf"{pid}\s+setresuid\(", line):
            return False
        if re.match(descriptor_call, line) is None:
            continue
        # A nonblocking retry advances no protocol state. The successful reply
        # and close must still appear within the pre-drop trace boundary.
        if phase == 1 and re.fullmatch(would_block, line):
            continue
        if re.fullmatch(expected[phase], line) is None:
            return False
        phase += 1
        if phase == len(expected):
            return True
    return False


def allowed_nss_lookup(lines, index, account, before=None):
    before = len(lines) if before is None else before
    if index + 2 >= before:
        return False
    return failed_nss_lookup(lines, index) or nss_membership_lookup(lines, index, account, before)


def check_startup_trace(gate, trace, socket, mode):
    lines = trace.read_text().splitlines()
    dropped = next((index for index, line in enumerate(lines)
                    if "setresuid(" in line and str(gate.account.pw_uid) in line and line.endswith("= 0")), None)
    require(dropped is not None, "Native trace has no successful permanent UID drop")
    verified = next((index for index, line in enumerate(lines)
                     if index > dropped and "capget(" in line and "effective=0" in line and "permitted=0" in line), None)
    require(verified is not None, "Native trace has no successful capability verification after drop")
    for index, line in enumerate(lines):
        if any(call in line for call in ("clone(", "clone3(", "socket(", "bind(", "listen(")):
            require(index > verified or allowed_nss_lookup(lines, index, gate.account.pw_name, dropped),
                    f"Worker/network operation precedes identity verification: {line}")
        if ".sqlite" in line and any(call in line for call in ("open(", "openat(")):
            require(index > verified, "Database open precedes identity verification")

    bound = next((index for index, line in enumerate(lines) if "bind(" in line and str(socket) in line), None)
    listening = next((index for index, line in enumerate(lines) if "listen(" in line), None)
    restricted = [index for index, line in enumerate(lines)
                  if str(socket) in line and "chmod" in line and f"{mode:o}" in line and line.endswith("= 0")]
    require(bound is not None and listening is not None, "Native trace lacks bind/listen")
    require(any(bound < index < listening for index in restricted), "Socket mode was not applied before listen")
    require(any("umask(077)" in line for line in lines[:bound]), "Restrictive umask missing before bind")
    if mode == 0o660:
        assigned = [index for index, line in enumerate(lines)
                    if str(socket) in line and "chown" in line and str(gate.account.pw_gid) in line and line.endswith("= 0")]
        require(any(bound < index < listening for index in assigned), "Administrative socket group was not assigned before listen")
    gate.event("startup.order", trace=str(trace), uid_drop=dropped, capability_verification=verified,
               bind=bound, listen=listening, mode=oct(mode))


def reject(gate, daemon, directory, options, code, *, user=None, trace=False):
    database = directory / "rejected.sqlite"
    socket = directory / "rejected.sock"
    arguments = [daemon, "--no-config", "--database", database, "--socket", socket, *options]
    trace_path = gate.evidence / f"rejected-{len(gate.results):03d}.trace"
    if trace:
        arguments = ["strace", "-f", "-o", trace_path,
                     "-e", "trace=%process,%file,%network,%creds,close", *arguments]
    result = gate.run(arguments, user=user, expected=1)
    require(code in result.stderr, f"Expected rejection code {code}")
    require("jobud.ready" not in result.stderr, "Rejected daemon reported ready")
    require(not database.exists() and not socket.exists(), "Rejected startup created resources")
    require(not Path(str(database) + ".lock").exists(), "Rejected startup created a database lock")
    if trace:
        lines = trace_path.read_text().splitlines()
        for index, line in enumerate(lines):
            if any(call in line for call in ("clone(", "clone3(", "socket(", "bind(", "listen(")):
                require(allowed_nss_lookup(lines, index, gate.account.pw_name),
                        f"Rejected identity started worker/network operations: {line}")
    gate.event("rejected", code=code, directory=str(directory), trace=str(trace_path) if trace else None)


def run_foreground(gate, prefix, helper):
    daemon = prefix / "bin/jobud"
    client = prefix / "bin/jobuctl"
    state = prefix / "var/lib"
    runtime = prefix / "var/run"
    gate.directory(prefix / "var", mode=0o755)
    gate.directory(state, owner=gate.account)
    gate.directory(runtime, owner=gate.account)
    database = state / "jobu.sqlite3"
    socket = runtime / "jobud.sock"
    work = gate.fixture / "work"
    gate.directory(work, owner=gate.account)

    with gate.case("private-prefix packaged foreground smoke"):
        config = prefix / "etc/jobud.ini"
        gate.directory(config.parent, mode=0o755)
        gate.config(config, (prefix / "share/jobu/jobud.ini.example").read_text())
        before = set(state.iterdir()) | set(runtime.iterdir())
        gate.run([daemon, "--check-config", "--config", config], user="jobu")
        require(before == set(state.iterdir()) | set(runtime.iterdir()), "Check mode created resources")
        with Foreground(gate, daemon, database, socket, options=["--run-as-user", "jobu"],
                        config=config, use_defaults=True) as process:
            gate.check_identity(process.ready(client))
            gate.check_paths(database, socket)
            gate.run([client, "--json", "--timeout", "1000", "system", "info"], user="jobu")
            gate.run(["sh", prefix / "share/jobu/services/wait-ready.sh", client, socket, "5", "1000"], user="jobu")
            queue = gate.create_queue(client, socket, "private-prefix")
            job, replay_request = gate.create_job(client, socket, queue, gate.worker_payload(work, "complete"), "private-complete")
            completed = gate.run_for(client, socket, job, "succeeded")
            report = gate.worker_report(work, "complete")
            require(set(map(int, report["Uid"].split())) == {gate.account.pw_uid}, "CLI worker UID mismatch")
            require(report["NoNewPrivs"] == "1", "CLI worker can gain privileges")
            require(process.stop() == 0, "Foreground SIGTERM failed")
        require(not socket.exists(), "Clean foreground stop left a socket")

    with gate.case("permanent root drop and pre-listen permission ordering"):
        trace = gate.evidence / "root-drop.trace"
        options = ["--run-as-user", "jobu", "--allow-root-daemon", "--allow-root-cli"]
        with Foreground(gate, daemon, database, socket, options=options, user=None, trace=trace) as process:
            gate.check_identity(process.ready(client))
            gate.check_paths(database, socket)
            require(gate.rpc(client, socket, "job.create", replay_request) == job, "Restart changed creation replay")
            require(gate.rpc(client, socket, "run.get", {"run_id": completed["id"]})["state"] == "succeeded",
                    "Completed history did not survive restart")
            require(process.stop() == 0, "Root-drop foreground stop failed")
        text = process.log_path.read_text()
        require("jobud.unsafe.root_daemon" not in text and "jobud.unsafe.root_cli" not in text,
                "Requested non-root target did not take precedence")
        check_startup_trace(gate, trace, socket, 0o600)
        gate.run([helper, "target", "jobu"])
        gate.run([helper, "target", "jobu", "jobu"])
        retained = gate.run([helper, "retain-caps", "jobu"])
        require("jobud.privilege.drop_failed" in retained.stderr, "Retained-capability helper was not refused")

    with gate.case("administrative socket group is applied before listening"):
        trace = gate.evidence / "group-listen.trace"
        group_config = prefix / "etc/group.ini"
        gate.config(group_config, "daemon.run_as_user = jobu\nsocket.mode = 0660\nsocket.group = jobu\n")
        runtime.chmod(0o750)
        with Foreground(gate, daemon, database, socket, config=group_config, user=None, trace=trace) as process:
            gate.check_identity(process.ready(client))
            gate.check_paths(database, socket, mode=0o660)
            gate.rpc(client, socket, "system.info", user="jobu926_admin")
            require(process.stop() == 0, "Group-mode foreground stop failed")
        check_startup_trace(gate, trace, socket, 0o660)
        runtime.chmod(0o700)

    refusals = gate.fixture / "refusals"
    gate.directory(refusals, mode=0o755)
    for allowed_cli in (False, True):
        with gate.case(f"root startup rejected with cli override={allowed_cli}"):
            directory = refusals / f"root-{allowed_cli}"
            gate.directory(directory)
            reject(gate, daemon, directory, ["--allow-root-cli"] if allowed_cli else [],
                   "jobud.privilege.root_disallowed", trace=True)

    for options in (["--run-as-user", "root", "--allow-root-daemon"],
                    ["--run-as-user", "jobu", "--run-as-group", "root"]):
        with gate.case(f"non-root identity mismatch {' '.join(options)}"):
            directory = refusals / f"mismatch-{len(gate.results)}"
            gate.directory(directory, owner=gate.account)
            reject(gate, daemon, directory, options, "jobud.privilege.identity_mismatch", user="jobu")

    with gate.case("mixed real/effective identity rejected"):
        directory = refusals / "mixed"
        gate.directory(directory, mode=0o755)
        command = [sys.executable, "-c",
                   "import os,sys; os.setresuid(0,int(sys.argv[1]),0); os.execv(sys.argv[2],sys.argv[2:])",
                   str(gate.account.pw_uid), daemon, "--no-config", "--database", directory / "mixed.sqlite",
                   "--socket", directory / "mixed.sock", "--allow-root-daemon"]
        result = gate.run(command, expected=1)
        require("jobud.privilege.identity_mismatch" in result.stderr, "Mixed identity was not refused")
        require(not list(directory.iterdir()), "Mixed-identity rejection created resources")

    for collision in ("file", "symlink", "listener"):
        with gate.case(f"packaged endpoint preserves {collision} collision"):
            path = runtime / f"{collision}.sock"
            database_path = state / f"{collision}.sqlite"
            listener = None
            if collision == "file":
                path.write_text("preserve-collision")
                path.chmod(0o600)
            elif collision == "symlink":
                path.symlink_to("missing-collision-target")
            else:
                listener = sockets.socket(sockets.AF_UNIX, sockets.SOCK_STREAM)
                listener.bind(str(path))
                path.chmod(0o600)
                listener.listen(4)
            os.lchown(path, gate.account.pw_uid, gate.account.pw_gid)
            before = metadata(path)
            try:
                result = gate.run([daemon, "--no-config", "--database", database_path, "--socket", path],
                                  user="jobu", expected=1)
                expected = "jobud.socket.in_use" if listener else "jobud.path.unsafe"
                require(expected in result.stderr, f"Wrong collision rejection: {collision}")
                require(metadata(path) == before, "Collision resource changed")
                if collision == "file":
                    require(path.read_text() == "preserve-collision", "Collision file contents changed")
                if collision == "symlink":
                    require(os.readlink(path) == "missing-collision-target", "Collision symlink changed")
                if listener:
                    listener.settimeout(2)
                    connection, _ = listener.accept()
                    connection.close()
                    probe = sockets.socket(sockets.AF_UNIX, sockets.SOCK_STREAM)
                    probe.settimeout(2)
                    probe.connect(str(path))
                    connection, _ = listener.accept()
                    connection.close()
                    probe.close()
                gate.event("collision.preserved", kind=collision, metadata=before)
            finally:
                if listener:
                    listener.close()

    with gate.case("SIGKILL preserves stale socket and restart keeps endpoint lock inode"):
        with Foreground(gate, daemon, database, socket) as first:
            first.ready(client)
            lock_before = metadata(Path(str(socket) + ".lock"))
            old_pid = first.pid
            first.stop(signal.SIGKILL)
        require(socket.is_socket(), "Crash did not leave a stale foreground socket")
        gate.event("stale.socket", metadata=metadata(socket), old_pid=old_pid)
        with Foreground(gate, daemon, database, socket) as next_process:
            next_process.ready(client)
            require(next_process.pid != old_pid, "Foreground crash restart reused daemon PID")
            require(metadata(Path(str(socket) + ".lock")) == lock_before, "Restart replaced lock inode")
            require(next_process.stop() == 0, "Restarted foreground daemon did not stop")

    # Each failure uses a fresh leaf; retained sentinels show rejection never
    # becomes implicit permission repair or database replacement.
    for unsafe in ("state-mode", "database-mode", "config-mode", "config-symlink"):
        with gate.case(f"protected startup rejects {unsafe}"):
            directory = refusals / unsafe
            gate.directory(directory, owner=gate.account)
            target_database = directory / "rejected.sqlite"
            options = []
            sentinel = None
            if unsafe == "state-mode":
                directory.chmod(0o755)
            elif unsafe == "database-mode":
                target_database.write_text("preserve-database")
                os.chown(target_database, gate.account.pw_uid, gate.account.pw_gid)
                target_database.chmod(0o644)
                sentinel = digest(target_database)
            else:
                config = directory / "unsafe.ini"
                gate.config(config, "logging.format = json\n", mode=0o666 if unsafe == "config-mode" else 0o640)
                if unsafe == "config-symlink":
                    link = directory / "linked.ini"
                    link.symlink_to(config.name)
                    config = link
                options = ["--config", config]
            arguments = [daemon, "--database", target_database, "--socket", directory / "rejected.sock"]
            arguments += options or ["--no-config"]
            result = gate.run(arguments, user="jobu", expected=1)
            require("jobud.path.unsafe" in result.stderr or "jobud.config.read_failed" in result.stderr,
                    "Unsafe startup did not report a path/config refusal")
            require(not (directory / "rejected.sock").exists(), "Unsafe startup opened a socket")
            require(not Path(str(target_database) + ".lock").exists(), "Unsafe startup opened a database lock")
            if sentinel:
                require(digest(target_database) == sentinel and metadata(target_database)["mode"] == "0o644",
                        "Unsafe database was repaired or changed")
