#!/usr/bin/env python3
"""Explicit Stage 9.27 scale/crash gate on an approved disposable Linux VM."""

import argparse
from contextlib import closing
import hashlib
import http.server
import json
import os
from pathlib import Path
import platform
import pwd
import shutil
import signal
import sqlite3
import subprocess
import tarfile
import threading
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def open_closed_seed(path):
    """Open a privately owned, closed seed without creating WAL/SHM sidecars."""
    # Check before connecting: mode=ro can itself create sidecars for a WAL-mode
    # database. Immutable reads are safe only while this closed fixture has no
    # writer and no WAL pages to apply; live service reads must keep mode=ro.
    require(not Path(str(path) + "-wal").exists(), "Seed was not closed coherently")
    return sqlite3.connect(f"file:{path}?mode=ro&immutable=1", uri=True)


def copy_private_database(source, destination, user, group):
    """Prepare an owned fixture copy for protected daemon or helper use."""
    # copyfile creates through the controller's umask, which may be permissive.
    # Set the private mode before handing ownership to the service account;
    # leave the closed source untouched for the other independent copies.
    shutil.copyfile(source, destination)
    destination.chmod(0o600)
    os.chown(destination, user, group)


def crash_milestone(text, operation, phase):
    for line in text.splitlines():
        if line.startswith("SCALE_MILESTONE\t"):
            fields = line.split("\t")
            require(len(fields) == 4 and fields[1:3] == [operation, phase], "Unexpected crash milestone")
            pid = int(fields[3])
            require(pid > 1, "Invalid crash worker PID")
            return pid
    return None


def wait_for_crash_launcher(process, timeout=15):
    """Reap our launcher after its verified worker has received SIGKILL."""
    # runuser forwards its child's stop by stopping itself. Observe/resume the
    # owned launcher while waiting: stop forwarding can arrive after an earlier
    # resume, so one SIGCONT immediately after the child kill is insufficient.
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise subprocess.TimeoutExpired(process.args, timeout)
        try:
            return process.wait(timeout=min(0.1, remaining))
        except subprocess.TimeoutExpired:
            try:
                status = Path(f"/proc/{process.pid}/status").read_text()
                if "State:\tT" in status:
                    os.kill(process.pid, signal.SIGCONT)
            except (FileNotFoundError, ProcessLookupError):
                # The launcher can exit between the wait and status/signal calls.
                # Its PID belongs to this unreaped child; the next wait collects it.
                continue


class HeldHTTP(http.server.BaseHTTPRequestHandler):
    release = threading.Event()

    def do_GET(self):
        self.release.wait(600)
        try:
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"fixture")
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_arguments):
        pass


