"""Unprivileged regressions for native trace, installation and teardown policy."""

import errno
import os
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from common import Gate, GateFailure, require
from foreground import allowed_nss_lookup, check_startup_trace, require_installed_config_directory


MEMBERSHIP = r'''
7614  socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0) = 6
7614  connect(6, {sa_family=AF_UNIX, sun_path="/run/systemd/userdb/io.systemd.DynamicUser"}, 45) = 0
7614  close(5) = 0
7614  openat(AT_FDCWD, "/etc/group", O_RDONLY|O_CLOEXEC) = 5
7614  close(5) = 0
7614  sendto(6, "{\"method\":\"io.systemd.UserDatabase.GetMemberships\",\"parameters\":{\"userName\":\"jobu\",\"service\":\"io.systemd.DynamicUser\"},\"more\":true}\0", 132, MSG_DONTWAIT|MSG_NOSIGNAL, NULL, 0) = 132
7614  recvfrom(6, "{\"error\":\"io.systemd.UserDatabase.NoRecordFound\"}\0", 131080, MSG_DONTWAIT, NULL, NULL) = 50
7614  close(6) = 0
'''.strip().splitlines()

STARTUP = r'''
7614  umask(077) = 077
7614  setgroups(1, [983]) = 0
7614  setresgid(983, 983, 983) = 0
7614  setresuid(999, 999, 999) = 0
7614  capget({version=_LINUX_CAPABILITY_VERSION_3, pid=0}, {effective=0, permitted=0, inheritable=0}) = 0
7614  openat(AT_FDCWD, "/state/jobu.sqlite3", O_RDWR|O_CREAT|O_CLOEXEC, 0600) = 11
7614  socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0) = 15
7614  bind(15, {sa_family=AF_UNIX, sun_path="/runtime/jobud.sock"}, 34) = 0
7614  chmod("/runtime/jobud.sock", 0600) = 0
7614  listen(15, 128) = 0
'''.strip().splitlines()

RECEIVE_RETRY = '7614  recvfrom(6, 0x62ca4624eec0, 131080, MSG_DONTWAIT, NULL, NULL) = -1 EAGAIN (Resource temporarily unavailable)'


