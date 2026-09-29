/*
 * Claude Desktop Profiles Manager - run several Claude Desktop accounts side by side on Windows.
 *
 * One executable, several roles (see main.c):
 *   (no arguments)      the profile manager window
 *   --launch <folder>   open a profile (what the shortcuts run)
 *   --url <claude://..> claude:// link handler: delivers the link to the right window
 *   --install           copy to %LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager and register
 *   --set-up-links      the manager, offering to make it the app for claude:// links
 *   --watch <folder>    gives that profile's Claude windows their own taskbar button
 *                       and its notification-area icon the profile's color *   --uninstall         the uninstall dialog (Apps & features entry)
 */
#ifndef CLAUDE_DESKTOP_PROFILES_MANAGER_APP_H
#define CLAUDE_DESKTOP_PROFILES_MANAGER_APP_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _WIN32_WINNT  0x0A00
#define NTDDI_VERSION 0x0A000006 /* Windows 10 1809 */

#include <windows.h>
#include <strsafe.h>
#include "version.h"

/* ------------------------------------------------------------------ names */

#define APP_NAME           L"Claude Desktop Profiles Manager"
#define APP_EXE            L"ClaudeDesktopProfilesManager.exe"
#define APP_WINDOW_CLASS   L"ClaudeDesktopProfilesManagerMain"
#define APP_AUMID_PREFIX   L"ClaudeDesktopProfilesManager."
#define APP_DOWNLOAD_URL   L"https://claude.ai/download"
#define APP_REPO           L"Freenitial/Claude_Desktop_Profiles_Manager"
#define APP_RELEASES_URL   L"https://github.com/" APP_REPO L"/releases"
#define APP_AUTHOR_URL     L"https://github.com/Freenitial"
/* Whom the release certificate is issued to (sign.cmd signs with it): an
 * update runs only with this signature. */
#define APP_SIGNER         L"Leo Antonin Gillet"

#define REG_ROOT           L"Software\\Claude Desktop Profiles Manager"
#define REG_PROFILES       REG_ROOT L"\\Profiles"
#define REG_SHORTCUTS      REG_ROOT L"\\Shortcuts"
#define REG_CAPABILITIES   REG_ROOT L"\\Capabilities"
#define REG_UNINSTALL      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ClaudeDesktopProfilesManager"
#define REG_PROGID         L"Software\\Classes\\ClaudeDesktopProfilesManager.Url"
#define PROGID_NAME        L"ClaudeDesktopProfilesManager.Url"
#define REG_REGISTERED     L"Software\\RegisteredApplications"
#define REG_USERCHOICE     L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\claude\\UserChoice"
#define REG_USERCHOICE2    L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\claude\\UserChoiceLatest"
/* The packages installed for the user: one subkey per package full name. */
#define REG_PACKAGES       L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages"

#define WATCH_MUTEX_PREFIX L"Local\\ClaudeDesktopProfilesManager.Watch."
#define WATCH_QUIT_EVENT   L"Local\\ClaudeDesktopProfilesManager.WatchQuit"
#define WATCH_HANDOVER_EVENT L"Local\\ClaudeDesktopProfilesManager.WatchHandover"
#define MANAGER_MUTEX      L"Local\\ClaudeDesktopProfilesManager.Manager"

#define STOCK_FOLDER       L"Claude"
#define PROFILE_PREFIX     L"Claude-"
#define STOCK_DEFAULT_NAME L"Main"

#define MAX_PROFILES       32
#define MAX_NAME           32   /* characters in a new profile name */
#define MAX_LABEL          48   /* characters in a display name */
#define FOLDER_CCH         64
#define LABEL_CCH          64
#define URL_CCH            4096
#define ARGS_CCH           (URL_CCH + 2 * MAX_PATH)
#define SIGNIN_WINDOW_MIN  15
#define PALETTE_SIZE       8
#define SESSION_TITLE_CCH  256
#define SESSION_ID_CCH     64

/* ------------------------------------------------------------------ types */

typedef struct Profile {
    WCHAR folder[FOLDER_CCH];  /* "Claude" (stock) or "Claude-<name>": the stable id */
    WCHAR name[LABEL_CCH];     /* display name, editable */
    WCHAR dataDir[MAX_PATH];   /* %APPDATA%\<folder>, as Claude sees it */
    WCHAR storageDir[MAX_PATH]; /* file access outside the package; empty when unresolved */
    int   color;               /* palette index */
    BOOL  isStock;             /* the folder the regular Claude icon opens */
    BOOL  running;
    DWORD pid;                 /* main process when running */
} Profile;

