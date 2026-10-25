#!/usr/bin/env python3
"""Integration tests for built-ins, external processes, and terminal job control."""

from __future__ import annotations

import os
import pty
import re
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path


from support import STATE, NESTED, AUTH, output

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("OS_SHELL_BINARY", ROOT / "build" / "os-shell")).resolve()


def run_shell(commands: str, cwd: Path, timeout: float = 8.0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [str(BINARY), "--state", str(STATE)],
        input=AUTH+commands,
        cwd=cwd,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )

    result.stdout=output(result.stdout)
    return result

class ShellBatchTests(unittest.TestCase):
    def test_builtins_and_external_command(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            commands = "\n".join(
                [
                    "pwd",
                    'echo "process management works"',
                    "mkdir demo",
                    "cd demo",
                    "touch notes.txt",
                    "/bin/sh -c 'printf kernel-notes > notes.txt'",
                    "cat notes.txt",
                    "ls",
                    "cd ..",
                    "rm demo/notes.txt",
                    "rmdir demo",
                    "exit",
                    "",
                ]
            )
            result = run_shell(commands, directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(str(directory), result.stdout)
            self.assertIn("process management works", result.stdout)
            self.assertIn("kernel-notes", result.stdout)
            self.assertIn("notes.txt", result.stdout)
            self.assertFalse((directory / "demo").exists())

    def test_errors_and_exit_status(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            result = run_shell(
                "cat missing.txt\nmkdir\nno_such_command_xyz\necho 'unfinished\nexit 7\n",
                Path(temporary),
            )
            self.assertEqual(result.returncode, 7)
            self.assertIn("cat: missing.txt", result.stderr)
            self.assertIn("mkdir: expected", result.stderr)
            self.assertIn("no_such_command_xyz", result.stderr)
            self.assertIn("unmatched quote", result.stderr)

    def test_background_job_and_fg(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            result = run_shell(
                "sleep 0.3 &\njobs\nfg %1\necho foreground-complete\nexit\n",
                Path(temporary),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertRegex(result.stdout, r"\[1\] \d+")
            self.assertIn("Running", result.stdout)
            self.assertIn("foreground-complete", result.stdout)

    def test_clear_and_kill_builtins(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            clear_result = run_shell("clear\nexit\n", Path(temporary))
            self.assertEqual(clear_result.returncode, 0, clear_result.stderr)
            self.assertIn("\x1b[2J\x1b[H", clear_result.stdout)

            victim = subprocess.Popen(["sleep", "30"])
            try:
                kill_result = run_shell(f"kill {victim.pid}\nexit\n", Path(temporary))
                self.assertEqual(kill_result.returncode, 0, kill_result.stderr)
                victim.wait(timeout=3.0)
                self.assertEqual(victim.returncode, -15)
            finally:
                if victim.poll() is None:
                    victim.terminate()
                    victim.wait(timeout=3.0)

    def test_empty_cat_and_directory_cat_preserve_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            result = run_shell(
                "touch empty.txt\ncat empty.txt\necho after-empty\ncat .\n"
                "echo after-directory\nexit 0\n", directory)
            self.assertIn("after-empty", result.stdout)
            self.assertIn("after-directory", result.stdout)
            self.assertIn("Is a directory", result.stderr)

    def test_symbolic_links_obey_rm_and_rmdir_semantics(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "target").mkdir()
            (directory / "keep-link").symlink_to("target", target_is_directory=True)
            (directory / "remove-link").symlink_to("target", target_is_directory=True)
            result = run_shell("rmdir keep-link\nrm remove-link\nexit 0\n", directory)
            self.assertIn("rmdir:", result.stderr)
            self.assertTrue((directory / "keep-link").is_symlink())
            self.assertFalse((directory / "remove-link").is_symlink())
            self.assertTrue((directory / "target").is_dir())

    def test_touch_preserves_data_and_updates_timestamp(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            target = directory / "file with spaces.txt"
            target.write_text("keep these notes")
            os.utime(target, (100, 100))
            result = run_shell('touch "file with spaces.txt"\ncat "file with spaces.txt"\nexit\n', directory)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(target.read_text(), "keep these notes")
            self.assertGreater(target.stat().st_mtime, 100)

    def test_error_statuses_are_returned_at_eof_and_exit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for commands, expected in [
                ("no_such_command_xyz\n", 127),
                ("/usr/bin/false\nexit\n", 1),
                ("echo 'unfinished\n", 2),
                ("mkdir\n", 1),
                ("exit 7\n", 7),
            ]:
                with self.subTest(commands=commands):
                    self.assertEqual(run_shell(commands, directory).returncode, expected)

    def test_background_builtins_use_child_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "child-directory").mkdir()
            result = run_shell(
                "echo background-builtin &\ncd child-directory &\nsleep 0.2\n"
                "pwd\nexit 0\n", directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("background-builtin", result.stdout)
            self.assertIn(str(directory.resolve()), result.stdout)
            self.assertNotIn(str(directory.resolve() / "child-directory"), result.stdout)

    def test_invalid_job_and_pid_values(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            result = run_shell(
                "fg %999\nbg abc\nkill -1\nkill 0\nkill abc\n"
                "kill 999999999999999999999\nfg &\nexit 0\n", Path(temporary))
            self.assertIn("fg: no such job", result.stderr)
            self.assertIn("bg: no such job", result.stderr)
            self.assertEqual(result.stderr.count("PID must be a positive integer"), 4)
            self.assertIn("stateful commands require a standalone foreground command", result.stderr)

    def test_immediate_exit_after_background_launch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            result = run_shell("sleep 30 &\nexit 0\n", Path(temporary), timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_batch_background_reader_does_not_steal_commands(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            reader = f'''{sys.executable} -c 'import sys; print("BACKGROUND-READ", repr(sys.stdin.read()))' &'''
            result = run_shell(reader + "\nsleep 0.2\necho parent-command-survived\nexit 0\n",
                               Path(temporary))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("BACKGROUND-READ ''", result.stdout)
            self.assertIn("\nparent-command-survived\n", result.stdout)

    def test_double_quoted_paths_keep_literal_backslashes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            result = run_shell('echo "C:\\notes"\necho "a\\q"\nexit 0\n', Path(temporary))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, "C:\\notes\na\\q\n")


class PtySession:
    def __init__(self) -> None:
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(ROOT)
            os.execl(str(BINARY), str(BINARY), "--state", str(STATE))
        self.transcript = bytearray()
        self.cursor = 0
        self.expect(b"Username: ")
        self.send(b"admin\n")
        self.expect(b"Password: ")
        self.send(b"AdminTest123!\n")
        self.expect(b"Authenticated user=admin role=admin")

    def send(self, data: bytes) -> None:
        os.write(self.fd, data)

    def expect(self, pattern: bytes, timeout: float = 5.0) -> bytes:
        expression = re.compile(pattern, re.DOTALL)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            match = expression.search(self.transcript, self.cursor)
            if match:
                self.cursor = match.end()
                return bytes(self.transcript)
            readable, _, _ = select.select([self.fd], [], [], 0.1)
            if not readable:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            self.transcript.extend(chunk)
        raise AssertionError(
            f"Timed out waiting for {pattern!r}. Transcript:\n"
            + self.transcript.decode(errors="replace")
        )

    def reset_capture(self) -> None:
        self.transcript.clear()
        self.cursor = 0

    def close(self) -> None:
        try:
            self.send(b"exit 0\n")
        except OSError:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            try:
                waited, _ = os.waitpid(self.pid, os.WNOHANG)
                if waited == self.pid: return
            except ChildProcessError:
                return
            time.sleep(0.02)
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except (ProcessLookupError, ChildProcessError):
            pass


class ShellJobControlTests(unittest.TestCase):
    def test_stop_background_resume_and_interrupt(self) -> None:
        session = PtySession()
        try:
            session.expect(rb"\$ ")
            session.reset_capture()

            session.send(b"sleep 10\n")
            time.sleep(0.25)
            session.send(b"\x1a")  # Ctrl-Z
            session.expect(rb"\[1\] Stopped.*\$ ")

            session.reset_capture()
            session.send(b"jobs\n")
            session.expect(rb"\[1\] Stopped.*sleep 10.*\$ ")

            session.reset_capture()
            session.send(b"bg %1\n")
            session.expect(rb"\[1\].*sleep 10.*\$ ")

            session.reset_capture()
            session.send(b"jobs\n")
            session.expect(rb"\[1\] Running.*sleep 10.*\$ ")

            session.reset_capture()
            session.send(b"fg %1\n")
            time.sleep(0.25)
            session.send(b"\x03")  # Ctrl-C
            session.expect(rb"Terminated by signal 2.*\$ ")

            session.send(b"exit 0\n")
            waited_pid, status = os.waitpid(session.pid, 0)
            self.assertEqual(waited_pid, session.pid)
            self.assertTrue(os.WIFEXITED(status))
            self.assertEqual(os.WEXITSTATUS(status), 0)
        finally:
            session.close()

    def test_idle_background_child_is_reaped_without_more_input(self) -> None:
        session = PtySession()
        try:
            session.expect(rb"\$ ")
            session.send(b"sleep 0.2 &\n")
            transcript = session.expect(rb"\[1\] (\d+)\r?\n")
            pid = int(re.search(rb"\[1\] (\d+)", transcript).group(1))
            session.expect(rb"\[1\] Done")
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
        finally:
            session.close()

    def test_interactive_reader_receives_terminal_before_launch(self) -> None:
        session = PtySession()
        try:
            session.expect(rb"\$ ")
            for number in range(12):
                session.send(b"/bin/cat\n")
                session.expect(rb"/bin/cat\r?\n")
                deadline = time.monotonic() + 2
                while os.tcgetpgrp(session.fd) == session.pid and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertNotEqual(os.tcgetpgrp(session.fd), session.pid)
                marker = f"reader-{number}".encode()
                session.send(marker + b"\n")
                session.expect(marker + rb"\r?\n" + marker + rb"\r?\n")
                session.send(b"\x04")
                session.expect(rb"\$ ")
                self.assertEqual(os.tcgetpgrp(session.fd), session.pid)
        finally:
            session.close()

    def test_background_terminal_reader_stops_then_fg_resumes(self) -> None:
        session = PtySession()
        try:
            session.expect(rb"\$ ")
            # Python retries terminal reads interrupted by SIGTTIN/SIGCONT;
            # macOS /bin/cat may report EINTR after resumption instead.
            reader = f"{sys.executable} -c 'print(input())' &\n"
            session.send(reader.encode())
            session.expect(rb"\[1\] Stopped.*\$ ")
            session.send(b"fg %1\n")
            session.expect(rb"fg %1\r?\n.*print\(input\(\)\).*\r?\n")
            session.send(b"terminal-reader\n")
            session.expect(rb"terminal-reader\r?\nterminal-reader\r?\n")
            session.send(b"\x04")
            session.expect(rb"\$ ")
        finally:
            session.close()

    def test_stopped_job_terminal_modes_are_restored_on_fg(self) -> None:
        session = PtySession()
        try:
            session.expect(rb"\$ ")
            program = (
                "import os,signal,termios; t=termios.tcgetattr(0); "
                "t[3] &= ~termios.ECHO; termios.tcsetattr(0,termios.TCSANOW,t); "
                "os.kill(os.getpid(),signal.SIGTSTP); "
                "assert not termios.tcgetattr(0)[3] & termios.ECHO; "
                'print("modes-preserved")'
            )
            session.send(f"{sys.executable} -c '{program}'\n".encode())
            session.expect(rb"\[1\] Stopped.*\$ ")
            session.send(b"fg %1\n")
            session.expect(rb"\r?\nmodes-preserved\r?\n.*\$ ")
        finally:
            session.close()

    def test_shell_started_in_background_waits_for_terminal(self) -> None:
        session = PtySession()
        try:
            session.expect(rb"\$ ")
            session.send(f"{BINARY} --state {NESTED} &\n".encode())
            session.expect(rb"\[1\] Stopped.*\$ ")
            session.send(b"fg %1\n")
            session.expect(rb"Username: ")
            session.send(b"admin\n")
            session.expect(rb"Password: ")
            session.send(b"AdminTest123!\n")
            session.expect(rb"Authenticated.*os-shell:.*\$ ")
            session.send(b"echo nested-shell-active\n")
            session.expect(rb"\r?\nnested-shell-active\r?\n.*\$ ")
            session.send(b"exit 0\n")
            session.expect(rb"exit 0\r?\n.*\$ ")
            self.assertEqual(os.tcgetpgrp(session.fd), session.pid)
        finally:
            session.close()

    def test_exit_sends_hangup_to_running_job(self) -> None:
        session = PtySession()
        try:
            session.expect(rb"\$ ")
            session.send(b"sleep 30 &\n")
            transcript = session.expect(rb"\[1\] (\d+)\r?\n.*\$ ")
            pid = int(re.search(rb"\[1\] (\d+)", transcript).group(1))
            session.send(b"exit 0\n")
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.02)
            else:
                self.fail("Background job survived shell shutdown")
        finally:
            session.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)

