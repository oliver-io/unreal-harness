// See MCPMeshCommands.cpp for the full file map.
//
// Static-mesh SOCKET authoring — the by-eye positioning workflow's STORAGE mechanism.
// A socket is a named relative transform on a UStaticMesh; UStaticMeshSocket::GetSocketTransform
// composes it with a component's LIVE world transform (rotation + translation + SCALE), so it is
// the correct, scale-proof home for muzzle points, attach/grip anchors, FX emitters, etc. (the
// hand-tuned "pre-scale offset vector" that sent a held gun's muzzle ~20-37 m downrange cannot
// happen with a socket — see room-craft docs/world/ITEMS.md §12).
//
//   - mesh_list_sockets   : enumerate sockets on a UStaticMesh (read, never blocked).
//   - mesh_add_socket      : create a named socket with a relative transform (dry_run; refuses /Engine/).
//   - mesh_modify_socket   : update only the provided fields of an existing socket (dry_run; refuses /Engine/).
//   - mesh_remove_socket   : delete a socket by name (dry_run; refuses /Engine/).
//
// Mirrors the skeletal-socket family (MCPBlueprintCommands.cpp HandleAddSkeletonSocket…) adapted to
// UStaticMesh / UStaticMeshSocket. Persistence uses UEditorAssetLibrary::SaveAsset(only_if_is_dirty=false)
// because the editor-Python add_socket + save_loaded_asset path silently no-ops (never dirties the package).

#include "Commands/MCPMeshCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "EditorAssetLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"

