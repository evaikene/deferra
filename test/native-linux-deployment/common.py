"""Native gate operations; every wait and external command has a finite budget."""

from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import signal
import stat
import subprocess
import tarfile
import time


class GateFailure(RuntimeError):
    """A required observation did not match the deployment contract."""


def require(condition, message):
    if not condition:
        raise GateFailure(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def metadata(path):
    value = Path(path).lstat()
    return {
        "path": str(path), "uid": value.st_uid, "gid": value.st_gid,
        "mode": oct(stat.S_IMODE(value.st_mode)), "device": value.st_dev, "inode": value.st_ino,
    }


def credentials(pid):
    status = Path(f"/proc/{pid}/status").read_text()
    result = {"pid": pid, "executable": os.readlink(f"/proc/{pid}/exe")}
    for line in status.splitlines():
        key, _, value = line.partition(":")
        if key in {"Uid", "Gid", "Groups", "CapEff", "CapPrm", "CapAmb", "NoNewPrivs", "State"}:
            result[key] = value.strip()
    result["limits"] = Path(f"/proc/{pid}/limits").read_text()
    result["cgroup"] = Path(f"/proc/{pid}/cgroup").read_text()
    return result


def active_process(pid):
    try:
        state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
        return state != "Z"
    except FileNotFoundError:
        return False


class Gate:
    def __init__(self, evidence, fixture, revision):
        self.evidence = evidence
        self.fixture = fixture
        self.revision = revision
        self.results = []
        self.command_number = 0
        self.created_roots = []
        self.installed_files = set()
        self.created_directories = []
        self.created_accounts = []
        self.created_groups = []
        self.service_touched = False
        self.account = pwd.getpwnam("jobu")
        self.worker = fixture / "worker.py"

    def event(self, event_name, **values):
        value = {"time": datetime.now(timezone.utc).isoformat(), "event": event_name, **values}
        with (self.evidence / "events.jsonl").open("a") as output:
            output.write(json.dumps(value) + "\n")

    def run(self, arguments, *, user=None, expected=0, timeout=30, input_text=None):
        command = [str(argument) for argument in arguments]
        if user is not None:
            command = ["runuser", "-u", user, "--", *command]
        self.command_number += 1
        log = self.evidence / f"command-{self.command_number:04d}.json"
        result = subprocess.run(command, input=input_text, text=True, capture_output=True, timeout=timeout, check=False)
        log.write_text(json.dumps({"command": command, "exit": result.returncode,
                                   "stdout": result.stdout, "stderr": result.stderr}, indent=2))
        if expected is not None:
            require(result.returncode == expected, f"Command exit {result.returncode}, expected {expected}: {log}")
        return result

    def wait(self, predicate, description, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.05)
        raise GateFailure(f"Timed out waiting for {description}")

    @contextmanager
    def case(self, name):
        self.event("case.start", name=name)
        try:
            yield
        except (GateFailure, OSError, subprocess.TimeoutExpired, ValueError, KeyError, TypeError) as error:
            self.results.append({"case": name, "status": "failed", "reason": str(error)})
            self.event("case.failed", name=name, reason=str(error))
            raise
        else:
            self.results.append({"case": name, "status": "passed"})
            self.event("case.passed", name=name)
            print(f"PASS {name}", flush=True)

    def own_root(self, path, mode=0o755, owner=None):
        require(not path.exists() and not path.is_symlink(), f"Fixture path already exists: {path}")
        path.mkdir(mode=mode)
        path.chmod(mode)
        if owner is not None:
            os.chown(path, owner.pw_uid, owner.pw_gid)
        value = path.lstat()
        self.created_roots.append((path, value.st_dev, value.st_ino))

    def directory(self, path, mode=0o700, owner=None):
        path.mkdir(mode=mode)
        path.chmod(mode)
        if owner is not None:
            os.chown(path, owner.pw_uid, owner.pw_gid)

    def config(self, path, text, mode=0o640):
        path.write_text(text)
        path.chmod(mode)
        os.chown(path, 0, self.account.pw_gid)

    def rpc(self, client, socket, method, params=None, user="jobu", expected=0):
        group, operation = method.split(".", 1)
        operation = {"run_now": "run-now"}.get(operation, operation)
        arguments = [client, "--socket", socket, "--json", "--timeout", "1000"]
        if params is not None:
            arguments += ["--request-file", "-"]
        arguments += [group, operation]
        result = self.run(arguments, user=user, expected=expected,
                          input_text=None if params is None else json.dumps(params))
        if expected == 0:
            return json.loads(result.stdout)
        return result

    def ready(self, client, socket, user="jobu"):
        def probe():
            result = self.run([client, "--socket", socket, "--json", "--timeout", "500", "system", "info"],
                              user=user, expected=None, timeout=5)
            if result.returncode == 0:
                return json.loads(result.stdout)
            return None
        info = self.wait(probe, f"RPC readiness at {socket}")
        self.event("ready", socket=str(socket), info=info)
        require(info["api_version"] == {"major": 1, "minor": 4}, "Unexpected deployed RPC API")
        require(info["daemon_version"] == "0.1.0", "Unexpected deployed project version")
        return info

    def check_identity(self, pid, *, root=False, no_new_privileges=False):
        value = credentials(pid)
        expected_user = 0 if root else self.account.pw_uid
        require(all(int(part) == expected_user for part in value["Uid"].split()), "Real/effective/saved UID mismatch")
        if not root:
            require(all(int(part) == self.account.pw_gid for part in value["Gid"].split()), "GID mismatch")
            require(set(map(int, value["Groups"].split())) == {self.account.pw_gid}, "Supplementary-group mismatch")
            for capability in ("CapEff", "CapPrm", "CapAmb"):
                require(int(value[capability], 16) == 0, f"Retained {capability}")
        if no_new_privileges:
            require(value["NoNewPrivs"] == "1", "Service privilege-gain prevention missing")
            core = next(line for line in value["limits"].splitlines() if line.startswith("Max core file size"))
            require(core.split()[4:6] == ["0", "0"], "Service core limit is not zero")
        self.event("credentials", **value)
        return value

    def check_paths(self, database, socket, mode=0o600):
        values = []
        for path in [database, Path(str(database) + ".lock"), Path(str(database) + "-wal"),
                     Path(str(database) + "-shm"), Path(str(socket) + ".lock"), socket]:
            value = metadata(path)
            require(value["uid"] == self.account.pw_uid, f"Wrong owner: {path}")
            require(int(value["mode"], 8) == (mode if path == socket else 0o600), f"Wrong mode: {path}")
            values.append(value)
        self.event("paths", values=values)

    def create_queue(self, client, socket, name, policy="fail_interrupted", concurrency=4):
        return self.rpc(client, socket, "queue.create", {"name": name, "concurrency_limit": concurrency,
                        "history_retention_seconds": 0, "recovery_policy": policy})

    def create_job(self, client, socket, queue, payload, name, kind="cli", attributes=None):
        request = {"queue_id": queue["id"], "type": kind, "schedule": {"kind": "once", "at": "now"},
                   "payload": payload, "attributes": attributes or {}, "idempotency_key": name}
        job = self.rpc(client, socket, "job.create", request)
        return job, request

    def run_for(self, client, socket, job, state=None):
        def query():
            items = self.rpc(client, socket, "run.list", {"job_id": job["id"]})["items"]
            if len(items) == 1 and (state is None or items[0]["state"] == state):
                return items[0]
            return None
        return self.wait(query, f"job {job['id']} run state {state}")

    def worker_payload(self, directory, name, mode="once"):
        return {"command": "/usr/bin/python3", "arguments": [str(self.worker), mode, str(directory), name]}

    def worker_report(self, directory, name):
        path = directory / f"{name}.json"
        self.wait(path.exists, f"worker report {name}")
        value = json.loads(path.read_text())
        self.event("worker", name=name, **value)
        return value

    def cleanup(self):
        # Resource registration follows successful creation. Never remove an
        # existing operator path, nor follow a substituted fixture root.
        if self.service_touched:
            self.run(["systemctl", "stop", "jobud.service"], expected=None)
        for name in reversed(self.created_accounts):
            self.run(["userdel", name])
        for name in reversed(self.created_groups):
            # Some userdel policies also remove the matching private group.
            # Verify absence rather than treating every groupdel error as benign.
            lookup = self.run(["getent", "group", name], expected=None)
            require(lookup.returncode in (0, 2), f"Cannot verify cleanup group {name}: exit {lookup.returncode}")
            if lookup.returncode == 0:
                self.run(["groupdel", name])
            else:
                self.event("cleanup.group_absent", name=name)
        for path in sorted(self.installed_files, key=lambda item: len(item.parts), reverse=True):
            path.unlink()
        for path in reversed(self.created_directories):
            path.rmdir()
        for path, device, inode in reversed(self.created_roots):
            if not path.exists() and not path.is_symlink():
                continue
            current = path.lstat()
            require((current.st_dev, current.st_ino) == (device, inode), f"Cleanup root replaced: {path}")
            shutil.rmtree(path)
        if self.service_touched:
            self.run(["systemctl", "daemon-reload"])
            self.run(["systemctl", "reset-failed", "jobud.service"], expected=None)
        self.event("cleanup.complete")


class Foreground:
    def __init__(self, gate, daemon, database, socket, *, options=(), user="jobu", trace=None,
                 config=None, use_defaults=False):
        self.gate = gate
        self.daemon = daemon
        self.database = database
        self.socket = socket
        arguments = [str(daemon)]
        arguments += ["--no-config"] if config is None else ["--config", str(config)]
        if not use_defaults:
            arguments += ["--database", str(database), "--socket", str(socket)]
        arguments += options
        if user is not None:
            arguments = ["runuser", "-u", user, "--", *arguments]
        if trace is not None:
            arguments = ["strace", "-f", "-s", "256", "-o", str(trace),
                         "-e", "trace=%process,%file,%network,%creds,prctl,capget,capset,umask,close", *arguments]
        self.log_path = gate.evidence / f"foreground-{len(list(gate.evidence.glob('foreground-*.log'))):03d}.log"
        self.log = self.log_path.open("w")
        self.process = subprocess.Popen(arguments, stdout=self.log, stderr=self.log, start_new_session=True)
        self.pid = None
        gate.event("foreground.start", command=arguments, launcher_pid=self.process.pid, log=str(self.log_path))

    def ready(self, client, user="jobu"):
        self.gate.ready(client, self.socket, user=user)

        def find_daemon():
            # Follow only this launcher's process tree. Compiled-default launches
            # have no socket argument, and an unrelated daemon must never match.
            pending = [self.process.pid]
            while pending:
                pid = pending.pop()
                candidate = Path(f"/proc/{pid}")
                try:
                    command = (candidate / "cmdline").read_bytes().split(b"\0")
                    if command and command[0] == os.fsencode(self.daemon):
                        return pid
                    children = (candidate / "task" / str(pid) / "children").read_text().split()
                    pending.extend(int(child) for child in children)
                except (FileNotFoundError, ProcessLookupError, PermissionError):
                    continue
            return None

        self.pid = self.gate.wait(find_daemon, "foreground daemon PID")
        return self.pid

    def stop(self, number=signal.SIGTERM):
        if self.pid is not None and active_process(self.pid):
            os.kill(self.pid, number)
        elif self.process.poll() is None:
            os.killpg(self.process.pid, number)
        result = self.process.wait(timeout=20)
        self.log.close()
        self.gate.event("foreground.stop", pid=self.pid, signal=number, exit=result)
        return result

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        if self.process.poll() is None:
            self.stop()
        else:
            self.log.close()


def extract_runtime(gate, archive, destination):
    checksum = archive.with_name(archive.name + ".sha256")
    require(digest(archive) == checksum.read_text().split()[0], f"Archive checksum mismatch: {archive}")
    destination.mkdir(mode=0o755)
    destination.chmod(0o755)
    with tarfile.open(archive) as payload:
        # These archives contain only regular files/directories. Reject links,
        # devices and path escapes before any extraction into the private root.
        for member in payload.getmembers():
            path = Path(member.name)
            require(not path.is_absolute() and ".." not in path.parts, f"Escaping package member: {member.name}")
            require(member.isfile() or member.isdir(), f"Unexpected package member: {member.name}")
        payload.extractall(destination, filter="data")
    expected = {}
    inventory = archive.with_name(archive.name + ".files.sha256")
    for line in inventory.read_text().splitlines():
        checksum, name = line.split("  ", 1)
        expected[name.removeprefix("./")] = checksum
    actual = {str(path.relative_to(destination)): digest(path) for path in destination.rglob("*") if path.is_file()}
    require(actual == expected, f"Extracted inventory mismatch: {archive}")
    info = destination / "share/jobu/package/build-info-runtime.txt"
    require(gate.revision in info.read_text(), "Package source revision mismatch")
    gate.event("package", archive=str(archive), sha256=digest(archive), inventory=actual, metadata=info.read_text())
    for binary in ("jobud", "jobuctl"):
        result = gate.run(["ldd", destination / "bin" / binary])
        require("not found" not in result.stdout, f"Missing runtime library for {binary}")
    return destination


def install_runtime(gate, source, prefix, *, reinstall=False):
    files = sorted(path for path in source.rglob("*") if path.is_file())
    for path in files:
        target = prefix / path.relative_to(source)
        if not reinstall:
            require(not target.exists() and not target.is_symlink(), f"Installation collision: {target}")
        else:
            require(target in gate.installed_files, f"Reinstall path not owned by gate: {target}")
    for path in files:
        target = prefix / path.relative_to(source)
        missing = []
        parent = target.parent
        while not parent.exists():
            missing.append(parent)
            parent = parent.parent
        for directory in reversed(missing):
            directory.mkdir(mode=0o755)
            directory.chmod(0o755)
            gate.created_directories.append(directory)
        shutil.copyfile(path, target)
        os.chown(target, 0, 0)
        target.chmod(stat.S_IMODE(path.stat().st_mode))
        gate.installed_files.add(target)
        require(digest(path) == digest(target), f"Installed file hash mismatch: {target}")
    gate.event("install", prefix=str(prefix), reinstall=reinstall,
               files=[metadata(prefix / path.relative_to(source)) for path in files])
