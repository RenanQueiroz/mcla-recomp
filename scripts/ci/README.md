# CI and releases

Two workflows in `.github/workflows/`, and the scripts in this folder that they run (each also runs
by hand). No game data is ever involved: the runners have none, and nothing a workflow builds
contains any.

## `ci.yml`: on every push and pull request

Read-only (`contents: read`), no secrets. Three jobs on `windows-2025`:

| Job | What it checks |
|---|---|
| Scripts, pins and patches | `check_pins.py`: the llvm-mingw and Clang versions and SHA-256s in `tools.json` are the ones `make_pc.ps1` uses; every `patches/rexglue-*.patch` is applied by `make_pc.ps1` (and listed in `FORK.md`) and the other way round; every action is pinned by commit SHA, no `pull_request_target`, no secret but the token; no tracked file looks like game data (disc image, `.xex`, `.rpf`, `.bik`, keys) or is over 8 MB; every Python source compiles without warnings. `check-powershell.ps1`: every `.ps1` parses with Windows PowerShell 5.1. |
| Host code against the SDK (llvm-mingw) | Builds the patched ReXGlue SDK with the pinned llvm-mingw (`make_pc.ps1 -Toolchain mingw -Until sdk`; cached by the hash of the patches and scripts, and a restored one is checked against the patches again), then **compiles every host source (`src/*.cpp`) with `-fsyntax-only`** and the project's own flags, against that SDK's headers, with stand-ins for the headers `rexglue codegen` writes from the game (`host_stub.py`). Then **`csr-exact.ps1`** compiles and runs `csr-exact/csr_exact.cpp` against that SDK: the host-side exactness test of the code generator's MXCSR options (every instruction that stops asking for a flush mode gives the same bits in both modes and all four rounding modes; the ones that keep asking must differ; the cheap switch equals the old calls; no game data). |
| Launcher | `dotnet publish` of the launcher, self-contained, with its notices (`publish-launcher.ps1`). Its NuGet packages float, as in any build of it: a Reactor preview that breaks the launcher shows up here first. Then `MCLA-Setup.exe` is built as a release builds it (`build-installer.ps1`: NativeAOT, the launcher folder as its payload, unpacked again and compared file by file) and tried on scratch folders (`smoke-installer.ps1`), and `launcher\dev\net-check` runs the network checks (the connect logic with an IPv6 address that never answers and a working IPv4 one: IPv4 wins after the 250 ms head start; the messages that name the stage that failed; the routes for `release.json`: its address, one more try, GitHub's API; all on local servers), and `launcher\dev\update-check` runs the self-update checks: the folder swap, the verified download, and the real installer with fake launchers (an update that waits for the old launcher, the way back when the new one does not start, a damaged file, a held file, an interrupted swap, refusals). |

**What the host check covers:** that the fork's own code in `src/` still compiles, in syntax and
types, against the SDK as patched, and that the SDK itself builds with the pinned toolchain and
patches (a patch that only builds with MSVC's Clang fails here). **What it does not:** the recompiled
game code (it comes from a player's `default.xex`; the stand-in headers declare the guest functions
the host code names and nothing else), linking the game, and running it. A change that breaks only
those is found on a PC with the game.

## `release.yml`: a draft release, never a published one

Run by hand (Actions, "Release (draft only)", with the tag name) from `main`, or by pushing a
`draft-release/*` branch to try it. A version tag (`v0.1.0`) is accepted from `main` only; a
`draft-release/` branch makes test drafts named `draft-<commit>`. Jobs:

1. **plan**: the tag rules above, and `check_pins.py` (a release is not made from a commit that
   fails the CI check).
2. **tools** (parallel): the four tools `release.json` names for the wizard are downloaded again (with
   retries) and compared with `tools.json`.
3. **sdk** (parallel): the patched SDK, llvm-mingw 20250709, built from nothing.
4. **source** (parallel): the SDK cloned and patched (`make_pc.ps1 -Until sdk-source`) and archived with
   the fork: `rexglue-sdk-source-<SDK commit>-<patches digest>.tar.xz` (see below).
5. **fidelityfx** (parallel): the pinned Vulkan SDK (installer SHA-256 checked) and the project's Clang
   20.1.8 (SHA-256 checked) build FSR's two runtimes from FidelityFX's source at the commit and tree
   hashes the SDK pins (`build-fidelityfx.ps1`), **with the C runtime linked in** (the arguments of the
   SDK's own `cmake/rexglue_fsr.cmake`): the DLLs import only `kernel32`, `user32`, `winmm`, `dxgi` /
   `vulkan-1`, so a player needs no Visual C++ Redistributable. The build fails if either imports a C
   or C++ runtime DLL, or if the import list could not be read.
6. **launcher** (parallel): `publish-launcher.ps1`, self-contained (the .NET runtime and the Windows App
   SDK runtime inside; the player installs nothing), zipped; the licence and notice files of all its
   packages in `notices\`. Then `build-installer.ps1` makes **`MCLA-Setup.exe`**: the installer program
   (`launcher\installer`, NativeAOT, 2.4 MB) with the published folder appended as a Brotli stream (55 MB
   for 175 MB), checked by unpacking it again and comparing every file; `smoke-installer.ps1` installs,
   reinstalls and removes it on scratch folders (the launcher is not started: a runner has nobody to look
   at its window).
7. **bundle**: `make-sdk-bundle.py` puts the SDK's install tree, the two DLLs, `libc++.dll` and
   `libunwind.dll`, the fork's patches, the licences and `NOTICE.txt` / `SOURCE.txt` in one zip named
   `rexglue-sdk-<SDK commit>-<patches digest>-llvm-mingw-<version>.zip`. The licences, submodule commits
   and notices come from the archived tree (the SDK it was built from; the script checks that the
   patches agree).
8. **verify**, on a fresh machine and checkout: `make_pc.ps1 -Toolchain mingw -SdkBundle <zip>
   -SdkBundleSha256 <hash> -Until sdk` (unpacked, laid out and manifest-checked as the wizard's install
   will use it), then `verify-bundle.ps1`: `rexglue.exe` runs, the FidelityFX DLLs import no C runtime
   (read from the files) and the Direct3D 12 one loads, **a small program (`linktest/`) is linked against
   the bundle's CMake package and import library with llvm-mingw and run with the bundle's DLLs**; and
   the host sources compile against the unpacked bundle.
9. **assemble**: `assemble-release.py` writes `release.json`, `NOTICES.txt`, `SHA256SUMS` and the
   draft's text, after checking the bundle is the one `verify` tested, that the source archive is of the
   same SDK commit, patches and fork commit, that the DLL record is sane, and that `MCLA-Setup.exe`'s
   payload header (read from the file) names this tag, commit and version and as many files as the
   launcher zip has. `release.json.installer` is its name, SHA-256 and size, which is what the launcher's
   self-update checks a download against.
10. **attest**: GitHub build-provenance attestations (`actions/attest-build-provenance`, pinned) for every
    asset. The only job with `id-token: write` and `attestations: write`; it has no write access to the
    contents. `gh attestation verify <file> -R RenanQueiroz/mcla-recomp` ties a file to this workflow, commit
    and run.
11. **draft**: `create-draft-release.ps1` creates the release with `gh release create --draft`. It makes
    no tag, refuses to touch a published release of the same tag, replaces only a draft it made itself
    (the text starts with a marker), checks the files before deleting anything, and checks that what it
    made is a draft. Only this job has `contents: write`, and only this step uses the token.

Assets of a draft: `MCLA-Setup.exe`, `MCLA-Launcher-<tag>-win-x64.zip`, the SDK bundle zip, the source
archive, `release.json`, `NOTICES.txt`, `SHA256SUMS`. The zip is the same launcher folder that is inside
`MCLA-Setup.exe` (for anyone who wants it unpacked; the wizard and the self-update use the exe).
**Nothing is signed**, and nothing will be (decided 2026-10-24): the README tells players how to get past
SmartScreen.

### The game's fingerprints (`release.json.game`)

The setup wizard checks the player's own game against a list in the release, never against a copy:
`game-files.json` has the 13 files of the Complete Edition's disc (path, size, SHA-256; the title id
`545407F8`, `default.xex` version 0.0.0.8) and nothing else of it. `assemble-release.py` copies it into
`release.json` (`game`). It is made once, from a disc image or an extracted folder the maintainer has,
by `make-game-files.py <iso or folder>` (read-only; the output has hashes and sizes only, so it is the
one file about the game that belongs in the repository). It was made from a single dump (the maintainer's disc
image): that every other pressing of the Complete Edition (USA, Europe) is byte for byte the same is an assumption;
a player whose copy differs is told it is another release or damaged, and has to send the file's names and sizes
to have it looked at. `check_pins.py` checks only its shape (title id,
version, plain names, 64-digit hashes, `default.xex` listed): a changed list is a change of what the wizard
accepts, so it is reviewed like code.

### The corresponding source

The bundle's binaries contain GPL and LGPL code (the fork's changes, GNU binutils' PowerPC
disassembler, FFmpeg's libavcodec and libavutil, libmspack), so the release carries the source:
`rexglue-sdk-source-<...>.tar.xz` holds the SDK tree the bundle was built from (every submodule at its
pinned commit, every patch applied, no `.git`) and the fork at the release commit. About 70 MB. The
bundle's `NOTICE.txt` says which binary contains what (found in the binaries themselves) and
`SOURCE.txt` where each part comes from.

### `release.json` and the tag

`release.json.commit` is the commit the release was built from. **The wizard must check that the tag it
installs points at that commit** (`git rev-parse <tag>` equals `commit`): a draft names its tag but does
not make it, and publishing makes it on the commit chosen at that moment. Leave the Target of the
draft as it is when publishing. (The repository's immutable releases and its tag ruleset for `v*` keep
a published release and its tag from changing afterwards.) `sdkBundle.patchesDigest` is the SHA-256 of
the lines `<patch name> <patch SHA-256>\n`, the patches sorted by name, ordinally (by code point); the
bundle's and the source archive's names have its first 12 digits. `make_pc.ps1` writes the manifest's
patches in the same order.

## Release checklist (before anyone publishes a draft)

1. Run the workflow from `main` with the version tag; all jobs green.
2. Download the draft's files; `sha256sum -c SHA256SUMS`; `gh attestation verify` on `MCLA-Setup.exe` and the zips.
3. **The FidelityFX DLLs in the game, with the draft's own DLLs, on both APIs** (CI cannot: a runner
   has no GPU): on a PC with the game built from this commit,
   `powershell -File scripts\ci\smoke-release-dlls.ps1 -Bundle <the draft's rexglue-sdk-*.zip>`.
   It runs FSR anti-aliasing and FSR frame generation on Direct3D 12 and on Vulkan through `devrun.py`
   and needs "loaded amd_fidelityfx_*.dll", "FSR 3.1 ready" and "frame generation: ready" in each log
   (four PASS lines). The GPU must be otherwise idle.
4. Read `NOTICES.txt`; run `MCLA-Setup.exe` (SmartScreen will warn: it is unsigned; **More info**, **Run anyway**) and
   check that the launcher installs in `%LOCALAPPDATA%\Programs\MCLA`, starts, and shows the wizard, on a PC without .NET.
5. **The setup wizard, end to end, from the draft** (needs the game's disc image and about 10 GB):
   `launcher\dev\wizard-check` runs the wizard's engine without a window, with a release source that is a
   folder (`release.json` and the assets of the draft, plus a `dev-source.json` naming a clone of the fork
   that has the tag; a draft has no tag yet): `wizard-check.exe install <folder> <install folder> <iso>`.
   It must end with all six steps done, and print the longest path of the install, which must not be
   longer than `PathBudget.LongestBelowRoot` (when `tools.json` changes a tool's version, this is where a
   longer path shows up). Then start the built game through `devrun.py` (see `launcher\README.md`).
6. Publish in the GitHub UI **without changing the Target**; look that the tag points at
   `release.json.commit`.

## Pins

The tools the build and the workflows download are pinned by hash: llvm-mingw, the project's Clang,
CMake, Ninja, MinGit and the Vulkan SDK installer by URL and SHA-256 in `tools.json` (the digest GitHub
shows for the release asset; the Vulkan installer has none published, so the hash is the one measured),
the FidelityFX source by commit and tree hashes, the SDK by commit, and third-party actions by commit
SHA (the version in a comment). **Not pinned by hash:** the launcher's NuGet packages, which float by
design (`Microsoft.UI.Reactor *-*`, `Microsoft.WindowsAppSDK.WinUI 2.*`; the versions each release resolved are
listed in its `release.json`); and what the runner image brings (Visual Studio, the Windows SDK, CMake,
Ninja, Git, the .NET SDK, `gh`). To change a pin: take the new file's SHA-256 from its release page,
change `tools.json` and, for llvm-mingw and Clang, the same pins in `scripts/make_pc.ps1`
(`check_pins.py` fails if they differ); `.github/dependabot.yml` proposes new action versions monthly.

## Notes

- **Not bit-for-bit reproducible.** Two runs on the same commit differ in the PE timestamps and the
  build stamp of the SDK's binaries, and the FidelityFX DLLs differ in size by a few hundred bytes. The
  zips themselves are deterministic for the same inputs (sorted entries, fixed times and modes). The
  attestations, not a rebuild, tie a file to its build.
- A bundle unpacked at `third_party\rexglue-sdk-bin\` has paths up to about 120 characters; the folder
  limits in docs/troubleshooting.md (148 characters for the clone) are for building the SDK.
- `release.yml` jobs run with the default cache mode; no job of the release uses a cache.

## Scripts

| Script | Does |
|---|---|
| `check_pins.py`, `check-powershell.ps1` | the first CI job, and the release's plan |
| `host_stub.py`, `host_check.py`, `host-compile.ps1` | the host-code check (`host-compile.ps1 -SdkDir <SDK install>` runs both) |
| `install-vulkan-sdk.ps1`, `build-fidelityfx.ps1` | FSR's DLLs |
| `publish-launcher.ps1`, `launcher-notices.py`, `zipdir.py` | the launcher zip and its notices |
| `build-installer.ps1`, `smoke-installer.ps1`, `check-installer.py` | `MCLA-Setup.exe` (the NativeAOT installer program plus the launcher folder), its use on scratch folders, and its payload header read back from the file by the reader `assemble-release.py` uses (`cilib.check_installer_header`), run on every exe that is built, in CI too |
| `make-source-archive.py`, `make-sdk-bundle.py`, `cilib.py` | the source archive and the SDK bundle zip |
| `verify-bundle.ps1`, `linktest/` | the look at the unpacked bundle |
| `assemble-release.py`, `verify_tools.py`, `create-draft-release.ps1` | `release.json` and the rest, and the draft |
| `smoke-release-dlls.ps1` | the in-game check of a draft's FidelityFX DLLs (checklist step 3) |
| `make-game-files.py`, `game-files.json` | the fingerprints of the game's files (`release.json.game`) |
| `tools.json`, `licenses/` | the pins; licence texts that packages do not carry |
