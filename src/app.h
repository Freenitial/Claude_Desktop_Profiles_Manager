/*
 * Claude Desktop Profiles Manager - run several Claude Desktop accounts side by side on Windows.
 *
 * One executable, several roles (see main.c):
 *   (no arguments)      the profile manager window
 *   --launch <folder>   open a profile (what the shortcuts run)
 *   --url <claude://..> claude:// link handler: delivers the link to the right window
 *   --install [--quiet] copy to %LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager, register,
 *                       then open the installed manager (not with --quiet)
 *   --set-up-links      the manager, offering to make it the app for claude:// links
 *   --watch <folder> [pid]  gives that profile's Claude windows their own taskbar button
 *                       and its notification-area icon the profile's color, makes the
 *                       session changes waiting for the profile each time its Claude
 *                       closes, and opens it again after a Claude update (pid: the
 *                       Claude process just started for it)
 *   --uninstall         the uninstall dialog (Apps & features entry)
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
#include "localize.h"

/* ------------------------------------------------------------------ names */

#define APP_NAME           L"Claude Desktop Profiles Manager"
#define APP_EXE            L"ClaudeDesktopProfilesManager.exe"
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
#define REG_USERCHOICE_LATEST L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\claude\\UserChoiceLatest"
/* The packages installed for the user: one subkey per package full name. */
#define REG_PACKAGES       L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages"
/* The taskbar's pin list (taskbar-pin.c). */
#define REG_TASKBAND       L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband"
#define TASKBAR_WINDOW_CLASS L"Shell_TrayWnd"   /* the primary taskbar */

#define WATCH_MUTEX_PREFIX L"Local\\ClaudeDesktopProfilesManager.Watch."
#define WATCH_QUIT_EVENT   L"Local\\ClaudeDesktopProfilesManager.WatchQuit"
#define WATCH_HANDOVER_EVENT L"Local\\ClaudeDesktopProfilesManager.WatchHandover"
#define MANAGER_MUTEX      L"Local\\ClaudeDesktopProfilesManager.Manager"
#define LOG_MUTEX          L"Local\\ClaudeDesktopProfilesManager.Log"
#define SESSION_MUTEX_PREFIX L"Local\\ClaudeDesktopProfilesManager.Session."

#define STOCK_FOLDER       L"Claude"
#define PROFILE_PREFIX     L"Claude-"
#define STOCK_DEFAULT_NAME L"Main"
#define CLAUDE_DESKTOP_SETTINGS L"claude_desktop_config.json"   /* in a profile's data folder */
#define CLAUDE_APP_SETTINGS L"config.json"                      /* ...its app state: the account signed in */

#define MAX_PROFILES       32
C_ASSERT(MAX_PROFILES <= 32);   /* sets of profiles are DWORD bit masks */
#define MAX_NAME           32   /* characters in a new profile name */
#define MAX_LABEL          48   /* characters in a display name */
#define FOLDER_CCH         64
#define LABEL_CCH          64
#define URL_CCH            4096
/* A path that can pass MAX_PATH, as Claude writes some. A file of a session:
 * Claude Code's or a profile's folder (shorter than MAX_PATH), then at most a
 * project folder and a file (255 characters each), with the \\?\ prefix. */
#define LONG_PATH_CCH      1024
#define SIGNIN_MAX_AGE_MINUTES 15   /* a sign-in started longer ago claims no link */
#define TICKS_PER_SECOND   10000000ULL   /* FILETIME units */
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
    WCHAR     value[SESSION_TITLE_CCH];    /* title; "1"/"0" for a star */
} PendingEdit;

typedef struct CoreSwap { const char *from, *to; } CoreSwap;

typedef enum RemoveResult { REMOVE_DONE, REMOVE_CANCELLED, REMOVE_FAILED } RemoveResult;

typedef enum SessionEntryKind { ENTRY_NOT_SESSION, ENTRY_LOCAL, ENTRY_ELSEWHERE } SessionEntryKind;

#define CORE_PROJECT_NAME_MAX 200
#define CORE_SCROLL_EASE_MS   35   /* a wheel notch is 95 % done after three of these */
#define CORE_HASH_START       14695981039346656037ULL

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
                                ULONGLONG signInMaxAgeTicks, BOOL signInUrl, int lastUsed,
                                int defaultIndex, int *targets, RouteReason *reason);
BOOL         Core_ArgsSelectProfile(const WCHAR *args, const WCHAR *folder);
BOOL         Core_ArgsReferenceDir(const WCHAR *args, const WCHAR *dir);
BOOL         Core_NamesClaudePackage(const WCHAR *text);
BOOL         Core_LinkOpensProfile(const LinkInfo *link, const WCHAR *folder,
                                   const WCHAR *dataDir, BOOL isStock);
