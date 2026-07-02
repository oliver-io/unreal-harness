#include "MCPVideoRecorder.h"

#if PLATFORM_WINDOWS

#include "MCPCommonUtils.h"

#include "AudioDevice.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "FrameGrabber.h"
#include "ISubmixBufferListener.h"
#include "Misc/ScopeLock.h"
#include "Sound/SoundSubmix.h"
#include "HAL/FileManager.h"
#include "HAL/RunnableThread.h"
#include "IAssetViewport.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PlayInEditorDataTypes.h"
#include "RenderingThread.h"
#include "Slate/SceneViewport.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include "Microsoft/COMPointer.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include "Windows/HideWindowsPlatformTypes.h"

// ---------------------------------------------------------------------------
// Encoder worker — owns the Media Foundation sink writer on its own thread.
// All MF calls (create, WriteSample, Finalize) happen on this thread only.
// ---------------------------------------------------------------------------

namespace
{

struct FMCPVideoFrame
{
	TArray64<uint8> Bgra; // tightly packed Width*Height*4, top-down
	double TimeSeconds = 0.0;
};

/** One submix callback's worth of audio, downmixed to interleaved stereo PCM16. */
struct FMCPAudioChunk
{
	TArray<int16> Pcm;         // Frames * 2 samples, interleaved L/R
	double TimeSeconds = 0.0;  // recording-timeline start of this chunk
	int32 Frames = 0;
};

constexpr int32 MaxPendingFrames = 8;        // render→encoder backpressure bound
constexpr int32 MaxPendingAudioChunks = 256; // ~2.7s of 512-frame buffers @48kHz
constexpr int32 AudioBitrateBytesPerSec = 24000; // 192 kbps AAC-LC

} // namespace

class FMCPVideoEncoderWorker : public FRunnable
{
public:
	FMCPVideoEncoderWorker(
		const FString& InPath, FIntPoint InInputSize, FIntPoint InOutputSize,
		double InNominalFps, int32 InBitrateKbps, int32 InAudioSampleRate)
		: Path(InPath)
		, InputSize(InInputSize)
		, OutputSize(InOutputSize)
		, NominalFps(InNominalFps > 0.0 ? InNominalFps : 60.0)
		, BitrateKbps(InBitrateKbps)
		, AudioSampleRate(InAudioSampleRate)
		, bAudioEnabled(InAudioSampleRate > 0)
	{
		ReadyEvent = FPlatformProcess::GetSynchEventFromPool(true /*manual reset*/);
	}

	virtual ~FMCPVideoEncoderWorker() override
	{
		StopThread(0.0);
		if (ReadyEvent)
		{
			FPlatformProcess::ReturnSynchEventToPool(ReadyEvent);
			ReadyEvent = nullptr;
		}
	}

	bool StartThread()
	{
		Thread.Reset(FRunnableThread::Create(this, TEXT("MCPVideoEncoder"), 0, TPri_Normal));
		return Thread.IsValid();
	}

	/** Block until the sink writer is created (or failed) — bounds Start() latency. */
	bool WaitUntilReady(double TimeoutS)
	{
		return ReadyEvent->Wait(FTimespan::FromSeconds(TimeoutS)) && !bFailed;
	}

	/** Render thread: hand a frame to the encoder. Returns false when full (drop). */
	bool EnqueueFrame(TUniquePtr<FMCPVideoFrame> Frame)
	{
		if (bFinishRequested || bFailed || PendingCount.GetValue() >= MaxPendingFrames)
		{
			return false;
		}
		PendingCount.Increment();
		Queue.Enqueue(MoveTemp(Frame));
		return true;
	}

	/** Audio render thread: hand a stereo PCM16 chunk to the encoder. */
	bool EnqueueAudio(TUniquePtr<FMCPAudioChunk> Chunk)
	{
		if (!bAudioEnabled || bFinishRequested || bFailed ||
			AudioPendingCount.GetValue() >= MaxPendingAudioChunks)
		{
			return false;
		}
		AudioPendingCount.Increment();
		AudioQueue.Enqueue(MoveTemp(Chunk));
		return true;
	}

	/** Ask the worker to drain the queue, finalize the MP4, and exit. */
	void RequestFinish() { bFinishRequested = true; }

	/** Join with a bound; returns true if the thread finished in time. */
	bool StopThread(double TimeoutS)
	{
		if (!Thread.IsValid())
		{
			return true;
		}
		RequestFinish();
		const double Deadline = FPlatformTime::Seconds() + TimeoutS;
		while (!bDone && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.01f);
		}
		if (bDone)
		{
			Thread->WaitForCompletion();
			Thread.Reset();
			return true;
		}
		// Worker is wedged (should not happen — WriteSample/Finalize are bounded).
		// Kill so we never hang the editor; the MP4 may be unplayable.
		Thread->Kill(true);
		Thread.Reset();
		return false;
	}

	uint64 GetFramesWritten() const { return static_cast<uint64>(FramesWritten.GetValue()); }
	double GetLastTimestampS() const { return LastTimestampS; }
	bool HasFailed() const { return bFailed; }
	FString GetError() const
	{
		FScopeLock Lock(&ErrorCS);
		return Error;
	}

	// FRunnable
	virtual uint32 Run() override
	{
		const bool bComOk = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
		HRESULT Hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
		if (FAILED(Hr))
		{
			Fail(FString::Printf(TEXT("MFStartup failed (hr=0x%08x)"), (uint32)Hr));
		}
		else
		{
			// Hardware MFTs first (NVENC/AMF/QuickSync behind the MF facade);
			// software H.264 MFT as the always-present fallback. When input and
			// output sizes differ, the sink writer must insert the video
			// processor to downscale — if a combination rejects that, retry with
			// output = input (native-res file rather than no file). If the AAC
			// audio stream is what fails, degrade to video-only rather than to
			// no recording at all.
			bool bCreated = CreateWriter(true) || CreateWriter(false);
			if (!bCreated && OutputSize != InputSize)
			{
				OutputSize = InputSize;
				bCreated = CreateWriter(true) || CreateWriter(false);
			}
			if (!bCreated && bAudioEnabled)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("MCPVideoRecorder: MP4 writer with AAC audio failed (%s); retrying video-only."),
					*GetError());
				bAudioEnabled = false;
				bCreated = CreateWriter(true) || CreateWriter(false);
			}
			if (!bCreated)
			{
				Fail(FString::Printf(TEXT("Could not create MP4 sink writer for '%s': %s"),
					*Path, *GetError()));
			}
		}
		ReadyEvent->Trigger();

		if (!bFailed)
		{
			EncodeLoop();
			const HRESULT FinalizeHr = Writer ? Writer->Finalize() : E_FAIL;
			if (FAILED(FinalizeHr))
			{
				Fail(FString::Printf(TEXT("MP4 finalize failed (hr=0x%08x)"), (uint32)FinalizeHr));
			}
		}
		DrainAndDiscard();

		Writer.Reset();
		if (SUCCEEDED(Hr))
		{
			MFShutdown();
		}
		if (bComOk)
		{
			CoUninitialize();
		}
		bDone = true;
		return 0;
	}

