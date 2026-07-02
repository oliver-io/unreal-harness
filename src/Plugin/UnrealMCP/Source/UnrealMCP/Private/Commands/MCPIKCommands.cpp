#include "Commands/MCPIKCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetData.h"

#include "Rig/IKRigDefinition.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/RetargetOps/PelvisMotionOp.h" // FIKRetargetPelvisMotionOpSettings, UIKRetargetPelvisMotionController
#include "Retargeter/RetargetOps/RootMotionGeneratorOp.h" // FIKRetargetRootMotionOpSettings, ERootMotionSource, ERootMotionHeightSource
#include "RigEditor/IKRigController.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RetargetEditor/IKRetargeterPoseGenerator.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Animation/PoseAsset.h"
#include "Animation/AnimSequence.h"
#include "AnimationRuntime.h"
#include "AnimPose.h" // UAnimPoseExtensions::GetAnimPoseAtFrame (AnimationBlueprintLibrary module)
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimTypes.h" // FRawAnimSequenceTrack

namespace
{
    // Resolve an asset path into (PackagePath, AssetName). Mirrors the pattern used in
    // MCPAssetFactoryCommands — accepts either {path, name} or {asset_path}.
    bool ResolveDestination(const TSharedPtr<FJsonObject>& Params,
                            FString& OutPackagePath, FString& OutAssetName, FString& OutFullPath,
                            FString& OutError)
    {
        FString Path, Name;
        Params->TryGetStringField(TEXT("path"), Path);
        Params->TryGetStringField(TEXT("name"), Name);

        if (Path.IsEmpty() || Name.IsEmpty())
        {
            FString Combined;
            if (Params->TryGetStringField(TEXT("asset_path"), Combined) && !Combined.IsEmpty())
            {
                int32 LastSlash = INDEX_NONE;
                if (Combined.FindLastChar(TEXT('/'), LastSlash))
                {
                    Path = Combined.Left(LastSlash);
                    Name = Combined.Mid(LastSlash + 1);
                }
            }
        }

        if (Path.IsEmpty())
        {
            OutError = TEXT("Missing destination 'path' (or full 'asset_path'). Pass /Game/... package path.");
            return false;
        }
        if (Name.IsEmpty())
        {
            OutError = TEXT("Missing 'name' (or include the asset name in 'asset_path').");
            return false;
        }
        if (!Path.StartsWith(TEXT("/")))
        {
            OutError = FString::Printf(TEXT("Path must begin with '/Game/' or '/<Plugin>/' — got: %s"), *Path);
            return false;
        }

        OutPackagePath = Path;
        OutAssetName = Name;
        OutFullPath = Path / Name;
        return true;
    }

    // Load a UIKRigDefinition by asset path. Returns nullptr on miss / wrong class.
    UIKRigDefinition* LoadIKRig(const FString& Path)
    {
        if (Path.IsEmpty()) return nullptr;
        UObject* Loaded = UEditorAssetLibrary::LoadAsset(Path);
        return Cast<UIKRigDefinition>(Loaded);
    }

    // Load a UIKRetargeter by asset path. Returns nullptr on miss / wrong class.
    UIKRetargeter* LoadRetargeter(const FString& Path)
    {
        if (Path.IsEmpty()) return nullptr;
        UObject* Loaded = UEditorAssetLibrary::LoadAsset(Path);
        return Cast<UIKRetargeter>(Loaded);
    }

    // Parse the match_type string into the engine enum.
    bool ParseAutoMapType(const FString& In, EAutoMapChainType& Out, FString& OutError)
    {
        const FString Lower = In.ToLower();
        if (Lower == TEXT("exact"))      { Out = EAutoMapChainType::Exact; return true; }
        if (Lower == TEXT("fuzzy"))      { Out = EAutoMapChainType::Fuzzy; return true; }
        if (Lower == TEXT("clear"))      { Out = EAutoMapChainType::Clear; return true; }
        if (Lower.IsEmpty())             { Out = EAutoMapChainType::Fuzzy; return true; } // default
        OutError = FString::Printf(TEXT("Unknown match_type '%s' — expected 'Exact', 'Fuzzy', or 'Clear'."), *In);
        return false;
    }

    // Parse "source" or "target" → ERetargetSourceOrTarget. Default = source.
    bool ParseSourceOrTarget(const FString& In, ERetargetSourceOrTarget& Out, FString& OutError)
    {
        const FString Lower = In.ToLower();
        if (Lower == TEXT("source") || Lower.IsEmpty()) { Out = ERetargetSourceOrTarget::Source; return true; }
        if (Lower == TEXT("target"))                    { Out = ERetargetSourceOrTarget::Target; return true; }
        OutError = FString::Printf(TEXT("Unknown source_or_target '%s' — expected 'source' or 'target'."), *In);
        return false;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Free function: reset + AutoAlignAllBones for one side of a retargeter.
    // The "align" step rotates each bone in the chosen side's retarget pose so
    // its chain direction matches the corresponding chain on the OTHER side's
    // current retarget pose. Run this on Target AFTER importing a Source pose
    // (or vice-versa) when you've replaced the canonical reference frame on one side.
    // ─────────────────────────────────────────────────────────────────────────
    TSharedPtr<FJsonObject> IKRetargeterAlignBones_Impl(const TSharedPtr<FJsonObject>& Params)
    {
        FString RetargeterPath, SideStr;
        if (!Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'retargeter_path' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass the asset path of a UIKRetargeter."));
        }
        Params->TryGetStringField(TEXT("source_or_target"), SideStr);

        ERetargetSourceOrTarget Side;
        FString SideErr;
        if (!ParseSourceOrTarget(SideStr, Side, SideErr))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                SideErr, EMCPErrorCode::InvalidArgument,
                TEXT("Pass 'source' or 'target' (default = 'source')."));
        }

        bool bResetFirst = true;
        Params->TryGetBoolField(TEXT("reset_first"), bResetFirst);