BOOL         Core_IsOurExe(const WCHAR *path);
BOOL         Core_EqualsI(const WCHAR *a, const WCHAR *b);
size_t       Core_TrimmedPathLength(const WCHAR *path);
int          Core_PathCompare(const WCHAR *a, const WCHAR *b);
BOOL         Core_PathEquals(const WCHAR *a, const WCHAR *b);
BOOL         Core_EndsWithI(const WCHAR *s, const WCHAR *suffix);
BOOL         Core_ContainsI(const WCHAR *s, const WCHAR *needle);
DWORD        Core_HashIgnoringCase(const WCHAR *text);
void         Core_ProfileAumid(const WCHAR *folder, WCHAR *out, size_t cch);
void         Core_ShortcutFileName(const WCHAR *label, int copyNumber, WCHAR *out, size_t cch);
ULONGLONG    Core_SystemTimeTicks(const SYSTEMTIME *st);
BOOL         Core_PathUnder(const WCHAR *path, const WCHAR *dir);
BOOL         Core_ProfileFilePath(const Profile *p, const WCHAR *path, WCHAR *out, size_t cch);
BOOL         Core_PackageCachePath(const WCHAR *localAppData, const WCHAR *family, const WCHAR *area, const WCHAR *name,
                                   WCHAR *out, size_t cch);
BOOL         Core_SameFatTime(const FILETIME *a, const FILETIME *b);
BOOL         Core_JsonMember(const char *json, size_t len, const char *key, const char **value, size_t *valueLen);
BOOL         Core_JsonString(const char *raw, size_t len, WCHAR *out, size_t cch);
BOOL         Core_JsonNumber(const char *raw, size_t len, ULONGLONG *value);
BOOL         Core_JsonTrue(const char *raw, size_t len);
SessionEntryKind Core_SessionEntryKind(const char *json, size_t len);
BOOL         Core_JsonSetMember(const char *json, size_t len, const char *key, const char *raw, char *out, size_t cap, size_t *outLen);
BOOL         Core_JsonQuote(const WCHAR *text, char *out, size_t cap);
BOOL         Core_ProjectDirName(const WCHAR *cwd, WCHAR *out, size_t cch);
BOOL         Core_IsUuid(const WCHAR *id);
BOOL         Core_ResumeLink(const WCHAR *sessionId, WCHAR *out, size_t cch);
void         Core_ScratchName(const SYSTEMTIME *day, DWORD random, WCHAR *out, size_t cch);
BOOL         Core_ScratchDirFor(const WCHAR *dataDir, const WCHAR *entriesDir, WCHAR *out, size_t cch);
size_t       Core_ReplaceChunk(const char *in, size_t len, const CoreSwap *swaps, int count, BOOL last, char *out, size_t *used);
BOOL         Core_PendingFormat(const PendingEdit *edit, WCHAR *out, size_t cch);
BOOL         Core_PendingParse(const WCHAR *line, PendingEdit *edit);
BOOL         Core_PendingReplaces(const PendingEdit *queued, const PendingEdit *added);
int          Core_ScrollStep(int pending, int elapsedMs);
ULONGLONG    Core_HashBytes(ULONGLONG hash, const void *data, size_t size);
ULONGLONG    Core_HashText(ULONGLONG hash, const WCHAR *text);
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
void      Util_SetStateDir(const WCHAR *dir);          /* tests: a private state folder */
BOOL      Util_FileExists(const WCHAR *path);
BOOL      Util_DirExists(const WCHAR *path);
typedef enum PathState { PATH_MISSING, PATH_PRESENT, PATH_UNREACHABLE } PathState;
PathState Util_QueryPath(const WCHAR *path, DWORD *attributes);
BOOL      Util_ExistingDir(const WCHAR *path, WCHAR *out, size_t cch);
BOOL      Util_EnsureDir(const WCHAR *path);
BOOL      Util_RegGetString(HKEY root, const WCHAR *key, const WCHAR *value, WCHAR *out, size_t cch);
BOOL      Util_RegSetString(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *data);
BOOL      Util_RegSetStringIfDifferent(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *data, BOOL *changed);
BOOL      Util_RegGetDword(HKEY root, const WCHAR *key, const WCHAR *value, DWORD *out);
BOOL      Util_RegSetDword(HKEY root, const WCHAR *key, const WCHAR *value, DWORD data);
BOOL      Util_RegKeyExists(HKEY root, const WCHAR *key);
BOOL      Util_RegValueExists(HKEY root, const WCHAR *key, const WCHAR *value);
LSTATUS   Util_RegDeleteValue(HKEY root, const WCHAR *key, const WCHAR *value);
LSTATUS   Util_RegDeleteTree(HKEY root, const WCHAR *key);
ULONGLONG Util_LocalNowTicks(void);
void      Util_Log(const WCHAR *fmt, ...);
BOOL      Util_OpenUrl(const WCHAR *url);
BOOL      Util_Spawn(const WCHAR *exe, const WCHAR *args, DWORD *pid);
BOOL      Util_IsDirectoryLink(const WCHAR *path);     /* a junction or directory symlink */
BOOL      Util_FitsRecycleBin(const WCHAR *path);
RemoveResult Util_Recycle(HWND owner, const WCHAR *const *paths, int count);
char     *Util_ReadFile(const WCHAR *path, DWORD maxBytes, BOOL tail, DWORD *len);
BOOL      Util_ExtendedPath(const WCHAR *path, WCHAR *out, size_t cch);   /* the \\?\ form, for paths past MAX_PATH */
HANDLE    Util_FindFiles(const WCHAR *dir, const WCHAR *pattern, WIN32_FIND_DATAW *found, BOOL foldersOnly);

