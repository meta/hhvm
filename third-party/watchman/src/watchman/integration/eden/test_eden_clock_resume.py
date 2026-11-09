# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import json

from watchman.integration.lib import WatchmanEdenTestCase


def populate(repo, resume: bool = False) -> None:
    config: dict[str, object] = {"ignore_dirs": [".hg"]}
    if resume:
        config["eden_resume_other_process_clocks"] = True
    repo.write_file(".watchmanconfig", json.dumps(config))
    repo.write_file("hello", "hola\n")
    repo.write_file("adir/file", "foo!\n")
    repo.commit("initial commit.")


# pyrefly: ignore [invalid-inheritance]
class TestEdenClockResume(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def startWatchman(self, root) -> None:
        self.eden_watchman.start()
        self.client = self.getClient(self.eden_watchman, replace_cached=True)
        self.watchmanCommand("watch", root)

    def restartWatchman(self, root) -> None:
        self.eden_watchman.stop()
        self.startWatchman(root)

    def querySince(self, root, clock):
        return self.watchmanCommand(
            "query",
            root,
            {"expression": ["true"], "fields": ["name", "exists"], "since": clock},
        )

    def test_clock_from_previous_watchman_is_fresh_instance_by_default(
        self,
    ) -> None:
        root = self.makeEdenMount(populate)
        self.watchmanCommand("watch", root)
        clock = self.watchmanCommand("clock", root)["clock"]

        self.restartWatchman(root)
        self.touchRelative(root, "added")

        res = self.querySince(root, clock)
        self.assertTrue(res["is_fresh_instance"])

    def test_resumes_clock_from_previous_watchman(self) -> None:
        root = self.makeEdenMount(lambda repo: populate(repo, resume=True))
        self.watchmanCommand("watch", root)
        clock = self.watchmanCommand("clock", root)["clock"]

        self.eden_watchman.stop()
        self.touchRelative(root, "added")
        self.removeRelative(root, "hello")
        self.startWatchman(root)

        res = self.querySince(root, clock)
        self.assertFalse(res["is_fresh_instance"])
        self.assertEqual(
            {f["name"]: f["exists"] for f in res["files"]},
            {"added": True, "hello": False},
        )

    def test_subscription_resumes_clock_from_previous_watchman(self) -> None:
        root = self.makeEdenMount(lambda repo: populate(repo, resume=True))
        self.watchmanCommand("watch", root)
        clock = self.watchmanCommand("clock", root)["clock"]

        self.eden_watchman.stop()
        self.touchRelative(root, "added")
        self.startWatchman(root)

        self.watchmanCommand(
            "subscribe",
            root,
            "resumed",
            {"expression": ["true"], "fields": ["name"], "since": clock},
        )
        dat = self.waitForSub("resumed", root=root)[0]
        self.assertFalse(dat["is_fresh_instance"])
        self.assertFileListsEqual(dat["files"], ["added"])

    def test_clock_from_before_edenfs_restart_is_fresh_instance(self) -> None:
        root = self.makeEdenMount(lambda repo: populate(repo, resume=True))
        self.watchmanCommand("watch", root)
        clock = self.watchmanCommand("clock", root)["clock"]

        self.eden.restart()
        self.restartWatchman(root)
        # A clock is c:<start time>:<pid>:<mount generation>:<journal position>.
        # The restarted mount's journal starts over, so move it past the old
        # position to leave the generation as the only difference.
        for i in range(int(clock.split(":")[4])):
            self.touchRelative(root, f"file{i}")

        res = self.querySince(root, clock)
        self.assertTrue(res["is_fresh_instance"])
