# How Claude Desktop Profiles Manager works

How Claude Desktop Profiles Manager runs one Claude Desktop per profile, tells which ones are running, gives each its own taskbar button, pin and notification-area icon, and routes `claude://` links.

Measured on Claude Desktop 2.9939 (MSIX) and Windows 11 26200. All of it relies on undocumented behavior of Claude or Windows.

## Profiles

Claude Desktop is an Electron (Chromium) app, and Chromium allows one instance per user data folder. Started with another `--user-data-dir`, it runs a second, independent instance with its own sign-in.

| Profile | Data folder | Started with |
|---|---|---|
| Main | `%APPDATA%\Claude` | no argument, like the Start menu |
| Others | `%APPDATA%\Claude-<name>` | `--user-data-dir="%APPDATA%\Claude-<name>"` |

- Folders stay directly under `%APPDATA%`: Cowork's VM service looks for its disk image in `%APPDATA%\<folder>\vm_bundles`.
- A folder is never renamed, since Claude stores absolute paths in it. Renaming a profile only changes its display name, kept in `HKCU\Software\Claude Desktop Profiles Manager\Profiles\<folder>`.
- Claude itself uses `%APPDATA%\Claude-3p` and `%LOCALAPPDATA%\<folder>-Data`, so the names `3p`, `Data` and `...-Data` are refused.
- A profile is `%APPDATA%\Claude`, or any `%APPDATA%\Claude-*` folder that holds a Chromium `Local State` file or was created by Claude Desktop Profiles Manager.

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

Claude Desktop Profiles Manager uses `ActivateApplication(<family>!Claude, arguments)`: the process gets its identity and the arguments arrive untouched. `CreateProcess` is only a fallback when activation fails. Claude logs which case it is in at start-up (the `[MSIX]` lines of `main.log`, `windowsStore=true|false`).

With its identity, Claude writes to AppData through the package's private copy, `%LOCALAPPDATA%\Packages\<family>\LocalCache`:

- a `--user-data-dir` that does not exist yet is created there, under `LocalCache\Roaming`. Claude Desktop Profiles Manager therefore creates a profile's folder before its first start (never Main's);
- a profile's logs (`%LOCALAPPDATA%\<folder>\logs`) can land in `LocalCache\Local\<folder>\logs`, so both places are read, and both are removed with the profile.

The package is `Claude_pzs8sxrjxfjjc` (from claude.ai) or `AnthropicPBC.Claude_fnn82j28hfe8t` (Microsoft Store), else any other package named Claude.

A process started from Claude Desktop (a Claude Code terminal, for example) runs inside that package: it sees the private copy, not the real registry and AppData. Test from a process started by Explorer or WMI.

## Running profiles

A running Chromium instance owns a message-only window of class `Chrome_MessageWindow`, titled with its data folder; a second launch uses it to hand over its arguments. Claude Desktop Profiles Manager lists those windows, without opening or locking anything. The window's process is the profile's main process, and its most recently used Claude window is the first one in Z-order.

## Taskbar and notification area

