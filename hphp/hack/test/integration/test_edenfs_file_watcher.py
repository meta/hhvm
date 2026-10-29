"""

This file tests the Edenfs_watcher module.

Note that like all the other tests in this directory, it is run *without* the
run_as_bundle configuration flag on its python_unittest definition in TARGETS.
This means that the distinction beween setUp vs setUpClass (and tearDown vs
tearDownClass) is insignificant. Therefore, we just follow CommonTestDriver to
decide what setup and teardown code goes where.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, List, Optional, Tuple

import hphp.hack.test.integration.common_tests as common_tests
from hphp.hack.test.integration.common_tests import CommonTestDriver
from hphp.hack.test.integration.eden_test_driver import EdenTestDriver
from hphp.hack.test.integration.hh_paths import hh_client, hh_server
from hphp.hack.test.integration.test_case import TestCase


# Test states used in deferral tests.
TEST_STATE_0 = "hh.state-tracking-test-state0"
TEST_STATE_1 = "hh.state-tracking-test-state1"


@dataclass
class Config:
    streaming_errors: bool
    state_tracking: bool
    tracked_states: tuple[str, ...] = (TEST_STATE_0, TEST_STATE_1)
    interruptions: bool = True
    throttle_time_ms: int = 50
    obey_deferral: bool = True
    block_connections: bool = True
    hg_aware: bool = False

    def write_hhconf(self, output_folder: str) -> None:
        streaming_errors = str(self.streaming_errors).lower()
        throttle_time_ms = str(self.throttle_time_ms)
        interruptions = str(self.interruptions).lower()
        state_tracking = str(self.state_tracking).lower()
        obey_deferral = str(self.obey_deferral).lower()
        block_connections = str(self.block_connections).lower()
        hg_aware = str(self.hg_aware).lower()

        config = f"""
