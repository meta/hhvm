# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import socket

import pywatchman
from watchman.integration.lib import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestCookie(WatchmanTestCase.WatchmanTestCase):
    def test_delete_cookie_dir(self) -> None:
        root = self.mkdtemp()
        cookie_dir = os.path.join(root, ".hg")
        os.mkdir(cookie_dir)
        self.touchRelative(root, "foo")

        self.watchmanCommand("watch-project", root)
        self.assertFileList(root, files=["foo", ".hg"])

        os.rmdir(cookie_dir)
        self.assertFileList(root, files=["foo"])
        os.unlink(os.path.join(root, "foo"))
        self.assertFileList(root, files=[])
        os.rmdir(root)
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            result = self.assertFileList(root, files=[])
            print("Should not have gotten here, but the result was:", result)

        reason = str(ctx.exception)
        self.assertTrue(
            ("No such file" in reason)
            or ("root dir was removed" in reason)
            or ("The system cannot find the file specified" in reason)
            or ("unable to resolve root" in reason),
            msg=reason,
        )

    def test_cookie_in_new_directory(self) -> None:
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, ".git"))
        self.watchmanCommand("watch", root)
        expected = [".git"]
        self.assertFileList(root, files=expected)
        host = socket.gethostname()

        for attempt in range(32):
            directory = f"dir{attempt}"
            os.mkdir(os.path.join(root, directory))
            cookie = f"{directory}/.watchman-cookie-{host}-1-100000"
            self.touchRelative(root, cookie)
            expected.extend([directory, cookie])
            self.assertFileList(root, files=expected)

    def test_other_cookies(self) -> None:
        root = self.mkdtemp()
        cookie_dir = os.path.join(root, ".git")
        os.mkdir(cookie_dir)
        watch = self.watchmanCommand("watch", root)

        host = socket.gethostname()
        pid = self.watchmanCommand("get-pid")["pid"]

        self.assertFileList(root, files=[".git"])
        os.mkdir(os.path.join(root, "foo"))

        # Same process, same watch
        self.touchRelative(root, ".git/.watchman-cookie-%s-%d-1000000" % (host, pid))

        cookies = [
            # Different process, same watch root
            ".git/.watchman-cookie-%s-1-100000" % host,
            # Different process, root dir instead of VCS dir
            ".watchman-cookie-%s-1-100000" % host,
            # Different process, different watch root
            "foo/.watchman-cookie-%s-1-100000" % host,
        ]

        if watch["watcher"] != "kqueue+fsevents":
            # With the split watch, a cookie is written in all top-level
            # directories and at the root, therefore the following 2 cookies
            # are expected to not be present in watchman's queries output as
            # they are genuine cookies.

            # Same process, different watch root
            cookies.append("foo/.watchman-cookie-%s-%d-100000" % (host, pid))
            # Same process, root dir instead of VCS dir
            cookies.append(".watchman-cookie-%s-%d-100000" % (host, pid))

        for cookie in cookies:
            self.touchRelative(root, cookie)

        self.assertFileList(root, files=["foo", ".git"] + cookies)
