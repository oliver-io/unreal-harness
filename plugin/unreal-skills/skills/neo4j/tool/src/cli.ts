#!/usr/bin/env bun
/**
 * ue-neo4j — project a live Unreal project's assets into Neo4j via the harness MCP.
 *
 * Commands:
 *   discover   list assets that match the selector (no reads, no graph writes)
 *   pull       read selected assets via MCP and write their JSON to the data dir
 *   ingest     pull (unless --cached) → shape → MERGE into Neo4j + index + verify
 *   query      run a Cypher statement or --file against Neo4j
 *   verify     print node-label counts and index states
 *   clear      DETACH DELETE the whole graph (guarded by --yes)
 *
 * Selector flags (discover/pull/ingest):
 *   --dir <path>        asset_list root (default /Game/)
 *   --class <Class>     server-side class filter (Blueprint, StateTree, Material, …)
 *   --filter <substr>   client-side name/path substring (case-insensitive)
 *   --paths a,b,c       client-side whitelist of exact asset paths
 *   --no-recursive      do not recurse under --dir
 *   --data-assets       also read *DataAsset classes via reflection
 *
 * ingest extras:
 *   --cached            skip MCP reads; shape JSON already in the data dir
 *   --with-refs         add REFERENCES edges between selected assets (asset_references)
 *   --clear             wipe the graph first (full rebuild; otherwise MERGE is additive)
 *
 * Connection (env, overridable):
 *   MCP_URL (http://127.0.0.1:8765)  NEO4J_URI  NEO4J_USER  NEO4J_PASS(required for graph)
 */

import { mkdir, writeFile, readdir, readFile } from "fs/promises";
import { join } from "path";
import { loadConfig, requirePass, type Config } from "./config.ts";
import { Mcp } from "./mcp.ts";
import { Graph } from "./graph.ts";
import { selectAssets, type Asset, type Selector } from "./select.ts";
import { handlerFor } from "./ontology.ts";
import { type GNode, type GRel } from "./shape.ts";

// ── arg parsing ────────────────────────────────────────────────────────────
const argv = process.argv.slice(2);
const cmd = argv[0];
const flags = new Map<string, string>();
const bools = new Set<string>();
const positionals: string[] = [];
for (let i = 1; i < argv.length; i++) {
  const a = argv[i];
  if (a.startsWith("--")) {
    const key = a.slice(2);
    const next = argv[i + 1];
    if (next !== undefined && !next.startsWith("--")) {
      flags.set(key, next);
      i++;
    } else {
      bools.add(key);
    }
  } else {
    positionals.push(a);
  }
}
const has = (k: string) => bools.has(k) || flags.has(k);

function selector(): Selector {
  return {
    dir: flags.get("dir") ?? "/Game/",
    cls: flags.get("class") ?? "",
    recursive: !bools.has("no-recursive"),
    filter: flags.get("filter"),
    paths: flags.get("paths")?.split(",").map((s) => s.trim()).filter(Boolean),
  };
}

const log = (...a: unknown[]) => console.log(...a);
const sanitizeName = (p: string) => p.split("/").pop()!.split(".")[0]!.replace(/[^A-Za-z0-9_.-]/g, "_");

// ── shared helpers ───────────────────────────────────────────────────────────
async function withMcp<T>(cfg: Config, fn: (mcp: Mcp) => Promise<T>): Promise<T> {
  const mcp = new Mcp(cfg.mcpUrl);
  await mcp.connect();
  try {
    return await fn(mcp);
  } finally {
    await mcp.close();
  }
}

/** Read every selected asset that has a handler; return {asset, doc}. */
async function readAssets(mcp: Mcp, assets: Asset[], dataAssets: boolean): Promise<{ asset: Asset; doc: any }[]> {
  const out: { asset: Asset; doc: any }[] = [];
  let skipped = 0;
  for (const a of assets) {
    const h = handlerFor(a.class, { dataAssets });
    if (!h) {
      skipped++;
      continue;
    }
    try {
      const doc = await h.read(mcp, a);
      out.push({ asset: a, doc });
      log(`  read ${a.class}: ${a.path}`);
    } catch (e) {
      log(`  ! skip ${a.path}: ${(e as Error).message}`);
    }
  }
  if (skipped) log(`  (${skipped} asset(s) had no handler — add the class to src/readers.ts / src/ontology.ts)`);
  return out;
}

