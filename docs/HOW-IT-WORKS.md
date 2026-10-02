# How Claude Desktop Profiles Manager works

How Claude Desktop Profiles Manager runs one Claude Desktop per profile, tells which ones are running, gives each its own taskbar button, pin and notification-area icon, and routes `claude://` links.

The observations below were made on Claude Desktop 2.16120.0 (MSIX) and Windows 11 (build 26200). All of it relies on undocumented behavior of Claude or Windows; the build checks Claude's side (see the last section).

## Profiles

Claude Desktop is an Electron (Chromium) app, and Chromium allows one instance per user data folder. Started with another `--user-data-dir`, it runs a second, independent instance with its own sign-in.

| Profile | Data folder seen by Claude | Started with |
|---|---|---|
| Main | `%APPDATA%\Claude` | no argument, like the Start menu |
| Others | `%APPDATA%\Claude-<name>` | `--user-data-dir="%APPDATA%\Claude-<name>"` |

- Folders stay directly under `%APPDATA%`: Cowork's VM service looks for its disk image in `%APPDATA%\<folder>\vm_bundles`.
- A folder is never renamed, since Claude stores absolute paths in it. Renaming a profile only changes its display name, kept in `HKCU\Software\Claude Desktop Profiles Manager\Profiles\<folder>`.
- Claude itself uses `%APPDATA%\Claude-3p` and `%LOCALAPPDATA%\<folder>-Data`, so the names `3p`, `Data`, `...-3p` and `...-Data` are refused.
- A profile is `%APPDATA%\Claude`, or a `%APPDATA%\Claude-*` folder with an allowed name that holds a Chromium `Local State` file or was created by the manager. A folder linked elsewhere (a junction or directory symbolic link) counts once its `Local State` is reachable.

The manager's **Role** column:

| Role | Meaning |
|---|---|
| Claude icon, default | Main, which the regular Claude icon opens, and also the default profile |
| Claude icon | Main; another profile is the default one |
| Default | The profile picked with **Set as default**: it opens `claude://` links while Claude is closed |
| *(empty)* | Any other profile |

## Starting Claude

Claude Desktop is an MSIX package, and the way it is started matters:

| Started by | Package identity | Updates |
|---|---|---|
| `CreateProcess` on `WindowsApps\...\app\Claude.exe` | no | fail: "Can not find Squirrel" |
| Start menu, or `IApplicationActivationManager::ActivateApplication` | yes | work (MSIX updater) |

The manager uses `ActivateApplication(<family>!Claude, arguments)`: the process gets its identity and the arguments arrive untouched. `CreateProcess` is only a fallback when activation fails. Claude logs which case it is in at start-up (the `[MSIX]` lines of `main.log`).

With its identity, Claude writes to AppData through the package's private copy, `%LOCALAPPDATA%\Packages\<family>\LocalCache`:

- Main's data is in `%APPDATA%\Claude` when that folder exists, else in `LocalCache\Roaming\Claude` (Claude then logs `Filesystem virtualization active`), as on an installation whose Main profile was first opened through the package;
- a `--user-data-dir` that does not exist yet is created under `LocalCache\Roaming`, so the manager creates a profile's folder before its first start (never Main's);
- a profile's logs (`%LOCALAPPDATA%\<folder>\logs`) can land in `LocalCache\Local\<folder>\logs`: both places are read, and both are removed with the profile.

The manager therefore keeps two paths per profile: the one Claude sees, `%APPDATA%\<folder>`, which identifies a running instance, goes into launch arguments and matches the folders recorded in sessions; and the one files are read and written through, which is Main's `LocalCache\Roaming\Claude` when `%APPDATA%\Claude` does not exist. Finding it never creates `%APPDATA%\Claude`.

The package is `Claude_pzs8sxrjxfjjc` (from claude.ai) or `AnthropicPBC.Claude_fnn82j28hfe8t` (Microsoft Store), else any other package named Claude.

A process started from Claude Desktop (a Claude Code terminal, for example) runs inside that package: it sees the private copy, not the real registry and AppData. Test from a process started by Explorer or WMI.