typedef struct ProfileList {
    Profile items[MAX_PROFILES];
    int     count;
    WCHAR   defaultFolder[FOLDER_CCH];
} ProfileList;

typedef struct ClaudePackage {
    BOOL  found;
    WCHAR fullName[256];
    WCHAR family[128];
    WCHAR aumid[256];
    WCHAR installDir[MAX_PATH];
    WCHAR exe[MAX_PATH];
    WCHAR version[32];
} ClaudePackage;

typedef enum RouteReason {
    ROUTE_NOTHING_RUNNING,  /* start the default profile */
    ROUTE_ONLY_ONE,
    ROUTE_SIGNIN,           /* the window that opened the browser */
    ROUTE_BROADCAST,        /* sign-in link no window claims: every window checks it */
    ROUTE_LAST_USED,        /* topmost Claude window */
    ROUTE_DEFAULT,
    ROUTE_FIRST
} RouteReason;

typedef struct LinkInfo {
    WCHAR target[MAX_PATH];
    WCHAR args[2048];
    WCHAR parsing[512];        /* shell parsing name when the target is not a file */
} LinkInfo;

typedef enum PendingOp { PENDING_TITLE, PENDING_STAR, PENDING_REMOVE } PendingOp;

typedef struct PendingEdit {   /* a change to a session entry, waiting for its profile to close */
    PendingOp op;
    WCHAR     key[SESSION_ID_CCH];         /* the session's id (cliSessionId, else the entry's own) */
    WCHAR     value[SESSION_TITLE_CCH];    /* title; "1"/"0" for a favorite */
} PendingEdit;

typedef struct CoreSwap { const char *from, *to; } CoreSwap;

typedef enum RemoveResult { REMOVE_DONE, REMOVE_CANCELLED, REMOVE_FAILED } RemoveResult;

/* ---------------------------------------------------------- core.c (pure) */

BOOL         Core_ValidateNewName(const WCHAR *raw, WCHAR *name, size_t nameCch,
                                  WCHAR *folder, size_t folderCch, const WCHAR **error);
BOOL         Core_ValidateLabel(const WCHAR *raw, WCHAR *label, size_t cch, const WCHAR **error);
BOOL         Core_IsProfileFolder(const WCHAR *folder);
BOOL         Core_SanitizeUrl(const WCHAR *in, WCHAR *out, size_t cch);
BOOL         Core_IsSignInUrl(const WCHAR *url);
BOOL         Core_BuildLaunchArgs(const WCHAR *dataDir, const WCHAR *url, WCHAR *out, size_t cch);
BOOL         Core_LatestSignInStart(const char *text, size_t len, SYSTEMTIME *latest);
BOOL         Core_LastQuit(const char *text, size_t len, SYSTEMTIME *when, BOOL *forUpdate);
int          Core_SelectTargets(int count, const ULONGLONG *signInTicks, ULONGLONG nowTicks,
                                ULONGLONG windowTicks, BOOL signInUrl, int lastUsed,
                                int defaultIdx, int *targets, RouteReason *reason);
const WCHAR *Core_RouteReasonText(RouteReason reason);
BOOL         Core_ArgsSelectProfile(const WCHAR *args, const WCHAR *folder);
BOOL         Core_ArgsReferenceDir(const WCHAR *args, const WCHAR *dir);
BOOL         Core_LinkOpensProfile(const LinkInfo *link, const WCHAR *folder,
                                   const WCHAR *dataDir, BOOL isStock);