// ── commands ──────────────────────────────────────────────────────────────────
async function cmdDiscover(cfg: Config) {
  const assets = await withMcp(cfg, (mcp) => selectAssets(mcp, selector()));
  const byClass = new Map<string, number>();
  for (const a of assets) byClass.set(a.class, (byClass.get(a.class) ?? 0) + 1);
  log(`\n${assets.length} asset(s):`);
  for (const [c, n] of [...byClass].sort()) log(`  ${c.padEnd(28)} ${n}${handlerFor(c, { dataAssets: true }) ? "" : "   (no reader)"}`);
  log("");
  for (const a of assets) log(`  ${a.class.padEnd(24)} ${a.path}`);
}

async function cmdPull(cfg: Config) {
  await mkdir(cfg.dataDir, { recursive: true });
  const read = await withMcp(cfg, async (mcp) => {
    const assets = await selectAssets(mcp, selector());
    log(`Selected ${assets.length} asset(s).`);
    return readAssets(mcp, assets, has("data-assets"));
  });
  for (const { asset, doc } of read) {
    const file = join(cfg.dataDir, `${sanitizeName(asset.path)}.json`);
    await writeFile(file, JSON.stringify({ asset, doc }, null, 2));
  }
  log(`\nWrote ${read.length} file(s) to ${cfg.dataDir}`);
}

async function loadCached(cfg: Config): Promise<{ asset: Asset; doc: any }[]> {
  const files = (await readdir(cfg.dataDir)).filter((f) => f.endsWith(".json"));
  const out: { asset: Asset; doc: any }[] = [];
  for (const f of files) {
    const parsed = JSON.parse(await readFile(join(cfg.dataDir, f), "utf8"));
    if (parsed?.asset && "doc" in parsed) out.push(parsed);
  }
  return out;
}

async function cmdIngest(cfg: Config) {
  requirePass(cfg);
  const graph = new Graph(cfg.neo4jUri, cfg.neo4jUser, cfg.neo4jPass);
  await graph.verifyConn();

  let read: { asset: Asset; doc: any }[];
  let refEdges: GRel[] = [];

  if (has("cached")) {
    read = await loadCached(cfg);
    log(`Loaded ${read.length} cached asset(s) from ${cfg.dataDir}`);
  } else {
    await mkdir(cfg.dataDir, { recursive: true });
    read = await withMcp(cfg, async (mcp) => {
      const assets = await selectAssets(mcp, selector());
      log(`Selected ${assets.length} asset(s).`);
      const r = await readAssets(mcp, assets, has("data-assets"));
      for (const { asset, doc } of r) {
        await writeFile(join(cfg.dataDir, `${sanitizeName(asset.path)}.json`), JSON.stringify({ asset, doc }, null, 2));
      }
      if (has("with-refs")) refEdges = await buildRefEdges(mcp, r.map((x) => x.asset));
      return r;
    });
  }

  if (read.length === 0) {
    log("Nothing to ingest. Exiting.");
    await graph.close();
    process.exit(1);
  }

  // Shape (typed ontology where available, generic walk otherwise)
  const nodes: GNode[] = [];
  const rels: GRel[] = [];
  for (const { asset, doc } of read) {
    const h = handlerFor(asset.class, { dataAssets: has("data-assets") });
    if (!h) continue;
    const g = h.shape(asset, doc);
    nodes.push(...g.nodes);
    rels.push(...g.rels);
  }
  rels.push(...refEdges);
  const labels = [...new Set(nodes.map((n) => n.label))];
  log(`\nShaped: ${nodes.length} node(s), ${rels.length} relationship(s) across ${labels.length} label(s).`);

  if (has("clear")) {
    log("--clear: wiping graph first…");
    await graph.clear();
  }

  await graph.ensureIndexes(labels);
  await graph.mergeNodes(nodes);
  await graph.mergeRels(rels);
  log("Uploaded to Neo4j.");

  await printVerify(graph);
  await graph.close();
}