## Running profiles

A running Chromium instance owns a message-only window of class `Chrome_MessageWindow`, titled with its data folder; a second launch uses it to hand over its arguments. The manager lists those windows, without opening or locking anything. The window's process is the profile's main process, and its most recently used Claude window is the first one in Z-order.

## Taskbar and notification area

A profile opened from the manager gets its own taskbar button with its badge, can be pinned from the manager, and shows its notification-area icon in its color (while Claude's **System tray** setting is on). A small watcher, `ClaudeDesktopProfilesManager.exe --watch "<folder>"`, does this while that profile's Claude runs: it only reacts to events and exits with Claude, unless Claude closed for an update (below).

The watcher knows that the profile's Claude has started when that Claude creates its `Chrome_MessageWindow`: it watches the window creations of the Claude process started for the profile, whose exit ends the wait. When that process is not known (Windows starts Claude itself after an update), it watches the profile's data folder instead, where a starting Claude creates `lockfile` right before that window. The wait lasts a minute and a half at most, a minute after an update. Renaming or recoloring a profile updates its button, pin and icon. A new display scale redraws the icon at the notification area's new size: the watcher's hidden window gets no `WM_DPICHANGED` then, but `WM_DISPLAYCHANGE`, and the size comes from the taskbar's own scale.

A profile started without the manager (the regular Claude icon) keeps Claude's button until the manager opens it or routes a link to it: its watcher then starts. A profile already watched gets no second watcher: its watcher takes the new start over, even when it was about to end.

A profile is pinned while Windows' pin list holds an active pin whose shortcut opens that profile (or Claude's own Apps folder item, for Main). A shortcut left in the pins folder, or a pin Windows retired, does not count. The manager follows changes to the pin list, without polling.

The manager writes its pins in the form Windows itself writes them, so Windows keeps them, and leaves other applications' pins as they are. It never writes over a list in a form it does not know, nor over a change Windows made meanwhile: it starts again from the new list, three times at most. A new pin takes the place of the profile's pin, active or retired, else goes last; the profile's other active pins are dropped, since Windows would unpin them and delete the shortcut they share. When the manager opens, after it changes one of its pins' shortcuts, and when a profile is pinned again, damaged pins of its own are repaired; a pin that is fine is left exactly as it is, and an unpinned profile stays unpinned. At uninstall, the active pins of the manager and its profiles leave the list, then their shortcuts are deleted.

The taskbar part is the Taskbar Module, under its own license (see `LICENSE`); the notification-area icon (`tray.c`) is not.

### Claude updates

A Claude update goes like this:

1. Claude's updater, in one of the open windows, logs `beforeQuitForUpdate handler fired, going down for update`.
2. Windows closes every Claude window to replace the package; each logs `Windows session ending (close-app) - quitting the app`.
3. Windows installs the new version and opens Claude again once, without arguments. That starts Main, within about half a minute, even when only another profile was open. The other profiles stay closed.

When its Claude exits, the watcher reads the last quit line of the profile's `main.log`. If it is one of the two above, from the last five minutes, the watcher waits, at most 2 minutes, for a Claude package other than the one that closed; changes to the user's package list and to the Apps folder wake it. Once the new package is there, Main is first given a minute to come back from Windows' own restart, and any other profile opens again at once; the watcher then watches the new window, so button, badge and notification-area color come back.

A quit from the window or the tray (`Quitting app...`, `beforeQuit:`, `willQuit:`), a Windows shutdown or a crash leaves the profile closed, as does a close for an update after which no new package comes. Opening the profile during the wait ends it.

## Start menu

Install puts a **Claude Desktop Profiles Manager** folder in the user's Start menu with the manager's shortcut, made again when the manager starts if it was deleted. **Add to Start menu** puts a profile shortcut (`Claude (<name>).lnk`) in that folder and **Remove from Start menu** deletes it. Like every profile shortcut the manager makes, it is recorded, so it follows renames and colors and goes with the profile. The button looks again when the selection changes, when the window comes back to the front, and when that folder, Programs, the desktop or the pins folder changes. At uninstall the manager's shortcut goes last, once its windows are closed: the taskbar takes their icon from it.

