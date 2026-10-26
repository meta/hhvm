# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import atexit
import functools
import hashlib
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid

import pywatchman

from . import TempDir

try:
    import pwd
except ImportError:
    # Windows
    pass

tls = threading.local()


def getSharedInstance(config=None):
    config_hash = hashlib.sha1(json.dumps(config).encode()).hexdigest()
    attr = f"instance_{config_hash}"
    global tls
    inst = getattr(tls, attr, None)
    if inst is None:
        # Ensure that the temporary dir is configured
        TempDir.get_temp_dir().get_dir()
        inst = Instance(config=config)
        inst.start()
        setattr(tls, attr, inst)
        atexit.register(lambda inst=inst: inst.stop())
    return inst


KICKSTART_FSEVENTSD = "/usr/local/bin/kickstart-fseventsd"


@functools.cache
def startFseventsd() -> None:
    """Start fseventsd on macOS hosts that provide KICKSTART_FSEVENTSD.

    Watchman cannot watch anything on macOS without fseventsd. Sandcastle's
    Mac VM images keep it off and provide the script, which the job user may
    run with ``sudo -n``; other hosts do not have the script.
    """
    if sys.platform == "darwin" and os.access(KICKSTART_FSEVENTSD, os.X_OK):
        subprocess.call(["sudo", "-n", KICKSTART_FSEVENTSD])


def mergeTestConfig(config):
    """Merge local config with test config specified via WATCHMAN_TEST_CONFIG"""
    test_config = json.loads(os.getenv("WATCHMAN_TEST_CONFIG") or "{}")
    return {**test_config, **(config or {})}


class InitWithFilesMixin:
    def _init_state(self) -> None:
        # pyre-fixme[16]: `InitWithFilesMixin` has no attribute `base_dir`.
        self.base_dir = tempfile.mkdtemp(prefix="inst")
        # no separate user directory here -- that's only in InitWithDirMixin
        # pyre-fixme[16]: `InitWithFilesMixin` has no attribute `user_dir`.
        self.user_dir = None
        # pyre-fixme[16]: `InitWithFilesMixin` has no attribute `cfg_file`.
        self.cfg_file = os.path.join(self.base_dir, "config.json")
        # pyre-fixme[16]: `InitWithFilesMixin` has no attribute `log_file_name`.
        self.log_file_name = os.path.join(self.base_dir, "log")
        # pyre-fixme[16]: `InitWithFilesMixin` has no attribute `cli_log_file_name`.
        self.cli_log_file_name = os.path.join(self.base_dir, "cli-log")
        # pyre-fixme[16]: `InitWithFilesMixin` has no attribute `pid_file`.
        self.pid_file = os.path.join(self.base_dir, "pid")
        # pyre-fixme[16]: `InitWithFilesMixin` has no attribute `pipe_name`.
        self.pipe_name = (
            "\\\\.\\pipe\\watchman-test-%s"
            % uuid.uuid5(uuid.NAMESPACE_URL, self.base_dir).hex
        )
        # pyre-fixme[16]: `InitWithFilesMixin` has no attribute `sock_file`.
        self.sock_file = os.path.join(self.base_dir, "sock")
        # pyre-fixme[16]: `InitWithFilesMixin` has no attribute `state_file`.
        self.state_file = os.path.join(self.base_dir, "state")

    def get_state_args(self):
        return [
            "--unix-listener-path={0}".format(self.sock_file),
            "--named-pipe-path={0}".format(self.pipe_name),
            "--logfile={0}".format(self.log_file_name),
            "--statefile={0}".format(self.state_file),
            "--pidfile={0}".format(self.pid_file),
        ]


