# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import contextlib
import errno
import functools
import inspect
import json
import os
import os.path
import sys
import tempfile
import time
import unittest
from typing import Any, Dict, Iterator, List

import pywatchman

from . import Interrupt, TempDir, WatchmanInstance
from .path_utils import norm_absolute_path, norm_relative_path


STRING_TYPES = (str, bytes)


if os.name == "nt":
    # monkey patch to hopefully minimize test flakiness
    def wrap_with_backoff(fn):
        def wrapper(*args, **kwargs):
            delay = 0.01
            attempts = 10
            while True:
                try:
                    return fn(*args, **kwargs)
                except WindowsError as e:
                    if attempts == 0:
                        raise
                    # WindowsError: [Error 32] The process cannot access the
                    # file because it is being used by another process.
                    # Error 5: Access is denied.
                    if e.winerror not in (5, 32):
                        raise

                attempts = attempts - 1
                time.sleep(delay)
                delay = delay * 2

        return wrapper

    for name in ["rename", "unlink", "remove", "rmdir", "makedirs"]:
        setattr(os, name, wrap_with_backoff(getattr(os, name)))


class TempDirPerTestMixin(unittest.TestCase):
    def __init__(self, *args, **kwargs) -> None:
        super(TempDirPerTestMixin, self).__init__(*args, **kwargs)
        self.tempdir = None

    def setUp(self) -> None:
        super(TempDirPerTestMixin, self).setUp()

        id = self._getTempDirName()

        # Arrange for any temporary stuff we create to go under
        # our global tempdir and put it in a dir named for the test
        self.tempdir = os.path.join(TempDir.get_temp_dir().get_dir(), id)
        os.mkdir(self.tempdir)

    def _getTempDirName(self) -> str:
        return self.id()

    def mkdtemp(self, **kwargs):
        return norm_absolute_path(tempfile.mkdtemp(dir=self.tempdir, **kwargs))

    def mktemp(self, prefix: str = "") -> str:
        f, name = tempfile.mkstemp(prefix=prefix, dir=self.tempdir)
        os.close(f)
        return name


