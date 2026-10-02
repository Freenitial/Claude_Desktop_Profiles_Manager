# Claude Desktop Profiles Manager

Run several Claude Desktop accounts side by side on Windows, each with **its own sign-in, history and settings**.

Unofficial tool, not affiliated with Anthropic.

![Claude Desktop Profiles Manager window in dark theme, listing the Personal and Work profiles](docs/screenshot.png)

## Features

- **Profile manager** - Lists existing profiles; creates, renames, recolors and deletes them (to the Recycle Bin)
- **Sign-in routing** - Browser sign-ins return to the window that started them, even with several open
- **Profile badges** - Each profile's shortcut and taskbar button show the Claude icon with its own colored badge, and its notification-area icon takes its color
- **Taskbar pins** - Pin a profile to the taskbar in one click
- **Start menu** - Add a profile to the Start menu, or take it out, in one click
- **Open at sign-in** - Any profile can open when you sign in to Windows, not only the one the regular Claude icon opens
- **Claude Code sessions** - The manager shows each profile's Code sessions by folder and where each one is listed; open a session in any profile, share it with another one or copy it there, rename it, star it or remove it per profile, or delete it everywhere. Changes to a profile that is open are made when it closes
- **Twelve languages** - The manager follows your Windows language, or the one you pick in its Language menu
- **Settings copy** - A new profile can start with the MCP servers, notification-area icon, hardware acceleration setting, language and theme of another one
- **Updates** - Tells you when a new version is out and installs it in one click, once Windows has checked its signature
- **Claude unchanged** - Starts the installed app like the Start menu does, so Claude keeps updating itself
- **Lightweight** - One native exe, no runtime, no admin rights

## Requirements

- Windows 10 1809+ (64-bit)
- [Claude Desktop](https://claude.ai/download), from claude.ai or the Microsoft Store

## Installation

**[Download ClaudeDesktopProfilesManager.exe](../../releases/latest/download/ClaudeDesktopProfilesManager.exe)**, then double-click it.

It installs itself for the current user in `%LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager`, adds a Claude Desktop Profiles Manager folder to the Start menu and opens; the downloaded file can then be deleted. Claude Desktop Profiles Manager then asks Windows once which app opens claude:// links: choose **Claude Desktop Profiles Manager**, then **Always**. To update, download and run the new version the same way.

To uninstall, click **Uninstall…** or use Settings → Apps: it asks which profiles to keep and sends the others to the Recycle Bin. Claude Desktop and its own data are never touched.

## Usage

1. Click **New profile…**, enter a name, pick a color and click **OK**: the profile opens in its own window
2. Sign in to Claude in that window
3. Click **Create shortcut on desktop** or **Pin to taskbar**, then open the profile from there
4. *(Optional)* Select a profile and click **Set as default**: it opens `claude://` links while Claude is closed

How it works: see [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

## Notes and limitations

- The regular Claude icon always opens Main, with Claude's plain taskbar button until Claude Desktop Profiles Manager opens it or sends it a link. Main is Claude's own `%APPDATA%\Claude`; the others live in `%APPDATA%\Claude-<name>` and open from their shortcuts
- Cowork runs in one profile at a time: its virtual machine is shared by the whole PC
- Claude Desktop settings (theme, language, notification-area icon, MCP servers, extensions) belong to each profile; Claude Code settings and memory (`%USERPROFILE%\.claude`) are shared by every profile
- When its window opens, Claude Desktop Profiles Manager asks GitHub for the latest release, at most every 4 hours
- A profile shows its icon in the notification area while Claude's own **System tray** setting is on for that profile
- When Claude updates itself, it closes every open profile: they open again once the update is installed, with their badge and colors (Windows also reopens Main, even if it was closed)
- Tested with Claude Desktop 2.16120.0. Sign-in routing relies on undocumented behavior that a Claude update can change

## Troubleshooting

| What you see | What to do |
|---|---|
| The browser sign-in finishes, but the window stays signed out | Sign in again from that window. If it still fails, click **Set up links** in Claude Desktop Profiles Manager, when shown |
| Windows asks which app opens a Claude link | Choose **Claude Desktop Profiles Manager**, then **Always** |
| A shortcut says its profile no longer exists | The profile was deleted: delete or unpin the shortcut |
| Something else | Read the log: `%LOCALAPPDATA%\Claude Desktop Profiles Manager\claude-desktop-profiles-manager.log` |

## Build from source

See [BUILD.md](BUILD.md).

## Credits

- [ai-multi-instance](https://github.com/Zoltak-Dev/ai-multi-instance) - the `--user-data-dir` technique for Claude Desktop
- [claude-desktop-clone](https://github.com/vodongha/claude-desktop-clone) and [claude-windows-multiprofile](https://github.com/fredless/claude-windows-multiprofile) - earlier PowerShell tools for the same job

---

## License

Partially open source - enterprise or commercial usage of the Taskbar Module requires a paid license. See [LICENSE](LICENSE) for details.

The commercial license includes:

- Reverse engineering documentation (Markdown, 3k lines) covering how the taskbar-related DLL files work (blob format, COM vtables, notification chains, WFC gating, PIDL structures, etc.)
- Support within reasonable limits

Contact: [freenitial@gmail.com](mailto:freenitial@gmail.com)