/* -------------------------------------------------------------- claude.c */

BOOL    Claude_FindPackage(ClaudePackage *pkg);
HRESULT Claude_Launch(const ClaudePackage *pkg, const Profile *profile, const WCHAR *url,
                      DWORD *pid, BOOL *withIdentity);
void    Claude_UpdateRunning(ProfileList *list);
void    Claude_RefreshRunning(Profile *profile);
BOOL    Claude_IsRunning(const Profile *profile);
BOOL    Claude_LastSignInStart(const ClaudePackage *pkg, const Profile *profile, ULONGLONG *ticks);
BOOL    Claude_ClosedForUpdate(const ClaudePackage *pkg, const Profile *profile);
int     Claude_TopmostProfile(const ProfileList *list);

/* ------------------------------------------------------------ profiles.c */

void         Profiles_Load(ProfileList *list, const ClaudePackage *pkg);
BOOL         Profiles_ResolveStorage(Profile *p, const WCHAR *localAppData, const WCHAR *family);
int          Profiles_Find(const ProfileList *list, const WCHAR *folder);
int          Profiles_DefaultIndex(const ProfileList *list);
BOOL         Profiles_Create(const WCHAR *name, int color, WCHAR *folder, size_t folderCch, WCHAR *error, size_t errorCch);
BOOL         Profiles_Update(const WCHAR *folder, const WCHAR *label, int color);
BOOL         Profiles_SetDefault(const WCHAR *folder);
void         Profiles_CopySettings(const Profile *from, const Profile *to);
RemoveResult Profiles_Delete(HWND owner, const Profile *profile);
RemoveResult Profiles_RecycleData(HWND owner, const Profile *profile);
BOOL         Profiles_IsLinked(const Profile *profile);
BOOL         Profiles_LinkTarget(const Profile *profile, WCHAR *out, size_t cch);

/* --------------------------------------------------------------- icons.c */

extern const WCHAR *const g_ColorNames[PALETTE_SIZE];
BOOL  Icons_Ensure(const ClaudePackage *pkg, const Profile *profile, WCHAR *out, size_t cch);
HICON Icons_Create(const ClaudePackage *pkg, const Profile *profile, int size);
WCHAR Icons_ProfileInitial(const WCHAR *name);
HICON Icons_CreateBadge(int color, int size);
HICON Icons_CreateTray(const ClaudePackage *pkg, const Profile *profile, int size, BOOL darkTaskbar);
BOOL  Icons_IsStale(const ClaudePackage *pkg, const Profile *profile);
void  Icons_DeleteStale(const Profile *profile, const WCHAR *keep);

/* ----------------------------------------------------------- shortcuts.c */

#define MAX_LINK_COPIES    50   /* "Claude (<name>) (n).lnk": n up to this */
#define AUMID_CCH          64   /* a profile's or the manager's AppUserModelID */
#define APP_MANAGER_AUMID  APP_AUMID_PREFIX L"Manager"   /* the manager's windows and Start menu shortcut */

BOOL    Shortcut_Read(const WCHAR *lnk, LinkInfo *info);
BOOL    Shortcut_HasAppId(const WCHAR *lnk, const WCHAR *aumid);
HRESULT Shortcut_CreateForProfile(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk);
BOOL    Shortcut_IsAtStartup(const Profile *profile);
BOOL    Shortcut_StartMenuDir(WCHAR *out, size_t cch);
BOOL    Shortcut_IsInStartMenu(const Profile *profile);
HRESULT Shortcut_AddToStartMenu(const ClaudePackage *pkg, const Profile *profile);
HRESULT Shortcut_RemoveFromStartMenu(const Profile *profile);
HRESULT Shortcut_SetStartup(const ClaudePackage *pkg, const Profile *profile, BOOL on);
HRESULT Shortcut_WriteForProfile(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk);
BOOL    Shortcut_DesktopPathFor(const Profile *profile, WCHAR *out, size_t cch);
BOOL    Shortcut_IsOnDesktop(const Profile *profile);
void    Shortcut_RemoveOurs(const Profile *profile);
HRESULT Shortcut_Update(const WCHAR *lnk, const Profile *after, const WCHAR *icon);
BOOL    Shortcut_RenamedPath(const WCHAR *path, const Profile *before, const Profile *after, WCHAR *out, size_t cch);
BOOL    Shortcut_Refresh(const Profile *before, const Profile *after, const WCHAR *icon);
void    Shortcut_RefreshProfiles(const ProfileList *list, const WCHAR *const *icons);
DWORD   Shortcut_ListOurs(const WCHAR *folder, WCHAR (*paths)[MAX_PATH], DWORD capacity);
HRESULT Shortcut_CreateManagerLink(const WCHAR *exe);
HRESULT Shortcut_RestoreManagerLink(const WCHAR *exe);   /* S_FALSE: it is as it should be */
void    Shortcut_RemoveManagerLink(void);