        UIKRetargeter* Retargeter = LoadRetargeter(RetargeterPath);
        if (!Retargeter)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset is not a UIKRetargeter: %s"), *RetargeterPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("Verify the asset path resolves to an IK Retargeter."));
        }
        UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
        if (!Controller)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("UIKRetargeterController::GetController returned null for: %s"), *RetargeterPath),
                EMCPErrorCode::EngineBusy,
                TEXT("UE could not produce a controller for this retargeter."));
        }

        // Optional: bones to RESET back to bind AFTER auto-align. The editor's
        // AutoGenerateIKRetargetAsset uses this pattern (see SRetargetAnimAssetsWindow.cpp)
        // to keep specific bones (typically feet, or clavicles on novel skeletons)
        // at their bind orientation while every other bone gets auto-aligned.
        TArray<FName> ExcludedBones;
        const TArray<TSharedPtr<FJsonValue>>* ExcludedArr = nullptr;
        if (Params->TryGetArrayField(TEXT("excluded_bones"), ExcludedArr))
        {
            for (const TSharedPtr<FJsonValue>& V : *ExcludedArr)
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    ExcludedBones.Add(FName(*V->AsString()));
                }
            }
        }

        Retargeter->PreEditChange(nullptr);
        if (bResetFirst)
        {
            const FName CurrentPose = Controller->GetCurrentRetargetPoseName(Side);
            Controller->ResetRetargetPose(CurrentPose, TArray<FName>(), Side);
        }
        Controller->AutoAlignAllBones(Side, ERetargetAutoAlignMethod::ChainToChain);
        if (ExcludedBones.Num() > 0)
        {
            // Reset just the excluded bones back to bind. ResetRetargetPose with
            // a non-empty BonesToReset only zeroes deltas for those bones.
            const FName CurrentPose = Controller->GetCurrentRetargetPoseName(Side);
            Controller->ResetRetargetPose(CurrentPose, ExcludedBones, Side);
        }
        Retargeter->PostEditChange();
        Retargeter->MarkPackageDirty();
        if (!UEditorAssetLibrary::SaveAsset(Retargeter->GetPathName(), /*bOnlyIfIsDirty=*/false))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Aligned bones but failed to persist retargeter to disk: %s"), *Retargeter->GetPathName()),
                EMCPErrorCode::Internal,
                TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
        }

        TArray<TSharedPtr<FJsonValue>> ExcludedJsonArr;
        for (const FName& B : ExcludedBones) ExcludedJsonArr.Add(MakeShared<FJsonValueString>(B.ToString()));

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("retargeter_path"), Retargeter->GetPathName());
        ResultObj->SetStringField(TEXT("side"), (Side == ERetargetSourceOrTarget::Source) ? TEXT("source") : TEXT("target"));
        ResultObj->SetBoolField(TEXT("reset_first"), bResetFirst);
        ResultObj->SetArrayField(TEXT("excluded_bones"), ExcludedJsonArr);
        ResultObj->SetBoolField(TEXT("success"), true);
        return ResultObj;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Free function: sample a frame of a UAnimSequence and store it as a retarget pose.
    // Mirrors the editor's "Import Retarget Pose from Sequence" workflow
    // (FIKRetargetPoseExporter::OnImportPoseFromSequence). The pelvis bone gets a
    // global translation delta + a custom-computed local rotation delta (because the
    // pelvis is treated as living in global space during retargeting and its parents
    // stay at ref pose); every other bone gets a simple local rotation delta vs ref.
    //
    // Critical for novel sources like SMPL where the animation's IDLE frame differs
    // significantly from both the skeleton's bind pose AND any shipped retarget pose
    // asset. Picking the animation's frame-0 (or any idle reference frame) as the
    // source retarget pose makes the delta at that frame ≈ 0, so the target rig
    // outputs its own bind pose at the idle moment — natural rest stance.
    // ─────────────────────────────────────────────────────────────────────────
    TSharedPtr<FJsonObject> ImportRetargetPoseFromAnimation_Impl(const TSharedPtr<FJsonObject>& Params)
    {
        FString RetargeterPath, AnimSequencePath, SideStr;
        if (!Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'retargeter_path' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass the asset path of a UIKRetargeter."));
        }
        if (!Params->TryGetStringField(TEXT("anim_sequence_path"), AnimSequencePath))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'anim_sequence_path' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass the asset path of a UAnimSequence to sample a frame from."));
        }
        Params->TryGetStringField(TEXT("source_or_target"), SideStr);
        ERetargetSourceOrTarget Side;
        FString SideErr;
        if (!ParseSourceOrTarget(SideStr, Side, SideErr))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                SideErr, EMCPErrorCode::InvalidArgument,
                TEXT("Pass 'source' (default) or 'target'."));
        }
        int32 FrameIndex = 0;
        Params->TryGetNumberField(TEXT("frame_index"), FrameIndex);

        // Optional: list of case-insensitive substrings; any bone whose name contains
        // one of these is skipped during the bake. Lets callers exclude limb chains
        // (e.g. "Shoulder","Elbow") so those bones ride at the source rig's true T-bind
        // and the chain solver computes per-frame motion natively, while torso/clavicle
        // bones still get the frame-0 idle-posture correction.
        TArray<FString> ExcludeSubstrings;
        const TArray<TSharedPtr<FJsonValue>>* ExcludeArrayPtr = nullptr;
        if (Params->TryGetArrayField(TEXT("exclude_bone_substrings"), ExcludeArrayPtr) && ExcludeArrayPtr)
        {
            for (const TSharedPtr<FJsonValue>& Val : *ExcludeArrayPtr)
            {
                if (Val.IsValid() && Val->Type == EJson::String)
                {
                    const FString S = Val->AsString();
                    if (!S.IsEmpty())
                    {
                        ExcludeSubstrings.Add(S);
                    }
                }
            }
        }

        UIKRetargeter* Retargeter = LoadRetargeter(RetargeterPath);
        if (!Retargeter)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset is not a UIKRetargeter: %s"), *RetargeterPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("Verify the asset path resolves to an IK Retargeter."));
        }
        UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
        if (!Controller)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("UIKRetargeterController::GetController returned null"),
                EMCPErrorCode::EngineBusy,
                TEXT("UE could not produce a controller for this retargeter."));
        }
        UObject* AnimObj = UEditorAssetLibrary::LoadAsset(AnimSequencePath);
        UAnimSequence* AnimSequence = Cast<UAnimSequence>(AnimObj);
        if (!AnimSequence)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset is not a UAnimSequence: %s"), *AnimSequencePath),
                AnimObj ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
                TEXT("Pass an animation sequence asset path."));
        }

        const UIKRigDefinition* IKRig = Controller->GetIKRig(Side);
        if (!IKRig)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Retargeter has no IK rig set for side '%s'"), (Side == ERetargetSourceOrTarget::Source) ? TEXT("source") : TEXT("target")),
                EMCPErrorCode::AssetLocked,
                TEXT("Call set_ik_retargeter_rigs first to wire the rig for this side."));
        }
        UIKRigController* RigCtrl = UIKRigController::GetController(const_cast<UIKRigDefinition*>(IKRig));
        if (!RigCtrl)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("UIKRigController::GetController returned null for the side's IK rig"),
                EMCPErrorCode::EngineBusy,
                TEXT("Reopen the rig editor and retry."));
        }
        USkeletalMesh* Mesh = RigCtrl->GetSkeletalMesh();
        if (!Mesh)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Side IK rig has no skeletal mesh assigned"),
                EMCPErrorCode::AssetLocked,
                TEXT("Open the rig editor and assign a USkeletalMesh."));
        }

        // Evaluate the animation at the requested frame, anchored on the side's mesh proportions.
        FAnimPoseEvaluationOptions EvaluationOptions = FAnimPoseEvaluationOptions();
        EvaluationOptions.OptionalSkeletalMesh = Mesh;
        FAnimPose ImportedPose;
        const int32 NumSampledKeys = AnimSequence->GetNumberOfSampledKeys();
        const int32 ClampedFrame = FMath::Clamp(FrameIndex, 0, FMath::Max(0, NumSampledKeys - 1));
        UAnimPoseExtensions::GetAnimPoseAtFrame(AnimSequence, ClampedFrame, EvaluationOptions, ImportedPose);

        // Create a new retarget pose on the controller for this side.
        const FName SuggestedName = FName(*FString::Printf(TEXT("FromAnim_%s_F%d"), *AnimSequence->GetName(), ClampedFrame));
        const FName NewPoseName = Controller->CreateRetargetPose(SuggestedName, Side);
        TMap<FName, FIKRetargetPose>& RetargetPoses = Controller->GetRetargetPoses(Side);
        FIKRetargetPose* NewPosePtr = RetargetPoses.Find(NewPoseName);
        if (!NewPosePtr)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("CreateRetargetPose returned '%s' but pose not found in GetRetargetPoses"), *NewPoseName.ToString()),
                EMCPErrorCode::EngineBusy,
                TEXT("Pose creation silently failed."));
        }
        FIKRetargetPose& NewPose = *NewPosePtr;

        Retargeter->PreEditChange(nullptr);

        // Walk the side's IK rig ref skeleton, sample each bone from the animation,
        // record delta vs the side's ref pose. Pelvis bone gets the more elaborate
        // global-space treatment from the editor's recipe (translation + global rotation).
        const FName PelvisBoneName = IKRig->GetPelvis();
        const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
        const TArray<FTransform>& RefPose = RefSkeleton.GetRefBonePose();
        const int32 NumBones = RefSkeleton.GetNum();

        int32 BoneCountSet = 0;
        int32 BoneCountExcluded = 0;
        TArray<FString> ExcludedBoneNames;
        for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
        {
            const FName& BoneName = RefSkeleton.GetBoneName(BoneIndex);

            // Substring exclusion filter (case-insensitive). Pelvis is never excluded —
            // it carries the translation delta which we want regardless.
            if (BoneName != PelvisBoneName && ExcludeSubstrings.Num() > 0)
            {
                const FString BoneNameStr = BoneName.ToString();
                bool bExcluded = false;
                for (const FString& Sub : ExcludeSubstrings)
                {
                    if (BoneNameStr.Contains(Sub, ESearchCase::IgnoreCase))
                    {
                        bExcluded = true;
                        break;
                    }
                }
                if (bExcluded)
                {
                    ++BoneCountExcluded;
                    ExcludedBoneNames.Add(BoneNameStr);
                    continue;
                }
            }

            if (BoneName == PelvisBoneName)
            {
                // pelvis: translation delta + global rotation delta (rebased through ref-pose parent)
                const FTransform GlobalImported  = UAnimPoseExtensions::GetBonePose(ImportedPose, BoneName, EAnimPoseSpaces::World);
                const FTransform GlobalReference = UAnimPoseExtensions::GetRefBonePose(ImportedPose, BoneName, EAnimPoseSpaces::World);

                NewPose.SetRootTranslationDelta(GlobalImported.GetLocation() - GlobalReference.GetLocation());

                FTransform GlobalParentRef = FTransform::Identity;
                const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
                if (ParentIndex != INDEX_NONE)
                {
                    const FName& ParentBoneName = RefSkeleton.GetBoneName(ParentIndex);
                    GlobalParentRef = UAnimPoseExtensions::GetRefBonePose(ImportedPose, ParentBoneName, EAnimPoseSpaces::World);
                }

                // Rebase delta into a local-space rotation that, when applied to the
                // pelvis at ref-pose-parent orientation, lands at the animation's pose.
                const FQuat GlobalDelta       = GlobalImported.GetRotation() * GlobalReference.GetRotation().Inverse();
                const FQuat BoneGlobalOrig    = GlobalReference.GetRotation();
                const FQuat BoneGlobalPlusOff = GlobalDelta * BoneGlobalOrig;
                const FQuat ParentInv         = GlobalParentRef.GetRotation().Inverse();
                const FQuat BoneLocal         = ParentInv * BoneGlobalOrig;
                const FQuat BoneLocalPlusOff  = ParentInv * BoneGlobalPlusOff;
                const FQuat BoneLocalOffset   = BoneLocal * BoneLocalPlusOff.Inverse();
                NewPose.SetDeltaRotationForBone(BoneName, BoneLocalOffset.Inverse());
                ++BoneCountSet;
            }
            else
            {
                const FTransform LocalImported  = UAnimPoseExtensions::GetBonePose(ImportedPose, BoneName, EAnimPoseSpaces::Local);
                const FTransform& LocalReference = RefPose[BoneIndex];
                const FQuat DeltaRotation = LocalReference.GetRotation().Inverse() * LocalImported.GetRotation();
                const double DeltaAngleDeg = FMath::RadiansToDegrees(DeltaRotation.GetAngle());
                constexpr double MinAngleThresholdDeg = 0.05;
                if (DeltaAngleDeg >= MinAngleThresholdDeg)
                {
                    NewPose.SetDeltaRotationForBone(BoneName, DeltaRotation);
                    ++BoneCountSet;
                }
            }
        }

        bool bMakeCurrent = true;
        Params->TryGetBoolField(TEXT("make_current"), bMakeCurrent);
        if (bMakeCurrent)
        {
            Controller->SetCurrentRetargetPose(NewPoseName, Side);
        }

        Retargeter->PostEditChange();
        Retargeter->MarkPackageDirty();
        if (!UEditorAssetLibrary::SaveAsset(Retargeter->GetPathName(), /*bOnlyIfIsDirty=*/false))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Imported retarget pose from animation but failed to persist retargeter to disk: %s"), *Retargeter->GetPathName()),
                EMCPErrorCode::Internal,
                TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("retargeter_path"), Retargeter->GetPathName());
        ResultObj->SetStringField(TEXT("anim_sequence_path"), AnimSequence->GetPathName());
        ResultObj->SetStringField(TEXT("side"), (Side == ERetargetSourceOrTarget::Source) ? TEXT("source") : TEXT("target"));
        ResultObj->SetNumberField(TEXT("frame_index"), ClampedFrame);
        ResultObj->SetNumberField(TEXT("num_sampled_keys"), NumSampledKeys);
        ResultObj->SetStringField(TEXT("imported_pose_name"), NewPoseName.ToString());
        ResultObj->SetNumberField(TEXT("bones_with_delta"), BoneCountSet);
        ResultObj->SetNumberField(TEXT("bones_excluded"), BoneCountExcluded);
        {
            TArray<TSharedPtr<FJsonValue>> ExcludedJson;
            for (const FString& S : ExcludedBoneNames)
            {
                ExcludedJson.Add(MakeShared<FJsonValueString>(S));
            }
            ResultObj->SetArrayField(TEXT("excluded_bone_names"), ExcludedJson);
        }
        ResultObj->SetBoolField(TEXT("made_current"), bMakeCurrent);
        ResultObj->SetBoolField(TEXT("success"), true);
        return ResultObj;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Free function: import a UPoseAsset's named pose as a retarget pose on the
    // given side (source or target) of a UIKRetargeter. Lives in the anonymous
    // namespace so the function definition is body-only and does not require a
    // header change (live-coding compatible).
    //
    // Mirrors FIKRetargetPoseExporter::ImportPoseAsset (Editor's "Import retarget
    // pose from PoseAsset" button). Steps:
    //   1. Resolve retargeter + side + pose asset
    //   2. Look up the named pose's index in the pose asset (or default to 0)
    //   3. Read its full pose as local bone transforms
    //   4. Walk the side's IK rig ref skeleton, compute deltas vs the pose
    //   5. CreateRetargetPose() on the controller, fill deltas, set as current
    //   6. Save asset
    // ─────────────────────────────────────────────────────────────────────────
    TSharedPtr<FJsonObject> ImportRetargetPoseFromPoseAsset_Impl(const TSharedPtr<FJsonObject>& Params)
    {
        FString RetargeterPath, PoseAssetPath, SideStr, PoseNameStr;
        if (!Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'retargeter_path' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass the asset path of a UIKRetargeter."));
        }
        if (!Params->TryGetStringField(TEXT("pose_asset_path"), PoseAssetPath))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'pose_asset_path' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass the asset path of a UPoseAsset (the source of the retarget pose)."));
        }
        Params->TryGetStringField(TEXT("source_or_target"), SideStr);
        Params->TryGetStringField(TEXT("pose_name"), PoseNameStr);

        ERetargetSourceOrTarget Side;
        FString SideErr;
        if (!ParseSourceOrTarget(SideStr, Side, SideErr))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                SideErr, EMCPErrorCode::InvalidArgument,
                TEXT("Pass 'source' (default) or 'target'."));
        }

        UIKRetargeter* Retargeter = LoadRetargeter(RetargeterPath);
        if (!Retargeter)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset is not a UIKRetargeter: %s"), *RetargeterPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("Verify the asset path resolves to an IK Retargeter."));
        }
        UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
        if (!Controller)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("UIKRetargeterController::GetController returned null for: %s"), *RetargeterPath),
                EMCPErrorCode::EngineBusy,
                TEXT("UE could not produce a controller for this retargeter."));
        }

        UObject* PoseObj = UEditorAssetLibrary::LoadAsset(PoseAssetPath);
        UPoseAsset* PoseAsset = Cast<UPoseAsset>(PoseObj);
        if (!PoseAsset)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset is not a UPoseAsset: %s"), *PoseAssetPath),
                PoseObj ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
                TEXT("Pass a UPoseAsset asset path (e.g. /Game/Characters/Retarget/PA_RetargetPose)."));
        }

        const UIKRigDefinition* IKRig = Controller->GetIKRig(Side);
        if (!IKRig)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Retargeter has no IK rig set for side '%s'"), (Side == ERetargetSourceOrTarget::Source) ? TEXT("source") : TEXT("target")),
                EMCPErrorCode::AssetLocked,
                TEXT("Call set_ik_retargeter_rigs first to wire the rig for this side."));
        }
        UIKRigController* RigCtrl = UIKRigController::GetController(const_cast<UIKRigDefinition*>(IKRig));
        if (!RigCtrl)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("UIKRigController::GetController returned null for the side's IK rig"),
                EMCPErrorCode::EngineBusy,
                TEXT("Reopen the rig editor and retry."));
        }
        USkeletalMesh* Mesh = RigCtrl->GetSkeletalMesh();
        if (!Mesh)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Side IK rig has no skeletal mesh assigned"),
                EMCPErrorCode::AssetLocked,
                TEXT("Open the rig editor and assign a USkeletalMesh."));
        }

        // Resolve which named pose inside the PoseAsset to import. Pose assets can hold
        // multiple named poses; default to the first if the caller doesn't specify.
        int32 PoseIndex = INDEX_NONE;
        if (!PoseNameStr.IsEmpty())
        {
            PoseIndex = PoseAsset->GetPoseIndexByName(FName(*PoseNameStr));
            if (PoseIndex == INDEX_NONE)
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Pose '%s' not found in pose asset %s"), *PoseNameStr, *PoseAssetPath),
                    EMCPErrorCode::InvalidArgument,
                    TEXT("Inspect the pose asset in the editor and use one of its declared pose names."));
            }
        }
        else
        {
            PoseIndex = 0; // first pose
        }

        TArray<FTransform> LocalBoneTransformsFromPose;
        if (!PoseAsset->GetFullPose(PoseIndex, LocalBoneTransformsFromPose))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("UPoseAsset::GetFullPose failed for pose index %d in %s"), PoseIndex, *PoseAssetPath),
                EMCPErrorCode::EngineBusy,
                TEXT("Pose asset may be empty or corrupted."));
        }

        const FName PelvisBoneName = IKRig->GetPelvis();
        const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
        const TArray<FTransform>& RefPose = RefSkeleton.GetRefBonePose();

        // Create a new retarget pose on the controller for this side.
        const FName SuggestedName = FName(*FString::Printf(TEXT("Imported_%s"), *PoseAsset->GetName()));
        const FName NewPoseName = Controller->CreateRetargetPose(SuggestedName, Side);
        TMap<FName, FIKRetargetPose>& RetargetPoses = Controller->GetRetargetPoses(Side);
        FIKRetargetPose* NewPosePtr = RetargetPoses.Find(NewPoseName);
        if (!NewPosePtr)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("CreateRetargetPose returned name '%s' but the pose was not found in GetRetargetPoses map"), *NewPoseName.ToString()),
                EMCPErrorCode::EngineBusy,
                TEXT("Pose creation appears to have failed silently."));
        }
        FIKRetargetPose& NewPose = *NewPosePtr;

        Retargeter->PreEditChange(nullptr);

        // Walk the reference skeleton, accumulate per-bone rotation deltas (and pelvis translation delta).
        int32 BoneCountSet = 0;
        for (int32 BoneIndex = 0; BoneIndex < RefSkeleton.GetNum(); ++BoneIndex)
        {
            const FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
            const int32 PoseTrackIndex = PoseAsset->GetTrackIndexByName(BoneName);
            if (PoseTrackIndex == INDEX_NONE)
            {
                continue;
            }

            // Pelvis: also record global translation offset.
            if (BoneName == PelvisBoneName)
            {
                const FTransform GlobalRefTransform   = FAnimationRuntime::GetComponentSpaceTransform(RefSkeleton, RefPose, BoneIndex);
                const FTransform GlobalImportedTransform = PoseAsset->GetComponentSpaceTransform(BoneName, LocalBoneTransformsFromPose);
                NewPose.SetRootTranslationDelta(GlobalImportedTransform.GetLocation() - GlobalRefTransform.GetLocation());
            }

            // Local-space rotation delta vs the ref skeleton bind pose.
            const FTransform& LocalRefTransform      = RefPose[BoneIndex];
            const FTransform& LocalImportedTransform = LocalBoneTransformsFromPose[PoseTrackIndex];
            const FQuat DeltaRotation = LocalRefTransform.GetRotation().Inverse() * LocalImportedTransform.GetRotation();
            const double DeltaAngleDeg = FMath::RadiansToDegrees(DeltaRotation.GetAngle());
            constexpr double MinAngleThresholdDeg = 0.05;
            if (DeltaAngleDeg >= MinAngleThresholdDeg)
            {
                NewPose.SetDeltaRotationForBone(BoneName, DeltaRotation);
                ++BoneCountSet;
            }
        }

        // Make the imported pose the current one for this side. Without this, the
        // import is recorded but the retargeter still uses the previous (default) pose.
        bool bMakeCurrent = true;
        Params->TryGetBoolField(TEXT("make_current"), bMakeCurrent);
        if (bMakeCurrent)
        {
            Controller->SetCurrentRetargetPose(NewPoseName, Side);
        }

        Retargeter->PostEditChange();
        Retargeter->MarkPackageDirty();
        if (!UEditorAssetLibrary::SaveAsset(Retargeter->GetPathName(), /*bOnlyIfIsDirty=*/false))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Imported retarget pose from pose asset but failed to persist retargeter to disk: %s"), *Retargeter->GetPathName()),
                EMCPErrorCode::Internal,
                TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("retargeter_path"), Retargeter->GetPathName());
        ResultObj->SetStringField(TEXT("pose_asset_path"), PoseAsset->GetPathName());
        ResultObj->SetStringField(TEXT("side"), (Side == ERetargetSourceOrTarget::Source) ? TEXT("source") : TEXT("target"));
        ResultObj->SetStringField(TEXT("imported_pose_name"), NewPoseName.ToString());
        ResultObj->SetNumberField(TEXT("pose_index"), PoseIndex);
        ResultObj->SetNumberField(TEXT("bones_with_delta"), BoneCountSet);
        ResultObj->SetBoolField(TEXT("made_current"), bMakeCurrent);
        ResultObj->SetBoolField(TEXT("success"), true);
        return ResultObj;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Free function: tune the Pelvis Motion Op's per-axis pelvis translation
    // and rotation handling.
    //
    // Why it exists: in 5.7 the retargeter's pelvis behavior moved out of the
    // deprecated URetargetRootSettings into a stack op — FIKRetargetPelvisMotionOp.
    // For sources like SMPL where per-frame pelvis Z micro-variation reads as
    // a knee bounce on the taller target, the cleanest fix is to drive that
    // op's ScaleVertical to 0 (or TranslationAlpha down) so the noise is not
    // carried through to the target.
    //
    // Parameters (all optional; only ones present are written):
    //   retargeter_path        — UIKRetargeter asset path (required)
    //   translation_alpha      — 0..1, overall pelvis translation blend
    //   scale_horizontal       — pelvis X/Y translation scale (default 1.0)
    //   scale_vertical         — pelvis Z translation scale (default 1.0)
    //                            Set to 0 to lock pelvis to retarget-pose Z.
    //   blend_to_source_translation — 0..1
    //   blend_to_source_x / _y / _z — per-axis weights for blend-to-source
    //   rotation_alpha         — 0..1, pelvis rotation blend
    //   affect_ik_horizontal   — 0..1
    //   affect_ik_vertical     — 0..1
    //   floor_constraint_weight — 0..1
    // ─────────────────────────────────────────────────────────────────────────
    TSharedPtr<FJsonObject> SetRetargeterPelvisSettings_Impl(const TSharedPtr<FJsonObject>& Params)
    {
        FString RetargeterPath;
        if (!Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'retargeter_path' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass the asset path of a UIKRetargeter."));
        }

        UIKRetargeter* Retargeter = LoadRetargeter(RetargeterPath);
        if (!Retargeter)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset is not a UIKRetargeter: %s"), *RetargeterPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("Verify the asset path resolves to an IK Retargeter."));
        }

        // Mutate the live op's settings in place. We deliberately avoid copying
        // FIKRetargetPelvisMotionOpSettings — its PostLoad symbol is not exported
        // from the IKRig module (5.7 oversight), so any out-of-module copy/construct
        // triggers a link error. GetFirstRetargetOpOfType<>() hands back a pointer
        // to the live FInstancedStruct storage, where field-level reads/writes are
        // just offsets and don't pull in the vtable PostLoad slot.
        FIKRetargetPelvisMotionOp* PelvisOp = Retargeter->GetFirstRetargetOpOfType<FIKRetargetPelvisMotionOp>();
        if (!PelvisOp)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("No Pelvis Motion op present in the retargeter stack"),
                EMCPErrorCode::AssetLocked,
                TEXT("Run AddDefaultOps (or recreate the retargeter) so the Pelvis Motion op exists, then retry."));
        }

        Retargeter->PreEditChange(nullptr);

        FIKRetargetPelvisMotionOpSettings& Settings = PelvisOp->Settings;
        TArray<FString> Written;

        double TranslationAlpha = 0.0;
        if (Params->TryGetNumberField(TEXT("translation_alpha"), TranslationAlpha))
        {
            Settings.TranslationAlpha = FMath::Clamp(TranslationAlpha, 0.0, 1.0);
            Written.Add(FString::Printf(TEXT("translation_alpha=%.4f"), Settings.TranslationAlpha));
        }

        double ScaleHorizontal = 0.0;
        if (Params->TryGetNumberField(TEXT("scale_horizontal"), ScaleHorizontal))
        {
            Settings.ScaleHorizontal = FMath::Max(0.0, ScaleHorizontal);
            Written.Add(FString::Printf(TEXT("scale_horizontal=%.4f"), Settings.ScaleHorizontal));
        }

        double ScaleVertical = 0.0;
        if (Params->TryGetNumberField(TEXT("scale_vertical"), ScaleVertical))
        {
            Settings.ScaleVertical = FMath::Max(0.0, ScaleVertical);
            Written.Add(FString::Printf(TEXT("scale_vertical=%.4f"), Settings.ScaleVertical));
        }

        double BlendToSrcTranslation = 0.0;
        if (Params->TryGetNumberField(TEXT("blend_to_source_translation"), BlendToSrcTranslation))
        {
            Settings.BlendToSourceTranslation = FMath::Clamp(BlendToSrcTranslation, 0.0, 1.0);
            Written.Add(FString::Printf(TEXT("blend_to_source_translation=%.4f"), Settings.BlendToSourceTranslation));
        }

        double BlendX = 0.0, BlendY = 0.0, BlendZ = 0.0;
        bool bAnyBlendWeight = false;
        if (Params->TryGetNumberField(TEXT("blend_to_source_x"), BlendX))
        {
            Settings.BlendToSourceTranslationWeights.X = FMath::Clamp(BlendX, 0.0, 1.0);
            bAnyBlendWeight = true;
        }
        if (Params->TryGetNumberField(TEXT("blend_to_source_y"), BlendY))
        {
            Settings.BlendToSourceTranslationWeights.Y = FMath::Clamp(BlendY, 0.0, 1.0);
            bAnyBlendWeight = true;
        }
        if (Params->TryGetNumberField(TEXT("blend_to_source_z"), BlendZ))
        {
            Settings.BlendToSourceTranslationWeights.Z = FMath::Clamp(BlendZ, 0.0, 1.0);
            bAnyBlendWeight = true;
        }
        if (bAnyBlendWeight)
        {
            Written.Add(FString::Printf(TEXT("blend_to_source_weights=(%.3f, %.3f, %.3f)"),
                Settings.BlendToSourceTranslationWeights.X,
                Settings.BlendToSourceTranslationWeights.Y,
                Settings.BlendToSourceTranslationWeights.Z));
        }

        double RotationAlpha = 0.0;
        if (Params->TryGetNumberField(TEXT("rotation_alpha"), RotationAlpha))
        {
            Settings.RotationAlpha = FMath::Clamp(RotationAlpha, 0.0, 1.0);
            Written.Add(FString::Printf(TEXT("rotation_alpha=%.4f"), Settings.RotationAlpha));
        }

        double AffectIKHorizontal = 0.0;
        if (Params->TryGetNumberField(TEXT("affect_ik_horizontal"), AffectIKHorizontal))
        {
            Settings.AffectIKHorizontal = FMath::Clamp(AffectIKHorizontal, 0.0, 1.0);
            Written.Add(FString::Printf(TEXT("affect_ik_horizontal=%.4f"), Settings.AffectIKHorizontal));
        }

        double AffectIKVertical = 0.0;
        if (Params->TryGetNumberField(TEXT("affect_ik_vertical"), AffectIKVertical))
        {
            Settings.AffectIKVertical = FMath::Clamp(AffectIKVertical, 0.0, 1.0);
            Written.Add(FString::Printf(TEXT("affect_ik_vertical=%.4f"), Settings.AffectIKVertical));
        }

        double FloorConstraintWeight = 0.0;
        if (Params->TryGetNumberField(TEXT("floor_constraint_weight"), FloorConstraintWeight))
        {
            Settings.FloorConstraintWeight = FMath::Clamp(FloorConstraintWeight, 0.0, 1.0);
            Written.Add(FString::Printf(TEXT("floor_constraint_weight=%.4f"), Settings.FloorConstraintWeight));
        }

        Retargeter->IncrementVersion();
        Retargeter->PostEditChange();
        Retargeter->MarkPackageDirty();
        if (!UEditorAssetLibrary::SaveAsset(Retargeter->GetPathName(), /*bOnlyIfIsDirty=*/false))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Updated pelvis settings but failed to persist retargeter to disk: %s"), *Retargeter->GetPathName()),
                EMCPErrorCode::Internal,
                TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("retargeter_path"), Retargeter->GetPathName());
        ResultObj->SetNumberField(TEXT("translation_alpha"), Settings.TranslationAlpha);
        ResultObj->SetNumberField(TEXT("scale_horizontal"), Settings.ScaleHorizontal);
        ResultObj->SetNumberField(TEXT("scale_vertical"), Settings.ScaleVertical);
        ResultObj->SetNumberField(TEXT("rotation_alpha"), Settings.RotationAlpha);
        ResultObj->SetNumberField(TEXT("blend_to_source_translation"), Settings.BlendToSourceTranslation);
        ResultObj->SetNumberField(TEXT("affect_ik_horizontal"), Settings.AffectIKHorizontal);
        ResultObj->SetNumberField(TEXT("affect_ik_vertical"), Settings.AffectIKVertical);
        ResultObj->SetNumberField(TEXT("floor_constraint_weight"), Settings.FloorConstraintWeight);
        {
            TArray<TSharedPtr<FJsonValue>> WrittenJson;
            for (const FString& S : Written) { WrittenJson.Add(MakeShared<FJsonValueString>(S)); }
            ResultObj->SetArrayField(TEXT("written_fields"), WrittenJson);
        }
        ResultObj->SetBoolField(TEXT("success"), true);
        return ResultObj;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Free function: tune the Root Motion Op's root-bone handling.
    //
    // The Root Motion op writes the target Root bone's transform each frame.
    // Default (CopyHeightFromSource) faithfully copies source root Z, which on
    // a noisy source like SMPL causes a whole-body vertical bob (knees flex to
    // compensate). SnapToGround locks the root to Z=0, killing the bob without
    // touching pelvis settings. RootMotionSource = GenerateFromTargetPelvis is
    // an alternative that derives root motion from the (retargeted, already-
    // sanitized) pelvis instead of the raw source.
    //
    // Parameters (all optional; only present ones are written):
    //   retargeter_path                — UIKRetargeter asset path (required)
    //   root_motion_source             — "copy_from_source_root" or "generate_from_target_pelvis"
    //   root_height_source             — "copy_height_from_source" or "snap_to_ground"
    //   rotate_with_pelvis             — bool (only relevant when source = pelvis)
    //   maintain_offset_from_pelvis    — bool
    //   propagate_to_non_retargeted_children — bool
    // ─────────────────────────────────────────────────────────────────────────
    TSharedPtr<FJsonObject> SetRetargeterRootMotionSettings_Impl(const TSharedPtr<FJsonObject>& Params)
    {
        FString RetargeterPath;
        if (!Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'retargeter_path' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass the asset path of a UIKRetargeter."));
        }

        UIKRetargeter* Retargeter = LoadRetargeter(RetargeterPath);
        if (!Retargeter)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset is not a UIKRetargeter: %s"), *RetargeterPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("Verify the asset path resolves to an IK Retargeter."));
        }

        FIKRetargetRootMotionOp* RootOp = Retargeter->GetFirstRetargetOpOfType<FIKRetargetRootMotionOp>();
        if (!RootOp)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("No Root Motion op present in the retargeter stack"),
                EMCPErrorCode::AssetLocked,
                TEXT("Run AddDefaultOps (or recreate the retargeter) so the Root Motion op exists, then retry."));
        }

        Retargeter->PreEditChange(nullptr);

        FIKRetargetRootMotionOpSettings& Settings = RootOp->Settings;
        TArray<FString> Written;

        FString RootMotionSourceStr;
        if (Params->TryGetStringField(TEXT("root_motion_source"), RootMotionSourceStr))
        {
            if (RootMotionSourceStr.Equals(TEXT("copy_from_source_root"), ESearchCase::IgnoreCase))
            {
                Settings.RootMotionSource = ERootMotionSource::CopyFromSourceRoot;
                Written.Add(TEXT("root_motion_source=copy_from_source_root"));
            }
            else if (RootMotionSourceStr.Equals(TEXT("generate_from_target_pelvis"), ESearchCase::IgnoreCase))
            {
                Settings.RootMotionSource = ERootMotionSource::GenerateFromTargetPelvis;
                Written.Add(TEXT("root_motion_source=generate_from_target_pelvis"));
            }
            else
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Unknown root_motion_source: '%s'"), *RootMotionSourceStr),
                    EMCPErrorCode::InvalidArgument,
                    TEXT("Use 'copy_from_source_root' or 'generate_from_target_pelvis'."));
            }
        }

        FString RootHeightSourceStr;
        if (Params->TryGetStringField(TEXT("root_height_source"), RootHeightSourceStr))
        {
            if (RootHeightSourceStr.Equals(TEXT("copy_height_from_source"), ESearchCase::IgnoreCase))
            {
                Settings.RootHeightSource = ERootMotionHeightSource::CopyHeightFromSource;
                Written.Add(TEXT("root_height_source=copy_height_from_source"));
            }
            else if (RootHeightSourceStr.Equals(TEXT("snap_to_ground"), ESearchCase::IgnoreCase))
            {
                Settings.RootHeightSource = ERootMotionHeightSource::SnapToGround;
                Written.Add(TEXT("root_height_source=snap_to_ground"));
            }
            else
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Unknown root_height_source: '%s'"), *RootHeightSourceStr),
                    EMCPErrorCode::InvalidArgument,
                    TEXT("Use 'copy_height_from_source' or 'snap_to_ground'."));
            }
        }

        bool bRotateWithPelvis = false;
        if (Params->TryGetBoolField(TEXT("rotate_with_pelvis"), bRotateWithPelvis))
        {
            Settings.bRotateWithPelvis = bRotateWithPelvis;
            Written.Add(FString::Printf(TEXT("rotate_with_pelvis=%s"), bRotateWithPelvis ? TEXT("true") : TEXT("false")));
        }

        bool bMaintainOffsetFromPelvis = false;
        if (Params->TryGetBoolField(TEXT("maintain_offset_from_pelvis"), bMaintainOffsetFromPelvis))
        {
            Settings.bMaintainOffsetFromPelvis = bMaintainOffsetFromPelvis;
            Written.Add(FString::Printf(TEXT("maintain_offset_from_pelvis=%s"), bMaintainOffsetFromPelvis ? TEXT("true") : TEXT("false")));
        }

        bool bPropagate = false;
        if (Params->TryGetBoolField(TEXT("propagate_to_non_retargeted_children"), bPropagate))
        {
            Settings.bPropagateToNonRetargetedChildren = bPropagate;
            Written.Add(FString::Printf(TEXT("propagate_to_non_retargeted_children=%s"), bPropagate ? TEXT("true") : TEXT("false")));
        }

        Retargeter->IncrementVersion();
        Retargeter->PostEditChange();
        Retargeter->MarkPackageDirty();
        if (!UEditorAssetLibrary::SaveAsset(Retargeter->GetPathName(), /*bOnlyIfIsDirty=*/false))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Updated root-motion settings but failed to persist retargeter to disk: %s"), *Retargeter->GetPathName()),
                EMCPErrorCode::Internal,
                TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("retargeter_path"), Retargeter->GetPathName());
        ResultObj->SetStringField(TEXT("root_motion_source"),
            (Settings.RootMotionSource == ERootMotionSource::CopyFromSourceRoot)
                ? TEXT("copy_from_source_root") : TEXT("generate_from_target_pelvis"));
        ResultObj->SetStringField(TEXT("root_height_source"),
            (Settings.RootHeightSource == ERootMotionHeightSource::CopyHeightFromSource)
                ? TEXT("copy_height_from_source") : TEXT("snap_to_ground"));
        ResultObj->SetBoolField(TEXT("rotate_with_pelvis"), Settings.bRotateWithPelvis);
        ResultObj->SetBoolField(TEXT("maintain_offset_from_pelvis"), Settings.bMaintainOffsetFromPelvis);
        ResultObj->SetBoolField(TEXT("propagate_to_non_retargeted_children"), Settings.bPropagateToNonRetargetedChildren);
        {
            TArray<TSharedPtr<FJsonValue>> WrittenJson;
            for (const FString& S : Written) { WrittenJson.Add(MakeShared<FJsonValueString>(S)); }
            ResultObj->SetArrayField(TEXT("written_fields"), WrittenJson);
        }
        ResultObj->SetBoolField(TEXT("success"), true);
        return ResultObj;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Build a symmetric kernel of length Window centered at index Half.
    //   "box"      → uniform weights (1/Window each), equivalent to a moving
    //                average. Sinc-shaped frequency response with sidelobes.
    //   "gaussian" → weights ∝ exp(-(j-Half)²/(2σ²)) then normalized to sum=1.
    //                Sigma defaults to Window/6 so ±3σ ≈ window width
    //                (captures ~99.7% of the bell). Monotonic frequency
    //                response with no sidelobes; ~20 dB cleaner stopband
    //                than box for same window width.
    // ─────────────────────────────────────────────────────────────────────────
    static TArray<double> BuildKernel(int32 Window, const FString& FilterType, double SigmaFrames)
    {
        const int32 Half = Window / 2;
        TArray<double> W;
        W.SetNum(Window);
        const bool bGaussian = FilterType.Equals(TEXT("gaussian"), ESearchCase::IgnoreCase);
        if (bGaussian)
        {
            const double Sigma = (SigmaFrames > 0.0) ? SigmaFrames : (double)Window / 6.0;
            const double TwoSigmaSq = 2.0 * Sigma * Sigma;
            double Sum = 0.0;
            for (int32 i = 0; i < Window; ++i)
            {
                const double Dx = (double)(i - Half);
                W[i] = FMath::Exp(-(Dx * Dx) / TwoSigmaSq);
                Sum += W[i];
            }
            if (Sum > 0.0) { for (double& V : W) V /= Sum; }
        }
        else
        {
            const double V = 1.0 / (double)Window;
            for (double& X : W) X = V;
        }
        return W;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Quaternion weighted-average smoothing with antipodal flipping.
    // For each key, weighted-sum quaternions in a symmetric window (flipping
    // the sign of any quaternion whose dot product with the center key is
    // negative, so we average along the short arc), then normalize. The
    // weights vector defines the kernel shape (box vs Gaussian etc).
    // For small noise this is visually indistinguishable from proper
    // Lie-algebra averaging but cheap.
    // ─────────────────────────────────────────────────────────────────────────
    static TArray<FQuat> SmoothQuatCurve(const TArray<FQuat>& Keys, const TArray<double>& Weights)
    {
        TArray<FQuat> Out;
        Out.SetNum(Keys.Num());
        if (Keys.Num() == 0) return Out;
        const int32 Window = Weights.Num();
        const int32 Half = Window / 2;
        for (int32 i = 0; i < Keys.Num(); ++i)
        {
            const FQuat Ref = Keys[i];
            FQuat Sum(0.0, 0.0, 0.0, 0.0);
            for (int32 k = 0; k < Window; ++k)
            {
                const int32 Idx = FMath::Clamp(i + (k - Half), 0, Keys.Num() - 1);
                FQuat Q = Keys[Idx];
                if ((Q.X * Ref.X + Q.Y * Ref.Y + Q.Z * Ref.Z + Q.W * Ref.W) < 0.0)
                {
                    Q.X = -Q.X; Q.Y = -Q.Y; Q.Z = -Q.Z; Q.W = -Q.W;
                }
                const double W = Weights[k];
                Sum.X += Q.X * W; Sum.Y += Q.Y * W;
                Sum.Z += Q.Z * W; Sum.W += Q.W * W;
            }
            Sum.Normalize();
            Out[i] = Sum;
        }
        return Out;
    }

    static TArray<FVector> SmoothVectorCurve(const TArray<FVector>& Keys, const TArray<double>& Weights)
    {
        TArray<FVector> Out;
        Out.SetNum(Keys.Num());
        if (Keys.Num() == 0) return Out;
        const int32 Window = Weights.Num();
        const int32 Half = Window / 2;
        for (int32 i = 0; i < Keys.Num(); ++i)
        {
            FVector Sum(0.0, 0.0, 0.0);
            for (int32 k = 0; k < Window; ++k)
            {
                const int32 Idx = FMath::Clamp(i + (k - Half), 0, Keys.Num() - 1);
                Sum += Keys[Idx] * Weights[k];
            }
            Out[i] = Sum;
        }
        return Out;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // smooth_anim_sequence — apply a smoothing filter to a UAnimSequence's
    // bone tracks. Targeted at SMPL text-to-motion outputs where per-frame
    // rotation noise is invisible at SMPL preview scale but visibly amplified
    // when retargeted onto larger skeletons (knee bounce, twitch).
    //
    // Parameters:
    //   anim_path             — UAnimSequence asset path (required)
    //   window_size           — odd integer, default 5. Sliding window width.
    //   filter_type           — "box" (default, uniform weights) or "gaussian"
    //                           (bell-shaped weights, σ = window/6 unless
    //                           overridden). Gaussian has cleaner stopband
    //                           (~20 dB better) for the same window width.
    //   sigma_frames          — for Gaussian only. Overrides the default
    //                           σ = window_size / 6. Larger sigma = wider
    //                           effective filter. Ignored when filter_type=box.
    //   output_suffix         — if non-empty, duplicate the asset and smooth
    //                           the copy (default "_Smoothed"). Empty string
    //                           = smooth in place.
    //   bone_substring_filter — optional array of case-insensitive substrings;
    //                           only bones matching ANY substring are smoothed.
    //                           Empty/missing = smooth ALL bones.
    //   smooth_positions      — bool, default false. When true, also applies
    //                           the same kernel to positional keys.
    // ─────────────────────────────────────────────────────────────────────────
    TSharedPtr<FJsonObject> SmoothAnimSequence_Impl(const TSharedPtr<FJsonObject>& Params)
    {
        FString AnimPath;
        if (!Params->TryGetStringField(TEXT("anim_path"), AnimPath))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'anim_path' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass the asset path of a UAnimSequence."));
        }

        int32 WindowSize = 5;
        Params->TryGetNumberField(TEXT("window_size"), WindowSize);
        WindowSize = FMath::Max(3, WindowSize);
        if ((WindowSize % 2) == 0) ++WindowSize; // force odd for symmetric window

        FString FilterType = TEXT("box");
        Params->TryGetStringField(TEXT("filter_type"), FilterType);
        if (!FilterType.Equals(TEXT("box"), ESearchCase::IgnoreCase) &&
            !FilterType.Equals(TEXT("gaussian"), ESearchCase::IgnoreCase))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Unknown filter_type: '%s'"), *FilterType),
                EMCPErrorCode::InvalidArgument,
                TEXT("Use 'box' (default) or 'gaussian'."));
        }

        double SigmaFrames = 0.0; // 0 → use default (window/6) inside BuildKernel
        Params->TryGetNumberField(TEXT("sigma_frames"), SigmaFrames);

        FString OutputSuffix = TEXT("_Smoothed");
        Params->TryGetStringField(TEXT("output_suffix"), OutputSuffix);

        bool bSmoothPositions = false;
        Params->TryGetBoolField(TEXT("smooth_positions"), bSmoothPositions);

        TArray<FString> BoneFilter;
        const TArray<TSharedPtr<FJsonValue>>* FilterArrayPtr = nullptr;
        if (Params->TryGetArrayField(TEXT("bone_substring_filter"), FilterArrayPtr) && FilterArrayPtr)
        {
            for (const TSharedPtr<FJsonValue>& V : *FilterArrayPtr)
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    const FString S = V->AsString();
                    if (!S.IsEmpty()) BoneFilter.Add(S);
                }
            }
        }

        UObject* SrcObj = UEditorAssetLibrary::LoadAsset(AnimPath);
        UAnimSequence* SrcSeq = Cast<UAnimSequence>(SrcObj);
        if (!SrcSeq)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset is not a UAnimSequence: %s"), *AnimPath),
                SrcObj ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
                TEXT("Pass an animation sequence asset path."));
        }

        UAnimSequence* TargetSeq = SrcSeq;
        FString OutputPath = SrcSeq->GetPathName();

        if (!OutputSuffix.IsEmpty())
        {
            // Duplicate source to a new asset alongside the original.
            FString PackagePath = FPackageName::GetLongPackagePath(SrcSeq->GetPathName());
            FString NewName = SrcSeq->GetName() + OutputSuffix;
            FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
            UObject* DuplicatedObj = AssetToolsModule.Get().DuplicateAsset(NewName, PackagePath, SrcSeq);
            TargetSeq = Cast<UAnimSequence>(DuplicatedObj);
            if (!TargetSeq)
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Failed to duplicate '%s' to '%s/%s'"), *AnimPath, *PackagePath, *NewName),
                    EMCPErrorCode::EngineBusy,
                    TEXT("AssetTools::DuplicateAsset returned null. Verify the destination is writable."));
            }
            OutputPath = TargetSeq->GetPathName();
        }

        const IAnimationDataModel* Model = TargetSeq->GetDataModel();
        if (!Model)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("UAnimSequence has no IAnimationDataModel"),
                EMCPErrorCode::EngineBusy,
                TEXT("The animation asset is missing its data model — try resaving in the editor first."));
        }
        IAnimationDataController& Controller = TargetSeq->GetController();

        TArray<FName> BoneNames;
        Model->GetBoneTrackNames(BoneNames);

        // Build the smoothing kernel once — shared across all bone tracks.
        const TArray<double> Kernel = BuildKernel(WindowSize, FilterType, SigmaFrames);

        // 5.7-modern API: GetBoneTrackTransforms gives us the full FTransform
        // array for the track. We split into pos/rot/scale, smooth, and write
        // back via the controller's FVector/FQuat overload of SetBoneTrackKeys.
        Controller.OpenBracket(FText::FromString(TEXT("MCP smooth_anim_sequence")), /*bShouldTransact=*/false);

        int32 BonesSmoothed = 0;
        int32 BonesSkipped = 0;
        TArray<FString> SmoothedBoneNames;
        for (const FName& BoneName : BoneNames)
        {
            const FString BoneNameStr = BoneName.ToString();
            if (BoneFilter.Num() > 0)
            {
                bool bMatched = false;
                for (const FString& Sub : BoneFilter)
                {
                    if (BoneNameStr.Contains(Sub, ESearchCase::IgnoreCase)) { bMatched = true; break; }
                }
                if (!bMatched) { ++BonesSkipped; continue; }
            }

            if (!Model->IsValidBoneTrackName(BoneName)) { ++BonesSkipped; continue; }

            TArray<FTransform> Transforms;
            Model->GetBoneTrackTransforms(BoneName, Transforms);
            if (Transforms.Num() == 0) { ++BonesSkipped; continue; }

            TArray<FVector> PosKeys; PosKeys.Reserve(Transforms.Num());
            TArray<FQuat>   RotKeys; RotKeys.Reserve(Transforms.Num());
            TArray<FVector> ScaleKeys; ScaleKeys.Reserve(Transforms.Num());
            for (const FTransform& T : Transforms)
            {
                PosKeys.Add(T.GetLocation());
                RotKeys.Add(T.GetRotation());
                ScaleKeys.Add(T.GetScale3D());
            }

            const TArray<FQuat>   SmoothedRot = SmoothQuatCurve(RotKeys, Kernel);
            const TArray<FVector> SmoothedPos = bSmoothPositions
                ? SmoothVectorCurve(PosKeys, Kernel)
                : PosKeys;

            Controller.SetBoneTrackKeys(
                BoneName,
                SmoothedPos,
                SmoothedRot,
                ScaleKeys,
                /*bShouldTransact=*/false);

            ++BonesSmoothed;
            SmoothedBoneNames.Add(BoneNameStr);
        }

        Controller.CloseBracket(/*bShouldTransact=*/false);

        TargetSeq->MarkPackageDirty();
        if (!UEditorAssetLibrary::SaveAsset(TargetSeq->GetPathName(), /*bOnlyIfIsDirty=*/false))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Smoothed animation but failed to persist it to disk: %s"), *TargetSeq->GetPathName()),
                EMCPErrorCode::Internal,
                TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("anim_path"), AnimPath);
        ResultObj->SetStringField(TEXT("output_path"), OutputPath);
        ResultObj->SetNumberField(TEXT("window_size"), WindowSize);
        ResultObj->SetStringField(TEXT("filter_type"), FilterType.ToLower());
        if (FilterType.Equals(TEXT("gaussian"), ESearchCase::IgnoreCase))
        {
            const double EffectiveSigma = (SigmaFrames > 0.0) ? SigmaFrames : (double)WindowSize / 6.0;
            ResultObj->SetNumberField(TEXT("sigma_frames"), EffectiveSigma);
        }
        ResultObj->SetBoolField(TEXT("smooth_positions"), bSmoothPositions);
        ResultObj->SetNumberField(TEXT("bones_smoothed"), BonesSmoothed);
        ResultObj->SetNumberField(TEXT("bones_skipped"), BonesSkipped);
        {
            TArray<TSharedPtr<FJsonValue>> NamesJson;
            for (const FString& S : SmoothedBoneNames) { NamesJson.Add(MakeShared<FJsonValueString>(S)); }
            ResultObj->SetArrayField(TEXT("smoothed_bone_names"), NamesJson);
        }
        ResultObj->SetBoolField(TEXT("success"), true);
        return ResultObj;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // normalize_anim_z_offset — shift bone-track Z so frame 0 sits at target_z.
    //
    // Pipeline use-case: SMPL / text-to-motion outputs have per-anim
    // drift in the character's starting world Z (one anim "stands a few cm low",
    // another "floats a few cm high"). When retargeted to a fixed-skeleton target
    // like Manny, that drift becomes a baseline offset that compresses or
    // stretches the silhouette — arms look hunched, etc. This tool rebases each
    // anim's frame 0 to a canonical Z (default 0 = floor) and preserves all
    // relative vertical motion by applying the same delta to every frame.
    //
    // Params:
    //   anim_path             — UAnimSequence asset path.
    //   target_z              — float, default 0.0. Frame-0 Z value to land at.
    //   bone_substring_filter — array of substrings. Default ["root"]. Each bone
    //                           whose name contains ANY substring is normalized
    //                           independently (per-bone delta from its own
    //                           frame-0 Z).
    //   output_suffix         — default "_ZNorm". Empty → in-place mutation.
    // ─────────────────────────────────────────────────────────────────────────
    TSharedPtr<FJsonObject> NormalizeAnimZOffset_Impl(const TSharedPtr<FJsonObject>& Params)
    {
        FString AnimPath;
        if (!Params->TryGetStringField(TEXT("anim_path"), AnimPath))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'anim_path' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass the asset path of a UAnimSequence."));
        }

        double TargetZ = 0.0;
        Params->TryGetNumberField(TEXT("target_z"), TargetZ);

        FString OutputSuffix = TEXT("_ZNorm");
        Params->TryGetStringField(TEXT("output_suffix"), OutputSuffix);

        TArray<FString> BoneFilter;
        const TArray<TSharedPtr<FJsonValue>>* FilterArrayPtr = nullptr;
        if (Params->TryGetArrayField(TEXT("bone_substring_filter"), FilterArrayPtr) && FilterArrayPtr)
        {
            for (const TSharedPtr<FJsonValue>& V : *FilterArrayPtr)
            {
                if (V.IsValid() && V->Type == EJson::String)
                {
                    const FString S = V->AsString();
                    if (!S.IsEmpty()) BoneFilter.Add(S);
                }
            }
        }
        if (BoneFilter.Num() == 0) { BoneFilter.Add(TEXT("root")); }

        UObject* SrcObj = UEditorAssetLibrary::LoadAsset(AnimPath);
        UAnimSequence* SrcSeq = Cast<UAnimSequence>(SrcObj);
        if (!SrcSeq)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset is not a UAnimSequence: %s"), *AnimPath),
                SrcObj ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
                TEXT("Pass an animation sequence asset path."));
        }

        UAnimSequence* TargetSeq = SrcSeq;
        FString OutputPath = SrcSeq->GetPathName();

        if (!OutputSuffix.IsEmpty())
        {
            FString PackagePath = FPackageName::GetLongPackagePath(SrcSeq->GetPathName());
            FString NewName = SrcSeq->GetName() + OutputSuffix;
            FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
            UObject* DuplicatedObj = AssetToolsModule.Get().DuplicateAsset(NewName, PackagePath, SrcSeq);
            TargetSeq = Cast<UAnimSequence>(DuplicatedObj);
            if (!TargetSeq)
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Failed to duplicate '%s' to '%s/%s'"), *AnimPath, *PackagePath, *NewName),
                    EMCPErrorCode::EngineBusy,
                    TEXT("AssetTools::DuplicateAsset returned null. Verify the destination is writable."));
            }
            OutputPath = TargetSeq->GetPathName();
        }

        const IAnimationDataModel* Model = TargetSeq->GetDataModel();
        if (!Model)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("UAnimSequence has no IAnimationDataModel"),
                EMCPErrorCode::EngineBusy,
                TEXT("The animation asset is missing its data model — try resaving in the editor first."));
        }
        IAnimationDataController& Controller = TargetSeq->GetController();

        TArray<FName> BoneNames;
        Model->GetBoneTrackNames(BoneNames);

        Controller.OpenBracket(FText::FromString(TEXT("MCP normalize_anim_z_offset")), /*bShouldTransact=*/false);

        int32 BonesNormalized = 0;
        int32 BonesSkipped = 0;
        TArray<TSharedPtr<FJsonValue>> PerBoneDeltas;
        for (const FName& BoneName : BoneNames)
        {
            const FString BoneNameStr = BoneName.ToString();
            bool bMatched = false;
            for (const FString& Sub : BoneFilter)
            {
                if (BoneNameStr.Contains(Sub, ESearchCase::IgnoreCase)) { bMatched = true; break; }
            }
            if (!bMatched) { ++BonesSkipped; continue; }

            if (!Model->IsValidBoneTrackName(BoneName)) { ++BonesSkipped; continue; }

            TArray<FTransform> Transforms;
            Model->GetBoneTrackTransforms(BoneName, Transforms);
            if (Transforms.Num() == 0) { ++BonesSkipped; continue; }

            const double Frame0Z = Transforms[0].GetLocation().Z;
            const double Delta = Frame0Z - TargetZ;

            TArray<FVector> PosKeys; PosKeys.Reserve(Transforms.Num());
            TArray<FQuat>   RotKeys; RotKeys.Reserve(Transforms.Num());
            TArray<FVector> ScaleKeys; ScaleKeys.Reserve(Transforms.Num());
            for (const FTransform& T : Transforms)
            {
                FVector P = T.GetLocation();
                P.Z -= Delta;
                PosKeys.Add(P);
                RotKeys.Add(T.GetRotation());
                ScaleKeys.Add(T.GetScale3D());
            }

            Controller.SetBoneTrackKeys(
                BoneName,
                PosKeys,
                RotKeys,
                ScaleKeys,
                /*bShouldTransact=*/false);

            ++BonesNormalized;

            TSharedPtr<FJsonObject> BoneEntry = MakeShared<FJsonObject>();
            BoneEntry->SetStringField(TEXT("bone"), BoneNameStr);
            BoneEntry->SetNumberField(TEXT("frame0_z"), Frame0Z);
            BoneEntry->SetNumberField(TEXT("delta_applied"), Delta);
            PerBoneDeltas.Add(MakeShared<FJsonValueObject>(BoneEntry));
        }

        Controller.CloseBracket(/*bShouldTransact=*/false);

        TargetSeq->MarkPackageDirty();
        if (!UEditorAssetLibrary::SaveAsset(TargetSeq->GetPathName(), /*bOnlyIfIsDirty=*/false))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Normalized Z offset but failed to persist animation to disk: %s"), *TargetSeq->GetPathName()),
                EMCPErrorCode::Internal,
                TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("anim_path"), AnimPath);
        ResultObj->SetStringField(TEXT("output_path"), OutputPath);
        ResultObj->SetNumberField(TEXT("target_z"), TargetZ);
        ResultObj->SetNumberField(TEXT("bones_normalized"), BonesNormalized);
        ResultObj->SetNumberField(TEXT("bones_skipped"), BonesSkipped);
        ResultObj->SetArrayField(TEXT("per_bone_deltas"), PerBoneDeltas);
        ResultObj->SetBoolField(TEXT("success"), true);
        return ResultObj;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // anchor_feet_to_floor — FK-compose foot world Z over first N frames,
    // take the median, and shift the pelvis Z curve uniformly so the median
    // foot Z lands at target_z (floor).
    //
    // Why: SMPL→Manny retargeting with limb-length mismatch and per-anim
    // postural variation produces output where the same source frame 0 lands
    // Manny's feet at different world Zs depending on leg pose. Anchoring
    // foot world Z via FK from the actual target hierarchy accounts for
    // Manny's geometry (vs source pelvis Z which uses SMPL's geometry).
    //
    // Algorithm (bedlam2's median-across-frames pattern, adapted to UE bone
    // tracks + skeleton ref hierarchy):
    //   1. Walk parent chain foot → root via FReferenceSkeleton::GetParentIndex
    //   2. For each frame in [0, sample_frames), compose foot world transform
    //      by accumulating bone_world = bone_local * parent_world up the chain
    //      (bone_local from anim track if animated, else bind pose ref).
    //   3. Median of foot world Z over the sampled frames → robust floor ref.
    //   4. delta_z = median_foot_z − target_z; subtract from every frame's
    //      pelvis Z track. Uniform translation, preserves all relative motion.
    //
    // Params:
    //   anim_path                — UAnimSequence to anchor.
    //   foot_bone_substring      — bone name substring, default "foot_l".
    //   pelvis_bone_substring    — bone name substring to receive the offset,
    //                              default "pelvis".
    //   target_z                 — floor Z value, default 0.0.
    //   sample_frames            — how many leading frames to sample for the
    //                              median, default 10. Use 1 for frame-0-only.
    //   output_suffix            — default "_FootAnchored". Empty = in-place.
    // ─────────────────────────────────────────────────────────────────────────
    TSharedPtr<FJsonObject> AnchorFeetToFloor_Impl(const TSharedPtr<FJsonObject>& Params)
    {
        FString AnimPath;
        if (!Params->TryGetStringField(TEXT("anim_path"), AnimPath))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Missing 'anim_path' parameter"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass the asset path of a UAnimSequence."));
        }

        FString FootSub = TEXT("foot_l");
        Params->TryGetStringField(TEXT("foot_bone_substring"), FootSub);

        FString PelvisSub = TEXT("pelvis");
        Params->TryGetStringField(TEXT("pelvis_bone_substring"), PelvisSub);

        double TargetZ = 0.0;
        Params->TryGetNumberField(TEXT("target_z"), TargetZ);

        int32 SampleFrames = 10;
        Params->TryGetNumberField(TEXT("sample_frames"), SampleFrames);
        SampleFrames = FMath::Max(1, SampleFrames);

        FString OutputSuffix = TEXT("_FootAnchored");
        Params->TryGetStringField(TEXT("output_suffix"), OutputSuffix);

        UObject* SrcObj = UEditorAssetLibrary::LoadAsset(AnimPath);
        UAnimSequence* SrcSeq = Cast<UAnimSequence>(SrcObj);
        if (!SrcSeq)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Asset is not a UAnimSequence: %s"), *AnimPath),
                SrcObj ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
                TEXT("Pass an animation sequence asset path."));
        }

        UAnimSequence* TargetSeq = SrcSeq;
        FString OutputPath = SrcSeq->GetPathName();
        if (!OutputSuffix.IsEmpty())
        {
            FString PackagePath = FPackageName::GetLongPackagePath(SrcSeq->GetPathName());
            FString NewName = SrcSeq->GetName() + OutputSuffix;
            FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
            UObject* DuplicatedObj = AssetToolsModule.Get().DuplicateAsset(NewName, PackagePath, SrcSeq);
            TargetSeq = Cast<UAnimSequence>(DuplicatedObj);
            if (!TargetSeq)
            {
                return FMCPCommonUtils::CreateErrorResponse(
                    FString::Printf(TEXT("Failed to duplicate '%s' to '%s/%s'"), *AnimPath, *PackagePath, *NewName),
                    EMCPErrorCode::EngineBusy,
                    TEXT("AssetTools::DuplicateAsset returned null. Verify the destination is writable."));
            }
            OutputPath = TargetSeq->GetPathName();
        }

        USkeleton* Skel = TargetSeq->GetSkeleton();
        if (!Skel)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("UAnimSequence has no Skeleton"),
                EMCPErrorCode::EngineBusy,
                TEXT("Animation must reference a USkeleton asset."));
        }
        const FReferenceSkeleton& RefSkel = Skel->GetReferenceSkeleton();
        const TArray<FTransform>& BindPose = RefSkel.GetRefBonePose();

        const IAnimationDataModel* Model = TargetSeq->GetDataModel();
        if (!Model)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("UAnimSequence has no IAnimationDataModel"),
                EMCPErrorCode::EngineBusy,
                TEXT("Resave the animation in the editor first."));
        }
        IAnimationDataController& Controller = TargetSeq->GetController();

        // Find foot bone (first match) and pelvis bone (first match).
        const int32 NumBones = RefSkel.GetNum();
        int32 FootIdx = INDEX_NONE;
        int32 PelvisIdx = INDEX_NONE;
        FName FootName, PelvisName;
        for (int32 i = 0; i < NumBones; ++i)
        {
            const FName N = RefSkel.GetBoneName(i);
            const FString S = N.ToString();
            if (FootIdx == INDEX_NONE && S.Contains(FootSub, ESearchCase::IgnoreCase))
            {
                FootIdx = i; FootName = N;
            }
            if (PelvisIdx == INDEX_NONE && S.Contains(PelvisSub, ESearchCase::IgnoreCase))
            {
                PelvisIdx = i; PelvisName = N;
            }
            if (FootIdx != INDEX_NONE && PelvisIdx != INDEX_NONE) break;
        }
        if (FootIdx == INDEX_NONE)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("No bone matched foot_bone_substring='%s'"), *FootSub),
                EMCPErrorCode::InvalidArgument,
                TEXT("Specify a substring that matches a bone in the target skeleton (e.g. 'foot_l')."));
        }
        if (PelvisIdx == INDEX_NONE)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("No bone matched pelvis_bone_substring='%s'"), *PelvisSub),
                EMCPErrorCode::InvalidArgument,
                TEXT("Specify a substring matching the bone whose Z carries the offset."));
        }

        // Build chain root → ... → foot.
        TArray<int32> Chain;
        for (int32 Curr = FootIdx; Curr != INDEX_NONE; Curr = RefSkel.GetParentIndex(Curr))
        {
            Chain.Insert(Curr, 0);
        }

        // Cache per-bone-in-chain animated transforms (or nullptr if not animated).
        // We read once and reuse across frames to avoid repeated GetBoneTrackTransforms calls.
        struct FChainBoneCache
        {
            int32 BoneIndex;
            FName BoneName;
            bool bAnimated;
            TArray<FTransform> Transforms;
        };
        TArray<FChainBoneCache> ChainCache;
        ChainCache.Reserve(Chain.Num());
        for (int32 BoneIdx : Chain)
        {
            FChainBoneCache E;
            E.BoneIndex = BoneIdx;
            E.BoneName = RefSkel.GetBoneName(BoneIdx);
            E.bAnimated = Model->IsValidBoneTrackName(E.BoneName);
            if (E.bAnimated)
            {
                Model->GetBoneTrackTransforms(E.BoneName, E.Transforms);
                if (E.Transforms.Num() == 0) { E.bAnimated = false; }
            }
            ChainCache.Add(E);
        }

        const int32 NumKeys = Model->GetNumberOfKeys();
        if (NumKeys <= 0)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("Animation has zero keys"),
                EMCPErrorCode::InvalidArgument,
                TEXT("Animation sequence is empty."));
        }
        const int32 ActualSampleCount = FMath::Min(SampleFrames, NumKeys);

        // For each sampled frame, compose foot world Z via FK walk.
        TArray<double> FootZs;
        FootZs.Reserve(ActualSampleCount);
        for (int32 Frame = 0; Frame < ActualSampleCount; ++Frame)
        {
            FTransform World = FTransform::Identity;
            for (const FChainBoneCache& E : ChainCache)
            {
                FTransform Local;
                if (E.bAnimated)
                {
                    const int32 Idx = FMath::Clamp(Frame, 0, E.Transforms.Num() - 1);
                    Local = E.Transforms[Idx];
                }
                else
                {
                    Local = BindPose[E.BoneIndex];
                }
                World = Local * World;
            }
            FootZs.Add(World.GetLocation().Z);
        }

        // Median for robustness vs frame-0 outliers.
        TArray<double> Sorted = FootZs;
        Sorted.Sort();
        double MedianFootZ;
        if ((Sorted.Num() % 2) == 1)
        {
            MedianFootZ = Sorted[Sorted.Num() / 2];
        }
        else
        {
            const int32 Mid = Sorted.Num() / 2;
            MedianFootZ = 0.5 * (Sorted[Mid - 1] + Sorted[Mid]);
        }
        const double DeltaZ = MedianFootZ - TargetZ;

        // Apply uniform shift to pelvis position Z across all frames.
        if (!Model->IsValidBoneTrackName(PelvisName))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Pelvis bone '%s' has no animation track"), *PelvisName.ToString()),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pelvis must be animated for the Z shift to apply. Try a different pelvis_bone_substring."));
        }
        TArray<FTransform> PelvisXforms;
        Model->GetBoneTrackTransforms(PelvisName, PelvisXforms);
        TArray<FVector> PosKeys; PosKeys.Reserve(PelvisXforms.Num());
        TArray<FQuat>   RotKeys; RotKeys.Reserve(PelvisXforms.Num());
        TArray<FVector> ScaleKeys; ScaleKeys.Reserve(PelvisXforms.Num());
        for (const FTransform& T : PelvisXforms)
        {
            FVector P = T.GetLocation();
            P.Z -= DeltaZ;
            PosKeys.Add(P);
            RotKeys.Add(T.GetRotation());
            ScaleKeys.Add(T.GetScale3D());
        }

        Controller.OpenBracket(FText::FromString(TEXT("MCP anchor_feet_to_floor")), /*bShouldTransact=*/false);
        Controller.SetBoneTrackKeys(PelvisName, PosKeys, RotKeys, ScaleKeys, /*bShouldTransact=*/false);
        Controller.CloseBracket(/*bShouldTransact=*/false);

        TargetSeq->MarkPackageDirty();
        if (!UEditorAssetLibrary::SaveAsset(TargetSeq->GetPathName(), /*bOnlyIfIsDirty=*/false))
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Anchored feet to floor but failed to persist animation to disk: %s"), *TargetSeq->GetPathName()),
                EMCPErrorCode::Internal,
                TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
        }

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("anim_path"), AnimPath);
        ResultObj->SetStringField(TEXT("output_path"), OutputPath);
        ResultObj->SetStringField(TEXT("foot_bone"), FootName.ToString());
        ResultObj->SetStringField(TEXT("pelvis_bone"), PelvisName.ToString());
        ResultObj->SetNumberField(TEXT("target_z"), TargetZ);
        ResultObj->SetNumberField(TEXT("sample_frames_used"), ActualSampleCount);
        ResultObj->SetNumberField(TEXT("median_foot_z"), MedianFootZ);
        ResultObj->SetNumberField(TEXT("delta_applied"), DeltaZ);
        {
            TArray<TSharedPtr<FJsonValue>> ZsJson;
            for (double V : FootZs) { ZsJson.Add(MakeShared<FJsonValueNumber>(V)); }
            ResultObj->SetArrayField(TEXT("sampled_foot_zs"), ZsJson);
        }
        ResultObj->SetBoolField(TEXT("success"), true);
        return ResultObj;
    }
}

