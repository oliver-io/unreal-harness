#include "Commands/MCPWidgetCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace
{
    // Mirror the destination-resolution helper from MCPAssetFactoryCommands.
    // Accept either {path, name} or {asset_path}.
    bool ResolveWidgetDestination(const TSharedPtr<FJsonObject>& Params, FString& OutPackagePath, FString& OutAssetName, FString& OutFullPath, FString& OutError)
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

        if (Path.IsEmpty() || Name.IsEmpty())
        {
            OutError = TEXT("Missing destination — pass either {path, name} or {asset_path}.");
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

    UClass* ResolveUserWidgetParentClass(const FString& ParentClassPath)
    {
        if (ParentClassPath.IsEmpty())
        {
            return UUserWidget::StaticClass();
        }

        // Asset path → append _C if missing.
        FString Path = ParentClassPath;
        if (Path.StartsWith(TEXT("/")))
        {
            if (!Path.Contains(TEXT(".")))
            {
                const FString ShortName = FPackageName::GetShortName(Path);
                Path = FString::Printf(TEXT("%s.%s_C"), *Path, *ShortName);
            }
            if (UClass* Loaded = LoadClass<UUserWidget>(nullptr, *Path))
            {
                return Loaded;
            }
        }

        if (UClass* Found = FindFirstObject<UClass>(*ParentClassPath, EFindFirstObjectOptions::ExactClass))
        {
            return Found;
        }
        return nullptr;
    }

    TSharedPtr<FJsonObject> SlotToJson(const UPanelSlot* Slot)
    {
        if (!Slot)
        {
            return nullptr;
        }
        TSharedPtr<FJsonObject> SlotObj = MakeShared<FJsonObject>();
        SlotObj->SetStringField(TEXT("slot_class"), Slot->GetClass()->GetPathName());

        // Generic property dump: every UPROPERTY on the slot. Slot-specific shapes
        // (CanvasPanelSlot anchors, HorizontalBoxSlot fill, etc.) all surface here
        // by name and exported text without per-class branching.
        TSharedPtr<FJsonObject> SlotProps = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> It(Slot->GetClass()); It; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop || !Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
            {
                continue;
            }
            FString Exported;
            Prop->ExportTextItem_Direct(Exported,
                Prop->ContainerPtrToValuePtr<void>(Slot),
                /*Defaults*/ nullptr,
                /*Parent*/ nullptr,
                PPF_None);
            SlotProps->SetStringField(Prop->GetName(), Exported);
        }
        SlotObj->SetObjectField(TEXT("slot_properties"), SlotProps);
        return SlotObj;
    }

    TSharedPtr<FJsonObject> WidgetToJson(const UWidget* W)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), W->GetName());
        Entry->SetStringField(TEXT("class"), W->GetClass()->GetPathName());

        if (const UWidget* Parent = W->GetParent())
        {
            Entry->SetStringField(TEXT("parent"), Parent->GetName());
        }

        if (const TSharedPtr<FJsonObject> SlotJson = SlotToJson(W->Slot))
        {
            Entry->SetObjectField(TEXT("slot"), SlotJson);
        }
        return Entry;
    }
}

TSharedPtr<FJsonObject> FMCPWidgetCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("widget_create"))
    {
        return HandleWidgetCreate(Params);
    }
    if (CommandType == TEXT("widget_tree_read"))
    {
        return HandleWidgetTreeRead(Params);
    }
    if (CommandType == TEXT("widget_add_child"))
    {
        return HandleWidgetAddChild(Params);
    }
    if (CommandType == TEXT("widget_bind_handler"))
    {
        return HandleWidgetBindHandler(Params);
    }
    if (CommandType == TEXT("widget_set_property"))
    {
        return HandleWidgetSetProperty(Params);
    }
    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown widget command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Supported widget commands in this build: widget_create, widget_tree_read, widget_add_child, widget_bind_handler, widget_set_property."));
}

