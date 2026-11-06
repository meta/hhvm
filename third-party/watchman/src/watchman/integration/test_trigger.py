# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import json
import os
import os.path
import re
import sys

from watchman.integration.lib import HELPER_ROOT, WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestTrigger(WatchmanTestCase.WatchmanTestCase):
    def requiresPersistentSession(self) -> bool:
        # cli transport has no log subscriptions
        return True

    def hasTriggerInLogs(self, root, triggerName) -> bool:
        pat = "%s.*posix_spawnp: %s" % (re.escape(root), triggerName)
        r = re.compile(pat, re.I)
        for line in self.getServerLogContents():
            line = line.replace("\\", "/")
            if r.search(line):
                return True
        return False

    # https://github.com/meta/watchman/issues/141
    def test_triggerIssue141(self) -> None:
        root = self.mkdtemp()
        self.touchRelative(root, "foo.js")

        self.watchmanCommand("watch", root)
        self.assertFileList(root, ["foo.js"])

        touch = os.path.join(HELPER_ROOT, "touch.py")
        logs = self.mkdtemp()
        first_log = os.path.join(logs, "first")
        second_log = os.path.join(logs, "second")

        res = self.watchmanCommand(
            "trigger", root, "first", "*.js", "--", sys.executable, touch, first_log
        )
        self.assertEqual(res["triggerid"], "first")

        res = self.watchmanCommand(
            "trigger", root, "second", "*.js", "--", sys.executable, touch, second_log
        )
        self.assertEqual(res["triggerid"], "second")

        self.assertWaitFor(
            lambda: os.path.exists(first_log) and os.path.exists(second_log),
            message="both triggers fire at start",
        )

        # touch the file, should run both triggers
        self.touchRelative(root, "foo.js")

        self.assertWaitFor(
            lambda: self.hasTriggerInLogs(root, "first")
            and self.hasTriggerInLogs(root, "second"),
            message="both triggers fired on update",
        )

    def validate_trigger_output(self, root, files, context) -> None:
        trigger_log = os.path.join(root, "trigger.log")
        trigger_json = os.path.join(root, "trigger.json")

        def files_are_listed():
            if not os.path.exists(trigger_log):
                return False
            with open(trigger_log) as f:
                n = 0
                for line in f:
                    for filename in files:
                        if filename in line:
                            n = n + 1
                return n == len(files)

        self.assertWaitFor(
            lambda: files_are_listed(),
            message="%s should contain %s" % (trigger_log, json.dumps(files)),
        )

        def files_are_listed_json():
            if not os.path.exists(trigger_json):
                return False
            expect = {}
            for f in files:
                expect[f] = True
            with open(trigger_json) as f:
                result = {}
                for line in f:
                    data = json.loads(line)
                    for item in data:
                        result[item["name"]] = item["exists"]

                return result == expect

        self.assertWaitFor(
            lambda: files_are_listed_json(),
            message="%s should contain %s" % (trigger_json, json.dumps(files)),
        )

    def test_legacyTrigger(self) -> None:
        root = self.mkdtemp()

        self.touchRelative(root, "foo.c")
        self.touchRelative(root, "b ar.c")
        self.touchRelative(root, "bar.txt")

        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            json.dump({"settle": 200}, f)

        watch = self.watchmanCommand("watch", root)

        self.assertFileList(root, [".watchmanconfig", "b ar.c", "bar.txt", "foo.c"])

        res = self.watchmanCommand(
            "trigger",
            root,
            "test",
            "*.c",
            "--",
            sys.executable,
            os.path.join(HELPER_ROOT, "trig.py"),
            os.path.join(root, "trigger.log"),
        )
        self.assertEqual("created", res["disposition"])

        res = self.watchmanCommand(
            "trigger",
            root,
            "other",
            "*.c",
            "--",
            sys.executable,
            os.path.join(HELPER_ROOT, "trigjson.py"),
            os.path.join(root, "trigger.json"),
        )
        self.assertEqual("created", res["disposition"])

        # check that the legacy parser produced the right trigger def
        expect = [
            {
                "name": "other",
                "append_files": True,
                "command": [
                    sys.executable,
                    os.path.join(HELPER_ROOT, "trigjson.py"),
                    os.path.join(root, "trigger.json"),
                ],
                "expression": ["anyof", ["match", "*.c", "wholename"]],
                "stdin": ["name", "exists", "new", "size", "mode"],
            },
            {
                "name": "test",
                "append_files": True,
                "command": [
                    sys.executable,
                    os.path.join(HELPER_ROOT, "trig.py"),
                    os.path.join(root, "trigger.log"),
                ],
                "expression": ["anyof", ["match", "*.c", "wholename"]],
                "stdin": ["name", "exists", "new", "size", "mode"],
            },
        ]

        triggers = self.watchmanCommand("trigger-list", root).get("triggers")
        self.assertCountEqual(triggers, expect)

        self.suspendWatchman()
        self.touchRelative(root, "foo.c")
        self.touchRelative(root, "b ar.c")
        self.resumeWatchman()

        self.assertWaitFor(
            lambda: self.hasTriggerInLogs(root, "test")
            and self.hasTriggerInLogs(root, "other"),
            message="both triggers fired on update",
        )

        self.validate_trigger_output(root, ["foo.c", "b ar.c"], "initial")

        def remove_logs():
            os.unlink(os.path.join(root, "trigger.log"))
            os.unlink(os.path.join(root, "trigger.json"))

        for f in ("foo.c", "b ar.c"):
            # Validate that we observe the updates correctly
            # (that we're handling the since portion of the query)
            self.suspendWatchman()
            remove_logs()
            self.touchRelative(root, f)
            self.resumeWatchman()

            self.validate_trigger_output(root, [f], "only %s" % f)

        remove_logs()

        self.watchmanCommand("debug-recrawl", root)
        # ensure that the triggers don't get deleted
        triggers = self.watchmanCommand("trigger-list", root).get("triggers")
        self.assertCountEqual(triggers, expect)

        self.touchRelative(root, "foo.c")
        expect = ["foo.c"]
        if watch["watcher"] == "win32":
            # We end up re-scanning the root here and noticing that
            # b ar.c has changed.  What we're testing here is that
            # the trigger is run again, and it is ok if it notifies
            # about more files on win32, so just adjust expec:tations
            # here in the test to accommodate that difference.
            expect = ["foo.c", "b ar.c"]

        self.validate_trigger_output(root, expect, "after recrawl")

        # Now test to see how we deal with updating the defs
        res = self.watchmanCommand("trigger", root, "other", "*.c", "--", "true")
        self.assertEqual("replaced", res["disposition"])

        res = self.watchmanCommand("trigger", root, "other", "*.c", "--", "true")
        self.assertEqual("already_defined", res["disposition"])

        # and deletion
        res = self.watchmanCommand("trigger-del", root, "test")
        self.assertTrue(res["deleted"])
        self.assertEqual("test", res["trigger"])

        triggers = self.watchmanCommand("trigger-list", root)
        self.assertEqual(1, len(triggers["triggers"]))

        res = self.watchmanCommand("trigger-del", root, "other")
        self.assertTrue(res["deleted"])
        self.assertEqual("other", res["trigger"])

        triggers = self.watchmanCommand("trigger-list", root)
        self.assertEqual(0, len(triggers["triggers"]))
