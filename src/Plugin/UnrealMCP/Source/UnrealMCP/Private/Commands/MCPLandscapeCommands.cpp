// Read-only landscape inspection (GAPS #13). No mutation — sculpt/paint/import
// belong to the editor's Landscape mode. See MCPLandscapeCommands.h.

#include "Commands/MCPLandscapeCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"

#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeStreamingProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#if WITH_EDITOR
#include "LandscapeEdit.h"
#endif

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	// Landscape raw-height encoding (LandscapeDataAccess.h): a uint16 height of
	// 32768 is "zero" local height, and one unit = 1/128 uu before the actor's
	// Z scale is applied.
	constexpr float LandscapeMidHeight = 32768.0f;
	constexpr float LandscapeZScale = 1.0f / 128.0f;

	UWorld* GetLandscapeEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	TSharedPtr<FJsonObject> Vec3Json(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	const TCHAR* LandscapeClassLabel(const ALandscapeProxy* Proxy)
	{
		if (Proxy->IsA<ALandscapeStreamingProxy>()) return TEXT("LandscapeStreamingProxy");
		if (Proxy->IsA<ALandscape>())               return TEXT("Landscape");
		return TEXT("LandscapeProxy");
	}

	// Find a landscape proxy by actor label/name; nullptr if Name is empty (caller
	// then iterates all). Out-param distinguishes "not found" from "list-all".
	ALandscapeProxy* FindLandscapeByName(UWorld* World, const FString& Name)
	{
		if (!World || Name.IsEmpty()) return nullptr;
		for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
		{
			ALandscapeProxy* P = *It;
			if (!P) continue;
			if (P->GetActorLabel() == Name || P->GetName() == Name) return P;
		}
		return nullptr;
	}
}

TSharedPtr<FJsonObject> FMCPLandscapeCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("landscape_inspect"))        return HandleInspect(Params);
	if (CommandType == TEXT("landscape_list_layers"))    return HandleListLayers(Params);
	if (CommandType == TEXT("landscape_read_heightmap")) return HandleReadHeightmap(Params);

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown landscape command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: landscape_inspect, landscape_list_layers, landscape_read_heightmap."));
}

// ─── landscape_inspect ──────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPLandscapeCommands::HandleInspect(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetLandscapeEditorWorld();
	if (!World)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No editor world"), EMCPErrorCode::Internal,
			TEXT("GEditor world is unavailable — retry once the editor has finished loading a map."));
	}

	FString ActorName;
	if (Params.IsValid()) Params->TryGetStringField(TEXT("actor_name"), ActorName);

	TArray<TSharedPtr<FJsonValue>> Landscapes;
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		ALandscapeProxy* Proxy = *It;
		if (!Proxy) continue;
		if (!ActorName.IsEmpty() && Proxy->GetActorLabel() != ActorName && Proxy->GetName() != ActorName)
		{
			continue;
		}

		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("name"), Proxy->GetActorLabel());
		E->SetStringField(TEXT("internal_name"), Proxy->GetName());
		E->SetStringField(TEXT("class"), LandscapeClassLabel(Proxy));
		E->SetStringField(TEXT("guid"), Proxy->GetLandscapeGuid().ToString());

		const FTransform Xform = Proxy->GetTransform();
		E->SetObjectField(TEXT("location"), Vec3Json(Xform.GetLocation()));
		E->SetObjectField(TEXT("scale"), Vec3Json(Xform.GetScale3D()));

		E->SetNumberField(TEXT("component_size_quads"), Proxy->ComponentSizeQuads);
		E->SetNumberField(TEXT("subsection_size_quads"), Proxy->SubsectionSizeQuads);
		E->SetNumberField(TEXT("num_subsections"), Proxy->NumSubsections);
		E->SetStringField(TEXT("material"),
			Proxy->LandscapeMaterial ? Proxy->LandscapeMaterial->GetPathName() : FString());

		if (ULandscapeInfo* Info = Proxy->GetLandscapeInfo())
		{
			int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
			if (Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
			{
				TSharedPtr<FJsonObject> Ext = MakeShared<FJsonObject>();
				Ext->SetNumberField(TEXT("min_x"), MinX);
				Ext->SetNumberField(TEXT("min_y"), MinY);
				Ext->SetNumberField(TEXT("max_x"), MaxX);
				Ext->SetNumberField(TEXT("max_y"), MaxY);
				Ext->SetNumberField(TEXT("width"), MaxX - MinX + 1);
				Ext->SetNumberField(TEXT("height"), MaxY - MinY + 1);
				E->SetObjectField(TEXT("extent_quads"), Ext);
			}
			E->SetNumberField(TEXT("paint_layer_count"), Info->Layers.Num());
		}

		Landscapes.Add(MakeShared<FJsonValueObject>(E));
	}

	if (!ActorName.IsEmpty() && Landscapes.Num() == 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Landscape actor not found: %s"), *ActorName),
			EMCPErrorCode::ActorNotFound,
			TEXT("`actor_name` matched no ALandscapeProxy. Omit it to list all landscapes in the loaded world."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("count"), Landscapes.Num());
	Result->SetArrayField(TEXT("landscapes"), Landscapes);
	return Result;
}

