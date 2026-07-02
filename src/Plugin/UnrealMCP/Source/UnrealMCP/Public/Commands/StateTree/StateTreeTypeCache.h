#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FStateTreeEditorNode;
class UStateTreeEditorData;
class UStateTreeState;
class UStateTree;

/**
 * Lazy-initialized cache mapping friendly type names to UScriptStruct* pointers
 * for all registered StateTree node types (tasks, conditions, evaluators, considerations).
 * Scans TObjectRange<UScriptStruct> on first use. Invalidate after live coding.
 */
class UNREALMCP_API FStateTreeTypeCache
{
public:
	static FStateTreeTypeCache& Get();

	/** Resolve a type name string (e.g. "FSTTask_FireAtTarget") to its UScriptStruct*. Returns nullptr if not found. */
	UScriptStruct* ResolveNodeType(const FString& TypeName);

	/** List all known node types as JSON, optionally filtered by base_class (task/condition/evaluator/consideration/all) */
	TSharedPtr<FJsonObject> ListNodeTypes(const FString& BaseClassFilter = TEXT("all"), const FString& NamePattern = TEXT(""));

	/** List all known UStateTreeSchema subclasses as JSON */
	TSharedPtr<FJsonObject> ListSchemaTypes();

	/** Force rebuild the cache (e.g. after hot-reload) */
	void Invalidate();

	// ---- Shared helpers used across all StateTree sub-handlers ----

	/** Load a UStateTree asset from a params JSON object (expects "asset_path" field). Returns null and sets OutError on failure. */
	static UStateTree* LoadStateTreeFromParams(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError);

	/** Get UStateTreeEditorData from a loaded UStateTree. Returns null and sets OutError on failure. */
	static UStateTreeEditorData* GetEditorData(UStateTree* StateTree, TSharedPtr<FJsonObject>& OutError);

	/** Find a state by name or GUID string. If ambiguous, sets OutError with all matches. */
	static UStateTreeState* FindState(UStateTreeEditorData* EditorData, const FString& StateIdentifier, TSharedPtr<FJsonObject>& OutError);

	/** Find an FStateTreeEditorNode by GUID anywhere in the tree. Returns pointer + metadata about location. */
	struct FNodeSearchResult
	{
		FStateTreeEditorNode* Node = nullptr;
		UStateTreeState* OwningState = nullptr;
		FString SlotType; // "task", "enter_condition", "consideration", "transition_condition", "evaluator", "global_task"
		int32 ArrayIndex = INDEX_NONE;
	};
	static FNodeSearchResult FindNodeByGuid(UStateTreeEditorData* EditorData, const FGuid& NodeGuid);

	/** Serialize all EditAnywhere properties from an FInstancedStruct to JSON */
	static TSharedPtr<FJsonObject> SerializeInstanceDataProperties(const struct FInstancedStruct& Instance);

	/** Apply property overrides from JSON to an FInstancedStruct via FProperty::ImportText */
	static bool ApplyPropertyOverrides(struct FInstancedStruct& Instance, const TSharedPtr<FJsonObject>& Properties, FString& OutError);

private:
	FStateTreeTypeCache() = default;
	void EnsureBuilt();

	bool bIsBuilt = false;
	TMap<FString, UScriptStruct*> AllNodeTypes;  // name -> struct for all categories
	TMap<FString, FString> TypeCategories;       // name -> "task"/"condition"/"evaluator"/"consideration"

	// Walk state hierarchy recursively to find a state
	static void FindStateRecursive(UStateTreeState* State, const FString& Identifier, bool bIsGuid, TArray<UStateTreeState*>& OutMatches);
	// Walk state hierarchy recursively to find a node by GUID
	static bool FindNodeInState(UStateTreeState* State, const FGuid& NodeGuid, FNodeSearchResult& OutResult);
};
