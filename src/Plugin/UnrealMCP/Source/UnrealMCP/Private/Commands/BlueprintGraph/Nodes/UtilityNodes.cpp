#include "Commands/BlueprintGraph/Nodes/UtilityNodes.h"
#include "Commands/MCPCommonUtils.h"
#include "Commands/BlueprintGraph/Nodes/NodeCreatorUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_Select.h"
#include "K2Node_SpawnActorFromClass.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Animation/AnimInstance.h"
#include "UObject/UObjectIterator.h"
#include "Json.h"

namespace
{
	// Resolve a UClass from a full path (/Script/Engine.Pawn) or a short name
	// (Pawn, APawn, AnimInstance). Short-name path iterates UClass objects and
	// matches against GetName() with and without a UE prefix (U/A) stripped.
	// Mirrors the resolver in NodePropertyManager.cpp — kept local to avoid a
	// header dependency (this file is live-coding-patched independently).
	UClass* ResolveClassForFunctionLookup(const FString& ClassNameOrPath)
	{
		if (ClassNameOrPath.IsEmpty()) return nullptr;

		if (ClassNameOrPath.Contains(TEXT("/")) || ClassNameOrPath.Contains(TEXT(".")))
		{
			// 1. Direct UClass path (already a generated-class / native-class path).
			if (UClass* Direct = LoadClass<UObject>(nullptr, *ClassNameOrPath))
			{
				return Direct;
			}
			// 2. Bare Blueprint asset path (e.g. /Game/UI/WBP_HUD or
			//    /Game/UI/WBP_HUD.WBP_HUD): the generated class is "<path>_C".
			//    GAP-040 — callers shouldn't have to know the _C suffix.
			if (UClass* AsGenerated = LoadClass<UObject>(nullptr, *(ClassNameOrPath + TEXT("_C"))))
			{
				return AsGenerated;
			}
			// 3. Load the Blueprint asset itself and return its GeneratedClass.
			if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *ClassNameOrPath))
			{
				return BP->GeneratedClass;
			}
			return nullptr;
		}

		FString Stripped = ClassNameOrPath;
		if (Stripped.Len() > 1 && (Stripped[0] == TEXT('U') || Stripped[0] == TEXT('A')) &&
			FChar::IsUpper(Stripped[1]))
		{
			Stripped = Stripped.RightChop(1);
		}

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Cls = *It;
			const FString CName = Cls->GetName();
			if (CName == ClassNameOrPath || CName == Stripped)
			{
				return Cls;
			}
		}
		return nullptr;
	}
}

UK2Node* FUtilityNodeCreator::CreatePrintNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid())
	{
		return nullptr;
	}

	UK2Node_CallFunction* PrintNode = NewObject<UK2Node_CallFunction>(Graph);
	if (!PrintNode)
	{
		return nullptr;
	}

	UFunction* PrintFunc = UKismetSystemLibrary::StaticClass()->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString)
	);

	if (!PrintFunc)
	{
		return nullptr;
	}

	// Set function reference BEFORE initialization
	PrintNode->SetFromFunction(PrintFunc);

	double PosX, PosY;
	FNodeCreatorUtils::ExtractNodePosition(Params, PosX, PosY);
	PrintNode->NodePosX = static_cast<int32>(PosX);
	PrintNode->NodePosY = static_cast<int32>(PosY);

	Graph->AddNode(PrintNode, true, false);
	FNodeCreatorUtils::InitializeK2Node(PrintNode, Graph);

	// Set message if provided AFTER initialization
	FString Message;
	if (Params->TryGetStringField(TEXT("message"), Message))
	{
		UEdGraphPin* InStringPin = PrintNode->FindPin(TEXT("InString"));
		if (InStringPin)
		{
			InStringPin->DefaultValue = Message;
		}
	}

	return PrintNode;
}