// ─── landscape_list_layers ──────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPLandscapeCommands::HandleListLayers(const TSharedPtr<FJsonObject>& Params)
{
	UWorld* World = GetLandscapeEditorWorld();
	if (!World)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No editor world"), EMCPErrorCode::Internal,
			TEXT("GEditor world is unavailable — retry once the editor has finished loading a map."));
	}

	FString ActorName;
	if (Params.IsValid()) Params->TryGetStringField(TEXT("actor_name"), ActorName);

	ALandscapeProxy* Proxy = FindLandscapeByName(World, ActorName);
	if (ActorName.IsEmpty())
	{
		// Default to the first landscape in the world.
		for (TActorIterator<ALandscapeProxy> It(World); It; ++It) { Proxy = *It; if (Proxy) break; }
	}
	if (!Proxy)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			ActorName.IsEmpty() ? TEXT("No landscape in the loaded world")
								 : *FString::Printf(TEXT("Landscape actor not found: %s"), *ActorName),
			EMCPErrorCode::ActorNotFound,
			TEXT("Provide `actor_name` of an ALandscapeProxy, or load a map containing a landscape."));
	}

	ULandscapeInfo* Info = Proxy->GetLandscapeInfo();
	TArray<TSharedPtr<FJsonValue>> LayersArr;
	if (Info)
	{
		for (const FLandscapeInfoLayerSettings& L : Info->Layers)
		{
			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("name"), L.LayerName.ToString());
			E->SetStringField(TEXT("layer_info_object"),
				L.LayerInfoObj ? L.LayerInfoObj->GetPathName() : FString());
			E->SetBoolField(TEXT("assigned"), L.LayerInfoObj != nullptr);
			LayersArr.Add(MakeShared<FJsonValueObject>(E));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor"), Proxy->GetActorLabel());
	Result->SetNumberField(TEXT("count"), LayersArr.Num());
	Result->SetArrayField(TEXT("layers"), LayersArr);
	return Result;
}

