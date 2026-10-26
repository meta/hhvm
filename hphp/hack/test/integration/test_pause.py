# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the "hack" directory of this source tree.

from __future__ import annotations

import time
from pathlib import Path

from hphp.hack.test.integration.eden_test_driver import EdenTestDriver
from hphp.hack.test.integration.test_case import TestCase


class PauseTestDriver(EdenTestDriver):
    def tearDown(self) -> None:
        """Stop the daemon before the shared driver removes its Eden mount."""
        self.stop_hh_server()
        super().tearDown()


class TestPause(TestCase[PauseTestDriver]):
    @classmethod
    def get_test_driver(cls) -> PauseTestDriver:
        return PauseTestDriver()

    def start_server(self, *, streaming: bool = False) -> None:
        """Exercise pause with Eden notifications and RPC or streaming errors."""
        root = Path(self.test_driver.repo_dir)
        with (root / "hh.conf").open("a") as config:
            config.write(
                "\nmin_log_level = Debug\n"
                "watchman_enabled = false\n"
                "edenfs_file_watcher_enabled = true\n"
                f"produce_streaming_errors = {str(streaming).lower()}\n"
                f"consume_streaming_errors = {str(streaming).lower()}\n"
            )
        self.test_driver.start_hh_server()
        log_name, errors, code = self.test_driver.run_check(options=["--logname"])
        self.assertEqual(0, code, log_name + errors)
        # Eden initialization failures can otherwise silently fall back to dfind.
        self.assertIn(
            "[edenfs_watcher][init] finished init",
            Path(log_name.strip()).read_text(),
        )

    def pause(self) -> None:
        """Wait for the server to acknowledge that checking has paused."""
        output, errors, code = self.test_driver.run_check(options=["--pause"])
        self.assertEqual(0, code, output + errors)
        self.assertIn("Paused automatic rechecks.", output)

    def wait_for_log(self, path: Path, offset: int, message: str) -> str:
        """Wait for a server event, failing if it does not arrive within the deadline."""
        deadline = time.monotonic() + 60.0
        while True:
            log = path.read_text()[offset:]
            if message in log:
                return log
            self.assertLess(
                time.monotonic(), deadline, f"No {message!r} in server log:\n{log}"
            )
            time.sleep(0.1)

    def test_changes_wait_for_next_command(self) -> None:
        """Paused edits wait; full and priority RPCs resume with fresh diagnostics."""
        self.start_server()
        log_name, errors, code = self.test_driver.run_check(options=["--logname"])
        self.assertEqual(0, code, log_name + errors)
        server_log = Path(log_name.strip())
        changed_file = Path(self.test_driver.repo_dir) / "pause_error.php"
        for options in ([], ["--single", "{root}pause_error.php"]):
            with self.subTest(options=options):
                self.pause()
                log_before = server_log.read_text()
                rechecks = log_before.count("RECHECK_END (recheck_id ")
                changed_file.write_text(
                    "<?hh\nfunction pause_error(): int { return 'wrong'; }\n"
                )

                # Wait until the server has queued the edit and chosen to stay paused.
                paused_log = self.wait_for_log(
                    server_log, len(log_before), "Queued file changes while paused:"
                )
                self.assertNotIn("Begin typechecking ", paused_log)
                output, errors, code = self.test_driver.run_check(options=options)
                self.assertEqual(2, code, output + errors)
                self.assertIn("pause_error.php", output)
                self.assertIn("4110", output)
                self.assertGreater(
                    server_log.read_text().count("RECHECK_END (recheck_id "), rechecks
                )
                log_offset = len(server_log.read_text())
                changed_file.write_text(
                    "<?hh\nfunction pause_error(): int { return 1; }\n"
                )
                # Observe the automatic recheck BEFORE sending another client request.
                self.wait_for_log(server_log, log_offset, "RECHECK_END (recheck_id ")
                output, errors, code = self.test_driver.run_check()
                self.assertEqual(0, code, output + errors)

    def test_status_without_edits(self) -> None:
        """Repeated pause and status without edits do not start another typecheck."""
        self.start_server()
        log_name, errors, code = self.test_driver.run_check(options=["--logname"])
        self.assertEqual(0, code, log_name + errors)
        server_log = Path(log_name.strip())
        log_offset = len(server_log.read_text())
        self.pause()
        self.pause()
        output, errors, code = self.test_driver.run_check()
        self.assertEqual(0, code, output + errors)
        # RECHECK_END omits empty checks; this also catches unnecessary empty work.
        self.assertNotIn("Begin typechecking ", server_log.read_text()[log_offset:])

    def test_streaming_timeout_then_stats_resumes(self) -> None:
        """Streaming status times out on paused edits; stats resumes fresh diagnostics."""
        self.start_server(streaming=True)
        log_name, errors, code = self.test_driver.run_check(options=["--logname"])
        self.assertEqual(0, code, log_name + errors)
        server_log = Path(log_name.strip())
        self.pause()
        log_offset = len(server_log.read_text())
        changed_file = Path(self.test_driver.repo_dir) / "pause_error.php"
        changed_file.write_text(
            "<?hh\nfunction pause_error(): int { return 'wrong'; }\n"
        )
        self.wait_for_log(server_log, log_offset, "Queued file changes while paused:")

        # Streaming status cannot refresh the cached errors while the server is paused.
        output, errors, code = self.test_driver.run_check(options=["--timeout", "15"])
        self.assertEqual(7, code, output + errors)  # Exit_status.Out_of_time
        self.assertNotIn("Begin typechecking ", server_log.read_text()[log_offset:])

        # Stats must resume checking before another streaming status request is sent.
        log_offset = len(server_log.read_text())
        output, errors, code = self.test_driver.run_check(options=["--stats"])
        self.assertEqual(0, code, output + errors)
        self.wait_for_log(server_log, log_offset, "RECHECK_END (recheck_id ")

        output, errors, code = self.test_driver.run_check()
        self.assertEqual(2, code, output + errors)
        self.assertIn("pause_error.php", output)
        self.assertIn("4110", output)
