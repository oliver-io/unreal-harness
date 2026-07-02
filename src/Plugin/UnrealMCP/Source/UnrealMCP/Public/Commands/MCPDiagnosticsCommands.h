#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Editor diagnostics tools — perf snapshot, content browser refresh, and
 * future log-filter / window-screenshot extensions.
 * Mirrors mcp/docs/todo/11_editor_diagnostics.md.
 *
 * Surface (rolling):
 *   - editor_perf_snapshot
 *   - content_browser_refresh
 *   - editor_window_screenshot   (this commit)
 *   - read_logs (extended in Python wrapper layer; not on the C++ side here)
 *
 * Doc 11 invariant: no `save_all_dirty` (philosophy #5 — auto-save policy
 * supersedes flush operations).
 */
class FMCPDiagnosticsCommands
{
public:
    FMCPDiagnosticsCommands() = default;

    TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
    TSharedPtr<FJsonObject> HandlePerfSnapshot(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleContentBrowserRefresh(const TSharedPtr<FJsonObject>& Params);
    TSharedPtr<FJsonObject> HandleWindowScreenshot(const TSharedPtr<FJsonObject>& Params);
};
