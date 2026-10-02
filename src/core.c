/*
 * Pure helpers: no registry, no files, no UI, each covered by tests/test_core.c.
 */
#include "app.h"
#include <math.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>

/* ---------------------------------------------------------------- strings */

/* A space of any kind: the ASCII ones and every one Windows classes as a
 * space (no-break, ideographic, U+2000-U+200A, U+202F, U+205F, U+1680,
 * U+0085...). */
static BOOL IsSpace(WCHAR c)
{
    WORD type = 0;
    if (c < 0x80) return c == L' ' || (c >= L'\t' && c <= L'\r');
    return GetStringTypeW(CT_CTYPE1, &c, 1, &type) && (type & C1_SPACE);
}

/* A C0 or C1 control character, DEL included. */
static BOOL IsControl(WCHAR c)
{
    return c < 0x20 || (c >= 0x7F && c <= 0x9F);
}

static size_t TrimmedSpan(const WCHAR *raw, const WCHAR **start)
{
    size_t n;
    while (*raw && IsSpace(*raw)) raw++;
    n = wcslen(raw);
    while (n > 0 && IsSpace(raw[n - 1])) n--;
    *start = raw;
    return n;
}

static BOOL EqualsI(const WCHAR *a, int aLength, const WCHAR *b, int bLength)
{
    return CompareStringOrdinal(a, aLength, b, bLength, TRUE) == CSTR_EQUAL;
}

BOOL Core_EqualsI(const WCHAR *a, const WCHAR *b)
{
    return a && b && EqualsI(a, -1, b, -1);
}

BOOL Core_EndsWithI(const WCHAR *s, const WCHAR *suffix)
{
    size_t length, suffixLength;
    if (!s || !suffix) return FALSE;
    length = wcslen(s);
    suffixLength = wcslen(suffix);
    return suffixLength <= length && EqualsI(s + length - suffixLength, (int)suffixLength, suffix, (int)suffixLength);
}

static const WCHAR *FindI(const WCHAR *s, const WCHAR *needle)
{
    size_t length = wcslen(s), needleLength = wcslen(needle), i;
    if (needleLength == 0 || needleLength > length) return NULL;
    for (i = 0; i + needleLength <= length; i++)
        if (EqualsI(s + i, (int)needleLength, needle, (int)needleLength)) return s + i;
    return NULL;
}

BOOL Core_ContainsI(const WCHAR *s, const WCHAR *needle)
{
    return s && needle && FindI(s, needle) != NULL;
}

/* The length of `path` without its trailing separators; a drive root
 * ("C:\\") keeps its own. */
size_t Core_TrimmedPathLength(const WCHAR *path)
{
    size_t n = wcslen(path);
    while (n > 3 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) n--;
    return n;
}

/* Like Core_PathEquals, but ordered (an empty path first). */
int Core_PathCompare(const WCHAR *a, const WCHAR *b)
{
    if (!a[0] || !b[0]) return a[0] ? 1 : b[0] ? -1 : 0;
    return CompareStringOrdinal(a, (int)Core_TrimmedPathLength(a), b, (int)Core_TrimmedPathLength(b), TRUE) - CSTR_EQUAL;
}

BOOL Core_PathEquals(const WCHAR *a, const WCHAR *b)
{
    return a && b && *a && *b && Core_PathCompare(a, b) == 0;
}

BOOL Core_IsOurExe(const WCHAR *path)
{
    const WCHAR *name;
    if (!path || !*path) return FALSE;
    name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return EqualsI(name, -1, APP_EXE, -1);
}

#define HASHED_TEXT_CCH 512

/* FNV-1a of the text in upper case; a text of HASHED_TEXT_CCH characters or
 * more hashes as an empty one (no name hashed is that long). AppUserModelIDs,
 * icon files and watcher mutexes are named after it: it must never change. */
DWORD Core_HashIgnoringCase(const WCHAR *text)
{
    WCHAR upper[HASHED_TEXT_CCH];
    DWORD hash = 2166136261u;
    int length, i;
    length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text, -1, upper, ARRAYSIZE(upper), NULL, NULL, 0);
    for (i = 0; i < length - 1; i++) {
        hash ^= (DWORD)upper[i];
        hash *= 16777619u;
    }
    return hash;
}

void Core_ProfileAumid(const WCHAR *folder, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, APP_AUMID_PREFIX L"Profile.%08lX", (unsigned long)Core_HashIgnoringCase(folder));
}

ULONGLONG Core_SystemTimeTicks(const SYSTEMTIME *st)
{
    FILETIME fileTime;
    ULARGE_INTEGER ticks;
    if (!SystemTimeToFileTime(st, &fileTime)) return 0;
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    return ticks.QuadPart;
}

/* ----------------------------------------------------------- profile names */

static BOOL IsNameChar(WCHAR c)
{
    WORD type = 0;
    if (c == L' ' || c == L'-' || c == L'_' || c == L'.') return TRUE;
    if (IsControl(c) || (c >= 0xD800 && c <= 0xDFFF)) return FALSE;
    if (!GetStringTypeW(CT_CTYPE1, &c, 1, &type)) return FALSE;
    return (type & (C1_ALPHA | C1_DIGIT)) != 0;
}

/* Claude keeps its own siblings next to a profile folder ("<folder>-Data" in
 * %LOCALAPPDATA%, "Claude-3p" in %APPDATA%): a name ending in "-Data" or
 * "-3p" would share them with another profile. */
static BOOL IsReservedSuffix(const WCHAR *s, size_t n)
{
    static const WCHAR *const exact[] = { L"3p", L"Data" };
    static const WCHAR *const tails[] = { L"-3p", L"-Data" };
    size_t i, tailLength;
    for (i = 0; i < ARRAYSIZE(exact); i++)
        if (EqualsI(s, (int)n, exact[i], -1)) return TRUE;
    for (i = 0; i < ARRAYSIZE(tails); i++) {
        tailLength = wcslen(tails[i]);
        if (n >= tailLength && EqualsI(s + n - tailLength, (int)tailLength, tails[i], (int)tailLength)) return TRUE;
    }
    return FALSE;
}