class NssTraceTest(unittest.TestCase):
    def test_observed_membership_transaction_is_allowed(self):
        self.assertTrue(allowed_nss_lookup(MEMBERSHIP, 0, "jobu"))

    def test_absent_nss_cache_probe_remains_allowed(self):
        lines = [MEMBERSHIP[0],
                 '7614  connect(6, {sa_family=AF_UNIX, sun_path="/var/run/nscd/socket"}, 110) = -1 ENOENT (No such file or directory)',
                 '7614  close(6) = 0']
        self.assertTrue(allowed_nss_lookup(lines, 0, "jobu"))

    def test_endpoint_request_response_and_completion_are_required(self):
        mutations = [
            (1, 'io.systemd.DynamicUser', 'other-service'),
            (1, '= 0', '= -1 ECONNREFUSED'),
            (5, 'GetMemberships', 'SetMemberships'),
            (5, r'\"jobu\"', r'\"root\"'),
            (5, 'io.systemd.DynamicUser', 'other-service'),
            (5, '= 132', '= 131'),
            (6, 'NoRecordFound', 'AccessDenied'),
            (6, 'recvfrom(6,', 'recvfrom(7,'),
            (6, '7614', '7615'),
            (7, '= 0', '= -1 EINTR'),
        ]
        for index, old, new in mutations:
            with self.subTest(index=index, replacement=new):
                lines = MEMBERSHIP.copy()
                self.assertIn(old, lines[index])
                lines[index] = lines[index].replace(old, new, 1)
                self.assertFalse(allowed_nss_lookup(lines, 0, "jobu"))
        self.assertFalse(allowed_nss_lookup(MEMBERSHIP[:-1], 0, "jobu"))

    def test_transaction_must_close_before_uid_drop(self):
        lines = MEMBERSHIP[:-1] + ['7614  setresuid(999, 999, 999) = 0', MEMBERSHIP[-1]]
        self.assertFalse(allowed_nss_lookup(lines, 0, "jobu"))
        self.assertFalse(allowed_nss_lookup(MEMBERSHIP, 0, "jobu", before=len(MEMBERSHIP) - 1))

    def test_nonblocking_receive_retries_still_require_reply_and_close(self):
        for count in (1, 3):
            with self.subTest(retry_count=count):
                lines = MEMBERSHIP[:6] + [RECEIVE_RETRY] * count + MEMBERSHIP[6:]
                self.assertTrue(allowed_nss_lookup(lines, 0, "jobu"))
                self.check_startup(lines + STARTUP)
                self.assertFalse(allowed_nss_lookup(lines[:-1], 0, "jobu"))
                self.assertFalse(allowed_nss_lookup(MEMBERSHIP[:6] + [RECEIVE_RETRY] * count, 0, "jobu"))

    def test_receive_retries_do_not_hide_other_errors(self):
        for error in ("EINTR", "ECONNRESET", "ETIMEDOUT"):
            with self.subTest(error=error):
                lines = MEMBERSHIP[:6] + [RECEIVE_RETRY.replace("EAGAIN", error)] + MEMBERSHIP[6:]
                self.assertFalse(allowed_nss_lookup(lines, 0, "jobu"))

    def test_receive_retry_is_allowed_only_while_awaiting_reply(self):
        before_request = MEMBERSHIP[:5] + [RECEIVE_RETRY] + MEMBERSHIP[5:]
        after_reply = MEMBERSHIP[:7] + [RECEIVE_RETRY] + MEMBERSHIP[7:]
        self.assertFalse(allowed_nss_lookup(before_request, 0, "jobu"))
        self.assertFalse(allowed_nss_lookup(after_reply, 0, "jobu"))

    def test_retry_does_not_substitute_for_same_pid_and_descriptor_reply(self):
        for old, new in (("7614", "7615"), ("recvfrom(6,", "recvfrom(7,")):
            with self.subTest(replacement=new):
                reply = MEMBERSHIP[6].replace(old, new, 1)
                lines = MEMBERSHIP[:6] + [RECEIVE_RETRY, reply, MEMBERSHIP[-1]]
                self.assertFalse(allowed_nss_lookup(lines, 0, "jobu"))

    def test_receive_retry_cannot_complete_after_uid_drop(self):
        lines = MEMBERSHIP[:6] + [RECEIVE_RETRY, '7614  setresuid(999, 999, 999) = 0'] + MEMBERSHIP[6:]
        self.assertFalse(allowed_nss_lookup(lines, 0, "jobu"))

    def check_startup(self, lines):
        gate = SimpleNamespace(account=SimpleNamespace(pw_uid=999, pw_gid=983, pw_name="jobu"),
                               event=lambda *arguments, **values: None)
        with tempfile.TemporaryDirectory(prefix="jobu926-trace-check-") as directory:
            trace = Path(directory) / "startup.trace"
            trace.write_text("\n".join(lines) + "\n")
            check_startup_trace(gate, trace, Path("/runtime/jobud.sock"), 0o600)

    def test_membership_lookup_preserves_startup_order_checks(self):
        self.check_startup(MEMBERSHIP + STARTUP)

    def test_other_early_network_workers_and_database_remain_failures(self):
        prohibited = [
            '7614  socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0) = 10',
            '7614  clone(child_stack=NULL, flags=SIGCHLD) = 7615',
            '7614  openat(AT_FDCWD, "/state/jobu.sqlite3", O_RDWR|O_CREAT, 0600) = 11',
        ]
        for operation in prohibited:
            with self.subTest(operation=operation), self.assertRaises(GateFailure):
                self.check_startup(MEMBERSHIP + [operation] + STARTUP)

    def test_permissive_mode_before_listen_remains_failure(self):
        lines = MEMBERSHIP + [line.replace('0600) = 0', '0666) = 0') for line in STARTUP]
        with self.assertRaises(GateFailure):
            self.check_startup(lines)


class InstalledConfigDirectoryTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="jobu926-config-check-")
        self.addCleanup(scratch.cleanup)
        self.directory = Path(scratch.name) / "etc"
        self.directory.mkdir()
        self.directory.chmod(0o755)
        self.gate = SimpleNamespace(created_directories=[self.directory])

    def test_reuses_installed_directory_without_changing_metadata_or_contents(self):
        readme = self.directory / "README.md"
        readme.write_text("Packaged configuration guidance.\n")
        before = self.directory.lstat()

        require_installed_config_directory(self.gate, self.directory)

        after = self.directory.lstat()
        self.assertEqual((after.st_mode, after.st_uid, after.st_gid, after.st_dev, after.st_ino, after.st_mtime_ns),
                         (before.st_mode, before.st_uid, before.st_gid, before.st_dev, before.st_ino, before.st_mtime_ns))
        self.assertEqual(readme.read_text(), "Packaged configuration guidance.\n")

    def test_rejects_directory_not_created_by_this_installation(self):
        self.gate.created_directories.clear()
        with self.assertRaisesRegex(GateFailure, "not created by this installation"):
            require_installed_config_directory(self.gate, self.directory)

    def test_rejects_symlink_without_changing_its_target(self):
        self.directory.rmdir()
        target = self.directory.parent / "target"
        target.mkdir()
        before = target.lstat()
        self.directory.symlink_to(target, target_is_directory=True)

        with self.assertRaisesRegex(GateFailure, "not a real directory"):
            require_installed_config_directory(self.gate, self.directory)

        self.assertTrue(self.directory.is_symlink())
        self.assertEqual(target.lstat(), before)

    def test_rejects_non_directory_without_replacing_it(self):
        self.directory.rmdir()
        self.directory.write_text("Preserve this collision.\n")

        with self.assertRaisesRegex(GateFailure, "not a real directory"):
            require_installed_config_directory(self.gate, self.directory)

        self.assertEqual(self.directory.read_text(), "Preserve this collision.\n")

    def test_rejects_wrong_mode_without_repairing_it(self):
        self.directory.chmod(0o750)

        with self.assertRaisesRegex(GateFailure, "does not have mode 0755"):
            require_installed_config_directory(self.gate, self.directory)

        self.assertEqual(stat.S_IMODE(self.directory.lstat().st_mode), 0o750)

    def test_rejects_directory_owned_by_another_installer(self):
        # Simulate a different installer identity without requiring privileged chown.
        owner = self.directory.lstat().st_uid
        with mock.patch("foreground.os.geteuid", return_value=os.geteuid() + 1):
            with self.assertRaisesRegex(GateFailure, "unexpected owner"):
                require_installed_config_directory(self.gate, self.directory)

        self.assertEqual(self.directory.lstat().st_uid, owner)