min_log_level = Debug
use_watchman = false
interrupt_on_file_changes = {interruptions}
interrupt_on_client = {interruptions}
edenfs_file_watcher_enabled = true
edenfs_file_watcher_throttle_time_ms = {throttle_time_ms}
edenfs_file_watcher_sync_queries_obey_deferral = {obey_deferral}
block_client_connections_while_deferring = {block_connections}
hg_aware = {hg_aware}
edenfs_file_watcher_state_tracking = {state_tracking}
edenfs_file_watcher_tracked_states = {", ".join(self.tracked_states)}
produce_streaming_errors = {streaming_errors}
consume_streaming_errors = {streaming_errors}
"""

        print("Writing config:\n", config)
        with open(os.path.join(output_folder, "hh.conf"), "w") as f:
            f.write(config)


# Matches the message that is logged by ServerNotifier.get_changes_sync and
# get_changes_async when it saw a non-zero number of changes. We look at the server log
# to test if the server did or didn't pick up some changes.
#
# While this approach is very brittle, we do have test_server_notifier_re in place to
# check that the RE still works.
SERVER_NOTIFIER_RE = r"ServerNotifier\.get_changes_(sync|async) got (\d+) changes"

# Used to debug this test suite.
# If enabled, we launch hh_server in the foreground and
# redirect its stdout and stderr into this process'.
# Make sure that this is never enabled by default, it disabled some testing
# assertions!
DEBUG_EDENFS_WATCHER_TEST_HH_SERVER_FOREGROUND: bool = (
    os.environ.get("DEBUG_EDENFS_WATCHER_TEST_HH_SERVER_FOREGROUND") is not None
)


def assertCurrentServerLogContains(driver: CommonTestDriver, needle: str) -> None:
    if DEBUG_EDENFS_WATCHER_TEST_HH_SERVER_FOREGROUND:
        # We don't have access to any logs
        return

    server_log = driver.get_all_logs(driver.repo_dir).current_server_log
    contains = needle in server_log
    if not contains:
        print("Server log:")
        print(server_log)
    driver.assertTrue(contains)


def waitForCurrentServerLogContains(
    driver: CommonTestDriver, needle: str, timeout_secs: float = 10
) -> str:
    """Wait for a message from the current server, ignoring logs from older runs.

    State-assertion tests use this to establish that hh_server has observed the
    state before launching a client, not merely that Eden has accepted it.
    """
    if DEBUG_EDENFS_WATCHER_TEST_HH_SERVER_FOREGROUND:
        driver.skipTest("This test requires the server log")
    stdout, _, retcode = driver.proc_call(
        [hh_client, "--logname", driver.repo_dir], log=False
    )
    driver.assertEqual(retcode, 0)
    server_log_path = Path(stdout.strip())
    deadline = time.monotonic() + timeout_secs
    while True:
        server_log = server_log_path.read_text()
        if needle in server_log:
            return server_log
        if time.monotonic() >= deadline:
            driver.fail(
                f"Timed out waiting for {needle!r} in server log:\n{server_log}"
            )
        time.sleep(0.1)


def assertAnyServerLogContains(driver: CommonTestDriver, needle: str) -> None:
    server_log = driver.get_all_logs(driver.repo_dir).all_server_logs
    contains = needle in server_log
    if not contains:
        print("Server log:")
        print(server_log)
    driver.assertTrue(contains)


def assertMonitorLogContains(driver: CommonTestDriver, needle: str) -> None:
    if DEBUG_EDENFS_WATCHER_TEST_HH_SERVER_FOREGROUND:
        # We don't have access to any logs
        return
    monitor_log = driver.get_all_logs(driver.repo_dir).all_monitor_logs

    driver.assertTrue(needle in monitor_log)


def waitForServerExitStatus(
    driver: CommonTestDriver,
    monitor_log_path: Path,
    status: str,
    timeout_secs: float = 30,
) -> None:
    """Wait until the monitor logs a server exit with the specified status."""
    deadline = time.monotonic() + timeout_secs
    while True:
        monitor_log = monitor_log_path.read_text()
        for line in monitor_log.splitlines():
            if "TYPECHECKER_EXIT " in line and status in line:
                return
        if time.monotonic() >= deadline:
            driver.fail(
                f"Timed out waiting for {status} in monitor log:\n{monitor_log}"
            )
        time.sleep(0.1)


def assertEdenFsWatcherInitialized(driver: CommonTestDriver) -> None:
    """Checks that an Edenfs_watcher instance was initialized

    Given that we may fall back to using dfind if Edenfs_watcher
    initialization fails, this prevents tests from passing even though the
    EdenFS watcher wasn't actually used.
    """
    assertCurrentServerLogContains(driver, "[edenfs_watcher][init] finished init")


def assertServerNotCrashed(driver: CommonTestDriver) -> None:
    if DEBUG_EDENFS_WATCHER_TEST_HH_SERVER_FOREGROUND:
        # We don't have access to any logs
        return
    monitor_log = driver.get_all_logs(driver.repo_dir).all_monitor_logs

    driver.assertFalse("Exit_status.Edenfs_watcher_failed" in monitor_log)
    driver.assertFalse("Exit_status.Edenfs_watcher_eden_shutting_down" in monitor_log)


def assertServerNotifierChangesYes(driver: CommonTestDriver) -> None:
    if DEBUG_EDENFS_WATCHER_TEST_HH_SERVER_FOREGROUND:
        # We don't have access to any logs
        return
    server_log = driver.get_all_logs(driver.repo_dir).current_server_log
    matches = re.search(SERVER_NOTIFIER_RE, server_log)
    driver.assertTrue(matches is not None)


def assertServerNotifierChangesNo(driver: CommonTestDriver) -> None:
    if DEBUG_EDENFS_WATCHER_TEST_HH_SERVER_FOREGROUND:
        # We don't have access to any logs
        return
    # Not that in the "No" version, we check all logs
    server_log = driver.get_all_logs(driver.repo_dir).all_server_logs
    matches = re.search(SERVER_NOTIFIER_RE, server_log)
    driver.assertTrue(matches is None)


class EdenfsWatcherInitDiagnosisTestDriver(CommonTestDriver):
    """Uses ordinary directories and allows dfind fallback after Eden init fails."""

    def setUp(self) -> None:
        super().setUp()
        Path(self.repo_dir, "hh.conf").write_text(
            "edenfs_file_watcher_enabled = true\n"
        )


class EdenfsWatcherInitDiagnosisTests(TestCase[EdenfsWatcherInitDiagnosisTestDriver]):
    @classmethod
    def get_test_driver(cls) -> EdenfsWatcherInitDiagnosisTestDriver:
        return EdenfsWatcherInitDiagnosisTestDriver()

    def test_non_eden_sapling_repo_is_non_eden(self) -> None:
        """A valid Sapling checkout without .eden gets the non-Eden classification."""
        driver = self.test_driver
        _, stderr, exit_code = driver.proc_call(["hg", "init", driver.repo_dir])
        self.assertEqual(exit_code, 0, stderr)
        commit_id, stderr, exit_code = driver.proc_call(
            ["hg", "--cwd", driver.repo_dir, "whereami"]
        )
        self.assertEqual(exit_code, 0, stderr)
        self.assertFalse(os.path.lexists(Path(driver.repo_dir, ".eden")))

        driver.start_hh_server()
        driver.check_cmd(["No errors!"])
        server_log = driver.get_all_logs(driver.repo_dir).current_server_log
        self.assertIn(f"Sapling commit {commit_id.strip()} verified", server_log)
        self.assertIn(
            "Failed to initialize EdenFS watcher, "
            f"www repo {driver.repo_dir} is not on Eden",
            server_log,
        )

    def test_a_move_is_not_refused_when_the_server_fell_back_to_dfind(self) -> None:
        """The setting is true but the repo is not on Eden, so the server uses
        dfind, which does report the moved directory."""
        driver = self.test_driver
        source = Path(driver.repo_dir, "to_move")
        source.mkdir()
        (source / "a.php").write_text("<?hh\nfunction a(): void {}\n")

        driver.start_hh_server()
        stdout, _, exit_code = driver.run_check(
            options=[
                "--isolation",
                "--isolation-seed-dir",
                "to_move",
                "--isolation-move-to",
                "move_dest",
                "--json",
            ],
        )

        self.assertEqual(exit_code, 0, stdout)
        self.assertTrue(json.loads(stdout)["moved"], stdout)
        self.assertTrue(
            Path(driver.repo_dir, "move_dest", "to_move", "a.php").is_file()
        )
        self.assertFalse(source.exists())


class EdenfsWatcherTestDriver(EdenTestDriver):
    @classmethod
    def getConfig(cls) -> Config:
        return Config(streaming_errors=False, state_tracking=False)

    @classmethod
    def prepare_initial_commit(cls, output_folder: str) -> list[str]:
        # The hh.conf file can go wherever, as long as HH_LOCALCONF_PATH points to
        # it. Some other tests expect it to be inside the folder cls.repo_dir.
        # So let's put it in the place that after mounting will end up at cls.repo_dir
        cls.getConfig().write_hhconf(output_folder)
        return super().prepare_initial_commit(output_folder) + ["hh.conf"]

    def tearDown(self) -> None:
        print("running EdenfsWatcherDriver.tearDown")

        print(self.get_all_logs(self.repo_dir).all_server_logs)

        # It's ugly to do this here, since this isn't really tear-down code,
        # but core testing logic, but we need to get this into every test
        assertEdenFsWatcherInitialized(self)

        # Similarly, another check: hh_server's resiliance is sometimes hiding bugs:
        # We may accidentally crash the Edenfs_watcher, which can take the server down.
        # But then the monitor restarts it and it re-checks the testing repo.
        # From the outside, that looks very similar to the server correctly
        # picking up a change we've made to the testing repo!
        assertServerNotCrashed(self)

        # For hygiene, we (re-)mount our Eden repo for each individual test.
        # Note that we must stop the server before unmounting the Eden repo that it works on.
        # 3 retries is the value used in CommonTestDriver.tearDown
        self.stop_hh_server(retries=3)
        super().tearDown()

    def start_hh_server(
        self,
        changed_files: Optional[List[str]] = None,
        saved_state_path: Optional[str] = None,
        args: Optional[List[str]] = None,
        wait_for_server: bool = True,
    ) -> None:
        args = args or []
        if DEBUG_EDENFS_WATCHER_TEST_HH_SERVER_FOREGROUND:
            cmd = [hh_server, "--max-procs", "2", self.repo_dir] + args
            subprocess.Popen(
                cmd,
                env=self.test_env,
                stdout=sys.stderr,
                stderr=sys.stderr,
                universal_newlines=True,
            )
            if wait_for_server:
                self.wait_until_server_ready()
        else:
            super().start_hh_server(
                changed_files, saved_state_path, args, wait_for_server
            )

    @classmethod
    def isMountPointIgnored(cls) -> bool:
        """Returns True if cls.eden_mount_point is not watched directly

        We always have one of the following:
        1. cls.repo_dir is identical to cls.eden_mount_point or
        2. cls.repo_dir is different from, but an ancestor of, cls.eden_mount_point

        In the second case, changes that happen within cls.eden_mount_point, but outside
        cls.repo_dir, are ignored.

        Note that this check is identical to asking if a test is running with
        EdenfsWatcherTestDriver or EdenfsWatcherNonMountPointTestDriver
        """

        return Path(cls.eden_mount_point).absolute() != Path(cls.repo_dir).absolute()

    @classmethod
    def createNonHackFile(cls, path: str, base: Optional[str] = None) -> None:
        """Creates a file that won't type-check"""

        base = base or cls.repo_dir
        full_path = os.path.join(base, path)
        os.makedirs(os.path.dirname(full_path), exist_ok=True)

        with open(full_path, "w") as f:
            f.write("Not a valid hack file")

    @classmethod
    def createIgnoredFiles(cls) -> None:
        cls.createNonHackFile("randomfileweshouldignore")
        cls.createNonHackFile("almost.hhconfig")
        cls.createNonHackFile(".php")
        cls.createNonHackFile("folder.php/ignored_file.txt")
        cls.createNonHackFile(".hg/ignored.php", base=cls.eden_mount_point)
        if cls.isMountPointIgnored():
            cls.createNonHackFile("ignored.php", base=cls.eden_mount_point)

    def assertStateForSeconds(
        self, state_name: str, duration_secs: float
    ) -> Tuple[Callable[[], None], Callable[[], bool], Callable[[], None]]:
        """Asserts the given state for the given number of seconds with Eden.

        This function returns as soon as the state is asserted.
        The state then remains asserted in the background.

        Returns three thunks:
        - Calling the first one waits until the state is de-asserted
        - Calling the second returns a boolean indicating whether the state is
           still asserted.
        - Calling the third kills the asserter process with SIGKILL.
        """

        eden_state_asserter_path = os.getenv("HH_EDEN_TEST_STATE_ASSERTER", None)
        if eden_state_asserter_path is None:
            raise Exception("HH_EDEN_TEST_STATE_ASSERTER not set")

        proc: subprocess.Popen[str] = subprocess.Popen(
            [
                eden_state_asserter_path,
                self.eden_mount_point,
                state_name,
                str(duration_secs),
            ],
            universal_newlines=True,
            stdout=subprocess.PIPE,
        )

        # We want to wait until the process is running and has actually asserted the state
        stdout = proc.stdout
        if stdout is None:
            raise Exception("Could not access stdout of state asserter executable")
        line = stdout.readline().strip()
        self.assertEqual(line, "state asserted")

        # ... but we won't wait until actual deassertion. That's what this thunk can be used for.
        def wait_and_check() -> None:
            retcode = proc.wait()
            self.assertEqual(retcode, 0)

        def is_still_asserted() -> bool:
            return proc.poll() is None

        def kill_asserter() -> None:
            proc.kill()
            # reap: the asserter is gone (and its EdenFS connection closed) before we return
            proc.wait()

        return (wait_and_check, is_still_asserted, kill_asserter)

    def check_cmd_eventually(
        self,
        expected_output: List[str],
        retries: int = 30,
        retry_delay_secs: float = 0.5,
    ) -> None:
        """Like check_cmd, but polls `hh check` until its output matches.

        check_cmd checks exactly once, which is racy for behavior that hh only
        reaches after observing an asynchronous EdenFS event. In particular,
        when a state-asserting process crashes, EdenFS notices that its
        connection closed - and hence de-asserts the state - only some time
        later. hh stops deferring and processes the changes made in the meantime
        only once it observes that de-assertion. We poll until that
        eventually-consistent state is reached; if it never is, the final
        check_cmd call fails the test with a readable diff.
        """
        root = self.repo_dir + os.path.sep
        expected_lines = sorted(line.format(root=root) for line in expected_output)
        for _ in range(retries):
            (output, _err, _retcode) = self.run_check()
            if sorted(output.splitlines()) == expected_lines:
                return
            time.sleep(retry_delay_secs)
        # Out of retries: run check_cmd once more so the test fails with a
        # useful expected-vs-actual diff.
        self.check_cmd(expected_output)


class EdenfsWatcherNonMountPointTestDriver(EdenfsWatcherTestDriver):
    """Uses a Hack root below the Eden mount, with streaming and state tracking enabled."""

    @classmethod
    def getConfig(cls) -> Config:
        return Config(streaming_errors=True, state_tracking=True)

    @classmethod
    def setUpClass(cls, template_repo: str) -> None:
        cls.setUpClassImpl(template_repo, "some/sub/folder")