class InitWithDirMixin:
    """A mixin to allow setting up a state dir rather than a state file. This is
    only meant to test state dir creation and permissions -- most operations are
    unlikely to work.
    """

    def _init_state(self) -> None:
        # pyre-fixme[16]: `InitWithDirMixin` has no attribute `base_dir`.
        self.base_dir = tempfile.mkdtemp(prefix="inst")
        # pyre-fixme[16]: `InitWithDirMixin` has no attribute `cfg_file`.
        self.cfg_file = os.path.join(self.base_dir, "config.json")
        # This needs to be separate from the log_file_name because the
        # log_file_name won't exist in the beginning, but the cli_log_file_name
        # will.
        # pyre-fixme[16]: `InitWithDirMixin` has no attribute `cli_log_file_name`.
        self.cli_log_file_name = os.path.join(self.base_dir, "cli-log")
        # pwd/os.getuid are POSIX-only. This mixin isn't expected to run on
        # Windows anyway, so gate the POSIX path on os.name and fail loudly
        # otherwise. Keeping the calls inside the guard also hides them from the
        # Windows type checker (which lacks pwd/os.getuid in its typeshed).
        if os.name == "posix":
            username = pwd.getpwuid(os.getuid())[0]
        else:
            raise RuntimeError(
                "InitWithDirMixin state dirs are only supported on POSIX"
            )
        # pyre-fixme[16]: `InitWithDirMixin` has no attribute `user_dir`.
        self.user_dir = os.path.join(self.base_dir, "%s-state" % username)
        # pyre-fixme[16]: `InitWithDirMixin` has no attribute `log_file_name`.
        self.log_file_name = os.path.join(self.user_dir, "log")
        # pyre-fixme[16]: `InitWithDirMixin` has no attribute `sock_file`.
        self.sock_file = os.path.join(self.user_dir, "sock")
        # pyre-fixme[16]: `InitWithDirMixin` has no attribute `state_file`.
        self.state_file = os.path.join(self.user_dir, "state")
        # pyre-fixme[16]: `InitWithDirMixin` has no attribute `pipe_name`.
        self.pipe_name = "INVALID"

    def get_state_args(self):
        return ["--test-state-dir={0}".format(self.base_dir)]


