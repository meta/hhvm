# pyre-strict

from __future__ import absolute_import, division, print_function, unicode_literals

import json
import os
from typing import Any, Dict, List, Tuple

import hphp.hack.test.integration.common_tests as common_tests
from hphp.hack.test.integration.test_case import TestCase


config_error_exit_code = 224
input_error_exit_code = 10


class TestValidateIsolation(TestCase[common_tests.CommonTestDriver]):
    """`hh --validate-isolation`: checking one candidate package in place.

    Apart from the other integration tests because these need a server that
    permits decl repackaging, which is off by default.
    """

    @classmethod
    def get_test_driver(cls) -> common_tests.CommonTestDriver:
        return common_tests.CommonTestDriver()

    def test_validate_isolation(self) -> None:
        # validate_user.php references validate_helper.php, and nothing else
        # references either. Whether the pair is isolatable therefore turns
        # entirely on which of the two the candidate contains, so the same
        # fixture serves as both the passing and the failing case.
        files = {
            "validate_helper.php": "<?hh\nfunction validate_helper(): void {}\n",
            "validate_user.php": (
                "<?hh\nfunction validate_user(): void {\n  validate_helper();\n}\n"
            ),
            # Names a package of its own, so no synthesized package can claim
            # it however its include paths are written.
            "validate_overridden.php": (
                '<?hh\n<<file: __PackageOverride("foo")>>\n'
                "function validate_overridden(): void {}\n"
            ),
        }
        for filename, contents in files.items():
            with open(os.path.join(self.test_driver.repo_dir, filename), "w") as f:
                f.write(contents)

        self.test_driver.start_hh_server(
            changed_files=list(files.keys()),
            # The mode refuses to run without this: it rebuilds decls under a
            # synthesized package, which is only safe on a server dedicated to
            # the analysis.
            args=["--no-load", "--config", "isolation_allow_decl_repackaging=true"],
        )

        def validate(candidate: List[str]) -> Tuple[Dict[str, Any], int]:
            list_file = os.path.join(self.test_driver.repo_dir, "candidate.txt")
            with open(list_file, "w") as f:
                f.write("\n".join(candidate) + "\n")
            output, _, retcode = self.test_driver.run_check(
                options=["--validate-isolation", list_file, "--json"]
            )
            # This mode emits one JSON document and nothing else. Failing here
            # with the actual output beats an opaque decode error if a stray
            # line appears.
            try:
                return json.loads(output), retcode
            except json.JSONDecodeError as exn:
                self.fail(f"Expected a lone JSON document, got {output!r} ({exn})")

        # The candidate leaves out its only referrer, so the call into it is a
        # violation. This is the case that shows the check can fail at all: a
        # validator that only ever says yes would pass the other two.
        result, retcode = validate(["validate_helper.php"])
        # The referrer and the candidate itself: the candidate's own files are
        # checked too, because a package reaching into its own excluded path is
        # reported inside it rather than at a referring site.
        self.assertEqual(2, result["files_checked"])
        self.assertFalse(result["isolatable"])
        self.assertEqual([], result["unknown_files"])
        self.assertEqual(
            ["validate_user.php"], [v["referrer"] for v in result["violations"]]
        )
        self.assertEqual(2, retcode)

        # The same two files, with the referrer now inside the candidate. The
        # reference has not gone away; it has become internal.
        result, retcode = validate(["validate_helper.php", "validate_user.php"])
        self.assertTrue(result["isolatable"])
        self.assertEqual([], result["violations"])
        self.assertEqual(0, retcode)

        # A path the naming table never saw contributes no referrers, so
        # without being named it would read exactly like a clean result.
        result, retcode = validate(["validate_nonexistent.php"])
        self.assertEqual(["validate_nonexistent.php"], result["unknown_files"])
        self.assertEqual([], result["violations"])
        # No violations, but the candidate was not wholly checked, so the
        # verdict is not a clean bill of health either.
        self.assertFalse(result["isolatable"])
        self.assertEqual(10, retcode)

        # A file that names its own package cannot be moved into the candidate,
        # so the candidate that would have been checked is not the one asked
        # about. Reported like an unknown path rather than passed over.
        result, retcode = validate(["validate_overridden.php"])
        self.assertEqual(["validate_overridden.php"], result["overridden_files"])
        self.assertEqual([], result["unknown_files"])
        self.assertEqual([], result["violations"])
        self.assertFalse(result["isolatable"])
        self.assertEqual(10, retcode)

        # Everything above goes through --json. The human-readable path is what
        # a person actually sees, and it renders from the same result, so it is
        # checked once here rather than pinned line by line.
        list_file = os.path.join(self.test_driver.repo_dir, "candidate.txt")
        with open(list_file, "w") as f:
            f.write("validate_helper.php\n")
        human, _, retcode = self.test_driver.run_check(
            options=["--validate-isolation", list_file]
        )
        self.assertEqual(2, retcode)
        self.assertIn("Not isolatable", human)
        # The violation is named, with the file and line a reader needs to go
        # and look; a verdict on its own would not be actionable.
        self.assertIn("validate_user.php", human)

        # A list file that cannot be read is an input problem, and is reported
        # as one rather than escaping as an exception from the client.
        missing, _, retcode = self.test_driver.run_check(
            options=["--validate-isolation", "no_such_list_file.txt"]
        )
        self.assertEqual(10, retcode)

    def test_validate_isolation_leaves_the_server_alone(self) -> None:
        # Validation deletes the candidate's decls from the server's heaps so
        # they get rebuilt knowing about the synthesized package. Those heaps
        # are process-wide, so the check is only usable if the server it ran in
        # is indistinguishable afterwards. Nothing here looks at the verdict:
        # these are the probes for what validation leaves behind.
        files = {
            "leftover_helper.php": "<?hh\nfunction leftover_helper(): void {}\n",
            "leftover_user.php": (
                "<?hh\nfunction leftover_user(): void {\n  leftover_helper();\n}\n"
            ),
        }
        for filename, contents in files.items():
            with open(os.path.join(self.test_driver.repo_dir, filename), "w") as f:
                f.write(contents)

        self.test_driver.start_hh_server(
            changed_files=list(files.keys()),
            # The mode refuses to run without this: it rebuilds decls under a
            # synthesized package, which is only safe on a server dedicated to
            # the analysis.
            args=["--no-load", "--config", "isolation_allow_decl_repackaging=true"],
        )

        def validate() -> str:
            list_file = os.path.join(self.test_driver.repo_dir, "leftover.txt")
            with open(list_file, "w") as f:
                f.write("leftover_helper.php\n")
            output, _, _ = self.test_driver.run_check(
                options=["--validate-isolation", list_file, "--json"]
            )
            return output

        helper = os.path.join(self.test_driver.repo_dir, "leftover_helper.php")

        def write_helper(signature: str) -> None:
            with open(helper, "w") as f:
                f.write(f"<?hh\nfunction leftover_helper({signature}): void {{}}\n")

        def errors_from_a_recheck() -> str:
            # Giving the helper a parameter makes the call in leftover_user.php
            # wrong, and wrong in a way only its *decl* can reveal. A check with
            # no file change would be answered from cache and could not tell a
            # corrupted heap from a healthy one, so the file is edited either
            # side of validation and the two rechecks compared.
            write_helper("int $_")
            errors, _, _ = self.test_driver.run_check()
            write_helper("")
            return errors

        before = errors_from_a_recheck()
        # Reads the dependency graph rather than the decl heaps, so it is the
        # probe for edges leaking out of the typecheck validation runs.
        clusters_before, _, _ = self.test_driver.run_check(
            options=["--isolation", "--json"]
        )

        first = validate()

        after = errors_from_a_recheck()
        clusters_after, _, _ = self.test_driver.run_check(
            options=["--isolation", "--json"]
        )
        # The arity error is the point: it is reported off the rebuilt decl of
        # the very file validation took away and put back.
        self.assertIn("leftover_user.php", before)
        self.assertEqual(before, after)
        self.assertEqual(clusters_before, clusters_after)

        # A decl rebuilt under the synthesized package and left behind would
        # make the second run disagree with the first.
        self.assertEqual(first, validate())

    def test_validate_isolation_refuses_without_the_config(self) -> None:
        # The mode rebuilds decls under a synthesized package, which is only
        # safe on a server dedicated to it. Off by default, so a server that was
        # not started for this refuses rather than quietly disturbing its heaps.
        with open(os.path.join(self.test_driver.repo_dir, "gated.php"), "w") as f:
            f.write("<?hh\nfunction gated(): void {}\n")
        self.test_driver.start_hh_server(
            changed_files=["gated.php"], args=["--no-load"]
        )

        list_file = os.path.join(self.test_driver.repo_dir, "gated.txt")
        with open(list_file, "w") as f:
            f.write("gated.php\n")
        output, _, retcode = self.test_driver.run_check(
            options=["--validate-isolation", list_file]
        )
        # A refusal is reported, not raised: the caller is told what to change
        # and the server it asked is still running afterwards.
        self.assertEqual(config_error_exit_code, retcode)
        self.assertIn("isolation_allow_decl_repackaging", output)
        self.test_driver.check_cmd(["No errors!"])

    def test_validate_isolation_refuses_the_rust_backend(self) -> None:
        # The Rust backend keeps its own copy of the package configuration, so
        # the synthesized package never reaches the code that enforces packages
        # and every file stays where it was. Refused rather than answered
        # wrongly. The two companion settings are what stop hh.conf turning the
        # Rust backend back off before the check can be reached.
        with open(os.path.join(self.test_driver.repo_dir, "rusty.php"), "w") as f:
            f.write("<?hh\nfunction rusty(): void {}\n")
        self.test_driver.start_hh_server(
            changed_files=["rusty.php"],
            args=[
                "--no-load",
                "--config",
                "isolation_allow_decl_repackaging=true",
                "--config",
                "rust_provider_backend=true",
                "--config",
                "shm_use_sharded_hashtbl=true",
                "--config",
                "populate_member_heaps=false",
            ],
        )

        list_file = os.path.join(self.test_driver.repo_dir, "rusty.txt")
        with open(list_file, "w") as f:
            f.write("rusty.php\n")
        output, _, retcode = self.test_driver.run_check(
            options=["--validate-isolation", list_file]
        )
        self.assertEqual(config_error_exit_code, retcode)
        self.assertIn("rust_provider_backend", output)
        self.assertTrue(output.startswith("Did not run: "), output)
        self.test_driver.check_cmd(["No errors!"])

    def test_validate_isolation_refuses_an_empty_list(self) -> None:
        # Nothing to check is not the same as nothing wrong, so it is an input
        # error rather than a pass.
        self.test_driver.start_hh_server(changed_files=[], args=["--no-load"])

        list_file = os.path.join(self.test_driver.repo_dir, "empty.txt")
        with open(list_file, "w") as f:
            f.write("\n")
        output, stderr, retcode = self.test_driver.run_check(
            options=["--validate-isolation", list_file]
        )
        self.assertEqual(input_error_exit_code, retcode, output)
        self.assertIn("names no files", stderr)

    def test_validate_isolation_names_every_missing_setting(self) -> None:
        # Each setting takes a restart, so a server missing both says so at
        # once rather than sending the caller round twice.
        with open(os.path.join(self.test_driver.repo_dir, "both.php"), "w") as f:
            f.write("<?hh\nfunction both(): void {}\n")
        self.test_driver.start_hh_server(
            changed_files=["both.php"],
            args=[
                "--no-load",
                "--config",
                "rust_provider_backend=true",
                "--config",
                "shm_use_sharded_hashtbl=true",
                "--config",
                "populate_member_heaps=false",
            ],
        )

        list_file = os.path.join(self.test_driver.repo_dir, "both.txt")
        with open(list_file, "w") as f:
            f.write("both.php\n")
        output, _, retcode = self.test_driver.run_check(
            options=["--validate-isolation", list_file]
        )
        self.assertEqual(config_error_exit_code, retcode)
        self.assertIn("isolation_allow_decl_repackaging", output)
        self.assertIn("rust_provider_backend", output)
