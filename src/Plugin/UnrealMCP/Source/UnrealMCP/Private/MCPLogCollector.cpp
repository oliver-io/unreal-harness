#include "MCPLogCollector.h"
#include "Commands/MCPCommonUtils.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceRedirector.h"
#include "HAL/PlatformFileManager.h"
#include "Editor.h"
#include "ILiveCodingModule.h"

// File-scope atomic — Serialize() reads from any thread, delegates write from game thread.
static TAtomic<bool> bPIEActive{false};

FMCPLogCollector::FMCPLogCollector()
{
}

FMCPLogCollector::~FMCPLogCollector()
{
	if (bInitialized)
	{
		Shutdown();
	}
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void FMCPLogCollector::Initialize()
{
	if (bInitialized)
	{
		return;
	}

	// --- Open log file (append mode) ---
	LogFilePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Logs"), TEXT("MCP_Unified.log"));

	// Ensure directory exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*FPaths::GetPath(LogFilePath));

	FileHandle = PlatformFile.OpenWrite(*LogFilePath, /*bAppend=*/true, /*bAllowRead=*/true);
	if (!FileHandle)
	{
		UE_LOG(LogUnrealMCP, Error, TEXT("FMCPLogCollector: Failed to open log file: %s"), *LogFilePath);
		return;
	}

	UE_LOG(LogUnrealMCP, Display, TEXT("FMCPLogCollector: Writing to %s"), *LogFilePath);

	// --- Register with GLog ---
	GLog->AddOutputDevice(this);

	// --- Bind PIE delegates ---
	if (GEditor)
	{
		PreBeginPIEHandle = FEditorDelegates::PreBeginPIE.AddLambda([this](bool bIsSimulating)
		{
			// Set PIE active BEFORE the lifecycle event so the event itself
			// doesn't get tagged as EDITOR
			bPIEActive.Store(true);
			InjectEvent(TEXT("PIE"), TEXT("Lifecycle"), ELogVerbosity::Display,
				bIsSimulating ? TEXT("Simulate session starting") : TEXT("PIE session starting"));
		});

		PostPIEStartedHandle = FEditorDelegates::PostPIEStarted.AddLambda([this](bool bIsSimulating)
		{
			InjectEvent(TEXT("PIE"), TEXT("Lifecycle"), ELogVerbosity::Display,
				bIsSimulating ? TEXT("Simulate session running") : TEXT("PIE session running"));
		});

		EndPIEHandle = FEditorDelegates::EndPIE.AddLambda([this](bool bIsSimulating)
		{
			InjectEvent(TEXT("PIE"), TEXT("Lifecycle"), ELogVerbosity::Display,
				bIsSimulating ? TEXT("Simulate session ended") : TEXT("PIE session ended"));
			// Clear PIE active AFTER the lifecycle event
			bPIEActive.Store(false);
		});
	}

	// Write a startup marker
	InjectEvent(TEXT("MCP"), TEXT("System"), ELogVerbosity::Display, TEXT("MCP Log Collector initialized"));

	bInitialized = true;
}

void FMCPLogCollector::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}

	bInitialized = false;
	bPIEActive.Store(false);

	// Write shutdown marker before closing
	InjectEvent(TEXT("MCP"), TEXT("System"), ELogVerbosity::Display, TEXT("MCP Log Collector shutting down"));

	// Unregister from GLog
	if (GLog)
	{
		GLog->RemoveOutputDevice(this);
	}

	// Unbind PIE delegates
	if (GEditor)
	{
		FEditorDelegates::PreBeginPIE.Remove(PreBeginPIEHandle);
		FEditorDelegates::PostPIEStarted.Remove(PostPIEStartedHandle);
		FEditorDelegates::EndPIE.Remove(EndPIEHandle);
	}

	// Close file
	{
		FScopeLock ScopeLock(&WriteLock);
		if (FileHandle)
		{
			delete FileHandle;
			FileHandle = nullptr;
		}
	}
}

// ---------------------------------------------------------------------------
// FOutputDevice — captures all UE_LOG calls
// ---------------------------------------------------------------------------

void FMCPLogCollector::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
                                  const FName& Category)
{
	if (!FileHandle)
	{
		return;
	}

	if (!V || V[0] == '\0')
	{
		return;
	}

	// Tag as PIE while a Play-In-Editor session is active, EDITOR otherwise.
	// This lets Claude isolate everything that happened during gameplay with
	// a single sources=PIE filter.
	const TCHAR* Source = bPIEActive.Load() ? TEXT("PIE") : TEXT("EDITOR");

	AppendLine(Source, Category.ToString(), Verbosity, V);
}

// ---------------------------------------------------------------------------
// Explicit event injection
// ---------------------------------------------------------------------------

void FMCPLogCollector::InjectEvent(const FString& Source, const FString& Category,
                                    ELogVerbosity::Type Verbosity, const FString& Message)
{
	if (!FileHandle)
	{
		return;
	}

	AppendLine(Source, Category, Verbosity, Message);
}

// ---------------------------------------------------------------------------
// File writing
// ---------------------------------------------------------------------------

void FMCPLogCollector::AppendLine(const FString& Source, const FString& Category,
                                   ELogVerbosity::Type Verbosity, const FString& Message)
{
	const uint64 Seq = ++CurrentSequence;

	const FDateTime Now = FDateTime::Now();
	const FString Timestamp = Now.ToString(TEXT("%Y-%m-%dT%H:%M:%S.")) +
		FString::Printf(TEXT("%03d"), Now.GetMillisecond());

	// Format: [seq] timestamp [SOURCE:Category] Verbosity: message
	// Replace embedded newlines with " | " to keep one-line-per-entry
	FString CleanMessage = Message;
	CleanMessage.TrimEndInline();
	CleanMessage.ReplaceInline(TEXT("\r\n"), TEXT(" | "));
	CleanMessage.ReplaceInline(TEXT("\n"), TEXT(" | "));
	CleanMessage.ReplaceInline(TEXT("\r"), TEXT(" | "));

	const FString Line = FString::Printf(TEXT("[%llu] %s [%s:%s] %s: %s\n"),
		Seq, *Timestamp, *Source, *Category, VerbosityToString(Verbosity), *CleanMessage);

	// Convert to UTF-8 and write
	FTCHARToUTF8 Utf8Line(*Line);

	{
		FScopeLock ScopeLock(&WriteLock);
		if (FileHandle)
		{
			FileHandle->Write(reinterpret_cast<const uint8*>(Utf8Line.Get()), Utf8Line.Length());
			FileHandle->Flush();
		}
	}
}

const TCHAR* FMCPLogCollector::VerbosityToString(ELogVerbosity::Type Verbosity)
{
	switch (Verbosity)
	{
	case ELogVerbosity::Fatal:         return TEXT("Fatal");
	case ELogVerbosity::Error:         return TEXT("Error");
	case ELogVerbosity::Warning:       return TEXT("Warning");
	case ELogVerbosity::Display:       return TEXT("Display");
	case ELogVerbosity::Log:           return TEXT("Log");
	case ELogVerbosity::Verbose:       return TEXT("Verbose");
	case ELogVerbosity::VeryVerbose:   return TEXT("VeryVerbose");
	default:                           return TEXT("Unknown");
	}
}
