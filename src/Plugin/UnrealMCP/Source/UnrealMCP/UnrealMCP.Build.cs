// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class UnrealMCP : ModuleRules
{
	public UnrealMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicDefinitions.Add("UNREALMCP_EXPORTS=1");

		PublicIncludePaths.AddRange(
			new string[] {
				System.IO.Path.Combine(ModuleDirectory, "Public"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Commands"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Commands/BlueprintGraph"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Commands/BlueprintGraph/Nodes"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Commands/MaterialGraph"),
				System.IO.Path.Combine(ModuleDirectory, "Public/Commands/StateTree")
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
				System.IO.Path.Combine(ModuleDirectory, "Private"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Commands"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Commands/BlueprintGraph"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Commands/BlueprintGraph/Nodes"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Commands/MaterialGraph"),
				System.IO.Path.Combine(ModuleDirectory, "Private/Commands/StateTree"),
				// AnimGraph's Internal/ — needed for UAnimGraphNodeBinding (the abstract
				// base behind UAnimGraphNode_Base::GetMutableBinding()).  bind_anim_node_property
				// uses this type directly to access the binding sub-object. Engine-relative,
				// so it ports to any host project.
				// (The former reach-in to the game module's source root is gone: the
				// Kinematics commands now use the vendored Private/Commands/Kinematics/
				// BoneIKUtils.h, keeping the plugin buildable standalone. The consuming
				// game's copy is canonical — see the vendored header's top note.)
				System.IO.Path.Combine(EngineDirectory, "Source/Editor/AnimGraph/Internal")
			}
		);
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Networking",
				"Sockets",
				"HTTP",
				"Json",
				"JsonUtilities",
				"DeveloperSettings",
				"PhysicsCore",
				"UnrealEd",           // For Blueprint editing
				"BlueprintGraph",     // For K2Node classes (F15-F22)
				"KismetCompiler",     // For Blueprint compilation (F15-F22)
				"InputBlueprintNodes", // For K2Node_EnhancedInputAction
				"EnhancedInput",       // UInputAction, UInputMappingContext, EInputActionValueType (todo/6 input_create)
				"AnimGraph",          // For UAnimGraphNode_SkeletalControlBase, UAnimGraphNode_TwoBoneIK
				"AnimGraphRuntime",   // For FAnimNode_* runtime structs (bone refs, IK params)
				"Landscape",          // For UMaterialExpressionLandscapeLayerBlend + landscape_inspect reads
				"Foliage",            // AInstancedFoliageActor / UFoliageType (foliage_inspect reads)
				"LiveCoding",         // For ILiveCodingModule (MCP compile trigger)
				"Niagara",            // For UNiagaraSystem, UNiagaraEmitter, parameter stores
				"NiagaraCore",        // For FNiagaraTypeDefinition, FNiagaraVariable
				"NiagaraEditor",      // For scratch pad modules, graph manipulation, stack utilities
				"PCG",                // Procedural Content Generation: UPCGGraph/Node/Pin/Settings/Component (docs/proposals/pcg-mcp.md)
				"StateTreeModule",    // UStateTree, FStateTreeTaskBase, runtime node types
				"StateTreeEditorModule", // UStateTreeEditorData, UStateTreeState, editor node types
				"PropertyBindingUtils", // FPropertyBindingPath, FPropertyBindingBindingCollection (5.7+)
				"AIModule",           // AAIController, UAIPerceptionComponent, EQS types
				"GameplayTags",       // FGameplayTag parameter handling in StateTree
				"GameplayStateTreeModule", // UStateTreeComponent (runtime AI component)
				"RHI",                    // GMaxRHIShaderPlatform for material compile error access
				"RenderCore",             // FlushRenderingCommands — render-thread drain barrier for move_asset
				"GeometryFramework",      // UDynamicMesh, UDynamicMeshComponent (bake_dynamic_mesh_to_static_mesh)
				"GeometryScriptingCore",  // UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh
				"MeshDescription",        // FMeshDescription (used internally by the bake path)
				"StaticMeshDescription",  // UStaticMeshDescription build path
				"SkeletalMeshDescription",      // FSkeletalMeshAttributes + FSkinWeightsVertexAttributesRef (skeletal_mesh_build_bend_chain re-skin)
				"SkeletalMeshUtilitiesCommon",  // FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions (rebuild path)
				"AnimationCore",                // UE::AnimationCore::FBoneWeight / FBoneWeights (skin weight authoring)
				"UMG",                    // UUserWidget, UWidgetTree, UWidget, panel slot classes (todo/7_widget_umg.md)
				"GameplayAbilities",      // UGameplayAbility, UGameplayEffect, UAttributeSet (todo/10_gas_authoring.md)
				"ImageWrapper"            // PNG encoding for editor_window_screenshot (todo/11_editor_diagnostics.md)
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"EditorScriptingUtilities",
				"EditorSubsystem",
				"StaticMeshEditor",  // UStaticMeshEditorSubsystem + EScriptCollisionShapeType (set_static_mesh_collision)
				"Slate",
				"SlateCore",
				"LevelEditor",        // SLevelViewport + FLevelEditorModule for editor_window_screenshot viewport mode

				"Kismet",
				"Projects",
				"AssetRegistry",
				"AssetTools",
				"AudioEditor",        // USoundFactory (asset_import_audio → USoundWave)
				"EngineSettings", // UGameMapsSettings (project_context tool, todo/3_multi_res_inspection.md)
				"UMGEditor",      // UWidgetBlueprint, UWidgetBlueprintFactory (todo/7_widget_umg.md)
				"IKRig",             // UIKRigDefinition, UIKRetargeter (todo/8_ik_rig_retargeter.md)
				"IKRigEditor",       // UIKRigController, UIKRetargeterController, factory classes
				"AnimationBlueprintLibrary", // UAnimPoseExtensions for sampling AnimSequence frames as retarget poses
				"GameplayTagsEditor", // IGameplayTagsEditorModule (todo/9_gameplay_tag_registry.md)
				"MovieSceneCapture",  // FFrameGrabber (pie_record_* back-buffer capture)

				// Pixel Streaming 2 editor streaming (stream_* commands, portable.dev#19 M2).
				// Hard-enables the PixelStreaming2 plugin for host projects (uplugin ref) —
				// accepted for the PoC.
				"PixelStreaming2Editor",   // IPixelStreaming2EditorModule (Start/StopStreaming, ports)
				"PixelStreaming2",         // IPixelStreaming2Module (GetStreamerIds / FindStreamer)
				"PixelStreaming2Core",     // IPixelStreaming2Streamer (IsStreaming, start/stop events, GetInputHandler)
				"PixelStreaming2Input",    // IPixelStreaming2InputHandler (RegisterMessageHandler "UIInteraction" — touch camera control)
				"PixelStreaming2Settings", // EPixelStreaming2EditorStreamTypes (PixelStreaming2SettingsEnums.h)
				"PixelStreaming2Servers"   // PixelStreaming2Servers.h (transitively included by the editor-module interface)
			}
		);

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Media Foundation H.264/MP4 sink writer for pie_record_* (MCPVideoRecorder).
			PublicSystemLibraries.AddRange(
				new string[]
				{
					"mfplat.lib",
					"mfreadwrite.lib",
					"mfuuid.lib"
				}
			);
		}
		
		if (Target.bBuildEditor == true)
		{
			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"PropertyEditor",      // For property editing
					"ToolMenus",           // For editor UI
					"BlueprintEditorLibrary" // For Blueprint utilities
				}
			);
		}
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
		);
	}
} 