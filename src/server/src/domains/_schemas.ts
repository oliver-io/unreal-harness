/**
 * Reusable Zod fragments shared across domains. Reuse these instead of
 * redefining vectors/rotators inline so the surface stays consistent.
 */

import { z } from "zod";

/** {x, y, z} — location/scale. Matches the C++ FVector wire shape. */
export const Vec3 = z.object({ x: z.number(), y: z.number(), z: z.number() });

/** {pitch, yaw, roll} — rotation. Matches the C++ FRotator wire shape. */
export const Rotator = z.object({
  pitch: z.number(),
  yaw: z.number(),
  roll: z.number(),
});

/** {r, g, b, a} in 0..1 — linear color. */
export const LinearColor = z.object({
  r: z.number(),
  g: z.number(),
  b: z.number(),
  a: z.number().default(1),
});

/** Standard dry-run flag (most mutators accept it; returns result.diff). */
export const dryRun = z.boolean().default(false);