C_ASSERT(MAX_NAME == 32 && MAX_LABEL == 48);   /* the limits the messages below give */

BOOL Core_ValidateNewName(const WCHAR *raw, WCHAR *name, size_t nameCch,
                          WCHAR *folder, size_t folderCch, const WCHAR **error)
{
    const WCHAR *start, *unused;
    size_t n, i;
    if (!error) error = &unused;
    *error = NULL;
    if (!raw) raw = L"";
    n = TrimmedSpan(raw, &start);
    if (n == 0) { *error = L"Enter a name."; return FALSE; }
    if (n > MAX_NAME) { *error = L"Use 32 characters or fewer."; return FALSE; }
    for (i = 0; i < n; i++) {
        if (!IsNameChar(start[i])) {
            *error = L"Use letters, digits, spaces, \x201C-\x201D, \x201C_\x201D or \x201C.\x201D.";
            return FALSE;
        }
    }
    if (start[0] == L'.' || start[n - 1] == L'.') {
        *error = L"The name cannot start or end with a dot.";
        return FALSE;
    }
    if (IsReservedSuffix(start, n)) { *error = L"Claude uses this name itself. Pick another one."; return FALSE; }
    if (FAILED(StringCchCopyNW(name, nameCch, start, n))) { *error = L"Name too long."; return FALSE; }
    if (FAILED(StringCchPrintfW(folder, folderCch, PROFILE_PREFIX L"%s", name))) { *error = L"Name too long."; return FALSE; }
    return TRUE;
}

BOOL Core_ValidateLabel(const WCHAR *raw, WCHAR *label, size_t cch, const WCHAR **error)
{
    const WCHAR *start, *unused;
    size_t n, i;
    if (!error) error = &unused;
    *error = NULL;
    if (!raw) raw = L"";
    n = TrimmedSpan(raw, &start);
    if (n == 0) { *error = L"Enter a name."; return FALSE; }
    if (n > MAX_LABEL) { *error = L"Use 48 characters or fewer."; return FALSE; }
    for (i = 0; i < n; i++) {
        if (IsControl(start[i])) { *error = L"The name contains an invalid character."; return FALSE; }
    }
    if (FAILED(StringCchCopyNW(label, cch, start, n))) { *error = L"Name too long."; return FALSE; }
    return TRUE;
}

BOOL Core_IsProfileFolder(const WCHAR *folder)
{
    size_t prefixLength = wcslen(PROFILE_PREFIX), n, i;
    if (!folder) return FALSE;
    n = wcslen(folder);
    if (n <= prefixLength || n >= FOLDER_CCH) return FALSE;
    if (!EqualsI(folder, (int)prefixLength, PROFILE_PREFIX, (int)prefixLength)) return FALSE;
    for (i = prefixLength; i < n; i++)
        if (IsControl(folder[i])) return FALSE;
    return !IsReservedSuffix(folder + prefixLength, n - prefixLength);
}

/* ------------------------------------------------------------------- links */

BOOL Core_SanitizeUrl(const WCHAR *in, WCHAR *out, size_t cch)
{
    static const WCHAR hex[] = L"0123456789ABCDEF";
    const WCHAR *start;
    size_t n, i, length = 0;
    if (!in || !out || cch == 0) return FALSE;
    out[0] = 0;
    n = TrimmedSpan(in, &start);
    if (n < 8 || !EqualsI(start, 7, L"claude:", 7)) return FALSE;
    for (i = 0; i < n; i++) {
        WCHAR c = start[i];
        /* The link travels as one quoted argument: a quote, a backslash (it
         * can escape the closing quote) or whitespace must not reach argv. */
        if (c < 0x20 || c == 0x7F || c == L'"' || c == L'\\' || c == L' ') {
            if (length + 3 >= cch) return FALSE;
            out[length++] = L'%';
            out[length++] = hex[(c >> 4) & 0xF];
            out[length++] = hex[c & 0xF];
        } else if (IsSpace(c)) {
            return FALSE;
        } else {
            if (length + 1 >= cch) return FALSE;
            out[length++] = c;
        }
    }
    out[length] = 0;
    return TRUE;
}

/* Looks at the link's path only: words in the query or fragment (a chat
 * prompt, say) never make a link a sign-in. */
BOOL Core_IsSignInUrl(const WCHAR *url)
{
    static const WCHAR *const markers[] = { L"login", L"auth", L"magic-link", L"sso", L"callback" };
    WCHAR path[URL_CCH];
    size_t i;
    if (!url) return FALSE;
    (void)StringCchCopyNW(path, ARRAYSIZE(path), url, wcscspn(url, L"?#"));   /* truncates if longer */
    for (i = 0; i < ARRAYSIZE(markers); i++)
        if (Core_ContainsI(path, markers[i])) return TRUE;
    return FALSE;
}

