#!/usr/bin/env python3

"""
Shows telemetry and output for hh_client for use in .exp tests.


## API

A test case is a .json file providing `args` and optional `cwd` and `stdin` fields. Paths
in `args` are relative to .sample_repos. An optional FIND_HH executable runs the
same case through a private production-mode launcher installation. Wrapper
snapshots fall back to the direct-client .exp unless an override exists.
`find_hh_expected_package` opts into package-selection checks: the named fixture
version runs the client; other versions record their version out of band and
exit 86, and the runner then fails the harness instead of snapshotting that
selection.

## Implementation notes

Testing telemetry is a little awakward, because
hh_client's telemetry-writing for bad args is done in an orphaned process.
(The writing is in an orphaned process so as to not block the user of hh_cleint
on nonessential work).

So this test runner works as follows to avoid flakiness and ensure cleanup.

    test runner # this program
    └── hh_client  # new process-group leader
          └── telemetry daemon # can exit after hh_client
                    └── fake scribe_cat # can be slow

we can't wait() for the telemetry daemon process,
so instead we repeatedly call killpg(group, 0) to see if a member of the group
still exists.
"""

# pyre-strict
# arc pyre check //hphp/hack/test/client_args/...

import errno
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, cast

# Remove the line numbers from stacks so snapshots remain stable.
# Example: `path.ml @ line`.
STACK_FRAME = re.compile(r"^\S.*\.(?:ml|mli) @ \d+$")

# Generous: the daemon only has to forward a handful of samples. Exceeding this
# means something is wedged, and failing loudly beats snapshotting partial data.
PROCESS_GROUP_DRAIN_TIMEOUT_SECONDS = 120.0
DEFAULT_CLIENT_TIMEOUT_SECONDS = 60.0
WEDGED_CLIENT_TIMEOUT_SECONDS = 10.0
PROCESS_GROUP_TERMINATION_TIMEOUT_SECONDS = 10.0
SIGINT_BLOCK_TIMEOUT_SECONDS = 30.0
WRONG_PACKAGE_MARKER = "wrong-find-hh-package"

ProcessCleanup = Callable[[], None]
AfterStart = Callable[[subprocess.Popen[str]], ProcessCleanup | None]


def _scrub(value: str, hh_client: str, temp_dir: Path) -> str:
    return value.replace(hh_client, "hh_client").replace(str(temp_dir), "<test_tmp>")


def _normalize_output(output: str, hh_client: str, temp_dir: Path) -> str:
    normalized: list[str] = []
    omitted_stack = False
    for line in _scrub(output, hh_client, temp_dir).splitlines():
        if line.lstrip().startswith(
            ("Called from ", "Raised at ", "Raised by ", "Re-raised at ")
        ) or STACK_FRAME.fullmatch(line):
            if not omitted_stack:
                normalized.append("<stack trace omitted>")
                omitted_stack = True
            continue
        # Handle how Exit.exit glues two stacktraces together with "\n\n"
        omitted_stack = omitted_stack and not line.strip()
        normalized.append(line)
        if line.startswith("Usage: "):
            normalized.append("<usage omitted>")
            break
    return "\n".join(normalized)


def _project_sample(
    *, sample: dict[str, Any], hh_client: str, temp_dir: Path
) -> dict[str, Any]:
    normal = sample.get("normal", {})
    projection = {
        key: normal[key]
        for key in ("client_command", "event", "exit_status", "process", "reason")
        if key in normal
    }
    ints = sample.get("int", {})
    if "exit_code" in ints:
        projection["exit_code"] = ints["exit_code"]
    denorm = sample.get("denorm", {})
    if "argv" in denorm:
        projection["argv"] = _scrub(denorm["argv"], hh_client, temp_dir)
    if "data" in denorm:
        # To avoid having to update tests when code lines change,
        # replace any exceptions with a deterministic marker
        try:
            data = json.loads(denorm["data"])
        except json.JSONDecodeError:
            data = {}
        if isinstance(data, dict) and "exn" in data:
            projection["exception"] = _scrub(data["exn"], hh_client, temp_dir)
            if "stack" in data:
                projection["stack"] = "<stack trace omitted>"
        elif normal.get("event") == "HELP":
            projection["data"] = data
    return projection


