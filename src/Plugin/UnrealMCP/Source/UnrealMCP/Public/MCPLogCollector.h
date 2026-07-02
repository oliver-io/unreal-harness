#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "Misc/DateTime.h"
#include "HAL/CriticalSection.h"

/**
 * Unified log collector for the MCP bridge — WRITE SIDE ONLY.
 *
 * Registers as an FOutputDevice with GLog to capture ALL UE_LOG output,
 * and hooks editor delegates (PIE lifecycle, compilation events) to inject
 * structured events. Everything is appended as tagged, sequenced lines to
 * a persistent log file (Saved/Logs/MCP_Unified.log).
 *
 * The Python MCP server reads this file directly for the read_logs tool —
 * no TCP round-trip needed for reads.
 *
 * Line format:
 *   [seq] ISO-timestamp [SOURCE:Category] Verbosity: message
 *   [42] 2026-04-22T14:23:01.234 [LOG:LogTemp] Display: Player jumped
 *   [43] 2026-04-22T14:23:05.100 [PIE:Lifecycle] Display: PIE session starting
 *
 * Thread-safe: Serialize() is called from any thread. File writes are
 * serialized via FCriticalSection with immediate flush.
 */
class UNREALMCP_API FMCPLogCollector : public FOutputDevice
{
public:
	FMCPLogCollector();
	virtual ~FMCPLogCollector() override;

	/** Register with GLog, bind editor delegates, open log file. Call from Bridge::Initialize. */
	void Initialize();

	/** Unregister from GLog, unbind delegates, close file. Call from Bridge::Deinitialize. */
	void Shutdown();

	// -- FOutputDevice interface (called from ANY thread) -----------------------

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
	                        const FName& Category) override;

	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	// -- Explicit event injection -----------------------------------------------

	/** Insert a structured event from a known source (PIE, LIVECODING, etc.). */
	void InjectEvent(const FString& Source, const FString& Category,
	                  ELogVerbosity::Type Verbosity, const FString& Message);

	/** Path to the unified log file. */
	FString GetLogFilePath() const { return LogFilePath; }

private:
	/** Format and append one line to the log file. Thread-safe. */
	void AppendLine(const FString& Source, const FString& Category,
	                 ELogVerbosity::Type Verbosity, const FString& Message);

	/** Convert ELogVerbosity to a short display string. */
	static const TCHAR* VerbosityToString(ELogVerbosity::Type Verbosity);

	FString LogFilePath;

	mutable FCriticalSection WriteLock;
	IFileHandle*             FileHandle = nullptr;
	TAtomic<uint64>          CurrentSequence{0};

	bool bInitialized = false;

	// Delegate handles for cleanup
	FDelegateHandle PreBeginPIEHandle;
	FDelegateHandle PostPIEStartedHandle;
	FDelegateHandle EndPIEHandle;
};
