#!/usr/bin/env python3
# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import argparse
import json
import math
import multiprocessing
import os
import os.path
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import unittest


# in the FB internal test infra, ensure that we are running from the
# dir that houses this script rather than some other higher level dir
# in the containing tree.  We can't use __file__ to determine this
# because our PAR machinery can generate a name like /proc/self/fd/3/foo
# which won't resolve to anything useful by the time we get here.
if not os.path.exists("runtests.py") and os.path.exists("watchman/runtests.py"):
    os.chdir("watchman")

# Ensure that we can find pywatchman and integration tests (if we're not the
# main module, a wrapper is probably loading us up and we shouldn't screw around
# with sys.path).
if __name__ == "__main__":
    sys.path.insert(0, os.path.join(os.getcwd(), "python"))
    sys.path.insert(1, os.path.join(os.getcwd(), "integration"))
    sys.path.insert(1, os.path.join(os.getcwd(), "integration", "facebook"))


# Only Python 3.5+ supports native asyncio
has_asyncio = sys.version_info >= (3, 5)
if has_asyncio:
    sys.path.insert(0, os.path.join(os.getcwd(), "tests", "async"))
    import asyncio

try:
    import queue
except Exception:
    import Queue

    queue = Queue

parser = argparse.ArgumentParser(
    description="Run the watchman unit and integration tests"
)
parser.add_argument("-v", "--verbosity", default=2, help="test runner verbosity")
parser.add_argument(
    "--keep",
    action="store_true",
    help="preserve all temporary files created during test execution",
)
parser.add_argument(
    "--keep-if-fail",
    action="store_true",
    help="preserve all temporary files created during test execution if failed",
)

parser.add_argument("files", nargs="*", help="specify which test files to run")

parser.add_argument(
    "--method", action="append", help="specify which python test method names to run"
)


def default_concurrency():
    # Python 2.7 hangs when we use threads, so avoid it
    # https://bugs.python.org/issue20318
    if sys.version_info >= (3, 0):
        level = min(4, math.ceil(1.5 * multiprocessing.cpu_count()))
        if "CIRCLECI" in os.environ:
            # Use fewer cores in circle CI because the inotify sysctls
            # are pretty low, and we sometimes hit those limits.
            level = level / 2
        return int(level)

    return 1


parser.add_argument(
    "--concurrency",
    default=default_concurrency(),
    type=int,
    help="How many tests to run at once",
)

parser.add_argument(
    "--watcher",
    action="store",
    default="auto",
    help="Specify which watcher should be used to run the tests",
)

parser.add_argument(
    "--debug-watchman",
    action="store_true",
    help="Pauses start up and prints out the PID for watchman server process."
    + "Forces concurrency to 1.",
)

parser.add_argument(
    "--watchman-path", action="store", help="Specify the path to the watchman binary"
)

parser.add_argument(
    "--win7", action="store_true", help="Set env to force win7 compatibility tests"
)

parser.add_argument(
    "--retry-flaky",
    action="store",
    type=int,
    default=2,
    help="How many additional times to retry flaky tests.",
)

parser.add_argument(
    "--testpilot-json",
    action="store_true",
    help="Output test results in Test Pilot JSON format",
)

parser.add_argument(
    "--pybuild-dir",
    action="store",
    help="For out-of-src-tree builds, where the generated python lives",
)

args = parser.parse_args()

if args.pybuild_dir is not None:
    sys.path.insert(0, os.path.realpath(args.pybuild_dir))

# Import our local stuff after we've had a chance to look at args.pybuild_dir.
# The `try` block prevents the imports from being reordered
try:
    import Interrupt
    import pywatchman
    import TempDir
    import WatchmanInstance
except ImportError:
    raise

# We test for this in a test case
os.environ["WATCHMAN_EMPTY_ENV_VAR"] = ""
os.environ["HGUSER"] = "John Smith <smith@example.com>"
os.environ["NOSCMLOG"] = "1"
os.environ["WATCHMAN_NO_SPAWN"] = "1"

