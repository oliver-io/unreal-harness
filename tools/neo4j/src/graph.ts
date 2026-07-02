/**
 * Neo4j loader. Idempotent by design: nodes MERGE on `id`, relationships MERGE on
 * (from,to,type). Per-label `id` range indexes are created before bulk MERGE so
 * loads stay fast. Nothing here is project-specific.
 */

import neo4j, { type Driver } from "neo4j-driver";
import type { GNode, GRel } from "./shape.ts";

/** Convert neo4j Integers (and nested) to plain JS for printing. */
function normalize(v: any): any {
  if (v == null) return v;
  if (neo4j.isInt(v)) return v.toNumber();
  if (Array.isArray(v)) return v.map(normalize);
  if (typeof v === "object") {
    if (v.properties) return normalize(v.properties); // Node/Relationship
    const o: Record<string, any> = {};
    for (const [k, val] of Object.entries(v)) o[k] = normalize(val);
    return o;
  }
  return v;
}

export class Graph {
  private driver: Driver;

  constructor(uri: string, user: string, pass: string) {
    this.driver = neo4j.driver(uri, neo4j.auth.basic(user, pass));
  }

  async verifyConn(): Promise<void> {
    const s = this.driver.session();
    try {
      await s.run("RETURN 1");
    } catch (e) {
      throw new Error(`Cannot reach Neo4j. Is it running and NEO4J_* correct? Underlying: ${(e as Error).message}`);
    } finally {
      await s.close();
    }
  }

  async ensureIndexes(labels: string[]): Promise<void> {
    const s = this.driver.session();
    try {
      for (const l of labels) {
        await s.run(`CREATE INDEX IF NOT EXISTS FOR (n:\`${l}\`) ON (n.id)`);
      }
    } finally {
      await s.close();
    }
  }

  async mergeNodes(nodes: GNode[]): Promise<number> {
    const byLabel = new Map<string, GNode[]>();
    for (const n of nodes) {
      const arr = byLabel.get(n.label) ?? [];
      arr.push(n);
      byLabel.set(n.label, arr);
    }
    const s = this.driver.session();
    try {
      for (const [label, rows] of byLabel) {
        await s.run(
          `UNWIND $rows AS row MERGE (n:\`${label}\` {id: row.id}) SET n += row.props`,
          { rows: rows.map((n) => ({ id: n.id, props: n.props })) },
        );
      }
    } finally {
      await s.close();
    }
    return nodes.length;
  }

  async mergeRels(rels: GRel[]): Promise<number> {
    const byType = new Map<string, GRel[]>();
    for (const r of rels) {
      const arr = byType.get(r.type) ?? [];
      arr.push(r);
      byType.set(r.type, arr);
    }
    const s = this.driver.session();
    try {
      for (const [type, rows] of byType) {
        await s.run(
          `UNWIND $rows AS row MATCH (a {id: row.from}) MATCH (b {id: row.to}) ` +
            `MERGE (a)-[e:\`${type}\`]->(b) SET e += row.props`,
          { rows: rows.map((r) => ({ from: r.from, to: r.to, props: r.props })) },
        );
      }
    } finally {
      await s.close();
    }
    return rels.length;
  }

  async counts(): Promise<{ label: string; count: number }[]> {
    const s = this.driver.session();
    try {
      const r = await s.run("MATCH (n) RETURN labels(n)[0] AS label, count(*) AS c ORDER BY label");
      return r.records.map((x) => ({ label: x.get("label"), count: normalize(x.get("c")) }));
    } finally {
      await s.close();
    }
  }

  async indexes(): Promise<{ labels: string[]; props: string[]; state: string }[]> {
    const s = this.driver.session();
    try {
      const r = await s.run(
        "SHOW INDEXES YIELD labelsOrTypes, properties, state RETURN labelsOrTypes, properties, state",
      );
      return r.records.map((x) => ({
        labels: x.get("labelsOrTypes"),
        props: x.get("properties"),
        state: x.get("state"),
      }));
    } finally {
      await s.close();
    }
  }

  async query(cypher: string, params: Record<string, unknown> = {}): Promise<any[]> {
    const s = this.driver.session();
    try {
      const r = await s.run(cypher, params);
      return r.records.map((rec) => {
        const o: Record<string, any> = {};
        for (const k of rec.keys) o[k as string] = normalize(rec.get(k));
        return o;
      });
    } finally {
      await s.close();
    }
  }

  async clear(): Promise<void> {
    const s = this.driver.session();
    try {
      await s.run("MATCH (n) DETACH DELETE n");
    } finally {
      await s.close();
    }
  }

  async close(): Promise<void> {
    await this.driver.close();
  }
}