TSharedPtr<FJsonObject> FMCPIKCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("ik_rig_list_chains"))                          return HandleListIKRigChains(Params);
    if (CommandType == TEXT("ik_retarget_read"))                          return HandleReadIKRetargeter(Params);
    if (CommandType == TEXT("ik_retarget_create"))                        return HandleCreateIKRetargeter(Params);
    if (CommandType == TEXT("ik_retarget_set_rigs"))                      return HandleSetIKRetargeterRigs(Params);
    if (CommandType == TEXT("ik_retarget_auto_map_chains"))               return HandleIKRetargeterAutoMapChains(Params);
    if (CommandType == TEXT("ik_retarget_set_chain_mapping"))             return HandleSetIKRetargeterChainMapping(Params);
    if (CommandType == TEXT("ik_retarget_import_pose_from_pose_asset")) return ImportRetargetPoseFromPoseAsset_Impl(Params);
    if (CommandType == TEXT("ik_retarget_import_pose_from_animation")) return ImportRetargetPoseFromAnimation_Impl(Params);
    if (CommandType == TEXT("ik_retarget_align_bones"))                 return IKRetargeterAlignBones_Impl(Params);
    if (CommandType == TEXT("ik_retarget_run_batch"))                       return HandleIKRetargetRunBatch(Params);
    if (CommandType == TEXT("ik_retarget_set_pelvis_settings"))         return SetRetargeterPelvisSettings_Impl(Params);
    if (CommandType == TEXT("ik_retarget_set_root_motion_settings"))    return SetRetargeterRootMotionSettings_Impl(Params);
    if (CommandType == TEXT("anim_smooth_sequence"))                      return SmoothAnimSequence_Impl(Params);
    if (CommandType == TEXT("anim_normalize_z_offset"))                   return NormalizeAnimZOffset_Impl(Params);
    if (CommandType == TEXT("anim_anchor_feet_to_floor"))                      return AnchorFeetToFloor_Impl(Params);

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown IK command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Shipped IK commands: list_ik_rig_chains, read_ik_retargeter, create_ik_retargeter, set_ik_retargeter_rigs, ik_retargeter_auto_map_chains, set_ik_retargeter_chain_mapping, set_ik_retargeter_pelvis_settings, set_ik_retargeter_root_motion_settings, smooth_anim_sequence, normalize_anim_z_offset, anchor_feet_to_floor, ik_retarget_run_batch."));
}