BOOL Core_BuildLaunchArgs(const WCHAR *dataDir, const WCHAR *url, WCHAR *out, size_t cch)
{
    if (!out || cch == 0) return FALSE;
    out[0] = 0;
    if (dataDir && *dataDir) {
        size_t n = Core_TrimmedPathLength(dataDir);
        if (wcschr(dataDir, L'"')) return FALSE;
        if (FAILED(StringCchPrintfW(out, cch, L"--user-data-dir=\"%.*s\"", (int)n, dataDir))) return FALSE;
    }
    if (url && *url) {
        /* Only a claude: link, never a switch, and nothing that can end the quotes. */
        if (!EqualsI(url, (int)min(wcslen(url), 7), L"claude:", 7) || wcspbrk(url, L"\"\\")) return FALSE;
        if (out[0] && FAILED(StringCchCatW(out, cch, L" "))) return FALSE;
        if (FAILED(StringCchCatW(out, cch, L"\""))) return FALSE;
        if (FAILED(StringCchCatW(out, cch, url))) return FALSE;
        if (FAILED(StringCchCatW(out, cch, L"\""))) return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------ log parsing */

static BOOL IsDigit(char c) { return c >= '0' && c <= '9'; }

static const char kStampShape[] = "dddd-dd-dd dd:dd:dd";
#define STAMP_LENGTH (sizeof kStampShape - 1)

/* The line starts with a "yyyy-mm-dd hh:mm:ss" stamp and goes on after it. */
static BOOL IsStampedLine(const char *line, const char *lineEnd)
{
    size_t i;
    if ((size_t)(lineEnd - line) <= STAMP_LENGTH) return FALSE;
    for (i = 0; i < STAMP_LENGTH; i++) {
        if (kStampShape[i] == 'd' ? !IsDigit(line[i]) : line[i] != kStampShape[i]) return FALSE;
    }
    return TRUE;
}

/* Walks a text line by line: FALSE after the last line. */
static BOOL NextLine(const char **cursor, const char *end, const char **line, const char **lineEnd)
{
    const char *newline;
    if (*cursor >= end) return FALSE;
    newline = (const char *)memchr(*cursor, '\n', (size_t)(end - *cursor));
    *line = *cursor;
    *lineEnd = newline ? newline : end;
    *cursor = newline ? newline + 1 : end;
    return TRUE;
}

static int ParseDigits(const char *p, int count)
{
    int value = 0, i;
    for (i = 0; i < count; i++) value = value * 10 + (p[i] - '0');
    return value;
}

static BOOL LineContains(const char *p, const char *end, const char *needle)
{
    size_t needleLength = strlen(needle);
    for (; (size_t)(end - p) >= needleLength; p++)
        if (memcmp(p, needle, needleLength) == 0) return TRUE;
    return FALSE;
}

/* FALSE for a date or time that does not exist (2026-09-31, 24:00:00). */
static BOOL StampToTime(const char *stamp, SYSTEMTIME *st)
{
    FILETIME unused;
    ZeroMemory(st, sizeof *st);
    st->wYear = (WORD)ParseDigits(stamp, 4);
    st->wMonth = (WORD)ParseDigits(stamp + 5, 2);
    st->wDay = (WORD)ParseDigits(stamp + 8, 2);
    st->wHour = (WORD)ParseDigits(stamp + 11, 2);
    st->wMinute = (WORD)ParseDigits(stamp + 14, 2);
    st->wSecond = (WORD)ParseDigits(stamp + 17, 2);
    return SystemTimeToFileTime(st, &unused);
}

/* Claude logs "<yyyy-mm-dd hh:mm:ss> [info] [Auth] Using system browser for:
 * /login/..." in the main.log of the window that opened the browser. */
BOOL Core_LatestSignInStart(const char *text, size_t len, SYSTEMTIME *latest)
{
    const char *cursor, *line, *lineEnd;
    char best[STAMP_LENGTH];
    SYSTEMTIME when;
    BOOL found = FALSE;
    if (!text || !latest) return FALSE;
    cursor = text;
    while (NextLine(&cursor, text + len, &line, &lineEnd)) {
        if (IsStampedLine(line, lineEnd) && StampToTime(line, &when) &&
            LineContains(line + STAMP_LENGTH, lineEnd, "[Auth] Using system browser for:") &&
            (!found || memcmp(line, best, STAMP_LENGTH) > 0)) {
            memcpy(best, line, STAMP_LENGTH);
            *latest = when;
            found = TRUE;
        }
    }
    return found;
}

/* The last line of a main.log saying why Claude quits. Going down for an
 * update, Claude's updater logs "beforeQuitForUpdate handler fired" and
 * Windows, closing every Claude to replace the package, "Windows session
 * ending (close-app) - quitting the app": that line ends the log of each
 * window. A quit from the window or the tray logs "Quitting app...",
 * "beforeQuit:" and "willQuit:" lines, a Windows shutdown "Windows session
 * ending (shutdown)". A crash logs none: the line found is then an older
 * run's, which the caller tells by its time. */
BOOL Core_LastQuit(const char *text, size_t len, SYSTEMTIME *when, BOOL *forUpdate)
{
    static const char *const update[] = { "beforeQuitForUpdate", "Windows session ending (close-app" };
    static const char *const other[] = { "Windows session ending (", "Quitting app", "beforeQuit:", "willQuit:" };
    const char *cursor, *line, *lineEnd, *last = NULL;
    BOOL lastUpdate = FALSE;
    size_t i;
    if (!text || !when || !forUpdate) return FALSE;
    cursor = text;
    while (NextLine(&cursor, text + len, &line, &lineEnd)) {
        BOOL isUpdate = FALSE, isOther = FALSE;
        if (!IsStampedLine(line, lineEnd)) continue;
        for (i = 0; i < ARRAYSIZE(update) && !isUpdate; i++) isUpdate = LineContains(line + STAMP_LENGTH, lineEnd, update[i]);
        for (i = 0; i < ARRAYSIZE(other) && !isUpdate && !isOther; i++)
            isOther = LineContains(line + STAMP_LENGTH, lineEnd, other[i]);
        if (isUpdate || isOther) {
            last = line;
            lastUpdate = isUpdate;
        }
    }
    if (!last || !StampToTime(last, when)) return FALSE;
    *forUpdate = lastUpdate;
    return TRUE;
}

/* --------------------------------------------------------------- routing */

int Core_SelectTargets(int count, const ULONGLONG *signInTicks, ULONGLONG nowTicks,
                       ULONGLONG signInMaxAgeTicks, BOOL signInUrl, int lastUsed,
                       int defaultIndex, int *targets, RouteReason *reason)
{
    /* A stamp up to a minute ahead still counts: the clock may have been set
     * back since Claude wrote it. */
    const ULONGLONG clockSkewTicks = 60 * TICKS_PER_SECOND;
    RouteReason chosen;
    int n = 0, i, best = -1;

    if (count <= 0) {
        chosen = ROUTE_NOTHING_RUNNING;
    } else if (count == 1) {
        targets[n++] = 0;
        chosen = ROUTE_ONLY_ONE;
    } else if (signInUrl) {
        for (i = 0; i < count; i++) {
            ULONGLONG signInTime = signInTicks ? signInTicks[i] : 0;
            if (!signInTime || signInTime > nowTicks + clockSkewTicks) continue;
            if (nowTicks > signInTime && nowTicks - signInTime > signInMaxAgeTicks) continue;
            if (best < 0 || signInTime > signInTicks[best]) best = i;
        }
        if (best >= 0) {
            targets[n++] = best;
            chosen = ROUTE_SIGNIN;
        } else {
            for (i = 0; i < count; i++) targets[n++] = i;
            chosen = ROUTE_BROADCAST;
        }
    } else if (lastUsed >= 0 && lastUsed < count) {
        targets[n++] = lastUsed;
        chosen = ROUTE_LAST_USED;
    } else if (defaultIndex >= 0 && defaultIndex < count) {
        targets[n++] = defaultIndex;
        chosen = ROUTE_DEFAULT;
    } else {
        targets[n++] = 0;
        chosen = ROUTE_FIRST;
    }
    if (reason) *reason = chosen;
    return n;
}

/* ---------------------------------------------------------------- shortcuts */

static const WCHAR *NextToken(const WCHAR *p, WCHAR *token, size_t cch)
{
    size_t length = 0;
    BOOL quoted = FALSE;
    while (*p && IsSpace(*p)) p++;
    if (!*p) return NULL;
    while (*p && (quoted || !IsSpace(*p))) {
        if (*p == L'"') { quoted = !quoted; p++; continue; }
        if (length + 1 < cch) token[length++] = *p;
        p++;
    }
    token[length] = 0;
    return p;
}

BOOL Core_ArgsSelectProfile(const WCHAR *args, const WCHAR *folder)
{
    WCHAR token[256];
    BOOL next = FALSE;
    const WCHAR *p = args;
    if (!args || !folder || !*folder) return FALSE;
    while ((p = NextToken(p, token, ARRAYSIZE(token))) != NULL) {
        if (next) return EqualsI(token, -1, folder, -1);
        next = EqualsI(token, -1, L"--launch", -1);
    }
    return FALSE;
}

BOOL Core_ArgsReferenceDir(const WCHAR *args, const WCHAR *dir)
{
    size_t dirLength;
    const WCHAR *p;
    WCHAR bare[MAX_PATH];
    if (!args || !dir || !*dir) return FALSE;
    dirLength = Core_TrimmedPathLength(dir);
    if (dirLength < 4 || FAILED(StringCchCopyNW(bare, ARRAYSIZE(bare), dir, dirLength))) return FALSE;
    for (p = FindI(args, bare); p; p = FindI(p + 1, bare)) {
        WCHAR before = p == args ? L' ' : p[-1];
        WCHAR after = p[dirLength];
        BOOL okBefore = before == L'"' || before == L'=' || IsSpace(before);
        BOOL okAfter = after == 0 || after == L'"' || IsSpace(after) ||
                       (after == L'\\' && (p[dirLength + 1] == 0 || p[dirLength + 1] == L'"' || IsSpace(p[dirLength + 1])));
        if (okBefore && okAfter) return TRUE;
    }
    return FALSE;
}

/* `text` (a package name, or a shell parsing name holding one) names a
 * package called Claude, whatever its publisher: "Claude_" starts it or
 * follows a backslash or a dot (AnthropicPBC.Claude_...). */
BOOL Core_NamesClaudePackage(const WCHAR *text)
{
    static const WCHAR family[] = L"Claude_";
    const WCHAR *p;
    if (!text) return FALSE;
    for (p = FindI(text, family); p; p = FindI(p + 1, family))
        if (p == text || p[-1] == L'\\' || p[-1] == L'.') return TRUE;
    return FALSE;
}

BOOL Core_LinkOpensProfile(const LinkInfo *link, const WCHAR *folder, const WCHAR *dataDir, BOOL isStock)
{
    if (!link) return FALSE;
    if (Core_IsOurExe(link->target)) return Core_ArgsSelectProfile(link->args, folder);
    if (Core_ArgsReferenceDir(link->args, dataDir)) return TRUE;
    if (isStock) {
        if (Core_EndsWithI(link->target, L"\\app\\Claude.exe") && !Core_ContainsI(link->args, L"--user-data-dir"))
            return TRUE;
        if (!link->target[0] && Core_EndsWithI(link->parsing, L"!Claude") && Core_NamesClaudePackage(link->parsing))
            return TRUE;
    }
    return FALSE;
}

/* `path` is `dir` itself or inside it (case-insensitive, whole folder names). */
BOOL Core_PathUnder(const WCHAR *path, const WCHAR *dir)
{
    size_t dirLength;
    if (!path || !dir) return FALSE;
    dirLength = Core_TrimmedPathLength(dir);
    if (dirLength == 0 || wcslen(path) < dirLength || !EqualsI(path, (int)dirLength, dir, (int)dirLength)) return FALSE;
    return dir[dirLength - 1] == L'\\' || dir[dirLength - 1] == L'/' || path[dirLength] == 0 || path[dirLength] == L'\\' ||
           path[dirLength] == L'/';
}

/* Where Claude's package keeps what it writes to AppData for itself:
 * %LOCALAPPDATA%\Packages\<family>\LocalCache\<area>\<name> (area: Local or Roaming). */
BOOL Core_PackageCachePath(const WCHAR *localAppData, const WCHAR *family, const WCHAR *area, const WCHAR *name,
                           WCHAR *out, size_t cch)
{
    if (!localAppData || !*localAppData || !family || !*family) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\Packages\\%s\\LocalCache\\%s\\%s", localAppData, family, area, name));
}

/* Stored working directories use the profile's logical path. File access
 * outside its package uses the same suffix below the resolved storage. */
BOOL Core_ProfileFilePath(const Profile *p, const WCHAR *path, WCHAR *out, size_t cch)
{
    WCHAR resolved[MAX_PATH];
    const WCHAR *result = path;
    size_t logical, physical;
    if (!out || !cch) return FALSE;
    if (!p || !path || !*path) {
        out[0] = 0;
        return FALSE;
    }
    if (Core_PathUnder(path, p->dataDir)) {
        if (!p->storageDir[0]) {
            out[0] = 0;
            return FALSE;
        }
        logical = Core_TrimmedPathLength(p->dataDir);
        physical = Core_TrimmedPathLength(p->storageDir);
        if (FAILED(StringCchPrintfW(resolved, ARRAYSIZE(resolved), L"%.*s%s",
                                    (int)physical, p->storageDir, path + logical))) {
            out[0] = 0;
            return FALSE;
        }
        result = resolved;
    }
    if (result == out) {
        if (wcslen(result) < cch) return TRUE;
    } else if (SUCCEEDED(StringCchCopyW(out, cch, result))) {
        return TRUE;
    }
    out[0] = 0;
    return FALSE;
}

/* The two times are the same to the shell (FAT date and time, 2-second steps). */
BOOL Core_SameFatTime(const FILETIME *a, const FILETIME *b)
{
    WORD dateA, timeA, dateB, timeB;
    if (!FileTimeToDosDateTime(a, &dateA, &timeA) || !FileTimeToDosDateTime(b, &dateB, &timeB)) return FALSE;
    return dateA == dateB && timeA == timeB;
}

void Core_ShortcutFileName(const WCHAR *label, int copyNumber, WCHAR *out, size_t cch)
{
    WCHAR clean[LABEL_CCH];
    size_t i;
    if (FAILED(StringCchCopyW(clean, ARRAYSIZE(clean), label ? label : L""))) clean[0] = 0;
    for (i = 0; clean[i]; i++) {
        if (IsControl(clean[i]) || wcschr(L"\\/:*?\"<>|", clean[i])) clean[i] = L'_';
    }
    if (copyNumber > 1)
        StringCchPrintfW(out, cch, L"Claude (%s) (%d).lnk", clean, copyNumber);
    else
        StringCchPrintfW(out, cch, L"Claude (%s).lnk", clean);
}

/* ------------------------------------------------------------------- JSON */

static BOOL IsJsonSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static size_t SkipSpace(const char *s, size_t len, size_t i)
{
    while (i < len && IsJsonSpace(s[i])) i++;
    return i;
}

/* The end of the JSON string that starts at s[i] (a quote), 0 when unterminated. */
static size_t StringEnd(const char *s, size_t len, size_t i)
{
    for (i++; i < len; i++) {
        if (s[i] == '\\') i++;
        else if (s[i] == '"') return i + 1;
    }
    return 0;
}

/* The end of the JSON value that starts at s[i], 0 when malformed. */
static size_t ValueEnd(const char *s, size_t len, size_t i)
{
    int depth = 0;
    if (i >= len) return 0;
    if (s[i] == '"') return StringEnd(s, len, i);
    if (s[i] != '{' && s[i] != '[') {
        while (i < len && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\r' && s[i] != '\n' && s[i] != '\t') i++;
        return i;
    }
    for (; i < len; i++) {
        if (s[i] == '"') {
            i = StringEnd(s, len, i);
            if (!i) return 0;
            i--;
        } else if (s[i] == '{' || s[i] == '[') {
            depth++;
        } else if (s[i] == '}' || s[i] == ']') {
            if (--depth == 0) return i + 1;
        }
    }
    return 0;
}

/* Where the JSON text starts, after a UTF-8 byte order mark. */
static size_t SkipByteOrderMark(const char *json, size_t len)
{
    return len >= 3 && (unsigned char)json[0] == 0xEF && (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF ? 3 : 0;
}

/* The raw text of member `key` of the JSON object `json` (not of a nested
 * one), as it is written: quotes, braces and all. */
BOOL Core_JsonMember(const char *json, size_t len, const char *key, const char **value, size_t *valueLen)
{
    size_t i, keyStart, keyEnd, end, keyLen = strlen(key);
    i = SkipSpace(json, len, SkipByteOrderMark(json, len));
    if (i >= len || json[i] != '{') return FALSE;
    i = SkipSpace(json, len, i + 1);
    while (i < len && json[i] == '"') {
        keyStart = i + 1;
        keyEnd = StringEnd(json, len, i);
        if (!keyEnd) return FALSE;
        i = SkipSpace(json, len, keyEnd);
        if (i >= len || json[i] != ':') return FALSE;
        i = SkipSpace(json, len, i + 1);
        end = ValueEnd(json, len, i);
        if (!end || end == i) return FALSE;
        if (keyEnd - 1 - keyStart == keyLen && memcmp(json + keyStart, key, keyLen) == 0) {
            *value = json + i;
            *valueLen = end - i;
            return TRUE;
        }
        i = SkipSpace(json, len, end);
        if (i >= len || json[i] != ',') return FALSE;
        i = SkipSpace(json, len, i + 1);
    }
    return FALSE;
}

/* Appends `codePoint` in UTF-8 at out[*length], leaving room for a
 * terminating zero in the `cap` bytes: FALSE, nothing appended, when it does
 * not fit. */
static BOOL PutUtf8(char *out, size_t cap, size_t *length, DWORD codePoint)
{
    unsigned char bytes[4];
    size_t count, i;
    if (codePoint < 0x80) {
        bytes[0] = (unsigned char)codePoint;
        count = 1;
    } else if (codePoint < 0x800) {
        bytes[0] = (unsigned char)(0xC0 | (codePoint >> 6));
        bytes[1] = (unsigned char)(0x80 | (codePoint & 0x3F));
        count = 2;
    } else if (codePoint < 0x10000) {
        bytes[0] = (unsigned char)(0xE0 | (codePoint >> 12));
        bytes[1] = (unsigned char)(0x80 | ((codePoint >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (codePoint & 0x3F));
        count = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0 | (codePoint >> 18));
        bytes[1] = (unsigned char)(0x80 | ((codePoint >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((codePoint >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (codePoint & 0x3F));
        count = 4;
    }
    if (*length + count >= cap) return FALSE;
    for (i = 0; i < count; i++) out[(*length)++] = (char)bytes[i];
    return TRUE;
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static BOOL Hex4(const char *p, DWORD *value)
{
    int i, digit;
    *value = 0;
    for (i = 0; i < 4; i++) {
        if ((digit = HexDigit(p[i])) < 0) return FALSE;
        *value = (*value << 4) | (DWORD)digit;
    }
    return TRUE;
}

static BOOL JsonUtf16(const char *utf8, size_t length, WCHAR *out, size_t cch)
{
    int n;
    if (length == 0) return TRUE;
    if (cch <= 1 || length > INT_MAX || cch - 1 > INT_MAX) return FALSE;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)length, out, (int)(cch - 1));
    if (n <= 0) { out[0] = 0; return FALSE; }
    out[n] = 0;
    return TRUE;
}

/* A JSON string as Core_JsonMember gives it (quotes included), decoded to
 * UTF-16. FALSE when it is not a string, is malformed or does not fit. */
BOOL Core_JsonString(const char *raw, size_t len, WCHAR *out, size_t cch)
{
    char *utf8;
    size_t i, length = 0;
    BOOL ok;
    if (!raw || !out || cch == 0 || len < 2 || raw[0] != '"' || raw[len - 1] != '"') return FALSE;
    out[0] = 0;
    for (i = 1; i + 1 < len; i++) {
        if ((unsigned char)raw[i] < 0x20 || raw[i] == '"') return FALSE;
        if (raw[i] == '\\') break;
    }
    if (i + 1 == len) return JsonUtf16(raw + 1, len - 2, out, cch);
    utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, len);
    if (!utf8) return FALSE;
    for (i = 1; i + 1 < len; i++) {
        char c = raw[i];
        DWORD cp, low;
        if (c != '\\') {
            if ((unsigned char)c < 0x20 || c == '"') goto bad;
            utf8[length++] = c;
            continue;
        }
        if (++i + 1 >= len) goto bad;
        switch (raw[i]) {
        case '"': case '\\': case '/': utf8[length++] = raw[i]; break;
        case 'b': utf8[length++] = '\b'; break;
        case 'f': utf8[length++] = '\f'; break;
        case 'n': utf8[length++] = '\n'; break;
        case 'r': utf8[length++] = '\r'; break;
        case 't': utf8[length++] = '\t'; break;
        case 'u':
            if (i + 5 > len - 1 || !Hex4(raw + i + 1, &cp)) goto bad;
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 7 <= len - 1 && raw[i + 1] == '\\' && raw[i + 2] == 'u' &&
                Hex4(raw + i + 3, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 6;
            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                cp = 0xFFFD;   /* a lone surrogate */
            }
            if (!PutUtf8(utf8, len, &length, cp)) goto bad;
            break;
        default:
            goto bad;
        }
    }
    ok = JsonUtf16(utf8, length, out, cch);
    HeapFree(GetProcessHeap(), 0, utf8);
    return ok;
bad:
    HeapFree(GetProcessHeap(), 0, utf8);
    out[0] = 0;
    return FALSE;
}

/* A JSON number without fraction or sign (a count, a time in ms). */
BOOL Core_JsonNumber(const char *raw, size_t len, ULONGLONG *value)
{
    size_t i;
    *value = 0;
    if (!raw || len == 0) return FALSE;
    for (i = 0; i < len; i++) {
        if (!IsDigit(raw[i]) || *value > (~0ULL - (ULONGLONG)(raw[i] - '0')) / 10) return FALSE;
        *value = *value * 10 + (ULONGLONG)(raw[i] - '0');
    }
    return TRUE;
}

BOOL Core_JsonTrue(const char *raw, size_t len)
{
    return raw && len == 4 && memcmp(raw, "true", 4) == 0;
}

/* The member is there, and neither null nor false. */
static BOOL JsonIsSet(const char *json, size_t len, const char *key)
{
    const char *value;
    size_t valueLength;
    return Core_JsonMember(json, len, key, &value, &valueLength) && !(valueLength == 4 && memcmp(value, "null", 4) == 0) &&
           !(valueLength == 5 && memcmp(value, "false", 5) == 0);
}

/* What a Code session entry (claude-code-sessions\...\local_*.json) is: a
 * session of this PC, one run over SSH, in WSL or in the cloud (its
 * conversation is elsewhere), or not an entry (no sessionId). Claude only
 * checks that these members are set: movedToCloud is true in some versions,
 * an object in others. */
SessionEntryKind Core_SessionEntryKind(const char *json, size_t len)
{
    const char *value;
    size_t valueLength;
    if (!json || !Core_JsonMember(json, len, "sessionId", &value, &valueLength) || valueLength < 3 || value[0] != '"')
        return ENTRY_NOT_SESSION;
    if (JsonIsSet(json, len, "sshConfig") || JsonIsSet(json, len, "wslConfig") || JsonIsSet(json, len, "cloudSessionId") ||
        JsonIsSet(json, len, "movedToCloud"))
        return ENTRY_ELSEWHERE;
    return ENTRY_LOCAL;
}

/* ---------------------------------------------------------- session edits */

/* `json` with its top-level member `key` set to `raw` (a JSON value as
 * text): replaced where it is, else added last in the object, the rest kept
 * byte for byte. FALSE when `json` is not an object or `out` is too small. */
BOOL Core_JsonSetMember(const char *json, size_t len, const char *key, const char *raw, char *out, size_t cap, size_t *outLen)
{
    const char *member;
    size_t memberLength, written, rawLen = strlen(raw), keyLen = strlen(key), head, tail, last, need, start;
    BOOL empty;
    *outLen = 0;
    start = SkipSpace(json, len, SkipByteOrderMark(json, len));
    if (start >= len || json[start] != '{') return FALSE;
    if (Core_JsonMember(json, len, key, &member, &memberLength)) {
        head = (size_t)(member - json);
        tail = len - head - memberLength;
        if (head + rawLen + tail > cap) return FALSE;
        memcpy(out, json, head);
        memcpy(out + head, raw, rawLen);
        memcpy(out + head + rawLen, member + memberLength, tail);
        *outLen = head + rawLen + tail;
        return TRUE;
    }
    /* Not there: added after the last value. */
    for (last = len; last > 0 && IsJsonSpace(json[last - 1]); last--) {}
    if (last == 0 || json[last - 1] != '}') return FALSE;
    for (head = last - 1; head > start && IsJsonSpace(json[head - 1]); head--) {}
    empty = head == start + 1;
    need = head + (empty ? 0 : 1) + 1 + keyLen + 2 + rawLen + (len - head);
    if (need > cap) return FALSE;
    memcpy(out, json, head);
    written = head;
    if (!empty) out[written++] = ',';
    out[written++] = '"';
    memcpy(out + written, key, keyLen);
    written += keyLen;
    out[written++] = '"';
    out[written++] = ':';
    memcpy(out + written, raw, rawLen);
    written += rawLen;
    memcpy(out + written, json + head, len - head);
    *outLen = written + (len - head);
    return TRUE;
}

/* `text` as a JSON string, quotes included, in UTF-8. */
BOOL Core_JsonQuote(const WCHAR *text, char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    const WCHAR *p;
#define PUT(c) do { if (!PutUtf8(out, cap, &n, (DWORD)(c))) return FALSE; } while (0)
    PUT('"');
    for (p = text; *p; p++) {
        DWORD c = *p;
        if (c == '"' || c == '\\') {
            PUT('\\');
            PUT(c);
        } else if (c < 0x20) {
            PUT('\\'); PUT('u'); PUT('0'); PUT('0'); PUT(hex[c >> 4]); PUT(hex[c & 15]);
        } else if (c >= 0xD800 && c <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF) {
            PUT(0x10000 + ((c - 0xD800) << 10) + (p[1] - 0xDC00));
            p++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            PUT(0xFFFD);   /* a lone surrogate */
        } else {
            PUT(c);
        }
    }
    PUT('"');
#undef PUT
    out[n] = 0;
    return TRUE;
}

/* The folder Claude Code keeps a working folder's transcripts in, under
 * ~\.claude\projects: every character but an ASCII letter or digit becomes
 * '-'. Past CORE_PROJECT_NAME_MAX characters Claude Code adds a hash of its
 * own: FALSE then. */
BOOL Core_ProjectDirName(const WCHAR *cwd, WCHAR *out, size_t cch)
{
    size_t i, len = wcslen(cwd);
    if (len == 0 || len > CORE_PROJECT_NAME_MAX || len >= cch) return FALSE;
    for (i = 0; i < len; i++) {
        WCHAR c = cwd[i];
        out[i] = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ? c : L'-';
    }
    out[len] = 0;
    return TRUE;
}

/* A UUID as Claude Code writes session ids: 8-4-4-4-12 hex digits. */
BOOL Core_IsUuid(const WCHAR *id)
{
    int i;
    if (!id || wcslen(id) != 36) return FALSE;
    for (i = 0; i < 36; i++) {
        WCHAR c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != L'-') return FALSE;
        } else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

/* The link Claude Desktop opens a Claude Code session with (adding it to
 * the profile's list when it is not there yet). */
BOOL Core_ResumeLink(const WCHAR *sessionId, WCHAR *out, size_t cch)
{
    return Core_IsUuid(sessionId) && SUCCEEDED(StringCchPrintfW(out, cch, L"claude://resume?session=%s", sessionId));
}

/* A new "no folder" working folder's name, as Claude Desktop names them:
 * scratch-<date>-<6 hex digits>. */
void Core_ScratchName(const SYSTEMTIME *day, DWORD random, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, L"scratch-%04u-%02u-%02u-%06lx", day->wYear, day->wMonth, day->wDay, (unsigned long)(random & 0xFFFFFF));
}

/* The "no folder" area of an entries folder (...\claude-code-sessions\
 * <account>\<organization>): <dataDir>\scratch-workspaces\<account>\<organization>. */
BOOL Core_ScratchDirFor(const WCHAR *dataDir, const WCHAR *entriesDir, WCHAR *out, size_t cch)
{
    const WCHAR *organization = wcsrchr(entriesDir, L'\\'), *account;
    if (!organization || organization == entriesDir || !organization[1]) return FALSE;
    for (account = organization - 1; account > entriesDir && *account != L'\\'; account--) {}
    if (*account != L'\\' || account + 1 == organization) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\scratch-workspaces%s", dataDir, account));
}

/* Copies `in` to `out` with every `swaps[i].from` replaced by its `to`, the
 * first swap that matches at a place winning, for a file read in chunks: when
 * a swap could start in the last bytes and this is not the `last` chunk, they
 * are left for the next one (`*used` says how much of `in` was taken). A swap
 * held there keeps the later ones from matching at that place, so where the
 * file is cut never changes the result. Returns the bytes written; `out` must
 * hold at least len + (len / shortest from + 1) * longest to bytes. */
size_t Core_ReplaceChunk(const char *in, size_t len, const CoreSwap *swaps, int count, BOOL last, char *out, size_t *used)
{
    size_t i = 0, n = 0;
    int swap;
    while (i < len) {
        BOOL held = FALSE, swapped = FALSE;
        for (swap = 0; swap < count && !swapped && !held; swap++) {
            const char *from = swaps[swap].from;
            size_t matched = 0;
            /* Compared as far as it matches, so most bytes cost one comparison. */
            while (from[matched] && i + matched < len && in[i + matched] == from[matched]) matched++;
            if (matched == 0) continue;
            if (!from[matched]) {
                size_t toLength = strlen(swaps[swap].to);
                memcpy(out + n, swaps[swap].to, toLength);
                n += toLength;
                i += matched;
                swapped = TRUE;
            } else if (i + matched == len && !last) {
                held = TRUE;
            }
        }
        if (swapped) continue;
        if (held) break;
        out[n++] = in[i++];
    }
    *used = i;
    return n;
}

/* A change to a session entry waiting for its profile to close, one per
 * line: "<op>\t<session id>\t<value>". */
static const WCHAR *const kPendingOps[] = { L"title", L"star", L"remove" };
C_ASSERT(ARRAYSIZE(kPendingOps) == PENDING_REMOVE + 1);

BOOL Core_PendingFormat(const PendingEdit *edit, WCHAR *out, size_t cch)
{
    WCHAR value[SESSION_TITLE_CCH];
    size_t i;
    if ((int)edit->op < 0 || (int)edit->op >= (int)ARRAYSIZE(kPendingOps) || !edit->key[0] || wcspbrk(edit->key, L"\t\r\n"))
        return FALSE;
    StringCchCopyW(value, ARRAYSIZE(value), edit->value);
    for (i = 0; value[i]; i++)
        if (value[i] == L'\t' || value[i] == L'\r' || value[i] == L'\n') value[i] = L' ';
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\t%s\t%s", kPendingOps[edit->op], edit->key, value));
}

BOOL Core_PendingParse(const WCHAR *line, PendingEdit *edit)
{
    const WCHAR *tab1 = wcschr(line, L'\t'), *tab2;
    size_t op;
    ZeroMemory(edit, sizeof *edit);
    if (!tab1 || (tab2 = wcschr(tab1 + 1, L'\t')) == NULL || tab2 == tab1 + 1) return FALSE;
    for (op = 0; op < ARRAYSIZE(kPendingOps); op++)
        if ((size_t)(tab1 - line) == wcslen(kPendingOps[op]) && wcsncmp(line, kPendingOps[op], (size_t)(tab1 - line)) == 0) break;
    if (op == ARRAYSIZE(kPendingOps)) return FALSE;
    edit->op = (PendingOp)op;
    if (FAILED(StringCchCopyNW(edit->key, ARRAYSIZE(edit->key), tab1 + 1, (size_t)(tab2 - tab1 - 1)))) return FALSE;
    StringCchCopyW(edit->value, ARRAYSIZE(edit->value), tab2 + 1);
    return TRUE;
}

/* A change queued after `queued` replaces it: one of the same kind to the
 * same session, and a removal replaces them all (and is replaced by any). */
BOOL Core_PendingReplaces(const PendingEdit *queued, const PendingEdit *added)
{
    return EqualsI(queued->key, -1, added->key, -1) &&
           (queued->op == added->op || queued->op == PENDING_REMOVE || added->op == PENDING_REMOVE);
}

/* ------------------------------------------------------------ drawing math */

/* One frame of a smooth wheel scroll: how many px of the `pending` ones it
 * covers, `elapsedMs` after the last frame. What is left shrinks by a factor
 * of e every CORE_SCROLL_EASE_MS, whatever the frame rate (a late frame covers
 * more, so the pace holds), and by at least a pixel, so it ends; never past it. */
int Core_ScrollStep(int pending, int elapsedMs)
{
    double part;
    int px;
    if (pending == 0) return 0;
    part = pending * (1.0 - exp(-(double)max(elapsedMs, 1) / CORE_SCROLL_EASE_MS));
    px = (int)(part + (part >= 0 ? 0.5 : -0.5));
    if (px == 0) px = pending > 0 ? 1 : -1;
    return px;
}

/* FNV-1a, 64 bits: `hash` goes on with `size` more bytes (start from CORE_HASH_START). */
ULONGLONG Core_HashBytes(ULONGLONG hash, const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Core_HashBytes going on with `text` and its terminator, so texts hashed one
 * after another never run together. Case counts. */
ULONGLONG Core_HashText(ULONGLONG hash, const WCHAR *text)
{
    return Core_HashBytes(hash, text, (wcslen(text) + 1) * sizeof(WCHAR));
}

/* A `width` x `height` rectangle centered on `on`, then moved inside `work`
 * (its top left corner inside when it is bigger). */
void Core_CenterRect(const RECT *on, int width, int height, const RECT *work, RECT *out)
{
    int x = on->left + ((on->right - on->left) - width) / 2, y = on->top + ((on->bottom - on->top) - height) / 2;
    if (x + width > work->right) x = work->right - width;
    if (y + height > work->bottom) y = work->bottom - height;
    if (x < work->left) x = work->left;
    if (y < work->top) y = work->top;
    out->left = x;
    out->top = y;
    out->right = x + width;
    out->bottom = y + height;
}

/* ---------------------------------------------------------------- versions */

/* "v1.2.3" or "1.2" as up to four numbers; FALSE when it does not start with
 * one or a number is above 65535. */
BOOL Core_ParseVersion(const WCHAR *text, DWORD parts[4])
{
    int n = 0;
    ZeroMemory(parts, 4 * sizeof(DWORD));
    if (!text) return FALSE;
    if (*text == L'v' || *text == L'V') text++;
    while (n < 4 && *text >= L'0' && *text <= L'9') {
        DWORD number = 0;
        while (*text >= L'0' && *text <= L'9') {
            number = number * 10 + (DWORD)(*text - L'0');
            if (number > 65535) return FALSE;
            text++;
        }
        parts[n++] = number;
        if (*text != L'.') break;
        text++;
    }
    return n > 0;
}

/* -1, 0 or 1 as version a is older than, the same as or newer than b. */
int Core_CompareVersions(const DWORD a[4], const DWORD b[4])
{
    int i;
    for (i = 0; i < 4; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