class WatchmanTestCase(TempDirPerTestMixin, unittest.TestCase):
    transport: str
    encoding: str
    parallelCrawl: bool
    splitWatcher: bool

    def __init__(self, methodName: str = "run") -> None:
        super(WatchmanTestCase, self).__init__(methodName)
        # pyre-fixme[16]: `WatchmanTestCase` has no attribute `setDefaultConfiguration`.
        self.setDefaultConfiguration()
        self.maxDiff = None
        self.attempt = 0
        # ASAN-enabled builds can be slower enough that we hit timeouts
        # with the default of 1 second
        self.socketTimeout = 80.0

    def requiresPersistentSession(self) -> bool:
        return False

    def checkPersistentSession(self) -> None:
        if self.requiresPersistentSession() and self.transport == "cli":
            self.skipTest("need persistent session")

    def checkOSApplicability(self) -> None:
        # override this to call self.skipTest if this test class should skip
        # on the current OS
        pass

    def skipIfCapabilityMissing(self, cap, reason) -> None:
        res = self.getClient().capabilityCheck([cap])
        if not res["capabilities"][cap]:
            self.skipTest(reason)

    def setUp(self) -> None:
        super(WatchmanTestCase, self).setUp()
        self.checkPersistentSession()
        self.checkOSApplicability()

    def tearDown(self) -> None:
        """Print the watchman logs when a test complete

        When debugging watchman test issues, having access to the logs makes
        debugging much easier, and since test runners usually omit the test
        output on success, let's always display the test output.
        """
        print("Watchman logs:")
        self.dumpLogs()

    def getClient(
        self, inst=None, replace_cached: bool = False, no_cache: bool = False
    ):
        if inst or not hasattr(self, "client") or no_cache:
            client = pywatchman.client(
                timeout=self.socketTimeout,
                transport=self.transport,
                sendEncoding=self.encoding,
                recvEncoding=self.encoding,
                sockpath=(inst or self.watchmanInstance()).getSockPath(),
            )
            if (not inst or replace_cached) and not no_cache:
                # only cache the client if it points to the shared instance
                # pyre-fixme[16]: `WatchmanTestCase` has no attribute `client`.
                self.client = client
                self.addCleanup(lambda: self.__clearClient())
            return client
        return self.client

    def __logTestInfo(self, test, msg):
        if hasattr(self, "client"):
            try:
                self.getClient().query("log", "debug", "TEST: %s %s\n\n" % (test, msg))
            except Exception:
                pass

    def setAttemptNumber(self, attempt: int) -> None:
        self.attempt = attempt

    def __clearClient(self):
        if hasattr(self, "client"):
            self.client.close()
            delattr(self, "client")

    def _getTempDirName(self) -> str:
        name = self._getLongTestID()
        if self.attempt > 0:
            name += "-%d" % self.attempt
        return name

    def _getLongTestID(self) -> str:
        parallel = "sp"[int(self.parallelCrawl)]
        return "%s.%s.%s.%s" % (self.id(), self.transport, self.encoding, parallel)

    # pyrefly: ignore [bad-override]
    def run(self, result):
        if result is None:
            raise Exception("MUST be a runtests.py:Result instance")

        id = self._getLongTestID()
        try:
            self.__logTestInfo(id, "BEGIN")
            super(WatchmanTestCase, self).run(result)
        finally:
            try:
                self.watchmanCommand("log-level", "off")
                self.getClient().getLog(remove=True)
            except Exception:
                pass
            self.__logTestInfo(id, "END")
            self.__clearWatches()
            self.__clearClient()

        return result

    def dumpLogs(self) -> None:
        """used in travis CI to show the hopefully relevant log snippets"""

        print(self.getLogSample())

    def getLogSample(self) -> str:
        """used in CI to show the hopefully relevant log snippets"""
        inst = self.watchmanInstance()

        def tail(logstr, n):
            lines = logstr.split("\n")[-n:]
            return "\n".join(lines)

        return "\n".join(
            [
                "CLI logs",
                tail(inst.getCLILogContents(), 500),
                "Server logs",
                tail(inst.getServerLogContents(), 500),
            ]
        )

    def getServerLogContents(self):
        """
        Returns the contents of the server log file as an array
        that has already been split by line.
        """
        return self.watchmanInstance().getServerLogContents().split("\n")

    def setConfiguration(
        self, transport, encoding, parallelCrawl, splitWatcher
    ) -> None:
        self.transport = transport
        self.encoding = encoding
        self.parallelCrawl = parallelCrawl
        self.splitWatcher = splitWatcher

    def removeRelative(self, base, *fname) -> None:
        fname = os.path.join(base, *fname)
        os.remove(fname)

    def touch(self, fname, times=None) -> None:
        try:
            os.utime(fname, times)
        except OSError as e:
            if e.errno == errno.ENOENT:
                with open(fname, "a"):
                    os.utime(fname, times)
            else:
                raise

    def touchRelative(self, base, *fname) -> None:
        fname = os.path.join(base, *fname)
        self.touch(fname, None)

    def __clearWatches(self):
        if hasattr(self, "client"):
            try:
                self.client.subs = {}
                self.client.sub_by_root = {}
                self.watchmanCommand("watch-del-all")
            except Exception:
                pass

    def __del__(self) -> None:
        self.__clearWatches()

    def watchmanCommand(self, *args):
        client = self.getClient()
        return client.query(*args)

    def watchmanInstance(self):
        return WatchmanInstance.getSharedInstance(self.watchmanConfig())

    def watchmanConfig(self):
        """Watchman config for this test case"""
        config = {
            "enable_parallel_crawl": self.parallelCrawl,
            "prefer_split_fsevents_watcher": self.splitWatcher,
        }
        return config

    def _waitForCheck(self, cond, res_check, timeout: float):
        deadline = time.time() + timeout
        res = None
        while time.time() < deadline:
            Interrupt.checkInterrupt()
            res = cond()
            if res_check(res):
                return [True, res]
            time.sleep(0.03)
        return [False, res]

    # Continually invoke `cond` until it returns true or timeout
    # is reached.  Returns a tuple of [bool, result] where the
    # first element of the tuple indicates success/failure and
    # the second element is the return value from the condition
    def waitFor(self, cond, timeout=None):
        timeout = self.getTimeout(timeout)
        return self._waitForCheck(cond, lambda res: res, timeout)

    def waitForEqual(self, expected, actual_cond, timeout=None):
        timeout = self.getTimeout(timeout)
        return self._waitForCheck(actual_cond, lambda res: res == expected, timeout)

    def getTimeout(self, timeout=None) -> float:
        return timeout or self.socketTimeout

    def assertWaitFor(self, cond, timeout=None, message=None):
        timeout = self.getTimeout(timeout)
        status, res = self.waitFor(cond, timeout)
        if status:
            return res
        if message is None:
            message = "%s was not met in %s seconds: %s" % (cond, timeout, res)
        self.fail(message)

    def assertWaitForEqual(self, expected, actual_cond, timeout=None, message=None):
        timeout = self.getTimeout(timeout)
        status, res = self.waitForEqual(expected, actual_cond, timeout)
        if status:
            return res
        if message is None:
            message = "%s was not equal to %s in %s seconds: %s" % (
                actual_cond,
                expected,
                timeout,
                res,
            )
        self.fail(message)

    def getFileList(self, root, cursor=None, relativeRoot=None):
        expr = {"expression": ["exists"], "fields": ["name"]}
        if cursor:
            expr["since"] = cursor
        if relativeRoot:
            expr["relative_root"] = relativeRoot
        res = self.watchmanCommand("query", root, expr)
        files = res["files"]
        self.last_file_list = files
        return files

    def waitForSync(self, root) -> None:
        """ensure that watchman has observed any pending file changes
        This is most useful after mutating the filesystem and before
        attempting to perform a since query
        """
        self.watchmanCommand(
            "query", root, {"expression": ["name", "_bogus_"], "fields": ["name"]}
        )

    def getWatchList(self):
        watch_list = self.watchmanCommand("watch-list")["roots"]
        self.last_root_list = watch_list
        return watch_list

    def assertFileListsEqual(self, list1, list2, message=None) -> None:
        list1 = [norm_relative_path(f) for f in list1]
        list2 = [norm_relative_path(f) for f in list2]
        self.assertCountEqual(list1, list2, message)

    def fileListsEqual(self, list1, list2) -> bool:
        list1 = [norm_relative_path(f) for f in list1]
        list2 = [norm_relative_path(f) for f in list2]
        return sorted(list1) == sorted(list2)

    def fileListContains(self, list1, list2) -> bool:
        """return true if list1 contains each unique element in list2"""
        set1 = {norm_relative_path(f) for f in list1}
        list2 = [norm_relative_path(f) for f in list2]
        return set1.issuperset(list2)

    def assertFileListContains(self, list1, list2, message=None) -> None:
        if not self.fileListContains(list1, list2):
            message = "list1 %r should contain %r: %s" % (list1, list2, message)
            self.fail(message)

    # Wait for the file list to match the input set
    def assertFileList(
        self, root, files=None, cursor=None, relativeRoot=None, message=None
    ) -> None:
        expected_files = files or []
        if (cursor is not None) and cursor[0:2] == "n:":
            # it doesn't make sense to repeat named cursor queries, as
            # the cursor moves each time
            self.getFileList(root, cursor=cursor, relativeRoot=relativeRoot)
        else:
            st, res = self.waitFor(
                lambda: self.fileListsEqual(
                    self.getFileList(root, cursor=cursor, relativeRoot=relativeRoot),
                    expected_files,
                )
            )
        # pyre-fixme[16]: `WatchmanTestCase` has no attribute `last_file_list`.
        self.assertFileListsEqual(self.last_file_list, expected_files, message)

    def assertQueryRepsonseEqual(self, expected_resp, actual_resp) -> None:
        # converting the dict to a string version of the json is not
        # exactly the fastest function to use to sort, but it is unambiguous.
        sorted_expected = sorted(
            expected_resp, key=lambda x: json.dumps(x, sort_keys=True)
        )
        sorted_actual = sorted(actual_resp, key=lambda x: json.dumps(x, sort_keys=True))

        self.assertCountEqual(sorted_expected, sorted_actual)

    # Wait for the list of watched roots to match the input set
    def assertWatchListContains(self, roots, message=None) -> None:
        st, res = self.waitFor(
            lambda: self.fileListContains(self.getWatchList(), roots)
        )
        # pyre-fixme[16]: `WatchmanTestCase` has no attribute `last_root_list`.
        self.assertFileListContains(self.last_root_list, roots, message)

    def waitForSub(
        self, name, root, accept=None, timeout=None, remove: bool = True, client=None
    ):
        timeout = self.getTimeout(timeout)
        client = client or self.getClient()

        def default_accept(dat):
            return True

        if accept is None:
            accept = default_accept

        deadline = time.time() + timeout
        while time.time() < deadline:
            Interrupt.checkInterrupt()
            sub = self.getSubscription(name, root=root, remove=False, client=client)
            if sub is not None:
                res = accept(sub)
                if res:
                    return self.getSubscription(
                        name, root=root, remove=remove, client=client
                    )
            # wait for more data
            client.setTimeout(deadline - time.time())
            client.receive()

        return None

    def waitForSubFileList(self, name, root, fileList: List[str], timeout=None):
        def accept(subResults: List[Dict[str, Any]]) -> bool:
            if subResults is None:
                return False

            normalizedResult = self.normalizeFiles(subResults[-1])

            for file in fileList:
                if file not in normalizedResult["files"]:
                    return False

            return True

        return self.waitForSub(name, root, accept=accept, timeout=timeout)

    def getSubscription(
        self, name, root, remove: bool = True, normalize: bool = True, client=None
    ):
        client = client or self.getClient()
        data = client.getSubscription(name, root=root, remove=remove)

        if data is None or not normalize:
            return data

        return [self.normalizeFiles(sub) for sub in data]

    def normalizeFiles(self, data):
        def norm_item(item):
            if isinstance(item, STRING_TYPES):
                return norm_relative_path(item)
            item["name"] = norm_relative_path(item["name"])
            return item

        if "files" in data:
            files = []
            for item in data["files"]:
                files.append(norm_item(item))
            data["files"] = files

        return data

    def isCaseInsensitive(self):
        if hasattr(self, "_case_insensitive"):
            return self._case_insensitive
        d = self.mkdtemp()
        self.touchRelative(d, "a")
        self._case_insensitive = os.path.exists(os.path.join(d, "A"))
        return self._case_insensitive

    def suspendWatchman(self) -> None:
        self.watchmanInstance().suspend()

    def resumeWatchman(self) -> None:
        self.watchmanInstance().resume()

    @contextlib.contextmanager
    def watchmanSuspended(self) -> Iterator[None]:
        """Suspends watchman while the body runs."""
        self.suspendWatchman()
        try:
            yield
        finally:
            self.resumeWatchman()

    def rootIsWatched(self, r) -> bool:
        r = norm_absolute_path(r)
        watches = [
            norm_absolute_path(root)
            for root in self.watchmanCommand("watch-list")["roots"]
        ]
        return r in watches