// ─────────────────────────────────────────────────────────────────────────────
// list_ik_rig_chains — read chain definitions + pelvis from a UIKRigDefinition.
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPIKCommands::HandleListIKRigChains(const TSharedPtr<FJsonObject>& Params)
{
    FString IKRigPath;
    if (!Params->TryGetStringField(TEXT("ik_rig_path"), IKRigPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'ik_rig_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path of a UIKRigDefinition."));
    }

    UIKRigDefinition* Rig = LoadIKRig(IKRigPath);
    if (!Rig)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UIKRigDefinition: %s"), *IKRigPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the asset path resolves to an IK Rig."));
    }

    UIKRigController* RigCtrl = UIKRigController::GetController(Rig);
    if (!RigCtrl)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRigController::GetController returned null for: %s"), *IKRigPath),
            EMCPErrorCode::EngineBusy,
            TEXT("Reopen the IK Rig in the editor and retry."));
    }

    const TArray<FBoneChain>& Chains = RigCtrl->GetRetargetChains();
    TArray<TSharedPtr<FJsonValue>> ChainArr;
    for (const FBoneChain& Chain : Chains)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"),       Chain.ChainName.ToString());
        Obj->SetStringField(TEXT("start_bone"), Chain.StartBone.BoneName.ToString());
        Obj->SetStringField(TEXT("end_bone"),   Chain.EndBone.BoneName.ToString());
        Obj->SetStringField(TEXT("goal"),       Chain.IKGoalName.ToString());
        ChainArr.Add(MakeShared<FJsonValueObject>(Obj));
    }

    USkeletalMesh* PreviewMesh = RigCtrl->GetSkeletalMesh();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("ik_rig_path"), Rig->GetPathName());
    ResultObj->SetStringField(TEXT("pelvis_bone"), RigCtrl->GetRetargetRoot().ToString());
    ResultObj->SetStringField(TEXT("preview_mesh"), PreviewMesh ? PreviewMesh->GetPathName() : FString());
    ResultObj->SetArrayField(TEXT("chains"), ChainArr);
    ResultObj->SetNumberField(TEXT("chain_count"), ChainArr.Num());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ─────────────────────────────────────────────────────────────────────────────
