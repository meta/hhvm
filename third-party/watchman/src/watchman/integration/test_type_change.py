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
class TestTypeChange(WatchmanTestCase.WatchmanTestCase):
    # `entry` is in `sub` and not in the root: how a watcher treats the entries
    # of the root is not what these tests are about.

    def watchAndGetClock(self, root: str, files: list[str]) -> str:
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files)
        return self.watchmanCommand("clock", root)["clock"]

    def changesSince(self, root: str, clock: str) -> dict[str, str]:
        """How watchman reports each file in `sub` that changed since `clock`."""

        def change(file: dict[str, bool]) -> str:
            if not file["exists"]:
                return "deleted"
            return "new" if file["new"] else "changed"

        result = self.watchmanCommand(
            "query",
            root,
            {
                "since": clock,
                "expression": ["dirname", "sub"],
                "fields": ["name", "exists", "new"],
            },
        )
        # Every file is new in a fresh instance.
        self.assertFalse(result["is_fresh_instance"])
        return {
            norm_relative_path(file["name"]): change(file) for file in result["files"]
        }

    def test_file_replaced_by_dir_is_new(self) -> None:
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, "sub"))
        self.touchRelative(root, "sub", "entry")
        clock = self.watchAndGetClock(root, ["sub", "sub/entry"])

        # Watchman cannot see the path without the file or the directory.
        with self.watchmanSuspended():
            os.unlink(os.path.join(root, "sub", "entry"))
            os.mkdir(os.path.join(root, "sub", "entry"))
            self.touchRelative(root, "sub", "entry", "inside")

        self.assertWaitForEqual(
            {"sub/entry": "new", "sub/entry/inside": "new"},
            lambda: self.changesSince(root, clock),
        )

    def test_dir_replaced_by_file_is_new(self) -> None:
        root = self.mkdtemp()
        os.makedirs(os.path.join(root, "sub", "entry"))
        self.touchRelative(root, "sub", "entry", "inside")
        clock = self.watchAndGetClock(root, ["sub", "sub/entry", "sub/entry/inside"])

        with self.watchmanSuspended():
            shutil.rmtree(os.path.join(root, "sub", "entry"))
            self.touchRelative(root, "sub", "entry")

        self.assertWaitForEqual(
            {"sub/entry": "new", "sub/entry/inside": "deleted"},
            lambda: self.changesSince(root, clock),
        )