namespace
{
	// NAME-DISTINCT (…ForSocket) so UE unity/jumbo builds don't collide with the identically-shaped
	// ResolveStaticMesh in MCPMeshCommands_Collision.cpp (two anon-namespace fns in one TU → C2084).
	UStaticMesh* ResolveStaticMeshForSocket(const FString& AssetPath)
	{
		return Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(AssetPath));
	}

	// NAME-DISTINCT (…ForSocketList) to avoid a unity-build clash with the identically-shaped
	// FindActorByLabelOrName in MCPMeshCommands.cpp. Used only by the actor/component read path.
	AActor* FindActorForSocketList(UWorld* World, const FString& Name)
	{
		if (!World) return nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A) continue;
			if (A->GetActorLabel() == Name) return A;
			if (A->GetName() == Name)       return A;
		}
		return nullptr;
	}

	/** Build a {location{x,y,z}, rotation{pitch,yaw,roll}, scale{x,y,z}} object from a world transform. */
	TSharedPtr<FJsonObject> SocketWorldTransformJson(const FTransform& W)
	{
		const FVector Loc = W.GetLocation();
		const FRotator Rot = W.Rotator();
		const FVector Scale = W.GetScale3D();

		TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
		L->SetNumberField(TEXT("x"), Loc.X);
		L->SetNumberField(TEXT("y"), Loc.Y);
		L->SetNumberField(TEXT("z"), Loc.Z);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetNumberField(TEXT("pitch"), Rot.Pitch);
		R->SetNumberField(TEXT("yaw"), Rot.Yaw);
		R->SetNumberField(TEXT("roll"), Rot.Roll);

		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetNumberField(TEXT("x"), Scale.X);
		S->SetNumberField(TEXT("y"), Scale.Y);
		S->SetNumberField(TEXT("z"), Scale.Z);

		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("location"), L);
		O->SetObjectField(TEXT("rotation"), R);
		O->SetObjectField(TEXT("scale"), S);
		return O;
	}

	/** Serialize one UStaticMeshSocket as { socket_name, index, location{x,y,z}, rotation{pitch,yaw,roll}, scale{x,y,z}, tag }. */
	TSharedPtr<FJsonObject> StaticSocketToJson(const UStaticMeshSocket* Socket, int32 Index)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("socket_name"), Socket->SocketName.ToString());
		Obj->SetNumberField(TEXT("index"), Index);

		TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
		Loc->SetNumberField(TEXT("x"), Socket->RelativeLocation.X);
		Loc->SetNumberField(TEXT("y"), Socket->RelativeLocation.Y);
		Loc->SetNumberField(TEXT("z"), Socket->RelativeLocation.Z);
		Obj->SetObjectField(TEXT("location"), Loc);

		TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
		Rot->SetNumberField(TEXT("pitch"), Socket->RelativeRotation.Pitch);
		Rot->SetNumberField(TEXT("yaw"), Socket->RelativeRotation.Yaw);
		Rot->SetNumberField(TEXT("roll"), Socket->RelativeRotation.Roll);
		Obj->SetObjectField(TEXT("rotation"), Rot);

		TSharedPtr<FJsonObject> Scale = MakeShared<FJsonObject>();
		Scale->SetNumberField(TEXT("x"), Socket->RelativeScale.X);
		Scale->SetNumberField(TEXT("y"), Socket->RelativeScale.Y);
		Scale->SetNumberField(TEXT("z"), Socket->RelativeScale.Z);
		Obj->SetObjectField(TEXT("scale"), Scale);

		Obj->SetStringField(TEXT("tag"), Socket->Tag);
		return Obj;
	}

	/**
	 * Resolve the target UStaticMesh from `asset_path`. For mutators (bMutator=true) refuse /Engine/ content.
	 * On failure returns nullptr and sets OutError to a ready-to-return envelope.
	 */
	UStaticMesh* ResolveSocketMesh(const TSharedPtr<FJsonObject>& Params, bool bMutator, TSharedPtr<FJsonObject>& OutError)
	{
		FString AssetPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			OutError = FMCPCommonUtils::CreateErrorResponse(
				TEXT("Missing 'asset_path' parameter"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`asset_path` is required (string) — the object/package path to an existing UStaticMesh, e.g. `/Game/Weapons/PSX/SM_1911_Gun`. Use `list_assets` with asset_type='StaticMesh' to discover."));
			return nullptr;
		}
		if (bMutator && AssetPath.StartsWith(TEXT("/Engine/")))
		{
			OutError = FMCPCommonUtils::CreateErrorResponse(
				TEXT("Refusing to author a socket on engine content"),
				EMCPErrorCode::InvalidArgument,
				TEXT("`asset_path` points at `/Engine/` content. Adding/modifying/removing a socket mutates and re-saves the asset; engine content must not be modified. Duplicate the mesh into project (`/Game/...`) or plugin content first."));
			return nullptr;
		}
		UStaticMesh* Mesh = ResolveStaticMeshForSocket(AssetPath);
		if (!Mesh)
		{
			OutError = FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Could not load a UStaticMesh at: %s"), *AssetPath),
				EMCPErrorCode::AssetNotFound,
				TEXT("`asset_path` did not resolve to a UStaticMesh. Verify the path (object/package path, case-sensitive) and that the asset is a static mesh. Use `list_assets` with `asset_type='StaticMesh'`."));
			return nullptr;
		}
		return Mesh;
	}

	/** Persist a socket-mutated mesh; returns an error envelope on save failure, else nullptr. */
	TSharedPtr<FJsonObject> SaveSocketMesh(UStaticMesh* Mesh, const TCHAR* What)
	{
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
		if (!UEditorAssetLibrary::SaveAsset(Mesh->GetPathName(), /*bOnlyIfIsDirty=*/false))
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Socket %s in-memory but failed to persist to disk: %s"), What, *Mesh->GetPathName()),
				EMCPErrorCode::Internal,
				TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written. SaveAsset no-ops while PIE is active or when the package is read-only / checked out. Stop PIE, ensure the target folder is writable, and retry; the change exists in-memory but will be lost on editor restart."));
		}
		return nullptr;
	}
}

