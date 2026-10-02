# Building from source

This is optional. Most users should simply download the prebuilt `ClaudeDesktopProfilesManager.exe` from the [latest release](../../releases/latest).

## Prerequisites

- **Visual Studio 2022** or later, or its **Build Tools**, with the **Desktop development with C++** workload
- **Git** - only to clone the source
- **Windows PowerShell 5.1** - only to regenerate the icon
- **Python 3.6 or later** (`python`, or `py -3`) - optional: `build.cmd` then also checks the interface catalogs (`tools\check-localization.py`), as GitHub Actions always does

## Build

Clone or download this repository, open a terminal in its folder, then:

```powershell
# Build build\ClaudeDesktopProfilesManager.exe, then build and run the unit tests
.\build.cmd

# Optional: install this build for the current user (what double-clicking it does)
.\build\ClaudeDesktopProfilesManager.exe --install

# Optional: regenerate src\app.ico
powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1
```

`build.cmd` finds the newest Visual Studio or Build Tools with the C++ tools (`vswhere.exe`) and loads its x64 compiler itself, so any terminal works. A compiler warning, a catalog error or a failing test stops it with a non-zero exit code, and leaves no `ClaudeDesktopProfilesManager.exe` in `build\`: the exe is put there only once everything passed. `test_theme` and `test_layout` open windows and check what is drawn in them (`test_theme` partly by reading the screen): run `build.cmd` in an unlocked desktop session.

`vswhere.exe not found` means Visual Studio (or its Build Tools) is not installed; `Visual Studio 2022 or later with its C++ build tools was not found` means the C++ workload is missing or Visual Studio is older; `... cannot be replaced: close it first` means the previous build's exe is still running; the other two messages point at a broken Visual Studio or Windows SDK install and name the script to run to see the details.

## Release

Releases are built and signed on the maintainer's machine: the code signing key lives on a hardware token, out of reach of GitHub Actions.

Set the version in `src\version.h` (the three numbers and the string) and commit, and write the release notes in `RELEASE_NOTES.md` at the root (git ignores it; `gh release create` reads it). Then, with the token plugged in:

```powershell
# From a clean working tree (git status shows nothing): build and test, then sign only if that passed
# (the token's software asks for its PIN)
.\build.cmd
if ($LASTEXITCODE -eq 0) { .\sign.cmd }
```

Once both succeeded, tag the commit and push it:

```powershell
git tag v1.1.0
git push origin main v1.1.0
```

Pushing a `v*` tag starts GitHub Actions, which builds it and fails when the tag is not `v` followed by `APP_VERSION_STR` from `src\version.h`. Every installed copy refuses an update whose version is not the release's, so publish only once that run passed:

```powershell
# Once the tag's run shows in GitHub Actions: wait for it, then publish the signed exe if it passed
$run = gh run list --workflow build.yml --branch v1.1.0 --limit 1 --json databaseId --jq '.[0].databaseId'
gh run watch $run --exit-status
if ($LASTEXITCODE -eq 0) { gh release create v1.1.0 build\ClaudeDesktopProfilesManager.exe --title v1.1.0 --notes-file RELEASE_NOTES.md }
```

`sign.cmd` signs with the certificate issued to `APP_SIGNER` from `src\app.h`, timestamps the signature (it stays valid after the certificate expires) and checks it. It removes the signature again when the certificate is not issued to exactly that name: an update runs only with that signature. The release gets one asset, `ClaudeDesktopProfilesManager.exe`.

> The asset name never changes: the README download link points to `releases/latest/download/ClaudeDesktopProfilesManager.exe`.

`gh release create` fails when the tag already has a release: tag a new version. Every push to `main` and every pull request also builds the exe, unsigned, as a workflow artifact.

## Project structure

```
.github/workflows/build.yml   unsigned CI build of pushes and pull requests, version check of v* tags
build.cmd                     builds the exe, checks the catalogs and runs the tests below, in this order
sign.cmd                      signs the built exe for a release (the key is on a hardware token)
docs/HOW-IT-WORKS.md          how profiles start, run and receive claude:// links, and the facts behind it
docs/UI-COMPONENTS.md         shared controls, scrolling views, painting and responsive layout contracts
docs/screenshot.png           README image
src/app.h                     shared declarations, constants, registry paths
src/core.c                    pure helpers (no I/O): names, links, launch arguments, log parsing, routing decision,
                              JSON and session entries, queued session changes, hashes, scrolling math, versions
src/util.c                    known folders, registry, log file, long paths, file reading, process start, Recycle Bin
src/localize.c                interface language selection, catalog lookup, reading direction (TR)
src/localize.h                its declarations and TR()
src/localize_catalog.inc      the twelve embedded interface catalogs
src/theme.c                   the look of every window (light, dark, high contrast): palette, fonts, rows,
                              off-screen drawing, buttons, lists, edits, smooth scrolling; dialog fitting,
                              the main window's layout and captions, the themed message box