class _Instance:
    # Tracks a running watchman instance.  It is created with an
    # overridden global configuration file; you may pass that
    # in to the constructor

    def __init__(
        self, config=None, start_timeout: float = 60.0, debug_watchman: bool = False
    ) -> None:
        self.start_timeout = start_timeout
        # pyre-fixme[16]: `_Instance` has no attribute `_init_state`.
        self._init_state()
        self.proc = None
        self.pid = None
        self.debug_watchman = debug_watchman
        # pyre-fixme[16]: `_Instance` has no attribute `cfg_file`.
        with open(self.cfg_file, "w") as f:
            f.write(json.dumps(mergeTestConfig(config)))

    def __del__(self) -> None:
        self.stop()

    def __enter__(self) -> "_Instance":
        return self

    def __exit__(self, *exc_info) -> None:
        self.stop()

    def getSockPath(self):
        return pywatchman.SockPath(
            unix_domain=self.getUnixSockPath(), named_pipe=self.getNamedPipePath()
        )

    def getUnixSockPath(self):
        return self.sock_file

    def getNamedPipePath(self):
        return self.pipe_name

    def getCLILogContents(self) -> str:
        # pyre-fixme[16]: `_Instance` has no attribute `cli_log_file_name`.
        with open(self.cli_log_file_name, "r") as f:
            return f.read()

    def getServerLogContents(self) -> str:
        # pyre-fixme[16]: `_Instance` has no attribute `log_file_name`.
        with open(self.log_file_name, "r") as f:
            return f.read()

    def stop(self) -> None:
        if self.proc:
            self.proc.kill()
            self.proc.wait()
            self.proc = None

    def watchmanBinary(self) -> str:
        return os.environ.get("WATCHMAN_BINARY", "watchman")

    def commandViaCLI(self, cmd, prefix=None):
        """a very bare bones helper to test the site spawner functionality"""
        args = prefix or []
        args.extend([self.watchmanBinary(), "--log-level=2"])
        args.extend(self.get_state_args())
        args.extend(cmd)

        env = os.environ.copy()
        env["WATCHMAN_CONFIG_FILE"] = self.cfg_file
        del env["WATCHMAN_NO_SPAWN"]
        proc = subprocess.Popen(
            args, env=env, stdin=None, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        return proc.communicate()

    def start(self, extra_env=None) -> None:
        startFseventsd()
        args = [self.watchmanBinary(), "--foreground", "--log-level=2"]
        # pyre-fixme[16]: `_Instance` has no attribute `get_state_args`.
        args.extend(self.get_state_args())
        env = os.environ.copy()
        # pyre-fixme[16]: `_Instance` has no attribute `cfg_file`.
        env["WATCHMAN_CONFIG_FILE"] = self.cfg_file
        if extra_env:
            env.update(extra_env)
        # pyre-fixme[16]: `_Instance` has no attribute `cli_log_file_name`.
        with open(self.cli_log_file_name, "w+") as cli_log_file:
            self.proc = subprocess.Popen(
                args, env=env, stdin=None, stdout=cli_log_file, stderr=cli_log_file
            )
        if self.debug_watchman:
            print("Watchman instance PID: " + str(self.proc.pid))
            user_input = input
            user_input("Press Enter to continue...")

        # wait for it to come up
        deadline = time.time() + self.start_timeout
        while time.time() < deadline:
            try:
                # Use a reasonable timeout for individual connection attempts.
                # This needs to be long enough to handle slow startup on
                # resource-constrained systems (e.g., Windows CI with large files).
                client = pywatchman.client(
                    sockpath=self.getSockPath(),
                    timeout=min(10.0, max(1.0, deadline - time.time())),
                )
                self.pid = client.query("get-pid")["pid"]
                break
            except (pywatchman.SocketConnectError, pywatchman.SocketTimeout):
                t, val, tb = sys.exc_info()
                time.sleep(0.1)
            finally:
                # pyrefly: ignore [unbound-name]
                client.close()

        if self.pid is None:
            # self.proc didn't come up: wait for it to die
            self.proc.wait(timeout=self.start_timeout)
            # pyre-fixme[61]: `val` is undefined, or not always defined.
            raise val

    def _waitForSuspend(self, suspended, timeout: float) -> bool:
        if os.name == "nt":
            # There's no 'ps' equivalent we can use
            return True

        # Check the information in the 'ps' output
        deadline = time.time() + timeout
        state = "s" if sys.platform.startswith("sunos") else "state"
        while time.time() < deadline:
            out, err = subprocess.Popen(
                ["ps", "-o", state, "-p", str(self.pid)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            ).communicate()
            status = out.splitlines()[-1]
            is_suspended = "T" in status.decode("utf-8", "surrogateescape")
            if is_suspended == suspended:
                return True

            time.sleep(0.03)
        return False

    def _susresBinary(self) -> str:
        return os.environ.get("WATCHMAN_SUSRES", "susres.exe")

    def suspend(self) -> None:
        # pyrefly: ignore [missing-attribute, unsupported-operation]
        if self.proc.poll() or self.pid <= 1:
            raise Exception("watchman process isn't running")
        if os.name == "nt":
            subprocess.check_call([self._susresBinary(), "suspend", str(self.pid)])
        else:
            # pyrefly: ignore [bad-argument-type]
            os.kill(self.pid, signal.SIGSTOP)

        if not self._waitForSuspend(True, 5):
            raise Exception("watchman process didn't stop in 5 seconds")

    def resume(self) -> None:
        # pyrefly: ignore [missing-attribute, unsupported-operation]
        if self.proc.poll() or self.pid <= 1:
            raise Exception("watchman process isn't running")
        if os.name == "nt":
            subprocess.check_call([self._susresBinary(), "resume", str(self.pid)])
        else:
            # pyrefly: ignore [bad-argument-type]
            os.kill(self.pid, signal.SIGCONT)

        if not self._waitForSuspend(False, 5):
            raise Exception("watchman process didn't resume in 5 seconds")


class Instance(_Instance, InitWithFilesMixin):
    pass


class InstanceWithStateDir(_Instance, InitWithDirMixin):
    pass
