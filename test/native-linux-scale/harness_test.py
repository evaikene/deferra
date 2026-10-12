"""Unprivileged checks of crash authorization and acceptance evidence recording."""

import json
from contextlib import closing
import os
from pathlib import Path
import selectors
import signal
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest

from verify import Gate, copy_private_database, crash_milestone, digest, open_closed_seed, wait_for_crash_launcher


class CrashMilestoneTests(unittest.TestCase):
    def test_no_marker_does_not_authorize_a_kill(self):
        self.assertIsNone(crash_milestone("Tests started\n", "purge", "write"))

    def test_exact_operation_phase_and_pid(self):
        self.assertEqual(crash_milestone("SCALE_MILESTONE\tpurge\twrite\t1234\n", "purge", "write"), 1234)

    def test_wrong_operation_or_phase_is_rejected(self):
        for text in ("SCALE_MILESTONE\tcheckpoint\twrite\t1234", "SCALE_MILESTONE\tpurge\tcommit\t1234"):
            with self.assertRaises(RuntimeError):
                crash_milestone(text, "purge", "write")

    def test_missing_extra_or_invalid_pid_is_rejected(self):
        for text in ("SCALE_MILESTONE\tpurge\twrite", "SCALE_MILESTONE\tpurge\twrite\t1234\textra",
                     "SCALE_MILESTONE\tpurge\twrite\t0", "SCALE_MILESTONE\tpurge\twrite\t-1"):
            with self.assertRaises(RuntimeError):
                crash_milestone(text, "purge", "write")


class CrashLauncherTests(unittest.TestCase):
    def check_stopped_launcher_exit(self, stops):
        # An owned unprivileged subprocess models runuser's forwarded stop.
        # A second stop exercises late/repeated forwarding without timed sleeps.
        script = ("import os, signal, sys\n"
                  "print('ready', flush=True)\n"
                  "for _ in range(int(sys.argv[1])):\n"
                  "    os.kill(os.getpid(), signal.SIGSTOP)\n"
                  "sys.exit(137)\n")
        process = subprocess.Popen([sys.executable, "-c", script, str(stops)],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        try:
            with selectors.DefaultSelector() as ready:
                ready.register(process.stdout, selectors.EVENT_READ)
                self.assertTrue(ready.select(timeout=5), "Launcher did not reach its ready barrier")
            self.assertEqual(process.stdout.readline(), "ready\n")
            self.assertEqual(wait_for_crash_launcher(process, timeout=5), 137)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
            process.stdout.close()
            process.stderr.close()

    def test_stopped_launcher_is_resumed_and_reaped(self):
        self.check_stopped_launcher_exit(1)

    def test_stop_after_an_earlier_resume_is_also_handled(self):
        self.check_stopped_launcher_exit(2)


class ClosedSeedTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / "seed.sqlite"
        with closing(sqlite3.connect(self.path)) as database:
            database.execute("PRAGMA journal_mode=WAL")
            database.execute("CREATE TABLE retained(value INTEGER)")
            database.execute("INSERT INTO retained VALUES(1)")
            database.commit()

    def test_closed_seed_reads_preserve_the_file_and_create_no_sidecars(self):
        before = digest(self.path)
        for _ in range(2):
            with closing(open_closed_seed(self.path)) as database:
                self.assertEqual(database.execute("SELECT value FROM retained").fetchall(), [(1,)])
            self.assertEqual(digest(self.path), before)
            self.assertFalse(Path(str(self.path) + "-wal").exists())
            self.assertFalse(Path(str(self.path) + "-shm").exists())

    def test_existing_wal_is_rejected_before_immutable_read_can_ignore_it(self):
        # Keep a committed WAL page outside the main file. An immutable read
        # would omit this second row if the closed-seed guard were bypassed.
        with closing(sqlite3.connect(self.path)) as writer:
            writer.execute("INSERT INTO retained VALUES(2)")
            writer.commit()
            self.assertTrue(Path(str(self.path) + "-wal").exists())
            with self.assertRaisesRegex(RuntimeError, "not closed coherently"):
                with closing(open_closed_seed(self.path)):
                    self.fail("An open WAL-mode seed was accepted")
            self.assertEqual(writer.execute("SELECT value FROM retained ORDER BY value").fetchall(), [(1,), (2,)])
            self.assertTrue(Path(str(self.path) + "-wal").exists())


class PrivateDatabaseCopyTests(unittest.TestCase):
    def test_permissive_umask_does_not_expose_a_fixture_copy_or_change_its_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "closed.sqlite"
            destination = Path(directory) / "live.sqlite"
            with closing(sqlite3.connect(source)) as database:
                database.execute("CREATE TABLE retained(value INTEGER)")
                database.execute("INSERT INTO retained VALUES(1)")
                database.commit()
            source.chmod(0o644)
            before = digest(source)

            previous_umask = os.umask(0o022)
            try:
                copy_private_database(source, destination, os.getuid(), os.getgid())
            finally:
                os.umask(previous_umask)

            self.assertEqual(digest(destination), before)
            self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o600)
            self.assertEqual(destination.stat().st_uid, os.getuid())
            self.assertEqual(destination.stat().st_gid, os.getgid())
            self.assertEqual(digest(source), before)
            self.assertEqual(stat.S_IMODE(source.stat().st_mode), 0o644)
            with closing(sqlite3.connect(f"file:{destination}?mode=ro", uri=True)) as database:
                self.assertEqual(database.execute("SELECT value FROM retained").fetchall(), [(1,)])


class GateRecordingTests(unittest.TestCase):
    def setUp(self):
        # Exercise recording without invoking privileged installation or services.
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.gate = Gate.__new__(Gate)
        self.gate.evidence = Path(directory.name)
        self.gate.events = []
        self.gate.result = {"status": "running", "cases": []}

    def test_case_name_is_an_event_field(self):
        self.gate.case("native medium")
        events = json.loads((self.gate.evidence / "events.json").read_text())
        self.assertEqual(self.gate.result["cases"], ["native medium"])
        self.assertEqual(events[0]["event"], "case.passed")
        self.assertEqual(events[0]["name"], "native medium")

    def test_unexpected_exception_writes_a_failed_result(self):
        with self.assertRaises(TypeError):
            try:
                raise TypeError("unexpected controller failure")
            finally:
                self.gate.write_result()
        result = json.loads((self.gate.evidence / "result.json").read_text())
        self.assertEqual(result["status"], "failed")
        self.assertIn("before completing", result["error"])

    def test_completed_success_is_preserved(self):
        self.gate.result["status"] = "passed"
        self.gate.write_result()
        result = json.loads((self.gate.evidence / "result.json").read_text())
        self.assertEqual(result["status"], "passed")
        self.assertNotIn("error", result)

    def test_recorded_failure_is_preserved(self):
        self.gate.result.update(status="failed", error="crash barrier deadline exceeded")
        self.gate.write_result()
        result = json.loads((self.gate.evidence / "result.json").read_text())
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["error"], "crash barrier deadline exceeded")


if __name__ == "__main__":
    unittest.main()