if args.win7:
    os.environ["WATCHMAN_WIN7_COMPAT"] = "1"

# Ensure that we find the watchman we built in the tests
if args.watchman_path:
    args.watchman_path = os.path.realpath(args.watchman_path)
    bin_dir = os.path.dirname(args.watchman_path)
    os.environ["WATCHMAN_BINARY"] = args.watchman_path
else:
    bin_dir = os.path.dirname(__file__)

os.environ["PYWATCHMAN_PATH"] = os.path.join(os.getcwd(), "python")
os.environ["PATH"] = "%s%s%s" % (
    os.path.abspath(bin_dir),
    os.pathsep,
    os.environ["PATH"],
)

# We'll put all our temporary stuff under one dir so that we
# can clean it all up at the end
temp_dir = TempDir.get_temp_dir(args.keep)


def interrupt_handler(signo, frame):
    Interrupt.setInterrupted()


signal.signal(signal.SIGINT, interrupt_handler)


class Result(unittest.TestResult):
    # Make it easier to spot success/failure by coloring the status
    # green for pass, red for fail and yellow for skip.
    # also print the elapsed time per test
    transport = None
    encoding = None
    attempt = 0

    def shouldStop(self):
        if Interrupt.wasInterrupted():
            return True
        return super(Result, self).shouldStop()

    def startTest(self, test):
        self.startTime = time.time()
        super(Result, self).startTest(test)

    def addSuccess(self, test):
        elapsed = time.time() - self.startTime
        super(Result, self).addSuccess(test)
        if args.testpilot_json:
            print(
                json.dumps(
                    {
                        "op": "test_done",
                        "status": "passed",
                        "test": test.id(),
                        "start_time": self.startTime,
                        "end_time": time.time(),
                    }
                )
            )
        else:
            print(
                "\033[32mPASS\033[0m %s (%.3fs)%s"
                % (test.id(), elapsed, self._attempts())
            )

    def addSkip(self, test, reason):
        elapsed = time.time() - self.startTime
        super(Result, self).addSkip(test, reason)
        if args.testpilot_json:
            print(
                json.dumps(
                    {
                        "op": "test_done",
                        "status": "skipped",
                        "test": test.id(),
                        "details": reason,
                        "start_time": self.startTime,
                        "end_time": time.time(),
                    }
                )
            )
        else:
            print("\033[33mSKIP\033[0m %s (%.3fs) %s" % (test.id(), elapsed, reason))

    def __printFail(self, test, err):
        elapsed = time.time() - self.startTime
        t, val, trace = err
        if args.testpilot_json:
            print(
                json.dumps(
                    {
                        "op": "test_done",
                        "status": "failed",
                        "test": test.id(),
                        "details": "".join(traceback.format_exception(t, val, trace)),
                        "start_time": self.startTime,
                        "end_time": time.time(),
                    }
                )
            )
        else:
            print(
                "\033[31mFAIL\033[0m %s (%.3fs)%s\n%s"
                % (
                    test.id(),
                    elapsed,
                    self._attempts(),
                    "".join(traceback.format_exception(t, val, trace)),
                )
            )

    def addFailure(self, test, err):
        self.__printFail(test, err)
        super(Result, self).addFailure(test, err)

    def addError(self, test, err):
        self.__printFail(test, err)
        super(Result, self).addError(test, err)

    def setAttemptNumber(self, attempt):
        self.attempt = attempt

    def _attempts(self):
        if self.attempt > 0:
            return " (%d attempts)" % self.attempt
        return ""


def expandFilesList(files):
    """expand any dir names into a full list of files"""
    res = []
    for g in args.files:
        if os.path.isdir(g):
            for dirname, _dirs, files in os.walk(g):
                for f in files:
                    if not f.startswith("."):
                        res.append(os.path.normpath(os.path.join(dirname, f)))
        else:
            res.append(os.path.normpath(g))
    return res


if args.files:
    args.files = expandFilesList(args.files)


