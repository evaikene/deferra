"""Real system-profile activation, authorization, cgroup stop and recovery."""

from contextlib import closing
import json
import os
from pathlib import Path
import re
import signal
import sqlite3
import uuid

from common import Foreground, active_process, digest, metadata, require


def configuration(text, changes):
    for key, value in changes.items():
        pattern = rf"^{re.escape(key)}\s*=.*$"
        replacement = f"{key} = {value}"
        if re.search(pattern, text, flags=re.MULTILINE):
            text = re.sub(pattern, replacement, text, flags=re.MULTILINE)
        else:
            text += replacement + "\n"
    return text


def unit_values(gate):
    result = gate.run(["systemctl", "show", "jobud.service", "-p", "MainPID", "-p", "NRestarts",
                       "-p", "ActiveState", "-p", "SubState", "-p", "ExecMainStatus", "-p", "ControlGroup"])
    return dict(line.split("=", 1) for line in result.stdout.splitlines())


def register_manager_roots(gate):
    # Preflight proved these names absent. The manager can recreate the runtime
    # leaf between incarnations, so register each actual new inode separately.
    for path in (Path("/var/lib/jobu"), Path("/run/jobu")):
        if path.exists():
            value = path.lstat()
            record = (path, value.st_dev, value.st_ino)
            if record not in gate.created_roots:
                gate.created_roots.append(record)


def start_service(gate, client, socket):
    gate.run(["systemctl", "start", "jobud.service"])
    register_manager_roots(gate)
    gate.ready(client, socket)
    register_manager_roots(gate)
    values = unit_values(gate)
    pid = int(values["MainPID"])
    require(pid > 0, "Ready service has no MainPID")
    gate.check_identity(pid, no_new_privileges=True)
    gate.event("service.ready", **values)
    return values


def stop_service(gate):
    gate.run(["systemctl", "stop", "jobud.service"], timeout=25)
    values = unit_values(gate)
    require(values["ActiveState"] == "inactive" and int(values["MainPID"]) == 0, "Service did not stop")
    require(not Path("/run/jobu/jobud.sock").exists(), "Stopped service left a socket")
    gate.event("service.stopped", **values)


def read_storage(gate, database, sql, parameters=()):
    # Inspection remains read-only, including when the daemon owns the WAL.
    with closing(sqlite3.connect(f"file:{database}?mode=ro", uri=True)) as connection:
        values = connection.execute(sql, parameters).fetchall()
    gate.event("storage.read", query=sql, parameters=[str(item) for item in parameters], rows=values)
    return values


def check_connection_denied(gate, client, socket, user):
    # Filesystem rejection is a local connection failure, not a represented
    # daemon operation error. Check its envelope so a deadline cannot pass here.
    result = gate.rpc(client, socket, "system.info", user=user, expected=3)
    require(not result.stdout, "Denied connection produced a successful result")
    error = json.loads(result.stderr)["error"]
    require(error["kind"] == "local" and error["code"] == "jobuctl.connection_failed"
            and error["category"] == "io" and error["rpc_code"] is None
            and error["outcome_unknown"] is False, "Unexpected socket-access rejection error")
    gate.event("access.denied", user=user, exit=result.returncode, error=error)


def work_pids(gate, work, name):
    parent = gate.worker_report(work, name)
    descendant = gate.worker_report(work, name + "-descendant")
    for value in (parent, descendant):
        require(set(map(int, value["Uid"].split())) == {gate.account.pw_uid}, "Managed CLI UID mismatch")
        require(value["NoNewPrivs"] == "1", "Managed CLI privilege-gain prevention missing")
    require(parent["group"] == descendant["group"], "Fixture descendant escaped its ordinary process group")
    return [parent["pid"], descendant["pid"]]


def check_children_gone(gate, pids, control_group):
    gate.wait(lambda: all(not active_process(pid) for pid in pids), "child work cleanup")
    group_file = Path("/sys/fs/cgroup") / control_group.lstrip("/") / "cgroup.procs"
    remaining = [] if not group_file.exists() else list(map(int, group_file.read_text().split()))
    require(not set(pids) & set(remaining), "Old worker remains in the service cgroup")
    gate.event("children.cleaned", pids=pids, cgroup=control_group, remaining=remaining,
               proc_entries=[pid for pid in pids if Path(f"/proc/{pid}").exists()])
    # Direct children must actually be reaped, beyond merely becoming zombies.
    gate.wait(lambda: not Path(f"/proc/{pids[0]}").exists(), "direct child reaping")


