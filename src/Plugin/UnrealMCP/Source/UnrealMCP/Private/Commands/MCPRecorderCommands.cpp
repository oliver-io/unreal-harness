#include "Commands/MCPRecorderCommands.h"

#include "Commands/MCPCommonUtils.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "MCPVideoRecorder.h"
#endif

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleCommand(
	const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandType == TEXT("pie_record_start"))
	{
		return HandleRecordStart(Params);
	}
	else if (CommandType == TEXT("pie_record_stop"))
	{
		return HandleRecordStop(Params);
	}
	else if (CommandType == TEXT("pie_record_status"))
	{
		return HandleRecordStatus(Params);
	}
	else if (CommandType == TEXT("pie_record_arm"))
	{
		return HandleRecordArm(Params);
	}
	else if (CommandType == TEXT("pie_record_disarm"))
	{
		return HandleRecordDisarm(Params);
	}

	return FMCPCommonUtils::CreateErrorResponse(
		FString::Printf(TEXT("Unknown recorder command: %s"), *CommandType));
}

#if PLATFORM_WINDOWS

/** Parse the shared encoder knobs (fps/size/bitrate/audio/watchdog) into Config.
 *  Returns a non-null error response on invalid input; null on success. */
static TSharedPtr<FJsonObject> ParseRecorderConfig(
	const TSharedPtr<FJsonObject>& Params, FMCPVideoRecorderConfig& Config)
{
	if (!Params.IsValid())
	{
		return nullptr;
	}

	double Fps = Config.CaptureFps;
	if (Params->TryGetNumberField(TEXT("fps"), Fps))
	{
		if (Fps < 0.0 || Fps > 240.0)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("fps must be 0 (every presented frame) or between 1 and 240."),
				EMCPErrorCode::InvalidArgument,
				TEXT("Typical values: 30 (default), 60; 0 records at the live frame rate."));
		}
		Config.CaptureFps = Fps;
	}

	int32 Width = Config.MaxWidth, Height = Config.MaxHeight;
	if (Params->TryGetNumberField(TEXT("width"), Width)) { Config.MaxWidth = Width; }
	if (Params->TryGetNumberField(TEXT("height"), Height)) { Config.MaxHeight = Height; }
	if (Config.MaxWidth < 2 || Config.MaxHeight < 2 ||
		Config.MaxWidth > 7680 || Config.MaxHeight > 4320)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("width/height must be between 2 and 7680x4320."),
			EMCPErrorCode::InvalidArgument,
			TEXT("The viewport is downscaled to FIT this box, preserving aspect; 1280x720 is the default."));
	}

	int32 Bitrate = Config.BitrateKbps;
	if (Params->TryGetNumberField(TEXT("bitrate_kbps"), Bitrate))
	{
		if (Bitrate < 250 || Bitrate > 100000)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("bitrate_kbps must be between 250 and 100000."),
				EMCPErrorCode::InvalidArgument,
				TEXT("8000 (8 Mbps) is the default — crisp 720p30."));
		}
		Config.BitrateKbps = Bitrate;
	}

	bool bAudio = Config.bAudio;
	if (Params->TryGetBoolField(TEXT("audio"), bAudio))
	{
		Config.bAudio = bAudio;
	}

	double MaxDuration = Config.MaxDurationS;
	if (Params->TryGetNumberField(TEXT("max_duration_s"), MaxDuration))
	{
		if (MaxDuration < 1.0 || MaxDuration > 600.0)
		{
			return FMCPCommonUtils::CreateErrorResponse(
				TEXT("max_duration_s must be between 1 and 600."),
				EMCPErrorCode::InvalidArgument,
				TEXT("This is the hard auto-stop watchdog, not the intended length; default 120."));
		}
		Config.MaxDurationS = MaxDuration;
	}

	return nullptr;
}

static FString ResolveRecorderDirectory(const TSharedPtr<FJsonObject>& Params)
{
	FString Directory;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("directory"), Directory) ||
		Directory.IsEmpty())
	{
		Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCPRecordings"));
	}
	return FPaths::ConvertRelativePathToFull(Directory);
}

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleRecordStart(
	const TSharedPtr<FJsonObject>& Params)
{
	FMCPVideoRecorderConfig Config;
	if (TSharedPtr<FJsonObject> ParseError = ParseRecorderConfig(Params, Config))
	{
		return ParseError;
	}

	FString Filename;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("filename"), Filename) ||
		Filename.IsEmpty())
	{
		Filename = FString::Printf(TEXT("MCP_Recording_%s"),
			*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}
	if (!Filename.EndsWith(TEXT(".mp4")))
	{
		Filename += TEXT(".mp4");
	}
	Config.OutputPath = FPaths::Combine(ResolveRecorderDirectory(Params), Filename);

	return FMCPVideoRecorder::Get().Start(Config);
}

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleRecordStop(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMCPVideoRecorder::Get().Stop();
}

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleRecordStatus(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMCPVideoRecorder::Get().Status();
}

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleRecordArm(
	const TSharedPtr<FJsonObject>& Params)
{
	FMCPVideoRecorderConfig Config;
	if (TSharedPtr<FJsonObject> ParseError = ParseRecorderConfig(Params, Config))
	{
		return ParseError;
	}

	FString BaseName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("base_name"), BaseName) ||
		BaseName.IsEmpty())
	{
		BaseName = TEXT("MCP_Take");
	}

	return FMCPVideoRecorder::Get().Arm(Config, ResolveRecorderDirectory(Params), BaseName);
}

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleRecordDisarm(
	const TSharedPtr<FJsonObject>& Params)
{
	return FMCPVideoRecorder::Get().Disarm();
}

#else // !PLATFORM_WINDOWS

static TSharedPtr<FJsonObject> RecorderUnsupported()
{
	return FMCPCommonUtils::CreateErrorResponse(
		TEXT("PIE video recording is Windows-only (Media Foundation encoder)."),
		EMCPErrorCode::FeatureDisabled,
		TEXT("Use editor_screenshot / pie_capture_from_pose for stills on this platform."));
}

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleRecordStart(const TSharedPtr<FJsonObject>&)
{
	return RecorderUnsupported();
}

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleRecordStop(const TSharedPtr<FJsonObject>&)
{
	return RecorderUnsupported();
}

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleRecordStatus(const TSharedPtr<FJsonObject>&)
{
	return RecorderUnsupported();
}

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleRecordArm(const TSharedPtr<FJsonObject>&)
{
	return RecorderUnsupported();
}

TSharedPtr<FJsonObject> FMCPRecorderCommands::HandleRecordDisarm(const TSharedPtr<FJsonObject>&)
{
	return RecorderUnsupported();
}

#endif // PLATFORM_WINDOWS