BOOL         Core_IsOurExe(const WCHAR *path);
BOOL         Core_PathEquals(const WCHAR *a, const WCHAR *b);
BOOL         Core_EndsWithI(const WCHAR *s, const WCHAR *suffix);
BOOL         Core_ContainsI(const WCHAR *s, const WCHAR *needle);
DWORD        Core_Hash(const WCHAR *s);
void         Core_ProfileAumid(const WCHAR *folder, WCHAR *out, size_t cch);
void         Core_ShortcutFileName(const WCHAR *label, int copy, WCHAR *out, size_t cch);
ULONGLONG    Core_SystemTimeTicks(const SYSTEMTIME *st);
BOOL         Core_PathUnder(const WCHAR *path, const WCHAR *dir);
BOOL         Core_ProfileFilePath(const Profile *p, const WCHAR *path, WCHAR *out, size_t cch);
BOOL         Core_SameFatTime(const FILETIME *a, const FILETIME *b);
BOOL         Core_JsonMember(const char *json, size_t len, const char *key, const char **value, size_t *valueLen);
BOOL         Core_JsonString(const char *raw, size_t len, WCHAR *out, size_t cch);
BOOL         Core_JsonNumber(const char *raw, size_t len, ULONGLONG *value);
BOOL         Core_JsonTrue(const char *raw, size_t len);
typedef enum SessionEntryKind { ENTRY_NOT_ONE, ENTRY_LOCAL, ENTRY_ELSEWHERE } SessionEntryKind;
SessionEntryKind Core_SessionEntryKind(const char *json, size_t len);
BOOL         Core_JsonSetMember(const char *json, size_t len, const char *key, const char *raw, char *out, size_t cap, size_t *outLen);
BOOL         Core_JsonQuote(const WCHAR *s, char *out, size_t cap);
#define CORE_PROJECT_NAME_MAX 200
BOOL         Core_ProjectDirName(const WCHAR *cwd, WCHAR *out, size_t cch);
BOOL         Core_IsSessionId(const WCHAR *id);
BOOL         Core_ResumeLink(const WCHAR *sessionId, WCHAR *out, size_t cch);
void         Core_ScratchName(const SYSTEMTIME *day, DWORD random, WCHAR *out, size_t cch);
size_t       Core_ReplaceChunk(const char *in, size_t len, const CoreSwap *swaps, int count, BOOL last, char *out, size_t *used);
BOOL         Core_PendingFormat(const PendingEdit *e, WCHAR *out, size_t cch);
BOOL         Core_PendingParse(const WCHAR *line, PendingEdit *e);
#define CORE_SCROLL_EASE_MS 35   /* a wheel notch is 95 % done after three of these */
int          Core_ScrollStep(int pending, int elapsedMs);
#define CORE_HASH_START 14695981039346656037ULL
ULONGLONG    Core_HashBytes(ULONGLONG hash, const void *data, size_t size);
void         Core_CenterRect(const RECT *on, int width, int height, const RECT *work, RECT *out);
BOOL         Core_ParseVersion(const WCHAR *text, DWORD parts[4]);
int          Core_CompareVersions(const DWORD a[4], const DWORD b[4]);

/* --------------------------------------------------------------- util.c */

extern HINSTANCE g_hInst;

BOOL      Util_KnownFolder(const GUID *id, WCHAR *out, size_t cch);
BOOL      Util_AppData(WCHAR *out, size_t cch);        /* %APPDATA% */
BOOL      Util_LocalAppData(WCHAR *out, size_t cch);   /* %LOCALAPPDATA% */
BOOL      Util_SelfExe(WCHAR *out, size_t cch);
BOOL      Util_InstallDir(WCHAR *out, size_t cch);     /* %LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager */
BOOL      Util_InstallExe(WCHAR *out, size_t cch);
BOOL      Util_StateDir(WCHAR *out, size_t cch);       /* %LOCALAPPDATA%\Claude Desktop Profiles Manager */
BOOL      Util_FileExists(const WCHAR *path);
BOOL      Util_DirExists(const WCHAR *path);
BOOL      Util_ExistingDir(const WCHAR *path, WCHAR *out, size_t cch);
BOOL      Util_EnsureDir(const WCHAR *path);
BOOL      Util_RegGetString(HKEY root, const WCHAR *key, const WCHAR *value, WCHAR *out, DWORD cch);
BOOL      Util_RegSetString(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *data);
BOOL      Util_RegGetDword(HKEY root, const WCHAR *key, const WCHAR *value, DWORD *out);
BOOL      Util_RegSetDword(HKEY root, const WCHAR *key, const WCHAR *value, DWORD data);
BOOL      Util_RegKeyExists(HKEY root, const WCHAR *key);
BOOL      Util_RegValueExists(HKEY root, const WCHAR *key, const WCHAR *value);
void      Util_RegDeleteValue(HKEY root, const WCHAR *key, const WCHAR *value);
void      Util_RegDeleteTree(HKEY root, const WCHAR *key);
ULONGLONG Util_LocalNowTicks(void);
void      Util_Log(const WCHAR *fmt, ...);
void      Util_OpenUrl(const WCHAR *url);
BOOL      Util_Spawn(const WCHAR *exe, const WCHAR *args);
RemoveResult Util_Recycle(HWND owner, const WCHAR *const *paths, int count);
char     *Util_ReadFile(const WCHAR *path, DWORD maxBytes, BOOL tail, DWORD *len);