private:
	void Fail(const FString& InError)
	{
		{
			FScopeLock Lock(&ErrorCS);
			if (Error.IsEmpty())
			{
				Error = InError;
			}
		}
		bFailed = true;
		UE_LOG(LogTemp, Error, TEXT("MCPVideoRecorder: %s"), *InError);
	}

	bool CreateWriter(bool bAllowHardware)
	{
		Writer.Reset();

		TComPtr<IMFAttributes> Attributes;
		HRESULT Hr = MFCreateAttributes(&Attributes, 2);
		if (SUCCEEDED(Hr) && bAllowHardware)
		{
			Attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1u);
		}
		if (SUCCEEDED(Hr))
		{
			Attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, 1u);
		}
		if (FAILED(Hr))
		{
			Fail(FString::Printf(TEXT("MFCreateAttributes failed (hr=0x%08x)"), (uint32)Hr));
			return false;
		}

		TComPtr<IMFSinkWriter> NewWriter;
		Hr = MFCreateSinkWriterFromURL(*Path, nullptr, Attributes, &NewWriter);
		if (FAILED(Hr))
		{
			Fail(FString::Printf(TEXT("MFCreateSinkWriterFromURL failed (hr=0x%08x)"), (uint32)Hr));
			return false;
		}

		// Frame rate as a ratio — round the nominal fps into num/1000.
		const UINT32 FpsNum = static_cast<UINT32>(FMath::RoundToInt(NominalFps * 1000.0));
		const UINT32 FpsDen = 1000;

		TComPtr<IMFMediaType> OutType;
		Hr = MFCreateMediaType(&OutType);
		if (SUCCEEDED(Hr)) Hr = OutType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		if (SUCCEEDED(Hr)) Hr = OutType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
		if (SUCCEEDED(Hr)) Hr = OutType->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(BitrateKbps) * 1000u);
		if (SUCCEEDED(Hr)) Hr = OutType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		if (SUCCEEDED(Hr)) Hr = OutType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
		if (SUCCEEDED(Hr)) Hr = MFSetAttributeSize(OutType, MF_MT_FRAME_SIZE, OutputSize.X, OutputSize.Y);
		if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(OutType, MF_MT_FRAME_RATE, FpsNum, FpsDen);
		if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(OutType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

		DWORD StreamIdx = 0;
		if (SUCCEEDED(Hr)) Hr = NewWriter->AddStream(OutType, &StreamIdx);

		// Input: tightly packed top-down BGRA. MF_MT_DEFAULT_STRIDE positive
		// declares top-down explicitly (RGB32 defaults to bottom-up like a DIB).
		TComPtr<IMFMediaType> InType;
		if (SUCCEEDED(Hr)) Hr = MFCreateMediaType(&InType);
		if (SUCCEEDED(Hr)) Hr = InType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		if (SUCCEEDED(Hr)) Hr = InType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
		if (SUCCEEDED(Hr)) Hr = InType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		if (SUCCEEDED(Hr)) Hr = InType->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(InputSize.X * 4));
		if (SUCCEEDED(Hr)) Hr = MFSetAttributeSize(InType, MF_MT_FRAME_SIZE, InputSize.X, InputSize.Y);
		if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(InType, MF_MT_FRAME_RATE, FpsNum, FpsDen);
		if (SUCCEEDED(Hr)) Hr = MFSetAttributeRatio(InType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		if (SUCCEEDED(Hr)) Hr = NewWriter->SetInputMediaType(StreamIdx, InType, nullptr);

		// Audio stream: AAC-LC out, interleaved stereo PCM16 in (the listener
		// downmixes to stereo; the sink writer resamples if the device rate is
		// not a native AAC rate).
		DWORD AudioIdx = 0;
		if (SUCCEEDED(Hr) && bAudioEnabled)
		{
			TComPtr<IMFMediaType> AudioOut;
			Hr = MFCreateMediaType(&AudioOut);
			if (SUCCEEDED(Hr)) Hr = AudioOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
			if (SUCCEEDED(Hr)) Hr = AudioOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
			if (SUCCEEDED(Hr)) Hr = AudioOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
				(AudioSampleRate == 44100) ? 44100u : 48000u);
			if (SUCCEEDED(Hr)) Hr = AudioOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
			if (SUCCEEDED(Hr)) Hr = AudioOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
			if (SUCCEEDED(Hr)) Hr = AudioOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
				static_cast<UINT32>(AudioBitrateBytesPerSec));
			if (SUCCEEDED(Hr)) Hr = NewWriter->AddStream(AudioOut, &AudioIdx);

			TComPtr<IMFMediaType> AudioIn;
			if (SUCCEEDED(Hr)) Hr = MFCreateMediaType(&AudioIn);
			if (SUCCEEDED(Hr)) Hr = AudioIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
			if (SUCCEEDED(Hr)) Hr = AudioIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
			if (SUCCEEDED(Hr)) Hr = AudioIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
				static_cast<UINT32>(AudioSampleRate));
			if (SUCCEEDED(Hr)) Hr = AudioIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
			if (SUCCEEDED(Hr)) Hr = AudioIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
			if (SUCCEEDED(Hr)) Hr = AudioIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
			if (SUCCEEDED(Hr)) Hr = AudioIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
				static_cast<UINT32>(AudioSampleRate) * 4u);
			if (SUCCEEDED(Hr)) Hr = NewWriter->SetInputMediaType(AudioIdx, AudioIn, nullptr);
		}

		if (SUCCEEDED(Hr)) Hr = NewWriter->BeginWriting();

		if (FAILED(Hr))
		{
			Fail(FString::Printf(TEXT("MP4 writer setup failed (hr=0x%08x, hardware=%d)"),
				(uint32)Hr, bAllowHardware ? 1 : 0));
			return false;
		}

		// A failed hardware attempt is not fatal — clear so SW retry can proceed.
		bFailed = false;
		{
			FScopeLock Lock(&ErrorCS);
			Error.Reset();
		}
		StreamIndex = StreamIdx;
		AudioStreamIndex = AudioIdx;
		Writer = NewWriter;
		bHardware = bAllowHardware;
		return true;
	}

	void EncodeLoop()
	{
		const LONGLONG NominalDuration = static_cast<LONGLONG>(10'000'000.0 / NominalFps);
		for (;;)
		{
			// Feed the muxer in rough timestamp order across the two streams.
			TUniquePtr<FMCPVideoFrame>* VideoHead = Queue.Peek();
			TUniquePtr<FMCPAudioChunk>* AudioHead = bAudioEnabled ? AudioQueue.Peek() : nullptr;

			if (VideoHead &&
				(!AudioHead || (*VideoHead)->TimeSeconds <= (*AudioHead)->TimeSeconds))
			{
				TUniquePtr<FMCPVideoFrame> Frame;
				Queue.Dequeue(Frame);
				PendingCount.Decrement();
				if (!bFailed)
				{
					WriteFrame(*Frame, NominalDuration); // failure recorded; keep draining
				}
				continue;
			}
			if (AudioHead)
			{
				TUniquePtr<FMCPAudioChunk> Chunk;
				AudioQueue.Dequeue(Chunk);
				AudioPendingCount.Decrement();
				if (!bFailed)
				{
					WriteAudioChunk(*Chunk);
				}
				continue;
			}
			if (bFinishRequested)
			{
				break;
			}
			FPlatformProcess::Sleep(0.002f);
		}
	}

	bool WriteFrame(const FMCPVideoFrame& Frame, LONGLONG NominalDuration)
	{
		const DWORD ByteCount = static_cast<DWORD>(Frame.Bgra.Num());

		TComPtr<IMFMediaBuffer> Buffer;
		HRESULT Hr = MFCreateMemoryBuffer(ByteCount, &Buffer);
		if (SUCCEEDED(Hr))
		{
			BYTE* Dest = nullptr;
			Hr = Buffer->Lock(&Dest, nullptr, nullptr);
			if (SUCCEEDED(Hr))
			{
				FMemory::Memcpy(Dest, Frame.Bgra.GetData(), ByteCount);
				Buffer->Unlock();
				Hr = Buffer->SetCurrentLength(ByteCount);
			}
		}

		TComPtr<IMFSample> Sample;
		if (SUCCEEDED(Hr)) Hr = MFCreateSample(&Sample);
		if (SUCCEEDED(Hr)) Hr = Sample->AddBuffer(Buffer);
		if (SUCCEEDED(Hr)) Hr = Sample->SetSampleTime(static_cast<LONGLONG>(Frame.TimeSeconds * 10'000'000.0));
		if (SUCCEEDED(Hr)) Hr = Sample->SetSampleDuration(NominalDuration);
		if (SUCCEEDED(Hr)) Hr = Writer->WriteSample(StreamIndex, Sample);

		if (FAILED(Hr))
		{
			Fail(FString::Printf(TEXT("WriteSample failed (hr=0x%08x) after %d frames"),
				(uint32)Hr, FramesWritten.GetValue()));
			return false;
		}
		FramesWritten.Increment();
		LastTimestampS = Frame.TimeSeconds;
		return true;
	}

	bool WriteAudioChunk(const FMCPAudioChunk& Chunk)
	{
		const DWORD ByteCount = static_cast<DWORD>(Chunk.Pcm.Num() * sizeof(int16));
		if (ByteCount == 0)
		{
			return true;
		}

		TComPtr<IMFMediaBuffer> Buffer;
		HRESULT Hr = MFCreateMemoryBuffer(ByteCount, &Buffer);
		if (SUCCEEDED(Hr))
		{
			BYTE* Dest = nullptr;
			Hr = Buffer->Lock(&Dest, nullptr, nullptr);
			if (SUCCEEDED(Hr))
			{
				FMemory::Memcpy(Dest, Chunk.Pcm.GetData(), ByteCount);
				Buffer->Unlock();
				Hr = Buffer->SetCurrentLength(ByteCount);
			}
		}

		TComPtr<IMFSample> Sample;
		if (SUCCEEDED(Hr)) Hr = MFCreateSample(&Sample);
		if (SUCCEEDED(Hr)) Hr = Sample->AddBuffer(Buffer);
		if (SUCCEEDED(Hr)) Hr = Sample->SetSampleTime(static_cast<LONGLONG>(Chunk.TimeSeconds * 10'000'000.0));
		if (SUCCEEDED(Hr)) Hr = Sample->SetSampleDuration(
			static_cast<LONGLONG>(10'000'000.0 * Chunk.Frames / AudioSampleRate));
		if (SUCCEEDED(Hr)) Hr = Writer->WriteSample(AudioStreamIndex, Sample);

		if (FAILED(Hr))
		{
			Fail(FString::Printf(TEXT("Audio WriteSample failed (hr=0x%08x)"), (uint32)Hr));
			return false;
		}
		AudioChunksWritten.Increment();
		return true;
	}

	void DrainAndDiscard()
	{
		TUniquePtr<FMCPVideoFrame> Frame;
		while (Queue.Dequeue(Frame))
		{
			PendingCount.Decrement();
		}
		TUniquePtr<FMCPAudioChunk> Chunk;
		while (AudioQueue.Dequeue(Chunk))
		{
			AudioPendingCount.Decrement();
		}
	}

	const FString Path;
	const FIntPoint InputSize;
	FIntPoint OutputSize; // may be widened to InputSize if the resize is rejected
	const double NominalFps;
	const int32 BitrateKbps;
	const int32 AudioSampleRate; // 0 = audio never requested
	FThreadSafeBool bAudioEnabled = false; // cleared on video-only fallback

	TQueue<TUniquePtr<FMCPVideoFrame>, EQueueMode::Spsc> Queue;
	TQueue<TUniquePtr<FMCPAudioChunk>, EQueueMode::Spsc> AudioQueue;
	FThreadSafeCounter PendingCount;
	FThreadSafeCounter AudioPendingCount;
	FThreadSafeCounter FramesWritten;
	FThreadSafeCounter AudioChunksWritten;
	double LastTimestampS = 0.0; // encoder thread only; read after join

	TUniquePtr<FRunnableThread> Thread;
	FEvent* ReadyEvent = nullptr;
	FThreadSafeBool bFinishRequested = false;
	FThreadSafeBool bFailed = false;
	FThreadSafeBool bDone = false;
	bool bHardware = false;

	mutable FCriticalSection ErrorCS;
	FString Error;

	TComPtr<IMFSinkWriter> Writer;
	DWORD StreamIndex = 0;
	DWORD AudioStreamIndex = 0;

