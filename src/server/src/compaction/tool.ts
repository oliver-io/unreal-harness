/**
 * `result_read` — pull a slice of a compacted result by handle. Registered in
 * all surface modes (cheap no-op when compaction is disabled and no handles
 * exist).
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { defineTool } from "../domains/_shared.ts";
import { envelopeError } from "../bridge/envelope.ts";
import { readHandle } from "./handles.ts";

export const resultTools: ToolDef[] = [
  defineTool({
    name: "result_read",
    domain: "result",
    description:
      "Read a slice of a compacted tool result by its _handle. Returns a chunk of " +
      "the raw JSON plus next_offset (null when done). For large data, prefer " +
      "code_run to filter in-sandbox instead of paging it into context.",
    input: z.object({
      handle: z.string().describe("The _handle from a compacted result."),
      offset: z.number().int().min(0).default(0).describe("Start byte offset."),
      length: z.number().int().min(1).max(50_000).default(8_000).describe("Chunk size in bytes."),
    }),
    annotations: { readOnlyHint: true, idempotentHint: true },
    handler: (a) => {
      const r = readHandle(a.handle, a.offset, a.length);
      if (!r.found) {
        return envelopeError(`Unknown or expired handle: ${a.handle}`, {
          code: "invalid_argument",
          hint: "Handles are bounded/LRU — re-run the original tool to regenerate.",
        });
      }
      return { status: "success", result: r };
    },
  }),
];
