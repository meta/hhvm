#!/usr/bin/env python3
# pyre-strict
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the "hack" directory of this source tree.

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from hphp.hack.src.hh_asdiff.hh_asdiff import parse_args


class InputFilesTest(unittest.TestCase):
    def test_inputs_are_opened_read_only(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "input.hhas"
            path.write_bytes(b"# test.php starts here\nInt 1\n# test.php ends here\n")
            opts = parse_args(["hh_asdiff", str(path), str(path)])
            with opts.expected_file, opts.actual_file:
                self.assertFalse(opts.expected_file.writable())
                self.assertFalse(opts.actual_file.writable())
                self.assertIsInstance(opts.expected_file.read(), bytes)
                self.assertIsInstance(opts.actual_file.read(), bytes)

    def test_compares_read_only_inputs(self) -> None:
        content = b"# test.php starts here\nInt 1\n# test.php ends here\n"
        with tempfile.TemporaryDirectory() as directory:
            expected = Path(directory) / "expected.hhas"
            actual = Path(directory) / "actual.hhas"
            expected.write_bytes(content)
            actual.write_bytes(content)
            for read_only in (expected, actual):
                read_only.chmod(0o444)
                try:
                    for option in ("--no-diff", "--report-summary"):
                        with self.subTest(input=read_only.name, option=option):
                            result = subprocess.run(
                                [
                                    sys.executable,
                                    "-m",
                                    "hphp.hack.src.hh_asdiff.hh_asdiff",
                                    option,
                                    str(expected),
                                    str(actual),
                                ],
                                cwd=Path(__file__).resolve().parents[4],
                                capture_output=True,
                                text=True,
                            )
                            self.assertEqual(result.returncode, 0, result.stderr)
                            if option == "--report-summary":
                                self.assertIn("|Identical| test.php", result.stdout)
                            else:
                                self.assertIn("files checked: 1", result.stdout)
                                self.assertIn("diffs handled: 0", result.stdout)
                            self.assertEqual(expected.read_bytes(), content)
                            self.assertEqual(actual.read_bytes(), content)
                finally:
                    read_only.chmod(0o644)