/* -------------------------------------------------------------- update.c */

typedef enum UpdateResult {
    UPDATE_READY,            /* downloaded and verified, held unchanged: Update_Run starts it */
    UPDATE_STARTED,          /* it runs, and installs itself over this copy */
    UPDATE_NOT_DOWNLOADED,
    UPDATE_NOT_SIGNED,       /* not signed by APP_SIGNER: deleted */
    UPDATE_NOT_VERIFIED,     /* Windows could not check the signature (the error is its answer): deleted */
    UPDATE_WRONG_VERSION,    /* signed, but not the release's version, newer than this one: deleted */
    UPDATE_NOT_STARTED       /* verified, but Windows did not start it (the error says why): deleted */
} UpdateResult;

void         Update_Check(HWND notify, UINT message);
BOOL         Update_Available(WCHAR *version, size_t cch);
void         Update_Download(HWND notify, UINT message);   /* message: wParam an UpdateResult, lParam an error */
UpdateResult Update_Run(DWORD *error);
void         Update_RemoveDownload(void);
BOOL         Update_DownloadPath(WCHAR *out, size_t cch);

/* --------------------------------------------------------- taskbar-pin.c */

BOOL    TaskbarPin_Dir(WCHAR *out, size_t cch);
BOOL    TaskbarPin_IsPinned(const Profile *profile);
BOOL    TaskbarPin_HasAppId(const WCHAR *aumid);
BOOL    TaskbarPin_UsesIcon(const WCHAR *icon);
BOOL    TaskbarPin_ValidateList(const BYTE *list, size_t len);
HRESULT TaskbarPin_ReadList(HKEY key, BYTE **list, DWORD *len);
typedef struct TaskbarPin_ValueIO {   /* the pin list's values: the user's, or a test's */
    void *context;
    LSTATUS (*read)(void *context, const WCHAR *name, DWORD *type, BYTE *data, DWORD *bytes);
    LSTATUS (*write)(void *context, const WCHAR *name, DWORD type, const BYTE *data, DWORD bytes);
    LSTATUS (*erase)(void *context, const WCHAR *name);
} TaskbarPin_ValueIO;
typedef struct TaskbarPin_CommitResult {
    BOOL listCommitted;
    BOOL uncertain; /* a new shortcut must be kept while the list and its records are uncertain */
    LSTATUS rollbackError;
} TaskbarPin_CommitResult;
typedef struct TaskbarPin_Value {
    BOOL  exists;
    DWORD type, bytes;
    BYTE *data;
} TaskbarPin_Value;
#define TASKBAR_PIN_NONE ((size_t)-1)
typedef struct TaskbarPin_Merge {
    size_t rank;        /* the entry's index in the new list */
    BOOL   replacedPin; /* it took the place of an active entry */
} TaskbarPin_Merge;
#define TASKBAR_PIN_MAX_OBSOLETE 16   /* shortcuts of dropped entries deleted at once; more only linger */
typedef struct TaskbarPin_Addition {
    const BYTE  *entry;
    DWORD        entrySize;
    const WCHAR *shortcut;      /* the .lnk the entry pins */
    const WCHAR *aumid;
    const WCHAR *pinsDir;       /* the only folder a dropped entry's shortcut is deleted from */
    BOOL         replacedPin;   /* out: the entry took the place of an active one */
    WCHAR        obsoleteShortcuts[TASKBAR_PIN_MAX_OBSOLETE][MAX_PATH];   /* out: the shortcuts to delete */
    DWORD        obsoleteShortcutCount;
} TaskbarPin_Addition;
BOOL    TaskbarPin_RegistryValues(HKEY key, TaskbarPin_ValueIO *io);
HRESULT TaskbarPin_CommitPrepared(const TaskbarPin_ValueIO *io, const TaskbarPin_Value *base,
                                  const BYTE *list, size_t listLen,
                                  const BYTE *records, size_t recordsLen, BOOL writeRecords, TaskbarPin_CommitResult *result);
