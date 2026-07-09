#!/usr/bin/env bash
# Thin wrapper for the generic Unreal -> Neo4j ingester (bundled sibling tool/). Forwards
# all args to its CLI. Runs `bun install` once if deps are missing. Generic — no project
# hardcoded; scope with the CLI's selector flags. See tool/README.md.
#
#   neo4j.sh discover
#   neo4j.sh ingest --class StateTree
#   neo4j.sh query "MATCH (n) RETURN labels(n)[0], count(*)"
#
# Env (see README): MCP_URL, NEO4J_URI, NEO4J_USER, NEO4J_PASS (required for graph ops).
set -euo pipefail
tool="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/tool"
[ -d "$tool/node_modules" ] || { echo "=== installing deps (first run) ==="; (cd "$tool" && bun install); }
cd "$tool"
exec bun run src/cli.ts "$@"
