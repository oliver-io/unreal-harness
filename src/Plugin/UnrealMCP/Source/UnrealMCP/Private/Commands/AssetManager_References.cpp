// See AssetManager.cpp for the rest of the FAssetManager implementation.
// This file holds the asset_references handler (mcp/docs/todo/4_asset_references.md).
// Split out so AssetManager.cpp stays under the project's 600 LOC ceiling.

#include "Commands/AssetManager.h"
#include "Commands/MCPCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/AssetRegistryInterface.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformTime.h"
#include "Containers/Set.h"

namespace
{
    // Soft cap on recursive depth. Doc invariant: depth caps are non-negotiable;
    // a request for unbounded recursion is rejected (invalid_argument).
    constexpr int32 MaxDepth = 10;

    // Soft wall-clock cap on a single request, in seconds. The doc requires "per-call
    // wall-clock cap protects against pathological graphs"; pathological here means a
    // recursive expansion that fans out into thousands of touched packages.
    constexpr double WallClockBudgetSeconds = 5.0;

    constexpr int32 DefaultLimit = 200;
    constexpr int32 MaxLimit = 1000;

    // Normalize either an object path "/Game/Foo/Bar.Bar" or a package name
    // "/Game/Foo/Bar" to the registry's package-name form.
    FName NormalizeToPackageName(const FString& In)
    {
        FString Pkg = In;
        int32 DotIdx = INDEX_NONE;
        if (Pkg.FindChar(TEXT('.'), DotIdx))
        {
            Pkg = Pkg.Left(DotIdx);
        }
        return FName(*Pkg);
    }

    // Map (Category, Properties) -> wire kind. Manage references are deliberately
    // skipped (out of scope per the doc).
    bool TryClassifyKind(UE::AssetRegistry::EDependencyCategory Category,
                         UE::AssetRegistry::EDependencyProperty Properties,
                         FString& OutKind)
    {
        if (Category == UE::AssetRegistry::EDependencyCategory::Package)
        {
            const bool bHard = !!(Properties & UE::AssetRegistry::EDependencyProperty::Hard);
            OutKind = bHard ? TEXT("hard") : TEXT("soft");
            return true;
        }
        if (Category == UE::AssetRegistry::EDependencyCategory::SearchableName)
        {
            OutKind = TEXT("searchable_name");
            return true;
        }
        return false;
    }

    struct FCollected
    {
        FName Path;
        FString Kind;
        bool bDirect = false;
    };
}

