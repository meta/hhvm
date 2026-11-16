# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import json
import os
import os.path
import time

from watchman.integration.lib import WatchmanTestCase


IDLE_REAP_AGE_SECONDS = 10


@WatchmanTestCase.expand_matrix
class TestAgeOutWatch(WatchmanTestCase.WatchmanTestCase):
    def makeRootAndConfig(self):
        root = self.mkdtemp()
        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"idle_reap_age_seconds": IDLE_REAP_AGE_SECONDS}))
        return root

    def assertNotReaped(self, root) -> None:
        time.sleep(IDLE_REAP_AGE_SECONDS * 2)
        self.assertTrue(
            self.rootIsWatched(root), "%s should not have been reaped" % root
        )

    def test_watchReap(self) -> None:
        root = self.makeRootAndConfig()
        self.watchmanCommand("watch", root)

        # make sure that we don't reap when there are registered triggers
        self.watchmanCommand("trigger", root, {"name": "t", "command": ["true"]})

        self.assertNotReaped(root)

        # Make sure that we don't reap while we hold a subscription
        self.watchmanCommand("subscribe", root, "s", {"fields": ["name"]})
        self.watchmanCommand("trigger-del", root, "t")

        # subscription won't stick in cli mode
        if self.transport != "cli":
            self.assertNotReaped(root)

            # let's verify that we can safely reap two roots at once without
            # causing a deadlock
            second = self.makeRootAndConfig()
            self.watchmanCommand("watch", second)
            self.assertFileList(second, [".watchmanconfig"])

            # and unsubscribe from root and allow it to be reaped
            unsub = self.watchmanCommand("unsubscribe", root, "s")
            self.assertTrue(unsub["deleted"], "deleted subscription %s" % unsub)
            # and now we should be ready to reap
            self.assertWaitFor(
                lambda: not self.rootIsWatched(root) and not self.rootIsWatched(second)
            )

    def test_sanityCheckDoesNotPreventReap(self) -> None:
        # The sanity check runs `clock` against every root once a minute.
        # That must not count as activity, or a root whose reap age is
        # longer than the check interval can never be reaped.
        # This is server-side behaviour and the test has to outlast the
        # check interval, so run it in a single client configuration.
        if self.encoding != "bser" or self.splitWatcher:
            self.skipTest("server-side behaviour; one configuration is enough")

        root = self.mkdtemp()
        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"idle_reap_age_seconds": 70}))
        self.watchmanCommand("watch", root)

        self.assertWaitFor(
            lambda: not self.rootIsWatched(root),
            timeout=180,
            message="%s should have been reaped" % root,
        )
