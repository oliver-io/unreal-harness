#include "Commands/MaterialGraph/MaterialExpressionManager.h"
#include "Commands/MCPMaterialCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
// Spatial / coordinate sources — needed by any pixel-position or per-instance
// shader logic (clip-plane materials, world-space gradients, fresnel rims, etc.)
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionObjectPositionWS.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionWorldPosition.h"
// Time / period source.
#include "Materials/MaterialExpressionTime.h"
// Extended math — the existing set (Add/Multiply/Lerp/OneMinus/Power/Clamp/
// AppendVector) is missing fundamentals needed for non-trivial shader graphs.
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionSine.h"
#include "Materials/MaterialExpressionCosine.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionAbs.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionMin.h"
#include "Materials/MaterialExpressionMax.h"
// Conditional + escape hatch.
#include "Materials/MaterialExpressionIf.h"
#include "Materials/MaterialExpressionCustom.h"
// Scene/screen-buffer read — the typed primitive for post-process materials (samples
// PostProcessInput0, Velocity, SceneColor, depth, GBuffer channels) with a UV input so a
// Custom/coordinate node can displace the lookup (screen distortion, datamosh, chroma split).
#include "Materials/MaterialExpressionSceneTexture.h"
#include "Materials/MaterialInstanceConstant.h"
#include "StaticParameterSet.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
// Identity exposure for previously-uninspectable expression types.
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionStaticBool.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionLandscapeLayerWeight.h"
#include "Materials/MaterialExpressionLandscapeGrassOutput.h"
#include "LandscapeGrassType.h"  // ULandscapeGrassType — forward-decl only in the above header
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionRuntimeVirtualTextureSampleParameter.h"
#include "Materials/MaterialParameterCollection.h"
// Per-particle / vertex sources — let materials read Niagara/Cascade particle
// attributes and mesh vertex color end-to-end via MCP.
#include "Materials/MaterialExpressionParticleColor.h"
#include "Materials/MaterialExpressionParticleRelativeTime.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionDynamicParameter.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static UMaterialExpression* CreateBaseExpression(
	UMaterial* Material, UClass* ExprClass, float PosX, float PosY, const FString& Desc)
{
	UMaterialExpression* Expr = NewObject<UMaterialExpression>(Material, ExprClass);
	if (!Expr) return nullptr;
	Expr->MaterialExpressionEditorX = static_cast<int32>(PosX);
	Expr->MaterialExpressionEditorY = static_cast<int32>(PosY);
	if (!Desc.IsEmpty())
	{
		Expr->Desc = Desc;
	}
	Material->GetExpressionCollection().AddExpression(Expr);
	return Expr;
}

static TSharedPtr<FJsonObject> MakeExpressionResult(UMaterialExpression* Expr, UMaterial* Material)
{
	int32 Idx = Material->GetExpressionCollection().Expressions.IndexOfByKey(Expr);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("expression_name"), Expr->GetName());
	Data->SetNumberField(TEXT("expression_index"), Idx);
	Data->SetStringField(TEXT("expression_type"), Expr->GetClass()->GetName());
	// Reader-shape aliases: ExpressionToJson emits per-expression
	// {name, index, type} with the "MaterialExpression" prefix stripped from
	// `type`. Emit both shapes so an add→read workflow can match entries
	// without key/value translation; the expression_* keys above stay the
	// canonical response shape for back-compat.
	FString ShortType = Expr->GetClass()->GetName();
	ShortType.RemoveFromStart(TEXT("MaterialExpression"));
	Data->SetStringField(TEXT("name"), Expr->GetName());
	Data->SetNumberField(TEXT("index"), Idx);
	Data->SetStringField(TEXT("type"), ShortType);
	Data->SetBoolField(TEXT("success"), true);
	return Data;
}

// Partial-spec FLinearColor construction shared by every color/vector-valued
// expression branch (Constant3Vector / Constant4Vector / VectorParameter /
// DynamicParameter, in both AddExpression and SetExpressionProperty's apply
// + dry-run trees). Returns false — leaving Out untouched — when the GATE
// key is absent: `r`, or additionally `<LongPrefix>r` when LongPrefix is
// supplied (the dual-key shapes accept the reader-canonical `default_*`
// aliases). The gate is deliberately r-specific, not any-component: a
// g-only payload writes nothing, matching the pre-extraction branches.
// Missing components take Defaults (0,0,0,1 everywhere except
// DynamicParameter's all-1 white). bReadAlpha=false skips the alpha key
// entirely — Constant3Vector's alpha is editor-hidden and stays Defaults.A.
static bool TryBuildLinearColorFromJson(
	const TSharedPtr<FJsonObject>& Params,
	bool bReadAlpha,
	const TCHAR* LongPrefix,        // e.g. TEXT("default_"); nullptr = single-key
	const FLinearColor& Defaults,
	FLinearColor& Out)
{
	auto Pick = [&Params, LongPrefix](const TCHAR* Short, float Default) -> float
	{
		if (Params->HasField(Short))
			return static_cast<float>(Params->GetNumberField(Short));
		if (LongPrefix)
		{
			const FString Long = FString(LongPrefix) + Short;
			if (Params->HasField(Long))
				return static_cast<float>(Params->GetNumberField(Long));
		}
		return Default;
	};

	const bool bGate = Params->HasField(TEXT("r")) ||
		(LongPrefix && Params->HasField(FString(LongPrefix) + TEXT("r")));
	if (!bGate) return false;

	Out = FLinearColor(
		Pick(TEXT("r"), Defaults.R),
		Pick(TEXT("g"), Defaults.G),
		Pick(TEXT("b"), Defaults.B),
		bReadAlpha ? Pick(TEXT("a"), Defaults.A) : Defaults.A);
	return true;
}

// ---------------------------------------------------------------------------
// FindExpression — by UObject name or numeric index
// ---------------------------------------------------------------------------

