#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * MCP command handler for runtime, component-space transform math against a
 * skeletal mesh in the editor world — the position's verify/produce
 * surface. Three tools:
 *
 *   - kinematic_read_transform : read a bone/socket's world + component-relative
 *                                transform (batch; reaches attached weapon meshes).
 *   - kinematic_probe          : forward FK — apply candidate bone rotation(s),
 *                                report the END-EFFECTOR world ΔP + ΔQ, scored
 *                                against an intended world direction. Dry-run by
 *                                default (copy the pose, RotateSubChain the copy,
 *                                recompose); opt-in live-apply-and-restore.
 *   - kinematic_solve          : inverse — solve a bone rotation that lands a tip
 *                                on a desired world direction (BoneIK::SolveTwoBoneIK).
 *
 * Design + invariants: docs/mcp/POSITION_PROBE_TOOLS.md.
 * Uses the vendored IK math (Private/Commands/Kinematics/BoneIKUtils.h —
 * verbatim copy of the consuming game's canonical header, see its top note)
 * so a verified rotation is exactly what the game's IK reproduces while the
 * plugin stays buildable standalone.
 *
 * Implementation split across 3 .cpp — see MCPKinematicsCommands.cpp
 * for the file map. Shared helpers live in the Private internal header
 * MCPKinematicsCommands_Internal.h.
 */
class UNREALMCP_API FMCPKinematicsCommands
{
public:
	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	TSharedPtr<FJsonObject> HandleReadTransform(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleProbe(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleSolve(const TSharedPtr<FJsonObject>& Params);
};