def shouldIncludeTestFile(filename):
    """used by our loader to respect the set of tests to run"""
    global args
    fname = os.path.relpath(filename.replace(".pyc", ".py"))
    if args.files:
        for f in args.files:
            if f == fname:
                return True
        return False

    if args.method:
        # implies python tests only
        if not fname.endswith(".py"):
            return False

    return True


def shouldIncludeTestName(name):
    """used by our loader to respect the set of tests to run"""
    global args
    if args.method:
        for f in args.method:
            if f in name:
                # the strict original interpretation of this flag
                # was pretty difficult to use in practice, so we
                # now also allow substring matches against the
                # entire test name.
                return True
        return False
    return True


class Loader(unittest.TestLoader):
    """allows us to control the subset of which tests are run"""

    def __init__(self):
        super(Loader, self).__init__()

    def loadTestsFromTestCase(self, testCaseClass):
        return super(Loader, self).loadTestsFromTestCase(testCaseClass)

    def getTestCaseNames(self, testCaseClass):
        names = super(Loader, self).getTestCaseNames(testCaseClass)
        return filter(lambda name: shouldIncludeTestName(name), names)

    def loadTestsFromModule(self, module, *args, **kw):
        if not shouldIncludeTestFile(module.__file__):
            return unittest.TestSuite()
        return super(Loader, self).loadTestsFromModule(module, *args, **kw)


loader = Loader()
suite = unittest.TestSuite()

directories = ["python/tests", "integration"]
facebook_directory = "integration/facebook"
if os.path.exists(facebook_directory):
    # the facebook dir isn't sync'd to github, but it
    # is present internally, so it should remain in this list
    directories += [facebook_directory]

if has_asyncio:
    directories += ["tests/async"]

for d in directories:
    suite.addTests(loader.discover(d, top_level_dir=d))

if os.name == "nt":
    t_globs = "tests/*.exe"
else:
    t_globs = "tests/*.t"

tls = threading.local()


# Manage printing from concurrent threads
# http://stackoverflow.com/a/3030755/149111
class ThreadSafeFile:
    def __init__(self, f):
        self.f = f
        self.lock = threading.RLock()
        self.nesting = 0

    def _getlock(self):
        self.lock.acquire()
        self.nesting += 1

    def _droplock(self):
        nesting = self.nesting
        self.nesting = 0
        for _ in range(nesting):
            self.lock.release()

    def __getattr__(self, name):
        if name == "softspace":
            return tls.softspace
        else:
            raise AttributeError(name)

    def __setattr__(self, name, value):
        if name == "softspace":
            tls.softspace = value
        else:
            return object.__setattr__(self, name, value)

    def write(self, data):
        self._getlock()
        self.f.write(data)
        if data == "\n":
            self._droplock()

    def flush(self):
        self._getlock()
        self.f.flush()
        self._droplock()


sys.stdout = ThreadSafeFile(sys.stdout)

tests_queue = queue.Queue()
results_queue = queue.Queue()


