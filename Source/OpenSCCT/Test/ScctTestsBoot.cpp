// The runner's own checks and the boot test (the Godot client's SelfTestFailSmoke and BootSmoke).

#include "OpenSCCT.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Play/ScctRuntimeSkins.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Play/ScctSessionSubsystem.h"
#include "Runtime/ScctFeatures.h"
#include "Test/ScctTest.h"
#include "Test/ScctTestLevels.h"

namespace
{
	/** Check actual GPU-ready rigs and base textures from the player's Steam build, including both first-person roles. */
	class FScctLocalSteamSkins : public IScctTest
	{
	public:
		virtual FScctTestResult Step(FScctTestContext&) override
		{
			const TCHAR* Files[] = { TEXT("SPerso/ATT_01"), TEXT("SPerso/DEF_01"), TEXT("SPerso/ATT_01_fp"),
				TEXT("SWeaponFirst/1DEF_01"), TEXT("SWeaponFirst/1DEF_Uzi"), TEXT("SWeaponFirst/M160"),
				TEXT("OpenScctRifles/pumpshotgun"), TEXT("OpenScctRifles/pumpshotgun_first"), TEXT("OpenScctRifles/compactsmg_first") };
			for (const TCHAR* ModelName : Files)
			{
				const FString Relative = FString(TEXT("shared/chars")) / (FString(ModelName) + TEXT(".glb"));
				USkeletalMesh* Mesh = ScctRuntimeSkins::Load(Relative);
				if (!Mesh || Mesh->GetRefSkeleton().GetNum() < 2 || ScctRuntimeSkins::ContentFileOf(Mesh).IsEmpty())
					return FScctTestResult::Fail(TEXT("Missing local skeleton for ") + Relative);
				const FSkeletalMeshRenderData* Data = Mesh->GetResourceForRendering();
				if (!Data || !Data->LODRenderData.Num() || !Data->LODRenderData[0].GetNumVertices()
					|| Data->LODRenderData[0].RenderSections.IsEmpty())
					return FScctTestResult::Fail(TEXT("Missing render sections for ") + Relative);
				const bool bLongGun = Relative.Contains(TEXT("/OpenScctRifles/"));
				bool bTextured = false;
				for (const FSkeletalMaterial& Slot : Mesh->GetMaterials())
				{
					const auto HasLocalTexture = [&Slot](const TCHAR* Parameter)
					{
						UTexture* Image = nullptr;
						return Slot.MaterialInterface && Slot.MaterialInterface->GetTextureParameterValue(Parameter, Image) &&
							Image && Image->GetOutermost()->GetName().StartsWith(TEXT("/Temp/OpenSCCTSteam/"));
					};
					bTextured |= bLongGun ? HasLocalTexture(TEXT("GunBase")) && HasLocalTexture(TEXT("GunNormal")) && HasLocalTexture(TEXT("GunORM")) :
						HasLocalTexture(TEXT("BaseColor"));
				}
				if (!bTextured) return FScctTestResult::Fail(TEXT("Missing locally loaded material inputs for ") + Relative);
			}
			UStaticMesh* Effect = ScctRuntimeSkins::LoadEffectMesh(TEXT("SGameplayObjects.Gadgets.flare_grenade"));
			if (!Effect || !Effect->GetRenderData() || Effect->GetRenderData()->LODResources.IsEmpty() ||
				!Effect->GetRenderData()->LODResources[0].GetNumVertices())
				return FScctTestResult::Fail(TEXT("Missing locally built effect mesh geometry"));
			return FScctTestResult::Pass(TEXT("9 local rigs have bones, geometry and textures; the local flare effect also has geometry"));
		}
	};

	/** Passes at once: the runner, its output and exit code 0 work. */
	class FScctSelfTestPass : public IScctTest
	{
	public:
		virtual FScctTestResult Step(FScctTestContext& Context) override
		{
			return FScctTestResult::Pass(FString::Printf(TEXT("frame=%d"), Context.Frame));
		}
	};

	/** Always fails: a run of it must end with exit code 1 (the check that a failure is not reported as a pass). */
	class FScctSelfTestFail : public IScctTest
	{
	public:
		virtual FScctTestResult Step(FScctTestContext& Context) override
		{
			return FScctTestResult::Fail(TEXT("this test always fails (it checks that a failure gives exit code 1)"));
		}
	};

	template <typename TTable>
	void CheckBootTable(const TCHAR* Label, TArray<FString>& Loaded, TArray<FString>& Missing)
	{
		FString Error;
		if (const TTable* Table = ScctFeature<TTable>(&Error))
		{
			Loaded.Add(FString::Printf(TEXT("%s=%d"), Label, Table->Version));
		}
		else
		{
			Missing.Add(FString::Printf(TEXT("%s (%s)"), Label, *Error));
		}
	}

