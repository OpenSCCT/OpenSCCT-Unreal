#include "Play/ScctAudioSubsystem.h"
#include "Demo/ScctDemoPlayback.h"

#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Decoders/VorbisAudioInfo.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Menu/ScctUserSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Engine/GameInstance.h"
#include "Online/ScctMapFolders.h"
#include "OpenSCCT.h"
#include "Play/ScctAudioMix.h"
#include "Play/ScctAudioOutput.h"
#include "Play/ScctPawnProxy.h"
#include "Play/ScctSessionSubsystem.h"
#include "Play/ScctSoundWave.h"
#include "Runtime/ScctFeatures.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/ReverbEffect.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"

namespace
{
	/** The settings' volume of a voice's kind (Menu/ScctUserSettings.h); the master volume is the engine's own. */
	float AudioSettingsGain(bool bMusic, bool bAmbient, bool bObjective)
	{
		const UScctUserSettings* Settings = UScctUserSettings::Get();
		return Settings == nullptr ? 1.0f : Settings->GetGain(bMusic ? EScctSoundKind::Music : bObjective ? EScctSoundKind::Objective : bAmbient ? EScctSoundKind::Ambient : EScctSoundKind::Effects);
	}
}

float UScctAudioSubsystem::SettingsGainOf(const FVoice& Voice)
{
	return AudioSettingsGain(Voice.bMusic, Voice.AmbientId != 0 && !Voice.bEquipmentLoop, Voice.Category == static_cast<uint8>(EScctAudioCategory::Objective));
}

namespace
{
	// The original mixes 32 voices; with a modern mixer there is room for more before the slot rule applies.
	constexpr int32 MaxEventVoices = 48;
	constexpr int32 MaxComponents = 128;

	// Decoded sound kept in memory; the files of a map in play come to 20 to 60 MB.
	constexpr int64 CacheBudgetBytes = 160ll * 1024 * 1024;
	constexpr double CacheIdleSeconds = 180.0;

	// The original's reach of a sound is 100 x its radius: the bridge has already tested a call against it (with
	// the call's radius, as APlayerController::HearSound does) before handing it over, and the ambient sources
	// are listed by it. A voice that plays is not cut off there: its gain only keeps falling with distance, as
	// in the Godot client (SoundRules.DistanceGain).
	constexpr float ReachPerRadius = 100.0f;

	// How far past that reach a 3D voice's attenuation goes on (FindOrCreateAttenuation), as a multiple of it.
	constexpr float HeardPastReach = 10.0f;

	// How much of the local player's own sounds (his shots, steps, breath: the AtListener voices) goes to the
	// room's reverb. They play without 3D, so no distance model sends them there; a gun that rings in the room for
	// everyone else rang dry for the one who fired it.
	constexpr float OwnSoundReverbSend = 0.3f;
	constexpr float ZeroRadiusMinDistance = 10.0f;

	// A flat ambient source falls silent below this volume (the original's rule 22).
	constexpr float AmbientSilence = 0.05f;
	constexpr float AmbientFadeSeconds = 0.4f;

	// Ambient voices are matched by actor key (3D), by the sound named (flat, a zone: zones naming one sound share a
	// voice) or by actor key with a bit of its own (flat, no zone: Play/ScctAudioAmbientLoops.cpp).

	const FName ZoneReverbTag(TEXT("ScctZone"));

	/** Millibels (the unit of the EAX level parameters) to a linear gain. */
	float FromMillibels(double Millibels)
	{
		return FMath::Pow(10.0f, static_cast<float>(Millibels) / 2000.0f);
	}
}

bool UScctAudioSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UScctAudioSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Session = Collection.InitializeDependency<UScctSessionSubsystem>();
	if (Session == nullptr)
	{
		return;
	}

	Session->OnSessionChanged.AddUObject(this, &UScctAudioSubsystem::HandleSessionChanged);
	Session->OnMatchStarted.AddUObject(this, &UScctAudioSubsystem::HandleMatchStarted);
	Session->OnFrame.AddUObject(this, &UScctAudioSubsystem::HandleFrame);

	bLogVoices = FParse::Param(FCommandLine::Get(), TEXT("ScctAudioLog"));
	FParse::Value(FCommandLine::Get(), TEXT("ScctAudioSummary="), SummaryEvery);
	FParse::Value(FCommandLine::Get(), TEXT("ScctAudioAmbientAfter="), AmbientAfterSeconds);
	SoundBuffer.SetNumUninitialized(256);
	AmbientBuffer.SetNumUninitialized(512);
	Voices.Reserve(MaxComponents);

	// The Vorbis decoder is used from worker tasks; its libraries are loaded here, once, on the game thread.
	LoadVorbisLibraries();

	// The room a zone without an environment of its own gets: short and quiet, so 3D sounds sit in a space
	// without any place sounding like a hall.
	DefaultReverb = NewObject<UReverbEffect>(this);
	DefaultReverb->DecayTime = 0.9f;
	DefaultReverb->DecayHFRatio = 0.6f;
	DefaultReverb->Gain = 0.22f;
	DefaultReverb->GainHF = 0.6f;
	DefaultReverb->ReflectionsGain = 0.12f;
	DefaultReverb->ReflectionsDelay = 0.012f;
	DefaultReverb->LateGain = 0.9f;
	DefaultReverb->LateDelay = 0.02f;
	DefaultReverb->Density = 1.0f;
	DefaultReverb->Diffusion = 0.85f;
}

void UScctAudioSubsystem::Deinitialize()
{
	StopAllVoices();
	ReleaseRoomFields();
	Cache.Reset();
	if (Session != nullptr)
	{
		Session->OnSessionChanged.RemoveAll(this);
		Session->OnMatchStarted.RemoveAll(this);
		Session->OnFrame.RemoveAll(this);
	}
	Super::Deinitialize();
}

UScctAudioSubsystem::FOnSummary& UScctAudioSubsystem::OnSummary()
{
	static FOnSummary Delegate;
	return Delegate;
}

float UScctAudioSubsystem::DistanceGain(float Distance, float MinDistance, float Radius)
{
	return Distance <= MinDistance ? 1.0f : MinDistance / (MinDistance + 0.5f * (Distance - MinDistance));
}

// ---------------------------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------------------------

void UScctAudioSubsystem::HandleSessionChanged(int32 OldId, int32 NewId)
{
	// Call ids start again at 1 and actor ids are reused in the next match: nothing may carry over.
	if (OldId != 0)
	{
		LogSummary();
	}
	StopAllVoices();
	Zones.Reset();
	ZoneEffects.Reset();
	ListenerZone = INDEX_NONE;
	ActiveZone = -2;
	ReleaseRoomFields();
	ProbeCalls.Reset();
	bLastListenerSet = false;
	FMemory::Memzero(NumStartedByCategory);
	NumStarted =NumStopped = NumOutOfReach = NumRefused = NumStolen = NumFailedFiles = NumObjectiveSkipped = PeakVoices = 0;
	NumHrtfStarted = NumPannedStarted = 0;
	bOutputModeHrtf = UScctAudioOutputSubsystem::UseHrtf();
	// Segment keys and clocks belong to one match (Play/ScctAudioSequences.cpp).
	SequenceSegments.Reset();
	SequenceClock = 0.0f;
	NumSequenceStarted = NumSequenceSeeks = NumSequenceStopped = NumSequenceEnded = NumSequenceNoFile = PeakSequenceSegments = 0;
}

