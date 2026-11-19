#!/usr/bin/env python3

"""Stand-in for scribe_cat that hands hh_client's Scuba samples to the harness.

test_runner puts this on PATH under the name scribe_cat and points
HH_TEST_SCRIBE_CAPTURE_DIR at a directory it will scan once hh_client's process
group has drained.
"""

# pyre-strict

import json
import os
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

# test_runner only reads back files under this prefix, so a write interrupted
# midway is never mistaken for a complete capture.
CAPTURE_PREFIX = "scribe-"

PARTIAL_PREFIX = ".partial-"


def main() -> None:
    if os.environ.get("HH_TEST_SCRIBE_WEDGE"):
        # Never read stdin and never exit; the test runner gives hh_client a
        # deadline and then kills this isolated process group.
        time.sleep(600)
        return
    # Drain stdin before anything that can fail, so a misconfigured harness
    # cannot leave hh_client blocked on the pipe or hand it EPIPE mid-sample.
    samples: list[Any] = [json.loads(line) for line in sys.stdin if line.strip()]
    if len(sys.argv) != 2 or not sys.argv[1]:
        raise ValueError("expected one nonempty Scuba category")
    capture_dir = Path(os.environ["HH_TEST_SCRIBE_CAPTURE_DIR"])
    fd, partial = tempfile.mkstemp(
        prefix=PARTIAL_PREFIX, suffix=".json", dir=capture_dir
    )
    with os.fdopen(fd, "w") as output:
        json.dump(samples, output, sort_keys=True)
        output.write("\n")
    name = Path(partial).name.replace(PARTIAL_PREFIX, CAPTURE_PREFIX, 1)
    os.replace(partial, capture_dir / name)


if __name__ == "__main__":
    main()