class Gate:
    def __init__(self, args):
        require(args.disposable_vm and os.geteuid() == 0, "Requires explicit disposable-VM acknowledgement and root")
        self.args = args
        self.account = pwd.getpwnam("jobu")
        require(self.account.pw_uid != 0, "Service identity must be non-root")
        self.evidence = args.evidence
        self.fixture = args.fixture
        self.unit = Path("/run/systemd/system/jobu927-scale.service")
        for path in (self.evidence, self.fixture, args.prefix, self.unit):
            require(path.is_absolute() and not path.exists() and not path.is_symlink(), f"Pre-existing path: {path}")
        require(str(self.fixture).startswith("/var/tmp/jobu927-"), "Fixture must use a fresh jobu927 leaf")
        require(args.prefix == Path("/opt/jobu926"), "This gate reviews the retained custom-profile package")
        require(digest(args.runtime) == args.runtime_sha256, "Runtime archive checksum mismatch")
        self.evidence.mkdir(mode=0o700)
        self.fixture.mkdir(mode=0o700)
        os.chown(self.fixture, self.account.pw_uid, self.account.pw_gid)
        self.fixture_identity = (self.fixture.stat().st_dev, self.fixture.stat().st_ino)
        self.events = []
        self.serial = 0
        self.unit_created = False
        self.prefix_created = False
        self.http = None
        self.result = {"status": "running", "source_snapshot": args.source_snapshot,
                       "product_revision": args.product_revision, "runtime_sha256": digest(args.runtime),
                       "controller_revision": args.controller_revision, "controller_sha256": digest(Path(__file__)),
                       "helper_sha256": digest(args.helper), "environment": platform.uname()._asdict(), "cases": []}

    def event(self, event_name, **values):
        self.events.append({"event": event_name, "monotonic": time.monotonic(), **values})
        (self.evidence / "events.json").write_text(json.dumps(self.events, indent=2) + "\n")

    def run(self, command, *, user=False, env=None, timeout=1200, input_text=None, expected=0):
        self.serial += 1
        command = [str(item) for item in command]
        if user:
            command = ["runuser", "-u", "jobu", "--", *command]
        begin = time.monotonic()
        result = subprocess.run(command, text=True, input=input_text, capture_output=True, timeout=timeout,
                                env={**os.environ, **(env or {})})
        log = self.evidence / f"{self.serial:04d}.log"
        log.write_text(result.stdout + "\nSTDERR\n" + result.stderr)
        self.event("command", command=command, exit=result.returncode, seconds=time.monotonic() - begin, log=log.name)
        if expected is not None:
            require(result.returncode == expected, f"Command failed ({result.returncode}): {log}")
        return result

    def helper(self, tag, path=None, **settings):
        env = {"JOBU_SCALE_PROFILE": "full", **settings}
        if path is not None:
            env["JOBU_SCALE_DATABASE"] = str(path)
        return self.run([self.args.helper, tag], user=True, env=env)

    def case(self, name):
        self.result["cases"].append(name)
        self.event("case.passed", name=name)

    def write_result(self):
        # An unexpected exception still passes through finally. Never leave its
        # retained result looking like an active or successful acceptance gate.
        if self.result["status"] == "running":
            self.result["status"] = "failed"
            self.result["error"] = "Gate exited before completing all acceptance checks"
        (self.evidence / "result.json").write_text(json.dumps(self.result, indent=2) + "\n")

    def install(self):
        with tarfile.open(self.args.runtime) as archive:
            members = archive.getmembers()
            require(all(member.isdir() or member.isfile() for member in members), "Unexpected runtime entry type")
            require(all(not Path(member.name).is_absolute() and ".." not in Path(member.name).parts
                        for member in members), "Unsafe runtime archive path")
            metadata = archive.extractfile("share/jobu/package/build-info-runtime.txt").read().decode()
            require(self.args.product_revision in metadata and "source-state=clean" in metadata,
                    "Runtime provenance differs from the selected unchanged product revision")
            self.args.prefix.mkdir(mode=0o755)
            self.prefix_created = True
            archive.extractall(self.args.prefix, filter="data")
        self.event("package.installed", metadata=metadata)

    def prepare_seed(self):
        self.http = http.server.ThreadingHTTPServer(("127.0.0.1", 0), HeldHTTP)
        self.http.daemon_threads = True
        threading.Thread(target=self.http.serve_forever, daemon=True).start()
        self.base = self.fixture / "base.sqlite"
        self.helper("[scale-seed]", self.base,
                    JOBU_SCALE_HTTP_URL=f"http://127.0.0.1:{self.http.server_port}/held")
        with closing(open_closed_seed(self.base)) as database:
            counts = database.execute("SELECT state,COUNT(*) FROM jobu_runs GROUP BY state").fetchall()
            require(dict(counts) == {"scheduled": 10000, "succeeded": 100000}, "Full-size seed counts differ")
            require(database.execute("SELECT COUNT(*) FROM jobu_queues").fetchone()[0] == 100, "Queue count differs")
        self.event("seed.closed", counts=counts, bytes=self.base.stat().st_size, sha256=digest(self.base))
        self.case("100 queues / 10000 pending / 100000 terminal seed passes production recovery")

    def crashes(self):
        for operation in ("checkpoint", "purge"):
            for phase in ("write", "commit"):
                path = self.fixture / f"{operation}-{phase}.sqlite"
                copy_private_database(self.base, path, self.account.pw_uid, self.account.pw_gid)
                env = {**os.environ, "JOBU_SCALE_PROFILE": "full", "JOBU_SCALE_DATABASE": str(path),
                       "JOBU_SCALE_CRASH_OPERATION": operation, "JOBU_SCALE_CRASH_PHASE": phase}
                log = self.evidence / f"crash-{operation}-{phase}.log"
                command = ["runuser", "-u", "jobu", "--", str(self.args.helper), "[scale-crash]"]
                with log.open("w") as output:
                    process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, env=env,
                                               start_new_session=True)
                    try:
                        deadline = time.monotonic() + 120
                        worker = None
                        while time.monotonic() < deadline:
                            worker = crash_milestone(log.read_text(), operation, phase)
                            if worker is not None:
                                status = Path(f"/proc/{worker}/status").read_text()
                                if "State:\tT" in status:
                                    require(f"PPid:\t{process.pid}\n" in status and
                                            f"Uid:\t{self.account.pw_uid}\t" in status,
                                            "Crash milestone does not name this launcher's service-account child")
                                    break
                            require(process.poll() is None, "Crash worker exited before its barrier")
                            time.sleep(0.02)
                        else:
                            raise RuntimeError("Crash barrier deadline exceeded")
                        os.kill(worker, signal.SIGKILL)
                        exit_code = wait_for_crash_launcher(process)
                        require(exit_code != 0, "Killed crash worker returned success")
                        self.event("crash.confirmed", operation=operation, phase=phase, worker=worker, exit=exit_code)
                    finally:
                        if process.poll() is None:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait(timeout=15)
                self.helper("[scale-restart]", path, JOBU_SCALE_CRASH_OPERATION=operation, JOBU_SCALE_CRASH_PHASE=phase)
                self.case(f"SIGKILL during {operation} after {phase}; production recovery and repeat recovery")
                # Evidence keeps observations, rather than four duplicate full databases on a small VM disk.
                for file in self.fixture.glob(path.name + "*"):
                    file.unlink()

    def rpc(self, method, params=None):
        group, operation = method.split(".")
        command = [self.args.prefix / "bin/jobuctl", "--socket", self.socket, "--json", "--timeout", "120000"]
        if params is not None:
            command += ["--request-file", "-"]
        command += [group, operation]
        result = self.run(command, user=True, timeout=125, input_text=None if params is None else json.dumps(params))
        return json.loads(result.stdout)

    def storage(self, sql):
        with closing(sqlite3.connect(f"file:{self.live}?mode=ro", uri=True)) as database:
            return database.execute(sql).fetchall()

    def service(self):
        self.live = self.fixture / "live.sqlite"
        copy_private_database(self.base, self.live, self.account.pw_uid, self.account.pw_gid)
        self.socket = self.fixture / "jobud.sock"
        config = self.fixture / "jobud.ini"
        config.write_text(f"database.path = {self.live}\nsocket.path = {self.socket}\ncli.concurrency = 1\n"
                          "http.concurrency = 1\nhistory.default_retention = 10s\nhistory.sweep_interval = 1s\n"
                          "history.batch_size = 100\ntelemetry.checkpoint_interval = 1s\n")
        config.chmod(0o600)
        os.chown(config, self.account.pw_uid, self.account.pw_gid)
        self.unit.write_text("[Unit]\nDescription=Disposable JobU Stage 9.27 scale fixture\n[Service]\n"
                             "Type=exec\nUser=jobu\nGroup=jobu\nUMask=0077\nNoNewPrivileges=yes\nLimitCORE=0\n"
                             f"ExecStart={self.args.prefix}/bin/jobud --config {config}\n"
                             "KillMode=mixed\nTimeoutStopSec=15\nStandardError=journal\nLogRateLimitIntervalSec=0\n")
        self.unit_created = True
        self.run(["systemd-analyze", "verify", self.unit])
        self.run(["systemctl", "daemon-reload"])
        self.run(["systemctl", "start", self.unit.name])
        deadline = time.monotonic() + 120
        while not self.socket.exists():
            require(time.monotonic() < deadline, "Native scale startup did not expose its socket")
            time.sleep(0.05)
        info = self.rpc("system.info")
        require(info["api_version"] == {"major": 1, "minor": 6}, "Unexpected native API")
        pid = int(self.run(["systemctl", "show", self.unit.name, "-p", "MainPID", "--value"]).stdout)
        status = Path(f"/proc/{pid}/status").read_text()
        require(f"Uid:\t{self.account.pw_uid}\t" in status, "Service did not retain its dedicated identity")
        self.event("service.ready", pid=pid, status=status)
        window = {"from": "1970-01-01T00:00:09Z", "to": "1970-01-01T00:00:11Z"}
        first = self.rpc("system.stats", {"planned": window, "group_by": "queue", "limit": 1})
        require(first["next_cursor"], "Expected a live statistics cursor")
        # Command/heartbeat latency is observed, not compared with a universal millisecond SLA.
        peak_rss = 0
        for sample in range(30):
            self.rpc("system.info")
            status = Path(f"/proc/{pid}/status").read_text()
            rss = int(next(line.split()[1] for line in status.splitlines() if line.startswith("VmRSS:")))
            peak_rss = max(peak_rss, rss)
            self.event("heartbeat", sample=sample, rss_kib=rss,
                       counts=self.storage("SELECT state,COUNT(*) FROM jobu_runs GROUP BY state"))
            time.sleep(0.25)
        next_page = self.rpc("system.stats", {"cursor": first["next_cursor"]})
        require(next_page["window"] == first["window"], "Retention moved a statistics cursor's window")
        for dimension in ("none", "type", "origin", "queue"):
            page = self.rpc("system.stats", {"planned": window, "group_by": dimension, "limit": 100})
            for group in page["groups"]:
                require(sum(group["runnable_wait_coverage"].values()) == group["runs"]["total"], "Coverage sum differs")
                require(group["runnable_wait_ms"]["samples"] == group["runnable_wait_coverage"]["complete"],
                        "Partial wait entered an exact duration")
        require(self.storage("SELECT COUNT(*) FROM jobu_run_timing WHERE delay_warned=1")[0][0] >= 9998,
                "Native warnings did not reach the backlog tail")
        require(self.storage("SELECT COUNT(*) FROM jobu_run_timing WHERE open_epoch IS NOT NULL "
                             "AND runnable_wait_us>0")[0][0] >= 9998, "Checkpoint sweep did not reach tail rows")
        self.event("service.scale", peak_rss_kib=peak_rss,
                   counts=self.storage("SELECT hex(queue_id),COUNT(*) FROM jobu_runs WHERE state='succeeded' GROUP BY queue_id"))
        self.run(["systemctl", "stop", self.unit.name], timeout=30)
        journal = self.run(["journalctl", "-u", self.unit.name, "--no-pager", "-o", "cat"]).stdout
        (self.evidence / "journal.log").write_text(journal)
        delayed = []
        for line in journal.splitlines():
            if line.startswith("{"):
                entry = json.loads(line)
                if entry.get("event") == "jobud.run.delayed":
                    delayed.append(entry["fields"]["run_id"])
        require(len(delayed) == len(set(delayed)) and len(delayed) >= 9998, "Warning log duplicates or missing tail")
        self.case("Installed native daemon: warnings, checkpoints, live statistics, RPC progress and healthy stop")

    def cleanup(self):
        if self.unit_created:
            self.run(["systemctl", "stop", self.unit.name], timeout=30)
            self.unit.unlink()
            self.run(["systemctl", "daemon-reload"])
            self.unit_created = False
        if self.http is not None:
            HeldHTTP.release.set()
            self.http.shutdown()
            self.http.server_close()
        if self.prefix_created:
            shutil.rmtree(self.args.prefix)
        value = self.fixture.stat()
        require((value.st_dev, value.st_ino) == self.fixture_identity, "Fixture root was replaced")
        shutil.rmtree(self.fixture)
        require(not self.unit.exists() and not self.args.prefix.exists() and not self.fixture.exists(), "Teardown incomplete")
        self.event("teardown.complete", account=pwd.getpwnam("jobu").pw_uid)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disposable-vm", action="store_true")
    for name in ("runtime", "helper", "fixture", "evidence"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--prefix", type=Path, default=Path("/opt/jobu926"))
    for name in ("source-snapshot", "product-revision", "controller-revision", "runtime-sha256"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    gate = Gate(args)
    try:
        gate.run(["systemctl", "--version"])
        gate.run(["df", "-h", "/"])
        gate.install()
        for size in ("medium", "full"):
            gate.helper("[scale]", JOBU_SCALE_PROFILE=size)
            gate.case(f"Native Release deterministic/scale/fault profile {size}")
        gate.prepare_seed()
        gate.crashes()
        gate.service()
        gate.result["status"] = "passed"
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        gate.result["status"] = "failed"
        gate.result["error"] = str(error)
    finally:
        try:
            gate.cleanup()
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            gate.result["status"] = "failed"
            gate.result["cleanup_error"] = str(error)
        gate.write_result()
    print(json.dumps(gate.result))
    return 0 if gate.result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