public:
	bool UsedHardwarePath() const { return bHardware; }
	/** Effective encoded frame size (post any native-res fallback). Only stable
	 *  once WaitUntilReady has returned. */
	FIntPoint GetOutputSize() const { return OutputSize; }
	/** True when the MP4 carries an AAC track (post any video-only fallback).
	 *  Only stable once WaitUntilReady has returned. */
	bool IsAudioEnabled() const { return bAudioEnabled; }
	int32 GetAudioChunksWritten() const { return AudioChunksWritten.GetValue(); }
};

// ---------------------------------------------------------------------------
// Submix capture — taps the PIE audio device's MAIN submix (the final mix the
// player hears, post-effects, pre-hardware) on the audio render thread.
//
// Sync model: video and audio share one timeline (t=0 = recording start, wall
// clock). The first callback anchors the audio timeline to that clock; every
// later chunk is stamped by ACCUMULATED SAMPLE COUNT from the anchor — gapless
// and drift-free within the stream, aligned to the video's frame timestamps.
// ---------------------------------------------------------------------------

class FMCPSubmixCapture : public ISubmixBufferListener
{
public:
	/** Game thread, before registration. Worker must outlive Deactivate(). */
	void Activate(FMCPVideoEncoderWorker* InWorker, double InRecStartTimeS)
	{
		FScopeLock Lock(&AccessCS);
		Worker = InWorker;
		RecStartTimeS = InRecStartTimeS;
		AnchorTimeS = -1.0;
		FramesAccum = 0;
	}