// read_ik_retargeter — inspect source/target rigs and current chain mappings.
// Mappings come from the retargeter's FK op (the "chain mapping" the user sees
// in the editor). For each target chain on the target IK Rig, query
// Controller->GetSourceChain(TargetChainName) and report what's wired.
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPIKCommands::HandleReadIKRetargeter(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath;
    if (!Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'retargeter_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path of a UIKRetargeter."));
    }

    UIKRetargeter* Retargeter = LoadRetargeter(RetargeterPath);
    if (!Retargeter)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UIKRetargeter: %s"), *RetargeterPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the asset path resolves to an IK Retargeter."));
    }

    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRetargeterController::GetController returned null for: %s"), *RetargeterPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UE could not produce a controller for this retargeter."));
    }

    const UIKRigDefinition* SourceRig = Controller->GetIKRig(ERetargetSourceOrTarget::Source);
    const UIKRigDefinition* TargetRig = Controller->GetIKRig(ERetargetSourceOrTarget::Target);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("retargeter_path"), Retargeter->GetPathName());
    ResultObj->SetStringField(TEXT("source_ik_rig_path"), SourceRig ? SourceRig->GetPathName() : FString());
    ResultObj->SetStringField(TEXT("target_ik_rig_path"), TargetRig ? TargetRig->GetPathName() : FString());

    // Build chain mapping list keyed by target chain name.
    TArray<TSharedPtr<FJsonValue>> MappingArr;
    if (TargetRig)
    {
        UIKRigController* TargetRigCtrl = UIKRigController::GetController(const_cast<UIKRigDefinition*>(TargetRig));
        if (TargetRigCtrl)
        {
            const TArray<FBoneChain>& TargetChains = TargetRigCtrl->GetRetargetChains();
            for (const FBoneChain& TChain : TargetChains)
            {
                const FName SourceName = Controller->GetSourceChain(TChain.ChainName);
                TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
                M->SetStringField(TEXT("target_chain"), TChain.ChainName.ToString());
                M->SetStringField(TEXT("source_chain"), SourceName.IsNone() ? FString() : SourceName.ToString());
                MappingArr.Add(MakeShared<FJsonValueObject>(M));
            }
        }
    }
    ResultObj->SetArrayField(TEXT("chain_mappings"), MappingArr);
    ResultObj->SetNumberField(TEXT("chain_mapping_count"), MappingArr.Num());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ─────────────────────────────────────────────────────────────────────────────
