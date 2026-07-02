#pragma once

#include "CoreMinimal.h"
#include "Json.h"

/**
 * Handler class for PIE video-recording MCP commands.
 * pie_record_start / pie_record_stop / pie_record_status — record the live PIE
 * viewport (UMG composited in) to an H.264 MP4 via the in-engine recorder
 * (FMCPVideoRecorder; Windows/Media Foundation). The capture primitive of the
 * PIE video plan — analysis lives server-side.
 */
class UNREALMCP_API FMCPRecorderCommands
{
public:
	FMCPRecorderCommands() = default;

	TSharedPtr<FJsonObject> HandleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	TSharedPtr<FJsonObject> HandleRecordStart(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleRecordStop(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleRecordStatus(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleRecordArm(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> HandleRecordDisarm(const TSharedPtr<FJsonObject>& Params);
};