class EdenfsWatcherShutdownTestDriver(EdenfsWatcherTestDriver):
    """Owns a foreground monitor for a test that intentionally stops Eden."""

    _monitor: subprocess.Popen[str] | None = None
    log_path: Path

    def setUp(self) -> None:
        super().setUp()
        self._monitor = None
        self.log_path = Path(self.hh_tmp_dir, "shutdown_hh_server.log")

    def start_hh_server(
        self,
        changed_files: list[str] | None = None,
        saved_state_path: str | None = None,
        args: list[str] | None = None,
        wait_for_server: bool = True,
    ) -> None:
        self.assertIsNone(self._monitor)
        # Both monitor and server inherit this output file, which survives Eden stop.
        with self.log_path.open("w") as log_file:
            monitor = subprocess.Popen(
                [hh_server, "--max-procs", "2", self.repo_dir] + (args or []),
                env=self.test_env,
                stdin=subprocess.DEVNULL,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                text=True,
            )
        self._monitor = monitor
        if wait_for_server:
            self.wait_until_server_ready()
        self.assertIsNone(monitor.poll(), self.log_path.read_text())

    def run_check(
        self, stdin: str | None = None, options: list[str] | None = None
    ) -> tuple[str, str, int]:
        # Clients must use the monitor owned by this driver rather than start another.
        return super().run_check(
            stdin=stdin, options=(options or []) + ["--autostart-server", "false"]
        )

    def tearDown(self) -> None:
        monitor = self._monitor
        if monitor is None:
            return
        try:
            monitor.terminate()
            monitor.wait(timeout=30)
        except subprocess.TimeoutExpired:
            monitor.kill()
            monitor.wait(timeout=30)
            raise
        finally:
            print(self.log_path.read_text())


class EdenfsWatcherShutdownTests(TestCase[EdenfsWatcherShutdownTestDriver]):
    @classmethod
    def get_test_driver(cls) -> EdenfsWatcherShutdownTestDriver:
        return EdenfsWatcherShutdownTestDriver()

    def test_eden_stop_uses_shutdown_exit_status(self) -> None:
        """`eden stop` logs shutdown distinctly and uses its dedicated exit status."""
        driver = self.test_driver
        driver.start_hh_server()
        driver.check_cmd(["No errors!"])
        # Establish that the server used Eden rather than falling back to dfind.
        self.assertIn(
            "[edenfs_watcher][init] finished init", driver.log_path.read_text()
        )

        # EdenFS.shutdown runs `eden stop -t 0` against this test's isolated daemon.
        driver.eden_instance.shutdown()
        waitForServerExitStatus(
            driver, driver.log_path, "Exit_status.Edenfs_watcher_eden_shutting_down"
        )
        self.assertIn(
            "Edenfs_watcher detected Eden shutting down:",
            driver.log_path.read_text(),
        )