// create_ik_retargeter — author a new UIKRetargeter asset.
// Optionally accepts source/target IK rig paths in the same call. The factory
// invokes Controller->AddDefaultOps() so the new asset ships with the standard
// FK + IK op stack the editor's "New IK Retargeter" menu produces.
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPIKCommands::HandleCreateIKRetargeter(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass {path:'/Game/Anim/Retargeters', name:'RTG_Foo'} or {asset_path:'/Game/Anim/Retargeters/RTG_Foo'}."));
    }
    if (UEditorAssetLibrary::DoesAssetExist(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    // Resolve source/target rigs upfront if provided — fail before any asset side-effects.
    FString SourceRigPath, TargetRigPath;
    Params->TryGetStringField(TEXT("source_ik_rig_path"), SourceRigPath);
    Params->TryGetStringField(TEXT("target_ik_rig_path"), TargetRigPath);

    UIKRigDefinition* SourceRig = nullptr;
    UIKRigDefinition* TargetRig = nullptr;
    if (!SourceRigPath.IsEmpty())
    {
        SourceRig = LoadIKRig(SourceRigPath);
        if (!SourceRig)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("source_ik_rig_path does not resolve to a UIKRigDefinition: %s"), *SourceRigPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("Pass an existing IK Rig asset path or omit the field."));
        }
    }
    if (!TargetRigPath.IsEmpty())
    {
        TargetRig = LoadIKRig(TargetRigPath);
        if (!TargetRig)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("target_ik_rig_path does not resolve to a UIKRigDefinition: %s"), *TargetRigPath),
                EMCPErrorCode::AssetNotFound,
                TEXT("Pass an existing IK Rig asset path or omit the field."));
        }
    }

    UIKRetargetFactory* Factory = NewObject<UIKRetargetFactory>();

    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    UObject* CreatedObj = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UIKRetargeter::StaticClass(), Factory);
    UIKRetargeter* Created = Cast<UIKRetargeter>(CreatedObj);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UAssetTools::CreateAsset returned null for the IK Retargeter"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log."));
    }

    // Wire source/target rigs if supplied, then rebuild the op stack so the
    // per-op chain mappings see the new rigs. SetIKRig alone does not propagate
    // the target rig to ops (see comment in UIKRetargeterController::SetIKRig).
    // Mirrors the editor's FProceduralRetargetAssets::AutoGenerateIKRetargetAsset
    // recipe: SetIKRig source + target, RemoveAllOps, AddDefaultOps. Skipping
    // this leaves ops with stale (null) target rigs and AutoMapChains is a no-op.
    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Created);
    if (Controller && (SourceRig || TargetRig))
    {
        Created->PreEditChange(nullptr);
        if (SourceRig) Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
        if (TargetRig) Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
        Controller->RemoveAllOps();
        Controller->AddDefaultOps();
        Created->PostEditChange();
    }

    Created->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Created IK retargeter in memory but failed to persist it to disk: %s"), *FullAssetPath),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), Created->GetPathName());
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/IKRig.IKRetargeter"));
    if (SourceRig) ResultObj->SetStringField(TEXT("source_ik_rig_path"), SourceRig->GetPathName());
    if (TargetRig) ResultObj->SetStringField(TEXT("target_ik_rig_path"), TargetRig->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ─────────────────────────────────────────────────────────────────────────────
// set_ik_retargeter_rigs — set source and/or target IK rigs on an existing
// retargeter. Either field is optional; passing neither is a no-op error.
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPIKCommands::HandleSetIKRetargeterRigs(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath;
    if (!Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'retargeter_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path of a UIKRetargeter."));
    }

    FString SourceRigPath, TargetRigPath;
    const bool bHasSource = Params->TryGetStringField(TEXT("source_ik_rig_path"), SourceRigPath) && !SourceRigPath.IsEmpty();
    const bool bHasTarget = Params->TryGetStringField(TEXT("target_ik_rig_path"), TargetRigPath) && !TargetRigPath.IsEmpty();
    if (!bHasSource && !bHasTarget)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Pass at least one of 'source_ik_rig_path' or 'target_ik_rig_path'"),
            EMCPErrorCode::InvalidArgument,
            TEXT("This handler is a no-op without a rig to set. Use read_ik_retargeter to inspect current wiring."));
    }

    UIKRetargeter* Retargeter = LoadRetargeter(RetargeterPath);
    if (!Retargeter)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UIKRetargeter: %s"), *RetargeterPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the asset path resolves to an IK Retargeter."));
    }
    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRetargeterController::GetController returned null for: %s"), *RetargeterPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UE could not produce a controller for this retargeter."));
    }

    UIKRigDefinition* SourceRig = bHasSource ? LoadIKRig(SourceRigPath) : nullptr;
    UIKRigDefinition* TargetRig = bHasTarget ? LoadIKRig(TargetRigPath) : nullptr;
    if (bHasSource && !SourceRig)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("source_ik_rig_path does not resolve to a UIKRigDefinition: %s"), *SourceRigPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("Pass an existing IK Rig asset path."));
    }
    if (bHasTarget && !TargetRig)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("target_ik_rig_path does not resolve to a UIKRigDefinition: %s"), *TargetRigPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("Pass an existing IK Rig asset path."));
    }

    // rebuild_ops=true (default) wipes the op stack + adds defaults so per-op
    // chain mappings re-anchor to the new rigs. Pass rebuild_ops=false when
    // modifying a vendor-supplied retargeter whose existing op stack (and
    // baked-in retarget pose) you want to preserve — e.g. swapping just the
    // target rig on an existing SMPL-to-MetaHumans retargeter.
    bool bRebuildOps = true;
    Params->TryGetBoolField(TEXT("rebuild_ops"), bRebuildOps);

    Retargeter->PreEditChange(nullptr);
    if (SourceRig) Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
    if (TargetRig) Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
    if (bRebuildOps)
    {
        Controller->RemoveAllOps();
        Controller->AddDefaultOps();
    }
    Retargeter->PostEditChange();
    Retargeter->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(Retargeter->GetPathName(), /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Set retargeter rigs but failed to persist retargeter to disk: %s"), *Retargeter->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("retargeter_path"), Retargeter->GetPathName());
    if (SourceRig) ResultObj->SetStringField(TEXT("source_ik_rig_path"), SourceRig->GetPathName());
    if (TargetRig) ResultObj->SetStringField(TEXT("target_ik_rig_path"), TargetRig->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ─────────────────────────────────────────────────────────────────────────────
// ik_retargeter_auto_map_chains — bulk name-based chain mapping.
// match_type: "Exact" (case-insensitive exact match), "Fuzzy" (Levenshtein
// closest), or "Clear" (zero out all mappings). force_remap=true overwrites
// existing wires; false only fills unmapped chains.
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPIKCommands::HandleIKRetargeterAutoMapChains(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath;
    if (!Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'retargeter_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path of a UIKRetargeter."));
    }

    FString MatchTypeStr;
    Params->TryGetStringField(TEXT("match_type"), MatchTypeStr);
    EAutoMapChainType MatchType;
    FString MatchError;
    if (!ParseAutoMapType(MatchTypeStr, MatchType, MatchError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            MatchError, EMCPErrorCode::InvalidArgument,
            TEXT("Use 'Exact' for case-insensitive identical names, 'Fuzzy' for closest match, 'Clear' to reset."));
    }

    bool bForceRemap = true; // default: overwrite, matching the editor's "Map All (Fuzzy/Exact)" buttons
    Params->TryGetBoolField(TEXT("force_remap"), bForceRemap);

    UIKRetargeter* Retargeter = LoadRetargeter(RetargeterPath);
    if (!Retargeter)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UIKRetargeter: %s"), *RetargeterPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the asset path resolves to an IK Retargeter."));
    }
    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRetargeterController::GetController returned null for: %s"), *RetargeterPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UE could not produce a controller for this retargeter."));
    }
    if (!Controller->GetIKRig(ERetargetSourceOrTarget::Source) || !Controller->GetIKRig(ERetargetSourceOrTarget::Target))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Retargeter must have both source and target IK rigs set before auto-mapping."),
            EMCPErrorCode::AssetLocked,
            TEXT("Call set_ik_retargeter_rigs (or create_ik_retargeter) with both rigs first."));
    }

    // Default to also running the editor's reset-target-pose + auto-align-bones step.
    // Without this, the target rig holds its bind pose as the "retarget pose" but the
    // source rig's retarget pose is whatever the SMPL bind pose looks like — bone deltas
    // get applied between mismatched reference frames and the output looks (a) crouched,
    // (b) shoulders / arms rotated wrong. Opt out with align_target_pose=false if you
    // want to author the pose yourself.
    bool bAlignTargetPose = true;
    Params->TryGetBoolField(TEXT("align_target_pose"), bAlignTargetPose);

    Retargeter->PreEditChange(nullptr);
    Controller->AutoMapChains(MatchType, bForceRemap, NAME_None);
    if (bAlignTargetPose && MatchType != EAutoMapChainType::Clear)
    {
        // Mirrors FProceduralRetargetAssets::AutoGenerateIKRetargetAsset (SRetargetAnimAssetsWindow.cpp):
        //   ResetRetargetPose(currentTargetPose, /*all bones*/ {}, Target)
        //   AutoAlignAllBones(Target, ChainToChain)
        // Source-side is intentionally NOT auto-aligned here — for novel-skeleton sources
        // (SMPL, mocap exports) the right call is to import a vendor-supplied UPoseAsset
        // via import_ik_retargeter_pose_from_pose_asset, then re-align target against that
        // imported source pose via ik_retargeter_align_bones.
        const FName CurrentTargetPose = Controller->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target);
        Controller->ResetRetargetPose(CurrentTargetPose, TArray<FName>(), ERetargetSourceOrTarget::Target);
        Controller->AutoAlignAllBones(ERetargetSourceOrTarget::Target, ERetargetAutoAlignMethod::ChainToChain);
    }
    Retargeter->PostEditChange();
    Retargeter->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(Retargeter->GetPathName(), /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Auto-mapped chains but failed to persist retargeter to disk: %s"), *Retargeter->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
    }

    // Read back mappings so the caller sees the resulting wiring without a second round-trip.
    const UIKRigDefinition* TargetRig = Controller->GetIKRig(ERetargetSourceOrTarget::Target);
    UIKRigController* TargetRigCtrl = TargetRig ? UIKRigController::GetController(const_cast<UIKRigDefinition*>(TargetRig)) : nullptr;
    TArray<TSharedPtr<FJsonValue>> MappingArr;
    int32 MappedCount = 0;
    if (TargetRigCtrl)
    {
        const TArray<FBoneChain>& TargetChains = TargetRigCtrl->GetRetargetChains();
        for (const FBoneChain& TChain : TargetChains)
        {
            const FName SourceName = Controller->GetSourceChain(TChain.ChainName);
            TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
            M->SetStringField(TEXT("target_chain"), TChain.ChainName.ToString());
            M->SetStringField(TEXT("source_chain"), SourceName.IsNone() ? FString() : SourceName.ToString());
            MappingArr.Add(MakeShared<FJsonValueObject>(M));
            if (!SourceName.IsNone()) ++MappedCount;
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("retargeter_path"), Retargeter->GetPathName());
    ResultObj->SetStringField(TEXT("match_type"), MatchTypeStr.IsEmpty() ? TEXT("Fuzzy") : MatchTypeStr);
    ResultObj->SetBoolField(TEXT("force_remap"), bForceRemap);
    ResultObj->SetArrayField(TEXT("chain_mappings"), MappingArr);
    ResultObj->SetNumberField(TEXT("chain_mapping_count"), MappingArr.Num());
    ResultObj->SetNumberField(TEXT("mapped_count"), MappedCount);
    ResultObj->SetNumberField(TEXT("unmapped_count"), MappingArr.Num() - MappedCount);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ─────────────────────────────────────────────────────────────────────────────
// set_ik_retargeter_chain_mapping — manually wire one source chain to one target.
// Pass source_chain = "" to clear the mapping (sets None).
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPIKCommands::HandleSetIKRetargeterChainMapping(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath, TargetChain, SourceChain;
    if (!Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'retargeter_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path of a UIKRetargeter."));
    }
    if (!Params->TryGetStringField(TEXT("target_chain"), TargetChain) || TargetChain.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'target_chain' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the name of a target IK rig chain (see list_ik_rig_chains)."));
    }
    Params->TryGetStringField(TEXT("source_chain"), SourceChain); // empty = clear

    UIKRetargeter* Retargeter = LoadRetargeter(RetargeterPath);
    if (!Retargeter)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UIKRetargeter: %s"), *RetargeterPath),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the asset path resolves to an IK Retargeter."));
    }
    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRetargeterController::GetController returned null for: %s"), *RetargeterPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UE could not produce a controller for this retargeter."));
    }

    const FName SourceName = SourceChain.IsEmpty() ? NAME_None : FName(*SourceChain);
    const FName TargetName = FName(*TargetChain);

    Retargeter->PreEditChange(nullptr);
    const bool bApplied = Controller->SetSourceChain(SourceName, TargetName, NAME_None);
    Retargeter->PostEditChange();
    Retargeter->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(Retargeter->GetPathName(), /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Set chain mapping but failed to persist retargeter to disk: %s"), *Retargeter->GetPathName()),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — PIE is likely active or the package is read-only. Stop PIE and retry."));
    }

    if (!bApplied)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRetargeterController::SetSourceChain returned false for target='%s' source='%s'"),
                            *TargetChain, *SourceChain),
            EMCPErrorCode::InvalidArgument,
            TEXT("Verify both names exist as chains on their respective IK rigs (list_ik_rig_chains)."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("retargeter_path"), Retargeter->GetPathName());
    ResultObj->SetStringField(TEXT("target_chain"), TargetChain);
    ResultObj->SetStringField(TEXT("source_chain"), SourceChain);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

