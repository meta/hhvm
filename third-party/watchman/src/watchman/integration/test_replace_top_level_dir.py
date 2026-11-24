# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import os
import shutil

from watchman.integration.lib import WatchmanTestCase
from watchman.integration.lib.path_utils import norm_relative_path


@WatchmanTestCase.expand_matrix
class TestReplaceTopLevelDir(WatchmanTestCase.WatchmanTestCase):
    def watchAndGetClock(self, root: str, files: list[str]) -> str:
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files)
        return self.watchmanCommand("clock", root)["clock"]

    def changesSince(self, root: str, clock: str) -> dict[str, str | None]:
        """Maps each file that changed since `clock` to its type, or to None if
        the file is gone."""
        result = self.watchmanCommand(
            "query", root, {"since": clock, "fields": ["name", "exists", "type"]}
        )
        # A fresh instance reports every file, changed or not.
        self.assertFalse(result["is_fresh_instance"])
        return {
            norm_relative_path(file["name"]): file["type"] if file["exists"] else None
            for file in result["files"]
        }

    def test_replaced_by_file(self) -> None:
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, "entry"))
        self.touchRelative(root, "entry", "inside")
        clock = self.watchAndGetClock(root, ["entry", "entry/inside"])

        # Watchman cannot see the root without `entry`.
        with self.watchmanSuspended():
            shutil.rmtree(os.path.join(root, "entry"))
            self.touchRelative(root, "entry")

        self.assertWaitForEqual(
            {"entry": "f", "entry/inside": None},
            lambda: self.changesSince(root, clock),
        )

    def test_replaced_by_dir(self) -> None:
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, "entry"))
        self.touchRelative(root, "entry", "before")
        os.mkdir(os.path.join(root, "incoming"))
        self.touchRelative(root, "incoming", "after")
        clock = self.watchAndGetClock(
            root, ["entry", "entry/before", "incoming", "incoming/after"]
        )

        with self.watchmanSuspended():
            os.rename(os.path.join(root, "entry"), os.path.join(root, "old"))
            os.rename(os.path.join(root, "incoming"), os.path.join(root, "entry"))

        self.assertWaitForEqual(
            {
                "entry": "d",
                "entry/after": "f",
                "entry/before": None,
                "incoming": None,
                "incoming/after": None,
                "old": "d",
                "old/before": "f",
            },
            lambda: self.changesSince(root, clock),
        )

        # The directory that is at `entry` now is watched.
        self.touchRelative(root, "entry", "later")
        self.assertFileList(
            root, ["entry", "entry/after", "entry/later", "old", "old/before"]
        )