// ─── landscape_read_heightmap ───────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPLandscapeCommands::HandleReadHeightmap(const TSharedPtr<FJsonObject>& Params)
{
#if !WITH_EDITOR
	return FMCPCommonUtils::CreateErrorResponse(
		TEXT("Heightmap read requires the editor"), EMCPErrorCode::Internal,
		TEXT("landscape_read_heightmap uses the editor-only landscape edit interface; it is unavailable in this build configuration."));
#else
	if (!Params.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing params"), EMCPErrorCode::InvalidArgument,
			TEXT("Requires `actor_name`; optional `region {min_x,min_y,max_x,max_y}`, `export_path` (.r16), and `include_samples`."));
	}

	UWorld* World = GetLandscapeEditorWorld();
	if (!World)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No editor world"), EMCPErrorCode::Internal,
			TEXT("GEditor world is unavailable — retry once the editor has finished loading a map."));
	}

	FString ActorName;
	Params->TryGetStringField(TEXT("actor_name"), ActorName);
	ALandscapeProxy* Proxy = FindLandscapeByName(World, ActorName);
	if (ActorName.IsEmpty())
	{
		for (TActorIterator<ALandscapeProxy> It(World); It; ++It) { Proxy = *It; if (Proxy) break; }
	}
	if (!Proxy)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Landscape not found"), EMCPErrorCode::ActorNotFound,
			TEXT("Provide `actor_name` of an ALandscapeProxy, or load a map containing a landscape."));
	}

	ULandscapeInfo* Info = Proxy->GetLandscapeInfo();
	if (!Info)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Landscape has no ULandscapeInfo (no components loaded)"), EMCPErrorCode::InvalidArgument,
			TEXT("The landscape's components are not loaded (e.g. an unloaded World Partition region). Load the region and retry."));
	}

	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Landscape extent unavailable"), EMCPErrorCode::InvalidArgument,
			TEXT("GetLandscapeExtent returned no region — the landscape has no loaded components."));
	}

	// Optional sub-region (clamped to the full extent).
	const TSharedPtr<FJsonObject>* RegionObj = nullptr;
	if (Params->TryGetObjectField(TEXT("region"), RegionObj) && RegionObj && RegionObj->IsValid())
	{
		int32 RX1 = MinX, RY1 = MinY, RX2 = MaxX, RY2 = MaxY;
		(*RegionObj)->TryGetNumberField(TEXT("min_x"), RX1);
		(*RegionObj)->TryGetNumberField(TEXT("min_y"), RY1);
		(*RegionObj)->TryGetNumberField(TEXT("max_x"), RX2);
		(*RegionObj)->TryGetNumberField(TEXT("max_y"), RY2);
		MinX = FMath::Clamp(RX1, MinX, MaxX);
		MinY = FMath::Clamp(RY1, MinY, MaxY);
		MaxX = FMath::Clamp(RX2, MinX, MaxX);
		MaxY = FMath::Clamp(RY2, MinY, MaxY);
	}

	const int64 Width = (int64)MaxX - MinX + 1;
	const int64 Height = (int64)MaxY - MinY + 1;
	const int64 Samples = Width * Height;

	FString ExportPath;
	Params->TryGetStringField(TEXT("export_path"), ExportPath);
	bool bIncludeSamples = false;
	Params->TryGetBoolField(TEXT("include_samples"), bIncludeSamples);

	// Guardrail: refuse an unbounded inline read. A summary always fits; large
	// regions must either be narrowed with `region` or written to `export_path`.
	constexpr int64 MaxInlineSamples = 4LL * 1024 * 1024; // 4M points
	if (Samples > MaxInlineSamples && ExportPath.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Heightmap region too large to read inline: %lld samples"), Samples),
			EMCPErrorCode::InvalidArgument,
			TEXT("Pass a smaller `region {min_x,min_y,max_x,max_y}` or an `export_path` (.r16) to write the raw grid to disk."));
	}

	// Sparse read keeps memory bounded and only returns valid samples.
	int32 X1 = MinX, Y1 = MinY, X2 = MaxX, Y2 = MaxY;
	TMap<FIntPoint, uint16> HeightData;
	FLandscapeEditDataInterface EditData(Info, /*bUploadTextureChangesToGPU=*/false);
	EditData.GetHeightData(X1, Y1, X2, Y2, HeightData);

	// Statistics over the read samples.
	const float ScaleZ = Proxy->GetTransform().GetScale3D().Z;
	const float BaseZ = Proxy->GetTransform().GetLocation().Z;
	uint16 RawMin = 65535, RawMax = 0;
	double RawSum = 0.0;
	int64 Counted = 0;
	for (const TPair<FIntPoint, uint16>& Pair : HeightData)
	{
		const uint16 H = Pair.Value;
		RawMin = FMath::Min(RawMin, H);
		RawMax = FMath::Max(RawMax, H);
		RawSum += H;
		++Counted;
	}
	auto RawToWorldZ = [&](float Raw) { return BaseZ + (Raw - LandscapeMidHeight) * LandscapeZScale * ScaleZ; };

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor"), Proxy->GetActorLabel());

	TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
	Region->SetNumberField(TEXT("min_x"), MinX);
	Region->SetNumberField(TEXT("min_y"), MinY);
	Region->SetNumberField(TEXT("max_x"), MaxX);
	Region->SetNumberField(TEXT("max_y"), MaxY);
	Region->SetNumberField(TEXT("width"), Width);
	Region->SetNumberField(TEXT("height"), Height);
	Result->SetObjectField(TEXT("region"), Region);
	Result->SetNumberField(TEXT("samples_read"), Counted);

	if (Counted > 0)
	{
		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetNumberField(TEXT("min_raw"), RawMin);
		Stats->SetNumberField(TEXT("max_raw"), RawMax);
		Stats->SetNumberField(TEXT("mean_raw"), RawSum / (double)Counted);
		Stats->SetNumberField(TEXT("min_world_z"), RawToWorldZ(RawMin));
		Stats->SetNumberField(TEXT("max_world_z"), RawToWorldZ(RawMax));
		Result->SetObjectField(TEXT("height_stats"), Stats);
	}

	// Optional raw export (.r16 — row-major uint16, missing samples → midpoint).
	bool bExported = false;
	if (!ExportPath.IsEmpty())
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized((int32)(Samples * (int64)sizeof(uint16)));
		uint16* Dst = reinterpret_cast<uint16*>(Bytes.GetData());
		for (int64 i = 0; i < Samples; ++i) Dst[i] = (uint16)LandscapeMidHeight;
		for (const TPair<FIntPoint, uint16>& Pair : HeightData)
		{
			const int64 LX = Pair.Key.X - MinX;
			const int64 LY = Pair.Key.Y - MinY;
			if (LX >= 0 && LX < Width && LY >= 0 && LY < Height)
			{
				Dst[LY * Width + LX] = Pair.Value;
			}
		}
		bExported = FFileHelper::SaveArrayToFile(Bytes, *ExportPath);
		if (!bExported)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Failed to write heightmap export: %s"), *ExportPath),
				EMCPErrorCode::Internal,
				TEXT("Could not write the .r16 file — check the path is an absolute, writable filesystem location."));
		}
		Result->SetStringField(TEXT("export_path"), ExportPath);
		Result->SetStringField(TEXT("export_format"), TEXT("r16"));
		Result->SetNumberField(TEXT("export_bytes"), Samples * (int64)sizeof(uint16));
	}
	Result->SetBoolField(TEXT("exported"), bExported);

	// Optional small inline grid (row-major) for programmatic sampling.
	if (bIncludeSamples)
	{
		constexpr int64 MaxInlineGrid = 256LL * 256;
		if (Samples > MaxInlineGrid)
		{
			Result->SetBoolField(TEXT("samples_included"), false);
			Result->SetStringField(TEXT("samples_note"),
				TEXT("Region exceeds 256x256; inline samples omitted. Narrow `region` to include them."));
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> Grid;
			Grid.Reserve((int32)Samples);
			for (int64 y = 0; y < Height; ++y)
			{
				for (int64 x = 0; x < Width; ++x)
				{
					const uint16* Found = HeightData.Find(FIntPoint(MinX + x, MinY + y));
					Grid.Add(MakeShared<FJsonValueNumber>(Found ? *Found : (uint16)LandscapeMidHeight));
				}
			}
			Result->SetArrayField(TEXT("samples"), Grid);
			Result->SetBoolField(TEXT("samples_included"), true);
		}
	}

	return Result;
#endif // WITH_EDITOR
}
