#include "Commands/MaterialGraph/MaterialConnector.h"
#include "Commands/MaterialGraph/MaterialExpressionManager.h"
#include "Commands/MCPMaterialCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"

// ---------------------------------------------------------------------------
// Material-level input lookup (UE5.5 — on UMaterialEditorOnlyData)
// ---------------------------------------------------------------------------

static FExpressionInput* FindMaterialInput(UMaterial* Material, const FString& InputName)
{
	UMaterialEditorOnlyData* Ed = Material->GetEditorOnlyData();
	if (!Ed) return nullptr;

	if (InputName.Equals(TEXT("BaseColor"),           ESearchCase::IgnoreCase)) return &Ed->BaseColor;
	if (InputName.Equals(TEXT("Metallic"),            ESearchCase::IgnoreCase)) return &Ed->Metallic;
	if (InputName.Equals(TEXT("Specular"),            ESearchCase::IgnoreCase)) return &Ed->Specular;
	if (InputName.Equals(TEXT("Roughness"),           ESearchCase::IgnoreCase)) return &Ed->Roughness;
	if (InputName.Equals(TEXT("Anisotropy"),          ESearchCase::IgnoreCase)) return &Ed->Anisotropy;
	if (InputName.Equals(TEXT("Normal"),              ESearchCase::IgnoreCase)) return &Ed->Normal;
	if (InputName.Equals(TEXT("Tangent"),             ESearchCase::IgnoreCase)) return &Ed->Tangent;
	if (InputName.Equals(TEXT("EmissiveColor"),       ESearchCase::IgnoreCase)) return &Ed->EmissiveColor;
	if (InputName.Equals(TEXT("Opacity"),             ESearchCase::IgnoreCase)) return &Ed->Opacity;
	if (InputName.Equals(TEXT("OpacityMask"),         ESearchCase::IgnoreCase)) return &Ed->OpacityMask;
	if (InputName.Equals(TEXT("WorldPositionOffset"), ESearchCase::IgnoreCase)) return &Ed->WorldPositionOffset;
	if (InputName.Equals(TEXT("SubsurfaceColor"),     ESearchCase::IgnoreCase)) return &Ed->SubsurfaceColor;
	if (InputName.Equals(TEXT("AmbientOcclusion"),    ESearchCase::IgnoreCase)) return &Ed->AmbientOcclusion;
	if (InputName.Equals(TEXT("Refraction"),          ESearchCase::IgnoreCase)) return &Ed->Refraction;
	if (InputName.Equals(TEXT("PixelDepthOffset"),    ESearchCase::IgnoreCase)) return &Ed->PixelDepthOffset;

	return nullptr;
}

// ---------------------------------------------------------------------------
// Find an input on an expression by name or index
// ---------------------------------------------------------------------------

static FExpressionInput* FindExpressionInput(
	UMaterialExpression* Expr, const FString& InputId)
{
	// Numeric → use as index
	if (InputId.IsNumeric())
	{
		int32 Idx = FCString::Atoi(*InputId);
		// GetInput only guards the upper bound (Idx < CachedInputs.Num()); a
		// negative Idx (e.g. target_input "-1") would index CachedInputs[-1] and
		// assert-crash, so route it to the not-found path here.
		return Idx >= 0 ? Expr->GetInput(Idx) : nullptr;
	}

	// String → match by input name
	for (int32 i = 0; ; ++i)
	{
		FExpressionInput* In = Expr->GetInput(i);
		if (!In) break;
		FName InName = Expr->GetInputName(i);
		if (InName.ToString().Equals(InputId, ESearchCase::IgnoreCase))
		{
			return In;
		}
	}

	// LandscapeLayerBlend special case — layer inputs are named after layers
	if (auto* LLB = Cast<UMaterialExpressionLandscapeLayerBlend>(Expr))
	{
		for (int32 i = 0; i < LLB->Layers.Num(); ++i)
		{
			if (LLB->Layers[i].LayerName.ToString().Equals(InputId, ESearchCase::IgnoreCase))
			{
				return &LLB->Layers[i].LayerInput;
			}
			// Also allow "LayerName_Height" for height inputs
			FString HeightName = LLB->Layers[i].LayerName.ToString() + TEXT("_Height");
			if (HeightName.Equals(InputId, ESearchCase::IgnoreCase))
			{
				return &LLB->Layers[i].HeightInput;
			}
		}
	}

	return nullptr;
}

