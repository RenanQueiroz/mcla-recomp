# Player name (2026-10-27)

`user_name` (`patches/rexglue-user-name.patch`) and the launcher's *Player name* box. The SDK's `UserProfile` had `name_ = "User"`; the game
asks for it with `XamUserGetName` (the recompiled game calls it through one function that fills a buffer and, if the call fails, writes a
default of its own with `snprintf`) and `XamUserGetSigninInfo` (the XUID, the sign-in state). A fork issue (#14) asked to change the nickname.

## What was built

- `user_name` (default `User`, restart to apply): the spaces around it dropped, then 1 to 15 ASCII letters, digits and spaces; anything else is
  ignored with a warning in the log and the name is `User`. The log line `user profile: name '...', XUID ...` is written at every start.
- The XUID `0xB13EBABEBABEBABE` is not touched (the folder of `mc4.sav` and the achievements is `<user root>/B13EBABEBABEBABE/<title id>/...`).
- `ContentManager::ResolveGameUserContentPath` (title-specific profile settings: `<user root>/<title id>/profile/<name>/<setting id>`) is fixed to
  `User`, the folder it always had, so changing the name never loses them.
- The launcher's *Player name* text box (`launcher/src/GameSettings.cs` `PlayerName`, `LauncherApp.cs` `PlayerNameBox`): ASCII letters, digits and
  spaces, 15 at most; written to `mcla.toml` as typed (spaces around it dropped); an empty box removes the line; disabled while the game runs.

## Checks

Written before the first run of the build with the patch (the lock was busy): what follows is the plan, and the results go here.

- the launcher's helpers with made-up input (`Clean`, `ForFile`, `In`): 13 rows, all as expected (a scratch program through reflection, not committed);
- on Windows, the game with `--user_name=Zed` (and a name with a space, a name of 15 characters, and names the SDK must refuse: 16 characters, `!`, an
  accent, only spaces, empty), reading the log line; a tour of the pause menu and a drive with the name, looking at the screens for it; the save
  (the pristine copy of `B13EBABEBABEBABE`) loaded under `User` and under `Zed`, its files unchanged by the run, the XUID in the log the same.
