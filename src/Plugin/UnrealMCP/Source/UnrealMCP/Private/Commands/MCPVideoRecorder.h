#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"
#include "Json.h"

#if PLATFORM_WINDOWS

#include "AudioDeviceHandle.h"

class FFrameGrabber;
class FSceneViewport;
class FMCPVideoEncoderWorker;
class FMCPSubmixCapture;

/** Resolved pie_record_start parameters. */
struct FMCPVideoRecorderConfig
{
	/** Encode-size ceiling; the PIE viewport is GPU-downscaled to fit inside this
	 *  box preserving aspect (then rounded down to even for H.264). */
	int32 MaxWidth = 1280;
	int32 MaxHeight = 720;
	/** Frames per second to capture. 0 = every presented frame ("full"). */
	double CaptureFps = 30.0;
	int32 BitrateKbps = 8000;
	/** Hard watchdog — the recording auto-stops and finalizes past this. */
	double MaxDurationS = 120.0;
	/** Capture the PIE world's audio (main-submix mix — exactly what the player
	 *  hears) as an AAC track in the same MP4. Falls back to video-only when no
	 *  audio device / encoder support exists. */
	bool bAudio = true;
	/** Absolute output .mp4 path. */
	FString OutputPath;
};

/**
 * Records the live PIE viewport to an H.264 MP4 — the pie_record_* capture
 * primitive (see the PIE video plan).
 *
 * Pipeline: FFrameGrabber hooks the Slate back buffer (the PRESENTED frame, so
 * UMG/HUD is composited in), crops to the game-viewport widget rect, GPU-downscales
 * to the encode size, and async-readbacks via fenced staging surfaces. Our frame
 * payload receives each resolved BGRA frame on the render thread and enqueues a
 * tightly-packed copy to a bounded queue; a dedicated encoder thread feeds Media
 * Foundation's IMFSinkWriter (RGB32 in → H.264/MP4 out, hardware MFTs allowed) —
 * no external binary, no extra plugins.
 *
 * Threading: Start/Stop/Status run on the game thread (bridge dispatch). A
 * game-thread ticker throttles capture to CaptureFps and enforces the duration
 * watchdog; frames are copied on the render thread; encoding runs on its own
 * thread. Backpressure DROPS frames (counted) rather than stalling the game.
 *
 * One recording at a time. Auto-stops on PIE end (PrePIEEnded) and on the
 * MaxDurationS watchdog, so an abandoned recording never leaks an encoder or an
 * unbounded file.
 */
class FMCPVideoRecorder
{
public:
	static FMCPVideoRecorder& Get();

	/** Begin recording. Game thread only. Returns the start result or an error. */
	TSharedPtr<FJsonObject> Start(const FMCPVideoRecorderConfig& InConfig);

	/**
	 * ARM the recorder: from now until Disarm, EVERY PIE session on this editor
	 * is recorded automatically — capture begins the moment the PIE world is up
	 * (PostPIEStarted + a short present-retry) and finalizes when PIE ends or
	 * the max_duration_s watchdog fires. Takes are auto-numbered
	 * <base_name>_NN.mp4 under the configured directory. If PIE is already
	 * running when armed, recording starts immediately.
	 */
	TSharedPtr<FJsonObject> Arm(
		const FMCPVideoRecorderConfig& InConfig, const FString& InDirectory, const FString& InBaseName);

	/** Stop auto-recording future PIE sessions. An ACTIVE recording is left
	 *  running (it still auto-finalizes on PIE end / watchdog / explicit stop). */
	TSharedPtr<FJsonObject> Disarm();

	/** Finalize the MP4 and return metadata. Game thread only. Idempotent-safe:
	 *  errors cleanly when nothing is recording. */
	TSharedPtr<FJsonObject> Stop(const FString& Reason = TEXT("requested"));

	/** Snapshot of the current recording state (works when idle too). */
	TSharedPtr<FJsonObject> Status() const;

	bool IsRecording() const { return bRecording; }

	/** Render-thread sink: called by the frame payload with the resolved,
	 *  downscaled BGRA frame. Copies + enqueues (or drops under backpressure). */
	void OnFrameResolved_RenderThread(
		const FColor* Pixels, FIntPoint BufferSize, FIntPoint TargetSize, double TimeSeconds);

private:
	FMCPVideoRecorder() = default;

	bool TickGameThread(float DeltaSeconds);
	void OnPrePIEEnded(bool bIsSimulating);

	// ── Armed auto-record ────────────────────────────────────────────────
	void OnPostPIEStarted(bool bIsSimulating);
	/** Retry an armed auto-start until the viewport presents (bounded). */
	bool TickArmedStart(float DeltaSeconds);
	/** Start the next auto-numbered take from ArmedConfig. */
	bool TryStartArmedTake();
	/** Next free <base>_NN.mp4 path under the armed directory. */
	FString NextArmedTakePath() const;

	FMCPVideoRecorderConfig Config;
	FString RecordingId;
	/** Readback/grab size — the NATIVE viewport resolution (evened). FFrameGrabber
	 *  cannot GPU-downscale: its resolve normalizes the destination quad against
	 *  the capture-rect size, so a smaller readback leaves the image parked in the
	 *  top-left with black margins. Downscale happens in the encoder instead. */
	FIntPoint CaptureSize = FIntPoint::ZeroValue;
	/** Encoded output size (fit inside MaxWidth/MaxHeight; may equal CaptureSize
	 *  when the MF video processor rejects the resize). */
	FIntPoint EncodeSize = FIntPoint::ZeroValue;
	FIntPoint SourceSize = FIntPoint::ZeroValue;
	double StartTimeS = 0.0;
	double LastCaptureTimeS = 0.0;
	FString LastStopReason;

	TUniquePtr<FFrameGrabber> Grabber;
	TUniquePtr<FMCPVideoEncoderWorker> Worker;
	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle PrePIEEndedHandle;

	/** Audio capture — main-submix listener on the PIE world's audio device.
	 *  Null when recording video-only (audio off, no device, or AAC rejected). */
	TSharedPtr<FMCPSubmixCapture, ESPMode::ThreadSafe> AudioCapture;
	/** Keeps the PIE audio device alive for the unregister in Stop(). */
	FAudioDeviceHandle AudioDeviceHandle;
	FString AudioNote;           // why audio is off, when it is
	int32 AudioSampleRate = 0;   // 0 = no audio track

	/** True from Start until Stop begins tearing down — gates the render-thread
	 *  sink so late in-flight resolves after Stop are ignored. */
	FThreadSafeBool bAcceptFrames = false;
	FThreadSafeBool bRecording = false;

	FThreadSafeCounter FramesCaptured;   // resolved frames handed to the encoder queue
	FThreadSafeCounter FramesDropped;    // resolved frames discarded (queue full)

	// ── Armed auto-record state (game thread only) ───────────────────────
	bool bArmed = false;
	FMCPVideoRecorderConfig ArmedConfig; // OutputPath ignored — derived per take
	FString ArmedDirectory;
	FString ArmedBaseName;
	int32 ArmedTakesRecorded = 0;
	FDelegateHandle PostPIEStartedHandle;
	FTSTicker::FDelegateHandle ArmRetryTickerHandle;
	double ArmRetryDeadlineS = 0.0;
};

#endif // PLATFORM_WINDOWS
