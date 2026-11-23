# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the "hack" directory of this source tree.

from __future__ import annotations

import errno
import os
import pty
import subprocess
from contextlib import ExitStack
from pathlib import Path

from hphp.hack.test.integration import common_tests, hh_paths, test_case


class StreamingDiagnosticsTest(test_case.TestCase[common_tests.CommonTestDriver]):
    @classmethod
    def get_test_driver(cls) -> common_tests.CommonTestDriver:
        return common_tests.CommonTestDriver()

    def setUp(self) -> None:
        super().setUp()
        Path(self.test_driver.repo_dir, "hh.conf").write_text(
            "use_mini_state=false\nmax_workers=2\n"
        )

    def run_client(
        self,
        args: list[str],
        *,
        stdout_tty: bool = False,
        stderr_tty: bool = False,
        env: dict[str, str] | None = None,
    ) -> tuple[str, str, int]:
        with ExitStack() as stack:
            terminals = {}
            for name, enabled in (("stdout", stdout_tty), ("stderr", stderr_tty)):
                if enabled:
                    master, slave = pty.openpty()
                    stack.callback(os.close, master)
                    stack.callback(os.close, slave)
                    terminals[name] = (master, slave)
            result = subprocess.run(
                [hh_paths.hh_client, *args, self.test_driver.repo_dir],
                stdin=subprocess.DEVNULL,
                stdout=terminals["stdout"][1] if stdout_tty else subprocess.PIPE,
                stderr=terminals["stderr"][1] if stderr_tty else subprocess.PIPE,
                env=dict(self.test_driver.test_env, **(env or {})),
                timeout=60,
            )
            outputs = {"stdout": result.stdout, "stderr": result.stderr}
            for name, (master, _slave) in terminals.items():
                os.set_blocking(master, False)
                chunks = []
                while True:
                    try:
                        chunk = os.read(master, 65536)
                        if not chunk:
                            break
                        chunks.append(chunk)
                    except BlockingIOError:
                        break
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                        break
                outputs[name] = b"".join(chunks)
            return (
                outputs["stdout"].decode(),
                outputs["stderr"].decode(),
                result.returncode,
            )

    def test_force_streaming_flag_keeps_existing_behavior(self) -> None:
        self.test_driver.start_hh_server()
        for args in (
            ["--error-format", "plain", "--config", "consume_streaming_errors=false"],
            ["--identify", "B", "--json"],
        ):
            with self.subTest(args=args):
                control = self.run_client(args)
                forced = self.run_client(["--force-streaming-diagnostics", *args])
                self.assertEqual(control, forced)
                self.assertEqual(0, forced[2], forced)