class EdenfsWatcherTests(common_tests.CommonTests):
    "Runs integration tests on an Eden-backed testing repo with edenfs_file_watcher_enabled set"

    @classmethod
    def get_test_driver(cls) -> common_tests.CommonTestDriver:
        return EdenfsWatcherTestDriver()

    # Need to duplicate this here from TestCase to make pyre happy
    # pyrefly: ignore [bad-override]
    _test_driver: Optional[EdenfsWatcherTestDriver] = None

    # Need to duplicate this here from TestCase to make pyre happy
    @property
    def test_driver(self) -> EdenfsWatcherTestDriver:
        test_driver = self._test_driver
        assert test_driver is not None
        return test_driver

    def test_server_notifier_re(self) -> None:
        "This just makes sure that SERVER_NOTIFIER_RE still matches the messages that ServerNotifier logs on file changes"

        self.test_driver.start_hh_server()

        with open(os.path.join(self.test_driver.repo_dir, "test_file.php"), "w") as f:
            f.write("<?hh")

        self.test_driver.check_cmd(["No errors!"])
        assertServerNotifierChangesYes(self.test_driver)

    def test_interrupt(self) -> None:
        # Duplicate the test body instead of calling CommonTests.test_interrupt
        # because the superclass method enables Watchman, which we want disabled.

        # We cannot run this test with streaming errors enabled:
        # If a type-check is running, no files have changed since the start of the type-check,
        # and we invoke `hh check` with streaming errors enabled, it will wait until the
        # type-check is finished.
        # That means that we would block forever in start_hh_loop_forever_assert_timeout.
        config = self.test_driver.getConfig()
        config.streaming_errors = False
        config.interruptions = True
        config.write_hhconf(self.test_driver.repo_dir)

        self.test_driver.start_hh_server()
        self.test_driver.start_hh_loop_forever_assert_timeout()
        self.test_driver.check_cmd(
            ["string"], options=["--type-at-pos", "{root}foo_3.php:11:14"]
        )
        self.test_driver.stop_hh_loop_forever()

    def test_sync_queries(self) -> None:
        """Tests that ServerNotifier.get_changes_sync works as expected"""

        iterations = 5

        config = self.test_driver.getConfig()
        # We want to create a situation where ServerNotifier.get_changes_sync has to
        # process some actual changes, instead of the background worker having processed
        # all changes already. We can enforce this by increasing throttle_time_ms: When
        # making two subsequent changes in the loop below, this means that the
        # background worker will wait after the first change, and get_changes_sync has
        # to process the second change itself.
        config.throttle_time_ms = 1000
        config.write_hhconf(self.test_driver.repo_dir)

        self.test_driver.start_hh_server()

        for i in range(iterations):
            for j in range(iterations):
                with open(
                    os.path.join(self.test_driver.repo_dir, "sync_1.php"), "w"
                ) as f:
                    f.write(
                        """<?hh

                    function sync_g(): int {
                        $res = sync_f();
                        return $res;
                    }
                    """
                    )

                # Some break between the two changes
                time.sleep(0.1 * i)

                with open(
                    os.path.join(self.test_driver.repo_dir, "sync_2.php"), "w"
                ) as f:
                    f.write(
                        """<?hh

                    function sync_f(): int {
                        return 3;
                    }
                    """
                    )

                # Some break between the last change and the query
                time.sleep(0.1 * j)

                self.test_driver.check_cmd(
                    ["int"], options=["--type-at-pos", "{root}sync_1.php:5:32"]
                )
                print("finished iteration")

                with open(
                    os.path.join(self.test_driver.repo_dir, "sync_1.php"), "w"
                ) as f:
                    f.write(
                        """<?hh

                    function sync_g(): string {
                        $res = sync_f();
                        return $res;
                    }
                    """
                    )
                # Some break between the two changes
                time.sleep(0.1 * i)

                with open(
                    os.path.join(self.test_driver.repo_dir, "sync_2.php"), "w"
                ) as f:
                    f.write(
                        """<?hh

                    function sync_f(): string {
                        return "123";
                    }
                    """
                    )

                # Some break between the last change and the query
                time.sleep(0.1 * j)
                self.test_driver.check_cmd(
                    ["string"], options=["--type-at-pos", "{root}sync_1.php:5:32"]
                )

    def change_files_and_check(self, config: Config) -> None:
        "Not a test itself. Changes files and checks that the changes are picked up immediately."

        config.write_hhconf(self.test_driver.repo_dir)

        # We run a few iterations, varying the amount of time between the two
        # changes we make and when we call hh afterwards.
        iterations = 5

        with open(os.path.join(self.test_driver.repo_dir, "hh.conf"), "a") as f:
            f.write(
                "produce_streaming_errors = true\n"
                + "consume_streaming_errors = true\n"
            )

        self.test_driver.start_hh_server()

        for i in range(iterations):
            for j in range(iterations):
                with open(
                    os.path.join(self.test_driver.repo_dir, "sync_1.php"), "w"
                ) as f:
                    f.write(
                        """<?hh

                    function sync_g(): int {
                        return "not an int";
                    }
                    """
                    )

                # Some break between the two changes
                time.sleep(0.1 * i)

                with open(
                    os.path.join(self.test_driver.repo_dir, "sync_2.php"), "w"
                ) as f:
                    f.write(
                        """<?hh

                    function sync_f(): string {
                        return 3;
                    }
                    """
                    )
                # Some break between the last change and the query
                time.sleep(0.1 * j)

                self.test_driver.check_cmd(
                    [
                        "ERROR: {root}sync_1.php:4:32,43: Invalid return type (Typing[4110])",
                        "  {root}sync_1.php:3:40,42: Expected `int`",
                        "  {root}sync_1.php:4:32,43: But got `string`",
                        "ERROR: {root}sync_2.php:4:32,32: Invalid return type (Typing[4110])",
                        "  {root}sync_2.php:3:40,45: Expected `string`",
                        "  {root}sync_2.php:4:32,32: But got `int`",
                    ]
                )

                print("finished iteration")

                with open(
                    os.path.join(self.test_driver.repo_dir, "sync_1.php"), "w"
                ) as f:
                    f.write(
                        """<?hh

                    function sync_g(): string {
                        return 3;
                    }
                    """
                    )

                # Some break between the two changes
                time.sleep(0.1 * i)

                with open(
                    os.path.join(self.test_driver.repo_dir, "sync_2.php"), "w"
                ) as f:
                    f.write(
                        """<?hh

                    function sync_f(): int {
                        return "not an int";
                    }
                    """
                    )
                # Some break between the last change and the query
                time.sleep(0.1 * j)
                self.test_driver.check_cmd(
                    [
                        "ERROR: {root}sync_1.php:4:32,32: Invalid return type (Typing[4110])",
                        "  {root}sync_1.php:3:40,45: Expected `string`",
                        "  {root}sync_1.php:4:32,32: But got `int`",
                        "ERROR: {root}sync_2.php:4:32,43: Invalid return type (Typing[4110])",
                        "  {root}sync_2.php:3:40,42: Expected `int`",
                        "  {root}sync_2.php:4:32,43: But got `string`",
                    ]
                )

    def test_hh_check_syncness_basic(self) -> None:
        # Just uses the test driver's settings
        self.change_files_and_check(self.test_driver.getConfig())

    def test_hh_check_syncness_zero_throttle_time(self) -> None:
        config = self.test_driver.getConfig()
        config.throttle_time_ms = 0
        self.change_files_and_check(config)

    def test_hh_check_syncness_no_interruptions(self) -> None:
        # Good to have at least one test where interrupt_on_* is disabled
        config = self.test_driver.getConfig()
        config.interruptions = False
        self.change_files_and_check(config)

    def test_hg_update_basic(self) -> None:
        self.test_driver.start_hh_server()

        # Create a file that uses hg_test_fun1
        hg_update2 = os.path.join(self.test_driver.repo_dir, "hg_update2.php")
        with open(hg_update2, "w") as f:
            f.write(
                """<?hh

function hg_test_fun2(): string {
    return hg_test_fun1();
}
"""
            )

        only_hg_update2_rev = self.test_driver.commitAllChanges()

        # Create a file that defines hg_test_fun1
        hg_update1 = os.path.join(self.test_driver.repo_dir, "hg_update1.php")
        with open(hg_update1, "w") as f:
            f.write(
                """<?hh

function hg_test_fun1(): string {
    return "Hello";
}
"""
            )

        self.test_driver.check_cmd(["No errors!"])

        all_present_rev = self.test_driver.commitAllChanges()

        # this effectively deletes hg_update1, so hg_test_fun1 is gone
        self.test_driver.gotoRev(only_hg_update2_rev)
        self.test_driver.check_cmd(
            [
                "ERROR: {root}hg_update2.php:4:12,23: Unbound name (typing): `hg_test_fun1` (Typing[4107])",
                "ERROR: {root}hg_update2.php:4:12,23: Unbound name: `hg_test_fun1` (a global function) (Naming[2049])",
            ]
        )

        # Let's bring the file back
        self.test_driver.gotoRev(all_present_rev)
        self.test_driver.check_cmd(["No errors!"])

    def test_hg_update_dirty(self) -> None:
        """Tests that we handle dirty repos correctly.

        When we switch commits from rev1 to rev2, the changes that Edenfs_watcher
        reports to hh_server are just the difference between the two revisions. However,
        if we have uncommitted changes, then this may not actually reflect how the
        working copy changes.
        """

        # Create a file with a type error
        hg_update_bad_file = os.path.join(
            self.test_driver.repo_dir, "hg_update_bad_file.php"
        )
        with open(hg_update_bad_file, "w") as f:
            f.write(
                """<?hh

function hg_test_fun(): string {
    return 3;
}
"""
            )

        added_bad_file = self.test_driver.commitAllChanges()

        os.remove(hg_update_bad_file)
        self.test_driver.start_hh_server()

        # Until T226510404 is fixed, this will actually cause a recheck.
        removed_bad_file = self.test_driver.commitAllChanges()
        self.test_driver.check_cmd(["No errors!"])

        # go back one commit, where the file was still there
        self.test_driver.gotoRev(added_bad_file)
        self.test_driver.check_cmd(
            [
                "ERROR: {root}hg_update_bad_file.php:4:12,12: Invalid return type (Typing[4110])",
                "  {root}hg_update_bad_file.php:3:25,30: Expected `string`",
                "  {root}hg_update_bad_file.php:4:12,12: But got `int`",
            ]
        )

        # let's remove the file ourselves ...
        os.remove(hg_update_bad_file)
        self.test_driver.check_cmd(["No errors!"])

        # ... and now remove it "again" by changing to the commit where it was removed.
        # In other words, we now report a change to hh_server about a file that was
        # already gone, and is still gone afterwards.
        # Need to set "--merge" here so that hg let's us do this.

        self.test_driver.gotoRev(removed_bad_file, merge=True)

        self.test_driver.check_cmd(["No errors!"])

    def test_folder_rename(self) -> None:
        self.test_driver.start_hh_server()

        folder_path = os.path.join(self.test_driver.repo_dir, "old_folder")
        os.makedirs(os.path.join(folder_path, "sub"), exist_ok=True)

        with open(os.path.join(folder_path, "sub", "rename_test.php"), "w") as f:
            f.write(
                """<?hh

function rename_sub_test_fun(): int {
    return "ill-typed";
}
"""
            )

        with open(os.path.join(folder_path, "rename_test.php"), "w") as f:
            f.write(
                """<?hh

function rename_test_fun(): int {
    return "ill-typed";
}
"""
            )

        self.test_driver.check_cmd(
            [
                "ERROR: {root}old_folder/rename_test.php:4:12,22: Invalid return type (Typing[4110])",
                "  {root}old_folder/rename_test.php:3:29,31: Expected `int`",
                "  {root}old_folder/rename_test.php:4:12,22: But got `string`",
                "ERROR: {root}old_folder/sub/rename_test.php:4:12,22: Invalid return type (Typing[4110])",
                "  {root}old_folder/sub/rename_test.php:3:33,35: Expected `int`",
                "  {root}old_folder/sub/rename_test.php:4:12,22: But got `string`",
            ]
        )

        # Rename the folder
        new_folder_path = os.path.join(self.test_driver.repo_dir, "new_folder")
        os.rename(folder_path, new_folder_path)

        self.test_driver.check_cmd(
            [
                "ERROR: {root}new_folder/rename_test.php:4:12,22: Invalid return type (Typing[4110])",
                "  {root}new_folder/rename_test.php:3:29,31: Expected `int`",
                "  {root}new_folder/rename_test.php:4:12,22: But got `string`",
                "ERROR: {root}new_folder/sub/rename_test.php:4:12,22: Invalid return type (Typing[4110])",
                "  {root}new_folder/sub/rename_test.php:3:33,35: Expected `int`",
                "  {root}new_folder/sub/rename_test.php:4:12,22: But got `string`",
            ]
        )

    def test_hhconfig_change(self) -> None:
        self.test_driver.start_hh_server()

        # Let's create a file that calls a deprecated function and tries to HH_FIXME
        # that away. With the current .hhconfig, that's not actually allowed.
        deprecated = os.path.join(self.test_driver.repo_dir, "deprecated.php")
        with open(deprecated, "a") as f:
            f.write("""<?hh
<<__Deprecated("deprecated")>>
function deprecated() : void {
}

function test_deprecated() : void {
    /* HH_FIXME[4128] */
    deprecated();
}
""")

        self.test_driver.check_cmd(
            [
                "ERROR: {root}deprecated.php:7:5,24: You cannot use `HH_FIXME` or `HH_IGNORE_ERROR` comments to suppress error 4128 (Typing[4128])",
                "ERROR: {root}deprecated.php:8:5,14: The function deprecated is deprecated: deprecated (Typing[4128])",
                "  {root}deprecated.php:3:10,19: Definition is here",
            ]
        )

        # Let's make HH_FIXME[4128] legal. There's an `allowed_fixme_codes_strict` line
        # in the mini repo's .hhconfig already, so we need to patch it :(
        hhconfig_path = os.path.join(self.test_driver.repo_dir, ".hhconfig")
        with open(hhconfig_path, "r") as f:
            hhconfig_content = f.read()

        lines = hhconfig_content.split("\n")
        found_allowed_fixme_line = False

        for i, line in enumerate(lines):
            if line.strip().startswith("allowed_fixme_codes_strict"):
                lines[i] = line + ", 4128"
                found_allowed_fixme_line = True
                break

        if not found_allowed_fixme_line:
            self.fail(
                "No line starting with 'allowed_fixme_codes_strict' found in .hhconfig"
            )

        with open(hhconfig_path, "w") as f:
            f.write("\n".join(lines))

        # We need to wait until the server is back up.
        # There is one annoyance: If we run `hh check` JUST in the moment that the
        # server notices the .hhconfig check and restarts, then `hh check` spits out an
        # error message complaining that the server disconnected unexpectedly (instead
        # of waiting until it's back).
        while True:
            time.sleep(1)
            (_, _, exit_code) = self.test_driver.run_check()
            if exit_code == 0:
                break

        # We've just made the HH_FIXME[4128] legal.
        self.test_driver.check_cmd(["No errors!"])

        # Let's double-check that we did indeed restart due to the .hhconfig change
        assertMonitorLogContains(
            self.test_driver,
            "Exit_status.Hhconfig_changed",
        )

    def test_filter_file_changes(self) -> None:
        self.test_driver.start_hh_server()

        # Let's check that changes to random files are not picked up:
        self.test_driver.createIgnoredFiles()

        assertServerNotifierChangesNo(self.test_driver)
        self.test_driver.check_cmd(["No errors!"])

        # Let's check that extensions other than php are included
        hhi_file = os.path.join(self.test_driver.repo_dir, "invalid_file.hhi")

        with open(hhi_file, "w") as f:
            f.write("not a valid file")

        self.test_driver.check_cmd(
            [
                "ERROR: {root}invalid_file.hhi:1:5,5: A semicolon `;` is expected here. (Parsing[1002])"
            ]
        )

    def test_hg_update_filter(self) -> None:
        """Tests that we correctly filter out changes we get due to changing commits"""

        # We use three commits:
        # rev0: baseline, state of testing repo set up by test harness
        # rev1: contains files that should be ignored
        # rev2: contains .hhconfig change
        # all three commits are built on top of each other

        rev0 = self.test_driver.commitAllChanges(allow_empty=True)
        self.test_driver.createIgnoredFiles()
        rev1 = self.test_driver.commitAllChanges()

        hhconfig_file = os.path.join(self.test_driver.repo_dir, ".hhconfig")
        with open(hhconfig_file, "a") as f:
            f.write("# a comment to change hhconfig")

        rev2 = self.test_driver.commitAllChanges()

        # We start the server on rev0, effectively hiding all changes we've made so far
        self.test_driver.gotoRev(rev0)
        self.test_driver.start_hh_server()

        # We go to rev1. The server shouldn't see any of these changes
        self.test_driver.gotoRev(rev1)
        self.test_driver.check_cmd(["No errors!"])
        assertServerNotifierChangesNo(self.test_driver)

        # We go to rev2. The server should see the hhconfig change and restart
        self.test_driver.gotoRev(rev2)
        time.sleep(2)  # give server some time to initiate restart
        self.test_driver.check_cmd(["No errors!"])  # wait until server ready again
        assertMonitorLogContains(
            self.test_driver,
            "Exit_status.Hhconfig_changed",
        )

    def test_filter_folder_rename(self) -> None:
        self.test_driver.start_hh_server()

        folder_path = os.path.join(self.test_driver.repo_dir, "old_folder")

        # create some ignored files
        self.test_driver.createNonHackFile("old_folder/randomfileweshouldignore")
        self.test_driver.createNonHackFile("old_folder/.php")
        self.test_driver.createNonHackFile("old_folder/folder.php/ignored_file.txt")

        self.test_driver.check_cmd(["No errors!"])
        assertServerNotifierChangesNo(self.test_driver)

        # Rename the folder
        new_folder_path = os.path.join(self.test_driver.repo_dir, "new_folder")
        os.rename(folder_path, new_folder_path)

        self.test_driver.check_cmd(["No errors!"])
        assertServerNotifierChangesNo(self.test_driver)

    def test_filter_alternation(self) -> None:
        """Alternates between making filtered and non-filtered changes

        This is a regression test for a bug where this pattern caused the notification mechanism
        to (errorenously) fail a consistency check.
        """

        self.test_driver.start_hh_server()

        for _ in range(5):
            self.test_driver.createNonHackFile("not_filtered_out.php")
            self.test_driver.createNonHackFile("filtered_out.txt")

            self.test_driver.check_cmd(
                [
                    "ERROR: {root}not_filtered_out.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])"
                ]
            )

    def test_get_all_files(self) -> None:
        # Let's go to the commit where only hh.conf and .hhconfig existed
        self.test_driver.gotoRev(self.test_driver.clean_slate_commit)

        # This list must include any directory that may exist by default in a fresh hg repository
        ignored_dirs = [".hg", ".eden", ".edenfs-notifications-state"]

        # Let's double-check that we really just have the files we are expecting:
        hhconfig_count = 0
        hh_conf_count = 0
        for _root, dirs, files in os.walk(self.test_driver.repo_dir):
            # Modifying dirs is the documented way to prevent os.walk from traversing into a subdirectory
            for ignored_dir in ignored_dirs:
                if ignored_dir in dirs:
                    dirs.remove(ignored_dir)

            for file in files:
                if file == ".hhconfig":
                    hhconfig_count += 1
                elif file == "hh.conf":
                    hh_conf_count += 1
                else:
                    self.fail(f"unexpected file {file} in repo at clean slate commit")

        if hhconfig_count != 1 or hh_conf_count != 1:
            self.fail("unexpected file in repo at clean slate commit")

        # Let's add a bunch of files, all of which should be ignored
        self.test_driver.createIgnoredFiles()

        # Let's create some files that should be picked up:
        self.test_driver.createNonHackFile("file.php")
        self.test_driver.createNonHackFile("subfolder/file.php")
        self.test_driver.createNonHackFile("subfolder/.hg/file.php")

        # Let's check that extensions other than .php are included
        hhi_file = os.path.join(self.test_driver.repo_dir, "invalid_file.hhi")
        with open(hhi_file, "w") as f:
            f.write("not a valid hhi file")

        # The .hhconfig file + the 4 non-ignored files we created above, none of the files added
        # by createIgnoredFiles
        exected_file_count = 5
        self.test_driver.start_hh_server()
        self.test_driver.check_cmd(
            [
                "ERROR: {root}file.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
                "ERROR: {root}invalid_file.hhi:1:5,5: A semicolon `;` is expected here. (Parsing[1002])",
                "ERROR: {root}subfolder/.hg/file.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
                "ERROR: {root}subfolder/file.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
            ]
        )
        assertCurrentServerLogContains(
            self.test_driver,
            f"Edenfs_watcher.get_all_files returned {exected_file_count} files",
        )

    def test_deferral1(self) -> None:
        config = self.test_driver.getConfig()
        config.state_tracking = True
        config.streaming_errors = True
        config.write_hhconf(self.test_driver.repo_dir)

        # These are two states that the server listens for
        state0 = TEST_STATE_0
        state1 = TEST_STATE_1

        self.test_driver.start_hh_server()

        self.test_driver.createNonHackFile("file1.php")

        wait_for0, is_asserted0, _ = self.test_driver.assertStateForSeconds(state0, 10)

        self.test_driver.createNonHackFile("file2.php")

        # We want this to be de-asserted *after* state0
        self.test_driver.assertStateForSeconds(state1, 20)

        self.test_driver.createNonHackFile("file3.php")

        wait_for0()

        self.test_driver.createNonHackFile("file4.php")

        self.test_driver.check_cmd(
            [
                "ERROR: {root}"
                + f"file{i}.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])"
                for i in range(1, 5)
            ]
        )
        # We are running with streaming errors enabled, so the hh invocation should block until
        # the state is deasserted.
        self.assertFalse(is_asserted0())

    def run_instant_deassertion_test(
        self, throttle_time_ms: int, wait_for_deassert: bool
    ) -> None:
        """Asserts and immediately deasserts states many times in quick succession.

        Checks that this does not incorrectly leave us stuck in a deferred state.
        """
        config = self.test_driver.getConfig()
        config.state_tracking = True
        config.throttle_time_ms = throttle_time_ms
        config.write_hhconf(self.test_driver.repo_dir)
        state = TEST_STATE_0

        self.test_driver.start_hh_server()

        # 30 iterations keeps this test at around 1.5 minutes
        for iterations in range(1, 30):
            for _ in range(iterations):
                wait_thunk, _, _ = self.test_driver.assertStateForSeconds(state, 0)
                if wait_for_deassert:
                    wait_thunk()

            self.test_driver.createNonHackFile("file.php")

            self.test_driver.check_cmd(
                [
                    "ERROR: {root}file.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
                ]
            )

            path = os.path.join(self.test_driver.repo_dir, "file.php")
            os.remove(path)

            self.test_driver.check_cmd(["No errors!"])

    def test_deferral2(self) -> None:
        self.run_instant_deassertion_test(throttle_time_ms=0, wait_for_deassert=True)

    def test_deferral3(self) -> None:
        self.run_instant_deassertion_test(throttle_time_ms=500, wait_for_deassert=True)

    def test_deferral6(self) -> None:
        """With admission blocking disabled, sync queries hide changes until all states end."""
        config = self.test_driver.getConfig()
        config.state_tracking = True
        config.streaming_errors = False
        config.block_connections = False
        config.write_hhconf(self.test_driver.repo_dir)

        state0 = TEST_STATE_0
        state1 = TEST_STATE_1

        self.test_driver.start_hh_server()

        self.test_driver.createNonHackFile("file0.php")

        wait_for0, _, _ = self.test_driver.assertStateForSeconds(state0, 20)

        self.test_driver.createNonHackFile("file1.php")

        self.test_driver.check_cmd(
            [
                "ERROR: {root}file0.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
            ]
        )

        # will be deasserted while state0 is still asserted
        wait_for1, _, _ = self.test_driver.assertStateForSeconds(state1, 10)

        self.test_driver.createNonHackFile("file2.php")

        self.test_driver.check_cmd(
            [
                "ERROR: {root}file0.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
            ]
        )

        wait_for1()
        self.test_driver.check_cmd(
            [
                "ERROR: {root}file0.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
            ]
        )

        wait_for0()
        self.test_driver.check_cmd(
            [
                "ERROR: {root}file0.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
                "ERROR: {root}file1.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
                "ERROR: {root}file2.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
            ]
        )

        self.test_driver.createNonHackFile("file3.php")
        wait_for0()
        self.test_driver.check_cmd(
            [
                "ERROR: {root}file0.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
                "ERROR: {root}file1.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
                "ERROR: {root}file2.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
                "ERROR: {root}file3.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
            ]
        )

    def test_deferral8(self) -> None:
        config = self.test_driver.getConfig()
        config.state_tracking = True
        config.streaming_errors = True
        config.write_hhconf(self.test_driver.repo_dir)

        state = TEST_STATE_0
        _, is_asserted, _ = self.test_driver.assertStateForSeconds(state, 20)

        # Note that we asserted before server startup
        self.test_driver.start_hh_server()

        self.test_driver.createNonHackFile("file.php")

        self.test_driver.check_cmd(
            [
                "ERROR: {root}file.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
            ]
        )
        # We are running with streaming errors enabled, so the hh invocation should block until
        # the state is deasserted.
        self.assertFalse(is_asserted())

    def test_deferral9(self) -> None:
        config = self.test_driver.getConfig()
        config.state_tracking = True
        config.write_hhconf(self.test_driver.repo_dir)

        self.test_driver.start_hh_server()

        # Assert a state that is NOT in the tracked_states list
        _, is_still_asserted, _ = self.test_driver.assertStateForSeconds(
            "untracked-state", 30
        )

        # Create a file while the untracked state is asserted
        self.test_driver.createNonHackFile("untracked_test.php")

        # We should see the change ...
        self.test_driver.check_cmd(
            [
                "ERROR: {root}untracked_test.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
            ]
        )

        # ... and we should also not have waited until deferral.
        self.assertTrue(is_still_asserted())

    def test_deferral10(self) -> None:
        """Test asserting application crashing while hh is running"""
        config = self.test_driver.getConfig()
        config.state_tracking = True
        config.write_hhconf(self.test_driver.repo_dir)

        self.test_driver.start_hh_server()
        self.test_driver.check_cmd(["No errors!"])

        for i, kill_after_secs in enumerate([0, 0.1, 0.5, 1]):
            # Assert state for a long duration (we'll kill it before it finishes)
            _, _, kill_asserter = self.test_driver.assertStateForSeconds(
                TEST_STATE_0, 60
            )

            time.sleep(kill_after_secs)

            # SIGKILL the asserter while it's asserting TEST_STATE_0
            kill_asserter()

            # The asserter is dead, we should not be deferring anymore. hh only
            # stops deferring once it observes - asynchronously - that EdenFS
            # de-asserted the state following the crash, so poll rather than
            # checking exactly once.
            file = f"file{i}.php"
            self.test_driver.createNonHackFile(file)
            # check_cmd_eventually won't be necessary once T280972039 is fixed
            self.test_driver.check_cmd_eventually(
                [
                    f"ERROR: {{root}}{file}:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
                ]
            )
            os.remove(os.path.join(self.test_driver.repo_dir, file))

    def test_deferral11(self) -> None:
        """Regression test for when an application asserting a state crashed before starting the server."""
        config = self.test_driver.getConfig()
        config.state_tracking = True
        config.write_hhconf(self.test_driver.repo_dir)

        # Assert state for a long duration (we'll kill it before it finishes)
        _, _, kill_asserter = self.test_driver.assertStateForSeconds(TEST_STATE_0, 60)

        # SIGKILL the asserter while it's asserting TEST_STATE_0
        kill_asserter()

        self.test_driver.start_hh_server()
        self.test_driver.check_cmd(["No errors!"])

        # Let's assert the state again
        wait_for_deassert, _, _ = self.test_driver.assertStateForSeconds(
            TEST_STATE_0, 1
        )
        wait_for_deassert()
        self.test_driver.check_cmd(["No errors!"])

    def test_deferral12(self) -> None:
        """Sync-discovered dotted states defer file changes until the state ends."""
        config = self.test_driver.getConfig()
        config.state_tracking = True
        config.streaming_errors = False  # we want to trigger get_changes_sync
        config.throttle_time_ms = 5000  # make it easier to trigger get_changes_sync
        config.obey_deferral = True
        config.block_connections = False
        config.write_hhconf(self.test_driver.repo_dir)

        # We need a test state with . in the name
        self.assertIn(".", TEST_STATE_0)

        self.test_driver.start_hh_server()

        # Create a file before asserting the state.
        # The worker should pick this up immediately (throttle_time_ms only delays *after* the first change).
        self.test_driver.createNonHackFile("pre_state.php")

        wait_for, is_still_asserted, _ = self.test_driver.assertStateForSeconds(
            TEST_STATE_0, 20
        )

        # Create a file while the state is asserted.
        # The worker is now in its 5000ms throttle wait.
        self.test_driver.createNonHackFile("during_state.php")

        # This should trigger a get_changes_sync call, picking up
        # 1. StateEntered("hh.state-tracking-test-state0")
        # 2. File change for "during_state.php" (but this one is deferred!)
        self.test_driver.check_cmd(
            [
                "ERROR: {root}pre_state.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
            ]
        )

        self.assertTrue(is_still_asserted())
        wait_for()

        # All changes must be visible now.
        self.test_driver.check_cmd(
            [
                "ERROR: {root}pre_state.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
                "ERROR: {root}during_state.php:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])",
            ]
        )

    def test_deferral13(self) -> None:
        """Regression test: state changes during EdenfsWatcherInstance::init

        State changes during EdenfsWatcherInstance::init can lead to some duplicate state
        enter/leave events reaching apply_incoming_changes. The latter must tolerate them in its
        invariant checks and filter them out.
        """
        config = self.test_driver.getConfig()
        config.state_tracking = True
        config.write_hhconf(self.test_driver.repo_dir)

        # We cannot exactly time when hh_server initializes Edenfs_watcher during server startup.
        # Instead, we run a background thread that continuously asserts and deasserts a test state
        # every 100ms. (until stop_event tells it to stop)

        stop_event: threading.Event = threading.Event()

        def assert_deassert_loop() -> None:
            while not stop_event.is_set():
                duration_secs = 0.1
                wait_for, _, _ = self.test_driver.assertStateForSeconds(
                    TEST_STATE_0, duration_secs
                )
                wait_for()
                time.sleep(duration_secs)

        loop_thread = threading.Thread(target=assert_deassert_loop)
        loop_thread.start()

        try:
            for i in range(10):
                self.test_driver.start_hh_server()
                self.test_driver.check_cmd(["No errors!"])
                self.test_driver.stop_hh_server()
                assertServerNotCrashed(self.test_driver)
                print(f"iteration {i} passed")
        finally:
            stop_event.set()
            loop_thread.join()

        # Start a final server so tearDown's assertEdenFsWatcherInitialized
        # and stop_hh_server work.
        self.test_driver.start_hh_server()