// ─── mesh_list_sockets ──────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPMeshCommands::HandleListStaticMeshSockets(const TSharedPtr<FJsonObject>& Params)
{
	// Two resolution paths:
	//   - `actor` (+ optional `component`): read the mesh off a live level actor's
	//     UStaticMeshComponent — additionally returns each socket's resolved WORLD transform.
	//   - `asset_path` / `static_mesh_path`: load the UStaticMesh asset directly (relative only).
	FString ActorName;
	const bool bHasActor = Params.IsValid() && Params->TryGetStringField(TEXT("actor"), ActorName) && !ActorName.IsEmpty();

	UStaticMesh* Mesh = nullptr;
	UStaticMeshComponent* Comp = nullptr; // set only on the actor/component path

	if (bHasActor)
	{
		FString CompName;
		Params->TryGetStringField(TEXT("component"), CompName);

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		AActor* Actor = FindActorForSocketList(World, ActorName);
		if (!Actor)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("Actor not found: %s"), *ActorName),
				EMCPErrorCode::ActorNotFound,
				TEXT("`actor` did not match a level actor by label or name. Use `get_actors_in_level` / `find_actors_by_name` to discover actors."));
		}

		TArray<UStaticMeshComponent*> Comps;
		Actor->GetComponents<UStaticMeshComponent>(Comps);
		for (UStaticMeshComponent* C : Comps)
		{
			if (C && (CompName.IsEmpty() || C->GetName() == CompName))
			{
				Comp = C;
				break;
			}
		}
		if (!Comp)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				FString::Printf(TEXT("No matching UStaticMeshComponent on '%s'"), *ActorName),
				EMCPErrorCode::InvalidArgument,
				CompName.IsEmpty()
					? TEXT("The actor has no UStaticMeshComponent.")
					: TEXT("`component` did not match a UStaticMeshComponent on the actor (names are case-sensitive). Omit `component` to take the first."));
		}

		Mesh = Comp->GetStaticMesh();
		if (!Mesh)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("Component has no UStaticMesh assigned"),
				EMCPErrorCode::InvalidArgument,
				TEXT("The matched StaticMeshComponent has no mesh set, so it has no sockets to list."));
		}
	}
	else
	{
		// Accept `static_mesh_path` as an alias for `asset_path` on the read path.
		if (Params.IsValid() && !Params->HasField(TEXT("asset_path")))
		{
			FString AltPath;
			if (Params->TryGetStringField(TEXT("static_mesh_path"), AltPath) && !AltPath.IsEmpty())
			{
				Params->SetStringField(TEXT("asset_path"), AltPath);
			}
		}
		TSharedPtr<FJsonObject> Error;
		Mesh = ResolveSocketMesh(Params, /*bMutator=*/false, Error);
		if (!Mesh) return Error;
	}

	TArray<TSharedPtr<FJsonValue>> SocketArray;
	for (int32 i = 0; i < Mesh->Sockets.Num(); ++i)
	{
		UStaticMeshSocket* Socket = Mesh->Sockets[i];
		if (!Socket) continue;

		TSharedPtr<FJsonObject> Obj = StaticSocketToJson(Socket, i);
		if (Comp) // live-component path: include the resolved world transform
		{
			Obj->SetObjectField(TEXT("world"),
				SocketWorldTransformJson(Comp->GetSocketTransform(Socket->SocketName, RTS_World)));
		}
		SocketArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
	if (Comp) ResultObj->SetStringField(TEXT("component"), Comp->GetName());
	ResultObj->SetArrayField(TEXT("sockets"), SocketArray);
	ResultObj->SetNumberField(TEXT("count"), SocketArray.Num());
	ResultObj->SetBoolField(TEXT("success"), true);
	return ResultObj;
}