HRESULT TaskbarPin_AddToList(const TaskbarPin_ValueIO *io, TaskbarPin_Addition *addition, TaskbarPin_CommitResult *result);
HRESULT TaskbarPin_RepairList(const TaskbarPin_ValueIO *io);
HRESULT TaskbarPin_RemoveFromList(const TaskbarPin_ValueIO *io, WCHAR (*shortcuts)[MAX_PATH], DWORD shortcutCount);
BOOL    TaskbarPin_ListHasAppId(const BYTE *list, size_t len, const WCHAR *appId, BOOL live);
BOOL    TaskbarPin_ListHasProfile(const BYTE *list, size_t len, const Profile *profile);
HRESULT TaskbarPin_MergeEntry(const BYTE *list, size_t len, const BYTE *entry, size_t entrySize,
                              const WCHAR *lnk, const WCHAR *aumid, BOOL *dropped,
                              BYTE *out, size_t cap, size_t *outLen, TaskbarPin_Merge *merge);
HRESULT TaskbarPin_BuildRecord(const BYTE *entry, size_t size, BYTE **data, size_t *len);
HRESULT TaskbarPin_PrepareRecords(const BYTE *oldList, size_t oldLen, const BYTE *newList, size_t newLen,
                                  const BOOL *dropped, size_t fresh, BOOL edited,
                                  const BYTE *records, size_t recordsLen, BYTE **out, size_t *outLen);
HRESULT TaskbarPin_UpdateLeftover(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk);
HRESULT TaskbarPin_Pin(const ClaudePackage *pkg, const Profile *profile);
void    TaskbarPin_Refresh(const Profile *before, const Profile *after, const WCHAR *icon);
void    TaskbarPin_RefreshProfiles(const ProfileList *list, const WCHAR *const *icons);
void    TaskbarPin_TellShortcutChanged(const WCHAR *lnk);
void    TaskbarPin_RepairOurs(void);
void    TaskbarPin_RemoveOurs(void);
size_t  TaskbarPin_InjectAppId(const BYTE *item, size_t cb, const WCHAR *appId, BYTE *out, size_t cap);
size_t  TaskbarPin_RepairItem(const BYTE *item, size_t cb, BYTE *out, size_t cap);

/* ------------------------------------------------------------- handler.c */

typedef enum UserChoiceState { USERCHOICE_NONE, USERCHOICE_OURS, USERCHOICE_OTHER } UserChoiceState;

BOOL            Handler_Register(const WCHAR *exe);
UserChoiceState Handler_UserChoice(void);
BOOL            Handler_AskUser(void);
void            Handler_Unregister(void);

/* -------------------------------------------------------------- taskbar.c */

void Taskbar_Watch(const Profile *profile);
void Taskbar_Refresh(const Profile *profile, BOOL linksChanged);
BOOL Taskbar_StopWatchers(BOOL giveBack);
BOOL Taskbar_IsWatched(const Profile *profile);
int  Taskbar_WatchRun(const WCHAR *folder, DWORD pid, void (*claudeClosed)(const WCHAR *folder));

/* --------------------------------------------------------- sessionstore.c */

#define SESSION_ENTRY_MAX_BYTES (4u * 1024u * 1024u)   /* larger, a file is not one of Claude's small JSON files */

typedef struct SessionEntry {          /* one profile's entry for a session */
    WCHAR     localId[SESSION_ID_CCH]; /* the entry's own id, its session's key until the first message */
    WCHAR     file[LONG_PATH_CCH];     /* its local_*.json */
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
    int       duplicate;               /* another entry of the session in the same profile, -1 when none */
    int       firstOtherTranscript, otherTranscriptCount;   /* the session's other transcripts, in SessionSet.otherTranscriptIds */
} SessionEntry;

typedef struct SessionRow {            /* one transcript */
    WCHAR     key[SESSION_ID_CCH];     /* its id, or the entry's own id before its first message */
    WCHAR     cwd[MAX_PATH];
    WCHAR     transcriptPath[LONG_PATH_CCH];
    int       group;
    BOOL      transcript;              /* on disk */
    ULONGLONG transcriptBytes;         /* with its other transcripts that no other session goes on with */
    ULONGLONG lastActivity;
    DWORD     live;                    /* the profiles running it now, one bit each (MAX_PROFILES <= 32) */
    int       entry[MAX_PROFILES];     /* into SessionSet.entries, the one Claude goes on with; -1: that profile does not list it */
} SessionRow;

typedef struct SessionGroup {          /* a project folder, or one profile's "no folder" sessions */
    WCHAR     name[MAX_PATH];          /* the folder's own name; empty for "no folder" and an unknown folder */
    WCHAR     path[MAX_PATH];          /* empty for "no folder" and an unknown folder */
    int       scratchOf;               /* that profile for "no folder", else -1 */
    ULONGLONG lastActivity;
} SessionGroup;

typedef struct SessionSource {         /* what reading one profile's entries found */
    BOOL      signedIn;                /* its config.json names an account */
    int       unreadable;              /* entries that are not a session entry */
    int       elsewhere;               /* SSH, WSL or cloud sessions: their conversation is not on this PC */
    int       pending;                 /* waiting changes its entries can take once it is closed */
    WCHAR     entriesDir[MAX_PATH];    /* claude-code-sessions\<account>\<organization>; empty when it has none */
    WCHAR     scratchDir[MAX_PATH];    /* logical scratch-workspaces\<account>\<organization>: its "no folder" */
} SessionSource;

