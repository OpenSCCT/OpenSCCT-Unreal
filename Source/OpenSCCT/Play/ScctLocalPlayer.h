#pragma once

#include "CoreMinimal.h"
#include "Engine/Console.h"
#include "Engine/LocalPlayer.h"
#include "ScctLocalPlayer.generated.h"

/**
 * The game's local player (Config/DefaultEngine.ini LocalPlayerClassName). Its only addition is the console: Get and
 * Set of a setting and the Godot console's words (Play/ScctConsoleCommands.h) are run before the engine's own words of
 * the same names (GET and SET of class properties, HELP, SAY), which would otherwise take them. Anything else goes on to
 * the engine as before, and a line the engine does not know either is answered as the Godot console answers it.
 */
UCLASS()
class OPENSCCT_API UScctLocalPlayer : public ULocalPlayer
{
	GENERATED_BODY()

public:
	virtual void PlayerAdded(UGameViewportClient* InViewport, FPlatformUserId InUserId) override;
	virtual void PlayerRemoved() override;
#if UE_ALLOW_EXEC_COMMANDS
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override;
#endif

private:
	TSharedPtr<class IInputProcessor> ConsoleShortcut;
	TSharedPtr<class IInputProcessor> NativePad;
};

/** Shipping has no engine exec console. The viewport still draws and edits the line for the game's own commands. */
UCLASS(Within=GameViewportClient)
class UScctGameConsole : public UConsole
{
	GENERATED_BODY()
public:
	virtual void ConsoleCommand(const FString& Command) override;
	/**
	 * Shipping compiles the engine's list out (ALLOW_CONSOLE), so the line offered nothing as it was typed: there this
	 * lists what ConsoleCommand runs, the game's console words and the settings keys. Other builds keep the engine's.
	 */
	virtual void BuildRuntimeAutoCompleteList(bool bForce = false) override;
};
