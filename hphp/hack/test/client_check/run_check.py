# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the "hack" directory of this source tree.

import json
import os
import subprocess
import sys
import tempfile
from contextlib import suppress
from pathlib import Path


def normalize_json_line(line: str) -> str:
    """Drop `last_recheck`, which holds timings and random ids, and mask
    `version`, which depends on the build."""
    payload = json.loads(line)
    payload.pop("last_recheck", None)
    if "version" in payload:
        payload["version"] = "<version>"
    return json.dumps(payload, separators=(",", ":"))


def main() -> None:
    source, client, server, *flags = sys.argv[1:]
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp, "repo").resolve()
        root.mkdir()
        (root / ".hg").mkdir()
        (root / ".hhconfig").touch()
        file = root / Path(source).name
        file.write_bytes(Path(source).read_bytes())
        flags = [str(file) if flag == "{file}" else flag for flag in flags]
        (root / "hh.conf").write_text(
            "max_workers=1\nuse_mini_state=false\nwarnings_default_all=true\n"
        )
        env = dict(
            os.environ,
            HH_TEST_MODE="1",
            HH_TMPDIR=temp,
            HH_LOCALCONF_PATH=str(root),
            HH_SERVER_PATH=server,
            TZ="UTC",
        )
        try:
            result = subprocess.run(
                [client, "check", "--no-load", *flags, str(root)],
                env=env,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=60,
            )
            output = result.stdout
            if "--json" in flags or "--jsonl" in flags:
                output = "".join(
                    normalize_json_line(line) + "\n" for line in output.splitlines()
                )
            # Startup progress goes to stderr; show it only if the check didn't run.
            if result.returncode not in (0, 2):
                output += result.stderr
            output = output.replace(str(root), "<repo>")
            print(f"exit_code: {result.returncode}")
            print(output, end="")
        finally:
            with suppress(OSError, subprocess.TimeoutExpired):
                subprocess.run(
                    [client, "stop", str(root)],
                    env=env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=30,
                    check=False,
                )
