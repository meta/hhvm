# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import os
import subprocess
import sys

from watchman.integration.lib import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestWatchmanWait(WatchmanTestCase.WatchmanTestCase):
    def requiresPersistentSession(self) -> bool:
        return True

    def spawnWatchmanWait(self, cmdArgs):
        wait_script = os.environ.get("WATCHMAN_WAIT_PATH")
        if wait_script:
            # Windows can't execute a .par directly.
            args = [sys.executable, wait_script] if os.name == "nt" else [wait_script]
        else:
            args = [
                sys.executable,
                os.path.join(os.environ["WATCHMAN_PYTHON_BIN"], "watchman-wait"),
            ]
        args.extend(cmdArgs)

        env = os.environ.copy()
        sock_path = self.watchmanInstance().getSockPath()
        env["WATCHMAN_SOCK"] = sock_path.legacy_sockpath()
        pywatchman_path = env.get("PYWATCHMAN_PATH")
        if pywatchman_path:
            env["PYTHONPATH"] = pywatchman_path
        return subprocess.Popen(
            args, env=env, stdin=None, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )

    def assertWaitedFileList(self, stdout, expected) -> None:
        stdout = stdout.decode("utf-8").rstrip()
        files = [f.rstrip() for f in stdout.split("\n")]
        self.assertFileListContains(files, expected)

    def assertWaitForWmWaitWatch(self, root) -> None:
        """Wait for the specified root to appear in the watch list;
        watchman-wait will initiate that asynchronously and we have
        to wait for that before proceeding.
        Then wait for the watch to be ready to query, otherwise the
        test expectations will not be reliably met."""

        # wait for the watch to appear
        self.assertWaitFor(
            lambda: self.rootIsWatched(root),
            message="%s was not watched by watchman-wait" % root,
        )

        # now wait for it to be ready to query.  The easiest way
        # to do this is to ask for the watch ourselves, as that
        # will block us until it is ready
        self.watchmanCommand("watch", root)

    def test_wait(self) -> None:
        root = self.mkdtemp()
        self.touchRelative(root, "foo")
        a_dir = os.path.join(root, "a")
        os.mkdir(a_dir)
        self.touchRelative(a_dir, "foo")

        wmwait = self.spawnWatchmanWait(
            ["--relative", root, "--max-events", "8", "-t", "3", root]
        )
        self.assertWaitForWmWaitWatch(root)

        self.touchRelative(root, "bar")
        self.removeRelative(root, "foo")
        self.touchRelative(a_dir, "bar")
        self.removeRelative(a_dir, "foo")

        b_dir = os.path.join(root, "b")
        os.mkdir(b_dir)
        self.touchRelative(b_dir, "foo")

        (stdout, stderr) = wmwait.communicate()
        self.assertWaitedFileList(stdout, ["a/bar", "a/foo", "b/foo", "bar", "foo"])

    def test_rel_root(self) -> None:
        root = self.mkdtemp()

        a_dir = os.path.join(root, "a")
        os.mkdir(a_dir)
        b_dir = os.path.join(root, "b")
        os.mkdir(b_dir)

        wmwait = self.spawnWatchmanWait(
            ["--relative", b_dir, "--max-events", "8", "-t", "6", a_dir, b_dir]
        )

        self.assertWaitForWmWaitWatch(b_dir)
        self.assertWaitForWmWaitWatch(a_dir)

        self.touchRelative(a_dir, "afoo")
        self.touchRelative(b_dir, "bfoo")

        a_sub_dir = os.path.join(a_dir, "asub")
        os.mkdir(a_sub_dir)
        b_sub_dir = os.path.join(b_dir, "bsub")
        os.mkdir(b_sub_dir)

        (stdout, stderr) = wmwait.communicate()
        self.assertWaitedFileList(stdout, ["../a/afoo", "../a/asub", "bfoo", "bsub"])
