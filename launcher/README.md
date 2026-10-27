# Midnight Club: Los Angeles launcher

Part of this fork (see `..\FORK.md`); Windows only. Desktop shortcut →
`launcher\app\MclaLauncher.exe`. It opens on the game's settings; **Play**
starts `mcla.exe`. On start it fetches the fork
([RenanQueiroz/mcla-recomp](https://github.com/RenanQueiroz/mcla-recomp)); when
the fork has new commits it offers **Update and rebuild**, with the commits folded
under *What's new*. Routine states (checking, up to date, offline) are a line
under the title. *More* links to the fork's recent changes and says how many
commits upstream ([holdmysocks/mcla-recomp](https://github.com/holdmysocks/mcla-recomp))
has that the fork has not taken yet: the fork takes them only when asked to.
A commit that changes only the launcher or documentation doesn't ask for a game
rebuild.

```
..\..\                               the folder the repository is in
├─ *.iso                             your disc image (only read until game\ is extracted)
└─ mcla-recomp\                      the fork's checkout; game\ and out\ live in here
   └─ launcher\
      ├─ build-game.ps1              update + build; the launcher runs this
      ├─ build-launcher.ps1          rebuild the launcher itself, recreate the shortcut
      ├─ update-launcher.ps1         rebuild and restart it after an update changed it
      ├─ src\                        launcher source (C#, Reactor on WinUI 3)
      ├─ dev\                        measurement tools (below)
      ├─ app\                        the built launcher                      (not tracked)
      ├─ logs\                       one log per build the launcher ran      (not tracked)
      ├─ settings.json               launcher options                        (not tracked)
      ├─ built-commit.txt            commit of the last successful build     (not tracked)
      └─ build-options.json          what the last build was asked for       (not tracked)
```

## From a terminal

```powershell
.\build-game.ps1             # build (or finish building) what is checked out
.\build-game.ps1 -Update     # fast-forward to the fork's latest commit, then build
.\build-game.ps1 -Ref v1.2.3 # move to a release tag (or -Ref origin/main) instead of following the branch, then build
                             # (what a wizard install does; its launcher moves the checkout itself)
.\build-game.ps1 -ResetSdk   # re-apply the SDK patches and rebuild the SDK
.\build-game.ps1 -Toolchain mingw   # build with llvm-mingw instead of the Visual Studio Build Tools
```

`build-game.ps1` wraps `scripts\make_pc.ps1` and adds what it leaves out:

- **Changed SDK patches.** `make_pc.ps1` patches the ReXGlue SDK once and adds
  later patches by a name each one adds, so a patch that changed would never be
  applied again. When an update changes `patches\*.patch` the SDK checkout is
  reset and patched again (incremental SDK rebuild). If the SDK version itself
  changes, the SDK is re-cloned.
- **Local edits.** Updates use `git merge --ff-only --autostash`. Codegen rewrites
  `mcla_manifest.toml` and `generated\rexglue.cmake` with LF line endings, which
  `.gitattributes` now asks the checkout for too (a clone made before that shows
  them as modified until `git add` on them); autostash carries them across
  updates safely.
- **The toolchain and the other options.** `-Toolchain msvc|mingw`, `-ToolsDir`,
  `-SdkBundle` and `-SdkBundleSha256` go to `make_pc.ps1` (README.md, "Without
  Microsoft's compiler"), which keeps them in `launcher\build-options.json` when a
  build finishes, however it was run, if they are more than the defaults. Given
  none of them, a build, an update and the launcher's **Update and rebuild** build
  the way the last build did (the same toolchain, portable tools and prebuilt SDK,
  and not msvc or an SDK built from source behind your back; `make_pc.ps1` reads the
  record itself, so an older `build-game.ps1`, which passes none, does too); `msvc`
  where there is no record (every install made before there was a choice). A record
  written by hand, `{"toolchain": "mingw"}`, switches an install with its next
  update (README.md, "Without Microsoft's compiler"). Given any of them,
  only what is given is used. Changing the toolchain reconfigures and rebuilds the
  game; each toolchain has an SDK build of its own, the first one built the first
  time. A prebuilt SDK has no patched checkout, so `-ResetSdk` and the patch check
  do nothing with it; an update that changes the patches makes it stale
  (`mcla-sdk-bundle.json` says what it was built from), and it is refused until a
  matching one is given.
- **Python.** `make_pc.ps1` counts a Python that runs (`python --version` says
  Python 3), also from `WindowsApps`, where the Microsoft Store and the Python
  install manager put theirs, and not Windows' placeholder `python.exe` there, which
  fails. (This wrapper used to hand over a working one itself.)

The fork's changes to the game and the SDK are ordinary commits, each described
in `KNOWN-ISSUES.md` (game) or `patches\README.md` (SDK).

## The setup wizard (a PC with no install)

Started from a folder that is not inside a fork checkout and has no finished install of its own, the
launcher shows the **setup wizard** instead of the launcher: it makes the install the rest of this file
assumes (a checkout, the game extracted from the player's own disc image, the build) with nothing but the
launcher's exe and an internet connection: no Visual Studio, Git, Python or administrator rights.

**Which one starts** (`Paths.Detect`, in this order):

1. **Classic**: the exe is somewhere below a folder with `build-game.ps1` (a fork checkout's `launcher\`; the
   desktop shortcut of an install made with `scripts\make_pc.ps1`). Nothing changes there; the wizard is never
   looked at. (A launcher exe copied out of its checkout used to open with an error bar; it now opens the
   wizard, as it has no checkout above it.)
2. **Installed**: `install.json` beside the exe names an install folder that holds a finished setup
   (`state` is `ready`) and `mcla-recomp\launcher\build-game.ps1`. The launcher works on that checkout
   (settings, Play, Update and rebuild). git is not installed on that PC: the launcher starts the install's
   own MinGit (`Paths.GitExe`) for its git calls and puts its folder first on the `PATH` of the scripts it runs.
3. **Wizard**: anything else. An `install.json` beside the exe without a finished setup is a setup to resume.

**What it asks.** The folder (default `%LOCALAPPDATA%\MCLA`, or `C:\MCLA` when that path has a character the
build can't take; it must be new or empty, or hold an earlier setup of this wizard, which then continues), then
the game: drop the disc image on the window, or **Browse** for an `.iso` or for a folder with the game's files.
The disc image is read as it is (XDVDFS; no extractor and no tool needed). A *Complete Edition (USA, Europe)*
disc or folder passes; anything else says why in plain words (another game, another release of this one, a
damaged or modified file, not a game at all). A folder can also be used **where it is**: nothing but
`default.xex` is copied, and `game_data_root` in `mcla.toml` (next to `mcla.exe`) names the folder, which then
has to stay where it is (if it is gone or has lost files, the game says which folder and stops, instead of
starting with part of its files). The check is the file list in the release (`release.json`, `game`: names,
sizes, SHA-256; no game data leaves the PC and none is in the repository); `root_directory_padding.pad`, the
disc's filler, may be missing from a folder.

A folder that already holds a setup **with its game** (the checkout's `game\`, or the kept folder, still passing
the check) goes straight on: the disc image is not asked for again. Pointing a newer release's wizard at an older
install replaces the release (the checkout is made again and built; a few minutes) and keeps the player's game
settings (`mcla.toml`), the launcher's `settings.json`, the saves and the shader storage.

**What it does** (six steps, shown as they run; the first two run together):

| Step | |
|---|---|
| Copying your game (or Checking it, in place) | The files into the install (`game-incoming\`, moved into place after the clone) with the SHA-256 of each one checked while it is copied; resumable file by file. |
| Downloading the build tools | llvm-mingw, CMake, Ninja, MinGit and the prebuilt SDK from the release, each checked against its SHA-256 and size (`downloads\`, `tools\`; an archive that writes outside its folder, or names a file Windows would treat as something else, is refused). A tool that is unpacked from the right archive (`tools\.mcla-tools.txt`) is not fetched again. |
| Getting the source | `git clone --branch <tag>` of the fork (not recursive: the SDK comes as the prebuilt bundle), then the tag is checked to point at `release.json`'s commit, and the shader pack to be the release's. A checkout counts as there only when the step is recorded finished and every tracked file is in the tree: a clone that was killed half way is made again. |
| Translating the game's code, Building the game | One `build-game.ps1 -Toolchain mingw -ToolsDir ... -SdkBundle ...` run, with the progress it prints. |
| Preparing shaders | The game's own shader preparation (see *Preparing shaders*). If it fails the setup is still done: the game compiles shaders as it meets them, and **More > Prepare shaders** tries again. |

The last page opens the launcher (the exe starts again, now in Installed mode); if the folder the exe is in can't
be written, there is no note of where the install is, the page says so, and the next start of the wizard on the
same folder opens at once. Cancelling, closing the window (it asks first) or a failure keeps what was done; the
next start of the same folder continues from there, and the disk space asked for is only what is still left
to do. A failure says what went wrong in plain words, shows the log under **Details**, and offers **Copy the
log** and **Report it on GitHub**. Logs are in the install folder's `logs\`.

**One setup at a time.** A folder is locked while a setup runs in it (`install.lock`, held open): a second wizard
on it says so. The programs the setup starts (git, the build) are in a job object that ends with the wizard,
however it ends; a build can't go on writing into an install after its wizard is gone. Renames and deletes of
files a virus scanner may hold are retried for about ten seconds; the network has timeouts (30 s for GitHub to
answer, 60 s without a byte in a download).

**The folder.** The install folder's path may have **129 characters** at most, and only letters without accents,
digits, spaces, dots, dashes and underscores (the wizard says so before it starts):

- Length: the longest path the install holds is 127 characters below the folder (CMake's own documentation,
  below `tools\`), and Windows' programs, PowerShell 5.1 and Explorer among them, stop at 259. The longest file
  the build reads is 124 below the folder (`mcla-recomp\third_party\rexglue-sdk-bin\include\...`); the compiler's
  object files are longer in a short folder (131) but CMake moves one into a hashed folder when its path would
  pass 250 characters. (`make_pc.ps1` alone refuses a checkout folder over 148, so 129 is the stricter rule.)
  `PathBudget.LongestBelowRoot` holds the number; the check harness's `install` measures a real install and
  fails if it is too small, which is what to run after `tools.json` changes a tool's version.
- Characters: the SDK's code generator opens its files by a path that is not Unicode (an install in
  `C:\...\Jogos é` stops at "File could not be opened for reading"), and the commands CMake and Ninja run through
  `cmd.exe` break at `&` (tested: `'Co' is not recognized`); `^ % ! ' ;` and non-ASCII letters are refused with
  them. A user name with an accent therefore gets `C:\MCLA` as the suggested folder instead of `%LOCALAPPDATA%`.
- Also refused: a network location or mapped network drive, a drive that is not NTFS or ReFS (Git refuses folders
  without owners), a drive's root, `C:\Windows` and `C:\Program Files` (no administrator rights are asked for),
  the folder of the launcher that is running and the one the installer puts the launcher in (`%LOCALAPPDATA%\Programs\MCLA`),
  whether the chosen folder is one of them, inside one, or has one inside, and a folder inside one that holds a launcher
  (`installed.txt` or `MclaLauncher.exe` beside it). The folders are compared as full paths, ignoring case, in the form Windows opens
  them (`GetFinalPathNameByHandle`: junctions and symbolic links at any level, chains of them, links to a drive root, `subst` drives
  and 8.3 names are resolved, on both sides): the launcher's folder is replaced when it updates and removed with it. A launcher
  unpacked into the root of a drive is asked to move into a folder of its own, rather than every folder of the drive being refused,
  a game folder inside the install or the install inside the game's folder; a folder in OneDrive is warned
  about. Free space needed: the game's size (when copied) and 3 GB, less what the folder holds already.

**Where the release comes from.** The latest published release of the fork
(`releases/latest/download/release.json`, then the assets of its tag). Until the first release is published
the wizard says there is none yet. What `release.json` names (the SDK bundle's file, the tools' addresses and
versions, the shader pack's path) must be plain file names and `https` addresses, or the release is refused.

**Removing an install.** There is no button yet; `InstallCleanup.Uninstall` (the harness's `uninstall`
command) removes exactly the names `install.json` lists as made by the setup (`mcla-recomp\`, `tools\`,
`downloads\`, `game-incoming\`, `kept\`, `logs\`, `install.lock`, `install.json`, and the pointer beside the exe),
never through a link, never a folder that has no `install.json` of the setup's own, a drive, a link, the user's
profile or one of its known folders or a folder holding them, never while a setup or the game runs from the folder,
and never the game's folder of an install made "where it is". Anything else the player put in the folder stays,
and so does the folder. Saves and shader storage (`Documents\mcla`) are not touched.

### Developing and testing the wizard

None of this changes what a player's run does; the window shows a banner (*Development options are in use*, at
the top of every page) whenever one is in use.

- `MCLA_RELEASE_SOURCE=<folder>` or `--release-source=<folder or file:// URL>`: `release.json` and the assets
  come from a folder (a downloaded draft; tool archives sit there too, and are used instead of downloads);
  `dev-source.json` in it, `{"gitUrl": "<a local repository>"}`, replaces the clone's URL (a draft has no tag
  on GitHub; make one in a local clone). An `http(s)` address is refused.
- `--install-dir=<folder>`, `--game=<.iso or folder>`, `--in-place`, `--auto` (go through the pages without
  clicks), `--quit-after=<seconds>`, `MCLA_DEV_SHOTS=<folder>` (a PNG of the window at each page and every
  20 s, to look at what nobody is watching).
- `MCLA_WIZARD_GAME_ARGS="--user_data_root=... --cache_root=..."`: options for the game, which end up in the new
  install's `settings.json` (`ExtraArguments`) and are used for the shader preparation, so that a test never
  touches the real saves or shader storage.
- `launcher\dev\wizard-check` runs the engine with no window: `xdvdfs`, `verdicts`, `copy` (with a cancel and a
  resume), `release`, `names`, `downloads` (a wrong hash, a wrong size, an archive that leaves its folder),
  `archive`, `nonhttps`, `budget`, `mode`, `lock`, `orphan` (a child in the job dies with its parent), `retry`,
  `remaining`, `git` (Installed mode without git on the PATH), `update-status`, `update-run [--kill-after <s>]`, `rebuild`, `channel` (Installed mode, from a
  copy of the harness with the install's `install.json` beside it: what an update would be, doing it (and ending the process in
  the middle of it), a plain Rebuild, and the channel), `cleanup`, `cleanup2`, `uninstall`, and
  `install <release folder> <install folder> <game, or "adopt"> [--in-place] [--args "..."]`.
  Everything runs on scratch folders with the game's files only read.

## Updates of a wizard install

An install made by the setup wizard (Installed mode) doesn't follow `main`; the launcher tells the two apart by
`install.json` beside its exe (a clone has none, and is updated exactly as before). In *release* mode, the default:

- At start (and *More > Check for updates*) the launcher reads the latest release's `release.json` from GitHub, fetches
  that release's tag into the checkout and checks it is the commit `release.json` names. The commits between the
  checkout and the tag are the *What's new*; **Update and rebuild** appears when there are any.
- **Update and rebuild**, in this order, so that a stop, a crash or a power cut at any point leaves something the next start
  can finish or build:
  1. Room is checked (the archives, what they unpack to, and the build), then the tools and the SDK bundle the release names
     are downloaded and checked (SHA-256, size) when they differ from the ones in the install. Nothing in the install has
     changed yet.
  2. The new tools are unpacked into `tools\.staged`, where no build looks (`env.ps1` takes the newest folder of `tools\` that
     matches a tool's name), and the record of `tools\` still names the old ones: until the checkout has moved, a build must use
     the tools the old checkout and its SDK were built with.
  3. The update is written into `install.json` (`pending`, with the unpacked tools). If an earlier update was waiting for its
     build, its record comes back when this one fails before its move.
  4. The checkout moves to the tag (`git merge --ff-only`, or a checkout when going back from `main`). What codegen rewrites
     (`mcla_manifest.toml`, `generated\rexglue.cmake`) is put back first so that nothing is stashed; a change someone made in
     either is saved in `kept\codegen-edits` (and the log says so). A merge that leaves conflicts is a failure.
  5. `build-options.json` names the new SDK bundle.
  6. The new tools are moved into `tools\` and recorded as the tools in use (the record, `tools\.mcla-tools.txt`, is written beside
     and renamed over), their archives go, and the folders of the tools they replaced are removed (renamed to a temporary name
     first, so that a file held open leaves the folder whole). Only a folder the record replaced with another folder of the same
     tool is removed: a record that is lost or cut short deletes nothing. If the build
     folder was configured with one of those (its `CMakeCache.txt` names the CMake, Ninja and compiler that made it), its
     `CMakeCache.txt` and `build.ninja` go first and the next build configures it again: no build, now or later, runs with an old
     tool or breaks for lack of it.
  7. `build-game.ps1` runs with `-Toolchain mingw -ToolsDir -SdkBundle -SdkBundleSha256`: `make_pc.ps1` unpacks the bundle when it
     is another one (the SDK is swapped, not patched and built again), checks it is this checkout's, and Ninja rebuilds only what
     changed.
  When the build worked, `pending` is cleared and the record names the release; older bundle zips are removed. A stop leaves a
  checkout, a bundle and tools that belong together (before the move: all old). What goes wrong after the move (a file of an old
  tool held open, say) does not stop the update: it is tried again when the build is over, or at the next start (the build that
  went on in between was made with the old tools; the next one configures the folder again). A start that finds `pending` (when no update, build or
  setup is running in the folder) looks at it: if the checkout never moved, the record is dropped and the unused new tools removed;
  if it did, steps 5 and 6 are finished, and when what was built is still current (a release that changed only the launcher or the
  documentation) the update is complete; otherwise the Rebuild that is offered completes it. Every build the launcher starts for an
  install takes the folder's lock (`install.lock`) and ends with the launcher (the job object), so no two overlap and none outlives
  a crashed launcher.
- The shaders are prepared again only when something the game is made of changed (files outside `launcher\`, the documentation
  and the release's machinery, a new SDK bundle or tool). A release that changes only the launcher or the documentation takes
  seconds. The player's `mcla.toml`, `settings.json`, saves and shader storage are not touched (they are not in git).
- The move is made by the launcher, not by `build-game.ps1 -Ref`: the script of the install being updated is the old one,
  which has no `-Ref`; the new script comes with the checkout. `.\build-game.ps1 -Ref <tag>` (or `-Ref origin/main`) does
  the same step from a terminal (an edit of the two codegen files is saved in `kept\codegen-edits` beside an install, else in `out\kept-edits`).
- **The launcher itself** is not swapped by the launcher: it can't know what else is in its folder (the zip is flat and
  "unpacked anywhere"); `MCLA-Setup.exe` does it ("The installer", below). When the release's launcher (`release.json`'s
  `launcher.sourceCommit`) is not the one running (its `source-commit.txt`), a bar says so, and its button depends on how this
  launcher got where it is (`Setup\LauncherUpdate.cs`):
  - **Installed by the setup** (`installed.txt` beside the exe): **Update the launcher** downloads the release's `MCLA-Setup.exe`
    (SHA-256 and size from `release.json`'s `installer`; copied from the folder with a development release source), starts it with
    `--update --wait-pid` and quits. The installer replaces the folder and starts the new launcher, which writes the file named by
    `MCLA_READY_FILE` five seconds after it is up (`App.cs`); without that file the old launcher is put back.
  - **Not installed by the setup** (unpacked from the zip, or an older wizard install): it can't be replaced in place, so **Move to
    the new launcher** runs the installer as an install with this launcher's `install.json` as the pointer (`--import-pointer`):
    the new launcher is in `%LOCALAPPDATA%\Programs\MCLA`, with shortcuts, on the same install; the old folder stays for the
    player to delete. Later updates are the first kind.
  - A release with no `installer`: the button opens the release's page.

*More > Latest changes (main, untested)* (and back, *Follow the releases again*) is kept in `install.json` (`channel`), changed
under the folder's lock. In *main* mode the same bar shows main's new commits; updating moves to `origin/main` and builds with the
SDK the install has, so it is refused (with the reason) when `main` changes `patches\*.patch`: an install made by the setup has
the SDK that came with its release and can't build one. Going back to the releases checks the release's tag out again.

## The installer (MCLA-Setup.exe)

The one file a player downloads. It is a small native program (`installer\`, C# compiled with NativeAOT: 2.4 MB, no
.NET runtime, starts at once) with the launcher's whole folder appended to it, compressed (Brotli): 55 MB for the
175 MB launcher folder. **No administrator rights, no registry, nothing outside the user's own folders.**

```
MCLA-Setup.exe                      install (or repair) the launcher for the current user, then start it
MCLA-Setup.exe --update --wait-pid N --dir <folder>
                                    the same, for a launcher that updates itself (below)
MCLA-Setup.exe --uninstall          remove the launcher and its shortcuts (not the game's install, not the saves)
```

What a plain run does: unpacks into `%LOCALAPPDATA%\Programs\MCLA` (a sibling folder first, each file checked against
the SHA-256 stored with it, so a damaged download is found before anything is touched), swaps it in, makes
`Midnight Club Los Angeles.lnk` in the Start menu and on the desktop, and starts the launcher, which is the setup
wizard on a PC with no install. A progress window (a Task Dialog) shows while it works; errors are message boxes;
the log is `%TEMP%\MCLA-Setup.log`.

**Why an installer and not one self-contained exe.** A single-file publish of this launcher was tried first and
does not give one file. The Windows App SDK 2.x supports `PublishSingleFile` (with `EnableMsixTooling`; it extracts
every file, managed and native, to `%TEMP%\.net\MclaLauncher\<hash>` on the first start and runs from there: 96 MB
compressed, 219 MB in the temp folder, again for every version), but Reactor loads its `ReactorApplication.xaml` and
`.xbf` with `ms-appx:///`, which an unpackaged app resolves next to the exe, not in the extraction folder: the exe
alone ends with 0xC000027B (a XAML parse failure) at start, and works only with `Reactor\Hosting\` beside it. Also
`AppContext.BaseDirectory` becomes the temp folder, where `install.json`, the icon and `source-commit.txt` are not.
An exe that has to write two files beside itself (in Downloads, maybe read-only) and keeps a 219 MB copy per
version in `%TEMP%` is worse than a folder that is unpacked once where it belongs, which is what this does.

**Where things live.** The launcher is a folder, `%LOCALAPPDATA%\Programs\MCLA`, always writable: the wizard's pointer
`install.json` goes next to `MclaLauncher.exe` there (the case of a read-only folder beside the exe does not come up for
an installed launcher). `installed.txt` in it says which setup made it (`version`, `tag`, `commit`, when), and
`installed-files.txt` lists the files the setup unpacked.

**What the installer may touch.** Never anything it did not make. A folder is the launcher's only when it has `installed.txt`:
anything else that is not empty is refused (an unpacked zip next to the player's files, a folder the player chose by mistake), for
install, update and uninstall alike. An update refuses a folder that holds anything the setup did not unpack, at any depth (the player's install
put inside it, a disc image, notes, a file in one of the launcher's own folders: it names them), and any folder whose `install.json`
names an install inside it, because the swap deletes the old folder (the launcher looks at the same things before it quits, so
that the reason is in its bar and it keeps running); an uninstall removes only the files in `installed-files.txt` (or the payload's, for a folder made before
there was a list) and the folders then empty, and leaves the rest, and the folder with it. It does not follow a link: a folder of the payload
that the player replaced by a junction is left as it is, and nothing is deleted behind it. Windows Explorer's own `desktop.ini` and
`Thumbs.db` are not counted as the player's files (an uninstall removes them), and the list of what is foreign stops at 200 entries. The leftovers of a swap are told by an
exact stamp (`MCLA.old-20261024-231500-1234`) and Setup's marker inside (`installed.txt`; `.mcla-staging` in a folder being
unpacked; `.mcla-download` in the folder of downloaded installers), so a player's folder with a similar name is never deleted or put
back as the launcher. It refuses a drive's or share's root, the user's own folders (profile, Desktop, Documents, Downloads, Windows,
Program Files, AppData, `Programs` itself) and any folder that holds one, a junction or link, any folder inside a fork checkout (a
folder with `build-game.ps1` above it), and the folder it runs from. The shortcut names are the ones `build-launcher.ps1` uses for
the launcher built from source: a shortcut file of that name is replaced only when it starts this launcher; one that starts
something else, can't be read, or whose program is gone is **never replaced**. An update renews the shortcuts that are there and
never makes one. That is what keeps the launcher built from source (Classic) and its desktop shortcut safe from it.

**Updating the launcher** (`SelfUpdate.cs`; the launcher's update bar, `Setup\LauncherUpdate.cs`, calls it). A running program
can't be replaced, so another program does it: the launcher downloads the new `MCLA-Setup.exe` (a release asset; its SHA-256 and
size are in `release.json`, `installer`) with `SelfUpdate.DownloadAsync` (https only, exact SHA-256 and size, three
tries for network trouble, nothing left behind when it fails), starts it with `SelfUpdate.HandOver` and quits. The
installer waits for that process to end, unpacks the new launcher beside the folder, carries `install.json` and
`settings.json` (and any `--keep <name>`) over, and swaps the two folders with two renames (`FolderSwap`: atomic, so
a crash between them is repaired by the next run; a second rename that fails puts the old folder back, retried for as long as
that takes, and the staged folder is only deleted when the old one is confirmed back; its marker stays until it is the launcher's folder, so
a kill in between leaves a folder the clean-up knows; the message then says where this installer is and where to get it again). It then starts the new launcher and waits
until it has come up: a window that is still there 3 s later, or, with `--ready-file <path>`, the launcher is given that path in
`MCLA_READY_FILE` and writes the file from its window's own thread five seconds after the window opened (`App.cs`), and it
must also have a window (`SelfUpdate.HandOver(..., readyFile)`). The wait is 90 s (a virus scanner can hold the first start of 480
new files). If the new launcher ends or does not come up, the folders are swapped back (never when the old folder is gone: the
new one then stays), the old launcher is started again and the installer exits 1 with a message (a first install has nothing to
go back to: its files stay, and the message says so). If the update fails or is refused before the folders change, the old launcher is started
again too (the launcher that quit to hand over would otherwise leave the player with nothing open). Only one setup runs at a time. What is left (`MCLA.old-*`, `.failed-*`, `.new-*`, the downloaded installer) is deleted at
the end of an update, or by the launcher at a later start (`SelfUpdate.CleanUpWhenSetupIsDone()`, which waits while a Setup runs:
its mutex `Local\MCLA-Setup`, so the folder a failed update goes back to is never deleted under it). A folder that is held open by
something else is waited for (60 s) and, if it stays held, nothing is changed. A launcher that was not installed by Setup (unpacked
from a zip) moves into Setup's folder with `SelfUpdate.MigrateTo` (`MCLA-Setup.exe --import-pointer <its install.json>`: the new
folder gets that file as its pointer unless it has one), so that it finds the same install; the old folder is left for the player to
delete. When Setup's folder already holds a launcher of its own (with an install of its own) nothing is moved: the bar says so and
offers that folder.

**Building it.** `scripts\ci\build-installer.ps1 -LauncherDir <what publish-launcher.ps1 made> -Out <folder>` (the release
workflow does exactly this): publishes `installer\MclaSetup.csproj` with NativeAOT (needs the .NET 10 SDK and the Visual
Studio Build Tools' C++ tools, which the CI runner has), appends the payload (`MCLA-Setup.exe --compose`), and checks
the result by unpacking it again and comparing every file. `scripts\ci\smoke-installer.ps1` installs, reinstalls and
removes it on scratch folders. Unsigned (see the README).

**Testing it.** `dotnet run --project dev\update-check -c Release -- all <the bare installer program>` (CI runs it; the
bare program is `build-installer.ps1 -StubOut <file>`): checks of the folder swap (replace, carry over, the way back and the way back
with the old folder gone, a held file, a staged folder that can't be moved in, a swap killed half way, leftovers and lookalike
folders, what a folder holds, the launcher's clean-up waiting for a Setup, junctions), of the verified download (a local server:
wrong hash, wrong size, cut short, stalled, 500, cancelled, refused addresses, a local copy; `-- download-loop 50` repeats them) and
of the real installer with fake launchers (a system program stands in for `MclaLauncher.exe`): fresh install, update that waits for
the old launcher, a new launcher that ends at once (rolled back; a first install keeps its files), the ready-file protocol, a
pointer to import, an older installer, a damaged file, a held file, an interrupted swap, two setups at once, a folder without
`installed.txt`, the player's install inside the launcher's folder, a junction, a drive's root, the profile folder, a failed update
that starts the old launcher again, a payload with more bytes than its header, unreadable shortcuts, refusals, uninstall.
They use scratch folders under `%TEMP%` and never the desktop or the Start menu.

`dev\update-check\bar-e2e.ps1 -Stub <the bare installer> -Launcher <a published launcher folder> -ReleaseJson <a release.json> -GitUrl <its repository> [-Way InPlace|Move|Other] [-Session]`
(a desktop session, UI Automation; not in CI) runs the real launcher's update bar end to end with no game: an older launcher installed by
Setup (or unpacked as a zip), a local release source that names a newer launcher and holds its `MCLA-Setup.exe`, a click on the bar's
button, and what must be true afterwards (the old launcher quit, the new one runs from the same folder, or from Setup's folder with the
same pointer, the version, no leftovers, no bar, the download cleaned up). The old and the new launcher are copies of one build that
differ in `source-commit.txt`, so it tests the mechanism, not a code change.
`-Session` (InPlace) first runs a realistic session in the old launcher (a setting changed, More > Check for updates, the channel
switched to main and back, a build started and stopped, the window closed and the launcher opened again) and lists every file in the
launcher's folder that the setup did not unpack: the update that follows refuses a folder that holds any, so a launcher that wrote
beside its exe in normal use would never update. None does (485 files: the payload's 482 and Setup's three).

## The game's settings

The drop-downs are the game's own settings, in `mcla.toml` next to `mcla.exe`
(`out\build\win-amd64-release\`): the same file and values as the Settings tab
of the game's pause menu (DISPLAY, PERFORMANCE, CONTROLS), so each shows what the
other set. A drop-down shows the file's value, or the game's default while the
file doesn't set it; a value set by hand that isn't one of its choices shows as
"... (set in mcla.toml)". A change is written at once, the way the game writes
(`rex::cvar::SaveConfigValues`, from `patches\rexglue-cvar-toml-strings.patch`):
the key's `key = value` is replaced where it is, keeping a comment after it, or
added before the first `[table]`; every other line is kept (comments, keys it
doesn't know, tables, values over several lines, a BOM, CRLF), and the file is
written beside the old one, flushed to the disk and renamed over it
(`src\GameSettings.cs`). A file that isn't UTF-8 text is left as it is, with
a message, rather than rewritten; a value it can't read, such as a broken
escape, shows as set by hand.

While the game runs (started here, from its shortcut, or again by its own
RESTART GAME; any `mcla.exe` counts, checked every two seconds) the drop-downs
are greyed out with a note to use the pause menu. The game reads the file only
when it starts, and its pause menu writes it too, so edits here would neither
reach the running game nor be safe from being overwritten. When it closes, the
file is read again and shows what was changed in the game.

The launcher passes only what is typed under *Advanced*, **More mcla.exe
options**; an option there wins over `mcla.toml` and the pause menu at every
start, also after the pause menu's RESTART GAME, which starts the game again
with the same options: `--resolution_scale` there overrides the pause menu's
RESOLUTION. It no longer passes `--vsync=false`: the game turns vsync off by itself
unless the pause menu's VSYNC (or `vsync = true` in the file) turns it on, and
with it on the GPU plugin's emulated 60 Hz vblank caps the game at 60 and paces
it unevenly. (An earlier note here said vsync set from the game never reached
the GPU plugin; rechecked on 2026-10-16, it does, at start and from the pause
menu.)

Launchers before 2026-10-16 passed every choice on the command line at every
start, which beat what the pause menu saved. The first time this one opens it
moves those choices into `mcla.toml`, once (`SettingsMigration` in
`src\GameSettings.cs`):

1. it keeps a copy of `mcla.toml` as `mcla.toml.before-launcher-settings`, next
   to it, before writing anything (an empty file when there was no
   `mcla.toml`), and takes the copy back if the write fails, to try again at
   the next start;
2. it writes each choice that was forcing a value: every one but "Game setting",
   and Resolution and HUD updates, which had no "Game setting"; and
   `vsync = false` with any frame rate, which came with `--vsync=false`;
3. it deletes nothing, and drops the old choices from `settings.json`, which
   then holds `SettingsInGameFile: true`.

It runs only once: not when the copy already exists or the flag is set, and
not from a `settings.json` it can't read (whose choices are unknown; it is
kept as `settings.json.unreadable` before the defaults replace it). A choice
made in the launcher before then also ends it. It doesn't run while the game
is running, which writes the file itself: it waits until the game has closed.
A new install (no `settings.json`) has nothing to move. The old HUD updates
choice (`HudUpdates`, an on/off switch) isn't carried over; the new one is a
rate, 60 per second unless set.

If `settings.json` can't be saved, the launcher says so instead of carrying on
as if it had.

*HDR*, *HDR highlights*, *HDR peak brightness* and *HDR paper white* are the
game's HDR output (`hdr`, `mcla_hdr_highlights`, `hdr_peak_nits`,
`hdr_paper_white_nits`; the pause menu's DISPLAY rows of the same names; all
change during play; *HDR* is *Auto* and *Highlights* Brighter unless
`mcla.toml` says otherwise, the game's defaults on Windows). The peak and paper
white are sliders with an *Auto* box, in the pause menu's steps (200 to 4,000
nits by 50, 80 to 480 by 10): while *Auto* is ticked the slider shows what
Windows reports and can't be moved, and unticking it starts from that value.
A drag is written to `mcla.toml` when it ends, an arrow key or the controller's
left and right one step at a time. The peak never goes below paper white: a
value that would cross it stops at the last step that doesn't, and the other
one isn't moved. A value typed into `mcla.toml` between the steps is shown as
it is, and a step goes from it to the next step that way. Below them the
launcher shows what Windows reports for the main display when it opens
(DXGI's `IDXGIOutput6::GetDesc1`, the peak the Windows HDR Calibration app
measured, and DisplayConfig's SDR white level), as *Auto* would use it: 1,000
and 200 nits if it reports nothing usable. In the game, *Auto* reads the
display its window is on, which may not be the main one (`src\DisplayHdr.cs`).

Every setting has an "i" next to its title (the game's settings and the
Advanced options): hovering shows what it does and which choice suits when, and
clicking it, or Enter or the controller's A on it, shows the same text in a
flyout. The texts are in `src\GameSettings.cs`, checked against the code and
the measurements in these notes.

*Display gamma* (`mcla_display_gamma`) picks the gamma ramp the picture goes
through on its way to the screen, and it switches during play too (the pause
menu's DISPLAY > DISPLAY GAMMA). *TV* (`tv`, the default) is the console's ramp
for an HDTV (BT.709), as before; *PC monitor* (`pc`) is sRGB, which leaves the
game's own sRGB-encoded output as it is, as a PC monitor expects. On a PC
monitor the TV ramp darkens the shadows: 8-bit levels 64, 128 and 192 come out
as 48, 115 and 185. In a night scene the average brightness went from 46 to 60
of 255 with *PC monitor* (`KNOWN-ISSUES.md`).

*Anti-aliasing* (`mcla_anti_aliasing`) is the game's 2× MSAA (`msaa`, the
default), AMD FSR 3.1 at native resolution (`fsr`) or none (`off`); all three
switch during play too (the pause menu's PERFORMANCE > ANTI ALIASING). Off, a 3×
frame is 2 to 5% shorter than with MSAA and single-tile rendering (2.1 and 3.1%
in the city by day on D3D12 and Vulkan, 2.7 and 5.3% in the race), for slightly
more jagged thin edges such as pylons and billboard frames; the dithered foliage
looks cleaner. Against MSAA in two tiles it was 8 to 18%
(`dev\single-tile-2026-10-16.md`). FSR blends each frame with the ones before
it, which also calms the shimmer MSAA leaves (fences, power lines, trees, the
shine on cars); its pass takes about 1.4 ms of GPU time a frame at 3× on an
RTX 5080 (about 1 ms more than MSAA, which is off with it), and it can leave
faint trails behind fast-moving things, rain and smoke. *FSR sharpness*
(`temporal_aa_sharpness`, 0.2 by default) is its sharpening. FSR is offered
only when its runtime for the graphics API in use was built
(`amd_fidelityfx_dx12.dll`, `amd_fidelityfx_vk.dll` next to `mcla.exe`; the
Vulkan one needs the Vulkan SDK when the game is built); where it isn't there
or fails, the game uses MSAA. An older `mcla_msaa = false` still means off while
`mcla_anti_aliasing` isn't set.

*Frame generation* (`frame_generation`, off by default) is AMD FSR 3.1's: one
generated frame between each two the game renders (the pause menu's DISPLAY >
FRAME GENERATION; it switches during play on both graphics APIs). FSR 3.1 is
offered only where its runtime for the graphics API in use was built
(`amd_fidelityfx_dx12.dll`, `amd_fidelityfx_vk.dll` next to `mcla.exe`, as for
anti-aliasing); a file that has it on without the runtime shows "FSR 3.1 (not
built for this graphics API)", and the game says why in its menu. While it is on,
the *Frame rate* setting is replaced by *Frame rate with frame generation*
(`mcla_fps_frame_generation`; the same row of the pause menu, FPS TARGET): *Half
display* (-1, the default: floor of 97% of half the refresh rate Windows reports
for the main display, 116 FPS on 240 Hz, shown as 232), 30 to 120 FPS in
rendered frames (60 shows 120), or *Uncapped*. A cap above half the display
adds latency (its tip says so); the game's own limits are in
`patches\README.md`. The old *Frame rate* (`mcla_fps`) stays as it is in the
file and is back when frame generation is turned off. On a main display under
100 Hz *Half display* isn't offered (half would be under 50 rendered), so
*Frame rate* stays whichever the setting, and the tip of *Frame generation* says
it is for 120 Hz or more, or G-Sync or FreeSync. The refresh rate is read when
the launcher opens (`DisplayRefresh` in `src\DisplayHdr.cs`); the game reads the
display its window is on, so on another display the pause menu's list may differ.
`dev\shaderpack\launcher-check` has a `settings` mode that prints the Graphics
rows for a file and a refresh rate after writing `key=value` pairs as the page
does, to check the round trip and that a file from before this setting stays
byte for byte as it was.

*NVIDIA Reflex* / *AMD Anti-Lag 2* (`low_latency`: Default, Off, On, and On +
Boost on a GeForce) is the GPU maker's latency reducer, named by the GPU the game
will most likely use (the adapter the game used at its last start, from its shader
record, `ShaderStamp.LastAdapterVendor`, read again only when the file changes, and
only while an adapter of that vendor is still in the machine: a swapped card or a
removed external one leaves the old vendor in the record until the next start;
before the first start the first adapter Windows lists, `GpuVendor.First` in
`src\DisplayHdr.cs`, and on a laptop with two GPUs the game may pick the other;
"Low latency" for any other GPU, where it does nothing).
Default is on for NVIDIA Reflex and off for AMD Anti-Lag 2 (untested), and says
so in the list ("Default (on)"). The tip states the price next to the gain: about
4% of the frame rate at 3x on Direct3D 12 (none on Vulkan or at native
resolution) for 13 to 15 ms less from the game's swap to the screen. Frame
generation turns it on whatever the setting says (the pause menu's row reads ON
(FRAME GEN), and the tip says so; `low_latency_frame_generation`, which only
`mcla.toml` has, turns that off). A value the list doesn't have (`boost` in a
file used on a Radeon) is shown as written. `launcher-check`'s `settings` mode
takes `LAUNCHER_CHECK_VENDOR` (hex: 10DE NVIDIA, 1002 AMD, 8086 Intel) for the
vendor. `devrun.py --presentmon --pm-pcl` makes PresentMon track the PC latency
(the markers' PCLStats events); `presents.py` reports each frame's time from the
engine timer pass (where the game reads its input) to the screen next to the
swap's, and the simulation step against the display interval (the judder), from
the `f` lines the dev pad writes at the start of every frame (`frames.csv`: the
start, the step in microseconds).

*Single tile rendering* (`mcla_single_tile`, on by default) draws the scene with
anti-aliasing on once instead of in the Xbox 360's two tiles, in the larger
EDRAM (4,096 tiles instead of the console's 2,048) the game asks the GPU plugin
for on PC (`edram_tiles`). It switches during play (the pause menu's PERFORMANCE
> SINGLE TILE RENDERING). Against two tiles, a 3× frame is 9 to 11% shorter in
the city by day and 15 to 20% in the race, the GPU command thread uses 28 to
35% less CPU time, and at native the race is 14% (D3D12) to 25% (Vulkan) faster;
with a 120 FPS cap at 3× in the city, 99% of frames are within 1 ms of the target
instead of 93 to 94% (`dev\single-tile-edram-2026-10-16.md`). Off, the scene is
drawn as on the console. With `--edram_tiles=2048` there is no room for one tile:
the pause menu's row says UNAVAILABLE and nothing changes either way.

*Graphics preset* (`mcla_preset`: `custom`, the default, or `low`) is a starting point for a weak
graphics card: Low-end makes anti-aliasing and the full-screen blur default to off, the far
sun shadows (`mcla_far_shadow_interval`) to being drawn every 2nd frame and the
environment map of the cars (`mcla_env_map_interval`) every 4th, the picture of the last
time kept in between; an option set by hand (the Anti-aliasing setting here, the pause
menu's PERFORMANCE rows, `mcla.toml`) keeps its value. On the integrated Radeon of the
Ryzen 9800X3D (Direct3D 12, native resolution) the passes and the blur are worth 1.6 ms
of 36.8 and anti-aliasing off another 2.5 to 2.9 ms (29% at 2x), 11 to 12% together
(free roam 36.8 to 32.3 ms, driving 35.8 to 31.7 ms; `dev\gpu-weak-2026-10-27.md` has
each option's saving and what was left out: the
reflection view every 2nd frame saves 0.3 ms, too little for a reflection that lags).

*Graphics API* picks the GPU plugin's backend (`mcla_gpu_backend`: `any`, which is
Vulkan on AMD's integrated graphics and on Intel graphics and Direct3D 12 on AMD graphics
cards and NVIDIA, `d3d12` or `vulkan`). The setting marks the one Automatic picks for
your GPU "(recommended)", as does the pause menu's row, so that the default is not ignored.
Every setting and SDK patch here works on both. On the RTX 5080 of this machine, in
scripted runs, Vulkan matches D3D12 at native resolution; at 3x the two are within a
few percent in the city (Vulkan slightly ahead in free roam), and D3D12 is 10 to 15%
faster in the race benchmark (`dev\direct-resolve-2026-10-15.md`), so NVIDIA stays on
D3D12. On the Ryzen 9800X3D's integrated Radeon Vulkan is 6 to 10% faster at native
resolution (35.7 against 39.5 ms a frame in free roam, three runs each; 2.6% at 2x) and
the GPU time per frame is 6 to 9% less, so Automatic takes it there. An integrated AMD GPU
is one that Direct3D 12 says shares the CPU's memory (the architecture's UMA), whatever size
of memory the BIOS sets aside for it; a Radeon graphics card is not one: a user's RX 6600 XT
with an i5-10400 was 20 to 25% faster on Direct3D 12 (fork issue #7: Vulkan's GPU command
thread costs more CPU), so cards stay on Direct3D 12 (not tried here: no AMD card was
available). On Intel graphics Direct3D 12 uses the SDK's slower rasterizer-ordered-view
path for the render targets, which Vulkan does not need, so Automatic takes Vulkan there
too (not measured: there was no Intel GPU to try it on). The GPU is the one Direct3D 12
would use (`d3d12_adapter`, else the first physical adapter), and the choice needs
`vulkan-1.dll` and a Vulkan device that is that GPU (the game points `vulkan_device` at it);
picking an API here, in the pause menu or with `--mcla_gpu_backend` skips all of it
(`dev\gpu-weak-2026-10-27.md`). A shader cache prepared before this change was made for
Direct3D 12: the launcher sees that Automatic now means Vulkan on AMD's integrated and
Intel graphics and prepares the shaders again before the next Play. Since D3D12's textures are placed in
shared heaps, both are about as smooth in the map zoom-out after the title
(`dev\d3d12-zoomout-2026-10-15.md`).

*HUD updates* (`mcla_ui_hud_rate`) sets how many times a second the HUD's
Flash movies (the gauges, the song ticker, the LOADING badge) take a step: 60
(the default since 2026-10-16, the user's choice after playing at 30 and 60), 30
(as on the console), 120, or every frame (0). The ticker scrolls one step at a
time and the needles move only when it steps, so at 30 the ticker scrolls at its
original speed and the needles move in 30 Hz steps; 60 and 120 make the needles
smoother and the ticker two and four times as fast (at a 120 FPS cap: 59, 118,
237 and 239 px/s at 720p for 30, 60, 120 and every frame). Menus, pop-ups and the
GPS panels keep the console's pace whatever it is set to (`mcla_ui_timing_fix`,
`KNOWN-ISSUES.md`).

The launcher also used to pass four SDK settings that measured faster (in-run
A/B; the first two are ideas from
[ForzaHorizon1-ReImagined](https://github.com/Dyotson/ForzaHorizon1-ReImagined)).
They are now the game's own defaults on PC (`MclaApp::ApplyPcDefaults` in
`src\mcla_app.cpp`), used unless the command line, an environment variable or
`mcla.toml` sets them:

- `clear_memory_page_state=false`: guest memory already uploaded to the GPU
  stays valid from one frame to the next instead of being copied again every
  frame (2.5 → 0.8 GB/s of uploads; 10 to 21% less GPU command thread time at
  Native, uncapped, depending on the scene: an older estimate with no dated
  measurement behind it, to be measured again). mcla-recomp's PS5 port runs this
  way too.
- `execute_unclipped_draw_vs_on_cpu=true` (Xenia's default): draws with
  clipping off no longer claim the whole EDRAM, which saves render-target round
  trips (about 11% faster at 3×; no change at Native: also an older estimate, to
  be measured again).
- `shared_memory_hot_page_faults=4`, `shared_memory_hot_page_ms=10000` (hot
  pages, as mcla-recomp's PS5 port runs): a page the game keeps rewriting is
  uploaded whenever a draw uses it instead of being write-protected again after
  every write fault. With the profiling notes' fast-path patch, write faults go
  from about 12,000 to about 200 a second and protection changes from about
  190,000 to 1,000-2,000; frames on the title screen at Native, uncapped, were
  about 9% shorter (6.87 against 7.57 ms, in-run switch every 20 s, both halves
  agree). Needs `rexglue-hot-pages-watched-ranges.patch`: without it, textures
  the CPU rewrites (the attract movie's colour planes) stopped updating.

To change one, type it under *More mcla.exe options* (e.g.
`--clear_memory_page_state=true` if geometry or textures ever look stale, or
`--shared_memory_hot_page_faults=0` to turn hot pages off). mcla.exe's
command-line parser doesn't take an option twice (CLI11 rejects a repeated
option by default; a boolean given twice adds up).

Fixed interaction (2026-10-14): uncapped with the intro at "Normal", about one
frame in four took 33 ms. mcla-recomp holds loops that never reach the frame
timer (start-up logos, loading screens) to 30 FPS, and took any single swap
without a timer pass for one; uncapped, the game's update and presenting threads
drift against each other and produce such swaps in normal play. The hold now
needs two in a row (`KNOWN-ISSUES.md`, "Loading-screen pacing"). The
"Fast" and "Skip" intros now speed up only the start-up logos, not loading
screens later.

## Preparing shaders

The launcher runs `mcla.exe --mcla_prepare_shaders=true` with the options it
starts the game with (*More mcla.exe options*; the rest comes from `mcla.toml`,
as when playing):

- after a build or update that succeeded;
- before Play, when the game's record of its shader storage (the stamp,
  `<cache>\shaders\local\545407F8.stamp.json`, written after every start and every
  preparation) says the shaders aren't compiled for the next start: the
  graphics driver was updated or the card changed (the display adapters'
  drivers, as DXGI reports them), the API or the resolution scale the game will
  start with differs from what the last start was asked for (not from what
  the game ended up with: it clamps a scale a weaker GPU can't do, and falls
  back to another API when a plugin doesn't load), the shader pack changed
  (the one `--mcla_shader_pack` names, if you pass one; with
  `mcla_shader_pack_auto = false` only after a preparation), the storage files
  shrank or vanished, or the last start ended while it was adding the pack's
  shaders. Relative paths in the options are the game's folder's, as for the
  game, and `cache_root` and `user_data_root` are read from the options and the
  `REX_` variables only: the game reads them before `mcla.toml` is loaded.
  It doesn't notice a graphics API left on "any" that the game resolves
  differently, a switch between two adapters that both stay installed, or a
  storage format change without a new pack (that one no preparation fixes).
  The check (`ShaderStamp.cs`) reads a few small files and lists the display
  adapters, in a few milliseconds, and never starts `mcla.exe`; when it finds
  nothing, the game starts at once. The status line says why it prepares, and
  the game starts when it's done, whatever the result;
- from *More* › **Prepare shaders**, which also tries again a pack whose
  import was turned off after it stopped the game's start twice.

The run:

1. takes the storage's session lock (exit code 7, *in use*, when a game holds
   it: tried again at the next Play);
2. undoes an import the game's last start didn't finish (the journal,
   `<cache>\shaders\local\545407F8.import`);
3. checks the shader pack (`shaderpack\545407F8.shaderpack`) against the game's
   files and the SDK's storage versions, rebuilds from `game\` the shaders the
   storage lacks and adds them, with the pack's pipeline descriptions, to the
   shader storage (`<cache>\shaders\shareable`), keeping what the files already
   hold; each file is written beside the old one and renamed over it;
4. creates every stored pipeline for the current API and resolution, as a start
   of the game does, so the graphics driver compiles them now, then closes the
   storage (a driver pipeline cache is saved) and writes the stamp;
5. quits, before the game would start; its window stays hidden, and a crash
   ends it without an error dialog.

The status line shows its progress, read from the lines it prints
(`SHADERPREP <step> ...`, `ShaderPrep.cs`). Its log is
`logs\prepare-shaders-*.log` (the last ten), and the game's own log of the run
`logs\prepare-shaders-game.log` (a `--log_file` of yours isn't touched). The
countdown of *start the game right away* and the settings wait meanwhile, and
the buttons are **Stop** (ends the run, starts nothing; Play doesn't prepare
again until the launcher is opened again or *Prepare shaders* is used) and
**Play now** (ends the run and starts the game, which adds the pack and
creates the stored pipelines by itself as it starts); the restart prompt after
a launcher update shows once it's done. Both are safe: the storage is only ever
replaced whole, and the game's next start undoes an import that didn't finish
(the interrupted run counts as a failed start in its journal, and two in a row
turn the import off until *Prepare shaders*). Closing the launcher stops it
too, and doesn't start the game; a launcher that dies takes it along (a job
object); a run that hangs is ended after 15 minutes. If the pack or the game's files
don't match, it says so and nothing is written; the game then plays as before,
compiling shaders as it meets them. A preparation, automatic or by hand, that
failed for good (not *in use*, and not ended by you or by closing the launcher)
isn't run again before every Play for the same pack (`settings.json`,
`ShaderPrepSkipPack`, which also silences the other reasons for that pack)
until *Prepare shaders* succeeds or the pack changes. Exit code 7 (*in use*) is
a sharing or lock violation, a storage file another program has open without
delete sharing or mapped (an antivirus scan, a backup, OneDrive: Windows calls
that access denied too, and the game asks for DELETE access to tell it from a
permission), or a storage lock held by a running game; a read-only file, a
permission that denies the replacement or a lock file the game can't open is a
failure for good. Without the launcher the game adds what its storage lacks
of the pack at every start anyway (`KNOWN-ISSUES.md`, "Shaders prepared before
the first start, and kept prepared").

## Rebuilding the launcher

Needs the .NET 10 SDK (`winget install Microsoft.DotNet.SDK.10`). Close the
launcher, then run `.\build-launcher.ps1`. After an update that changed the
launcher, it offers **Restart to update the launcher**, which does the same via
`update-launcher.ps1` once it has closed. `build-launcher.ps1` records the last
commit that changed the launcher in `app\source-commit.txt` for that check.

Package versions float: each build takes the newest Reactor preview and the newest
stable Windows App SDK 2.x of its WinUI component (not the whole `Microsoft.WindowsAppSDK`
metapackage, whose AI, ML, Search and Widgets parts the launcher does not use: `app\` is
99 MB instead of 159 MB; files left in `app\` by an older build stay and are harmless, delete
the folder and rebuild to drop them). Reactor is pre-1.0 and a new preview can change its API,
so if a rebuild suddenly fails to compile, check what changed in
`%USERPROFILE%\.nuget\packages\microsoft.ui.reactor\<version>\agentkit\reactor.api.txt`
(the index for the version actually used; Reactor's GitHub main runs ahead of it).

The icon is `src\launcher.ico`.

## Measurement tools

`dev\` holds what the measurements in this fork were made with; nothing reads it
during a build:

- `ab-harness.patch` (`src\perf_options.cpp`): flips cvars in-run every
  `MCLA_AB_SECONDS` (default 4) seconds, e.g.
  `--mcla_ab_test=set:gpu_regwrite_bulk=on/off`, so an A/B compares the same scenes.
  Run uncapped with `--mcla_intro=fast` and `REX_PERF_LOG_CSV`, and leave out
  the first 20 s and the attract video (about 120-131 s after launch). It also
  sets `tmp_ab_state` to the current side for SDK-side logs that have that cvar.
- `vulkan-submission-counters.sdk.patch` (SDK, Vulkan) with `cyc-ab.py`: every
  5 s a `TMPSUB` log line with the GPU command thread's CPU time, draws,
  command-buffer recording, `vkQueueSubmit` and fence-wait time per frame;
  with `TMPSUB_CSV=<file>`, one row per swap with the thread's cycle count and
  the A/B side, which `cyc-ab.py` compares per draw between neighbouring
  windows. Thread cycles measure the GPU command thread's own cost even when
  the frame rate is held by another thread.
  Stale: it no longer applies to the patched SDK, whose Vulkan submission code
  it changes has moved since.
- `draw-cost.py`: fits frame time = fixed + per draw x draws over a run's
  `REX_PERF_LOG_CSV` output.
- `hdr-2026-10-16.md` and `hdr\`: the HDR output, how it was checked, and the
  scripts: `scenes.py` (which composite each `log SCENE` marker of a pad run
  got), `restore_err.py`, `nits.py` (levels from `hdr_debug_ui 8` captures),
  `identboot.py` (boot frames of two builds) and `build_shaders.py` (the HDR
  shaders from their XeSL sources).
- `dropped-patches\`: patches that were measured and not kept: specialized
  system constant updates, sampler fetch-constant prefetch, reused texture
  request queues, skipping the clock in shared-memory requests, keeping bindings
  for fetch constants rewritten unchanged (ForzaHorizon1-ReImagined's patch 0032,
  -0.4%), the achievement-toast removal the on-demand toast replaced,
  byte-exact uploads of hot pages for D3D12 (no change in CPU time), and the
  block-by-block register writes of `rexglue-gpu-regwrite-bulk.patch` ported to
  Vulkan (`rexglue-vulkan-regwrite-walker.patch`: -0.5% +/- 0.6% GPU command
  thread time per draw on top of the patch's common part, which gives Vulkan
  its -9.1%). The PS5
  hot-page write-watch settings (`--shared_memory_hot_page_faults=4`) measured
  slightly worse on PC before `rexglue-shared-memory-fast-path-hot-pages.patch`;
  with it they take 20% off the GPU command thread (see the profiling notes).
- `profiling-2026-10-14.md`: where the time goes on the CPU at native resolution
  and on the GPU at 3x, with the instrumentation that measured it
  (`upload-counters.sdk.patch`, `gpu-timestamps.sdk.patch`, `gt-analyze.py`;
  `upload-counters.sdk.patch` no longer applies since the HDR patches changed
  the files it touches, and is kept only as a record of that measurement).
- `vulkan-2026-10-14.md`: the same settings and patches measured on Vulkan,
  where its GPU command thread's time goes, validation, pacing and stress runs,
  the render pass breaks that upload hoisting removed, the Vulkan submission
  thread, the texture copy that held Vulkan back at 3x, and a D3D12 against
  Vulkan benchmark in the city.
- `race-benchmark-2026-10-15.md`: the race benchmark (`scripts\race.pad`):
  how it gets into the race, how repeatable it is, D3D12 baselines and where
  the GPU time goes in it at 3x.
- `gpu-timestamps-vulkan.sdk.patch` (SDK, Vulkan): the frame timings of
  `gpu-timestamps.sdk.patch` for Vulkan (`MCLA_GT_EVERY`, `MCLA_GT_DIR`), also
  bracketing render pass begins and ends; applies together with the D3D12 one.
- `d3d12-zoomout-2026-10-15.md`: why D3D12 stuttered in the map zoom-out (and
  in races) and Vulkan did not, the placed texture heaps that fixed it, and
  their benchmark.
- `d3d12-hitch-timers.sdk.patch` (SDK, D3D12, `TMPHITCH`): CPU timers around the
  D3D12 calls that can stall the GPU command thread (texture creation and
  release, tile heaps, render targets, fence waits and more), one log line per
  slow frame; applies on top of `rexglue-d3d12-placed-texture-heaps.patch`.
- `texload-split-timestamps.sdk.patch` (SDK, D3D12 and Vulkan): on top of the
  two `gpu-timestamps` patches, times the steps of every texture load
  separately (the barriers, the decode dispatches, the copy into the image) and
  labels each load with its size and format; `texload-split.py <dir>` sums
  them per frame and per kind of load, with the rate of each step in GB/s. It
  found Vulkan's `vkCmdCopyBufferToImage` copying at about 120 GB/s
  (`rexglue-vulkan-texture-load-storage-copy.patch`).
- `edram-transfers-2026-10-15.md`: where the EDRAM ownership transfers came
  from (the game's clear rectangles at 4x MSAA), the change that removed most
  of them (`rexglue-clear-rectangle-draws.patch`) and its benchmark.
- `edram-transfer-log.sdk.patch` (SDK, D3D12, `TMPXF`): logs every draw that
  changes the bound render targets or causes transfers, with the state that
  decides whether it overwrites everything it covers, every
  `MCLA_XF_EVERY`-th frame to `MCLA_XF_FILE`; `xf-classify.py <log>` sorts
  the transferred samples by what triggered them. Applies on top of
  `rexglue-clear-rectangle-draws.patch`.
- `resolves-2026-10-15.md`: what the resolves and the texture reloads at 3x
  are, the direct texture loads that halved the reloads' cost
  (`rexglue-texture-load-direct.patch`) and their benchmark, and the options
  left.
- `resolve-split-timestamps.sdk.patch` (SDK, D3D12 and Vulkan, `TMPR4`): on top
  of `rexglue-direct-resolve.patch` and the two `gpu-timestamps` patches (and
  `texload-split-timestamps.sdk.patch` for the loads), labels each resolve
  (destination, size, formats, EDRAM range, MSAA, sample) and times its steps
  (the dump into the EDRAM buffer, the resolve shader, the direct resolve
  shader `rdirect`, the clear); `resolve-split.py <dir>` sums them per frame
  and per kind, with the texture loads by address.
  `texload-split-timestamps.sdk.patch` now also marks loads done by
  `texture_load_direct` (`direct=1`).
- `shader-stutter-2026-10-15.md`: the stalls while new pipelines compile
  (first sessions, new places, after driver updates), the patches that
  removed them (`rexglue-async-pipelines-no-frame-wait.patch`,
  `rexglue-vulkan-pipeline-storage-parallel-translation.patch`,
  `rexglue-vulkan-pipeline-cache-file.patch`) and the cold, post-update and
  warm sessions they were measured in.
- `shader-stutter-timers.sdk.patch` (SDK, D3D12 and Vulkan, `TMPSS`): a
  `tmp_shader_salt` setting that makes the translated shaders differ for the
  driver's cache (a cold driver cache without emptying it), and log lines for
  every pipeline created and every frame that left draws out, waited or did
  pipeline work on the GPU command thread (`coverage.py` sums these per run).
  Applies to the SDK as it is now, without the log lines of each pipeline's
  creation on the creation threads and of the Vulkan preload's creation time (their
  context moved). `devrun.py --cache-root <folder>` runs with a shader storage of
  its own.
- `shader-prepare-2026-10-17.md`: shader preparation (`--mcla_prepare_shaders`,
  `src/shader_prepare.cpp`): how the pack's shaders are rebuilt, the pack's
  contents, the preparation's time and memory, and the race benchmark with
  and without it with the driver's cache cold. Its tools, in `shaderpack/`:
  `build_pack.py` makes `shaderpack/545407F8.shaderpack` from shader storages,
  refuses anything but fingerprints, ranges of `default.xex` and pipeline
  descriptions, and accepts a pack only once `mcla.exe` rebuilt every shader
  in it (`--mcla_prepare_shaders_check`; `--previous` refuses a pack that
  lacks entries of the pack it replaces for no stated reason, and the game
  retires what a newer pack dropped: a ledger, `test_storage.py` R1 to R6);
  the pack of 2026-10-25 was grown with `coverage.py` below; `measure.py` runs the preparation
  and the race benchmark through `devrun.py`. `shader-salt.sdk.patch` is the
  salt part of `shader-stutter-timers.sdk.patch`, for a cold driver cache.
  `devrun.py --stdout <file>` gives the game a standard output, as the
  launcher does. `test_storage.py` tests the import's protections (a file
  held by another program, packs with lists this build doesn't take, the
  session lock, the rollback after a crash and the pack disabled after two)
  and the import at a start, through `devrun.py`; `launcher-check\` builds the
  launcher's shader logic without its window (`dotnet build`), to run the check
  before Play against a stamp and a preparation run against a stand-in game, and
  the settings page's rows (`settings <mcla.toml> <refresh Hz> [key=value ...]`).
- `direct-resolve-2026-10-15.md`: resolves that read the render target
  directly instead of dumping it into the EDRAM buffer
  (`rexglue-direct-resolve.patch`) at any resolution scale, and the 4x MSAA
  depth samples the EDRAM path misread
  (`rexglue-resolve-4xmsaa-depth-samples.patch`): the byte-for-byte
  comparisons, GPU time per resolve, the benchmarks and the checks.
- `direct-resolve-validate.sdk.patch` (SDK, D3D12 and Vulkan, `TMPDV`): on top
  of `rexglue-direct-resolve.patch`, a `direct_resolve_validate` setting: one
  direct resolve a frame, each in turn, is also done through the EDRAM buffer
  into the same destination (the scaled resolve buffer, or shared memory at
  native resolution), and both results are read back and compared byte for
  byte; differences, and every 50 a summary per kind (copy shader, MSAA,
  formats, samples, conversion), go to the log. With
  `rexglue-resolve-4xmsaa-depth-samples.patch` in, no difference is expected.
- `single-tile-2026-10-16.md`: why `mcla_single_tile` never took effect (no
  call passes a null render target, and the SDK's EDRAM was 2,048 tiles, too
  few for a 720p 2x MSAA tile), what one tile would be worth and what it
  needs, and MSAA on against off (one tile) on both APIs. Fixed since (next
  entry); its diagnostic game patch, `single-tile-diag.patch`, was for the old
  hook and is only in the history.
- `single-tile-edram-2026-10-16.md`: single-tile rendering that works, in a
  4,096-tile EDRAM (`rexglue-edram-size.patch`): what changed in the SDK and
  the game, the checks, one tile against two (and anti-aliasing off) on both
  APIs at native and 3x, and with a 120 FPS cap; why mode switches without the
  pause menu can leave the vehicles in the other MSAA mode.
- `edram-wrap-log.sdk.patch` (SDK, `TMPWRAP`): logs every new kind of EDRAM
  ownership range or resolve area that crosses tile 2,048 (with the console's
  EDRAM, the ranges that wrap around), with a count every 5,000. Goes on top of
  `rexglue-edram-size.patch`.
- `gt-build.ps1` and `gt-phases.py` (2026-10-26): the GPU timestamp layer as one
  reusable tool. The five `*timestamps*.sdk.patch` and `frame-log.sdk.patch`
  files were rebased onto the SDK of v0.1.2 (`git apply` takes them in this
  order, without fuzz: gpu-timestamps, gpu-timestamps-vulkan,
  texload-split-timestamps, resolve-split-timestamps, frame-log).
  `gt-build.ps1` applies them to `third_party\rexglue-sdk` (recorded in
  `.gt-layer`; `-Reverse` takes them out again), rebuilds only the
  `rexgpu-xenos` target and copies the plugin to `out\gt`, to put next to a
  normal `mcla.exe` and DLLs of the same build. `gt-phases.py <dir>` reads
  the timed frames (`<label>_<n>_gt.csv` from `set tmp_frame_log <label>` in
  a pad, or `gt_<n>.csv` from `MCLA_GT_EVERY`) and prints the GPU time by
  render phase (the game's phase markers: pre-passes, scene, post, tone map,
  HUD), by category (draws, EDRAM transfers, resolves, texture loads,
  uploads, and the split steps) and, with `--draws` and a text-log frame next
  to them, by shader and draw size. Run frames with
  `--mcla_render_markers_all_phases=true`, `--tmp_frame_log_dir=<folder>`,
  `--tmp_frame_log_quiet=true` (timestamps only) and, on D3D12 at 1x and
  2x, `--d3d12_threaded_submission_split_draws=0`; phases and per-draw
  labels exist on D3D12 only, categories and passes on both APIs.

## Scripted controller input (unattended runs)

A development tool of this fork only; it is never part of an upstream pull
request. `src\dev_pad.cpp` adds a synthetic controller to the game when one of
its options is set, and does nothing otherwise. It drives the game for
unattended runs and benchmarks: through the menus, through the map zoom-out
loading screen, into the city and around it, with or without the game window
in front. A real controller keeps working alongside it.

- `--mcla_pad_script=<file>` runs a script from start-up,
  `--mcla_pad_pipe=mcla-pad` takes commands on `\.\pipe\mcla-pad`
  (`dev\pad.py press start`, `pad.py shot C:\temp\a.png 0.5`, `pad.py -i`), and
  `--mcla_pad_frames_csv=<file>` logs every swap with the script's `mark`s. The
  commands are listed at the top of `src\dev_pad.cpp`: buttons, triggers and
  sticks, waits in time, engine frames or both (`wait 300ms 18f`: until both
  have passed, for menus at low frame rates), `wait_until` / `press_until` a
  condition (`left_title`, `loading`, `roam`, or a guest memory word),
  screenshots, recording and playback. A line that fails (a condition timing
  out, an included script failing) ends the script, and with
  `--mcla_pad_exit_on_end` the game; `devrun.py` then reports the run as
  failed. `set <option> <value>` changes an
  option while the game runs, for comparing two settings within one run (the
  time of day and the traffic differ from run to run), and `get <option>...`
  logs the values the options have now (`fg_status` is frame generation's
  state, `ll_status` the low latency feature's), `window minimize|restore|hide|show|cover|uncover|state`
  does that to the game's own window (`cover`: a black pop-up of the game's over it), to see what a minimised, hidden or covered
  window does to the frame loop (Windows), and `save_config` does what the F4 overlay's Save does
  (`rex::cvar::SaveConfig` into `mcla.toml`), to see what it writes, and
  `write_content_header <name>` writes a content header file from a struct
  whose padding holds 0xEE (`scripts\content-header.pad`). `close` closes the
  window as its close button does (waiting for a save being written; `quit`
  doesn't), and `saving` says whether one is (`saving wait`: until one is).
  `--mcla_fixed_dt=N` makes every game update advance exactly 1/N s:
  uncapped, a timedemo. `--mcla_pad_timeout_shot=<file.png>` saves a screenshot
  (as `shot` does) when a `wait_until` or `press_until` times out, before the
  script fails, so that a run that never got where it should leaves a picture
  of where it was; `devrun.py` gives it `timeout.png` in the run's folder.
- `dev\devrun.py` runs the game with a script **on a copy of the save data**:
  `prepare` snapshots `Documents\mcla` (career save, achievements) to
  `%LOCALAPPDATA%\mcla-dev\pristine`; every run restores
  `%LOCALAPPDATA%\mcla-dev\work` from it and passes `--user_data_root` and
  `--cache_root` there, backs up `mcla.toml` next to `mcla.exe` and puts it back
  after the run, and stops if the real save, achievements or `mcla.toml`
  changed. `--toml <file>` starts a run with that `mcla.toml` instead (`none`:
  with none), and what the game saved there (the pause menu's settings) is kept
  as `mcla.toml.after` in the run folder. `--vsync unset` leaves `--vsync` off
  the command line, which otherwise has `--vsync=false`.
  `--fps` is also the cap with frame generation on (`--mcla_fps_frame_generation`
  gets the same value, so a run's cap doesn't depend on it); `--fps-fg half` (or a
  number) sets that one apart, `half` being Half display. `fg\fg-live-switch.pad`
  switches frame generation on and off while driving, `fg\fg-menu.pad` steps the
  pause menu's DISPLAY rows (FPS TARGET, FRAME GENERATION) and
  `fg\fg-menu-unavailable.pad` does the same with generation made to fail
  (`--extra --frame_generation_debug_fail=1`).
  A RESTART GAME in the run (a new mcla.exe started by the game) is followed
  until the last one exits, before `mcla.toml` is put back and the checks run.
  `run` runs one script, `bench` repeats one per configuration
  (alternating, e.g. `--configs d3d12:1,vulkan:1`) and reports each segment's
  median frame time, 1% low and run-to-run spread, `analyze` reads a run.
  `devrun.py prepare --refresh` takes a new snapshot after you have played on.
  Every run is also checked for other programs using the GPU (the GPU guard,
  below).
- `dev\scripts\`: `boot-to-roam.pad` (title, the map zoom-out loading, the
  city: the loading benchmark), `drive-spawn.pad` (the same, then about 8 s of
  driving from the spawn point; with `--fixed-dt 60 --track` three runs' paths
  stayed within 1.7 m of each other), `mark-roam.pad` (for recordings) and
  `race.pad` (an arcade race, "Sunset and Vine" with 7 opponents, traffic,
  rain and midnight, through `boot-to-race.pad`; the car follows
  `sunset-and-vine.route` with the `follow` command; see
  `dev\race-benchmark-2026-10-15.md`). Scripts can `include` others.
- `dev\route.py record <file>` writes the path the leading opponent drives in
  a race (from the pipe's `cars` command) as a route for `follow`;
  `route.py from-log <run folder> <file>` does the same from a run whose script
  had `watch 15` on (no pipe, uncapped; one reading far off that the next undoes, every
  car at once, is a glitch of the log and skipped, a jump that stays starts the route
  over). `follow` drives the triggers and the left
  stick only: buttons a script holds meanwhile (`down x`, `press lb`) stay. A car knocked
  off the route is brought back in steps, each logged and counted (`recoveries=` in
  the `follow` summary, with `best=` for how far along it got): four kinds of longer
  reverse, then a forward burst toward the route; after 30 m off, the nearest point by
  distance, behind or ahead; no progress for 20 s counts as stuck. (Putting the car
  back by writing its position and velocity was tried and the car drove on: its state
  isn't in a copy of those floats; `findvec <addr>` looks for copies. The game has no
  reset either: the pause menu has Restart Race, Next Race and Quit Race.)
  A tip or dialog over a race ("SLIP STREAM TURBO ... CONTINUE (A)") stops the whole
  world; `follow` presses A for it, and `autocontinue on|off` does the same for a script
  that doesn't follow a route (it is the same thing: no car's position changed for a
  quarter of a second once one had moved; the pause menu stops the world too).
- `devrun.py` writes only inside its dev root: `--cache-root`, `--stdout` and
  `--record` must be absolute and inside `MCLA_DEV_ROOT` (or `mcla-dev` in
  `%LOCALAPPDATA%`), and the same goes for `coverage.py`'s `--out`, `--route-out` and
  `grow --out` (a pack may go to the repository's `shaderpack\` too); a path that lost
  its backslashes on the way ("C:mcvshp-devx") is named as that. Jobs files use forward
  slashes and are not read as POSIX shell lines.
- `dev\scripts\arcade-race.pad`, `roam-soak.pad` and the `explore-*.pad` tours
  (the pause menu, Modes > Arcade and Goal Attack and their race lists, the race
  options, Settings, the controls) are templates and tours for
  `dev\shaderpack\coverage.py` (below); `#@default NAME=VALUE`, `{NAME}`,
  `@repeat N: a ; b`, `@if V: a ; b` and `{SHOTS}` in them are expanded by it,
  not by the game.
- `dev\shaderpack\coverage.py` plays a script on a new shader storage with the
  shader pack imported at the start and reports what the pack did not cover (the
  pipelines and shaders the game created while playing, each a draw left out while
  it compiled), the frames over 50 ms, and what a player would see of them, from the
  dev build's counters (`shader-stutter-timers.sdk.patch`): draws left out, draws
  whose pipeline was made on the spot, the frame budget waited, pipeline work on the
  GPU command thread, Vulkan presents skipped. A race that stalled is a failed job
  (a probe route under 2,000 m, a route covered under 80%) and a rerun redoes it.
  `matrix.py` writes the jobs files for the arcade's 38 races (a probe per race that
  makes its route, then variants of every race on each render target path with the
  traffic off, a subset with it on, and a hold-out variant), `coverage.py batch`
  runs them under the shared GPU lock (`--salt-base` for a driver cache that is cold
  for each job, `--only <regex>` for a subset of a file that keeps the salts of the
  whole), `coverage.py revalidate --from-logs` makes the probes' routes again from their
  logs, `coverage.py grow` builds the grown pack from the soaks' storages,
  `coverage.py compare <folders> --old <pack> --new <pack>` says what the storages of
  a hold-out (races played with the old pack in conditions the new pack's soaks did
  not use) hold beyond each pack, and `test_pack.py` tests the tools that need no
  game. `measure.py prepare --pack <file> --label <name>` times a preparation from
  a pack other than the repository's.

The game ignores buttons during its own transitions (the title screen's camera
cuts, a menu closing), so scripts press until a condition holds rather than
once. Benchmarks run with `--mcla_intro=normal` (`devrun.py bench`'s default):
"fast" also lets the loading loops run unpaced.

### GPU guard (other programs on the GPU)

Another program using the GPU during a run (a second game, a video, a stress
test) takes GPU time from the game and skews its frame times. While the game
runs, `devrun.py` samples every process's GPU use once a second from Windows'
`\GPU Engine(*)\Utilization Percentage` counters through PDH (no admin rights;
about 1 ms a read). It adds up each process's 3D and compute engines (copy and
video engines are left out) and ignores the game, its child processes and
`dwm.exe`, which composes the game's window and gets busier with it. Another
process above `--gpu-threshold` (default 5, in % of one engine) over the whole
run or over any 5 s marks the run as **contaminated**:

- the run's summary line (`run`, `bench`, `record`) says whether it was clean,
  and lists the offending processes with their pid, mean, highest 5 s and peak
  use, and the game's own use;
- `gpu-guard.json` in the run folder holds the verdict and those figures for
  every process seen; `devrun.py analyze <run>` prints it.

Before each run the guard samples for 2 s. If another process is busy, it says
so and waits up to `--gpu-wait` seconds (default 0; 120 for `bench`) for it to
stop. If it doesn't stop, the run goes ahead and is marked (`busy_at_start`).

`bench` redoes a contaminated run straight away, so the configurations still
alternate, up to `--gpu-retries` redos in all (default 3; warm-up runs are not
redone; a redo's folder ends in `-redoN`). Contaminated runs are left out of the
statistics, and the summary says how many runs were redone and why. Past the
cap it says the comparison is NOT RELIABLE, with the runs left out and who was
using the GPU.

`--no-gpu-guard` turns the guard off. Where the counters are not available
(some sessions, or GPU performance counters turned off) it warns once and the
runs go on unchecked. `devrun.py gpu [--seconds 10]` lists who is using the GPU
right now. `dev\devrun_test.py` tests the verdicts and `bench`'s redos on
synthetic samples.

Measured on this machine (RTX 5080, 2026-10-16):

- **Background levels.** Idle and during runs, no other process went above
  2.3% over 5 s: Chrome, an Edge window animating a full-screen canvas (1.8%),
  the NVIDIA overlay, Windows Terminal, Explorer. dwm.exe peaked at 3%. The
  game itself reads 50-70% at Native (CPU-bound) and about 77% at 3x.
- **A light load.** A compute load of about 9% was flagged in every run, yet
  over a whole run it averaged only 3.5-6%, which is why the 5 s window counts.
  It did real damage: at 3x (`drive-spawn.pad --fixed-dt 60`) the city
  segments' 1% low fell to 54-56 and 82-83 fps in two runs against 87-90 clean,
  with medians within 2%. At Native, which is CPU-bound, the medians moved by
  only 0.4%. 5% is twice the busiest harmless process above.
- **Cost.** Guard on against off, 4 alternating runs each of
  `drive-spawn.pad --fixed-dt 60` at Native: the city segments' medians agreed
  within 0.2% (6.864 against 6.880 ms), inside the run-to-run spread.

### Recording a drive for playback

    python launcher\dev\devrun.py record C:\Users\<you>\Desktop\drive.csv

starts the game (Direct3D 12, Native, 120 FPS cap, windowed; `--fullscreen`,
`--res 3`, `--backend vulkan` as you like) on the save copy, so nothing you do
in that session reaches your real career. Press Start at the title as usual and
drive with your controller; the recording starts counting when you are in the
city. Close the game when you are done. The file holds your controller's state
frame by frame and the car's position twice a second. A run replays it with
`play <file> from=free_roam` after `boot-to-roam.pad`'s steps, and reports how
far the car strayed from the recorded positions (`max_dev`); played back with
`--mcla_fixed_dt=120` it advances the game exactly as at the recording's cap.

### How repeatable (2026-10-14, RTX 5080, alternating runs after a warm-up)

- `drive-spawn.pad` as a timedemo (`--fixed-dt 60 --intro fast`, uncapped):
  the drive segment's median frame time varied 1.3-2.5% between runs (Native
  and 3x, both APIs) and every run's path stayed within 0.4-4.9 m of the first
  run's, including across APIs. The map zoom-out's median varied 0.5-0.7%
  (8% once on Vulkan at 3x).
- `boot-to-roam.pad` with the normal intro: the zoom-out took 6.4-7.2 s in
  every run. Uncapped, its frame-time median is not a good measure: about a
  quarter of uncapped swaps in the city carry no game update, and the normal
  intro's pacing holds each of those to 30 per second (33 ms), so medians mix
  two populations (spread up to 21%). At a 120 cap there are none.
- A recorded drive replayed within 0.06-0.8 m of the recording for its first
  3.5 s. Traffic is not the same from run to run: in the recording run the car
  stopped against something after 4.5 s, the replays drove on, and `max_dev`
  (70 m) flagged it.
