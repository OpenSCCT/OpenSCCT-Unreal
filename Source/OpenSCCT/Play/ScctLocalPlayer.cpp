#include "Play/ScctLocalPlayer.h"
#include "Play/ScctNativePad.h"

#include "Engine/GameViewportClient.h"
#include "Engine/GameInstance.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/InputSettings.h"
#include "HAL/IConsoleManager.h"
#include "Menu/ScctMenuKit.h"
#include "Menu/SScctPadPanel.h"
#include "Misc/StringOutputDevice.h"
#include "Play/ScctConsoleCommands.h"
#include "Settings/ScctSettingsConsole.h"
#include "Widgets/SViewport.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
// Keep WinUser's LoadImageW alias out of FImageUtils declarations in unity builds.
#undef LoadImage
#endif

namespace
{
	class FScctConsoleShortcut : public IInputProcessor
	{
	public:
		explicit FScctConsoleShortcut(UGameViewportClient* InViewport) : Viewport(InViewport) {}
		virtual void Tick(float, FSlateApplication&, TSharedRef<ICursor>) override {}

		virtual bool HandleMouseButtonDownEvent(FSlateApplication& Slate, const FPointerEvent&) override { return CapturesPointer(Slate); }
		virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& Slate, const FPointerEvent&) override { return CapturesPointer(Slate); }
		virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& Slate, const FPointerEvent&, const FPointerEvent*) override { return CapturesPointer(Slate); }

		virtual bool HandleKeyDownEvent(FSlateApplication& Slate, const FKeyEvent& Event) override
		{
			UGameViewportClient* Client = Viewport.Get();
			if (!Client || !Client->ViewportConsole || Event.IsControlDown() || Event.IsAltDown() || Event.IsCommandDown())
			{
				return false;
			}
			const TArray<FKey>& Keys = GetDefault<UInputSettings>()->ConsoleKeys;
			bool bToggle = Keys.Contains(Event.GetKey());
#if PLATFORM_WINDOWS
			// Godot uses the physical key left of 1. On a UK keyboard it can report a different virtual key.
			if (!bToggle && Keys.Contains(EKeys::Tilde))
			{
				bToggle = Event.GetKeyCode() == ::MapVirtualKeyW(0x29, MAPVK_VSC_TO_VK_EX);
			}
#endif
			if (!bToggle) return false;
			const TSharedPtr<SViewport> Widget = Client->GetGameViewportWidget();
			if (!Widget || (!Widget->HasKeyboardFocus() && !Widget->HasFocusedDescendants())) return false;

			UConsole* Console = Client->ViewportConsole;
			const auto Focus = Slate.GetKeyboardFocusedWidget();
			if (!Console->ConsoleActive() && (ScctUi::IsTyping() ||
				(Focus && Focus->GetType() == FName(TEXT("SScctKeyRow")) && StaticCastSharedPtr<SScctKeyRow>(Focus)->IsCapturing()) ||
				(Focus && Focus->GetType() == FName(TEXT("SScctPadBindRow")) && StaticCastSharedPtr<SScctPadBindRow>(Focus)->IsCapturing()))) return false;
			if (Event.IsRepeat()) return true;
			if (Console->ConsoleActive())
			{
				Console->FakeGotoState(NAME_None);
			}
			else
			{
				// The typing state saves menu focus and gives the viewport the keys. Open shows command output too.
				Console->StartTyping(TEXT(""));
				Console->FakeGotoState(TEXT("Open"));
			}
			return true;
		}
	private:
		bool CapturesPointer(FSlateApplication& Slate) const
		{
			const UGameViewportClient* Client = Viewport.Get();
			return Client && Client->ViewportConsole && Client->ViewportConsole->ConsoleActive() &&
				Client->GetWindow() == Slate.GetActiveTopLevelWindow();
		}
		TWeakObjectPtr<UGameViewportClient> Viewport;
	};
}

void UScctLocalPlayer::PlayerAdded(UGameViewportClient* InViewport, FPlatformUserId InUserId)
{
	Super::PlayerAdded(InViewport, InUserId);
	if (InViewport && FSlateApplication::IsInitialized() && !ConsoleShortcut)
	{
		if (!InViewport->ViewportConsole)
		{
			InViewport->ViewportConsole = NewObject<UScctGameConsole>(InViewport);
		}
#if PLATFORM_WINDOWS
		if (InViewport->GetGameInstance() && InViewport->GetGameInstance()->GetFirstGamePlayer() == this)
		{
			NativePad = MakeScctNativePad(this);
			FSlateApplication::Get().RegisterInputPreProcessor(NativePad, 0);
		}
#endif
		ConsoleShortcut = MakeShared<FScctConsoleShortcut>(InViewport);
		FSlateApplication::Get().RegisterInputPreProcessor(ConsoleShortcut, 0);
	}
}

