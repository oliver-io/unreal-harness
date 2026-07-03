/**
 * Coverage gate for SERVER-LOCAL tools — the bun analog of the pytest
 * `integration/test_zz_coverage.py` scoreboard.
 *
 * The pytest oracle only sees bridge operations (wire names with a C++
 * dispatch key); registry tools that never hit the bridge are invisible to it
 * by construction. This gate computes that server-local set from source
 * (registry tools whose `command ?? name` is not a C++ dispatch key), scans
 * `test/**` for covers("tool") declarations, and asserts the two sets match.
 *
 * EXPECTED to be red until every server-local tool has a bun test; the
 * failure message lists exactly which tools still need one.
 */

import { expect, test, describe } from "bun:test";
import {
  cppDispatchKeys,
  scanCoveredTools,
  serverLocalTools,
} from "./harness/coverage.ts";

/** Tools exempt from the gate, each with a documented reason. Empty today —
 *  add `["tool_name", "reason"]` entries only when a tool genuinely cannot be
 *  tested without violating docs/TESTING.md (prefer #DEFERRED in
 *  docs/loops/tests/TASKS.md first). */
const EXEMPT = new Map<string, string>([]);

const serverLocal = serverLocalTools();
const covered = scanCoveredTools(import.meta.dir);

describe("server-local coverage gate", () => {
  test("classification sanity: bridge tools stay pytest's job", () => {
    // The C++ scan found a real dispatch surface, not an empty/missing tree.
    expect(cppDispatchKeys().size).toBeGreaterThan(250);
    // Known server-local tools are in-scope…
    expect(serverLocal).toContain("catalog_call");
    expect(serverLocal).toContain("actor_spawn_physics");
    // …while plain bridge tools and legacy `command:`-override bridge tools
    // (wire ≠ name, e.g. statetree_node_add → st_add_node) are excluded:
    // pytest covers those under their wire names.
    expect(serverLocal).not.toContain("actor_spawn");
    expect(serverLocal).not.toContain("statetree_node_add");
    expect(serverLocal).not.toContain("bp_add_node");
    // Exemptions must name real server-local tools.
    for (const name of EXEMPT.keys()) expect(serverLocal).toContain(name);
  });

  test("no unknown covers() names", () => {
    const unknown = [...covered].filter((t) => !serverLocal.includes(t)).sort();
    if (unknown.length) {
      throw new Error(
        "These covers(...) names are not server-local tools (typo, or a " +
          "bridge tool — bridge coverage belongs to the pytest @covers " +
          `ledger, not here): ${unknown.join(", ")}`,
      );
    }
  });

  test("every server-local tool has a bun test", () => {
    const missing = serverLocal.filter(
      (t) => !covered.has(t) && !EXEMPT.has(t),
    );
    const done = serverLocal.length - missing.length;
    if (missing.length) {
      throw new Error(
        `\nServer-local coverage: ${done}/${serverLocal.length} ` +
          `(${Math.round((100 * done) / serverLocal.length)}%). ` +
          `${missing.length} tool(s) still need a bun test ` +
          "(see docs/loops/tests/TASKS.md §C):\n  " +
          missing.join("\n  "),
      );
    }
  });
});
