# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import os
import sys

import pywatchman
import pywatchman.capabilities
from watchman.integration.lib import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestCapabilities(WatchmanTestCase.WatchmanTestCase):
    def test_capabilities(self) -> None:
        client = self.getClient()
        res = client.query("version")
        self.assertFalse("error" in res, "version with no args still works")

        res = client.query("version", {"optional": ["term-match", "will-never-exist"]})
        self.assertDictEqual(
            res["capabilities"], {"term-match": True, "will-never-exist": False}
        )

        res = client.query(
            "version", {"required": ["term-match"], "optional": ["will-never-exist"]}
        )
        self.assertDictEqual(
            res["capabilities"], {"term-match": True, "will-never-exist": False}
        )
        self.assertFalse("error" in res, "no error for missing optional")

        with self.assertRaisesRegex(
            pywatchman.CommandError,
            "client required capabilities \\[will-never-exist\\] not "
            + "supported by this server",
        ):
            client.query("version", {"required": ["term-match", "will-never-exist"]})

    def test_capabilityCheck(self) -> None:
        client = self.getClient()

        res = client.capabilityCheck(optional=["term-match", "will-never-exist"])
        self.assertDictEqual(
            res["capabilities"], {"term-match": True, "will-never-exist": False}
        )

        res = client.capabilityCheck(
            required=["term-match"], optional=["will-never-exist"]
        )
        self.assertDictEqual(
            res["capabilities"], {"term-match": True, "will-never-exist": False}
        )

        with self.assertRaisesRegex(
            pywatchman.CommandError,
            "client required capabilities \\[will-never-exist\\] not "
            + "supported by this server",
        ):
            client.capabilityCheck(required=["term-match", "will-never-exist"])

    def test_capabilitySynth(self) -> None:
        res = pywatchman.capabilities.synthesize(
            {"version": "1.0"}, {"optional": ["will-never-exist"], "required": []}
        )
        self.assertDictEqual(
            res, {"version": "1.0", "capabilities": {"will-never-exist": False}}
        )

        res = pywatchman.capabilities.synthesize(
            {"version": "1.0"}, {"required": ["will-never-exist"], "optional": []}
        )
        self.assertDictEqual(
            res,
            {
                "version": "1.0",
                "error": "client required capabilities [will-never-exist] "
                + "not supported by this server",
                "capabilities": {"will-never-exist": False},
            },
        )

        res = pywatchman.capabilities.synthesize(
            {"version": "3.2"}, {"optional": ["relative_root"], "required": []}
        )
        self.assertDictEqual(
            res, {"version": "3.2", "capabilities": {"relative_root": False}}
        )
        res = pywatchman.capabilities.synthesize(
            {"version": "3.3"}, {"optional": ["relative_root"], "required": []}
        )
        self.assertDictEqual(
            res, {"version": "3.3", "capabilities": {"relative_root": True}}
        )

    def test_full_capability_set(self) -> None:
        client = self.getClient()
        res = client.listCapabilities()

        expected = {
            "bser-v2",
            "clock-sync-timeout",
            "cmd-clock",
            "cmd-debug-ageout",
            "cmd-debug-contenthash",
            "cmd-debug-drop-privs",
            "cmd-debug-get-asserted-states",
            "cmd-debug-get-subscriptions",
            "cmd-debug-poison",
            "cmd-debug-recrawl",
            "cmd-debug-root-status",
            "cmd-debug-set-parallel-crawl",
            "cmd-debug-set-subscriptions-paused",
            "cmd-debug-show-cursors",
            "cmd-debug-status",
            "cmd-debug-symlink-target-cache",
            "cmd-debug-watcher-info",
            "cmd-debug-watcher-info-clear",
            "cmd-find",
            "cmd-flush-subscriptions",
            "cmd-get-config",
            "cmd-get-log",
            "cmd-get-pid",
            "cmd-get-sockname",
            "cmd-global-log-level",
            "cmd-list-capabilities",
            "cmd-log",
            "cmd-log-level",
            "cmd-query",
            "cmd-shutdown-server",
            "cmd-since",
            "cmd-state-enter",
            "cmd-state-leave",
            "cmd-subscribe",
            "cmd-trigger",
            "cmd-trigger-del",
            "cmd-trigger-list",
            "cmd-unsubscribe",
            "cmd-version",
            "cmd-watch",
            "cmd-watch-del",
            "cmd-watch-del-all",
            "cmd-watch-list",
            "cmd-watch-project",
            "dedup_results",
            "field-atime",
            "field-atime_f",
            "field-atime_ms",
            "field-atime_ns",
            "field-atime_us",
            "field-cclock",
            "field-content.sha1hex",
            "field-ctime",
            "field-ctime_f",
            "field-ctime_ms",
            "field-ctime_ns",
            "field-ctime_us",
            "field-dev",
            "field-exists",
            "field-gid",
            "field-ino",
            "field-mode",
            "field-mtime",
            "field-mtime_f",
            "field-mtime_ms",
            "field-mtime_ns",
            "field-mtime_us",
            "field-name",
            "field-new",
            "field-nlink",
            "field-oclock",
            "field-size",
            "field-symlink_target",
            "field-type",
            "field-uid",
            "glob_generator",
            "relative_root",
            "saved-state-local",
            "scm-git",
            "scm-hg",
            "scm-since",
            "scope_fresh_instance_threshold",
            "suffix-set",
            "term-allof",
            "term-anyof",
            "term-dirname",
            "term-empty",
            "term-exists",
            "term-false",
            "term-idirname",
            "term-imatch",
            "term-iname",
            "term-ipcre",
            "term-match",
            "term-name",
            "term-not",
            "term-pcre",
            "term-since",
            "term-size",
            "term-suffix",
            "term-true",
            "term-type",
            "watcher-eden",
            "wildmatch",
            "wildmatch-multislash",
        }

        if sys.platform == "darwin":
            expected.add("watcher-fsevents")
            expected.add("watcher-kqueue")
            expected.add("watcher-kqueue+fsevents")
            expected.add("cmd-debug-kqueue-and-fsevents-recrawl")
            expected.add("cmd-debug-fsevents-inject-drop")
        elif sys.platform == "linux":
            expected.add("watcher-inotify")
        elif sys.platform == "win32":
            expected.add("watcher-win32")

        if os.environ.get("TESTING_VIA_BUCK", "0") == "1":
            expected.add("saved-state-manifold")

        unimportant = {
            "cmd-debug-prof-dump",
        }

        self.assertEqual(
            expected,
            set(res) - unimportant,
        )