// ─── mesh_add_socket ────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPMeshCommands::HandleAddStaticMeshSocket(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStaticMesh* Mesh = ResolveSocketMesh(Params, /*bMutator=*/true, Error);
	if (!Mesh) return Error;

	FString SocketName;
	if (!Params->TryGetStringField(TEXT("socket_name"), SocketName) || SocketName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'socket_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`socket_name` is required (string, the FName of the new socket — unique on this mesh). Use `mesh_list_sockets` to enumerate existing sockets."));
	}

	if (Mesh->FindSocket(FName(*SocketName)))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Socket '%s' already exists on this mesh"), *SocketName),
			EMCPErrorCode::NameCollision,
			TEXT("A socket with the requested `socket_name` already exists on this UStaticMesh. Pick a different name, or call `mesh_modify_socket` to update it, or `mesh_remove_socket` first to replace it."));
	}

	// Parse transform up-front so dry-run and apply share parsing. Defaults: location/rotation = identity, scale = unit.
	double X = 0, Y = 0, Z = 0;
	Params->TryGetNumberField(TEXT("location_x"), X);
	Params->TryGetNumberField(TEXT("location_y"), Y);
	Params->TryGetNumberField(TEXT("location_z"), Z);
	double Pitch = 0, Yaw = 0, Roll = 0;
	Params->TryGetNumberField(TEXT("rotation_pitch"), Pitch);
	Params->TryGetNumberField(TEXT("rotation_yaw"), Yaw);
	Params->TryGetNumberField(TEXT("rotation_roll"), Roll);
	double SX = 1, SY = 1, SZ = 1;
	Params->TryGetNumberField(TEXT("scale_x"), SX);
	Params->TryGetNumberField(TEXT("scale_y"), SY);
	Params->TryGetNumberField(TEXT("scale_z"), SZ);

	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
		Entry->SetStringField(TEXT("socket_name"), SocketName);
		Entry->SetNumberField(TEXT("would_be_index"), Mesh->Sockets.Num());
		TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
		Loc->SetNumberField(TEXT("x"), X); Loc->SetNumberField(TEXT("y"), Y); Loc->SetNumberField(TEXT("z"), Z);
		Entry->SetObjectField(TEXT("location"), Loc);
		TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
		Rot->SetNumberField(TEXT("pitch"), Pitch); Rot->SetNumberField(TEXT("yaw"), Yaw); Rot->SetNumberField(TEXT("roll"), Roll);
		Entry->SetObjectField(TEXT("rotation"), Rot);
		TSharedPtr<FJsonObject> Scale = MakeShared<FJsonObject>();
		Scale->SetNumberField(TEXT("x"), SX); Scale->SetNumberField(TEXT("y"), SY); Scale->SetNumberField(TEXT("z"), SZ);
		Entry->SetObjectField(TEXT("scale"), Scale);

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("sockets_added"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	UStaticMeshSocket* Socket = NewObject<UStaticMeshSocket>(Mesh);
	Socket->SocketName = FName(*SocketName);
	Socket->RelativeLocation = FVector(X, Y, Z);
	Socket->RelativeRotation = FRotator(Pitch, Yaw, Roll);
	Socket->RelativeScale = FVector(SX, SY, SZ);

	Mesh->PreEditChange(nullptr);
	Mesh->AddSocket(Socket);
	if (TSharedPtr<FJsonObject> SaveErr = SaveSocketMesh(Mesh, TEXT("added"))) return SaveErr;

	TSharedPtr<FJsonObject> ResultObj = StaticSocketToJson(Socket, Mesh->Sockets.Num() - 1);
	ResultObj->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
	ResultObj->SetBoolField(TEXT("success"), true);
	return ResultObj;
}

