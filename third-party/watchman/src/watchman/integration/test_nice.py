# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import os
import sys
import unittest

from watchman.integration.lib import WatchmanInstance


@unittest.skipIf(os.name == "nt", "N/A on windows")
class TestNice(unittest.TestCase):
    @unittest.skipIf(
        sys.platform == "darwin", "launchd renders this test invalid on macOS"
    )
    def test_failing_to_start_when_nice(self) -> None:
        inst = WatchmanInstance.Instance()
        stdout, stderr = inst.commandViaCLI(["version"], prefix=["nice"])
        print("stdout", stdout.decode(errors="replace"))
        print("stderr", stderr.decode(errors="replace"))
        stderr = stderr.decode("ascii")
        self.assertEqual(b"", stdout)
        self.assertRegex(stderr, "refusing to start")

    def test_failing_to_start_when_nice_foreground(self) -> None:
        # `nice` adds 10, and watchman only refuses to start above 0.
        if os.nice(0) + 10 <= 0:
            self.skipTest(
                f"nice can't lower watchman below the default from {os.nice(0)}"
            )
        inst = WatchmanInstance.Instance()
        stdout, stderr = inst.commandViaCLI(
            ["--foreground", "version"], prefix=["nice"]
        )
        print("stdout", stdout)
        print("stderr", stderr)

        output = stderr.decode("ascii")
        try:
            output += inst.getServerLogContents()
        except IOError:
            # on macos, we may not have gotten as far
            # as creating the log file when the error
            # triggers, so we're fine with passing
            # on io errors here
            pass
        self.assertRegex(output, "refusing to start")