A profile opened from Claude Desktop Profiles Manager gets its own taskbar button with its badge, can be pinned from the manager, and shows its notification-area icon in its color (while Claude's **System tray** setting is on). A small watcher, `ClaudeDesktopProfilesManager.exe --watch "<folder>"`, does this for as long as that profile's Claude runs: it only reacts to events and exits with Claude, unless Claude closed for an update (below). It knows that the profile's Claude has started when that Claude creates its `Chrome_MessageWindow`. Renaming or recoloring a profile updates its button, pin and icon. At uninstall the windows and icons go back to Claude's own and the profiles' pins are removed.

A profile started without Claude Desktop Profiles Manager (the regular Claude icon) keeps Claude's button.

The taskbar part is the Taskbar Module, under its own license (see `LICENSE`); the notification-area icon (`tray.c`) is not.

### Claude updates

Measured with the updates to 2.7032, 2.9939.2 and 2.9939.4:

1. Claude's updater, in one of the open windows, logs `beforeQuitForUpdate handler fired, going down for update`.
2. Windows closes every Claude window to replace the package; each logs `Windows session ending (close-app) - quitting the app`.
3. Windows installs the new version and opens Claude again once, without arguments. That starts Main, 1 to 33 seconds later, even when only another profile was open. The other profiles stay closed.

When its Claude exits, the watcher reads the last quit line of the profile's `main.log` (both places Claude writes it). If that line is one of the two above and is from the last five minutes, the watcher waits for a Claude package other than the one the closed Claude ran. The wait lasts at most 2 minutes; changes to the user's package list (`HKCU\Software\Classes\Local Settings\...\AppModel\Repository\Packages`) and to the Apps folder wake it. Once the new package is there:

- Main is first given a minute to come back from Windows' own restart; any other profile opens again at once, like from its shortcut.
- The watcher then watches the new window: button, badge and notification-area color come back.

A quit from the window or the tray (`Quitting app...`, `beforeQuit:`, `willQuit:`), a Windows shutdown or a crash leaves the profile closed. So does a close for an update after which no new package comes.

## Start menu

Install puts a **Claude Desktop Profiles Manager** folder in the user's Start menu (`%APPDATA%\Microsoft\Windows\Start Menu\Programs\Claude Desktop Profiles Manager`) with the manager's shortcut. **Add to Start menu** puts a profile shortcut (`Claude (<name>).lnk`) in that folder; **Remove from Start menu** deletes it. The shortcut is recorded like the others, so it follows renames and colors and goes with the profile; uninstall removes the folder. The manager's shortcut goes last, once the manager's windows are closed: the taskbar takes their icon from it (it carries their AppUserModelID), and a window shown after it is deleted, once the uninstall has told Windows that the link handlers changed, gets a blank icon. The button shows which one applies: the manager looks again when the selection changes, when its window comes back to the front, and when that folder, Programs, the desktop or the taskbar pins folder changes (a shell change notification), so a shortcut removed elsewhere shows at once.

## The manager window

The manager follows changes made outside it without polling. A thread waits for registry notifications on `HKCU\Software\Claude Desktop Profiles Manager` (names, colors, default), on `...\Shell\Associations\UrlAssociations` (the app picked for `claude://`) and on the user's package list (Claude installed or updated), and for a folder notification on `%APPDATA%` (profile folders made or removed); a burst of them refreshes the window once. Coming back to the window refreshes it too.

## Claude Code sessions

A Code-tab session is stored in two places:

- its conversation, the transcript: `%USERPROFILE%\.claude\projects\<project>\<id>.jsonl`, in the Claude Code folder every profile shares;
- one entry per profile that lists it, in that profile's `claude-code-sessions\<account>\<organization>\local_<...>.json`: title, favorite (`isStarred`), folder, archived, last activity.

Claude shows the entries of the account signed in (`lastKnownAccountUuid` in the profile's `config.json`) and its current organization, taken as the one with the latest entry; the entries of an account signed in before stay on disk but are not shown. A session without a project folder works in `<profile data>\scratch-workspaces\<account>\<organization>\scratch-...`: Claude shows it under "no folder" only in the profile that owns that area. A session run over SSH (`sshConfig` in its entry), in WSL (`wslConfig`) or in the cloud (`cloudSessionId`, `movedToCloud`) has its conversation elsewhere: the manager does not list it, it only counts it.

**Sessions >**, at the top left of the manager, turns the window to its sessions view; **< Back** turns it back. On the left the profiles, with how many sessions each lists; in the middle the chosen profile's favorites, folders and sessions as a tree, which the search box filters; on the right the selected session: its folder's path (a button that opens it in Explorer), when it was last used and the size of its conversation stay at the top; **Delete session everywhere** stays at the bottom; between them each profile (struck out where the session is not listed) with a star where it is a favorite, its title there when it differs (in italics), open or in use there, archived there, and an **Actions** menu, scrolling when there are more profiles than room. The same actions are in the tree's context menu; Enter or a double click opens the session, F2 renames it, Delete removes it. An empty tree says why (not signed in, no session yet, all archived, nothing matches the search, only remote sessions); below the session, notes say what is missing (remote sessions not listed, entries that could not be read, no transcripts folder). While it shows, the view follows the disk through folder notifications on each profile's entries and on the transcripts folder; a burst of changes reloads it once.

### Session actions

- **Open** starts the profile, or reaches its window, with `claude://resume?session=<id>`. Claude opens the session, and first adds its entry when the profile does not list it yet (`local_<id>`).
- **Share** sends that link to a profile that does not list the session: both profiles then go on with the same conversation. A session without a folder shows there in a folder named `scratch-...`, not under "no folder", which is the other profile's area.
- **Copy** writes a new conversation with a new id: the transcript with every `sessionId` replaced, and for a session without a folder, its `cwd` pointed at a copy of its scratch folder made in the target's own area. The target then opens it with the link; the copy goes on separately.
- **Rename**, **Favorite** and **Remove** change that profile's entry: `title` (with `titleSource` `user`), `isStarred`, or the entry itself, which goes to the Recycle Bin. A shared or copied session gets the title it has in the profile it came from.
- **Delete session everywhere** moves every entry of the session, its transcript (`<id>.jsonl`) and its `<id>` folder, in every project folder, to the Recycle Bin. It waits until the profiles that list the session are closed.

Archiving stays in Claude: its own archive also cleans up the session's worktree and records it in `archived-sessions.idx`.

A running Claude keeps its sessions in memory and writes them back: an entry changed under it is overwritten, and one added under it shows only at its next start. A change to a running profile therefore waits in `%LOCALAPPDATA%\Claude Desktop Profiles Manager\pending-sessions-<folder>.txt`, one `title`, `star` or `remove` line per change, and is made when that profile closes: by its watcher once Claude has exited, before Claude Desktop Profiles Manager starts it again, or when the sessions view finds it closed. The session then reads "Changes made when <profile> closes", and **Keep** cancels a removal still waiting. Every file is written to a temporary copy first, then put in place.

Claude Code records each session it runs in `%USERPROFILE%\.claude\sessions\<pid>.json` (`pid`, `sessionId`, `procStart`). A session whose process still runs, with the same start time, under a profile's Claude process is "in use" there. A session in use in two profiles at once gets a warning: each profile goes on from what it read, so go on in one of them only.

`build.cmd` also runs `tests/test_claude.c`, which looks up in the installed Claude Desktop (its `app.asar` and `Claude.exe`) and in the Claude Code it installed each fact this document relies on: the `claude://resume` link, the quit and sign-in log lines, the session entry fields (and the ones that mark a remote session), the "no folder" area (`userData` + `scratch-workspaces`) and how its folders are named, the window classes, the transcript lookup by id, how a project folder is named after its path, the running-session records. A Claude update that changes one of them makes the build fail with that fact's name.

## Opening at sign-in

**Open at Windows sign-in** puts a profile shortcut (`Claude (<name>).lnk`) in the user's Startup folder, which Windows opens at sign-in: the profile starts with its own button and icon. Claude's own start-up setting cannot do this for a profile other than Main, since it starts Claude without arguments. The shortcut is recorded like the others, so it follows renames and goes with the profile and at uninstall.

## Settings copy

A new profile can start with some settings of an existing one. From `claude_desktop_config.json`: `mcpServers`, `isHardwareAccelerationDisabled` and `preferences.menuBarEnabled`; from `config.json`: `locale` and `userThemeMode`. Nothing else is copied: the sign-in (`oauth:tokenCache`), account ids and everything tied to an account stay behind. The files are written before the profile's first start and never over existing ones.

## Updates

When the manager opens, at most every 4 hours, it reads the tag of the latest release (`api.github.com/repos/<owner>/<repo>/releases/latest`) on a background thread and compares it with its own version. A newer one shows next to the version with an **Update** button, which downloads that release's `ClaudeDesktopProfilesManager.exe` to `%TEMP%`, checks that Windows finds it signed by the author (a valid signature, a trusted and unrevoked certificate issued to the name the releases are signed with; otherwise the file is deleted and nothing runs) and runs it: it installs itself over the current copy, restarts the watchers and reopens the manager. Nothing else is sent, and nothing runs on a timer.

## claude:// links

A browser sign-in returns to the app through a `claude://` link, and Windows gives every such link to one program. Claude accepts a sign-in only in the window that opened the browser, so with several windows open the link must reach the right one.

### Which app opens them

Windows opens `claude://` links with the default app picked for `claude://`, kept in `HKCU\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\claude\UserChoiceLatest` (current Windows 11 builds; other builds use `UserChoice`). The choice is signed (`Hash`): only Windows' chooser and Settings can write it.

Claude Desktop Profiles Manager registers as an app for `claude://`: the ProgId `ClaudeDesktopProfilesManager.Url`, which runs `"<ClaudeDesktopProfilesManager.exe>" --url "%1"`, its capabilities, a `RegisteredApplications` value and an `ApplicationAssociationToasts` entry. Windows then asks the user once. After install, and from **Set up links** while links do not open in Claude Desktop Profiles Manager, it opens an empty `claude:` link so that Windows shows its chooser (the router ignores that link). Windows shows the chooser only when no app is picked, so **Set up links** first forgets another app's choice.

`HKCU\Software\Classes\claude` belongs to Claude, which rewrites it at every start: Claude Desktop Profiles Manager leaves it alone.

### Routing a link

1. The link must start with `claude:`. Quotes, backslashes, spaces and control characters are percent-encoded so it stays one argument.
2. **Sign-in links** (`login`, `auth`, `magic-link`, `sso` or `callback` in the path, not in the query or fragment) go to the window that opened the browser. That window logs `[Auth] Using system browser for: /login/...` in its `main.log` (`%LOCALAPPDATA%\<folder>\logs`, or the package's `LocalCache\Local\<folder>\logs`); the link goes to the window with the latest such line from the last 15 minutes, else to every open window. Other windows ignore it (`Google sign-in code does not answer a sign-in this app started; ignoring`).
3. **Other links** go to the Claude window used last, else the default profile's window, else the first one.
4. **No Claude window open**: the default profile starts with the link.

A link reaches a running window through a second activation with the same `--user-data-dir`: Chromium's single-instance lock hands the link over and the new process exits.

## Files and registry

| What | Where |
|---|---|
| Program | `%LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager\ClaudeDesktopProfilesManager.exe` |
| Start menu folder | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Claude Desktop Profiles Manager` (the manager, and the profiles added to the Start menu) |
| Profile names, colors, default | `HKCU\Software\Claude Desktop Profiles Manager` |
| Profile icons, log, session changes waiting for a profile to close | `%LOCALAPPDATA%\Claude Desktop Profiles Manager` |
| Taskbar pins | Windows' own pin list |
| Apps & features entry | `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\ClaudeDesktopProfilesManager` |
| Link handler | `HKCU\Software\Classes\ClaudeDesktopProfilesManager.Url`, value `Claude Desktop Profiles Manager` in `HKCU\Software\RegisteredApplications`, value `ClaudeDesktopProfilesManager.Url_claude` in `HKCU\Software\Microsoft\Windows\CurrentVersion\ApplicationAssociationToasts` |
| Default app for `claude://` | picked by the user in Windows: `...\UrlAssociations\claude\UserChoiceLatest` |

Uninstalling removes all of it, forgets the default-app choice when it is Claude Desktop Profiles Manager (links then go back to Claude), gives open windows their Claude button back, and never touches `%APPDATA%\Claude`.

## Command line

| Option | Effect |
|---|---|
| none | opens the manager; a copy outside the install folder installs itself first |
| `--launch "<folder>"` | opens a profile (what shortcuts run) |
| `--url "<link>"` | routes a link |
| `--watch "<folder>"` | runs a profile's watcher, then makes the session changes waiting for it |
| `--set-up-links` | opens the manager and offers to set up links |
| `--install [--quiet]` | installs this copy |
| `--uninstall` | opens the uninstall dialog |

Any other option does nothing.

## Limitations

- **The regular Claude icon opens Main**, with Claude's own button: the Start menu and taskbar start Claude without arguments. The default profile only decides where `claude://` links go while Claude is closed.
- **After a Claude update** Windows opens Main again, even when only another profile was open. That Main keeps Claude's button unless it was open with its own before the update.
- **Shared Claude Code data**: `%USERPROFILE%\.claude` (Claude Code settings and memory) is the same for every profile.
- **Cowork** runs in one profile at a time: its VM is shared by the whole PC.
- **One sign-in at a time**: the newest `[Auth]` line wins. A second sign-in started in another window before the first one finishes sends the first one's link to that window, which ignores it; start that sign-in again.
- **Running profiles** cannot be deleted: quit them from the notification area first.
- **Deleted profiles**: a shortcut or pin to one offers to open Claude Desktop Profiles Manager instead of starting Claude.
- **Linked folders**: a `%APPDATA%\Claude-*` folder moved elsewhere and linked back (junction or directory symlink) counts once its `Local State` is reachable. Deleting that profile removes the link and Claude's local files for it (`%LOCALAPPDATA%\<folder>`, `<folder>-Data`), not the folder it leads to.
- **Limits**: 32 profiles; `3p`, `Data` and `...-Data` are refused as names.
- **Uninstall** keeps the profiles you choose in `%APPDATA%`, and a new install finds them again with default names and colors. A kept profile that was never opened is an empty folder, so it is removed.
- **Claude not installed**: only the MSIX packages are detected; the manager then shows **Get Claude**.