/* -------------------------------------------------------------- claude.c */

BOOL    Claude_FindPackage(ClaudePackage *pkg);
HRESULT Claude_Launch(const ClaudePackage *pkg, const Profile *profile, const WCHAR *url,
                      DWORD *pid, BOOL *withIdentity);
void    Claude_UpdateRunning(ProfileList *list);
BOOL    Claude_IsRunning(const Profile *profile);
BOOL    Claude_LastSignInStart(const ClaudePackage *pkg, const Profile *profile, ULONGLONG *ticks);
BOOL    Claude_ClosedForUpdate(const ClaudePackage *pkg, const Profile *profile);
int     Claude_TopmostProfile(const ProfileList *list);

/* ------------------------------------------------------------ profiles.c */

void Profiles_Load(ProfileList *list);
BOOL Profiles_ResolveStorage(Profile *p, const WCHAR *localAppData, const WCHAR *family);
int  Profiles_Find(const ProfileList *list, const WCHAR *folder);
int  Profiles_DefaultIndex(const ProfileList *list);
BOOL Profiles_Create(const WCHAR *name, int color, WCHAR *folder, size_t folderCch,
                     WCHAR *err, size_t errCch);
BOOL Profiles_Update(const WCHAR *folder, const WCHAR *label, int color);
void Profiles_SetDefault(const WCHAR *folder);
void Profiles_CopySettings(const Profile *from, const Profile *to);
RemoveResult Profiles_Delete(HWND owner, const Profile *profile);
RemoveResult Profiles_RecycleData(HWND owner, const Profile *profile);
BOOL         Profiles_IsLinked(const Profile *profile);
BOOL Profiles_LinkTarget(const Profile *profile, WCHAR *out, size_t cch);

/* --------------------------------------------------------------- icons.c */

extern const WCHAR *const g_ColorNames[PALETTE_SIZE];
BOOL  Icons_Ensure(const ClaudePackage *pkg, const Profile *profile, WCHAR *out, size_t cch);
HICON Icons_Create(const ClaudePackage *pkg, const Profile *profile, int size);
HICON Icons_CreateBadge(int color, int size);
HICON Icons_CreateTray(const ClaudePackage *pkg, const Profile *profile, int size, BOOL darkTaskbar);
BOOL  Icons_IsStale(const ClaudePackage *pkg, const Profile *profile);
void  Icons_DeleteStale(const Profile *profile, const WCHAR *keep);

/* ----------------------------------------------------------- shortcuts.c */

BOOL    Shortcut_Read(const WCHAR *lnk, LinkInfo *info);
BOOL    Shortcut_HasAppId(const WCHAR *lnk, const WCHAR *aumid);
HRESULT Shortcut_CreateForProfile(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk);
BOOL    Shortcut_IsAtStartup(const Profile *profile);
BOOL    Shortcut_StartMenuDir(WCHAR *out, size_t cch);
BOOL    Shortcut_IsInStartMenu(const Profile *profile);
HRESULT Shortcut_AddToStartMenu(const ClaudePackage *pkg, const Profile *profile);
void    Shortcut_RemoveFromStartMenu(const Profile *profile);
HRESULT Shortcut_SetStartup(const ClaudePackage *pkg, const Profile *profile, BOOL on);
HRESULT Shortcut_WriteForProfile(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk);
BOOL    Shortcut_DesktopPathFor(const Profile *profile, WCHAR *out, size_t cch);
BOOL    Shortcut_FindOnDesktop(const Profile *profile, WCHAR *found, size_t cch);
void    Shortcut_RemoveOurs(const Profile *profile);
HRESULT Shortcut_Update(const WCHAR *lnk, const Profile *after, const WCHAR *icon);
BOOL    Shortcut_RenamedPath(const WCHAR *path, const Profile *before, const Profile *after, WCHAR *out, size_t cch);
BOOL    Shortcut_Refresh(const Profile *before, const Profile *after, const WCHAR *icon);
DWORD   Shortcut_ListOurs(const WCHAR *folder, WCHAR (*paths)[MAX_PATH], DWORD max);
HRESULT Shortcut_CreateManagerLink(const WCHAR *exe);
void    Shortcut_RemoveManagerLink(void);