class EdenfsWatcherNonMountPointRepoTests(EdenfsWatcherTests):
    """Runs the same tests as EdenfsWatcherTests, but with a testing repo that's not the mount point of the Eden mount.

    We also make two orthogonal changes compared to EdenfsWatcherTestDriver:
    - We run with streaming errors enabled. This way, we run all tests with and without streaming
      errors enabled.
    - We run with state tracking enabled. This way, we run all tests with and without state
      tracking enabled.
    """

    @classmethod
    def get_test_driver(cls) -> common_tests.CommonTestDriver:
        return EdenfsWatcherNonMountPointTestDriver()

    # Don't add any tests here, add them to EdenfsWatcherTests!


class ClientConnectionDeferralTest(TestCase[EdenfsWatcherTestDriver]):
    """Exercise admission through real Eden, monitor, server, and client processes.

    Each waiting scenario establishes a known deferral before connecting a client, then checks that
    the same request completes after release.
    """

    @classmethod
    def get_test_driver(cls) -> EdenfsWatcherTestDriver:
        """Reuse the subdirectory fixture without inheriting its general tests."""
        return EdenfsWatcherNonMountPointTestDriver()

    def setUp(self) -> None:
        """Mount the test checkout and track subprocesses for failure cleanup."""
        super().setUp()
        self.clients: list[subprocess.Popen[str]] = []
        self.state_releases: list[Callable[[], None]] = []

    def tearDown(self) -> None:
        # Reap clients and state asserters before the driver stops hh_server and
        # unmounts Eden, including when a test fails while waiting for a connection.
        try:
            for client in self.clients:
                if client.poll() is None:
                    client.kill()
                client.communicate()
        finally:
            try:
                for release_state in reversed(self.state_releases):
                    release_state()
            finally:
                super().tearDown()

    def wait_for_server_progress(self, message: str) -> None:
        """Expect the waiting message and a working, rather than ready, disposition.

        Progress is published asynchronously, so allow the server loop to
        observe the current states before checking the on-disk progress JSON.
        """
        stdout, _, retcode = self.test_driver.proc_call(
            [hh_client, "--logname", self.test_driver.repo_dir], log=False
        )
        self.assertEqual(retcode, 0)
        progress_path = Path(stdout.strip()).with_suffix(".progress.json")
        deadline = time.monotonic() + 10
        while True:
            progress = json.loads(progress_path.read_text())
            if progress["message"] == message:
                self.assertEqual(progress["disposition"], ["DWorking"])
                return
            if time.monotonic() >= deadline:
                self.fail(f"Expected progress {message!r}, got {progress!r}")
            time.sleep(0.1)

    def start_connection_test_server(
        self,
        *,
        hg_aware: bool,
        block_connections: bool,
        obey_deferral: bool,
    ) -> None:
        """Start a server that tracks both hg and synthetic non-hg Eden states.

        Disable streaming errors so clients must query the server.
        """
        config = self.test_driver.getConfig()
        config.state_tracking = True
        config.tracked_states = (
            TEST_STATE_0,
            TEST_STATE_1,
            "hg.update",
            "hg.transaction",
        )
        config.streaming_errors = False
        config.hg_aware = hg_aware
        config.block_connections = block_connections
        config.obey_deferral = obey_deferral
        config.write_hhconf(self.test_driver.repo_dir)
        self.test_driver.start_hh_server()

    def assert_state(
        self, state_name: str
    ) -> tuple[Callable[[], bool], Callable[[], None]]:
        """Assert an Eden state and wait until hh_server has observed its entry.

        Return callbacks to check that the asserter process is still alive and
        to kill/reap it, causing Eden to release the state asynchronously. The
        60-second lifetime is a fallback; tests normally release it explicitly,
        and teardown also releases it if an assertion fails.
        """
        _, is_asserted, release_state = self.test_driver.assertStateForSeconds(
            state_name, 60
        )
        self.state_releases.append(release_state)
        waitForCurrentServerLogContains(
            self.test_driver, f"ServerNotifier: StateEnter({state_name})"
        )
        return is_asserted, release_state

    def start_client_query(self, *options: str) -> subprocess.Popen[str]:
        """Launch a non-streaming query and wait for its connection to the monitor.

        Return the running client without waiting for server admission or a
        response, so tests can observe whether deferral keeps it waiting.
        """
        stdout, _, retcode = self.test_driver.proc_call(
            [hh_client, "--client-logname", self.test_driver.repo_dir], log=False
        )
        self.assertEqual(retcode, 0)
        client_log_path = Path(stdout.strip())
        # Startup/readiness checks also use hh_client. Only a connection message
        # appended after this query starts can establish that it reached the monitor.
        log_offset = len(client_log_path.read_text())
        client = self.test_driver.proc_create(
            [
                hh_client,
                "check",
                "--config",
                "consume_streaming_errors=false",
                "--error-format",
                "raw",
                self.test_driver.repo_dir,
                *options,
            ],
            {},
        )
        self.clients.append(client)
        deadline = time.monotonic() + 10
        while True:
            client_log = client_log_path.read_text()[log_offset:]
            if (
                "ClientConnect.connect: successfully connected to monitor."
                in client_log
            ):
                break
            if time.monotonic() >= deadline:
                self.fail(f"Client did not connect to monitor:\n{client_log}")
            time.sleep(0.1)
        return client

    def assert_client_waiting(self, client: subprocess.Popen[str]) -> None:
        """Expect a monitor-connected client to remain unanswered for one second.

        This observes the request's behavior, not a particular server branch;
        the one-second observation window is not an hh_client timeout setting.
        """
        with self.assertRaises(subprocess.TimeoutExpired):
            client.communicate(timeout=1)

    def assert_client_errors(
        self, client: subprocess.Popen[str], filenames: Iterable[str]
    ) -> None:
        """Expect this query to finish with exactly one parser error per named file.

        Tests create malformed .php files with createNonHackFile: their distinct
        filenames make it clear which disk changes the response has incorporated.
        """
        stdout, stderr = client.communicate(timeout=15)
        self.assertEqual(client.returncode, 2, stderr)
        self.assertCountEqual(
            [
                f"ERROR: {self.test_driver.repo_dir}/{filename}:1:1,1: A .php file must begin with `<?hh`. (Parsing[1002])"
                for filename in filenames
            ],
            stdout.splitlines(),
        )

    def test_client_connection_deferral(self) -> None:
        """A default-pipe query must wait with working progress during known deferral.

        After release, that same query must report changes made both before it
        connected and while it was waiting.
        """
        # Block on non-hg states even with hg awareness disabled. Keep the
        # watcher withholding deferred changes from explicit queries: the
        # client should wait to connect until the state ends.
        self.start_connection_test_server(
            hg_aware=False, block_connections=True, obey_deferral=True
        )
        # Establish the deferral before either the file change or the query,
        # so this exercises admission to an already-deferred server.
        is_asserted, release_state = self.assert_state(TEST_STATE_0)
        self.test_driver.createNonHackFile("before_client.php")
        client = self.start_client_query()

        # An unanswered query must be presented as waiting/working, not ready.
        self.wait_for_server_progress(f"waiting for {TEST_STATE_0}")
        self.assert_client_waiting(client)
        self.assertTrue(is_asserted())

        # The eventual response must include later changes too, not just the
        # files that existed when the client connected.
        self.test_driver.createNonHackFile("while_waiting.php")
        release_state()
        self.assert_client_errors(client, ["before_client.php", "while_waiting.php"])

    def test_client_connection_deferral_disabled(self) -> None:
        """Disabling connection blocking must bypass both hg and non-hg deferrals.

        Even with hg_aware enabled, the query must return current errors without
        waiting for either state to end.
        """
        # Enable hg awareness to ensure disabling connection blocking overrides
        # it. Let explicit queries see deferred changes so we can check fresh
        # errors without releasing either state.
        self.start_connection_test_server(
            hg_aware=True, block_connections=False, obey_deferral=False
        )
        hg_is_asserted, _ = self.assert_state("hg.update")
        other_is_asserted, _ = self.assert_state(TEST_STATE_0)
        self.test_driver.createNonHackFile("file.php")

        # Do not release either state: completion must come from bypassing
        # admission blocking, not from the deferral expiring.
        client = self.start_client_query()
        self.assert_client_errors(client, ["file.php"])
        self.assertTrue(hg_is_asserted())
        self.assertTrue(other_is_asserted())

    def test_client_connection_deferral_waits_for_all_hg_states(self) -> None:
        """With hg_aware enabled, queries must wait until every hg state ends.

        Progress must list the remaining blocking states in sorted order and
        remain working when only one state has ended.
        """
        # Enable hg-aware blocking while retaining the watcher's existing
        # behavior of withholding deferred changes from explicit queries.
        self.start_connection_test_server(
            hg_aware=True, block_connections=True, obey_deferral=True
        )
        # Assert in the opposite order from the expected progress text, and
        # prepare an error that the eventual query must report.
        _, finish_update = self.assert_state("hg.update")
        _, finish_transaction = self.assert_state("hg.transaction")
        self.test_driver.createNonHackFile("file.php")
        client = self.start_client_query()
        self.wait_for_server_progress("waiting for hg.transaction, hg.update")
        self.assert_client_waiting(client)

        # Releasing one state updates progress but must not admit the client.
        finish_update()
        self.wait_for_server_progress("waiting for hg.transaction")
        self.assert_client_waiting(client)

        # Only the last release permits the same query to receive current errors.
        finish_transaction()
        self.assert_client_errors(client, ["file.php"])

    def test_client_connection_deferral_only_waits_for_non_hg_state(self) -> None:
        """With hg_aware disabled, mixed states must block only on non-hg deferral.

        Progress must omit hg states, and the query must complete while both
        hg.update and hg.transaction remain asserted.
        """
        # With hg awareness off, only the non-hg state should block connections.
        # Let explicit queries see deferred changes, since the expected response
        # arrives while the hg states are still asserted.
        self.start_connection_test_server(
            hg_aware=False, block_connections=True, obey_deferral=False
        )
        update_is_asserted, _ = self.assert_state("hg.update")
        transaction_is_asserted, _ = self.assert_state("hg.transaction")
        _, release_other_state = self.assert_state(TEST_STATE_0)
        self.test_driver.createNonHackFile("file.php")
        client = self.start_client_query()
        self.wait_for_server_progress(f"waiting for {TEST_STATE_0}")
        self.assert_client_waiting(client)

        # Releasing only the non-hg state must suffice; leave both hg states alive.
        release_other_state()
        self.assert_client_errors(client, ["file.php"])
        self.assertTrue(update_is_asserted())
        self.assertTrue(transaction_is_asserted())

    def test_client_connection_deferral_blocks_priority_query(self) -> None:
        """An idle server must defer priority-pipe queries as well as ordinary ones.

        A liveness request must remain unanswered until the known deferral ends,
        then complete successfully without needing a full check.
        """
        # Non-hg states must block priority connections with hg awareness off,
        # even with the watcher still withholding deferred changes from queries.
        self.start_connection_test_server(
            hg_aware=False, block_connections=True, obey_deferral=True
        )
        # Keep the server idle: this covers main-loop admission rather than the
        # priority interrupt handler exercised by the separate active-check test.
        _, release_state = self.assert_state(TEST_STATE_0)
        # CHECK_LIVENESS uses the priority pipe and does not require a full check.
        client = self.start_client_query(
            "--search", "this_is_just_to_check_liveness_of_hh_server", "--json"
        )
        self.assert_client_waiting(client)

        # The already-connected request, not a retry launched by the test, succeeds.
        release_state()
        stdout, stderr = client.communicate(timeout=15)
        self.assertEqual(client.returncode, 0, stderr)
        self.assertEqual(stdout.strip(), "[]")

    def test_client_connection_deferral_blocks_priority_interrupt(self) -> None:
        """Priority interrupts must respect known deferral during an ongoing check.

        After release, the liveness request must complete while the full check is
        still running, demonstrating that it was served through the interrupt path.
        """
        # Use the same settings as the idle-priority test, so only the server's
        # execution path changes: non-hg blocking with watcher deferral enabled.
        self.start_connection_test_server(
            hg_aware=False, block_connections=True, obey_deferral=True
        )
        # The existing fixture creates a worker-based check stuck in
        # hh_loop_forever(), keeping execution out of idle main-loop admission.
        self.test_driver.start_hh_loop_forever_assert_timeout()
        is_asserted, release_state = self.assert_state(TEST_STATE_0)
        client = self.start_client_query(
            "--search", "this_is_just_to_check_liveness_of_hh_server", "--json"
        )
        self.assert_client_waiting(client)
        self.assertTrue(is_asserted())

        release_state()
        # Receive the response before stopping the check, to exercise the interrupt path.
        stdout, stderr = client.communicate(timeout=15)
        self.assertEqual(client.returncode, 0, stderr)
        self.assertEqual(stdout.strip(), "[]")
        self.test_driver.stop_hh_loop_forever()

    def test_client_connection_deferral_allows_force_dormant_query(self) -> None:
        """Force-dormant queries must bypass admission blocking during deferral.

        The response must include deferred file changes while the state is still
        asserted, with synchronous-query deferral disabled.
        """
        # Keep ordinary connections blocked; only this client's force-dormant
        # option should let it through. Let queries see deferred changes so we
        # can verify fresh errors before releasing the state.
        self.start_connection_test_server(
            hg_aware=False, block_connections=True, obey_deferral=False
        )
        is_asserted, _ = self.assert_state(TEST_STATE_0)
        self.test_driver.createNonHackFile("file.php")
        client = self.start_client_query("--force-dormant-start", "true")
        self.assert_client_errors(client, ["file.php"])
        # Completion must not depend on the state ending naturally.
        self.assertTrue(is_asserted())