	/** Game thread, before the worker is torn down. Fences in-flight callbacks. */
	void Deactivate()
	{
		FScopeLock Lock(&AccessCS);
		Worker = nullptr;
	}

	int32 GetDroppedChunks() const { return DroppedChunks.GetValue(); }

	// ISubmixBufferListener — audio render thread.
	virtual void OnNewSubmixBuffer(
		const USoundSubmix* /*OwningSubmix*/, float* AudioData, int32 NumSamples,
		int32 NumChannels, const int32 SampleRate, double /*AudioClock*/) override
	{
		FScopeLock Lock(&AccessCS);
		if (!Worker || NumChannels <= 0 || NumSamples <= 0 || SampleRate <= 0)
		{
			return;
		}
		if (AnchorTimeS < 0.0)
		{
			AnchorTimeS = FPlatformTime::Seconds() - RecStartTimeS;
		}

		const int32 Frames = NumSamples / NumChannels;
		TUniquePtr<FMCPAudioChunk> Chunk = MakeUnique<FMCPAudioChunk>();
		Chunk->Frames = Frames;
		Chunk->TimeSeconds = AnchorTimeS + static_cast<double>(FramesAccum) / SampleRate;
		Chunk->Pcm.SetNumUninitialized(Frames * 2);

		// Downmix to interleaved stereo PCM16. UE surround order:
		// FL, FR, C, LFE, then surround pairs (L/R alternating).
		for (int32 F = 0; F < Frames; ++F)
		{
			const float* In = AudioData + static_cast<int64>(F) * NumChannels;
			float L, R;
			if (NumChannels == 1)
			{
				L = R = In[0];
			}
			else if (NumChannels == 2)
			{
				L = In[0];
				R = In[1];
			}
			else
			{
				L = In[0];
				R = In[1];
				if (NumChannels > 2) { L += 0.707f * In[2]; R += 0.707f * In[2]; }
				for (int32 C = 4; C < NumChannels; ++C) // skip LFE (index 3)
				{
					if ((C & 1) == 0) { L += 0.707f * In[C]; } else { R += 0.707f * In[C]; }
				}
			}
			Chunk->Pcm[F * 2] = static_cast<int16>(FMath::Clamp(L, -1.0f, 1.0f) * 32767.0f);
			Chunk->Pcm[F * 2 + 1] = static_cast<int16>(FMath::Clamp(R, -1.0f, 1.0f) * 32767.0f);
		}

		// Advance the sample clock even when a chunk is dropped, so timestamps
		// stay wall-aligned (a drop is a momentary gap, never creeping desync).
		FramesAccum += Frames;
		if (!Worker->EnqueueAudio(MoveTemp(Chunk)))
		{
			DroppedChunks.Increment();
		}
	}

	virtual const FString& GetListenerName() const override
	{
		static const FString Name = TEXT("MCPVideoRecorder");
		return Name;
	}

private:
	mutable FCriticalSection AccessCS;
	FMCPVideoEncoderWorker* Worker = nullptr;
	double RecStartTimeS = 0.0;
	double AnchorTimeS = -1.0;
	int64 FramesAccum = 0;
	FThreadSafeCounter DroppedChunks;
};

// ---------------------------------------------------------------------------
// Frame payload — bridges FFrameGrabber's render-thread resolve to the recorder.
// ---------------------------------------------------------------------------

namespace
{

struct FMCPVideoFramePayload : IFramePayload
{
	FMCPVideoRecorder* Recorder = nullptr;
	double TimeSeconds = 0.0;