def root_cli_guards(gate, daemon, client, http):
    for allow_cli in (False, True):
        with gate.case(f"unsafe root daemon independently guards CLI allow={allow_cli}"):
            directory = gate.fixture / f"unsafe-root-{allow_cli}"
            gate.directory(directory)
            database = directory / "root.sqlite"
            socket = directory / "root.sock"
            options = ["--allow-root-daemon"] + (["--allow-root-cli"] if allow_cli else [])
            with Foreground(gate, daemon, database, socket, options=options, user=None) as process:
                process.ready(client, user="root")
                gate.check_identity(process.pid, root=True)
                queue = gate.rpc(client, socket, "queue.create", {"name": "root-guards", "concurrency_limit": 2}, user="root")
                name = f"root-cli-{allow_cli}"
                cli = gate.rpc(client, socket, "job.create", {"queue_id": queue["id"], "schedule": {"kind": "once", "at": "now"},
                               "payload": gate.worker_payload(directory, name)}, user="root")
                http_name = f"root-http-{allow_cli}"
                job = gate.rpc(client, socket, "job.create", {"queue_id": queue["id"], "type": "http",
                               "schedule": {"kind": "once", "at": "now"}, "payload": http.payload(http_name)}, user="root")
                http.wait(http_name)

                def http_completed():
                    items = gate.rpc(client, socket, "run.list", {"job_id": job["id"]}, user="root")["items"]
                    return items and items[0]["state"] == "succeeded"
                gate.wait(http_completed, "root HTTP completion")
                runs = gate.rpc(client, socket, "run.list", {"job_id": cli["id"]}, user="root")["items"]
                if allow_cli:
                    value = gate.worker_report(directory, name)
                    require(set(map(int, value["Uid"].split())) == {0}, "Explicit root CLI did not execute as root")
                    require(value["NoNewPrivs"] == "1", "Unsafe root CLI lost privilege-gain prevention")
                else:
                    require(runs[0]["state"] == "scheduled", "Disabled root CLI was dispatched")
                    require(not (directory / f"{name}.json").exists(), "Disabled root CLI started external work")
                require(process.stop() == 0, "Unsafe root fixture stop failed")
            text = process.log_path.read_text()
            require("jobud.unsafe.root_daemon" in text, "Unsafe root daemon warning missing")
            require(("jobud.unsafe.root_cli" in text) == allow_cli, "Root CLI warning disagrees with its independent flag")