TSharedPtr<FJsonObject> FAssetManager::AssetReferences(const TSharedPtr<FJsonObject>& Params)
{
    using namespace UE::AssetRegistry;

    // ── Parameter validation ────────────────────────────────────────────────
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'asset_path' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Pass a package name (\"/Game/Foo/Bar\") or object path (\"/Game/Foo/Bar.Bar\")."));
    }

    FString Direction;
    if (!Params->TryGetStringField(TEXT("direction"), Direction) || Direction.IsEmpty())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("Missing 'direction' parameter"),
            EMCPErrorCode::InvalidArgument,
            TEXT("Direction is one of: outbound, inbound."));
    }
    const bool bOutbound = Direction.Equals(TEXT("outbound"), ESearchCase::IgnoreCase);
    const bool bInbound  = Direction.Equals(TEXT("inbound"),  ESearchCase::IgnoreCase);
    if (!bOutbound && !bInbound)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Invalid 'direction' value: %s"), *Direction),
            EMCPErrorCode::InvalidArgument,
            TEXT("Direction must be exactly 'outbound' or 'inbound' (lowercase)."));
    }

    int32 Depth = 1;
    Params->TryGetNumberField(TEXT("depth"), Depth);
    if (Depth < 1 || Depth > MaxDepth)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'depth' must be in [1, %d]"), MaxDepth),
            EMCPErrorCode::OutOfRange,
            TEXT("Default depth is 1 (one-hop). Recursion is bounded; unbounded traversal is rejected by design."));
    }

    int32 Cursor = 0;
    Params->TryGetNumberField(TEXT("cursor"), Cursor);
    if (Cursor < 0)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("'cursor' must be non-negative"),
            EMCPErrorCode::OutOfRange,
            TEXT("Pass the 'next_cursor' value from a previous response, or omit for the first page."));
    }

    int32 Limit = DefaultLimit;
    Params->TryGetNumberField(TEXT("limit"), Limit);
    if (Limit <= 0 || Limit > MaxLimit)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("'limit' must be in [1, %d]"), MaxLimit),
            EMCPErrorCode::OutOfRange,
            TEXT("Default limit is 200; the upper bound prevents accidentally pulling the entire graph."));
    }

    bool bIncludeHard = true;
    bool bIncludeSoft = true;
    bool bIncludeSearchableName = true;
    Params->TryGetBoolField(TEXT("include_hard"), bIncludeHard);
    Params->TryGetBoolField(TEXT("include_soft"), bIncludeSoft);
    Params->TryGetBoolField(TEXT("include_searchable_name"), bIncludeSearchableName);

    if (!bIncludeHard && !bIncludeSoft && !bIncludeSearchableName)
    {
        return FMCPCommonUtils::CreateErrorResponse(
            TEXT("All reference kinds disabled — nothing to return"),
            EMCPErrorCode::InvalidArgument,
            TEXT("At least one of include_hard / include_soft / include_searchable_name must remain true."));
    }

    // ── Registry handle ─────────────────────────────────────────────────────
    FAssetRegistryModule& RegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& Registry = RegistryModule.Get();

    // BFS frontier — every entry holds the package name + the level it was
    // discovered at (1 == direct).
    struct FFrontierEntry
    {
        FName Package;
        int32 Level = 1;
    };

    const FName StartPackage = NormalizeToPackageName(AssetPath);
    if (StartPackage.IsNone())
    {
        return FMCPCommonUtils::CreateErrorResponse(
            FString::Printf(TEXT("Could not normalize asset_path to a package name: %s"), *AssetPath),
            EMCPErrorCode::InvalidPath,
            TEXT("Expected a path that begins with '/Game/' or '/<Module>/' — e.g. /Game/Foo/Bar."));
    }

    // Determine which categories to query — anything we can return ends up here.
    EDependencyCategory CategoryMask = EDependencyCategory::None;
    if (bIncludeHard || bIncludeSoft)
    {
        CategoryMask |= EDependencyCategory::Package;
    }
    if (bIncludeSearchableName)
    {
        CategoryMask |= EDependencyCategory::SearchableName;
    }

    TSet<FName> Visited;
    Visited.Add(StartPackage);
    TArray<FFrontierEntry> Frontier;
    Frontier.Add({StartPackage, 1});

    TArray<FCollected> Collected;
    bool bWallClockCapped = false;
    const double StartTime = FPlatformTime::Seconds();

    while (Frontier.Num() > 0)
    {
        if (FPlatformTime::Seconds() - StartTime > WallClockBudgetSeconds)
        {
            bWallClockCapped = true;
            break;
        }

        const FFrontierEntry Current = Frontier.Pop(EAllowShrinking::No);

        TArray<FAssetDependency> Hits;
        if (bOutbound)
        {
            Registry.GetDependencies(FAssetIdentifier(Current.Package), Hits, CategoryMask);
        }
        else
        {
            Registry.GetReferencers(FAssetIdentifier(Current.Package), Hits, CategoryMask);
        }

        for (const FAssetDependency& Hit : Hits)
        {
            FString Kind;
            if (!TryClassifyKind(Hit.Category, Hit.Properties, Kind))
            {
                continue;
            }
            if (Kind == TEXT("hard")            && !bIncludeHard)            continue;
            if (Kind == TEXT("soft")            && !bIncludeSoft)            continue;
            if (Kind == TEXT("searchable_name") && !bIncludeSearchableName)  continue;

            const FName HitPackage = Hit.AssetId.PackageName;
            if (HitPackage.IsNone() || HitPackage == StartPackage)
            {
                continue;
            }

            const bool bAlreadyVisited = Visited.Contains(HitPackage);
            if (!bAlreadyVisited)
            {
                Visited.Add(HitPackage);

                FCollected Row;
                Row.Path = HitPackage;
                Row.Kind = MoveTemp(Kind);
                Row.bDirect = (Current.Level == 1);
                Collected.Add(MoveTemp(Row));

                // Schedule next level only if we have depth remaining.
                if (Current.Level < Depth)
                {
                    Frontier.Add({HitPackage, Current.Level + 1});
                }
            }
        }
    }

    // Stable sort by path for deterministic pagination.
    Collected.Sort([](const FCollected& A, const FCollected& B) { return A.Path.LexicalLess(B.Path); });

    const int32 TotalMatched = Collected.Num();
    const int32 PageEnd = FMath::Min(Cursor + Limit, TotalMatched);

    TArray<TSharedPtr<FJsonValue>> RowsArray;
    RowsArray.Reserve(FMath::Max(0, PageEnd - Cursor));
    for (int32 i = Cursor; i < PageEnd; ++i)
    {
        const FCollected& Row = Collected[i];
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("path"), Row.Path.ToString());
        Entry->SetStringField(TEXT("kind"), Row.Kind);
        Entry->SetBoolField(TEXT("direct"), Row.bDirect);
        RowsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), StartPackage.ToString());
    ResultObj->SetStringField(TEXT("direction"), bOutbound ? TEXT("outbound") : TEXT("inbound"));
    ResultObj->SetNumberField(TEXT("depth"), Depth);
    ResultObj->SetArrayField(TEXT("references"), RowsArray);
    ResultObj->SetNumberField(TEXT("returned_count"), RowsArray.Num());
    ResultObj->SetNumberField(TEXT("total_matched"), TotalMatched);
    ResultObj->SetNumberField(TEXT("cursor"), Cursor);
    if (PageEnd < TotalMatched)
    {
        ResultObj->SetNumberField(TEXT("next_cursor"), PageEnd);
    }
    ResultObj->SetBoolField(TEXT("wall_clock_capped"), bWallClockCapped);
    ResultObj->SetBoolField(TEXT("success"), true);
    return ResultObj;
}
