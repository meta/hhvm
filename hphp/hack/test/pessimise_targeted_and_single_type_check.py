#!/usr/bin/env python3
# pyre-strict

import io
import os
import subprocess
import sys
from typing import List


def arg_extract(args: List[str], arg: str) -> str:
    index = args.index(arg)
    value = args.pop(index + 1)
    del args[index]
    return value


def main() -> None:
    # Parsing the arguments is not straightforward as pessimise_and_single_type_check
    # must be aware of all the arguments supported by hh_single_type_check, but we do
    # not want to duplicate the list here (which is long and moving fast).
    # We thus use the following ad-hoc strategy:
    #
    # 1. extract the paths to hh_stc, hh_pessimisation, pessimised_hhi from the arguments
    # 2. delete the binary name
    # 3. extract all the *.php strings, assuming these are the files being tested
    # 4. the remaining arguments are the flags to be passed to hh_single_type_checks

    arguments: List[str] = sys.argv
    hh_stc_path: str = arg_extract(arguments, "--hh-stc-path")
    hh_pessimisation_path: str = arg_extract(arguments, "--hh-pessimisation-path")
    pessimised_hhi_path: str = arg_extract(arguments, "--pessimised-hhi-path")
    del arguments[0]

    hh_stc_arguments: List[str] = []
    files: List[str] = []

    for a in arguments:
        if a.endswith(".php"):
            files.append(a)
        else:
            hh_stc_arguments.append(a)

    # pessimise all the current tests
    # 1. run hh_single_type_check to collect pessimisation targets
    cmd: List[str] = (
        [hh_stc_path, "--config", 'log_levels={"pessimise":1}']
        + hh_stc_arguments
        + ["--config", "like_casts=true"]
        + ["--error-format", "plain"]
        + files
    )

    subprocess.call(cmd)

    for file in files:
        # 2. run targeted hh_pessimisation on each file
        cmd: List[str] = [
            hh_pessimisation_path,
            "--file=" + file,
            "--update",
            "--forward",
            "--pessimisation-targets=" + file + ".out",
            "--enable-multifile-support",
        ]
        subprocess.run(cmd)

    # identify the extra builtins used
    extra_builtins: List[str] = []
    while True:
        try:
            extra_builtins.append(arg_extract(hh_stc_arguments, "--extra-builtin"))
        except ValueError:
            break

    # pessimise the extra builtins and build the cli options
    # - if extra_builtin refer to <path>/<file>, the pessimised extra builtin is
    #   generated inside <path>/pessimised_hhi/<file>
    # - to avoid a race condition, the hhi is first pessimised into <file>.pess.<PID>,
    #   and then moved to <path>/pessimised_hhi/<file>
    extra_builtins_opts: List[str] = []
    for file in extra_builtins:
        (file_dir, file_name) = os.path.split(file)
        unique_output_file = file + ".pess." + str(os.getpid())
        pessimised_file_dir = os.path.join(file_dir, "pessimised_hhi")
        pessimised_file = os.path.join(pessimised_file_dir, file_name)

        if not (os.path.exists(pessimised_file)):
            cmd: List[str] = [
                hh_pessimisation_path,
                "--file",
                file,
                "--forward",
                "--output-file",
                unique_output_file,
            ]
            subprocess.run(cmd)
            os.makedirs(pessimised_file_dir, exist_ok=True)
            # this is expected to be atomic
            os.rename(unique_output_file, pessimised_file)

        extra_builtins_opts.append("--extra-builtin")
        extra_builtins_opts.append(pessimised_file)

    # pessimised tests have extension file+".pess"
    # invoke hh_single_type_check on the pessimised tests
    pessimised_files: List[str] = list(map(lambda file: file + ".pess", files))
    cmd: List[str] = (
        [hh_stc_path]
        + ["--batch-files"]
        + hh_stc_arguments
        + extra_builtins_opts
        + ["--custom-hhi-path", pessimised_hhi_path]
        + pessimised_files
    )
    subprocess.run(cmd)

    # Remark: in batch-mode the out files will have extension ".pess.out"
    # instead of ".out"


if __name__ == "__main__":
    main()