// ─── mesh_modify_socket ─────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPMeshCommands::HandleModifyStaticMeshSocket(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStaticMesh* Mesh = ResolveSocketMesh(Params, /*bMutator=*/true, Error);
	if (!Mesh) return Error;

	FString SocketName;
	if (!Params->TryGetStringField(TEXT("socket_name"), SocketName) || SocketName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'socket_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`socket_name` is required (string, the FName of an existing socket). Use `mesh_list_sockets` to enumerate."));
	}

	UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
	if (!Socket)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Socket '%s' not found"), *SocketName),
			EMCPErrorCode::NodeNotFound,
			TEXT("`socket_name` did not match any socket on the UStaticMesh via FindSocket. Socket names are case-sensitive FNames. Use `mesh_list_sockets` to enumerate."));
	}

	// dry_run: report which fields WOULD change (only the provided ones). No mutation.
	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
		Entry->SetStringField(TEXT("socket_name"), SocketName);
		double Tmp;
		TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
		if (Params->TryGetNumberField(TEXT("location_x"), Tmp)) Fields->SetNumberField(TEXT("location_x"), Tmp);
		if (Params->TryGetNumberField(TEXT("location_y"), Tmp)) Fields->SetNumberField(TEXT("location_y"), Tmp);
		if (Params->TryGetNumberField(TEXT("location_z"), Tmp)) Fields->SetNumberField(TEXT("location_z"), Tmp);
		if (Params->TryGetNumberField(TEXT("rotation_pitch"), Tmp)) Fields->SetNumberField(TEXT("rotation_pitch"), Tmp);
		if (Params->TryGetNumberField(TEXT("rotation_yaw"), Tmp)) Fields->SetNumberField(TEXT("rotation_yaw"), Tmp);
		if (Params->TryGetNumberField(TEXT("rotation_roll"), Tmp)) Fields->SetNumberField(TEXT("rotation_roll"), Tmp);
		if (Params->TryGetNumberField(TEXT("scale_x"), Tmp)) Fields->SetNumberField(TEXT("scale_x"), Tmp);
		if (Params->TryGetNumberField(TEXT("scale_y"), Tmp)) Fields->SetNumberField(TEXT("scale_y"), Tmp);
		if (Params->TryGetNumberField(TEXT("scale_z"), Tmp)) Fields->SetNumberField(TEXT("scale_z"), Tmp);
		FString TagVal;
		if (Params->TryGetStringField(TEXT("tag"), TagVal)) Fields->SetStringField(TEXT("tag"), TagVal);
		Entry->SetObjectField(TEXT("changed_fields"), Fields);

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("sockets_modified"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	Mesh->PreEditChange(nullptr);
	double Val;
	if (Params->TryGetNumberField(TEXT("location_x"), Val)) Socket->RelativeLocation.X = Val;
	if (Params->TryGetNumberField(TEXT("location_y"), Val)) Socket->RelativeLocation.Y = Val;
	if (Params->TryGetNumberField(TEXT("location_z"), Val)) Socket->RelativeLocation.Z = Val;
	if (Params->TryGetNumberField(TEXT("rotation_pitch"), Val)) Socket->RelativeRotation.Pitch = Val;
	if (Params->TryGetNumberField(TEXT("rotation_yaw"), Val)) Socket->RelativeRotation.Yaw = Val;
	if (Params->TryGetNumberField(TEXT("rotation_roll"), Val)) Socket->RelativeRotation.Roll = Val;
	if (Params->TryGetNumberField(TEXT("scale_x"), Val)) Socket->RelativeScale.X = Val;
	if (Params->TryGetNumberField(TEXT("scale_y"), Val)) Socket->RelativeScale.Y = Val;
	if (Params->TryGetNumberField(TEXT("scale_z"), Val)) Socket->RelativeScale.Z = Val;
	FString NewTag;
	if (Params->TryGetStringField(TEXT("tag"), NewTag)) Socket->Tag = NewTag;

	if (TSharedPtr<FJsonObject> SaveErr = SaveSocketMesh(Mesh, TEXT("modified"))) return SaveErr;

	int32 Idx = Mesh->Sockets.IndexOfByKey(Socket);
	TSharedPtr<FJsonObject> ResultObj = StaticSocketToJson(Socket, Idx);
	ResultObj->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
	ResultObj->SetBoolField(TEXT("success"), true);
	return ResultObj;
}

// ─── mesh_remove_socket ─────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FMCPMeshCommands::HandleRemoveStaticMeshSocket(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UStaticMesh* Mesh = ResolveSocketMesh(Params, /*bMutator=*/true, Error);
	if (!Mesh) return Error;

	FString SocketName;
	if (!Params->TryGetStringField(TEXT("socket_name"), SocketName) || SocketName.IsEmpty())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'socket_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`socket_name` is required (string, the FName of an existing socket). Use `mesh_list_sockets` to enumerate."));
	}

	UStaticMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
	if (!Socket)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Socket '%s' not found"), *SocketName),
			EMCPErrorCode::NodeNotFound,
			TEXT("`socket_name` did not match any socket on the UStaticMesh via FindSocket. Socket names are case-sensitive FNames. Use `mesh_list_sockets` to enumerate."));
	}

	if (FMCPCommonUtils::ParseDryRun(Params))
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
		Entry->SetStringField(TEXT("socket_name"), SocketName);
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetArrayField(TEXT("sockets_removed"), Arr);
		return FMCPCommonUtils::CreateDryRunResponse(Diff);
	}

	Mesh->PreEditChange(nullptr);
	Mesh->RemoveSocket(Socket);
	if (TSharedPtr<FJsonObject> SaveErr = SaveSocketMesh(Mesh, TEXT("removed"))) return SaveErr;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
	ResultObj->SetStringField(TEXT("removed_socket"), SocketName);
	ResultObj->SetNumberField(TEXT("remaining_sockets"), Mesh->Sockets.Num());
	ResultObj->SetBoolField(TEXT("success"), true);
	return ResultObj;
}
