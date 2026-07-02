#include "MCPAIRuntimeCommands.h"
#include "MCPCommonUtils.h"
#include "AIController.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig.h"
#include "StateTree.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"

// Helper: get PIE world
static UWorld* GetPIEWorld(TSharedPtr<FJsonObject>& OutError)
{
	if (!GEditor)
	{
		OutError = FMCPCommonUtils::CreateErrorResponse(
			TEXT("GEditor not available"),
			EMCPErrorCode::Internal,
			TEXT("GEditor is null — the editor is shutting down or has not finished initializing. AI-runtime tools only function within an interactive editor session. If reproducible at editor start, retry after the editor is fully loaded."));
		return nullptr;
	}

	// Find PIE world
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			return Context.World();
		}
	}

	OutError = FMCPCommonUtils::CreateErrorResponse(
		TEXT("PIE is not running. Start Play-In-Editor first."),
		EMCPErrorCode::NotInPie,
		TEXT("AI-runtime tools query the live PIE world (where AIControllers, perception, and StateTrees are instantiated). Start Play-In-Editor via the editor toolbar or `start_pie`, then retry."));
	return nullptr;
}

// Helper: find actor by name in PIE world
static AActor* FindActorByName(UWorld* World, const FString& ActorName, TSharedPtr<FJsonObject>& OutError)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->GetActorLabel() == ActorName || It->GetName() == ActorName || It->GetFName().ToString() == ActorName)
		{
			return *It;
		}
	}

	OutError = FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Actor not found in PIE world: '%s'"), *ActorName),
		EMCPErrorCode::ActorNotFound,
		TEXT("No PIE actor matches the given name across actor-label, short-name, or FName. Names are case-sensitive. Use `get_actors_in_level` against the PIE world or `find_actors_by_name` to discover. PIE actors may differ from editor-world actors — verify against the running session."));
	return nullptr;
}

// Helper: get AIController from an actor (pawn or controller)
static AAIController* GetAIController(AActor* Actor)
{
	if (AAIController* Controller = Cast<AAIController>(Actor))
	{
		return Controller;
	}

	if (APawn* Pawn = Cast<APawn>(Actor))
	{
		return Cast<AAIController>(Pawn->GetController());
	}

	return nullptr;
}

// ---- HandleCommand ----

TSharedPtr<FJsonObject> FMCPAIRuntimeCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("ai_get_state"))      return HandleGetAIState(Params);
	if (CommandType == TEXT("ai_get_awareness"))  return HandleGetAIAwareness(Params);
	if (CommandType == TEXT("ai_get_perception")) return HandleGetAIPerception(Params);

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown AI runtime command: %s"), *CommandType),
		EMCPErrorCode::InvalidArgument,
		TEXT("`command` must be one of: get_ai_state, get_ai_awareness, get_ai_perception. All three require PIE to be running."));
}

// ---- GetAIState ----

TSharedPtr<FJsonObject> FMCPAIRuntimeCommands::HandleGetAIState(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UWorld* World = GetPIEWorld(Error);
	if (!World) return Error;

	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'actor_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`actor_name` is required. Pass the actor's label (display name), short name, or FName. Use `get_actors_in_level` or `find_actors_by_name` against the PIE world to discover."));
	}

	AActor* Actor = FindActorByName(World, ActorName, Error);
	if (!Actor) return Error;

	AAIController* Controller = GetAIController(Actor);
	if (!Controller)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Actor '%s' has no AIController"), *ActorName),
			EMCPErrorCode::UnsupportedClass,
			TEXT("This tool requires an actor with an AIController. The resolved actor is either a non-AAIController controller, or a APawn whose Controller is null/non-AI. For pawns, verify AIControllerClass is set on the pawn and that the pawn has spawned its controller (AutoPossessAI). Use `get_actors_in_level` to find pawns with AI controllers."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("controller"), Controller->GetName());

	// Find StateTreeComponent by class name (avoid hard dependency on GameplayStateTreeModule header)
	UActorComponent* STComp = nullptr;
	TArray<UActorComponent*> AllComps;
	Controller->GetComponents(AllComps);
	for (UActorComponent* Comp : AllComps)
	{
		if (Comp && Comp->GetClass()->GetName().Contains(TEXT("StateTree")))
		{
			STComp = Comp;
			break;
		}
	}
	if (!STComp && Controller->GetPawn())
	{
		Controller->GetPawn()->GetComponents(AllComps);
		for (UActorComponent* Comp : AllComps)
		{
			if (Comp && Comp->GetClass()->GetName().Contains(TEXT("StateTree")))
			{
				STComp = Comp;
				break;
			}
		}
	}

	if (STComp)
	{
		Result->SetBoolField(TEXT("has_state_tree"), true);
		Result->SetStringField(TEXT("component_class"), STComp->GetClass()->GetName());
	}
	else
	{
		Result->SetBoolField(TEXT("has_state_tree"), false);
	}

	return Result;
}

// ---- GetAIAwareness ----

