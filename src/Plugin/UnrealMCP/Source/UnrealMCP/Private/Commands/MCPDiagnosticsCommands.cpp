#include "Commands/MCPDiagnosticsCommands.h"
#include "Commands/MCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

#include "HAL/PlatformMemory.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "RHI.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

// Engine globals — declared in Runtime/Engine/Private/UnrealEngine.cpp.
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

namespace
{
    constexpr double BytesToMB = 1.0 / (1024.0 * 1024.0);
}

TSharedPtr<FJsonObject> FMCPDiagnosticsCommands::HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    if (CommandType == TEXT("editor_perf_snapshot"))     return HandlePerfSnapshot(Params);
    if (CommandType == TEXT("editor_content_browser_refresh"))  return HandleContentBrowserRefresh(Params);
    if (CommandType == TEXT("editor_window_screenshot")) return HandleWindowScreenshot(Params);

    return FMCPCommonUtils::CreateErrorResponse(
        FString::Printf(TEXT("Unknown diagnostics command: %s"), *CommandType),
        EMCPErrorCode::InvalidArgument,
        TEXT("Supported diagnostics commands in this build: editor_perf_snapshot, content_browser_refresh, editor_window_screenshot."));
}

TSharedPtr<FJsonObject> FMCPDiagnosticsCommands::HandlePerfSnapshot(const TSharedPtr<FJsonObject>& /*Params*/)
{
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();

    // ── Frame timing ────────────────────────────────────────────────────────
    // GAverageFPS / GAverageMS are 1-frame moving averages updated by
    // CalculateFPSTimings. They reflect "what the editor is currently doing"
    // at the instant of the call. Snapshot, not a profile.
    ResultObj->SetNumberField(TEXT("average_fps"), GAverageFPS);
    ResultObj->SetNumberField(TEXT("average_frame_ms"), GAverageMS);
    ResultObj->SetNumberField(TEXT("frame_counter"), static_cast<double>(GFrameCounter));
    ResultObj->SetNumberField(TEXT("delta_seconds"), FApp::GetDeltaTime());
    ResultObj->SetNumberField(TEXT("current_time_seconds"), FApp::GetCurrentTime());

    // ── Memory ──────────────────────────────────────────────────────────────
    const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
    TSharedPtr<FJsonObject> MemObj = MakeShared<FJsonObject>();
    MemObj->SetNumberField(TEXT("used_physical_mb"),
        static_cast<double>(Stats.UsedPhysical)  * BytesToMB);
    MemObj->SetNumberField(TEXT("used_virtual_mb"),
        static_cast<double>(Stats.UsedVirtual)   * BytesToMB);
    MemObj->SetNumberField(TEXT("available_physical_mb"),
        static_cast<double>(Stats.AvailablePhysical) * BytesToMB);
    MemObj->SetNumberField(TEXT("available_virtual_mb"),
        static_cast<double>(Stats.AvailableVirtual)  * BytesToMB);
    MemObj->SetNumberField(TEXT("peak_used_physical_mb"),
        static_cast<double>(Stats.PeakUsedPhysical)  * BytesToMB);
    MemObj->SetNumberField(TEXT("peak_used_virtual_mb"),
        static_cast<double>(Stats.PeakUsedVirtual)   * BytesToMB);
    ResultObj->SetObjectField(TEXT("memory"), MemObj);

    // ── RHI / GPU description ───────────────────────────────────────────────
    // GPU timings require an active profiler — the doc explicitly calls this
    // out as best-effort, "field is absent (not zero)" when unavailable.
    // We surface the RHI/adapter description universally so the snapshot is
    // useful even without GPU timing.
    TSharedPtr<FJsonObject> GpuObj = MakeShared<FJsonObject>();
    GpuObj->SetStringField(TEXT("rhi_name"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT(""));
    if (!GRHIAdapterName.IsEmpty())
    {
        GpuObj->SetStringField(TEXT("adapter_name"), GRHIAdapterName);
    }
    GpuObj->SetNumberField(TEXT("vendor_id"), static_cast<double>(GRHIVendorId));
    GpuObj->SetNumberField(TEXT("max_shader_platform"), static_cast<double>(GMaxRHIShaderPlatform));
    ResultObj->SetObjectField(TEXT("gpu"), GpuObj);

    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

TSharedPtr<FJsonObject> FMCPDiagnosticsCommands::HandleContentBrowserRefresh(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetPath;
    Params->TryGetStringField(TEXT("path"), TargetPath);
    if (TargetPath.IsEmpty())
    {
        TargetPath = TEXT("/Game");
    }
    if (!TargetPath.StartsWith(TEXT("/")))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Path must begin with '/Game/' or '/<Plugin>/' — got: %s"), *TargetPath),
            EMCPErrorCode::InvalidPath,
            TEXT("Use a logical content-tree path; the asset registry rescans by virtual path, not by filesystem path."));
    }

    bool bForceRescan = true;
    Params->TryGetBoolField(TEXT("force_rescan"), bForceRescan);

    FAssetRegistryModule& RegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& Registry = RegistryModule.Get();

    TArray<FString> Paths;
    Paths.Add(TargetPath);
    Registry.ScanPathsSynchronous(Paths, bForceRescan);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("path"), TargetPath);
    ResultObj->SetBoolField(TEXT("force_rescan"), bForceRescan);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}

