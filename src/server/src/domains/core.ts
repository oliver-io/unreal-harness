/**
 * Domain: core — protocol primitives that aren't domain operations.
 *
 * `mcp_status` is the boot-gate readiness probe; the bridge answers it on its
 * network thread without touching the game thread. Exempt from the canonical
 * `<domain>_<verb>` rule (see scripts/lint-canonical-names.ts EXEMPT set).
 */

import { z } from "zod";
import type { ToolDef } from "../registry/types.ts";
import { bridgeTool } from "./_shared.ts";

const mcpStatus = bridgeTool({
  name: "mcp_status",
  domain: "core",
  description:
    "Editor readiness/liveness probe. result.ready === true once the editor is " +
    "interactive and safe to drive. Answered even mid-boot.",
  input: z.object({}),
  annotations: { readOnlyHint: true, idempotentHint: true },
});

export const coreTools: ToolDef[] = [mcpStatus];