TSharedPtr<FJsonObject> FMCPWidgetCommands::HandleWidgetCreate(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, AssetName, FullAssetPath, ResolveError;
    if (!ResolveWidgetDestination(Params, PackagePath, AssetName, FullAssetPath, ResolveError))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            ResolveError,
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass either {path:'/Game/UI', name:'WBP_Foo'} or {asset_path:'/Game/UI/WBP_Foo'}."));
    }
    if (UEditorAssetLibrary::DoesAssetExist(FullAssetPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset already exists: %s"), *FullAssetPath),
            EMCPErrorCode::NameCollision,
            TEXT("No silent overwrite. Pick a different path or delete the existing asset first."));
    }

    // Optional parent class — defaults to UUserWidget if omitted.
    FString ParentClassPath;
    Params->TryGetStringField(TEXT("parent_class"), ParentClassPath);
    UClass* ParentClass = ResolveUserWidgetParentClass(ParentClassPath);
    if (!ParentClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve parent class: %s"), *ParentClassPath),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("Pass a UUserWidget or subclass — script path (\"/Script/UMG.UserWidget\"), asset path (\"/Game/UI/WBP_Base\"), or short name."));
    }
    if (!ParentClass->IsChildOf(UUserWidget::StaticClass()))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Parent class is not a UUserWidget subclass: %s"), *ParentClass->GetPathName()),
            EMCPErrorCode::UnsupportedClass,
            TEXT("Widget Blueprints must extend UUserWidget."));
    }

    UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
    Factory->ParentClass = ParentClass;

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    UObject* CreatedObj = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, UWidgetBlueprint::StaticClass(), Factory);
    UWidgetBlueprint* Created = Cast<UWidgetBlueprint>(CreatedObj);
    if (!Created)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("UAssetTools::CreateAsset returned null for the widget Blueprint"),
            EMCPErrorCode::EngineBusy,
            TEXT("UE rejected the creation; check the editor log. Common cause: parent UClass not yet loaded into the editor."));
    }

    Created->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(FullAssetPath, /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget Blueprint created in-editor but saving to disk failed: %s"), *FullAssetPath),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the package was not written (read-only file, source-control checkout failure, or a save-blocked PIE/unattended session). Do not report success."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("path"), FullAssetPath);
    ResultObj->SetStringField(TEXT("class"), TEXT("/Script/UMGEditor.WidgetBlueprint"));
    ResultObj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPWidgetCommands::HandleWidgetTreeRead(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetPath;
    if (!Params->TryGetStringField(TEXT("widget_path"), WidgetPath) || WidgetPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'widget_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path to a UWidgetBlueprint, e.g. /Game/UI/WBP_HUD."));
    }

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetPath),
            Loaded ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
            Loaded
                ? TEXT("widget_tree_read only operates on Widget Blueprint assets. Other Blueprint kinds use read_blueprint_content / analyze_blueprint_graph.")
                : TEXT("Verify the path; asset registry returned no entry."));
    }

    UWidgetTree* Tree = WBP->WidgetTree;
    if (!Tree)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget Blueprint has no WidgetTree: %s"), *WidgetPath),
            EMCPErrorCode::Internal,
            TEXT("This usually indicates a corrupt asset or a partially-initialized BP. Try reopening the asset in the editor."));
    }

    TArray<TSharedPtr<FJsonValue>> WidgetEntries;
    // ForEachWidgetAndDescendants walks the full tree (root + all panel children, recursively).
    Tree->ForEachWidgetAndDescendants([&WidgetEntries](UWidget* W)
    {
        if (W)
        {
            WidgetEntries.Add(MakeShared<FJsonValueObject>(WidgetToJson(W)));
        }
    });

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("widget_path"), WidgetPath);
    if (UWidget* Root = Tree->RootWidget)
    {
        ResultObj->SetStringField(TEXT("root_widget"), Root->GetName());
    }
    ResultObj->SetArrayField(TEXT("widgets"), WidgetEntries);
    ResultObj->SetNumberField(TEXT("widget_count"), WidgetEntries.Num());
    if (UClass* ParentClass = WBP->ParentClass.Get())
    {
        ResultObj->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

namespace
{
    // Resolve a UWidget class (any subclass) for widget_add_child.
    // Accepts native script paths (/Script/UMG.Button), asset paths
    // (/Game/UI/WBP_Foo, _C is appended automatically), or short names
    // with the `U`-prefix-fallback convention.
    UClass* ResolveAnyWidgetClass(const FString& Name)
    {
        if (Name.IsEmpty())
        {
            return nullptr;
        }

        if (Name.StartsWith(TEXT("/")))
        {
            FString Path = Name;
            if (!Path.Contains(TEXT(".")))
            {
                const FString ShortName = FPackageName::GetShortName(Path);
                Path = FString::Printf(TEXT("%s.%s_C"), *Path, *ShortName);
            }
            if (UClass* Loaded = LoadClass<UWidget>(nullptr, *Path))
            {
                return Loaded;
            }
        }

        if (UClass* Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::ExactClass))
        {
            return Found;
        }

        if (Name.StartsWith(TEXT("U")) && Name.Len() > 1)
        {
            if (UClass* Found = FindFirstObject<UClass>(*Name.RightChop(1), EFindFirstObjectOptions::ExactClass))
            {
                return Found;
            }
        }
        else if (!Name.StartsWith(TEXT("U")))
        {
            const FString WithU = FString::Printf(TEXT("U%s"), *Name);
            if (UClass* Found = FindFirstObject<UClass>(*WithU, EFindFirstObjectOptions::ExactClass))
            {
                return Found;
            }
        }
        return nullptr;
    }

    UWidget* FindWidgetByNameInTree(UWidgetTree* Tree, const FString& Name)
    {
        if (!Tree || Name.IsEmpty())
        {
            return nullptr;
        }
        UWidget* Hit = nullptr;
        Tree->ForEachWidgetAndDescendants([&Hit, &Name](UWidget* W)
        {
            if (!Hit && W && W->GetName() == Name)
            {
                Hit = W;
            }
        });
        return Hit;
    }
}