/* -------------------------------------------------------------- update.c */

typedef enum UpdateResult {
    UPDATE_READY,            /* downloaded and signed: Update_Run can start it */
    UPDATE_NOT_DOWNLOADED,
    UPDATE_NOT_SIGNED        /* downloaded, without the releases' signature: deleted */
} UpdateResult;

void Update_Check(HWND notify, UINT msg);
BOOL Update_Available(WCHAR *version, size_t cch);
void Update_Download(HWND notify, UINT msg);
BOOL Update_Run(void);
void Update_RemoveDownload(void);

/* --------------------------------------------------------- taskbar-pin.c */

BOOL    TaskbarPin_Dir(WCHAR *out, size_t cch);
BOOL    TaskbarPin_IsPinned(const Profile *profile);
BOOL    TaskbarPin_HasAppId(const WCHAR *aumid);
BOOL    TaskbarPin_UsesIcon(const WCHAR *icon);
HRESULT TaskbarPin_Pin(const ClaudePackage *pkg, const Profile *profile);
BOOL    TaskbarPin_Refresh(const Profile *before, const Profile *after, const WCHAR *icon);
void    TaskbarPin_RepairOurs(void);
void    TaskbarPin_RemoveOurs(void);
size_t  TaskbarPin_InjectAppId(const BYTE *item, size_t cb, const WCHAR *appId, BYTE *out, size_t cap);
size_t  TaskbarPin_RepairItem(const BYTE *item, size_t cb, BYTE *out, size_t cap);
BOOL    TaskbarPin_FitResolve(const BYTE *res, size_t len, const BOOL *dropped, size_t oldCount, size_t count,
                              BYTE *out, size_t cap, size_t *outLen);

/* ------------------------------------------------------------- handler.c */

typedef enum UserChoiceState { USERCHOICE_NONE, USERCHOICE_OURS, USERCHOICE_OTHER } UserChoiceState;

BOOL            Handler_Register(const WCHAR *exe);
UserChoiceState Handler_UserChoice(void);
BOOL            Handler_AskUser(void);
void            Handler_Unregister(void);

/* -------------------------------------------------------------- taskbar.c */

void Taskbar_Watch(const Profile *profile);
void Taskbar_Refresh(const Profile *profile, BOOL linksChanged);
void Taskbar_StopWatchers(BOOL giveBack);
BOOL Taskbar_IsWatched(const Profile *profile);
int  Taskbar_WatchRun(const WCHAR *folder);

/* --------------------------------------------------------- sessionstore.c */

typedef struct SessionEntry {          /* one profile's entry for a session */
    WCHAR     localId[SESSION_ID_CCH];
    WCHAR     file[MAX_PATH];          /* its local_*.json */
    WCHAR     title[SESSION_TITLE_CCH];
    BOOL      userTitle;               /* named by someone, not by Claude */
    BOOL      starred;
    BOOL      archived;
    ULONGLONG lastActivity;            /* ms since 1970 */
    /* Changes made here while the profile runs, waiting for it to close. */
    BOOL      pending;
    WCHAR     pendingTitle[SESSION_TITLE_CCH];   /* "" when unchanged */
    int       pendingStar;             /* -1 unchanged, 0 or 1 */
    BOOL      pendingRemove;
} SessionEntry;

typedef struct SessionRow {            /* one transcript */
    WCHAR     key[SESSION_ID_CCH];     /* its id, or the entry's own id before its first message */
    WCHAR     cwd[MAX_PATH];
    WCHAR     transcriptPath[MAX_PATH];
    int       group;
    BOOL      transcript;              /* on disk */
    ULONGLONG transcriptBytes;
    ULONGLONG lastActivity;
    DWORD     live;                    /* the profiles running it now, one bit each (MAX_PROFILES <= 32) */
    int       entry[MAX_PROFILES];     /* into SessionSet.entries; -1: that profile does not list it */
} SessionRow;