class IsolationMoveToTests(TestCase[EdenfsWatcherTestDriver]):
    """Runs `--isolation-move-to` against an Eden-backed repo.

    Watchman does not report a renamed directory on an Eden mount, so the server
    needs the EdenFS file watcher, which reports the files under it.
    """

    @classmethod
    def get_test_driver(cls) -> EdenfsWatcherTestDriver:
        return EdenfsWatcherTestDriver()

    def write_files(self, files: dict[str, str]) -> None:
        for filename, contents in files.items():
            path = os.path.join(self.test_driver.repo_dir, filename)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(contents)

    def move_to(self, seed_dir: str, parent: str) -> Tuple[int, dict[str, object]]:
        stdout, _, retcode = self.test_driver.run_check(
            options=[
                "--isolation",
                "--isolation-seed-dir",
                seed_dir,
                "--isolation-move-to",
                parent,
                "--json",
            ],
        )
        return (retcode, json.loads(stdout))

    def exists(self, path: str) -> bool:
        return os.path.exists(os.path.join(self.test_driver.repo_dir, path))

    def enable_packages(self) -> None:
        hhconfig_path = os.path.join(self.test_driver.repo_dir, ".hhconfig")
        with open(hhconfig_path, "a") as f:
            f.write("\npackages_config_path = PACKAGES.toml\n")

    def test_a_directory_is_moved_into_the_parent(self) -> None:
        self.write_files(
            {
                "move_src/helper.php": "<?hh\nfunction move_helper(): void {}\n",
                "move_src/user.php": (
                    "<?hh\nfunction move_user(): void {\n  move_helper();\n}\n"
                ),
            }
        )
        self.test_driver.start_hh_server()

        retcode, result = self.move_to("move_src", "move_dest")

        self.assertEqual(0, retcode, result)
        self.assertEqual("move_src", result["source"])
        self.assertEqual("move_dest/move_src", result["target"])
        self.assertTrue(result["moved"])
        self.assertEqual(2, result["files"])
        self.assertFalse(self.exists("move_src"))
        self.assertTrue(self.exists("move_dest/move_src/helper.php"))
        self.assertTrue(self.exists("move_dest/move_src/user.php"))

    def test_a_blocked_directory_is_left_alone(self) -> None:
        self.write_files(
            {
                "blocked_src/helper.php": (
                    "<?hh\nfunction blocked_helper(): void {}\n"
                ),
                "blocked_user.php": (
                    "<?hh\nfunction blocked_user(): void {\n  blocked_helper();\n}\n"
                ),
            }
        )
        self.test_driver.start_hh_server()

        retcode, result = self.move_to("blocked_src", "blocked_dest")

        type_error_exit_code = 2
        self.assertEqual(type_error_exit_code, retcode, result)
        self.assertFalse(result["moved"])
        self.assertEqual(["blocked_user.php"], result["blockers"])
        self.assertTrue(self.exists("blocked_src/helper.php"))
        self.assertFalse(self.exists("blocked_dest"))

    def test_a_move_that_adds_an_error_is_undone(self) -> None:
        self.write_files(
            {
                "PACKAGES.toml": (
                    "[packages]\n"
                    "[packages.pkg_a]\n"
                    'include_paths=["//undo_a/"]\n'
                    "[packages.pkg_b]\n"
                    'include_paths=["//undo_b/"]\n'
                    "[deployments]\n"
                ),
                "undo_a/other.php": "<?hh\nfunction undo_other(): void {}\n",
                "undo_a/mover/user.php": (
                    "<?hh\nfunction undo_user(): void {\n  undo_other();\n}\n"
                ),
            }
        )
        self.enable_packages()
        self.test_driver.start_hh_server()

        retcode, result = self.move_to("undo_a/mover", "undo_b")

        type_error_exit_code = 2
        self.assertEqual(type_error_exit_code, retcode, result)
        self.assertFalse(result["moved"])
        cross_package_access = 4472
        new_errors = result["new_errors"]
        assert isinstance(new_errors, list)
        self.assertEqual(
            [cross_package_access],
            [error["code"] for error in new_errors],
        )
        self.assertTrue(result["baseline_restored"])
        self.assertTrue(self.exists("undo_a/mover/user.php"))
        self.assertFalse(self.exists("undo_b"))