typedef struct SessionRowIndex SessionRowIndex;

typedef struct SessionSet {
    ProfileList      profiles;
    SessionSource    source[MAX_PROFILES];
    BOOL             noTranscripts;    /* no Claude Code transcripts folder */
    SessionEntry    *entries;
    SessionRow      *rows;             /* by group, then last activity */
    SessionGroup    *groups;
    SessionRowIndex *index;            /* rows by key (SessionStore_FindRow) */
    WCHAR          (*otherTranscriptIds)[SESSION_ID_CCH];
    int              entryCount, entryCap, rowCount, rowCap, groupCount, groupCap, otherTranscriptCount, otherTranscriptCapacity;
} SessionSet;

#define SESSION_PENDING_MAX 256

BOOL         SessionStore_LoadProfiles(SessionSet *set, const ProfileList *profiles);
BOOL         SessionStore_LoadProfilesCancel(SessionSet *set, const ProfileList *profiles, HANDLE cancel);
BOOL         SessionStore_LoadEntries(SessionSet *set, const Profile *profile);   /* FALSE: not every entry could be read */
void         SessionStore_Free(SessionSet *set);
int          SessionStore_FindRow(const SessionSet *set, const WCHAR *key);
int          SessionStore_FindEntry(const SessionSet *set, int profile, const WCHAR *key, int *row);
const WCHAR **SessionStore_TranscriptIds(const SessionSet *set, int row, int *count);   /* HeapFree the array */
BOOL         SessionStore_ClaimedByOther(const SessionSet *set, int row, int profile, const WCHAR *id);
const WCHAR *SessionStore_OwnId(const SessionEntry *entry);                                 /* without local_ */
DWORD        SessionStore_RunningNow(const ProfileList *profiles, const WCHAR *sessionId, BOOL *outside);
BOOL         SessionStore_ClaudeCodePath(const WCHAR *sub, WCHAR *out, size_t cch);
BOOL         SessionStore_ProjectsDir(WCHAR *out, size_t cch);
BOOL         SessionStore_SessionsDir(const Profile *p, WCHAR *out, size_t cch);
BOOL         SessionStore_WorkingDir(const SessionSet *set, const WCHAR *cwd, WCHAR *out, size_t cch);
BOOL         SessionStore_WatchDir(const Profile *p, WCHAR *out, size_t cch);
BOOL         SessionStore_PendingPath(const Profile *p, WCHAR *out, size_t cch);
int          SessionStore_LoadPending(const Profile *p, PendingEdit *edits, int capacity);   /* -1: unreadable */
const WCHAR *SessionStore_RowTitle(const SessionSet *set, const SessionRow *row);           /* "": untitled */
void         SessionStore_GroupName(const SessionSet *set, int group, WCHAR *out, size_t cch);

/* ---------------------------------------------------------- sessionedit.c */

typedef enum CopyResult { COPY_MADE, COPY_CANCELLED, COPY_FAILED } CopyResult;

/* `owner`: the window Windows' questions about the Recycle Bin belong to; NULL away from the manager. */
HRESULT      SessionEdit_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *sessionId);
BOOL         SessionEdit_Change(HWND owner, const Profile *p, const SessionEntry *entry, const PendingEdit *edit, BOOL *waiting,
                                WCHAR *error, size_t errorCch);
BOOL         SessionEdit_Cancel(const Profile *p, const PendingEdit *edits, int count);
void         SessionEdit_Forget(const Profile *p, const WCHAR *const *keys, int count);
int          SessionEdit_ApplyPending(HWND owner, const Profile *p);
int          SessionEdit_ApplyPendingAfterRun(const Profile *p);   /* its Claude has just closed */
void         SessionEdit_ApplyPendingFor(const WCHAR *folder);
CopyResult   SessionEdit_CopyConversation(HWND owner, const SessionSet *set, int row, int target, WCHAR *newId, size_t idCch,
                                          WCHAR *error, size_t errorCch);
BOOL         SessionEdit_RemoveCopy(const SessionSet *set, int row, int target, const WCHAR *copyId, WCHAR *left, size_t leftCch);
BOOL         SessionEdit_CanDelete(const SessionSet *set, int row, WCHAR *error, size_t errorCch);
BOOL         SessionEdit_RemovesWorkingFolder(const SessionSet *set, int row, WCHAR *physical, size_t cch);
RemoveResult SessionEdit_DeleteEverywhere(HWND owner, const SessionSet *set, int row, WCHAR *error, size_t errorCch);
BOOL         SessionEdit_ListFiles(const SessionSet *set, int row, WCHAR (**paths)[LONG_PATH_CCH], int *count,
                                   WCHAR *error, size_t errorCch);

/* ------------------------------------------------------------- sessions.c */

