import { expect, test, describe, afterEach } from "bun:test";
import { maybeCompact, readHandle, _clearHandles } from "../src/compaction/handles.ts";

afterEach(() => _clearHandles());

describe("result compaction", () => {
  test("disabled (maxBytes<=0) passes payload through untouched", () => {
    const payload = { status: "success", result: { big: "x".repeat(10_000) } };
    expect(maybeCompact(payload, 0)).toBe(payload);
  });

  test("small payloads under threshold pass through", () => {
    const payload = { status: "success", result: { a: 1 } };
    expect(maybeCompact(payload, 1000)).toBe(payload);
  });

  test("oversized payloads become a digest + handle, readable in slices", () => {
    const payload = { status: "success", result: { rows: Array.from({ length: 500 }, (_, i) => ({ i })) } };
    const digest: any = maybeCompact(payload, 200);
    expect(digest.result._compacted).toBe(true);
    expect(typeof digest.result._handle).toBe("string");
    expect(digest.result._bytes).toBeGreaterThan(200);

    const full = JSON.stringify(payload);
    const first = readHandle(digest.result._handle, 0, 100);
    expect(first.found).toBe(true);
    expect(first.chunk).toBe(full.slice(0, 100));
    expect(first.next_offset).toBe(100);
    expect(first.total_bytes).toBe(full.length);

    // Walk to the end → next_offset null.
    const tail = readHandle(digest.result._handle, full.length - 10, 1000);
    expect(tail.next_offset).toBeNull();
  });

  test("unknown handle reports not found", () => {
    expect(readHandle("res_nope", 0, 10).found).toBe(false);
  });
});
