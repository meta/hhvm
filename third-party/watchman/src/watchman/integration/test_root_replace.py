# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import sys

import pywatchman
from watchman.integration.lib import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestRootReplace(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self) -> None:
        if not sys.platform.startswith("linux"):
            self.skipTest("the test needs inotify")

    def queryFiles(self, root: str, sync_timeout: int) -> list[str]:
        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["exists"],
                "fields": ["name"],
                "sync_timeout": sync_timeout,
            },
        )
        return res["files"]

    def querySynced(self, root: str) -> list[str]:
        return self.queryFiles(root, sync_timeout=2000)

    def assertSyncTimesOut(self, root: str) -> None:
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.querySynced(root)
        self.assertIn("timed out waiting for cookie", str(ctx.exception))

    def waitForInitialCrawl(self, root: str) -> None:
        # Do not use a sync query. Watchman deletes a sync cookie after it sees the
        # cookie. When the root is the cookie directory, the deletion starts a scan of
        # the root. The scan can occur while the root path does not exist.
        def doneInitial() -> bool:
            res = self.watchmanCommand("debug-root-status", root)
            return res["root_status"]["done_initial"]

        self.assertWaitFor(doneInitial)

    def replaceRoot(self, root: str) -> None:
        # The open descriptor holds a reference to the old root directory.
        # While this reference exists, the kernel does not send
        # IN_DELETE_SELF for the old root.
        old_root_fd = os.open(root, os.O_RDONLY)
        self.addCleanup(os.close, old_root_fd)
        os.rmdir(root)
        os.mkdir(root)

    def test_query_after_hg_root_is_replaced(self) -> None:
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, ".hg"))
        self.watchmanCommand("watch", root)
        self.waitForInitialCrawl(root)
        self.assertFileListsEqual(self.queryFiles(root, sync_timeout=0), [".hg"])

        # Suspend Watchman during the replacement. If Watchman scans the root
        # while the root path does not exist, Watchman cancels the watch.
        self.suspendWatchman()
        os.rmdir(os.path.join(root, ".hg"))
        self.replaceRoot(root)
        os.mkdir(os.path.join(root, ".hg"))
        self.touchRelative(root, "foo")
        self.resumeWatchman()

        self.assertFileListsEqual(self.querySynced(root), [".hg", "foo"])

    def test_query_after_root_is_replaced(self) -> None:
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        self.waitForInitialCrawl(root)
        self.assertFileListsEqual(self.queryFiles(root, sync_timeout=0), [])

        self.replaceRoot(root)
        self.touchRelative(root, "foo")

        # Watchman does not watch the new root yet. This sync query times out.
        # The timeout makes Watchman recrawl the root.
        self.assertSyncTimesOut(root)

        self.assertFileListsEqual(self.querySynced(root), ["foo"])
