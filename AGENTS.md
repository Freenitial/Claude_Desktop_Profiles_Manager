# AGENTS.md

Guidance for AI assistants working in this repository.

## What this is

Claude Desktop Profiles Manager: one Win32 C executable that runs several Claude Desktop
accounts side by side on Windows. It never modifies Claude Desktop; it starts the
installed MSIX app with a separate `--user-data-dir` per profile, routes
`claude://` links to the right window, and manages shortcuts. User-facing
overview: `README.md`. Building and releasing: `BUILD.md`. Mechanics and the
measured facts behind them: `docs/HOW-IT-WORKS.md` - read it before changing
launching or routing.

## Layout

Every file and what it holds: [BUILD.md](BUILD.md#project-structure), "Project structure". `taskbar.c` and
`taskbar-pin.c` are the Taskbar Module (rule 11).

## Rules that must hold

1. **Launch through `Claude_Launch` only.** It uses
   `IApplicationActivationManager::ActivateApplication`, which gives the process
   its package identity (Claude's updater breaks without it) and passes the
   arguments through. `CreateProcess` on the WindowsApps exe is only the
   fallback when activation fails.
2. **Create a profile folder before its first launch**, never the stock
   `%APPDATA%\Claude`. With package identity, a missing `--user-data-dir` is
   silently redirected into the package's `LocalCache`.
3. **Profile folders stay directly under `%APPDATA%`** (Cowork VM) and are never
   renamed (Claude stores absolute paths in them); only display names change.
4. **Arguments are built by `Core_BuildLaunchArgs`** and links cleaned by
   `Core_SanitizeUrl`: a quote, backslash or space in a `claude://` link would
   otherwise inject Chromium switches.
5. **Routing follows what the app did, not what the user declared**: sign-in
   links go to the window whose `main.log` last logged
   `[Auth] Using system browser for:`. No marker files, arm windows or
   "sign in" shortcuts.
6. **Running detection is read-only**: `Chrome_MessageWindow` titles, and
   change notifications on a profile's folder to see it start. Do not open
   Chromium's `lockfile` (an exclusive open can make Claude think it is a
   second instance).
7. **Per-user only**: HKCU, `%LOCALAPPDATA%`, no admin rights, no services.
8. **Never touch the user's running Claude instances** from tooling or tests;
   use throwaway profiles (`%APPDATA%\Claude-<test>`) and remove them after.
9. **claude:// goes through the user's default-app choice.** Only Windows'
   chooser or Settings can set it (the choice is signed): never write
   `UserChoice`/`UserChoiceLatest`. **Set up links** only deletes another
   app's choice, so that Windows asks again. `HKCU\Software\Classes\claude` is Claude's
   own key (Claude rewrites it at every start); do not rely on it.
10. **Pins are written by `taskbar-pin.c` only.** One entry per profile in
    Windows' pin list, and one record per entry, both in the form Windows
    writes them. A pinned .lnk is updated
    in place, and renamed within the pins folder when its profile is
    renamed, each followed by the notifications the taskbar acts on
    (`TaskbarPin_Refresh`); never unpin and pin again to update it. All pin
    code stays in `taskbar-pin.c`.
11. **The Taskbar Module stays separate.** `taskbar-pin.c` and `taskbar.c`
    are under their own commercial license (see `LICENSE`): they use only the
    shared helpers of `util.c`, `core.c`, `claude.c`, `profiles.c`,
    `shortcuts.c`, `icons.c` and `tray.c` (open source), and no session, theme
    or manager code goes in them.
12. **Nothing polls.** The watcher and the manager act on events (window
    shown or created, a folder changed, package list changed, Explorer
    restart, theme change, focus, selection); a retry, a wait or an animation
    (a wheel scroll's) after an event is bounded.
13. **A running profile's session entries are not written.** Claude keeps its
    sessions in memory and writes them back, so a change to a running
    profile waits in `pending-sessions-<folder>.txt` (`SessionEdit_Change`)
    and is made once it closes. A session reaches a profile through
    `claude://resume`, which makes the entry: never write a new entry.

## Conventions

- C (MSVC, `/W4 /WX /sdl`), Unicode everywhere (`WCHAR`, `...W` APIs), `strsafe.h`
  for every string operation, fixed-size buffers sized with `ARRAYSIZE`.
- No third-party code; system DLLs only (`build.cmd` has the list). The CRT is
  static (`/MT`), so the exe runs on a bare Windows 10 1809+.
- Pure logic goes in `core.c` with a test in `tests/test_core.c`; the pin
  helpers are tested in `tests/test_pin.c`. A new fact about how Claude
  works gets a check in `tests/test_claude.c`.
- Every look comes from `theme.c`: colors (`Theme_Color`, the main, bright and
  pale blues of a list row, `THEME_SEPARATOR`), fonts (`Theme_CreateFonts`), rows (`Theme_DrawRow`),
  tree arrows (`Theme_DrawTreeGlyph`), buttons and drop-downs (`Theme_DrawButton`, `Theme_DrawDropDown`,
  `Theme_DropDownWidth`, `Theme_SetStrong`), off-screen
  drawing (`Theme_BufferBegin`); every list, list box and tree scrolls by the
  pixel in a smooth view (`Theme_SmoothView`), as what the program draws does.
  Every dialog but the manager window itself opens through `Ui_Dialog`, which
  centers it on its owner and themes its controls (`Theme_Apply`); its own
  procedure only fills it. Nothing else picks a color, draws a selection or
  places a dialog; a new kind of control is themed there, and
  `tests/test_theme.c` compares it with Windows.
- User-visible text is plain English in `TR(L"...")` or `app.rc`, with a row for all twelve
  languages in `localize_catalog.inc` (kept sorted); `test_localize` and
  `tools/check-localization.py` check it. A key the code reaches through a variable is listed
  in the checker. English uses "…" (`\x2026` in `app.rc`, which stays ASCII); zh, hi, bn and ur
  put access keys after the text as "(&X)"; text naming Claude's interface uses Claude's own
  words. Comments explain why, not history.
- Tests never touch the user's real state: private folders (`Util_SetStateDir` for the log and
  queued session changes), private registry keys, throwaway profiles.

## Build and test

```bat
build.cmd
```

Builds `build\ClaudeDesktopProfilesManager.exe` and runs the nine test programs of `tests\`; the exe
is put in `build\` only once they all pass (`test_claude` is skipped without Claude; `test_theme` and
`test_layout` show windows and check what is drawn, so they need an unlocked desktop session). Manual
checks on a real machine: install (`build\ClaudeDesktopProfilesManager.exe --install`), create
a throwaway profile, open it, create and detect a desktop shortcut, sign in with
two profiles open and read `%LOCALAPPDATA%\Claude Desktop Profiles Manager\claude-desktop-profiles-manager.log`,
delete the throwaway profile, uninstall.

A release is signed by `sign.cmd` on the maintainer's machine (the key is on a
hardware token; see `BUILD.md`). Never sign anything else with it.

A coding agent started from Claude Desktop (Claude Code in its Code tab, for
example) runs inside Claude's MSIX package: its shell sees a private copy of
HKCU and `%APPDATA%`/`%LOCALAPPDATA%`, not the real ones. Install, test and
inspect through a process started outside the package (Explorer, or WMI
`Win32_Process.Create`), or the results are wrong.