void UScctAudioSubsystem::HandleMatchStarted()
{
	FString Error;
	Events = ScctFeature<FScctEventsApi>(&Error);
	ContentRoot = FOpenSCCTModule::FindContentRoot();
	bHasDevice = GetWorld()->GetAudioDeviceRaw() != nullptr;
	SummaryTimer = SummaryEvery;
	const UGameInstance* GameInstance = GetWorld()->GetGameInstance();
	Mix = GameInstance != nullptr ? GameInstance->GetSubsystem<UScctAudioMix>() : nullptr;
	if (Mix != nullptr)
	{
		// The world's audio device may be newer than the game: it gets the categories and the gains now.
		Mix->EnsureApplied();
	}
	if (Events == nullptr)
	{
		UE_LOG(LogOpenSCCT, Error, TEXT("Audio: no sound this match, the bridge's events are missing: %s"), *Error);
		return;
	}

	// Harmless when the bridge already started the feature with the session.
	Events->Begin();

	if (Session->IsBackdrop())
	{
		// The match behind the main menu is not heard: the menu has its own music. Its sound records are still
		// taken off the bridge's queue every frame (HandleFrame), so the queue does not grow.
		UE_LOG(LogOpenSCCT, Log, TEXT("Audio: the menu's backdrop match on %s is silent"), Session->GetSessionInfo().Map);
		return;
	}

	FString AcousticError;
	Acoustics = ScctFeature<FScctAcousticApi>(&AcousticError);
	UE_CLOG(Acoustics == nullptr, LogOpenSCCT, Warning, TEXT("Audio: walls do not change sound this match, the bridge's acoustic table is missing: %s"), *AcousticError);
	ReadAcousticSwitches();
	ReadAudioViewSwitches();

	FString Song;
	if (FParse::Value(FCommandLine::Get(), TEXT("ScctAudioMusic="), Song, false))
	{
		FString At;
		CheckSongIn = 3.0f;
		if (Song.Split(TEXT("@"), &Song, &At))
		{
			CheckSongIn = FMath::Max(FCString::Atof(*At), 0.0f);
		}
		CheckSong = Song;
	}
	LoadZones(Session->GetSessionInfo().Map);
	ReadRoomSwitches();
	CreateRoomFields();
	ReadSequenceSwitches();
	const FAudioDevice* Device = GetWorld()->GetAudioDeviceRaw();
	UE_LOG(LogOpenSCCT, Log, TEXT("Audio: ready for %s (%s, %d zones with reverb, content %s)"), Session->GetSessionInfo().Map,
		Device != nullptr ? *FString::Printf(TEXT("audio device present, %d engine voices"), Device->GetMaxChannels())
			: TEXT("no audio device: voices are tracked but not heard"), Zones.Num(), *ContentRoot);
	if (Device != nullptr && Device->GetMaxChannels() < MaxEventVoices + 16)
	{
		// Config/DefaultEngine.ini sets the engine's limit above ours; with less, the engine drops voices by its
		// own rule before the original's slot rule here is ever asked.
		UE_LOG(LogOpenSCCT, Warning, TEXT("Audio: the engine mixes only %d voices at once (AudioSettings QualityLevels MaxChannels); this game keeps up to %d event voices plus ambient loops"),
			Device->GetMaxChannels(), MaxEventVoices);
	}
}