	virtual bool OnFrameReady_RenderThread(
		FColor* ColorBuffer, FIntPoint BufferSize, FIntPoint TargetSize) const override
	{
		Recorder->OnFrameResolved_RenderThread(ColorBuffer, BufferSize, TargetSize, TimeSeconds);
		return false; // consumed — don't accumulate in GetCapturedFrames()
	}
};

/** The PIE session's scene viewport: floating-window PIE exposes it directly;
 *  docked-in-level-viewport PIE goes through the destination asset viewport. */
TSharedPtr<FSceneViewport> FindPieSceneViewport()
{
	if (!GEditor)
	{
		return nullptr;
	}
	for (TPair<FName, FSlatePlayInEditorInfo>& Pair : GEditor->SlatePlayInEditorMap)
	{
		if (Pair.Value.SlatePlayInEditorWindowViewport.IsValid())
		{
			return Pair.Value.SlatePlayInEditorWindowViewport;
		}
		if (TSharedPtr<IAssetViewport> Dest = Pair.Value.DestinationSlateViewport.Pin())
		{
			return Dest->GetSharedActiveViewport();
		}
	}
	return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// FMCPVideoRecorder
// ---------------------------------------------------------------------------

// The editor mutes ALL game audio when the app loses focus: the platform layer
// flips FApp::VolumeMultiplier to FApp::UnfocusedVolumeMultiplier (default 0),
// and the mixer applies it PER-SOURCE (AudioMixerSource.cpp), upstream of the
// submix our listener taps — an unfocused editor mixes genuine silence. While
// recording we force both multipliers to 1 so an alt-tab (or an unattended
// agent run) never punches a silent hole in the track; restored on stop.
// File-scope (not a member) to keep the class Live-Coding-compatible.
static float GMCPSavedUnfocusedVolumeMultiplier = -1.0f; // < 0 = not saved

static void MCPForceAudibleWhileRecording()
{
	GMCPSavedUnfocusedVolumeMultiplier = FApp::GetUnfocusedVolumeMultiplier();
	FApp::SetUnfocusedVolumeMultiplier(1.0f);
	FApp::SetVolumeMultiplier(1.0f); // covers "already unfocused right now"
}

static void MCPRestoreUnfocusedMute()
{
	if (GMCPSavedUnfocusedVolumeMultiplier >= 0.0f)
	{
		FApp::SetUnfocusedVolumeMultiplier(GMCPSavedUnfocusedVolumeMultiplier);
		GMCPSavedUnfocusedVolumeMultiplier = -1.0f;
		// FApp::VolumeMultiplier stays at 1 until the next focus transition,
		// when the platform layer reapplies the right value — harmless.
	}
}

FMCPVideoRecorder& FMCPVideoRecorder::Get()
{
	static FMCPVideoRecorder Singleton;
	return Singleton;
}

TSharedPtr<FJsonObject> FMCPVideoRecorder::Start(const FMCPVideoRecorderConfig& InConfig)
{
	check(IsInGameThread());

	if (bRecording)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("A recording is already active (recording_id=%s)."), *RecordingId),
			EMCPErrorCode::EngineBusy,
			TEXT("One recording at a time. Call pie_record_stop first (or wait for the max_duration_s watchdog)."));
	}
	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Video capture requires a real RHI — this editor renders no frames (-nullrhi)."),
			EMCPErrorCode::FeatureDisabled,
			TEXT("Launch the editor with a GUI (or -RenderOffscreen), not -nullrhi; under nullrhi there are no frames to record."));
	}
	if (!GEditor || !GEditor->IsPlaySessionInProgress())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No PIE session is running — recording captures the live PIE viewport."),
			EMCPErrorCode::NotInPie,
			TEXT("Start PIE first (pie_start), then pie_record_start."));
	}

	TSharedPtr<FSceneViewport> Viewport = FindPieSceneViewport();
	if (!Viewport.IsValid())
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("Could not resolve the PIE scene viewport."),
			EMCPErrorCode::Internal,
			TEXT("PIE reports running but no floating-window or docked PIE viewport was found; retry once the session has fully started."));
	}

	const FIntPoint ViewportSize(Viewport->GetSize().X, Viewport->GetSize().Y);
	if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("PIE viewport has zero size — nothing to record yet."),
			EMCPErrorCode::EngineBusy,
			TEXT("The PIE viewport has not presented a frame yet; retry in a moment."));
	}

	// Grab at NATIVE viewport resolution (evened for H.264/NV12) — FFrameGrabber's
	// resolve pass draws the capture rect normalized against the capture-rect
	// size, so a smaller readback target leaves the image in the top-left with
	// black margins. The encoder's video processor does the downscale instead.
	const FIntPoint NewCaptureSize(
		FMath::Max(2, (ViewportSize.X / 2) * 2),
		FMath::Max(2, (ViewportSize.Y / 2) * 2));

	// Encoded size: fit inside the requested box, preserve aspect, round to even.
	const double Scale = FMath::Min(1.0, FMath::Min(
		static_cast<double>(InConfig.MaxWidth) / NewCaptureSize.X,
		static_cast<double>(InConfig.MaxHeight) / NewCaptureSize.Y));
	FIntPoint NewEncodeSize(
		FMath::Max(2, (static_cast<int32>(NewCaptureSize.X * Scale) / 2) * 2),
		FMath::Max(2, (static_cast<int32>(NewCaptureSize.Y * Scale) / 2) * 2));

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(InConfig.OutputPath), true);
	// A stale file at the target path would make bytes/duration reporting lie.
	if (IFileManager::Get().FileExists(*InConfig.OutputPath))
	{
		IFileManager::Get().Delete(*InConfig.OutputPath);
	}

	// Resolve the PIE world's audio device up front (the encoder needs the mix
	// sample rate before it opens the sink writer). Missing device → video-only.
	FAudioDeviceHandle NewAudioHandle;
	int32 NewAudioRate = 0;
	FString NewAudioNote;
	if (InConfig.bAudio)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE && Ctx.World())
			{
				NewAudioHandle = Ctx.World()->GetAudioDevice();
				if (NewAudioHandle.IsValid())
				{
					break;
				}
			}
		}
		if (NewAudioHandle.IsValid() && NewAudioHandle.GetAudioDevice())
		{
			NewAudioRate = FMath::RoundToInt32(NewAudioHandle.GetAudioDevice()->GetSampleRate());
		}
		else
		{
			NewAudioNote = TEXT("PIE world has no audio device (editor running -nosound?) — recording video-only.");
		}
	}
	else
	{
		NewAudioNote = TEXT("audio disabled by request");
	}

	const double NominalFps = InConfig.CaptureFps > 0.0 ? InConfig.CaptureFps : 60.0;
	TUniquePtr<FMCPVideoEncoderWorker> NewWorker = MakeUnique<FMCPVideoEncoderWorker>(
		InConfig.OutputPath, NewCaptureSize, NewEncodeSize, NominalFps, InConfig.BitrateKbps,
		NewAudioRate);
	if (!NewWorker->StartThread() || !NewWorker->WaitUntilReady(5.0))
	{
		const FString Why = NewWorker->GetError();
		NewWorker->StopThread(2.0);
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to initialize the MP4 encoder: %s"),
				Why.IsEmpty() ? TEXT("timed out waiting for the encoder thread") : *Why),
			EMCPErrorCode::Internal,
			TEXT("Media Foundation could not open an H.264 sink writer (both hardware and software paths)."));
	}

	Config = InConfig;
	CaptureSize = NewCaptureSize;
	EncodeSize = NewWorker->GetOutputSize(); // post any native-res fallback
	SourceSize = ViewportSize;
	Worker = MoveTemp(NewWorker);
	RecordingId = FGuid::NewGuid().ToString(EGuidFormats::Short);
	FramesCaptured.Reset();
	FramesDropped.Reset();
	LastStopReason.Reset();

	Grabber = MakeUnique<FFrameGrabber>(Viewport.ToSharedRef(), CaptureSize);
	Grabber->StartCapturingFrames();

	StartTimeS = FPlatformTime::Seconds();
	LastCaptureTimeS = 0.0; // capture the very first ticked frame immediately
	bAcceptFrames = true;
	bRecording = true;

	// Audio tap: main submix of the PIE audio device — the exact mix the player
	// hears. Registered as close to StartTimeS as possible; the listener anchors
	// its sample clock to the shared recording timeline at its first callback.
	AudioSampleRate = 0;
	AudioNote = NewAudioNote;
	if (NewAudioRate > 0 && Worker->IsAudioEnabled() && NewAudioHandle.IsValid())
	{
		MCPForceAudibleWhileRecording();
		AudioCapture = MakeShared<FMCPSubmixCapture, ESPMode::ThreadSafe>();
		AudioCapture->Activate(Worker.Get(), StartTimeS);
		FAudioDevice* Device = NewAudioHandle.GetAudioDevice();
		Device->RegisterSubmixBufferListener(
			AudioCapture.ToSharedRef(), Device->GetMainSubmixObject());
		AudioDeviceHandle = NewAudioHandle;
		AudioSampleRate = NewAudioRate;
	}
	else if (NewAudioRate > 0 && !Worker->IsAudioEnabled())
	{
		AudioNote = TEXT("AAC audio stream unavailable on this system — recording video-only.");
	}

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FMCPVideoRecorder::TickGameThread));
	PrePIEEndedHandle = FEditorDelegates::PrePIEEnded.AddRaw(
		this, &FMCPVideoRecorder::OnPrePIEEnded);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("recording_id"), RecordingId);
	Result->SetStringField(TEXT("path"), Config.OutputPath);
	Result->SetNumberField(TEXT("width"), EncodeSize.X);
	Result->SetNumberField(TEXT("height"), EncodeSize.Y);
	Result->SetNumberField(TEXT("capture_width"), CaptureSize.X);
	Result->SetNumberField(TEXT("capture_height"), CaptureSize.Y);
	Result->SetNumberField(TEXT("source_width"), SourceSize.X);
	Result->SetNumberField(TEXT("source_height"), SourceSize.Y);
	Result->SetNumberField(TEXT("capture_fps"), Config.CaptureFps);
	Result->SetNumberField(TEXT("bitrate_kbps"), Config.BitrateKbps);
	Result->SetNumberField(TEXT("max_duration_s"), Config.MaxDurationS);
	Result->SetStringField(TEXT("encoder"),
		Worker->UsedHardwarePath() ? TEXT("media_foundation_hw") : TEXT("media_foundation_sw"));
	Result->SetStringField(TEXT("codec"), TEXT("h264"));
	Result->SetBoolField(TEXT("audio"), AudioSampleRate > 0);
	if (AudioSampleRate > 0)
	{
		Result->SetNumberField(TEXT("audio_sample_rate"), AudioSampleRate);
		Result->SetStringField(TEXT("audio_codec"), TEXT("aac"));
		Result->SetNumberField(TEXT("audio_bitrate_kbps"), AudioBitrateBytesPerSec * 8 / 1000);
	}
	else if (!AudioNote.IsEmpty())
	{
		Result->SetStringField(TEXT("audio_note"), AudioNote);
	}
	return Result;
}