typedef struct SessionGroup {          /* a project folder, or one profile's "no folder" sessions */
    WCHAR     name[MAX_PATH];
    WCHAR     path[MAX_PATH];
    int       scratchOf;               /* that profile for "no folder", else -1 */
    ULONGLONG lastActivity;
    int       count;
} SessionGroup;

typedef struct SessionSource {         /* what reading one profile's entries found */
    BOOL      signedIn;                /* its config.json names an account */
    BOOL      found;                   /* it has entries (it opened the Code tab signed in) */
    int       unreadable;              /* entries that are not a session entry */
    int       elsewhere;               /* SSH, WSL or cloud sessions: their conversation is not on this PC */
    int       pending;                 /* changes waiting for it to close */
    WCHAR     entriesDir[MAX_PATH];    /* claude-code-sessions\<account>\<organization>, when found */
    WCHAR     scratchDir[MAX_PATH];    /* logical scratch-workspaces\<account>\<organization>: its "no folder" */
} SessionSource;

typedef struct SessionSet {
    ProfileList   profiles;
    SessionSource source[MAX_PROFILES];
    BOOL          noTranscripts;       /* no Claude Code transcripts folder */
    SessionEntry *entries;
    SessionRow   *rows;                /* by group, then last activity */
    SessionGroup *groups;
    int           entryCount, entryCap, rowCount, rowCap, groupCount, groupCap;
} SessionSet;

#define SESSION_PENDING_MAX 256

BOOL         SessionStore_Load(SessionSet *set);
BOOL         SessionStore_LoadProfiles(SessionSet *set, const ProfileList *profiles);
void         SessionStore_Free(SessionSet *set);
BOOL         SessionStore_ProjectsDir(WCHAR *out, size_t cch);
BOOL         SessionStore_SessionsDir(const Profile *p, WCHAR *out, size_t cch);
BOOL         SessionStore_WorkingDir(const SessionSet *set, const WCHAR *cwd, WCHAR *out, size_t cch);
BOOL         SessionStore_WatchDir(const Profile *p, WCHAR *out, size_t cch);
BOOL         SessionStore_PendingPath(const Profile *p, WCHAR *out, size_t cch);
int          SessionStore_LoadPending(const Profile *p, PendingEdit *edits, int max);
const WCHAR *SessionStore_RowTitle(const SessionSet *set, const SessionRow *row);

/* ---------------------------------------------------------- sessionedit.c */

HRESULT      SessionEdit_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *sessionId);
BOOL         SessionEdit_Change(const Profile *p, const SessionEntry *e, const PendingEdit *edit, BOOL *waiting);
BOOL         SessionEdit_Cancel(const Profile *p, const PendingEdit *edit);
int          SessionEdit_ApplyPending(const Profile *p);
void         SessionEdit_ApplyPendingFor(const WCHAR *folder);
BOOL         SessionEdit_CopyConversation(const SessionSet *s, int row, int target, WCHAR *newId, size_t idCch,
                                          WCHAR *error, size_t errorCch);
RemoveResult SessionEdit_DeleteEverywhere(HWND owner, const SessionSet *s, int row, WCHAR *error, size_t errorCch);

/* ------------------------------------------------------------- sessions.c */

#define WM_APP_SESSIONS (WM_APP + 12)   /* to the manager: session entries or transcripts changed on disk */

void         SessionsView_Init(HWND dlg);
void         SessionsView_Enter(const ClaudePackage *pkg, const WCHAR *folder);
void         SessionsView_Leave(void);
BOOL         SessionsView_Shown(void);
const WCHAR *SessionsView_Profile(void);
void         SessionsView_Reload(void);
BOOL         SessionsView_Command(WPARAM wp);
BOOL         SessionsView_ClearSearch(void);
BOOL         SessionsView_Notify(const NMHDR *h, LPARAM lp, LRESULT *result);
BOOL         SessionsView_ContextMenu(HWND from, LPARAM pos);
BOOL         SessionsView_DrawItem(const DRAWITEMSTRUCT *di);
void         SessionsView_Relayout(void);
void         SessionsView_Destroy(void);

/* ----------------------------------------------------------------- tray.c */

BOOL Tray_IsHost(HWND h);
BOOL Tray_Apply(const ClaudePackage *pkg, const Profile *profile);
void Tray_GiveBack(const ClaudePackage *pkg, DWORD pid);

/* -------------------------------------------------------------- install.c */

