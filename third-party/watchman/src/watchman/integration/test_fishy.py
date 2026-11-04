# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import os
import subprocess

from watchman.integration.lib import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestFishy(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self) -> None:
        if os.name == "nt":
            self.skipTest("non admin symlinks and unix userland not available")

    def test_fishy(self) -> None:
        root = self.mkdtemp()

        os.mkdir(os.path.join(root, "foo"))
        self.touchRelative(root, "foo", "a")

        self.watchmanCommand("watch", root)
        base = self.watchmanCommand("find", root, ".")
        clock = base["clock"]

        self.suspendWatchman()
        # Explicitly using the shell to run these commands
        # as the original test case wasn't able to reproduce
        # the problem with whatever sequence and timing of
        # operations was produced by the original php test
        subprocess.check_call(
            "mv foo bar && ln -s bar foo",
            shell=True,
            cwd=root,
        )

        self.resumeWatchman()
        self.assertFileList(root, files=["bar", "bar/a", "foo"], cursor=clock)

    def test_more_moves(self) -> None:
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        base = self.watchmanCommand("find", root, ".")
        clock = base["clock"]

        self.suspendWatchman()
        # Explicitly using the shell to run these commands
        # as the original test case wasn't able to reproduce
        # the problem with whatever sequence and timing of
        # operations was produced by the original php test
        subprocess.check_call(
            "touch a && mkdir d1 d2 && mv d1 d2 && mv d2/d1 . && mv a d1",
            shell=True,
            cwd=root,
        )
        self.resumeWatchman()
        self.assertFileList(root, files=["d1", "d1/a", "d2"], cursor=clock)

    def test_even_more_moves(self) -> None:
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        base = self.watchmanCommand("find", root, ".")
        clock = base["clock"]

        self.suspendWatchman()
        # Explicitly using the shell to run these commands
        # as the original test case wasn't able to reproduce
        # the problem with whatever sequence and timing of
        # operations was produced by the original php test
        subprocess.check_call(
            (
                "mkdir d1 d2 && "
                "touch d1/a && "
                "mkdir d3 && "
                "mv d1 d2 d3 && "
                "mv d3/* . && "
                "mv d1 d2 d3 && "
                "mv d3/* . && "
                "mv d1/a d2"
            ),
            shell=True,
            cwd=root,
        )
        self.resumeWatchman()
        self.assertFileList(root, files=["d1", "d2", "d2/a", "d3"], cursor=clock)

    def test_notify_dir(self) -> None:
        root = self.mkdtemp()
        watch = self.watchmanCommand("watch", root)
        os.mkdir(os.path.join(root, "wtest"))
        os.mkdir(os.path.join(root, "wtest", "dir"))
        self.touchRelative(root, "wtest", "1")
        self.touchRelative(root, "wtest", "2")
        self.assertFileList(
            root, ["wtest", "wtest/1", "wtest/2", "wtest/dir"], cursor="n:foo"
        )

        def assertChangedSinceFoo(expected: list[str]) -> None:
            files = self.watchmanCommand(
                "query", root, {"fields": ["name"], "since": "n:foo"}
            )["files"]
            if watch["watcher"] == "kqueue+fsevents":
                # A sync can end before FSEvents has reported every earlier
                # change in wtest, so files that changed before n:foo can be
                # reported again.
                self.assertFileListContains(files, expected)
            else:
                self.assertFileListsEqual(files, expected)

        os.rmdir(os.path.join(root, "wtest/dir"))
        assertChangedSinceFoo(["wtest", "wtest/dir"])

        os.unlink(os.path.join(root, "wtest/2"))
        assertChangedSinceFoo(["wtest", "wtest/2"])

        os.unlink(os.path.join(root, "wtest/1"))
        assertChangedSinceFoo(["wtest", "wtest/1"])