## The manager window

The manager follows changes made outside it without polling: a thread waits for registry notifications on `HKCU\Software\Claude Desktop Profiles Manager` (names, colors, default), on `...\Shell\Associations\UrlAssociations` (the app picked for `claude://`), on the user's package list (Claude installed or updated) and on the taskbar's pin list, and for folder notifications on `%APPDATA%` (profile folders made or removed); a burst of them refreshes the window once. Coming back to the window refreshes it too.

Interface text follows Windows' preferred UI language, with English as a fallback, or the one picked in the Language menu (stored in the manager's `Language` registry value), among twelve embedded catalogs. Profile names, session titles, paths and log messages are never translated. Text is measured with a font covering the selected script. Arabic and Urdu read right to left; in their catalogs a path sits in a left-to-right embedding and every inserted name is followed by a left-to-right mark, so neither is reordered. The Portuguese catalog is Brazilian. Where the text names Claude's own interface (its Code tab, its No folder group, its Quit command, its stars), it uses Claude's words in the languages Claude offers. After a change of language, what Windows shows of the program follows it, in the background: the `claude://` handler's name and description, and the tooltips of the manager's shortcut and of the profiles' shortcuts and pins.

The main window can be resized and maximized. Its minimum size fits every interface language and both views, so changing the language, the view or the status line never resizes it. A wider window keeps the content at a reading width, centered, and gives the lists the extra height. The Profile and Role columns start at the same width in every language and the last column takes the rest; a column the user sized keeps its width.

Every control's look comes from `theme.c`; [UI-COMPONENTS.md](UI-COMPONENTS.md) describes how to add one. Text cut short in a list, tree or label shows whole in a tooltip at once. The color selector and the session **Actions** buttons open Windows' own menus.

## Claude Code sessions

A Code-tab session is stored in two places:

- its conversation, the transcript: `%USERPROFILE%\.claude\projects\<project>\<id>.jsonl` (or under `CLAUDE_CONFIG_DIR` when it is set), in the Claude Code folder every profile shares;
- one entry per profile that lists it, in that profile's `claude-code-sessions\<account>\<organization>\local_<...>.json`: title, star (`isStarred`), folder, archived, last activity, and the session's earlier transcripts (`priorCliSessionIds`, `preClearCliSessionId`, `unarchivedCliSessionId`), which its size counts unless another session goes on with one. A profile can list a session twice; every entry of it is changed together.

