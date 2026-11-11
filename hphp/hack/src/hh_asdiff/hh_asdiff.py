#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the "hack" directory of this source tree.

# pyre-unsafe

import argparse
import difflib
import itertools
import logging
import os
import sys
from typing import AnyStr, Dict, Iterator, List, Optional, Sequence, Tuple

from hphp.hack.src.hh_asdiff import diff, parsing


log = logging.getLogger("hack.asdiff")


def parse_args(argv: List[str]) -> argparse.Namespace:
    program = os.path.basename(argv[0])
    ap = argparse.ArgumentParser(
        program,
        description="""\
A tool to compare and diff/pretty-print bytecode differences based on at least
one precomputed (expected) output.  It is powerful enough to compare multiple
input files (e.g., recursively the entire codebase written in Hack) without
storing the actual output on disk (for convenience & efficiency), as well as
simple ranking/filtering (e.g., on which files HHAS differs least/most lines).
""",
        epilog=f"""\
Examples:
  # Compare two compilation results stored in an expected and an actual file
  {program} my/hack/test_dir/compiled1.hhas /tmp/changed-bc/compiled1.hhas

  # Compare compilation output of multiple files on-the-fly
  find all-hack -name '*.php' | hh_single_compile --input-file-list /dev/stdin\
    | {program} all-hack.expected.hhas  # stops on first mismatch unless --all
  """,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--all",
        action="store_true",
        help="do not stop after a single HHAS mismatches (consider with --no-diff)",
    )
    ap.add_argument(
        "--no-diff",
        dest="diff",
        action="store_false",
        help="do not show any diffs, just fail if HHAS mismatches",
    )
    ap.add_argument(
        "--diff-smallest",
        metavar="K",
        type=int,
        default=0,
        help="if positive, show smallest K diffs (implies --all)"
        " (note: K inputs are kept in memory, and O(log K) time factor)",
    )
    ap.add_argument(
        "-s",
        "--report-identical-files",
        action="store_true",
        help="report when two HHASes are identical",
    )
    ap.add_argument("--report-summary", action="store_true", help="report")
    ap.add_argument("expected_file", type=argparse.FileType("rb"), help="Expected HHAS")
    ap.add_argument(
        "actual_file",
        nargs="?",
        type=argparse.FileType("rb"),
        default=sys.stdin,
        help="Actual HHAS; a .hhas file or stdin (piped from hh_single_compile)",
    )
    opts = ap.parse_args(argv[1:])
    return opts


HhasResult = Tuple[parsing.MarkedFilename, Sequence[str]]


class DiffHandler:
    def __init__(self, opts: argparse.Namespace):
        self._opts = opts
        self.call_count = 0
        self.ranker = diff.UnifiedDiffRanker(opts.diff_smallest)

    def __call__(self, exp: HhasResult, act: HhasResult):
        exp_filename, exp_lines = exp
        act_filename, act_lines = act

        def unified_diff():
            return difflib.unified_diff(
                exp_lines,
                act_lines,
                fromfile=exp_filename,
                tofile=act_filename,
            )

        if self._opts.diff_smallest:
            self.ranker(exp_filename or "", exp_lines, act_lines)
        elif self._opts.diff:
            sys.stdout.writelines(unified_diff())

        self.call_count += 1

    def __str__(self) -> str:
        """Prints the info for the current smallest diffs"""
        ret = f"diffs handled: {self.call_count}\n"
        if self._opts.diff_smallest:
            smallest = "\n".join(
                f"size={size} @ {path}" for (size, path), _, _ in iter(self.ranker)
            )
            ret += f"K-th smallest:\n{smallest}\n\n"
        return ret