src/claude.c                  package discovery, ActivateApplication launch, running profiles, main.log reading
src/profiles.c                profile detection, create, rename, delete, Recycle Bin, settings copy
src/icons.c                   badged profile icons (hand-written .ico), badge, notification-area icon
src/shortcuts.c               profile shortcuts: create, find, remove, refresh (IShellLink, AppUserModelID)
src/taskbar.c                 per-profile taskbar buttons and the profile watcher (--watch) (Taskbar Module, see LICENSE)
src/taskbar-pin.c             taskbar pins: pin, update, repair, removal at uninstall (Taskbar Module)
src/tray.c                    notification-area icon in the profile's color
src/handler.c                 claude:// registration and default-app check
src/install.c                 install, repair, uninstall
src/update.c                  new release check (GitHub API, WinHTTP) and one-click update
src/router.c                  --launch (shortcuts) and --url (claude:// links)
src/gui.c                     manager window and dialogs
src/sessionstore.c            every profile's Claude Code sessions: entries, transcripts, projects, sessions in use (read only)
src/sessionedit.c             session actions: open, copy, entry changes (made when an open profile closes), delete
src/sessions.c                the sessions view of the manager window: profiles, tree, details, menus
src/main.c                    command-line dispatch
src/app.rc, src/resource.h    dialogs, version info, control ids
src/app.manifest              DPI awareness, common controls 6, no elevation
src/app.ico                   application icon (made by tools/make-icon.ps1)
src/version.h                 version shown in the app and in Settings
tests/test_core.c             core.c alone
tests/test_localize.c         catalogs, format arguments, language selection, reading direction, name cuts, key names
tests/test_pin.c              taskbar-pin.c: entries, records, edits and their retries, on private values and files
tests/test_sessions.c         sessionstore.c, sessionedit.c and the sessions view on private profiles, transcripts and state folder
tests/test_platform.c         update, install, watcher, routing and removal failures (sources included with fixtures
                              in place of their system calls; their exported functions renamed Tested<Name>)
tests/test_shortcuts.c        shortcuts.c on private files and registry records, with COM failure fixtures
tests/test_theme.c            what theme.c draws, against Windows' own drawing
tests/test_layout.c           native control bounds and rendered text in every interface language and scale
tests/test_claude.c           the installed Claude Desktop still works as HOW-IT-WORKS.md says (skipped without it)
tools/make-icon.ps1           regenerates src/app.ico
tools/check-localization.py   checks the catalogs: keys used and translated, formats, punctuation, direction marks,
                              command wording and access keys per language
```

## Technical notes

- **Plain Win32 C** - MSVC with `/W4 /WX /sdl` and Control Flow Guard, Unicode (`...W`) APIs, `strsafe.h` for every string copy and format; no third-party code
- **Static CRT** - `/MT` and system DLLs only (listed in `build.cmd`): the exe runs on a bare Windows 10 1809+
- **Tests** - Pure logic goes in `src/core.c`, with a test in `tests/test_core.c`; the pin entry helpers are tested in `tests/test_pin.c`; `tests/test_theme.c`, linked with the program's manifest, compares what `src/theme.c` draws (rows and their three blues, separators and tree arrows, list headers, edits and their printing, drop-down buttons and their width, side bar colors, scroll bars at the edges and following the wheel, off-screen drawing) with what Windows draws, in the mode Windows is set to, and checks the views that scroll lists by the pixel (paging, keeping their place on refills and resizes, following only the keyboard and the program, the keys that scroll a tree, the wheel and what stops it, lists taller than a native control can scroll), push buttons, tooltips and info tips, the last column's width, the focus cue and secondary text, names never cut inside a character, and that every resource is freed after destruction; `tests/test_claude.c` looks up in the installed Claude Desktop each fact the program relies on (skipped where Claude is not installed, as on GitHub Actions), so a Claude update that changes one fails the build
- **Manual checks** - Use a throwaway profile (`%APPDATA%\Claude-<test>`) and delete it afterwards; never test with the Claude windows in daily use
- **Session fixtures** - `tests/test_sessions.c` runs through `build.cmd` without launching Claude. It covers real and virtualized Main storage, account selection, filesystem notifications, concurrent pending edits, unreadable queue files, complete deletion plans, scratch copies and settings-copy exclusions. Its profiles, transcripts, log and queued changes stay in a unique temporary directory, which it removes afterwards; deletion plans do not send user files to the Recycle Bin.
- **Platform fixtures** - `tests/test_platform.c` includes `update.c`, `install.c`, `taskbar.c`, `gui.c`, `router.c`, `util.c` and `profiles.c` with fixtures in place of their system calls. It checks the release check and the download against a fixture server (URLs, HTTPS, status, the `MZ` header and size limits); the download's verification (the signature checked on the opened file, its version read as data, a refused download deleted, `Update_Run` starting only a verified file, once); the atomic install (`<exe>.new` then a rename, retries, rollback, leftovers removed, an unsupported Windows refused first); the uninstall (the download, registry keys and state folder removed, linked profile folders never removed, running profiles refused); the wait for a profile's Claude (its process, the folder watch with its lock file then its window, quit and handover); the router's watchers; the manager's update messages; and the Recycle Bin results. It runs on private files, a faked WinHTTP, faked registry writes and recorded hooks, without stopping the user's watchers or launching an installation.
- **Shortcut fixtures** - `tests/test_shortcuts.c` checks setup, allocation, reading and property-store failures (every COM interface released on each path), the saved target, arguments and AppUserModelID, updates, renames, the recorded shortcuts and their removal. Its shortcuts stay in a private temporary directory, their records in a private volatile registry key, and their targets are never launched.
- **Interface fixtures** - `tests/test_layout.c`, linked with the program's manifest, measures native controls and compares rendered sidebar text across twelve languages and four simulated scales (font and geometry scaled on top of the monitor's DPI; a scale whose window does not fit the work area is skipped and printed), including repeated language changes in the same window. It also drives the sessions view on private profiles: refills that keep the selection, folded folders and the top row, the folder watcher, a steady stream of transcript writes, double clicks. It checks the real message box in every language. Its log and queued session changes stay in a private folder, and its manager window uses a class of its own, so a running manager never finds it. It briefly shows windows; its maximized window, profile dialog and message boxes take the foreground. Physical monitor changes and the perceived menu animation still need manual checks.
- **Launching and routing** - Read [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) before changing them: it lists the measured behavior they rely on