TSharedPtr<FJsonObject> FMCPVideoRecorder::Stop(const FString& Reason)
{
	check(IsInGameThread());

	if (!bRecording)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			TEXT("No recording is in progress."),
			EMCPErrorCode::InvalidArgument,
			TEXT("pie_record_stop only applies to an active recording — start one with pie_record_start."));
	}

	bAcceptFrames = false;
	bRecording = false;
	LastStopReason = Reason;

	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	TickerHandle.Reset();
	FEditorDelegates::PrePIEEnded.Remove(PrePIEEndedHandle);
	PrePIEEndedHandle.Reset();

	// Audio first: fence the listener (Deactivate blocks on any in-flight
	// callback) and unregister BEFORE the encoder drains, so no chunk can
	// arrive against a dying worker. The device handle we hold keeps the
	// audio device alive for the unregister even during PIE teardown.
	int32 AudioDropped = 0;
	if (AudioCapture.IsValid())
	{
		MCPRestoreUnfocusedMute();
		AudioCapture->Deactivate();
		AudioDropped = AudioCapture->GetDroppedChunks();
		if (AudioDeviceHandle.IsValid() && AudioDeviceHandle.GetAudioDevice())
		{
			FAudioDevice* Device = AudioDeviceHandle.GetAudioDevice();
			Device->UnregisterSubmixBufferListener(
				AudioCapture.ToSharedRef(), Device->GetMainSubmixObject());
		}
		AudioCapture.Reset();
		AudioDeviceHandle = FAudioDeviceHandle();
	}

	if (Grabber)
	{
		Grabber->StopCapturingFrames();
		FlushRenderingCommands(); // run in-flight resolves so their payloads land
		Grabber->Shutdown();
		Grabber.Reset();
	}

	bool bEncoderFinished = true;
	if (Worker)
	{
		Worker->RequestFinish();
		bEncoderFinished = Worker->StopThread(15.0);
	}

	const int64 FileBytes = IFileManager::Get().FileSize(*Config.OutputPath);
	const uint64 FramesWritten = Worker ? Worker->GetFramesWritten() : 0;
	const double DurationS = Worker ? Worker->GetLastTimestampS() : 0.0;
	const bool bFailed = !Worker || Worker->HasFailed() || !bEncoderFinished;
	const FString EncoderError = Worker ? Worker->GetError() : FString();
	Worker.Reset();

	if (bFailed)
	{
		return FMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Recording stopped but the MP4 could not be finalized: %s"),
				EncoderError.IsEmpty() ? TEXT("encoder did not finish in time") : *EncoderError),
			EMCPErrorCode::Internal,
			FString::Printf(TEXT("Partial file (if any) at '%s'. frames_captured=%d dropped=%d."),
				*Config.OutputPath, FramesCaptured.GetValue(), FramesDropped.GetValue()));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("recording_id"), RecordingId);
	Result->SetStringField(TEXT("path"), Config.OutputPath);
	Result->SetNumberField(TEXT("width"), EncodeSize.X);
	Result->SetNumberField(TEXT("height"), EncodeSize.Y);
	Result->SetNumberField(TEXT("capture_fps"), Config.CaptureFps);
	Result->SetNumberField(TEXT("frames_encoded"), static_cast<double>(FramesWritten));
	Result->SetNumberField(TEXT("frames_dropped"), FramesDropped.GetValue());
	Result->SetNumberField(TEXT("duration_s"), DurationS);
	Result->SetNumberField(TEXT("bytes"), static_cast<double>(FMath::Max<int64>(0, FileBytes)));
	Result->SetStringField(TEXT("codec"), TEXT("h264"));
	Result->SetStringField(TEXT("container"), TEXT("mp4"));
	Result->SetBoolField(TEXT("audio"), AudioSampleRate > 0);
	if (AudioSampleRate > 0)
	{
		Result->SetNumberField(TEXT("audio_sample_rate"), AudioSampleRate);
		Result->SetNumberField(TEXT("audio_chunks_dropped"), AudioDropped);
	}
	else if (!AudioNote.IsEmpty())
	{
		Result->SetStringField(TEXT("audio_note"), AudioNote);
	}
	Result->SetStringField(TEXT("stop_reason"), Reason);
	return Result;
}

