# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import json
import os
import os.path
import unittest

import pywatchman
from watchman.integration.lib import WatchmanTestCase
from watchman.integration.lib.path_utils import norm_relative_path


@WatchmanTestCase.expand_matrix
class TestSubscribe(WatchmanTestCase.WatchmanTestCase):
    def requiresPersistentSession(self) -> bool:
        return True

    def wlockExists(self, subdata, exists) -> bool:
        norm_wlock = norm_relative_path(".hg/wlock")
        for sub in subdata:
            if "files" not in sub:
                # Don't trip over cancellation notices left over from other
                # tests that ran against this same instance
                continue
            for f in sub["files"]:
                if (
                    f["exists"] == exists
                    and norm_relative_path(f["name"]) == norm_wlock
                ):
                    return True
        return False

    def matchStateSubscription(self, subdata, mode):
        for sub in subdata:
            if mode in sub:
                return sub
        return None

    def assertWaitForAssertedStates(self, root, states) -> None:
        def sortStates(states):
            """Deterministically sort the states for comparison.
            We sort by name and rely on the sort being stable as the
            relative ordering of the potentially multiple queueued
            entries per name is important to preserve"""
            return sorted(states, key=lambda x: x["name"])

        states = sortStates(states)

        def getStates():
            res = self.watchmanCommand("debug-get-asserted-states", root)
            return sortStates(res["states"])

        self.assertWaitForEqual(states, getStates)

    def test_state_enter_leave(self) -> None:
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        result = self.watchmanCommand("debug-get-asserted-states", root)
        self.assertEqual([], result["states"])

        self.watchmanCommand("state-enter", root, "foo")
        self.watchmanCommand("state-enter", root, "bar")
        self.assertWaitForAssertedStates(
            root,
            [
                {"name": "bar", "state": "Asserted"},
                {"name": "foo", "state": "Asserted"},
            ],
        )

        self.assertListEqual(
            ["bar", "foo"],
            sorted(
                self.watchmanCommand(
                    "subscribe",
                    root,
                    "defer",
                    {"fields": ["name"], "defer": ["foo", "bar"]},
                ).get("asserted-states")
            ),
        )

        self.watchmanCommand("state-leave", root, "foo")
        self.assertWaitForAssertedStates(root, [{"name": "bar", "state": "Asserted"}])

        self.watchmanCommand("state-leave", root, "bar")
        self.assertWaitForAssertedStates(root, [])

    def test_defer_state(self) -> None:
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)

        self.watchmanCommand(
            "subscribe", root, "defer", {"fields": ["name"], "defer": ["foo"]}
        )

        self.touchRelative(root, "a")
        self.assertNotEqual(None, self.waitForSubFileList("defer", root, ["a"]))

        def isStateEnterFoo(sub):
            for item in sub:
                if item.get("state-enter", None) == "foo":
                    return True
            return False

        self.watchmanCommand("state-enter", root, "foo")
        sub = self.waitForSub("defer", root, accept=isStateEnterFoo)
        self.assertTrue(isStateEnterFoo(sub))

        self.touchRelative(root, "in-foo")
        # We expect this to timeout because state=foo is asserted
        with self.assertRaises(pywatchman.SocketTimeout):
            self.waitForSub("defer", root, timeout=1)

        self.watchmanCommand("state-leave", root, "foo")

        self.assertNotEqual(
            None,
            self.waitForSub(
                "defer",
                root,
                accept=lambda x: self.matchStateSubscription(x, "state-leave"),
            ),
        )

        # and now we should observe the file change
        self.assertNotEqual(None, self.waitForSub("defer", root))

        # and again, but this time passing metadata
        self.watchmanCommand("state-enter", root, {"name": "foo", "metadata": "meta!"})
        # pyrefly: ignore [unsupported-operation]
        begin = self.waitForSub("defer", root)[0]
        self.assertEqual("foo", begin["state-enter"])
        self.assertEqual("meta!", begin["metadata"])

        self.touchRelative(root, "in-foo-2")
        # flush-subscriptions should let this come through immediately
        flush = self.watchmanCommand(
            "flush-subscriptions", root, {"sync_timeout": 1000}
        )
        del flush["version"]
        self.assertDictEqual(
            {"synced": ["defer"], "no_sync_needed": [], "dropped": []}, flush
        )
        sub_data = self.getSubscription("defer", root)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(1, len(sub_data))
        # pyrefly: ignore [unsupported-operation]
        self.assertFileListsEqual(["in-foo-2"], sub_data[0]["files"])

        self.touchRelative(root, "in-foo-3")
        # We expect this to timeout because state=foo is asserted
        with self.assertRaises(pywatchman.SocketTimeout):
            self.waitForSub("defer", root, timeout=1)

        self.watchmanCommand(
            "state-leave", root, {"name": "foo", "metadata": "leavemeta"}
        )

        # pyrefly: ignore [unsupported-operation]
        end = self.waitForSub(
            "defer",
            root,
            accept=lambda x: self.matchStateSubscription(x, "state-leave"),
        )[0]
        self.assertEqual("leavemeta", end["metadata"])

        # and now we should observe the file change
        self.assertNotEqual(None, self.waitForSubFileList("defer", root, ["in-foo-3"]))

    def test_drop_state(self) -> None:
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)

        self.watchmanCommand(
            "subscribe", root, "drop", {"fields": ["name"], "drop": ["foo"]}
        )
        self.assertNotEqual(None, self.waitForSub("drop", root=root))

        self.touchRelative(root, "a")
        subResult = self.waitForSubFileList("drop", root, ["a"])
        self.assertNotEqual(None, subResult)

        self.watchmanCommand("state-enter", root, "foo")
        subResult = self.waitForSub("drop", root)
        print(subResult)
        # pyrefly: ignore [unsupported-operation]
        begin = subResult[0]
        self.assertEqual("foo", begin["state-enter"])

        self.touchRelative(root, "in-foo")
        flush = self.watchmanCommand(
            "flush-subscriptions", root, {"sync_timeout": 1000}
        )
        del flush["version"]
        self.assertDictEqual(
            {"synced": [], "no_sync_needed": [], "dropped": ["drop"]}, flush
        )

        self.touchRelative(root, "in-foo-2")
        # We expect this to timeout because state=foo is asserted
        with self.assertRaises(pywatchman.SocketTimeout):
            self.waitForSub("drop", root, timeout=1)

        self.watchmanCommand("state-leave", root, "foo")

        self.assertNotEqual(
            None,
            self.waitForSub(
                "drop",
                root,
                accept=lambda x: self.matchStateSubscription(x, "state-leave"),
            ),
        )

        # There should be no more subscription data to observe
        # because we requested that it be dropped
        with self.assertRaises(pywatchman.SocketTimeout):
            self.waitForSub("drop", root, timeout=1)

        # let's make sure that we can observe new changes
        self.touchRelative(root, "out-foo")

        self.assertFileList(root, files=["a", "in-foo", "in-foo-2", "out-foo"])
        self.assertNotEqual(None, self.waitForSubFileList("drop", root, ["out-foo"]))

    def test_defer_vcs(self) -> None:
        root = self.mkdtemp()
        # fake an hg control dir
        os.mkdir(os.path.join(root, ".hg"))
        # touch another file so that the initial subscription result comes
        # through
        self.touchRelative(root, "foo")
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=[".hg", "foo"])

        self.watchmanCommand(
            "subscribe",
            root,
            "defer",
            {
                "expression": ["type", "f"],
                "fields": ["name", "exists"],
                "defer_vcs": True,
            },
        )

        # pyrefly: ignore [unsupported-operation]
        dat = self.waitForSub("defer", root)[0]
        self.assertEqual(True, dat["is_fresh_instance"])
        self.assertEqual([{"name": "foo", "exists": True}], dat["files"])

        # Pretend that hg is update the working copy
        self.touchRelative(root, ".hg", "wlock")
        # flush-subscriptions should force the update through
        flush = self.watchmanCommand(
            "flush-subscriptions", root, {"sync_timeout": 1000}
        )
        del flush["version"]
        self.assertDictEqual(
            {"synced": ["defer"], "no_sync_needed": [], "dropped": []}, flush
        )
        sub_data = self.getSubscription("defer", root)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(1, len(sub_data))
        self.assertFileListsEqual(
            [".hg/wlock"],
            # pyrefly: ignore [unsupported-operation]
            [d["name"] for d in sub_data[0]["files"]],
        )

        self.touchRelative(root, "in-foo")
        # We expect this to timeout because the wlock file exists
        with self.assertRaises(pywatchman.SocketTimeout):
            self.waitForSub(
                "defer", root, accept=lambda x: self.wlockExists(x, True), timeout=2
            )

        # Remove the wlock and allow subscriptions to flow
        os.unlink(os.path.join(root, ".hg", "wlock"))

        dat = self.waitForSub(
            "defer", root, timeout=2, accept=lambda x: self.wlockExists(x, False)
        )
        self.assertNotEqual(None, dat)

    def test_immediate_subscribe(self) -> None:
        root = self.mkdtemp()
        # fake an hg control dir
        os.mkdir(os.path.join(root, ".hg"))
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=[".hg"])

        self.watchmanCommand(
            "subscribe",
            root,
            "nodefer",
            {"fields": ["name", "exists"], "defer_vcs": False},
        )

        # pyrefly: ignore [unsupported-operation]
        dat = self.waitForSub("nodefer", root)[0]
        self.assertEqual(True, dat["is_fresh_instance"])
        self.assertEqual([{"name": ".hg", "exists": True}], dat["files"])

        # Pretend that hg is update the working copy
        self.touchRelative(root, ".hg", "wlock")

        dat = self.waitForSub(
            "nodefer", root, accept=lambda x: self.wlockExists(x, True)
        )
        # We observed the changes even though wlock existed
        self.assertNotEqual(None, dat)

        os.unlink(os.path.join(root, ".hg", "wlock"))

        dat = self.waitForSub(
            "nodefer", root, accept=lambda x: self.wlockExists(x, False)
        )
        self.assertNotEqual(None, dat)

    def test_multi_cancel(self) -> None:
        """Test that for multiple subscriptions on the same socket, we receive
        cancellation notices for all of them."""
        root = self.mkdtemp()
        self.touchRelative(root, "lemon")
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=["lemon"])

        for n in range(32):
            sub_name = "sub%d" % n
            self.watchmanCommand("subscribe", root, sub_name, {"fields": ["name"]})
            # Drain the initial messages
            dat = self.waitForSub(sub_name, root, remove=True)
            # pyrefly: ignore [bad-argument-type]
            self.assertEqual(len(dat), 1)
            # pyrefly: ignore [unsupported-operation]
            dat = dat[0]
            self.assertFileListsEqual(dat["files"], ["lemon"])

        self.watchmanCommand("watch-del", root)

        for n in range(32):
            # If the cancellation notice doesn't come through this will timeout.
            dat = self.waitForSub("sub%d" % n, root)
            # pyrefly: ignore [bad-argument-type]
            self.assertEqual(len(dat), 1)
            # pyrefly: ignore [unsupported-operation]
            dat = dat[0]
            self.assertTrue(dat["canceled"])
            self.assertTrue(dat["unilateral"])

    def test_subscribe(self) -> None:
        root = self.mkdtemp()
        a_dir = os.path.join(root, "a")
        os.mkdir(a_dir)
        self.touchRelative(a_dir, "lemon")
        self.touchRelative(root, "b")

        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=["a", "a/lemon", "b"])

        self.watchmanCommand("subscribe", root, "myname", {"fields": ["name"]})

        self.watchmanCommand(
            "subscribe", root, "relative", {"fields": ["name"], "relative_root": "a"}
        )

        # prove initial results come through
        # pyrefly: ignore [unsupported-operation]
        dat = self.waitForSub("myname", root=root)[0]
        self.assertEqual(True, dat["is_fresh_instance"])
        self.assertFileListsEqual(dat["files"], ["a", "a/lemon", "b"])

        # and that relative_root adapts the path name
        # pyrefly: ignore [unsupported-operation]
        dat = self.waitForSub("relative", root=root)[0]
        self.assertEqual(True, dat["is_fresh_instance"])
        self.assertFileListsEqual(dat["files"], ["lemon"])

        # check that deletes show up in the subscription results
        os.unlink(os.path.join(root, "a", "lemon"))
        dat = self.waitForSub(
            "myname",
            root=root,
            accept=lambda x: self.findSubscriptionContainingFile(x, "a/lemon"),
        )
        self.assertNotEqual(None, dat)
        # pyrefly: ignore [unsupported-operation]
        self.assertEqual(False, dat[0]["is_fresh_instance"])

        dat = self.waitForSub(
            "relative",
            root=root,
            accept=lambda x: self.findSubscriptionContainingFile(x, "lemon"),
        )
        self.assertNotEqual(None, dat)
        # pyrefly: ignore [unsupported-operation]
        self.assertEqual(False, dat[0]["is_fresh_instance"])

        # Trigger a recrawl and ensure that the subscription isn't lost
        self.watchmanCommand("debug-recrawl", root)

        def matchesRecrawledDir(subdata):
            for sub in subdata:
                if "warning" in sub:
                    return True
            return False

        # ensure that there is at least one change to broadcast
        self.touchRelative(root, "a")
        dat = self.waitForSub("myname", root=root, accept=matchesRecrawledDir)
        self.assertNotEqual(None, dat)

        # Ensure that we observed the recrawl warning
        warn = None
        # pyrefly: ignore [not-iterable]
        for item in dat:
            if "warning" in item:
                warn = item["warning"]
                break
        # pyrefly: ignore [bad-specialization]
        self.assertRegex(warn, r"Recrawled this watch")

    # TODO: this test is very flaky on Windows
    @unittest.skipIf(os.name == "nt", "win")
    def test_flush_subscriptions(self) -> None:
        root = self.mkdtemp()

        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"win32_batch_latency_ms": 0}))

        a_dir = os.path.join(root, "a")

        os.mkdir(a_dir)
        self.touchRelative(a_dir, "lemon.txt")
        self.touchRelative(a_dir, "orange.dat")
        self.touchRelative(root, "b")

        self.watchmanCommand("watch", root)
        self.assertFileList(
            root, files=[".watchmanconfig", "a", "a/lemon.txt", "a/orange.dat", "b"]
        )

        self.watchmanCommand(
            "subscribe", root, "sub1", {"fields": ["name"], "expression": ["type", "f"]}
        )
        self.watchmanCommand(
            "subscribe",
            root,
            "sub2",
            {
                "fields": ["name"],
                "expression": ["allof", ["type", "f"], ["suffix", "txt"]],
            },
        )
        self.watchmanCommand(
            "subscribe",
            root,
            "sub3",
            {
                "fields": ["name"],
                "expression": ["allof", ["type", "f"], ["suffix", "dat"]],
            },
        )

        root2 = self.mkdtemp()
        self.touchRelative(root2, "banana")
        self.watchmanCommand("watch", root2)
        self.assertFileList(root2, files=["banana"])
        ret = self.watchmanCommand(
            "subscribe", root2, "sub-other", {"fields": ["name"]}
        )

        # discard the initial result PDUs
        self.waitForSub("sub1", root=root)
        self.waitForSub("sub2", root=root)
        self.waitForSub("sub3", root=root)

        # pause subscriptions so that the result of flush-subscriptions is
        # deterministic
        debug_ret = self.watchmanCommand(
            "debug-set-subscriptions-paused", {"sub1": True, "sub2": True, "sub3": True}
        )
        self.assertDictEqual(
            {
                "sub1": {"old": False, "new": True},
                "sub2": {"old": False, "new": True},
                "sub3": {"old": False, "new": True},
            },
            debug_ret["paused"],
        )

        self.touchRelative(root, "c")
        self.touchRelative(a_dir, "d.txt")
        self.touchRelative(a_dir, "e.dat")

        # test out a few broken flush-subscriptions
        broken_args = [
            ((), "wrong number of arguments to 'flush-subscriptions'"),
            ((root,), "wrong number of arguments to 'flush-subscriptions'"),
            (
                (root, {"subscriptions": ["sub1"]}),
                "key 'sync_timeout' is not present in this json object",
            ),
            (
                (root, {"subscriptions": "wat", "sync_timeout": 2000}),
                "expected 'subscriptions' to be an array of subscription names",
            ),
            (
                (root, {"subscriptions": ["sub1", False], "sync_timeout": 2000}),
                "expected 'subscriptions' to be an array of subscription names",
            ),
            (
                (root, {"subscriptions": ["sub1", "notsub"], "sync_timeout": 2000}),
                "this client does not have a subscription named 'notsub'",
            ),
            (
                (root, {"subscriptions": ["sub1", "sub-other"], "sync_timeout": 2000}),
                "subscription 'sub-other' is on root",
            ),
        ]

        for args, err_msg in broken_args:
            with self.assertRaises(pywatchman.WatchmanError) as ctx:
                self.watchmanCommand("flush-subscriptions", *args)
            self.assertIn(err_msg, str(ctx.exception))

        ret = self.watchmanCommand(
            "flush-subscriptions",
            root,
            {"sync_timeout": 1000, "subscriptions": ["sub1", "sub2"]},
        )
        version = ret["version"]
        self.assertEqual([], ret["no_sync_needed"])
        self.assertCountEqual(["sub1", "sub2"], ret["synced"])

        # Do not wait for subscription results -- instead, make sure they've
        # shown up immediately.
        sub1_data = self.getSubscription("sub1", root)
        sub2_data = self.getSubscription("sub2", root)

        for sub_name, sub_data in [("sub1", sub1_data), ("sub2", sub2_data)]:
            # pyrefly: ignore [bad-argument-type]
            self.assertEqual(1, len(sub_data))
            # pyrefly: ignore [unsupported-operation]
            data = sub_data[0].copy()
            # we'll verify these below
            del data["files"]
            del data["since"]
            # this is subject to change so we can't verify it
            del data["clock"]

            self.assertDictEqual(
                {
                    "version": version,
                    "is_fresh_instance": False,
                    "subscription": sub_name,
                    "root": root,
                    "unilateral": True,
                },
                data,
            )

        # pyrefly: ignore [unsupported-operation]
        self.assertFileListsEqual(["a/d.txt", "a/e.dat", "c"], sub1_data[0]["files"])
        # pyrefly: ignore [unsupported-operation]
        self.assertFileListsEqual(["a/d.txt"], sub2_data[0]["files"])

        # touch another file, make sure the updates come through again
        self.touchRelative(a_dir, "f.dat")
        ret = self.watchmanCommand(
            "flush-subscriptions",
            root,
            # default for subscriptions is to sync all subscriptions matching
            # root (so sub1, sub2 and sub3, but not sub-other)
            {"sync_timeout": 1000},
        )
        self.assertCountEqual(["sub2"], ret["no_sync_needed"])
        self.assertCountEqual(["sub1", "sub3"], ret["synced"])

        # again, don't wait for the subscriptions
        sub1_data2 = self.getSubscription("sub1", root)
        sub2_data2 = self.getSubscription("sub2", root)
        sub3_data2 = self.getSubscription("sub3", root)

        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(1, len(sub1_data2))
        # no updates to sub2, so we expect nothing
        self.assertIs(None, sub2_data2)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(1, len(sub3_data2))

        self.assertEqual(
            # pyrefly: ignore [unsupported-operation]
            sub1_data[0]["clock"],
            # pyrefly: ignore [unsupported-operation]
            sub1_data2[0]["since"],
            'for sub1, previous "clock" should be current "since"',
        )
        # pyrefly: ignore [unsupported-operation]
        self.assertFileListsEqual(["a/f.dat"], sub1_data2[0]["files"])
        # pyrefly: ignore [unsupported-operation]
        self.assertFileListsEqual(["a/e.dat", "a/f.dat"], sub3_data2[0]["files"])

        # now resume the subscriptions and make sure future updates (and only
        # future updates) come through
        self.watchmanCommand(
            "debug-set-subscriptions-paused",
            {"sub1": False, "sub2": False, "sub3": False},
        )

        self.touchRelative(root, "newfile.txt")
        # pyrefly: ignore [unsupported-operation]
        new_sub1 = self.waitForSub("sub1", root=root)[0]
        # pyrefly: ignore [unsupported-operation]
        new_sub2 = self.waitForSub("sub2", root=root)[0]

        # pyrefly: ignore [unsupported-operation]
        self.assertEqual(sub1_data2[0]["clock"], new_sub1["since"])
        # for sub2 the clock is different because we've actually forced an
        # evaluation in between in the second flush-subscriptions call, so we
        # don't have a reference point
        self.assertFileListsEqual(["newfile.txt"], new_sub1["files"])
        self.assertFileListsEqual(["newfile.txt"], new_sub2["files"])

    def test_unsub_deadlock(self) -> None:
        """I saw a stack trace of a lock assertion that seemed to originate
        in the unsubByName() method.  It looks possible for this to call
        itself recursively and this test exercises that code path.  It
        also exercises a similar deadlock where multiple subscriptions from
        multiple connections are torn down around the same time."""
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        clock = self.watchmanCommand("clock", root)["clock"]
        for _ in range(0, 100):
            clients = []
            for i in range(0, 20):
                client = self.getClient(no_cache=True)
                client.query(
                    "subscribe", root, "sub%s" % i, {"fields": ["name"], "since": clock}
                )
                self.touchRelative(root, "a")
                clients.append(client)
            for client in clients:
                client.close()

    def test_subscription_cleanup(self) -> None:
        """Verify that subscriptions get cleaned up from internal state on
        unsubscribes and socket disconnects. This test failing usually
        indicates a reference cycle keeping the subscriber alive."""
        root = self.mkdtemp()
        a_dir = os.path.join(root, "a")

        os.mkdir(a_dir)
        self.touchRelative(a_dir, "lemon.txt")
        self.touchRelative(a_dir, "orange.dat")
        self.touchRelative(root, "b")

        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=["a", "a/lemon.txt", "a/orange.dat", "b"])

        self.watchmanCommand(
            "subscribe", root, "sub1", {"fields": ["name"], "expression": ["type", "f"]}
        )

        self.touchRelative(a_dir, "wat.txt")

        self.watchmanCommand(
            "subscribe", root, "sub2", {"fields": ["name"], "expression": ["type", "f"]}
        )

        out = self.watchmanCommand("debug-get-subscriptions", root)
        subs = {sub["info"]["name"] for sub in out["subscribers"]}
        self.assertCountEqual({"sub1", "sub2"}, subs)

        # this should remove sub1 from the map
        self.watchmanCommand("unsubscribe", root, "sub1")
        out = self.watchmanCommand("debug-get-subscriptions", root)
        subs = {sub["info"]["name"] for sub in out["subscribers"]}
        self.assertCountEqual({"sub2"}, subs)

        # flush sub2 so that there's no reason anything else would be keeping
        # it around
        self.watchmanCommand("flush-subscriptions", root, {"sync_timeout": 1000})
        # disconnect from the socket -- the next command will reconnect the
        # socket, but sub2 should have disappeared
        self.client.close()

        # It might take a while for watchman to realize its connection has been
        # reset, so check repeatedly.
        def checkSubscribers():
            out = self.watchmanCommand("debug-get-subscriptions", root)
            return out["subscribers"]

        self.assertWaitForEqual([], checkSubscribers)

    @WatchmanTestCase.skip_for(codecs=["json"])
    def test_subscribe_unicode(self) -> None:
        unicode_filename = "\u263a"

        root = self.mkdtemp()
        a_dir = os.path.join(root, "a")
        os.mkdir(a_dir)
        self.touchRelative(a_dir, "lemon")
        self.touchRelative(root, "b")
        self.touchRelative(root, unicode_filename)

        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=["a", "a/lemon", "b", unicode_filename])

        self.watchmanCommand("subscribe", root, "myname", {"fields": ["name"]})

        self.watchmanCommand(
            "subscribe", root, "relative", {"fields": ["name"], "relative_root": "a"}
        )

        # prove initial results come through
        # pyrefly: ignore [unsupported-operation]
        dat = self.waitForSub("myname", root=root)[0]
        self.assertEqual(True, dat["is_fresh_instance"])
        self.assertFileListsEqual(dat["files"], ["a", "a/lemon", "b", unicode_filename])

        os.unlink(os.path.join(root, "a", "lemon"))

        # Trigger a recrawl and ensure that the subscription isn't lost
        self.watchmanCommand("debug-recrawl", root)

        def matchesRecrawledDir(subdata):
            for sub in subdata:
                if "warning" in sub:
                    return True
            return False

        # ensure that there is at least one change to broadcast
        self.touchRelative(root, "a")
        dat = self.waitForSub("myname", root=root, accept=matchesRecrawledDir)
        self.assertNotEqual(None, dat)

        # Ensure that we observed the recrawl warning
        warn = None
        # pyrefly: ignore [not-iterable]
        for item in dat:
            if "warning" in item:
                warn = item["warning"]
                break
        # pyrefly: ignore [bad-specialization]
        self.assertRegex(warn, r"Recrawled this watch")

    def test_unique_name_error(self) -> None:
        root = self.mkdtemp()
        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"enforce_unique_subscription_names": True}))
        self.touchRelative(root, "lemon")
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=["lemon", ".watchmanconfig"])

        # Create a subscription
        self.watchmanCommand("subscribe", root, "my_sub", {"fields": ["name"]})
        # Drain the initial messages
        dat = self.waitForSub("my_sub", root, remove=True)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["lemon", ".watchmanconfig"])

        # Try to create another subscription with the same name
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand(
                "subscribe",
                root,
                "my_sub",
                {"fields": ["name"], "expression": ["false"]},
            )
        self.assertIn("subscription name 'my_sub' is not unique", str(ctx.exception))

        # Ensure the initial subscription is in force
        self.touchRelative(root, "banana")
        dat = self.waitForSub("my_sub", root, remove=True)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["banana"])

    def test_resubscribe_same_name_no_error(self) -> None:
        root = self.mkdtemp()
        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"enforce_unique_subscription_names": True}))
        self.touchRelative(root, "lemon")
        self.touchRelative(root, "banana")
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=["lemon", "banana", ".watchmanconfig"])

        # Create a subscription
        self.watchmanCommand(
            "subscribe",
            root,
            "my_sub",
            {"fields": ["name"], "expression": ["name", "lemon"]},
        )
        # Drain the initial messages
        dat = self.waitForSub("my_sub", root, remove=True)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["lemon"])
        # Unsubscribe
        self.watchmanCommand("unsubscribe", root, "my_sub")

        # Create a new subscription with the same name
        self.watchmanCommand(
            "subscribe",
            root,
            "my_sub",
            {"fields": ["name"], "expression": ["name", "banana"]},
        )
        # Ensure the later subscription is in force
        dat = self.waitForSub("my_sub", root, remove=True)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["banana"])

    def test_resubscribe_same_name_no_warning(self) -> None:
        root = self.mkdtemp()
        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"enforce_unique_subscription_names": False}))
        self.touchRelative(root, "lemon")
        self.touchRelative(root, "banana")
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=["lemon", "banana", ".watchmanconfig"])

        # Create a subscription
        self.watchmanCommand(
            "subscribe",
            root,
            "my_sub",
            {"fields": ["name"], "expression": ["name", "lemon"]},
        )
        # Drain the initial messages
        dat = self.waitForSub("my_sub", root, remove=True)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["lemon"])
        # Unsubscribe
        self.watchmanCommand("unsubscribe", root, "my_sub")

        # Create a new subscription with the same name
        dat = self.watchmanCommand(
            "subscribe",
            root,
            "my_sub",
            {"fields": ["name"], "expression": ["name", "banana"]},
        )
        # Make sure we don't get a spurious warning here
        self.assertNotIn("warning", dat)

        # Ensure the later subscription is in force
        dat = self.waitForSub("my_sub", root, remove=True)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["banana"])

    def test_multi_client_same_name(self) -> None:
        root = self.mkdtemp()
        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"enforce_unique_subscription_names": True}))
        self.touchRelative(root, "lemon")
        self.touchRelative(root, "banana")
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=["lemon", "banana", ".watchmanconfig"])

        client1 = self.getClient(no_cache=True)
        client2 = self.getClient(no_cache=True)

        # Create a subscription
        client1.query(
            "subscribe",
            root,
            "my_sub",
            {"fields": ["name"], "expression": ["name", "lemon"]},
        )
        dat = self.waitForSub("my_sub", root, remove=True, client=client1)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["lemon"])

        # Create a new subscription with the same name from a different client,
        # with a different expression
        client2.query(
            "subscribe",
            root,
            "my_sub",
            {"fields": ["name"], "expression": ["name", "banana"]},
        )
        dat = self.waitForSub("my_sub", root, remove=True, client=client2)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["banana"])

        # Trigger events on both and ensure they are routed correctly.
        self.touchRelative(root, "lemon")
        self.touchRelative(root, "banana")

        dat = self.waitForSub("my_sub", root, remove=True, client=client1)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["lemon"])

        dat = self.waitForSub("my_sub", root, remove=True, client=client2)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["banana"])

        client1.query("unsubscribe", root, "my_sub")
        client1.close()

        # Ensure unsubscribing and closing one client hasn't affected the other
        self.touchRelative(root, "lemon")
        self.touchRelative(root, "banana")

        dat = self.waitForSub("my_sub", root, remove=True, client=client2)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["banana"])

        client2.close()

    def test_unique_name_warning(self) -> None:
        root = self.mkdtemp()
        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"enforce_unique_subscription_names": False}))
        self.touchRelative(root, "lemon")
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=["lemon", ".watchmanconfig"])

        # Create a subscription
        self.watchmanCommand("subscribe", root, "my_sub", {"fields": ["name"]})
        # Drain the initial messages
        dat = self.waitForSub("my_sub", root, remove=True)
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(len(dat), 1)
        # pyrefly: ignore [unsupported-operation]
        dat = dat[0]
        self.assertFileListsEqual(dat["files"], ["lemon", ".watchmanconfig"])

        # Create another subscription with the same name
        dat = self.watchmanCommand("subscribe", root, "my_sub", {"fields": ["name"]})
        self.assertEqual(dat["warning"], "subscription name 'my_sub' is not unique")

    def findSubscriptionContainingFile(self, subdata, filename):
        filename = norm_relative_path(filename)
        for dat in subdata:
            if "files" in dat and filename in self.normFileList(dat["files"]):
                return dat
        return None

    def normFileList(self, files):
        return sorted(map(norm_relative_path, files))
