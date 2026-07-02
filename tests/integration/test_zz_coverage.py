"""Coverage guard — the scoreboard for "a test for every operation".

Statically scans every integration/test_*.py for @covers("op") declarations and
asserts the union equals the full operation manifest. Named test_zz_* so it
sorts last. This test is EXPECTED to be red until coverage is complete; the
failure message lists exactly which operations still need a test.
"""

from pathlib import Path

from harness.operations import ALL_OPERATIONS
from harness.coverage import scan_covered_ops

INTEGRATION_DIR = Path(__file__).parent


def test_every_operation_has_a_test():
    covered = scan_covered_ops(INTEGRATION_DIR)

    unknown = covered - ALL_OPERATIONS
    assert not unknown, (
        "These @covers(...) names are not real operations (typo, or manifest is "
        f"stale — rerun tests/tools/regen_operations.py): {sorted(unknown)}"
    )

    missing = ALL_OPERATIONS - covered
    pct = 100.0 * (len(ALL_OPERATIONS) - len(missing)) / len(ALL_OPERATIONS)
    assert not missing, (
        f"\nOperation coverage: {len(ALL_OPERATIONS) - len(missing)}/{len(ALL_OPERATIONS)} "
        f"({pct:.0f}%). {len(missing)} operation(s) still need an integration test:\n  "
        + "\n  ".join(sorted(missing))
    )