TSharedPtr<FJsonObject> FMCPVideoRecorder::Status() const
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const bool bActive = bRecording;
	Result->SetBoolField(TEXT("recording"), bActive);
	if (bActive)
	{
		Result->SetStringField(TEXT("recording_id"), RecordingId);
		Result->SetStringField(TEXT("path"), Config.OutputPath);
		Result->SetNumberField(TEXT("elapsed_s"), FPlatformTime::Seconds() - StartTimeS);
		Result->SetNumberField(TEXT("max_duration_s"), Config.MaxDurationS);
		Result->SetNumberField(TEXT("frames_captured"), FramesCaptured.GetValue());
		Result->SetNumberField(TEXT("frames_encoded"),
			Worker ? static_cast<double>(Worker->GetFramesWritten()) : 0.0);
		Result->SetNumberField(TEXT("frames_dropped"), FramesDropped.GetValue());
		Result->SetNumberField(TEXT("width"), EncodeSize.X);
		Result->SetNumberField(TEXT("height"), EncodeSize.Y);
		Result->SetNumberField(TEXT("capture_fps"), Config.CaptureFps);
		Result->SetBoolField(TEXT("audio"), AudioSampleRate > 0);
		if (AudioSampleRate > 0 && Worker)
		{
			Result->SetNumberField(TEXT("audio_chunks_encoded"), Worker->GetAudioChunksWritten());
		}
		else if (!AudioNote.IsEmpty())
		{
			Result->SetStringField(TEXT("audio_note"), AudioNote);
		}
		if (Worker && Worker->HasFailed())
		{
			Result->SetStringField(TEXT("encoder_error"), Worker->GetError());
		}
	}
	else if (!LastStopReason.IsEmpty())
	{
		Result->SetStringField(TEXT("last_stop_reason"), LastStopReason);
		Result->SetStringField(TEXT("last_path"), Config.OutputPath);
	}
	Result->SetBoolField(TEXT("armed"), bArmed);
	if (bArmed)
	{
		Result->SetStringField(TEXT("armed_base_name"), ArmedBaseName);
		Result->SetNumberField(TEXT("armed_takes_recorded"), ArmedTakesRecorded);
		Result->SetStringField(TEXT("next_take_path"), NextArmedTakePath());
	}
	return Result;
}

void FMCPVideoRecorder::OnFrameResolved_RenderThread(
	const FColor* Pixels, FIntPoint BufferSize, FIntPoint TargetSize, double TimeSeconds)
{
	if (!bAcceptFrames || !Worker.IsValid() || !Pixels)
	{
		return;
	}

	// BufferSize is the mapped staging surface (width may include row padding);
	// TargetSize is the encode size. Repack tightly, top-down.
	const int32 CopyW = FMath::Min(TargetSize.X, BufferSize.X);
	const int32 CopyH = FMath::Min(TargetSize.Y, BufferSize.Y);

	TUniquePtr<FMCPVideoFrame> Frame = MakeUnique<FMCPVideoFrame>();
	Frame->TimeSeconds = TimeSeconds;
	Frame->Bgra.SetNumZeroed(static_cast<int64>(TargetSize.X) * TargetSize.Y * 4);
	for (int32 Row = 0; Row < CopyH; ++Row)
	{
		FMemory::Memcpy(
			Frame->Bgra.GetData() + static_cast<int64>(Row) * TargetSize.X * 4,
			Pixels + static_cast<int64>(Row) * BufferSize.X,
			static_cast<SIZE_T>(CopyW) * 4);
	}

	if (Worker->EnqueueFrame(MoveTemp(Frame)))
	{
		FramesCaptured.Increment();
	}
	else
	{
		FramesDropped.Increment();
	}
}