class OwnedRootCleanupTest(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="jobu926-cleanup-check-")
        self.addCleanup(scratch.cleanup)
        self.scratch = Path(scratch.name)
        self.gate = object.__new__(Gate)
        self.gate.service_touched = False
        self.gate.created_accounts = []
        self.gate.created_groups = []
        self.gate.installed_files = set()
        self.gate.created_directories = []
        self.gate.created_roots = []
        self.events = []
        self.gate.event = lambda name, **values: self.events.append((name, values))

    def test_owned_root_removes_package_parent_with_active_configuration(self):
        root = self.scratch / "prefix"
        self.gate.own_root(root)
        config = root / "etc"
        self.gate.directory(config, mode=0o755)
        self.gate.created_directories.append(config)
        readme = config / "README.md"
        readme.write_text("Packaged guidance.\n")
        self.gate.installed_files.add(readme)
        (config / "jobud.ini").write_text("Active fixture configuration.\n")

        self.gate.cleanup()

        self.assertFalse(root.exists())
        self.assertEqual(self.events[-1][0], "cleanup.complete")

    def test_separate_empty_installation_directory_is_removed(self):
        directory = self.scratch / "shared-installation"
        self.gate.directory(directory)
        self.gate.created_directories.append(directory)

        self.gate.cleanup()

        self.assertFalse(directory.exists())
        self.assertEqual(self.events[-1][0], "cleanup.complete")

    def test_nonempty_outside_directory_is_preserved_and_stops_cleanup(self):
        root = self.scratch / "prefix"
        self.gate.own_root(root)
        # A shared name prefix must not make this a descendant of the owned root.
        outside = self.scratch / "prefix-other"
        self.gate.directory(outside)
        self.gate.created_directories.append(outside)
        sentinel = outside / "operator-file"
        sentinel.write_text("Preserve this unexpected file.\n")

        with self.assertRaises(OSError) as error:
            self.gate.cleanup()

        self.assertEqual(error.exception.errno, errno.ENOTEMPTY)
        self.assertEqual(sentinel.read_text(), "Preserve this unexpected file.\n")
        self.assertTrue(root.exists())
        self.assertNotIn(("cleanup.complete", {}), self.events)

    def test_replaced_root_is_rejected_before_removing_descendants(self):
        root = self.scratch / "prefix"
        self.gate.own_root(root)
        config = root / "etc"
        self.gate.directory(config)
        self.gate.created_directories.append(config)
        (config / "jobud.ini").write_text("Original fixture.\n")
        saved = self.scratch / "original-prefix"
        root.rename(saved)
        config.mkdir(parents=True)
        replacement = config / "operator-file"
        replacement.write_text("Preserve the replacement.\n")

        with self.assertRaisesRegex(GateFailure, "Cleanup root replaced"):
            self.gate.cleanup()

        self.assertEqual(replacement.read_text(), "Preserve the replacement.\n")
        self.assertEqual((saved / "etc/jobud.ini").read_text(), "Original fixture.\n")
        self.assertNotIn(("cleanup.complete", {}), self.events)

    def test_owned_root_cleanup_does_not_follow_descendant_symlink(self):
        root = self.scratch / "prefix"
        self.gate.own_root(root)
        outside = self.scratch / "outside"
        outside.mkdir()
        sentinel = outside / "operator-file"
        sentinel.write_text("Preserve the symlink target.\n")
        config = root / "etc"
        config.symlink_to(outside, target_is_directory=True)
        self.gate.created_directories.append(config)

        self.gate.cleanup()

        self.assertFalse(root.exists())
        self.assertEqual(sentinel.read_text(), "Preserve the symlink target.\n")
        self.assertEqual(self.events[-1][0], "cleanup.complete")


class ProbeGroupCleanupTest(unittest.TestCase):
    def gate(self, lookup_exit, removal_exit=0):
        gate = object.__new__(Gate)
        gate.service_touched = False
        gate.created_accounts = ["probe"]
        gate.created_groups = ["probe"]
        gate.installed_files = set()
        gate.created_directories = []
        gate.created_roots = []
        gate.commands = []
        gate.events = []

        # Replace external operations so this test never mutates native accounts.
        def run(arguments, *, expected=0):
            gate.commands.append(arguments)
            exit_code = lookup_exit if arguments[0] == "getent" else removal_exit if arguments[0] == "groupdel" else 0
            if expected is not None:
                require(exit_code == expected, "Injected group removal failure")
            return SimpleNamespace(returncode=exit_code)

        gate.run = run
        gate.event = lambda event, **values: gate.events.append((event, values))
        return gate

    def test_userdel_removed_private_group(self):
        gate = self.gate(2)
        gate.cleanup()
        self.assertEqual(gate.commands, [["userdel", "probe"], ["getent", "group", "probe"]])
        self.assertIn(("cleanup.group_absent", {"name": "probe"}), gate.events)
        self.assertEqual(gate.events[-1][0], "cleanup.complete")

    def test_remaining_registered_group_is_removed(self):
        gate = self.gate(0)
        gate.cleanup()
        self.assertEqual(gate.commands[-1], ["groupdel", "probe"])
        self.assertEqual(gate.events[-1][0], "cleanup.complete")

    def test_lookup_error_blocks_cleanup_success(self):
        gate = self.gate(1)
        with self.assertRaises(GateFailure):
            gate.cleanup()
        self.assertNotIn(["groupdel", "probe"], gate.commands)
        self.assertEqual(gate.events, [])

    def test_group_removal_error_blocks_cleanup_success(self):
        gate = self.gate(0, removal_exit=1)
        with self.assertRaises(GateFailure):
            gate.cleanup()
        self.assertEqual(gate.events, [])


if __name__ == "__main__":
    unittest.main()