def skip_for(transports=None, codecs=None):
    """
    Decorator to allow skipping tests for particular transports or codecs."""
    transports = set(transports or ())
    codecs = set(codecs or ())

    def skip(f):
        @functools.wraps(f)
        def wrapper(self, *args, **kwargs):
            if self.transport in transports or self.encoding in codecs:
                self.skipTest(
                    "test skipped for transport %s, codec %s"
                    % (self.transport, self.encoding)
                )
            return f(self, *args, **kwargs)

        return wrapper

    return skip


def expand_matrix(test_class) -> None:
    """
    A decorator function used to create different permutations from
    a given input test class.

    Given a test class named "MyTest", this will create 4 separate
    classes named "MyTestLocalBser", "MyTestLocalBser2",
    "MyTestLocalJson" and "MyTestCliJson" that will exercise the
    different transport and encoding options implied by their names.
    """

    matrix = [
        ("unix", "bser", "parallel", False, "UnixBser2"),
        ("unix", "json", "serial", False, "UnixJson"),
        ("cli", "json", "parallel", False, "CliJson"),
    ]

    if os.name == "nt":
        matrix += [("namedpipe", "bser", "serial", False, "NamedPipeBser2")]

    if sys.platform == "darwin":
        matrix += [
            ("unix", "bser", "parallel", True, "KQueueAndFSEventsUnixBser2"),
        ]

    # We do some rather hacky things here to define new test class types
    # in our caller's scope.  This is needed so that the unittest TestLoader
    # will find the subclasses we define.
    # pyre-fixme[16]: Optional type has no attribute `f_back`.
    caller_scope = inspect.currentframe().f_back.f_locals

    def make_class(transport, encoding, suffix, parallel, split):
        subclass_name = test_class.__name__ + suffix

        # Define a new class that derives from the input class
        class MatrixTest(test_class):
            def setDefaultConfiguration(self):
                self.setConfiguration(transport, encoding, parallel, split)

        # Set the name and module information on our new subclass
        MatrixTest.__name__ = subclass_name
        MatrixTest.__qualname__ = subclass_name
        MatrixTest.__module__ = test_class.__module__

        # Before we publish the test, check whether that generated
        # configuration would always skip
        try:
            t = MatrixTest()
            t.checkPersistentSession()
            t.checkOSApplicability()
            caller_scope[subclass_name] = MatrixTest
        except unittest.SkipTest:
            pass

    for transport, encoding, parallel, split, suffix in matrix:
        make_class(transport, encoding, suffix, parallel == "parallel", split)

    return None