Claude shows the entries of the account signed in (`lastKnownAccountUuid` in the profile's `config.json`; without one, the account with the latest entry) and of its organization with the latest entry; the entries of an account signed in before stay on disk but are not shown. A session without a project folder works in `<profile data>\scratch-workspaces\<account>\<organization>\scratch-...`, and Claude shows it under "no folder" only in the profile that owns that area. A session run over SSH (`sshConfig`), in WSL (`wslConfig`) or in the cloud (`cloudSessionId`, or `movedToCloud` set to anything but `null` or `false`) has its conversation elsewhere: the manager only counts it.

A session's `cwd` stays the path Claude recorded; opening, copying or deleting its scratch folder goes through the profile's file path (above), which differs for a virtualized Main. Paths can be longer than MAX_PATH: transcripts and entries are reached through their `\\?\` form. The Recycle Bin takes no path of MAX_PATH characters or more, so a removal that would need it for one is refused with that path, and nothing moves. A junction or directory link among the paths is removed as a link, never followed.

**Sessions >** turns the manager window to its sessions view, **< Back** turns it back. On the left the profiles, with how many sessions each lists; in the middle the chosen profile's sessions as a tree, the starred ones first, then by folder, filtered by the search box; on the right the selected session: its folder, last use and size, then each profile with what the session is there (not listed, starred, its title there, open or in use, archived) and an **Actions** menu, and **Delete session everywhere** at the bottom. The tree's context menu (Shift+F10 too) holds the shown profile's actions (Open, Rename, Star, Remove, or **Keep** while a removal waits), **Open in**, **Share with** and **Copy to** for the other profiles, **Show folder** and **Delete session everywhere**; Enter opens the session, F2 renames it, Delete removes it. An empty tree says why (not signed in, no session yet, all archived, nothing matches the search, only remote sessions, entries that could not be read).

Sessions are read on a background thread from a snapshot of the profiles, which publishes a complete result to the window. The view follows the disk through folder notifications on each profile's entries folder and on the transcripts folder; a burst of changes reloads it once, at most twice a second, and a change of profiles cancels a read in progress.

### Session actions

- **Open** starts the profile, or reaches its window, with `claude://resume?session=<id>`. Claude opens the session, and first adds its entry when the profile does not list it yet (`local_<id>`).
- **Share** sends that link to a profile that does not list the session: both profiles then go on with the same conversation. A session without a folder shows there in a folder named `scratch-...`, not under "no folder", which is the other profile's area.
- **Copy** writes a new conversation with a new id: the transcript with every `sessionId` replaced, up to its last whole line; a session without a folder gets a copy of its scratch folder in the target's own area, with every `cwd` pointed at it (Windows shows the progress of a long folder copy, which can be cancelled). The target then opens it with the link, and the copy goes on separately. A copy that cannot be opened is deleted again.
- **Rename**, **Star** and **Remove** change that profile's entries of the session: `title` (with `titleSource` `user`), `isStarred`, or the entry itself, which goes to the Recycle Bin with Claude's own `deleted_<id>` marks (the time in ms) beside it, one for each id of the session that no other session there claims, as Claude's own delete does. A shared or copied session gets the title it has in the profile it came from.
- **Delete session everywhere** moves to the Recycle Bin, in one operation, what Claude's own delete removes: the session's entries in each profile (with the transcript Claude staged when it took the session in); its scratch folder, unless another session works in it; and for each of its transcripts that no other session goes on with, `<id>.jsonl`, the `<id>` folder, `.jsonl.pre-import`, `.ccr-tip.json`, `.precompact.json` and `.desktop-released.json` in every project folder, Claude Code's temporary folder of the session (`<temp>\claude\<project>\<id>`), and the session's `file-history`, `session-env`, `uploads`, `tasks`, `image-cache`, `debug`, `usage-data` and `startup-perf` items in the Claude Code folder. It then writes the `deleted_<id>` marks and drops the changes waiting for the session. It is refused while a profile that lists the session is open (that Claude would write it back), or while any Claude Code runs it.

Archiving stays in Claude: its own archive also cleans up the session's worktree and records it in `archived-sessions.idx`.

A running Claude keeps its sessions in memory and writes them back: an entry changed under it is overwritten. A change to a running profile therefore waits in `%LOCALAPPDATA%\Claude Desktop Profiles Manager\pending-sessions-<folder>.txt` and is made when that profile closes: by its watcher, before the manager starts it again, or when the sessions view finds it closed. The session then reads "Changes made when <profile> closes", and **Keep in <profile>** cancels a removal still waiting. A newer change replaces what waited of its kind, a removal replaces everything, and any other change cancels a waiting removal; at most 256 changes wait. A pending file is written to a temporary copy, flushed, then put in place, and one that cannot be read is never written over. Entry changes and pending-file rewrites share a per-profile mutex across processes; a waiting change is made only once Claude is checked closed, and a removal overtaken by Claude's start waits again. When a profile starts while Windows asks about a removal, its Claude keeps the session, and the entry goes when that Claude closes.

Claude Code records each session it runs in `<Claude Code folder>\sessions\<pid>.json` (`sessionId`, `procStart`). A session whose process still runs, with that start time, under a profile's Claude is "in use" there; one in use in two profiles at once gets a warning, since each goes on from what it read.

## Opening at sign-in

**Open at Windows sign-in** puts a profile shortcut (`Claude (<name>).lnk`) in the user's Startup folder, which Windows opens at sign-in: the profile starts with its own button and icon. Claude's own start-up setting cannot do this for a profile other than Main, since it starts Claude without arguments. The shortcut is recorded like the others.

## Settings copy

A new profile can start with some settings of an existing one. From `claude_desktop_config.json`: `mcpServers`, `isHardwareAccelerationDisabled` and `preferences.menuBarEnabled`; from `config.json`: `locale` and `userThemeMode`. Nothing else is copied: the sign-in, account ids and everything tied to an account stay behind. The files are written before the profile's first start and never over existing ones.

## Updates

When the manager opens, at most every 4 hours, it reads the tag of the latest release (`api.github.com/repos/<owner>/<repo>/releases/latest`) on a background thread. A newer one shows next to the version with an **Update** button, which downloads that release's `ClaudeDesktopProfilesManager.exe` to `%TEMP%` and checks it while keeping it open, so it cannot change before it runs: Windows must find it signed by the author (a valid signature, a trusted and unrevoked certificate whose common name is exactly the one the releases are signed with), and its file version must be the release's. Otherwise the file is deleted and nothing runs. The release check gives up after about a minute, the download after about ten. The new copy then installs itself over the current one (written next to it, then put in place in one step, the old copy kept on failure), restarts the watchers, reopens the manager and deletes the download. Nothing else is sent.

## claude:// links

A browser sign-in returns to the app through a `claude://` link, and Windows gives every such link to one program. Claude accepts a sign-in only in the window that opened the browser, so with several windows open the link must reach the right one.

### Which app opens them

Windows opens `claude://` links with the default app picked for `claude://`, kept in `HKCU\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\claude\UserChoiceLatest` (current Windows 11 builds; other builds use `UserChoice`). The choice is signed (`Hash`): only Windows' chooser and Settings can write it.

The manager registers as an app for `claude://`: the ProgId `ClaudeDesktopProfilesManager.Url`, which runs `"<ClaudeDesktopProfilesManager.exe>" --url "%1"`, its capabilities, a `RegisteredApplications` value and an `ApplicationAssociationToasts` value of 0, which keeps Windows from announcing the new app. After a first install, and from **Set up links**, while Claude Desktop is installed and links do not open in the manager, the manager says what to pick, and on **Continue** opens an empty `claude:` link so that Windows shows its chooser (the router ignores that link). Windows shows the chooser only when no app is picked, so **Set up links** first forgets another app's choice.

`HKCU\Software\Classes\claude` belongs to Claude, which rewrites it at every start: the manager leaves it alone.

### Routing a link

1. Spaces around the link are trimmed; it must start with `claude:` followed by something. Quotes, backslashes, spaces and control characters inside it are percent-encoded so it stays one argument; a link holding another kind of space (no-break, ideographic, any other Unicode space) is refused.
2. **Sign-in links** (`login`, `auth`, `magic-link`, `sso` or `callback` in the path, not in the query or fragment) go to the window that opened the browser. That window logs `[Auth] Using system browser for: /login/...` in its `main.log`; the link goes to the window with the latest such line from the last 15 minutes, else to every open window. Other windows ignore it.
3. **Other links** go to the Claude window used last, else the default profile's window, else the first one.
4. **No Claude window open**: the default profile starts with the link.

A link reaches a running window through a second activation with the same `--user-data-dir`: Chromium's single-instance lock hands the link over and the new process exits.

## Files and registry

| What | Where |
|---|---|
| Program | `%LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager\ClaudeDesktopProfilesManager.exe` |
| Start menu folder | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Claude Desktop Profiles Manager` (the manager, and the profiles added to the Start menu) |
| Settings | `HKCU\Software\Claude Desktop Profiles Manager`: `Profiles\<folder>` (name, color), `DefaultProfile`, `Language`, `InstallPath`, `Shortcuts` (the profile shortcuts made, which follow renames), `Update` (the last release check), `Capabilities` (the `claude://` link handler as Windows lists it) |
| Profile icons, log, session changes waiting for a profile to close | `%LOCALAPPDATA%\Claude Desktop Profiles Manager` |
| A downloaded update, until it has installed itself | `%TEMP%\update-ClaudeDesktopProfilesManager.exe` |
| Taskbar pins | Windows' own pin list |
| Apps & features entry | `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ClaudeDesktopProfilesManager` |
| Link handler | `HKCU\Software\Classes\ClaudeDesktopProfilesManager.Url`, value `Claude Desktop Profiles Manager` in `HKCU\Software\RegisteredApplications`, value `ClaudeDesktopProfilesManager.Url_claude` in `HKCU\Software\Microsoft\Windows\CurrentVersion\ApplicationAssociationToasts` |
| Default app for `claude://` | picked by the user in Windows: `...\UrlAssociations\claude\UserChoiceLatest` |

Uninstalling removes all of it, asks first when a kept profile is open with session changes still waiting for it, stops (and says so) when the watchers cannot be stopped, forgets the default-app choice when it is the manager (links then go back to Claude), gives open windows their Claude button back, and never touches `%APPDATA%\Claude`.

## Command line

| Option | Effect |
|---|---|
| none | opens the manager; a copy outside the install folder installs itself first |
| `--launch "<folder>"` | opens a profile (what shortcuts run) |
| `--url "<link>"` | routes a link |
| `--watch "<folder>" [<pid>]` | runs a profile's watcher, which also makes the session changes waiting for the profile when its Claude closes and opens the profile again after a Claude update (`<pid>`: the Claude process just started for it) |
| `--set-up-links` | opens the manager and offers to set up links |
| `--install [--quiet]` | installs this copy, then opens the manager (not with `--quiet`) |
| `--uninstall` | opens the uninstall dialog |

Any other option, or `--launch` and `--watch` without their folder, does nothing; a first argument that is not an option opens the manager.

## Limitations

- **The regular Claude icon opens Main**, with Claude's own button until the manager opens it or sends it a link: the Start menu and taskbar start Claude without arguments. The default profile only decides where `claude://` links go while Claude is closed.
- **After a Claude update** Windows opens Main again, even when only another profile was open. That Main keeps Claude's button unless it was open with its own before the update.
- **Shared Claude Code data**: `%USERPROFILE%\.claude` (or `CLAUDE_CONFIG_DIR`: Claude Code settings and memory) is the same for every profile. A `CLAUDE_CONFIG_DIR` set in a profile's own Claude settings is not followed.
- **Cowork** runs in one profile at a time: its VM is shared by the whole PC.
- **One sign-in at a time**: the newest `[Auth]` line wins. A second sign-in started in another window before the first one finishes sends the first one's link to that window, which ignores it; start that sign-in again.
- **Running profiles** cannot be deleted: quit them from the notification area first.
- **Deleted profiles**: a shortcut or pin to one offers to open the manager instead of starting Claude.
- **Linked folders**: deleting a profile whose folder is linked elsewhere removes the link and Claude's local files for it (`%LOCALAPPDATA%\<folder>`, `<folder>-Data`), not the folder it leads to.
- **Limits**: 32 profiles; `3p`, `Data`, `...-3p` and `...-Data` are refused as new profiles' names (they name the folder); a display name can be any text of 48 characters or fewer, without control characters.
- **Uninstall** keeps the profiles you choose in `%APPDATA%`, and a new install finds them again with default names and colors. A kept profile that was never opened and holds only the settings copied into it is removed.
- **Claude not installed**: only the MSIX packages are detected; the manager then shows **Get Claude**.

## Checking Claude's behavior

`build.cmd` runs `tests/test_claude.c`, which looks up in the installed Claude Desktop (`app.asar`, `Claude.exe`, its notification-area icons) and in the Claude Code it installed each fact this document relies on: the `claude://resume` link and the entry it makes, the quit, sign-in and virtualization log lines, where entries live and their fields, what Claude's own delete removes and the marks it leaves, the "no folder" area and its folder names, the window classes, how Claude Code finds transcripts and names project folders, and its running-session records. While a profile runs, it also checks the `lockfile` the watcher relies on. A Claude update that changes one of these facts makes the build fail with that fact's name; the checks read the installed files, they do not reproduce a clean installation.
