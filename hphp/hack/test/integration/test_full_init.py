# pyre-strict

from __future__ import absolute_import, division, print_function, unicode_literals

import json
import os
import re
import time
import unittest
from typing import List, Optional, Tuple

import hphp.hack.test.integration.common_tests as common_tests
from hphp.hack.test.integration.hh_paths import hh_client


class FreshInitTestDriver(common_tests.CommonTestDriver):
    # pyrefly: ignore [bad-override]
    def write_load_config(
        self, use_serverless_ide: bool = False, use_saved_state: bool = False
    ) -> None:
        # Fresh init tests don't care about which files changed, so we can
        # just use the default .hhconfig in the template repo
        pass

    # pyrefly: ignore [bad-override]
    def check_cmd(
        self,
        expected_output: Optional[List[str]],
        stdin: Optional[str] = None,
        options: Optional[List[str]] = None,
        retries: int = 30,
        assert_loaded_saved_state: bool = False,
    ) -> Tuple[str, str]:
        if options is None:
            options = []
        time.sleep(2)  # wait for Hack to catch up with file system changes

        root = self.repo_dir + os.path.sep
        (output, err, retcode) = self.proc_call(
            [
                hh_client,
                "check",
                "--retries",
                "120",
                "--no-load",
                "--error-format",
                "raw",
                "--config",
                "max_workers=2",
                self.repo_dir,
            ]
            + list(map(lambda x: x.format(root=root), options)),
            stdin=stdin,
        )

        if (retcode == 6 or retcode == 7) and retries > 0:
            # 6 = "No_server_running_should_retry" or "Server_hung_up_should_retry"
            # 7 = "Out_of_time" or "Out_of_retries"
            return self.check_cmd(expected_output, stdin, options, retries - 1)
        if retcode == 7:
            raise unittest.SkipTest("Hack server exit code 7 - out of time/retries")
        self.assertIn(retcode, [0, 2])

        if expected_output is not None:
            self.assertCountEqual(
                map(lambda x: x.format(root=root), expected_output), output.splitlines()
            )
        return output, err

    def assertEqualString(
        self, first: str, second: str, msg: Optional[str] = None
    ) -> None:
        root = self.repo_dir + os.path.sep
        second = second.format(root=root)
        self.assertEqual(first, second, msg)


def parse_isolation_json(output):
    """Split `--json` isolation output into its clusters and its summary.

    One JSON object per cluster, then the summary document last. The clusters
    are written by the server as it finds them, so they arrive as lines rather
    than inside one document.
    """
    lines = [line for line in output.splitlines() if line.strip()]
    if not lines:
        raise AssertionError(f"Expected JSON output, got {output!r}")
    try:
        summary_doc = json.loads(lines[-1])
        clusters = [json.loads(line) for line in lines[:-1]]
    except json.JSONDecodeError as exn:
        raise AssertionError(f"Unparseable isolation output {output!r} ({exn})")
    return clusters, summary_doc


