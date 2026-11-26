#!/usr/bin/env python3

# pyre-unsafe

# Gather all of the relevant files from buck file groups and execute
# `hh_single_type_check` with the correct flags

import argparse
import os
import subprocess as p
import sys
from typing import List

FIXME_CODES: List[int] = [
    # "Missing symbol:" used to break dependency cycles between files that might
    # be mutually recursive or referential in some form (e.g.: any class with
    # a `__Sealed` attribute).
    2049,
    # `__toString` should return `string`: a handful of `<<__Native>>` methods
    # are declared `?string` to match their underlying `OptString` C++ return,
    # which Hack's structural rule for `__toString` won't accept without a
    # FIXME at the decl site.
    3020,
    # Typing of idx($this, _) is broken; also covers `?string` flowing into
    # `string`-typed call sites for `<<__Native>>` functions whose `.php` was
    # tightened from `string` to `?string` to match an `OptString` C++ return.
    4110,
    # Method override-incompatibility for `<<__Native>>` methods whose `.php`
    # was tightened from `string` to `?string`; their interface/parent
    # declarations still say `string` and haven't been migrated yet.
    4341,
    # `?string` used as a dict key after one of the tightened `<<__Native>>`
    # functions started returning `?string` instead of `string`.
    4371,
    # "Memoizing object parameters requires the capability AccessGlobals:" for
    # now, we're allowing this in some places like `create_opaque_value`
    4447,
    # There are some functions that don't have *quite* correct coeffects; if
    # we're going to change these it should be done separate from an initial
    # pass making systemlib "clean."
    4390,
]

FLAGS: List[str] = [
    "--no-builtins",
    "--config",
    "is_systemlib=true",
    "--config",
    "enable_no_auto_dynamic=true",
    "--config",
    "allowed_fixme_codes_strict=" + ",".join(map(str, FIXME_CODES)),
    "--config",
    "allowed_decl_fixme_codes=" + ",".join(map(str, FIXME_CODES)),
]


def get_files_in(path: str) -> List[str]:
    all_files = []
    for root, _, files in os.walk(path):
        all_files.extend(os.path.join(root, f) for f in files)
    return all_files


def main():
    parser = argparse.ArgumentParser(
        description="Gather PHP files in given directories and run `hh_single_type_check`"
    )
    parser.add_argument("paths", type=str, help="paths to traverse", nargs="+")
    parser.add_argument("--hhstc-path", type=str, help="`hh_single_type_check` to run")
    parser.add_argument(
        "--report-number-of-files",
        action="store_true",
        help="instead of running the typechecker, just print the number of files we'd typecheck",
    )
    args = parser.parse_args()
    files = []
    for path in args.paths:
        files.extend(get_files_in(path))
    if args.report_number_of_files:
        print(len(list(filter(lambda f: f.endswith("php"), files))))
        return
    sys.exit(p.run([args.hhstc_path] + FLAGS + files).returncode)


if __name__ == "__main__":
    main()