def _wait_for_process_group(
    pgid: int, timeout_seconds: float = PROCESS_GROUP_DRAIN_TIMEOUT_SECONDS
) -> None:
    """Block until no process remains in [pgid].

    hh_client's scuba daemon is not reaped by us -- it is reparented when
    hh_client exits -- so waitpid cannot see it. Signal 0 against the group is
    the portable way to ask whether any member is still alive.
    """
    deadline = time.monotonic() + timeout_seconds
    while True:
        try:
            os.killpg(pgid, 0)
        except ProcessLookupError:
            return
        except PermissionError:
            # Group still exists; a member we may not signal is enough to know.
            pass
        if time.monotonic() > deadline:
            raise TimeoutError(
                f"process group {pgid} still alive after {timeout_seconds}s"
            )
        time.sleep(0.01)


def _terminate_process_group(process: subprocess.Popen[str]) -> None:
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    if process.poll() is None:
        process.kill()
        process.wait()
    _wait_for_process_group(process.pid, PROCESS_GROUP_TERMINATION_TIMEOUT_SECONDS)


def _open_fifo_once_read(process: subprocess.Popen[str], fifo: Path) -> int:
    # Opening a FIFO write-only and non-blocking fails with ENXIO until some
    # process has it open for reading, so a successful open is itself proof that
    # hh_client reached its own open(). The caller keeps the returned fd open and
    # never writes, leaving hh_client parked in read().
    deadline = time.monotonic() + SIGINT_BLOCK_TIMEOUT_SECONDS
    while True:
        returncode = process.poll()
        if returncode is not None:
            raise RuntimeError(
                f"hh_client exited with {returncode} before opening the FIFO"
            )
        try:
            return os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
        except OSError as e:
            if e.errno != errno.ENXIO:
                raise
        if time.monotonic() > deadline:
            raise TimeoutError(
                "hh_client did not open the FIFO within "
                f"{SIGINT_BLOCK_TIMEOUT_SECONDS:g}s"
            )
        time.sleep(0.05)


def _sigint_after_opening_fifo(
    fifo: Path, client_pid: Path | None = None
) -> AfterStart:
    def after_start(process: subprocess.Popen[str]) -> ProcessCleanup:
        fifo_writer = _open_fifo_once_read(process, fifo)
        try:
            pid = int(client_pid.read_text()) if client_pid is not None else process.pid
            os.kill(pid, signal.SIGINT)
        except OSError:
            os.close(fifo_writer)
            raise
        return lambda: os.close(fifo_writer)

    return after_start


def _run_hh_client(
    argv: list[str],
    *,
    cwd: Path,
    env: dict[str, str],
    stdin: str,
    after_start: AfterStart | None,
    communicate_timeout: float,
    terminate_process_group_after_exit: bool,
) -> subprocess.CompletedProcess[str]:
    """Run hh_client in its own process group and clean up its descendants.
    See comment at the top of this module for why
    """
    with subprocess.Popen(
        argv,
        cwd=cwd,
        env=env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    ) as process:
        after_start_cleanup: ProcessCleanup | None = None
        completed_normally = False
        try:
            if after_start is not None:
                after_start_cleanup = after_start(process)
            try:
                stdout, stderr = process.communicate(stdin, timeout=communicate_timeout)
            except subprocess.TimeoutExpired as error:
                raise TimeoutError(
                    f"hh_client did not exit within {communicate_timeout:g}s"
                ) from error
            returncode = process.wait()
            completed_normally = True
        finally:
            if after_start_cleanup is not None:
                after_start_cleanup()
            if terminate_process_group_after_exit or not completed_normally:
                _terminate_process_group(process)
    if not terminate_process_group_after_exit:
        _wait_for_process_group(process.pid)
    return subprocess.CompletedProcess(argv, returncode, stdout, stderr)


def _load_telemetry(
    capture_dir: Path, hh_client: str, temp_dir: Path
) -> list[dict[str, Any]]:
    telemetry: list[dict[str, Any]] = []
    for capture_path in sorted(capture_dir.glob("scribe-*.json")):
        with capture_path.open() as capture_file:
            capture = json.load(capture_file)
        for sample in capture:
            if sample.get("normal", {}).get("event") == "PROFILE":
                continue
            telemetry.append(
                _project_sample(
                    sample=sample,
                    hh_client=hh_client,
                    temp_dir=temp_dir,
                )
            )
    return sorted(telemetry, key=lambda sample: json.dumps(sample, sort_keys=True))


def _render_stream(name: str, value: str) -> None:
    print(f"{name}:")
    print(value if value else "<empty>")


@dataclass(frozen=True)
class Case:
    args: list[str]
    cwd: str
    stdin: str
    lsp_initialize_root: str | None
    sigint_after_opening_fifo: str | None
    wedge_scribe: bool
    find_hh_expected_package: str | None