BOOL Install_IsInstalledCopy(void);
BOOL Install_IsRegistered(void);
BOOL Install_Run(BOOL openManager);
void Install_Repair(void);
BOOL Install_Uninstall(HWND owner, const ProfileList *list, const BOOL *removeData);
void Install_FinishUninstall(void);

/* --------------------------------------------------------------- theme.c */
/* The look of every window, in one place: Theme_Apply on a dialog themes its
 * controls (buttons, check boxes, lists and their headers, edits, combo boxes,
 * frames, focus rectangles, smooth wheel scrolling); what a window draws
 * itself takes its colors, fonts and rows from here. */

typedef enum ThemeColor {
    THEME_TEXT,
    THEME_MUTED,        /* secondary text */
    THEME_FACE,         /* window background */
    THEME_FIELD,        /* lists, trees, edits */
    THEME_MAIN_BLUE,    /* a selected row, selected text, a button's frame */
    THEME_BRIGHT_BLUE,  /* a selected row's frame; a button under the mouse or pressed */
    THEME_PALE_BLUE,    /* a row or a button under the mouse */
    THEME_COLORS
} ThemeColor;

#define THEME_ROW_SELECTED 0x1
#define THEME_ROW_HOT      0x2

#define THEME_BUTTON_HOT      0x1
#define THEME_BUTTON_PRESSED  0x2
#define THEME_BUTTON_DISABLED 0x4
#define THEME_BUTTON_DEFAULT  0x8

typedef enum ThemeFont {
    THEME_FONT_TEXT,
    THEME_FONT_STRONG,   /* semibold */
    THEME_FONT_HEADING,  /* semibold, larger */
    THEME_FONT_CURRENT,  /* semibold, underlined */
    THEME_FONT_ABSENT,   /* struck out */
    THEME_FONT_ITALIC,
    THEME_FONTS
} ThemeFont;

typedef struct ThemeFonts { HFONT font[THEME_FONTS]; } ThemeFonts;

typedef struct ThemeBuffer {   /* drawing off screen, shown at once */
    HDC     target, dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT    rc;
} ThemeBuffer;

void     Theme_Init(void);
BOOL     Theme_IsDark(void);
COLORREF Theme_Color(ThemeColor c);
HBRUSH   Theme_Brush(ThemeColor c);
COLORREF Theme_DrawRow(HDC dc, const RECT *rc, UINT state, COLORREF around);   /* returns the text color */
COLORREF Theme_RowMuted(UINT state);
void     Theme_DrawButton(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state, UINT format);
void     Theme_DrawDropDown(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state);   /* THEME_BUTTON_* */
void     Theme_SetStrong(HWND control);   /* its text semibold, at the dialog's scale */
void     Theme_CreateFonts(HWND dlg, ThemeFonts *fonts);
void     Theme_FreeFonts(ThemeFonts *fonts);
HDC      Theme_BufferBegin(ThemeBuffer *b, HDC target, const RECT *rc);
void     Theme_BufferEnd(ThemeBuffer *b);
void     Theme_SetScrollRow(HWND list, int rowPx);   /* how far a wheel line scrolls, 0: one row of the control */
HWND     Theme_SmoothView(HWND control);   /* a list or tree scrolled by the pixel; returns the view, which has its place */
void     Theme_Follow(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);
void     Theme_Forget(HWND dlg);
void     Theme_Apply(HWND dlg);
INT_PTR  Theme_CtlColor(UINT msg, WPARAM wp, LPARAM lp, int mutedId);
BOOL    Ui_Ask(HWND owner, LPCWSTR icon, const WCHAR *text, const WCHAR *ok, const WCHAR *cancel, BOOL defaultCancel);
int     Util_Message(HWND owner, UINT flags, const WCHAR *fmt, ...);   /* MessageBox look-alike */
INT_PTR Ui_Dialog(HWND owner, int id, DLGPROC proc, LPARAM param);   /* every dialog opens here */

/* ------------------------------------------------ router.c / main.c / gui.c */

int     Router_Run(const WCHAR *rawUrl);
int     Launcher_Run(const WCHAR *folder);
HRESULT Launcher_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *url, DWORD *pid, BOOL *identity);
typedef enum GuiStart { GUI_MANAGER, GUI_SET_UP_LINKS, GUI_UNINSTALL } GuiStart;
int Gui_Run(GuiStart start);

#endif