TSharedPtr<FJsonObject> FMCPAIRuntimeCommands::HandleGetAIAwareness(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UWorld* World = GetPIEWorld(Error);
	if (!World) return Error;

	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'actor_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`actor_name` is required. Pass the actor's label (display name), short name, or FName. Use `get_actors_in_level` or `find_actors_by_name` against the PIE world to discover."));
	}

	AActor* Actor = FindActorByName(World, ActorName, Error);
	if (!Actor) return Error;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actor"), Actor->GetActorLabel());

	// Look for CombatAwarenessComponent on the actor or its pawn
	APawn* Pawn = Cast<APawn>(Actor);
	if (!Pawn)
	{
		AAIController* Controller = Cast<AAIController>(Actor);
		if (Controller)
		{
			Pawn = Controller->GetPawn();
		}
	}

	if (Pawn)
	{
		// Use FindComponentByClass with the name since we don't want a hard dependency on the game module
		UActorComponent* AwarenessComp = nullptr;
		for (UActorComponent* Comp : Pawn->GetComponents())
		{
			if (Comp && Comp->GetClass()->GetName().Contains(TEXT("CombatAwareness")))
			{
				AwarenessComp = Comp;
				break;
			}
		}

		if (AwarenessComp)
		{
			Result->SetBoolField(TEXT("has_awareness"), true);

			// Serialize all editable properties on the awareness component
			TArray<TSharedPtr<FJsonValue>> PropsArray;
			for (TFieldIterator<FProperty> PropIt(AwarenessComp->GetClass()); PropIt; ++PropIt)
			{
				FProperty* Prop = *PropIt;
				if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
				// Skip inherited ActorComponent properties
				if (Prop->GetOwnerClass() == UActorComponent::StaticClass()) continue;

				TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
				PropObj->SetStringField(TEXT("name"), Prop->GetName());
				PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());

				FString Value;
				const uint8* ValuePtr = Prop->ContainerPtrToValuePtr<uint8>(AwarenessComp);
				Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, AwarenessComp, PPF_None);
				PropObj->SetStringField(TEXT("value"), Value);

				PropsArray.Add(MakeShared<FJsonValueObject>(PropObj));
			}
			Result->SetField(TEXT("awareness_properties"), MakeShared<FJsonValueArray>(PropsArray));
		}
		else
		{
			Result->SetBoolField(TEXT("has_awareness"), false);
		}
	}
	else
	{
		Result->SetBoolField(TEXT("has_awareness"), false);
		Result->SetStringField(TEXT("note"), TEXT("No pawn found for this actor"));
	}

	return Result;
}

// ---- GetAIPerception ----

TSharedPtr<FJsonObject> FMCPAIRuntimeCommands::HandleGetAIPerception(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UWorld* World = GetPIEWorld(Error);
	if (!World) return Error;

	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Missing 'actor_name' parameter"),
			EMCPErrorCode::InvalidArgument,
			TEXT("`actor_name` is required. Pass the actor's label (display name), short name, or FName. Use `get_actors_in_level` or `find_actors_by_name` against the PIE world to discover."));
	}

	AActor* Actor = FindActorByName(World, ActorName, Error);
	if (!Actor) return Error;

	AAIController* Controller = GetAIController(Actor);
	if (!Controller)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Actor '%s' has no AIController"), *ActorName),
			EMCPErrorCode::UnsupportedClass,
			TEXT("This tool requires an actor with an AIController. The resolved actor is either a non-AAIController controller, or a APawn whose Controller is null/non-AI. For pawns, verify AIControllerClass is set on the pawn and that the pawn has spawned its controller (AutoPossessAI). Use `get_actors_in_level` to find pawns with AI controllers."));
	}

	UAIPerceptionComponent* PerceptionComp = Controller->GetPerceptionComponent();
	if (!PerceptionComp)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("has_perception"), false);
		return Result;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("has_perception"), true);

	// Currently perceived actors
	TArray<AActor*> PerceivedActors;
	PerceptionComp->GetCurrentlyPerceivedActors(nullptr, PerceivedActors);

	TArray<TSharedPtr<FJsonValue>> PerceivedArray;
	for (AActor* Perceived : PerceivedActors)
	{
		if (!Perceived) continue;
		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("name"), Perceived->GetActorLabel());
		ActorObj->SetStringField(TEXT("class"), Perceived->GetClass()->GetName());

		FVector Loc = Perceived->GetActorLocation();
		ActorObj->SetStringField(TEXT("location"), FString::Printf(TEXT("X=%.1f Y=%.1f Z=%.1f"), Loc.X, Loc.Y, Loc.Z));

		float Distance = FVector::Dist(Actor->GetActorLocation(), Loc);
		ActorObj->SetNumberField(TEXT("distance"), Distance);

		PerceivedArray.Add(MakeShared<FJsonValueObject>(ActorObj));
	}
	Result->SetField(TEXT("perceived_actors"), MakeShared<FJsonValueArray>(PerceivedArray));
	Result->SetNumberField(TEXT("perceived_count"), PerceivedArray.Num());

	// Known actors (includes actors that were perceived but may no longer be visible)
	TArray<AActor*> KnownActors;
	PerceptionComp->GetKnownPerceivedActors(nullptr, KnownActors);
	Result->SetNumberField(TEXT("known_count"), KnownActors.Num());

	// Report sense configuration info from the component's class
	Result->SetStringField(TEXT("perception_class"), PerceptionComp->GetClass()->GetName());

	return Result;
}