def runner():
    global results_queue
    global tests_queue

    broken = False
    try:
        # Start up a shared watchman instance for the tests.
        inst = WatchmanInstance.Instance(
            {"watcher": args.watcher}, debug_watchman=args.debug_watchman
        )
        inst.start()
        # Allow tests to locate this default instance
        WatchmanInstance.setSharedInstance(inst)

        if has_asyncio:
            # Each thread will have its own event loop
            asyncio.set_event_loop(asyncio.new_event_loop())

    except Exception as e:
        print("while starting watchman: %s" % str(e))
        traceback.print_exc()
        broken = True

    while not broken:
        test = tests_queue.get()
        try:
            if test == "terminate":
                break

            if Interrupt.wasInterrupted() or broken:
                continue

            result = None
            for attempt in range(0, args.retry_flaky + 1):
                # Check liveness of the server
                try:
                    client = pywatchman.client(timeout=3.0, sockpath=inst.getSockPath())
                    client.query("version")
                    client.close()
                except Exception as exc:
                    print(
                        "Failed to connect to watchman server: %s; starting a new one"
                        % exc
                    )

                    try:
                        inst.stop()
                    except Exception:
                        pass

                    try:
                        inst = WatchmanInstance.Instance(
                            {"watcher": args.watcher},
                            debug_watchman=args.debug_watchman,
                        )
                        inst.start()
                        # Allow tests to locate this default instance
                        WatchmanInstance.setSharedInstance(inst)
                    except Exception as e:
                        print("while starting watchman: %s" % str(e))
                        traceback.print_exc()
                        broken = True
                        continue

                try:
                    result = Result()
                    result.setAttemptNumber(attempt)

                    if hasattr(test, "setAttemptNumber"):
                        test.setAttemptNumber(attempt)

                    test.run(result)

                    if hasattr(test, "setAttemptNumber") and not result.wasSuccessful():
                        # Facilitate retrying this possibly flaky test
                        continue

                    break
                except Exception as e:
                    print(e)

                    if hasattr(test, "setAttemptNumber") and not result.wasSuccessful():
                        # Facilitate retrying this possibly flaky test
                        continue

            if (
                not result.wasSuccessful()
                and "TRAVIS" in os.environ
                and hasattr(test, "dumpLogs")
            ):
                test.dumpLogs()

            results_queue.put(result)

        finally:
            tests_queue.task_done()

    if not broken:
        inst.stop()


def expand_suite(suite, target=None):
    """recursively expand a TestSuite into a list of TestCase"""
    if target is None:
        target = []
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            expand_suite(test, target)
        else:
            target.append(test)

    # randomize both because we don't want tests to have relatively
    # dependency ordering and also because this can help avoid clumping
    # longer running tests together
    random.shuffle(target)
    return target


def queue_jobs(tests):
    for test in tests:
        tests_queue.put(test)


all_tests = expand_suite(suite)
if args.debug_watchman:
    args.concurrency = 1
elif len(all_tests) < args.concurrency:
    args.concurrency = len(all_tests)
queue_jobs(all_tests)

if args.concurrency > 1:
    for _ in range(args.concurrency):
        t = threading.Thread(target=runner)
        t.daemon = True
        t.start()
        # also send a termination sentinel
        tests_queue.put("terminate")

    # Wait for all tests to have been dispatched
    tests_queue.join()
else:
    # add a termination sentinel
    tests_queue.put("terminate")
    runner()

# Now pull out and aggregate the results
tests_run = 0
tests_failed = 0
tests_skipped = 0
while not results_queue.empty():
    res = results_queue.get()
    tests_run = tests_run + res.testsRun
    tests_failed = tests_failed + len(res.errors) + len(res.failures)
    tests_skipped = tests_skipped + len(res.skipped)

if not args.testpilot_json:
    print(
        "Ran %d, failed %d, skipped %d, concurrency %d"
        % (tests_run, tests_failed, tests_skipped, args.concurrency)
    )

if "APPVEYOR" in os.environ:
    logdir = "logs7" if args.win7 else "logs"
    logzip = "%s.zip" % logdir
    shutil.copytree(tempfile.tempdir, logdir)
    subprocess.call(["7z", "a", logzip, logdir])
    subprocess.call(["appveyor", "PushArtifact", logzip])

if "CIRCLE_ARTIFACTS" in os.environ:
    print("Creating %s/logs.zip" % os.environ["CIRCLE_ARTIFACTS"])
    subprocess.call(
        [
            "zip",
            "-q",
            "-r",
            "%s/logs.zip" % os.environ["CIRCLE_ARTIFACTS"],
            temp_dir.get_dir(),
        ]
    )

if tests_failed or (tests_run == 0):
    if args.keep_if_fail:
        temp_dir.set_keep(True)
    if args.testpilot_json:
        # When outputting JSON, our return code indicates if we successfully
        # produced output or not, not whether the tests passed.  The JSON
        # output contains the detailed test pass/failure information.
        sys.exit(0)
    sys.exit(1)
