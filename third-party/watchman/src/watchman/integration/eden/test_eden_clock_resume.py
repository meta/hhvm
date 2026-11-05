# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import json

from watchman.integration.lib import WatchmanEdenTestCase


def populate(repo) -> None:
    repo.write_file(".watchmanconfig", json.dumps({"ignore_dirs": [".hg"]}))
    repo.write_file("hello", "hola\n")
    repo.write_file("adir/file", "foo!\n")
    repo.commit("initial commit.")


# pyrefly: ignore [invalid-inheritance]
class TestEdenClockResume(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def restartWatchman(self, root) -> None:
        self.eden_watchman.stop()
        self.eden_watchman.start()
        self.client = self.getClient(self.eden_watchman, replace_cached=True)
        self.watchmanCommand("watch", root)

    def querySince(self, root, clock):
        return self.watchmanCommand(
            "query",
            root,
            {"expression": ["true"], "fields": ["name", "exists"], "since": clock},
        )

    def test_clock_from_previous_watchman_is_fresh_instance(self) -> None:
        root = self.makeEdenMount(populate)
        self.watchmanCommand("watch", root)
        clock = self.watchmanCommand("clock", root)["clock"]

        self.restartWatchman(root)
        self.touchRelative(root, "added")

        res = self.querySince(root, clock)
        # FIXME: EdenFS still has the journal this clock points into, so
        # watchman could resume from it instead of listing every file.
        self.assertTrue(res["is_fresh_instance"])
