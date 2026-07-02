// Read-only foliage inspection (GAPS #14). No mutation — scatter/remove belong to
// the editor's Foliage mode. See MCPFoliageCommands.h.

#include "Commands/MCPFoliageCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"

#include "InstancedFoliageActor.h"
#include "InstancedFoliage.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"

namespace
{
	UWorld* GetFoliageEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	const TCHAR* ImplTypeLabel(EFoliageImplType T)
	{
		switch (T)
		{
		case EFoliageImplType::StaticMesh: return TEXT("StaticMesh");
		case EFoliageImplType::Actor:      return TEXT("Actor");
		case EFoliageImplType::ISMActor:   return TEXT("ISMActor");
		default:                           return TEXT("Unknown");
		}
	}

	// Stable identity to aggregate the same foliage type across multiple IFAs
	// (each level/cell may hold a local copy sharing a source mesh).
	FString FoliageTypeIdentity(UFoliageType* Type)
	{
		if (UFoliageType_InstancedStaticMesh* ISM = Cast<UFoliageType_InstancedStaticMesh>(Type))
		{
			if (UStaticMesh* M = ISM->GetStaticMesh()) return M->GetPathName();
		}
#if WITH_EDITOR
		return Type->GetDisplayFName().ToString();
#else
		return Type->GetName();
#endif
	}

	FString FoliageTypeMeshPath(UFoliageType* Type)
	{
		if (UFoliageType_InstancedStaticMesh* ISM = Cast<UFoliageType_InstancedStaticMesh>(Type))
		{
			if (UStaticMesh* M = ISM->GetStaticMesh()) return M->GetPathName();
		}
		return FString();
	}

	TSharedPtr<FJsonObject> Vec3Json(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}
}

TSharedPtr<FJsonObject> FMCPFoliageCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("foliage_inspect")) return HandleInspect(Params);

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown foliage command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be: foliage_inspect."));
}