class TestFreshInit(common_tests.CommonTests):
    @classmethod
    def get_test_driver(cls) -> common_tests.CommonTestDriver:
        return common_tests.CommonTestDriver()

    def test_is_subtype_invalid_json_input(self) -> None:
        self.test_driver.start_hh_server()

        input_error_exit_code = 10
        for invalid_json in ("", " \n\t", "{"):
            with self.subTest(invalid_json=invalid_json):
                stdout, stderr, retcode = self.test_driver.run_check(
                    stdin=invalid_json,
                    options=["--is-subtype"],
                )
                self.assertEqual("", stdout)
                self.assertTrue(stderr.startswith("Invalid JSON input:"), stderr)
                self.assertEqual(input_error_exit_code, retcode)

    def test_is_subtype_unknown_fields_free_generic(self) -> None:
        self.test_driver.start_hh_server()
        shape_type = {
            "kind": "shape",
            "is_array": False,
            "fields_known": False,
            "fields": [],
            "unknown_fields_type": {"kind": "generic", "name": "TRowValue"},
        }
        query = [
            [
                {"kind": "type", "type": shape_type},
                {"kind": "type", "type": {"kind": "mixed"}},
            ]
        ]
        stdout, stderr, retcode = self.test_driver.run_check(
            stdin=json.dumps(query),
            options=["--is-subtype"],
        )
        self.assertEqual(0, retcode, stderr)
        self.assertEqual(
            [{"errors": ["Unsupported free type TRowValue"]}],
            json.loads(stdout),
        )

    def test_isolation(self) -> None:
        files = {
            "isolation_seed.php": (
                "<?hh\nfunction isolation_seed(): void {\n  isolation_seed();\n}\n"
            ),
            "isolation_function_target.php": (
                "<?hh\nfunction isolation_function_target(): void {}\n"
            ),
            "isolation_function_caller.php": (
                "<?hh\n"
                "function isolation_function_caller(): void {\n"
                "  isolation_function_target();\n"
                "}\n"
            ),
            "isolation_static_target.php": (
                "<?hh\n"
                "class IsolationStaticTarget {\n"
                "  public static function target(): void {}\n"
                "}\n"
            ),
            "isolation_static_caller.php": (
                "<?hh\n"
                "function isolation_static_caller(): void {\n"
                "  IsolationStaticTarget::target();\n"
                "}\n"
            ),
            "isolation_base.php": (
                "<?hh\n"
                "class IsolationBase {\n"
                "  public function inherited(): void {}\n"
                "}\n"
            ),
            "isolation_child.php": (
                "<?hh\nclass IsolationChild extends IsolationBase {}\n"
            ),
            "isolation_inherited_caller.php": (
                "<?hh\n"
                "function isolation_inherited_caller(\n"
                "  IsolationChild $child,\n"
                "): void {\n"
                "  $child->inherited();\n"
                "}\n"
            ),
            # isolation_closure_shared is referenced from two seeds, so neither
            # can absorb it under the subset rule: when either is tested, the
            # other is still outside. Its transitive dependents are just those
            # two, well under the bound, so the closure rule takes all three
            # together into whichever seed is grown first.
            "isolation_closure_shared.php": (
                "<?hh\nfunction isolation_closure_shared(): void {}\n"
            ),
            "isolation_closure_a.php": (
                "<?hh\n"
                "function isolation_closure_a(): void {\n"
                "  isolation_closure_shared();\n"
                "}\n"
            ),
            "isolation_closure_b.php": (
                "<?hh\n"
                "function isolation_closure_b(): void {\n"
                "  isolation_closure_shared();\n"
                "}\n"
            ),
            # A file is absorbed whole, so a closure reaching it through one of
            # its symbols takes the rest of them too. The walk runs over
            # symbols, so the dependents of the symbol it did not arrive at are
            # left outside a cluster that holds what they reference, which is
            # the one thing a cluster may not have.
            #
            # isolation_multi_symbols.php is reachable from the shared function
            # through isolation_multi_fn, and only through it. Its constant is
            # read from nowhere else in the cluster, so a closure that stops at
            # the symbols it walked leaves that reader out.
            "isolation_multi_shared.php": (
                "<?hh\nfunction isolation_multi_shared(): void {}\n"
            ),
            "isolation_multi_other.php": (
                "<?hh\n"
                "function isolation_multi_other(): void {\n"
                "  isolation_multi_shared();\n"
                "}\n"
            ),
            "isolation_multi_symbols.php": (
                "<?hh\n"
                "const int ISOLATION_MULTI_CONST = 1;\n"
                "function isolation_multi_fn(): void {\n"
                "  isolation_multi_shared();\n"
                "}\n"
            ),
            # Reads the constant and nothing else, so no symbol the walk visits
            # leads here. On an excluded path so that it cannot be a seed:
            # growth reaches it through the file above or not at all.
            "__tests__/isolation_multi_const_test.php": (
                "<?hh\n"
                "function isolation_multi_const_test(): int {\n"
                "  return ISOLATION_MULTI_CONST;\n"
                "}\n"
            ),
            # Nothing references this and it references nothing, so it would
            # be a seed on every count except that it sits on an excluded path.
            "__tests__/isolation_excluded_test.php": (
                "<?hh\nfunction isolation_excluded_test(): void {}\n"
            ),
        }
        for filename, contents in files.items():
            path = os.path.join(self.test_driver.repo_dir, filename)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(contents)

        self.test_driver.start_hh_server(
            changed_files=list(files.keys()), args=["--no-load"]
        )
        output, _ = self.test_driver.check_cmd(
            expected_output=None,
            options=["--isolation", "--isolation-grow"],
        )
        lines = output.splitlines()

        self.assertEqual(2, len(lines))
        header_match = re.fullmatch(r"Started from (\d+) files\.", lines[0])
        if header_match is None:
            self.fail(f"Unexpected command header: {lines[0]}")
        if (
            re.fullmatch(
                r"Grew \d+ clusters covering \d+ files \(largest: \d+\)\.", lines[1]
            )
            is None
        ):
            self.fail(f"Unexpected cluster summary: {lines[1]}")

        seed_count = int(header_match.group(1))
        self.assertGreater(seed_count, 0)

        # Fifteen of the sixteen fixture files form six clusters; the sixteenth
        # is on an excluded path and is reached by nothing.
        # Most are grown from a
        # seed by absorbing files whose only dependents are already inside, so
        # this exercises the subset rule and, for the inheritance case, that
        # growth continues past the first round: the caller absorbs
        # IsolationChild, which then makes IsolationBase absorbable.
        #
        # The closure cluster exercises the other rule, and is the only case
        # that does. isolation_closure_shared is rejected by the subset rule
        # whichever seed tests it, so without the closure rule this fixture
        # reports two singleton clusters instead of one cluster of three.
        json_output, _ = self.test_driver.check_cmd(
            expected_output=None,
            options=[
                "--isolation",
                "--isolation-grow",
                "--json",
            ],
        )
        json_clusters, json_result = parse_isolation_json(json_output)
        summary = json_result["summary"]
        self.assertEqual(seed_count, summary["total_seeds"])

        fixture_names = {os.path.basename(name) for name in files}
        fixture_clusters = []
        for cluster in json_clusters:
            names = {os.path.basename(path) for path in cluster["files"]}
            overlap = names & fixture_names
            if not overlap:
                continue
            # A fixture cluster must not have absorbed anything outside the
            # fixture; nothing in it references the template repo.
            self.assertEqual(names, overlap)
            self.assertEqual(len(cluster["files"]), cluster["size"])
            fixture_clusters.append(frozenset(overlap))

        # A file on an excluded path is not a seed, so it heads no cluster. It
        # is still free to be absorbed *into* one: under strict isolation a test
        # left outside the package holding the code it exercises would stop
        # typechecking, which is why isolation_multi_const_test.php is expected
        # inside a cluster below. This one references nothing and so is reached
        # by nothing, leaving it absent rather than absorbed.
        for cluster in json_clusters:
            for path in cluster["files"]:
                self.assertNotIn("isolation_excluded_test.php", path)

        self.assertCountEqual(
            [
                frozenset(
                    {
                        "isolation_function_caller.php",
                        "isolation_function_target.php",
                    }
                ),
                frozenset(
                    {"isolation_static_caller.php", "isolation_static_target.php"}
                ),
                frozenset(
                    {
                        "isolation_inherited_caller.php",
                        "isolation_child.php",
                        "isolation_base.php",
                    }
                ),
                frozenset(
                    {
                        "isolation_closure_a.php",
                        "isolation_closure_shared.php",
                        "isolation_closure_b.php",
                    }
                ),
                # The constant's reader is in here only because the closure is
                # taken to a fixpoint over the files it resolves. Stop at the
                # symbols the walk visited and this cluster comes back as the
                # three files above it, with the reader outside referencing the
                # constant inside.
                frozenset(
                    {
                        "isolation_multi_other.php",
                        "isolation_multi_shared.php",
                        "isolation_multi_symbols.php",
                        "isolation_multi_const_test.php",
                    }
                ),
                frozenset({"isolation_seed.php"}),
            ],
            fixture_clusters,
        )

        # Measuring is the default, so the same command without
        # --isolation-grow reports each seed's own closure instead. Growth
        # merges seeds into shared clusters, so over the same repo it reports
        # fewer of them than measuring does — which is what tells the two modes
        # apart, rather than merely reading the flag back out.
        measured_output, _ = self.test_driver.check_cmd(
            expected_output=None,
            options=["--isolation", "--json"],
        )
        (_, measured_doc) = parse_isolation_json(measured_output)
        measured = measured_doc["summary"]
        self.assertFalse(measured["grown"])
        self.assertGreater(measured["total_clusters"], summary["total_clusters"])

        # isolation_seed.php calls only itself, so it is still a seed: a
        # self-reference is not an external dependent. It absorbs nothing and is
        # reported as a cluster of one, which is why it is expected above.

        # Check that the seed count *responds* to a dependency edge. Give
        # isolation_seed.php an external referrer by editing a file that already
        # exists: no file is added, so exactly one file changes classification
        # and the seed count must drop by exactly one. If reverse dependencies
        # were invisible, every file would be a seed both times and the count
        # would not move. The drop also pins the self-reference case above: the
        # file counted as a seed until a *different* file referenced it.
        with open(
            os.path.join(self.test_driver.repo_dir, "isolation_function_caller.php"),
            "w",
        ) as f:
            f.write(
                "<?hh\n"
                "function isolation_function_caller(): void {\n"
                "  isolation_function_target();\n"
                "  isolation_seed();\n"
                "}\n"
            )

        output, _ = self.test_driver.check_cmd(
            expected_output=None,
            options=["--isolation", "--isolation-grow"],
        )
        lines = output.splitlines()
        self.assertEqual(2, len(lines))
        header_match = re.fullmatch(r"Started from (\d+) files\.", lines[0])
        if header_match is None:
            self.fail(f"Unexpected command header: {lines[0]}")

        self.assertEqual(seed_count - 1, int(header_match.group(1)))

    def test_growth_seeds_from_the_closure(self) -> None:
        # A cluster must be closed under inbound references, and growth only
        # ever looks outbound. So a seed that something references can only be
        # made safe by seeding the cluster with the seed's closure rather than
        # the seed alone.
        #
        # The whole-repository scan never produces such a seed — it filters them
        # out — so this drives the seed-list path, which does not, and which is
        # the path every published measurement used.
        files = {
            "closure_seed_target.php": (
                "<?hh\nfunction closure_seed_target(): void {}\n"
            ),
            # Referencing the seed, and not referenced by it. Growth cannot
            # reach this file by absorbing outward; only the seed's closure
            # brings it in.
            "closure_seed_referrer.php": (
                "<?hh\n"
                "function closure_seed_referrer(): void {\n"
                "  closure_seed_target();\n"
                "}\n"
            ),
        }
        for filename, contents in files.items():
            path = os.path.join(self.test_driver.repo_dir, filename)
            with open(path, "w") as f:
                f.write(contents)

        seed_list = os.path.join(self.test_driver.repo_dir, "seeds.txt")
        with open(seed_list, "w") as f:
            f.write("closure_seed_target.php\n")

        self.test_driver.start_hh_server(
            changed_files=list(files.keys()), args=["--no-load"]
        )
        json_output, _ = self.test_driver.check_cmd(
            expected_output=None,
            options=[
                "--isolation",
                "--isolation-grow",
                "--isolation-seed-list",
                seed_list,
                "--json",
            ],
        )
        (payload_clusters, payload) = parse_isolation_json(json_output)

        clusters = [
            frozenset(cluster["files"])
            for cluster in payload_clusters
            if any(f.startswith("closure_seed_") for f in cluster["files"])
        ]
        # Seeded from the target alone, the cluster must still come back holding
        # the referrer. Before the closure seeding this returned the target on
        # its own — a cluster with a reference into it, which is the one thing a
        # cluster is defined not to have.
        self.assertEqual(
            [frozenset({"closure_seed_target.php", "closure_seed_referrer.php"})],
            clusters,
        )
        self.assertTrue(payload["summary"]["grown"])

    def test_growth_counts_only_new_files_against_the_cap(self) -> None:
        # A candidate's closure is everything that transitively depends on it,
        # and a candidate is only ever offered because a file already in the
        # cluster references it. So the closure always contains part of the
        # cluster, and only the files it would actually add can count against
        # the size cap.
        files = {
            "cap_shared.php": "<?hh\nfunction cap_shared(): void {}\n",
            "cap_a.php": "<?hh\nfunction cap_a(): void {\n  cap_shared();\n}\n",
            "cap_b.php": "<?hh\nfunction cap_b(): void {\n  cap_shared();\n}\n",
        }
        for filename, contents in files.items():
            path = os.path.join(self.test_driver.repo_dir, filename)
            with open(path, "w") as f:
                f.write(contents)

        self.test_driver.start_hh_server(
            changed_files=list(files.keys()), args=["--no-load"]
        )
        # Three is exactly the size of the closed cluster. Growing from cap_a,
        # the cluster holds one file and cap_shared's closure holds three — it,
        # cap_a and cap_b — of which two are new, so it fits the two remaining
        # places. Measured against the whole closure it does not, and the seed
        # is reported alone and truncated.
        json_output, _ = self.test_driver.check_cmd(
            expected_output=None,
            options=[
                "--isolation",
                "--isolation-grow",
                "--isolation-max-cluster-size",
                "3",
                "--json",
            ],
        )
        (payload_clusters, _) = parse_isolation_json(json_output)

        clusters = [
            (frozenset(cluster["files"]), cluster["truncated"])
            for cluster in payload_clusters
            if any(f.startswith("cap_") for f in cluster["files"])
        ]
        self.assertEqual(
            [(frozenset({"cap_a.php", "cap_b.php", "cap_shared.php"}), False)],
            clusters,
        )

    def test_isolation_move_to_refuses_a_missing_seed_dir(self) -> None:
        self.test_driver.start_hh_server(changed_files=[], args=["--no-load"])
        stdout, _, retcode = self.test_driver.run_check(
            options=[
                "--isolation",
                "--isolation-seed-dir",
                "no_such_dir",
                "--isolation-move-to",
                "move_dest",
                "--json",
            ],
        )
        input_error_exit_code = 10
        self.assertEqual(input_error_exit_code, retcode, stdout)
        self.assertIsNotNone(json.loads(stdout)["refused"])

    def test_remove_dead_fixmes(self) -> None:
        with open(os.path.join(self.test_driver.repo_dir, "foo_4.php"), "w") as f:
            f.write(
                """<?hh // strict
                function expect_int(int $_): void {}
                function foo(?string $s, ?int $i): void {
                  /* HH_FIXME[4089] We can delete this one */
                  /* HH_FIXME[4110] We need to keep this one */
                  /* HH_FIXME[4099] We can delete this one */
                  expect_int($s);
                  if (/* HH_FIXME[4011] We can delete this one */   $s) {
                    print "hello";
                  } else if ($s /* HH_FIXME[4011] We can delete this one */) {
                    print "world";
                  }
                  /* HH_FIXME[4099] We can delete this one */
                  /* HH_FIXME[4098] We can delete this one */
                  print "done\n";
                  /* HH_IGNORE[4110] We can delete this one */
                  /* HH_IGNORE[12001] We can delete this one */
                  print "sauerkraut";
                  if (/* HH_IGNORE[12004] We can delete this one */   $s) {
                    print "hello";
                  } else if ($s /* HH_FIXME[4011] We can delete this one */) {
                    print "world";
                  } else if ($s) {
                    print "hello";
                  }
                  /* HH_IGNORE[12001] We need to keep this one */
                  /* HH_FIXME[4099] We can delete this one */
                  /* HH_IGNORE[12004] We can delete this one */
                  $s === $i;
                }
            """
            )

        # Allow to print full diff in case of failure of the assert
        self.maxDiff = None

        self.test_driver.start_hh_server(
            changed_files=["foo_4.php"], args=["--no-load"]
        )
        self.test_driver.check_cmd(
            expected_output=None, options=["--remove-dead-fixmes"]
        )

        with open(os.path.join(self.test_driver.repo_dir, "foo_4.php")) as f:
            out = f.read()
            self.assertEqual(
                out,
                """<?hh // strict
                function expect_int(int $_): void {}
                function foo(?string $s, ?int $i): void {
                  /* HH_FIXME[4110] We need to keep this one */
                  expect_int($s);
                  if ($s) {
                    print "hello";
                  } else if ($s ) {
                    print "world";
                  }
                  print "done\n";
                  print "sauerkraut";
                  if ($s) {
                    print "hello";
                  } else if ($s ) {
                    print "world";
                  } else if ($s) {
                    print "hello";
                  }
                  /* HH_IGNORE[12001] We need to keep this one */
                  $s === $i;
                }
            """,
            )

    def test_remove_dead_fixmes_single_alive(self) -> None:
        with open(
            os.path.join(self.test_driver.repo_dir, "foo_fixme_single_alive.php"), "w"
        ) as f:
            f.write(
                """<?hh
                function takes_int(int $_): void {}

                function foo(): void {
                  /* HH_FIXME[4110] not dead. */
                  takes_int("not an int");
                }
            """
            )

        self.test_driver.start_hh_server(
            changed_files=["foo_fixme_single_alive.php"], args=["--no-load"]
        )
        self.test_driver.check_cmd(
            expected_output=None, options=["--remove-dead-fixmes"]
        )

        with open(
            os.path.join(self.test_driver.repo_dir, "foo_fixme_single_alive.php")
        ) as f:
            out = f.read()
            self.assertEqual(
                out,
                """<?hh
                function takes_int(int $_): void {}

                function foo(): void {
                  /* HH_FIXME[4110] not dead. */
                  takes_int("not an int");
                }
            """,
            )

    def test_remove_dead_unsafe_casts(self) -> None:
        with open(os.path.join(self.test_driver.repo_dir, "foo_5.php"), "w") as f:
            f.write(
                r"""<?hh
                function takes_string(string $i): void {}
                function id<T>(T $t): T { return $t; }

                function foo(?string $s): ?string {
                  takes_string(\HH\FIXME\UNSAFE_CAST<?string, string>($s)); // Not redundant
                  \HH\FIXME\UNSAFE_CAST<mixed, ?string>($s); // Redundant
                  if (\HH\FIXME\UNSAFE_CAST<mixed, ?string>(id($s)) === 'test') { // Redundant
                    print "hello";
                    return \HH\FIXME\UNSAFE_CAST<?string, string>($s); // Not redundant
                  } else {
                    return \HH\FIXME\UNSAFE_CAST<mixed, ?string>($s); // Redundant
                  }
                }
            """
            )

        self.test_driver.start_hh_server(
            changed_files=["foo_5.php"],
            args=["--no-load", "--config", "populate_dead_unsafe_cast_heap=true"],
        )
        self.test_driver.check_cmd(
            expected_output=None, options=["--remove-dead-unsafe-casts"]
        )

        with open(os.path.join(self.test_driver.repo_dir, "foo_5.php")) as f:
            out = f.read()
            self.assertEqual(
                out,
                r"""<?hh
                function takes_string(string $i): void {}
                function id<T>(T $t): T { return $t; }

                function foo(?string $s): ?string {
                  takes_string(\HH\FIXME\UNSAFE_CAST<?string, string>($s)); // Not redundant
                  $s; // Redundant
                  if (id($s) === 'test') { // Redundant
                    print "hello";
                    return \HH\FIXME\UNSAFE_CAST<?string, string>($s); // Not redundant
                  } else {
                    return $s; // Redundant
                  }
                }
            """,
            )