/** REFERENCES edges between selected asset roots (outbound, one hop). */
async function buildRefEdges(mcp: Mcp, assets: Asset[]): Promise<GRel[]> {
  const stem = (p: string) => p.split(".")[0]!;
  const present = new Map(assets.map((a) => [stem(a.path), a.path]));
  const edges: GRel[] = [];
  for (const a of assets) {
    try {
      const r = await mcp.call("asset_references", { asset_path: a.path, direction: "outbound", depth: 1 });
      const refs: any[] = r?.references ?? r?.outbound ?? r?.assets ?? [];
      for (const ref of refs) {
        const target = typeof ref === "string" ? ref : ref?.path ?? ref?.asset_path;
        if (!target) continue;
        const to = present.get(stem(target));
        if (to && to !== a.path) edges.push({ from: a.path, to, type: "REFERENCES", props: {} });
      }
    } catch {
      /* references are best-effort */
    }
  }
  log(`  ${edges.length} cross-asset REFERENCES edge(s).`);
  return edges;
}

async function cmdQuery(cfg: Config) {
  requirePass(cfg);
  const cypher = flags.get("file")
    ? await readFile(flags.get("file")!, "utf8")
    : positionals.join(" ");
  if (!cypher.trim()) {
    log('Usage: ue-neo4j query "MATCH (n) RETURN n LIMIT 5"   |   query --file q.cypher');
    process.exit(1);
  }
  const graph = new Graph(cfg.neo4jUri, cfg.neo4jUser, cfg.neo4jPass);
  await graph.verifyConn();
  const rows = await graph.query(cypher);
  log(JSON.stringify(rows, null, 2));
  log(`\n${rows.length} row(s).`);
  await graph.close();
}

async function printVerify(graph: Graph) {
  const counts = await graph.counts();
  log("\n=== Node counts ===");
  for (const c of counts) log(`  ${c.label?.padEnd(28) ?? "(none)"} ${c.count}`);
  const idx = await graph.indexes();
  log("=== Indexes ===");
  for (const i of idx) log(`  ${JSON.stringify(i.labels).padEnd(28)} ${JSON.stringify(i.props).padEnd(10)} ${i.state}`);
}

async function cmdVerify(cfg: Config) {
  requirePass(cfg);
  const graph = new Graph(cfg.neo4jUri, cfg.neo4jUser, cfg.neo4jPass);
  await graph.verifyConn();
  await printVerify(graph);
  await graph.close();
}

async function cmdClear(cfg: Config) {
  requirePass(cfg);
  if (!has("yes")) {
    log("Refusing to clear without --yes. This DETACH DELETEs the entire graph.");
    process.exit(1);
  }
  const graph = new Graph(cfg.neo4jUri, cfg.neo4jUser, cfg.neo4jPass);
  await graph.verifyConn();
  await graph.clear();
  log("Graph cleared.");
  await graph.close();
}

// ── dispatch ────────────────────────────────────────────────────────────────
async function main() {
  const cfg = loadConfig({
    mcpUrl: flags.get("mcp-url"),
    neo4jUri: flags.get("neo4j-uri"),
  });
  switch (cmd) {
    case "discover": return cmdDiscover(cfg);
    case "pull": return cmdPull(cfg);
    case "ingest": return cmdIngest(cfg);
    case "query": return cmdQuery(cfg);
    case "verify": return cmdVerify(cfg);
    case "clear": return cmdClear(cfg);
    default:
      log("Usage: ue-neo4j <discover|pull|ingest|query|verify|clear> [flags]");
      log("See the header of src/cli.ts or the neo4j skill's tool/README.md for flags.");
      process.exit(cmd ? 1 : 0);
  }
}

main().catch((e) => {
  console.error(`\nerror: ${(e as Error).message}`);
  process.exit(1);
});
