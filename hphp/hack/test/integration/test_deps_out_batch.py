# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

from __future__ import annotations

from pathlib import Path

from hphp.hack.test.integration.common_tests import CommonTestDriver
from hphp.hack.test.integration.test_case import TestCase


class TestDepsOutBatch(TestCase[CommonTestDriver]):
    @classmethod
    def get_template_repo(cls) -> str:
        return "hphp/hack/test/integration/data/empty_repo"

    @classmethod
    def get_test_driver(cls) -> CommonTestDriver:
        return CommonTestDriver()

    def query(self, positions: list[str]) -> str:
        output, error, status = self.test_driver.run_check(
            options=["--deps-out-at-pos-batch", *positions]
        )
        self.assertEqual(0, status, error)
        return output

    def test_batch_matches_individual_queries(self) -> None:
        path = Path(self.test_driver.repo_dir) / "dependencies.php"
        path.write_text(
            "<?hh\n"
            "class First {}\n"
            "class Second {}\n"
            "class Third {}\n"
            "function first(First $x): void {}\n"
            "function second(Second $x): void {}\n"
            "function third(Third $x): void {}\n"
        )
        self.test_driver.start_hh_server(args=["--no-load"])
        positions = [f"{path}:{line}:17" for line in (5, 6, 7)]
        individual = [self.query([position]) for position in positions]
        for name, output in zip(("First", "Second", "Third"), individual):
            self.assertIn(f'"name": "\\\\{name}"', output)
            self.assertIn('"depends_on"', output)
        # hh_client accumulates batch positions in reverse order.
        for count in (2, 3):
            with self.subTest(count=count):
                self.assertEqual(
                    "".join(reversed(individual[:count])),
                    self.query(positions[:count]),
                )