class EdenfsWatcherWatchmanTestDriver(EdenfsWatcherTestDriver):
    """An Eden-backed repo whose server uses Watchman, not Edenfs_watcher."""

    @classmethod
    def prepare_initial_commit(cls, output_folder: str) -> list[str]:
        with open(os.path.join(output_folder, ".watchmanconfig"), "w") as f:
            f.write("{}")
        with open(os.path.join(output_folder, "hh.conf"), "w") as f:
            f.write(
                "min_log_level = Debug\n"
                "use_watchman = true\n"
                "watchman_subscribe_v2 = true\n"
                "edenfs_file_watcher_enabled = false\n"
            )
        return EdenTestDriver.prepare_initial_commit(output_folder) + [
            "hh.conf",
            ".watchmanconfig",
        ]

    def tearDown(self) -> None:
        # Not EdenfsWatcherTestDriver.tearDown: it asserts that Edenfs_watcher ran.
        assertServerNotCrashed(self)
        self.stop_hh_server(retries=3)
        EdenTestDriver.tearDown(self)


class IsolationMoveToWatchmanTests(TestCase[EdenfsWatcherWatchmanTestDriver]):
    """Watchman does not report a renamed directory on an Eden mount
    (T118370264), so `--isolation-move-to` is refused there before anything moves.
    """

    @classmethod
    def get_test_driver(cls) -> EdenfsWatcherWatchmanTestDriver:
        return EdenfsWatcherWatchmanTestDriver()

    def test_a_move_is_refused_on_eden_with_watchman(self) -> None:
        repo_dir = Path(self.test_driver.repo_dir)
        (repo_dir / "to_move").mkdir()
        (repo_dir / "to_move" / "a.php").write_text("<?hh\nfunction a(): void {}\n")

        self.test_driver.start_hh_server()
        stdout, _, exit_code = self.test_driver.run_check(
            options=[
                "--isolation",
                "--isolation-seed-dir",
                "to_move",
                "--isolation-move-to",
                "move_dest",
                "--json",
            ],
        )

        input_error_exit_code = 10
        self.assertEqual(exit_code, input_error_exit_code, stdout)
        result = json.loads(stdout)
        self.assertFalse(result["moved"])
        self.assertIn("watching this Eden checkout with Watchman", result["refused"])
        self.assertTrue((repo_dir / "to_move" / "a.php").is_file())
        self.assertFalse((repo_dir / "move_dest").exists())