def run_managed(gate, source, http):
    daemon = Path("/usr/bin/jobud")
    client = Path("/usr/bin/jobuctl")
    database = Path("/var/lib/jobu/jobu.sqlite3")
    socket = Path("/run/jobu/jobud.sock")
    active_config = Path("/etc/jobu/jobud.ini")
    work = gate.fixture / "work"
    gate.own_root(active_config.parent)
    gate.service_touched = True
    gate.run(["systemctl", "daemon-reload"])

    with gate.case("managed service requires an explicit active config"):
        gate.run(["systemctl", "start", "jobud.service"])
        register_manager_roots(gate)
        gate.wait(lambda: unit_values(gate)["ExecMainStatus"] == "1", "missing-config startup failure")
        require(not database.exists() and not socket.exists(), "Missing config admitted daemon resources")
        gate.run(["journalctl", "-u", "jobud.service", "-n", "20", "--no-pager"])
        stop_service(gate)

    template = (source / "share/jobu/jobud.ini.example").read_text()
    text = configuration(template, {"history.default_retention": "0", "telemetry.checkpoint_interval": "1s"})
    gate.config(active_config, text)

    with gate.case("system-profile installation readiness and default owner-only access"):
        gate.run([daemon, "--check-config", "--config", active_config], user="jobu")
        gate.run(["systemd-analyze", "--generators=no", "--man=no", "--recursive-errors=yes", "verify",
                  "/usr/lib/systemd/system/jobud.service"])
        current = start_service(gate, client, socket)
        gate.run(["sh", "/usr/share/jobu/services/wait-ready.sh", client, socket, "5", "1000"], user="jobu")
        gate.check_paths(database, socket)
        for leaf in (database.parent, socket.parent):
            value = metadata(leaf)
            require(value["uid"] == gate.account.pw_uid and value["mode"] == "0o700", "Managed private directory mismatch")
        for user in ("jobu926_admin", "jobu926_outside"):
            check_connection_denied(gate, client, socket, user)
        queue = gate.create_queue(client, socket, "completed-history")
        completed_job, replay = gate.create_job(client, socket, queue, gate.worker_payload(work, "managed-complete"), "managed-complete")
        completed_run = gate.run_for(client, socket, completed_job, "succeeded")
        http_job, _ = gate.create_job(client, socket, queue, http.payload("managed-complete"), "managed-http-complete", kind="http")
        http.wait("managed-complete")
        gate.run_for(client, socket, http_job, "succeeded")

    with gate.case("second installed daemon loses endpoint ownership before creating another database"):
        before_socket = metadata(socket)
        before_lock = metadata(Path(str(socket) + ".lock"))
        alternate = database.parent / "must-not-create.sqlite"
        result = gate.run([daemon, "--config", active_config, "--database", alternate], user="jobu", expected=1)
        require("jobud.socket.in_use" in result.stderr, "Second daemon did not report endpoint conflict")
        require(not alternate.exists(), "Losing endpoint contender created its database")
        require(metadata(socket) == before_socket and metadata(Path(str(socket) + ".lock")) == before_lock,
                "Endpoint competition replaced socket or lock")
        gate.ready(client, socket)

    with gate.case("managed SIGTERM cleans mixed CLI HTTP work and preserves Running for recovery"):
        queue = gate.create_queue(client, socket, "normal-stop")
        cli, _ = gate.create_job(client, socket, queue, gate.worker_payload(work, "normal-stop", "hold"), "normal-stop-cli")
        pids = work_pids(gate, work, "normal-stop")
        cli_run = gate.run_for(client, socket, cli, "running")
        http_job, _ = gate.create_job(client, socket, queue, http.payload("normal-stop", hold=True), "normal-stop-http", kind="http")
        http.wait("normal-stop")
        http_run = gate.run_for(client, socket, http_job, "running")
        cgroup = unit_values(gate)["ControlGroup"]
        stop_service(gate)
        http.release("normal-stop")
        check_children_gone(gate, pids, cgroup)
        for run in (cli_run, http_run):
            rows = read_storage(gate, database, "SELECT state FROM jobu_runs WHERE id=?", (uuid.UUID(run["id"]).bytes,))
            require(rows == [("running",)], "Graceful stop unexpectedly finalized durable Running work")
        current = start_service(gate, client, socket)
        gate.run_for(client, socket, cli, "interrupted")
        gate.run_for(client, socket, http_job, "interrupted")

    with gate.case("package reinstall preserves active config and persisted history"):
        from common import install_runtime
        stop_service(gate)
        saved_config = digest(active_config)
        saved_database = digest(database)
        install_runtime(gate, source, Path("/usr"), reinstall=True)
        require(digest(active_config) == saved_config and digest(database) == saved_database,
                "Package reinstall changed config or operator state")
        current = start_service(gate, client, socket)
        require(gate.rpc(client, socket, "job.create", replay) == completed_job, "Reinstall/restart changed accepted replay")
        require(gate.rpc(client, socket, "run.get", {"run_id": completed_run["id"]})["state"] == "succeeded",
                "Reinstall lost completed history")

    with gate.case("managed SIGKILL restarts cleans children and recovers both interruption policies"):
        jobs = []
        pids = []
        for policy in ("fail_interrupted", "retry_interrupted"):
            queue = gate.create_queue(client, socket, policy, policy=policy, concurrency=2)
            attributes = {"retry.max_attempts": 2, "retry.initial_delay": 3600000, "retry.max_delay": 3600000}
            name = f"crash-{policy}"
            cli, _ = gate.create_job(client, socket, queue, gate.worker_payload(work, name, "hold"), name,
                                     attributes=attributes)
            pids += work_pids(gate, work, name)
            gate.run_for(client, socket, cli, "running")
            http_job, _ = gate.create_job(client, socket, queue, http.payload(name, hold=True), name + "-http",
                                          kind="http", attributes=attributes)
            http.wait(name)
            gate.run_for(client, socket, http_job, "running")
            jobs.append((policy, cli, http_job))
            if policy == "fail_interrupted":
                pending, _ = gate.create_job(client, socket, queue, gate.worker_payload(work, "pending"), "crash-pending")
                pending_run = gate.run_for(client, socket, pending, "scheduled")

        timing_id = uuid.UUID(pending_run["id"]).bytes
        def checkpointed():
            rows = read_storage(gate, database, "SELECT runnable_wait_us, open_epoch IS NOT NULL FROM jobu_run_timing WHERE run_id=?",
                                (timing_id,))
            return rows and rows[0][0] > 0 and rows[0][1] == 1
        gate.wait(checkpointed, "pending timing checkpoint")
        before = unit_values(gate)
        old_pid = int(before["MainPID"])
        gate.event("service.crash", signal="SIGKILL", **before)
        os.kill(old_pid, signal.SIGKILL)

        def restarted():
            values = unit_values(gate)
            return (int(values["MainPID"]) not in (0, old_pid)
                    and int(values["NRestarts"]) > int(before["NRestarts"]))
        gate.wait(restarted, "automatic failure restart", timeout=30)
        gate.ready(client, socket)
        register_manager_roots(gate)
        after = unit_values(gate)
        gate.check_identity(int(after["MainPID"]), no_new_privileges=True)
        check_children_gone(gate, pids, before["ControlGroup"])
        require(not active_process(old_pid), "Crashed daemon is still active")
        for policy, cli, http_job in jobs:
            expected = "interrupted" if policy == "fail_interrupted" else "retry_wait"
            gate.run_for(client, socket, cli, expected)
            gate.run_for(client, socket, http_job, expected)
            http.release(f"crash-{policy}")
        gate.run_for(client, socket, pending, "succeeded")
        rows = read_storage(gate, database, "SELECT measurement_status, open_epoch IS NULL FROM jobu_run_timing WHERE run_id=?",
                            (timing_id,))
        require(rows == [("partial", 1)], "Crash lost-open timing tail was not marked partial")
        require(gate.rpc(client, socket, "job.create", replay) == completed_job, "Crash restart changed creation replay")
        require(gate.rpc(client, socket, "run.get", {"run_id": completed_run["id"]})["state"] == "succeeded",
                "Crash restart lost terminal history")
        gate.check_paths(database, socket)
        gate.event("service.restarted", old_pid=old_pid, **after)

    with gate.case("administrative socket group and filesystem outsider rejection"):
        stop_service(gate)
        gate.config(active_config, configuration(text, {"socket.mode": "0660", "socket.group": "jobu"}))
        dropin = Path("/etc/systemd/system/jobud.service.d")
        gate.own_root(dropin)
        (dropin / "group.conf").write_text("[Service]\nRuntimeDirectoryMode=0750\n")
        (dropin / "group.conf").chmod(0o644)
        gate.run(["systemctl", "daemon-reload"])
        gate.run([daemon, "--check-config", "--config", active_config], user="jobu")
        start_service(gate, client, socket)
        gate.check_paths(database, socket, mode=0o660)
        runtime = metadata(socket.parent)
        require(runtime["mode"] == "0o750" and runtime["gid"] == gate.account.pw_gid, "Administrative group cannot traverse runtime leaf")
        gate.rpc(client, socket, "system.info", user="jobu926_admin")
        gate.rpc(client, socket, "queue.create", {"name": "admin-authority"}, user="jobu926_admin")
        check_connection_denied(gate, client, socket, "jobu926_outside")
        probe = gate.run(["python3", "-c", "import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])", socket],
                         user="jobu926_outside", expected=1)
        require("PermissionError" in probe.stderr, "Outsider rejection is not filesystem permission enforcement")
        stop_service(gate)

    gate.run(["journalctl", "-u", "jobud.service", "--no-pager", "-n", "300"])
    values = unit_values(gate)
    require(values["ActiveState"] == "inactive", "Managed gate did not finish stopped")
