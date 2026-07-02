#pragma once

#include "CoreMinimal.h"
#include "Json.h"

// Single log category for the whole UnrealMCP plugin (replaces scattered LogTemp
// usage so plugin output is filterable). Defined in MCPCommonUtils.cpp.
DECLARE_LOG_CATEGORY_EXTERN(LogUnrealMCP, Log, All);

// Forward declarations
class AActor;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UK2Node_Event;
class UK2Node_CallFunction;
class UK2Node_VariableGet;
class UK2Node_VariableSet;
class UK2Node_InputAction;
class UK2Node_Self;
class UFunction;
class UMaterial;

/**
 * Closed taxonomy of structured error codes emitted by handler responses.
 * Wire form is flat snake_case: "asset_not_found", "invalid_argument", etc.
 * Mirrors todo/1_error_envelopes.md (groups: identity / input shape / asset state / capability / authority / engine).
 * Un-migrated handlers omit error_code entirely (legacy CreateErrorResponse(Message) path); never emit a fallback string.
 */
enum class EMCPErrorCode : uint8
{
    // Identity / lookup
    AssetNotFound,
    ClassNotLoaded,
    NodeNotFound,
    ActorNotFound,
    VariableNotFound,
    PinNotFound,
    FunctionNotFound,
    UnknownTag,

    // Input shape
    InvalidArgument,
    InvalidPath,
    InvalidPinType,
    AmbiguousTarget,
    OutOfRange,

    // Asset state
    AssetDirty,
    AssetCompileFailed,
    AssetLocked,
    NameCollision,

    // Capability
    UnsupportedClass,
    NotInPie,
    FeatureDisabled,
    DryRunUnsupported,

    // Authority / safety
    WouldBreakReferences,
    CircularDependency,

    // Engine
    EngineBusy,
    LiveCodingUnavailable,
    CompileInProgress,
    Internal,

    // Execution / transport — an async, end-of-frame operation (e.g. an editor
    // viewport screenshot serviced by FScreenshotRequest) did not complete within
    // a bounded budget. Returned instead of a false "requested" when the editor
    // viewport could not render a qualifying frame (occluded / minimized /
    // realtime-off). See GAP-007. Recoverable by foregrounding the editor.
    Timeout,

    // Identity (added post-doc-1; documented in todo/11_editor_diagnostics.md)
    WindowNotFound,

    // Editor lifecycle — bridge boot gate. Returned by ExecuteCommand on the
    // server thread when a command arrives before the editor is fully
    // initialized; the command is NOT dispatched. Recoverable by waiting (poll
    // mcp_status). See docs/bugs/mcp.md "Sidecar replays queued/in-flight commands".
    EditorNotReady,

    // Editor lifecycle — PIE gate. Returned by ExecuteCommand on the server
    // thread when an asset-mutating / asset-loading command arrives while a
    // Play-In-Editor session is running; the command is NOT dispatched. During
    // PIE, the editor's asset-load path (UEditorAssetLibrary::LoadAsset →
    // CheckIfInEditorAndPIE) returns null, so these commands would otherwise fail
    // with a misleading "asset not found" / intermittent socket error. Recoverable
    // by stopping PIE. See docs/bugs/mcp.md "An active PIE session silently blocks
    // asset-mutation / asset-load commands".
    PieActive,
};

/**
 * Common utilities for MCP commands
 */
class UNREALMCP_API FMCPCommonUtils
{
public:
    // JSON utilities
    static TSharedPtr<FJsonObject> CreateErrorResponse(const FString& Message);
    static TSharedPtr<FJsonObject> CreateErrorResponse(const FString& Message, EMCPErrorCode Code, const FString& Hint = FString());
    static FString MCPErrorCodeToString(EMCPErrorCode Code);
    static TSharedPtr<FJsonObject> CreateSuccessResponse(const TSharedPtr<FJsonObject>& Data = nullptr);