def _load_case(case_path: Path) -> Case:
    with case_path.open() as case_file:
        case: dict[str, Any] = json.load(case_file)
    unknown_fields = set(case) - {
        "args",
        "cwd",
        "lsp_initialize_root",
        "sigint_after_opening_fifo",
        "stdin",
        "wedge_scribe",
        "find_hh_expected_package",
    }
    if unknown_fields:
        raise ValueError(f"{case_path}: unknown fields: {sorted(unknown_fields)}")
    args = case.get("args")
    if not isinstance(args, list) or not all(isinstance(arg, str) for arg in args):
        raise ValueError(f"{case_path}: args must be a list of strings")
    cwd = case.get("cwd", "root")
    if not isinstance(cwd, str):
        raise ValueError(f"{case_path}: cwd must be a string")
    stdin = case.get("stdin", "")
    if not isinstance(stdin, str):
        raise ValueError(f"{case_path}: stdin must be a string")
    lsp_initialize_root = case.get("lsp_initialize_root")
    if lsp_initialize_root is not None and not isinstance(lsp_initialize_root, str):
        raise ValueError(f"{case_path}: lsp_initialize_root must be a string")
    if stdin and lsp_initialize_root is not None:
        raise ValueError(f"{case_path}: stdin and lsp_initialize_root conflict")
    sigint_after_opening_fifo = case.get("sigint_after_opening_fifo")
    if sigint_after_opening_fifo is not None and not isinstance(
        sigint_after_opening_fifo, str
    ):
        raise ValueError(f"{case_path}: sigint_after_opening_fifo must be a string")
    wedge_scribe = case.get("wedge_scribe", False)
    if not isinstance(wedge_scribe, bool):
        raise ValueError(f"{case_path}: wedge_scribe must be a boolean")
    expected_package = case.get("find_hh_expected_package")
    if expected_package not in (None, "1.0.0", "2.0.0"):
        raise ValueError(
            f"{case_path}: find_hh_expected_package must be 1.0.0 or 2.0.0"
        )
    return Case(
        args=cast(list[str], args),
        cwd=cwd,
        stdin=stdin,
        lsp_initialize_root=lsp_initialize_root,
        sigint_after_opening_fifo=sigint_after_opening_fifo,
        wedge_scribe=wedge_scribe,
        find_hh_expected_package=expected_package,
    )


def _lsp_initialize_stdin(root: Path) -> str:
    message = json.dumps(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {"rootUri": root.as_uri(), "capabilities": {}},
        },
        separators=(",", ":"),
    )
    return f"Content-Length: {len(message.encode())}\r\n\r\n{message}"


def _find_hh_command(
    wrapper: str,
    hh_client: str,
    temp_dir: Path,
    env: dict[str, str],
    expected_package: str | None,
) -> str:
    home = temp_dir / "home"
    home.mkdir()
    (home / ".hack_rc_mode").write_text("prod\n")
    packages = temp_dir / "packages"
    for version in ("latest", "1.0.0", "2.0.0"):
        executable = packages / version / "hh_client"
        executable.parent.mkdir(parents=True)
        if expected_package is None or version == expected_package:
            executable.write_text(
                "#!/bin/sh\n"
                f"echo $$ > {shlex.quote(str(temp_dir / 'hh_client.pid'))}\n"
                f'exec {shlex.quote(hh_client)} "$@"\n'
            )
        else:
            # Keep the selection out of the child's stdout/stderr: that
            # stream is snapshotted, so a sentinel there could be blessed
            # as an .exp. The marker file is the harness failure channel.
            marker = temp_dir / WRONG_PACKAGE_MARKER
            executable.write_text(
                "#!/bin/sh\n"
                f"printf %s {shlex.quote(version)} > {shlex.quote(str(marker))}\n"
                "exit 86\n"
            )
        executable.chmod(0o755)
    command = temp_dir / "wrapper" / "hh_client"
    command.parent.mkdir()
    shutil.copy2(wrapper, command)
    command.chmod(0o755)
    env.update(
        HOME=str(home),
        HH_HOME="",
        HH_RUST_FIND_HH="0",
        FIND_HH_BIN=str(packages),
        FIND_HH_TMP=str(temp_dir / "package-cache"),
    )
    return str(command)