UMaterialExpression* FMaterialExpressionManager::FindExpression(
	UMaterial* Material, const FString& ExpressionId)
{
	if (!Material) return nullptr;

	auto& Exprs = Material->GetExpressionCollection().Expressions;

	// Try numeric index first
	if (ExpressionId.IsNumeric())
	{
		int32 Idx = FCString::Atoi(*ExpressionId);
		if (Exprs.IsValidIndex(Idx))
		{
			return Exprs[Idx];
		}
	}

	// Search by UObject name or Desc
	for (UMaterialExpression* Expr : Exprs)
	{
		if (!Expr) continue;
		if (Expr->GetName() == ExpressionId || Expr->Desc == ExpressionId)
		{
			return Expr;
		}
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// AddExpression
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMaterialExpressionManager::AddExpression(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'material_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`material_path` is required and must be the full asset path to a UMaterial, e.g. `/Game/Materials/M_Foo`. Use `list_assets` with `asset_type='Material'` to discover."));

	FString ExprType;
	if (!Params->TryGetStringField(TEXT("expression_type"), ExprType))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'expression_type'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`expression_type` is required. Valid values include `TextureSample`, `TextureSampleParameter2D`, `TextureCoordinate`, `ComponentMask`, `LandscapeLayerBlend`, `Constant`, `Constant3Vector`, `Constant4Vector`, `Add`/`Multiply`/`Lerp`/`OneMinus`/`Power`/`Clamp`/`AppendVector`, `Subtract`/`Divide`/`Sine`/`Cosine`/`Frac`/`Abs`/`Saturate`/`Min`/`Max`, `LocalPosition`/`ObjectPositionWS`/`VertexNormalWS`, `ParticleColor`/`VertexColor`/`DynamicParameter`, `Time`, `If`, `Custom`, `ScalarParameter`, `VectorParameter`."));

	UMaterial* Material = FMCPMaterialCommands::FindMaterialByPath(MaterialPath);
	if (!Material)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`material_path` did not resolve to a UMaterial. Verify the asset exists and the path is the full `/Game/...` form. Use `list_assets` with `asset_type='Material'` to discover."));

	float PosX = 0, PosY = 0;
	if (Params->HasField(TEXT("pos_x"))) PosX = Params->GetNumberField(TEXT("pos_x"));
	if (Params->HasField(TEXT("pos_y"))) PosY = Params->GetNumberField(TEXT("pos_y"));

	FString Desc;
	Params->TryGetStringField(TEXT("desc"), Desc);

	// dry_run: material lookup + position parse + desc are validation. The
	// type-dispatch that follows would CreateBaseExpression and add to the
	// material's expression collection — that's the side effect we skip.
	// Diff shape per todo/13 phase 3: {expressions_added: [{type, position,
	// desc, requested_params (echoed back from input)}]}. The new expression
	// has no UE-assigned identifier pre-create (UMaterialExpression objects
	// have FGuid MaterialExpressionGuid filled at CreateBaseExpression time),
	// so the dry-run diff carries the requested type + position. Type-specific
	// parameter validation (texture path resolution, etc.) happens at commit;
	// dry-run echoes the type-specific kwargs as-is.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> AddEntry = MakeShared<FJsonObject>();
		AddEntry->SetStringField(TEXT("type"), ExprType);
		TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), PosX);
		Position->SetNumberField(TEXT("y"), PosY);
		AddEntry->SetObjectField(TEXT("position"), Position);
		if (!Desc.IsEmpty()) AddEntry->SetStringField(TEXT("desc"), Desc);

		// Echo the type-specific kwargs the agent passed — texture_path,
		// parameter_name, u_tiling/v_tiling, etc. — so the diff documents
		// what would have been seeded onto the new expression. Use the
		// existing Params object; pull only the keys the agent supplied that
		// aren't the four core inputs (material_path / expression_type /
		// pos_x/pos_y / desc / dry_run).
		TSharedPtr<FJsonObject> RequestedParams = MakeShared<FJsonObject>();
		for (const auto& Pair : Params->Values)
		{
			const FString& Key = Pair.Key;
			if (Key == TEXT("material_path") || Key == TEXT("expression_type") ||
			    Key == TEXT("pos_x") || Key == TEXT("pos_y") ||
			    Key == TEXT("desc") || Key == TEXT("dry_run"))
			{
				continue;
			}
			RequestedParams->SetField(Key, Pair.Value);
		}
		if (RequestedParams->Values.Num() > 0)
		{
			AddEntry->SetObjectField(TEXT("requested_params"), RequestedParams);
		}

		TArray<TSharedPtr<FJsonValue>> AddedArr;
		AddedArr.Add(MakeShared<FJsonValueObject>(AddEntry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("expressions_added"), AddedArr);
		TSharedPtr<FJsonObject> Wrapped = FMCPCommonUtils::CreateDryRunResponse(Diff);
		Wrapped->SetStringField(TEXT("material_path"), Material->GetPathName());
		return Wrapped;
	}

	// Open edit envelope before the first mutation inside the type-dispatch
	// switch below. Each branch calls CreateBaseExpression, whose tail
	// Material->GetExpressionCollection().AddExpression(Expr) is the UPROPERTY
	// write that must be bracketed by PreEditChange / PostEditChange.
	// Engine pattern: UMaterialEditingLibrary::RecompileMaterialInternal
	// (/ue5-source/Editor/MaterialEditor/Private/MaterialEditingLibrary.cpp:717-720)
	// pairs PreEditChange(nullptr) / PostEditChange() / MarkPackageDirty().
	// Explicit-close-on-error posture: the two early error returns below
	// (Unknown expression type, !NewExpr) close the envelope with
	// PostEditChange() before returning. Mirrors the Niagara setter pattern
	// at MCPNiagaraCommands.cpp:431-440 cited in the
	// 2026-04-23 EQS set_eqs_property bundle's #RESEARCH note on the two
	// correct shapes for handling failable mutations.
	Material->PreEditChange(nullptr);

	UMaterialExpression* NewExpr = nullptr;

	// -----------------------------------------------------------------------
	// TextureSample
	// -----------------------------------------------------------------------
	if (ExprType.Equals(TEXT("TextureSample"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionTextureSample>(
			CreateBaseExpression(Material, UMaterialExpressionTextureSample::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create TextureSample"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionTextureSample>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));

		// `texture_path` is setter-canonical; `texture` is the key ExpressionToJson
		// emits, accepted as a read→write round-trip alias. texture_path wins when
		// both are supplied. Same shim as set_material_instance_parameter's
		// texture/texture_path pair.
		FString TexturePath;
		if (Params->TryGetStringField(TEXT("texture_path"), TexturePath) ||
		    Params->TryGetStringField(TEXT("texture"), TexturePath))
		{
			UTexture* Tex = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexturePath));
			// Match the material editor: deriving SamplerType from the texture's
			// compression (Normal → Normal, Grayscale/Masks → LinearGrayscale, …)
			// keeps the node compilable. Without this a normal/data map sampled as
			// the default Color sampler fails compilation ("Sampler type is Color,
			// should be Normal/Linear Grayscale").
			if (Tex) { Expr->Texture = Tex; Expr->AutoSetSampleType(); }
			else UE_LOG(LogUnrealMCP, Warning, TEXT("Texture not found: %s"), *TexturePath);
		}
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// TextureSampleParameter2D
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("TextureSampleParameter2D"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionTextureSampleParameter2D>(
			CreateBaseExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create TextureSampleParameter2D"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionTextureSampleParameter2D>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));

		FString ParamName;
		if (Params->TryGetStringField(TEXT("parameter_name"), ParamName))
			Expr->ParameterName = FName(*ParamName);
		// texture_path canonical, texture reader-alias — see TextureSample above.
		FString TexturePath;
		if (Params->TryGetStringField(TEXT("texture_path"), TexturePath) ||
		    Params->TryGetStringField(TEXT("texture"), TexturePath))
		{
			UTexture* Tex = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexturePath));
			if (Tex) { Expr->Texture = Tex; Expr->AutoSetSampleType(); }
		}
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// TextureCoordinate
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("TextureCoordinate"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionTextureCoordinate>(
			CreateBaseExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create TextureCoordinate"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionTextureCoordinate>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));

		if (Params->HasField(TEXT("u_tiling"))) Expr->UTiling = Params->GetNumberField(TEXT("u_tiling"));
		if (Params->HasField(TEXT("v_tiling"))) Expr->VTiling = Params->GetNumberField(TEXT("v_tiling"));
		if (Params->HasField(TEXT("coordinate_index")))
			Expr->CoordinateIndex = static_cast<int32>(Params->GetNumberField(TEXT("coordinate_index")));
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// ComponentMask
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("ComponentMask"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionComponentMask>(
			CreateBaseExpression(Material, UMaterialExpressionComponentMask::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create ComponentMask"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionComponentMask>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));

		if (Params->HasField(TEXT("r"))) Expr->R = Params->GetBoolField(TEXT("r")) ? 1 : 0;
		if (Params->HasField(TEXT("g"))) Expr->G = Params->GetBoolField(TEXT("g")) ? 1 : 0;
		if (Params->HasField(TEXT("b"))) Expr->B = Params->GetBoolField(TEXT("b")) ? 1 : 0;
		if (Params->HasField(TEXT("a"))) Expr->A = Params->GetBoolField(TEXT("a")) ? 1 : 0;
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// LandscapeLayerBlend
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("LandscapeLayerBlend"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionLandscapeLayerBlend>(
			CreateBaseExpression(Material, UMaterialExpressionLandscapeLayerBlend::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create LandscapeLayerBlend"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionLandscapeLayerBlend>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));

		const TArray<TSharedPtr<FJsonValue>>* LayersArray = nullptr;
		if (Params->TryGetArrayField(TEXT("layers"), LayersArray))
		{
			for (const auto& LayerVal : *LayersArray)
			{
				const TSharedPtr<FJsonObject>& LayerObj = LayerVal->AsObject();
				if (!LayerObj) continue;

				FLayerBlendInput Layer;
				FString LayerName;
				if (LayerObj->TryGetStringField(TEXT("name"), LayerName))
					Layer.LayerName = FName(*LayerName);

				FString BlendTypeStr;
				if (LayerObj->TryGetStringField(TEXT("blend_type"), BlendTypeStr))
				{
					if (BlendTypeStr.Contains(TEXT("Height")))
						Layer.BlendType = LB_HeightBlend;
					else if (BlendTypeStr.Contains(TEXT("Alpha")))
						Layer.BlendType = LB_AlphaBlend;
					else
						Layer.BlendType = LB_WeightBlend;
				}

				if (LayerObj->HasField(TEXT("preview_weight")))
					Layer.PreviewWeight = static_cast<float>(LayerObj->GetNumberField(TEXT("preview_weight")));

				Expr->Layers.Add(Layer);
			}
		}
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// Constant
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("Constant"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionConstant>(
			CreateBaseExpression(Material, UMaterialExpressionConstant::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create Constant"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionConstant>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));
		if (Params->HasField(TEXT("value"))) Expr->R = Params->GetNumberField(TEXT("value"));
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// Constant3Vector
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("Constant3Vector"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionConstant3Vector>(
			CreateBaseExpression(Material, UMaterialExpressionConstant3Vector::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create Constant3Vector"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionConstant3Vector>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));
		// Gate on `r` only and zero-fill missing g/b (no alpha key — editor-
		// hidden). Mirrors Constant4Vector (below) and VectorParameter.
		TryBuildLinearColorFromJson(Params, /*bReadAlpha*/false, nullptr,
			FLinearColor(0.f, 0.f, 0.f, 1.f), Expr->Constant);
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// Constant4Vector
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("Constant4Vector"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionConstant4Vector>(
			CreateBaseExpression(Material, UMaterialExpressionConstant4Vector::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create Constant4Vector"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionConstant4Vector>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));
		TryBuildLinearColorFromJson(Params, /*bReadAlpha*/true, nullptr,
			FLinearColor(0.f, 0.f, 0.f, 1.f), Expr->Constant);
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// Math nodes (no extra params beyond inputs)
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("Add"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionAdd::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionMultiply::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Lerp"), ESearchCase::IgnoreCase) ||
	         ExprType.Equals(TEXT("LinearInterpolate"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionLinearInterpolate::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("OneMinus"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionOneMinus::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Power"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionPower::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Clamp"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionClamp::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("AppendVector"), ESearchCase::IgnoreCase) ||
	         ExprType.Equals(TEXT("Append"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionAppendVector::StaticClass(), PosX, PosY, Desc);
	}
	// -----------------------------------------------------------------------
	// Extended math (no per-type fields — wire inputs via connect_material_expressions)
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("Subtract"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionSubtract::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Divide"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionDivide::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Sine"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionSine::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Cosine"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionCosine::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Frac"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionFrac::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Abs"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionAbs::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Saturate"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionSaturate::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Min"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionMin::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("Max"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionMax::StaticClass(), PosX, PosY, Desc);
	}
	// -----------------------------------------------------------------------
	// Spatial sources (no per-type fields)
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("LocalPosition"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionLocalPosition::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("ObjectPositionWS"), ESearchCase::IgnoreCase) ||
	         ExprType.Equals(TEXT("ObjectPosition"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionObjectPositionWS::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("VertexNormalWS"), ESearchCase::IgnoreCase) ||
	         ExprType.Equals(TEXT("VertexNormal"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionVertexNormalWS::StaticClass(), PosX, PosY, Desc);
	}
	// GAP-008: first-class absolute world-position source. Previously only
	// LocalPosition / ObjectPositionWS existed, forcing a Custom HLSL node (with the
	// UE5.7 DFDemote large-world-coords dance) for what is a one-node primitive.
	else if (ExprType.Equals(TEXT("WorldPosition"), ESearchCase::IgnoreCase) ||
	         ExprType.Equals(TEXT("WorldPositionWS"), ESearchCase::IgnoreCase) ||
	         ExprType.Equals(TEXT("AbsoluteWorldPosition"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionWorldPosition::StaticClass(), PosX, PosY, Desc);
	}
	// -----------------------------------------------------------------------
	// Time — period and ignore_pause are optional. bOverride_Period must be
	// set when Period is specified, or the engine ignores the value (engine
	// ref: UMaterialExpressionTime::Period UPROPERTY(meta=(EditCondition=...))
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("Time"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionTime>(
			CreateBaseExpression(Material, UMaterialExpressionTime::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create Time"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionTime>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));
		if (Params->HasField(TEXT("ignore_pause")))
			Expr->bIgnorePause = Params->GetBoolField(TEXT("ignore_pause"));
		if (Params->HasField(TEXT("period")))
		{
			Expr->bOverride_Period = true;
			Expr->Period = static_cast<float>(Params->GetNumberField(TEXT("period")));
		}
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// If — conditional (A>B → AGreaterThan, A<B → ALessThan, A==B → AEqualsB).
	// equals_threshold is optional; defaults to engine CDO (0.00001).
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("If"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionIf>(
			CreateBaseExpression(Material, UMaterialExpressionIf::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create If"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionIf>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));
		if (Params->HasField(TEXT("equals_threshold")))
			Expr->EqualsThreshold = static_cast<float>(Params->GetNumberField(TEXT("equals_threshold")));
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// Custom HLSL — escape hatch for any expression the per-type branches
	// above don't cover. `code` is the HLSL body, `output_type` is one of
	// "Float1"/"Float2"/"Float3"/"Float4" (CMOT_* prefix accepted), and
	// `inputs` is an optional [{name: str}, ...] for naming pin inputs.
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionCustom>(
			CreateBaseExpression(Material, UMaterialExpressionCustom::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create Custom"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionCustom>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));
		FString Code;
		if (Params->TryGetStringField(TEXT("code"), Code))
			Expr->Code = Code;
		FString OutputType;
		if (Params->TryGetStringField(TEXT("output_type"), OutputType))
		{
			if (OutputType.Equals(TEXT("Float1"), ESearchCase::IgnoreCase) ||
			    OutputType.Equals(TEXT("CMOT_Float1"), ESearchCase::IgnoreCase))
				Expr->OutputType = CMOT_Float1;
			else if (OutputType.Equals(TEXT("Float2"), ESearchCase::IgnoreCase) ||
			         OutputType.Equals(TEXT("CMOT_Float2"), ESearchCase::IgnoreCase))
				Expr->OutputType = CMOT_Float2;
			else if (OutputType.Equals(TEXT("Float3"), ESearchCase::IgnoreCase) ||
			         OutputType.Equals(TEXT("CMOT_Float3"), ESearchCase::IgnoreCase))
				Expr->OutputType = CMOT_Float3;
			else if (OutputType.Equals(TEXT("Float4"), ESearchCase::IgnoreCase) ||
			         OutputType.Equals(TEXT("CMOT_Float4"), ESearchCase::IgnoreCase))
				Expr->OutputType = CMOT_Float4;
		}
		const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
		if (Params->TryGetArrayField(TEXT("inputs"), InputsArray))
		{
			Expr->Inputs.Empty();
			for (const auto& InputVal : *InputsArray)
			{
				const TSharedPtr<FJsonObject>& InputObj = InputVal->AsObject();
				if (!InputObj) continue;
				FCustomInput NewInput;
				FString InputName;
				if (InputObj->TryGetStringField(TEXT("name"), InputName))
					NewInput.InputName = FName(*InputName);
				Expr->Inputs.Add(NewInput);
			}
		}
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// SceneTexture — typed screen/scene-buffer read. The primitive a post-process
	// material needs: a `Coordinates` UV input (GetInput(0)) decides WHERE it samples,
	// so a Custom/coordinate node can displace the lookup (screen warp, datamosh,
	// chromatic split). `scene_texture_id` selects the buffer (default PostProcessInput0
	// — the pass's own scene-color input); `filtered` toggles bilinear sampling. Placing
	// this node is also what flags the buffer as USED, so SceneTextureLookup(...) calls in
	// a sibling Custom node actually bind (a Custom node alone is opaque to the translator).
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("SceneTexture"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionSceneTexture>(
			CreateBaseExpression(Material, UMaterialExpressionSceneTexture::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create SceneTexture"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionSceneTexture>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));

		// Default to PostProcessInput0 (the factory default is SceneColor, wrong for a PP graph).
		Expr->SceneTextureId = PPI_PostProcessInput0;
		FString IdStr;
		if (Params->TryGetStringField(TEXT("scene_texture_id"), IdStr))
		{
			if      (IdStr.Equals(TEXT("PostProcessInput0"), ESearchCase::IgnoreCase)) Expr->SceneTextureId = PPI_PostProcessInput0;
			else if (IdStr.Equals(TEXT("SceneColor"),        ESearchCase::IgnoreCase)) Expr->SceneTextureId = PPI_SceneColor;
			else if (IdStr.Equals(TEXT("SceneDepth"),        ESearchCase::IgnoreCase)) Expr->SceneTextureId = PPI_SceneDepth;
			else if (IdStr.Equals(TEXT("Velocity"),          ESearchCase::IgnoreCase)) Expr->SceneTextureId = PPI_Velocity;
			else if (IdStr.Equals(TEXT("WorldNormal"),       ESearchCase::IgnoreCase)) Expr->SceneTextureId = PPI_WorldNormal;
			else if (IdStr.Equals(TEXT("CustomDepth"),       ESearchCase::IgnoreCase)) Expr->SceneTextureId = PPI_CustomDepth;
			else if (IdStr.Equals(TEXT("CustomStencil"),     ESearchCase::IgnoreCase)) Expr->SceneTextureId = PPI_CustomStencil;
			else if (IdStr.Equals(TEXT("BaseColor"),         ESearchCase::IgnoreCase)) Expr->SceneTextureId = PPI_BaseColor;
			else if (IdStr.Equals(TEXT("WorldTangent"),      ESearchCase::IgnoreCase)) Expr->SceneTextureId = PPI_WorldTangent;
			else if (IdStr.Equals(TEXT("AmbientOcclusion"),  ESearchCase::IgnoreCase)) Expr->SceneTextureId = PPI_AmbientOcclusion;
			else UE_LOG(LogUnrealMCP, Warning, TEXT("Unknown scene_texture_id '%s' — defaulting to PostProcessInput0"), *IdStr);
		}
		if (Params->HasField(TEXT("filtered")))
			Expr->bFiltered = Params->GetBoolField(TEXT("filtered"));
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// ScalarParameter
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("ScalarParameter"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionScalarParameter>(
			CreateBaseExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create ScalarParameter"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionScalarParameter>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));
		FString ParamName;
		if (Params->TryGetStringField(TEXT("parameter_name"), ParamName))
			Expr->ParameterName = FName(*ParamName);
		if (Params->HasField(TEXT("default_value")))
			Expr->DefaultValue = Params->GetNumberField(TEXT("default_value"));
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// VectorParameter
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("VectorParameter"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionVectorParameter>(
			CreateBaseExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create VectorParameter"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionVectorParameter>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));
		FString ParamName;
		if (Params->TryGetStringField(TEXT("parameter_name"), ParamName))
			Expr->ParameterName = FName(*ParamName);
		// Accepts both the legacy `r/g/b/a` keys and the reader-canonical
		// `default_r/g/b/a` keys that ExpressionToJson emits for
		// VectorParameter; short keys win when both are supplied.
		TryBuildLinearColorFromJson(Params, /*bReadAlpha*/true, TEXT("default_"),
			FLinearColor(0.f, 0.f, 0.f, 1.f), Expr->DefaultValue);
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// Per-particle / vertex sources (no per-type fields). ParticleColor and
	// VertexColor are UMaterialExpressionExternalCodeBase nodes with fixed
	// outputs and no editable members — create-and-done, like the spatial
	// sources above.
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("ParticleColor"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionParticleColor::StaticClass(), PosX, PosY, Desc);
	}
	// ParticleRelativeTime — per-particle normalized age (0 at spawn → 1 at death).
	// A fixed-output ExternalCode node like ParticleColor; must exist as a real
	// expression node (not just referenced in a Custom node) for the translator to
	// bind the per-particle interpolator, so this factory entry is what makes an
	// age-driven fade actually work on Niagara/Cascade particles.
	else if (ExprType.Equals(TEXT("ParticleRelativeTime"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionParticleRelativeTime::StaticClass(), PosX, PosY, Desc);
	}
	else if (ExprType.Equals(TEXT("VertexColor"), ESearchCase::IgnoreCase))
	{
		NewExpr = CreateBaseExpression(Material, UMaterialExpressionVertexColor::StaticClass(), PosX, PosY, Desc);
	}
	// -----------------------------------------------------------------------
	// DynamicParameter — exposes a Niagara/Cascade per-particle DynamicParameter
	// channel as four named RGBA outputs. All fields optional:
	//   param_names      : exactly 4 strings (the RGBA output labels). ParamNames
	//                      is editfixedsize at 4 — we only OVERWRITE when 4 names
	//                      are supplied, never clear it (an empty array would
	//                      break the output pins).
	//   parameter_index  : which of the 4 dynamic-param channels to read (0..3).
	//   r/g/b/a or default_r/g/b/a : seed DefaultValue (used when the emitter
	//                      provides no dynamic-param value). Default white.
	// -----------------------------------------------------------------------
	else if (ExprType.Equals(TEXT("DynamicParameter"), ESearchCase::IgnoreCase))
	{
		auto* Expr = Cast<UMaterialExpressionDynamicParameter>(
			CreateBaseExpression(Material, UMaterialExpressionDynamicParameter::StaticClass(), PosX, PosY, Desc));
		if (!Expr) return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create DynamicParameter"),
			EMCPErrorCode::Internal,
			TEXT("`CreateBaseExpression` returned null — `NewObject<UMaterialExpressionDynamicParameter>` failed inside the editor. Check the editor log for `LogObj` warnings, save and restart the editor, and retry."));
		const TArray<TSharedPtr<FJsonValue>>* ParamNamesArr = nullptr;
		if (Params->TryGetArrayField(TEXT("param_names"), ParamNamesArr) && ParamNamesArr)
		{
			if (ParamNamesArr->Num() != 4)
				return FMCPCommonUtils::CreateErrorResponse(
					TEXT("`param_names` must have exactly 4 entries"),
					EMCPErrorCode::InvalidArgument,
					TEXT("UMaterialExpressionDynamicParameter::ParamNames is editfixedsize at 4 (one label per RGBA output). Supply exactly 4 strings, or omit `param_names` to keep the defaults."));
			if (Expr->ParamNames.Num() < 4)
				Expr->ParamNames.SetNum(4);
			for (int32 i = 0; i < 4; ++i)
				Expr->ParamNames[i] = (*ParamNamesArr)[i]->AsString();
		}
		if (Params->HasField(TEXT("parameter_index")))
			Expr->ParameterIndex = (uint32)FMath::Clamp((int32)Params->GetNumberField(TEXT("parameter_index")), 0, 3);
		// Dual-key like VectorParameter, but components default to 1 (white)
		// — the engine fallback when the emitter provides no value.
		TryBuildLinearColorFromJson(Params, /*bReadAlpha*/true, TEXT("default_"),
			FLinearColor(1.f, 1.f, 1.f, 1.f), Expr->DefaultValue);
		NewExpr = Expr;
	}
	// -----------------------------------------------------------------------
	// Unknown
	// -----------------------------------------------------------------------
	else
	{
		// Close the envelope opened above before erroring out. No mutation
		// occurred in this branch — PreEditChange's Modify() call will have
		// marked the package dirty, which is a benign false-positive cleared
		// on the next save.
		Material->PostEditChange();
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown expression type: %s"), *ExprType),
			EMCPErrorCode::InvalidArgument,
			TEXT("`expression_type` is not one of the supported values. Valid set: `TextureSample`/`TextureSampleParameter2D`/`TextureCoordinate`/`ComponentMask`/`LandscapeLayerBlend`/`Constant`/`Constant3Vector`/`Constant4Vector`/`Add`/`Multiply`/`Lerp`/`OneMinus`/`Power`/`Clamp`/`AppendVector`/`Subtract`/`Divide`/`Sine`/`Cosine`/`Frac`/`Abs`/`Saturate`/`Min`/`Max`/`LocalPosition`/`ObjectPositionWS`/`VertexNormalWS`/`WorldPosition`/`ParticleColor`/`ParticleRelativeTime`/`VertexColor`/`DynamicParameter`/`Time`/`If`/`Custom`/`SceneTexture`/`ScalarParameter`/`VectorParameter`. Names are case-insensitive."));
	}

	if (!NewExpr)
	{
		// Defensive path (NewObject failure is practically unreachable for
		// the validated UClasses above). Close the envelope before erroring.
		Material->PostEditChange();
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Failed to create expression"),
			EMCPErrorCode::Internal,
			TEXT("Defensive fallback — every per-type branch passed type-validation but `NewExpr` is still null. This indicates an internal engine failure; check the editor log and retry."));
	}

	// Propagate to dependent material instances (not just the base UMaterial), else
	// child MICs keep a stale render proxy and draw as the default grey-checker.
	FMCPCommonUtils::RecompileMaterialWithDependents(Material);
	// GAP-062: persist by default so the new node survives an editor reload.
	if (TSharedPtr<FJsonObject> SaveErr = FMCPCommonUtils::SaveMaterialOrError(Material))
		return SaveErr;
	return MakeExpressionResult(NewExpr, Material);
}

// ---------------------------------------------------------------------------
// DeleteExpression
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMaterialExpressionManager::DeleteExpression(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'material_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`material_path` is required and must be the full asset path to a UMaterial, e.g. `/Game/Materials/M_Foo`. Use `list_assets` with `asset_type='Material'` to discover."));

	FString ExprId;
	if (!Params->TryGetStringField(TEXT("expression_name"), ExprId))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'expression_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`expression_name` is required — pass the node's `GetName()` value. Use `read_material_graph` to list expression names for this material."));

	UMaterial* Material = FMCPMaterialCommands::FindMaterialByPath(MaterialPath);
	if (!Material)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`material_path` did not resolve to a UMaterial. Verify the asset exists and the path is the full `/Game/...` form. Use `list_assets` with `asset_type='Material'` to discover."));

	UMaterialExpression* Expr = FindExpression(Material, ExprId);
	if (!Expr)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Expression not found: %s"), *ExprId),
			EMCPErrorCode::NodeNotFound,
			TEXT("`expression_name` did not match any node in this material. Use `read_material_graph` to see current expression names — they are GUID-suffixed engine names (e.g. `MaterialExpressionMultiply_0`)."));

	// dry_run: every preflight ran (material lookup, expression lookup). The
	// remaining work — Material->PreEditChange + RemoveExpression + PostEditChange
	// + MarkPackageDirty — is the side effect we skip. Diff shape per todo/13
	// phase 3: expressions_removed[] with the cascade made explicit. The cascade
	// for a material expression has two arms:
	//   1. Other expressions whose input pins point at this expression — those
	//      wires get severed (FExpressionInput::Expression goes null).
	//   2. The material's own attribute slots (BaseColor, Metallic, ...) on
	//      UMaterialEditorOnlyData — same severing semantics.
	// We enumerate both so the caller sees exactly what edges disappear.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("expression_name"), Expr->GetName());
		Entry->SetStringField(TEXT("class_name"), Expr->GetClass()->GetName());
		Entry->SetNumberField(TEXT("pos_x"), Expr->MaterialExpressionEditorX);
		Entry->SetNumberField(TEXT("pos_y"), Expr->MaterialExpressionEditorY);

		// Arm 1: scan every other expression's inputs for references back to Expr.
		TArray<TSharedPtr<FJsonValue>> SeveredArr;
		for (UMaterialExpression* Other : Material->GetExpressions())
		{
			if (!Other || Other == Expr) continue;
			for (int32 i = 0; ; ++i)
			{
				FExpressionInput* In = Other->GetInput(i);
				if (!In) break;
				if (In->Expression == Expr)
				{
					TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
					S->SetStringField(TEXT("from_expression"), Expr->GetName());
					S->SetNumberField(TEXT("from_output_index"), In->OutputIndex);
					S->SetStringField(TEXT("to_expression"), Other->GetName());
					S->SetNumberField(TEXT("to_input_index"), i);
					S->SetStringField(TEXT("to_input_name"), Other->GetInputName(i).ToString());
					SeveredArr.Add(MakeShared<FJsonValueObject>(S));
				}
			}
		}

		// Arm 2: scan the 15 material-attribute slots on UMaterialEditorOnlyData.
		// Inlined here rather than calling MaterialConnector::FindMaterialInput
		// (file-static) — duplicating the slot list once is cheaper than
		// promoting that helper to a shared header. If the slot list ever
		// changes upstream, both this and FindMaterialInput need updating.
		if (UMaterialEditorOnlyData* Ed = Material->GetEditorOnlyData())
		{
			struct FNamedSlot { const TCHAR* Name; FExpressionInput* Input; };
			const FNamedSlot Slots[] = {
				{ TEXT("BaseColor"),           &Ed->BaseColor },
				{ TEXT("Metallic"),            &Ed->Metallic },
				{ TEXT("Specular"),            &Ed->Specular },
				{ TEXT("Roughness"),           &Ed->Roughness },
				{ TEXT("Anisotropy"),          &Ed->Anisotropy },
				{ TEXT("Normal"),              &Ed->Normal },
				{ TEXT("Tangent"),             &Ed->Tangent },
				{ TEXT("EmissiveColor"),       &Ed->EmissiveColor },
				{ TEXT("Opacity"),             &Ed->Opacity },
				{ TEXT("OpacityMask"),         &Ed->OpacityMask },
				{ TEXT("WorldPositionOffset"), &Ed->WorldPositionOffset },
				{ TEXT("SubsurfaceColor"),     &Ed->SubsurfaceColor },
				{ TEXT("AmbientOcclusion"),    &Ed->AmbientOcclusion },
				{ TEXT("Refraction"),          &Ed->Refraction },
				{ TEXT("PixelDepthOffset"),    &Ed->PixelDepthOffset },
			};
			for (const FNamedSlot& Slot : Slots)
			{
				if (Slot.Input && Slot.Input->Expression == Expr)
				{
					TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
					S->SetStringField(TEXT("from_expression"), Expr->GetName());
					S->SetNumberField(TEXT("from_output_index"), Slot.Input->OutputIndex);
					S->SetStringField(TEXT("to_material_attribute"), Slot.Name);
					SeveredArr.Add(MakeShared<FJsonValueObject>(S));
				}
			}
		}

		Entry->SetArrayField(TEXT("connections_severed"), SeveredArr);

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("expressions_removed"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Open edit envelope before the UPROPERTY mutation. Engine pattern:
	// UMaterialEditingLibrary::RecompileMaterialInternal
	// (/ue5-source/Editor/MaterialEditor/Private/MaterialEditingLibrary.cpp:717-720).
	// Validate-before-mutate posture: all parse + existence checks above fire
	// before the envelope opens, so no close-on-error is required here.
	Material->PreEditChange(nullptr);

	// CRASH FIX: sever EVERY link into this node *before* removing it — exactly
	// what the engine's own UMaterialEditingLibrary::DeleteMaterialExpression does
	// (/ue5-source/Editor/MaterialEditor/Private/MaterialEditingLibrary.cpp:447-472:
	// BreakLinksToExpression + the MP_MAX material-attribute-input loop, *then*
	// RemoveExpression). Previously we called RemoveExpression and immediately
	// PostEditChange() without severing, which left dangling
	// FExpressionInput::Expression pointers (on surviving nodes and on the
	// material's own attribute slots) aimed at a node no longer in the collection.
	// PostEditChange() → UMaterial::UpdateCachedExpressionData() →
	// AnalyzeMaterial() (Material.cpp:2349-2385) does a *connectivity* walk of that
	// now-inconsistent graph, so the rebuilt CachedExpressionData->ReferencedTextures
	// no longer matched what the HLSL translator walks. During the ensuing
	// CacheResourceShadersForRendering() recompile (Material.cpp:5380-5384) a
	// SURVIVING UMaterialExpressionTextureSample::Compile() then called
	// Compiler->Texture(), whose Material->GetReferencedTextures().Find(InTexture)
	// returned INDEX_NONE and tripped the hard assert at
	// FHLSLMaterialTranslator::Texture() (HLSLMaterialTranslator.cpp:8920).
	// Severing first keeps the graph and the cached data consistent across the
	// recompile, so deleting a connected/textured node can no longer crash.

	// Arm 1: break input pins on every other expression that point at Expr
	// (mirrors BreakLinksToExpression; uses the same GetInput(i) walk as the
	// dry-run cascade above).
	for (UMaterialExpression* Other : Material->GetExpressions())
	{
		if (!Other || Other == Expr) continue;
		for (int32 i = 0; ; ++i)
		{
			FExpressionInput* In = Other->GetInput(i);
			if (!In) break;
			if (In->Expression == Expr) In->Expression = nullptr;
		}
	}

	// Arm 2: clear the material's own attribute slots (BaseColor, Normal, …).
	// GetExpressionInputForProperty over MP_MAX is the engine's canonical sweep.
	for (int32 InputIndex = 0; InputIndex < MP_MAX; ++InputIndex)
	{
		FExpressionInput* Input = Material->GetExpressionInputForProperty((EMaterialProperty)InputIndex);
		if (Input && Input->Expression == Expr) Input->Expression = nullptr;
	}

	// Drop any parameter bookkeeping for this node (matches the engine path),
	// then remove it from the collection and retire the object.
	Material->RemoveExpressionParameter(Expr);
	Material->GetExpressionCollection().RemoveExpression(Expr);

	// CRASH FIX: MarkAsGarbage() hard-asserts check(!IsRooted())
	// (UObjectBaseUtility.h:184). The engine's UMaterialEditingLibrary::
	// DeleteMaterialExpression calls MarkAsGarbage() unconditionally because a
	// material expression is never in the GC root set — but ours WAS once (it took
	// the whole editor down with `Assertion failed: !IsRooted()`). Unroot it first:
	// by this point the node is already fully dereferenced (input links severed on
	// every sibling and every MP_MAX material attribute slot above, and removed from
	// the expression collection), so normal GC reclaims it whether or not we expedite
	// it with MarkAsGarbage — but the unconditional mark can crash, so guard it.
	if (Expr->IsRooted())
	{
		Expr->RemoveFromRoot();
	}
	Expr->MarkAsGarbage();

	// Propagate to dependent material instances (see RecompileMaterialWithDependents).
	FMCPCommonUtils::RecompileMaterialWithDependents(Material);
	// GAP-062: persist by default so the deletion survives an editor reload.
	if (TSharedPtr<FJsonObject> SaveErr = FMCPCommonUtils::SaveMaterialOrError(Material))
		return SaveErr;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("deleted"), ExprId);
	Data->SetBoolField(TEXT("success"), true);
	return Data;
}

// ---------------------------------------------------------------------------
// SetExpressionProperty
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMaterialExpressionManager::SetExpressionProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'material_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`material_path` is required and must be the full asset path to a UMaterial, e.g. `/Game/Materials/M_Foo`. Use `list_assets` with `asset_type='Material'` to discover."));

	FString ExprId;
	if (!Params->TryGetStringField(TEXT("expression_name"), ExprId))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'expression_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`expression_name` is required — pass the node's `GetName()` value. Use `read_material_graph` to list expression names for this material."));

	UMaterial* Material = FMCPMaterialCommands::FindMaterialByPath(MaterialPath);
	if (!Material)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`material_path` did not resolve to a UMaterial. Verify the asset exists and the path is the full `/Game/...` form. Use `list_assets` with `asset_type='Material'` to discover."));

	UMaterialExpression* Expr = FindExpression(Material, ExprId);
	if (!Expr)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Expression not found: %s"), *ExprId),
			EMCPErrorCode::NodeNotFound,
			TEXT("`expression_name` did not match any node in this material. Use `read_material_graph` to see current expression names — they are GUID-suffixed engine names (e.g. `MaterialExpressionMultiply_0`)."));

	// dry_run: every preflight ran (paths, expression lookup). The remaining
	// work — Material->PreEditChange + the per-branch UPROPERTY writes — is the
	// side effect we skip. Diff shape per todo/13 phase 3 + mirroring
	// set_node_property's legacy mode (Phase 2e): properties_changed[] with one
	// entry per (expression, field, before, after). The dispatch tree here is a
	// mirror of the apply tree below but with capture-instead-of-write — every
	// type-cast branch the apply branch has gets a parallel dry-run branch.
	//
	// no-op suppression: AddChange skips entries where before == after.
	// Apply-time still does the redundant write (consistent with original
	// behavior), but the diff is more useful when it shows ONLY actual deltas.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TArray<TSharedPtr<FJsonValue>> Changes;
		auto AddChange = [&](const FString& Name, const FString& Before, const FString& After)
		{
			if (Before == After) return;
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("expression_name"), Expr->GetName());
			O->SetStringField(TEXT("property_name"), Name);
			O->SetStringField(TEXT("before"), Before);
			O->SetStringField(TEXT("after"), After);
			Changes.Add(MakeShared<FJsonValueObject>(O));
		};

		// Position + description (common to all expressions)
		if (Params->HasField(TEXT("pos_x")))
		{
			AddChange(TEXT("pos_x"),
				FString::FromInt(Expr->MaterialExpressionEditorX),
				FString::FromInt(static_cast<int32>(Params->GetNumberField(TEXT("pos_x")))));
		}
		if (Params->HasField(TEXT("pos_y")))
		{
			AddChange(TEXT("pos_y"),
				FString::FromInt(Expr->MaterialExpressionEditorY),
				FString::FromInt(static_cast<int32>(Params->GetNumberField(TEXT("pos_y")))));
		}
		if (Params->HasField(TEXT("desc")))
		{
			AddChange(TEXT("desc"), Expr->Desc, Params->GetStringField(TEXT("desc")));
		}

		// TextureSample - texture_path (alias: texture, the reader-emitted key).
		// Apply branch silently no-ops if the asset doesn't load; mirror that
		// here so the dry-run delta matches apply behavior exactly (a
		// non-loading texture_path is a no-op in both).
		if (auto* TexSample = Cast<UMaterialExpressionTextureSample>(Expr))
		{
			FString TexturePath;
			if (Params->TryGetStringField(TEXT("texture_path"), TexturePath) ||
			    Params->TryGetStringField(TEXT("texture"), TexturePath))
			{
				UTexture* NewTex = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexturePath));
				if (NewTex)
				{
					const FString Before = TexSample->Texture
						? TexSample->Texture->GetPathName()
						: FString(TEXT("None"));
					AddChange(TEXT("texture_path"), Before, NewTex->GetPathName());
				}
			}
		}

		// TextureCoordinate - u_tiling, v_tiling, coordinate_index
		if (auto* TexCoord = Cast<UMaterialExpressionTextureCoordinate>(Expr))
		{
			if (Params->HasField(TEXT("u_tiling")))
			{
				AddChange(TEXT("u_tiling"),
					FString::SanitizeFloat(TexCoord->UTiling),
					FString::SanitizeFloat(Params->GetNumberField(TEXT("u_tiling"))));
			}
			if (Params->HasField(TEXT("v_tiling")))
			{
				AddChange(TEXT("v_tiling"),
					FString::SanitizeFloat(TexCoord->VTiling),
					FString::SanitizeFloat(Params->GetNumberField(TEXT("v_tiling"))));
			}
			if (Params->HasField(TEXT("coordinate_index")))
			{
				AddChange(TEXT("coordinate_index"),
					FString::FromInt(TexCoord->CoordinateIndex),
					FString::FromInt(static_cast<int32>(Params->GetNumberField(TEXT("coordinate_index")))));
			}
		}

		// ComponentMask - r/g/b/a (booleans encode to 1/0)
		if (auto* Mask = Cast<UMaterialExpressionComponentMask>(Expr))
		{
			auto BoolField = [&](const TCHAR* Name, uint32 Current)
			{
				if (Params->HasField(Name))
				{
					AddChange(Name,
						FString::FromInt(Current),
						FString::FromInt(Params->GetBoolField(Name) ? 1 : 0));
				}
			};
			BoolField(TEXT("r"), Mask->R);
			BoolField(TEXT("g"), Mask->G);
			BoolField(TEXT("b"), Mask->B);
			BoolField(TEXT("a"), Mask->A);
		}

		// Constant - value
		if (auto* Const = Cast<UMaterialExpressionConstant>(Expr))
		{
			if (Params->HasField(TEXT("value")))
			{
				AddChange(TEXT("value"),
					FString::SanitizeFloat(Const->R),
					FString::SanitizeFloat(Params->GetNumberField(TEXT("value"))));
			}
		}

		// ScalarParameter - parameter_name, default_value
		if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
		{
			FString ParamName;
			if (Params->TryGetStringField(TEXT("parameter_name"), ParamName))
			{
				AddChange(TEXT("parameter_name"), SP->ParameterName.ToString(), ParamName);
			}
			if (Params->HasField(TEXT("default_value")))
			{
				AddChange(TEXT("default_value"),
					FString::SanitizeFloat(SP->DefaultValue),
					FString::SanitizeFloat(Params->GetNumberField(TEXT("default_value"))));
			}
		}

		// Constant3Vector - r/g/b → Constant FLinearColor (a is hidden)
		if (auto* C3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
		{
			FLinearColor NewColor;
			if (TryBuildLinearColorFromJson(Params, /*bReadAlpha*/false, nullptr,
					FLinearColor(0.f, 0.f, 0.f, 1.f), NewColor))
			{
				AddChange(TEXT("constant"), C3->Constant.ToString(), NewColor.ToString());
			}
		}

		// Constant4Vector - r/g/b/a → Constant FLinearColor
		if (auto* C4 = Cast<UMaterialExpressionConstant4Vector>(Expr))
		{
			FLinearColor NewColor;
			if (TryBuildLinearColorFromJson(Params, /*bReadAlpha*/true, nullptr,
					FLinearColor(0.f, 0.f, 0.f, 1.f), NewColor))
			{
				AddChange(TEXT("constant"), C4->Constant.ToString(), NewColor.ToString());
			}
		}

		// VectorParameter - parameter_name + r/g/b/a or default_r/... (back-compat)
		if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
		{
			FString ParamName;
			if (Params->TryGetStringField(TEXT("parameter_name"), ParamName))
			{
				AddChange(TEXT("parameter_name"), VP->ParameterName.ToString(), ParamName);
			}

			FLinearColor NewColor;
			if (TryBuildLinearColorFromJson(Params, /*bReadAlpha*/true, TEXT("default_"),
					FLinearColor(0.f, 0.f, 0.f, 1.f), NewColor))
			{
				AddChange(TEXT("default_value"), VP->DefaultValue.ToString(), NewColor.ToString());
			}
		}

		// TextureSampleParameter2D - parameter_name (texture_path handled by
		// the TextureSample branch above; TSP2D IsA TextureSample)
		if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
		{
			FString ParamName;
			if (Params->TryGetStringField(TEXT("parameter_name"), ParamName))
			{
				AddChange(TEXT("parameter_name"), TP->ParameterName.ToString(), ParamName);
			}
		}

		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("properties_changed"), Changes);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// Open edit envelope before the per-expression property writes below.
	// Engine pattern: UMaterialEditingLibrary::RecompileMaterialInternal
	// (/ue5-source/Editor/MaterialEditor/Private/MaterialEditingLibrary.cpp:717-720).
	// Validate-before-mutate posture: Material + expression lookups fire
	// before the envelope opens, so no close-on-error is required. The
	// property sets below are infallible sub-object field writes on an
	// expression that is already owned by the Material.
	Material->PreEditChange(nullptr);

	// Position
	if (Params->HasField(TEXT("pos_x")))
		Expr->MaterialExpressionEditorX = static_cast<int32>(Params->GetNumberField(TEXT("pos_x")));
	if (Params->HasField(TEXT("pos_y")))
		Expr->MaterialExpressionEditorY = static_cast<int32>(Params->GetNumberField(TEXT("pos_y")));
	if (Params->HasField(TEXT("desc")))
		Expr->Desc = Params->GetStringField(TEXT("desc"));

	// TextureSample — update texture reference. `texture_path` is
	// setter-canonical; `texture` is the key ExpressionToJson emits, accepted
	// as a read→write round-trip alias (texture_path wins when both supplied).
	if (auto* TexSample = Cast<UMaterialExpressionTextureSample>(Expr))
	{
		FString TexturePath;
		if (Params->TryGetStringField(TEXT("texture_path"), TexturePath) ||
		    Params->TryGetStringField(TEXT("texture"), TexturePath))
		{
			UTexture* Tex = Cast<UTexture>(UEditorAssetLibrary::LoadAsset(TexturePath));
			if (Tex) { TexSample->Texture = Tex; TexSample->AutoSetSampleType(); }
		}
	}

	// TextureCoordinate — update tiling + UV channel. coordinate_index matches
	// the AddExpression branch and the reader's emission, closing the
	// setter-only gap tracked in the #DEFERRED entry.
	if (auto* TexCoord = Cast<UMaterialExpressionTextureCoordinate>(Expr))
	{
		if (Params->HasField(TEXT("u_tiling"))) TexCoord->UTiling = Params->GetNumberField(TEXT("u_tiling"));
		if (Params->HasField(TEXT("v_tiling"))) TexCoord->VTiling = Params->GetNumberField(TEXT("v_tiling"));
		if (Params->HasField(TEXT("coordinate_index")))
			TexCoord->CoordinateIndex = static_cast<int32>(Params->GetNumberField(TEXT("coordinate_index")));
	}

	// ComponentMask — update mask channels
	if (auto* Mask = Cast<UMaterialExpressionComponentMask>(Expr))
	{
		if (Params->HasField(TEXT("r"))) Mask->R = Params->GetBoolField(TEXT("r")) ? 1 : 0;
		if (Params->HasField(TEXT("g"))) Mask->G = Params->GetBoolField(TEXT("g")) ? 1 : 0;
		if (Params->HasField(TEXT("b"))) Mask->B = Params->GetBoolField(TEXT("b")) ? 1 : 0;
		if (Params->HasField(TEXT("a"))) Mask->A = Params->GetBoolField(TEXT("a")) ? 1 : 0;
	}

	// Constant
	if (auto* Const = Cast<UMaterialExpressionConstant>(Expr))
	{
		if (Params->HasField(TEXT("value"))) Const->R = Params->GetNumberField(TEXT("value"));
	}

	// ScalarParameter
	if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
	{
		FString ParamName;
		if (Params->TryGetStringField(TEXT("parameter_name"), ParamName))
			SP->ParameterName = FName(*ParamName);
		if (Params->HasField(TEXT("default_value")))
			SP->DefaultValue = Params->GetNumberField(TEXT("default_value"));
	}

	// Constant3Vector — gate on `r`, zero-fill missing g/b. Mirrors the
	// AddExpression branch's partial-spec semantic so a read→edit→set
	// workflow lands the modified components. Constant3Vector's alpha is
	// hidden in the editor (UPROPERTY meta=(HideAlphaChannel)), and the
	// reader emits only r/g/b — so the setter does not read `a` either,
	// matching the read shape exactly.
	if (auto* C3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
	{
		TryBuildLinearColorFromJson(Params, /*bReadAlpha*/false, nullptr,
			FLinearColor(0.f, 0.f, 0.f, 1.f), C3->Constant);
	}

	// Constant4Vector — gate on `r`, zero-fill missing g/b, default a=1.0.
	if (auto* C4 = Cast<UMaterialExpressionConstant4Vector>(Expr))
	{
		TryBuildLinearColorFromJson(Params, /*bReadAlpha*/true, nullptr,
			FLinearColor(0.f, 0.f, 0.f, 1.f), C4->Constant);
	}

	// VectorParameter — accepts parameter_name and either `r/g/b/a` (legacy)
	// or `default_r/g/b/a` (reader-canonical); short keys win when both are
	// supplied. Same back-compat shim as the AddExpression branch.
	if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
	{
		FString ParamName;
		if (Params->TryGetStringField(TEXT("parameter_name"), ParamName))
			VP->ParameterName = FName(*ParamName);

		TryBuildLinearColorFromJson(Params, /*bReadAlpha*/true, TEXT("default_"),
			FLinearColor(0.f, 0.f, 0.f, 1.f), VP->DefaultValue);
	}

	// TextureSampleParameter2D — parameter_name. The texture_path field is
	// already handled by the TextureSample branch above (TextureSampleParameter2D
	// IsA TextureSample), so only parameter_name needs its own cast. Mirrors
	// the AddExpression branch and the reader's parameter_name emission at
	// :685-688.
	if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
	{
		FString ParamName;
		if (Params->TryGetStringField(TEXT("parameter_name"), ParamName))
			TP->ParameterName = FName(*ParamName);
	}

	// GAP-052: Custom HLSL nodes were only configurable at creation — editing the
	// code/output_type/inputs meant deleting and re-adding the node (which renamed it
	// and dropped its wires). Mirror the AddExpression Custom branch here so a Custom
	// node's body can be tuned in place. Only fields that are present are changed.
	if (auto* CustomExpr = Cast<UMaterialExpressionCustom>(Expr))
	{
		FString Code;
		if (Params->TryGetStringField(TEXT("code"), Code))
			CustomExpr->Code = Code;

		FString OutputType;
		if (Params->TryGetStringField(TEXT("output_type"), OutputType))
		{
			if (OutputType.Equals(TEXT("Float1"), ESearchCase::IgnoreCase) ||
			    OutputType.Equals(TEXT("CMOT_Float1"), ESearchCase::IgnoreCase))
				CustomExpr->OutputType = CMOT_Float1;
			else if (OutputType.Equals(TEXT("Float2"), ESearchCase::IgnoreCase) ||
			         OutputType.Equals(TEXT("CMOT_Float2"), ESearchCase::IgnoreCase))
				CustomExpr->OutputType = CMOT_Float2;
			else if (OutputType.Equals(TEXT("Float3"), ESearchCase::IgnoreCase) ||
			         OutputType.Equals(TEXT("CMOT_Float3"), ESearchCase::IgnoreCase))
				CustomExpr->OutputType = CMOT_Float3;
			else if (OutputType.Equals(TEXT("Float4"), ESearchCase::IgnoreCase) ||
			         OutputType.Equals(TEXT("CMOT_Float4"), ESearchCase::IgnoreCase))
				CustomExpr->OutputType = CMOT_Float4;
		}

		const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
		if (Params->TryGetArrayField(TEXT("inputs"), InputsArray))
		{
			CustomExpr->Inputs.Empty();
			for (const auto& InputVal : *InputsArray)
			{
				const TSharedPtr<FJsonObject>& InputObj = InputVal->AsObject();
				if (!InputObj) continue;
				FCustomInput NewInput;
				FString InputName;
				if (InputObj->TryGetStringField(TEXT("name"), InputName))
					NewInput.InputName = FName(*InputName);
				CustomExpr->Inputs.Add(NewInput);
			}
			// Rebuild the node's input pins to reflect the new Inputs array. The
			// expression's own PostEditChange re-derives pins from Inputs (the
			// material-wide PostEditChange below handles recompile/render-state).
			CustomExpr->PostEditChange();
		}
	}

	// Propagate to dependent material instances (see RecompileMaterialWithDependents).
	FMCPCommonUtils::RecompileMaterialWithDependents(Material);
	// GAP-062: persist by default so the property edit survives an editor reload.
	if (TSharedPtr<FJsonObject> SaveErr = FMCPCommonUtils::SaveMaterialOrError(Material))
		return SaveErr;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("expression_name"), Expr->GetName());
	Data->SetBoolField(TEXT("success"), true);
	return Data;
}

// ---------------------------------------------------------------------------
// ExpressionToJson
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMaterialExpressionManager::ExpressionToJson(
	UMaterialExpression* Expr, int32 Index)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("index"), Index);
	Obj->SetStringField(TEXT("name"), Expr->GetName());
	Obj->SetStringField(TEXT("desc"), Expr->Desc);

	// Strip "MaterialExpression" prefix for readability
	FString ClassName = Expr->GetClass()->GetName();
	ClassName.RemoveFromStart(TEXT("MaterialExpression"));
	Obj->SetStringField(TEXT("type"), ClassName);

	Obj->SetNumberField(TEXT("pos_x"), Expr->MaterialExpressionEditorX);
	Obj->SetNumberField(TEXT("pos_y"), Expr->MaterialExpressionEditorY);

	// Type-specific properties
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	if (auto* TS = Cast<UMaterialExpressionTextureSample>(Expr))
	{
		Props->SetStringField(TEXT("texture"), TS->Texture ? TS->Texture->GetPathName() : TEXT("None"));
	}
	if (auto* TC = Cast<UMaterialExpressionTextureCoordinate>(Expr))
	{
		Props->SetNumberField(TEXT("u_tiling"), TC->UTiling);
		Props->SetNumberField(TEXT("v_tiling"), TC->VTiling);
		Props->SetNumberField(TEXT("coordinate_index"), TC->CoordinateIndex);
	}
	if (auto* CM = Cast<UMaterialExpressionComponentMask>(Expr))
	{
		Props->SetBoolField(TEXT("r"), CM->R != 0);
		Props->SetBoolField(TEXT("g"), CM->G != 0);
		Props->SetBoolField(TEXT("b"), CM->B != 0);
		Props->SetBoolField(TEXT("a"), CM->A != 0);
	}
	if (auto* C = Cast<UMaterialExpressionConstant>(Expr))
	{
		Props->SetNumberField(TEXT("value"), C->R);
	}
	if (auto* C3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
	{
		Props->SetNumberField(TEXT("r"), C3->Constant.R);
		Props->SetNumberField(TEXT("g"), C3->Constant.G);
		Props->SetNumberField(TEXT("b"), C3->Constant.B);
	}
	if (auto* C4 = Cast<UMaterialExpressionConstant4Vector>(Expr))
	{
		Props->SetNumberField(TEXT("r"), C4->Constant.R);
		Props->SetNumberField(TEXT("g"), C4->Constant.G);
		Props->SetNumberField(TEXT("b"), C4->Constant.B);
		Props->SetNumberField(TEXT("a"), C4->Constant.A);
	}
	if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
	{
		Props->SetStringField(TEXT("parameter_name"), SP->ParameterName.ToString());
		Props->SetNumberField(TEXT("default_value"), SP->DefaultValue);
	}
	if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
	{
		Props->SetStringField(TEXT("parameter_name"), VP->ParameterName.ToString());
		Props->SetNumberField(TEXT("default_r"), VP->DefaultValue.R);
		Props->SetNumberField(TEXT("default_g"), VP->DefaultValue.G);
		Props->SetNumberField(TEXT("default_b"), VP->DefaultValue.B);
		Props->SetNumberField(TEXT("default_a"), VP->DefaultValue.A);
	}
	if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
	{
		Props->SetStringField(TEXT("parameter_name"), TP->ParameterName.ToString());
		// Expose the default texture so we can see what fallback the parameter holds.
		Props->SetStringField(TEXT("texture"), TP->Texture ? TP->Texture->GetPathName() : TEXT("None"));
	}
	// Static switches — parameterized (overridable on MaterialInstance) and non-parameterized.
	if (auto* SSP = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
	{
		Props->SetStringField(TEXT("parameter_name"), SSP->ParameterName.ToString());
		Props->SetBoolField(TEXT("default_value"), SSP->DefaultValue);
	}
	if (auto* SBP = Cast<UMaterialExpressionStaticBoolParameter>(Expr))
	{
		Props->SetStringField(TEXT("parameter_name"), SBP->ParameterName.ToString());
		Props->SetBoolField(TEXT("default_value"), SBP->DefaultValue);
	}
	if (auto* SB = Cast<UMaterialExpressionStaticBool>(Expr))
	{
		Props->SetBoolField(TEXT("value"), SB->Value);
	}
	if (auto* SS = Cast<UMaterialExpressionStaticSwitch>(Expr))
	{
		Props->SetBoolField(TEXT("default_value"), SS->DefaultValue);
	}
	// Function-graph plumbing — without input/output names we cannot map a
	// MaterialFunctionCall's pins back to the function's interface.
	if (auto* FI = Cast<UMaterialExpressionFunctionInput>(Expr))
	{
		Props->SetStringField(TEXT("input_name"), FI->InputName.ToString());
		Props->SetNumberField(TEXT("input_type"), static_cast<int32>(FI->InputType));
		Props->SetNumberField(TEXT("sort_priority"), FI->SortPriority);
		Props->SetStringField(TEXT("description"), FI->Description);
		Props->SetBoolField(TEXT("use_preview_value_as_default"), FI->bUsePreviewValueAsDefault);
		Props->SetNumberField(TEXT("preview_x"), FI->PreviewValue.X);
		Props->SetNumberField(TEXT("preview_y"), FI->PreviewValue.Y);
		Props->SetNumberField(TEXT("preview_z"), FI->PreviewValue.Z);
		Props->SetNumberField(TEXT("preview_w"), FI->PreviewValue.W);
	}
	if (auto* FO = Cast<UMaterialExpressionFunctionOutput>(Expr))
	{
		Props->SetStringField(TEXT("output_name"), FO->OutputName.ToString());
		Props->SetNumberField(TEXT("sort_priority"), FO->SortPriority);
		Props->SetStringField(TEXT("description"), FO->Description);
	}
	// Named reroutes — declaration carries the name; usage carries the link.
	if (auto* NRD = Cast<UMaterialExpressionNamedRerouteDeclaration>(Expr))
	{
		Props->SetStringField(TEXT("name"), NRD->Name.ToString());
		Props->SetNumberField(TEXT("node_color_r"), NRD->NodeColor.R);
		Props->SetNumberField(TEXT("node_color_g"), NRD->NodeColor.G);
		Props->SetNumberField(TEXT("node_color_b"), NRD->NodeColor.B);
		Props->SetStringField(TEXT("variable_guid"), NRD->VariableGuid.ToString());
	}
	if (auto* NRU = Cast<UMaterialExpressionNamedRerouteUsage>(Expr))
	{
		Props->SetStringField(TEXT("declaration_name"),
			NRU->Declaration ? NRU->Declaration->Name.ToString() : TEXT(""));
		Props->SetStringField(TEXT("declaration_guid"), NRU->DeclarationGuid.ToString());
	}
	// Landscape helpers carrying paint-layer identity.
	if (auto* LLW = Cast<UMaterialExpressionLandscapeLayerWeight>(Expr))
	{
		Props->SetStringField(TEXT("parameter_name"), LLW->ParameterName.ToString());
		Props->SetNumberField(TEXT("preview_weight"), LLW->PreviewWeight);
		Props->SetNumberField(TEXT("const_base_r"), LLW->ConstBase.X);
		Props->SetNumberField(TEXT("const_base_g"), LLW->ConstBase.Y);
		Props->SetNumberField(TEXT("const_base_b"), LLW->ConstBase.Z);
	}
	if (auto* LGO = Cast<UMaterialExpressionLandscapeGrassOutput>(Expr))
	{
		TArray<TSharedPtr<FJsonValue>> GrassArr;
		for (const FGrassInput& G : LGO->GrassTypes)
		{
			TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
			GObj->SetStringField(TEXT("layer_name"), G.Name.ToString());
			GObj->SetStringField(TEXT("grass_type"), G.GrassType ? G.GrassType->GetPathName() : TEXT("None"));
			GrassArr.Add(MakeShared<FJsonValueObject>(GObj));
		}
		Props->SetArrayField(TEXT("grass_types"), GrassArr);
	}
	// Texture-object variants (used as inputs to TextureSample, not samplers themselves).
	if (auto* TO = Cast<UMaterialExpressionTextureObject>(Expr))
	{
		Props->SetStringField(TEXT("texture"), TO->Texture ? TO->Texture->GetPathName() : TEXT("None"));
	}
	if (auto* TOP = Cast<UMaterialExpressionTextureObjectParameter>(Expr))
	{
		Props->SetStringField(TEXT("parameter_name"), TOP->ParameterName.ToString());
		Props->SetStringField(TEXT("texture"), TOP->Texture ? TOP->Texture->GetPathName() : TEXT("None"));
	}
	// Material Parameter Collection lookup — name + which collection.
	if (auto* CP = Cast<UMaterialExpressionCollectionParameter>(Expr))
	{
		Props->SetStringField(TEXT("parameter_name"), CP->ParameterName.ToString());
		Props->SetStringField(TEXT("collection"),
			CP->Collection ? CP->Collection->GetPathName() : TEXT("None"));
		Props->SetStringField(TEXT("parameter_id"), CP->ParameterId.ToString());
	}
	// Runtime virtual texture sampler parameter.
	if (auto* RVT = Cast<UMaterialExpressionRuntimeVirtualTextureSampleParameter>(Expr))
	{
		Props->SetStringField(TEXT("parameter_name"), RVT->ParameterName.ToString());
		Props->SetStringField(TEXT("virtual_texture"),
			RVT->VirtualTexture ? RVT->VirtualTexture->GetPathName() : TEXT("None"));
	}
	// Math nodes — when an input pin is unwired, ConstA/B/Alpha provides the
	// runtime value. Surfacing these lets us reason about effective defaults.
	if (auto* M = Cast<UMaterialExpressionMultiply>(Expr))
	{
		Props->SetNumberField(TEXT("const_a"), M->ConstA);
		Props->SetNumberField(TEXT("const_b"), M->ConstB);
	}
	if (auto* A = Cast<UMaterialExpressionAdd>(Expr))
	{
		Props->SetNumberField(TEXT("const_a"), A->ConstA);
		Props->SetNumberField(TEXT("const_b"), A->ConstB);
	}
	if (auto* S = Cast<UMaterialExpressionSubtract>(Expr))
	{
		Props->SetNumberField(TEXT("const_a"), S->ConstA);
		Props->SetNumberField(TEXT("const_b"), S->ConstB);
	}
	if (auto* D = Cast<UMaterialExpressionDivide>(Expr))
	{
		Props->SetNumberField(TEXT("const_a"), D->ConstA);
		Props->SetNumberField(TEXT("const_b"), D->ConstB);
	}
	if (auto* LI = Cast<UMaterialExpressionLinearInterpolate>(Expr))
	{
		Props->SetNumberField(TEXT("const_a"), LI->ConstA);
		Props->SetNumberField(TEXT("const_b"), LI->ConstB);
		Props->SetNumberField(TEXT("const_alpha"), LI->ConstAlpha);
	}
	if (auto* MFC = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
	{
		if (MFC->MaterialFunction)
		{
			Props->SetStringField(TEXT("function_path"), MFC->MaterialFunction->GetPathName());
			Props->SetStringField(TEXT("function_name"), MFC->MaterialFunction->GetName());
		}
		else
		{
			Props->SetStringField(TEXT("function_path"), TEXT(""));
			Props->SetStringField(TEXT("function_name"), TEXT(""));
		}
	}
	if (auto* LLB = Cast<UMaterialExpressionLandscapeLayerBlend>(Expr))
	{
		TArray<TSharedPtr<FJsonValue>> LayersArr;
		for (const FLayerBlendInput& L : LLB->Layers)
		{
			TSharedPtr<FJsonObject> LObj = MakeShared<FJsonObject>();
			LObj->SetStringField(TEXT("name"), L.LayerName.ToString());
			LObj->SetNumberField(TEXT("preview_weight"), L.PreviewWeight);
			FString BT = (L.BlendType == LB_HeightBlend) ? TEXT("HeightBlend")
				: (L.BlendType == LB_AlphaBlend) ? TEXT("AlphaBlend") : TEXT("WeightBlend");
			LObj->SetStringField(TEXT("blend_type"), BT);
			LayersArr.Add(MakeShared<FJsonValueObject>(LObj));
		}
		Props->SetArrayField(TEXT("layers"), LayersArr);
	}
	// Custom HLSL — round-trips with material_add_expression / material_set_expression_property
	// (GAP-063). `Code`, `OutputType`, and `Inputs` are direct UPROPERTYs holding the node's real
	// logic; without these a Custom node read back as an empty {} (write-only black box). Field
	// names + value forms mirror the writer (`code`, `output_type` as "Float1".."Float4", `inputs`
	// as [{name}]) so a read can be edited and pushed straight back.
	if (auto* CU = Cast<UMaterialExpressionCustom>(Expr))
	{
		Props->SetStringField(TEXT("code"), CU->Code);
		const TCHAR* OutType =
			(CU->OutputType == CMOT_Float1) ? TEXT("Float1")
			: (CU->OutputType == CMOT_Float2) ? TEXT("Float2")
			: (CU->OutputType == CMOT_Float3) ? TEXT("Float3")
			: (CU->OutputType == CMOT_Float4) ? TEXT("Float4")
			: TEXT("Float3"); // engine default (CMOT_Float3)
		Props->SetStringField(TEXT("output_type"), OutType);
		TArray<TSharedPtr<FJsonValue>> InputsArr;
		for (const FCustomInput& In : CU->Inputs)
		{
			TSharedPtr<FJsonObject> InObj = MakeShared<FJsonObject>();
			InObj->SetStringField(TEXT("name"), In.InputName.ToString());
			InputsArr.Add(MakeShared<FJsonValueObject>(InObj));
		}
		Props->SetArrayField(TEXT("inputs"), InputsArr);
	}
	// SceneTexture — emit the selected buffer + filtering so a read round-trips back to
	// material_add_expression (scene_texture_id as the display-name string, filtered as bool).
	if (auto* ST = Cast<UMaterialExpressionSceneTexture>(Expr))
	{
		const TCHAR* IdName =
			(ST->SceneTextureId == PPI_PostProcessInput0) ? TEXT("PostProcessInput0")
			: (ST->SceneTextureId == PPI_SceneColor)      ? TEXT("SceneColor")
			: (ST->SceneTextureId == PPI_SceneDepth)      ? TEXT("SceneDepth")
			: (ST->SceneTextureId == PPI_Velocity)        ? TEXT("Velocity")
			: (ST->SceneTextureId == PPI_WorldNormal)     ? TEXT("WorldNormal")
			: (ST->SceneTextureId == PPI_CustomDepth)     ? TEXT("CustomDepth")
			: (ST->SceneTextureId == PPI_CustomStencil)   ? TEXT("CustomStencil")
			: (ST->SceneTextureId == PPI_BaseColor)       ? TEXT("BaseColor")
			: (ST->SceneTextureId == PPI_WorldTangent)    ? TEXT("WorldTangent")
			: (ST->SceneTextureId == PPI_AmbientOcclusion)? TEXT("AmbientOcclusion")
			: TEXT("PostProcessInput0");
		Props->SetStringField(TEXT("scene_texture_id"), IdName);
		Props->SetBoolField(TEXT("filtered"), ST->bFiltered);
	}
	Obj->SetObjectField(TEXT("properties"), Props);

	// Inputs
	TArray<TSharedPtr<FJsonValue>> InputsArr;
	for (int32 i = 0; ; ++i)
	{
		FExpressionInput* In = Expr->GetInput(i);
		if (!In) break;
		TSharedPtr<FJsonObject> InObj = MakeShared<FJsonObject>();
		InObj->SetNumberField(TEXT("index"), i);
		InObj->SetStringField(TEXT("name"), Expr->GetInputName(i).ToString());
		if (In->Expression)
		{
			InObj->SetStringField(TEXT("connected_expression"), In->Expression->GetName());
			InObj->SetNumberField(TEXT("connected_output_index"), In->OutputIndex);
		}
		InputsArr.Add(MakeShared<FJsonValueObject>(InObj));
	}
	Obj->SetArrayField(TEXT("inputs"), InputsArr);

	// Outputs (names + count)
	const TArray<FExpressionOutput>& Outputs = Expr->GetOutputs();
	Obj->SetNumberField(TEXT("output_count"), Outputs.Num());
	TArray<TSharedPtr<FJsonValue>> OutputsArr;
	for (int32 i = 0; i < Outputs.Num(); ++i)
	{
		TSharedPtr<FJsonObject> OutObj = MakeShared<FJsonObject>();
		OutObj->SetNumberField(TEXT("index"), i);
		OutObj->SetStringField(TEXT("name"), Outputs[i].OutputName.ToString());
		OutputsArr.Add(MakeShared<FJsonValueObject>(OutObj));
	}
	Obj->SetArrayField(TEXT("outputs"), OutputsArr);

	return Obj;
}

// ---------------------------------------------------------------------------
// ReadMaterialGraph
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMaterialExpressionManager::ReadMaterialGraph(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'material_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`material_path` is required and must be the full asset path to a UMaterial, e.g. `/Game/Materials/M_Foo`. Use `list_assets` with `asset_type='Material'` to discover."));

	UMaterial* Material = FMCPMaterialCommands::FindMaterialByPath(MaterialPath);
	if (!Material)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material not found: %s"), *MaterialPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`material_path` did not resolve to a UMaterial. Verify the asset exists and the path is the full `/Game/...` form. Use `list_assets` with `asset_type='Material'` to discover."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("material_name"), Material->GetName());
	Result->SetStringField(TEXT("material_path"), Material->GetPathName());
	Result->SetNumberField(TEXT("blend_mode"), static_cast<int32>(Material->BlendMode));
	Result->SetBoolField(TEXT("two_sided"), Material->IsTwoSided());
	Result->SetNumberField(TEXT("material_domain"), static_cast<int32>(Material->MaterialDomain));
	Result->SetNumberField(TEXT("shading_model"), static_cast<int32>(Material->GetShadingModels().GetFirstShadingModel()));

	// Expressions
	auto& Exprs = Material->GetExpressionCollection().Expressions;
	TArray<TSharedPtr<FJsonValue>> ExprArr;
	for (int32 i = 0; i < Exprs.Num(); ++i)
	{
		if (Exprs[i])
			ExprArr.Add(MakeShared<FJsonValueObject>(ExpressionToJson(Exprs[i], i)));
	}
	Result->SetArrayField(TEXT("expressions"), ExprArr);

	// Material input connections
	TSharedPtr<FJsonObject> MatInputs = MakeShared<FJsonObject>();
	struct FNamedInput { const TCHAR* Name; FExpressionInput* Input; };

	// In UE5.5 these are on the material's editor-only data
	UMaterialEditorOnlyData* EdData = Material->GetEditorOnlyData();
	if (EdData)
	{
		TArray<FNamedInput> Inputs = {
			{ TEXT("BaseColor"),          &EdData->BaseColor },
			{ TEXT("Metallic"),           &EdData->Metallic },
			{ TEXT("Specular"),           &EdData->Specular },
			{ TEXT("Roughness"),          &EdData->Roughness },
			{ TEXT("Anisotropy"),         &EdData->Anisotropy },
			{ TEXT("Normal"),             &EdData->Normal },
			{ TEXT("Tangent"),            &EdData->Tangent },
			{ TEXT("EmissiveColor"),      &EdData->EmissiveColor },
			{ TEXT("Opacity"),            &EdData->Opacity },
			{ TEXT("OpacityMask"),        &EdData->OpacityMask },
			{ TEXT("WorldPositionOffset"),&EdData->WorldPositionOffset },
			{ TEXT("SubsurfaceColor"),    &EdData->SubsurfaceColor },
			{ TEXT("AmbientOcclusion"),   &EdData->AmbientOcclusion },
			{ TEXT("Refraction"),         &EdData->Refraction },
			{ TEXT("PixelDepthOffset"),   &EdData->PixelDepthOffset },
		};

		for (const auto& NI : Inputs)
		{
			if (NI.Input->Expression)
			{
				TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
				ConnObj->SetStringField(TEXT("connected_expression"), NI.Input->Expression->GetName());
				ConnObj->SetNumberField(TEXT("connected_output_index"), NI.Input->OutputIndex);
				MatInputs->SetObjectField(NI.Name, ConnObj);
			}
		}
	}
	Result->SetObjectField(TEXT("material_inputs"), MatInputs);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ---------------------------------------------------------------------------
// ReadMaterialFunction
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMaterialExpressionManager::ReadMaterialFunction(
	const TSharedPtr<FJsonObject>& Params)
{
	FString FunctionPath;
	if (!Params->TryGetStringField(TEXT("function_path"), FunctionPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'function_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`function_path` is required and must be the full asset path to a UMaterialFunction, e.g. `/Game/Materials/Functions/MF_Foo`. Use `list_assets` with `asset_type='MaterialFunction'` to discover."));

	UMaterialFunction* Func = LoadObject<UMaterialFunction>(nullptr, *FunctionPath);
	if (!Func)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("MaterialFunction not found: %s"), *FunctionPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`function_path` did not resolve to a UMaterialFunction. Verify the asset exists. Use `list_assets` with `asset_type='MaterialFunction'` to discover."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("function_name"), Func->GetName());
	Result->SetStringField(TEXT("function_path"), Func->GetPathName());
	Result->SetStringField(TEXT("description"), Func->GetDescription());

	// Expressions — same collection API as UMaterial
	auto& Exprs = Func->GetExpressionCollection().Expressions;
	TArray<TSharedPtr<FJsonValue>> ExprArr;
	for (int32 i = 0; i < Exprs.Num(); ++i)
	{
		if (Exprs[i])
			ExprArr.Add(MakeShared<FJsonValueObject>(ExpressionToJson(Exprs[i], i)));
	}
	Result->SetArrayField(TEXT("expressions"), ExprArr);

	// Function inputs (FunctionInput expressions that define the interface)
	TArray<TSharedPtr<FJsonValue>> InputArr;
	for (int32 i = 0; i < Exprs.Num(); ++i)
	{
		if (!Exprs[i]) continue;
		if (Exprs[i]->IsA(UMaterialExpressionFunctionInput::StaticClass()))
		{
			auto* InputExpr = Cast<UMaterialExpressionFunctionInput>(Exprs[i]);
			TSharedPtr<FJsonObject> InObj = MakeShared<FJsonObject>();
			InObj->SetStringField(TEXT("name"), InputExpr->InputName.ToString());
			InObj->SetNumberField(TEXT("input_type"), static_cast<int32>(InputExpr->InputType));
			InObj->SetStringField(TEXT("description"), InputExpr->Description);
			InObj->SetNumberField(TEXT("sort_priority"), InputExpr->SortPriority);
			InputArr.Add(MakeShared<FJsonValueObject>(InObj));
		}
	}
	Result->SetArrayField(TEXT("function_inputs"), InputArr);

	// Function outputs
	TArray<TSharedPtr<FJsonValue>> OutputArr;
	for (int32 i = 0; i < Exprs.Num(); ++i)
	{
		if (!Exprs[i]) continue;
		if (Exprs[i]->IsA(UMaterialExpressionFunctionOutput::StaticClass()))
		{
			auto* OutputExpr = Cast<UMaterialExpressionFunctionOutput>(Exprs[i]);
			TSharedPtr<FJsonObject> OutObj = MakeShared<FJsonObject>();
			OutObj->SetStringField(TEXT("name"), OutputExpr->OutputName.ToString());
			OutObj->SetStringField(TEXT("description"), OutputExpr->Description);
			OutObj->SetNumberField(TEXT("sort_priority"), OutputExpr->SortPriority);
			OutputArr.Add(MakeShared<FJsonValueObject>(OutObj));
		}
	}
	Result->SetArrayField(TEXT("function_outputs"), OutputArr);

	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ---------------------------------------------------------------------------
// ReadMaterialInstance
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMaterialExpressionManager::ReadMaterialInstance(
	const TSharedPtr<FJsonObject>& Params)
{
	FString InstancePath;
	if (!Params->TryGetStringField(TEXT("instance_path"), InstancePath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'instance_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`instance_path` is required and must be the full asset path to a UMaterialInstanceConstant, e.g. `/Game/Materials/MI_Foo`. Use `list_assets` with `asset_type='MaterialInstanceConstant'` to discover."));

	UObject* Obj = UEditorAssetLibrary::LoadAsset(InstancePath);
	if (!Obj)
	{
		FString AssetName = FPaths::GetBaseFilename(InstancePath);
		FString FullPath = FString::Printf(TEXT("%s.%s"), *InstancePath, *AssetName);
		Obj = LoadObject<UMaterialInstanceConstant>(nullptr, *FullPath);
	}
	UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(Obj);
	if (!MIC)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material instance not found: %s"), *InstancePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`instance_path` did not resolve to a UMaterialInstanceConstant. Verify the asset exists. Use `list_assets` with `asset_type='MaterialInstanceConstant'` to discover."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("instance_name"), MIC->GetName());
	Result->SetStringField(TEXT("instance_path"), MIC->GetPathName());

	// Parent chain
	TArray<TSharedPtr<FJsonValue>> ParentArr;
	UMaterialInterface* Current = MIC->Parent;
	while (Current)
	{
		TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
		PObj->SetStringField(TEXT("name"), Current->GetName());
		PObj->SetStringField(TEXT("path"), Current->GetPathName());
		PObj->SetStringField(TEXT("class"), Current->GetClass()->GetName());
		ParentArr.Add(MakeShared<FJsonValueObject>(PObj));

		if (UMaterialInstance* ParentMI = Cast<UMaterialInstance>(Current))
			Current = ParentMI->Parent;
		else
			Current = nullptr;
	}
	Result->SetArrayField(TEXT("parent_chain"), ParentArr);

	// Scalar parameters
	TArray<TSharedPtr<FJsonValue>> ScalarArr;
	for (const auto& Param : MIC->ScalarParameterValues)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		P->SetNumberField(TEXT("value"), Param.ParameterValue);
		ScalarArr.Add(MakeShared<FJsonValueObject>(P));
	}
	Result->SetArrayField(TEXT("scalar_parameters"), ScalarArr);

	// Vector parameters
	TArray<TSharedPtr<FJsonValue>> VectorArr;
	for (const auto& Param : MIC->VectorParameterValues)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		const FLinearColor& C = Param.ParameterValue;
		P->SetNumberField(TEXT("r"), C.R);
		P->SetNumberField(TEXT("g"), C.G);
		P->SetNumberField(TEXT("b"), C.B);
		P->SetNumberField(TEXT("a"), C.A);
		VectorArr.Add(MakeShared<FJsonValueObject>(P));
	}
	Result->SetArrayField(TEXT("vector_parameters"), VectorArr);

	// Texture parameters
	TArray<TSharedPtr<FJsonValue>> TextureArr;
	for (const auto& Param : MIC->TextureParameterValues)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		P->SetStringField(TEXT("texture"),
			Param.ParameterValue ? Param.ParameterValue->GetPathName() : TEXT("None"));
		TextureArr.Add(MakeShared<FJsonValueObject>(P));
	}
	Result->SetArrayField(TEXT("texture_parameters"), TextureArr);

	// Static switch parameters
	TArray<TSharedPtr<FJsonValue>> SwitchArr;
	FStaticParameterSet StaticParams;
	MIC->GetStaticParameterValues(StaticParams);
	for (const auto& Sw : StaticParams.StaticSwitchParameters)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("name"), Sw.ParameterInfo.Name.ToString());
		P->SetBoolField(TEXT("value"), Sw.Value);
		P->SetBoolField(TEXT("override"), Sw.bOverride);
		SwitchArr.Add(MakeShared<FJsonValueObject>(P));
	}
	Result->SetArrayField(TEXT("static_switch_parameters"), SwitchArr);

	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ---------------------------------------------------------------------------
// SetMaterialInstanceParameter
// ---------------------------------------------------------------------------

static UMaterialInstanceConstant* LoadMIC(const FString& Path)
{
	UObject* Obj = UEditorAssetLibrary::LoadAsset(Path);
	if (!Obj)
	{
		FString AssetName = FPaths::GetBaseFilename(Path);
		FString FullPath = FString::Printf(TEXT("%s.%s"), *Path, *AssetName);
		Obj = LoadObject<UMaterialInstanceConstant>(nullptr, *FullPath);
	}
	return Cast<UMaterialInstanceConstant>(Obj);
}

TSharedPtr<FJsonObject> FMaterialExpressionManager::SetMaterialInstanceParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	FString InstancePath;
	if (!Params->TryGetStringField(TEXT("instance_path"), InstancePath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'instance_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`instance_path` is required and must be the full asset path to a UMaterialInstanceConstant, e.g. `/Game/Materials/MI_Foo`. Use `list_assets` with `asset_type='MaterialInstanceConstant'` to discover."));

	FString ParamName;
	if (!Params->TryGetStringField(TEXT("parameter_name"), ParamName))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'parameter_name'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`parameter_name` is required — the named parameter on the Material Instance to set. Use `read_material_instance` to see existing scalar/vector/texture/static-switch parameter names."));

	FString ParamType;
	if (!Params->TryGetStringField(TEXT("parameter_type"), ParamType))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'parameter_type' (scalar|vector|texture|static_switch|base_override)"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`parameter_type` is required and must be one of `scalar`, `vector`, `texture`, `static_switch`, or `base_override`."));

	UMaterialInstanceConstant* MIC = LoadMIC(InstancePath);
	if (!MIC)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material instance not found: %s"), *InstancePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`instance_path` did not resolve to a UMaterialInstanceConstant. Verify the asset exists. Use `list_assets` with `asset_type='MaterialInstanceConstant'` to discover."));

	// base_override: short-circuit before the named-parameter validation block.
	// Sets MaterialInstance::BasePropertyOverrides flags (TwoSided, BlendMode,
	// ShadingModel, OpacityMaskClipValue, DitheredLODTransition). Param name
	// must be one of these. Param value depends on which.
	if (ParamType == TEXT("base_override"))
	{
		// dry_run (base_override branch): validate ParamName up-front and
		// capture the current Override/value pair without calling PreEditChange.
		// (Apply still PreEditChange's first per the existing pattern — this
		// dry-run path skips that side effect entirely.) Diff shape:
		// properties_changed[] with a stringified `(override=X,value=Y)` pair.
		if (FMCPCommonUtils::ParseDryRun(Params))
		{
			const FMaterialInstanceBasePropertyOverrides& Ov = MIC->BasePropertyOverrides;
			auto BoolStr = [](bool b) { return b ? TEXT("true") : TEXT("false"); };

			FString BeforeStr;
			FString AfterStr;

			if (ParamName == TEXT("TwoSided"))
			{
				bool b = false; Params->TryGetBoolField(TEXT("value"), b);
				BeforeStr = FString::Printf(TEXT("(override=%s,value=%s)"),
					BoolStr(Ov.bOverride_TwoSided), BoolStr(Ov.TwoSided));
				AfterStr = FString::Printf(TEXT("(override=true,value=%s)"), BoolStr(b));
			}
			else if (ParamName == TEXT("OpacityMaskClipValue"))
			{
				double f = 0; Params->TryGetNumberField(TEXT("value"), f);
				BeforeStr = FString::Printf(TEXT("(override=%s,value=%s)"),
					BoolStr(Ov.bOverride_OpacityMaskClipValue),
					*FString::SanitizeFloat(Ov.OpacityMaskClipValue));
				AfterStr = FString::Printf(TEXT("(override=true,value=%s)"),
					*FString::SanitizeFloat((float)f));
			}
			else if (ParamName == TEXT("DitheredLODTransition"))
			{
				bool b = false; Params->TryGetBoolField(TEXT("value"), b);
				BeforeStr = FString::Printf(TEXT("(override=%s,value=%s)"),
					BoolStr(Ov.bOverride_DitheredLODTransition),
					BoolStr(Ov.DitheredLODTransition));
				AfterStr = FString::Printf(TEXT("(override=true,value=%s)"), BoolStr(b));
			}
			else
			{
				return FMCPCommonUtils::CreateErrorResponse(
					FString::Printf(TEXT("Unsupported base_override name: %s (try TwoSided/OpacityMaskClipValue/DitheredLODTransition)"), *ParamName),
					EMCPErrorCode::InvalidArgument,
					TEXT("`parameter_name` for `parameter_type='base_override'` must be one of: `TwoSided` (bool), `OpacityMaskClipValue` (float), or `DitheredLODTransition` (bool). These are the only `FMaterialInstanceBasePropertyOverrides` fields exposed by MCP."));
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("instance_path"), MIC->GetPathName());
			Entry->SetStringField(TEXT("parameter_name"), ParamName);
			Entry->SetStringField(TEXT("parameter_type"), TEXT("base_override"));
			Entry->SetStringField(TEXT("before"), BeforeStr);
			Entry->SetStringField(TEXT("after"), AfterStr);

			TArray<TSharedPtr<FJsonValue>> Arr;
			Arr.Add(MakeShared<FJsonValueObject>(Entry));
			TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
			Diff->SetArrayField(TEXT("properties_changed"), Arr);
			return FMCPCommonUtils::CreateDryRunResponse(Diff);
		}

		MIC->PreEditChange(nullptr);
		FMaterialInstanceBasePropertyOverrides& Ov = MIC->BasePropertyOverrides;

		if (ParamName == TEXT("TwoSided"))
		{
			bool b = false;
			Params->TryGetBoolField(TEXT("value"), b);
			Ov.bOverride_TwoSided = true;
			Ov.TwoSided = b;
		}
		else if (ParamName == TEXT("OpacityMaskClipValue"))
		{
			double f = 0;
			Params->TryGetNumberField(TEXT("value"), f);
			Ov.bOverride_OpacityMaskClipValue = true;
			Ov.OpacityMaskClipValue = (float)f;
		}
		else if (ParamName == TEXT("DitheredLODTransition"))
		{
			bool b = false;
			Params->TryGetBoolField(TEXT("value"), b);
			Ov.bOverride_DitheredLODTransition = true;
			Ov.DitheredLODTransition = b;
		}
		else
		{
			MIC->PostEditChange();
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Unsupported base_override name: %s (try TwoSided/OpacityMaskClipValue/DitheredLODTransition)"), *ParamName),
				EMCPErrorCode::InvalidArgument,
				TEXT("`parameter_name` for `parameter_type='base_override'` must be one of: `TwoSided` (bool), `OpacityMaskClipValue` (float), or `DitheredLODTransition` (bool). These are the only `FMaterialInstanceBasePropertyOverrides` fields exposed by MCP."));
		}

		MIC->PostEditChange();
		MIC->MarkPackageDirty();
		if (!UEditorAssetLibrary::SaveLoadedAsset(MIC, /*bOnlyIfIsDirty=*/false))
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("base_override applied in-memory but failed to persist: %s"), *MIC->GetPathName()),
				EMCPErrorCode::Internal,
				TEXT("`UEditorAssetLibrary::SaveLoadedAsset` returned false — the edit was NOT written to disk and will be lost on editor restart. The save no-ops while PIE is running (and under other read-only / asset-registration failures). Stop PIE and retry."));

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("instance_path"), MIC->GetPathName());
		R->SetStringField(TEXT("parameter_name"), ParamName);
		R->SetStringField(TEXT("parameter_type"), TEXT("base_override"));
		R->SetBoolField(TEXT("success"), true);
		return R;
	}

	// Validate-before-mutate. UObject::PreEditChange (Runtime/CoreUObject/Private/UObject/Obj.cpp:483)
	// calls Modify(true) which dirties the package and snapshots state into the active undo
	// transaction; every PreEditChange must be balanced by PostEditChange. Parsing and
	// existence checks run above the edit envelope so an early-return error path cannot
	// leave the asset half-edited (dirty package, orphan transaction record, missing
	// FCoreUObjectDelegates::OnObjectPropertyChanged broadcast).

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	double ScalarValue = 0;
	double VR = 0, VG = 0, VB = 0, VA = 1;
	UTexture* TexValue = nullptr;
	FString ResolvedTexturePath;
	bool SwitchValue = false;
	// Default matches engine convention: `SetStaticSwitchParameterValueEditorOnly`
	// (Runtime/Engine/Private/Materials/MaterialInstance.cpp:2214-2232) always sets
	// `bOverride = true`. Callers may opt out by passing `override: false` explicitly,
	// which closes the read→write round-trip silent-flip on the `override` field that
	// `ReadMaterialInstance` emits (see :848 of this file).
	bool SwitchOverride = true;
	FStaticParameterSet StaticParams;
	int32 StaticSwitchIdx = INDEX_NONE;

	if (ParamType == TEXT("scalar"))
	{
		if (!Params->TryGetNumberField(TEXT("value"), ScalarValue))
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'value' for scalar parameter"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`value` is required when `parameter_type='scalar'` and must be a number (the float to write to the scalar parameter)."));
		Result->SetNumberField(TEXT("value"), ScalarValue);
	}
	else if (ParamType == TEXT("vector"))
	{
		Params->TryGetNumberField(TEXT("r"), VR);
		Params->TryGetNumberField(TEXT("g"), VG);
		Params->TryGetNumberField(TEXT("b"), VB);
		Params->TryGetNumberField(TEXT("a"), VA);
		// Emit both the legacy stringified `value` (back-compat for any caller that
		// already parses `(r, g, b, a)`) and the per-component keys that
		// `ReadMaterialInstance` emits (:819-822). Closes the setter→reader output
		// shape drift: `set → read` now round-trips with matching keys.
		Result->SetStringField(TEXT("value"),
			FString::Printf(TEXT("(%f, %f, %f, %f)"), VR, VG, VB, VA));
		Result->SetNumberField(TEXT("r"), VR);
		Result->SetNumberField(TEXT("g"), VG);
		Result->SetNumberField(TEXT("b"), VB);
		Result->SetNumberField(TEXT("a"), VA);
	}
	else if (ParamType == TEXT("texture"))
	{
		// Accept both `texture_path` (setter-canonical key) and `texture` (the key
		// `ReadMaterialInstance` emits at :833). Back-compat shim: callers that did
		// `read_material_instance → edit → set_material_instance_parameter` no
		// longer have to rename the field. `texture_path` wins when both are supplied.
		FString TexturePath;
		if (!Params->TryGetStringField(TEXT("texture_path"), TexturePath) &&
			!Params->TryGetStringField(TEXT("texture"), TexturePath))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'texture_path' (or legacy alias 'texture') for texture parameter"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`texture_path` is required when `parameter_type='texture'` — pass the full asset path to a UTexture, e.g. `/Game/Textures/T_Foo`. The legacy alias `texture` is also accepted for round-trip back-compat with `read_material_instance` output."));
		}

		TexValue = LoadObject<UTexture>(nullptr, *TexturePath);
		if (!TexValue)
		{
			UObject* TexObj = UEditorAssetLibrary::LoadAsset(TexturePath);
			TexValue = Cast<UTexture>(TexObj);
		}
		if (!TexValue)
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Texture not found: %s"), *TexturePath),
				EMCPErrorCode::AssetNotFound,
				TEXT("`texture_path` did not resolve to a UTexture asset. Verify the asset exists. Use `list_assets` with `asset_type='Texture2D'` to discover."));
		ResolvedTexturePath = TexValue->GetPathName();
		// Emit both `value` (legacy setter-response key) and `texture` (the key the
		// reader emits) for round-trip parity.
		Result->SetStringField(TEXT("value"), ResolvedTexturePath);
		Result->SetStringField(TEXT("texture"), ResolvedTexturePath);
	}
	else if (ParamType == TEXT("static_switch"))
	{
		if (!Params->TryGetBoolField(TEXT("value"), SwitchValue))
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'value' for static_switch parameter"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`value` is required when `parameter_type='static_switch'` and must be a boolean. Optionally pass `override` (bool) to flip the switch's `bOverride` flag — defaults to `true`."));

		// Optional `override`; default `true` preserves pre-bundle behavior for
		// callers that never supplied the field. Respecting it when present closes
		// the read→write round-trip silent-flip documented in the 2026-04-23
		// `SetMaterialInstanceParameter` validate-before-mutate bundle's #RESEARCH
		// note (sub-issue c of the three-part parity drift).
		Params->TryGetBoolField(TEXT("override"), SwitchOverride);

		MIC->GetStaticParameterValues(StaticParams);
		for (int32 i = 0; i < StaticParams.StaticSwitchParameters.Num(); ++i)
		{
			if (StaticParams.StaticSwitchParameters[i].ParameterInfo.Name.ToString() == ParamName)
			{
				StaticSwitchIdx = i;
				break;
			}
		}
		if (StaticSwitchIdx == INDEX_NONE)
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Static switch not found: %s"), *ParamName),
				EMCPErrorCode::NodeNotFound,
				TEXT("`parameter_name` did not match any static-switch parameter on this Material Instance. Use `read_material_instance` to list `static_switch_parameters`. Static switches must be declared on the parent material."));
		// Emit both `value` and `override` — matches the reader's per-switch JSON
		// at :847-848.
		Result->SetBoolField(TEXT("value"), SwitchValue);
		Result->SetBoolField(TEXT("override"), SwitchOverride);
	}
	else
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Unknown parameter_type: %s (use scalar|vector|texture|static_switch)"),
				*ParamType),
			EMCPErrorCode::InvalidArgument,
			TEXT("`parameter_type` must be one of `scalar`, `vector`, `texture`, `static_switch`, or `base_override` (the latter handled in a separate branch). Names are exact and case-sensitive."));
	}

	// dry_run (main 4-type branch): every preflight ran (paths, MIC load,
	// parameter_type validation, value parses, texture asset load, static-switch
	// existence check). Skip PreEditChange + Set*ParameterValueEditorOnly /
	// UpdateStaticPermutation + SaveLoadedAsset. Diff shape: properties_changed[]
	// with the type-appropriate before/after pair stringified the same way the
	// reader emits the value (FLinearColor::ToString for vector,
	// path-string for texture, raw float for scalar, "(value=,override=)"
	// composite for static_switch).
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		FString BeforeStr;
		FString AfterStr;

		if (ParamType == TEXT("scalar"))
		{
			float Current = 0;
			FMaterialParameterInfo Info{FName(*ParamName)};
			MIC->GetScalarParameterValue(Info, Current);
			BeforeStr = FString::SanitizeFloat(Current);
			AfterStr = FString::SanitizeFloat(static_cast<float>(ScalarValue));
		}
		else if (ParamType == TEXT("vector"))
		{
			FLinearColor Current(0,0,0,0);
			FMaterialParameterInfo Info{FName(*ParamName)};
			MIC->GetVectorParameterValue(Info, Current);
			BeforeStr = Current.ToString();
			AfterStr = FLinearColor((float)VR, (float)VG, (float)VB, (float)VA).ToString();
		}
		else if (ParamType == TEXT("texture"))
		{
			UTexture* Current = nullptr;
			FMaterialParameterInfo Info{FName(*ParamName)};
			MIC->GetTextureParameterValue(Info, Current);
			BeforeStr = Current ? Current->GetPathName() : FString(TEXT("None"));
			AfterStr = ResolvedTexturePath;
		}
		else // static_switch
		{
			const FStaticSwitchParameter& Sw = StaticParams.StaticSwitchParameters[StaticSwitchIdx];
			BeforeStr = FString::Printf(TEXT("(value=%s,override=%s)"),
				Sw.Value ? TEXT("true") : TEXT("false"),
				Sw.bOverride ? TEXT("true") : TEXT("false"));
			AfterStr = FString::Printf(TEXT("(value=%s,override=%s)"),
				SwitchValue ? TEXT("true") : TEXT("false"),
				SwitchOverride ? TEXT("true") : TEXT("false"));
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("instance_path"), MIC->GetPathName());
		Entry->SetStringField(TEXT("parameter_name"), ParamName);
		Entry->SetStringField(TEXT("parameter_type"), ParamType);
		Entry->SetStringField(TEXT("before"), BeforeStr);
		Entry->SetStringField(TEXT("after"), AfterStr);

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("properties_changed"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	// All parses + existence checks succeeded; open the edit transaction.
	MIC->PreEditChange(nullptr);

	if (ParamType == TEXT("scalar"))
	{
		FMaterialParameterInfo Info{FName(*ParamName)};
		MIC->SetScalarParameterValueEditorOnly(Info, static_cast<float>(ScalarValue));
	}
	else if (ParamType == TEXT("vector"))
	{
		FMaterialParameterInfo Info{FName(*ParamName)};
		MIC->SetVectorParameterValueEditorOnly(Info, FLinearColor(VR, VG, VB, VA));
	}
	else if (ParamType == TEXT("texture"))
	{
		FMaterialParameterInfo Info{FName(*ParamName)};
		MIC->SetTextureParameterValueEditorOnly(Info, TexValue);
	}
	else // static_switch (only remaining type — exhaustive above)
	{
		FStaticSwitchParameter& Sw = StaticParams.StaticSwitchParameters[StaticSwitchIdx];
		Sw.Value = SwitchValue;
		Sw.bOverride = SwitchOverride;
		MIC->UpdateStaticPermutation(StaticParams);
	}

	MIC->PostEditChange();
	MIC->MarkPackageDirty();
	// Persist to disk so the param edit survives editor restart. MarkPackageDirty
	// alone only flags the asset as dirty in-memory; without SaveLoadedAsset,
	// changes are lost on next reload.
	if (!UEditorAssetLibrary::SaveLoadedAsset(MIC, /*bOnlyIfIsDirty=*/false))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Parameter '%s' applied in-memory but failed to persist: %s"), *ParamName, *MIC->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("`UEditorAssetLibrary::SaveLoadedAsset` returned false — the edit was NOT written to disk and will be lost on editor restart. The save no-ops while PIE is running (and under other read-only / asset-registration failures). Stop PIE and retry."));

	Result->SetStringField(TEXT("instance_path"), MIC->GetPathName());
	Result->SetStringField(TEXT("parameter_name"), ParamName);
	// Reader-shape alias: read_material_instance emits each parameter entry
	// as {name, ...}. Emit `name` too so a set→read workflow can match the
	// reader's array entries without a key rename; `parameter_name` stays
	// the canonical response key.
	Result->SetStringField(TEXT("name"), ParamName);
	Result->SetStringField(TEXT("parameter_type"), ParamType);
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}

// ---------------------------------------------------------------------------
// ReparentMaterialInstance
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMaterialExpressionManager::ReparentMaterialInstance(
	const TSharedPtr<FJsonObject>& Params)
{
	FString InstancePath;
	if (!Params->TryGetStringField(TEXT("instance_path"), InstancePath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'instance_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`instance_path` is required and must be the full asset path to a UMaterialInstanceConstant, e.g. `/Game/Materials/MI_Foo`. Use `list_assets` with `asset_type='MaterialInstanceConstant'` to discover."));

	FString NewParentPath;
	if (!Params->TryGetStringField(TEXT("new_parent_path"), NewParentPath))
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'new_parent_path'"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`new_parent_path` is required and must be the full asset path to a UMaterialInterface (UMaterial or UMaterialInstance) to reparent under, e.g. `/Game/Materials/M_Foo`."));

	UMaterialInstanceConstant* MIC = LoadMIC(InstancePath);
	if (!MIC)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Material instance not found: %s"), *InstancePath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`instance_path` did not resolve to a UMaterialInstanceConstant. Verify the asset exists. Use `list_assets` with `asset_type='MaterialInstanceConstant'` to discover."));

	UObject* ParentObj = UEditorAssetLibrary::LoadAsset(NewParentPath);
	UMaterialInterface* NewParent = Cast<UMaterialInterface>(ParentObj);
	if (!NewParent)
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Parent material not found: %s"), *NewParentPath),
			EMCPErrorCode::AssetNotFound,
			TEXT("`new_parent_path` did not resolve to a UMaterialInterface (UMaterial or UMaterialInstance). Verify the asset exists and is a material/material-instance — not a UMaterialFunction or other asset type."));

	MIC->PreEditChange(nullptr);
	MIC->SetParentEditorOnly(NewParent);
	MIC->PostEditChange();
	MIC->MarkPackageDirty();
	// Persist to disk so the reparent survives editor restart. MarkPackageDirty
	// alone only flags the asset as dirty in-memory; without SaveLoadedAsset,
	// the new parent is lost on next reload.
	if (!UEditorAssetLibrary::SaveLoadedAsset(MIC, /*bOnlyIfIsDirty=*/false))
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Reparent mutated in-memory but failed to persist to disk: %s"), *MIC->GetPathName()),
			EMCPErrorCode::Internal,
			TEXT("UEditorAssetLibrary::SaveLoadedAsset returned false — the package was not written. SaveLoadedAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("instance_path"), MIC->GetPathName());
	Result->SetStringField(TEXT("new_parent"), NewParent->GetPathName());
	Result->SetStringField(TEXT("new_parent_class"), NewParent->GetClass()->GetName());
	Result->SetBoolField(TEXT("success"), true);
	return Result;
}
