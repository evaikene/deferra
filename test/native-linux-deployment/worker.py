"""Disposable CLI work with an observable start and explicit completion gate."""

import json
import os
from pathlib import Path
import signal
import sys
import time


def report(path):
    status = Path("/proc/self/status").read_text()
    credentials = {
        line.split(":", 1)[0]: line.split(":", 1)[1].strip()
        for line in status.splitlines()
        if line.startswith(("Uid:", "Gid:", "Groups:", "NoNewPrivs:", "CapEff:", "CapPrm:", "CapAmb:"))
    }
    value = {"pid": os.getpid(), "parent": os.getppid(), "group": os.getpgrp(), **credentials}
    temporary = path.with_suffix(".new")
    temporary.write_text(json.dumps(value))
    temporary.replace(path)
    return value


def main():
    mode, directory, name = sys.argv[1:]
    directory = Path(directory)
    if mode == "hold":
        # Both processes remain in the executor's normal group. Ignore TERM so
        # the test observes its final cleanup boundary, without escaping a session.
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        if os.fork() == 0:
            report(directory / f"{name}-descendant.json")
            while not (directory / f"{name}.release").exists():
                time.sleep(0.05)
            return

    value = report(directory / f"{name}.json")
    if mode == "hold":
        while not (directory / f"{name}.release").exists():
            time.sleep(0.05)
        os.wait()
    print(json.dumps(value), flush=True)


if __name__ == "__main__":
    main()