void UScctAudioSubsystem::HandleFrame(float DeltaSeconds)
{
	if (Events == nullptr)
	{
		return;
	}
	if (Session->IsBackdrop())
	{
		while (Events->DrainSounds(SoundBuffer.GetData(), SoundBuffer.Num()) >= SoundBuffer.Num())
		{
		}
		return;
	}

	const float Rate = UScctDemoPlayback::PresentationRate(GetWorld());
	if ((Rate == 0.0f) != (PlaybackRate == 0.0f))
	{
		for (const FVoice& Voice : Voices)
			if (auto* Component = Voice.Component.Get()) Component->SetPaused(Rate == 0.0f);
	}
	PlaybackRate = Rate;
	if (Rate == 0.0f) return;
	DeltaSeconds *= Rate;

	// The listener is the camera, which Unreal's audio follows by itself (the player's view target); the same
	// point is used here for reach, the flat ambient rules and the log.
	const FScctBridgeView& View = Session->GetView();
	bListenerValid = View.Valid != 0;
	Listener = FVector(View.X, View.Y, View.Z);
	// -ScctAudioListener holds it at a forced place (Play/ScctAudioRooms.cpp).
	UpdateForcedListener(DeltaSeconds);
	// The scope's audio zoom and the listener's velocity (Play/ScctAudioView.cpp).
	UpdateAudioView(DeltaSeconds);

	if (CheckSongIn >= 0.0f)
	{
		CheckSongIn -= DeltaSeconds;
		if (CheckSongIn < 0.0f)
		{
			const FScctAudioCheckApi* Check = ScctFeature<FScctAudioCheckApi>();
			const bool bPlays = Check != nullptr && Check->PlayMusic(*CheckSong, 0.0f) != 0;
			UE_LOG(LogOpenSCCT, Log, TEXT("ScctAudio: -ScctAudioMusic: '%s' %s"), *CheckSong,
				bPlays ? TEXT("started as the music") : Check == nullptr ? TEXT("not started: the bridge has no audio check table") : TEXT("not started: unknown sound or no file"));
		}
	}

	UpdateAudioProbe(DeltaSeconds);
	DrainSounds();
	UpdateAmbient();
	// The animation sequences' audio segments (Play/ScctAudioSequences.cpp).
	UpdateSequenceAudio(DeltaSeconds);
	UpdateAcoustics(DeltaSeconds);
	UpdateVoices(DeltaSeconds);
	// The listener's zone and every voice's source room (Play/ScctAudioRooms.cpp), then the main reverb of that zone.
	UpdateRooms(DeltaSeconds);
	UpdateReverb();

	TrimTimer -= DeltaSeconds;
	if (TrimTimer <= 0.0f)
	{
		TrimTimer = 2.0f;
		Cache.Trim(CacheBudgetBytes, CacheIdleSeconds,
			[this](int32 FileId)
			{
				for (const FVoice& Voice : Voices)
				{
					if (Voice.FileId == FileId)
					{
						return true;
					}
				}
				return false;
			},
			[this](int32 FileId)
			{
				Waves.Remove(static_cast<int64>(FileId) * 2);
				Waves.Remove(static_cast<int64>(FileId) * 2 + 1);
			});
	}

	PeakVoices = FMath::Max(PeakVoices, Voices.Num());
	if (SummaryEvery > 0.0f)
	{
		SummaryTimer -= DeltaSeconds;
		if (SummaryTimer <= 0.0f)
		{
			SummaryTimer = SummaryEvery;
			LogSummary();
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Sound records
// ---------------------------------------------------------------------------------------------------------------

void UScctAudioSubsystem::DrainSounds()
{
	for (;;)
	{
		const int32 Count = Events->DrainSounds(SoundBuffer.GetData(), SoundBuffer.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FScctSound& Record = SoundBuffer[Index];
			switch (Record.Op)
			{
			case EScctSoundOp::Play:
				Play(Record);
				break;
			case EScctSoundOp::Stop:
				Stop(Record);
				break;
			case EScctSoundOp::PlayMusic:
				PlayMusic(Record);
				break;
			case EScctSoundOp::StopMusic:
				for (FVoice& Voice : Voices)
				{
					if (Voice.bMusic)
					{
						FadeOutVoice(Voice, Record.FadeSeconds, TEXT("music stopped"));
					}
				}
				break;
			case EScctSoundOp::StopCall:
				for (FVoice& Voice : Voices)
				{
					if (Voice.CallId == Record.CallId && Voice.AmbientId == 0)
					{
						FadeOutVoice(Voice, 0.05f, TEXT("call stopped"));
					}
				}
				break;
			default:
				break;
			}
		}
		if (Count < SoundBuffer.Num())
		{
			return;
		}
	}
}

void UScctAudioSubsystem::Play(const FScctSound& Record)
{
	if (Record.FileId == 0)
	{
		return;
	}

	const bool bObjective = (Record.Flags & EScctSoundFlags::Objective) != 0;
	if (bObjective && Mix != nullptr && !Mix->AreObjectiveSoundsOn())
	{
		// The original's profile switch bObjectiveSounds, which Godot turns off at objective volume 0: the jingle is
		// dropped, not played silently.
		++NumObjectiveSkipped;
		UE_LOG(LogOpenSCCT, Log, TEXT("ScctAudio: objective jingle '%s' skipped (objective volume 0)"), *GetName(Record.SoundId));
		return;
	}

	const bool bAtListener = (Record.Flags & EScctSoundFlags::AtListener) != 0;
	const bool bPositional = !bAtListener && (Record.Flags & EScctSoundFlags::Positional) != 0;
	const float MinDistance = Record.MinDistance > 0.0f ? Record.MinDistance : ZeroRadiusMinDistance;
	const FVector Location(Record.X, Record.Y, Record.Z);
	if (bPositional && bListenerValid && (Record.Flags & EScctSoundFlags::Loop) == 0 && Record.Radius > 0.0f
		&& FVector::Dist(Location, Listener) >= ReachPerRadius * Record.Radius)
	{
		// The bridge passes on what the original would start (its reach test uses the radius of the call, and the
		// server's own calls are not tested again), and the Godot client plays every such call however small the
		// file's own radius is: so does this, quietly. It is only counted. The voice limit (MakeRoom) is what
		// keeps far, faint one-shots from crowding out near ones: the quietest voice goes first.
		++NumOutOfReach;
		UE_CLOG(bLogVoices, LogOpenSCCT, Log, TEXT("Audio beyond 100 x its radius, played all the same: '%s' actor %d, %.0f cm from the listener, 100 x radius is %.0f"), *GetName(Record.FileId),
			Record.ActorId, FVector::Dist(Location, Listener), ReachPerRadius * Record.Radius);
	}
	if (!MakeRoom(Record))
	{
		++NumRefused;
		return;
	}

	FVoice Voice;
	Voice.CallId = Record.CallId;
	Voice.Segment = Record.Segment;
	Voice.ActorId = Record.ActorId;
	Voice.FollowActorId = Record.FollowActorId;
	Voice.FollowBoneId = Record.FollowBoneId;
	Voice.SoundId = Record.SoundId;
	Voice.LeafId = Record.LeafId;
	Voice.FileId = Record.FileId;
	Voice.Slot = Record.Slot;
	Voice.Location = Location;
	Voice.Volume = FMath::Clamp(Record.Volume, 0.0f, 1.0f);
	Voice.Radius = Record.Radius;
	Voice.MinDistance = MinDistance;
	Voice.Pitch = FMath::Clamp(Record.Pitch, 0.5f, 2.0f);
	Voice.LoopStart = Record.LoopStart;
	Voice.LoopEnd = Record.LoopEnd;
	Voice.StartIn = FMath::Max(Record.Delay, 0.0f);
	Voice.StartOffset = Record.SeekOffset;
	Voice.bPositional = bPositional;
	Voice.bAtListener = bAtListener;
	Voice.bLoop = (Record.Flags & EScctSoundFlags::Loop) != 0;
	Voice.bNoOverride = (Record.Flags & EScctSoundFlags::NoOverride) != 0;
	Voice.Category = static_cast<uint8>(bObjective ? EScctAudioCategory::Objective : EScctAudioCategory::World);
	// A timed loop of a sequence ends by the clock; any other loop runs until it is stopped.
	Voice.LifeLeft = Record.PlaySeconds > 0.0f ? Record.PlaySeconds : -1.0f;
	Voice.bProbe = ProbeCalls.Contains(Record.CallId);
	Voices.Add(Voice);

	// Start the decode now, so a delayed entry of a sequence is ready when its time comes.
	FScctPcmPtr Pcm;
	Cache.Request(Record.FileId, GetFilePath(Record.FileId), Pcm);
}

bool UScctAudioSubsystem::MakeRoom(const FScctSound& Record)
{
	int32 EventVoices = 0;
	for (const FVoice& Voice : Voices)
	{
		// An animation sequence's segments have players of their own (Godot's AnimationSequenceAudio): they neither count
		// toward the event voices nor are taken by a new call, which would end their segment for good.
		EventVoices += Voice.AmbientId == 0 && Voice.SequenceKey == 0 && !Voice.bMusic && Voice.State != EVoiceState::Fading ? 1 : 0;
	}
	if (EventVoices < MaxEventVoices)
	{
		return true;
	}

	// The original's rule when no voice is free: the new call replaces the voice of the same actor and slot
	// (slot 0 never matches), unless it is a NoOverride call, which is then not played; otherwise the quietest
	// voice at the listener goes.
	FVoice* Victim = nullptr;
	if (Record.Slot != 0)
	{
		for (FVoice& Voice : Voices)
		{
			if (Voice.AmbientId == 0 && Voice.SequenceKey == 0 && !Voice.bMusic && Voice.State != EVoiceState::Fading && Voice.ActorId == Record.ActorId && Voice.Slot == Record.Slot)
			{
				if ((Record.Flags & EScctSoundFlags::NoOverride) != 0)
				{
					return false;
				}
				Victim = &Voice;
				break;
			}
		}
	}
	if (Victim == nullptr)
	{
		float Quietest = MAX_flt;
		for (FVoice& Voice : Voices)
		{
			if (Voice.AmbientId != 0 || Voice.SequenceKey != 0 || Voice.bMusic || Voice.State == EVoiceState::Fading)
			{
				continue;
			}
			const float Heard = Voice.Volume * (Voice.bPositional ? DistanceGain(FVector::Dist(Voice.Location, Listener), Voice.MinDistance, Voice.Radius) : 1.0f);
			if (Heard < Quietest)
			{
				Quietest = Heard;
				Victim = &Voice;
			}
		}
	}
	if (Victim == nullptr)
	{
		return false;
	}

	++NumStolen;
	FadeOutVoice(*Victim, 0.03f, TEXT("voice taken by a new call"));
	return true;
}

void UScctAudioSubsystem::Stop(const FScctSound& Record)
{
	const bool bAllSounds = (Record.Flags & EScctSoundFlags::AllSounds) != 0;
	const bool bLocalPawnVoices = (Record.Flags & EScctSoundFlags::LocalPawnVoices) != 0;
	for (FVoice& Voice : Voices)
	{
		// Ambient loops and sequence segments are state: they stop when their source or segment leaves its list.
		if (Voice.AmbientId != 0 || Voice.SequenceKey != 0 || Voice.State == EVoiceState::Fading)
		{
			continue;
		}
		const bool bOwner = Record.ActorId == 0
			? (!bAllSounds || !Voice.bMusic)
			: (Voice.ActorId == Record.ActorId || (bLocalPawnVoices && Voice.bAtListener));
		if (bOwner && (bAllSounds || Voice.SoundId == Record.SoundId || Voice.LeafId == Record.SoundId))
		{
			FadeOutVoice(Voice, 0.05f, TEXT("StopSound"));
		}
	}
}

void UScctAudioSubsystem::PlayMusic(const FScctSound& Record)
{
	for (const FVoice& Voice : Voices)
	{
		if (Voice.bMusic && Voice.State != EVoiceState::Fading && Voice.SeekFadeOut <= 0.0f && Voice.SoundId == Record.SoundId)
		{
			// The track asked for already plays.
			return;
		}
	}
	for (FVoice& Voice : Voices)
	{
		if (Voice.bMusic && Voice.SeekFadeOut <= 0.0f)
		{
			FadeOutVoice(Voice, FMath::Max(Record.FadeSeconds, 2.0f), TEXT("music replaced"));
		}
	}
	if (Record.FileId == 0)
	{
		return;
	}

	FVoice Voice;
	Voice.SoundId = Record.SoundId;
	Voice.LeafId = Record.LeafId;
	Voice.FileId = Record.FileId;
	Voice.Volume = Record.Volume > 0.0f ? FMath::Min(Record.Volume, 1.0f) : 1.0f;
	Voice.Pitch = Record.Pitch > 0.0f ? FMath::Clamp(Record.Pitch, 0.5f, 2.0f) : 1.0f;
	Voice.LoopStart = Record.LoopStart;
	Voice.LoopEnd = Record.LoopEnd;
	Voice.FadeIn = Record.FadeSeconds;
	Voice.StartOffset = Record.SeekOffset;
	Voice.SeekFadeOut = Record.SeekFadeOut;
	Voice.bLoop = (Record.Flags & EScctSoundFlags::Loop) != 0;
	Voice.bMusic = true;
	Voice.Category = static_cast<uint8>(EScctAudioCategory::Music);
	Voice.LifeLeft = -1.0f;
	Voices.Add(Voice);

	FScctPcmPtr Pcm;
	Cache.Request(Record.FileId, GetFilePath(Record.FileId), Pcm);
}

// ---------------------------------------------------------------------------------------------------------------
// Ambient sources
// ---------------------------------------------------------------------------------------------------------------

void UScctAudioSubsystem::UpdateAmbient()
{
	for (FVoice& Voice : Voices)
	{
		Voice.bSeen = false;
	}
	ListenerZone = INDEX_NONE;

	const bool bHeld = AmbientAfterSeconds > 0.0f && GetWorld()->GetTimeSeconds() < AmbientAfterSeconds;
	const int32 Count = bListenerValid && !bHeld ? Events->GetAmbientSounds(AmbientBuffer.GetData(), AmbientBuffer.Num()) : 0;
	AmbientCount = BeginFlatLoopPass(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FScctAmbientSound& Source = AmbientBuffer[Index];
		const FVector Location(Source.X, Source.Y, Source.Z);
		const bool bZone = (Source.Flags & EScctAmbientFlags::Zone) != 0;
		const bool bListenerInZone = bZone && (Source.Flags & EScctAmbientFlags::ListenerInZone) != 0;
		if (bListenerInZone)
		{
			// The zone the listener stands in also picks the reverb; the audio table knows zones by their place.
			for (int32 Zone = 0; Zone < Zones.Num(); ++Zone)
			{
				if (Zones[Zone].Location.Equals(Location, 1.0))
				{
					ListenerZone = Zone;
					break;
				}
			}
		}
		if (Source.FileId == 0 || Source.Volume <= 0.0f)
		{
			continue;
		}

		const bool bPositional = (Source.Flags & EScctAmbientFlags::Positional) != 0;
		const float Radius = Source.Radius;
		const float Distance = FVector::Dist(Location, Listener);
		float Volume = Source.Volume;
		int64 Id = bPositional ? static_cast<int64>(Source.Key) : (FlatZoneLoopBit | static_cast<uint32>(Source.SoundId));
		if (bPositional)
		{
			if (Radius > 0.0f && Distance >= ReachPerRadius * Radius)
			{
				continue;
			}
		}
		else if (bZone)
		{
			if (!bListenerInZone)
			{
				continue;
			}
		}
		else
		{
			// A flat source that is no zone holds a voice of its own, full inside its radius and falling off beyond
			// it; the voices of several speakers of one sound add up. Kept, handed over or silent: nothing to make.
			if (!UpdateFlatActorLoop(Source, Distance, Id, Volume))
			{
				continue;
			}
		}

		// The zones that name one sound share a flat voice, matched by the sound and not by the file: a random list is
		// picked per source, and two sources with different picks must not take the voice from each other every frame.
		// The voice keeps the file it started with.
		if (const int32* Found = AmbientVoices.Find(Id))
		{
			FVoice& Voice = Voices[*Found];
			if ((bPositional ? Voice.FileId == Source.FileId : Voice.SoundId == Source.SoundId) && Voice.State != EVoiceState::Fading)
			{
				// Several zones of one sound share the voice: it plays as loud as the loudest of them.
				Voice.Volume = Voice.bSeen ? FMath::Max(Voice.Volume, Volume) : Volume;
				if (bPositional)
				{
					Voice.Location = Location;
				}
				Voice.bSeen = true;
				continue;
			}
			// The source now plays another file (or its voice is on its way out): it starts afresh.
			FadeOutVoice(Voice, AmbientFadeSeconds, TEXT("ambient source changed"));
			Voice.AmbientId = 0;
			AmbientVoices.Remove(Id);
		}
		else if (bZone && HandFlatLoopToZone(Source, Id))
		{
			// The loop an actor played carries on as the zone's.
			continue;
		}

		FVoice Voice;
		Voice.AmbientId = Id;
		Voice.bEquipmentLoop = (Source.Flags & EScctAmbientFlags::Equipment) != 0;
		Voice.AmbientPass = AmbientPass;
		Voice.ActorId = Source.Key;
		Voice.SoundId = Source.SoundId;
		Voice.LeafId = Source.SoundId;
		Voice.FileId = Source.FileId;
		Voice.Location = Location;
		Voice.Volume = Volume;
		Voice.Radius = Radius;
		Voice.MinDistance = Radius > 0.0f ? Radius : ZeroRadiusMinDistance;
		Voice.Pitch = FMath::Clamp(Source.Pitch, 0.5f, 2.0f);
		Voice.FadeIn = AmbientFadeSeconds;
		Voice.RepeatInterval = Source.RepeatInterval;
		Voice.bPositional = bPositional;
		Voice.bLoop = (Source.Flags & EScctAmbientFlags::Loop) != 0;
		Voice.LifeLeft = -1.0f;
		Voice.bSeen = true;
		AmbientVoices.Add(Id, Voices.Add(Voice));
	}

	// A source that is no longer listed, in reach or in the listener's zone fades out.
	for (auto It = AmbientVoices.CreateIterator(); It; ++It)
	{
		FVoice& Voice = Voices[It.Value()];
		if (!Voice.bSeen)
		{
			FadeOutVoice(Voice, AmbientFadeSeconds, TEXT("ambient source gone or out of reach"));
			Voice.AmbientId = 0;
			It.RemoveCurrent();
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------------------------------------------

void UScctAudioSubsystem::UpdateVoices(float DeltaSeconds)
{
	// Speakers or Headphones changed (Play/ScctAudioOutput.h): the engine cannot change a playing voice's
	// spatialization, so the looping 3D voices (machines, ambient emitters), which would otherwise keep the old mode
	// for as long as they play, are started again with the new one. One-shots end by themselves within seconds.
	const bool bHrtfNow = UScctAudioOutputSubsystem::UseHrtf();
	if (bHrtfNow != bOutputModeHrtf)
	{
		bOutputModeHrtf = bHrtfNow;
		int32 Restarted = 0;
		for (FVoice& Voice : Voices)
		{
			UAudioComponent* Component = Voice.Component.Get();
			if (Voice.State != EVoiceState::Playing || !Voice.bPositional || !Voice.bLoop || Voice.SequenceKey != 0 || Component == nullptr)
			{
				continue;
			}
			Component->AttenuationSettings = FindOrCreateAttenuation(Voice.MinDistance, Voice.Radius);
			// The new engine sound has none of the old one's sends: the room routing puts them on again next frame.
			Component->bOverrideAttenuation = false;
			Voice.AppliedRoomMix = 0.0f;
			Voice.AppliedRoomSend = 0.0f;
			Voice.AppliedRoomField = INDEX_NONE;
			Component->Play();
			++Restarted;
		}
		UE_LOG(LogOpenSCCT, Log, TEXT("ScctAudio: output mode now %s: %d looping 3D voices started again with it"), bHrtfNow ? TEXT("HRTF (Headphones)") : TEXT("panning (Speakers)"), Restarted);
	}

	for (auto It = Voices.CreateIterator(); It; ++It)
	{
		FVoice& Voice = *It;
		bool bRemove = false;
		switch (Voice.State)
		{
		case EVoiceState::Waiting:
		{
			Voice.StartIn -= DeltaSeconds;
			FScctPcmPtr Pcm;
			const FScctSoundCache::EState FileState = Cache.Request(Voice.FileId, GetFilePath(Voice.FileId), Pcm);
			if (FileState == FScctSoundCache::EState::Failed)
			{
				++NumFailedFiles;
				if (Voice.AmbientId != 0)
				{
					// The source is listed again next frame: its voice stays, silent, so it is not made, counted
					// and dropped every frame for as long as the source is audible.
					Voice.State = EVoiceState::Dormant;
					Voice.RepeatInterval = 0.0f;
				}
				else
				{
					bRemove = true;
				}
			}
			else if (FileState == FScctSoundCache::EState::Ready && Voice.StartIn <= 0.0f)
			{
				bRemove = !StartVoice(Voice, Pcm);
			}
			break;
		}

		case EVoiceState::Playing:
			if (Voice.LifeLeft >= 0.0f)
			{
				Voice.LifeLeft -= DeltaSeconds;
				if (Voice.LifeLeft < 0.0f)
				{
					if (Voice.AmbientId != 0)
					{
						// An ambient one-shot stays known while its source is audible, so it does not start again
						// every frame; it plays again after its repeat interval if it has one.
						if (UAudioComponent* Component = Voice.Component.Get())
						{
							Component->Stop();
						}
						Voice.State = EVoiceState::Dormant;
						Voice.RepeatLeft = Voice.RepeatInterval;
					}
					else
					{
						if (bLogVoices)
						{
							LogVoice(TEXT("ended"), Voice);
						}
						bRemove = true;
					}
					break;
				}
			}
			if (Voice.bPositional)
			{
				Voice.Location = FindVoiceLocation(Voice);
			}
			// The scope's audio zoom on its volume and the Doppler shift on its pitch (Play/ScctAudioView.cpp).
			UpdateZoomAndDoppler(Voice, DeltaSeconds);
			if (UAudioComponent* Component = Voice.Component.Get())
			{
				if (Voice.bPositional)
				{
					// Heard from where its sound reaches the listener (an opening when a wall is in the way).
					Component->SetWorldLocation(Voice.Location + Voice.AcousticOffset);
				}
				// The ambience follows the map's volumes, every voice the settings' volume of its kind (SettingsGainOf),
				// a 3D voice its sound through walls and the audio zoom, and its pitch the Doppler shift: ApplyAcoustics
				// sets them.
				ApplyAcoustics(Voice, false);
			}
			break;

		case EVoiceState::Dormant:
			if (Voice.RepeatInterval > 0.0f)
			{
				Voice.RepeatLeft -= DeltaSeconds;
				if (Voice.RepeatLeft <= 0.0f)
				{
					Voice.State = EVoiceState::Waiting;
					Voice.StartIn = 0.0f;
					Voice.FadeIn = 0.0f;
				}
			}
			break;

		case EVoiceState::Fading:
			Voice.FadeLeft -= DeltaSeconds;
			bRemove = Voice.FadeLeft <= 0.0f;
			break;
		}

		if (bRemove)
		{
			ReleaseVoice(Voice);
			It.RemoveCurrent();
		}
	}
}

bool UScctAudioSubsystem::StartVoice(FVoice& Voice, const FScctPcmPtr& Pcm)
{
	if (!Pcm.IsValid() || Pcm->NumFrames() == 0)
	{
		return false;
	}

	if (Voice.bPositional)
	{
		Voice.Location = FindVoiceLocation(Voice);
	}
	// An animation sequence's segment starts where its clock is now, part-way into its file (Play/ScctAudioSequences.cpp).
	if (Voice.SequenceKey != 0 && !PrepareSequenceStart(Voice, Pcm->Seconds()))
	{
		return false;
	}
	if (!Voice.bLoop)
	{
		if (Voice.StartOffset >= Pcm->Seconds()) return false;
		// The voice ends by the clock (the engine's voice ends by itself at the same moment); a little margin
		// lets the last block play out.
		Voice.LifeLeft = (Pcm->Seconds() - Voice.StartOffset) / Voice.Pitch + 0.1f;
	}
	Voice.State = EVoiceState::Playing;
	// Its first frame has no movement yet: only the listener's own counts toward the Doppler shift.
	Voice.bHasLastLocation = false;
	UpdateZoomAndDoppler(Voice, 0.0f);
	Voice.AppliedVolume = HeardVolumeOf(Voice);
	Voice.AppliedPitch = Voice.Pitch * Voice.DopplerPitch * PlaybackRate;
	++NumStarted;
	++NumStartedByCategory[Voice.Category & 7];

	if (bHasDevice)
	{
		UScctSoundWave* Wave = FindOrCreateWave(Voice, Pcm);
		// An ambient one-shot that plays again still holds the component of its last play.
		UAudioComponent* Component = Voice.Component.IsValid() ? Voice.Component.Get() : AcquireComponent();
		if (Wave != nullptr && Component != nullptr)
		{
			Component->SetSound(Wave);
			Component->bAllowSpatialization = Voice.bPositional;
			// A 3D voice has its distance model, which also sends it to the reverb. The local player's own sounds
			// play flat but are in the room all the same; music and the interface's flat sounds are not.
			Component->AttenuationSettings = Voice.bPositional ? FindOrCreateAttenuation(Voice.MinDistance, Voice.Radius)
				: (Voice.bAtListener && !Voice.bMusic ? FindOrCreateOwnSoundSettings() : nullptr);
			if (Voice.bPositional && Component->AttenuationSettings != nullptr)
			{
				const bool bHrtf = Component->AttenuationSettings->Attenuation.SpatializationAlgorithm == ESoundSpatializationAlgorithm::SPATIALIZATION_HRTF;
				int32& Count = bHrtf ? NumHrtfStarted : NumPannedStarted;
				UE_CLOG(Count == 0, LogOpenSCCT, Log, TEXT("ScctAudio: first 3D voice %s ('%s')"),
					bHrtf ? TEXT("created with HRTF (Headphones)") : TEXT("created with panning (Speakers)"), *GetName(Voice.FileId));
				++Count;
			}
			// A reused component may still carry the last voice's room routing (Play/ScctAudioRooms.cpp): this voice starts
			// on its attenuation's own reverb send and is routed again on the next frame.
			Component->bOverrideAttenuation = false;
			Voice.AppliedRoomMix = 0.0f;
			Voice.AppliedRoomSend = 0.0f;
			Voice.AppliedRoomField = INDEX_NONE;
			Component->bIsMusic = Voice.bMusic;
			// The category's class routes the voice to its submix (the reverb, the peaks); the player's volume of its
			// kind is in AppliedVolume (Menu/ScctUserSettings.h), so the submix itself is not turned down again.
			Component->SoundClassOverride = Mix != nullptr ? Mix->GetClass(static_cast<EScctAudioCategory>(Voice.Category)) : nullptr;
			Component->SetVolumeMultiplier(Voice.AppliedVolume);
			Component->SetPitchMultiplier(Voice.AppliedPitch);
			Component->SetWorldLocation(Voice.Location + Voice.AcousticOffset);
			// A reused component keeps the filter of its last voice: every voice sets its own. Before Play the
			// component's own properties are what the new engine voice starts with (the setters reach only a playing one).
			Voice.AppliedLowPass = Voice.AcousticLowPass;
			Component->bEnableLowPassFilter = Voice.AcousticLowPass < 19999.0f;
			Component->LowPassFilterFrequency = Voice.AcousticLowPass;
			if (Voice.FadeIn > 0.0f)
			{
				Component->FadeIn(Voice.FadeIn, 1.0f, Voice.StartOffset);
			}
			else
			{
				Component->Play(Voice.StartOffset);
			}
			Voice.Component = Component;
		}
	}

	if (Voice.SeekFadeOut > 0.0f) FadeOutVoice(Voice, Voice.SeekFadeOut, TEXT("demo music fade"));
	if (bLogVoices)
	{
		LogVoice(TEXT("play"), Voice, *FString::Printf(TEXT(" %d Hz %d ch %.2f s"), Pcm->SampleRate, Pcm->Channels, Pcm->Seconds()));
	}
	if (Voice.bMusic || Voice.Category == static_cast<uint8>(EScctAudioCategory::Objective))
	{
		// Rare and the ones a player turns down most: said in the log with the class and the gain they play at.
		const EScctAudioCategory Category = static_cast<EScctAudioCategory>(Voice.Category);
		const UAudioComponent* Component = Voice.Component.Get();
		UE_LOG(LogOpenSCCT, Log, TEXT("ScctAudio: %s '%s' starts in class %s at category gain %.2f x master %.2f"), Voice.bMusic ? TEXT("music") : TEXT("objective jingle"),
			*GetName(Voice.FileId), Component != nullptr && Component->SoundClassOverride != nullptr ? *Component->SoundClassOverride->GetName() : UScctAudioMix::CategoryName(Category),
			Mix != nullptr ? Mix->GetGain(Category) : 1.0f, Mix != nullptr ? Mix->GetMasterGain() : 1.0f);
	}
	return true;
}

void UScctAudioSubsystem::FadeOutVoice(FVoice& Voice, float Seconds, const TCHAR* Why)
{
	if (Voice.State == EVoiceState::Fading)
	{
		return;
	}

	const bool bSounding = Voice.State == EVoiceState::Playing;
	if (bLogVoices && (bSounding || Voice.State == EVoiceState::Waiting))
	{
		LogVoice(TEXT("stop"), Voice, *FString::Printf(TEXT(" (%s, fade %.2f s)"), Why, Seconds));
	}
	++NumStopped;

	UAudioComponent* Component = Voice.Component.Get();
	if (Component != nullptr && bSounding && Seconds > 0.0f)
	{
		Component->FadeOut(Seconds, 0.0f);
	}
	Voice.State = EVoiceState::Fading;
	Voice.FadeLeft = bSounding ? FMath::Max(Seconds, 0.0f) : 0.0f;
}

void UScctAudioSubsystem::ReleaseVoice(FVoice& Voice)
{
	if (Voice.AmbientId != 0)
	{
		AmbientVoices.Remove(Voice.AmbientId);
	}
	if (UAudioComponent* Component = Voice.Component.Get())
	{
		Component->Stop();
		FreeComponents.Add(Component);
	}
	Voice.Component.Reset();
}

void UScctAudioSubsystem::StopAllVoices()
{
	for (FVoice& Voice : Voices)
	{
		ReleaseVoice(Voice);
	}
	Voices.Reset();
	AmbientVoices.Reset();
}

FVector UScctAudioSubsystem::FindVoiceLocation(const FVoice& Voice) const
{
	if (Voice.FollowActorId == 0)
	{
		return Voice.Location;
	}

	if (const AScctPawnProxy* Proxy = Session->FindProxy(Voice.FollowActorId))
	{
		// A pawn: the drawn body, and the drawn bone when the call names one (a voice from the head, steps from a foot).
		if (Voice.FollowBoneId != 0 && Proxy->HasModel())
		{
			const FName Bone = FindDrawnBone(*Proxy, Voice.FollowBoneId);
			if (!Bone.IsNone())
			{
				return Proxy->GetBodyMesh()->GetSocketLocation(Bone);
			}
		}
		const FScctBridgePawn& Pawn = Proxy->GetPawn();
		return FVector(Pawn.X, Pawn.Y, Pawn.Z);
	}

	float Location[3];
	if (Events->GetActorLocation(Voice.FollowActorId, Location) != 0)
	{
		return FVector(Location[0], Location[1], Location[2]);
	}
	// The actor is gone: the sound stays where it last was.
	return Voice.Location;
}

// ---------------------------------------------------------------------------------------------------------------
// Unreal objects
// ---------------------------------------------------------------------------------------------------------------

UScctSoundWave* UScctAudioSubsystem::FindOrCreateWave(const FVoice& Voice, const FScctPcmPtr& Pcm)
{
	TObjectPtr<UScctSoundWave>& Wave = Waves.FindOrAdd(static_cast<int64>(Voice.FileId) * 2 + (Voice.bLoop ? 1 : 0));
	if (Wave == nullptr)
	{
		Wave = NewObject<UScctSoundWave>(this);
		Wave->Init(Pcm, Voice.bLoop, Voice.LoopStart, Voice.LoopEnd);
	}
	return Wave;
}

USoundAttenuation* UScctAudioSubsystem::FindOrCreateAttenuation(float MinDistance, float Radius)
{
	// Whole centimetres are fine enough, and keep the number of distinct settings small.
	const uint32 MinKey = static_cast<uint32>(FMath::RoundToInt(FMath::Max(MinDistance, 1.0f)));
	const uint32 RadiusKey = static_cast<uint32>(FMath::RoundToInt(FMath::Max(Radius, 0.0f)));
	// Headphones renders 3D voices by HRTF through the spatialization plugin (Play/ScctAudioOutput.h), Speakers pans
	// them. Each has its own settings: a voice already playing keeps the ones it started with, as the engine cannot
	// change a playing voice's spatialization.
	const bool bHrtf = UScctAudioOutputSubsystem::UseHrtf();
	const uint64 SettingsKey = (static_cast<uint64>(MinKey & 0x7fffffffu) << 32) | RadiusKey | (bHrtf ? (1ull << 63) : 0ull);
	TObjectPtr<USoundAttenuation>& Found = Attenuations.FindOrAdd(SettingsKey);
	if (Found != nullptr)
	{
		return Found;
	}

	const float Min = static_cast<float>(MinKey);
	// A sound without a radius has no reach limit in the original; it is inaudible long before this.
	const float Reach = RadiusKey > 0 ? ReachPerRadius * RadiusKey : 200.0f * Min;
	// The settings reach well past the original's reach: Unreal does not start a sound of a second or less whose
	// place lies beyond the attenuation's outer distance (au.SoundDistanceOptimizationLength), so with the outer
	// distance at the reach the one-shots handed over from beyond it (Play) were counted as played and never heard.
	const float Falloff = FMath::Max(Reach * HeardPastReach - Min, 1.0f);

	Found = NewObject<USoundAttenuation>(this);
	FSoundAttenuationSettings& Settings = Found->Attenuation;
	Settings.bAttenuate = true;
	Settings.bSpatialize = true;
	Settings.SpatializationAlgorithm = bHrtf ? ESoundSpatializationAlgorithm::SPATIALIZATION_HRTF : ESoundSpatializationAlgorithm::SPATIALIZATION_Default;
	Settings.AttenuationShape = EAttenuationShape::Sphere;
	Settings.AttenuationShapeExtents = FVector(Min, 0.0, 0.0);
	Settings.FalloffDistance = Falloff;

	// The original's curve, MinDistance / (MinDistance + 0.5 x (d - MinDistance)), sampled where it bends most
	// (close to the source). It is not cut to nothing at the reach: it goes on to the outer distance, and the last
	// key is the curve's own value there, which Unreal holds for anything further away (about 2% of full volume at
	// 100 x a radius equal to the minimum distance, 0.2% at 1000 x).
	Settings.DistanceAlgorithm = EAttenuationDistanceModel::Custom;
	FRichCurve* Curve = Settings.CustomAttenuationCurve.GetRichCurve();
	Curve->Reset();
	Curve->AddKey(0.0f, 1.0f);
	constexpr int32 Keys = 28;
	for (int32 Key = 1; Key <= Keys; ++Key)
	{
		const float Beyond = Falloff * FMath::Pow(1.0e-4f, 1.0f - static_cast<float>(Key) / Keys) * 0.98f;
		Curve->AddKey(Beyond / Falloff, Min / (Min + 0.5f * Beyond));
	}
	Curve->AddKey(1.0f, Min / (Min + 0.5f * Falloff));

	// Far sounds lose their top end, as through air.
	Settings.bAttenuateWithLPF = true;
	Settings.LPFRadiusMin = FMath::Max(Min, 1500.0f);
	Settings.LPFRadiusMax = FMath::Max(Min * 4.0f, 9000.0f);
	Settings.LPFFrequencyAtMin = 20000.0f;
	Settings.LPFFrequencyAtMax = 5000.0f;

	// The room is heard more on a distant source than on a near one.
	Settings.bEnableReverbSend = true;
	Settings.ReverbSendMethod = EReverbSendMethod::Linear;
	Settings.ReverbWetLevelMin = 0.12f;
	Settings.ReverbWetLevelMax = 0.5f;
	Settings.ReverbDistanceMin = Min;
	Settings.ReverbDistanceMax = FMath::Max(Min * 4.0f, 3000.0f);
	return Found;
}

USoundAttenuation* UScctAudioSubsystem::FindOrCreateOwnSoundSettings()
{
	if (OwnSoundSettings != nullptr)
	{
		return OwnSoundSettings;
	}

	// No distance, no direction: only the send to the reverb of the room the listener stands in.
	OwnSoundSettings = NewObject<USoundAttenuation>(this);
	FSoundAttenuationSettings& Settings = OwnSoundSettings->Attenuation;
	Settings.bAttenuate = false;
	Settings.bSpatialize = false;
	Settings.bAttenuateWithLPF = false;
	Settings.bEnableListenerFocus = false;
	Settings.bEnableOcclusion = false;
	Settings.bEnableReverbSend = true;
	Settings.ReverbSendMethod = EReverbSendMethod::Manual;
	Settings.ManualReverbSendLevel = OwnSoundReverbSend;
	return OwnSoundSettings;
}

UAudioComponent* UScctAudioSubsystem::AcquireComponent()
{
	while (FreeComponents.Num() > 0)
	{
		if (UAudioComponent* Component = FreeComponents.Pop(EAllowShrinking::No))
		{
			return Component;
		}
	}
	if (Components.Num() >= MaxComponents)
	{
		return nullptr;
	}

	UAudioComponent* Component = NewObject<UAudioComponent>(GetWorld()->GetWorldSettings());
	Component->bAutoActivate = false;
	Component->bAutoDestroy = false;
	Component->bStopWhenOwnerDestroyed = true;
	Component->RegisterComponentWithWorld(GetWorld());
	Components.Add(Component);
	return Component;
}

// ---------------------------------------------------------------------------------------------------------------
// Reverb
// ---------------------------------------------------------------------------------------------------------------

void UScctAudioSubsystem::LoadZones(const FString& Map)
{
	const FString Folder = ScctMapFolders::FolderOf(Map);
	const FString Path = (Folder.IsEmpty() ? ContentRoot / TEXT("svmap") / Map : Folder) / TEXT("audio/audio.json");
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		UE_LOG(LogOpenSCCT, Warning, TEXT("Audio: no audio table at %s: every zone gets the default room"), *Path);
		return;
	}

	// The zone rows have the reverb, the actor rows of the same id have the zone's place (which is how the
	// bridge's ambient records name a zone).
	TMap<FString, FVector> Places;
	const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
	if (Root->TryGetArrayField(TEXT("actors"), Actors))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Actors)
		{
			const TSharedPtr<FJsonObject>* Actor = nullptr;
			const TSharedPtr<FJsonObject>* Place = nullptr;
			FString Id;
			if (Value->TryGetObject(Actor) && (*Actor)->TryGetStringField(TEXT("id"), Id) && (*Actor)->TryGetObjectField(TEXT("location"), Place))
			{
				Places.Add(Id, FVector((*Place)->GetNumberField(TEXT("X")), (*Place)->GetNumberField(TEXT("Y")), (*Place)->GetNumberField(TEXT("Z"))));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* ZoneRows = nullptr;
	if (!Root->TryGetArrayField(TEXT("zones"), ZoneRows))
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *ZoneRows)
	{
		const TSharedPtr<FJsonObject>* Row = nullptr;
		const TSharedPtr<FJsonObject>* Effect = nullptr;
		const TSharedPtr<FJsonObject>* Params = nullptr;
		FString Id;
		if (!Value->TryGetObject(Row) || !(*Row)->TryGetStringField(TEXT("id"), Id) || !(*Row)->TryGetObjectField(TEXT("effect"), Effect)
			|| !(*Effect)->TryGetObjectField(TEXT("params"), Params))
		{
			continue;
		}
		const FVector* Place = Places.Find(Id);
		if (Place == nullptr)
		{
			continue;
		}

		const auto Number = [Params](const TCHAR* Name, double Default)
		{
			double Found = Default;
			(*Params)->TryGetNumberField(Name, Found);
			return Found;
		};

		// The EAX environment in the terms of Unreal's reverb, which keeps the same model: levels in millibels
		// become linear gains, times stay seconds.
		UReverbEffect* Reverb = NewObject<UReverbEffect>(this);
		Reverb->Gain = FMath::Clamp(FromMillibels(Number(TEXT("Room"), -1000.0)), 0.0f, 1.0f);
		Reverb->GainHF = FMath::Clamp(FromMillibels(Number(TEXT("RoomHF"), -100.0)), 0.0f, 1.0f);
		Reverb->DecayTime = FMath::Clamp(static_cast<float>(Number(TEXT("DecayTime"), 1.49)), 0.1f, 20.0f);
		Reverb->DecayHFRatio = FMath::Clamp(static_cast<float>(Number(TEXT("DecayHFRatio"), 0.83)), 0.1f, 2.0f);
		Reverb->ReflectionsGain = FMath::Clamp(FromMillibels(Number(TEXT("Reflections"), -2602.0)), 0.0f, 3.16f);
		Reverb->ReflectionsDelay = FMath::Clamp(static_cast<float>(Number(TEXT("ReflectionsDelay"), 0.007)), 0.0f, 0.3f);
		Reverb->LateGain = FMath::Clamp(FromMillibels(Number(TEXT("Reverb"), 200.0)), 0.0f, 10.0f);
		Reverb->LateDelay = FMath::Clamp(static_cast<float>(Number(TEXT("ReverbDelay"), 0.011)), 0.0f, 0.1f);
		Reverb->Diffusion = FMath::Clamp(static_cast<float>(Number(TEXT("EnvironmentDiffusion"), 1.0)), 0.0f, 1.0f);
		Reverb->Density = 1.0f;
		Reverb->AirAbsorptionGainHF = FMath::Clamp(FromMillibels(Number(TEXT("AirAbsorptionHF"), -5.0)), 0.892f, 1.0f);

		FZone Zone;
		Zone.Id = Id;
		(*Effect)->TryGetStringField(TEXT("class"), Zone.EffectClass);
		Zone.Location = *Place;
		Zone.Effect = ZoneEffects.Add(Reverb);
		Zone.DecayTime = Reverb->DecayTime;
		Zones.Add(MoveTemp(Zone));
	}
}

void UScctAudioSubsystem::UpdateReverb()
{
	if (ListenerZone == ActiveZone)
	{
		return;
	}
	ActiveZone = ListenerZone;

	const bool bZone = Zones.IsValidIndex(ActiveZone);
	UReverbEffect* Reverb = bZone ? ZoneEffects[Zones[ActiveZone].Effect].Get() : DefaultReverb.Get();
	if (bZone)
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("Audio: reverb of zone %s: %s (decay %.2f s, room %.2f, late %.2f)"), *Zones[ActiveZone].Id,
			*Zones[ActiveZone].EffectClass, Reverb->DecayTime, Reverb->Gain, Reverb->LateGain);
	}
	else
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("Audio: reverb of the default room (the listener's zone has none of its own)"));
	}

	if (FAudioDevice* Device = GetWorld()->GetAudioDeviceRaw())
	{
		// One tag: the new environment replaces the one before, cross-faded as the listener walks through the door.
		Device->ActivateReverbEffect(Reverb, ZoneReverbTag, 1.0f, 0.5f, 1.0f);
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Names and logging
// ---------------------------------------------------------------------------------------------------------------

const FString& UScctAudioSubsystem::GetName(int32 Id)
{
	if (const FString* Known = Names.Find(Id))
	{
		return *Known;
	}

	TCHAR Buffer[512];
	const int32 Length = Id != 0 ? Events->GetName(Id, Buffer, UE_ARRAY_COUNT(Buffer)) : 0;
	return Names.Add(Id, Length > 0 ? FString(Buffer) : FString());
}

FName UScctAudioSubsystem::FindDrawnBone(const AScctPawnProxy& Proxy, int32 BoneId) const
{
	// The spy's and the merc's models are different meshes, so the answer is kept per kind.
	const int64 Key = (static_cast<int64>(Proxy.GetPawn().Kind) << 32) | static_cast<uint32>(BoneId);
	if (const FName* Known = DrawnBones.Find(Key))
	{
		return *Known;
	}
	return DrawnBones.Add(Key, Proxy.FindMeshBoneName(const_cast<UScctAudioSubsystem*>(this)->GetName(BoneId)));
}

FString UScctAudioSubsystem::GetFilePath(int32 FileId)
{
	// A map outside the content folder (a downloaded pack, a custom map) has its files named in full.
	const FString& Name = GetName(FileId);
	return FPaths::IsRelative(Name) ? ContentRoot / Name : Name;
}

void UScctAudioSubsystem::LogVoice(const TCHAR* What, const FVoice& Voice, const TCHAR* Extra) const
{
	UScctAudioSubsystem* Self = const_cast<UScctAudioSubsystem*>(this);
	const float Distance = bListenerValid ? FVector::Dist(Voice.Location, Listener) : 0.0f;
	const TCHAR* Kind = Voice.bMusic ? TEXT("music") : Voice.AmbientId != 0 ? (Voice.bPositional ? TEXT("ambient 3D") : TEXT("ambient flat"))
		: Voice.bAtListener ? TEXT("at listener") : Voice.bPositional ? TEXT("3D") : TEXT("flat");
	if (Voice.bPositional)
	{
		const float Gain = DistanceGain(Distance, Voice.MinDistance, Voice.Radius);
		UE_LOG(LogOpenSCCT, Log, TEXT("Audio %s: %s '%s' call %d.%d actor %d at (%.0f, %.0f, %.0f) %.0f cm from the listener, volume %.2f x distance gain %.2f (min %.0f, reach %.0f), pitch %.2f%s%s%s%s [%d voices]"),
			What, Kind, *Self->GetName(Voice.FileId), Voice.CallId, Voice.Segment, Voice.ActorId, Voice.Location.X, Voice.Location.Y, Voice.Location.Z,
			Distance, Voice.Volume, Gain, Voice.MinDistance, ReachPerRadius * Voice.Radius, Voice.Pitch, Voice.bLoop ? TEXT(", loop") : TEXT(""),
			Voice.FollowActorId != 0 ? *FString::Printf(TEXT(", follows %d"), Voice.FollowActorId) : TEXT(""),
			Voice.FollowBoneId != 0 ? *FString::Printf(TEXT(" bone '%s'"), *Self->GetName(Voice.FollowBoneId)) : TEXT(""), Extra, Voices.Num());
	}
	else
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("Audio %s: %s '%s' call %d.%d actor %d, volume %.2f, pitch %.2f%s%s [%d voices]"),
			What, Kind, *Self->GetName(Voice.FileId), Voice.CallId, Voice.Segment, Voice.ActorId, Voice.Volume, Voice.Pitch,
			Voice.bLoop ? TEXT(", loop") : TEXT(""), Extra, Voices.Num());
	}
}

void UScctAudioSubsystem::LogSummary() const
{
	int32 Playing = 0, Waiting = 0, Ambient = 0, Sounding = 0;
	for (const FVoice& Voice : Voices)
	{
		Playing += Voice.State == EVoiceState::Playing ? 1 : 0;
		Waiting += Voice.State == EVoiceState::Waiting ? 1 : 0;
		Ambient += Voice.AmbientId != 0 ? 1 : 0;
		Sounding += Voice.Component.IsValid() ? 1 : 0;
	}
	UE_LOG(LogOpenSCCT, Log, TEXT("Audio summary: %d voices now (%d playing, %d waiting, %d ambient, peak %d), %d Unreal components in use of %d made; since the match began %d started, %d stopped, %d played beyond 100 x their radius, %d refused by the slot rule, %d replaced, %d objective jingles skipped; files: %d decoded (%.1f MB held), %d failed to decode, %d plays lost to them; listener at (%.0f, %.0f, %.0f)"),
		Voices.Num(), Playing, Waiting, Ambient, PeakVoices, Sounding, Components.Num(), NumStarted, NumStopped, NumOutOfReach, NumRefused, NumStolen, NumObjectiveSkipped,
		Cache.GetNumDecoded(), Cache.GetBytes() / (1024.0 * 1024.0), Cache.GetNumFailed(), NumFailedFiles, Listener.X, Listener.Y, Listener.Z);
	UE_LOG(LogOpenSCCT, Log, TEXT("Audio summary: 3D voices started %d with HRTF, %d panned; output mode %s%s"), NumHrtfStarted, NumPannedStarted,
		UScctAudioOutputSubsystem::HeadphonesChosen() ? TEXT("Headphones") : TEXT("Speakers"),
		UScctAudioOutputSubsystem::SpatializerProblem().IsEmpty() ? TEXT("") : *(TEXT(" (") + UScctAudioOutputSubsystem::SpatializerProblem() + TEXT(")")));
	LogAcousticSummary();
	LogAudioViewSummary();
	LogRoomSummary();
	LogSequenceSummary();
	TArray<FString> Notes;
	OnSummary().Broadcast(Notes);
	UE_CLOG(Notes.Num() > 0, LogOpenSCCT, Log, TEXT("Audio summary: also %s"), *FString::Join(Notes, TEXT("; ")));

	// The engine's own listener must be where this subsystem takes it to be (the camera), or every 3D voice is
	// panned and attenuated from somewhere else: said in the log when the two are apart.
	FVector EngineListener;
	const UWorld* World = GetWorld();
	const FAudioDevice* Device = World != nullptr ? World->GetAudioDeviceRaw() : nullptr;
	if (bListenerValid && Device != nullptr && Device->GetListenerPosition(0, EngineListener, false))
	{
		const double Apart = FVector::Dist(EngineListener, Listener);
		UE_CLOG(Apart > 50.0, LogOpenSCCT, Warning, TEXT("Audio: the engine's listener is at (%.0f, %.0f, %.0f), %.0f cm from the camera"),
			EngineListener.X, EngineListener.Y, EngineListener.Z, Apart);
		UE_CLOG(Apart <= 50.0 && bLogVoices, LogOpenSCCT, Log, TEXT("Audio: the engine's listener is at the camera (%.0f cm apart)"), Apart);
	}
}