    /** Finalize a base-UMaterial edit so the change actually reaches the renderer
     *  AND every dependent material instance. A bare Material->PostEditChange()
     *  recompiles only the UMaterial itself; a mesh slot almost always renders a
     *  child UMaterialInstance, whose uniform-expression set (texture bindings) is
     *  refreshed only by FMaterialUpdateContext's destructor (UpdateCachedData +
     *  RecacheUniformExpressions + InitStaticPermutation on each child). Without it,
     *  instances whose parent graph changed keep a stale, textureless render proxy
     *  and draw as the default grey-checkerboard ("false-compiled" state that
     *  otherwise only clears by opening the asset in the Material Editor). Mirrors
     *  UMaterialEditingLibrary::RecompileMaterialInternal without a MaterialEditor
     *  module dependency. Call this in place of a bare PostEditChange()+MarkPackageDirty()
     *  after mutating a UMaterial's graph/properties. No-op on null. */
    static void RecompileMaterialWithDependents(UMaterial* Material);

    /** Persist a just-mutated UMaterial to disk (GAP-062). The material-graph
     *  mutators (add/set/delete expression, connect, compile) used to only
     *  MarkPackageDirty via RecompileMaterialWithDependents and relied on a
     *  promised server auto-save that never covered this path — so every graph
     *  edit was silently reverted on the next editor reload. Per the repo
     *  contract ("the server auto-saves; callers don't call save"; every
     *  successful asset mutation persists by default), each graph mutator now
     *  calls this at its successful tail. Returns nullptr on success, or a ready
     *  error response (EMCPErrorCode::Internal) when SaveLoadedAsset fails (e.g.
     *  PIE active or a read-only/checked-out package). No-op (nullptr) on null.
     *  Must be called only on the COMMIT path — never on a dry_run early return. */
    static TSharedPtr<FJsonObject> SaveMaterialOrError(UMaterial* Material);

    /** dry_run plumbing — see todo/13_dry_run_plumbing.md.
     *  Mutators that opt in route through these helpers so the on-the-wire
     *  shape stays consistent across handlers and the diff field never leaks
     *  into a commit response (or vice versa). */
    static bool ParseDryRun(const TSharedPtr<FJsonObject>& Params);
    /** Wrap a per-subsystem diff payload as a successful dry-run response.
     *  Sets success=true, dry_run=true, and stores the diff under "diff". */
    static TSharedPtr<FJsonObject> CreateDryRunResponse(const TSharedPtr<FJsonObject>& Diff);
    /** Standard response for a tool that received dry_run=true but does not
     *  yet support the keyword. Matches the doc-13 mid-rollout contract. */
    static TSharedPtr<FJsonObject> CreateDryRunUnsupportedResponse(const FString& ToolName);

    /** Bridge-level safety net for the doc-13 contract: returns true when the
     *  named command is a known mutator that does NOT support dry_run yet.
     *  The bridge intercepts these and returns CreateDryRunUnsupportedResponse
     *  rather than dispatching with dry_run silently ignored. Currently flags:
     *
     *    - `add_node` / `add_blueprint_node` (alias) — BLOCKED per doc-13 status:
     *      the per-type dispatch tree (~67 node types across 6 creator files) is
     *      co-mingled with construction (every creator does NewObject+AddNode),
     *      so per-type validation extraction is out of scope for autonomous iter.
     *
     *  Net-add: read tools and dry_run-supporting mutators pass through unchanged.
     *  Adding a new entry codifies a known gap; removing one means the mutator
     *  has shipped dry_run support. */
    static bool IsBlockedFromDryRun(const FString& CommandType);