bool FMCPVideoRecorder::TickGameThread(float /*DeltaSeconds*/)
{
	if (!bRecording)
	{
		return true;
	}

	const double Now = FPlatformTime::Seconds();

	// Watchdog: never record unbounded.
	if (Now - StartTimeS >= Config.MaxDurationS)
	{
		Stop(TEXT("max_duration"));
		return true;
	}
	// Belt and braces alongside PrePIEEnded — PIE gone means nothing to record.
	if (!GEditor || !GEditor->IsPlaySessionInProgress())
	{
		Stop(TEXT("pie_ended"));
		return true;
	}
	// Encoder died mid-recording (disk full, MFT failure): finalize what we have.
	if (Worker && Worker->HasFailed())
	{
		Stop(TEXT("encoder_error"));
		return true;
	}

	const double Interval = Config.CaptureFps > 0.0 ? 1.0 / Config.CaptureFps : 0.0;
	if (Interval > 0.0)
	{
		if (Now - LastCaptureTimeS < Interval)
		{
			return true;
		}
		// Advance by whole intervals (no drift), but never build up a burst debt.
		LastCaptureTimeS = FMath::Max(LastCaptureTimeS + Interval, Now - Interval);
	}

	TSharedPtr<FMCPVideoFramePayload, ESPMode::ThreadSafe> Payload =
		MakeShared<FMCPVideoFramePayload, ESPMode::ThreadSafe>();
	Payload->Recorder = this;
	Payload->TimeSeconds = Now - StartTimeS;
	Grabber->CaptureThisFrame(Payload);

	return true;
}

void FMCPVideoRecorder::OnPrePIEEnded(bool /*bIsSimulating*/)
{
	if (bRecording)
	{
		Stop(TEXT("pie_ended"));
	}
}

// ---------------------------------------------------------------------------
// Armed auto-record — every PIE session records itself from its first frames.
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> FMCPVideoRecorder::Arm(
	const FMCPVideoRecorderConfig& InConfig, const FString& InDirectory, const FString& InBaseName)
{
	check(IsInGameThread());

	ArmedConfig = InConfig;
	ArmedDirectory = InDirectory;
	ArmedBaseName = InBaseName;
	if (!bArmed)
	{
		PostPIEStartedHandle = FEditorDelegates::PostPIEStarted.AddRaw(
			this, &FMCPVideoRecorder::OnPostPIEStarted);
		bArmed = true;
		ArmedTakesRecorded = 0;
	}

	// Mid-session arm: latch onto the session that is already running.
	const bool bPieRunning = GEditor && GEditor->IsPlaySessionInProgress();
	if (bPieRunning && !bRecording)
	{
		OnPostPIEStarted(false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("armed"), true);
	Result->SetStringField(TEXT("base_name"), ArmedBaseName);
	Result->SetStringField(TEXT("directory"), ArmedDirectory);
	Result->SetStringField(TEXT("next_take_path"), NextArmedTakePath());
	Result->SetBoolField(TEXT("recording_now"), bRecording);
	Result->SetNumberField(TEXT("capture_fps"), ArmedConfig.CaptureFps);
	Result->SetNumberField(TEXT("max_width"), ArmedConfig.MaxWidth);
	Result->SetNumberField(TEXT("max_height"), ArmedConfig.MaxHeight);
	Result->SetNumberField(TEXT("bitrate_kbps"), ArmedConfig.BitrateKbps);
	Result->SetNumberField(TEXT("max_duration_s"), ArmedConfig.MaxDurationS);
	Result->SetBoolField(TEXT("audio"), ArmedConfig.bAudio);
	return Result;
}

TSharedPtr<FJsonObject> FMCPVideoRecorder::Disarm()
{
	check(IsInGameThread());

	const bool bWasArmed = bArmed;
	if (bArmed)
	{
		FEditorDelegates::PostPIEStarted.Remove(PostPIEStartedHandle);
		PostPIEStartedHandle.Reset();
		bArmed = false;
	}
	if (ArmRetryTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ArmRetryTickerHandle);
		ArmRetryTickerHandle.Reset();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("armed"), false);
	Result->SetBoolField(TEXT("was_armed"), bWasArmed);
	Result->SetNumberField(TEXT("takes_recorded"), ArmedTakesRecorded);
	// A live recording is deliberately left running (auto-finalizes on PIE
	// end / watchdog, or pie_record_stop).
	Result->SetBoolField(TEXT("recording_now"), bRecording);
	return Result;
}

FString FMCPVideoRecorder::NextArmedTakePath() const
{
	for (int32 Index = 1; Index < 1000; ++Index)
	{
		const FString Candidate = FPaths::Combine(ArmedDirectory,
			FString::Printf(TEXT("%s_%02d.mp4"), *ArmedBaseName, Index));
		if (!IFileManager::Get().FileExists(*Candidate))
		{
			return Candidate;
		}
	}
	return FPaths::Combine(ArmedDirectory, ArmedBaseName + TEXT("_overflow.mp4"));
}

bool FMCPVideoRecorder::TryStartArmedTake()
{
	FMCPVideoRecorderConfig TakeConfig = ArmedConfig;
	TakeConfig.OutputPath = NextArmedTakePath();
	TSharedPtr<FJsonObject> StartResult = Start(TakeConfig);

	const bool bStarted = StartResult.IsValid() && !StartResult->HasField(TEXT("error"));
	if (bStarted)
	{
		++ArmedTakesRecorded;
		UE_LOG(LogTemp, Log, TEXT("MCPVideoRecorder: armed auto-record started take '%s'"),
			*TakeConfig.OutputPath);
	}
	return bStarted;
}

void FMCPVideoRecorder::OnPostPIEStarted(bool /*bIsSimulating*/)
{
	if (!bArmed || bRecording)
	{
		return;
	}
	// The PIE world is up but the viewport may not have presented yet — try
	// now, and retry every tick for a bounded window until Start succeeds.
	if (TryStartArmedTake())
	{
		return;
	}
	ArmRetryDeadlineS = FPlatformTime::Seconds() + 10.0;
	if (!ArmRetryTickerHandle.IsValid())
	{
		ArmRetryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FMCPVideoRecorder::TickArmedStart), 0.25f);
	}
}

bool FMCPVideoRecorder::TickArmedStart(float /*DeltaSeconds*/)
{
	const bool bGiveUp = FPlatformTime::Seconds() > ArmRetryDeadlineS;
	const bool bPieUp = GEditor && GEditor->IsPlaySessionInProgress();
	if (!bArmed || bRecording || !bPieUp || bGiveUp || TryStartArmedTake())
	{
		if (bGiveUp && bArmed && !bRecording && bPieUp)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("MCPVideoRecorder: armed auto-record could not start within 10s of PIE start."));
		}
		ArmRetryTickerHandle.Reset();
		return false; // one-shot ticker: returning false unregisters it
	}
	return true; // keep retrying
}

#endif // PLATFORM_WINDOWS
