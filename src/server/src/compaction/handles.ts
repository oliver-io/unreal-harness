/**
 * Result compaction — keep oversized tool payloads out of context until asked.
 *
 * When enabled (config.maxResultBytes > 0), a tool result whose JSON exceeds the
 * threshold is stashed in a bounded LRU and replaced on the wire with a small
 * digest: a preview + a `_handle`. The agent pulls the rest in slices via the
 * `result_read` tool. Default OFF, so direct full-mode calls stay byte-identical
 * to the Python server (parity), and code-mode (filter-in-sandbox) remains the
 * primary path for big data.
 */

const MAX_ENTRIES = 64;

interface Entry {
  json: string;
  createdSeq: number;
}

let seq = 0;
const store = new Map<string, Entry>(); // insertion-ordered → LRU by re-insert

function put(json: string): string {
  const handle = `res_${(++seq).toString(36)}_${json.length.toString(36)}`;
  store.set(handle, { json, createdSeq: seq });
  while (store.size > MAX_ENTRIES) {
    const oldest = store.keys().next().value as string | undefined;
    if (oldest === undefined) break;
    store.delete(oldest);
  }
  return handle;
}

/**
 * If `payload` serializes larger than `maxBytes`, cache it and return a digest;
 * otherwise return the payload unchanged. `maxBytes <= 0` disables compaction.
 */
export function maybeCompact(payload: unknown, maxBytes: number): unknown {
  if (maxBytes <= 0) return payload;
  let json: string;
  try {
    json = JSON.stringify(payload);
  } catch {
    return payload;
  }
  if (json.length <= maxBytes) return payload;

  const handle = put(json);
  return {
    status: isErrorEnvelope(payload) ? "error" : "success",
    result: {
      _compacted: true,
      _handle: handle,
      _bytes: json.length,
      _preview: json.slice(0, Math.min(1500, maxBytes)),
      _hint:
        `Result was ${json.length} bytes — truncated. Read more with ` +
        `result_read({ handle: "${handle}", offset, length }), or re-run via ` +
        `code_run to filter it down in-sandbox before it reaches context.`,
    },
  };
}

/** A char-range slice of a cached payload's JSON, with continuation metadata. */
export function readHandle(
  handle: string,
  offset: number,
  length: number,
): { found: boolean; chunk?: string; offset?: number; next_offset?: number | null; total_bytes?: number } {
  const entry = store.get(handle);
  if (!entry) return { found: false };
  // Refresh LRU position.
  store.delete(handle);
  store.set(handle, entry);

  const start = Math.max(0, offset);
  const end = Math.min(entry.json.length, start + Math.max(1, length));
  return {
    found: true,
    chunk: entry.json.slice(start, end),
    offset: start,
    next_offset: end < entry.json.length ? end : null,
    total_bytes: entry.json.length,
  };
}

/** Test/maintenance hook. */
export function _clearHandles(): void {
  store.clear();
  seq = 0;
}

function isErrorEnvelope(p: unknown): boolean {
  return typeof p === "object" && p !== null && (p as { status?: unknown }).status === "error";
}