void UScctLocalPlayer::PlayerRemoved()
{
	if (ConsoleShortcut && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(ConsoleShortcut);
	}
	if (NativePad && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(NativePad);
	}
	NativePad.Reset();
	ConsoleShortcut.Reset();
	Super::PlayerRemoved();
}

void UScctGameConsole::ConsoleCommand(const FString& Command)
{
	const FString Line = Command.TrimStartAndEnd();
	HistoryBuffer.Remove(Line);
	HistoryBuffer.Add(Line);
	if (HistoryBuffer.Num() > MAX_HISTORY_ENTRIES) HistoryBuffer.RemoveAt(0);
	OutputText(TEXT("> ") + Line);
	FStringOutputDevice Reply;
	Reply.SetAutoEmitLineTerminator(true);
	UWorld* World = GetOuterUGameViewportClient()->GetWorld();
	if (!ScctConsoleCommands::ExecSettingCommand(*Line, World, Reply) && !ScctConsoleCommands::ExecGameCommand(*Line, World, Reply))
	{
		FString Verb = Line;
		int32 Space;
		if (Verb.FindChar(TEXT(' '), Space)) Verb.LeftInline(Space);
		if (Verb.Equals(TEXT("Settings"), ESearchCase::IgnoreCase) || Verb.Equals(TEXT("Admin"), ESearchCase::IgnoreCase) ||
			Verb.Equals(TEXT("AdminLogin"), ESearchCase::IgnoreCase))
		{
			IConsoleManager::Get().ProcessUserConsoleInput(*Line, Reply, World);
		}
		else ScctConsoleCommands::SayUnknown(*Line, Reply);
	}
	OutputText(Reply.TrimEnd());
}

void UScctGameConsole::BuildRuntimeAutoCompleteList(bool bForce)
{
#if ALLOW_CONSOLE
	Super::BuildRuntimeAutoCompleteList(bForce);
#else
	if (!bForce)
	{
		bIsRuntimeAutoCompleteUpToDate = false;
		return;
	}
	AutoCompleteList.Reset();
	const FColor CommandColor(180, 180, 180);
	auto Add = [this, &CommandColor](const FString& Command, const FString& Desc)
	{
		FAutoCompleteCommand& Entry = AutoCompleteList.AddDefaulted_GetRef();
		Entry.Command = Command;
		Entry.Desc = Desc;
		Entry.Color = CommandColor;
	};
	// The console commands registered in Shipping are the game's own (ScctConsoleCommands.cpp): its Godot console words,
	// Settings, Admin, AdminLogin and the Scct.* commands. Their help's first line describes each.
	IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateLambda(
		[&Add](const TCHAR* Name, IConsoleObject* Object)
		{
			if (Object == nullptr || !Object->IsEnabled() || Object->AsCommand() == nullptr) return;
			TArray<FString> Lines;
			FString(Object->GetHelp()).ParseIntoArrayLines(Lines);
			Add(Name, Lines.IsEmpty() ? FString() : Lines[0]);
		}));
	Add(TEXT("help"), TEXT("help [command|bots] - the commands, or one command's help."));
	Add(TEXT("Get"), TEXT("Get <key> - a setting's value (Settings lists the keys)."));
	Add(TEXT("Set"), TEXT("Set <key> <value> - sets a setting, puts it in force and saves it."));
	for (const FString& Key : FScctSettingsConsole::Keys())
	{
		Add(Key, TEXT("<value> - a setting (alone: its value)."));
	}
	AutoCompleteList.Sort();
	bIsRuntimeAutoCompleteUpToDate = true;
#endif
}

#if UE_ALLOW_EXEC_COMMANDS
bool UScctLocalPlayer::Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
{
	if (ScctConsoleCommands::ExecSettingCommand(Cmd, InWorld, Ar) || ScctConsoleCommands::ExecGameCommand(Cmd, InWorld, Ar))
	{
		return true;
	}
	if (Super::Exec(InWorld, Cmd, Ar))
	{
		return true;
	}
	if (FString(Cmd).TrimStartAndEnd().IsEmpty())
	{
		return false;
	}
	ScctConsoleCommands::SayUnknown(Cmd, Ar);
	return true;
}
#endif