def _check_fixture_dispatch(
    temp_dir: Path,
    case_path: Path,
    expected_package: str | None,
    result: subprocess.CompletedProcess[str],
) -> None:
    if (temp_dir / "unexpected-fbpkg").exists():
        raise RuntimeError(f"find_hh attempted a package fetch for {case_path.name}")
    wrong_package = temp_dir / WRONG_PACKAGE_MARKER
    if wrong_package.exists():
        raise RuntimeError(
            f"find_hh selected package {wrong_package.read_text()!r} for "
            f"{case_path.name}, expected {expected_package!r}"
        )
    # Only an accepted fixture writes this immediately before exec. Treat
    # shell exec failures as harness failures too, not client exit codes.
    if not (temp_dir / "hh_client.pid").is_file() or result.returncode in (126, 127):
        raise RuntimeError(
            f"find_hh did not start the fixture client for {case_path.name} "
            f"(exit {result.returncode})\n{result.stderr}"
        )


def main() -> None:
    if len(sys.argv) not in (4, 5):
        raise SystemExit(
            f"usage: {sys.argv[0]} CASE HH_CLIENT FAKE_SCRIBE_CAT [FIND_HH]"
        )
    case_path = Path(sys.argv[1])
    sample_repos = case_path.parent / "sample_repos"
    hh_client = sys.argv[2]
    fake_scribe_cat = sys.argv[3]
    find_hh = sys.argv[4] if len(sys.argv) == 5 else None
    case = _load_case(case_path)
    with tempfile.TemporaryDirectory() as raw_temp_dir:
        temp_dir = Path(raw_temp_dir)
        shutil.copytree(sample_repos, temp_dir, dirs_exist_ok=True)
        cwd = temp_dir / case.cwd
        sigint_fifo = (
            cwd / case.sigint_after_opening_fifo
            if case.sigint_after_opening_fifo is not None
            else None
        )
        if sigint_fifo is not None:
            os.mkfifo(sigint_fifo)
        after_start = (
            _sigint_after_opening_fifo(
                sigint_fifo, temp_dir / "hh_client.pid" if find_hh is not None else None
            )
            if sigint_fifo is not None
            else None
        )
        capture_dir = temp_dir / "capture"
        capture_dir.mkdir()

        bin_dir = temp_dir / "bin"
        bin_dir.mkdir()
        (bin_dir / "scribe_cat").symlink_to(fake_scribe_cat)
        if find_hh is not None:
            fbpkg = bin_dir / "fbpkg"
            marker = shlex.quote(str(temp_dir / "unexpected-fbpkg"))
            fbpkg.write_text(f"#!/bin/sh\ntouch {marker}\nexit 1\n")
            fbpkg.chmod(0o755)

        env = os.environ.copy()
        env["HH_TEST_MODE"] = "1"
        env["HH_TEST_SCRIBE_CAPTURE_DIR"] = str(capture_dir)
        env.pop("HH_TEST_SCRIBE_WEDGE", None)
        if case.wedge_scribe:
            env["HH_TEST_SCRIBE_WEDGE"] = "1"
        env["PATH"] = os.pathsep.join([str(bin_dir), env.get("PATH", "")])
        stdin = (
            _lsp_initialize_stdin(temp_dir / case.lsp_initialize_root)
            if case.lsp_initialize_root is not None
            else case.stdin
        )
        command = (
            _find_hh_command(
                find_hh, hh_client, temp_dir, env, case.find_hh_expected_package
            )
            if find_hh is not None
            else hh_client
        )
        result = _run_hh_client(
            [command, *case.args],
            cwd=cwd,
            env=env,
            stdin=stdin,
            after_start=after_start,
            communicate_timeout=(
                WEDGED_CLIENT_TIMEOUT_SECONDS
                if case.wedge_scribe
                else SIGINT_BLOCK_TIMEOUT_SECONDS
                if sigint_fifo is not None
                else DEFAULT_CLIENT_TIMEOUT_SECONDS
            ),
            terminate_process_group_after_exit=case.wedge_scribe,
        )

        if find_hh is not None:
            _check_fixture_dispatch(
                temp_dir, case_path, case.find_hh_expected_package, result
            )

        print("=== hh_client result ===")
        print(f"argv: hh_client {_scrub(shlex.join(case.args), hh_client, temp_dir)}")
        print(f"cwd: {_scrub(str(cwd), hh_client, temp_dir)}")
        print(f"exit_code: {result.returncode}")
        if case.wedge_scribe:
            print("client exit bounded with wedged scribe: True")
        _render_stream("stdout", _normalize_output(result.stdout, hh_client, temp_dir))
        _render_stream("stderr", _normalize_output(result.stderr, hh_client, temp_dir))
        print("=== telemetry ===")
        print(
            json.dumps(
                _load_telemetry(capture_dir, hh_client, temp_dir),
                indent=2,
                sort_keys=True,
            )
        )


if __name__ == "__main__":
    main()