    /** PIE gate predicate — returns true when the command loads or mutates a
     *  content asset on the game thread and therefore must NOT run while a
     *  Play-In-Editor session is active (UEditorAssetLibrary::LoadAsset returns
     *  null during PIE, surfacing as misleading "asset not found" / socket errors).
     *  The bridge refuses these with EMCPErrorCode::PieActive while PIE runs.
     *
     *  This is a BLOCKLIST (asset mutate/load commands), not an allowlist of
     *  PIE-safe commands — so PIE-driving automation (start/stop PIE, input,
     *  screenshots, console, AI-runtime reads) and registry/world reads stay
     *  allowed by default and cannot be accidentally broken. A command missing
     *  from this set simply keeps its prior (pre-gate) behavior during PIE. */
    static bool IsBlockedDuringPie(const FString& CommandType);

    /** Domain-first naming alias resolver — see todo/14_naming_migration.md.
     *  Canonical (new) names are routed to their existing dispatched names so
     *  the rest of the bridge stays unchanged. Net-add: unrecognized inputs
     *  pass through unchanged. */
    static void GetIntArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<int32>& OutArray);
    static void GetFloatArrayFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray);
    static FVector2D GetVector2DFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    static FVector GetVectorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    static FRotator GetRotatorFromJson(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);
    
    // Actor utilities
    static TSharedPtr<FJsonValue> ActorToJson(AActor* Actor);
    static TSharedPtr<FJsonObject> ActorToJsonObject(AActor* Actor, bool bDetailed = false);
    
    /** Resolve a UClass from a flexible string. Accepts (in priority order):
     *    - native class path        ("/Script/Engine.PointLight")
     *    - Blueprint asset path      ("/Game/.../BP_Foo" → _C appended, or
     *                                 ".../BP_Foo.BP_Foo_C") → its GeneratedClass
     *    - short native name         ("Pawn", "APawn", "UObject")
     *    - bare Blueprint asset name ("BP_Foo") → its GeneratedClass (loads the
     *                                 asset via the Asset Registry if unloaded)
     *  Returns nullptr if nothing resolves. This is the single shared class-path
     *  resolver reused by actor_spawn, object/class Blueprint variables, node
     *  class/object pin defaults, and DynamicCast target resolution. */
    static UClass* ResolveClass(const FString& Name);

    // Blueprint utilities
    static UBlueprint* FindBlueprint(const FString& BlueprintName);
    static UBlueprint* FindBlueprintByName(const FString& BlueprintName);
    static UEdGraph* FindOrCreateEventGraph(UBlueprint* Blueprint);

    /** Find a graph by name, searching UbergraphPages, FunctionGraphs, DelegateSignatureGraphs,
     *  and recursively inside state machine sub-graphs (states, transitions).
     *  When TransFromState and TransToState are both non-empty, disambiguates
     *  transition rule graphs (which share the name "Transition") by matching
     *  the source and target state names. */
    static UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName,
        const FString& TransFromState = TEXT(""), const FString& TransToState = TEXT(""));
    
    // Blueprint node utilities
    static UK2Node_Event* CreateEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position);
    static UK2Node_CallFunction* CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, const FVector2D& Position);
    static UK2Node_VariableGet* CreateVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position);
    static UK2Node_VariableSet* CreateVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position);
    static UK2Node_InputAction* CreateInputActionNode(UEdGraph* Graph, const FString& ActionName, const FVector2D& Position);
    static UK2Node_Self* CreateSelfReferenceNode(UEdGraph* Graph, const FVector2D& Position);
    static bool ConnectGraphNodes(UEdGraph* Graph, UEdGraphNode* SourceNode, const FString& SourcePinName, 
                                UEdGraphNode* TargetNode, const FString& TargetPinName);
    static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX);
    static UK2Node_Event* FindExistingEventNode(UEdGraph* Graph, const FString& EventName);

    // Property utilities
    static bool SetObjectProperty(UObject* Object, const FString& PropertyName,
                                 const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage);

    // Dotted-path variant: walks struct/object segments (e.g.
    // "BodyInstance.bNotifyRigidBodyCollision") before setting the leaf. Single-segment
    // paths delegate to SetObjectProperty.
    static bool SetObjectPropertyByPath(UObject* Root, const FString& DottedPath,
                                 const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage);
};