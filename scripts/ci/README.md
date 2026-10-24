# CI and releases

Two workflows in `.github/workflows/`, and the scripts in this folder that they run (each also runs
by hand). No game data is ever involved: the runners have none, and nothing a workflow builds
contains any.

## `ci.yml`: on every push and pull request

Read-only (`contents: read`), no secrets, nothing kept. Three jobs on `windows-2025`:

| Job | What it checks |
|---|---|
| Scripts, pins and patches | `check_pins.py`: the llvm-mingw and Clang versions and SHA-256s in `tools.json` are the ones `make_pc.ps1` uses; every `patches/rexglue-*.patch` is applied by `make_pc.ps1` (and listed in `FORK.md`) and the other way round; no tracked file looks like game data (disc image, `.xex`, `.rpf`, `.bik`, keys) or is over 8 MB; every Python source compiles without warnings. `check-powershell.ps1`: every `.ps1` parses with Windows PowerShell 5.1. |
| Host code against the SDK (llvm-mingw) | Builds the patched ReXGlue SDK with the pinned llvm-mingw (`make_pc.ps1 -Toolchain mingw -Until sdk`; cached by the hash of the patches and scripts, and a restored one is checked against the patches again), then **compiles every host source (`src/*.cpp`) with `-fsyntax-only`** and the project's own flags, against that SDK's headers, with stand-ins for the headers `rexglue codegen` writes from the game (`host_stub.py`). |
| Launcher | `dotnet publish` of the launcher, self-contained, with its notices (`publish-launcher.ps1`). |

**What the host check covers:** that the fork's own code (the 17 files in `src/`) still compiles, in
syntax and types, against the SDK as patched, and that the SDK itself builds with the pinned
toolchain and patches. **What it does not:** the recompiled game code (it comes from a player's
`default.xex`; the stand-in headers declare the ~30 guest functions the host code names and nothing
else), linking, and running the game. A change that breaks only those is found on a PC with the game.

## `release.yml`: a draft release, never a published one

Run by hand (Actions, "Release (draft only)", with the tag name) from `main`, or by pushing a
`draft-release/*` branch to try it. Jobs, in order:

1. **sdk**: the patched SDK, llvm-mingw 20250709, built from nothing.
2. **fidelityfx** (in parallel): the pinned Vulkan SDK (installer SHA-256 checked) and the project's
   Clang 20.1.8 (SHA-256 checked) build FSR's two runtimes from FidelityFX's source at the commit and
   tree hashes the SDK pins (`build-fidelityfx.ps1`), **with the C runtime linked in**
   (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`): the DLLs import only `kernel32`, `user32`, `winmm`,
   `dxgi` / `vulkan-1`, so a player needs no Visual C++ Redistributable. The build fails if either imports
   a C or C++ runtime DLL.
3. **launcher** (in parallel): `publish-launcher.ps1`, self-contained (the .NET runtime and the Windows
   App SDK runtime inside; the player installs nothing), zipped; the licence and notice files of all its
   packages in `notices\`.
4. **bundle**: `make-sdk-bundle.py` puts the SDK's install tree, the two DLLs, `libc++.dll` and
   `libunwind.dll`, the licences and `NOTICE.txt` / `SOURCE.txt` in one deterministic zip named
   `rexglue-sdk-<SDK commit>-<patches hash>-llvm-mingw-<version>.zip`.
5. **verify**, on a fresh machine and checkout: `make_pc.ps1 -Toolchain mingw -SdkBundle <zip>
   -SdkBundleSha256 <hash> -Until sdk` (unpacked, laid out and manifest-checked as the wizard's install
   will use it), `rexglue.exe` runs, the Direct3D 12 DLL loads, and the host sources compile against the
   unpacked bundle.
6. **draft**: the pinned tools `release.json` names are downloaded again and compared with
   `tools.json`; `assemble-release.py` writes `release.json`, `NOTICES.txt`, `SHA256SUMS` and the draft's
   text; `create-draft-release.ps1` creates the release with `gh release create --draft`. It makes no tag,
   refuses to touch a published release of the same tag, replaces an earlier draft of it, and checks that
   what it made is a draft. Only this job has `contents: write`, and only this step uses the token.

Assets of a draft: `MCLA-Launcher-<tag>-win-x64.zip`, the SDK bundle zip, `release.json`, `NOTICES.txt`,
`SHA256SUMS`. `MCLA-Setup.exe` is not built yet (it comes with the setup wizard): `installer` and `game`
are `null` in `release.json`.

## Pins

Everything a workflow downloads is pinned by hash. Third-party actions are pinned by commit SHA
(with the version in a comment); `tools.json` pins llvm-mingw, CMake, Ninja, MinGit, the project's Clang
and the Vulkan SDK by URL and SHA-256 (the digest GitHub shows for the release asset, or the one
measured for the Vulkan installer, which has no published hash). To change one: take the new file's
SHA-256 from its release page, change `tools.json` and, for llvm-mingw and Clang, the same pins in
`scripts/make_pc.ps1` (`check_pins.py` fails if they differ).

## Scripts

| Script | Does |
|---|---|
| `check_pins.py`, `check-powershell.ps1` | the first CI job |
| `host_stub.py`, `host_check.py`, `host-compile.ps1` | the host-code check (`host-compile.ps1 -SdkDir <SDK install>` runs both) |
| `install-vulkan-sdk.ps1`, `build-fidelityfx.ps1` | FSR's DLLs |
| `publish-launcher.ps1`, `launcher-notices.py`, `zipdir.py` | the launcher zip and its notices |
| `make-sdk-bundle.py` | the SDK bundle zip |
| `assemble-release.py`, `verify_tools.py`, `create-draft-release.ps1` | `release.json` and the rest, and the draft |
| `tools.json`, `licenses/` | the pins; licence texts that packages do not carry |