#define WM_APP_SESSIONS (WM_APP + 12)   /* to the manager: session entries or transcripts changed on disk */
#define WM_APP_SESSIONS_READY (WM_APP + 13) /* background session snapshot ready for the window */

void         SessionsView_Init(HWND dlg);
void         SessionsView_Warm(const ProfileList *profiles);
void         SessionsView_SetProfiles(const ProfileList *profiles);
void         SessionsView_Ready(BOOL allowChanges);
void         SessionsView_Enter(const ClaudePackage *pkg, const WCHAR *folder);   /* pkg: the manager's, kept current, read at each use */
void         SessionsView_Leave(void);
BOOL         SessionsView_Shown(void);
const WCHAR *SessionsView_Profile(void);
void         SessionsView_Reload(void);
BOOL         SessionsView_Command(WPARAM wp);
BOOL         SessionsView_ClearSearch(void);
BOOL         SessionsView_Notify(const NMHDR *header, LRESULT *result);
BOOL         SessionsView_ContextMenu(HWND from, LPARAM pos);
BOOL         SessionsView_DrawItem(const DRAWITEMSTRUCT *item);
void         SessionsView_Relayout(void);
void         SessionsView_Resize(void);
void         SessionsView_Destroy(void);

/* ----------------------------------------------------------------- tray.c */

/* Where Windows keeps the light or dark choice of apps and of the taskbar. */
#define REG_PERSONALIZE L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"

BOOL Tray_ClaudeImagePath(const ClaudePackage *pkg, BOOL lightApps, WCHAR *out, size_t cch);
BOOL Tray_IsHost(HWND window);
void Tray_Apply(const ClaudePackage *pkg, const Profile *profile);
void Tray_GiveBack(const ClaudePackage *pkg, DWORD pid);

/* -------------------------------------------------------------- install.c */

BOOL Install_IsInstalledCopy(void);
BOOL Install_IsRegistered(void);
BOOL Install_Run(BOOL openManager);
void Install_Repair(void);
void Install_ApplyLanguage(const ClaudePackage *pkg, const ProfileList *list);
BOOL Install_Uninstall(HWND owner, const ProfileList *list, const BOOL *removeData);
void Install_FinishUninstall(void);

/* --------------------------------------------------------------- theme.c */
/* The look of every window, in one place: Theme_Apply on a dialog themes its
 * controls (buttons, check boxes, lists and their headers, edits, combo boxes,
 * frames, focus rectangles, smooth wheel scrolling); what a window draws
 * itself takes its colors, fonts and rows from here. Also: fitting a dialog
 * to its captions in every language (Theme_FitDialog), the manager window's
 * layout (Theme_MainMinimum, Theme_LayoutMain), every dialog's shell
 * (Ui_Dialog) and the themed message box (Ui_Ask, Ui_Message). */

typedef enum ThemeColor {
    THEME_TEXT,
    THEME_MUTED,        /* secondary text */
    THEME_FACE,         /* window background */
    THEME_FIELD,        /* lists, trees, edits */
    THEME_MAIN_BLUE,    /* a selected row, selected text, a button's frame */
    THEME_BRIGHT_BLUE,  /* a selected row's frame; a button under the mouse or pressed */
    THEME_PALE_BLUE,    /* a row or a button under the mouse */
    THEME_SEPARATOR,    /* a line between two parts of a window */
    THEME_COLORS
} ThemeColor;

#define THEME_ROW_SELECTED 0x1
#define THEME_ROW_HOT      0x2

