/**
 * Assemble the tool registry from every domain module. This is the single place
 * that imports all domains. The registry is built once and shared by the server,
 * the meta-tools, and code-execution mode.
 *
 * StateTree note: there is exactly one canonical StateTree family, `statetree_*`.
 * The abbreviated `st_*` family was retired (full-word domain, strict superset).
 */

import { ToolRegistry } from "./registry/index.ts";
import {
  withAssetAliases,
  withBpAliases,
  withMaterialAliases,
} from "./registry/aliases.ts";
import { metaTools } from "./disclosure/metatools.ts";
import { codeTools } from "./disclosure/codemode/tools.ts";
import { resultTools } from "./compaction/tool.ts";

import { actorTools } from "./domains/actor.ts";
import { aiTools } from "./domains/ai.ts";
import { animTools } from "./domains/anim.ts";
import { assetTools } from "./domains/asset.ts";
import { bpTools } from "./domains/bp.ts";
import { buildTools } from "./domains/build.ts";
import { classTools } from "./domains/class.ts";
import { coreTools } from "./domains/core.ts";
import { datatableTools } from "./domains/datatable.ts";
import { editorTools } from "./domains/editor.ts";
import { enumTools } from "./domains/enum.ts";
import { eqsTools } from "./domains/eqs.ts";
import { foliageTools } from "./domains/foliage.ts";
import { gasTools } from "./domains/gas.ts";
import { ik_retargetTools } from "./domains/ik_retarget.ts";
import { ik_rigTools } from "./domains/ik_rig.ts";
import { inputTools } from "./domains/input.ts";
import { kinematicsTools } from "./domains/kinematics.ts";
import { landscapeTools } from "./domains/landscape.ts";
import { levelTools } from "./domains/level.ts";
import { materialTools } from "./domains/material.ts";
import { meshTools } from "./domains/mesh.ts";
import { mpcTools } from "./domains/mpc.ts";
import { niagaraTools } from "./domains/niagara.ts";
import { pcgTools } from "./domains/pcg.ts";
import { physicsTools } from "./domains/physics.ts";
import { pieTools } from "./domains/pie.ts";
import { projectTools } from "./domains/project.ts";
import { reflectionTools } from "./domains/reflection.ts";
import { sceneTools } from "./domains/scene.ts";
import { statetreeTools } from "./domains/statetree.ts";
import { structTools } from "./domains/struct.ts";
import { tagTools } from "./domains/tag.ts";
import { videoTools } from "./domains/video.ts";
import { widgetTools } from "./domains/widget.ts";

const ALL_DOMAINS = [
  actorTools,
  aiTools,
  animTools,
  withAssetAliases(assetTools),
  withBpAliases(bpTools),
  buildTools,
  classTools,
  coreTools,
  datatableTools,
  editorTools,
  enumTools,
  eqsTools,
  foliageTools,
  gasTools,
  ik_retargetTools,
  ik_rigTools,
  inputTools,
  kinematicsTools,
  landscapeTools,
  levelTools,
  withMaterialAliases(materialTools),
  meshTools,
  mpcTools,
  niagaraTools,
  pcgTools,
  physicsTools,
  pieTools,
  projectTools,
  reflectionTools,
  sceneTools,
  statetreeTools,
  structTools,
  tagTools,
  videoTools,
  widgetTools,
];

export function buildRegistry(): ToolRegistry {
  const registry = new ToolRegistry();
  for (const domain of ALL_DOMAINS) registry.registerAll(domain);
  // Disclosure layers are registry-backed too. They reference the registry, so
  // they register last. Exposure is filtered per surface mode in server.ts;
  // catalog_search/call deliberately skip the catalog+code domains.
  registry.registerAll(metaTools(registry));
  registry.registerAll(codeTools(registry));
  registry.registerAll(resultTools);
  return registry;
}