TSharedPtr<FJsonObject> FMCPFoliageCommands::HandleInspect(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_EDITOR
	return FMCPCommonUtils::CreateErrorResponse(
		TEXT("Foliage inspection requires the editor"), EMCPErrorCode::Internal,
		TEXT("foliage_inspect reads editor-only foliage instance data; unavailable in this build configuration."));
#else
	UWorld* World = GetFoliageEditorWorld();
	if (!World)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No editor world"), EMCPErrorCode::Internal,
			TEXT("GEditor world is unavailable — retry once the editor has finished loading a map."));
	}

	FString Mode = TEXT("types");
	if (Params.IsValid()) Params->TryGetStringField(TEXT("mode"), Mode);

	if (Mode == TEXT("types"))
	{
		// Aggregate per-type across every loaded IFA.
		struct FTypeAgg
		{
			UFoliageType* Type = nullptr;
			int32 InstanceCount = 0;
		};
		TMap<FString, FTypeAgg> Agg;
		int32 IFACount = 0;

		for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
		{
			AInstancedFoliageActor* IFA = *It;
			if (!IFA) continue;
			++IFACount;
			for (const TPair<UFoliageType*, TUniqueObj<FFoliageInfo>>& Pair : IFA->GetFoliageInfos())
			{
				UFoliageType* Type = Pair.Key;
				if (!Type) continue;
				const FFoliageInfo& Info = *Pair.Value;
				FTypeAgg& A = Agg.FindOrAdd(FoliageTypeIdentity(Type));
				if (!A.Type) A.Type = Type;
				A.InstanceCount += Info.GetPlacedInstanceCount();
			}
		}

		TArray<TSharedPtr<FJsonValue>> TypesArr;
		int64 TotalInstances = 0;
		for (const TPair<FString, FTypeAgg>& Pair : Agg)
		{
			UFoliageType* Type = Pair.Value.Type;
			TotalInstances += Pair.Value.InstanceCount;

			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("identity"), Pair.Key);
			E->SetStringField(TEXT("display_name"), Type->GetDisplayFName().ToString());
			E->SetStringField(TEXT("mesh"), FoliageTypeMeshPath(Type));
			E->SetNumberField(TEXT("instance_count"), Pair.Value.InstanceCount);
			E->SetNumberField(TEXT("density"), Type->Density);
			E->SetNumberField(TEXT("radius"), Type->Radius);
			E->SetBoolField(TEXT("align_to_normal"), Type->AlignToNormal != 0);
			E->SetBoolField(TEXT("random_yaw"), Type->RandomYaw != 0);

			TSharedPtr<FJsonObject> Cull = MakeShared<FJsonObject>();
			Cull->SetNumberField(TEXT("min"), Type->CullDistance.Min);
			Cull->SetNumberField(TEXT("max"), Type->CullDistance.Max);
			E->SetObjectField(TEXT("cull_distance"), Cull);

			TypesArr.Add(MakeShared<FJsonValueObject>(E));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("mode"), TEXT("types"));
		Result->SetNumberField(TEXT("ifa_count"), IFACount);
		Result->SetNumberField(TEXT("total_types"), TypesArr.Num());
		Result->SetNumberField(TEXT("total_instances"), TotalInstances);
		Result->SetArrayField(TEXT("types"), TypesArr);
		return Result;
	}

	if (Mode == TEXT("instances"))
	{
		FString TargetType;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("foliage_type"), TargetType) || TargetType.IsEmpty())
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("mode='instances' requires `foliage_type`"), EMCPErrorCode::InvalidArgument,
				TEXT("Pass `foliage_type` (an identity / mesh path / display name from mode='types')."));
		}

		int32 Offset = 0, Limit = 100;
		Params->TryGetNumberField(TEXT("offset"), Offset);
		Params->TryGetNumberField(TEXT("limit"), Limit);
		Offset = FMath::Max(0, Offset);
		Limit = FMath::Clamp(Limit, 1, 1000);

		// Collect matching FFoliageInfo across all IFAs, in a stable order.
		TArray<const FFoliageInfo*> MatchingInfos;
		int64 TotalInstances = 0;
		for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
		{
			AInstancedFoliageActor* IFA = *It;
			if (!IFA) continue;
			for (const TPair<UFoliageType*, TUniqueObj<FFoliageInfo>>& Pair : IFA->GetFoliageInfos())
			{
				UFoliageType* Type = Pair.Key;
				if (!Type) continue;
				const bool bMatch =
					FoliageTypeIdentity(Type) == TargetType ||
					FoliageTypeMeshPath(Type) == TargetType ||
					Type->GetDisplayFName().ToString() == TargetType;
				if (bMatch)
				{
					const FFoliageInfo& Info = *Pair.Value;
					MatchingInfos.Add(&Info);
					TotalInstances += Info.Instances.Num();
				}
			}
		}

		if (MatchingInfos.Num() == 0)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("No foliage type matched: %s"), *TargetType),
				EMCPErrorCode::AssetNotFound,
				TEXT("`foliage_type` matched no placed foliage. Use mode='types' to list valid identities."));
		}

		TArray<TSharedPtr<FJsonValue>> InstArr;
		int64 GlobalIndex = 0;
		int32 Returned = 0;
		for (const FFoliageInfo* Info : MatchingInfos)
		{
			for (const FFoliageInstance& Inst : Info->Instances)
			{
				if (GlobalIndex >= Offset && Returned < Limit)
				{
					const FTransform T = Inst.GetInstanceWorldTransform();
					TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
					E->SetNumberField(TEXT("index"), GlobalIndex);
					E->SetObjectField(TEXT("location"), Vec3Json(T.GetTranslation()));
					TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
					Rot->SetNumberField(TEXT("pitch"), T.Rotator().Pitch);
					Rot->SetNumberField(TEXT("yaw"), T.Rotator().Yaw);
					Rot->SetNumberField(TEXT("roll"), T.Rotator().Roll);
					E->SetObjectField(TEXT("rotation"), Rot);
					E->SetObjectField(TEXT("scale"), Vec3Json(T.GetScale3D()));
					InstArr.Add(MakeShared<FJsonValueObject>(E));
					++Returned;
				}
				++GlobalIndex;
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("mode"), TEXT("instances"));
		Result->SetStringField(TEXT("foliage_type"), TargetType);
		Result->SetNumberField(TEXT("total_instances"), TotalInstances);
		Result->SetNumberField(TEXT("offset"), Offset);
		Result->SetNumberField(TEXT("limit"), Limit);
		Result->SetNumberField(TEXT("returned"), Returned);
		Result->SetBoolField(TEXT("truncated"), (int64)Offset + Returned < TotalInstances);
		Result->SetArrayField(TEXT("instances"), InstArr);
		return Result;
	}

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown mode: %s"), *Mode),
		EMCPErrorCode::InvalidArgument,
		TEXT("`mode` must be 'types' (default) or 'instances'."));
#endif // WITH_EDITOR
}
