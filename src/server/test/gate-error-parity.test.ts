/**
 * C++ ↔ TypeScript parity for the two hand-duplicated cross-language contracts:
 *   1. the PIE / dry-run gate blocklists (`gates.ts` ↔ `MCPCommonUtils.cpp`), and
 *   2. the error-code taxonomy (`errors.ts` ↔ `EMCPErrorCode` + its string map).
 *
 * The C++ side is authoritative (it enforces). This test parses the C++ source
 * and fails loudly if the TS mirrors drift — so a blocklist/error-code edit on
 * one side without the other is caught by `bun test`, not in production.
 *
 * Gate namespacing: the C++ blocklists key on WIRE command names; the TS sets key
 * on canonical TOOL names (they annotate tools by `def.name`). We reconcile by
 * mapping each registered tool to its wire command (identity, unless the domain
 * declares a `command:` override) and comparing in the tool-name namespace.
 */
import { describe, expect, test } from "bun:test";
import { readFileSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { buildRegistry } from "../src/register.ts";
import { PIE_BLOCKED, DRY_RUN_UNSUPPORTED } from "../src/bridge/gates.ts";
import { ErrorCode } from "../src/bridge/errors.ts";

const CPP_DIR = "../Plugin/UnrealMCP/Source/UnrealMCP/Private/Commands";
const CPP = readFileSync(join(import.meta.dir, `../${CPP_DIR}/MCPCommonUtils.cpp`), "utf8");
const DOMAINS_DIR = join(import.meta.dir, "../src/domains");

/** Extract the TEXT("...") literals from a `static const TSet<FString>` block
 *  inside the named C++ function. */
function cppBlocklist(fnName: string): Set<string> {
  const fnAt = CPP.indexOf(`::${fnName}(`);
  if (fnAt < 0) throw new Error(`C++ function ${fnName} not found in MCPCommonUtils.cpp`);
  const setAt = CPP.indexOf("BlockedCommands = {", fnAt);
  const end = CPP.indexOf("};", setAt);
  const body = CPP.slice(setAt, end);
  return new Set([...body.matchAll(/TEXT\("([^"]+)"\)/g)].map((m) => m[1]!));
}

/** The set of error-code strings the C++ `MCPErrorCodeToString` switch returns. */
function cppErrorCodes(): Set<string> {
  return new Set(
    [...CPP.matchAll(/case\s+EMCPErrorCode::\w+:\s*return\s+TEXT\("([a-z_]+)"\)/g)].map((m) => m[1]!),
  );
}

/** tool name → wire command (identity unless a domain declares `command:`). */
function toolToWire(): Map<string, string> {
  const map = new Map<string, string>();
  const overrides = new Map<string, string>();
  for (const f of readdirSync(DOMAINS_DIR).filter((f) => f.endsWith(".ts"))) {
    const src = readFileSync(join(DOMAINS_DIR, f), "utf8");
    let last = "";
    for (const line of src.split("\n")) {
      const n = line.match(/^\s*name:\s*"([^"]+)"/);
      if (n) last = n[1]!;
      const c = line.match(/^\s*command:\s*"([^"]+)"/);
      if (c && last) overrides.set(last, c[1]!);
    }
  }
  for (const def of buildRegistry().all()) {
    map.set(def.name, overrides.get(def.name) ?? def.name);
  }
  return map;
}

const wireByTool = toolToWire();
const toolNames = [...wireByTool.keys()];

/** Tools whose wire command is in a given C++ wire-name blocklist. */
function expectedTools(cppWire: Set<string>): Set<string> {
  return new Set(toolNames.filter((t) => cppWire.has(wireByTool.get(t)!)));
}

const symdiff = (a: ReadonlySet<string>, b: ReadonlySet<string>) => ({
  onlyInTs: [...a].filter((x) => !b.has(x)).sort(),
  missingFromTs: [...b].filter((x) => !a.has(x)).sort(),
});

describe("C++/TS gate + error-code parity", () => {
  test("PIE_BLOCKED mirrors IsBlockedDuringPie (tool↔wire mapped)", () => {
    const expected = expectedTools(cppBlocklist("IsBlockedDuringPie"));
    expect(symdiff(PIE_BLOCKED, expected)).toEqual({ onlyInTs: [], missingFromTs: [] });
  });

  test("DRY_RUN_UNSUPPORTED mirrors IsBlockedFromDryRun (tool↔wire mapped)", () => {
    const expected = expectedTools(cppBlocklist("IsBlockedFromDryRun"));
    expect(symdiff(DRY_RUN_UNSUPPORTED, expected)).toEqual({ onlyInTs: [], missingFromTs: [] });
  });

  test("every TS gate entry is a real registered tool (no dead entries)", () => {
    const names = new Set(toolNames);
    expect([...PIE_BLOCKED].filter((t) => !names.has(t))).toEqual([]);
    expect([...DRY_RUN_UNSUPPORTED].filter((t) => !names.has(t))).toEqual([]);
  });

  test("ErrorCode mirrors the C++ EMCPErrorCode string map", () => {
    const ts = new Set<string>(Object.values(ErrorCode));
    const cpp = cppErrorCodes();
    expect(symdiff(ts, cpp)).toEqual({ onlyInTs: [], missingFromTs: [] });
  });
});
