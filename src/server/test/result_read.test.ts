/**
 * End-to-end test for the `result_read` TOOL WRAPPER (not just the store).
 *
 * Arrange: drive the real production compaction path — `maybeCompact` (the
 * exact function `server.ts` applies to every non-plumbing tool result) with a
 * low maxBytes so a `_handle` digest is produced against the REAL handle store
 * (no mocks). Act: invoke the `result_read` handler pulled from the canonical
 * registry (`buildRegistry()`), the same ToolDef an MCP client's call is
 * dispatched to. Observe: chunk slicing, next_offset continuation, full-walk
 * reassembly, and the documented error envelope for a bogus handle.
 */

import { expect, test, describe, afterEach } from "bun:test";
import { buildRegistry } from "../src/register.ts";
import { maybeCompact, _clearHandles } from "../src/compaction/handles.ts";
import type { ToolContext, ToolDef } from "../src/registry/types.ts";
import { covers } from "./harness/coverage.ts";

// result_read never touches the bridge; a connection-less context matches how
// disclosure.test.ts drives server-local handlers.
const ctx = { conn: undefined } as unknown as ToolContext;

function resultReadTool(): ToolDef {
  const def = buildRegistry().get("result_read");
  if (!def) throw new Error("result_read not registered in the canonical registry");
  return def;
}

/** Produce a compacted digest via the production path and return its handle
 *  plus the full JSON the store cached. */
function arrangeCompacted(): { handle: string; full: string } {
  const payload = {
    status: "success",
    result: { rows: Array.from({ length: 300 }, (_, i) => ({ i, tag: `row-${i}` })) },
  };
  const full = JSON.stringify(payload);
  const digest = maybeCompact(payload, 200) as {
    status: string;
    result: { _compacted: boolean; _handle: string; _bytes: number };
  };
  expect(digest.result._compacted).toBe(true);
  expect(digest.result._bytes).toBe(full.length);
  return { handle: digest.result._handle, full };
}

afterEach(() => _clearHandles());

describe("result_read tool wrapper", () => {
  covers("result_read");
  test("returns the exact slice of the compacted payload with continuation", async () => {
    const { handle, full } = arrangeCompacted();
    const tool = resultReadTool();

    const r: any = await tool.handler({ handle, offset: 0, length: 100 }, ctx);
    expect(r.status).toBe("success");
    expect(r.result.found).toBe(true);
    expect(r.result.chunk).toBe(full.slice(0, 100));
    expect(r.result.offset).toBe(0);
    expect(r.result.next_offset).toBe(100);
    expect(r.result.total_bytes).toBe(full.length);

    // A mid-payload slice equals the corresponding slice of the original.
    const mid: any = await tool.handler({ handle, offset: 100, length: 250 }, ctx);
    expect(mid.status).toBe("success");
    expect(mid.result.chunk).toBe(full.slice(100, 350));
    expect(mid.result.next_offset).toBe(350);
  });

  test("walking next_offset to null reassembles the full original payload", async () => {
    const { handle, full } = arrangeCompacted();
    const tool = resultReadTool();

    const pieces: string[] = [];
    let offset: number | null = 0;
    let guard = 0;
    while (offset !== null) {
      if (++guard > 1000) throw new Error("next_offset never reached null");
      const r: any = await tool.handler({ handle, offset, length: 137 }, ctx);
      expect(r.status).toBe("success");
      expect(r.result.offset).toBe(offset);
      pieces.push(r.result.chunk);
      offset = r.result.next_offset;
    }
    expect(pieces.join("")).toBe(full);
    // The reassembled JSON round-trips to the original structure.
    expect(JSON.parse(pieces.join("")).result.rows).toHaveLength(300);
  });

  test("bogus handle returns the documented error envelope", async () => {
    const tool = resultReadTool();
    const r: any = await tool.handler({ handle: "res_bogus_zz", offset: 0, length: 100 }, ctx);
    expect(r.status).toBe("error");
    expect(r.error).toContain("res_bogus_zz");
    expect(r.error_code).toBe("invalid_argument");
    expect(typeof r.error_hint).toBe("string");
    expect(r.result).toBeUndefined();
  });
});