// ─────────────────────────────────────────────────────────────────────────────
// ik_retarget_run_batch (existing) — duplicate-and-retarget a list of anim assets.
// Unchanged from the prior surface; retained verbatim.
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPIKCommands::HandleIKRetargetRunBatch(const TSharedPtr<FJsonObject>& Params)
{
    FString RetargeterPath;
    if (!Params->TryGetStringField(TEXT("retargeter_path"), RetargeterPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'retargeter_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path of a UIKRetargeter (build one via create_ik_retargeter or in the editor)."));
    }

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(RetargeterPath);
    UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Loaded);
    if (!Retargeter)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UIKRetargeter: %s"), *RetargeterPath),
            Loaded ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
            TEXT("Build a UIKRetargeter via create_ik_retargeter and pass its asset path."));
    }

    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
    if (!Controller)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("UIKRetargeterController::GetController returned null for: %s"), *RetargeterPath),
            EMCPErrorCode::EngineBusy,
            TEXT("UE could not produce a controller for this retargeter."));
    }

    // Resolve source/target meshes from the retargeter's IK rigs.
    const UIKRigDefinition* SourceRig = Controller->GetIKRig(ERetargetSourceOrTarget::Source);
    const UIKRigDefinition* TargetRig = Controller->GetIKRig(ERetargetSourceOrTarget::Target);
    if (!SourceRig || !TargetRig)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Retargeter has no source and/or target IK rig configured"),
            EMCPErrorCode::AssetLocked,
            TEXT("Call set_ik_retargeter_rigs to wire source/target IK Rigs, then retry."));
    }

    UIKRigController* SourceRigCtrl = UIKRigController::GetController(const_cast<UIKRigDefinition*>(SourceRig));
    UIKRigController* TargetRigCtrl = UIKRigController::GetController(const_cast<UIKRigDefinition*>(TargetRig));
    if (!SourceRigCtrl || !TargetRigCtrl)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UIKRigController::GetController returned null for source or target rig"),
            EMCPErrorCode::EngineBusy,
            TEXT("Reopen the retargeter editor and retry."));
    }
    USkeletalMesh* SourceMesh = SourceRigCtrl->GetSkeletalMesh();
    USkeletalMesh* TargetMesh = TargetRigCtrl->GetSkeletalMesh();
    if (!SourceMesh || !TargetMesh)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Source or target skeletal mesh not assigned on the IK rigs"),
            EMCPErrorCode::AssetLocked,
            TEXT("Each IK rig referenced by the retargeter must have a USkeletalMesh assigned. Open the rig editor and assign one."));
    }

    const TArray<TSharedPtr<FJsonValue>>* AnimsArr;
    if (!Params->TryGetArrayField(TEXT("source_animations"), AnimsArr) || AnimsArr->Num() == 0)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing or empty 'source_animations' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass an array of asset paths — UAnimSequence / UBlendSpace / UAnimMontage."));
    }

    TArray<FAssetData> SourceAssets;
    TArray<FString> MissingPaths;
    for (const TSharedPtr<FJsonValue>& V : *AnimsArr)
    {
        if (!V.IsValid() || V->Type != EJson::String) continue;
        const FString Path = V->AsString();
        if (UObject* Anim = UEditorAssetLibrary::LoadAsset(Path))
        {
            SourceAssets.Add(FAssetData(Anim));
        }
        else
        {
            MissingPaths.Add(Path);
        }
    }
    if (MissingPaths.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> MissingArr;
        for (const FString& P : MissingPaths) MissingArr.Add(MakeShared<FJsonValueString>(P));
        TSharedPtr<FJsonObject> Err = FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not load %d source animation asset(s)"), MissingPaths.Num()),
            EMCPErrorCode::AssetNotFound,
            TEXT("Verify the paths via list_assets. All inputs must resolve before the batch starts."));
        Err->SetArrayField(TEXT("missing_animations"), MissingArr);
        return Err;
    }
    if (SourceAssets.Num() == 0)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("No valid source animations resolved"),
            EMCPErrorCode::InvalidArgument,
            TEXT("source_animations must contain at least one valid asset path."));
    }

    FString Search, Replace, Prefix, Suffix;
    Params->TryGetStringField(TEXT("name_search"),  Search);
    Params->TryGetStringField(TEXT("name_replace"), Replace);
    Params->TryGetStringField(TEXT("name_prefix"),  Prefix);
    Params->TryGetStringField(TEXT("name_suffix"),  Suffix);

    bool bIncludeReferenced = true;
    Params->TryGetBoolField(TEXT("include_referenced_assets"), bIncludeReferenced);

    bool bOverwriteExisting = false;
    Params->TryGetBoolField(TEXT("overwrite_existing"), bOverwriteExisting);

    const TArray<FAssetData> NewAssets = UIKRetargetBatchOperation::DuplicateAndRetarget(
        SourceAssets,
        SourceMesh,
        TargetMesh,
        Retargeter,
        Search, Replace, Prefix, Suffix,
        bIncludeReferenced,
        bOverwriteExisting);

    TArray<TSharedPtr<FJsonValue>> NewPathsArr;
    for (const FAssetData& AD : NewAssets)
    {
        NewPathsArr.Add(MakeShared<FJsonValueString>(AD.GetObjectPathString()));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("retargeter_path"), Retargeter->GetPathName());
    ResultObj->SetStringField(TEXT("source_mesh"), SourceMesh->GetPathName());
    ResultObj->SetStringField(TEXT("target_mesh"), TargetMesh->GetPathName());
    ResultObj->SetNumberField(TEXT("source_count"), SourceAssets.Num());
    ResultObj->SetArrayField(TEXT("new_assets"), NewPathsArr);
    ResultObj->SetNumberField(TEXT("new_assets_count"), NewPathsArr.Num());
    ResultObj->SetBoolField(TEXT("include_referenced_assets"), bIncludeReferenced);
    ResultObj->SetBoolField(TEXT("overwrite_existing"), bOverwriteExisting);
    if (!Search.IsEmpty())  ResultObj->SetStringField(TEXT("name_search"),  Search);
    if (!Replace.IsEmpty()) ResultObj->SetStringField(TEXT("name_replace"), Replace);
    if (!Prefix.IsEmpty())  ResultObj->SetStringField(TEXT("name_prefix"),  Prefix);
    if (!Suffix.IsEmpty())  ResultObj->SetStringField(TEXT("name_suffix"),  Suffix);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
