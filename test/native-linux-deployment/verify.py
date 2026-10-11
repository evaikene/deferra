"""Opt-in Stage 9.26 gate. Run only as root on an approved disposable systemd VM."""

import argparse
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import subprocess
import sys
import traceback

from common import Gate, GateFailure, digest, extract_runtime, install_runtime, require
from foreground import run_foreground
from http_fixture import LocalHttp
from managed import root_cli_guards, run_managed


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disposable-vm", action="store_true", required=True,
                        help="Explicitly authorize this invocation's native installation and service cases")
    parser.add_argument("--system-runtime", type=Path, required=True)
    parser.add_argument("--custom-runtime", type=Path, required=True)
    parser.add_argument("--identity-helper", type=Path, required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    return parser.parse_args()


def preflight(options):
    require(os.geteuid() == 0, "Native gate incomplete: root privileges required")
    require(Path("/proc/1/comm").read_text().strip() == "systemd", "Native gate incomplete: PID 1 must be systemd")
    require(re.fullmatch(r"[0-9a-f]{40}", options.source_revision), "A full source revision is required")
    require(options.evidence.is_absolute() and not options.evidence.exists() and not options.evidence.is_symlink(),
            "Evidence must be a new absolute directory")
    for tool in ("strace", "systemctl", "systemd-analyze", "journalctl", "runuser", "ldd", "groupadd", "groupdel", "useradd", "userdel"):
        require(shutil.which(tool) is not None, f"Native gate incomplete: missing {tool}")
    account = pwd.getpwnam("jobu")
    require(account.pw_uid != 0 and account.pw_shell.endswith("/nologin"), "jobu must be a dedicated non-root nologin account")
    for path in ("/usr/bin/jobud", "/usr/bin/jobuctl", "/usr/share/jobu", "/etc/jobu", "/var/lib/jobu", "/run/jobu",
                 "/opt/jobu926", "/usr/lib/systemd/system/jobud.service", "/etc/systemd/system/jobud.service",
                 "/etc/systemd/system/jobud.service.d"):
        value = Path(path)
        require(not value.exists() and not value.is_symlink(), f"Deployment fixture collision: {path}")
    for name in ("jobu926_admin", "jobu926_outside"):
        result = subprocess.run(["getent", "passwd", name], capture_output=True, text=True, check=False)
        require(result.returncode == 2, f"Test account already exists or lookup failed: {name}")
    result = subprocess.run(["getent", "group", "jobu926_outside"], capture_output=True, text=True, check=False)
    require(result.returncode == 2, "Test outsider group already exists or lookup failed")
    for archive in (options.system_runtime, options.custom_runtime):
        require(archive.is_absolute() and archive.is_file(), "An absolute Runtime archive is required")
    require(options.identity_helper.is_absolute() and options.identity_helper.is_file(), "An absolute identity helper is required")


def provision_probe_accounts(gate):
    # Preserve the existing service account and its group membership. Probe
    # identities are newly created, non-login accounts owned by this gate.
    gate.run(["groupadd", "--system", "jobu926_outside"])
    gate.created_groups.append("jobu926_outside")
    for name, group in (("jobu926_admin", "jobu"), ("jobu926_outside", "jobu926_outside")):
        gate.run(["useradd", "--system", "--gid", group, "--home-dir", "/nonexistent", "--no-create-home",
                  "--shell", "/usr/sbin/nologin", name])
        gate.created_accounts.append(name)
        gate.run(["id", name])


def main():
    options = arguments()
    preflight(options)
    os.umask(0o077)
    options.evidence.mkdir(mode=0o700)
    fixture = Path(f"/var/tmp/jobu926-gate-{os.getpid()}")
    gate = Gate(options.evidence, fixture, options.source_revision)
    status = "incomplete"
    error_text = None
    try:
        gate.own_root(fixture)
        shutil.copyfile(Path(__file__).with_name("worker.py"), gate.worker)
        gate.worker.chmod(0o644)
        with gate.case("native environment package integrity and installation preflight"):
            for command in (["uname", "-a"], ["systemctl", "--version"], ["strace", "--version"],
                            ["id", "jobu"], ["passwd", "-S", "jobu"], ["df", "-h", "/", "/var/tmp"]):
                result = gate.run(command)
                if command[:2] == ["passwd", "-S"]:
                    require(result.stdout.split()[1] == "L", "Dedicated jobu password is not locked")
            load = gate.run(["systemctl", "show", "jobud.service", "-p", "LoadState", "--value"])
            require(load.stdout.strip() == "not-found", "An existing JobU unit is loaded")
            system = extract_runtime(gate, options.system_runtime, fixture / "system-runtime")
            custom = extract_runtime(gate, options.custom_runtime, fixture / "custom-runtime")
            gate.event("identity.helper", path=str(options.identity_helper), sha256=digest(options.identity_helper))
            for source, profile, prefix in ((system, "system", "/usr"), (custom, "custom", "/opt/jobu926")):
                text = (source / "share/jobu/package/build-info-runtime.txt").read_text()
                for expected in ("source-state=clean", "build-configuration=Release", "tests-enabled=OFF",
                                 "examples-enabled=OFF", f"systemd-profile={profile}", f"configured-install-prefix={prefix}"):
                    require(expected in text.splitlines(), f"Package provenance mismatch: {expected}")
            gate.own_root(Path("/opt/jobu926"))
            install_runtime(gate, custom, Path("/opt/jobu926"))
            install_runtime(gate, system, Path("/usr"))
            require(not Path("/etc/jobu").exists() and not Path("/var/lib/jobu").exists() and not Path("/run/jobu").exists(),
                    "Package installation created operational state")
            provision_probe_accounts(gate)

        run_foreground(gate, Path("/opt/jobu926"), options.identity_helper)
        with LocalHttp(gate) as http:
            root_cli_guards(gate, Path("/opt/jobu926/bin/jobud"), Path("/opt/jobu926/bin/jobuctl"), http)
            run_managed(gate, system, http)
        status = "passed"
    except (GateFailure, OSError, subprocess.TimeoutExpired, ValueError, KeyError, TypeError) as error:
        status = "failed"
        error_text = str(error)
        (options.evidence / "failure.txt").write_text(traceback.format_exc())
        print(f"FAIL {error}; evidence: {options.evidence}", file=sys.stderr, flush=True)
    finally:
        try:
            gate.cleanup()
        except (GateFailure, OSError, subprocess.TimeoutExpired, TypeError) as error:
            status = "failed"
            error_text = f"{error_text or ''}; teardown: {error}"
            (options.evidence / "cleanup-failure.txt").write_text(traceback.format_exc())
        summary = {"status": status, "source_revision": options.source_revision, "cases": gate.results,
                   "error": error_text, "evidence": str(options.evidence)}
        (options.evidence / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"Native gate {status}: {len(gate.results)} case outcomes; {options.evidence}", flush=True)
    return 0 if status == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