UK2Node* FUtilityNodeCreator::CreateCallFunctionNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid())
	{
		return nullptr;
	}

	// Get target function name
	FString TargetFunction;
	if (!Params->TryGetStringField(TEXT("target_function"), TargetFunction))
	{
		return nullptr;
	}

	// Guard against FName's NAME_SIZE (1024) fatal assert: a caller-supplied
	// target_function >= NAME_SIZE chars would crash the editor inside FName(*TargetFunction).
	if (TargetFunction.Len() >= NAME_SIZE)
	{
		UE_LOG(LogUnrealMCP, Warning, TEXT("CreateCallFunctionNode: target_function length %d exceeds NAME_SIZE limit"), TargetFunction.Len());
		return nullptr;
	}

	// Find the function to call. Pre-fix, this walked only UKismetSystemLibrary
	// and UKismetMathLibrary when no target_class was given — which meant
	// instance methods on engine classes (AActor::GetVelocity, UAnimInstance::TryGetPawnOwner,
	// etc.) and methods on the blueprint's own class always failed with "Failed
	// to create CallFunction node". Walk three layers:
	//   1. Explicit target_class (bare name, UE-prefixed, or /Script/... path)
	//   2. The owning Blueprint's own class hierarchy (GeneratedClass + ParentClass)
	//   3. Common engine classes — Actor/Pawn/Character/Controller/PlayerController,
	//      AnimInstance, CharacterMovementComponent, and the Kismet*Library / GameplayStatics
	//      static-function libraries.
	UFunction* TargetFunc = nullptr;
	FString ClassName;
	Params->TryGetStringField(TEXT("target_class"), ClassName);

	// 1. Explicit target_class
	if (!ClassName.IsEmpty())
	{
		if (UClass* TargetClass = ResolveClassForFunctionLookup(ClassName))
		{
			TargetFunc = TargetClass->FindFunctionByName(FName(*TargetFunction));
		}
	}

	// 1b. Explicit target_blueprint (cross-BP call) — resolve its generated class.
	//     Uses the same resolver, so it inherits the bare-path "_C" fallback
	//     (GAP-040: target_blueprint was previously read by the server but ignored here).
	if (!TargetFunc)
	{
		FString TargetBlueprint;
		if (Params->TryGetStringField(TEXT("target_blueprint"), TargetBlueprint) && !TargetBlueprint.IsEmpty())
		{
			if (UClass* BPClass = ResolveClassForFunctionLookup(TargetBlueprint))
			{
				TargetFunc = BPClass->FindFunctionByName(FName(*TargetFunction));
			}
		}
	}

	// 2. Blueprint's own class hierarchy (GeneratedClass first, then ParentClass)
	if (!TargetFunc)
	{
		UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph);
		if (Blueprint)
		{
			if (Blueprint->GeneratedClass)
			{
				TargetFunc = Blueprint->GeneratedClass->FindFunctionByName(FName(*TargetFunction));
			}
			if (!TargetFunc && Blueprint->ParentClass)
			{
				TargetFunc = Blueprint->ParentClass->FindFunctionByName(FName(*TargetFunction));
			}
		}
	}

	// 3. Common engine instance classes + static-function libraries
	if (!TargetFunc)
	{
		static UClass* const CommonClasses[] = {
			AActor::StaticClass(),
			APawn::StaticClass(),
			ACharacter::StaticClass(),
			AController::StaticClass(),
			APlayerController::StaticClass(),
			UAnimInstance::StaticClass(),
			UCharacterMovementComponent::StaticClass(),
			UKismetSystemLibrary::StaticClass(),
			UKismetMathLibrary::StaticClass(),
			UGameplayStatics::StaticClass(),
		};
		for (UClass* Cls : CommonClasses)
		{
			if (Cls)
			{
				TargetFunc = Cls->FindFunctionByName(FName(*TargetFunction));
				if (TargetFunc) break;
			}
		}
	}

	if (!TargetFunc)
	{
		return nullptr;
	}

	// GAP-023/029: array library functions (Array_Add / Array_Get / … on
	// UKismetArrayLibrary) carry the `ArrayParm` metadata, and the editor spawns
	// them as UK2Node_CallArrayFunction — the subclass whose
	// NotifyPinConnectionListChanged / PostReconstructNode propagate a connected
	// pin's concrete type into the wildcard TargetArray + element pins. A plain
	// UK2Node_CallFunction leaves them wildcard → compile "type undetermined".
	// Mirror the editor's node-class selection from the resolved UFunction.
	const bool bIsArrayFunc = TargetFunc->HasMetaData(FBlueprintMetadata::MD_ArrayParam);
	UK2Node_CallFunction* CallNode = bIsArrayFunc
		? NewObject<UK2Node_CallFunction>(Graph, UK2Node_CallArrayFunction::StaticClass())
		: NewObject<UK2Node_CallFunction>(Graph);
	if (!CallNode)
	{
		return nullptr;
	}

	// Set function reference BEFORE initialization
	CallNode->SetFromFunction(TargetFunc);

	double PosX, PosY;
	FNodeCreatorUtils::ExtractNodePosition(Params, PosX, PosY);
	CallNode->NodePosX = static_cast<int32>(PosX);
	CallNode->NodePosY = static_cast<int32>(PosY);

	Graph->AddNode(CallNode, true, false);
	FNodeCreatorUtils::InitializeK2Node(CallNode, Graph);

	return CallNode;
}

UK2Node* FUtilityNodeCreator::CreateSelectNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid())
	{
		return nullptr;
	}

	UK2Node_Select* SelectNode = NewObject<UK2Node_Select>(Graph);
	if (!SelectNode)
	{
		return nullptr;
	}

	double PosX, PosY;
	FNodeCreatorUtils::ExtractNodePosition(Params, PosX, PosY);
	SelectNode->NodePosX = static_cast<int32>(PosX);
	SelectNode->NodePosY = static_cast<int32>(PosY);

	Graph->AddNode(SelectNode, true, false);
	FNodeCreatorUtils::InitializeK2Node(SelectNode, Graph);

	return SelectNode;
}

UK2Node* FUtilityNodeCreator::CreateSpawnActorNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& Params)
{
	if (!Graph || !Params.IsValid())
	{
		return nullptr;
	}

	UK2Node_SpawnActorFromClass* SpawnActorNode = NewObject<UK2Node_SpawnActorFromClass>(Graph);
	if (!SpawnActorNode)
	{
		return nullptr;
	}

	double PosX, PosY;
	FNodeCreatorUtils::ExtractNodePosition(Params, PosX, PosY);
	SpawnActorNode->NodePosX = static_cast<int32>(PosX);
	SpawnActorNode->NodePosY = static_cast<int32>(PosY);

	Graph->AddNode(SpawnActorNode, true, false);
	FNodeCreatorUtils::InitializeK2Node(SpawnActorNode, Graph);

	return SpawnActorNode;
}