TSharedPtr<FJsonObject> FMCPWidgetCommands::HandleWidgetAddChild(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetPath;
    if (!Params->TryGetStringField(TEXT("widget_path"), WidgetPath) || WidgetPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'widget_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path to a UWidgetBlueprint, e.g. /Game/UI/WBP_HUD."));
    }

    FString ChildClassPath;
    if (!Params->TryGetStringField(TEXT("child_class"), ChildClassPath) || ChildClassPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'child_class' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass a UWidget subclass — script path (/Script/UMG.Button), asset path (/Game/UI/WBP_Foo), or short name (UButton / Button)."));
    }

    // Optional. When omitted, the new widget becomes the WBP's root widget
    // (only valid for an empty tree).
    FString ParentName;
    Params->TryGetStringField(TEXT("parent_name"), ParentName);

    FString ChildName;
    Params->TryGetStringField(TEXT("child_name"), ChildName);

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetPath),
            Loaded ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
            Loaded
                ? TEXT("widget_add_child only operates on Widget Blueprint assets.")
                : TEXT("Verify the path; asset registry returned no entry."));
    }
    if (!WBP->WidgetTree)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget Blueprint has no WidgetTree: %s"), *WidgetPath),
            EMCPErrorCode::Internal,
            TEXT("Corrupt asset; reopen in the editor."));
    }

    UClass* ChildClass = ResolveAnyWidgetClass(ChildClassPath);
    if (!ChildClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not resolve child class: %s"), *ChildClassPath),
            EMCPErrorCode::ClassNotLoaded,
            TEXT("Verify the path. Native widgets live under /Script/UMG; user widgets under /Game/... (the _C suffix is added automatically)."));
    }
    if (!ChildClass->IsChildOf(UWidget::StaticClass()))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class is not a UWidget subclass: %s"), *ChildClass->GetPathName()),
            EMCPErrorCode::UnsupportedClass,
            TEXT("widget_add_child requires a UWidget descendant (UButton, UTextBlock, UCanvasPanel, UVerticalBox, etc.)."));
    }
    if (ChildClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Class is abstract and cannot be instantiated: %s"), *ChildClass->GetPathName()),
            EMCPErrorCode::UnsupportedClass,
            TEXT("Pick a concrete UWidget subclass."));
    }

    UWidgetTree* Tree = WBP->WidgetTree;
    const FName ChildFName = ChildName.IsEmpty() ? NAME_None : FName(*ChildName);

    // A caller-supplied child_name that collides with an existing widget makes
    // NewObject recycle the existing object in place — and LogFatal-crash the
    // editor when the existing widget's class differs. Refuse before constructing.
    if (ChildFName != NAME_None && FindWidgetByNameInTree(Tree, ChildName))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("A widget named '%s' already exists in the tree"), *ChildName),
            EMCPErrorCode::NameCollision,
            TEXT("Pick a unique child_name, or omit it to let UE auto-name the widget. widget_tree_read lists existing names."));
    }

    // ConstructWidget's templated entry point assumes a known type at compile
    // time; the generic NewObject path is the equivalent at runtime.
    UWidget* NewChild = NewObject<UWidget>(Tree, ChildClass, ChildFName, RF_Transactional);
    if (!NewChild)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("NewObject<UWidget> returned null for class %s"), *ChildClass->GetPathName()),
            EMCPErrorCode::EngineBusy,
            TEXT("UE refused to construct the widget; check the editor log."));
    }

    // GAP-037: expose the widget as a member variable by default. Widgets created with
    // bIsVariable=false are not properties of the generated class, so a later
    // bp_add_node VariableGet produces an unbound, pinless getter and the widget can't
    // be wired/driven from the graph. Defaulting to true (the common authoring intent)
    // makes Get<Widget> resolve after compile. Callers can pass is_variable=false for
    // purely-decorative widgets they never reference.
    bool bIsVariable = true;
    Params->TryGetBoolField(TEXT("is_variable"), bIsVariable);
    NewChild->bIsVariable = bIsVariable;

    bool bAttachedToParent = false;
    if (ParentName.IsEmpty())
    {
        // Empty parent → seat as root. Only valid for an empty tree.
        if (Tree->RootWidget)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("WBP already has root widget '%s'; pass parent_name to add as a child"), *Tree->RootWidget->GetName()),
                EMCPErrorCode::AssetLocked,
                TEXT("Use the existing root or remove it first. parent_name='<root_widget_name>' attaches under the root."));
        }
        Tree->RootWidget = NewChild;
        bAttachedToParent = false;
    }
    else
    {
        UWidget* Parent = FindWidgetByNameInTree(Tree, ParentName);
        if (!Parent)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent widget not found in tree: %s"), *ParentName),
                EMCPErrorCode::NodeNotFound,
                TEXT("widget_tree_read shows the available widgets and their names."));
        }
        UPanelWidget* ParentPanel = Cast<UPanelWidget>(Parent);
        if (!ParentPanel)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Parent '%s' is not a UPanelWidget (class: %s); only panels can hold children"), *ParentName, *Parent->GetClass()->GetPathName()),
                EMCPErrorCode::UnsupportedClass,
                TEXT("Children attach under panel widgets — UCanvasPanel, UVerticalBox, UHorizontalBox, UGridPanel, UOverlay, UScrollBox, etc."));
        }
        ParentPanel->AddChild(NewChild);
        bAttachedToParent = true;
    }

    // Compile + save per the doc invariant: "Tree changes require a Blueprint
    // compile to take effect at runtime."
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);
    WBP->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Child added and BP compiled in-editor but saving to disk failed: %s"), *WidgetPath),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the change is live in the editor but was not written to disk (read-only file, source-control checkout failure, or a save-blocked PIE/unattended session). Do not report success."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("widget_path"), WidgetPath);
    ResultObj->SetStringField(TEXT("child_name"), NewChild->GetName());
    ResultObj->SetStringField(TEXT("child_class"), ChildClass->GetPathName());
    if (bAttachedToParent)
    {
        ResultObj->SetStringField(TEXT("parent_name"), ParentName);
    }
    else
    {
        ResultObj->SetBoolField(TEXT("set_as_root"), true);
    }
    if (UPanelSlot* Slot = NewChild->Slot)
    {
        ResultObj->SetStringField(TEXT("slot_class"), Slot->GetClass()->GetPathName());
    }
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPWidgetCommands::HandleWidgetBindHandler(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetPath, WidgetName, EventName;
    if (!Params->TryGetStringField(TEXT("widget_path"), WidgetPath) || WidgetPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'widget_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path to a UWidgetBlueprint."));
    }
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'widget_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the named widget instance inside the WBP — widget_tree_read surfaces them."));
    }
    if (!Params->TryGetStringField(TEXT("event_name"), EventName) || EventName.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'event_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the multicast delegate property name on the widget — e.g. 'OnClicked' for UButton, 'OnHovered' for UButton, 'OnTextCommitted' for UEditableTextBox."));
    }

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetPath),
            Loaded ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
            TEXT("widget_bind_handler only operates on Widget Blueprint assets."));
    }
    if (!WBP->WidgetTree)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget Blueprint has no WidgetTree: %s"), *WidgetPath),
            EMCPErrorCode::Internal,
            TEXT("Corrupt asset; reopen in the editor."));
    }

    UWidget* Widget = FindWidgetByNameInTree(WBP->WidgetTree, WidgetName);
    if (!Widget)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget not found in tree: %s"), *WidgetName),
            EMCPErrorCode::NodeNotFound,
            TEXT("widget_tree_read shows the available widgets and their names."));
    }

    // Validate the event name resolves to a multicast delegate property on the
    // widget's class. The `OnClicked` etc. fields are FMulticastDelegateProperty
    // (or FMulticastInlineDelegateProperty / FMulticastSparseDelegateProperty).
    const FName EventFName(*EventName);
    UClass* WidgetClass = Widget->GetClass();
    FProperty* DelegateProp = WidgetClass->FindPropertyByName(EventFName);
    if (!DelegateProp || !DelegateProp->IsA<FMulticastDelegateProperty>())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Event '%s' is not a multicast delegate on widget class '%s'"),
                *EventName, *WidgetClass->GetPathName()),
            EMCPErrorCode::PinNotFound,
            TEXT("Verify the event name. Common UButton events: OnClicked, OnPressed, OnReleased, OnHovered, OnUnhovered."));
    }

    // The component property is on the WBP's GeneratedClass (not the WidgetTree).
    // After compile, each named widget appears as a UWidget*-typed FObjectProperty
    // on the generated UUserWidget subclass. CreateNewBoundEventForComponent uses
    // this property to route the bound event to the right widget instance at runtime.
    UClass* GenClass = WBP->GeneratedClass;
    if (!GenClass)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Widget Blueprint has no GeneratedClass — compile the BP first"),
            EMCPErrorCode::AssetCompileFailed,
            TEXT("Newly-created widgets only appear as FObjectProperty on the generated class after a compile. Run compile_blueprint then retry."));
    }
    FProperty* CompPropBase = GenClass->FindPropertyByName(FName(*WidgetName));
    FObjectProperty* CompProp = CastField<FObjectProperty>(CompPropBase);
    if (!CompProp)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget '%s' has no FObjectProperty on the generated class — recompile required"), *WidgetName),
            EMCPErrorCode::AssetCompileFailed,
            TEXT("If the widget was just added, compile the BP before binding handlers (the property is generated by the compiler)."));
    }

    // Wire the bound event. CreateNewBoundEventForComponent finds-or-creates a
    // K2Node_ActorBoundEvent subclass in the BP's event graph, named per UE's
    // convention (BndEvt__widget__delegate). Idempotent — if the binding already
    // exists, no duplicate is created.
    FKismetEditorUtilities::CreateNewBoundEventForComponent(Widget, EventFName, WBP, CompProp);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);
    WBP->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Handler bound and BP compiled in-editor but saving to disk failed: %s"), *WidgetPath),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the binding is live in the editor but was not written to disk (read-only file, source-control checkout failure, or a save-blocked PIE/unattended session). Do not report success."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("widget_path"), WidgetPath);
    ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
    ResultObj->SetStringField(TEXT("event_name"), EventName);
    ResultObj->SetStringField(TEXT("widget_class"), WidgetClass->GetPathName());
    // UE's autogenerated event-node name format: BndEvt__<WidgetName>_K2Node_ComponentBoundEvent_<n>_<EventDelegate>__DelegateSignature.
    // The full resolved name is determined post-create; we surface what we know.
    ResultObj->SetStringField(TEXT("delegate_property"), DelegateProp->GetCPPType());
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPWidgetCommands::HandleWidgetSetProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetPath, WidgetName, PropertyName;
    if (!Params->TryGetStringField(TEXT("widget_path"), WidgetPath) || WidgetPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'widget_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the asset path to a UWidgetBlueprint."));
    }
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'widget_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the named widget instance — widget_tree_read surfaces them."));
    }
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'property_name' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the property's FName — case-sensitive. widget_tree_read's slot dump and class_inspect on the widget class surface valid names."));
    }

    // target discriminator: "widget" (default — set on the widget instance itself)
    // or "slot" (set on the widget's UPanelSlot — slot-layout properties like
    // anchors, position, fill, etc.). Per doc 7, widget_set_property is the
    // umbrella; the target field is the dispatch.
    FString TargetStr = TEXT("widget");
    Params->TryGetStringField(TEXT("target"), TargetStr);
    const FString TargetLower = TargetStr.ToLower();
    const bool bTargetSlot = (TargetLower == TEXT("slot"));
    if (!bTargetSlot && TargetLower != TEXT("widget"))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Unknown 'target' value: %s"), *TargetStr),
            EMCPErrorCode::InvalidArgument,
            TEXT("Closed set: 'widget' (default — set on the UWidget instance) or 'slot' (set on the widget's UPanelSlot for layout/anchor/fill properties)."));
    }

    // Required value field — accept any JSON shape. SetObjectProperty handles
    // string / number / bool / object / array via the FJsonValue infrastructure.
    TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("property_value"));
    if (!Value.IsValid())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'property_value' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass the value as the appropriate JSON type — string for FString/FName/FText, number for numeric props, bool for bool props, object for FStruct, array for TArray."));
    }

    UObject* Loaded = UEditorAssetLibrary::LoadAsset(WidgetPath);
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(Loaded);
    if (!WBP)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Asset is not a UWidgetBlueprint: %s"), *WidgetPath),
            Loaded ? EMCPErrorCode::UnsupportedClass : EMCPErrorCode::AssetNotFound,
            TEXT("widget_set_property only operates on Widget Blueprint assets."));
    }
    if (!WBP->WidgetTree)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget Blueprint has no WidgetTree: %s"), *WidgetPath),
            EMCPErrorCode::Internal,
            TEXT("Corrupt asset; reopen in the editor."));
    }

    UWidget* Widget = FindWidgetByNameInTree(WBP->WidgetTree, WidgetName);
    if (!Widget)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Widget not found in tree: %s"), *WidgetName),
            EMCPErrorCode::NodeNotFound,
            TEXT("widget_tree_read shows the available widgets and their names."));
    }

    UObject* TargetObject = Widget;
    if (bTargetSlot)
    {
        if (!Widget->Slot)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Widget '%s' has no slot — it's the root widget or unparented"), *WidgetName),
                EMCPErrorCode::AssetLocked,
                TEXT("Slots only exist on widgets attached under a panel. The root widget has no slot — set its properties with target='widget'."));
        }
        TargetObject = Widget->Slot;
    }

    // Capture the before value for the response. ExportTextItem_Direct gives
    // us the same on-disk text format the editor uses, useful for confirming
    // the change in agent-readable form.
    FProperty* Prop = TargetObject->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (!Prop)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Property '%s' not found on %s class '%s'"),
                *PropertyName, bTargetSlot ? TEXT("slot") : TEXT("widget"),
                *TargetObject->GetClass()->GetPathName()),
            EMCPErrorCode::PinNotFound,
            TEXT("Verify the property name. widget_tree_read's slot dump shows slot-side names; class_inspect on the widget class shows widget-side names."));
    }

    FString BeforeText;
    Prop->ExportTextItem_Direct(BeforeText,
        Prop->ContainerPtrToValuePtr<void>(TargetObject),
        nullptr, nullptr, PPF_None);

    FString WriteError;
    const bool bSet = FMCPCommonUtils::SetObjectProperty(TargetObject, PropertyName, Value, WriteError);
    if (!bSet)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to set property '%s' on %s: %s"),
                *PropertyName, bTargetSlot ? TEXT("slot") : TEXT("widget"), *WriteError),
            EMCPErrorCode::InvalidArgument,
            TEXT("Common causes: type mismatch between property_value and the FProperty's expected type, malformed struct/array JSON, or the property is read-only."));
    }

    FString AfterText;
    Prop->ExportTextItem_Direct(AfterText,
        Prop->ContainerPtrToValuePtr<void>(TargetObject),
        nullptr, nullptr, PPF_None);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);
    WBP->MarkPackageDirty();
    if (!UEditorAssetLibrary::SaveAsset(WBP->GetPathName(), /*bOnlyIfIsDirty=*/false))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Property set and BP compiled in-editor but saving to disk failed: %s"), *WidgetPath),
            EMCPErrorCode::Internal,
            TEXT("UEditorAssetLibrary::SaveAsset returned false — the change is live in the editor but was not written to disk (read-only file, source-control checkout failure, or a save-blocked PIE/unattended session). Do not report success."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("widget_path"), WidgetPath);
    ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
    ResultObj->SetStringField(TEXT("target"), bTargetSlot ? TEXT("slot") : TEXT("widget"));
    ResultObj->SetStringField(TEXT("target_class"), TargetObject->GetClass()->GetPathName());
    ResultObj->SetStringField(TEXT("property_name"), PropertyName);
    ResultObj->SetStringField(TEXT("property_type"), Prop->GetCPPType());
    ResultObj->SetStringField(TEXT("before"), BeforeText);
    ResultObj->SetStringField(TEXT("after"),  AfterText);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