namespace
{
    // Sanitize a filename by replacing characters that would break the path.
    FString SanitizeForFilename(const FString& In)
    {
        FString Out = In;
        Out.ReplaceInline(TEXT("/"),  TEXT("_"));
        Out.ReplaceInline(TEXT("\\"), TEXT("_"));
        Out.ReplaceInline(TEXT(":"),  TEXT("_"));
        Out.ReplaceInline(TEXT("\""), TEXT("_"));
        Out.ReplaceInline(TEXT("*"),  TEXT("_"));
        Out.ReplaceInline(TEXT("?"),  TEXT("_"));
        Out.ReplaceInline(TEXT("<"),  TEXT("_"));
        Out.ReplaceInline(TEXT(">"),  TEXT("_"));
        Out.ReplaceInline(TEXT("|"),  TEXT("_"));
        Out.ReplaceInline(TEXT(" "),  TEXT("_"));
        return Out;
    }
}

TSharedPtr<FJsonObject> FMCPDiagnosticsCommands::HandleWindowScreenshot(const TSharedPtr<FJsonObject>& Params)
{
    if (!FSlateApplication::IsInitialized())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Slate application not initialized"),
            EMCPErrorCode::EngineBusy,
            TEXT("Editor mid-load or running headless. Retry shortly."));
    }

    bool bViewport = false;
    FString TabName;
    FString WindowTitle;
    Params->TryGetBoolField(TEXT("viewport"), bViewport);
    Params->TryGetStringField(TEXT("tab_name"), TabName);
    Params->TryGetStringField(TEXT("window_title"), WindowTitle);

    // Resolve target Slate widget. Priority: viewport → tab_name → window_title → editor root.
    //
    // All paths land on a widget that FSlateApplication::TakeScreenshot can capture safely.
    // The capture path inside TakeScreenshot internally calls GeneratePathToWidgetChecked,
    // which asserts on bWasFound if the widget is not reachable from SlateWindows /
    // SlateVirtualWindows. We pre-validate with the matching Unchecked variant below.
    TSharedPtr<SWidget> TargetWidget;
    FString TargetDesc;

    if (bViewport)
    {
        FLevelEditorModule& LE = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
        TSharedPtr<SLevelViewport> Viewport = LE.GetFirstActiveLevelViewport();
        if (!Viewport.IsValid())
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("No active level viewport"),
                EMCPErrorCode::WindowNotFound,
                TEXT("Open the level editor and ensure at least one viewport is visible."));
        }
        TargetWidget = Viewport;
        TargetDesc = TEXT("viewport");
    }
    else if (!TabName.IsEmpty())
    {
        if (TabName.Len() > NAME_SIZE)
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("tab_name exceeds the maximum FName length (%d chars)"), NAME_SIZE),
                EMCPErrorCode::InvalidArgument,
                TEXT("Pass a registered tab id (FName) shorter than 1024 characters."));
        }
        TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId(FName(*TabName)));
        if (!Tab.IsValid())
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Tab not found: %s"), *TabName),
                EMCPErrorCode::WindowNotFound,
                TEXT("Pass a registered tab id (FName) — e.g. 'LevelEditor', 'ContentBrowserTab1', 'OutputLog'. Tab IDs are exact, case-sensitive FNames."));
        }
        TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Tab.ToSharedRef());
        if (!Window.IsValid())
        {
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("Tab '%s' has no live containing window"), *TabName),
                EMCPErrorCode::WindowNotFound,
                TEXT("Re-dock or re-focus the tab and retry."));
        }
        TargetWidget = Window;
        TargetDesc = TabName;
    }
    else if (!WindowTitle.IsEmpty())
    {
        // Case-insensitive substring match across all interactive top-level windows
        // (includes undocked / popped-out editor windows).
        const TArray<TSharedRef<SWindow>> AllWindows = FSlateApplication::Get().GetInteractiveTopLevelWindows();
        for (const TSharedRef<SWindow>& W : AllWindows)
        {
            const FString Title = W->GetTitle().ToString();
            if (Title.Contains(WindowTitle, ESearchCase::IgnoreCase))
            {
                TargetWidget = W;
                TargetDesc = Title;
                break;
            }
        }
        if (!TargetWidget.IsValid())
        {
            FString AvailableTitles;
            for (const TSharedRef<SWindow>& W : AllWindows)
            {
                if (!AvailableTitles.IsEmpty()) AvailableTitles.Append(TEXT(", "));
                AvailableTitles.Append(W->GetTitle().ToString());
            }
            return FMCPCommonUtils::CreateErrorResponse(
                FString::Printf(TEXT("No interactive top-level window matched title substring: '%s'"), *WindowTitle),
                EMCPErrorCode::WindowNotFound,
                FString::Printf(TEXT("Available window titles: [%s]"), *AvailableTitles));
        }
    }
    else
    {
        // Default — editor root window via FGlobalTabmanager. Always present once the
        // editor has finished initialization, regardless of OS focus.
        TSharedPtr<SWindow> Root = FGlobalTabmanager::Get()->GetRootWindow();
        if (!Root.IsValid())
        {
            Root = FSlateApplication::Get().GetActiveTopLevelWindow();
        }
        if (!Root.IsValid())
        {
            return FMCPCommonUtils::CreateErrorResponse(
                TEXT("No editor root window available"),
                EMCPErrorCode::WindowNotFound,
                TEXT("Editor may still be loading. Retry shortly."));
        }
        TargetWidget = Root;
        TargetDesc = TEXT("root");
    }

    // Pre-validate that the widget is currently reachable in the Slate widget tree.
    // Mirrors the precondition of GeneratePathToWidgetChecked inside TakeScreenshot —
    // bypassing this guard would crash the editor with the bWasFound assert at
    // SlateApplication.cpp:3263 when the widget is collapsed, mid-teardown, or in a
    // backgrounded tab stack.
    const TSharedRef<SWidget> TargetRef = TargetWidget.ToSharedRef();
    FWidgetPath ValidationPath;
    if (!FSlateApplication::Get().GeneratePathToWidgetUnchecked(TargetRef, ValidationPath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Target '%s' is not currently drawable (parent stack collapsed, tab backgrounded, or window mid-teardown)"), *TargetDesc),
            EMCPErrorCode::WindowNotFound,
            TEXT("Bring the containing tab/window to the foreground and retry."));
    }

    TArray<FColor> ColorData;
    FIntVector OutSize(0, 0, 0);
    const bool bCaptured = FSlateApplication::Get().TakeScreenshot(TargetRef, ColorData, OutSize);
    if (!bCaptured || OutSize.X <= 0 || OutSize.Y <= 0)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Slate screenshot capture returned no pixels for '%s'"), *TargetDesc),
            EMCPErrorCode::EngineBusy,
            TEXT("The widget may be minimized or in a state where its renderer can't produce pixels. Bring it to front and retry."));
    }

    // Encode to PNG.
    IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    TSharedPtr<IImageWrapper> Png = IWM.CreateImageWrapper(EImageFormat::PNG);
    if (!Png.IsValid())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Failed to create PNG image wrapper"),
            EMCPErrorCode::Internal,
            TEXT("ImageWrapper module returned a null wrapper."));
    }
    if (!Png->SetRaw(ColorData.GetData(), ColorData.Num() * sizeof(FColor),
                     OutSize.X, OutSize.Y, ERGBFormat::BGRA, /*BitDepth=*/8))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Failed to set raw pixel data on PNG wrapper"),
            EMCPErrorCode::Internal,
            TEXT("Pixel buffer dimensions or format mismatch."));
    }

    const TArray64<uint8>& PngBytes = Png->GetCompressed();

    // Save to Saved/MCPScreenshots/<timestamp>_<target>.png.
    const FString DirPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MCPScreenshots"));
    IFileManager::Get().MakeDirectory(*DirPath, /*Tree=*/true);

    const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    const FString SafeDesc = SanitizeForFilename(TargetDesc);
    const FString FileName = FString::Printf(TEXT("%s_%s.png"), *Timestamp, *SafeDesc);
    const FString FilePath = DirPath / FileName;

    if (!FFileHelper::SaveArrayToFile(PngBytes, *FilePath))
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Failed to write screenshot to: %s"), *FilePath),
            EMCPErrorCode::Internal,
            TEXT("Verify the Saved/ directory is writable."));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("file_path"), FilePath);
    ResultObj->SetStringField(TEXT("target"), TargetDesc);
    if (!TabName.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("tab_name"), TabName);
    }
    if (!WindowTitle.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("window_title"), WindowTitle);
    }
    if (bViewport)
    {
        ResultObj->SetBoolField(TEXT("viewport"), true);
    }
    ResultObj->SetNumberField(TEXT("width"), OutSize.X);
    ResultObj->SetNumberField(TEXT("height"), OutSize.Y);
    ResultObj->SetNumberField(TEXT("bytes"), static_cast<double>(PngBytes.Num()));
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
