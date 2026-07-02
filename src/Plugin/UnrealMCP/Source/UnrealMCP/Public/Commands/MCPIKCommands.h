#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Animation retargeting — IK Rig + IK Retargeter authoring + batch execution.
 *
 * Shipped surface:
 *   - list_ik_rig_chains              — read chain definitions + pelvis bone from an IK Rig
 *   - read_ik_retargeter              — inspect retargeter (source/target rigs, chain mappings)
 *   - create_ik_retargeter            — author a new UIKRetargeter asset (optionally setting source/target rigs)
 *   - set_ik_retargeter_rigs          — set source and/or target IK rigs on an existing retargeter
 *   - ik_retargeter_auto_map_chains   — name-based bulk mapping (Exact / Fuzzy / Clear)
 *   - set_ik_retargeter_chain_mapping — manually wire one source chain to one target chain
 *   - ik_retarget_run_batch           — duplicate-and-retarget a list of animation assets
 *
 * Authoring mutators run on the game thread, follow the universal PreEditChange /
 * mutate / PostEditChange / MarkPackageDirty contract, and save the asset before
 * returning.
 *
 * IK Rig authoring (chain creation, goal authoring, solver wiring) is NOT exposed —
 * the shipped IK rigs from upstream skeleton vendors (SMPL, Mixamo, Mannequin) are
 * the expected source-of-truth. Building a chain layout from raw bones is still an
 * editor-UI workflow.
 */
class FMCPIKCommands
{
public:
    FMCPIKCommands() = default;

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandleListIKRigChains(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleReadIKRetargeter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleCreateIKRetargeter(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetIKRetargeterRigs(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleIKRetargeterAutoMapChains(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleSetIKRetargeterChainMapping(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleIKRetargetRunBatch(const TSharedPtr<FJsonObject>& Params);
};