// ---------------------------------------------------------------------------
// ConnectExpressions
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMaterialConnector::ConnectExpressions(
	const TSharedPtr<FJsonObject>& Params)
{
	// Required params
	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'material_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`material_path` is required and must be the full asset path to a UMaterial, e.g. `/Game/Materials/M_Foo`. Use `list_assets` with `asset_type='Material'` to discover."));

	FString SourceExprId;
	if (!Params->TryGetStringField(TEXT("source_expression"), SourceExprId))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'source_expression'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`source_expression` is required. Pass either the expression's UObject name (`MaterialExpressionConstant3Vector_0`) or a friendly stable id. Use `read_material_graph` to enumerate expressions on the material."));

	FString TargetExprId;
	if (!Params->TryGetStringField(TEXT("target_expression"), TargetExprId))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target_expression'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`target_expression` is required. Pass either the expression's UObject name, or the literal string `Material` to wire the source into the material's attribute inputs (BaseColor, Metallic, Roughness, Normal, EmissiveColor, etc.)."));

	FString TargetInput;
	if (!Params->TryGetStringField(TEXT("target_input"), TargetInput))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'target_input'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`target_input` is required. When `target_expression='Material'`, valid values are: BaseColor, Metallic, Specular, Roughness, Anisotropy, Normal, Tangent, EmissiveColor, Opacity, OpacityMask, WorldPositionOffset, SubsurfaceColor, AmbientOcclusion, Refraction, PixelDepthOffset. Otherwise, pass the input name or numeric index — use `read_material_graph` to enumerate inputs."));

	int32 SourceOutputIndex = 0;
	if (Params->HasField(TEXT("source_output_index")))
		SourceOutputIndex = static_cast<int32>(Params->GetNumberField(TEXT("source_output_index")));

	// Load material
	UMaterial* Material = FMCPMaterialCommands::FindMaterialByPath(MaterialPath);
	if (!Material)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`material_path` did not resolve to a UMaterial. Verify the path with `list_assets` (asset_type='Material'). Paths are case-sensitive and must include `/Game/` prefix. Note this tool requires a UMaterial — material instances and functions are separate assets."));

	// Find source expression
	UMaterialExpression* SourceExpr = FMaterialExpressionManager::FindExpression(Material, SourceExprId);
	if (!SourceExpr)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Source expression not found: %s"), *SourceExprId),
			EMCPErrorCode::NodeNotFound,
			TEXT("`source_expression` did not match any UMaterialExpression on the material. Use `read_material_graph` to list the material's expressions and their stable identifiers. Identifiers are case-sensitive."));

	// Validate source output index
	if (SourceOutputIndex < 0 || SourceOutputIndex >= SourceExpr->GetOutputs().Num())
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Source output index %d out of range (0-%d)"),
				SourceOutputIndex, SourceExpr->GetOutputs().Num() - 1),
			EMCPErrorCode::OutOfRange,
			TEXT("`source_output_index` must be in the half-open range [0, GetOutputs().Num()). Most expressions have a single output at index 0 (which is the default if you omit `source_output_index`). Multi-output expressions (e.g. TextureSample with its RGBA + per-channel outputs) require an explicit index — use `read_material_graph` to enumerate outputs."));

	// Mode 1: Connect to Material input
	if (TargetExprId.Equals(TEXT("Material"), ESearchCase::IgnoreCase))
	{
		FExpressionInput* MatInput = FindMaterialInput(Material, TargetInput);
		if (!MatInput)
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Unknown material input: %s"), *TargetInput),
				EMCPErrorCode::InvalidArgument,
				TEXT("When `target_expression='Material'`, `target_input` must be one of: BaseColor, Metallic, Specular, Roughness, Anisotropy, Normal, Tangent, EmissiveColor, Opacity, OpacityMask, WorldPositionOffset, SubsurfaceColor, AmbientOcclusion, Refraction, PixelDepthOffset. Match is case-insensitive."));

		// dry_run (mode 1: material-attribute target): every preflight ran
		// (paths, source expr, source output index, target slot lookup). Skip the
		// PreEditChange + UPROPERTY writes. Diff shape per todo/13 phase 3:
		// connections_added[] with the previous_connection sub-object if the
		// slot was already wired (a connect on a non-empty slot is an
		// overwrite — surfacing the previous source matters because it'll go
		// dangling in the apply call).
		if (FMCPCommonUtils::ParseDryRun(Params))
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("from_expression"), SourceExpr->GetName());
			Entry->SetNumberField(TEXT("from_output_index"), SourceOutputIndex);
			Entry->SetStringField(TEXT("to_material_attribute"), TargetInput);

			if (MatInput->Expression)
			{
				TSharedPtr<FJsonObject> Prev = MakeShared<FJsonObject>();
				Prev->SetStringField(TEXT("from_expression"), MatInput->Expression->GetName());
				Prev->SetNumberField(TEXT("from_output_index"), MatInput->OutputIndex);
				Entry->SetObjectField(TEXT("previous_connection"), Prev);
			}

			TArray<TSharedPtr<FJsonValue>> Arr;
			Arr.Add(MakeShared<FJsonValueObject>(Entry));
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetArrayField(TEXT("connections_added"), Arr);
			return FMCPCommonUtils::CreateDryRunResponse(Diff);
		}

		// Open edit envelope before the FExpressionInput UPROPERTY writes.
		// Engine pattern: UMaterialEditingLibrary::RecompileMaterialInternal
		// (/ue5-source/Editor/MaterialEditor/Private/MaterialEditingLibrary.cpp:717-720).
		// Validate-before-mutate posture: FindMaterialInput failure fires
		// before the envelope opens.
		Material->PreEditChange(nullptr);
		MatInput->Expression = SourceExpr;
		MatInput->OutputIndex = SourceOutputIndex;
		// Propagate to dependent material instances (see RecompileMaterialWithDependents).
		FMCPCommonUtils::RecompileMaterialWithDependents(Material);
		// GAP-062: persist by default so the wire survives an editor reload.
		if (TSharedPtr<FJsonObject> SaveErr = FMCPCommonUtils::SaveMaterialOrError(Material))
			return SaveErr;

		UE_LOG(LogUnrealMCP, Display, TEXT("Connected %s[%d] → Material.%s"),
			*SourceExpr->GetName(), SourceOutputIndex, *TargetInput);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("source"), SourceExpr->GetName());
		Data->SetNumberField(TEXT("source_output_index"), SourceOutputIndex);
		Data->SetStringField(TEXT("target"), TEXT("Material"));
		Data->SetStringField(TEXT("target_input"), TargetInput);
		Data->SetBoolField(TEXT("success"), true);
		return Data;
	}

	// Mode 2: Connect to another expression's input
	UMaterialExpression* TargetExpr = FMaterialExpressionManager::FindExpression(Material, TargetExprId);
	if (!TargetExpr)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Target expression not found: %s"), *TargetExprId),
			EMCPErrorCode::NodeNotFound,
			TEXT("`target_expression` did not match any UMaterialExpression on the material (and is not the literal `Material`). Use `read_material_graph` to list expressions, or pass `Material` to wire into the material's attribute inputs."));

	FExpressionInput* ExprInput = FindExpressionInput(TargetExpr, TargetInput);
	if (!ExprInput)
	{
		// Build helpful error with available input names
		FString Available;
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* In = TargetExpr->GetInput(i);
			if (!In) break;
			if (i > 0) Available += TEXT(", ");
			Available += FString::Printf(TEXT("%d:%s"), i, *TargetExpr->GetInputName(i).ToString());
		}
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Input '%s' not found on %s. Available: [%s]"),
				*TargetInput, *TargetExpr->GetName(), *Available),
			EMCPErrorCode::PinNotFound,
			TEXT("`target_input` did not match any input pin on the target expression. The error message lists the available input indices and names — pass one of those values. Index forms (`0`, `1`) and name forms are both accepted. For LandscapeLayerBlend, layer names and `<LayerName>_Height` suffixes are also recognized."));
	}

	// dry_run (mode 2: expression target): every preflight ran (paths, source
	// expr, source output range, target expr, input lookup). Skip the
	// PreEditChange + UPROPERTY writes. Same diff shape as mode 1, with the
	// to_input_index/to_input_name pair instead of to_material_attribute. We
	// resolve the resolved input's index via pointer-equality reverse-lookup so
	// the caller gets both the canonical name and the integer index regardless
	// of which form they passed in TargetInput. previous_connection captures
	// the pre-write Expression/OutputIndex pair if the slot was already wired —
	// callers see exactly which existing wire gets clobbered.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		int32 ResolvedInputIndex = INDEX_NONE;
		FString ResolvedInputName;
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* In = TargetExpr->GetInput(i);
			if (!In) break;
			if (In == ExprInput)
			{
				ResolvedInputIndex = i;
				ResolvedInputName = TargetExpr->GetInputName(i).ToString();
				break;
			}
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("from_expression"), SourceExpr->GetName());
		Entry->SetNumberField(TEXT("from_output_index"), SourceOutputIndex);
		Entry->SetStringField(TEXT("to_expression"), TargetExpr->GetName());
		if (ResolvedInputIndex != INDEX_NONE)
		{
			Entry->SetNumberField(TEXT("to_input_index"), ResolvedInputIndex);
			Entry->SetStringField(TEXT("to_input_name"), ResolvedInputName);
		}
		else
		{
			// Fallback: emit the user-supplied identifier so the diff is never
			// missing the target. This branch is unreachable in practice — the
			// pointer-equality scan must succeed if FindExpressionInput did —
			// but kept defensive against future API drift.
			Entry->SetStringField(TEXT("to_input_name"), TargetInput);
		}

		if (ExprInput->Expression)
		{
			TSharedPtr<FJsonObject> Prev = MakeShared<FJsonObject>();
			Prev->SetStringField(TEXT("from_expression"), ExprInput->Expression->GetName());
			Prev->SetNumberField(TEXT("from_output_index"), ExprInput->OutputIndex);
			Entry->SetObjectField(TEXT("previous_connection"), Prev);
		}

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("connections_added"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Open edit envelope before the FExpressionInput UPROPERTY writes.
	// Engine pattern: UMaterialEditingLibrary::RecompileMaterialInternal
	// (/ue5-source/Editor/MaterialEditor/Private/MaterialEditingLibrary.cpp:717-720).
	// Validate-before-mutate posture: target-expression + input lookups fire
	// before the envelope opens.
	Material->PreEditChange(nullptr);
	ExprInput->Expression = SourceExpr;
	ExprInput->OutputIndex = SourceOutputIndex;
	// Propagate to dependent material instances (see RecompileMaterialWithDependents).
	FMCPCommonUtils::RecompileMaterialWithDependents(Material);
	// GAP-062: persist by default so the wire survives an editor reload.
	if (TSharedPtr<FJsonObject> SaveErr = FMCPCommonUtils::SaveMaterialOrError(Material))
		return SaveErr;

	UE_LOG(LogUnrealMCP, Display, TEXT("Connected %s[%d] → %s.%s"),
		*SourceExpr->GetName(), SourceOutputIndex, *TargetExpr->GetName(), *TargetInput);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source"), SourceExpr->GetName());
	Data->SetNumberField(TEXT("source_output_index"), SourceOutputIndex);
	Data->SetStringField(TEXT("target"), TargetExpr->GetName());
	Data->SetStringField(TEXT("target_input"), TargetInput);
	Data->SetBoolField(TEXT("success"), true);
	return Data;
}