/* Geometry the theme draws and places with, which the tests check too. */
#define THEME_CORNER_RADIUS_DIPS      4       /* buttons, selected rows, views and edits */
#define THEME_VIEW_MAX_PX             30000   /* a control taller than this scrolls itself in its view */
#define THEME_MAIN_MARGIN_DIPS        15      /* around the manager window's content */
#define THEME_MAIN_GAP_DIPS           9       /* between its neighboring buttons */
#define THEME_MAIN_READING_WIDTH_DIPS 1200    /* a wider manager window centers its content */
#define THEME_PROFILE_COLUMN_DIPS     180     /* the profile list's first column */

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
COLORREF Theme_Color(ThemeColor color);
HBRUSH   Theme_Brush(ThemeColor color);
COLORREF Theme_DrawRow(HWND owner, HDC dc, const RECT *rc, UINT state, COLORREF around);   /* returns the text color */
COLORREF Theme_RowMuted(UINT state);
void     Theme_DrawTreeGlyph(HWND tree, HDC dc, const RECT *cell, UINT state, BOOL open);   /* a folder's arrow; state: THEME_ROW_* */
void     Theme_DrawFocusCue(HWND owner, HDC dc, const RECT *row);   /* the keyboard's row, after keyboard use only */
void     Theme_DrawButton(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state, UINT format);
void     Theme_DrawDropDown(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state);   /* THEME_BUTTON_* */
int      Theme_DropDownWidth(HWND owner, HFONT font, const WCHAR *text);   /* fits `text` whole, in the current language */
void     Theme_DropDownLabel(HWND owner, const RECT *box, RECT *label);   /* where Theme_DrawDropDown draws the label of `box` */
BOOL     Theme_TableCellText(HWND list, int row, int column, RECT *text);   /* where a table cell's text is drawn; a wider text shows a tip */
UINT     Theme_TrackDropDown(HWND owner, HMENU menu, const RECT *screenBox);
void     Theme_SetStrong(HWND control);   /* its text semibold, at the dialog's scale */
BOOL     Theme_CheckBoxSize(HWND control, SIZE *size);
void     Theme_CreateFonts(HWND dialog, ThemeFonts *fonts);
void     Theme_FreeFonts(ThemeFonts *fonts);
HDC      Theme_BufferBegin(ThemeBuffer *buffer, HDC target, const RECT *rc);
void     Theme_BufferEnd(ThemeBuffer *buffer);
void     Theme_SetScrollRow(HWND control, int rowPx);   /* how far a wheel line scrolls, 0: one row of the control */
int      Theme_ScrollTarget(HWND window, WPARAM request, int linePx);   /* the position a WM_VSCROLL asks of a window scrolled by the pixel */
HWND     Theme_SmoothView(HWND control);   /* a list or tree scrolled by the pixel; returns the view, which has its place */
void     Theme_Follow(HWND dialog, UINT msg, WPARAM wp, LPARAM lp);
void     Theme_Apply(HWND dialog);   /* what it keeps on the dialog goes with the dialog */
void     Theme_RememberLayout(HWND dialog);
void     Theme_FitDialog(HWND dialog);
BOOL     Theme_MainMinimum(HWND dialog, SIZE *client);
void     Theme_LayoutMain(HWND dialog);
void     Theme_ProfileColumnWidths(HWND list, int *profile, int *role, int *dataMinimum);   /* dataMinimum may be NULL */
void     Theme_FitLastColumn(HWND list, int minimum);   /* the width the other columns leave, at least `minimum`, unless the user sized it or drags it */
/* The catalog keys of what the manager window shows, the ones its layout is
 * measured with: a button's caption in `state` (0: the resource's, 1: its
 * other one), a profile column's title (NULL past the last), a row's role. */
const WCHAR *Theme_MainCaption(int id, int state);
const WCHAR *Theme_ProfileColumnTitle(int column);
const WCHAR *Theme_ProfileRole(BOOL stock, BOOL isDefault);
/* The footer link's text in each state, the note under Set as default
 * (its %s: the profile the regular Claude icon opens) and the sessions
 * details' captions, also measured with the window: catalog keys. */
typedef enum MainFooter { MAIN_FOOTER_CREDITS, MAIN_FOOTER_AVAILABLE, MAIN_FOOTER_DOWNLOADING, MAIN_FOOTER_INSTALLING, MAIN_FOOTERS } MainFooter;
typedef enum SessionsCaption { SESSIONS_ACTIONS, SESSIONS_DELETE_EVERYWHERE, SESSIONS_CAPTIONS } SessionsCaption;
const WCHAR *Theme_MainFooter(MainFooter state);   /* credits: this version, the author's page; available: this version, the new one; downloading: the new one */
const WCHAR *Theme_MainNote(void);
const WCHAR *Theme_SessionsCaption(SessionsCaption caption);
BOOL     Theme_ColumnResizeIsManual(HWND list, int column);   /* the user sized it (a header divider dragged or double-clicked) */
void     Theme_SetColumnWidth(HWND list, int column, int width);
void     Theme_LayoutSidebarNote(HWND note, const WCHAR *format, const WCHAR *name);
INT_PTR  Theme_CtlColor(UINT msg, WPARAM wp, LPARAM lp, int mutedId);
BOOL     Ui_Ask(HWND owner, LPCWSTR icon, const WCHAR *text, const WCHAR *ok, const WCHAR *cancel, BOOL defaultCancel);
int      Ui_Message(HWND owner, UINT flags, const WCHAR *format, ...);   /* MessageBox look-alike, `format` translated */
INT_PTR  Ui_Dialog(HWND owner, int id, DLGPROC proc, LPARAM param);   /* every dialog opens here */

/* ------------------------------------------------ router.c / main.c / gui.c */

int     Router_Run(const WCHAR *rawUrl);
int     Launcher_Run(const WCHAR *folder);
HRESULT Launcher_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *url, DWORD *pid, BOOL *identity);
typedef enum GuiStart { GUI_MANAGER, GUI_SET_UP_LINKS, GUI_UNINSTALL } GuiStart;
int     Gui_Run(GuiStart start);
BOOL    Gui_MainWindowGeometry(HWND dialog, UINT message, WPARAM wp, LPARAM lp);
void    Gui_LayoutProfileColumns(HWND list);
void    Gui_ShowSessions(HWND dialog, const ClaudePackage *package, BOOL showSessions, const WCHAR *folder);

#endif