	/**
	 * The bridge loads with the content folder this run was given, each of the six feature tables the client is
	 * built on comes at the version this build mirrors (what -ScctCheckFeatures only logs), the simulation assemblies
	 * load and a World steps 600 ticks, and a local match (TestBox, the bridge's own fixture, with three bots) runs 600
	 * client ticks in the world the game started in.
	 */
	class FScctBootTest : public IScctTest
	{
	public:
		static constexpr int32 Ticks = 600;

		virtual bool AllowsSessionFailure() const override { return true; }

		virtual FScctTestResult Step(FScctTestContext& Context) override
		{
			if (!bChecked)
			{
				bChecked = true;
				FScctTestResult Result = CheckBridge(Context);
				if (Result.State != EScctTestState::Running)
				{
					return Result;
				}
			}

			UScctSessionSubsystem* Session = Context.GetSession();
			if (Session == nullptr)
			{
				// Between levels as the game starts: wait for its first world.
				return Context.Frame < 600 ? FScctTestResult::Running() : FScctTestResult::Fail(TEXT("no game world came up"));
			}

			if (!bStarted)
			{
				bStarted = true;
				if (!Session->StartLocalMatch(ScctTestLevels::TestBoxMap, TEXT("Story"), false, 3))
				{
					return FScctTestResult::Fail(FString::Printf(TEXT("the %s match did not start: %s"), ScctTestLevels::TestBoxMap, *Session->GetFailure()));
				}
				SessionId = Session->GetSessionId();
				return FScctTestResult::Running();
			}

			if (!Session->GetFailure().IsEmpty())
			{
				return FScctTestResult::Fail(FString::Printf(TEXT("the match failed: %s"), *Session->GetFailure()));
			}
			if (Session->GetSessionId() != SessionId)
			{
				return FScctTestResult::Fail(FString::Printf(TEXT("the match (session %d) ended; the world presents session %d"), SessionId, Session->GetSessionId()));
			}

			const int32 ClientTicks = Session->GetSessionInfo().Ticks;
			if (ClientTicks < Ticks)
			{
				return FScctTestResult::Running();
			}

			Session->StopMatch();
			return FScctTestResult::Pass(FString::Printf(TEXT("%s worldTicks=%d match=%s clientTicks=%d"),
				*FString::Join(Tables, TEXT(" ")), WorldTicks, ScctTestLevels::TestBoxMap, ClientTicks));
		}

	private:
		FScctTestResult CheckBridge(FScctTestContext& Context)
		{
			FString Error;
			if (FOpenSCCTModule::Get().GetBridge(Error) == nullptr)
			{
				return FScctTestResult::Fail(FString::Printf(TEXT("the bridge did not load: %s"), *Error));
			}

			TArray<FString> Missing;
			CheckBootTable<FScctControlsApi>(TEXT("controls"), Tables, Missing);
			CheckBootTable<FScctEventsApi>(TEXT("events"), Tables, Missing);
			CheckBootTable<FScctHudApi>(TEXT("hud"), Tables, Missing);
			CheckBootTable<FScctWorldApi>(TEXT("world"), Tables, Missing);
			CheckBootTable<FScctEquipmentApi>(TEXT("equipment"), Tables, Missing);
			CheckBootTable<FScctOnlineApi>(TEXT("online"), Tables, Missing);
			CheckBootTable<FScctTestApi>(TEXT("test"), Tables, Missing);
			if (Missing.Num() > 0)
			{
				return FScctTestResult::Fail(FString::Printf(TEXT("feature tables missing: %s"), *FString::Join(Missing, TEXT("; "))));
			}

			const FScctTestApi* Test = ScctFeature<FScctTestApi>();
			WorldTicks = Test->StepWorld(Ticks);
			if (WorldTicks != Ticks)
			{
				return FScctTestResult::Fail(WorldTicks < 0
					? FString::Printf(TEXT("simulation: %s"), *Context.GetTestApiError())
					: FString::Printf(TEXT("simulation: the world ran %d ticks, expected %d"), WorldTicks, Ticks));
			}
			return FScctTestResult::Running();
		}

		bool bChecked = false;
		bool bStarted = false;
		int32 SessionId = 0;
		int32 WorldTicks = 0;
		TArray<FString> Tables;
	};
}

SCCT_TEST("selftest-pass", FScctSelfTestPass);
SCCT_TEST("selftest-fail", FScctSelfTestFail);
SCCT_TEST("boot", FScctBootTest);

SCCT_TEST("local-steam-skins", FScctLocalSteamSkins);