def report_summary(exp_file: Iterator[AnyStr], act_file: Iterator[AnyStr]):
    act: Dict[Optional[str], Sequence[str]] = {}
    exp: Dict[Optional[str], Sequence[str]] = {}
    for fn, content in parsing.split_lines(exp_file):
        if fn is None:
            log.error("Found file without name")
        exp[fn] = content
    act = {}
    for fn, content in parsing.split_lines(act_file):
        if fn is None:
            log.error("Found file without name")
        act[fn] = content
    exp_keys = set(exp.keys())
    act_keys = set(act.keys())
    # pyre-fixme[6]: For 1st argument expected
    #  `pyre_extensions.PyreReadOnly[Iterable[SupportsRichComparisonT]]` but got
    #  `Set[Optional[str]]`.
    all_keys = sorted(exp_keys.union(act_keys))

    for k in all_keys:
        from_exp = exp.get(k)
        from_act = act.get(k)
        if from_exp is None:
            print("|NotInExp|", k)
        elif from_act is None:
            print("|NotInAct|", k)
        elif (
            next(filter(lambda l: "#### NotImpl:" in l, iter(from_act)), None)
            is not None
        ):
            print("|PrintNotImplA|", k)
        elif (
            next(filter(lambda l: "#### NotImpl:" in l, iter(from_exp)), None)
            is not None
        ):
            print("|PrintNotImplE|", k)
        elif diff.equal_lines(iter(from_exp), iter(from_act)):
            print("|Identical|", k)
        else:
            print("|Mismatch|", k)


def main(args: List[str]) -> int:
    exit_code = 0
    opts = parse_args(sys.argv)
    compare_all = opts.all or opts.diff_smallest
    try:
        if opts.report_summary:
            report_summary(opts.expected_file, opts.actual_file)
            return exit_code
        compare_count = 0
        diff_handler = DiffHandler(opts)
        for exp, act in itertools.zip_longest(
            parsing.split_lines(opts.expected_file),
            parsing.split_lines(opts.actual_file),
            fillvalue=None,
        ):
            compare_count += 1
            if None in (exp, act):
                exit_code |= 1 << 1
                source, filename = (
                    # pyre-fixme[16]: `Optional` has no attribute `__getitem__`.
                    ("expected", act[0]) if exp is None else ("actual", exp[0])
                )
                log.error(f"missing filename in {source} HHAS: {filename}")
                if not compare_all:
                    break
                continue

            # pyre-fixme[23]: Unable to unpack `Optional[Tuple[Optional[str],
            #  Sequence[str]]]` into 2 values.
            exp_filename, exp_lines = exp
            # pyre-fixme[23]: Unable to unpack `Optional[Tuple[Optional[str],
            #  Sequence[str]]]` into 2 values.
            act_filename, act_lines = act
            if exp_filename != act_filename:
                exit_code |= 1 << 2
                log.error(
                    "filename mismatch:\n"
                    f"expected: {exp_filename}\n"
                    f"  actual: {act_filename}\n"
                    "Did you ensure stable order in `hh_(single_)compile "
                    "--input-file-list`?"
                )
                if not compare_all:
                    break
                continue

            # pyrefly: ignore [bad-argument-type]
            if diff.equal_lines(exp_lines, act_lines):
                if opts.report_identical_files:
                    print("identical:", exp_filename)
                continue

            exit_code |= 1 << 3
            # pyre-fixme[6]: For 1st argument expected `Tuple[Optional[str],
            #  Sequence[str]]` but got `Optional[Tuple[Optional[str], Sequence[str]]]`.
            # pyre-fixme[6]: For 2nd argument expected `Tuple[Optional[str],
            #  Sequence[str]]` but got `Optional[Tuple[Optional[str], Sequence[str]]]`.
            diff_handler(exp, act)

            if not compare_all:
                break

        print("files checked:", compare_count)
        print(str(diff_handler), end="")
        if opts.diff:
            for rank, ((_, exp_file), exp_lines, act_lines) in enumerate(
                diff_handler.ranker, start=1
            ):
                print(f"\n== Rank #{rank} diff:", exp_file)
                gen = difflib.unified_diff(exp_lines, act_lines)
                next(gen)  # skip: +++ file1
                next(gen)  # skip: --- file2
                sys.stdout.writelines(gen)
    finally:
        opts.expected_file.close()
        opts.actual_file.close()

    return exit_code


def invoke_main() -> None:
    logging.basicConfig(format="%(levelname)s: %(message)s")
    log.setLevel(logging.WARN)

    sys.exit(main(sys.argv))


if __name__ == "__main__":
    invoke_main()
